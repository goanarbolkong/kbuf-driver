/*
 * test_spsc.c — stress test for the lock-free SPSC mode.
 *
 * Switches /dev/kbuf0 to SPSC mode, then forks a producer and a consumer
 * pinned to different CPUs. The producer writes N fixed-size messages, each
 * carrying its sequence number and a sequence-derived byte pattern; the
 * consumer reads them with blocking I/O (exercising the wait-queue fallback)
 * and verifies strict FIFO order and byte-for-byte payload integrity. With an
 * 8-slot ring and N in the tens of thousands this drives many full/empty
 * transitions across the two cores.
 *
 * Restores blocking mode before exiting so later tests see a clean device.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "kbuf.h"

#define DEVICE   "/dev/kbuf0"
#define MSG_SIZE 64

static void fill_msg(unsigned char *buf, uint32_t seq)
{
	int j;

	memcpy(buf, &seq, sizeof(seq));
	for (j = sizeof(seq); j < MSG_SIZE; j++)
		buf[j] = (unsigned char)(seq + j);
}

/* Returns 0 if @buf is the well-formed message for @seq, -1 otherwise. */
static int verify_msg(const unsigned char *buf, uint32_t seq)
{
	uint32_t got;
	int j;

	memcpy(&got, buf, sizeof(got));
	if (got != seq)
		return -1;
	for (j = sizeof(got); j < MSG_SIZE; j++)
		if (buf[j] != (unsigned char)(seq + j))
			return -1;
	return 0;
}

static void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		fprintf(stderr, "  note: could not pin to CPU %d (%s)\n",
			cpu, strerror(errno));
}

static int producer(uint32_t n)
{
	unsigned char buf[MSG_SIZE];
	uint32_t seq;
	int fd;

	pin_to_cpu(0);
	fd = open(DEVICE, O_WRONLY);	/* blocking */
	if (fd < 0) {
		perror("  producer open");
		return 1;
	}
	for (seq = 0; seq < n; seq++) {
		ssize_t w;

		fill_msg(buf, seq);
		w = write(fd, buf, MSG_SIZE);
		if (w != MSG_SIZE) {
			fprintf(stderr, "  producer write seq=%u returned %zd (%s)\n",
				seq, w, strerror(errno));
			close(fd);
			return 1;
		}
	}
	close(fd);
	return 0;
}

static int consumer(uint32_t n)
{
	unsigned char buf[MSG_SIZE];
	uint32_t seq;
	int fd, bad = 0;

	pin_to_cpu(1);
	fd = open(DEVICE, O_RDONLY);	/* blocking */
	if (fd < 0) {
		perror("  consumer open");
		return 1;
	}
	for (seq = 0; seq < n; seq++) {
		ssize_t r = read(fd, buf, MSG_SIZE);

		if (r != MSG_SIZE) {
			fprintf(stderr, "  consumer read seq=%u returned %zd (%s)\n",
				seq, r, strerror(errno));
			bad++;
			break;
		}
		if (verify_msg(buf, seq) != 0) {
			fprintf(stderr, "  corruption/reorder at seq=%u\n", seq);
			bad++;
			if (bad > 5)
				break;
		}
	}
	close(fd);
	return bad ? 1 : 0;
}

int main(int argc, char *argv[])
{
	uint32_t n = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 20000;
	struct kbuf_stats st;
	char drainbuf[MSG_SIZE];
	int ctrl, mode, status, prod_rc, cons_rc = 0, failures = 0;
	pid_t pid;

	printf("=== SPSC stress: %u messages of %d bytes ===\n", n, MSG_SIZE);

	ctrl = open(DEVICE, O_RDWR | O_NONBLOCK);
	if (ctrl < 0) {
		perror("open");
		return 1;
	}
	while (read(ctrl, drainbuf, sizeof(drainbuf)) > 0)	/* start empty */
		;
	ioctl(ctrl, KBUF_IOCRESET);

	mode = KBUF_MODE_SPSC;
	if (ioctl(ctrl, KBUF_IOCSMODE, &mode) != 0) {
		perror("  SMODE SPSC");
		close(ctrl);
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(ctrl);
		return 1;
	}
	if (pid == 0) {
		/* child: producer */
		_exit(producer(n));
	}

	/* parent: consumer */
	cons_rc = consumer(n);

	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		failures++;
	}
	prod_rc = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;

	printf("  [%s] producer completed\n", prod_rc ? "FAIL" : "PASS");
	printf("  [%s] consumer verified all %u messages in order\n",
	       cons_rc ? "FAIL" : "PASS", n);
	failures += prod_rc + cons_rc;

	if (ioctl(ctrl, KBUF_IOCGSTATS, &st) == 0) {
		printf("  [%s] stats: produced=%llu consumed=%llu peak=%u\n",
		       (st.msgs_produced == n && st.msgs_consumed == n) ? "PASS" : "FAIL",
		       (unsigned long long)st.msgs_produced,
		       (unsigned long long)st.msgs_consumed, st.peak_count);
		if (st.msgs_produced != n || st.msgs_consumed != n)
			failures++;
	}

	/* Restore blocking mode for subsequent tests (ring is empty now). */
	mode = KBUF_MODE_BLOCKING;
	if (ioctl(ctrl, KBUF_IOCSMODE, &mode) != 0)
		perror("  restore SMODE");
	close(ctrl);

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
