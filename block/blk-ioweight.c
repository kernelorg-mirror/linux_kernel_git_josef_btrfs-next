/*
 * Copyright (C) 2018 Josef Bacik
 */
#include <linux/kernel.h>
#include <linux/blk_types.h>
#include <linux/backing-dev.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/memcontrol.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/signal.h>
#include <trace/events/block.h>
#include "blk-rq-qos.h"
#include "blk-stat.h"

static struct blkcg_policy blkcg_policy_ioweight;
struct ioweight_grp;

struct blk_ioweight {
	struct rq_qos rqos;
	u64 saturation;
	atomic_t enabled;
	struct timer_list timer;
};

static inline struct blk_ioweight *BLKIOWEIGHT(struct rq_qos *rqos)
{
	return container_of(rqos, struct blk_ioweight, rqos);
}

static inline bool blk_ioweight_enabled(struct blk_ioweight *blkioweight)
{
	return atomic_read(&blkioweight->enabled) > 0;
}

#define TIME_SLOTS 5

enum ioweight_bucket {
	BUCKET_READS = 0,
	BUCKET_WRITES,
	BUCKET_TRIMS,
	NR_BUCKETS,
};

struct ioweight_stat {
	u64 nr_ios;
	u64 bytes;
	u64 start_ns;
	u64 end_ns;
};

struct ioweight_grp {
	struct blkg_policy_data pd;
	struct blk_ioweight *blkioweight;
	struct rq_depth rq_depth;
	struct rq_wait rq_wait;
	struct ioweight_grp *next;

	u64 weight;
	atomic64_t child_weight;

	struct ioweight_stat __percpu *stat;
	struct ioweight_stat sum[NR_BUCKETS];
	u64 children_total_load;
	u64 load;
	u64 io_time;
};

struct coefficient {
	u64 numerator;
	u64 denominator;
};

static inline struct ioweight_grp *pd_to_ioweight(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct ioweight_grp, pd) : NULL;
}

static inline struct ioweight_grp *blkg_to_ioweight(struct blkcg_gq *blkg)
{
	return pd_to_ioweight(blkg_to_pd(blkg, &blkcg_policy_ioweight));
}

static inline struct blkcg_gq *ioweight_to_blkg(struct ioweight_grp *ioweight)
{
	return pd_to_blkg(&ioweight->pd);
}

static inline void ioweight_stat_init(struct ioweight_stat *stat)
{
	stat->nr_ios = 0;
	stat->bytes = 0;
	stat->start_ns = (u64)-1;
	stat->end_ns = 0;
}

static inline void ioweight_stat_sum(struct ioweight_stat *dst,
				     struct ioweight_stat *src)
{
	dst->nr_ios += src->nr_ios;
	dst->bytes += src->bytes;
	dst->start_ns = min(dst->start_ns, src->start_ns);
	dst->end_ns = max(dst->end_ns, src->end_ns);
}

static inline u64 ioweight_stat_bw(struct ioweight_stat *stat)
{
	u64 time = stat->end_ns - stat->start_ns;
	u64 size = stat->bytes;
	u64 bw;
	u64 multiplier = NSEC_PER_SEC;

	if (!time || !size)
		return 0;

	while (time && size < time) {
		time = div64_u64(time, 100);
		multiplier = div64_u64(multiplier, 100);
	}

	time = max_t(u64, time, 1);

	if (multiplier)
		bw *= multiplier;
	bw = div64_u64(size, time);
	return bw;
}

static inline void ioweight_grp_sum_stats(struct ioweight_grp *ioweight)
{
	u64 total_time = 0;
	u64 total_ios = 0;
	int bucket, cpu;

	for (bucket = 0; bucket < NR_BUCKETS; bucket++)
		ioweight_stat_init(&ioweight->sum[bucket]);

	for_each_online_cpu(cpu) {
		struct ioweight_stat *stat;

		stat = per_cpu_ptr(ioweight->stat, cpu);
		for (bucket = 0; bucket < NR_BUCKETS; bucket++) {
			ioweight_stat_sum(&ioweight->sum[bucket],
					  &stat[bucket]);
			total_ios += ioweight->sum[bucket].nr_ios;
			total_time += (ioweight->sum[bucket].end_ns -
				       ioweight->sum[bucket].start_ns);
			ioweight_stat_init(&stat[bucket]);
		}
	}
	total_time = max_t(u64, total_time, 1);
	total_ios = max_t(u64, total_ios, 1);
	ioweight->io_time = max(NSEC_PER_USEC,
				div64_u64(total_time, total_ios));
}

