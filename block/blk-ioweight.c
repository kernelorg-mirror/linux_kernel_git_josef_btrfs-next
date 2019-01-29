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
#include <linux/time64.h>
#include <trace/events/block.h>
#include "blk-rq-qos.h"
#include "blk-stat.h"
#include "ordered-wq.h"

#define TIMING 1

static struct blkcg_policy blkcg_policy_ioweight;
struct ioweight_grp;

struct blk_ioweight {
	struct rq_qos rqos;
	ordered_wait_queue_head_t owq;
	u64 saturation;
	atomic_t enabled;
	atomic64_t iocounter;
	u64 last_io_count;
	atomic_t global_waiters;
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

static inline void
blk_ioweight_inc_global_waiter(struct blk_ioweight *blkioweight)
{
	atomic_inc(&blkioweight->global_waiters);
}

static inline void
blk_ioweight_dec_global_waiter(struct blk_ioweight *blkioweight)
{
	if (atomic_dec_and_test(&blkioweight->global_waiters))
		ordered_wake_up_all(&blkioweight->owq);
}

static inline void blk_ioweight_io_done(struct blk_ioweight *blkioweight)
{
	u64 key;

	if (atomic_read(&blkioweight->global_waiters) == 0)
		return;
	key = atomic64_inc_return(&blkioweight->iocounter);
	ordered_wake_up(&blkioweight->owq, key);
}

#define TIME_SLOTS 10

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

struct child_time_info {
	struct percpu_counter total_time;
	u64 slots[TIME_SLOTS];
	unsigned slot;
	u64 total_1sec;
	u64 total_5sec;
	u64 total_10sec;
};

struct child_history {
	u64 last_1sec;
	u64 last_5sec;
	u64 last_10sec;
};

struct ioweight_grp {
	struct blkg_policy_data pd;
	struct blk_ioweight *blkioweight;
	struct rq_depth rq_depth;
	struct rq_wait rq_wait;
	struct ioweight_grp *next;

	u64 io_charge;
	u64 weight;
	atomic64_t child_weight;

	char *name;
	struct ioweight_stat __percpu *stat;
	struct ioweight_stat sum[NR_BUCKETS];
	u64 children_total_load;
	u64 load;
	u64 io_time;

