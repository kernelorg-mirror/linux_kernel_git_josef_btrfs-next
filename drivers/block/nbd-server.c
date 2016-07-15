#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>
#include <net/sock.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/nbd.h>
#include <linux/blk_types.h>
#include <linux/file.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>

#include <asm/uaccess.h>

#define NBD_SERVER_FLAG_READONLY	(1 << 0)

struct nbd_conn {
	u64 flags;
	fmode_t mode;
	struct socket *sock;
	struct block_device *bdev;
	struct mutex tx_lock;
	struct list_head list;
	struct work_struct work;
	struct kref ref;
};

struct nbd_command {
	struct nbd_conn *conn;
	struct bio *bio;
	struct nbd_request req;
	struct work_struct work;
};

/*
 * This protects the connections and exports.  It's pretty heavy, but we only
 * need to take it when making changes and when accepting new connections, so it
 * shouldn't be hit too often.  It can be broken up in the future if it causes
 * problems.
 */
static DEFINE_MUTEX(config_mutex);
static LIST_HEAD(conn_list);

static void conn_free(struct kref *ref)
{
	struct nbd_conn *conn = container_of(ref, struct nbd_conn, ref);

	blkdev_put(conn->bdev, conn->mode);
	kfree(conn);
}

static int sock_xmit(struct socket *sock, int send, void *buf, int size,
		     int msg_flags)
{
	int result;
	struct msghdr msg;
	struct kvec iov;
	unsigned long pflags = current->flags;

	current->flags |= PF_MEMALLOC;
	do {
		sock->sk->sk_allocation = GFP_NOIO | __GFP_MEMALLOC;
		iov.iov_base = buf;
		iov.iov_len = size;
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = msg_flags | MSG_NOSIGNAL;

		if (send)
			result = kernel_sendmsg(sock, &msg, &iov, 1, size);
		else
			result = kernel_recvmsg(sock, &msg, &iov, 1, size,
						msg.msg_flags);

		if (result <= 0) {
			if (result == 0)
				result = -EPIPE; /* short read */
			break;
		}
		size -= result;
		buf += result;
	} while (size > 0);

	tsk_restore_flags(current, pflags, PF_MEMALLOC);

	return result;
}

static void nbds_response(struct work_struct *work)
{
	struct nbd_command *cmd = container_of(work, struct nbd_command,
						 work);
	struct nbd_conn *conn = cmd->conn;
	struct bio *bio = cmd->bio;
	struct nbd_reply reply;
	struct bio_vec *bv;
	u32 len = ntohl(cmd->req.len);
	bool read = (ntohl(cmd->req.type) & NBD_CMD_MASK_COMMAND) ==
		NBD_CMD_READ;
	int ret, i;

	reply.magic = htonl(NBD_REPLY_MAGIC);
	reply.error = htonl(cmd->bio->bi_error);
	memcpy(reply.handle, cmd->req.handle, sizeof(reply.handle));

	mutex_lock(&conn->tx_lock);
	ret = sock_xmit(conn->sock, 1, &reply, sizeof(reply),
			read ? MSG_MORE : 0);
	if (ret) {
		printk(KERN_ERR "nbd-server: failed to send reply msg %d\n",
		       ret);
		goto out;
	}
	if (!read || cmd->bio->bi_error)
		goto out;

	bio_for_each_segment_all(bv, bio, i) {
		ret = kernel_sendpage(conn->sock, bv->bv_page, bv->bv_offset,
				      bv->bv_len,
				      len <= PAGE_SIZE ? 0 : MSG_MORE);
		if (ret) {
			printk(KERN_ERR "nbd-server: failed to send page %d\n",
			       ret);
			goto out;
		}
		len -= bv->bv_len;
		__free_page(bv->bv_page);
	}
out:
	mutex_unlock(&conn->tx_lock);
	kref_put(&conn->ref, conn_free);
	kfree(cmd);
	bio_put(bio);
}

static void nbds_endio(struct bio *bio)
{
	struct nbd_command *cmd = bio->bi_private;

	queue_work(system_wq, &cmd->work);
}

