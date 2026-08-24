# 阶段五 ABI 设计与本地实现：MLX90640 V4L2 Meta/VB2

> 建立日期：2026-08-23（Asia/Shanghai）  
> 当前状态：**P1.5 built-in板端验收已于2026-08-24通过；最终结论见 `阶段五总结.md`**  
> 前置门禁：P1.4 400 kHz EEPROM与8/16 Hz Chess Subpage真机功能验收通过

## 1. 阶段目标和边界

P1.5把P1.4已验证的原始I2C传输封装成应用层可稳定消费的版本化接口：

- 独立 `zzh,mlx90640` I2C驱动；
- 只读EEPROM缓存和NVMEM provider；
- `V4L2_BUF_TYPE_META_CAPTURE` + VB2；
- MMAP、poll、sequence和 `CLOCK_MONOTONIC` 时间戳；
- 两个连续Chess Subpage组成一个Meta Buffer；
- STREAMOFF同步停线程、回收Buffer并恢复原控制寄存器；
- I2C/数据错误通过 `vb2_queue_error()` 传播给用户态。

本阶段不计算摄氏温度，不做坏点、滤波、伪彩、跨光谱标定或融合；这些属于
项目二用户态工作。RKCIF错误恢复、双路并采和故障注入属于P1.6。

## 2. ABI v1

权威共享头：
`projects/rk3588_camera_bsp/include/uapi/linux/zzh_mlx90640_meta.h`。

```text
V4L2 Meta fourcc: 'ZMLX'
magic:            0x584c4d5a
version:          1
buffer size:      3400 bytes
byte order:       little-endian
```

Meta Buffer结构：

```text
24-byte header
  magic/version/header_size/buffer_size
  pair_sequence/flags/reserved

subpage[0]（按捕获时间排序）
  ready_timestamp_ns/read_done_timestamp_ns
  status/control/subpage_id
  pixels[768]/auxiliary[64]

subpage[1]（按捕获时间排序）
  同上
```

`subpage[0/1]`表示时间先后，不表示固定ID；应用必须读取 `subpage_id`。V4L2
Buffer时间戳取第二个Subpage的data-ready时刻，Meta内部同时保留两页ready和
read-done时间，便于项目二定义配对窗口和曝光中心近似。

## 3. 驱动设计

权威实现：`projects/rk3588_camera_bsp/driver/zzh_mlx90640.c`。

### 3.1 probe

1. 要求plain I2C和地址 `0x33`；
2. 读取status/control；
3. 两次分段读取832-word EEPROM并比较；
4. 缓存1664-byte大端EEPROM，注册root-only只读NVMEM；
5. 注册固定3400-byte `ZMLX` Meta节点和VB2 vmalloc队列。

`0x131a-0x4948-0x0190`只称EEPROM reference ID words，不称官方唯一Chip ID。

### 3.2 STREAMON

1. 保存原始control；
2. read-modify-write设置16 Hz Subpage和Chess模式；
3. 清data-ready并读取/丢弃两个配置过渡Subpage；
4. 启动内核采集线程；
5. 等待data-ready，读取768 pixel + 64 aux + control；
6. 只把连续且ID相反的两页组成一对；
7. 从active queue取Buffer、填充v1结构并 `VB2_BUF_STATE_DONE`；
8. 没有可用Buffer时丢弃当前pair并计数，不复用仍由用户态占用的Buffer。

### 3.3 STREAMOFF和错误

- `kthread_stop()`同步等待采集线程结束；
- 恢复STREAMON前保存的原始control并回读校验；
- active queue中未完成Buffer统一以ERROR完成；
- I2C、超时、数据或control异常调用 `vb2_queue_error()`；
- 日志输出pair、no-buffer和duplicate计数。

因此旧采集线程不会跨越STREAMOFF进入下一次会话，Buffer也不会double-done。

## 4. 集成

