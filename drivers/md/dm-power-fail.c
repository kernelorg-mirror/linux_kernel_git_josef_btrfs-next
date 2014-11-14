/*
 * Copyright (C) 2014 Facebook. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include <linux/device-mapper.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>

#define DM_MSG_PREFIX "power-fail"

/*
 * The way this interface is meant to be used is like this
 *
 * dmsetup create powerfail
 * mkfs /dev/powerfail
 * mount /dev/powerfail /mnt/test
 * do some stuff &
 * sleep 30
 * dmsetup message powerfail (drop_after_flush|drop_after_fua|drop_writes)
 * umount /mnt/test
 * dmsetup message powerfail redirect_reads
 * fsck /dev/powerfail || exit 1
 * mount /dev/powerfail /mnt/test
 * <verify contents>
 *
 * You can set redirect_reads whenever you want, but the idea is that you want
 * the teardown stuff to work like normal, and then flip the switch for us to
 * start returning garbage on any writes that would have failed, and then do
 * verification checks.  A perfectly functioning fs will recover properly and
 * not give you IO errors and such.
 *
 * There are two modes for this target.
 *
 * zero - any write IO's that are lost once the power fail event happens will
 * return 0's when read after redirect_reads is set.  This is meant for btrfs or
 * any other future COW fs that comes along.
 *
 * split - we split the device in half, and write to alternating sides of the
 * device.  We track which is the last good mirror to have completed.  Then
 * whenever the power fail event occurs we will stop updating the last good
 * mirror for writes and when redirect_reads is turned on we will read from the
 * last good mirror.
 */

struct pfail_ctx {
	struct dm_dev *dev;
	spinlock_t pending_blocks_lock;
	struct rb_root pending_blocks;
	struct list_head unflushed_blocks;
	long endio_delay;
	unsigned long flags;
	bool split;
	bool error;
};

enum pfail_flag_bits {
	DROP_WRITES,
	DROP_AFTER_FUA,
	DROP_AFTER_FLUSH,
	REDIRECT_READS,
};

struct pending_block {
	struct rb_node node;
	struct list_head list;
	unsigned long long bytenr;
	unsigned bytes;
	unsigned good_mirror;
	unsigned cur_mirror;
};

struct per_bio_data {
	struct pending_block *block;
	struct pfail_ctx *pc;
	struct work_struct work;
	int error;
	bool track;
};

static int contains(unsigned long long a, unsigned a_len,
		    unsigned long long b, unsigned b_len)
{
	if (a >= (b + b_len))
		return 0;
	if (b >= (a + a_len))
		return 0;
	return 1;
}

static struct pending_block *insert_pending_block(struct pfail_ctx *pc,
						  struct pending_block *block)
{
	struct rb_node **p = &pc->pending_blocks.rb_node;
	struct rb_node *parent = NULL;
	struct pending_block *entry;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct pending_block, node);

		if (contains(block->bytenr, block->bytes, entry->bytenr,
			     entry->bytes)) {
			if (!pc->split) {
				/*
				 * With zero mode we free up blocks once they
				 * successfully complete, and assume the fs
				 * doesn't write to the same block until it has
				 * been completely written, so if this happens
				 * we have a problem.
				 */
				DMERR("existing block %llu-%u insert "
				      "%llu-%u", entry->bytenr, entry->bytes,
				       block->bytenr, block->bytes);
			}
			kfree(block);
			return entry;
		} else if (entry->bytenr > block->bytenr)
			p = &(*p)->rb_left;
		else if (entry->bytenr < block->bytenr)
			p = &(*p)->rb_right;
	}

	rb_link_node(&block->node, parent, p);
	rb_insert_color(&block->node, &pc->pending_blocks);
	return block;
}

static struct pending_block *find_pending_block(struct pfail_ctx *pc,
						unsigned long long bytenr,
						unsigned bytes)
{
	struct rb_node *n = pc->pending_blocks.rb_node;
	struct pending_block *block;

	while (n) {
		block = rb_entry(n, struct pending_block, node);
		if (contains(block->bytenr, block->bytes, bytenr, bytes))
			return block;
		else if (block->bytenr > bytenr)
			n = n->rb_left;
		else if (block->bytenr < bytenr)
			n = n->rb_right;
	}
	return NULL;
}

