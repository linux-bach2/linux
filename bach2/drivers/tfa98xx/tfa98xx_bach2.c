// SPDX-License-Identifier: GPL-2.0-only
/*
 * bach2 glue for the NXP TFA98xx (TFA2) core in nxp/ (tfa_container.c, tfa_dsp.c, tfa_init.c from
 * the OnePlus 7 Pro TFA9894 driver, Apache-2.0 file headers): the four I2C primitives and the
 * DSP-message stubs the core needs, plus a staged bring-up for the 4x TFA9894 on bach2 i2c3.
 * docs/mainline-amps.md sections 8-10.
 *
 *   stage=0 (default)  read-only: rev, MTP calibration state, container load + CRC, device and
 *                      profile match. Every register write is refused (and counted).
 *   stage=1            write experiment A5a (needs approval and S4 clocks): HW init, container
 *                      device registers, INIT_CF, DSP patch load; amplifier output stays off;
 *                      read-back of the state, then power-down.
 *
 * Hard guards in the write path, at every stage:
 *   - never 0xa2-0xaf (MTP copy/read triggers, CIMTP) or 0xf0-0xff (MTP shadow): MTP is unwritable;
 *   - SYS_CTRL0 (0x00): AMPE (bit 3) and SBSL (bit 5) may not be set, so the amplifier cannot
 *     be enabled (by the host or by the DSP) at stage <= 1;
 *   - raw (multi-byte) writes may only target the CoolFlux window 0x90-0x9f.
 * Any refused write fails the operation, and the device is returned to PWDN = 1.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "tfa98xx_guard.h"
#include "nxp/tfa.h"
#include "nxp/tfa_container.h"
#include "nxp/tfa_internal.h"
#include "nxp/tfa_service.h"

#define LOGP "BACH2ML: TFA: "

static int stage;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage, "0 = read-only probe (default), 1 = A5a: patch load, amplifier off, 2 = A5: start 0x37 only (MUSIC profile, highest-index vstep) until remove, 3 = A5b/A6: the same on all four amps");
/* GK (GOER) box: B1 on hardware, docs/mainline-amps.md section 11 */
static char *fw_name = "tfa98xx/BACH2-NXP_GK_GK_GK_GK.cnt";
module_param(fw_name, charp, 0444);
MODULE_PARM_DESC(fw_name, "container file under /lib/firmware");

struct tfa98xx_bach2 {
	struct i2c_client *i2c;
	struct regmap *regmap;
	struct tfa_device tfa;
	unsigned int refused;	/* writes refused by the guards */
	int started;		/* A5: tfa_dev_start succeeded */
	unsigned int writes;	/* writes performed */
};

static DEFINE_MUTEX(cnt_lock);
static const struct firmware *cnt_fw;
static nxpTfaContainer_t *cnt;
static int cnt_users;
static struct kmem_cache *tfa_cache;

#define SYS_CTRL0	0x00
#define SYS_CTRL0_PWDN	BIT(0)
#define SYS_CTRL0_AMPE	BIT(3)
#define SYS_CTRL0_SBSL	BIT(5)

static enum Tfa98xx_Error refuse(struct tfa98xx_bach2 *t, unsigned int reg, unsigned int val, int len)
{
	t->refused++;
	dev_err(&t->i2c->dev, LOGP "0x%02x WRITE REFUSED reg 0x%02x val 0x%04x len %d (stage %d)\n",
		t->i2c->addr, reg, val, len, stage);
	return Tfa98xx_Error_Fail;
}

/* ---- the four I2C primitives the NXP core calls (tfa_service.h) ---- */

enum Tfa98xx_Error tfa98xx_read_register16(struct tfa_device *tfa, unsigned char subaddress,
					   unsigned short *val)
{
	struct tfa98xx_bach2 *t = tfa->data;
	unsigned int v;

	if (regmap_read(t->regmap, subaddress, &v))
		return Tfa98xx_Error_Fail;
	*val = v;
	return Tfa98xx_Error_Ok;
}

enum Tfa98xx_Error tfa98xx_write_register16(struct tfa_device *tfa, unsigned char subaddress,
					    unsigned short value)
{
	struct tfa98xx_bach2 *t = tfa->data;

	if (!tfa_guard_reg(stage, subaddress, value))
		return refuse(t, subaddress, value, 2);
	if (regmap_write(t->regmap, subaddress, value))
		return Tfa98xx_Error_Fail;
	t->writes++;
	return Tfa98xx_Error_Ok;
}