| 文件 | 作用 |
|---|---|
| `driver/zzh_mlx90640.c` | 唯一权威驱动实现 |
| `include/uapi/linux/zzh_mlx90640_meta.h` | 内核/用户态共享ABI |
| `tools/verify_mlx90640_stage5.c` | MMAP/poll/DQBUF/重复开关流验收 |
| `kernel/drivers/media/i2c/zzh_mlx90640_builtin.c` | built-in包装器 |
| `rk3588-alientek-mlx90640-stage5.dtsi` | I2C5 `0x33`节点和16Hz设置 |
| 内核Kconfig/Makefile/defconfig | `CONFIG_VIDEO_ZZH_MLX90640=y` |

P1.4的 `clock-frequency = <400000>` 保持不变。

## 5. 静态和构建门禁

- 外置模块 `W=1` 构建通过；
- 驱动、UAPI、DTSI、包装器和用户态工具均为checkpatch
  `0 error / 0 warning / 0 check`；
- 用户态工具以 `-O2 -Wall -Wextra -Werror` 交叉编译为AArch64 ELF；
- `./build.sh kernel`成功，build session为
  `output/sessions/2026-08-23_23-36-52/`；
- `CONFIG_VIDEO_ZZH_MLX90640=y`，vmlinux存在probe/thread/driver符号；
- 部署NFS DTB同时包含 `zzh,mlx90640`、16Hz、400kHz和NFS bootargs。

源码SHA-256：

```text
eeb654120afbdfc27768b97b9570a6a3b4f8a41137df4cd52727df1fdde15c3b  zzh_mlx90640.c
241058cbb9c56224a2b055134302bc85776c2b06dd7e375c85f3c03e5a54cea4  zzh_mlx90640_meta.h
fa3f203da35a860cc539f98c59bb56605a766f165d1b12b78ad840205be3fb51  verify_mlx90640_stage5.c
```

部署载荷：

```text
Image:    da9cbf7de2542809324885e464cb0daa5b2e602d19a08af12e35ac363b6bebeb
NFS DTB:  45815e08a057b0b49688a0abefd2a19e4d29ac9c09814177a79dbc69d81dc02c
validator:91efde60354570acfd01d7a4b294cd3fa541f9f3de230c848f52851539ebf76a
```

## 6. 外置模块真机烟测

在仍运行P1.4内核的开发板上临时加载同一权威源码生成的外置模块，并通过I2C
`new_device`创建 `5-0033`。测试结束后删除client并卸载模块，回退成功。

结果：

```text
Meta node:              /dev/video81（本次动态编号）
Meta size:              3400
cycles:                 3
pairs per cycle:        10
total pairs:            30
Subpage gap:            约62.5~65.7 ms
MMAP/poll/sequence:      PASS
timestamp/ABI/hash:      PASS
STREAMOFF reopen:        PASS
no-buffer/duplicate:     0/0（每周期）
restored control:        0x1901（每周期）
STAGE5_RUNTIME_CHECK:    PASS
cleanup:                 PASS
```

2026-08-24继续扩展的非重启门禁：

```text
NVMEM:                       1664 bytes，SHA-256与P1.4黄金值一致
normal extended capture:     200 pairs / 10 cycles PASS
repeated STREAMON/OFF:       100/100 PASS
abnormal process exit:       第14对终止，release自动恢复control=0x1901
immediate reopen:            PASS
500ms slow consumer:         delivered 12 pairs PASS
slow-consumer counters:      pairs=16 no-buffer=32 duplicate=0
post-slow immediate recovery:10 pairs PASS
```

原始证据位于 `evidence/stage5/2026-08-24/`。慢消费时驱动只丢弃没有可用
Buffer期间的新pair，不复用用户持有的Buffer；STREAMOFF后立即正常恢复。

## 7. built-in启动后门禁

完成 `run nfsbootfdt` 后仍需取得：

1. 运行DT `zzh,mlx90640@33`和400kHz；
2. built-in probe、EEPROM缓存和NVMEM 1664-byte哈希；
3. Meta节点自动发现和格式；
4. 多周期MMAP/poll/sequence/时间戳；
5. 重复STREAMON/OFF后的control恢复和Buffer计数；
6. IMX415、Goodix和FUSB302共享总线回归；
7. 原始日志、载荷哈希和阶段五总结。

上述built-in门禁已于2026-08-24全部通过，P1.5关闭；证据见
`evidence/stage5/2026-08-24/builtin/`，完整结论见`docs/阶段五总结.md`。

