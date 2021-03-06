/*
 * Analog Device ADV7180 video decoder driver
 *
 * Copyright (c) 2012-2014 Mentor Graphics Inc.
 * Copyright 2005-2012 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/semaphore.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/wait.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/imx6.h>

struct adv7180_dev {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct v4l2_subdev sd;
	struct v4l2_of_endpoint ep; /* the parsed DT endpoint info */
	struct v4l2_ctrl_handler ctrl_hdl;
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_captureparm streamcap;
	int rev_id;
	bool on;

	bool locked;             /* locked to signal */

	/* control settings */
	int brightness;
	int hue;
	int contrast;
	int saturation;
	int red;
	int green;
	int blue;
	int ae_mode;

	struct regulator *dvddio;
	struct regulator *dvdd;
	struct regulator *avdd;
	struct regulator *pvdd;
	int pwdn_gpio;

	v4l2_std_id std_id;

	/* Standard index of ADV7180. */
	int video_idx;

	/* Current analog input mux */
	int current_input;

	struct mutex mutex;
};

static inline struct adv7180_dev *to_adv7180_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct adv7180_dev, sd);
}

static inline struct adv7180_dev *ctrl_to_adv7180_dev(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct adv7180_dev, ctrl_hdl);
}

/*! List of input video formats supported. The video formats is corresponding
 * with v4l2 id in video_fmt_t
 */
enum {
	ADV7180_NTSC = 0,	/*!< Locked on (M) NTSC video signal. */
	ADV7180_PAL,		/*!< (B, G, H, I, N)PAL video signal. */
};

/*! Number of video standards supported (including 'not locked' signal). */
#define ADV7180_STD_MAX		(ADV7180_PAL + 1)

/*! Video format structure. */
struct video_fmt_t {
	int v4l2_id;		/*!< Video for linux ID. */
	char name[16];		/*!< Name (e.g., "NTSC", "PAL", etc.) */
	struct v4l2_rect raw;
	struct v4l2_rect crop;
};

/*! Description of video formats supported.
 *
 *  PAL: raw=720x625, crop=720x576.
 *  NTSC: raw=720x525, crop=720x480.
 */
static struct video_fmt_t video_fmts[] = {
	{       /* NTSC */
		.v4l2_id = V4L2_STD_NTSC,
		.name = "NTSC",
		.raw = {
			.width = 720,
			.height = 525,
		},
		.crop = {
			.width = 720,
			.height = 480,
			.top = 13,
			.left = 0,
		}
	}, {    /* (B, G, H, I, N) PAL */
		.v4l2_id = V4L2_STD_PAL,
		.name = "PAL",
		.raw = {
			.width = 720,
			.height = 625,
		},
		.crop = {
			.width = 720,
			.height = 576,
		},
	},
};

#define IF_NAME                    "adv7180"
#define ADV7180_INPUT_CTL              0x00	/* Input Control */
#define ADV7180_STATUS_1               0x10	/* Status #1 */
#define   ADV7180_IN_LOCK              (1 << 0)
#define   ADV7180_LOST_LOCK            (1 << 1)
#define   ADV7180_FSC_LOCK             (1 << 2)
#define   ADV7180_AD_RESULT_BIT        4
#define   ADV7180_AD_RESULT_MASK       (0x7 << ADV7180_AD_RESULT_BIT)
#define   ADV7180_AD_NTSC              0
#define   ADV7180_AD_NTSC_4_43         1
#define   ADV7180_AD_PAL_M             2
#define   ADV7180_AD_PAL_60            3
#define   ADV7180_AD_PAL               4
#define   ADV7180_AD_SECAM             5
#define   ADV7180_AD_PAL_N             6
#define   ADV7180_AD_SECAM_525         7
#define ADV7180_CONTRAST               0x08	/* Contrast */
#define ADV7180_BRIGHTNESS             0x0a	/* Brightness */
#define ADV7180_HUE_REG                0x0b	/* Signed, inverted */
#define ADV7180_IDENT                  0x11	/* IDENT */
#define ADV7180_VSYNC_FIELD_CTL_1      0x31	/* VSYNC Field Control #1 */
#define ADV7180_MANUAL_WIN_CTL         0x3d	/* Manual Window Control */
#define ADV7180_SD_SATURATION_CB       0xe3	/* SD Saturation Cb */
#define ADV7180_SD_SATURATION_CR       0xe4	/* SD Saturation Cr */
#define ADV7180_PWR_MNG                0x0f     /* Power Management */
#define ADV7180_INT_CONFIG_1           0x40     /* Interrupt Config 1 */
#define ADV7180_INT_STATUS_1           0x42     /* Interrupt Status 1 (r/o) */
#define   ADV7180_INT_SD_LOCK          (1 << 0)
#define   ADV7180_INT_SD_UNLOCK        (1 << 1)
#define ADV7180_INT_CLEAR_1            0x43     /* Interrupt Clear 1 (w/o) */
#define ADV7180_INT_MASK_1             0x44     /* Interrupt Mask 1 */
#define ADV7180_INT_STATUS_2           0x46     /* Interrupt Status 2 (r/o) */
#define ADV7180_INT_CLEAR_2            0x47     /* Interrupt Clear 2 (w/o) */
#define ADV7180_INT_MASK_2             0x48     /* Interrupt Mask 2 */
#define ADV7180_INT_RAW_STATUS_3       0x49   /* Interrupt Raw Status 3 (r/o) */
#define   ADV7180_INT_SD_V_LOCK        (1 << 1)
#define ADV7180_INT_STATUS_3           0x4a   /* Interrupt Status 3 (r/o) */
#define   ADV7180_INT_SD_V_LOCK_CHNG   (1 << 1)
#define   ADV7180_INT_SD_AD_CHNG       (1 << 3)
#define ADV7180_INT_CLEAR_3            0x4b     /* Interrupt Clear 3 (w/o) */
#define ADV7180_INT_MASK_3             0x4c     /* Interrupt Mask 3 */

/* supported controls */
/* This hasn't been fully implemented yet.
 * This is how it should work, though. */
