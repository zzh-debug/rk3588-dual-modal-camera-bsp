# P1.6双路并采基线证据（2026-08-24）

本目录记录IMX415 RKISP NV12与MLX90640 ZMLX Meta的真实并行采集。
板端RTC未校时，验收日期使用开发宿主机时间。

| 文件 | 内容 |
|---|---|
| `board-snapshot.log` | 运行内核、NFS cmdline、两路绑定、RKISP和工具哈希 |
| `imx-media-graph.log` | Sensor-RKCIF-RKISP Media Graph |
| `kernel.log` | IMX/MLX/RKCIF/RKISP相关内核日志 |
| `smoke/visible-nv12.log` | 300正式NV12的逐DQBUF日志 |
| `smoke/thermal-meta.log` | 80对Meta日志 |
| `smoke/summary.log` | 短并采汇总 |
| `long/visible-nv12.log` | 3000正式NV12的逐DQBUF日志 |
| `long/thermal-meta.log` | 800对Meta日志 |
| `long/summary.log` | 扩大并采汇总 |
| `*/new-kernel-errors.log` | 测试窗口新增fatal日志，均为空 |

结果：

```text
smoke: 308 DQBUF, seq 0..307, 80 Meta pairs, all zero errors
long:  3008 DQBUF, seq 0..3007, 800 Meta pairs, all zero errors
RKISP ErrCnt=0
MLX no-buffer=0 duplicate=0
STAGE6_DUAL_CAPTURE=PASS
```

关键文件SHA-256：

```text
63f64dacff5ec7d62c8673a47aa08f429c4a8c73ede55e769f1d917054fbc7a4  smoke/summary.log
fc1b1712cdbba3082561fecf361d938ee81cf7b1c649a8a76f7ed9bd148d995f  smoke/visible-nv12.log
bb2a254f59c372de2886cd473cc32dd5cf2ac63ab0fb6bac23e5af64c87baea8  smoke/thermal-meta.log
f57130ed9b4f029a2df75585c724d3f1ba660a30b93c48ab31ae65d7cf2e0311  long/summary.log
5a8874b3a0b8b4392ecaf7d5d28bbab52b6d4a55890f4594cd608eda0c439c43  long/visible-nv12.log
aaf3857972bee7488ef464df5e4c6b57a550942a470b358e9c1ec54ada73bff0  long/thermal-meta.log
```

完整结论见`docs/阶段六双路并采基线.md`。

## P1.6-B RKCIF可观测性

| 目录/文件 | 内容 |
|---|---|
| `observability/kernel-build-online-path.log` | 含TOISP online插点的Kernel成功构建日志 |
| `observability/first-boot/` | #6内核确认trace注册、发现online路径缺口的原始数据 |
| `observability/runtime/rkcif-observability-stage6-final/` | #7内核online双路proc/trace/自动门禁 |
| `observability/runtime/rkcif-observability-stage6-trace-off/` | 同规模trace关闭A/B结果 |
| `observability/runtime/rkcif-raw-*.log` | RAW 300帧、proc快照和完整trace |

正式结果：online双路与RAW均PASS；online trace FS/FE/DMA为310/310/310，
RAW trace FS/FE/DMA/VB2为309/308/308/308，IRQ→VB2为1.166–19.834µs。

## P1.6-C L1错误传播

- `l1/kernel-build.log`：generation-aware timeout与mainpath
  `vb2_queue_error()`的成功构建日志。
- 自动门禁：`scripts/verify_rkisp_l1_timeout_stage6.sh`。
- `l1/runtime/rkisp-l1-timeout-stage6-final3/`：#9内核500ms超时、DQBUF EIO、关闭/重开双路恢复的正式证据。
- `l1/first-boot/`：#8内核故障未arm及function tracer定位ISP V30真实生命周期的证据；随后在#9内核正式通过。

## 重复启停与预长稳

- `l1/cycles/`：L1启用状态下100次mainpath短流，generation 6..105连续，
  0健康timeout，循环后双路PASS。
- `soak/10min/`：18000 NV12 + 4800 Meta预长稳，0 gap/0错误，
  RKISP ErrCnt=0，MLX no-buffer/duplicate=0/0，L1误报0；11组资源样本为
  33.307–36.076°C、MemAvailable 7,513,440–7,569,292kB。

## 2小时长稳中途故障证据

- `soak/failures/limit/`：热工具10000 pair参数上限导致的无效第一次尝试。
- `soak/failures/frame-error/`：真正双路运行在1292 pair出现`-EILSEQ`并
  queue error的热日志、dmesg与资源样本。
- `soak/mlx-frame-recovery-kernel-build.log`：单次坏Subpage恢复修正版的
  Kernel成功构建日志。
