#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

I2C_DEVICE=/sys/bus/i2c/devices/3-001a
SKIP_FRAMES=8
SHORT_FRAMES=${3:-300}
LONG_FRAMES=${4:-3000}
OUTPUT_DIR=${5:-/tmp/imx415-stage3}
RAW_NODE=${1:-}
ISP_NODE=${2:-}
RAW_BYTES=10506240
NV12_BYTES=12441600
Y_BYTES=8294400

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

find_media_with_entities()
{
	first=$1
	second=$2

	for media in /dev/media*; do
		[ -c "$media" ] || continue
		graph=$(media-ctl -d "$media" -p 2>/dev/null || true)
		if printf '%s\n' "$graph" | grep -Fq "$first" &&
			printf '%s\n' "$graph" | grep -Fq "$second"; then
			printf '%s\n' "$media"
			return 0
		fi
	done
	return 1
}

new_fatal_log()
{
	start_line=$1

	dmesg | tail -n +"$start_line" | grep -Ei \
		'zzh_imx415.*(error|fail|timeout)|rkcif-mipi-lvds2.*(error|fail|overflow|timeout)|rkisp0-vir0.*(error|fail|overflow|timeout)|iommu.*fault|Oops|BUG:|Kernel panic' || true
}

run_stream_check()
{
	node=$1
	pixfmt=$2
	frames=$3
	expected_bytes=$4
	label=$5
	capture_file=${6:-}
	expected_buffers=$((SKIP_FRAMES + frames))
	timeout_seconds=$((frames / 20 + 30))
	log=$OUTPUT_DIR/$label.log
	dmesg_line=$(( $(dmesg | wc -l) + 1 ))
	start=$(awk '{ print $1 }' /proc/uptime)

	if [ -n "$capture_file" ]; then
		if timeout "$timeout_seconds" v4l2-ctl -d "$node" \
			--set-fmt-video=width=3840,height=2160,pixelformat="$pixfmt" \
			--verbose --stream-show-delta-now --stream-mmap=4 \
			--stream-skip="$SKIP_FRAMES" --stream-count="$frames" \
			--stream-poll --stream-to="$capture_file" >"$log" 2>&1; then
			status=0
		else
			status=$?
		fi
	else
		if timeout "$timeout_seconds" v4l2-ctl -d "$node" \
			--set-fmt-video=width=3840,height=2160,pixelformat="$pixfmt" \
			--verbose --stream-show-delta-now --stream-mmap=4 \
			--stream-skip="$SKIP_FRAMES" --stream-count="$frames" \
			--stream-poll >"$log" 2>&1; then
			status=0
		else
			status=$?
		fi
	fi
	end=$(awk '{ print $1 }' /proc/uptime)

	[ "$status" -eq 0 ] || fail "$label exited with status $status; see $log"

	set -- $(awk -v expected="$expected_bytes" '
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
	' "$log")
	actual_buffers=$1
	first_sequence=$2
	last_sequence=$3
	sequence_gaps=$4
	bad_bytes=$5

	[ "$actual_buffers" -eq "$expected_buffers" ] ||
		fail "$label got $actual_buffers DQBUFs, expected $expected_buffers"
	[ "$first_sequence" -eq 0 ] || fail "$label first sequence is $first_sequence"
	[ "$last_sequence" -eq $((expected_buffers - 1)) ] ||
		fail "$label last sequence is $last_sequence"
	[ "$sequence_gaps" -eq 0 ] || fail "$label has $sequence_gaps sequence gaps"
	[ "$bad_bytes" -eq 0 ] || fail "$label has $bad_bytes bad bytesused values"

	if [ -n "$capture_file" ]; then
		[ "$(stat -c %s "$capture_file")" -eq $((expected_bytes * frames)) ] ||
			fail "$label capture file has an unexpected size"
	fi

	fatal_log=$(new_fatal_log "$dmesg_line")
	[ -z "$fatal_log" ] || fail "$label generated a fatal kernel log: $fatal_log"

	printf '%s: PASS, uptime=%s..%s, dqbuf=%s, seq=%s..%s, bytes=%s\n' \
		"$label" "$start" "$end" "$actual_buffers" "$first_sequence" \
		"$last_sequence" "$expected_bytes"
}

y_mean()
{
	file=$1

	od -An -v -tu1 -N "$Y_BYTES" "$file" | awk '
		{ for (i = 1; i <= NF; i++) { sum += $i; count++ } }
		END { if (!count) exit 1; printf "%.3f\n", sum / count }
	'
}

average_delta()
{
	log=$1

	awk '
		/cap dqbuf:/ {
			for (i = 1; i <= NF; i++) {
				if ($i == "delta:") { sum += $(i + 1); count++ }
			}
		}
		END { if (!count) exit 1; printf "%.3f\n", sum / count }
	' "$log"
}

positive_integer "$SHORT_FRAMES" || fail "short frame count must be positive"
positive_integer "$LONG_FRAMES" || fail "long frame count must be positive"
[ "$(id -u)" -eq 0 ] || fail "run as root on the development board"
for required_command in media-ctl v4l2-ctl timeout awk od stat; do
	command -v "$required_command" >/dev/null 2>&1 || fail "$required_command is missing"
done

ROOT_TYPE=$(awk '$2 == "/" { print $3; exit }' /proc/mounts)
case "$ROOT_TYPE" in
	nfs|nfs4) ;;
	*) fail "root filesystem is $ROOT_TYPE, expected NFS" ;;