enum Tfa98xx_Error tfa98xx_read_data(struct tfa_device *tfa, unsigned char reg, int len,
				     unsigned char value[])
{
	struct tfa98xx_bach2 *t = tfa->data;
	struct i2c_msg m[2] = {
		{ .addr = t->i2c->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = t->i2c->addr, .flags = I2C_M_RD, .len = len, .buf = value },
	};

	return i2c_transfer(t->i2c->adapter, m, 2) == 2 ? Tfa98xx_Error_Ok : Tfa98xx_Error_Fail;
}

enum Tfa98xx_Error tfa98xx_write_raw(struct tfa_device *tfa, int len, const unsigned char data[])
{
	struct tfa98xx_bach2 *t = tfa->data;

	if (!tfa_guard_raw(stage, data, len))
		return refuse(t, len ? data[0] : 0xff, len >= 3 ? data[1] << 8 | data[2] : 0, len);
	if (i2c_master_send(t->i2c, (const char *)data, len) != len)
		return Tfa98xx_Error_Fail;
	t->writes++;
	return Tfa98xx_Error_Ok;
}

/* External-DSP message path (has_msg = 1): not used, the TFA9894 CoolFlux is reached over I2C. */
enum Tfa98xx_Error tfa98xx_init_dsp(struct tfa_device *tfa)
{
	return Tfa98xx_Error_Not_Supported;
}

enum Tfa98xx_Error tfa98xx_write_dsp(struct tfa_device *tfa, int num_bytes, const char *command_buffer)
{
	return Tfa98xx_Error_Not_Supported;
}

enum Tfa98xx_Error tfa98xx_read_dsp(struct tfa_device *tfa, int num_bytes, unsigned char *result_buffer)
{
	return Tfa98xx_Error_Not_Supported;
}

/* ---- container: one copy for all devices ---- */

static int cnt_get(struct device *dev)
{
	int ret = 0;
	enum tfa_error err;

	mutex_lock(&cnt_lock);
	if (!cnt) {
		ret = request_firmware(&cnt_fw, fw_name, dev);
		if (ret) {
			dev_err(dev, LOGP "request_firmware(%s) failed: %d\n", fw_name, ret);
			goto out;
		}
		err = tfa_load_cnt((void *)cnt_fw->data, cnt_fw->size);
		if (err != tfa_error_ok) {
			dev_err(dev, LOGP "container %s rejected by tfa_load_cnt: %d\n", fw_name, err);
			release_firmware(cnt_fw);
			cnt_fw = NULL;
			ret = -EINVAL;
			goto out;
		}
		cnt = (nxpTfaContainer_t *)cnt_fw->data;
		pr_info(LOGP "container %s size %zu %.2s%.2s%.2s customer %.8s app %.8s type %.8s ndev %d nprof %d CRC 0x%08x ok\n",
			fw_name, cnt_fw->size, cnt->id, cnt->version, cnt->subversion, cnt->customer,
			cnt->application, cnt->type, cnt->ndev, cnt->nprof, cnt->CRC);
	}
	cnt_users++;
out:
	mutex_unlock(&cnt_lock);
	return ret;
}

static void cnt_put(void)
{
	mutex_lock(&cnt_lock);
	if (cnt && !--cnt_users) {
		release_firmware(cnt_fw);
		cnt_fw = NULL;
		cnt = NULL;
	}
	mutex_unlock(&cnt_lock);
}

/* ---- read-back ---- */

