# ATK-DLRK3588 SDK Phase 0 首次完整构建报告

生成日期：2026-07-30（Asia/Shanghai）

## 1. 结论

本次在 `/rk3588_dev` 中使用普通用户 `vscode` 完成了 ATK-DLRK3588 Linux 5.10 SDK 的默认完整构建。构建退出码为 0，loader、U-Boot、Linux kernel、Buildroot RootFS、recovery、额外分区镜像、固件集合和 `update.img` 均生成成功。

- 未使用 root 或 `sudo` 执行 SDK 编译。
- 未运行 clean、cleanall、repo init、烧写工具或 USB 设备操作。
- 未修改厂商源码、SDK defconfig 或板级配置。
- `BR2_PACKAGE_MEDIAMTX=y` 保持启用，MediaMTX 1.7.0 构建成功。
- 完整构建日志 SHA-256：`942b6e0712d6eddceee98c07f78d5cd4cf5f6d1963af157d4079a187b7cd0478`

## 2. 环境信息

| 项目 | 实际值 |
|---|---|
| SDK 路径 | `/rk3588_dev` |
| 当前用户 | `vscode`，UID/GID `1000:1000` |
| HOME | `/home/vscode` |
| 容器 OS | Ubuntu 20.04.6 LTS (Focal) |
| 宿主内核 | Linux `6.17.0-20-generic` x86_64 |
| CPU | 8 个逻辑核；SDK 内核/Buildroot 实际使用 `-j9` |
| 构建前内存 | 7.7 GiB RAM，4.0 GiB swap |
| 构建前磁盘 | 344 GiB，总可用约 212 GiB |
| 构建后磁盘 | 344 GiB，总可用约 149 GiB |
| Git | 2.49.0 |
| repo | SDK 内置 `.repo/repo/repo` 2.27；系统 PATH 中无 `repo` |
| GCC/G++（宿主） | Ubuntu 9.4.0 |
| GNU Make | 4.2.1 |
| Python | 3.8.10 |
| CMake | 3.16.3 |
| DTC | 1.5.0 |

`/rk3588_dev` 是唯一包含 `app/`、`buildroot/`、`device/`、`external/`、`kernel/`、`prebuilts/`、`rkbin/`、`tools/`、`u-boot/` 和 `build.sh` 的完整候选 SDK。SDK 根目录、`build.sh`、`kernel/`、`u-boot/`、`buildroot/` 均由 `vscode:vscode`（1000:1000）拥有。

`build.sh` 是软链接：

```text
/rk3588_dev/build.sh
  -> device/rockchip/common/scripts/build.sh
```

Manifest 为 `atk-rk3588_linux_release_v2.0_20260703.xml`，SDK 标记为 `linux-5.10-gen-rkr8`。冻结后的 resolved manifest 保存于 `/tmp/rk3588_manifest.xml`，SHA-256 为 `75cdfa07580698c617c5e0ac5bdec276002633b04126f989da88093f40264e04`；`/tmp` 文件不是长期归档，后续需要时可再次用 SDK 内置 repo 生成。

## 3. SDK 组件版本

| 组件 | 版本/标签 | Git commit |
|---|---|---|
| Linux kernel | 5.10.209；标签 `atk-dlrk3588-release-v2.0` | `cfc6be6c04e6c8e66f3fc9e33f8f27b10e3c00e8` |
| U-Boot | Rockchip U-Boot 2017.09；标签 `atk-dlrk3588-release-v2.0` | `23c0020b9bf075194ea1a9ed31b6cf64632f8ff9` |
| Buildroot | 2021.11，运行时版本 `-gc52b62b8` | `c52b62b898eb86ed18fb2236d602a26c713601f4` |
| rkbin | 标签 `linux5.10-atk-r8` | `b78c2e8e237330c042c1fea31c197dc32c4fb086` |

顶层 kernel/U-Boot 使用的交叉编译器为：

```text
/rk3588_dev/prebuilts/gcc/linux-x86/aarch64/
  gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/
  bin/aarch64-none-linux-gnu-
```

