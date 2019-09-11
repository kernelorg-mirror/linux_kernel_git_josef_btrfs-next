// SPDX-License-Identifier: GPL-2.0

#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/prefetch.h>
#include <linux/cleancache.h>
#include "extent_io.h"
#include "extent-io-tree.h"
#include "extent_map.h"
#include "ctree.h"
#include "btrfs_inode.h"
#include "volumes.h"
#include "check-integrity.h"
#include "locking.h"
#include "rcu-string.h"
#include "backref.h"
#include "disk-io.h"

static struct kmem_cache *extent_buffer_cache;
static struct bio_set btrfs_bioset;

#ifdef CONFIG_BTRFS_DEBUG
static LIST_HEAD(buffers);

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

static inline void btrfs_extent_buffer_leak_debug_check(void)
{
	struct extent_buffer *eb;

	while (!list_empty(&buffers)) {
		eb = list_entry(buffers.next, struct extent_buffer, leak_list);
		pr_err("BTRFS: buffer leak start %llu len %lu refs %d bflags %lu\n",
		       eb->start, eb->len, atomic_read(&eb->refs), eb->bflags);
		list_del(&eb->leak_list);
		kmem_cache_free(extent_buffer_cache, eb);
	}
}
#else
#define btrfs_leak_debug_add(new, head)	do {} while (0)
#define btrfs_leak_debug_del(entry)	do {} while (0)
#define btrfs_extent_buffer_leak_debug_check()	do {} while (0)
#endif

struct extent_page_data {
	struct bio *bio;
	struct extent_io_tree *tree;
	/* tells writepage not to lock the state bits for this range
	 * it still does the unlocking
	 */
	unsigned int extent_locked:1;

	/* tells the submit_bio code to use REQ_SYNC */
	unsigned int sync_io:1;
};

static int __must_check submit_one_bio(struct bio *bio, int mirror_num,
				       unsigned long bio_flags)
{
	blk_status_t ret = 0;
	struct extent_io_tree *tree = bio->bi_private;

	bio->bi_private = NULL;

	if (tree->ops)
		ret = tree->ops->submit_bio_hook(tree->private_data, bio,
						 mirror_num, bio_flags);
	else
		btrfsic_submit_bio(bio);

	return blk_status_to_errno(ret);
}

/* Cleanup unsubmitted bios */
static void end_write_bio(struct extent_page_data *epd, int ret)
{
	if (epd->bio) {
		epd->bio->bi_status = errno_to_blk_status(ret);
		bio_endio(epd->bio);
		epd->bio = NULL;
	}
}

/*
 * Submit bio from extent page data via submit_one_bio
 *
 * Return 0 if everything is OK.
 * Return <0 for error.
 */
static int __must_check flush_write_bio(struct extent_page_data *epd)
{
	int ret = 0;

	if (epd->bio) {
		ret = submit_one_bio(epd->bio, 0, 0);
		/*
		 * Clean up of epd->bio is handled by its endio function.
		 * And endio is either triggered by successful bio execution
		 * or the error handler of submit bio hook.
		 * So at this point, no matter what happened, we don't need
		 * to clean up epd->bio.
		 */
		epd->bio = NULL;
	}
	return ret;
}

int __init extent_io_init(void)
{
	extent_buffer_cache = kmem_cache_create("btrfs_extent_buffer",
			sizeof(struct extent_buffer), 0,
			SLAB_MEM_SPREAD, NULL);
	if (!extent_buffer_cache)
		return -ENOMEM;

	if (bioset_init(&btrfs_bioset, BIO_POOL_SIZE,
			offsetof(struct btrfs_io_bio, bio),
			BIOSET_NEED_BVECS))
		goto free_buffer_cache;

	if (bioset_integrity_create(&btrfs_bioset, BIO_POOL_SIZE))
		goto free_bioset;

	return 0;

free_bioset:
	bioset_exit(&btrfs_bioset);

free_buffer_cache:
	kmem_cache_destroy(extent_buffer_cache);
	extent_buffer_cache = NULL;
	return -ENOMEM;
}

void __cold extent_io_exit(void)
{
	btrfs_extent_buffer_leak_debug_check();

	/*
	 * Make sure all delayed rcu free are flushed before we
	 * destroy caches.
	 */
	rcu_barrier();
	kmem_cache_destroy(extent_buffer_cache);
	bioset_exit(&btrfs_bioset);
}

void extent_range_clear_dirty_for_io(struct inode *inode, u64 start, u64 end)
{
	unsigned long index = start >> PAGE_SHIFT;
	unsigned long end_index = end >> PAGE_SHIFT;
	struct page *page;

	while (index <= end_index) {
		page = find_get_page(inode->i_mapping, index);
		BUG_ON(!page); /* Pages should be in the extent_io_tree */
		clear_page_dirty_for_io(page);
		put_page(page);
		index++;
	}
}

void extent_range_redirty_for_io(struct inode *inode, u64 start, u64 end)
{
	unsigned long index = start >> PAGE_SHIFT;
	unsigned long end_index = end >> PAGE_SHIFT;
	struct page *page;

	while (index <= end_index) {
		page = find_get_page(inode->i_mapping, index);
		BUG_ON(!page); /* Pages should be in the extent_io_tree */
		__set_page_dirty_nobuffers(page);
		account_page_redirty(page);
		put_page(page);
		index++;
	}
}

static int __process_pages_contig(struct address_space *mapping,
				  struct page *locked_page,
				  pgoff_t start_index, pgoff_t end_index,
				  unsigned long page_ops, pgoff_t *index_ret);

static noinline void __unlock_for_delalloc(struct inode *inode,
					   struct page *locked_page,
					   u64 start, u64 end)
{
	unsigned long index = start >> PAGE_SHIFT;
	unsigned long end_index = end >> PAGE_SHIFT;

	ASSERT(locked_page);
	if (index == locked_page->index && end_index == index)
		return;

	__process_pages_contig(inode->i_mapping, locked_page, index, end_index,
			       PAGE_UNLOCK, NULL);
}

static noinline int lock_delalloc_pages(struct inode *inode,
					struct page *locked_page,
					u64 delalloc_start,
					u64 delalloc_end)
{
	unsigned long index = delalloc_start >> PAGE_SHIFT;
	unsigned long index_ret = index;
	unsigned long end_index = delalloc_end >> PAGE_SHIFT;
	int ret;

	ASSERT(locked_page);
	if (index == locked_page->index && index == end_index)
		return 0;

	ret = __process_pages_contig(inode->i_mapping, locked_page, index,
				     end_index, PAGE_LOCK, &index_ret);
	if (ret == -EAGAIN)
		__unlock_for_delalloc(inode, locked_page, delalloc_start,
				      (u64)index_ret << PAGE_SHIFT);
	return ret;
}

/*
 * Find and lock a contiguous range of bytes in the file marked as delalloc, no
 * more than @max_bytes.  @Start and @end are used to return the range,
 *
 * Return: true if we find something
 *         false if nothing was in the tree
 */
EXPORT_FOR_TESTS
noinline_for_stack bool find_lock_delalloc_range(struct inode *inode,
				    struct page *locked_page, u64 *start,
				    u64 *end)
{
	struct extent_io_tree *tree = &BTRFS_I(inode)->io_tree;
	u64 max_bytes = BTRFS_MAX_EXTENT_SIZE;
	u64 delalloc_start;
	u64 delalloc_end;
	bool found;
	struct extent_state *cached_state = NULL;
	int ret;
	int loops = 0;

again:
	/* step one, find a bunch of delalloc bytes starting at start */
	delalloc_start = *start;
	delalloc_end = 0;
	found = btrfs_find_delalloc_range(tree, &delalloc_start, &delalloc_end,
					  max_bytes, &cached_state);
	if (!found || delalloc_end <= *start) {
		*start = delalloc_start;
		*end = delalloc_end;
		free_extent_state(cached_state);
		return false;
	}

	/*
	 * start comes from the offset of locked_page.  We have to lock
	 * pages in order, so we can't process delalloc bytes before
	 * locked_page
	 */
	if (delalloc_start < *start)
		delalloc_start = *start;

	/*
	 * make sure to limit the number of pages we try to lock down
	 */
	if (delalloc_end + 1 - delalloc_start > max_bytes)
		delalloc_end = delalloc_start + max_bytes - 1;

	/* step two, lock all the pages after the page that has start */
	ret = lock_delalloc_pages(inode, locked_page,
				  delalloc_start, delalloc_end);
	ASSERT(!ret || ret == -EAGAIN);
	if (ret == -EAGAIN) {
		/* some of the pages are gone, lets avoid looping by
		 * shortening the size of the delalloc range we're searching
		 */
		free_extent_state(cached_state);
		cached_state = NULL;
		if (!loops) {
			max_bytes = PAGE_SIZE;
			loops = 1;
			goto again;
		} else {
			found = false;
			goto out_failed;
		}
	}

	/* step three, lock the state bits for the whole range */
	lock_extent_bits(tree, delalloc_start, delalloc_end, &cached_state);

	/* then test to make sure it is all still delalloc */
	ret = test_range_bit(tree, delalloc_start, delalloc_end,
			     EXTENT_DELALLOC, 1, cached_state);
	if (!ret) {
		unlock_extent_cached(tree, delalloc_start, delalloc_end,
				     &cached_state);
		__unlock_for_delalloc(inode, locked_page,
			      delalloc_start, delalloc_end);
		cond_resched();
		goto again;
	}
	free_extent_state(cached_state);
	*start = delalloc_start;
	*end = delalloc_end;
out_failed:
	return found;
}

static int __process_pages_contig(struct address_space *mapping,
				  struct page *locked_page,
				  pgoff_t start_index, pgoff_t end_index,
				  unsigned long page_ops, pgoff_t *index_ret)
{
	unsigned long nr_pages = end_index - start_index + 1;
	unsigned long pages_locked = 0;
	pgoff_t index = start_index;
	struct page *pages[16];
	unsigned ret;
	int err = 0;
	int i;

	if (page_ops & PAGE_LOCK) {
		ASSERT(page_ops == PAGE_LOCK);
		ASSERT(index_ret && *index_ret == start_index);
	}

	if ((page_ops & PAGE_SET_ERROR) && nr_pages > 0)
		mapping_set_error(mapping, -EIO);

	while (nr_pages > 0) {
		ret = find_get_pages_contig(mapping, index,
				     min_t(unsigned long,
				     nr_pages, ARRAY_SIZE(pages)), pages);
		if (ret == 0) {
			/*
			 * Only if we're going to lock these pages,
			 * can we find nothing at @index.
			 */
			ASSERT(page_ops & PAGE_LOCK);
			err = -EAGAIN;
			goto out;
		}

		for (i = 0; i < ret; i++) {
			if (page_ops & PAGE_SET_PRIVATE2)
				SetPagePrivate2(pages[i]);

			if (pages[i] == locked_page) {
				put_page(pages[i]);
				pages_locked++;
				continue;
			}
			if (page_ops & PAGE_CLEAR_DIRTY)
				clear_page_dirty_for_io(pages[i]);
			if (page_ops & PAGE_SET_WRITEBACK)
				set_page_writeback(pages[i]);
			if (page_ops & PAGE_SET_ERROR)
				SetPageError(pages[i]);
			if (page_ops & PAGE_END_WRITEBACK)
				end_page_writeback(pages[i]);
			if (page_ops & PAGE_UNLOCK)
				unlock_page(pages[i]);
			if (page_ops & PAGE_LOCK) {
				lock_page(pages[i]);
				if (!PageDirty(pages[i]) ||
				    pages[i]->mapping != mapping) {
					unlock_page(pages[i]);
					put_page(pages[i]);
					err = -EAGAIN;
					goto out;
				}
			}
			put_page(pages[i]);
			pages_locked++;
		}
		nr_pages -= ret;
		index += ret;
		cond_resched();
	}
out:
	if (err && index_ret)
		*index_ret = start_index + pages_locked - 1;
	return err;
}

void extent_clear_unlock_delalloc(struct inode *inode, u64 start, u64 end,
				  struct page *locked_page,
				  unsigned clear_bits,
				  unsigned long page_ops)
{
	clear_extent_bit(&BTRFS_I(inode)->io_tree, start, end, clear_bits, 1, 0,
			 NULL);

	__process_pages_contig(inode->i_mapping, locked_page,
			       start >> PAGE_SHIFT, end >> PAGE_SHIFT,
			       page_ops, NULL);
}

/*
 * helper function to set a given page up to date if all the
 * extents in the tree for that page are up to date
 */
static void check_page_uptodate(struct extent_io_tree *tree, struct page *page)
{
	u64 start = page_offset(page);
	u64 end = start + PAGE_SIZE - 1;
	if (test_range_bit(tree, start, end, EXTENT_UPTODATE, 1, NULL))
		SetPageUptodate(page);
}

/*
 * this bypasses the standard btrfs submit functions deliberately, as
 * the standard behavior is to write all copies in a raid setup. here we only
 * want to write the one bad copy. so we do the mapping for ourselves and issue
 * submit_bio directly.
 * to avoid any synchronization issues, wait for the data after writing, which
 * actually prevents the read that triggered the error from finishing.
 * currently, there can be no more than two copies of every data bit. thus,
 * exactly one rewrite is required.
 */
int repair_io_failure(struct btrfs_fs_info *fs_info, u64 ino, u64 start,
		      u64 length, u64 logical, struct page *page,
		      unsigned int pg_offset, int mirror_num)
{
	struct bio *bio;
	struct btrfs_device *dev;
	u64 map_length = 0;
	u64 sector;
	struct btrfs_bio *bbio = NULL;
	int ret;

	ASSERT(!(fs_info->sb->s_flags & SB_RDONLY));
	BUG_ON(!mirror_num);

	bio = btrfs_io_bio_alloc(1);
	bio->bi_iter.bi_size = 0;
	map_length = length;

	/*
	 * Avoid races with device replace and make sure our bbio has devices
	 * associated to its stripes that don't go away while we are doing the
	 * read repair operation.
	 */
	btrfs_bio_counter_inc_blocked(fs_info);
	if (btrfs_is_parity_mirror(fs_info, logical, length)) {
		/*
		 * Note that we don't use BTRFS_MAP_WRITE because it's supposed
		 * to update all raid stripes, but here we just want to correct
		 * bad stripe, thus BTRFS_MAP_READ is abused to only get the bad
		 * stripe's dev and sector.
		 */
		ret = btrfs_map_block(fs_info, BTRFS_MAP_READ, logical,
				      &map_length, &bbio, 0);
		if (ret) {
			btrfs_bio_counter_dec(fs_info);
			bio_put(bio);
			return -EIO;
		}
		ASSERT(bbio->mirror_num == 1);
	} else {
		ret = btrfs_map_block(fs_info, BTRFS_MAP_WRITE, logical,
				      &map_length, &bbio, mirror_num);
		if (ret) {
			btrfs_bio_counter_dec(fs_info);
			bio_put(bio);
			return -EIO;
		}
		BUG_ON(mirror_num != bbio->mirror_num);
	}

	sector = bbio->stripes[bbio->mirror_num - 1].physical >> 9;
	bio->bi_iter.bi_sector = sector;
	dev = bbio->stripes[bbio->mirror_num - 1].dev;
	btrfs_put_bbio(bbio);
	if (!dev || !dev->bdev ||
	    !test_bit(BTRFS_DEV_STATE_WRITEABLE, &dev->dev_state)) {
		btrfs_bio_counter_dec(fs_info);
		bio_put(bio);
		return -EIO;
	}
	bio_set_dev(bio, dev->bdev);
	bio->bi_opf = REQ_OP_WRITE | REQ_SYNC;
	bio_add_page(bio, page, length, pg_offset);

	if (btrfsic_submit_bio_wait(bio)) {
		/* try to remap that extent elsewhere? */
		btrfs_bio_counter_dec(fs_info);
		bio_put(bio);
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_WRITE_ERRS);
		return -EIO;
	}

	btrfs_info_rl_in_rcu(fs_info,
		"read error corrected: ino %llu off %llu (dev %s sector %llu)",
				  ino, start,
				  rcu_str_deref(dev->name), sector);
	btrfs_bio_counter_dec(fs_info);
	bio_put(bio);
	return 0;
}

