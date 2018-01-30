/*
 * buffered writeback throttling. loosely based on CoDel. We can't drop
 * packets for IO scheduling, so the logic is something like this:
 *
 * - Monitor latencies in a defined window of time.
 * - If the minimum latency in the above window exceeds some target, increment
 *   scaling step and scale down queue depth by a factor of 2x. The monitoring
 *   window is then shrunk to 100 / sqrt(scaling step + 1).
 * - For any window where we don't have solid data on what the latencies
 *   look like, retain status quo.
 * - If latencies look good, decrement scaling step.
 * - If we're only doing writes, allow the scaling step to go negative. This
 *   will temporarily boost write performance, snapping back to a stable
 *   scaling step of 0 if reads show up or the heavy writers finish. Unlike
 *   positive scaling steps where we shrink the monitoring window, a negative
 *   scaling step retains the default step==0 window size.
 *
 * Copyright (C) 2016 Jens Axboe
 *
 */
#include <linux/kernel.h>
#include <linux/blk_types.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/swap.h>
#include <linux/module.h>

#include "blk-wbt.h"

#define CREATE_TRACE_POINTS
#include <trace/events/wbt.h>

static inline bool rwb_enabled(struct rq_wb *rwb)
{
	return rwb && rwb->wb_normal != 0;
}

/*
 * Increment 'v', if 'v' is below 'below'. Returns true if we succeeded,
 * false if 'v' + 1 would be bigger than 'below'.
 */
static bool atomic_inc_below(atomic_t *v, int below)
{
	int cur = atomic_read(v);

	for (;;) {
		int old;

		if (cur >= below)
			return false;
		old = atomic_cmpxchg(v, cur, cur + 1);
		if (old == cur)
			break;
		cur = old;
	}

	return true;
}

u64 wbt_get_min_lat(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	if (!rqos)
		return 0;
	return RQWB(rqos)->rq_depth.min_lat_nsec;
}

void wbt_set_min_lat(struct request_queue *q, u64 val)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	if (!rqos)
		return;
	RQWB(rqos)->rq_depth.min_lat_nsec = val;
	RQWB(rqos)->enable_state = WBT_STATE_ON_MANUAL;
}

static void wb_timestamp(struct rq_wb *rwb, unsigned long *var)
{
	if (rwb_enabled(rwb)) {
		const unsigned long cur = jiffies;

		if (cur != *var)
			*var = cur;
	}
}

/*
 * If a task was rate throttled in balance_dirty_pages() within the last
 * second or so, use that to indicate a higher cleaning rate.
 */
static bool wb_recent_wait(struct rq_wb *rwb)
{
	struct bdi_writeback *wb = &rwb->rqos.q->backing_dev_info->wb;

	return time_before(jiffies, wb->dirty_sleep + HZ);
}

static inline struct rq_wait *get_rq_wait(struct rq_wb *rwb, bool is_kswapd)
{
	return &rwb->rq_wait[is_kswapd];
}

static void rwb_wake_all(struct rq_wb *rwb)
{
	int i;

	for (i = 0; i < WBT_NUM_RWQ; i++) {
		struct rq_wait *rqw = &rwb->rq_wait[i];

		if (waitqueue_active(&rqw->wait))
			wake_up_all(&rqw->wait);
	}
}

static void __wbt_done(struct rq_qos *rqos, enum wbt_flags wb_acct)
{
	struct rq_wb *rwb = RQWB(rqos);
	struct rq_wait *rqw;
	int inflight, limit;

	if (!(wb_acct & WBT_TRACKED))
		return;

	rqw = get_rq_wait(rwb, wb_acct & WBT_KSWAPD);
	inflight = atomic_dec_return(&rqw->inflight);

	/*
	 * wbt got disabled with IO in flight. Wake up any potential
	 * waiters, we don't have to do more than that.
	 */
	if (unlikely(!rwb_enabled(rwb))) {
		rwb_wake_all(rwb);
		return;
	}

	/*
	 * If the device does write back caching, drop further down
	 * before we wake people up.
	 */
	if (rwb->wc && !wb_recent_wait(rwb))
		limit = 0;
	else
		limit = rwb->wb_normal;

	/*
	 * Don't wake anyone up if we are above the normal limit.
	 */
	if (inflight && inflight >= limit)
		return;

	if (waitqueue_active(&rqw->wait)) {
		int diff = limit - inflight;

		if (!inflight || diff >= rwb->wb_background / 2)
			wake_up_all(&rqw->wait);
	}
}

/*
 * Called on completion of a request. Note that it's also called when
 * a request is merged, when the request gets freed.
 */
static void wbt_done(struct rq_qos *rqos, struct blk_issue_stat *stat)
{
	struct rq_wb *rwb = RQWB(rqos);

	if (!wbt_is_tracked(stat)) {
		if (rwb->sync_cookie == stat) {
			rwb->sync_issue = 0;
			rwb->sync_cookie = NULL;
		}

		if (wbt_is_read(stat))
			wb_timestamp(rwb, &rwb->last_comp);
	} else {
		WARN_ON_ONCE(stat == rwb->sync_cookie);
		__wbt_done(rqos, wbt_stat_to_mask(stat));
	}
	wbt_clear_state(stat);
}

