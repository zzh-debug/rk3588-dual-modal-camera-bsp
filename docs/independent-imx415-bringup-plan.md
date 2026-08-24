# RK3588 IMX415 独立导入项目章程

> 文档状态：计划基线，尚未实施内核或 DTS 修改
>
> 制定日期：2026-08-01
>
> 工作区：`/rk3588_dev`
>
> 目标板：ATK-DLRK3588，Rockchip Linux 5.10.209
>
> 板级配置：`01_atk_dlrk3588_auto2mipi_2hdmi_defconfig`
>
> 日常启动路径：TFTP 最新 `Image + rk3588-alientek-nfs.dtb`，U-Boot 执行 `run nfsbootfdt`

## 1. 章程结论

本项目的主线不是在厂商 IMX415 驱动已经存在的前提下，只寻找一个厂商尚未实现的生产缺陷；而是假设 RK3588 通用 Camera Host 框架已经具备、目标连接器上的 IMX415 板级适配和 Sensor 驱动尚未完成，按真实 BSP 工程流程独立导入 ATK-MCIMX415。

厂商实现继续保留，但其定位是黄金基线、硬件参考、未公开寄存器参考和 A/B 对照答案。自研实现必须使用独立源文件、Kconfig 符号和 OF compatible，不删除、不覆盖厂商 `imx415.c`，也不声称填补了厂商 SDK 的产品空白。

项目按 P0-P8 顺序推进。P0 只做最小黄金基线，完成后必须立即进入 P1 硬件契约和 P2 最小自研 Sensor 驱动；在自研 RAW 链路稳定前，不让 RKCIF 可观测性、超时恢复或核心 DMA 修改取代 Sensor 导入主线。

### 1.1 项目目标

独立完成并用真机证据闭环以下能力：

1. 从原理图、Sensor 资料和 SoC Camera 文档还原 I2C、供电、MCLK、reset、PWDN、MIPI lane 和 Media Graph；
2. 实现最小 I2C Sensor 驱动，完成资源获取、上电、Chip ID、失败回滚和 remove；
3. 实现一个 3864x2192 RAW10、4-lane、30 fps 固定模式及可靠的 `s_stream()`；
4. 完整实现 V4L2 Sensor Subdev、source pad、格式、selection 和基础 controls；
5. 完成 runtime PM、状态互斥、并发保护和异常路径；
6. 打通 Sensor 到 RKCIF 的 RAW10 采集，再打通 RKISP 到 NV12 的成像闭环；
7. 通过 A/B、循环启停、Buffer 异常、长稳和错误计数证明实现的正确性与可靠性。

### 1.2 第一版模式的准确口径

第一版不是一个未经证明的“3840x2160 Sensor 寄存器模式”，而是：

```text
Sensor 输出：3864x2192
Media Bus：MEDIA_BUS_FMT_SGBRG10_1X10
MIPI CSI-2：4 lane
帧率：30 fps
XVCLK/INCK：37.125 MHz
Lane rate：Sony 表中的 891 Mbps/lane 名义值
V4L2 link-frequency 元数据：约 446 MHz，对应约 892 Mbps/lane DDR rate
有效成像区域：居中 3840x2160 crop/selection
```

现有厂商 mode 的 `VMAX=0x08ca=2250`、`HMAX=0x044c=1100` 可作为对照。最终自研寄存器表必须建立来源台账；数据手册能解释的项目按手册推导，手册未公开的寄存器明确标成厂商参考值，不能伪装成自行推导。

### 1.3 两类价值分别评价

| 工作项 | 生产新增价值 | BSP 学习与能力证明价值 | 项目决策 |
|---|---|---|---|
| 原厂黄金基线 | 低 | 高：建立可复现对照和回退 | P0 必做，但不能停在 P0 |
| 硬件契约和单路 DTS | 中 | 高：板级资源、时序和 graph 是 Sensor 导入基础 | P1、P6 必做 |
| 独立最小 Sensor 驱动 | 低到中：厂商已有正式实现 | 很高：覆盖 I2C、资源、时序和错误回滚 | P2 必做 |
| 固定 3864x2192 RAW10 30 fps | 低：厂商已有同类 mode | 很高：覆盖寄存器、MIPI 与帧时序 | P3 必做 |
| V4L2 Subdev 和 controls | 低到中 | 很高：体现 Linux Media Sensor 驱动完整性 | P4 必做 |
| runtime PM 和异常路径 | 中 | 很高：体现生产级生命周期与并发意识 | P5 必做 |
| RKCIF RAW 和 RKISP NV12 联调 | 中 | 很高：证明不是只会写孤立 Sensor 文件 | P6、P7 必做 |
| 稳定性与厂商 A/B | 中到高 | 很高：用数据证明边界和质量 | P8 必做 |
| RKCIF Host 修复 | 取决于是否发现真实缺陷 | 高，但必须有复现和归因 | P8 后条件立项 |

### 1.4 明确不做和不得声称的事项

- 不重写 Rockchip D-PHY/DC-PHY、CSI2 Host、RKCIF、RKISP、VB2 或 Media Controller 核心。
- 不因项目交付压力，在没有真机复现和责任层证据时修改 RKCIF ping-pong/DMA、reset work 或透明恢复。
- 第一版不同时扩展 HDR、双摄、MPP 编码、日夜切换和 L3 内核透明恢复。
- 不删除或改名 `kernel/drivers/media/i2c/imx415.c`，不复用 `sony,imx415` 让两个驱动竞争。
- 不把复制厂商驱动并批量改名描述为“从零实现”。允许参考厂商 mode table，但每个寄存器组必须记录来源。
- 不声称厂商 SDK 原本没有 IMX415 支持；当前 `CONFIG_VIDEO_IMX415=y`、`imx415.o`、四路 `sony,imx415` DTS 和 IQ 文件均是已确认事实。
- 不把 3840x2160 crop 描述成已经存在的独立 Sensor mode。
- 不把计划、编译通过或 probe 成功描述为已经出图、长稳通过或产品化完成。

## 2. 厂商路径与自研路径隔离

### 2.1 命名和代码边界