int btrfs_repair_eb_io_failure(struct extent_buffer *eb, int mirror_num)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	u64 start = eb->start;
	int i, num_pages = num_extent_pages(eb);
	int ret = 0;

	if (sb_rdonly(fs_info->sb))
		return -EROFS;

	for (i = 0; i < num_pages; i++) {
		struct page *p = eb->pages[i];

		ret = repair_io_failure(fs_info, 0, start, PAGE_SIZE, start, p,
					start - page_offset(p), mirror_num);
		if (ret)
			break;
		start += PAGE_SIZE;
	}

	return ret;
}

bool btrfs_check_repairable(struct inode *inode, unsigned failed_bio_pages,
			   struct io_failure_record *failrec, int failed_mirror)
{
	struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);
	int num_copies;

	num_copies = btrfs_num_copies(fs_info, failrec->logical, failrec->len);
	if (num_copies == 1) {
		/*
		 * we only have a single copy of the data, so don't bother with
		 * all the retry and error correction code that follows. no
		 * matter what the error is, it is very likely to persist.
		 */
		btrfs_debug(fs_info,
			"Check Repairable: cannot repair, num_copies=%d, next_mirror %d, failed_mirror %d",
			num_copies, failrec->this_mirror, failed_mirror);
		return false;
	}

	/*
	 * there are two premises:
	 *	a) deliver good data to the caller
	 *	b) correct the bad sectors on disk
	 */
	if (failed_bio_pages > 1) {
		/*
		 * to fulfill b), we need to know the exact failing sectors, as
		 * we don't want to rewrite any more than the failed ones. thus,
		 * we need separate read requests for the failed bio
		 *
		 * if the following BUG_ON triggers, our validation request got
		 * merged. we need separate requests for our algorithm to work.
		 */
		BUG_ON(failrec->in_validation);
		failrec->in_validation = 1;
		failrec->this_mirror = failed_mirror;
	} else {
		/*
		 * we're ready to fulfill a) and b) alongside. get a good copy
		 * of the failed sector and if we succeed, we have setup
		 * everything for repair_io_failure to do the rest for us.
		 */
		if (failrec->in_validation) {
			BUG_ON(failrec->this_mirror != failed_mirror);
			failrec->in_validation = 0;
			failrec->this_mirror = 0;
		}
		failrec->failed_mirror = failed_mirror;
		failrec->this_mirror++;
		if (failrec->this_mirror == failed_mirror)
			failrec->this_mirror++;
	}

	if (failrec->this_mirror > num_copies) {
		btrfs_debug(fs_info,
			"Check Repairable: (fail) num_copies=%d, next_mirror %d, failed_mirror %d",
			num_copies, failrec->this_mirror, failed_mirror);
		return false;
	}

	return true;
}


struct bio *btrfs_create_repair_bio(struct inode *inode, struct bio *failed_bio,
				    struct io_failure_record *failrec,
				    struct page *page, int pg_offset, int icsum,
				    bio_end_io_t *endio_func, void *data)
{
	struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);
	struct bio *bio;
	struct btrfs_io_bio *btrfs_failed_bio;
	struct btrfs_io_bio *btrfs_bio;

	bio = btrfs_io_bio_alloc(1);
	bio->bi_end_io = endio_func;
	bio->bi_iter.bi_sector = failrec->logical >> 9;
	bio_set_dev(bio, fs_info->fs_devices->latest_bdev);
	bio->bi_iter.bi_size = 0;
	bio->bi_private = data;

	btrfs_failed_bio = btrfs_io_bio(failed_bio);
	if (btrfs_failed_bio->csum) {
		u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);

		btrfs_bio = btrfs_io_bio(bio);
		btrfs_bio->csum = btrfs_bio->csum_inline;
		icsum *= csum_size;
		memcpy(btrfs_bio->csum, btrfs_failed_bio->csum + icsum,
		       csum_size);
	}

	bio_add_page(bio, page, failrec->len, pg_offset);

	return bio;
}

/*
 * This is a generic handler for readpage errors. If other copies exist, read
 * those and write back good data to the failed position. Does not investigate
 * in remapping the failed extent elsewhere, hoping the device will be smart
 * enough to do this as needed
 */
static int bio_readpage_error(struct bio *failed_bio, u64 phy_offset,
			      struct page *page, u64 start, u64 end,
			      int failed_mirror)
{
	struct io_failure_record *failrec;
	struct inode *inode = page->mapping->host;
	struct extent_io_tree *tree = &BTRFS_I(inode)->io_tree;
	struct extent_io_tree *failure_tree = &BTRFS_I(inode)->io_failure_tree;
	struct bio *bio;
	int read_mode = 0;
	blk_status_t status;
	int ret;
	unsigned failed_bio_pages = failed_bio->bi_iter.bi_size >> PAGE_SHIFT;

	BUG_ON(bio_op(failed_bio) == REQ_OP_WRITE);

	ret = btrfs_get_io_failure_record(inode, start, end, &failrec);
	if (ret)
		return ret;

	if (!btrfs_check_repairable(inode, failed_bio_pages, failrec,
				    failed_mirror)) {
		free_io_failure(failure_tree, tree, failrec);
		return -EIO;
	}

	if (failed_bio_pages > 1)
		read_mode |= REQ_FAILFAST_DEV;

	phy_offset >>= inode->i_sb->s_blocksize_bits;
	bio = btrfs_create_repair_bio(inode, failed_bio, failrec, page,
				      start - page_offset(page),
				      (int)phy_offset, failed_bio->bi_end_io,
				      NULL);
	bio->bi_opf = REQ_OP_READ | read_mode;

	btrfs_debug(btrfs_sb(inode->i_sb),
		"Repair Read Error: submitting new read[%#x] to this_mirror=%d, in_validation=%d",
		read_mode, failrec->this_mirror, failrec->in_validation);

	status = tree->ops->submit_bio_hook(tree->private_data, bio, failrec->this_mirror,
					 failrec->bio_flags);
	if (status) {
		free_io_failure(failure_tree, tree, failrec);
		bio_put(bio);
		ret = blk_status_to_errno(status);
	}

	return ret;
}

/* lots and lots of room for performance fixes in the end_bio funcs */

void end_extent_writepage(struct page *page, int err, u64 start, u64 end)
{
	int uptodate = (err == 0);
	int ret = 0;

	btrfs_writepage_endio_finish_ordered(page, start, end, uptodate);

	if (!uptodate) {
		ClearPageUptodate(page);
		SetPageError(page);
		ret = err < 0 ? err : -EIO;
		mapping_set_error(page->mapping, ret);
	}
}

/*
 * after a writepage IO is done, we need to:
 * clear the uptodate bits on error
 * clear the writeback bits in the extent tree for this IO
 * end_page_writeback if the page has no more pending IO
 *
 * Scheduling is not allowed, so the extent state tree is expected
 * to have one and only one object corresponding to this IO.
 */
static void end_bio_extent_writepage(struct bio *bio)
{
	int error = blk_status_to_errno(bio->bi_status);
	struct bio_vec *bvec;
	u64 start;
	u64 end;
	struct bvec_iter_all iter_all;

	ASSERT(!bio_flagged(bio, BIO_CLONED));
	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;
		struct inode *inode = page->mapping->host;
		struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);

		/* We always issue full-page reads, but if some block
		 * in a page fails to read, blk_update_request() will
		 * advance bv_offset and adjust bv_len to compensate.
		 * Print a warning for nonzero offsets, and an error
		 * if they don't add up to a full page.  */
		if (bvec->bv_offset || bvec->bv_len != PAGE_SIZE) {
			if (bvec->bv_offset + bvec->bv_len != PAGE_SIZE)
				btrfs_err(fs_info,
				   "partial page write in btrfs with offset %u and length %u",
					bvec->bv_offset, bvec->bv_len);
			else
				btrfs_info(fs_info,
				   "incomplete page write in btrfs with offset %u and length %u",
					bvec->bv_offset, bvec->bv_len);
		}

		start = page_offset(page);
		end = start + bvec->bv_offset + bvec->bv_len - 1;

		end_extent_writepage(page, error, start, end);
		end_page_writeback(page);
	}

	bio_put(bio);
}

static void
endio_readpage_release_extent(struct extent_io_tree *tree, u64 start, u64 len,
			      int uptodate)
{
	struct extent_state *cached = NULL;
	u64 end = start + len - 1;

	if (uptodate && tree->track_uptodate)
		set_extent_uptodate(tree, start, end, &cached, GFP_ATOMIC);
	unlock_extent_cached_atomic(tree, start, end, &cached);
}

/*
 * after a readpage IO is done, we need to:
 * clear the uptodate bits on error
 * set the uptodate bits if things worked
 * set the page up to date if all extents in the tree are uptodate
 * clear the lock bit in the extent tree
 * unlock the page if there are no other extents locked for it
 *
 * Scheduling is not allowed, so the extent state tree is expected
 * to have one and only one object corresponding to this IO.
 */
static void end_bio_extent_readpage(struct bio *bio)
{
	struct bio_vec *bvec;
	int uptodate = !bio->bi_status;
	struct btrfs_io_bio *io_bio = btrfs_io_bio(bio);
	struct extent_io_tree *tree, *failure_tree;
	u64 offset = 0;
	u64 start;
	u64 end;
	u64 len;
	u64 extent_start = 0;
	u64 extent_len = 0;
	int mirror;
	int ret;
	struct bvec_iter_all iter_all;

	ASSERT(!bio_flagged(bio, BIO_CLONED));
	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;
		struct inode *inode = page->mapping->host;
		struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);
		bool data_inode = btrfs_ino(BTRFS_I(inode))
			!= BTRFS_BTREE_INODE_OBJECTID;

		btrfs_debug(fs_info,
			"end_bio_extent_readpage: bi_sector=%llu, err=%d, mirror=%u",
			(u64)bio->bi_iter.bi_sector, bio->bi_status,
			io_bio->mirror_num);
		tree = &BTRFS_I(inode)->io_tree;
		failure_tree = &BTRFS_I(inode)->io_failure_tree;

		/* We always issue full-page reads, but if some block
		 * in a page fails to read, blk_update_request() will
		 * advance bv_offset and adjust bv_len to compensate.
		 * Print a warning for nonzero offsets, and an error
		 * if they don't add up to a full page.  */
		if (bvec->bv_offset || bvec->bv_len != PAGE_SIZE) {
			if (bvec->bv_offset + bvec->bv_len != PAGE_SIZE)
				btrfs_err(fs_info,
					"partial page read in btrfs with offset %u and length %u",
					bvec->bv_offset, bvec->bv_len);
			else
				btrfs_info(fs_info,
					"incomplete page read in btrfs with offset %u and length %u",
					bvec->bv_offset, bvec->bv_len);
		}

		start = page_offset(page);
		end = start + bvec->bv_offset + bvec->bv_len - 1;
		len = bvec->bv_len;

		mirror = io_bio->mirror_num;
		if (likely(uptodate)) {
			ret = tree->ops->readpage_end_io_hook(io_bio, offset,
							      page, start, end,
							      mirror);
			if (ret)
				uptodate = 0;
			else
				clean_io_failure(BTRFS_I(inode)->root->fs_info,
						 failure_tree, tree, start,
						 page,
						 btrfs_ino(BTRFS_I(inode)), 0);
		}

		if (likely(uptodate))
			goto readpage_ok;

		if (data_inode) {

			/*
			 * The generic bio_readpage_error handles errors the
			 * following way: If possible, new read requests are
			 * created and submitted and will end up in
			 * end_bio_extent_readpage as well (if we're lucky,
			 * not in the !uptodate case). In that case it returns
			 * 0 and we just go on with the next page in our bio.
			 * If it can't handle the error it will return -EIO and
			 * we remain responsible for that page.
			 */
			ret = bio_readpage_error(bio, offset, page, start, end,
						 mirror);
			if (ret == 0) {
				uptodate = !bio->bi_status;
				offset += len;
				continue;
			}
		} else {
			struct extent_buffer *eb;

			eb = (struct extent_buffer *)page->private;
			set_bit(EXTENT_BUFFER_READ_ERR, &eb->bflags);
			eb->read_mirror = mirror;
			atomic_dec(&eb->io_pages);
			if (test_and_clear_bit(EXTENT_BUFFER_READAHEAD,
					       &eb->bflags))
				btree_readahead_hook(eb, -EIO);
		}
readpage_ok:
		if (likely(uptodate)) {
			loff_t i_size = i_size_read(inode);
			pgoff_t end_index = i_size >> PAGE_SHIFT;
			unsigned off;

			/* Zero out the end if this page straddles i_size */
			off = offset_in_page(i_size);
			if (page->index == end_index && off)
				zero_user_segment(page, off, PAGE_SIZE);
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		unlock_page(page);
		offset += len;

		if (unlikely(!uptodate)) {
			if (extent_len) {
				endio_readpage_release_extent(tree,
							      extent_start,
							      extent_len, 1);
				extent_start = 0;
				extent_len = 0;
			}
			endio_readpage_release_extent(tree, start,
						      end - start + 1, 0);
		} else if (!extent_len) {
			extent_start = start;
			extent_len = end + 1 - start;
		} else if (extent_start + extent_len == start) {
			extent_len += end + 1 - start;
		} else {
			endio_readpage_release_extent(tree, extent_start,
						      extent_len, uptodate);
			extent_start = start;
			extent_len = end + 1 - start;
		}
	}

	if (extent_len)
		endio_readpage_release_extent(tree, extent_start, extent_len,
					      uptodate);
	btrfs_io_bio_free_csum(io_bio);
	bio_put(bio);
}

/*
 * Initialize the members up to but not including 'bio'. Use after allocating a
 * new bio by bio_alloc_bioset as it does not initialize the bytes outside of
 * 'bio' because use of __GFP_ZERO is not supported.
 */
static inline void btrfs_io_bio_init(struct btrfs_io_bio *btrfs_bio)
{
	memset(btrfs_bio, 0, offsetof(struct btrfs_io_bio, bio));
}

/*
 * The following helpers allocate a bio. As it's backed by a bioset, it'll
 * never fail.  We're returning a bio right now but you can call btrfs_io_bio
 * for the appropriate container_of magic
 */
struct bio *btrfs_bio_alloc(u64 first_byte)
{
	struct bio *bio;

	bio = bio_alloc_bioset(GFP_NOFS, BIO_MAX_PAGES, &btrfs_bioset);
	bio->bi_iter.bi_sector = first_byte >> 9;
	btrfs_io_bio_init(btrfs_io_bio(bio));
	return bio;
}

struct bio *btrfs_bio_clone(struct bio *bio)
{
	struct btrfs_io_bio *btrfs_bio;
	struct bio *new;

	/* Bio allocation backed by a bioset does not fail */
	new = bio_clone_fast(bio, GFP_NOFS, &btrfs_bioset);
	btrfs_bio = btrfs_io_bio(new);
	btrfs_io_bio_init(btrfs_bio);
	btrfs_bio->iter = bio->bi_iter;
	return new;
}

struct bio *btrfs_io_bio_alloc(unsigned int nr_iovecs)
{
	struct bio *bio;

	/* Bio allocation backed by a bioset does not fail */
	bio = bio_alloc_bioset(GFP_NOFS, nr_iovecs, &btrfs_bioset);
	btrfs_io_bio_init(btrfs_io_bio(bio));
	return bio;
}

struct bio *btrfs_bio_clone_partial(struct bio *orig, int offset, int size)
{
	struct bio *bio;
	struct btrfs_io_bio *btrfs_bio;

	/* this will never fail when it's backed by a bioset */
	bio = bio_clone_fast(orig, GFP_NOFS, &btrfs_bioset);
	ASSERT(bio);

	btrfs_bio = btrfs_io_bio(bio);
	btrfs_io_bio_init(btrfs_bio);

	bio_trim(bio, offset >> 9, size >> 9);
	btrfs_bio->iter = bio->bi_iter;
	return bio;
}

