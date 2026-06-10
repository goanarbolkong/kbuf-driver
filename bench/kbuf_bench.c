/*
 * kbuf_bench.c — throughput: mmap zero-copy ring vs read()/write() syscalls.
 *
 * Transfers the same number of bytes through (a) the mmap ring on /dev/kbuf0
 * via libkbuf and (b) the slot ring on /dev/kbuf1 via blocking read()/write(),
 * with a producer and consumer pinned to different CPUs, and prints MB/s for
 * each plus the speedup.
 *
 * This is a quick comparison, not the rigorous report — Phase 9 (docs/
 * BENCHMARKS.md) adds methodology (governor pinning, repeated runs, error
 * bars). Numbers from inside a VM are illustrative only.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <sys/wait.h>

#include "kbuf.h"
#include "libkbuf.h"

#define CHUNK 4096

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

/* ---- mmap path ---- */
static double bench_mmap(uint64_t total)
{
	struct kbuf_map m;
	unsigned char buf[CHUNK];
	uint64_t done = 0;
	double t0;
	int fd;
	pid_t pid;

	fd = open("/dev/kbuf0", O_RDWR);
	if (fd < 0 || kbuf_map_open(fd, &m) != 0) {
		perror("mmap setup");
		return -1;
	}
	kbuf_map_reset(&m);

	t0 = now_sec();
	pid = fork();
	if (pid == 0) {
		pin_to_cpu(0);
		memset(buf, 0xa5, sizeof(buf));
		while (done < total) {
			size_t w = kbuf_map_write(&m, buf, CHUNK);

			if (!w)
				sched_yield();
			done += w;
		}
		_exit(0);
	}
	pin_to_cpu(1);
	while (done < total) {
		size_t r = kbuf_map_read(&m, buf, CHUNK);

		if (!r)
			sched_yield();
		done += r;
	}
	waitpid(pid, NULL, 0);

	kbuf_map_close(&m);
	close(fd);
	return now_sec() - t0;
}

/* ---- syscall path ---- */
static double bench_syscall(uint64_t total)
{
	uint64_t msgs = total / CHUNK;
	double t0;
	pid_t pid;

	t0 = now_sec();
	pid = fork();
	if (pid == 0) {
		unsigned char buf[CHUNK];
		uint64_t i;
		int fd = open("/dev/kbuf1", O_WRONLY);

		pin_to_cpu(0);
		memset(buf, 0xa5, sizeof(buf));
		for (i = 0; i < msgs; i++)
			if (write(fd, buf, CHUNK) != CHUNK)
				break;
		close(fd);
		_exit(0);
	}
	unsigned char buf[CHUNK];
	uint64_t i;
	int fd = open("/dev/kbuf1", O_RDONLY);

	pin_to_cpu(1);
	for (i = 0; i < msgs; i++)
		if (read(fd, buf, CHUNK) != CHUNK)
			break;
	close(fd);
	waitpid(pid, NULL, 0);
	return now_sec() - t0;
}

int main(int argc, char *argv[])
{
	uint64_t total = (argc > 1) ? strtoull(argv[1], NULL, 10) : (64ULL << 20);
	double mb = total / (1024.0 * 1024.0);
	double tm, ts;

	printf("=== kbuf throughput: %.0f MiB, %u-byte chunks ===\n", mb, CHUNK);

	tm = bench_mmap(total);
	if (tm < 0)
		return 1;
	printf("  mmap zero-copy : %7.1f MB/s  (%.3f s)\n", mb / tm, tm);

	ts = bench_syscall(total);
	printf("  read()/write() : %7.1f MB/s  (%.3f s)\n", mb / ts, ts);

	printf("  speedup (mmap / syscall): %.2fx\n", ts / tm);
	return 0;
}
