# ATK-DLRK3588 Phase 1：TFTP + NFS 网络启动

生成日期：2026-07-31（Asia/Shanghai）

## 1. 结论

**已在真机跑通**：U-Boot 通过 TFTP 拉 kernel，kernel 通过 NFS 挂根文件系统，显示正常。
eMMC 上的 loader/boot/recovery/rootfs/oem/userdata 全程未改动；为加入网络启动默认环境和 TFTP 提速，只单独刷过 `uboot` 分区。复位且不干预仍会回到原厂 eMMC 启动链。

Phase 0 报告第 15 节前两条结论需要更正：

- **第 1 条**（"U-Boot 板级 DTB 未启用 GMAC，TFTP 不是可执行闭环"）不成立。
  U-Boot 早期使用裁剪的 `u-boot.dtb`，运行时再从 `resource.img` 读取完整内核 DTB 并重新扫描 Driver Model，所以 GMAC 本来就可用。**不需要修改 U-Boot DTS 来补 GMAC 节点**；但为加入网络启动默认环境和 TFTP 提速，`evb_rk3588.h` 确实有修改并已单刷 `uboot`。见 2.1。
- **第 2 条**（缺 `CONFIG_ROOT_NFS` 等）已由 07-30 22:18 那次内核重编解决。

## 2. 三个决定方案形态的机制

### 2.1 U-Boot 早期用小 DTB，运行时切换到完整内核 DTB

`u-boot/.config` 中 `CONFIG_USING_KERNEL_DTB=y`、`CONFIG_USING_KERNEL_DTB_V2=y`：

```text
arch/arm/mach-rockchip/board.c:539  board_init() -> init_kernel_dtb()
arch/arm/mach-rockchip/kernel_dtb.c:297
  rockchip_read_dtb_file()          从 eMMC boot 分区的 resource.img 读内核 DTB 到 fdt_addr_r
  gd->fdt_blob = <resource.img 中选中的完整内核 DTB>
  of_live_build() + dm_scan_fdt()   用完整 DTB 重新扫描并绑定 Driver Model
```

`u-boot/u-boot.dtb` 只有 8,883 字节，是 `dts/Makefile` 用
`fdtgrep -b u-boot,dm-pre-reloc -b u-boot,dm-spl` 剪剩的重定位前设备集合。
Phase 0 检查的是这份剪过的 DTB，才得出"没有 ethernet 节点"的结论。

实测 `dm tree` 两个 GMAC 都已 probe：

```text
ebeacc50  ethernet   [ + ]  gmac_rockchip        |-- ethernet@fe1b0000
ebeacd90  eth_phy_ge [ + ]  eth_phy_generic_drv  |   `-- phy@1
ebeace40  ethernet   [ + ]  gmac_rockchip        |-- ethernet@fe1c0000
Net:   eth0: ethernet@fe1b0000, eth1: ethernet@fe1c0000
```

PHY 是 **YT8531C**，U-Boot 无 motorcomm 驱动，走 `eth_phy_generic_drv`（genphy），
实测能正常自协商到千兆并完成 TFTP，无需移植厂商 PHY 驱动。

### 2.2 U-Boot 按 SARADC 通道 7 自动选屏（**最容易踩的坑**）

板子有三块可选 MIPI 屏，`resource.img` 里打包了三份 DTB，文件名尾部编码了识别电压，
`arch/arm/mach-rockchip/resource_hwid.c: hwid_adc_find_dtb()` 读 SARADC ch7 后按 ±100 容差匹配：

| DTB | saradc_ch7 |
|---|---:|
| `rk3588-alientek-2mipi720x1280-2hdmi` | 12 |
| `rk3588-alientek-2mipi1080x1920-2hdmi` | 1405 |
| `rk3588-alientek-2mipi800x1280-2hdmi` | 2880 |

本板实测：

```text
   - dev=saradc, channel=7, dtb_adc=12,   read=1395, found=0
   - dev=saradc, channel=7, dtb_adc=1405, read=1395, found=1