| 项目 | 厂商黄金路径 | 自研实验路径 |
|---|---|---|
| OF compatible | `sony,imx415` | `zzh,imx415` |
| 驱动源码 | `kernel/drivers/media/i2c/imx415.c` | `kernel/drivers/media/i2c/zzh_imx415.c` |
| Kconfig | `CONFIG_VIDEO_IMX415` | `CONFIG_VIDEO_ZZH_IMX415` |
| 对象/模块名 | `imx415.o` / `imx415` | `zzh_imx415.o` / `zzh_imx415` |
| I2C driver name | `imx415` | `zzh_imx415` |
| DTS | 目标节点保持 `sony,imx415` | 只把已选中的一个节点覆盖为 `zzh,imx415` |
| 构建产物 | P0 已知可用 Image/DTB | 每阶段新 Image/DTB，标明 commit、hash 和阶段 |

`zzh,imx415` 是本地教学实验 compatible，用于明确隔离，不作为可直接上游的正式 binding。若未来做 upstream 版本，需要重新讨论标准 `sony,imx415` binding 和驱动替换策略，不能把本地隔离方案当成上游 ABI。

### 2.2 唯一绑定规则

1. 一个 I2C 设备节点在任一 DTB 中只放一个 compatible，不使用 `compatible = "zzh,imx415", "sony,imx415"` 的 fallback 列表。
2. `zzh_imx415` 的 OF match table 只匹配 `zzh,imx415`；厂商驱动只匹配原有 `sony,imx415`。
3. 自研 DTS 只切换 P0/P1 已确认的物理连接器，其余三个 Sensor 节点及 endpoint 不因本实验产生新的 compatible 或错误绑定。
4. 第一版不创建第二个同地址 `@1a` 节点；在已选节点上覆盖 compatible，并继承其 I2C 地址、clock、GPIO、module metadata 和 endpoint。
5. Image 可同时编入厂商和自研驱动，绑定选择完全由目标 DTB 的单一 compatible 决定。这样 A/B 通常只需切换 DTB，不必删除任一驱动。

### 2.3 DTS 隔离方案

预计新建一个仅含实验覆盖的文件：

```text
kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-zzh-imx415.dtsi
```

在目标 `rk3588-alientek-2mipi1080x1920-2hdmi.dts` 中，将它放在厂商 `rk3588-alientek-cameras.dtsi` 之后 include。覆盖文件只修改 P0 确认的 Sensor 节点 compatible，不复制整条 Media Graph。以 J20/CSI3 为示意而非当前选型事实：

```dts
&imx415 {
	compatible = "zzh,imx415";
};
```

J20 在现有 DTS 中有 `imx415` label；若实物连接在 J18、J19 或 J21，则应按对应 I2C bus 和 `imx415@1a` 节点路径做最小覆盖。P0 未确认实物连接器前不得提交具体覆盖。

厂商 DTS 文件保持可审计。实验回退方式是去掉该 include 或部署 P0 厂商 DTB，而不是删除 endpoint、Host 节点或厂商驱动。

### 2.4 提交、产物和回退纪律

- 每个阶段至少一个独立提交；驱动骨架、固定 mode、V4L2 controls、runtime PM、DTS、测试脚本不混成一次大提交。
- 每次真机测试记录源码提交、`Image`/DTB SHA-256、`/proc/cmdline`、根文件系统来源、连接器、media graph、设备节点和测试命令。
- 厂商 A/B 与自研 A/B 必须使用相同模组、连接器、Host graph、格式、测试时长和用户态工具。
- 保留 eMMC 原厂系统作为最终回退；网络开发始终使用当前固定 1080x1920 DTS 生成的 `rk3588-alientek-nfs.dtb`。
- 只执行 kernel 分项构建。不要为了本项目执行全量构建、`repo init`、清理厂商仓库或修改 `output/`、`rockdev/` 生成物。

## 3. 复用模块与重新实现模块

### 3.1 复用而不重写

```text
V4L2 / Media Controller / VB2 core
                    |
自研 zzh_imx415 --> Rockchip D-PHY/DC-PHY --> MIPI CSI-2 Host
                                             |
                                             +--> RKCIF RAW video node
                                             |
                                             +--> RKCIF SDITF --> RKISP --> NV12
```

| 复用模块 | 复用范围 | 本项目责任 |
|---|---|---|
| RK3588 I2C/clock/GPIO/pinctrl/regulator | Linux 通用资源框架和 SoC controller | 正确声明、获取、排序、回滚和实测 |
| V4L2/Media Controller/VB2 | 框架、ioctl、entity、queue core | 实现符合框架契约的 Sensor subdev，不重写 core |
| Rockchip D-PHY/DC-PHY | PHY 驱动及硬件控制 | DTS lane/endpoint 正确，检查 PHY 错误 |
| Rockchip CSI2 Host | CSI-2 接收、CRC/ECC/SOT 统计 | 配置匹配并归因链路错误 |
| RKCIF/VICAP | RAW capture、SDITF 和 DMA | 使用现有 video node 采集，不先改核心 DMA |
| RKISP | ISP pipeline、mainpath/selfpath | 匹配 IQ、Bayer、crop 和 NV12 输出 |
| 部署与网络启动 | `deploy_net_boot.sh`、TFTP/NFS | 构建后部署正确 1080x1920 DTB，执行 `run nfsbootfdt` |

### 3.2 独立重新实现

- 目标连接器上的 `zzh,imx415` 节点切换和 Sensor 到 PHY 的 endpoint 契约确认；
- 16-bit 寄存器地址、8-bit 数据的 I2C 读写封装；
- clock、GPIO、regulator、pinctrl 资源管理和上/下电时序；
- Chip ID 读取、判断、错误返回和 probe 回滚；
- 3864x2192 RAW10 30 fps 固定 mode、寄存器表和 standby/stream；
- V4L2 subdev、media entity、source pad、format/selection；
- LINK_FREQ、PIXEL_RATE、HBLANK、VBLANK、EXPOSURE、ANALOGUE_GAIN；
- runtime PM、mutex、stream/power 状态和 remove；
- 板级联调、RAW/RKISP 验证、自动化循环和 A/B 报告。

Rockchip 私有 module-info ioctl 是否为 RKISP/AIQ 闭环所必需，在 P7 由真机和厂商接口文档确认后按最小集合实现。它不能在 P2 被无解释地整段复制，也不能为追求“纯净”而忽略实际 BSP ABI。

