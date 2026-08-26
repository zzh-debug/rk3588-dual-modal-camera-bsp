// SPDX-License-Identifier: GPL-2.0
/*
 * Independent fixed-mode IMX415 experiment for ATK-DLRK3588 J20.
 *
 * The V4L2 state machine and resource management are project code.  Register
 * values required for the single 3864x2192 RAW10 mode are attributed in
 * docs/阶段二寄存器来源台账.md.  Opaque tuning values originate from the
 * board vendor reference and are not presented as public Sony register data.
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
#include <linux/rk-camera-module.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define ZZH_IMX415_NAME			"zzh_imx415"
#define ZZH_IMX415_SENSOR_NAME		"imx415"
#define ZZH_IMX415_I2C_ADDR		0x1a

#define ZZH_IMX415_XVCLK_HZ		37125000UL
#define ZZH_IMX415_XVCLK_MIN_HZ		35640000UL
#define ZZH_IMX415_XVCLK_MAX_HZ		37867500UL

#define ZZH_IMX415_WIDTH			3864U
#define ZZH_IMX415_HEIGHT		2192U
#define ZZH_IMX415_CROP_LEFT		12
#define ZZH_IMX415_CROP_TOP		16
#define ZZH_IMX415_CROP_WIDTH		3840U
#define ZZH_IMX415_CROP_HEIGHT		2160U
#define ZZH_IMX415_LANES			4U
#define ZZH_IMX415_BITS_PER_SAMPLE	10U

/* Sony specifies 891 Mbit/s/lane. CSI-2 LINK_FREQ is the DDR clock. */
#define ZZH_IMX415_LINK_FREQ_HZ		445500000LL
#define ZZH_IMX415_PIXEL_RATE		356400000LL
#define ZZH_IMX415_VTS_DEFAULT		2250U
#define ZZH_IMX415_VTS_MAX		0x7fffU
#define ZZH_IMX415_HBLANK		1416U
#define ZZH_IMX415_VBLANK_DEFAULT	(ZZH_IMX415_VTS_DEFAULT - \
					 ZZH_IMX415_HEIGHT)

#define ZZH_IMX415_EXPOSURE_MIN		4U
#define ZZH_IMX415_EXPOSURE_DEFAULT	(ZZH_IMX415_VTS_DEFAULT - 8U)
#define ZZH_IMX415_GAIN_MIN		0U
#define ZZH_IMX415_GAIN_MAX		240U

#define IMX415_REG_STANDBY		0x3000
#define IMX415_STANDBY			0x01
#define IMX415_OPERATING			0x00
#define IMX415_REG_REGHOLD		0x3001
#define IMX415_REGHOLD_ENABLE		0x01
#define IMX415_REGHOLD_DISABLE		0x00
#define IMX415_REG_VMAX			0x3024
#define IMX415_REG_FLIP			0x3030
#define IMX415_MIRROR_BIT		BIT(0)
#define IMX415_FLIP_BIT			BIT(1)
#define IMX415_REG_SHR0			0x3050
#define IMX415_REG_GAIN			0x3090
#define IMX415_REFERENCE_REG		0x311a
#define IMX415_REFERENCE_VALUE		0xe0

#define ZZH_IMX415_REG_END		0xffff

struct zzh_imx415_reg {
	u16 address;
	u8 value;
};

