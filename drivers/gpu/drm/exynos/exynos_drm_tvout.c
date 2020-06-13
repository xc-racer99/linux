// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Jonathan Bakker
 *
 * Based on drivers/media/platform/s5p-tv/sdo_drv.c
 */

#include <drm/exynos_drm.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "exynos_drm_crtc.h"
#include "regs-sdo.h"

#define PMU_DAC_PHY_ENABLE_BIT		BIT(0)
#define E4210_DAC_PHY_OFFSET		0x070C
#define S5PV210_DAC_PHY_OFFSET		0x6810

enum tv_norm {
	TV_NORM_NTSC,
	TV_NORM_NTSC_443,
	TV_NORM_PAL,
	TV_NORM_PAL_M,
	TV_NORM_PAL_N,
	TV_NORM_PAL_NC,
	TV_NORM_PAL_60,
	NUM_TV_NORMS
};

struct tv_mode {
	u32 mode;
	const struct drm_display_mode *disp_mode;
};

struct sdo_context {
	struct drm_connector		connector;
	struct drm_encoder		encoder;
	struct device			*dev;
	struct drm_device		*drm_dev;

	void __iomem			*regs;
	unsigned int			irq;
	struct clk			*dac;
	struct clk			*sclk_dac;
	struct clk			*fout_vpll;
	struct regmap			*pmureg;
	struct regulator		*vdd;
	unsigned long			vpll_rate;

	struct exynos_drm_clk		phy_clk;

	enum tv_norm			norm;

	u32				pmu_offset;

	struct mutex			mutex;
	bool				enabled;
};

static const struct drm_display_mode ntsc_mode = {
	DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 13500,
		720, 739, 801, 858, 0, 480, 488, 494, 525, 0,
		DRM_MODE_FLAG_INTERLACE),
	.vrefresh = 60,
};

static const struct drm_display_mode pal_mode = {
	DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 13500,
		 720, 732, 795, 864, 0, 576, 580, 586, 625, 0,
		 DRM_MODE_FLAG_INTERLACE),
	.vrefresh = 50,
};

static const struct tv_mode tv_modes[] = {
	[TV_NORM_NTSC] = {
		.mode = SDO_NTSC_M,
		.disp_mode = &ntsc_mode,
	},
	[TV_NORM_NTSC_443] = {
		.mode = SDO_NTSC_443,
		.disp_mode = &ntsc_mode,
	},
	[TV_NORM_PAL] = {
		.mode = SDO_PAL_BGHID,
		.disp_mode = &pal_mode,
	},
	[TV_NORM_PAL_M] = {
		.mode = SDO_PAL_M,
		.disp_mode = &pal_mode,
	},
	[TV_NORM_PAL_N] = {
		.mode = SDO_PAL_N,
		.disp_mode = &pal_mode,
	},
	[TV_NORM_PAL_NC] = {
		.mode = SDO_PAL_NC,
		.disp_mode = &pal_mode,
	},
	[TV_NORM_PAL_60] = {
		.mode = SDO_PAL_60,
		.disp_mode = &pal_mode,
	},
};

static inline struct sdo_context *encoder_to_sdo(struct drm_encoder *e)
{
	return container_of(e, struct sdo_context, encoder);
}

static inline
void sdo_write_mask(struct sdo_context *sdata, u32 reg_id, u32 value, u32 mask)
{
	u32 old = readl(sdata->regs + reg_id);
	value = (value & mask) | (old & ~mask);
	writel(value, sdata->regs + reg_id);
}

static inline
void sdo_write(struct sdo_context *sdata, u32 reg_id, u32 value)
{
	writel(value, sdata->regs + reg_id);
}

static inline
u32 sdo_read(struct sdo_context *sdata, u32 reg_id)
{
	return readl(sdata->regs + reg_id);
}

static irqreturn_t sdo_irq_handler(int irq, void *dev_data)
{
	struct sdo_context *sdata = dev_data;

	/* clear interrupt */
	sdo_write_mask(sdata, SDO_IRQ, ~0, SDO_VSYNC_IRQ_PEND);

	return IRQ_HANDLED;
}

/* Connector */