void rq_qos_cleanup(struct request_queue *q, enum wbt_flags wb_acct)
{
	struct rq_qos *rqos;

	for (rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->ops->cleanup)
			rqos->ops->cleanup(rqos, wb_acct);
	}
}

void rq_qos_done(struct request_queue *q, struct blk_issue_stat *stat)
{
	struct rq_qos *rqos;

	for (rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->ops->done)
			rqos->ops->done(rqos, stat);
	}
}

void rq_qos_issue(struct request_queue *q, struct blk_issue_stat *stat)
{
	struct rq_qos *rqos;

	for(rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->ops->issue)
			rqos->ops->issue(rqos, stat);
	}
}

void rq_qos_requeue(struct request_queue *q, struct blk_issue_stat *stat)
{
	struct rq_qos *rqos;

	for(rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->ops->requeue)
			rqos->ops->requeue(rqos, stat);
	}
}

enum wbt_flags rq_qos_throttle(struct request_queue *q, struct bio *bio,
			       spinlock_t *lock)
{
	struct rq_qos *rqos;
	enum wbt_flags flags = 0;

	for(rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->ops->throttle)
			flags |= rqos->ops->throttle(rqos, bio, lock);
	}
	return flags;
}

void rq_qos_done_bio(struct request_queue *q, struct bio *bio)
{
	struct rq_qos *rqos;

	for(rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->ops->done_bio)
			rqos->ops->done_bio(rqos, bio);
	}
}

/*
 * Return true, if we can't increase the depth further by scaling
 */
static bool calc_max_depth(struct rq_depth *rqd)
{
	unsigned int depth;
	bool ret = false;

	/*
	 * For QD=1 devices, this is a special case. It's important for those
	 * to have one request ready when one completes, so force a depth of
	 * 2 for those devices. On the backend, it'll be a depth of 1 anyway,
	 * since the device can't have more than that in flight. If we're
	 * scaling down, then keep a setting of 1/1/1.
	 */
	if (rqd->queue_depth == 1) {
		if (rqd->scale_step > 0)
			rqd->max_depth = 1;
		else {
			rqd->max_depth = 2;
			ret = true;
		}
	} else {
		/*
		 * scale_step == 0 is our default state. If we have suffered
		 * latency spikes, step will be > 0, and we shrink the
		 * allowed write depths. If step is < 0, we're only doing
		 * writes, and we allow a temporarily higher depth to
		 * increase performance.
		 */
		depth = min_t(unsigned int, RWB_DEF_DEPTH, rqd->queue_depth);
		if (rqd->scale_step > 0)
			depth = 1 + ((depth - 1) >> min(31, rqd->scale_step));
		else if (rqd->scale_step < 0) {
			unsigned int maxd = 3 * rqd->queue_depth / 4;

			depth = 1 + ((depth - 1) << -rqd->scale_step);
			if (depth > maxd) {
				depth = maxd;
				ret = true;
			}
		}

		rqd->max_depth = depth;
	}

	return ret;
}

static inline bool stat_sample_valid(struct blk_rq_stat *stat)
{
	/*
	 * We need at least one read sample, and a minimum of
	 * RWB_MIN_WRITE_SAMPLES. We require some write samples to know
	 * that it's writes impacting us, and not just some sole read on
	 * a device that is in a lower power state.
	 */
	return (stat[READ].nr_samples >= 1 &&
		stat[WRITE].nr_samples >= RWB_MIN_WRITE_SAMPLES);
}

static u64 rwb_sync_issue_lat(struct rq_wb *rwb)
{
	u64 now, issue = READ_ONCE(rwb->sync_issue);

	if (!issue || !rwb->sync_cookie)
		return 0;

	now = ktime_to_ns(ktime_get());
	return now - issue;
}

enum {
	LAT_OK = 1,
	LAT_UNKNOWN,
	LAT_UNKNOWN_WRITES,
	LAT_EXCEEDED,
};

static int latency_exceeded(struct rq_wb *rwb, struct blk_rq_stat *stat)
{
	struct backing_dev_info *bdi = rwb->rqos.q->backing_dev_info;
	struct rq_depth *rqd = &rwb->rq_depth;
	u64 thislat;

	/*
	 * If our stored sync issue exceeds the window size, or it
	 * exceeds our min target AND we haven't logged any entries,
	 * flag the latency as exceeded. wbt works off completion latencies,
	 * but for a flooded device, a single sync IO can take a long time
	 * to complete after being issued. If this time exceeds our
	 * monitoring window AND we didn't see any other completions in that
	 * window, then count that sync IO as a violation of the latency.
	 */
	thislat = rwb_sync_issue_lat(rwb);
	if (thislat > rwb->cur_win_nsec ||
	    (thislat > rqd->min_lat_nsec && !stat[READ].nr_samples)) {
		trace_wbt_lat(bdi, thislat);
		return LAT_EXCEEDED;
	}

	/*
	 * No read/write mix, if stat isn't valid
	 */
	if (!stat_sample_valid(stat)) {
		/*
		 * If we had writes in this stat window and the window is
		 * current, we're only doing writes. If a task recently
		 * waited or still has writes in flights, consider us doing
		 * just writes as well.
		 */
		if (stat[WRITE].nr_samples || wb_recent_wait(rwb) ||
		    wbt_inflight(rwb))
			return LAT_UNKNOWN_WRITES;
		return LAT_UNKNOWN;
	}

	/*
	 * If the 'min' latency exceeds our target, step down.
	 */
	if (stat[READ].min > rqd->min_lat_nsec) {
		trace_wbt_lat(bdi, stat[READ].min);
		trace_wbt_stat(bdi, stat);
		return LAT_EXCEEDED;
	}

	if (rqd->scale_step)
		trace_wbt_stat(bdi, stat);

	return LAT_OK;
}

