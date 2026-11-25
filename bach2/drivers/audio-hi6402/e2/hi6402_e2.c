// SPDX-License-Identifier: GPL-2.0
/*
 * hi6402_e2: one-shot audio experiment E2 for bach2 (docs/mainline-audio.md, section 6).
 *
 *   insmod hi6402_e2.ko                 report preconditions only (null run, no writes)
 *   insmod hi6402_e2.ko stages=1        + HI6402 version over the SoC codec SSI master
 *   insmod hi6402_e2.ko stages=2        + ASP power-on request to LPM3 (mailbox 14), then off again
 *   insmod hi6402_e2.ko stages=4|1      + codec supply/clock enable (ldo29 en, pmu audio clk), then SSI
 *   insmod hi6402_e2.ko stages=8        + ASP subsys clock request {0xd0002, 0xb} to LPM3, then off
 *
 * Every write is listed here and in the doc:
 *   stage 1: CRG PEREN2 (0xfff35020) = BIT(26)      clk_codecssi on, only if it was off
 *            SSI 0xe82b9000 + 0x1fd/0x1fe/0x1ff * 4  HI6402 page pointer = 0x200070
 *            CRG PERDIS2 (0xfff35024) = BIT(26)      clk_codecssi off again, only if it was off
 *   stage 2: mailbox 14 message {0x00050001} (stock "asp" regulator enable), then {0x00050101}
 *            (stock disable) unless keep_asp=1. Skipped if SCPWRACK bit 0 is already 1.
 *   stage 8: mailbox 14 message {0x000d0002, 0x0b} (stock clk_asp_subsys "hisilicon,interactive-clk"
 *            ipc-lpm3-cmd-en; stock sends it on mailbox 13, which mainline's stub_clock owns), then
 *            {0x000d0102, 0x0b} unless keep_asp=1. Skipped if SCPERSTAT0 bit 26 is already 1.
 *   stage 4: PMIC 0x30 |= BIT(0) (ldo29 enable, vset untouched), PMIC 0x114 |= BIT(0)
 *            (clk_pmuaudioclk), each only if clear; restored at the end unless keep_codec=1.
 * Anything else is a read. Module load always fails (-EAGAIN on success, a real error otherwise)
 * so nothing stays bound; the result is the E2_RESULT kmsg line.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define TAG "BACH2ML: E2: "

#define SCTRL_BASE	0xfff0a000UL
#define CRG_BASE	0xfff35000UL
#define PMIC_BASE	0xfff34000UL
#define AOIOC_BASE	0xfff11000UL
#define SSI_BASE	0xe82b9000UL	/* SOC_ACPU_CODEC_SSI_BASE_ADDR */
#define GPIO9_BASE	0xe8a14000UL

#define SCISOSTAT	0x048
#define SCPWRACK	0x06c
#define SCPERSTAT0	0x16c
#define CRG_PERCLKEN1	0x018
#define CRG_PERSTAT1	0x01c
#define CRG_PEREN2	0x020
#define CRG_PERDIS2	0x024
#define CRG_PERCLKEN2	0x028
#define CRG_PERSTAT2	0x02c
#define CRG_PERRSTSTAT2	0x080
#define PMIC_REG(r)	((r) << 2)
#define PMIC_LDO29_CTRL	0x30	/* bit 0 enable (stock hisi-ctrl <0x30 0x01 0x10>) */
#define PMIC_LDO29_VSET	0x59	/* bits 2:0 (stock hisi-vset <0x59 0x07>) */
#define PMIC_AUDIOCLK	0x114	/* bit 0 (stock clk_pmuaudioclk hisilicon,clkgate <0x114 0x00>) */
#define CODECSSI_BIT	BIT(26)
#define GPIO076_BIT	BIT(4)	/* GPIO 76 = gpio9 line 4: stock codec gpio_reset */

#define HI6402_VERSION	0x20007000	/* expected 0x11 (HI6402_VERSION_VALUE) */
#define HI6402_CHIPID0	0x20007092

#define ASP_ON_MSG	0x00050001	/* stock asp regulator hi3xxx_to_lpm3_enable_step */
#define ASP_OFF_MSG	0x00050101	/* stock hi3xxx_to_lpm3_disable_step */
#define ASPCLK_ON_MSG	0x000d0002	/* stock clk_asp_subsys ipc-lpm3-cmd-en <0xd0002 0x0b> */
#define ASPCLK_OFF_MSG	0x000d0102
#define ASPCLK_ARG	0x0000000b

