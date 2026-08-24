# 阶段六 RKCIF 可观测性实现与待验收项

> 实现日期：2026-08-24（Asia/Shanghai）  
> 当前状态：**P1.6-B 已实现、编译并部署到 TFTP/NFS；等待开发板重新执行 `run nfsbootfdt` 后真机验收**

## 1. 本次个人增量

本次不修改厂商 RKCIF 的 DMA 地址轮换、reset/watchdog 或媒体拓扑，只在现有
IRQ、Tasklet 和 procfs 框架上增加逐 stream 关联信息：

| 增量 | 插点 | 行为 |
|---|---|---|
| 采集代际 | 首次进入一个新采集会话 | 每次新会话递增 `generation`，并清零该会话统计 |
| FS/FE | `rkcif_irq_pingpong_v1()` | 记录逐 stream 次数和最后单调时间戳 |
| DMA update | VICAP/ISP/Rockit 更新分支 | 记录次数、最后时间和 DMA mode |
| VB2 done | `rkcif_vb_done_oneframe()` | 记录次数、sequence 和 IRQ 到 VB2 完成延迟 |
| procfs 快照 | 厂商已有 `/proc/rkcif-*` | 在原只读文件末尾输出四个 stream 的一致性快照 |
| tracepoint | `rkcif:rkcif_stream_event` | 默认关闭；启用后输出 FS/FE/DMA/VB2 事件 |

统计使用独立的 `observe_lock`。IRQ/Tasklet 高频路径只做计数、时间读取、少量
字段更新和默认关闭的静态 tracepoint，不输出逐帧 printk。

## 2. 数据解释

RKISP online 路径中，RKCIF 把帧送给 ISP，而不是通过 RKCIF capture node 向
用户态完成 VB2 Buffer。因此预期：

```text
FS > 0
FE > 0
DMA > 0
RKCIF VB2 done = 0
```

这不是丢帧；用户态 NV12 Buffer 在 RKISP mainpath 完成。以后测试 RKCIF RAW
capture node 时，RKCIF 的 `vb2_done_count` 和 IRQ→VB2 延迟才应增长。

厂商已有的 `not_active_buf_cnt`、CSI overflow/size/bandwidth error 等统计继续
保留，不重复定义为个人新增能力。

## 3. 自动验收

新增脚本：

```sh
verify_rkcif_observability_stage6 300 80 /tmp/rkcif-observability-stage6
```

脚本会：

1. 启用 `rkcif:rkcif_stream_event`；
2. 调用原有双路并采门禁；
3. 在采集中读取 `/proc/rkcif-mipi-lvds2`；
4. 校验 stream 0 的 generation、FS、FE、DMA 和 online-path VB2 语义；
5. 校验 FS/FE/DMA trace 事件；
6. 保留 proc、trace、双路日志和汇总。

## 4. 构建与部署

| 项目 | 结果 |
|---|---|
| Kernel 分项编译 | PASS |
| 构建会话 | `output/sessions/2026-08-24_12-07-12` |
| 项目内构建日志 | `evidence/stage6/2026-08-24/observability/kernel-build.log` |
| SDK 复现检查 | `./sdk/apply_sdk_changes.sh --check-only /rk3588_dev` PASS |
| TFTP Image SHA-256 | `bf1f007b6f2df63f4ad45079978a8171fb73c9a9f8a59b1c2f69d6a675d7e519` |
| NFS DTB SHA-256 | `45815e08a057b0b49688a0abefd2a19e4d29ac9c09814177a79dbc69d81dc02c` |
| 验收脚本 SHA-256 | `d77145acc39e24b30231136c27e80497e27feecc104b3642ea326636e1f49025` |

新 Image 和固定 1080×1920 NFS DTB 已部署。当前开发板仍运行旧的 `#5`
内核，所以尚不能声称板端通过；必须重新执行 `run nfsbootfdt` 加载新 Image
后再运行上述验收脚本。

## 5. 后续门禁

真机通过后再进入：

1. RKCIF RAW 路径的 VB2 done/IRQ 延迟闭环；
2. 统计与 trace 开/关 A/B 对照；
3. generation-aware timeout；
4. `vb2_queue_error()` 向用户态传播；
5. 用户态关闭/重开恢复、100 次启停和 2 小时双路并采。
