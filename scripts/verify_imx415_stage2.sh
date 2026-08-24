#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

MODULE=zzh_imx415
BUILTIN_MODULE=zzh_imx415_builtin
I2C_DEVICE=/sys/bus/i2c/devices/3-001a
I2C_DT_ROOT=/proc/device-tree/i2c@feab0000
DT_NODE=$I2C_DT_ROOT/imx415-stage2@1a
VENDOR_DT_NODE=$I2C_DT_ROOT/imx415@1a
CYCLES=${1:-10}
RAW_NODE=${2:-}
FRAMES=${3:-12}
SKIP_FRAMES=8

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

dt_string()
{
	tr -d '\000' <"$1"
}

dt_node_available()
{
	node=$1
	[ -d "$node" ] || return 1
	[ ! -f "$node/status" ] || [ "$(dt_string "$node/status")" = okay ]
}

find_sensor_subdev()
{
	for name_file in /sys/class/video4linux/v4l-subdev*/name; do
		[ -f "$name_file" ] || continue
		name=$(cat "$name_file")
		case "$name" in
			*zzh_imx415*3-001a*|*zzh_imx415*)
				basename "$(dirname "$name_file")"
				return 0
				;;
		esac
	done
	return 1
}

find_sensor_media()
{
	sensor_name=$1
	for media in /dev/media*; do
		[ -c "$media" ] || continue
		if media-ctl -d "$media" -p 2>/dev/null |
			grep -Fq "$sensor_name"; then
			printf '%s\n' "$media"
			return 0
		fi
	done
	return 1
}

find_rkcif_raw_node()
{
	media=$1
	media-ctl -d "$media" -p 2>/dev/null | awk '
		/entity [0-9]+: stream_cif_mipi_id0 / { in_entity = 1; next }
		in_entity && /device node name/ {
			sub(/^.*device node name[[:space:]]+/, "")
			print
			exit
		}
		in_entity && /entity [0-9]+:/ { in_entity = 0 }
	'
}

positive_integer "$CYCLES" || fail "cycle count must be a positive integer"
positive_integer "$FRAMES" || fail "frame count must be a positive integer"
[ "$(id -u)" -eq 0 ] || fail "run as root on the development board"
command -v media-ctl >/dev/null 2>&1 || fail "media-ctl is missing"
command -v v4l2-ctl >/dev/null 2>&1 || fail "v4l2-ctl is missing"
command -v timeout >/dev/null 2>&1 || fail "timeout is missing"

ROOT_TYPE=$(awk '$2 == "/" { print $3; exit }' /proc/mounts)
case "$ROOT_TYPE" in
	nfs|nfs4) ;;
	*) fail "root filesystem is $ROOT_TYPE, expected NFS test root" ;;
esac

[ -r /etc/rootfs-source ] || fail "/etc/rootfs-source is missing"
[ -d "$DT_NODE" ] || fail "stage-two DT node is absent: $DT_NODE"
[ "$(dt_string "$DT_NODE/compatible")" = 'zzh,imx415' ] ||
	fail "unexpected stage-two compatible"
[ -f "$VENDOR_DT_NODE/status" ] ||
	fail "vendor node status is absent; same-address isolation is unproven"
[ "$(dt_string "$VENDOR_DT_NODE/status")" = disabled ] ||
	fail "vendor same-address node is not disabled"

