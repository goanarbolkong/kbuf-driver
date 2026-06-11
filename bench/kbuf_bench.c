/*
 * kbuf_bench.c — throughput, latency, and false-sharing benchmarks.
 *
 * Compares four byte-transfer transports between a producer and a consumer
 * pinned to different CPUs:
 *   mutex  - slot ring on /dev/kbuf1 in blocking mode, via read()/write()
 *   spsc   - slot ring on /dev/kbuf2 in lock-free SPSC mode, via read()/write()
 *   mmap   - mmap zero-copy ring on /dev/kbuf0, via libkbuf (no syscalls)
 *   pipe   - pipe(2), as a kernel baseline
 *
 * Experiments:
 *   throughput   - MB/s vs message size, several runs (mean +/- sample stddev)
 *   latency      - one-way produce->consume latency percentiles (mmap ring)
 *   falsesharing - two cores hammering counters on the same vs separate lines,
 *                  for both plain stores and atomic read-modify-writes
 *
 * Numbers are environment-dependent. The figures in docs/BENCHMARKS.md were
 * gathered on bare metal (i9-12900H, governor performance, two pinned P-cores)
 * via scripts/run-baremetal-bench.sh; see that file for the full methodology and
 * the host-stability caveats.
 *
 * Usage: kbuf_bench [quick|full]   (default: full)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "kbuf.h"
#include "libkbuf.h"

enum { T_MUTEX, T_SPSC, T_MMAP, T_PIPE, T_COUNT };
static const char *t_name[T_COUNT] = { "mutex", "spsc", "mmap", "pipe" };

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/*
 * The two CPUs the producer and consumer are pinned to. They must be distinct
 * PHYSICAL cores: pinning to hyperthread siblings of one core makes the two
 * busy-poll loops contend for that core's execution units and wrecks latency.
 * Defaults are 0 and 1 (fine when those are separate cores, e.g. a 2-vCPU VM);
 * override with KBUF_BENCH_CPU_A / KBUF_BENCH_CPU_B on SMT hardware.
 */
static int cpu_a = 0;
static int cpu_b = 1;

static void pin_cpus_from_env(void)
{
	const char *a = getenv("KBUF_BENCH_CPU_A");
	const char *b = getenv("KBUF_BENCH_CPU_B");

	if (a)
		cpu_a = atoi(a);
	if (b)
		cpu_b = atoi(b);
}

static void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

/*
 * Reduce host-noise in the measurements:
 *   - mlockall pins all pages so a swapping/under-pressure host cannot fault
 *     the busy-poll loops mid-sample;
 *   - with KBUF_BENCH_RT=1, run SCHED_FIFO so the desktop (gnome-shell, etc.)
 *     cannot preempt a pinned producer/consumer and inflate the latency tail.
 *     RT requires privilege (run via sudo) and is inherited across fork().
 * Both are best-effort: on failure we warn and continue with noisier numbers.
 */
static void reduce_noise(void)
{
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fprintf(stderr, "warn: mlockall: %s (paging noise possible)\n",
			strerror(errno));

	if (getenv("KBUF_BENCH_RT")) {
		struct sched_param sp = { .sched_priority = 80 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
			fprintf(stderr,
				"warn: SCHED_FIFO: %s (run via sudo for RT; continuing)\n",
				strerror(errno));
		else
			printf("scheduling: SCHED_FIFO prio 80\n");
	}
}

/*
 * Open and mmap /dev/kbuf0. A failure here used to leave kbuf_map zeroed and
 * crash on the first head/tail dereference; report errno and bail loudly so a
 * mmap problem is diagnosable instead of a null-pointer segfault.
 */
static void map_or_die(struct kbuf_map *m, const char *who)
{
	int fd = open("/dev/kbuf0", O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "%s: open(/dev/kbuf0): %s\n", who, strerror(errno));
		exit(2);
	}
	if (kbuf_map_open(fd, m) != 0) {
		fprintf(stderr, "%s: kbuf_map_open (mmap %zu bytes): %s\n",
			who, (size_t)(sysconf(_SC_PAGESIZE) + 2 * KBUF_MMAP_CAPACITY),
			strerror(errno));
		exit(2);
	}
}

