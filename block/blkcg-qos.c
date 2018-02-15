/*
 * Block rq-qos base io controller
 *
 * This works similar to wbt with a few exceptions
 *
 * - It's bio based, so the latency covers the whole block layer in addition to
 *   the actual io.
 * - We will throttle all IO that comes in here if we need to.
 * - We use the mean latency over the 100ms window.  This is because writes can
 *   be particularly fast, which could give us a false sense of the impact of
 *   other workloads on our protected workload.
 * - By default there's no throttling, we set the queue_depth to INT_MAX so that
 *   we can have as many outstanding bio's as we're allowed to.  Only at
 *   throttle time do we pay attention to the actual queue depth.
 *
 * Copyright (C) 2018 Josef Bacik
 */
#include <linux/kernel.h>
#include <linux/blk_types.h>
#include <linux/backing-dev.h>
#include <linux/module.h>
#include "blk-wbt.h"

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
	u64 min_lat_nsec;
	u64 cur_win_nsec;
	atomic_t throttle_count;
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
	return rq_wait_inc_below(rqw, qg->rq_depth.max_depth);
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
	u64 scale_lat = READ_ONCE(blkcg_qos->scale_lat);
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

	/* We're as low as we can go. */
	if (qg->rq_depth.max_depth == 1 && direction < 0)
		return;

	/* We're back to the default cookie, unthrottle all the things. */
	if (cur_cookie == DEFAULT_SCALE_COOKIE) {
		qg->rq_depth.max_depth = INT_MAX;
		wake_up_all(&qg->rq_wait.wait);
		return;
	}

	/*
	 * If the missed target was a higher requirement than ours we don't have
	 * to do anything.
	 */
	if (qg->min_lat_nsec &&
	    scale_lat >= qg->min_lat_nsec)
		return;

	if (direction > 0) {
		rq_depth_scale_up(&qg->rq_depth);
		wake_up_all(&qg->rq_wait.wait);
		return;
	}

	rq_depth_scale_down(&qg->rq_depth, false);
}

static void blkcg_qos_throttle(struct rq_qos *rqos, struct bio *bio,
			       spinlock_t *lock)
{
	struct blkcg_qos *blkqos = BLKQOS(rqos);
	struct blkcg *blkcg;
	struct blkcg_gq *blkg;
	struct qos_grp *qg = NULL;
	struct request_queue *q = rqos->q;
	bool throttle = false;

	if (!blkcg_qos_enabled(blkqos))
		return;

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

	/* We don't worry about kernel submitted io. */
	if (blkg == q->root_blkg)
		goto out;

	qg = blkg_to_qg(blkg);
	check_scale_change(blkqos, qg);
	if (!rq_wait_inc_below(&qg->rq_wait, qg->rq_depth.max_depth))
		throttle = true;
out:
	rcu_read_unlock();
	if (throttle)
		__blkcg_qos_throttle(rqos, qg, lock);
	if (blkg)
		blk_stat_set_issue(&bio->bi_issue_stat, bio_sectors(bio));
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
	if (stat.mean <= qg->min_lat_nsec &&
	    cookie == DEFAULT_SCALE_COOKIE) {
		return;
	}

	/*
	 * If we've recently adjusted the scale then give the throttling some
	 * time to work its magic.
	 */
	if (blkqos->last_scale_event >= now)
		return;
	if (now - blkqos->last_scale_event < BLKQOS_MIN_ADJUST_TIME)
		return;

	spin_lock(&blkqos->scale_lock);
	if (blkqos->last_scale_event >= now ||
	    now - blkqos->last_scale_event < BLKQOS_MIN_ADJUST_TIME)
		goto out;

	blkqos->last_scale_event = now;
	if (stat.mean <= qg->min_lat_nsec) {
		/* If we're the reason we scaled down then relax. */
		if (blkqos->scale_lat == qg->min_lat_nsec)
			atomic_inc(&blkqos->scale_cookie);
	} else {
		blkqos->scale_lat = qg->min_lat_nsec;
		smp_mb__before_atomic();
		atomic_dec(&blkqos->scale_cookie);
	}
out:
	spin_unlock(&blkqos->scale_lock);
}

static void blkcg_qos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw;
	struct qos_grp *qg;
	u64 window_start;
	u64 now;

	blkg = bio->bi_blkg;
	if (!blkg)
		return;

	qg = blkg_to_qg(blkg);
	rqw = &qg->rq_wait;

	atomic_dec(&rqw->inflight);
	if (!blkcg_qos_enabled(qg->blkqos) || qg->min_lat_nsec == 0) {
		wake_up_all(&rqw->wait);
		return;
	}

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
	wake_up_all(&rqw->wait);
}

static void blkcg_qos_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct rq_wait *rqw;
	struct qos_grp *qg;

	blkg = bio->bi_blkg;
	if (!blkg)
		return;
	qg = blkg_to_qg(blkg);
	rqw = &qg->rq_wait;

	atomic_dec(&rqw->inflight);
	wake_up_all(&rqw->wait);
}

static struct rq_qos_ops blkcg_qos_ops = {
	.throttle = blkcg_qos_throttle,
	.cleanup = blkcg_qos_cleanup,
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
	u64 oldval = qg->min_lat_nsec;

	while (blkg->parent) {
		struct qos_grp *this_qg = blkg_to_qg(blkg->parent);
		val = max(val, this_qg->set_min_lat_nsec);
		blkg = blkg->parent;
	}
	qg->min_lat_nsec = val;
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
	qg->set_min_lat_nsec = val * NSEC_PER_USEC;

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
	seq_printf(sf, "%s latency=%llu",
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
	int cpu;

	for_each_possible_cpu(cpu) {
		struct blk_rq_stat *stat;
		stat = per_cpu_ptr(qg->stats, cpu);
		blk_rq_stat_init(stat);
	}

	rq_wait_init(&qg->rq_wait);
	qg->rq_depth.queue_depth = blk_queue_depth(blkg->q);
	qg->rq_depth.max_depth = INT_MAX;
	qg->blkqos = blkqos;
	qg->cur_win_nsec = 100 * NSEC_PER_MSEC;
	atomic64_set(&qg->window_start, now);
	atomic_set(&qg->scale_cookie, atomic_read(&blkqos->scale_cookie));
	qg->set_min_lat_nsec = 0;

	atomic_set(&qg->throttle_count, 0);

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