版本：GNU AArch64 Toolchain 10.3.1 20210621。Buildroot 另外自行构建并使用 GCC 10.4.0、glibc 2.38 的内部目标工具链；不要把它与顶层预置 GCC 10.3 混淆。

构建前 repo 状态中已有、且本次保留的非提交项：

```text
kernel/:  ?? make.sh
u-boot/:  ?? bl31_0x00040000.bin.gz
          ?? examples/standalone/rkspi
          ?? tee.bin.gz
```

其中 `kernel/make.sh` 在构建前已经存在；U-Boot 的三个文件也在构建前已被记录。Buildroot 与 rkbin 在构建后保持 clean。未清理或回退上述文件。

## 4. 板级配置

构建前已经选择正确配置，无需执行交互式 `./build.sh lunch`：

```text
output/defconfig
  -> device/rockchip/.chips/rk3588/
     01_atk_dlrk3588_auto2mipi_2hdmi_defconfig
```

`output/.config` 中的 `RK_DEFCONFIG` 也为 `01_atk_dlrk3588_auto2mipi_2hdmi_defconfig`，实际关联如下：

| 项目 | 配置/路径 |
|---|---|
| 目标板 | `ATK-DLRK3588` |
| U-Boot defconfig | `u-boot/configs/alientek_rk3588_defconfig` |
| Kernel defconfig | `kernel/arch/arm64/configs/alientek_rk3588_defconfig`，并合并 `rk3588_linux.config` |
| 主 DTS | `kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-2mipi720x1280-2hdmi.dts` |
| 主 DTB | 同目录的 `rk3588-alientek-2mipi720x1280-2hdmi.dtb` |
| 同时构建的 DTB | 720x1280、800x1280、1080x1920 三个 MIPI 版本 |
| Buildroot defconfig | `buildroot/configs/alientek_rk3588_defconfig` |
| RootFS | Buildroot，ext4（实际文件 `rootfs.ext2`，`rootfs.ext4` 为其软链接） |
| Recovery | `alientek_rk3588_recovery`，ramdisk 为 `cpio.gz` |
| Boot FIT ITS | `device/rockchip/.chips/rk3588/boot.its` |
| Recovery FIT ITS | `device/rockchip/.chips/rk3588/boot4recovery.its` |
| 分区表 | `device/rockchip/.chips/rk3588/parameter.txt` |

## 5. 完整构建记录

执行方式（普通用户，无 sudo，无 clean）：

```bash
set -o pipefail
./build.sh 2>&1 | tee /rk3588_dev/logs/sdk-build/rk3588_sdk_full_build_20260729_230258.log
```

| 项目 | 结果 |
|---|---|
| 开始时间 | `2026-07-29T23:02:58+08:00` |
| 结束时间 | `2026-07-30T02:46:35+08:00` |
| 总耗时 | 13,417 秒（3 小时 43 分 37 秒） |
| 退出码 | `0` |
| 完整日志 | `/rk3588_dev/logs/sdk-build/rk3588_sdk_full_build_20260729_230258.log` |
| 元数据 | `/rk3588_dev/logs/sdk-build/rk3588_sdk_full_build_20260729_230258.meta` |
| SDK 分阶段日志 | `/rk3588_dev/output/sessions/2026-07-29_23-02-58/` |

默认无参数命令和 `./build.sh all` 等价：顶层脚本设置 `RK_DEFAULT_TARGET=all`，帮助信息也明确 `Default option is 'all'`。完整流程为 misc -> loader/U-Boot -> kernel/boot FIT -> Buildroot RootFS -> recovery -> extra partitions -> firmware 检查 -> update.img -> Linux headers。

### 构建警告与问题