static enum drm_connector_status
cvbs_connector_detect(struct drm_connector *connector, bool force)
{
	/* FIXME: Add load-detect or jack-detect if possible */
	return connector_status_unknown;
}

static int cvbs_connector_get_modes(struct drm_connector *connector)
{
	struct drm_connector_state *state = connector->state;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev,
				  tv_modes[state->tv.mode].disp_mode);
	if (!mode) {
		DRM_ERROR("Failed to create a new display mode\n");
		return -ENOMEM;
	}

	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_funcs cvbs_connector_funcs = {
	.detect			= cvbs_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static const
struct drm_connector_helper_funcs cvbs_connector_helper_funcs = {
	.get_modes	= cvbs_connector_get_modes,
};

static const char * const tv_mode_names[] = {
	[TV_NORM_NTSC] = "NTSC",
	[TV_NORM_NTSC_443] = "NTSC-443",
	[TV_NORM_PAL] = "PAL",
	[TV_NORM_PAL_M] = "PAL-M",
	[TV_NORM_PAL_N] = "PAL-N",
	[TV_NORM_PAL_NC] = "PAL-Nc",
	[TV_NORM_PAL_60] = "PAL-60",
};

static int sdo_create_connector(struct drm_encoder *encoder)
{
	struct sdo_context *sdata = encoder_to_sdo(encoder);
	struct drm_connector *connector = &sdata->connector;
	int ret;

	connector->interlace_allowed = true;
	connector->polled = 0;

	ret = drm_connector_init(sdata->drm_dev, connector,
				&cvbs_connector_funcs,
				DRM_MODE_CONNECTOR_Composite);
	if (ret) {
		DRM_DEV_ERROR(sdata->dev,
			      "Failed to initialize connector with drm\n");
		return ret;
	}

	drm_connector_helper_add(connector, &cvbs_connector_helper_funcs);

	ret = drm_mode_create_tv_properties(sdata->drm_dev, ARRAY_SIZE(tv_mode_names),
					    tv_mode_names);
	if (ret)
		return ret;

	drm_object_attach_property(&sdata->connector.base,
				   sdata->drm_dev->mode_config.tv_mode_property,
				   TV_NORM_NTSC);
	sdata->norm = TV_NORM_NTSC;

	drm_connector_attach_encoder(connector, encoder);

	return 0;
}

/* Encoder */

static bool sdo_mode_fixup(struct drm_encoder *encoder,
			    const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode)
{
	struct sdo_context *sdata = encoder_to_sdo(encoder);

	drm_mode_copy(adjusted_mode, tv_modes[sdata->norm].disp_mode);

	return true;
}

/* Should be called with sdata->mutex mutex held */
static void sdo_phy_enable(struct sdo_context *sdata)
{
	int ret;

	if (sdata->enabled)
		return;

	pm_runtime_get_sync(sdata->dev);

	/* Save VPLL rate so we can restore it later */
	sdata->vpll_rate = clk_get_rate(sdata->fout_vpll);

	ret = clk_set_rate(sdata->fout_vpll, 54000000);
	if (ret < 0)
		pr_err("Failed to set rate!");

	sdo_write_mask(sdata, SDO_CLKCON, ~0, SDO_TVOUT_CLOCK_ON);

	ret = regulator_enable(sdata->vdd);
	if (ret < 0)
		pr_err("Failed to enable regulator: %d", ret);

	regmap_update_bits(sdata->pmureg, sdata->pmu_offset,
			PMU_DAC_PHY_ENABLE_BIT, PMU_DAC_PHY_ENABLE_BIT);

	sdo_write_mask(sdata, SDO_DAC, ~0, SDO_POWER_ON_DAC);

	sdata->enabled = true;
}

/* Should be called with sdata->mutex mutex held */
static void sdo_phy_disable(struct sdo_context *sdata)
{
	int tries;

	if (!sdata->enabled)
		return;

	sdo_write_mask(sdata, SDO_DAC, 0, SDO_POWER_ON_DAC);

	regmap_update_bits(sdata->pmureg, sdata->pmu_offset,
			PMU_DAC_PHY_ENABLE_BIT, 0);

	sdo_write_mask(sdata, SDO_CLKCON, 0, SDO_TVOUT_CLOCK_ON);
	for (tries = 100; tries; --tries) {
		if (sdo_read(sdata, SDO_CLKCON) & SDO_TVOUT_CLOCK_READY)
			break;
		mdelay(1);
	}
	if (tries == 0)
		dev_err(sdata->dev, "failed to stop streaming\n");

	regulator_disable(sdata->vdd);

	/* Restore VPLL rate */
	clk_set_rate(sdata->fout_vpll, sdata->vpll_rate);

	pm_runtime_put_sync(sdata->dev);

	sdata->enabled = false;
}

static void sdo_enable(struct drm_encoder *encoder)
{
	struct sdo_context *sdata = encoder_to_sdo(encoder);

	mutex_lock(&sdata->mutex);

	sdo_phy_enable(sdata);

	mutex_unlock(&sdata->mutex);
}

static void sdo_disable(struct drm_encoder *encoder)
{
	/* Intentionally empty */
}

static void sdo_atomic_mode_set(struct drm_encoder *encoder,
				struct drm_crtc_state *crtc_state,
				struct drm_connector_state *conn_state)
{
	struct sdo_context *sdata = encoder_to_sdo(encoder);

	sdata->norm = conn_state->tv.mode;
}

static const struct drm_encoder_helper_funcs exynos_sdo_encoder_helper_funcs = {
	.mode_fixup		= sdo_mode_fixup,
	.enable			= sdo_enable,
	.disable		= sdo_disable,
	.atomic_mode_set	= sdo_atomic_mode_set,
};

static const struct drm_encoder_funcs exynos_sdo_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void sdo_clk_enable(struct exynos_drm_clk *clk, bool enable)
{
	struct sdo_context *sdata = container_of(clk, struct sdo_context,
						  phy_clk);
	mutex_lock(&sdata->mutex);

	if (enable)
		sdo_phy_enable(sdata);
	else
		sdo_phy_disable(sdata);

	mutex_unlock(&sdata->mutex);
}

static const struct of_device_id sdo_match_types[] = {
	{
		.compatible = "samsung,s5pv210-sdo",
		.data = (const void *) S5PV210_DAC_PHY_OFFSET,
	}, {
		.compatible = "samsung,exynos4210-sdo",
		.data = (const void *) E4210_DAC_PHY_OFFSET,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE (of, sdo_match_types);

static int sdo_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm_dev = data;
	struct sdo_context *sdata = dev_get_drvdata(dev);
	struct drm_encoder *encoder = &sdata->encoder;
	struct exynos_drm_crtc *crtc;
	int ret;

	sdata->drm_dev = drm_dev;
	sdata->phy_clk.enable = sdo_clk_enable;

	drm_encoder_init(drm_dev, encoder, &exynos_sdo_encoder_funcs,
			 DRM_MODE_ENCODER_TVDAC, NULL);

	drm_encoder_helper_add(encoder, &exynos_sdo_encoder_helper_funcs);

	ret = exynos_drm_set_possible_crtcs(encoder, EXYNOS_DISPLAY_TYPE_TVOUT);
	if (ret < 0)
		return ret;

	crtc = exynos_drm_crtc_get_by_type(drm_dev, EXYNOS_DISPLAY_TYPE_TVOUT);
	crtc->pipe_clk = &sdata->phy_clk;

	ret = sdo_create_connector(encoder);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to create connector ret = %d\n",
			      ret);
		drm_encoder_cleanup(encoder);
		return ret;
	}

	return 0;
}

