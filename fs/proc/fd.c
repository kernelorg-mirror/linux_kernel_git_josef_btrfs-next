#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/fdtable.h>
#include <linux/namei.h>
#include <linux/pid.h>
#include <linux/security.h>
#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/fs.h>

#include <linux/proc_fs.h>

#include "../mount.h"
#include "internal.h"
#include "fd.h"

enum proc_fdinfo_states {
	FDINFO_GENERIC = 0,
	FDINFO_LOCKS = 1,
	FDINFO_PRIVATE = 2,
	FDINFO_DONE = 3,
};

struct proc_fdinfo_ctx {
	struct file *file;
	struct files_struct *files;
	int f_flags;
	unsigned state;
	loff_t ppos;
};

static int seq_fdinfo_show(struct seq_file *seq, void *v)
{
	struct proc_fdinfo_ctx *ctx = seq->private;
	struct file *file = ctx->file;
	struct files_struct *files = ctx->files;

	switch (ctx->state) {
	case FDINFO_GENERIC:
		seq_printf(seq, "pos:\t%lli\nflags:\t0%o\nmnt_id:\t%i\n",
			   (long long)file->f_pos, ctx->f_flags,
			   real_mount(file->f_path.mnt)->mnt_id);
		return 0;
	case FDINFO_LOCKS:
		show_fd_locks(seq, file, files);
		return 0;
	case FDINFO_PRIVATE:
		if (!file->f_op->show_fdinfo)
			return 1;
		file->f_op->show_fdinfo(seq, file, v);
		return 0;
	default:
		break;
	}
	return 1;
}

static void *seq_fdinfo_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct proc_fdinfo_ctx *ctx = seq->private;
	struct file *file = ctx->file;
	switch (ctx->state) {
	case FDINFO_GENERIC:
	case FDINFO_LOCKS:
		ctx->state++;
		*pos = 0;

		/* We're switching states, we need to make sure the
		 * ->start_fdinfo stuff is run if it exists.
		 */
		if (ctx->state == FDINFO_PRIVATE && file->f_op->start_fdinfo)
			return file->f_op->start_fdinfo(seq, file, pos);
		return pos;
	case FDINFO_PRIVATE:
		if (!file->f_op->next_fdinfo) {
			ctx->state++;
			return NULL;
		}
		return file->f_op->next_fdinfo(seq, ctx->file, v, pos);
	default:
		break;
	}
	return NULL;
}

static void seq_fdinfo_stop(struct seq_file *seq, void *v)
{
	struct proc_fdinfo_ctx *ctx = seq->private;
	struct file *file = ctx->file;

	if (ctx->state == FDINFO_PRIVATE &&
	    file->f_op->stop_fdinfo)
		file->f_op->stop_fdinfo(seq, file, v);
}

static void *seq_fdinfo_start(struct seq_file *seq, loff_t *pos)
{
	struct proc_fdinfo_ctx *ctx = seq->private;
	struct file *file = ctx->file;

	switch (ctx->state) {
	case FDINFO_GENERIC:
	case FDINFO_LOCKS:
		*pos = 0;
		return pos;
	case FDINFO_PRIVATE:
		if (!file->f_op->show_fdinfo) {
			ctx->state = FDINFO_DONE;
			return NULL;
		}
		if (file->f_op->start_fdinfo)
			return file->f_op->start_fdinfo(seq, file, pos);
		return pos;
	default:
		break;
	}
	return NULL;
}

const struct seq_operations proc_fdinfo_seq_operations = {
	.start	=	seq_fdinfo_start,
	.stop	=	seq_fdinfo_stop,
	.next	=	seq_fdinfo_next,
	.show	=	seq_fdinfo_show,
};

static int seq_fdinfo_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct proc_fdinfo_ctx *ctx = seq->private;

	fput(ctx->file);
	return seq_release_private(inode, file);
}