- 构建最终退出码为 0，所有关键阶段均打印 succeeded，不存在导致退出的 error/failed。
- 多 DTB 资源脚本打印过 `Not Found io-domains in ...rk3588-alientek-2mipi720x1280-2hdmi.dts`。这是非致命探测信息；三个 DTB、`resource.img` 和 `boot.img` 随后均成功生成。
- 日志里的 `ignore-failed-read` 是 Linux headers 打包命令的 tar 选项，不是一次失败。
- 日志中 `error_report-traces.o`、`xfs_error.o`、`scsi_error.o` 等是源码目标文件名，不是错误。
- MediaMTX 1.7.0 在 01:01:06 至 01:01:28 正常完成 Extract/Patch/Configure/Build/Install，无下载重试。
- 首次构建过程中 swap 曾被使用，构建后约占用 810 MiB，但没有 OOM 或进程被杀记录。

## 6. 镜像产物与校验值

`rockdev -> output/firmware`。以下 SHA-256 均针对软链接解析后的真实文件内容：

| 产物 | 大小（bytes） | 类型 | SHA-256 |
|---|---:|---|---|
| `MiniLoaderAll.bin` | 483,776 | Rockchip loader（二进制数据） | `e0ed3f7c76716127ec3fde97d88afbe875c177d40a4350e050f1143f569d09a6` |
| `uboot.img` | 4,194,304 | FIT/DTB 容器 | `8fb0a2075e50abb9688130ed4671a1297a6c2219cf5bd2ae02d43f5bc26b7a93` |
| `boot.img` | 38,627,328 | FIT/DTB 容器 | `9402b5e520e8d234652026ff714ac7066543bccb83ae9ba47160ee10c9f74469` |
| `rootfs.img` | 1,491,075,072 | ext4，卷标 rootfs | `ab08d9528d8709945469a0304ae912bff8ecdd009b0ef1253bd8dafd0e1a8bd0` |
| `misc.img` | 49,152 | 空白 misc 数据 | `2aae7dc846aaf25f1cadf55f1666862046c6db9d65d84bdc07fa039dac405606` |
| `recovery.img` | 49,422,848 | Recovery FIT | `82e53e2f370bcc0aed8b51a24bc313617944f3896524724fe6930a21d6439087` |
| `oem.img` | 18,804,736 | ext4，卷标 oem | `fc03e084eb61461f6b4462c8624cbb91a2b3a40975ec2934158beb33136b720b` |
| `userdata.img` | 4,513,792 | ext 文件系统，卷标 userdata | `ae860e84d343b438ab87b9654fed0fd43e03705b6e3e6103b27a4fe02e15461d` |
| `parameter.txt` | 539 | ASCII GPT 分区描述 | `0a87b46be31e98e0fa6c2775bc4996b9fa5dc38fe2ad83b92a2d197eda6972a7` |
| `update.img` | 1,607,664,202 | Rockchip update 容器 | `f96ba33de5f7e7fb7b23677d1d422111676df5bf967c08cb4cc3146a4684ac49` |

### 软链接与真实路径

| rockdev 名称 | 真实路径 |
|---|---|
| `MiniLoaderAll.bin` | `/rk3588_dev/u-boot/rk3588_spl_loader_v1.17.113.bin` |
| `uboot.img` | `/rk3588_dev/u-boot/uboot.img` |
| `boot.img` | `/rk3588_dev/kernel/boot.img` |
| `rootfs.img` | `/rk3588_dev/buildroot/output/alientek_rk3588/images/rootfs.ext2` |
| `misc.img` | `/rk3588_dev/output/misc.img` |
| `recovery.img` | `/rk3588_dev/output/recovery/ramboot.img` |
| `oem.img` | `/rk3588_dev/output/extra-parts/oem.img` |
| `userdata.img` | `/rk3588_dev/output/extra-parts/userdata.img` |
| `parameter.txt` | `/rk3588_dev/device/rockchip/.chips/rk3588/parameter.txt` |
| `update.img` | `/rk3588_dev/output/update/Image/update.img` |

没有单独的 `backup.img`：package-file 将 backup 标记为 `RESERVED`。这符合实际打包清单，不是缺失错误。

## 7. 启动链分析

