/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#include "ctree.h"
#include "extent-io-tree.h"
#include "btrfs_inode.h"
#include "volumes.h"

static struct kmem_cache *extent_state_cache;

static inline bool extent_state_in_tree(const struct extent_state *state)
{
	return !RB_EMPTY_NODE(&state->rb_node);
}

#ifdef CONFIG_BTRFS_DEBUG
static LIST_HEAD(states);

static DEFINE_SPINLOCK(leak_lock);

static inline
void btrfs_leak_debug_add(struct list_head *new, struct list_head *head)
{
	unsigned long flags;

	spin_lock_irqsave(&leak_lock, flags);
	list_add(new, head);
	spin_unlock_irqrestore(&leak_lock, flags);
}

static inline
void btrfs_leak_debug_del(struct list_head *entry)
{
	unsigned long flags;

	spin_lock_irqsave(&leak_lock, flags);
	list_del(entry);
	spin_unlock_irqrestore(&leak_lock, flags);
}

static inline
void btrfs_extent_state_leak_debug_check(void)
{
	struct extent_state *state;

	while (!list_empty(&states)) {
		state = list_entry(states.next, struct extent_state, leak_list);
		pr_err("BTRFS: state leak: start %llu end %llu state %u in tree %d refs %d\n",
		       state->start, state->end, state->state,
		       extent_state_in_tree(state),
		       refcount_read(&state->refs));
		list_del(&state->leak_list);
		kmem_cache_free(extent_state_cache, state);
	}
}

#define btrfs_debug_check_extent_io_range(tree, start, end)		\
	__btrfs_debug_check_extent_io_range(__func__, (tree), (start), (end))
static inline void __btrfs_debug_check_extent_io_range(const char *caller,
		struct extent_io_tree *tree, u64 start, u64 end)
{
	struct inode *inode = tree->private_data;
	u64 isize;

	if (!inode || !is_data_inode(inode))
		return;

	isize = i_size_read(inode);
	if (end >= PAGE_SIZE && (end % 2) == 0 && end != isize - 1) {
		btrfs_debug_rl(BTRFS_I(inode)->root->fs_info,
		    "%s: ino %llu isize %llu odd range [%llu,%llu]",
			caller, btrfs_ino(BTRFS_I(inode)), isize, start, end);
	}
}
#else
#define btrfs_leak_debug_add(new, head)	do {} while (0)
#define btrfs_leak_debug_del(entry)	do {} while (0)
#define btrfs_extent_state_leak_debug_check()	do {} while (0)
#define btrfs_debug_check_extent_io_range(c, s, e)	do {} while (0)
#endif

struct tree_entry {
	u64 start;
	u64 end;
	struct rb_node rb_node;
};

int __init extent_state_cache_init(void)
{
	extent_state_cache = kmem_cache_create("btrfs_extent_state",
			sizeof(struct extent_state), 0,
			SLAB_MEM_SPREAD, NULL);
	if (!extent_state_cache)
		return -ENOMEM;
	return 0;
}

void __cold extent_state_cache_exit(void)
{
	btrfs_extent_state_leak_debug_check();
	kmem_cache_destroy(extent_state_cache);
}

void extent_io_tree_init(struct btrfs_fs_info *fs_info,
			 struct extent_io_tree *tree, unsigned int owner,
			 void *private_data)
{
	tree->fs_info = fs_info;
	tree->state = RB_ROOT;
	tree->ops = NULL;
	tree->dirty_bytes = 0;
	spin_lock_init(&tree->lock);
	tree->private_data = private_data;
	tree->owner = owner;
}

void extent_io_tree_release(struct extent_io_tree *tree)
{
	spin_lock(&tree->lock);
	/*
	 * Do a single barrier for the waitqueue_active check here, the state
	 * of the waitqueue should not change once extent_io_tree_release is
	 * called.
	 */
	smp_mb();
	while (!RB_EMPTY_ROOT(&tree->state)) {
		struct rb_node *node;
		struct extent_state *state;

		node = rb_first(&tree->state);
		state = rb_entry(node, struct extent_state, rb_node);
		rb_erase(&state->rb_node, &tree->state);
		RB_CLEAR_NODE(&state->rb_node);
		/*
		 * btree io trees aren't supposed to have tasks waiting for
		 * changes in the flags of extent states ever.
		 */
		ASSERT(!waitqueue_active(&state->wq));
		free_extent_state(state);

		cond_resched_lock(&tree->lock);
	}
	spin_unlock(&tree->lock);
}

static int add_extent_changeset(struct extent_state *state, unsigned bits,
				 struct extent_changeset *changeset,
				 int set)
{
	int ret;

	if (!changeset)
		return 0;
	if (set && (state->state & bits) == bits)
		return 0;
	if (!set && (state->state & bits) == 0)
		return 0;
	changeset->bytes_changed += state->end - state->start + 1;
	ret = ulist_add(&changeset->range_changed, state->start, state->end,
			GFP_ATOMIC);
	return ret;
}

static struct extent_state *alloc_extent_state(gfp_t mask)
{
	struct extent_state *state;

	/*
	 * The given mask might be not appropriate for the slab allocator,
	 * drop the unsupported bits
	 */
	mask &= ~(__GFP_DMA32|__GFP_HIGHMEM);
	state = kmem_cache_alloc(extent_state_cache, mask);
	if (!state)
		return state;
	state->state = 0;
	state->failrec = NULL;
	RB_CLEAR_NODE(&state->rb_node);
	btrfs_leak_debug_add(&state->leak_list, &states);
	refcount_set(&state->refs, 1);
	init_waitqueue_head(&state->wq);
	trace_alloc_extent_state(state, mask, _RET_IP_);
	return state;
}

void free_extent_state(struct extent_state *state)
{
	if (!state)
		return;
	if (refcount_dec_and_test(&state->refs)) {
		WARN_ON(extent_state_in_tree(state));
		btrfs_leak_debug_del(&state->leak_list);
		trace_free_extent_state(state, _RET_IP_);
		kmem_cache_free(extent_state_cache, state);
	}
}

static struct rb_node *tree_insert(struct rb_root *root,
				   struct rb_node *search_start,
				   u64 offset,
				   struct rb_node *node,
				   struct rb_node ***p_in,
				   struct rb_node **parent_in)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct tree_entry *entry;

	if (p_in && parent_in) {
		p = *p_in;
		parent = *parent_in;
		goto do_insert;
	}

	p = search_start ? &search_start : &root->rb_node;
	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct tree_entry, rb_node);

		if (offset < entry->start)
			p = &(*p)->rb_left;
		else if (offset > entry->end)
			p = &(*p)->rb_right;
		else
			return parent;
	}

do_insert:
	rb_link_node(node, parent, p);
	rb_insert_color(node, root);
	return NULL;
}

