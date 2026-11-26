// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon Hi6250 (Kirin 659) power domains switched by the LPM3 firmware.
 *
 * The vendor kernel models these as "hi3xxx-regulator-type2" regulators: enable and disable send
 * a two-word message to LPM3 on IPC mailbox 14 (hisi_kirin950_regulator.c,
 * RPROC_SYNC_SEND(HISI_RPROC_LPM3_MBX14, ...)); LPM3 does the power switch, isolation, resets,
 * clocks and the supply voltage. The words are the stock ones from the live DT
 * (hi3xxx_to_lpm3_enable_step / _disable_step); nothing here writes a voltage, a clock or a reset.
 *
 * hi3660-mbox does not wait for LPM3's acknowledge of a message (only for the previous one at the
 * next send), so completion is read back from the CRG: the domain counts as on only when its
 * isolation is released, its resets are deasserted and its clocks run. A domain that does not
 * reach that state fails power-on, so a consumer never touches a dead bus.
 *
 * Mailbox 14 is exclusive (mailbox framework): this driver holds it and lends it to the other LPM3
 * users of that mailbox (the ASP audio power and clock votes of bach2_asp), which send their own
 * stock words through hi6250_lpm3_send() (include/linux/soc/hisilicon/hi6250-lpm3.h) instead of
 * requesting the channel. One mutex orders all messages; each one is handled exactly as if its
 * sender owned the channel (same channel arguments, same send + txdone sequence).
 * Built in only: genpd domains cannot be torn down while consumers may still be attached, and the
 * shared channel must outlive every borrower.
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/soc/hisilicon/hi6250-lpm3.h>

#include <dt-bindings/power/hisilicon,hi6250-lpm3-pd.h>

/* CRG (0xfff35000) status registers, vendor soc_crgperiph_interface.h (hi6250) */
#define CRG_PERSTAT3		0x03c	/* b3 gt_pclk_g3d, b4 gt_clk_g3dmt, b5 gt_clk_g3d */
#define CRG_PERRSTSTAT3		0x08c	/* b2 ip_prst_g3d, b3 ip_rst_g3d, b4 ip_rst_g3dmt */
#define CRG_ISOSTAT		0x14c	/* b5 g3disoen */
#define CRG_PERPWRACK		0x15c	/* b20-25 g3dshpwrack0-5, b26 g3dcg0en (logged only) */

#define PD_POLL_MS		100

struct hi6250_lpm3_pd_desc {
	const char *name;
	u32 on;			/* LPM3 words: { on, arg } / { off, arg } */
	u32 off;
	u32 arg;
	u32 iso_mask;		/* CRG_ISOSTAT bits: set = isolated */
	u32 rst_mask;		/* CRG_PERRSTSTAT3 bits: set = in reset */
	u32 clk_mask;		/* CRG_PERSTAT3 bits: set = running */
};

static const struct hi6250_lpm3_pd_desc hi6250_lpm3_pds[] = {
	/* stock live DT regulator_hi3xxx@0xFFF35000/hi3xxx@14 "g3d", the gpu-supply of mali@E8970000 */
	[HI6250_LPM3_PD_G3D] = {
		.name = "g3d",
		.on = 0x00030004, .off = 0x00030104, .arg = 0,
		.iso_mask = BIT(5),
		.rst_mask = GENMASK(4, 2),
		.clk_mask = GENMASK(5, 3),
	},
};

struct hi6250_lpm3_pd {
	struct generic_pm_domain genpd;
	const struct hi6250_lpm3_pd_desc *desc;
	struct hi6250_lpm3 *lpm3;
	/*
	 * Persistent message buffers: the mailbox core queues the pointer, and a send that failed
	 * (previous message never acknowledged) can be retried later; a stack buffer would then be
	 * resent as garbage.
	 */
	u32 msg_on[8];
	u32 msg_off[8];
};

struct hi6250_lpm3 {
	struct device *dev;
	struct regmap *crg;
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct mutex lock;	/* one message on the channel at a time */
	struct hi6250_lpm3_pd pds[ARRAY_SIZE(hi6250_lpm3_pds)];
	struct generic_pm_domain *domains[ARRAY_SIZE(hi6250_lpm3_pds)];
	struct genpd_onecell_data onecell;
};

