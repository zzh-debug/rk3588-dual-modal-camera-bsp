# RK3588 双模态成像与智能预警简历项目可实现性及成果边界审计

> 审计日期：2026-08-02  
> 工作区：`/rk3588_dev`  
> 目标平台：ATK-DLRK3588，Rockchip Linux 5.10.209，板级配置 `01_atk_dlrk3588_auto2mipi_2hdmi_defconfig`  
> 审计性质：只读源码、文档、原理图、构建记录与现有产物审计；本次未修改源码、未编译、未部署、未刷机、未清理仓库  
> 结论时点：本报告只认截至审计日已经存在的代码、日志和实物证据。规划中的功能不能使用完成时态写入简历。

## 1. 结论先行

### 1.1 总体评级

| 项目 | 按初稿原文评级 | 收敛到建议第一版后的评级 | 当前能否作为“已完成项目”投递 | 核心判断 |
|---|---:|---:|---|---|
| 项目一：双模态成像 BSP 与高可靠采集 | **B-/条件可行** | **B/可实现** | **不能按原文投递** | IMX415、RKCIF、RKISP、网络迭代环境及内核通用 MLX90640 参考驱动基础充分；但 MLX90640 板级节点/配置/实物、LED 硬件、Host 增量和稳定性数据尚不存在。第一版应止于“隔离式 IMX415 导入 + 经重新设计的 MLX90640 原始数据节点 + 可观测性 + L1 错误上报”。 |
| 项目二：协同感知与低照度智能预警 | **C+/有明显前置条件** | **B-/可实现但需系统联调** | **当前不应作为完成项目投递** | RGA、RKNN、MPP、OpenCV、FFmpeg/GStreamer、MediaMTX/RKADK 组件存在，但没有本项目应用代码、外部热阵列/补光硬件、标定数据、性能日志或端到端 Buffer 证据。“全程零拷贝”和“像素级精准对齐”必须删除。 |

评级含义：`A` 为现有硬件和源码证据充分、主要剩余板测；`B` 为技术路径清楚且工作量可控；`C` 为依赖新增硬件、接口实测或高风险跨子系统开发；`D` 为当前平台或证据下不应承诺。评级描述的是可实现性，不是当前完成度。

### 1.2 当前已成立和未成立的成果边界

已成立：

- 已有普通用户下的 SDK 全量构建成功记录，覆盖 Loader、U-Boot、Kernel/DTB、Buildroot RootFS、Recovery 和 `update.img`，并有产物哈希与构建日志。
- 已有真实开发板 TFTP + NFS RootFS 启动闭环；当前日常 DTS/驱动开发的正确入口是 `run nfsbootfdt`，加载固定由 `rk3588-alientek-2mipi1080x1920-2hdmi.dtb` 生成的最新 Image + DTB。eMMC 默认启动链仍是回退路径。
- 厂商已经提供完整 IMX415 基础路径：四个 Sensor DTS 分支、IMX415 V4L2 Subdev 驱动、Media Graph、RKCIF、RKISP 和匹配 IQ 文件。
- 已经完成独立的阶段一最小识别代码、实验 DTS、Kconfig/Makefile 和本地构建部署；但尚无真实板端识别日志，只能表述为“代码及编译部署完成，待真机验证”。
- 当前 SDK/RootFS 确实包含 RGA、RKNN、MPP、OpenCV 4.5.4、FFmpeg、GStreamer、Live555、MediaMTX 1.7.0 以及 RKADK RTSP/OSD/录像源码和示例。

未成立：

- 尚未证明 J20 上的实验节点在真机稳定读到 `0x311A = 0xE0`，更没有独立自研 IMX415 mode、V4L2 Subdev、Controls、`s_stream()` 或 Media Graph 成果。
- 当前内核已有通用 `video-i2c.c` 的 MLX90640 参考实现和 DT binding，但 `CONFIG_VIDEO_I2C` 未启用，板级 DTS、用户态温度库集成、测试程序和板端日志均不存在，也没有 MLX90640 模组原理图/实物信息；因此不能把“源码中有参考驱动”写成项目已接入。
- 没有白光 LED、恒流驱动/MOSFET、电源预算、PWM 引脚分配或波形证据。
- 没有新增 RKCIF debugfs/trace 代码、可复现故障、修复补丁、A/B 数据或自动恢复证明。
- 没有双模态并采、标定、RKNN 人员检测、融合、编码、RTSP、OSD 或事件录像的本项目代码和端到端日志。

因此，初稿是一个合理的实施蓝图，不是当前已经完成的成果清单。

## 2. 证据口径和直接信息源

### 2.1 证据等级

| 标记 | 含义 |
|---|---|
| `[S]` | 开发板/模组原理图或传感器数据手册直接证据 |
| `[K]` | 当前内核、DTS、用户态源码或构建配置直接证据 |
| `[B]` | 已有本机构建记录或真实开发板日志 |
| `[V]` | 尚需开发板、示波器、逻辑分析仪或压力测试验证 |
| `[H]` | 尚需新增外部硬件或确认具体模组版本 |
| `[A]` | 尚需厂商 API、模型、标定方法或其他外部资料 |

### 2.2 已核对的主要资料

| 类别 | 直接信息源 |
|---|---|
| 工作区约束与启动链 | `AGENTS.md`、`TFTP_NFS_网络启动.md`、`U-Boot_DTB与TFTP_NFS网络启动机制总结.md`、`docs/bringup/phase0_sdk_full_build_report.md`、`docs/bringup/phase1_tftp_nfs_report.md` |
| 既有 IMX415 审计 | `projects/rk3588_camera_bsp/docs/feasibility-study.md`、`阶段一硬件契约.md`、`independent-imx415-bringup-plan.md` |
| 开发板硬件 | `ATK-DLRK3588B V1.2(底板原理图).pdf`、`ATK-DLRK3588B开发板规格书V1.4.pdf` |
| IMX415 硬件 | `ATK-MCIMX415 V1.4 原理图.pdf`、`IMX415-AAQR-C_Datasheet_E19504(产品信息).pdf`、ATK-MCIMX415 规格书 V1.1 |
| IMX415 与图链 | `kernel/drivers/media/i2c/imx415.c`、`rk3588-alientek-cameras.dtsi`、目标 1080x1920 DTS、`kernel/drivers/media/platform/rockchip/{cif,isp}/` |
| 阶段一实现 | `projects/rk3588_camera_bsp/driver/zzh_imx415_minimal.c`、同目录 Kconfig/Makefile、`rk3588-alientek-imx415-minimal.dtsi` |
| 网络部署 | `projects/scripts/deploy_net_boot.sh`；当前策略以 `AGENTS.md` 的 `run nfsbootfdt` 为准，脚本内早期注释不能覆盖已验证的现行规则 |
| MLX90640 | Melexis 官方 `MLX90640 32x24 IR array Datasheet, Revision 12, 2019-12-03` 和官方 `mlx90640-library`；`kernel/drivers/media/i2c/video-i2c.c`、`kernel/Documentation/devicetree/bindings/media/i2c/melexis,mlx90640.txt`、当前内核 `.config`；工作区没有 Melexis 手册/官方用户态算法副本、板级 DTS 或本项目实现，本次只作只读核对，未下载保存 |
| RGA/RKNN/MPP | `external/linux-rga/`、`external/rknpu2/`、`external/mpp/`、Buildroot 当前 `.config` 和 NFS RootFS 已安装库 |
| RTSP/OSD/录像 | `app/rkadk/`、Buildroot 的 FFmpeg/GStreamer/Live555/MediaMTX 配置、`nfs_rootfs/atk_dlrk3588/etc/mediamtx.yml` |

## 3. 基线事实核查

### 3.1 IMX415 实际能力和所有权

1. 当前厂商首个线性模式是 **3864x2192、RAW10、4 lane、30 fps**，见 `imx415.c` 的 `supported_modes[]`。`3840x2160` 是推荐记录窗口/居中裁剪，不是另一张自研 mode 表。
2. 驱动 link frequency 为 `446 MHz`；按 DDR 传输约为 `892 Mbit/s/lane`，与 Sony 标称 `891 Mbit/s/lane` 是同一数量口径，不能混写成两种模式。
3. 现有图链是 `Sensor -> D-PHY/DC-PHY -> CSI2 Host -> RKCIF`，再由 `RKCIF SDITF -> RKISP`。DTS endpoint、RKCIF/RKISP 驱动和 IQ 文件均为厂商已有能力。
4. 原厂出图只能证明至少一条既有路径可用；实际 `/dev/videoX` 映射、RAW compact stride、RKISP mainpath NV12、online/readback 模式、DMABUF 和 4K30 长稳仍须目标板日志。
5. 3840 宽 RAW10 compact 的 RKCIF 行步长不能简单写成 `3840*10/8=4800`，RK3588 对齐后应按 `ALIGN(4800, 256)=4864` 核实。

