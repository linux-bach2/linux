// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only battery monitor for the HI6555V100 coulomb counter in the Hi6250 (Kirin 659) PMIC,
 * as on the Huawei MediaPad M5 lite 10 (bach2).
 *
 * The PMIC is reached through the pmu_ctrl syscon window: 8-bit registers at a 4-byte stride.
 * Register map and conversions follow the vendor 4.9 driver
 * (drivers/power/hisi/coul/hi6555v100/hisi_hi6555v100_coul.[ch]):
 *
 *   0x1f2/0x1f3  CURRENT  s16, 1 LSB = 5/10661 A across the 10 mOhm sense resistor,
 *                         negative while charging (the vendor's "in" direction)
 *   0x1f4/0x1f5  V_OUT    s16, 1 LSB = 4.8/21322 V, then * a / 1e6 + b (trim)
 *   0x1f6        CLJ_CTRL coulomb counter control (only read here)
 *   0x1f8-0x1fb  CL_OUT   u32 LE, charge out; 0x1fc-0x1ff CL_IN, charge in.
 *                         1 LSB = 143127e-7 uAh (32 kHz mode; 143153e-7 with the DCXO clock)
 *   0x182/0x183  OTP      ATE voltage trim a (9 bits) and b (bits 6:1)
 *
 * This driver never writes a register: it does not start, stop, calibrate or clear the
 * counter. It only takes differences of CL_IN/CL_OUT between its own samples, so whatever
 * absolute value the bootloader or the stock kernel left there does not matter.
 *
 * Capacity: every POLL_S the open-circuit voltage is estimated as V + I_discharge * R
 * (factory-internal-resistance-micro-ohms of the monitored battery) and looked up in the
 * battery's ocv-capacity table nearest 25 C (there is no battery temperature on mainline).
 * The state starts at that OCV estimate and is then propagated by coulomb counting and pulled
 * towards the OCV estimate with a time constant of TAU_REST_S at low current and TAU_LOAD_S
 * otherwise (a complementary filter). Nothing persists across boots: each boot re-anchors on
 * the OCV estimate.
 */
#include <linux/devm-helpers.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define PMIC_REG(r)		((r) << 2)
#define COUL_VERSION4		0x004
#define COUL_VERSION5		0x005
#define COUL_OTP2_2		0x182	/* ATE a, low 8 bits */
#define COUL_OTP2_3		0x183	/* ATE a bit 8 = bit 7; ATE b = bits 6:1 */
#define COUL_CURRENT_0		0x1f2
#define COUL_V_OUT_0		0x1f4
#define COUL_CLJ_CTRL		0x1f6
#define COUL_CL_OUT_0		0x1f8
#define COUL_CL_IN_0		0x1fc

#define VOL_OFFSET_A_BASE	974400
#define VOL_OFFSET_A_STEP	100
#define VOL_OFFSET_B_BASE	(-31500)
#define VOL_OFFSET_B_STEP	500
#define CC_UAH_NUM		143127	/* uAh per LSB * 1e7 */
#define CC_UAH_DEN		10000000

#define POLL_S			10
#define TAU_REST_S		1800	/* |I| < REST_UA: trust the OCV estimate within ~30 min */
#define TAU_LOAD_S		7200
#define REST_UA			100000
#define STATUS_DEADBAND_UA	20000
#define OCV_TEMP_C		25
#define MPCT			1000	/* state is kept in milli-percent */
#define REPORT_HYST		700	/* reported capacity moves only after 0.7 % of change */

struct hi6555v100_bat {
	struct device *dev;
	struct regmap *map;
	struct power_supply *psy;
	struct delayed_work work;
	struct mutex lock;
	int v_a;		/* ppm */
	int v_b;		/* uV */
	int c_a;		/* ppm */
	int c_b;		/* uA, discharge positive (vendor convention) */
	int r_uohm;
	int fcc_uah;
	/* state, under lock */
	bool have_state;
	u32 cl_in, cl_out;
	ktime_t t_last;
	int soc;		/* milli-percent */
	int capacity;		/* reported percent */
	int i_avg_ua;		/* power_supply convention: > 0 charging */
	int ocv_uv;
	int status;
};

static int coul_read8(struct hi6555v100_bat *b, unsigned int reg, u8 *v)
{
	unsigned int x;
	int ret = regmap_read(b->map, PMIC_REG(reg), &x);

	if (!ret)
		*v = x & 0xff;
	return ret;
}

/* A multi-byte sample is several registers; re-read until two passes agree. */
static int coul_read_le(struct hi6555v100_bat *b, unsigned int reg, int n, u32 *v)
{
	u32 a, prev = 0;
	int tries, i, ret;
	u8 x;

	for (tries = 0; tries < 4; tries++) {
		a = 0;
		for (i = n - 1; i >= 0; i--) {
			ret = coul_read8(b, reg + i, &x);
			if (ret)
				return ret;
			a = (a << 8) | x;
		}
		if (tries && a == prev)
			break;
		prev = a;
	}
	*v = a;
	return 0;
}

static int coul_voltage_uv(struct hi6555v100_bat *b, int *uv)
{
	u32 raw;
	s64 t;
	int ret = coul_read_le(b, COUL_V_OUT_0, 2, &raw);

	if (ret)
		return ret;
	t = div_s64((s64)(s16)raw * 2400000, 10661);
	t = div_s64(t * b->v_a, 1000000);
	*uv = (int)t + b->v_b;
	return 0;
}

/* power_supply convention: positive while charging. */
static int coul_current_ua(struct hi6555v100_bat *b, int *ua)
{
	u32 raw;
	s64 t;
	int ret = coul_read_le(b, COUL_CURRENT_0, 2, &raw);

	if (ret)
		return ret;
	t = div_s64((s64)(s16)raw * 5000000, 10661);
	*ua = -((int)div_s64(t * b->c_a, 1000000) + b->c_b);
	return 0;
}

/* OCV (uV) -> milli-percent, linear between the battery's table points nearest 25 C. */
static int ocv_to_mpct(struct hi6555v100_bat *b, int ocv)
{
	struct power_supply_battery_info *info = b->psy->battery_info;
	const struct power_supply_battery_ocv_table *t;
	int len, i;

	t = info ? power_supply_find_ocv2cap_table(info, OCV_TEMP_C, &len) : NULL;
	if (!t || len < 2)
		return -ENODATA;
	if (ocv >= t[0].ocv)
		return t[0].capacity * MPCT;
	for (i = 1; i < len; i++) {
		if (ocv >= t[i].ocv)
			return t[i].capacity * MPCT +
			       (int)div_s64((s64)(ocv - t[i].ocv) *
					    (t[i - 1].capacity - t[i].capacity) * MPCT,
					    t[i - 1].ocv - t[i].ocv);
	}
	return t[len - 1].capacity * MPCT;
}

static void hi6555v100_bat_update(struct hi6555v100_bat *b)
{
	int uv, ua, ocv, soc_ocv, tau, status, old_cap, old_status;
	u32 cl_in, cl_out;
	ktime_t now = ktime_get_boottime();
	s64 dq_uah = 0, dt_ms;

	if (coul_voltage_uv(b, &uv) || coul_current_ua(b, &ua) ||
	    coul_read_le(b, COUL_CL_IN_0, 4, &cl_in) || coul_read_le(b, COUL_CL_OUT_0, 4, &cl_out)) {
		dev_err_ratelimited(b->dev, "coul read failed\n");
		return;
	}
	/* Discharge current raises the OCV above the terminal voltage, charge lowers it. */
	ocv = uv - (int)div_s64((s64)ua * b->r_uohm, 1000000);
	soc_ocv = ocv_to_mpct(b, ocv);

	mutex_lock(&b->lock);
	old_cap = b->capacity;
	old_status = b->status;
	b->ocv_uv = ocv;
	if (!b->have_state) {
		if (soc_ocv >= 0) {
			b->soc = soc_ocv;
			b->capacity = DIV_ROUND_CLOSEST(b->soc, MPCT);
			b->have_state = true;
		}
		b->i_avg_ua = ua;
	} else {
		dt_ms = ktime_ms_delta(now, b->t_last);
		/* u32 wrap-around differences; > 0 = net discharge */
		dq_uah = div_s64(((s64)(u32)(cl_out - b->cl_out) - (s64)(u32)(cl_in - b->cl_in)) *
				 CC_UAH_NUM, CC_UAH_DEN);
		/* vendor hi6555v100_coul_calculate_cc_uah(): scale by a, offset b over time */
		dq_uah = div_s64(dq_uah * b->c_a, 1000000) + div_s64((s64)b->c_b * dt_ms, 3600000);
		if (dt_ms > 0)
			b->i_avg_ua = -(int)div_s64(dq_uah * 3600000, dt_ms);
		if (b->fcc_uah > 0)
			b->soc -= (int)div_s64(dq_uah * 100 * MPCT, b->fcc_uah);
		if (soc_ocv >= 0 && dt_ms > 0) {
			tau = abs(b->i_avg_ua) < REST_UA ? TAU_REST_S : TAU_LOAD_S;
			b->soc += (int)div_s64((s64)(soc_ocv - b->soc) * min_t(s64, dt_ms, tau * 1000LL),
					       tau * 1000LL);
		}
		b->soc = clamp(b->soc, 0, 100 * MPCT);
		if (abs(b->soc - b->capacity * MPCT) > REPORT_HYST)
			b->capacity = DIV_ROUND_CLOSEST(b->soc, MPCT);
	}
	b->cl_in = cl_in;
	b->cl_out = cl_out;
	b->t_last = now;

	if (b->i_avg_ua > STATUS_DEADBAND_UA)
		status = POWER_SUPPLY_STATUS_CHARGING;
	else if (b->i_avg_ua < -STATUS_DEADBAND_UA)
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (b->capacity >= 99)
		status = POWER_SUPPLY_STATUS_FULL;
	else
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	b->status = status;
	mutex_unlock(&b->lock);

	dev_dbg(b->dev, "v=%d ocv=%d i=%d iavg=%d dq=%lld soc_ocv=%d soc=%d cap=%d\n",
		uv, ocv, ua, b->i_avg_ua, dq_uah, soc_ocv, b->soc, b->capacity);
	if (old_cap != b->capacity || old_status != status)
		power_supply_changed(b->psy);
}

static void hi6555v100_bat_work(struct work_struct *work)
{
	struct hi6555v100_bat *b = container_of(work, struct hi6555v100_bat, work.work);

	hi6555v100_bat_update(b);
	queue_delayed_work(system_freezable_wq, &b->work, POLL_S * HZ);
}

static const enum power_supply_property hi6555v100_bat_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL,
};

