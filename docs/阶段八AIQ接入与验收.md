# 阶段八：IMX415 Rockchip AIQ/IQ 接入与真机验收

## 1. 结论

P1.8 已于 2026-08-26 在 ATK-DLRK3588 真机上完成。自研
`zzh_imx415` 不再只提供 RAW/NV12 采集能力，而是补齐 Rockchip AIQ
识别当前模组所需的最小私有 ABI 和设备树元数据，并复用原厂针对本模组/镜头的
IQ 文件：

```text
/etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json
```

最终使用 Linux 5.10.209 `#14`、NFS RootFS 和自研驱动完成 180 帧门禁，实际
收到 188 个 DQBUF；AIQ 进程在取流前后 PID 均为 615，脚本返回
`IMX415_AIQ_STAGE8=PASS`。启动日志不再出现 `failed to set hflip` 或
`failed to set hdr mode 0`。

## 2. 原问题与根因

阶段三虽然已经证明 Sensor-RKCIF-RKISP 可稳定输出 3840×2160 NV12，但自研
驱动当时没有实现 Rockchip AIQ 使用的模组信息 ioctl、兼容实体命名和 DTS
模组/镜头属性。因此“ISP 能出 NV12”不等于“AIQ 能按原厂规则选择正确 IQ”。

接入过程中还发现内核 UAPI 与 SDK 内 AIQ 私有副本的 `rkmodule_hdr_cfg` 尾部
布局不同。Linux ioctl 编号编码了结构大小，导致按内核头文件编译的工具调用
成功，而原厂 AIQ 的 `RKMODULE_SET_HDR_CFG` 仍不能命中。驱动最终按稳定的
ioctl 类型/编号识别该命令，只读取两个版本共同的首字段 `hdr_mode`，并继续只
接受当前驱动真正支持的线性 `NO_HDR`，没有虚构 HDR 能力。

## 3. 实现范围

| 位置 | 本阶段改动 |
|---|---|
| `driver/zzh_imx415.c` | 解析模组序号、朝向、模组名和镜头名；实体命名为 `m00_b_zzh_imx415 3-001a`；实现 `GET_MODULE_INFO`、`GET/SET_HDR_CFG`、`GET_CHANNEL_INFO`；增加 H/V Flip 控件及 `0x3030` 寄存器读改写；兼容 AIQ 私有头文件的 HDR ioctl 大小差异 |
| `sdk/overlay/kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-imx415-stage2.dtsi` | 增加 `rockchip,camera-module-index/facing/name/lens-name`，名称与现有原厂 IQ 文件严格对应 |
| `tools/verify_imx415_aiq_module.c` | 真机校验模组信息、GET/SET HDR 契约和 channel 0 格式 |
| `scripts/verify_imx415_aiq_stage8.sh` | 自动发现动态 media/subdev 节点，校验 NFS、AIQ、IQ 哈希和模块契约，执行 180 帧 NV12 取流并检查进程、DQBUF 和新增 fatal 日志 |

本阶段没有复制原厂 Sensor 驱动，也没有把 JSON IQ 文件提交进本仓库；IQ 文件
继续由原厂 RootFS 提供，本仓库只记录其文件名、哈希和选择证据。

## 4. 构建与静态门禁

| 项目 | 结果 |
|---|---|
| 驱动 `checkpatch --strict` | 0 error / 0 warning / 0 check |
| 验收工具 `checkpatch --strict` | 0 error / 0 warning / 0 check |
| 外置模块 `W=1` | 编译、MODPOST、链接通过，无 warning |
| SDK 复现检查 | `sdk/apply_sdk_changes.sh --check-only /rk3588_dev` 通过 |
| Kernel 分项构建 | `output/sessions/2026-08-26_22-51-36/` 成功，Linux `#14` |
| 构建日志归档 | `evidence/stage8/2026-08-26/build/` |

最终载荷：

| 产物 | SHA-256 |
|---|---|
| `Image` | `e9fbb471104b3f33b0cbf602c2eeb36625edb37f19274662189cb2002af0950c` |
| NFS DTB | `eda3cd4ab8612113e14dfba6f952187e4ad579df30fd43bae05167c9192009e9` |
| 原厂 IQ JSON | `7f0e3e3fc1cff63800297ccba9e5a1f81c4b7fc5007862deb19c21a88ef93d84` |

## 5. 真机结果

| 门禁 | 结果 |
|---|---|
| 启动与根文件系统 | Linux 5.10.209 `#14`；NFS 根为 `192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588` |
| 驱动绑定 | `3-001a -> zzh_imx415` |
| AIQ 实体 | `m00_b_zzh_imx415 3-001a`，本次为 `/dev/v4l-subdev2` |
| 模组契约 | sensor=`imx415`、module=`CMK-OT2022-PX1`、lens=`IR0147-50IRC-8M-F20`、NO_HDR、vc0、3864×2192、SGBRG10，PASS |
| IQ 选择 | AIQ 明确记录 `rk_aiq_uapi_sysctl_init success` 并选择目标 JSON |
| AIQ 运行 | `rk_aiq_uapi_sysctl_start success`；180 帧前后 PID 都为 615 |
| NV12 取流 | 请求 180 帧，日志中 188 个 DQBUF，脚本 PASS |
| 3A 活动证据 | 冷启动后首轮曝光 `203 -> 1216`、模拟增益 `0 -> 40`；最终归档轮曝光 `1216 -> 1334` |
| 兼容错误 | `#14` 当前启动无 hflip/vflip/HDR 设置失败日志 |

原始证据位于：

- `evidence/stage8/2026-08-26/kernel14-first/`
- `evidence/stage8/2026-08-26/kernel14-final/`
- `evidence/stage8/2026-08-26/build/`

## 6. 对项目二的影响与事实边界

项目二现在可以直接消费自研项目一输出的 RKISP NV12，并由原厂 AIQ/目标 IQ
负责基础 3A 和 ISP 调优，不必为了启动应用层开发切回原厂 Sensor 驱动。P2.1
的双路采集、时间匹配和后续 RGA/RKNN 流水线不再被“自研驱动未接 AIQ/IQ”阻塞。

本阶段证明的是“目标 IQ 被正确选择、AIQ 能工作、3A 控件会动态变化且 NV12
稳定出流”，并不等价于已经完成标准色卡、畸变、白平衡、噪声、动态范围或主观
画质标定。若项目二需要可量化画质指标，应另立色卡和场景测试，不把本次接入
门禁表述成新的 IQ 调参成果。
