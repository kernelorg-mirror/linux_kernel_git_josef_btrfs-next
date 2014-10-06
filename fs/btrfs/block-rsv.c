/*
 * Copyright (C) 2014 Facebook.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/ratelimit.h>
#include "block-group.h"
#include "transaction.h"
#include "math.h"
#include "qgroup.h"
#include "block-rsv.h"

static int block_rsv_use_bytes(struct btrfs_block_rsv *block_rsv,
			       u64 num_bytes);

static inline u64 heads_to_leaves(struct btrfs_root *root, u64 heads)
{
	u64 num_bytes;

	num_bytes = heads * (sizeof(struct btrfs_extent_item) +
			     sizeof(struct btrfs_extent_inline_ref));
	if (!btrfs_fs_incompat(root->fs_info, SKINNY_METADATA))
		num_bytes += heads * sizeof(struct btrfs_tree_block_info);

	/*
	 * We don't ever fill up leaves all the way so multiply by 2 just to be
	 * closer to what we're really going to want to ouse.
	 */
	return div64_u64(num_bytes, BTRFS_LEAF_DATA_SIZE(root));
}

int btrfs_check_space_for_delayed_refs(struct btrfs_trans_handle *trans,
				       struct btrfs_root *root)
{
	struct btrfs_block_rsv *global_rsv;
	u64 num_heads = trans->transaction->delayed_refs.num_heads_ready;
	u64 num_bytes;
	int ret = 0;

	num_bytes = btrfs_calc_trans_metadata_size(root, 1);
	num_heads = heads_to_leaves(root, num_heads);
	if (num_heads > 1)
		num_bytes += (num_heads - 1) * root->nodesize;
	num_bytes <<= 1;
	global_rsv = &root->fs_info->global_block_rsv;

	/*
	 * If we can't allocate any more chunks lets make sure we have _lots_ of
	 * wiggle room since running delayed refs can create more delayed refs.
	 */
	if (global_rsv->space_info->full)
		num_bytes <<= 1;

	spin_lock(&global_rsv->lock);
	if (global_rsv->reserved <= num_bytes)
		ret = 1;
	spin_unlock(&global_rsv->lock);
	return ret;
}


static int can_overcommit(struct btrfs_root *root,
			  struct btrfs_space_info *space_info, u64 bytes,
			  enum btrfs_reserve_flush_enum flush)
{
	struct btrfs_block_rsv *global_rsv = &root->fs_info->global_block_rsv;
	u64 profile = btrfs_get_alloc_profile(root, 0);
	u64 space_size;
	u64 avail;
	u64 used;

	used = space_info->bytes_used + space_info->bytes_reserved +
		space_info->bytes_pinned + space_info->bytes_readonly;

	/*
	 * We only want to allow over committing if we have lots of actual space
	 * free, but if we don't have enough space to handle the global reserve
	 * space then we could end up having a real enospc problem when trying
	 * to allocate a chunk or some other such important allocation.
	 */
	spin_lock(&global_rsv->lock);
	space_size = calc_global_rsv_need_space(global_rsv);
	spin_unlock(&global_rsv->lock);
	if (used + space_size >= space_info->total_bytes)
		return 0;

	used += space_info->bytes_may_use;

	spin_lock(&root->fs_info->free_chunk_lock);
	avail = root->fs_info->free_chunk_space;
	spin_unlock(&root->fs_info->free_chunk_lock);

	/*
	 * If we have dup, raid1 or raid10 then only half of the free
	 * space is actually useable.  For raid56, the space info used
	 * doesn't include the parity drive, so we don't have to
	 * change the math
	 */
	if (profile & (BTRFS_BLOCK_GROUP_DUP |
		       BTRFS_BLOCK_GROUP_RAID1 |
		       BTRFS_BLOCK_GROUP_RAID10))
		avail >>= 1;

	/*
	 * If we aren't flushing all things, let us overcommit up to
	 * 1/2th of the space. If we can flush, don't let us overcommit
	 * too much, let it overcommit up to 1/8 of the space.
	 */
	if (flush == BTRFS_RESERVE_FLUSH_ALL)
		avail >>= 3;
	else
		avail >>= 1;

	if (used + bytes < space_info->total_bytes + avail)
		return 1;
	return 0;
}

static void btrfs_writeback_inodes_sb_nr(struct btrfs_root *root,
					 unsigned long nr_pages, int nr_items)
{
	struct super_block *sb = root->fs_info->sb;

	if (down_read_trylock(&sb->s_umount)) {
		writeback_inodes_sb_nr(sb, nr_pages, WB_REASON_FS_FREE_SPACE);
		up_read(&sb->s_umount);
	} else {
		/*
		 * We needn't worry the filesystem going from r/w to r/o though
		 * we don't acquire ->s_umount mutex, because the filesystem
		 * should guarantee the delalloc inodes list be empty after
		 * the filesystem is readonly(all dirty pages are written to
		 * the disk).
		 */
		btrfs_start_delalloc_roots(root->fs_info, 0, nr_items);
		if (!current->journal_info)
			btrfs_wait_ordered_roots(root->fs_info, nr_items);
	}
}

static inline int calc_reclaim_items_nr(struct btrfs_root *root, u64 to_reclaim)
{
	u64 bytes;
	int nr;

	bytes = btrfs_calc_trans_metadata_size(root, 1);
	nr = (int)div64_u64(to_reclaim, bytes);
	if (!nr)
		nr = 1;
	return nr;
}

#define EXTENT_SIZE_PER_ITEM	(256 * 1024)

/*
 * shrink metadata reservation for delalloc
 */