static void report(struct tfa98xx_bach2 *t, const char *tag)
{
	struct tfa_device *tfa = &t->tfa;
	unsigned short c0 = 0, s0 = 0, s1 = 0, st = 0, cf = 0;

	tfa98xx_read_register16(tfa, 0x00, &c0);
	tfa98xx_read_register16(tfa, 0x10, &s0);
	tfa98xx_read_register16(tfa, 0x11, &s1);
	tfa98xx_read_register16(tfa, 0x14, &st);
	if (stage >= 1)
		tfa98xx_read_register16(tfa, 0x90, &cf);	/* CF control; stage 0 stays out of 0x90-0x9f */
	pr_info(LOGP "0x%02x %s: ctrl0=%04x status0=%04x status1=%04x manstate=%u cfctrl=%04x PWDN=%u CFE=%u AMPE=%u SBSL=%u AMPC=%u CLKS=%u PLLS=%u NOCLK=%u ACS=%u SWS=%u AMPS=%u writes=%u refused=%u\n",
		t->i2c->addr, tag, c0, s0, s1, (st >> 3) & 0xf, cf, c0 & 1, (c0 >> 2) & 1, (c0 >> 3) & 1,
		(c0 >> 5) & 1, (c0 >> 6) & 1, (s0 >> 6) & 1, (s0 >> 1) & 1, (s0 >> 8) & 1, (s0 >> 9) & 1,
		(s0 >> 11) & 1, (s0 >> 12) & 1, t->writes, t->refused);
}

/* ---- stage 1 (A5a): init, device registers, INIT_CF, patch; amplifier off ---- */

static int stage1(struct tfa98xx_bach2 *t)
{
	struct tfa_device *tfa = &t->tfa;
	enum Tfa98xx_Error err;
	enum tfa_error terr;
	unsigned short c0;
	int ready = 0, i;

	err = tfa98xx_init(tfa);	/* I2CR, DSP reset, rev 0x1a94 corrections */
	pr_info(LOGP "0x%02x A5a tfa98xx_init: %d\n", t->i2c->addr, err);
	if (err)
		return -EIO;
	err = tfaContWriteRegsDev(tfa);	/* container device-level registers (TDM, gains, boost params) */
	pr_info(LOGP "0x%02x A5a tfaContWriteRegsDev: %d\n", t->i2c->addr, err);
	if (err)
		return -EIO;
	terr = tfa_dev_set_state(tfa, TFA_STATE_INIT_CF);	/* SBSL = 0, MANSCONF = 1, PWDN = 0 */
	pr_info(LOGP "0x%02x A5a INIT_CF: %d\n", t->i2c->addr, terr);
	for (i = 0; i < 20 && !ready; i++) {
		if (tfa98xx_dsp_system_stable(tfa, &ready))
			break;
		if (!ready)
			msleep(50);
	}
	report(t, ready ? "A5a clocks-ok" : "A5a NO-CLOCKS");
	if (!ready)
		return -ETIMEDOUT;
	err = tfaContWritePatch(tfa);
	pr_info(LOGP "0x%02x A5a patch: %d\n", t->i2c->addr, err);
	report(t, err ? "A5a PATCH-FAILED" : "A5a patch-loaded");
	if (tfa98xx_read_register16(tfa, SYS_CTRL0, &c0) || (c0 & SYS_CTRL0_AMPE)) {
		pr_err(LOGP "0x%02x A5a AMPE set after patch (0x%04x)\n", t->i2c->addr, c0);
		return -EIO;
	}
	return err ? -EIO : 0;
}

static void power_down(struct tfa98xx_bach2 *t)
{
	unsigned short c0;

	if (stage < 1 || tfa98xx_read_register16(&t->tfa, SYS_CTRL0, &c0))
		return;
	tfa98xx_write_register16(&t->tfa, SYS_CTRL0, (c0 & ~SYS_CTRL0_AMPE & ~SYS_CTRL0_SBSL) | SYS_CTRL0_PWDN);
	/* INIT_CF opened the FAIM/MTP protection (OPENMTP, reg 0x01 bit 14): close it again */
	if (!tfa98xx_read_register16(&t->tfa, 0x01, &c0) && (c0 & BIT(14)))
		tfa98xx_write_register16(&t->tfa, 0x01, c0 & ~BIT(14));
	report(t, "powered-down");
}

/* ---- I2C driver ---- */

static bool tfa_volatile(struct device *dev, unsigned int reg)
{
	return true;	/* no cache: every read goes to the chip (read-back is the point) */
}

static const struct regmap_config tfa_regmap = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
	.volatile_reg = tfa_volatile,
	.cache_type = REGCACHE_NONE,
};

