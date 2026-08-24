# P1.5 built-in正式验收证据（2026-08-24）

本目录记录通过 `run nfsbootfdt` 加载新Image/DTB后的MLX90640 built-in验收。
板端RTC未校时，日志内部日期为1970；验收日期使用开发宿主机时间。

| 文件 | 内容 |
|---|---|
| `board-snapshot.log` | Linux #5、NFS cmdline、400kHz、built-in配置、绑定和NVMEM哈希 |
| `meta-node.log` | `/dev/video0`的driver/capabilities/`ZMLX`/3400-byte格式 |
| `meta-builtin-200pairs-10cycles.log` | 10周期、共200对正式Meta Buffer |
| `builtin-kernel.log` | built-in probe、start/stop和control恢复日志 |
| `imx415-blocker.log` | 与P1.5分账的IMX415 NACK阻塞 |

关键结果：

```text
CONFIG_VIDEO_ZZH_MLX90640=y
5-0033 -> zzh_mlx90640
NVMEM: 1664 bytes / cc4628409b57ca3137065b10fd40c684fcef9d16ab493ace882710ad490185b2
Meta: ZMLX / 3400 bytes
200 pairs / 10 cycles: PASS
no-buffer/duplicate: 0/0 per cycle
restored control: 0x1901 per cycle
STAGE5_RUNTIME_CHECK=PASS
```

日志SHA-256：

```text
15f01e458a44f44c23bef81a986962aee798572d62ae3658ede89bba417ede82  board-snapshot.log
41cdceda9083aea126dc780ee2bb29b024986a2390f588780e36a8d087c2c5f0  builtin-kernel.log
8a25c5b1f1d86662edf72313bbfe096fd0f02f4b13282ca2ce325fe22728241c  imx415-blocker.log
1df84904b38d1035520b1795563cdb4a4efe3f7b65fad26f75e9c05a7961ce52  meta-builtin-200pairs-10cycles.log
869e26eb895be3b3b297132717eeef16b5ccf01022dfa29a1951bb98897c5757  meta-node.log
```

完整结论见 `docs/阶段五总结.md`。
