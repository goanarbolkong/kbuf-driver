/*
 * test_ioctl.c — exercises the kbuf ioctl UAPI.
 *
 * Self-contained and deterministic, safe to run unattended in the QEMU
 * harness. It RESTORES the default geometry (8 x 4096) and drains the ring
 * before exiting, so later tests in the same boot see a clean device.
 *
 * Covers: GSTATS counters after a known produce/consume, RESET, RESIZE on an
 * empty ring, RESIZE refused (-EBUSY) on a non-empty ring, SMODE validation
 * (blocking OK, SPSC -EOPNOTSUPP, bad value -EINVAL), and an unknown ioctl
 * (-ENOTTY).
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "kbuf.h"

#define DEVICE          "/dev/kbuf0"
#define DEFAULT_NBUF    8
#define DEFAULT_BUFSIZE 4096

static int failures;

static void check(const char *what, int cond)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

/* Drain the ring via non-blocking reads. */
static void drain(int fd)
{
	char buf[256];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

int main(void)
{
	struct kbuf_stats st;
	struct kbuf_resize rz;
	char buf[64];
	int fd, mode, i;

	fd = open(DEVICE, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	drain(fd);

	printf("=== Test 1: RESET then GSTATS on idle ring ===\n");
	check("KBUF_IOCRESET ok", ioctl(fd, KBUF_IOCRESET) == 0);
	check("KBUF_IOCGSTATS ok", ioctl(fd, KBUF_IOCGSTATS, &st) == 0);
	check("counters zeroed", st.msgs_produced == 0 && st.msgs_consumed == 0 &&
	      st.bytes_produced == 0 && st.bytes_consumed == 0);
	check("geometry is default", st.num_buffers == DEFAULT_NBUF &&
	      st.buffer_size == DEFAULT_BUFSIZE);
	check("ring empty", st.cur_count == 0);

	printf("=== Test 2: counters track a 3-message round trip ===\n");
	for (i = 0; i < 3; i++)
		if (write(fd, "abcd", 4) != 4)
			perror("  write");
	if (ioctl(fd, KBUF_IOCGSTATS, &st) == 0) {
		check("msgs_produced == 3", st.msgs_produced == 3);
		check("bytes_produced == 12", st.bytes_produced == 12);
		check("cur_count == 3", st.cur_count == 3);
		check("peak_count == 3", st.peak_count == 3);
	} else {
		perror("  GSTATS");
		failures++;
	}
	for (i = 0; i < 3; i++)
		if (read(fd, buf, sizeof(buf)) != 4)
			perror("  read");
	if (ioctl(fd, KBUF_IOCGSTATS, &st) == 0) {
		check("msgs_consumed == 3", st.msgs_consumed == 3);
		check("bytes_consumed == 12", st.bytes_consumed == 12);
		check("cur_count == 0 after drain", st.cur_count == 0);
		check("peak_count stays 3", st.peak_count == 3);
	}

	printf("=== Test 3: RESIZE on empty ring ===\n");
	rz.num_buffers = 4;
	rz.buffer_size = 1024;
	check("RESIZE to 4x1024 ok", ioctl(fd, KBUF_IOCRESIZE, &rz) == 0);
	if (ioctl(fd, KBUF_IOCGSTATS, &st) == 0)
		check("geometry now 4x1024", st.num_buffers == 4 && st.buffer_size == 1024);

	printf("=== Test 4: RESIZE refused on non-empty ring ===\n");
	if (write(fd, "x", 1) != 1)
		perror("  write");
	rz.num_buffers = 16;
	rz.buffer_size = 4096;
	errno = 0;
	check("RESIZE returns -1", ioctl(fd, KBUF_IOCRESIZE, &rz) == -1);
	check("errno == EBUSY", errno == EBUSY);
	drain(fd);

	printf("=== Test 5: RESIZE bounds checking ===\n");
	rz.num_buffers = 0;
	rz.buffer_size = 4096;
	errno = 0;
	check("0 slots rejected (EINVAL)", ioctl(fd, KBUF_IOCRESIZE, &rz) == -1 &&
	      errno == EINVAL);

	printf("=== Test 6: SMODE (blocking <-> spsc) ===\n");
	/* Geometry here is 4x1024 (from Test 3), a power of two. */
	mode = KBUF_MODE_BLOCKING;
	check("set blocking ok", ioctl(fd, KBUF_IOCSMODE, &mode) == 0);
	mode = KBUF_MODE_SPSC;
	check("set SPSC ok (empty, pow2)", ioctl(fd, KBUF_IOCSMODE, &mode) == 0);
	mode = KBUF_MODE_BLOCKING;
	check("back to blocking ok", ioctl(fd, KBUF_IOCSMODE, &mode) == 0);
	mode = 99;
	errno = 0;
	check("bad mode rejected (EINVAL)",
	      ioctl(fd, KBUF_IOCSMODE, &mode) == -1 && errno == EINVAL);
	/* SPSC needs a power-of-two capacity. */
	rz.num_buffers = 3;
	rz.buffer_size = 256;
	check("resize to 3x256 (blocking) ok", ioctl(fd, KBUF_IOCRESIZE, &rz) == 0);
	mode = KBUF_MODE_SPSC;
	errno = 0;
	check("SPSC rejected on non-pow2 (EINVAL)",
	      ioctl(fd, KBUF_IOCSMODE, &mode) == -1 && errno == EINVAL);

	printf("=== Test 7: unknown ioctl ===\n");
	errno = 0;
	check("bad command (ENOTTY)", ioctl(fd, _IO('Z', 1)) == -1 && errno == ENOTTY);

	/* Restore defaults so later tests in this boot see a clean 8x4096 ring. */
	drain(fd);
	rz.num_buffers = DEFAULT_NBUF;
	rz.buffer_size = DEFAULT_BUFSIZE;
	if (ioctl(fd, KBUF_IOCRESIZE, &rz) != 0)
		perror("  restore RESIZE");

	close(fd);

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