static int parse_pfail_features(struct dm_arg_set *as, struct pfail_ctx *pc,
				struct dm_target *ti)
{
	const char *arg_name;
	unsigned argc;
	int ret;
	static struct dm_arg _args[] = {
		{0, 3, "Invalid number of power fail feature arguments"},
	};

	if (!as->argc)
		return 0;
	ret = dm_read_arg_group(_args, as, &argc, &ti->error);
	if (ret)
		return -EINVAL;

	while (argc) {
		arg_name = dm_shift_arg(as);
		argc--;

		if (strcasecmp(arg_name, "split")) {
			pc->split = true;
		} else if (strcasecmp(arg_name, "error_on_fail")) {
			pc->error = true;
		} else {
			ti->error = "Unrecognized power fail feature "
				"requested";
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

/*
 * Construct a power-fail mapping:
 * power-fail <dev_path> <endio_delay> [<#feature args [<arg>]*]
 *
 * endio_delay is in jiffies, if it is 0 there will be no delay.  This is
 * helpful for widening suspected races in your waiting logic.
 *
 * Option feature arguments are:
 *
 * split - Meant for overwrite fs'es where we need to return the old busted data
 *	   for reads on uncompleted blocks.  Must pass in a length of >= half of
 *	   the disk so that we can use one side for pending blocks and one side
 *	   for completed blocks
 * error_on_fail - Instead of just pretending that the writes completed normally
 *		   after the failure event, we will return -EIO.
 */
static int pfail_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct pfail_ctx *pc;
	struct dm_arg_set as;
	const char *devname;
	sector_t sectors;
	long endio_delay;
	char dummy;
	int ret;

	as.argc = argc;
	as.argv = argv;

	if (argc < 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	pc = kzalloc(sizeof(*pc), GFP_KERNEL);
	if (!pc) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}
	pc->pending_blocks = RB_ROOT;
	spin_lock_init(&pc->pending_blocks_lock);
	INIT_LIST_HEAD(&pc->unflushed_blocks);

	devname = dm_shift_arg(&as);
	if (sscanf(dm_shift_arg(&as), "%ld%c", &endio_delay, &dummy) != 1) {
		ti->error = "Invalid endio delay";
		goto bad;
	}
	pc->endio_delay = endio_delay;
	ret = parse_pfail_features(&as, pc, ti);
	if (ret)
		goto bad;

	if (dm_get_device(ti, devname, dm_table_get_mode(ti->table), &pc->dev)) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	ret = dm_set_target_max_io_len(ti, PAGE_CACHE_SIZE >> 9);
	if (ret)
		goto bad;

	sectors = (i_size_read(pc->dev->bdev->bd_inode) >> 9) - ti->begin;
	sectors /= 2;
	if (pc->split && ti->len > sectors) {
		ti->error = "Using split but specified a device size that is "
			"too large";
		dm_put_device(ti, pc->dev);
		goto bad;
	}

	ti->num_flush_bios = 1;
	ti->flush_supported = true;
	ti->num_discard_bios = 1;
	ti->per_bio_data_size = sizeof(struct per_bio_data);
	ti->private = pc;
	return 0;

bad:
	kfree(pc);
	return -EINVAL;
}

static void pfail_dtr(struct dm_target *ti)
{
	struct pfail_ctx *pc = ti->private;
	struct pending_block *block;
	struct rb_node *n;

	dm_put_device(ti, pc->dev);
	printk(KERN_ERR "dropping zero'ed blocks\n");
	while ((n = rb_last(&pc->pending_blocks)) != NULL) {
		block = rb_entry(n, struct pending_block, node);
		rb_erase(&block->node, &pc->pending_blocks);
		kfree(block);
	}
	kfree(pc);
}

static sector_t pfail_map_sector(struct dm_target *ti, sector_t bi_sector,
				 int mirror)
{
	WARN_ON(bi_sector > ti->len);
	if (mirror == 2)
		bi_sector += ti->len;
	return dm_target_offset(ti, bi_sector);
}

static void pfail_map_bio(struct dm_target *ti, struct bio *bio, int mirror)
{
	struct pfail_ctx *pc = ti->private;

	bio->bi_bdev = pc->dev->bdev;
	if (bio_sectors(bio))
		bio->bi_iter.bi_sector =
			pfail_map_sector(ti, bio->bi_iter.bi_sector, mirror);
}

static int maybe_zero_bio(struct pfail_ctx *pc, struct bio *bio, int *mirror)
{
	struct pending_block *block;
	char *data = bio_data(bio);
	unsigned long long bytenr = bio->bi_iter.bi_sector << 9;
	int good_mirror, cur_mirror;
	int ret = 0;

	if (!bio_has_data(bio))
		return 0;

	spin_lock(&pc->pending_blocks_lock);
	block = find_pending_block(pc, bytenr, bio_cur_bytes(bio));
	if (block) {
		good_mirror = block->good_mirror;
		cur_mirror = block->cur_mirror;
	}
	spin_unlock(&pc->pending_blocks_lock);
	if (!block)
		return 0;

	if (test_bit(REDIRECT_READS, &pc->flags)) {
		if (pc->split) {
			*mirror = good_mirror;
		} else {
			printk(KERN_ERR "zeroing block %llu\n", block->bytenr);
			memset(data, 0, bio_cur_bytes(bio));
			ret = 1;
		}
	} else if (pc->split) {
		*mirror = cur_mirror;
	}

	return ret;
}

static unsigned bio_bytes(struct bio *bio)
{
	if (!bio_has_data(bio))
		return 0;

	return bio_cur_bytes(bio);
}

static int pfail_map(struct dm_target *ti, struct bio *bio)
{
	struct pfail_ctx *pc = ti->private;
	struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
	struct pending_block *block;
	unsigned mirror = 0;
	bool fua_bio = (bio->bi_rw & REQ_FUA);
	bool flush_bio = (bio->bi_rw & REQ_FLUSH);

	if (bio_sectors(bio) || flush_bio)
		pb->track = true;
	else
		pb->track = false;
	pb->pc = pc;
	pb->block = NULL;

	/*
	 * Map reads as normal.
	 */
	if (bio_data_dir(bio) == READ) {
		if (maybe_zero_bio(pc, bio, &mirror)) {
			bio_endio(bio, 0);
			return DM_MAPIO_SUBMITTED;
		}
		goto map_bio;
	}

	if (test_bit(DROP_WRITES, &pc->flags)) {
		if (pc->error)
			return -EIO;
		pb->track = false;
		bio_endio(bio, 0);
		return DM_MAPIO_SUBMITTED;
	}

	/*
	 * We want to allow the last fua to go through and then block
	 * anybody else, emit a message for scripts that want to
	 * automate checking.
	 */
	if (fua_bio && test_bit(DROP_AFTER_FUA, &pc->flags)) {
		if (!test_and_set_bit(DROP_WRITES, &pc->flags)) {
			DMINFO("fua seen, dropping future writes");
			goto map_bio;
		}
		pb->track = false;
		bio_endio(bio, 0);
		return DM_MAPIO_SUBMITTED;
	}

	/* Same as after_fua only we do it as soon as we see a flush */
	if (flush_bio && test_bit(DROP_AFTER_FLUSH, &pc->flags)) {
		if (!test_and_set_bit(DROP_WRITES, &pc->flags)) {
			DMINFO("flush seen, dropping future writes");
			goto map_bio;
		}
		pb->track = false;
		bio_endio(bio, 0);
		return DM_MAPIO_SUBMITTED;
	}

map_bio:
	if (flush_bio) {
		struct pending_block *block;

		block = kzalloc(sizeof(struct pending_block), GFP_NOIO);
		if (!block) {
			DMERR("Error allocating pending block");
			return -ENOMEM;
		}
		INIT_LIST_HEAD(&block->list);

		/*
		 * IMPORTANT NOTE FOR FS DEVELOPERS!!!!
		 *
		 * We only take blocks that have already come through the end_io
		 * handler as flushable blocks.  Anything that is still
		 * outstanding IO at this point is assumed to be unflushable.
		 */
		spin_lock(&pc->pending_blocks_lock);
		list_splice_init(&pc->unflushed_blocks, &block->list);
		spin_unlock(&pc->pending_blocks_lock);
		pb->block = block;
	} else if (bio_data_dir(bio) == WRITE && !flush_bio && !fua_bio) {

		block = kzalloc(sizeof(struct pending_block), GFP_NOIO);
		if (!block) {
			DMERR("Error allocating pending block");
			return -ENOMEM;
		}
		block->bytenr = bio->bi_iter.bi_sector << 9;
		block->bytes = bio_bytes(bio);
		INIT_LIST_HEAD(&block->list);
		spin_lock(&pc->pending_blocks_lock);
		block = insert_pending_block(pc, block);
		if (pc->split) {
			if (block->good_mirror == 0 ||
			    block->good_mirror == 2)
				block->cur_mirror = 1;
			else
				block->cur_mirror = 2;
			mirror = block->cur_mirror;
		}
		pb->block = block;
		spin_unlock(&pc->pending_blocks_lock);
	}
	pfail_map_bio(ti, bio, mirror);

	return DM_MAPIO_REMAPPED;
}

static void pfail_write_end_io(struct work_struct *work)
{
	struct per_bio_data *pb = container_of(work, struct per_bio_data,
					       work);
	struct bio *bio;
	struct pfail_ctx *pc = pb->pc;
	struct pending_block *block;
	bool flush, fua;

	bio = dm_bio_from_per_bio_data(pb, sizeof(struct per_bio_data));
	flush = bio->bi_rw & REQ_FLUSH;
	fua = bio->bi_rw & REQ_FUA;
	pb->track = false;
	block = pb->block;

	if (pc->endio_delay)
		schedule_timeout(pc->endio_delay);

	/*
	 * If we have drop_after_fua/drop_after_flush we want to allow the last
	 * flush/fua to finish so everybody is happy.
	 */
	if (test_bit(DROP_WRITES, &pc->flags) &&
	    !(fua && test_bit(DROP_AFTER_FUA, &pc->flags)) &&
	    !(flush && test_bit(DROP_AFTER_FLUSH, &pc->flags))) {
		if (block)
			printk(KERN_ERR "block %llu-%u completed after fail\n",
			       block->bytenr, block->bytes);
		if (flush)
			kfree(block);
		goto out;
	}

	if ((flush || fua) && test_bit(DROP_WRITES, &pc->flags)) {
		struct pending_block *tmp;

		list_for_each_entry(tmp, &pc->unflushed_blocks, list)
			printk(KERN_ERR "unflushed block at %llu-%u\n",
			       tmp->bytenr, tmp->bytes);
	}

	if (flush) {
		struct pending_block *tmp, *n;

		if (!block)
			goto out;

		spin_lock(&pc->pending_blocks_lock);
		list_for_each_entry_safe(tmp, n, &block->list, list) {
			list_del_init(&tmp->list);
			if (pc->split) {
				tmp->good_mirror = tmp->cur_mirror;
			} else {
				rb_erase(&tmp->node, &pc->pending_blocks);
				kfree(tmp);
			}
			cond_resched_lock(&pc->pending_blocks_lock);
		}
		spin_unlock(&pc->pending_blocks_lock);
		kfree(block);
	} else if (block) {
		spin_lock(&pc->pending_blocks_lock);
		list_move_tail(&block->list, &pc->unflushed_blocks);
		spin_unlock(&pc->pending_blocks_lock);
	}
out:
	bio_endio(bio, pb->error);
}

static int pfail_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	struct pfail_ctx *pc = ti->private;
	struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));

	if (bio_data_dir(bio) == WRITE && pb->track &&
	    !test_bit(DROP_WRITES, &pc->flags)) {
		pb->error = error;

		atomic_inc(&bio->bi_remaining);
		INIT_WORK(&pb->work, pfail_write_end_io);
		schedule_work(&pb->work);
		return DM_ENDIO_INCOMPLETE;
	}

	return error;
}

