// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for NEC CE147 5MP CMOS Image Sensor SoC
 *
 * Copyright (C) 2019 Jonathan Bakker <xc-racer2@live.ca>
 *
 * Based on a driver authored by Tushar Behera <tushar.b@samsung.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/videodev2.h>
#include <media/drv-intf/exynos-fimc.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

// FIXME
static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable module debug trace. Set to 1 to enable.");

#define MODULE_NAME		"CE147"

#define	SENSOR_JPEG_SNAPSHOT_MEMSIZE	0x360000

/* Camera ISP command */
#define	CMD_VERSION			0x00
#define	DATA_VERSION_FW			0x00
#define	DATA_VERSION_DATE		0x01
#define	CMD_GET_BATCH_REFLECTION_STATUS	0x02
#define	DATA_VERSION_SENSOR		0x03
#define	CMD_HD_PREVIEW			0x03
#define	CMD_SET_WB			0x04
#define	DATA_VERSION_AF			0x05
#define	CMD_SET_FLASH_MANUAL		0x06
#define	CMD_SET_EXIF_CTRL		0x07
#define	CMD_AE_WB_LOCK			0x11
#define	CMD_SET_ANTI_BANDING		0x14
#define	CMD_SET_WB_AUTO			0x1A
#define	CMD_SET_AUTO_FOCUS_MODE		0x20
#define	CMD_START_AUTO_FOCUS_SEARCH	0x23
#define	CMD_CHECK_AUTO_FOCUS_SEARCH	0x24
#define	CMD_STOP_LENS_MOVEMENT		0x35
#define	CMD_SET_EFFECT			0x3D
#define	CMD_SET_TOUCH_AUTO_FOCUS	0x4D
#define	CMD_START_OT			0x50
#define	CMD_CHECK_OT			0x51
#define	CMD_PREVIEW_SIZE		0x54
#define	CMD_FPS				0x5A
#define	CMD_SET_ANTI_SHAKE		0x5B
#define	CMD_DATA_READY			0x61
#define	CMD_SET_DATA			0x65
#define	CMD_DATA_OUT_REQ		0x66
#define	CMD_PREVIEW			0x6B
#define	CMD_PREVIEW_STATUS		0x6C
#define	CMD_CAPTURE_SIZE		0x73
#define	CMD_BUFFERING_CAPTURE		0x74
#define	CMD_SET_SMART_AUTO		0x82
#define	CMD_GET_SMART_AUTO_STATUS	0x83
#define	CMD_SET_WDR			0x88
#define	CMD_JPEG_SIZE			0x8E
#define	CMD_JPEG_BUFFERING		0x8F
#define	CMD_JPEG_CONFIG			0x90
#define	CMD_JPEG_BUFFERING2		0x92
#define	CMD_SET_FACE_DETECTION		0x9A
#define	CMD_SET_FACE_LOCK		0x9C
#define	CMD_INFO_EXIF			0xA0
#define	CMD_INFO_MODEL			0xA1
#define	CMD_INFO_ROT			0xA2
#define	CMD_INFO_LONGITUDE_LATITUDE	0xA3
#define	CMD_INFO_ALTITUDE		0xA4
#define	CMD_GPS_TIMESTAMP		0xA7
#define	CMD_SET_FLASH			0xB2
#define	CMD_SET_FLASH_POWER             0xB3
#define	CMD_SET_DZOOM			0xB9
#define	CMD_GET_DZOOM_LEVEL		0xBA
#define	CMD_SET_EFFECT_SHOT		0xC0
#define	DATA_VERSION_GAMMA		0xE0
#define	DATA_VERSION_SENSOR_MAKER	0xE0
#define	CMD_CHECK_DATALINE		0xEC
#define	CMD_INIT			0xF0
#define	CMD_FW_INFO			0xF2
#define	CMD_FWU_UPDATE			0xF3
#define	CMD_FW_UPDATE			0xF4
#define	CMD_FW_STATUS			0xF5
#define	CMD_FW_DUMP			0xFB

enum ce147_state {
	CE147_OFF,
	CE147_POWERED,
	CE147_STREAMING,
};

enum ce147_vendor {
	CAM_VENDOR_UNKNOWN = 0,
	CAM_VENDOR_SAMSUNG_ELEC,
	CAM_VENDOR_TECHWIN,
	CAM_VENDOR_SAMSUNG_OPT,
};

enum ce147_frame_size {
	CE147_PREVIEW_QCIF = 0,
	CE147_PREVIEW_QVGA,
	CE147_PREVIEW_592x480,
	CE147_PREVIEW_VGA,
	CE147_PREVIEW_D1,
	CE147_PREVIEW_WVGA,
	CE147_PREVIEW_720P,
	CE147_PREVIEW_VERT_QCIF,
	CE147_PREVIEW_MAX = CE147_PREVIEW_VERT_QCIF,

	/* 640 x 480 */
	CE147_CAPTURE_VGA,
	/* 800 x 480 */
	CE147_CAPTURE_WVGA,
	/* 1600	x 960 */
	CE147_CAPTURE_W1MP,
	/* UXGA	 - 1600 x 1200 */
	CE147_CAPTURE_2MP,
	/* 35mm Academy Offset Standard 1.66 - 2048 x 1232, 2.4MP */
	CE147_CAPTURE_W2MP,
	/* QXGA	 - 2048 x 1536 */
	CE147_CAPTURE_3MP,
	/* WQXGA - 2560 x 1536 */
	CE147_CAPTURE_W4MP,
	/* 2560	x 1920 */
	CE147_CAPTURE_5MP,
	CE147_CAPTURE_MAX,
};

