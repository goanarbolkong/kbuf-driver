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

/*
 * mmap zero-copy ring (Phase 6).
 *
 * The mmap region is one control page followed by the data ring mapped twice
 * back-to-back (the "magic ring buffer"), so a record that wraps the end of the
 * ring is still contiguous in the mapping. Map exactly
 *   page_size + 2 * KBUF_MMAP_CAPACITY
 * bytes at offset 0. head and tail are free-running byte indices; the slot is
 * (index & (KBUF_MMAP_CAPACITY - 1)). One producer updates head, one consumer
 * updates tail, using acquire/release atomics in user space (see libkbuf.h).
 */
#define KBUF_MMAP_CAPACITY (64u * 1024u)	/* data ring bytes; power of two */

struct kbuf_mmap_ctrl {
	__u64 head;		/* producer write index (bytes, free-running)  */
	__u64 _pad0[7];		/* keep head and tail on separate cache lines  */
	__u64 tail;		/* consumer read index (bytes, free-running)   */
	__u64 _pad1[7];
	__u32 capacity;		/* == KBUF_MMAP_CAPACITY                       */
};

#define KBUF_IOC_MAGIC 'k'

#define KBUF_IOCRESIZE	_IOW(KBUF_IOC_MAGIC, 1, struct kbuf_resize)
#define KBUF_IOCGSTATS	_IOR(KBUF_IOC_MAGIC, 2, struct kbuf_stats)
#define KBUF_IOCRESET	_IO(KBUF_IOC_MAGIC,  3)
#define KBUF_IOCSMODE	_IOW(KBUF_IOC_MAGIC, 4, int)

/*
 * dma-buf exporter (Phase 13). KBUF_IOCEXPORT wraps the device's mmap data
 * ring (the same KBUF_MMAP_CAPACITY-byte buffer the mmap zero-copy ring uses)
 * in a dma-buf and writes the new fd back through @arg. The fd can be mmap'd,
 * shared with another driver, or imported for DMA; it keeps the ring alive even
 * after the originating /dev/kbufN fd is closed.
 *
 * KBUF_IOCIMPORT runs the kernel-side importer self-test on a dma-buf fd passed
 * in through @arg (attach + map sg_table + vmap + read-back). It returns 0 when
 * the whole importer path succeeds, letting a userspace test exercise code no
 * userspace-only path can reach.
 */
#define KBUF_IOCEXPORT	_IOR(KBUF_IOC_MAGIC, 5, int)	/* out: dma-buf fd  */
#define KBUF_IOCIMPORT	_IOW(KBUF_IOC_MAGIC, 6, int)	/* in:  dma-buf fd  */

#define KBUF_IOC_MAXNR 6

/*
 * Control device (/dev/kbuf-ctl) — create and destroy kbuf devices at runtime.
 * CREATE returns the new device id N (its node is /dev/kbufdN); DESTROY takes
 * an id. A device destroyed while still open stays alive until its last close
 * (kref), but accepts no new opens.
 */
#define KBUF_CTL_MAGIC 'K'
#define KBUF_CTL_CREATE  _IOR(KBUF_CTL_MAGIC, 1, int)	/* out: new device id  */
#define KBUF_CTL_DESTROY _IOW(KBUF_CTL_MAGIC, 2, int)	/* in:  device id      */

#endif /* _UAPI_KBUF_H */
