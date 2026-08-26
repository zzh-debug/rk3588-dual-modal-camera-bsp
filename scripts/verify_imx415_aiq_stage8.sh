#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

OUTPUT_DIR=${1:-/tmp/imx415-aiq-stage8}
PROBE_TOOL=${PROBE_TOOL:-/usr/bin/verify_imx415_aiq_module}
IQ_BASENAME=imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json
IQ_FILE=/etc/iqfiles/$IQ_BASENAME
CAPTURE_FRAMES=${CAPTURE_FRAMES:-180}

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

find_media_with_entity()
{
	entity=$1

	for media in /dev/media*; do
		[ -c "$media" ] || continue
		if media-ctl -d "$media" -p 2>/dev/null | grep -Fq "$entity"; then
			printf '%s\n' "$media"
			return 0
		fi
	done
	return 1
}

snapshot_aiq_logs()
{
	for log_file in /var/log/messages /var/log/messages.*; do
		[ -r "$log_file" ] || continue
		cat "$log_file"
	done
	logread 2>/dev/null || true
}

[ "$(id -u)" -eq 0 ] || fail 'run as root on the development board'
for command in media-ctl v4l2-ctl timeout awk grep sha256sum pidof; do
	command -v "$command" >/dev/null 2>&1 || fail "$command is missing"
done
[ -x "$PROBE_TOOL" ] || fail "missing module-contract probe: $PROBE_TOOL"
[ -r "$IQ_FILE" ] || fail "missing IQ file: $IQ_FILE"

ROOT_TYPE=$(awk '$2 == "/" { print $3; exit }' /proc/mounts)
case "$ROOT_TYPE" in
	nfs|nfs4) ;;
	*) fail "root filesystem is $ROOT_TYPE, expected NFS" ;;
esac

AIQ_PID=$(pidof rkaiq_3A_server 2>/dev/null || true)
[ -n "$AIQ_PID" ] || fail 'rkaiq_3A_server is not running'

SENSOR_SUBDEV=
SENSOR_ENTITY=
for name_file in /sys/class/video4linux/v4l-subdev*/name; do
	[ -f "$name_file" ] || continue
	name=$(cat "$name_file")
	case "$name" in
		m[0-9][0-9]_[bf]_zzh_imx415\ *)
			SENSOR_SUBDEV=/dev/$(basename "$(dirname "$name_file")")
			SENSOR_ENTITY=$name
			break
			;;
	esac
done
[ -c "$SENSOR_SUBDEV" ] || fail 'AIQ-compatible IMX415 Sensor entity is absent'

RKCIF_MEDIA=$(find_media_with_entity "$SENSOR_ENTITY") ||
	fail 'cannot find Sensor media graph'
RKISP_MEDIA=$(find_media_with_entity rkisp_mainpath) ||
	fail 'cannot find RKISP media graph'
ISP_NODE=$(find_entity_node "$RKISP_MEDIA" rkisp_mainpath)
[ -c "$ISP_NODE" ] || fail "invalid RKISP mainpath: $ISP_NODE"

mkdir -p "$OUTPUT_DIR"
uname -a | tee "$OUTPUT_DIR/kernel.log"
printf 'ROOT_TYPE=%s\n' "$ROOT_TYPE" | tee "$OUTPUT_DIR/runtime.log"
printf 'AIQ_PID=%s\n' "$AIQ_PID" | tee -a "$OUTPUT_DIR/runtime.log"
printf 'SENSOR_ENTITY=%s SENSOR_SUBDEV=%s\n' \
	"$SENSOR_ENTITY" "$SENSOR_SUBDEV" | tee -a "$OUTPUT_DIR/runtime.log"
printf 'RKCIF_MEDIA=%s RKISP_MEDIA=%s ISP_NODE=%s\n' \
	"$RKCIF_MEDIA" "$RKISP_MEDIA" "$ISP_NODE" | tee -a "$OUTPUT_DIR/runtime.log"
sha256sum "$IQ_FILE" | tee "$OUTPUT_DIR/iq-sha256.log"
"$PROBE_TOOL" "$SENSOR_SUBDEV" | tee "$OUTPUT_DIR/module-contract.log"
media-ctl -d "$RKCIF_MEDIA" -p >"$OUTPUT_DIR/rkcif-graph.log"
media-ctl -d "$RKISP_MEDIA" -p >"$OUTPUT_DIR/rkisp-graph.log"
v4l2-ctl -d "$SENSOR_SUBDEV" --all >"$OUTPUT_DIR/sensor-controls-before.log"
snapshot_aiq_logs >"$OUTPUT_DIR/aiq-logs-before.log"

DMESG_LINE=$(( $(dmesg | wc -l) + 1 ))
CAPTURE_LOG=$OUTPUT_DIR/nv12-$CAPTURE_FRAMES.log
if timeout 30 v4l2-ctl -d "$ISP_NODE" \
	--set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
	--verbose --stream-show-delta-now --stream-mmap=4 --stream-skip=8 \
	--stream-count="$CAPTURE_FRAMES" --stream-poll >"$CAPTURE_LOG" 2>&1; then
	status=0
else
	status=$?
fi
[ "$status" -eq 0 ] || fail "NV12 capture exited with status $status"

DQBUF_COUNT=$(grep -c 'cap dqbuf:' "$CAPTURE_LOG" || true)
[ "$DQBUF_COUNT" -ge "$CAPTURE_FRAMES" ] ||
	fail "only $DQBUF_COUNT DQBUF records, expected at least $CAPTURE_FRAMES"
v4l2-ctl -d "$SENSOR_SUBDEV" --all >"$OUTPUT_DIR/sensor-controls-after.log"
snapshot_aiq_logs >"$OUTPUT_DIR/aiq-logs-after.log"

AIQ_PID_AFTER=$(pidof rkaiq_3A_server 2>/dev/null || true)
[ -n "$AIQ_PID_AFTER" ] || fail 'rkaiq_3A_server exited during capture'
IQ_LOG=$(sed 's#/etc/iqfiles//*#/etc/iqfiles/#g' \
	"$OUTPUT_DIR/aiq-logs-after.log" | \
	grep -F "rk_aiq_uapi_sysctl_init success. iq:$IQ_FILE" | tail -n 1 || true)
[ -n "$IQ_LOG" ] || fail "AIQ log does not confirm $IQ_FILE"

FATAL_LOG=$(dmesg | tail -n +"$DMESG_LINE" | grep -Ei \
	'zzh_imx415.*(error|fail|timeout)|rkisp.*(error|fail|overflow|timeout)|iommu.*fault|Oops|BUG:|Kernel panic' || true)
[ -z "$FATAL_LOG" ] || fail "fatal kernel log: $FATAL_LOG"

printf 'DQBUF_COUNT=%s\n' "$DQBUF_COUNT" | tee -a "$OUTPUT_DIR/runtime.log"
printf 'AIQ_PID_AFTER=%s\n' "$AIQ_PID_AFTER" | tee -a "$OUTPUT_DIR/runtime.log"
printf '%s\n' "$IQ_LOG" | tee "$OUTPUT_DIR/iq-selected.log"
printf 'IMX415_AIQ_STAGE8=PASS\n' | tee "$OUTPUT_DIR/result.log"