static int seq_fdinfo_open(struct inode *inode, struct file *file)
{
	struct files_struct *files = NULL;
	int f_flags = 0, ret = -ENOENT;
	struct file *target_file = NULL;
	struct task_struct *task;
	struct seq_file *seq;
	struct proc_fdinfo_ctx *ctx;

	task = get_proc_task(inode);
	if (!task)
		return -ENOENT;

	files = get_files_struct(task);
	put_task_struct(task);

	if (files) {
		int fd = proc_fd(inode);

		spin_lock(&files->file_lock);
		target_file = fcheck_files(files, fd);
		if (target_file) {
			struct fdtable *fdt = files_fdtable(files);

			f_flags = target_file->f_flags;
			if (close_on_exec(fd, fdt))
				f_flags |= O_CLOEXEC;

			get_file(target_file);
			ret = 0;
		}
		spin_unlock(&files->file_lock);
		put_files_struct(files);
	}
	if (ret)
		return ret;

	ret = seq_open_private(file, &proc_fdinfo_seq_operations,
			       sizeof(*ctx));
	if (ret) {
		fput(target_file);
		return ret;
	}
	seq = file->private_data;
	ctx = seq->private;
	ctx->file = target_file;
	ctx->files = files;
	ctx->f_flags = f_flags;
	ctx->state = FDINFO_GENERIC;
	return 0;
}

static const struct file_operations proc_fdinfo_file_operations = {
	.open		= seq_fdinfo_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_fdinfo_release,
};

static int tid_fd_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct files_struct *files;
	struct task_struct *task;
	const struct cred *cred;
	struct inode *inode;
	int fd;

	if (flags & LOOKUP_RCU)
		return -ECHILD;

	inode = d_inode(dentry);
	task = get_proc_task(inode);
	fd = proc_fd(inode);

	if (task) {
		files = get_files_struct(task);
		if (files) {
			struct file *file;

			rcu_read_lock();
			file = fcheck_files(files, fd);
			if (file) {
				unsigned f_mode = file->f_mode;

				rcu_read_unlock();
				put_files_struct(files);

				if (task_dumpable(task)) {
					rcu_read_lock();
					cred = __task_cred(task);
					inode->i_uid = cred->euid;
					inode->i_gid = cred->egid;
					rcu_read_unlock();
				} else {
					inode->i_uid = GLOBAL_ROOT_UID;
					inode->i_gid = GLOBAL_ROOT_GID;
				}

				if (S_ISLNK(inode->i_mode)) {
					unsigned i_mode = S_IFLNK;
					if (f_mode & FMODE_READ)
						i_mode |= S_IRUSR | S_IXUSR;
					if (f_mode & FMODE_WRITE)
						i_mode |= S_IWUSR | S_IXUSR;
					inode->i_mode = i_mode;
				}

				security_task_to_inode(task, inode);
				put_task_struct(task);
				return 1;
			}
			rcu_read_unlock();
			put_files_struct(files);
		}
		put_task_struct(task);
	}
	return 0;
}

static const struct dentry_operations tid_fd_dentry_operations = {
	.d_revalidate	= tid_fd_revalidate,
	.d_delete	= pid_delete_dentry,
};

static int proc_fd_link(struct dentry *dentry, struct path *path)
{
	struct files_struct *files = NULL;
	struct task_struct *task;
	int ret = -ENOENT;

	task = get_proc_task(d_inode(dentry));
	if (task) {
		files = get_files_struct(task);
		put_task_struct(task);
	}

	if (files) {
		int fd = proc_fd(d_inode(dentry));
		struct file *fd_file;

		spin_lock(&files->file_lock);
		fd_file = fcheck_files(files, fd);
		if (fd_file) {
			*path = fd_file->f_path;
			path_get(&fd_file->f_path);
			ret = 0;
		}
		spin_unlock(&files->file_lock);
		put_files_struct(files);
	}

	return ret;
}

static int
proc_fd_instantiate(struct inode *dir, struct dentry *dentry,
		    struct task_struct *task, const void *ptr)
{
	unsigned fd = (unsigned long)ptr;
	struct proc_inode *ei;
	struct inode *inode;

	inode = proc_pid_make_inode(dir->i_sb, task);
	if (!inode)
		goto out;

	ei = PROC_I(inode);
	ei->fd = fd;

	inode->i_mode = S_IFLNK;
	inode->i_op = &proc_pid_link_inode_operations;
	inode->i_size = 64;

	ei->op.proc_get_link = proc_fd_link;

	d_set_d_op(dentry, &tid_fd_dentry_operations);
	d_add(dentry, inode);

	/* Close the race of the process dying before we return the dentry */
	if (tid_fd_revalidate(dentry, 0))
		return 0;
 out:
	return -ENOENT;
}

