// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) eMMC host: Arasan SDHCI with a HiSilicon PHY at 0x430.
 *
 * Bring-up driver for the Huawei MediaPad M5 lite 10 (bach2). The PHY handling mirrors the
 * vendor 4.9 driver (drivers/mmc/host/sdhci-of-arasan.c, compatible "arasan,sdhci") for the
 * legacy and high-speed timings only; HS200/HS400/DDR, tuning and command queueing are
 * not supported and are masked out of the host caps.
 *
 * This eMMC holds the bootloader and the only recovery path, so the driver refuses every
 * command that can change the device's contents (see hi6250_cmd_blocked()). There is no
 * way to lift that at runtime.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "sdhci-pltfm.h"

#define HI6250_MIN_FREQ			400000
#define HI6250_MAX_XIN_RATE		200000000UL	/* vendor SDHCI_ARASAN_XIN_CLK_FREQ */

/* Vendor sdhci.h register map (beyond the standard SDHCI block). */
#define HI6250_CORE_CFG1		0x404
#define HI6250_CORE_CFG1_64BIT		BIT(31)
#define HI6250_PHY_CTRL1		0x430
#define HI6250_PHY_CTRL1_ODEN_CMD	BIT(0)
#define HI6250_PHY_CTRL1_EN_DLL		BIT(7)
#define HI6250_PHY_CTRL1_ICP_SHIFT	8
#define HI6250_PHY_CTRL1_ICP_MASK	(0xff << HI6250_PHY_CTRL1_ICP_SHIFT)
#define HI6250_PHY_CTRL1_EN_RTRIM	BIT(16)
#define HI6250_PHY_CTRL1_DR_TY_SHIFT	22
#define HI6250_PHY_CTRL1_DR_TY_MASK	(0x7 << HI6250_PHY_CTRL1_DR_TY_SHIFT)
#define HI6250_PHY_CTRL1_PDB		BIT(25)
#define HI6250_PHY_CTRL2		0x434
#define HI6250_PHY_CTRL2_ODEN_DAT_SHIFT	22
#define HI6250_PHY_CTRL2_PUSHPULL	0x003ff7fc	/* vendor: data/cmd pull-up, strobe pull-down */
#define HI6250_PHY_CTRL3		0x438
#define HI6250_PHY_CTRL3_OTAPDLYENA	BIT(8)
#define HI6250_PHY_CTRL3_ITAPDLYENA	BIT(14)
#define HI6250_PHY_STATUS		0x440
#define HI6250_PHY_STATUS_EXR_NINST	BIT(0)
#define HI6250_PHY_STATUS_CALDONE	BIT(6)
#define HI6250_ENHANCED_STROBE		0x78

#define HI6250_HOST_DRV_DEFAULT		0x4		/* 40 ohm */

struct hi6250_sdhci {
	struct clk *clk_ahb;
	u32 host_drv;
	unsigned char old_bus_mode;
	unsigned int stray_data_end;
	void (*sdhci_request)(struct mmc_host *mmc, struct mmc_request *mrq);
	void (*sdhci_set_ios)(struct mmc_host *mmc, struct mmc_ios *ios);
};

/*
 * EXT_CSD bytes the MMC core may switch while bringing the card up to high speed and
 * shutting it down. Everything else (PART_CONFIG, BOOT_WP, BKOPS_*, SANITIZE, CMDQ,
 * RST_n, partitioning, ...) is refused.
 */
static const u8 hi6250_switch_allowed[] = {
	EXT_CSD_FLUSH_CACHE,		/* 32 */
	EXT_CSD_CACHE_CTRL,		/* 33 */
	EXT_CSD_POWER_OFF_NOTIFICATION,	/* 34 */
	EXT_CSD_HPI_MGMT,		/* 161 */
	EXT_CSD_ERASE_GROUP_DEF,	/* 175 */
	EXT_CSD_BUS_WIDTH,		/* 183 */
	EXT_CSD_HS_TIMING,		/* 185 */
	EXT_CSD_POWER_CLASS,		/* 187 */
};

/*
 * Allow-list of the commands the 7.0 MMC core sends to this eMMC (NO_SD, NO_SDIO, no CMD23, no
 * HS200/DDR/tuning, no HW reset, no CQE): identify and init (0, 1, 2, 3, 7, 8, 9, 10, 16), the
 * EXT_CSD switches above (6), status and HPI (13, 12), reads (17, 18, 12). Everything else is
 * refused, notably 4 SET_DSR, 5 SLEEP, 14/19 bus test, 15 GO_INACTIVE, 55 APP_CMD and every
 * write, erase, write-protect, lock and vendor command.
 */