### 3.2 IMX415 电源和“Chip ID”边界

J20/MIPI CSI3 是当前实验选择，尚不是实物连接器确认：

| 项目 | 已核实结论 | 边界 |
|---|---|---|
| I2C | `i2c3`，7 位地址 `0x1a`；线上写/读地址字节为 `0x34/0x35` | DTS 和 Linux client 只写 7 位地址 `0x1a` |
| MCLK | `CLK_MIPI_CAMARAOUT_M3`，目标 37.125 MHz | 需真机用 clk_summary/示波器确认 |
| RESET/PWDN | GPIO1_PB1、GPIO1_PA7，实验 DTS 均按低有效描述 | PWDN 是模组/板级契约；实际电平仍需测量 |
| 电源 | 连接器只输入 3.3 V；模组本地 LDO 产生约 2.8 V AVDD、1.8 V DOVDD、1.2 V DVDD | 三颗 LDO 没有软件 enable，Linux 不能完成或证明三路 rail 的独立时序 |
| 识别签名 | 厂商代码读取 `0x311A` 期望 `0xE0` | Sony 手册把 `0x311A` 定义为 `INCKSEL4`，`0xE0` 是复位值；只能称“厂商参考识别签名”，不能称“官方 Chip ID” |

Sony 手册规定的理想 rail 顺序是 `DVDD -> OVDD -> AVDD`，断电反向；所有 rail 稳定后 XCLR 保持低至少 500 ns，XCLR 释放后至少 1 us 才给 INCK 边沿、至少 20 us 才通信。当前 ATK 模组由于 rail 由无使能本地 LDO 生成，软件真正能控制的是 RESET/PWDN/MCLK 及等待时间，不能把它描述成“完成三路 Sensor 电源时序”。

### 3.3 MLX90640 协议、速率和刷新率

#### 3.3.1 当前内核已有参考能力及其边界

当前源码树确实包含 MLX90640 支持，不能把它误判成“从零开始”：

- `kernel/drivers/media/i2c/video-i2c.c:71-78` 定义 `V4L2_PIX_FMT_Y16_BE`、`32x26`，其中 24 行为像素、2 行为处理数据；`348-384` 定义 0.5~64 Hz、1664-byte Buffer、EEPROM nvmem provider 和 MLX transfer/setup；`387-564` 已有 read/MMAP/poll、采集 kthread、VB2 入队/完成、start 失败回滚和 STREAMOFF ERROR 回收；`789-817` 支持 `VB2_DMABUF | VB2_MMAP | VB2_USERPTR | VB2_READ` 及 monotonic timestamp。
- `kernel/Documentation/devicetree/bindings/media/i2c/melexis,mlx90640.txt` 给出 `compatible = "melexis,mlx90640"` 和 7 位地址 `0x33`；但目标板 DTS 没有该节点，当前 `kernel/.config` 明确为 `# CONFIG_VIDEO_I2C is not set`，所以它既没有进入当前内核，也没有任何板端运行证据。
- 该通用实现按软件定时直接从 `0x0400` bulk read，把结果包装成普通 32x26 Y16_BE video frame；它没有等待/清除 `0x8000` new-data、记录 subpage ID、保存 control/status、验证 0/1 交替、把两页组成有明确时间语义的 pair，也没有版本化 raw ABI。这些正是本项目仍需自行设计的部分。
- 现有 `mlx90640_xfer()` 将 `buffer_size = 1664` 直接作为 `regmap_bulk_read()` 的 `val_count`，而 regmap 配置为 16-bit value；启用前必须审计“字节数/寄存器值个数”口径及目标 I2C adapter 的分段限制。nvmem callback 的 byte offset 到 16-bit register address 换算也必须用 EEPROM 重复读取和哈希验证。报告不能在未验证前把这条通用路径称为安全可用。

因此，自研价值不在于声称 SDK 完全没有 MLX90640，而在于以现有 `video-i2c` 为对照，给出符合 subpage、时间戳、错误传播和用户态温度算法需求的可审计 ABI；也可以先修正/扩展通用驱动，但必须把上游已有代码与个人增量分账。

#### 3.3.2 官方协议事实

| 项目 | 官方资料结论 | 对本项目的约束 |
|---|---|---|
| 地址 | 默认 7 位地址 `0x33`，可编程 | 简历、DTS、`i2c_client->addr` 应写 `0x33`，不要写移位后的地址字节 |
| 电气 | 3.0~3.6 V 供电，典型 3.3 V；典型约 20 mA，最大约 25 mA；SDA/SCL 可容忍 5 V | 本板使用 JP5 的 3.3 V I2C 信号，并从 JP1（VOUT1）专用 3.3V/GND 供电；JP2（VOUT2）为 5V，禁止接入 |
| 总线 | RAM/寄存器通信支持 FM+，最高 1 MHz；**EEPROM 操作最高 400 kHz** | 当前共享总线和开发板资料以 400 kHz 为稳妥第一版；不能为追求高刷新率把整条共享总线直接升到 1 MHz |
| EEPROM | `0x2400..0x273F`，832 个 16-bit word，保存校准常数和配置 | 通常上电/初始化读取一次并缓存；不应逐帧读取 |
| RAM/寄存器 | pixel `0x0400..0x06FF` 共 768 word；辅助区从 `0x0700`；状态 `0x8000`；控制 `0x800D` | 传输是 16 位寄存器地址和 16 位大端 word；需检查适配器 block 长度并支持分段批量读 |
| Subpage | Chess/Interleaved 都把阵列分成 subpage 0/1；默认 Chess，官方建议 Chess | 单个 subpage 不是完整同步的 32x24 温度帧；必须保留 subpage ID、状态和时间戳 |
| 刷新率 | 控制值为 0.5/1/2/4/8/16/32/64 Hz，表示**新 subpage** 到达速率 | 组合两页后，完整 Ta/温度矩阵的更新速率是设置值的一半；例如设置 16 Hz，完整成对输出上限约 8 Hz |

官方 API 每个 subpage 读取 768 pixel word、64 auxiliary word，再读控制和状态，共形成 834-word `frameData`。仅 832 个 RAM word 的有效载荷就是 1664 byte；400 kHz 下加上地址、ACK 和多次事务后，理论线时已约 37 ms。由此：

- 8 Hz 或 16 Hz subpage 设置适合作为第一版目标，分别对应约 4 Hz 或 8 Hz 完整成对帧。
- 32 Hz 的 subpage 周期只有 31.25 ms，小于单次完整读取的保守线时，400 kHz 共享总线下不应承诺稳定实现。
- 64 Hz 更不现实。若以后使用 1 MHz，仍必须在 400 kHz 下读 EEPROM，且需要确认同总线触摸、Type-C/RTC 等设备是否支持，或把 MLX90640 放到独占控制器。

### 3.4 MLX90640、LED 与开发板接口

底板原理图 JP5 扩展口提供：

- I2C5：JP5 pin 2/4，3.3 V；当前 DTS 上还有 RTC/DSI0 触摸等设备，`0x33` 无直接地址冲突。
- I2C6：JP5 pin 6/8，3.3 V；当前 DTS 上还有 Type-C、RTC/DSI1 触摸等设备，`0x33` 无直接地址冲突。
- I2C2：JP5 pin 11/13，为 1.8 V，不适合作为未加电平转换的默认接法。
- JP5 右侧若标注 `3.3V`，表示对应 GPIO 的 I/O 电压域，不等于电源输出；
  MLX90640 VCC/GND 应使用板边 JP1（VOUT1）的专用 3.3V/GND，JP2（VOUT2）为 5V；JP5 pin 10 的 GPIO0_C6
  还被基准 DTS 用作 PCF8563 RTC 中断，不能供电。

当前可在 JP5 复用成 PWM 的候选脚并不是无条件空闲：GPIO1_PD2/GPIO1_PA2/GPIO1_PA7 等分别与现有摄像头 RESET/PWDN 资源关联，PWM1/PWM15 被两路屏背光使用，PWM3 被风扇使用。第一版必须先选定唯一 IMX415 接口并做 pinctrl 冲突表；例如释放未使用摄像头分支后再选择一个 JP5 PWM 复用脚，不能在不改资源所有权的情况下直接声明 LED PWM 完成。

