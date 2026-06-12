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
| `test_functional.py` | `functional` | nonblock, poll, ioctl, edge cases, multi-device, dynamic ctl, round-trip with tracepoint + debugfs counters |
| `test_stress.py` | `stress` | lock-free SPSC and mmap magic-ring integrity under pinned producer/consumer load |
| `test_module.py` | `module` | rmmod refused (EBUSY) while an fd is open; full unload/reload cycle |
| `test_boot_matrix.py` | `matrix` | `ndevices=` boot matrix: 1, 2, 8, 64 |

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

Artifacts land in `verif/_artifacts/<test-id>/{console.log,dmesg.txt}`;
CI uploads them together with the JUnit report on every run.

The original single-boot shell harness (`scripts/run-qemu.sh`) is kept as a
zero-dependency fallback and runs the same guest binaries.
