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
 * By default the regmap refuses every write: the driver only reports the state the bootloader (or
 * the hardware defaults after a watchdog expiry) left, and warns when that state exceeds the
 * monitored battery's stock limits.
 *
 * With "hisilicon,managed-charging" in DT it manages charging (bring-up docs/mainline-power.md,
 * "P2"), with every value at or below the most conservative stock setting: CV 4100 mV, ICHG 700 mA,
 * termination 300 mA (HW termination on), fast-charge timer 12 h, input limit 1000 mA (2000 mA only
 * via "hisilicon,input-current-limit-microamp"), the chip's I2C watchdog at 40 s, kicked every 10 s.
 * Only those fields are written, each read-modify-write and read back. Any mismatch is re-applied
 * once; an I2C error or a battery voltage above 4150 mV turns charging off. remove/shutdown turn it off.
 * If Linux hangs, the kicks stop and the chip drops chg_en within 40 s.
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
#define HI6523_WDT_RST		0xe8
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
	bool managed;		/* P2 active */
	bool ovp;		/* software OVP tripped: chg_en held 0 until below the release level */
	unsigned int iin_code;
	unsigned int start_tries;
	struct delayed_work work;
	struct delayed_work start;
};

/* P2 fields: register, mask, value. Order matters: chg_en off first, on last. */
struct hi6523_field { u8 reg, mask, val; const char *name; };
#define P2_IIN_1000 4		/* BUCK_REG0[6:2] code: 1000 mA */
#define P2_IIN_2000 13		/* 2000 mA, opt-in for a known DCP */
static struct hi6523_field p2_fields[] = {
	{ 0x90, BIT(5),        0,             "chg_en off" },
	{ 0x92, GENMASK(6, 0), (6 << 3) | 4,  "CV 4100 mV, iterm 300 mA" },
	{ 0x91, GENMASK(4, 0), 6,             "ICHG 700 mA" },
	{ 0x93, GENMASK(3, 2), 2 << 2,        "fast timer 12 h" },
	{ 0x94, BIT(2),        BIT(2),        "HW termination on" },
	{ 0x60, GENMASK(6, 2), P2_IIN_1000 << 2, "IIN" },
	{ 0xe9, GENMASK(1, 0), 3,             "I2C watchdog 40 s" },
	{ 0x90, BIT(5),        BIT(5),        "chg_en on" },
};
#define P2_OVP_MV	4150
#define P2_OVP_RELEASE_MV 4000
#define P2_VMIN_MV	3300
#define P2_PERIOD	(10 * HZ)

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

