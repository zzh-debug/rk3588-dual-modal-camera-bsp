// SPDX-License-Identifier: GPL-2.0
/* [详细注释]
 * 文件角色
 * ========
 * 这是阶段二 IMX415 固定模式驱动的“完整展开教学镜像”，不参与 Kbuild。
 * 权威源码：projects/rk3588_camera_bsp/driver/zzh_imx415.c
 * 内建入口：kernel/drivers/media/i2c/zzh_imx415_builtin.c
 *
 * 与早先只在文件末尾 include 权威源码的版本不同，本文件完整展开权威源码，
 * 并在实际代码之间穿插中文说明，便于从上到下阅读。功能修复必须先修改权威
 * 源码，随后同步本文件；验证时删除所有带“[详细注释]”标记的块，剩余内容应
 * 与权威源码逐行完全一致。
 *
 * 阶段二在阶段一硬件契约之上增加的主链路是：
 *
 *   DT 创建 zzh,imx415 I2C client
 *     -> probe 校验 4-lane/445.5 MHz endpoint
 *     -> 获取 MCLK、RESET/PWDN、可选 regulator
 *     -> 临时上电并读取厂商参考签名 0x311a=0xe0
 *     -> 注册 V4L2 Sensor Subdev、Media Entity、source pad 和 Controls
 *     -> runtime idle 后断电
 *     -> RKCIF STREAMON 调用 s_stream(1)
 *     -> runtime resume、写 global/mode 表、应用 Controls、退出 standby
 *     -> 输出 3864x2192 SGBRG10 30 fps MIPI CSI-2 数据
 *     -> STREAMOFF 进入 standby 并释放 runtime PM 引用
 *
 * Sensor 驱动不申请图像 Buffer；MMAP/VB2 Buffer 由 RKCIF Host 管理。本文件
 * 也不实现 HDR、12-bit、2-lane、RKISP IQ、RKCIF 恢复或长期采集策略。
 */
/*
 * Independent fixed-mode IMX415 experiment for ATK-DLRK3588 J20.
 *
 * The V4L2 state machine and resource management are project code.  Register
 * values required for the single 3864x2192 RAW10 mode are attributed in
 * docs/阶段二寄存器来源台账.md.  Opaque tuning values originate from the
 * board vendor reference and are not presented as public Sony register data.
 */

/* [详细注释]
 * 头文件按四类依赖组织：
 *
 * 1. Linux 设备资源：clock、GPIO、I2C、regulator、Device Tree；
 * 2. 并发与功耗：mutex、runtime PM；
 * 3. 固件节点解析：property/fwnode；
 * 4. Media/V4L2：entity、async subdev、controls、events、pad operations。
 *
 * 阶段一没有最后一组头文件，因为它只做 I2C 识别，不注册摄像头实体。
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* [详细注释]
 * 驱动名同时用于 I2C driver.name、传统 i2c_device_id 和日志/模块元数据。
 * I2C 地址是 Linux 使用的 7 位 0x1a；线上写、读地址字节分别为 0x34/0x35。
 */
#define ZZH_IMX415_NAME			"zzh_imx415"
#define ZZH_IMX415_I2C_ADDR		0x1a

/* [详细注释]
 * 外部输入时钟目标是 37.125 MHz。MIN/MAX 对应 Sony 允许的 0.96~1.02 倍
 * 范围。clk_set_rate 之后仍要 clk_get_rate 回读，不能把请求值当成硬件事实。
 */
#define ZZH_IMX415_XVCLK_HZ		37125000UL
#define ZZH_IMX415_XVCLK_MIN_HZ		35640000UL
#define ZZH_IMX415_XVCLK_MAX_HZ		37867500UL

/* [详细注释]
 * 固定 Sensor 输出为 3864x2192；对 Rockchip Host 暴露的推荐有效裁剪区域是
 * 从 (12,16) 开始的 3840x2160。后者是 selection/crop，不是第二套寄存器 mode。
 * 当前仅支持四条 CSI-2 data lane 和每像素 10 bit。
 */
#define ZZH_IMX415_WIDTH			3864U
#define ZZH_IMX415_HEIGHT		2192U
#define ZZH_IMX415_CROP_LEFT		12
#define ZZH_IMX415_CROP_TOP		16
#define ZZH_IMX415_CROP_WIDTH		3840U
#define ZZH_IMX415_CROP_HEIGHT		2160U
#define ZZH_IMX415_LANES			4U
#define ZZH_IMX415_BITS_PER_SAMPLE	10U

/* Sony specifies 891 Mbit/s/lane. CSI-2 LINK_FREQ is the DDR clock. */
/* [详细注释]
 * CSI-2 每 lane 是 DDR 传输，所以 891 Mbit/s/lane 对应 445.5 MHz link
 * frequency。pixel_rate = 445.5M * 2 * 4 lanes / 10 bits = 356.4 Mpixel/s。
 */
#define ZZH_IMX415_LINK_FREQ_HZ		445500000LL
#define ZZH_IMX415_PIXEL_RATE		356400000LL
#define ZZH_IMX415_VTS_DEFAULT		2250U
#define ZZH_IMX415_VTS_MAX		0x7fffU
#define ZZH_IMX415_HBLANK		1416U
#define ZZH_IMX415_VBLANK_DEFAULT	(ZZH_IMX415_VTS_DEFAULT - \
					 ZZH_IMX415_HEIGHT)

/* [详细注释]
 * 曝光以“行”为 V4L2 对外单位，默认 VTS-8；GAIN 取值 0..240，每步对应
 * Sensor 定义的 0.3 dB。VBLANK 改变时 exposure 最大值必须跟着 VTS 收敛。
 */
#define ZZH_IMX415_EXPOSURE_MIN		4U
#define ZZH_IMX415_EXPOSURE_DEFAULT	(ZZH_IMX415_VTS_DEFAULT - 8U)
#define ZZH_IMX415_GAIN_MIN		0U
#define ZZH_IMX415_GAIN_MAX		240U

/* [详细注释]
 * 运行控制寄存器：
 *
 * STANDBY 0x3000：1 停止输出，0 开始 operating；
 * REGHOLD 0x3001：把多个时序寄存器更新锁成同一提交点；
 * VMAX 0x3024：总行数，三个连续 little-byte-order 寄存器；
 * SHR0 0x3050：曝光开始位置，exposure = VTS - SHR0；
 * GAIN 0x3090：模拟增益，两字节；
 * 0x311a：厂商参考识别签名，公开名称为 INCKSEL4，不是唯一 Chip ID。
 */
#define IMX415_REG_STANDBY		0x3000
#define IMX415_STANDBY			0x01
#define IMX415_OPERATING			0x00
#define IMX415_REG_REGHOLD		0x3001
#define IMX415_REGHOLD_ENABLE		0x01
#define IMX415_REGHOLD_DISABLE		0x00
#define IMX415_REG_VMAX			0x3024
#define IMX415_REG_SHR0			0x3050
#define IMX415_REG_GAIN			0x3090
#define IMX415_REFERENCE_REG		0x311a
#define IMX415_REFERENCE_VALUE		0xe0

#define ZZH_IMX415_REG_END		0xffff

/* [详细注释]
 * 一个寄存器表项只保存 16 位地址和 8 位值。表是 static const，只占只读
 * 数据段，不在 probe 中动态分配；0xffff 是软件终止哨兵，不会发到 I2C 总线。
 */
struct zzh_imx415_reg {
	u16 address;
	u8 value;
};

/*
 * [D/V] Public clock/format registers plus [V] board-vendor tuning values.
 * Only the vendor's 10-bit, full-resolution global array is retained.
 */
/* [详细注释]
 * global 表只保留当前 RAW10 全分辨率路径需要的公共配置和厂商调优值。
 * 无法从 Sony 公共手册解释的数值按 [V] 记账，不能表述为自行推导。
 * STREAMON 每次重新写这张表，使掉电后的 Sensor 不依赖上一次寄存器残留。
 */