int hi6250_lpm3_send(struct hi6250_lpm3 *l, u32 *msg)
{
	int ret;

	mutex_lock(&l->lock);
	ret = mbox_send_message(l->chan, msg);
	mbox_client_txdone(l->chan, 0);
	mutex_unlock(&l->lock);
	dev_dbg(l->dev, "lpm3 {0x%08x, 0x%x}: %d\n", msg[0], msg[1], ret < 0 ? ret : 0);
	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(hi6250_lpm3_send);

struct hi6250_lpm3 *hi6250_lpm3_get(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct hi6250_lpm3 *l;

	np = of_parse_phandle(dev->of_node, "hisilicon,lpm3", 0);
	if (!np)
		return ERR_PTR(-ENOENT);
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	/* Set only after a complete probe; the provider never unbinds (built in, no bind attrs). */
	l = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	return l ? l : ERR_PTR(-EPROBE_DEFER);
}
EXPORT_SYMBOL_GPL(hi6250_lpm3_get);

struct hi6250_lpm3_state {
	u32 iso, rst, clk, pwrack;
};

static int hi6250_lpm3_read_state(struct hi6250_lpm3 *l, struct hi6250_lpm3_state *s)
{
	int ret;

	ret = regmap_read(l->crg, CRG_ISOSTAT, &s->iso);
	if (!ret)
		ret = regmap_read(l->crg, CRG_PERRSTSTAT3, &s->rst);
	if (!ret)
		ret = regmap_read(l->crg, CRG_PERSTAT3, &s->clk);
	if (!ret)
		ret = regmap_read(l->crg, CRG_PERPWRACK, &s->pwrack);
	return ret;
}

static bool hi6250_lpm3_pd_open(const struct hi6250_lpm3_pd_desc *d,
				const struct hi6250_lpm3_state *s)
{
	return !(s->iso & d->iso_mask) && !(s->rst & d->rst_mask) &&
	       (s->clk & d->clk_mask) == d->clk_mask;
}

static bool hi6250_lpm3_pd_closed(const struct hi6250_lpm3_pd_desc *d,
				  const struct hi6250_lpm3_state *s)
{
	return (s->iso & d->iso_mask) == d->iso_mask;
}

static int hi6250_lpm3_pd_wait(struct hi6250_lpm3_pd *pd, bool on, struct hi6250_lpm3_state *s)
{
	const struct hi6250_lpm3_pd_desc *d = pd->desc;
	int t, ret;

	for (t = 0; t <= PD_POLL_MS; t++) {
		ret = hi6250_lpm3_read_state(pd->lpm3, s);
		if (ret)
			return ret;
		if (on ? hi6250_lpm3_pd_open(d, s) : hi6250_lpm3_pd_closed(d, s))
			return 0;
		usleep_range(1000, 1200);
	}
	return -ETIMEDOUT;
}

static int hi6250_lpm3_pd_power_on(struct generic_pm_domain *genpd)
{
	struct hi6250_lpm3_pd *pd = container_of(genpd, struct hi6250_lpm3_pd, genpd);
	struct device *dev = pd->lpm3->dev;
	struct hi6250_lpm3_state s = { };
	int ret;

	ret = hi6250_lpm3_send(pd->lpm3, pd->msg_on);
	if (!ret)
		ret = hi6250_lpm3_pd_wait(pd, true, &s);
	if (ret) {
		/* No consumer register access follows a failed power-on. */
		dev_err(dev, "%s: power on failed (%d): iso=0x%08x rst3=0x%08x clk3=0x%08x pwrack=0x%08x\n",
			genpd->name, ret, s.iso, s.rst, s.clk, s.pwrack);
		return ret;
	}
	dev_dbg(dev, "%s: on: iso=0x%08x rst3=0x%08x clk3=0x%08x pwrack=0x%08x\n",
		genpd->name, s.iso, s.rst, s.clk, s.pwrack);
	return 0;
}

static int hi6250_lpm3_pd_power_off(struct generic_pm_domain *genpd)
{
	struct hi6250_lpm3_pd *pd = container_of(genpd, struct hi6250_lpm3_pd, genpd);
	struct device *dev = pd->lpm3->dev;
	struct hi6250_lpm3_state s = { };
	int ret;

	ret = hi6250_lpm3_send(pd->lpm3, pd->msg_off);
	if (!ret)
		ret = hi6250_lpm3_pd_wait(pd, false, &s);
	if (ret) {
		/* genpd keeps the domain marked on: the next power-on just re-sends the on word. */
		dev_warn(dev, "%s: power off not confirmed (%d): iso=0x%08x rst3=0x%08x clk3=0x%08x\n",
			 genpd->name, ret, s.iso, s.rst, s.clk);
		return ret;
	}
	return 0;
}

static void hi6250_lpm3_free_chan(void *chan)
{
	mbox_free_channel(chan);
}

static int hi6250_lpm3_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct hi6250_lpm3 *l;
	unsigned int i;
	int ret;

	l = devm_kzalloc(dev, sizeof(*l), GFP_KERNEL);
	if (!l)
		return -ENOMEM;
	l->dev = dev;
	mutex_init(&l->lock);

	l->crg = syscon_regmap_lookup_by_phandle(np, "hisilicon,crg-syscon");
	if (IS_ERR(l->crg))
		return dev_err_probe(dev, PTR_ERR(l->crg), "crg syscon\n");

	l->cl.dev = dev;
	l->cl.tx_block = false;
	l->cl.knows_txdone = true;
	l->chan = mbox_request_channel(&l->cl, 0);
	if (IS_ERR(l->chan))
		return dev_err_probe(dev, PTR_ERR(l->chan),
				     "LPM3 mailbox 14 (an old bach2_asp/hi6402_e2 that requests it itself?)\n");
	ret = devm_add_action_or_reset(dev, hi6250_lpm3_free_chan, l->chan);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(hi6250_lpm3_pds); i++) {
		struct hi6250_lpm3_pd *pd = &l->pds[i];
		const struct hi6250_lpm3_pd_desc *d = &hi6250_lpm3_pds[i];
		struct hi6250_lpm3_state s;
		bool off;

		pd->desc = d;
		pd->lpm3 = l;
		pd->msg_on[0] = d->on;
		pd->msg_on[1] = d->arg;
		pd->msg_off[0] = d->off;
		pd->msg_off[1] = d->arg;
		pd->genpd.name = d->name;
		pd->genpd.power_on = hi6250_lpm3_pd_power_on;
		pd->genpd.power_off = hi6250_lpm3_pd_power_off;

		ret = hi6250_lpm3_read_state(l, &s);
		if (ret)
			return ret;
		/* Anything but fully open counts as off, so the first user sends the on word. */
		off = !hi6250_lpm3_pd_open(d, &s);
		dev_info(dev, "%s: initially %s: iso=0x%08x rst3=0x%08x clk3=0x%08x pwrack=0x%08x\n",
			 d->name, off ? "off" : "on", s.iso, s.rst, s.clk, s.pwrack);

		ret = pm_genpd_init(&pd->genpd, NULL, off);
		if (ret)
			return ret;
		l->domains[i] = &pd->genpd;
	}

	l->onecell.domains = l->domains;
	l->onecell.num_domains = ARRAY_SIZE(l->domains);
	ret = of_genpd_add_provider_onecell(np, &l->onecell);
	if (ret)
		return ret;
	/* From here on hi6250_lpm3_get() hands the channel to the other mailbox-14 users. */
	platform_set_drvdata(pdev, l);
	return 0;
}

static const struct of_device_id hi6250_lpm3_pd_of_match[] = {
	{ .compatible = "hisilicon,hi6250-lpm3-pd" },
	{ }
};

static struct platform_driver hi6250_lpm3_pd_driver = {
	.probe = hi6250_lpm3_pd_probe,
	.driver = {
		.name = "hi6250-lpm3-pd",
		.of_match_table = hi6250_lpm3_pd_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(hi6250_lpm3_pd_driver);
