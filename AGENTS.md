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

## Git 仓库与远端

| 项目 | 当前值 |
|---|---|
| GitHub 仓库 | `https://github.com/zzh-debug/rk3588-dual-modal-camera-bsp` |
| 可见性 | Private |
| 默认分支 | `main` |
| 远端名称 | `origin` |
| 远端 SSH 地址 | `git@github-rk3588:zzh-debug/rk3588-dual-modal-camera-bsp.git` |
| 容器内仓库根目录 | `/rk3588_dev/projects/rk3588_camera_bsp` |
| Ubuntu 宿主机仓库根目录 | `/home/zzh/workspace/rk3588_project/projects/rk3588_camera_bsp` |
| 初始基线提交 | `db4c3e337b7fae127727e469b9c77afe80e4ea6e` |

- 新对话开始项目一工作前，先读取本文件和 `README.md`，再检查 `git status`。
- GitHub 专用 SSH 私钥只保存在 Ubuntu 宿主机；不得读取输出、复制到容器或加入仓库。
- 容器内可以编辑、构建和提交；需要认证推送时，从 Ubuntu 宿主机仓库路径执行 `git push`。
- SDK canonical 集成有变化时，同步更新 `sdk/` 复现包，并运行 `./sdk/apply_sdk_changes.sh --check-only /rk3588_dev`。
- 推送、公开仓库或改写远端历史必须得到用户明确授权。
