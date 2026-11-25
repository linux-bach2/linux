// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6555v100 (Hi6250 PMIC) LDOs, minimal: only the LDOs listed here, each locked to
 * one voltage (the stock consumer's), as on the Huawei MediaPad M5 lite 10 (bach2).
 *
 * Register layout from the stock vendor DT (pmic@FFF34000, "hisilicon,hisi-ldo"):
 *   hisilicon,hisi-ctrl = <reg enable_mask eco_mask>, hisilicon,hisi-vset = <reg mask>.
 * PMIC register r is the 32-bit word at byte offset r << 2 of the PMU window (pmu_ctrl syscon).
 *
 * Safety invariant (hi6555_ldo_sel_ok): a selector other than the LDO's stock one is never
 * written, and the LDO is never enabled unless its vset field reads back as the stock selector.
 * This holds whatever the DT constraints say.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define PMIC_REG(r)	((r) << 2)

struct hi6555_ldo {
	struct regulator_desc desc;
	unsigned int stock_sel;
};

/* The one place the invariant is decided. */
static bool hi6555_ldo_sel_ok(struct regulator_dev *rdev, unsigned int sel)
{
	const struct hi6555_ldo *ldo = container_of_const(rdev->desc, struct hi6555_ldo, desc);

	return sel == ldo->stock_sel;
}

static int hi6555_ldo_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	if (!hi6555_ldo_sel_ok(rdev, sel)) {
		dev_err(&rdev->dev, "%s: refusing selector %u (not the stock one)\n",
			rdev->desc->name, sel);
		return -EPERM;
	}
	return regulator_set_voltage_sel_regmap(rdev, sel);
}

static void hi6555_ldo_log(struct regulator_dev *rdev, const char *what)
{
	unsigned int ctrl = ~0u, vset = ~0u;

	regmap_read(rdev->regmap, rdev->desc->enable_reg, &ctrl);
	regmap_read(rdev->regmap, rdev->desc->vsel_reg, &vset);
	dev_info(&rdev->dev, "%s %s: ctrl[0x%03x]=0x%02x vset[0x%03x]=0x%02x\n",
		 rdev->desc->name, what, rdev->desc->enable_reg >> 2, ctrl,
		 rdev->desc->vsel_reg >> 2, vset);
}

static int hi6555_ldo_enable(struct regulator_dev *rdev)
{
	int sel, ret;

	sel = regulator_get_voltage_sel_regmap(rdev);
	if (sel < 0)
		return sel;
	if (!hi6555_ldo_sel_ok(rdev, sel)) {
		dev_err(&rdev->dev, "%s: vset selector %d is not the stock one, not enabling\n",
			rdev->desc->name, sel);
		return -EPERM;
	}
	ret = regulator_enable_regmap(rdev);
	hi6555_ldo_log(rdev, ret ? "enable FAILED" : "enabled");
	return ret;
}

static int hi6555_ldo_disable(struct regulator_dev *rdev)
{
	int ret = regulator_disable_regmap(rdev);

	hi6555_ldo_log(rdev, ret ? "disable FAILED" : "disabled");
	return ret;
}

static const struct regulator_ops hi6555_ldo_ops = {
	.list_voltage		= regulator_list_voltage_table,
	.map_voltage		= regulator_map_voltage_ascend,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= hi6555_ldo_set_voltage_sel,
	.is_enabled		= regulator_is_enabled_regmap,
	.enable			= hi6555_ldo_enable,
	.disable		= hi6555_ldo_disable,
};

/* hisilicon,hisi-vset-table of the vendor DT, in uV */
static const unsigned int ldo4_uv[] = {
	1700000, 1750000, 1775000, 1800000, 1825000, 1850000, 1875000, 1900000,
};
static const unsigned int ldo17_uv[] = {
	2600000, 2700000, 2800000, 2850000, 3000000, 3100000, 3200000, 3300000,
};

/*
 * _ctrl/_vset: PMIC register numbers (vendor DT), enable mask 0x01, vset mask 0x07.
 * _time/_off: hisilicon,hisi-enable-time-us / hisilicon,hisi-off-on-delay-us.
 */
#define HI6555_LDO(_name, _id, _tab, _ctrl, _vset, _stock, _time, _off) {	\
	.desc = {								\
		.name		= #_name,					\
		.of_match	= of_match_ptr(#_name),				\
		.regulators_node = of_match_ptr("regulators"),			\
		.id		= _id,						\
		.ops		= &hi6555_ldo_ops,				\
		.type		= REGULATOR_VOLTAGE,				\
		.owner		= THIS_MODULE,					\
		.n_voltages	= ARRAY_SIZE(_tab),				\
		.volt_table	= _tab,						\
		.vsel_reg	= PMIC_REG(_vset),				\
		.vsel_mask	= 0x07,						\
		.enable_reg	= PMIC_REG(_ctrl),				\
		.enable_mask	= 0x01,						\
		.enable_time	= _time,					\
		.off_on_delay	= _off,						\
	},									\
	.stock_sel = _stock,							\
}

static const struct hi6555_ldo hi6555_ldos[] = {
	/* ldo@19: touch VDDIO, stock 1.8 V (elan vddio_ldo_value) = selector 3 */
	HI6555_LDO(ldo4, 4, ldo4_uv, 0x19, 0x42, 3, 120, 20000),
	/* ldo@25: touch VCI, stock 3.3 V (elan vci_ldo_value) = selector 7 */
	HI6555_LDO(ldo17, 17, ldo17_uv, 0x25, 0x4e, 7, 120, 20000),
};

static int hi6555_ldo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	unsigned int i;

	config.regmap = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,pmic-syscon");
	if (IS_ERR(config.regmap))
		return dev_err_probe(dev, PTR_ERR(config.regmap), "no hisilicon,pmic-syscon\n");

	for (i = 0; i < ARRAY_SIZE(hi6555_ldos); i++) {
		const struct regulator_desc *d = &hi6555_ldos[i].desc;

		if (hi6555_ldos[i].stock_sel >= d->n_voltages)
			return -EINVAL;
		rdev = devm_regulator_register(dev, d, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "%s: register failed\n", d->name);
		hi6555_ldo_log(rdev, "registered");
	}
	return 0;
}

static const struct of_device_id hi6555_ldo_of_match[] = {
	{ .compatible = "hisilicon,hi6555v100-ldo" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6555_ldo_of_match);

static struct platform_driver hi6555_ldo_driver = {
	.driver = {
		.name = "hi6555v100-ldo",
		.of_match_table = hi6555_ldo_of_match,
	},
	.probe = hi6555_ldo_probe,
};
module_platform_driver(hi6555_ldo_driver);

MODULE_DESCRIPTION("HiSilicon Hi6555v100 LDOs locked to stock voltages");
MODULE_LICENSE("GPL");
