/*
 * test_poll.c — exercises poll()/epoll readiness reporting on /dev/kbuf.
 *
 * Self-contained and deterministic (uses a zero/short timeout to sample the
 * current readiness), so it is safe to run unattended in the QEMU harness.
 *
 * Checks, with an empty -> 1 msg -> full -> drained progression:
 *   - empty   : writable, not readable
 *   - 1 slot  : readable and writable
 *   - full    : readable, not writable
 *   - drained : writable, not readable
 *   - epoll   : epoll_wait reports EPOLLIN once data is present
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>

#define DEVICE      "/dev/kbuf"
#define NUM_BUFFERS 8	/* must match KBUF_NUM_BUFFERS in the module */

static int failures;

/* Sample current readiness with a non-blocking poll (timeout 0). */
static short poll_now(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
	int n = poll(&pfd, 1, 0);

	if (n < 0) {
		perror("  poll");
		return 0;
	}
	return pfd.revents;
}

static void check(const char *what, int cond)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

int main(void)
{
	char buf[64];
	short r;
	int fd, i;

	fd = open(DEVICE, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* Drain anything left over so we start from a known empty ring. */
	while (read(fd, buf, sizeof(buf)) > 0)
		;

	printf("=== Test 1: empty ring ===\n");
	r = poll_now(fd);
	check("writable (POLLOUT set)", r & POLLOUT);
	check("not readable (POLLIN clear)", !(r & POLLIN));

	printf("=== Test 2: one message queued ===\n");
	if (write(fd, "hello", 5) != 5)
		perror("  write");
	r = poll_now(fd);
	check("readable (POLLIN set)", r & POLLIN);
	check("still writable (POLLOUT set)", r & POLLOUT);

	printf("=== Test 3: ring full ===\n");
	/* One slot already used; fill the rest. */
	for (i = 1; i < NUM_BUFFERS; i++) {
		int len = snprintf(buf, sizeof(buf), "fill-%d", i);

		if (write(fd, buf, len) != len) {
			perror("  write");
			break;
		}
	}
	r = poll_now(fd);
	check("readable (POLLIN set)", r & POLLIN);
	check("not writable (POLLOUT clear)", !(r & POLLOUT));

	printf("=== Test 4: epoll reports EPOLLIN with data present ===\n");
	{
		int ep = epoll_create1(0);
		struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
		struct epoll_event out;
		int n;

		if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
			perror("  epoll setup");
			failures++;
		} else {
			n = epoll_wait(ep, &out, 1, 100);
			check("epoll_wait returned one ready fd", n == 1);
			check("EPOLLIN reported", n == 1 && (out.events & EPOLLIN));
			close(ep);
		}
	}

	printf("=== Test 5: drained ring ===\n");
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	r = poll_now(fd);
	check("writable (POLLOUT set)", r & POLLOUT);
	check("not readable (POLLIN clear)", !(r & POLLIN));

	close(fd);

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