static bool hi6250_cmd_blocked(struct mmc_command *cmd)
{
	unsigned int i, index;

	if (!cmd)
		return false;
	/*
	 * MMC_IOC_CMD passes a 32-bit opcode through unchecked, and SDHCI_MAKE_CMD keeps bits 7:0
	 * of it, the low 6 in the Command Index field: 64 + n would reach the card as CMDn.
	 */
	if (cmd->opcode > 63)
		return true;
	if (cmd->data && (cmd->data->flags & MMC_DATA_WRITE))
		return true;		/* any data write, whatever the opcode */

	switch (cmd->opcode) {
	case MMC_GO_IDLE_STATE:		/* 0 */
	case MMC_SEND_OP_COND:		/* 1 */
	case MMC_ALL_SEND_CID:		/* 2 */
	case MMC_SET_RELATIVE_ADDR:	/* 3 */
	case MMC_SELECT_CARD:		/* 7, also deselect (arg 0) */
	case MMC_SEND_EXT_CSD:		/* 8 (SD_SEND_IF_COND never: NO_SD) */
	case MMC_SEND_CSD:		/* 9 */
	case MMC_SEND_CID:		/* 10 */
	case MMC_STOP_TRANSMISSION:	/* 12, also HPI */
	case MMC_SEND_STATUS:		/* 13, also HPI */
	case MMC_SET_BLOCKLEN:		/* 16 */
	case MMC_READ_SINGLE_BLOCK:	/* 17 */
	case MMC_READ_MULTIPLE_BLOCK:	/* 18 */
		return false;
	case MMC_SET_BLOCK_COUNT:	/* 23: only as the sbc of a read, see hi6250_sbc_blocked() */
		return false;
	case MMC_SWITCH:		/* 6 */
		if (((cmd->arg >> 24) & 0x3) != MMC_SWITCH_MODE_WRITE_BYTE)
			return true;
		index = (cmd->arg >> 16) & 0xff;
		for (i = 0; i < ARRAY_SIZE(hi6250_switch_allowed); i++)
			if (index == hi6250_switch_allowed[i])
				return false;
		return true;
	}
	return true;
}

/* CMD23 arg bits 31 reliable write, 30 packed, 29 tag request, 24 forced programming. */
#define HI6250_SBC_WRITE_BITS	(BIT(31) | BIT(30) | BIT(29) | BIT(24))

static bool hi6250_sbc_blocked(struct mmc_request *mrq)
{
	if (!mrq->sbc)
		return false;
	if (mrq->sbc->opcode != MMC_SET_BLOCK_COUNT || (mrq->sbc->arg & HI6250_SBC_WRITE_BITS))
		return true;
	return !mrq->cmd->data || !(mrq->cmd->data->flags & MMC_DATA_READ) ||
	       (mrq->cmd->opcode != MMC_READ_MULTIPLE_BLOCK && mrq->cmd->opcode != MMC_READ_SINGLE_BLOCK);
}

static void hi6250_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct hi6250_sdhci *priv = sdhci_pltfm_priv(pltfm_host);
	struct mmc_command *bad = NULL;

	if (hi6250_cmd_blocked(mrq->sbc) || hi6250_sbc_blocked(mrq))
		bad = mrq->sbc;
	else if (hi6250_cmd_blocked(mrq->cmd) || mrq->cmd->opcode == MMC_SET_BLOCK_COUNT)
		bad = mrq->cmd;
	else if (mrq->stop && mrq->stop->opcode != MMC_STOP_TRANSMISSION)
		bad = mrq->stop;

	if (bad) {
		dev_err_ratelimited(mmc_dev(mmc),
				    "BLOCKED CMD%u arg=0x%08x%s (read-only eMMC host)\n",
				    bad->opcode, bad->arg,
				    bad->data && (bad->data->flags & MMC_DATA_WRITE) ? " data-write" : "");
		mrq->cmd->error = -EROFS;
		mrq->cmd->retries = 0;
		if (mrq->sbc)
			mrq->sbc->error = mrq->sbc == bad ? -EROFS : 0;
		if (mrq->stop)
			mrq->stop->error = mrq->stop == bad ? -EROFS : 0;
		if (mrq->data) {
			mrq->data->error = -EROFS;
			mrq->data->bytes_xfered = 0;
		}
		mmc_request_done(mmc, mrq);
		return;
	}

	priv->sdhci_request(mmc, mrq);
}