/**
 * __etree_search - searche @tree for an entry that contains @offset. Such
 * entry would have entry->start <= offset && entry->end >= offset.
 *
 * @tree - the tree to search
 * @offset - offset that should fall within an entry in @tree
 * @next_ret - pointer to the first entry whose range ends after @offset
 * @prev - pointer to the first entry whose range begins before @offset
 * @p_ret - pointer where new node should be anchored (used when inserting an
 *	    entry in the tree)
 * @parent_ret - points to entry which would have been the parent of the entry,
 *               containing @offset
 *
 * This function returns a pointer to the entry that contains @offset byte
 * address. If no such entry exists, then NULL is returned and the other
 * pointer arguments to the function are filled, otherwise the found entry is
 * returned and other pointers are left untouched.
 */
static struct rb_node *__etree_search(struct extent_io_tree *tree, u64 offset,
				      struct rb_node **next_ret,
				      struct rb_node **prev_ret,
				      struct rb_node ***p_ret,
				      struct rb_node **parent_ret)
{
	struct rb_root *root = &tree->state;
	struct rb_node **n = &root->rb_node;
	struct rb_node *prev = NULL;
	struct rb_node *orig_prev = NULL;
	struct tree_entry *entry;
	struct tree_entry *prev_entry = NULL;

	while (*n) {
		prev = *n;
		entry = rb_entry(prev, struct tree_entry, rb_node);
		prev_entry = entry;

		if (offset < entry->start)
			n = &(*n)->rb_left;
		else if (offset > entry->end)
			n = &(*n)->rb_right;
		else
			return *n;
	}

	if (p_ret)
		*p_ret = n;
	if (parent_ret)
		*parent_ret = prev;

	if (next_ret) {
		orig_prev = prev;
		while (prev && offset > prev_entry->end) {
			prev = rb_next(prev);
			prev_entry = rb_entry(prev, struct tree_entry, rb_node);
		}
		*next_ret = prev;
		prev = orig_prev;
	}

	if (prev_ret) {
		prev_entry = rb_entry(prev, struct tree_entry, rb_node);
		while (prev && offset < prev_entry->start) {
			prev = rb_prev(prev);
			prev_entry = rb_entry(prev, struct tree_entry, rb_node);
		}
		*prev_ret = prev;
	}
	return NULL;
}

static inline struct rb_node *
tree_search_for_insert(struct extent_io_tree *tree,
		       u64 offset,
		       struct rb_node ***p_ret,
		       struct rb_node **parent_ret)
{
	struct rb_node *next= NULL;
	struct rb_node *ret;

	ret = __etree_search(tree, offset, &next, NULL, p_ret, parent_ret);
	if (!ret)
		return next;
	return ret;
}

static inline struct rb_node *tree_search(struct extent_io_tree *tree,
					  u64 offset)
{
	return tree_search_for_insert(tree, offset, NULL, NULL);
}

/*
 * utility function to look for merge candidates inside a given range.
 * Any extents with matching state are merged together into a single
 * extent in the tree.  Extents with EXTENT_IO in their state field
 * are not merged because the end_io handlers need to be able to do
 * operations on them without sleeping (or doing allocations/splits).
 *
 * This should be called with the tree lock held.
 */
static void merge_state(struct extent_io_tree *tree,
		        struct extent_state *state)
{
	struct extent_state *other;
	struct rb_node *other_node;

	if (state->state & (EXTENT_LOCKED | EXTENT_BOUNDARY))
		return;

	other_node = rb_prev(&state->rb_node);
	if (other_node) {
		other = rb_entry(other_node, struct extent_state, rb_node);
		if (other->end == state->start - 1 &&
		    other->state == state->state) {
			if (tree->private_data &&
			    is_data_inode(tree->private_data))
				btrfs_merge_delalloc_extent(tree->private_data,
							    state, other);
			state->start = other->start;
			rb_erase(&other->rb_node, &tree->state);
			RB_CLEAR_NODE(&other->rb_node);
			free_extent_state(other);
		}
	}
	other_node = rb_next(&state->rb_node);
	if (other_node) {
		other = rb_entry(other_node, struct extent_state, rb_node);
		if (other->start == state->end + 1 &&
		    other->state == state->state) {
			if (tree->private_data &&
			    is_data_inode(tree->private_data))
				btrfs_merge_delalloc_extent(tree->private_data,
							    state, other);
			state->end = other->end;
			rb_erase(&other->rb_node, &tree->state);
			RB_CLEAR_NODE(&other->rb_node);
			free_extent_state(other);
		}
	}
}

static void set_state_bits(struct extent_io_tree *tree,
			   struct extent_state *state, unsigned *bits,
			   struct extent_changeset *changeset);

/*
 * insert an extent_state struct into the tree.  'bits' are set on the
 * struct before it is inserted.
 *
 * This may return -EEXIST if the extent is already there, in which case the
 * state struct is freed.
 *
 * The tree lock is not taken internally.  This is a utility function and
 * probably isn't what you want to call (see set/clear_extent_bit).
 */
static int insert_state(struct extent_io_tree *tree,
			struct extent_state *state, u64 start, u64 end,
			struct rb_node ***p,
			struct rb_node **parent,
			unsigned *bits, struct extent_changeset *changeset)
{
	struct rb_node *node;

	if (end < start) {
		btrfs_err(tree->fs_info,
			"insert state: end < start %llu %llu", end, start);
		WARN_ON(1);
	}
	state->start = start;
	state->end = end;

	set_state_bits(tree, state, bits, changeset);

	node = tree_insert(&tree->state, NULL, end, &state->rb_node, p, parent);
	if (node) {
		struct extent_state *found;
		found = rb_entry(node, struct extent_state, rb_node);
		btrfs_err(tree->fs_info,
		       "found node %llu %llu on insert of %llu %llu",
		       found->start, found->end, start, end);
		return -EEXIST;
	}
	merge_state(tree, state);
	return 0;
}

/*
 * split a given extent state struct in two, inserting the preallocated
 * struct 'prealloc' as the newly created second half.  'split' indicates an
 * offset inside 'orig' where it should be split.
 *
 * Before calling,
 * the tree has 'orig' at [orig->start, orig->end].  After calling, there
 * are two extent state structs in the tree:
 * prealloc: [orig->start, split - 1]
 * orig: [ split, orig->end ]
 *
 * The tree locks are not taken by this function. They need to be held
 * by the caller.
 */
static int split_state(struct extent_io_tree *tree, struct extent_state *orig,
		       struct extent_state *prealloc, u64 split)
{
	struct rb_node *node;

	if (tree->private_data && is_data_inode(tree->private_data))
		btrfs_split_delalloc_extent(tree->private_data, orig, split);

	prealloc->start = orig->start;
	prealloc->end = split - 1;
	prealloc->state = orig->state;
	orig->start = split;

