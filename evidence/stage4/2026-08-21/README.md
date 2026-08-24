# P1.4 启动基线证据（2026-08-21）

本目录记录 P1.4 启动时的只读基线，尚不代表 MLX90640 实物已经接入或 I2C
识别通过。

- `mlx90640-p14-i2c-baseline.log`：当前板端 I2C adapter、I2C5/I2C6 OF node
  和已绑定 client 清单。
- I2C5、I2C6 当前都有 `/dev/i2c-*`，没有 `*-0033` client。
- 两条候选总线都与 GT911、FUSB302、PCF8563 共享，因此没有运行整总线
  `i2cdetect -y`。
- `tools/mlx90640_readonly_probe.c` 已交叉构建并在板端验证 AArch64 ELF 可执行，
  最终二进制 SHA-256 为
  `951da2fe52f330709462ee486327d37503f4b6090af34af9f5524c96884596e3`；
  未在接线不明时给它传入 `/dev/i2c-N`，所以没有访问 `0x33`。

完整硬件契约、工具语义和下一门禁见
`docs/阶段四硬件契约与实施计划.md`。