static void hi6250_phy_init(struct sdhci_host *host)
{
	u32 reg;
	int n;

	reg = sdhci_readl(host, HI6250_PHY_CTRL1);
	reg |= HI6250_PHY_CTRL1_EN_RTRIM;
	reg &= ~HI6250_PHY_CTRL1_ICP_MASK;
	reg |= 0x4 << HI6250_PHY_CTRL1_ICP_SHIFT;
	sdhci_writel(host, reg, HI6250_PHY_CTRL1);

	reg = sdhci_readl(host, HI6250_PHY_CTRL1);
	sdhci_writel(host, reg | HI6250_PHY_CTRL1_PDB, HI6250_PHY_CTRL1);

	for (n = 100; n; n--) {
		if (sdhci_readl(host, HI6250_PHY_STATUS) & HI6250_PHY_STATUS_CALDONE)
			break;
		udelay(1);
	}
	if (!n)
		dev_err(mmc_dev(host->mmc), "PHY CALDONE timeout\n");

	for (n = 100; n; n--) {
		if (!(sdhci_readl(host, HI6250_PHY_STATUS) & HI6250_PHY_STATUS_EXR_NINST))
			break;
		udelay(1);
	}
	if (!n)
		dev_err(mmc_dev(host->mmc), "PHY EXR_NINST timeout\n");

	/* Open drain on CMD and DAT[7:0] during identification. */
	reg = sdhci_readl(host, HI6250_PHY_CTRL1);
	sdhci_writel(host, reg | HI6250_PHY_CTRL1_ODEN_CMD, HI6250_PHY_CTRL1);
	sdhci_writel(host, 0xff << HI6250_PHY_CTRL2_ODEN_DAT_SHIFT, HI6250_PHY_CTRL2);
}

/* Vendor sdhci_of_arasan_PHY_set_dll(host, 0xff): DLL pulsed off, on, off. */
static void hi6250_phy_dll_off(struct sdhci_host *host)
{
	u32 reg;

	reg = sdhci_readl(host, HI6250_PHY_CTRL1) & ~HI6250_PHY_CTRL1_EN_DLL;
	sdhci_writel(host, reg, HI6250_PHY_CTRL1);
	udelay(10);
	sdhci_writel(host, reg | HI6250_PHY_CTRL1_EN_DLL, HI6250_PHY_CTRL1);
	udelay(10);
	sdhci_writel(host, reg, HI6250_PHY_CTRL1);
}

/* Vendor sdhci_of_arasan_update_phy_control(), legacy and MMC_TIMING_MMC_HS branches. */
static void hi6250_update_phy(struct sdhci_host *host, struct mmc_ios *ios)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct hi6250_sdhci *priv = sdhci_pltfm_priv(pltfm_host);
	u32 reg, drv = 0;

	if (host->clock < 50000000) {
		if (host->clock && priv->old_bus_mode != MMC_BUSMODE_PUSHPULL &&
		    ios->bus_mode == MMC_BUSMODE_PUSHPULL) {
			reg = sdhci_readl(host, HI6250_PHY_CTRL1);
			sdhci_writel(host, reg & ~HI6250_PHY_CTRL1_ODEN_CMD, HI6250_PHY_CTRL1);
			sdhci_writel(host, HI6250_PHY_CTRL2_PUSHPULL, HI6250_PHY_CTRL2);
		}
		priv->old_bus_mode = ios->bus_mode;
	}

	if (ios->timing == MMC_TIMING_MMC_HS)
		drv = priv->host_drv;
	else if (ios->timing != MMC_TIMING_LEGACY)
		dev_warn_once(mmc_dev(host->mmc), "unsupported timing %u, using legacy PHY setup\n",
			      ios->timing);

	/* tx and rx tap delays off, strobe untouched (HS400 only) */
	reg = sdhci_readl(host, HI6250_PHY_CTRL3);
	reg &= ~(HI6250_PHY_CTRL3_OTAPDLYENA | HI6250_PHY_CTRL3_ITAPDLYENA);
	sdhci_writel(host, reg, HI6250_PHY_CTRL3);

	reg = sdhci_readl(host, HI6250_PHY_CTRL1) & ~HI6250_PHY_CTRL1_DR_TY_MASK;
	reg |= (drv << HI6250_PHY_CTRL1_DR_TY_SHIFT) & HI6250_PHY_CTRL1_DR_TY_MASK;
	sdhci_writel(host, reg, HI6250_PHY_CTRL1);

	hi6250_phy_dll_off(host);
	sdhci_writew(host, 0, HI6250_ENHANCED_STROBE);
}

