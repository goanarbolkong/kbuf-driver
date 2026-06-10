/*
 * test_mmap.c — stress test for the mmap zero-copy ring (libkbuf).
 *
 * Maps /dev/kbuf0, then forks a producer and consumer pinned to different CPUs
 * that move a multi-megabyte deterministic byte stream through the 64 KiB ring
 * using only user-space atomics (no syscalls on the data path). Byte k of the
 * stream is (unsigned char)k, so the consumer can verify every byte by its
 * absolute position — catching any loss, reorder, or wrap miscopy. With the
 * default 4 MiB transfer the stream wraps the ring ~64 times, exercising the
 * "magic" double mapping that keeps each memcpy contiguous.
 *
 * The mmap is established and reset before fork(); MAP_SHARED means the child
 * inherits the same physical ring.
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
#include <sys/wait.h>

#include "kbuf.h"
#include "libkbuf.h"

#define DEVICE "/dev/kbuf0"
#define CHUNK  4096

static void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		fprintf(stderr, "  note: could not pin to CPU %d (%s)\n",
			cpu, strerror(errno));
}

static int producer(struct kbuf_map *m, uint64_t total)
{
	unsigned char chunk[CHUNK];
	uint64_t done = 0;

	pin_to_cpu(0);
	while (done < total) {
		size_t want = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
		size_t j, w;

		for (j = 0; j < want; j++)
			chunk[j] = (unsigned char)(done + j);
		w = kbuf_map_write(m, chunk, want);
		if (w == 0)
			sched_yield();	/* ring full — let the consumer run */
		done += w;
	}
	return 0;
}

static int consumer(struct kbuf_map *m, uint64_t total)
{
	unsigned char chunk[CHUNK];
	uint64_t done = 0;
	int bad = 0;

	pin_to_cpu(1);
	while (done < total) {
		size_t r = kbuf_map_read(m, chunk, CHUNK);
		size_t j;

		if (r == 0) {
			sched_yield();	/* ring empty — let the producer run */
			continue;
		}
		for (j = 0; j < r; j++) {
			if (chunk[j] != (unsigned char)(done + j)) {
				fprintf(stderr, "  mismatch at byte %llu\n",
					(unsigned long long)(done + j));
				bad++;
				break;
			}
		}
		if (bad)
			break;
		done += r;
	}
	return (bad || done != total) ? 1 : 0;
}

int main(int argc, char *argv[])
{
	uint64_t total = (argc > 1) ? strtoull(argv[1], NULL, 10) : (4ULL << 20);
	struct kbuf_map m;
	int fd, status, prod_rc, cons_rc, failures = 0;
	pid_t pid;

	printf("=== mmap zero-copy stress: %llu bytes, %u-byte ring ===\n",
	       (unsigned long long)total, KBUF_MMAP_CAPACITY);

	fd = open(DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (kbuf_map_open(fd, &m) != 0) {
		perror("mmap");
		close(fd);
		return 1;
	}
	kbuf_map_reset(&m);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0)
		_exit(producer(&m, total));

	cons_rc = consumer(&m, total);
	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		failures++;
	}
	prod_rc = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;

	printf("  [%s] producer completed\n", prod_rc ? "FAIL" : "PASS");
	printf("  [%s] consumer verified %llu bytes (sequence + integrity)\n",
	       cons_rc ? "FAIL" : "PASS", (unsigned long long)total);
	failures += prod_rc + cons_rc;

	kbuf_map_close(&m);
	close(fd);

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
