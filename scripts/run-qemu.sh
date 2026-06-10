#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-qemu.sh — boot a throwaway VM, insmod kbuf.ko, run the test suite.
#
# This protects the development workstation: experimental module builds are
# never insmod'd on the host (a NULL deref or bad refcount there can take down
# the machine). The VM shares no state with the host kernel and is discarded.
#
# It builds a minimal busybox initramfs containing kbuf.ko and statically
# linked copies of every tests/*.c program, boots it under QEMU, runs the
# suite, and reports PASS/FAIL via a sentinel line on the serial console.
#
# No root is needed to build the initramfs: /init mounts devtmpfs and reopens
# /dev/console itself, so there are no device nodes to mknod. Booting does need
# a readable kernel image (see KERNEL_IMG below).
#
# Usage:
#   ./scripts/run-qemu.sh
# Env overrides:
#   KERNEL_IMG=/path/to/bzImage   (default: .qemu/bzImage, else host kernel)
#   MEM=512  SMP=2  TIMEOUT=90
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$HERE/.qemu"
ROOT="$BUILD/initramfs"
KERNEL_IMG="${KERNEL_IMG:-$BUILD/bzImage}"
MEM="${MEM:-512}"
SMP="${SMP:-2}"
TIMEOUT="${TIMEOUT:-90}"

die() { echo "run-qemu: $*" >&2; exit 1; }

# --- preflight ---------------------------------------------------------------
command -v qemu-system-x86_64 >/dev/null 2>&1 \
	|| die "qemu-system-x86_64 not found. Install it:
    sudo apt-get install -y qemu-system-x86"

BB="$(command -v busybox)" \
	|| die "busybox not found. Install it: sudo apt-get install -y busybox-static"
[[ "$(file -L "$BB")" == *statically\ linked* ]] \
	|| die "busybox at $BB is not static; install busybox-static"

# Resolve the kernel image. The host kernel under /boot is usually mode 0600,
# so prefer a readable copy in .qemu/. The hint below uses absolute paths so it
# works no matter which directory the script was invoked from.
if [[ ! -f "$KERNEL_IMG" ]]; then
	host_k="/boot/vmlinuz-$(uname -r)"
	if [[ -r "$host_k" ]]; then
		KERNEL_IMG="$host_k"
	else
		die "no kernel image. Provision a readable one (creates parent dir):
    sudo install -D -m644 $host_k '$BUILD/bzImage'"
	fi
fi
[[ -r "$KERNEL_IMG" ]] || die "kernel image not readable: $KERNEL_IMG"

# --- build module + static test binaries -------------------------------------
echo "run-qemu: building module + tests"
make -C "$HERE" modules >/dev/null

rm -rf "$ROOT"
mkdir -p "$ROOT"/bin "$ROOT"/tests "$ROOT"/bench "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev

cp "$BB" "$ROOT/bin/busybox"
chmod +x "$ROOT/bin/busybox"
# Symlinks for the applets /init relies on (the kernel runs /init via /bin/sh,
# so /bin/sh must exist in the cpio before boot).
for applet in sh mount insmod rmmod cat ls echo sleep poweroff dmesg mkdir timeout grep; do
	ln -sf busybox "$ROOT/bin/$applet"
done