	node = tree_insert(&tree->state, &orig->rb_node, prealloc->end,
			   &prealloc->rb_node, NULL, NULL);
	if (node) {
		free_extent_state(prealloc);
		return -EEXIST;
	}
	return 0;
}

static struct extent_state *next_state(struct extent_state *state)
{
	struct rb_node *next = rb_next(&state->rb_node);
	if (next)
		return rb_entry(next, struct extent_state, rb_node);
	else
		return NULL;
}

/*
 * utility function to clear some bits in an extent state struct.
 * it will optionally wake up anyone waiting on this state (wake == 1).
 *
 * If no bits are set on the state struct after clearing things, the
 * struct is freed and removed from the tree
 */
static struct extent_state *clear_state_bit(struct extent_io_tree *tree,
					    struct extent_state *state,
					    unsigned *bits, int wake,
					    struct extent_changeset *changeset)
{
	struct extent_state *next;
	unsigned bits_to_clear = *bits & ~EXTENT_CTLBITS;
	int ret;

	if ((bits_to_clear & EXTENT_DIRTY) && (state->state & EXTENT_DIRTY)) {
		u64 range = state->end - state->start + 1;
		WARN_ON(range > tree->dirty_bytes);
		tree->dirty_bytes -= range;
	}

	if (tree->private_data && is_data_inode(tree->private_data))
		btrfs_clear_delalloc_extent(tree->private_data, state, bits);

	ret = add_extent_changeset(state, bits_to_clear, changeset, 0);
	BUG_ON(ret < 0);
	state->state &= ~bits_to_clear;
	if (wake)
		wake_up(&state->wq);
	if (state->state == 0) {
		next = next_state(state);
		if (extent_state_in_tree(state)) {
			rb_erase(&state->rb_node, &tree->state);
			RB_CLEAR_NODE(&state->rb_node);
			free_extent_state(state);
		} else {
			WARN_ON(1);
		}
	} else {
		merge_state(tree, state);
		next = next_state(state);
	}
	return next;
}

static struct extent_state *
alloc_extent_state_atomic(struct extent_state *prealloc)
{
	if (!prealloc)
		prealloc = alloc_extent_state(GFP_ATOMIC);

	return prealloc;
}

static void extent_io_tree_panic(struct extent_io_tree *tree, int err)
{
	struct inode *inode = tree->private_data;

	btrfs_panic(btrfs_sb(inode->i_sb), err,
	"locking error: extent tree was modified by another thread while locked");
}

/*
 * clear some bits on a range in the tree.  This may require splitting
 * or inserting elements in the tree, so the gfp mask is used to
 * indicate which allocations or sleeping are allowed.
 *
 * pass 'wake' == 1 to kick any sleepers, and 'delete' == 1 to remove
 * the given range from the tree regardless of state (ie for truncate).
 *
 * the range [start, end] is inclusive.
 *
 * This takes the tree lock, and returns 0 on success and < 0 on error.
 */
