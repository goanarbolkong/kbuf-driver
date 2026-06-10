# bench/

Benchmarks land here in Phase 9. Planned comparisons (each with documented
methodology — CPU pinning, fixed governor, ≥10 runs, error bars):

- throughput vs message size for `{mutex, lock-free SPSC, mmap, pipe(2)}`
- read/write latency percentiles (p50/p90/p99)
- false-sharing before/after experiment

Results and methodology are written up in `../docs/BENCHMARKS.md`. No number
goes in that file without the methodology that produced it.
