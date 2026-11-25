/*
 * asp_dma.c -- asp dma driver
 *
 * Copyright (c) 2015 Hisilicon Technologies CO., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/hwspinlock.h>
#include <linux/dma-mapping.h>
#include <linux/wakelock.h>
#include <linux/interrupt.h>
#include <linux/hisi/hi64xx/asp_dma.h>
#include <linux/delay.h>

/*lint -e774 -e747 -e502*/

#define ASP_DMA_HWLOCK_ID (6)
#define HWLOCK_WAIT_TIME (50)

struct dma_callback{
    callback_t callback;
    unsigned long para;
};

struct asp_dma_priv {
	struct device *dev;
	int irq;
	spinlock_t lock;
	struct resource *res;
	struct hwspinlock *hwlock;
	struct wake_lock wake_lock;
	void __iomem *asp_dma_reg_base_addr;
	struct dma_callback callback[ASP_DMA_MAX_CHANNEL_NUM];
};

static struct asp_dma_priv *asp_dma_priv = NULL;

unsigned int _dmac_reg_read(unsigned int reg)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned long flag = 0;
	unsigned int ret = 0;

	WARN_ON(NULL == priv);

	if (hwspin_lock_timeout_irqsave(priv->hwlock, HWLOCK_WAIT_TIME, &flag)) {
		dev_err(priv->dev, "hwspinlock timeout\n");
		return 0;
	}

	ret = readl(priv->asp_dma_reg_base_addr + reg);

	hwspin_unlock_irqrestore(priv->hwlock, &flag);

	return ret;
}

static void _dmac_reg_write(unsigned int reg, unsigned int value)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned long flag = 0;

	WARN_ON(NULL == priv);

	if (hwspin_lock_timeout_irqsave(priv->hwlock, HWLOCK_WAIT_TIME, &flag)) {
		dev_err(priv->dev, "hwspinlock timeout\n");
		return;
	}

	writel(value, priv->asp_dma_reg_base_addr + reg);

	hwspin_unlock_irqrestore(priv->hwlock, &flag);
}

static void _dmac_reg_set_bit(unsigned int reg, unsigned int offset)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int value = 0;
	unsigned long flag_hw = 0;
	unsigned long flag_sft = 0;

	WARN_ON(NULL == priv);

	if (hwspin_lock_timeout_irqsave(priv->hwlock, HWLOCK_WAIT_TIME, &flag_hw)) {
		dev_err(priv->dev, "hwspinlock timeout\n");
		return;
	}

	spin_lock_irqsave(&priv->lock, flag_sft);

	value = readl(priv->asp_dma_reg_base_addr + reg);
	value |= (1 << offset);
	writel(value, priv->asp_dma_reg_base_addr + reg);

	spin_unlock_irqrestore(&priv->lock, flag_sft);

	hwspin_unlock_irqrestore(priv->hwlock, &flag_hw);
}

static void _dmac_reg_clr_bit(unsigned int reg, unsigned int offset)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int value = 0;
	unsigned long flag_hw = 0;
	unsigned long flag_sft = 0;

	WARN_ON(NULL == priv);

	if (hwspin_lock_timeout_irqsave(priv->hwlock, HWLOCK_WAIT_TIME, &flag_hw)) {
		dev_err(priv->dev, "hwspinlock timeout!\n");
		return;
	}

	spin_lock_irqsave(&priv->lock, flag_sft);

	value = readl(priv->asp_dma_reg_base_addr + reg);
	value &= ~(1UL << offset);
	writel(value, priv->asp_dma_reg_base_addr + reg);

	spin_unlock_irqrestore(&priv->lock, flag_sft);

	hwspin_unlock_irqrestore(priv->hwlock, &flag_hw);
}

static void _dmac_dump(unsigned int dma_channel)
{
	struct asp_dma_priv *priv = asp_dma_priv;

	WARN_ON(NULL == priv);

	if (dma_channel >= ASP_DMA_MAX_CHANNEL_NUM) {
		dev_err(priv->dev, "dma channel err:%d\n", dma_channel);
		return;
	}

	dev_dbg(priv->dev, "a count:0x%x\n", _dmac_reg_read(ASP_DMA_CX_CNT0(dma_channel)));
	dev_dbg(priv->dev, "src addr:0x%x\n", _dmac_reg_read(ASP_DMA_CX_SRC_ADDR(dma_channel)));
	dev_dbg(priv->dev, "des addr:0x%x\n", _dmac_reg_read(ASP_DMA_CX_DES_ADDR(dma_channel)));
	dev_dbg(priv->dev, "lli:0x%x\n", _dmac_reg_read(ASP_DMA_CX_LLI(dma_channel)));
	dev_dbg(priv->dev, "config:0x%x\n", _dmac_reg_read(ASP_DMA_CX_CONFIG(dma_channel)));

	dev_dbg(priv->dev, "c count:0x%x\n", _dmac_reg_read(ASP_DMA_CX_CNT1(dma_channel)));
	dev_dbg(priv->dev, "b index:0x%x\n", _dmac_reg_read(ASP_DMA_CX_BINDX(dma_channel)));
	dev_dbg(priv->dev, "c index:0x%x\n", _dmac_reg_read(ASP_DMA_CX_CINDX(dma_channel)));
}


