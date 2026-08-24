// SPDX-License-Identifier: GPL-2.0
/*
 * 文件角色
 * ========
 * 这是阶段一最小驱动的“详细注释教学镜像”，不参与 Kbuild。
 * 权威源码：projects/rk3588_camera_bsp/driver/zzh_imx415_minimal.c
 *
 * 这份驱动只证明以下最小闭环：
 *   Device Tree 创建 I2C client
 *     -> compatible 匹配本驱动
 *     -> probe 获取 clock/GPIO/可选 regulator
 *     -> 按硬件契约上电
 *     -> I2C 读取 0x311a
 *     -> 校验厂商参考复位签名 0xe0
 *     -> 无论成功失败都安全断电
 *
 * 它故意没有 mode table、V4L2 Subdev、Controls、s_stream()、Media Graph、
 * RKCIF 或 RKISP。0x311a 在 Sony 公开手册中是 INCKSEL4，不是公开的唯一
 * Chip ID；0xe0 只能称为厂商参考识别签名。
 */

/* Common Clock Framework：struct clk、clk_set_rate、clk_prepare_enable。 */
#include <linux/clk.h>
/* usleep_range：实现满足手册下限、又允许调度器合并的微秒级等待。 */
#include <linux/delay.h>
/* IS_ERR/PTR_ERR：Linux 资源获取 API 的 error-pointer 处理。 */
#include <linux/err.h>
/* descriptor GPIO API：不在驱动中硬编码 GPIO controller register。 */
#include <linux/gpio/consumer.h>
/* I2C client/message/driver 结构和 i2c_transfer。 */
#include <linux/i2c.h>
/* ARRAY_SIZE、bool 等通用内核定义。 */
#include <linux/kernel.h>
/* module_i2c_driver、MODULE_* 元数据。 */
#include <linux/module.h>
/* Device Tree/OF 匹配相关定义。 */
#include <linux/of.h>
/* regulator consumer API；本板实际走模组本地 LDO 分支。 */
#include <linux/regulator/consumer.h>

/*
 * IMX415 在当前硬件绑带下的 7 位 I2C 地址。
 * Linux/DTS 使用 0x1a；控制器在线上自动生成写地址 0x34、读地址 0x35。
 */
#define ZZH_IMX415_I2C_ADDR                    0x1a

/* 目标外部输入时钟 INCK/MCLK：37.125 MHz。 */
#define ZZH_IMX415_XVCLK_HZ                    37125000UL
/* Sony 允许下限：37.125 MHz * 0.96 = 35.640 MHz。 */
#define ZZH_IMX415_XVCLK_MIN_HZ                35640000UL
/* Sony 允许上限：37.125 MHz * 1.02 = 37.8675 MHz。 */
#define ZZH_IMX415_XVCLK_MAX_HZ                37867500UL

/* 厂商参考驱动用来识别设备的寄存器地址；公开名为 INCKSEL4。 */
#define IMX415_REFERENCE_SIGNATURE_REG         0x311a
/* 上述寄存器公开复位值和厂商参考驱动期望值。 */
#define IMX415_REFERENCE_SIGNATURE_VALUE       0xe0

/*
 * struct zzh_imx415_minimal - 一个 I2C Sensor 实例的私有状态
 *
 * 申请位置：probe 中 devm_kzalloc(dev, sizeof(*imx415), GFP_KERNEL)。
 * 内存内容：只存放这个 imx415@1a 实例的资源句柄和当前电源状态，不存图像帧、
 *           mode 寄存器表或 I2C 大缓存。
 * 生命周期：与 struct device 绑定；probe 失败或 device 释放时由 devres 自动释放。
 * 并发边界：阶段一没有 streaming/用户 ioctl，probe/remove 串行执行，因此没有
 *           mutex。未来加入 V4L2 和 runtime PM 后必须重新设计状态锁。
 *
 * @client: I2C core 创建的 client；包含 adapter、7 位地址和 struct device。
 * @xvclk:  从 DTS clock-names="xvclk" 获取的 MCLK 句柄。
 * @reset_gpio: RESET/XCLR descriptor；DTS 定义为 active-low。
 * @pwdn_gpio:  模组 PWDN descriptor；DTS 定义为 active-low。
 * @dvdd: 可选 1.2 V 数字核电源句柄；ATK 模组场景为空。
 * @dovdd: 可选 1.8 V I/O 电源句柄；ATK 模组场景为空。
 * @avdd: 可选约 2.8 V 模拟电源句柄；ATK 模组场景为空。
 * @supplies_present: 三路 regulator 是否“全部存在”；不允许只声明一部分。
 * @powered: 本驱动是否已经成功执行 power_on 且尚未 power_off。
 */
