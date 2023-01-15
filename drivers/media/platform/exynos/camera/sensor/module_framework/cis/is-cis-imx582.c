/*
 * Samsung Exynos5 SoC series Sensor driver
 *
 *
 * Copyright (c) 2018 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/videodev2.h>
#include <videodev2_exynos_camera.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/syscalls.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include <exynos-is-sensor.h>
#include "is-hw.h"
#include "is-core.h"
#include "is-param.h"
#include "is-device-sensor.h"
#include "is-device-sensor-peri.h"
#include "is-resourcemgr.h"
#include "is-dt.h"
#include "is-cis-imx582.h"
#include "is-cis-imx582-setA.h"
#include "is-cis-imx582-setB.h"

#include "is-helper-i2c.h"
#ifdef CONFIG_CAMERA_VENDER_MCD_V2
#include "is-sec-define.h"
#endif

#include "interface/is-interface-library.h"

#define SENSOR_NAME "IMX582"

static u8 coarse_integ_step_value;

static const u32 *sensor_imx582_global;
static u32 sensor_imx582_global_size;
static const u32 **sensor_imx582_setfiles;
static const u32 *sensor_imx582_setfile_sizes;
static u32 sensor_imx582_max_setfile_num;
static const struct sensor_pll_info_compact **sensor_imx582_pllinfos;

static bool sensor_imx582_cal_write_flag;

int sensor_imx582_cis_check_rev(struct is_cis *cis)
{
	int ret = 0;
	u8 status = 0;
	u8 rev = 0;
	struct i2c_client *client;

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* Specify OTP Page Address for READ - Page127(dec) */
	is_sensor_write8(client, SENSOR_IMX582_OTP_PAGE_SETUP_ADDR, 0x7F);

	/* Turn ON OTP Read MODE */
	is_sensor_write8(client, SENSOR_IMX582_OTP_READ_TRANSFER_MODE_ADDR,  0x01);

	/* Check status - 0x01 : read ready*/
	is_sensor_read8(client, SENSOR_IMX582_OTP_STATUS_REGISTER_ADDR,  &status);
	if ((status & 0x1) == false)
		err("status fail, (%d)", status);

	/* CHIP REV 0x0018 */
	ret = is_sensor_read8(client, SENSOR_IMX582_OTP_CHIP_REVISION_ADDR, &rev);
	if (ret < 0) {
		err("is_sensor_read8 fail (ret %d)", ret);
		I2C_MUTEX_UNLOCK(cis->i2c_lock);
		return ret;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	cis->cis_data->cis_rev = rev;

	probe_info("imx582 rev:%#x", rev);

	return 0;
}

static void sensor_imx582_set_integration_max_margin(u32 mode, cis_shared_data *cis_data)
{
	FIMC_BUG_VOID(!cis_data);

	cis_data->max_margin_coarse_integration_time = SENSOR_IMX582_COARSE_INTEGRATION_TIME_MAX_MARGIN;
	dbg_sensor(1, "max_margin_coarse_integration_time(%d)\n",
		cis_data->max_margin_coarse_integration_time);
}

static void sensor_imx582_set_integration_min(u32 mode, cis_shared_data *cis_data)
{
	FIMC_BUG_VOID(!cis_data);

	switch (mode) {
	case SENSOR_IMX582_REMOSAIC_FULL_8000x6000_15FPS:
	case SENSOR_IMX582_REMOSAIC_CROP_4000x3000_30FPS:
	case SENSOR_IMX582_REMOSAIC_CROP_4000x2256_30FPS:
		cis_data->min_coarse_integration_time = SENSOR_IMX582_COARSE_INTEGRATION_TIME_MIN;
		dbg_sensor(1, "min_coarse_integration_time(%d)\n",
			cis_data->min_coarse_integration_time);
		break;
	case SENSOR_IMX582_2X2BIN_FULL_4000X3000_30FPS:
	case SENSOR_IMX582_2X2BIN_CROP_4000X2256_30FPS:
	case SENSOR_IMX582_2X2BIN_CROP_4000X1800_30FPS:
		cis_data->min_coarse_integration_time = SENSOR_IMX582_COARSE_INTEGRATION_TIME_MIN_FOR_PDAF;
		dbg_sensor(1, "min_coarse_integration_time(%d)\n",
			cis_data->min_coarse_integration_time);
		break;
	case SENSOR_IMX582_2X2BIN_V2H2_2000X1500_120FPS:
	case SENSOR_IMX582_2X2BIN_V2H2_2000X1128_120FPS:
	case SENSOR_IMX582_2X2BIN_V2H2_2000X1128_240FPS:
		cis_data->min_coarse_integration_time = SENSOR_IMX582_COARSE_INTEGRATION_TIME_MIN_FOR_V2H2;
		dbg_sensor(1, "min_coarse_integration_time(%d)\n",
			cis_data->min_coarse_integration_time);
		break;
	default:
		err("[%s] Unsupport imx582 sensor mode\n", __func__);
		cis_data->min_coarse_integration_time = SENSOR_IMX582_COARSE_INTEGRATION_TIME_MIN;
		dbg_sensor(1, "min_coarse_integration_time(%d)\n",
			cis_data->min_coarse_integration_time);
		break;
	}
}

static void sensor_imx582_set_integration_step_value(u32 mode)
{
	switch (mode) {
	case SENSOR_IMX582_REMOSAIC_FULL_8000x6000_15FPS:
	case SENSOR_IMX582_2X2BIN_FULL_4000X3000_30FPS:
	case SENSOR_IMX582_2X2BIN_CROP_4000X2256_30FPS:
	case SENSOR_IMX582_2X2BIN_CROP_4000X1800_30FPS:
		coarse_integ_step_value = 1;
		dbg_sensor(1, "coarse_integration step value(%d)\n",
			coarse_integ_step_value);
		break;
	case SENSOR_IMX582_2X2BIN_V2H2_2000X1500_120FPS:
	case SENSOR_IMX582_2X2BIN_V2H2_2000X1128_120FPS:
	case SENSOR_IMX582_2X2BIN_V2H2_2000X1128_240FPS:
		coarse_integ_step_value = 2;
		dbg_sensor(1, "coarse_integration step value(%d)\n",
			coarse_integ_step_value);
		break;
	default:
		err("[%s] Unsupport imx582 sensor mode\n", __func__);
		coarse_integ_step_value = 1;
		dbg_sensor(1, "coarse_integration step value(%d)\n",
			coarse_integ_step_value);
		break;
	}
}