/*
 * @opf:	bio REQ_OP_* and REQ_* flags as one value
 * @tree:	tree so we can call our merge_bio hook
 * @wbc:	optional writeback control for io accounting
 * @page:	page to add to the bio
 * @pg_offset:	offset of the new bio or to check whether we are adding
 *              a contiguous page to the previous one
 * @size:	portion of page that we want to write
 * @offset:	starting offset in the page
 * @bdev:	attach newly created bios to this bdev
 * @bio_ret:	must be valid pointer, newly allocated bio will be stored there
 * @end_io_func:     end_io callback for new bio
 * @mirror_num:	     desired mirror to read/write
 * @prev_bio_flags:  flags of previous bio to see if we can merge the current one
 * @bio_flags:	flags of the current bio to see if we can merge them
 */
static int submit_extent_page(unsigned int opf, struct extent_io_tree *tree,
			      struct writeback_control *wbc,
			      struct page *page, u64 offset,
			      size_t size, unsigned long pg_offset,
			      struct block_device *bdev,
			      struct bio **bio_ret,
			      bio_end_io_t end_io_func,
			      int mirror_num,
			      unsigned long prev_bio_flags,
			      unsigned long bio_flags,
			      bool force_bio_submit)
{
	int ret = 0;
	struct bio *bio;
	size_t page_size = min_t(size_t, size, PAGE_SIZE);
	sector_t sector = offset >> 9;

	ASSERT(bio_ret);

	if (*bio_ret) {
		bool contig;
		bool can_merge = true;

		bio = *bio_ret;
		if (prev_bio_flags & EXTENT_BIO_COMPRESSED)
			contig = bio->bi_iter.bi_sector == sector;
		else
			contig = bio_end_sector(bio) == sector;

		ASSERT(tree->ops);
		if (btrfs_bio_fits_in_stripe(page, page_size, bio, bio_flags))
			can_merge = false;

		if (prev_bio_flags != bio_flags || !contig || !can_merge ||
		    force_bio_submit ||
		    bio_add_page(bio, page, page_size, pg_offset) < page_size) {
			ret = submit_one_bio(bio, mirror_num, prev_bio_flags);
			if (ret < 0) {
				*bio_ret = NULL;
				return ret;
			}
			bio = NULL;
		} else {
			if (wbc)
				wbc_account_cgroup_owner(wbc, page, page_size);
			return 0;
		}
	}

	bio = btrfs_bio_alloc(offset);
	bio_set_dev(bio, bdev);
	bio_add_page(bio, page, page_size, pg_offset);
	bio->bi_end_io = end_io_func;
	bio->bi_private = tree;
	bio->bi_write_hint = page->mapping->host->i_write_hint;
	bio->bi_opf = opf;
	if (wbc) {
		wbc_init_bio(wbc, bio);
		wbc_account_cgroup_owner(wbc, page, page_size);
	}

	*bio_ret = bio;

	return ret;
}

static void attach_extent_buffer_page(struct extent_buffer *eb,
				      struct page *page)
{
	if (!PagePrivate(page)) {
		SetPagePrivate(page);
		get_page(page);
		set_page_private(page, (unsigned long)eb);
	} else {
		WARN_ON(page->private != (unsigned long)eb);
	}
}

void set_page_extent_mapped(struct page *page)
{
	if (!PagePrivate(page)) {
		SetPagePrivate(page);
		get_page(page);
		set_page_private(page, EXTENT_PAGE_PRIVATE);
	}
}

static struct extent_map *
__get_extent_map(struct inode *inode, struct page *page, size_t pg_offset,
		 u64 start, u64 len, get_extent_t *get_extent,
		 struct extent_map **em_cached)
{
	struct extent_map *em;

	if (em_cached && *em_cached) {
		em = *em_cached;
		if (extent_map_in_tree(em) && start >= em->start &&
		    start < extent_map_end(em)) {
			refcount_inc(&em->refs);
			return em;
		}

		free_extent_map(em);
		*em_cached = NULL;
	}

	em = get_extent(BTRFS_I(inode), page, pg_offset, start, len, 0);
	if (em_cached && !IS_ERR_OR_NULL(em)) {
		BUG_ON(*em_cached);
		refcount_inc(&em->refs);
		*em_cached = em;
	}
	return em;
}
/*
 * basic readpage implementation.  Locked extent state structs are inserted
 * into the tree that are removed when the IO is done (by the end_io
 * handlers)
 * XXX JDM: This needs looking at to ensure proper page locking
 * return 0 on success, otherwise return error
 */
static int __do_readpage(struct extent_io_tree *tree,
			 struct page *page,
			 get_extent_t *get_extent,
			 struct extent_map **em_cached,
			 struct bio **bio, int mirror_num,
			 unsigned long *bio_flags, unsigned int read_flags,
			 u64 *prev_em_start)
{
	struct inode *inode = page->mapping->host;
	u64 start = page_offset(page);
	const u64 end = start + PAGE_SIZE - 1;
	u64 cur = start;
	u64 extent_offset;
	u64 last_byte = i_size_read(inode);
	u64 block_start;
	u64 cur_end;
	struct extent_map *em;
	struct block_device *bdev;
	int ret = 0;
	int nr = 0;
	size_t pg_offset = 0;
	size_t iosize;
	size_t disk_io_size;
	size_t blocksize = inode->i_sb->s_blocksize;
	unsigned long this_bio_flag = 0;

	set_page_extent_mapped(page);

	if (!PageUptodate(page)) {
		if (cleancache_get_page(page) == 0) {
			BUG_ON(blocksize != PAGE_SIZE);
			unlock_extent(tree, start, end);
			goto out;
		}
	}

	if (page->index == last_byte >> PAGE_SHIFT) {
		char *userpage;
		size_t zero_offset = offset_in_page(last_byte);

		if (zero_offset) {
			iosize = PAGE_SIZE - zero_offset;
			userpage = kmap_atomic(page);
			memset(userpage + zero_offset, 0, iosize);
			flush_dcache_page(page);
			kunmap_atomic(userpage);
		}
	}
	while (cur <= end) {
		bool force_bio_submit = false;
		u64 offset;

		if (cur >= last_byte) {
			char *userpage;
			struct extent_state *cached = NULL;

			iosize = PAGE_SIZE - pg_offset;
			userpage = kmap_atomic(page);
			memset(userpage + pg_offset, 0, iosize);
			flush_dcache_page(page);
			kunmap_atomic(userpage);
			set_extent_uptodate(tree, cur, cur + iosize - 1,
					    &cached, GFP_NOFS);
			unlock_extent_cached(tree, cur,
					     cur + iosize - 1, &cached);
			break;
		}
		em = __get_extent_map(inode, page, pg_offset, cur,
				      end - cur + 1, get_extent, em_cached);
		if (IS_ERR_OR_NULL(em)) {
			SetPageError(page);
			unlock_extent(tree, cur, end);
			break;
		}
		extent_offset = cur - em->start;
		BUG_ON(extent_map_end(em) <= cur);
		BUG_ON(end < cur);

		if (test_bit(EXTENT_FLAG_COMPRESSED, &em->flags)) {
			this_bio_flag |= EXTENT_BIO_COMPRESSED;
			extent_set_compress_type(&this_bio_flag,
						 em->compress_type);
		}

		iosize = min(extent_map_end(em) - cur, end - cur + 1);
		cur_end = min(extent_map_end(em) - 1, end);
		iosize = ALIGN(iosize, blocksize);
		if (this_bio_flag & EXTENT_BIO_COMPRESSED) {
			disk_io_size = em->block_len;
			offset = em->block_start;
		} else {
			offset = em->block_start + extent_offset;
			disk_io_size = iosize;
		}
		bdev = em->bdev;
		block_start = em->block_start;
		if (test_bit(EXTENT_FLAG_PREALLOC, &em->flags))
			block_start = EXTENT_MAP_HOLE;

		/*
		 * If we have a file range that points to a compressed extent
		 * and it's followed by a consecutive file range that points to
		 * to the same compressed extent (possibly with a different
		 * offset and/or length, so it either points to the whole extent
		 * or only part of it), we must make sure we do not submit a
		 * single bio to populate the pages for the 2 ranges because
		 * this makes the compressed extent read zero out the pages
		 * belonging to the 2nd range. Imagine the following scenario:
		 *
		 *  File layout
		 *  [0 - 8K]                     [8K - 24K]
		 *    |                               |
		 *    |                               |
		 * points to extent X,         points to extent X,
		 * offset 4K, length of 8K     offset 0, length 16K
		 *
		 * [extent X, compressed length = 4K uncompressed length = 16K]
		 *
		 * If the bio to read the compressed extent covers both ranges,
		 * it will decompress extent X into the pages belonging to the
		 * first range and then it will stop, zeroing out the remaining
		 * pages that belong to the other range that points to extent X.
		 * So here we make sure we submit 2 bios, one for the first
		 * range and another one for the third range. Both will target
		 * the same physical extent from disk, but we can't currently
		 * make the compressed bio endio callback populate the pages
		 * for both ranges because each compressed bio is tightly
		 * coupled with a single extent map, and each range can have
		 * an extent map with a different offset value relative to the
		 * uncompressed data of our extent and different lengths. This
		 * is a corner case so we prioritize correctness over
		 * non-optimal behavior (submitting 2 bios for the same extent).
		 */
		if (test_bit(EXTENT_FLAG_COMPRESSED, &em->flags) &&
		    prev_em_start && *prev_em_start != (u64)-1 &&
		    *prev_em_start != em->start)
			force_bio_submit = true;

		if (prev_em_start)
			*prev_em_start = em->start;

		free_extent_map(em);
		em = NULL;

		/* we've found a hole, just zero and go on */
		if (block_start == EXTENT_MAP_HOLE) {
			char *userpage;
			struct extent_state *cached = NULL;

			userpage = kmap_atomic(page);
			memset(userpage + pg_offset, 0, iosize);
			flush_dcache_page(page);
			kunmap_atomic(userpage);

			set_extent_uptodate(tree, cur, cur + iosize - 1,
					    &cached, GFP_NOFS);
			unlock_extent_cached(tree, cur,
					     cur + iosize - 1, &cached);
			cur = cur + iosize;
			pg_offset += iosize;
			continue;
		}
		/* the get_extent function already copied into the page */
		if (test_range_bit(tree, cur, cur_end,
				   EXTENT_UPTODATE, 1, NULL)) {
			check_page_uptodate(tree, page);
			unlock_extent(tree, cur, cur + iosize - 1);
			cur = cur + iosize;
			pg_offset += iosize;
			continue;
		}
		/* we have an inline extent but it didn't get marked up
		 * to date.  Error out
		 */
		if (block_start == EXTENT_MAP_INLINE) {
			SetPageError(page);
			unlock_extent(tree, cur, cur + iosize - 1);
			cur = cur + iosize;
			pg_offset += iosize;
			continue;
		}

		ret = submit_extent_page(REQ_OP_READ | read_flags, tree, NULL,
					 page, offset, disk_io_size,
					 pg_offset, bdev, bio,
					 end_bio_extent_readpage, mirror_num,
					 *bio_flags,
					 this_bio_flag,
					 force_bio_submit);
		if (!ret) {
			nr++;
			*bio_flags = this_bio_flag;
		} else {
			SetPageError(page);
			unlock_extent(tree, cur, cur + iosize - 1);
			goto out;
		}
		cur = cur + iosize;
		pg_offset += iosize;
	}
out:
	if (!nr) {
		if (!PageError(page))
			SetPageUptodate(page);
		unlock_page(page);
	}
	return ret;
}

static inline void contiguous_readpages(struct extent_io_tree *tree,
					     struct page *pages[], int nr_pages,
					     u64 start, u64 end,
					     struct extent_map **em_cached,
					     struct bio **bio,
					     unsigned long *bio_flags,
					     u64 *prev_em_start)
{
	struct btrfs_inode *inode = BTRFS_I(pages[0]->mapping->host);
	int index;

	btrfs_lock_and_flush_ordered_range(tree, inode, start, end, NULL);

	for (index = 0; index < nr_pages; index++) {
		__do_readpage(tree, pages[index], btrfs_get_extent, em_cached,
				bio, 0, bio_flags, REQ_RAHEAD, prev_em_start);
		put_page(pages[index]);
	}
}

static int __extent_read_full_page(struct extent_io_tree *tree,
				   struct page *page,
				   get_extent_t *get_extent,
				   struct bio **bio, int mirror_num,
				   unsigned long *bio_flags,
				   unsigned int read_flags)
{
	struct btrfs_inode *inode = BTRFS_I(page->mapping->host);
	u64 start = page_offset(page);
	u64 end = start + PAGE_SIZE - 1;
	int ret;

	btrfs_lock_and_flush_ordered_range(tree, inode, start, end, NULL);

	ret = __do_readpage(tree, page, get_extent, NULL, bio, mirror_num,
			    bio_flags, read_flags, NULL);
	return ret;
}

int extent_read_full_page(struct extent_io_tree *tree, struct page *page,
			    get_extent_t *get_extent, int mirror_num)
{
	struct bio *bio = NULL;
	unsigned long bio_flags = 0;
	int ret;

	ret = __extent_read_full_page(tree, page, get_extent, &bio, mirror_num,
				      &bio_flags, 0);
	if (bio)
		ret = submit_one_bio(bio, mirror_num, bio_flags);
	return ret;
}

static void update_nr_written(struct writeback_control *wbc,
			      unsigned long nr_written)
{
	wbc->nr_to_write -= nr_written;
}

/*
 * helper for __extent_writepage, doing all of the delayed allocation setup.
 *
 * This returns 1 if btrfs_run_delalloc_range function did all the work required
 * to write the page (copy into inline extent).  In this case the IO has
 * been started and the page is already unlocked.
 *
 * This returns 0 if all went well (page still locked)
 * This returns < 0 if there were errors (page still locked)
 */
static noinline_for_stack int writepage_delalloc(struct inode *inode,
		struct page *page, struct writeback_control *wbc,
		u64 delalloc_start, unsigned long *nr_written)
{
	u64 page_end = delalloc_start + PAGE_SIZE - 1;
	bool found;
	u64 delalloc_to_write = 0;
	u64 delalloc_end = 0;
	int ret;
	int page_started = 0;


	while (delalloc_end < page_end) {
		found = find_lock_delalloc_range(inode, page,
					       &delalloc_start,
					       &delalloc_end);
		if (!found) {
			delalloc_start = delalloc_end + 1;
			continue;
		}
		ret = btrfs_run_delalloc_range(inode, page, delalloc_start,
				delalloc_end, &page_started, nr_written, wbc);
		if (ret) {
			SetPageError(page);
			/*
			 * btrfs_run_delalloc_range should return < 0 for error
			 * but just in case, we use > 0 here meaning the IO is
			 * started, so we don't want to return > 0 unless
			 * things are going well.
			 */
			ret = ret < 0 ? ret : -EIO;
			goto done;
		}
		/*
		 * delalloc_end is already one less than the total length, so
		 * we don't subtract one from PAGE_SIZE
		 */
		delalloc_to_write += (delalloc_end - delalloc_start +
				      PAGE_SIZE) >> PAGE_SHIFT;
		delalloc_start = delalloc_end + 1;
	}
	if (wbc->nr_to_write < delalloc_to_write) {
		int thresh = 8192;

		if (delalloc_to_write < thresh * 2)
			thresh = delalloc_to_write;
		wbc->nr_to_write = min_t(u64, delalloc_to_write,
					 thresh);
	}

	/* did the fill delalloc function already unlock and start
	 * the IO?
	 */
	if (page_started) {
		/*
		 * we've unlocked the page, so we can't update
		 * the mapping's writeback index, just update
		 * nr_to_write.
		 */
		wbc->nr_to_write -= *nr_written;
		return 1;
	}

	ret = 0;

done:
	return ret;
}

/*
 * helper for __extent_writepage.  This calls the writepage start hooks,
 * and does the loop to map the page into extents and bios.
 *
 * We return 1 if the IO is started and the page is unlocked,
 * 0 if all went well (page still locked)
 * < 0 if there were errors (page still locked)
 */
