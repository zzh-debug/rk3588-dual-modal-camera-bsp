# 项目一详细注释版

## 1. 目录用途

本目录保存项目一源码的教学镜像，用于逐行理解驱动、设备树、构建配置和
验证脚本。这里的文件具有以下边界：

- **不参与正式编译和部署**；Kbuild、DTC 和部署脚本仍只使用“权威源码路径”。
- 注释版可以增加大量中文解释，但不得成为第二套功能实现。
- 修改功能时先改权威源码、完成构建和测试，再同步注释版并记录对应哈希。
- 如果注释版和权威源码控制流不一致，以权威源码为准并立即修正文档。
- 厂商大文件不整份复制；只保存本项目新增源码或与集成有关的最小片段。

这样处理的原因是：如果两份源码都参与编译，后续很容易改错文件；如果只给
正式源码堆叠大段教学注释，又会显著降低内核代码可读性。注释版解决学习需求，
权威源码保持可维护性。

## 2. 文件映射

| 注释版 | 权威源码 | 作用 |
|---|---|---|
| `driver/zzh_imx415_minimal.annotated.c` | `projects/rk3588_camera_bsp/driver/zzh_imx415_minimal.c` | 阶段一 I2C 最小识别驱动 |
| `driver/zzh_imx415.annotated.c` | `projects/rk3588_camera_bsp/driver/zzh_imx415.c` | 阶段二 1279 行权威代码完整展开，并在实际代码之间穿插固定 mode/Subdev/Controls/runtime PM 中文注释 |
| `driver/Kconfig.annotated` | `projects/rk3588_camera_bsp/driver/Kconfig` | 独立配置选项及依赖 |
| `driver/Makefile.annotated` | `projects/rk3588_camera_bsp/driver/Makefile` | 外置模块 Kbuild 规则 |
| `device-tree/rk3588-alientek-imx415-minimal.annotated.dtsi` | `kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-imx415-minimal.dtsi` | 实验节点、原厂节点隔离及图链禁用 |
| `device-tree/rk3588-alientek-imx415-stage2.annotated.dtsi` | `kernel/arch/arm64/boot/dts/rockchip/rk3588-alientek-imx415-stage2.dtsi` | 阶段二节点、同地址隔离和双向 Media Graph endpoint |
| `device-tree/主DTS集成与DTB生成链路.md` | 目标主 DTS、Rockchip DTS Makefile、部署脚本 | 解释 DTSI 如何进入 DTB 及两个 DTB 名称的区别 |
| `scripts/verify_imx415_stage1.annotated.sh` | `projects/rk3588_camera_bsp/scripts/verify_imx415_stage1.sh` | 板端 20 次加载/卸载验收 |
| `scripts/verify_imx415_stage2.annotated.sh` | `projects/rk3588_camera_bsp/scripts/verify_imx415_stage2.sh` | 动态发现 Subdev/media/RKCIF node、API 证据和短流循环 |
| `scripts/verify_imx415_stage3.annotated.sh` | `projects/rk3588_camera_bsp/scripts/verify_imx415_stage3.sh` | 动态发现 RKCIF/RKISP 节点，执行 GB10/NV12 长稳、Controls 和停流门禁 |
| `scripts/verify_dual_capture_stage6.sh` | `projects/rk3588_camera_bsp/scripts/verify_dual_capture_stage6.sh` | 并行运行RKISP NV12与ZMLX Meta并分别校验两路序列和错误状态 |
| `tools/mlx90640_readonly_probe.md` | `projects/rk3588_camera_bsp/tools/mlx90640_readonly_probe.c` | P1.4 分段 repeated-start 只读工具的数据路径和安全边界 |
| `tools/mlx90640_raw_probe.md` | `projects/rk3588_camera_bsp/tools/mlx90640_raw_probe.c` | P1.4 data-ready、pixel/aux、Subpage、时间戳和控制恢复流程 |
| `tools/verify_mlx90640_stage5.md` | `projects/rk3588_camera_bsp/tools/verify_mlx90640_stage5.c` | P1.5 Meta节点发现、MMAP/poll、ABI/sequence/时间戳和重复开关流验证 |
| `scripts/deploy_net_boot.annotated.sh` | `projects/scripts/deploy_net_boot.sh` | Image/目标 DTB/NFS DTB 的部署和 bootargs 改写 |
| `application/README.md` | 尚不存在 | 应用层代码占位和未来收录规则 |

