# 基于 RK3588 的 IMX415 MIPI 双模态成像 BSP 适配与采集链路可靠性开发

> 当前进度：P1.6-A IMX415 4K NV12 + MLX90640 ZMLX Meta双路并采基线通过；进入RKCIF可观测性与L1错误传播  
> 目标平台：ATK-DLRK3588，Rockchip Linux 5.10.209  
> 项目关系：本项目独立开发；项目二使用本项目验证通过的稳定接口

本目录保存项目的独立驱动、测试工具、验证记录和设计文档。厂商内核、DTS、
U-Boot 修改仍位于 SDK 原有目录，不在此处复制或迁移。

源码教学镜像入口：`详细注释版/README.md`。注释版不参与正式构建，功能修改
仍以正式驱动、内核 DTS 和部署脚本为准。

## 1. 项目概述

以下内容用于说明项目完成后的整体范围，不代表截至当前已经全部实现。当前实际
进度以第 2 节为准；只有取得代码、真实板端日志和量化测试结果后，相应内容才会
作为已完成成果使用。

**基于 RK3588 的 IMX415 MIPI 双模态成像 BSP 适配与采集链路可靠性开发**

个人独立开发｜ATK-DLRK3588｜Linux 5.10.209

技术栈：C、U-Boot、Device Tree、I2C、PWM、V4L2、Videobuf2、RKCIF、RKISP、debugfs、tracepoint、workqueue

- 基于 Rockchip Linux 5.10 SDK 完成 Loader、U-Boot、Kernel、DTB 及 Buildroot RootFS 构建，打通“TFTP 加载 Image/DTB + NFS RootFS”网络启动调试链路，并保留 eMMC 原厂启动和镜像回退路径；通过调优 TFTP Blocksize 并启用 U-Boot IP 分片重组，将 36 MiB Image 下载速度由约 1.4 MiB/s 提升至 4.1 MiB/s。
- 基于原理图和厂商参考实现，独立完成 IMX415 驱动及设备树适配，实现 I2C 识别、MCLK、RESET/PWDN 控制和 3864×2192 RAW10 30 fps 采集，并接入 V4L2 Subdev 及曝光、增益、VBLANK 控制；完成 Sensor-RKCIF-RKISP 媒体链路配置，通过独立 compatible 和 DTB 保留原厂方案进行 A/B 验证。
- 参考并分析内核 `video-i2c` 中的 MLX90640 实现，完成独立热成像采集驱动，采用 400 kHz 分段 I2C 读取并缓存 EEPROM 校准数据，实现 Chess 模式双 Subpage 配对组帧；基于 V4L2 Meta/VB2 向用户态输出热成像数据，支持 MMAP、poll 和时间戳，并完善 STREAMOFF 及异常路径下的 Buffer 回收。
- 针对采集卡死和丢帧问题，在 RKCIF 中补充 FS/FE、DMA Buffer 切换、VB2 完成及无可用 Buffer 等 debugfs 统计与 tracepoint 跟踪；增加带采集代际校验的帧超时检测，通过 `vb2_queue_error()` 向用户态传播异常，避免残留超时任务影响新的采集会话，并验证用户态关闭、重开后的采集恢复。
- 完成 IMX415 与 MLX90640 双路并行采集及白光 LED GPIO/PWM 控制，并编写自动化部署、故障注入和稳定性测试脚本；覆盖重复 STREAMON/OFF、慢速 DQBUF、I2C 异常及长时间并采等场景，验证 Buffer 回收、错误传播和异常退出后的重新采集能力。

## 2. 当前进展

| 能力 | 当前状态 | 已有依据 | 后续工作 |
|---|---|---|---|
| Loader/U-Boot/Kernel/DTB/RootFS 构建 | 已验证 | `docs/sdk-bringup/phase0_sdk_full_build_report.md` 的构建记录、产物和哈希 | 保留原始构建日志和版本信息 |
| TFTP Image/DTB + NFS RootFS | 已验证 | `docs/sdk-bringup/phase1_tftp_nfs_report.md`；实测 1.4 -> 4.1 MiB/s；`run nfsbootfdt` 启动闭环 | 后续每次测试记录 Image/DTB hash 和 `/proc/cmdline` |
| IMX415 最小 I2C 识别 | 已通过真机功能验证；波形/实物证据待补 | 独立 `zzh,imx415-minimal`、实验 DTS、外置模块；NFS 实启动；原厂同地址节点 disabled；20/20 次 remove/probe 均读得 `0x311A=0xE0`，`VERIFY_RC=0`；`docs/阶段一总结.md` 和 `evidence/stage1/2026-08-02/` | 补齐 J20 照片、完整串口捕获与必要的电源/MCLK/GPIO 波形；对外始终称“厂商参考签名” |
| IMX415 固定 mode/Subdev/Controls/runtime PM | 已通过板端验收 | `docs/阶段二总结.md`；新内建驱动、固定 1080x1920 NFS DTB、Subdev/Media graph、Controls、10/100 轮短流和 PM 证据 | 寄存器 readback、物理波形和原厂 A/B 作为补充审计 |
| IMX415 Media Graph/RKCIF RAW/RKISP NV12 | P1.3 目标路径板端验收通过 | `docs/阶段三总结.md`；GB10/NV12 单帧、300/3000 帧，sequence 无断号、RKISP ErrCnt=0；曝光/增益亮度变化与 VBLANK 15/30 fps 动态验证 | ISP IQ/标准色卡画质和 Host 故障恢复保留到具备相应标定与故障门禁时 |
| MLX90640 原始传输 | P1.4 板端功能验收通过；仪器波形不纳入当前验收，FOV型号在项目二标定前确认 | `docs/阶段四总结.md`；`0x33`、EEPROM 20/20 同哈希；400 kHz；8/16 Hz 各20个正式 Subpage，0交替/周期/数据错误；控制寄存器恢复 | P1.5 冻结版本化 V4L2 Meta/VB2 ABI；项目二跨光谱标定前确认精确FOV型号 |
| MLX90640 V4L2 Meta/VB2 | P1.5 built-in正式验收通过 | `docs/阶段五总结.md`、`evidence/stage5/2026-08-24/builtin/`；自动probe、1664-byte NVMEM黄金哈希、`ZMLX` 3400-byte、200对/10周期、control恢复和共享总线回归通过 | 向P1.6/项目二提供稳定热阵列ABI；后续故障注入继续复用该接口 |
| RKCIF 增量观测和 L1 恢复 | 未实现 | 原厂 IRQ/procfs/reset 框架存在 | 独立 diff、trace/debugfs 数据、可控故障、Buffer 守恒和用户态重开 |
| LED GPIO/PWM | 需新增硬件 | 仅有板级候选引脚分析 | 外部恒流/MOSFET、电源/PWM 冲突表、波形、电流、温升和默认关 |
| 双路并采与可靠性矩阵 | P1.6-A双路基线通过；完整长稳/故障矩阵待做 | `docs/阶段六双路并采基线.md`；300 NV12+80 Meta与3000 NV12+800 Meta均PASS，可见光0 gap/0 bad bytes，MLX no-buffer/duplicate=0/0，RKISP ErrCnt=0 | 进入逐stream可观测性、trace、L1错误传播、2小时并采和故障矩阵 |