static unsigned int stages;
module_param(stages, uint, 0444);
MODULE_PARM_DESC(stages, "bitmask: 1 SSI version read, 2 ASP power request, 4 codec power enable, 8 ASP clock request");
static bool keep_asp;
module_param(keep_asp, bool, 0444);
static bool keep_codec;
module_param(keep_codec, bool, 0444);
static bool force_no_reset_check;
module_param(force_no_reset_check, bool, 0444);
MODULE_PARM_DESC(force_no_reset_check, "run stage 1 even if the GPIO 76 level cannot be read");

struct e2 {
	void __iomem *sctrl, *crg, *pmic, *aoioc, *ssi, *gpio9;
};

static u32 rd(void __iomem *b, u32 off) { return readl(b + off); }

static int map_all(struct device *dev, struct e2 *e)
{
	e->sctrl = devm_ioremap(dev, SCTRL_BASE, 0x1000);
	e->crg = devm_ioremap(dev, CRG_BASE, 0x1000);
	e->pmic = devm_ioremap(dev, PMIC_BASE, 0x1000);
	e->aoioc = devm_ioremap(dev, AOIOC_BASE, 0x1000);
	e->ssi = devm_ioremap(dev, SSI_BASE, 0x800);
	e->gpio9 = devm_ioremap(dev, GPIO9_BASE, 0x1000);
	return (e->sctrl && e->crg && e->pmic && e->aoioc && e->ssi && e->gpio9) ? 0 : -ENOMEM;
}

/* -1 unknown (gpio9 pclk off: do not touch the bank), 0 low, 1 high */
static int codec_reset_level(struct e2 *e)
{
	if (!(rd(e->crg, CRG_PERCLKEN1) & BIT(9)) || !(rd(e->crg, CRG_PERSTAT1) & BIT(9)))
		return -1;
	return !!(rd(e->gpio9, 0x3fc) & GPIO076_BIT);
}

static void report(struct e2 *e, const char *when)
{
	pr_crit(TAG "%s scpwrack=0x%08x sciso=0x%08x scperstat0=0x%08x crg_clken2=0x%08x crg_stat2=0x%08x crg_rststat2=0x%08x ldo29=0x%02x vset=0x%02x audioclk=0x%02x iomg186=0x%x iomg189=0x%x iomg190=0x%x reset76=%d\n",
		when, rd(e->sctrl, SCPWRACK), rd(e->sctrl, SCISOSTAT), rd(e->sctrl, SCPERSTAT0),
		rd(e->crg, CRG_PERCLKEN2), rd(e->crg, CRG_PERSTAT2), rd(e->crg, CRG_PERRSTSTAT2),
		rd(e->pmic, PMIC_REG(PMIC_LDO29_CTRL)), rd(e->pmic, PMIC_REG(PMIC_LDO29_VSET)),
		rd(e->pmic, PMIC_REG(PMIC_AUDIOCLK)), rd(e->aoioc, 0x20), rd(e->aoioc, 0x2c),
		rd(e->aoioc, 0x30), codec_reset_level(e));
}

/* hi_cdc_ssi.c: page = reg[31:8] into codec regs 0x1fd..0x1ff, data at (reg & 0xff) << 2, read twice */
static void ssi_page(struct e2 *e, u32 reg)
{
	u32 p = reg >> 8;

	writel(p & 0xff, e->ssi + (0x1fd << 2));
	writel((p >> 8) & 0xff, e->ssi + (0x1fe << 2));
	writel((p >> 16) & 0xff, e->ssi + (0x1ff << 2));
}

static u32 ssi_read8(struct e2 *e, u32 reg)
{
	readl(e->ssi + ((reg & 0xff) << 2));
	return readl(e->ssi + ((reg & 0xff) << 2));
}

static int stage_ssi(struct e2 *e)
{
	bool was_on = rd(e->crg, CRG_PERCLKEN2) & CODECSSI_BIT;
	u32 ver[2], id[4];
	int lvl = codec_reset_level(e), i, pass;

	if (rd(e->crg, CRG_PERRSTSTAT2) & CODECSSI_BIT) {
		pr_crit(TAG "E2_RESULT stage=ssi FAIL precondition: codecssi in reset (not touched)\n");
		return -EPERM;
	}
	if (!(rd(e->pmic, PMIC_REG(PMIC_LDO29_CTRL)) & BIT(0)) ||
	    !(rd(e->pmic, PMIC_REG(PMIC_AUDIOCLK)) & BIT(0))) {
		pr_crit(TAG "E2_RESULT stage=ssi FAIL precondition: codec unpowered (ldo29/audioclk off; stage 4 enables)\n");
		return -EPERM;
	}
	if (lvl == 0 || (lvl < 0 && !force_no_reset_check)) {
		pr_crit(TAG "E2_RESULT stage=ssi FAIL precondition: codec reset GPIO 76 %s\n",
			lvl == 0 ? "low (held in reset)" : "unreadable (force_no_reset_check=1 to run)");
		return -EPERM;
	}
	if (!was_on) {
		writel(CODECSSI_BIT, e->crg + CRG_PEREN2);
		udelay(10);
	}
	ssi_page(e, HI6402_VERSION);
	ver[0] = ssi_read8(e, HI6402_VERSION);
	for (i = 0; i < 4; i++)
		id[i] = ssi_read8(e, HI6402_CHIPID0 + i);
	ver[1] = ssi_read8(e, HI6402_VERSION);
	if (!was_on)
		writel(CODECSSI_BIT, e->crg + CRG_PERDIS2);
	pass = ver[0] == 0x11 && ver[1] == 0x11;
	pr_crit(TAG "E2_RESULT stage=ssi %s ver=0x%02x/0x%02x chipid=%02x%02x%02x%02x clk_was_on=%d (expect ver 0x11/0x11)\n",
		pass ? "PASS" : "FAIL", ver[0], ver[1], id[0] & 0xff, id[1] & 0xff, id[2] & 0xff,
		id[3] & 0xff, was_on);
	return pass ? 0 : -EIO;
}

