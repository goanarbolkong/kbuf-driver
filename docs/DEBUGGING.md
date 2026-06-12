# kbuf — Debugging Log

A running record of non-obvious bugs. Format per entry:
**symptom → investigation → root cause → fix.** These are kept because the
reasoning is worth as much as the final code.

---

## 1. Deadlock: sleeping with the mutex held

**Symptom.** A reader on an empty buffer hung forever. A writer that should
have woken it blocked too; both processes ended up in `D` (uninterruptible-ish)
state and the device became unusable until reboot.

**Investigation.** The read path acquired `kbuf.lock`, found the ring empty,
and called `wait_event_interruptible()` *while still holding the mutex*. The
writer then blocked in `mutex_lock_interruptible()` trying to enter at all, so
it could never reach the `wake_up` that would release the reader. Classic
lock-ordering / hold-and-wait deadlock.

**Root cause.** Mutex held across a sleep that depends on another thread which
itself needs the mutex.

**Fix.** Adopt the standard check-sleep-recheck pattern: drop the lock before
sleeping, sleep on the wait queue, then re-acquire and re-test the predicate in
a `while` loop (not an `if`, to handle spurious/lost wakeups and multiple
waiters). See `kbuf_read`/`kbuf_write` in `src/kbuf_main.c`.

---

## 2. Lost `-ERESTARTSYS` handling on signal

**Symptom.** Pressing Ctrl-C on a blocked reader/writer sometimes returned
garbage or left the mutex locked, wedging subsequent opens.

**Investigation.** `mutex_lock_interruptible()` and `wait_event_interruptible()`
both return non-zero when interrupted by a signal. Early code ignored those
returns and proceeded as if the lock were held / the condition true. On the
signal path the mutex was either not held (double-unlock risk) or still held
(leak on return).

**Root cause.** Interruptible kernel primitives must have their return value
checked; on interrupt the syscall should bail with `-ERESTARTSYS` so the kernel
can restart or report `EINTR`, and any partially acquired lock must be released
first.

**Fix.** Every `mutex_lock_interruptible` / `wait_event_interruptible` return is
checked; the sleep path returns `-ERESTARTSYS` *after* the lock has been
dropped, so no lock leaks across the signal boundary.

---

## 3. Module load rejected under Secure Boot (MOK signing)

**Symptom.** `insmod kbuf.ko` failed with `Key was rejected by service` /
`Required key not available`; `dmesg` showed `Loading of unsigned module` or a
signature-verification failure.

**Investigation.** The development host boots with Secure Boot enabled, which
makes the kernel enforce module signature verification against keys in the
platform keyring (db) and the Machine Owner Key (MOK) list. An unsigned
out-of-tree module is refused.

**Root cause.** Out-of-tree modules are not signed by a key the kernel trusts
under Secure Boot.

**Fix.** Generate a MOK keypair, enrol the public key with `mokutil --import`
(confirmed at the pre-boot MOK Manager prompt on next reboot), then sign the
module with the kernel's `scripts/sign-file` before loading. The private key
(`MOK.key`) is gitignored and never committed. The signing flow lives in
`scripts/sign_and_load.sh`.

> Going forward, experimental builds are validated under QEMU
> (`scripts/run-qemu.sh`), which sidesteps host signing and protects the
> workstation from a faulty module — host loading is reserved for known-good
> builds.

---

## 4. Use-after-free unloading a dynamic device while open

**Symptom.** `test_ctl` created a device via `/dev/kbuf-ctl`, opened it, then
destroyed it while the fd was still open. Every data check passed — but the
final `close()` panicked the VM:

```
Oops: general protection fault ... RIP: module_put+0xf
Call Trace:  cdev_put  __fput  __x64_sys_close
```

`module_put` dereferenced a poisoned pointer (`RBX = e50159f6...`).

**Investigation.** The fault is in `cdev_put(inode->i_cdev)`, which `__fput`
calls *after* `f_op->release`. So the order on the last close is: our
`kbuf_release` runs first → `kref_put` drops the device's last reference →
`kbuf_dev_release` `kfree()`s the `kbuf_dev` — which **embedded** `struct cdev`.
Then the VFS runs `cdev_put` on that just-freed cdev and reads `cdev->owner`
(for `module_put`) from freed memory.