白光 LED 不能由 SoC GPIO/PWM 直接供电。至少还需要：

- 明确 LED 模组电压、最大/持续电流、功率、散热和光学安全信息；
- 逻辑电平兼容的 N-MOSFET 或恒流 LED 驱动器、栅极电阻/下拉、公共地和独立电源预算；
- 若为感性负载才需要续流器件；纯 LED 负载重点是恒流、浪涌、热和 EMI；
- 上电默认关闭、PWM 失控保护和过温/过流回退；
- 示波器确认频率、占空比、逻辑电平和负载电流。

### 3.5 RKCIF IRQ、DMA 和 VB2 的真实调用链

RK3588 ping-pong 路径的关键链路是：

```text
硬件 IRQ
  -> dev.c:rkcif_irq_handler()
  -> capture.c:rkcif_irq_pingpong_v1()
     -> 读取/清除 INTSTAT
     -> 分类 SIZE/FIFO OVERFLOW/BANDWIDTH/FRAME END/FRAME START
     -> rkcif_update_stream()
        -> 选择 active/curr/next buffer，更新下一 DMA 地址
        -> rkcif_buf_done_prepare()
           -> 写 fs timestamp、sequence、fe timestamp
           -> rkcif_vb_done_tasklet()
              -> vb_done_list
              -> rkcif_tasklet_handle()
                 -> rkcif_vb_done_oneframe()
                    -> payload/计数
                    -> vb2_buffer_done(..., VB2_BUF_STATE_DONE)
```

停止和失败路径并不相同：

- 正常/异常 stop 会把 current、next、排队和 pending-done Buffer 汇总后以 `VB2_BUF_STATE_ERROR` 交回。
- start 失败 unwind 会把未真正采集的 Buffer 以 `VB2_BUF_STATE_QUEUED` 交回，让 VB2 核心统一取消。
- RKCIF 已有 ping-pong、dummy buffer、IRQ 错误计数、tasklet 完成、soft reset、reset work/watchdog 和 procfs。新增代码只能宣称补充未覆盖的逐 stream 观测，不能把已有能力算作原创。
- `CONFIG_ROCKCHIP_CIF_MONITOR`/对应 mode 在当前配置中未启用；框架存在不等于板上正在自动恢复。

低开销统计的合理插点：

| 统计 | 插点 | 约束 |
|---|---|---|
| SOF/FE、size/overflow/bandwidth | `rkcif_irq_pingpong_v1()` 已完成状态分类的位置 | 只做无阻塞计数、时间戳和小型事件快照；禁止格式化大日志、分配内存或等待 |
| DMA slot/active buffer/no-buffer | `rkcif_update_stream()` 选择并轮换 Buffer 的位置 | 必须按 stream/slot 区分，不能把“DMA 完成”与“VB2 已交用户”混为一谈 |
| VB2 done/ERROR、sequence、延迟 | `rkcif_buf_done_prepare()` 和 tasklet 的 `rkcif_vb_done_oneframe()` | 统计 IRQ 到 tasklet、SOF 到 done 的延迟；保证同一 Buffer 只完成一次 |
| last frame/timeout | 在成功 VB2 done 时更新 heartbeat；delayed work 只读代际和时间 | start/stop/close/remove/suspend 必须同步取消，防止旧 work 处理新一代 stream |
| debugfs/trace | debugfs 读时汇总；tracepoint 受开关控制 | 不在高频路径打印；读统计时接受一致性快照或加最小锁 |

### 3.6 RGA、RKNN、MPP 和 DMA-BUF 边界

源码充分证明接口存在：

- RKCIF capture queue 支持 `VB2_MMAP | VB2_DMABUF`，RKISP v30 capture queue 支持 `VB2_MMAP | VB2_USERPTR | VB2_DMABUF`，并使用 monotonic timestamp。
- RGA im2d 提供 `importbuffer_fd()`、`wrapbuffer_fd_t()`、异步 fence 和 `imsync()`。
- 当前 RKNN API 提供 `rknn_create_mem_from_fd()`、`rknn_set_io_mem()`、`rknn_mem_sync()`；示例还演示把 RKNN input fd 直接作为 RGA 输出。
- MPP 提供 `MppBufferInfo`/`mpp_buffer_import()`、外部 fd、DRM allocator、`mpp_frame_set_buffer()`、stride/format 和 cache sync；编码器支持 AVC/H.264 与 HEVC/H.265。

这些证据只支持“构建基于 DMA-BUF 的链路、减少 CPU 全帧复制”，不支持“RKISP、RGA、RKNN、MPP 全程零拷贝”。原因是：

1. RAW 进入 RKISP、RKISP 输出 NV12、RGA 生成模型 RGB tensor、OSD 合成后的编码 NV12 是不同格式和尺寸，至少需要硬件读写不同 Buffer；零 CPU `memcpy` 不等于零内存搬运。
2. RKNN 模型的 NHWC/NCHW、数据类型、量化、`size_with_stride` 和 pass-through 必须匹配；仅有 fd import API 不保证任意 ISP Buffer 可直接作为 tensor。
3. OSD 若由 CPU/OpenCV 画到整帧，会引入映射和写回；若由 RGA/RKADK region 合成，通常仍需目的 Buffer 或 overlay plane。
4. MPP 输入格式、水平/垂直 stride、颜色空间和 Buffer 生命周期必须与上游一致；简单 `encode_get_packet` 还可能复制码流。
5. CPU 访问 DMA-BUF 前后需要 `DMA_BUF_IOCTL_SYNC START/END` 或库等价接口；RGA 异步任务需要 fence/`imsync()`，RKNN/MPP 也有各自同步和占用期。不能用 `close(fd)` 或重新 QBUF 提前回收仍被下游使用的 Buffer。

建议的可证明链路：

```text
Pool A: dma-heap/DRM NV12 buffers
  -> VIDIOC_QBUF(V4L2_MEMORY_DMABUF) 给 RKISP mainpath
  -> DQBUF 后 RGA import fd

Pool B: RKNN input tensor fd
  -> RGA 将 A 裁剪/缩放/转 RGB 到 B
  -> 等待 RGA fence
  -> rknn_create_mem_from_fd + rknn_set_io_mem + rknn_run

Pool C: 编码/OSD NV12 buffers（需要叠加时）
  -> RGA/region 合成检测框、文字或小热图
  -> MPP import fd + mpp_frame_set_buffer
  -> 编码完成后才释放/复用

控制面
  -> 有界队列保存 fd、generation、sequence、timestamp 和 ownership
  -> 超时只丢弃可丢阶段，不破坏仍由设备占用的 Buffer
```

验收应写“CPU profile 中无逐帧 4K 全图 `memcpy`，fd/stride/ownership 可追踪”，而不是只看应用能运行。

### 3.7 RTSP、OSD、编码和事件录像库边界

| 功能 | 当前 SDK 证据 | 仍需开发/验证 |
|---|---|---|
| H.264/H.265 | MPP 编码 API、测试程序、RootFS `librockchip_mpp.so` | 目标分辨率/FPS/码率/GOP、输入 stride、阻塞、码流可解码、CPU/DDR/温升 |
| RTSP | RKADK RTSP 源码，Live555，FFmpeg/GStreamer，RootFS MediaMTX 1.7.0 且 RTSP `:8554` 配置存在 | 选择一条架构：嵌入 RKADK RTSP，或应用向 MediaMTX 发布；证明多客户端、TCP/UDP、断连重连和延迟 |
| OSD | RKADK OSD 示例支持 ARGB8888 bitmap；RGA 支持 blend/OSD 相关操作 | 中文/温度文字的字形生成、坐标缩放、热图 alpha、更新频率和额外带宽；不能把示例 bitmap 当作本项目 OSD 已完成 |
| 事件录像 | RKADK muxer 有 pre-record/manual split 代码，FFmpeg/libavformat 也可复用 | 环形编码包缓存、从 IDR 开始、VPS/SPS/PPS、时间戳、封装、磁盘满/慢、文件原子关闭和掉电恢复 |
| 指标 | procfs/debugfs、应用计数、MediaMTX metrics 可配置 | 定义 fps/drop/queue depth/latency/temperature/bitrate/recovery 指标和持久化格式 |

## 4. 两类 IMX415 自研价值必须分开评价