static int tfa98xx_bach2_probe(struct i2c_client *i2c)
{
	struct tfa98xx_bach2 *t;
	struct tfa_device *tfa;
	int ret, i, nprof;

	t = devm_kzalloc(&i2c->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->i2c = i2c;
	t->regmap = devm_regmap_init_i2c(i2c, &tfa_regmap);
	if (IS_ERR(t->regmap))
		return PTR_ERR(t->regmap);
	i2c_set_clientdata(i2c, t);
	tfa = &t->tfa;
	tfa->data = t;
	tfa->cachep = tfa_cache;
	tfa->buffer_size = 65536;
	tfa->has_msg = 0;

	if (tfa_dev_probe(i2c->addr, tfa)) {
		dev_err(&i2c->dev, LOGP "0x%02x: revision read failed\n", i2c->addr);
		return -ENODEV;
	}
	pr_info(LOGP "0x%02x rev 0x%04x family %d MTPOTC %d MTPEX %d R25C 0x%04x\n", i2c->addr, tfa->rev,
		tfa->tfa_family, tfa_dev_mtp_get(tfa, TFA_MTP_OTC), tfa_dev_mtp_get(tfa, TFA_MTP_EX),
		tfa_dev_mtp_get(tfa, TFA_MTP_RE25));
	if (tfa->rev != 0x1a94) {
		dev_err(&i2c->dev, LOGP "0x%02x: rev 0x%04x is not the expected 0x1a94\n", i2c->addr, tfa->rev);
		return -ENODEV;
	}

	ret = cnt_get(&i2c->dev);
	if (ret)
		return ret;
	tfa->cnt = cnt;
	tfa->dev_idx = tfa_cont_get_idx(tfa);
	if (tfa->dev_idx < 0) {
		dev_err(&i2c->dev, LOGP "0x%02x not in container\n", i2c->addr);
		ret = -ENODEV;
		goto err_put;
	}
	nprof = tfa_cnt_get_dev_nprof(tfa);
	pr_info(LOGP "0x%02x container device %d '%s', %d profiles\n", i2c->addr, tfa->dev_idx,
		tfaContDeviceName(cnt, tfa->dev_idx), nprof);
	for (i = 0; i < nprof; i++)
		pr_info(LOGP "0x%02x   profile %d '%s'\n", i2c->addr, i,
			tfaContProfileName(cnt, tfa->dev_idx, i));

	if (stage == 2 || stage == 3) {
		/*
		 * A5 (docs/mainline-amps.md section 17): only 0x37 is started, through the NXP DSP path (speaker
		 * protection active), with a MUSIC profile and the highest-index vstep; the others stay down. The
		 * guards keep MTP untouched; refuse outright unless MTPEX = 1 (calibrated: no calibration run).
		 * The caller plays a -40 dBFS tone; remove() stops the amp.
		 */
		int prof = -1, vmax, vstep;

		if (stage == 2 && i2c->addr != 0x37) {
			report(t, "A5 not this amp");
		} else if (tfa_dev_mtp_get(tfa, TFA_MTP_EX) != 1) {
			pr_err(LOGP "0x%02x A5 refused: MTPEX %d (not calibrated; a start would calibrate)\n",
			       i2c->addr, tfa_dev_mtp_get(tfa, TFA_MTP_EX));
		} else {
			for (i = 0; i < nprof && prof < 0; i++) {
				const char *n = tfaContProfileName(cnt, tfa->dev_idx, i);
				if (n && strstr(n, "MUSIC") && !strstr(n, ".standby"))
					prof = i;
			}
			if (prof < 0) {
				pr_err(LOGP "0x%02x A5: no MUSIC profile\n", i2c->addr);
			} else {
				vmax = tfacont_get_max_vstep(tfa, prof);
				vstep = vmax > 0 ? vmax - 1 : 0;
				pr_info(LOGP "0x%02x A5 start: profile %d '%s' vsteps %d using vstep %d\n", i2c->addr, prof,
					tfaContProfileName(cnt, tfa->dev_idx, prof), vmax, vstep);
				/*
				 * Always a cold start: after a stop the amp keeps its SW profile, and tfa_dev_start then takes
				 * the warm path, which leaves AMPE = 0 (cycles 19/20: every start after the first was silent).
				 * Forget the profile and re-init (I2CR, DSP reset) first.
				 */
				tfa_dev_set_swprof(tfa, 0xff);
				tfa_dev_set_swvstep(tfa, 0xff);
				pr_info(LOGP "0x%02x A5 cold init: %d\n", i2c->addr, tfa98xx_init(tfa));
				/*
				 * I2CR does not set ACS (c22: every start after the first of a boot was a warm start that
				 * failed with the DSP in reset). Power up as tfaRunColdStartup does, then force ACS = 1 via
				 * CF_CONTROL, so tfa_dev_start takes the cold path (patch load, DSP start).
				 */
				{
					int su = tfaRunStartup(tfa, prof), cb = su ? -1 : tfaRunColdboot(tfa, 1);

					pr_info(LOGP "0x%02x A5 startup: %d, force ACS: %d, ACS=%d\n", i2c->addr, su, cb,
						tfa_is_cold(tfa));
				}
				ret = tfa_dev_start(tfa, prof, vstep);
				report(t, ret ? "A5 START-FAILED" : "A5 started");
				pr_info(LOGP "0x%02x A5 result %d\n", i2c->addr, ret);
				if (ret)
					power_down(t);
				else
					t->started = 1;
			}
		}
	} else if (stage >= 1) {
		ret = stage1(t);
		power_down(t);
		pr_info(LOGP "0x%02x A5a result %d\n", i2c->addr, ret);
	} else {
		report(t, "stage0");
	}
	pr_info(LOGP "0x%02x done stage %d writes %u refused %u\n", i2c->addr, stage, t->writes, t->refused);
	return 0;

err_put:
	cnt_put();
	return ret;
}

static void tfa98xx_bach2_remove(struct i2c_client *i2c)
{
	struct tfa98xx_bach2 *t = i2c_get_clientdata(i2c);

	if (t->started) {
		report(t, "A5 before stop");
		pr_info(LOGP "0x%02x A5 tfa_dev_stop: %d\n", i2c->addr, tfa_dev_stop(&t->tfa));
	}
	power_down(t);
	cnt_put();
}

/*
 * A8 (docs/mainline-amps.md section 21): "live" (sysfs, read-only for the chip) asks a started amp's DSP for its
 * SpeakerBoost/framework state (FW_PARAM_GET_STATE, 9 words: agcGain limGain sMax T statusFlag X1 X2 Re shortOnMips,
 * raw 24-bit fixed point). Excursion X1/X2 and the gains move with the signal, so tone vs silence shows whether
 * the speaker is driven without any loudness or a mic. The DSP read is a CoolFlux message (guard window 0x90-0x9f).
 */
static DEFINE_MUTEX(live_lock);
static ssize_t live_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tfa98xx_bach2 *t = dev_get_drvdata(dev);
	unsigned char bytes[3 * 9] = { 0 };
	int data[9] = { 0 };
	unsigned int n = 0;
	int err;

	if (!t || !t->started)
		return sysfs_emit(buf, "not started\n");
	mutex_lock(&live_lock);
	err = tfa98xx_dsp_get_state_info(&t->tfa, bytes, &n);
	if (err) {	/* c22: the framework module answered 203 (parameter id): ask SpeakerBoost, 8 words */
		int e2 = tfa_dsp_cmd_id_write_read(&t->tfa, MODULE_SPEAKERBOOST, SB_PARAM_GET_STATE, 3 * 8, bytes);

		n = 8;
		err = err * 1000 + e2;
	}
	mutex_unlock(&live_lock);
	if (n > 9)
		n = 9;
	tfa98xx_convert_bytes2data(3 * n, bytes, data);
	return sysfs_emit(buf, "err=%d n=%u agc=%d lim=%d smax=%d T=%d flag=0x%x X1=%d X2=%d Re=%d mips=%d\n", err, n,
			  data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8]);
}
static DEVICE_ATTR_RO(live);

