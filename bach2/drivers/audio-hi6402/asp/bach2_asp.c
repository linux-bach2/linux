// SPDX-License-Identifier: GPL-2.0
/*
 * bach2_asp: the ASP (audio subsystem) power and clock providers the vendor SLIMbus/HI6402 code
 * expects, on Linux 7.0 (docs/mainline-audio.md section 7).
 *
 *   regulator "asp"          stock hi3xxx-regulator-type2 "asp": LPM3 {0x00050001} / {0x00050101}
 *                            on mailbox 14, state = SCTRL SCPWRACK bit 0 (audio_mtcmos_ack)
 *   clock 0 clk_pmuaudioclk  PMIC 0x114 bit 0 (stock "hisilicon,clk-gate" <0x114 0x00>)
 *   clock 1 clk_asp_subsys   stock "hisilicon,interactive-clk": LPM3 {0x000d0002, 0xb} /
 *                            {0x000d0102, 0xb}; state = SCTRL SCPERSTAT0 bit 26. Stock sends these
 *                            on mailbox 13 (owned here by stub_clock); mailbox 14 is unproven (E2-3).
 *
 * The messages are the stock words; nothing here writes a voltage. The mailbox channel is held for
 * the module's lifetime: C3 showed that freeing and re-requesting it fails ("failed to acquire
 * channel": after LPM3's auto-ack the channel is not IDLE until the next send clears the ACK), so
 * hi6402_e2.ko must not be loaded together with this module.
 *
 * The regulator enable only reports success once LPM3 has also released the ASP isolation, bus
 * resets and gates (SCISOSTAT b0 = 0, SCPERRSTSTAT0 b7-10 = 0, SCPERSTAT1 b0-3 = 0xf): the first
 * ASP_CFG access by the SLIMbus driver happens after this enable and must not hit a dead bus.
 */
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define SCTRL_BASE	0xfff0a000UL
#define PMIC_BASE	0xfff34000UL
#define SCISOSTAT	0x048
#define SCPWRACK	0x06c
#define SCPERSTAT1	0x18c
#define SCPERRSTSTAT0	0x208
#define SCPERSTAT0	0x16c
#define PMIC_AUDIOCLK	(0x114 << 2)

#define ASP_ON		0x00050001
#define ASP_OFF		0x00050101
#define ASPCLK_ON	0x000d0002
#define ASPCLK_OFF	0x000d0102
#define ASPCLK_ARG	0x0000000b

struct bach2_asp {
	struct device *dev;
	void __iomem *sctrl, *pmic;
	struct mutex lock;
	struct mbox_client cl;
	struct mbox_chan *ch;
	struct clk_hw aspclk;
	struct clk_hw_onecell_data *clks;
};

/*
 * Persistent buffers: the mailbox core queues the pointer, and hi3660-mbox's send_data can fail
 * silently (ack timeout) and leave it queued for a later retry. A stack buffer would then be resent
 * as garbage; a static one is resent as the same stock command.
 */
static u32 msg_asp_on[8] = { ASP_ON };
static u32 msg_asp_off[8] = { ASP_OFF };
static u32 msg_aspclk_on[8] = { ASPCLK_ON, ASPCLK_ARG };
static u32 msg_aspclk_off[8] = { ASPCLK_OFF, ASPCLK_ARG };

static int lpm3_send(struct bach2_asp *a, u32 *msg)
{
	int r;

	mutex_lock(&a->lock);
	r = mbox_send_message(a->ch, msg);
	mbox_client_txdone(a->ch, 0);
	mutex_unlock(&a->lock);
	dev_info(a->dev, "lpm3 {0x%08x, 0x%x}: %d\n", msg[0], msg[1], r < 0 ? r : 0);
	return r < 0 ? r : 0;
}

static int wait_bit(struct bach2_asp *a, u32 off, u32 bit, bool want)
{
	int t;

	for (t = 0; t <= 100; t++) {
		if (!!(readl(a->sctrl + off) & bit) == want)
			return 0;
		usleep_range(1000, 1200);
	}
	dev_err(a->dev, "sctrl 0x%x bit 0x%x did not become %d (0x%08x)\n", off, bit, want,
		readl(a->sctrl + off));
	return -ETIMEDOUT;
}

/* regulator "asp" */
static int asp_enable(struct regulator_dev *rdev)
{
	struct bach2_asp *a = rdev_get_drvdata(rdev);
	u32 iso, rst, gates;
	int r = lpm3_send(a, msg_asp_on);

	if (!r)
		r = wait_bit(a, SCPWRACK, BIT(0), true);
	if (r)
		return r;
	iso = readl(a->sctrl + SCISOSTAT) & BIT(0);
	rst = (readl(a->sctrl + SCPERRSTSTAT0) >> 7) & 0xf;
	gates = readl(a->sctrl + SCPERSTAT1) & 0xf;
	dev_info(a->dev, "asp up: scpwrack=0x%08x iso=%u rst=0x%x gates=0x%x scperstat0=0x%08x\n",
		 readl(a->sctrl + SCPWRACK), iso, rst, gates, readl(a->sctrl + SCPERSTAT0));
	if (iso || rst || gates != 0xf || !(readl(a->sctrl + SCPERSTAT0) & BIT(26))) {
		dev_err(a->dev, "asp powered but not usable (iso/reset/gates): refusing; nothing in the ASP was touched\n");
		return -EIO;
	}
	return 0;
}