static void sensor_imx582_cis_data_calculation(const struct sensor_pll_info_compact *pll_info, cis_shared_data *cis_data)
{
	u64 pixel_rate;
	u32 frame_rate, max_fps, frame_valid_us;

	FIMC_BUG_VOID(!pll_info);

	/* 1. get pclk value from pll info */
	pixel_rate = pll_info->pclk * TOTAL_NUM_OF_IVTPX_CHANNEL;

	/* 2. FPS calculation */
	frame_rate = pixel_rate / (pll_info->frame_length_lines * pll_info->line_length_pck);

	/* 3. the time of processing one frame calculation (us) */
	cis_data->min_frame_us_time = (((u64)pll_info->frame_length_lines) *
					pll_info->line_length_pck * 1000
					/ (pixel_rate / 1000));

	cis_data->cur_frame_us_time = cis_data->min_frame_us_time;

#ifdef CAMERA_REAR2
	cis_data->min_sync_frame_us_time = cis_data->min_frame_us_time;
#endif

	dbg_sensor(1, "frame_duration(%d) - frame_rate (%d) = pixel_rate(%llu) / "
		KERN_CONT "(pll_info->frame_length_lines(%d) * pll_info->line_length_pck(%d))\n",
		cis_data->min_frame_us_time, frame_rate, pixel_rate, pll_info->frame_length_lines, pll_info->line_length_pck);

	/* calculate max fps */
	max_fps = (pixel_rate * 10) / (pll_info->frame_length_lines * pll_info->line_length_pck);
	max_fps = (max_fps % 10 >= 5 ? frame_rate + 1 : frame_rate);

	cis_data->pclk = pixel_rate;
	cis_data->max_fps = max_fps;
	cis_data->frame_length_lines = pll_info->frame_length_lines;
	cis_data->line_length_pck = pll_info->line_length_pck;
	cis_data->line_readOut_time = (u64)cis_data->line_length_pck * 1000
					* 1000 * 1000 / cis_data->pclk;
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;
	cis_data->stream_on = false;

	/* Frame valid time calcuration */
	frame_valid_us = (u64)cis_data->cur_height * cis_data->line_length_pck
				* 1000 * 1000 / cis_data->pclk;
	cis_data->frame_valid_us_time = (int)frame_valid_us;

	dbg_sensor(1, "%s\n", __func__);
	dbg_sensor(1, "Sensor size(%d x %d) setting: SUCCESS!\n", cis_data->cur_width, cis_data->cur_height);
	dbg_sensor(1, "Frame Valid(us): %d\n", frame_valid_us);
	dbg_sensor(1, "rolling_shutter_skew: %lld\n", cis_data->rolling_shutter_skew);

	dbg_sensor(1, "Fps: %d, max fps(%d)\n", frame_rate, cis_data->max_fps);
	dbg_sensor(1, "min_frame_time(%d us)\n", cis_data->min_frame_us_time);
	dbg_sensor(1, "Pixel rate(Mbps): %d\n", cis_data->pclk / 1000000);

	/* Frame period calculation */
	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;

	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n", __func__,
	cis_data->frame_time, cis_data->rolling_shutter_skew);

	/* Constant values */
	cis_data->min_fine_integration_time = SENSOR_IMX582_FINE_INTEGRATION_TIME;
	cis_data->max_fine_integration_time = SENSOR_IMX582_FINE_INTEGRATION_TIME;
	info("%s: done", __func__);
}

#if SENSOR_IMX582_CAL_DEBUG
int sensor_imx582_cis_cal_dump(char* name, char *buf, size_t size)
{
	int ret = 0;
#ifdef USE_KERNEL_VFS_READ_WRITE
	struct file *fp;

	fp = filp_open(name, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0666);
	if (IS_ERR_OR_NULL(fp)) {
		ret = PTR_ERR(fp);
		err("%s(): open file error(%s), error(%d)\n", __func__, name, ret);
		goto p_err;
	}

	ret = kernel_write(fp, buf, size, &fp->f_pos);
	if (ret < 0) {
		err("%s(): file write fail(%s) ret(%d)", __func__,
				name, ret);
		goto p_err;
	}

p_err:
	if (!IS_ERR_OR_NULL(fp))
		filp_close(fp, NULL);

#else
	err("not support %s due to kernel_write!", __func__);
	ret = -EINVAL;
#endif
	return ret;
}
#endif

int sensor_imx582_cis_LRC_write(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client = NULL;
	struct is_device_sensor_peri *sensor_peri = NULL;

	int position;
	u16 start_addr;
	u16 data_size;

	ulong cal_addr;
	u8 cal_data[SENSOR_IMX582_LRC_CAL_SIZE] = {0, };

#ifdef CONFIG_CAMERA_VENDER_MCD_V2
	char *rom_cal_buf = NULL;
#else
	struct is_lib_support *lib = is_get_lib_support();
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	sensor_peri = container_of(cis, struct is_device_sensor_peri, cis);
	FIMC_BUG(!sensor_peri);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		return -EINVAL;
	}

	position = sensor_peri->module->position;

#ifdef CONFIG_CAMERA_VENDER_MCD_V2
	is_sec_get_cal_buf(&rom_cal_buf, position);

	cal_addr = (ulong)rom_cal_buf;
	if (position == SENSOR_POSITION_REAR) {
		cal_addr += SENSOR_IMX582_LRC_CAL_BASE_REAR;
	} else {
		err("cis_imx582 position(%d) is invalid!\n", position);
		goto p_err;
	}
#else
	if (position == SENSOR_POSITION_REAR){
		cal_addr = lib->minfo->kvaddr_cal[position] + SENSOR_IMX582_LRC_CAL_BASE_REAR;
	}else {
		err("cis_imx582 position(%d) is invalid!\n", position);
		goto p_err;
	}
#endif

	memcpy(cal_data, (u16 *)cal_addr, SENSOR_IMX582_LRC_CAL_SIZE);

#if SENSOR_IMX582_CAL_DEBUG
	ret = sensor_imx582_cis_cal_dump(SENSOR_IMX582_LRC_DUMP_NAME, (char *)cal_data, (size_t)SENSOR_IMX582_LRC_CAL_SIZE);
	if (ret < 0) {
		err("cis_imx582 LRC Cal dump fail(%d)!\n", ret);
		goto p_err;
	}