static const struct zzh_imx415_reg zzh_imx415_global_regs[] = {
	{ 0x3002, 0x00 },
	{ 0x3008, 0x7f },
	{ 0x300a, 0x5b },
	{ 0x3031, 0x00 },
	{ 0x3032, 0x00 },
	{ 0x30c1, 0x00 },
	{ 0x30d9, 0x06 },
	{ 0x3116, 0x24 },
	{ 0x311e, 0x24 },
	{ 0x32d4, 0x21 },
	{ 0x32ec, 0xa1 },
	{ 0x3452, 0x7f },
	{ 0x3453, 0x03 },
	{ 0x358a, 0x04 },
	{ 0x35a1, 0x02 },
	{ 0x36bc, 0x0c },
	{ 0x36cc, 0x53 },
	{ 0x36cd, 0x00 },
	{ 0x36ce, 0x3c },
	{ 0x36d0, 0x8c },
	{ 0x36d1, 0x00 },
	{ 0x36d2, 0x71 },
	{ 0x36d4, 0x3c },
	{ 0x36d6, 0x53 },
	{ 0x36d7, 0x00 },
	{ 0x36d8, 0x71 },
	{ 0x36da, 0x8c },
	{ 0x36db, 0x00 },
	{ 0x3701, 0x00 },
	{ 0x3724, 0x02 },
	{ 0x3726, 0x02 },
	{ 0x3732, 0x02 },
	{ 0x3734, 0x03 },
	{ 0x3736, 0x03 },
	{ 0x3742, 0x03 },
	{ 0x3862, 0xe0 },
	{ 0x38cc, 0x30 },
	{ 0x38cd, 0x2f },
	{ 0x395c, 0x0c },
	{ 0x3a42, 0xd1 },
	{ 0x3a4c, 0x77 },
	{ 0x3ae0, 0x02 },
	{ 0x3aec, 0x0c },
	{ 0x3b00, 0x2e },
	{ 0x3b06, 0x29 },
	{ 0x3b98, 0x25 },
	{ 0x3b99, 0x21 },
	{ 0x3b9b, 0x13 },
	{ 0x3b9c, 0x13 },
	{ 0x3b9d, 0x13 },
	{ 0x3b9e, 0x13 },
	{ 0x3ba1, 0x00 },
	{ 0x3ba2, 0x06 },
	{ 0x3ba3, 0x0b },
	{ 0x3ba4, 0x10 },
	{ 0x3ba5, 0x14 },
	{ 0x3ba6, 0x18 },
	{ 0x3ba7, 0x1a },
	{ 0x3ba8, 0x1a },
	{ 0x3ba9, 0x1a },
	{ 0x3bac, 0xed },
	{ 0x3bad, 0x01 },
	{ 0x3bae, 0xf6 },
	{ 0x3baf, 0x02 },
	{ 0x3bb0, 0xa2 },
	{ 0x3bb1, 0x03 },
	{ 0x3bb2, 0xe0 },
	{ 0x3bb3, 0x03 },
	{ 0x3bb4, 0xe0 },
	{ 0x3bb5, 0x03 },
	{ 0x3bb6, 0xe0 },
	{ 0x3bb7, 0x03 },
	{ 0x3bb8, 0xe0 },
	{ 0x3bba, 0xe0 },
	{ 0x3bbc, 0xda },
	{ 0x3bbe, 0x88 },
	{ 0x3bc0, 0x44 },
	{ 0x3bc2, 0x7b },
	{ 0x3bc4, 0xa2 },
	{ 0x3bc8, 0xbd },
	{ 0x3bca, 0xbd },
	{ 0x4004, 0x48 },
	{ 0x4005, 0x09 },
	{ ZZH_IMX415_REG_END, 0x00 },
};

/* [D/V] 3864x2192, RAW10, 891 Mbit/s/lane, linear mode only. */
/* [详细注释]
 * mode 表冻结唯一线性模式。关键项目包括：
 *
 * 0x3024/25：VMAX=0x08ca=2250；
 * 0x3028/29：HMAX=0x044c=1100；
 * 0x3050..：默认 SHR0；
 * 0x3118、0x400c、0x4018..：37.125 MHz/891 Mbps MIPI timing。
 *
 * 本阶段不在数组中混入 HDR2/HDR3、12-bit、2-lane 或 binning 配置。
 */
static const struct zzh_imx415_reg zzh_imx415_mode_regs[] = {
	{ 0x3020, 0x00 },
	{ 0x3021, 0x00 },
	{ 0x3022, 0x00 },
	{ 0x3024, 0xca },
	{ 0x3025, 0x08 },
	{ 0x3028, 0x4c },
	{ 0x3029, 0x04 },
	{ 0x302c, 0x00 },
	{ 0x302d, 0x00 },
	{ 0x3033, 0x05 },
	{ 0x3050, 0x08 },
	{ 0x3051, 0x00 },
	{ 0x3054, 0x19 },
	{ 0x3058, 0x3e },
	{ 0x3060, 0x25 },
	{ 0x3064, 0x4a },
	{ 0x30cf, 0x00 },
	{ 0x3118, 0xc0 },
	{ 0x3260, 0x01 },
	{ 0x400c, 0x00 },
	{ 0x4018, 0x7f },
	{ 0x401a, 0x37 },
	{ 0x401c, 0x37 },
	{ 0x401e, 0xf7 },
	{ 0x401f, 0x00 },
	{ 0x4020, 0x3f },
	{ 0x4022, 0x6f },
	{ 0x4024, 0x3f },
	{ 0x4026, 0x5f },
	{ 0x4028, 0x2f },
	{ 0x4074, 0x01 },
	{ ZZH_IMX415_REG_END, 0x00 },
};

/* [详细注释]
 * LINK_FREQ 是 V4L2 integer-menu control。驱动只有一个模式，因此菜单也只有
 * 一个项目；control 的当前 index 固定为 0，并标记只读。
 */
static const s64 zzh_imx415_link_freq_menu[] = {
	ZZH_IMX415_LINK_FREQ_HZ,
};

/* [详细注释]
 * struct zzh_imx415 - 一个真实 Sensor 实例的全部软件状态
 *
 * 资源句柄：client、xvclk、RESET/PWDN、三路可选 regulator；
 * V4L2 对象：subdev、唯一 source pad、control handler 和六个 control 指针；
 * 并发状态：mutex、current_vts、powered/manual_power/streaming/timing_update；
 * 清理状态：每一路 enabled flag 精确记录哪些资源确实打开成功。
 *
 * 结构由 probe 中 devm_kzalloc 分配并清零，随 device 生命周期释放。V4L2
 * control/entity 等框架对象仍要在错误路径和 remove 中显式 cleanup/free。
 * mutex 同时交给 ctrl_handler->lock，串行化 ACTIVE 状态、control callback、
 * runtime PM 引用和 STREAMON/OFF，避免 VTS/曝光/电源状态相互穿插。
 */
struct zzh_imx415 {
	/* [详细注释]
	 * 第一组是 Linux device resource 句柄。devm 管理句柄生命周期，enabled
	 * 状态则由本结构底部的 flag 管理。
	 */
	struct i2c_client *client;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	struct regulator *dvdd;
	struct regulator *dovdd;
	struct regulator *avdd;

	/* [详细注释]
	 * 第二组构成用户态可见的 V4L2/Media Sensor：一个 subdev、一个 source
	 * pad、一个 handler，以及由 handler 拥有的六个 control。
	 */
	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	/* Serializes ACTIVE state, controls, PM references and streaming. */
	struct mutex mutex;

	/* [详细注释]
	 * 第三组是不能从硬件资源句柄自动推导的软件状态。current_vts 参与曝光和
	 * 帧间隔换算；enabled flags 支持精确回滚；manual_power/streaming 分别
	 * 对应两类 PM 引用；timing_update 防止 control 范围收敛时递归写寄存器。
	 */
	u32 current_vts;
	bool supplies_present;
	bool dvdd_enabled;
	bool dovdd_enabled;
	bool avdd_enabled;
	bool clock_enabled;
	bool powered;
	bool manual_power;
	bool streaming;
	bool timing_update;
};

/* [详细注释]
 * v4l2_subdev 嵌入在私有结构中。container_of 只做成员地址反推，不分配内存；
 * 所有接收 subdev 指针的回调都用它取回当前 I2C Sensor 实例。
 */
static inline struct zzh_imx415 *to_zzh_imx415(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct zzh_imx415, subdev);
}