static void shrink_delalloc(struct btrfs_root *root, u64 to_reclaim, u64 orig,
			    bool wait_ordered)
{
	struct btrfs_block_rsv *block_rsv;
	struct btrfs_space_info *space_info;
	struct btrfs_trans_handle *trans;
	u64 delalloc_bytes;
	u64 max_reclaim;
	long time_left;
	unsigned long nr_pages;
	int loops;
	int items;
	enum btrfs_reserve_flush_enum flush;

	/* Calc the number of the pages we need flush for space reservation */
	items = calc_reclaim_items_nr(root, to_reclaim);
	to_reclaim = items * EXTENT_SIZE_PER_ITEM;

	trans = (struct btrfs_trans_handle *)current->journal_info;
	block_rsv = &root->fs_info->delalloc_block_rsv;
	space_info = block_rsv->space_info;

	delalloc_bytes = percpu_counter_sum_positive(
						&root->fs_info->delalloc_bytes);
	if (delalloc_bytes == 0) {
		if (trans)
			return;
		if (wait_ordered)
			btrfs_wait_ordered_roots(root->fs_info, items);
		return;
	}

	loops = 0;
	while (delalloc_bytes && loops < 3) {
		max_reclaim = min(delalloc_bytes, to_reclaim);
		nr_pages = max_reclaim >> PAGE_CACHE_SHIFT;
		btrfs_writeback_inodes_sb_nr(root, nr_pages, items);
		/*
		 * We need to wait for the async pages to actually start before
		 * we do anything.
		 */
		max_reclaim = atomic_read(&root->fs_info->async_delalloc_pages);
		if (!max_reclaim)
			goto skip_async;

		if (max_reclaim <= nr_pages)
			max_reclaim = 0;
		else
			max_reclaim -= nr_pages;

		wait_event(root->fs_info->async_submit_wait,
			   atomic_read(&root->fs_info->async_delalloc_pages) <=
			   (int)max_reclaim);
skip_async:
		if (!trans)
			flush = BTRFS_RESERVE_FLUSH_ALL;
		else
			flush = BTRFS_RESERVE_NO_FLUSH;
		spin_lock(&space_info->lock);
		if (can_overcommit(root, space_info, orig, flush)) {
			spin_unlock(&space_info->lock);
			break;
		}
		spin_unlock(&space_info->lock);

		loops++;
		if (wait_ordered && !trans) {
			btrfs_wait_ordered_roots(root->fs_info, items);
		} else {
			time_left = schedule_timeout_killable(1);
			if (time_left)
				break;
		}
		delalloc_bytes = percpu_counter_sum_positive(
						&root->fs_info->delalloc_bytes);
	}
}

/**
 * maybe_commit_transaction - possibly commit the transaction if its ok to
 * @root - the root we're allocating for
 * @bytes - the number of bytes we want to reserve
 * @force - force the commit
 *
 * This will check to make sure that committing the transaction will actually
 * get us somewhere and then commit the transaction if it does.  Otherwise it
 * will return -ENOSPC.
 */
static int may_commit_transaction(struct btrfs_root *root,
				  struct btrfs_space_info *space_info,
				  u64 bytes, int force)
{
	struct btrfs_block_rsv *delayed_rsv = &root->fs_info->delayed_block_rsv;
	struct btrfs_trans_handle *trans;

	trans = (struct btrfs_trans_handle *)current->journal_info;
	if (trans)
		return -EAGAIN;

	if (force)
		goto commit;

	/* See if there is enough pinned space to make this reservation */
	if (percpu_counter_compare(&space_info->total_bytes_pinned,
				   bytes) >= 0)
		goto commit;

	/*
	 * See if there is some space in the delayed insertion reservation for
	 * this reservation.
	 */
	if (space_info != delayed_rsv->space_info)
		return -ENOSPC;

	spin_lock(&delayed_rsv->lock);
	if (percpu_counter_compare(&space_info->total_bytes_pinned,
				   bytes - delayed_rsv->size) >= 0) {
		spin_unlock(&delayed_rsv->lock);
		return -ENOSPC;
	}
	spin_unlock(&delayed_rsv->lock);

commit:
	trans = btrfs_join_transaction(root);
	if (IS_ERR(trans))
		return -ENOSPC;

	return btrfs_commit_transaction(trans, root);
}

enum flush_state {
	FLUSH_DELAYED_ITEMS_NR	=	1,
	FLUSH_DELAYED_ITEMS	=	2,
	FLUSH_DELALLOC		=	3,
	FLUSH_DELALLOC_WAIT	=	4,
	ALLOC_CHUNK		=	5,
	COMMIT_TRANS		=	6,
};

static int flush_space(struct btrfs_root *root,
		       struct btrfs_space_info *space_info, u64 num_bytes,
		       u64 orig_bytes, int state)
{
	struct btrfs_trans_handle *trans;
	int nr;
	int ret = 0;

	switch (state) {
	case FLUSH_DELAYED_ITEMS_NR:
	case FLUSH_DELAYED_ITEMS:
		if (state == FLUSH_DELAYED_ITEMS_NR)
			nr = calc_reclaim_items_nr(root, num_bytes) * 2;
		else
			nr = -1;

		trans = btrfs_join_transaction(root);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			break;
		}
		ret = btrfs_run_delayed_items_nr(trans, root, nr);
		btrfs_end_transaction(trans, root);
		break;
	case FLUSH_DELALLOC:
	case FLUSH_DELALLOC_WAIT:
		shrink_delalloc(root, num_bytes * 2, orig_bytes,
				state == FLUSH_DELALLOC_WAIT);
		break;
	case ALLOC_CHUNK:
		trans = btrfs_join_transaction(root);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			break;
		}
		ret = do_chunk_alloc(trans, root->fs_info->extent_root,
				     btrfs_get_alloc_profile(root, 0),
				     CHUNK_ALLOC_NO_FORCE);
		btrfs_end_transaction(trans, root);
		if (ret == -ENOSPC)
			ret = 0;
		break;
	case COMMIT_TRANS:
		ret = may_commit_transaction(root, space_info, orig_bytes, 0);
		break;
	default:
		ret = -ENOSPC;
		break;
	}

	return ret;
}

static inline u64
btrfs_calc_reclaim_metadata_size(struct btrfs_root *root,
				 struct btrfs_space_info *space_info)
{
	u64 used;
	u64 expected;
	u64 to_reclaim;

	to_reclaim = min_t(u64, num_online_cpus() * 1024 * 1024,
				16 * 1024 * 1024);
	spin_lock(&space_info->lock);
	if (can_overcommit(root, space_info, to_reclaim,
			   BTRFS_RESERVE_FLUSH_ALL)) {
		to_reclaim = 0;
		goto out;
	}

	used = space_info->bytes_used + space_info->bytes_reserved +
	       space_info->bytes_pinned + space_info->bytes_readonly +
	       space_info->bytes_may_use;
	if (can_overcommit(root, space_info, 1024 * 1024,
			   BTRFS_RESERVE_FLUSH_ALL))
		expected = div_factor_fine(space_info->total_bytes, 95);
	else
		expected = div_factor_fine(space_info->total_bytes, 90);

	if (used > expected)
		to_reclaim = used - expected;
	else
		to_reclaim = 0;
	to_reclaim = min(to_reclaim, space_info->bytes_may_use +
				     space_info->bytes_reserved);
out:
	spin_unlock(&space_info->lock);

	return to_reclaim;
}

static inline int need_do_async_reclaim(struct btrfs_space_info *space_info,
					struct btrfs_fs_info *fs_info, u64 used)
{
	return (used >= div_factor_fine(space_info->total_bytes, 98) &&
		!btrfs_fs_closing(fs_info) &&
		!test_bit(BTRFS_FS_STATE_REMOUNTING, &fs_info->fs_state));
}

