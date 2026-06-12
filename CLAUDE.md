# kbuf — Project Context

## What this is
A standalone Linux kernel character device driver implementing a
producer/consumer circular buffer, developed as a showcase of systems
programming and verification practice: every feature lands with tests,
documentation, and (where relevant) benchmarks.

Current state: all nine phases below are complete and QEMU-verified — multiple
instances (static and dynamic via /dev/kbuf-ctl), versioned ioctl UAPI,
lock-free SPSC mode, mmap zero-copy magic ring, debugfs + tracepoints,
CI, and a bare-metal benchmark report with figures.

## Goal
Keep the quality bar: no feature without tests, no claim without a
reproducible measurement, no design decision without a written rationale.

## Phase plan
1. **Restructure** — split into src/ (kbuf_main.c, kbuf_ring.c, kbuf_proc.c,
   kbuf_ioctl.c), include/kbuf.h (UAPI), tests/, bench/, docs/, CI workflow.
2. **poll/epoll** — implement .poll; wake both read_wq and write_wq correctly;
   test epoll wakeup latency.
3. **ioctl UAPI** — KBUF_IOCRESIZE, KBUF_IOCGSTATS, KBUF_IOCRESET, KBUF_IOCSMODE
   (magic 'k'). Resize policy: document the chosen semantics for non-empty
   buffers in docs/DESIGN.md.
4. **Multiple instances** — N minors via alloc_chrdev_region + device_create,
   module param ndevices=4. Stretch: /dev/kbuf-ctl control device for dynamic
   create/destroy with kref lifetime management.
5. **Lock-free SPSC mode** — smp_load_acquire/smp_store_release, power-of-two
   capacity with masking, free-running indices. Hybrid blocking: lock-free fast
   path, wait-queue fallback (check-sleep-recheck). Stress test with checksummed
   data, producer/consumer pinned to different cores.
6. **mmap zero-copy** — vmalloc_user buffer, double virtual mapping ("magic ring
   buffer") so wraps are contiguous, shared control page with head/tail, C11
   atomics userspace library (libkbuf), benchmark vs syscall path.
7. **Observability** — debugfs per device + TRACE_EVENT tracepoints for
   produce/consume/wakeup; usable via perf record -e kbuf:*.
8. **Tests + CI** — kselftest-style suite (blocking, O_NONBLOCK, partial reads,
   signal interruption, concurrent stress, epoll, mmap, resize races,
   unload-under-load). GitHub Actions: build matrix on 3-4 kernel header
   versions, checkpatch.pl --strict, sparse (make C=2), QEMU boot + insmod +
   run suite.
9. **Benchmark report** — docs/BENCHMARKS.md in English: methodology (pinning,
   governor, 10 runs, error bars), throughput vs message size for
   {mutex, lock-free, mmap, pipe(2)}, latency percentiles, false-sharing
   before/after experiment, discussion section.
10. **pytest verification framework** — verif/: one QEMU boot per test,
    cmdline-driven guest (kbuf.cmd=/kbuf.ndevices=), structured serial
    markers, per-test console/dmesg artifacts, JUnit XML, ndevices boot
    matrix, CI integration. docs/VERIFICATION.md.
11. **Race/memory verification gates** — debug kernel configs (KCSAN against
    the lock-free SPSC ring, KASAN, lockdep, kmemleak) booted by the
    framework; failslab fault-injection scenario; document findings.
12. **C++ RAII library** — include/kbufpp.hpp: move-only device + mmap-ring
    handles, span-based API; GoogleTest suite run inside QEMU via the
    framework; CI job.
13. **dma-buf exporter** — export the data ring as a dma-buf fd
    (KBUF_IOCEXPORT); attach/map/vmap ops; importer test; DESIGN.md section.

## Conventions & quality bar
- Kernel coding style; all patches must pass `checkpatch.pl --strict`.
- Run sparse (`make C=2`) on every build; fix all warnings.
- Test under lockdep and KASAN-enabled kernels; never ship known races.
- Every nontrivial design decision gets a paragraph in docs/DESIGN.md.
- Every gnarly bug encountered gets an entry in docs/DEBUGGING.md
  (symptom → investigation → root cause → fix).
- All docs, comments, and commit messages in clear English.
- Commit style: small, logical commits with kernel-style subject lines
  ("kbuf: add poll support to file operations").
- copy_from_user/copy_to_user can sleep — the "lock-free" path is still not
  atomic-context safe; keep this documented honestly.

## Environment notes
- Development against current LTS kernel headers; QEMU for boot tests
  (never insmod experimental builds on the host without a snapshot/VM).
- Secure Boot machines need MOK-signed modules (already solved once — see
  DEBUGGING.md once migrated).

## What NOT to do
- No features without tests.
- No benchmark numbers without documented methodology.
- Don't silently change UAPI; version it via the stats struct if needed.
