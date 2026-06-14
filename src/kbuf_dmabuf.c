// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_dmabuf.c - export the mmap data ring as a dma-buf (Phase 13).
 *
 * KBUF_IOCEXPORT wraps a device's existing vmalloc_user data ring
 * (dev->mmap_data, KBUF_MMAP_CAPACITY bytes) in a dma-buf and returns a fd.
 * The same physical pages already back the mmap zero-copy ring, so a dma-buf
 * importer and an mmap() of /dev/kbufN see one and the same buffer - that is
 * the point: hand the ring to another subsystem (a GPU, a V4L2 device, another
 * driver) with no copy.
 *
 * The exporter implements the full producer contract:
 *   - map_dma_buf / unmap_dma_buf: build an sg_table over the ring's pages and
 *     DMA-map it for an attached device.
 *   - vmap / vunmap: hand back the ring's existing contiguous kernel mapping
 *     (it is vmalloc'd, so it is already virtually contiguous).
 *   - mmap: fault the ring's pages into a userspace VMA (single, flat copy -
 *     unlike the driver's own mmap this is not the double-mapped magic ring).
 *   - begin/end_cpu_access: no-op; the ring is plain cache-coherent RAM.
 *
 * Lifetime. dma_buf_export() pins THIS_MODULE until the last reference to the
 * buffer is dropped, so the module cannot be unloaded while an exported fd is
 * live. A dynamic device (/dev/kbufdN) can still be destroyed underneath an
 * exported buffer, so export takes a kref and release drops it, keeping the
 * ring's pages alive for as long as the dma-buf exists.
 *
 * KBUF_IOCIMPORT is an in-kernel importer self-test: it imports a dma-buf fd,
 * attaches a synthetic DMA-capable device, maps the sg_table, vmaps the buffer,
 * reads the first 32-bit word and echoes it into the second word, then tears it
 * all down. It exercises the attach/map/vmap path that no userspace-only test
 * can reach; the echo lets a userspace test prove both directions of aliasing
 * in one shot (it wrote word[0] through the device mmap and reads word[1] back),
 * without a userspace mmap of the dma-buf fd - which on 6.17 trips a benign
 * path_noexec WARN on the dma-buf pseudo-mount (see docs/DEBUGGING.md).
 */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "kbuf_internal.h"

/* The dma-buf core puts its exported symbols in the DMA_BUF namespace. */
MODULE_IMPORT_NS("DMA_BUF");

#define KBUF_DMABUF_PAGES (KBUF_MMAP_CAPACITY / PAGE_SIZE)

/*
 * A synthetic DMA-capable device used only as the importer in the self-test.
 * Real importers bring their own struct device; QEMU has none we can borrow,
 * so we register a platform device and give it a 64-bit DMA mask. dma-direct
 * then maps the ring's RAM pages without an IOMMU.
 */
static struct platform_device *kbuf_import_pdev;

static struct sg_table *kbuf_dmabuf_map(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct kbuf_dev *dev = attach->dmabuf->priv;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int i;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, KBUF_DMABUF_PAGES, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	sg = sgt->sgl;
	for (i = 0; i < KBUF_DMABUF_PAGES; i++) {
		struct page *page =
			vmalloc_to_page(dev->mmap_data + i * PAGE_SIZE);

		sg_set_page(sg, page, PAGE_SIZE, 0);
		sg = sg_next(sg);
	}

	ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}
	return sgt;
}

static void kbuf_dmabuf_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static int kbuf_dmabuf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct kbuf_dev *dev = dmabuf->priv;

	/* The ring is vmalloc'd, hence already a contiguous kernel mapping. */
	iosys_map_set_vaddr(map, dev->mmap_data);
	return 0;
}

static void kbuf_dmabuf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	/* We handed back an existing mapping; nothing to release. */
}

static int kbuf_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct kbuf_dev *dev = dmabuf->priv;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > KBUF_MMAP_CAPACITY)
		return -EINVAL;

	/*
	 * A flat single mapping of the ring - not the driver's double-mapped
	 * magic ring. remap_vmalloc_range works because mmap_data came from
	 * vmalloc_user().
	 */
	return remap_vmalloc_range(vma, dev->mmap_data, vma->vm_pgoff);
}