static void pfail_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	unsigned sz = 0;
	struct pfail_ctx *pc = ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s %ld ", pc->dev->name, pc->endio_delay);

		DMEMIT("%u ", pc->split + pc->error);

		if (pc->split)
			DMEMIT("split ");
		if (pc->error)
			DMEMIT("error_on_fail ");
		break;
	}
}

static int pfail_ioctl(struct dm_target *ti, unsigned int cmd, unsigned long arg)
{
	struct pfail_ctx *pc = ti->private;
	struct dm_dev *dev = pc->dev;
	int r = 0;

	/*
	 * Only pass ioctls through if the device sizes match exactly.
	 */
	if (ti->len != i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT)
		r = scsi_verify_blk_ioctl(NULL, cmd);

	return r ? : __blkdev_driver_ioctl(dev->bdev, dev->mode, cmd, arg);
}

static int pfail_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			struct bio_vec *biovec, int max_size)
{
	struct pfail_ctx *pc = ti->private;
	struct request_queue *q = bdev_get_queue(pc->dev->bdev);

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = pc->dev->bdev;
	bvm->bi_sector = pfail_map_sector(ti, bvm->bi_sector, 0);

	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

static int pfail_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct pfail_ctx *pc = ti->private;

	return fn(ti, pc->dev, 0, ti->len, data);
}