static void hi6250_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct hi6250_sdhci *priv = sdhci_pltfm_priv(pltfm_host);

	priv->sdhci_set_ios(mmc, ios);
	hi6250_update_phy(host, ios);
}

static void hi6250_set_clock(struct sdhci_host *host, unsigned int clock)
{
	sdhci_set_clock(host, clock);
	/* vendor: wait 8 card clocks (20 us at 400 kHz) before the next command */
	if (clock)
		mdelay(1);
}

/*
 * SD bus power, as the vendor kernel: its sdhci_set_power() (CONFIG_HISI_MMC) calls
 * sdhci_set_power_noreg() after the vmmc regulator, and SDHCI_QUIRK2_USE_1_8_V_VMMC makes that
 * SDHCI_POWER_180 | SDHCI_POWER_ON (0x0b). Caps advertise 1.8 V only (0x44ecc881, bit 26).
 * Without it the controller never shifts a command out (E1: Power 0x00, CMD inhibit stuck,
 * Int stat 0, zero IRQs). This is a host register; card VCC (PMU LDO15) is not touched and no
 * regulator is used.
 */
static void hi6250_set_power(struct sdhci_host *host, unsigned char mode, unsigned short vdd)
{
	sdhci_set_power_noreg(host, mode, ilog2(MMC_VDD_165_195));
}

/*
 * Safety net (S1: 329 "Got data interrupt 0x00000002 even though no data operation was in
 * progress" + register dumps, all right after CMD12, which STOP_WITH_TC now consumes). A
 * Transfer Complete with no data and no busy command pending is acknowledged here and counted,
 * with a rate-limited one-line note instead of the core's full dump on every occurrence: a printk
 * storm drawn on fbcon in hard-IRQ context must not starve the system again. Nothing else in
 * intmask is touched; any other unexpected interrupt still reaches the core's handling.
 */
static u32 hi6250_irq(struct sdhci_host *host, u32 intmask)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct hi6250_sdhci *priv = sdhci_pltfm_priv(pltfm_host);

	if ((intmask & SDHCI_INT_DATA_END) && !host->data && !host->data_cmd &&
	    !host->pending_reset) {
		sdhci_writel(host, SDHCI_INT_DATA_END, SDHCI_INT_STATUS);
		priv->stray_data_end++;
		dev_warn_ratelimited(mmc_dev(host->mmc),
				     "stray Transfer Complete (no data/busy cmd), %u so far, last cmd 0x%04x\n",
				     priv->stray_data_end, sdhci_readw(host, SDHCI_COMMAND));
		intmask &= ~SDHCI_INT_DATA_END;
	}
	return intmask;
}

static unsigned int hi6250_get_min_clock(struct sdhci_host *host)
{
	return HI6250_MIN_FREQ;
}

static unsigned int hi6250_get_timeout_clock(struct sdhci_host *host)
{
	return sdhci_pltfm_clk_get_max_clock(host);
}