static int btrfs_need_do_async_reclaim(struct btrfs_space_info *space_info,
				       struct btrfs_fs_info *fs_info,
				       int flush_state)
{
	u64 used;

	spin_lock(&space_info->lock);
	/*
	 * We run out of space and have not got any free space via flush_space,
	 * so don't bother doing async reclaim.
	 */
	if (flush_state > COMMIT_TRANS && space_info->full) {
		spin_unlock(&space_info->lock);
		return 0;
	}

	used = space_info->bytes_used + space_info->bytes_reserved +
	       space_info->bytes_pinned + space_info->bytes_readonly +
	       space_info->bytes_may_use;
	if (need_do_async_reclaim(space_info, fs_info, used)) {
		spin_unlock(&space_info->lock);
		return 1;
	}
	spin_unlock(&space_info->lock);

	return 0;
}

static void btrfs_async_reclaim_metadata_space(struct work_struct *work)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_space_info *space_info;
	u64 to_reclaim;
	int flush_state;

	fs_info = container_of(work, struct btrfs_fs_info, async_reclaim_work);
	space_info = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_METADATA);

	to_reclaim = btrfs_calc_reclaim_metadata_size(fs_info->fs_root,
						      space_info);
	if (!to_reclaim)
		return;

	flush_state = FLUSH_DELAYED_ITEMS_NR;
	do {
		flush_space(fs_info->fs_root, space_info, to_reclaim,
			    to_reclaim, flush_state);
		flush_state++;
		if (!btrfs_need_do_async_reclaim(space_info, fs_info,
						 flush_state))
			return;
	} while (flush_state <= COMMIT_TRANS);

	if (btrfs_need_do_async_reclaim(space_info, fs_info, flush_state))
		queue_work(system_unbound_wq, work);
}

void btrfs_init_async_reclaim_work(struct work_struct *work)
{
	INIT_WORK(work, btrfs_async_reclaim_metadata_space);
}

/**
 * reserve_metadata_bytes - try to reserve bytes from the block_rsv's space
 * @root - the root we're allocating for
 * @block_rsv - the block_rsv we're allocating for
 * @orig_bytes - the number of bytes we want
 * @flush - whether or not we can flush to make our reservation
 *
 * This will reserve orgi_bytes number of bytes from the space info associated
 * with the block_rsv.  If there is not enough space it will make an attempt to
 * flush out space to make room.  It will do this by flushing delalloc if
 * possible or committing the transaction.  If flush is 0 then no attempts to
 * regain reservations will be made and this will fail if there is not enough
 * space already.
 */
static int reserve_metadata_bytes(struct btrfs_root *root,
				  struct btrfs_block_rsv *block_rsv,
				  u64 orig_bytes,
				  enum btrfs_reserve_flush_enum flush)
{
	struct btrfs_space_info *space_info = block_rsv->space_info;
	u64 used;
	u64 num_bytes = orig_bytes;
	int flush_state = FLUSH_DELAYED_ITEMS_NR;
	int ret = 0;
	bool flushing = false;

again:
	ret = 0;
	spin_lock(&space_info->lock);
	/*
	 * We only want to wait if somebody other than us is flushing and we
	 * are actually allowed to flush all things.
	 */
	while (flush == BTRFS_RESERVE_FLUSH_ALL && !flushing &&
	       space_info->flush) {
		spin_unlock(&space_info->lock);
		/*
		 * If we have a trans handle we can't wait because the flusher
		 * may have to commit the transaction, which would mean we would
		 * deadlock since we are waiting for the flusher to finish, but
		 * hold the current transaction open.
		 */
		if (current->journal_info)
			return -EAGAIN;
		ret = wait_event_killable(space_info->wait, !space_info->flush);
		/* Must have been killed, return */
		if (ret)
			return -EINTR;

		spin_lock(&space_info->lock);
	}

	ret = -ENOSPC;
	used = space_info->bytes_used + space_info->bytes_reserved +
		space_info->bytes_pinned + space_info->bytes_readonly +
		space_info->bytes_may_use;

	/*
	 * The idea here is that we've not already over-reserved the block group
	 * then we can go ahead and save our reservation first and then start
	 * flushing if we need to.  Otherwise if we've already overcommitted
	 * lets start flushing stuff first and then come back and try to make
	 * our reservation.
	 */
	if (used <= space_info->total_bytes) {
		if (used + orig_bytes <= space_info->total_bytes) {
			space_info->bytes_may_use += orig_bytes;
			trace_btrfs_space_reservation(root->fs_info,
				"space_info", space_info->flags, orig_bytes, 1);
			ret = 0;
		} else {
			/*
			 * Ok set num_bytes to orig_bytes since we aren't
			 * overocmmitted, this way we only try and reclaim what
			 * we need.
			 */
			num_bytes = orig_bytes;
		}
	} else {
		/*
		 * Ok we're over committed, set num_bytes to the overcommitted
		 * amount plus the amount of bytes that we need for this
		 * reservation.
		 */
		num_bytes = used - space_info->total_bytes +
			(orig_bytes * 2);
	}

	if (ret && can_overcommit(root, space_info, orig_bytes, flush)) {
		space_info->bytes_may_use += orig_bytes;
		trace_btrfs_space_reservation(root->fs_info, "space_info",
					      space_info->flags, orig_bytes,
					      1);
		ret = 0;
	}

	/*
	 * Couldn't make our reservation, save our place so while we're trying
	 * to reclaim space we can actually use it instead of somebody else
	 * stealing it from us.
	 *
	 * We make the other tasks wait for the flush only when we can flush
	 * all things.
	 */
	if (ret && flush != BTRFS_RESERVE_NO_FLUSH) {
		flushing = true;
		space_info->flush = 1;
	} else if (!ret && space_info->flags & BTRFS_BLOCK_GROUP_METADATA) {
		used += orig_bytes;
		/*
		 * We will do the space reservation dance during log replay,
		 * which means we won't have fs_info->fs_root set, so don't do
		 * the async reclaim as we will panic.
		 */
		if (!root->fs_info->log_root_recovering &&
		    need_do_async_reclaim(space_info, root->fs_info, used) &&
		    !work_busy(&root->fs_info->async_reclaim_work))
			queue_work(system_unbound_wq,
				   &root->fs_info->async_reclaim_work);
	}
	spin_unlock(&space_info->lock);

	if (!ret || flush == BTRFS_RESERVE_NO_FLUSH)
		goto out;

