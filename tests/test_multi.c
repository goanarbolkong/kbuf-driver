/*
 * test_multi.c — verifies that /dev/kbuf0 and /dev/kbuf1 are independent.
 *
 * Self-contained and deterministic, safe to run unattended in the QEMU
 * harness. Requires the module to expose at least two devices (default
 * ndevices=4). Leaves both rings empty and at default geometry on exit.
 *
 * Covers: data written to one device is invisible to the other; reads come
 * back from the right device; and per-device geometry (KBUF_IOCRESIZE on
 * kbuf0 does not affect kbuf1).
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "kbuf.h"

#define DEV0            "/dev/kbuf0"
#define DEV1            "/dev/kbuf1"
#define DEFAULT_NBUF    8
#define DEFAULT_BUFSIZE 4096

static int failures;

static void check(const char *what, int cond)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

static void drain(int fd)
{
	char buf[256];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

int main(void)
{
	struct kbuf_stats st0, st1;
	struct kbuf_resize rz;
	char buf[64];
	ssize_t n;
	int fd0, fd1;

	fd0 = open(DEV0, O_RDWR | O_NONBLOCK);
	fd1 = open(DEV1, O_RDWR | O_NONBLOCK);
	if (fd0 < 0 || fd1 < 0) {
		perror("open");
		return 1;
	}
	drain(fd0);
	drain(fd1);

	printf("=== Test 1: data on kbuf0 is invisible to kbuf1 ===\n");
	check("write 5 bytes to kbuf0", write(fd0, "hello", 5) == 5);
	errno = 0;
	n = read(fd1, buf, sizeof(buf));
	check("kbuf1 read returns EAGAIN (still empty)", n == -1 && errno == EAGAIN);
	n = read(fd0, buf, sizeof(buf));
	check("kbuf0 read returns the 5 bytes", n == 5);
	check("kbuf0 data is correct", n == 5 && memcmp(buf, "hello", 5) == 0);

	printf("=== Test 2: per-device counters are independent ===\n");
	if (ioctl(fd0, KBUF_IOCGSTATS, &st0) == 0 &&
	    ioctl(fd1, KBUF_IOCGSTATS, &st1) == 0) {
		check("kbuf0 produced >= 1", st0.msgs_produced >= 1);
		check("kbuf1 produced == 0", st1.msgs_produced == 0);
	} else {
		perror("  GSTATS");
		failures++;
	}

	printf("=== Test 3: resizing kbuf0 does not affect kbuf1 ===\n");
	rz.num_buffers = 2;
	rz.buffer_size = 512;
	check("resize kbuf0 to 2x512", ioctl(fd0, KBUF_IOCRESIZE, &rz) == 0);
	if (ioctl(fd0, KBUF_IOCGSTATS, &st0) == 0 &&
	    ioctl(fd1, KBUF_IOCGSTATS, &st1) == 0) {
		check("kbuf0 geometry now 2x512",
		      st0.num_buffers == 2 && st0.buffer_size == 512);
		check("kbuf1 geometry still default",
		      st1.num_buffers == DEFAULT_NBUF && st1.buffer_size == DEFAULT_BUFSIZE);
	}

	/* Restore kbuf0 to defaults so later tests see a clean device. */
	drain(fd0);
	rz.num_buffers = DEFAULT_NBUF;
	rz.buffer_size = DEFAULT_BUFSIZE;
	if (ioctl(fd0, KBUF_IOCRESIZE, &rz) != 0)
		perror("  restore RESIZE");

	close(fd0);
	close(fd1);

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
