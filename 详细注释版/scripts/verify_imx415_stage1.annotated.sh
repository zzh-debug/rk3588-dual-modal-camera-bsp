#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# 教学镜像，不是 NFS RootFS 中实际执行的脚本。
# 权威源码：projects/rk3588_camera_bsp/scripts/verify_imx415_stage1.sh
#
# 输入：可选位置参数 $1，表示模块卸载/加载循环次数，默认 20。
# 输出：环境信息、每轮绑定结果、相关 dmesg，以及最终 PASS_CANDIDATE。
# 退出码：0 表示全部运行检查满足；非 0 表示参数、DT、模块或绑定失败。
#
# 脚本只读取 DT/sysfs/dmesg，并通过 modprobe 驱动正常 probe/remove；不会执行
# i2cdetect、不会清空 dmesg，也不会直接向 /dev/i2c-* 写 Sensor 寄存器。

# -e：任一未处理失败立即退出；-u：引用未定义变量立即退出。
set -eu

# MODULE 同时是 .ko 的模块名、/sys/module 子目录和期望 I2C driver.name。
MODULE=zzh_imx415_minimal

# i2c3 adapter 编号为 3，7 位地址 0x1a，所以 sysfs client 名为 3-001a。
I2C_DEVICE=/sys/bus/i2c/devices/3-001a

# 运行 DT 中自研节点的绝对路径；/proc/device-tree 属性是 NUL 结尾二进制数据。
DT_NODE=/proc/device-tree/i2c@feab0000/imx415-minimal@1a

# 原厂同地址节点路径；必须存在但 status=disabled，证明没有地址竞争。
VENDOR_DT_NODE=/proc/device-tree/i2c@feab0000/imx415@1a

# ${1:-20}：调用者提供 $1 时使用它，否则使用默认 20。
CYCLES=${1:-20}

# fail() - 统一打印错误并以失败状态结束
# 输入：$* 是调用位置提供的一段错误说明。
# 输出：stderr 上一行 "FAIL: ..."。
# 返回：不返回调用者，直接 exit 1。
fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

# signature_count() - 统计当前 dmesg 中成功签名日志总数
# 输入：无。
# 输出：stdout 上一个十进制计数，供命令替换保存。
# 返回：即使 grep 没找到而返回 1，也通过 "|| true" 让函数整体成功；grep -c
#       在这种情况下仍输出 0。
# 内存：不建立日志副本，dmesg 输出经 pipe 流式交给 grep。
signature_count()
{
	dmesg | grep -c \
		'reference signature matched (vendor reference, not an official unique Chip ID)' || true
}

# case 拒绝空字符串或任何非数字字符；随后单独拒绝 0。
case "$CYCLES" in
	''|*[!0-9]*) fail "cycle count must be a positive integer" ;;
esac
[ "$CYCLES" -gt 0 ] || fail "cycle count must be greater than zero"

# 模块装卸需要 root；提前失败比执行到中途再出现权限错误更清楚。
[ "$(id -u)" -eq 0 ] || fail "run as root on the development board"

# 如果目录不存在，说明 Linux 没收到实验网络 DTB，最常见原因是误用 nfsboot。
[ -d "$DT_NODE" ] || fail "experimental DT node is absent: $DT_NODE"

# tr 删除 DT 字符串属性末尾的 NUL；COMPATIBLE 保存可直接比较的 shell 字符串。
COMPATIBLE=$(tr -d '\000' <"$DT_NODE/compatible")
[ "$COMPATIBLE" = 'zzh,imx415-minimal' ] ||
	fail "unexpected compatible: $COMPATIBLE"

# 原厂节点必须仍可审计，并明确 disabled；不存在 status 不能算隔离证据。
[ -f "$VENDOR_DT_NODE/status" ] ||
	fail "vendor node status is absent; cannot prove same-address isolation"
VENDOR_STATUS=$(tr -d '\000' <"$VENDOR_DT_NODE/status")
[ "$VENDOR_STATUS" = 'disabled' ] ||
	fail "vendor same-address node is not disabled: $VENDOR_STATUS"

# 打印本次证据所依赖的内核、bootargs、根文件系统、模块和 DT 信息。
printf '%s\n' '=== stage-one environment ==='
uname -a
printf 'cmdline: '
cat /proc/cmdline
printf 'rootfs: '

# awk 查找挂载点恰好为 / 的一行；found 防止“没有输出但管道仍成功”。
mount | awk '$3 == "/" { print; found = 1 } END { if (!found) exit 1 }'
printf 'rootfs-source: '
cat /etc/rootfs-source
printf 'module: '
modinfo -n "$MODULE"
printf 'dt-compatible: %s\n' "$COMPATIBLE"
printf 'vendor-node-status: %s\n' "$VENDOR_STATUS"

# BEFORE 是循环开始前已有的匹配日志数，避免把旧启动日志算进本次 20 次验证。
BEFORE=$(signature_count)

# cycle 是当前轮次，从 1 递增到 CYCLES；shell 只保存小整数，不申请帧缓存。
cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
	# 如果模块已加载，先卸载；这会触发 I2C remove 并验证幂等 power_off。
	if [ -d "/sys/module/$MODULE" ]; then
		modprobe -r "$MODULE" || fail "cycle $cycle: module unload failed"
	fi

	# 加载 .ko、注册 I2C driver，并让 driver core 对 OF client 调用 probe。
	modprobe "$MODULE" || fail "cycle $cycle: module load failed"

	# probe 返回 0 后 sysfs 才会出现 driver 符号链接；只加载模块不等于绑定成功。
	[ -L "$I2C_DEVICE/driver" ] ||
		fail "cycle $cycle: I2C device did not bind after probe"

	# readlink -f 得到实际 driver 目录；basename 只保留驱动名用于精确比较。
	DRIVER=$(basename "$(readlink -f "$I2C_DEVICE/driver")")
	[ "$DRIVER" = "$MODULE" ] ||
		fail "cycle $cycle: unexpected driver $DRIVER"

	# NAME 是 I2C core 给 client 的名称，只用于证据输出，不用它代替 driver 判断。
	NAME=$(cat "$I2C_DEVICE/name")
	printf 'cycle %d/%d: client %s bound to %s\n' \
		"$cycle" "$CYCLES" "$NAME" "$DRIVER"

	cycle=$((cycle + 1))
done

# AFTER 是循环结束后的日志总数；DELTA 必须恰好等于循环数。
AFTER=$(signature_count)
DELTA=$((AFTER - BEFORE))
[ "$DELTA" -eq "$CYCLES" ] ||
	fail "expected $CYCLES new signature matches, observed $DELTA"

# 保留所有相关日志，包含 adapter/address、资源、寄存器值和签名结论。
printf '%s\n' '=== relevant kernel log ==='
dmesg | grep -E \
	'zzh_imx415_minimal|reference register|reference signature|module-local'

# PASS_CANDIDATE 表示脚本条件满足；最终阶段结论仍需人工审核串口和硬件接线。
printf '%s\n' 'STAGE1_RUNTIME_CHECK=PASS_CANDIDATE'
printf '%s\n' \
	'Retain this complete output with the serial boot log; final acceptance still requires connector and waveform review.'