## 4. 阶段门禁总则

每个阶段都遵循以下规则：

1. 只有前一阶段验收通过，才能进入下一阶段；失败时回到最近的已知可用 Image/DTB。
2. 编译通过只证明静态集成，不等于 probe、出流、图像或稳定性通过。
3. DTS 或驱动 C 代码变更后执行 `./build.sh kernel`，再部署固定 1080x1920 DTS；本项目日常启动统一使用 `run nfsbootfdt`。
4. 阶段日志至少包含 `dmesg`、media graph、V4L2 fmt/control、测试返回值和 Image/DTB hash。
5. 任何 Host 错误先按 Sensor、PHY、CSI Host、RKCIF、RKISP、用户态分层归因，不以一个 `dmesg` 关键词直接修改 RKCIF。

标准构建和部署命令为：

```bash
cd /rk3588_dev
./build.sh kernel
./projects/scripts/deploy_net_boot.sh \
    -d rk3588-alientek-2mipi1080x1920-2hdmi \
    -m
```

如果自研驱动编为 built-in 且模块集合没有变化，`-m` 可省略；保留 `-m` 是为了阶段间统一同步当前内核模块。部署脚本中的 `sudo rsync/chown` 只用于保持 NFS RootFS 属主，SDK 编译仍必须由 `vscode` 普通用户完成。

开发板启动前后至少核对：

```text
U-Boot:
printenv nfsbootfdt nfsimage nfsfdt ipaddr serverip kernel_addr_r fdt_addr_r
run nfsbootfdt

Linux:
cat /proc/cmdline
mount | grep ' / '
cat /etc/rootfs-source
ip addr show eth0
dmesg | tail -200
```

### 4.1 阶段验证命令模板

设备号由注册顺序动态决定。先根据 `media-ctl -p` 填写以下变量，禁止把示例占位符直接当成板端事实：

```sh
MEDIA_DEV=/dev/mediaX
SENSOR_SUBDEV=/dev/v4l-subdevX
RAW_NODE=/dev/videoX
ISP_NODE=/dev/videoY
I2C_DEV=N-001a
```

P0/P1 基线与 graph：

```sh
v4l2-ctl --list-devices
media-ctl -d "$MEDIA_DEV" -p
v4l2-ctl -d "$SENSOR_SUBDEV" --all
cat /sys/bus/i2c/devices/"$I2C_DEV"/name
readlink /sys/bus/i2c/devices/"$I2C_DEV"/driver
cat /sys/kernel/debug/clk/clk_summary | grep -E 'mipi.*camera|clk_mipi'
dmesg | grep -Ei 'imx415|csi|dphy|rkcif|rkisp'
```

P2 自研绑定与 Chip ID：

```sh
readlink /sys/bus/i2c/devices/"$I2C_DEV"/driver
cat /sys/bus/i2c/devices/"$I2C_DEV"/name
dmesg | grep -Ei 'zzh_imx415|chip.*id|xvclk|regulator|pinctrl'
```

P3/P6 固定流与 RAW 采集。P3 先做少量帧，P6 再递增到 300/3000；首轮用 `/dev/null` 避免把数 GB RAW 数据写入 NFS，另存一帧用于内容检查：

```sh
v4l2-ctl -d "$RAW_NODE" --get-fmt-video
v4l2-ctl -d "$RAW_NODE" --stream-mmap=4 --stream-count=10 --stream-to=/dev/null
v4l2-ctl -d "$RAW_NODE" --stream-mmap=4 --stream-count=300 --stream-to=/dev/null
v4l2-ctl -d "$RAW_NODE" --stream-mmap=4 --stream-count=3000 --stream-to=/dev/null
v4l2-ctl -d "$RAW_NODE" --stream-mmap=4 --stream-count=1 --stream-to=/tmp/imx415-one-frame.raw
```

P4 format/selection/control。具体 `pad` 和 control 名称先从 `--all`/graph 确认：

```sh
v4l2-ctl -d "$SENSOR_SUBDEV" --all
v4l2-ctl -d "$SENSOR_SUBDEV" --get-subdev-fmt pad=0
v4l2-ctl -d "$SENSOR_SUBDEV" --get-subdev-selection pad=0,target=crop_bounds
v4l2-ctl -d "$SENSOR_SUBDEV" --list-ctrls-menus
v4l2-ctl -d "$SENSOR_SUBDEV" --get-ctrl=link_frequency,pixel_rate,hblank,vblank,exposure,analogue_gain
```

P5 先执行 100 次短流。测试前后各保存一次 dmesg、runtime status、clock summary 和内存快照：

```sh
i=1
while [ "$i" -le 100 ]; do
	v4l2-ctl -d "$RAW_NODE" --stream-mmap=4 --stream-count=3 \
		--stream-to=/dev/null || exit 1
	i=$((i + 1))
done
cat /sys/bus/i2c/devices/"$I2C_DEV"/power/runtime_status
cat /proc/meminfo
dmesg | tail -300
```

P7 NV12 冒烟：

```sh
v4l2-ctl -d "$ISP_NODE" --get-fmt-video
v4l2-ctl -d "$ISP_NODE" \
	--set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
	--stream-mmap=4 --stream-count=300 --stream-to=/dev/null
```

P8 不用人工重复上述循环，而是在项目内测试工具实现固定 run ID、seed、CSV 和失败 bundle 后执行 100/500/1000 次及 2 h/8 h。工具尚未实现前，文档不得给出虚构的脚本路径或成功输出。

## 5. P0-P8 实施计划

### P0：厂商最小黄金基线

**目的：** 证明模组、连接器和 Rockchip Host 链路至少有一条已知可用路径，并冻结可回退证据。P0 是起点，不是项目终点。

**输入：** 当前厂商 Image/DTB、`sony,imx415` 驱动、原厂应用/IQ、已跑通的 TFTP/NFS 启动链。

**代码范围：** 不改 kernel、DTS、U-Boot 和 RootFS；只在 `projects/rk3588_camera_bsp/` 保存 manifest、命令和日志。

**任务与验证：**

