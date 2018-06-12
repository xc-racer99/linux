/*
 * S6E63M0 AMOLED panel driver.
 *
 * Author: InKi Dae  <inki.dae@samsung.com>
 *
 * Derived from drivers/video/omap/lcd-apollon.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_panel.h>

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/wait.h>

#include <video/of_videomode.h>
#include <video/videomode.h>

#include "s6e63m0_gamma.h"

#define SLEEPMSEC		0x1000
#define ENDDEF			0x2000
#define	DEFMASK			0xFF00
#define COMMAND_ONLY		0xFE
#define DATA_ONLY		0xFF

#define MIN_BRIGHTNESS		0
#define MAX_BRIGHTNESS		10

struct s6e63m0 {
	struct device			*dev;
	struct spi_device		*spi;
	unsigned int			power;
	unsigned int			current_brightness;
	unsigned int			gamma_mode;
	unsigned int			gamma_table_count;
	struct drm_panel		panel;
	struct backlight_device		*bd;

	struct regulator_bulk_data supplies[2];
	int reset_gpio;

	unsigned int reset_delay;
	unsigned int power_on_delay;
	unsigned int power_off_delay;
	struct videomode vm;
};

static const unsigned short seq_panel_condition_set[] = {
	0xF8, 0x01,
	DATA_ONLY, 0x27,
	DATA_ONLY, 0x27,
	DATA_ONLY, 0x07,
	DATA_ONLY, 0x07,
	DATA_ONLY, 0x54,
	DATA_ONLY, 0x9f,
	DATA_ONLY, 0x63,
	DATA_ONLY, 0x86,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x33,
	DATA_ONLY, 0x0d,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,

	ENDDEF, 0x0000
};

static const unsigned short seq_display_condition_set[] = {
	0xf2, 0x02,
	DATA_ONLY, 0x03,
	DATA_ONLY, 0x1c,
	DATA_ONLY, 0x10,
	DATA_ONLY, 0x10,

	0xf7, 0x03,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,

	ENDDEF, 0x0000
};

static const unsigned short seq_gamma_setting[] = {
	0xfa, 0x00,
	DATA_ONLY, 0x18,
	DATA_ONLY, 0x08,
	DATA_ONLY, 0x24,
	DATA_ONLY, 0x64,
	DATA_ONLY, 0x56,
	DATA_ONLY, 0x33,
	DATA_ONLY, 0xb6,
	DATA_ONLY, 0xba,
	DATA_ONLY, 0xa8,
	DATA_ONLY, 0xac,
	DATA_ONLY, 0xb1,
	DATA_ONLY, 0x9d,
	DATA_ONLY, 0xc1,
	DATA_ONLY, 0xc1,
	DATA_ONLY, 0xb7,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x9c,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x9f,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0xd6,

	0xfa, 0x01,

	ENDDEF, 0x0000
};

static const unsigned short seq_etc_condition_set[] = {
	0xf6, 0x00,
	DATA_ONLY, 0x8c,
	DATA_ONLY, 0x07,

	0xb3, 0xc,

	0xb5, 0x2c,
	DATA_ONLY, 0x12,
	DATA_ONLY, 0x0c,
	DATA_ONLY, 0x0a,
	DATA_ONLY, 0x10,
	DATA_ONLY, 0x0e,
	DATA_ONLY, 0x17,
	DATA_ONLY, 0x13,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x2a,
	DATA_ONLY, 0x24,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x1b,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x17,

	DATA_ONLY, 0x2b,
	DATA_ONLY, 0x26,
	DATA_ONLY, 0x22,
	DATA_ONLY, 0x20,
	DATA_ONLY, 0x3a,
	DATA_ONLY, 0x34,
	DATA_ONLY, 0x30,
	DATA_ONLY, 0x2c,
	DATA_ONLY, 0x29,
	DATA_ONLY, 0x26,
	DATA_ONLY, 0x25,
	DATA_ONLY, 0x23,
	DATA_ONLY, 0x21,
	DATA_ONLY, 0x20,
	DATA_ONLY, 0x1e,
	DATA_ONLY, 0x1e,

	0xb6, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x11,
	DATA_ONLY, 0x22,
	DATA_ONLY, 0x33,
	DATA_ONLY, 0x44,
	DATA_ONLY, 0x44,
	DATA_ONLY, 0x44,

	DATA_ONLY, 0x55,
	DATA_ONLY, 0x55,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,

	0xb7, 0x2c,
	DATA_ONLY, 0x12,
	DATA_ONLY, 0x0c,
	DATA_ONLY, 0x0a,
	DATA_ONLY, 0x10,
	DATA_ONLY, 0x0e,
	DATA_ONLY, 0x17,
	DATA_ONLY, 0x13,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x2a,
	DATA_ONLY, 0x24,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x1b,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x17,

	DATA_ONLY, 0x2b,
	DATA_ONLY, 0x26,
	DATA_ONLY, 0x22,
	DATA_ONLY, 0x20,
	DATA_ONLY, 0x3a,
	DATA_ONLY, 0x34,
	DATA_ONLY, 0x30,
	DATA_ONLY, 0x2c,
	DATA_ONLY, 0x29,
	DATA_ONLY, 0x26,
	DATA_ONLY, 0x25,
	DATA_ONLY, 0x23,
	DATA_ONLY, 0x21,
	DATA_ONLY, 0x20,
	DATA_ONLY, 0x1e,
	DATA_ONLY, 0x1e,

	0xb8, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x11,
	DATA_ONLY, 0x22,
	DATA_ONLY, 0x33,
	DATA_ONLY, 0x44,
	DATA_ONLY, 0x44,
	DATA_ONLY, 0x44,

	DATA_ONLY, 0x55,
	DATA_ONLY, 0x55,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,

	0xb9, 0x2c,
	DATA_ONLY, 0x12,
	DATA_ONLY, 0x0c,
	DATA_ONLY, 0x0a,
	DATA_ONLY, 0x10,
	DATA_ONLY, 0x0e,
	DATA_ONLY, 0x17,
	DATA_ONLY, 0x13,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x2a,
	DATA_ONLY, 0x24,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x1b,
	DATA_ONLY, 0x1a,
	DATA_ONLY, 0x17,

	DATA_ONLY, 0x2b,
	DATA_ONLY, 0x26,
	DATA_ONLY, 0x22,
	DATA_ONLY, 0x20,
	DATA_ONLY, 0x3a,
	DATA_ONLY, 0x34,
	DATA_ONLY, 0x30,
	DATA_ONLY, 0x2c,
	DATA_ONLY, 0x29,
	DATA_ONLY, 0x26,
	DATA_ONLY, 0x25,
	DATA_ONLY, 0x23,
	DATA_ONLY, 0x21,
	DATA_ONLY, 0x20,
	DATA_ONLY, 0x1e,
	DATA_ONLY, 0x1e,

	0xba, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x11,
	DATA_ONLY, 0x22,
	DATA_ONLY, 0x33,
	DATA_ONLY, 0x44,
	DATA_ONLY, 0x44,
	DATA_ONLY, 0x44,

	DATA_ONLY, 0x55,
	DATA_ONLY, 0x55,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,
	DATA_ONLY, 0x66,

	0xc1, 0x4d,
	DATA_ONLY, 0x96,
	DATA_ONLY, 0x1d,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x01,
	DATA_ONLY, 0xdf,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x03,
	DATA_ONLY, 0x1f,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x00,
	DATA_ONLY, 0x03,
	DATA_ONLY, 0x06,
	DATA_ONLY, 0x09,
	DATA_ONLY, 0x0d,
	DATA_ONLY, 0x0f,
	DATA_ONLY, 0x12,
	DATA_ONLY, 0x15,
	DATA_ONLY, 0x18,

	0xb2, 0x10,
	DATA_ONLY, 0x10,
	DATA_ONLY, 0x0b,
	DATA_ONLY, 0x05,

	ENDDEF, 0x0000
};

static const unsigned short seq_acl_on[] = {
	/* ACL on */
	0xc0, 0x01,

	ENDDEF, 0x0000
};

