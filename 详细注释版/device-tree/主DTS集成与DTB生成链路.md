# 主 DTS 集成与 DTB 生成链路

## 1. DTSI 为什么会被编译发现

实验文件不是放在项目目录里等待 DTC 自动搜索。它实际位于内核的 Rockchip
设备树目录：

```text
/rk3588_dev/kernel/arch/arm64/boot/dts/rockchip/
    rk3588-alientek-imx415-minimal.dtsi
```

正确的 1080x1920 主 DTS 明确包含它：

```dts
/dts-v1/;

#include "rk3588-alientek.dtsi"
#include "rk3588-alientek-cameras.dtsi"
#include "rk3588-linux.dtsi"
#include "rk3588-alientek-imx415-minimal.dtsi"
```

这里的顺序有意义：原厂 camera DTSI 先创建带 `imx415` label 的原厂节点，
实验 DTSI 后执行，才能通过 `&imx415 { status = "disabled"; };` 覆盖它。

内核的 Rockchip DTS Makefile 又明确注册主 DTS 对应的 DTB：

```make
dtb-$(CONFIG_ARCH_ROCKCHIP) += \
	rk3588-alientek-2mipi1080x1920-2hdmi.dtb
```

因此编译链是：

```text
Rockchip Makefile 选择目标 DTB
  -> 对主 DTS 做 C 预处理
  -> 递归展开全部 #include DTSI
  -> 解析 &label 覆盖和 phandle
  -> DTC 生成一个扁平化 DTB
```

`.dtsi` 是“可被其他 DTS include 的设备树源片段”，不是独立启动文件，也不会
单独生成同名 DTB。把板级公共定义和小型实验覆盖拆成 DTSI，是内核设备树的
正常组织方式；关键不是文件是否单独，而是主 DTS 是否明确 include。

## 2. 三个内核侧文件分别是什么

| 名称 | 位置 | 内容和消费者 |
|---|---|---|
| 目标主 DTS | `kernel/.../rk3588-alientek-2mipi1080x1920-2hdmi.dts` | 人可编辑的顶层硬件描述；包含屏幕、板级公共 DTSI、camera DTSI 和实验 DTSI |
| 目标硬件 DTB | `kernel/.../rk3588-alientek-2mipi1080x1920-2hdmi.dtb` | DTC 输出的二进制硬件树；部署脚本称为 `STOCK_DTB`，这里的 stock 表示“未改 NFS bootargs”，不是“未包含实验节点” |
| 网络启动 DTB | `tftpboot/atk_dlrk3588/rk3588-alientek-nfs.dtb` | 复制目标硬件 DTB后，仅用 `fdtput` 把 `/chosen/bootargs` 改成 NFS root/IP 参数 |

两个 DTB 的硬件节点相同。当前大小相差 132 byte，原因是 NFS bootargs 字符串
更长；它们不是两套不同的 Camera DTS。

```text
目标硬件 DTB SHA-256:
a94638d4587d5de2a27646b5b0f12d4c0f24164d1bae306ce346f8d58db9d6b7

网络启动 DTB SHA-256:
c7049e38273e66308d85232fceae7501106557e6fce2e9c257c4ebf6544de2c2
```

## 3. 它们不是 U-Boot 自身的 u-boot.dtb

必须区分三类 DTB：

1. `u-boot/u-boot.dtb`：约 8.8 KiB 的 U-Boot 最小设备树，服务于 U-Boot/SPL
   早期 Driver Model，不包含完整 Camera 图链。
2. eMMC `resource.img` 中的完整内核 DTB：U-Boot proper 启动后按 SARADC 屏幕
   ID 选择，用于 U-Boot 后续网络、显示等能力和默认 Linux 启动。
3. TFTP 下载的 `rk3588-alientek-nfs.dtb`：`run nfsbootfdt` 在 U-Boot 网络已经
   可用之后才把它放到 `fdt_addr_r`，`booti` 最终把它传给 Linux。

所以“网络启动 DTB”是**由 U-Boot 下载、最终交给 Linux 的 DTB**，不是用于
重新初始化 U-Boot Driver Model 的 `u-boot.dtb`。下载覆盖 `fdt_addr_r` 后，
U-Boot 不会销毁并按新树重建已经 probe 的网络设备，但仍可能在 `booti` 前对
Linux DTB 做 `/chosen`、memory、MAC、reserved-memory 等 fixup。

## 4. run nfsbootfdt 的准确数据流

```text
U-Boot 已经基于 eMMC resource.img 的 DTB 启动并获得网络能力
  -> tftp ${kernel_addr_r} atk_dlrk3588/Image
  -> tftp ${fdt_addr_r} atk_dlrk3588/rk3588-alientek-nfs.dtb
  -> booti ${kernel_addr_r} - ${fdt_addr_r}
  -> Linux 解压/启动 Image
  -> Linux 解析 fdt_addr_r 指向的新实验 DTB
  -> OF 为 okay 的 imx415-minimal@1a 创建设备
  -> disabled 的原厂 imx415@1a 不创建设备
```

这正是修改 DTS 后必须用 `run nfsbootfdt` 的原因；旧的 `run nfsboot` 只下载
Image，Linux 可能继续收到 eMMC `resource.img` 中没有实验节点的旧 DTB。

## 5. Image 是否变化

本阶段最小驱动编译为外置 `.ko`，没有链接进 Image；实验 DTSI 只影响 DTB，
也不会改变 Image。因此 IMX415 阶段一新增内容本身没有向 Image 加入代码。

当前事实是：

```text
kernel/arch/arm64/boot/Image
tftpboot/atk_dlrk3588/Image
```

两者大小均为 36,207,104 byte，SHA-256 都是：

```text
aeca702de12c2e0d6161cd8c097ac1a940a409c91a8e6176e5bd57891f7369ba
```

它不是“厂商出厂分区里未经任何配置变化的原始 Image”，而是当前 SDK 已构建、
支持现有 NFS root 启动环境的内核 Image。准确说法是：**阶段一 Camera 最小
驱动没有改变这份 Image；部署脚本只是把当前 Image 原样复制到 TFTP 目录。**