cp "$HERE/kbuf.ko" "$ROOT/kbuf.ko"
for src in "$HERE"/tests/*.c; do
	[ -e "$src" ] || continue
	name="$(basename "${src%.c}")"
	gcc -static -O2 -I"$HERE/include" -o "$ROOT/tests/$name" "$src"
done
for src in "$HERE"/bench/*.c; do
	[ -e "$src" ] || continue
	name="$(basename "${src%.c}")"
	gcc -static -O2 -I"$HERE/include" -o "$ROOT/bench/$name" "$src"
done

# --- /init: the VM's PID 1 ---------------------------------------------------
cat > "$ROOT/init" <<'INIT'
#!/bin/sh
# kbuf QEMU test runner (PID 1). Mount devtmpfs first, then reopen console so
# stdio is attached even though the initramfs has no static /dev/console node.
/bin/busybox mount -t devtmpfs none /dev 2>/dev/null
exec 0</dev/console 1>/dev/console 2>/dev/console
export PATH=/bin
mount -t proc  none /proc
mount -t sysfs none /sys

echo
echo "=== kbuf QEMU boot test ==="
uname -r

rc=0

echo "--- insmod ---"
if insmod /kbuf.ko; then
	echo "insmod: OK"
else
	echo "insmod: FAIL"; rc=1
fi
[ -e /dev/kbuf0 ] && echo "/dev/kbuf0 present" || { echo "/dev/kbuf0 MISSING"; rc=1; }
ndev=$(ls -d /dev/kbuf[0-9]* 2>/dev/null | wc -l)
echo "device nodes: $ndev"
[ "$ndev" -eq 4 ] && echo "4 devices (default ndevices): OK" || { echo "expected 4 devices, got $ndev"; rc=1; }
cat /proc/kbuf_status

# Observability (Phase 7): mount debugfs and enable the kbuf tracepoints up
# front, so the traffic from the tests below is recorded; verified near the end.
echo "--- observability setup ---"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d /sys/kernel/debug/kbuf/kbuf0 ] && echo "debugfs kbuf dir: OK" || { echo "debugfs kbuf dir: MISSING"; rc=1; }
TRACE=/sys/kernel/debug/tracing
if echo 1 > "$TRACE/events/kbuf/enable" 2>/dev/null; then
	traced=1; echo "tracepoints enabled: OK"
else
	traced=0; echo "tracepoints: unavailable (skipping trace check)"
fi

# Self-contained tests first (each fills and drains the ring itself). Then the
# producer/consumer pair, kept adjacent so the producer's 8 messages are still
# present when the (blocking) consumer reads them. Every test is wrapped in a
# timeout so a hang can never wedge the VM — it fails the run instead.
echo "--- test_nonblock ---"
if timeout 20 /tests/test_nonblock; then echo "test_nonblock: OK"; else echo "test_nonblock: FAIL"; rc=1; fi

if [ -x /tests/test_poll ]; then
	echo "--- test_poll ---"
	if timeout 20 /tests/test_poll; then echo "test_poll: OK"; else echo "test_poll: FAIL"; rc=1; fi
fi

if [ -x /tests/test_ioctl ]; then
	echo "--- test_ioctl ---"
	if timeout 20 /tests/test_ioctl; then echo "test_ioctl: OK"; else echo "test_ioctl: FAIL"; rc=1; fi
fi

if [ -x /tests/test_multi ]; then
	echo "--- test_multi ---"
	if timeout 20 /tests/test_multi; then echo "test_multi: OK"; else echo "test_multi: FAIL"; rc=1; fi
fi

if [ -x /tests/test_spsc ]; then
	echo "--- test_spsc (lock-free stress) ---"
	if timeout 40 /tests/test_spsc; then echo "test_spsc: OK"; else echo "test_spsc: FAIL"; rc=1; fi
fi

if [ -x /tests/test_mmap ]; then
	echo "--- test_mmap (zero-copy stress) ---"
	if timeout 40 /tests/test_mmap; then echo "test_mmap: OK"; else echo "test_mmap: FAIL"; rc=1; fi
fi

# Throughput benchmark (informational — numbers from a VM are illustrative).
if [ -x /bench/kbuf_bench ]; then
	echo "--- kbuf_bench (mmap vs syscall) ---"
	if timeout 40 /bench/kbuf_bench; then echo "kbuf_bench: OK"; else echo "kbuf_bench: FAIL"; rc=1; fi
fi

echo "--- producer fills 8 slots ---"
if timeout 15 /tests/test_producer 8 0; then echo "producer: OK"; else echo "producer: FAIL"; rc=1; fi
cat /proc/kbuf_status
echo "--- consumer drains 8 slots ---"
if timeout 15 /tests/test_consumer 8 0; then echo "consumer: OK"; else echo "consumer: FAIL"; rc=1; fi

echo "--- observability check ---"
mp=$(cat /sys/kernel/debug/kbuf/kbuf0/msgs_produced 2>/dev/null)
echo "debugfs kbuf0/msgs_produced=${mp:-?}"
[ "${mp:-0}" -gt 0 ] && echo "debugfs counter nonzero: OK" || { echo "debugfs counter: FAIL"; rc=1; }
if [ "${traced:-0}" = 1 ]; then
	if grep -q kbuf_produce "$TRACE/trace"; then
		echo "tracepoint kbuf_produce recorded: OK"
	else
		echo "tracepoint kbuf_produce: FAIL"; rc=1
	fi
fi

echo "--- rmmod ---"
if rmmod kbuf; then echo "rmmod: OK"; else echo "rmmod: FAIL"; rc=1; fi

# Reload with a non-default ndevices to exercise the module param and a full
# unload/reload cycle.
echo "--- reload with ndevices=2 ---"
if insmod /kbuf.ko ndevices=2; then
	n2=$(ls -d /dev/kbuf[0-9]* 2>/dev/null | wc -l)
	[ "$n2" -eq 2 ] && echo "ndevices=2 honored: OK" || { echo "ndevices=2 FAIL (got $n2)"; rc=1; }
	if rmmod kbuf; then echo "reload rmmod: OK"; else echo "reload rmmod: FAIL"; rc=1; fi
else
	echo "reload insmod: FAIL"; rc=1
fi

if [ "$rc" -eq 0 ]; then
	echo "KBUF_QEMU_RESULT: PASS"
else
	echo "KBUF_QEMU_RESULT: FAIL"
fi

poweroff -f
INIT
chmod +x "$ROOT/init"

# --- pack initramfs ----------------------------------------------------------
echo "run-qemu: packing initramfs"
( cd "$ROOT" && find . -print0 \
	| cpio --null -o --format=newc 2>/dev/null \
	| gzip -9 ) > "$BUILD/initramfs.cpio.gz"

# --- boot --------------------------------------------------------------------
# Hardware acceleration is effectively mandatory: under TCG software emulation
# this kernel takes minutes just to reach userspace. Use KVM when /dev/kvm is
# usable, otherwise warn and fall back to (very slow) TCG.
if [ -w /dev/kvm ]; then
	ACCEL=(-enable-kvm -cpu host)
	echo "run-qemu: KVM acceleration enabled"
else
	ACCEL=(-cpu qemu64)
	echo "run-qemu: WARNING /dev/kvm not writable — falling back to slow TCG."
	echo "          Add yourself to the kvm group: sudo usermod -aG kvm \$USER"
fi

LOG="$BUILD/console.log"
echo "run-qemu: booting $KERNEL_IMG (timeout ${TIMEOUT}s)"
set +e
timeout "$TIMEOUT" qemu-system-x86_64 "${ACCEL[@]}" \
	-kernel "$KERNEL_IMG" \
	-initrd "$BUILD/initramfs.cpio.gz" \
	-append "console=ttyS0 panic=-1 oops=panic loglevel=4 rdinit=/init" \
	-m "$MEM" -smp "$SMP" -nographic -no-reboot 2>&1 | tee "$LOG"
set -e

echo
if grep -q "KBUF_QEMU_RESULT: PASS" "$LOG"; then
	echo "run-qemu: PASS"
	exit 0
fi
echo "run-qemu: FAIL (see $LOG)"
exit 1
