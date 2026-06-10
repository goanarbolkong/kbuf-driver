// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_proc.c - /proc/kbuf_status read-only view of device state.
 */
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "kbuf_internal.h"

static struct proc_dir_entry *kbuf_proc_entry;

static int kbuf_proc_show(struct seq_file *m, void *v)
{
	static const char * const mode_name[] = { "blocking", "spsc" };

	mutex_lock(&kbuf.lock);
	seq_puts(m,    "=== kbuf driver status ===\n");
	seq_printf(m,  "Total slots    : %u\n", kbuf.num_buffers);
	seq_printf(m,  "Slot size      : %u bytes\n", kbuf.buffer_size);
	seq_printf(m,  "Full slots     : %d\n", kbuf.count);
	seq_printf(m,  "Free slots     : %u\n", kbuf.num_buffers - kbuf.count);
	seq_printf(m,  "Peak full slots: %u\n", kbuf.peak_count);
	seq_printf(m,  "Read  position : %d\n", kbuf.read_pos);
	seq_printf(m,  "Write position : %d\n", kbuf.write_pos);
	seq_printf(m,  "Mode           : %s\n",
		   kbuf.mode < (int)ARRAY_SIZE(mode_name) ? mode_name[kbuf.mode] : "?");
	seq_puts(m,    "--- throughput ---\n");
	seq_printf(m,  "Msgs  produced : %llu\n", kbuf.msgs_produced);
	seq_printf(m,  "Msgs  consumed : %llu\n", kbuf.msgs_consumed);
	seq_printf(m,  "Bytes produced : %llu\n", kbuf.bytes_produced);
	seq_printf(m,  "Bytes consumed : %llu\n", kbuf.bytes_consumed);
	seq_printf(m,  "Reader sleeps  : %llu\n", kbuf.read_sleeps);
	seq_printf(m,  "Writer sleeps  : %llu\n", kbuf.write_sleeps);
	mutex_unlock(&kbuf.lock);
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
