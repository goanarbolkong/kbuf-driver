#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# failslab.sh - drive the ring-allocation error paths under slab fault
# injection, then scan for leaks. Meant to run on the KASAN debug guest
# (CONFIG_FAILSLAB + CONFIG_DEBUG_KMEMLEAK), where a mishandled -ENOMEM unwind
# shows up as a KASAN report, a kmemleak entry, or a corrupted device.
#
# On a kernel without failslab the script degrades to a plain resize stress
# plus functional check, so it stays green in the default suite too. The
# host-side gate (verif/test_gates.py) is what fails the run on any KASAN/BUG
# splat in the captured dmesg.
rc=0
FS=/sys/kernel/debug/failslab
ML=/sys/kernel/debug/kmemleak

if [ -d "$FS" ]; then
	echo "failslab: configuring fault injection"
	# Only tasks that set /proc/self/make-it-fail are perturbed (the
	# workload opts in), so the shell and busybox keep allocating normally.
	echo 1   > "$FS/task-filter"
	echo 10  > "$FS/probability"	# ~10% of eligible allocations fail
	echo 0   > "$FS/space"
	echo -1  > "$FS/times"		# no cap on number of injected faults
	echo 0   > "$FS/verbose"	# don't flood the console with stacks
else
	echo "failslab: CONFIG_FAILSLAB absent, running plain resize stress"
fi

# kmemleak: take a baseline scan and clear known-old objects before the run.
if [ -f "$ML" ]; then
	echo scan  > "$ML" 2>/dev/null
	echo clear > "$ML" 2>/dev/null
fi

/tests/fault_resize 4000 || { echo "fault_resize failed (rc=$?)"; rc=1; }

# Turn injection off before tearing anything down.
[ -d "$FS" ] && echo 0 > "$FS/probability"

# Two scans with a settle in between: kmemleak only reports an object once it
# has survived a full scan without a discovered reference.
if [ -f "$ML" ]; then
	echo scan > "$ML"; sleep 1; echo scan > "$ML"
	leaks=$(grep -c 'backtrace' "$ML" 2>/dev/null)
	if [ "${leaks:-0}" -gt 0 ]; then
		echo "failslab: kmemleak reported $leaks suspected leak(s)"
		cat "$ML"
		rc=1
	else
		echo "failslab: kmemleak clean"
	fi
fi

exit $rc
