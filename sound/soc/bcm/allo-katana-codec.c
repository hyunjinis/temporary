/*
 * Driver for the ALLO KATANA CODEC
 *
 * Author: Jaikumar <jaikumar@cem-solutions.net>
 *		Copyright 2018
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/gcd.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <linux/i2c.h>


#define KATANA_CODEC_CHIP_ID		0x30
#define KATANA_CODEC_VIRT_BASE		0x100
#define KATANA_CODEC_PAGE		0

#define KATANA_CODEC_CHIP_ID_REG	(KATANA_CODEC_VIRT_BASE + 0)
#define KATANA_CODEC_RESET		(KATANA_CODEC_VIRT_BASE + 1)
#define KATANA_CODEC_VOLUME_1		(KATANA_CODEC_VIRT_BASE + 2)
#define KATANA_CODEC_VOLUME_2		(KATANA_CODEC_VIRT_BASE + 3)
#define KATANA_CODEC_MUTE		(KATANA_CODEC_VIRT_BASE + 4)
#define KATANA_CODEC_DSP_PROGRAM	(KATANA_CODEC_VIRT_BASE + 5)
#define KATANA_CODEC_DEEMPHASIS		(KATANA_CODEC_VIRT_BASE + 6)
#define KATANA_CODEC_DOP		(KATANA_CODEC_VIRT_BASE + 7)
#define KATANA_CODEC_FORMAT		(KATANA_CODEC_VIRT_BASE + 8)
#define KATANA_CODEC_COMMAND		(KATANA_CODEC_VIRT_BASE + 9)
#define KATANA_CODEC_MUTE_STREAM	(KATANA_CODEC_VIRT_BASE + 10)

#define KATANA_CODEC_MAX_REGISTER	(KATANA_CODEC_VIRT_BASE + 10)

#define KATANA_CODEC_FMT		0xff
#define KATANA_CODEC_CHAN_MONO		0x00
#define KATANA_CODEC_CHAN_STEREO	0x80
#define KATANA_CODEC_ALEN_16		0x10
#define KATANA_CODEC_ALEN_24		0x20
#define KATANA_CODEC_ALEN_32		0x30
#define KATANA_CODEC_RATE_11025		0x01
#define KATANA_CODEC_RATE_22050		0x02
#define KATANA_CODEC_RATE_32000		0x03
#define KATANA_CODEC_RATE_44100		0x04
#define KATANA_CODEC_RATE_48000		0x05
#define KATANA_CODEC_RATE_88200		0x06
#define KATANA_CODEC_RATE_96000		0x07
#define KATANA_CODEC_RATE_176400	0x08
#define KATANA_CODEC_RATE_192000	0x09
#define KATANA_CODEC_RATE_352800	0x0a
#define KATANA_CODEC_RATE_384000	0x0b


struct katana_codec_priv {
	struct regmap *regmap;
	int fmt;
};

static const struct reg_default katana_codec_reg_defaults[] = {
	{ KATANA_CODEC_RESET,		0x00 },
	{ KATANA_CODEC_VOLUME_1,	0xF0 },
	{ KATANA_CODEC_VOLUME_2,	0xF0 },
	{ KATANA_CODEC_MUTE,		0x00 },
	{ KATANA_CODEC_DSP_PROGRAM,	0x04 },
	{ KATANA_CODEC_DEEMPHASIS,	0x00 },
	{ KATANA_CODEC_DOP,		0x00 },
	{ KATANA_CODEC_FORMAT,		0xb4 },
};

static const char * const katana_codec_dsp_program_texts[] = {
	"Linear Phase Fast Roll-off Filter",
	"Linear Phase Slow Roll-off Filter",
	"Minimum Phase Fast Roll-off Filter",
	"Minimum Phase Slow Roll-off Filter",
	"Apodizing Fast Roll-off Filter",
	"Corrected Minimum Phase Fast Roll-off Filter",
	"Brick Wall Filter",
};

static const unsigned int katana_codec_dsp_program_values[] = {
	0,
	1,
	2,
	3,
	4,
	6,
	7,
};

static SOC_VALUE_ENUM_SINGLE_DECL(katana_codec_dsp_program,
				  KATANA_CODEC_DSP_PROGRAM, 0, 0x07,
				  katana_codec_dsp_program_texts,
				  katana_codec_dsp_program_values);

static const char * const katana_codec_deemphasis_texts[] = {
	"Bypass",
	"32kHz",
	"44.1kHz",
	"48kHz",
};

static const unsigned int katana_codec_deemphasis_values[] = {
	0,
	1,
	2,
	3,
};

static SOC_VALUE_ENUM_SINGLE_DECL(katana_codec_deemphasis,
				  KATANA_CODEC_DEEMPHASIS, 0, 0x03,
				  katana_codec_deemphasis_texts,
				  katana_codec_deemphasis_values);

static const SNDRV_CTL_TLVD_DECLARE_DB_MINMAX(master_tlv, -12750, 0);

static const struct snd_kcontrol_new katana_codec_controls[] = {
	SOC_DOUBLE_R_TLV("Master Playback Volume", KATANA_CODEC_VOLUME_1,
			KATANA_CODEC_VOLUME_2, 0, 255, 1, master_tlv),
	SOC_DOUBLE("Master Playback Switch", KATANA_CODEC_MUTE, 0, 0, 1, 1),
	SOC_ENUM("DSP Program Route", katana_codec_dsp_program),
	SOC_ENUM("Deemphasis Route", katana_codec_deemphasis),
	SOC_SINGLE("DoP Playback Switch", KATANA_CODEC_DOP, 0, 1, 1)
};

static bool katana_codec_readable_register(struct device *dev,
				unsigned int reg)
{
	switch (reg) {
	case KATANA_CODEC_CHIP_ID_REG:
		return true;
	default:
		return reg < 0xff;
	}
}

static int katana_codec_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct katana_codec_priv *katana_codec =
		snd_soc_component_get_drvdata(component);
	int fmt = 0;
	int ret;

	dev_dbg(component->card->dev, "hw_params %u Hz, %u channels, %u bits\n",
			params_rate(params),
			params_channels(params),
			params_width(params));

	switch (katana_codec->fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM: // master
		if (params_channels(params) == 2)
			fmt = KATANA_CODEC_CHAN_STEREO;
		else
			fmt = KATANA_CODEC_CHAN_MONO;

		switch (params_width(params)) {
		case 16:
			fmt |= KATANA_CODEC_ALEN_16;
			break;
		case 24:
			fmt |= KATANA_CODEC_ALEN_24;
			break;
		case 32:
			fmt |= KATANA_CODEC_ALEN_32;
			break;
		default:
			dev_err(component->card->dev, "Bad frame size: %d\n",
					params_width(params));
			return -EINVAL;
		}

		switch (params_rate(params)) {
		case 44100:
			fmt |= KATANA_CODEC_RATE_44100;
			break;
		case 48000:
			fmt |= KATANA_CODEC_RATE_48000;
			break;
		case 88200:
			fmt |= KATANA_CODEC_RATE_88200;
			break;
		case 96000:
			fmt |= KATANA_CODEC_RATE_96000;
			break;
		case 176400:
			fmt |= KATANA_CODEC_RATE_176400;
			break;
		case 192000:
			fmt |= KATANA_CODEC_RATE_192000;
			break;
		case 352800:
			fmt |= KATANA_CODEC_RATE_352800;
			break;
		case 384000:
			fmt |= KATANA_CODEC_RATE_384000;
			break;
		default:
			dev_err(component->card->dev, "Bad sample rate: %d\n",
					params_rate(params));
			return -EINVAL;
		}

		ret = regmap_write(katana_codec->regmap, KATANA_CODEC_FORMAT,
					fmt);
		if (ret != 0) {
			dev_err(component->card->dev, "Failed to set format: %d\n", ret);
			return ret;
		}
		break;

	case SND_SOC_DAIFMT_CBS_CFS:
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int katana_codec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct katana_codec_priv *katana_codec =
		snd_soc_component_get_drvdata(component);

	katana_codec->fmt = fmt;

	return 0;
}

static int katana_codec_dai_mute_stream(struct snd_soc_dai *dai, int mute,
						int stream)
{
	struct snd_soc_component *component = dai->component;
	struct katana_codec_priv *katana_codec =
		snd_soc_component_get_drvdata(component);
	int ret = 0;

	ret = regmap_write(katana_codec->regmap, KATANA_CODEC_MUTE_STREAM,
				mute);
	if (ret != 0) {
		dev_err(component->card->dev, "Failed to set mute: %d\n", ret);
		return ret;
	}
	return ret;
}

static const struct snd_soc_dai_ops katana_codec_dai_ops = {
	.mute_stream = katana_codec_dai_mute_stream,
	.hw_params = katana_codec_hw_params,
	.set_fmt = katana_codec_set_fmt,
};

static struct snd_soc_dai_driver katana_codec_dai = {
	.name = "allo-katana-codec",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min = 44100,
		.rate_max = 384000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			SNDRV_PCM_FMTBIT_S32_LE
	},
	.ops = &katana_codec_dai_ops,
};

static struct snd_soc_component_driver katana_codec_component_driver = {
	.idle_bias_on = true,

	.controls		= katana_codec_controls,
	.num_controls	= ARRAY_SIZE(katana_codec_controls),
};

static const struct regmap_range_cfg katana_codec_range = {
	.name = "Pages", .range_min = KATANA_CODEC_VIRT_BASE,
	.range_max = KATANA_CODEC_MAX_REGISTER,
	.selector_reg = KATANA_CODEC_PAGE,
	.selector_mask = 0xff,
	.window_start = 0, .window_len = 0x100,
};

const struct regmap_config katana_codec_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.ranges = &katana_codec_range,
	.num_ranges = 1,

	.max_register = KATANA_CODEC_MAX_REGISTER,
	.readable_reg = katana_codec_readable_register,
	.reg_defaults = katana_codec_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(katana_codec_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
};

static int allo_katana_component_probe(struct i2c_client *i2c)
{
	struct regmap *regmap;
	struct regmap_config config = katana_codec_regmap;
	struct device *dev = &i2c->dev;
	struct katana_codec_priv *katana_codec;
	unsigned int chip_id = 0;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	katana_codec = devm_kzalloc(dev, sizeof(struct katana_codec_priv),
					GFP_KERNEL);
	if (!katana_codec)
		return -ENOMEM;

	dev_set_drvdata(dev, katana_codec);
	katana_codec->regmap = regmap;

	ret = regmap_read(regmap, KATANA_CODEC_CHIP_ID_REG, &chip_id);
	if ((ret != 0) || (chip_id != KATANA_CODEC_CHIP_ID)) {
		dev_err(dev, "Failed to read Chip or wrong Chip id: %d\n", ret);
		return ret;
	}
	regmap_update_bits(regmap, KATANA_CODEC_RESET, 0x01, 0x01);
	msleep(10);

	ret = snd_soc_register_component(dev, &katana_codec_component_driver,
				    &katana_codec_dai, 1);
	if (ret != 0) {
		dev_err(dev, "failed to register codec: %d\n", ret);
		return ret;
	}

	return 0;
}

static void allo_katana_component_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_component(&i2c->dev);
}

static const struct i2c_device_id allo_katana_component_id[] = {
	{ "allo-katana-codec", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, allo_katana_component_id);

static const struct of_device_id allo_katana_codec_of_match[] = {
	{ .compatible = "allo,allo-katana-codec", },
	{ }
};
MODULE_DEVICE_TABLE(of, allo_katana_codec_of_match);

static struct i2c_driver allo_katana_component_driver = {
	.probe		= allo_katana_component_probe,
	.remove		= allo_katana_component_remove,
	.id_table	= allo_katana_component_id,
	.driver		= {
	.name		= "allo-katana-codec",
	.of_match_table = allo_katana_codec_of_match,
	},
};

module_i2c_driver(allo_katana_component_driver);

MODULE_DESCRIPTION("ASoC Allo Katana Codec Driver");
MODULE_AUTHOR("Jaikumar <jaikumar@cem-solutions.net>");
MODULE_LICENSE("GPL v2");
