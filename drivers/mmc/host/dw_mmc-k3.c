// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2013 Linaro Ltd.
 * Copyright (c) 2013 HiSilicon Limited.
 */

#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

/*
 * hi6220 sd only support io voltage 1.8v and 3v
 * Also need config AO_SCTRL_SEL18 accordingly
 */
#define AO_SCTRL_SEL18		BIT(10)
#define AO_SCTRL_CTRL3		0x40C

#define DWMMC_SDIO_ID 2

#define SOC_SCTRL_SCPERCTRL5    (0x314)
#define SDCARD_IO_SEL18         BIT(2)

#define SDCARD_RD_THRESHOLD  (512)

#define GENCLK_DIV (7)

#define GPIO_CLK_ENABLE                   BIT(16)
#define GPIO_CLK_DIV_MASK                 GENMASK(11, 8)
#define GPIO_USE_SAMPLE_DLY_MASK          GENMASK(13, 13)
#define UHS_REG_EXT_SAMPLE_PHASE_MASK     GENMASK(20, 16)
#define UHS_REG_EXT_SAMPLE_DRVPHASE_MASK  GENMASK(25, 21)
#define UHS_REG_EXT_SAMPLE_DLY_MASK       GENMASK(30, 26)

#define TIMING_MODE     3
#define TIMING_CFG_NUM 10

#define NUM_PHASES (40)

#define ENABLE_SHIFT_MIN_SMPL (4)
#define ENABLE_SHIFT_MAX_SMPL (12)

/*
 * The use_sample_dly window sits at the top of the 2 * (div + 1) sample
 * phases: 11..14 for div 7, 13..16 for div 8, 15..18 for div 9 (vendor 4.9
 * dw_mmc_hi3xxx.c dw_mci_shift_sdio_set(); the div 9 window is confirmed by
 * the stock 160 MHz tuning log, GPIO=0x12900 for samples 15..18 only).
 */
#define USE_DLY_MIN_SMPL(div) (2 * ((div) + 1) - 5)
#define USE_DLY_MAX_SMPL(div) (2 * ((div) + 1) - 2)

struct k3_priv {
	int ctrl_id;
	u32 cur_speed;
	/* hi6250: last applied request, so set_ios keeps a tuned phase */
	u32 cur_clock;
	u32 cur_timing;
	struct regmap	*reg;
};

static unsigned long dw_mci_hi6220_caps[] = {
	MMC_CAP_CMD23,
	MMC_CAP_CMD23,
	0
};

struct hs_timing {
	u32 drv_phase;
	u32 smpl_dly;
	u32 smpl_phase_max;
	u32 smpl_phase_min;
	u32 clk_div;	/* ciu divider in GPIO[11:8]; 0 means GENCLK_DIV */
};

static u32 hs_timing_clk_div(const struct hs_timing *t)
{
	return t->clk_div ? t->clk_div : GENCLK_DIV;
}

static struct hs_timing hs_timing_cfg[TIMING_MODE][TIMING_CFG_NUM] = {
	{ /* reserved */ },
	{ /* SD */
		{7, 0, 15, 15,},  /* 0: LEGACY 400k */
		{6, 0,  4,  4,},  /* 1: MMC_HS */
		{6, 0,  3,  3,},  /* 2: SD_HS */
		{6, 0, 15, 15,},  /* 3: SDR12 */
		{6, 0,  2,  2,},  /* 4: SDR25 */
		{4, 0, 11,  0,},  /* 5: SDR50 */
		{6, 4, 15,  0,},  /* 6: SDR104 */
		{0},              /* 7: DDR50 */
		{0},              /* 8: DDR52 */
		{0},              /* 9: HS200 */
	},
	{ /* SDIO */
		{7, 0, 15, 15,},  /* 0: LEGACY 400k */
		{0},              /* 1: MMC_HS */
		{6, 0, 15, 15,},  /* 2: SD_HS */
		{6, 0, 15, 15,},  /* 3: SDR12 */
		{6, 0,  0,  0,},  /* 4: SDR25 */
		{4, 0, 12,  0,},  /* 5: SDR50 */
		{5, 4, 15,  0,},  /* 6: SDR104 */
		{0},              /* 7: DDR50 */
		{0},              /* 8: DDR52 */
		{0},              /* 9: HS200 */
	}
};