	struct child_time_info info;
	struct child_time_info child_info;
	struct child_history history;
	int scale;
	int stable;
	int wait_for_stable;
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
	ioweight->io_time = max_t(u64, NSEC_PER_USEC,
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

#if 0
static struct coefficient hdd_coefficients[] = {
	[READ_IOPS_COEF]	= { .numerator = 131, .denominator = 100000 }, // 1.31e-3
	[READ_BW_COEF]		= { .numerator = 131, .denominator = 1000000000ULL}, // 1.31e-7
	[WRITE_IOPS_COEF]	= { .numerator = 257, .denominator = 1000ULL}, // .257
	[WRITE_BW_COEF]		= { .numerator = 503, .denominator = 1000000000ULL}, // 5.03e-9
};
#endif

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

static noinline void wait_global_counter(struct ioweight_grp *ioweight, u64 key)
{
	struct blk_ioweight *blkioweight = ioweight->blkioweight;
	ordered_wait_queue_entry_t entry;
	u64 last = atomic64_read(&blkioweight->iocounter);
	u64 cur = 0;
	bool slept = false;

	key += last;
	init_ordered_wait_queue_entry(&entry, current, key);
	while (1) {
		ordered_prepare_to_wait(&blkioweight->owq, &entry,
					TASK_UNINTERRUPTIBLE);
		cur = atomic64_read(&blkioweight->iocounter);
		if ((slept && cur == last) || cur >= key)
			break;
		io_schedule_timeout(HZ);
		last = cur;
		slept = true;
	}
	ordered_finish_wait(&blkioweight->owq, &entry);
}

static void __blkcg_ioweight_throttle(struct rq_qos *rqos,
				       struct ioweight_grp *ioweight,
				       bool issue_as_root,
				       bool use_memdelay)
{
	struct rq_wait *rqw = &ioweight->rq_wait;
	u64 key = READ_ONCE(ioweight->io_charge);

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
	if (key == 0)
		return;

	wait_global_counter(ioweight, key);
}

static void blkcg_ioweight_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct blk_ioweight *blkioweight = BLKIOWEIGHT(rqos);
	struct blkcg_gq *blkg = bio->bi_blkg;
	bool issue_as_root = bio_issue_as_root_blkg(bio);
	bool arm = false;

	if (!blk_ioweight_enabled(blkioweight))
		return;

	for (; blkg; blkg = blkg->parent) {
		struct ioweight_grp *ioweight = blkg_to_ioweight(blkg);
		if (!ioweight)
			continue;

		arm = true;
		__blkcg_ioweight_throttle(rqos, ioweight, issue_as_root,
				     (bio->bi_opf & REQ_SWAP) == REQ_SWAP);
	}
	if (arm && !timer_pending(&blkioweight->timer))
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

#ifdef TIMING
void blk_ioweight_stat_add(struct request *rq, u64 now) {}
#else
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
#endif

#ifdef TIMING
static void ioweight_record_time(struct ioweight_grp *ioweight,
				 u64 start, u64 now,
				 bool issue_as_root)
{
	struct blkcg_gq *blkg = ioweight_to_blkg(ioweight);
	u64 req_time;

	if (now <= start)
		return;

	req_time = now - start;
	percpu_counter_add(&ioweight->info.total_time, req_time);
	if (blkg->parent) {
		struct ioweight_grp *parent = blkg_to_ioweight(blkg->parent);
		if (parent)
			percpu_counter_add(&parent->child_info.total_time, req_time);
	}
}
#endif

static void blkcg_ioweight_done(struct rq_qos *rqos, struct request *rq)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw;
	struct ioweight_grp *ioweight;
#ifdef TIMING
	u64 now = ktime_to_ns(ktime_get());
	bool issue_as_root = (rq->cmd_flags & (REQ_META | REQ_SWAP)) != 0;
#endif

	blkg = rq->blkg;
	if (!blkg)
		return;

	ioweight = blkg_to_ioweight(blkg);
	if (!ioweight)
		return;

	if (!blk_ioweight_enabled(ioweight->blkioweight))
		return;

	blk_ioweight_io_done(ioweight->blkioweight);

	while (blkg && blkg->parent) {
		ioweight = blkg_to_ioweight(blkg);
		if (!ioweight) {
			blkg = blkg->parent;
			continue;
		}
		rqw = &ioweight->rq_wait;
		BUG_ON(atomic_dec_return(&rqw->inflight) < 0);
#ifdef TIMING
		if (ioweight->weight != 0)
			ioweight_record_time(ioweight, rq->io_start_time_ns, now,
					     issue_as_root);
#endif
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
//	bio_issue_init(&bio->bi_issue, bio_sectors(bio));
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
	.done = blkcg_ioweight_done,
	.exit = blkcg_ioweight_exit,
	.debugfs_attrs = ioweight_debugfs_attrs,
};

#define SCALE_DOWN_FACTOR 2
#define SCALE_UP_FACTOR 5

static inline unsigned long scale_amount(unsigned long qd, bool up)
{
	return max(up ? qd >> SCALE_UP_FACTOR : qd >> SCALE_DOWN_FACTOR, 1UL);
}

static void scale_up(struct ioweight_grp *ioweight)
{
	unsigned long qd = ioweight->blkioweight->rqos.q->nr_requests;
	unsigned long old = ioweight->rq_depth.max_depth;
	unsigned long scale = scale_amount(qd, true);
	u64 io_charge = ioweight->io_charge;

	if (old > qd)
		old = qd;

	if (old == 1 && io_charge) {
		io_charge--;
		trace_printk("%s weight %llu scale up charge %llu\n", ioweight->name, ioweight->weight, io_charge);
		WRITE_ONCE(ioweight->io_charge, io_charge);
		if (io_charge == 0)
			blk_ioweight_dec_global_waiter(ioweight->blkioweight);
		return;
	}

	if (old < qd) {
		old = min(old + scale, qd);
		WRITE_ONCE(ioweight->rq_depth.max_depth, old);
		wake_up_all(&ioweight->rq_wait.wait);
	}

	trace_printk("%s weight %llu scale up %lu\n", ioweight->name, ioweight->weight, old);
}

static void scale_down(struct ioweight_grp *ioweight, u64 mult)
{
	unsigned long qd = ioweight->blkioweight->rqos.q->nr_requests;
	unsigned long old = ioweight->rq_depth.max_depth;
	unsigned long scale = scale_amount(qd, true) * mult;

	if (old > qd)
		old = qd;

	if (old == 1) {
		u64 io_charge = ioweight->io_charge + mult;
		WRITE_ONCE(ioweight->io_charge, io_charge);
		if (io_charge == 1)
			blk_ioweight_inc_global_waiter(ioweight->blkioweight);
		trace_printk("%s weight %llu add charge %llu\n", ioweight->name, ioweight->weight,
			     io_charge);
		/*
		blkcg_add_delay(ioweight_to_blkg(ioweight),
				ktime_to_ns(ktime_get()),
				250 * NSEC_PER_MSEC);
		blkcg_use_delay(ioweight_to_blkg(ioweight));
		*/
		return;
	}

	if (scale > old)
		old = 1;
	else
		old -= scale;
	trace_printk("%s weight %llu scale down %lu\n", ioweight->name, ioweight->weight, old);
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

#ifdef TIMING

struct running_share {
	u64 share_1sec;
	u64 share_5sec;
	u64 share_10sec;
};

static inline void update_times(struct child_time_info *info,
				struct child_history *history)
{
	u64 total_time = percpu_counter_sum_reset(&info->total_time);
	unsigned slot_5sec = (info->slot + 5) % 10;

	if (history) {
		history->last_1sec = info->total_1sec;
		if (!(info->slot % 5))
			history->last_5sec = info->total_5sec;
		if (!(info->slot % 10))
			history->last_10sec = info->total_10sec;
	}

	info->total_10sec += total_time;
	info->total_5sec += total_time;
	info->total_1sec = total_time;
	info->total_10sec -= info->slots[info->slot];
	info->total_5sec -= info->slots[slot_5sec];
	info->slots[info->slot] = total_time;
	info->slot++;
	if (info->slot == TIME_SLOTS)
		info->slot = 0;
}

static inline u64 calc_share(u64 parent_total, u64 child_total)
{
	parent_total = max_t(u64, 1, parent_total);
	child_total = max_t(u64, 1, child_total);
	return div64_u64(child_total * 100, parent_total);
}

static inline void calculate_shares(struct child_time_info *parent,
				    struct child_time_info *info,
				    struct running_share *share)
{
	share->share_1sec = calc_share(parent->total_1sec, info->total_1sec);
	share->share_5sec = calc_share(parent->total_5sec, info->total_5sec);
	share->share_10sec = calc_share(parent->total_10sec, info->total_10sec);
}

static inline u64 safe_div(u64 numerator, u64 denominator)
{
	numerator = max_t(u64, 1, numerator);
	denominator = max_t(u64, 1, denominator);
	return div64_u64(numerator, denominator);
}

/*
 * |val1 - val2| / ((val1 + val2) / 2) * 100
 */
static inline u64 pct_diff(u64 val1, u64 val2)
{
	u64 abs;
	if (val1 < val2)
		abs = val2 - val1;
	else
		abs = val1 - val2;
	val1 += val2;
	val1 >>= 1;
	abs *= 100;
	return safe_div(abs, val1);
}

static inline int check_stable(struct ioweight_grp *ioweight)
{
	u64 diff;
	diff = pct_diff(ioweight->history.last_5sec,
			ioweight->child_info.total_5sec);
	if (diff > 5) {
		trace_printk("%s last5sec %llu, total5sec %llu, diff %llu\n",
			     ioweight->name, ioweight->history.last_5sec,
			     ioweight->child_info.total_5sec, diff);
		return 0;
	}
	diff = pct_diff(ioweight->history.last_10sec,
			ioweight->child_info.total_10sec);
	if (diff > 5) {
		trace_printk("%s last10sec %llu, total10sec %llu, diff %llu\n",
			     ioweight->name, ioweight->history.last_10sec,
			     ioweight->child_info.total_10sec, diff);
	}
	return (diff > 5) ? 0 : 1;
}

static void blkioweight_timer_fn(struct timer_list *t)
{
	struct blk_ioweight *blkioweight = from_timer(blkioweight, t, timer);
	struct blkcg_gq *blkg;
	struct cgroup_subsys_state *pos_css;
	bool reset_io = false;
	bool rearm = false;

	if (atomic_read(&blkioweight->global_waiters)) {
		u64 cur = atomic64_read(&blkioweight->iocounter);
		if (blkioweight->last_io_count == cur)
			reset_io = true;
		blkioweight->last_io_count = cur;
	}
	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css,
				     blkioweight->rqos.q->root_blkg) {
		struct ioweight_grp *ioweight, *parent;
		struct running_share share;
		u64 weight_share;
		u64 allowable = 0;
		int scale = 0;

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
		update_times(&ioweight->info, NULL);
		update_times(&ioweight->child_info, &ioweight->history);
		ioweight->stable = check_stable(ioweight);

		if (!ioweight->weight)
			goto next;

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
		 * We were the only group doing IO this go around, scale
		 * ourselves up and carry on.
		 */
		if (ioweight->info.total_5sec ==
		    parent->child_info.total_5sec) {
			if (ioweight->io_charge) {
				WRITE_ONCE(ioweight->io_charge, 0);
				blk_ioweight_dec_global_waiter(blkioweight);
				reset_io = true;
			}
			scale_up(ioweight);
			ioweight->scale = 1;
			goto next;
		}

		if (parent->stable)
			goto check_shares;

		if (ioweight->wait_for_stable)
			goto next;

		/*
		 * We scaled down last time, we need to see if the overall usage
		 * went down.
		 */
		if (ioweight->scale < 0 &&
		    parent->child_info.total_5sec <
		    parent->history.last_5sec) {
			u64 diff = pct_diff(parent->child_info.total_5sec,
					    parent->history.last_5sec);
			if (diff >= 5) {
				trace_printk("%s needs to wait for stable\n",
					     ioweight->name);
				ioweight->wait_for_stable = 1;
				scale_up(ioweight);
				ioweight->scale = 0;
				goto next;
			}
		}
check_shares:
		ioweight->wait_for_stable = 0;
		calculate_shares(&parent->child_info, &ioweight->info, &share);
		weight_share = calc_share(atomic64_read(&parent->child_weight),
					  ioweight->weight);

		if (weight_share > 5)
			allowable = 3;

		trace_printk("%s 1sec total %llu 5sec total %llu 10sec total %llu\n",
			     ioweight->name, ioweight->info.total_1sec, ioweight->info.total_5sec,
			     ioweight->info.total_10sec);
		trace_printk("parent 1sec total %llu 5sec total %llu 10sec total %llu\n",
			     parent->child_info.total_1sec, parent->child_info.total_5sec,
			     parent->child_info.total_10sec);
		trace_printk("%s 1sec share %llu 5sec share %llu 10sec share %llu weight %llu\n",
			     ioweight->name, share.share_1sec, share.share_5sec,
			     share.share_10sec, ioweight->weight);

		if (share.share_1sec == 0) {
			scale_up(ioweight);
			ioweight->scale = 1;
			goto next;
		}

		if (share.share_1sec > weight_share) {
			scale = max_t(int, 1, safe_div(share.share_1sec, weight_share));
			if (share.share_5sec > weight_share)
				scale += max_t(int, 1, safe_div(share.share_5sec, weight_share));
			else if (share.share_5sec < weight_share)
				scale -= max_t(int, 1, safe_div(weight_share, share.share_5sec));
			if (share.share_10sec > weight_share)
				scale += max_t(int, 1, safe_div(share.share_10sec, weight_share));
			else if (share.share_10sec < weight_share)
				scale -= max_t(int, 1, safe_div(weight_share, share.share_10sec));
			if (scale > 0) {
				scale_down(ioweight, scale);
				scale = -scale;
			} else if (scale < 0) {
				scale = 0;
			}
		} else if (share.share_1sec < weight_share) {
			scale = 1;
			if (share.share_5sec > weight_share)
				scale--;
			else if (share.share_5sec < weight_share)
				scale++;
			if (share.share_10sec > weight_share)
				scale--;
			else if (share.share_10sec < weight_share)
				scale++;
			if (scale > 0)
				scale_up(ioweight);
			else if (scale < 0)
				scale = 0;
		}
		ioweight->scale = scale;
#if 0
		/*
		 * If it's been 10 seconds, check our 10 second average and
		 * scale accordingly.
		 */
		if (share.share_10sec < weight_share)
			scale_up(ioweight);
		else if (share.share_10sec > weight_share) {
//			u64 mult = max_t(u64, 1, div64_u64(share.share_10sec, weight_share));
			scale_down(ioweight, 1);
		}

		/*
		 * If it's been 5 seconds, check our 5 second average, and scale
		 * accordingly.
		 */
		if (share.share_5sec < weight_share)
			scale_up(ioweight);
		else if (share.share_5sec > weight_share) {
//			u64 mult = max_t(u64, 1, div64_u64(share.share_5sec, weight_share));
			scale_down(ioweight, 1);
		}

		/* Scale according to our most recent information.
		if (share.share_1sec - allowable > weight_share)
			scale_down(ioweight, 1);
		else if (share.share_1sec + allowable < weight_share)
			scale_up(ioweight);
		*/
#endif
next:
		blkg_put(blkg);
	}
	rcu_read_unlock();
	if (reset_io)
		ordered_wake_up_all(&blkioweight->owq);
	if (rearm && !timer_pending(&blkioweight->timer))
		mod_timer(&blkioweight->timer, jiffies + HZ);
}
#else
static void blkioweight_timer_fn(struct timer_list *t)
{
	struct blk_ioweight *blkioweight = from_timer(blkioweight, t, timer);
	struct blkcg_gq *blkg;
	struct cgroup_subsys_state *pos_css;
	struct ioweight_grp *head = NULL, *next;
	bool reset_io = false;
	bool rearm = false;

	if (atomic_read(&blkioweight->global_waiters)) {
		u64 cur = atomic64_read(&blkioweight->iocounter);
		if (blkioweight->last_io_count == cur)
			reset_io = true;
		blkioweight->last_io_count = cur;
	}
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

		rearm = true;
		child_total_load = max_t(u64, 1, parent->children_total_load);
		child_weight = max_t(u64, 1, atomic64_read(&parent->child_weight));
		load = max_t(u64, head->load, 1);
		weight = max_t(u64, head->weight, 1);

		actual_share = div64_u64(load * 100, child_total_load);
		weight_share = div64_u64(weight * 100, child_weight);
		trace_printk("%s: weight %llu load %llu parent total load %llu, actual_share %llu, weight_share %llu\n",
			     head->name, head->weight, head->load, parent->children_total_load,
			     actual_share, weight_share);

		if (actual_share > weight_share)
			scale_down(head);
		if (actual_share < weight_share)
			scale_up(head);
		if (reset_io) {
			if (head->io_charge) {
				WRITE_ONCE(head->io_charge, 0);
				blk_ioweight_dec_global_waiter(blkioweight);
			}
		}
		blkg_put(blkg);
	}
	if (reset_io)
		ordered_wake_up_all(&blkioweight->owq);
	if (rearm && !timer_pending(&blkioweight->timer))
		mod_timer(&blkioweight->timer, jiffies + HZ);
}
#endif

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
	init_ordered_wait_queue_head(&blkioweight->owq);
	atomic64_set(&blkioweight->iocounter, 0);
	atomic_set(&blkioweight->global_waiters, 0);

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
	char *p, *tok, *name = NULL;
	u64 weight = 0;
	u64 oldval;
	int ret;