/*
 * bach2: DMA diagnostics (docs/mainline-audio-port.md, C5c "transit error").
 *  - bach2_dmac_dump(): read-only dump of the DMAC global state, the channels in a mask, and the
 *    ASP_CFG words that gate DMAC/SLIMbus bus access (reset, clocks, TZ secure, SLIMbus data
 *    format, codec DMA select, address monitor). Printed once, on the first error interrupt.
 *  - selftest (sysfs "selftest", also once at probe): channel BACH2_ST_CH moves 256 bytes
 *    mem->mem inside the no-map hifi_data scratch window, then 16 bytes mem->SLIMbus port-0 FIFO
 *    (flow mem2mem, no handshake). Which leg raises ERR2 says whether the DMAC cannot reach DDR
 *    (source of the playback failure) or the SLIMbus FIFO (destination).
 * Only touches hardware when SCTRL says the ASP is powered, isolated off and its bus/cfg gates on.
 */
#define BACH2_ST_CH		7		/* vendor: capture port 2; unused for playback */
#define BACH2_ST_PHYS		0x35110000UL	/* hifi_data (0x34f00000 +4 MiB), past every
						 * vendor PCM ring used here (PCM_DMA_BUF_2_*) */
#define BACH2_ST_SIZE		0x2000
#define BACH2_FIFO0		0xe8051000UL	/* dma_cfg_slimbus_6402[0].port */
#define BACH2_CFG_M2M		0xC3322001	/* SI|DI, burst 4/4, width 32/32, mem2mem, EN */
#define BACH2_CFG_M2F		0x83322001	/* SI, burst 4/4, width 32/32, mem2mem, EN */

static void __iomem *bach2_aspcfg;
static void __iomem *bach2_sctrl;
static bool bach2_dumped;

static bool bach2_asp_alive(void)
{
	u32 ack, iso, ps0, ps1;

	if (!bach2_sctrl)
		return false;
	ack = readl(bach2_sctrl + 0x06c) & 1;
	iso = readl(bach2_sctrl + 0x048) & 1;
	ps0 = readl(bach2_sctrl + 0x16c);
	ps1 = readl(bach2_sctrl + 0x18c);
	return ack && !iso && (ps0 & BIT(26)) && (ps1 & 0xf) == 0xf;
}

