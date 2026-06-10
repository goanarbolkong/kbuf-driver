# kbuf — Debugging Log

A running record of non-obvious bugs. Format per entry:
**symptom → investigation → root cause → fix.** These are kept because the
reasoning is the portfolio, not just the final code.

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
