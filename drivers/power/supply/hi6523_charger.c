// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only status driver for the HiSilicon Hi6523 ("SchargerV300") switching charger at I2C
 * 0x6b, as on the Huawei MediaPad M5 lite 10 (bach2).
 *
 * Register map and encodings follow the vendor 4.9 driver
 * (drivers/power/hisi/charger/schargerV300/hisi_scharger_v300.[ch] and
 * drivers/hisi/ap/platform/hi6250/soc_schargerV300_interface.h):
 *
 *   0x04         CHIP_VERSION_4  chip id 0x01, 0x02 or 0x03
 *   0x06         STATUS0         bit 7 buck_ok, bits 5:4 chgstate (3 = done), bit 1 therm, bit 2 dpm
 *   0x07         STATUS1         bit 1 wdt_time_out_n (stayed 1 across an observed expiry, E3)
 *   0x0f         WATCHDOG_IRQ    bit 0: the I2C watchdog expired (latched, write-1-to-clear;
 *                                nothing on mainline clears it). E3: e9 03 -> 00 with 0f = 01
 *   0x60         BUCK_REG0       bits 6:2 input current limit
 *   0x90         CHG_REG0        bit 5 chg_en
 *   0x91         CHG_REG1        bits 4:0 fast charge current
 *   0x92         CHG_REG2        bits 6:3 CV (3800 + 50 mV/step), bits 2:0 termination current
 *   0xa3         SCHG_LOGIC_CTRL bit 0 buck_enb (1 = HiZ)
 *   0xe9         WATCHDOG_CTRL   bits 1:0: 0 off, 1 10 s, 2 20 s, 3 40 s
 *
 * The regmap refuses every write: this driver cannot change charging. It reports the state the
 * bootloader (or the hardware defaults after a watchdog expiry) left, and warns when that state
 * exceeds the monitored battery's stock limits.
 */
#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define HI6523_CHIP_VERSION_4	0x04
#define HI6523_STATUS0		0x06
#define HI6523_STATUS1		0x07
#define HI6523_WDT_IRQ		0x0f
#define HI6523_BUCK_REG0	0x60
#define HI6523_CHG_REG0		0x90
#define HI6523_CHG_REG1		0x91
#define HI6523_CHG_REG2		0x92
#define HI6523_CHG_REG3		0x93
#define HI6523_CHG_REG4		0x94
#define HI6523_LOGIC_CTRL	0xa3
#define HI6523_WDT_CTRL		0xe9
#define HI6523_MAX_REG		0xea

#define STATUS0_BUCK_OK		BIT(7)
#define STATUS0_CHGSTATE	GENMASK(5, 4)
#define STATUS0_CHGSTATE_DONE	3
#define CHG_REG0_CHG_EN		BIT(5)
#define LOGIC_CTRL_HIZ		BIT(0)

struct hi6523 {
	struct device *dev;
	struct regmap *map;
	struct power_supply *psy;
};

/* Readable windows; FCP (0x20-0x56), ADC (0xb0-0xb5) and eFuse (0xc0-0xd2) are left alone. */
static bool hi6523_readable(struct device *dev, unsigned int reg)
{
	return reg <= 0x13 || (reg >= 0x60 && reg <= 0x70) || (reg >= 0x80 && reg <= 0x81) ||
	       (reg >= 0x90 && reg <= 0x9b) || (reg >= 0xa0 && reg <= 0xa5) ||
	       (reg >= 0xe8 && reg <= 0xe9);
}

static bool hi6523_never(struct device *dev, unsigned int reg)
{
	return false;
}

static const struct regmap_config hi6523_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = HI6523_MAX_REG,
	.readable_reg = hi6523_readable,
	.writeable_reg = hi6523_never,
	.volatile_reg = hi6523_readable,
	.precious_reg = hi6523_never,
	.cache_type = REGCACHE_NONE,
};

/* Vendor hi6523_set_charge_current(), inverted. */
static int hi6523_ichg_ma(unsigned int code)
{
	if (code <= 9)
		return 100 + 100 * code;
	if (code <= 14)
		return 1000 + 200 * (code - 9);
	if (code <= 24)
		return 2000 + 100 * (code - 14);
	if (code <= 30)
		return 3000 + 200 * (code - 24);
	return 4500;
}

/* Vendor BUCK_REG0 table: 100, 150, 500, 900, 1000, 1200, then 100 mA steps to 2200, ... */
static int hi6523_iin_ma(unsigned int code)
{
	static const u16 t[] = { 100, 150, 500, 900, 1000, 1200, 1300, 1400, 1500, 1600, 1700,
				 1800, 1900, 2000, 2100, 2200, 2400, 2600, 2800, 3000, 3200 };

	return t[min_t(unsigned int, code, ARRAY_SIZE(t) - 1)];
}

static int hi6523_cv_mv(unsigned int code)
{
	return code >= 14 ? 4500 : 3800 + 50 * code;
}

static int hi6523_read(struct hi6523 *c, unsigned int reg, unsigned int *v)
{
	int ret = regmap_read(c->map, reg, v);

	if (ret)
		dev_err_ratelimited(c->dev, "read 0x%02x failed: %d\n", reg, ret);
	return ret;
}