static const unsigned short seq_acl_off[] = {
	/* ACL off */
	0xc0, 0x00,

	ENDDEF, 0x0000
};

static const unsigned short seq_elvss_on[] = {
	/* ELVSS on */
	0xb1, 0x0b,

	ENDDEF, 0x0000
};

static const unsigned short seq_elvss_off[] = {
	/* ELVSS off */
	0xb1, 0x0a,

	ENDDEF, 0x0000
};

static const unsigned short seq_stand_by_off[] = {
	0x11, COMMAND_ONLY,

	ENDDEF, 0x0000
};

static const unsigned short seq_stand_by_on[] = {
	0x10, COMMAND_ONLY,

	ENDDEF, 0x0000
};

static const unsigned short seq_display_on[] = {
	0x29, COMMAND_ONLY,

	ENDDEF, 0x0000
};

static inline struct s6e63m0 *panel_to_s6e63m0(struct drm_panel *panel)
{
	return container_of(panel, struct s6e63m0, panel);
}


static int s6e63m0_spi_write_byte(struct s6e63m0 *lcd, int addr, int data)
{
	u16 buf[1];
	struct spi_message msg;

	struct spi_transfer xfer = {
		.len		= 2,
		.tx_buf		= buf,
	};

	buf[0] = (addr << 8) | data;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(lcd->spi, &msg);
}