```text
RK3588 BootROM
  -> MiniLoaderAll.bin（DDR/FlashData + SPL/FlashBoot）
  -> eMMC GPT 的 uboot 分区中的 uboot.img FIT
  -> ATF BL31 + OP-TEE BL32 + U-Boot proper + U-Boot DTB
  -> boot 分区中的 boot.img FIT
  -> Linux Image + 最终 DTB + resource.img
  -> rootfs 分区（PARTUUID 614e0000-0000）
  -> /sbin/init -> ../bin/busybox
```

### MiniLoaderAll.bin

实际打包配置为 `rkbin/RKBOOT/RK3588MINIALL.ini`：

- TPL/FlashData/DDR 初始化来源：`rkbin/bin/rk35/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.17.bin`。
- SPL/FlashBoot 来源：`rkbin/bin/rk35/rk3588_spl_v1.13.bin`。
- USB plug 辅助固件：`rk3588_usbplug_v1.11.bin`。
- 输出：`u-boot/rk3588_spl_loader_v1.17.113.bin`。
- 打包链：`u-boot/make.sh` -> `u-boot/scripts/loader.sh`/`fit-core.sh` -> `u-boot/tools/boot_merger`。

当前默认配置未设置顶层 `RK_UBOOT_SPL`，因此最终 loader 采用 RKBOOT INI 指定的 rkbin DDR/SPL blob，而不是把工作树中新编的 `u-boot-tpl.bin` 作为最终 FlashData。BootROM 从固定介质位置读取 ID block/loader，DDR 初始化完成后 SPL 根据 Rockchip 启动介质/GPT 布局加载后续 `uboot` 分区 FIT。

### uboot.img FIT

只读 `dumpimage -l` 确认包含：

| 子镜像 | 内容 | 压缩 | 加载地址 |
|---|---|---|---|
| `uboot` | U-Boot proper | gzip | `0x00200000` |
| `atf-1` | ARM Trusted Firmware 主段 | gzip | `0x00040000` |
| `atf-2` | ATF 段 | none | `0xff100000` |
| `atf-3` | ATF 段 | none | `0x000f0000` |
| `optee` | OP-TEE / BL32 | gzip | `0x08400000` |
| `fdt` | U-Boot DTB (`rk3588-alientek`) | none | FIT 未声明 |

ATF 来源于 `rkbin/RKTRUST/RK3588TRUST.ini` 指定的 `rk3588_bl31_v1.46.elf`；OP-TEE 来源为 `rk3588_bl32_v1.16.bin`。没有独立 `trust.img` 分区，信任固件已经作为 `uboot.img` FIT 子镜像加载。

### boot.img FIT

| 子镜像 | 大小 | 说明 |
|---|---:|---|
| `fdt` | 290,164 bytes | 最终 Linux DTB |
| `kernel` | 36,207,104 bytes | 未压缩 AArch64 Linux Image |
| `resource` | 2,126,848 bytes | 多 DTB/Logo 等 Rockchip resource |

普通 `boot.img` **没有 ramdisk**。`boot.its` 中的 `0xffffff00/01` 是打包占位加载值，由 Rockchip U-Boot FIT 启动逻辑处理。Recovery FIT 才包含 11,669,360-byte ramdisk，另含相同 kernel/DTB 和 recovery resource。

原始文件：

```text
Image:    /rk3588_dev/kernel/arch/arm64/boot/Image
DTB:      /rk3588_dev/kernel/arch/arm64/boot/dts/rockchip/
          rk3588-alientek-2mipi720x1280-2hdmi.dtb
resource: /rk3588_dev/kernel/resource.img
modules:  /rk3588_dev/buildroot/output/alientek_rk3588/target/
          lib/modules/5.10.209/
```

主 DTB 的 `/chosen/bootargs` 为：

```text
earlycon=uart8250,mmio32,0xfeb50000 console=ttyFIQ0
irqchip.gicv3_pseudo_nmi=0 root=PARTUUID=614e0000-0000
rw rootwait rcupdate.rcu_expedited=1 rcu_nocbs=all
```

### RootFS

