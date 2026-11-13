// SPDX-License-Identifier: GPL-2.0
/*
 * msgring - a bounded, message-oriented queue exposed as a character device.
 *
 * Each write() enqueues exactly one message and each read() dequeues exactly
 * one, so message boundaries are preserved (datagram semantics). Any number of
 * readers and writers, in any number of processes, may share a device.
 *
 * Locking: one mutex per device protects the queue and the counters. Sleeping
 * happens on two wait queues (readers wait for "not empty", writers wait for
 * "not full") and always with the mutex dropped.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "msgring.h"

#define MSGRING_MAX_DEVS	8

static unsigned int ndevs = 1;
module_param(ndevs, uint, 0444);
MODULE_PARM_DESC(ndevs, "Number of /dev/msgringN devices to create (1-8)");

static unsigned int capacity = MSGRING_DEFAULT_CAPACITY;
module_param(capacity, uint, 0444);
MODULE_PARM_DESC(capacity, "Initial queue capacity in messages");

struct msgring_msg {
	struct list_head node;
	size_t len;
	u8 data[] __counted_by(len);
};

struct msgring_dev {
	struct miscdevice misc;
	char name[16];

	struct mutex lock;		/* protects everything below */
	struct list_head queue;
	u32 depth;
	u32 capacity;
	u32 high_watermark;
	u64 enqueued;
	u64 dequeued;
	u64 flushed;
	u64 bytes_in;
	u64 bytes_out;

	wait_queue_head_t read_wq;	/* woken when the queue gains a message */
	wait_queue_head_t write_wq;	/* woken when the queue gains free space */
};

/* Per-open state: lets two openers of one device use different timeouts. */
struct msgring_file {
	struct msgring_dev *dev;
	long timeout;			/* jiffies, MAX_SCHEDULE_TIMEOUT = forever */
};

static struct msgring_dev *msgring_devs;
static struct dentry *msgring_dbg_root;

static int msgring_open(struct inode *inode, struct file *file)
{
	/* misc_open() stored our miscdevice in private_data. */
	struct miscdevice *misc = file->private_data;
	struct msgring_file *mf;

	mf = kzalloc(sizeof(*mf), GFP_KERNEL);
	if (!mf)
		return -ENOMEM;

	mf->dev = container_of(misc, struct msgring_dev, misc);
	mf->timeout = MAX_SCHEDULE_TIMEOUT;
	file->private_data = mf;

	return stream_open(inode, file);
}

static int msgring_release(struct inode *inode, struct file *file)
{
	/* Queued messages belong to the device, not the opener: keep them. */
	kfree(file->private_data);
	return 0;
}

static ssize_t msgring_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct msgring_file *mf = file->private_data;
	struct msgring_dev *dev = mf->dev;
	long timeout = READ_ONCE(mf->timeout);
	struct msgring_msg *msg;
	ssize_t ret;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (list_empty(&dev->queue)) {
		mutex_unlock(&dev->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/*
		 * The condition is re-checked under the lock after waking, so
		 * a racing reader that steals the message just sends us back
		 * to sleep for the remaining time.
		 */
		timeout = wait_event_interruptible_timeout(dev->read_wq,
					READ_ONCE(dev->depth) > 0, timeout);
		if (timeout < 0)
			return timeout;
		if (timeout == 0)
			return -ETIMEDOUT;

		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
	}

	msg = list_first_entry(&dev->queue, struct msgring_msg, node);

	/* Never truncate: the message stays queued for a larger buffer. */
	if (count < msg->len) {
		ret = -EMSGSIZE;
		goto out_unlock;
	}

	/* Copy before unlinking so a bad pointer does not lose the message. */
	if (copy_to_user(buf, msg->data, msg->len)) {
		ret = -EFAULT;
		goto out_unlock;
	}

	list_del(&msg->node);
	WRITE_ONCE(dev->depth, dev->depth - 1);
	dev->dequeued++;
	dev->bytes_out += msg->len;
	ret = msg->len;
	mutex_unlock(&dev->lock);

	kfree(msg);
	wake_up_interruptible(&dev->write_wq);
	return ret;

out_unlock:
	mutex_unlock(&dev->lock);
	return ret;
}

