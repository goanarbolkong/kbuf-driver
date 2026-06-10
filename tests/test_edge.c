/*
 * test_edge.c — edge-case behaviour of the blocking slot interface.
 *
 * Self-contained; leaves /dev/kbuf0 empty.
 *
 *   1. Partial read: a read() smaller than the queued message returns just the
 *      requested bytes and consumes the whole slot (datagram semantics) — the
 *      next read on the drained ring blocks/EAGAINs.
 *   2. Signal interruption: a blocking read() interrupted by a signal returns
 *      -1/EINTR rather than restarting or hanging (the -ERESTARTSYS path).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define DEVICE "/dev/kbuf0"

static int failures;

static void check(const char *what, int cond)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

static void drain(int fd)
{
	char buf[4096];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

static void noop_handler(int sig)
{
	(void)sig;
}

static int test_partial_read(void)
{
	char wbuf[100], rbuf[100];
	ssize_t n;
	int fd, i;

	printf("=== Test 1: partial read consumes the whole slot ===\n");
	fd = open(DEVICE, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("  open");
		return 1;
	}
	drain(fd);

	for (i = 0; i < 100; i++)
		wbuf[i] = (char)i;
	check("write 100-byte message", write(fd, wbuf, 100) == 100);

	n = read(fd, rbuf, 10);		/* ask for only 10 of 100 */
	check("short read returns 10 bytes", n == 10);
	check("those 10 bytes are correct", n == 10 && memcmp(rbuf, wbuf, 10) == 0);

	errno = 0;
	n = read(fd, rbuf, sizeof(rbuf));
	check("slot fully consumed (next read EAGAIN)", n == -1 && errno == EAGAIN);

	close(fd);
	return 0;
}

static int test_signal_interrupt(void)
{
	int fd, status;
	pid_t pid;

	printf("=== Test 2: blocking read interrupted by a signal ===\n");
	fd = open(DEVICE, O_RDWR | O_NONBLOCK);
	if (fd >= 0) {
		drain(fd);		/* make sure the ring is empty */
		close(fd);
	}

	pid = fork();
	if (pid < 0) {
		perror("  fork");
		return 1;
	}
	if (pid == 0) {
		struct sigaction sa = { 0 };
		char buf[16];
		ssize_t r;

		/* No SA_RESTART: the syscall must report EINTR, not restart. */
		sa.sa_handler = noop_handler;
		sigaction(SIGUSR1, &sa, NULL);

		int cfd = open(DEVICE, O_RDONLY);	/* blocking */

		if (cfd < 0)
			_exit(2);
		r = read(cfd, buf, sizeof(buf));	/* blocks: ring empty */
		_exit((r == -1 && errno == EINTR) ? 0 : 1);
	}

	usleep(300 * 1000);		/* let the child enter read() */
	kill(pid, SIGUSR1);
	waitpid(pid, &status, 0);

	check("interrupted read returned EINTR",
	      WIFEXITED(status) && WEXITSTATUS(status) == 0);
	return 0;
}

int main(void)
{
	test_partial_read();
	test_signal_interrupt();

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
