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