Buildroot 输出目录：`/rk3588_dev/buildroot/output/alientek_rk3588/images/`。

| 文件 | 大小 | 用途 |
|---|---:|---|
| `rootfs.ext2` | 1,491,075,072 | 实际 ext4 RootFS 镜像 |
| `rootfs.ext4` | -> `rootfs.ext2` | ext4 别名 |
| `rootfs.cpio` | 1,223,112,192 | 未压缩 cpio |
| `rootfs.cpio.gz` | 566,834,388 | gzip cpio |
| `rootfs.squashfs` | 570,535,936 | 只读 squashfs |

本配置没有生成 `rootfs.tar`。`buildroot/output/alientek_rk3588/target/` 已包含构建后的普通文件树和 `/lib/modules/5.10.209`，可用于先期 NFS 调试，但 Buildroot 的最终 fakeroot 所有权/设备节点处理发生在镜像生成阶段。正式 NFS RootFS 建议从 ext4 镜像只读挂载后用保留 UID/GID、权限、xattr 和硬链接的方式导出；不要直接把镜像当目录使用，也不要在镜像上写入。

## 8. GPT 分区布局

`parameter.txt` 的单位是 512-byte sector。`MiniLoaderAll.bin` 不在 CMDLINE 列出的普通 GPT 分区内；烧写工具把 bootloader 放到 Rockchip loader/ID block 规定区域。其余布局如下：

| 分区 | 起始 sector | 起始 MiB | 大小 | 对应镜像 | 用途 | 启动关键 |
|---|---:|---:|---:|---|---|---|
| uboot | `0x00004000` | 8 | 4 MiB | `uboot.img` | ATF/OP-TEE/U-Boot FIT | 是 |
| misc | `0x00006000` | 12 | 4 MiB | `misc.img` | 启动模式/恢复控制；本次为空白 | 否 |
| boot | `0x00008000` | 16 | 64 MiB | `boot.img` | Linux/DTB/resource FIT | 是 |
| recovery | `0x00028000` | 80 | 128 MiB | `recovery.img` | 恢复内核和 ramdisk | 回退关键 |
| backup | `0x00068000` | 208 | 32 MiB | `RESERVED` | 保留区，无独立镜像 | 否 |
| rootfs | `0x00078000` | 240 | 14 GiB | `rootfs.img` | Buildroot 根文件系统 | 是 |
| oem | `0x01c78000` | 14,576 | 128 MiB | `oem.img` | OEM 数据/应用 | 否 |
| userdata | `0x01cb8000` | 14,704 | 剩余空间 | `userdata.img` | 可增长用户数据 | 否 |

RootFS UUID：`614e0000-0000-4b53-8000-1d28000054a9`；DTB bootargs 使用其前缀形式 `root=PARTUUID=614e0000-0000`。

## 9. update.img 组成与分发

实际 package-file：`/rk3588_dev/output/update/Image/package-file`：

```text
package-file  package-file
parameter     parameter.txt
bootloader    MiniLoaderAll.bin
uboot         uboot.img
misc          misc.img
boot          boot.img
recovery      recovery.img
backup        RESERVED
rootfs        rootfs.img
oem           oem.img
userdata      userdata.img
```

打包过程：

1. `mk-updateimg.sh` 把 `output/firmware` 的镜像链接到 `output/update/Image/`。
2. 默认模式根据 `parameter.txt` 动态生成 package-file。
3. `tools/linux/Linux_Pack_Firmware/rockdev/afptool -pack` 生成 `update.raw.img`。
4. `rkImageMaker -RK3588 MiniLoaderAll.bin update.raw.img update.img -os_type:androidos` 封装最终镜像。
5. RKDevTool/upgrade_tool 读取容器内 package-file 和 parameter，根据名称及 GPT 起始/大小写入 loader 与各分区。

本次默认完整构建已经执行 updateimg，不需要额外运行 `./build.sh updateimg`。

## 10. 分组件构建命令映射