static void bach2_dmac_dump(const char *why, unsigned int chmask)
{
	static const struct { unsigned int off; const char *n; } cfg[] = {
		{ 0x008, "RST_CTRLSTAT" }, { 0x018, "GATE_CLKSTAT" }, { 0x038, "CLK_SEL" },
		{ 0x054, "DMAC_SEL" }, { 0x058, "BUS_PRIORITY" }, { 0x05c, "CG_EN" },
		{ 0x078, "DLOCK_BP" }, { 0x100, "TZ_SECURE_N" }, { 0x16c, "ADDR_MON_INTR_STAT" },
		{ 0x178, "ADDR_MON0_HIT_AW" }, { 0x17c, "ADDR_MON0_HIT_AR" }, { 0x1b4, "SLIMBUS_PRIMARY" },
		{ 0x1bc, "SLIM_DAT_FMT_CFG" }, { 0x1c0, "SLIM_DAT_CHNL_CFG" }, { 0x1e0, "CODEC_SEL_EN" },
		{ 0x200, "SECURE_AUTHORITY_EN" },
		{ 0x118, "DSP_REMAPPING_EN" }, { 0x11c, "DSP_REMAP_SRC" }, { 0x120, "DSP_REMAP_DES" },
		{ 0x124, "DDR_HARQ_REMAP_EN" }, { 0x130, "DDR_MMBUF_REMAP_EN" }, { 0x13c, "DDR_OCRAM_REMAP_EN" },
		{ 0x148, "MMBUF_CTRL" }, { 0x160, "ADDR_MON_EN" },
	};
	unsigned int i, ch;

	if (!bach2_asp_alive()) {
		pr_crit("BACH2ML: AUD: dmac-dump %s skipped: ASP not powered/ungated\n", why);
		return;
	}
	pr_crit("BACH2ML: AUD: dmac-dump %s: INT_STAT=0x%x ERR1=0x%x ERR2=0x%x ERR3=0x%x TC1=0x%x TC2=0x%x RAW ERR1=0x%x ERR2=0x%x ERR3=0x%x TC1=0x%x TC2=0x%x CH_STAT=0x%x CH_PRI=0x%x DMA_CTRL=0x%x\n",
		why, _dmac_reg_read(ASP_DMA_INT_STAT_AP), _dmac_reg_read(ASP_DMA_INT_ERR1_AP),
		_dmac_reg_read(ASP_DMA_INT_ERR2_AP), _dmac_reg_read(ASP_DMA_INT_ERR3_AP),
		_dmac_reg_read(ASP_DMA_INT_TC1_AP), _dmac_reg_read(ASP_DMA_INT_TC2_AP),
		_dmac_reg_read(ASP_DMA_INT_ERR1_RAW), _dmac_reg_read(ASP_DMA_INT_ERR2_RAW),
		_dmac_reg_read(ASP_DMA_INT_ERR3_RAW), _dmac_reg_read(ASP_DMA_INT_TC1_RAW),
		_dmac_reg_read(ASP_DMA_INT_TC2_RAW), _dmac_reg_read(ASP_DMA_CH_STAT),
		_dmac_reg_read(ASP_DMA_CH_PRI), _dmac_reg_read(ASP_DMA_DMA_CTRL));
	for (ch = 0; ch < ASP_DMA_MAX_CHANNEL_NUM; ch++) {
		if (!(chmask & BIT(ch)))
			continue;
		pr_crit("BACH2ML: AUD: dmac-dump ch%u: curr_cnt1=0x%x curr_cnt0=0x%x curr_src=0x%08x curr_des=0x%08x lli=0x%08x cnt0=0x%x src=0x%08x des=0x%08x config=0x%08x axi_conf=0x%08x\n",
			ch, _dmac_reg_read(ASP_DMA_CX_CURR_CNT1(ch)), _dmac_reg_read(ASP_DMA_CX_CURR_CNT0(ch)),
			_dmac_reg_read(ASP_DMA_CX_CURR_SRC_ADDR(ch)), _dmac_reg_read(ASP_DMA_CX_CURR_DES_ADDR(ch)),
			_dmac_reg_read(ASP_DMA_CX_LLI(ch)), _dmac_reg_read(ASP_DMA_CX_CNT0(ch)),
			_dmac_reg_read(ASP_DMA_CX_SRC_ADDR(ch)), _dmac_reg_read(ASP_DMA_CX_DES_ADDR(ch)),
			_dmac_reg_read(ASP_DMA_CX_CONFIG(ch)), _dmac_reg_read(ASP_DMA_CX_AXI_CONF(ch)));
	}
	if (bach2_aspcfg)
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			pr_crit("BACH2ML: AUD: dmac-dump asp_cfg %s(0x%03x)=0x%08x\n", cfg[i].n,
				cfg[i].off, readl(bach2_aspcfg + cfg[i].off));
}

/* one single-node transfer on BACH2_ST_CH; returns "TC", "ERR1".."ERR3" or "TIMEOUT" */
static const char *bach2_st_run(u32 src, u32 des, u32 cnt, u32 cfg)
{
	unsigned int m = BIT(BACH2_ST_CH), t;
	const char *r = "TIMEOUT";

	_dmac_reg_clr_bit(ASP_DMA_CX_CONFIG(BACH2_ST_CH), 0);
	_dmac_reg_write(ASP_DMA_INT_TC1_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_TC2_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_ERR1_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_ERR2_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_ERR3_RAW, m);
	_dmac_reg_write(ASP_DMA_CX_CNT0(BACH2_ST_CH), cnt);
	_dmac_reg_write(ASP_DMA_CX_CNT1(BACH2_ST_CH), 0);
	_dmac_reg_write(ASP_DMA_CX_BINDX(BACH2_ST_CH), 0);
	_dmac_reg_write(ASP_DMA_CX_CINDX(BACH2_ST_CH), 0);
	_dmac_reg_write(ASP_DMA_CX_SRC_ADDR(BACH2_ST_CH), src);
	_dmac_reg_write(ASP_DMA_CX_DES_ADDR(BACH2_ST_CH), des);
	_dmac_reg_write(ASP_DMA_CX_LLI(BACH2_ST_CH), 0);
	_dmac_reg_write(ASP_DMA_CX_CONFIG(BACH2_ST_CH), cfg);
	for (t = 0; t < 1000; t++) {
		if (_dmac_reg_read(ASP_DMA_INT_ERR1_RAW) & m) { r = "ERR1"; break; }
		if (_dmac_reg_read(ASP_DMA_INT_ERR2_RAW) & m) { r = "ERR2"; break; }
		if (_dmac_reg_read(ASP_DMA_INT_ERR3_RAW) & m) { r = "ERR3"; break; }
		if (_dmac_reg_read(ASP_DMA_INT_TC1_RAW) & m) { r = "TC"; break; }
		udelay(10);
	}
	if (strcmp(r, "TC"))
		bach2_dmac_dump(r, m);
	_dmac_reg_clr_bit(ASP_DMA_CX_CONFIG(BACH2_ST_CH), 0);
	_dmac_reg_write(ASP_DMA_INT_TC1_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_ERR1_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_ERR2_RAW, m);
	_dmac_reg_write(ASP_DMA_INT_ERR3_RAW, m);
	return r;
}