/* [详细注释]
 * zzh_imx415_read_reg8() - 读取一个 16 位地址、8 位数据寄存器
 *
 * 第一条 i2c_msg 写 reg_hi/reg_lo，第二条 message 在 repeated-start 后读 1
 * byte。i2c_transfer 必须完成两条 message 才算成功；只完成一部分返回 -EIO。
 */
static int zzh_imx415_read_reg8(struct zzh_imx415 *imx415, u16 reg,
				u8 *value)
{
	u8 address[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg messages[2] = {
		{
			.addr = imx415->client->addr,
			.len = sizeof(address),
			.buf = address,
		},
		{
			.addr = imx415->client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = value,
		},
	};
	int ret;

	ret = i2c_transfer(imx415->client->adapter, messages,
			   ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages))
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

/* [详细注释]
 * zzh_imx415_write_reg8() - 写一个 16 位地址、8 位数据寄存器
 *
 * 栈上 message 格式为 [reg_hi, reg_lo, value]。短写不能当成成功，统一转换为
 * -EIO；负的 adapter errno 原样向上传递给 STREAMON/control/probe 调用者。
 */
static int zzh_imx415_write_reg8(struct zzh_imx415 *imx415, u16 reg,
				 u8 value)
{
	u8 message[3] = { reg >> 8, reg & 0xff, value };
	int ret;

	ret = i2c_master_send(imx415->client, message, sizeof(message));
	if (ret == sizeof(message))
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

/* [详细注释]
 * zzh_imx415_write_reg_le() - 连续写 1..4 字节参数
 *
 * 寄存器地址本身仍是高字节在前；参数值按 IMX415 连续寄存器定义从低字节到
 * 高字节写入。例如 VMAX=2250 会从 0x3024 开始写 ca 08 00。length 超过
 * 栈缓存能力或为 0 时返回 -EINVAL。
 */
static int zzh_imx415_write_reg_le(struct zzh_imx415 *imx415, u16 reg,
				   unsigned int length, u32 value)
{
	u8 message[6];
	unsigned int i;
	int ret;

	if (!length || length > 4)
		return -EINVAL;

	message[0] = reg >> 8;
	message[1] = reg & 0xff;
	for (i = 0; i < length; i++)
		message[2 + i] = value >> (8 * i);

	ret = i2c_master_send(imx415->client, message, length + 2);
	if (ret == length + 2)
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

/* [详细注释]
 * zzh_imx415_write_array() - 顺序下发以 0xffff 结束的寄存器表
 *
 * 每项都复用 write_reg8；第一处失败立即停止并记录精确地址，防止后续寄存器
 * 在不完整前提下继续生效。调用者负责让 Sensor 处于 standby 并执行回滚。
 */
static int zzh_imx415_write_array(struct zzh_imx415 *imx415,
				  const struct zzh_imx415_reg *registers)
{
	unsigned int i;
	int ret;

	for (i = 0; registers[i].address != ZZH_IMX415_REG_END; i++) {
		ret = zzh_imx415_write_reg8(imx415,
					    registers[i].address,
					    registers[i].value);
		if (ret) {
			dev_err(&imx415->client->dev,
				"register table failed at 0x%04x: %d\n",
				registers[i].address, ret);
			return ret;
		}
	}

	return 0;
}

/* [详细注释]
 * zzh_imx415_write_held() - 在一次 REGHOLD 中更新单个多字节参数
 *
 * 顺序是 hold=1、写参数、hold=0。即使参数写失败也尝试释放 REGHOLD；返回值
 * 优先保留参数错误，否则返回释放错误，避免把真正根因覆盖掉。
 */
static int zzh_imx415_write_held(struct zzh_imx415 *imx415, u16 reg,
				 unsigned int length, u32 value)
{
	int release_ret;
	int ret;

	ret = zzh_imx415_write_reg8(imx415, IMX415_REG_REGHOLD,
				    IMX415_REGHOLD_ENABLE);
	if (ret)
		return ret;

	ret = zzh_imx415_write_reg_le(imx415, reg, length, value);
	release_ret = zzh_imx415_write_reg8(imx415, IMX415_REG_REGHOLD,
					    IMX415_REGHOLD_DISABLE);

	return ret ? ret : release_ret;
}

/* [详细注释]
 * zzh_imx415_write_timing_held() - 原子更新 VMAX 和对应 SHR0
 *
 * VBLANK 改变意味着新 VTS。为了保持 V4L2 exposure“曝光行数”语义，代码在
 * 同一次 REGHOLD 中写 VMAX 和 SHR0=(vts-exposure)，不让 Sensor 短暂看到一新
 * 一旧的时序组合。无论中途成功与否，最后都尝试 release REGHOLD。
 */
static int zzh_imx415_write_timing_held(struct zzh_imx415 *imx415,
					u32 vts, u32 exposure)
{
	int release_ret;
	int ret;

	ret = zzh_imx415_write_reg8(imx415, IMX415_REG_REGHOLD,
				    IMX415_REGHOLD_ENABLE);
	if (ret)
		return ret;

	ret = zzh_imx415_write_reg_le(imx415, IMX415_REG_VMAX, 3, vts);
	if (!ret)
		ret = zzh_imx415_write_reg_le(imx415, IMX415_REG_SHR0, 3,
					      vts - exposure);

	release_ret = zzh_imx415_write_reg8(imx415, IMX415_REG_REGHOLD,
					    IMX415_REGHOLD_DISABLE);
	return ret ? ret : release_ret;
}

/* [详细注释]
 * zzh_imx415_get_optional_supply() - 获取一路允许缺省的 regulator
 *
 * 返回 1 表示句柄存在，0 表示 DTS 未声明且允许走模组本地 LDO，负值表示
 * -EPROBE_DEFER 等真正错误。devm 管理句柄内存，但 enable 后仍必须手工 disable。
 */
static int zzh_imx415_get_optional_supply(struct device *dev,
					  const char *name,
					  struct regulator **supply)
{
	int ret;

	*supply = devm_regulator_get_optional(dev, name);
	if (!IS_ERR(*supply))
		return 1;

	ret = PTR_ERR(*supply);
	if (ret == -ENODEV) {
		*supply = NULL;
		return 0;
	}

	return ret;
}

/* [详细注释]
 * zzh_imx415_get_supplies() - 校验三路 Sensor 电源描述是否自洽
 *
 * 只接受 0/3 或 3/3。当前 ATK 模组是 0/3：连接器输入 VCC3V3_SYS，模组本地
 * LDO 生成 DVDD/DOVDD/AVDD，Linux 不能逐路开关，也不能声称测过三路 rail
 * 软件时序。若未来板级 DTS 提供三路 supply，则本驱动按规定顺序控制它们。
 */
static int zzh_imx415_get_supplies(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	int count = 0;
	int ret;

	ret = zzh_imx415_get_optional_supply(dev, "dvdd", &imx415->dvdd);
	if (ret < 0)
		return ret;
	count += ret;

	ret = zzh_imx415_get_optional_supply(dev, "dovdd", &imx415->dovdd);
	if (ret < 0)
		return ret;
	count += ret;

	ret = zzh_imx415_get_optional_supply(dev, "avdd", &imx415->avdd);
	if (ret < 0)
		return ret;
	count += ret;

	if (count && count != 3) {
		dev_err(dev,
			"dvdd/dovdd/avdd must be described together, got %d/3\n",
			count);
		return -EINVAL;
	}

	imx415->supplies_present = count == 3;
	if (!imx415->supplies_present)
		dev_info(dev,
			 "using module-local 3.3V-to-AVDD/DOVDD/DVDD LDOs\n");

	return 0;
}

/* [详细注释]
 * zzh_imx415_set_clock_rate() - 请求并回读检查 37.125 MHz MCLK
 *
 * 这里只设置 rate，不打开时钟；第一条实际 MCLK 边沿由 power_on 在释放 RESET
 * 并等待 2..5 us 后产生。回读不在 35.64..37.8675 MHz 时拒绝继续 probe/恢复。
 */
static int zzh_imx415_set_clock_rate(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	unsigned long actual;
	int ret;

	ret = clk_set_rate(imx415->xvclk, ZZH_IMX415_XVCLK_HZ);
	if (ret) {
		dev_err(dev, "failed to set xvclk to %lu Hz: %d\n",
			ZZH_IMX415_XVCLK_HZ, ret);
		return ret;
	}

	actual = clk_get_rate(imx415->xvclk);
	if (actual < ZZH_IMX415_XVCLK_MIN_HZ ||
	    actual > ZZH_IMX415_XVCLK_MAX_HZ) {
		dev_err(dev, "xvclk %lu Hz is outside the Sony 0.96..1.02 range\n",
			actual);
		return -EINVAL;
	}

	return 0;
}

/* [详细注释]
 * RESET/PWDN 在 DTS 中都是 active-low。descriptor API 写逻辑 1 会输出物理低，
 * 因而本 helper 表示“断言复位并让模组进入 power-down”，不是输出高电平。
 */
static void zzh_imx415_assert_controls(struct zzh_imx415 *imx415)
{
	gpiod_set_value_cansleep(imx415->reset_gpio, 1);
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 1);
}

/* [详细注释]
 * 任何一个 enabled flag 仍为 true，都说明硬件资源没有完全关闭。这个判断用于
 * power_on 前清理残留状态，以及 remove 时即便 PM core 认为 suspended 也补清理。
 */
static bool zzh_imx415_has_enabled_resource(struct zzh_imx415 *imx415)
{
	return imx415->clock_enabled || imx415->dvdd_enabled ||
	       imx415->dovdd_enabled || imx415->avdd_enabled;
}

/* [详细注释]
 * zzh_imx415_power_off() - 幂等、尽最大努力关闭所有可控资源
 *
 * 顺序：断言 RESET/PWDN -> 关 MCLK -> AVDD -> DOVDD -> DVDD。每项只有在
 * disable 成功后才清对应 flag；若一路失败仍继续清理后续项，并返回第一项错误。
 * powered 最终由剩余 enabled flag 重新计算，不能把不完整断电伪装成成功。
 */
static int zzh_imx415_power_off(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	int first_error = 0;
	int ret;

	/* [详细注释]
	 * 先让 Sensor 物理停止，再处理 clock/rail；即使软件 powered 已异常，
	 * RESET/PWDN 也会被重新断言，因此该入口可以安全重复调用。
	 */
	zzh_imx415_assert_controls(imx415);

	if (imx415->clock_enabled) {
		clk_disable_unprepare(imx415->xvclk);
		imx415->clock_enabled = false;
	}

	if (imx415->avdd_enabled) {
		ret = regulator_disable(imx415->avdd);
		if (!ret)
			imx415->avdd_enabled = false;
		else if (!first_error)
			first_error = ret;
	}
	if (imx415->dovdd_enabled) {
		ret = regulator_disable(imx415->dovdd);
		if (!ret)
			imx415->dovdd_enabled = false;
		else if (!first_error)
			first_error = ret;
	}
	if (imx415->dvdd_enabled) {
		ret = regulator_disable(imx415->dvdd);
		if (!ret)
			imx415->dvdd_enabled = false;
		else if (!first_error)
			first_error = ret;
	}

	/* [详细注释]
	 * 只要还有任一路关闭失败，就继续把 powered 视为 true，提醒下一次
	 * power_on/remove 先处理残留资源。
	 */
	imx415->powered = zzh_imx415_has_enabled_resource(imx415);
	if (first_error)
		dev_err(dev, "power-off cleanup incomplete: %d\n", first_error);

	return first_error;
}

/* [详细注释]
 * zzh_imx415_power_on() - 执行可重复、可回滚的 Sensor 上电
 *
 * 前置清理任何残留 enabled 资源，然后设置 MCLK rate、断言控制脚、按
 * DVDD->DOVDD->AVDD 开电、保持 XCLR 低 1..2 ms、释放 PWDN/RESET、等待
 * 2..5 us、打开 MCLK，再等 20..30 us 才允许 I2C。每个成功步骤立即提交
 * enabled flag；任一步失败统一调用 power_off 逆序回滚并保留原始 errno。
 */
static int zzh_imx415_power_on(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	int cleanup_ret;
	int ret;

	/* [详细注释] 已完整上电时保持幂等，不重复 enable regulator/clock。 */
	if (imx415->powered)
		return 0;

	/* [详细注释]
	 * powered=false 但 enabled flag 非空表示上次清理不完整；不能在残留状态上
	 * 继续叠加 enable，必须先恢复到确定的全关基线。
	 */
	if (zzh_imx415_has_enabled_resource(imx415)) {
		ret = zzh_imx415_power_off(imx415);
		if (ret)
			return ret;
	}

	ret = zzh_imx415_set_clock_rate(imx415);
	if (ret)
		return ret;

	zzh_imx415_assert_controls(imx415);

	/* [详细注释]
	 * 当前 ATK 模组 supplies_present=false，会跳过这一段；保留该分支是为了
	 * 兼容三路 rail 都由 Linux 控制的其他板级实现。
	 */
	if (imx415->supplies_present) {
		ret = regulator_enable(imx415->dvdd);
		if (ret)
			goto rollback;
		imx415->dvdd_enabled = true;

		ret = regulator_enable(imx415->dovdd);
		if (ret)
			goto rollback;
		imx415->dovdd_enabled = true;

		ret = regulator_enable(imx415->avdd);
		if (ret)
			goto rollback;
		imx415->avdd_enabled = true;
	}

	/* Keep XCLR asserted well beyond Sony's 500 ns minimum. */
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 0);
	gpiod_set_value_cansleep(imx415->reset_gpio, 0);

	/* Sony requires at least 1 us from XCLR high to the first INCK edge. */
	usleep_range(2, 5);
	ret = clk_prepare_enable(imx415->xvclk);
	if (ret)
		goto rollback;
	imx415->clock_enabled = true;

	/* Sony requires at least 20 us from XCLR high to I2C traffic. */
	usleep_range(20, 30);
	imx415->powered = true;
	return 0;

rollback:
	/* [详细注释]
	 * power_off 依靠已提交的 enabled flag 精确回滚。cleanup_ret 只用于日志，
	 * 函数仍返回最初失败步骤的 ret，便于定位真正根因。
	 */
	cleanup_ret = zzh_imx415_power_off(imx415);
	if (cleanup_ret)
		dev_err(dev, "power-on rollback also failed: %d\n", cleanup_ret);
	return ret;
}

/* [详细注释]
 * runtime PM resume 回调只负责恢复硬件资源，不写 mode 表。mode 表属于每次
 * STREAMON 的 start_stream，这样 runtime suspend 彻底掉电后仍能完整重建状态。
 */
static int zzh_imx415_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	return zzh_imx415_power_on(imx415);
}

