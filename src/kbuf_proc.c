// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_proc.c - /proc/kbuf_status, a read-only view of every kbuf device.
 */
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "kbuf_internal.h"

static struct proc_dir_entry *kbuf_proc_entry;

static void kbuf_proc_show_dev(struct seq_file *m, struct kbuf_dev *dev, unsigned int idx)
{
	static const char * const mode_name[] = { "blocking", "spsc" };
	unsigned int occ, rd, wr;

	mutex_lock(&dev->lock);
	occ = kbuf_occupancy(dev);
	if (dev->mode == KBUF_MODE_SPSC) {
		rd = dev->cons_idx & (dev->num_buffers - 1);
		wr = dev->prod_idx & (dev->num_buffers - 1);
	} else {
		rd = dev->read_pos;
		wr = dev->write_pos;
	}
	seq_printf(m,  "=== kbuf%u ===\n", idx);
	seq_printf(m,  "Total slots    : %u\n", dev->num_buffers);
	seq_printf(m,  "Slot size      : %u bytes\n", dev->buffer_size);
	seq_printf(m,  "Full slots     : %u\n", occ);
	seq_printf(m,  "Free slots     : %u\n", dev->num_buffers - occ);
	seq_printf(m,  "Peak full slots: %u\n", dev->peak_count);
	seq_printf(m,  "Read  position : %u\n", rd);
	seq_printf(m,  "Write position : %u\n", wr);
	seq_printf(m,  "Mode           : %s\n",
		   dev->mode < (int)ARRAY_SIZE(mode_name) ? mode_name[dev->mode] : "?");
	seq_printf(m,  "Msgs  produced : %llu\n", dev->msgs_produced);
	seq_printf(m,  "Msgs  consumed : %llu\n", dev->msgs_consumed);
	seq_printf(m,  "Bytes produced : %llu\n", dev->bytes_produced);
	seq_printf(m,  "Bytes consumed : %llu\n", dev->bytes_consumed);
	seq_printf(m,  "Reader sleeps  : %llu\n", dev->read_sleeps);
	seq_printf(m,  "Writer sleeps  : %llu\n", dev->write_sleeps);
	mutex_unlock(&dev->lock);
}

static int kbuf_proc_show(struct seq_file *m, void *v)
{
	unsigned int i;

	for (i = 0; i < kbuf_ndevices; i++)
		kbuf_proc_show_dev(m, &kbuf_devices[i], i);
	return 0;
}

static int kbuf_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, kbuf_proc_show, NULL);
}

static const struct proc_ops kbuf_proc_ops = {
	.proc_open    = kbuf_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

int kbuf_proc_register(void)
{
	kbuf_proc_entry = proc_create("kbuf_status", 0444, NULL, &kbuf_proc_ops);
	if (!kbuf_proc_entry)
		return -ENOMEM;
	return 0;
}

void kbuf_proc_unregister(void)
{
	if (kbuf_proc_entry)
		proc_remove(kbuf_proc_entry);
}