struct zzh_imx415_minimal {
	struct i2c_client *client;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	struct regulator *dvdd;
	struct regulator *dovdd;
	struct regulator *avdd;
	bool supplies_present;
	bool powered;
};

/**
 * zzh_imx415_read_reg() - 从一个 16 位地址寄存器读取 8 位数据
 * @client: 已由 I2C core 注册的目标 client；使用其中 adapter 和 7 位地址。
 * @reg:    要读取的 16 位寄存器地址，例如 0x311a。
 * @value:  输出参数；成功时写入 Sensor 返回的 1 byte 数据。
 *
 * 栈内存说明：
 * @address 是 2-byte 临时地址缓存，按 MSB first 保存 reg；函数返回即销毁。
 * @messages 是两项 I2C transaction 描述，不保存数据副本：第一项指向 address，
 * 第二项直接指向调用者提供的 value。
 * @ret 保存 i2c_transfer 返回的“已完成 message 数”或负 errno。
 *
 * 传输形态：START + 0x34 + reg_hi + reg_lo + repeated START + 0x35 + data + STOP。
 *
 * Return: 0 表示两条 message 全部成功；负 errno 表示总线错误；控制器只完成
 *         部分 message 时返回 -EIO，避免把半次传输误判为成功。
 */
static int zzh_imx415_read_reg(struct i2c_client *client, u16 reg, u8 *value)
{
	u8 address[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg messages[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(address),
			.buf = address,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = value,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages))
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

/**
 * zzh_imx415_write_reg() - 向一个 16 位地址寄存器写入 8 位数据
 * @client: 目标 I2C client。
 * @reg:    16 位寄存器地址。
 * @value:  要写入的 8 位寄存器值。
 *
 * @message 是 3-byte 栈缓存：[reg_hi, reg_lo, value]。
 * 阶段一不调用这个函数，也不写 Sensor；保留它是为了完整验证寄存器协议封装。
 * __maybe_unused 避免当前未调用时产生编译告警。
 *
 * Return: 0 表示 3 byte 全部发出；负 errno 原样返回；短写返回 -EIO。
 */
static int __maybe_unused zzh_imx415_write_reg(struct i2c_client *client,
					       u16 reg, u8 value)
{
	u8 message[3] = { reg >> 8, reg & 0xff, value };
	int ret;

	ret = i2c_master_send(client, message, sizeof(message));
	if (ret == sizeof(message))
		return 0;
	if (ret < 0)
		return ret;
	return -EIO;
}

/**
 * zzh_imx415_get_optional_supply() - 获取一路允许缺省的 regulator
 * @dev:    用于 devres 生命周期和错误日志的 Sensor device。
 * @name:   DTS supply 基础名，如 "dvdd"，consumer API 会查 dvdd-supply。
 * @supply: 输出参数；存在时写 regulator 句柄，缺省时写 NULL。
 *
 * 这个函数使用三态返回值，和普通“0 成功”API 不同：
 *   1：该 regulator 存在并成功获取；
 *   0：DTS 没有描述，允许由模组本地 LDO 提供；
 *  <0：真正错误，例如 -EPROBE_DEFER，调用者必须停止 probe。
 *
 * devm_regulator_get_optional 返回的句柄由 devres 管理，不需要手工 kfree；
 * 但 enable 成功后的电源状态仍必须由本驱动 disable。
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

/**
 * zzh_imx415_get_supplies() - 获取并校验 DVDD/DOVDD/AVDD 描述的一致性
 * @imx415: 当前设备私有状态；函数填充三个 regulator 指针和状态位。
 *
 * @dev 是 client->dev 的便捷局部指针。
 * @count 统计成功获取了几路 supply，只允许 0 或 3。
 * @ret 复用保存每次 helper 返回值，负数立即向上返回。
 *
 * 为什么拒绝 1/3 或 2/3：部分 supply 会使上/下电顺序不完整，驱动无法知道
 * 剩余电源由谁控制。ATK 模组正确情况是 0/3，因为三路都由本地无 enable LDO
 * 从连接器 3.3 V 生成。
 *
 * Return: 0 表示资源描述自洽；负 errno 表示资源错误或部分声明。
 */
static int zzh_imx415_get_supplies(struct zzh_imx415_minimal *imx415)
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

/**
 * zzh_imx415_set_clock_rate() - 设置并回读检查 Sensor 外部输入时钟
 * @imx415: 包含已获取 xvclk 句柄的设备状态。
 *
 * @dev 用于日志；@rate 保存 clock framework 最终实际给出的频率；@ret 保存
 * clk_set_rate 错误。请求值不代表硬件一定精确产生，因此必须 clk_get_rate 回读。
 *
 * 本函数只设置 rate，不打开 clock。真正的第一条 INCK 边沿由 power_on 在释放
 * XCLR 并等待至少 1 us 后通过 clk_prepare_enable 产生。
 *
 * Return: 0 表示实际频率落在 0.96~1.02 允许范围；否则为负 errno。
 */
static int zzh_imx415_set_clock_rate(struct zzh_imx415_minimal *imx415)
{
	struct device *dev = &imx415->client->dev;
	unsigned long rate;
	int ret;

	ret = clk_set_rate(imx415->xvclk, ZZH_IMX415_XVCLK_HZ);
	if (ret) {
		dev_err(dev, "failed to set xvclk to %lu Hz: %d\n",
			ZZH_IMX415_XVCLK_HZ, ret);
		return ret;
	}

	rate = clk_get_rate(imx415->xvclk);
	if (rate < ZZH_IMX415_XVCLK_MIN_HZ ||
	    rate > ZZH_IMX415_XVCLK_MAX_HZ) {
		dev_err(dev,
			"xvclk rate %lu Hz is outside the IMX415 0.96..1.02 range\n",
			rate);
		return -EINVAL;
	}

	dev_dbg(dev, "xvclk configured at %lu Hz\n", rate);
	return 0;
}

/**
 * zzh_imx415_assert_controls() - 把 RESET 和 PWDN 都置为有效状态
 * @imx415: 包含两个 GPIO descriptor 的设备状态。
 *
 * DTS 将两个 GPIO 标为 GPIO_ACTIVE_LOW，所以 descriptor API 的逻辑值 1 会
 * 转换成物理低电平。函数没有返回值；cansleep 版本允许 GPIO 控制器访问睡眠。
 */
static void zzh_imx415_assert_controls(struct zzh_imx415_minimal *imx415)
{
	gpiod_set_value_cansleep(imx415->reset_gpio, 1);
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 1);
}

