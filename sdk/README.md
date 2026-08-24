# SDK integration package

This directory makes the standalone project repository reproducible without
copying the vendor SDK.

## Contents

```text
patches/kernel-tracked.patch
  Tracked kernel Kconfig/Makefile/defconfig/main-DTS changes.

patches/u-boot-network-boot.patch
  Lab-specific U-Boot TFTP/NFS defaults and TFTP block-size optimization.

overlay/kernel/
  New DTSI files, built-in wrappers and the RKCIF tracepoint header that are
  untracked in the vendor tree.

overlay/projects/scripts/deploy_net_boot.sh
  Shared Image/DTB/NFS deployment helper.

apply_sdk_changes.sh
  Baseline validation, dry-run check, idempotent patch application and overlay
  installation.
```

## Required repository location

Clone this repository at:

```text
<SDK_ROOT>/projects/rk3588_camera_bsp
```

The kernel built-in wrappers deliberately include the project-owned source
from that stable path so external-module and built-in builds share one source
of truth.

## Check and apply

```sh
cd <SDK_ROOT>/projects/rk3588_camera_bsp
./sdk/apply_sdk_changes.sh --check-only <SDK_ROOT>
./sdk/apply_sdk_changes.sh <SDK_ROOT>
```

The script accepts both a fresh matching SDK and an already-integrated work
tree. It refuses base-commit mismatches, partially applied patches, or overlay
files that differ from the packaged copies.

Review the U-Boot patch before applying it outside the original lab because it
contains fixed IP addresses and an NFS export path.

## Build

Inside the RK3588 Dev Container, as the ordinary `vscode` user:

```sh
cd /rk3588_dev
./build.sh kernel
./projects/scripts/deploy_net_boot.sh \
  -d rk3588-alientek-2mipi1080x1920-2hdmi
```

Do not run SDK builds as root and do not build from `/mnt/hgfs`.
