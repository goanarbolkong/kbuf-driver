/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kbuf.h - user/kernel ABI (UAPI) for the kbuf character device.
 *
 * Included by both the kernel module and user-space programs so the ioctl
 * command numbers and the kbuf_stats layout never drift apart. Anything that
 * crosses the syscall boundary lives here; internal kernel types do not.
 *
 * ABI stability: the command numbers and struct layouts below are committed.
 * New fields are added by appending and bumping the relevant phase, never by
 * reordering or resizing existing members.
 */
#ifndef _UAPI_KBUF_H
#define _UAPI_KBUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Runtime mode of a kbuf device (see KBUF_IOCSMODE). */
enum kbuf_mode {
	KBUF_MODE_BLOCKING = 0,	/* mutex + wait queues (default)            */
	KBUF_MODE_SPSC     = 1,	/* lock-free single-producer/consumer (P5)  */
};

/* Argument to KBUF_IOCRESIZE: requested ring geometry. */
struct kbuf_resize {
	__u32 num_buffers;	/* number of slots                          */
	__u32 buffer_size;	/* bytes per slot                           */
};

/*
 * Snapshot of device counters, returned by KBUF_IOCGSTATS.
 * Fixed-width types keep the layout identical in kernel and user space.
 */
struct kbuf_stats {
	__u64 bytes_produced;	/* total bytes accepted from writers        */
	__u64 bytes_consumed;	/* total bytes handed to readers            */
	__u64 msgs_produced;	/* successful write() calls                 */
	__u64 msgs_consumed;	/* successful read() calls                  */
	__u64 read_sleeps;	/* times a reader blocked on empty ring     */
	__u64 write_sleeps;	/* times a writer blocked on full ring      */
	__u32 num_buffers;	/* configured slot count                    */
	__u32 buffer_size;	/* configured bytes per slot                */
	__u32 cur_count;	/* full slots right now                     */
	__u32 peak_count;	/* high-water mark of full slots            */
};

#define KBUF_IOC_MAGIC 'k'

#define KBUF_IOCRESIZE	_IOW(KBUF_IOC_MAGIC, 1, struct kbuf_resize)
#define KBUF_IOCGSTATS	_IOR(KBUF_IOC_MAGIC, 2, struct kbuf_stats)
#define KBUF_IOCRESET	_IO(KBUF_IOC_MAGIC,  3)
#define KBUF_IOCSMODE	_IOW(KBUF_IOC_MAGIC, 4, int)

#define KBUF_IOC_MAXNR 4

#endif /* _UAPI_KBUF_H */