/* "rpc": write "MODULE PARAM NWORDS" (a DSP get: module_id + 128), read the answer as 24-bit words. Reads only. */
static int rpc_mod, rpc_par, rpc_n;
static ssize_t rpc_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	int m, p, n;

	if (sscanf(buf, "%i %i %i", &m, &p, &n) != 3 || m < 0 || m > 15 || p < 0x80 || p > 255 || n < 1 || n > 32)	/* gets only */
		return -EINVAL;
	rpc_mod = m; rpc_par = p; rpc_n = n;
	return len;
}
static ssize_t rpc_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tfa98xx_bach2 *t = dev_get_drvdata(dev);
	unsigned char bytes[3 * 32] = { 0 };
	int data[32] = { 0 }, err, i, o;

	if (!t || !t->started || !rpc_n)
		return sysfs_emit(buf, "not started\n");
	mutex_lock(&live_lock);
	err = tfa_dsp_cmd_id_write_read(&t->tfa, rpc_mod, rpc_par, 3 * rpc_n, bytes);
	mutex_unlock(&live_lock);
	tfa98xx_convert_bytes2data(3 * rpc_n, bytes, data);
	o = sysfs_emit(buf, "m=%d p=0x%x err=%d:", rpc_mod, rpc_par, err);
	for (i = 0; i < rpc_n; i++)
		o += sysfs_emit_at(buf, o, " %d", data[i]);
	return o + sysfs_emit_at(buf, o, "\n");
}
static DEVICE_ATTR_RW(rpc);
/* "status": STATUS0..3 (0x10-0x13) by plain reads. 0x12 holds CLIPS (bit 4), LP1 (bit 7: low-power, no audio) and
 * LA (bit 8: low amplitude); 0x13 bits 0-2 are TDMSTAT. Tone vs silence shows whether audio reaches the amp (A9). */
