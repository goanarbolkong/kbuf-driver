// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_ring.c - circular-buffer core for the kbuf device.
 *
 * Pure ring mechanics: slot copy and index advance. These helpers assume the
 * caller already holds dev->lock and has checked the empty/full predicate, so
 * blocking and wakeup policy stays in kbuf_main.c. copy_to_user/copy_from_user
 * may sleep; that is fine under the mutex but is why this path is not
 * atomic-context safe (see CLAUDE.md / docs/DESIGN.md).
 */
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include "kbuf_internal.h"

bool kbuf_ring_is_empty(const struct kbuf_dev *dev)
{
	return dev->count == 0;
}

bool kbuf_ring_is_full(const struct kbuf_dev *dev)
{
	return dev->count == KBUF_NUM_BUFFERS;
}

/*
 * Consume one slot into @ubuf. Caller holds dev->lock and guarantees the ring
 * is non-empty. Returns bytes copied, or -EFAULT.
 */
ssize_t kbuf_ring_consume(struct kbuf_dev *dev, char __user *ubuf, size_t count)
{
	struct kbuf_slot *slot = &dev->slots[dev->read_pos];
	size_t len = min(count, slot->len);

	if (copy_to_user(ubuf, slot->data, len))
		return -EFAULT;

	pr_info("kbuf: read %zu bytes from slot[%d] (pid=%d)\n",
		len, dev->read_pos, current->pid);

	dev->read_pos = (dev->read_pos + 1) % KBUF_NUM_BUFFERS;
	dev->count--;
	return (ssize_t)len;
}

/*
 * Produce one slot from @ubuf. Caller holds dev->lock, guarantees the ring is
 * not full, and has clamped @count to KBUF_BUFFER_SIZE. Returns bytes copied,
 * or -EFAULT.
 */
ssize_t kbuf_ring_produce(struct kbuf_dev *dev, const char __user *ubuf, size_t count)
{
	struct kbuf_slot *slot = &dev->slots[dev->write_pos];

	if (copy_from_user(slot->data, ubuf, count))
		return -EFAULT;
	slot->len = count;

	pr_info("kbuf: wrote %zu bytes to slot[%d] (pid=%d)\n",
		count, dev->write_pos, current->pid);

	dev->write_pos = (dev->write_pos + 1) % KBUF_NUM_BUFFERS;
	dev->count++;
	return (ssize_t)count;
}
