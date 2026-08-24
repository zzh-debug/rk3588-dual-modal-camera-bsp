# 基于 RK3588 的 IMX415 MIPI 摄像头 BSP 适配与视频采集链路可靠性优化

> 只读立项审计与可行性分析
>
> 审计日期：2026-07-31
>
> 工作区：`/rk3588_dev`
>
> 目标板：ATK-DLRK3588，Rockchip Linux 5.10.209
>
> 当前板级配置：`01_atk_dlrk3588_auto2mipi_2hdmi_defconfig`

## 1. 执行摘要

### 1.1 结论先行

**项目总体评级：B（值得做，但必须收敛为“现有链路审计 + Host 可观测性 + 可重复可靠性验证”，而不是“从零写 IMX415 驱动”）。**

1. **整体值得做。** 当前硬件、DTS、IMX415 驱动、RKCIF、RKISP 和 IQ 文件已经形成完整基础链路，适合在真实 BSP 上做可靠性工程；但这也意味着“把相机点亮”不是个人增量。
2. **它比“只写 IMX415 Sensor 驱动”更适合作为 RK3588 BSP 求职项目。** 原厂驱动已经提供四 lane、3864x2192、RAW10、30 fps 线性模式以及标准控制、runtime PM 和 Rockchip 私有接口。从零重写会重复原厂能力，且难以证明比现有实现更可靠。
3. **最值得做的三个 Host 增量：**
   - 以现有 procfs 为基线，补齐逐 stream 的 SOF/FE、DMA/VB2 完成、no-buffer、timeout、queue-error/recovery 结果及事件快照，避免重复已有统计；
   - 建立长稳、100/500/1000 次启停、少 QBUF、延迟 DQBUF、异常退出和系统压力的自动复现与量化框架；
   - 只对真机稳定复现的问题做一个最小 Host 修复；若确实出现“无帧但 queue 不报错”，再实现 L1 超时检测和 `vb2_queue_error()`。
4. **原厂已经实现的 Host 设想：** ping-pong DMA、dummy buffer、IRQ 错误分类、tasklet 延后 VB2 完成、流停止时异常 Buffer 回收、soft reset、reset work/watchdog 框架、CSI Host CRC/ECC/SOT 计数、RKCIF/RKISP procfs、dynamic debug、通用 ftrace/VB2 trace。当前配置未启用 RKCIF monitor，不能把“框架存在”说成“自动恢复已运行”。
5. **第一版没有依据修改 RKCIF 核心 DMA 算法。** 当前源码审计没有证明 DMA 地址轮换、锁或 Buffer 完成存在缺陷。直接改核心路径会扩大中断上下文和多设备共享路径的回归面。
6. **帧超时可以安全做到 L1；L2 为条件性；L3 不建议。** L1 必须做一次性触发、流代际检查以及 STREAMOFF/close/remove/suspend 的同步取消。L2 需先审计并实测厂商 reset work，特别是 work 生命周期。L3 透明恢复同时跨 Sensor、CSI Host、RKCIF、Media Pipeline、VB2 和 runtime PM，当前收益不足以覆盖竞态风险。
7. **能力等级：** 单摄 RAW 4K30 为 A（基础模式已存在，性能需实测）；RKISP NV12 为 B（图和 IQ 已存在，节点/格式/AIQ 需真机确认）；MPP H.265 为 C（软件能力存在，需完成 DMABUF、编码负载和温升闭环）；双 IMX415 4K30 为 C（物理接口明确，需第二模组并验证 ISP 回读、内存/CMA/IOMMU 和热约束）。
8. **推荐第一版边界：** 单路 IMX415；明确选择一个连接器；验证 RAW compact 与 RKISP NV12 两条路径；只增加低开销可观测性；完成可靠性测试矩阵；修复一个已复现 Host 问题；L1 仅在复现超时时进入。
9. **最可能失控的三个方向：** 在无复现证据时重写 RKCIF DMA/强行找 bug；直接实现 L3 内核透明恢复；把双摄、MPP 和日夜辅助硬件同时纳入第一版。
10. **如果只做一个 Host 增量：** 做“逐 stream、可关联 SOF/FE/DMA/VB2/错误路径的低开销观测与耐久测试数据闭环”。它既能支撑后续修复，又能展示 IRQ、DMA、VB2、锁和并发理解。

### 1.2 重要纠偏

- 当前 sensor mode 是 **3864x2192 RAW10 30 fps**；3840x2160 是 Sony 推荐记录窗口，也是驱动 `get_selection()` 暴露的居中裁剪范围。它不是当前驱动中另一个独立的 3840x2160 sensor register mode。RAW 节点是否实际裁到 3840x2160 必须从 Media Graph/selection 和 `G_FMT` 实测确认。
- 驱动名为 `imx415_linear_10bit_3864x2192_891M_regs`；Sony 表给出 891 Mbps/lane。V4L2 `LINK_FREQ` 菜单为 446 MHz，按 DDR 元数据换算为约 892 Mbps/lane。两者是命名/取整层面的差异，报告不把它们混成同一个精确值。
- RKCIF RAW10 在 RK3588 默认 compact，但 `ALIGN(width * 10 / 8, 256)` 对 3840 宽的结果是 **4864**，不是 4800。最终必须以目标 video node 返回的 `bytesperline`、`sizeimage` 和 private memory mode 为准。
- DMABUF 能避免 CPU 搬运，不会消除 ISP 写 DDR、MPP 读输入或压缩码流写出的内存流量；VICAP online 模式又可能避免 RAW 中间帧落 DDR，因此不能无条件把 RAW 写带宽和 NV12 写带宽相加。

## 2. 审计范围与证据等级

### 2.1 证据标记

- **[F] 已确认事实：** 可由当前源码、DTS、配置、PDF、已有生成物或既有真机报告直接证明。
- **[I] 合理推断：** 多项本地证据一致，但仍需编译或真机运行确认。
- **[T] 待确认事项：** 必须由当前开发板、额外模组、测量仪器或运行日志确认。

性能计算只代表理论数据量，不代表已经完成真机压测。凡未有真机原始数据支持的成功率、温度、延迟和错误数，本报告只定义采集方法和门槛。

### 2.2 主要证据

| 类别 | 直接证据 |
|---|---|
| 工作区与启动基线 | `/rk3588_dev/AGENTS.md`；`/rk3588_dev/docs/bringup/phase0_sdk_full_build_report.md`；`/rk3588_dev/docs/bringup/phase1_tftp_nfs_report.md`；`/rk3588_dev/U-Boot_DTB与TFTP_NFS网络启动机制总结.md`；`/rk3588_dev/TFTP_NFS_网络启动.md` |
| 部署与正确 DTB | `/rk3588_dev/projects/scripts/deploy_net_boot.sh`；`/rk3588_dev/kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-2mipi1080x1920-2hdmi.dts` |
| 摄像头 DTS | `/rk3588_dev/kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-cameras.dtsi` |
| Sensor 驱动 | `/rk3588_dev/kernel/drivers/media/i2c/imx415.c` |
| Host 驱动 | `/rk3588_dev/kernel/drivers/media/platform/rockchip/cif/{capture.c,dev.c,dev.h,hw.c,mipi-csi2.c,procfs.c,regs.h}`；`/rk3588_dev/kernel/drivers/media/platform/rockchip/isp/` |
| 配置 | `/rk3588_dev/kernel/arch/arm64/configs/alientek_rk3588_defconfig`；现有 `/rk3588_dev/kernel/.config` |
| 硬件 | `/rk3588_dev/docs/参考资料/02、开发板原理图/01、底板原理图/ATK-DLRK3588B V1.2(底板原理图).pdf` 第 12 页；`/rk3588_dev/docs/参考资料/02、开发板原理图/02、其它模块原理图/ATK-MCIMX415 V1.4 原理图.pdf` 第 2 页 |
| Sensor 数据手册 | `/rk3588_dev/docs/参考资料/07、硬件资料/06、模块资料/【正点原子】ATK-MCIMX415摄像头模块/2，IMX415参考资料/IMX415-AAQR-C_Datasheet_E19504(产品信息).pdf` |
| Rockchip 指南 | `/rk3588_dev/docs/cn/Common/ISP/ISP30/Rockchip_Driver_Guide_VI_CN_v1.1.3.pdf`；`/rk3588_dev/docs/cn/Linux/Camera/Rockchip_Trouble_Shooting_Linux5.10_Camera_CN.pdf`；`/rk3588_dev/docs/cn/Common/MPP/Rockchip_Developer_Guide_MPP_CN.pdf` |
| IQ | `/rk3588_dev/nfs_rootfs/atk_dlrk3588/etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json`（本次只读，未修改 RootFS） |

### 2.3 工作树保护记录

审计开始时保留如下既有状态；它们不是本报告创建的修改：

```text
kernel:
 M arch/arm64/configs/alientek_rk3588_defconfig
?? make.sh

u-boot:
 M include/configs/evb_rk3588.h
?? bl31_0x00040000.bin.gz
?? examples/standalone/rkspi
?? tee.bin.gz

buildroot: clean
device/rockchip: clean
```

本次没有执行构建、刷写、部署、RootFS 修改、清理或 reset。

## 3. 当前硬件和软件基线

### 3.1 启动与验证边界