/* [详细注释]
 * runtime PM suspend 回调统一走 power_off。STREAMOFF 和手工 s_power 只释放
 * PM usage 引用，由 PM core 决定何时进入这个回调，避免各路径重复写关电逻辑。
 */
static int zzh_imx415_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	return zzh_imx415_power_off(imx415);
}

/* [详细注释]
 * zzh_imx415_set_ctrl() - 把 V4L2 control 值转换为 Sensor 寄存器写入
 *
 * ctrl_handler->lock 指向 imx415->mutex，因此回调进入时已经持锁。VBLANK 先
 * 计算 new_vts 和新的曝光上限；范围收敛可能递归触发 exposure callback，
 * timing_update 用来禁止那次递归单独写旧时序。Sensor 当前断电时只保存软件
 * control 值，不为了改参数单独唤醒；下一次 STREAMON 由 handler_setup 下发。
 *
 * 硬件在线时映射关系：
 *   VBLANK       -> REGHOLD 内写 VMAX + 最终 SHR0；
 *   EXPOSURE     -> SHR0 = current_vts - exposure；
 *   ANALOGUE_GAIN-> 两字节 GAIN。
 * 每次 pm_runtime_get_if_in_use 成功取得的临时引用最终都必须 put。
 */
static int zzh_imx415_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct zzh_imx415 *imx415 =
		container_of(ctrl->handler, struct zzh_imx415, ctrl_handler);
	struct device *dev = &imx415->client->dev;
	u32 old_vts = imx415->current_vts;
	u32 new_vts = old_vts;
	u32 exposure_max;
	u32 exp;
	int pm_ref;
	int ret = 0;

	/* [详细注释]
	 * VBLANK 是唯一会改变另一个 control 合法范围的入口。先更新软件 VTS，
	 * 再让 control core 调整 exposure max；失败时必须恢复 old_vts。
	 */
	if (ctrl->id == V4L2_CID_VBLANK) {
		new_vts = ZZH_IMX415_HEIGHT + ctrl->val;
		exposure_max = new_vts - 8;

		/*
		 * A smaller VTS can clamp the exposure control.  Suppress the
		 * nested exposure I2C write and apply VMAX plus the final SHR0 in
		 * one REGHOLD transaction below.
		 */
		imx415->current_vts = new_vts;
		imx415->timing_update = true;
		ret = __v4l2_ctrl_modify_range(imx415->exposure,
					       ZZH_IMX415_EXPOSURE_MIN,
					       exposure_max, 1,
					       min_t(u32,
						     ZZH_IMX415_EXPOSURE_DEFAULT,
						     exposure_max));
		imx415->timing_update = false;
		if (ret) {
			imx415->current_vts = old_vts;
			return ret;
		}
	}

	if (imx415->timing_update && ctrl->id == V4L2_CID_EXPOSURE)
		return 0;

	/* [详细注释]
	 * get_if_in_use 返回 0 表示设备当前休眠：只保留 control 软件值，不唤醒。
	 * 返回正值表示取得临时 PM 引用，可以安全访问 I2C；负值直接传播。
	 */
	pm_ref = pm_runtime_get_if_in_use(dev);
	if (pm_ref <= 0) {
		if (pm_ref < 0 && ctrl->id == V4L2_CID_VBLANK)
			imx415->current_vts = old_vts;
		if (pm_ref < 0)
			return pm_ref;
		return 0;
	}

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* [详细注释] VTS 和曝光相关 SHR0 必须在同一 REGHOLD 中提交。 */
		exp = imx415->exposure->val;
		ret = zzh_imx415_write_timing_held(imx415, new_vts, exp);
		if (ret)
			imx415->current_vts = old_vts;
		break;
	case V4L2_CID_EXPOSURE:
		/* [详细注释] V4L2 用曝光行数，Sensor 寄存器用倒数位置 SHR0。 */
		ret = zzh_imx415_write_held(imx415, IMX415_REG_SHR0, 3,
					    imx415->current_vts - ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		/* [详细注释] control 值直接映射两字节 GAIN，范围已由 core 检查。 */
		ret = zzh_imx415_write_held(imx415, IMX415_REG_GAIN, 2,
					    ctrl->val);
		break;
	default:
		break;
	}

	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops zzh_imx415_ctrl_ops = {
	.s_ctrl = zzh_imx415_set_ctrl,
};