### 4.1 隔离路径独立重做的 BSP 学习价值：高

保留厂商节点和驱动，通过独立 `compatible`、独立 Kconfig/Makefile、独立实验 DTS，按阶段完成 I2C 识别、mode、Subdev、Controls、runtime PM、Media Graph 和 stream bring-up，能够真实训练：

- 从原理图/手册建立 I2C、MCLK、GPIO、电源与时序契约；
- 区分 7 位地址、线上地址字节、寄存器宽度和 endian；
- 理解 V4L2 Subdev pad ops、Controls、runtime PM、Media Controller endpoint；
- 理解 Sensor、CSI2 Host、RKCIF、RKISP 的 ownership 和启动/回滚顺序；
- 通过与原厂实现 A/B 对照验证，而不是把“能出图”当作唯一目标。

这类重做即使最终 mode 与厂商一致，也有明确学习价值。关键是代码必须独立组织、结论注明来源，并保留原厂路径作为 oracle/回退。

### 4.2 相对原厂实现的生产新增价值：当前低，完成可靠性闭环后可升至中高

以下内容不是生产新增价值：

- 抄录厂商 3864x2192 RAW10 30 fps mode 表；
- 复用原有四条 DTS graph、IQ 文件和 RKISP 支持后称“自研打通”；
- 把原有 ping-pong DMA、IRQ 错误计数、tasklet、watchdog/reset work 改名描述为本人开发；
- 只证明和原厂一样能预览。

可形成生产新增价值的内容：

- 原理图/手册/DTS/驱动之间可审计的硬件契约和回退路径；
- 逐 stream、可关联 SOF/FE/DMA/VB2/error 的低开销可观测性；
- 可重复的 100/500/1000 次启停、长稳、少 Buffer、慢 DQBUF、异常退出、总线故障和压力测试；
- 对真实复现问题的最小补丁、A/B 数据和无回归矩阵；
- 明确的 L1 错误传播和用户态恢复；只有通过竞态/PM/多路交叉验证后，L2 才可能成为生产增量。

## 5. 项目一逐条可行性矩阵

### 5.1 矩阵

| ID/初稿工作 | 按原文可实现 | 当前源码已有能力 | 仍需自行开发 | 额外硬件/资料 | 主要风险 | 必须取得的证据 | 建议 |
|---|---|---|---|---|---|---|---|
| P1-0 标题、个人独立开发、技术栈 | **条件性**。项目可以独立完成，但厂商基础不能算个人开发 | Linux 5.10 BSP、完整相机栈和媒体组件 | 独立路径代码、测试和本人增量 | MLX90640、LED 模组/驱动电路 | “个人独立”被追问时无法划清厂商代码 | 自研文件清单、diffstat、设计文档、commit、A/B 日志 | 保留标题；注明“基于厂商 BSP，在隔离路径实现/补充”，不要暗示从零开发 RKCIF/RKISP |
| P1-1 “完成 Loader、U-Boot、Kernel、DTB、RootFS 完整构建与烧写；TFTP+NFS；保留 eMMC” | **部分不成立**。“构建”成立；“完整烧写”证据不足 | 全量构建日志、U-Boot 单分区更新、真机 TFTP/NFS 已完成 | 固化部署校验和回退脚本即可 | 串口、TFTP/NFS 网络 | 把 Phase 0 未刷机、单刷 U-Boot 写成全镜像烧写 | build exit 0、哈希、U-Boot printenv/TFTP、NFS mount、刷写命令和分区日志 | 改为“完成 SDK 全量构建与镜像校验；单独更新 U-Boot 分区并打通 TFTP Image/DTB + NFS RootFS，保留 eMMC 默认启动回退” |
| P1-2 “根据原理图完成 IMX415、MLX90640、白光 LED Pinmux/电源/DTS，打通两条链路” | **当前不可按完成时态** | IMX415 原厂 graph；阶段一实验 DTS/最小驱动已构建；内核有未启用的通用 MLX90640 驱动/binding | 独立 IMX415 完整导入、MLX DTS/驱动适配或独立实现、LED pinmux/PWM/电源电路、双路并采 | MLX90640 具体模组；LED、恒流/MOSFET、电源；连接器/FOV资料 | PWM 与相机/背光/风扇冲突；共享 I2C；把本地 LDO 写成三路可控；厂商 graph 冒充自研 | 运行 DT、`media-ctl -p`、I2C bus 映射/定点寄存器读、MCLK/GPIO/PWM 波形、供电/电流、两路 sequence/timestamp | 只有完成后才写；改成“在独立 compatible/DTS 路径实现 IMX415 导入，并基于现有通用驱动审计新增 MLX90640 稳定采集 ABI 与外置恒流 LED 控制” |
| P1-3 “开发 MLX90640 驱动：EEPROM、批量 I2C、Subpage、V4L2/VB2、MMAP/poll/sequence/timestamp/异常回收” | **可实现，工作量中等** | `video-i2c.c` 已有 MLX90640 Y16_BE 32x26、nvmem、VB2/read/MMAP/poll/DMABUF/USERPTR、monotonic timestamp 和基本回收；当前未配置、未接板 | 审计/修复 bulk-read 与 nvmem 口径；new-data/status、0/1 成对、两页时间戳、版本化 meta ABI、适配器分段、测试程序；或独立实现这些增量 | 官方 datasheet/API；实物 BAA/BAB 模组；逻辑分析仪 | 把内核已有驱动冒充自研；刷新率误读；400 kHz 带宽；endian；重复/缺页；stop 与 work 竞态；错误 Buffer double-done | 原厂通用/自研 diff 与 A/B；EEPROM 哈希/ID、总线 trace、subpage 0/1 交替、V4L2 compliance、MMAP/poll、序号/时间戳、I2C 注错与 stop 压测 | 保留，但改成“审计现有通用驱动并实现/补充……”；把温度计算留在用户态，写明 raw metadata ABI 和完整 pair 更新率是寄存器设置一半 |
| P1-4 “深入分析 RKCIF IRQ/DMA/VB2，增加统计” | **可实现，但必须写成补充** | 已有 IRQ stats、procfs、CRC/ECC/SOT、frame end、overflow、tasklet | SOF/FE/DMA/VB2 关联、last-complete、queue-error/recovery 快照、tracepoint/debugfs 文档 | 一路稳定 IMX415；可控注错手段 | 重复已有计数；IRQ 开销；无锁读取撕裂；多 stream 归属错误 | 调用图、插点 diff、trace 样例、开关前后 CPU/IRQ 对比、计数与帧数闭环 | 改成“基于既有 procfs/irq_stats 补充逐 stream 关联统计”，明确哪些计数原厂已有 |
| P1-5 “帧超时异步恢复，完成 DMA 停流、Buffer 回收、链路重启和回滚” | **原文过度承诺** | 原厂 soft reset/reset work/watchdog 框架存在但当前 monitor idle/disabled | L1 generation-aware timeout、queue error、同步取消、用户态 reopen；L2 需另行审计 | 可重复 freeze/断链方法，串口和长期压力环境 | double-done、UAF、stop/recovery/suspend 竞态、PM 引用泄漏、跨 Sensor/CSI/ISP 状态不一致 | lock/state 图、fault injection、1000 次 stop/recover、KASAN/lockdep（可行时）、Buffer 数量守恒、A/B 恢复率 | 第一版删除“采集链路重启/无感”；写“超时后 `vb2_queue_error()`，由用户态 STREAMOFF/reopen 恢复”。L2 完成后再升级措辞 |
| P1-6 “脚本化部署和稳定性测试，完成 IMX415 4K30、MLX 并采、长稳/启停/总线异常/恢复” | **技术可做，当前未完成** | 部署脚本和网络启动已存在；厂商 IMX415 模式存在 | 专用采集器、统计解析、测试矩阵、故障注入、结果归档 | MLX 硬件、LED（若纳入）、足够散热/存储 | 把 3864x2192 mode 简写为自研 4K30；无日志却称“完成”；热降频/丢包混淆 | 原始命令、完整日志、CSV、码流/帧 hash、持续时长、启停次数、错误注入、CPU/DDR/温度 | 完成前删除；完成后写具体数字，如“12 h、1000 次、0 double-done、恢复 P95”而非泛称稳定 |

### 5.2 项目一第一版和完整版边界