static int asp_disable(struct regulator_dev *rdev)
{
	struct bach2_asp *a = rdev_get_drvdata(rdev);
	int r = lpm3_send(a, msg_asp_off);

	return r ? r : wait_bit(a, SCPWRACK, BIT(0), false);
}

static int asp_is_enabled(struct regulator_dev *rdev)
{
	struct bach2_asp *a = rdev_get_drvdata(rdev);

	return readl(a->sctrl + SCPWRACK) & BIT(0);
}

static const struct regulator_ops asp_ops = {
	.enable = asp_enable,
	.disable = asp_disable,
	.is_enabled = asp_is_enabled,
};

static const struct regulator_desc asp_desc = {
	.name = "asp",
	.of_match = NULL,
	.ops = &asp_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
};

/* clk_asp_subsys */
static int aspclk_prepare(struct clk_hw *hw)
{
	struct bach2_asp *a = container_of(hw, struct bach2_asp, aspclk);
	int r;

	if (readl(a->sctrl + SCPERSTAT0) & BIT(26))
		return 0;
	r = lpm3_send(a, msg_aspclk_on);
	if (r)
		return r;
	/*
	 * The stock vote; the gate itself may only follow the ASP power-on (C3: the power-on set
	 * SCPERSTAT0 bits 26/27). Not fatal here: the regulator enable checks bit 26 afterwards.
	 */
	if (wait_bit(a, SCPERSTAT0, BIT(26), true))
		dev_info(a->dev, "clk_asp_subsys voted; gate follows the ASP power-on\n");
	return 0;
}

static void aspclk_unprepare(struct clk_hw *hw)
{
	struct bach2_asp *a = container_of(hw, struct bach2_asp, aspclk);

	if (!lpm3_send(a, msg_aspclk_off))
		wait_bit(a, SCPERSTAT0, BIT(26), false);
}

static int aspclk_is_prepared(struct clk_hw *hw)
{
	struct bach2_asp *a = container_of(hw, struct bach2_asp, aspclk);

	return !!(readl(a->sctrl + SCPERSTAT0) & BIT(26));
}

static const struct clk_ops aspclk_ops = {
	.prepare = aspclk_prepare,
	.unprepare = aspclk_unprepare,
	.is_prepared = aspclk_is_prepared,
};

static void free_chan(void *ch)
{
	mbox_free_channel(ch);
}

static struct regulator_init_data asp_init = {
	.constraints = {
		.name = "asp",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static int bach2_asp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config rc = { };
	struct clk_init_data init = { .name = "bach2_clk_asp_subsys", .ops = &aspclk_ops,
				      .flags = CLK_IGNORE_UNUSED };
	struct regulator_dev *rdev;
	struct clk_hw *pmuclk;
	struct bach2_asp *a;
	int r;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	mutex_init(&a->lock);
	/* Shared always-on blocks: mapped without claiming them (crg/sctrl syscons, PMIC MFD). */
	a->sctrl = devm_ioremap(dev, SCTRL_BASE, 0x1000);
	a->pmic = devm_ioremap(dev, PMIC_BASE, 0x1000);
	if (!a->sctrl || !a->pmic)
		return -ENOMEM;
	a->cl.dev = dev;
	a->cl.tx_block = false;
	a->cl.knows_txdone = true;
	a->ch = mbox_request_channel(&a->cl, 0);
	if (IS_ERR(a->ch))
		return dev_err_probe(dev, PTR_ERR(a->ch), "mailbox 14\n");
	r = devm_add_action_or_reset(dev, free_chan, a->ch);
	if (r)
		return r;

	rc.dev = dev;
	rc.driver_data = a;
	rc.of_node = dev->of_node;
	rc.init_data = &asp_init;
	rdev = devm_regulator_register(dev, &asp_desc, &rc);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev), "regulator\n");

	a->clks = devm_kzalloc(dev, struct_size(a->clks, hws, 2), GFP_KERNEL);
	if (!a->clks)
		return -ENOMEM;
	a->clks->num = 2;
	/* No CLK_IGNORE_UNUSED: unused-clock cleanup ran long before this module loads. */
	pmuclk = devm_clk_hw_register_gate(dev, "bach2_clk_pmuaudioclk", NULL, 0,
					   a->pmic + PMIC_AUDIOCLK, 0, 0, NULL);
	if (IS_ERR(pmuclk))
		return dev_err_probe(dev, PTR_ERR(pmuclk), "pmuaudioclk\n");
	a->aspclk.init = &init;
	r = devm_clk_hw_register(dev, &a->aspclk);
	if (r)
		return dev_err_probe(dev, r, "asp subsys clk\n");
	a->clks->hws[0] = pmuclk;
	a->clks->hws[1] = &a->aspclk;
	r = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, a->clks);
	if (r)
		return r;
	dev_info(dev, "asp providers up: scpwrack=0x%08x scperstat0=0x%08x pmic0x114=0x%02x\n",
		 readl(a->sctrl + SCPWRACK), readl(a->sctrl + SCPERSTAT0),
		 readl(a->pmic + PMIC_AUDIOCLK));
	return 0;
}

static const struct of_device_id bach2_asp_of_match[] = {
	{ .compatible = "bach2,asp" },
	{ }
};
MODULE_DEVICE_TABLE(of, bach2_asp_of_match);

static struct platform_driver bach2_asp_driver = {
	.probe = bach2_asp_probe,
	.driver = { .name = "bach2-asp", .of_match_table = bach2_asp_of_match },
};
module_platform_driver(bach2_asp_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("bach2 ASP power regulator and audio clocks via LPM3 mailbox");
