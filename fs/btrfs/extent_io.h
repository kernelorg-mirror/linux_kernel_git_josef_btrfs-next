/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_EXTENT_IO_H
#define BTRFS_EXTENT_IO_H

#include <linux/rbtree.h>
#include <linux/refcount.h>
#include "ulist.h"

/*
 * flags for bio submission. The high bits indicate the compression
 * type for this bio
 */
#define EXTENT_BIO_COMPRESSED 1
#define EXTENT_BIO_FLAG_SHIFT 16

/* these are flags for __process_pages_contig */
#define PAGE_UNLOCK		(1 << 0)
#define PAGE_CLEAR_DIRTY	(1 << 1)
#define PAGE_SET_WRITEBACK	(1 << 2)
#define PAGE_END_WRITEBACK	(1 << 3)
#define PAGE_SET_PRIVATE2	(1 << 4)
#define PAGE_SET_ERROR		(1 << 5)
#define PAGE_LOCK		(1 << 6)

/*
 * page->private values.  Every page that is controlled by the extent
 * map has page->private set to one.
 */
#define EXTENT_PAGE_PRIVATE 1

struct btrfs_root;
struct btrfs_inode;
struct btrfs_io_bio;
struct io_failure_record;
struct extent_io_tree;
struct extent_buffer;

typedef blk_status_t (extent_submit_bio_start_t)(void *private_data,
		struct bio *bio, u64 bio_offset);

struct extent_io_ops {
	/*
	 * The following callbacks must be always defined, the function
	 * pointer will be called unconditionally.
	 */
	blk_status_t (*submit_bio_hook)(struct inode *inode, struct bio *bio,
					int mirror_num, unsigned long bio_flags);
	int (*readpage_end_io_hook)(struct btrfs_io_bio *io_bio, u64 phy_offset,
				    struct page *page, u64 start, u64 end,
				    int mirror);
};

/*
 * Structure to record how many bytes and which ranges are set/cleared
 */
struct extent_changeset {
	/* How many bytes are set/cleared in this operation */
	unsigned int bytes_changed;

	/* Changed ranges */
	struct ulist range_changed;
};

static inline void extent_changeset_init(struct extent_changeset *changeset)
{
	changeset->bytes_changed = 0;
	ulist_init(&changeset->range_changed);
}

static inline struct extent_changeset *extent_changeset_alloc(void)
{
	struct extent_changeset *ret;

	ret = kmalloc(sizeof(*ret), GFP_KERNEL);
	if (!ret)
		return NULL;

	extent_changeset_init(ret);
	return ret;
}

static inline void extent_changeset_release(struct extent_changeset *changeset)
{
	if (!changeset)
		return;
	changeset->bytes_changed = 0;
	ulist_release(&changeset->range_changed);
}

static inline void extent_changeset_free(struct extent_changeset *changeset)
{
	if (!changeset)
		return;
	extent_changeset_release(changeset);
	kfree(changeset);
}

static inline void extent_set_compress_type(unsigned long *bio_flags,
					    int compress_type)
{
	*bio_flags |= compress_type << EXTENT_BIO_FLAG_SHIFT;
}

static inline int extent_compress_type(unsigned long bio_flags)
{
	return bio_flags >> EXTENT_BIO_FLAG_SHIFT;
}

struct extent_map_tree;

typedef struct extent_map *(get_extent_t)(struct btrfs_inode *inode,
					  struct page *page,
					  size_t pg_offset,
					  u64 start, u64 len,
					  int create);

int try_release_extent_mapping(struct page *page, gfp_t mask);

int extent_read_full_page(struct extent_io_tree *tree, struct page *page,
			  get_extent_t *get_extent, int mirror_num);
int extent_write_full_page(struct page *page, struct writeback_control *wbc);
int extent_write_locked_range(struct inode *inode, u64 start, u64 end,
			      int mode);
int extent_writepages(struct address_space *mapping,
		      struct writeback_control *wbc);
int btree_write_cache_pages(struct address_space *mapping,
			    struct writeback_control *wbc);
int extent_readpages(struct address_space *mapping, struct list_head *pages,
		     unsigned nr_pages);
int extent_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		__u64 start, __u64 len);
void set_page_extent_mapped(struct page *page);

#define WAIT_NONE	0
#define WAIT_COMPLETE	1
#define WAIT_PAGE_LOCK	2

int read_extent_buffer_pages(struct extent_buffer *eb, int wait,
			     int mirror_num);

void extent_range_clear_dirty_for_io(struct inode *inode, u64 start, u64 end);
void extent_range_redirty_for_io(struct inode *inode, u64 start, u64 end);
void extent_clear_unlock_delalloc(struct inode *inode, u64 start, u64 end,
				  struct page *locked_page,
				  unsigned bits_to_clear,
				  unsigned long page_ops);
struct bio *btrfs_bio_alloc(u64 first_byte);
struct bio *btrfs_io_bio_alloc(unsigned int nr_iovecs);
struct bio *btrfs_bio_clone(struct bio *bio);
struct bio *btrfs_bio_clone_partial(struct bio *orig, int offset, int size);

struct btrfs_fs_info;
struct btrfs_inode;

int repair_io_failure(struct btrfs_fs_info *fs_info, u64 ino, u64 start,
		      u64 length, u64 logical, struct page *page,
		      unsigned int pg_offset, int mirror_num);
void end_extent_writepage(struct page *page, int err, u64 start, u64 end);
int btrfs_repair_eb_io_failure(struct extent_buffer *eb, int mirror_num);

/*
 * When IO fails, either with EIO or csum verification fails, we
 * try other mirrors that might have a good copy of the data.  This
 * io_failure_record is used to record state as we go through all the
 * mirrors.  If another mirror has good data, the page is set up to date
 * and things continue.  If a good mirror can't be found, the original
 * bio end_io callback is called to indicate things have failed.
 */
struct io_failure_record {
	struct page *page;
	u64 start;
	u64 len;
	u64 logical;
	unsigned long bio_flags;
	int this_mirror;
	int failed_mirror;
	int in_validation;
};


bool btrfs_check_repairable(struct inode *inode, unsigned failed_bio_pages,
			    struct io_failure_record *failrec, int fail_mirror);
struct bio *btrfs_create_repair_bio(struct inode *inode, struct bio *failed_bio,
				    struct io_failure_record *failrec,
				    struct page *page, int pg_offset, int icsum,
				    bio_end_io_t *endio_func, void *data);
#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
bool find_lock_delalloc_range(struct inode *inode,
			     struct page *locked_page, u64 *start,
			     u64 *end);
#endif
#endif