/*
 * C5d follow-up (docs/mainline-audio-port.md): the hifi_data legs fail with ERR2 on the read side.
 * Leg 3: an ordinary dma_alloc_coherent buffer (32-bit DMA mask, CMA/System RAM, outside
 * hifi_data): is DDR in general unreachable for this master, or only the reserved HiFi window?
 * Leg 4: ASP-internal HiFi DTCM -> DTCM (0xe8058000, never runs firmware here): does the DMAC
 * work at all on its local bus? An ERR2 there is ambiguous (DTCM clocking), a TC is not.
 * Plus the read-only SCTRL words around the ASP master/NoC path.
 */
#define BACH2_DTCM		0xe8058000UL	/* SOC_ACPU_DSP_DTCM_BASE_ADDR */

static void bach2_sctrl_words(const char *why)
{
	static const struct { unsigned int off; const char *n; } w[] = {
		{ 0x0b8, "SCINT_STAT1" }, { 0x18c, "SCPERSTAT1" }, { 0x1a4, "SCPERCLKEN3" },
		{ 0x31c, "SCPERCTRL7(b2=noc_asp_power_idlereq)" }, { 0x360, "SCPERSTATUS0(asp dwaxi dlock)" },
		{ 0x3d0, "SC_SECOND_INT_MASK" }, { 0x3d4, "SC_SECOND_INT_ORG(b2 asp probe alarm, b3 asp err probe, b5 asp cfg timeout)" },
	};
	unsigned int i;

	if (!bach2_sctrl)
		return;
	for (i = 0; i < ARRAY_SIZE(w); i++)
		pr_crit("BACH2ML: AUD: sctrl %s %s(0x%03x)=0x%08x\n", why, w[i].n, w[i].off,
			readl(bach2_sctrl + w[i].off));
}

static bool bach2_dtcm_leg;
module_param(bach2_dtcm_leg, bool, 0444);
MODULE_PARM_DESC(bach2_dtcm_leg, "self-test leg 4 (DTCM->DTCM); it hung the channel in C5d");

static void bach2_selftest2(const char *why)
{
	struct device *dev = asp_dma_priv->dev;
	dma_addr_t pa = 0;
	u32 *v;
	const char *r3, *r4;
	unsigned int i, bad = 0;

	bach2_sctrl_words(why);
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32)))
		pr_crit("BACH2ML: AUD: dma-selftest2 %s: 32-bit DMA mask refused\n", why);
	v = dma_alloc_coherent(dev, 0x2000, &pa, GFP_KERNEL);
	if (!v) {
		pr_crit("BACH2ML: AUD: dma-selftest2 %s: dma_alloc_coherent failed\n", why);
		r3 = "NOBUF";
	} else if (pa > 0xffffffffULL - 0x2000) {
		pr_crit("BACH2ML: AUD: dma-selftest2 %s: coherent buffer above 4G (%pad)\n", why, &pa);
		r3 = "ABOVE4G";
	} else {
		for (i = 0; i < 64; i++) {
			v[i] = 0x5a000000 | i;
			v[0x400 + i] = 0;
		}
		wmb();
		r3 = bach2_st_run((u32)pa, (u32)pa + 0x1000, 256, BACH2_CFG_M2M);
		rmb();
		for (i = 0; i < 64; i++)
			bad += v[0x400 + i] != (0x5a000000 | i);
	}
	/* C5d: the DTCM read never completed and left ch7 active (CH_STAT 0x80) even after the enable
	 * bit was cleared, so a later self-test saw ch7 busy. Opt-in only (bach2_dtcm_leg=1). */
	r4 = bach2_dtcm_leg ? bach2_st_run(BACH2_DTCM, BACH2_DTCM + 0x100, 256, BACH2_CFG_M2M) : "SKIPPED";
	pr_crit("BACH2ML: AUD: dma-selftest2 %s: coherent(%pad)->coherent=%s mismatches=%u/64 dtcm->dtcm=%s\n",
		why, &pa, r3, bad, r4);
	if (v)
		dma_free_coherent(dev, 0x2000, v, pa);
}