/**
 * zzh_imx415_power_on() - 执行阶段一可控制部分的严格上电时序
 * @imx415: 已获得全部资源、但尚未上电的设备状态。
 *
 * 局部变量：
 * @dev: 日志上下文。
 * @dvdd_on/@dovdd_on/@avdd_on: 记录每一路是否由本次调用成功 enable；只在
 *   rollback 时关闭已经打开的资源，避免 disable 未 enable 的 regulator。
 * @clock_on: 同样记录 xvclk 是否已经 enable。
 * @ret: 当前失败步骤的 errno，rollback 后原样返回。
 *
 * ATK 模组 supplies_present=false，因此三路 regulator 代码不会执行；实际 3.3 V
 * 必须在 probe 前稳定。本函数仍保留另一块板上三路都可控时的顺序实现。
 *
 * Return: 0 表示时序完成、可以通信并设置 powered=true；失败时逆序回滚并返回
 *         负 errno，powered 保持 false。
 */
static int zzh_imx415_power_on(struct zzh_imx415_minimal *imx415)
{
	struct device *dev = &imx415->client->dev;
	bool dvdd_on = false;
	bool dovdd_on = false;
	bool avdd_on = false;
	bool clock_on = false;
	int ret;

	/* 在改变电源前先确保 XCLR/PWDN 为物理低。 */
	zzh_imx415_assert_controls(imx415);

	if (imx415->supplies_present) {
		/* Sony 手册要求的上升方向：DVDD -> DOVDD/OVDD -> AVDD。 */
		ret = regulator_enable(imx415->dvdd);
		if (ret)
			goto rollback;
		dvdd_on = true;

		ret = regulator_enable(imx415->dovdd);
		if (ret)
			goto rollback;
		dovdd_on = true;

		ret = regulator_enable(imx415->avdd);
		if (ret)
			goto rollback;
		avdd_on = true;
	}

	/* 1~2 ms 远大于 XCLR 在电源稳定后保持低至少 500 ns 的要求。 */
	usleep_range(1000, 2000);

	/* descriptor 逻辑 0 对 active-low GPIO 表示物理释放为高。 */
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 0);
	gpiod_set_value_cansleep(imx415->reset_gpio, 0);

	/* XCLR high 到第一条 INCK 上升沿要求 >= 1 us，这里等待 2~5 us。 */
	usleep_range(2, 5);
	ret = clk_prepare_enable(imx415->xvclk);
	if (ret) {
		dev_err(dev, "failed to enable xvclk: %d\n", ret);
		goto rollback;
	}
	clock_on = true;

	/* XCLR high 到寄存器通信要求 >= 20 us。 */
	usleep_range(20, 30);
	imx415->powered = true;
	return 0;

