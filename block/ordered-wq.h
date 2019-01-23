#ifndef _ORDERED_WQ_H_
#define _ORDERED_WQ_H_

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/rbtree.h>

struct ordered_wait_queue_entry {
	struct task_struct *task;
	u64 key;
	struct rb_node node;
};

struct ordered_wait_queue_head {
	spinlock_t lock;
	struct rb_root_cached root;
};

typedef struct ordered_wait_queue_head ordered_wait_queue_head_t;
typedef struct ordered_wait_queue_entry ordered_wait_queue_entry_t;

void ordered_prepare_to_wait(ordered_wait_queue_head_t *owq,
			     ordered_wait_queue_entry_t *entry, int state);
void ordered_wake_up(ordered_wait_queue_head_t *owq, u64 key);
void ordered_finish_wait(ordered_wait_queue_head_t *owq,
			 ordered_wait_queue_entry_t *entry);

static inline void init_ordered_wait_queue_head(ordered_wait_queue_head_t *owq)
{
	spin_lock_init(&owq->lock);
	owq->root = RB_ROOT_CACHED;
}

static inline void
init_ordered_wait_queue_entry(ordered_wait_queue_entry_t *entry,
			      struct task_struct *p, u64 key)
{
	entry->task = p;
	entry->key = key;
	RB_CLEAR_NODE(&entry->node);
}

static inline int
ordered_wait_queue_has_sleepers(ordered_wait_queue_head_t *owq)
{
	/* Paired with the mb in set_current_state(). */
	smp_mb();
	return RB_EMPTY_ROOT(&owq->root.rb_root);
}

static inline void ordered_wake_up_all(ordered_wait_queue_head_t *owq)
{
	ordered_wake_up(owq, (u64)-1);
}
#endif