enum {
	READ_IOPS_COEF = 0,
	READ_BW_COEF,
	WRITE_IOPS_COEF,
	WRITE_BW_COEF,
	TRIM_IOPS_COEF,
	TRIM_BW_COEF,
	TOTAL_COEFF,
};

static struct coefficient ssd_coefficients[] = {
	[READ_IOPS_COEF]	= { .numerator = 121, .denominator = 10000 }, // 1.21e-2
	[READ_BW_COEF]		= { .numerator = 625, .denominator = 1000000000ULL}, // 6.25e-7
	[WRITE_IOPS_COEF]	= { .numerator = 107, .denominator = 100000ULL}, // 1.07e-3
	[WRITE_BW_COEF]		= { .numerator = 261, .denominator = 1000000000ULL}, // 2.61e-7
	[TRIM_IOPS_COEF]	= { .numerator = 237, .denominator = 10000 }, // 2.37e-2
	[TRIM_BW_COEF]		= { .numerator = 91, .denominator = 100000000000ULL}, // 9.10e-10
};

static struct coefficient hdd_coefficients[] = {
	[READ_IOPS_COEF]	= { .numerator = 131, .denominator = 100000 }, // 1.31e-3
	[READ_BW_COEF]		= { .numerator = 131, .denominator = 1000000000ULL}, // 1.31e-7
	[WRITE_IOPS_COEF]	= { .numerator = 257, .denominator = 1000ULL}, // .257
	[WRITE_BW_COEF]		= { .numerator = 503, .denominator = 1000000000ULL}, // 5.03e-9
};

static inline u64 multiply_stat_by_coef(u64 val, struct coefficient *coef)
{
	if (val == 0)
		return 0;
	val *= coef->numerator;
	return div64_u64(val, coef->denominator);
}

static inline void ioweight_grp_load(struct ioweight_grp *ioweight)
{
	struct blkcg_gq *blkg = ioweight_to_blkg(ioweight);
	struct coefficient *coef = ssd_coefficients;
	u64 sum = 0;

	sum += multiply_stat_by_coef(ioweight->sum[BUCKET_READS].nr_ios,
				     &coef[READ_IOPS_COEF]);
	sum += multiply_stat_by_coef(ioweight_stat_bw(&ioweight->sum[BUCKET_READS]),
				     &coef[READ_BW_COEF]);
	sum += multiply_stat_by_coef(ioweight->sum[BUCKET_WRITES].nr_ios,
				     &coef[WRITE_IOPS_COEF]);
	sum += multiply_stat_by_coef(ioweight_stat_bw(&ioweight->sum[BUCKET_WRITES]),
				     &coef[WRITE_BW_COEF]);
	sum += multiply_stat_by_coef(ioweight->sum[BUCKET_TRIMS].nr_ios,
				     &coef[TRIM_IOPS_COEF]);
	sum += multiply_stat_by_coef(ioweight_stat_bw(&ioweight->sum[BUCKET_TRIMS]),
				     &coef[TRIM_BW_COEF]);
	ioweight->load = sum;
	if (blkg->parent) {
		struct ioweight_grp *parent = blkg_to_ioweight(blkg->parent);
		if (parent)
			parent->children_total_load += sum;
	}
}

static void ioweight_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	if (atomic_dec_return(&rqw->inflight) < 0)
		printk(KERN_ERR "we have fucked up\n");
	wake_up(&rqw->wait);
}

static bool ioweight_acquire_inflight(struct rq_wait *rqw, void *private_data)
{
	struct ioweight_grp *ioweight = private_data;
	return rq_wait_inc_below(rqw, READ_ONCE(ioweight->rq_depth.max_depth));
}