rollback:
	/* 失败回滚先禁止 Sensor 活动，再按上电逆序关闭已经成功的资源。 */
	zzh_imx415_assert_controls(imx415);
	if (clock_on)
		clk_disable_unprepare(imx415->xvclk);
	if (avdd_on)
		regulator_disable(imx415->avdd);
	if (dovdd_on)
		regulator_disable(imx415->dovdd);
	if (dvdd_on)
		regulator_disable(imx415->dvdd);
	return ret;
}

/**
 * zzh_imx415_power_off() - 幂等关闭 Sensor 可控资源
 * @imx415: 当前设备状态。
 *
 * @dev 用于错误日志；@first_error 只保存第一项 regulator disable 错误，但函数
 * 仍继续关闭后续资源，尽最大努力完成清理；@ret 保存每次 disable 返回值。
 *
 * 幂等含义：powered 已经是 false 时再次调用不会重复 disable clock/regulator，
 * 但仍会断言 RESET/PWDN，确保 remove 或失败清理后的物理控制状态安全。
 *
 * Return: 0 表示所有清理成功；否则返回第一次清理错误。
 */
static int zzh_imx415_power_off(struct zzh_imx415_minimal *imx415)
{
	struct device *dev = &imx415->client->dev;
	int first_error = 0;
	int ret;

	if (!imx415->powered) {
		zzh_imx415_assert_controls(imx415);
		return 0;
	}

	/* 先让 Sensor 停止，再关闭 INCK，最后才允许可控电源下降。 */
	zzh_imx415_assert_controls(imx415);
	clk_disable_unprepare(imx415->xvclk);

	if (imx415->supplies_present) {
		/* 下降顺序与上升相反：AVDD -> DOVDD -> DVDD。 */
		ret = regulator_disable(imx415->avdd);
		if (ret && !first_error)
			first_error = ret;
		ret = regulator_disable(imx415->dovdd);
		if (ret && !first_error)
			first_error = ret;
		ret = regulator_disable(imx415->dvdd);
		if (ret && !first_error)
			first_error = ret;
	}

	imx415->powered = false;
	if (first_error)
		dev_err(dev, "power-off regulator cleanup failed: %d\n",
			first_error);
	return first_error;
}

