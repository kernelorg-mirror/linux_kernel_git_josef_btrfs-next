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
#ifndef __BLOCK_RSV__
#define __BLOCK_RSV__

enum btrfs_reserve_flush_enum {
	/* If we are in the transaction, we can't flush anything.*/
	BTRFS_RESERVE_NO_FLUSH,
	/*
	 * Flushing delalloc may cause deadlock somewhere, in this
	 * case, use FLUSH LIMIT
	 */
	BTRFS_RESERVE_FLUSH_LIMIT,
	BTRFS_RESERVE_FLUSH_ALL,
};

struct btrfs_block_rsv *
use_block_rsv(struct btrfs_trans_handle *trans,
	      struct btrfs_root *root, u32 blocksize);
void unuse_block_rsv(struct btrfs_fs_info *fs_info,
		     struct btrfs_block_rsv *block_rsv, u32 blocksize);
void update_global_block_rsv(struct btrfs_fs_info *fs_info);
void release_global_block_rsv(struct btrfs_fs_info *fs_info);
void init_global_block_rsv(struct btrfs_fs_info *fs_info);
int btrfs_check_data_free_space(struct inode *inode, u64 bytes);
void btrfs_free_reserved_data_space(struct inode *inode, u64 bytes);
void btrfs_trans_release_metadata(struct btrfs_trans_handle *trans,
				struct btrfs_root *root);
int btrfs_orphan_reserve_metadata(struct btrfs_trans_handle *trans,
				  struct inode *inode);
void btrfs_orphan_release_metadata(struct inode *inode);
int btrfs_subvolume_reserve_metadata(struct btrfs_root *root,
				     struct btrfs_block_rsv *rsv,
				     int nitems,
				     u64 *qgroup_reserved, bool use_global_rsv);
void btrfs_subvolume_release_metadata(struct btrfs_root *root,
				      struct btrfs_block_rsv *rsv,
				      u64 qgroup_reserved);
int btrfs_delalloc_reserve_metadata(struct inode *inode, u64 num_bytes);
void btrfs_delalloc_release_metadata(struct inode *inode, u64 num_bytes);
int btrfs_delalloc_reserve_space(struct inode *inode, u64 num_bytes);
void btrfs_delalloc_release_space(struct inode *inode, u64 num_bytes);
void btrfs_init_block_rsv(struct btrfs_block_rsv *rsv, unsigned short type);
struct btrfs_block_rsv *btrfs_alloc_block_rsv(struct btrfs_root *root,
					      unsigned short type);
void btrfs_free_block_rsv(struct btrfs_root *root,
			  struct btrfs_block_rsv *rsv);
int btrfs_block_rsv_add(struct btrfs_root *root,
			struct btrfs_block_rsv *block_rsv, u64 num_bytes,
			enum btrfs_reserve_flush_enum flush);
int btrfs_block_rsv_check(struct btrfs_root *root,
			  struct btrfs_block_rsv *block_rsv, int min_factor);
int btrfs_block_rsv_refill(struct btrfs_root *root,
			   struct btrfs_block_rsv *block_rsv, u64 min_reserved,
			   enum btrfs_reserve_flush_enum flush);
int btrfs_block_rsv_migrate(struct btrfs_block_rsv *src_rsv,
			    struct btrfs_block_rsv *dst_rsv,
			    u64 num_bytes);
int btrfs_cond_migrate_bytes(struct btrfs_fs_info *fs_info,
			     struct btrfs_block_rsv *dest, u64 num_bytes,
			     int min_factor);
void btrfs_block_rsv_release(struct btrfs_root *root,
			     struct btrfs_block_rsv *block_rsv,
			     u64 num_bytes);

static inline u64 calc_global_rsv_need_space(struct btrfs_block_rsv *global)
{
	return (global->size << 1);
}


#endif /* __BLOCK_RSV__ */