- **[F]** 当前正确 Linux DTS 在 `rk3588-alientek-2mipi1080x1920-2hdmi.dts:9-11` 依次 include `rk3588-alientek.dtsi`、`rk3588-alientek-cameras.dtsi` 和 `rk3588-linux.dtsi`。
- **[F]** 日常开发应由部署脚本从上述 1080x1920 DTS 生成 `rk3588-alientek-nfs.dtb`，在 U-Boot 执行 `run nfsbootfdt`，同时 TFTP 最新 `Image + DTB`，Linux 再挂载 NFS RootFS。
- **[F]** 当前固件中 `nfsboot` 与 `nfsbootfdt` 仍是两条命令；本报告没有声称已经合并。原厂 eMMC 启动链继续作为回退。
- **[F]** 用户已确认原厂相机应用能预览和拍照。这证明至少一条基础链路在既有系统上出图，不等同于已经证明指定节点 RAW 4K30、长稳、DMABUF、MPP 或双摄达标。

### 3.2 内核能力基线

- **[F]** `alientek_rk3588_defconfig` 启用 `CONFIG_MEDIA_SUPPORT=y`、`CONFIG_VIDEO_ROCKCHIP_CIF=y`、`CONFIG_VIDEO_ROCKCHIP_ISP=y`、`CONFIG_VIDEO_ROCKCHIP_ISPP=y`、`CONFIG_VIDEO_IMX415=y`。
- **[F]** 现有 `.config` 为 `CONFIG_ROCKCHIP_CIF_WORKMODE_PINGPONG=y`、`CONFIG_ROCKCHIP_CIF_USE_DUMMY_BUF=y`、`# CONFIG_ROCKCHIP_CIF_USE_MONITOR is not set`、`# CONFIG_VIDEO_CAM_SLEEP_WAKEUP is not set`。
- **[F]** RK3588 使用 `vb2_cma_sg_memops`，说明 Host 队列按 scatter-gather/CMA 与 IOMMU 场景工作；可用 CMA/IOMMU 容量仍是板端指标。

## 4. 实际 IMX415 Media Graph

### 4.1 四套已启用连接

`rk3588-alientek-cameras.dtsi:7-10` 定义了四个 `ATK_CAMERA_*` 宏，因此四个分支均进入最终源码。底板原理图第 12 页显示 J18-J21 均引出 Clock + Data0..3，控制线独立；模组原理图第 2 页显示 3.3 V 输入、本地 2.8/1.8/1.2 V LDO、DNP 的 24 MHz 晶振、外部 MCLK 和直连 Sensor 的 `CSI_PDN`。

| 接口 | Sensor/I2C | MCLK、Reset、`power-gpios` | PHY -> CSI Host -> RKCIF | SDITF -> ISP | 模组元数据 |
|---|---|---|---|---|---|
| J18 / CSI1 | I2C7 `0x1a`，index 2 | `CLK_MIPI_CAMARAOUT_M1`；GPIO1_PD2 low；GPIO1_PA2 high | `csi2_dcphy0 -> mipi0_csi2 -> rkcif_mipi_lvds` | `rkcif_mipi_lvds_sditf -> rkisp1_vir0` | back / CMK-OT2022-PX1 / IR0147-50IRC-8M-F20 |
| J19 / CSI2 | I2C2 `0x1a`，index 3 | `CLK_MIPI_CAMARAOUT_M2`；GPIO1_PD3 low；GPIO1_PA4 high | `csi2_dcphy1 -> mipi1_csi2 -> rkcif_mipi_lvds1` | `rkcif_mipi_lvds1_sditf -> rkisp1_vir1` | 同上 |
| J20 / CSI3 | I2C3 `0x1a`，index 0 | `CLK_MIPI_CAMARAOUT_M3`；GPIO1_PB1 low；GPIO1_PA7 high | `csi2_dphy0 -> mipi2_csi2 -> rkcif_mipi_lvds2` | `rkcif_mipi_lvds2_sditf -> rkisp0_vir0` | 同上 |
| J21 / CSI4 | I2C4 `0x1a`，index 1 | `CLK_MIPI_CAMARAOUT_M4`；GPIO1_PB2 low；GPIO1_PB0 high | `csi2_dphy3 -> mipi4_csi2 -> rkcif_mipi_lvds4` | `rkcif_mipi_lvds4_sditf -> rkisp0_vir1` | 同上 |

- **[F]** 四个 Sensor 均为 `compatible = "sony,imx415"`，地址均为 `0x1a`，但位于四个独立 I2C adapter，不发生同一总线地址冲突。
- **[F]** Sensor endpoint 与 PHY endpoint 均使用 `data-lanes = <1 2 3 4>`，不是两 lane 配置。
- **[F]** 所列 sensor、PHY、CSI Host、RKCIF、SDITF、ISP、MMU 节点在各分支均被设为 `status = "okay"`。
- **[F]** DTS 使用 `clock-names = "xvclk"`，频率不在 DTS 固定，由当前 mode 的 `xvclk=37125000` 在驱动 power-on 时 `clk_set_rate()`。
- **[F]** DTS 没有 `avdd-supply`、`dvdd-supply`、`dovdd-supply`，驱动却请求这三路 regulator；模组本地 LDO 说明板级可由 3.3 V + GPIO 工作，但 dummy regulator/电源依赖应在板端 dmesg 核实。
- **[F]** DTS 的 `power-gpios` 实际连到模组 `CSI_PDN`，属于命名不理想的厂商适配；不能把它描述为直接控制三路 Sensor 电源轨。
- **[F]** DTS `pinctrl-names = "default"`，而驱动显式查找 `rockchip,camera_default` 与 `rockchip,camera_sleep`（`imx415.c:173-174,3136-3149`）。默认 pinctrl 可能已由 core 应用，但驱动显式 state 查找失败是可维护性风险，需以 probe log 与 MCLK 实测确认。

### 4.2 一条链路的 remote-endpoint 还原

以 J20 为例，双向 endpoint 是：

```text
imx415_c3_out
  <-> mipidphy0_in_ucam0
  csi2_dphy0 / csi2_dphy0_hw
  csidphy0_out
  <-> mipi2_csi2_input
  mipi2_csi2_output
  <-> cif_mipi2_in0
  rkcif_mipi_lvds2
  rkcif_mipi_lvds2_sditf:mipi_lvds2_sditf
  <-> rkisp0_vir0:isp0_vir0
```

对应源码为 `rk3588-alientek-cameras.dtsi:15-160`。其余三条连接按上表映射，均有完整的双向 `remote-endpoint`。

### 4.3 两条用途不同的路径

```text
RAW 调试路径：
IMX415 -> D-PHY/DC-PHY -> CSI2 Host -> RKCIF ping-pong DMA
       -> rkcif_mipi_lvds* 的 capture video_device -> RAW Buffer

ISP 成像路径：
IMX415 -> D-PHY/DC-PHY -> CSI2 Host -> RKCIF/VICAP -> 对应 SDITF
       -> rkisp{0,1}_vir{0,1} -> rkisp_mainpath/selfpath -> NV12 Buffer
```

- **[F]** RKCIF capture video device 名称由 `CIF_MIPI_ID*_VDEV_NAME` 和 `rkcif_register_stream_vdev()` 决定；RKISP 路径名称由 `SP_VDEV_NAME = "rkisp_selfpath"`、`MP_VDEV_NAME = "rkisp_mainpath"` 决定。
- **[T]** `/dev/videoX` 的数字是动态注册结果，必须先运行 `media-ctl -p` 和 `v4l2-ctl --list-devices`，不能假定 `/dev/video0`。
- **[F]** RAW capture 与 SDITF/ISP 是两种工作路径；online/readback 和 DMA enable 决策在 `rkcif_do_start_stream()` 中完成。不能把一帧 RAW DDR 写和 ISP NV12 DDR 写默认画成天然同时发生。
- **[F]** IQ 文件名精确匹配 DTS 的 sensor/module/lens 三元信息：`imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json`。

## 5. IMX415 驱动能力审计

### 5.1 现有能力

驱动位于 `/rk3588_dev/kernel/drivers/media/i2c/imx415.c`，版本宏对应 vendor driver `0.1.8`。当前仓库历史主要是 SDK 汇总导入，不能仅凭 Git 历史证明更早的上游谱系；Rockchip module ioctl、HDR、thunderboot/camera sleep 代码明确表明它是厂商 BSP 版本。

默认四 lane、无 `rockchip,camera-hdr-mode` 的首个 mode：

| 项目 | 当前值 | 证据 |
|---|---:|---|
| Bus format | `MEDIA_BUS_FMT_SGBRG10_1X10` | `supported_modes[0]`, `imx415.c:1084-1101` |
| 输出尺寸 | 3864x2192 | 同上；Sony p.2 有效/活动尺寸说明 |
| 帧率 | 30 fps | `10000/300000` |
| Lane | 4 | DTS endpoint；probe 根据 `num_data_lanes` 选择 `supported_modes` |
| HDR | `NO_HDR` | mode 与 DTS 默认 |
| XVCLK | 37.125 MHz | `IMX415_XVCLK_FREQ_37M` |
| VMAX | `0x08ca = 2250` | mode + register `0x3024/25` |
| HMAX | `0x044c = 1100` | register `0x3028/29` |
| Sensor lane rate | 891 Mbps/lane nominal | register table 名；Sony pp.55, 80 |
| V4L2 link-frequency | 446 MHz | `link_freq_items[1]` |
| V4L2 pixel-rate metadata | `446e6 * 2 * 4 / 10 = 356.8 MP/s` | `imx415_initialize_controls()` |
| Bayer | GBRG | `SGBRG10` |