	if (of->kn->parent)
		name = kstrdup(of->kn->parent->name, GFP_KERNEL);

	printk(KERN_ERR "setting limit?\n");
	printk(KERN_ERR "set limit on %s\n", name);
	ret = blkg_conf_prep(blkcg, &blkcg_policy_ioweight, buf, &ctx);
	if (ret) {
		printk(KERN_ERR "conf prep failed\n");
		kfree(name);
		return ret;
	}

	ioweight = blkg_to_ioweight(ctx.blkg);
	if (!ioweight) {
		printk(KERN_ERR "wtf, no ioweight?\n");
		ret = -EINVAL;
		goto out;
	}
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
	blkioweight = ioweight->blkioweight;
	if (ioweight->name == NULL) {
		ioweight->name = name;
		name = NULL;
	}
	if (blkg->parent) {
		struct ioweight_grp *parent = blkg_to_ioweight(blkg->parent);
		if (parent) {
			atomic64_sub(oldval, &parent->child_weight);
			atomic64_add(weight, &parent->child_weight);
		}
	}
	ioweight->weight = weight;
	if (ioweight->io_charge) {
		WRITE_ONCE(ioweight->io_charge, 0);
		blk_ioweight_dec_global_waiter(blkioweight);
	}

	WRITE_ONCE(ioweight->rq_depth.max_depth, UINT_MAX);
	wake_up_all(&ioweight->rq_wait.wait);
	if (oldval && !weight)
		atomic_dec(&blkioweight->enabled);
	if (!oldval && weight)
		atomic_inc(&blkioweight->enabled);
	ordered_wake_up_all(&blkioweight->owq);
	ret = 0;
out:
	if (ret)
		printk(KERN_ERR "hmm something failed\n");
	blkg_conf_finish(&ctx);
	kfree(name);
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

#ifdef TIMING
static struct blkg_policy_data *ioweight_pd_alloc(gfp_t gfp, int node)
{
	struct ioweight_grp *ioweight;

	ioweight = kzalloc_node(sizeof(*ioweight), gfp, node);
	if (!ioweight)
		return NULL;
	if (percpu_counter_init(&ioweight->info.total_time, 0, gfp)) {
		kfree(ioweight);
		return NULL;
	}
	if (percpu_counter_init(&ioweight->child_info.total_time, 0, gfp)) {
		percpu_counter_destroy(&ioweight->info.total_time);
		kfree(ioweight);
		return NULL;
	}
	return &ioweight->pd;
}
#else
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
#endif

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
	ioweight->io_time = 5 * NSEC_PER_MSEC;
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

#ifdef TIMING
static void ioweight_pd_free(struct blkg_policy_data *pd)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);
	percpu_counter_destroy(&ioweight->info.total_time);
	percpu_counter_destroy(&ioweight->child_info.total_time);
	kfree(ioweight);
}
#else
static void ioweight_pd_free(struct blkg_policy_data *pd)
{
	struct ioweight_grp *ioweight = pd_to_ioweight(pd);
	free_percpu(ioweight->stat);
	kfree(ioweight);
}
#endif

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