DTB: rk3588-alientek-2mipi1080x1920-2hdmi#_saradc_ch7=1405.dtb
```

**这块板接的是 1080x1920 的屏**，而板级 defconfig 的主 DTS（`RK_KERNEL_DTS_NAME`）是
720x1280。U-Boot 显示初始化也印证：

```text
dsi@fde20000:  detailed mode clock 119000 kHz
    H: 1080 1090 1096 1128
    V: 1920 1940 1946 1956
VOP update mode to: 1080x1920p54, type: MIPI0 for VP2
```

因此**不能用任意或不匹配的 `tftp ${fdt_addr_r} <dtb>`**覆盖自动选好的 DTB；例如用 720x1280 时序驱 1080x1920 的屏会只显示一角。对当前固定 1080x1920 开发板，可以通过 TFTP 加载由部署脚本从匹配 DTS 生成的最新 DTB；回退路径才是 5.3 的 `fdt set` 流程。

### 2.3 bootargs 只能写在 DTB 里

```text
arch/arm/mach-rockchip/board.c:1353  bootargs_add_dtb_dtbo()
    读 DTB 的 /chosen/bootargs，env_update("bootargs", <dtb 值>)
cmd/nvedit.c:406  env_update_filter()
    同名 key（root=）用 DTB 的值 replace 掉 env 的值 → DTB 覆盖 setenv
```

同时 `CONFIG_ENV_IS_NOWHERE=y`（`saveenv` 无效）、`CONFIG_ENVF` 与 `CONFIG_ENV_PARTITION`
均未开（`sys_bootargs` 那条覆盖路径被 `#if` 编译掉）。

结论：`setenv bootargs` 改不动 `root=`。保留 resource DTB 的回退路径应用
**`fdt set` 就地改 U-Boot 已经自动选好的那份 DTB 的 `/chosen/bootargs`**；日常 `nfsbootfdt` 则使用部署脚本预先写好 NFS `/chosen/bootargs` 的匹配完整 DTB。
`CONFIG_CMD_FDT=y`、`CONFIG_HUSH_PARSER=y`，`cmd/fdt.c:22` 的 SCRATCHPAD 为 1024 字节，
当前 cmdline 约 380 字节，够用。

## 3. 网络与目录拓扑

```text
Windows 11        192.168.5.10   ASIX USB 拓展坞网卡（板子直连这块）
Ubuntu 24.04      192.168.5.11   ens37（桥接），tftpd-hpa + nfs-kernel-server
ATK-DLRK3588      192.168.5.20   eth0 = gmac0 = fe1b0000，YT8531C，1Gbps/Full
```

服务端使用宿主机的 `myproject` 服务根，RK3588 的两个子目录通过 bind mount 接入：

| 容器路径 | 宿主机真实路径 | 服务端看到的路径 |
|---|---|---|
| `/rk3588_dev/tftpboot/atk_dlrk3588` | `.../rk3588_project/tftpboot/atk_dlrk3588` | `.../myproject/tftpboot/atk_dlrk3588` |
| `/rk3588_dev/nfs_rootfs/atk_dlrk3588` | `.../rk3588_project/nfs_rootfs/atk_dlrk3588` | `.../myproject/nfs_rootfs/atk_dlrk3588` |

宿主机上一次性执行并写入 `/etc/fstab`：

```bash
sudo mount --bind /home/zzh/workspace/rk3588_project/tftpboot/atk_dlrk3588 \
                  /home/zzh/workspace/myproject/tftpboot/atk_dlrk3588
sudo mount --bind /home/zzh/workspace/rk3588_project/nfs_rootfs/atk_dlrk3588 \
                  /home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588
sudo exportfs -ra
```

`tftpd-hpa` 带 `--secure` 只允许单个根目录，bind mount 就是为了绕开这一点，
`/etc/exports` 和 tftpd 配置都不用改。

## 4. 容器内的改动与产物