/* [详细注释]
 * zzh_imx415_init_controls() - 创建固定模式的六项标准 V4L2 Controls
 *
 * LINK_FREQ、PIXEL_RATE、HBLANK 是固定元数据并标记 READ_ONLY。VBLANK、
 * EXPOSURE、ANALOGUE_GAIN 可写且共用 set_ctrl。handler 拥有各 control 对象，
 * 初始化失败或 remove 时只调用 v4l2_ctrl_handler_free，不能逐项 kfree。
 * current_vts 初始化为 2250，后续帧间隔和曝光换算都以它为软件真值。
 */
static int zzh_imx415_init_controls(struct zzh_imx415 *imx415)
{
	struct v4l2_ctrl_handler *handler = &imx415->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 6);
	if (ret)
		return ret;
	handler->lock = &imx415->mutex;

	/* [详细注释] 固定 link frequency 菜单，只有 index 0。 */
	imx415->link_freq =
		v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(zzh_imx415_link_freq_menu) - 1,
				       0, zzh_imx415_link_freq_menu);
	/* [详细注释] 固定 pixel rate，min=max=default，因此用户不能改。 */
	imx415->pixel_rate =
		v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
				  ZZH_IMX415_PIXEL_RATE,
				  ZZH_IMX415_PIXEL_RATE, 1,
				  ZZH_IMX415_PIXEL_RATE);
	/* [详细注释] HBLANK 只表达 payload 元数据，不在运行中改 HMAX。 */
	imx415->hblank =
		v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				  ZZH_IMX415_HBLANK, ZZH_IMX415_HBLANK,
				  1, ZZH_IMX415_HBLANK);
	/* [详细注释] VBLANK 最小为 2250-2192=58，最大受 0x7fff VTS 限制。 */
	imx415->vblank =
		v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
				  V4L2_CID_VBLANK,
				  ZZH_IMX415_VBLANK_DEFAULT,
				  ZZH_IMX415_VTS_MAX - ZZH_IMX415_HEIGHT,
				  1, ZZH_IMX415_VBLANK_DEFAULT);
	/* [详细注释] 默认曝光 2242 行，始终至少为 VTS 留出 8 行 SHR0 边界。 */
	imx415->exposure =
		v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
				  V4L2_CID_EXPOSURE,
				  ZZH_IMX415_EXPOSURE_MIN,
				  ZZH_IMX415_VTS_DEFAULT - 8,
				  1, ZZH_IMX415_EXPOSURE_DEFAULT);
	/* [详细注释] 模拟增益范围 0..240，step=1。 */
	imx415->analogue_gain =
		v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
				  V4L2_CID_ANALOGUE_GAIN,
				  ZZH_IMX415_GAIN_MIN, ZZH_IMX415_GAIN_MAX,
				  1, ZZH_IMX415_GAIN_MIN);

	if (imx415->link_freq)
		imx415->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	if (imx415->pixel_rate)
		imx415->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	if (imx415->hblank)
		imx415->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (handler->error) {
		ret = handler->error;
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	imx415->subdev.ctrl_handler = handler;
	imx415->current_vts = ZZH_IMX415_VTS_DEFAULT;
	return 0;
}

/* [详细注释]
 * 把任意请求收敛为唯一 ACTIVE/TRY 格式：3864x2192、SGBRG10、progressive、
 * RAW colorspace、full range。固定模式没有“选择最近分辨率”的多 mode 逻辑。
 */
static void zzh_imx415_fill_format(struct v4l2_mbus_framefmt *format)
{
	format->width = ZZH_IMX415_WIDTH;
	format->height = ZZH_IMX415_HEIGHT;
	format->code = MEDIA_BUS_FMT_SGBRG10_1X10;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;
}

/* [详细注释]
 * pad0/index0 只枚举一个 Media Bus Code。subdev/config 对固定枚举没有状态依赖；
 * 非法 pad/index 返回 -EINVAL，让调用者明确知道不存在第二个 source pad/格式。
 */
static int zzh_imx415_enum_mbus_code(struct v4l2_subdev *subdev,
				     struct v4l2_subdev_pad_config *config,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGBRG10_1X10;
	return 0;
}

/* [详细注释]
 * frame-size 枚举要求调用者指定 SGBRG10，并把 min/max 都设为 3864x2192。
 * 3840x2160 是 selection 返回的有效 crop，不在这里伪装成第二个 Sensor size。
 */
static int zzh_imx415_enum_frame_size(struct v4l2_subdev *subdev,
				      struct v4l2_subdev_pad_config *config,
				      struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index ||
	    size->code != MEDIA_BUS_FMT_SGBRG10_1X10)
		return -EINVAL;

	size->min_width = ZZH_IMX415_WIDTH;
	size->max_width = ZZH_IMX415_WIDTH;
	size->min_height = ZZH_IMX415_HEIGHT;
	size->max_height = ZZH_IMX415_HEIGHT;
	return 0;
}

/* [详细注释]
 * frame-interval 枚举只检查 pad/index，然后主动回填 code、width、height 和 1/30。
 * 这是板测后修正的关键契约：Rockchip RKCIF 内部可能只预填 index，如果驱动
 * 反过来要求 caller 先填格式尺寸，会让 RKCIF 得到 size=0 并导致 dummy buffer
 * 分配失败。当前实际帧间隔随 VBLANK 变化时由 g_frame_interval 读取。
 */
static int zzh_imx415_enum_frame_interval(struct v4l2_subdev *subdev,
					  struct v4l2_subdev_pad_config *config,
					  struct v4l2_subdev_frame_interval_enum *interval)
{
	if (interval->pad || interval->index)
		return -EINVAL;