/*
 * [D/V] Public clock/format registers plus [V] board-vendor tuning values.
 * Only the vendor's 10-bit, full-resolution global array is retained.
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

static const s64 zzh_imx415_link_freq_menu[] = {
	ZZH_IMX415_LINK_FREQ_HZ,
};

struct zzh_imx415 {
	struct i2c_client *client;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	struct regulator *dvdd;
	struct regulator *dovdd;
	struct regulator *avdd;

	u32 module_index;
	const char *module_facing;
	const char *module_name;
	const char *lens_name;
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

static inline struct zzh_imx415 *to_zzh_imx415(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct zzh_imx415, subdev);
}

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

static void zzh_imx415_assert_controls(struct zzh_imx415 *imx415)
{
	gpiod_set_value_cansleep(imx415->reset_gpio, 1);
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 1);
}

static bool zzh_imx415_has_enabled_resource(struct zzh_imx415 *imx415)
{
	return imx415->clock_enabled || imx415->dvdd_enabled ||
	       imx415->dovdd_enabled || imx415->avdd_enabled;
}

static int zzh_imx415_power_off(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	int first_error = 0;
	int ret;

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

	imx415->powered = zzh_imx415_has_enabled_resource(imx415);
	if (first_error)
		dev_err(dev, "power-off cleanup incomplete: %d\n", first_error);

	return first_error;
}

static int zzh_imx415_power_on(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	int cleanup_ret;
	int ret;

	if (imx415->powered)
		return 0;

	if (zzh_imx415_has_enabled_resource(imx415)) {
		ret = zzh_imx415_power_off(imx415);
		if (ret)
			return ret;
	}

	ret = zzh_imx415_set_clock_rate(imx415);
	if (ret)
		return ret;

	zzh_imx415_assert_controls(imx415);

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
	cleanup_ret = zzh_imx415_power_off(imx415);
	if (cleanup_ret)
		dev_err(dev, "power-on rollback also failed: %d\n", cleanup_ret);
	return ret;
}

static int zzh_imx415_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	return zzh_imx415_power_on(imx415);
}

static int zzh_imx415_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);

	return zzh_imx415_power_off(imx415);
}

static int zzh_imx415_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct zzh_imx415 *imx415 =
		container_of(ctrl->handler, struct zzh_imx415, ctrl_handler);
	struct device *dev = &imx415->client->dev;
	u32 old_vts = imx415->current_vts;
	u32 new_vts = old_vts;
	u32 exposure_max;
	u32 exp;
	u8 flip;
	int pm_ref;
	int ret = 0;

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
		exp = imx415->exposure->val;
		ret = zzh_imx415_write_timing_held(imx415, new_vts, exp);
		if (ret)
			imx415->current_vts = old_vts;
		break;
	case V4L2_CID_EXPOSURE:
		ret = zzh_imx415_write_held(imx415, IMX415_REG_SHR0, 3,
					    imx415->current_vts - ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = zzh_imx415_write_held(imx415, IMX415_REG_GAIN, 2,
					    ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		ret = zzh_imx415_read_reg8(imx415, IMX415_REG_FLIP, &flip);
		if (ret)
			break;
		if (ctrl->val)
			flip |= IMX415_MIRROR_BIT;
		else
			flip &= ~IMX415_MIRROR_BIT;
		ret = zzh_imx415_write_reg8(imx415, IMX415_REG_FLIP, flip);
		break;
	case V4L2_CID_VFLIP:
		ret = zzh_imx415_read_reg8(imx415, IMX415_REG_FLIP, &flip);
		if (ret)
			break;
		if (ctrl->val)
			flip |= IMX415_FLIP_BIT;
		else
			flip &= ~IMX415_FLIP_BIT;
		ret = zzh_imx415_write_reg8(imx415, IMX415_REG_FLIP, flip);
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

static int zzh_imx415_init_controls(struct zzh_imx415 *imx415)
{
	struct v4l2_ctrl_handler *handler = &imx415->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &imx415->mutex;

	imx415->link_freq =
		v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(zzh_imx415_link_freq_menu) - 1,
				       0, zzh_imx415_link_freq_menu);
	imx415->pixel_rate =
		v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
				  ZZH_IMX415_PIXEL_RATE,
				  ZZH_IMX415_PIXEL_RATE, 1,
				  ZZH_IMX415_PIXEL_RATE);
	imx415->hblank =
		v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				  ZZH_IMX415_HBLANK, ZZH_IMX415_HBLANK,
				  1, ZZH_IMX415_HBLANK);
	imx415->vblank =
		v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
				  V4L2_CID_VBLANK,
				  ZZH_IMX415_VBLANK_DEFAULT,
				  ZZH_IMX415_VTS_MAX - ZZH_IMX415_HEIGHT,
				  1, ZZH_IMX415_VBLANK_DEFAULT);
	imx415->exposure =
		v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
				  V4L2_CID_EXPOSURE,
				  ZZH_IMX415_EXPOSURE_MIN,
				  ZZH_IMX415_VTS_DEFAULT - 8,
				  1, ZZH_IMX415_EXPOSURE_DEFAULT);
	imx415->analogue_gain =
		v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
				  V4L2_CID_ANALOGUE_GAIN,
				  ZZH_IMX415_GAIN_MIN, ZZH_IMX415_GAIN_MAX,
				  1, ZZH_IMX415_GAIN_MIN);
	v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(handler, &zzh_imx415_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);

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

static int zzh_imx415_enum_mbus_code(struct v4l2_subdev *subdev,
				     struct v4l2_subdev_pad_config *config,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGBRG10_1X10;
	return 0;
}

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

static int zzh_imx415_start_stream(struct zzh_imx415 *imx415)
{
	int standby_ret;
	int ret;

	ret = zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
				    IMX415_STANDBY);
	if (ret)
		return ret;

	ret = zzh_imx415_write_array(imx415, zzh_imx415_global_regs);
	if (ret)
		goto restore_standby;

	ret = zzh_imx415_write_array(imx415, zzh_imx415_mode_regs);
	if (ret)
		goto restore_standby;

	ret = __v4l2_ctrl_handler_setup(&imx415->ctrl_handler);
	if (ret)
		goto restore_standby;

	ret = zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
				    IMX415_OPERATING);
	if (!ret)
		return 0;

restore_standby:
	standby_ret = zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
					    IMX415_STANDBY);
	if (standby_ret)
		dev_err(&imx415->client->dev,
			"failed to restore standby after start error: %d\n",
			standby_ret);
	return ret;
}

static int zzh_imx415_stop_stream(struct zzh_imx415 *imx415)
{
	return zzh_imx415_write_reg8(imx415, IMX415_REG_STANDBY,
				     IMX415_STANDBY);
}

static int zzh_imx415_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);
	struct device *dev = &imx415->client->dev;
	int pm_ret;
	int ret = 0;

	enable = !!enable;
	mutex_lock(&imx415->mutex);
	if (imx415->streaming == enable)
		goto unlock;

	if (enable) {
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

static void zzh_imx415_get_module_info(struct zzh_imx415 *imx415,
				       struct rkmodule_inf *info)
{
	memset(info, 0, sizeof(*info));
	strscpy(info->base.sensor, ZZH_IMX415_SENSOR_NAME,
		sizeof(info->base.sensor));
	strscpy(info->base.module, imx415->module_name,
		sizeof(info->base.module));
	strscpy(info->base.lens, imx415->lens_name,
		sizeof(info->base.lens));
}

static long zzh_imx415_ioctl(struct v4l2_subdev *subdev,
			     unsigned int command, void *argument)
{
	struct zzh_imx415 *imx415 = to_zzh_imx415(subdev);
	struct rkmodule_channel_info *channel_info;
	struct rkmodule_hdr_cfg *hdr_config;

	/*
	 * Rockchip AIQ releases carry private copies of rk-camera-module.h.
	 * Some of them use a different rkmodule_hdr_cfg tail layout, and the
	 * encoded ioctl size therefore differs from the kernel UAPI value.  The
	 * command type/number and the leading hdr_mode field remain stable.
	 */
	if (_IOC_TYPE(command) == _IOC_TYPE(RKMODULE_SET_HDR_CFG) &&
	    _IOC_NR(command) == _IOC_NR(RKMODULE_SET_HDR_CFG)) {
		hdr_config = argument;
		if (hdr_config->hdr_mode != NO_HDR)
			return -EINVAL;
		return 0;
	}

	switch (command) {
	case RKMODULE_GET_MODULE_INFO:
		zzh_imx415_get_module_info(imx415, argument);
		return 0;
	case RKMODULE_GET_HDR_CFG:
		hdr_config = argument;
		memset(hdr_config, 0, sizeof(*hdr_config));
		hdr_config->esp.mode = HDR_NORMAL_VC;
		hdr_config->hdr_mode = NO_HDR;
		return 0;
	case RKMODULE_GET_CHANNEL_INFO:
		channel_info = argument;
		if (channel_info->index != 0)
			return -EINVAL;
		channel_info->vc = 0;
		channel_info->width = ZZH_IMX415_WIDTH;
		channel_info->height = ZZH_IMX415_HEIGHT;
		channel_info->bus_fmt = MEDIA_BUS_FMT_SGBRG10_1X10;
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct v4l2_subdev_core_ops zzh_imx415_core_ops = {
	.s_power = zzh_imx415_s_power,
	.ioctl = zzh_imx415_ioctl,
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

static const struct media_entity_operations zzh_imx415_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int zzh_imx415_parse_module_info(struct zzh_imx415 *imx415)
{
	struct device *dev = &imx415->client->dev;
	struct device_node *node = dev->of_node;
	int ret;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx415->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx415->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx415->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx415->lens_name);
	if (ret) {
		dev_err(dev, "missing Rockchip camera module metadata\n");
		return -EINVAL;
	}

	if (strcmp(imx415->module_facing, "back") &&
	    strcmp(imx415->module_facing, "front")) {
		dev_err(dev, "camera-module-facing must be back or front\n");
		return -EINVAL;
	}

	return 0;
}

static int zzh_imx415_check_endpoint(struct device *dev)
{
	struct v4l2_fwnode_endpoint endpoint = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *handle;
	int ret;

	handle = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!handle) {
		dev_err(dev, "CSI-2 endpoint is missing\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(handle, &endpoint);
	fwnode_handle_put(handle);
	if (ret) {
		dev_err(dev, "failed to parse CSI-2 endpoint: %d\n", ret);
		goto free_endpoint;
	}

	if (endpoint.bus.mipi_csi2.num_data_lanes != ZZH_IMX415_LANES) {
		dev_err(dev, "expected 4 CSI-2 lanes, got %u\n",
			endpoint.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto free_endpoint;
	}

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

static int zzh_imx415_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct zzh_imx415 *imx415;
	struct v4l2_subdev *subdev;
	char facing[2] = { 0 };
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->addr != ZZH_IMX415_I2C_ADDR) {
		dev_err(dev, "expected 7-bit I2C address 0x1a, got 0x%02x\n",
			client->addr);
		return -EINVAL;
	}

	imx415 = devm_kzalloc(dev, sizeof(*imx415), GFP_KERNEL);
	if (!imx415)
		return -ENOMEM;

	imx415->client = client;
	mutex_init(&imx415->mutex);
	ret = zzh_imx415_parse_module_info(imx415);
	if (ret)
		goto destroy_mutex;

	subdev = &imx415->subdev;
	v4l2_i2c_subdev_init(subdev, client, &zzh_imx415_subdev_ops);

	ret = zzh_imx415_check_endpoint(dev);
	if (ret)
		goto destroy_mutex;

	imx415->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx415->xvclk)) {
		ret = PTR_ERR(imx415->xvclk);
		goto destroy_mutex;
	}

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
	imx415->pad.flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	subdev->entity.ops = &zzh_imx415_entity_ops;
	ret = media_entity_pads_init(&subdev->entity, 1, &imx415->pad);
	if (ret)
		goto power_off;
#endif

	if (!strcmp(imx415->module_facing, "back"))
		facing[0] = 'b';
	else
		facing[0] = 'f';
	snprintf(subdev->name, sizeof(subdev->name), "m%02u_%s_%s %s",
		 imx415->module_index, facing, ZZH_IMX415_NAME,
		 dev_name(subdev->dev));

	ret = v4l2_async_register_subdev_sensor_common(subdev);
	if (ret)
		goto cleanup_entity;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	dev_info(dev,
		 "registered independent 3864x2192 RAW10 30 fps Sensor subdev as %s (%s/%s)\n",
		 subdev->name, imx415->module_name, imx415->lens_name);
	return 0;

cleanup_entity:
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

static const struct dev_pm_ops zzh_imx415_pm_ops = {
	SET_RUNTIME_PM_OPS(zzh_imx415_runtime_suspend,
			   zzh_imx415_runtime_resume, NULL)
};

static const struct of_device_id zzh_imx415_of_match[] = {
	{ .compatible = "zzh,imx415" },
	{ }
};
MODULE_DEVICE_TABLE(of, zzh_imx415_of_match);

static const struct i2c_device_id zzh_imx415_id[] = {
	{ ZZH_IMX415_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zzh_imx415_id);

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

module_i2c_driver(zzh_imx415_i2c_driver);

MODULE_DESCRIPTION("ZZH independent IMX415 fixed-mode V4L2 sensor driver");
MODULE_AUTHOR("zzh");
MODULE_LICENSE("GPL v2");