static const struct sdhci_ops hi6250_sdhci_ops = {
	.irq = hi6250_irq,
	.set_clock = hi6250_set_clock,
	.set_power = hi6250_set_power,
	.get_max_clock = sdhci_pltfm_clk_get_max_clock,
	.get_min_clock = hi6250_get_min_clock,
	.get_timeout_clock = hi6250_get_timeout_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data hi6250_sdhci_pdata = {
	.ops = &hi6250_sdhci_ops,
	.quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN | SDHCI_QUIRK_BROKEN_TIMEOUT_VAL,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN | SDHCI_QUIRK2_BROKEN_HS200 |
		   SDHCI_QUIRK2_BROKEN_DDR50 | SDHCI_QUIRK2_BROKEN_64_BIT_DMA |
		   SDHCI_QUIRK2_HOST_NO_CMD23 |	/* no CMD23: the core then registers no RPMB part */
		   SDHCI_QUIRK2_STOP_WITH_TC,	/* Arasan: Transfer Complete also ends CMD12 */
};

static int hi6250_sdhci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rst;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_host *host;
	struct hi6250_sdhci *priv;
	unsigned long xin_rate;
	u32 drv[2];
	int ret;

	host = sdhci_pltfm_init(pdev, &hi6250_sdhci_pdata, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);
	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);
	priv->old_bus_mode = MMC_BUSMODE_OPENDRAIN;
	priv->host_drv = HI6250_HOST_DRV_DEFAULT;
	if (!of_property_read_u32_array(dev->of_node, "hisilicon,drv-strength", drv, 2) &&
	    drv[0] <= 0x4)
		priv->host_drv = drv[0];

	/* Bootloader rates are kept: this driver never calls clk_set_rate(). */
	priv->clk_ahb = devm_clk_get_enabled(dev, "clk_ahb");
	if (IS_ERR(priv->clk_ahb))
		return dev_err_probe(dev, PTR_ERR(priv->clk_ahb), "clk_ahb\n");
	pltfm_host->clk = devm_clk_get_enabled(dev, "clk_xin");
	if (IS_ERR(pltfm_host->clk))
		return dev_err_probe(dev, PTR_ERR(pltfm_host->clk), "clk_xin\n");
	xin_rate = clk_get_rate(pltfm_host->clk);
	dev_info(dev, "clk_xin %lu Hz, clk_ahb %lu Hz (bootloader setting, not changed)\n",
		 xin_rate, clk_get_rate(priv->clk_ahb));
	if (!xin_rate || xin_rate > HI6250_MAX_XIN_RATE)
		return dev_err_probe(dev, -EINVAL, "implausible clk_xin rate %lu\n", xin_rate);

	/* Controller-only reset (PERI_CRG RSTEN4/RSTDIS4 bit 16, as the vendor probe). The card's
	 * RST_n is not touched. */
	rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "reset\n");
	if (rst) {
		reset_control_assert(rst);
		udelay(10);
		reset_control_deassert(rst);
		udelay(10);
	}

	hi6250_phy_init(host);

	ret = mmc_of_parse(host->mmc);
	if (ret)
		return ret;
	sdhci_get_of_property(pdev);

	host->mmc->caps2 |= MMC_CAP2_BOOTPART_NOACC | MMC_CAP2_NO_SD | MMC_CAP2_NO_SDIO;
	host->mmc->caps |= MMC_CAP_NONREMOVABLE;
	host->mmc->caps &= ~MMC_CAP_HW_RESET;

	/* 32-bit ADMA/SDMA only (RAM ends at 3 GiB): the vendor 64-bit enable bit must be off. */
	if (sdhci_readl(host, HI6250_CORE_CFG1) & HI6250_CORE_CFG1_64BIT) {
		dev_info(dev, "clearing CORE_CFG1 64-bit DMA enable\n");
		sdhci_writel(host, sdhci_readl(host, HI6250_CORE_CFG1) & ~HI6250_CORE_CFG1_64BIT,
			     HI6250_CORE_CFG1);
	}
	if (of_property_read_bool(dev->of_node, "hisilicon,no-dma"))
		host->quirks |= SDHCI_QUIRK_BROKEN_DMA | SDHCI_QUIRK_BROKEN_ADMA;

	ret = sdhci_setup_host(host);
	if (ret)
		return ret;

	host->mmc->caps &= ~(MMC_CAP_UHS | MMC_CAP_1_8V_DDR | MMC_CAP_1_2V_DDR | MMC_CAP_3_3V_DDR |
			     MMC_CAP_HW_RESET);
	host->mmc->caps2 &= ~(MMC_CAP2_HS200 | MMC_CAP2_HS400 | MMC_CAP2_HS400_ES |
			      MMC_CAP2_CQE | MMC_CAP2_CQE_DCMD);

	priv->sdhci_request = host->mmc_host_ops.request;
	priv->sdhci_set_ios = host->mmc_host_ops.set_ios;
	host->mmc_host_ops.request = hi6250_request;
	host->mmc_host_ops.set_ios = hi6250_set_ios;

	ret = __sdhci_add_host(host);
	if (ret) {
		sdhci_cleanup_host(host);
		return ret;
	}
	dev_info(dev, "read-only eMMC host: caps 0x%08x caps2 0x%08x f_max %u\n",
		 host->mmc->caps, host->mmc->caps2, host->mmc->f_max);
	return 0;
}

static const struct of_device_id hi6250_sdhci_of_match[] = {
	{ .compatible = "hisilicon,hi6250-sdhci" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6250_sdhci_of_match);

static struct platform_driver hi6250_sdhci_driver = {
	.driver = {
		.name = "sdhci-hi6250",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = hi6250_sdhci_of_match,
	},
	.probe = hi6250_sdhci_probe,
	.remove = sdhci_pltfm_remove,
};
module_platform_driver(hi6250_sdhci_driver);

MODULE_DESCRIPTION("HiSilicon Hi6250 eMMC host (read-only bring-up)");
MODULE_LICENSE("GPL");