static noinline_for_stack int __extent_writepage_io(struct inode *inode,
				 struct page *page,
				 struct writeback_control *wbc,
				 struct extent_page_data *epd,
				 loff_t i_size,
				 unsigned long nr_written,
				 unsigned int write_flags, int *nr_ret)
{
	struct extent_io_tree *tree = epd->tree;
	u64 start = page_offset(page);
	u64 page_end = start + PAGE_SIZE - 1;
	u64 end;
	u64 cur = start;
	u64 extent_offset;
	u64 block_start;
	u64 iosize;
	struct extent_map *em;
	struct block_device *bdev;
	size_t pg_offset = 0;
	size_t blocksize;
	int ret = 0;
	int nr = 0;
	bool compressed;

	ret = btrfs_writepage_cow_fixup(page, start, page_end);
	if (ret) {
		/* Fixup worker will requeue */
		if (ret == -EBUSY)
			wbc->pages_skipped++;
		else
			redirty_page_for_writepage(wbc, page);

		update_nr_written(wbc, nr_written);
		unlock_page(page);
		return 1;
	}

	/*
	 * we don't want to touch the inode after unlocking the page,
	 * so we update the mapping writeback index now
	 */
	update_nr_written(wbc, nr_written + 1);

	end = page_end;
	if (i_size <= start) {
		btrfs_writepage_endio_finish_ordered(page, start, page_end, 1);
		goto done;
	}

	blocksize = inode->i_sb->s_blocksize;

	while (cur <= end) {
		u64 em_end;
		u64 offset;

		if (cur >= i_size) {
			btrfs_writepage_endio_finish_ordered(page, cur,
							     page_end, 1);
			break;
		}
		em = btrfs_get_extent(BTRFS_I(inode), page, pg_offset, cur,
				     end - cur + 1, 1);
		if (IS_ERR_OR_NULL(em)) {
			SetPageError(page);
			ret = PTR_ERR_OR_ZERO(em);
			break;
		}

		extent_offset = cur - em->start;
		em_end = extent_map_end(em);
		BUG_ON(em_end <= cur);
		BUG_ON(end < cur);
		iosize = min(em_end - cur, end - cur + 1);
		iosize = ALIGN(iosize, blocksize);
		offset = em->block_start + extent_offset;
		bdev = em->bdev;
		block_start = em->block_start;
		compressed = test_bit(EXTENT_FLAG_COMPRESSED, &em->flags);
		free_extent_map(em);
		em = NULL;

		/*
		 * compressed and inline extents are written through other
		 * paths in the FS
		 */
		if (compressed || block_start == EXTENT_MAP_HOLE ||
		    block_start == EXTENT_MAP_INLINE) {
			/*
			 * end_io notification does not happen here for
			 * compressed extents
			 */
			if (!compressed)
				btrfs_writepage_endio_finish_ordered(page, cur,
							    cur + iosize - 1,
							    1);
			else if (compressed) {
				/* we don't want to end_page_writeback on
				 * a compressed extent.  this happens
				 * elsewhere
				 */
				nr++;
			}

			cur += iosize;
			pg_offset += iosize;
			continue;
		}

		btrfs_set_range_writeback(tree, cur, cur + iosize - 1);
		if (!PageWriteback(page)) {
			btrfs_err(BTRFS_I(inode)->root->fs_info,
				   "page %lu not writeback, cur %llu end %llu",
			       page->index, cur, end);
		}

		ret = submit_extent_page(REQ_OP_WRITE | write_flags, tree, wbc,
					 page, offset, iosize, pg_offset,
					 bdev, &epd->bio,
					 end_bio_extent_writepage,
					 0, 0, 0, false);
		if (ret) {
			SetPageError(page);
			if (PageWriteback(page))
				end_page_writeback(page);
		}

		cur = cur + iosize;
		pg_offset += iosize;
		nr++;
	}
done:
	*nr_ret = nr;
	return ret;
}

/*
 * the writepage semantics are similar to regular writepage.  extent
 * records are inserted to lock ranges in the tree, and as dirty areas
 * are found, they are marked writeback.  Then the lock bits are removed
 * and the end_io handler clears the writeback ranges
 *
 * Return 0 if everything goes well.
 * Return <0 for error.
 */
static int __extent_writepage(struct page *page, struct writeback_control *wbc,
			      struct extent_page_data *epd)
{
	struct inode *inode = page->mapping->host;
	u64 start = page_offset(page);
	u64 page_end = start + PAGE_SIZE - 1;
	int ret;
	int nr = 0;
	size_t pg_offset = 0;
	loff_t i_size = i_size_read(inode);
	unsigned long end_index = i_size >> PAGE_SHIFT;
	unsigned int write_flags = 0;
	unsigned long nr_written = 0;

	write_flags = wbc_to_write_flags(wbc);

	trace___extent_writepage(page, inode, wbc);

	WARN_ON(!PageLocked(page));

	ClearPageError(page);

	pg_offset = offset_in_page(i_size);
	if (page->index > end_index ||
	   (page->index == end_index && !pg_offset)) {
		page->mapping->a_ops->invalidatepage(page, 0, PAGE_SIZE);
		unlock_page(page);
		return 0;
	}

	if (page->index == end_index) {
		char *userpage;

		userpage = kmap_atomic(page);
		memset(userpage + pg_offset, 0,
		       PAGE_SIZE - pg_offset);
		kunmap_atomic(userpage);
		flush_dcache_page(page);
	}

	pg_offset = 0;

	set_page_extent_mapped(page);

	if (!epd->extent_locked) {
		ret = writepage_delalloc(inode, page, wbc, start, &nr_written);
		if (ret == 1)
			goto done_unlocked;
		if (ret)
			goto done;
	}

	ret = __extent_writepage_io(inode, page, wbc, epd,
				    i_size, nr_written, write_flags, &nr);
	if (ret == 1)
		goto done_unlocked;

done:
	if (nr == 0) {
		/* make sure the mapping tag for page dirty gets cleared */
		set_page_writeback(page);
		end_page_writeback(page);
	}
	if (PageError(page)) {
		ret = ret < 0 ? ret : -EIO;
		end_extent_writepage(page, ret, start, page_end);
	}
	unlock_page(page);
	ASSERT(ret <= 0);
	return ret;

done_unlocked:
	return 0;
}

void wait_on_extent_buffer_writeback(struct extent_buffer *eb)
{
	wait_on_bit_io(&eb->bflags, EXTENT_BUFFER_WRITEBACK,
		       TASK_UNINTERRUPTIBLE);
}

static void end_extent_buffer_writeback(struct extent_buffer *eb)
{
	clear_bit(EXTENT_BUFFER_WRITEBACK, &eb->bflags);
	smp_mb__after_atomic();
	wake_up_bit(&eb->bflags, EXTENT_BUFFER_WRITEBACK);
}

/*
 * Lock eb pages and flush the bio if we can't the locks
 *
 * Return  0 if nothing went wrong
 * Return >0 is same as 0, except bio is not submitted
 * Return <0 if something went wrong, no page is locked
 */
static noinline_for_stack int lock_extent_buffer_for_io(struct extent_buffer *eb,
			  struct extent_page_data *epd)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	int i, num_pages, failed_page_nr;
	int flush = 0;
	int ret = 0;

	if (!btrfs_try_tree_write_lock(eb)) {
		ret = flush_write_bio(epd);
		if (ret < 0)
			return ret;
		flush = 1;
		btrfs_tree_lock(eb);
	}

	if (test_bit(EXTENT_BUFFER_WRITEBACK, &eb->bflags)) {
		btrfs_tree_unlock(eb);
		if (!epd->sync_io)
			return 0;
		if (!flush) {
			ret = flush_write_bio(epd);
			if (ret < 0)
				return ret;
			flush = 1;
		}
		while (1) {
			wait_on_extent_buffer_writeback(eb);
			btrfs_tree_lock(eb);
			if (!test_bit(EXTENT_BUFFER_WRITEBACK, &eb->bflags))
				break;
			btrfs_tree_unlock(eb);
		}
	}

	/*
	 * We need to do this to prevent races in people who check if the eb is
	 * under IO since we can end up having no IO bits set for a short period
	 * of time.
	 */
	spin_lock(&eb->refs_lock);
	if (test_and_clear_bit(EXTENT_BUFFER_DIRTY, &eb->bflags)) {
		set_bit(EXTENT_BUFFER_WRITEBACK, &eb->bflags);
		spin_unlock(&eb->refs_lock);
		btrfs_set_header_flag(eb, BTRFS_HEADER_FLAG_WRITTEN);
		percpu_counter_add_batch(&fs_info->dirty_metadata_bytes,
					 -eb->len,
					 fs_info->dirty_metadata_batch);
		ret = 1;
	} else {
		spin_unlock(&eb->refs_lock);
	}

	btrfs_tree_unlock(eb);

	if (!ret)
		return ret;

	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		struct page *p = eb->pages[i];

		if (!trylock_page(p)) {
			if (!flush) {
				int err;

				err = flush_write_bio(epd);
				if (err < 0) {
					ret = err;
					failed_page_nr = i;
					goto err_unlock;
				}
				flush = 1;
			}
			lock_page(p);
		}
	}

	return ret;
err_unlock:
	/* Unlock already locked pages */
	for (i = 0; i < failed_page_nr; i++)
		unlock_page(eb->pages[i]);
	/*
	 * Clear EXTENT_BUFFER_WRITEBACK and wake up anyone waiting on it.
	 * Also set back EXTENT_BUFFER_DIRTY so future attempts to this eb can
	 * be made and undo everything done before.
	 */
	btrfs_tree_lock(eb);
	spin_lock(&eb->refs_lock);
	set_bit(EXTENT_BUFFER_DIRTY, &eb->bflags);
	end_extent_buffer_writeback(eb);
	spin_unlock(&eb->refs_lock);
	percpu_counter_add_batch(&fs_info->dirty_metadata_bytes, eb->len,
				 fs_info->dirty_metadata_batch);
	btrfs_clear_header_flag(eb, BTRFS_HEADER_FLAG_WRITTEN);
	btrfs_tree_unlock(eb);
	return ret;
}

static void set_btree_ioerr(struct page *page)
{
	struct extent_buffer *eb = (struct extent_buffer *)page->private;
	struct btrfs_fs_info *fs_info;

	SetPageError(page);
	if (test_and_set_bit(EXTENT_BUFFER_WRITE_ERR, &eb->bflags))
		return;

	/*
	 * If we error out, we should add back the dirty_metadata_bytes
	 * to make it consistent.
	 */
	fs_info = eb->fs_info;
	percpu_counter_add_batch(&fs_info->dirty_metadata_bytes,
				 eb->len, fs_info->dirty_metadata_batch);

	/*
	 * If writeback for a btree extent that doesn't belong to a log tree
	 * failed, increment the counter transaction->eb_write_errors.
	 * We do this because while the transaction is running and before it's
	 * committing (when we call filemap_fdata[write|wait]_range against
	 * the btree inode), we might have
	 * btree_inode->i_mapping->a_ops->writepages() called by the VM - if it
	 * returns an error or an error happens during writeback, when we're
	 * committing the transaction we wouldn't know about it, since the pages
	 * can be no longer dirty nor marked anymore for writeback (if a
	 * subsequent modification to the extent buffer didn't happen before the
	 * transaction commit), which makes filemap_fdata[write|wait]_range not
	 * able to find the pages tagged with SetPageError at transaction
	 * commit time. So if this happens we must abort the transaction,
	 * otherwise we commit a super block with btree roots that point to
	 * btree nodes/leafs whose content on disk is invalid - either garbage
	 * or the content of some node/leaf from a past generation that got
	 * cowed or deleted and is no longer valid.
	 *
	 * Note: setting AS_EIO/AS_ENOSPC in the btree inode's i_mapping would
	 * not be enough - we need to distinguish between log tree extents vs
	 * non-log tree extents, and the next filemap_fdatawait_range() call
	 * will catch and clear such errors in the mapping - and that call might
	 * be from a log sync and not from a transaction commit. Also, checking
	 * for the eb flag EXTENT_BUFFER_WRITE_ERR at transaction commit time is
	 * not done and would not be reliable - the eb might have been released
	 * from memory and reading it back again means that flag would not be
	 * set (since it's a runtime flag, not persisted on disk).
	 *
	 * Using the flags below in the btree inode also makes us achieve the
	 * goal of AS_EIO/AS_ENOSPC when writepages() returns success, started
	 * writeback for all dirty pages and before filemap_fdatawait_range()
	 * is called, the writeback for all dirty pages had already finished
	 * with errors - because we were not using AS_EIO/AS_ENOSPC,
	 * filemap_fdatawait_range() would return success, as it could not know
	 * that writeback errors happened (the pages were no longer tagged for
	 * writeback).
	 */
	switch (eb->log_index) {
	case -1:
		set_bit(BTRFS_FS_BTREE_ERR, &eb->fs_info->flags);
		break;
	case 0:
		set_bit(BTRFS_FS_LOG1_ERR, &eb->fs_info->flags);
		break;
	case 1:
		set_bit(BTRFS_FS_LOG2_ERR, &eb->fs_info->flags);
		break;
	default:
		BUG(); /* unexpected, logic error */
	}
}

static void end_bio_extent_buffer_writepage(struct bio *bio)
{
	struct bio_vec *bvec;
	struct extent_buffer *eb;
	int done;
	struct bvec_iter_all iter_all;

	ASSERT(!bio_flagged(bio, BIO_CLONED));
	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;

		eb = (struct extent_buffer *)page->private;
		BUG_ON(!eb);
		done = atomic_dec_and_test(&eb->io_pages);

		if (bio->bi_status ||
		    test_bit(EXTENT_BUFFER_WRITE_ERR, &eb->bflags)) {
			ClearPageUptodate(page);
			set_btree_ioerr(page);
		}

		end_page_writeback(page);

		if (!done)
			continue;

		end_extent_buffer_writeback(eb);
	}

	bio_put(bio);
}

static noinline_for_stack int write_one_eb(struct extent_buffer *eb,
			struct writeback_control *wbc,
			struct extent_page_data *epd)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	struct block_device *bdev = fs_info->fs_devices->latest_bdev;
	struct extent_io_tree *tree = &BTRFS_I(fs_info->btree_inode)->io_tree;
	u64 offset = eb->start;
	u32 nritems;
	int i, num_pages;
	unsigned long start, end;
	unsigned int write_flags = wbc_to_write_flags(wbc) | REQ_META;
	int ret = 0;

	clear_bit(EXTENT_BUFFER_WRITE_ERR, &eb->bflags);
	num_pages = num_extent_pages(eb);
	atomic_set(&eb->io_pages, num_pages);

	/* set btree blocks beyond nritems with 0 to avoid stale content. */
	nritems = btrfs_header_nritems(eb);
	if (btrfs_header_level(eb) > 0) {
		end = btrfs_node_key_ptr_offset(nritems);

		memzero_extent_buffer(eb, end, eb->len - end);
	} else {
		/*
		 * leaf:
		 * header 0 1 2 .. N ... data_N .. data_2 data_1 data_0
		 */
		start = btrfs_item_nr_offset(nritems);
		end = BTRFS_LEAF_DATA_OFFSET + leaf_data_end(eb);
		memzero_extent_buffer(eb, start, end - start);
	}

	for (i = 0; i < num_pages; i++) {
		struct page *p = eb->pages[i];

		clear_page_dirty_for_io(p);
		set_page_writeback(p);
		ret = submit_extent_page(REQ_OP_WRITE | write_flags, tree, wbc,
					 p, offset, PAGE_SIZE, 0, bdev,
					 &epd->bio,
					 end_bio_extent_buffer_writepage,
					 0, 0, 0, false);
		if (ret) {
			set_btree_ioerr(p);
			if (PageWriteback(p))
				end_page_writeback(p);
			if (atomic_sub_and_test(num_pages - i, &eb->io_pages))
				end_extent_buffer_writeback(eb);
			ret = -EIO;
			break;
		}
		offset += PAGE_SIZE;
		update_nr_written(wbc, 1);
		unlock_page(p);
	}

	if (unlikely(ret)) {
		for (; i < num_pages; i++) {
			struct page *p = eb->pages[i];
			clear_page_dirty_for_io(p);
			unlock_page(p);
		}
	}

	return ret;
}