	ret = flush_space(root, space_info, num_bytes, orig_bytes,
			  flush_state);
	flush_state++;

	/*
	 * If we are FLUSH_LIMIT, we can not flush delalloc, or the deadlock
	 * would happen. So skip delalloc flush.
	 */
	if (flush == BTRFS_RESERVE_FLUSH_LIMIT &&
	    (flush_state == FLUSH_DELALLOC ||
	     flush_state == FLUSH_DELALLOC_WAIT))
		flush_state = ALLOC_CHUNK;

	if (!ret)
		goto again;
	else if (flush == BTRFS_RESERVE_FLUSH_LIMIT &&
		 flush_state < COMMIT_TRANS)
		goto again;
	else if (flush == BTRFS_RESERVE_FLUSH_ALL &&
		 flush_state <= COMMIT_TRANS)
		goto again;

out:
	if (ret == -ENOSPC &&
	    unlikely(root->orphan_cleanup_state == ORPHAN_CLEANUP_STARTED)) {
		struct btrfs_block_rsv *global_rsv =
			&root->fs_info->global_block_rsv;

		if (block_rsv != global_rsv &&
		    !block_rsv_use_bytes(global_rsv, orig_bytes))
			ret = 0;
	}
	if (ret == -ENOSPC)
		trace_btrfs_space_reservation(root->fs_info,
					      "space_info:enospc",
					      space_info->flags, orig_bytes, 1);
	if (flushing) {
		spin_lock(&space_info->lock);
		space_info->flush = 0;
		wake_up_all(&space_info->wait);
		spin_unlock(&space_info->lock);
	}
	return ret;
}

static struct btrfs_block_rsv *get_block_rsv(
					const struct btrfs_trans_handle *trans,
					const struct btrfs_root *root)
{
	struct btrfs_block_rsv *block_rsv = NULL;

	if (test_bit(BTRFS_ROOT_REF_COWS, &root->state))
		block_rsv = trans->block_rsv;

	if (root == root->fs_info->csum_root && trans->adding_csums)
		block_rsv = trans->block_rsv;

	if (root == root->fs_info->uuid_root)
		block_rsv = trans->block_rsv;

	if (!block_rsv)
		block_rsv = root->block_rsv;

	if (!block_rsv)
		block_rsv = &root->fs_info->empty_block_rsv;

	return block_rsv;
}

static int block_rsv_use_bytes(struct btrfs_block_rsv *block_rsv,
			       u64 num_bytes)
{
	int ret = -ENOSPC;
	spin_lock(&block_rsv->lock);
	if (block_rsv->reserved >= num_bytes) {
		block_rsv->reserved -= num_bytes;
		if (block_rsv->reserved < block_rsv->size)
			block_rsv->full = 0;
		ret = 0;
	}
	spin_unlock(&block_rsv->lock);
	return ret;
}

static void block_rsv_add_bytes(struct btrfs_block_rsv *block_rsv,
				u64 num_bytes, int update_size)
{
	spin_lock(&block_rsv->lock);
	block_rsv->reserved += num_bytes;
	if (update_size)
		block_rsv->size += num_bytes;
	else if (block_rsv->reserved >= block_rsv->size)
		block_rsv->full = 1;
	spin_unlock(&block_rsv->lock);
}

int btrfs_cond_migrate_bytes(struct btrfs_fs_info *fs_info,
			     struct btrfs_block_rsv *dest, u64 num_bytes,
			     int min_factor)
{
	struct btrfs_block_rsv *global_rsv = &fs_info->global_block_rsv;
	u64 min_bytes;

	if (global_rsv->space_info != dest->space_info)
		return -ENOSPC;

	spin_lock(&global_rsv->lock);
	min_bytes = div_factor(global_rsv->size, min_factor);
	if (global_rsv->reserved < min_bytes + num_bytes) {
		spin_unlock(&global_rsv->lock);
		return -ENOSPC;
	}
	global_rsv->reserved -= num_bytes;
	if (global_rsv->reserved < global_rsv->size)
		global_rsv->full = 0;
	spin_unlock(&global_rsv->lock);

	block_rsv_add_bytes(dest, num_bytes, 1);
	return 0;
}

static void block_rsv_release_bytes(struct btrfs_fs_info *fs_info,
				    struct btrfs_block_rsv *block_rsv,
				    struct btrfs_block_rsv *dest, u64 num_bytes)
{
	struct btrfs_space_info *space_info = block_rsv->space_info;

	spin_lock(&block_rsv->lock);
	if (num_bytes == (u64)-1)
		num_bytes = block_rsv->size;
	block_rsv->size -= num_bytes;
	if (block_rsv->reserved >= block_rsv->size) {
		num_bytes = block_rsv->reserved - block_rsv->size;
		block_rsv->reserved = block_rsv->size;
		block_rsv->full = 1;
	} else {
		num_bytes = 0;
	}
	spin_unlock(&block_rsv->lock);

	if (num_bytes > 0) {
		if (dest) {
			spin_lock(&dest->lock);
			if (!dest->full) {
				u64 bytes_to_add;

				bytes_to_add = dest->size - dest->reserved;
				bytes_to_add = min(num_bytes, bytes_to_add);
				dest->reserved += bytes_to_add;
				if (dest->reserved >= dest->size)
					dest->full = 1;
				num_bytes -= bytes_to_add;
			}
			spin_unlock(&dest->lock);
		}
		if (num_bytes) {
			spin_lock(&space_info->lock);
			space_info->bytes_may_use -= num_bytes;
			trace_btrfs_space_reservation(fs_info, "space_info",
					space_info->flags, num_bytes, 0);
			spin_unlock(&space_info->lock);
		}
	}
}

static int block_rsv_migrate_bytes(struct btrfs_block_rsv *src,
				   struct btrfs_block_rsv *dst, u64 num_bytes)
{
	int ret;

	ret = block_rsv_use_bytes(src, num_bytes);
	if (ret)
		return ret;

	block_rsv_add_bytes(dst, num_bytes, 1);
	return 0;
}

void btrfs_init_block_rsv(struct btrfs_block_rsv *rsv, unsigned short type)
{
	memset(rsv, 0, sizeof(*rsv));
	spin_lock_init(&rsv->lock);
	rsv->type = type;
}

struct btrfs_block_rsv *btrfs_alloc_block_rsv(struct btrfs_root *root,
					      unsigned short type)
{
	struct btrfs_block_rsv *block_rsv;
	struct btrfs_fs_info *fs_info = root->fs_info;

	block_rsv = kmalloc(sizeof(*block_rsv), GFP_NOFS);
	if (!block_rsv)
		return NULL;

	btrfs_init_block_rsv(block_rsv, type);
	block_rsv->space_info = __find_space_info(fs_info,
						  BTRFS_BLOCK_GROUP_METADATA);
	return block_rsv;
}

