/*
 * aries_wm8994.c
 *
 * Copyright (C) 2010 Samsung Electronics Co.Ltd
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/wm8994.h"
#include "i2s.h"

#define ARIES_WM8994_FREQ 24000000

static struct snd_soc_card aries;

/* 3.5 pie jack */
static struct snd_soc_jack jack;

/* 3.5 pie jack detection DAPM pins */
static struct snd_soc_jack_pin jack_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	}, {
		.pin = "Headset Stereophone",
		.mask = SND_JACK_HEADPHONE | SND_JACK_MECHANICAL |
			SND_JACK_AVOUT,
	},
};

/* 3.5 pie jack detection gpios */
static struct snd_soc_jack_gpio jack_gpio = {
		.name = "DET_3.5",
		.report = SND_JACK_HEADSET | SND_JACK_MECHANICAL |
			SND_JACK_AVOUT,
		.debounce_time = 200,
};

struct aries_wm8994_data {
	int 		gpio_jack_det;
};

static const struct snd_soc_dapm_widget aries_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Back Spk", NULL),
	SND_SOC_DAPM_SPK("Front Earpiece", NULL),
	SND_SOC_DAPM_HP("Headset Stereophone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("2nd Mic", NULL),
};

static const struct snd_soc_dapm_route aries_dapm_routes[] = {
	{"Back Spk", NULL, "SPKOUTLP"},
	{"Back Spk", NULL, "SPKOUTLN"},

	{"Front Earpiece", NULL, "HPOUT2N"},
	{"Front Earpiece", NULL, "HPOUT2P"},

	{"Headset Stereophone", NULL, "HPOUT1L"},
	{"Headset Stereophone", NULL, "HPOUT1R"},

	{"IN1RN", NULL, "Headset Mic"},
	{"IN1RP", NULL, "Headset Mic"},

	{"IN1LN", NULL, "Main Mic"},
	{"IN1LP", NULL, "Main Mic"},

	{"IN2LN", NULL, "2nd Mic"},
};

static int aries_wm8994_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dapm_context *dapm = &rtd->card->dapm;
	struct aries_wm8994_data *board = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = snd_soc_dapm_new_controls(dapm, aries_dapm_widgets,
				ARRAY_SIZE(aries_dapm_widgets));
	if (ret)
		return ret;

	snd_soc_dapm_add_routes(dapm, aries_dapm_routes,
				ARRAY_SIZE(aries_dapm_routes));

	/* Other pins NC */
	snd_soc_dapm_nc_pin(dapm, "SPKOUTRP");
	snd_soc_dapm_nc_pin(dapm, "SPKOUTRN");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT1N");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT1P");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT2N");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT2P");
	snd_soc_dapm_nc_pin(dapm, "IN2RN");
	snd_soc_dapm_nc_pin(dapm, "IN2RP:VXRP");
	snd_soc_dapm_nc_pin(dapm, "IN2LP:VXRN");

	ret = snd_soc_dapm_sync(dapm);
	if (ret)
		return ret;

	if (gpio_is_valid(board->gpio_jack_det)) {
		/* Headset jack detection */
		ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
				SND_JACK_HEADSET | SND_JACK_MECHANICAL | SND_JACK_AVOUT,
				&jack, jack_pins, ARRAY_SIZE(jack_pins));

		jack_gpio.gpio = board->gpio_jack_det;

		ret = snd_soc_jack_add_gpios(&jack, 1, &jack_gpio);
		if (ret)
			return ret;
	}

	return 0;
}

static int aries_hifi_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int pll_out;
	int ret;

	/* AIF1CLK should be >=3MHz for optimal performance */
	if (params_width(params) == 24)
		pll_out = params_rate(params) * 384;
	else if (params_rate(params) == 8000 || params_rate(params) == 11025)
		pll_out = params_rate(params) * 512;
	else
		pll_out = params_rate(params) * 256;

	/* select the AP sysclk */
	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_CDCLK,
				     0, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_OPCLK,
				     0, SAMSUNG_I2S_OPCLK_PCLK);
	if (ret < 0)
		return ret;

	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, WM8994_FLL1, WM8994_FLL_SRC_MCLK1,
				  ARIES_WM8994_FREQ, pll_out);
	if (ret < 0)
		return ret;

	/* set the codec system clock */
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_FLL1,
				     pll_out, SND_SOC_CLOCK_IN);

	if (ret < 0)
		return ret;

	return 0;
}