1. 记录 `git -C kernel status --short`，不清理现有用户修改；记录 Image/DTB/IQ SHA-256。
2. 执行 `run nfsbootfdt`，用 `/proc/cmdline` 和 NFS mount 证明 Linux 使用最新网络 DTB/RootFS。
3. 确认模组实际插在 J18-J21 中的哪一个接口；记录对应 I2C adapter、Sensor subdev、`/dev/media*` 和 video node，不能假定 `/dev/video0`。
4. 保存 `media-ctl -p`、`v4l2-ctl --list-devices`、subdev format/selection/controls 和相关 dmesg。
5. 用厂商应用完成预览/抓图；条件允许时做一轮短 RAW 和 NV12 冒烟，记录协商格式、帧数和错误计数。

**通过门槛：** 唯一确认实际连接器和动态设备节点；基础出图可复现；Image/DTB/IQ/graph 均可追溯。

**失败与回退：** 网络基线失败则启动 eMMC 原厂系统对照。若 eMMC 原厂路径也不能出图，停止自研代码推进，先排查接线、供电、模组和硬件。

**产物：** `P0-baseline-manifest.md`、media graph、dmesg、fmt/control 快照、known-good hash。

**后续约束：** P0 一旦通过，不新增“只继续采集基线”的阶段，立即执行 P1 并开始 P2 设计。

### P1：硬件契约分析

**目的：** 把原理图、数据手册、厂商 DTS 和真机连接器收敛成自研驱动可执行的硬件契约。

**输入：** P0 实物接口和 graph、ATK-DLRK3588B V1.2 底板原理图、ATK-MCIMX415 V1.4 原理图、IMX415 数据手册、Rockchip VI 指南、现有 camera DTS。

**代码范围：** 默认只新增项目文档和 DTS 设计草案，不修改 RKCIF 或 Sensor mode table。

**必须确认：**

- 目标 I2C adapter、`0x1a` 地址和总线可达性；
- `CLK_MIPI_CAMARAOUT_M*` 来源、pinctrl 和 37.125 MHz 实际输出；
- reset 的 active-low 语义；现有 `power-gpios` 实际连接模组 `CSI_PDN`，不是三路 Sensor 电源轨；
- 模组 3.3 V 输入与本地 2.8/1.8/1.2 V LDO，DTS 缺少三路 supply 时 dummy regulator 的实际行为；
- PWDN/reset/MCLK/I2C 的上电和下电先后、最小等待时间；
- `<1 2 3 4>` 四 lane 排列、RAW10、GBRG、891 Mbps/lane 量级；
- Sensor endpoint 到 PHY、CSI2 Host、RKCIF、SDITF、RKISP 的双向 remote-endpoint；
- 3864x2192 Sensor output 与 3840x2160 selection 的责任层。

P0 确认连接器后，从下表冻结唯一一行：

| 接口 | I2C/MCLK | 当前 DTS GPIO 声明 | Host graph |
|---|---|---|---|
| J18 / CSI1 | I2C7，`CLK_MIPI_CAMARAOUT_M1` | GPIO1_PD2 `ACTIVE_LOW` / GPIO1_PA2 `ACTIVE_HIGH` | `csi2_dcphy0 -> mipi0_csi2 -> rkcif_mipi_lvds -> rkisp1_vir0` |
| J19 / CSI2 | I2C2，`CLK_MIPI_CAMARAOUT_M2` | GPIO1_PD3 `ACTIVE_LOW` / GPIO1_PA4 `ACTIVE_HIGH` | `csi2_dcphy1 -> mipi1_csi2 -> rkcif_mipi_lvds1 -> rkisp1_vir1` |
| J20 / CSI3 | I2C3，`CLK_MIPI_CAMARAOUT_M3` | GPIO1_PB1 `ACTIVE_LOW` / GPIO1_PA7 `ACTIVE_HIGH` | `csi2_dphy0 -> mipi2_csi2 -> rkcif_mipi_lvds2 -> rkisp0_vir0` |
| J21 / CSI4 | I2C4，`CLK_MIPI_CAMARAOUT_M4` | GPIO1_PB2 `ACTIVE_LOW` / GPIO1_PB0 `ACTIVE_HIGH` | `csi2_dphy3 -> mipi4_csi2 -> rkcif_mipi_lvds4 -> rkisp0_vir1` |

**验证：** I2C 只做无破坏性识别；通过 debugfs/clock summary、GPIO 状态和必要时示波器核对 MCLK/电平；将运行 graph 与 DTS graph 逐 endpoint 对账。

**通过门槛：** 形成无 TBD 的目标接口硬件检查表、power sequence、graph 和实验 DTS 设计；Chip ID 和 mode 寄存器的来源等级已标注。

**失败与回退：** 保持厂商 DTS/驱动运行；任何原理图和真机矛盾先测量，不通过猜测修改有效电平。

**产物：** `P1-hardware-contract.md`、目标 graph、上/下电时序表、寄存器来源台账初版、DTS 设计草案。

### P2：最小自研 Sensor 驱动

**目的：** 在不要求出图的前提下，独立完成 I2C 驱动、资源管理、上/下电、Chip ID、失败回滚和 remove。

**输入：** P1 硬件契约、Linux I2C/clock/GPIO/regulator API、厂商驱动仅作 A/B 参考。

**代码范围：**

- 新建 `kernel/drivers/media/i2c/zzh_imx415.c`；
- 在 `kernel/drivers/media/i2c/Kconfig` 增加 `VIDEO_ZZH_IMX415`；
- 在 `kernel/drivers/media/i2c/Makefile` 增加 `zzh_imx415.o`；
- 在 `kernel/arch/arm64/configs/alientek_rk3588_defconfig` 启用该符号；
- 增加单路实验 DTS 覆盖及顶层 DTS include；
- 不修改厂商 `imx415.c`，不修改 RKCIF/RKISP。

**实现内容：**

