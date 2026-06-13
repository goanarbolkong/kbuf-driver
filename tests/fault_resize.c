// SPDX-License-Identifier: GPL-2.0
/*
 * fault_resize - hammer KBUF_IOCRESIZE so the ring allocation path runs over
 * and over. Intended to be driven under slab fault injection (failslab): the
 * kernel-side kcalloc()/kmalloc() in kbuf_alloc_slots() then fails on a
 * fraction of calls and the driver must unwind cleanly (no leak, no oops,
 * no corrupted geometry) and return -ENOMEM to user space.
 *
 * The process opts itself into the fault-injection task filter by writing
 * /proc/self/make-it-fail, so only this task's allocations are perturbed and
 * the rest of the throwaway guest (shell, busybox) keeps running. When the
 * kernel lacks CONFIG_FAILSLAB / make-it-fail, the program still runs as a
 * plain resize stress test and a final functional check.
 *
 * Exit status: 0 if the device is still fully functional afterwards (a write
 * followed by a matching read succeeds), non-zero otherwise. Observing zero
 * injected failures is not itself a failure - the host-side gate relies on the
 * kernel staying silent (no KASAN/BUG report) across the run.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "kbuf.h"

#define DEV "/dev/kbuf0"

static int set_make_it_fail(int on)
{
	int fd = open("/proc/self/make-it-fail", O_WRONLY);

	if (fd < 0)
		return -1;	/* kernel without fault-injection task filter */
	if (write(fd, on ? "1\n" : "0\n", 2) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 2000;
	unsigned long ok = 0, enomem = 0, ebusy = 0, other = 0;
	struct kbuf_resize geo[2] = {
		{ .num_buffers = 8,  .buffer_size = 4096 },
		{ .num_buffers = 16, .buffer_size = 8192 },
	};
	int have_filter;
	int fd, i;

	fd = open(DEV, O_RDWR);
	if (fd < 0) {
		perror("open " DEV);
		return 2;
	}

	have_filter = set_make_it_fail(1) == 0;
	printf("fault_resize: iters=%lu task-filter=%s\n",
	       iters, have_filter ? "on" : "absent");

	for (i = 0; (unsigned long)i < iters; i++) {
		if (ioctl(fd, KBUF_IOCRESIZE, &geo[i & 1]) == 0) {
			ok++;
			continue;
		}
		switch (errno) {
		case ENOMEM:	/* injected slab allocation failure */
			enomem++;
			break;
		case EBUSY:	/* ring went non-empty under a writer */
			ebusy++;
			break;
		default:
			other++;
			break;
		}
	}

	/* Stop perturbing allocations before the functional check below. */
	if (have_filter)
		set_make_it_fail(0);

	printf("fault_resize: ok=%lu enomem=%lu ebusy=%lu other=%lu\n",
	       ok, enomem, ebusy, other);

	if (other) {
		fprintf(stderr, "fault_resize: unexpected ioctl errno seen\n");
		close(fd);
		return 3;
	}

	/* The device must still work after all that allocation churn. */
	if (ioctl(fd, KBUF_IOCRESIZE, &geo[0]) != 0) {
		perror("final resize");
		close(fd);
		return 4;
	}
	const char *msg = "post-faultinjection-roundtrip";
	char back[64] = {0};
	ssize_t n;

	if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg)) {
		perror("write");
		close(fd);
		return 5;
	}
	n = read(fd, back, sizeof(back));
	if (n != (ssize_t)strlen(msg) || memcmp(msg, back, n) != 0) {
		fprintf(stderr, "fault_resize: round-trip mismatch (n=%zd)\n", n);
		close(fd);
		return 6;
	}

	printf("fault_resize: device functional after fault injection\n");
	close(fd);
	return 0;
}
