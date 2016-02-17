#ifndef __FSNOTIFY_FDINFO_H__
#define __FSNOTIFY_FDINFO_H__

#include <linux/errno.h>
#include <linux/proc_fs.h>

struct seq_file;
struct file;

#ifdef CONFIG_PROC_FS

#ifdef CONFIG_INOTIFY_USER
void inotify_show_fdinfo(struct seq_file *m, struct file *f, void *v);
#endif

#ifdef CONFIG_FANOTIFY
void fanotify_show_fdinfo(struct seq_file *m, struct file *f, void *v);
#endif

void *fsnotify_next_fdinfo(struct seq_file *seq, struct file *f, void *v,
			   loff_t *pos);
void *fsnotify_start_fdinfo(struct seq_file *seq, struct file *f, loff_t *pos);
void fsnotify_stop_fdinfo(struct seq_file *seq, struct file *f, void *v);

#else /* CONFIG_PROC_FS */

#define inotify_show_fdinfo	NULL
#define fanotify_show_fdinfo	NULL
#define fsnotify_start_fdinfo	NULL
#define fsnotify_stop_fdinfo	NULL
#define fsnotify_next_fdinfo	NULL

#endif /* CONFIG_PROC_FS */

#endif /* __FSNOTIFY_FDINFO_H__ */
