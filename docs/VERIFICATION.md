# Verification framework

The driver is verified by a host-side **pytest** framework (`verif/`) that
boots one disposable QEMU VM per test. The host machine never loads an
experimental module build; a kernel oops costs a failed test, not a crashed
workstation.

## Architecture

```
host (pytest)                              guest (QEMU, throwaway)
─────────────                              ───────────────────────
verif/kbufverif/initramfs.py  ──builds──►  busybox initramfs
  • make modules                             • /kbuf.ko        (fresh build)
  • gcc -static tests/*.c                    • /tests/*        (static bins)
  • verif/scenarios/*.sh                     • /scenarios/*.sh
  • generic /init                            • /init  ◄─ kernel cmdline

verif/kbufverif/qemu.py       ──boots───►  qemu-system-x86_64 -kernel ...
  • KVM if /dev/kvm, else TCG                -append "... kbuf.cmd=/tests/x"
  • captures serial console    ◄─markers──  KBUF_VERIF: insmod rc=0
  • parses structured markers                KBUF_VERIF: cmd rc=0
                                             KBUF_VERIF: dmesg-begin/.../end
verif/conftest.py                            KBUF_VERIF: done
  • one image per session
  • one boot per test
  • per-test artifacts: console.log, dmesg.txt
```

The guest workload is selected entirely through the kernel command line —
the initramfs is built once per session and never modified between boots:

| parameter | effect |
|---|---|
| `kbuf.cmd=PATH[,ARG...]` | workload to run; commas become argv separators |
| `kbuf.ndevices=N` | `insmod kbuf.ko ndevices=N` |
| `kbuf.noinsmod` | skip insmod (workload manages the module itself) |
| `kbuf.timeout=SEC` | guest-side `timeout` around the workload |

A test fails — with the full console log attached — if the workload exits
nonzero, if the kernel logs a BUG/oops, or if the guest never reaches the
final marker (hang or panic; the boot is reaped by a host-side timeout).

## Test inventory

| file | marker | what it proves |
|---|---|---|
| `test_smoke.py` | `smoke` | boot, insmod, device topology, /proc, debugfs |
| `test_functional.py` | `functional` | nonblock, poll, ioctl, edge cases, multi-device, dynamic ctl, dma-buf export/import, round-trip with tracepoint + debugfs counters |
| `test_cpp.py` | `functional` | the kbuf++ C++20 RAII wrapper, exercised by a GoogleTest suite (skipped unless GoogleTest is built) |
| `test_stress.py` | `stress` | lock-free SPSC and mmap magic-ring integrity under pinned producer/consumer load |
| `test_module.py` | `module` | rmmod refused (EBUSY) while an fd is open; full unload/reload cycle |
| `test_boot_matrix.py` | `matrix` | `ndevices=` boot matrix: 1, 2, 8, 64 |
| `test_gates.py` | `gate` | memory/race gates under an instrumented kernel (see below) |

## Running

```sh
make verif                      # full suite (~18 s with KVM), JUnit to .qemu/verif.xml
make verif-smoke                # one boot, sanity only
python3 -m pytest verif -m "functional or module" -q     # any pytest selection
python3 -m pytest verif -k spsc -q                       # single test
```

Requirements: `pytest`, `qemu-system-x86`, `busybox-static`, kernel headers,
and a readable kernel image at `.qemu/bzImage` (the framework falls back to
`/boot/vmlinuz-$(uname -r)` when that is world-readable). Without KVM the
suite still runs, just slowly — per-boot timeouts widen automatically.

The C++ `test_cpp.py` additionally needs a statically built GoogleTest; run
`make gtest` (or `scripts/fetch-googletest.sh`) once and the framework compiles
`tests/*.cpp` into the guest image. Until then it skips, so the C suite never
depends on it.

Artifacts land in `verif/_artifacts/<test-id>/{console.log,dmesg.txt}`;
CI uploads them together with the JUnit report on every run.

The original single-boot shell harness (`scripts/run-qemu.sh`) is kept as a
zero-dependency fallback and runs the same guest binaries.

## Memory & race verification gates

The default suite runs against a stock kernel. The `gate` tests instead boot a
**purpose-built debug kernel** so the instrumentation can catch defects the
functional tests cannot observe — use-after-free, out-of-bounds, data races on
the lock-free ring, and leaks on error paths.

`scripts/build-debug-kernel.sh` builds two variants from upstream source:

| variant | configuration | catches |
|---|---|---|
| `kasan` | KASAN (generic, vmalloc, inline) + kmemleak + lockdep + `failslab` fault injection | OOB/UAF on the slot buffers and the mmap magic ring; leaks and bad unwinds on `-ENOMEM` error paths; lock-order violations |
| `kcsan` | KCSAN data-race detector + lockdep | unsynchronised access in the SPSC release/acquire handoff |

```sh
# build once (downloads + builds a kernel; tens of minutes, needs ~a few GB
# of disk and flex/bison/libelf-dev/libssl-dev):
./scripts/build-debug-kernel.sh kasan

# then run the whole suite, gates included, on that kernel:
python3 -m pytest verif --kbuf-variant=kasan
```

With `--kbuf-variant` set, the framework rebuilds `kbuf.ko` against the debug
kernel's tree (so the module layout matches), boots that kernel, widens the
per-boot timeouts (instrumentation is slow), and **re-runs the existing
workloads under the instrumentation in addition to the gate-only tests**. Any
KASAN / KCSAN / lockdep / BUG signature in the captured serial log fails the
boot — `BootResult.oops` greps for exactly those markers.

The gate-only test `test_failslab_unwind` drives `verif/scenarios/failslab.sh`:
it turns on slab fault injection scoped to the workload task
(`/proc/self/make-it-fail`), runs `tests/fault_resize` to hammer
`KBUF_IOCRESIZE` so the ring-allocation path fails on ~10 % of allocations, and
then asks kmemleak to confirm nothing leaked. The driver must return `-ENOMEM`,
unwind without a splat, and leave the device fully functional.

Because the kernel build is heavy, the gates do **not** run on every push. CI
exposes them as a separate `gates` job triggered by `workflow_dispatch` or a
weekly schedule; it installs the toolchain, builds each variant, and uploads
the per-test console/dmesg artifacts and JUnit reports. On a workstation short
on disk or build dependencies, run them in CI rather than locally.