static void rwb_trace_step(struct rq_wb *rwb, const char *msg)
{
	struct backing_dev_info *bdi = rwb->rqos.q->backing_dev_info;
	struct rq_depth *rqd = &rwb->rq_depth;

	trace_wbt_step(bdi, msg, rqd->scale_step, rwb->cur_win_nsec,
			rwb->wb_background, rwb->wb_normal, rqd->max_depth);
}

static void rq_depth_scale_up(struct rq_depth *rqd)
{
	/*
	 * Hit max in previous round, stop here
	 */
	if (rqd->scaled_max)
		return;

	rqd->scale_step--;

	rqd->scaled_max = calc_max_depth(rqd);
}

/*
 * Scale rwb down. If 'hard_throttle' is set, do it quicker, since we
 * had a latency violation.
 */
static void rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle)
{
	/*
	 * Stop scaling down when we've hit the limit. This also prevents
	 * ->scale_step from going to crazy values, if the device can't
	 * keep up.
	 */
	if (rqd->max_depth == 1)
		return;

	if (rqd->scale_step < 0 && hard_throttle)
		rqd->scale_step = 0;
	else
		rqd->scale_step++;

	rqd->scaled_max = false;
	calc_max_depth(rqd);
}

static void calc_wb_limits(struct rq_wb *rwb)
{
	if (rwb->rq_depth.max_depth == 0) {
		rwb->wb_normal = rwb->wb_background = 0;
	} else if (rwb->rq_depth.max_depth <= 2) {
		rwb->wb_normal = rwb->rq_depth.max_depth;
		rwb->wb_background = 1;
	} else {
		rwb->wb_normal = (rwb->rq_depth.max_depth + 1) / 2;
		rwb->wb_background = (rwb->rq_depth.max_depth + 3) / 4;
	}
}

static void scale_up(struct rq_wb *rwb)
{
	rq_depth_scale_up(&rwb->rq_depth);
	calc_wb_limits(rwb);
	rwb->unknown_cnt = 0;
	rwb_trace_step(rwb, "scale up");
}

static void scale_down(struct rq_wb *rwb, bool hard_throttle)
{
	rq_depth_scale_down(&rwb->rq_depth, hard_throttle);
	calc_wb_limits(rwb);
	rwb->unknown_cnt = 0;
	rwb_wake_all(rwb);
	rwb_trace_step(rwb, "scale down");
}

static void rwb_arm_timer(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth;

	if (rqd->scale_step > 0) {
		/*
		 * We should speed this up, using some variant of a fast
		 * integer inverse square root calculation. Since we only do
		 * this for every window expiration, it's not a huge deal,
		 * though.
		 */
		rwb->cur_win_nsec = div_u64(rwb->win_nsec << 4,
					int_sqrt((rqd->scale_step + 1) << 8));
	} else {
		/*
		 * For step < 0, we don't want to increase/decrease the
		 * window size.
		 */
		rwb->cur_win_nsec = rwb->win_nsec;
	}

	blk_stat_activate_nsecs(rwb->cb, rwb->cur_win_nsec);
}

static void wb_timer_fn(struct blk_stat_callback *cb)
{
	struct rq_wb *rwb = cb->data;
	struct rq_depth *rqd = &rwb->rq_depth;
	unsigned int inflight = wbt_inflight(rwb);
	int status;

	status = latency_exceeded(rwb, cb->stat);

	trace_wbt_timer(rwb->rqos.q->backing_dev_info, status, rqd->scale_step,
			inflight);

	/*
	 * If we exceeded the latency target, step down. If we did not,
	 * step one level up. If we don't know enough to say either exceeded
	 * or ok, then don't do anything.
	 */
	switch (status) {
	case LAT_EXCEEDED:
		scale_down(rwb, true);
		break;
	case LAT_OK:
		scale_up(rwb);
		break;
	case LAT_UNKNOWN_WRITES:
		/*
		 * We started a the center step, but don't have a valid
		 * read/write sample, but we do have writes going on.
		 * Allow step to go negative, to increase write perf.
		 */
		scale_up(rwb);
		break;
	case LAT_UNKNOWN:
		if (++rwb->unknown_cnt < RWB_UNKNOWN_BUMP)
			break;
		/*
		 * We get here when previously scaled reduced depth, and we
		 * currently don't have a valid read/write sample. For that
		 * case, slowly return to center state (step == 0).
		 */
		if (rqd->scale_step > 0)
			scale_up(rwb);
		else if (rqd->scale_step < 0)
			scale_down(rwb, false);
		break;
	default:
		break;
	}

	/*
	 * Re-arm timer, if we have IO in flight
	 */
	if (rqd->scale_step || inflight)
		rwb_arm_timer(rwb);
}