static int bach2_selftest(const char *why)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	void __iomem *b;
	const char *r1, *r2;
	unsigned int i, bad = 0;

	if (!priv || !bach2_asp_alive()) {
		pr_crit("BACH2ML: AUD: dma-selftest %s skipped: ASP not powered/ungated\n", why);
		return -ENODEV;
	}
	if (_dmac_reg_read(ASP_DMA_CH_STAT) & BIT(BACH2_ST_CH)) {
		pr_crit("BACH2ML: AUD: dma-selftest %s skipped: ch%u busy\n", why, BACH2_ST_CH);
		return -EBUSY;
	}
	b = ioremap(BACH2_ST_PHYS, BACH2_ST_SIZE);
	if (!b)
		return -ENOMEM;
	for (i = 0; i < 256; i += 4) {
		writel(0xa5000000 | i, b + i);
		writel(0, b + 0x1000 + i);
	}
	wmb();
	r1 = bach2_st_run(BACH2_ST_PHYS, BACH2_ST_PHYS + 0x1000, 256, BACH2_CFG_M2M);
	for (i = 0; i < 256; i += 4)
		bad += readl(b + 0x1000 + i) != (0xa5000000 | i);
	/* zeros into the port-0 FIFO: 16 bytes, no handshake */
	for (i = 0; i < 16; i += 4)
		writel(0, b + 0x800 + i);
	wmb();
	r2 = bach2_st_run(BACH2_ST_PHYS + 0x800, BACH2_FIFO0, 16, BACH2_CFG_M2F);
	iounmap(b);
	pr_crit("BACH2ML: AUD: dma-selftest %s: ddr->ddr=%s mismatches=%u/64 ddr->slimbus_fifo0=%s\n",
		why, r1, bad, r2);
	bach2_selftest2(why);
	return 0;
}

/*
 * sysfs "2" only (never at probe): the hifi_data DDR->DDR leg again with channel 7's AXI_CONF
 * ARPROT/AWPROT = 0 (secure attribute) instead of the reset 0x00040040 (both 0b010, non-secure).
 * Writes nothing but the channel-7 AXI_CONF (restored) and the scratch window; no TZ/DMSS register.
 * The NS legs already showed that a rejected ASP access only yields ERR2, the system stays up.
 */
static void bach2_selftest_prot(void)
{
	void __iomem *b;
	u32 old;
	const char *r;
	unsigned int i, bad = 0;

	if (!asp_dma_priv || !bach2_asp_alive()) {
		pr_crit("BACH2ML: AUD: dma-selftest-prot skipped: ASP not powered/ungated\n");
		return;
	}
	b = ioremap(BACH2_ST_PHYS, BACH2_ST_SIZE);
	if (!b)
		return;
	for (i = 0; i < 256; i += 4) {
		writel(0x3c000000 | i, b + i);
		writel(0, b + 0x1000 + i);
	}
	wmb();
	old = _dmac_reg_read(ASP_DMA_CX_AXI_CONF(BACH2_ST_CH));
	_dmac_reg_write(ASP_DMA_CX_AXI_CONF(BACH2_ST_CH), old & ~((0x7u << 17) | (0x7u << 5)));
	r = bach2_st_run(BACH2_ST_PHYS, BACH2_ST_PHYS + 0x1000, 256, BACH2_CFG_M2M);
	_dmac_reg_write(ASP_DMA_CX_AXI_CONF(BACH2_ST_CH), old);
	for (i = 0; i < 256; i += 4)
		bad += readl(b + 0x1000 + i) != (0x3c000000 | i);
	iounmap(b);
	pr_crit("BACH2ML: AUD: dma-selftest-prot: axi_conf 0x%08x->0x%08x (restored) ddr->ddr=%s mismatches=%u/64\n",
		old, old & ~((0x7u << 17) | (0x7u << 5)), r, bad);
}

static ssize_t selftest_store(struct device *dev, struct device_attribute *a, const char *buf,
			      size_t n)
{
	int r;

	if (n && buf[0] == '2') {
		bach2_selftest_prot();
		return n;
	}
	r = bach2_selftest("sysfs");

	return r ? r : n;
}
static DEVICE_ATTR_WO(selftest);