| 命令 | 主要输入/配置 | 原始与打包输出 | 是否带动其他镜像 |
|---|---|---|---|
| `./build.sh loader` | `u-boot/`、`rkbin/RKBOOT`、`RKTRUST`、`alientek_rk3588_defconfig` | `u-boot/rk3588_spl_loader_v1.17.113.bin`、`u-boot/uboot.img`，链接到 firmware | 本 SDK 中与 uboot 走同一个 `build_uboot` 函数，同时重建 loader 和 U-Boot FIT |
| `./build.sh uboot` | 同上 | 同上 | 与 `loader` 等价；不是只构建 U-Boot proper |
| `./build.sh kernel` | kernel defconfig + `rk3588_linux.config` + 三个 DTS | `Image`、modules、3 DTB、`resource.img`、`kernel/boot.img` | **会重新打包 boot.img**，不会重建 RootFS/update.img |
| `./build.sh modules` | 当前 kernel config/source | 默认安装到 `output/kernel-modules/lib/modules/<release>` | 不重打包 boot.img/rootfs；完整 RootFS 流程另在 post-rootfs 安装模块 |
| `./build.sh buildroot` | 强制选择 `RK_ROOTFS_SYSTEM=buildroot`，使用 `alientek_rk3588_defconfig` | `buildroot/output/alientek_rk3588/images/rootfs.*`，链接 `output/buildroot` 和 firmware/rootfs | 当前配置下会执行完整 rootfs hook，并安装 modules；不重打包 boot.img |
| `./build.sh rootfs` | 使用当前选择的 rootfs system；本板为 buildroot | 当前配置下结果与 buildroot 命令相同 | rootfs 类型若切到 Debian/Yocto，行为会随选择变化 |
| `./build.sh firmware` | 已存在的 loader/boot/rootfs/recovery、parameter | 重建/链接 oem、userdata，检查分区大小，生成 `rockdev` | 因 `RK_UPDATE=y`，会继续打包 update.img；缺 loader 时会补建 loader |
| `./build.sh updateimg` | 已存在 firmware + parameter + package-file | `output/update/Image/update.raw.img`、`update.img` | firmware/parameter 缺失时会先补 `firmware`，但正常情况下不重编组件 |
| `./build.sh all` | 所有启用组件 | 完整 firmware 和 update.img | 与无参数 `./build.sh` 等价 |

`firmware` 的边界是收集、链接和检查分区镜像，并在当前配置中触发 updateimg；`updateimg` 的边界是把已经存在的镜像封装为 Rockchip 升级包。

## 11. TFTP + NFS 准备方案

已创建但未配置服务、未复制镜像的目录：

```text
/rk3588_dev/tftpboot/atk_dlrk3588/
/rk3588_dev/nfs_rootfs/atk_dlrk3588/
/rk3588_dev/deploy/atk_dlrk3588/
```

### 当前可部署输入

```text
TFTP Image: /rk3588_dev/kernel/arch/arm64/boot/Image
TFTP DTB:   /rk3588_dev/kernel/arch/arm64/boot/dts/rockchip/
            rk3588-alientek-2mipi720x1280-2hdmi.dtb
Modules:    /rk3588_dev/buildroot/output/alientek_rk3588/target/
            lib/modules/5.10.209/
RootFS:     /rk3588_dev/buildroot/output/alientek_rk3588/images/rootfs.ext2
```

### U-Boot 能力与限制

- AArch64 原始 `Image` 的启动命令为 `booti`；当前 U-Boot 二进制包含 `booti`。
- `kernel_addr_r=0x00400000`。
- `fdt_addr_r=0x08300000`。
- `ramdisk_addr_r=0x0a200000`。
- `CONFIG_CMD_NET=y`、`CONFIG_CMD_DHCP=y`、`CONFIG_CMD_PING=y`、`CONFIG_CMD_TFTPPUT=y`，二进制中也存在 `tftpboot` 实现。
- `CONFIG_DM_ETH=y`、`CONFIG_DWC_ETH_QOS=y`、`CONFIG_GMAC_ROCKCHIP=y`。
- 但是本次 `u-boot/u-boot.dtb` 没有启用任何 ethernet 节点；基础 RK3588 U-Boot DTS 中 gmac0 为 disabled，板级 U-Boot DTS 没有 override。因而仅凭静态配置不能确认 U-Boot 提示符下网卡可用，也无法可靠给出 `ethact`/设备名。预计设备节点若补齐会是 SoC GMAC（常见环境名 `ethernet@fe1b0000`/`eth0`），必须先在串口执行 `dm tree`、`mii device`、`printenv ethact ethprime`、`dhcp` 验证，不能把 Linux DTS 中两个可用 GMAC 等同于 U-Boot 可用。