AVAILABLE_AT_1A=0
for node in "$I2C_DT_ROOT"/*@1a; do
	[ -d "$node" ] || continue
	if dt_node_available "$node"; then
		AVAILABLE_AT_1A=$((AVAILABLE_AT_1A + 1))
	fi
done
[ "$AVAILABLE_AT_1A" -eq 1 ] ||
	fail "expected one available i2c3@0x1a DT node, got $AVAILABLE_AT_1A"

[ -d "$I2C_DEVICE" ] || fail "I2C client is absent: $I2C_DEVICE"
[ -L "$I2C_DEVICE/driver" ] || {
	modprobe "$MODULE" 2>/dev/null ||
		modprobe "$BUILTIN_MODULE" 2>/dev/null ||
		fail "failed to load $MODULE or $BUILTIN_MODULE"
}
[ -L "$I2C_DEVICE/driver" ] || fail "I2C client is not bound"
DRIVER=$(basename "$(readlink -f "$I2C_DEVICE/driver")")
[ "$DRIVER" = "$MODULE" ] || fail "I2C client is bound to $DRIVER"

MODULE_INFO_NAME=$MODULE
if ! modinfo "$MODULE_INFO_NAME" >/dev/null 2>&1; then
	MODULE_INFO_NAME=$BUILTIN_MODULE
fi
modinfo "$MODULE_INFO_NAME" >/dev/null 2>&1 ||
	fail "module metadata is missing for $MODULE and $BUILTIN_MODULE"

SUBDEV_NAME=$(find_sensor_subdev) || fail "cannot find the Sensor subdev"
SUBDEV=/dev/$SUBDEV_NAME
SENSOR_NAME=$(cat "/sys/class/video4linux/$SUBDEV_NAME/name")
MEDIA=$(find_sensor_media "$SENSOR_NAME") ||
	fail "cannot find the media graph containing $SENSOR_NAME"

if [ -z "$RAW_NODE" ]; then
	RAW_NODE=$(find_rkcif_raw_node "$MEDIA")
fi
[ -n "$RAW_NODE" ] ||
	fail "cannot resolve stream_cif_mipi_id0; pass /dev/videoX as argument 2"
[ -c "$RAW_NODE" ] || fail "RAW node is not a character device: $RAW_NODE"

printf '%s\n' '=== stage-two environment ==='
uname -a
printf 'cmdline: '
cat /proc/cmdline
printf 'rootfs: '
awk '$2 == "/" { print; found = 1 } END { if (!found) exit 1 }' /proc/mounts
printf 'rootfs-source: '
cat /etc/rootfs-source
printf 'module: '
modinfo -n "$MODULE_INFO_NAME"
MODULE_VERMAGIC=$(modinfo -F vermagic "$MODULE_INFO_NAME" 2>/dev/null || true)
if [ -n "$MODULE_VERMAGIC" ]; then
	printf 'module-vermagic: %s\n' "$MODULE_VERMAGIC"
else
	printf 'module-vermagic: built-in (kernel %s)\n' "$(uname -r)"
fi
printf 'dt-compatible: %s\n' "$(dt_string "$DT_NODE/compatible")"
printf 'vendor-node-status: %s\n' "$(dt_string "$VENDOR_DT_NODE/status")"
printf 'available-i2c3-address-0x1a-nodes: %s\n' "$AVAILABLE_AT_1A"
printf 'i2c-client: %s, driver: %s\n' "$(cat "$I2C_DEVICE/name")" "$DRIVER"
printf 'sensor-subdev: %s (%s)\n' "$SUBDEV" "$SENSOR_NAME"
printf 'sensor-media: %s\n' "$MEDIA"
printf 'rkcif-raw-node: %s (%s)\n' \
	"$RAW_NODE" "$(cat "/sys/class/video4linux/$(basename "$RAW_NODE")/name")"

printf '%s\n' '=== media graph ==='
media-ctl -d "$MEDIA" -p
printf '%s\n' '=== all V4L2 devices ==='
v4l2-ctl --list-devices || true

printf '%s\n' '=== Sensor subdev contract ==='
v4l2-ctl -d "$SUBDEV" --all
v4l2-ctl -d "$SUBDEV" --list-subdev-mbus-codes 0
v4l2-ctl -d "$SUBDEV" \
	--list-subdev-framesizes pad=0,code=0x300e
v4l2-ctl -d "$SUBDEV" \
	--list-subdev-frameintervals pad=0,width=3864,height=2192,code=0x300e
v4l2-ctl -d "$SUBDEV" --get-subdev-fmt 0
v4l2-ctl -d "$SUBDEV" --get-subdev-fps 0
for target in native_size crop_bounds crop_default crop; do
	v4l2-ctl -d "$SUBDEV" \
		--get-subdev-selection "pad=0,target=$target"
done
v4l2-ctl -d "$SUBDEV" --list-ctrls-menus

printf '%s\n' '=== RKCIF RAW format ==='
v4l2-ctl -d "$RAW_NODE" --all
v4l2-ctl -d "$RAW_NODE" \
	--set-fmt-video=width=3840,height=2160,pixelformat=GB10
v4l2-ctl -d "$RAW_NODE" --get-fmt-video

printf '%s\n' '=== short STREAMON/OFF cycles ==='
printf 'cycle,start_uptime_s,end_uptime_s,skipped_frames,captured_frames,result\n'
cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
	start=$(awk '{ print $1 }' /proc/uptime)
	if stream_output=$(timeout 15 v4l2-ctl -d "$RAW_NODE" \
		--verbose --stream-show-delta-now \
		--stream-mmap=4 --stream-skip="$SKIP_FRAMES" \
		--stream-count="$FRAMES" --stream-poll 2>&1); then
		stream_status=0
	else
		stream_status=$?
	fi
	printf '%s\n' "$stream_output"
	if [ "$stream_status" -eq 0 ] &&
		! printf '%s\n' "$stream_output" |
			grep -Eiq 'VIDIOC_[A-Z_]+ returned -[1-9]|Cannot allocate memory|select timeout|streaming error'; then
		result=PASS
	else
		result=FAIL
	fi
	end=$(awk '{ print $1 }' /proc/uptime)
	printf '%d,%s,%s,%d,%d,%s\n' \
		"$cycle" "$start" "$end" "$SKIP_FRAMES" "$FRAMES" "$result"
	[ "$result" = PASS ] || fail "stream cycle $cycle failed"
	cycle=$((cycle + 1))
done

printf '%s\n' '=== runtime PM and relevant kernel log ==='
for attribute in runtime_status runtime_active_time runtime_suspended_time; do
	if [ -r "$I2C_DEVICE/power/$attribute" ]; then
		printf '%s: ' "$attribute"
		cat "$I2C_DEVICE/power/$attribute"
	fi
done
dmesg | grep -Ei \
	'zzh_imx415|reference register|reference signature|csi2|dphy|rkcif|rkisp|mipi|overflow|timeout|iommu|dma'

printf '%s\n' 'STAGE2_RUNTIME_CHECK=PASS_CANDIDATE'
printf '%s\n' \
	'This marker is script-level evidence; archive the complete serial log, deployed hashes, RAW metadata, control readback, and PM evidence before final review. Vendor A/B is supplementary to the primary fixed-mode gate.'