### 4.1 NFS RootFS（1.2 GiB）

从 ext4 镜像只读挂载后保留 numeric owner / mode / xattr / hardlink 同步，
不直接用 `buildroot/output/.../target/`（那里没有 fakeroot 属主语义）：

```bash
sudo mount -o loop,ro buildroot/output/alientek_rk3588/images/rootfs.ext2 /mnt/rk3588_rootfs_ro
sudo rsync -aHAX --numeric-ids --delete /mnt/rk3588_rootfs_ro/ nfs_rootfs/atk_dlrk3588/
sudo umount /mnt/rk3588_rootfs_ro
```

落地后的改动：

| 改动 | 原因 |
|---|---|
| `etc/init.d/S45connman` → `K45connman` | connmand 会接管 eth0 重新配置，NFS 根链路会断。`etc/init.d/rcS` 只跑 `S??*`，改 K 开头即失效 |
| 新增 `etc/rootfs-source` | 标记当前根来自 NFS，和 eMMC 根一眼可分 |

不需要动的：`etc/network/interfaces` 只有 lo；`etc/fstab` 里 `/dev/root / ext4 rw,noauto`
在 NFS 启动时 `usr/bin/disk-helper: prepare_part()` 会因设备不存在直接返回，
`PARTLABEL=oem` / `PARTLABEL=userdata` 仍从 eMMC 正常挂上；`CONFIG_DEVTMPFS_MOUNT=y`，
`/dev` 由内核挂载，不需要静态设备节点。`etc/iptables.conf` 是 0 字节空文件、
`etc/sysctl.conf` 为空，都不影响网络。

### 4.2 TFTP 载荷

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| `Image` | 36,207,104 | `aeca702de12c2e0d6161cd8c097ac1a940a409c91a8e6176e5bd57891f7369ba` |
| `rk3588-alientek-2mipi1080x1920-2hdmi.dtb` | 290,156 | `4365f56a6898ba1b22b2bc08f2206ee840ccd0c2ce29e3203abe1c78f32eb042` |
| `rk3588-alientek-2mipi720x1280-2hdmi.dtb` | 290,164 | `7516aaf3a833a0f127ae7bc56cc19f9d602c7b5800e4bb8acd11bd00eb278f30` |
| `rk3588-alientek-nfs.dtb` | 290,288 | `4430b7c024eaf75056c030e68436dca6b78f973aa0c00a185ce3602d12344104` |

`Image` 来自 2026-07-30 22:18 那次重编，已含 NFS Root 支持
（`kernel/System.map` 有 `nfs_root_data`、`root_nfs_parse_addr`、`ip_auto_config`）。
`rk3588-alientek-nfs.dtb` 由部署脚本从匹配当前屏幕的完整 DTB 生成，保留 Linux DTS 修改并写入 NFS `/chosen/bootargs`。对当前固定 1080x1920 开发板，它是日常 Linux 驱动/DTS 网络验证的标准 DTB。

kernel cmdline（`fdt set` 写进 `/chosen/bootargs` 的内容）：

```text
earlycon=uart8250,mmio32,0xfeb50000 console=ttyFIQ0 irqchip.gicv3_pseudo_nmi=0
rootwait rcupdate.rcu_expedited=1 rcu_nocbs=all
root=/dev/nfs rw
nfsroot=192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588,v3,tcp
ip=192.168.5.20:192.168.5.11::255.255.255.0:atk-dlrk3588:eth0:off
```

`nfsroot=` 用**服务端 export 路径**；`ip=` 的网关字段留空（直连网段）；device 字段必须写死
`eth0`——内核 `net/ipv4/ipconfig.c:1545` 静态配置时取 `ic_first_dev`，不按 carrier 挑。

### 4.3 U-Boot

改动集中在 `u-boot/include/configs/evb_rk3588.h`：

