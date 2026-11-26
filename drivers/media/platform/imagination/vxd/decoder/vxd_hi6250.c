// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon Hi6250 (Kirin 659) power/clock glue for the PVDEC (D5500) video decoder.
 *
 * The stock kernel powers the decoder through two "hi3xxx" regulator step lists (vendor
 * drivers/regulator/hisi_kirin950_regulator.c, live stock DT regulator_hi3xxx@0xFFF35000
 * hi3xxx@2 "vcodecsubsys" and hi3xxx@3 "vdec"), framed by the freq-autodown handling of
 * drivers/mfd/hisi_freq_autodown.c. None of these clocks, resets or power switches is
 * modelled by clk-hi6250 or a power-domain driver yet, so the sequence lives here, replayed
 * bit for bit as verified on this board by the vdec.py V2 cycle (docs/mainline-vdec.md):
 *
 *   vcodecsubsys on: autodown off, clk_vcodecbus (+ its andgt), clk_vcodeccfg, clk_atdiv_vcbus,
 *                    reset release, NOC exit idle, autodown on
 *   vdec on:         autodown off, MTCMOS on (+100 us), re-clock pulse, ISO off, reset release,
 *                    clk_vdec (+ its andgt), NOC exit idle, autodown on
 *   off:             the stock disable lists in reverse; a NOC port that does not go idle keeps
 *                    its domain powered (no power cut under traffic)
 *
 * Clock rates are the stock ones (vdec 360 MHz = PPLL0 1440 / 4, vcodecbus 400 MHz = PPLL3
 * 1600 / 4): both dividers are programmed before the gates open. Mainline boots with the vdec
 * divider at PPLL3 / 4 = 400 MHz, above stock. No voltage is involved (MTCMOS is an on-die
 * switch). The parents (vivobus/dsssubsys, LPM3-switched) are kept up by the display.
 *
 * Writes go only to the CRG set/clear registers' vdec/vcodec bits, the two hiword dividers and
 * andgt gates, the two freq-autodown bypass bits and clocks, and the PMC NOC idle request bits.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "vxd_hi6250.h"

/* CRG (crg_ctrl@fff35000) */
#define CRG_PEREN0		0x000
#define CRG_PERDIS0		0x004
#define CRG_PERCLKEN0		0x008
#define CRG_PEREN3		0x030
#define CRG_PERDIS3		0x034
#define CRG_PERCLKEN3		0x038
#define CRG_PERRSTEN0		0x060
#define CRG_PERRSTDIS0		0x064
#define CRG_PERRSTSTAT0		0x068
#define CRG_PERRSTEN3		0x084
#define CRG_PERRSTDIS3		0x088
#define CRG_PERRSTSTAT3		0x08c
#define CRG_DIV_VDEC		0x0cc	/* hiword: mux [6:5], div [4:0] */
#define CRG_DIV_VCBUS		0x0d0
#define CRG_GT_VDEC		0x0f0	/* hiword andgt */
#define CRG_GT_VCBUS		0x0f8
#define CRG_ISOEN		0x144
#define CRG_ISODIS		0x148
#define CRG_ISOSTAT		0x14c
#define CRG_PWREN		0x150
#define CRG_PWRDIS		0x154
#define CRG_PWRACK		0x15c
#define CRG_BYP_VDEC		0x36c	/* freq-autodown bypass, bit 0 */
#define CRG_BYP_VCBUS		0x374
#define CRG_AD_EN		0x410	/* freq-autodown clocks */
#define CRG_AD_DIS		0x414
#define CRG_AD_STAT		0x418

#define B_VCCFG			BIT(5)	/* PEREN0: clk_vcodeccfg */
#define B_VCBUS			BIT(6)	/* PEREN0: clk_vcodecbus */
#define B_VDEC_CLK		BIT(11)	/* PEREN3: clk_vdec */
#define B_VC_RST		0x18	/* PERRSTEN0: vcodecsubsys */
#define B_VDEC_RST		BIT(9)	/* PERRSTEN3: vdec */
#define B_VDEC_PD		BIT(2)	/* ISO/MTCMOS: vdec */
#define B_AD_VCBUS		BIT(5)	/* 0x410: clk_atdiv_vcbus = vcodecbus autodown clock */
#define B_AD_VDEC		BIT(3)	/* 0x410: vdecbus autodown clock */
#define GT_VDEC_BIT		15
#define GT_VCBUS_BIT		2

