/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BTRFS_EXTENT_BUFFER_H
#define BTRFS_EXTENT_BUFFER_H

enum {
	EXTENT_BUFFER_UPTODATE,
	EXTENT_BUFFER_DIRTY,
	EXTENT_BUFFER_CORRUPT,
	/* this got triggered by readahead */
	EXTENT_BUFFER_READAHEAD,
	EXTENT_BUFFER_TREE_REF,
	EXTENT_BUFFER_STALE,
	EXTENT_BUFFER_WRITEBACK,
	/* read IO error */
	EXTENT_BUFFER_READ_ERR,
	EXTENT_BUFFER_UNMAPPED,
	EXTENT_BUFFER_IN_TREE,
	/* write IO error */
	EXTENT_BUFFER_WRITE_ERR,
};

#define INLINE_EXTENT_BUFFER_PAGES 16
#define MAX_INLINE_EXTENT_BUFFER_SIZE (INLINE_EXTENT_BUFFER_PAGES * PAGE_SIZE)
struct extent_buffer {
	u64 start;
	unsigned long len;
	unsigned long bflags;
	struct btrfs_fs_info *fs_info;
	spinlock_t refs_lock;
	atomic_t refs;
	atomic_t io_pages;
	int read_mirror;
	struct rcu_head rcu_head;
	pid_t lock_owner;

	int blocking_writers;
	atomic_t blocking_readers;
	bool lock_nested;
	/* >= 0 if eb belongs to a log tree, -1 otherwise */
	short log_index;

	/* protects write locks */
	rwlock_t lock;

	/* readers use lock_wq while they wait for the write
	 * lock holders to unlock
	 */
	wait_queue_head_t write_lock_wq;

	/* writers use read_lock_wq while they wait for readers
	 * to unlock
	 */
	wait_queue_head_t read_lock_wq;
	struct page *pages[INLINE_EXTENT_BUFFER_PAGES];
#ifdef CONFIG_BTRFS_DEBUG
	int spinning_writers;
	atomic_t spinning_readers;
	atomic_t read_locks;
	int write_locks;
	struct list_head leak_list;
#endif
};

/*
 * The extent buffer bitmap operations are done with byte granularity instead of
 * word granularity for two reasons:
 * 1. The bitmaps must be little-endian on disk.
 * 2. Bitmap items are not guaranteed to be aligned to a word and therefore a
 *    single word in a bitmap may straddle two pages in the extent buffer.
 */
#define BIT_BYTE(nr) ((nr) / BITS_PER_BYTE)
#define BYTE_MASK ((1 << BITS_PER_BYTE) - 1)
#define BITMAP_FIRST_BYTE_MASK(start) \
	((BYTE_MASK << ((start) & (BITS_PER_BYTE - 1))) & BYTE_MASK)
#define BITMAP_LAST_BYTE_MASK(nbits) \
	(BYTE_MASK >> (-(nbits) & (BITS_PER_BYTE - 1)))

int __init extent_buffer_init(void);
void __cold extent_buffer_exit(void);
int try_release_extent_buffer(struct page *page);
struct extent_buffer *alloc_extent_buffer(struct btrfs_fs_info *fs_info,
					  u64 start);
struct extent_buffer *__alloc_dummy_extent_buffer(struct btrfs_fs_info *fs_info,
						  u64 start, unsigned long len);
struct extent_buffer *alloc_dummy_extent_buffer(struct btrfs_fs_info *fs_info,
						u64 start);
struct extent_buffer *alloc_test_extent_buffer(struct btrfs_fs_info *fs_info,
					       u64 start);
struct extent_buffer *btrfs_clone_extent_buffer(struct extent_buffer *src);
struct extent_buffer *find_extent_buffer(struct btrfs_fs_info *fs_info,
					 u64 start);
void free_extent_buffer(struct extent_buffer *eb);
void free_extent_buffer_stale(struct extent_buffer *eb);

void wait_on_extent_buffer_writeback(struct extent_buffer *eb);

static inline int num_extent_pages(const struct extent_buffer *eb)
{
	return (round_up(eb->start + eb->len, PAGE_SIZE) >> PAGE_SHIFT) -
	       (eb->start >> PAGE_SHIFT);
}

static inline void extent_buffer_get(struct extent_buffer *eb)
{
	atomic_inc(&eb->refs);
}

static inline int extent_buffer_uptodate(struct extent_buffer *eb)
{
	return test_bit(EXTENT_BUFFER_UPTODATE, &eb->bflags);
}

int memcmp_extent_buffer(const struct extent_buffer *eb, const void *ptrv,
			 unsigned long start, unsigned long len);
void read_extent_buffer(const struct extent_buffer *eb, void *dst,
			unsigned long start,
			unsigned long len);
int read_extent_buffer_to_user(const struct extent_buffer *eb,
			       void __user *dst, unsigned long start,
			       unsigned long len);
void write_extent_buffer_fsid(struct extent_buffer *eb, const void *src);
void write_extent_buffer_chunk_tree_uuid(struct extent_buffer *eb,
		const void *src);
void write_extent_buffer(struct extent_buffer *eb, const void *src,
			 unsigned long start, unsigned long len);
void copy_extent_buffer_full(struct extent_buffer *dst,
			     struct extent_buffer *src);
void copy_extent_buffer(struct extent_buffer *dst, struct extent_buffer *src,
			unsigned long dst_offset, unsigned long src_offset,
			unsigned long len);
void memcpy_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len);
void memmove_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len);
void memzero_extent_buffer(struct extent_buffer *eb, unsigned long start,
			   unsigned long len);
int extent_buffer_test_bit(struct extent_buffer *eb, unsigned long start,
			   unsigned long pos);
void extent_buffer_bitmap_set(struct extent_buffer *eb, unsigned long start,
			      unsigned long pos, unsigned long len);
void extent_buffer_bitmap_clear(struct extent_buffer *eb, unsigned long start,
				unsigned long pos, unsigned long len);
void clear_extent_buffer_dirty(struct extent_buffer *eb);
bool set_extent_buffer_dirty(struct extent_buffer *eb);
void set_extent_buffer_uptodate(struct extent_buffer *eb);
void clear_extent_buffer_uptodate(struct extent_buffer *eb);
int extent_buffer_under_io(struct extent_buffer *eb);
int map_private_extent_buffer(const struct extent_buffer *eb,
			      unsigned long offset, unsigned long min_len,
			      char **map, unsigned long *map_start,
			      unsigned long *map_len);
#endif /* BTRFS_EXTENT_BUFFER_H */
