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
mkdir -p "$ROOT"/bin "$ROOT"/tests "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev

cp "$BB" "$ROOT/bin/busybox"
chmod +x "$ROOT/bin/busybox"
# Symlinks for the applets /init relies on (the kernel runs /init via /bin/sh,
# so /bin/sh must exist in the cpio before boot).
for applet in sh mount insmod rmmod cat ls echo sleep poweroff dmesg mkdir timeout; do
	ln -sf busybox "$ROOT/bin/$applet"
done

cp "$HERE/kbuf.ko" "$ROOT/kbuf.ko"
for src in "$HERE"/tests/*.c; do
	[ -e "$src" ] || continue
	name="$(basename "${src%.c}")"
	gcc -static -O2 -I"$HERE/include" -o "$ROOT/tests/$name" "$src"
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
[ -e /dev/kbuf ] && echo "/dev/kbuf present" || { echo "/dev/kbuf MISSING"; rc=1; }
cat /proc/kbuf_status

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

echo "--- producer fills 8 slots ---"
if timeout 15 /tests/test_producer 8 0; then echo "producer: OK"; else echo "producer: FAIL"; rc=1; fi
cat /proc/kbuf_status
echo "--- consumer drains 8 slots ---"
if timeout 15 /tests/test_consumer 8 0; then echo "consumer: OK"; else echo "consumer: FAIL"; rc=1; fi

echo "--- rmmod ---"
if rmmod kbuf; then echo "rmmod: OK"; else echo "rmmod: FAIL"; rc=1; fi

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
