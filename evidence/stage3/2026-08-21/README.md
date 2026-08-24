# P1.3 板端证据索引（2026-08-21）

## 环境

- 开发板：ATK-DLRK3588，Linux 5.10.209。
- 启动方式：U-Boot 执行 `run nfsbootfdt`，TFTP 加载 Image 和固定
  1080x1920 NFS DTB，Linux 根文件系统为 NFS v3。
- 网络：开发板 `192.168.5.20/24`，NFS/TFTP 服务器 `192.168.5.11`。
- 运行 DT：`zzh,imx415` 为 `okay`，同地址 `sony,imx415` 为 `disabled`。
- 设备映射：Sensor `/dev/v4l-subdev2`，RKCIF RAW `/dev/video22`，
  RKISP mainpath `/dev/video44`。
- TFTP Image SHA-256：
  `f76ad33c48520b79460425a204602936b986e706a7307ff99cd177d3d467eeb8`。
- TFTP NFS DTB SHA-256：
  `f9b6888a4be0bc19a53d6017d1d2473d2fc0afb04fd39f0e1049b8b9b257abdd`。

## 结果摘要

| 路径 | 测试 | 结果 |
|---|---|---|
| RKCIF GB10 | 单帧 | 10,506,240 bytes；SHA-256 `28e48f76dcb3995a8594fd8c9a641bbaff9d932d0b118e244a33c412de3398b3` |
| RKCIF GB10 | 300 帧 | 8 skip + 300 正式帧；通过 |
| RKCIF GB10 | 3000 帧 | 3008 DQBUF，sequence `0..3007`，0 gap，0 bad bytesused，约 100.50 s |
| RKISP NV12 | 单帧 | 12,441,600 bytes；SHA-256 `d3250f1cb1d73f8a796cc5db9d63dc2f85a4b1865dab039f99f5ee922c6e6b03` |
| RKISP NV12 | 300 帧 | 308 DQBUF，sequence `0..307`，0 gap，0 bad bytesused，30.00 fps |
| RKISP NV12 | 3000 帧 | 3008 DQBUF，sequence `0..3007`，0 gap，0 bad bytesused，100.54 s |

RAW 样本共有 256 种 byte 值，不是全零或单一常量；抽查第 1、2、3、100、
1000、2000 行哈希均不同。这里仅据此确认 Buffer 中存在变化数据，不把该统计
冒充 Bayer 排列、色彩或成像质量的主观验收。

NV12 单帧统计：Y 平面范围 12..255、均值 29.251、242 种 byte 值；UV 平面
范围 75..132、均值 123.877、58 种 byte 值。文件不是全零或单一常量。

## Controls

同一相对稳定场景下的 NV12 Y 平面统计：

| 设置 | Y 均值 | 样本 SHA-256 |
|---|---:|---|
| exposure=200, gain=0 | 14.235 | `7658d0eb4a7c62d2336516770aa675b69d47262293c977eac3c5e7697f62d363` |
| exposure=2000, gain=0 | 28.520 | `fc57046eaf352aff9c8d4ff9800ef40bc3a653aede5b2c600dd48e81ae367f22` |
| exposure=200, gain=120 | 110.091 | `edafdfb3fd0d26ca5bd35fb3d7c4fdd01e9f61f23912f7e7a1942810512539e5` |

VBLANK 从 58 增到 2308 后，38 个 DQBUF 的平均帧间隔为 66.667 ms
（15 fps）；恢复 VBLANK=58 后平均帧间隔为 33.333 ms（30 fps）。测试结束
已恢复 `exposure=2242, vertical_blanking=58, analogue_gain=0`。

所有采集结束后目标 RKCIF `dma_en=0x0`、Sensor runtime PM 为 `suspended`，
RKISP `ErrCnt=0`；本轮目标链路没有 overflow、timeout、IOMMU fault、DMA error、
Oops、BUG 或 panic。

## 文件

- `imx415-p13-board-snapshot.log`：启动环境、运行 DT、两级 media graph、格式、
  Controls、RKISP 状态和相关 dmesg 快照。
- `imx415-p13-raw300.log`、`imx415-p13-raw3000.log`：RKCIF GB10 DQBUF 日志。
- `imx415-p13-nv12-300.log`、`imx415-p13-nv12-3000.log`：RKISP NV12 DQBUF 日志。
- `imx415-p13-vblank2308.log`、`imx415-p13-vblank-restored.log`：VBLANK
  动态帧周期证据。
- `imx415-p13-stage3-script-smoke.log`：新增阶段三验收脚本以短参数在板端完整
  实跑的输出，结束标记 `STAGE3_RUNTIME_CHECK=PASS`。
- 其余 `imx415-p13-*.log`：单帧和 Controls 抓帧命令输出。

原始图像样本约 59 MiB，保存在本轮容器 `/tmp`，项目证据目录只保存哈希、
统计和可审计的 DQBUF 日志，避免把大体积一次性样本纳入源码目录。