/* stock rates: (mux, div field) */
#define DIV_VDEC_STOCK		((0 << 5) | 3)	/* PPLL0 1440 MHz / 4 = 360 MHz */
#define DIV_VCBUS_STOCK		((3 << 5) | 3)	/* PPLL3 1600 MHz / 4 = 400 MHz */
#define DIV_FIELD_MASK		0x7f

/* PMC (pmctrl@fff31000): NOC power idle */
#define PMC_NOC_REQ		0x380	/* hiword */
#define PMC_NOC_ACK		0x384
#define PMC_NOC_IDLE		0x388
#define NOC_VC			BIT(4)
#define NOC_VDEC		BIT(10)
#define NOC_POLL_US		1000	/* the stock driver polls without a bound; V2 saw < 1 ms */

struct vxd_hi6250 {
	struct device *dev;
	struct regmap *crg;
	struct regmap *pmc;
	struct mutex lock;	/* serialises power on/off */
	bool own_vc;		/* vcodecsubsys was down at probe: we power it */
	bool gt_vcbus_boot;	/* clk_vcodecbus andgt was open at probe: leave it open */
	u32 byp_boot;		/* bit 0: vdec bypass, bit 1: vcbus bypass */
	u32 ad_boot;		/* AD_STAT & (B_AD_VDEC | B_AD_VCBUS) at probe */
	bool on;
	bool stuck;		/* a NOC port refused idle: domain left powered */
	unsigned int cycles;
};

static u32 crg_rd(struct vxd_hi6250 *hp, unsigned int off)
{
	unsigned int v = 0;

	regmap_read(hp->crg, off, &v);
	return v;
}

static void crg_wr(struct vxd_hi6250 *hp, unsigned int off, u32 v)
{
	regmap_write(hp->crg, off, v);
}

static u32 pmc_rd(struct vxd_hi6250 *hp, unsigned int off)
{
	unsigned int v = 0;

	regmap_read(hp->pmc, off, &v);
	return v;
}

struct hi6250_state {
	u32 clken0, clken3, rst0, rst3, gt_vdec, gt_vcbus, iso, pwrack, ad, noc_ack, noc_idle;
	u32 div_vdec, div_vcbus;
	bool vc_up, vd_up, vd_off;
};

static void hi6250_read_state(struct vxd_hi6250 *hp, struct hi6250_state *s)
{
	bool vc_clk, vc_noc, vd_clk, vd_noc;

	s->clken0 = crg_rd(hp, CRG_PERCLKEN0);
	s->clken3 = crg_rd(hp, CRG_PERCLKEN3);
	s->rst0 = crg_rd(hp, CRG_PERRSTSTAT0);
	s->rst3 = crg_rd(hp, CRG_PERRSTSTAT3);
	s->gt_vdec = crg_rd(hp, CRG_GT_VDEC);
	s->gt_vcbus = crg_rd(hp, CRG_GT_VCBUS);
	s->iso = crg_rd(hp, CRG_ISOSTAT);
	s->pwrack = crg_rd(hp, CRG_PWRACK);
	s->ad = crg_rd(hp, CRG_AD_STAT);
	s->div_vdec = crg_rd(hp, CRG_DIV_VDEC);
	s->div_vcbus = crg_rd(hp, CRG_DIV_VCBUS);
	s->noc_ack = pmc_rd(hp, PMC_NOC_ACK);
	s->noc_idle = pmc_rd(hp, PMC_NOC_IDLE);

	vc_clk = (s->clken0 & B_VCBUS) && (s->clken0 & B_VCCFG) && (s->ad & B_AD_VCBUS) &&
		 (s->gt_vcbus & BIT(GT_VCBUS_BIT));
	vc_noc = (s->noc_ack & NOC_VC) || (s->noc_idle & NOC_VC);
	vd_clk = (s->clken3 & B_VDEC_CLK) && (s->gt_vdec & BIT(GT_VDEC_BIT));
	vd_noc = (s->noc_ack & NOC_VDEC) || (s->noc_idle & NOC_VDEC);

	s->vc_up = vc_clk && !(s->rst0 & B_VC_RST) && !vc_noc;
	s->vd_up = s->vc_up && (s->pwrack & B_VDEC_PD) && !(s->iso & B_VDEC_PD) &&
		   !(s->rst3 & B_VDEC_RST) && vd_clk && !vd_noc;
	s->vd_off = !(s->pwrack & B_VDEC_PD) && (s->iso & B_VDEC_PD) && (s->rst3 & B_VDEC_RST);
}

