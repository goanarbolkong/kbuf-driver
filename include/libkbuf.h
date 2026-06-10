/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libkbuf.h - user-space helper for the kbuf mmap zero-copy ring.
 *
 * Header-only. Maps a /dev/kbufN device's control page + double-mapped data
 * ring and provides single-producer / single-consumer byte transfer entirely
 * in user space — no syscall on the fast path. head/tail are exchanged with
 * acquire/release atomics (the C11 memory model, via the GCC/Clang __atomic
 * builtins, which operate directly on the shared plain __u64 fields).
 *
 * Contract: exactly one producer (kbuf_map_write) and one consumer
 * (kbuf_map_read), matching the kernel SPSC discipline. The double mapping
 * makes every transfer a single contiguous memcpy even across the wrap.
 */
#ifndef _LIBKBUF_H
#define _LIBKBUF_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "kbuf.h"

struct kbuf_map {
	unsigned char         *base;	/* start of the mmap region (ctrl page) */
	struct kbuf_mmap_ctrl *ctrl;
	unsigned char         *data;	/* first copy of the data ring          */
	size_t                 cap;	/* ring capacity in bytes (power of two)*/
	size_t                 maplen;
};

/* Map the device. Returns 0 on success, -1 on failure (errno set by mmap). */
static inline int kbuf_map_open(int fd, struct kbuf_map *m)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t cap = KBUF_MMAP_CAPACITY;
	size_t len = (size_t)pg + 2 * cap;
	void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (base == MAP_FAILED)
		return -1;
	m->base = (unsigned char *)base;
	m->ctrl = (struct kbuf_mmap_ctrl *)base;
	m->data = (unsigned char *)base + pg;
	m->cap = cap;
	m->maplen = len;
	return 0;
}

static inline void kbuf_map_close(struct kbuf_map *m)
{
	if (m->base)
		munmap(m->base, m->maplen);
	m->base = NULL;
}

/* Reset the ring to empty. Call once, before the producer/consumer start. */
static inline void kbuf_map_reset(struct kbuf_map *m)
{
	__atomic_store_n(&m->ctrl->head, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&m->ctrl->tail, 0, __ATOMIC_RELAXED);
}

/* SPSC producer: copy up to n bytes into the ring; returns bytes written. */
static inline size_t kbuf_map_write(struct kbuf_map *m, const void *buf, size_t n)
{
	uint64_t head = __atomic_load_n(&m->ctrl->head, __ATOMIC_RELAXED);
	uint64_t tail = __atomic_load_n(&m->ctrl->tail, __ATOMIC_ACQUIRE);
	size_t space = m->cap - (size_t)(head - tail);
	size_t off;

	if (n > space)
		n = space;
	if (n == 0)
		return 0;
	off = (size_t)(head & (m->cap - 1));
	memcpy(m->data + off, buf, n);	/* contiguous thanks to the double map */
	__atomic_store_n(&m->ctrl->head, head + n, __ATOMIC_RELEASE);
	return n;
}

/* SPSC consumer: copy up to n bytes out of the ring; returns bytes read. */
static inline size_t kbuf_map_read(struct kbuf_map *m, void *buf, size_t n)
{
	uint64_t tail = __atomic_load_n(&m->ctrl->tail, __ATOMIC_RELAXED);
	uint64_t head = __atomic_load_n(&m->ctrl->head, __ATOMIC_ACQUIRE);
	size_t avail = (size_t)(head - tail);
	size_t off;

	if (n > avail)
		n = avail;
	if (n == 0)
		return 0;
	off = (size_t)(tail & (m->cap - 1));
	memcpy(buf, m->data + off, n);
	__atomic_store_n(&m->ctrl->tail, tail + n, __ATOMIC_RELEASE);
	return n;
}

#endif /* _LIBKBUF_H */