/**
 * zzh_imx415_probe() - OF/I2C 匹配成功后的最小识别入口
 * @client: I2C core 为 DTS imx415-minimal@1a 创建的 client。
 * @id:     旧式 I2C ID table 匹配项；OF 匹配路径下可为空，本函数不需要使用。
 *
 * 内存和变量：
 * @dev 指向 client 内嵌的 struct device，不新申请内存。
 * @imx415 指向 devm_kzalloc 创建的私有状态，初始全 0；用于整个绑定生命周期。
 * @signature 是 1-byte 栈变量，接收 0x311a 读取结果。
 * @ret 保存每一步返回值，并决定进入 power_off 还是直接失败。
 * power_off 内部的 @off_ret 保存清理错误；只有主流程成功时才用它覆盖 ret，
 * 从而不掩盖更早发生的 I2C/签名错误。
 *
 * 成功 probe 的特殊行为：驱动会保持绑定，但读取签名后立即 power_off，所以
 * Sensor 不会因“识别成功”一直保持 MCLK/运行状态。
 *
 * Return: 0 表示参考签名匹配且断电成功；负 errno 会让 driver core 认为 probe
 *         失败，不在 client 上建立成功绑定。
 */
static int zzh_imx415_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct zzh_imx415_minimal *imx415;
	u8 signature = 0;
	int ret;

	/* 本驱动需要 repeated-start 的原生 I2C transaction，不接受仅 SMBus adapter。 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "adapter lacks raw I2C transfer support\n");
		return -EOPNOTSUPP;
	}

	/* DTS 写错地址时明确失败，避免在意外地址上发送协议数据。 */
	if (client->addr != ZZH_IMX415_I2C_ADDR) {
		dev_err(dev,
			"unexpected 7-bit address 0x%02x (expected 0x%02x)\n",
			client->addr, ZZH_IMX415_I2C_ADDR);
		return -EINVAL;
	}

	/*
	 * GFP_KERNEL 允许睡眠，适合 probe 上下文。
	 * devm_kzalloc 把整个结构清零，使 supplies_present/powered 初始为 false。
	 */
	imx415 = devm_kzalloc(dev, sizeof(*imx415), GFP_KERNEL);
	if (!imx415)
		return -ENOMEM;
	imx415->client = client;

	/* remove 通过 i2c_get_clientdata 取回同一个私有状态。 */
	i2c_set_clientdata(client, imx415);

	dev_info(dev,
		 "probing adapter %s, 7-bit addr 0x%02x (wire write/read 0x34/0x35)\n",
		 client->adapter->name, client->addr);

	/* "xvclk" 必须和 DTS clock-names 完全一致。句柄由 devres 管理。 */
	imx415->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx415->xvclk)) {
		ret = PTR_ERR(imx415->xvclk);
		dev_err(dev, "failed to get xvclk: %d\n", ret);
		return ret;
	}

	/*
	 * GPIOD_OUT_HIGH 表示初始逻辑 1；结合 active-low DTS，物理立即拉低，
	 * 所以资源刚申请时 Sensor 已保持 reset/power-down，不会先短暂释放。
	 */
	imx415->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(imx415->reset_gpio)) {
		ret = PTR_ERR(imx415->reset_gpio);
		dev_err(dev, "failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	imx415->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(imx415->pwdn_gpio)) {
		ret = PTR_ERR(imx415->pwdn_gpio);
		dev_err(dev, "failed to get pwdn-gpios: %d\n", ret);
		return ret;
	}

	ret = zzh_imx415_get_supplies(imx415);
	if (ret)
		return ret;

	ret = zzh_imx415_set_clock_rate(imx415);
	if (ret)
		return ret;

	ret = zzh_imx415_power_on(imx415);
	if (ret)
		return ret;

	ret = zzh_imx415_read_reg(client,
				  IMX415_REFERENCE_SIGNATURE_REG,
				  &signature);
	if (ret) {
		dev_err(dev, "failed to read reference register 0x%04x: %d\n",
			IMX415_REFERENCE_SIGNATURE_REG, ret);
		goto power_off;
	}

	dev_info(dev, "reference register 0x%04x = 0x%02x\n",
		 IMX415_REFERENCE_SIGNATURE_REG, signature);

	if (signature != IMX415_REFERENCE_SIGNATURE_VALUE) {
		dev_err(dev,
			"reference signature mismatch: expected 0x%02x, got 0x%02x\n",
			IMX415_REFERENCE_SIGNATURE_VALUE, signature);
		ret = -ENODEV;
		goto power_off;
	}

	dev_info(dev,
		 "reference signature matched (vendor reference, not an official unique Chip ID)\n");

power_off:
	{
		int off_ret = zzh_imx415_power_off(imx415);

		if (!ret && off_ret)
			ret = off_ret;
	}
	return ret;
}