void btrfs_free_block_rsv(struct btrfs_root *root,
			  struct btrfs_block_rsv *rsv)
{
	if (!rsv)
		return;
	btrfs_block_rsv_release(root, rsv, (u64)-1);
	kfree(rsv);
}

int btrfs_block_rsv_add(struct btrfs_root *root,
			struct btrfs_block_rsv *block_rsv, u64 num_bytes,
			enum btrfs_reserve_flush_enum flush)
{
	int ret;

	if (num_bytes == 0)
		return 0;

	ret = reserve_metadata_bytes(root, block_rsv, num_bytes, flush);
	if (!ret) {
		block_rsv_add_bytes(block_rsv, num_bytes, 1);
		return 0;
	}

	return ret;
}

int btrfs_block_rsv_check(struct btrfs_root *root,
			  struct btrfs_block_rsv *block_rsv, int min_factor)
{
	u64 num_bytes = 0;
	int ret = -ENOSPC;

	if (!block_rsv)
		return 0;

	spin_lock(&block_rsv->lock);
	num_bytes = div_factor(block_rsv->size, min_factor);
	if (block_rsv->reserved >= num_bytes)
		ret = 0;
	spin_unlock(&block_rsv->lock);

	return ret;
}

int btrfs_block_rsv_refill(struct btrfs_root *root,
			   struct btrfs_block_rsv *block_rsv, u64 min_reserved,
			   enum btrfs_reserve_flush_enum flush)
{
	u64 num_bytes = 0;
	int ret = -ENOSPC;

	if (!block_rsv)
		return 0;

	spin_lock(&block_rsv->lock);
	num_bytes = min_reserved;
	if (block_rsv->reserved >= num_bytes)
		ret = 0;
	else
		num_bytes -= block_rsv->reserved;
	spin_unlock(&block_rsv->lock);

	if (!ret)
		return 0;

	ret = reserve_metadata_bytes(root, block_rsv, num_bytes, flush);
	if (!ret) {
		block_rsv_add_bytes(block_rsv, num_bytes, 0);
		return 0;
	}

	return ret;
}

int btrfs_block_rsv_migrate(struct btrfs_block_rsv *src_rsv,
			    struct btrfs_block_rsv *dst_rsv,
			    u64 num_bytes)
{
	return block_rsv_migrate_bytes(src_rsv, dst_rsv, num_bytes);
}

void btrfs_block_rsv_release(struct btrfs_root *root,
			     struct btrfs_block_rsv *block_rsv,
			     u64 num_bytes)
{
	struct btrfs_block_rsv *global_rsv = &root->fs_info->global_block_rsv;
	if (global_rsv == block_rsv ||
	    block_rsv->space_info != global_rsv->space_info)
		global_rsv = NULL;
	block_rsv_release_bytes(root->fs_info, block_rsv, global_rsv,
				num_bytes);
}

/*
 * helper to calculate size of global block reservation.
 * the desired value is sum of space used by extent tree,
 * checksum tree and root tree
 */
static u64 calc_global_metadata_size(struct btrfs_fs_info *fs_info)
{
	struct btrfs_space_info *sinfo;
	u64 num_bytes;
	u64 meta_used;
	u64 data_used;
	int csum_size = btrfs_super_csum_size(fs_info->super_copy);

	sinfo = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_DATA);
	spin_lock(&sinfo->lock);
	data_used = sinfo->bytes_used;
	spin_unlock(&sinfo->lock);

	sinfo = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_METADATA);
	spin_lock(&sinfo->lock);
	if (sinfo->flags & BTRFS_BLOCK_GROUP_DATA)
		data_used = 0;
	meta_used = sinfo->bytes_used;
	spin_unlock(&sinfo->lock);

	num_bytes = (data_used >> fs_info->sb->s_blocksize_bits) *
		    csum_size * 2;
	num_bytes += div64_u64(data_used + meta_used, 50);

	if (num_bytes * 3 > meta_used)
		num_bytes = div64_u64(meta_used, 3);

	return ALIGN(num_bytes, fs_info->extent_root->nodesize << 10);
}

void update_global_block_rsv(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_rsv *block_rsv = &fs_info->global_block_rsv;
	struct btrfs_space_info *sinfo = block_rsv->space_info;
	u64 num_bytes;

	num_bytes = calc_global_metadata_size(fs_info);

	spin_lock(&sinfo->lock);
	spin_lock(&block_rsv->lock);

	block_rsv->size = min_t(u64, num_bytes, 512 * 1024 * 1024);

	num_bytes = sinfo->bytes_used + sinfo->bytes_pinned +
		    sinfo->bytes_reserved + sinfo->bytes_readonly +
		    sinfo->bytes_may_use;

	if (sinfo->total_bytes > num_bytes) {
		num_bytes = sinfo->total_bytes - num_bytes;
		block_rsv->reserved += num_bytes;
		sinfo->bytes_may_use += num_bytes;
		trace_btrfs_space_reservation(fs_info, "space_info",
				      sinfo->flags, num_bytes, 1);
	}

	if (block_rsv->reserved >= block_rsv->size) {
		num_bytes = block_rsv->reserved - block_rsv->size;
		sinfo->bytes_may_use -= num_bytes;
		trace_btrfs_space_reservation(fs_info, "space_info",
				      sinfo->flags, num_bytes, 0);
		block_rsv->reserved = block_rsv->size;
		block_rsv->full = 1;
	}

	spin_unlock(&block_rsv->lock);
	spin_unlock(&sinfo->lock);
}

void init_global_block_rsv(struct btrfs_fs_info *fs_info)
{
	struct btrfs_space_info *space_info;

	space_info = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_SYSTEM);
	fs_info->chunk_block_rsv.space_info = space_info;

	space_info = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_METADATA);
	fs_info->global_block_rsv.space_info = space_info;
	fs_info->delalloc_block_rsv.space_info = space_info;
	fs_info->trans_block_rsv.space_info = space_info;
	fs_info->empty_block_rsv.space_info = space_info;
	fs_info->delayed_block_rsv.space_info = space_info;

	fs_info->extent_root->block_rsv = &fs_info->global_block_rsv;
	fs_info->csum_root->block_rsv = &fs_info->global_block_rsv;
	fs_info->dev_root->block_rsv = &fs_info->global_block_rsv;
	fs_info->tree_root->block_rsv = &fs_info->global_block_rsv;
	if (fs_info->quota_root)
		fs_info->quota_root->block_rsv = &fs_info->global_block_rsv;
	fs_info->chunk_root->block_rsv = &fs_info->chunk_block_rsv;

	update_global_block_rsv(fs_info);
}