static ssize_t msgring_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct msgring_file *mf = file->private_data;
	struct msgring_dev *dev = mf->dev;
	long timeout = READ_ONCE(mf->timeout);
	struct msgring_msg *msg;
	ssize_t ret;

	if (count == 0)
		return -EINVAL;
	if (count > MSGRING_MAX_MSG_SIZE)
		return -EMSGSIZE;

	/* Allocate and copy outside the lock: both may sleep or fault. */
	msg = kmalloc(struct_size(msg, data, count), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	msg->len = count;

	if (copy_from_user(msg->data, buf, count)) {
		ret = -EFAULT;
		goto err_free;
	}

	if (mutex_lock_interruptible(&dev->lock)) {
		ret = -ERESTARTSYS;
		goto err_free;
	}

	while (dev->depth >= dev->capacity) {
		mutex_unlock(&dev->lock);

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto err_free;
		}

		timeout = wait_event_interruptible_timeout(dev->write_wq,
			READ_ONCE(dev->depth) < READ_ONCE(dev->capacity),
			timeout);
		if (timeout < 0) {
			ret = timeout;
			goto err_free;
		}
		if (timeout == 0) {
			ret = -ETIMEDOUT;
			goto err_free;
		}

		if (mutex_lock_interruptible(&dev->lock)) {
			ret = -ERESTARTSYS;
			goto err_free;
		}
	}

	list_add_tail(&msg->node, &dev->queue);
	WRITE_ONCE(dev->depth, dev->depth + 1);
	if (dev->depth > dev->high_watermark)
		dev->high_watermark = dev->depth;
	dev->enqueued++;
	dev->bytes_in += count;
	mutex_unlock(&dev->lock);

	wake_up_interruptible(&dev->read_wq);
	return count;

err_free:
	kfree(msg);
	return ret;
}

static __poll_t msgring_poll(struct file *file, poll_table *wait)
{
	struct msgring_file *mf = file->private_data;
	struct msgring_dev *dev = mf->dev;
	__poll_t mask = 0;

	poll_wait(file, &dev->read_wq, wait);
	poll_wait(file, &dev->write_wq, wait);

	mutex_lock(&dev->lock);
	if (dev->depth > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (dev->depth < dev->capacity)
		mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&dev->lock);

	return mask;
}

/* Detach every queued message under the lock, free them after dropping it. */
static u32 msgring_flush(struct msgring_dev *dev)
{
	struct msgring_msg *msg, *tmp;
	LIST_HEAD(doomed);
	u32 n;

	mutex_lock(&dev->lock);
	list_splice_init(&dev->queue, &doomed);
	n = dev->depth;
	WRITE_ONCE(dev->depth, 0);
	dev->flushed += n;
	mutex_unlock(&dev->lock);

	list_for_each_entry_safe(msg, tmp, &doomed, node)
		kfree(msg);

	if (n)
		wake_up_interruptible(&dev->write_wq);
	return n;
}

static void msgring_fill_stats(struct msgring_dev *dev,
			       struct msgring_stats *st)
{
	memset(st, 0, sizeof(*st));	/* no kernel stack bytes leak out */

	mutex_lock(&dev->lock);
	st->enqueued = dev->enqueued;
	st->dequeued = dev->dequeued;
	st->flushed = dev->flushed;
	st->bytes_in = dev->bytes_in;
	st->bytes_out = dev->bytes_out;
	st->depth = dev->depth;
	st->capacity = dev->capacity;
	st->high_watermark = dev->high_watermark;
	mutex_unlock(&dev->lock);
}

