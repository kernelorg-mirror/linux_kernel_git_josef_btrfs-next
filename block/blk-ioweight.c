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
	struct blk_stat_callback *cb;
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

struct child_weight_info {
	struct percpu_counter total_time;
	u64 slots[TIME_SLOTS];
	unsigned slot;
	u64 last_total_time;
};

struct ioweight_grp {
	struct blkg_policy_data pd;
	struct percpu_counter total_time;
	struct blk_ioweight *blkioweight;
	struct rq_depth rq_depth;
	struct rq_wait rq_wait;

	struct child_weight_info info;
	u64 weight;
	atomic64_t child_weight;
	u64 slots[TIME_SLOTS];
	unsigned slot;
	u64 last_total_time;
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

static void ioweight_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	atomic_dec(&rqw->inflight);
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
	if (!blk_stat_is_active(blkioweight->cb))
		blk_stat_activate_nsecs(blkioweight->cb, NSEC_PER_SEC);
}

static void ioweight_record_time(struct ioweight_grp *ioweight,
				  struct bio_issue *issue, u64 now,
				  bool issue_as_root)
{
	struct blkcg_gq *blkg = ioweight_to_blkg(ioweight);
	u64 start = bio_issue_time(issue);
	u64 req_time;

	/*
	 * Have to do this so we are truncated to the correct time that our
	 * issue is truncated to.
	 */
	now = __bio_issue_time(now);

	if (now <= start)
		return;

	req_time = now - start;
	percpu_counter_add(&ioweight->total_time, req_time);
	if (blkg->parent) {
		struct ioweight_grp *parent = blkg_to_ioweight(blkg->parent);
		if (parent)
			percpu_counter_add(&parent->info.total_time, req_time);
	}
}

static void blkcg_ioweight_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw;
	struct ioweight_grp *ioweight;
	u64 now = ktime_to_ns(ktime_get());
	bool issue_as_root = bio_issue_as_root_blkg(bio);
	bool enabled = false;

	blkg = bio->bi_blkg;
	if (!blkg)
		return;

	ioweight = blkg_to_ioweight(bio->bi_blkg);
	if (!ioweight)
		return;

	enabled = blk_ioweight_enabled(ioweight->blkioweight);
	while (blkg && blkg->parent) {
		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight) {
			blkg = blkg->parent;
			continue;
		}
		rqw = &ioweight->rq_wait;

		atomic_dec(&rqw->inflight);
		if (!enabled || ioweight->weight == 0)
			goto next;
		ioweight_record_time(ioweight, &bio->bi_issue, now,
				      issue_as_root);
next:
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
		atomic_dec(&rqw->inflight);
		wake_up(&rqw->wait);
next:
		blkg = blkg->parent;
	}
}

static void blkcg_ioweight_exit(struct rq_qos *rqos)
{
	struct blk_ioweight *blkioweight = BLKIOWEIGHT(rqos);


	blk_stat_free_callback(blkioweight->cb);
	blkcg_deactivate_policy(rqos->q, &blkcg_policy_ioweight);
	kfree(blkioweight);
}

static void blkcg_ioweight_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	bio_issue_init(&bio->bi_issue, bio_sectors(bio));
}

static struct rq_qos_ops blkcg_ioweight_ops = {
	.throttle = blkcg_ioweight_throttle,
	.track = blkcg_ioweight_track,
	.cleanup = blkcg_ioweight_cleanup,
	.done_bio = blkcg_ioweight_done_bio,
	.exit = blkcg_ioweight_exit,
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

static void blkioweight_timer_fn(struct blk_stat_callback *cb)
{
	struct blk_ioweight *blkioweight = cb->data;
	struct blkcg_gq *blkg;
	struct cgroup_subsys_state *pos_css;
	u64 saturation;

	saturation = cb->stat[0].time;
	if (blkioweight->saturation < round_down(saturation, POW2_NSEC_PER_MSEC))
		blkioweight->saturation = round_down(saturation, POW2_NSEC_PER_MSEC);
	trace_printk("current saturation %llu, highest %llu\n", saturation, blkioweight->saturation);

	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css,
				     blkioweight->rqos.q->root_blkg) {
		struct ioweight_grp *ioweight, *parent;
		u64 parent_total_time, total_weight;
		u64 total_time, actual_share, weight_share;

		/*
		 * We could be exiting, don't access the pd unless we have a
		 * ref on the blkg.
		 */
		if (!blkg_tryget(blkg))
			continue;

		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight)
			goto next;

