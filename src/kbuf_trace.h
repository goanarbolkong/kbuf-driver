/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kbuf_trace.h - tracepoints for the kbuf device.
 *
 * Events register under the "kbuf" system, so they can be enabled via
 * /sys/kernel/tracing/events/kbuf/ or recorded with `perf record -e kbuf:*`.
 * One translation unit (kbuf_main.c) defines CREATE_TRACE_POINTS before
 * including this header to instantiate the tracepoints.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kbuf

#if !defined(_KBUF_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _KBUF_TRACE_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(kbuf_xfer,
	TP_PROTO(unsigned int id, unsigned int bytes, unsigned int occupancy),
	TP_ARGS(id, bytes, occupancy),
	TP_STRUCT__entry(
		__field(unsigned int, id)
		__field(unsigned int, bytes)
		__field(unsigned int, occupancy)
	),
	TP_fast_assign(
		__entry->id = id;
		__entry->bytes = bytes;
		__entry->occupancy = occupancy;
	),
	TP_printk("kbuf%u bytes=%u occ=%u",
		  __entry->id, __entry->bytes, __entry->occupancy)
);

DEFINE_EVENT(kbuf_xfer, kbuf_produce,
	TP_PROTO(unsigned int id, unsigned int bytes, unsigned int occupancy),
	TP_ARGS(id, bytes, occupancy));

DEFINE_EVENT(kbuf_xfer, kbuf_consume,
	TP_PROTO(unsigned int id, unsigned int bytes, unsigned int occupancy),
	TP_ARGS(id, bytes, occupancy));

TRACE_EVENT(kbuf_wakeup,
	TP_PROTO(unsigned int id, int woke_readers),
	TP_ARGS(id, woke_readers),
	TP_STRUCT__entry(
		__field(unsigned int, id)
		__field(int, woke_readers)
	),
	TP_fast_assign(
		__entry->id = id;
		__entry->woke_readers = woke_readers;
	),
	TP_printk("kbuf%u wake %s", __entry->id,
		  __entry->woke_readers ? "readers" : "writers")
);

#endif /* _KBUF_TRACE_H */

/* Out-of-tree include plumbing for define_trace.h (see samples/trace_events). */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE kbuf_trace
#include <trace/define_trace.h>
