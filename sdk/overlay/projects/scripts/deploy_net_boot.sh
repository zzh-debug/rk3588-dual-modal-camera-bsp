#!/bin/bash
# 部署 ATK-DLRK3588 的 TFTP + NFS 网络启动载荷。
#
# 用法：
#   ./deploy_net_boot.sh              # 部署 Image + 原生 dtb + NFS 版 dtb，并打印 U-Boot 命令
#   ./deploy_net_boot.sh -m           # 顺带把内核模块同步进 NFS RootFS
#   ./deploy_net_boot.sh -i eth1      # 板子实际用的是第二个网口时
#   ./deploy_net_boot.sh -d rk3588-alientek-2mipi800x1280-2hdmi   # 换一块屏的 DTB
#
# 每次 ./build.sh kernel 之后跑一遍即可，不需要重新烧写。
#
# 注意：板子有三块可选屏，U-Boot 会读 SARADC 通道 7 从 resource.img 里自动挑 DTB
# （arch/arm/mach-rockchip/resource_hwid.c: hwid_adc_find_dtb）。当前固定板驱动
# 开发的标准启动方式是 TFTP Image + 固定的 1080x1920 NFS DTB，然后执行
# run nfsbootfdt，这样 Linux 每次都使用最新 DTS。只 TFTP kernel、保留
# resource.img DTB 的 nfsboot 仍作为自动选屏/回退路径，不是阶段二默认路径。

set -euo pipefail

SDK_ROOT=${SDK_ROOT:-/rk3588_dev}
BOARD=atk_dlrk3588

# ---- 网络参数（与 /etc/exports、宿主机 IP 保持一致）----
BOARD_IP=192.168.5.20
SERVER_IP=192.168.5.11
NETMASK=255.255.255.0
GATEWAY=                                    # 直连网段，无网关
BOARD_HOSTNAME=atk-dlrk3588
IFACE=eth0                                  # 板子上带网线的那个口，-i 可覆盖
NFS_EXPORT=/home/zzh/workspace/myproject/nfs_rootfs/${BOARD}   # NFS 服务端看到的路径
DTS_NAME=rk3588-alientek-2mipi1080x1920-2hdmi                   # 实测 SARADC ch7=1395 -> 这块屏；-d 可覆盖

SYNC_MODULES=0
while getopts "mi:d:h" opt; do
	case "$opt" in
		m) SYNC_MODULES=1 ;;
		i) IFACE=$OPTARG ;;
		d) DTS_NAME=$OPTARG ;;
		h) sed -n '2,16p' "$0"; exit 0 ;;
		*) exit 2 ;;
	esac
done

KERNEL_DIR=$SDK_ROOT/kernel
DTS_DIR=$KERNEL_DIR/arch/arm64/boot/dts/rockchip
STOCK_DTB_NAME=$DTS_NAME.dtb
IMAGE=$KERNEL_DIR/arch/arm64/boot/Image
STOCK_DTB=$DTS_DIR/$STOCK_DTB_NAME
TFTP_DIR=$SDK_ROOT/tftpboot/$BOARD
NFS_DIR=$SDK_ROOT/nfs_rootfs/$BOARD
NFS_DTB=$TFTP_DIR/rk3588-alientek-nfs.dtb

for f in "$IMAGE" "$STOCK_DTB"; do
	[ -f "$f" ] || { echo "缺少构建产物：$f，先跑 ./build.sh kernel" >&2; exit 1; }
done
[ -d "$TFTP_DIR" ] || { echo "缺少目录：$TFTP_DIR" >&2; exit 1; }

echo "== 部署 kernel 与 dtb 到 $TFTP_DIR"
install -m 0644 "$IMAGE" "$TFTP_DIR/Image"
install -m 0644 "$STOCK_DTB" "$TFTP_DIR/$STOCK_DTB_NAME"

# NFS 版 dtb：继承厂商 bootargs，只替换 root= 并补 nfsroot=/ip=。
# 这版 Rockchip U-Boot 是 DTB 的 /chosen/bootargs 覆盖 env（board.c: bootargs_add_dtb_dtbo
# + nvedit.c: env_update_filter），且 CONFIG_ENV_IS_NOWHERE 让 saveenv 无效，
# 所以 NFS 的 cmdline 只能写进这个 dtb。
BASE_ARGS=$(fdtget -t s "$STOCK_DTB" /chosen bootargs)
CLEAN_ARGS=$(echo "$BASE_ARGS" | tr ' ' '\n' | grep -vE '^(root=|rw$)' | tr '\n' ' ')
NFS_ARGS="${CLEAN_ARGS}root=/dev/nfs rw"
NFS_ARGS="$NFS_ARGS nfsroot=${SERVER_IP}:${NFS_EXPORT},v3,tcp"
NFS_ARGS="$NFS_ARGS ip=${BOARD_IP}:${SERVER_IP}:${GATEWAY}:${NETMASK}:${BOARD_HOSTNAME}:${IFACE}:off"

echo "== 生成 $(basename "$NFS_DTB")（网口 $IFACE）"
install -m 0644 "$STOCK_DTB" "$NFS_DTB"
fdtput -t s "$NFS_DTB" /chosen bootargs "$NFS_ARGS"

if [ "$SYNC_MODULES" = 1 ]; then
	REL=$(cat "$KERNEL_DIR/include/config/kernel.release")
	SRC=$SDK_ROOT/buildroot/output/alientek_rk3588/target/lib/modules/$REL
	[ -d "$SRC" ] || { echo "找不到模块目录：$SRC" >&2; exit 1; }
	echo "== 同步 lib/modules/$REL 到 NFS RootFS"
	sudo rsync -a --delete "$SRC/" "$NFS_DIR/lib/modules/$REL/"
	sudo chown -R 0:0 "$NFS_DIR/lib/modules/$REL"
fi

echo
echo "== 部署完成"
ls -l "$TFTP_DIR"
echo
cat <<EOF

cmdline:
$NFS_ARGS

============================================================
方式 A（回退）：只 TFTP kernel，保留 U-Boot 的 SARADC 自动选屏
============================================================
setenv nfsargs '$NFS_ARGS'
tftp \${kernel_addr_r} ${BOARD}/Image
fdt addr \${fdt_addr_r}
fdt resize 1024
fdt set /chosen bootargs "\${nfsargs}"
booti \${kernel_addr_r} - \${fdt_addr_r}

刷了带默认环境的 uboot.img 之后等价于一条：  run nfsboot

fdt_addr_r 上放的是 U-Boot 启动时从 boot 分区 resource.img 里按 SARADC 通道 7
自动挑出来的那份 DTB（init_kernel_dtb），只改它的 bootargs 就不会挑错屏。

============================================================
方式 B（当前标准）：TFTP 固定的 NFS 版 DTB，执行 run nfsbootfdt
============================================================
tftp \${kernel_addr_r} ${BOARD}/Image
tftp \${fdt_addr_r} ${BOARD}/$(basename "$NFS_DTB")
booti \${kernel_addr_r} - \${fdt_addr_r}

当前这份是从 $STOCK_DTB_NAME 生成的；
换屏用 -d 指定别的 DTS 名重新生成。当前已刷 U-Boot 中已有的命令等价于：  run nfsbootfdt
EOF
