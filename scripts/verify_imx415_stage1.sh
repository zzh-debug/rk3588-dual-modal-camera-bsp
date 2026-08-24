#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

MODULE=zzh_imx415_minimal
I2C_DEVICE=/sys/bus/i2c/devices/3-001a
DT_NODE=/proc/device-tree/i2c@feab0000/imx415-minimal@1a
VENDOR_DT_NODE=/proc/device-tree/i2c@feab0000/imx415@1a
CYCLES=${1:-20}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

signature_count()
{
	dmesg | grep -c \
		'reference signature matched (vendor reference, not an official unique Chip ID)' || true
}

case "$CYCLES" in
	''|*[!0-9]*) fail "cycle count must be a positive integer" ;;
esac
[ "$CYCLES" -gt 0 ] || fail "cycle count must be greater than zero"
[ "$(id -u)" -eq 0 ] || fail "run as root on the development board"
[ -d "$DT_NODE" ] || fail "experimental DT node is absent: $DT_NODE"

COMPATIBLE=$(tr -d '\000' <"$DT_NODE/compatible")
[ "$COMPATIBLE" = 'zzh,imx415-minimal' ] ||
	fail "unexpected compatible: $COMPATIBLE"

[ -f "$VENDOR_DT_NODE/status" ] ||
	fail "vendor node status is absent; cannot prove same-address isolation"
VENDOR_STATUS=$(tr -d '\000' <"$VENDOR_DT_NODE/status")
[ "$VENDOR_STATUS" = 'disabled' ] ||
	fail "vendor same-address node is not disabled: $VENDOR_STATUS"

printf '%s\n' '=== stage-one environment ==='
uname -a
printf 'cmdline: '
cat /proc/cmdline
printf 'rootfs: '
mount | awk '$3 == "/" { print; found = 1 } END { if (!found) exit 1 }'
printf 'rootfs-source: '
cat /etc/rootfs-source
printf 'module: '
modinfo -n "$MODULE"
printf 'dt-compatible: %s\n' "$COMPATIBLE"
printf 'vendor-node-status: %s\n' "$VENDOR_STATUS"

BEFORE=$(signature_count)
cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
	if [ -d "/sys/module/$MODULE" ]; then
		modprobe -r "$MODULE" || fail "cycle $cycle: module unload failed"
	fi

	modprobe "$MODULE" || fail "cycle $cycle: module load failed"
	[ -L "$I2C_DEVICE/driver" ] ||
		fail "cycle $cycle: I2C device did not bind after probe"

	DRIVER=$(basename "$(readlink -f "$I2C_DEVICE/driver")")
	[ "$DRIVER" = "$MODULE" ] ||
		fail "cycle $cycle: unexpected driver $DRIVER"

	NAME=$(cat "$I2C_DEVICE/name")
	printf 'cycle %d/%d: client %s bound to %s\n' \
		"$cycle" "$CYCLES" "$NAME" "$DRIVER"
	cycle=$((cycle + 1))
done

AFTER=$(signature_count)
DELTA=$((AFTER - BEFORE))
[ "$DELTA" -eq "$CYCLES" ] ||
	fail "expected $CYCLES new signature matches, observed $DELTA"

printf '%s\n' '=== relevant kernel log ==='
dmesg | grep -E \
	'zzh_imx415_minimal|reference register|reference signature|module-local'

printf '%s\n' 'STAGE1_RUNTIME_CHECK=PASS_CANDIDATE'
printf '%s\n' \
	'Retain this complete output with the serial boot log; final acceptance still requires connector and waveform review.'
