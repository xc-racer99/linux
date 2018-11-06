// SPDX-License-Identifier: GPL-2.0+
//
// Wolfson wm8994 machine driver for Aries board

#include <linux/extcon.h>
#include <linux/iio/consumer.h>
#include <linux/input-event-codes.h>
#include <linux/limits.h>
#include <linux/mfd/wm8994/registers.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "i2s.h"
#include "../codecs/wm8994.h"

/* All Aries have a 24MHz clock attached to WM8994 */
#define ARIES_MCLK1_FREQ 24000000

struct aries_wm8994_data {
	struct extcon_dev *usb_extcon;
	struct regulator *reg_main_micbias;
	struct regulator *reg_headset_micbias;
	struct gpio_desc *gpio_headset_detect;
	struct gpio_desc *gpio_headset_key;
	struct iio_channel *adc;
};

/* USB dock */
static struct snd_soc_jack aries_dock;

static struct snd_soc_jack_pin dock_pins[] = {
	{
		.pin = "LINE",
		.mask = SND_JACK_LINEOUT,
	},
};

static int aries_extcon_notifier(struct notifier_block *this,
				 unsigned long connected, void *_cmd)
{
	if (connected)
		snd_soc_jack_report(&aries_dock, SND_JACK_LINEOUT,
				SND_JACK_LINEOUT);
	else
		snd_soc_jack_report(&aries_dock, 0, SND_JACK_LINEOUT);

	return NOTIFY_DONE;
}

static struct notifier_block aries_extcon_notifier_block = {
	.notifier_call = aries_extcon_notifier,
};

/* Headset jack */
static struct snd_soc_jack aries_headset;

static struct snd_soc_jack_pin jack_pins[] = {
	{
		.pin = "HP",
		.mask = SND_JACK_HEADPHONE,
	}, {
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
};

static struct snd_soc_jack_zone headset_zones[] = {
	{
		.min_mv = 0,
		.max_mv = 299,
		.jack_type = SND_JACK_HEADPHONE,
	}, {
		.min_mv = 300,
		.max_mv = 3699,
		.jack_type = SND_JACK_HEADSET,
	}, {
		.min_mv = 3700,
		.max_mv = UINT_MAX,
		.jack_type = SND_JACK_HEADPHONE,
	},
};

static int headset_adc_check(void *data)
{
	struct aries_wm8994_data *priv = (struct aries_wm8994_data *) data;
	int ret, adc;

	if (gpiod_get_value_cansleep(priv->gpio_headset_detect)) {
		if (priv->adc) {
			ret = iio_read_channel_raw(priv->adc, &adc);
			if (ret < 0) {
				pr_err("%s failed to read adc: %d", __func__, ret);
				return SND_JACK_HEADPHONE;
			}

			return snd_soc_jack_get_type(&aries_headset, adc);
		} else {
			/* Default to headphone if no ADC available */
			return SND_JACK_HEADPHONE;
		}
	}

	/* jack was unplugged */
	return 0;
}

static int headset_button_check(void *data)
{
	struct aries_wm8994_data *priv = (struct aries_wm8994_data *) data;

	/* Filter out keypresses when 4 pole jack not detected */
	if (gpiod_get_value_cansleep(priv->gpio_headset_key) &&
			aries_headset.status & SND_JACK_MICROPHONE)
		return SND_JACK_BTN_0;

	return 0;
}

static struct snd_soc_jack_gpio headset_gpios[] = {
	{
		.name = "Headset Detect",
		.report = SND_JACK_HEADSET,
		.debounce_time = 200,
		.jack_status_check = headset_adc_check,
	},
	{
		.name = "Media Button",
		.report = SND_JACK_BTN_0,
		.debounce_time  = 30,
		.jack_status_check = headset_button_check,
	},
};

static int aries_main_bias(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct aries_wm8994_data *priv = snd_soc_card_get_drvdata(card);
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regulator_enable(priv->reg_main_micbias);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = regulator_disable(priv->reg_main_micbias);
		break;
	}

	return ret;
}

static int aries_headset_bias(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct aries_wm8994_data *priv = snd_soc_card_get_drvdata(card);
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regulator_enable(priv->reg_headset_micbias);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = regulator_disable(priv->reg_headset_micbias);
	}

	return ret;
}