	interval->code = MEDIA_BUS_FMT_SGBRG10_1X10;
	interval->width = ZZH_IMX415_WIDTH;
	interval->height = ZZH_IMX415_HEIGHT;
	interval->interval.numerator = 1;
	interval->interval.denominator = 30;
	return 0;
}

/* [详细注释]
 * get_fmt 区分 TRY 和 ACTIVE：TRY 状态属于每个打开的 file handle，必须从传入
 * pad_config 读取；ACTIVE 永远返回冻结格式。mutex 防止与 set_fmt/open 并发。
 */
static int zzh_imx415_get_format(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_pad_config *config,
				 struct v4l2_subdev_format *format)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	if (format->pad)
		return -EINVAL;

	mutex_lock(&imx415->mutex);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		if (!config) {
			mutex_unlock(&imx415->mutex);
			return -EINVAL;
		}
		format->format =
			*v4l2_subdev_get_try_format(subdev, config, format->pad);
#else
		mutex_unlock(&imx415->mutex);
		return -ENOTTY;
#endif
	} else {
		zzh_imx415_fill_format(&format->format);
	}
	mutex_unlock(&imx415->mutex);

	return 0;
}

/* [详细注释]
 * set_fmt 不会改变硬件 mode：无论用户请求什么都回填唯一固定格式。TRY 写入
 * 当前 file handle 的 try state；ACTIVE 只返回收敛结果，不需要保存 mode 指针。
 */
static int zzh_imx415_set_format(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_pad_config *config,
				 struct v4l2_subdev_format *format)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	if (format->pad)
		return -EINVAL;

	mutex_lock(&imx415->mutex);
	zzh_imx415_fill_format(&format->format);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		if (!config) {
			mutex_unlock(&imx415->mutex);
			return -EINVAL;
		}
		*v4l2_subdev_get_try_format(subdev, config, format->pad) =
			format->format;
#else
		mutex_unlock(&imx415->mutex);
		return -ENOTTY;
#endif
	}
	mutex_unlock(&imx415->mutex);

	return 0;
}

/* [详细注释]
 * selection 语义把 Sensor 原生输出与推荐有效窗口分开：
 *
 * NATIVE_SIZE                     -> (0,0) 3864x2192；
 * CROP_BOUNDS/CROP_DEFAULT/CROP   -> (12,16) 3840x2160。
 *
 * 本阶段 crop 固定不可写，其他 target 返回 -EINVAL。
 */
static int zzh_imx415_get_selection(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_pad_config *config,
				    struct v4l2_subdev_selection *selection)
{
	if (selection->pad)
		return -EINVAL;

	switch (selection->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
		selection->r.left = 0;
		selection->r.top = 0;
		selection->r.width = ZZH_IMX415_WIDTH;
		selection->r.height = ZZH_IMX415_HEIGHT;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		selection->r.left = ZZH_IMX415_CROP_LEFT;
		selection->r.top = ZZH_IMX415_CROP_TOP;
		selection->r.width = ZZH_IMX415_CROP_WIDTH;
		selection->r.height = ZZH_IMX415_CROP_HEIGHT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* [详细注释]
 * 向 Host 报告 CSI-2 物理总线契约：D-PHY、4 data lanes、虚拟通道 VC0、
 * continuous clock。该返回值与 DTS endpoint 和模式寄存器必须一致。
 */
static int zzh_imx415_get_mbus_config(struct v4l2_subdev *subdev,
				      unsigned int pad,
				      struct v4l2_mbus_config *config)
{
	if (pad)
		return -EINVAL;

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = V4L2_MBUS_CSI2_4_LANE |
			V4L2_MBUS_CSI2_CHANNEL_0 |
			V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	return 0;
}

/* [详细注释]
 * g_frame_interval 返回 current_vts / (2250*30)。默认 VTS=2250 时即 1/30；
 * 增大 VBLANK 后分子变大，帧周期等比例变长。读取 current_vts 时持 mutex。
 */
static int zzh_imx415_get_frame_interval(struct v4l2_subdev *subdev,
					 struct v4l2_subdev_frame_interval *interval)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	mutex_lock(&imx415->mutex);
	interval->interval.numerator = imx415->current_vts;
	interval->interval.denominator = ZZH_IMX415_VTS_DEFAULT * 30U;
	mutex_unlock(&imx415->mutex);

	return 0;
}

/* [详细注释]
 * zzh_imx415_start_stream() - 已上电前提下把 Sensor 从 standby 配成 operating
 *
 * 顺序固定为：先确保 STANDBY=1，写 global 表，写唯一 mode 表，调用带双下划线
 * 的 handler_setup 应用当前 Controls，最后 STANDBY=0。调用者已经持有 mutex，
 * 所以必须使用 __v4l2_ctrl_handler_setup，不能让 control core 再锁同一 mutex。
 * 任一步失败都尽力恢复 STANDBY=1，返回原始错误，不掩盖真正失败点。
 */
static int zzh_imx415_start_stream(struct zzh_imx415 *imx415)
{
	int standby_ret;
	int ret;

	/* [详细注释] 在改动任何 mode 寄存器前先禁止有效输出。 */
	ret = zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
				    IMX415_STANDBY);
	if (ret)
		return ret;

	/* [详细注释] 先下发全局 RAW10/tuning 基线，再下发固定 mode 时序。 */
	ret = zzh_imx415_write_array(imx415, zzh_imx415_global_regs);
	if (ret)
		goto restore_standby;

	ret = zzh_imx415_write_array(imx415, zzh_imx415_mode_regs);
	if (ret)
		goto restore_standby;

	/* [详细注释]
	 * mode 表含默认值，但用户可能在休眠时修改过 Controls；这里统一覆盖为
	 * control core 当前软件值，保证第一次有效帧使用最新参数。
	 */
	ret = __v4l2_ctrl_handler_setup(&imx415->ctrl_handler);
	if (ret)
		goto restore_standby;

	/* [详细注释] 所有配置成功后才退出 standby，避免输出半配置帧。 */
	ret = zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
				    IMX415_OPERATING);
	if (!ret)
		return 0;

restore_standby:
	/* [详细注释] 回滚写失败只额外记录，最终返回原始 start error。 */
	standby_ret = zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
					    IMX415_STANDBY);
	if (standby_ret)
		dev_err(&imx415->client->dev,
			"failed to restore standby after start error: %d\n",
			standby_ret);
	return ret;
}

/* [详细注释]
 * stop_stream 只负责写 STANDBY=1，不直接关 clock/GPIO。PM 引用释放和最终
 * runtime_suspend 由 s_stream 统一处理，使停流 I2C 错误仍能向用户态传播。
 */
static int zzh_imx415_stop_stream(struct zzh_imx415 *imx415)
{
	return zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
				     IMX415_STANDBY);
}

/* [详细注释]
 * zzh_imx415_s_stream() - V4L2 video_ops 的 STREAMON/OFF 状态机
 *
 * enable 分支先取得 runtime PM 引用，确保 resume/power_on 完成，再调用
 * start_stream；失败时释放本轮引用，streaming 保持 false。disable 分支先写
 * standby，即使写失败也清 streaming 并 put PM 引用，避免永久 usage 泄漏；
 * 若 standby 和 pm put 都报错，优先保留前面的停流错误。重复请求是幂等操作。
 */
static int zzh_imx415_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);
	struct device *dev = &imx415->client->dev;
	int pm_ret;
	int ret = 0;

	enable = !!enable;
	mutex_lock(&imx415->mutex);
	/* [详细注释] 把所有非 0 请求规范化成 1，并让重复 ON/OFF 幂等返回。 */
	if (imx415->streaming == enable)
		goto unlock;

	if (enable) {
		/* [详细注释]
		 * resume_and_get 的成功返回按本内核 API 约定为 0；成功后持有一份
		 * stream 专属 usage 引用，直到 STREAMOFF。
		 */
		ret = pm_runtime_resume_and_get(dev);
		if (ret)
			goto unlock;

		ret = zzh_imx415_start_stream(imx415);
		if (ret) {
			pm_runtime_put(dev);
			goto unlock;
		}
		imx415->streaming = true;
		dev_info(dev,
			 "stream on: 3864x2192 SGBRG10, 4 lanes, 891 Mbps/lane\n");
	} else {
		/* [详细注释]
		 * 即使 standby I2C 写失败，也必须释放 stream 引用并允许后续重新打开；
		 * 用户态仍通过返回 errno 看见这次停流不完整。
		 */
		ret = zzh_imx415_stop_stream(imx415);
		imx415->streaming = false;
		pm_ret = pm_runtime_put(dev);
		if (!ret && pm_ret < 0)
			ret = pm_ret;
		if (ret)
			dev_err(dev, "stream off completed with error: %d\n", ret);
	}

