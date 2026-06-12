#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Blocking producer/consumer round-trip with observability checks:
# the debugfs counter must advance and the produce tracepoint must fire.
rc=0
TRACE=/sys/kernel/debug/tracing
traced=0
echo 1 > "$TRACE/events/kbuf/enable" 2>/dev/null && traced=1

/tests/test_producer 8 0 || { echo "producer failed"; rc=1; }
/tests/test_consumer 8 0 || { echo "consumer failed"; rc=1; }

mp=$(cat /sys/kernel/debug/kbuf/kbuf0/msgs_produced 2>/dev/null)
[ "${mp:-0}" -gt 0 ] || { echo "debugfs msgs_produced not > 0"; rc=1; }

if [ "$traced" = 1 ]; then
	grep -q kbuf_produce "$TRACE/trace" \
		|| { echo "kbuf_produce tracepoint not recorded"; rc=1; }
else
	echo "tracing unavailable, tracepoint check skipped"
fi
exit $rc
