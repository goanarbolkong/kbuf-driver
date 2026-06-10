# kbuf — a Linux producer/consumer character device

`kbuf` is an out-of-tree Linux kernel module exposing `ndevices` independent
character devices `/dev/kbuf0..N`, each a fixed-size circular queue with
blocking and non-blocking semantics. Producers `write()` messages into empty
slots; consumers `read()` them back in FIFO order. A per-device mutex serialises
ring access and two wait queues block callers when that ring is full or empty.

This repository started as a university OS course project and is being grown
into a portfolio-grade driver. Development direction and the phase plan live in
[`CLAUDE.md`](CLAUDE.md); design decisions in [`docs/DESIGN.md`](docs/DESIGN.md);
the running log of bugs and fixes in [`docs/DEBUGGING.md`](docs/DEBUGGING.md).

## Layout

```
include/kbuf.h        user/kernel ABI (ioctl numbers, kbuf_stats, mmap ctrl)
include/libkbuf.h     user-space lib for the mmap zero-copy ring (SPSC)
src/kbuf_main.c       module lifecycle + file operations (blocking + SPSC)
src/kbuf_ring.c       circular-buffer core: blocking + lock-free SPSC mechanics
src/kbuf_proc.c       /proc/kbuf_status
src/kbuf_ioctl.c      ioctl dispatch (stats, resize, reset, mode)
src/kbuf_mmap.c       mmap + magic-ring double-mapping fault handler
src/kbuf_debugfs.c    /sys/kernel/debug/kbuf/kbufN/ counter files
src/kbuf_ctl.c        /dev/kbuf-ctl: runtime create/destroy (kref lifetime)
src/kbuf_trace.h      TRACE_EVENT definitions (perf record -e 'kbuf:*')
src/kbuf_internal.h   in-kernel types and cross-file prototypes
tests/                user-space functional + stress tests
bench/                throughput benchmarks (kbuf_bench: mmap vs syscall)
docs/                 DESIGN.md, DEBUGGING.md, BENCHMARKS.md (Phase 9)
scripts/              QEMU boot-test harness, host signing helper
```

## Build

```sh
make            # builds kbuf.ko + test programs
make sparse     # static analysis (make C=2)
make checkpatch # kernel coding-style check (--strict)
```

## Run

Experimental builds are validated under QEMU, never insmod'd directly on the
development host (see `scripts/run-qemu.sh`). On a machine where host loading is
acceptable, `make load` / `make unload` are provided; Secure Boot hosts require
the module to be MOK-signed first.

## Status

| Phase | Feature | State |
|-------|---------|-------|
| 1 | Multi-file restructure + UAPI scaffold | ✅ done |
| 2 | poll/epoll + QEMU test harness | ✅ done (verified under QEMU) |
| 3 | ioctl UAPI (resize, stats, reset, mode) | ✅ done (verified under QEMU) |
| 4 | Multiple instances (N minors, `ndevices=`) | ✅ done (verified under QEMU) |
| 4+ | Dynamic create/destroy via `/dev/kbuf-ctl` (kref) | ✅ done (stretch, verified under QEMU) |
| 5 | Lock-free SPSC mode | ✅ done (verified under QEMU) |
| 6 | mmap zero-copy ring (magic ring + libkbuf) | ✅ done (verified under QEMU) |
| 7 | debugfs + tracepoints | ✅ done (verified under QEMU) |
| 8 | functional suite + CI | ✅ done (suite verified under QEMU) |
| 9 | Benchmark report | ✅ done (docs/BENCHMARKS.md) |