1. `ROCKCHIP_DEVICE_SETTINGS` 里追加 `ATK_NET_BOOT_SETTINGS`
   （`ipaddr`/`serverip`/`netmask`/`nfsimage`/`nfsfdt`/`nfsargs`/`nfsboot`/`nfsbootfdt`）。
   因为 `CONFIG_ENV_IS_NOWHERE`，只能编进默认环境。
2. TFTP 提速：`CONFIG_IP_DEFRAG` + `CONFIG_TFTP_BLOCKSIZE 8192`。TFTP 是逐块 ACK 的
   往返受限传输，默认 1468 字节块在 1.1 ms RTT 下只能跑 ~1.4 MiB/s；8192 的块会以 IP
   分片到达，需要 `net/net.c` 的重组（缓冲默认 16384 字节，够用）。这是 U-Boot 中常见的大块 TFTP + IP 分片重组配置。编译产物里 `net/net.o` 的
   `pkt_buff` 为 0x4000 字节，确认已生效。
   **实测 1.4 → 4.1 MiB/s**，36 MiB 的 Image 从 26 秒降到约 9 秒。没到理论的 5.5 倍是因为
   8192 的块要拆成 6 个 IP 分片再重组。还想更快可以上 16352，收益递减。

`CONFIG_BOOTCOMMAND` 未动，仍是原厂的
`boot_android ${devtype} ${devnum};boot_fit;bootrkp;run distro_bootcmd;`——
**复位/不干预就从 eMMC 启动，只有执行 `run nfsboot` 或 `run nfsbootfdt` 才走网络**，两条路互不影响。

曾尝试给 `u-boot/arch/arm/dts/rk3588-alientek.dts` 补 gmac 节点，确认会被 `fdtgrep` 剪掉、
对运行时无影响，已撤销，该文件保持厂商原样。

| 固件 | 用途 | SHA-256 |
|---|---|---|
| `deploy/atk_dlrk3588/uboot.img.pre-gmac` | 出厂构建版本，回退用 | `8fb0a2075e50abb9688130ed4671a1297a6c2219cf5bd2ae02d43f5bc26b7a93` |
| `deploy/atk_dlrk3588/uboot.img.nfsboot` | 带 `run nfsboot` + TFTP 提速，**已刷入 2026-07-31** | `ec1304aaf35b609d54094c5b46bd83ae1799451246c48ae860cd02b0dec95631` |

### 4.4 部署脚本

`projects/scripts/deploy_net_boot.sh`：拷 `Image`、拷屏对应的原生 DTB、生成日常 Linux DTS 验证用的 NFS DTB，
打印可直接粘贴的 U-Boot 命令。`-m` 同步内核模块，`-i` 指定网口，`-d` 指定屏的 DTS 名
（默认已按实测 ADC 设为 1080x1920 那块）。

## 5. 板上操作

### 5.1 Linux 侧验证服务端（可选，排障用）

```sh
ip addr add 192.168.5.20/24 dev eth0 && ip link set eth0 up
ping -c3 192.168.5.11
mount -t nfs -o nolock,vers=3 192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588 /mnt
cat /mnt/etc/rootfs-source
cd /tmp && busybox tftp -g -r atk_dlrk3588/Image 192.168.5.11 && sha256sum Image
```

### 5.2 U-Boot 侧验证网络

```text
printenv ethaddr            # 由 board.c:457 rockchip_set_ethaddr() 设置，读不到就随机
dm tree                     # 应见 ethernet@fe1b0000 / fe1c0000 均 [ + ]
setenv ipaddr 192.168.5.20
setenv serverip 192.168.5.11
ping 192.168.5.11           # host 192.168.5.11 is alive
```

### 5.3 保留 resource DTB 的回退启动流程

```text
setenv nfsargs '<4.2 的 cmdline，一整行>'
tftp ${kernel_addr_r} atk_dlrk3588/Image
fdt addr ${fdt_addr_r}
fdt resize 1024
fdt set /chosen bootargs "${nfsargs}"
booti ${kernel_addr_r} - ${fdt_addr_r}
```