#endif

	start_addr = SENSOR_IMX582_LRC_REG_ADDR;
	data_size = SENSOR_IMX582_LRC_CAL_SIZE;

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_write8_sequential(client, start_addr, cal_data, data_size);
	if (ret < 0) {
		err("cis_imx582 LRC write Error(%d)\n", ret);
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_QuadSensCal_write(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client = NULL;
	struct is_device_sensor_peri *sensor_peri = NULL;

	int position;
	u16 start_addr;
	u16 data_size;

	ulong cal_addr;
	u8 cal_data[SENSOR_IMX582_QUAD_SENS_CAL_SIZE] = {0, };

#ifdef CONFIG_CAMERA_VENDER_MCD_V2
	char *rom_cal_buf = NULL;
#else
	struct is_lib_support *lib = is_get_lib_support();
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	sensor_peri = container_of(cis, struct is_device_sensor_peri, cis);
	FIMC_BUG(!sensor_peri);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		return -EINVAL;
	}

	position = sensor_peri->module->position;

#ifdef CONFIG_CAMERA_VENDER_MCD_V2
	is_sec_get_cal_buf(&rom_cal_buf, position);

	cal_addr = (ulong)rom_cal_buf;
	if (position == SENSOR_POSITION_REAR) {
		cal_addr += SENSOR_IMX582_QUAD_SENS_CAL_BASE_REAR;
	} else {
		err("cis_imx582 position(%d) is invalid!\n", position);
		goto p_err;
	}
#else
	if (position == SENSOR_POSITION_REAR){
		cal_addr = lib->minfo->kvaddr_cal[position] + SENSOR_IMX582_QUAD_SENS_CAL_BASE_REAR;
	}else {
		err("cis_imx582 position(%d) is invalid!\n", position);
		goto p_err;
	}
#endif

	memcpy(cal_data, (u16 *)cal_addr, SENSOR_IMX582_QUAD_SENS_CAL_SIZE);

#if SENSOR_IMX582_CAL_DEBUG
	ret = sensor_imx582_cis_cal_dump(SENSOR_IMX582_QSC_DUMP_NAME, (char *)cal_data, (size_t)SENSOR_IMX582_QUAD_SENS_CAL_SIZE);
	if (ret < 0) {
		err("cis_imx582 QSC Cal dump fail(%d)!\n", ret);
		goto p_err;
	}
#endif

	start_addr = SENSOR_IMX582_QUAD_SENS_REG_ADDR;
	data_size = SENSOR_IMX582_QUAD_SENS_CAL_SIZE;

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_write8_sequential(client, start_addr, cal_data, data_size);
	if (ret < 0) {
		err("cis_imx582 QSC write Error(%d)\n", ret);
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

/*************************************************
 *  [IMX582 Analog gain formular]
 *
 *  m0: [0x008c:0x008d] fixed to 0
 *  m1: [0x0090:0x0091] fixed to -1
 *  c0: [0x008e:0x008f] fixed to 1024
 *  c1: [0x0092:0x0093] fixed to 1024
 *  X : [0x0204:0x0205] Analog gain setting value
 *
 *  Analog Gain = (m0 * X + c0) / (m1 * X + c1)
 *              = 1024 / (1024 - X)
 *
 *  Analog Gain Range = 112 to 1008 (1dB to 26dB)
 *
 *************************************************/

u32 sensor_imx582_cis_calc_again_code(u32 permille)
{
	return 1024 - (1024000 / permille);
}

u32 sensor_imx582_cis_calc_again_permile(u32 code)
{
	return 1024000 / (1024 - code);
}

u32 sensor_imx582_cis_calc_dgain_code(u32 permile)
{
	u8 buf[2] = {0, 0};
	buf[0] = (permile / 1000) & 0x0F;
	buf[1] = (((permile - (buf[0] * 1000)) * 256) / 1000);

	return (buf[0] << 8 | buf[1]);
}

u32 sensor_imx582_cis_calc_dgain_permile(u32 code)
{
	return (((code & 0x0F00) >> 8) * 1000) + ((code & 0x00FF) * 1000 / 256);
}

u32 sensor_imx582_cis_get_fineIntegTime(struct is_cis *cis)
{
	u32 ret = 0;
	u16 fine_integ_time = 0;
	struct i2c_client *client;

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* FINE_INTEG_TIME ADDR [0x0200:0x0201] */
	ret = is_sensor_read16(client, SENSOR_IMX582_FINE_INTEG_TIME_ADDR, &fine_integ_time);
	if (ret < 0) {
		err("is_sensor_read16 fail (ret %d)", ret);
		I2C_MUTEX_UNLOCK(cis->i2c_lock);
		return ret;
	}
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	info("%s: read fine_integ_time = %#x\n", __func__, fine_integ_time);

	return ret;
}

/* CIS OPS */
int sensor_imx582_cis_init(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	u32 setfile_index = 0;
	cis_setting_info setinfo;
#ifdef USE_CAMERA_HW_BIG_DATA
	struct cam_hw_param *hw_param = NULL;
	struct is_device_sensor_peri *sensor_peri = NULL;
#endif
	ktime_t st = ktime_get();

	setinfo.param = NULL;
	setinfo.return_value = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	FIMC_BUG(!cis->cis_data);
	memset(cis->cis_data, 0, sizeof(cis_shared_data));

	probe_info("%s imx582 init\n", __func__);
	cis->rev_flag = false;

/***********************************************************************
***** Check that QSC Cal is written for Remosaic Capture.
***** false : Not yet write the QSC
***** true  : Written the QSC Or Skip
***********************************************************************/
	sensor_imx582_cal_write_flag = false;

	ret = sensor_imx582_cis_check_rev(cis);
	if (ret < 0) {
#ifdef USE_CAMERA_HW_BIG_DATA
		sensor_peri = container_of(cis, struct is_device_sensor_peri, cis);
		if (sensor_peri)
			is_sec_get_hw_param(&hw_param, sensor_peri->module->position);
		if (hw_param)
			hw_param->i2c_sensor_err_cnt++;
#endif
		warn("sensor_imx582_check_rev is fail when cis init");
		cis->rev_flag = true;
		ret = 0;
	}

	cis->cis_data->cur_width = SENSOR_IMX582_MAX_WIDTH;
	cis->cis_data->cur_height = SENSOR_IMX582_MAX_HEIGHT;
	cis->cis_data->low_expo_start = 33000;
	cis->need_mode_change = false;
	cis->long_term_mode.sen_strm_off_on_step = 0;
	cis->long_term_mode.sen_strm_off_on_enable = false;

	sensor_imx582_cis_data_calculation(sensor_imx582_pllinfos[setfile_index], cis->cis_data);
	sensor_imx582_set_integration_max_margin(setfile_index, cis->cis_data);
	sensor_imx582_set_integration_min(setfile_index, cis->cis_data);
	sensor_imx582_set_integration_step_value(setfile_index);

#if SENSOR_IMX582_DEBUG_INFO
	ret = sensor_imx582_cis_get_fineIntegTime(cis);
	if (ret < 0) {
		err("sensor_imx582_cis_get_fineIntegTime fail!! (%d)", ret);
		goto p_err;
	}

	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min dgain : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max dgain : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
#endif

#if SENSOR_IMX582_WRITE_PDAF_CAL
	ret = sensor_imx582_cis_LRC_write(subdev);
	if (ret < 0) {
		err("sensor_imx582_LRC_Cal_write fail!! (%d)", ret);
		goto p_err;
	}
#endif

#if SENSOR_IMX582_WRITE_SENSOR_CAL
	if (sensor_imx582_cal_write_flag == false) {
		sensor_imx582_cal_write_flag = true;

		info("[%s] mode is QBC Remosaic Mode! Write QSC data.\n", __func__);

		ret = sensor_imx582_cis_QuadSensCal_write(subdev);
		if (ret < 0) {
			err("sensor_imx582_Quad_Sens_Cal_write fail!! (%d)", ret);
			goto p_err;
		}
	}
#endif

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err:
	return ret;
}

int sensor_imx582_cis_log_status(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client = NULL;
	u8 data8 = 0;
	u16 data16 = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -ENODEV;
		goto p_err;
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -ENODEV;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	pr_err("[SEN:DUMP] *******************************\n");
	is_sensor_read16(client, 0x0000, &data16);
	pr_err("[SEN:DUMP] model_id(%x)\n", data16);
	is_sensor_read8(client, 0x0002, &data8);
	pr_err("[SEN:DUMP] revision_number(%x)\n", data8);
	is_sensor_read8(client, 0x0005, &data8);
	pr_err("[SEN:DUMP] frame_count(%x)\n", data8);
	is_sensor_read8(client, 0x0100, &data8);
	pr_err("[SEN:DUMP] mode_select(%x)\n", data8);

	sensor_cis_dump_registers(subdev, sensor_imx582_setfiles[0], sensor_imx582_setfile_sizes[0]);
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	pr_err("[SEN:DUMP] *******************************\n");

p_err:
	return ret;
}

static int sensor_imx582_cis_group_param_hold_func(struct v4l2_subdev *subdev, unsigned int hold)
{
#if USE_GROUP_PARAM_HOLD
	int ret = 0;
	struct is_cis *cis = NULL;
	struct i2c_client *client = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (hold == cis->cis_data->group_param_hold) {
		pr_debug("already group_param_hold (%d)\n", cis->cis_data->group_param_hold);
		goto p_err;
	}

	ret = is_sensor_write8(client, 0x0104, hold);
	if (ret < 0)
		goto p_err;

	cis->cis_data->group_param_hold = hold;
	ret = 1;

p_err:
	return ret;
#else
	return 0;
#endif
}

/*
  hold control register for updating multiple-parameters within the same frame. 
  true : hold, flase : no hold/release
*/
#if USE_GROUP_PARAM_HOLD
int sensor_imx582_cis_group_param_hold(struct v4l2_subdev *subdev, bool hold)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	u32 mode;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	if (cis->cis_data->stream_on == false && hold == true) {
		ret = 0;
		dbg_sensor(1, "%s : sensor stream off skip group_param_hold", __func__);
		goto p_err;
	}

	mode = cis->cis_data->sens_config_index_cur;

	if (mode == SENSOR_IMX582_2X2BIN_V2H2_2000X1500_120FPS) {
		ret = 0;
		dbg_sensor(1, "%s : fast ae skip group_param_hold", __func__);
		goto p_err;
	}


	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = sensor_imx582_cis_group_param_hold_func(subdev, hold);
	if (ret < 0)
		goto p_err_unlock;

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}
#endif

int sensor_imx582_cis_set_global_setting(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	/* setfile global setting is at camera entrance */
	info("[%s] global setting start\n", __func__);
	ret = sensor_cis_set_registers(subdev, sensor_imx582_global, sensor_imx582_global_size);
	if (ret < 0) {
		err("sensor_imx582_set_registers fail!!");
		goto p_err;
	}

	dbg_sensor(1, "[%s] global setting done\n", __func__);

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	// Check that QSC and DPC Cal is written for Remosaic Capture.
	// false : Not yet write the QSC and DPC
	// true  : Written the QSC and DPC
	sensor_imx582_cal_write_flag = false;
	return ret;
}

int sensor_imx582_cis_mode_change(struct v4l2_subdev *subdev, u32 mode)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	if (mode > sensor_imx582_max_setfile_num) {
		err("invalid mode(%d)!!", mode);
		ret = -EINVAL;
		goto p_err;
	}

	/* If check_rev(Sensor ID in OTP) of IMX582 fail when cis_init, one more check_rev in mode_change */
	if (cis->rev_flag == true) {
		cis->rev_flag = false;
		ret = sensor_imx582_cis_check_rev(cis);
		if (ret < 0) {
			err("sensor_imx582_check_rev is fail");
			goto p_err;
		}
		info("[%s] cis_rev=%#x\n", __func__, cis->cis_data->cis_rev);
	}

	sensor_imx582_set_integration_max_margin(mode, cis->cis_data);
	sensor_imx582_set_integration_min(mode, cis->cis_data);
	sensor_imx582_set_integration_step_value(mode);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	info("[%s] mode=%d, mode change setting start\n", __func__, mode);
	ret = sensor_cis_set_registers(subdev, sensor_imx582_setfiles[mode], sensor_imx582_setfile_sizes[mode]);
	if (ret < 0) {
		err("sensor_imx582_set_registers fail!!");
		goto p_i2c_err;
	}
	dbg_sensor(1, "[%s] mode changed(%d)\n", __func__, mode);

p_i2c_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_set_size(struct v4l2_subdev *subdev, cis_shared_data *cis_data)
{
	int ret = 0;
	struct i2c_client *client = NULL;
	struct is_cis *cis = NULL;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	if (unlikely(!cis_data)) {
		err("cis data is NULL");
		if (unlikely(!cis->cis_data)) {
			ret = -EINVAL;
			goto p_err;
		} else {
			cis_data = cis->cis_data;
		}
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err:

	return ret;
}

int sensor_imx582_cis_stream_on(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;
	struct is_device_sensor_peri *sensor_peri = NULL;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	sensor_peri = container_of(cis, struct is_device_sensor_peri, cis);
	FIMC_BUG(!sensor_peri);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

#ifdef SENSOR_IMX582_DEBUG_INFO
	{
	u16 pll;
	is_sensor_read16(client, 0x0300, &pll);
	dbg_sensor(1, "______ vt_pix_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0302, &pll);
	dbg_sensor(1, "______ vt_sys_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0304, &pll);
	dbg_sensor(1, "______ pre_pll_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0306, &pll);
	dbg_sensor(1, "______ pll_multiplier(%x)\n", pll);
	is_sensor_read16(client, 0x030a, &pll);
	dbg_sensor(1, "______ op_sys_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x030c, &pll);
	dbg_sensor(1, "______ op_prepllck_div(%x)\n", pll);
	is_sensor_read16(client, 0x030e, &pll);
	dbg_sensor(1, "______ op_pll_multiplier(%x)\n", pll);
	is_sensor_read16(client, 0x0310, &pll);
	dbg_sensor(1, "______ pll_mult_driv(%x)\n", pll);
	is_sensor_read16(client, 0x0340, &pll);
	dbg_sensor(1, "______ frame_length_lines(%x)\n", pll);
	is_sensor_read16(client, 0x0342, &pll);
	dbg_sensor(1, "______ line_length_pck(%x)\n", pll);
	}
#endif

	I2C_MUTEX_LOCK(cis->i2c_lock);

	info("[%s] start\n", __func__);
	/* Here Add for Master mode in dual */
//	is_sensor_write8(client, 0x3040, 0x01);
//	is_sensor_write8(client, 0x3F71, 0x01);

	/* Sensor stream on */
	is_sensor_write8(client, 0x0100, 0x01);
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	cis_data->stream_on = true;

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err:
	return ret;
}

int sensor_imx582_cis_stream_off(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;
	ktime_t st = ktime_get();
	u8 frame_count = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	cis_data->stream_on = false; /* for not working group_param_hold after stream off */

	I2C_MUTEX_LOCK(cis->i2c_lock);
 
	ret = is_sensor_read8(client, 0x0005, &frame_count);
	info("%s: frame_count(%#x)\n", __func__, frame_count);

  	ret = sensor_imx582_cis_group_param_hold_func(subdev, 0x00);
	if (ret < 0)
		err("group_param_hold_func failed at stream off");

 	/* Sensor stream off */
	ret = is_sensor_write8(client, 0x0100, 0x00);
	if (ret < 0)
		err("stream off fail");

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err:
	return ret;
}

int sensor_imx582_cis_adjust_frame_duration(struct v4l2_subdev *subdev,
						u32 input_exposure_time,
						u32 *target_duration)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u64 pix_rate_freq_khz = 0;
	u32 line_length_pck = 0;
	u32 coarse_integ_time = 0;
	u32 frame_length_lines = 0;
	u32 frame_duration = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_duration);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (input_exposure_time == 0) {
		input_exposure_time  = cis_data->min_frame_us_time;
		info("[%s] Not proper exposure time(0), so apply min frame duration to exposure time forcely!!!(%d)\n",
			__func__, cis_data->min_frame_us_time);
	}

	pix_rate_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;
	coarse_integ_time = (u32)(((pix_rate_freq_khz * input_exposure_time)
							- cis_data->min_fine_integration_time) / (line_length_pck * 1000));
	
	if (cis->min_fps == cis->max_fps && cis->max_fps <= 60) {
		/*
		 * If input exposure is setted as 33333us@30fps from ddk
		 * then calculated frame duration is larger than 33333us because of CIT MARGIN.
		 */
		dbg_sensor(1, "[%s] requested min_fps(%d), max_fps(%d) from HAL\n", __func__, cis->min_fps, cis->max_fps);

		if (coarse_integ_time > cis_data->max_coarse_integration_time) {
			dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), input exp(%d) coarse(%u) max(%u)\n", cis->id, __func__,
				cis_data->sen_vsync_count, input_exposure_time, coarse_integ_time, cis_data->max_coarse_integration_time);
			coarse_integ_time = cis_data->max_coarse_integration_time;
		}

		if (coarse_integ_time < cis_data->min_coarse_integration_time) {
			dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), input exp(%d) coarse(%u) min(%u)\n", cis->id, __func__,
				cis_data->sen_vsync_count, input_exposure_time, coarse_integ_time, cis_data->min_coarse_integration_time);
			coarse_integ_time = cis_data->min_coarse_integration_time;
		}
	}

	frame_length_lines = coarse_integ_time + cis_data->max_margin_coarse_integration_time;
	frame_duration = (u32)((u64)(frame_length_lines * line_length_pck) / pix_rate_freq_khz) * 1000;

	dbg_sensor(1, "[%s](vsync cnt = %d) input exp(%d), adj duration, frame duration(%d), min_frame_us(%d)\n",
			__func__, cis_data->sen_vsync_count, input_exposure_time, frame_duration, cis_data->min_frame_us_time);

	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		*target_duration = MAX(frame_duration, cis_data->min_frame_us_time);
	} else {
		*target_duration = frame_duration;
	}

	dbg_sensor(1, "[%s] calcurated frame_duration(%d), adjusted frame_duration(%d)\n", __func__, frame_duration, *target_duration);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_set_frame_duration(struct v4l2_subdev *subdev, u32 frame_duration)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;
	struct is_long_term_expo_mode *lte_mode;

 	u32 pix_rate_freq_mhz = 0;
	u32 line_length_pck = 0;
	u16 frame_length_lines = 0;
	u64 max_fll_frame_time;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;
	lte_mode = &cis->long_term_mode;

	if (frame_duration < cis_data->min_frame_us_time) {
		dbg_sensor(1, "frame duration(%d) is less than min(%d)\n", frame_duration, cis_data->min_frame_us_time);
		frame_duration = cis_data->min_frame_us_time;
	}

	pix_rate_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;

	max_fll_frame_time = (65535 * (u64)line_length_pck) / (u64)pix_rate_freq_mhz;

	/*
	 * For Long Exposure Mode without stream on_off. (ex. Night HyperLaps)
	 * If frame duration over than 1sec, then it has to be applied CIT shift.
	 * In this case, frame_duration is setted in set_exposure_time with CIT shift.
	 */
	if (lte_mode->sen_strm_off_on_enable == false && cis_data ->min_frame_us_time > max_fll_frame_time) {
		cis_data->cur_frame_us_time = frame_duration;
		dbg_sensor(1, "[MOD:D:%d][%s] Skip set frame duration(%d) for CIT SHIFT.\n",
			cis->id, __func__, frame_duration);
		return ret;
	}

	frame_length_lines = (u16)((pix_rate_freq_mhz * frame_duration) / line_length_pck);

	dbg_sensor(1, "[MOD:D:%d] %s, pix_rate_freq_mhz(%#x) frame_duration = %d us,"
			KERN_CONT "(line_length_pck%#x), frame_length_lines(%#x)\n",
			cis->id, __func__, pix_rate_freq_mhz, frame_duration,
			line_length_pck, frame_length_lines);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	if (lte_mode->sen_strm_off_on_enable == false && cis_data->frame_length_lines_shifter > 0) {
		cis_data->frame_length_lines_shifter = 0;
		ret = is_sensor_write8(client, SENSOR_IMX582_CIT_LSHIFT_ADDR, 0);
		if (ret < 0) {
			goto p_err_unlock;
		}
	}

	ret = is_sensor_write16(client, SENSOR_IMX582_FRAME_LENGTH_LINE_ADDR, frame_length_lines);
	if (ret < 0) {
		goto p_err_unlock;
	}

	cis_data->cur_frame_us_time = frame_duration;
	cis_data->frame_length_lines = frame_length_lines;
	if (lte_mode->sen_strm_off_on_enable) {
		cis_data->max_coarse_integration_time =
			SENSOR_IMX582_MAX_COARSE_INTEG_WITH_FRM_LENGTH_CTRL - cis_data->max_margin_coarse_integration_time;
	} else {
		cis_data->max_coarse_integration_time = cis_data->frame_length_lines - cis_data->max_margin_coarse_integration_time;
	}

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_set_frame_rate(struct v4l2_subdev *subdev, u32 min_fps)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 frame_duration = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (min_fps > cis_data->max_fps) {
		err("[MOD:D:%d] %s, request FPS is too high(%d), set to max_fps(%d)\n",
			cis->id, __func__, min_fps, cis_data->max_fps);
		min_fps = cis_data->max_fps;
	}

	if (min_fps == 0) {
		err("[MOD:D:%d] %s, request FPS is 0, set to min FPS(1)\n", cis->id, __func__);
		min_fps = 1;
	}

	frame_duration = (1 * 1000 * 1000) / min_fps;

	dbg_sensor(1, "[MOD:D:%d] %s, set FPS(%d), frame duration(%d)\n",
			cis->id, __func__, min_fps, frame_duration);

	ret = sensor_imx582_cis_set_frame_duration(subdev, frame_duration);
	if (ret < 0) {
		err("[MOD:D:%d] %s, set frame duration is fail(%d)\n",
			cis->id, __func__, ret);
		goto p_err;
	}