static struct hs_timing hs_timing_cfg_hi6250[TIMING_MODE][TIMING_CFG_NUM] = {
	{},
	{ /*SD*/
		{7, 0, 15, 15, }, /* 0: LEGACY 400k */
		{0},					    /* 1: MMC_HS */
		{6, 0,  2,  2, }, /* 2: SD_HS */
		{6, 0, 15, 15, },	/* 3: SDR12 */
		{6, 0,  1,  1, },	/* 4: SDR25 */
		{3, 0, 12,  0, },	/* 5: SDR50 */
		{5, 4, 15,  0, },	/* 6: SDR104 */
		{0},					    /* 7: DDR50 */
		{0},					    /* 8: DDR52 */
		{0},					    /* 9: HS200 */
	},
	{ /*SDIO*/
		{7, 0, 15, 15, },	/* 0: LEGACY 400k */
		{0},					    /* 1: MMC_HS */
		{6, 0, 15, 15, },	/* 2: SD_HS */
		{6, 0, 15, 15, },	/* 3: SDR12 */
		{6, 0,  0,  0, },	/* 4: SDR25 */
		{5, 0, 12,  0, },	/* 5: SDR50 */
		/*
		 * 6: SDR104. Stock (DT wifi_sdio_sdr104_156M): ciu 1600 MHz
		 * (ppll3) / (9 + 1) = 160 MHz, samples 0..19. ppll3 needs an
		 * LPM3 IPC here, so use ppll0: 1440 MHz / (8 + 1) = 160 MHz,
		 * samples 0..2 * (8 + 1) - 1 = 17 (the vendor's div 8 entry,
		 * wifi_sdio_sdr104_177M, uses the same range). drv_phase and
		 * smpl_dly are the vendor values, unchanged across dividers.
		 */
		{5, 4, 17,  0, 8, },
		{0},					    /* 7: DDR50 */
		{0},					    /* 8: DDR52 */
		{0},					    /* 9: HS200 */
	}
};

static void dw_mci_k3_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	int ret;

	ret = clk_set_rate(host->ciu_clk, ios->clock);
	if (ret)
		dev_warn(host->dev, "failed to set rate %uHz\n", ios->clock);

	host->bus_hz = clk_get_rate(host->ciu_clk);
}

static const struct dw_mci_drv_data k3_drv_data = {
	.set_ios		= dw_mci_k3_set_ios,
};

static int dw_mci_hi6220_parse_dt(struct dw_mci *host)
{
	struct k3_priv *priv;

	priv = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reg = syscon_regmap_lookup_by_phandle(host->dev->of_node,
					 "hisilicon,peripheral-syscon");
	if (IS_ERR(priv->reg))
		priv->reg = NULL;

	priv->ctrl_id = of_alias_get_id(host->dev->of_node, "mshc");
	if (priv->ctrl_id < 0)
		priv->ctrl_id = 0;

	if (priv->ctrl_id >= TIMING_MODE)
		return -EINVAL;

	host->priv = priv;
	return 0;
}

static int dw_mci_hi6220_switch_voltage(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct k3_priv *priv;
	struct dw_mci *host;
	int min_uv, max_uv;
	int ret;

	host = slot->host;
	priv = host->priv;

	if (!priv || !priv->reg)
		return 0;

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		ret = regmap_update_bits(priv->reg, AO_SCTRL_CTRL3,
					 AO_SCTRL_SEL18, 0);
		min_uv = 3000000;
		max_uv = 3000000;
	} else if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180) {
		ret = regmap_update_bits(priv->reg, AO_SCTRL_CTRL3,
					 AO_SCTRL_SEL18, AO_SCTRL_SEL18);
		min_uv = 1800000;
		max_uv = 1800000;
	} else {
		dev_dbg(host->dev, "voltage not supported\n");
		return -EINVAL;
	}

	if (ret) {
		dev_dbg(host->dev, "switch voltage failed\n");
		return ret;
	}

	if (IS_ERR_OR_NULL(mmc->supply.vqmmc))
		return 0;

	ret = regulator_set_voltage(mmc->supply.vqmmc, min_uv, max_uv);
	if (ret) {
		dev_dbg(host->dev, "Regulator set error %d: %d - %d\n",
				 ret, min_uv, max_uv);
		return ret;
	}

	return 0;
}