static struct dentry *proc_lookupfd_common(struct inode *dir,
					   struct dentry *dentry,
					   instantiate_t instantiate)
{
	struct task_struct *task = get_proc_task(dir);
	int result = -ENOENT;
	unsigned fd = name_to_int(&dentry->d_name);

	if (!task)
		goto out_no_task;
	if (fd == ~0U)
		goto out;

	result = instantiate(dir, dentry, task, (void *)(unsigned long)fd);
out:
	put_task_struct(task);
out_no_task:
	return ERR_PTR(result);
}

static int proc_readfd_common(struct file *file, struct dir_context *ctx,
			      instantiate_t instantiate)
{
	struct task_struct *p = get_proc_task(file_inode(file));
	struct files_struct *files;
	unsigned int fd;

	if (!p)
		return -ENOENT;

	if (!dir_emit_dots(file, ctx))
		goto out;
	files = get_files_struct(p);
	if (!files)
		goto out;

	rcu_read_lock();
	for (fd = ctx->pos - 2;
	     fd < files_fdtable(files)->max_fds;
	     fd++, ctx->pos++) {
		char name[PROC_NUMBUF];
		int len;

		if (!fcheck_files(files, fd))
			continue;
		rcu_read_unlock();

		len = snprintf(name, sizeof(name), "%d", fd);
		if (!proc_fill_cache(file, ctx,
				     name, len, instantiate, p,
				     (void *)(unsigned long)fd))
			goto out_fd_loop;
		cond_resched();
		rcu_read_lock();
	}
	rcu_read_unlock();
out_fd_loop:
	put_files_struct(files);
out:
	put_task_struct(p);
	return 0;
}

static int proc_readfd(struct file *file, struct dir_context *ctx)
{
	return proc_readfd_common(file, ctx, proc_fd_instantiate);
}

const struct file_operations proc_fd_operations = {
	.read		= generic_read_dir,
	.iterate	= proc_readfd,
	.llseek		= default_llseek,
};

static struct dentry *proc_lookupfd(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	return proc_lookupfd_common(dir, dentry, proc_fd_instantiate);
}

/*
 * /proc/pid/fd needs a special permission handler so that a process can still
 * access /proc/self/fd after it has executed a setuid().
 */
int proc_fd_permission(struct inode *inode, int mask)
{
	struct task_struct *p;
	int rv;

	rv = generic_permission(inode, mask);
	if (rv == 0)
		return rv;

	rcu_read_lock();
	p = pid_task(proc_pid(inode), PIDTYPE_PID);
	if (p && same_thread_group(p, current))
		rv = 0;
	rcu_read_unlock();

	return rv;
}

const struct inode_operations proc_fd_inode_operations = {
	.lookup		= proc_lookupfd,
	.permission	= proc_fd_permission,
	.setattr	= proc_setattr,
};

static int
proc_fdinfo_instantiate(struct inode *dir, struct dentry *dentry,
			struct task_struct *task, const void *ptr)
{
	unsigned fd = (unsigned long)ptr;
	struct proc_inode *ei;
	struct inode *inode;

	inode = proc_pid_make_inode(dir->i_sb, task);
	if (!inode)
		goto out;

	ei = PROC_I(inode);
	ei->fd = fd;

	inode->i_mode = S_IFREG | S_IRUSR;
	inode->i_fop = &proc_fdinfo_file_operations;

	d_set_d_op(dentry, &tid_fd_dentry_operations);
	d_add(dentry, inode);

	/* Close the race of the process dying before we return the dentry */
	if (tid_fd_revalidate(dentry, 0))
		return 0;
 out:
	return -ENOENT;
}

static struct dentry *
proc_lookupfdinfo(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	return proc_lookupfd_common(dir, dentry, proc_fdinfo_instantiate);
}

static int proc_readfdinfo(struct file *file, struct dir_context *ctx)
{
	return proc_readfd_common(file, ctx,
				  proc_fdinfo_instantiate);
}

const struct inode_operations proc_fdinfo_inode_operations = {
	.lookup		= proc_lookupfdinfo,
	.setattr	= proc_setattr,
};

const struct file_operations proc_fdinfo_operations = {
	.read		= generic_read_dir,
	.iterate	= proc_readfdinfo,
	.llseek		= default_llseek,
};
