#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

usage()
{
	printf 'usage: %s [--check-only] <SDK_ROOT>\n' "$0" >&2
	exit 2
}

CHECK_ONLY=0
if [ "${1:-}" = "--check-only" ]; then
	CHECK_ONLY=1
	shift
fi
[ "$#" -eq 1 ] || usage

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SDK_ROOT=$(realpath "$1")
EXPECTED_PROJECT=$(realpath "$SDK_ROOT/projects/rk3588_camera_bsp")

[ "$PROJECT_ROOT" = "$EXPECTED_PROJECT" ] || {
	printf 'repository must be located at %s\n' \
		"$SDK_ROOT/projects/rk3588_camera_bsp" >&2
	exit 1
}
[ -d "$SDK_ROOT/kernel/.git" ] || {
	printf 'kernel Git repository not found under %s\n' "$SDK_ROOT" >&2
	exit 1
}
[ -d "$SDK_ROOT/u-boot/.git" ] || {
	printf 'U-Boot Git repository not found under %s\n' "$SDK_ROOT" >&2
	exit 1
}

KERNEL_BASE=cfc6be6c04e6c8e66f3fc9e33f8f27b10e3c00e8
UBOOT_BASE=23c0020b9bf075194ea1a9ed31b6cf64632f8ff9

[ "$(git -C "$SDK_ROOT/kernel" rev-parse HEAD)" = "$KERNEL_BASE" ] || {
	printf 'kernel HEAD does not match %s\n' "$KERNEL_BASE" >&2
	exit 1
}
[ "$(git -C "$SDK_ROOT/u-boot" rev-parse HEAD)" = "$UBOOT_BASE" ] || {
	printf 'U-Boot HEAD does not match %s\n' "$UBOOT_BASE" >&2
	exit 1
}

patch_state()
{
	repository=$1
	patch_file=$2

	if git -C "$repository" apply --check "$patch_file" 2>/dev/null; then
		printf 'pending'
	elif git -C "$repository" apply --reverse --check \
		"$patch_file" 2>/dev/null; then
		printf 'applied'
	else
		printf 'patch is partially applied or conflicts: %s\n' \
			"$patch_file" >&2
		exit 1
	fi
}

check_overlay()
{
	relative=$1
	source_file=$SCRIPT_DIR/overlay/$relative
	target_file=$SDK_ROOT/$relative

	[ -f "$source_file" ] || {
		printf 'overlay source missing: %s\n' "$source_file" >&2
		exit 1
	}
	if [ -e "$target_file" ] && ! cmp -s "$source_file" "$target_file"; then
		printf 'overlay target differs: %s\n' "$target_file" >&2
		exit 1
	fi
}

install_overlay()
{
	relative=$1
	mode=$2
	source_file=$SCRIPT_DIR/overlay/$relative
	target_file=$SDK_ROOT/$relative

	if [ ! -e "$target_file" ]; then
		mkdir -p "$(dirname -- "$target_file")"
		install -m "$mode" "$source_file" "$target_file"
		printf 'installed %s\n' "$relative"
	fi
}

KERNEL_PATCH=$SCRIPT_DIR/patches/kernel-tracked.patch
UBOOT_PATCH=$SCRIPT_DIR/patches/u-boot-network-boot.patch
KERNEL_STATE=$(patch_state "$SDK_ROOT/kernel" "$KERNEL_PATCH")
UBOOT_STATE=$(patch_state "$SDK_ROOT/u-boot" "$UBOOT_PATCH")

OVERLAYS='kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-imx415-minimal.dtsi
kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-imx415-stage2.dtsi
kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-mlx90640-stage5.dtsi
kernel/drivers/media/i2c/zzh_imx415_builtin.c
kernel/drivers/media/i2c/zzh_mlx90640_builtin.c
projects/scripts/deploy_net_boot.sh'

for relative in $OVERLAYS; do
	check_overlay "$relative"
done

printf 'kernel patch: %s\n' "$KERNEL_STATE"
printf 'U-Boot patch: %s\n' "$UBOOT_STATE"

if [ "$CHECK_ONLY" -eq 1 ]; then
	printf 'SDK integration check passed\n'
	exit 0
fi

if [ "$KERNEL_STATE" = pending ]; then
	git -C "$SDK_ROOT/kernel" apply "$KERNEL_PATCH"
	printf 'applied kernel patch\n'
fi
if [ "$UBOOT_STATE" = pending ]; then
	git -C "$SDK_ROOT/u-boot" apply "$UBOOT_PATCH"
	printf 'applied U-Boot patch\n'
fi

for relative in $OVERLAYS; do
	case "$relative" in
		*.sh) mode=0755 ;;
		*) mode=0644 ;;
	esac
	install_overlay "$relative" "$mode"
done

printf 'SDK integration applied successfully\n'