static int s6e63m0_spi_write(struct s6e63m0 *lcd, unsigned char address,
	unsigned char command)
{
	int ret = 0;

	if (address != DATA_ONLY)
		ret = s6e63m0_spi_write_byte(lcd, 0x0, address);
	if (command != COMMAND_ONLY)
		ret = s6e63m0_spi_write_byte(lcd, 0x1, command);

	return ret;
}

static int s6e63m0_panel_send_sequence(struct s6e63m0 *lcd,
	const unsigned short *wbuf)
{
	int ret = 0, i = 0;

	while ((wbuf[i] & DEFMASK) != ENDDEF) {
		if ((wbuf[i] & DEFMASK) != SLEEPMSEC) {
			ret = s6e63m0_spi_write(lcd, wbuf[i], wbuf[i+1]);
			if (ret)
				break;
		} else {
			msleep(wbuf[i+1]);
		}
		i += 2;
	}

	return ret;
}

static int _s6e63m0_gamma_ctl(struct s6e63m0 *lcd, const unsigned int *gamma)
{
	unsigned int i = 0;
	int ret = 0;

	/* disable gamma table updating. */
	ret = s6e63m0_spi_write(lcd, 0xfa, 0x00);
	if (ret) {
		dev_err(lcd->dev, "failed to disable gamma table updating.\n");
		goto gamma_err;
	}

	for (i = 0 ; i < GAMMA_TABLE_COUNT; i++) {
		ret = s6e63m0_spi_write(lcd, DATA_ONLY, gamma[i]);
		if (ret) {
			dev_err(lcd->dev, "failed to set gamma table.\n");
			goto gamma_err;
		}
	}

	/* update gamma table. */
	ret = s6e63m0_spi_write(lcd, 0xfa, 0x01);
	if (ret)
		dev_err(lcd->dev, "failed to update gamma table.\n");

gamma_err:
	return ret;
}

static int s6e63m0_gamma_ctl(struct s6e63m0 *lcd, int gamma)
{
	int ret = 0;

	ret = _s6e63m0_gamma_ctl(lcd, gamma_table.gamma_22_table[gamma]);

	return ret;
}