static irqreturn_t _asp_dmac_irq_handler(int irq, void *data)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int err1 = 0;
	unsigned int err2 = 0;
	unsigned int err3 = 0;
	unsigned int tc1 = 0;
	unsigned int tc2 = 0;
	unsigned int int_mask = 0;
	unsigned short int_state = 0;
	unsigned int int_type = 0;
	unsigned int i = 0;

	WARN_ON(NULL == priv);

	/* if have interupts */
	int_state = (unsigned short)_dmac_reg_read(ASP_DMA_INT_STAT_AP);

	if (0 == int_state)
		return IRQ_HANDLED;

	/* read interupt states */
	err1 = _dmac_reg_read(ASP_DMA_INT_ERR1_AP);
	err2 = _dmac_reg_read(ASP_DMA_INT_ERR2_AP);
	err3 = _dmac_reg_read(ASP_DMA_INT_ERR3_AP);
	tc1  = _dmac_reg_read(ASP_DMA_INT_TC1_AP);
	tc2  = _dmac_reg_read(ASP_DMA_INT_TC2_AP);

	/* bach2: one read-only dump on the first error interrupt (before the RAW clears below) */
	if ((err1 | err2 | err3) && !bach2_dumped) {
		bach2_dumped = true;
		bach2_dmac_dump("first-error", (err1 | err2 | err3) | BIT(5) | BIT(6));
	}

	/* clr interupt states */
	_dmac_reg_write(ASP_DMA_INT_TC1_RAW, int_state);
	_dmac_reg_write(ASP_DMA_INT_TC2_RAW, int_state);
	_dmac_reg_write(ASP_DMA_INT_ERR1_RAW, int_state);
	_dmac_reg_write(ASP_DMA_INT_ERR2_RAW, int_state);
	_dmac_reg_write(ASP_DMA_INT_ERR3_RAW, int_state);

	for (i = 0; i < ASP_DMA_MAX_CHANNEL_NUM; i++) {
		int_mask = 0x1L << i;

		if (int_state & int_mask) {
			if (priv->callback[i].callback) {
				if (err1 & int_mask)
					int_type = ASP_DMA_INT_TYPE_ERR1;
				else if (err2 & int_mask)
					int_type = ASP_DMA_INT_TYPE_ERR2;
				else if (err3 & int_mask)
					int_type = ASP_DMA_INT_TYPE_ERR3;
				else if (tc1 & int_mask)
					int_type = ASP_DMA_INT_TYPE_TC1;
				else if (tc2 & int_mask)
					int_type = ASP_DMA_INT_TYPE_TC2;
				else
					int_type = ASP_DMA_INT_TYPE_BUTT;

				priv->callback[i].callback(int_type, priv->callback[i].para, i);
			}
		}
	}

	return IRQ_HANDLED;
}

unsigned int asp_dma_get_des(unsigned short dma_channel)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int dma_used_addr = 0;

	if (!priv) {
		pr_err("priv is null\n");
		return 0;
	}

	dma_used_addr = _dmac_reg_read(ASP_DMA_CX_DES_ADDR(dma_channel));

	return dma_used_addr;
}
EXPORT_SYMBOL(asp_dma_get_des);

unsigned int asp_dma_get_src(unsigned short dma_channel)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int dma_used_addr = 0;

	if (!priv) {
		pr_err("priv is null\n");
		return 0;
	}

	dma_used_addr = _dmac_reg_read(ASP_DMA_CX_SRC_ADDR(dma_channel));

	return dma_used_addr;
}
EXPORT_SYMBOL(asp_dma_get_src);

/**
 * asp dma clk has the same life cycle as asp subsys clk.
 * ensure asp subsys clk enabled, when called this func.
 * asp subsys clk enabled in slimbus & codec controller.
 */