1. `i2c_driver`、`of_match_table` 和 `zzh,imx415`；
2. 明确实现 16-bit 大端寄存器地址、8-bit 数据的 read/write helper，并严格判断 `i2c_transfer()` 消息数；
3. 获取 `xvclk`、reset、PWDN、pinctrl 和实际存在的 supply；对 optional 与 required 资源作明确决定；
4. 按 P1 契约实现 `power_on()`/`power_off()`，每个失败点逆序回滚；
5. 上电后读取 Chip ID，I2C 错误优先返回原错误，不把总线错误折叠成单一 `-ENODEV`；
6. probe 失败和 remove 后保证 clock、GPIO、regulator、pinctrl 状态可预测；
7. 日志包含驱动身份、adapter/address、实际 xvclk、Chip ID 和失败阶段，不在正常路径逐寄存器刷屏。

现有厂商驱动使用 `0x311a -> 0xe0`，但源码在该定义旁标有“Get the real chip id” TODO。因此 P2 把它列为“厂商参考值”，必须结合数据手册可获得内容和实物读值交叉确认后，才冻结为自研识别契约。

**验证：**

- 先编译，再使用实验 DTB 启动，确认只有 `zzh_imx415` 绑定目标 `@1a`；
- 核对 probe 成功日志、I2C 地址、37.125 MHz 和电源状态；
- 在断开模组、错误 compatible/地址等可恢复条件下验证 probe 失败及资源回滚；
- 切回厂商 DTB，确认同一 Image 中 `sony,imx415` 仍可绑定并出图。

断开模组的 negative test 必须先让开发板完全断电再拔插，禁止带电插拔 MIPI 模组。P2 尚未注册 Sensor subdev，因此 media graph 中缺少该 Sensor entity、RKCIF 尚不能出帧是本阶段的预期状态，不应误判成 Host 回归。

**通过门槛：** 自研驱动稳定读到已确认 Chip ID；重复 bind/unbind 或重启无资源泄漏和异常日志；厂商回退路径不受影响。本阶段不以“出图”为验收条件。

**失败与回退：** 部署 P0 DTB，将节点恢复为 `sony,imx415`；必要时同时部署 P0 Image。不得通过覆盖厂商源文件“快速恢复”。

**产物：** driver skeleton patch、Kconfig/Makefile/DTS patch、probe/negative-test 日志、P2 设计说明。

### P3：固定模式与 `s_stream()`

**目的：** 让 Sensor 正确产生 3864x2192 RAW10、4-lane、30 fps MIPI 数据。

**输入：** P2 最小驱动、数据手册时序表、厂商 3864x2192/891M mode 作为带来源标记的参考。

**代码范围：** 扩展 `zzh_imx415.c` 的 mode、register table 和 stream state，并增加从标准 pipeline 触发 `.s_stream` 所必需的最小 `v4l2_subdev`、source pad、固定 ACTIVE format 和 video ops；不实现完整枚举、TRY format、selection 和 controls，不实现 HDR，不改 Host。

**实现内容：**

- mode struct：3864x2192、SGBRG10、4 lane、30 fps、37.125 MHz；
- `VMAX=2250`、`HMAX=1100` 以及 standby、master/start、MIPI timing；
- global 与 mode register table 分层，支持 delay/end sentinel，并在首个写失败时停止；
- `s_stream(1)`：按 P2 的临时显式电源持有规则确保设备上电，写 global/mode 和固定初值，再退出 standby；任一步失败都回到非 streaming 状态；
- `s_stream(0)`：进入 standby，保留并返回 I2C 错误，随后按临时显式电源规则释放资源；P5 再将这套持有规则收敛为 runtime PM 引用；
- mutex 下维护 `streaming`，重复 ON/ON 和 OFF/OFF 幂等；
- 对实际帧周期核算 `2250 * 1100 / 74.25 MHz = 33.333 ms`，不把 V4L2 `hts_def` 元数据混成 Sony HMAX 公式。

**寄存器纪律：** 每组寄存器标注 `[D]` 数据手册、`[V]` 厂商参考或 `[M]` 真机测量/读回。手册保留位不靠猜测填写；只能来自厂商表的值保留 `[V]`。

**验证：** Sensor stream 日志和 MIPI/PHY/CSI Host 状态；确认无持续 CRC/ECC/SOT；用帧开始/结束或下游最小采集证明约 30 fps。此阶段只证明 Sensor 产生预期 MIPI 数据，不宣称 V4L2 controls 已完整。

**通过门槛：** 连续启停至少 10 次，Sensor 每次进入/退出 streaming；30 fps 和 MIPI lane 配置符合预期；失败注入后仍可再次启动。

**失败与回退：** 切回 P2 仅 Chip ID 版本定位 power/I2C，或切回厂商 DTB 对照硬件；不修改 RKCIF 来掩盖 Sensor 寄存器错误。

**产物：** mode patch、register-source ledger、时序计算、首帧/错误日志。

### P4：V4L2 Subdev 完整化

**目的：** 让自研 Sensor 以完整且可协商的 Media Controller 实体接入 Rockchip pipeline。

**输入：** P3 固定 mode、V4L2 subdev/pad/control API、运行 graph 要求。

**代码范围：** `zzh_imx415.c` 的 subdev、media entity、pad 和 controls；DTS 只在 graph 契约有证据错误时调整。

**实现内容：**

- 完整化 P3 的最小 `v4l2_subdev`/source pad，增加 devnode/internal ops、标准 entity function 和完整 pad ops；
- `enum_mbus_code()`、`enum_frame_size()`、`get_fmt()`、`set_fmt()`；
- TRY format 与 ACTIVE format 分开保存，固定 mode 仍需返回合法协商结果；
- `get_selection()` 为 3864x2192 输出声明居中 3840x2160 crop bounds/default；
- LINK_FREQ、PIXEL_RATE、只读 HBLANK、VBLANK、EXPOSURE、ANALOGUE_GAIN；
- `PIXEL_RATE = link_freq * 2 * 4 / 10`，使用约 446 MHz 菜单值时为 356.8 MP/s 元数据；
- VBLANK 改变时同步 VMAX、当前 VTS 和 EXPOSURE 上限；EXPOSURE 按 SHR0/VMAX 约束转换；
- control setup 与 stream start 的顺序确定，I2C 写失败可见且不会错误更新软件状态。

**验证：** 用 `media-ctl` 和 `v4l2-ctl` 枚举 code、size、TRY/ACTIVE fmt、selection 和 controls；逐项测试最小值、默认值、最大值及越界返回；读回帧率和曝光/增益实际效果。