int btree_write_cache_pages(struct address_space *mapping,
				   struct writeback_control *wbc)
{
	struct extent_io_tree *tree = &BTRFS_I(mapping->host)->io_tree;
	struct extent_buffer *eb, *prev_eb = NULL;
	struct extent_page_data epd = {
		.bio = NULL,
		.tree = tree,
		.extent_locked = 0,
		.sync_io = wbc->sync_mode == WB_SYNC_ALL,
	};
	int ret = 0;
	int done = 0;
	int nr_to_write_done = 0;
	struct pagevec pvec;
	int nr_pages;
	pgoff_t index;
	pgoff_t end;		/* Inclusive */
	int scanned = 0;
	xa_mark_t tag;

	pagevec_init(&pvec);
	if (wbc->range_cyclic) {
		index = mapping->writeback_index; /* Start from prev offset */
		end = -1;
	} else {
		index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;
		scanned = 1;
	}
	if (wbc->sync_mode == WB_SYNC_ALL)
		tag = PAGECACHE_TAG_TOWRITE;
	else
		tag = PAGECACHE_TAG_DIRTY;
retry:
	if (wbc->sync_mode == WB_SYNC_ALL)
		tag_pages_for_writeback(mapping, index, end);
	while (!done && !nr_to_write_done && (index <= end) &&
	       (nr_pages = pagevec_lookup_range_tag(&pvec, mapping, &index, end,
			tag))) {
		unsigned i;

		scanned = 1;
		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			if (!PagePrivate(page))
				continue;

			spin_lock(&mapping->private_lock);
			if (!PagePrivate(page)) {
				spin_unlock(&mapping->private_lock);
				continue;
			}

			eb = (struct extent_buffer *)page->private;

			/*
			 * Shouldn't happen and normally this would be a BUG_ON
			 * but no sense in crashing the users box for something
			 * we can survive anyway.
			 */
			if (WARN_ON(!eb)) {
				spin_unlock(&mapping->private_lock);
				continue;
			}

			if (eb == prev_eb) {
				spin_unlock(&mapping->private_lock);
				continue;
			}

			ret = atomic_inc_not_zero(&eb->refs);
			spin_unlock(&mapping->private_lock);
			if (!ret)
				continue;

			prev_eb = eb;
			ret = lock_extent_buffer_for_io(eb, &epd);
			if (!ret) {
				free_extent_buffer(eb);
				continue;
			} else if (ret < 0) {
				done = 1;
				free_extent_buffer(eb);
				break;
			}

			ret = write_one_eb(eb, wbc, &epd);
			if (ret) {
				done = 1;
				free_extent_buffer(eb);
				break;
			}
			free_extent_buffer(eb);

			/*
			 * the filesystem may choose to bump up nr_to_write.
			 * We have to make sure to honor the new nr_to_write
			 * at any time
			 */
			nr_to_write_done = wbc->nr_to_write <= 0;
		}
		pagevec_release(&pvec);
		cond_resched();
	}
	if (!scanned && !done) {
		/*
		 * We hit the last page and there is more work to be done: wrap
		 * back to the start of the file
		 */
		scanned = 1;
		index = 0;
		goto retry;
	}
	ASSERT(ret <= 0);
	if (ret < 0) {
		end_write_bio(&epd, ret);
		return ret;
	}
	ret = flush_write_bio(&epd);
	return ret;
}

/**
 * write_cache_pages - walk the list of dirty pages of the given address space and write all of them.
 * @mapping: address space structure to write
 * @wbc: subtract the number of written pages from *@wbc->nr_to_write
 * @data: data passed to __extent_writepage function
 *
 * If a page is already under I/O, write_cache_pages() skips it, even
 * if it's dirty.  This is desirable behaviour for memory-cleaning writeback,
 * but it is INCORRECT for data-integrity system calls such as fsync().  fsync()
 * and msync() need to guarantee that all the data which was dirty at the time
 * the call was made get new I/O started against them.  If wbc->sync_mode is
 * WB_SYNC_ALL then we were called for data integrity and we must wait for
 * existing IO to complete.
 */
static int extent_write_cache_pages(struct address_space *mapping,
			     struct writeback_control *wbc,
			     struct extent_page_data *epd)
{
	struct inode *inode = mapping->host;
	int ret = 0;
	int done = 0;
	int nr_to_write_done = 0;
	struct pagevec pvec;
	int nr_pages;
	pgoff_t index;
	pgoff_t end;		/* Inclusive */
	pgoff_t done_index;
	int range_whole = 0;
	int scanned = 0;
	xa_mark_t tag;

	/*
	 * We have to hold onto the inode so that ordered extents can do their
	 * work when the IO finishes.  The alternative to this is failing to add
	 * an ordered extent if the igrab() fails there and that is a huge pain
	 * to deal with, so instead just hold onto the inode throughout the
	 * writepages operation.  If it fails here we are freeing up the inode
	 * anyway and we'd rather not waste our time writing out stuff that is
	 * going to be truncated anyway.
	 */
	if (!igrab(inode))
		return 0;

	pagevec_init(&pvec);
	if (wbc->range_cyclic) {
		index = mapping->writeback_index; /* Start from prev offset */
		end = -1;
	} else {
		index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;
		if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
			range_whole = 1;
		scanned = 1;
	}

	/*
	 * We do the tagged writepage as long as the snapshot flush bit is set
	 * and we are the first one who do the filemap_flush() on this inode.
	 *
	 * The nr_to_write == LONG_MAX is needed to make sure other flushers do
	 * not race in and drop the bit.
	 */
	if (range_whole && wbc->nr_to_write == LONG_MAX &&
	    test_and_clear_bit(BTRFS_INODE_SNAPSHOT_FLUSH,
			       &BTRFS_I(inode)->runtime_flags))
		wbc->tagged_writepages = 1;

	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag = PAGECACHE_TAG_TOWRITE;
	else
		tag = PAGECACHE_TAG_DIRTY;
retry:
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag_pages_for_writeback(mapping, index, end);
	done_index = index;
	while (!done && !nr_to_write_done && (index <= end) &&
			(nr_pages = pagevec_lookup_range_tag(&pvec, mapping,
						&index, end, tag))) {
		unsigned i;

		scanned = 1;
		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			done_index = page->index;
			/*
			 * At this point we hold neither the i_pages lock nor
			 * the page lock: the page may be truncated or
			 * invalidated (changing page->mapping to NULL),
			 * or even swizzled back from swapper_space to
			 * tmpfs file mapping
			 */
			if (!trylock_page(page)) {
				ret = flush_write_bio(epd);
				BUG_ON(ret < 0);
				lock_page(page);
			}

			if (unlikely(page->mapping != mapping)) {
				unlock_page(page);
				continue;
			}

			if (wbc->sync_mode != WB_SYNC_NONE) {
				if (PageWriteback(page)) {
					ret = flush_write_bio(epd);
					BUG_ON(ret < 0);
				}
				wait_on_page_writeback(page);
			}

			if (PageWriteback(page) ||
			    !clear_page_dirty_for_io(page)) {
				unlock_page(page);
				continue;
			}

			ret = __extent_writepage(page, wbc, epd);
			if (ret < 0) {
				/*
				 * done_index is set past this page,
				 * so media errors will not choke
				 * background writeout for the entire
				 * file. This has consequences for
				 * range_cyclic semantics (ie. it may
				 * not be suitable for data integrity
				 * writeout).
				 */
				done_index = page->index + 1;
				done = 1;
				break;
			}

			/*
			 * the filesystem may choose to bump up nr_to_write.
			 * We have to make sure to honor the new nr_to_write
			 * at any time
			 */
			nr_to_write_done = wbc->nr_to_write <= 0;
		}
		pagevec_release(&pvec);
		cond_resched();
	}
	if (!scanned && !done) {
		/*
		 * We hit the last page and there is more work to be done: wrap
		 * back to the start of the file
		 */
		scanned = 1;
		index = 0;
		goto retry;
	}

	if (wbc->range_cyclic || (wbc->nr_to_write > 0 && range_whole))
		mapping->writeback_index = done_index;

	btrfs_add_delayed_iput(inode);
	return ret;
}

int extent_write_full_page(struct page *page, struct writeback_control *wbc)
{
	int ret;
	struct extent_page_data epd = {
		.bio = NULL,
		.tree = &BTRFS_I(page->mapping->host)->io_tree,
		.extent_locked = 0,
		.sync_io = wbc->sync_mode == WB_SYNC_ALL,
	};

	ret = __extent_writepage(page, wbc, &epd);
	ASSERT(ret <= 0);
	if (ret < 0) {
		end_write_bio(&epd, ret);
		return ret;
	}

	ret = flush_write_bio(&epd);
	ASSERT(ret <= 0);
	return ret;
}

int extent_write_locked_range(struct inode *inode, u64 start, u64 end,
			      int mode)
{
	int ret = 0;
	struct address_space *mapping = inode->i_mapping;
	struct extent_io_tree *tree = &BTRFS_I(inode)->io_tree;
	struct page *page;
	unsigned long nr_pages = (end - start + PAGE_SIZE) >>
		PAGE_SHIFT;

	struct extent_page_data epd = {
		.bio = NULL,
		.tree = tree,
		.extent_locked = 1,
		.sync_io = mode == WB_SYNC_ALL,
	};
	struct writeback_control wbc_writepages = {
		.sync_mode	= mode,
		.nr_to_write	= nr_pages * 2,
		.range_start	= start,
		.range_end	= end + 1,
	};

	while (start <= end) {
		page = find_get_page(mapping, start >> PAGE_SHIFT);
		if (clear_page_dirty_for_io(page))
			ret = __extent_writepage(page, &wbc_writepages, &epd);
		else {
			btrfs_writepage_endio_finish_ordered(page, start,
						    start + PAGE_SIZE - 1, 1);
			unlock_page(page);
		}
		put_page(page);
		start += PAGE_SIZE;
	}

	ASSERT(ret <= 0);
	if (ret < 0) {
		end_write_bio(&epd, ret);
		return ret;
	}
	ret = flush_write_bio(&epd);
	return ret;
}

int extent_writepages(struct address_space *mapping,
		      struct writeback_control *wbc)
{
	int ret = 0;
	struct extent_page_data epd = {
		.bio = NULL,
		.tree = &BTRFS_I(mapping->host)->io_tree,
		.extent_locked = 0,
		.sync_io = wbc->sync_mode == WB_SYNC_ALL,
	};

	ret = extent_write_cache_pages(mapping, wbc, &epd);
	ASSERT(ret <= 0);
	if (ret < 0) {
		end_write_bio(&epd, ret);
		return ret;
	}
	ret = flush_write_bio(&epd);
	return ret;
}

int extent_readpages(struct address_space *mapping, struct list_head *pages,
		     unsigned nr_pages)
{
	struct bio *bio = NULL;
	unsigned long bio_flags = 0;
	struct page *pagepool[16];
	struct extent_map *em_cached = NULL;
	struct extent_io_tree *tree = &BTRFS_I(mapping->host)->io_tree;
	int nr = 0;
	u64 prev_em_start = (u64)-1;

	while (!list_empty(pages)) {
		u64 contig_end = 0;

		for (nr = 0; nr < ARRAY_SIZE(pagepool) && !list_empty(pages);) {
			struct page *page = lru_to_page(pages);

			prefetchw(&page->flags);
			list_del(&page->lru);
			if (add_to_page_cache_lru(page, mapping, page->index,
						readahead_gfp_mask(mapping))) {
				put_page(page);
				break;
			}

			pagepool[nr++] = page;
			contig_end = page_offset(page) + PAGE_SIZE - 1;
		}

		if (nr) {
			u64 contig_start = page_offset(pagepool[0]);

			ASSERT(contig_start + nr * PAGE_SIZE - 1 == contig_end);

			contiguous_readpages(tree, pagepool, nr, contig_start,
				     contig_end, &em_cached, &bio, &bio_flags,
				     &prev_em_start);
		}
	}

	if (em_cached)
		free_extent_map(em_cached);

	if (bio)
		return submit_one_bio(bio, 0, bio_flags);
	return 0;
}

/*
 * basic invalidatepage code, this waits on any locked or writeback
 * ranges corresponding to the page, and then deletes any extent state
 * records from the tree
 */
int extent_invalidatepage(struct extent_io_tree *tree,
			  struct page *page, unsigned long offset)
{
	struct extent_state *cached_state = NULL;
	u64 start = page_offset(page);
	u64 end = start + PAGE_SIZE - 1;
	size_t blocksize = page->mapping->host->i_sb->s_blocksize;

	start += ALIGN(offset, blocksize);
	if (start > end)
		return 0;

	lock_extent_bits(tree, start, end, &cached_state);
	wait_on_page_writeback(page);
	clear_extent_bit(tree, start, end, EXTENT_LOCKED | EXTENT_DELALLOC |
			 EXTENT_DO_ACCOUNTING, 1, 1, &cached_state);
	return 0;
}

/*
 * a helper for releasepage, this tests for areas of the page that
 * are locked or under IO and drops the related state bits if it is safe
 * to drop the page.
 */
static int try_release_extent_state(struct extent_io_tree *tree,
				    struct page *page, gfp_t mask)
{
	u64 start = page_offset(page);
	u64 end = start + PAGE_SIZE - 1;
	int ret = 1;

	if (test_range_bit(tree, start, end, EXTENT_LOCKED, 0, NULL)) {
		ret = 0;
	} else {
		/*
		 * at this point we can safely clear everything except the
		 * locked bit and the nodatasum bit
		 */
		ret = __clear_extent_bit(tree, start, end,
				 ~(EXTENT_LOCKED | EXTENT_NODATASUM),
				 0, 0, NULL, mask, NULL);

		/* if clear_extent_bit failed for enomem reasons,
		 * we can't allow the release to continue.
		 */
		if (ret < 0)
			ret = 0;
		else
			ret = 1;
	}
	return ret;
}

/*
 * a helper for releasepage.  As long as there are no locked extents
 * in the range corresponding to the page, both state records and extent
 * map records are removed
 */
int try_release_extent_mapping(struct page *page, gfp_t mask)
{
	struct extent_map *em;
	u64 start = page_offset(page);
	u64 end = start + PAGE_SIZE - 1;
	struct btrfs_inode *btrfs_inode = BTRFS_I(page->mapping->host);
	struct extent_io_tree *tree = &btrfs_inode->io_tree;
	struct extent_map_tree *map = &btrfs_inode->extent_tree;

	if (gfpflags_allow_blocking(mask) &&
	    page->mapping->host->i_size > SZ_16M) {
		u64 len;
		while (start <= end) {
			len = end - start + 1;
			write_lock(&map->lock);
			em = lookup_extent_mapping(map, start, len);
			if (!em) {
				write_unlock(&map->lock);
				break;
			}
			if (test_bit(EXTENT_FLAG_PINNED, &em->flags) ||
			    em->start != start) {
				write_unlock(&map->lock);
				free_extent_map(em);
				break;
			}
			if (!test_range_bit(tree, em->start,
					    extent_map_end(em) - 1,
					    EXTENT_LOCKED, 0, NULL)) {
				set_bit(BTRFS_INODE_NEEDS_FULL_SYNC,
					&btrfs_inode->runtime_flags);
				remove_extent_mapping(map, em);
				/* once for the rb tree */
				free_extent_map(em);
			}
			start = extent_map_end(em);
			write_unlock(&map->lock);

			/* once for us */
			free_extent_map(em);
		}
	}
	return try_release_extent_state(tree, page, mask);
}

/*
 * helper function for fiemap, which doesn't want to see any holes.
 * This maps until we find something past 'last'
 */
static struct extent_map *get_extent_skip_holes(struct inode *inode,
						u64 offset, u64 last)
{
	u64 sectorsize = btrfs_inode_sectorsize(inode);
	struct extent_map *em;
	u64 len;

	if (offset >= last)
		return NULL;

	while (1) {
		len = last - offset;
		if (len == 0)
			break;
		len = ALIGN(len, sectorsize);
		em = btrfs_get_extent_fiemap(BTRFS_I(inode), offset, len);
		if (IS_ERR_OR_NULL(em))
			return em;

		/* if this isn't a hole return it */
		if (em->block_start != EXTENT_MAP_HOLE)
			return em;

		/* this is a hole, advance to the next extent */
		offset = extent_map_end(em);
		free_extent_map(em);
		if (offset >= last)
			break;
	}
	return NULL;
}