/* Managed mode: only the P2 registers and the watchdog kick are writable. */
static bool hi6523_p2_writeable(struct device *dev, unsigned int reg)
{
	return reg == 0x60 || (reg >= 0x90 && reg <= 0x94) || reg == HI6523_WDT_RST || reg == HI6523_WDT_CTRL;
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

static const struct regmap_config hi6523_regmap_p2 = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = HI6523_MAX_REG,
	.readable_reg = hi6523_readable,
	.writeable_reg = hi6523_p2_writeable,
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

static int hi6523_battery_mv(void)
{
	struct power_supply *bat = power_supply_get_by_name("battery");
	union power_supply_propval v;
	int ret;

	if (!bat)
		return -ENODEV;
	ret = power_supply_get_property(bat, POWER_SUPPLY_PROP_VOLTAGE_NOW, &v);
	power_supply_put(bat);
	return ret ? ret : v.intval / 1000;
}

static int hi6523_chg_off(struct hi6523 *c, const char *why)
{
	int ret = regmap_update_bits(c->map, HI6523_CHG_REG0, CHG_REG0_CHG_EN, 0);

	dev_warn(c->dev, "managed: chg_en = 0 (%s)%s\n", why, ret ? ": write failed" : "");
	return ret;
}

/* Apply one field and read it back. */
static int hi6523_p2_field(struct hi6523 *c, const struct hi6523_field *f)
{
	unsigned int v;
	int ret = regmap_update_bits(c->map, f->reg, f->mask, f->val) ?: regmap_read(c->map, f->reg, &v);

	if (ret)
		return ret;
	if ((v & f->mask) != f->val) {
		dev_err(c->dev, "managed: %s: reg 0x%02x = 0x%02x, want 0x%02x under 0x%02x\n",
			f->name, f->reg, v, f->val, f->mask);
		return -EIO;
	}
	return 0;
}

static int hi6523_p2_apply(struct hi6523 *c, bool enable)
{
	unsigned int i, n = ARRAY_SIZE(p2_fields) - (enable ? 0 : 1);
	int ret;

	for (i = 0; i < n; i++) {
		ret = hi6523_p2_field(c, &p2_fields[i]);
		if (ret) {
			hi6523_chg_off(c, "apply failed");
			return ret;
		}
	}
	return 0;
}

/* Do the configured P2 fields (all but the chg_en steps) still hold? */
static bool hi6523_p2_holds(struct hi6523 *c)
{
	unsigned int i, v;

	for (i = 1; i < ARRAY_SIZE(p2_fields) - 1; i++)
		if (regmap_read(c->map, p2_fields[i].reg, &v) ||
		    (v & p2_fields[i].mask) != p2_fields[i].val)
			return false;
	return true;
}

static void hi6523_p2_log(struct hi6523 *c, const char *tag)
{
	unsigned int r[7] = {}, regs[7] = { 0x92, 0x91, 0x93, 0x94, 0x60, 0xe9, 0x90 }, i;

	for (i = 0; i < 7; i++)
		regmap_read(c->map, regs[i], &r[i]);
	dev_info(c->dev, "managed (%s): 92=%02x 91=%02x 93=%02x 94=%02x 60=%02x e9=%02x 90=%02x vbat=%d mV\n",
		 tag, r[0], r[1], r[2], r[3], r[4], r[5], r[6], hi6523_battery_mv());
}

static void hi6523_p2_work(struct work_struct *w)
{
	struct hi6523 *c = container_of(to_delayed_work(w), struct hi6523, work);
	unsigned int s0, r0;
	int mv;

	if (!c->managed)
		return;
	if (regmap_write(c->map, HI6523_WDT_RST, 1) || regmap_read(c->map, HI6523_STATUS0, &s0) ||
	    regmap_read(c->map, HI6523_CHG_REG0, &r0)) {
		hi6523_chg_off(c, "I2C error");
		c->managed = false;
		return;
	}
	mv = hi6523_battery_mv();
	if (mv > P2_OVP_MV && (r0 & CHG_REG0_CHG_EN)) {
		c->ovp = true;
		hi6523_chg_off(c, "battery above 4150 mV");
	} else if (c->ovp && mv > 0 && mv < P2_OVP_RELEASE_MV) {
		c->ovp = false;
		dev_info(c->dev, "managed: battery %d mV < %d mV, charging again\n", mv, P2_OVP_RELEASE_MV);
	}
	if (!hi6523_p2_holds(c) || (!c->ovp && (s0 & STATUS0_BUCK_OK) && !(r0 & CHG_REG0_CHG_EN))) {
		/* e.g. a default-mode reset after a watchdog expiry: re-apply once */
		hi6523_p2_log(c, "drift, re-applying");
		if (hi6523_p2_apply(c, !c->ovp)) {
			c->managed = false;
			return;
		}
	}
	schedule_delayed_work(&c->work, P2_PERIOD);
}

static void hi6523_p2_stop(void *data)
{
	struct hi6523 *c = data;

	c->managed = false;
	cancel_delayed_work_sync(&c->work);
	hi6523_chg_off(c, "driver stop");
}

static void hi6523_p2_start(struct hi6523 *c, unsigned int s0);
static void hi6523_p2_start_work(struct work_struct *w)
{
	struct hi6523 *c = container_of(to_delayed_work(w), struct hi6523, start);
	unsigned int s0;

	if (!regmap_read(c->map, HI6523_STATUS0, &s0))
		hi6523_p2_start(c, s0);
}

static void hi6523_p2_start_cancel(void *data)
{
	cancel_delayed_work_sync(&((struct hi6523 *)data)->start);
}

static void hi6523_p2_start(struct hi6523 *c, unsigned int s0)
{
	u32 ua = 0;
	int mv = hi6523_battery_mv(), ret;

	if (mv == -ENODEV && c->start_tries++ < 12) {	/* the battery driver may probe later */
		schedule_delayed_work(&c->start, P2_PERIOD);
		return;
	}

	/*
	 * Preconditions not met (no VBUS, or the battery outside 3300..4150 mV, e.g. still full from a stock boot):
	 * don't write anything now, but check again every 60 s, so managed charging starts once it is needed.
	 */
	if (!(s0 & STATUS0_BUCK_OK)) {
		dev_info_ratelimited(c->dev, "managed: no VBUS (STATUS0 0x%02x): not yet; rechecking every 60 s\n", s0);
		schedule_delayed_work(&c->start, 60 * HZ);
		return;
	}
	if (mv < P2_VMIN_MV || mv > 4400) {
		dev_info_ratelimited(c->dev, "managed: battery %d mV outside %d..4400 mV: not yet; rechecking every 60 s\n",
				     mv, P2_VMIN_MV);
		schedule_delayed_work(&c->start, 60 * HZ);
		return;
	}
	/* Full battery (e.g. after a stock boot): configure everything but keep chg_en = 0 (the OVP-held state);
	 * the loop enables charging only once the battery is below 4000 mV. */
	if (mv > P2_OVP_MV)
		c->ovp = true;
	of_property_read_u32(c->dev->of_node, "hisilicon,input-current-limit-microamp", &ua);
	p2_fields[5].val = (ua >= 2000000 ? P2_IIN_2000 : P2_IIN_1000) << 2;
	INIT_DELAYED_WORK(&c->work, hi6523_p2_work);
	ret = hi6523_p2_apply(c, !c->ovp);
	if (ret) {
		dev_err(c->dev, "managed: apply failed (%d): charging off, not managing\n", ret);
		return;
	}
	c->managed = true;
	if (devm_add_action_or_reset(c->dev, hi6523_p2_stop, c))
		return;
	hi6523_p2_log(c, c->ovp ? "readback ok, battery full: chg_en held 0" : "readback ok");
	schedule_delayed_work(&c->work, P2_PERIOD);
}

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
	i2c_set_clientdata(client, c);
	c->map = devm_regmap_init_i2c(client, of_property_read_bool(dev->of_node, "hisilicon,managed-charging") ?
				      &hi6523_regmap_p2 : &hi6523_regmap);
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
	/* read-only unless managed */

	if (of_property_read_bool(dev->of_node, "hisilicon,managed-charging")) {
		INIT_DELAYED_WORK(&c->start, hi6523_p2_start_work);
		ret = devm_add_action_or_reset(dev, hi6523_p2_start_cancel, c);
		if (ret)
			return ret;
		hi6523_p2_start(c, s0);
	}

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

static void hi6523_shutdown(struct i2c_client *client)
{
	struct hi6523 *c = i2c_get_clientdata(client);

	if (c && c->managed) {
		c->managed = false;
		cancel_delayed_work_sync(&c->work);
		hi6523_chg_off(c, "shutdown");
	}
}

static struct i2c_driver hi6523_driver = {
	.probe = hi6523_probe,
	.shutdown = hi6523_shutdown,
	.driver = {
		.name = "hi6523-charger",
		.of_match_table = hi6523_of_match,
	},
};
module_i2c_driver(hi6523_driver);

MODULE_DESCRIPTION("HiSilicon Hi6523 charger: status, optional conservative managed charging");
MODULE_LICENSE("GPL");