static int asp_msg(struct device *dev, u32 word, u32 arg)
{
	struct mbox_client cl = { .dev = dev, .tx_block = false, .knows_txdone = true };
	u32 msg[8] = { word, arg };
	struct mbox_chan *ch;
	int r;

	if (!dev->of_node)
		return -ENODEV;
	ch = mbox_request_channel(&cl, 0);
	if (IS_ERR(ch))
		return PTR_ERR(ch);
	r = mbox_send_message(ch, msg);
	mbox_client_txdone(ch, 0);
	mbox_free_channel(ch);
	return r < 0 ? r : 0;
}

static int wait_bit(struct e2 *e, u32 off, u32 bit, u32 want, unsigned int ms)
{
	unsigned int t;

	for (t = 0; t <= ms; t++) {
		if (!!(rd(e->sctrl, off) & bit) == want)
			return t;
		msleep(1);
	}
	return -ETIMEDOUT;
}

static int stage_asp(struct device *dev, struct e2 *e)
{
	int r, t_on, t_off = 0;

	if (rd(e->sctrl, SCPWRACK) & BIT(0)) {
		pr_crit(TAG "E2_RESULT stage=asp SKIP already powered (scpwrack b0=1): no message sent\n");
		return 0;
	}
	r = asp_msg(dev, ASP_ON_MSG, 0);
	if (r) {
		pr_crit(TAG "E2_RESULT stage=asp FAIL mailbox send 0x%08x: %d\n", ASP_ON_MSG, r);
		return r;
	}
	t_on = wait_bit(e, SCPWRACK, BIT(0), 1, 100);
	report(e, "after-asp-on");
	if (!keep_asp) {
		r = asp_msg(dev, ASP_OFF_MSG, 0);
		t_off = r ? r : wait_bit(e, SCPWRACK, BIT(0), 0, 100);
	}
	pr_crit(TAG "E2_RESULT stage=asp %s on_ms=%d off=%s%d (expect on_ms 0..100, off 0..100)\n",
		t_on >= 0 ? "PASS" : "FAIL", t_on, keep_asp ? "kept/" : "", t_off);
	return t_on >= 0 ? 0 : -ETIMEDOUT;
}

static int stage_aspclk(struct device *dev, struct e2 *e)
{
	int r, t_on, t_off = 0;

	if (rd(e->sctrl, SCPERSTAT0) & BIT(26)) {
		pr_crit(TAG "E2_RESULT stage=aspclk SKIP already on (scperstat0 b26=1): no message sent\n");
		return 0;
	}
	r = asp_msg(dev, ASPCLK_ON_MSG, ASPCLK_ARG);
	if (r) {
		pr_crit(TAG "E2_RESULT stage=aspclk FAIL mailbox send 0x%08x: %d\n", ASPCLK_ON_MSG, r);
		return r;
	}
	t_on = wait_bit(e, SCPERSTAT0, BIT(26), 1, 100);
	report(e, "after-aspclk-on");
	if (!keep_asp) {
		r = asp_msg(dev, ASPCLK_OFF_MSG, ASPCLK_ARG);
		t_off = r ? r : wait_bit(e, SCPERSTAT0, BIT(26), 0, 100);
	}
	pr_crit(TAG "E2_RESULT stage=aspclk %s on_ms=%d off=%s%d (expect on_ms 0..100, off 0..100)\n",
		t_on >= 0 ? "PASS" : "FAIL", t_on, keep_asp ? "kept/" : "", t_off);
	return t_on >= 0 ? 0 : -ETIMEDOUT;
}