/*
 * To cache previous fiemap extent
 *
 * Will be used for merging fiemap extent
 */
struct fiemap_cache {
	u64 offset;
	u64 phys;
	u64 len;
	u32 flags;
	bool cached;
};

/*
 * Helper to submit fiemap extent.
 *
 * Will try to merge current fiemap extent specified by @offset, @phys,
 * @len and @flags with cached one.
 * And only when we fails to merge, cached one will be submitted as
 * fiemap extent.
 *
 * Return value is the same as fiemap_fill_next_extent().
 */
static int emit_fiemap_extent(struct fiemap_extent_info *fieinfo,
				struct fiemap_cache *cache,
				u64 offset, u64 phys, u64 len, u32 flags)
{
	int ret = 0;

	if (!cache->cached)
		goto assign;

	/*
	 * Sanity check, extent_fiemap() should have ensured that new
	 * fiemap extent won't overlap with cached one.
	 * Not recoverable.
	 *
	 * NOTE: Physical address can overlap, due to compression
	 */
	if (cache->offset + cache->len > offset) {
		WARN_ON(1);
		return -EINVAL;
	}

	/*
	 * Only merges fiemap extents if
	 * 1) Their logical addresses are continuous
	 *
	 * 2) Their physical addresses are continuous
	 *    So truly compressed (physical size smaller than logical size)
	 *    extents won't get merged with each other
	 *
	 * 3) Share same flags except FIEMAP_EXTENT_LAST
	 *    So regular extent won't get merged with prealloc extent
	 */
	if (cache->offset + cache->len  == offset &&
	    cache->phys + cache->len == phys  &&
	    (cache->flags & ~FIEMAP_EXTENT_LAST) ==
			(flags & ~FIEMAP_EXTENT_LAST)) {
		cache->len += len;
		cache->flags |= flags;
		goto try_submit_last;
	}

	/* Not mergeable, need to submit cached one */
	ret = fiemap_fill_next_extent(fieinfo, cache->offset, cache->phys,
				      cache->len, cache->flags);
	cache->cached = false;
	if (ret)
		return ret;
assign:
	cache->cached = true;
	cache->offset = offset;
	cache->phys = phys;
	cache->len = len;
	cache->flags = flags;
try_submit_last:
	if (cache->flags & FIEMAP_EXTENT_LAST) {
		ret = fiemap_fill_next_extent(fieinfo, cache->offset,
				cache->phys, cache->len, cache->flags);
		cache->cached = false;
	}
	return ret;
}

/*
 * Emit last fiemap cache
 *
 * The last fiemap cache may still be cached in the following case:
 * 0		      4k		    8k
 * |<- Fiemap range ->|
 * |<------------  First extent ----------->|
 *
 * In this case, the first extent range will be cached but not emitted.
 * So we must emit it before ending extent_fiemap().
 */
static int emit_last_fiemap_cache(struct fiemap_extent_info *fieinfo,
				  struct fiemap_cache *cache)
{
	int ret;

	if (!cache->cached)
		return 0;

	ret = fiemap_fill_next_extent(fieinfo, cache->offset, cache->phys,
				      cache->len, cache->flags);
	cache->cached = false;
	if (ret > 0)
		ret = 0;
	return ret;
}

int extent_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		__u64 start, __u64 len)
{
	int ret = 0;
	u64 off = start;
	u64 max = start + len;
	u32 flags = 0;
	u32 found_type;
	u64 last;
	u64 last_for_get_extent = 0;
	u64 disko = 0;
	u64 isize = i_size_read(inode);
	struct btrfs_key found_key;
	struct extent_map *em = NULL;
	struct extent_state *cached_state = NULL;
	struct btrfs_path *path;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct fiemap_cache cache = { 0 };
	struct ulist *roots;
	struct ulist *tmp_ulist;
	int end = 0;
	u64 em_start = 0;
	u64 em_len = 0;
	u64 em_end = 0;

	if (len == 0)
		return -EINVAL;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->leave_spinning = 1;

	roots = ulist_alloc(GFP_KERNEL);
	tmp_ulist = ulist_alloc(GFP_KERNEL);
	if (!roots || !tmp_ulist) {
		ret = -ENOMEM;
		goto out_free_ulist;
	}

	start = round_down(start, btrfs_inode_sectorsize(inode));
	len = round_up(max, btrfs_inode_sectorsize(inode)) - start;

	/*
	 * lookup the last file extent.  We're not using i_size here
	 * because there might be preallocation past i_size
	 */
	ret = btrfs_lookup_file_extent(NULL, root, path,
			btrfs_ino(BTRFS_I(inode)), -1, 0);
	if (ret < 0) {
		goto out_free_ulist;
	} else {
		WARN_ON(!ret);
		if (ret == 1)
			ret = 0;
	}

	path->slots[0]--;
	btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
	found_type = found_key.type;

	/* No extents, but there might be delalloc bits */
	if (found_key.objectid != btrfs_ino(BTRFS_I(inode)) ||
	    found_type != BTRFS_EXTENT_DATA_KEY) {
		/* have to trust i_size as the end */
		last = (u64)-1;
		last_for_get_extent = isize;
	} else {
		/*
		 * remember the start of the last extent.  There are a
		 * bunch of different factors that go into the length of the
		 * extent, so its much less complex to remember where it started
		 */
		last = found_key.offset;
		last_for_get_extent = last + 1;
	}
	btrfs_release_path(path);

	/*
	 * we might have some extents allocated but more delalloc past those
	 * extents.  so, we trust isize unless the start of the last extent is
	 * beyond isize
	 */
	if (last < isize) {
		last = (u64)-1;
		last_for_get_extent = isize;
	}

	lock_extent_bits(&BTRFS_I(inode)->io_tree, start, start + len - 1,
			 &cached_state);

	em = get_extent_skip_holes(inode, start, last_for_get_extent);
	if (!em)
		goto out;
	if (IS_ERR(em)) {
		ret = PTR_ERR(em);
		goto out;
	}

	while (!end) {
		u64 offset_in_extent = 0;

		/* break if the extent we found is outside the range */
		if (em->start >= max || extent_map_end(em) < off)
			break;

		/*
		 * get_extent may return an extent that starts before our
		 * requested range.  We have to make sure the ranges
		 * we return to fiemap always move forward and don't
		 * overlap, so adjust the offsets here
		 */
		em_start = max(em->start, off);

		/*
		 * record the offset from the start of the extent
		 * for adjusting the disk offset below.  Only do this if the
		 * extent isn't compressed since our in ram offset may be past
		 * what we have actually allocated on disk.
		 */
		if (!test_bit(EXTENT_FLAG_COMPRESSED, &em->flags))
			offset_in_extent = em_start - em->start;
		em_end = extent_map_end(em);
		em_len = em_end - em_start;
		flags = 0;
		if (em->block_start < EXTENT_MAP_LAST_BYTE)
			disko = em->block_start + offset_in_extent;
		else
			disko = 0;

		/*
		 * bump off for our next call to get_extent
		 */
		off = extent_map_end(em);
		if (off >= max)
			end = 1;

		if (em->block_start == EXTENT_MAP_LAST_BYTE) {
			end = 1;
			flags |= FIEMAP_EXTENT_LAST;
		} else if (em->block_start == EXTENT_MAP_INLINE) {
			flags |= (FIEMAP_EXTENT_DATA_INLINE |
				  FIEMAP_EXTENT_NOT_ALIGNED);
		} else if (em->block_start == EXTENT_MAP_DELALLOC) {
			flags |= (FIEMAP_EXTENT_DELALLOC |
				  FIEMAP_EXTENT_UNKNOWN);
		} else if (fieinfo->fi_extents_max) {
			u64 bytenr = em->block_start -
				(em->start - em->orig_start);

			/*
			 * As btrfs supports shared space, this information
			 * can be exported to userspace tools via
			 * flag FIEMAP_EXTENT_SHARED.  If fi_extents_max == 0
			 * then we're just getting a count and we can skip the
			 * lookup stuff.
			 */
			ret = btrfs_check_shared(root,
						 btrfs_ino(BTRFS_I(inode)),
						 bytenr, roots, tmp_ulist);
			if (ret < 0)
				goto out_free;
			if (ret)
				flags |= FIEMAP_EXTENT_SHARED;
			ret = 0;
		}
		if (test_bit(EXTENT_FLAG_COMPRESSED, &em->flags))
			flags |= FIEMAP_EXTENT_ENCODED;
		if (test_bit(EXTENT_FLAG_PREALLOC, &em->flags))
			flags |= FIEMAP_EXTENT_UNWRITTEN;

		free_extent_map(em);
		em = NULL;
		if ((em_start >= last) || em_len == (u64)-1 ||
		   (last == (u64)-1 && isize <= em_end)) {
			flags |= FIEMAP_EXTENT_LAST;
			end = 1;
		}

		/* now scan forward to see if this is really the last extent. */
		em = get_extent_skip_holes(inode, off, last_for_get_extent);
		if (IS_ERR(em)) {
			ret = PTR_ERR(em);
			goto out;
		}
		if (!em) {
			flags |= FIEMAP_EXTENT_LAST;
			end = 1;
		}
		ret = emit_fiemap_extent(fieinfo, &cache, em_start, disko,
					   em_len, flags);
		if (ret) {
			if (ret == 1)
				ret = 0;
			goto out_free;
		}
	}
out_free:
	if (!ret)
		ret = emit_last_fiemap_cache(fieinfo, &cache);
	free_extent_map(em);
out:
	unlock_extent_cached(&BTRFS_I(inode)->io_tree, start, start + len - 1,
			     &cached_state);

out_free_ulist:
	btrfs_free_path(path);
	ulist_free(roots);
	ulist_free(tmp_ulist);
	return ret;
}

static void __free_extent_buffer(struct extent_buffer *eb)
{
	btrfs_leak_debug_del(&eb->leak_list);
	kmem_cache_free(extent_buffer_cache, eb);
}

int extent_buffer_under_io(struct extent_buffer *eb)
{
	return (atomic_read(&eb->io_pages) ||
		test_bit(EXTENT_BUFFER_WRITEBACK, &eb->bflags) ||
		test_bit(EXTENT_BUFFER_DIRTY, &eb->bflags));
}

/*
 * Release all pages attached to the extent buffer.
 */
static void btrfs_release_extent_buffer_pages(struct extent_buffer *eb)
{
	int i;
	int num_pages;
	int mapped = !test_bit(EXTENT_BUFFER_UNMAPPED, &eb->bflags);

	BUG_ON(extent_buffer_under_io(eb));

	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		struct page *page = eb->pages[i];

		if (!page)
			continue;
		if (mapped)
			spin_lock(&page->mapping->private_lock);
		/*
		 * We do this since we'll remove the pages after we've
		 * removed the eb from the radix tree, so we could race
		 * and have this page now attached to the new eb.  So
		 * only clear page_private if it's still connected to
		 * this eb.
		 */
		if (PagePrivate(page) &&
		    page->private == (unsigned long)eb) {
			BUG_ON(test_bit(EXTENT_BUFFER_DIRTY, &eb->bflags));
			BUG_ON(PageDirty(page));
			BUG_ON(PageWriteback(page));
			/*
			 * We need to make sure we haven't be attached
			 * to a new eb.
			 */
			ClearPagePrivate(page);
			set_page_private(page, 0);
			/* One for the page private */
			put_page(page);
		}

		if (mapped)
			spin_unlock(&page->mapping->private_lock);

		/* One for when we allocated the page */
		put_page(page);
	}
}

/*
 * Helper for releasing the extent buffer.
 */
static inline void btrfs_release_extent_buffer(struct extent_buffer *eb)
{
	btrfs_release_extent_buffer_pages(eb);
	__free_extent_buffer(eb);
}

static struct extent_buffer *
__alloc_extent_buffer(struct btrfs_fs_info *fs_info, u64 start,
		      unsigned long len)
{
	struct extent_buffer *eb = NULL;

	eb = kmem_cache_zalloc(extent_buffer_cache, GFP_NOFS|__GFP_NOFAIL);
	eb->start = start;
	eb->len = len;
	eb->fs_info = fs_info;
	eb->bflags = 0;
	rwlock_init(&eb->lock);
	atomic_set(&eb->blocking_readers, 0);
	eb->blocking_writers = 0;
	eb->lock_nested = false;
	init_waitqueue_head(&eb->write_lock_wq);
	init_waitqueue_head(&eb->read_lock_wq);

	btrfs_leak_debug_add(&eb->leak_list, &buffers);

	spin_lock_init(&eb->refs_lock);
	atomic_set(&eb->refs, 1);
	atomic_set(&eb->io_pages, 0);

	/*
	 * Sanity checks, currently the maximum is 64k covered by 16x 4k pages
	 */
	BUILD_BUG_ON(BTRFS_MAX_METADATA_BLOCKSIZE
		> MAX_INLINE_EXTENT_BUFFER_SIZE);
	BUG_ON(len > MAX_INLINE_EXTENT_BUFFER_SIZE);

#ifdef CONFIG_BTRFS_DEBUG
	eb->spinning_writers = 0;
	atomic_set(&eb->spinning_readers, 0);
	atomic_set(&eb->read_locks, 0);
	eb->write_locks = 0;
#endif

	return eb;
}

struct extent_buffer *btrfs_clone_extent_buffer(struct extent_buffer *src)
{
	int i;
	struct page *p;
	struct extent_buffer *new;
	int num_pages = num_extent_pages(src);

	new = __alloc_extent_buffer(src->fs_info, src->start, src->len);
	if (new == NULL)
		return NULL;

	for (i = 0; i < num_pages; i++) {
		p = alloc_page(GFP_NOFS);
		if (!p) {
			btrfs_release_extent_buffer(new);
			return NULL;
		}
		attach_extent_buffer_page(new, p);
		WARN_ON(PageDirty(p));
		SetPageUptodate(p);
		new->pages[i] = p;
		copy_page(page_address(p), page_address(src->pages[i]));
	}

	set_bit(EXTENT_BUFFER_UPTODATE, &new->bflags);
	set_bit(EXTENT_BUFFER_UNMAPPED, &new->bflags);

	return new;
}

struct extent_buffer *__alloc_dummy_extent_buffer(struct btrfs_fs_info *fs_info,
						  u64 start, unsigned long len)
{
	struct extent_buffer *eb;
	int num_pages;
	int i;

	eb = __alloc_extent_buffer(fs_info, start, len);
	if (!eb)
		return NULL;

	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		eb->pages[i] = alloc_page(GFP_NOFS);
		if (!eb->pages[i])
			goto err;
	}
	set_extent_buffer_uptodate(eb);
	btrfs_set_header_nritems(eb, 0);
	set_bit(EXTENT_BUFFER_UNMAPPED, &eb->bflags);

	return eb;
err:
	for (; i > 0; i--)
		__free_page(eb->pages[i - 1]);
	__free_extent_buffer(eb);
	return NULL;
}

struct extent_buffer *alloc_dummy_extent_buffer(struct btrfs_fs_info *fs_info,
						u64 start)
{
	return __alloc_dummy_extent_buffer(fs_info, start, fs_info->nodesize);
}

static void check_buffer_tree_ref(struct extent_buffer *eb)
{
	int refs;
	/* the ref bit is tricky.  We have to make sure it is set
	 * if we have the buffer dirty.   Otherwise the
	 * code to free a buffer can end up dropping a dirty
	 * page
	 *
	 * Once the ref bit is set, it won't go away while the
	 * buffer is dirty or in writeback, and it also won't
	 * go away while we have the reference count on the
	 * eb bumped.
	 *
	 * We can't just set the ref bit without bumping the
	 * ref on the eb because free_extent_buffer might
	 * see the ref bit and try to clear it.  If this happens
	 * free_extent_buffer might end up dropping our original
	 * ref by mistake and freeing the page before we are able
	 * to add one more ref.
	 *
	 * So bump the ref count first, then set the bit.  If someone
	 * beat us to it, drop the ref we added.
	 */
	refs = atomic_read(&eb->refs);
	if (refs >= 2 && test_bit(EXTENT_BUFFER_TREE_REF, &eb->bflags))
		return;

	spin_lock(&eb->refs_lock);
	if (!test_and_set_bit(EXTENT_BUFFER_TREE_REF, &eb->bflags))
		atomic_inc(&eb->refs);
	spin_unlock(&eb->refs_lock);
}