static void __wbt_update_limits(struct rq_wb *rwb)
{
	struct rq_depth *rqd = &rwb->rq_depth;

	rqd->scale_step = 0;
	rqd->scaled_max = false;

	calc_max_depth(rqd);
	calc_wb_limits(rwb);

	rwb_wake_all(rwb);
}

void wbt_update_limits(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	if (!rqos)
		return;
	__wbt_update_limits(RQWB(rqos));
}

static bool close_io(struct rq_wb *rwb)
{
	const unsigned long now = jiffies;

	return time_before(now, rwb->last_issue + HZ / 10) ||
		time_before(now, rwb->last_comp + HZ / 10);
}

#define REQ_HIPRIO	(REQ_SYNC | REQ_META | REQ_PRIO)

static inline unsigned int get_limit(struct rq_wb *rwb, unsigned long rw)
{
	unsigned int limit;

	/*
	 * At this point we know it's a buffered write. If this is
	 * kswapd trying to free memory, or REQ_SYNC is set, then
	 * it's WB_SYNC_ALL writeback, and we'll use the max limit for
	 * that. If the write is marked as a background write, then use
	 * the idle limit, or go to normal if we haven't had competing
	 * IO for a bit.
	 */
	if ((rw & REQ_HIPRIO) || wb_recent_wait(rwb) || current_is_kswapd())
		limit = rwb->rq_depth.max_depth;
	else if ((rw & REQ_BACKGROUND) || close_io(rwb)) {
		/*
		 * If less than 100ms since we completed unrelated IO,
		 * limit us to half the depth for background writeback.
		 */
		limit = rwb->wb_background;
	} else
		limit = rwb->wb_normal;

	return limit;
}

static inline bool may_queue(struct rq_wb *rwb, struct rq_wait *rqw,
			     wait_queue_entry_t *wait, unsigned long rw)
{
	/*
	 * inc it here even if disabled, since we'll dec it at completion.
	 * this only happens if the task was sleeping in __wbt_wait(),
	 * and someone turned it off at the same time.
	 */
	if (!rwb_enabled(rwb)) {
		atomic_inc(&rqw->inflight);
		return true;
	}

	/*
	 * If the waitqueue is already active and we are not the next
	 * in line to be woken up, wait for our turn.
	 */
	if (waitqueue_active(&rqw->wait) &&
	    rqw->wait.head.next != &wait->entry)
		return false;

	return atomic_inc_below(&rqw->inflight, get_limit(rwb, rw));
}

/*
 * Block if we will exceed our limit, or if we are currently waiting for
 * the timer to kick off queuing again.
 */
static void __wbt_wait(struct rq_wb *rwb, unsigned long rw, spinlock_t *lock)
	__releases(lock)
	__acquires(lock)
{
	struct rq_wait *rqw = get_rq_wait(rwb, current_is_kswapd());
	DEFINE_WAIT(wait);

	if (may_queue(rwb, rqw, &wait, rw))
		return;

	do {
		prepare_to_wait_exclusive(&rqw->wait, &wait,
						TASK_UNINTERRUPTIBLE);

		if (may_queue(rwb, rqw, &wait, rw))
			break;

		if (lock) {
			spin_unlock_irq(lock);
			io_schedule();
			spin_lock_irq(lock);
		} else
			io_schedule();
	} while (1);

	finish_wait(&rqw->wait, &wait);
}

static inline bool wbt_should_throttle(struct rq_wb *rwb, struct bio *bio)
{
	const int op = bio_op(bio);

	/*
	 * If not a WRITE, do nothing
	 */
	if (op != REQ_OP_WRITE)
		return false;

	/*
	 * Don't throttle WRITE_ODIRECT
	 */
	if ((bio->bi_opf & (REQ_SYNC | REQ_IDLE)) == (REQ_SYNC | REQ_IDLE))
		return false;

	return true;
}

/*
 * Returns true if the IO request should be accounted, false if not.
 * May sleep, if we have exceeded the writeback limits. Caller can pass
 * in an irq held spinlock, if it holds one when calling this function.
 * If we do sleep, we'll release and re-grab it.
 */
static enum wbt_flags wbt_wait(struct rq_qos *rqos, struct bio *bio,
			       spinlock_t *lock)
{
	struct rq_wb *rwb = RQWB(rqos);
	unsigned int ret = 0;

	if (!rwb_enabled(rwb))
		return 0;

	if (bio_op(bio) == REQ_OP_READ)
		ret = WBT_READ;

	if (!wbt_should_throttle(rwb, bio)) {
		if (ret & WBT_READ)
			wb_timestamp(rwb, &rwb->last_issue);
		return ret;
	}

	__wbt_wait(rwb, bio->bi_opf, lock);

	if (!blk_stat_is_active(rwb->cb))
		rwb_arm_timer(rwb);

	if (current_is_kswapd())
		ret |= WBT_KSWAPD;

	return ret | WBT_TRACKED;
}

