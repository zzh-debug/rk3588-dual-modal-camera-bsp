# MLX90640 只读探测工具注释

权威源码：`projects/rk3588_camera_bsp/tools/mlx90640_readonly_probe.c`。

本文件不复制第二份 C 控制流。工具的数据路径是：

```text
/dev/i2c-N
  -> ioctl(I2C_FUNCS)，要求 I2C_FUNC_I2C
  -> I2C_RDWR，两条 message
       message 0: 写 16-bit 大端寄存器地址
       repeated START
       message 1: 读最多 32 个 16-bit 大端 word
  -> status/control 定点只读
  -> EEPROM 832 word 分段读两遍
  -> 内存比较 + ID/耗时输出 + 可选大端文件
```

`read_words()` 没有 `I2C_M_NOSTART`，两条 message 在一次 `I2C_RDWR` 中提交，
由 adapter 产生 repeated-start。32-word 分段用于避免把参考实现的一次 1664-byte
读请求直接带到 RK3X adapter；后续可根据逻辑分析仪和耗时结果调整，但 ABI 不
依赖固定分段长度。

工具唯一可选写操作是把已经读回的 EEPROM 保存为普通文件；它对传感器本身
没有任何 write-register transaction。调用者必须先确认实物接线和总线号，
不能把 `/dev/i2c-5`、`/dev/i2c-6` 当作自动探测列表依次盲试。
