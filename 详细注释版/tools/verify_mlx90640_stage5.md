# `verify_mlx90640_stage5.c` 教学说明

权威源码：`projects/rk3588_camera_bsp/tools/verify_mlx90640_stage5.c`。
本文件不参与构建。

工具自动遍历 `/dev/video*`，通过 `VIDIOC_QUERYCAP` 找到driver name为
`zzh_mlx90640`的Meta节点，然后执行：

```text
VIDIOC_G_FMT
  -> 确认fourcc='ZMLX'、buffersize=3400
VIDIOC_REQBUFS(MMAP, 4)
  -> QUERYBUF/mmap/QBUF
VIDIOC_STREAMON
  -> poll
  -> DQBUF
  -> 校验magic/version/size/sequence/flags
  -> 校验两个Subpage ID相反、时间戳递增
  -> 校验V4L2 timestamp等于第二页ready时间
  -> 计算Buffer哈希
  -> QBUF复用
VIDIOC_STREAMOFF
  -> munmap
  -> REQBUFS(count=0)
  -> close/reopen下一周期
```

命令：

```sh
verify_mlx90640_stage5 [pairs] [cycles] [/dev/videoN]
```

默认每周期20对、执行3个周期。任何poll错误、ABI不一致、sequence断号、时间戳
异常或ioctl失败都会返回非零；全部通过才输出
`STAGE5_RUNTIME_CHECK=PASS`。
