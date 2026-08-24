# P1.7 5V白光PWM补光证据

当前仅完成实现、静态DTB反查、Kernel/DTB构建和TFTP部署；真机证据将在执行
`run nfsbootfdt`后补充。

| 文件 | 内容 |
|---|---|
| `kernel-build.log` | `CONFIG_LEDS_PWM=y`及三套DTB成功构建日志 |
| `dtb-static.log` | LED/PWM5/pinctrl/冲突节点及部署哈希反查 |

完成态必须另有默认关闭、低亮度、熄灭、PWM pinmux及双路相机回归证据。