static void hi6250_log_state(struct vxd_hi6250 *hp, const char *tag)
{
	struct hi6250_state s;

	hi6250_read_state(hp, &s);
	dev_info(hp->dev,
		 "hi6250 %s: vc_up=%d vd_up=%d vd_off=%d clken0=%08x clken3=%08x rst0=%08x rst3=%08x iso=%08x pwrack=%08x ad=%08x noc_ack=%08x noc_idle=%08x div_vdec=%02x div_vcbus=%02x\n",
		 tag, s.vc_up, s.vd_up, s.vd_off, s.clken0, s.clken3, s.rst0, s.rst3, s.iso,
		 s.pwrack, s.ad, s.noc_ack, s.noc_idle, s.div_vdec & DIV_FIELD_MASK,
		 s.div_vcbus & DIV_FIELD_MASK);
}

bool vxd_hi6250_is_up(struct vxd_hi6250 *hp)
{
	struct hi6250_state s;

	hi6250_read_state(hp, &s);
	return s.vd_up;
}

static int hi6250_noc(struct vxd_hi6250 *hp, u32 bit, bool enter_idle, const char *what)
{
	unsigned int i;
	u32 ack = 0, idle = 0;

	regmap_write(hp->pmc, PMC_NOC_REQ, (bit << 16) | (enter_idle ? bit : 0));
	for (i = 0; i < NOC_POLL_US; i++) {
		ack = pmc_rd(hp, PMC_NOC_ACK);
		idle = pmc_rd(hp, PMC_NOC_IDLE);
		if (enter_idle ? ((ack & bit) && (idle & bit)) : (!(ack & bit) && !(idle & bit)))
			return 0;
		udelay(1);
	}
	dev_err(hp->dev, "hi6250: NOC %s %s idle timed out (ack=%08x idle=%08x)\n",
		what, enter_idle ? "enter" : "exit", ack, idle);
	return -ETIMEDOUT;
}

/* hisi_freq_autodown_clk_set: off = gate the autodown clock, then bypass; on = the reverse */
static void hi6250_autodown(struct vxd_hi6250 *hp, unsigned int byp, u32 bit, bool enable)
{
	if (enable) {
		regmap_update_bits(hp->crg, byp, 1, 0);
		crg_wr(hp, CRG_AD_EN, bit);
	} else {
		crg_wr(hp, CRG_AD_DIS, bit);
		regmap_update_bits(hp->crg, byp, 1, 1);
	}
}

static void hi6250_vdec_clk(struct vxd_hi6250 *hp, bool on)
{
	if (on) {
		crg_wr(hp, CRG_GT_VDEC, BIT(GT_VDEC_BIT + 16) | BIT(GT_VDEC_BIT));
		crg_wr(hp, CRG_PEREN3, B_VDEC_CLK);
	} else {
		crg_wr(hp, CRG_PERDIS3, B_VDEC_CLK);
		crg_wr(hp, CRG_GT_VDEC, BIT(GT_VDEC_BIT + 16));
	}
}

static void hi6250_set_div(struct vxd_hi6250 *hp, unsigned int off, u32 field)
{
	if ((crg_rd(hp, off) & DIV_FIELD_MASK) != field)
		crg_wr(hp, off, (DIV_FIELD_MASK << 16) | field);
}

