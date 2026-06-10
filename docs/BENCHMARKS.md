# kbuf — Benchmarks

Throughput, latency, and a false-sharing experiment for the kbuf transports,
produced by `bench/kbuf_bench.c`.

> **Environment caveat.** The numbers below were gathered **inside a QEMU/KVM
> guest** (2 vCPUs), not on bare metal. KVM runs guest code on real cores, so
> the *shape* of the results — mmap ≫ syscall paths, lock-free SPSC > mutex,
> and the false-sharing penalty — is representative, but the absolute figures
> (especially syscall costs and latency tails, which are sensitive to VM exits
> and host scheduling) are **illustrative only**. Re-run on bare metal with a
> fixed CPU governor for publishable numbers.

## Methodology

- **Transports.** `mutex` = slot ring in blocking mode via `read()/write()`;
  `spsc` = slot ring in lock-free SPSC mode via `read()/write()`; `mmap` = the
  zero-copy ring via `libkbuf` (no syscalls on the data path); `pipe` = a
  `pipe(2)` baseline.
- **Topology.** One producer process and one consumer process, pinned to
  separate CPUs with `sched_setaffinity` (`bench/kbuf_bench.c`).
- **Throughput.** Move a fixed total (64 MiB) in fixed-size chunks; sweep the
  chunk size; report MB/s = total / wall-clock, as **min/avg/max over 5 runs**.
  For the slot transports the device is resized so one slot holds one message.
- **Latency.** One-way producer→consumer latency on the mmap ring: the producer
  stamps `CLOCK_MONOTONIC` into each 8-byte message, the consumer subtracts on
  receipt. 20 000 samples, sorted for percentiles.
- **False sharing.** Two pinned processes each increment a counter in a shared
  page for a fixed number of iterations, once with the counters on the **same**
  cache line and once on **separate** lines; compare wall-clock.
- Reproduce with `make bench` then `./bench/kbuf_bench full` on a host with the
  module loaded (`/dev/kbuf0..2`), or via the QEMU harness
  (`scripts/run-qemu.sh`, which runs the `quick` profile).

## Throughput (MB/s), 64 MiB per run, 5 runs (min/avg/max)

| chunk |        mutex |         spsc |              mmap |         pipe |
|------:|-------------:|-------------:|------------------:|-------------:|
|   64 B|   16/  16/ 16|   56/  59/ 60| 1160/ 1324/ 1435 |  126/ 134/141|
|  256 B|   57/  60/ 61|  210/ 230/243| 2323/ 3993/ 4437 |  440/ 466/480|
| 1024 B|  232/ 239/241|  457/ 625/715| 5602/ 5984/ 6291 | 1052/1084/1118|
| 4096 B|  763/ 802/845| 2630/2714/2813| 7220/ 8602/ 9494 | 1738/2074/2332|
|16384 B| 2420/2473/2516| 6718/6869/7004|11079/11274/11481 | 2602/3233/3762|

## Latency — mmap ring, one-way (ns), 20 000 samples

| p50  | p90   | p99   | max   |
|-----:|------:|------:|------:|
| 9111 | 55845 | 67182 | 70070 |

(High tails reflect the busy-poll consumer yielding under a 2-vCPU guest; on
bare metal with dedicated cores these collapse dramatically. Reported as-is.)

## False sharing — 200 Mi increments/core

| layout         | time   |
|----------------|-------:|
| same line      | 0.150 s|
| separate lines | 0.091 s|

**Separation speedup: 1.65×.** Two cores writing different variables that share
one cache line still bounce the line between caches on every store; padding them
onto separate lines removes the coherence traffic. This is exactly why the mmap
control page keeps `head` and `tail` on separate cache lines.

## Discussion

- **mmap dominates, most at small messages.** At 64 B the mmap ring moves ~1.3
  GB/s versus ~16 MB/s for the mutex syscall path — roughly **80×** — because it
  pays no syscall or `copy_*_user` per message; the cost is a `memcpy` plus two
  atomics. As the chunk grows the per-message syscall overhead amortises and the
  gap narrows (mmap is ~4.5× the mutex path at 16 KB), with all transports
  trending toward memory-copy bandwidth.
- **Lock-free SPSC beats the mutex path** at every size (e.g. ~3.4× at 4 KB),
  still over the syscall boundary — it just removes the per-op mutex
  lock/unlock and the contention between producer and consumer.
- **`pipe(2)` sits between them**: a well-tuned kernel path, faster than the
  mutex slot ring at small sizes but well short of mmap, and below SPSC once
  messages are large enough to amortise the syscall.
- **Takeaway.** For high message rates the syscall and the in-kernel lock are
  the costs that matter; removing the syscall (mmap) wins big at small sizes,
  and removing the lock (SPSC) is a solid mid-ground when a syscall interface is
  still required. Cache-line hygiene on the shared control words is worth a
  measurable amount on its own.

## Threats to validity / future work

- VM, not bare metal; single 2-vCPU topology; no governor pinning.
- Busy-poll consumers in the latency test inflate tail latency under
  oversubscription; a blocking-notify variant would change the tail profile.
- Bandwidth, not goodput, at large sizes — all transports approach the same
  `memcpy` ceiling, so the interesting regime is small messages.
- Next: bare-metal runs with `cpupower frequency-set`, more sizes, error bars
  as stddev, and a NUMA-aware producer/consumer placement sweep.