esac

[ -d "$I2C_DEVICE" ] || fail "IMX415 I2C device is absent"
[ "$(basename "$(readlink -f "$I2C_DEVICE/driver")")" = zzh_imx415 ] ||
	fail "3-001a is not bound to zzh_imx415"

RKCIF_MEDIA=$(find_media_with_entities 'zzh_imx415 3-001a' 'stream_cif_mipi_id0') ||
	fail "cannot find the IMX415 RKCIF graph"
RKISP_MEDIA=$(find_media_with_entities 'rkcif-mipi-lvds2' 'rkisp_mainpath') ||
	fail "cannot find the RKCIF-to-RKISP graph"
[ -n "$RAW_NODE" ] || RAW_NODE=$(find_entity_node "$RKCIF_MEDIA" stream_cif_mipi_id0)
[ -n "$ISP_NODE" ] || ISP_NODE=$(find_entity_node "$RKISP_MEDIA" rkisp_mainpath)
[ -c "$RAW_NODE" ] || fail "RAW node is invalid: $RAW_NODE"
[ -c "$ISP_NODE" ] || fail "ISP node is invalid: $ISP_NODE"

SENSOR_SUBDEV=
for name_file in /sys/class/video4linux/v4l-subdev*/name; do
	[ -f "$name_file" ] || continue
	case "$(cat "$name_file")" in
		*zzh_imx415*) SENSOR_SUBDEV=/dev/$(basename "$(dirname "$name_file")"); break ;;
	esac
done
[ -c "$SENSOR_SUBDEV" ] || fail "cannot find the IMX415 Sensor subdev"

mkdir -p "$OUTPUT_DIR"
printf 'RKCIF_MEDIA=%s RAW_NODE=%s\n' "$RKCIF_MEDIA" "$RAW_NODE"
printf 'RKISP_MEDIA=%s ISP_NODE=%s SENSOR_SUBDEV=%s\n' \
	"$RKISP_MEDIA" "$ISP_NODE" "$SENSOR_SUBDEV"
media-ctl -d "$RKCIF_MEDIA" -p >"$OUTPUT_DIR/rkcif-graph.log"
media-ctl -d "$RKISP_MEDIA" -p >"$OUTPUT_DIR/rkisp-graph.log"

ORIGINAL_VBLANK=$(v4l2-ctl -d "$SENSOR_SUBDEV" --get-ctrl=vertical_blanking | awk '{ print $2 }')
ORIGINAL_EXPOSURE=$(v4l2-ctl -d "$SENSOR_SUBDEV" --get-ctrl=exposure | awk '{ print $2 }')
ORIGINAL_GAIN=$(v4l2-ctl -d "$SENSOR_SUBDEV" --get-ctrl=analogue_gain | awk '{ print $2 }')

restore_controls()
{
	v4l2-ctl -d "$SENSOR_SUBDEV" \
		--set-ctrl=vertical_blanking="$ORIGINAL_VBLANK",exposure="$ORIGINAL_EXPOSURE",analogue_gain="$ORIGINAL_GAIN" \
		>/dev/null 2>&1 || true
}
trap restore_controls EXIT HUP INT TERM

