// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_debugfs.c - per-device counters under /sys/kernel/debug/kbuf/.
 *
 * Creates one directory per device (kbuf0, kbuf1, ...) exposing the raw
 * counters as individual files. These are read without the device lock, so
 * they are a best-effort live view for debugging — exact only at rest.
 * Everything degrades to a no-op if CONFIG_DEBUG_FS is disabled.
 */
#include <linux/debugfs.h>
#include <linux/kernel.h>

#include "kbuf_internal.h"

static struct dentry *kbuf_debug_root;

static void kbuf_debugfs_add(struct kbuf_dev *dev, unsigned int id)
{
	char name[16];
	struct dentry *d;

	scnprintf(name, sizeof(name), "kbuf%u", id);
	d = debugfs_create_dir(name, kbuf_debug_root);

	debugfs_create_u32("num_buffers",   0444, d, &dev->num_buffers);
	debugfs_create_u32("buffer_size",   0444, d, &dev->buffer_size);
	debugfs_create_u32("peak_count",    0444, d, &dev->peak_count);
	debugfs_create_u32("prod_idx",      0444, d, &dev->prod_idx);
	debugfs_create_u32("cons_idx",      0444, d, &dev->cons_idx);
	debugfs_create_u64("msgs_produced", 0444, d, &dev->msgs_produced);
	debugfs_create_u64("msgs_consumed", 0444, d, &dev->msgs_consumed);
	debugfs_create_u64("bytes_produced", 0444, d, &dev->bytes_produced);
	debugfs_create_u64("bytes_consumed", 0444, d, &dev->bytes_consumed);
	debugfs_create_u64("read_sleeps",   0444, d, &dev->read_sleeps);
	debugfs_create_u64("write_sleeps",  0444, d, &dev->write_sleeps);
}

void kbuf_debugfs_register(void)
{
	unsigned int i;

	kbuf_debug_root = debugfs_create_dir(KBUF_DEVICE_NAME, NULL);
	for (i = 0; i < kbuf_ndevices; i++)
		kbuf_debugfs_add(&kbuf_devices[i], i);
}

void kbuf_debugfs_unregister(void)
{
	debugfs_remove_recursive(kbuf_debug_root);
	kbuf_debug_root = NULL;
}
