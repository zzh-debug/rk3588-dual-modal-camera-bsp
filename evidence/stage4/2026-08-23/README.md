# P1.4 MLX90640 真机验收证据（2026-08-23）

本目录记录 MLX90640 在 ATK-DLRK3588 上完成实物接入、400 kHz EEPROM和
8/16 Hz Chess Subpage 验收的原始证据。

板端 RTC 未校时，因此原始日志中的 `DATE=1970-01-01` 和文件初始时间不能作为
验收日期；目录日期及项目总结使用开发宿主机的 Asia/Shanghai 时间。

| 文件 | 内容 |
|---|---|
| `mlx90640-module-rear.jpg` | 微雪模块背面和 `SCL/SDA/GND/VCC` 丝印 |
| `jp5-i2c-wiring.jpg` | JP5 pin 2/4 的 I2C5 实物接线 |
| `jp1-power-wiring.jpg` | JP1 3.3V/GND 供电实物接线 |
| `board-snapshot.log` | NFS cmdline、根挂载、运行DT频率、I2C5节点和载荷哈希 |
| `eeprom-400khz-20x.log` | 400kHz下20次EEPROM双读、ID、耗时和导出哈希 |
| `subpage-8hz.log` | 8Hz Chess，2个过渡帧和20个正式Subpage |
| `subpage-16hz.log` | 16Hz Chess，2个过渡帧和20个正式Subpage |
| `i2c-dmesg.log` | 启动及验收后的I2C相关内核日志 |

关键结论：

```text
I2C5 clock-frequency bytes: 00 06 1a 80 = 400000
MLX90640 address:            0x33
ID:                          0x131a-0x4948-0x0190
EEPROM SHA-256:              cc4628409b57ca3137065b10fd40c684fcef9d16ab493ace882710ad490185b2
EEPROM 20/20:                PASS
8 Hz / 20 subpages:          0 alternation, gap, validation errors
16 Hz / 20 subpages:         0 alternation, gap, validation errors
restored control:            0x1901
```

原始日志 SHA-256：

```text
d5714d98c9361b9d21f81b7314abc45cd6e76673d8a95333842574404617319b  board-snapshot.log
b71d8905ace8d1afbb1ed2e581aff3f98fce7bc2b591424957c5a13f319e26b9  eeprom-400khz-20x.log
7881641d068724304b7336c5a42ce52f4209975098f175033c63719767f2bd1d  i2c-dmesg.log
edfd84644c4a42588d9fa464b7b5518f68e0ee625502e54732fcc64fe6b9091e  subpage-16hz.log
feaacdf243f90af338655078272bff5b01895214858511e5932c1cf71be030c1  subpage-8hz.log
```

完整结论和边界见 `docs/阶段四总结.md`。