unlock:
	mutex_unlock(&imx415->mutex);
	return ret;
}

/* [详细注释]
 * s_power 为显式 Subdev 电源请求单独记一份 manual_power PM 引用。它与 stream
 * 引用分账：用户先 s_power(1) 再 STREAMON 时会有两份引用，各自关闭各自释放，
 * 不会因为停流而错误关闭仍被手工请求保持的 Sensor 电源。
 */
static int zzh_imx415_s_power(struct v4l2_subdev *subdev, int enable)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);
	struct device *dev = &imx415->client->dev;
	int ret = 0;

	enable = !!enable;
	mutex_lock(&imx415->mutex);
	if (imx415->manual_power == enable)
		goto unlock;

	if (enable) {
		ret = pm_runtime_resume_and_get(dev);
		if (!ret)
			imx415->manual_power = true;
	} else {
		ret = pm_runtime_put(dev);
		if (ret >= 0) {
			imx415->manual_power = false;
			ret = 0;
		}
	}

unlock:
	mutex_unlock(&imx415->mutex);
	return ret;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
/* [详细注释]
 * 每次打开 /dev/v4l-subdevX 时初始化该 file handle 的 pad0 TRY format。open
 * 不上电、不写 Sensor、不申请图像 Buffer；它只建立用户态格式协商初值。
 */
static int zzh_imx415_open(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_fh *file_handle)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);
	struct v4l2_mbus_framefmt *try_format;

	mutex_lock(&imx415->mutex);
	try_format = v4l2_subdev_get_try_format(subdev, file_handle->pad, 0);
	zzh_imx415_fill_format(try_format);
	mutex_unlock(&imx415->mutex);
	return 0;
}

static const struct v4l2_subdev_internal_ops zzh_imx415_internal_ops = {
	.open = zzh_imx415_open,
};
#endif

/* [详细注释]
 * 三组 V4L2 operations 分工：
 *
 * core_ops：显式电源和 control event 订阅；
 * video_ops：STREAMON/OFF 和当前帧间隔；
 * pad_ops：mbus code、尺寸、名义间隔、格式、selection、总线配置。
 *
 * 最终 subdev_ops 只是把三张表挂到一个 V4L2 Subdev 上，不在表中保存状态。
 */
static const struct v4l2_subdev_core_ops zzh_imx415_core_ops = {
	.s_power = zzh_imx415_s_power,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops zzh_imx415_video_ops = {
	.s_stream = zzh_imx415_s_stream,
	.g_frame_interval = zzh_imx415_get_frame_interval,
};

static const struct v4l2_subdev_pad_ops zzh_imx415_pad_ops = {
	.enum_mbus_code = zzh_imx415_enum_mbus_code,
	.enum_frame_size = zzh_imx415_enum_frame_size,
	.enum_frame_interval = zzh_imx415_enum_frame_interval,
	.get_fmt = zzh_imx415_get_format,
	.set_fmt = zzh_imx415_set_format,
	.get_selection = zzh_imx415_get_selection,
	.get_mbus_config = zzh_imx415_get_mbus_config,
};

static const struct v4l2_subdev_ops zzh_imx415_subdev_ops = {
	.core = &zzh_imx415_core_ops,
	.video = &zzh_imx415_video_ops,
	.pad = &zzh_imx415_pad_ops,
};

/* [详细注释]
 * Media Controller 在建立 link 时调用标准 link_validate，核对相邻 entity 的
 * pad format 是否兼容。Sensor 本身只有一个 MEDIA_PAD_FL_SOURCE pad。
 */
static const struct media_entity_operations zzh_imx415_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* [详细注释]
 * zzh_imx415_check_endpoint() - 在注册 Subdev 前验证 DTS CSI-2 endpoint
 *
 * fwnode parser 动态分配 lane/frequency 数组，因此所有出口都必须调用
 * v4l2_fwnode_endpoint_free。当前只接受恰好四条 data lane 和唯一 445500000
 * link frequency；缺 endpoint、lane 数错误或频率错误都让 probe 失败，避免
 * Sensor mode 与 D-PHY 配置不一致后再以无帧/CRC 错误形式暴露。
 */
static int zzh_imx415_check_endpoint(struct device *dev)
{
	struct v4l2_fwnode_endpoint endpoint = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *handle;
	int ret;

	/* [详细注释] Sensor 固定只有一个输出 endpoint，取不到就拒绝注册。 */
	handle = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!handle) {
		dev_err(dev, "CSI-2 endpoint is missing\n");
		return -EINVAL;
	}

	/* [详细注释]
	 * alloc_parse 会复制可变长 lane/frequency 属性；解析后立即 put 原 fwnode，
	 * 最终再用 endpoint_free 释放解析结果。
	 */
	ret = v4l2_fwnode_endpoint_alloc_parse(handle, &endpoint);
	fwnode_handle_put(handle);
	if (ret) {
		dev_err(dev, "failed to parse CSI-2 endpoint: %d\n", ret);
		goto free_endpoint;
	}

	/* [详细注释] 驱动没有 2-lane 寄存器表，因此不能静默接受两条 lane。 */
	if (endpoint.bus.mipi_csi2.num_data_lanes != ZZH_IMX415_LANES) {
		dev_err(dev, "expected 4 CSI-2 lanes, got %u\n",
			endpoint.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto free_endpoint;
	}

	/* [详细注释] 多一个或少一个频率都意味着 DTS 与单模式契约不一致。 */
	if (endpoint.nr_of_link_frequencies != 1 ||
	    endpoint.link_frequencies[0] != ZZH_IMX415_LINK_FREQ_HZ) {
		dev_err(dev,
			"endpoint must contain only link-frequencies = <445500000>\n");
		ret = -EINVAL;
		goto free_endpoint;
	}

	ret = 0;

free_endpoint:
	v4l2_fwnode_endpoint_free(&endpoint);
	return ret;
}

/* [详细注释]
 * 临时上电后读取 0x311a 并检查 0xe0。这个值用于确认当前硬件响应与厂商参考
 * 路径一致；代码和日志都明确不把它称为 Sony 官方唯一 Chip ID。
 */
static int zzh_imx415_check_reference(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	u8 value;
	int ret;

	ret = zzh_imx415_read_reg8(imx415, IMX415_REFERENCE_REG, &value);
	if (ret) {
		dev_err(dev, "failed to read reference register 0x%04x: %d\n",
			IMX415_REFERENCE_REG, ret);
		return ret;
	}

	dev_info(dev, "reference register 0x%04x = 0x%02x\n",
		 IMX415_REFERENCE_REG, value);
	if (value != IMX415_REFERENCE_VALUE) {
		dev_err(dev,
			"vendor reference signature mismatch: expected 0x%02x, got 0x%02x\n",
			IMX415_REFERENCE_VALUE, value);
		return -ENODEV;
	}

	dev_info(dev,
		 "vendor reference signature matched (not an official unique Chip ID)\n");
	return 0;
}

/* [详细注释]
 * zzh_imx415_probe() - 从 I2C client 建立完整 V4L2 Sensor 实例
 *
 * 主流程分为七步：
 *
 * 1. 检查 adapter 支持原生 I2C、地址必须是 0x1a；
 * 2. devm 分配私有结构，初始化 mutex 和嵌入式 v4l2_subdev；
 * 3. 校验 endpoint，获取 MCLK、RESET/PWDN 和可选 regulator；
 * 4. 创建 Controls，临时上电并检查厂商参考签名；
 * 5. 建立一个 Sensor source pad 和 Media Entity；
 * 6. async 注册 Sensor，让 D-PHY/RKCIF notifier 能匹配 graph；
 * 7. 把当前已上电状态交给 runtime PM，enable 后立即 idle，空闲时自动断电。
 *
 * goto 标签严格按已完成对象的相反顺序清理：entity -> power -> controls ->
 * mutex。devm 资源由 device core 自动释放，因此错误路径不手工 put 它们。
 */
static int zzh_imx415_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct zzh_imx415 *imx415;
	struct v4l2_subdev *subdev;
	int ret;

	/* [详细注释] 寄存器读取需要 repeated-start 原生 I2C，不接受只支持 SMBus。 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->addr != ZZH_IMX415_I2C_ADDR) {
		dev_err(dev, "expected 7-bit I2C address 0x1a, got 0x%02x\n",
			client->addr);
		return -EINVAL;
	}

	/* [详细注释]
	 * devm_kzalloc 清零全部状态 flag；GFP_KERNEL 适合允许睡眠的 probe 上下文。
	 * v4l2_i2c_subdev_init 同时把 subdev 写入 clientdata，PM/remove 都依赖它。
	 */
	imx415 = devm_kzalloc(dev, sizeof(*imx415), GFP_KERNEL);
	if (!imx415)
		return -ENOMEM;

	imx415->client = client;
	mutex_init(&imx415->mutex);
	subdev = &imx415->subdev;
	v4l2_i2c_subdev_init(subdev, client, &zzh_imx415_subdev_ops);

	/* [详细注释] 先验证不可运行的固件契约，再申请并开硬件资源。 */
	ret = zzh_imx415_check_endpoint(dev);
	if (ret)
		goto destroy_mutex;

	imx415->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx415->xvclk)) {
		ret = PTR_ERR(imx415->xvclk);
		goto destroy_mutex;
	}

	/* [详细注释]
	 * GPIOD_OUT_HIGH 是 descriptor 逻辑高；配合 ACTIVE_LOW DTS，申请瞬间就把
	 * 物理 RESET/PWDN 拉低，避免资源初始化中途意外释放 Sensor。
	 */
	imx415->reset_gpio =
		devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(imx415->reset_gpio)) {
		ret = PTR_ERR(imx415->reset_gpio);
		goto destroy_mutex;
	}

	imx415->pwdn_gpio =
		devm_gpiod_get(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(imx415->pwdn_gpio)) {
		ret = PTR_ERR(imx415->pwdn_gpio);
		goto destroy_mutex;
	}

	ret = zzh_imx415_get_supplies(imx415);
	if (ret)
		goto destroy_mutex;

	ret = zzh_imx415_init_controls(imx415);
	if (ret)
		goto destroy_mutex;

	/* [详细注释]
	 * probe 只临时上电做参考签名检查。注册完成后 pm_runtime_idle 会把它关掉，
	 * 不会因为驱动内建并成功绑定就长期耗电或持续输出 MIPI。
	 */
	ret = zzh_imx415_power_on(imx415);
	if (ret)
		goto free_controls;

	ret = zzh_imx415_check_reference(imx415);
	if (ret)
		goto power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	subdev->internal_ops = &zzh_imx415_internal_ops;
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			 V4L2_SUBDEV_FL_HAS_EVENTS;
#endif