/* freq-autodown bypass bits and clocks back to their values at probe */
static void hi6250_restore_autodown(struct vxd_hi6250 *hp)
{
	u32 ad = crg_rd(hp, CRG_AD_STAT);

	regmap_update_bits(hp->crg, CRG_BYP_VDEC, 1, hp->byp_boot & 1);
	regmap_update_bits(hp->crg, CRG_BYP_VCBUS, 1, (hp->byp_boot >> 1) & 1);
	if ((ad ^ hp->ad_boot) & B_AD_VDEC)
		crg_wr(hp, (hp->ad_boot & B_AD_VDEC) ? CRG_AD_EN : CRG_AD_DIS, B_AD_VDEC);
	if (hp->own_vc && ((ad ^ hp->ad_boot) & B_AD_VCBUS))
		crg_wr(hp, (hp->ad_boot & B_AD_VCBUS) ? CRG_AD_EN : CRG_AD_DIS, B_AD_VCBUS);
}

static int hi6250_vc_off(struct vxd_hi6250 *hp)
{
	/* vcodecsubsys disable steps {NOCIDLE, SOFTRESET, CLOCK} */
	hi6250_autodown(hp, CRG_BYP_VCBUS, B_AD_VCBUS, false);
	if (hi6250_noc(hp, NOC_VC, true, "vcodec")) {
		hp->stuck = true;
		return -ETIMEDOUT;
	}
	crg_wr(hp, CRG_PERRSTEN0, B_VC_RST);
	crg_wr(hp, CRG_PERDIS0, B_VCCFG);
	crg_wr(hp, CRG_PERDIS0, B_VCBUS);
	if (!hp->gt_vcbus_boot)
		crg_wr(hp, CRG_GT_VCBUS, BIT(GT_VCBUS_BIT + 16));
	return 0;
}

static int hi6250_vd_off(struct vxd_hi6250 *hp)
{
	/* vdec disable steps {NOCIDLE, SOFTRESET, CLOCK, ISO, MTCMOS} */
	hi6250_autodown(hp, CRG_BYP_VDEC, B_AD_VDEC, false);
	if (hi6250_noc(hp, NOC_VDEC, true, "vdec")) {
		hp->stuck = true;
		return -ETIMEDOUT;
	}
	crg_wr(hp, CRG_PERRSTEN3, B_VDEC_RST);
	hi6250_vdec_clk(hp, false);
	crg_wr(hp, CRG_ISOEN, B_VDEC_PD);
	crg_wr(hp, CRG_PWRDIS, B_VDEC_PD);
	return 0;
}

int vxd_hi6250_power_on(struct vxd_hi6250 *hp)
{
	int ret = 0;

	mutex_lock(&hp->lock);
	if (hp->on)
		goto out;
	if (hp->stuck) {
		/* a previous power-down left the domain on: do not stack another sequence */
		ret = -EIO;
		goto out;
	}

	hi6250_set_div(hp, CRG_DIV_VCBUS, DIV_VCBUS_STOCK);
	hi6250_set_div(hp, CRG_DIV_VDEC, DIV_VDEC_STOCK);

	if (hp->own_vc) {
		/* vcodecsubsys (hi3xxx@2): {CLOCK, SOFTRESET, NOCIDLE} inside autodown off/on */
		hi6250_autodown(hp, CRG_BYP_VCBUS, B_AD_VCBUS, false);
		crg_wr(hp, CRG_GT_VCBUS, BIT(GT_VCBUS_BIT + 16) | BIT(GT_VCBUS_BIT));
		crg_wr(hp, CRG_PEREN0, B_VCBUS);
		crg_wr(hp, CRG_PEREN0, B_VCCFG);
		crg_wr(hp, CRG_AD_EN, B_AD_VCBUS);
		crg_wr(hp, CRG_PERRSTDIS0, B_VC_RST);
		ret = hi6250_noc(hp, NOC_VC, false, "vcodec");
		if (ret) {
			hi6250_vc_off(hp);
			hi6250_restore_autodown(hp);
			goto out;
		}
		hi6250_autodown(hp, CRG_BYP_VCBUS, B_AD_VCBUS, true);
	}

	/* vdec (hi3xxx@3): {MTCMOS +100us, RECLOCK, ISO, SOFTRESET, CLOCK, NOCIDLE} */
	hi6250_autodown(hp, CRG_BYP_VDEC, B_AD_VDEC, false);
	crg_wr(hp, CRG_PWREN, B_VDEC_PD);
	usleep_range(100, 200);
	if (!(crg_rd(hp, CRG_PWRACK) & B_VDEC_PD))
		dev_warn(hp->dev, "hi6250: no MTCMOS ack (pwrack=%08x)\n", crg_rd(hp, CRG_PWRACK));
	hi6250_vdec_clk(hp, true);
	udelay(1);
	hi6250_vdec_clk(hp, false);
	udelay(1);
	crg_wr(hp, CRG_ISODIS, B_VDEC_PD);
	crg_wr(hp, CRG_PERRSTDIS3, B_VDEC_RST);
	hi6250_vdec_clk(hp, true);
	ret = hi6250_noc(hp, NOC_VDEC, false, "vdec");
	if (ret) {
		if (!hi6250_vd_off(hp) && hp->own_vc)
			hi6250_vc_off(hp);
		hi6250_restore_autodown(hp);
		goto out;
	}
	hi6250_autodown(hp, CRG_BYP_VDEC, B_AD_VDEC, true);

	if (!vxd_hi6250_is_up(hp)) {
		hi6250_log_state(hp, "power-on FAILED");
		if (!hi6250_vd_off(hp) && hp->own_vc)
			hi6250_vc_off(hp);
		hi6250_restore_autodown(hp);
		ret = -EIO;
		goto out;
	}
	hp->on = true;
	hp->cycles++;
	if (hp->cycles <= 8)
		hi6250_log_state(hp, "power-on");
out:
	mutex_unlock(&hp->lock);
	return ret;
}