static const enum power_supply_property hi6523_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static int hi6523_get_property(struct power_supply *psy, enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct hi6523 *c = power_supply_get_drvdata(psy);
	unsigned int s0, s1, r;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = hi6523_read(c, HI6523_STATUS0, &s0);
		if (!ret)
			val->intval = !!(s0 & STATUS0_BUCK_OK);
		return ret;
	case POWER_SUPPLY_PROP_STATUS:
		ret = hi6523_read(c, HI6523_STATUS0, &s0) ?: hi6523_read(c, HI6523_CHG_REG0, &r);
		if (ret)
			return ret;
		if (!(s0 & STATUS0_BUCK_OK))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (FIELD_GET(STATUS0_CHGSTATE, s0) == STATUS0_CHGSTATE_DONE)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (!(r & CHG_REG0_CHG_EN) || !FIELD_GET(STATUS0_CHGSTATE, s0))
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = hi6523_read(c, HI6523_WDT_IRQ, &s1);
		if (!ret)
			val->intval = (s1 & BIT(0)) ? POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE :
				      POWER_SUPPLY_HEALTH_GOOD;
		return ret;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = hi6523_read(c, HI6523_CHG_REG1, &r);
		if (!ret)
			val->intval = hi6523_ichg_ma(r & 0x1f) * 1000;
		return ret;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = hi6523_read(c, HI6523_CHG_REG2, &r);
		if (!ret)
			val->intval = hi6523_cv_mv((r >> 3) & 0xf) * 1000;
		return ret;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = hi6523_read(c, HI6523_BUCK_REG0, &r);
		if (!ret)
			val->intval = hi6523_iin_ma((r >> 2) & 0x1f) * 1000;
		return ret;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc hi6523_desc = {
	.name		= "charger",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= hi6523_props,
	.num_properties	= ARRAY_SIZE(hi6523_props),
	.get_property	= hi6523_get_property,
};

static int hi6523_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = {};
	struct power_supply_battery_info *bat;
	unsigned int id, s0, s1, iin, r0, r1, r2, r3, r4, hiz, wdt, wirq;
	struct hi6523 *c;
	int cv, ichg, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	c->map = devm_regmap_init_i2c(client, &hi6523_regmap);
	if (IS_ERR(c->map))
		return PTR_ERR(c->map);

	ret = hi6523_read(c, HI6523_CHIP_VERSION_4, &id);
	if (ret)
		return ret;
	if (id < 1 || id > 3)
		return dev_err_probe(dev, -ENODEV, "chip id 0x%02x is not a Hi6523\n", id);

	ret = hi6523_read(c, HI6523_STATUS0, &s0) ?: hi6523_read(c, HI6523_STATUS1, &s1) ?:
	      hi6523_read(c, HI6523_BUCK_REG0, &iin) ?: hi6523_read(c, HI6523_CHG_REG0, &r0) ?:
	      hi6523_read(c, HI6523_CHG_REG1, &r1) ?: hi6523_read(c, HI6523_CHG_REG2, &r2) ?:
	      hi6523_read(c, HI6523_CHG_REG3, &r3) ?: hi6523_read(c, HI6523_CHG_REG4, &r4) ?:
	      hi6523_read(c, HI6523_LOGIC_CTRL, &hiz) ?: hi6523_read(c, HI6523_WDT_CTRL, &wdt) ?:
	      hi6523_read(c, HI6523_WDT_IRQ, &wirq);
	if (ret)
		return ret;
	cv = hi6523_cv_mv((r2 >> 3) & 0xf);
	ichg = hi6523_ichg_ma(r1 & 0x1f);
	dev_info(dev, "hi6523: id=%02x st0=%02x st1=%02x buck0=%02x chg0..4=%02x,%02x,%02x,%02x,%02x hiz=%02x wdt=%02x wdt_irq=%02x -> chg_en=%d cv=%d mV ichg=%d mA iin=%d mA (read-only)\n",
		 id, s0, s1, iin, r0, r1, r2, r3, r4, hiz, wdt, wirq, !!(r0 & CHG_REG0_CHG_EN), cv, ichg,
		 hi6523_iin_ma((iin >> 2) & 0x1f));

	cfg.drv_data = c;
	cfg.fwnode = dev_fwnode(dev);
	c->psy = devm_power_supply_register(dev, &hi6523_desc, &cfg);
	if (IS_ERR(c->psy))
		return dev_err_probe(dev, PTR_ERR(c->psy), "power_supply register failed\n");

	/* Charger supplies do not get psy->battery_info from the core; look it up here. */
	if (power_supply_get_battery_info(c->psy, &bat)) {
		dev_warn(dev, "no monitored-battery: cannot compare against stock limits\n");
		return 0;
	}
	if (bat->constant_charge_voltage_max_uv > 0 && cv * 1000 > bat->constant_charge_voltage_max_uv)
		dev_warn(dev, "CV %d mV exceeds the battery's %d uV (left by the bootloader)\n",
			 cv, bat->constant_charge_voltage_max_uv);
	if (bat->constant_charge_current_max_ua > 0 &&
	    ichg * 1000 > bat->constant_charge_current_max_ua)
		dev_warn(dev, "ICHG %d mA exceeds the battery's %d uA (left by the bootloader)\n",
			 ichg, bat->constant_charge_current_max_ua);
	power_supply_put_battery_info(c->psy, bat);
	return 0;
}

static const struct of_device_id hi6523_of_match[] = {
	{ .compatible = "hisilicon,hi6523-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6523_of_match);

static struct i2c_driver hi6523_driver = {
	.probe = hi6523_probe,
	.driver = {
		.name = "hi6523-charger",
		.of_match_table = hi6523_of_match,
	},
};
module_i2c_driver(hi6523_driver);

MODULE_DESCRIPTION("HiSilicon Hi6523 charger, read-only status");
MODULE_LICENSE("GPL");
