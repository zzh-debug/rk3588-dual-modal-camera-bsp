#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

LED_PATH=${LED_PATH:-/sys/class/leds/zzh:white:fill}
COMMAND=${1:-status}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

show_status()
{
	printf 'LED_PATH=%s\n' "$LED_PATH"
	printf 'BRIGHTNESS=%s\n' "$(cat "$LED_PATH/brightness")"
	printf 'MAX_BRIGHTNESS=%s\n' "$(cat "$LED_PATH/max_brightness")"
	printf 'TRIGGER=%s\n' "$(cat "$LED_PATH/trigger")"
	printf 'DEVICE=%s\n' "$(readlink -f "$LED_PATH/device")"
}

[ -d "$LED_PATH" ] || fail "missing LED class device: $LED_PATH"
[ -r "$LED_PATH/brightness" ] || fail 'brightness is not readable'
[ -r "$LED_PATH/max_brightness" ] || fail 'max_brightness is not readable'

case "$COMMAND" in
	status)
		show_status
		;;
	assert-off)
		[ "$(cat "$LED_PATH/brightness")" -eq 0 ] ||
			fail 'LED is not at brightness 0'
		show_status
		printf 'LED_DEFAULT_OFF=PASS\n'
		;;
	off)
		[ "$(id -u)" -eq 0 ] || fail 'off requires root'
		printf 0 >"$LED_PATH/brightness"
		show_status
		printf 'LED_OFF=PASS\n'
		;;
	set)
		[ "$(id -u)" -eq 0 ] || fail 'set requires root'
		value=${2:-}
		case "$value" in
			''|*[!0-9]*) fail 'brightness must be an integer' ;;
		esac
		max=$(cat "$LED_PATH/max_brightness")
		[ "$value" -le "$max" ] ||
			fail "brightness $value exceeds max $max"
		printf '%s' "$value" >"$LED_PATH/brightness"
		show_status
		printf 'LED_SET=PASS\n'
		;;
	*)
		printf 'usage: %s {status|assert-off|off|set BRIGHTNESS}\n' "$0" >&2
		exit 2
		;;
esac
