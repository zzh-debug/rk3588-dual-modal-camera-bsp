#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

OUTPUT_DIR=${1:-/tmp/rkisp-l1-timeout-stage6}
TIMEOUT_MS=${2:-500}
RECOVERY_VISIBLE=${3:-300}
RECOVERY_THERMAL=${4:-80}
PARAM_ROOT=/sys/module/video_rkisp/parameters
DUAL_VALIDATOR=/usr/local/sbin/verify_dual_capture_stage6
ISP_NODE=
original_enable=
original_timeout=
original_drop=

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

find_entity_node()
{
	media=$1
	entity=$2

	media-ctl -d "$media" -p 2>/dev/null | awk -v target="$entity" '
		/^- entity [0-9]+:/ {
			in_entity = index($0, ": " target " ") > 0
			next
		}
		in_entity && /device node name/ {
			sub(/^.*device node name[[:space:]]+/, "")
			print
			exit
		}
	'
}

find_isp_node()
{
	for media in /dev/media*; do
		[ -c "$media" ] || continue
		graph=$(media-ctl -d "$media" -p 2>/dev/null || true)
		if printf '%s\n' "$graph" | grep -Fq 'rkcif-mipi-lvds2' &&
			printf '%s\n' "$graph" | grep -Fq 'rkisp_mainpath'; then
			find_entity_node "$media" rkisp_mainpath
			return
		fi
	done
}

restore_params()
{
	[ -n "$original_drop" ] &&
		printf '%s' "$original_drop" >"$PARAM_ROOT/l1_fault_drop_heartbeat" ||
		true
	[ -n "$original_timeout" ] &&
		printf '%s' "$original_timeout" >"$PARAM_ROOT/l1_timeout_ms" ||
		true
	[ -n "$original_enable" ] &&
		printf '%s' "$original_enable" >"$PARAM_ROOT/l1_timeout_enable" ||
		true
}

trap restore_params EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail 'run as root'
for command in media-ctl v4l2-ctl timeout awk grep; do
	command -v "$command" >/dev/null 2>&1 || fail "missing command: $command"
done
for parameter in l1_timeout_enable l1_timeout_ms l1_fault_drop_heartbeat; do
	[ -w "$PARAM_ROOT/$parameter" ] ||
		fail "missing writable parameter: $parameter"
done
[ -x "$DUAL_VALIDATOR" ] || fail "missing $DUAL_VALIDATOR"

ISP_NODE=$(find_isp_node)
[ -c "$ISP_NODE" ] || fail 'rkisp_mainpath node not found'
mkdir -p "$OUTPUT_DIR"
FAULT_LOG=$OUTPUT_DIR/fault-injection.log
FAULT_DMESG=$OUTPUT_DIR/fault-dmesg.log
RECOVERY_DIR=$OUTPUT_DIR/recovery
RECOVERY_LOG=$OUTPUT_DIR/recovery-validator.log
SUMMARY=$OUTPUT_DIR/summary.log

original_enable=$(cat "$PARAM_ROOT/l1_timeout_enable")
original_timeout=$(cat "$PARAM_ROOT/l1_timeout_ms")
original_drop=$(cat "$PARAM_ROOT/l1_fault_drop_heartbeat")
dmesg_line=$(( $(dmesg | wc -l) + 1 ))

printf '%s' "$TIMEOUT_MS" >"$PARAM_ROOT/l1_timeout_ms"
printf 1 >"$PARAM_ROOT/l1_timeout_enable"
printf 1 >"$PARAM_ROOT/l1_fault_drop_heartbeat"

fault_status=0
timeout 10 v4l2-ctl -d "$ISP_NODE" \
	--set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
	--verbose --stream-show-delta-now --stream-mmap=4 \
	--stream-count=300 --stream-poll >"$FAULT_LOG" 2>&1 ||
	fault_status=$?

[ "$fault_status" -ne 124 ] ||
	fail 'fault-injected capture hung until userspace timeout'
grep -q 'Input/output error' "$FAULT_LOG" ||
	fail 'userspace did not receive EIO from vb2_queue_error'
fault_dqbuf=$(grep -c '^cap dqbuf:' "$FAULT_LOG" || true)
[ "$fault_dqbuf" -lt 300 ] ||
	fail "fault-injected capture reached all $fault_dqbuf frames"

dmesg | tail -n +"$dmesg_line" >"$FAULT_DMESG"
timeout_events=$(grep -c 'L1 timeout: stream=0 ' "$FAULT_DMESG" || true)
[ "$timeout_events" -eq 1 ] ||
	fail "expected one L1 timeout event, got $timeout_events"
grep -q 'L1 timeout: stream=0 generation=' "$FAULT_DMESG" ||
	fail 'generation-aware timeout marker missing'

printf 0 >"$PARAM_ROOT/l1_fault_drop_heartbeat"
recovery_dmesg_line=$(( $(dmesg | wc -l) + 1 ))
"$DUAL_VALIDATOR" "$RECOVERY_VISIBLE" "$RECOVERY_THERMAL" "$RECOVERY_DIR" \
	>"$RECOVERY_LOG" 2>&1
grep -q '^STAGE6_DUAL_CAPTURE=PASS$' "$RECOVERY_DIR/summary.log" ||
	fail 'close/reopen recovery did not pass dual capture'
if dmesg | tail -n +"$recovery_dmesg_line" |
	grep -q 'L1 timeout: stream=0 '; then
	fail 'healthy reopened session triggered another L1 timeout'
fi

{
	printf 'ISP_NODE=%s\n' "$ISP_NODE"
	printf 'TIMEOUT_MS=%s\n' "$TIMEOUT_MS"
	printf 'FAULT_EXIT=%s\n' "$fault_status"
	printf 'FAULT_DQBUF=%s\n' "$fault_dqbuf"
	printf 'FAULT_TIMEOUT_EVENTS=%s\n' "$timeout_events"
	printf 'USERSPACE_EIO=PASS\n'
	printf 'CLOSE_REOPEN_DUAL_CAPTURE=PASS\n'
	printf 'STALE_TIMEOUT_AFTER_REOPEN=0\n'
	printf 'RKISP_L1_TIMEOUT=PASS\n'
} | tee "$SUMMARY"