/* Configure a slot-ring device: geometry to fit msgsize, and the ring mode. */
static int slot_setup(const char *dev, unsigned int msgsize, int spsc)
{
	struct kbuf_resize rz = { .num_buffers = 8, .buffer_size = msgsize };
	int mode = spsc ? KBUF_MODE_SPSC : KBUF_MODE_BLOCKING;
	char drain[65536];
	int fd = open(dev, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		return -1;
	mode = KBUF_MODE_BLOCKING;		/* leave SPSC first if set */
	ioctl(fd, KBUF_IOCSMODE, &mode);
	while (read(fd, drain, sizeof(drain)) > 0)
		;
	if (ioctl(fd, KBUF_IOCRESIZE, &rz) != 0) {
		close(fd);
		return -1;
	}
	mode = spsc ? KBUF_MODE_SPSC : KBUF_MODE_BLOCKING;
	if (ioctl(fd, KBUF_IOCSMODE, &mode) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* One throughput run; returns seconds to move @total bytes in @msgsize chunks. */
static double run_once(int transport, unsigned int msgsize, uint64_t total)
{
	uint64_t nmsg = total / msgsize;
	double t0;
	pid_t pid;
	int pipefd[2] = { -1, -1 };
	const char *dev = NULL;

	if (transport == T_MUTEX) {
		dev = "/dev/kbuf1";
		if (slot_setup(dev, msgsize, 0) != 0)
			return -1;
	} else if (transport == T_SPSC) {
		dev = "/dev/kbuf2";
		if (slot_setup(dev, msgsize, 1) != 0)
			return -1;
	} else if (transport == T_PIPE) {
		if (pipe(pipefd) != 0)
			return -1;
	}

	t0 = now_sec();
	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* producer */
		unsigned char *buf = malloc(msgsize);
		uint64_t i;

		pin_to_cpu(cpu_a);
		memset(buf, 0xa5, msgsize);
		if (transport == T_MMAP) {
			struct kbuf_map m = { 0 };
			uint64_t done = 0;

			map_or_die(&m, "producer");
			while (done < total) {
				size_t w = kbuf_map_write(&m, buf, msgsize);

				if (!w)
					sched_yield();
				done += w;
			}
		} else if (transport == T_PIPE) {
			close(pipefd[0]);
			for (i = 0; i < nmsg; i++)
				if (write(pipefd[1], buf, msgsize) != (ssize_t)msgsize)
					break;
			close(pipefd[1]);
		} else {
			int fd = open(dev, O_WRONLY);

			for (i = 0; i < nmsg; i++)
				if (write(fd, buf, msgsize) != (ssize_t)msgsize)
					break;
			close(fd);
		}
		_exit(0);
	}

	/* consumer */
	{
		unsigned char *buf = malloc(msgsize);
		uint64_t i;

		pin_to_cpu(cpu_b);
		if (transport == T_MMAP) {
			struct kbuf_map m = { 0 };
			uint64_t done = 0;

			map_or_die(&m, "consumer");
			kbuf_map_reset(&m);
			while (done < total) {
				size_t r = kbuf_map_read(&m, buf, msgsize);

				if (!r)
					sched_yield();
				done += r;
			}
		} else if (transport == T_PIPE) {
			close(pipefd[1]);
			for (i = 0; i < nmsg; i++)
				if (read(pipefd[0], buf, msgsize) != (ssize_t)msgsize)
					break;
			close(pipefd[0]);
		} else {
			int fd = open(dev, O_RDONLY);

			for (i = 0; i < nmsg; i++)
				if (read(fd, buf, msgsize) != (ssize_t)msgsize)
					break;
			close(fd);
		}
		free(buf);
	}
	waitpid(pid, NULL, 0);
	return now_sec() - t0;
}

static void throughput(uint64_t total, int runs)
{
	static const unsigned int sizes[] = { 64, 256, 1024, 4096, 16384 };
	unsigned int si;
	int t, r;

	printf("\n## Throughput (MB/s), %llu MiB per run, %d runs (mean +/- stddev)\n",
	       (unsigned long long)(total >> 20), runs);
	printf("%-8s", "size");
	for (t = 0; t < T_COUNT; t++)
		printf("%18s", t_name[t]);
	printf("\n");

	for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
		printf("%-8u", sizes[si]);
		for (t = 0; t < T_COUNT; t++) {
			double mb = total / (1024.0 * 1024.0);
			double sum = 0, sumsq = 0, mean, sd;
			int ok = 1;

			for (r = 0; r < runs; r++) {
				double s = run_once(t, sizes[si], total);
				double rate;

				if (s < 0) {
					ok = 0;
					break;
				}
				rate = mb / s;
				sum += rate;
				sumsq += rate * rate;
			}
			if (ok) {
				mean = sum / runs;
				/* Bessel-corrected sample standard deviation. */
				sd = runs > 1
				   ? sqrt((sumsq - sum * sum / runs) / (runs - 1))
				   : 0.0;
				printf("  %7.0f +/-%-6.0f", mean, sd);
			} else {
				printf("%18s", "n/a");
			}
		}
		printf("\n");
	}
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* One-way produce->consume latency on the mmap ring (8-byte timestamps). */
static void latency(int samples)
{
	uint64_t *lat;
	int i;
	struct kbuf_map m = { 0 };
	pid_t pid;

	lat = mmap(NULL, samples * sizeof(uint64_t), PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (lat == MAP_FAILED)
		return;

	map_or_die(&m, "latency");
	kbuf_map_reset(&m);

	pid = fork();
	if (pid == 0) {
		uint64_t ts;

		pin_to_cpu(cpu_a);
		for (i = 0; i < samples; i++) {
			ts = now_ns();
			while (kbuf_map_write(&m, &ts, sizeof(ts)) == 0)
				;
		}
		_exit(0);
	}
	pin_to_cpu(cpu_b);
	for (i = 0; i < samples; i++) {
		uint64_t ts;

		while (kbuf_map_read(&m, &ts, sizeof(ts)) == 0)
			;
		lat[i] = now_ns() - ts;
	}
	waitpid(pid, NULL, 0);

	qsort(lat, samples, sizeof(uint64_t), cmp_u64);
	printf("\n## Latency (mmap ring, one-way, ns), %d samples\n", samples);
	printf("  p50=%llu  p90=%llu  p99=%llu  max=%llu\n",
	       (unsigned long long)lat[samples / 2],
	       (unsigned long long)lat[(int)(samples * 0.90)],
	       (unsigned long long)lat[(int)(samples * 0.99)],
	       (unsigned long long)lat[samples - 1]);
}

/* False sharing: two cores increment counters on the same vs separate lines. */
struct fs_shared {
	volatile uint64_t a;
	volatile uint64_t b;			/* adjacent: shares a's line  */
	char pad[128];
	volatile uint64_t c;
	char pad2[128];
	volatile uint64_t d;			/* separate line from c        */
};

/*
 * @atomic selects the contention pattern. A plain store-only increment lets a
 * core keep the line in its store buffer for a burst before the sibling steals
 * it, so false sharing is muted. An atomic read-modify-write (LOCK XADD) must
 * own the line *exclusively* for every single op, so when the two counters
 * share a line the line ping-pongs between L1s on every increment — the textbook
 * coherence penalty, and a far larger gap than the store-only case shows.
 */
static double fs_run(volatile uint64_t *x, volatile uint64_t *y,
		     uint64_t iters, int atomic)
{
	double t0 = now_sec();
	pid_t pid = fork();
	uint64_t i;

	if (pid == 0) {
		pin_to_cpu(cpu_a);
		if (atomic)
			for (i = 0; i < iters; i++)
				__atomic_fetch_add(x, 1, __ATOMIC_RELAXED);
		else
			for (i = 0; i < iters; i++)
				(*x)++;
		_exit(0);
	}
	pin_to_cpu(cpu_b);
	if (atomic)
		for (i = 0; i < iters; i++)
			__atomic_fetch_add(y, 1, __ATOMIC_RELAXED);
	else
		for (i = 0; i < iters; i++)
			(*y)++;
	waitpid(pid, NULL, 0);
	return now_sec() - t0;
}

static void fs_report(struct fs_shared *s, const char *what,
		      uint64_t iters, int atomic)
{
	double shared = fs_run(&s->a, &s->b, iters, atomic);	/* same line  */
	double separate = fs_run(&s->c, &s->d, iters, atomic);	/* diff lines */

	printf("  %-10s same line     : %.3f s\n", what, shared);
	printf("  %-10s separate lines: %.3f s\n", what, separate);
	printf("  %-10s speedup       : %.2fx\n", what, shared / separate);
}

static void falsesharing(uint64_t iters)
{
	struct fs_shared *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	/* Atomic RMWs are ~10x slower per op; scale iters down to keep runtime
	 * comparable while still landing in a stable regime.
	 */
	uint64_t atomic_iters = iters / 8;

	if (s == MAP_FAILED)
		return;

	printf("\n## False sharing (%llu store / %llu atomic-RMW increments/core)\n",
	       (unsigned long long)iters, (unsigned long long)atomic_iters);
	fs_report(s, "store", iters, 0);
	fs_report(s, "atomic-RMW", atomic_iters, 1);
}

int main(int argc, char *argv[])
{
	int quick = (argc > 1 && strcmp(argv[1], "quick") == 0);
	uint64_t total = quick ? (8ULL << 20) : (64ULL << 20);
	int runs = quick ? 3 : 10;
	int lat_samples = quick ? 2000 : 20000;
	uint64_t fs_iters = quick ? (20ULL << 20) : (200ULL << 20);

	pin_cpus_from_env();
	reduce_noise();
	printf("=== kbuf benchmarks (%s) ===\n", quick ? "quick" : "full");
	printf("producer CPU=%d  consumer CPU=%d\n", cpu_a, cpu_b);
	throughput(total, runs);
	latency(lat_samples);
	falsesharing(fs_iters);
	return 0;
}
