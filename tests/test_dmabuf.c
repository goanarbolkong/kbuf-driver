/*
 * test_dmabuf.c — exercise the dma-buf exporter (KBUF_IOCEXPORT / KBUF_IOCIMPORT).
 *
 * The dma-buf wraps the very same pages as the mmap zero-copy ring, so this
 * test proves the two views alias one buffer and that the kernel-side importer
 * path (attach + map sg_table + vmap) works end to end:
 *
 *   1. mmap the device's data ring and stamp a magic word into word[0];
 *   2. export a dma-buf fd from the same device;
 *   3. run the in-kernel importer self-test (KBUF_IOCIMPORT): it attaches a
 *      DMA device, maps the sg_table, vmaps the buffer, reads word[0] and
 *      echoes it into word[1] — so finding MAGIC in word[1] through the device
 *      mmap proves the importer mapped the exact same pages (both directions);
 *   4. confirm the dma-buf outlives the device fd that created it.
 *
 * Note: we deliberately do not mmap the dma-buf fd from userspace. On 6.17 that
 * trips a benign path_noexec WARN on the dma-buf pseudo-mount (see
 * docs/DEBUGGING.md); the in-kernel importer reaches the same pages and proves
 * the same property without it. Returns 0 only if every check passes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "kbuf.h"
#include "libkbuf.h"

#define DEVICE "/dev/kbuf0"
#define MAGIC  0xCAFEF00Du

static int check(int cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	return cond ? 0 : 1;
}

int main(void)
{
	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
	struct kbuf_map m;
	uint32_t *words;
	int fd, dbuf_fd, fails = 0;

	printf("=== dma-buf exporter: %u-byte ring ===\n", KBUF_MMAP_CAPACITY);

	fd = open(DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (kbuf_map_open(fd, &m) != 0) {
		perror("mmap(device)");
		close(fd);
		return 1;
	}

	/* Stamp a magic word into word[0] and clear the importer's echo slot. */
	words = (uint32_t *)m.data;
	words[0] = MAGIC;
	words[1] = 0;

	dbuf_fd = -1;
	if (ioctl(fd, KBUF_IOCEXPORT, &dbuf_fd) != 0) {
		perror("KBUF_IOCEXPORT");
		kbuf_map_close(&m);
		close(fd);
		return 1;
	}
	fails += check(dbuf_fd >= 0, "KBUF_IOCEXPORT returned a dma-buf fd");

	/* In-kernel importer: attach + map sg_table + vmap + echo word[0]->word[1]. */
	fails += check(ioctl(fd, KBUF_IOCIMPORT, &dbuf_fd) == 0,
		       "KBUF_IOCIMPORT self-test (attach/map/vmap) succeeded");
	fails += check(words[1] == MAGIC,
		       "importer mapped the same ring pages (echo matches)");

	/* The CPU-access protocol is a no-op on coherent RAM but must succeed. */
	fails += check(ioctl(dbuf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
		       "DMA_BUF_IOCTL_SYNC start accepted");
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
	ioctl(dbuf_fd, DMA_BUF_IOCTL_SYNC, &sync);

	/* The dma-buf outlives the device fd that created it. */
	kbuf_map_close(&m);
	close(fd);
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
	fails += check(ioctl(dbuf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
		       "dma-buf still usable after device fd closed");

	close(dbuf_fd);

	printf("\n%s (%d failure%s)\n", fails ? "RESULT: FAIL" : "RESULT: PASS",
	       fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