Sony 数据手册还给出：p.1 支持 all-pixel、2/4 lane、RAW10/RAW12；p.2 给出 3864x2192 有效像素、3864x2176 活动像素以及推荐 3840x2160；p.16 给出 24/27/37.125/72/74.25 MHz INCK；p.55 的四 lane 表给出 30 fps、891 Mbps/lane、HMAX `0x44c`、VMAX `0x8ca`；pp.70-73 给出曝光公式与 SHR0/VMAX 约束；p.80 给出 891 Mbps/lane 对 37.125 MHz 的 INCK 设置。

### 5.2 V4L2 与电源路径

- **格式协商 [F]：** `enum_mbus_code()` 四 lane 枚举 RAW10/RAW12；`enum_frame_sizes()` 逐 mode 返回固定尺寸；`set_fmt()` 选择最接近且 code 匹配的 mode，并同步 HBLANK/VBLANK/link-frequency/pixel-rate；`get_fmt()` 返回当前/TRY format。
- **裁剪 [F]：** `get_selection()` 对 3864 输出暴露居中的 3840x2160 crop bounds。驱动没有形成一个新的 3840x2160 register mode；最终 crop 由下游节点与 media setup 决定。
- **启动 [F]：** `imx415_s_stream(1)` 持有 sensor mutex，获取 runtime PM，写 global/mode registers、controls 和 streaming register；失败会 `pm_runtime_put()`，且不置 `streaming=true`。
- **停止 [F]：** `imx415_s_stream(0)` 调 `__imx415_stop_stream()` 后 runtime put；当前调用者忽略 stop register write 返回值。这是错误可见性不足的候选改进，不是已复现故障。
- **供电 [F]：** `__imx415_power_on/off()` 管 pinctrl、`power_gpio`、reset、XVCLK、三路 regulator 和规定延时；`s_power()` 与 `runtime_resume/suspend()` 复用该路径。
- **remove [F]：** unregister subdev、清理 media entity/controls/mutex/camera sleep、disable runtime PM，并在必要时 power off。
- **系统睡眠 [F]：** 可疑的 `imx415->cur_mode != NO_HDR` 比较位于 `CONFIG_VIDEO_CAM_SLEEP_WAKEUP` 条件代码；当前配置禁用，不能把它列为当前运行 bug。

### 5.3 Controls 与约束

已实现：`LINK_FREQ`、`PIXEL_RATE`、只读 `HBLANK`、`VBLANK`、`EXPOSURE`、`ANALOGUE_GAIN`、`HFLIP`、`VFLIP`。改变 VBLANK 会把线性曝光上限动态改为 `height + vblank - 8`；线性曝光写 SHR0=`cur_vts - exposure`。Sony p.70 的公式为：

```text
Integration time = frame period - SHR0 * 1H + Toffset
```

Sony p.71 要求正常曝光 SHR0 位于 8 到 frame-lines-4；当前驱动给用户的 exposure 范围为 4 到 VTS-8，再转换为 SHR0，工程上需用 control 边界测试验证两者语义一致。

未实现标准 Test Pattern control；没有创建 Digital Gain V4L2 control。文件顶部兼容定义 `V4L2_CID_DIGITAL_GAIN` 不代表已经注册/处理该 control。

### 5.4 是否重写

- **结论 [F/I]：** 无需从零实现正式驱动。固定 4K30 RAW10 的核心寄存器、格式协商、controls、power/runtime PM、HDR 私有接口均已存在。
- **适合的真实增量：** probe/dmesg 证实 regulator 与 pinctrl 行为；补 stop error 传播前先复现；对 controls 边界、启停和 PM 做测试；修复真机暴露的具体问题。
- **教学版隔离：** 若为学习另写简化 Sensor 驱动，应放在独立实验分支/独立模块名与 compatible，禁止与正式 DTS 同时 bind；不计入正式项目的生产能力成果。

## 6. 4K30 时序和带宽核查

### 6.1 时钟与帧率

必须区分五个值：

1. Sensor 外部 `INCK/XVCLK = 37.125 MHz`；
2. 1H 计算采用的内部时序基准 `74.25 MHz`；
3. `HMAX = 1100`；
4. CSI DDR bit clock 对应 V4L2 link-frequency 约 `446 MHz`；
5. lane rate 为 Sony nominal `891 Mbps`，V4L2 元数据换算约 `892 Mbps`。

```text
1H = HMAX / 74.25 MHz
   = 1100 / 74,250,000
   = 14.8148 us

frame = VMAX * 1H
      = 2250 * 14.8148 us
      = 33.3333 ms

fps = 1 / frame = 30 fps
```

驱动的 `hts_def = 0x44c * 4 * 2 = 8800` 是其 V4L2 HBLANK 元数据表示，不能用它替换 Sony HMAX/内部时序公式。板端应同时记录 subdev `HBLANK/PIXEL_RATE` 和实际 Buffer 时间戳，确认用户空间看到的控制元数据是否满足预期。

### 6.2 MIPI 有效负载

推荐裁剪有效像素：

```text
total payload = 3840 * 2160 * 10 * 30
              = 2.48832 Gbit/s
per lane      = 2.48832 / 4
              = 622.08 Mbit/s/lane
```

完整 driver output：

```text
total payload = 3864 * 2192 * 10 * 30
              = 2.5409664 Gbit/s
per lane      = 635.2416 Mbit/s/lane
```

相对 891 Mbps/lane，完整有效像素占用约 `635.2416 / 891 = 71.30%`，名义差额约 28.70%。这是“lane gross rate 减 active RAW payload”的差额，尚包含 CSI-2 包头/包尾、行帧 blanking 和实现余量，不能称为可任意使用的净带宽。单路无明显理论 CSI 瓶颈，但 CRC/ECC/SOT 和实际帧率必须实测。

### 6.3 RAW DMA 写带宽

`rkcif_cal_csi_crop_width_vwidth()` 对 compact RAW 执行 `ALIGN(raw_width * raw_bpp / 8, 256)`。RK3588 stream 初始化默认 `is_compact=true`；private `RKCIF_CMD_SET_CSI_MEMORY_MODE` 可切到 16-bit word mode。

```text
3840 compact RAW10:
bytesperline = ALIGN(3840 * 10 / 8, 256) = ALIGN(4800, 256) = 4864 B
frame        = 4864 * 2160 = 10,506,240 B
write rate   = 10,506,240 * 30 = 315.1872 MB/s

3864x2192 compact RAW10:
bytesperline = ALIGN(3864 * 10 / 8, 256) = ALIGN(4830, 256) = 4864 B
frame        = 4864 * 2192 = 10,661,888 B
write rate   = 319.85664 MB/s
```

16-bit word storage 上界：

```text
3840 * 2160 * 2 * 30 = 497.664 MB/s
3864 * 2192 * 2 * 30 = 508.19328 MB/s
```

**[T]** 以上必须由目标 node 的 `G_FMT`、`bytesperline`、`sizeimage`、procfs compact 字段和 private memory mode 交叉确认；不能只看 V4L2 fourcc 推断内存布局。

### 6.4 ISP、MPP 与双摄

```text
NV12 3840x2160 write = 3840 * 2160 * 1.5 * 30
                      = 373.248 MB/s
```

- **[F]** VI 指南 pp.13-15 描述 RK3588 的 VICAP、两个 ISP30、virtual ISP、online/readback；p.14 说明两路复用的最大分辨率 3840x2160，p.57 又明确警告单 ISP 双摄硬件直接能力以 1080p 为界，更高分辨率需要一帧多次回读并消耗额外带宽。双 4K30 不能只按 MIPI 带宽判断。
- **[I]** online 模式下 RAW 可不先写完整中间帧到 DDR；readback 模式则会增加 VICAP RAW write + ISP read + ISP output write。实际模式可从 RKCIF/RKISP procfs 与日志确认。
- **[F]** MPP 指南 3.4.1 说明 `encode_put_frame` 的输入生命周期会阻塞到硬件用完图像；同时本地 `rk_mpi.h` 将 put/get 归类为分离的 async data API。工程上应以实际调用延迟和配置为准，不把“接口分离”误写成“调用必不阻塞”。
- **[F]** MPP 偏好 dmabuf/ion/drm 输入，可避免 CPU copy；简单 `encode_get_packet` 仍会复制输出码流，如需输出码流零拷贝要用 `poll/dequeue/enqueue + MppTask`（MPP 指南 3.4.2）。
- **[I]** MPP 增量内存流量至少包含 NV12 输入读取约 373.248 MB/s，压缩输出若码率为 `R Mbit/s` 则约 `R/8 MB/s`，另有参考帧和内部访问；本地资料不足以给出一个可信固定总带宽，必须用 devfreq/DDR/CPU/温度实测。
- **[T]** 双摄需要分别记录两路实际 compact/word mode、online/readback、ISP 归属、CMA/IOMMU、帧间隔、错误和温度。优先选择跨 ISP 的 J18(ISP1) + J20(ISP0)，但 VICAP、内存和部分 IRQ/电源域仍共享，不能声称完全隔离。