## 3. 当前阶段的数据流

```text
目标主 DTS
  rk3588-alientek-2mipi1080x1920-2hdmi.dts
      |
      +-- include rk3588-alientek.dtsi
      +-- include rk3588-alientek-cameras.dtsi
      +-- include rk3588-linux.dtsi
      +-- include rk3588-alientek-imx415-stage2.dtsi  <-- 当前实验覆盖
      |
      v
Linux DTC 编译
      |
      v
rk3588-alientek-2mipi1080x1920-2hdmi.dtb              <-- 硬件树原始产物
      |
      +-- 原样部署一份到 TFTP，便于对照
      |
      +-- 复制后仅改 /chosen/bootargs 为 NFS 参数
             |
             v
        rk3588-alientek-nfs.dtb                        <-- Linux 网络启动 DTB

zzh_imx415.c（唯一权威实现）
      |
      +-- kernel/drivers/media/i2c/zzh_imx415_builtin.c
      |      -> CONFIG_VIDEO_ZZH_IMX415=y -> 新 Image 内建
      |
      +-- driver/zzh_imx415.ko（仍可独立外部模块构建，用于回退/对照）
             |
             v
板端内建驱动 -> OF compatible 匹配 -> probe/签名 -> V4L2 Subdev
      |
      v
RKCIF GB10 -> RKISP mainpath NV12 -> sequence/bytesused/Controls/PM 验收
```

P1.3已于2026-08-21关闭，P1.4已于2026-08-23关闭，P1.5 built-in
`ZMLX` Meta/NVMEM已于2026-08-24完成板端验收；见`../docs/阶段五总结.md`。
当前进入P1.6，但本次会话IMX415 I2C NACK，双路并采待恢复。详细注释版只解释
权威实现，不参与构建。

## 4. 同步基线

创建本注释版时对应的权威产物 SHA-256：

```text
Image:
aeca702de12c2e0d6161cd8c097ac1a940a409c91a8e6176e5bd57891f7369ba

目标硬件 DTB:
a94638d4587d5de2a27646b5b0f12d4c0f24164d1bae306ce346f8d58db9d6b7

网络启动 DTB:
c7049e38273e66308d85232fceae7501106557e6fce2e9c257c4ebf6544de2c2

最小驱动模块:
9a82e8127cc9726e718adeb5715fd2a008c0e40d5f0f2253c63bb76b6560167b
```

这些哈希只说明注释版建立时对应哪一套实现，不是真机验收结论。

阶段二本地同步基线（2026-08-06，历史记录；真实 TFTP/NFS 服务随后已部署）：

```text
阶段二权威驱动源码:
713c37e4f7877958bccec19eee91c8e0dec7891c55c6db5e39db72790d8243da

阶段二 DTSI:
a9f1ab4fce8d6c1e71616bb49514ea630dbc5e1aaea360b1df870c491198e4f4

阶段二验证脚本:
7a52018bd654bfbfd8b2ab55ee35aa27b07a2398eb0906b050c31bbc25e0c800

阶段二外部模块:
4dcef12313787c1afad91a9567b7a68de0ca28cc3aeef8fb680f1efbe49d99a9

1080x1920 目标 DTB:
a2e30a3fcafa0eaab8956422b40ffb4393a8127daea4b7ef99d3026650be7a48
```

阶段二板端验收结论和更新后的载荷 hash 见 `../docs/阶段二总结.md`。内建集成的
实际入口仍是内核树中的 `zzh_imx415_builtin.c`；详细注释版只是完整展开的教学
快照，不参与 Kbuild。

阶段二完整展开同步基线（2026-08-08）：

```text
权威源码行数: 1279
权威源码 SHA-256:
7a3d634641f9b193ff99c7e1ad6d1f4b9c926d5fc15e307db3cfefa7f72017b6

详细注释版行数: 1739
详细注释版 SHA-256:
10e8dd610abd4a4531c8ab5847a0a880512c866cef017591d6b586bf0c23b1d2
```

所有新增中文块都以 `/* [详细注释]` 开头。同步检查会删除这些标记块，再把
剩余文本与权威源码逐行比较；2026-08-08 检查结果为零差异。因此当前注释版
是“全部权威代码 + 仅注释增量”，没有修改函数、寄存器值或控制流。
