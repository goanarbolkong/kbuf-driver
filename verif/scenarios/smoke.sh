#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Post-insmod sanity: device nodes, control device, /proc, debugfs.
# $1 = expected number of static /dev/kbufN nodes (default 4).
exp="${1:-4}"
rc=0
[ -e /dev/kbuf0 ] || { echo "missing /dev/kbuf0"; rc=1; }
n=$(ls -d /dev/kbuf[0-9]* 2>/dev/null | wc -l)
[ "$n" -eq "$exp" ] || { echo "expected $exp devices, got $n"; rc=1; }
[ -e /dev/kbuf-ctl ] || { echo "missing /dev/kbuf-ctl"; rc=1; }
cat /proc/kbuf_status || { echo "/proc/kbuf_status unreadable"; rc=1; }
[ -d /sys/kernel/debug/kbuf/kbuf0 ] || { echo "missing debugfs dir"; rc=1; }
exit $rc
