#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

VISIBLE_FRAMES=${1:-300}
THERMAL_PAIRS=${2:-80}
OUTPUT_DIR=${3:-/tmp/rkcif-observability-stage6}
DUAL_VALIDATOR=/usr/local/sbin/verify_dual_capture_stage6
PROC_FILE=/proc/rkcif-mipi-lvds2
TRACE_ROOT=
dual_pid=

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

positive_integer()
{
	case "$1" in
		''|*[!0-9]*) return 1 ;;
	esac
	[ "$1" -gt 0 ]
}

cleanup()
{
	if [ -n "$TRACE_ROOT" ]; then
		printf 0 >"$TRACE_ROOT/tracing_on" 2>/dev/null || true
		printf 0 >"$TRACE_ROOT/events/rkcif/rkcif_stream_event/enable" \
			2>/dev/null || true
	fi
	if [ -n "$dual_pid" ] && kill -0 "$dual_pid" 2>/dev/null; then
		kill "$dual_pid" 2>/dev/null || true
		wait "$dual_pid" 2>/dev/null || true
	fi
}

trap cleanup EXIT INT TERM

positive_integer "$VISIBLE_FRAMES" ||
	fail 'visible frame count must be positive'
positive_integer "$THERMAL_PAIRS" ||
	fail 'thermal pair count must be positive'
[ -x "$DUAL_VALIDATOR" ] || fail "missing $DUAL_VALIDATOR"
[ -r "$PROC_FILE" ] || fail "missing $PROC_FILE"

for candidate in /sys/kernel/tracing /sys/kernel/debug/tracing; do
	if [ -d "$candidate/events/rkcif/rkcif_stream_event" ]; then
		TRACE_ROOT=$candidate
		break
	fi
done
[ -n "$TRACE_ROOT" ] ||
	fail 'rkcif tracepoint is unavailable; verify the new kernel is running'

mkdir -p "$OUTPUT_DIR"
DUAL_DIR=$OUTPUT_DIR/dual
DUAL_LOG=$OUTPUT_DIR/dual-validator.log
PROC_LOG=$OUTPUT_DIR/proc-during-stream.log
TRACE_LOG=$OUTPUT_DIR/rkcif-trace.log
SUMMARY=$OUTPUT_DIR/summary.log

printf 0 >"$TRACE_ROOT/tracing_on"
printf 0 >"$TRACE_ROOT/events/rkcif/rkcif_stream_event/enable"
: >"$TRACE_ROOT/trace"
printf 1 >"$TRACE_ROOT/events/rkcif/rkcif_stream_event/enable"
printf 1 >"$TRACE_ROOT/tracing_on"

"$DUAL_VALIDATOR" "$VISIBLE_FRAMES" "$THERMAL_PAIRS" "$DUAL_DIR" \
	>"$DUAL_LOG" 2>&1 &
dual_pid=$!

sampled=0
while kill -0 "$dual_pid" 2>/dev/null; do
	if grep -q 'per-stream observability:' "$PROC_FILE" 2>/dev/null; then
		cat "$PROC_FILE" >"$PROC_LOG"
		sampled=1
		break
	fi
	sleep 1
done

dual_status=0
wait "$dual_pid" || dual_status=$?
dual_pid=

printf 0 >"$TRACE_ROOT/tracing_on"
printf 0 >"$TRACE_ROOT/events/rkcif/rkcif_stream_event/enable"
cat "$TRACE_ROOT/trace" >"$TRACE_LOG"

[ "$dual_status" -eq 0 ] ||
	fail "dual validator exited $dual_status; see $DUAL_LOG"
[ "$sampled" -eq 1 ] ||
	fail 'procfs observability was not sampled while streaming'
grep -q '^STAGE6_DUAL_CAPTURE=PASS$' "$DUAL_DIR/summary.log" ||
	fail 'dual capture did not report PASS'

stream0=$(grep 'stream\[0\] generation:' "$PROC_LOG" | tail -n 1)
[ -n "$stream0" ] || fail 'stream[0] observability line missing'

generation=$(printf '%s\n' "$stream0" |
	sed -n 's/.*generation:\([0-9][0-9]*\).*/\1/p')
fs_count=$(printf '%s\n' "$stream0" |
	sed -n 's/.*fs:\([0-9][0-9]*\).*/\1/p')
fe_count=$(printf '%s\n' "$stream0" |
	sed -n 's/.*fe:\([0-9][0-9]*\).*/\1/p')
dma_count=$(printf '%s\n' "$stream0" |
	sed -n 's/.*dma:\([0-9][0-9]*\).*/\1/p')
vb2_count=$(printf '%s\n' "$stream0" |
	sed -n 's/.*vb2:\([0-9][0-9]*\).*/\1/p')

[ "${generation:-0}" -gt 0 ] || fail "invalid generation: $stream0"
[ "${fs_count:-0}" -gt 0 ] || fail "FS count did not advance: $stream0"
[ "${fe_count:-0}" -gt 0 ] || fail "FE count did not advance: $stream0"
[ "${dma_count:-0}" -gt 0 ] || fail "DMA count did not advance: $stream0"
[ "${vb2_count:-0}" -eq 0 ] ||
	fail "RKCIF VB2 count must stay zero on the RKISP online path: $stream0"

trace_fs=$(grep -c 'event=fs ' "$TRACE_LOG" || true)
trace_fe=$(grep -c 'event=fe ' "$TRACE_LOG" || true)
trace_dma=$(grep -c 'event=dma ' "$TRACE_LOG" || true)
[ "$trace_fs" -gt 0 ] || fail 'FS trace event missing'
[ "$trace_fe" -gt 0 ] || fail 'FE trace event missing'
[ "$trace_dma" -gt 0 ] || fail 'DMA trace event missing'

{
	printf 'GENERATION=%s\n' "$generation"
	printf 'PROC_FS=%s\n' "$fs_count"
	printf 'PROC_FE=%s\n' "$fe_count"
	printf 'PROC_DMA=%s\n' "$dma_count"
	printf 'PROC_VB2=%s\n' "$vb2_count"
	printf 'TRACE_FS=%s\n' "$trace_fs"
	printf 'TRACE_FE=%s\n' "$trace_fe"
	printf 'TRACE_DMA=%s\n' "$trace_dma"
	printf 'DUAL_CAPTURE=PASS\n'
	printf 'RKCIF_OBSERVABILITY=PASS\n'
} | tee "$SUMMARY"
