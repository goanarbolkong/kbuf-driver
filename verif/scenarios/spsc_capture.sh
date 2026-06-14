#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Capture a real lock-free SPSC tracepoint stream for docs/img/spsc_handoff.png.
#
# test_spsc switches /dev/kbuf0 to SPSC mode and runs a producer pinned to CPU 0
# against a consumer pinned to CPU 1, so every message is handed off across
# cores through the release/acquire ring. We enable the kbuf tracepoints, run a
# short burst, then dump the trace buffer between markers; ftrace tags each line
# with the CPU it ran on, which is what scripts/plot_spsc.py uses to draw the
# cross-core handoff.
TRACE=/sys/kernel/debug/tracing
[ -d "$TRACE" ] || TRACE=/sys/kernel/tracing

echo 0 > "$TRACE/tracing_on"
echo    > "$TRACE/trace"
echo 1  > "$TRACE/events/kbuf/enable"
echo 1  > "$TRACE/tracing_on"

/tests/test_spsc 40 >/dev/null 2>&1

echo 0 > "$TRACE/tracing_on"
echo "KBUF_TRACE_BEGIN"
cat "$TRACE/trace"
echo "KBUF_TRACE_END"
