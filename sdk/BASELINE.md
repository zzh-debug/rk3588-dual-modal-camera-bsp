# SDK baseline

| Item | Value |
|---|---|
| Vendor release manifest name | `atk-rk3588_linux_release_v2.0_20260703.xml` |
| Rockchip release family | `linux-5.10-gen-rkr8` |
| Kernel base commit | `cfc6be6c04e6c8e66f3fc9e33f8f27b10e3c00e8` |
| U-Boot base commit | `23c0020b9bf075194ea1a9ed31b6cf64632f8ff9` |
| Board defconfig | `01_atk_dlrk3588_auto2mipi_2hdmi_defconfig` |
| Target development DTS | `rk3588-alientek-2mipi1080x1920-2hdmi` |
| Kernel version | `5.10.209` |
| Cross toolchain | GCC Arm 10.3-2021.07, AArch64 GNU/Linux |

The vendor `repo manifest -r` is intentionally not committed because it
contains a vendor-internal Git fetch URL. Obtain the matching SDK from the
board vendor, then verify the kernel and U-Boot commits above.

Current validated network-boot payloads:

```text
Image:
da9cbf7de2542809324885e464cb0daa5b2e602d19a08af12e35ac363b6bebeb

rk3588-alientek-nfs.dtb:
45815e08a057b0b49688a0abefd2a19e4d29ac9c09814177a79dbc69d81dc02c
```