**不 TFTP dtb**，让 U-Boot 自动选好的那份留在 `fdt_addr_r`，只改它的 bootargs。
这是保留 SARADC 自动选屏的回退/排障路径，刷了 `uboot.img.nfsboot` 之后等价于一条 `run nfsboot`。当前日常 DTS 开发应使用 5.4 的 `run nfsbootfdt`。

地址无冲突：`kernel_addr_r=0x00400000` + 36 MiB < `fdt_addr_r=0x08300000`。

成功标志：`Kernel command line:` 里能看到 `root=/dev/nfs`、`IP-Config: Complete`、
`VFS: Mounted root (nfs filesystem)`；进 shell 后 `cat /etc/rootfs-source` 有标记。

### 5.4 日常 Linux 驱动/DTS 开发（推荐）

```text
tftp ${kernel_addr_r} atk_dlrk3588/Image
tftp ${fdt_addr_r} atk_dlrk3588/rk3588-alientek-nfs.dtb
booti ${kernel_addr_r} - ${fdt_addr_r}
```

刷了新 uboot.img 后等价于 `run nfsbootfdt`。

这条路径会绕过 U-Boot 对 resource.img 的自动选屏，因此 `rk3588-alientek-nfs.dtb` 必须从当前实际屏幕对应的 DTS 生成。当前板子使用：

```text
rk3588-alientek-2mipi1080x1920-2hdmi
```

修改 Linux DTS 后，在容器重新编译、执行 `deploy_net_boot.sh -d rk3588-alientek-2mipi1080x1920-2hdmi`，然后在 U-Boot 中执行：

```text
run nfsbootfdt
```

### 5.5 刷 uboot 分区拿到 `run nfsboot` 与 TFTP 提速

板子 `reboot loader` 进 Loader 模式（工具栏应显示 LOADER 设备，不是 MASKROM），
RKDevTool「下载镜像」页 **只勾 uboot 行**（0x00004000），路径指向
`deploy/atk_dlrk3588/uboot.img.nfsboot`，其余行全部取消勾选，「强制按地址写」不勾。
loader/misc/boot/recovery/rootfs/oem/userdata 一律不动。

刷完验证：

```text
printenv nfsboot            # 应打印完整的 fdt set 流程
printenv nfsbootfdt         # 应打印 Image + DTB 的网络启动流程
printenv ipaddr serverip    # 192.168.5.20 / 192.168.5.11
run nfsbootfdt              # 日常 Image + 最新匹配 DTB 进 NFS 根
reset                       # 不干预 → 仍从 eMMC 启动
```

出问题就进 Maskrom 刷回 `uboot.img.pre-gmac`。

## 6. 日常迭代流程

```bash
# 改内核/驱动后
./build.sh kernel
./projects/scripts/deploy_net_boot.sh \
    -d rk3588-alientek-2mipi1080x1920-2hdmi \
    -m
# 板子：U-Boot 下 run nfsbootfdt（日常开发）
```

只改用户态或 `.ko`：直接写 `/rk3588_dev/nfs_rootfs/atk_dlrk3588/`，板子上立刻可见，不用重启。

## 7. 踩坑记录

| 现象 | 根因 | 处理 |
|---|---|---|
| 板子 ping 不通 5.11，但能 ping 通 Windows 5.10 | VMware 虚拟网络编辑器里 VMnet0 是**「自动桥接」**，多网卡环境下挑错了物理网卡（USB 拓展坞拔插后更容易漂） | VMnet0 明确桥接到 `ASIX USB to Gigabit Ethernet Family Adapter`，不要留"自动" |
| 网络启动后画面只占屏幕一角 | TFTP 了不匹配的 DTB，绕过了 2.2 的 SARADC 自动选屏，用 720x1280 时序驱 1080x1920 的屏 | 使用匹配 1080x1920 DTS 重新生成 `rk3588-alientek-nfs.dtb`，再执行 `run nfsbootfdt`；需要回退自动选屏时执行 `run nfsboot` |
| 宿主机 `ping 192.168.5.20` 报 Destination Host Unreachable | 板子当时还在启动、`ip=` 尚未生效，宿主机缓存了 FAILED 的 ARP 记录 | 待板子起来后 `sudo ip neigh flush dev ens37` 再 ping；TCP 通即证明链路正常 |
| U-Boot 提示符下 `printenv ethact` 报未定义 | 正常，`ethact` 在第一次网络操作时才赋值 | 无需处理 |

