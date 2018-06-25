// SPDX-License-Identifier: GPL-2.0
//
// S5PV210 Generic power domain support.
//
// Copyright (c) 2010 Samsung Electronics Co., Ltd.
//		http://www.samsung.com
//
// Implementation of S5PV210 specific power domain control which is used in
// conjunction with runtime-pm.

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>

#include <dt-bindings/power/s5pv210-power.h>

#define S5PV210_PD_AUDIO_SHIFT        (1 << 7)
#define S5PV210_PD_CAM_SHIFT          (1 << 5)
#define S5PV210_PD_TV_SHIFT           (1 << 4)
#define S5PV210_PD_LCD_SHIFT          (1 << 3)
#define S5PV210_PD_G3D_SHIFT          (1 << 2)
#define S5PV210_PD_MFC_SHIFT          (1 << 1)

#define NORMAL_CFG_OFFSET 0x4010
#define BLK_PWR_STAT_OFFSET 0x4204

#define MAX_CLKS 6

/*
 * S5PV210 specific wrapper around the generic power domain
 */
struct s5pv210_pm_domain {
	struct regmap *reg_pmu;
	struct generic_pm_domain genpd;
	struct clk_bulk_data clks[MAX_CLKS];
	int num_clks;
	u32 ctrlbit;
	char name[30];
};

static unsigned int ctrlbits[S5PV210_POWER_DOMAIN_COUNT] = {
	S5PV210_PD_AUDIO_SHIFT,
	S5PV210_PD_CAM_SHIFT,
	S5PV210_PD_TV_SHIFT,
	S5PV210_PD_LCD_SHIFT,
	S5PV210_PD_G3D_SHIFT,
	S5PV210_PD_MFC_SHIFT,
};

static DEFINE_MUTEX(pd_lock);

static int s5pv210_pd_done(struct s5pv210_pm_domain *pd, bool ena)
{
	u32 val = 0;

	if (ena) {
		return regmap_read_poll_timeout(pd->reg_pmu,
				BLK_PWR_STAT_OFFSET, val,
				(val & pd->ctrlbit) == pd->ctrlbit, 1, 1000);
	} else {
		return regmap_read_poll_timeout(pd->reg_pmu,
				BLK_PWR_STAT_OFFSET, val,
				(val & pd->ctrlbit) == 0, 1, 1000);
	}
}

static int s5pv210_pd_power(struct generic_pm_domain *domain, bool ena)
{
	struct s5pv210_pm_domain *pd;
	u32 ctrlbit;
	int ret;

	pd = container_of(domain, struct s5pv210_pm_domain, genpd);
	ctrlbit = pd->ctrlbit;

	ret = clk_bulk_prepare_enable(pd->num_clks, pd->clks);
	if (ret < 0) {
		pr_err("%s: failed to enable clocks", __func__);
		return ret;
	}

	mutex_lock(&pd_lock);

	regmap_write_bits(pd->reg_pmu, NORMAL_CFG_OFFSET, ctrlbit,
			ena ? ctrlbit : 0);

	ret = s5pv210_pd_done(pd, ena);
	if (ret < 0)
		pr_err("%s: timed out waiting for lock", __func__);

	mutex_unlock(&pd_lock);

	clk_bulk_disable_unprepare(pd->num_clks, pd->clks);

	return ret;
}

static int s5pv210_pd_power_on(struct generic_pm_domain *domain)
{
	return s5pv210_pd_power(domain, true);
}

static int s5pv210_pd_power_off(struct generic_pm_domain *domain)
{
	return s5pv210_pd_power(domain, false);
}