struct ce147_capture {
	u32 buf_size;
	u32 main;
	u32 thumb;
	u32 postview;
	u32 total;
};

struct ce147_date_code {
	u16 year;
	u16 month;
	u16 day;
};

struct ce147_format {
	u32 code;
	enum v4l2_colorspace colorspace;
};

struct ce147_frmsize {
	u32 code;
	enum ce147_frame_size frs;
	u16 width;
	u16 height;
	u16 max_fps;
};

static struct ce147_frmsize ce147_frmsize_list[] = {
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_QCIF,      176,  144, 120 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_QVGA,      320,  240, 120 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_592x480,   592,  480, 120 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_VGA,       640,  480, 120 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_D1,        720,  480, 30 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_WVGA,      800,  480, 30 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_720P,      1280, 720, 30 },
	{ MEDIA_BUS_FMT_YUYV8_2X8, CE147_PREVIEW_VERT_QCIF, 144,  176, 30 },
	/* Don't set max fps here, as captures are single-shot */
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_VGA,        640,  480 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_WVGA,       800,  480 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_W1MP,       1600, 960 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_2MP,        1600, 1200 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_W2MP,       2048, 1232 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_3MP,        2048, 1536 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_W4MP,       2560, 1536 },
	{ MEDIA_BUS_FMT_JPEG_1X8, CE147_CAPTURE_5MP,        2560, 1920 },
};

/* Supported pixel formats */
static const struct ce147_format ce147_formats[] = {
	{
		.code		= MEDIA_BUS_FMT_YUYV8_2X8,
		.colorspace	= V4L2_COLORSPACE_JPEG,
	},
	{
		.code		= MEDIA_BUS_FMT_JPEG_1X8,
		.colorspace	= V4L2_COLORSPACE_JPEG,
	},
};

static const char * const ce147_supply_name[] = {
	"isp_core", "isp_host", "isp_sys", "af",
	"sensor", "vddio", "dvdd", "avdd",
};

#define CE147_NUM_SUPPLIES ARRAY_SIZE(ce147_supply_name)

struct ce147_info {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler hdl;
	struct regulator_bulk_data supply[CE147_NUM_SUPPLIES];
	struct gpio_desc *gpio_nreset;
	struct gpio_desc *gpio_ena;
	struct clk *mclk;

	/* Protects the struct members below */
	struct mutex lock;

	enum ce147_vendor cam_vendor;
	struct ce147_date_code date;
	u16 fw_ver[2];
	u16 prm_ver[2];
	u16 sensor_ver;

	const struct ce147_frmsize *cur_framesize;
	u32 cur_fps;
	u32 quality;

	struct ce147_capture cap_info;

	enum ce147_state state;
};

static inline struct ce147_info *to_ce147(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct ce147_info, sd);
}

static inline struct v4l2_subdev *to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct ce147_info, hdl)->sd;
}

static int ce147_i2c_write(struct v4l2_subdev *sd, u8 *buf, int count)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret, retries = 2;
	struct i2c_msg msg;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = count;
	msg.buf = buf;

	while (retries--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			break;
		msleep(20);
	}

	if (ret < 0)
		v4l2_err(sd, "%s: i2c write fail: %d", __func__, ret);

	return ret;
}

static int ce147_i2c_write_cmd_read_resp(struct v4l2_subdev *sd, u8 *wbuf,
			int wlen, u8 *rbuf, int rlen)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret, retries = 2;
	struct i2c_msg msg;

	ret = ce147_i2c_write(sd, wbuf, wlen);
	if (ret < 0)
		return ret;

	msg.addr = client->addr;
	msg.flags = I2C_M_RD;
	msg.len = rlen;
	msg.buf = rbuf;

	while (retries--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			break;
		msleep(20);
	}

	if (ret < 0)
		v4l2_err(sd, "%s: i2c read fail: %d", __func__, ret);

	return (ret == 1) ? 0 : ret;
}

static int ce147_wait_for_status(struct v4l2_subdev *sd, u8 cmd, u8 val)
{
	int ret = 0, tries = 300;
	u8 stat;

	while (--tries > 0) {
		ret = ce147_i2c_write_cmd_read_resp(sd, &cmd, 1, &stat, 1);
		if (ret < 0 || stat == cmd)
			break;
		msleep(20);
	}

	return ret;
}

static int ce147_set_awb_lock(struct ce147_info *info, bool lock)
{
	u8 buf[2];

	buf[0] = CMD_AE_WB_LOCK;
	buf[1] = lock ? 0x11 : 0x00;

	return ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
}

static int ce147_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = to_sd(ctrl);
	struct ce147_info *info = to_ce147(sd);
	int ret = 0;

	v4l2_dbg(1, debug, sd, "%s: ctrl_id: %d, value: %d\n",
		 __func__, ctrl->id, ctrl->val);

	mutex_lock(&info->lock);
	/**
	 * If the device is not powered up by the host driver do
	 * not apply any controls to H/W at this time. Instead
	 * the controls will be restored right after power-up.
	 */
	if (info->state == CE147_OFF)
		goto unlock;

	switch (ctrl->id) {

	default:
		ret = -EINVAL;
	}
unlock:
	mutex_unlock(&info->lock);
	return ret;
}

static int ce147_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(ce147_formats))
		return -EINVAL;

	code->code = ce147_formats[code->index].code;
	return 0;
}