static struct v4l2_queryctrl adv7180_qctrl[] = {
	{
		.id = V4L2_CID_BRIGHTNESS,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Brightness",
		.minimum = 0,		/* check this value */
		.maximum = 255,		/* check this value */
		.step = 1,		/* check this value */
		.default_value = 0,	/* check this value */
		.flags = 0,
	}, {
		.id = V4L2_CID_SATURATION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Saturation",
		.minimum = 0,		/* check this value */
		.maximum = 255,		/* check this value */
		.step = 0x1,		/* check this value */
		.default_value = 128,	/* check this value */
		.flags = 0,
	}, {
		.id = V4L2_CID_CONTRAST,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Contrast",
		.minimum = 0,
		.maximum = 255,
		.step = 0x1,
		.default_value = 128,
		.flags = 0,
	}, {
		.id = V4L2_CID_HUE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Hue",
		.minimum = -127,
		.maximum = 128,
		.step = 0x1,
		.default_value = 0,
		.flags = 0,
	}
};
#define ADV7180_NUM_CONTROLS ARRAY_SIZE(adv7180_qctrl)

struct adv7180_inputs_t {
	const char *desc;   /* Analog input description */
	u8 insel;           /* insel bits to select this input */
};

/* Analog Inputs on 64-Lead and 48-Lead LQFP */
static const struct adv7180_inputs_t adv7180_inputs_64_48[] = {
	{ .insel = 0x00, .desc = "ADV7180 Composite on Ain1" },
	{ .insel = 0x01, .desc = "ADV7180 Composite on Ain2" },
	{ .insel = 0x02, .desc = "ADV7180 Composite on Ain3" },
	{ .insel = 0x03, .desc = "ADV7180 Composite on Ain4" },
	{ .insel = 0x04, .desc = "ADV7180 Composite on Ain5" },
	{ .insel = 0x05, .desc = "ADV7180 Composite on Ain6" },
	{ .insel = 0x06, .desc = "ADV7180 Y/C on Ain1/4" },
	{ .insel = 0x07, .desc = "ADV7180 Y/C on Ain2/5" },
	{ .insel = 0x08, .desc = "ADV7180 Y/C on Ain3/6" },
	{ .insel = 0x09, .desc = "ADV7180 YPbPr on Ain1/4/5" },
	{ .insel = 0x0a, .desc = "ADV7180 YPbPr on Ain2/3/6" },
};
#define NUM_INPUTS_64_48 ARRAY_SIZE(adv7180_inputs_64_48)

#if 0
/*
 * FIXME: there is no way to distinguish LQFP vs LFCSP chips, so
 * we will just have to assume LQFP.
 */
/* Analog Inputs on 40-Lead and 32-Lead LFCSP */
static const struct adv7180_inputs_t adv7180_inputs_40_32[] = {
	{ .insel = 0x00, .desc = "ADV7180 Composite on Ain1" },
	{ .insel = 0x03, .desc = "ADV7180 Composite on Ain2" },
	{ .insel = 0x04, .desc = "ADV7180 Composite on Ain3" },
	{ .insel = 0x06, .desc = "ADV7180 Y/C on Ain1/2" },
	{ .insel = 0x09, .desc = "ADV7180 YPbPr on Ain1/2/3" },
};
#define NUM_INPUTS_40_32 ARRAY_SIZE(adv7180_inputs_40_32)
#endif

#define ADV7180_VOLTAGE_ANALOG               1800000
#define ADV7180_VOLTAGE_DIGITAL_CORE         1800000
#define ADV7180_VOLTAGE_DIGITAL_IO           3300000
#define ADV7180_VOLTAGE_PLL                  1800000

static int adv7180_regulator_enable(struct adv7180_dev *sensor)
{
	struct device *dev = sensor->dev;
	int ret = 0;

	sensor->dvddio = devm_regulator_get(dev, "DOVDD");
	if (!IS_ERR(sensor->dvddio)) {
		regulator_set_voltage(sensor->dvddio,
				      ADV7180_VOLTAGE_DIGITAL_IO,
				      ADV7180_VOLTAGE_DIGITAL_IO);
		ret = regulator_enable(sensor->dvddio);
		if (ret) {
			v4l2_err(&sensor->sd, "set io voltage failed\n");
			return ret;
		}
	} else
		v4l2_warn(&sensor->sd, "cannot get io voltage\n");

	sensor->dvdd = devm_regulator_get(dev, "DVDD");
	if (!IS_ERR(sensor->dvdd)) {
		regulator_set_voltage(sensor->dvdd,
				      ADV7180_VOLTAGE_DIGITAL_CORE,
				      ADV7180_VOLTAGE_DIGITAL_CORE);
		ret = regulator_enable(sensor->dvdd);
		if (ret) {
			v4l2_err(&sensor->sd, "set core voltage failed\n");
			return ret;
		}
	} else
		v4l2_warn(&sensor->sd, "cannot get core voltage\n");

	sensor->avdd = devm_regulator_get(dev, "AVDD");
	if (!IS_ERR(sensor->avdd)) {
		regulator_set_voltage(sensor->avdd,
				      ADV7180_VOLTAGE_ANALOG,
				      ADV7180_VOLTAGE_ANALOG);
		ret = regulator_enable(sensor->avdd);
		if (ret) {
			v4l2_err(&sensor->sd, "set analog voltage failed\n");
			return ret;
		}
	} else
		v4l2_warn(&sensor->sd, "cannot get analog voltage\n");

	sensor->pvdd = devm_regulator_get(dev, "PVDD");
	if (!IS_ERR(sensor->pvdd)) {
		regulator_set_voltage(sensor->pvdd,
				      ADV7180_VOLTAGE_PLL,
				      ADV7180_VOLTAGE_PLL);
		ret = regulator_enable(sensor->pvdd);
		if (ret) {
			v4l2_err(&sensor->sd, "set pll voltage failed\n");
			return ret;
		}
	} else
		v4l2_warn(&sensor->sd, "cannot get pll voltage\n");

	return ret;
}

static void adv7180_regulator_disable(struct adv7180_dev *sensor)
{
	if (sensor->dvddio)
		regulator_disable(sensor->dvddio);

	if (sensor->dvdd)
		regulator_disable(sensor->dvdd);

	if (sensor->avdd)
		regulator_disable(sensor->avdd);

	if (sensor->pvdd)
		regulator_disable(sensor->pvdd);
}

/***********************************************************************
 * I2C transfer.
 ***********************************************************************/

/*! Read one register from a ADV7180 i2c slave device.
 *
 *  @param *reg		register in the device we wish to access.
 *
 *  @return		       0 if success, an error code otherwise.
 */