## 8. 回退

- eMMC 上的 loader/boot/recovery/rootfs/oem/userdata 未动；`uboot` 分区是唯一单独刷过的分区
- 复位且不干预即回到 `root=PARTUUID=614e0000-0000` 的 eMMC 系统
- 若刷了新 uboot 并出问题：进 Maskrom，用 `deploy/atk_dlrk3588/uboot.img.pre-gmac` 重刷

## 9. 已知可优化项

1. **MAC 地址每次上电随机**（`d6:71:…`，本地管理位为 1）。vendor 分区里没有 LAN_MAC_ID，
   `board.c:108 rockchip_set_ethaddr()` 就随机生成。可用 `vendor_storage` 写死一个，
   免得 ARP 缓存和交换机 MAC 表被搅乱。
2. `drm-cubic-lut@00000000` 保留内存 size 0 的告警在两种启动方式下都出现，非本次引入。
3. TFTP 还能再快：块大小提到 16352 理论上还有提升，
   但分片数更多、收益递减，当前 9 秒已够用。

## 10. 实测记录（2026-07-31，刷入 uboot.img.nfsboot 后）

```text
U-Boot 2017.09-g23c0020-dirty #vscode (Jul 31 2026 - 12:07:48 +0800)
   - dev=saradc, channel=7, dtb_adc=12,   read=1393, found=0
   - dev=saradc, channel=7, dtb_adc=1405, read=1393, found=1
DTB: rk3588-alientek-2mipi1080x1920-2hdmi#_saradc_ch7=1405.dtb
VOP update mode to: 1080x1920p54, type: MIPI0 for VP2
Net:   eth0: ethernet@fe1b0000, eth1: ethernet@fe1c0000

=> run nfsboot
ethernet@fe1b0000 Waiting for PHY auto negotiation to complete. done
Loading: ... 4.1 MiB/s
Bytes transferred = 36207104 (2287a00 hex)

Kernel command line: ... root=/dev/nfs
  nfsroot=192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588,v3,tcp
  ip=192.168.5.20:192.168.5.11::255.255.255.0:atk-dlrk3588:eth0:off
```

这段日志是当时刷入 `uboot.img.nfsboot` 后执行 `run nfsboot` 的真机实测，证明保留 resource DTB 的回退路径可用；当前日常 Linux DTS/驱动开发应按 5.4 使用 `run nfsbootfdt`。

banner 里的 `-dirty` 来自 u-boot 仓库中未提交的 `evb_rk3588.h` 改动，可当作
"自建固件"的标记。

进系统后的两项收尾验证（均通过）：

```text
# cat /etc/rootfs-source
NFS rootfs (192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588)
exported from buildroot/output/alientek_rk3588/images/rootfs.ext2
connman disabled: etc/init.d/S45connman -> K45connman

# mount | grep ' / '
192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588 on / type nfs
  (rw,relatime,vers=3,rsize=4096,wsize=4096,hard,nolock,proto=tcp,timeo=600,...)
```

不干预复位后仍从 eMMC 正常启动，**两条启动路径互不影响，Phase 1 收尾**。

`rsize/wsize=4096` 是内核 NFS root 客户端在没有用户态 mount 参与时的保守默认值。
交互和改 `.ko` 完全够用；将来批量读大文件觉得慢，可在 `nfsroot=` 里补
`,rsize=131072,wsize=131072`（要同时确认服务端 `/etc/exports` 无 `wdelay` 相关限制）。
