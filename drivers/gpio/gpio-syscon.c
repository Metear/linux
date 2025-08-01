// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  SYSCON GPIO driver
 *
 *  Copyright (C) 2014 Alexander Shiyan <shc_work@mail.ru>
 */

#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#define GPIO_SYSCON_FEAT_IN	BIT(0)
#define GPIO_SYSCON_FEAT_OUT	BIT(1)
#define GPIO_SYSCON_FEAT_DIR	BIT(2)

/* SYSCON driver is designed to use 32-bit wide registers */
#define SYSCON_REG_SIZE		(4)
#define SYSCON_REG_BITS		(SYSCON_REG_SIZE * 8)

/**
 * struct syscon_gpio_data - Configuration for the device.
 * @flags:		Set of GPIO_SYSCON_FEAT_ flags:
 *			GPIO_SYSCON_FEAT_IN:	GPIOs supports input,
 *			GPIO_SYSCON_FEAT_OUT:	GPIOs supports output,
 *			GPIO_SYSCON_FEAT_DIR:	GPIOs supports switch direction.
 * @bit_count:		Number of bits used as GPIOs.
 * @dat_bit_offset:	Offset (in bits) to the first GPIO bit.
 * @dir_bit_offset:	Optional offset (in bits) to the first bit to switch
 *			GPIO direction (Used with GPIO_SYSCON_FEAT_DIR flag).
 * @set:		HW specific callback to assigns output value
 *			for signal "offset"
 */

struct syscon_gpio_data {
	unsigned int	flags;
	unsigned int	bit_count;
	unsigned int	dat_bit_offset;
	unsigned int	dir_bit_offset;
	int		(*set)(struct gpio_chip *chip, unsigned int offset,
			       int value);
};

struct syscon_gpio_priv {
	struct gpio_chip		chip;
	struct regmap			*syscon;
	const struct syscon_gpio_data	*data;
	u32				dreg_offset;
	u32				dir_reg_offset;
};

static int syscon_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct syscon_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned int val, offs;
	int ret;

	offs = priv->dreg_offset + priv->data->dat_bit_offset + offset;

	ret = regmap_read(priv->syscon,
			  (offs / SYSCON_REG_BITS) * SYSCON_REG_SIZE, &val);
	if (ret)
		return ret;

	return !!(val & BIT(offs % SYSCON_REG_BITS));
}

static int syscon_gpio_set(struct gpio_chip *chip, unsigned int offset, int val)
{
	struct syscon_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned int offs;

	offs = priv->dreg_offset + priv->data->dat_bit_offset + offset;

	return regmap_update_bits(priv->syscon,
				  (offs / SYSCON_REG_BITS) * SYSCON_REG_SIZE,
				  BIT(offs % SYSCON_REG_BITS),
				  val ? BIT(offs % SYSCON_REG_BITS) : 0);
}

static int syscon_gpio_dir_in(struct gpio_chip *chip, unsigned offset)
{
	struct syscon_gpio_priv *priv = gpiochip_get_data(chip);

	if (priv->data->flags & GPIO_SYSCON_FEAT_DIR) {
		unsigned int offs;

		offs = priv->dir_reg_offset +
		       priv->data->dir_bit_offset + offset;

		regmap_update_bits(priv->syscon,
				   (offs / SYSCON_REG_BITS) * SYSCON_REG_SIZE,
				   BIT(offs % SYSCON_REG_BITS), 0);
	}

	return 0;
}

static int syscon_gpio_dir_out(struct gpio_chip *chip, unsigned offset, int val)
{
	struct syscon_gpio_priv *priv = gpiochip_get_data(chip);

	if (priv->data->flags & GPIO_SYSCON_FEAT_DIR) {
		unsigned int offs;

		offs = priv->dir_reg_offset +
		       priv->data->dir_bit_offset + offset;

		regmap_update_bits(priv->syscon,
				   (offs / SYSCON_REG_BITS) * SYSCON_REG_SIZE,
				   BIT(offs % SYSCON_REG_BITS),
				   BIT(offs % SYSCON_REG_BITS));
	}

	return chip->set_rv(chip, offset, val);
}

static const struct syscon_gpio_data clps711x_mctrl_gpio = {
	/* ARM CLPS711X SYSFLG1 Bits 8-10 */
	.flags		= GPIO_SYSCON_FEAT_IN,
	.bit_count	= 3,
	.dat_bit_offset	= 0x40 * 8 + 8,
};