#ifdef CONFIG_MEDIA_CONTROLLER
	/* [详细注释]
	 * Sensor 只有一个输出 pad；entity.function 让 media-ctl 把它识别为 CAM_SENSOR。
	 */
	imx415->pad.flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	subdev->entity.ops = &zzh_imx415_entity_ops;
	ret = media_entity_pads_init(&subdev->entity, 1, &imx415->pad);
	if (ret)
		goto power_off;
#endif

	/* [详细注释]
	 * async 注册是 D-PHY/RKCIF notifier 发现 Sensor entity 的关键步骤。当前驱动
	 * 内建正是为了让这一步发生在 Rockchip 下游 notifier 完成之前。
	 */
	ret = v4l2_async_register_subdev_sensor_common(subdev);
	if (ret)
		goto cleanup_entity;

	/* [详细注释]
	 * 此时 probe 临时上电尚未关，所以先告诉 PM core 状态是 active，再 enable；
	 * idle 检查 usage=0 后调用 runtime_suspend，把硬件交给按需唤醒状态机。
	 */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	dev_info(dev,
		 "registered independent 3864x2192 RAW10 30 fps Sensor subdev\n");
	return 0;

cleanup_entity:
	/* [详细注释] 以下标签按创建顺序严格逆序释放，避免双 free 或遗漏。 */
#ifdef CONFIG_MEDIA_CONTROLLER
	media_entity_cleanup(&subdev->entity);
#endif
power_off:
	zzh_imx415_power_off(imx415);
free_controls:
	v4l2_ctrl_handler_free(&imx415->ctrl_handler);
destroy_mutex:
	mutex_destroy(&imx415->mutex);
	return ret;
}

/* [详细注释]
 * zzh_imx415_remove() - 解绑时撤销所有框架对象并确保最终断电
 *
 * 先 unregister async subdev，阻止新 graph/stream 调用；再清 entity 和 controls；
 * disable runtime PM 后检查 PM 状态与 enabled flags，必要时补 power_off；最后
 * 标成 suspended 并销毁 mutex。返回清理错误，让解绑路径保留真实失败信息。
 */
static int zzh_imx415_remove(struct i2c_client *client)
{
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);
	int ret = 0;

	v4l2_async_unregister_subdev(subdev);
#ifdef CONFIG_MEDIA_CONTROLLER
	media_entity_cleanup(&subdev->entity);
#endif
	v4l2_ctrl_handler_free(&imx415->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev) ||
	    zzh_imx415_has_enabled_resource(imx415))
		ret = zzh_imx415_power_off(imx415);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&imx415->mutex);
	return ret;
}

/* [详细注释]
 * 把 runtime_suspend/resume 注册给 PM core。SET_RUNTIME_PM_OPS 的 idle 参数
 * 留空；usage counter 归零后的挂起时机由通用 runtime PM 框架管理。
 */
static const struct dev_pm_ops zzh_imx415_pm_ops = {
	SET_RUNTIME_PM_OPS(zzh_imx415_runtime_suspend,
			   zzh_imx415_runtime_resume, NULL)
};

/* [详细注释]
 * OF 表匹配阶段二 DTS 的 compatible="zzh,imx415"。它与阶段一
 * "zzh,imx415-minimal"、原厂 "sony,imx415" 完全隔离，避免同一节点误绑定。
 */
static const struct of_device_id zzh_imx415_of_match[] = {
	{ .compatible = "zzh,imx415" },
	{ }
};
MODULE_DEVICE_TABLE(of, zzh_imx415_of_match);

/* [详细注释]
 * 传统 I2C ID 表用于非 OF 实例化和生成 i2c modalias；当前开发板主要走 OF
 * 匹配。空表项是内核遍历终止标志。
 */
static const struct i2c_device_id zzh_imx415_id[] = {
	{ ZZH_IMX415_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zzh_imx415_id);

/* [详细注释]
 * i2c_driver 聚合名称、runtime PM、OF/ID 匹配和 probe/remove。当前板端通过
 * kernel-tree wrapper 以 built-in 形式注册，确保早于 Rockchip camera async
 * notifier 完成；外置模块构建仍可复用相同权威源码做对照。
 */
static struct i2c_driver zzh_imx415_i2c_driver = {
	.driver = {
		.name = ZZH_IMX415_NAME,
		.pm = &zzh_imx415_pm_ops,
		.of_match_table = zzh_imx415_of_match,
	},
	.probe = zzh_imx415_probe,
	.remove = zzh_imx415_remove,
	.id_table = zzh_imx415_id,
};

/* [详细注释]
 * module_i2c_driver 对模块构建生成 init/exit；当 CONFIG=y 时等价地生成内建
 * initcall。下面的 MODULE_* 元数据仍会进入 built-in modinfo 索引。
 */
module_i2c_driver(zzh_imx415_i2c_driver);

MODULE_DESCRIPTION("ZZH independent IMX415 fixed-mode V4L2 sensor driver");
MODULE_AUTHOR("zzh");
MODULE_LICENSE("GPL v2");