static int kbuf_dmabuf_begin_cpu(struct dma_buf *dmabuf,
				 enum dma_data_direction dir)
{
	/* Plain cache-coherent RAM: no bounce, no cache maintenance needed. */
	return 0;
}

static int kbuf_dmabuf_end_cpu(struct dma_buf *dmabuf,
			       enum dma_data_direction dir)
{
	return 0;
}

static void kbuf_dmabuf_release(struct dma_buf *dmabuf)
{
	struct kbuf_dev *dev = dmabuf->priv;

	/* Balance the kref taken in kbuf_dmabuf_export() for dynamic devices. */
	if (dev->dynamic)
		kref_put(&dev->ref, kbuf_dev_release);
}

static const struct dma_buf_ops kbuf_dmabuf_ops = {
	.map_dma_buf	 = kbuf_dmabuf_map,
	.unmap_dma_buf	 = kbuf_dmabuf_unmap,
	.release	 = kbuf_dmabuf_release,
	.mmap		 = kbuf_dmabuf_mmap,
	.vmap		 = kbuf_dmabuf_vmap,
	.vunmap		 = kbuf_dmabuf_vunmap,
	.begin_cpu_access = kbuf_dmabuf_begin_cpu,
	.end_cpu_access	 = kbuf_dmabuf_end_cpu,
};

/* KBUF_IOCEXPORT: wrap dev->mmap_data in a dma-buf and return a new fd. */
int kbuf_dmabuf_export(struct kbuf_dev *dev)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int fd;

	exp_info.ops = &kbuf_dmabuf_ops;
	exp_info.size = KBUF_MMAP_CAPACITY;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv = dev;
	exp_info.exp_name = dev_name(dev->dev);

	/* Keep a dynamic device's ring alive for the life of the dma-buf. */
	if (dev->dynamic)
		kref_get(&dev->ref);

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		if (dev->dynamic)
			kref_put(&dev->ref, kbuf_dev_release);
		return PTR_ERR(dmabuf);
	}

	/* On success the fd owns the only reference; release() drops the kref. */
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);
	return fd;
}

/*
 * KBUF_IOCIMPORT: in-kernel importer self-test. Imports @fd, attaches the
 * synthetic DMA device, maps the sg_table, vmaps the buffer, reads the first
 * 32-bit word and echoes it into the second word, then unwinds. Returns 0 on
 * success or a negative errno. The echo lets userspace confirm, through the
 * device mmap, that the importer mapped the very same pages.
 */
long kbuf_dmabuf_import_selftest(int fd)
{
	struct dma_buf_attachment *attach;
	struct iosys_map map = {};
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	u32 *words, first;
	long ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	attach = dma_buf_attach(dmabuf, &kbuf_import_pdev->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto put;
	}

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto detach;
	}
	if (sgt->nents < 1) {
		ret = -EIO;
		goto unmap;
	}

	ret = dma_buf_vmap_unlocked(dmabuf, &map);
	if (ret)
		goto unmap;

	words = map.vaddr;
	first = words[0];
	words[1] = first;		/* echo back so userspace can verify */
	pr_info("kbuf: dmabuf import ok: size=%zu nents=%u first=0x%08x\n",
		dmabuf->size, sgt->nents, first);
	dma_buf_vunmap_unlocked(dmabuf, &map);
	ret = 0;

unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(dmabuf, attach);
put:
	dma_buf_put(dmabuf);
	return ret;
}

int kbuf_dmabuf_init(void)
{
	int ret;

	kbuf_import_pdev = platform_device_register_simple("kbuf-dmabuf-import",
							   -1, NULL, 0);
	if (IS_ERR(kbuf_import_pdev))
		return PTR_ERR(kbuf_import_pdev);

	ret = dma_coerce_mask_and_coherent(&kbuf_import_pdev->dev,
					   DMA_BIT_MASK(64));
	if (ret) {
		platform_device_unregister(kbuf_import_pdev);
		kbuf_import_pdev = NULL;
		return ret;
	}
	return 0;
}

void kbuf_dmabuf_exit(void)
{
	platform_device_unregister(kbuf_import_pdev);
	kbuf_import_pdev = NULL;
}