/**
 * zzh_imx415_remove() - 模块卸载或设备解绑时的清理入口
 * @client: 正在解除绑定的 I2C client。
 *
 * @imx415 从 clientdata 取回 probe 分配的状态，不新申请内存。正常 probe 已经
 * power_off，因此这里主要验证幂等清理；如果未来流程改变，它仍能关闭资源。
 *
 * Return: power_off 的 0 或第一项清理 errno。
 */
static int zzh_imx415_remove(struct i2c_client *client)
{
	struct zzh_imx415_minimal *imx415 = i2c_get_clientdata(client);

	return zzh_imx415_power_off(imx415);
}

/*
 * OF 匹配表：DTS compatible 精确匹配这里的字符串。
 * 结尾空项是内核遍历表的终止标志；MODULE_DEVICE_TABLE 生成模块 alias，
 * 使 modprobe/udev 能根据 OF modalias 找到 zzh_imx415_minimal.ko。
 */
static const struct of_device_id zzh_imx415_of_match[] = {
	{ .compatible = "zzh,imx415-minimal" },
	{ }
};
MODULE_DEVICE_TABLE(of, zzh_imx415_of_match);

/*
 * 旧式 I2C device-id 匹配表。当前板主要走 OF 表，但保留它可支持按 I2C 名称
 * 实例化，并生成 i2c:zzh_imx415_minimal 模块 alias。
 */
static const struct i2c_device_id zzh_imx415_id[] = {
	{ "zzh_imx415_minimal", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zzh_imx415_id);

/*
 * 注册给 I2C core 的驱动对象：
 *   driver.name     出现在 /sys/bus/i2c/drivers/；
 *   of_match_table  负责 compatible 匹配；
 *   probe/remove    负责绑定和解绑生命周期；
 *   id_table        提供非 OF/I2C modalias 匹配。
 */
static struct i2c_driver zzh_imx415_driver = {
	.driver = {
		.name = "zzh_imx415_minimal",
		.of_match_table = zzh_imx415_of_match,
	},
	.probe = zzh_imx415_probe,
	.remove = zzh_imx415_remove,
	.id_table = zzh_imx415_id,
};

/* 自动生成 module_init/module_exit，加载时注册、卸载时注销 I2C driver。 */
module_i2c_driver(zzh_imx415_driver);

/* modinfo 可见的模块说明、作者和许可证。 */
MODULE_DESCRIPTION("ZZH IMX415 minimal I2C identification driver");
MODULE_AUTHOR("zzh");
MODULE_LICENSE("GPL v2");