static int s6e63m0_ldi_init(struct s6e63m0 *lcd)
{
	int ret, i;
	const unsigned short *init_seq[] = {
		seq_panel_condition_set,
		seq_display_condition_set,
		seq_gamma_setting,
		seq_etc_condition_set,
		seq_acl_on,
		seq_elvss_on,
	};

	for (i = 0; i < ARRAY_SIZE(init_seq); i++) {
		ret = s6e63m0_panel_send_sequence(lcd, init_seq[i]);
		if (ret)
			break;
	}

	return ret;
}

static int s6e63m0_ldi_enable(struct s6e63m0 *lcd)
{
	int ret = 0, i;
	const unsigned short *enable_seq[] = {
		seq_stand_by_off,
		seq_display_on,
	};

	for (i = 0; i < ARRAY_SIZE(enable_seq); i++) {
		ret = s6e63m0_panel_send_sequence(lcd, enable_seq[i]);
		if (ret)
			break;
	}

	return ret;
}

static int s6e63m0_ldi_disable(struct s6e63m0 *lcd)
{
	int ret;

	ret = s6e63m0_panel_send_sequence(lcd, seq_stand_by_on);

	return ret;
}

static int s6e63m0_prepare(struct drm_panel *panel)
{
	struct s6e63m0 *lcd = panel_to_s6e63m0(panel);
	int ret = 0;
	struct backlight_device *bd;

	bd = lcd->bd;

	ret = regulator_bulk_enable(ARRAY_SIZE(lcd->supplies), lcd->supplies);
	if (ret)
		return ret;

	msleep(lcd->power_on_delay);

	gpio_direction_output(lcd->reset_gpio, 1);

	msleep(lcd->reset_delay);

	ret = s6e63m0_ldi_init(lcd);
	if (ret) {
		dev_err(lcd->dev, "failed to initialize ldi.\n");
		return ret;
	}

	ret = s6e63m0_ldi_enable(lcd);
	if (ret) {
		dev_err(lcd->dev, "failed to enable ldi.\n");
		return ret;
	}

	/* set brightness to current value after power on or resume. */
	ret = s6e63m0_gamma_ctl(lcd, bd->props.brightness);
	if (ret) {
		dev_err(lcd->dev, "lcd gamma setting failed.\n");
		return ret;
	}

	dev_info(lcd->dev, "s6e63m0 prepared");

	return 0;
}

static int s6e63m0_unprepare(struct drm_panel *panel)
{
	struct s6e63m0 *lcd = panel_to_s6e63m0(panel);
	int ret;

	ret = s6e63m0_ldi_disable(lcd);
	if (ret) {
		dev_err(lcd->dev, "lcd setting failed.\n");
		return -EIO;
	}

	msleep(lcd->power_off_delay);

	ret = regulator_bulk_disable(ARRAY_SIZE(lcd->supplies), lcd->supplies);
	if (ret)
		return ret;

	dev_info(lcd->dev, "s6e63m0 unprepared");

	return 0;
}



static int s6e63m0_set_brightness(struct backlight_device *bd)
{
	int ret = 0, brightness = bd->props.brightness;
	struct s6e63m0 *lcd = bl_get_data(bd);

	if (brightness < MIN_BRIGHTNESS ||
		brightness > bd->props.max_brightness) {
		dev_err(&bd->dev, "lcd brightness should be %d to %d.\n",
			MIN_BRIGHTNESS, MAX_BRIGHTNESS);
		return -EINVAL;
	}

	ret = s6e63m0_gamma_ctl(lcd, bd->props.brightness);
	if (ret) {
		dev_err(&bd->dev, "lcd brightness setting failed.\n");
		return -EIO;
	}

	return ret;
}

static int s6e63m0_disable(struct drm_panel *panel)
{
	// Nothing needed
	return 1;
}

static int s6e63m0_enable(struct drm_panel *panel)
{
	// Nothing needed
	return 0;
}