## 7. RKCIF/VB2/IRQ/DMA 真实调用链

### 7.1 Video node、文件操作与 VB2 queue

`rkcif_register_stream_vdev()`（`capture.c:8730`）注册 RKCIF capture node：

| 对象 | 当前实现 |
|---|---|
| `video_device` | `stream->vnode.vdev`，名称来自 `CIF_MIPI_ID*_VDEV_NAME` |
| `v4l2_file_operations` | `rkcif_fops`：`rkcif_fh_open`、`rkcif_fh_release`、`video_ioctl2`、`vb2_fop_poll`、`vb2_fop_mmap` |
| `v4l2_ioctl_ops` | `rkcif_v4l2_ioctl_ops`，标准 VB2 `REQBUFS/QUERYBUF/QBUF/DQBUF/STREAMON/STREAMOFF/EXPBUF` 路径 |
| queue | `stream->vnode.buf_queue`，`VIDEO_CAPTURE_MPLANE` |
| `io_modes` | `VB2_MMAP | VB2_DMABUF`；RKCIF 主 capture node 不支持 USERPTR |
| `mem_ops` | RK3588 `hw_dev->mem_ops = &vb2_cma_sg_memops` |
| queue/process lock | `stream->vnode.vlock` mutex，同时赋给 `video_device.lock`/VB2 queue lock |
| timestamp | `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`，完成 Buffer 使用保存的 SOF timestamp |

VB2 ops 为 `rkcif_queue_setup`、`rkcif_buf_queue`、`rkcif_stop_streaming`、`rkcif_start_streaming`、标准 wait prepare/finish；没有自定义 `.buf_prepare`。`queue_setup` 负责 plane 数量/size，不能在报告中虚构一个不存在的 prepare 回调。

Buffer 私有结构是 `struct rkcif_buffer`，包含 `vb2_v4l2_buffer vb`、链表节点、DMA 地址/时间戳/stream id 等。等待队列是 `stream->buf_head`；硬件活动 Buffer 是 `curr_buf`/`next_buf`；延后完成使用 `vb_done_list`。

`rkcif_buf_queue()` 的转换为：

```text
vb2_buffer
 -> to_vb2_v4l2_buffer()
 -> container_of(..., struct rkcif_buffer, vb)
 -> vb2_dma_sg_plane_desc() / mem_ops helper
 -> sg DMA/IOVA 写入 rkcif_buffer 地址字段
 -> vbq_lock 下加入 stream->buf_head
 -> 若之前因缺 Buffer 停 DMA，则安装真实 Buffer 并恢复 DMA
```

### 7.2 STREAMON 启动调用链

```text
VIDIOC_STREAMON
 -> video_ioctl2()
 -> vb2_ioctl_streamon()
 -> VB2 调 q->ops->start_streaming
 -> rkcif_start_streaming()                         capture.c:7377
 -> rkcif_do_start_stream()                         capture.c:7125
    [dev->stream_lock 序列化]
    -> rkcif_update_sensor_info()/HDR/interval
    -> 必要时创建 dummy buffer
    -> dev->pipe.open()
       -> rkcif_pipeline_open()                     dev.c:1300
       -> 上游 subdev power/open 与 runtime PM
    -> rkcif_csi_stream_start()
       -> 安装 curr/next Buffer，配置 DMA 地址/中断/stream
    -> media_pipeline_start()
    -> dev->pipe.set_stream(true)
       -> rkcif_pipeline_set_stream()               dev.c:1449
       -> 遍历已准备的 upstream subdev
          -> CSI Host csi2_s_stream(1)              mipi-csi2.c:311
             -> csi2_start()                        mipi-csi2.c:237
             -> reset/clock/IRQ mask/Host config/PHY stream
          -> Sensor imx415_s_stream(1)              imx415.c:2448
    -> 设置 stream state、启用 tasklet、启动 monitor event
```

默认 MIPI 流程先准备 Host/DMA，再放行上游 Sensor，避免首帧写入无 Buffer。`rkcif_pipeline_set_stream()` 还会根据 `RKMODULE_GET_START_STREAM_SEQ` 调整厂商要求的顺序，因此精确 subdev 次序应以目标 sensor 的 ioctl 返回和动态 trace 为准。任一步失败时，start unwind 会停止已启部分、disable tasklet，把 current/next 和队列 Buffer 复位，并以 `VB2_BUF_STATE_QUEUED` 交回 VB2，让 VB2 统一取消；这不同于正常 STREAMOFF 返回 ERROR。

### 7.3 IRQ 到 DQBUF 完成调用链

RK3588 RKCIF 硬件节点通过 `devm_request_irq(..., rkcif_irq_handler, ...)` 注册共享、非线程化硬 IRQ：

```text
RKCIF hard IRQ
 -> rkcif_irq_handler()                             hw.c:1278
    -> rkcif_irq_global()
    -> 写回 CIF_REG_GLB_INTST 清 GLB 触发位
    -> cif_dev->isr_hdl()
       -> dev.c:rkcif_irq_handler()
       -> rkcif_irq_pingpong_v1()                   capture.c:12666
          -> 读 CIF_REG_MIPI_LVDS_INTSTAT 和 frame size/line 状态
          -> 把 intstat 原值写回清已触发位
          -> 分类 SIZE_ERR / FIFO_OVERFLOW / BANDWIDTH_LACK
          -> 对 FRAME_END_IDn 选择 stream 和 ping-pong phase
          -> rkcif_update_stream()                  capture.c:10398
             -> 取 curr_buf/next_buf
             -> rkcif_assign_new_buffer_pingpong()
             -> rkcif_buf_done_prepare()            capture.c:9931
                -> vb.timestamp = readout.fs_timestamp
                -> vb.sequence = frame_idx - 1
                -> active_buf->fe_timestamp = now
                -> rkcif_vb_done_tasklet()
 -> tasklet rkcif_tasklet_handle()                  capture.c:8688
    -> 搬出 vb_done_list
    -> rkcif_vb_done_oneframe()
    -> vb2_buffer_done(..., VB2_BUF_STATE_DONE)      capture.c:8659
 -> 唤醒 VB2 wait/poll
 -> vb2_ioctl_dqbuf() 返回用户空间
```

- **[F]** 正常 RKCIF hard IRQ 完成路径没有 I2C 或其他可睡眠 Sensor 操作；日志/错误、Sensor quick-stream 和 reset 分别通过 work、delayed_work 或其他路径处理。
- **[F]** Buffer 对外 timestamp 是保存的 frame-start timestamp，sequence 在 SOF/frame index 基础上形成；`fe_timestamp` 单独记录完成时刻。两者可用于计算 sensor 间隔与 Host readout/DMA 延迟。
- **[F]** CSI Host 的 CRC/ECC/SOT/PHY 错误来自 `mipi-csi2.c` 的 Host IRQ；RKCIF 本地 IRQ负责 frame end、size、FIFO overflow、bandwidth-lack 和 DMA accounting。模块归属不可混写。
- **[F]** SOF 由 CSI Host/readout 侧维护 frame-start 计数和时间；RKCIF `CSI_FRAME_END_IDn` 及 ping-pong phase 表示对应 DMA 帧已经到达可完成阶段，驱动没有再暴露一个独立于该 FE 路径的 VB2 “DMA Done”回调；size/overflow/bandwidth-lack 是独立状态位。验收时应分别记录 Host SOF、RKCIF FE 和 tasklet/VB2 done，而不是把三者称为同一个中断。

### 7.4 停止、close 与 Buffer 饥饿

- `rkcif_do_stop_stream()` 在 `dev->stream_lock` 下协调 DMA stop waitqueue/completion、上游 pipeline stop、media pipeline/PM put、tasklet 和 soft reset。
- STREAMOFF 会把 `curr_buf`、`next_buf`、`buf_head` 和尚未完成列表中的 Buffer 以 `VB2_BUF_STATE_ERROR` 归还，防止用户空间永久等待。
- close 经 `rkcif_fh_release()`/`vb2_fop_release()` 触发 VB2 release，再执行 pipeline/runtime PM put。
- system sleep 通过 `dev.c:rkcif_sleep_suspend/resume()` 进入 `rkcif_stream_suspend/resume()`；runtime suspend/resume 以 `power_cnt` 和 `hw_dev->dev_lock` 协调共享硬件 PM，RK3588 runtime resume 后执行 `rkcif_do_soft_reset()`。正常 streaming 的 pipeline/PM 引用应阻止不合时宜的 runtime suspend，仍需用循环启停和 suspend 测试验证引用计数平衡。
- dummy buffer 已启用；`lack_buf_cnt` 跟踪无替换 Buffer。持续缺 Buffer 后 IRQ 路径可停 DMA；随后 QBUF 能安装真实 Buffer 并重新使能。这意味着“no-buffer”已有保护，但仍需要测试 sequence、恢复时间和错误计数。

## 8. 中断、Buffer 和锁模型

### 8.1 上下文分工