void wbt_issue(struct rq_qos *rqos, struct blk_issue_stat *stat)
{
	struct rq_wb *rwb = RQWB(rqos);

	if (!rwb_enabled(rwb))
		return;

	/*
	 * Track sync issue, in case it takes a long time to complete. Allows
	 * us to react quicker, if a sync IO takes a long time to complete.
	 * Note that this is just a hint. 'stat' can go away when the
	 * request completes, so it's important we never dereference it. We
	 * only use the address to compare with, which is why we store the
	 * sync_issue time locally.
	 */
	if (wbt_is_read(stat) && !rwb->sync_issue) {
		rwb->sync_cookie = stat;
		rwb->sync_issue = blk_stat_time(stat);
	}
}

void wbt_requeue(struct rq_qos *rqos, struct blk_issue_stat *stat)
{
	struct rq_wb *rwb = RQWB(rqos);
	if (!rwb_enabled(rwb))
		return;
	if (stat == rwb->sync_cookie) {
		rwb->sync_issue = 0;
		rwb->sync_cookie = NULL;
	}
}

void wbt_set_queue_depth(struct request_queue *q, unsigned int depth)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	if (rqos) {
		RQWB(rqos)->rq_depth.queue_depth = depth;
		__wbt_update_limits(RQWB(rqos));
	}
}

void wbt_set_write_cache(struct request_queue *q, bool write_cache_on)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	if (rqos)
		RQWB(rqos)->wc = write_cache_on;
}

/*
 * Enable wbt if defaults are configured that way
 */
void wbt_enable_default(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	/* Throttling already enabled? */
	if (rqos)
		return;

	/* Queue not registered? Maybe shutting down... */
	if (!test_bit(QUEUE_FLAG_REGISTERED, &q->queue_flags))
		return;

	if ((q->mq_ops && IS_ENABLED(CONFIG_BLK_WBT_MQ)) ||
	    (q->request_fn && IS_ENABLED(CONFIG_BLK_WBT_SQ)))
		wbt_init(q);
}
EXPORT_SYMBOL_GPL(wbt_enable_default);

u64 wbt_default_latency_nsec(struct request_queue *q)
{
	/*
	 * We default to 2msec for non-rotational storage, and 75msec
	 * for rotational storage.
	 */
	if (blk_queue_nonrot(q))
		return 2000000ULL;
	else
		return 75000000ULL;
}

static int wbt_data_dir(const struct request *rq)
{
	return rq_data_dir(rq);
}

static void wbt_exit(struct rq_qos *rqos)
{
	struct rq_wb *rwb = RQWB(rqos);
	struct request_queue *q = rqos->q;

	blk_stat_remove_callback(q, rwb->cb);
	blk_stat_free_callback(rwb->cb);
	kfree(rwb);
}

/*
 * Disable wbt, if enabled by default.
 */
void wbt_disable_default(struct request_queue *q)
{
	struct rq_qos *rqos = wbt_rq_qos(q);
	struct rq_wb *rwb;
	if (!rqos)
		return;
	rwb = RQWB(rqos);
	if (rwb->enable_state == WBT_STATE_ON_DEFAULT)
		wbt_exit(rqos);
}
EXPORT_SYMBOL_GPL(wbt_disable_default);


static struct rq_qos_ops wbt_rqos_ops = {
	.throttle = wbt_wait,
	.issue = wbt_issue,
	.requeue = wbt_requeue,
	.done = wbt_done,
	.cleanup = __wbt_done,
	.exit = wbt_exit,
};

int wbt_init(struct request_queue *q)
{
	struct rq_wb *rwb;
	int i;

	BUILD_BUG_ON(WBT_NR_BITS > BLK_STAT_RES_BITS);

	rwb = kzalloc(sizeof(*rwb), GFP_KERNEL);
	if (!rwb)
		return -ENOMEM;

	rwb->cb = blk_stat_alloc_callback(wb_timer_fn, wbt_data_dir, 2, rwb);
	if (!rwb->cb) {
		kfree(rwb);
		return -ENOMEM;
	}

	for (i = 0; i < WBT_NUM_RWQ; i++)
		rq_wait_init(&rwb->rq_wait[i]);

	rwb->rqos.id = RQ_QOS_WBT;
	rwb->rqos.ops = &wbt_rqos_ops;
	rwb->rqos.q = q;
	rwb->last_comp = rwb->last_issue = jiffies;
	rwb->win_nsec = RWB_WINDOW_NSEC;
	rwb->enable_state = WBT_STATE_ON_DEFAULT;
	__wbt_update_limits(rwb);

	/*
	 * Assign rwb and add the stats callback.
	 */
	rq_qos_add(q, &rwb->rqos);
	blk_stat_add_callback(q, rwb->cb);

	rwb->rq_depth.min_lat_nsec = wbt_default_latency_nsec(q);

	wbt_set_queue_depth(q, blk_queue_depth(q));
	wbt_set_write_cache(q, test_bit(QUEUE_FLAG_WC, &q->queue_flags));

	return 0;
}

