#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# 阶段三验收脚本的权威实现位于：
#   projects/rk3588_camera_bsp/scripts/verify_imx415_stage3.sh
#
# 本文件只作教学入口，不复制第二套控制流。权威脚本依次完成：
#
# 1. 确认当前根文件系统为 NFS，3-001a 绑定 zzh_imx415；
# 2. 从 media graph 动态解析 RKCIF RAW 和 RKISP mainpath 节点；
# 3. 对 GB10 与 NV12 分别执行单帧、短序列和长序列采集；
# 4. 逐帧检查 DQBUF 数量、sequence 连续性和 bytesused；
# 5. 比较不同曝光/增益下 NV12 Y 平面均值；
# 6. 把 VBLANK 从 58 增到 2308，验证帧周期从约 33.3 ms 变为约
#    66.7 ms，再恢复进入脚本前的 Controls；
# 7. 检查 runtime PM、RKISP ErrCnt 和本轮新增的目标链路内核错误。
#
# 用法：
#   sh verify_imx415_stage3.annotated.sh \
#       [RAW节点] [ISP节点] [短测帧数] [长测帧数] [输出目录]
#
# 节点参数留空时按 graph 自动发现。默认短测 300 帧、长测 3000 帧，完整
# 运行需要约 4 分钟。脚本会产生大约 60 MiB 的临时 RAW/NV12 文件，默认写入
# /tmp/imx415-stage3；正式归档只需保存日志、统计摘要和样本哈希。
#
# 曝光和增益的亮度比较依赖镜头前存在非全黑、相对稳定的场景。若场景在测试
# 中被遮挡或照明突变，脚本会失败并保留日志，不会把一次 ioctl 成功误判为
# Controls 已产生可观测效果。

ANNOTATED_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$ANNOTATED_DIR/../../scripts/verify_imx415_stage3.sh" "$@"
