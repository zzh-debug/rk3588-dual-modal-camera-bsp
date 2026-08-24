# P1.5 外置模块扩展验收证据（2026-08-24）

本目录记录在无法重启进入新built-in Image时，使用同一权威源码生成的外置模块
继续完成的真实硬件验证。built-in自动probe仍待后续 `run nfsbootfdt` 验收。

开发板RTC未校时，原始日志时间不是验收日期；目录日期使用开发宿主机时间。

| 文件 | 内容 |
|---|---|
| `board-snapshot.log` | 内核/cmdline、NVMEM大小及EEPROM、模块、工具哈希 |
| `meta-200pairs-10cycles.log` | 每周期20对、10周期，共200对正常采集 |
| `meta-100-stream-cycles.log` | 每周期1对、100次STREAMON/OFF |
| `meta-slow-500ms.log` | 用户态每次DQBUF后持有Buffer 500ms |
| `meta-after-slow-recovery.log` | 慢消费STREAMOFF后的立即恢复采集 |
| `mlx-kernel.log` | probe、start/stop、control恢复和no-buffer计数 |
| `imx415-current-blocker.log` | 当前会话IMX415 `0x311a` NACK证据 |

关键结果：

```text
NVMEM size:                         1664 bytes
NVMEM SHA-256:                      cc4628409b57ca3137065b10fd40c684fcef9d16ab493ace882710ad490185b2
normal extended capture:            200 pairs / 10 cycles PASS
repeated STREAMON/OFF:              100/100 PASS
abnormal process termination:       release -> STREAMOFF -> control 0x1901
immediate reopen after termination: PASS
500ms slow consumer:                delivered 12 pairs PASS
driver slow-consumer counters:      pairs=16 no-buffer=32 duplicate=0
immediate normal recovery:          10 pairs PASS
```

慢消费测试证明当用户态长期占用全部4个Buffer时，驱动丢弃新pair并增加
`no-buffer`，不会复用仍由用户持有的Buffer；恢复QBUF后继续输出。未观察到
duplicate、queue error、double-done或无法重开的情况。

当前不能执行IMX415+MLX90640双路并采：运行DT和`CONFIG_VIDEO_ZZH_IMX415=y`
均正确，但启动probe和手动bind都在读取 `0x311a` 时返回I2C NACK `-6`，
RKCIF因此没有terminal sensor。该问题发生在MLX流启动前，不是双路压力导致。

原始日志SHA-256：

```text
729f944fe369d7a1a7922585ac8f29234eb83f7be312b0463da454ba995247fe  board-snapshot.log
837ed86215da5caf07bfb263492de85140d5fb61f00bfcbb845a54ed491bad0c  imx415-current-blocker.log
72c6722da3109bca68c070881f49a63ae186d8c77c524226f9c036dcc5bbeb89  meta-100-stream-cycles.log
44292675089438362efc4d54dc7149a7455583e5334748b4fb198ccaaa6c6b2f  meta-200pairs-10cycles.log
1050e1631c2d95c8fe7ebd4d7debf245c3a397cf94c28738777891a20f006e37  meta-after-slow-recovery.log
b3aa6c7f70866b7e2c160c350ca76d584698de2ac24e816b8c25ab28a58c0ec0  meta-slow-500ms.log
22a3ec7b6cb46c7b4c82fdc32322b3149ff98b501352f179b90fe1037241abc7  mlx-kernel.log
```

