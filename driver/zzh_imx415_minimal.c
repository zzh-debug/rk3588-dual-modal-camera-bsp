// SPDX-License-Identifier: GPL-2.0
/*
 * Stage-one IMX415 hardware-contract experiment.
 *
 * This is intentionally a small I2C-only driver.  It does not implement a
 * V4L2 sub-device, mode registers, controls, streaming, or a media graph.
 * The 0x311a/0xe0 check is a vendor-reference signature: Sony's public
 * register map identifies 0x311a as INCKSEL4, not as a unique chip ID.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#define ZZH_IMX415_I2C_ADDR			0x1a
#define ZZH_IMX415_XVCLK_HZ			37125000UL
#define ZZH_IMX415_XVCLK_MIN_HZ			35640000UL
#define ZZH_IMX415_XVCLK_MAX_HZ			37867500UL

#define IMX415_REFERENCE_SIGNATURE_REG		0x311a
#define IMX415_REFERENCE_SIGNATURE_VALUE	0xe0

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

static void zzh_imx415_assert_controls(struct zzh_imx415_minimal *imx415)
{
	/* Both GPIOs are active-low in the experimental DTS. */
	gpiod_set_value_cansleep(imx415->reset_gpio, 1);
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 1);
}

static int zzh_imx415_power_on(struct zzh_imx415_minimal *imx415)
{
	struct device *dev = &imx415->client->dev;
	bool dvdd_on = false;
	bool dovdd_on = false;
	bool avdd_on = false;
	bool clock_on = false;
	int ret;

	zzh_imx415_assert_controls(imx415);

	if (imx415->supplies_present) {
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

	/* Also covers the module-local LDO case: XCLR stays low >= 500 ns. */
	usleep_range(1000, 2000);

	/* Release module PWDN, then the sensor system-clear/reset input. */
	gpiod_set_value_cansleep(imx415->pwdn_gpio, 0);
	gpiod_set_value_cansleep(imx415->reset_gpio, 0);

	/* Sony requires at least 1 us from XCLR high to the first INCK edge. */
	usleep_range(2, 5);
	ret = clk_prepare_enable(imx415->xvclk);
	if (ret) {
		dev_err(dev, "failed to enable xvclk: %d\n", ret);
		goto rollback;
	}
	clock_on = true;

	/* Sony requires at least 20 us from XCLR high to I2C communication. */
	usleep_range(20, 30);
	imx415->powered = true;
	return 0;

rollback:
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

static int zzh_imx415_power_off(struct zzh_imx415_minimal *imx415)
{
	struct device *dev = &imx415->client->dev;
	int first_error = 0;
	int ret;

	if (!imx415->powered) {
		zzh_imx415_assert_controls(imx415);
		return 0;
	}

	/* XCLR, PWDN and INCK are inactive before any OVDD/DOVDD fall. */
	zzh_imx415_assert_controls(imx415);
	clk_disable_unprepare(imx415->xvclk);

	if (imx415->supplies_present) {
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
		dev_err(dev, "power-off regulator cleanup failed: %d\n", first_error);
	return first_error;
}

static int zzh_imx415_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct zzh_imx415_minimal *imx415;
	u8 signature = 0;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "adapter lacks raw I2C transfer support\n");
		return -EOPNOTSUPP;
	}
	if (client->addr != ZZH_IMX415_I2C_ADDR) {
		dev_err(dev, "unexpected 7-bit address 0x%02x (expected 0x%02x)\n",
			client->addr, ZZH_IMX415_I2C_ADDR);
		return -EINVAL;
	}

	imx415 = devm_kzalloc(dev, sizeof(*imx415), GFP_KERNEL);
	if (!imx415)
		return -ENOMEM;
	imx415->client = client;
	i2c_set_clientdata(client, imx415);

	dev_info(dev,
		"probing adapter %s, 7-bit addr 0x%02x (wire write/read 0x34/0x35)\n",
		client->adapter->name, client->addr);

	imx415->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx415->xvclk)) {
		ret = PTR_ERR(imx415->xvclk);
		dev_err(dev, "failed to get xvclk: %d\n", ret);
		return ret;
	}

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

	ret = zzh_imx415_read_reg(client, IMX415_REFERENCE_SIGNATURE_REG,
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

static int zzh_imx415_remove(struct i2c_client *client)
{
	struct zzh_imx415_minimal *imx415 = i2c_get_clientdata(client);

	return zzh_imx415_power_off(imx415);
}

static const struct of_device_id zzh_imx415_of_match[] = {
	{ .compatible = "zzh,imx415-minimal" },
	{ }
};
MODULE_DEVICE_TABLE(of, zzh_imx415_of_match);

static const struct i2c_device_id zzh_imx415_id[] = {
	{ "zzh_imx415_minimal", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zzh_imx415_id);

static struct i2c_driver zzh_imx415_driver = {
	.driver = {
		.name = "zzh_imx415_minimal",
		.of_match_table = zzh_imx415_of_match,
	},
	.probe = zzh_imx415_probe,
	.remove = zzh_imx415_remove,
	.id_table = zzh_imx415_id,
};

module_i2c_driver(zzh_imx415_driver);

MODULE_DESCRIPTION("ZZH IMX415 minimal I2C identification driver");
MODULE_AUTHOR("zzh");
MODULE_LICENSE("GPL v2");
