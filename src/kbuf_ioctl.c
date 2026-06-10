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
 *   KBUF_IOCSMODE  - select blocking vs lock-free SPSC mode. SPSC is not yet
 *                    implemented (Phase 5), so it returns -EOPNOTSUPP.
 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "kbuf_internal.h"

static long kbuf_ioc_getstats(unsigned long arg)
{
	struct kbuf_stats st;

	memset(&st, 0, sizeof(st));
	mutex_lock(&kbuf.lock);
	st.bytes_produced = kbuf.bytes_produced;
	st.bytes_consumed = kbuf.bytes_consumed;
	st.msgs_produced  = kbuf.msgs_produced;
	st.msgs_consumed  = kbuf.msgs_consumed;
	st.read_sleeps    = kbuf.read_sleeps;
	st.write_sleeps   = kbuf.write_sleeps;
	st.num_buffers    = kbuf.num_buffers;
	st.buffer_size    = kbuf.buffer_size;
	st.cur_count      = kbuf.count;
	st.peak_count     = kbuf.peak_count;
	mutex_unlock(&kbuf.lock);

	if (copy_to_user((void __user *)arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

static long kbuf_ioc_reset(void)
{
	mutex_lock(&kbuf.lock);
	kbuf.bytes_produced = 0;
	kbuf.bytes_consumed = 0;
	kbuf.msgs_produced  = 0;
	kbuf.msgs_consumed  = 0;
	kbuf.read_sleeps    = 0;
	kbuf.write_sleeps   = 0;
	kbuf.peak_count     = kbuf.count;
	mutex_unlock(&kbuf.lock);
	return 0;
}

static long kbuf_ioc_resize(unsigned long arg)
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

	/*
	 * Allocate before taking the lock so the (possibly large) allocation
	 * does not stall readers/writers, and the lock is held only for the
	 * pointer swap.
	 */
	new_slots = kbuf_alloc_slots(rq.num_buffers, rq.buffer_size);
	if (!new_slots)
		return -ENOMEM;

	mutex_lock(&kbuf.lock);
	if (kbuf.count != 0) {
		/* Refuse to discard in-flight data; caller drains first. */
		mutex_unlock(&kbuf.lock);
		kbuf_free_slots(new_slots, rq.num_buffers);
		return -EBUSY;
	}

	old_slots = kbuf.slots;
	old_n     = kbuf.num_buffers;
	kbuf.slots       = new_slots;
	kbuf.num_buffers = rq.num_buffers;
	kbuf.buffer_size = rq.buffer_size;
	kbuf.read_pos    = 0;
	kbuf.write_pos   = 0;
	kbuf.count       = 0;
	kbuf.peak_count  = 0;
	mutex_unlock(&kbuf.lock);

	kbuf_free_slots(old_slots, old_n);
	wake_up_interruptible(&kbuf.write_wq);	/* free space may have grown */
	pr_info("kbuf: resized to %u slots x %u bytes (pid=%d)\n",
		rq.num_buffers, rq.buffer_size, current->pid);
	return 0;
}

static long kbuf_ioc_smode(unsigned long arg)
{
	int mode;

	if (get_user(mode, (int __user *)arg))
		return -EFAULT;
	if (mode != KBUF_MODE_BLOCKING && mode != KBUF_MODE_SPSC)
		return -EINVAL;
	if (mode == KBUF_MODE_SPSC)
		return -EOPNOTSUPP;	/* lock-free SPSC arrives in Phase 5 */

	mutex_lock(&kbuf.lock);
	kbuf.mode = mode;
	mutex_unlock(&kbuf.lock);
	return 0;
}

long kbuf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != KBUF_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) == 0 || _IOC_NR(cmd) > KBUF_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case KBUF_IOCGSTATS:
		return kbuf_ioc_getstats(arg);
	case KBUF_IOCRESET:
		return kbuf_ioc_reset();
	case KBUF_IOCRESIZE:
		return kbuf_ioc_resize(arg);
	case KBUF_IOCSMODE:
		return kbuf_ioc_smode(arg);
	default:
		return -ENOTTY;
	}
}