void release_global_block_rsv(struct btrfs_fs_info *fs_info)
{
	block_rsv_release_bytes(fs_info, &fs_info->global_block_rsv, NULL,
				(u64)-1);
	WARN_ON(fs_info->delalloc_block_rsv.size > 0);
	WARN_ON(fs_info->delalloc_block_rsv.reserved > 0);
	WARN_ON(fs_info->trans_block_rsv.size > 0);
	WARN_ON(fs_info->trans_block_rsv.reserved > 0);
	WARN_ON(fs_info->chunk_block_rsv.size > 0);
	WARN_ON(fs_info->chunk_block_rsv.reserved > 0);
	WARN_ON(fs_info->delayed_block_rsv.size > 0);
	WARN_ON(fs_info->delayed_block_rsv.reserved > 0);
}

void btrfs_trans_release_metadata(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root)
{
	if (!trans->block_rsv)
		return;

	if (!trans->bytes_reserved)
		return;

	trace_btrfs_space_reservation(root->fs_info, "transaction",
				      trans->transid, trans->bytes_reserved, 0);
	btrfs_block_rsv_release(root, trans->block_rsv, trans->bytes_reserved);
	trans->bytes_reserved = 0;
}

/* Can only return 0 or -ENOSPC */
int btrfs_orphan_reserve_metadata(struct btrfs_trans_handle *trans,
				  struct inode *inode)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct btrfs_block_rsv *src_rsv = get_block_rsv(trans, root);
	struct btrfs_block_rsv *dst_rsv = root->orphan_block_rsv;

	/*
	 * We need to hold space in order to delete our orphan item once we've
	 * added it, so this takes the reservation so we can release it later
	 * when we are truly done with the orphan item.
	 */
	u64 num_bytes = btrfs_calc_trans_metadata_size(root, 1);
	trace_btrfs_space_reservation(root->fs_info, "orphan",
				      btrfs_ino(inode), num_bytes, 1);
	return block_rsv_migrate_bytes(src_rsv, dst_rsv, num_bytes);
}

void btrfs_orphan_release_metadata(struct inode *inode)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	u64 num_bytes = btrfs_calc_trans_metadata_size(root, 1);
	trace_btrfs_space_reservation(root->fs_info, "orphan",
				      btrfs_ino(inode), num_bytes, 0);
	btrfs_block_rsv_release(root, root->orphan_block_rsv, num_bytes);
}

/*
 * btrfs_subvolume_reserve_metadata() - reserve space for subvolume operation
 * root: the root of the parent directory
 * rsv: block reservation
 * items: the number of items that we need do reservation
 * qgroup_reserved: used to return the reserved size in qgroup
 *
 * This function is used to reserve the space for snapshot/subvolume
 * creation and deletion. Those operations are different with the
 * common file/directory operations, they change two fs/file trees
 * and root tree, the number of items that the qgroup reserves is
 * different with the free space reservation. So we can not use
 * the space reseravtion mechanism in start_transaction().
 */
int btrfs_subvolume_reserve_metadata(struct btrfs_root *root,
				     struct btrfs_block_rsv *rsv,
				     int items,
				     u64 *qgroup_reserved,
				     bool use_global_rsv)
{
	u64 num_bytes;
	int ret;
	struct btrfs_block_rsv *global_rsv = &root->fs_info->global_block_rsv;

	if (root->fs_info->quota_enabled) {
		/* One for parent inode, two for dir entries */
		num_bytes = 3 * root->nodesize;
		ret = btrfs_qgroup_reserve(root, num_bytes);
		if (ret)
			return ret;
	} else {
		num_bytes = 0;
	}

	*qgroup_reserved = num_bytes;

	num_bytes = btrfs_calc_trans_metadata_size(root, items);
	rsv->space_info = __find_space_info(root->fs_info,
					    BTRFS_BLOCK_GROUP_METADATA);
	ret = btrfs_block_rsv_add(root, rsv, num_bytes,
				  BTRFS_RESERVE_FLUSH_ALL);

	if (ret == -ENOSPC && use_global_rsv)
		ret = btrfs_block_rsv_migrate(global_rsv, rsv, num_bytes);

	if (ret) {
		if (*qgroup_reserved)
			btrfs_qgroup_free(root, *qgroup_reserved);
	}

	return ret;
}

void btrfs_subvolume_release_metadata(struct btrfs_root *root,
				      struct btrfs_block_rsv *rsv,
				      u64 qgroup_reserved)
{
	btrfs_block_rsv_release(root, rsv, (u64)-1);
	if (qgroup_reserved)
		btrfs_qgroup_free(root, qgroup_reserved);
}

/**
 * drop_outstanding_extent - drop an outstanding extent
 * @inode: the inode we're dropping the extent for
 *
 * This is called when we are freeing up an outstanding extent, either called
 * after an error or after an extent is written.  This will return the number of
 * reserved extents that need to be freed.  This must be called with
 * BTRFS_I(inode)->lock held.
 */
static unsigned drop_outstanding_extent(struct inode *inode)
{
	unsigned drop_inode_space = 0;
	unsigned dropped_extents = 0;

	BUG_ON(!BTRFS_I(inode)->outstanding_extents);
	BTRFS_I(inode)->outstanding_extents--;

	if (BTRFS_I(inode)->outstanding_extents == 0 &&
	    test_and_clear_bit(BTRFS_INODE_DELALLOC_META_RESERVED,
			       &BTRFS_I(inode)->runtime_flags))
		drop_inode_space = 1;

	/*
	 * If we have more or the same amount of outsanding extents than we have
	 * reserved then we need to leave the reserved extents count alone.
	 */
	if (BTRFS_I(inode)->outstanding_extents >=
	    BTRFS_I(inode)->reserved_extents)
		return drop_inode_space;

	dropped_extents = BTRFS_I(inode)->reserved_extents -
		BTRFS_I(inode)->outstanding_extents;
	BTRFS_I(inode)->reserved_extents -= dropped_extents;
	return dropped_extents + drop_inode_space;
}