static void __blkcg_ioweight_throttle(struct rq_qos *rqos,
				       struct ioweight_grp *ioweight,
				       bool issue_as_root,
				       bool use_memdelay)
{
	struct rq_wait *rqw = &ioweight->rq_wait;

	if (atomic_read(&ioweight_to_blkg(ioweight)->use_delay))
		blkcg_schedule_throttle(rqos->q, use_memdelay);

	/*
	 * To avoid priority inversions we want to just take a slot if we are
	 * issuing as root.  If we're being killed off there's no point in
	 * delaying things, we may have been killed by OOM so throttling may
	 * make recovery take even longer, so just let the IO's through so the
	 * task can go away.
	 */
	if (issue_as_root || fatal_signal_pending(current)) {
		atomic_inc(&rqw->inflight);
		return;
	}

	rq_qos_wait(rqw, ioweight, ioweight_acquire_inflight, ioweight_cleanup_cb);
}

static void blkcg_ioweight_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct blk_ioweight *blkioweight = BLKIOWEIGHT(rqos);
	struct blkcg_gq *blkg = bio->bi_blkg;
	bool issue_as_root = bio_issue_as_root_blkg(bio);

	if (!blk_ioweight_enabled(blkioweight))
		return;

	while (blkg && blkg->parent) {
		struct ioweight_grp *ioweight = blkg_to_ioweight(blkg);
		if (!ioweight) {
			blkg = blkg->parent;
			continue;
		}

		__blkcg_ioweight_throttle(rqos, ioweight, issue_as_root,
				     (bio->bi_opf & REQ_SWAP) == REQ_SWAP);
		blkg = blkg->parent;
	}
	if (!timer_pending(&blkioweight->timer))
		mod_timer(&blkioweight->timer, jiffies + HZ);
}

static inline enum ioweight_bucket rq_to_bucket(struct request *rq)
{
	const int op = req_op(rq);

	if (op == REQ_OP_DISCARD)
		return BUCKET_TRIMS;
	if (op == REQ_OP_READ)
		return BUCKET_READS;
	if (op_is_write(op))
		return BUCKET_WRITES;
	return NR_BUCKETS;
}

void blk_ioweight_stat_add(struct request *rq, u64 now)
{
	struct blkcg_gq *blkg;
	enum ioweight_bucket bucket = rq_to_bucket(rq);
	u64 start;

	if (bucket == NR_BUCKETS)
		return;

	start = rq->io_start_time_ns;
	if (start > now)
		start = now;

	for (blkg = rq->blkg; blkg; blkg = blkg->parent) {
		struct ioweight_grp *ioweight;
		struct ioweight_stat *stat;

		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight)
			continue;
		if (ioweight->weight == 0)
			continue;
		stat = &get_cpu_ptr(ioweight->stat)[bucket];
		stat->nr_ios++;
		stat->bytes += rq->io_bytes;
		stat->start_ns = min(stat->start_ns, start);
		stat->end_ns = max(stat->end_ns, now);
		put_cpu_ptr(ioweight->stat);
	}
}

static void blkcg_ioweight_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw;
	struct ioweight_grp *ioweight;
	bool enabled = false;

	blkg = bio->bi_blkg;
	if (!blkg || !bio_flagged(bio, BIO_TRACKED))
		return;

	ioweight = blkg_to_ioweight(bio->bi_blkg);
	if (!ioweight)
		return;

	if (!blk_ioweight_enabled(ioweight->blkioweight))
		return;

	while (blkg && blkg->parent) {
		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight) {
			blkg = blkg->parent;
			continue;
		}
		rqw = &ioweight->rq_wait;

		BUG_ON(atomic_dec_return(&rqw->inflight) < 0);
		wake_up(&rqw->wait);
		blkg = blkg->parent;
	}
}

static void blkcg_ioweight_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;

	blkg = bio->bi_blkg;
	while (blkg && blkg->parent) {
		struct rq_wait *rqw;
		struct ioweight_grp *ioweight;

		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight)
			goto next;

		rqw = &ioweight->rq_wait;
		BUG_ON(atomic_dec_return(&rqw->inflight) < 0);
		wake_up(&rqw->wait);
next:
		blkg = blkg->parent;
	}
}

static void blkcg_ioweight_exit(struct rq_qos *rqos)
{
	struct blk_ioweight *blkioweight = BLKIOWEIGHT(rqos);

	del_timer_sync(&blkioweight->timer);
	blkcg_deactivate_policy(rqos->q, &blkcg_policy_ioweight);
	kfree(blkioweight);
}