static const struct snd_kcontrol_new aries_controls[] = {
	SOC_DAPM_PIN_SWITCH("HP"),
	SOC_DAPM_PIN_SWITCH("SPK"),
	SOC_DAPM_PIN_SWITCH("RCV"),
	SOC_DAPM_PIN_SWITCH("LINE"),

	SOC_DAPM_PIN_SWITCH("Main Mic"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),

	SOC_DAPM_PIN_SWITCH("FM In"),

	SOC_DAPM_PIN_SWITCH("Modem In"),
	SOC_DAPM_PIN_SWITCH("Modem Out"),

	SOC_DAPM_PIN_SWITCH("Bluetooth Mic"),
	SOC_DAPM_PIN_SWITCH("Bluetooth Speaker"),
};

static const struct snd_soc_dapm_widget aries_dapm_widgets[] = {
	SND_SOC_DAPM_HP("HP", NULL),

	SND_SOC_DAPM_SPK("SPK", NULL),
	SND_SOC_DAPM_SPK("RCV", NULL),

	SND_SOC_DAPM_LINE("LINE", NULL),

	SND_SOC_DAPM_MIC("Main Mic", aries_main_bias),
	SND_SOC_DAPM_MIC("Headset Mic", aries_headset_bias),

	SND_SOC_DAPM_LINE("FM In", NULL),

	SND_SOC_DAPM_LINE("Modem In", NULL),
	SND_SOC_DAPM_LINE("Modem Out", NULL),

	SND_SOC_DAPM_MIC("Bluetooth Mic", NULL),
	SND_SOC_DAPM_SPK("Bluetooth Speaker", NULL),
};

static const struct snd_soc_dapm_route aries_dapm_routes[] = {
	/* Static modem routes */
	{ "Modem Out", NULL, "Modem TX" },
	{ "Modem RX", NULL, "Modem In" },

	/* Static bluetooth routes */
	{ "Bluetooth Speaker", NULL, "TX" },
	{ "RX", NULL, "Bluetooth Mic" },
};

static int aries_hw_params(struct snd_pcm_substream *substream,
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

	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, WM8994_FLL1, WM8994_FLL_SRC_MCLK1,
			ARIES_MCLK1_FREQ, pll_out);
	if (ret < 0)
		return ret;

	/* set the codec system clock */
	return snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_FLL1,
			pll_out, SND_SOC_CLOCK_IN);
}

static int aries_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	/* set system clock to MCLK1 as it is always on */
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_MCLK1,
			ARIES_MCLK1_FREQ, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	/* disable FLL1 */
	return snd_soc_dai_set_pll(codec_dai, WM8994_FLL1, WM8994_SYSCLK_MCLK1,
			0, 0);
}

static struct snd_soc_ops aries_ops = {
	.hw_params = aries_hw_params,
	.hw_free = aries_hw_free,
};

static int aries_modem_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int pll_out;
	int ret;

	pll_out = 8000 * 512;

	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, WM8994_FLL2, WM8994_FLL_SRC_MCLK1,
			ARIES_MCLK1_FREQ, pll_out);
	if (ret < 0)
		return ret;

	/* set the codec system clock */
	return snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_FLL2,
			pll_out, SND_SOC_CLOCK_IN);
}