static int adv7180_read_reg(struct adv7180_dev *sensor, u8 reg, u8 *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(sensor->i2c_client, reg);
	if (ret < 0) {
		v4l2_err(&sensor->sd, "%s: read reg error: reg=%2x\n",
			 __func__, reg);
		return ret;
	}

	*val = ret;
	return 0;
}

/*! Write one register of a ADV7180 i2c slave device.
 *
 *  @param *reg		register in the device we wish to access.
 *
 *  @return		       0 if success, an error code otherwise.
 */
static int adv7180_write_reg(struct adv7180_dev *sensor, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(sensor->i2c_client, reg, val);

	if (ret < 0)
		v4l2_err(&sensor->sd, "%s: write reg error:reg=%2x,val=%2x\n",
			 __func__, reg, val);
	return ret;
}

#define ADV7180_READ_REG(s, r, v) {				\
		ret = adv7180_read_reg((s), (r), (v));		\
		if (ret)					\
			return ret;				\
	}
#define ADV7180_WRITE_REG(s, r, v) {				\
		ret = adv7180_write_reg((s), (r), (v));		\
		if (ret)					\
			return ret;				\
	}

/* Read AD_RESULT to get the autodetected video standard */
static int adv7180_get_autodetect_std(struct adv7180_dev *sensor,
				      bool *status_change)
{
	int ad_result, idx = ADV7180_PAL;
	v4l2_std_id std = V4L2_STD_PAL;
	u8 stat1;
	int ret;

	*status_change = false;

	/*
	 * When the chip loses lock, it continues to send data at whatever
	 * standard was detected before, so leave the standard at the last
	 * detected standard.
	 */
	if (!sensor->locked)
		return 0; /* no status change */

	ADV7180_READ_REG(sensor, ADV7180_STATUS_1, &stat1);
	ad_result = (stat1 & ADV7180_AD_RESULT_MASK) >> ADV7180_AD_RESULT_BIT;

	switch (ad_result) {
	case ADV7180_AD_PAL:
		std = V4L2_STD_PAL;
		idx = ADV7180_PAL;
		break;
	case ADV7180_AD_PAL_M:
		std = V4L2_STD_PAL_M;
		/* PAL M is very similar to NTSC (same lines/field) */
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_PAL_N:
		std = V4L2_STD_PAL_N;
		idx = ADV7180_PAL;
		break;
	case ADV7180_AD_PAL_60:
		std = V4L2_STD_PAL_60;
		/* PAL 60 has same lines as NTSC */
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_NTSC:
		std = V4L2_STD_NTSC;
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_NTSC_4_43:
		std = V4L2_STD_NTSC_443;
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_SECAM:
		std = V4L2_STD_SECAM;
		idx = ADV7180_PAL;
		break;
	case ADV7180_AD_SECAM_525:
		/*
		 * FIXME: could not find any info on "SECAM 525", assume
		 * it is SECAM but with NTSC line standard.
		 */
		std = V4L2_STD_SECAM;
		idx = ADV7180_NTSC;
		break;
	}

	if (std != sensor->std_id) {
		sensor->video_idx = idx;
		sensor->std_id = std;
		sensor->fmt.width = video_fmts[sensor->video_idx].raw.width;
		sensor->fmt.height = video_fmts[sensor->video_idx].raw.height;
		*status_change = true;
	}

	return 0;
}

/* Update lock status */
static int adv7180_update_lock_status(struct adv7180_dev *sensor,
				      bool *status_change)
{
	u8 stat1, int_stat1, int_stat3, int_raw_stat3;
	int ret;

	ADV7180_READ_REG(sensor, ADV7180_STATUS_1, &stat1);

	/* Switch to interrupt register map */
	ADV7180_WRITE_REG(sensor, 0x0E, 0x20);

	ADV7180_READ_REG(sensor, ADV7180_INT_STATUS_1, &int_stat1);
	ADV7180_READ_REG(sensor, ADV7180_INT_STATUS_3, &int_stat3);
	/* clear the interrupts */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CLEAR_1, int_stat1);
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CLEAR_3, int_stat3);

	ADV7180_READ_REG(sensor, ADV7180_INT_RAW_STATUS_3, &int_raw_stat3);

	/* Switch back to normal register map */
	ADV7180_WRITE_REG(sensor, 0x0E, 0x00);

	*status_change = (((int_stat1 & ADV7180_INT_SD_LOCK) ||
			   (int_stat1 & ADV7180_INT_SD_UNLOCK) ||
			   (int_stat3 & ADV7180_INT_SD_V_LOCK_CHNG)) != 0);

	sensor->locked = ((stat1 & ADV7180_IN_LOCK) &&
			  (stat1 & ADV7180_FSC_LOCK) &&
			  (int_raw_stat3 & ADV7180_INT_SD_V_LOCK));

	return 0;
}

static void adv7180_power(struct adv7180_dev *sensor, bool enable)
{
	if (enable && !sensor->on) {
		if (gpio_is_valid(sensor->pwdn_gpio))
			gpio_set_value_cansleep(sensor->pwdn_gpio, 1);

		usleep_range(5000, 5001);
		adv7180_write_reg(sensor, ADV7180_PWR_MNG, 0);
	} else if (!enable && sensor->on) {
		adv7180_write_reg(sensor, ADV7180_PWR_MNG, 0x24);

		if (gpio_is_valid(sensor->pwdn_gpio))
			gpio_set_value_cansleep(sensor->pwdn_gpio, 0);
	}

	sensor->on = enable;
}

/* threaded irq handler */
static irqreturn_t adv7180_interrupt(int irq, void *dev_id)
{
	struct adv7180_dev *sensor = dev_id;
	bool std_change, lock_status_change;

	mutex_lock(&sensor->mutex);

	adv7180_update_lock_status(sensor, &lock_status_change);
	adv7180_get_autodetect_std(sensor, &std_change);

	mutex_unlock(&sensor->mutex);

	if (lock_status_change || std_change)
		v4l2_subdev_notify(&sensor->sd,
				   DECODER_STATUS_CHANGE_NOTIFY, NULL);

	return IRQ_HANDLED;
}

static const struct adv7180_inputs_t *
adv7180_find_input(struct adv7180_dev *sensor, u32 insel)
{
	int i;

	for (i = 0; i < NUM_INPUTS_64_48; i++) {
		if (insel == adv7180_inputs_64_48[i].insel)
			return &adv7180_inputs_64_48[i];
	}

	return NULL;
}

/* --------------- Subdev Operations --------------- */

