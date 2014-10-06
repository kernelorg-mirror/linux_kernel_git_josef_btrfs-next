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
#ifndef __BLOCK_GROUP__
#define __BLOCK_GROUP__

#include "ctree.h"

/*
 * control flags for btrfs_maybe_chunk_alloc's force field
 * CHUNK_ALLOC_NO_FORCE means to only allocate a chunk
 * if we really need one.
 *
 * CHUNK_ALLOC_LIMITED means to only try and allocate one
 * if we have very few chunks already allocated.  This is
 * used as part of the clustering code to help make sure
 * we have a good pool of storage to cluster in, without
 * filling the FS with empty chunks
 *
 * CHUNK_ALLOC_FORCE means it must try to allocate one
 *
 */
enum {
	CHUNK_ALLOC_NO_FORCE = 0,
	CHUNK_ALLOC_LIMITED = 1,
	CHUNK_ALLOC_FORCE = 2,
};

/*
 * Control how reservations are dealt with.
 *
 * RESERVE_FREE - freeing a reservation.
 * RESERVE_ALLOC - allocating space and we need to update bytes_may_use for
 *   ENOSPC accounting
 * RESERVE_ALLOC_NO_ACCOUNT - allocating space and we should not update
 *   bytes_may_use as the ENOSPC accounting is done elsewhere
 */
enum {
	RESERVE_FREE = 0,
	RESERVE_ALLOC = 1,
	RESERVE_ALLOC_NO_ACCOUNT = 2,
};

enum btrfs_disk_cache_state {
	BTRFS_DC_WRITTEN	= 0,
	BTRFS_DC_ERROR		= 1,
	BTRFS_DC_CLEAR		= 2,
	BTRFS_DC_SETUP		= 3,
	BTRFS_DC_NEED_WRITE	= 4,
};

struct btrfs_space_info {
	spinlock_t lock;

	u64 total_bytes;	/* total bytes in the space,
				   this doesn't take mirrors into account */
	u64 bytes_used;		/* total bytes used,
				   this doesn't take mirrors into account */
	u64 bytes_pinned;	/* total bytes pinned, will be freed when the
				   transaction finishes */
	u64 bytes_reserved;	/* total bytes the allocator has reserved for
				   current allocations */
	u64 bytes_may_use;	/* number of bytes that may be used for
				   delalloc/allocations */
	u64 bytes_readonly;	/* total bytes that are read only */

	unsigned int full:1;	/* indicates that we cannot allocate any more
				   chunks for this space */
	unsigned int chunk_alloc:1;	/* set if we are allocating a chunk */

	unsigned int flush:1;		/* set if we are trying to make space */

	unsigned int force_alloc;	/* set if we need to force a chunk
					   alloc for this space */

	u64 disk_used;		/* total bytes used on disk */
	u64 disk_total;		/* total bytes on disk, takes mirrors into
				   account */

	u64 flags;

	/*
	 * bytes_pinned is kept in line with what is actually pinned, as in
	 * we've called update_block_group and dropped the bytes_used counter
	 * and increased the bytes_pinned counter.  However this means that
	 * bytes_pinned does not reflect the bytes that will be pinned once the
	 * delayed refs are flushed, so this counter is inc'ed everytime we call
	 * btrfs_free_extent so it is a realtime count of what will be freed
	 * once the transaction is committed.  It will be zero'ed everytime the
	 * transaction commits.
	 */
	struct percpu_counter total_bytes_pinned;

	struct list_head list;

	struct rw_semaphore groups_sem;
	/* for block groups in our same type */
	struct list_head block_groups[BTRFS_NR_RAID_TYPES];
	wait_queue_head_t wait;

	struct kobject kobj;
	struct kobject *block_group_kobjs[BTRFS_NR_RAID_TYPES];
};

struct btrfs_caching_control {
	struct list_head list;
	struct mutex mutex;
	wait_queue_head_t wait;
	struct btrfs_work work;
	struct btrfs_block_group_cache *block_group;
	u64 progress;
	atomic_t count;
};

struct btrfs_block_group_cache {
	struct btrfs_key key;
	struct btrfs_block_group_item item;
	struct btrfs_fs_info *fs_info;
	struct inode *inode;
	spinlock_t lock;
	u64 pinned;
	u64 reserved;
	u64 delalloc_bytes;
	u64 bytes_super;
	u64 flags;
	u64 sectorsize;
	u64 cache_generation;

	/*
	 * It is just used for the delayed data space allocation because
	 * only the data space allocation and the relative metadata update
	 * can be done cross the transaction.
	 */
	struct rw_semaphore data_rwsem;

	/* for raid56, this is a full stripe, without parity */
	unsigned long full_stripe_len;

	unsigned int ro:1;
	unsigned int dirty:1;
	unsigned int iref:1;

	int disk_cache_state;

