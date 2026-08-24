# `mlx90640_raw_probe.c` 教学说明

权威源码：`projects/rk3588_camera_bsp/tools/mlx90640_raw_probe.c`。
本文件只解释设计，不参与构建。

## 为什么不直接启用 `video-i2c.c`

当前 SDK 的通用 MLX90640 实现没有保存 data-ready、Subpage ID和成对时间语义，
且 `regmap_bulk_read()` 的 byte/word 口径需要修正。P1.4 先用用户态工具建立
可复现的协议事实，再在 P1.5 冻结内核 ABI。

## 每个 Subpage 的流程

```text
轮询 0x8000 bit3 data-ready
  -> 保存 CLOCK_MONOTONIC 时间戳和 bit0 Subpage ID
  -> 写 0x0030 清除 ready
  -> 分段读 0x0400..0x06ff（768 pixel words）
  -> 分段读 0x0700..0x073f（64 auxiliary words）
  -> 回读 0x800d control
  -> 校验数据、0/1交替、周期和读取耗时
```

每段使用标准“两字节寄存器地址写 + repeated START + 读64字节”，不使用
`I2C_M_NOSTART`。

## 配置与恢复

命令格式：

```sh
mlx90640_raw_probe /dev/i2c-5 <keep|8|16> [frame-count]
```

- `keep` 不修改 `0x800d`；
- `8/16` 使用 read-modify-write，只修改 refresh bits并确保 Chess bit置位；
- 修改配置后先丢弃两个流水线过渡 Subpage；
- 所有退出路径都尝试恢复最初的 `0x800d`并校验回读。

工具只用于原始传输和时间语义验证，不计算温度，也不定义最终V4L2 ABI。
