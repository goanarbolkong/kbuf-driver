#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-qemu.sh — boot a throwaway VM, insmod kbuf.ko, run the test suite.
#
# This protects the development workstation: experimental module builds are
# never insmod'd on the host (a NULL deref or bad refcount there can take down
# the machine). The VM shares no state with the host kernel and is discarded.
#
# STATUS: scaffold. It documents the intended workflow and checks for its
# inputs, but the rootfs/kernel image are not yet provisioned — fully wiring
# this is the first task of Phase 2, when there is new runtime behaviour to
# validate. Until then, `make modules` (compile-only) is the Phase-1 gate.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_IMG="${KERNEL_IMG:-$HERE/.qemu/bzImage}"
ROOTFS_IMG="${ROOTFS_IMG:-$HERE/.qemu/rootfs.ext4}"

die() { echo "run-qemu: $*" >&2; exit 1; }

command -v qemu-system-x86_64 >/dev/null 2>&1 \
	|| die "qemu-system-x86_64 not found (apt-get install qemu-system-x86)"

# Build the module and tests for the VM's kernel before booting.
make -C "$HERE" modules tests

[[ -f "$KERNEL_IMG" ]] || die "kernel image not found: $KERNEL_IMG
  TODO(phase-2): build/fetch a kernel matching the headers used for kbuf.ko
  and a minimal rootfs (busybox/buildroot) containing kbuf.ko + tests/."
[[ -f "$ROOTFS_IMG" ]] || die "rootfs image not found: $ROOTFS_IMG"

# Intended invocation once images exist (init script runs insmod + tests,
# writes results to the serial console, then powers off):
exec qemu-system-x86_64 \
	-kernel "$KERNEL_IMG" \
	-drive file="$ROOTFS_IMG",format=raw,if=virtio \
	-append "root=/dev/vda console=ttyS0 init=/init.kbuf" \
	-smp 2 -m 512 -nographic -no-reboot