/* Returns a mask of the bits this call set, so they can be cleared again. */
static u32 stage_codec_power(struct e2 *e)
{
	u32 vset = rd(e->pmic, PMIC_REG(PMIC_LDO29_VSET)) & 0x7, set = 0, v;

	/* Never write vset; only enable at whatever voltage the bootloader programmed. */
	v = rd(e->pmic, PMIC_REG(PMIC_LDO29_CTRL));
	if (!(v & BIT(0))) {
		writel(v | BIT(0), e->pmic + PMIC_REG(PMIC_LDO29_CTRL));
		set |= 1;
		msleep(1);	/* stock hisi-enable-time-us 120 */
	}
	v = rd(e->pmic, PMIC_REG(PMIC_AUDIOCLK));
	if (!(v & BIT(0))) {
		writel(v | BIT(0), e->pmic + PMIC_REG(PMIC_AUDIOCLK));
		set |= 2;
		mdelay(1);	/* hi_cdc_ctrl.c: mdelay(1) after clk_pmuaudioclk */
	}
	pr_crit(TAG "stage=codec-power set=0x%x vset=%u ldo29=0x%02x audioclk=0x%02x\n", set, vset,
		rd(e->pmic, PMIC_REG(PMIC_LDO29_CTRL)), rd(e->pmic, PMIC_REG(PMIC_AUDIOCLK)));
	return set;
}

static void codec_power_restore(struct e2 *e, u32 set)
{
	if (set & 2)
		writel(rd(e->pmic, PMIC_REG(PMIC_AUDIOCLK)) & ~BIT(0), e->pmic + PMIC_REG(PMIC_AUDIOCLK));
	if (set & 1)
		writel(rd(e->pmic, PMIC_REG(PMIC_LDO29_CTRL)) & ~BIT(0), e->pmic + PMIC_REG(PMIC_LDO29_CTRL));
}

static int e2_probe(struct platform_device *pdev)
{
	struct e2 e;
	u32 set = 0;
	int r = 0, r1;

	if (stages & ~15u) {
		pr_crit(TAG "E2_RESULT FAIL unknown stages=0x%x\n", stages);
		return -EINVAL;
	}
	if (map_all(&pdev->dev, &e)) {
		pr_crit(TAG "E2_RESULT FAIL ioremap\n");
		return -ENOMEM;
	}
	pr_crit(TAG "start stages=0x%x keep_asp=%d keep_codec=%d\n", stages, keep_asp, keep_codec);
	report(&e, "before");
	if (stages & 2)
		r = stage_asp(&pdev->dev, &e);
	if (stages & 8) {
		r1 = stage_aspclk(&pdev->dev, &e);
		r = r ? r : r1;
	}
	if (stages & 4)
		set = stage_codec_power(&e);
	if (stages & 1) {
		r1 = stage_ssi(&e);
		r = r ? r : r1;
	}
	if (set && !keep_codec)
		codec_power_restore(&e, set);
	report(&e, "after");
	pr_crit(TAG "E2_DONE stages=0x%x rc=%d\n", stages, r);
	return r ? r : -EAGAIN;	/* never stay bound */
}

static const struct of_device_id e2_of_match[] = {
	{ .compatible = "bach2,audio-e2" },
	{ }
};
MODULE_DEVICE_TABLE(of, e2_of_match);

static struct platform_driver e2_driver = {
	.probe = e2_probe,
	.driver = { .name = "hi6402-e2", .of_match_table = e2_of_match },
};
/*
 * Without the 0050 DT node (not in READY) a plain platform device is registered instead, so the
 * register-only stages (1, 4) still run; the mailbox stages (2, 8) then fail loudly (no "mboxes").
 */
static struct platform_device *e2_pdev;

static int __init e2_init(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "bach2,audio-e2");
	int r = platform_driver_register(&e2_driver);

	if (r)
		return r;
	if (np) {
		of_node_put(np);
		pr_crit(TAG "using DT node bach2,audio-e2 (mailbox stages available)\n");
		return 0;
	}
	pr_crit(TAG "no bach2,audio-e2 DT node: plain device, mailbox stages unavailable\n");
	e2_pdev = platform_device_register_simple("hi6402-e2", -1, NULL, 0);
	if (IS_ERR(e2_pdev)) {
		platform_driver_unregister(&e2_driver);
		return PTR_ERR(e2_pdev);
	}
	return 0;
}
module_init(e2_init);

static void __exit e2_exit(void)
{
	if (!IS_ERR_OR_NULL(e2_pdev))
		platform_device_unregister(e2_pdev);
	platform_driver_unregister(&e2_driver);
}
module_exit(e2_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("bach2 audio experiment E2: ASP power request and HI6402 SSI version read");