| 版本 | 纳入 | 明确不纳入 | 验收门槛 |
|---|---|---|---|
| V1 | J20 阶段一真机识别；独立 IMX415 单线性 mode/Subdev/Controls/graph；单路 RAW 与 RKISP NV12；MLX90640 400 kHz、8/16 Hz subpage、成对 meta node；现有统计基线 + 少量逐 stream 增量；L1 timeout + `vb2_queue_error()` + 用户态 reopen；部署/长稳脚本 | 三路 rail 软件时序、官方 Chip ID、Host 透明重启、32/64 Hz MLX、LED、RKNN/MPP | IMX415 原厂/自研 A/B；MLX EEPROM/双页/ABI；并采 2 h；100 次启停；故障后无 Buffer 泄漏/双完成 |
| 完整版 | LED 安全驱动；500/1000 次启停和 12/24 h；受控 I2C/CSI/慢 DQBUF 注错；条件性 L2 reset work；双模态时间同步；可选 MPP | 没有证据的 L3 “无感”；未复现问题的核心 DMA 重写 | 原始/修复 A/B、竞态矩阵、功耗温升、回归、多路/PM/close/remove 交叉测试 |

## 6. 项目二逐条可行性矩阵

### 6.1 矩阵

| ID/初稿工作 | 按原文可实现 | 当前源码已有能力 | 仍需自行开发 | 额外硬件/资料 | 主要风险 | 必须取得的证据 | 建议 |
|---|---|---|---|---|---|---|---|
| P2-0 标题、个人独立、技术栈 | **作为目标可行，当前无项目代码** | 媒体/NPU库齐全 | 整个业务应用、状态机、标定和测试 | 项目一双模态硬件 | 组件清单被误当成果 | 应用仓库、架构图、线程/Buffer ownership、性能数据 | 项目一 V1 未完成前不要把项目二列为完成项目 |
| P2-1 “IMX415 4K + 32x24 并采，统一 CLOCK_MONOTONIC 最近邻匹配和延迟统计” | **可实现** | RKCIF/RKISP queue 已标 monotonic；MLX 节点可同样设置 | 两路采集器、有界 timestamp deque、匹配窗口、丢弃策略、时钟/延迟统计 | MLX 模组和固定安装 | IMX timestamp 是 SOF，MLX 成对帧有两个 subpage 时刻；不同曝光中心造成系统偏差 | 两路 timestamp 定义、直方图、匹配误差 P50/P95/P99、队列深度和 drop | 保留；把 MLX 帧时间定义为两页中点/第二页完成并同时保留两页时间，写出最大配对窗 |
| P2-2 “用户态 EEPROM 解析、温度计算、坏点、滤波、伪彩；双目标定区域映射” | **可实现且边界合理** | 官方算法可移植；OpenCV calib3d/imgproc 已安装 | 参数解析/缓存、发射率/反射温度配置、计算、滤波、伪彩、标定和误差评估 | BAA/BAB FOV、刚性支架、跨光谱标定靶、距离标尺 | 32x24、噪声、发射率、坏点、视差；普通棋盘格热端不可见 | EEPROM 参数单测、黑体/参考温计对比、FOV/畸变、标定文件、不同距离的 ROI/质心/IoU 误差 | 保留“区域级映射”；删除“像素级精准对齐”。写清适用距离和误差指标 |
| P2-3 “RGA 预处理 + RKNN 人员检测 + 热源融合” | **可实现，需性能闭环** | RGA fd import；RKNN fd memory；RK3588 YOLOv5 640 模型示例存在 | 摄像头输入、模型许可/类别、letterbox/量化、后处理、热 ROI fusion、跟踪 | 真实昼夜数据集；必要时重训练模型 | 4K 不是模型输入；坐标反变换；NPU fd/stride不匹配；低照误检；热源非人体 | RKNN/API/driver版本、model hash、mAP/precision/recall、FPS/latency、RGA/NPU profile、融合消融实验 | 改成“对 4K 画面选区并缩放至模型输入，在 RKNN 部署人员检测”；不要暗示 4K 全分辨率推理 |
| P2-4 “低照补光状态机，PWM 调光，目标后补光/提频/事件录像” | **算法可行，硬件未满足** | Linux PWM、应用定时器/线程；RKADK 事件录像可参考 | 状态机、滞回/冷却、亮度/热目标判据、PWM backend、录像触发 | 白光 LED、恒流/MOSFET、电源、散热、选定 PWM pin | PWM 资源冲突；光照导致曝光震荡；LED 过热；频繁触发；夜间眩光 | 电路图/BOM、默认关、波形/电流/温升、状态转移日志、抗抖、故障安全、录像文件 | 保留为完整版；V1 可先用 GPIO 低功率指示灯/软件 mock 验证状态机，随后再接功率 LED |
| P2-5 “DMA-BUF 可见光链路，减少 RKISP/RGA/RKNN/MPP CPU 拷贝；有界多线程流水线” | **前半句可实现；若理解为全程零拷贝则过度** | 每个组件均有 fd/import 接口 | 多池 Buffer、ownership、fence/cache、backpressure、drop policy、线程退出 | 无新增硬件，需厂商 API 与板端版本核对 | 格式/stride/tensor不匹配；CPU OSD；过早 QBUF；死锁；隐式同步延迟 | fd 流转日志、无全帧 memcpy profile、DMA-BUF sync/fence、队列水位、端到端延迟、码流正确性 | 改成“基于 DMA-BUF 复用 Buffer，消除可见光主路径的 CPU 全帧复制”；不要写“全程零拷贝” |
| P2-6 “MPP H.264/H.265 + RTSP；OSD；事件录像；指标” | **可实现，集成量较大** | MPP、RKADK RTSP/OSD/muxer、FFmpeg/GStreamer/Live555、MediaMTX 均存在 | 选择单一架构、编码参数、RTP/RTSP、OSD布局、预录环、指标与异常处理 | 客户端/网络、存储介质 | 4K30 编码/OSD带宽、GOP起播、码流拷贝、断网、磁盘满、音视频时间戳 | ffprobe/解码帧数、码率/FPS、RTSP多客户端/重连、Glass-to-glass、OSD截图、预录前后时长、磁盘故障 | 第一版先 H.264 或 H.265 二选一、单客户端 RTSP、小 OSD；完整后再写双编码和事件录像 |

### 6.2 项目二第一版和完整版边界

| 版本 | 纳入 | 明确不纳入 | 验收门槛 |
|---|---|---|---|
| V1 | IMX415 RKISP NV12 + MLX 成对帧并采；monotonic 最近邻匹配；用户态温度/坏点/简单时域滤波；固定距离区域级映射；RGA 到 640x640；单人员模型；热 ROI 规则融合；单编码格式；RTSP；简单 OSD；软件 mock LED | 功率 LED、像素级对齐、4K tensor 推理、全程零拷贝、复杂追踪、预录/掉电恢复 | 2 h 并采、匹配 P95、温度参考误差、区域误差、模型/融合指标、端到端 FPS/延迟、RTSP 重连 |
| 完整版 | 安全 PWM LED；多距离标定/视差模型；异步 Buffer 池和 fence；H.264/H.265 配置切换；OSD/热图；事件预录；多客户端；12/24 h；功耗温升 | 无量化数据的“精准”“无感”“零拷贝”绝对表述 | 性能/准确率/功耗/温升/网络/存储故障矩阵，所有简历数字可由原始日志复算 |

## 7. 热阵列内核与用户态职责、V4L2 ABI 建议

### 7.1 职责边界

| 内核驱动负责 | 用户态负责 |
|---|---|
| I2C 16-bit address/word 传输、重试和 errno；EEPROM/raw RAM 读取；状态位清除；刷新率/Chess 配置；subpage 校验和成对；稳定二进制 ABI；VB2 queue；MMAP/poll；sequence/monotonic timestamp；stop/error 回收；设备 PM | EEPROM 校准参数解析；Vdd/Ta/To 公式；发射率、反射温度；坏点补偿；时域/空间滤波；伪彩；温度阈值；标定/融合；算法版本管理 |

理由：温度计算包含大量浮点、策略参数和可升级算法，不应固化在 Linux 内核。内核只提供可复现、带时序语义的原始测量；这样既保持 ABI 稳定，也便于用 Melexis API 或独立实现做 A/B 单测。

### 7.2 推荐 V4L2 节点和 Buffer 格式

第一版优先使用独立 `V4L2_BUF_TYPE_META_CAPTURE` 节点，而不是冒充普通灰度视频：