static struct snd_soc_ops aries_hifi_ops = {
	.hw_params = aries_hifi_hw_params,
};

static int aries_modem_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int pll_out;
	int ret = 0;

	if (params_rate(params) != 8000)
		return -EINVAL;

	pll_out = params_rate(params) * 256;

	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, WM8994_FLL2, WM8994_FLL_SRC_MCLK2,
				  ARIES_WM8994_FREQ, pll_out);
	if (ret < 0)
		return ret;

	/* set the codec system clock */
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_FLL2,
				     pll_out, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	return 0;
}

static struct snd_soc_ops aries_modem_ops = {
	.hw_params = aries_modem_hw_params,
};

static struct snd_soc_dai_driver voice_dai = {
	.name = "aries-modem-dai",
	.playback = {
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static const struct snd_soc_component_driver voice_component = {
	.name		= "aries-voice",
};

static struct snd_soc_dai_link aries_dai[] = {
	{
		.name = "WM8994",
		.stream_name = "WM8994 HiFi",
		.cpu_dai_name = SAMSUNG_I2S_DAI,
		.codec_dai_name = "wm8994-aif1",
		.platform_name = "samsung-i2s.0",
		.codec_name = "wm8994-codec",
		.init = aries_wm8994_init,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBM_CFM,
		.ops = &aries_hifi_ops,
	}, {
		.name = "WM8994 Modem",
		.stream_name = "Modem",
		.cpu_dai_name = "aries-modem-dai",
		.codec_dai_name = "wm8994-aif2",
		.codec_name = "wm8994-codec",
		.dai_fmt = SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_IB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.ops = &aries_modem_ops,
	},
};

static struct snd_soc_card aries = {
	.name = "aries",
	.owner = THIS_MODULE,
	.dai_link = aries_dai,
	.num_links = ARRAY_SIZE(aries_dai),
};

static const struct of_device_id samsung_wm8994_of_match[] = {
	{ .compatible = "samsung,aries-wm8994", },
	{},
};
MODULE_DEVICE_TABLE(of, samsung_wm8994_of_match);

static int aries_audio_probe(struct platform_device *pdev)
{
	int ret;
	struct device_node *np = pdev->dev.of_node;
	struct snd_soc_card *card = &aries;
	struct aries_wm8994_data *board;

	board = devm_kzalloc(&pdev->dev, sizeof(*board), GFP_KERNEL);
	if (!board)
		return -ENOMEM;

	card->dev = &pdev->dev;
	snd_soc_card_set_drvdata(card, board);

	if (np) {
		aries_dai[0].cpu_dai_name = NULL;
		aries_dai[0].cpu_of_node = of_parse_phandle(np,
				"samsung,i2s-controller", 0);
		if (!aries_dai[0].cpu_of_node) {
			dev_err(&pdev->dev,
			   "Property 'samsung,i2s-controller' missing or invalid\n");
			ret = -EINVAL;
		}

		aries_dai[0].platform_name = NULL;
		aries_dai[0].platform_of_node = aries_dai[0].cpu_of_node;

		board->gpio_jack_det =
				of_get_named_gpio(np, "samsung,jack-det-gpios", 0);
		if (board->gpio_jack_det == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	ret = devm_snd_soc_register_component(&pdev->dev, &voice_component,
						&voice_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "failed to register voice dai:%d\n", ret);
	}

	platform_set_drvdata(pdev, board);

	ret = devm_snd_soc_register_card(&pdev->dev, card);

	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed:%d\n", ret);

	return ret;
}

static int aries_audio_remove(struct platform_device *pdev)
{
	struct aries_wm8994_data *board = platform_get_drvdata(pdev);

	return 0;
}

static struct platform_driver aries_audio_driver = {
	.driver		= {
		.name	= "aries-audio-wm8994",
		.of_match_table = of_match_ptr(samsung_wm8994_of_match),
		.pm	= &snd_soc_pm_ops,
	},
	.probe		= aries_audio_probe,
	.remove		= aries_audio_remove,
};

module_platform_driver(aries_audio_driver);

MODULE_DESCRIPTION("ALSA SoC Aries WM8994");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:aries-audio-wm8994");
