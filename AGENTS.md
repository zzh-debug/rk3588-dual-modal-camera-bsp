# RK3588 Camera BSP 仓库约束

## 范围

- 本仓库只保存项目自有驱动、UAPI、工具、脚本、文档、证据和最小SDK集成包。
- 不复制或提交完整Rockchip/正点原子SDK、交叉工具链、固件和RootFS。
- 权威驱动位于`driver/`；`详细注释版/`不参与编译。
- SDK canonical集成仍位于SDK的`kernel/`、`u-boot/`和`projects/scripts/`。

## 构建与编辑

- 在ATK-DLRK3588 Dev Container中以普通用户构建，禁止root/sudo编译kernel。
- 未明确要求时只运行kernel/DTB或外置模块分项构建，不运行全量SDK编译。
- 修改权威驱动后同步UAPI、验收工具、文档和`sdk/`集成包。
- 保留已有工作，不清理、不reset厂商仓库。

## 证据门禁

- 编译成功不等于真机通过；完成声明必须链接原始板端日志。
- 厂商已有Sensor/RKCIF/RKISP能力和个人新增实现分开记账。
- `0x311a=0xe0`只称厂商参考签名，不称官方唯一Chip ID。
- DMA-BUF只表述为减少CPU全帧复制，不使用“全程零拷贝”。
- 第一版错误恢复止于统计、`vb2_queue_error()`和用户态关闭/重开。

## 发布

- 远端仓库默认先使用private。
- 发布前检查硬编码账号、token、密码、私钥、内网地址和不适合公开的硬件照片。
- 原始大文件优先放Release/Object Storage，不直接进入Git历史。
