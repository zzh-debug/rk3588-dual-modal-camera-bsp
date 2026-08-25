# P1.7 LED真机验收证据

## 真机结果

- `board-snapshot.log`：#11内核、IMX/MLX绑定、brightness=0、PWM5 pinmux和冲突节点。
- `p17-off/summary.log`：LED关闭双路PASS。
- `p17-on32/summary.log`：brightness=32低亮度双路PASS。
- 用户手动确认brightness 1/4/8/16/32逐级调光正常。

两轮均为308 NV12 + 80 Meta，0 gap/0错误，RKISP ErrCnt=0，
MLX no-buffer/duplicate/invalid=0/0/0。测试结束brightness=0。
U-Boot阶段微亮保留为已知边界，不纳入Linux LED验收失败。