**通过门槛：** Media Graph 出现独立自研 Sensor entity/source pad；格式和 crop 语义一致；controls 边界无溢出，VBLANK/EXPOSURE 动态约束正确。

**失败与回退：** 回到 P3 固定内部配置版本或厂商 DTB；不把格式协商失败归因成 Host DMA 故障。

**产物：** V4L2 patch、API/控制映射表、fmt/selection/control 测试日志。

### P5：runtime PM 与错误路径

**目的：** 把能出流的驱动收敛为生命周期、并发和失败路径可验证的实现。

**输入：** P4 驱动、kernel PM/runtime API、P2-P4 已记录的失败点。

**代码范围：** `zzh_imx415.c` 的 mutex、power/stream state、runtime PM、probe/remove 和 rollback；不改 Host 恢复。

**实现内容：**

- `runtime_resume()` 调统一 `power_on()`，`runtime_suspend()` 调统一 `power_off()`；
- probe 完成后的 active/suspended 状态和 PM 引用对称；
- stream ON 获取 PM 引用，启动失败归还；stream OFF 停流后归还；
- `power_on`、`streaming` 状态在同一 mutex 规则下更新，状态只在硬件操作成功后提交；
- stop I2C 失败必须记录并返回，软件状态和 PM 回收策略写入注释/设计文档；
- remove 先注销 subdev/entity/controls，再同步终止使用并关闭硬件；
- 重复 open/close、STREAMON/OFF、异常进程退出不留下 clock、PM 或 streaming 泄漏。

**验证：** 先做 10 次，再做 100 次 STREAMON/OFF；每轮检查 ioctl、首帧、sequence、runtime status、clock、dmesg、fd/slab 趋势。加入断开用户进程、错误 control、启动寄存器写失败等可恢复测试。

**通过门槛：** 100 次最小循环 100% 成功；无 PM usage count 泄漏、无持续 clock、无 lockdep/KASAN 告警、异常退出后可立即 reopen。

**失败与回退：** 回到 P4 Image/DTB；先用最小循环复现，不进入 P6 长链路并发测试。

**产物：** PM/error patch、状态机说明、100-cycle 原始日志和资源差值。

### P6：DTS 与 RKCIF RAW 联调

**目的：** 打通并证明以下单路链路：

```text
zzh_imx415 -> D-PHY/DC-PHY -> CSI2 Host -> RKCIF -> RAW video node
```

**输入：** P5 稳定 Sensor 驱动、P1 目标 connector graph、实验 DTS、现有 RKCIF/VB2。

**代码范围：** 单路 Sensor DTS/endpoint 的必要修正、项目测试工具/脚本；原则上不改 RKCIF、CSI Host 和 PHY 驱动。

**任务与验证：**

1. 验证 Sensor/PHY 两端都是 `<1 2 3 4>`，remote-endpoint 双向闭合；
2. 保存 `media-ctl -p`，确认 graph 中只出现预期自研 Sensor 绑定；
3. 配置 subdev 为 3864x2192 SGBRG10，读取 3840x2160 selection；
4. 先采 300 帧，再采 3000 帧，最后进入长稳；记录实际 `G_FMT`、`bytesperline`、`sizeimage` 和 Buffer timestamp/sequence；
5. 分别统计 CSI CRC/ECC/SOT 与 RKCIF overflow/size/no-buffer，不能把不同层错误混为一个“丢帧”；
6. 校验 RAW 文件大小、Bayer 顺序、黑电平/亮度响应，并保存少量可复查样本及 SHA-256。

RKCIF compact RAW10 的理论 stride 只是核查参考：3840 和 3864 宽在当前 256-byte 对齐算法下都可能得到 4864 bytes/line；验收必须以目标 node 的 `G_FMT` 为准。

**通过门槛：** 300/3000 帧采集无不可解释 sequence gap、ERROR Buffer、size mismatch、CRC/ECC/SOT 或 overflow；长稳门槛由 P8 执行。

**失败与回退：** 同一硬件切回厂商 DTB 做 A/B。厂商正常而自研失败时回查 Sensor mode/format；两者均失败时回查硬件/Host，不先修改核心 DMA。

**产物：** 单路 DTS patch、运行 graph、RAW 样本、300/3000 帧 CSV/log、分层错误统计。

### P7：RKISP 成像闭环

**目的：** 在 RAW 稳定后打通：

```text
Sensor -> RKCIF/SDITF -> RKISP -> mainpath/selfpath -> NV12
```

**输入：** P6 RAW 基线、目标 SDITF/RKISP graph、厂商 IQ 文件、Rockchip VI/AIQ 接口要求。

**代码范围：** 自研 Sensor 为 RKISP/AIQ 所需的最小 Rockchip module-info/private ioctl、测试应用和实验 IQ 映射；只有证据要求时调整 DTS。不得整段无解释复制厂商 HDR/thunderboot/camera-sleep 接口。

**任务与验证：**

- 确认 `rockchip,camera-module-*` 元数据和 Sensor entity 名称如何参与 IQ 选择；
- 复用已存在的 `imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json` 作为黄金 IQ，不覆盖其内容；若自研命名需要映射，单独记录实验 symlink/配置；
- 配置 RKISP mainpath/selfpath NV12，确认实际输出为 3840x2160 有效图像；
- 检查 GBRG Bayer、颜色、方向、黑电平、曝光和模拟增益实际生效；
- 用原厂应用或项目内 V4L2 程序预览、抓图并测量稳定帧率；
- 区分 VICAP online/readback，记录是否存在 RAW 中间 DDR 写，避免错误计算带宽。

**通过门槛：** 连续 NV12 3840x2160@30 fps；颜色和 Bayer 正确；exposure/gain 可见且无 ISP/RKCIF 错误；用户态工具可重复启动/停止。

**失败与回退：** 回 P6 RAW 路径；切换厂商 DTB/驱动和同一 IQ 做 A/B，先分清 Sensor 数据、Bayer/crop、AIQ 命名或 ISP graph 问题。

**产物：** RKISP 兼容 patch（若需要）、IQ 映射说明、NV12 样本、graph/fmt/FPS/error 日志。

### P8：可靠性与厂商 A/B

