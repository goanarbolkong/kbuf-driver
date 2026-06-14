#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Capture a real kbuf tracepoint stream for docs/img/trace_timeline.png.
#
# A fast producer and a slow consumer share the default 8-slot blocking ring,
# so the ring fills, the producer blocks on backpressure, and wakeups fire as
# the consumer drains one slot at a time. We enable the kbuf tracepoints, run
# the workload, then dump the trace buffer between markers for
# scripts/plot_trace.py to parse. The produce/consume events carry the ring
# occupancy, which is what the figure plots over time.
TRACE=/sys/kernel/debug/tracing
[ -d "$TRACE" ] || TRACE=/sys/kernel/tracing

echo 0 > "$TRACE/tracing_on"
echo    > "$TRACE/trace"
echo 1  > "$TRACE/events/kbuf/enable"
echo 1  > "$TRACE/tracing_on"

# count delay_ms: 50 messages each, producer ~5x faster than the consumer.
/tests/test_producer 50 5  >/dev/null 2>&1 &
prod=$!
/tests/test_consumer 50 25 >/dev/null 2>&1 &
cons=$!
wait $prod
wait $cons

echo 0 > "$TRACE/tracing_on"
echo "KBUF_TRACE_BEGIN"
cat "$TRACE/trace"
echo "KBUF_TRACE_END"