static void dw_mci_hi6220_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	int ret;
	unsigned int clock;

	clock = (ios->clock <= 25000000) ? 25000000 : ios->clock;

	ret = clk_set_rate(host->biu_clk, clock);
	if (ret)
		dev_warn(host->dev, "failed to set rate %uHz\n", clock);

	host->bus_hz = clk_get_rate(host->biu_clk);
}

static int dw_mci_hi6220_execute_tuning(struct dw_mci_slot *slot, u32 opcode)
{
	return 0;
}

static const struct dw_mci_drv_data hi6220_data = {
	.caps			= dw_mci_hi6220_caps,
	.num_caps		= ARRAY_SIZE(dw_mci_hi6220_caps),
	.switch_voltage		= dw_mci_hi6220_switch_voltage,
	.set_ios		= dw_mci_hi6220_set_ios,
	.parse_dt		= dw_mci_hi6220_parse_dt,
	.execute_tuning		= dw_mci_hi6220_execute_tuning,
};

static void dw_mci_hs_set_timing(struct dw_mci *host, int timing,
				     int smpl_phase, struct hs_timing (*timings)[TIMING_CFG_NUM])
{
	u32 drv_phase;
	u32 smpl_dly;
	u32 use_smpl_dly = 0;
	u32 enable_shift = 0;
	u32 reg_value;
	u32 clk_div;
	int ctrl_id;
	struct k3_priv *priv;

	priv = host->priv;
	ctrl_id = priv->ctrl_id;

	drv_phase = timings[ctrl_id][timing].drv_phase;
	smpl_dly   = timings[ctrl_id][timing].smpl_dly;
	clk_div = hs_timing_clk_div(&timings[ctrl_id][timing]);
	if (smpl_phase == -1)
		smpl_phase = (timings[ctrl_id][timing].smpl_phase_max +
			     timings[ctrl_id][timing].smpl_phase_min) / 2;

	switch (timing) {
	case MMC_TIMING_UHS_SDR104:
		if (smpl_phase >= USE_DLY_MIN_SMPL(clk_div) &&
				smpl_phase <= USE_DLY_MAX_SMPL(clk_div))
			use_smpl_dly = 1;
		fallthrough;
	case MMC_TIMING_UHS_SDR50:
		if (smpl_phase >= ENABLE_SHIFT_MIN_SMPL &&
				smpl_phase <= ENABLE_SHIFT_MAX_SMPL)
			enable_shift = 1;
		break;
	}

	mci_writel(host, GPIO, 0x0);
	usleep_range(5, 10);

	reg_value = FIELD_PREP(UHS_REG_EXT_SAMPLE_PHASE_MASK, smpl_phase) |
		    FIELD_PREP(UHS_REG_EXT_SAMPLE_DLY_MASK, smpl_dly) |
		    FIELD_PREP(UHS_REG_EXT_SAMPLE_DRVPHASE_MASK, drv_phase);
	mci_writel(host, UHS_REG_EXT, reg_value);

	mci_writel(host, ENABLE_SHIFT, enable_shift);

	reg_value = FIELD_PREP(GPIO_CLK_DIV_MASK, clk_div) |
			     FIELD_PREP(GPIO_USE_SAMPLE_DLY_MASK, use_smpl_dly);
	mci_writel(host, GPIO, (unsigned int)reg_value | GPIO_CLK_ENABLE);

	/* We should delay 1ms wait for timing setting finished. */
	usleep_range(1000, 2000);
}


static int dw_mci_hi3660_init(struct dw_mci *host)
{
	mci_writel(host, CDTHRCTL, SDMMC_SET_THLD(SDCARD_RD_THRESHOLD,
		    SDMMC_CARD_RD_THR_EN));

	dw_mci_hs_set_timing(host, MMC_TIMING_LEGACY, -1, hs_timing_cfg);
	host->bus_hz /= (GENCLK_DIV + 1);

	return 0;
}