	/* cache tracking stuff */
	int cached;
	struct btrfs_caching_control *caching_ctl;
	u64 last_byte_to_unpin;

	struct btrfs_space_info *space_info;

	/* free space cache stuff */
	struct btrfs_free_space_ctl *free_space_ctl;

	/* block group cache stuff */
	struct rb_node cache_node;

	/* for block groups in the same raid type */
	struct list_head list;

	/* usage count */
	atomic_t count;

	/* List of struct btrfs_free_clusters for this block group.
	 * Today it will only have one thing on it, but that may change
	 */
	struct list_head cluster_list;

	/* For delayed block group creation or deletion of empty block groups */
	struct list_head bg_list;
};

int do_chunk_alloc(struct btrfs_trans_handle *trans,
		   struct btrfs_root *extent_root, u64 flags, int force);
void add_pinned_bytes(struct btrfs_fs_info *fs_info, u64 num_bytes,
		      u64 owner, u64 root_objectid);
int update_block_group(struct btrfs_root *root,
		       u64 bytenr, u64 num_bytes, int alloc);
int pin_down_extent(struct btrfs_root *root,
		    struct btrfs_block_group_cache *cache,
		    u64 bytenr, u64 num_bytes, int reserved);
int btrfs_update_reserved_bytes(struct btrfs_block_group_cache *cache,
				u64 num_bytes, int reserve, int delalloc);
int __exclude_logged_extent(struct btrfs_root *root, u64 start, u64 num_bytes);
struct btrfs_space_info *__find_space_info(struct btrfs_fs_info *info,
					   u64 flags);
int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root);
int btrfs_extent_readonly(struct btrfs_root *root, u64 bytenr);
int btrfs_free_block_groups(struct btrfs_fs_info *info);
int btrfs_read_block_groups(struct btrfs_root *root);
int btrfs_can_relocate(struct btrfs_root *root, u64 bytenr);
int btrfs_make_block_group(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, u64 bytes_used,
			   u64 type, u64 chunk_objectid, u64 chunk_offset,
			   u64 size);
int btrfs_remove_block_group(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 group_start);
void btrfs_delete_unused_bgs(struct btrfs_fs_info *fs_info);
void btrfs_create_pending_block_groups(struct btrfs_trans_handle *trans,
				       struct btrfs_root *root);
u64 btrfs_get_alloc_profile(struct btrfs_root *root, int data);
void btrfs_clear_space_info_full(struct btrfs_fs_info *info);

int btrfs_set_block_group_ro(struct btrfs_root *root,
			     struct btrfs_block_group_cache *cache);
void btrfs_set_block_group_rw(struct btrfs_root *root,
			      struct btrfs_block_group_cache *cache);
void btrfs_put_block_group_cache(struct btrfs_fs_info *info);
u64 btrfs_account_ro_block_groups_free_space(struct btrfs_space_info *sinfo);
int btrfs_error_unpin_extent_range(struct btrfs_root *root,
				   u64 start, u64 end);
int btrfs_error_discard_extent(struct btrfs_root *root, u64 bytenr,
			       u64 num_bytes, u64 *actual_bytes);
int btrfs_force_chunk_alloc(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, u64 type);
int btrfs_trim_fs(struct btrfs_root *root, struct fstrim_range *range);
int btrfs_free_reserved_extent(struct btrfs_root *root, u64 start, u64 len,
			       int delalloc);
int btrfs_free_and_pin_reserved_extent(struct btrfs_root *root,
				       u64 start, u64 len);
void btrfs_prepare_extent_commit(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root);
int btrfs_finish_extent_commit(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root);
int btrfs_reserve_extent(struct btrfs_root *root, u64 num_bytes,
			 u64 min_alloc_size, u64 empty_size, u64 hint_byte,
			 struct btrfs_key *ins, int is_data, int delalloc);
int btrfs_init_space_info(struct btrfs_fs_info *fs_info);
int btrfs_pin_extent(struct btrfs_root *root,
		     u64 bytenr, u64 num, int reserved);
int btrfs_pin_extent_for_log_replay(struct btrfs_root *root,
				    u64 bytenr, u64 num_bytes);
int btrfs_exclude_logged_extents(struct btrfs_root *root,
				 struct extent_buffer *eb);
struct btrfs_block_group_cache *btrfs_lookup_block_group(
						 struct btrfs_fs_info *info,
						 u64 bytenr);
void btrfs_put_block_group(struct btrfs_block_group_cache *cache);
int get_block_group_index(struct btrfs_block_group_cache *cache);

static inline bool btrfs_mixed_space_info(struct btrfs_space_info *space_info)
{
	return ((space_info->flags & BTRFS_BLOCK_GROUP_METADATA) &&
		(space_info->flags & BTRFS_BLOCK_GROUP_DATA));
}

#endif /* __BLOCK_GROUP__ */
