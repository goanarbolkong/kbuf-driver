// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_mmap.c - zero-copy mmap ring for the kbuf device ("magic ring buffer").
 *
 * Each device owns two vmalloc_user buffers shared with user space:
 *   - mmap_ctrl: one page, a struct kbuf_mmap_ctrl (head/tail/capacity).
 *   - mmap_data: KBUF_MMAP_CAPACITY bytes, the byte ring.
 *
 * The mmap region is laid out as [ctrl page][data][data] — the data buffer is
 * mapped *twice*, back to back, so a record that wraps the end of the ring is
 * still contiguous in the mapping and user space can memcpy it in one shot.
 * The double mapping is achieved in the fault handler by reducing the data
 * page offset modulo the number of data pages, so both copies resolve to the
 * same physical pages.
 *
 * The kernel only provides the shared memory; all producer/consumer index
 * management lives in user space (see include/libkbuf.h). Because the ring is
 * shared by physical page, two processes that each mmap the same /dev/kbufN see
 * the same ring — that is how a producer and consumer rendezvous.
 */
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include "kbuf_internal.h"

int kbuf_mmap_dev_alloc(struct kbuf_dev *dev)
{
	struct kbuf_mmap_ctrl *ctrl;

	dev->mmap_ctrl = vmalloc_user(PAGE_SIZE);
	if (!dev->mmap_ctrl)
		return -ENOMEM;

	dev->mmap_data = vmalloc_user(KBUF_MMAP_CAPACITY);
	if (!dev->mmap_data) {
		vfree(dev->mmap_ctrl);
		dev->mmap_ctrl = NULL;
		return -ENOMEM;
	}

	ctrl = dev->mmap_ctrl;
	ctrl->head = 0;
	ctrl->tail = 0;
	ctrl->capacity = KBUF_MMAP_CAPACITY;
	return 0;
}

void kbuf_mmap_dev_free(struct kbuf_dev *dev)
{
	vfree(dev->mmap_data);
	dev->mmap_data = NULL;
	vfree(dev->mmap_ctrl);
	dev->mmap_ctrl = NULL;
}

/*
 * Fault handler. pgoff 0 is the control page; pgoff >= 1 indexes the data ring,
 * reduced modulo the data page count so the two virtual copies share pages.
 */
static vm_fault_t kbuf_vm_fault(struct vm_fault *vmf)
{
	struct kbuf_dev *dev = vmf->vma->vm_private_data;
	unsigned long data_pages = KBUF_MMAP_CAPACITY / PAGE_SIZE;
	struct page *page;
	void *kaddr;

	if (vmf->pgoff == 0) {
		kaddr = dev->mmap_ctrl;
	} else {
		unsigned long idx = (vmf->pgoff - 1) % data_pages;

		kaddr = dev->mmap_data + idx * PAGE_SIZE;
	}

	page = vmalloc_to_page(kaddr);
	if (!page)
		return VM_FAULT_SIGBUS;

	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct kbuf_vm_ops = {
	.fault = kbuf_vm_fault,
};

int kbuf_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct kbuf_dev *dev = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long expected = PAGE_SIZE + 2UL * KBUF_MMAP_CAPACITY;

	/* The whole region must be mapped from the start, exactly once. */
	if (vma->vm_pgoff != 0)
		return -EINVAL;
	if (size != expected)
		return -EINVAL;

	vma->vm_private_data = dev;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &kbuf_vm_ops;
	return 0;
}