static void mark_extent_buffer_accessed(struct extent_buffer *eb,
		struct page *accessed)
{
	int num_pages, i;

	check_buffer_tree_ref(eb);

	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		struct page *p = eb->pages[i];

		if (p != accessed)
			mark_page_accessed(p);
	}
}

struct extent_buffer *find_extent_buffer(struct btrfs_fs_info *fs_info,
					 u64 start)
{
	struct extent_buffer *eb;

	rcu_read_lock();
	eb = radix_tree_lookup(&fs_info->buffer_radix,
			       start >> PAGE_SHIFT);
	if (eb && atomic_inc_not_zero(&eb->refs)) {
		rcu_read_unlock();
		/*
		 * Lock our eb's refs_lock to avoid races with
		 * free_extent_buffer. When we get our eb it might be flagged
		 * with EXTENT_BUFFER_STALE and another task running
		 * free_extent_buffer might have seen that flag set,
		 * eb->refs == 2, that the buffer isn't under IO (dirty and
		 * writeback flags not set) and it's still in the tree (flag
		 * EXTENT_BUFFER_TREE_REF set), therefore being in the process
		 * of decrementing the extent buffer's reference count twice.
		 * So here we could race and increment the eb's reference count,
		 * clear its stale flag, mark it as dirty and drop our reference
		 * before the other task finishes executing free_extent_buffer,
		 * which would later result in an attempt to free an extent
		 * buffer that is dirty.
		 */
		if (test_bit(EXTENT_BUFFER_STALE, &eb->bflags)) {
			spin_lock(&eb->refs_lock);
			spin_unlock(&eb->refs_lock);
		}
		mark_extent_buffer_accessed(eb, NULL);
		return eb;
	}
	rcu_read_unlock();

	return NULL;
}

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
struct extent_buffer *alloc_test_extent_buffer(struct btrfs_fs_info *fs_info,
					u64 start)
{
	struct extent_buffer *eb, *exists = NULL;
	int ret;

	eb = find_extent_buffer(fs_info, start);
	if (eb)
		return eb;
	eb = alloc_dummy_extent_buffer(fs_info, start);
	if (!eb)
		return NULL;
	eb->fs_info = fs_info;
again:
	ret = radix_tree_preload(GFP_NOFS);
	if (ret)
		goto free_eb;
	spin_lock(&fs_info->buffer_lock);
	ret = radix_tree_insert(&fs_info->buffer_radix,
				start >> PAGE_SHIFT, eb);
	spin_unlock(&fs_info->buffer_lock);
	radix_tree_preload_end();
	if (ret == -EEXIST) {
		exists = find_extent_buffer(fs_info, start);
		if (exists)
			goto free_eb;
		else
			goto again;
	}
	check_buffer_tree_ref(eb);
	set_bit(EXTENT_BUFFER_IN_TREE, &eb->bflags);

	return eb;
free_eb:
	btrfs_release_extent_buffer(eb);
	return exists;
}
#endif

struct extent_buffer *alloc_extent_buffer(struct btrfs_fs_info *fs_info,
					  u64 start)
{
	unsigned long len = fs_info->nodesize;
	int num_pages;
	int i;
	unsigned long index = start >> PAGE_SHIFT;
	struct extent_buffer *eb;
	struct extent_buffer *exists = NULL;
	struct page *p;
	struct address_space *mapping = fs_info->btree_inode->i_mapping;
	int uptodate = 1;
	int ret;

	if (!IS_ALIGNED(start, fs_info->sectorsize)) {
		btrfs_err(fs_info, "bad tree block start %llu", start);
		return ERR_PTR(-EINVAL);
	}

	eb = find_extent_buffer(fs_info, start);
	if (eb)
		return eb;

	eb = __alloc_extent_buffer(fs_info, start, len);
	if (!eb)
		return ERR_PTR(-ENOMEM);

	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++, index++) {
		p = find_or_create_page(mapping, index, GFP_NOFS|__GFP_NOFAIL);
		if (!p) {
			exists = ERR_PTR(-ENOMEM);
			goto free_eb;
		}

		spin_lock(&mapping->private_lock);
		if (PagePrivate(p)) {
			/*
			 * We could have already allocated an eb for this page
			 * and attached one so lets see if we can get a ref on
			 * the existing eb, and if we can we know it's good and
			 * we can just return that one, else we know we can just
			 * overwrite page->private.
			 */
			exists = (struct extent_buffer *)p->private;
			if (atomic_inc_not_zero(&exists->refs)) {
				spin_unlock(&mapping->private_lock);
				unlock_page(p);
				put_page(p);
				mark_extent_buffer_accessed(exists, p);
				goto free_eb;
			}
			exists = NULL;

			/*
			 * Do this so attach doesn't complain and we need to
			 * drop the ref the old guy had.
			 */
			ClearPagePrivate(p);
			WARN_ON(PageDirty(p));
			put_page(p);
		}
		attach_extent_buffer_page(eb, p);
		spin_unlock(&mapping->private_lock);
		WARN_ON(PageDirty(p));
		eb->pages[i] = p;
		if (!PageUptodate(p))
			uptodate = 0;

		/*
		 * We can't unlock the pages just yet since the extent buffer
		 * hasn't been properly inserted in the radix tree, this
		 * opens a race with btree_releasepage which can free a page
		 * while we are still filling in all pages for the buffer and
		 * we could crash.
		 */
	}
	if (uptodate)
		set_bit(EXTENT_BUFFER_UPTODATE, &eb->bflags);
again:
	ret = radix_tree_preload(GFP_NOFS);
	if (ret) {
		exists = ERR_PTR(ret);
		goto free_eb;
	}

	spin_lock(&fs_info->buffer_lock);
	ret = radix_tree_insert(&fs_info->buffer_radix,
				start >> PAGE_SHIFT, eb);
	spin_unlock(&fs_info->buffer_lock);
	radix_tree_preload_end();
	if (ret == -EEXIST) {
		exists = find_extent_buffer(fs_info, start);
		if (exists)
			goto free_eb;
		else
			goto again;
	}
	/* add one reference for the tree */
	check_buffer_tree_ref(eb);
	set_bit(EXTENT_BUFFER_IN_TREE, &eb->bflags);

	/*
	 * Now it's safe to unlock the pages because any calls to
	 * btree_releasepage will correctly detect that a page belongs to a
	 * live buffer and won't free them prematurely.
	 */
	for (i = 0; i < num_pages; i++)
		unlock_page(eb->pages[i]);
	return eb;

free_eb:
	WARN_ON(!atomic_dec_and_test(&eb->refs));
	for (i = 0; i < num_pages; i++) {
		if (eb->pages[i])
			unlock_page(eb->pages[i]);
	}

	btrfs_release_extent_buffer(eb);
	return exists;
}

static inline void btrfs_release_extent_buffer_rcu(struct rcu_head *head)
{
	struct extent_buffer *eb =
			container_of(head, struct extent_buffer, rcu_head);

	__free_extent_buffer(eb);
}

static int release_extent_buffer(struct extent_buffer *eb)
{
	lockdep_assert_held(&eb->refs_lock);

	WARN_ON(atomic_read(&eb->refs) == 0);
	if (atomic_dec_and_test(&eb->refs)) {
		if (test_and_clear_bit(EXTENT_BUFFER_IN_TREE, &eb->bflags)) {
			struct btrfs_fs_info *fs_info = eb->fs_info;

			spin_unlock(&eb->refs_lock);

			spin_lock(&fs_info->buffer_lock);
			radix_tree_delete(&fs_info->buffer_radix,
					  eb->start >> PAGE_SHIFT);
			spin_unlock(&fs_info->buffer_lock);
		} else {
			spin_unlock(&eb->refs_lock);
		}

		/* Should be safe to release our pages at this point */
		btrfs_release_extent_buffer_pages(eb);
#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
		if (unlikely(test_bit(EXTENT_BUFFER_UNMAPPED, &eb->bflags))) {
			__free_extent_buffer(eb);
			return 1;
		}
#endif
		call_rcu(&eb->rcu_head, btrfs_release_extent_buffer_rcu);
		return 1;
	}
	spin_unlock(&eb->refs_lock);

	return 0;
}

void free_extent_buffer(struct extent_buffer *eb)
{
	int refs;
	int old;
	if (!eb)
		return;

	while (1) {
		refs = atomic_read(&eb->refs);
		if ((!test_bit(EXTENT_BUFFER_UNMAPPED, &eb->bflags) && refs <= 3)
		    || (test_bit(EXTENT_BUFFER_UNMAPPED, &eb->bflags) &&
			refs == 1))
			break;
		old = atomic_cmpxchg(&eb->refs, refs, refs - 1);
		if (old == refs)
			return;
	}

	spin_lock(&eb->refs_lock);
	if (atomic_read(&eb->refs) == 2 &&
	    test_bit(EXTENT_BUFFER_STALE, &eb->bflags) &&
	    !extent_buffer_under_io(eb) &&
	    test_and_clear_bit(EXTENT_BUFFER_TREE_REF, &eb->bflags))
		atomic_dec(&eb->refs);

	/*
	 * I know this is terrible, but it's temporary until we stop tracking
	 * the uptodate bits and such for the extent buffers.
	 */
	release_extent_buffer(eb);
}

void free_extent_buffer_stale(struct extent_buffer *eb)
{
	if (!eb)
		return;

	spin_lock(&eb->refs_lock);
	set_bit(EXTENT_BUFFER_STALE, &eb->bflags);

	if (atomic_read(&eb->refs) == 2 && !extent_buffer_under_io(eb) &&
	    test_and_clear_bit(EXTENT_BUFFER_TREE_REF, &eb->bflags))
		atomic_dec(&eb->refs);
	release_extent_buffer(eb);
}

void clear_extent_buffer_dirty(struct extent_buffer *eb)
{
	int i;
	int num_pages;
	struct page *page;

	num_pages = num_extent_pages(eb);

	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];
		if (!PageDirty(page))
			continue;

		lock_page(page);
		WARN_ON(!PagePrivate(page));

		clear_page_dirty_for_io(page);
		xa_lock_irq(&page->mapping->i_pages);
		if (!PageDirty(page))
			__xa_clear_mark(&page->mapping->i_pages,
					page_index(page), PAGECACHE_TAG_DIRTY);
		xa_unlock_irq(&page->mapping->i_pages);
		ClearPageError(page);
		unlock_page(page);
	}
	WARN_ON(atomic_read(&eb->refs) == 0);
}

bool set_extent_buffer_dirty(struct extent_buffer *eb)
{
	int i;
	int num_pages;
	bool was_dirty;

	check_buffer_tree_ref(eb);

	was_dirty = test_and_set_bit(EXTENT_BUFFER_DIRTY, &eb->bflags);

	num_pages = num_extent_pages(eb);
	WARN_ON(atomic_read(&eb->refs) == 0);
	WARN_ON(!test_bit(EXTENT_BUFFER_TREE_REF, &eb->bflags));

	if (!was_dirty)
		for (i = 0; i < num_pages; i++)
			set_page_dirty(eb->pages[i]);

#ifdef CONFIG_BTRFS_DEBUG
	for (i = 0; i < num_pages; i++)
		ASSERT(PageDirty(eb->pages[i]));
#endif

	return was_dirty;
}

void clear_extent_buffer_uptodate(struct extent_buffer *eb)
{
	int i;
	struct page *page;
	int num_pages;

	clear_bit(EXTENT_BUFFER_UPTODATE, &eb->bflags);
	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];
		if (page)
			ClearPageUptodate(page);
	}
}

void set_extent_buffer_uptodate(struct extent_buffer *eb)
{
	int i;
	struct page *page;
	int num_pages;

	set_bit(EXTENT_BUFFER_UPTODATE, &eb->bflags);
	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];
		SetPageUptodate(page);
	}
}

int read_extent_buffer_pages(struct extent_buffer *eb, int wait, int mirror_num)
{
	int i;
	struct page *page;
	int err;
	int ret = 0;
	int locked_pages = 0;
	int all_uptodate = 1;
	int num_pages;
	unsigned long num_reads = 0;
	struct bio *bio = NULL;
	unsigned long bio_flags = 0;
	struct extent_io_tree *tree = &BTRFS_I(eb->fs_info->btree_inode)->io_tree;

	if (test_bit(EXTENT_BUFFER_UPTODATE, &eb->bflags))
		return 0;

	num_pages = num_extent_pages(eb);
	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];
		if (wait == WAIT_NONE) {
			if (!trylock_page(page))
				goto unlock_exit;
		} else {
			lock_page(page);
		}
		locked_pages++;
	}
	/*
	 * We need to firstly lock all pages to make sure that
	 * the uptodate bit of our pages won't be affected by
	 * clear_extent_buffer_uptodate().
	 */
	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];
		if (!PageUptodate(page)) {
			num_reads++;
			all_uptodate = 0;
		}
	}

	if (all_uptodate) {
		set_bit(EXTENT_BUFFER_UPTODATE, &eb->bflags);
		goto unlock_exit;
	}

	clear_bit(EXTENT_BUFFER_READ_ERR, &eb->bflags);
	eb->read_mirror = 0;
	atomic_set(&eb->io_pages, num_reads);
	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];

		if (!PageUptodate(page)) {
			if (ret) {
				atomic_dec(&eb->io_pages);
				unlock_page(page);
				continue;
			}

			ClearPageError(page);
			err = __extent_read_full_page(tree, page,
						      btree_get_extent, &bio,
						      mirror_num, &bio_flags,
						      REQ_META);
			if (err) {
				ret = err;
				/*
				 * We use &bio in above __extent_read_full_page,
				 * so we ensure that if it returns error, the
				 * current page fails to add itself to bio and
				 * it's been unlocked.
				 *
				 * We must dec io_pages by ourselves.
				 */
				atomic_dec(&eb->io_pages);
			}
		} else {
			unlock_page(page);
		}
	}

	if (bio) {
		err = submit_one_bio(bio, mirror_num, bio_flags);
		if (err)
			return err;
	}

	if (ret || wait != WAIT_COMPLETE)
		return ret;

	for (i = 0; i < num_pages; i++) {
		page = eb->pages[i];
		wait_on_page_locked(page);
		if (!PageUptodate(page))
			ret = -EIO;
	}

	return ret;

unlock_exit:
	while (locked_pages > 0) {
		locked_pages--;
		page = eb->pages[locked_pages];
		unlock_page(page);
	}
	return ret;
}

void read_extent_buffer(const struct extent_buffer *eb, void *dstv,
			unsigned long start, unsigned long len)
{
	size_t cur;
	size_t offset;
	struct page *page;
	char *kaddr;
	char *dst = (char *)dstv;
	size_t start_offset = offset_in_page(eb->start);
	unsigned long i = (start_offset + start) >> PAGE_SHIFT;

	if (start + len > eb->len) {
		WARN(1, KERN_ERR "btrfs bad mapping eb start %llu len %lu, wanted %lu %lu\n",
		     eb->start, eb->len, start, len);
		memset(dst, 0, len);
		return;
	}

	offset = offset_in_page(start_offset + start);

	while (len > 0) {
		page = eb->pages[i];

		cur = min(len, (PAGE_SIZE - offset));
		kaddr = page_address(page);
		memcpy(dst, kaddr + offset, cur);

		dst += cur;
		len -= cur;
		offset = 0;
		i++;
	}
}