static void blkcg_ioweight_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	bio_issue_init(&bio->bi_issue, bio_sectors(bio));
}

static int ioweight_inflight_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct blk_ioweight *blkioweight = BLKIOWEIGHT(rqos);
	struct cgroup_subsys_state *pos_css;
	struct blkcg_gq *blkg;
	struct ioweight_grp *ioweight;

	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css,
				     blkioweight->rqos.q->root_blkg) {
		if (!blkg_tryget(blkg))
			continue;
		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight) {
			blkg_put(blkg);
			continue;
		}
		seq_printf(m, "%llu: inflight %d\n",
			   ioweight->weight,
			   atomic_read(&ioweight->rq_wait.inflight));
		blkg_put(blkg);
	}
	rcu_read_unlock();
	return 0;
}

static const struct blk_mq_debugfs_attr ioweight_debugfs_attrs[] = {
	{"inflight", 0400, ioweight_inflight_show},
	{},
};

static struct rq_qos_ops blkcg_ioweight_ops = {
	.throttle = blkcg_ioweight_throttle,
	.track = blkcg_ioweight_track,
	.cleanup = blkcg_ioweight_cleanup,
	.done_bio = blkcg_ioweight_done_bio,
	.exit = blkcg_ioweight_exit,
	.debugfs_attrs = ioweight_debugfs_attrs,
};

#define SCALE_DOWN_FACTOR 2
#define SCALE_UP_FACTOR 4

static inline unsigned long scale_amount(unsigned long qd, bool up)
{
	return max(up ? qd >> SCALE_UP_FACTOR : qd >> SCALE_DOWN_FACTOR, 1UL);
}

static void scale_up(struct ioweight_grp *ioweight)
{
	unsigned long qd = ioweight->blkioweight->rqos.q->nr_requests;
	unsigned long old = ioweight->rq_depth.max_depth;
	unsigned long scale = scale_amount(qd, true);

	if (old > qd)
		old = qd;

	if (old == 1 && blkcg_unuse_delay(ioweight_to_blkg(ioweight)))
		return;

	if (old < qd) {
		old = min(old + scale, qd);
		WRITE_ONCE(ioweight->rq_depth.max_depth, old);
		wake_up_all(&ioweight->rq_wait.wait);
	}

	trace_printk("weight %llu scale up %lu\n", ioweight->weight, old);
}

static void scale_down(struct ioweight_grp *ioweight)
{
	unsigned long qd = ioweight->blkioweight->rqos.q->nr_requests;
	unsigned long old = ioweight->rq_depth.max_depth;
	unsigned long scale = scale_amount(qd, true);

	if (old > qd)
		old = qd;

	if (old == 1) {
		trace_printk("weight %llu add delay\n", ioweight->weight);
		blkcg_add_delay(ioweight_to_blkg(ioweight),
				ktime_to_ns(ktime_get()),
				250 * NSEC_PER_MSEC);
		blkcg_use_delay(ioweight_to_blkg(ioweight));
		return;
	}

	if (scale > old)
		old = 1;
	else
		old -= scale;
	trace_printk("weight %llu scale down %lu\n", ioweight->weight, old);
	WRITE_ONCE(ioweight->rq_depth.max_depth, old);
}

/*
 * We want to have the saturation time rounded down to the nearest msec, but we
 * live in a binary world, and NSEC_PER_MSEC is not a power of 2.  We don't
 * want to ilog2 every time we do this, and ilog2(NSEC_PER_MSEC) is 19, which
 * actually comes out to .5msec.  So instead use 20 which is 1.5msec, because
 * the fuzzier the better.
 */
#define POW2_NSEC_PER_MSEC (1 << 20)

