#!/usr/bin/env bash
# run-baremetal-bench.sh — sign, load, and benchmark kbuf on bare metal.
#
# Unlike the QEMU harness this touches the live host, so:
#   * the module is MOK-signed (Secure Boot) before insmod;
#   * the CPU governor is pinned to "performance" for stable numbers;
#   * teardown ALWAYS unloads the module and restores the governor.
#
# Modes:
#   sudo bash scripts/run-baremetal-bench.sh            # all-in-one (default)
#   sudo bash scripts/run-baremetal-bench.sh setup      # sign+gov+insmod, leave loaded
#        bash scripts/run-baremetal-bench.sh bench      # run bench (no root; /dev/kbuf* is 0666)
#   sudo bash scripts/run-baremetal-bench.sh teardown   # rmmod + restore governor
#
# The split lets you iterate on the benchmark as an unprivileged user between a
# single setup and teardown, instead of re-entering the privileged path each run.
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

KO="$DIR/kbuf.ko"
KEY="$DIR/MOK.key"
CERT="$DIR/MOK.crt"
SIGN="/lib/modules/$(uname -r)/build/scripts/sign-file"
BENCH="$DIR/bench/kbuf_bench"
LOG="$DIR/.bench-baremetal.log"
GOVSAVE="$DIR/.saved-governor"

need_root() { [ "$(id -u)" -eq 0 ] || { echo "Run this mode as root."; exit 1; }; }

do_setup() {
	need_root
	[ -f "$KO" ]    || { echo "Missing $KO — run 'make modules' first."; exit 1; }
	[ -x "$BENCH" ] || { echo "Missing $BENCH — run 'make bench' first."; exit 1; }

	local gov
	gov="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
	echo "$gov" > "$GOVSAVE"

	echo "== kbuf bare-metal setup =="
	echo "kernel : $(uname -r)"
	echo "cpu    : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')  ($(nproc) CPUs)"
	echo "gov    : was $gov -> performance"

	echo "[1/3] sign module with MOK (kbuf-lab-mok)"
	"$SIGN" sha256 "$KEY" "$CERT" "$KO"

	echo "[2/3] governor -> performance"
	cpupower frequency-set -g performance >/dev/null

	echo "[3/3] insmod + verify"
	local mark="kbuf-bench-mark-$$"
	echo "$mark" > /dev/kmsg
	insmod "$KO"
	udevadm settle 2>/dev/null || sleep 0.3
	ls -l /dev/kbuf0 /dev/kbuf1 /dev/kbuf2
	echo "--- /proc/kbuf_status (head) ---"
	sed -n '1,14p' /proc/kbuf_status
	# Real kernel-side trouble only, scoped to lines after our marker. Userspace
	# "segfault" lines are deliberately NOT matched. Capture to a variable (grep
	# without -q reads all input, so no SIGPIPE under pipefail); `|| true` keeps
	# a clean no-match (grep exit 1) from tripping set -e.
	local trouble
	trouble="$(dmesg | sed -n "/$mark/,\$p" | \
	   grep -iE '(BUG:|WARNING:|kernel panic|general protection|stack segment|Call Trace|kbuf:.*(fail|error))' || true)"
	if [ -n "$trouble" ]; then
		echo "!! kernel-side trouble after insmod:"
		echo "$trouble"
		exit 1
	fi
	echo "Module loaded and clean. Run:  bash scripts/run-baremetal-bench.sh bench"
}

do_bench() {
	[ -e /dev/kbuf0 ] || { echo "kbuf not loaded — run setup first."; exit 1; }
	# Pin to two DISTINCT physical P-cores, avoiding cpu0 (IRQ-heavy). On the
	# 12900H cpu2 (core 4) and cpu4 (core 8) are separate P-cores; HT siblings
	# share a core (0-1, 2-3, ...) and must not be used as a pair.
	export KBUF_BENCH_CPU_A="${KBUF_BENCH_CPU_A:-2}"
	export KBUF_BENCH_CPU_B="${KBUF_BENCH_CPU_B:-4}"
	# RT (SCHED_FIFO) is OPT-IN only: set KBUF_BENCH_RT=1 explicitly. Never auto.
	# On a memory-pressured host RT does not help and makes a stuck run worse.
	echo "[bench] ${1:-full}  cpus=$KBUF_BENCH_CPU_A,$KBUF_BENCH_CPU_B  rt=${KBUF_BENCH_RT:-0}  (log: $LOG)"
	"$BENCH" "${1:-full}" 2>&1 | tee "$LOG"
	chown "${SUDO_USER:-$(id -un)}" "$LOG" 2>/dev/null || true
}

do_teardown() {
	need_root
	# Pipe-free load check: `lsmod | grep -q` would die to SIGPIPE under
	# `set -o pipefail` (grep -q closes the pipe early) and skip the rmmod.
	if [ -d /sys/module/kbuf ]; then
		echo "[teardown] rmmod kbuf"
		rmmod kbuf
	fi
	if [ -f "$GOVSAVE" ]; then
		local gov; gov="$(cat "$GOVSAVE")"
		echo "[teardown] restore governor -> $gov"
		cpupower frequency-set -g "$gov" >/dev/null 2>&1 || true
		rm -f "$GOVSAVE"
	fi
	echo "[teardown] dmesg tail:"
	dmesg | tail -4
}

case "${1:-all}" in
	setup)    do_setup ;;
	bench)    do_bench "${2:-full}" ;;
	teardown) do_teardown ;;
	all)
		need_root
		trap do_teardown EXIT
		do_setup
		do_bench full
		;;
	*) echo "Usage: $0 [setup|bench [quick|full]|teardown|all]"; exit 1 ;;
esac