static int nbds_handle_io(struct nbd_command *cmd, bool write)
{
	struct nbd_conn *conn = cmd->conn;
	struct bio *bio;
	struct bio_vec *bv;
	int num_pages, i, ret = 0;
	u32 len = ntohl(cmd->req.len);

	INIT_WORK(&cmd->work, nbds_response);
	num_pages = (len >> PAGE_SHIFT) + (len & PAGE_MASK);
	if (num_pages > BIO_MAX_PAGES) {
		printk(KERN_ERR "nbd-server: read request too large %u\n",
		       len);
		return -E2BIG;
	}

	bio = bio_alloc(GFP_KERNEL, num_pages);
	if (!bio) {
		printk(KERN_ERR "nbd-server: couldn't allocate bio\n");
		return -ENOMEM;
	}

	bio->bi_bdev = conn->bdev;
	bio->bi_iter.bi_sector = ntohl(cmd->req.from) >> 9;
	bio->bi_end_io = nbds_endio;
	bio->bi_vcnt = num_pages;
	bio->bi_iter.bi_size = len;
	bio->bi_private = conn;

	bio_for_each_segment_all(bv, bio, i) {
		bv->bv_page = alloc_page(GFP_KERNEL);
		if (!bv->bv_page) {
			ret = -ENOMEM;
			goto out_free;
		}

		bv->bv_len = (len >= PAGE_SIZE) ? PAGE_SIZE : len;

		/*
		 * Writes have the req proceeded by the bytes we are writing, so
		 * we need to consume those here and add them to the pages in
		 * the bio.
		 */
		if (write) {
			int ret;
			void *kaddr = kmap(bv->bv_page);

			ret = sock_xmit(conn->sock, 0, kaddr, bv->bv_len,
					MSG_WAITALL);
			kunmap(bv->bv_page);
			if (ret)
				goto out_free;
		}
		len -= PAGE_SIZE;
	}

	cmd->bio = bio;
	kref_get(&conn->ref);
	if (write) {
		/*
		 * If we are readonly we need to return EPERM as per the
		 * userspace nbd server.
		 */
		if (!(conn->mode & FMODE_WRITE)) {
			bio->bi_error = EPERM;
			bio_endio(bio);
		} else {
			submit_bio(WRITE, bio);
		}
	} else {
		submit_bio(READ, bio);
	}
	return 0;
out_free:
	while (--bv >= bio->bi_io_vec)
		__free_page(bv->bv_page);
	bio_put(bio);
	return ret;
}

static void nbds_handle_command(struct work_struct *work)
{
	struct nbd_command *cmd = container_of(work, struct nbd_command, work);
	struct nbd_conn *conn = cmd->conn;
	struct nbd_reply reply;
	int type = ntohl(cmd->req.type) & NBD_CMD_MASK_COMMAND;
	int ret;

	if (type == NBD_CMD_FLUSH) {
		ret = blkdev_issue_flush(conn->bdev, GFP_KERNEL, NULL);
	} else {
		sector_t start = ntohl(cmd->req.from) >> 9;
		sector_t nr_sectors = ntohl(cmd->req.len) >> 9;

		ret = blkdev_issue_discard(conn->bdev, start, nr_sectors,
					   GFP_KERNEL, 0);
	}

	reply.magic = htonl(NBD_REPLY_MAGIC);
	reply.error = htonl(ret);
	memcpy(reply.handle, cmd->req.handle, sizeof(reply.handle));

	mutex_lock(&conn->tx_lock);
	ret = sock_xmit(conn->sock, 1, &reply, sizeof(reply), 0);
	mutex_unlock(&conn->tx_lock);

	if (ret)
		printk(KERN_ERR "nbd-server: failed to send reply msg %d\n",
		       ret);
	kref_put(&conn->ref, conn_free);
	kfree(cmd);
}