int asp_dma_config(
			unsigned short dma_channel,
			struct dma_lli_cfg *lli_cfg,
			callback_t callback,
			unsigned long para)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int channel_mask = (0x1L << dma_channel);

	if (!priv) {
		pr_err("priv is null\n");
		return -EINVAL;
	}

	if (dma_channel >= ASP_DMA_MAX_CHANNEL_NUM) {
		dev_err(priv->dev, "dma channel err:%d\n", dma_channel);
		return -EINVAL;
	}

	if (!lli_cfg) {
		dev_err(priv->dev, "lli cfg is null.\n");
		return -EINVAL;
	}

	/* disable dma channel */
	_dmac_reg_clr_bit(ASP_DMA_CX_CONFIG(dma_channel), 0);

	_dmac_reg_write(ASP_DMA_CX_CNT0(dma_channel), lli_cfg->a_count);

	/* c count */
	_dmac_reg_write(ASP_DMA_CX_CNT1(dma_channel), 0);
	_dmac_reg_write(ASP_DMA_CX_BINDX(dma_channel), 0);
	_dmac_reg_write(ASP_DMA_CX_CINDX(dma_channel), 0);

	/* set dma src/des addr */
	_dmac_reg_write(ASP_DMA_CX_SRC_ADDR(dma_channel), lli_cfg->src_addr);
	_dmac_reg_write(ASP_DMA_CX_DES_ADDR(dma_channel), lli_cfg->des_addr);

	/* set dma lli config */
	_dmac_reg_write(ASP_DMA_CX_LLI(dma_channel), lli_cfg->lli );

	/* clr irq status of dma channel */
	_dmac_reg_write(ASP_DMA_INT_TC1_RAW, channel_mask);
	_dmac_reg_write(ASP_DMA_INT_TC2_RAW, channel_mask);
	_dmac_reg_write(ASP_DMA_INT_ERR1_RAW, channel_mask);
	_dmac_reg_write(ASP_DMA_INT_ERR2_RAW, channel_mask);
	_dmac_reg_write(ASP_DMA_INT_ERR3_RAW, channel_mask);

	if (NULL != callback) {
		priv->callback[dma_channel].callback = callback;
		priv->callback[dma_channel].para = para;

		/* release irq mask */
		_dmac_reg_set_bit(ASP_DMA_INT_ERR1_MASK_AP, dma_channel);
		_dmac_reg_set_bit(ASP_DMA_INT_ERR2_MASK_AP, dma_channel);
		_dmac_reg_set_bit(ASP_DMA_INT_ERR3_MASK_AP, dma_channel);
		_dmac_reg_set_bit(ASP_DMA_INT_TC1_MASK_AP, dma_channel);
		_dmac_reg_set_bit(ASP_DMA_INT_TC2_MASK_AP, dma_channel);
	}

	dev_info(priv->dev, "dma config succ.\n");

	return 0;
}
EXPORT_SYMBOL(asp_dma_config);

int asp_dma_start(
			unsigned short dma_channel,
			struct dma_lli_cfg *lli_cfg)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int lli_reg;

	if (!priv) {
		pr_err("priv is null\n");
		return -EINVAL;
	}

	if (dma_channel >= ASP_DMA_MAX_CHANNEL_NUM) {
		dev_err(priv->dev, "dma channel err:%d\n", dma_channel);
		return -EINVAL;
	}

	if (!lli_cfg) {
		dev_err(priv->dev, "lli cfg is null.\n");
		return -EINVAL;
	}

	lli_reg = _dmac_reg_read(ASP_DMA_CX_LLI(dma_channel));
	if (lli_reg != lli_cfg->lli) {
		dev_err(priv->dev, "lli is changed, lli_reg: %d, lli_cfg: %d\n", lli_reg, lli_cfg->lli);
		return -EINVAL;
	}

	_dmac_reg_write(ASP_DMA_CX_CONFIG(dma_channel), lli_cfg->config);

	_dmac_dump(dma_channel);

	dev_info(priv->dev, "dma start succ.\n");

	return 0;
}
EXPORT_SYMBOL(asp_dma_start);

void asp_dma_stop(unsigned short dma_channel)
{
	struct asp_dma_priv *priv = asp_dma_priv;
	unsigned int channel_mask = (0x1U << dma_channel);
	unsigned int i = 40;

	if (!priv) {
		pr_err("priv is null\n");
		return;
	}

	if (dma_channel >= ASP_DMA_MAX_CHANNEL_NUM) {
		dev_err(priv->dev, "dma channel err:%d\n", dma_channel);
		return;
	}

	/* disable dma channel */
	_dmac_reg_clr_bit(ASP_DMA_CX_CONFIG(dma_channel), 0);

	do {
		if (0 == (_dmac_reg_read(ASP_DMA_CH_STAT) & channel_mask)) {
			break;
		}
		dev_info(priv->dev, "stopping dma_channel: %d\n", dma_channel);
		udelay(250);
	} while (--i);

	_dmac_reg_clr_bit(ASP_DMA_INT_ERR1_MASK_AP, dma_channel);
	_dmac_reg_clr_bit(ASP_DMA_INT_ERR2_MASK_AP, dma_channel);
	_dmac_reg_clr_bit(ASP_DMA_INT_ERR3_MASK_AP, dma_channel);
	_dmac_reg_clr_bit(ASP_DMA_INT_TC1_MASK_AP, dma_channel);
	_dmac_reg_clr_bit(ASP_DMA_INT_TC2_MASK_AP, dma_channel);

	memset(&priv->callback[dma_channel], 0, sizeof(struct dma_callback));

	if (!i) {
		dev_err(priv->dev, "dma_channel:%d stop fail, channel state: %d\n", dma_channel, (_dmac_reg_read(ASP_DMA_CH_STAT) & channel_mask));
		return;
	}
	dev_info(priv->dev, "dma_channel:%d stop succ\n", dma_channel);
}
EXPORT_SYMBOL(asp_dma_stop);
/*lint -e429*/
static int asp_dma_probe (struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct asp_dma_priv *priv = NULL;

	dev_info(dev, "probe begin.\n");

	priv = devm_kzalloc(dev, sizeof(struct asp_dma_priv), GFP_KERNEL);
	if (!priv) {
		dev_err(dev, "malloc failed.\n");
		return -ENOMEM;
	}

	priv->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!priv->res) {
		dev_err(dev,"get resource failed.\n");
		ret = -ENOENT;
		goto err_exit;
	}

