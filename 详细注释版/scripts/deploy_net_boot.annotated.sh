#!/bin/bash
# 教学镜像，不参与日常部署，也没有设置可执行权限。
# 权威源码：projects/scripts/deploy_net_boot.sh
#
# 输入选项：
#   -m：同步 Buildroot 生成的内核模块树；
#   -i <iface>：覆盖 Linux NFS 启动网卡名，默认 eth0；
#   -d <dts-name>：覆盖目标 DTS 基础名，默认固定 1080x1920；
#   -h：打印帮助。
#
# 输出文件：
#   tftpboot/atk_dlrk3588/Image
#   tftpboot/atk_dlrk3588/<DTS_NAME>.dtb
#   tftpboot/atk_dlrk3588/rk3588-alientek-nfs.dtb
#
# 重要：这个脚本不编译任何东西，只部署已经存在的构建产物并修改网络 DTB 的
# /chosen/bootargs。缺 Image/目标 DTB 时会要求先运行 ./build.sh kernel。

# -e：命令失败立即退出；-u：未定义变量报错；pipefail：管道任一环节失败报错。
set -euo pipefail

# SDK_ROOT 允许测试时通过环境变量覆盖；正常固定为容器挂载根目录。
SDK_ROOT=${SDK_ROOT:-/rk3588_dev}

# BOARD 同时决定 TFTP 子目录和 NFS RootFS 子目录名。
BOARD=atk_dlrk3588

# U-Boot TFTP 阶段和 Linux NFS 阶段使用同一静态板端地址，但属于两层配置。
BOARD_IP=192.168.5.20
SERVER_IP=192.168.5.11
NETMASK=255.255.255.0

# 直连同网段没有网关，保留空字段以生成 Linux ip= 七字段格式。
GATEWAY=
BOARD_HOSTNAME=atk-dlrk3588

# IFACE 是 Linux NFS 根使用的网卡名，不自动改变 U-Boot 当前网络设备。
IFACE=eth0

# NFS_EXPORT 必须写宿主机 nfs-kernel-server 导出的真实路径，不是容器路径。
NFS_EXPORT=/home/zzh/workspace/myproject/nfs_rootfs/${BOARD}

# DTS_NAME 不带 .dts/.dtb 后缀；当前实物屏固定选择 1080x1920 版本。
DTS_NAME=rk3588-alientek-2mipi1080x1920-2hdmi

# 0 表示只部署 Image/DTB；-m 后改为 1，再同步模块。
SYNC_MODULES=0

# getopts 把参数值放到 OPTARG；错误或未知选项退出 2。
while getopts "mi:d:h" opt; do
	case "$opt" in
		m) SYNC_MODULES=1 ;;
		i) IFACE=$OPTARG ;;
		d) DTS_NAME=$OPTARG ;;
		h) sed -n '2,16p' "$0"; exit 0 ;;
		*) exit 2 ;;
	esac
done

# 以下变量只拼接路径，不申请或复制文件内容到 shell 内存。
KERNEL_DIR=$SDK_ROOT/kernel
DTS_DIR=$KERNEL_DIR/arch/arm64/boot/dts/rockchip
STOCK_DTB_NAME=$DTS_NAME.dtb
IMAGE=$KERNEL_DIR/arch/arm64/boot/Image

# STOCK_DTB 是 DTC 刚生成、尚未改 NFS bootargs 的目标硬件 DTB。
# 名字 stock 不是“原厂无实验节点”：它已经包含主 DTS include 的实验 DTSI。
STOCK_DTB=$DTS_DIR/$STOCK_DTB_NAME

TFTP_DIR=$SDK_ROOT/tftpboot/$BOARD
NFS_DIR=$SDK_ROOT/nfs_rootfs/$BOARD

# NFS_DTB 是从 STOCK_DTB 复制后，只改 /chosen/bootargs 得到的 Linux DTB。
NFS_DTB=$TFTP_DIR/rk3588-alientek-nfs.dtb

# 在写 TFTP 目录前先完整检查两个构建输入，避免留下半套新旧混合产物。
for f in "$IMAGE" "$STOCK_DTB"; do
	[ -f "$f" ] || {
		echo "缺少构建产物：$f，先跑 ./build.sh kernel" >&2
		exit 1
	}
done
[ -d "$TFTP_DIR" ] || {
	echo "缺少目录：$TFTP_DIR" >&2
	exit 1
}

echo "== 部署 kernel 与 dtb 到 $TFTP_DIR"

# install 在这里等价于带目标权限的受控复制；不会改变源 Image/DTB。
install -m 0644 "$IMAGE" "$TFTP_DIR/Image"
install -m 0644 "$STOCK_DTB" "$TFTP_DIR/$STOCK_DTB_NAME"

# BASE_ARGS 读取目标 DTB 现有 /chosen/bootargs 字符串。
BASE_ARGS=$(fdtget -t s "$STOCK_DTB" /chosen bootargs)

# CLEAN_ARGS 逐 token 删除旧 root= 和独立 rw，保留 console/earlycon 等参数。
CLEAN_ARGS=$(echo "$BASE_ARGS" | tr ' ' '\n' |
	grep -vE '^(root=|rw$)' | tr '\n' ' ')

# NFS_ARGS 在保留参数后追加 NFS root、服务端导出和静态 IP 配置。
NFS_ARGS="${CLEAN_ARGS}root=/dev/nfs rw"
NFS_ARGS="$NFS_ARGS nfsroot=${SERVER_IP}:${NFS_EXPORT},v3,tcp"
NFS_ARGS="$NFS_ARGS ip=${BOARD_IP}:${SERVER_IP}:${GATEWAY}:${NETMASK}:${BOARD_HOSTNAME}:${IFACE}:off"

echo "== 生成 $(basename "$NFS_DTB")（网口 $IFACE）"

# 先得到硬件节点完全相同的副本，再只修改 chosen/bootargs 字符串属性。
install -m 0644 "$STOCK_DTB" "$NFS_DTB"
fdtput -t s "$NFS_DTB" /chosen bootargs "$NFS_ARGS"

if [ "$SYNC_MODULES" = 1 ]; then
	# REL 例如 5.10.209，必须与 uname -r 和模块 vermagic 对应。
	REL=$(cat "$KERNEL_DIR/include/config/kernel.release")

	# SRC 是 Buildroot 构建模块 staging，不是 target/ 整个 RootFS 导出源。
	SRC=$SDK_ROOT/buildroot/output/alientek_rk3588/target/lib/modules/$REL
	[ -d "$SRC" ] || {
		echo "找不到模块目录：$SRC" >&2
		exit 1
	}

	echo "== 同步 lib/modules/$REL 到 NFS RootFS"

	# --delete 会删除目标模块树中源目录没有的文件；因此外置实验模块必须在
	# 这一步之后单独 install 并 depmod，不能先放进去再运行 -m。
	sudo rsync -a --delete "$SRC/" "$NFS_DIR/lib/modules/$REL/"
	sudo chown -R 0:0 "$NFS_DIR/lib/modules/$REL"
fi

echo
echo "== 部署完成"
ls -l "$TFTP_DIR"
echo

# 权威脚本最后还会打印 nfsboot/nfsbootfdt 手工命令。本教学镜像省略重复的
# here-document；当前 Camera/DTS 开发固定执行 run nfsbootfdt，确保 Linux 收到
# 上面生成的 rk3588-alientek-nfs.dtb。
printf 'Linux NFS cmdline:\n%s\n' "$NFS_ARGS"