static struct ce147_frmsize *__ce147_find_framesize(u32 pixformat,
				u32 width, u32 height, bool exact)
{
	struct ce147_frmsize *match = &ce147_frmsize_list[0];
	struct ce147_frmsize *fsize;
	unsigned int err, min_err = ~0;
	int i;

	for (i = 0; i < ARRAY_SIZE(ce147_frmsize_list); i++) {
		fsize = &ce147_frmsize_list[i];
		if (fsize->code != pixformat)
			continue;

		err = abs(fsize->width - width)
				+ abs(fsize->height - height);

		if (err == 0) {
			return fsize;
		} else if (err < min_err) {
			min_err = err;
			match = fsize;
		}
	}

	return exact ? NULL : match;
}

static int ce147_enum_frame_interval(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_frame_interval_enum *fie)
{
	struct ce147_frmsize *framesize = NULL;
	int fps;

	/* Set the frame interval JPEG as an arbitrary 1/1 */
	if (fie->code == MEDIA_BUS_FMT_JPEG_1X8) {
		if (fie->index > 0)
			return -EINVAL;

		fie->interval.numerator = 1;
		fie->interval.denominator = 1;

		return 0;
	}

	framesize = __ce147_find_framesize(fie->code, fie->width,
				fie->height, true);
	if (framesize == NULL)
		return -EINVAL;

	/* Min fps is 7 */
	fps = fie->index + 7;

	if (fps > framesize->max_fps)
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = fps;

	return 0;
}

static int ce147_enum_frame_size(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_frame_size_enum *fse)
{
	switch (fse->index) {
	case 0:
		/* YUYV */
		fse->code = MEDIA_BUS_FMT_YUYV8_2X8;
		fse->min_width = 144;
		fse->max_width = 1280;
		fse->min_height = 144;
		fse->max_height = 720;
		break;
	case 1:
		/* JPEG */
		fse->code = MEDIA_BUS_FMT_JPEG_1X8;
		fse->min_width = 480;
		fse->max_width = 2560;
		fse->min_height = 640;
		fse->max_height = 1920;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ce147_get_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_pad_config *cfg,
			 struct v4l2_subdev_format *fmt)
{
	struct ce147_info *info = to_ce147(sd);
	struct v4l2_mbus_framefmt *mf;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		if (cfg) {
			mf = v4l2_subdev_get_try_format(sd, cfg, 0);
			fmt->format = *mf;
		}
		return 0;
	}
	mf = &fmt->format;

	mutex_lock(&info->lock);

	mf->width = info->cur_framesize->width;
	mf->height = info->cur_framesize->height;
	mf->code = info->cur_framesize->code;
	mf->colorspace = V4L2_COLORSPACE_JPEG;
	mf->field = V4L2_FIELD_NONE;

	mutex_unlock(&info->lock);
	return 0;
}

/* Return nearest media bus frame format. */
static const struct ce147_format *ce147_try_fmt(struct v4l2_subdev *sd,
					    struct v4l2_mbus_framefmt *mf)
{
	int i = ARRAY_SIZE(ce147_formats);

	while (--i)
		if (mf->code == ce147_formats[i].code)
			break;
	mf->code = ce147_formats[i].code;

	return &ce147_formats[i];
}

static int ce147_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *fmt)
{
	struct ce147_info *info = to_ce147(sd);
	const struct ce147_frmsize *size;
	const struct ce147_format *nf;
	struct v4l2_mbus_framefmt *mf;
	int ret = 0;

	nf = ce147_try_fmt(sd, &fmt->format);

	size = __ce147_find_framesize(nf->code, fmt->format.width,
				fmt->format.height, false);
	if (size == NULL)
		return -EINVAL;

	fmt->format.width = size->width;
	fmt->format.height = size->height;
	fmt->format.code = size->code;
	fmt->format.colorspace = V4L2_COLORSPACE_JPEG;
	fmt->format.field = V4L2_FIELD_NONE;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		if (cfg) {
			mf = v4l2_subdev_get_try_format(sd, cfg, 0);
			*mf = fmt->format;
		}
		return 0;
	}

	mutex_lock(&info->lock);

	if (info->state == CE147_STREAMING) {
		ret = -EBUSY;
		goto unlock;
	}

	info->cur_framesize = size;

	/* Adjust FPS if the previous was too fast */
	if (info->cur_framesize->max_fps < info->cur_fps)
		info->cur_fps = info->cur_framesize->max_fps;

unlock:
	mutex_unlock(&info->lock);

	return ret;
}

static int ce147_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	struct ce147_info *info = to_ce147(sd);

	if (pad != 0 || fd == NULL)
		return -EINVAL;

	/**
	 * .{get/set}_frame_desc is only used for compressed formats,
	 * so hardcode everything as we only support one config
	 */
	mutex_lock(&info->lock);

	fd->entry[0].length = clamp_val(fd->entry[0].length,
			info->cur_framesize->width *
			info->cur_framesize->height * 8,
			SENSOR_JPEG_SNAPSHOT_MEMSIZE);
	fd->entry[0].pixelcode = MEDIA_BUS_FMT_JPEG_1X8;
	fd->entry[0].flags = V4L2_MBUS_FRAME_DESC_FL_LEN_MAX;
	fd->num_entries = 1;

	mutex_unlock(&info->lock);

	return 0;
}

static int ce147_get_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct ce147_info *info = to_ce147(sd);

	if (fi->pad != 0)
		return -EINVAL;

	mutex_lock(&info->lock);

	fi->interval.numerator = 1;
	fi->interval.denominator = info->cur_fps;

	mutex_unlock(&info->lock);

	return 0;
}

