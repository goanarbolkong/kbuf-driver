// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_main.c - module lifecycle and file operations for the kbuf devices.
 *
 * The module creates `ndevices` independent character devices /dev/kbuf0..N-1,
 * each a fixed-size circular queue of slots. Writers (producers) fill empty
 * slots; readers (consumers) drain full ones. A per-device mutex serialises
 * ring access and two per-device wait queues block callers when no slot is
 * available; O_NONBLOCK returns -EAGAIN instead of sleeping. open() binds the
 * file to its device via container_of on the inode's cdev, stashing it in
 * filp->private_data so every operation acts on the right ring. Ring mechanics
 * live in kbuf_ring.c, the /proc view in kbuf_proc.c, ioctl in kbuf_ioctl.c.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "kbuf_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Lab Student");
MODULE_DESCRIPTION("Kernel multi-buffer producer/consumer driver");
MODULE_VERSION("3.0");

static unsigned int ndevices = KBUF_DEFAULT_NDEVICES;
module_param(ndevices, uint, 0444);
MODULE_PARM_DESC(ndevices, "number of /dev/kbufN devices to create (default 4)");

struct kbuf_dev *kbuf_devices;
unsigned int     kbuf_ndevices;

static struct class *kbuf_class;
static dev_t         kbuf_base_devno;

static char *kbuf_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int kbuf_open(struct inode *inode, struct file *filp)
{
	struct kbuf_dev *dev = container_of(inode->i_cdev, struct kbuf_dev, cdev);

	filp->private_data = dev;
	pr_info("kbuf: %s opened (pid=%d, flags=0x%x)\n",
		dev_name(dev->dev), current->pid, filp->f_flags);
	return 0;
}

static int kbuf_release(struct inode *inode, struct file *filp)
{
	struct kbuf_dev *dev = filp->private_data;

	pr_info("kbuf: %s closed (pid=%d)\n", dev_name(dev->dev), current->pid);
	return 0;
}