void rq_qos_exit(struct request_queue *q)
{
	while (q->rq_qos) {
		struct rq_qos *rqos = q->rq_qos;
		q->rq_qos = rqos->next;
		rqos->ops->exit(rqos);
	}
}

#define DEFAULT_SCALE_COOKIE 100000U

static struct blkcg_policy blkcg_policy_qos;

struct blkcg_qos {
	struct rq_qos rqos;
	spinlock_t scale_lock;
	atomic_t enabled;
	atomic_t scale_cookie;
	u64 scale_lat;
	u64 last_scale_event;
};

static inline struct blkcg_qos *BLKQOS(struct rq_qos *rqos)
{
	return container_of(rqos, struct blkcg_qos, rqos);
}

static inline bool blkcg_qos_enabled(struct blkcg_qos *blkqos)
{
	return atomic_read(&blkqos->enabled) > 0;
}

struct qos_grp {
	struct blk_rq_stat __percpu *stats;
	struct blkcg_qos *blkqos;
	struct blkg_policy_data pd;
	struct rq_depth rq_depth;
	struct rq_wait rq_wait;
	atomic64_t window_start;
	atomic_t scale_cookie;
	u64 set_min_lat_nsec;
	u64 cur_win_nsec;
};

static inline struct qos_grp *pd_to_qg(struct blkg_policy_data *pd)
{
	return container_of(pd, struct qos_grp, pd);
}

static inline struct qos_grp *blkg_to_qg(struct blkcg_gq *blkg)
{
	return pd_to_qg(blkg_to_pd(blkg, &blkcg_policy_qos));
}

static inline struct blkcg_gq *qg_to_blkg(struct qos_grp *qg)
{
	return pd_to_blkg(&qg->pd);
}

static inline bool qg_may_queue(struct qos_grp *qg,
				wait_queue_entry_t *wait)
{
	struct rq_wait *rqw = &qg->rq_wait;

	if (waitqueue_active(&rqw->wait) &&
	    rqw->wait.head.next != &wait->entry)
		return false;
	return atomic_inc_below(&rqw->inflight, qg->rq_depth.max_depth);
}

static void __blkcg_qos_throttle(struct rq_qos *rqos, struct qos_grp *qg,
				 spinlock_t *lock)
	__releases(lock)
	__acquires(lock)
{
	struct rq_wait *rqw = &qg->rq_wait;
	DEFINE_WAIT(wait);

	if (qg_may_queue(qg, &wait))
		return;

	do {
		prepare_to_wait_exclusive(&rqw->wait, &wait,
					  TASK_UNINTERRUPTIBLE);
		if (qg_may_queue(qg, &wait))
			break;

		if (lock) {
			spin_unlock_irq(lock);
			io_schedule();
			spin_lock_irq(lock);
		} else {
			io_schedule();
		}
	} while (1);

	finish_wait(&rqw->wait, &wait);
}

static void check_scale_change(struct blkcg_qos *blkcg_qos, struct qos_grp *qg)
{
	unsigned int cur_cookie = atomic_read(&blkcg_qos->scale_cookie);
	unsigned int our_cookie = atomic_read(&qg->scale_cookie);
	unsigned int scale_lat = READ_ONCE(blkcg_qos->scale_lat);
	unsigned int old;
	int direction = 0;

	if (cur_cookie < our_cookie)
		direction = -1;
	else if (cur_cookie > our_cookie)
		direction = 1;
	else
		return;

	old = atomic_cmpxchg(&qg->scale_cookie, our_cookie, cur_cookie);

	/* Somebody beat us to the punch, just bail. */
	if (old != our_cookie)
		return;

	/*
	 * If the missed target was a higher requirement than ours we don't have
	 * to do anything.
	 */
	if (scale_lat >= qg->rq_depth.min_lat_nsec)
		return;

	if (direction < 0) {
		rq_depth_scale_down(&qg->rq_depth, false);
	} else {
		rq_depth_scale_up(&qg->rq_depth);
		wake_up_all(&qg->rq_wait.wait);
	}

	if (cur_cookie == DEFAULT_SCALE_COOKIE)
		qg->rq_depth.max_depth = INT_MAX;
}

static enum wbt_flags blkcg_qos_throttle(struct rq_qos *rqos, struct bio *bio,
					 spinlock_t *lock)
{
	struct blkcg_qos *blkqos = BLKQOS(rqos);
	struct blkcg *blkcg;
	struct blkcg_gq *blkg;
	struct qos_grp *qg;
	struct request_queue *q = rqos->q;
	bool throttle = false;

	if (!blkcg_qos_enabled(blkqos))
		return 0;

	rcu_read_lock();
	blkcg = bio_blkcg(bio);
	bio_associate_blkcg(bio, &blkcg->css);
	blkg = blkg_lookup(blkcg, q);
	if (unlikely(!blkg)) {
		if (!lock)
			spin_lock_irq(q->queue_lock);
		blkg = blkg_lookup_create(blkcg, q);
		if (IS_ERR(blkg))
			blkg = NULL;
		if (!lock)
			spin_unlock_irq(q->queue_lock);
	}
	if (!blkg)
		goto out;

	bio_associate_blkg(bio, blkg);
	qg = blkg_to_qg(blkg);
	check_scale_change(blkqos, qg);
	if (!atomic_inc_below(&qg->rq_wait.inflight,
			      qg->rq_depth.max_depth))
		throttle = true;
out:
	rcu_read_unlock();
	if (throttle)
		__blkcg_qos_throttle(rqos, qg, lock);
	if (blkg)
		blk_stat_set_issue(&bio->bi_issue_stat, bio_sectors(bio));
	return 0;
}