static int ce147_set_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct ce147_info *info = to_ce147(sd);
	u32 fps, num, denom;
	int ret = 0;

	v4l2_dbg(1, debug, sd, "Setting %d/%d frame interval\n",
		 fi->interval.numerator, fi->interval.denominator);

	/* Convert to fps */
	num = fi->interval.numerator * 1000;
	denom = fi->interval.denominator * 1000;

	fps = denom / num;

	mutex_lock(&info->lock);

	if (info->state == CE147_STREAMING) {
		ret = -EBUSY;
		goto unlock;
	}

	/* Set capture frame interval to 1 */
	if (info->cur_framesize->code == MEDIA_BUS_FMT_JPEG_1X8) {
		fi->interval.numerator = 1;
		fi->interval.denominator = 1;
		info->cur_fps = 1;
		goto unlock;
	}

	if (info->cur_framesize->max_fps < fps)
		fps = info->cur_framesize->max_fps;
	else if (fps < 7)
		fps = 7;

	fi->interval.numerator = 1;
	fi->interval.denominator = fps;
	info->cur_fps = fps;

unlock:
	mutex_unlock(&info->lock);

	return ret;
}

static int ce147_set_preview_size(struct ce147_info *info,
		enum ce147_frame_size frs)
{
	u8 size_buf[3];
	u8 hd_buf[2];
	int ret;

	hd_buf[0] = CMD_HD_PREVIEW;

	if (frs == CE147_PREVIEW_720P)
		hd_buf[1] = 0x01;
	else
		hd_buf[1] = 0x00;

	ret = ce147_i2c_write(&info->sd, hd_buf, ARRAY_SIZE(hd_buf));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting hd preview", __func__);
		return ret;
	}

	size_buf[0] = CMD_PREVIEW_SIZE;
	size_buf[2] = 0x01;

	switch (frs) {
	case CE147_PREVIEW_QCIF:
		size_buf[1] = 0x1E;
		break;
	case CE147_PREVIEW_QVGA:
		size_buf[1] = 0x02;
		break;
	case CE147_PREVIEW_592x480:
		size_buf[1] = 0x24;
		break;
	case CE147_PREVIEW_VGA:
		size_buf[1] = 0x04;
		break;
	case CE147_PREVIEW_WVGA:
		size_buf[1] = 0x13;
		break;
	case CE147_PREVIEW_D1:
		size_buf[1] = 0x20;
		break;
	case CE147_PREVIEW_720P:
		size_buf[1] = 0x16;
		size_buf[2] = 0x02;
		break;
	case CE147_PREVIEW_VERT_QCIF:
		size_buf[1] = 0x26;
		break;
	default:
		v4l2_err(&info->sd, "%s: unknown framesize", __func__);
		return -EINVAL;
	}

	ret = ce147_i2c_write(&info->sd, size_buf, ARRAY_SIZE(size_buf));
	if (ret < 0)
		v4l2_err(&info->sd, "%s: fail setting framesize", __func__);

	return ret;
}

static int ce147_set_frame_rate(struct ce147_info *info)
{
	u8 buf[3];
	int ret;

	buf[0] = CMD_FPS;
	buf[1] = 0x1E;
	buf[2] = info->cur_fps;

	ret = ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		v4l2_err(&info->sd, "%s: fail setting frame rate", __func__);

	return ret;
}

static int __ce147_start_preview(struct ce147_info *info)
{
	u8 buf[2];
	int ret;

	buf[0] = CMD_PREVIEW;
	buf[1] = 0x1;

	ret = ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		return ret;

	ret = ce147_wait_for_status(&info->sd, CMD_PREVIEW_STATUS, 0x08);
	if (ret < 0)
		v4l2_err(&info->sd, "%s: failed to start preview", __func__);

	return ret;
}

/* Called with info->lock held */
static int ce147_start_preview(struct ce147_info *info,
		enum ce147_frame_size frs)
{
	int ret;

	v4l2_dbg(1, debug, &info->sd, "%s: starting preview", __func__);

	ret = ce147_set_preview_size(info, frs);
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting size", __func__);
		return ret;
	}

	ret = ce147_set_frame_rate(info);
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting fps", __func__);
		return ret;
	}

	ret = __ce147_start_preview(info);
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: failed to start preview", __func__);
		return ret;
	}

	v4l2_dbg(1, debug, &info->sd, "%s: preview started", __func__);

	info->state = CE147_STREAMING;

	return 0;
}

/* Called with info->lock held */
static int ce147_stop_preview(struct ce147_info *info)
{
	u8 buf[2];
	int ret;

	buf[0] = CMD_PREVIEW;
	buf[1] = 0x0;

	ret = ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		return ret;

	ret = ce147_wait_for_status(&info->sd, CMD_PREVIEW_STATUS, 0x00);
	if (ret < 0)
		v4l2_err(&info->sd, "%s: failed to stop preview", __func__);

	v4l2_dbg(1, debug, &info->sd, "%s: preview stopped", __func__);

	return ret;
}