static int dw_mci_set_sel18(struct dw_mci *host, bool set)
{
	int ret;
	unsigned int val;
	struct k3_priv *priv;

	priv = host->priv;

	val = set ? SDCARD_IO_SEL18 : 0;
	ret = regmap_update_bits(priv->reg, SOC_SCTRL_SCPERCTRL5,
				 SDCARD_IO_SEL18, val);
	if (ret) {
		dev_err(host->dev, "sel18 %u error\n", val);
		return ret;
	}

	return 0;
}

static void dw_mci_hi3660_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	int ret;
	unsigned long wanted;
	unsigned long actual;
	struct k3_priv *priv = host->priv;

	if (!ios->clock || ios->clock == priv->cur_speed)
		return;

	wanted = ios->clock * (GENCLK_DIV + 1);
	ret = clk_set_rate(host->ciu_clk, wanted);
	if (ret) {
		dev_err(host->dev, "failed to set rate %luHz\n", wanted);
		return;
	}
	actual = clk_get_rate(host->ciu_clk);

	dw_mci_hs_set_timing(host, ios->timing, -1, hs_timing_cfg);
	host->bus_hz = actual / (GENCLK_DIV + 1);
	host->current_speed = 0;
	priv->cur_speed = host->bus_hz;
}

static int dw_mci_get_best_clksmpl(unsigned int sample_flag)
{
	int i;
	int interval;
	unsigned int v;
	unsigned int len;
	unsigned int range_start = 0;
	unsigned int range_length = 0;
	unsigned int middle_range = 0;

	if (!sample_flag)
		return -EIO;

	if (~sample_flag == 0)
		return 0;

	i = ffs(sample_flag) - 1;

	/*
	* A clock cycle is divided into 32 phases,
	* each of which is represented by a bit,
	* finding the optimal phase.
	*/
	while (i < 32) {
		v = ror32(sample_flag, i);
		len = ffs(~v) - 1;

		if (len > range_length) {
			range_length = len;
			range_start = i;
		}

		interval = ffs(v >> len) - 1;
		if (interval < 0)
			break;

		i += len + interval;
	}

	middle_range = range_start + range_length / 2;
	if (middle_range >= 32)
		middle_range %= 32;

	return middle_range;
}

static int dw_mci_hi3660_execute_tuning(struct dw_mci_slot *slot, u32 opcode)
{
	int i = 0;
	struct dw_mci *host = slot->host;
	struct mmc_host *mmc = slot->mmc;
	int smpl_phase = 0;
	u32 tuning_sample_flag = 0;
	int best_clksmpl = 0;

	for (i = 0; i < NUM_PHASES; ++i, ++smpl_phase) {
		smpl_phase %= 32;

		mci_writel(host, TMOUT, ~0);
		dw_mci_hs_set_timing(host, mmc->ios.timing, smpl_phase, hs_timing_cfg);

		if (!mmc_send_tuning(mmc, opcode, NULL))
			tuning_sample_flag |= (1 << smpl_phase);
		else
			tuning_sample_flag &= ~(1 << smpl_phase);
	}

	best_clksmpl = dw_mci_get_best_clksmpl(tuning_sample_flag);
	if (best_clksmpl < 0) {
		dev_err(host->dev, "All phases bad!\n");
		return -EIO;
	}

	dw_mci_hs_set_timing(host, mmc->ios.timing, best_clksmpl, hs_timing_cfg);

	dev_info(host->dev, "tuning ok best_clksmpl %u tuning_sample_flag %x\n",
		 best_clksmpl, tuning_sample_flag);
	return 0;
}

static int dw_mci_hi3660_switch_voltage(struct mmc_host *mmc,
					struct mmc_ios *ios)
{
	int ret = 0;
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct k3_priv *priv;
	struct dw_mci *host;

	host = slot->host;
	priv = host->priv;

	if (!priv || !priv->reg)
		return 0;