static void sdo_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops sdo_component_ops = {
	.bind	= sdo_bind,
	.unbind = sdo_unbind,
};

static int sdo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sdo_context *sdata;
	struct resource *res;
	struct clk *mout_vpll, *mout_dac;
	int ret;

	sdata = devm_kzalloc(dev, sizeof(struct sdo_context), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	sdata->pmu_offset = (u32) of_device_get_match_data(dev);

	platform_set_drvdata(pdev, sdata);

	sdata->dev = dev;

	mutex_init(&sdata->mutex);

	sdata->sclk_dac = devm_clk_get(dev, "sclk_dac");
	if (IS_ERR(sdata->sclk_dac))
		return PTR_ERR(sdata->sclk_dac);

	sdata->dac = devm_clk_get(dev, "dac");
	if (IS_ERR(sdata->dac))
		return PTR_ERR(sdata->dac);

	mout_dac = clk_get(dev, "mout_dac");
	if (IS_ERR(mout_dac))
		return PTR_ERR(mout_dac);

	mout_vpll = clk_get(dev, "mout_vpll");
	if (IS_ERR(mout_vpll)) {
		clk_put(mout_dac);
		return PTR_ERR(mout_vpll);
	}

	clk_set_parent(mout_dac, mout_vpll);
	clk_put(mout_vpll);
	clk_put(mout_dac);

	sdata->fout_vpll = clk_get(dev, "fout_vpll");
	if (IS_ERR(sdata->fout_vpll))
		return PTR_ERR(sdata->fout_vpll);

	sdata->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(sdata->vdd))
		return PTR_ERR(sdata->vdd);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sdata->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(sdata->regs)) {
		ret = PTR_ERR(sdata->regs);
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		dev_err(dev, "get interrupt resource failed.\n");
		return -ENXIO;
	}

	ret = devm_request_irq(dev, res->start, sdo_irq_handler, 0,
			       "exynos-sdo", sdata);
	if (ret) {
		dev_err(dev, "request interrupt failed.\n");
		return ret;
	}
	sdata->irq = res->start;

	sdata->pmureg = syscon_regmap_lookup_by_phandle(dev->of_node,
			"samsung,pmureg-phandle");
	if (IS_ERR(sdata->pmureg)) {
		DRM_DEV_ERROR(dev, "syscon regmap lookup failed.\n");
		return -EPROBE_DEFER;
	}

	pm_runtime_enable(dev);

	ret = component_add(&pdev->dev, &sdo_component_ops);
	if (ret < 0)
		pm_runtime_disable(dev);

	return ret;
}