static int adv7180_querystd(struct v4l2_subdev *sd, v4l2_std_id *std)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	bool std_change, lsc;
	int ret = 0;

	mutex_lock(&sensor->mutex);

	/*
	 * If we have the ADV7180 irq, we can just return the currently
	 * detected standard. Otherwise we have to poll the AD_RESULT
	 * bits every time querystd() is called.
	 */
	if (!sensor->i2c_client->irq) {
		ret = adv7180_update_lock_status(sensor, &lsc);
		if (ret)
			goto unlock;
		ret = adv7180_get_autodetect_std(sensor, &std_change);
		if (ret)
			goto unlock;
	}

	*std = sensor->std_id;

unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static int adv7180_s_power(struct v4l2_subdev *sd, int on)
{
	return 0;
}

static int adv7180_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *a)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	struct v4l2_captureparm *cparm = &a->parm.capture;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(a, 0, sizeof(*a));
	a->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cparm->capability = sensor->streamcap.capability;
	cparm->timeperframe = sensor->streamcap.timeperframe;
	cparm->capturemode = sensor->streamcap.capturemode;

	return 0;
}

static int adv7180_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *a)
{
	return 0;
}

static int adv7180_g_mbus_fmt(struct v4l2_subdev *sd,
			      struct v4l2_mbus_framefmt *fmt)

{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	*fmt = sensor->fmt;
	return 0;
}

/*
 * This driver autodetects a standard video mode, so we don't allow
 * setting a mode, just return the current autodetected mode.
 *
 * Return 0.
 */
static int adv7180_try_mbus_fmt(struct v4l2_subdev *sd,
				struct v4l2_mbus_framefmt *fmt)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	*fmt = sensor->fmt;
	return 0;
}

/*
 * This driver autodetects a standard video mode, so we don't allow
 * setting a mode, just return the current autodetected mode.
 *
 * Return 0.
 */
static int adv7180_s_mbus_fmt(struct v4l2_subdev *sd,
			      struct v4l2_mbus_framefmt *fmt)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	*fmt = sensor->fmt;
	return 0;
}


/* Controls */

static int adv7180_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct adv7180_dev *sensor = ctrl_to_adv7180_dev(ctrl);
	int ret = 0;
	u8 tmp;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_BRIGHTNESS, tmp);
		sensor->brightness = ctrl->val;
		break;
	case V4L2_CID_CONTRAST:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_CONTRAST, tmp);
		sensor->contrast = ctrl->val;
		break;
	case V4L2_CID_SATURATION:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_SD_SATURATION_CB, tmp);
		ADV7180_WRITE_REG(sensor, ADV7180_SD_SATURATION_CR, tmp);
		sensor->saturation = ctrl->val;
		break;
	case V4L2_CID_HUE:
		tmp = ctrl->val;
		/* Hue is inverted according to HSL chart */
		ADV7180_WRITE_REG(sensor, ADV7180_HUE_REG, -tmp);
		sensor->hue = ctrl->val;
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		break;
	case V4L2_CID_DO_WHITE_BALANCE:
		break;
	case V4L2_CID_RED_BALANCE:
		break;
	case V4L2_CID_BLUE_BALANCE:
		break;
	case V4L2_CID_GAMMA:
		break;
	case V4L2_CID_EXPOSURE:
		break;
	case V4L2_CID_AUTOGAIN:
		break;
	case V4L2_CID_GAIN:
		break;
	case V4L2_CID_HFLIP:
		break;
	case V4L2_CID_VFLIP:
		break;
	default:
		ret = -EPERM;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops adv7180_ctrl_ops = {
	.s_ctrl = adv7180_s_ctrl,
};

static int adv7180_init_controls(struct adv7180_dev *sensor)
{
	struct v4l2_queryctrl *c;
	int i;

	v4l2_ctrl_handler_init(&sensor->ctrl_hdl, ADV7180_NUM_CONTROLS);

	for (i = 0; i < ADV7180_NUM_CONTROLS; i++) {
		c = &adv7180_qctrl[i];

		v4l2_ctrl_new_std(&sensor->ctrl_hdl, &adv7180_ctrl_ops,
				  c->id, c->minimum, c->maximum,
				  c->step, c->default_value);
	}

	sensor->sd.ctrl_handler = &sensor->ctrl_hdl;
	if (sensor->ctrl_hdl.error) {
		int err = sensor->ctrl_hdl.error;

		v4l2_ctrl_handler_free(&sensor->ctrl_hdl);

		v4l2_err(&sensor->sd, "%s: error %d\n", __func__, err);
		return err;
	}
	v4l2_ctrl_handler_setup(&sensor->ctrl_hdl);

	return 0;
}

static int adv7180_enum_framesizes(struct v4l2_subdev *sd,
				   struct v4l2_frmsizeenum *fsize)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	if (fsize->index > 0)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = video_fmts[sensor->video_idx].crop.width;
	fsize->discrete.height = video_fmts[sensor->video_idx].crop.height;
	return 0;
}

static int adv7180_g_crop(struct v4l2_subdev *sd, struct v4l2_crop *a)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	a->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	a->c = video_fmts[sensor->video_idx].crop;

	return 0;
}

static int adv7180_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	mutex_lock(&sensor->mutex);

	*status = 0;

	if (sensor->on) {
		if (!sensor->locked)
			*status = V4L2_IN_ST_NO_SIGNAL | V4L2_IN_ST_NO_SYNC;
	} else
		*status = V4L2_IN_ST_NO_POWER;

	mutex_unlock(&sensor->mutex);

	return 0;
}

static int adv7180_s_routing(struct v4l2_subdev *sd, u32 input,
			     u32 output, u32 config)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	const struct adv7180_inputs_t *advinput;

	advinput = adv7180_find_input(sensor, input);
	if (!advinput)
		return -EINVAL;

	mutex_lock(&sensor->mutex);

	adv7180_write_reg(sensor, ADV7180_INPUT_CTL, advinput->insel);

	sensor->current_input = input;

	mutex_unlock(&sensor->mutex);

	return 0;
}

static int adv7180_enum_mbus_fmt(struct v4l2_subdev *sd, unsigned index,
				 u32 *code)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	if (index != 0)
		return -EINVAL;

	*code = sensor->fmt.code;

	return 0;
}

static int adv7180_g_mbus_config(struct v4l2_subdev *sd,
				struct v4l2_mbus_config *cfg)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	cfg->type = V4L2_MBUS_BT656;
	cfg->flags = sensor->ep.bus.parallel.flags;

	return 0;
}