**Root cause.** The device object's lifetime (our kref) and the cdev's lifetime
(the VFS's own kobject refcount, released only after `cdev_put`) were tied to
the same allocation. Freeing the object in `->release` is always too early for
an embedded cdev, because the VFS touches the cdev again after `->release`.

**Fix.** Give dynamic devices a **standalone** cdev via `cdev_alloc()` instead
of an embedded `cdev_init()`. The kernel then owns the cdev's kobject and frees
the cdev itself after the final `cdev_put` — independent of our kref, which now
frees only the `kbuf_dev` (ring buffers + struct). Since `open()` could no
longer use `container_of(inode->i_cdev, ...)`, it resolves the device by minor
instead (static minors index the array; the rest are the dynamic pool, looked
up under the list lock with `kref_get`). Static devices keep their embedded
cdev — they are never freed before module unload, so the race does not apply.

**Why it mattered that this ran in QEMU.** A GP fault + kernel panic on the
development workstation would have taken the machine down. The throwaway VM
turned a fatal bug into a log line and a stack trace.

---

## 5. Benchmark segfault: unchecked `mmap()` failure (null-deref)

**Symptom.** The first bare-metal run of `kbuf_bench` crashed both worker
processes immediately:

```
kbuf_bench[...]: segfault at 0 ... error 6 in kbuf_bench   (producer, write to NULL)
kbuf_bench[...]: segfault at 0 ... error 4 in kbuf_bench   (consumer, read from NULL)
```

Faulting address `0`, one process on a write fault and one on a read fault.

**Investigation.** Address 0 means a null base pointer, not a wild one. The mmap
transport zero-initialises `struct kbuf_map m = { 0 }` and then calls
`kbuf_map_open(fd, &m)` — but the **return value was ignored**. When `mmap()`
fails it returns `MAP_FAILED` and leaves `m` all-zero; the very next
`kbuf_map_write`/`kbuf_map_read` dereferences `m.ctrl == NULL`. The module
itself was healthy (`/proc/kbuf_status` clean, devices present); the crash was
entirely user-space. The triggering `mmap()` failure was a transient state left
by an interrupted earlier session (stale module/binary pairing); a freshly
loaded module mmaps fine.

**Root cause.** A library call that can fail (`mmap`) used without checking its
return, turning a recoverable error into a null-pointer dereference.

**Fix.** Add `map_or_die()`: it opens `/dev/kbuf0`, calls `kbuf_map_open`, and on
failure prints the `errno` (and the requested mapping size) and exits non-zero
instead of charging ahead. All three mmap call sites (throughput producer,
throughput consumer, latency) route through it. A mmap problem is now a clear
diagnostic, not a segfault. See `bench/kbuf_bench.c`.

---

## 6. Benchmark methodology: hyperthread siblings and host noise

**Symptom.** Two implausible bare-metal numbers. (a) The mmap latency p50 came
out at **~32 µs** — worse than an earlier 2-vCPU VM run, which is backwards for
a shared-memory handoff that should be hundreds of ns. (b) Small-message
throughput was suspiciously *high* in one configuration, and the false-sharing
experiment showed almost no penalty.

**Investigation.** Two independent problems.

*Core placement.* The benchmark pins producer→CPU 0 and consumer→CPU 1. On the
i9-12900H `cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list`
returns `0-1`: CPU 0 and CPU 1 are the two **hyperthreads of one physical
P-core**. So the "two cores" shared an L1/L2 and an execution backend — which
inflated small-message throughput (the ring never left L1) and made the
false-sharing test meaningless (no second core to bounce the line to). CPU 2 and
CPU 4 are separate physical P-cores (core ids 4 and 8).

*Host noise.* Even on distinct cores the latency tail stayed huge. `free -h`
showed the desktop **swapping** (swap 100% full) and `ps` showed gnome-shell at
~47% CPU. The busy-poll consumer was being preempted and its pages paged out
mid-sample.

**Root cause.** (a) Naive CPU indices assume "CPU N and CPU N+1 are different
cores," false under SMT. (b) Benchmarking a latency-sensitive busy-poll on a
loaded, memory-pressured interactive host.

**Fix.** (a) Make the pinning configurable (`KBUF_BENCH_CPU_A`/`_B`) and default
the bare-metal runner to two distinct P-cores (2 and 4), documenting the sibling
trap. (b) `mlockall(MCL_CURRENT | MCL_FUTURE)` so no sample faults, plus an
opt-in `SCHED_FIFO` mode — and, more importantly, run on a quiet machine. After
freeing memory the latency p50 dropped from ~32 µs to **~2.8 µs** with a tail
under 4 µs. A first attempt left `SCHED_FIFO` on by default and, combined with
the swapping host, produced a multi-minute apparently-hung run; RT is now strictly
opt-in. See `bench/kbuf_bench.c` and `scripts/run-baremetal-bench.sh`.
