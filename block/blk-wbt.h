/* SPDX-License-Identifier: GPL-2.0 */
#ifndef WB_THROTTLE_H
#define WB_THROTTLE_H

#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/ktime.h>

#include "blk-stat.h"

enum rq_qos_id {
	RQ_QOS_WBT,
	RQ_QOS_CGROUP,
};

enum wbt_flags {
	WBT_TRACKED		= 1,	/* write, tracked for throttling */
	WBT_READ		= 2,	/* read */
	WBT_KSWAPD		= 4,	/* write, from kswapd */

	WBT_NR_BITS		= 3,	/* number of bits */
};

enum {
	WBT_NUM_RWQ		= 2,
};

/*
 * Enable states. Either off, or on by default (done at init time),
 * or on through manual setup in sysfs.
 */
enum {
	WBT_STATE_ON_DEFAULT	= 1,
	WBT_STATE_ON_MANUAL	= 2,
};

enum {
	/*
	 * Default setting, we'll scale up (to 75% of QD max) or down (min 1)
	 * from here depending on device stats
	 */
	RWB_DEF_DEPTH	= 16,

	/*
	 * 100msec window
	 */
	RWB_WINDOW_NSEC		= 100 * 1000 * 1000ULL,

	/*
	 * Disregard stats, if we don't meet this minimum
	 */
	RWB_MIN_WRITE_SAMPLES	= 3,

	/*
	 * If we have this number of consecutive windows with not enough
	 * information to scale up or down, scale up.
	 */
	RWB_UNKNOWN_BUMP	= 5,
};

static inline void wbt_clear_state(struct blk_issue_stat *stat)
{
	stat->stat &= ~BLK_STAT_RES_MASK;
}

static inline enum wbt_flags wbt_stat_to_mask(struct blk_issue_stat *stat)
{
	return (stat->stat & BLK_STAT_RES_MASK) >> BLK_STAT_RES_SHIFT;
}

static inline void wbt_track(struct blk_issue_stat *stat, enum wbt_flags wb_acct)
{
	stat->stat |= ((u64) wb_acct) << BLK_STAT_RES_SHIFT;
}

static inline bool wbt_is_tracked(struct blk_issue_stat *stat)
{
	return (stat->stat >> BLK_STAT_RES_SHIFT) & WBT_TRACKED;
}

static inline bool wbt_is_read(struct blk_issue_stat *stat)
{
	return (stat->stat >> BLK_STAT_RES_SHIFT) & WBT_READ;
}

struct rq_wait {
	wait_queue_head_t wait;
	atomic_t inflight;
};

struct rq_qos {
	struct rq_qos_ops *ops;
	struct request_queue *q;
	enum rq_qos_id id;
	struct rq_qos *next;
};

struct rq_qos_ops {
	enum wbt_flags (*throttle)(struct rq_qos *, struct bio *,
				   spinlock_t *);
	void (*issue)(struct rq_qos *, struct blk_issue_stat *);
	void (*requeue)(struct rq_qos *, struct blk_issue_stat *);
	void (*done)(struct rq_qos *, struct blk_issue_stat *);
	void (*done_bio)(struct rq_qos *, struct bio *);
	void (*cleanup)(struct rq_qos *, enum wbt_flags);
	void (*exit)(struct rq_qos *);
};

struct rq_depth {
	unsigned int max_depth;

	int scale_step;
	bool scaled_max;

	unsigned int queue_depth;

	unsigned long min_lat_nsec;
};

struct rq_wb {
	struct blk_stat_callback *cb;

	/*
	 * Settings that govern how we throttle
	 */
	unsigned int wb_background;		/* background writeback */
	unsigned int wb_normal;			/* normal writeback */

	unsigned long last_issue;		/* last non-throttled issue */
	unsigned long last_comp;		/* last non-throttled comp */

	/*
	 * Number of consecutive periods where we don't have enough
	 * information to make a firm scale up/down decision.
	 */
	unsigned int unknown_cnt;
	u64 win_nsec;				/* default window size */
	u64 cur_win_nsec;			/* current window size */

