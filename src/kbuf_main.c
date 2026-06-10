// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_main.c - module lifecycle and file operations for the kbuf device.
 *
 * A fixed-size circular queue of slots. Writers (producers) fill empty slots;
 * readers (consumers) drain full ones. A mutex serialises ring access; two
 * wait queues block callers when no slot is available. O_NONBLOCK returns
 * -EAGAIN instead of sleeping. Ring mechanics live in kbuf_ring.c, the /proc
 * view in kbuf_proc.c, and ioctl dispatch in kbuf_ioctl.c.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "kbuf_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Lab Student");
MODULE_DESCRIPTION("Kernel multi-buffer producer/consumer driver");
MODULE_VERSION("2.0");

struct kbuf_dev kbuf;

static char *kbuf_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int kbuf_open(struct inode *inode, struct file *filp)
{
	pr_info("kbuf: opened (pid=%d, flags=0x%x)\n", current->pid, filp->f_flags);
	return 0;
}

static int kbuf_release(struct inode *inode, struct file *filp)
{
	pr_info("kbuf: closed (pid=%d)\n", current->pid);
	return 0;
}

static ssize_t kbuf_read(struct file *filp, char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	ssize_t ret;

	if (mutex_lock_interruptible(&kbuf.lock))
		return -ERESTARTSYS;

	while (kbuf_ring_is_empty(&kbuf)) {
		mutex_unlock(&kbuf.lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		pr_info("kbuf: reader pid=%d sleeping (buffer empty)\n", current->pid);
		if (wait_event_interruptible(kbuf.read_wq, !kbuf_ring_is_empty(&kbuf)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&kbuf.lock))
			return -ERESTARTSYS;
	}

	ret = kbuf_ring_consume(&kbuf, ubuf, count);
	mutex_unlock(&kbuf.lock);

	if (ret >= 0)
		wake_up_interruptible(&kbuf.write_wq);	/* a slot is now free */
	return ret;
}

static ssize_t kbuf_write(struct file *filp, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	ssize_t ret;

	if (count > KBUF_BUFFER_SIZE)
		count = KBUF_BUFFER_SIZE;

	if (mutex_lock_interruptible(&kbuf.lock))
		return -ERESTARTSYS;

	while (kbuf_ring_is_full(&kbuf)) {
		mutex_unlock(&kbuf.lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		pr_info("kbuf: writer pid=%d sleeping (buffer full)\n", current->pid);
		if (wait_event_interruptible(kbuf.write_wq, !kbuf_ring_is_full(&kbuf)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&kbuf.lock))
			return -ERESTARTSYS;
	}

	ret = kbuf_ring_produce(&kbuf, ubuf, count);
	mutex_unlock(&kbuf.lock);

	if (ret >= 0)
		wake_up_interruptible(&kbuf.read_wq);	/* a full slot is ready */
	return ret;
}

static const struct file_operations kbuf_fops = {
	.owner          = THIS_MODULE,
	.open           = kbuf_open,
	.release        = kbuf_release,
	.read           = kbuf_read,
	.write          = kbuf_write,
	.unlocked_ioctl = kbuf_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static int __init kbuf_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&kbuf.devno, 0, 1, KBUF_DEVICE_NAME);
	if (ret < 0) {
		pr_err("kbuf: alloc_chrdev_region failed (%d)\n", ret);
		return ret;
	}

	cdev_init(&kbuf.cdev, &kbuf_fops);
	kbuf.cdev.owner = THIS_MODULE;
	ret = cdev_add(&kbuf.cdev, kbuf.devno, 1);
	if (ret < 0) {
		pr_err("kbuf: cdev_add failed (%d)\n", ret);
		goto err_cdev;
	}

	kbuf.cls = class_create(KBUF_DEVICE_NAME);
	if (IS_ERR(kbuf.cls)) {
		ret = PTR_ERR(kbuf.cls);
		pr_err("kbuf: class_create failed (%d)\n", ret);
		goto err_class;
	}
	kbuf.cls->devnode = kbuf_devnode;

	kbuf.dev = device_create(kbuf.cls, NULL, kbuf.devno, NULL, KBUF_DEVICE_NAME);
	if (IS_ERR(kbuf.dev)) {
		ret = PTR_ERR(kbuf.dev);
		pr_err("kbuf: device_create failed (%d)\n", ret);
		goto err_device;
	}

	mutex_init(&kbuf.lock);
	init_waitqueue_head(&kbuf.read_wq);
	init_waitqueue_head(&kbuf.write_wq);

	if (kbuf_proc_register())
		pr_warn("kbuf: proc_create failed (non-fatal)\n");

	pr_info("kbuf: loaded - /dev/kbuf (major=%d), /proc/kbuf_status\n",
		MAJOR(kbuf.devno));
	return 0;

err_device:
	class_destroy(kbuf.cls);
err_class:
	cdev_del(&kbuf.cdev);
err_cdev:
	unregister_chrdev_region(kbuf.devno, 1);
	return ret;
}

static void __exit kbuf_exit(void)
{
	kbuf_proc_unregister();
	device_destroy(kbuf.cls, kbuf.devno);
	class_destroy(kbuf.cls);
	cdev_del(&kbuf.cdev);
	unregister_chrdev_region(kbuf.devno, 1);
	pr_info("kbuf: unloaded\n");
}

module_init(kbuf_init);
module_exit(kbuf_exit);
