#include "ordered-wq.h"

static void ordered_wait_queue_insert(ordered_wait_queue_head_t *owq,
				      ordered_wait_queue_entry_t *ins)
{
	struct rb_node **p = &owq->root.rb_root.rb_node;
	struct rb_node *parent = NULL;
	ordered_wait_queue_entry_t *entry;
	bool leftmost = true;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, ordered_wait_queue_entry_t, node);
		if (ins->key < entry->key) {
			p = &parent->rb_left;
		} else if (ins->key >= entry->key) {
			leftmost = false;
			p = &parent->rb_right;
		}
	}

	rb_link_node(&ins->node, parent, p);
	rb_insert_color_cached(&ins->node, &owq->root, leftmost);
}

void ordered_wake_up(ordered_wait_queue_head_t *owq, u64 key)
{
	unsigned long flags;
	struct rb_node *n;

	spin_lock_irqsave(&owq->lock, flags);
	for (n = rb_first_cached(&owq->root); n; n = rb_next(n)) {
		ordered_wait_queue_entry_t *entry;
		entry = rb_entry(n, ordered_wait_queue_entry_t, node);
		if (entry->key > key)
			break;
//		trace_printk("waking up key %llu with key %llu\n",
//			     entry->key, key);
		wake_up_process(entry->task);
	}
	spin_unlock_irqrestore(&owq->lock, flags);
}

void ordered_prepare_to_wait(ordered_wait_queue_head_t *owq,
			     ordered_wait_queue_entry_t *entry, int state)
{
	unsigned long flags;

	spin_lock_irqsave(&owq->lock, flags);
	if (RB_EMPTY_NODE(&entry->node))
		ordered_wait_queue_insert(owq, entry);
	set_current_state(state);
	spin_unlock_irqrestore(&owq->lock, flags);
}

void ordered_finish_wait(ordered_wait_queue_head_t *owq,
			 ordered_wait_queue_entry_t *entry)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	if (!RB_EMPTY_NODE(&entry->node)) {
		spin_lock_irqsave(&owq->lock, flags);
		rb_erase_cached(&entry->node, &owq->root);
		spin_unlock_irqrestore(&owq->lock, flags);
	}
}