| 上下文 | 实际职责 |
|---|---|
| hard IRQ | 读/清状态、错误计数、ping-pong 地址轮换、准备完成 Buffer、调度 tasklet/work；不得睡眠 |
| tasklet | 从 `vb_done_list` 批量调用 `vb2_buffer_done()`，缩短 hard IRQ 临界工作 |
| work | reset、错误打印、Sensor quick stream、tools 等可睡眠工作 |
| delayed_work | bandwidth-lack 中断延时重开等 |
| timer | RKCIF reset watchdog；当前 monitor mode 为 IDLE |
| process context | ioctl、VB2 queue、STREAMON/OFF、close、control、runtime PM |

### 8.2 共享状态和锁

| 锁/同步原语 | 保护对象与要求 |
|---|---|
| `vnode.vlock` mutex | video ioctl 与 VB2 queue 的进程级序列化 |
| `cif_dev->stream_lock` mutex | start/stop/reset、跨 stream pipeline 状态；reset work 的外层锁 |
| `stream->vbq_lock` spinlock | `buf_head`、`vb_done_list`、curr/next 等 IRQ/进程共享 Buffer 状态 |
| `stream->fps_lock` spinlock | frame timestamp/FPS 与部分 stop/quick-stream 标志 |
| `hw_dev->dev_lock` mutex | 多个逻辑 CIF 共享硬件及 runtime PM |
| CSI2 `lock` mutex | Host subdev stream count/config |
| atomic | pipeline power/stream count、Buffer/streamoff/power count |
| waitqueue/completion | DMA 停止、单帧/quick stream 和 stop complete |

现有主序是由进程路径先取得 `vlock`，进入驱动后再使用 `stream_lock`，短临界区内使用 `vbq_lock/fps_lock`；IRQ 不取得 mutex。任何新 timeout work 都不得在持有 `vbq_lock` 时调用 `vb2_queue_error()`、stop pipeline 或等待 completion，也不得制造 `vlock <-> stream_lock` 的反向等待。

### 8.3 静态审计风险结论

- **未证明** 当前存在重复 Buffer 完成、UAF、锁反转或 DMA 地址轮换错误；它们应成为 KASAN/lockdep/启停压测的观察项，而不是预设结论。
- `rkcif_plat_remove()` 可见 `del_timer_sync()`，`rkcif_plat_uninit()` 会 unregister vdev/tasklet，但在所审路径未见对 `reset_work`、`sensor_work`、`err_state_work`、`work_deal_err` 的统一 `cancel_*_sync()`。由于常规产品多为 built-in 且 remove 场景少，这只能列为“unbind/remove 与异步 work 生命周期审计候选”，不能在没有可卸载/解绑复现时宣称 UAF。
- STREAMOFF 会设置 `reset_work_cancel=true`，reset work 又在 `stream_lock` 下复查；该逻辑降低主动 stop 与 reset 竞争，但不替代 work object 在 remove 前的同步销毁。
- `imx415_s_stream(0)` 忽略 stop I2C 返回值，会降低故障可见性；修复前应先通过注入/总线错误证明它影响用户可见语义。

## 9. 原厂统计、调试和恢复能力

### 9.1 已有可观测性

`cif/procfs.c` 已为每个 CIF device 创建 `/proc/<device-name>`，包含：输入/输出格式、compact mode、frame count、即时 frame interval/FPS、readout timing、各 stream DMA-end、overflow、bandwidth-lack、size-error、inactive buffer、IRQ 执行时间、DMA enable 和驱动 Buffer 数量等。

另外已有：

- CSI Host `mipi-csi2.c` 的 SOT/SOT sync/CRC/ECC/PHY error list 与 IRQ；
- RKCIF `v4l2_dbg/dev_dbg`、模块 debug level、dynamic debug；
- module parameters：RKCIF `debug`、只读 `version`、维护用 `clr_unready_dev`，CSI Host `debug_csi2`；
- platform device sysfs attribute group：`compact_test`、`wait_line`、`is_use_dummybuf`、`is_high_align`、`fps`、`rdbk_debug`、各 scale BLC、interlace/debug 等。多数字段是厂商调试开关，不应在不知道副作用时作为稳定性脚本的常规写接口；
- 通用 function_graph/ftrace、IRQ/workqueue/VB2 trace events；
- RKISP 自有 procfs、frame/readback/error/工作状态；
- dmesg 中的 probe、format、stream、error 和 reset 信息。

对 `cif/` 和 trace event 的源码检索没有发现 RKCIF 专用 debugfs 实现或自定义 RKCIF tracepoint。`include/trace/events/vb2.h` 等通用 tracepoint 可先复用。

**真正缺口：** 可关联同一 stream 的持久 timeout/queue_error/recovery attempt/success/failure 计数、触发前后快照、SOF->FE->VB2 的分位延迟、测试 run id/流代际，以及异常退出后的 reopen 结果。新增 debugfs/procfs 不应照抄已有 IRQ/格式字段。

### 9.2 错误归属

| 错误/事件 | 所属层 | 证据/处理点 |
|---|---|---|
| Sensor I2C、曝光/增益、stream standby | IMX415 | `imx415.c` |
| SOT/SOT sync、CRC、ECC、PHY | CSI2 Host/PHY | `mipi-csi2.c:rk_csirx_irq1_handler/rk_csirx_irq2_handler` |
| frame size、FIFO overflow、bandwidth lack、frame end | RKCIF/VICAP DMA | `rkcif_irq_pingpong_v1()` 与 RKCIF registers |
| ISP input/frame/processing errors | RKISP | `drivers/media/platform/rockchip/isp/` registers/procfs |
| no-buffer/queue completion | RKCIF + VB2 | `rkcif_buf_queue()`、IRQ、tasklet、VB2 |

`rk_csirx_irq1_handler()` 对 `CSIHOST_ERR1_ERR_ECC2` 增加 `err_list[RK_CSI2_ERR_CRC]`，看起来可能把 ECC2 合并进 CRC。它是一个明确的源码审计疑点，但只有在能触发/读取 ECC2 并证明统计语义错误后，才可作为“真实 Host bug”修复。

### 9.3 原厂恢复

- `rkcif_init_reset_monitor()` 初始化 watchdog timer 与 `reset_work`；启用 `CONFIG_ROCKCHIP_CIF_USE_MONITOR` 时使用 Kconfig 周期/模式/错误阈值，否则设为 `RKCIF_MONITOR_MODE_IDLE`。
- `rkcif_reset_watchdog_timer_handler()` 检测 Buffer 心跳/CSI 错误并调度 `rkcif_reset_work()`；`rkcif_do_reset_work()` 持 `stream_lock`，停止活动 stream/上游、soft/CRU reset、重新配置并恢复原活动 stream。
- private `RKCIF_CMD_SET_RESET` 可直接进入 `rkcif_do_reset_work()`；RK3588 runtime resume 还会执行 `rkcif_do_soft_reset()`。
- bandwidth-lack 过多时会临时 mask 对应中断并以 delayed_work 延时恢复，避免日志/中断风暴。

**当前事实：monitor 未启用。** 所以 watchdog 自动 L2 不是当前运行能力。private reset/soft reset 路径存在，也不等于它能在任意 VB2/close/suspend 时刻安全透明恢复。

### 9.4 分级超时与恢复评估

#### L1：检测并上报（B，推荐条件性实施）

最小设计：每个 stream 保存流代际、最后一次有效 FE/VB2 completion timestamp、一次性 timeout 标志和 delayed_work。成功 STREAMON 后启动；正常 completion 在 `fps_lock` 或单独 spinlock 下更新时间；work 只在相同代际且仍 STREAMING 时触发，先快照统计，再在不持 `vbq_lock`/`fps_lock` 时调用 `vb2_queue_error()`。

生命周期要求：

1. STREAMOFF/close：先使该代际失效，再同步取消 delayed_work；不要在持有 work 也要获取的 mutex 时 `cancel_delayed_work_sync()`。
2. suspend/remove：在 unregister vdev、释放 stream/tasklet 或断时钟前同步取消；系统恢复不沿用旧 timestamp。
3. Buffer：timeout work 不自行拼装完成顺序；用户空间收到 poll/DQBUF 错误后 STREAMOFF，复用现有 stop path 将 queued/current/next/pending-done 统一返回 ERROR。
4. 用户空间：预期 poll 出现 error，后续 queue ioctl/DQBUF 返回错误（精确 errno 以 VB2 真机为准）；测试程序执行 STREAMOFF、close、重新 open/配置/排队。
5. 阈值：以协商 frame interval 为基础，例如 `max(3*frame_period, 200 ms)` 的候选值；最终阈值必须经过低帧率/VBLANK/首帧延迟测试，不能硬编码 4K30 常数。

#### L2：复用厂商 reset（C）

可复用入口是 watchdog -> `rkcif_reset_work()` -> `rkcif_do_reset_work()` 或受控 private reset。它已经有 `stream_lock`、活动 stream 收集、上游 quick-stream/s_stream、DMA/soft reset 和 restart 逻辑。

实施前必须补证：当前 mode 下 Buffer ownership 在 reset 前后是否保持；reset 与 VB2 queue_error/STREAMOFF 谁拥有最终 Buffer；reset work 的 cancel/flush 覆盖 remove/suspend；多 logical CIF/ISP online/readback 的锁要求；失败是否让 queue 明确进入 error。未通过这些测试前，不开启 `CONFIG_ROCKCHIP_CIF_USE_MONITOR` 作为产品默认。

#### L3：内核透明恢复（D，暂不实施）

