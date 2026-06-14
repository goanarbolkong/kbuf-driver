// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_ioctl.c - ioctl dispatch for the kbuf device.
 *
 * The UAPI (command numbers and structs) is committed in include/kbuf.h.
 *
 *   KBUF_IOCGSTATS - copy the throughput counters to user space.
 *   KBUF_IOCRESET  - zero the counters (peak is reset to the current depth).
 *   KBUF_IOCRESIZE - change ring geometry; only when the ring is empty, else
 *                    -EBUSY (see docs/DESIGN.md for the rationale).
 *   KBUF_IOCSMODE  - select blocking vs lock-free SPSC mode (only on an idle,
 *                    empty ring; else -EBUSY).
 *   KBUF_IOCEXPORT - export the mmap data ring as a dma-buf fd (Phase 13).
 *   KBUF_IOCIMPORT - run the in-kernel importer self-test on a dma-buf fd.
 *
 * Every command acts on the device the fd was opened against
 * (filp->private_data), so each /dev/kbufN is controlled independently.
 */
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "kbuf_internal.h"

static long kbuf_ioc_getstats(struct kbuf_dev *dev, unsigned long arg)
{
	struct kbuf_stats st;

	memset(&st, 0, sizeof(st));
	mutex_lock(&dev->lock);
	st.bytes_produced = dev->bytes_produced;
	st.bytes_consumed = dev->bytes_consumed;
	st.msgs_produced  = dev->msgs_produced;
	st.msgs_consumed  = dev->msgs_consumed;
	st.read_sleeps    = dev->read_sleeps;
	st.write_sleeps   = dev->write_sleeps;
	st.num_buffers    = dev->num_buffers;
	st.buffer_size    = dev->buffer_size;
	st.cur_count      = kbuf_occupancy(dev);
	st.peak_count     = dev->peak_count;
	mutex_unlock(&dev->lock);

	if (copy_to_user((void __user *)arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

static long kbuf_ioc_reset(struct kbuf_dev *dev)
{
	mutex_lock(&dev->lock);
	dev->bytes_produced = 0;
	dev->bytes_consumed = 0;
	dev->msgs_produced  = 0;
	dev->msgs_consumed  = 0;
	dev->read_sleeps    = 0;
	dev->write_sleeps   = 0;
	dev->peak_count     = dev->count;
	mutex_unlock(&dev->lock);
	return 0;
}

static long kbuf_ioc_resize(struct kbuf_dev *dev, unsigned long arg)
{
	struct kbuf_resize rq;
	struct kbuf_slot *new_slots, *old_slots;
	unsigned int old_n;

	if (copy_from_user(&rq, (void __user *)arg, sizeof(rq)))
		return -EFAULT;
	if (rq.num_buffers < 1 || rq.num_buffers > KBUF_MAX_NUM_BUFFERS)
		return -EINVAL;
	if (rq.buffer_size < 1 || rq.buffer_size > KBUF_MAX_BUFFER_SIZE)
		return -EINVAL;
	/* SPSC masking requires a power-of-two capacity. */
	if (dev->mode == KBUF_MODE_SPSC && !is_power_of_2(rq.num_buffers))
		return -EINVAL;

	/*
	 * Allocate before taking the lock so the (possibly large) allocation
	 * does not stall readers/writers, and the lock is held only for the
	 * pointer swap.
	 */
	new_slots = kbuf_alloc_slots(rq.num_buffers, rq.buffer_size);
	if (!new_slots)
		return -ENOMEM;

	mutex_lock(&dev->lock);
	if (kbuf_occupancy(dev) != 0) {
		/* Refuse to discard in-flight data; caller drains first. */
		mutex_unlock(&dev->lock);
		kbuf_free_slots(new_slots, rq.num_buffers);
		return -EBUSY;
	}

	old_slots = dev->slots;
	old_n     = dev->num_buffers;
	dev->slots       = new_slots;
	dev->num_buffers = rq.num_buffers;
	dev->buffer_size = rq.buffer_size;
	dev->read_pos    = 0;
	dev->write_pos   = 0;
	dev->count       = 0;
	dev->prod_idx    = 0;
	dev->cons_idx    = 0;
	dev->peak_count  = 0;
	mutex_unlock(&dev->lock);

	kbuf_free_slots(old_slots, old_n);
	wake_up_interruptible(&dev->write_wq);	/* free space may have grown */
	pr_info("kbuf: %s resized to %u slots x %u bytes (pid=%d)\n",
		dev_name(dev->dev), rq.num_buffers, rq.buffer_size, current->pid);
	return 0;
}

static long kbuf_ioc_smode(struct kbuf_dev *dev, unsigned long arg)
{
	int mode;

	if (get_user(mode, (int __user *)arg))
		return -EFAULT;
	if (mode != KBUF_MODE_BLOCKING && mode != KBUF_MODE_SPSC)
		return -EINVAL;

	mutex_lock(&dev->lock);
	/*
	 * Switch only on an idle, empty ring. The mutex does not exclude an
	 * in-flight lock-free SPSC reader/writer, so the caller must also
	 * ensure no I/O is in progress (see docs/DESIGN.md).
	 */
	if (kbuf_occupancy(dev) != 0) {
		mutex_unlock(&dev->lock);
		return -EBUSY;
	}
	if (mode == KBUF_MODE_SPSC && !is_power_of_2(dev->num_buffers)) {
		mutex_unlock(&dev->lock);
		return -EINVAL;
	}

	/* Reset both index representations to a clean empty state. */
	dev->read_pos  = 0;
	dev->write_pos = 0;
	dev->count     = 0;
	dev->prod_idx  = 0;
	dev->cons_idx  = 0;
	WRITE_ONCE(dev->mode, mode);
	mutex_unlock(&dev->lock);
	return 0;
}

static long kbuf_ioc_export(struct kbuf_dev *dev, unsigned long arg)
{
	int fd = kbuf_dmabuf_export(dev);

	if (fd < 0)
		return fd;
	if (put_user(fd, (int __user *)arg)) {
		/*
		 * The caller will never learn the fd, so close it here rather
		 * than leak it into their table.
		 */
		close_fd(fd);
		return -EFAULT;
	}
	return 0;
}

static long kbuf_ioc_import(unsigned long arg)
{
	int fd;

	if (get_user(fd, (int __user *)arg))
		return -EFAULT;
	return kbuf_dmabuf_import_selftest(fd);
}

long kbuf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct kbuf_dev *dev = filp->private_data;

	if (_IOC_TYPE(cmd) != KBUF_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) == 0 || _IOC_NR(cmd) > KBUF_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case KBUF_IOCGSTATS:
		return kbuf_ioc_getstats(dev, arg);
	case KBUF_IOCRESET:
		return kbuf_ioc_reset(dev);
	case KBUF_IOCRESIZE:
		return kbuf_ioc_resize(dev, arg);
	case KBUF_IOCSMODE:
		return kbuf_ioc_smode(dev, arg);
	case KBUF_IOCEXPORT:
		return kbuf_ioc_export(dev, arg);
	case KBUF_IOCIMPORT:
		return kbuf_ioc_import(arg);
	default:
		return -ENOTTY;
	}
}