		/*
		 * We need to sum up the childrens time as well as our own time.
		 * We don't user our childrens time here, it's just calculated
		 * so the children don't have to caculate it when we reach them.
		 */
		total_time = percpu_counter_sum_reset(&ioweight->info.total_time);
		ioweight->info.last_total_time += total_time;
		ioweight->info.last_total_time -= ioweight->info.slots[ioweight->info.slot];
		ioweight->info.slots[ioweight->info.slot] = total_time;
		if (++ioweight->info.slot >= TIME_SLOTS)
			ioweight->info.slot = 0;

		total_time = percpu_counter_sum_reset(&ioweight->total_time);
		ioweight->last_total_time += total_time;
		ioweight->last_total_time -= ioweight->slots[ioweight->slot];
		ioweight->slots[ioweight->slot] = total_time;
		if (++ioweight->slot >= TIME_SLOTS)
			ioweight->slot = 0;

		total_time = ioweight->last_total_time;
		trace_printk("weight %llu, child_time %llu, last_time %llu, saturation %llu\n",
			     ioweight->weight, ioweight->info.last_total_time,
			     ioweight->last_total_time, blkioweight->saturation);

		/* We are the root, there's nothing more to do. */
		if (!blkg->parent)
			goto next;

		/*
		 * If we're holding a ref to the child then the parent is pinned
		 * as well, so we're good.
		 */
		parent = blkg_to_ioweight(blkg->parent);
		if (!parent)
			goto next;

		/*
		 * We didn't do any IO, scale us up if we were scaled down at
		 * all.
		 */
		if (!total_time) {
			scale_up(ioweight);
			goto next;
		}

		/*
		 * We were the only group doing IO this go around, scale
		 * ourselves up and carry on.
		 */
		if (total_time == parent->info.last_total_time) {
			scale_up(ioweight);
			goto next;
		}

		/*
		 * actual_share is the percentage of time spent doing IO compard
		 * to the overall time spent doing IO for all of the peers in
		 * this group.
		 *
		 * weight_share is the percentage of share of this group
		 * compared to the weight of all of its peers.
		 */
		parent_total_time = max_t(u64, 1, parent->info.last_total_time);
		total_weight = max_t(u64, 1, atomic64_read(&parent->child_weight));
		if (total_weight < ioweight->weight)
			printk(KERN_ERR "WTF\n");
		actual_share = div64_u64(total_time * 100, parent_total_time);
		weight_share = div64_u64(ioweight->weight * 100, total_weight);

		trace_printk("weight %llu, total_time %llu, total_weight %llu, parent_total_time %llu, actual_share %llu, weight_share %llu\n", ioweight->weight, total_time, total_weight, parent_total_time, actual_share, weight_share);
		/* We used more than our fair share, scale down. */
		if (actual_share > weight_share)
			scale_down(ioweight);

		/* We used less than our fair share, scale up. */
		if (actual_share < weight_share)
			scale_up(ioweight);
next:
		blkg_put(blkg);
	}
	rcu_read_unlock();
}

static int blkioweight_bucket_fn(const struct request *rq)
{
	return 0;
}

int blk_ioweight_init(struct request_queue *q)
{
	struct blk_ioweight *blkioweight;
	struct rq_qos *rqos;
	int ret;

	blkioweight = kzalloc(sizeof(*blkioweight), GFP_KERNEL);
	if (!blkioweight)
		return -ENOMEM;

	blkioweight->cb = blk_stat_alloc_callback(blkioweight_timer_fn,
						  blkioweight_bucket_fn, 1,
						  blkioweight);
	if (!blkioweight->cb) {
		kfree(blkioweight);
		return -ENOMEM;
	}

	rqos = &blkioweight->rqos;
	rqos->id = RQ_QOS_IOWEIGHT;
	rqos->ops = &blkcg_ioweight_ops;
	rqos->q = q;

	rq_qos_add(q, rqos);

	printk(KERN_ERR "JOSEF: activate blkcg policy\n");
	ret = blkcg_activate_policy(q, &blkcg_policy_ioweight);
	if (ret) {
		rq_qos_del(q, rqos);
		blk_stat_free_callback(blkioweight->cb);
		kfree(blkioweight);
		return ret;
	}
	blk_stat_add_callback(q, blkioweight->cb);

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

	ioweight = kzalloc_node(sizeof(*ioweight), gfp, node);
	if (!ioweight)
		return NULL;
	if (percpu_counter_init(&ioweight->total_time, 0, gfp)) {
		kfree(ioweight);
		return NULL;
	}
	if (percpu_counter_init(&ioweight->info.total_time, 0, gfp)) {
		percpu_counter_destroy(&ioweight->total_time);
		kfree(ioweight);
		return NULL;
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
	percpu_counter_destroy(&ioweight->total_time);
	percpu_counter_destroy(&ioweight->info.total_time);
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