	short enable_state;			/* WBT_STATE_* */

	s64 sync_issue;
	void *sync_cookie;

	unsigned int wc;
	struct rq_qos rqos;
	struct rq_wait rq_wait[WBT_NUM_RWQ];
	struct rq_depth rq_depth;
};

static inline struct rq_wb *RQWB(struct rq_qos *rqos)
{
	return container_of(rqos, struct rq_wb, rqos);
}

static inline struct rq_qos *rq_qos_id(struct request_queue *q,
				       enum rq_qos_id id)
{
	struct rq_qos *rqos;
	for (rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (rqos->id == id)
			break;
	}
	return rqos;
}

static inline struct rq_qos *wbt_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_WBT);
}

static inline struct rq_qos *blkcg_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_CGROUP);
}

static inline unsigned int wbt_inflight(struct rq_wb *rwb)
{
	unsigned int i, ret = 0;

	for (i = 0; i < WBT_NUM_RWQ; i++)
		ret += atomic_read(&rwb->rq_wait[i].inflight);

	return ret;
}

static inline void rq_wait_init(struct rq_wait *rq_wait)
{
	atomic_set(&rq_wait->inflight, 0);
	init_waitqueue_head(&rq_wait->wait);
}

static inline void rq_qos_add(struct request_queue *q, struct rq_qos *rqos)
{
	rqos->next = q->rq_qos;
	q->rq_qos = rqos;
}

int blkcg_qos_init(struct request_queue *q);
#ifdef CONFIG_BLK_WBT

void rq_qos_cleanup(struct request_queue *, enum wbt_flags);
void rq_qos_done(struct request_queue *, struct blk_issue_stat *);
void rq_qos_issue(struct request_queue *, struct blk_issue_stat *);
void rq_qos_requeue(struct request_queue *, struct blk_issue_stat *);
void rq_qos_done_bio(struct request_queue *q, struct bio *bio);
enum wbt_flags rq_qos_throttle(struct request_queue *, struct bio *, spinlock_t *);
void rq_qos_exit(struct request_queue *);

int wbt_init(struct request_queue *);
void wbt_update_limits(struct request_queue *);
void wbt_disable_default(struct request_queue *);
void wbt_enable_default(struct request_queue *);

u64 wbt_get_min_lat(struct request_queue *q);
void wbt_set_min_lat(struct request_queue *q, u64 val);

void wbt_set_queue_depth(struct request_queue *, unsigned int);
void wbt_set_write_cache(struct request_queue *, bool);

u64 wbt_default_latency_nsec(struct request_queue *);

#else

static inline void __wbt_done(struct rq_wb *rwb, enum wbt_flags flags)
{
}
static inline void wbt_done(struct rq_wb *rwb, struct blk_issue_stat *stat)
{
}
static inline enum wbt_flags wbt_wait(struct rq_wb *rwb, struct bio *bio,
				      spinlock_t *lock)
{
	return 0;
}
static inline int wbt_init(struct request_queue *q)
{
	return -EINVAL;
}
static inline void wbt_exit(struct request_queue *q)
{
}
static inline void wbt_update_limits(struct rq_wb *rwb)
{
}
static inline void wbt_requeue(struct rq_wb *rwb, struct blk_issue_stat *stat)
{
}
static inline void wbt_issue(struct rq_wb *rwb, struct blk_issue_stat *stat)
{
}
static inline void wbt_disable_default(struct request_queue *q)
{
}
static inline void wbt_enable_default(struct request_queue *q)
{
}
static inline void wbt_set_queue_depth(struct rq_wb *rwb, unsigned int depth)
{
}
static inline void wbt_set_write_cache(struct rq_wb *rwb, bool wc)
{
}
static inline u64 wbt_default_latency_nsec(struct request_queue *q)
{
	return 0;
}

#endif /* CONFIG_BLK_WBT */

#endif