### Kernel NFS Root 能力

当前内核已内置：

```text
CONFIG_NET=y
CONFIG_INET=y
CONFIG_NFS_FS=y
CONFIG_STMMAC_ETH=y
CONFIG_DWMAC_ROCKCHIP=y
CONFIG_MOTORCOMM_PHY=y
CONFIG_REALTEK_PHY=y
```

当前缺少：

```text
CONFIG_ROOT_NFS
CONFIG_IP_PNP
CONFIG_IP_PNP_DHCP
```

因此本次生成的内核 **不能直接作为 NFS Root 闭环使用**。后续应把上述三项设为 `y`（内置，不能是 module），重新执行 `./build.sh kernel`，再确认 GMAC/PHY 仍为内置。该改动不属于本次 Phase 0，未执行。

### 建议 U-Boot 命令（仅方案，未执行）

先解决/确认 U-Boot Ethernet DTS 后，使用独立环境变量，保留 eMMC 默认启动为回退：

```text
setenv serverip <TFTP_NFS_SERVER_IP>
setenv ipaddr <BOARD_IP>                 # 或先 dhcp
tftp ${kernel_addr_r} atk_dlrk3588/Image
tftp ${fdt_addr_r} atk_dlrk3588/rk3588-alientek-2mipi720x1280-2hdmi.dtb
setenv bootargs 'earlycon=uart8250,mmio32,0xfeb50000 console=ttyFIQ0 root=/dev/nfs rw nfsroot=<SERVER_IP>:/rk3588_dev/nfs_rootfs/atk_dlrk3588,vers=3,tcp ip=<BOARD_IP>:<SERVER_IP>:<GATEWAY>:<NETMASK>:atk-dlrk3588:eth0:off rootwait'
booti ${kernel_addr_r} - ${fdt_addr_r}
```

使用 DHCP 时可将 `ip=...` 改为 `ip=dhcp`。首次验证不要 `saveenv`；复位即可回到 eMMC 的 `root=PARTUUID=614e0000-0000`。确认完整启动后再决定是否持久化单独的 `nfsboot` 环境脚本。

### RootFS 导出建议

优先把 `rootfs.ext2` 按 ext4 只读挂载到临时目录，再以保留 numeric owner、mode、xattr、ACL、hardlink 的方式同步到 NFS 目录。mount/loop 与保留所有权的同步需要时，只对具体命令使用 sudo；不要 sudo 运行 SDK 构建。另一个长期方案是在 Buildroot defconfig 中启用 `BR2_TARGET_ROOTFS_TAR` 后重建 RootFS，获得适合解包的 fakeroot tar。本次未修改配置，也未挂载或导出 RootFS。

## 12. 首次烧写建议与检查清单

本次未执行烧写。后续首次烧写前：

1. 保存本报告、完整构建日志和 `update.img` SHA-256。
2. 再次确认目标设备确为 ATK-DLRK3588，显示组合对应 720x1280 双 MIPI + 双 HDMI 配置。
3. 确认 Rockchip USB VID/PID、Maskrom/Loader 模式和唯一设备枚举，避免多板误写。
4. 使用工具先读取/确认设备信息，不要直接选择擦除全部存储。
5. 首次优先使用完整 `update.img`，或严格按 parameter/package-file 写入；不要混用其他 SDK 的 loader/parameter。
6. 烧写后保存串口全日志，核对 DDR、loader、ATF、OP-TEE、U-Boot、FIT hash、kernel、rootfs PARTUUID。
7. 验证 recovery 可进入后再做网络启动或持久化 U-Boot 环境变量。
8. 保留本次完整镜像和 eMMC rootfs 作为已知可回退基线。