#ifdef CAMERA_REAR2
	cis_data->min_frame_us_time = MAX(frame_duration, cis_data->min_sync_frame_us_time);
#else
	cis_data->min_frame_us_time = frame_duration;
#endif
	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err:
	return ret;
}

int sensor_imx582_cis_set_exposure_time(struct v4l2_subdev *subdev, struct ae_param *target_exposure)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;
	struct is_long_term_expo_mode *lte_mode;

	u32 pix_rate_freq_mhz = 0;
	u16 coarse_int = 0;
	u32 line_length_pck = 0;
	u32 min_fine_int = 0;
	ktime_t st = ktime_get();

	u32 target_exp = 0;
	u32 target_frame_duration = 0;
	u16 frame_length_lines = 0;
	u64 max_fll_frame_time;

	unsigned char cit_lshift_val = 0;
	int cit_lshift_count = 0;

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_exposure);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if ((target_exposure->long_val <= 0) || (target_exposure->short_val <= 0)) {
		err("[%s] invalid target exposure(%d, %d)\n", __func__,
				target_exposure->long_val, target_exposure->short_val);
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;
	lte_mode = &cis->long_term_mode;

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), target long(%d), short(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, target_exposure->long_val, target_exposure->short_val);

	target_exp = target_exposure->val;
	pix_rate_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;
	min_fine_int = cis_data->min_fine_integration_time;

	max_fll_frame_time = (65535 * (u64)line_length_pck) / (u64)pix_rate_freq_mhz;

	/*
	 * For Long Exposure Mode without stream on_off. (ex. Night Hyper Laps: min exp. is 1.5sec)
	 * If frame duration over than 1sec, then sequence is same as below
	 * 1. set CIT_LSHFIT
	 * 2. set COARSE_INTEGRATION_TIME
	 * 3. set FRM_LENGTH_LINES
	 */
	if ((cis_data->is_data.scene_mode == AA_SCENE_MODE_SUPER_NIGHT || cis_data->is_data.scene_mode == AA_SCENE_MODE_HYPERLAPSE)
		&& lte_mode->sen_strm_off_on_enable == false && cis_data ->cur_frame_us_time > max_fll_frame_time) {
		target_frame_duration = cis_data->cur_frame_us_time;
		dbg_sensor(1, "[MOD:D:%d] %s, input frame duration(%d) for CIT SHIFT \n",
			cis->id, __func__, target_frame_duration);

		if (target_frame_duration > 100000) {
			cit_lshift_val = (unsigned char)(target_frame_duration / 100000);
			while(cit_lshift_val > 1) {
				cit_lshift_val /= 2;
				target_frame_duration /= 2;
				target_exp /= 2;
				cit_lshift_count ++;
			}

			if (cit_lshift_count > SENSOR_IMX582_MAX_CIT_LSHIFT_VALUE)
				cit_lshift_count = SENSOR_IMX582_MAX_CIT_LSHIFT_VALUE;
		}

		frame_length_lines = (u16)((pix_rate_freq_mhz * target_frame_duration) / (line_length_pck));

		cis_data->frame_length_lines = frame_length_lines;
		cis_data->frame_length_lines_shifter = cit_lshift_count;
		cis_data->max_coarse_integration_time =
			frame_length_lines - cis_data->max_margin_coarse_integration_time;

		dbg_sensor(1, "[MOD:D:%d] %s, target_frame_duration(%d), frame_length_line(%d), cit_lshift_count(%d)\n",
			cis->id, __func__, target_frame_duration, frame_length_lines, cit_lshift_count);
	}

	coarse_int = ((target_exp * pix_rate_freq_mhz) - min_fine_int) / line_length_pck;

	if (coarse_int%coarse_integ_step_value)
		coarse_int -= 1;

	if (coarse_int > cis_data->max_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), coarse(%d) max(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, coarse_int, cis_data->max_coarse_integration_time);
		coarse_int = cis_data->max_coarse_integration_time;
	}

	if (coarse_int < cis_data->min_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), coarse(%d) min(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, coarse_int, cis_data->min_coarse_integration_time);
		coarse_int = cis_data->min_coarse_integration_time;
	}

	if (cis_data->min_coarse_integration_time == SENSOR_IMX582_COARSE_INTEGRATION_TIME_MIN
		&& coarse_int < 16
		&& coarse_int%2 == 0) {
		coarse_int -= 1;
	}

	cis_data->cur_exposure_coarse = coarse_int;
	cis_data->cur_long_exposure_coarse = coarse_int;
	cis_data->cur_short_exposure_coarse = coarse_int;

	I2C_MUTEX_LOCK(cis->i2c_lock);
	if (lte_mode->sen_strm_off_on_enable == false && cis_data->min_frame_us_time > max_fll_frame_time) {
		if (cit_lshift_count > 0) {
			ret = is_sensor_write8(client, SENSOR_IMX582_CIT_LSHIFT_ADDR, cit_lshift_count);
			if (ret < 0)
				goto p_err_unlock;
		}
	}

	ret = is_sensor_write16(client, SENSOR_IMX582_COARSE_INTEG_TIME_ADDR, coarse_int);
		if (ret < 0)
			goto p_err_unlock;

	if (lte_mode->sen_strm_off_on_enable == false && cis_data->min_frame_us_time > max_fll_frame_time) {
		ret = is_sensor_write16(client, SENSOR_IMX582_FRAME_LENGTH_LINE_ADDR, frame_length_lines);
		if (ret < 0)
			goto p_err_unlock;
	}

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d): pix_rate_freq_mhz (%d),"
		KERN_CONT "line_length_pck(%d), frame_length_lines(%#x)\n", cis->id, __func__,
		cis_data->sen_vsync_count, pix_rate_freq_mhz, line_length_pck, cis_data->frame_length_lines);
	dbg_sensor(1, "[MOD:D:%d] %s, pixelrate(%zu), llp(%d), fll(%d), min_fine_int(%d) cit(%d)\n", cis->id, __func__,
		pix_rate_freq_mhz, line_length_pck, cis_data->frame_length_lines, min_fine_int, coarse_int);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_get_min_exposure_time(struct v4l2_subdev *subdev, u32 *min_expo)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	cis_shared_data *cis_data = NULL;
	u32 min_integration_time = 0;
	u32 min_coarse = 0;
	u32 min_fine = 0;
	u32 pix_rate_freq_mhz = 0;
	u32 line_length_pck = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	pix_rate_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;
	min_coarse = cis_data->min_coarse_integration_time;
	min_fine = cis_data->min_fine_integration_time;

	min_integration_time = ((line_length_pck * min_coarse) + min_fine) / pix_rate_freq_mhz;
	*min_expo = min_integration_time;

	dbg_sensor(1, "[%s] min integration time %d\n", __func__, min_integration_time);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_get_max_exposure_time(struct v4l2_subdev *subdev, u32 *max_expo)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u32 max_integration_time = 0;
	u32 max_coarse_margin = 0;
	u32 max_coarse = 0;
	u32 max_fine = 0;
	u32 pix_rate_freq_mhz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	pix_rate_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;
	frame_length_lines = cis_data->frame_length_lines;
	max_coarse_margin = cis_data->max_margin_coarse_integration_time;
	max_coarse = frame_length_lines - max_coarse_margin;
	max_fine = cis_data->max_fine_integration_time;

	max_integration_time = ((line_length_pck * max_coarse) + max_fine) / pix_rate_freq_mhz;

	*max_expo = max_integration_time;

	/* To Do: is udating this value right hear? */
	cis_data->max_coarse_integration_time = max_coarse;

	dbg_sensor(1, "[%s] max integration time %d, max coarse integration %d\n",
			__func__, max_integration_time, cis_data->max_coarse_integration_time);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_adjust_analog_gain(struct v4l2_subdev *subdev, u32 input_again, u32 *target_permile)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 again_code = 0;
	u32 again_permile = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_permile);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	again_code = sensor_imx582_cis_calc_again_code(input_again);

	if (again_code > cis_data->max_analog_gain[0]) {
		again_code = cis_data->max_analog_gain[0];
	} else if (again_code < cis_data->min_analog_gain[0]) {
		again_code = cis_data->min_analog_gain[0];
	}

	again_permile = sensor_imx582_cis_calc_again_permile(again_code);

	dbg_sensor(1, "[%s] min again(%d), max(%d), input_again(%d), code(%d), permile(%d)\n", __func__,
			cis_data->max_analog_gain[0],
			cis_data->min_analog_gain[0],
			input_again,
			again_code,
			again_permile);

	*target_permile = again_permile;

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_set_analog_gain(struct v4l2_subdev *subdev, struct ae_param *again)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	analog_gain = (u16)sensor_imx582_cis_calc_again_code(again->val);

	if (analog_gain < cis->cis_data->min_analog_gain[0]) {
		analog_gain = cis->cis_data->min_analog_gain[0];
	}

	if (analog_gain > cis->cis_data->max_analog_gain[0]) {
		analog_gain = cis->cis_data->max_analog_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d), input_again = %d us, analog_gain(%#x)\n",
		cis->id, __func__, cis->cis_data->sen_vsync_count, again->val, analog_gain);

	// the address of analog_gain is [9:0] from 0x0204 to 0x0205
	analog_gain &= 0x03FF;

	// Analog gain
	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = is_sensor_write16(client, SENSOR_IMX582_ANALOG_GAIN_ADDR, analog_gain);
	if (ret < 0)
		goto p_err_unlock;

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_get_analog_gain(struct v4l2_subdev *subdev, u32 *again)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = is_sensor_read16(client, SENSOR_IMX582_ANALOG_GAIN_ADDR, &analog_gain);
	if (ret < 0)
		goto p_err_unlock;

	analog_gain &= 0x03FF;
	*again = sensor_imx582_cis_calc_again_permile(analog_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_again = %d us, analog_gain(%#x)\n",
			cis->id, __func__, *again, analog_gain);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_get_min_analog_gain(struct v4l2_subdev *subdev, u32 *min_again)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u16 min_again_code = SENSOR_IMX582_MIN_ANALOG_GAIN_SET_VALUE;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	cis_data->min_analog_gain[0] = min_again_code;
	cis_data->min_analog_gain[1] = sensor_imx582_cis_calc_again_permile(min_again_code);
	*min_again = cis_data->min_analog_gain[1];

	dbg_sensor(1, "[%s] min_again_code %d, main_again_permile %d\n", __func__,
		cis_data->min_analog_gain[0], cis_data->min_analog_gain[1]);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_get_max_analog_gain(struct v4l2_subdev *subdev, u32 *max_again)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u16 max_again_code = SENSOR_IMX582_MAX_ANALOG_GAIN_SET_VALUE;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (IS_REMOSAIC_MODE(cis))
		max_again_code = SENSOR_IMX582_FULL_PIXEL_MAX_ANALOG_GAIN_SET_VALUE;

	cis_data->max_analog_gain[0] = max_again_code;
	cis_data->max_analog_gain[1] = sensor_imx582_cis_calc_again_permile(max_again_code);
	*max_again = cis_data->max_analog_gain[1];

	dbg_sensor(1, "[%s] max_again_code %d, max_again_permile %d\n", __func__,
		cis_data->max_analog_gain[0], cis_data->max_analog_gain[1]);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_set_digital_gain(struct v4l2_subdev *subdev, struct ae_param *dgain)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u16 dgain_code = 0;
	u8 dgains[2] = {0};
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dgain_code = (u16)sensor_cis_calc_dgain_code(dgain->val);

	if (dgain_code < cis->cis_data->min_digital_gain[0]) {
		dgain_code = cis->cis_data->min_digital_gain[0];
	}
	if (dgain_code > cis->cis_data->max_digital_gain[0]) {
		dgain_code = cis->cis_data->max_digital_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d), input_dgain = %d, dgain_code(%#x)\n",
			cis->id, __func__, cis->cis_data->sen_vsync_count, dgain->val, dgain_code);

	dgains[0] = (u8)((dgain_code & 0x0F00) >> 8);
	dgains[1] = (u8)(dgain_code & 0xFF);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = is_sensor_write8_array(client, SENSOR_IMX582_DIG_GAIN_ADDR, dgains, 2);
	if (ret < 0) {
		goto p_err_unlock;
	}

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_get_digital_gain(struct v4l2_subdev *subdev, u32 *dgain)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 digital_gain = 0;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = is_sensor_read16(client, SENSOR_IMX582_DIG_GAIN_ADDR, &digital_gain);
	if (ret < 0)
		goto p_err_unlock;

	*dgain = sensor_cis_calc_dgain_permile(digital_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, dgain_permile = %d, dgain_code(%#x)\n",
			cis->id, __func__, *dgain, digital_gain);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_imx582_cis_get_min_digital_gain(struct v4l2_subdev *subdev, u32 *min_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u16 min_dgain_code = SENSOR_IMX582_MIN_DIGITAL_GAIN_SET_VALUE;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	cis_data->min_digital_gain[0] = min_dgain_code;
	cis_data->min_digital_gain[1] = sensor_imx582_cis_calc_dgain_permile(min_dgain_code);

	*min_dgain = cis_data->min_digital_gain[1];

	dbg_sensor(1, "[%s] min_dgain_code %d, min_dgain_permile %d\n", __func__,
		cis_data->min_digital_gain[0], cis_data->min_digital_gain[1]);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_get_max_digital_gain(struct v4l2_subdev *subdev, u32 *max_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u16 max_dgain_code = SENSOR_IMX582_MAX_DIGITAL_GAIN_SET_VALUE;
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	cis_data->max_digital_gain[0] = max_dgain_code;
	cis_data->max_digital_gain[1] = sensor_imx582_cis_calc_dgain_permile(max_dgain_code);

	*max_dgain = cis_data->max_digital_gain[1];

	dbg_sensor(1, "[%s] max_dgain_code %d, max_dgain_permile %d\n", __func__,
		cis_data->max_digital_gain[0], cis_data->max_digital_gain[1]);

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

	return ret;
}

int sensor_imx582_cis_long_term_exposure(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct is_long_term_expo_mode *lte_mode;
	unsigned char cit_lshift_val = 0;
	int cit_lshift_count = 0;
	u32 target_exp = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	lte_mode = &cis->long_term_mode;

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* LTE mode or normal mode set */
	if (lte_mode->sen_strm_off_on_enable) {
		target_exp = lte_mode->expo[0];
		if (target_exp >= 125000) {
			cit_lshift_val = (unsigned char)(target_exp / 125000);
			while (cit_lshift_val > 1) {
				cit_lshift_val /= 2;
				target_exp /= 2;
				cit_lshift_count++;
			}

			lte_mode->expo[0] = target_exp;

			if (cit_lshift_count > SENSOR_IMX582_MAX_CIT_LSHIFT_VALUE)
				cit_lshift_count = SENSOR_IMX582_MAX_CIT_LSHIFT_VALUE;

			ret = is_sensor_write8(cis->client, SENSOR_IMX582_CIT_LSHIFT_ADDR, cit_lshift_count);
		}
	} else {
		cit_lshift_count = 0;
		ret = is_sensor_write8(cis->client, SENSOR_IMX582_CIT_LSHIFT_ADDR, cit_lshift_count);
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	info("[%s] sen_strm_enable(%d), cit_lshift_count (%d), target_exp(%d)", __func__,
		lte_mode->sen_strm_off_on_enable, cit_lshift_count, lte_mode->expo[0]);

	if (ret < 0)
		pr_err("ERR[%s]: LTE register setting fail\n", __func__);
	return ret;
}

int sensor_imx582_cis_set_wb_gain(struct v4l2_subdev *subdev, struct wb_gains wb_gains)
{
	int ret = 0;
	int mode = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	u16 abs_gains[4] = {0, };	//[0]=gr, [1]=r, [2]=b, [3]=gb
	ktime_t st = ktime_get();

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	if (!cis->use_wb_gain)
		return ret;

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	mode = cis->cis_data->sens_config_index_cur;

	if (mode != SENSOR_IMX582_REMOSAIC_FULL_8000x6000_15FPS)
		return 0;

	dbg_sensor(1, "[SEN:%d]%s:DDK vlaue: wb_gain_gr(%d), wb_gain_r(%d), wb_gain_b(%d), wb_gain_gb(%d)\n",
		cis->id, __func__, wb_gains.gr, wb_gains.r, wb_gains.b, wb_gains.gb);

	abs_gains[0] = (u16)((wb_gains.gr / 4) & 0xFFFF);
	abs_gains[1] = (u16)((wb_gains.r / 4) & 0xFFFF);
	abs_gains[2] = (u16)((wb_gains.b / 4) & 0xFFFF);
	abs_gains[3] = (u16)((wb_gains.gb / 4) & 0xFFFF);

	dbg_sensor(1, "[SEN:%d]%s, abs_gain_gr(0x%4X), abs_gain_r(0x%4X), abs_gain_b(0x%4X), abs_gain_gb(0x%4X)\n",
		cis->id, __func__, abs_gains[0], abs_gains[1], abs_gains[2], abs_gains[3]);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = is_sensor_write16_array(client, SENSOR_IMX582_ABS_GAIN_GR_SET_ADDR, abs_gains, 4);
	if (ret < 0)
		goto p_err_unlock;

	if (IS_ENABLED(DEBUG_SENSOR_TIME))
		dbg_sensor(1, "[%s] time %ldus", __func__, PABLO_KTIME_US_DELTA_NOW(st));

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

void sensor_imx582_cis_data_calc(struct v4l2_subdev *subdev, u32 mode)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG_VOID(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG_VOID(!cis);
	FIMC_BUG_VOID(!cis->cis_data);

	if (mode > sensor_imx582_max_setfile_num) {
		err("invalid mode(%d)!!", mode);
		return;
	}

	/* If check_rev fail when cis_init, one more check_rev in mode_change */
	if (cis->rev_flag == true) {
		cis->rev_flag = false;
		ret = sensor_imx582_cis_check_rev(cis);
		if (ret < 0) {
			err("sensor_imx582_check_rev is fail: ret(%d)", ret);
			return;
		}
	}

	sensor_imx582_cis_data_calculation(sensor_imx582_pllinfos[mode], cis->cis_data);
}

static struct is_cis_ops cis_ops_imx582 = {
	.cis_init = sensor_imx582_cis_init,
	.cis_log_status = sensor_imx582_cis_log_status,
#if USE_GROUP_PARAM_HOLD
	.cis_group_param_hold = sensor_imx582_cis_group_param_hold,
#endif	
	.cis_set_global_setting = sensor_imx582_cis_set_global_setting,
	.cis_set_size = sensor_imx582_cis_set_size,
	.cis_mode_change = sensor_imx582_cis_mode_change,
	.cis_stream_on = sensor_imx582_cis_stream_on,
	.cis_stream_off = sensor_imx582_cis_stream_off,
	.cis_wait_streamon = sensor_cis_wait_streamon,
	.cis_wait_streamoff = sensor_cis_wait_streamoff,
	.cis_adjust_frame_duration = sensor_imx582_cis_adjust_frame_duration,
	.cis_set_frame_duration = sensor_imx582_cis_set_frame_duration,
	.cis_set_frame_rate = sensor_imx582_cis_set_frame_rate,
	.cis_set_exposure_time = sensor_imx582_cis_set_exposure_time,
	.cis_get_min_exposure_time = sensor_imx582_cis_get_min_exposure_time,
	.cis_get_max_exposure_time = sensor_imx582_cis_get_max_exposure_time,
	.cis_adjust_analog_gain = sensor_imx582_cis_adjust_analog_gain,
	.cis_set_analog_gain = sensor_imx582_cis_set_analog_gain,
	.cis_get_analog_gain = sensor_imx582_cis_get_analog_gain,
	.cis_get_min_analog_gain = sensor_imx582_cis_get_min_analog_gain,
	.cis_get_max_analog_gain = sensor_imx582_cis_get_max_analog_gain,
	.cis_set_digital_gain = sensor_imx582_cis_set_digital_gain,
	.cis_get_digital_gain = sensor_imx582_cis_get_digital_gain,
	.cis_get_min_digital_gain = sensor_imx582_cis_get_min_digital_gain,
	.cis_get_max_digital_gain = sensor_imx582_cis_get_max_digital_gain,
	.cis_compensate_gain_for_extremely_br = sensor_cis_compensate_gain_for_extremely_br,
	.cis_set_long_term_exposure = sensor_imx582_cis_long_term_exposure,
	.cis_set_wb_gains = sensor_imx582_cis_set_wb_gain,
	.cis_data_calculation = sensor_imx582_cis_data_calc,
};

int cis_imx582_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret;
	struct is_cis *cis;
	struct is_device_sensor_peri *sensor_peri;
	char const *setfile;
	struct device_node *dnode = client->dev.of_node;

	ret = sensor_cis_probe(client, id, &sensor_peri);
	if (ret) {
		probe_info("%s: sensor_cis_probe ret(%d)\n", __func__, ret);
		return ret;
	}

	cis = &sensor_peri->cis;
	cis->ctrl_delay = N_PLUS_TWO_FRAME;
	cis->cis_ops = &cis_ops_imx582;
	/* belows are depend on sensor cis. MUST check sensor spec */
	cis->bayer_order = OTF_INPUT_ORDER_BAYER_RG_GB;
	cis->use_wb_gain = true;

	ret = of_property_read_string(dnode, "setfile", &setfile);
	if (ret) {
		err("setfile index read fail(%d), take default setfile!!", ret);
		setfile = "default";
	}

	if (strcmp(setfile, "default") == 0 || strcmp(setfile, "setA") == 0) {
		probe_info("%s setfile_A\n", __func__);
		sensor_imx582_global = sensor_imx582_setfile_A_Global;
		sensor_imx582_global_size = ARRAY_SIZE(sensor_imx582_setfile_A_Global);
		sensor_imx582_setfiles = sensor_imx582_setfiles_A;
		sensor_imx582_setfile_sizes = sensor_imx582_setfile_A_sizes;
		sensor_imx582_pllinfos = sensor_imx582_pllinfos_A;
		sensor_imx582_max_setfile_num = ARRAY_SIZE(sensor_imx582_setfiles_A);
	} else if (strcmp(setfile, "setB") == 0) {
		probe_info("%s setfile_B\n", __func__);
		sensor_imx582_global = sensor_imx582_setfile_B_Global;
		sensor_imx582_global_size = ARRAY_SIZE(sensor_imx582_setfile_B_Global);
		sensor_imx582_setfiles = sensor_imx582_setfiles_B;
		sensor_imx582_setfile_sizes = sensor_imx582_setfile_B_sizes;
		sensor_imx582_pllinfos = sensor_imx582_pllinfos_B;
		sensor_imx582_max_setfile_num = ARRAY_SIZE(sensor_imx582_setfiles_B);
	} else {
		err("%s setfile index out of bound, take default (setfile_A)", __func__);
		sensor_imx582_global = sensor_imx582_setfile_A_Global;
		sensor_imx582_global_size = ARRAY_SIZE(sensor_imx582_setfile_A_Global);
		sensor_imx582_setfiles = sensor_imx582_setfiles_A;
		sensor_imx582_setfile_sizes = sensor_imx582_setfile_A_sizes;
		sensor_imx582_pllinfos = sensor_imx582_pllinfos_A;
		sensor_imx582_max_setfile_num = ARRAY_SIZE(sensor_imx582_setfiles_A);
	}

	probe_info("%s done\n", __func__);

	return ret;
}

static const struct of_device_id sensor_cis_imx582_match[] = {
	{
		.compatible = "samsung,exynos-is-cis-imx582",
	},
	{},
};
MODULE_DEVICE_TABLE(of, sensor_cis_imx582_match);

static const struct i2c_device_id sensor_cis_imx582_idt[] = {
	{ SENSOR_NAME, 0 },
	{},
};

static struct i2c_driver sensor_cis_imx582_driver = {
	.probe	= cis_imx582_probe,
	.driver = {
		.name	= SENSOR_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = sensor_cis_imx582_match,
		.suppress_bind_attrs = true,
	},
	.id_table = sensor_cis_imx582_idt
};

static int __init sensor_cis_imx582_init(void)
{
	int ret;

	ret = i2c_add_driver(&sensor_cis_imx582_driver);
	if (ret)
		err("failed to add %s driver: %d\n",
			sensor_cis_imx582_driver.driver.name, ret);

	return ret;
}
late_initcall_sync(sensor_cis_imx582_init);

MODULE_LICENSE("GPL");