透明地停止 Sensor、mask Host IRQ、停 DMA、回收 Buffer、清状态、复位、重配、重启，必须同时与主动 STREAMOFF、close/kill、suspend、remove、runtime PM、control I2C、仍在执行的 IRQ/tasklet 和 VB2 cancel 协调。当前没有证明 L1/L2 不足，也没有正式的跨层 transaction/rollback 状态机。第一版实现 L3 很容易产生重复完成、旧 DMA 写入、PM ref 泄漏或 deadlock，求职展示价值反而低于一个经过验证的最小修复。

### 9.5 安全故障注入

| 方法 | 结论 | 约束 |
|---|---|---|
| 少 QBUF / 延迟 DQBUF | 安全、第一版必做 | 逐步减少有效 queued Buffer，观察 dummy/lack/DMA resume；不要耗尽整机内存 |
| 快速 STREAMON/OFF 100/500/1000 次 | 安全 | 每轮检查返回值、sequence、fd/memory、dmesg |
| 采集中 SIGTERM/SIGKILL/异常退出 | 安全 | 随后立即 reopen；不同时 kill 系统关键 AIQ 服务，先明确 graph owner |
| CPU/内存/I/O 压力 | 条件安全 | 设上限、监控温度/降频/NFS；不要用内存 OOM 作为默认方法 |
| 冻结 heartbeat / force timeout once | 适合 debug Kconfig | debugfs 仅在调试 Kconfig 下创建，one-shot、默认关闭、不能跳过 IRQ status 清除 |
| 直接调度 reset work | L2 阶段条件性 | 先检查 streaming/generation，禁止生产配置暴露无权限接口 |
| 控制 Sensor quick-stream off | 后期条件性 | 确认没有 AIQ/其他 owner，确保恢复/close；不用于首轮基线 |
| 软件伪造 CRC/ECC/overflow | 当前无安全接口 | 不直接跳过/延迟 IRQ clear，避免中断风暴；没有寄存器仿真依据则标为不可用 |

## 10. 各项目标可行性矩阵

评级：A=第一版必做且证据明确；B=可行但需修改/真机验证；C=条件性；D=主线不建议；E=原厂已完整实现，重复开发价值低。

| # | 目标/评级 | 证据 | 工作量/技术价值 | 主要风险 | 验证方法 | 回退 | 简历 |
|---:|---|---|---|---|---|---|---|
| 1 | 黄金基线 **A** | 已有 eMMC、TFTP/NFS、原厂出图及 phase 报告 | 2-3 天；建立所有增量的对照组 | 节点/版本/启动 DTB 记录不全 | hash、cmdline、graph、格式、10 min RAW/NV12、dmesg/proc | eMMC + 已知 Image/DTB | 是，作为工程方法 |
| 2 | 硬件/DTS 审计 **A** | 四条四 lane graph、原理图、IQ 均明确 | 2-3 天；BSP 核心能力 | 把四个 enabled 节点误当四路已实测 | connector-by-connector probe/graph/MCLK/GPIO | 只保留原 DTS，不改 | 是 |
| 3 | 固定 4K30 mode 适配 **E** | 原厂已有 3864x2192 RAW10 30 fps/4 lane | 1-2 天验证；重写价值低 | 把 3840 crop 误当 sensor mode | subdev fmt/selection、30 fps、register/log | 完全使用原 mode | 只能写“验证/审计”，不能写“实现” |
| 4 | 重写 Sensor 驱动 **D** | 当前驱动已有 mode/controls/PM/HDR | 3-6 周；教学价值高、产品价值低 | 功能退化、私有 ABI/AIQ 不兼容 | 仅独立实验 compatible/A-B | 不 bind 教学驱动 | 不建议作为主成果 |
| 5 | 还原调用链 **A** | 源码符号和路径完整 | 3-5 天；能展示 V4L2/VB2/IRQ/DMA | 只写通用理论 | ftrace + source symbol + timestamp 对照 | 文档无运行影响 | 是，强 |
| 6 | 逐 stream 统计 **A** | procfs 已有基础，但缺 timeout/recovery/关联快照 | 1-2 周；最佳 Host 增量 | IRQ 热路径开销、重复统计 | 开关前 FPS/IRQ latency 对比；计数与用户 sequence 对账 | Kconfig/静态分支关闭 | 是，强 |
| 7 | debugfs **B** | RKCIF 当前无专用 debugfs；procfs 已存在 | 3-5 天；适合 snapshot/injection | ABI 滥用、卸载生命周期 | debug Kconfig、读并发、unbind/streamoff | 关闭 Kconfig/删除节点 | 是，但强调调试用途 |
| 8 | 自定义 tracepoint **C** | 通用 VB2/IRQ/workqueue/ftrace 可先用 | 3-5 天；精确事件关联 | 维护成本、热路径开销 | 先证明 generic trace 不足，再 benchmark | static key 关闭/撤销 | 条件性 |
| 9 | 长时间稳定性 **A** | 单摄基础链路已出图 | 1-2 周执行；可靠性核心 | NFS/应用问题误归 Host | 2h 冒烟、8h 门槛、24h 扩展；全指标归档 | 回原 Image/DTB 重跑 | 是，强 |
| 10 | 100/500/1000 启停 **A** | VB2 start/stop/recovery 路径明确 | 3-5 天；覆盖 PM/锁/回收 | 测试程序泄漏造成假阳性 | 阶段递进，成功率、fd/slab/dmesg | 失败即回退到上一级次数 | 是 |
| 11 | Buffer 饥饿 **A** | dummy/lack/停 DMA/再 QBUF 已实现 | 2-3 天；验证真实异常路径 | 用户态策略不真正造成 starvation | 逐减 queued buffers、延迟 DQBUF、看 lack/sequence | 恢复正常 queue depth/STREAMOFF | 是 |
| 12 | 帧超时检测 **B** | monitor 框架存在但当前 disabled | 1 周；补 queue hang 可见性 | 假阳性、work 生命周期 | debug freeze heartbeat + 首帧/低 fps/stop race | Kconfig 关闭 | 是 |
| 13 | L1 queue error **B** | VB2 提供 `vb2_queue_error()`，现驱动无此超时出口 | 1 周；明确用户空间恢复契约 | 锁序与 pending Buffer | poll/DQBUF/STREAMOFF/reopen，lockdep/KASAN | 关闭 timeout 功能 | 是，强 |
| 14 | L2 原厂恢复 **C** | reset work/watchdog/private reset/soft reset 已有 | 2-4 周；厂商路径复用 | monitor disabled、Buffer/PM/remove 竞态 | 可控 timeout、stop/kill/suspend 交叉矩阵 | 保留 L1 用户态 reopen | 条件性 |
| 15 | L3 透明恢复 **D** | 无跨层 transaction，竞争面大 | 1-2 月以上；收益不确定 | UAF、double done、旧 DMA、PM 泄漏 | 需 fault matrix + KASAN/lockdep + 长稳 | 不合入主线 | 否，除非后续产品需求 |
| 16 | 一个真实 Host 修复 **B** | 有 stop error、ECC2 counter、remove-work 等候选但未复现 | 1-3 周；价值最高 | 为交付强行制造 bug | 最小复现->根因->补丁->A/B->回归 | 单补丁 revert | 是，必须诚实写复现 |
| 17 | RKISP NV12 **B** | SDITF/ISP graph 与 IQ 存在，原厂应用出图 | 3-5 天；成像闭环 | node/AIQ owner、online/readback 未确认 | media graph、mainpath NV12 4K30、AIQ/proc | 回 RAW 或原厂应用 | 是 |
| 18 | MPP H.265 高负载 **C** | MPP H.265、DRM/DMABUF、测试程序存在 | 1-2 周；系统级负载价值 | 格式/stride、编码阻塞、热降频 | NV12->DMABUF import->H.265，FPS/CPU/DDR/temp | 去掉 MPP，仅 NV12 | 条件性 |
| 19 | DMABUF 链路 **B** | RKCIF/RKISP VB2 支持 DMABUF，MPP 支持外部 dma-buf | 1-2 周；零 CPU copy 价值高 | exporter/importer stride/cache/fence | EXPBUF/import、fd identity、无 memcpy profile | MMAP baseline | 是，强 |
| 20 | 双 IMX415 **C** | 四套物理四 lane/I2C/MCLK/GPIO，两个 ISP | 2-4 周 + 第二模组；扩展价值高 | ISP 多回读、DDR/CMA/IOMMU/热、共享故障域 | 先双 RAW，再跨 ISP NV12，再编码；逐级指标 | 拔第二模组/恢复单摄 DTS | 条件性，不写成已完成 |
| 21 | 日夜辅助控制 **D** | 当前范围未确认 IR-cut/补光/光敏硬件与驱动 | 3-6 周；偏产品外设 | 硬件不足、偏离 Host 主线 | 先审原理图/BOM/接口，再单独立项 | 不接入相机主线 | 当前不适合 |

## 11. 推荐的第一版项目边界

### 11.1 必做主线