## 13. 回退与风险

- 显示配置不匹配会导致 MIPI 屏参数不正确；本次主 DTB 明确是 720x1280 版本。
- U-Boot 网络命令和驱动虽已编译，但 U-Boot DTB 没有启用网卡节点；在修复/验证之前，TFTP 方案不是可执行闭环。
- 当前 kernel 缺 NFS Root 和 IP autoconfig 配置；即使 U-Boot TFTP 成功，现有内核也不能直接挂载 NFS 根目录。
- RootFS 没有 tar，直接用 Buildroot `target/` 做长期 NFS 导出可能丢失 fakeroot 所有权语义。
- `userdata` 是 grow 分区；按单分区烧写和全量烧写的扩容行为不同，首次应按厂商完整包流程验证。
- 不应混用其他板级 parameter、MiniLoader 或 DTB；loader/DDR 固件错误可能导致设备仅能进入 Maskrom 恢复。

## 14. 本次实际修改与生成内容

人工创建：

```text
/rk3588_dev/docs/bringup/phase0_sdk_full_build_report.md
/rk3588_dev/logs/sdk-build/rk3588_sdk_full_build_20260729_230258.log
/rk3588_dev/logs/sdk-build/rk3588_sdk_full_build_20260729_230258.meta
/rk3588_dev/tftpboot/atk_dlrk3588/             (空目录)
/rk3588_dev/nfs_rootfs/atk_dlrk3588/           (空目录)
/rk3588_dev/deploy/atk_dlrk3588/               (空目录)
```

厂商构建脚本生成/更新了 `output/`、`rockdev`、`kernel/`、`u-boot/`、`buildroot/output/` 中的正常构建产物。没有手工编辑这些生成文件，没有编辑厂商源码，没有创建 Git commit。

## 15. 尚未解决的问题

1. U-Boot 板级 DTB 未启用 GMAC，U-Boot 网卡设备名和实际 DHCP/TFTP 能力需要串口板测，必要时另立变更修改 U-Boot DTS。
2. Kernel 的 `CONFIG_ROOT_NFS`、`CONFIG_IP_PNP`、`CONFIG_IP_PNP_DHCP` 尚未启用；NFS Root 需后续配置并分项重建 kernel。
3. 没有生成 `rootfs.tar`；正式 NFS 导出需只读挂载 ext4 镜像或后续启用 tar 输出。
4. 未在真实开发板上验证启动、双网口、显示、recovery 或分区扩容；本次范围明确不包括烧写。

## 16. 最终摘要

```text
SDK路径：/rk3588_dev
当前用户：vscode (uid=1000 gid=1000)
板级配置：01_atk_dlrk3588_auto2mipi_2hdmi_defconfig
完整构建结果：成功，BUILD_RC=0，耗时 3:43:37
构建日志：/rk3588_dev/logs/sdk-build/rk3588_sdk_full_build_20260729_230258.log
update.img路径：/rk3588_dev/output/update/Image/update.img
update.img SHA-256：f96ba33de5f7e7fb7b23677d1d422111676df5bf967c08cb4cc3146a4684ac49
Image真实路径：/rk3588_dev/kernel/arch/arm64/boot/Image
DTB真实路径：/rk3588_dev/kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-2mipi720x1280-2hdmi.dtb
rootfs产物：/rk3588_dev/buildroot/output/alientek_rk3588/images/rootfs.ext2
kernelrelease：5.10.209
报告路径：/rk3588_dev/docs/bringup/phase0_sdk_full_build_report.md
是否修改源码：否
是否执行烧写：否
下一步建议：先串口验证 U-Boot GMAC；启用内置 ROOT_NFS/IP_PNP 后分项重建 kernel；随后准备只读导出的 NFS RootFS。
```