int read_extent_buffer_to_user(const struct extent_buffer *eb,
			       void __user *dstv,
			       unsigned long start, unsigned long len)
{
	size_t cur;
	size_t offset;
	struct page *page;
	char *kaddr;
	char __user *dst = (char __user *)dstv;
	size_t start_offset = offset_in_page(eb->start);
	unsigned long i = (start_offset + start) >> PAGE_SHIFT;
	int ret = 0;

	WARN_ON(start > eb->len);
	WARN_ON(start + len > eb->start + eb->len);

	offset = offset_in_page(start_offset + start);

	while (len > 0) {
		page = eb->pages[i];

		cur = min(len, (PAGE_SIZE - offset));
		kaddr = page_address(page);
		if (copy_to_user(dst, kaddr + offset, cur)) {
			ret = -EFAULT;
			break;
		}

		dst += cur;
		len -= cur;
		offset = 0;
		i++;
	}

	return ret;
}

/*
 * return 0 if the item is found within a page.
 * return 1 if the item spans two pages.
 * return -EINVAL otherwise.
 */
int map_private_extent_buffer(const struct extent_buffer *eb,
			      unsigned long start, unsigned long min_len,
			      char **map, unsigned long *map_start,
			      unsigned long *map_len)
{
	size_t offset;
	char *kaddr;
	struct page *p;
	size_t start_offset = offset_in_page(eb->start);
	unsigned long i = (start_offset + start) >> PAGE_SHIFT;
	unsigned long end_i = (start_offset + start + min_len - 1) >>
		PAGE_SHIFT;

	if (start + min_len > eb->len) {
		WARN(1, KERN_ERR "btrfs bad mapping eb start %llu len %lu, wanted %lu %lu\n",
		       eb->start, eb->len, start, min_len);
		return -EINVAL;
	}

	if (i != end_i)
		return 1;

	if (i == 0) {
		offset = start_offset;
		*map_start = 0;
	} else {
		offset = 0;
		*map_start = ((u64)i << PAGE_SHIFT) - start_offset;
	}

	p = eb->pages[i];
	kaddr = page_address(p);
	*map = kaddr + offset;
	*map_len = PAGE_SIZE - offset;
	return 0;
}

int memcmp_extent_buffer(const struct extent_buffer *eb, const void *ptrv,
			 unsigned long start, unsigned long len)
{
	size_t cur;
	size_t offset;
	struct page *page;
	char *kaddr;
	char *ptr = (char *)ptrv;
	size_t start_offset = offset_in_page(eb->start);
	unsigned long i = (start_offset + start) >> PAGE_SHIFT;
	int ret = 0;

	WARN_ON(start > eb->len);
	WARN_ON(start + len > eb->start + eb->len);

	offset = offset_in_page(start_offset + start);

	while (len > 0) {
		page = eb->pages[i];

		cur = min(len, (PAGE_SIZE - offset));

		kaddr = page_address(page);
		ret = memcmp(ptr, kaddr + offset, cur);
		if (ret)
			break;

		ptr += cur;
		len -= cur;
		offset = 0;
		i++;
	}
	return ret;
}

void write_extent_buffer_chunk_tree_uuid(struct extent_buffer *eb,
		const void *srcv)
{
	char *kaddr;

	WARN_ON(!PageUptodate(eb->pages[0]));
	kaddr = page_address(eb->pages[0]);
	memcpy(kaddr + offsetof(struct btrfs_header, chunk_tree_uuid), srcv,
			BTRFS_FSID_SIZE);
}

void write_extent_buffer_fsid(struct extent_buffer *eb, const void *srcv)
{
	char *kaddr;

	WARN_ON(!PageUptodate(eb->pages[0]));
	kaddr = page_address(eb->pages[0]);
	memcpy(kaddr + offsetof(struct btrfs_header, fsid), srcv,
			BTRFS_FSID_SIZE);
}

void write_extent_buffer(struct extent_buffer *eb, const void *srcv,
			 unsigned long start, unsigned long len)
{
	size_t cur;
	size_t offset;
	struct page *page;
	char *kaddr;
	char *src = (char *)srcv;
	size_t start_offset = offset_in_page(eb->start);
	unsigned long i = (start_offset + start) >> PAGE_SHIFT;

	WARN_ON(start > eb->len);
	WARN_ON(start + len > eb->start + eb->len);

	offset = offset_in_page(start_offset + start);

	while (len > 0) {
		page = eb->pages[i];
		WARN_ON(!PageUptodate(page));

		cur = min(len, PAGE_SIZE - offset);
		kaddr = page_address(page);
		memcpy(kaddr + offset, src, cur);

		src += cur;
		len -= cur;
		offset = 0;
		i++;
	}
}

void memzero_extent_buffer(struct extent_buffer *eb, unsigned long start,
		unsigned long len)
{
	size_t cur;
	size_t offset;
	struct page *page;
	char *kaddr;
	size_t start_offset = offset_in_page(eb->start);
	unsigned long i = (start_offset + start) >> PAGE_SHIFT;

	WARN_ON(start > eb->len);
	WARN_ON(start + len > eb->start + eb->len);

	offset = offset_in_page(start_offset + start);

	while (len > 0) {
		page = eb->pages[i];
		WARN_ON(!PageUptodate(page));

		cur = min(len, PAGE_SIZE - offset);
		kaddr = page_address(page);
		memset(kaddr + offset, 0, cur);

		len -= cur;
		offset = 0;
		i++;
	}
}

void copy_extent_buffer_full(struct extent_buffer *dst,
			     struct extent_buffer *src)
{
	int i;
	int num_pages;

	ASSERT(dst->len == src->len);

	num_pages = num_extent_pages(dst);
	for (i = 0; i < num_pages; i++)
		copy_page(page_address(dst->pages[i]),
				page_address(src->pages[i]));
}

void copy_extent_buffer(struct extent_buffer *dst, struct extent_buffer *src,
			unsigned long dst_offset, unsigned long src_offset,
			unsigned long len)
{
	u64 dst_len = dst->len;
	size_t cur;
	size_t offset;
	struct page *page;
	char *kaddr;
	size_t start_offset = offset_in_page(dst->start);
	unsigned long i = (start_offset + dst_offset) >> PAGE_SHIFT;

	WARN_ON(src->len != dst_len);

	offset = offset_in_page(start_offset + dst_offset);

	while (len > 0) {
		page = dst->pages[i];
		WARN_ON(!PageUptodate(page));

		cur = min(len, (unsigned long)(PAGE_SIZE - offset));

		kaddr = page_address(page);
		read_extent_buffer(src, kaddr + offset, src_offset, cur);

		src_offset += cur;
		len -= cur;
		offset = 0;
		i++;
	}
}

/*
 * eb_bitmap_offset() - calculate the page and offset of the byte containing the
 * given bit number
 * @eb: the extent buffer
 * @start: offset of the bitmap item in the extent buffer
 * @nr: bit number
 * @page_index: return index of the page in the extent buffer that contains the
 * given bit number
 * @page_offset: return offset into the page given by page_index
 *
 * This helper hides the ugliness of finding the byte in an extent buffer which
 * contains a given bit.
 */
static inline void eb_bitmap_offset(struct extent_buffer *eb,
				    unsigned long start, unsigned long nr,
				    unsigned long *page_index,
				    size_t *page_offset)
{
	size_t start_offset = offset_in_page(eb->start);
	size_t byte_offset = BIT_BYTE(nr);
	size_t offset;

	/*
	 * The byte we want is the offset of the extent buffer + the offset of
	 * the bitmap item in the extent buffer + the offset of the byte in the
	 * bitmap item.
	 */
	offset = start_offset + start + byte_offset;

	*page_index = offset >> PAGE_SHIFT;
	*page_offset = offset_in_page(offset);
}

/**
 * extent_buffer_test_bit - determine whether a bit in a bitmap item is set
 * @eb: the extent buffer
 * @start: offset of the bitmap item in the extent buffer
 * @nr: bit number to test
 */
int extent_buffer_test_bit(struct extent_buffer *eb, unsigned long start,
			   unsigned long nr)
{
	u8 *kaddr;
	struct page *page;
	unsigned long i;
	size_t offset;

	eb_bitmap_offset(eb, start, nr, &i, &offset);
	page = eb->pages[i];
	WARN_ON(!PageUptodate(page));
	kaddr = page_address(page);
	return 1U & (kaddr[offset] >> (nr & (BITS_PER_BYTE - 1)));
}

/**
 * extent_buffer_bitmap_set - set an area of a bitmap
 * @eb: the extent buffer
 * @start: offset of the bitmap item in the extent buffer
 * @pos: bit number of the first bit
 * @len: number of bits to set
 */
void extent_buffer_bitmap_set(struct extent_buffer *eb, unsigned long start,
			      unsigned long pos, unsigned long len)
{
	u8 *kaddr;
	struct page *page;
	unsigned long i;
	size_t offset;
	const unsigned int size = pos + len;
	int bits_to_set = BITS_PER_BYTE - (pos % BITS_PER_BYTE);
	u8 mask_to_set = BITMAP_FIRST_BYTE_MASK(pos);

	eb_bitmap_offset(eb, start, pos, &i, &offset);
	page = eb->pages[i];
	WARN_ON(!PageUptodate(page));
	kaddr = page_address(page);

	while (len >= bits_to_set) {
		kaddr[offset] |= mask_to_set;
		len -= bits_to_set;
		bits_to_set = BITS_PER_BYTE;
		mask_to_set = ~0;
		if (++offset >= PAGE_SIZE && len > 0) {
			offset = 0;
			page = eb->pages[++i];
			WARN_ON(!PageUptodate(page));
			kaddr = page_address(page);
		}
	}
	if (len) {
		mask_to_set &= BITMAP_LAST_BYTE_MASK(size);
		kaddr[offset] |= mask_to_set;
	}
}


/**
 * extent_buffer_bitmap_clear - clear an area of a bitmap
 * @eb: the extent buffer
 * @start: offset of the bitmap item in the extent buffer
 * @pos: bit number of the first bit
 * @len: number of bits to clear
 */
void extent_buffer_bitmap_clear(struct extent_buffer *eb, unsigned long start,
				unsigned long pos, unsigned long len)
{
	u8 *kaddr;
	struct page *page;
	unsigned long i;
	size_t offset;
	const unsigned int size = pos + len;
	int bits_to_clear = BITS_PER_BYTE - (pos % BITS_PER_BYTE);
	u8 mask_to_clear = BITMAP_FIRST_BYTE_MASK(pos);

	eb_bitmap_offset(eb, start, pos, &i, &offset);
	page = eb->pages[i];
	WARN_ON(!PageUptodate(page));
	kaddr = page_address(page);

	while (len >= bits_to_clear) {
		kaddr[offset] &= ~mask_to_clear;
		len -= bits_to_clear;
		bits_to_clear = BITS_PER_BYTE;
		mask_to_clear = ~0;
		if (++offset >= PAGE_SIZE && len > 0) {
			offset = 0;
			page = eb->pages[++i];
			WARN_ON(!PageUptodate(page));
			kaddr = page_address(page);
		}
	}
	if (len) {
		mask_to_clear &= BITMAP_LAST_BYTE_MASK(size);
		kaddr[offset] &= ~mask_to_clear;
	}
}

static inline bool areas_overlap(unsigned long src, unsigned long dst, unsigned long len)
{
	unsigned long distance = (src > dst) ? src - dst : dst - src;
	return distance < len;
}

static void copy_pages(struct page *dst_page, struct page *src_page,
		       unsigned long dst_off, unsigned long src_off,
		       unsigned long len)
{
	char *dst_kaddr = page_address(dst_page);
	char *src_kaddr;
	int must_memmove = 0;

	if (dst_page != src_page) {
		src_kaddr = page_address(src_page);
	} else {
		src_kaddr = dst_kaddr;
		if (areas_overlap(src_off, dst_off, len))
			must_memmove = 1;
	}

	if (must_memmove)
		memmove(dst_kaddr + dst_off, src_kaddr + src_off, len);
	else
		memcpy(dst_kaddr + dst_off, src_kaddr + src_off, len);
}

void memcpy_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len)
{
	struct btrfs_fs_info *fs_info = dst->fs_info;
	size_t cur;
	size_t dst_off_in_page;
	size_t src_off_in_page;
	size_t start_offset = offset_in_page(dst->start);
	unsigned long dst_i;
	unsigned long src_i;

	if (src_offset + len > dst->len) {
		btrfs_err(fs_info,
			"memmove bogus src_offset %lu move len %lu dst len %lu",
			 src_offset, len, dst->len);
		BUG();
	}
	if (dst_offset + len > dst->len) {
		btrfs_err(fs_info,
			"memmove bogus dst_offset %lu move len %lu dst len %lu",
			 dst_offset, len, dst->len);
		BUG();
	}

	while (len > 0) {
		dst_off_in_page = offset_in_page(start_offset + dst_offset);
		src_off_in_page = offset_in_page(start_offset + src_offset);

		dst_i = (start_offset + dst_offset) >> PAGE_SHIFT;
		src_i = (start_offset + src_offset) >> PAGE_SHIFT;

		cur = min(len, (unsigned long)(PAGE_SIZE -
					       src_off_in_page));
		cur = min_t(unsigned long, cur,
			(unsigned long)(PAGE_SIZE - dst_off_in_page));

		copy_pages(dst->pages[dst_i], dst->pages[src_i],
			   dst_off_in_page, src_off_in_page, cur);

		src_offset += cur;
		dst_offset += cur;
		len -= cur;
	}
}

void memmove_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len)
{
	struct btrfs_fs_info *fs_info = dst->fs_info;
	size_t cur;
	size_t dst_off_in_page;
	size_t src_off_in_page;
	unsigned long dst_end = dst_offset + len - 1;
	unsigned long src_end = src_offset + len - 1;
	size_t start_offset = offset_in_page(dst->start);
	unsigned long dst_i;
	unsigned long src_i;

	if (src_offset + len > dst->len) {
		btrfs_err(fs_info,
			  "memmove bogus src_offset %lu move len %lu len %lu",
			  src_offset, len, dst->len);
		BUG();
	}
	if (dst_offset + len > dst->len) {
		btrfs_err(fs_info,
			  "memmove bogus dst_offset %lu move len %lu len %lu",
			  dst_offset, len, dst->len);
		BUG();
	}
	if (dst_offset < src_offset) {
		memcpy_extent_buffer(dst, dst_offset, src_offset, len);
		return;
	}
	while (len > 0) {
		dst_i = (start_offset + dst_end) >> PAGE_SHIFT;
		src_i = (start_offset + src_end) >> PAGE_SHIFT;

		dst_off_in_page = offset_in_page(start_offset + dst_end);
		src_off_in_page = offset_in_page(start_offset + src_end);

		cur = min_t(unsigned long, len, src_off_in_page + 1);
		cur = min(cur, dst_off_in_page + 1);
		copy_pages(dst->pages[dst_i], dst->pages[src_i],
			   dst_off_in_page - cur + 1,
			   src_off_in_page - cur + 1, cur);

		dst_end -= cur;
		src_end -= cur;
		len -= cur;
	}
}

int try_release_extent_buffer(struct page *page)
{
	struct extent_buffer *eb;

	/*
	 * We need to make sure nobody is attaching this page to an eb right
	 * now.
	 */
	spin_lock(&page->mapping->private_lock);
	if (!PagePrivate(page)) {
		spin_unlock(&page->mapping->private_lock);
		return 1;
	}

	eb = (struct extent_buffer *)page->private;
	BUG_ON(!eb);

	/*
	 * This is a little awful but should be ok, we need to make sure that
	 * the eb doesn't disappear out from under us while we're looking at
	 * this page.
	 */
	spin_lock(&eb->refs_lock);
	if (atomic_read(&eb->refs) != 1 || extent_buffer_under_io(eb)) {
		spin_unlock(&eb->refs_lock);
		spin_unlock(&page->mapping->private_lock);
		return 0;
	}
	spin_unlock(&page->mapping->private_lock);

	/*
	 * If tree ref isn't set then we know the ref on this eb is a real ref,
	 * so just return, this page will likely be freed soon anyway.
	 */
	if (!test_and_clear_bit(EXTENT_BUFFER_TREE_REF, &eb->bflags)) {
		spin_unlock(&eb->refs_lock);
		return 0;
	}

	return release_extent_buffer(eb);
}