static int s6e63m0_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct s6e63m0 *lcd = panel_to_s6e63m0(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		DRM_ERROR("failed to create a new display mode\n");
		return 0;
	}

	drm_display_mode_from_videomode(&lcd->vm, mode);
	mode->width_mm = 52;
	mode->height_mm = 86;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs s6e63m0_funcs = {
	.disable = s6e63m0_disable,
	.unprepare = s6e63m0_unprepare,
	.prepare = s6e63m0_prepare,
	.enable = s6e63m0_enable,
	.get_modes = s6e63m0_get_modes,
};

static const struct backlight_ops s6e63m0_backlight_ops  = {
	.update_status = s6e63m0_set_brightness,
};

static ssize_t s6e63m0_sysfs_show_gamma_mode(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct s6e63m0 *lcd = dev_get_drvdata(dev);
	char temp[10];

	switch (lcd->gamma_mode) {
	case 0:
		sprintf(temp, "2.2 mode\n");
		strcat(buf, temp);
		break;
	case 1:
		sprintf(temp, "1.9 mode\n");
		strcat(buf, temp);
		break;
	case 2:
		sprintf(temp, "1.7 mode\n");
		strcat(buf, temp);
		break;
	default:
		dev_info(dev, "gamma mode could be 0:2.2, 1:1.9 or 2:1.7)n");
		break;
	}

	return strlen(buf);
}

static ssize_t s6e63m0_sysfs_store_gamma_mode(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct s6e63m0 *lcd = dev_get_drvdata(dev);
	struct backlight_device *bd = NULL;
	int brightness, rc;

	rc = kstrtouint(buf, 0, &lcd->gamma_mode);
	if (rc < 0)
		return rc;

	bd = lcd->bd;

	brightness = bd->props.brightness;

	switch (lcd->gamma_mode) {
	case 0:
		_s6e63m0_gamma_ctl(lcd, gamma_table.gamma_22_table[brightness]);
		break;
	case 1:
		_s6e63m0_gamma_ctl(lcd, gamma_table.gamma_19_table[brightness]);
		break;
	case 2:
		_s6e63m0_gamma_ctl(lcd, gamma_table.gamma_17_table[brightness]);
		break;
	default:
		dev_info(dev, "gamma mode could be 0:2.2, 1:1.9 or 2:1.7\n");
		_s6e63m0_gamma_ctl(lcd, gamma_table.gamma_22_table[brightness]);
		break;
	}
	return len;
}

static DEVICE_ATTR(gamma_mode, 0644,
		s6e63m0_sysfs_show_gamma_mode, s6e63m0_sysfs_store_gamma_mode);

static ssize_t s6e63m0_sysfs_show_gamma_table(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct s6e63m0 *lcd = dev_get_drvdata(dev);
	char temp[3];

	sprintf(temp, "%u\n", lcd->gamma_table_count);
	strcpy(buf, temp);

	return strlen(buf);
}
static DEVICE_ATTR(gamma_table, 0444,
		s6e63m0_sysfs_show_gamma_table, NULL);

