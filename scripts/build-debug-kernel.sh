#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# build-debug-kernel.sh — build a QEMU guest kernel with heavyweight runtime
# verification enabled, for use by the pytest framework (verif/).
#
# Variants:
#   kasan   KASAN (generic, vmalloc) + lockdep + kmemleak + failslab
#           -> memory-safety gate + fault-injection of every error path
#   kcsan   KCSAN (data-race detector) + lockdep
#           -> concurrency gate; aimed at the lock-free SPSC ring
#
# The two are built separately because KASAN and KCSAN instrumentations are
# not meant to run together (each already costs a multiple in runtime).
#
# Usage:
#   ./scripts/build-debug-kernel.sh kasan|kcsan
# Env:
#   KBUF_KERNEL_VER   upstream version to fetch          (default 6.17)
#   KBUF_KERNEL_WORK  scratch dir for source + builds    (default .qemu/kernels)
#   JOBS              parallelism                        (default nproc)
#
# Output:
#   .qemu/bzImage-<variant>      bootable image for the framework
#   .qemu/debug-<variant>.env    KERNEL=/KDIR= consumed by `pytest --kbuf-variant`
#
# Build deps: flex bison libelf-dev libssl-dev (sudo apt-get install -y ...)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VARIANT="${1:?usage: build-debug-kernel.sh kasan|kcsan}"
VER="${KBUF_KERNEL_VER:-6.17}"
WORK="${KBUF_KERNEL_WORK:-$HERE/.qemu/kernels}"
JOBS="${JOBS:-$(nproc)}"

case "$VARIANT" in kasan|kcsan) ;; *)
	echo "unknown variant '$VARIANT' (kasan|kcsan)" >&2; exit 1 ;;
esac

SRC="$WORK/linux-$VER"
BUILD="$WORK/build-$VARIANT"
TARBALL="$WORK/linux-$VER.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/linux-$VER.tar.xz"

mkdir -p "$WORK"
if [[ ! -d "$SRC" ]]; then
	[[ -f "$TARBALL" ]] || { echo "fetching $URL"; wget -q -O "$TARBALL" "$URL"; }
	echo "extracting $(basename "$TARBALL")"
	tar -C "$WORK" -xf "$TARBALL"
fi

mkdir -p "$BUILD"
make -C "$SRC" O="$BUILD" defconfig kvm_guest.config >/dev/null

cfg() { "$SRC/scripts/config" --file "$BUILD/.config" "$@"; }

# Common verification options: lockdep, sleep-in-atomic, list corruption.
cfg -e DEBUG_KERNEL -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP -e DEBUG_LIST

case "$VARIANT" in
kasan)
	cfg -e KASAN -e KASAN_GENERIC -e KASAN_VMALLOC -e KASAN_INLINE \
	    -e SLUB_DEBUG -e DEBUG_KMEMLEAK \
	    -e FAULT_INJECTION -e FAILSLAB -e FAULT_INJECTION_DEBUG_FS \
	    -e FAIL_PAGE_ALLOC
	;;
kcsan)
	cfg -e KCSAN -d KCSAN_REPORT_ONCE_IN_MS \
	    -e KCSAN_REPORT_VALUE_CHANGE_ONLY
	;;
esac

make -C "$SRC" O="$BUILD" olddefconfig >/dev/null
echo "building $VARIANT kernel ($JOBS jobs) in $BUILD"
make -C "$SRC" O="$BUILD" -j"$JOBS" bzImage modules >/dev/null

install -D -m644 "$BUILD/arch/x86/boot/bzImage" "$HERE/.qemu/bzImage-$VARIANT"
cat > "$HERE/.qemu/debug-$VARIANT.env" <<EOF
KERNEL=$HERE/.qemu/bzImage-$VARIANT
KDIR=$BUILD
EOF
echo "done: .qemu/bzImage-$VARIANT  (KDIR=$BUILD)"
echo "run:  python3 -m pytest verif --kbuf-variant=$VARIANT"
