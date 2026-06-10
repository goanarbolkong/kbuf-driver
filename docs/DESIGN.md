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

## Open questions (to resolve in the phase that needs them)

- **Resize semantics (`KBUF_IOCRESIZE`).** What happens to in-flight data when
  the ring is resized while non-empty? Candidate policies: reject with `-EBUSY`
  unless empty; drain-then-resize; preserve as much as fits. To be decided and
  documented in Phase 4.
- **Mode switch (`KBUF_IOCSMODE`).** Whether switching between blocking and
  lock-free SPSC mode is allowed with an open ring, or only when idle. Phase 5.