- 私有 dataformat，例如 `v4l2_fourcc('M','L','X','4')`，在项目 UAPI 头中明确定义；`v4l2_meta_format.buffersize` 固定为 ABI v1 大小。
- EEPROM 校准区应通过只读 nvmem provider（可复用现有通用驱动思路）或单独的版本化校准 ABI 在初始化时读取一次；不要把 1664-byte EEPROM 逐帧塞进采集 Buffer，也不要暴露无保护的 EEPROM 写接口。
- 每个 DQBUF 表示一对连续且 subpage ID 不同的快照，用户态据此计算一个完整 32x24 温度矩阵。
- payload 使用固定宽度、固定 endian 类型；建议统一 little-endian UAPI，并在 I2C big-endian word 读入后转换。
- payload 必须有 `magic`、`abi_version`、`header_size`、`payload_size`、flags、pair sequence、两页 subpage ID/状态/控制寄存器、两页 monotonic 时间戳和两份官方 834-word frame snapshot，不能依赖 C 结构体隐式 padding。
- `v4l2_buffer.sequence` 表示完整 pair 序号；`timestamp` 可定义为第二页完成时刻，payload 另保留两个 subpage 时间，算法可计算中点和页间隔。
- flags 至少表示 duplicate/missing subpage、I2C retry、aux validation、stale pair 和 sensor reconfigure generation。

不建议直接使用 `V4L2_PIX_FMT_Y16`：MLX raw word 不是 16-bit 线性灰度或温度图，且完整计算还依赖 aux、控制、状态和 subpage 语义。若为了通用图像工具另提供 Y16 节点，应由用户态输出量化温度图，并定义比例/offset；原始内核 ABI 仍保留 meta node。

### 7.3 VB2 状态机要求

```text
REQBUFS/QBUF
  -> vb2 将 Buffer 交给驱动
  -> 驱动在自有 queued list 持有
  -> 采集线程等 new-data，读取并组成 0/1 pair
  -> 取一个 queued Buffer，填充 payload/sequence/timestamp
  -> vb2_buffer_done(DONE)

STREAMOFF/remove/suspend
  -> 先阻止新调度
  -> cancel_work_sync/kthread_stop
  -> 再从驱动 list 摘除所有 Buffer
  -> 锁外逐个 vb2_buffer_done(ERROR)

不可恢复 I2C/协议错误
  -> 记录错误快照
  -> vb2_queue_error()
  -> 唤醒用户态
  -> stop_streaming 回收所有未完成 Buffer
  -> 用户态 STREAMOFF/close/reopen
```

`vb2_queue_error()` 只把 queue 标成错误并唤醒等待者，不替代驱动的 Buffer 回收。不能在 timeout work 和 STREAMOFF 两条路径同时对同一 Buffer `vb2_buffer_done()`。

## 8. RKCIF 恢复分层和回退方案

| 等级 | 实现 | 风险 | 第一版回退 |
|---|---|---|---|
| L0 观测 | 统计、last frame、事件快照、trace、用户态 watchdog | 低 | 关闭 debug Kconfig/trace，回原厂行为 |
| L1 错误传播 | generation-aware delayed work；超时一次性 `vb2_queue_error()`；stop/close/remove/suspend 同步取消；用户态 STREAMOFF/close/reopen | 中低 | 仅报警，不 queue_error；或禁用 timeout 参数 |
| L2 厂商 reset work | 在审计原有 watchdog/reset work 后条件触发 RKCIF/CSI reset，并验证 Buffer/PM/多 stream | 高 | 回到 L1 用户态重开 |
| L3 Host 透明重启 | 内核跨 Sensor、CSI Host、RKCIF、RKISP、VB2、runtime PM 保持应用无感 | 极高 | **不作为本项目承诺** |

“RKCIF 自动无感恢复”只有在以下证据全部具备后才可能写入：可控故障、恢复前后 Buffer ownership 守恒、无 double-done/UAF、所有等待者被唤醒、stream generation 隔离、close/remove/suspend/并发 STREAMOFF 通过、PM 引用归零、多次恢复后图像/sequence 正常、长稳无退化。当前不具备这些证据。

## 9. 双目标定和精度边界

IMX415 模组规格给出的可见光镜头视场角约 86°；MLX90640 有 BAA `110°x75°` 和 BAB `55°x35°` 两种 FOV。未确认具体热阵列型号前，连覆盖关系都不能固定。

限制：

1. 热阵列只有 32x24。即使上采样到 4K，每个热像素仍覆盖很大的角区域，上采样不会创造空间信息。
2. 两个传感器光心不同。单个 homography 只对近似平面或固定距离有效；目标距离变化会产生视差，近距离最明显。
3. 普通可见光棋盘格在长波红外中通常没有足够对比，需要加热棋盘、不同发射率材料或可同时被两种传感器观察的标定靶。
4. 温度还受发射率、反射温度、窗口材料、热漂移、坏点和噪声影响；热源轮廓不是精确人体轮廓。
5. 机械支架必须刚性固定。重新安装、镜头对焦或外壳遮挡后应重新标定。

合理成果应称“区域级空间映射/ROI 对应”，并给出适用距离，例如 2~5 m。推荐指标是热目标质心角误差、映射到可见光后的像素误差分布、ROI IoU/覆盖率和不同距离的 P50/P95；不使用“像素级精准对齐”。

第一版可采用固定距离平面 homography；完整版再做可见光内参、热阵列等效内参、外参和按距离分段/深度辅助的视差补偿。

## 10. 五项高风险措辞的审计结论

| 原表述 | 结论 | 技术上诚实的替代措辞 |
|---|---|---|
| “完整三路 Sensor 电源时序” | **不成立**。ATK IMX415 模组只输入 3.3 V，三颗本地 LDO无软件 enable | “依据手册实现 RESET/PWDN/MCLK 与可控等待时序；三路本地 rail 由模组 LDO 生成，使用示波器核验而非软件独立控制” |
| “官方 Chip ID 识别” | **不成立**。`0x311A` 是 `INCKSEL4`，`0xE0` 是厂商驱动采用的复位签名 | “读取并校验厂商参考识别签名 `0x311A=0xE0`，明确其不是 Sony 公布的唯一 Chip ID” |
| “RKCIF 自动无感恢复” | **当前过度承诺**，原厂 monitor 也未在当前配置启用 | “实现帧超时检测、错误快照和 `vb2_queue_error()`，由用户态完成 STREAMOFF/reopen；条件性评估原厂 reset work” |
| “RKISP、RGA、RKNN、MPP 全程零拷贝” | **不成立**。可以消除 CPU 全帧 memcpy，但格式转换和设备 DMA 仍读写内存 | “基于 DMA-BUF 复用可见光 Buffer，减少 CPU 全帧复制，并显式管理 stride、cache、fence 和生命周期” |
| “热成像与可见光像素级精准对齐” | **不成立**，受 32x24、FOV、光心和视差限制 | “在限定距离内完成热阵列到可见光的区域级标定，量化 ROI/质心映射误差” |

## 11. 分阶段实施与验收顺序

### M0：冻结基线和硬件清单

- 确认实物板版本、IMX415 插座、MLX90640 BAA/BAB 模组、I2C 电平/上拉、LED 参数和支架。
- 采集原厂 `media-ctl`、`v4l2-ctl`、AIQ、RAW/NV12、interrupt/procfs 基线。
- 验收：所有设备节点、DTS、模块和工具版本可追溯；原厂路径可随时恢复。

### M1：阶段一真机 I2C 最小识别

- 模组接 J20，`run nfsbootfdt`，加载 `zzh_imx415_minimal`，完成 20 次 probe/remove。
- 示波器核实 MCLK、RESET/PWDN，必要时看本地 rails。
- 验收：每次稳定读到参考签名、无资源错误、probe 后安全断电；随后才生成 `阶段一总结.md`。

### M2：隔离式 IMX415 完整导入

- 只做一个 3864x2192 RAW10 30 fps 线性 mode，完成 Subdev/pad/video ops、必要 Controls、runtime PM 和独立 graph。
- 与原厂 mode 表来源逐项标注，做原厂/自研 A/B。
- 验收：media graph、RAW pattern/crop/stride、RKISP NV12、100 次启停；不修改原厂节点和驱动。

### M3：MLX90640 电气与原始读取

- 先用用户态 `i2ctransfer`/小工具确认 `0x33`、EEPROM、status/control 和 subpage 交替；总线先固定 400 kHz。
- 验收：EEPROM 832-word 重复读取哈希一致；8/16 Hz 下无重复/缺页；逻辑分析仪或 adapter trace 证明 repeated-start、endian 和传输时长。