static int hi6555v100_bat_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct hi6555v100_bat *b = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return coul_voltage_uv(b, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return coul_current_ua(b, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (b->fcc_uah <= 0)
			return -ENODATA;
		val->intval = b->fcc_uah;
		return 0;
	default:
		break;
	}

	mutex_lock(&b->lock);
	if (!b->have_state) {
		ret = -ENODATA;
	} else {
		switch (psp) {
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = b->status;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = b->capacity;
			break;
		case POWER_SUPPLY_PROP_VOLTAGE_OCV:
			val->intval = b->ocv_uv;
			break;
		case POWER_SUPPLY_PROP_CURRENT_AVG:
			val->intval = b->i_avg_ua;
			break;
		default:
			ret = -EINVAL;
		}
	}
	mutex_unlock(&b->lock);
	return ret;
}

static const struct power_supply_desc hi6555v100_bat_desc = {
	.name		= "battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= hi6555v100_bat_props,
	.num_properties	= ARRAY_SIZE(hi6555v100_bat_props),
	.get_property	= hi6555v100_bat_get_property,
};

static int hi6555v100_bat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct power_supply_battery_info *info;
	struct hi6555v100_bat *b;
	u8 ver4, ver5, otp2, otp3, ctrl;
	u32 raw_v, raw_i, v;
	int uv, ua, ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	mutex_init(&b->lock);
	b->status = POWER_SUPPLY_STATUS_UNKNOWN;
	b->map = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,pmic-syscon");
	if (IS_ERR(b->map))
		return dev_err_probe(dev, PTR_ERR(b->map), "no pmic syscon\n");

	ret = coul_read8(b, COUL_VERSION4, &ver4) ?: coul_read8(b, COUL_VERSION5, &ver5) ?:
	      coul_read8(b, COUL_OTP2_2, &otp2) ?: coul_read8(b, COUL_OTP2_3, &otp3) ?:
	      coul_read8(b, COUL_CLJ_CTRL, &ctrl) ?:
	      coul_read_le(b, COUL_V_OUT_0, 2, &raw_v) ?: coul_read_le(b, COUL_CURRENT_0, 2, &raw_i);
	if (ret)
		return dev_err_probe(dev, ret, "register read failed\n");

	/* Vendor hi6555v100_coul_get_ate_a/_b; DT may override with the stock in-use values. */
	b->v_a = VOL_OFFSET_A_BASE +
		 ((otp2 | ((otp3 & 0x80) << 1)) & 0x1ff) * VOL_OFFSET_A_STEP;
	b->v_b = VOL_OFFSET_B_BASE + (otp3 & 0x7e) * VOL_OFFSET_B_STEP;
	if (!of_property_read_u32(dev->of_node, "hisilicon,v-offset-a", &v))
		b->v_a = v;
	if (!of_property_read_u32(dev->of_node, "hisilicon,v-offset-b-microvolt", &v))
		b->v_b = (s32)v;
	b->c_a = 1000000;	/* stock DT coul_core current_offset_a */
	if (!of_property_read_u32(dev->of_node, "hisilicon,c-offset-a", &v))
		b->c_a = v;
	if (!of_property_read_u32(dev->of_node, "hisilicon,c-offset-b-microamp", &v))
		b->c_b = (s32)v;
	if (of_property_read_u32(dev->of_node, "hisilicon,charge-full-microamp-hours", &v))
		v = 0;
	b->fcc_uah = v;

	coul_voltage_uv(b, &uv);
	coul_current_ua(b, &ua);
	dev_info(dev, "coul: ver=%02x,%02x otp=%02x,%02x a=%d b=%d c_a=%d c_b=%d ctrl=%02x raw_v=%d raw_i=%d -> %d uV %d uA (read-only)\n",
		 ver4, ver5, otp2, otp3, b->v_a, b->v_b, b->c_a, b->c_b, ctrl, (s16)raw_v, (s16)raw_i, uv, ua);

	cfg.drv_data = b;
	cfg.fwnode = dev_fwnode(dev);
	b->psy = devm_power_supply_register(dev, &hi6555v100_bat_desc, &cfg);
	if (IS_ERR(b->psy))
		return dev_err_probe(dev, PTR_ERR(b->psy), "power_supply register failed\n");
	info = b->psy->battery_info;
	if (!info)
		return dev_err_probe(dev, -ENODEV, "monitored-battery is required\n");
	if (!b->fcc_uah)
		b->fcc_uah = info->charge_full_design_uah;
	b->r_uohm = info->factory_internal_resistance_uohm > 0 ?
		    info->factory_internal_resistance_uohm : 0;
	if (ocv_to_mpct(b, uv) < 0)
		dev_warn(dev, "battery has no ocv-capacity table: capacity unavailable\n");

	ret = devm_delayed_work_autocancel(dev, &b->work, hi6555v100_bat_work);
	if (ret)
		return ret;
	hi6555v100_bat_update(b);
	dev_info(dev, "capacity: ocv=%d uV -> %d %% (R=%d uohm, fcc=%d uAh, poll %d s)\n",
		 b->ocv_uv, b->capacity, b->r_uohm, b->fcc_uah, POLL_S);
	queue_delayed_work(system_freezable_wq, &b->work, POLL_S * HZ);
	return 0;
}

static const struct of_device_id hi6555v100_bat_of_match[] = {
	{ .compatible = "hisilicon,hi6555v100-coul" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6555v100_bat_of_match);

static struct platform_driver hi6555v100_bat_driver = {
	.probe = hi6555v100_bat_probe,
	.driver = {
		.name = "hi6555v100-battery",
		.of_match_table = hi6555v100_bat_of_match,
	},
};
module_platform_driver(hi6555v100_bat_driver);

MODULE_DESCRIPTION("HI6555V100 coulomb counter, read-only battery monitor");
MODULE_LICENSE("GPL");