static void nbds_recv(struct work_struct *work)
{
	struct nbd_conn *conn = container_of(work, struct nbd_conn, work);
	struct nbd_command *cmd;
	int ret, type;
	bool done = false;

	while (!done) {
		cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
		if (!cmd) {
			printk(KERN_ERR "nbd-server: Couldn't allocate a command\n");
			break;
		}
		cmd->conn = conn;

		ret = sock_xmit(conn->sock, 0, &cmd->req, sizeof(cmd->req),
				MSG_WAITALL);
		if (ret <= 0) {
			printk(KERN_ERR "nbd-server: Receive request failed %d\n",
			       ret);
			kfree(cmd);
			break;
		}

		if (ntohl(cmd->req.magic) != NBD_REQUEST_MAGIC) {
			printk(KERN_ERR "nbd-server: Invalid magic\n");
			kfree(cmd);
			break;
		}

		ret = 0;
		type = ntohl(cmd->req.type) & NBD_CMD_MASK_COMMAND;
		switch (type) {
		case NBD_CMD_READ:
			ret = nbds_handle_io(cmd, false);
			break;
		case NBD_CMD_WRITE:
			ret = nbds_handle_io(cmd, true);
			break;
		case NBD_CMD_DISC:
			done = true;
			kfree(cmd);
			break;
		case NBD_CMD_FLUSH:
		case NBD_CMD_TRIM:
			kref_get(&conn->ref);
			INIT_WORK(&cmd->work, nbds_handle_command);
			queue_work(system_wq, &cmd->work);
			break;
		default:
			kfree(cmd);
			printk(KERN_ERR "nbd-server: Invalid command %d\n",
			       type);
			done = true;
			break;
		}
		if (ret)
			done = true;
	}

	mutex_lock(&config_mutex);
	kernel_sock_shutdown(conn->sock, SHUT_RDWR);
	sockfd_put(conn->sock);
	list_del_init(&conn->list);
	mutex_unlock(&config_mutex);
	kref_put(&conn->ref, conn_free);
}

static int nbds_add_conn(void __user *arg)
{
	struct nbd_conn *conn = NULL;
	struct nbd_server_args *args;
	fmode_t mode = FMODE_READ;
	int ret = -ENOMEM;

	args = memdup_user(arg, sizeof(*args));
	if (!args)
		return -ENOMEM;

	args->devname[NBD_DEVNAME_MAX - 1] = '\0';

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn)
		goto out_err;

	if (!(args->flags & NBD_FLAG_READ_ONLY))
		mode |= FMODE_WRITE;

	conn->bdev = blkdev_get_by_path(args->devname, mode, NULL);
	if (!conn->bdev) {
		printk(KERN_ERR "nbd-server: couldn't open %s\n",
		       args->devname);
		ret = -EINVAL;
		goto out_err;
	}

	conn->sock = sockfd_lookup(args->sockfd, &ret);
	if (!conn->sock) {
		printk(KERN_ERR "nbd-server: invalid socket %d\n", ret);
		goto out_bdev;
	}

	kref_init(&conn->ref);
	conn->flags = args->flags;
	conn->mode = mode;
	INIT_LIST_HEAD(&conn->list);
	INIT_WORK(&conn->work, nbds_recv);
	mutex_init(&conn->tx_lock);

	mutex_lock(&config_mutex);
	list_add_tail(&conn->list, &conn_list);
	mutex_unlock(&config_mutex);

	queue_work(system_long_wq, &conn->work);

	return 0;
out_bdev:
	blkdev_put(conn->bdev, mode);
out_err:
	kfree(args);
	kfree(conn);
	return ret;
}

static void nbds_disconnect_all(void)
{
	struct nbd_conn *conn;

	/*
	 * We just have to disconnect the sockets here, the recv threads will
	 * error out and clean themselves up.
	 */
	mutex_lock(&config_mutex);
	list_for_each_entry(conn, &conn_list, list)
		kernel_sock_shutdown(conn->sock, SHUT_RDWR);
	mutex_unlock(&config_mutex);
}

static long nbds_control_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (cmd) {
	case NBD_SERVER_CTL_ADD:
		return nbds_add_conn(argp);
	case NBD_SERVER_CTL_DISCONNECT_ALL:
		nbds_disconnect_all();
		return 0;
	default:
		break;
	}
	printk(KERN_ERR "Invalid nbd server ioctl %u\n", cmd);
	return -EINVAL;
}

static const struct file_operations nbds_ctl_fops = {
	.open		= nonseekable_open,
	.unlocked_ioctl	= nbds_control_ioctl,
	.compat_ioctl	= nbds_control_ioctl,
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
};

static struct miscdevice nbds_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "nbd-server-control",
	.fops	= &nbds_ctl_fops,
};

MODULE_ALIAS("devname:nbd-server-contorl");

static int __init nbds_init(void)
{
	return misc_register(&nbds_misc);
}

static void __exit nbds_exit(void)
{
	misc_deregister(&nbds_misc);
}

module_init(nbds_init);
module_exit(nbds_exit);

MODULE_DESCRIPTION("Network Block Device Server");
MODULE_LICENSE("GPL");