1. 冻结一份能由 `run nfsbootfdt` 重现的黄金基线，记录 Image/DTB/IQ/命令行/graph/node/格式/统计。
2. 选定一个物理接口，完成 Sensor->PHY->Host->RKCIF->SDITF->RKISP 的证据化 graph，验证 3864x2192 sensor output 和 3840x2160 crop 的真实位置。
3. 还原并用 trace 验证 STREAMON、IRQ->tasklet->DQBUF、STREAMOFF、close/runtime PM 的真实路径。
4. 在不改变采集行为的前提下补逐 stream 缺失统计与事件快照；优先复用 procfs/generic trace。
5. 自动完成长稳、100/500/1000 启停、Buffer 饥饿、异常退出、压力测试并形成原始 CSV/log。
6. 对一个稳定复现的 Host 问题做最小修复和 A/B 回归；没有复现就不强行改核心。

### 11.2 条件性增强

- 如果出现无帧且 VB2 永久等待：做 L1 timeout + queue error。
- 如果 L1 数据证明厂商 reset 能解决且生命周期测试通过：评估 L2。
- RAW 主线稳定后验证 RKISP mainpath NV12；再做 DMABUF 到 MPP H.265。
- 单路全部通过且有第二模组后，先跨 ISP 双 RAW，再双 NV12。

### 11.3 暂不实施

- 重写正式 IMX415 驱动；
- 无复现依据修改 RKCIF 核心 ping-pong/DMA 逻辑；
- L3 内核透明恢复；
- 日夜辅助控制子系统；
- 第一版同时承诺双摄 + 双 ISP + 双 MPP 编码。

## 12. 里程碑与验收方案

### M0：黄金基线

- **进入条件：** eMMC 可回退；现有 `run nfsbootfdt` 可启动；原厂应用能出图。
- **任务：** 记录仓库状态、Image/DTB/IQ hash、`/proc/cmdline`、NFS root、dmesg、media graph、所有相关 node fmt/control、RKCIF/RKISP proc；完成 10 min RAW 与 NV12 冒烟。
- **修改范围：** 仅项目文档/测试输出；不改 kernel/DTS/RootFS。
- **不修改：** U-Boot 默认环境、部署脚本、厂商驱动、生成目录。
- **验收命令：** 第 14 节的 container/U-Boot/Linux 最小采集命令。
- **退出条件：** 任意人能凭 artifact 识别正确 connector/node/DTB，并复现基础流。
- **失败回退：** 从 eMMC 启动，对比原厂应用；若原厂也失败则停止项目增量。
- **产物：** baseline manifest、media graph、raw logs、known-good hash。

### M1：DTS、Sensor 与 Media Graph 审计

- **进入条件：** M0 完成且目标 connector 唯一确定。
- **任务：** 核对 I2C/MCLK/GPIO/regulator/pinctrl/lane/link、endpoint、IQ；读回 subdev fmt/selection/control；示波器仅在 MCLK/电平存疑时使用。
- **修改范围：** 默认只写文档；只有证据显示 DTS 错误才单独提交 DTS patch。
- **不修改：** Sensor mode table、RKCIF DMA。
- **验收命令：** `media-ctl -p`、sensor subdev `--all/--get-subdev-fmt/selection`、dmesg probe。
- **退出条件：** 源码 graph 与运行 graph 一致；3864 output/3840 crop 语义明确。
- **失败回退：** 撤销单独 DTS patch，部署 M0 DTB。
- **产物：** graph 文档、connector 表、可选单一 DTS patch。

### M2：VB2、RKCIF、IRQ 与 DMA 调用链

- **进入条件：** M1 graph/node 已确认。
- **任务：** 用 function_graph、VB2/IRQ/workqueue trace 与用户 timestamp 对齐 start、FE、tasklet、DQBUF、stop；确认 MMAP/DMABUF、compact/stride/sizeimage。
- **修改范围：** 测试文档/脚本；原则上不改 kernel。
- **不修改：** IRQ handler、锁、Buffer struct、DMA register logic。
- **验收命令：** tracefs 可用性、`v4l2-ctl --stream-*`、procfs 前后快照。
- **退出条件：** 两条调用链每个阶段均有源码符号和至少一次运行证据。
- **失败回退：** 关闭 tracing，恢复 M0 采集。
- **产物：** call-chain 文档、trace excerpt、stride/format 表。

### M3：只增加可观测性

- **进入条件：** M2 已证明现有 proc/trace 的具体缺口。
- **任务：** 增加逐 stream timeout/recovery/queue-error/event correlation 计数与 snapshot；只在需要时增加 debugfs；用 static key/Kconfig 或低开销计数。
- **修改范围：** `cif/dev.h`、最小 capture/IRQ 完成点、procfs 或 debug-only 文件、Kconfig；一补丁一个主题。
- **不修改：** DMA 算法、Buffer ownership、stream ordering、自动恢复。
- **验收命令：** 同一 workload 开关前后 FPS/IRQ time/CPU；计数与用户 sequence 对账。
- **退出条件：** 统计语义有文档、无明显性能回归、STREAMOFF 后不再变化、并发读取安全。
- **失败回退：** 关闭 Kconfig/static branch 或 revert 单补丁。
- **产物：** observability patch、ABI/字段说明、开销报告。

### M4：自动化故障复现

- **进入条件：** M3 指标稳定可读。
- **任务：** 2h/8h/24h、100/500/1000 cycle、少 QBUF、延迟 DQBUF、SIGTERM/SIGKILL、CPU/I/O 压力；debug-only heartbeat freeze 仅在需要时添加。
- **修改范围：** `projects/rk3588_camera_bsp/` 后续测试程序/脚本与 debug-only injection。
- **不修改：** IRQ clear、生产默认配置、Sensor 寄存器表。
- **验收命令：** 测试程序固定 seed/run id，自动采集 CSV、dmesg、proc、meminfo、thermal。
- **退出条件：** 至少一种异常可重复，或有足够证据说明黄金基线在规定矩阵无异常。
- **失败回退：** 停 stress、STREAMOFF/close、重启到 M0；注入默认关闭。
- **产物：** test harness、failure bundle、复现说明。

### M5：一个真实问题修复

- **进入条件：** M4 有稳定复现、最小触发条件和责任层证据。
- **任务：** 根因定位；选择最小改动；补错误路径/锁/计数；原版与修复版 A/B；全套回归。
- **修改范围：** 仅根因所在 Host 模块；可选候选不等于预先认定 bug。
- **不修改：** 与根因无关的 Sensor/RKCIF 重构。
- **验收命令：** 同一 seed 至少原版连续复现、修复版 1000 cycle + 8h 不复现，KASAN/lockdep 条件允许时运行。
- **退出条件：** failure 消失且无格式/FPS/PM/双路径回归；解释符合源码和 trace。
- **失败回退：** revert 单一修复 commit，M3/M4 仍可运行。
- **产物：** reproducer、root-cause report、fix patch、A/B data。

### M6：条件性超时恢复

- **进入条件：** 真机存在超时/永久等待，M5 未从根因完全消除，且 L1 有明确用户价值。
- **任务：** 先 L1 generation-aware delayed_work + snapshot + `vb2_queue_error()`；stop/close/remove/suspend 同步取消；L2 另立 gate。
- **修改范围：** per-stream state、start/completion/stop 生命周期和 debug injection。
- **不修改：** 第一阶段不自动重启 pipeline，不实现 L3。
- **验收命令：** force-once timeout；poll/DQBUF/STREAMOFF/reopen；与主动 stop/kill/suspend 并发循环。
- **退出条件：** 零假阳性基线；100% timeout 可见；用户态 reopen 成功；无 pending work/Buffer 泄漏。
- **失败回退：** Kconfig 关闭，回到 M5 + 用户态 watchdog。
- **产物：** L1 patch、userspace contract、race matrix；L2 独立 RFC（可选）。

### M7：RKISP、MPP 或双摄扩展

- **进入条件：** M0-M5 通过；M6 非必需；每次只选一个扩展。
- **任务：** 首选 RKISP NV12；其后 DMABUF->MPP H.265；有第二模组再跨 ISP 双 RAW/双 NV12。
- **修改范围：** 测试应用/配置；只有证据要求时改 DTS/driver。
- **不修改：** 为跑分修改可靠性统计或隐藏错误；不并行引入三项扩展。
- **验收命令：** media graph + NV12 4K30；DMABUF fd import + H.265 decode verification；双路独立 sequence/error/temp。
- **退出条件：** 对应扩展达到第 13 节门槛且不破坏单路基线。
- **失败回退：** 依次移除 MPP、第二路、扩展 DTS，回到单路 M5 Image/DTB。
- **产物：** 一项独立扩展报告/patch/data；未完成项仍标条件性。

## 13. 验收指标、风险和停止条件

### 13.1 指标定义

建议先以“零内核错误、零不可解释 sequence gap、平均 FPS 在协商值 ±0.5%”作为黄金基线门槛；分位数、恢复耗时和资源阈值先采样再冻结，避免伪造经验值。