/**
 * calc_csum_metadata_size - return the amount of metada space that must be
 *	reserved/free'd for the given bytes.
 * @inode: the inode we're manipulating
 * @num_bytes: the number of bytes in question
 * @reserve: 1 if we are reserving space, 0 if we are freeing space
 *
 * This adjusts the number of csum_bytes in the inode and then returns the
 * correct amount of metadata that must either be reserved or freed.  We
 * calculate how many checksums we can fit into one leaf and then divide the
 * number of bytes that will need to be checksumed by this value to figure out
 * how many checksums will be required.  If we are adding bytes then the number
 * may go up and we will return the number of additional bytes that must be
 * reserved.  If it is going down we will return the number of bytes that must
 * be freed.
 *
 * This must be called with BTRFS_I(inode)->lock held.
 */
static u64 calc_csum_metadata_size(struct inode *inode, u64 num_bytes,
				   int reserve)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	u64 csum_size;
	int num_csums_per_leaf;
	int num_csums;
	int old_csums;

	if (BTRFS_I(inode)->flags & BTRFS_INODE_NODATASUM &&
	    BTRFS_I(inode)->csum_bytes == 0)
		return 0;

	old_csums = (int)div64_u64(BTRFS_I(inode)->csum_bytes, root->sectorsize);
	if (reserve)
		BTRFS_I(inode)->csum_bytes += num_bytes;
	else
		BTRFS_I(inode)->csum_bytes -= num_bytes;
	csum_size = BTRFS_LEAF_DATA_SIZE(root) - sizeof(struct btrfs_item);
	num_csums_per_leaf = (int)div64_u64(csum_size,
					    sizeof(struct btrfs_csum_item) +
					    sizeof(struct btrfs_disk_key));
	num_csums = (int)div64_u64(BTRFS_I(inode)->csum_bytes, root->sectorsize);
	num_csums = num_csums + num_csums_per_leaf - 1;
	num_csums = num_csums / num_csums_per_leaf;

	old_csums = old_csums + num_csums_per_leaf - 1;
	old_csums = old_csums / num_csums_per_leaf;

	/* No change, no need to reserve more */
	if (old_csums == num_csums)
		return 0;

	if (reserve)
		return btrfs_calc_trans_metadata_size(root,
						      num_csums - old_csums);

	return btrfs_calc_trans_metadata_size(root, old_csums - num_csums);
}

int btrfs_delalloc_reserve_metadata(struct inode *inode, u64 num_bytes)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct btrfs_block_rsv *block_rsv = &root->fs_info->delalloc_block_rsv;
	u64 to_reserve = 0;
	u64 csum_bytes;
	unsigned nr_extents = 0;
	int extra_reserve = 0;
	enum btrfs_reserve_flush_enum flush = BTRFS_RESERVE_FLUSH_ALL;
	int ret = 0;
	bool delalloc_lock = true;
	u64 to_free = 0;
	unsigned dropped;

	/* If we are a free space inode we need to not flush since we will be in
	 * the middle of a transaction commit.  We also don't need the delalloc
	 * mutex since we won't race with anybody.  We need this mostly to make
	 * lockdep shut its filthy mouth.
	 */
	if (btrfs_is_free_space_inode(inode)) {
		flush = BTRFS_RESERVE_NO_FLUSH;
		delalloc_lock = false;
	}

	if (flush != BTRFS_RESERVE_NO_FLUSH &&
	    btrfs_transaction_in_commit(root->fs_info))
		schedule_timeout(1);

	if (delalloc_lock)
		mutex_lock(&BTRFS_I(inode)->delalloc_mutex);

	num_bytes = ALIGN(num_bytes, root->sectorsize);

	spin_lock(&BTRFS_I(inode)->lock);
	BTRFS_I(inode)->outstanding_extents++;

	if (BTRFS_I(inode)->outstanding_extents >
	    BTRFS_I(inode)->reserved_extents)
		nr_extents = BTRFS_I(inode)->outstanding_extents -
			BTRFS_I(inode)->reserved_extents;

	/*
	 * Add an item to reserve for updating the inode when we complete the
	 * delalloc io.
	 */
	if (!test_bit(BTRFS_INODE_DELALLOC_META_RESERVED,
		      &BTRFS_I(inode)->runtime_flags)) {
		nr_extents++;
		extra_reserve = 1;
	}

	to_reserve = btrfs_calc_trans_metadata_size(root, nr_extents);
	to_reserve += calc_csum_metadata_size(inode, num_bytes, 1);
	csum_bytes = BTRFS_I(inode)->csum_bytes;
	spin_unlock(&BTRFS_I(inode)->lock);

	if (root->fs_info->quota_enabled) {
		ret = btrfs_qgroup_reserve(root, num_bytes +
					   nr_extents * root->nodesize);
		if (ret)
			goto out_fail;
	}

	ret = reserve_metadata_bytes(root, block_rsv, to_reserve, flush);
	if (unlikely(ret)) {
		if (root->fs_info->quota_enabled)
			btrfs_qgroup_free(root, num_bytes +
						nr_extents * root->nodesize);
		goto out_fail;
	}

	spin_lock(&BTRFS_I(inode)->lock);
	if (extra_reserve) {
		set_bit(BTRFS_INODE_DELALLOC_META_RESERVED,
			&BTRFS_I(inode)->runtime_flags);
		nr_extents--;
	}
	BTRFS_I(inode)->reserved_extents += nr_extents;
	spin_unlock(&BTRFS_I(inode)->lock);

	if (delalloc_lock)
		mutex_unlock(&BTRFS_I(inode)->delalloc_mutex);

	if (to_reserve)
		trace_btrfs_space_reservation(root->fs_info, "delalloc",
					      btrfs_ino(inode), to_reserve, 1);
	block_rsv_add_bytes(block_rsv, to_reserve, 1);

	return 0;