static int adv7180_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static struct v4l2_subdev_core_ops adv7180_core_ops = {
	.s_power = adv7180_s_power,
	.g_ext_ctrls = v4l2_subdev_g_ext_ctrls,
	.try_ext_ctrls = v4l2_subdev_try_ext_ctrls,
	.s_ext_ctrls = v4l2_subdev_s_ext_ctrls,
	.g_ctrl = v4l2_subdev_g_ctrl,
	.s_ctrl = v4l2_subdev_s_ctrl,
	.queryctrl = v4l2_subdev_queryctrl,
	.querymenu = v4l2_subdev_querymenu,
};

static struct v4l2_subdev_video_ops adv7180_video_ops = {
	.enum_mbus_fmt = adv7180_enum_mbus_fmt,
	.try_mbus_fmt = adv7180_try_mbus_fmt,
	.g_mbus_fmt = adv7180_g_mbus_fmt,
	.s_mbus_fmt = adv7180_s_mbus_fmt,
	.s_parm = adv7180_s_parm,
	.g_parm = adv7180_g_parm,
	.enum_framesizes = adv7180_enum_framesizes,
	.g_crop = adv7180_g_crop,
	.g_input_status = adv7180_g_input_status,
	.s_routing = adv7180_s_routing,
	.querystd = adv7180_querystd,
	.g_mbus_config  = adv7180_g_mbus_config,
	.s_stream = adv7180_s_stream,
};

static struct v4l2_subdev_ops adv7180_subdev_ops = {
	.core = &adv7180_core_ops,
	.video = &adv7180_video_ops,
};

/***********************************************************************
 * I2C client and driver.
 ***********************************************************************/

/*! ADV7180 Reset function.
 *
 *  @return		None.
 */
