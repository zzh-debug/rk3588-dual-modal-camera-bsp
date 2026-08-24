#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -u

MINUTES=${1:-120}
OUTPUT_DIR=${2:-/tmp/dual-soak-stage6-2h}
PARAM_ROOT=/sys/module/video_rkisp/parameters
DUAL_VALIDATOR=/usr/local/sbin/verify_dual_capture_stage6
STATE=STARTING
RESULT_RC=1
original_enable=
original_timeout=
original_drop=
validator_pid=

positive_integer()
{
	case "$1" in
		''|*[!0-9]*) return 1 ;;
	esac
	[ "$1" -gt 0 ]
}

write_status()
{
	{
		printf 'STATE=%s\n' "$STATE"
		printf 'MINUTES=%s\n' "$MINUTES"
		printf 'VISIBLE_TARGET=%s\n' "$VISIBLE_FRAMES"
		printf 'THERMAL_TARGET=%s\n' "$THERMAL_PAIRS"
		printf 'WORKER_PID=%s\n' "$$"
		printf 'VALIDATOR_PID=%s\n' "${validator_pid:-}"
		printf 'START_UPTIME=%s\n' "${start_uptime:-}"
		printf 'END_UPTIME=%s\n' "${end_uptime:-}"
		printf 'RESOURCE_SAMPLES=%s\n' "${resource_samples:-0}"
		printf 'L1_TIMEOUTS=%s\n' "${l1_timeouts:-}"
		printf 'RESULT_RC=%s\n' "$RESULT_RC"
		printf 'UPDATED_UPTIME=%s\n' "$(awk '{ print $1 }' /proc/uptime)"
	} >"$STATUS.tmp"
	mv "$STATUS.tmp" "$STATUS"
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

cleanup()
{
	rc=$?
	restore_params
	if [ "$STATE" != COMPLETE ]; then
		STATE=FAILED
		RESULT_RC=$rc
		end_uptime=$(awk '{ print $1 }' /proc/uptime)
		write_status
	fi
}

trap cleanup EXIT INT TERM

positive_integer "$MINUTES" || exit 2
[ "$(id -u)" -eq 0 ] || exit 2
[ -x "$DUAL_VALIDATOR" ] || exit 2
for command in awk grep gzip sha256sum; do
	command -v "$command" >/dev/null 2>&1 || exit 2
done

VISIBLE_FRAMES=$((MINUTES * 60 * 30))
THERMAL_PAIRS=$((MINUTES * 60 * 8))
mkdir -p "$OUTPUT_DIR"
STATUS=$OUTPUT_DIR/status.env
RUNNER_LOG=$OUTPUT_DIR/runner.log
RESOURCE_LOG=$OUTPUT_DIR/resources.log
L1_LOG=$OUTPUT_DIR/l1-timeouts.log
DUAL_DIR=$OUTPUT_DIR/dual
VALIDATOR_LOG=$OUTPUT_DIR/validator.log
SHA_FILE=$OUTPUT_DIR/SHA256SUMS

if [ -f "$STATUS" ] &&
	grep -q '^STATE=RUNNING$' "$STATUS" 2>/dev/null; then
	exit 3
fi

original_enable=$(cat "$PARAM_ROOT/l1_timeout_enable")
original_timeout=$(cat "$PARAM_ROOT/l1_timeout_ms")
original_drop=$(cat "$PARAM_ROOT/l1_fault_drop_heartbeat")
printf 1000 >"$PARAM_ROOT/l1_timeout_ms"
printf 1 >"$PARAM_ROOT/l1_timeout_enable"
printf 0 >"$PARAM_ROOT/l1_fault_drop_heartbeat"

start_uptime=$(awk '{ print $1 }' /proc/uptime)
dmesg_line=$(( $(dmesg | wc -l) + 1 ))
resource_samples=0
l1_timeouts=
: >"$RESOURCE_LOG"
: >"$L1_LOG"
STATE=RUNNING
RESULT_RC=1
write_status

"$DUAL_VALIDATOR" "$VISIBLE_FRAMES" "$THERMAL_PAIRS" "$DUAL_DIR" \
	>"$VALIDATOR_LOG" 2>&1 &
validator_pid=$!
write_status

while kill -0 "$validator_pid" 2>/dev/null; do
	{
		printf 'UPTIME='
		cat /proc/uptime
		printf 'LOAD='
		cat /proc/loadavg
		grep -E 'MemAvailable|MemFree|Slab|SReclaimable' /proc/meminfo
		for zone in /sys/class/thermal/thermal_zone*; do
			[ -r "$zone/type" ] && [ -r "$zone/temp" ] &&
				printf 'THERMAL %s %s\n' \
				"$(cat "$zone/type")" "$(cat "$zone/temp")"
		done
		echo ---
	} >>"$RESOURCE_LOG"
	resource_samples=$((resource_samples + 1))
	write_status
	sleep 60
done

RESULT_RC=0
wait "$validator_pid" || RESULT_RC=$?
validator_pid=
end_uptime=$(awk '{ print $1 }' /proc/uptime)
dmesg | tail -n +"$dmesg_line" |
	grep 'L1 timeout: stream=0 ' >"$L1_LOG" || true
l1_timeouts=$(wc -l <"$L1_LOG")

if [ "$RESULT_RC" -ne 0 ] ||
	[ "$l1_timeouts" -ne 0 ] ||
	! grep -q '^STAGE6_DUAL_CAPTURE=PASS$' "$DUAL_DIR/summary.log"; then
	STATE=FAILED
	write_status
	exit 1
fi

gzip -9 "$DUAL_DIR/visible-nv12.log"
gzip -9 "$DUAL_DIR/thermal-meta.log"
(
	cd "$OUTPUT_DIR" || exit 1
	sha256sum \
		dual/summary.log \
		dual/visible-nv12.log.gz \
		dual/thermal-meta.log.gz \
		dual/new-kernel-errors.log \
		resources.log \
		l1-timeouts.log \
		validator.log >"$SHA_FILE"
)

STATE=COMPLETE
RESULT_RC=0
write_status
exit 0