out_fail:
	spin_lock(&BTRFS_I(inode)->lock);
	dropped = drop_outstanding_extent(inode);
	/*
	 * If the inodes csum_bytes is the same as the original
	 * csum_bytes then we know we haven't raced with any free()ers
	 * so we can just reduce our inodes csum bytes and carry on.
	 */
	if (BTRFS_I(inode)->csum_bytes == csum_bytes) {
		calc_csum_metadata_size(inode, num_bytes, 0);
	} else {
		u64 orig_csum_bytes = BTRFS_I(inode)->csum_bytes;
		u64 bytes;

		/*
		 * This is tricky, but first we need to figure out how much we
		 * free'd from any free-ers that occured during this
		 * reservation, so we reset ->csum_bytes to the csum_bytes
		 * before we dropped our lock, and then call the free for the
		 * number of bytes that were freed while we were trying our
		 * reservation.
		 */
		bytes = csum_bytes - BTRFS_I(inode)->csum_bytes;
		BTRFS_I(inode)->csum_bytes = csum_bytes;
		to_free = calc_csum_metadata_size(inode, bytes, 0);


		/*
		 * Now we need to see how much we would have freed had we not
		 * been making this reservation and our ->csum_bytes were not
		 * artificially inflated.
		 */
		BTRFS_I(inode)->csum_bytes = csum_bytes - num_bytes;
		bytes = csum_bytes - orig_csum_bytes;
		bytes = calc_csum_metadata_size(inode, bytes, 0);

		/*
		 * Now reset ->csum_bytes to what it should be.  If bytes is
		 * more than to_free then we would have free'd more space had we
		 * not had an artificially high ->csum_bytes, so we need to free
		 * the remainder.  If bytes is the same or less then we don't
		 * need to do anything, the other free-ers did the correct
		 * thing.
		 */
		BTRFS_I(inode)->csum_bytes = orig_csum_bytes - num_bytes;
		if (bytes > to_free)
			to_free = bytes - to_free;
		else
			to_free = 0;
	}
	spin_unlock(&BTRFS_I(inode)->lock);
	if (dropped)
		to_free += btrfs_calc_trans_metadata_size(root, dropped);

	if (to_free) {
		btrfs_block_rsv_release(root, block_rsv, to_free);
		trace_btrfs_space_reservation(root->fs_info, "delalloc",
					      btrfs_ino(inode), to_free, 0);
	}
	if (delalloc_lock)
		mutex_unlock(&BTRFS_I(inode)->delalloc_mutex);
	return ret;
}

/**
 * btrfs_delalloc_release_metadata - release a metadata reservation for an inode
 * @inode: the inode to release the reservation for
 * @num_bytes: the number of bytes we're releasing
 *
 * This will release the metadata reservation for an inode.  This can be called
 * once we complete IO for a given set of bytes to release their metadata
 * reservations.
 */
void btrfs_delalloc_release_metadata(struct inode *inode, u64 num_bytes)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	u64 to_free = 0;
	unsigned dropped;

	num_bytes = ALIGN(num_bytes, root->sectorsize);
	spin_lock(&BTRFS_I(inode)->lock);
	dropped = drop_outstanding_extent(inode);

	if (num_bytes)
		to_free = calc_csum_metadata_size(inode, num_bytes, 0);
	spin_unlock(&BTRFS_I(inode)->lock);
	if (dropped > 0)
		to_free += btrfs_calc_trans_metadata_size(root, dropped);

	trace_btrfs_space_reservation(root->fs_info, "delalloc",
				      btrfs_ino(inode), to_free, 0);
	if (root->fs_info->quota_enabled) {
		btrfs_qgroup_free(root, num_bytes +
					dropped * root->nodesize);
	}

	btrfs_block_rsv_release(root, &root->fs_info->delalloc_block_rsv,
				to_free);
}

/**
 * btrfs_delalloc_reserve_space - reserve data and metadata space for delalloc
 * @inode: inode we're writing to
 * @num_bytes: the number of bytes we want to allocate
 *
 * This will do the following things
 *
 * o reserve space in the data space info for num_bytes
 * o reserve space in the metadata space info based on number of outstanding
 *   extents and how much csums will be needed
 * o add to the inodes ->delalloc_bytes
 * o add it to the fs_info's delalloc inodes list.
 *
 * This will return 0 for success and -ENOSPC if there is no space left.
 */
int btrfs_delalloc_reserve_space(struct inode *inode, u64 num_bytes)
{
	int ret;

	ret = btrfs_check_data_free_space(inode, num_bytes);
	if (ret)
		return ret;

	ret = btrfs_delalloc_reserve_metadata(inode, num_bytes);
	if (ret) {
		btrfs_free_reserved_data_space(inode, num_bytes);
		return ret;
	}

	return 0;
}

/**
 * btrfs_delalloc_release_space - release data and metadata space for delalloc
 * @inode: inode we're releasing space for
 * @num_bytes: the number of bytes we want to free up
 *
 * This must be matched with a call to btrfs_delalloc_reserve_space.  This is
 * called in the case that we don't need the metadata AND data reservations
 * anymore.  So if there is an error or we insert an inline extent.
 *
 * This function will release the metadata space that was not used and will
 * decrement ->delalloc_bytes and remove it from the fs_info delalloc_inodes
 * list if there are no delalloc bytes left.
 */
void btrfs_delalloc_release_space(struct inode *inode, u64 num_bytes)
{
	btrfs_delalloc_release_metadata(inode, num_bytes);
	btrfs_free_reserved_data_space(inode, num_bytes);
}

struct btrfs_block_rsv *
use_block_rsv(struct btrfs_trans_handle *trans,
	      struct btrfs_root *root, u32 blocksize)
{
	struct btrfs_block_rsv *block_rsv;
	struct btrfs_block_rsv *global_rsv = &root->fs_info->global_block_rsv;
	int ret;
	bool global_updated = false;

	block_rsv = get_block_rsv(trans, root);

	if (unlikely(block_rsv->size == 0))
		goto try_reserve;
again:
	ret = block_rsv_use_bytes(block_rsv, blocksize);
	if (!ret)
		return block_rsv;

	if (block_rsv->failfast)
		return ERR_PTR(ret);

	if (block_rsv->type == BTRFS_BLOCK_RSV_GLOBAL && !global_updated) {
		global_updated = true;
		update_global_block_rsv(root->fs_info);
		goto again;
	}

	if (btrfs_test_opt(root, ENOSPC_DEBUG)) {
		static DEFINE_RATELIMIT_STATE(_rs,
				DEFAULT_RATELIMIT_INTERVAL * 10,
				/*DEFAULT_RATELIMIT_BURST*/ 1);
		if (__ratelimit(&_rs))
			WARN(1, KERN_DEBUG
				"BTRFS: block rsv returned %d\n", ret);
	}
try_reserve:
	ret = reserve_metadata_bytes(root, block_rsv, blocksize,
				     BTRFS_RESERVE_NO_FLUSH);
	if (!ret)
		return block_rsv;
	/*
	 * If we couldn't reserve metadata bytes try and use some from
	 * the global reserve if its space type is the same as the global
	 * reservation.
	 */
	if (block_rsv->type != BTRFS_BLOCK_RSV_GLOBAL &&
	    block_rsv->space_info == global_rsv->space_info) {
		ret = block_rsv_use_bytes(global_rsv, blocksize);
		if (!ret)
			return global_rsv;
	}
	return ERR_PTR(ret);
}

void unuse_block_rsv(struct btrfs_fs_info *fs_info,
		     struct btrfs_block_rsv *block_rsv, u32 blocksize)
{
	block_rsv_add_bytes(block_rsv, blocksize, 0);
	block_rsv_release_bytes(fs_info, block_rsv, NULL, 0);
}
