#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

CYCLES=${1:-100}
FRAMES=${2:-3}
OUTPUT_DIR=${3:-/tmp/rkisp-l1-cycles-stage6}
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

positive_integer()
{
	case "$1" in
		''|*[!0-9]*) return 1 ;;
	esac
	[ "$1" -gt 0 ]
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

positive_integer "$CYCLES" || fail 'cycle count must be positive'
positive_integer "$FRAMES" || fail 'frame count must be positive'
[ "$(id -u)" -eq 0 ] || fail 'run as root'
for command in media-ctl v4l2-ctl timeout awk grep sed; do
	command -v "$command" >/dev/null 2>&1 || fail "missing command: $command"
done
[ -x "$DUAL_VALIDATOR" ] || fail "missing $DUAL_VALIDATOR"

ISP_NODE=$(find_isp_node)
[ -c "$ISP_NODE" ] || fail 'rkisp_mainpath node not found'
mkdir -p "$OUTPUT_DIR"
CYCLE_LOG=$OUTPUT_DIR/cycles.log
CYCLE_DMESG=$OUTPUT_DIR/cycles-dmesg.log
DUAL_DIR=$OUTPUT_DIR/dual
DUAL_LOG=$OUTPUT_DIR/dual-validator.log
SUMMARY=$OUTPUT_DIR/summary.log

original_enable=$(cat "$PARAM_ROOT/l1_timeout_enable")
original_timeout=$(cat "$PARAM_ROOT/l1_timeout_ms")
original_drop=$(cat "$PARAM_ROOT/l1_fault_drop_heartbeat")
printf 500 >"$PARAM_ROOT/l1_timeout_ms"
printf 1 >"$PARAM_ROOT/l1_timeout_enable"
printf 0 >"$PARAM_ROOT/l1_fault_drop_heartbeat"

dmesg_line=$(( $(dmesg | wc -l) + 1 ))
: >"$CYCLE_LOG"
i=1
while [ "$i" -le "$CYCLES" ]; do
	printf 'CYCLE=%s START\n' "$i" >>"$CYCLE_LOG"
	if ! timeout 5 v4l2-ctl -d "$ISP_NODE" \
		--set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
		--stream-mmap=4 --stream-count="$FRAMES" --stream-poll \
		>>"$CYCLE_LOG" 2>&1; then
		fail "cycle $i failed; see $CYCLE_LOG"
	fi
	printf 'CYCLE=%s PASS\n' "$i" >>"$CYCLE_LOG"
	i=$((i + 1))
done

dmesg | tail -n +"$dmesg_line" >"$CYCLE_DMESG"
armed_count=$(grep -c 'L1 timeout armed: stream=0 ' "$CYCLE_DMESG" || true)
timeout_count=$(grep -c 'L1 timeout: stream=0 ' "$CYCLE_DMESG" || true)
[ "$armed_count" -eq "$CYCLES" ] ||
	fail "armed count $armed_count, expected $CYCLES"
[ "$timeout_count" -eq 0 ] ||
	fail "healthy cycles triggered $timeout_count timeouts"

set -- $(sed -n 's/.*L1 timeout armed: stream=0 generation=\([0-9][0-9]*\).*/\1/p' \
	"$CYCLE_DMESG" | awk '
		NR == 1 { first = $1 }
		{ last = $1; count++ }
		END { print first + 0, last + 0, count + 0 }
	')
first_generation=$1
last_generation=$2
generation_count=$3
[ "$generation_count" -eq "$CYCLES" ] ||
	fail "generation count $generation_count, expected $CYCLES"
[ "$last_generation" -eq $((first_generation + CYCLES - 1)) ] ||
	fail "generation sequence is not contiguous: $first_generation..$last_generation"

dual_dmesg_line=$(( $(dmesg | wc -l) + 1 ))
"$DUAL_VALIDATOR" 300 80 "$DUAL_DIR" >"$DUAL_LOG" 2>&1
grep -q '^STAGE6_DUAL_CAPTURE=PASS$' "$DUAL_DIR/summary.log" ||
	fail 'post-cycle dual capture did not pass'
if dmesg | tail -n +"$dual_dmesg_line" |
	grep -q 'L1 timeout: stream=0 '; then
	fail 'post-cycle dual capture triggered L1 timeout'
fi

{
	printf 'ISP_NODE=%s\n' "$ISP_NODE"
	printf 'CYCLES=%s\n' "$CYCLES"
	printf 'FRAMES_PER_CYCLE=%s\n' "$FRAMES"
	printf 'ARMED_COUNT=%s\n' "$armed_count"
	printf 'GENERATION=%s..%s\n' "$first_generation" "$last_generation"
	printf 'HEALTHY_TIMEOUTS=%s\n' "$timeout_count"
	printf 'POST_CYCLE_DUAL_CAPTURE=PASS\n'
	printf 'RKISP_L1_CYCLES=PASS\n'
} | tee "$SUMMARY"