### M4：MLX90640 V4L2/VB2 节点

- 实现 meta ABI、pair、MMAP/poll/sequence/monotonic timestamp、I2C error 和 stop 回收。
- 验收：`v4l2-compliance` 相关项、1000 次 stream on/off、拔设备/总线 NACK（安全可行时）、所有 Buffer 守恒。

### M5：双路并采和区域级标定

- 实现 timestamp matcher、温度算法、坏点/滤波/伪彩和固定距离标定。
- 验收：2 h 并采；匹配误差分布；参考温度误差；2~3 个距离的 ROI/质心误差。

### M6：RKCIF 可观测性与 L1 恢复

- 先复现，再加逐 stream 观测；只有确认无帧且 queue 不报错时才加入 L1 timeout。
- 验收：开关观测的性能对比、可控故障、`POLLERR`/DQBUF 错误、用户态 reopen、stop/close/remove/suspend 交叉测试。

### M7：RGA/RKNN 融合

- 先 MMAP/CPU baseline，再做 DMA-BUF；固定 640x640 人员模型和单一 fusion 规则。
- 验收：model hash/version、RGA/RKNN FPS/延迟、fd ownership、无 4K 全帧 memcpy、可见/热/融合三组消融指标。

### M8：MPP、RTSP、OSD、事件录像和 LED

- 编码先 H.264 或 H.265 二选一；RTSP 单路；OSD 小区域；再加入预录和功率 LED。
- 验收：码流解码、30 min/2 h/12 h、网络重连、多客户端、预录从 IDR 开始、磁盘故障、LED 波形/电流/温升/默认关。

### M9：条件性完整版恢复和性能优化

- 仅在 L1 数据证明有收益时评估 L2；用实测定位拷贝和带宽，不追求口号式零拷贝。
- 验收：原厂/修改 A/B、1000 次启停、24 h、故障矩阵、CPU/DDR/NPU/RGA/MPP/温度，无原厂基线回归。

## 12. 面试前必须真实完成的证据清单

### 12.1 代码和设计

- 每个自研模块的独立目录、Kconfig/Makefile、commit 和 diffstat；原厂参考代码与自研代码文件级边界。
- 硬件契约表、Media Graph 图、MLX ABI 文档、线程模型、Buffer ownership 状态图、锁/上下文图和恢复代际设计。
- 每个关键常数的来源：原理图页、Sony/Melexis 手册章节或厂商参考代码；尤其是 `0x311A=0xE0` 的非官方性质。
- 可一键恢复到原厂节点/驱动的配置方法；debug/recovery 均有 Kconfig 或运行时关闭入口。

### 12.2 板端基础日志

- U-Boot `printenv`、TFTP Image/DTB 文件名和地址、`run nfsbootfdt` 完整启动片段。
- `/proc/cmdline`、NFS 根挂载、`/etc/rootfs-source`、内核版本和运行 DT compatible。
- IMX415 原厂/最小/自研三种路径的 probe、MCLK/GPIO、Media Graph、format/crop/stride、stream 命令和帧 hash。
- MLX90640 地址、EEPROM hash/ID、控制寄存器、subpage 0/1、实际总线频率和一次批量读耗时。

### 12.3 正确性和可靠性

- `v4l2-compliance`、MMAP/DMABUF、poll/nonblock、sequence/timestamp、Buffer state 和异常退出日志。
- 至少 100 次第一版、500/1000 次完整版启停；至少 2 h 第一版、12/24 h 完整版连续运行。
- 少 QBUF、慢 DQBUF、消费者退出、I2C NACK、可控 CSI 中断/断流、STREAMOFF 与 timeout 同时发生、suspend/remove 的矩阵。
- 每次测试的命令、git commit、DTB/Image/module hash、起止时间、帧数、drop、错误、恢复次数和退出码，而非截取几行“成功”日志。

### 12.4 算法和系统性能

- MLX 温度与参考温计/黑体的误差分布；发射率和反射温度设置；坏点/滤波前后对比。
- 标定靶、两相机姿态、FOV、适用距离、标定文件 hash、ROI/质心/IoU P50/P95。
- 模型来源/许可证/hash、RKNN toolkit/runtime/driver 版本、输入/输出 tensor、量化参数、人员类别指标。
- CPU、内存、CMA/IOMMU、RGA/NPU/MPP 利用率、DDR、温度/频率、端到端延迟和队列水位。
- DMABUF fd 流转与 cache/fence 证据；`perf`/profile 证明没有逐帧 4K 全图 CPU memcpy。
- 编码码率/GOP/FPS、ffprobe、解码帧数、RTSP 起播/重连/多客户端、事件预录前后时长和文件完整性。

没有这些证据时，面试官对“稳定”“恢复”“零拷贝”“精准”“4K30”的追问无法闭环。

## 13. 推荐简历项目描述

以下两份是**完成对应 V1 验收后**的推荐模板，不代表截至本审计日已经完成。方括号中的数字必须替换为真实日志结果；未完成对应证据前不得使用该条。当前立即可安全写入简历的只有 SDK 构建/网络启动，以及“完成阶段一独立最小驱动和 DTB 的代码及构建，待真机验证”，后者不能写成板端通过。

### 13.1 项目一推荐稿（V1 验收后使用）

**基于 RK3588 的双模态成像 BSP 适配与采集链路可靠性开发**  
个人独立开发｜ATK-DLRK3588｜Linux 5.10.209  
技术栈：C、Device Tree、I2C、PWM、V4L2、Videobuf2、RKCIF、RKISP、debugfs、workqueue

- 完成 Rockchip SDK 全量构建与镜像校验，单独更新 U-Boot 分区并打通 TFTP 加载 Image/固定 1080x1920 DTB + NFS RootFS 的驱动迭代链路，保留 eMMC 默认启动和原厂固件回退。
- 基于原理图和 Sony 手册，在独立 `compatible`/DTS 路径实现 IMX415 从 I2C 参考签名、3864x2192 RAW10 30 fps mode、V4L2 Subdev 到 Sensor-RKCIF-RKISP Media Graph 的分阶段导入；保留原厂驱动并完成 A/B 验证，明确 `0x311A=0xE0` 为厂商参考签名而非官方唯一 Chip ID。
- 审计内核既有 `video-i2c` MLX90640 路径的 Y16_BE 32x26、bulk-read、nvmem 和 VB2 行为，在独立实现中新增 400 kHz 分段读取、只读 EEPROM 缓存接口及版本化 V4L2 meta ABI，完成 Chess 双 Subpage 成对、MMAP/poll、sequence、monotonic timestamp 及 STREAMOFF/错误 Buffer 回收；在 16 Hz subpage 设置下输出约 8 Hz 完整热阵列帧。
- 基于既有 RKCIF `irq_stats`/procfs，补充逐 stream 的 SOF/FE、DMA slot、VB2 done、no-buffer、last-frame 和 timeout 事件快照，建立 IRQ-DMA-tasklet-VB2 的可关联故障定位链路，观测开启后的 CPU/IRQ 增量为 `[实测值]`。
- 实现 generation-aware 帧超时检测和 `vb2_queue_error()` 错误传播，由用户态完成 STREAMOFF/close/reopen 恢复；通过 `[次数]` 次启停、`[时长]` 并行采集和 `[故障类型]` 注入验证无 double-done、Buffer 泄漏及 stop/recovery 竞态。

### 13.2 项目二推荐稿（V1 验收后使用）

**基于 RK3588 的可见光-热成像协同感知与低照度预警系统**  
个人独立开发｜RK3588 异构计算平台  
技术栈：C/C++、V4L2、DMA-BUF、RGA、RKNN、MPP、RTSP、OpenCV、多线程