	if (priv->ctrl_id == DWMMC_SDIO_ID)
		return 0;

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330)
		ret = dw_mci_set_sel18(host, 0);
	else if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)
		ret = dw_mci_set_sel18(host, 1);
	if (ret)
		return ret;

	if (!IS_ERR(mmc->supply.vqmmc)) {
		ret = mmc_regulator_set_vqmmc(mmc, ios);
		if (ret < 0) {
			dev_err(host->dev, "Regulator set error %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static void dw_mci_hi6250_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	int ret;
	unsigned long wanted;
	unsigned long actual;
	u32 clk_div;
	struct k3_priv *priv = host->priv;

	/*
	 * Compare against the request, not the rounded bus_hz, so an unchanged
	 * clock + timing leaves the registers (and a tuned phase) alone.
	 */
	if (!ios->clock || (ios->clock == priv->cur_clock &&
			    ios->timing == priv->cur_timing))
		return;

	if (ios->timing >= TIMING_CFG_NUM) {
		dev_err(host->dev, "unsupported timing %u\n", ios->timing);
		return;
	}

	clk_div = hs_timing_clk_div(&hs_timing_cfg_hi6250[priv->ctrl_id][ios->timing]);
	wanted = ios->clock * (clk_div + 1);
	ret = clk_set_rate(host->ciu_clk, wanted);
	if (ret) {
		dev_err(host->dev, "failed to set rate %luHz\n", wanted);
		return;
	}
	actual = clk_get_rate(host->ciu_clk);

	dw_mci_hs_set_timing(host, ios->timing, -1, hs_timing_cfg_hi6250);
	host->bus_hz = actual / (clk_div + 1);
	host->current_speed = 0;
	priv->cur_speed = host->bus_hz;
	priv->cur_clock = ios->clock;
	priv->cur_timing = ios->timing;
}

/*
 * Vendor 4.9 dw_mci_hs_tuning_find_condition(): odd window lengths from the
 * whole range down to 1; for each length, start positions from the centre
 * outwards, stopping at the first one out of range; return the centre of
 * the first fully passing window. Stock 160 MHz: flags 0xfbfff over 0..19
 * gives sample 7, as logged ("tuning sample = 7").
 */
static int dw_mci_hi6250_best_clksmpl(u32 flags, int min, int max)
{
	int len, start, i, j;
	u32 mask;

	for (len = (((max - min) >> 1) << 1) + 1; len >= 1; len -= 2) {
		mask = GENMASK(len - 1, 0);
		start = (min + max - len + 1) / 2;
		for (i = start, j = 1; i >= min && i <= max - len + 1;
		     i = start + ((j % 2) ? -1 : 1) * (j / 2)) {
			if ((flags & (mask << i)) == (mask << i))
				return i + len / 2;
			j++;
		}
	}

	return -EIO;
}

static int dw_mci_hi6250_execute_tuning(struct dw_mci_slot *slot, u32 opcode)
{
	struct dw_mci *host = slot->host;
	struct mmc_host *mmc = slot->mmc;
	struct k3_priv *priv = host->priv;
	const struct hs_timing *t;
	u32 tuning_sample_flag = 0;
	int smpl_phase;
	int best_clksmpl;

	if (mmc->ios.timing >= TIMING_CFG_NUM)
		return -EINVAL;
	t = &hs_timing_cfg_hi6250[priv->ctrl_id][mmc->ios.timing];

	for (smpl_phase = t->smpl_phase_min;
	     smpl_phase <= t->smpl_phase_max; smpl_phase++) {
		mci_writel(host, TMOUT, ~0);
		dw_mci_hs_set_timing(host, mmc->ios.timing, smpl_phase,
				     hs_timing_cfg_hi6250);

		if (!mmc_send_tuning(mmc, opcode, NULL))
			tuning_sample_flag |= BIT(smpl_phase);
	}

	best_clksmpl = dw_mci_hi6250_best_clksmpl(tuning_sample_flag,
						  t->smpl_phase_min,
						  t->smpl_phase_max);
	if (best_clksmpl < 0) {
		dev_err(host->dev,
			"All phases bad! timing %u samples %u..%u tuning_sample_flag %x\n",
			mmc->ios.timing, t->smpl_phase_min, t->smpl_phase_max,
			tuning_sample_flag);
		return -EIO;
	}

	dw_mci_hs_set_timing(host, mmc->ios.timing, best_clksmpl, hs_timing_cfg_hi6250);

	dev_info(host->dev,
		 "tuning ok: timing %u div %u samples %u..%u best_clksmpl %d tuning_sample_flag %x\n",
		 mmc->ios.timing, hs_timing_clk_div(t), t->smpl_phase_min,
		 t->smpl_phase_max, best_clksmpl, tuning_sample_flag);
	return 0;
}


static int dw_mci_hi6250_switch_voltage(struct mmc_host *mmc,
					struct mmc_ios *ios)
{
	int ret = 0;
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct k3_priv *priv;
	struct dw_mci *host;

	host = slot->host;
	priv = host->priv;

	if (!priv || !priv->reg)
		return 0;

	/*
	 * SDIO pads: CMD11 only, nothing to switch. Stock does the same: the
	 * vendor dw_mci_priv_voltage_switch() returns early for every host but
	 * the SD slot, mmc2 has no vqmmc supply ("using dummy regulator"), and
	 * the stock boot log has no dw_mci_set_sel18 line around mmc2's CMD11.
	 */
	if (priv->ctrl_id == DWMMC_SDIO_ID)
		return 0;

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330)
		ret = dw_mci_set_sel18(host, 0);
	else if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)
		ret = dw_mci_set_sel18(host, 1);
	if (ret)
		return ret;

	if (!IS_ERR(mmc->supply.vqmmc)) {
		ret = mmc_regulator_set_vqmmc(mmc, ios);
		if (ret < 0) {
			dev_err(host->dev, "Regulator set error %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static const struct dw_mci_drv_data hi6250_data = {
	.init = dw_mci_hi3660_init,
	.set_ios = dw_mci_hi6250_set_ios,
	.parse_dt = dw_mci_hi6220_parse_dt,
	.execute_tuning = dw_mci_hi6250_execute_tuning,
	.switch_voltage  = dw_mci_hi6250_switch_voltage,
};

static const struct dw_mci_drv_data hi3660_data = {
	.init = dw_mci_hi3660_init,
	.set_ios = dw_mci_hi3660_set_ios,
	.parse_dt = dw_mci_hi6220_parse_dt,
	.execute_tuning = dw_mci_hi3660_execute_tuning,
	.switch_voltage  = dw_mci_hi3660_switch_voltage,
};

static const struct of_device_id dw_mci_k3_match[] = {
	{ .compatible = "hisilicon,hi3660-dw-mshc", .data = &hi3660_data, },
	{ .compatible = "hisilicon,hi4511-dw-mshc", .data = &k3_drv_data, },
	{ .compatible = "hisilicon,hi6220-dw-mshc", .data = &hi6220_data, },
	{ .compatible = "hisilicon,hi6250-dw-mshc", .data = &hi6250_data, },
	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_k3_match);

static int dw_mci_k3_probe(struct platform_device *pdev)
{
	const struct dw_mci_drv_data *drv_data;
	const struct of_device_id *match;

	match = of_match_node(dw_mci_k3_match, pdev->dev.of_node);
	drv_data = match->data;

	return dw_mci_pltfm_register(pdev, drv_data);
}

static const struct dev_pm_ops dw_mci_k3_dev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(dw_mci_runtime_suspend, dw_mci_runtime_resume, NULL)
};

static struct platform_driver dw_mci_k3_pltfm_driver = {
	.probe		= dw_mci_k3_probe,
	.remove		= dw_mci_pltfm_remove,
	.driver		= {
		.name		= "dwmmc_k3",
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table	= dw_mci_k3_match,
		.pm		= pm_ptr(&dw_mci_k3_dev_pm_ops),
	},
};

module_platform_driver(dw_mci_k3_pltfm_driver);

MODULE_DESCRIPTION("K3 Specific DW-MSHC Driver Extension");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:dwmmc_k3");