static int s5pv210_pm_domain_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct s5pv210_pm_domain *s5pv210_pd;
	struct genpd_onecell_data *s5pv210_pd_data;
	struct generic_pm_domain **domains;
	struct regmap *reg_pmu;
	int i, ret;

	if (!np) {
		dev_err(dev, "device tree node not found\n");
		return -ENODEV;
	}

	s5pv210_pd = devm_kcalloc(dev, S5PV210_POWER_DOMAIN_COUNT,
					sizeof(*s5pv210_pd), GFP_KERNEL);
	if (!s5pv210_pd)
		return -ENOMEM;

	s5pv210_pd_data = devm_kzalloc(dev, sizeof(*s5pv210_pd_data),
					GFP_KERNEL);
	if (!s5pv210_pd_data)
		return -ENOMEM;

	domains = devm_kcalloc(dev, S5PV210_POWER_DOMAIN_COUNT,
				sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	reg_pmu = syscon_regmap_lookup_by_phandle(np,
			"samsung,pmureg-phandle");
	if (IS_ERR(reg_pmu)) {
		dev_err(dev, "Failed to map PMU registers");
		return PTR_ERR(reg_pmu);
	}

	/* Audio clocks */
	s5pv210_pd[0].clks[0].id = "i2s_audss";
	s5pv210_pd[0].num_clks = 1;

	/* Camera clocks */
	s5pv210_pd[1].clks[0].id = "fimc0";
	s5pv210_pd[1].clks[1].id = "fimc1";
	s5pv210_pd[1].clks[2].id = "fimc2";
	s5pv210_pd[1].clks[3].id = "sclk_csis";
	s5pv210_pd[1].clks[4].id = "jpeg";
	s5pv210_pd[1].clks[5].id = "rot";
	s5pv210_pd[1].num_clks = 6;

	/* TV clocks */
	s5pv210_pd[2].clks[0].id = "vp";
	s5pv210_pd[2].clks[1].id = "mixer";
	s5pv210_pd[2].clks[2].id = "tvenc";
	s5pv210_pd[2].clks[3].id = "hdmi";
	s5pv210_pd[2].num_clks = 4;

	/* LCD clocks */
	s5pv210_pd[3].clks[0].id = "lcd";
	s5pv210_pd[3].clks[1].id = "dsim";
	s5pv210_pd[3].clks[2].id = "g2d";
	s5pv210_pd[3].num_clks = 3;

	/* G3D clocks */
	s5pv210_pd[4].clks[0].id = "g3d";
	s5pv210_pd[4].num_clks = 1;

	/* MFC clocks */
	s5pv210_pd[5].clks[0].id = "mfc";
	s5pv210_pd[5].num_clks = 1;

	for (i = 0; i < ARRAY_SIZE(ctrlbits); i++, ++s5pv210_pd) {
		domains[i] = &s5pv210_pd->genpd;

		s5pv210_pd->reg_pmu = reg_pmu;

		ret = devm_clk_bulk_get(dev, s5pv210_pd->num_clks,
				s5pv210_pd->clks);
		if (ret < 0) {
			dev_err(dev, "Failed to get clocks");
			return ret;
		}

		s5pv210_pd->ctrlbit = ctrlbits[i];
		sprintf(s5pv210_pd->name, "%s.%d", np->name, i);
		s5pv210_pd->genpd.name = s5pv210_pd->name;
		s5pv210_pd->genpd.power_off = s5pv210_pd_power_off;
		s5pv210_pd->genpd.power_on = s5pv210_pd_power_on;

		/*
		 * Treat all power domains as on at boot.
		 *
		 * Some domains may actually be off, but keep it this way
		 * for reference counting purpose.
		 */
		pm_genpd_init(&s5pv210_pd->genpd, NULL, false);
	}

	s5pv210_pd_data->domains = domains;
	s5pv210_pd_data->num_domains = S5PV210_POWER_DOMAIN_COUNT;

	of_genpd_add_provider_onecell(np, s5pv210_pd_data);

	return 0;
}

static const struct of_device_id s5pv210_power_domain_ids[] = {
	{ .compatible = "samsung,s5pv210-pd", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5pv210_power_domain_ids);

static struct platform_driver s5pv210_power_domain_driver = {
	.driver	= {
		.name = "s5pv210-pd",
		.of_match_table = s5pv210_power_domain_ids,
	},
	.probe = s5pv210_pm_domain_probe,
};
module_platform_driver(s5pv210_power_domain_driver);