- 并行采集 RKISP NV12 可见光帧与 MLX90640 32x24 成对热阵列帧，统一使用 `CLOCK_MONOTONIC`，以有界时间队列完成最近邻匹配；实测配对误差 P50/P95 为 `[数值]`，超窗帧按可追踪策略丢弃。
- 在用户态完成 MLX90640 EEPROM 参数解析、发射率/反射温度补偿、温度矩阵、坏点修复、时域滤波和伪彩；使用跨光谱标定靶在 `[距离范围]` 内实现热 ROI 到可见光画面的区域级映射，质心/IoU P95 误差为 `[数值]`。
- 使用 RGA 将 4K 可见光选区裁剪、缩放并转换为模型所需 640x640 RGB tensor，通过 RKNN 部署人员检测模型；设计检测框与热 ROI 的规则融合，相比单可见光基线将 `[指标]` 从 `[A]` 提升到 `[B]`。
- 构建基于 DMA-BUF 的多 Buffer 池和有界线程流水线，显式管理 V4L2、RGA、RKNN 与 MPP 的 fd、stride、cache、fence 和生命周期，消除可见光主路径逐帧 4K CPU 全图复制；端到端 FPS/P95 延迟为 `[数值]`。
- 使用 MPP 完成 `[H.264或H.265]` 硬件编码并向 RTSP 服务发布，叠加检测框、温度和告警 OSD，支持带 `[秒]` 预录的事件文件；通过 `[时长]` 运行、断网重连、磁盘满和多客户端测试，记录码率、丢帧、队列水位与温升。
- 在外置恒流/MOSFET 驱动电路上实现默认关闭、带滞回和冷却时间的 PWM 补光状态机；依据画面亮度和持续热目标切换补光/推理频率/录像状态，并完成占空比、电流和温升验证。此条只有功率硬件验收后保留。

## 14. 原厂回退方法

1. 保留 `kernel/drivers/media/i2c/imx415.c`、原厂 camera DTS、IQ 文件和所有厂商 Kconfig；自研路径使用独立 compatible/选项。
2. 网络开发回退时部署不 include 实验覆盖的原厂 `rk3588-alientek-2mipi1080x1920-2hdmi.dtb`，仍用 `run nfsbootfdt`；完全回退可不打断 U-Boot，直接让默认 bootcmd 进入 eMMC 原厂系统。
3. RKCIF 统计、timeout 和 recovery 分别有 Kconfig/运行时开关；先关闭 recovery，再关闭新增观测，确认可恢复到 byte-for-byte 的原厂数据路径行为。
4. MLX90640 和 LED 是外部模块，回退时先关闭 PWM、确认负载断电，再断开模块并移除实验 DTS/模块；不能带电插拔未知接线。
5. 用户态媒体链路保留 MMAP/CPU baseline；DMA-BUF、RKNN、MPP 或 RTSP 任一不稳定时按 `MPP -> RKNN -> RGA -> 双模态` 逆序移除，回到单路 RKISP NV12 采集。

## 15. 仅靠源码无法确认的事项

1. 实物 IMX415 插在 J18/J19/J20/J21 的哪一个连接器，以及当前线缆、模组 PCB 版本和实际 RESET/PWDN 电平。
2. 阶段一 `0x311A=0xE0` 是否在目标板稳定读取；MCLK 和本地 AVDD/DOVDD/DVDD 的真实波形。
3. 原厂和后续自研路径实际生成的 `/dev/mediaX`/`/dev/videoX`、RAW compact stride、RKISP NV12 节点、online/readback 模式和 AIQ 行为。
4. 是否已经购置 MLX90640；具体为 BAA 110x75° 还是 BAB 55x35°；模块上拉、稳压、电平、EEPROM 内容、坏点和真实最大稳定刷新率。
5. JP5 I2C5/I2C6 在实物上的上拉阻值、总线电容、当前挂载器件和 400 kHz 波形质量。
6. 最终可分配给 LED 的 PWM 控制器/引脚是否与实际使用的摄像头、屏幕背光和风扇无冲突。
7. LED 的电压、电流、功率、驱动拓扑、散热和光学安全；目前没有任何外部补光硬件证据。
8. 当前板端 librga、RKNN runtime/driver、MPP、GStreamer 插件的运行版本、支持格式、fd import、cache/fence 和性能。
9. 4K30 RKISP NV12、RGA、RKNN、MPP 同时运行时的 CMA/IOMMU、DDR、CPU、NPU、RGA、温升和降频。
10. RTSP 多客户端、网络断连、OSD 和事件录像是否满足目标延迟/可靠性。
11. 可见光与热阵列的机械基线、FOV 重叠、标定靶、目标距离和可达到的区域映射误差。
12. RKCIF 当前是否存在可稳定复现的 frame timeout；若没有复现，不能声称完成修复或恢复率提升。

## 16. 开发板/实物最小信息采集命令

以下命令只用于采集事实；I2C bus 编号必须先根据 sysfs/运行 DT 确认。未知接线、未知电压或功率 LED 未经万用表/原理图确认前不要上电。

### 16.1 U-Boot 与网络启动

```text
printenv bootcmd nfsboot nfsbootfdt nfsimage nfsfdt ipaddr serverip netmask kernel_addr_r fdt_addr_r
run nfsbootfdt
```

保留从 TFTP 开始到 Linux 挂载 NFS 根的完整串口日志，不只截取最后几行。

### 16.2 Linux 基线、运行 DT 和模块

```sh
uname -a
cat /proc/cmdline
mount | grep ' / '
cat /etc/rootfs-source
ip addr show eth0
tr -d '\0' </proc/device-tree/model; echo

find /sys/bus/i2c/devices -maxdepth 1 -type l -name 'i2c-*' -print -exec readlink -f {} \;
i2cdetect -l
lsmod
dmesg | grep -Ei 'imx415|rkcif|rkisp|mipi|csi|i2c|iommu|dma'
```

### 16.3 IMX415 与 Media Graph

```sh
v4l2-ctl --list-devices
for m in /dev/media*; do echo "===== $m ====="; media-ctl -d "$m" -p; done
for v in /dev/video*; do echo "===== $v ====="; v4l2-ctl -d "$v" --all; done

cat /sys/kernel/debug/clk/clk_summary | grep -Ei 'mipi.*camera|cameraout|mclk'
grep -Ei 'rkcif|csi|mipi|isp' /proc/interrupts
find /proc -maxdepth 2 -iname '*rkcif*' -o -iname '*rkisp*'
```

阶段一实验节点：

```sh
modprobe zzh_imx415_minimal
dmesg | grep -Ei 'zzh_imx415_minimal|reference signature|0x311a'
readlink -f /sys/bus/i2c/devices/3-001a/driver
cat /sys/bus/i2c/devices/3-001a/name
```

### 16.4 MLX90640 最小只读识别

先把下面的 `5` 替换为确认后的 I2C5/JP5 对应 Linux bus；若选 I2C6 则相应替换。该总线与触摸/Type-C/RTC 等设备共享，不执行整总线 `i2cdetect -y` 扫描；仅在接线、电压、上拉和 `0x33` 地址确认后，对 MLX90640 已知只读寄存器做定点读取。下面 `w2` 只是发送 16-bit 寄存器地址，不改写寄存器值。

```sh
# 读 status 0x8000、control 0x800D、EEPROM ID words 0x2407..0x2409
i2ctransfer -y 5 w2@0x33 0x80 0x00 r2
i2ctransfer -y 5 w2@0x33 0x80 0x0d r2
i2ctransfer -y 5 w2@0x33 0x24 0x07 r6
```

随后用专用只读工具分段读取 EEPROM `0x2400..0x273F` 和 RAM，而不是把 1664-byte 命令手工展开；记录每段长度、耗时、返回码和 SHA-256。不要在未设计写保护和掉电风险前改写 EEPROM 地址/配置。

### 16.5 PWM、设备节点和媒体运行库

```sh
find /sys/class/pwm -maxdepth 3 -type f -print -exec sh -c 'printf "  "; cat "$1" 2>/dev/null' sh {} \;
ls -l /dev/video* /dev/media* /dev/rga /dev/rknpu /dev/dri /dev/dma_heap 2>/dev/null

mpp_info_test
ffmpeg -version | head
gst-inspect-1.0 | grep -Ei 'mpp|rockchip|rtsp|h264|h265'
mediamtx --version
ldconfig -p 2>/dev/null | grep -Ei 'rga|rknn|rockchip_mpp|opencv'
```

还需人工记录：板卡和模组正反面清晰照片、连接器丝印、万用表电压、示波器 MCLK/RESET/PWDN/PWM、逻辑分析仪 I2C、LED 电流与连续点亮温升、双传感器刚性安装尺寸及标定距离。这些信息不能由源码替代。

---

最终审计结论：两个项目都可以成为有含金量的 RK3588 项目，但前提是把厂商已有基础、个人隔离重做和生产增量严格分账。项目一应先完成真机最小识别、独立单 mode 导入、MLX 原始 ABI 和 L1 可靠性闭环；项目二再建立在该稳定接口上。任何没有代码、原始日志、测试数字和可回退实现支持的完成时态，都应从简历中删除。
