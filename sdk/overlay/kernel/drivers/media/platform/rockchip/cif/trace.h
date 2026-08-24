/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rkcif

#if !defined(_TRACE_RKCIF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RKCIF_H
#include <linux/tracepoint.h>

TRACE_EVENT(rkcif_stream_event,
	TP_PROTO(const char *device_name, u32 stream_id, u64 generation,
		 u32 event, u32 sequence, u32 dma_mode, u64 timestamp_ns,
		 u64 latency_ns),
	TP_ARGS(device_name, stream_id, generation, event, sequence, dma_mode,
		timestamp_ns, latency_ns),
	TP_STRUCT__entry(
		__string(device_name, device_name)
		__field(u32, stream_id)
		__field(u64, generation)
		__field(u32, event)
		__field(u32, sequence)
		__field(u32, dma_mode)
		__field(u64, timestamp_ns)
		__field(u64, latency_ns)
	),
	TP_fast_assign(
		__assign_str(device_name, device_name);
		__entry->stream_id = stream_id;
		__entry->generation = generation;
		__entry->event = event;
		__entry->sequence = sequence;
		__entry->dma_mode = dma_mode;
		__entry->timestamp_ns = timestamp_ns;
		__entry->latency_ns = latency_ns;
	),
	TP_printk("%s stream=%u generation=%llu event=%s sequence=%u "
		  "dma=0x%x timestamp_ns=%llu latency_ns=%llu",
		  __get_str(device_name), __entry->stream_id,
		  __entry->generation,
		  __print_symbolic(__entry->event,
			{ RKCIF_OBSERVE_FS, "fs" },
			{ RKCIF_OBSERVE_FE, "fe" },
			{ RKCIF_OBSERVE_DMA, "dma" },
			{ RKCIF_OBSERVE_VB2_DONE, "vb2_done" }),
		  __entry->sequence, __entry->dma_mode,
		  __entry->timestamp_ns, __entry->latency_ns)
);
#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/media/platform/rockchip/cif
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>