static void qos_record_time(struct qos_grp *qg, struct blk_issue_stat *stat,
			    u64 now)
{
	struct blk_rq_stat *rq_stat;
	u64 start = blk_stat_time(stat);

	if (now <= start)
		return;
	rq_stat = get_cpu_ptr(qg->stats);
	blk_rq_stat_add(rq_stat, now - start);
	put_cpu_ptr(rq_stat);
}

#define BLKCG_QOS_MIN_SAMPLES 4
#define BLKQOS_MIN_ADJUST_TIME (5 * NSEC_PER_MSEC)

static void qos_check_latencies(struct qos_grp *qg, u64 now)
{
	struct blkcg_qos *blkqos = qg->blkqos;
	struct blk_rq_stat stat;
	unsigned cookie = atomic_read(&blkqos->scale_cookie);
	int cpu;

	blk_rq_stat_init(&stat);
	preempt_disable();
	for_each_online_cpu(cpu) {
		struct blk_rq_stat *s;
		s = per_cpu_ptr(qg->stats, cpu);
		blk_rq_stat_sum(&stat, s);
		blk_rq_stat_init(s);
	}
	preempt_enable();

	if (stat.nr_samples < BLKCG_QOS_MIN_SAMPLES)
		return;

	/* Everything is ok and we don't need to adjust the scale. */
	if (stat.min <= qg->rq_depth.min_lat_nsec &&
	    cookie == DEFAULT_SCALE_COOKIE)
		return;

	if (blkqos->last_scale_event >= now)
		return;

	if (now - blkqos->last_scale_event < BLKQOS_MIN_ADJUST_TIME)
		return;

	spin_lock(&blkqos->scale_lock);
	if (blkqos->last_scale_event >= now ||
	    now - blkqos->last_scale_event < BLKQOS_MIN_ADJUST_TIME)
		goto out;

	if (stat.min <= qg->rq_depth.min_lat_nsec) {
		if (blkqos->scale_lat == qg->rq_depth.min_lat_nsec)
			atomic_inc(&qg->scale_cookie);
	} else {
		blkqos->scale_lat = qg->rq_depth.min_lat_nsec;
		smp_mb__before_atomic();
		atomic_dec(&blkqos->scale_cookie);
	}
out:
	spin_unlock(&blkqos->scale_lock);
}

static void blkcg_qos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct qos_grp *qg;
	u64 window_start;
	u64 now;

	blkg = bio->bi_blkg;
	if (!blkg)
		return;
	qg = blkg_to_qg(blkg);

	if (!blkcg_qos_enabled(qg->blkqos))
		return;

	/* Record the time for our bio. */
	now = ktime_to_ns(ktime_get());
	qos_record_time(qg, &bio->bi_issue_stat, now);

	/*
	 * Now check to see if enough time has elapsed, and if it has check to
	 * see if we've missed our latency targets.
	 */
	window_start = atomic64_read(&qg->window_start);
	if (now > window_start && (now - window_start) >= qg->cur_win_nsec) {
		if (atomic64_cmpxchg(&qg->window_start, window_start, now) ==
		    window_start)
			qos_check_latencies(qg, now);
	}
}

static struct rq_qos_ops blkcg_qos_ops = {
	.throttle = blkcg_qos_throttle,
	.done_bio = blkcg_qos_done_bio,
};

int blkcg_qos_init(struct request_queue *q)
{
	struct blkcg_qos *blkqos;
	struct rq_qos *rqos;
	int ret;

	BUILD_BUG_ON(WBT_NR_BITS > BLK_STAT_RES_BITS);

	blkqos = kzalloc(sizeof(*blkqos), GFP_KERNEL);
	if (!blkqos)
		return -ENOMEM;

	rqos = &blkqos->rqos;
	rqos->id = RQ_QOS_CGROUP;
	rqos->ops = &blkcg_qos_ops;
	rqos->q = q;

	rq_qos_add(q, rqos);
	atomic_set(&blkqos->scale_cookie, DEFAULT_SCALE_COOKIE);
	blkqos->scale_lat = 0;

	ret = blkcg_activate_policy(q, &blkcg_policy_qos);
	if (ret) {
		kfree(blkqos);
		return ret;
	}

	return 0;
}