static int s6e63m0_probe(struct spi_device *spi)
{
	int ret = 0;
	struct device_node *np = spi->dev.of_node;
	struct s6e63m0 *lcd = NULL;
	struct backlight_device *bd = NULL;
	struct backlight_properties props;

	if (!np) {
		dev_err(&spi->dev, "device must be instantiated using DT\n");
		return -EINVAL;
	}

	lcd = devm_kzalloc(&spi->dev, sizeof(struct s6e63m0), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	/* s6e63m0 panel uses 3-wire 9bits SPI Mode. */
	spi->bits_per_word = 9;

	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(&spi->dev, "spi setup failed.\n");
		return ret;
	}

	lcd->spi = spi;
	lcd->dev = &spi->dev;
	lcd->supplies[0].supply = "vdd3";
	lcd->supplies[1].supply = "vci";

	drm_panel_init(&lcd->panel);
	lcd->panel.dev = &spi->dev;
	lcd->panel.funcs = &s6e63m0_funcs;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS;

	bd = devm_backlight_device_register(&spi->dev, "s6e63m0bl-bl",
					&spi->dev, lcd, &s6e63m0_backlight_ops,
					&props);
	if (IS_ERR(bd)) {
		dev_err(&spi->dev, "backlight device registration failed\n");
		return PTR_ERR(bd);
	}

	bd->props.brightness = MAX_BRIGHTNESS;
	lcd->bd = bd;

	/*
	 * it gets gamma table count available so it gets user
	 * know that.
	 */
	lcd->gamma_table_count =
	    sizeof(gamma_table) / (MAX_GAMMA_LEVEL * sizeof(int *));

	ret = device_create_file(&(spi->dev), &dev_attr_gamma_mode);
	if (ret < 0)
		dev_err(&(spi->dev), "failed to add sysfs entries\n");

	ret = device_create_file(&(spi->dev), &dev_attr_gamma_table);
	if (ret < 0)
		dev_err(&(spi->dev), "failed to add sysfs entries\n");

	lcd->reset_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (lcd->reset_gpio < 0) {
		dev_err(&(spi->dev), "failed to get reset-gpios\n");
		return lcd->reset_gpio;
	}

	ret = devm_regulator_bulk_get(lcd->dev, ARRAY_SIZE(lcd->supplies),
					lcd->supplies);
	if (ret) {
		dev_err(&spi->dev, "failed to get regulators\n");
		return ret;
	}

	ret = devm_gpio_request_one(lcd->dev, lcd->reset_gpio,
					GPIOF_OUT_INIT_HIGH, "s6e63m0-reset");

	if (ret) {
		dev_err(&spi->dev, "failed to request reset GPIO\n");
		return ret;
	}

	ret = of_property_read_u32(np, "reset_delay", &(lcd->reset_delay));
	if (ret) {
		dev_info(&(spi->dev), "using default reset_delay of 120ms");
		lcd->reset_delay = 120;
	}

	ret = of_property_read_u32(np, "power_on_delay", &(lcd->power_on_delay));
	if (ret) {
		dev_info(&(spi->dev), "using default power_on_delay of 25ms");
		lcd->power_on_delay = 25;
	}

	ret = of_property_read_u32(np, "power_off_delay", &(lcd->power_off_delay));
	if (ret) {
		dev_info(&(spi->dev), "using default power_of_delay of 200ms");
		lcd->power_off_delay = 200;
	}

	ret = of_get_videomode(np, &lcd->vm, 0);
	if (ret < 0) {
		dev_err(&spi->dev, "failed to get video mode");
		return ret;
	}

	ret = drm_panel_add(&lcd->panel);
	if (ret < 0) {
		dev_err(&spi->dev, "failed to add drm panel");
		return ret;
	}

	spi_set_drvdata(spi, lcd);

	dev_info(&spi->dev, "s6e63m0 panel driver has been probed.\n");

	return 0;
}

static int s6e63m0_remove(struct spi_device *spi)
{
	struct s6e63m0 *lcd = spi_get_drvdata(spi);

	drm_panel_remove(&lcd->panel);
	device_remove_file(&spi->dev, &dev_attr_gamma_table);
	device_remove_file(&spi->dev, &dev_attr_gamma_mode);
	backlight_device_unregister(lcd->bd);

	return 0;
}

static const struct of_device_id s6e63m0_of_match[] = {
	{ .compatible = "samsung,s6e63m0", },
	{ /* sentinel */ }
};

static struct spi_driver s6e63m0_driver = {
	.driver = {
		.name	= "s6e63m0",
		.of_match_table = of_match_ptr(s6e63m0_of_match),
	},
	.probe		= s6e63m0_probe,
	.remove		= s6e63m0_remove,
};

module_spi_driver(s6e63m0_driver);

MODULE_AUTHOR("InKi Dae <inki.dae@samsung.com>");
MODULE_DESCRIPTION("S6E63M0 LCD Driver");
MODULE_LICENSE("GPL");