static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tfa98xx_bach2 *t = dev_get_drvdata(dev);
	unsigned int v[4] = { 0 }, d[7] = { 0 }, r;
	int e = 0;

	if (!t)
		return -ENODEV;
	mutex_lock(&live_lock);
	for (r = 0; r < 4; r++)
		e |= regmap_read(t->regmap, 0x10 + r, &v[r]);
	for (r = 0; r < 7; r++)	/* TDM config 0x20-0x26 (A11: why 0x35/0x36 see audio and 0x34/0x37 not) */
		e |= regmap_read(t->regmap, 0x20 + r, &d[r]);
	mutex_unlock(&live_lock);
	return sysfs_emit(buf, "e=%d s0=%04x s1=%04x s2=%04x s3=%04x CLIPS=%u LP1=%u LA=%u TDMSTAT=%u tdm=%04x,%04x,%04x,%04x,%04x,%04x,%04x\n",
			  e, v[0], v[1], v[2], v[3], (v[2] >> 4) & 1, (v[2] >> 7) & 1, (v[2] >> 8) & 1, v[3] & 7,
			  d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
}
static DEVICE_ATTR_RO(status);
static struct attribute *tfa98xx_bach2_attrs[] = { &dev_attr_live.attr, &dev_attr_rpc.attr, &dev_attr_status.attr, NULL };
ATTRIBUTE_GROUPS(tfa98xx_bach2);

static const struct of_device_id tfa98xx_bach2_of_match[] = {
	{ .compatible = "nxp,tfa9894" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa98xx_bach2_of_match);

static struct i2c_driver tfa98xx_bach2_driver = {
	.driver = {
		.name = "tfa98xx-bach2",
		.of_match_table = tfa98xx_bach2_of_match,
		.dev_groups = tfa98xx_bach2_groups,	/* removed before remove() runs */
	},
	.probe = tfa98xx_bach2_probe,
	.remove = tfa98xx_bach2_remove,
};

static int __init tfa98xx_bach2_init(void)
{
	int ret;

	if (stage < 0 || stage > 3) {
		pr_err(LOGP "stage %d not supported (0..3)\n", stage);
		return -EINVAL;
	}
	tfa_cache = kmem_cache_create("tfa98xx_bach2", PAGE_SIZE, 0, SLAB_HWCACHE_ALIGN, NULL);
	if (!tfa_cache)
		return -ENOMEM;
	pr_info(LOGP "init stage %d fw %s\n", stage, fw_name);
	ret = i2c_add_driver(&tfa98xx_bach2_driver);
	if (ret)
		kmem_cache_destroy(tfa_cache);
	return ret;
}
module_init(tfa98xx_bach2_init);

static void __exit tfa98xx_bach2_exit(void)
{
	i2c_del_driver(&tfa98xx_bach2_driver);
	kmem_cache_destroy(tfa_cache);
}
module_exit(tfa98xx_bach2_exit);

MODULE_DESCRIPTION("bach2 TFA9894 bring-up glue for the NXP TFA98xx core");
MODULE_LICENSE("GPL");