## 3. 开发计划

```text
P1.1  IMX415 最小 I2C 真机识别（已通过真机功能验证）
  -> P1.2  独立单 mode Sensor 驱动和 V4L2 Subdev（已通过）
  -> P1.3  独立 Media Graph、RKCIF RAW、RKISP NV12（已通过）
  -> P1.4  MLX90640 电气确认、原始读取和 EEPROM（板端功能验收通过）
  -> P1.5  MLX90640 V4L2 Meta/VB2 ABI（built-in板端验收通过）
  -> P1.6  双路并采、RKCIF 可观测性和 L1 错误传播（双路基线通过，当前阶段）
  -> P1.7  外置 LED GPIO/PWM 与完整可靠性矩阵
  -> 向项目二提供验证通过的接口
```

## 4. 项目文档

- `docs/项目台账.md`（连续开发记录）
- `docs/阶段一硬件契约.md`
- `docs/阶段一总结.md`
- `docs/阶段二实施计划.md`
- `docs/阶段二本地实现与构建记录.md`
- `docs/阶段二总结.md`
- `docs/阶段三总结.md`
- `docs/阶段四硬件契约与实施计划.md`
- `docs/阶段四总结.md`
- `docs/阶段五ABI设计与本地实现.md`
- `docs/阶段五总结.md`
- `docs/阶段六实施计划.md`
- `docs/阶段六双路并采基线.md`
- `docs/independent-imx415-bringup-plan.md`
- `docs/resume-project-feasibility-audit.md`
- `docs/feasibility-study.md`

## 5. 事实边界

- 厂商已有的 Sensor、Media Graph、RKCIF、RKISP、`video-i2c` 和媒体库不计入个人新增工作。
- 本地编译或部署成功不能替代真机 probe、出流或长稳证据。
- `0x311A=0xE0` 只称厂商参考识别签名，不称 Sony 官方唯一 Chip ID。
- 模组本地 LDO 不表述为软件完成三路独立电源时序。
- 第一版恢复止于统计、`vb2_queue_error()` 和用户态关闭/重开，不表述为 Host 无感恢复。
- DMA-BUF 目标是减少 CPU 全帧复制，不使用“全程零拷贝”。

## 6. 独立仓库与SDK复现

本目录已经作为独立Git仓库维护，不在SDK根目录初始化Git，也不复制完整厂商SDK。

| 项目 | 当前值 |
|---|---|
| GitHub | `https://github.com/zzh-debug/rk3588-dual-modal-camera-bsp` |
| 可见性 | Private |
| 分支/远端 | `main` / `origin` |
| 容器路径 | `/rk3588_dev/projects/rk3588_camera_bsp` |
| 宿主机路径 | `/home/zzh/workspace/rk3588_project/projects/rk3588_camera_bsp` |

仓库中的`sdk/`保存：

- kernel/U-Boot tracked patch；
- 新增DTSI和built-in包装器overlay；
- 网络启动部署脚本；
- 厂商release名和kernel/U-Boot基线commit；
- 可重复运行的`--check-only/apply`脚本。

仓库应克隆到匹配SDK的固定位置：

```text
<SDK_ROOT>/projects/rk3588_camera_bsp
```

检查和应用：

```sh
./sdk/apply_sdk_changes.sh --check-only <SDK_ROOT>
./sdk/apply_sdk_changes.sh <SDK_ROOT>
```

详细说明见`sdk/README.md`和`sdk/BASELINE.md`。远端仓库建议初期设为private；
公开前需要确定仓库级许可证，并复核实验网络地址、硬件照片和证据日志。

