// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_ring.c - circular-buffer core for the kbuf device.
 *
 * Pure ring mechanics: allocation of the slot array, slot copy, index advance,
 * and throughput accounting. The consume/produce helpers assume the caller
 * already holds dev->lock and has checked the empty/full predicate, so blocking
 * and wakeup policy stays in kbuf_main.c. copy_to_user/copy_from_user may sleep;
 * that is fine under the mutex but is why this path is not atomic-context safe
 * (see CLAUDE.md / docs/DESIGN.md).
 */
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "kbuf_internal.h"
#include "kbuf_trace.h"

/* Device index (minor) for tracepoints. */
static inline unsigned int kbuf_id(const struct kbuf_dev *dev)
{
	return (unsigned int)(dev - kbuf_devices);
}

/*
 * Allocate a slot array and each slot's data buffer. No lock required: the
 * result is published into a device under dev->lock by the caller.
 */
struct kbuf_slot *kbuf_alloc_slots(unsigned int num_buffers, unsigned int buffer_size)
{
	struct kbuf_slot *slots;
	unsigned int i;

	slots = kcalloc(num_buffers, sizeof(*slots), GFP_KERNEL);
	if (!slots)
		return NULL;

	for (i = 0; i < num_buffers; i++) {
		slots[i].data = kmalloc(buffer_size, GFP_KERNEL);
		if (!slots[i].data) {
			while (i--)
				kfree(slots[i].data);
			kfree(slots);
			return NULL;
		}
	}
	return slots;
}

void kbuf_free_slots(struct kbuf_slot *slots, unsigned int num_buffers)
{
	unsigned int i;

	if (!slots)
		return;
	for (i = 0; i < num_buffers; i++)
		kfree(slots[i].data);
	kfree(slots);
}

bool kbuf_ring_is_empty(const struct kbuf_dev *dev)
{
	return dev->count == 0;
}

bool kbuf_ring_is_full(const struct kbuf_dev *dev)
{
	return dev->count == dev->num_buffers;
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

	dev->read_pos = (dev->read_pos + 1) % dev->num_buffers;
	dev->count--;
	dev->bytes_consumed += len;
	dev->msgs_consumed++;
	trace_kbuf_consume(kbuf_id(dev), (unsigned int)len, dev->count);
	return (ssize_t)len;
}

/*
 * Produce one slot from @ubuf. Caller holds dev->lock, guarantees the ring is
 * not full, and has clamped @count to dev->buffer_size. Returns bytes copied,
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

	dev->write_pos = (dev->write_pos + 1) % dev->num_buffers;
	dev->count++;
	if (dev->count > dev->peak_count)
		dev->peak_count = dev->count;
	dev->bytes_produced += count;
	dev->msgs_produced++;
	trace_kbuf_produce(kbuf_id(dev), (unsigned int)count, dev->count);
	return (ssize_t)count;
}

/* ---------- lock-free SPSC mode ---------- */
/*
 * These run without dev->lock and are correct only with a single producer and
 * a single consumer. prod_idx and cons_idx are free-running (they never wrap to
 * zero); the slot index is prod/cons masked by (num_buffers - 1), which is why
 * SPSC requires a power-of-two num_buffers. The acquire/release pairs make the
 * slot contents visible across cores before the matching index becomes visible.
 *
 * copy_to_user / copy_from_user may sleep, so — like the blocking path — this
 * is process-context only and not safe to call from atomic context.
 */

bool kbuf_spsc_is_empty(struct kbuf_dev *dev)
{
	/* Consumer view: observe the producer's published head. */
	return smp_load_acquire(&dev->prod_idx) == READ_ONCE(dev->cons_idx);
}

bool kbuf_spsc_is_full(struct kbuf_dev *dev)
{
	/* Producer view: observe the consumer's published tail. */
	return READ_ONCE(dev->prod_idx) - smp_load_acquire(&dev->cons_idx)
		>= dev->num_buffers;
}

/* Single consumer. Caller has confirmed the ring is non-empty. */
ssize_t kbuf_spsc_consume(struct kbuf_dev *dev, char __user *ubuf, size_t count)
{
	unsigned int cons = dev->cons_idx;	/* only we advance it */
	struct kbuf_slot *slot = &dev->slots[cons & (dev->num_buffers - 1)];
	size_t len = min(count, slot->len);

	if (copy_to_user(ubuf, slot->data, len))
		return -EFAULT;

	/* Publish the freed slot to the producer. */
	smp_store_release(&dev->cons_idx, cons + 1);

	WRITE_ONCE(dev->bytes_consumed, dev->bytes_consumed + len);
	WRITE_ONCE(dev->msgs_consumed, dev->msgs_consumed + 1);
	trace_kbuf_consume(kbuf_id(dev), (unsigned int)len,
			   READ_ONCE(dev->prod_idx) - (cons + 1));
	return (ssize_t)len;
}

/* Single producer. Caller has confirmed the ring is not full. */
ssize_t kbuf_spsc_produce(struct kbuf_dev *dev, const char __user *ubuf, size_t count)
{
	unsigned int prod = dev->prod_idx;	/* only we advance it */
	struct kbuf_slot *slot = &dev->slots[prod & (dev->num_buffers - 1)];
	unsigned int occ;

	if (copy_from_user(slot->data, ubuf, count))
		return -EFAULT;
	slot->len = count;

	/* Publish the filled slot to the consumer (orders the writes above). */
	smp_store_release(&dev->prod_idx, prod + 1);

	WRITE_ONCE(dev->bytes_produced, dev->bytes_produced + count);
	WRITE_ONCE(dev->msgs_produced, dev->msgs_produced + 1);
	occ = prod + 1 - READ_ONCE(dev->cons_idx);
	if (occ > READ_ONCE(dev->peak_count))
		WRITE_ONCE(dev->peak_count, occ);
	trace_kbuf_produce(kbuf_id(dev), (unsigned int)count, occ);
	return (ssize_t)count;
}

unsigned int kbuf_occupancy(struct kbuf_dev *dev)
{
	if (dev->mode == KBUF_MODE_SPSC)
		return READ_ONCE(dev->prod_idx) - READ_ONCE(dev->cons_idx);
	return dev->count;
}