static void blkioweight_timer_fn(struct timer_list *t)
{
	struct blk_ioweight *blkioweight = from_timer(blkioweight, t, timer);
	struct blkcg_gq *blkg;
	struct cgroup_subsys_state *pos_css;
	struct ioweight_grp *head = NULL, *next;

	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css,
				     blkioweight->rqos.q->root_blkg) {
		struct ioweight_grp *ioweight;

		/*
		 * We could be exiting, don't access the pd unless we have a
		 * ref on the blkg.
		 */
		if (!blkg_tryget(blkg))
			continue;

		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight) {
			blkg_put(blkg);
			continue;
		}

		ioweight->children_total_load = 0;
		if (ioweight->weight == 0) {
			blkg_put(blkg);
			continue;
		}

		ioweight_grp_sum_stats(ioweight);
		ioweight_grp_load(ioweight);
		ioweight->next = head;
		head = ioweight;
	}
	rcu_read_unlock();

	for (next = head->next; head;
	     head = next, next = next ? next->next : NULL) {
		struct ioweight_grp *parent;
		u64 load, weight;
		u64 actual_share, weight_share;
		u64 child_total_load, child_weight;

		blkg = ioweight_to_blkg(head);
		if (!blkg->parent) {
			blkg_put(blkg);
			continue;
		}

		parent = blkg_to_ioweight(blkg->parent);
		if (!parent) {
			blkg_put(blkg);
			continue;
		}

		child_total_load = max_t(u64, 1, parent->children_total_load);
		child_weight = max_t(u64, 1, atomic64_read(&parent->child_weight));
		load = max_t(u64, head->load, 1);
		weight = max_t(u64, head->weight, 1);

		actual_share = div64_u64(load * 100, child_total_load);
		weight_share = div64_u64(weight * 100, child_weight);
		trace_printk("weight %llu load %llu parent total load %llu, actual_share %llu, weight_share %llu\n",
			     head->weight, head->load, parent->children_total_load,
			     actual_share, weight_share);

		if (actual_share > weight_share)
			scale_down(head);
		if (actual_share < weight_share)
			scale_up(head);
		blkg_put(blkg);
	}
}

int blk_ioweight_init(struct request_queue *q)
{
	struct blk_ioweight *blkioweight;
	struct rq_qos *rqos;
	int ret;

	blkioweight = kzalloc(sizeof(*blkioweight), GFP_KERNEL);
	if (!blkioweight)
		return -ENOMEM;

	rqos = &blkioweight->rqos;
	rqos->id = RQ_QOS_IOWEIGHT;
	rqos->ops = &blkcg_ioweight_ops;
	rqos->q = q;

	rq_qos_add(q, rqos);

	printk(KERN_ERR "JOSEF: activate blkcg policy\n");
	ret = blkcg_activate_policy(q, &blkcg_policy_ioweight);
	if (ret) {
		rq_qos_del(q, rqos);
		kfree(blkioweight);
		return ret;
	}
	timer_setup(&blkioweight->timer, blkioweight_timer_fn, 0);

	return 0;
}

static ssize_t ioweight_set_limit(struct kernfs_open_file *of, char *buf,
			     size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkcg_gq *blkg;
	struct blkg_conf_ctx ctx;
	struct ioweight_grp *ioweight;
	struct blk_ioweight *blkioweight;
	char *p, *tok;
	u64 weight = 0;
	u64 oldval;
	int ret;

	ret = blkg_conf_prep(blkcg, &blkcg_policy_ioweight, buf, &ctx);
	if (ret) {
		printk(KERN_ERR "conf prep failed\n");
		return ret;
	}

	ioweight = blkg_to_ioweight(ctx.blkg);
	p = ctx.body;

	ret = -EINVAL;
	while ((tok = strsep(&p, " "))) {
		char key[16];
		char val[21];	/* 18446744073709551616 */

		if (sscanf(tok, "%15[^=]=%20s", key, val) != 2)
			goto out;

		if (!strcmp(key, "weight")) {
			u64 v;

			if (sscanf(val, "%llu", &v) == 1)
				weight = v;
			else
				goto out;
		} else {
			goto out;
		}
	}

	blkg = ctx.blkg;
	oldval = ioweight->weight;

	if (blkg->parent) {
		struct ioweight_grp *parent = blkg_to_ioweight(blkg->parent);
		if (parent) {
			atomic64_sub(oldval, &parent->child_weight);
			atomic64_add(weight, &parent->child_weight);
		}
	}
	ioweight->weight = weight;

	WRITE_ONCE(ioweight->rq_depth.max_depth, UINT_MAX);
	wake_up_all(&ioweight->rq_wait.wait);
	blkioweight = ioweight->blkioweight;
	if (oldval && !weight)
		atomic_dec(&blkioweight->enabled);
	if (!oldval && weight)
		atomic_inc(&blkioweight->enabled);
	ret = 0;
out:
	if (ret)
		printk(KERN_ERR "hmm something failed\n");
	blkg_conf_finish(&ctx);
	return ret ?: nbytes;
}