static int ce147_set_capture_size(struct ce147_info *info)
{
	u8 buf[5];

	buf[0] = CMD_CAPTURE_SIZE;
	buf[2] = 0x00;
	buf[3] = 0x01;
	buf[4] = 0x00;

	switch (info->cur_framesize->frs) {
	case CE147_CAPTURE_VGA:
		buf[1] = 0x04;
		break;
	case CE147_CAPTURE_WVGA:
		buf[1] = 0x13;
		break;
	case CE147_CAPTURE_W1MP:
		buf[1] = 0x0E;
		break;
	case CE147_CAPTURE_2MP:
		buf[1] = 0x08;
		break;
	case CE147_CAPTURE_W2MP:
		buf[1] = 0x0F;
		break;
	case CE147_CAPTURE_3MP:
		buf[1] = 0x09;
		break;
	case CE147_CAPTURE_W4MP:
		buf[1] = 0x15;
		break;
	case CE147_CAPTURE_5MP:
		buf[1] = 0x0B;
		break;
	default:
		v4l2_err(&info->sd, "%s: wrong capture res %d", __func__,
			 info->cur_framesize->frs);
		return -EINVAL;
	}

	return ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
}

static int ce147_set_capture_cmd(struct ce147_info *info)
{
	u8 buf[2];
	int ret;

	buf[0] = CMD_BUFFERING_CAPTURE;
	buf[1] = 0x00;

	ret = ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: failed setting cap cmd", __func__);
		return ret;
	}

	ret = ce147_wait_for_status(&info->sd, CMD_PREVIEW_STATUS, 0x0);
	if (ret < 0)
		v4l2_err(&info->sd, "%s: waiting preview stat fail", __func__);

	return ret;
}

static int ce147_set_exif_ctrl(struct ce147_info *info)
{
	u8 buf[3];

	/* Set to thumbnail but no GPS info */
	buf[0] = CMD_SET_EXIF_CTRL;
	buf[1] = 0x10;
	buf[2] = 0x00;

	return ce147_i2c_write(&info->sd, buf, ARRAY_SIZE(buf));
}

static int ce147_set_capture_exif(struct ce147_info *info)
{
	struct device_node *root;
	struct property *prop;
	struct rtc_time time;
	u8 model[130];
	u8 timestamp[8];
	u8 rot[2];
	int ret;

	/* Set time to current time */
	time = rtc_ktime_to_tm(ktime_get_coarse_real());
	time.tm_year += 1900;
	time.tm_mon += 1;

	timestamp[0] = CMD_INFO_EXIF;
	timestamp[1] = time.tm_year & 0x00ff;
	timestamp[2] = (time.tm_year & 0xff00) >> 8;
	timestamp[3] = time.tm_mon;
	timestamp[4] = time.tm_mday;
	timestamp[5] = time.tm_hour;
	timestamp[6] = time.tm_min;
	timestamp[7] = time.tm_sec;

	ret = ce147_i2c_write(&info->sd, timestamp, ARRAY_SIZE(timestamp));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting timestamp", __func__);
		return ret;
	}

	/* Always set rotation to 0 */
	rot[0] = CMD_INFO_ROT;
	rot[1] = 0x0;
	ret = ce147_i2c_write(&info->sd, rot, ARRAY_SIZE(rot));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting rotation", __func__);
		return ret;
	}

	/* Use the machine model name */
	model[0] = CMD_INFO_MODEL;
	model[1] = 0x06;
	model[2] = 0x09;
	root = of_find_node_by_path("/");
	if (root) {
		prop = of_find_property(root, "model", NULL);
		strncpy(&model[3], of_prop_next_string(prop, NULL), 127);
		of_node_put(root);
	} else {
		/* Default to CE147 */
		strncpy(&model[3], "CE147", 127);
	}

	ret = ce147_i2c_write(&info->sd, model, ARRAY_SIZE(model));
	if (ret < 0)
		v4l2_err(&info->sd, "%s: fail setting model", __func__);

	return ret;
}

static int ce147_set_jpeg_config(struct ce147_info *info)
{
	u8 qual[8];
	u8 buffering[3];
	unsigned int comp_ratio, min_comp_ratio;
	int ret;

	comp_ratio = info->quality / 100 + 8;
	min_comp_ratio = comp_ratio - 3;

	qual[0] = CMD_JPEG_CONFIG;
	qual[1] = 0x00;
	qual[2] = (comp_ratio * 100) & 0xFF;
	qual[3] = ((comp_ratio * 100) & 0xFF00) >> 8;
	qual[4] = (min_comp_ratio * 100) & 0xFF;
	qual[5] = ((min_comp_ratio * 100) & 0xFF00) >> 8;
	qual[6] = 0x05;
	qual[7] = 0x01;

	ret = ce147_i2c_write(&info->sd, qual, ARRAY_SIZE(qual));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting quality", __func__);
		return ret;
	}

	buffering[0] = CMD_JPEG_BUFFERING;
	buffering[1] = 0x00;

	switch (info->cur_framesize->frs) {
	case CE147_CAPTURE_VGA:
	case CE147_CAPTURE_2MP:
	case CE147_CAPTURE_3MP:
	case CE147_CAPTURE_5MP:
		buffering[2] = 0x04;
		break;
	default:
		buffering[2] = 0x13;
	}

	ret = ce147_i2c_write(&info->sd, buffering, ARRAY_SIZE(buffering));
	if (ret < 0)
		v4l2_err(&info->sd, "%s: fail setting buffering", __func__);

	return ret;
}