**目的：** 用相同硬件和工作负载量化自研实现边界，并决定是否存在值得另立项的 Host 问题。

**输入：** P7 完整单路链路、P0 厂商基线、自动化采集工具。

**代码范围：** `projects/rk3588_camera_bsp/` 测试程序、脚本、日志格式；Sensor 修复只针对已复现问题。Host 修改必须另立问题、另提补丁。

**测试矩阵：**

| 测试 | 递进门槛 | 主要指标 |
|---|---|---|
| STREAMON/OFF | 100 -> 500 -> 1000 次 | 成功率、首帧、PM、fd/slab、错误日志 |
| 异常退出后重开 | SIGTERM/SIGKILL 后 100 次 | reopen/首帧成功率、资源回收 |
| Control 边界 | min/default/max/越界，循环变化 | 返回值、实际曝光/增益、VTS/帧率 |
| Buffer 不足/延迟 DQBUF | 逐步减少 queue depth | no-buffer/dummy、sequence、恢复能力 |
| RAW 长稳 | 2 h smoke -> 8 h gate | FPS、帧间隔、sequence、CSI/RKCIF 错误 |
| NV12 长稳 | 2 h smoke -> 8 h gate | 同上，加 RKISP 错误、图像正确性 |
| 资源和温度 | 全程采样 | MemAvailable/CMA/slab、CPU、温度、降频 |
| 厂商/自研 A/B | 同一模组、连接器、DTB graph、工具 | 功能、错误数、资源和稳定性差异 |

正常稳定段的初始目标是：零内核错误、零不可解释 sequence gap、零异常 Buffer，平均 FPS 在协商值的正负 0.5% 内。P50/P95/P99 帧间隔、资源和温度阈值先保存原始数据，再基于 P0/P8 数据冻结，不编造经验门槛。

**Host 问题的进入条件：** 只有错误能稳定复现、Sensor/PHY/CSI/RKCIF/RKISP 责任层证据充分、厂商与自研 A/B 已排除 Sensor 特有问题，才允许单独立项 RKCIF 可观测性、frame timeout、`vb2_queue_error()`、reset work 或真实 Host 缺陷修复。没有复现就记录“规定矩阵内未发现”，不制造修复成果。

**通过门槛：** 自研 RAW/NV12 完成 1000 次启停和 8 小时 gate；异常退出可恢复；无资源单调泄漏；A/B 差异有原始日志和解释。

**失败与回退：** 回到最近通过的 P6/P7 Image/DTB；切回 P0 厂商 DTB 重跑同一 seed。任何 Host 实验均需独立 Kconfig/commit 并可单补丁撤销。

**产物：** test harness、CSV/log、failure bundle、A/B 报告、最终验收报告；可选的 Host 问题单另立项。

## 6. 第一阶段预计修改点

本节描述 P2 实施时的预计修改，不表示本轮已经创建或修改这些内核文件。

| 文件 | 预计改动 | 约束 |
|---|---|---|
| `kernel/drivers/media/i2c/zzh_imx415.c` | 新建最小 I2C/power/Chip ID 驱动 | 不从厂商文件批量改名；先不加入 mode/controls/HDR |
| `kernel/drivers/media/i2c/Kconfig` | 新增 `VIDEO_ZZH_IMX415` | 依赖与 V4L2 Sensor 驱动一致；help 明确是本地独立实验驱动 |
| `kernel/drivers/media/i2c/Makefile` | `obj-$(CONFIG_VIDEO_ZZH_IMX415) += zzh_imx415.o` | 保留原 `VIDEO_IMX415 += imx415.o` |
| `kernel/arch/arm64/configs/alientek_rk3588_defconfig` | 增加 `CONFIG_VIDEO_ZZH_IMX415=y` | 不关闭 `CONFIG_VIDEO_IMX415`，便于同 Image A/B 和其他节点回退 |
| `kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-zzh-imx415.dtsi` | 新建单路 compatible 覆盖 | 只覆盖 P0 选定节点，不复制/改写整条 graph |
| `kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-2mipi1080x1920-2hdmi.dts` | 在 camera dtsi 后 include 实验覆盖 | P0 DTB 和去掉 include 的厂商路径可回退 |
| `projects/rk3588_camera_bsp/docs/` | 硬件契约、寄存器台账、阶段报告 | 自有文档集中在项目目录 |

建议的 P2 Kconfig 形态：

```text
config VIDEO_ZZH_IMX415
	tristate "Independent IMX415 sensor bring-up driver"
	depends on I2C && VIDEO_V4L2 && VIDEO_V4L2_SUBDEV_API
	depends on MEDIA_CAMERA_SUPPORT
	help
	  Local, independently implemented IMX415 bring-up driver for
	  the ATK-DLRK3588 camera BSP practice project.
```

P2 首次提交只接受以下功能：probe、资源获取、power、Chip ID、错误回滚、remove。固定 mode 和 `s_stream()` 留到 P3，pad/format/controls 留到 P4，runtime PM 完整化留到 P5。这样每个阶段的失败面和成果归属都可验证。

## 7. 证据映射与寄存器来源纪律

### 7.1 主要证据映射