static int adv7180_hard_reset(struct adv7180_dev *sensor)
{
	int ret;

	/* assert reset bit */
	adv7180_write_reg(sensor, ADV7180_PWR_MNG, 0x80);
	usleep_range(5000, 5001);

	/* Set analog mux for Composite Ain1 */
	ADV7180_WRITE_REG(sensor, ADV7180_INPUT_CTL, 0x00);

	/* Datasheet recommends */
	ADV7180_WRITE_REG(sensor, 0x01, 0xc8);
	ADV7180_WRITE_REG(sensor, 0x02, 0x04);
	ADV7180_WRITE_REG(sensor, 0x03, 0x00);
	ADV7180_WRITE_REG(sensor, 0x04, 0x45);
	ADV7180_WRITE_REG(sensor, 0x05, 0x00);
	ADV7180_WRITE_REG(sensor, 0x06, 0x02);
	ADV7180_WRITE_REG(sensor, 0x07, 0x7F);
	ADV7180_WRITE_REG(sensor, 0x08, 0x80);
	ADV7180_WRITE_REG(sensor, 0x0A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x0B, 0x00);
	ADV7180_WRITE_REG(sensor, 0x0C, 0x36);
	ADV7180_WRITE_REG(sensor, 0x0D, 0x7C);
	ADV7180_WRITE_REG(sensor, 0x0E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x0F, 0x00);
	ADV7180_WRITE_REG(sensor, 0x13, 0x00);
	ADV7180_WRITE_REG(sensor, 0x14, 0x12);
	ADV7180_WRITE_REG(sensor, 0x15, 0x00);
	ADV7180_WRITE_REG(sensor, 0x16, 0x00);
	ADV7180_WRITE_REG(sensor, 0x17, 0x01);
	ADV7180_WRITE_REG(sensor, 0x18, 0x93);
	ADV7180_WRITE_REG(sensor, 0xF1, 0x19);
	ADV7180_WRITE_REG(sensor, 0x1A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x1B, 0x00);
	ADV7180_WRITE_REG(sensor, 0x1C, 0x00);
	ADV7180_WRITE_REG(sensor, 0x1D, 0x40);
	ADV7180_WRITE_REG(sensor, 0x1E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x1F, 0x00);
	ADV7180_WRITE_REG(sensor, 0x20, 0x00);
	ADV7180_WRITE_REG(sensor, 0x21, 0x00);
	ADV7180_WRITE_REG(sensor, 0x22, 0x00);
	ADV7180_WRITE_REG(sensor, 0x23, 0xC0);
	ADV7180_WRITE_REG(sensor, 0x24, 0x00);
	ADV7180_WRITE_REG(sensor, 0x25, 0x00);
	ADV7180_WRITE_REG(sensor, 0x26, 0x00);
	ADV7180_WRITE_REG(sensor, 0x27, 0x58);
	ADV7180_WRITE_REG(sensor, 0x28, 0x00);
	ADV7180_WRITE_REG(sensor, 0x29, 0x00);
	ADV7180_WRITE_REG(sensor, 0x2A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x2B, 0xE1);
	ADV7180_WRITE_REG(sensor, 0x2C, 0xAE);
	ADV7180_WRITE_REG(sensor, 0x2D, 0xF4);
	ADV7180_WRITE_REG(sensor, 0x2E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x2F, 0xF0);
	ADV7180_WRITE_REG(sensor, 0x30, 0x00);
	ADV7180_WRITE_REG(sensor, 0x31, 0x12);
	ADV7180_WRITE_REG(sensor, 0x32, 0x41);
	ADV7180_WRITE_REG(sensor, 0x33, 0x84);
	ADV7180_WRITE_REG(sensor, 0x34, 0x00);
	ADV7180_WRITE_REG(sensor, 0x35, 0x02);
	ADV7180_WRITE_REG(sensor, 0x36, 0x00);
	ADV7180_WRITE_REG(sensor, 0x37, 0x01);
	ADV7180_WRITE_REG(sensor, 0x38, 0x80);
	ADV7180_WRITE_REG(sensor, 0x39, 0xC0);
	ADV7180_WRITE_REG(sensor, 0x3A, 0x10);
	ADV7180_WRITE_REG(sensor, 0x3B, 0x05);
	ADV7180_WRITE_REG(sensor, 0x3C, 0x58);
	ADV7180_WRITE_REG(sensor, 0x3D, 0xB2);
	ADV7180_WRITE_REG(sensor, 0x3E, 0x64);
	ADV7180_WRITE_REG(sensor, 0x3F, 0xE4);
	ADV7180_WRITE_REG(sensor, 0x40, 0x90);
	ADV7180_WRITE_REG(sensor, 0x41, 0x01);
	ADV7180_WRITE_REG(sensor, 0x42, 0x7E);
	ADV7180_WRITE_REG(sensor, 0x43, 0xA4);
	ADV7180_WRITE_REG(sensor, 0x44, 0xFF);
	ADV7180_WRITE_REG(sensor, 0x45, 0xB6);
	ADV7180_WRITE_REG(sensor, 0x46, 0x12);
	ADV7180_WRITE_REG(sensor, 0x48, 0x00);
	ADV7180_WRITE_REG(sensor, 0x49, 0x00);
	ADV7180_WRITE_REG(sensor, 0x4A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x4B, 0x00);
	ADV7180_WRITE_REG(sensor, 0x4C, 0x00);
	ADV7180_WRITE_REG(sensor, 0x4D, 0xEF);
	ADV7180_WRITE_REG(sensor, 0x4E, 0x08);
	ADV7180_WRITE_REG(sensor, 0x4F, 0x08);
	ADV7180_WRITE_REG(sensor, 0x50, 0x08);
	ADV7180_WRITE_REG(sensor, 0x51, 0xA4);
	ADV7180_WRITE_REG(sensor, 0x52, 0x0B);
	ADV7180_WRITE_REG(sensor, 0x53, 0x4E);
	ADV7180_WRITE_REG(sensor, 0x54, 0x80);
	ADV7180_WRITE_REG(sensor, 0x55, 0x00);
	ADV7180_WRITE_REG(sensor, 0x56, 0x10);
	ADV7180_WRITE_REG(sensor, 0x57, 0x00);
	ADV7180_WRITE_REG(sensor, 0x58, 0x00);
	ADV7180_WRITE_REG(sensor, 0x59, 0x00);
	ADV7180_WRITE_REG(sensor, 0x5A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x5B, 0x00);
	ADV7180_WRITE_REG(sensor, 0x5C, 0x00);
	ADV7180_WRITE_REG(sensor, 0x5D, 0x00);
	ADV7180_WRITE_REG(sensor, 0x5E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x5F, 0x00);
	ADV7180_WRITE_REG(sensor, 0x60, 0x00);
	ADV7180_WRITE_REG(sensor, 0x61, 0x00);
	ADV7180_WRITE_REG(sensor, 0x62, 0x20);
	ADV7180_WRITE_REG(sensor, 0x63, 0x00);
	ADV7180_WRITE_REG(sensor, 0x64, 0x00);
	ADV7180_WRITE_REG(sensor, 0x65, 0x00);
	ADV7180_WRITE_REG(sensor, 0x66, 0x00);
	ADV7180_WRITE_REG(sensor, 0x67, 0x03);
	ADV7180_WRITE_REG(sensor, 0x68, 0x01);
	ADV7180_WRITE_REG(sensor, 0x69, 0x00);
	ADV7180_WRITE_REG(sensor, 0x6A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x6B, 0xC0);
	ADV7180_WRITE_REG(sensor, 0x6C, 0x00);
	ADV7180_WRITE_REG(sensor, 0x6D, 0x00);
	ADV7180_WRITE_REG(sensor, 0x6E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x6F, 0x00);
	ADV7180_WRITE_REG(sensor, 0x70, 0x00);
	ADV7180_WRITE_REG(sensor, 0x71, 0x00);
	ADV7180_WRITE_REG(sensor, 0x72, 0x00);
	ADV7180_WRITE_REG(sensor, 0x73, 0x10);
	ADV7180_WRITE_REG(sensor, 0x74, 0x04);
	ADV7180_WRITE_REG(sensor, 0x75, 0x01);
	ADV7180_WRITE_REG(sensor, 0x76, 0x00);
	ADV7180_WRITE_REG(sensor, 0x77, 0x3F);
	ADV7180_WRITE_REG(sensor, 0x78, 0xFF);
	ADV7180_WRITE_REG(sensor, 0x79, 0xFF);
	ADV7180_WRITE_REG(sensor, 0x7A, 0xFF);
	ADV7180_WRITE_REG(sensor, 0x7B, 0x1E);
	ADV7180_WRITE_REG(sensor, 0x7C, 0xC0);
	ADV7180_WRITE_REG(sensor, 0x7D, 0x00);
	ADV7180_WRITE_REG(sensor, 0x7E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x7F, 0x00);
	ADV7180_WRITE_REG(sensor, 0x80, 0x00);
	ADV7180_WRITE_REG(sensor, 0x81, 0xC0);
	ADV7180_WRITE_REG(sensor, 0x82, 0x04);
	ADV7180_WRITE_REG(sensor, 0x83, 0x00);
	ADV7180_WRITE_REG(sensor, 0x84, 0x0C);
	ADV7180_WRITE_REG(sensor, 0x85, 0x02);
	ADV7180_WRITE_REG(sensor, 0x86, 0x03);
	ADV7180_WRITE_REG(sensor, 0x87, 0x63);
	ADV7180_WRITE_REG(sensor, 0x88, 0x5A);
	ADV7180_WRITE_REG(sensor, 0x89, 0x08);
	ADV7180_WRITE_REG(sensor, 0x8A, 0x10);
	ADV7180_WRITE_REG(sensor, 0x8B, 0x00);
	ADV7180_WRITE_REG(sensor, 0x8C, 0x40);
	ADV7180_WRITE_REG(sensor, 0x8D, 0x00);
	ADV7180_WRITE_REG(sensor, 0x8E, 0x40);
	ADV7180_WRITE_REG(sensor, 0x8F, 0x00);
	ADV7180_WRITE_REG(sensor, 0x90, 0x00);
	ADV7180_WRITE_REG(sensor, 0x91, 0x50);
	ADV7180_WRITE_REG(sensor, 0x92, 0x00);
	ADV7180_WRITE_REG(sensor, 0x93, 0x00);
	ADV7180_WRITE_REG(sensor, 0x94, 0x00);
	ADV7180_WRITE_REG(sensor, 0x95, 0x00);
	ADV7180_WRITE_REG(sensor, 0x96, 0x00);
	ADV7180_WRITE_REG(sensor, 0x97, 0xF0);
	ADV7180_WRITE_REG(sensor, 0x98, 0x00);
	ADV7180_WRITE_REG(sensor, 0x99, 0x00);
	ADV7180_WRITE_REG(sensor, 0x9A, 0x00);
	ADV7180_WRITE_REG(sensor, 0x9B, 0x00);
	ADV7180_WRITE_REG(sensor, 0x9C, 0x00);
	ADV7180_WRITE_REG(sensor, 0x9D, 0x00);
	ADV7180_WRITE_REG(sensor, 0x9E, 0x00);
	ADV7180_WRITE_REG(sensor, 0x9F, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA0, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA1, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA2, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA3, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA4, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA5, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA6, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA7, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA8, 0x00);
	ADV7180_WRITE_REG(sensor, 0xA9, 0x00);
	ADV7180_WRITE_REG(sensor, 0xAA, 0x00);
	ADV7180_WRITE_REG(sensor, 0xAB, 0x00);
	ADV7180_WRITE_REG(sensor, 0xAC, 0x00);
	ADV7180_WRITE_REG(sensor, 0xAD, 0x00);
	ADV7180_WRITE_REG(sensor, 0xAE, 0x60);
	ADV7180_WRITE_REG(sensor, 0xAF, 0x00);
	ADV7180_WRITE_REG(sensor, 0xB0, 0x00);
	ADV7180_WRITE_REG(sensor, 0xB1, 0x60);
	ADV7180_WRITE_REG(sensor, 0xB2, 0x1C);
	ADV7180_WRITE_REG(sensor, 0xB3, 0x54);
	ADV7180_WRITE_REG(sensor, 0xB4, 0x00);
	ADV7180_WRITE_REG(sensor, 0xB5, 0x00);
	ADV7180_WRITE_REG(sensor, 0xB6, 0x00);
	ADV7180_WRITE_REG(sensor, 0xB7, 0x13);
	ADV7180_WRITE_REG(sensor, 0xB8, 0x03);
	ADV7180_WRITE_REG(sensor, 0xB9, 0x33);
	ADV7180_WRITE_REG(sensor, 0xBF, 0x02);
	ADV7180_WRITE_REG(sensor, 0xC0, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC1, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC2, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC3, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC4, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC5, 0x81);
	ADV7180_WRITE_REG(sensor, 0xC6, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC7, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC8, 0x00);
	ADV7180_WRITE_REG(sensor, 0xC9, 0x04);
	ADV7180_WRITE_REG(sensor, 0xCC, 0x69);
	ADV7180_WRITE_REG(sensor, 0xCD, 0x00);
	ADV7180_WRITE_REG(sensor, 0xCE, 0x01);
	ADV7180_WRITE_REG(sensor, 0xCF, 0xB4);
	ADV7180_WRITE_REG(sensor, 0xD0, 0x00);
	ADV7180_WRITE_REG(sensor, 0xD1, 0x10);
	ADV7180_WRITE_REG(sensor, 0xD2, 0xFF);
	ADV7180_WRITE_REG(sensor, 0xD3, 0xFF);
	ADV7180_WRITE_REG(sensor, 0xD4, 0x7F);
	ADV7180_WRITE_REG(sensor, 0xD5, 0x7F);
	ADV7180_WRITE_REG(sensor, 0xD6, 0x3E);
	ADV7180_WRITE_REG(sensor, 0xD7, 0x08);
	ADV7180_WRITE_REG(sensor, 0xD8, 0x3C);
	ADV7180_WRITE_REG(sensor, 0xD9, 0x08);
	ADV7180_WRITE_REG(sensor, 0xDA, 0x3C);
	ADV7180_WRITE_REG(sensor, 0xDB, 0x9B);
	ADV7180_WRITE_REG(sensor, 0xDC, 0xAC);
	ADV7180_WRITE_REG(sensor, 0xDD, 0x4C);
	ADV7180_WRITE_REG(sensor, 0xDE, 0x00);
	ADV7180_WRITE_REG(sensor, 0xDF, 0x00);
	ADV7180_WRITE_REG(sensor, 0xE0, 0x14);
	ADV7180_WRITE_REG(sensor, 0xE1, 0x80);
	ADV7180_WRITE_REG(sensor, 0xE2, 0x80);
	ADV7180_WRITE_REG(sensor, 0xE3, 0x80);
	ADV7180_WRITE_REG(sensor, 0xE4, 0x80);
	ADV7180_WRITE_REG(sensor, 0xE5, 0x25);
	ADV7180_WRITE_REG(sensor, 0xE6, 0x44);
	ADV7180_WRITE_REG(sensor, 0xE7, 0x63);
	ADV7180_WRITE_REG(sensor, 0xE8, 0x65);
	ADV7180_WRITE_REG(sensor, 0xE9, 0x14);
	ADV7180_WRITE_REG(sensor, 0xEA, 0x63);
	ADV7180_WRITE_REG(sensor, 0xEB, 0x55);
	ADV7180_WRITE_REG(sensor, 0xEC, 0x55);
	ADV7180_WRITE_REG(sensor, 0xEE, 0x00);
	ADV7180_WRITE_REG(sensor, 0xEF, 0x4A);
	ADV7180_WRITE_REG(sensor, 0xF0, 0x44);
	ADV7180_WRITE_REG(sensor, 0xF1, 0x0C);
	ADV7180_WRITE_REG(sensor, 0xF2, 0x32);
	ADV7180_WRITE_REG(sensor, 0xF3, 0x00);
	ADV7180_WRITE_REG(sensor, 0xF4, 0x3F);
	ADV7180_WRITE_REG(sensor, 0xF5, 0xE0);
	ADV7180_WRITE_REG(sensor, 0xF6, 0x69);
	ADV7180_WRITE_REG(sensor, 0xF7, 0x10);
	ADV7180_WRITE_REG(sensor, 0xF8, 0x00);
	ADV7180_WRITE_REG(sensor, 0xF9, 0x03);
	ADV7180_WRITE_REG(sensor, 0xFA, 0xFA);
	ADV7180_WRITE_REG(sensor, 0xFB, 0x40);

	return 0;
}