int __clear_extent_bit(struct extent_io_tree *tree, u64 start, u64 end,
			      unsigned bits, int wake, int delete,
			      struct extent_state **cached_state,
			      gfp_t mask, struct extent_changeset *changeset)
{
	struct extent_state *state;
	struct extent_state *cached;
	struct extent_state *prealloc = NULL;
	struct rb_node *node;
	u64 last_end;
	int err;
	int clear = 0;

	btrfs_debug_check_extent_io_range(tree, start, end);
	trace_btrfs_clear_extent_bit(tree, start, end - start + 1, bits);

	if (bits & EXTENT_DELALLOC)
		bits |= EXTENT_NORESERVE;

	if (delete)
		bits |= ~EXTENT_CTLBITS;

	if (bits & (EXTENT_LOCKED | EXTENT_BOUNDARY))
		clear = 1;
again:
	if (!prealloc && gfpflags_allow_blocking(mask)) {
		/*
		 * Don't care for allocation failure here because we might end
		 * up not needing the pre-allocated extent state at all, which
		 * is the case if we only have in the tree extent states that
		 * cover our input range and don't cover too any other range.
		 * If we end up needing a new extent state we allocate it later.
		 */
		prealloc = alloc_extent_state(mask);
	}

	spin_lock(&tree->lock);
	if (cached_state) {
		cached = *cached_state;

		if (clear) {
			*cached_state = NULL;
			cached_state = NULL;
		}

		if (cached && extent_state_in_tree(cached) &&
		    cached->start <= start && cached->end > start) {
			if (clear)
				refcount_dec(&cached->refs);
			state = cached;
			goto hit_next;
		}
		if (clear)
			free_extent_state(cached);
	}
	/*
	 * this search will find the extents that end after
	 * our range starts
	 */
	node = tree_search(tree, start);
	if (!node)
		goto out;
	state = rb_entry(node, struct extent_state, rb_node);
hit_next:
	if (state->start > end)
		goto out;
	WARN_ON(state->end < start);
	last_end = state->end;

	/* the state doesn't have the wanted bits, go ahead */
	if (!(state->state & bits)) {
		state = next_state(state);
		goto next;
	}

	/*
	 *     | ---- desired range ---- |
	 *  | state | or
	 *  | ------------- state -------------- |
	 *
	 * We need to split the extent we found, and may flip
	 * bits on second half.
	 *
	 * If the extent we found extends past our range, we
	 * just split and search again.  It'll get split again
	 * the next time though.
	 *
	 * If the extent we found is inside our range, we clear
	 * the desired bit on it.
	 */

	if (state->start < start) {
		prealloc = alloc_extent_state_atomic(prealloc);
		BUG_ON(!prealloc);
		err = split_state(tree, state, prealloc, start);
		if (err)
			extent_io_tree_panic(tree, err);

		prealloc = NULL;
		if (err)
			goto out;
		if (state->end <= end) {
			state = clear_state_bit(tree, state, &bits, wake,
						changeset);
			goto next;
		}
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *                        | state |
	 * We need to split the extent, and clear the bit
	 * on the first half
	 */
	if (state->start <= end && state->end > end) {
		prealloc = alloc_extent_state_atomic(prealloc);
		BUG_ON(!prealloc);
		err = split_state(tree, state, prealloc, end + 1);
		if (err)
			extent_io_tree_panic(tree, err);

		if (wake)
			wake_up(&state->wq);

		clear_state_bit(tree, prealloc, &bits, wake, changeset);

		prealloc = NULL;
		goto out;
	}

	state = clear_state_bit(tree, state, &bits, wake, changeset);
next:
	if (last_end == (u64)-1)
		goto out;
	start = last_end + 1;
	if (start <= end && state && !need_resched())
		goto hit_next;

search_again:
	if (start > end)
		goto out;
	spin_unlock(&tree->lock);
	if (gfpflags_allow_blocking(mask))
		cond_resched();
	goto again;

out:
	spin_unlock(&tree->lock);
	if (prealloc)
		free_extent_state(prealloc);

	return 0;

}

static void wait_on_state(struct extent_io_tree *tree,
			  struct extent_state *state)
		__releases(tree->lock)
		__acquires(tree->lock)
{
	DEFINE_WAIT(wait);
	prepare_to_wait(&state->wq, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock(&tree->lock);
	schedule();
	spin_lock(&tree->lock);
	finish_wait(&state->wq, &wait);
}

/*
 * waits for one or more bits to clear on a range in the state tree.
 * The range [start, end] is inclusive.
 * The tree lock is taken by this function
 */
static void wait_extent_bit(struct extent_io_tree *tree, u64 start, u64 end,
			    unsigned long bits)
{
	struct extent_state *state;
	struct rb_node *node;

	btrfs_debug_check_extent_io_range(tree, start, end);

	spin_lock(&tree->lock);
again:
	while (1) {
		/*
		 * this search will find all the extents that end after
		 * our range starts
		 */
		node = tree_search(tree, start);
process_node:
		if (!node)
			break;

		state = rb_entry(node, struct extent_state, rb_node);

		if (state->start > end)
			goto out;

		if (state->state & bits) {
			start = state->start;
			refcount_inc(&state->refs);
			wait_on_state(tree, state);
			free_extent_state(state);
			goto again;
		}
		start = state->end + 1;

		if (start > end)
			break;

		if (!cond_resched_lock(&tree->lock)) {
			node = rb_next(node);
			goto process_node;
		}
	}
out:
	spin_unlock(&tree->lock);
}

static void set_state_bits(struct extent_io_tree *tree,
			   struct extent_state *state,
			   unsigned *bits, struct extent_changeset *changeset)
{
	unsigned bits_to_set = *bits & ~EXTENT_CTLBITS;
	int ret;

	if (tree->private_data && is_data_inode(tree->private_data))
		btrfs_set_delalloc_extent(tree->private_data, state, bits);

	if ((bits_to_set & EXTENT_DIRTY) && !(state->state & EXTENT_DIRTY)) {
		u64 range = state->end - state->start + 1;
		tree->dirty_bytes += range;
	}
	ret = add_extent_changeset(state, bits_to_set, changeset, 1);
	BUG_ON(ret < 0);
	state->state |= bits_to_set;
}

static void cache_state_if_flags(struct extent_state *state,
				 struct extent_state **cached_ptr,
				 unsigned flags)
{
	if (cached_ptr && !(*cached_ptr)) {
		if (!flags || (state->state & flags)) {
			*cached_ptr = state;
			refcount_inc(&state->refs);
		}
	}
}

static void cache_state(struct extent_state *state,
			struct extent_state **cached_ptr)
{
	return cache_state_if_flags(state, cached_ptr,
				    EXTENT_LOCKED | EXTENT_BOUNDARY);
}

/*
 * set some bits on a range in the tree.  This may require allocations or
 * sleeping, so the gfp mask is used to indicate what is allowed.
 *
 * If any of the exclusive bits are set, this will fail with -EEXIST if some
 * part of the range already has the desired bits set.  The start of the
 * existing range is returned in failed_start in this case.
 *
 * [start, end] is inclusive This takes the tree lock.
 */

static int __must_check
__set_extent_bit(struct extent_io_tree *tree, u64 start, u64 end,
		 unsigned bits, unsigned exclusive_bits,
		 u64 *failed_start, struct extent_state **cached_state,
		 gfp_t mask, struct extent_changeset *changeset)
{
	struct extent_state *state;
	struct extent_state *prealloc = NULL;
	struct rb_node *node;
	struct rb_node **p;
	struct rb_node *parent;
	int err = 0;
	u64 last_start;
	u64 last_end;

	btrfs_debug_check_extent_io_range(tree, start, end);
	trace_btrfs_set_extent_bit(tree, start, end - start + 1, bits);

again:
	if (!prealloc && gfpflags_allow_blocking(mask)) {
		/*
		 * Don't care for allocation failure here because we might end
		 * up not needing the pre-allocated extent state at all, which
		 * is the case if we only have in the tree extent states that
		 * cover our input range and don't cover too any other range.
		 * If we end up needing a new extent state we allocate it later.
		 */
		prealloc = alloc_extent_state(mask);
	}

	spin_lock(&tree->lock);
	if (cached_state && *cached_state) {
		state = *cached_state;
		if (state->start <= start && state->end > start &&
		    extent_state_in_tree(state)) {
			node = &state->rb_node;
			goto hit_next;
		}
	}
	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search_for_insert(tree, start, &p, &parent);
	if (!node) {
		prealloc = alloc_extent_state_atomic(prealloc);
		BUG_ON(!prealloc);
		err = insert_state(tree, prealloc, start, end,
				   &p, &parent, &bits, changeset);
		if (err)
			extent_io_tree_panic(tree, err);

		cache_state(prealloc, cached_state);
		prealloc = NULL;
		goto out;
	}
	state = rb_entry(node, struct extent_state, rb_node);
hit_next:
	last_start = state->start;
	last_end = state->end;

	/*
	 * | ---- desired range ---- |
	 * | state |
	 *
	 * Just lock what we found and keep going
	 */
	if (state->start == start && state->end <= end) {
		if (state->state & exclusive_bits) {
			*failed_start = state->start;
			err = -EEXIST;
			goto out;
		}

		set_state_bits(tree, state, &bits, changeset);
		cache_state(state, cached_state);
		merge_state(tree, state);
		if (last_end == (u64)-1)
			goto out;
		start = last_end + 1;
		state = next_state(state);
		if (start < end && state && state->start == start &&
		    !need_resched())
			goto hit_next;
		goto search_again;
	}

	/*
	 *     | ---- desired range ---- |
	 * | state |
	 *   or
	 * | ------------- state -------------- |
	 *
	 * We need to split the extent we found, and may flip bits on
	 * second half.
	 *
	 * If the extent we found extends past our
	 * range, we just split and search again.  It'll get split
	 * again the next time though.
	 *
	 * If the extent we found is inside our range, we set the
	 * desired bit on it.
	 */
	if (state->start < start) {
		if (state->state & exclusive_bits) {
			*failed_start = start;
			err = -EEXIST;
			goto out;
		}

		prealloc = alloc_extent_state_atomic(prealloc);
		BUG_ON(!prealloc);
		err = split_state(tree, state, prealloc, start);
		if (err)
			extent_io_tree_panic(tree, err);

		prealloc = NULL;
		if (err)
			goto out;
		if (state->end <= end) {
			set_state_bits(tree, state, &bits, changeset);
			cache_state(state, cached_state);
			merge_state(tree, state);
			if (last_end == (u64)-1)
				goto out;
			start = last_end + 1;
			state = next_state(state);
			if (start < end && state && state->start == start &&
			    !need_resched())
				goto hit_next;
		}
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *     | state | or               | state |
	 *
	 * There's a hole, we need to insert something in it and
	 * ignore the extent we found.
	 */
	if (state->start > start) {
		u64 this_end;
		if (end < last_start)
			this_end = end;
		else
			this_end = last_start - 1;

		prealloc = alloc_extent_state_atomic(prealloc);
		BUG_ON(!prealloc);

		/*
		 * Avoid to free 'prealloc' if it can be merged with
		 * the later extent.
		 */
		err = insert_state(tree, prealloc, start, this_end,
				   NULL, NULL, &bits, changeset);
		if (err)
			extent_io_tree_panic(tree, err);

		cache_state(prealloc, cached_state);
		prealloc = NULL;
		start = this_end + 1;
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *                        | state |
	 * We need to split the extent, and set the bit
	 * on the first half
	 */
	if (state->start <= end && state->end > end) {
		if (state->state & exclusive_bits) {
			*failed_start = start;
			err = -EEXIST;
			goto out;
		}

		prealloc = alloc_extent_state_atomic(prealloc);
		BUG_ON(!prealloc);
		err = split_state(tree, state, prealloc, end + 1);
		if (err)
			extent_io_tree_panic(tree, err);

		set_state_bits(tree, prealloc, &bits, changeset);
		cache_state(prealloc, cached_state);
		merge_state(tree, prealloc);
		prealloc = NULL;
		goto out;
	}

search_again:
	if (start > end)
		goto out;
	spin_unlock(&tree->lock);
	if (gfpflags_allow_blocking(mask))
		cond_resched();
	goto again;

out:
	spin_unlock(&tree->lock);
	if (prealloc)
		free_extent_state(prealloc);

	return err;

}

int set_extent_bit(struct extent_io_tree *tree, u64 start, u64 end,
		   unsigned bits, u64 * failed_start,
		   struct extent_state **cached_state, gfp_t mask)
{
	return __set_extent_bit(tree, start, end, bits, 0, failed_start,
				cached_state, mask, NULL);
}


/**
 * convert_extent_bit - convert all bits in a given range from one bit to
 * 			another
 * @tree:	the io tree to search
 * @start:	the start offset in bytes
 * @end:	the end offset in bytes (inclusive)
 * @bits:	the bits to set in this range
 * @clear_bits:	the bits to clear in this range
 * @cached_state:	state that we're going to cache
 *
 * This will go through and set bits for the given range.  If any states exist
 * already in this range they are set with the given bit and cleared of the
 * clear_bits.  This is only meant to be used by things that are mergeable, ie
 * converting from say DELALLOC to DIRTY.  This is not meant to be used with
 * boundary bits like LOCK.
 *
 * All allocations are done with GFP_NOFS.
 */
int convert_extent_bit(struct extent_io_tree *tree, u64 start, u64 end,
		       unsigned bits, unsigned clear_bits,
		       struct extent_state **cached_state)
{
	struct extent_state *state;
	struct extent_state *prealloc = NULL;
	struct rb_node *node;
	struct rb_node **p;
	struct rb_node *parent;
	int err = 0;
	u64 last_start;
	u64 last_end;
	bool first_iteration = true;

	btrfs_debug_check_extent_io_range(tree, start, end);
	trace_btrfs_convert_extent_bit(tree, start, end - start + 1, bits,
				       clear_bits);

again:
	if (!prealloc) {
		/*
		 * Best effort, don't worry if extent state allocation fails
		 * here for the first iteration. We might have a cached state
		 * that matches exactly the target range, in which case no
		 * extent state allocations are needed. We'll only know this
		 * after locking the tree.
		 */
		prealloc = alloc_extent_state(GFP_NOFS);
		if (!prealloc && !first_iteration)
			return -ENOMEM;
	}

	spin_lock(&tree->lock);
	if (cached_state && *cached_state) {
		state = *cached_state;
		if (state->start <= start && state->end > start &&
		    extent_state_in_tree(state)) {
			node = &state->rb_node;
			goto hit_next;
		}
	}

	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search_for_insert(tree, start, &p, &parent);
	if (!node) {
		prealloc = alloc_extent_state_atomic(prealloc);
		if (!prealloc) {
			err = -ENOMEM;
			goto out;
		}
		err = insert_state(tree, prealloc, start, end,
				   &p, &parent, &bits, NULL);
		if (err)
			extent_io_tree_panic(tree, err);
		cache_state(prealloc, cached_state);
		prealloc = NULL;
		goto out;
	}
	state = rb_entry(node, struct extent_state, rb_node);
hit_next:
	last_start = state->start;
	last_end = state->end;

	/*
	 * | ---- desired range ---- |
	 * | state |
	 *
	 * Just lock what we found and keep going
	 */
	if (state->start == start && state->end <= end) {
		set_state_bits(tree, state, &bits, NULL);
		cache_state(state, cached_state);
		state = clear_state_bit(tree, state, &clear_bits, 0, NULL);
		if (last_end == (u64)-1)
			goto out;
		start = last_end + 1;
		if (start < end && state && state->start == start &&
		    !need_resched())
			goto hit_next;
		goto search_again;
	}

	/*
	 *     | ---- desired range ---- |
	 * | state |
	 *   or
	 * | ------------- state -------------- |
	 *
	 * We need to split the extent we found, and may flip bits on
	 * second half.
	 *
	 * If the extent we found extends past our
	 * range, we just split and search again.  It'll get split
	 * again the next time though.
	 *
	 * If the extent we found is inside our range, we set the
	 * desired bit on it.
	 */
	if (state->start < start) {
		prealloc = alloc_extent_state_atomic(prealloc);
		if (!prealloc) {
			err = -ENOMEM;
			goto out;
		}
		err = split_state(tree, state, prealloc, start);
		if (err)
			extent_io_tree_panic(tree, err);
		prealloc = NULL;
		if (err)
			goto out;
		if (state->end <= end) {
			set_state_bits(tree, state, &bits, NULL);
			cache_state(state, cached_state);
			state = clear_state_bit(tree, state, &clear_bits, 0,
						NULL);
			if (last_end == (u64)-1)
				goto out;
			start = last_end + 1;
			if (start < end && state && state->start == start &&
			    !need_resched())
				goto hit_next;
		}
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *     | state | or               | state |
	 *
	 * There's a hole, we need to insert something in it and
	 * ignore the extent we found.
	 */
	if (state->start > start) {
		u64 this_end;
		if (end < last_start)
			this_end = end;
		else
			this_end = last_start - 1;

		prealloc = alloc_extent_state_atomic(prealloc);
		if (!prealloc) {
			err = -ENOMEM;
			goto out;
		}

		/*
		 * Avoid to free 'prealloc' if it can be merged with
		 * the later extent.
		 */
		err = insert_state(tree, prealloc, start, this_end,
				   NULL, NULL, &bits, NULL);
		if (err)
			extent_io_tree_panic(tree, err);
		cache_state(prealloc, cached_state);
		prealloc = NULL;
		start = this_end + 1;
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *                        | state |
	 * We need to split the extent, and set the bit
	 * on the first half
	 */
	if (state->start <= end && state->end > end) {
		prealloc = alloc_extent_state_atomic(prealloc);
		if (!prealloc) {
			err = -ENOMEM;
			goto out;
		}

		err = split_state(tree, state, prealloc, end + 1);
		if (err)
			extent_io_tree_panic(tree, err);

		set_state_bits(tree, prealloc, &bits, NULL);
		cache_state(prealloc, cached_state);
		clear_state_bit(tree, prealloc, &clear_bits, 0, NULL);
		prealloc = NULL;
		goto out;
	}

search_again:
	if (start > end)
		goto out;
	spin_unlock(&tree->lock);
	cond_resched();
	first_iteration = false;
	goto again;

out:
	spin_unlock(&tree->lock);
	if (prealloc)
		free_extent_state(prealloc);

	return err;
}

/* wrappers around set/clear extent bit */
int set_record_extent_bits(struct extent_io_tree *tree, u64 start, u64 end,
			   unsigned bits, struct extent_changeset *changeset)
{
	/*
	 * We don't support EXTENT_LOCKED yet, as current changeset will
	 * record any bits changed, so for EXTENT_LOCKED case, it will
	 * either fail with -EEXIST or changeset will record the whole
	 * range.
	 */
	BUG_ON(bits & EXTENT_LOCKED);

	return __set_extent_bit(tree, start, end, bits, 0, NULL, NULL, GFP_NOFS,
				changeset);
}

int set_extent_bits_nowait(struct extent_io_tree *tree, u64 start, u64 end,
			   unsigned bits)
{
	return __set_extent_bit(tree, start, end, bits, 0, NULL, NULL,
				GFP_NOWAIT, NULL);
}

int clear_extent_bit(struct extent_io_tree *tree, u64 start, u64 end,
		     unsigned bits, int wake, int delete,
		     struct extent_state **cached)
{
	return __clear_extent_bit(tree, start, end, bits, wake, delete,
				  cached, GFP_NOFS, NULL);
}

int clear_record_extent_bits(struct extent_io_tree *tree, u64 start, u64 end,
		unsigned bits, struct extent_changeset *changeset)
{
	/*
	 * Don't support EXTENT_LOCKED case, same reason as
	 * set_record_extent_bits().
	 */
	BUG_ON(bits & EXTENT_LOCKED);

	return __clear_extent_bit(tree, start, end, bits, 0, 0, NULL, GFP_NOFS,
				  changeset);
}

/*
 * either insert or lock state struct between start and end use mask to tell
 * us if waiting is desired.
 */
int lock_extent_bits(struct extent_io_tree *tree, u64 start, u64 end,
		     struct extent_state **cached_state)
{
	int err;
	u64 failed_start;

	while (1) {
		err = __set_extent_bit(tree, start, end, EXTENT_LOCKED,
				       EXTENT_LOCKED, &failed_start,
				       cached_state, GFP_NOFS, NULL);
		if (err == -EEXIST) {
			wait_extent_bit(tree, failed_start, end, EXTENT_LOCKED);
			start = failed_start;
		} else
			break;
		WARN_ON(start > end);
	}
	return err;
}

int try_lock_extent(struct extent_io_tree *tree, u64 start, u64 end)
{
	int err;
	u64 failed_start;

	err = __set_extent_bit(tree, start, end, EXTENT_LOCKED, EXTENT_LOCKED,
			       &failed_start, NULL, GFP_NOFS, NULL);
	if (err == -EEXIST) {
		if (failed_start > start)
			clear_extent_bit(tree, start, failed_start - 1,
					 EXTENT_LOCKED, 1, 0, NULL);
		return 0;
	}
	return 1;
}

/* find the first state struct with 'bits' set after 'start', and
 * return it.  tree->lock must be held.  NULL will returned if
 * nothing was found after 'start'
 */
static struct extent_state *
find_first_extent_bit_state(struct extent_io_tree *tree,
			    u64 start, unsigned bits)
{
	struct rb_node *node;
	struct extent_state *state;

	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search(tree, start);
	if (!node)
		goto out;

	while (1) {
		state = rb_entry(node, struct extent_state, rb_node);
		if (state->end >= start && (state->state & bits))
			return state;

		node = rb_next(node);
		if (!node)
			break;
	}
out:
	return NULL;
}

/*
 * find the first offset in the io tree with 'bits' set. zero is
 * returned if we find something, and *start_ret and *end_ret are
 * set to reflect the state struct that was found.
 *
 * If nothing was found, 1 is returned. If found something, return 0.
 */
int find_first_extent_bit(struct extent_io_tree *tree, u64 start,
			  u64 *start_ret, u64 *end_ret, unsigned bits,
			  struct extent_state **cached_state)
{
	struct extent_state *state;
	int ret = 1;

	spin_lock(&tree->lock);
	if (cached_state && *cached_state) {
		state = *cached_state;
		if (state->end == start - 1 && extent_state_in_tree(state)) {
			while ((state = next_state(state)) != NULL) {
				if (state->state & bits)
					goto got_it;
			}
			free_extent_state(*cached_state);
			*cached_state = NULL;
			goto out;
		}
		free_extent_state(*cached_state);
		*cached_state = NULL;
	}

	state = find_first_extent_bit_state(tree, start, bits);
got_it:
	if (state) {
		cache_state_if_flags(state, cached_state, 0);
		*start_ret = state->start;
		*end_ret = state->end;
		ret = 0;
	}
out:
	spin_unlock(&tree->lock);
	return ret;
}

/**
 * find_first_clear_extent_bit - find the first range that has @bits not set.
 * This range could start before @start.
 *
 * @tree - the tree to search
 * @start - the offset at/after which the found extent should start
 * @start_ret - records the beginning of the range
 * @end_ret - records the end of the range (inclusive)
 * @bits - the set of bits which must be unset
 *
 * Since unallocated range is also considered one which doesn't have the bits
 * set it's possible that @end_ret contains -1, this happens in case the range
 * spans (last_range_end, end of device]. In this case it's up to the caller to
 * trim @end_ret to the appropriate size.
 */
void find_first_clear_extent_bit(struct extent_io_tree *tree, u64 start,
				 u64 *start_ret, u64 *end_ret, unsigned bits)
{
	struct extent_state *state;
	struct rb_node *node, *prev = NULL, *next;

	spin_lock(&tree->lock);

	/* Find first extent with bits cleared */
	while (1) {
		node = __etree_search(tree, start, &next, &prev, NULL, NULL);
		if (!node) {
			node = next;
			if (!node) {
				/*
				 * We are past the last allocated chunk,
				 * set start at the end of the last extent. The
				 * device alloc tree should never be empty so
				 * prev is always set.
				 */
				ASSERT(prev);
				state = rb_entry(prev, struct extent_state, rb_node);
				*start_ret = state->end + 1;
				*end_ret = -1;
				goto out;
			}
		}
		/*
		 * At this point 'node' either contains 'start' or start is
		 * before 'node'
		 */
		state = rb_entry(node, struct extent_state, rb_node);

		if (in_range(start, state->start, state->end - state->start + 1)) {
			if (state->state & bits) {
				/*
				 * |--range with bits sets--|
				 *    |
				 *    start
				 */
				start = state->end + 1;
			} else {
				/*
				 * 'start' falls within a range that doesn't
				 * have the bits set, so take its start as
				 * the beginning of the desired range
				 *
				 * |--range with bits cleared----|
				 *      |
				 *      start
				 */
				*start_ret = state->start;
				break;
			}
		} else {
			/*
			 * |---prev range---|---hole/unset---|---node range---|
			 *                          |
			 *                        start
			 *
			 *                        or
			 *
			 * |---hole/unset--||--first node--|
			 * 0   |
			 *    start
			 */
			if (prev) {
				state = rb_entry(prev, struct extent_state,
						 rb_node);
				*start_ret = state->end + 1;
			} else {
				*start_ret = 0;
			}
			break;
		}
	}

	/*
	 * Find the longest stretch from start until an entry which has the
	 * bits set
	 */
	while (1) {
		state = rb_entry(node, struct extent_state, rb_node);
		if (state->end >= start && !(state->state & bits)) {
			*end_ret = state->end;
		} else {
			*end_ret = state->start - 1;
			break;
		}

		node = rb_next(node);
		if (!node)
			break;
	}
out:
	spin_unlock(&tree->lock);
}

/*
 * find a contiguous range of bytes in the file marked as delalloc, not
 * more than 'max_bytes'.  start and end are used to return the range,
 *
 * true is returned if we find something, false if nothing was in the tree
 */
bool btrfs_find_delalloc_range(struct extent_io_tree *tree, u64 *start,
			       u64 *end, u64 max_bytes,
			       struct extent_state **cached_state)
{
	struct rb_node *node;
	struct extent_state *state;
	u64 cur_start = *start;
	bool found = false;
	u64 total_bytes = 0;

	spin_lock(&tree->lock);

	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search(tree, cur_start);
	if (!node) {
		*end = (u64)-1;
		goto out;
	}

	while (1) {
		state = rb_entry(node, struct extent_state, rb_node);
		if (found && (state->start != cur_start ||
			      (state->state & EXTENT_BOUNDARY))) {
			goto out;
		}
		if (!(state->state & EXTENT_DELALLOC)) {
			if (!found)
				*end = state->end;
			goto out;
		}
		if (!found) {
			*start = state->start;
			*cached_state = state;
			refcount_inc(&state->refs);
		}
		found = true;
		*end = state->end;
		cur_start = state->end + 1;
		node = rb_next(node);
		total_bytes += state->end - state->start + 1;
		if (total_bytes >= max_bytes)
			break;
		if (!node)
			break;
	}
out:
	spin_unlock(&tree->lock);
	return found;
}

/*
 * count the number of bytes in the tree that have a given bit(s)
 * set.  This can be fairly slow, except for EXTENT_DIRTY which is
 * cached.  The total number found is returned.
 */
u64 count_range_bits(struct extent_io_tree *tree,
		     u64 *start, u64 search_end, u64 max_bytes,
		     unsigned bits, int contig)
{
	struct rb_node *node;
	struct extent_state *state;
	u64 cur_start = *start;
	u64 total_bytes = 0;
	u64 last = 0;
	int found = 0;

	if (WARN_ON(search_end <= cur_start))
		return 0;

	spin_lock(&tree->lock);
	if (cur_start == 0 && bits == EXTENT_DIRTY) {
		total_bytes = tree->dirty_bytes;
		goto out;
	}
	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search(tree, cur_start);
	if (!node)
		goto out;

	while (1) {
		state = rb_entry(node, struct extent_state, rb_node);
		if (state->start > search_end)
			break;
		if (contig && found && state->start > last + 1)
			break;
		if (state->end >= cur_start && (state->state & bits) == bits) {
			total_bytes += min(search_end, state->end) + 1 -
				       max(cur_start, state->start);
			if (total_bytes >= max_bytes)
				break;
			if (!found) {
				*start = max(cur_start, state->start);
				found = 1;
			}
			last = state->end;
		} else if (contig && found) {
			break;
		}
		node = rb_next(node);
		if (!node)
			break;
	}
out:
	spin_unlock(&tree->lock);
	return total_bytes;
}

/*
 * set the private field for a given byte offset in the tree.  If there isn't
 * an extent_state there already, this does nothing.
 */
int set_state_failrec(struct extent_io_tree *tree, u64 start,
		      struct io_failure_record *failrec)
{
	struct rb_node *node;
	struct extent_state *state;
	int ret = 0;

	spin_lock(&tree->lock);
	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search(tree, start);
	if (!node) {
		ret = -ENOENT;
		goto out;
	}
	state = rb_entry(node, struct extent_state, rb_node);
	if (state->start != start) {
		ret = -ENOENT;
		goto out;
	}
	state->failrec = failrec;
out:
	spin_unlock(&tree->lock);
	return ret;
}

int get_state_failrec(struct extent_io_tree *tree, u64 start,
		      struct io_failure_record **failrec)
{
	struct rb_node *node;
	struct extent_state *state;
	int ret = 0;

	spin_lock(&tree->lock);
	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = tree_search(tree, start);
	if (!node) {
		ret = -ENOENT;
		goto out;
	}
	state = rb_entry(node, struct extent_state, rb_node);
	if (state->start != start) {
		ret = -ENOENT;
		goto out;
	}
	*failrec = state->failrec;
out:
	spin_unlock(&tree->lock);
	return ret;
}

/*
 * searches a range in the state tree for a given mask.
 * If 'filled' == 1, this returns 1 only if every extent in the tree
 * has the bits set.  Otherwise, 1 is returned if any bit in the
 * range is found set.
 */
int test_range_bit(struct extent_io_tree *tree, u64 start, u64 end,
		   unsigned bits, int filled, struct extent_state *cached)
{
	struct extent_state *state = NULL;
	struct rb_node *node;
	int bitset = 0;

	spin_lock(&tree->lock);
	if (cached && extent_state_in_tree(cached) && cached->start <= start &&
	    cached->end > start)
		node = &cached->rb_node;
	else
		node = tree_search(tree, start);
	while (node && start <= end) {
		state = rb_entry(node, struct extent_state, rb_node);

		if (filled && state->start > start) {
			bitset = 0;
			break;
		}

		if (state->start > end)
			break;

		if (state->state & bits) {
			bitset = 1;
			if (!filled)
				break;
		} else if (filled) {
			bitset = 0;
			break;
		}

		if (state->end == (u64)-1)
			break;

		start = state->end + 1;
		if (start > end)
			break;
		node = rb_next(node);
		if (!node) {
			if (filled)
				bitset = 0;
			break;
		}
	}
	spin_unlock(&tree->lock);
	return bitset;
}

int free_io_failure(struct extent_io_tree *failure_tree,
		    struct extent_io_tree *io_tree,
		    struct io_failure_record *rec)
{
	int ret;
	int err = 0;

	set_state_failrec(failure_tree, rec->start, NULL);
	ret = clear_extent_bits(failure_tree, rec->start,
				rec->start + rec->len - 1,
				EXTENT_LOCKED | EXTENT_DIRTY);
	if (ret)
		err = ret;

	ret = clear_extent_bits(io_tree, rec->start,
				rec->start + rec->len - 1,
				EXTENT_DAMAGED);
	if (ret && !err)
		err = ret;

	kfree(rec);
	return err;
}

/*
 * each time an IO finishes, we do a fast check in the IO failure tree
 * to see if we need to process or clean up an io_failure_record
 */
int clean_io_failure(struct btrfs_fs_info *fs_info,
		     struct extent_io_tree *failure_tree,
		     struct extent_io_tree *io_tree, u64 start,
		     struct page *page, u64 ino, unsigned int pg_offset)
{
	u64 private;
	struct io_failure_record *failrec;
	struct extent_state *state;
	int num_copies;
	int ret;

	private = 0;
	ret = count_range_bits(failure_tree, &private, (u64)-1, 1,
			       EXTENT_DIRTY, 0);
	if (!ret)
		return 0;

	ret = get_state_failrec(failure_tree, start, &failrec);
	if (ret)
		return 0;

	BUG_ON(!failrec->this_mirror);

	if (failrec->in_validation) {
		/* there was no real error, just free the record */
		btrfs_debug(fs_info,
			"clean_io_failure: freeing dummy error at %llu",
			failrec->start);
		goto out;
	}
	if (sb_rdonly(fs_info->sb))
		goto out;

	spin_lock(&io_tree->lock);
	state = find_first_extent_bit_state(io_tree,
					    failrec->start,
					    EXTENT_LOCKED);
	spin_unlock(&io_tree->lock);

	if (state && state->start <= failrec->start &&
	    state->end >= failrec->start + failrec->len - 1) {
		num_copies = btrfs_num_copies(fs_info, failrec->logical,
					      failrec->len);
		if (num_copies > 1)  {
			repair_io_failure(fs_info, ino, start, failrec->len,
					  failrec->logical, page, pg_offset,
					  failrec->failed_mirror);
		}
	}

out:
	free_io_failure(failure_tree, io_tree, failrec);

	return 0;
}

/*
 * Can be called when
 * - hold extent lock
 * - under ordered extent
 * - the inode is freeing
 */
void btrfs_free_io_failure_record(struct btrfs_inode *inode, u64 start, u64 end)
{
	struct extent_io_tree *failure_tree = &inode->io_failure_tree;
	struct io_failure_record *failrec;
	struct extent_state *state, *next;

	if (RB_EMPTY_ROOT(&failure_tree->state))
		return;

	spin_lock(&failure_tree->lock);
	state = find_first_extent_bit_state(failure_tree, start, EXTENT_DIRTY);
	while (state) {
		if (state->start > end)
			break;

		ASSERT(state->end <= end);

		next = next_state(state);

		failrec = state->failrec;
		free_extent_state(state);
		kfree(failrec);

		state = next;
	}
	spin_unlock(&failure_tree->lock);
}

int btrfs_get_io_failure_record(struct inode *inode, u64 start, u64 end,
		struct io_failure_record **failrec_ret)
{
	struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);
	struct io_failure_record *failrec;
	struct extent_map *em;
	struct extent_io_tree *failure_tree = &BTRFS_I(inode)->io_failure_tree;
	struct extent_io_tree *tree = &BTRFS_I(inode)->io_tree;
	struct extent_map_tree *em_tree = &BTRFS_I(inode)->extent_tree;
	int ret;
	u64 logical;

	ret = get_state_failrec(failure_tree, start, &failrec);
	if (ret) {
		failrec = kzalloc(sizeof(*failrec), GFP_NOFS);
		if (!failrec)
			return -ENOMEM;

		failrec->start = start;
		failrec->len = end - start + 1;
		failrec->this_mirror = 0;
		failrec->bio_flags = 0;
		failrec->in_validation = 0;

		read_lock(&em_tree->lock);
		em = lookup_extent_mapping(em_tree, start, failrec->len);
		if (!em) {
			read_unlock(&em_tree->lock);
			kfree(failrec);
			return -EIO;
		}

		if (em->start > start || em->start + em->len <= start) {
			free_extent_map(em);
			em = NULL;
		}
		read_unlock(&em_tree->lock);
		if (!em) {
			kfree(failrec);
			return -EIO;
		}

		logical = start - em->start;
		logical = em->block_start + logical;
		if (test_bit(EXTENT_FLAG_COMPRESSED, &em->flags)) {
			logical = em->block_start;
			failrec->bio_flags = EXTENT_BIO_COMPRESSED;
			extent_set_compress_type(&failrec->bio_flags,
						 em->compress_type);
		}

		btrfs_debug(fs_info,
			"Get IO Failure Record: (new) logical=%llu, start=%llu, len=%llu",
			logical, start, failrec->len);

		failrec->logical = logical;
		free_extent_map(em);

		/* set the bits in the private failure tree */
		ret = set_extent_bits(failure_tree, start, end,
					EXTENT_LOCKED | EXTENT_DIRTY);
		if (ret >= 0)
			ret = set_state_failrec(failure_tree, start, failrec);
		/* set the bits in the inode's tree */
		if (ret >= 0)
			ret = set_extent_bits(tree, start, end, EXTENT_DAMAGED);
		if (ret < 0) {
			kfree(failrec);
			return ret;
		}
	} else {
		btrfs_debug(fs_info,
			"Get IO Failure Record: (found) logical=%llu, start=%llu, len=%llu, validation=%d",
			failrec->logical, failrec->start, failrec->len,
			failrec->in_validation);
		/*
		 * when data can be on disk more than twice, add to failrec here
		 * (e.g. with a list for failed_mirror) to make
		 * clean_io_failure() clean all those errors at once.
		 */
	}

	*failrec_ret = failrec;

	return 0;
}