static int rockchip_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int val)
{
	struct syscon_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned int offs;
	u8 bit;
	u32 data;
	int ret;

	offs = priv->dreg_offset + priv->data->dat_bit_offset + offset;
	bit = offs % SYSCON_REG_BITS;
	data = (val ? BIT(bit) : 0) | BIT(bit + 16);
	ret = regmap_write(priv->syscon,
			   (offs / SYSCON_REG_BITS) * SYSCON_REG_SIZE,
			   data);
	if (ret < 0)
		dev_err(chip->parent, "gpio write failed ret(%d)\n", ret);

	return ret;
}

static const struct syscon_gpio_data rockchip_rk3328_gpio_mute = {
	/* RK3328 GPIO_MUTE is an output only pin at GRF_SOC_CON10[1] */
	.flags		= GPIO_SYSCON_FEAT_OUT,
	.bit_count	= 1,
	.dat_bit_offset = 0x0428 * 8 + 1,
	.set		= rockchip_gpio_set,
};

#define KEYSTONE_LOCK_BIT BIT(0)

static int keystone_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int val)
{
	struct syscon_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned int offs;
	int ret;

	offs = priv->dreg_offset + priv->data->dat_bit_offset + offset;

	if (!val)
		return 0;

	ret = regmap_update_bits(
			priv->syscon,
			(offs / SYSCON_REG_BITS) * SYSCON_REG_SIZE,
			BIT(offs % SYSCON_REG_BITS) | KEYSTONE_LOCK_BIT,
			BIT(offs % SYSCON_REG_BITS) | KEYSTONE_LOCK_BIT);
	if (ret < 0)
		dev_err(chip->parent, "gpio write failed ret(%d)\n", ret);

	return ret;
}

static const struct syscon_gpio_data keystone_dsp_gpio = {
	/* ARM Keystone 2 */
	.flags		= GPIO_SYSCON_FEAT_OUT,
	.bit_count	= 28,
	.dat_bit_offset	= 4,
	.set		= keystone_gpio_set,
};

static const struct of_device_id syscon_gpio_ids[] = {
	{
		.compatible	= "cirrus,ep7209-mctrl-gpio",
		.data		= &clps711x_mctrl_gpio,
	},
	{
		.compatible	= "ti,keystone-dsp-gpio",
		.data		= &keystone_dsp_gpio,
	},
	{
		.compatible	= "rockchip,rk3328-grf-gpio",
		.data		= &rockchip_rk3328_gpio_mute,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, syscon_gpio_ids);

static int syscon_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct syscon_gpio_priv *priv;
	struct device_node *np = dev->of_node;
	int ret;
	bool use_parent_regmap = false;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);

	priv->syscon = syscon_regmap_lookup_by_phandle(np, "gpio,syscon-dev");
	if (IS_ERR(priv->syscon) && np->parent) {
		priv->syscon = syscon_node_to_regmap(np->parent);
		use_parent_regmap = true;
	}
	if (IS_ERR(priv->syscon))
		return PTR_ERR(priv->syscon);

	if (!use_parent_regmap) {
		ret = of_property_read_u32_index(np, "gpio,syscon-dev", 1,
						 &priv->dreg_offset);
		if (ret)
			dev_err(dev, "can't read the data register offset!\n");

		priv->dreg_offset <<= 3;

		ret = of_property_read_u32_index(np, "gpio,syscon-dev", 2,
						 &priv->dir_reg_offset);
		if (ret)
			dev_dbg(dev, "can't read the dir register offset!\n");

		priv->dir_reg_offset <<= 3;
	}

	priv->chip.parent = dev;
	priv->chip.owner = THIS_MODULE;
	priv->chip.label = dev_name(dev);
	priv->chip.base = -1;
	priv->chip.ngpio = priv->data->bit_count;
	priv->chip.get = syscon_gpio_get;
	if (priv->data->flags & GPIO_SYSCON_FEAT_IN)
		priv->chip.direction_input = syscon_gpio_dir_in;
	if (priv->data->flags & GPIO_SYSCON_FEAT_OUT) {
		priv->chip.set_rv = priv->data->set ? : syscon_gpio_set;
		priv->chip.direction_output = syscon_gpio_dir_out;
	}

	return devm_gpiochip_add_data(&pdev->dev, &priv->chip, priv);
}

static struct platform_driver syscon_gpio_driver = {
	.driver	= {
		.name		= "gpio-syscon",
		.of_match_table	= syscon_gpio_ids,
	},
	.probe	= syscon_gpio_probe,
};
module_platform_driver(syscon_gpio_driver);

MODULE_AUTHOR("Alexander Shiyan <shc_work@mail.ru>");
MODULE_DESCRIPTION("SYSCON GPIO driver");
MODULE_LICENSE("GPL");