static int aries_late_probe(struct snd_soc_card *card)
{
	struct aries_wm8994_data *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd;
	int ret;

	rtd = snd_soc_get_pcm_runtime(card, card->dai_link[0].name);

	/* Initialize AIF1 clock for sysclk */
	ret = snd_soc_dai_set_sysclk(rtd->codec_dai, WM8994_SYSCLK_MCLK1,
			ARIES_MCLK1_FREQ, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	if (priv->usb_extcon) {
		ret = devm_extcon_register_notifier(card->dev,
				priv->usb_extcon, EXTCON_JACK_LINE_OUT,
				&aries_extcon_notifier_block);
		if (ret)
			return ret;

		ret = snd_soc_card_jack_new(card, "Dock", SND_JACK_LINEOUT,
				&aries_dock, dock_pins, ARRAY_SIZE(dock_pins));
		if (ret)
			return ret;

		if (extcon_get_state(priv->usb_extcon,
				EXTCON_JACK_LINE_OUT) > 0)
			snd_soc_jack_report(&aries_dock, SND_JACK_LINEOUT,
					SND_JACK_LINEOUT);
		else
			snd_soc_jack_report(&aries_dock, 0, SND_JACK_LINEOUT);
	}

	ret = snd_soc_card_jack_new(card, "Headset",
			SND_JACK_HEADSET | SND_JACK_BTN_0,
			&aries_headset,
			jack_pins, ARRAY_SIZE(jack_pins));
	if (ret)
		return ret;

	headset_gpios[0].data = priv;
	headset_gpios[0].desc = priv->gpio_headset_detect;

	headset_gpios[1].data = priv;
	headset_gpios[1].desc = priv->gpio_headset_key;

	snd_jack_set_key(aries_headset.jack, SND_JACK_BTN_0,
			KEY_MEDIA);

	ret = snd_soc_jack_add_zones(&aries_headset, ARRAY_SIZE(headset_zones),
			headset_zones);
	if (ret)
		return ret;

	return snd_soc_jack_add_gpios(&aries_headset,
			ARRAY_SIZE(headset_gpios), headset_gpios);
}

static const struct snd_soc_dapm_widget aries_modem_widgets[] = {
	SND_SOC_DAPM_INPUT("Modem RX"),
	SND_SOC_DAPM_OUTPUT("Modem TX"),
};

static const struct snd_soc_dapm_route aries_modem_routes[] = {
	{ "Modem Capture", NULL, "Modem RX" },
	{ "Modem TX", NULL, "Modem Playback" },
};

static struct snd_soc_dai_driver aries_modem_dai[] = {
	{
		.name = "aries-modem-dai",
		.playback = {
			.stream_name = "Modem Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.stream_name = "Modem Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
};

static const struct snd_soc_component_driver aries_component = {
	.dapm_widgets		= aries_modem_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(aries_modem_widgets),
	.dapm_routes		= aries_modem_routes,
	.num_dapm_routes	= ARRAY_SIZE(aries_modem_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
	.non_legacy_dai_naming	= 1,
};

static const struct snd_soc_pcm_stream baseband_params = {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 8000,
	.rate_max = 8000,
	.channels_min = 2,
	.channels_max = 2,
};

static const struct snd_soc_pcm_stream bluetooth_params = {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 8000,
	.rate_max = 8000,
	.channels_min = 1,
	.channels_max = 2,
};

static struct snd_soc_dai_link aries_dai[] = {
	{
		.name = "WM8994 AIF1",
		.stream_name = "Pri_Dai",
		.codec_dai_name = "wm8994-aif1",
		.cpu_dai_name = SAMSUNG_I2S_DAI,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBM_CFM,
		.ops = &aries_ops,
	},
	{
		.name = "WM8994 AIF2",
		.stream_name = "Voice",
		.codec_dai_name = "wm8994-aif2",
		.cpu_dai_name = "aries-modem-dai",
		.init = &aries_modem_init,
		.params = &baseband_params,
		.ignore_suspend = 1,
	},
	{
		.name = "WM8994 AIF3",
		.stream_name = "Bluetooth",
		.codec_dai_name = "wm8994-aif3",
		.cpu_dai_name = "bt-sco-pcm",
		.params = &bluetooth_params,
		.ignore_suspend = 1,
	},
};

static struct snd_soc_card aries = {
	.name = "Aries-I2S",
	.owner = THIS_MODULE,
	.dai_link = aries_dai,
	.num_links = ARRAY_SIZE(aries_dai),
#if 0
	.fully_routed = true,
#endif
	.controls = aries_controls,
	.num_controls = ARRAY_SIZE(aries_controls),
	.dapm_widgets = aries_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aries_dapm_widgets),
	.dapm_routes = aries_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(aries_dapm_routes),
	.late_probe = aries_late_probe,
};

static const struct of_device_id samsung_wm8994_of_match[] = {
	{ .compatible = "samsung,aries-wm8994" },
	{},
};
MODULE_DEVICE_TABLE(of, samsung_wm8994_of_match);

static int aries_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *cpu_dai_np, *codec_dai_np, *extcon_np;
	struct snd_soc_card *card = &aries;
	struct aries_wm8994_data *priv;
	struct snd_soc_dai_link *dai_link;
	int ret, i;

	card->dev = dev;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	snd_soc_card_set_drvdata(card, priv);

	priv->reg_main_micbias = devm_regulator_get(dev, "main-micbias");
	if (IS_ERR(priv->reg_main_micbias)) {
		dev_err(dev, "Failed to get main micbias regulator\n");
		return PTR_ERR(priv->reg_main_micbias);
	}

	priv->reg_headset_micbias = devm_regulator_get(dev, "headset-micbias");
	if (IS_ERR(priv->reg_headset_micbias)) {
		dev_err(dev, "Failed to get headset micbias regulator\n");
		return PTR_ERR(priv->reg_headset_micbias);
	}

	extcon_np = of_parse_phandle(np, "dock-extcon", 0);
	if (extcon_np) {
		priv->usb_extcon = extcon_find_edev_by_node(extcon_np);
		if (IS_ERR(priv->usb_extcon)) {
			dev_warn(dev, "Couldn't get extcon device");
			priv->usb_extcon = NULL;
		}
		of_node_put(extcon_np);
	}

	/* ADC is optional */
	priv->adc = devm_iio_channel_get(dev, "headset-detect");
	if (IS_ERR(priv->adc)) {
		if (PTR_ERR(priv->adc) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	priv->gpio_headset_key = devm_gpiod_get(dev, "headset-key",
			GPIOD_IN);
	if (IS_ERR(priv->gpio_headset_detect)) {
		dev_err(dev, "Failed to get headset key gpio");
		return PTR_ERR(priv->gpio_headset_detect);
	}

	priv->gpio_headset_detect = devm_gpiod_get(dev,
			"headset-detect", GPIOD_IN);
	if (IS_ERR(priv->gpio_headset_detect)) {
		dev_err(dev, "Failed to get headset detect GPIO");
		return PTR_ERR(priv->gpio_headset_detect);
	}

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret < 0) {
		dev_err(dev, "Card name is not specified\n");
		return ret;
	}

	ret = snd_soc_of_parse_audio_routing(card, "samsung,audio-routing");
	if (ret < 0) {
		dev_err(dev, "Audio routing invalid/unspecified\n");
		return ret;
	}

	aries_dai[1].dai_fmt = snd_soc_of_parse_daifmt(np, "samsung,modem-",
			NULL, NULL);

	cpu_dai_np = of_parse_phandle(dev->of_node, "i2s-controller", 0);
	if (!cpu_dai_np) {
		dev_err(dev, "i2s-controller property invalid/missing\n");
		return -EINVAL;
	}

	codec_dai_np = of_parse_phandle(dev->of_node, "audio-codec", 0);
	if (!codec_dai_np) {
		dev_err(dev, "audio-codec property invalid/missing\n");
		ret = -EINVAL;
		goto cpu_dai_node_put;
	}

	card->dai_link[0].cpu_of_node = cpu_dai_np;
	card->dai_link[0].platform_of_node = cpu_dai_np;

	for_each_card_prelinks(card, i, dai_link)
		dai_link->codec_of_node = codec_dai_np;

	ret = devm_snd_soc_register_component(dev, &aries_component,
			aries_modem_dai, ARRAY_SIZE(aries_modem_dai));
	if (ret < 0) {
		dev_err(dev, "Failed to register component: %d\n", ret);
		goto codec_dai_node_put;
	}

	ret = devm_snd_soc_register_card(dev, card);
	if (ret < 0)
		dev_err(dev, "Failed to register card: %d\n", ret);

codec_dai_node_put:
	of_node_put(codec_dai_np);

cpu_dai_node_put:
	of_node_put(cpu_dai_np);

	return ret;
}

static struct platform_driver aries_audio_driver = {
	.driver		= {
		.name	= "aries-audio-wm8994",
		.of_match_table = of_match_ptr(samsung_wm8994_of_match),
		.pm	= &snd_soc_pm_ops,
	},
	.probe		= aries_audio_probe,
};

module_platform_driver(aries_audio_driver);

MODULE_DESCRIPTION("ALSA SoC Aries WM8994");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:aries-audio-wm8994");