| 结论/设计输入 | 直接证据 | 用途 | 仍需真机确认 |
|---|---|---|---|
| J18-J21 均引出四 lane 和独立控制 | `ATK-DLRK3588B V1.2(底板原理图).pdf` 第 12 页 | 连接器、lane、I2C/MCLK/GPIO 契约 | 实物实际插在哪一路、线序/信号完整性 |
| 模组使用 3.3 V 输入、本地 LDO、外部 MCLK、CSI_PDN | `ATK-MCIMX415 V1.4 原理图.pdf` 第 2 页 | power/PWDN/MCLK 设计 | GPIO 电平、上电延时、dummy regulator 日志 |
| 3864x2192、RAW10、4 lane、30 fps、37.125 MHz | `IMX415-AAQR-C_Datasheet_E19504(产品信息).pdf` pp.1-2、16、55、80 | mode、lane rate、INCK 和时序公式 | MCLK 实测、实际 FPS/CSI error |
| SHR0/VMAX 曝光约束 | 同一数据手册 pp.70-73 | EXPOSURE/VBLANK 动态约束 | control 边界和成像响应 |
| 四条完整厂商 graph | `kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-cameras.dtsi` | P1 graph 和单路实验 DTS 基线 | 动态 media graph、实际 node |
| 厂商固定 mode 和 V4L2 行为 | `kernel/drivers/media/i2c/imx415.c` | A/B、私有 ABI、未公开寄存器参考 | 自研实现不可直接宣称等价 |
| `0x311a -> 0xe0` Chip ID | 厂商 `imx415.c`，且源码带 TODO | P2 暂定参考 | 必须与资料和实物读值交叉确认 |
| Kconfig/Makefile 接入方式 | `kernel/drivers/media/i2c/Kconfig`、`Makefile` | 自研编译接入 | kernel build 结果 |
| RK3588 camera pipeline 和排障 | `Rockchip_Driver_Guide_VI_CN_v1.1.3.pdf`、`Rockchip_Trouble_Shooting_Linux5.10_Camera_CN.pdf` | P6/P7 graph、节点和错误归因 | 当前 BSP 运行差异 |
| 正确 DTB 部署与 NFS cmdline | `projects/scripts/deploy_net_boot.sh`、工作区启动报告 | 每阶段部署和启动证明 | 板端 `/proc/cmdline`、NFS mount |
| IQ 基线 | `nfs_rootfs/atk_dlrk3588/etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json` | P7 厂商 A/B | 自研 entity 命名和 AIQ 发现方式 |

### 7.2 寄存器来源台账格式

每个寄存器或连续寄存器组至少记录：

| 字段 | 含义 |
|---|---|
| Address/range | 16-bit 地址或范围 |
| Value | 第一版固定 mode 值 |
| Phase | power/global/mode/stream/control |
| Source | `[D]` 数据手册页码、`[V]` 厂商文件/表、`[M]` 真机读回 |
| Meaning | 已知位域、公式或“手册未公开” |
| Dependency | INCK、lane、bit depth、VMAX/HMAX、Bayer 等 |
| Validation | readback、FPS、CSI error、图像/control 响应 |

来源优先级为：数据手册明确项优先独立分析；数据手册未覆盖但厂商表需要的值保留 `[V]`；真机读回只能证明当前值，不能自动证明寄存器语义。提交评审时不接受没有来源的“大表”。

## 8. 简历与项目描述边界

### 8.1 完成相应阶段后可以诚实描述

- “在 Rockchip Linux 5.10 Camera Host 框架上，使用独立 compatible/Kconfig/驱动文件完成 IMX415 单路板级导入，并保留厂商驱动作为黄金 A/B 基线。”
- “基于原理图和 Sensor 资料实现 I2C、37.125 MHz MCLK、reset/PWDN、供电时序、Chip ID、错误回滚和 runtime PM。”
- “实现 3864x2192 RAW10、4-lane、30 fps 固定模式，以及 V4L2 subdev/pad/format/selection 和曝光、增益、blanking controls。”
- “打通 IMX415 -> D-PHY/CSI2 -> RKCIF RAW 与 RKCIF/SDITF -> RKISP -> NV12，并用 media graph、Buffer timestamp/sequence 和分层错误计数验证。”
- “完成 1000 次启停、异常退出恢复和 8 小时长稳，并与厂商实现在相同硬件/工作负载下 A/B。”

上述句子只能在对应阶段有代码、构建、板端日志和原始测试数据后使用。未完成时应写“设计/实施中”，不得用计划替代结果。

### 8.2 不可以描述

- “为原厂 SDK 新增了此前不存在的 IMX415 支持”；
- “完全从零推导全部 IMX415 寄存器”；
- “独立实现 RK3588 RKCIF/RKISP/CSI Host”；
- “实现真正的 3840x2160 Sensor mode”，除非后续资料和寄存器证据证明；
- “解决 RKCIF 丢帧/超时/恢复缺陷”，除非已有稳定复现、根因、补丁和 A/B；
- “产品化支持双摄/HDR/MPP/24 小时稳定”，除非这些扩展单独完成验收。

### 8.3 推荐的项目成果组织

最终材料按“硬件契约 -> 驱动状态机 -> mode/controls -> graph -> RAW/NV12 -> 错误路径 -> 稳定性数据 -> 厂商 A/B”组织。厂商已经实现相同 Sensor 不是需要隐藏的弱点，而是对照实验成立的前提；价值来自独立实现过程、证据纪律、异常处理和可重复验证，而不是虚构产品空白。

## 9. 下一步唯一建议任务

下一项工作是一个连续任务，而不是只做 P0 后再次停下来审计：

> 完成 P0 最小黄金基线后，立即进入 P1 硬件契约和 P2 最小自研 Sensor 驱动设计。

具体执行顺序：

1. 用当前厂商 Image/DTB 执行 `run nfsbootfdt`，在一轮会话内确认实物连接器、Sensor subdev、media device、RAW/RKISP node、fmt/selection 和 IQ；保存 P0 manifest。
2. 根据实际连接器，从 P1 表中冻结唯一一行，完成 I2C/MCLK/reset/PWDN/supply/lane/graph/power sequence 检查表。
3. 对 `0x311a -> 0xe0` 和固定 mode 寄存器建立第一版来源台账，先解决“数据手册事实、厂商参考值、真机读值”三者边界。
4. 设计 P2 的 `zzh_imx415.c` 状态和错误回滚图，列出每个资源获取/启用失败点的逆序释放动作。
5. 在独立提交中创建 P2 驱动骨架、Kconfig、Makefile、defconfig 和单路 DTS compatible 覆盖，只实现 power + Chip ID，不提前塞入 P3-P5 功能。
6. 只运行 `./build.sh kernel`，用部署脚本生成固定 1080x1920 的最新 NFS DTB，板端执行 `run nfsbootfdt`；通过 probe/negative test 后再进入 P3。

P0 的结束条件不是“资料已经足够”，而是“已确定自研 P2 的唯一实物目标并保存厂商回退证据”。P2 未通过 Chip ID 和错误回滚前，不进入固定 mode；P3 未产生稳定 MIPI 数据前，不进入 V4L2 完整化；P6 RAW 未稳定前，不进入 RKISP 和 Host 可靠性扩展。