static void qos_set_min_lat_nsec(struct blkcg_gq *blkg, int direction)
{
	struct qos_grp *qg = blkg_to_qg(blkg);
	struct blkcg_qos *blkqos = qg->blkqos;
	u64 val = qg->set_min_lat_nsec;
	u64 oldval = qg->rq_depth.min_lat_nsec;

	while (blkg->parent) {
		struct qos_grp *this_qg = blkg_to_qg(blkg->parent);
		val = max(val, this_qg->set_min_lat_nsec);
		blkg = blkg->parent;
	}
	qg->rq_depth.min_lat_nsec = val;
	if (direction > 0 && !oldval && val)
		atomic_inc(&blkqos->enabled);
	if (direction < 0 && oldval && !val)
		atomic_dec(&blkqos->enabled);
}

static ssize_t qos_set_limit(struct kernfs_open_file *of, char *buf,
			     size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkcg_gq *blkg;
	struct cgroup_subsys_state *pos_css;
	struct blkcg_qos *blkqos;
	struct blkg_conf_ctx ctx;
	struct qos_grp *qg;
	char tok[29];	/* latency=18446744073709551616 */
	char *p;
	u64 val;
	int len, ret;

	ret = blkg_conf_prep(blkcg, &blkcg_policy_qos, buf, &ctx);
	if (ret)
		return ret;

	qg = blkg_to_qg(ctx.blkg);
	blkqos = qg->blkqos;
	if (sscanf(ctx.body,"%28s%n", tok, &len) != 1)
		goto out;
	if (tok[0] == '\0')
		goto out;

	ret = -EINVAL;
	p = tok;
	strsep(&p, "=");
	if (!p || sscanf(p, "%llu", &val) != 1)
		goto out;
	if (strcmp(tok, "latency"))
		goto out;
	qg->set_min_lat_nsec = val;

	/* Walk up the tree to see if our new val is lower than it should be. */
	blkg = ctx.blkg;
	qos_set_min_lat_nsec(blkg, 1);

	blkg_for_each_descendant_pre(blkg, pos_css, ctx.blkg)
		qos_set_min_lat_nsec(blkg, 1);
	ret = 0;
out:
	blkg_conf_finish(&ctx);
	return ret ?: nbytes;
}

static u64 qg_prfill_limit(struct seq_file *sf, struct blkg_policy_data *pd,
			   int off)
{
	struct qos_grp *qg = pd_to_qg(pd);
	const char *dname = blkg_dev_name(pd->blkg);

	if (!dname)
		return 0;
	seq_printf(sf, "%s latency=%llu\n",
		   dname, (unsigned long long)qg->set_min_lat_nsec);
	return 0;
}

static int qos_print_limit(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), qg_prfill_limit,
			  &blkcg_policy_qos, seq_cft(sf)->private, false);
	return 0;
}

static struct blkg_policy_data *qos_pd_alloc(gfp_t gfp, int node)
{
	struct qos_grp *qg;

	qg = kzalloc_node(sizeof(*qg), gfp, node);
	if (!qg)
		return NULL;
	qg->stats = __alloc_percpu_gfp(sizeof(struct blk_rq_stat),
				       __alignof__(struct blk_rq_stat), gfp);
	if (!qg->stats) {
		kfree(qg);
		return NULL;
	}
	return &qg->pd;
}

static void qos_pd_init(struct blkg_policy_data *pd)
{
	struct qos_grp *qg = pd_to_qg(pd);
	struct blkcg_gq *blkg = qg_to_blkg(qg);
	struct rq_qos *rqos = blkcg_rq_qos(blkg->q);
	struct blkcg_qos *blkqos = BLKQOS(rqos);
	u64 now = ktime_to_ns(ktime_get());

	rq_wait_init(&qg->rq_wait);
	qg->rq_depth.queue_depth = blk_queue_depth(blkg->q);
	qg->rq_depth.max_depth = INT_MAX;
	qg->blkqos = blkqos;
	atomic64_set(&qg->window_start, now);
	atomic_set(&qg->scale_cookie, atomic_read(&blkqos->scale_cookie));
	qg->set_min_lat_nsec = 0;

	qos_set_min_lat_nsec(blkg, 1);
}

static void qos_pd_offline(struct blkg_policy_data *pd)
{
	struct qos_grp *qg = pd_to_qg(pd);
	struct blkcg_gq *blkg = qg_to_blkg(qg);
	struct cgroup_subsys_state *pos_css;

	qg->set_min_lat_nsec = 0;
	qos_set_min_lat_nsec(blkg, -1);

	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css, qg_to_blkg(qg))
		qos_set_min_lat_nsec(blkg, -1);
	rcu_read_unlock();
}

static void qos_pd_free(struct blkg_policy_data *pd)
{
	struct qos_grp *qg = pd_to_qg(pd);
	free_percpu(qg->stats);
	kfree(qg);
}

static struct cftype qos_files[] = {
	{
		.name = "qos",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = qos_print_limit,
		.write = qos_set_limit,
	},
	{}
};

static struct blkcg_policy blkcg_policy_qos = {
	.dfl_cftypes	= qos_files,
	.pd_alloc_fn	= qos_pd_alloc,
	.pd_init_fn	= qos_pd_init,
	.pd_offline_fn	= qos_pd_offline,
	.pd_free_fn	= qos_pd_free,
};

static int __init qos_init(void)
{
	return blkcg_policy_register(&blkcg_policy_qos);
}

module_init(qos_init);
