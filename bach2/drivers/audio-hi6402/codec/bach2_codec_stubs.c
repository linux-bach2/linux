// SPDX-License-Identifier: GPL-2.0
/*
 * bach2: stubs for vendor parts that are not ported (docs/mainline-audio-port.md). Playback to the
 * speakers needs none of them:
 *   - MBHC (headset/button detection, hi64xx_mbhc.c): no headset support yet
 *   - VAD/MAD (voice activity, hi64xx_vad.c) and the codec-DSP config (hi64xx_dsp): soundtrigger
 *   - hifi_misc peri lock: serialises against the codec DSP, which is never started here
 * Each init succeeds without doing anything; the MBHC pointer stays NULL.
 */
#include <linux/module.h>
#include <sound/soc.h>
#include <linux/hisi/hi64xx/hi64xx_mbhc.h>
#include <linux/hisi/hi64xx/hi64xx_vad.h>
#include <linux/hisi/hi64xx_hifi_misc.h>
#include "hi6402_hifi_config.h"

int hi64xx_mbhc_init(struct snd_soc_codec *codec, struct device_node *node,
		     struct hi64xx_hs_cfg *hi64xx_hs_cfg, struct hi64xx_resmgr *resmgr,
		     struct hi64xx_irq *irqmgr, struct hi64xx_mbhc **mbhc)
{
	*mbhc = NULL;
	dev_info(codec->dev, "bach2: MBHC (headset detection) not ported\n");
	return 0;
}

void hi64xx_mbhc_deinit(struct hi64xx_mbhc *mbhc) { }
void hi64xx_irq_mask_btn_irqs(struct hi64xx_mbhc *mbhc) { }
void hi64xx_irq_unmask_btn_irqs(struct hi64xx_mbhc *mbhc) { }

bool hi64xx_check_saradc_ready_detect(struct snd_soc_codec *codec)
{
	return false;
}

int hi64xx_vad_init(struct snd_soc_codec *codec, struct hi64xx_irq *irq)
{
	return 0;
}

int hi64xx_vad_deinit(void)
{
	return 0;
}

int hi6402_hifi_config_init(struct snd_soc_codec *codec, struct hi64xx_resmgr *resmgr,
			    struct hi64xx_irq *irqmgr, enum bustype_select bus_sel)
{
	dev_info(codec->dev, "bach2: codec DSP (hi64xx_dsp) not ported\n");
	return 0;
}

void hi6402_hifi_config_deinit(void) { }
void hi64xx_hifi_misc_peri_lock(void) { }
void hi64xx_hifi_misc_peri_unlock(void) { }
