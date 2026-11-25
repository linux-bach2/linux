/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ASoC 4.4 -> 7.0 shims for the vendor HI6402 codec (docs/mainline-audio-port.md): the 4.4
 * struct snd_soc_codec became struct snd_soc_component. Pure renames; the codec registration
 * struct is converted in the source (see the bach2 marker in hi6402.c).
 */
#ifndef BACH2_ASOC_COMPAT_H
#define BACH2_ASOC_COMPAT_H
#include <sound/soc.h>

#define snd_soc_codec			snd_soc_component
#define snd_soc_codec_get_drvdata	snd_soc_component_get_drvdata
#define snd_soc_codec_set_drvdata	snd_soc_component_set_drvdata
#define snd_soc_dapm_to_codec		snd_soc_dapm_to_component
#define snd_soc_codec_get_dapm		snd_soc_component_get_dapm
/*
 * 4.4 snd_soc_read/snd_soc_write called the codec's .read/.write without any lock
 * (sound/soc/soc-io.c in the 4.4 tree); only snd_soc_update_bits took component->io_mutex.
 * 7.0 snd_soc_component_read/write take io_mutex. The vendor HI6402 code calls snd_soc_write from
 * inside its own .write callback (hi6402_reg_write -> hi64xx_resmgr_request_reg_access ->
 * PLL turn-on -> snd_soc_write), so mapping onto the 7.0 locked calls self-deadlocks on io_mutex
 * at the first PLL-gated register (C5: bach2_card insmod hung in the component probe). Keep the
 * 4.4 semantics: call the callbacks directly. Accesses issued by the 7.0 core (kcontrols, DAPM)
 * still serialise on io_mutex, as 4.4's update_bits did.
 */
static inline unsigned int bach2_soc_read(struct snd_soc_component *c, unsigned int r)
{
	if (!c->driver->read) {
		dev_err(c->dev, "bach2: no .read for reg 0x%x\n", r);
		return 0;
	}
	return c->driver->read(c, r);
}

static inline int bach2_soc_write(struct snd_soc_component *c, unsigned int r, unsigned int v)
{
	if (!c->driver->write) {
		dev_err(c->dev, "bach2: no .write for reg 0x%x\n", r);
		return -EIO;
	}
	return c->driver->write(c, r, v);
}
#define snd_soc_read(c, r)		bach2_soc_read((c), (r))
#define snd_soc_write(c, r, v)		bach2_soc_write((c), (r), (v))
#define snd_soc_update_bits(c, r, m, v)	snd_soc_component_update_bits((c), (r), (m), (v))
#endif