| 指标 | 数据源 | 计算/判定 |
|---|---|---|
| 连续时长/总帧数 | 测试程序 monotonic clock、DQBUF count | 2h smoke -> 8h release gate -> 24h extended；进程与 kernel count 对账 |
| 平均 FPS | `(last_ts-first_ts)/(N-1)`、proc FPS | 目标 30 fps；稳定段建议 ±0.5%，启动/恢复单列 |
| P50/P95/P99 帧间隔 | 相邻 V4L2 Buffer timestamp | 原始 ns 存 CSV；与 33.333 ms 对比，不提前虚构分位值 |
| sequence gap | `vb.sequence` | `max(0, cur-prev-1)` 累加；明确 skip-frame/recovery 是否为预期 |
| 异常 Buffer | DQBUF flags/error、应用日志 | ERROR/短帧/size mismatch 分开计数 |
| STREAMON/OFF | ioctl return + 每轮首帧 deadline | 100 后再 500/1000；目标 100%，任一失败保存完整 bundle |
| CSI CRC/ECC/SOT | CSI Host error list、dmesg | 分类型 delta；不能使用 RKCIF overflow 代替 |
| FIFO/带宽/size error | RKCIF proc/IRQ stats、dmesg | 每 run 起止差值，稳定基线目标 0 |
| no-buffer | 新/现有 lack/inactive/dummy/DMA stop stats | 与故障注入时间点关联；正常长稳目标 0 |
| timeout | 新 L1 counter/event | 正常 run 目标 0；注入 run 应一注入一事件 |
| recovery 次数/成功率 | attempt/success/fail + reopen log | `success/attempt`；L1 指用户 reopen，L2 单独统计 |
| recovery 耗时 | timeout event 到第一帧 timestamp | P50/P95/P99；L1、L2 不混合 |
| 可用内存 | `/proc/meminfo` 前后、运行中采样 | `MemAvailable/CmaFree/Slab/SUnreclaim`；稳定段不得持续单调增长 |
| Slab/对象 | slabinfo（可读时）、kmemleak/KASAN（调试内核） | 100/500/1000 线性回归；异常增长定位对象 |
| CPU | `pidstat/top/perf` 或 `/proc/stat` | 应用、ksoftirqd、AIQ、MPP 分开；记录核频率 |
| 温度/降频 | thermal sysfs、cpufreq/devfreq、dmesg | 记录 P50/max 与 throttling 事件，不预设板级温限 |
| 异常退出后重开 | kill 后 open/config/首帧 | 100 轮目标 100%；失败保留 fd/process/dmesg/proc |
| DMA/Host 延迟 | SOF timestamp、FE timestamp、VB2 done trace | 计算 SOF->FE、FE->done 分位，识别 sensor 与 Host 卡顿 |

DMABUF 验收不仅看“程序能跑”：还要确认 V4L2 `EXPBUF`/DMABUF fd 被 MPP 作为外部 buffer 导入、stride/offset/format 一致，CPU profile 中没有逐帧全图 memcpy，同时验证码流可解码和帧数对应。

### 13.2 主要未知项

- **[T]** 当前插在哪个 J18-J21、实际 sensor/subdev/video node、最终 media graph。
- **[T]** RKCIF RAW 节点实际输出 3864x2192 还是已在下游裁为 3840x2160；实际 `bytesperline/sizeimage/compact`。
- **[T]** 当前运行路径是 VICAP online 还是 DDR readback，RKISP mainpath 是否稳定 NV12 3840x2160@30。
- **[T]** DTS 无显式 supplies 与 pinctrl state 名不匹配时，probe 是否使用 dummy regulator、MCLK pin 是否正确切换。
- **[T]** 长稳 CSI/RKCIF/RKISP 错误、CMA/IOMMU 余量、CPU/DDR/温度和降频。
- **[T]** 当前 RootFS 中实际 MPP/AIQ 工具版本、DMABUF importer 路径和 H.265 4K30 负载。
- **[T]** 第二模组不存在时无法验证双摄；单 ISP 双 4K 的多回读代价尤其未知。

### 13.3 停止条件

出现以下任一项，应停止扩展并回到前一里程碑：

1. eMMC 原厂基线也不能稳定出图，说明前置硬件/供电/连接问题未解决；
2. 运行 graph 与目标 DTS 不一致或 Linux 并非使用 TFTP 最新 DTB；
3. M3 观测本身造成可测 FPS/IRQ latency 回归，尚未消除；
4. 故障不能稳定复现或责任层不能区分，禁止进入“修核心 DMA”；
5. L1 出现正常流假 timeout、pending work、lockdep/KASAN 或 Buffer 不归还；
6. L2 与主动 STREAMOFF/close/suspend 任一组合发生死锁、double done 或 PM ref 泄漏；
7. 双摄/MPP 导致持续热降频、CMA 失败或单路基线退化，立即移出第一版。

## 14. 最小板端信息采集命令

以下命令用于后续 M0；本次审计没有执行。设备节点必须先由 graph 识别，再替换占位变量。

### 14.1 Ubuntu 20.04 Dev Container

```bash
cd /rk3588_dev

git -C kernel status --short
git -C u-boot status --short
git -C buildroot status --short
git -C device/rockchip status --short

rg -n 'CONFIG_(VIDEO_IMX415|VIDEO_ROCKCHIP_CIF|VIDEO_ROCKCHIP_ISP)|ROCKCHIP_CIF_' \
  kernel/arch/arm64/configs/alientek_rk3588_defconfig kernel/.config

sha256sum kernel/arch/arm64/boot/Image \
  kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-2mipi1080x1920-2hdmi.dtb \
  tftpboot/atk_dlrk3588/Image \
  tftpboot/atk_dlrk3588/rk3588-alientek-nfs.dtb
```

后续确有 kernel/DTS 修改时，仍按普通用户 `vscode` 分项构建和部署，不用 root/sudo 编译：

```bash
cd /rk3588_dev
./build.sh kernel
./projects/scripts/deploy_net_boot.sh \
  -d rk3588-alientek-2mipi1080x1920-2hdmi \
  -m
```

仅改 DTB 且模块未变时可省略 `-m`。不要在 Ubuntu 24.04 VMware 宿主机分析或构建源码。

### 14.2 开发板 U-Boot

```text
printenv nfsbootfdt nfsboot nfsimage nfsfdt ipaddr serverip netmask kernel_addr_r fdt_addr_r
run nfsbootfdt
```

成功进入 Linux 前不要把 `run nfsboot` 当作等价命令：当前它不下载最新 DTB。

### 14.3 开发板 Linux

先确认启动和根文件系统：

```sh
cat /proc/cmdline
uname -a
mount | grep ' / '
cat /etc/rootfs-source
ip addr show eth0
dmesg > /tmp/camera-baseline-dmesg.txt
```

发现 graph 和节点，不假设编号：

```sh
media-ctl -p
v4l2-ctl --list-devices
for media_node in /dev/media*; do
    echo "===== ${media_node} ====="
    media-ctl -d "${media_node}" -p
done
```

根据 graph 人工确定下列三个变量后再执行：

```sh
CAM_RAW_NODE=/dev/videoX
CAM_SENSOR_NODE=/dev/v4l-subdevY
CAM_ISP_NODE=/dev/videoZ

v4l2-ctl -d "${CAM_RAW_NODE}" --all
v4l2-ctl -d "${CAM_RAW_NODE}" --get-fmt-video
v4l2-ctl -d "${CAM_SENSOR_NODE}" --all
v4l2-ctl -d "${CAM_SENSOR_NODE}" --get-subdev-fmt pad=0
v4l2-ctl -d "${CAM_SENSOR_NODE}" --get-subdev-selection pad=0,target=crop_bounds
v4l2-ctl -d "${CAM_ISP_NODE}" --all
v4l2-ctl -d "${CAM_ISP_NODE}" --list-formats-ext
```

短时采集时，格式必须使用上述查询得到的真实 fourcc/尺寸；下面不硬编码 `/dev/video0`：

```sh
v4l2-ctl -d "${CAM_RAW_NODE}" \
  --stream-mmap=4 --stream-count=300 --stream-to=/dev/null --verbose

v4l2-ctl -d "${CAM_ISP_NODE}" \
  --stream-mmap=4 --stream-count=300 --stream-to=/dev/null --verbose
```

收集错误、proc、内存和温度：

```sh
dmesg | grep -Ei 'imx415|csi|cif|vicap|rkisp|iommu|overflow|bandwidth|crc|ecc|sot|timeout'
find /proc -maxdepth 1 -type f \( -name '*cif*' -o -name '*isp*' \) -print
cat /proc/meminfo
for thermal_file in /sys/class/thermal/thermal_zone*/temp; do
    printf '%s ' "${thermal_file}"
    cat "${thermal_file}"
done
```

只读检查可用通用 tracing；不要为了采集基线现场新建不存在的 debugfs 节点：

```sh
test -r /sys/kernel/debug/tracing/available_filter_functions && \
  grep -E 'rkcif_(do_start_stream|irq_pingpong_v1|buf_done_prepare|do_stop_stream)|imx415_s_stream' \
  /sys/kernel/debug/tracing/available_filter_functions

test -d /sys/kernel/debug/tracing/events/vb2 && \
  find /sys/kernel/debug/tracing/events/vb2 -maxdepth 1 -mindepth 1 -type d -print
```

## 15. 下一步唯一建议任务

**只执行 M0 黄金基线采集，不修改任何驱动。** 用当前 `run nfsbootfdt` 启动，按第 14 节取得 `/proc/cmdline`、完整 `media-ctl -p`、相关 node 的 `--all/G_FMT/selection`、RKCIF/RKISP proc、300 帧 RAW/NV12 日志和 dmesg。只有这组 artifact 能回答“实物接在哪个接口、实际 node/stride/crop、online/readback 与错误基线”，也是后续任何 Host 补丁的唯一可信对照。
