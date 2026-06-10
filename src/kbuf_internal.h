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

/* Default geometry at module load; changeable at runtime via KBUF_IOCRESIZE. */
#define KBUF_DEFAULT_NUM_BUFFERS 8	/* circular buffer slots */
#define KBUF_DEFAULT_BUFFER_SIZE 4096	/* bytes per slot        */

/* Upper bounds accepted by KBUF_IOCRESIZE (worst case ~16 MiB of slot data). */
#define KBUF_MAX_NUM_BUFFERS 256
#define KBUF_MAX_BUFFER_SIZE (64 * 1024)

/* Number of /dev/kbufN devices, set by the ndevices module param. */
#define KBUF_DEFAULT_NDEVICES 4
#define KBUF_MAX_NDEVICES     64

struct kbuf_slot {
	char  *data;	/* heap buffer of dev->buffer_size bytes */
	size_t len;	/* valid bytes currently held            */
};

/*
 * The device state. A single global instance backs /dev/kbuf today; Phase 4
 * turns this into one-per-minor, which is why the ring/proc/ioctl helpers all
 * take an explicit struct kbuf_dev * rather than reaching for a global.
 *
 * Geometry (slots, num_buffers, buffer_size) and all counters are protected by
 * @lock. The slots array and each slot's data buffer are heap-allocated so the
 * ring can be resized at runtime.
 */
struct kbuf_dev {
	struct kbuf_slot      *slots;		/* num_buffers entries         */
	unsigned int           num_buffers;
	unsigned int           buffer_size;
	int                    read_pos;	/* next slot to consume        */
	int                    write_pos;	/* next slot to produce        */
	int                    count;		/* number of full slots        */
	unsigned int           peak_count;	/* high-water mark of count    */

	/* throughput counters (see struct kbuf_stats) */
	u64                    bytes_produced;
	u64                    bytes_consumed;
	u64                    msgs_produced;
	u64                    msgs_consumed;
	u64                    read_sleeps;
	u64                    write_sleeps;

	int                    mode;		/* enum kbuf_mode              */

	wait_queue_head_t      read_wq;		/* readers sleep when empty    */
	wait_queue_head_t      write_wq;	/* writers sleep when full     */
	struct mutex           lock;
	struct cdev            cdev;		/* container_of target in open()*/
	dev_t                  devno;
	struct device         *dev;
};

/* The device array and its length, owned by kbuf_main.c. */
extern struct kbuf_dev *kbuf_devices;
extern unsigned int     kbuf_ndevices;

/* ring core - src/kbuf_ring.c */
struct kbuf_slot *kbuf_alloc_slots(unsigned int num_buffers, unsigned int buffer_size);
void              kbuf_free_slots(struct kbuf_slot *slots, unsigned int num_buffers);

/* The predicates and the consume/produce helpers all require dev->lock held. */
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