static long msgring_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct msgring_file *mf = file->private_data;
	struct msgring_dev *dev = mf->dev;
	void __user *argp = (void __user *)arg;
	struct msgring_stats st;
	u32 val;
	int ret = 0;

	switch (cmd) {
	case MSGRING_IOC_GET_STATS:
		msgring_fill_stats(dev, &st);
		if (copy_to_user(argp, &st, sizeof(st)))
			return -EFAULT;
		return 0;

	case MSGRING_IOC_SET_CAPACITY:
		if (get_user(val, (u32 __user *)argp))
			return -EFAULT;
		if (val == 0 || val > MSGRING_MAX_CAPACITY)
			return -EINVAL;

		mutex_lock(&dev->lock);
		if (val < dev->depth)
			ret = -EBUSY;
		else
			WRITE_ONCE(dev->capacity, val);
		mutex_unlock(&dev->lock);

		if (!ret)
			wake_up_interruptible(&dev->write_wq);
		return ret;

	case MSGRING_IOC_FLUSH:
		msgring_flush(dev);
		return 0;

	case MSGRING_IOC_SET_TIMEOUT:
		if (get_user(val, (u32 __user *)argp))
			return -EFAULT;
		WRITE_ONCE(mf->timeout,
			   val ? msecs_to_jiffies(val) : MAX_SCHEDULE_TIMEOUT);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations msgring_fops = {
	.owner		= THIS_MODULE,
	.open		= msgring_open,
	.release	= msgring_release,
	.read		= msgring_read,
	.write		= msgring_write,
	.poll		= msgring_poll,
	.unlocked_ioctl	= msgring_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int msgring_stats_show(struct seq_file *s, void *unused)
{
	struct msgring_dev *dev = s->private;
	struct msgring_stats st;

	msgring_fill_stats(dev, &st);
	seq_printf(s, "depth:          %u\n", st.depth);
	seq_printf(s, "capacity:       %u\n", st.capacity);
	seq_printf(s, "high_watermark: %u\n", st.high_watermark);
	seq_printf(s, "enqueued:       %llu\n", st.enqueued);
	seq_printf(s, "dequeued:       %llu\n", st.dequeued);
	seq_printf(s, "flushed:        %llu\n", st.flushed);
	seq_printf(s, "bytes_in:       %llu\n", st.bytes_in);
	seq_printf(s, "bytes_out:      %llu\n", st.bytes_out);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(msgring_stats);

static int msgring_dev_init(struct msgring_dev *dev, unsigned int idx)
{
	int ret;

	mutex_init(&dev->lock);
	INIT_LIST_HEAD(&dev->queue);
	init_waitqueue_head(&dev->read_wq);
	init_waitqueue_head(&dev->write_wq);
	dev->capacity = capacity;

	snprintf(dev->name, sizeof(dev->name), "msgring%u", idx);
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = dev->name;
	dev->misc.fops = &msgring_fops;
	dev->misc.mode = 0666;

	ret = misc_register(&dev->misc);
	if (ret)
		return ret;

	/* debugfs is best effort by design: its errors are not checked. */
	debugfs_create_file(dev->name, 0444, msgring_dbg_root, dev,
			    &msgring_stats_fops);
	return 0;
}

static void msgring_dev_destroy(struct msgring_dev *dev)
{
	misc_deregister(&dev->misc);
	msgring_flush(dev);
}

static int __init msgring_init(void)
{
	unsigned int i;
	int ret;

	if (ndevs == 0 || ndevs > MSGRING_MAX_DEVS) {
		pr_err("ndevs must be between 1 and %d\n", MSGRING_MAX_DEVS);
		return -EINVAL;
	}
	if (capacity == 0 || capacity > MSGRING_MAX_CAPACITY) {
		pr_err("capacity must be between 1 and %d\n",
		       MSGRING_MAX_CAPACITY);
		return -EINVAL;
	}

	msgring_devs = kcalloc(ndevs, sizeof(*msgring_devs), GFP_KERNEL);
	if (!msgring_devs)
		return -ENOMEM;

	msgring_dbg_root = debugfs_create_dir(KBUILD_MODNAME, NULL);

	for (i = 0; i < ndevs; i++) {
		ret = msgring_dev_init(&msgring_devs[i], i);
		if (ret)
			goto err_unwind;
	}

	pr_info("loaded, %u device(s), capacity %u\n", ndevs, capacity);
	return 0;

err_unwind:
	while (i--)
		msgring_dev_destroy(&msgring_devs[i]);
	debugfs_remove_recursive(msgring_dbg_root);
	kfree(msgring_devs);
	return ret;
}

static void __exit msgring_exit(void)
{
	unsigned int i;

	/*
	 * The module refcount (fops.owner) guarantees nobody has a device
	 * open here, so no reader or writer can still be sleeping on us.
	 */
	debugfs_remove_recursive(msgring_dbg_root);
	for (i = 0; i < ndevs; i++)
		msgring_dev_destroy(&msgring_devs[i]);
	kfree(msgring_devs);
	pr_info("unloaded\n");
}

module_init(msgring_init);
module_exit(msgring_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sahana Patte Keshava");
MODULE_DESCRIPTION("Bounded message queue character device");
