#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fsnotify_backend.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/inotify.h>
#include <linux/fanotify.h>
#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/exportfs.h>

#include "inotify/inotify.h"
#include "../fs/mount.h"

#if defined(CONFIG_PROC_FS)

#if defined(CONFIG_INOTIFY_USER) || defined(CONFIG_FANOTIFY)

#if defined(CONFIG_EXPORTFS)
static void show_mark_fhandle(struct seq_file *m, struct inode *inode)
{
	struct {
		struct file_handle handle;
		u8 pad[MAX_HANDLE_SZ];
	} f;
	int size, ret, i;

	f.handle.handle_bytes = sizeof(f.pad);
	size = f.handle.handle_bytes >> 2;

	ret = exportfs_encode_inode_fh(inode, (struct fid *)f.handle.f_handle, &size, 0);
	if ((ret == FILEID_INVALID) || (ret < 0)) {
		WARN_ONCE(1, "Can't encode file handler for inotify: %d\n", ret);
		return;
	}

	f.handle.handle_type = ret;
	f.handle.handle_bytes = size * sizeof(u32);

	seq_printf(m, "fhandle-bytes:%x fhandle-type:%x f_handle:",
		   f.handle.handle_bytes, f.handle.handle_type);

	for (i = 0; i < f.handle.handle_bytes; i++)
		seq_printf(m, "%02x", (int)f.handle.f_handle[i]);
}
#else
static void show_mark_fhandle(struct seq_file *m, struct inode *inode)
{
}
#endif

#ifdef CONFIG_INOTIFY_USER

void inotify_show_fdinfo(struct seq_file *m, struct file *f, void *v)
{
	struct fsnotify_group *group = f->private_data;
	struct list_head *cur = v;
	struct fsnotify_mark *mark = list_entry(cur, struct fsnotify_mark,
						g_list);
	struct inotify_inode_mark *inode_mark;
	struct inode *inode;

	if (v == &group->marks_list)
		return;

	if (!(mark->flags & FSNOTIFY_MARK_FLAG_ALIVE) ||
	    !(mark->flags & FSNOTIFY_MARK_FLAG_INODE))
		return;

	inode_mark = container_of(mark, struct inotify_inode_mark, fsn_mark);
	inode = igrab(mark->inode);
	if (inode) {
		/*
		 * IN_ALL_EVENTS represents all of the mask bits
		 * that we expose to userspace.  There is at
		 * least one bit (FS_EVENT_ON_CHILD) which is
		 * used only internally to the kernel.
		 */
		u32 mask = mark->mask & IN_ALL_EVENTS;
		seq_printf(m, "inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:%x ",
			   inode_mark->wd, inode->i_ino, inode->i_sb->s_dev,
			   mask, mark->ignored_mask);
		show_mark_fhandle(m, inode);
		seq_putc(m, '\n');
		iput(inode);
	}
}

#endif /* CONFIG_INOTIFY_USER */

#ifdef CONFIG_FANOTIFY

static void fanotify_fdinfo(struct seq_file *m, struct fsnotify_mark *mark)
{
	unsigned int mflags = 0;
	struct inode *inode;

	if (!(mark->flags & FSNOTIFY_MARK_FLAG_ALIVE))
		return;

	if (mark->flags & FSNOTIFY_MARK_FLAG_IGNORED_SURV_MODIFY)
		mflags |= FAN_MARK_IGNORED_SURV_MODIFY;

	if (mark->flags & FSNOTIFY_MARK_FLAG_INODE) {
		inode = igrab(mark->inode);
		if (!inode)
			return;
		seq_printf(m, "fanotify ino:%lx sdev:%x mflags:%x mask:%x ignored_mask:%x ",
			   inode->i_ino, inode->i_sb->s_dev,
			   mflags, mark->mask, mark->ignored_mask);
		show_mark_fhandle(m, inode);
		seq_putc(m, '\n');
		iput(inode);
	} else if (mark->flags & FSNOTIFY_MARK_FLAG_VFSMOUNT) {
		struct mount *mnt = real_mount(mark->mnt);

		seq_printf(m, "fanotify mnt_id:%x mflags:%x mask:%x ignored_mask:%x\n",
			   mnt->mnt_id, mflags, mark->mask, mark->ignored_mask);
	}
}

void fanotify_show_fdinfo(struct seq_file *m, struct file *f, void *v)
{
	struct fsnotify_group *group = f->private_data;
	unsigned int flags = 0;

	if (v != &group->marks_list) {
		struct list_head *cur = v;
		struct fsnotify_mark *mark = list_entry(cur,
							struct fsnotify_mark,
							g_list);
		return fanotify_fdinfo(m, mark);
	}

	switch (group->priority) {
	case FS_PRIO_0:
		flags |= FAN_CLASS_NOTIF;
		break;
	case FS_PRIO_1:
		flags |= FAN_CLASS_CONTENT;
		break;
	case FS_PRIO_2:
		flags |= FAN_CLASS_PRE_CONTENT;
		break;
	}

	if (group->max_events == UINT_MAX)
		flags |= FAN_UNLIMITED_QUEUE;

	if (group->fanotify_data.max_marks == UINT_MAX)
		flags |= FAN_UNLIMITED_MARKS;

	seq_printf(m, "fanotify flags:%x event-flags:%x\n",
		   flags, group->fanotify_data.f_flags);
}
#endif /* CONFIG_FANOTIFY */

void *fsnotify_next_fdinfo(struct seq_file *seq, struct file *f, void *v,
			   loff_t *pos)
{
	struct fsnotify_group *group = f->private_data;
	return seq_list_next(v, &group->marks_list, pos);
}

void *fsnotify_start_fdinfo(struct seq_file *seq, struct file *f, loff_t *pos)
{
	struct fsnotify_group *group = f->private_data;

	mutex_lock(&group->mark_mutex);
	return seq_list_start_head(&group->marks_list, *pos);
}

void fsnotify_stop_fdinfo(struct seq_file *seq, struct file *f, void *v)
{
	struct fsnotify_group *group = f->private_data;

	mutex_unlock(&group->mark_mutex);
}

#endif /* CONFIG_INOTIFY_USER || CONFIG_FANOTIFY */

#endif /* CONFIG_PROC_FS */
