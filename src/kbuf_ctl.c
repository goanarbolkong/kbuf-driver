// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_ctl.c - /dev/kbuf-ctl, runtime create/destroy of kbuf devices.
 *
 * A misc device exposes two ioctls: KBUF_CTL_CREATE allocates a new device
 * (/dev/kbufdN) and returns its id; KBUF_CTL_DESTROY tears one down. Dynamic
 * devices are heap-allocated and reference-counted with a kref so that a device
 * destroyed while a process still has it open survives until the last close —
 * destroy only removes the node (blocking new opens) and drops the initial
 * reference. Minors come from an ida over the dynamic range reserved in
 * kbuf_main.c (the minors just past the static devices).
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "kbuf_internal.h"

static LIST_HEAD(kbuf_dyn_list);
static DEFINE_MUTEX(kbuf_dyn_lock);
static DEFINE_IDA(kbuf_dyn_ida);

/*
 * kref release: the last reference is gone, so the ring is idle. Frees only the
 * kbuf_dev (ring buffers + struct) — the cdev is separately owned and freed by
 * the kernel after the VFS's final cdev_put, which is why it is a standalone
 * cdev_alloc()'d object rather than embedded here.
 */
void kbuf_dev_release(struct kref *ref)
{
	struct kbuf_dev *dev = container_of(ref, struct kbuf_dev, ref);

	kbuf_mmap_dev_free(dev);
	kbuf_free_slots(dev->slots, dev->num_buffers);
	ida_free(&kbuf_dyn_ida, dev->minor_off);
	pr_info("kbuf: kbufd%d freed\n", dev->minor_off);
	kfree(dev);
}

/* Find a dynamic device by minor offset and pin it with a reference. */
struct kbuf_dev *kbuf_dyn_get(int minor_off)
{
	struct kbuf_dev *dev, *found = NULL;

	mutex_lock(&kbuf_dyn_lock);
	list_for_each_entry(dev, &kbuf_dyn_list, list) {
		if (dev->minor_off == minor_off) {
			kref_get(&dev->ref);
			found = dev;
			break;
		}
	}
	mutex_unlock(&kbuf_dyn_lock);
	return found;
}

static int kbuf_dyn_create(int *id_out)
{
	struct kbuf_dev *dev;
	int off, ret;

	off = ida_alloc_max(&kbuf_dyn_ida, KBUF_DYN_MAX - 1, GFP_KERNEL);
	if (off < 0)
		return off == -ENOSPC ? -ENOSPC : off;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto err_ida;
	}

	dev->dynamic     = true;
	dev->minor_off   = off;
	dev->num_buffers = KBUF_DEFAULT_NUM_BUFFERS;
	dev->buffer_size = KBUF_DEFAULT_BUFFER_SIZE;
	dev->mode        = KBUF_MODE_BLOCKING;
	dev->devno       = kbuf_base_devno + kbuf_ndevices + off;
	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->read_wq);
	init_waitqueue_head(&dev->write_wq);
	INIT_LIST_HEAD(&dev->list);
	kref_init(&dev->ref);		/* the "registered" reference */

	dev->slots = kbuf_alloc_slots(dev->num_buffers, dev->buffer_size);
	if (!dev->slots) {
		ret = -ENOMEM;
		goto err_free;
	}
	ret = kbuf_mmap_dev_alloc(dev);
	if (ret < 0)
		goto err_slots;

	/*
	 * Standalone cdev (not embedded): the kernel owns its kobject and frees
	 * it after the VFS's final cdev_put, which happens *after* our ->release.
	 * Embedding it in the kref-freed kbuf_dev would let ->release free it
	 * out from under that cdev_put — a use-after-free.
	 */
	dev->cdevp = cdev_alloc();
	if (!dev->cdevp) {
		ret = -ENOMEM;
		goto err_mmap;
	}
	dev->cdevp->ops = &kbuf_fops;
	dev->cdevp->owner = THIS_MODULE;
	ret = cdev_add(dev->cdevp, dev->devno, 1);
	if (ret < 0) {
		kobject_put(&dev->cdevp->kobj);
		goto err_mmap;
	}

	dev->dev = device_create(kbuf_class, NULL, dev->devno, NULL, "kbufd%d", off);
	if (IS_ERR(dev->dev)) {
		ret = PTR_ERR(dev->dev);
		goto err_cdev;
	}

	mutex_lock(&kbuf_dyn_lock);
	list_add_tail(&dev->list, &kbuf_dyn_list);
	mutex_unlock(&kbuf_dyn_lock);

	pr_info("kbuf: created kbufd%d (pid=%d)\n", off, current->pid);
	*id_out = off;
	return 0;

err_cdev:
	cdev_del(dev->cdevp);
err_mmap:
	kbuf_mmap_dev_free(dev);
err_slots:
	kbuf_free_slots(dev->slots, dev->num_buffers);
err_free:
	kfree(dev);
err_ida:
	ida_free(&kbuf_dyn_ida, off);
	return ret;
}

/*
 * Unlink and tear down a dynamic device; the kref keeps it alive for any
 * already-open files until they close. Caller must not hold kbuf_dyn_lock.
 */
static void kbuf_dyn_teardown(struct kbuf_dev *dev)
{
	device_destroy(kbuf_class, dev->devno);	/* removes /dev node: no new opens */
	cdev_del(dev->cdevp);			/* kernel frees the cdev after fput */
	kref_put(&dev->ref, kbuf_dev_release);	/* drop the registered reference */
}

static int kbuf_dyn_destroy(int id)
{
	struct kbuf_dev *dev, *found = NULL;

	mutex_lock(&kbuf_dyn_lock);
	list_for_each_entry(dev, &kbuf_dyn_list, list) {
		if (dev->minor_off == id) {
			found = dev;
			list_del(&dev->list);
			break;
		}
	}
	mutex_unlock(&kbuf_dyn_lock);

	if (!found)
		return -ENOENT;
	pr_info("kbuf: destroying kbufd%d (pid=%d)\n", id, current->pid);
	kbuf_dyn_teardown(found);
	return 0;
}

static long kbuf_ctl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int id, ret;

	switch (cmd) {
	case KBUF_CTL_CREATE:
		ret = kbuf_dyn_create(&id);
		if (ret)
			return ret;
		if (put_user(id, (int __user *)arg)) {
			kbuf_dyn_destroy(id);
			return -EFAULT;
		}
		return 0;
	case KBUF_CTL_DESTROY:
		if (get_user(id, (int __user *)arg))
			return -EFAULT;
		return kbuf_dyn_destroy(id);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations kbuf_ctl_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = kbuf_ctl_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice kbuf_ctl_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "kbuf-ctl",
	.fops  = &kbuf_ctl_fops,
};

int kbuf_ctl_register(void)
{
	return misc_register(&kbuf_ctl_misc);
}

void kbuf_ctl_unregister(void)
{
	struct kbuf_dev *dev;

	/* No more creates can arrive once the control node is gone. */
	misc_deregister(&kbuf_ctl_misc);

	/* Tear down any devices the user left behind. */
	for (;;) {
		mutex_lock(&kbuf_dyn_lock);
		dev = list_first_entry_or_null(&kbuf_dyn_list, struct kbuf_dev, list);
		if (dev)
			list_del(&dev->list);
		mutex_unlock(&kbuf_dyn_lock);
		if (!dev)
			break;
		kbuf_dyn_teardown(dev);
	}

	ida_destroy(&kbuf_dyn_ida);
}
