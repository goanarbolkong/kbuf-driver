/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kbuf_internal.h - in-kernel types and cross-file prototypes for kbuf.
 *
 * Private to the module. The user/kernel ABI lives in include/kbuf.h; nothing
 * here crosses the syscall boundary.
 */
#ifndef _KBUF_INTERNAL_H
#define _KBUF_INTERNAL_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "kbuf.h"		/* UAPI (via -Iinclude) */

#define KBUF_DEVICE_NAME "kbuf"
#define KBUF_NUM_BUFFERS 8	/* number of circular buffer slots */
#define KBUF_BUFFER_SIZE 4096	/* bytes per slot */

struct kbuf_slot {
	char   data[KBUF_BUFFER_SIZE];
	size_t len;
};

/*
 * The device state. A single global instance backs /dev/kbuf today; Phase 4
 * turns this into one-per-minor, which is why the ring/proc/ioctl helpers all
 * take an explicit struct kbuf_dev * rather than reaching for a global.
 */
struct kbuf_dev {
	struct kbuf_slot       slots[KBUF_NUM_BUFFERS];
	int                    read_pos;	/* next slot to consume        */
	int                    write_pos;	/* next slot to produce        */
	int                    count;		/* number of full slots        */
	wait_queue_head_t      read_wq;		/* readers sleep when empty    */
	wait_queue_head_t      write_wq;	/* writers sleep when full     */
	struct mutex           lock;
	struct cdev            cdev;
	dev_t                  devno;
	struct class          *cls;
	struct device         *dev;
};

extern struct kbuf_dev kbuf;

/* ring core - src/kbuf_ring.c. All callers must hold dev->lock. */
bool    kbuf_ring_is_empty(const struct kbuf_dev *dev);
bool    kbuf_ring_is_full(const struct kbuf_dev *dev);
ssize_t kbuf_ring_consume(struct kbuf_dev *dev, char __user *ubuf, size_t count);
ssize_t kbuf_ring_produce(struct kbuf_dev *dev, const char __user *ubuf, size_t count);

/* /proc interface - src/kbuf_proc.c */
int  kbuf_proc_register(void);
void kbuf_proc_unregister(void);

/* ioctl dispatch - src/kbuf_ioctl.c */
long kbuf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

#endif /* _KBUF_INTERNAL_H */