int vxd_hi6250_power_off(struct vxd_hi6250 *hp)
{
	int ret = 0;

	mutex_lock(&hp->lock);
	if (!hp->on)
		goto out;
	ret = hi6250_vd_off(hp);
	if (ret) {
		dev_err(hp->dev, "hi6250: vdec left powered (NOC not idle)\n");
		goto out;	/* hp->on stays true: the parent stays up too */
	}
	if (hp->own_vc) {
		ret = hi6250_vc_off(hp);
		if (ret)
			dev_err(hp->dev, "hi6250: vcodecsubsys left powered (NOC not idle)\n");
	}
	hi6250_restore_autodown(hp);
	hp->on = false;
	if (hp->cycles <= 8)
		hi6250_log_state(hp, "power-off");
out:
	mutex_unlock(&hp->lock);
	return ret;
}

struct vxd_hi6250 *vxd_hi6250_probe(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct vxd_hi6250 *hp;
	struct hi6250_state s;

	hp = devm_kzalloc(dev, sizeof(*hp), GFP_KERNEL);
	if (!hp)
		return ERR_PTR(-ENOMEM);
	hp->dev = dev;
	mutex_init(&hp->lock);

	hp->crg = syscon_regmap_lookup_by_phandle(np, "hisilicon,crg-syscon");
	if (IS_ERR(hp->crg))
		return dev_err_cast_probe(dev, hp->crg, "no hisilicon,crg-syscon\n");
	hp->pmc = syscon_regmap_lookup_by_phandle(np, "hisilicon,pmctrl-syscon");
	if (IS_ERR(hp->pmc))
		return dev_err_cast_probe(dev, hp->pmc, "no hisilicon,pmctrl-syscon\n");

	hi6250_read_state(hp, &s);
	hi6250_log_state(hp, "boot");
	if (!s.vd_off) {
		/* someone else (bootloader, another driver) owns the domain: do not touch it */
		dev_err(dev, "hi6250: decoder domain not in the expected off state, refusing\n");
		return ERR_PTR(-EBUSY);
	}
	hp->own_vc = !s.vc_up;
	hp->gt_vcbus_boot = !!(s.gt_vcbus & BIT(GT_VCBUS_BIT));
	hp->byp_boot = (crg_rd(hp, CRG_BYP_VDEC) & 1) | ((crg_rd(hp, CRG_BYP_VCBUS) & 1) << 1);
	hp->ad_boot = s.ad & (B_AD_VDEC | B_AD_VCBUS);
	if (!hp->own_vc)
		dev_info(dev, "hi6250: vcodec subsystem already up: not switched by this driver\n");
	return hp;
}