static int sdo_remove(struct platform_device *pdev)
{
	struct sdo_context *sdata = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &sdo_component_ops);

	pm_runtime_disable(&pdev->dev);

	mutex_destroy(&sdata->mutex);

	return 0;
}

static int __maybe_unused exynos_sdo_suspend(struct device *dev)
{
	struct sdo_context *sdata = dev_get_drvdata(dev);

	clk_disable_unprepare(sdata->sclk_dac);
	clk_disable_unprepare(sdata->dac);

	return 0;
}

static int __maybe_unused exynos_sdo_resume(struct device *dev)
{
	struct sdo_context *sdata = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(sdata->sclk_dac);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(sdata->dac);
	if (ret < 0)
		goto err_sclk_dac_clk_disable;

	/* software reset */
	sdo_write_mask(sdata, SDO_CLKCON, ~0, SDO_TVOUT_SW_RESET);
	mdelay(10);
	sdo_write_mask(sdata, SDO_CLKCON, 0, SDO_TVOUT_SW_RESET);

	/* set TV mode */
	sdo_write_mask(sdata, SDO_CONFIG, tv_modes[sdata->norm].mode,
		SDO_STANDARD_MASK);

	/* force interlaced mode */
	sdo_write_mask(sdata, SDO_CONFIG, 0, SDO_PROGRESSIVE);

	/* turn all VBI off */
	sdo_write_mask(sdata, SDO_VBI, 0, SDO_CVBS_WSS_INS |
		SDO_CVBS_CLOSED_CAPTION_MASK);

	/* turn all post processing off */
	sdo_write_mask(sdata, SDO_CCCON, ~0, SDO_COMPENSATION_BHS_ADJ_OFF |
		SDO_COMPENSATION_CVBS_COMP_OFF);

	return 0;

err_sclk_dac_clk_disable:
	clk_disable_unprepare(sdata->sclk_dac);

	return ret;
}

static const struct dev_pm_ops exynos_sdo_pm_ops = {
	SET_RUNTIME_PM_OPS(exynos_sdo_suspend, exynos_sdo_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

struct platform_driver sdo_driver = {
	.probe		= sdo_probe,
	.remove		= sdo_remove,
	.driver		= {
		.name	= "exynos-sdo",
		.owner	= THIS_MODULE,
		.pm	= &exynos_sdo_pm_ops,
		.of_match_table = sdo_match_types,
	},
};
