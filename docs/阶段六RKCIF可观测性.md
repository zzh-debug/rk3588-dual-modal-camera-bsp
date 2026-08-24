# 阶段六 RKCIF 可观测性实现与真机验收

> 实现日期：2026-08-24（Asia/Shanghai）  
> 当前状态：**P1.6-B RKISP online、RKCIF RAW、trace开/关A/B真机门禁通过**

## 1. 本次个人增量

本次不修改厂商 RKCIF 的 DMA 地址轮换、reset/watchdog 或媒体拓扑，只在现有
IRQ、Tasklet 和 procfs 框架上增加逐 stream 关联信息：

| 增量 | 插点 | 行为 |
|---|---|---|
| 采集代际 | 首次进入一个新采集会话 | 每次新会话递增 `generation`，并清零该会话统计 |
| FS/FE | `rkcif_irq_pingpong_v1()`与`TOISP_FS/TOISP_END` | 覆盖RAW写DDR和RKISP online两条真实路径 |
| DMA/handoff | VICAP/ISP/Rockit更新及TOISP end | 记录次数、最后时间和DMA mode |
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
| 构建会话 | `output/sessions/2026-08-24_13-01-06` |
| 项目内构建日志 | `evidence/stage6/2026-08-24/observability/kernel-build-online-path.log` |
| SDK 复现检查 | `./sdk/apply_sdk_changes.sh --check-only /rk3588_dev` PASS |
| TFTP Image SHA-256 | `810fa3e3575c7e57894d8585d2accb6424ad8fb0de3618782b9f188606eaa1b0` |
| NFS DTB SHA-256 | `45815e08a057b0b49688a0abefd2a19e4d29ac9c09814177a79dbc69d81dc02c` |
| 验收脚本 SHA-256 | `377c1a403df5d0d5377bf54b67e4873f71c6cf31d842a8665273f3c99563f144` |

第一次加载的 `#6` 内核证明tracepoint注册成功，但真机同时揭示RKISP online链路使用全局 `TOISP_FS/TOISP_END`，不经过写DDR/RAW的ping-pong统计插点。补齐online插点并修复首帧前采样竞态后，`#7` 内核完成正式验收。

## 5. 真机验收结果

| 门禁 | 结果 |
|---|---|
| RKISP online + MLX双路 | 300 NV12 + 80 Meta pair，0 gap、0 bad bytes、RKISP ErrCnt=0、MLX no-buffer/duplicate=0/0 |
| online procfs中途快照 | generation=1，FS/FE/DMA/VB2=18/17/17/0；中途采样允许FS比FE多1 |
| online完整trace | FS/FE/DMA=310/310/310 |
| RKCIF RAW | 308 DQBUF，sequence 0..307，0 gap，0 bad bytes |
| RAW procfs中途快照 | generation=2，FS/FE/DMA/VB2=57/56/56/56 |
| RAW完整trace | FS/FE/DMA/VB2=309/308/308/308 |
| RAW IRQ→VB2 | last/min/max=3.208/1.166/19.834 µs |
| trace A/B | 开启10.59s，关闭10.51s；两轮均PASS，未见明显回归，不据单次样本宣称确定性能差值 |
| fatal日志 | 0 |

正式证据位于`evidence/stage6/2026-08-24/observability/runtime/`；第一次#6路径定位证据保存在`observability/first-boot/`。

## 6. 后续门禁

1. generation-aware timeout；
2. `vb2_queue_error()` 向用户态传播；
3. 用户态关闭/重开恢复；
4. 100 次启停和 2 小时双路并采。
