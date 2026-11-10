/* SPDX-License-Identifier: GPL-2.0 */
/*
 * svmd_trace.h - tracepoints: named events in the driver that tracing
 * tools (perf, ftrace, bpftrace) can watch without changing any code.
 *
 *   bpftrace -e 'tracepoint:svmd:svmd_fault_done { @us = hist(args->ns / 1000); }'
 *
 * scripts/svmd-trace.bt has the full set of measurements.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM svmd

#if !defined(SVMD_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define SVMD_TRACE_H

#include <linux/tracepoint.h>

/* The device stopped at `va` and asked for help (spec §6). */
TRACE_EVENT(svmd_fault,
	TP_PROTO(u64 va, u32 access, u32 reason, u64 root),
	TP_ARGS(va, access, reason, root),
	TP_STRUCT__entry(
		__field(u64, va)
		__field(u32, access)
		__field(u32, reason)
		__field(u64, root)
	),
	TP_fast_assign(
		__entry->va = va;
		__entry->access = access;
		__entry->reason = reason;
		__entry->root = root;
	),
	TP_printk("va=0x%llx %s reason=%u root=0x%llx", __entry->va,
		  __entry->access & 1 ? "write" : "read", __entry->reason,
		  __entry->root)
);

/* ...and what we did about it: ret 0 = mapped and retried, <0 = aborted. */
TRACE_EVENT(svmd_fault_done,
	TP_PROTO(u64 va, int ret, u64 ns),
	TP_ARGS(va, ret, ns),
	TP_STRUCT__entry(
		__field(u64, va)
		__field(int, ret)
		__field(u64, ns)
	),
	TP_fast_assign(
		__entry->va = va;
		__entry->ret = ret;
		__entry->ns = ns;
	),
	TP_printk("va=0x%llx ret=%d took=%lluns", __entry->va, __entry->ret,
		  __entry->ns)
);

/* The CPU side of [start, end) changed: entries cleared, TLB flushed. */
TRACE_EVENT(svmd_invalidate,
	TP_PROTO(u64 start, u64 end, u32 event, u64 ns),
	TP_ARGS(start, end, event, ns),
	TP_STRUCT__entry(
		__field(u64, start)
		__field(u64, end)
		__field(u32, event)
		__field(u64, ns)
	),
	TP_fast_assign(
		__entry->start = start;
		__entry->end = end;
		__entry->event = event;
		__entry->ns = ns;
	),
	TP_printk("[0x%llx, 0x%llx) event=%u took=%lluns", __entry->start,
		  __entry->end, __entry->event, __entry->ns)
);

/* Pages moved between system RAM and VRAM by the copy engine. */
TRACE_EVENT(svmd_migrate,
	TP_PROTO(bool to_vram, unsigned long npages, u64 ns),
	TP_ARGS(to_vram, npages, ns),
	TP_STRUCT__entry(
		__field(bool, to_vram)
		__field(unsigned long, npages)
		__field(u64, ns)
	),
	TP_fast_assign(
		__entry->to_vram = to_vram;
		__entry->npages = npages;
		__entry->ns = ns;
	),
	TP_printk("%s %lu pages took=%lluns",
		  __entry->to_vram ? "to_vram" : "to_ram", __entry->npages,
		  __entry->ns)
);

#endif /* SVMD_TRACE_H */

/* this part must be outside the include guard */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE svmd_trace
#include <trace/define_trace.h>
