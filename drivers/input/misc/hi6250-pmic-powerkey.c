// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power key of the PMIC behind the Hi6250 PMU control window (0xfff34000, one 8-bit PMIC register
 * per 32-bit word). Register layout from the vendor 4.9 kernel (drivers/mfd/hisi_pmic.c) and the
 * bach2 device tree:
 *
 *   IRQ status 0x1b0, 0x1b1  write 1 to clear
 *   IRQ mask   0x130, 0x131  1 = masked
 *   parent IRQ               one SoC GPIO, level low, shared by all 16 PMIC sources
 *   power key                status 0x1b0 bit 7 = pressed, bit 6 = released
 *                            (bit 5 = held 1 s, bit 4 = held 6 s; not used here)
 *
 * Only bits 7 and 6 are unmasked; every other PMIC interrupt source stays masked.
 */

#include <linux/bits.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define PMIC_REG(r)		((r) << 2)
#define PMIC_IRQ_MASK0		PMIC_REG(0x130)
#define PMIC_IRQ_MASK1		PMIC_REG(0x131)
#define PMIC_IRQ_STAT0		PMIC_REG(0x1b0)
#define PMIC_IRQ_STAT1		PMIC_REG(0x1b1)
#define PWRKEY_DOWN		BIT(7)
#define PWRKEY_UP		BIT(6)
#define PWRKEY_BITS		(PWRKEY_DOWN | PWRKEY_UP)

struct hi6250_pwrkey {
	struct device *dev;
	struct regmap *map;
	struct input_dev *input;
};

static irqreturn_t hi6250_pwrkey_irq(int irq, void *data)
{
	struct hi6250_pwrkey *pk = data;
	unsigned int st0, st1;

	if (regmap_read(pk->map, PMIC_IRQ_STAT0, &st0) ||
	    regmap_read(pk->map, PMIC_IRQ_STAT1, &st1))
		return IRQ_NONE;
	st0 &= 0xff;
	st1 &= 0xff;
	/* Clear everything that is pending, as the vendor handler does; other sources are masked. */
	if (st0)
		regmap_write(pk->map, PMIC_IRQ_STAT0, st0);
	if (st1)
		regmap_write(pk->map, PMIC_IRQ_STAT1, st1);
	if (!(st0 & PWRKEY_BITS))
		return (st0 || st1) ? IRQ_HANDLED : IRQ_NONE;

	/* A short press can latch both edges before the handler runs: report press, then release. */
	if (device_may_wakeup(pk->dev))
		pm_wakeup_event(pk->dev, 0);
	if (st0 & PWRKEY_DOWN) {
		input_report_key(pk->input, KEY_POWER, 1);
		input_sync(pk->input);
	}
	if (st0 & PWRKEY_UP) {
		input_report_key(pk->input, KEY_POWER, 0);
		input_sync(pk->input);
	}
	return IRQ_HANDLED;
}

static void hi6250_pwrkey_mask(void *data)
{
	struct hi6250_pwrkey *pk = data;

	regmap_update_bits(pk->map, PMIC_IRQ_MASK0, PWRKEY_BITS, PWRKEY_BITS);
}

static int hi6250_pwrkey_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi6250_pwrkey *pk;
	unsigned int m0, m1, s0, s1;
	int irq, ret;

	pk = devm_kzalloc(dev, sizeof(*pk), GFP_KERNEL);
	if (!pk)
		return -ENOMEM;
	pk->dev = dev;

	pk->map = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,pmic-syscon");
	if (IS_ERR(pk->map))
		return dev_err_probe(dev, PTR_ERR(pk->map), "no hisilicon,pmic-syscon\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = regmap_read(pk->map, PMIC_IRQ_MASK0, &m0) ?:
	      regmap_read(pk->map, PMIC_IRQ_MASK1, &m1) ?:
	      regmap_read(pk->map, PMIC_IRQ_STAT0, &s0) ?:
	      regmap_read(pk->map, PMIC_IRQ_STAT1, &s1);
	if (ret)
		return dev_err_probe(dev, ret, "PMIC register read failed\n");
	dev_info(dev, "at probe: mask=%02x,%02x status=%02x,%02x\n",
		 m0 & 0xff, m1 & 0xff, s0 & 0xff, s1 & 0xff);

	/* Same order as the vendor probe: mask all sources, then clear all status bits. */
	ret = regmap_write(pk->map, PMIC_IRQ_MASK0, 0xff) ?:
	      regmap_write(pk->map, PMIC_IRQ_MASK1, 0xff) ?:
	      regmap_write(pk->map, PMIC_IRQ_STAT0, 0xff) ?:
	      regmap_write(pk->map, PMIC_IRQ_STAT1, 0xff);
	if (ret)
		return dev_err_probe(dev, ret, "PMIC register write failed\n");

	pk->input = devm_input_allocate_device(dev);
	if (!pk->input)
		return -ENOMEM;
	pk->input->name = "hi6250 PMIC power key";
	pk->input->phys = "hi6250_pmic/input0";
	input_set_capability(pk->input, EV_KEY, KEY_POWER);
	ret = input_register_device(pk->input);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, irq, NULL, hi6250_pwrkey_irq, IRQF_ONESHOT,
					"hi6250-pmic-powerkey", pk);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request IRQ %d\n", irq);

	ret = devm_add_action_or_reset(dev, hi6250_pwrkey_mask, pk);
	if (ret)
		return ret;
	ret = regmap_update_bits(pk->map, PMIC_IRQ_MASK0, PWRKEY_BITS, 0);
	if (ret)
		return dev_err_probe(dev, ret, "cannot unmask the power key\n");
	ret = regmap_read(pk->map, PMIC_IRQ_MASK0, &m0) ?:
	      regmap_read(pk->map, PMIC_IRQ_MASK1, &m1);
	if (ret)
		return ret;
	if (device_property_read_bool(dev, "wakeup-source")) {
		device_init_wakeup(dev, true);
		ret = devm_pm_set_wake_irq(dev, irq);
		if (ret)
			dev_warn(dev, "cannot set wake IRQ %d (%d)\n", irq, ret);
	}
	dev_info(dev, "power key armed: mask=%02x,%02x irq=%d wakeup=%d\n",
		 m0 & 0xff, m1 & 0xff, irq, device_may_wakeup(dev));
	return 0;
}

static const struct of_device_id hi6250_pwrkey_of_match[] = {
	{ .compatible = "hisilicon,hi6250-pmic-powerkey" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6250_pwrkey_of_match);

static struct platform_driver hi6250_pwrkey_driver = {
	.probe = hi6250_pwrkey_probe,
	.driver = {
		.name = "hi6250-pmic-powerkey",
		.of_match_table = hi6250_pwrkey_of_match,
	},
};
module_platform_driver(hi6250_pwrkey_driver);

MODULE_DESCRIPTION("Hi6250 PMIC power key");
MODULE_LICENSE("GPL");
