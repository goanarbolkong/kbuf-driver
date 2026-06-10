# bench/

`kbuf_bench.c` compares the kbuf byte-transfer transports between a producer and
a consumer pinned to different CPUs: the `mmap` zero-copy ring, the blocking
`mutex` slot path, the lock-free `spsc` slot path, and a `pipe(2)` baseline. It
measures throughput vs message size, one-way mmap latency percentiles, and a
false-sharing experiment.

Build with `make bench`; run with the `kbuf` module loaded (`/dev/kbuf0..2`):

```
./bench/kbuf_bench full      # 64 MiB/run, 5 runs, 20k latency samples
./bench/kbuf_bench quick     # smaller/faster smoke run
```

Knobs (environment variables):

- `KBUF_BENCH_CPU_A` / `KBUF_BENCH_CPU_B` — the producer/consumer CPUs. **Must be
  two distinct physical cores**; on an SMT host, CPU 0 and 1 are usually
  hyperthread siblings of one core (check `thread_siblings_list`) and will skew
  the results. Defaults to 0 and 1.
- `KBUF_BENCH_RT=1` — run the workers `SCHED_FIFO` (needs root) to keep the
  desktop from preempting the busy-poll loops. Opt-in; unnecessary on a quiet host.

The benchmark always `mlockall`s its pages so no sample includes a page fault.

For a full, reproducible bare-metal run (MOK-sign + governor pin + load + run +
teardown), use `scripts/run-baremetal-bench.sh`. Results and methodology are
written up in `../docs/BENCHMARKS.md`; no number goes in that file without the
methodology that produced it.
