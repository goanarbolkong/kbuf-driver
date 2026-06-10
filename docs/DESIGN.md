# kbuf — Design Notes

This document records non-trivial design decisions. Each entry states the
decision, the alternatives considered, and the rationale.

## 1. Multi-file module layout (Phase 1)

**Decision.** Split the original single `kbuf_driver.c` into a small set of
translation units linked into one module object (`kbuf.ko`):

- `src/kbuf_main.c` — module init/exit and the `file_operations` (open,
  release, read, write, ioctl wiring). Owns blocking and wakeup policy.
- `src/kbuf_ring.c` — the circular-buffer core: slot copy and index advance.
- `src/kbuf_proc.c` — the `/proc/kbuf_status` view.
- `src/kbuf_ioctl.c` — ioctl command dispatch.
- `src/kbuf_internal.h` — in-kernel types and cross-file prototypes.
- `include/kbuf.h` — the user/kernel ABI, included by both sides.

**Rationale.** Later phases (poll, ioctl handlers, multiple instances,
lock-free mode, mmap) each touch a distinct concern. Separating them now keeps
each file small enough to reason about and review, and forces a clean line
between the ring mechanics and the syscall/policy layer.

**Locking boundary.** The ring helpers (`kbuf_ring_*`) assume the caller holds
`dev->lock` and has already checked the empty/full predicate. Blocking
(`wait_event_interruptible`), `O_NONBLOCK` handling, and wakeups stay entirely
in `kbuf_main.c`. This keeps `kbuf_ring.c` a pure data-structure module with no
scheduling policy baked in — important for the Phase 5 lock-free variant, which
will replace the mechanics without touching the policy layer.

**Per-device pointer, not a global reach-in.** Even though a single global
`struct kbuf_dev kbuf` backs `/dev/kbuf` today, every ring/proc/ioctl helper
takes an explicit `struct kbuf_dev *`. Phase 4 (one device per minor) then
becomes a wiring change in `kbuf_main.c` rather than a rewrite of the core.

**Build split.** `Kbuild` declares the module object layout for the kernel
build system; the top-level `Makefile` is the human entry point and also hosts
the `sparse` and `checkpatch` quality gates. Keeping them separate avoids the
kernel build system trying to parse host-make targets.

## 2. UAPI ABI (Phase 1 scaffold, Phase 3 implementation)

`include/kbuf.h` commits the ioctl command numbers (magic `'k'`) and the
`kbuf_stats` / `kbuf_resize` layouts now, so user space can be written against a
stable interface before the kernel-side handlers land. The dispatcher in
`kbuf_ioctl.c` validates the command encoding and returns `-ENOSYS` for
recognised-but-unimplemented commands (vs `-ENOTTY` for unknown ones).

**ABI policy.** Fixed-width types throughout. New fields are appended, never
reordered or resized. The `kbuf_stats` struct is the versioning vehicle if the
interface must grow.

## 3. poll/select/epoll support (Phase 2)

**Decision.** Implement `.poll` by registering the caller on *both* wait queues
with `poll_wait()` and returning `EPOLLIN | EPOLLRDNORM` while the ring is
non-empty and `EPOLLOUT | EPOLLWRNORM` while it is non-full.

**Why both queues.** A poller waiting only for readability still needs to be
woken when the ring transitions full → not-full if it also asked for
writability, and vice versa. `kbuf_read` wakes `write_wq` and `kbuf_write`
wakes `read_wq` on every state change, so registering on both queues guarantees
the poller is woken for either edge regardless of which event it is waiting on.
`poll_wait()` only enqueues the task — it never sleeps — so briefly taking the
mutex afterwards for a consistent (empty, full) snapshot is safe in poll
context.

**Level-triggered semantics.** The mask reflects current state every call, so
both level-triggered (default) and edge-triggered (`EPOLLET`) epoll users behave
correctly: a reader that drains only part of the ring is re-notified on the next
`epoll_wait` because slots remain.

**Test.** `tests/test_poll.c` samples readiness with a zero-timeout `poll()`
across an empty → 1-message → full → drained progression, and confirms
`epoll_wait` reports `EPOLLIN` when data is present. It is self-contained and
runs unattended under the QEMU harness.

## 4. ioctl UAPI + throughput stats (Phase 3)

**Dynamic geometry.** Supporting `KBUF_IOCRESIZE` forces the ring to be
heap-allocated: the slot array (`kcalloc`) and each slot's data buffer
(`kmalloc`) are now allocated at load time from `num_buffers`/`buffer_size`
fields on the device, bounded by `KBUF_MAX_NUM_BUFFERS` (256) and
`KBUF_MAX_BUFFER_SIZE` (64 KiB). The fixed `[8][4096]` arrays are gone. Init
allocates the ring and the mutex/wait queues *before* `cdev_add`, so the device
is never reachable in a half-initialised state (a latent bug in the v1 ordering,
which called `mutex_init` after `device_create`).

**Resize policy: reject unless empty (`-EBUSY`).** When a resize is requested on
a non-empty ring, the call fails with `-EBUSY` rather than silently dropping or
truncating queued messages. This is the only policy that never loses data the
caller hasn't acknowledged; the caller drains and retries. The new slot array is
allocated *before* taking the lock, so the mutex is held only for the pointer
swap (not the allocation); the old array is freed after unlocking. Alternatives
considered: drain-then-resize (silent data loss) and preserve-as-much-as-fits
(surprising partial loss, and complex to copy across differing geometries).

**Reset semantics (`KBUF_IOCRESET`).** Zeroes the throughput counters and sets
`peak_count` to the current depth. It does *not* discard queued data — "reset
the statistics", not "flush the ring" — so it is safe to call on a live device.

**Stats are lifetime-cumulative and device-global.** Counters
(`bytes/msgs_produced/consumed`, `read/write_sleeps`, `peak_count`) live on the
device, updated under the same mutex as the ring, and persist across open/close.
They are surfaced both via `KBUF_IOCGSTATS` and the enriched `/proc/kbuf_status`.

**Mode (`KBUF_IOCSMODE`).** Validates the argument and stores it, but only
`KBUF_MODE_BLOCKING` is implemented; `KBUF_MODE_SPSC` returns `-EOPNOTSUPP`
until the lock-free path lands in Phase 5. Honest stub over a silent no-op.

**Test.** `tests/test_ioctl.c` covers GSTATS accounting across a known
produce/consume, RESET, RESIZE (empty OK / non-empty `-EBUSY` / bounds
`-EINVAL`), SMODE validation, and an unknown command (`-ENOTTY`). It restores
the default 8×4096 geometry and drains before exiting so later tests in the same
boot see a clean device.

## Test harness (QEMU)

`scripts/run-qemu.sh` builds a busybox initramfs containing `kbuf.ko` and
statically linked copies of every `tests/*.c`, boots it under QEMU, runs the
suite as PID 1, and reports a `KBUF_QEMU_RESULT: PASS/FAIL` sentinel on the
serial console. No root is needed to build the image: `/init` mounts devtmpfs
and reopens `/dev/console` itself, so there are no device nodes to create. Only
the boot needs a readable kernel image (host `/boot/vmlinuz-*` is typically mode
0600, so the script expects a readable copy in `.qemu/bzImage`).

## Open questions (to resolve in the phase that needs them)

- **Mode switch (`KBUF_IOCSMODE`).** Whether switching between blocking and
  lock-free SPSC mode is allowed with an open ring, or only when idle. Phase 5.
  (The command and its validation exist now; only the behaviour is deferred.)