/*
 * Enable the SD_UNLOCK and SD_AD_CHNG interrupts.
 */
static int adv7180_enable_interrupts(struct adv7180_dev *sensor)
{
	int ret;

	/* Switch to interrupt register map */
	ADV7180_WRITE_REG(sensor, 0x0E, 0x20);
	/* INTRQ active low, active until cleared */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CONFIG_1, 0xd1);
	/* unmask SD_UNLOCK and SD_LOCK */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_MASK_1,
			  ADV7180_INT_SD_UNLOCK | ADV7180_INT_SD_LOCK);
	/* unmask SD_AD_CHNG and SD_V_LOCK_CHNG */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_MASK_3,
			  ADV7180_INT_SD_AD_CHNG | ADV7180_INT_SD_V_LOCK_CHNG);
	/* Switch back to normal register map */
	ADV7180_WRITE_REG(sensor, 0x0E, 0x00);

	return 0;
}

/*!
 * ADV7180 I2C probe function.
 * Function set in i2c_driver struct.
 * Called by insmod.
 *
 *  @param *adapter	I2C adapter descriptor.
 *
 *  @return		Error code indicating success or failure.
 */
static int adv7180_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device_node *endpoint;
	struct adv7180_dev *sensor;
	struct device_node *np;
	const char *norm = "pal";
	bool std_change, lsc;
	u8 rev_id;
	int ret = 0;

	sensor = devm_kzalloc(&client->dev, sizeof(struct adv7180_dev),
			      GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &client->dev;
	np = sensor->dev->of_node;

	ret = of_property_read_string(np, "default-std", &norm);
	if (ret < 0 && ret != -EINVAL) {
		dev_err(sensor->dev, "error reading default-std property!\n");
		return ret;
	}
	if (!strcasecmp(norm, "pal")) {
		sensor->std_id = V4L2_STD_PAL;
		sensor->video_idx = ADV7180_PAL;
		dev_info(sensor->dev, "defaulting to PAL!\n");
	} else if (!strcasecmp(norm, "ntsc")) {
		sensor->std_id = V4L2_STD_NTSC;
		sensor->video_idx = ADV7180_NTSC;
		dev_info(sensor->dev, "defaulting to NTSC!\n");
	} else {
		dev_err(sensor->dev, "invalid default-std value: '%s'!\n",
			norm);
		return -EINVAL;
	}

	/* Set initial values for the sensor struct. */
	sensor->i2c_client = client;
	sensor->streamcap.timeperframe.denominator = 30;
	sensor->streamcap.timeperframe.numerator = 1;
	sensor->fmt.width = video_fmts[sensor->video_idx].raw.width;
	sensor->fmt.height = video_fmts[sensor->video_idx].raw.height;
	sensor->fmt.code = MEDIA_BUS_FMT_UYVY8_2X8;
	sensor->fmt.field = V4L2_FIELD_SEQ_BT;

	mutex_init(&sensor->mutex);

	endpoint = of_graph_get_next_endpoint(np, NULL);
	if (!endpoint) {
		dev_err(sensor->dev, "endpoint node not found\n");
		return -EINVAL;
	}

	v4l2_of_parse_endpoint(endpoint, &sensor->ep);
	if (sensor->ep.bus_type != V4L2_MBUS_BT656) {
		dev_err(sensor->dev, "invalid bus type, must be bt.656\n");
		return -EINVAL;
	}
	of_node_put(endpoint);

	ret = of_get_named_gpio(np, "pwdn-gpio", 0);
	if (gpio_is_valid(ret)) {
		sensor->pwdn_gpio = ret;
		ret = devm_gpio_request_one(sensor->dev,
					    sensor->pwdn_gpio,
					    GPIOF_OUT_INIT_HIGH,
					    "adv7180_pwdn");
		if (ret < 0) {
			dev_err(sensor->dev,
				"request for power down gpio failed\n");
			return ret;
		}
	} else {
		if (ret == -EPROBE_DEFER)
			return ret;
		/* assume a power-down gpio is not required */
		sensor->pwdn_gpio = -1;
	}

	adv7180_regulator_enable(sensor);

	/* Power on the chip */
	adv7180_power(sensor, true);

	/*! ADV7180 initialization. */
	ret = adv7180_hard_reset(sensor);
	if (ret) {
		dev_err(sensor->dev, "hard reset failed!\n");
		goto cleanup;
	}

	/*! Read the revision ID of the chip */
	ret = adv7180_read_reg(sensor, ADV7180_IDENT, &rev_id);
	if (ret < 0) {
		dev_err(sensor->dev,
			"failed to read ADV7180 IDENT register!\n");
		ret = -ENODEV;
		goto cleanup;
	}
	sensor->rev_id = rev_id;

	dev_info(sensor->dev, "Analog Devices ADV7180 Rev 0x%02X detected!\n",
		 sensor->rev_id);

	v4l2_i2c_subdev_init(&sensor->sd, client, &adv7180_subdev_ops);

	/* see if there is a signal lock already */
	ret = adv7180_update_lock_status(sensor, &lsc);
	if (ret < 0)
		goto cleanup;
	ret = adv7180_get_autodetect_std(sensor, &std_change);
	if (ret < 0)
		goto cleanup;

	if (sensor->i2c_client->irq) {
		ret = request_threaded_irq(sensor->i2c_client->irq,
					   NULL, adv7180_interrupt,
					   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					   IF_NAME, sensor);
		if (ret < 0) {
			dev_err(sensor->dev, "Failed to register irq %d\n",
				sensor->i2c_client->irq);
			goto cleanup;
		}

		adv7180_enable_interrupts(sensor);

		dev_info(sensor->dev, "Registered irq %d\n",
			 sensor->i2c_client->irq);
	}

	return adv7180_init_controls(sensor);

cleanup:
	adv7180_regulator_disable(sensor);
	return ret;
}

/*!
 * ADV7180 I2C detach function.
 * Called on rmmod.
 *
 *  @param *client	struct i2c_client*.
 *
 *  @return		Error code indicating success or failure.
 */
static int adv7180_detach(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	if (sensor->i2c_client->irq)
		free_irq(sensor->i2c_client->irq, sensor);

	v4l2_ctrl_handler_free(&sensor->ctrl_hdl);

	/* Power off the chip */
	adv7180_power(sensor, false);

	adv7180_regulator_disable(sensor);

	return 0;
}

static const struct i2c_device_id adv7180_id[] = {
	{ "adv7180", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, adv7180_id);

static struct of_device_id adv7180_dt_ids[] = {
	{ .compatible = "adi,adv7180" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, adv7180_dt_ids);

static struct i2c_driver adv7180_driver = {
	.driver = {
		.name	= "adv7180",
		.owner	= THIS_MODULE,
		.of_match_table	= adv7180_dt_ids,
	},
	.id_table	= adv7180_id,
	.probe		= adv7180_probe,
	.remove		= adv7180_detach,
};

module_i2c_driver(adv7180_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Analog Devices ADV7180 Subdev driver");
MODULE_LICENSE("GPL");