static int ce147_get_snapshot_data(struct ce147_info *info)
{
	u8 framesize_cmd[2];
	u8 setdata[3];
	u8 stat[3];
	u8 framesize_info[4];
	u8 cmd;
	int ret;

	/* Get main jpeg size */
	framesize_cmd[0] = CMD_JPEG_SIZE;
	framesize_cmd[1] = 0x00;

	ret = ce147_i2c_write_cmd_read_resp(&info->sd, framesize_cmd,
			ARRAY_SIZE(framesize_cmd), framesize_info,
			ARRAY_SIZE(framesize_info));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail getting main size", __func__);
		return ret;
	}

	info->cap_info.main = framesize_info[1] | framesize_info[2] << 8 |
			framesize_info[3] << 16;

	v4l2_dbg(1, debug, &info->sd, "%s: main size is %d", __func__,
			info->cap_info.main);

	/* Get thumbnail jpeg size */
	framesize_cmd[0] = CMD_JPEG_SIZE;
	framesize_cmd[1] = 0x01;

	ret = ce147_i2c_write_cmd_read_resp(&info->sd, framesize_cmd,
			ARRAY_SIZE(framesize_cmd), framesize_info,
			ARRAY_SIZE(framesize_info));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail getting thumb size", __func__);
		return ret;
	}

	info->cap_info.thumb = framesize_info[1] | framesize_info[2] << 8 |
			framesize_info[3] << 16;

	v4l2_dbg(1, debug, &info->sd, "%s: thumbnail size is %d", __func__,
			info->cap_info.thumb);

	/* Set data out */
	setdata[0] = CMD_SET_DATA;
	setdata[1] = 0x02;
	setdata[2] = 0x00;

	ret = ce147_i2c_write_cmd_read_resp(&info->sd, setdata,
			ARRAY_SIZE(setdata), stat, ARRAY_SIZE(stat));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail setting data", __func__);
		return ret;
	}

	v4l2_dbg(1, debug, &info->sd, "%s: size after set data out: %d",
			__func__, stat[0] | stat[1] << 8 | stat[2] << 16);

	/* Set request data */
	cmd = CMD_DATA_OUT_REQ;
	ret = ce147_i2c_write_cmd_read_resp(&info->sd, &cmd, 1,
			stat, ARRAY_SIZE(stat));
	if (ret < 0) {
		v4l2_err(&info->sd, "%s: fail requesting data", __func__);
		return ret;
	}

	v4l2_dbg(1, debug, &info->sd, "%s: size after set data req: %d",
			__func__, stat[0] | stat[1] << 8 | stat[2] << 16);

	/* Wait for done */
	ret = ce147_wait_for_status(&info->sd, CMD_DATA_READY, 0x00);
	if (ret < 0)
		v4l2_err(&info->sd, "%s: fail wait for data", __func__);

	return 0;
}

/* Called with info->lock held */
static int ce147_start_capture(struct ce147_info *info)
{
	struct v4l2_subdev *sd = &info->sd;
	int ret;

	v4l2_dbg(1, debug, sd, "starting capture sequence\n");

	/* In order for JPEG capture to work, we need the preview running */
	ret = ce147_start_preview(info, CE147_PREVIEW_VGA);
	if (ret < 0) {
		v4l2_err(sd, "%s: preview start fail", __func__);
		return ret;
	}

	ret = ce147_set_capture_size(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting capture size", __func__);
		return ret;
	}

	/* Set the AWB lock */
	// TODO - Restore this afterwards?
	ret = ce147_set_awb_lock(info, true);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting AWB lock", __func__);
		return ret;
	}

	ret = ce147_set_capture_cmd(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting capture cmd", __func__);
		return ret;
	}

	ret = ce147_set_exif_ctrl(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting exif ctrl", __func__);
		return ret;
	}

	ret = ce147_set_capture_exif(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting exif info", __func__);
		return ret;
	}

	ret = ce147_set_jpeg_config(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting exif info", __func__);
		return ret;
	}

	ret = ce147_get_snapshot_data(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: fail setting exif info", __func__);
		return ret;
	}

	v4l2_subdev_notify(&info->sd, S5P_FIMC_TX_END_NOTIFY,
			&info->cap_info.main);

	return 0;
}

static int ce147_s_stream(struct v4l2_subdev *sd, int on)
{
	struct ce147_info *info = to_ce147(sd);
	int ret = 0;

	v4l2_dbg(1, debug, sd, "%s: setting to %d\n", __func__, on);

	mutex_lock(&info->lock);

	if (on) {
		if (info->cur_framesize->code == MEDIA_BUS_FMT_YUYV8_2X8)
			ret = ce147_start_preview(info,
					info->cur_framesize->frs);
		else
			ret = ce147_start_capture(info);
	} else {
		if (info->cur_framesize->code == MEDIA_BUS_FMT_YUYV8_2X8)
			ret = ce147_stop_preview(info);
		/* else - no need to stop capture */

		info->state = CE147_POWERED;
	}

	mutex_unlock(&info->lock);

	return ret;
}

