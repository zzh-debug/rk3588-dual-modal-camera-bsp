#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

VISIBLE_FRAMES=${1:-300}
THERMAL_PAIRS=${2:-80}
OUTPUT_DIR=${3:-/tmp/dual-capture-stage6}
SKIP_FRAMES=8
NV12_BYTES=12441600
ISP_NODE=
THERMAL_VALIDATOR=/usr/local/sbin/verify_mlx90640_stage5

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

new_fatal_log()
{
	start_line=$1

	dmesg | tail -n +"$start_line" | grep -Ei \
		'zzh_(imx415|mlx90640).*(error|fail|timeout)|rkcif-mipi-lvds2.*(error|fail|overflow|timeout)|rkisp0-vir0.*(error|fail|overflow|timeout)|iommu.*fault|Oops|BUG:|Kernel panic' || true
}

positive_integer "$VISIBLE_FRAMES" || fail 'visible frame count must be positive'
positive_integer "$THERMAL_PAIRS" || fail 'thermal pair count must be positive'

for command in media-ctl v4l2-ctl timeout awk grep; do
	command -v "$command" >/dev/null 2>&1 || fail "missing command: $command"
done
[ -x "$THERMAL_VALIDATOR" ] || fail "missing $THERMAL_VALIDATOR"

[ -n "$(readlink /sys/bus/i2c/devices/3-001a/driver 2>/dev/null || true)" ] ||
	fail 'IMX415 3-001a is not bound'
[ -n "$(readlink /sys/bus/i2c/devices/5-0033/driver 2>/dev/null || true)" ] ||
	fail 'MLX90640 5-0033 is not bound'

ISP_NODE=$(find_isp_node)
[ -n "$ISP_NODE" ] || fail 'rkisp_mainpath node not found'
[ -c "$ISP_NODE" ] || fail "$ISP_NODE is not a video device"

mkdir -p "$OUTPUT_DIR"
VISIBLE_LOG=$OUTPUT_DIR/visible-nv12.log
THERMAL_LOG=$OUTPUT_DIR/thermal-meta.log
DMESG_LOG=$OUTPUT_DIR/new-kernel-errors.log
SUMMARY=$OUTPUT_DIR/summary.log
dmesg_line=$(( $(dmesg | wc -l) + 1 ))
expected_visible=$((SKIP_FRAMES + VISIBLE_FRAMES))
visible_timeout=$((VISIBLE_FRAMES / 20 + 45))
thermal_timeout=$((THERMAL_PAIRS / 4 + 45))
start=$(awk '{ print $1 }' /proc/uptime)

timeout "$thermal_timeout" "$THERMAL_VALIDATOR" \
	"$THERMAL_PAIRS" 1 >"$THERMAL_LOG" 2>&1 &
thermal_pid=$!

timeout "$visible_timeout" v4l2-ctl -d "$ISP_NODE" \
	--set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
	--verbose --stream-show-delta-now --stream-mmap=4 \
	--stream-skip="$SKIP_FRAMES" --stream-count="$VISIBLE_FRAMES" \
	--stream-poll >"$VISIBLE_LOG" 2>&1 &
visible_pid=$!

visible_status=0
thermal_status=0
wait "$visible_pid" || visible_status=$?
wait "$thermal_pid" || thermal_status=$?
end=$(awk '{ print $1 }' /proc/uptime)

[ "$visible_status" -eq 0 ] ||
	fail "visible capture exited $visible_status; see $VISIBLE_LOG"
[ "$thermal_status" -eq 0 ] ||
	fail "thermal capture exited $thermal_status; see $THERMAL_LOG"

set -- $(awk -v expected="$NV12_BYTES" '
	/cap dqbuf:/ {
		seq = -1
		bytes = -1
		for (i = 1; i <= NF; i++) {
			if ($i == "seq:") seq = $(i + 1) + 0
			if ($i == "bytesused:") bytes = $(i + 1) + 0
		}
		if (count == 0) first = seq
		if (count > 0 && seq != previous + 1) gaps++
		if (bytes != expected) bad_bytes++
		previous = seq
		last = seq
		count++
	}
	END { print count + 0, first + 0, last + 0, gaps + 0, bad_bytes + 0 }
' "$VISIBLE_LOG")
visible_count=$1
visible_first=$2
visible_last=$3
visible_gaps=$4
visible_bad_bytes=$5

[ "$visible_count" -eq "$expected_visible" ] ||
	fail "visible DQBUF count $visible_count, expected $expected_visible"
[ "$visible_first" -eq 0 ] || fail "visible first sequence $visible_first"
[ "$visible_last" -eq $((expected_visible - 1)) ] ||
	fail "visible last sequence $visible_last"
[ "$visible_gaps" -eq 0 ] || fail "visible sequence gaps $visible_gaps"
[ "$visible_bad_bytes" -eq 0 ] || fail "visible bad bytesused $visible_bad_bytes"

thermal_count=$(grep -c '^CYCLE=0 PAIR=' "$THERMAL_LOG")
[ "$thermal_count" -eq "$THERMAL_PAIRS" ] ||
	fail "thermal pair count $thermal_count, expected $THERMAL_PAIRS"
grep -q '^STAGE5_RUNTIME_CHECK=PASS$' "$THERMAL_LOG" ||
	fail 'thermal validator did not report PASS'

fatal_log=$(new_fatal_log "$dmesg_line")
printf '%s\n' "$fatal_log" >"$DMESG_LOG"
[ -z "$fatal_log" ] || fail "new fatal kernel log; see $DMESG_LOG"

isp_errors=$(sed -n 's/.*ErrCnt:\([0-9][0-9]*\).*/\1/p' \
	/proc/rkisp0-vir0 | tail -n 1)
[ "${isp_errors:-0}" -eq 0 ] || fail "RKISP ErrCnt=$isp_errors"

mlx_stop=$(dmesg | tail -n +"$dmesg_line" | \
	grep 'zzh_mlx90640.*stream stopped:' | tail -n 1)
printf '%s\n' "$mlx_stop" | grep -q 'no-buffer=0 duplicate=0' ||
	fail "unexpected MLX stop counters: $mlx_stop"

{
	printf 'ISP_NODE=%s\n' "$ISP_NODE"
	printf 'UPTIME=%s..%s\n' "$start" "$end"
	printf 'VISIBLE_DQBUF=%s\n' "$visible_count"
	printf 'VISIBLE_SEQUENCE=%s..%s\n' "$visible_first" "$visible_last"
	printf 'VISIBLE_GAPS=%s\n' "$visible_gaps"
	printf 'VISIBLE_BAD_BYTES=%s\n' "$visible_bad_bytes"
	printf 'THERMAL_PAIRS=%s\n' "$thermal_count"
	printf 'RKISP_ERRCNT=%s\n' "${isp_errors:-0}"
	printf 'MLX_STOP=%s\n' "$mlx_stop"
	printf 'STAGE6_DUAL_CAPTURE=PASS\n'
} | tee "$SUMMARY"