/* Blocking mode: mutex-serialised ring with check-sleep-recheck on empty. */
static ssize_t kbuf_read_blocking(struct kbuf_dev *dev, struct file *filp,
				  char __user *ubuf, size_t count)
{
	ssize_t ret;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (kbuf_ring_is_empty(dev)) {
		if (filp->f_flags & O_NONBLOCK) {
			mutex_unlock(&dev->lock);
			return -EAGAIN;
		}
		dev->read_sleeps++;
		mutex_unlock(&dev->lock);
		pr_info("kbuf: reader pid=%d sleeping (buffer empty)\n", current->pid);
		if (wait_event_interruptible(dev->read_wq, !kbuf_ring_is_empty(dev)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
	}

	ret = kbuf_ring_consume(dev, ubuf, count);
	mutex_unlock(&dev->lock);

	if (ret >= 0)
		wake_up_interruptible(&dev->write_wq);	/* a slot is now free */
	return ret;
}

/*
 * SPSC mode: lock-free fast path, with a wait-queue fallback when the ring is
 * empty. No mutex is taken; correctness relies on a single consumer here and a
 * single producer in kbuf_write_spsc().
 */
static ssize_t kbuf_read_spsc(struct kbuf_dev *dev, struct file *filp,
			      char __user *ubuf, size_t count)
{
	ssize_t ret;

	while (kbuf_spsc_is_empty(dev)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		WRITE_ONCE(dev->read_sleeps, dev->read_sleeps + 1);
		if (wait_event_interruptible(dev->read_wq, !kbuf_spsc_is_empty(dev)))
			return -ERESTARTSYS;
	}

	ret = kbuf_spsc_consume(dev, ubuf, count);
	if (ret >= 0)
		wake_up_interruptible(&dev->write_wq);	/* a slot is now free */
	return ret;
}

static ssize_t kbuf_read(struct file *filp, char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct kbuf_dev *dev = filp->private_data;

	if (READ_ONCE(dev->mode) == KBUF_MODE_SPSC)
		return kbuf_read_spsc(dev, filp, ubuf, count);
	return kbuf_read_blocking(dev, filp, ubuf, count);
}

static ssize_t kbuf_write_blocking(struct kbuf_dev *dev, struct file *filp,
				   const char __user *ubuf, size_t count)
{
	ssize_t ret;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	/* Clamp under the lock: buffer_size can change via KBUF_IOCRESIZE. */
	if (count > dev->buffer_size)
		count = dev->buffer_size;

	while (kbuf_ring_is_full(dev)) {
		if (filp->f_flags & O_NONBLOCK) {
			mutex_unlock(&dev->lock);
			return -EAGAIN;
		}
		dev->write_sleeps++;
		mutex_unlock(&dev->lock);
		pr_info("kbuf: writer pid=%d sleeping (buffer full)\n", current->pid);
		if (wait_event_interruptible(dev->write_wq, !kbuf_ring_is_full(dev)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
	}

	ret = kbuf_ring_produce(dev, ubuf, count);
	mutex_unlock(&dev->lock);

	if (ret >= 0)
		wake_up_interruptible(&dev->read_wq);	/* a full slot is ready */
	return ret;
}

static ssize_t kbuf_write_spsc(struct kbuf_dev *dev, struct file *filp,
			       const char __user *ubuf, size_t count)
{
	ssize_t ret;

	/* buffer_size is stable in SPSC mode (resize requires an idle ring). */
	if (count > dev->buffer_size)
		count = dev->buffer_size;

	while (kbuf_spsc_is_full(dev)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		WRITE_ONCE(dev->write_sleeps, dev->write_sleeps + 1);
		if (wait_event_interruptible(dev->write_wq, !kbuf_spsc_is_full(dev)))
			return -ERESTARTSYS;
	}

	ret = kbuf_spsc_produce(dev, ubuf, count);
	if (ret >= 0)
		wake_up_interruptible(&dev->read_wq);	/* a full slot is ready */
	return ret;
}

static ssize_t kbuf_write(struct file *filp, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct kbuf_dev *dev = filp->private_data;

	if (READ_ONCE(dev->mode) == KBUF_MODE_SPSC)
		return kbuf_write_spsc(dev, filp, ubuf, count);
	return kbuf_write_blocking(dev, filp, ubuf, count);
}

/*
 * poll/select/epoll support. We register on both wait queues so the caller is
 * woken whether a slot frees up (writable) or fills (readable); kbuf_read and
 * kbuf_write already wake the matching queue on every state change. The ring is
 * readable while non-empty and writable while non-full. poll_wait() only queues
 * the task; it never sleeps, so taking the mutex here just for a consistent
 * snapshot is safe.
 */
static __poll_t kbuf_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct kbuf_dev *dev = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &dev->read_wq, wait);
	poll_wait(filp, &dev->write_wq, wait);

	if (READ_ONCE(dev->mode) == KBUF_MODE_SPSC) {
		if (!kbuf_spsc_is_empty(dev))
			mask |= EPOLLIN | EPOLLRDNORM;
		if (!kbuf_spsc_is_full(dev))
			mask |= EPOLLOUT | EPOLLWRNORM;
	} else {
		mutex_lock(&dev->lock);
		if (!kbuf_ring_is_empty(dev))
			mask |= EPOLLIN | EPOLLRDNORM;
		if (!kbuf_ring_is_full(dev))
			mask |= EPOLLOUT | EPOLLWRNORM;
		mutex_unlock(&dev->lock);
	}

	return mask;
}

static const struct file_operations kbuf_fops = {
	.owner          = THIS_MODULE,
	.open           = kbuf_open,
	.release        = kbuf_release,
	.read           = kbuf_read,
	.write          = kbuf_write,
	.poll           = kbuf_poll,
	.unlocked_ioctl = kbuf_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* Tear down the first @n fully-created devices (used on exit and unwind). */
static void kbuf_teardown_devices(unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct kbuf_dev *dev = &kbuf_devices[i];

		device_destroy(kbuf_class, dev->devno);
		cdev_del(&dev->cdev);
		kbuf_free_slots(dev->slots, dev->num_buffers);
	}
}

static int __init kbuf_init(void)
{
	unsigned int i;
	int ret;

	if (ndevices < 1 || ndevices > KBUF_MAX_NDEVICES)
		return -EINVAL;
	kbuf_ndevices = ndevices;

	kbuf_devices = kcalloc(kbuf_ndevices, sizeof(*kbuf_devices), GFP_KERNEL);
	if (!kbuf_devices)
		return -ENOMEM;

	ret = alloc_chrdev_region(&kbuf_base_devno, 0, kbuf_ndevices, KBUF_DEVICE_NAME);
	if (ret < 0) {
		pr_err("kbuf: alloc_chrdev_region failed (%d)\n", ret);
		goto err_free;
	}

	kbuf_class = class_create(KBUF_DEVICE_NAME);
	if (IS_ERR(kbuf_class)) {
		ret = PTR_ERR(kbuf_class);
		pr_err("kbuf: class_create failed (%d)\n", ret);
		goto err_region;
	}
	kbuf_class->devnode = kbuf_devnode;

	for (i = 0; i < kbuf_ndevices; i++) {
		struct kbuf_dev *dev = &kbuf_devices[i];

		dev->num_buffers = KBUF_DEFAULT_NUM_BUFFERS;
		dev->buffer_size = KBUF_DEFAULT_BUFFER_SIZE;
		dev->mode = KBUF_MODE_BLOCKING;
		dev->devno = kbuf_base_devno + i;
		mutex_init(&dev->lock);
		init_waitqueue_head(&dev->read_wq);
		init_waitqueue_head(&dev->write_wq);

		dev->slots = kbuf_alloc_slots(dev->num_buffers, dev->buffer_size);
		if (!dev->slots) {
			ret = -ENOMEM;
			goto err_devices;
		}

		cdev_init(&dev->cdev, &kbuf_fops);
		dev->cdev.owner = THIS_MODULE;
		ret = cdev_add(&dev->cdev, dev->devno, 1);
		if (ret < 0) {
			pr_err("kbuf: cdev_add failed for kbuf%u (%d)\n", i, ret);
			kbuf_free_slots(dev->slots, dev->num_buffers);
			goto err_devices;
		}

		dev->dev = device_create(kbuf_class, NULL, dev->devno, NULL,
					 "kbuf%u", i);
		if (IS_ERR(dev->dev)) {
			ret = PTR_ERR(dev->dev);
			pr_err("kbuf: device_create failed for kbuf%u (%d)\n", i, ret);
			cdev_del(&dev->cdev);
			kbuf_free_slots(dev->slots, dev->num_buffers);
			goto err_devices;
		}
	}

	if (kbuf_proc_register())
		pr_warn("kbuf: proc_create failed (non-fatal)\n");

	pr_info("kbuf: loaded - %u devices /dev/kbuf0..%u, /proc/kbuf_status\n",
		kbuf_ndevices, kbuf_ndevices - 1);
	return 0;

err_devices:
	kbuf_teardown_devices(i);	/* devices [0, i) are fully created */
	class_destroy(kbuf_class);
err_region:
	unregister_chrdev_region(kbuf_base_devno, kbuf_ndevices);
err_free:
	kfree(kbuf_devices);
	return ret;
}

static void __exit kbuf_exit(void)
{
	kbuf_proc_unregister();
	kbuf_teardown_devices(kbuf_ndevices);
	class_destroy(kbuf_class);
	unregister_chrdev_region(kbuf_base_devno, kbuf_ndevices);
	kfree(kbuf_devices);
	pr_info("kbuf: unloaded\n");
}

module_init(kbuf_init);
module_exit(kbuf_exit);