/*
 * Valid messages
 *
 * drop_after_fua - drop all writes after the next seen fua write.
 * drop_after_flush - drop all writes after the next seen flush write.
 * drop_writes - drop all writes from now on.
 * redirect_reads - start returning 0's/old data for reads done on unflushed
 *	blocks.
 * allow_writes - allow writes to start back up again.
 */
static int pfail_message(struct dm_target *ti, unsigned argc, char **argv)
{
	struct pfail_ctx *pc = ti->private;

	if (argc != 1) {
		DMWARN("Invalid power-fail message arguments, expect 1 "
		       "argument, got %d", argc);
		return -EINVAL;
	}

	if (!strcasecmp(argv[0], "redirect_reads")) {
		set_bit(REDIRECT_READS, &pc->flags);
	} else if (!strcasecmp(argv[0], "drop_after_fua")) {
		set_bit(DROP_AFTER_FUA, &pc->flags);
	} else if (!strcasecmp(argv[0], "drop_after_flush")) {
		set_bit(DROP_AFTER_FLUSH, &pc->flags);
	} else if (!strcasecmp(argv[0], "drop_writes")) {
		set_bit(DROP_WRITES, &pc->flags);
	} else if (!strcasecmp(argv[0], "allow_writes")) {
		clear_bit(DROP_WRITES, &pc->flags);
		clear_bit(DROP_AFTER_FUA, &pc->flags);
		clear_bit(DROP_AFTER_FLUSH, &pc->flags);
	} else {
		DMWARN("Invalid argument %s", argv[0]);
		return -EINVAL;
	}

	return 0;
}

static struct target_type pfail_target = {
	.name   = "power-fail",
	.version = {1, 3, 1},
	.module = THIS_MODULE,
	.ctr    = pfail_ctr,
	.dtr    = pfail_dtr,
	.map    = pfail_map,
	.end_io = pfail_end_io,
	.status = pfail_status,
	.ioctl	= pfail_ioctl,
	.merge	= pfail_merge,
	.message = pfail_message,
	.iterate_devices = pfail_iterate_devices,
};

static int __init dm_pfail_init(void)
{
	int r = dm_register_target(&pfail_target);

	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

static void __exit dm_pfail_exit(void)
{
	dm_unregister_target(&pfail_target);
}

/* Module hooks */
module_init(dm_pfail_init);
module_exit(dm_pfail_exit);

MODULE_DESCRIPTION(DM_NAME " power fail target");
MODULE_AUTHOR("Josef Bacik <jbacik@fb.com>");
MODULE_LICENSE("GPL");