/* fixme: need request mem region when soundtrigger
 * used this module to request dma
 */

	priv->asp_dma_reg_base_addr = devm_ioremap(dev, priv->res->start,
					resource_size(priv->res));
	if (!priv->asp_dma_reg_base_addr) {
		dev_err(dev,"asp dma reg addr ioremap failed.\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	priv->irq = platform_get_irq_byname(pdev, "asp_dma_irq");
	if (priv->irq < 0) {
		dev_err(dev,"get asp dma irq failed.\n");
		ret= -ENOENT;
		goto err_exit;
	}

	ret = request_irq(priv->irq, _asp_dmac_irq_handler,
			  IRQF_SHARED | IRQF_NO_SUSPEND, "asp_dma_irq", dev);
	if (ret) {
		dev_err(dev,"request asp dma irq failed.\n");
		ret = -ENOENT;
		goto err_exit;
	}

	priv->hwlock = hwspin_lock_request_specific(ASP_DMA_HWLOCK_ID);
	if (!priv->hwlock) {
		dev_err(dev,"request hw spinlok failed.\n");
		ret = -ENOENT;
		goto err_exit;
	}

	wake_lock_init(&priv->wake_lock, WAKE_LOCK_SUSPEND, "asp_dma");

	spin_lock_init(&priv->lock);

	priv->dev = dev;

	platform_set_drvdata(pdev, priv);

	asp_dma_priv = priv;
	/* bach2: diagnostics (always-on SCTRL + ASP_CFG, read-only except the selftest channel) */
	bach2_sctrl = devm_ioremap(dev, 0xfff0a000, 0x1000);
	bach2_aspcfg = devm_ioremap(dev, 0xe804e000, 0x1000);
	if (device_create_file(dev, &dev_attr_selftest))
		dev_err(dev, "bach2: selftest attribute not created\n");
	bach2_selftest("probe");

	dev_info(dev, "probe end.\n");

	return 0;

err_exit:
	if (priv->asp_dma_reg_base_addr)
		devm_iounmap(dev, priv->asp_dma_reg_base_addr);

	devm_kfree(dev, priv);

	dev_err(dev, "probe failed.\n");

	return ret;
}
/*lint +e429*/
static int asp_dma_remove (struct platform_device *pdev)
{
	struct asp_dma_priv *priv =
		(struct asp_dma_priv*)platform_get_drvdata(pdev);

	if (!priv)
		return 0;

	if (priv->asp_dma_reg_base_addr)
		devm_iounmap(priv->dev, priv->asp_dma_reg_base_addr);

	if (hwspin_lock_free(priv->hwlock)) {
		dev_err(priv->dev,"hwspinlock free failed.\n");
	}

	wake_lock_destroy(&priv->wake_lock);

	free_irq(priv->irq, priv);

	devm_kfree(priv->dev, priv);

	asp_dma_priv = NULL;

	return 0;
}

static const struct of_device_id of_asp_dma_match[] = {
	{ .compatible = "hisilicon,hi64xx-asp-dma", },
	{},
};

MODULE_DEVICE_TABLE(of, of_asp_dma_match);

BACH2_VOID_REMOVE(asp_dma_remove)

static struct platform_driver asp_dma_driver = {
	.driver 	= {
		.name	= "hi64xx_asp_dma_drv",
		.owner	= THIS_MODULE,
		.of_match_table = of_asp_dma_match,
	},
	.probe		= asp_dma_probe,
	.remove = asp_dma_remove_void,
};

static int __init asp_dma_init(void)
{
	return platform_driver_register(&asp_dma_driver);
}
module_init(asp_dma_init);

static void __exit asp_dma_exit(void)
{
	platform_driver_unregister(&asp_dma_driver);
}
module_exit(asp_dma_exit);

MODULE_AUTHOR("LiuJinHong <liujinhong@hisilicon.com>");
MODULE_DESCRIPTION("Hisilicon (R) ASP DMA Driver");
MODULE_LICENSE("GPL");