static u64 ioweight_prfill_limit(struct seq_file *sf,
				  struct blkg_policy_data *pd, int off)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);
	const char *dname = blkg_dev_name(pd->blkg);

	if (!dname || !ioweight->weight)
		return 0;
	seq_printf(sf, "%s weight=%llu\n",
		   dname, (unsigned long long)ioweight->weight);
	return 0;
}

static int ioweight_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  ioweight_prfill_limit,
			  &blkcg_policy_ioweight, seq_cft(sf)->private, false);
	return 0;
}

static size_t ioweight_pd_stat(struct blkg_policy_data *pd, char *buf,
				size_t size)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);

	if (ioweight->rq_depth.max_depth == UINT_MAX)
		return scnprintf(buf, size, " depth=max");

	return scnprintf(buf, size, " depth=%u", ioweight->rq_depth.max_depth);
}


static struct blkg_policy_data *ioweight_pd_alloc(gfp_t gfp, int node)
{
	struct ioweight_grp *ioweight;
	int cpu;

	ioweight = kzalloc_node(sizeof(*ioweight), gfp, node);
	if (!ioweight)
		return NULL;

	ioweight->stat = __alloc_percpu_gfp(NR_BUCKETS *
					    sizeof(struct ioweight_stat),
					    __alignof__(struct ioweight_stat),
					    gfp);
	if (!ioweight->stat) {
		kfree(ioweight);
		return NULL;
	}

	for_each_online_cpu(cpu) {
		struct ioweight_stat *stat;
		int bucket;

		stat = per_cpu_ptr(ioweight->stat, cpu);
		for (bucket = 0; bucket < NR_BUCKETS; bucket++)
			ioweight_stat_init(&stat[bucket]);
	}
	return &ioweight->pd;
}

static void ioweight_pd_init(struct blkg_policy_data *pd)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);
	struct blkcg_gq *blkg = ioweight_to_blkg(ioweight);
	struct rq_qos *rqos = rq_qos_id(blkg->q, RQ_QOS_IOWEIGHT);
	struct blk_ioweight *blkioweight = BLKIOWEIGHT(rqos);

	rq_wait_init(&ioweight->rq_wait);
	ioweight->rq_depth.queue_depth = blkg->q->nr_requests;
	ioweight->rq_depth.max_depth = UINT_MAX;
	ioweight->rq_depth.default_depth = ioweight->rq_depth.queue_depth;
	ioweight->blkioweight = blkioweight;
	ioweight->weight = 0;
	ioweight->io_time = 5 * NSECS_PER_MSEC;
	atomic64_set(&ioweight->child_weight, 0);
}

static void ioweight_pd_offline(struct blkg_policy_data *pd)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);
	struct blkcg_gq *blkg = ioweight_to_blkg(ioweight);

	if (blkg->parent) {
		struct ioweight_grp *parent = blkg_to_ioweight(blkg->parent);
		if (parent)
			atomic64_sub(ioweight->weight, &parent->child_weight);
	}
	ioweight->weight = 0;
}

static void ioweight_pd_free(struct blkg_policy_data *pd)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);
	free_percpu(ioweight->stat);
	kfree(ioweight);
}

static struct cftype ioweight_files[] = {
	{
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = ioweight_print_limit,
		.write = ioweight_set_limit,
	},
	{}
};

static struct blkcg_policy blkcg_policy_ioweight = {
	.dfl_cftypes	= ioweight_files,
	.pd_alloc_fn	= ioweight_pd_alloc,
	.pd_init_fn	= ioweight_pd_init,
	.pd_offline_fn	= ioweight_pd_offline,
	.pd_free_fn	= ioweight_pd_free,
	.pd_stat_fn	= ioweight_pd_stat,
};

static int __init ioweight_init(void)
{
	return blkcg_policy_register(&blkcg_policy_ioweight);
}

static void __exit ioweight_exit(void)
{
	return blkcg_policy_unregister(&blkcg_policy_ioweight);
}

module_init(ioweight_init);
module_exit(ioweight_exit);
