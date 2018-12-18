/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_STAT_H
#define BLK_STAT_H

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/ktime.h>
#include <linux/rcupdate.h>
#include <linux/timer.h>

/**
 * struct blk_stat_callback - Block statistics callback.
 *
 * A &struct blk_stat_callback is associated with a &struct request_queue. While
 * @timer is active, that queue's request completion latencies are sorted into
 * buckets by @bucket_fn and added to a per-cpu buffer, @cpu_stat. When the
 * timer fires, @cpu_stat is flushed to @stat and @timer_fn is invoked.
 */
struct blk_stat_callback {
	/*
	 * @list: RCU list of callbacks for a &struct request_queue.
	 */
	struct list_head list;

	/**
	 * @timer: Timer for the next callback invocation.
	 */
	struct timer_list timer;

	/**
	 * @cpu_stat: Per-cpu statistics buckets.
	 */
	struct blk_rq_stat __percpu *cpu_stat;

	/**
	 * @bucket_fn: Given a request, returns which statistics bucket it
	 * should be accounted under. Return -1 for no bucket for this
	 * request.
	 */
	int (*bucket_fn)(const struct request *);

	/**
	 * @buckets: Number of statistics buckets.
	 */
	unsigned int buckets;

	/**
	 * @stat: Array of statistics buckets.
	 */
	struct blk_rq_stat *stat;

	/**
	 * @fn: Callback function.
	 */
	void (*timer_fn)(struct blk_stat_callback *);

	/**
	 * @data: Private pointer for the user.
	 */
	void *data;

	struct rcu_head rcu;
};

struct blk_queue_stats *blk_alloc_queue_stats(void);
void blk_free_queue_stats(struct blk_queue_stats *);

void blk_stat_add(struct request *rq, u64 now);

/* record time/size info in request but not add a callback */
void blk_stat_enable_accounting(struct request_queue *q);

/**
 * blk_stat_alloc_callback() - Allocate a block statistics callback.
 * @timer_fn: Timer callback function.
 * @bucket_fn: Bucket callback function.
 * @buckets: Number of statistics buckets.
 * @data: Value for the @data field of the &struct blk_stat_callback.
 *
 * See &struct blk_stat_callback for details on the callback functions.
 *
 * Return: &struct blk_stat_callback on success or NULL on ENOMEM.
 */
struct blk_stat_callback *
blk_stat_alloc_callback(void (*timer_fn)(struct blk_stat_callback *),
			int (*bucket_fn)(const struct request *),
			unsigned int buckets, void *data);

/**
 * blk_stat_add_callback() - Add a block statistics callback to be run on a
 * request queue.
 * @q: The request queue.
 * @cb: The callback.
 *
 * Note that a single &struct blk_stat_callback can only be added to a single
 * &struct request_queue.
 */
void blk_stat_add_callback(struct request_queue *q,
			   struct blk_stat_callback *cb);

/**
 * blk_stat_remove_callback() - Remove a block statistics callback from a
 * request queue.
 * @q: The request queue.
 * @cb: The callback.
 *
 * When this returns, the callback is not running on any CPUs and will not be
 * called again unless readded.
 */
void blk_stat_remove_callback(struct request_queue *q,
			      struct blk_stat_callback *cb);

/**
 * blk_stat_free_callback() - Free a block statistics callback.
 * @cb: The callback.
 *
 * @cb may be NULL, in which case this does nothing. If it is not NULL, @cb must
 * not be associated with a request queue. I.e., if it was previously added with
 * blk_stat_add_callback(), it must also have been removed since then with
 * blk_stat_remove_callback().
 */
void blk_stat_free_callback(struct blk_stat_callback *cb);

/**
 * blk_stat_is_active() - Check if a block statistics callback is currently
 * gathering statistics.
 * @cb: The callback.
 */
static inline bool blk_stat_is_active(struct blk_stat_callback *cb)
{
	return timer_pending(&cb->timer);
}

/**
 * blk_stat_activate_nsecs() - Gather block statistics during a time window in
 * nanoseconds.
 * @cb: The callback.
 * @nsecs: Number of nanoseconds to gather statistics for.
 *
 * The timer callback will be called when the window expires.
 */
static inline void blk_stat_activate_nsecs(struct blk_stat_callback *cb,
					   u64 nsecs)
{
	mod_timer(&cb->timer, jiffies + nsecs_to_jiffies(nsecs));
}

static inline void blk_stat_deactivate(struct blk_stat_callback *cb)
{
	del_timer_sync(&cb->timer);
}

/**
 * blk_stat_activate_msecs() - Gather block statistics during a time window in
 * milliseconds.
 * @cb: The callback.
 * @msecs: Number of milliseconds to gather statistics for.
 *
 * The timer callback will be called when the window expires.
 */
static inline void blk_stat_activate_msecs(struct blk_stat_callback *cb,
					   unsigned int msecs)
{
	mod_timer(&cb->timer, jiffies + msecs_to_jiffies(msecs));
}

/**
 * blk_stat_bw() - Calculate the bw for the given stat in bytes/sec
 * @stat -The blk_rq_stat to calculate the bw for.
 *
 * This will calculate the bandwidth for this blk_rq_stat in bytes/sec, doing
 * the appropriate time scaling as the time measurements are in ns.
 */
static inline u64 blk_stat_bw(struct blk_rq_stat *stat)
{
	u64 time = stat->end_ns - stat->start_ns;
	u64 size = stat->size;
	u64 bw;
	u64 multiplier = NSEC_PER_SEC;

	/*
	 * We want bytes/sec, but we have bytes/nsec.  To get bytes/sec we need
	 * to do (bytes/nsec)(NSECS_PER_SEC), however if time > bytes we'll just
	 * get 0, so we need to scale down time until we can divide safely.
	 * Then we can multiply by the remaining part of NSECS_PER_SEC to get
	 * our bytes/sec.
	 */
	while (size < time) {
		time = div64_u64(time, 100);
		multiplier = div64_u64(time, 100);
	}

	if (multiplier)
		bw *= multiplier;
	bw = div64_u64(size, time);
	return bw;
}

void blk_rq_stat_add(struct blk_rq_stat *, u64, u64, u64);
void blk_rq_stat_sum(struct blk_rq_stat *, struct blk_rq_stat *);
void blk_rq_stat_init(struct blk_rq_stat *);

#endif