RAW_CAPTURE=$OUTPUT_DIR/imx415-gb10.raw
NV12_CAPTURE=$OUTPUT_DIR/imx415-nv12.raw
run_stream_check "$RAW_NODE" GB10 1 "$RAW_BYTES" raw-single "$RAW_CAPTURE"
run_stream_check "$RAW_NODE" GB10 "$SHORT_FRAMES" "$RAW_BYTES" raw-short
run_stream_check "$RAW_NODE" GB10 "$LONG_FRAMES" "$RAW_BYTES" raw-long
run_stream_check "$ISP_NODE" NV12 1 "$NV12_BYTES" nv12-single "$NV12_CAPTURE"
run_stream_check "$ISP_NODE" NV12 "$SHORT_FRAMES" "$NV12_BYTES" nv12-short
run_stream_check "$ISP_NODE" NV12 "$LONG_FRAMES" "$NV12_BYTES" nv12-long

v4l2-ctl -d "$SENSOR_SUBDEV" --set-ctrl=exposure=200,analogue_gain=0
run_stream_check "$ISP_NODE" NV12 1 "$NV12_BYTES" control-exp200 \
	"$OUTPUT_DIR/control-exp200.nv12"
LOW_MEAN=$(y_mean "$OUTPUT_DIR/control-exp200.nv12")

v4l2-ctl -d "$SENSOR_SUBDEV" --set-ctrl=exposure=2000,analogue_gain=0
run_stream_check "$ISP_NODE" NV12 1 "$NV12_BYTES" control-exp2000 \
	"$OUTPUT_DIR/control-exp2000.nv12"
HIGH_MEAN=$(y_mean "$OUTPUT_DIR/control-exp2000.nv12")

v4l2-ctl -d "$SENSOR_SUBDEV" --set-ctrl=exposure=200,analogue_gain=120
run_stream_check "$ISP_NODE" NV12 1 "$NV12_BYTES" control-gain120 \
	"$OUTPUT_DIR/control-gain120.nv12"
GAIN_MEAN=$(y_mean "$OUTPUT_DIR/control-gain120.nv12")

awk -v low="$LOW_MEAN" -v high="$HIGH_MEAN" 'BEGIN { exit !(high > low) }' ||
	fail "exposure did not increase Y mean: $LOW_MEAN -> $HIGH_MEAN"
awk -v low="$LOW_MEAN" -v gain="$GAIN_MEAN" 'BEGIN { exit !(gain > low) }' ||
	fail "analogue gain did not increase Y mean: $LOW_MEAN -> $GAIN_MEAN"
printf 'controls: PASS, Y mean exp200=%s exp2000=%s gain120=%s\n' \
	"$LOW_MEAN" "$HIGH_MEAN" "$GAIN_MEAN"

v4l2-ctl -d "$SENSOR_SUBDEV" \
	--set-ctrl=exposure=200,vertical_blanking=2308,analogue_gain=0
run_stream_check "$ISP_NODE" NV12 30 "$NV12_BYTES" vblank-2308
SLOW_DELTA=$(average_delta "$OUTPUT_DIR/vblank-2308.log")
awk -v delta="$SLOW_DELTA" 'BEGIN { exit !(delta > 60 && delta < 74) }' ||
	fail "VBLANK frame interval is unexpected: $SLOW_DELTA ms"

restore_controls
run_stream_check "$ISP_NODE" NV12 10 "$NV12_BYTES" vblank-restored
NORMAL_DELTA=$(average_delta "$OUTPUT_DIR/vblank-restored.log")
awk -v delta="$NORMAL_DELTA" 'BEGIN { exit !(delta > 28 && delta < 40) }' ||
	fail "restored frame interval is unexpected: $NORMAL_DELTA ms"
printf 'vblank: PASS, extended=%s ms restored=%s ms\n' \
	"$SLOW_DELTA" "$NORMAL_DELTA"

[ "$(cat "$I2C_DEVICE/power/runtime_status")" = suspended ] ||
	fail "Sensor runtime PM did not return to suspended"
ISP_ERRORS=$(sed -n 's/.*ErrCnt:\([0-9][0-9]*\).*/\1/p' /proc/rkisp0-vir0 | tail -n 1)
[ -n "$ISP_ERRORS" ] || fail "cannot read RKISP error count"
[ "$ISP_ERRORS" -eq 0 ] || fail "RKISP ErrCnt is $ISP_ERRORS"

restore_controls
trap - EXIT HUP INT TERM
printf '%s\n' 'STAGE3_RUNTIME_CHECK=PASS'
