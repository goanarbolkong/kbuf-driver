# bench/

`kbuf_bench.c` (added in Phase 6) compares mmap zero-copy throughput against the
`read()`/`write()` slot path with a producer/consumer pinned to different CPUs.
Build with `make bench`; run on a `kbuf` device present (`/dev/kbuf0` and
`/dev/kbuf1`). It prints MB/s for each path and the speedup. The numbers it
prints are a quick comparison only.

The rigorous report lands in Phase 9. Planned comparisons (each with documented
methodology — CPU pinning, fixed governor, ≥10 runs, error bars):

- throughput vs message size for `{mutex, lock-free SPSC, mmap, pipe(2)}`
- read/write latency percentiles (p50/p90/p99)
- false-sharing before/after experiment

Results and methodology are written up in `../docs/BENCHMARKS.md`. No number
goes in that file without the methodology that produced it.
