// SPDX-License-Identifier: GPL-2.0
/*
 * bach2_card: one-link ALSA card for the HI6402 on bach2 (docs/mainline-audio-port.md, C5/C6).
 *
 *   CPU DAI    "bach2-slimbus-dai": a no-op DAI like the stock slimbus_dai.c; SLIMbus port setup is
 *              done by the codec's DAPM ("S1 IL PGA" -> slimbus_track_activate(AUDIO_PLAY)).
 *   codec      hi6402-codec / "hi6402-audio-dai"  (DT "codec-handle", as the stock sound node)
 *   platform   hi3xxx-pcm-asp-dma                 (DT "platform-handle"): the vendor DSP-free PCM,
 *              ASP DMAC channels 5/6 into SLIMbus port FIFOs 0/1
 *
 * Replaces the stock machine driver hi6250_hi6402.c, whose links all go through the HiFi DSP
 * platform "hi6210-hifi". The mixer routes (S1 -> DAC -> S4 for the amps) are applied from
 * userspace with the stock mixer_paths (tools/mixer-path.py media-speaker).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

static const struct snd_soc_dai_ops bach2_slimbus_dai_ops = { };

static struct snd_soc_dai_driver bach2_slimbus_dai = {
	.name = "bach2-slimbus-dai",
	.playback = {
		.stream_name = "SLIMbus Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_384000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &bach2_slimbus_dai_ops,
};

static const struct snd_soc_component_driver bach2_slimbus_dai_component = {
	.name = "bach2-slimbus-dai",
};

SND_SOC_DAILINK_DEFS(bach2_pb,
	DAILINK_COMP_ARRAY(COMP_CPU("bach2-slimbus-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "hi6402-audio-dai")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));

static struct snd_soc_dai_link bach2_links[] = {
	{
		.name = "bach2-hi6402-pb",
		.stream_name = "bach2-hi6402-pb",
		.playback_only = 1,
		SND_SOC_DAILINK_REG(bach2_pb),
	},
};

static struct snd_soc_card bach2_card = {
	.name = "bach2-hi6402",
	.owner = THIS_MODULE,
	.dai_link = bach2_links,
	.num_links = ARRAY_SIZE(bach2_links),
};

static int bach2_card_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *codec, *plat;
	int r;

	codec = of_parse_phandle(dev->of_node, "codec-handle", 0);
	plat = of_parse_phandle(dev->of_node, "platform-handle", 0);
	if (!codec || !plat) {
		of_node_put(codec);
		of_node_put(plat);
		return dev_err_probe(dev, -EINVAL, "codec-handle/platform-handle missing\n");
	}
	bach2_links[0].codecs->of_node = codec;
	bach2_links[0].platforms->of_node = plat;

	r = devm_snd_soc_register_component(dev, &bach2_slimbus_dai_component,
					    &bach2_slimbus_dai, 1);
	if (r)
		return dev_err_probe(dev, r, "cpu dai\n");
	bach2_card.dev = dev;
	pr_crit("BACH2ML: AUD: bach2_card register_card enter\n");	/* breadcrumb */
	r = devm_snd_soc_register_card(dev, &bach2_card);
	pr_crit("BACH2ML: AUD: bach2_card register_card returned %d\n", r);
	if (r)
		return dev_err_probe(dev, r, "card\n");
	dev_info(dev, "card %s registered (codec %pOF, platform %pOF)\n", bach2_card.name, codec, plat);
	return 0;
}

static const struct of_device_id bach2_card_of_match[] = {
	{ .compatible = "bach2,hi6402-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, bach2_card_of_match);

static struct platform_driver bach2_card_driver = {
	.probe = bach2_card_probe,
	.driver = { .name = "bach2-hi6402-card", .of_match_table = bach2_card_of_match },
};
module_platform_driver(bach2_card_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("bach2 HI6402 one-link ALSA card (DSP-free playback)");