/* Called with info->lock held */
static int ce147_power_on(struct ce147_info *info)
{
	struct v4l2_subdev *sd = &info->sd;
	u8 buf[2];
	int ret;

	if (info->state != CE147_OFF) {
		v4l2_info(&info->sd, "%s: sensor is already on\n", __func__);
		return 0;
	}

	gpiod_set_value_cansleep(info->gpio_ena, 0);
	gpiod_set_value_cansleep(info->gpio_nreset, 0);

	ret = regulator_bulk_enable(CE147_NUM_SUPPLIES, info->supply);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = clk_prepare_enable(info->mclk);
	if (ret)
		return ret;

	usleep_range(1000, 1500);

	gpiod_set_value_cansleep(info->gpio_ena, 1);
	gpiod_set_value_cansleep(info->gpio_nreset, 1);

	msleep(30);

	buf[0] = CMD_INIT;
	buf[1] = 0x00;
	ret = ce147_i2c_write(sd, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		return ret;

	/**
	 * The delay required for the internal firmware
	 * for ce147 camera ISP to fully load varies based on
	 * the vendor
	 */
	if (info->cam_vendor == CAM_VENDOR_UNKNOWN ||
			info->cam_vendor == CAM_VENDOR_TECHWIN)
		msleep(800);
	else
		msleep(700);

	v4l2_dbg(1, debug, sd, "%s: powered up", __func__);

	info->state = CE147_POWERED;

	return 0;
}

/* Called with info->lock held */
static int ce147_power_off(struct ce147_info *info)
{
	int ret;

	if (info->state == CE147_OFF) {
		v4l2_info(&info->sd, "%s: sensor is already off\n", __func__);
		return 0;
	}

	gpiod_set_value_cansleep(info->gpio_nreset, 0);

	clk_disable_unprepare(info->mclk);

	gpiod_set_value_cansleep(info->gpio_ena, 0);

	ret = regulator_bulk_disable(CE147_NUM_SUPPLIES, info->supply);
	if (ret < 0)
		return ret;

	usleep_range(5000, 6500);

	v4l2_dbg(1, debug, &info->sd, "%s: powered off", __func__);

	info->state = CE147_OFF;

	return ret;
}

static int ce147_set_af_softlanding(struct v4l2_subdev *sd)
{
	int ret;
	u8 buf[2];

	buf[0] = CMD_SET_AUTO_FOCUS_MODE;
	buf[1] = 0x08;

	ret = ce147_i2c_write(sd, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		return ret;

	ret = ce147_wait_for_status(sd, CMD_CHECK_AUTO_FOCUS_SEARCH, 0x08);
	if (ret < 0)
		v4l2_warn(sd, "%s: soft landing fail", __func__);

	return 0;
}

/**
 * ce147_s_power - Main sensor power control function
 * @sd: sub-device, as pointed by struct v4l2_subdev
 * @on: if true, powers on the device; powers off otherwise.
 *
 * To prevent breaking the lens when the sensor is powered off the Soft-Landing
 * algorithm is called on shutdown
 */
static int ce147_s_power(struct v4l2_subdev *sd, int on)
{
	struct ce147_info *info = to_ce147(sd);
	int ret;

	v4l2_dbg(1, debug, sd, "%s: setting power to %d", __func__, on);

	mutex_lock(&info->lock);

	if (on) {
		ret = ce147_power_on(info);
	} else {
		ret = ce147_set_af_softlanding(sd);
		if (ret < 0)
			v4l2_err(sd, "%s: soft landing fail", __func__);
		ret = ce147_power_off(info);
	}

	mutex_unlock(&info->lock);

	/* Restore the controls state */
	if (ret >= 0 && on)
		ret = v4l2_ctrl_handler_setup(&info->hdl);

	return ret;
}

static int ce147_log_status(struct v4l2_subdev *sd)
{
	struct ce147_info *info = to_ce147(sd);

	v4l2_ctrl_handler_log_status(&info->hdl, sd->name);

	return 0;
}

/* V4L2 subdev internal operations */
static int ce147_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ce147_info *info = to_ce147(sd);
	struct v4l2_mbus_framefmt *mf =
			v4l2_subdev_get_try_format(sd, fh->pad, 0);

	mutex_lock(&info->lock);

	mf->width = info->cur_framesize->width;
	mf->height = info->cur_framesize->height;
	mf->code = info->cur_framesize->code;
	mf->colorspace = V4L2_COLORSPACE_JPEG;
	mf->field = V4L2_FIELD_NONE;

	mutex_unlock(&info->lock);

	return 0;
}

static const struct v4l2_ctrl_ops ce147_ctrl_ops = {
	.s_ctrl		= ce147_s_ctrl,
};

static const struct v4l2_subdev_internal_ops ce147_subdev_internal_ops = {
	.open		= ce147_open,
};

static const struct v4l2_subdev_core_ops ce147_core_ops = {
	.s_power	= ce147_s_power,
	.log_status	= ce147_log_status,
};

static const struct v4l2_subdev_pad_ops ce147_pad_ops = {
	.enum_mbus_code		= ce147_enum_mbus_code,
	.enum_frame_interval	= ce147_enum_frame_interval,
	.enum_frame_size	= ce147_enum_frame_size,
	.get_fmt		= ce147_get_fmt,
	.set_fmt		= ce147_set_fmt,
	.get_frame_desc		= ce147_frame_desc,
	.set_frame_desc		= ce147_frame_desc,
};

static const struct v4l2_subdev_video_ops ce147_video_ops = {
	.g_frame_interval	= ce147_get_frame_interval,
	.s_frame_interval	= ce147_set_frame_interval,
	.s_stream		= ce147_s_stream,
};

static const struct v4l2_subdev_ops ce147_ops = {
	.core		= &ce147_core_ops,
	.pad		= &ce147_pad_ops,
	.video		= &ce147_video_ops,
};

static int ce147_get_version(struct v4l2_subdev *sd, u8 which, u8 *values)
{
	u8 buf[2];

	buf[0] = CMD_VERSION;
	buf[1] = which;

	return ce147_i2c_write_cmd_read_resp(sd, buf, ARRAY_SIZE(buf),
				values, 4);
}

static int ce147_get_fw_info(struct v4l2_subdev *sd)
{
	struct ce147_info *info = to_ce147(sd);
	u8 values[4];
	int ret;

	/* Temporarily power on the camera */
	ret = ce147_power_on(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: failed to power on camera", __func__);
		return ret;
	}

	ret = ce147_get_version(sd, DATA_VERSION_FW, values);
	if (ret < 0) {
		v4l2_err(sd, "%s: failed to get main version", __func__);
		return ret;
	}

	info->fw_ver[0] = values[1];
	info->fw_ver[1] = values[0];

	switch (values[1]) {
	case 0x05:
		info->cam_vendor = CAM_VENDOR_SAMSUNG_OPT;
		break;
	case 0x0F:
		info->cam_vendor = CAM_VENDOR_TECHWIN;
		break;
	case 0x31:
		info->cam_vendor = CAM_VENDOR_SAMSUNG_ELEC;
		break;
	default:
		v4l2_warn(sd, "%s: unknown vendor code: %d", __func__,
			  values[1]);
	}

	v4l2_info(sd, "%s: firmware version %d.%d", __func__,
		  info->fw_ver[0], info->fw_ver[1]);

	info->prm_ver[0] = values[3];
	info->prm_ver[1] = values[2];

	v4l2_info(sd, "%s: prm version %d.%d", __func__,
		  info->prm_ver[0], info->prm_ver[1]);

	ret = ce147_get_version(sd, DATA_VERSION_DATE, values);
	if (ret < 0) {
		v4l2_err(sd, "%s: failed to get date version", __func__);
		return ret;
	}

	info->date.year = values[0] - 'A' + 2007;
	info->date.month = values[1] - 'A' + 1;
	info->date.day = values[2];

	v4l2_info(sd, "%s: date code %d-%d-%d", __func__,
		  info->date.year, info->date.month, info->date.day);

	ret = ce147_get_version(sd, DATA_VERSION_SENSOR, values);
	if (ret < 0) {
		v4l2_err(sd, "%s: failed to get sensor version", __func__);
		return ret;
	}

	info->sensor_ver = values[0];

	v4l2_info(sd, "%s: sensor version %d", __func__, info->sensor_ver);

	ret = ce147_power_off(info);
	if (ret < 0) {
		v4l2_err(sd, "%s: failed to power off camera", __func__);
		return ret;
	}

	return 0;
}

static int ce147_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct ce147_info *info;
	struct v4l2_subdev *sd;
	int ret;
	int i;

	info = devm_kzalloc(&client->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	mutex_init(&info->lock);
	sd = &info->sd;
	v4l2_i2c_subdev_init(sd, client, &ce147_ops);

	sd->internal_ops = &ce147_subdev_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	v4l2_ctrl_handler_init(&info->hdl, 5);

	// TODO - add controls

	sd->ctrl_handler = &info->hdl;

	ret = info->hdl.error;
	if (ret)
		goto np_err;

	info->gpio_nreset = devm_gpiod_get(&client->dev,
			"nreset", GPIOD_OUT_HIGH);
	if (IS_ERR(info->gpio_nreset)) {
		ret = PTR_ERR(info->gpio_nreset);
		dev_err(&client->dev, "nreset gpio request fail: %d\n", ret);
		goto np_err;
	}

	info->gpio_ena = devm_gpiod_get(&client->dev,
			"ena", GPIOD_OUT_HIGH);
	if (IS_ERR(info->gpio_ena)) {
		ret = PTR_ERR(info->gpio_ena);
		dev_err(&client->dev, "nstandby gpiorequest fail: %d\n", ret);
		goto np_err;
	}

	info->mclk = devm_clk_get(&client->dev, "mclk");
	if (IS_ERR(info->mclk)) {
		ret = PTR_ERR(info->mclk);
		goto np_err;
	}

	ret = clk_set_rate(info->mclk, 24000000);
	if (ret < 0) {
		dev_err(&client->dev, "fail setting mclk to 24000000");
		goto np_err;
	}

	for (i = 0; i < CE147_NUM_SUPPLIES; i++)
		info->supply[i].supply = ce147_supply_name[i];

	ret = devm_regulator_bulk_get(&client->dev, CE147_NUM_SUPPLIES,
				 info->supply);
	if (ret)
		goto np_err;

	ret = ce147_get_fw_info(sd);
	if (ret < 0)
		goto np_err;

	/* Default to VGA preview mode at 30fps, 100% jpeg quality */
	info->cur_framesize = &ce147_frmsize_list[3];
	info->cur_fps = 30;
	info->quality = 100;

	info->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &info->pad);
	if (ret < 0)
		goto np_err;

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0)
		goto np_err;

	dev_info(&client->dev, "ce147: successfully probed");

	return 0;

np_err:
	v4l2_ctrl_handler_free(&info->hdl);
	media_entity_cleanup(&sd->entity);
	return ret;
}

static int ce147_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ce147_info *info = to_ce147(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&info->hdl);
	media_entity_cleanup(&sd->entity);

	return 0;
}

static const struct i2c_device_id ce147_id[] = {
	{ MODULE_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, ce147_id);

#ifdef CONFIG_OF
static const struct of_device_id ce147_of_match[] = {
	{ .compatible = "nec,ce147" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ce147_of_match);
#endif

static struct i2c_driver ce147_i2c_driver = {
	.driver = {
		.of_match_table	= of_match_ptr(ce147_of_match),
		.name = MODULE_NAME
	},
	.probe		= ce147_probe,
	.remove		= ce147_remove,
	.id_table	= ce147_id,
};

module_i2c_driver(ce147_i2c_driver);

MODULE_DESCRIPTION("Samsung CE147 camera driver");
MODULE_AUTHOR("Jonathan Bakker <xc-racer2@live.ca>");
MODULE_LICENSE("GPL");
