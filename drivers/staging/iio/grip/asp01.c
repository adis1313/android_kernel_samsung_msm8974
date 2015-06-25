/*
 *  AD semiconductor asp01 grip sensor driver
 *
 *  Copyright (C) 2012 Samsung Electronics Co.Ltd
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>
#include <linux/asp01.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>


#include "../iio.h"
#include "../sysfs.h"
#include "../events.h"

/* For Debugging */
#undef GRIP_DEBUG

#ifdef CONFIG_SENSORS
#include "../../../sensors/sensors_core.h"
#define VENDOR	"ADSEMICON"
#define CHIP_ID	"ASP01"
#endif
#define CALIBRATION_FILE_PATH	"/efs/grip_cal_data"
#define CAL_DATA_NUM	20
#define MFM_DATA_NUM	16
#define MFM_REF_NUM	(MFM_DATA_NUM / 2)
#define SLAVE_ADDR	0x48
#define SET_MFM_DONE	(0x1 << 4)

#define DIV	1000
#define BYTE_MSK	0xff
#define BYTE_SFT	8
#define RESET_MSK	0xfe
#define CAL_CHECK_VAL	40/* CR percent : 40 = 0.5% / 0.125%(step) */
#define GRIP_CLOSE	0
#define GRIP_FAR	1
/* return values */
#define ASP01_WRONG_RANGE	-1
#define ASP01_DEV_WORKING	-1

static u8 init_reg[SET_REG_NUM][2] = {
	{ REG_UNLOCK, 0x00},
	{ REG_RST_ERR, 0x22},
	{ REG_PROX_PER, 0x10},
	{ REG_PAR_PER, 0x10},
	{ REG_TOUCH_PER, 0x3c},
	{ REG_HI_CAL_PER, 0x10},
	{ REG_BSMFM_SET, 0x36},
	{ REG_ERR_MFM_CYC, 0x12},
	{ REG_TOUCH_MFM_CYC, 0x23},
	{ REG_HI_CAL_SPD, 0x23},
	{ REG_CAL_SPD, 0x04},
	{ REG_BFT_MOT, 0x10},
	{ REG_TOU_RF_EXT, 0x22},
	{ REG_SYS_FUNC, 0x30},
	{ REG_OFF_TIME, 0x64},
	{ REG_SENSE_TIME, 0xa0},
	{ REG_DUTY_TIME, 0x50},
	{ REG_HW_CON1, 0x78},
	{ REG_HW_CON2, 0x17},
	{ REG_HW_CON3, 0xe3},
	{ REG_HW_CON4, 0x30},
	{ REG_HW_CON5, 0x83},
	{ REG_HW_CON6, 0x33},
	{ REG_HW_CON7, 0x70},
	{ REG_HW_CON8, 0x20},
	{ REG_HW_CON9, 0x04},
	{ REG_HW_CON10, 0x12},
	{ REG_HW_CON11, 0x00},
};

struct asp01_data {
	struct i2c_client *client;
	struct device *dev;
	struct iio_dev *indio_dev;
	struct work_struct work; /* for grip sensor */
	struct delayed_work d_work;
	struct delayed_work d_work_initdev;
	struct mutex data_mutex;
	atomic_t enable;
	struct asp01_platform_data *pdata;
	struct wake_lock gr_wake_lock;
	u8 cal_data[CAL_DATA_NUM];
	u8 default_mfm[MFM_DATA_NUM];
	int cr_cosnt;
	int cs_cosnt;
	bool init_touch_needed;
	bool first_close_check;
	bool is_first_close;
	bool skip_data;
	bool shutdown;
};

static int asp01_grip_enable(struct asp01_data *data)
{
	int err;

	err = i2c_smbus_write_byte_data(data->client,
			control_reg[CMD_CLK_ON][REG],
			control_reg[CMD_CLK_ON][CMD]);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);

#ifdef GRIP_DEBUG
	pr_debug("%s: reg: 0x%x, data: 0x%x\n", __func__,
		control_reg[CMD_CLK_ON][REG],
		control_reg[CMD_CLK_ON][CMD]);
#endif

	return err;
}

static int asp01_grip_disable(struct asp01_data *data)
{
	int err, grip;
	int count = 10;

	err = i2c_smbus_write_byte_data(data->client,
			REG_SYS_FUNC, 0x14);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
	do {
		usleep_range(5000, 5100);
		grip = gpio_get_value(data->pdata->gpio);
#ifdef GRIP_DEBUG
		pr_info("%s: grip=%d, count=%d\n",
			__func__, grip, count);
#endif
	} while (!grip && count--);

	err = i2c_smbus_write_byte_data(data->client,
			control_reg[CMD_CLK_OFF][REG],
			control_reg[CMD_CLK_OFF][CMD]);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);

#ifdef GRIP_DEBUG
	pr_debug("%s: reg: %x, data: %x\n", __func__,
		control_reg[CMD_CLK_OFF][REG],
		control_reg[CMD_CLK_OFF][CMD]);
#endif

	return err;
}

static void asp01_restore_from_eeprom(struct asp01_data *data)
{
	int err;

	err = i2c_smbus_write_byte_data(data->client,
			REG_EEP_ST_CON, 0x06);
	if (err) {
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
		return;
	}
	usleep_range(6000, 6100);
}

static int asp01_reset(struct asp01_data *data)
{
	int err;
	u8 reg;

	reg = i2c_smbus_read_byte_data(data->client,
			control_reg[CMD_RESET][REG]);
	if (reg < 0) {
		pr_err("%s : i2c read fail, err=%d, %d line\n",
			__func__, reg, __LINE__);
		return reg;
	}
	err = i2c_smbus_write_byte_data(data->client,
			control_reg[CMD_RESET][REG],
			(RESET_MSK & reg)
			| control_reg[CMD_RESET][CMD]);
	if (err) {
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
		goto done;
	}
	pr_info("%s, reset_reg = 0x%x\n", __func__, RESET_MSK & reg);
	err = i2c_smbus_write_byte_data(data->client,
			control_reg[CMD_RESET][REG],
			RESET_MSK & reg);
	if (err) {
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
		goto done;
	}
done:
	return err;
}

static int asp01_init_touch_onoff(struct asp01_data *data, bool onoff)
{
	int err;

	pr_info("%s, onoff = %d\n", __func__, onoff);
	if (onoff) {
		err = i2c_smbus_write_byte_data(data->client,
			control_reg[CMD_INIT_TOUCH_ON][REG],
			control_reg[CMD_INIT_TOUCH_ON][CMD]);
		if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
		}
	} else {
		err = i2c_smbus_write_byte_data(data->client,
			control_reg[CMD_INIT_TOUCH_OFF][REG],
			control_reg[CMD_INIT_TOUCH_OFF][CMD]);
		if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
		}
	}
	return err;
}

static int asp01_init_code_set(struct asp01_data *data)
{
	int err, i;
	u8 reg;

	/* write Initial code */
	for (i = 0; i < SET_REG_NUM; i++) {
		err = i2c_smbus_write_byte_data(data->client,
			init_reg[i][REG], init_reg[i][CMD]);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			goto done;
		}
		pr_debug("%s: reg: %x, data: %x\n", __func__,
			init_reg[i][REG], init_reg[i][CMD]);
	}
	msleep(500);
	/* check if the device is working */
	reg = i2c_smbus_read_byte_data(data->client,
		0x10);
	pr_info("%s: reg 0x10 = 0x%x\n",
		__func__, reg);
	if (reg & 0x02) {
		pr_info("%s, asp01 is working. do not turn off "\
			"init touch mode!\n", __func__);
		return ASP01_DEV_WORKING;
	}

	/* disable initial touch mode */
	asp01_init_touch_onoff(data, false);
	msleep(200);
done:
	return err;
}

static int asp01_apply_hw_dep_set(struct asp01_data *data)
{
	u8 reg, reg_prom_en1, i;
	int err = 0;

	if (data->pdata == NULL) {
		err = -EINVAL;
		pr_err("%s: data->pdata is NULL\n", __func__);
		goto done;
	}

	if (!data->pdata->cr_divsr)
		data->cr_cosnt = DIV;
	else
		data->cr_cosnt =
			((data->pdata->cr_divnd * DIV) / data->pdata->cr_divsr);

	if (!data->pdata->cs_divsr)
		data->cs_cosnt = 0;
	else
		data->cs_cosnt =
			((data->pdata->cs_divnd * DIV) / data->pdata->cs_divsr);

	for (i = 0; i < MFM_DATA_NUM; i++) {
		reg = i2c_smbus_read_byte_data(data->client,
			REG_MFM_INIT_REF0 + i);
		if (reg < 0) {
			pr_err("%s : i2c read fail, err=%d, %d line\n",
				__func__, reg, __LINE__);
			err = reg;
			goto done;
		}
		data->default_mfm[i] = reg;

		pr_info("%s: default_mfm[%d] = 0x%x\n",
			__func__, i, data->default_mfm[i]);
	}

	reg_prom_en1 = i2c_smbus_read_byte_data(data->client,
		REG_PROM_EN1);
	if (reg_prom_en1 < 0) {
		pr_err("%s : i2c read fail, err=%d, %d line\n",
			__func__, reg_prom_en1, __LINE__);
		err = reg_prom_en1;
		goto done;
	}

	for (i = 0; i < SET_REG_NUM; i++) {
		if (i < SET_HW_CON1) {
			init_reg[i][CMD] = data->pdata->init_code[i];
			pr_info("%s, init_reg[%d] = 0x%x\n", __func__, i,
				init_reg[i][CMD]);
		} else {
			/* keep reset value if PROM_EN1 is 0xaa */
			if (reg_prom_en1 != 0xaa)
				init_reg[i][CMD] =
					data->pdata->init_code[i];
			else {
				reg = i2c_smbus_read_byte_data(data->client,
					init_reg[i][REG]);
				if (reg < 0) {
					pr_err("%s : i2c read fail, err=%d\n",
						__func__, atomic_read(&data->enable));
					err = reg;
					goto done;
				}
				init_reg[i][CMD] = reg;
#if defined(CONFIG_TARGET_TAB3_LTE8) || defined(CONFIG_TARGET_TAB3_3G8)
				if (i == SET_HW_CON3)
					init_reg[i][CMD] =
						data->pdata->init_code[i];
#endif
				pr_info("%s: skip HW_CONs, init_reg[%d] = 0x%x\n",
					__func__, i, init_reg[i][CMD]);
			}
		}
	}
done:
	return err;
}

static void asp01_load_caldata(struct asp01_data *data)
{
	struct file *cal_filp = NULL;
	mm_segment_t old_fs;
	int err, i;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(CALIBRATION_FILE_PATH, O_RDONLY,
			S_IRUGO | S_IWUSR | S_IWGRP);
	if (IS_ERR(cal_filp)) {
		err = PTR_ERR(cal_filp);
		if (err != -ENOENT)
			pr_err("%s: Can't open calibration file.\n", __func__);
		else {
			pr_info("%s: There is no calibration file.\n",
				__func__);
			/* calibration status init */
			for (i = 0; i < CAL_DATA_NUM; i++)
				data->cal_data[i] = 0;
		}
		set_fs(old_fs);
		return;
	}

	err = cal_filp->f_op->read(cal_filp,
		(char *)data->cal_data,
		CAL_DATA_NUM * sizeof(u8), &cal_filp->f_pos);
	if (err != CAL_DATA_NUM * sizeof(u8))
		pr_err("%s: Can't read the cal data from file\n", __func__);

	filp_close(cal_filp, current->files);
	set_fs(old_fs);
}

static void asp01_open_calibration(struct asp01_data *data)
{
	int i;
	int err;
	int count = CAL_DATA_NUM;
#ifdef GRIP_DEBUG
	u8 reg;
#endif

	asp01_load_caldata(data);

	for (i = 0; i < CAL_DATA_NUM; i++) {
		if (!data->cal_data[i])
			count -= 1;
	}

	if (count) {
		asp01_restore_from_eeprom(data);

		err = asp01_reset(data);
		if (err) {
			pr_err("%s: asp01_reset, err=%d\n",
				__func__, err);
			return;
		}
		msleep(500);

		/* If MFM is not calibrated, count will be zero. */
		err = asp01_init_code_set(data);
		if (err < ASP01_DEV_WORKING) {
			pr_err("%s: asp01_init_code_set, err=%d\n",
				__func__, err);
			return;
		}
	} else
		pr_info("%s: There is no cal data.\n", __func__);

#ifdef GRIP_DEBUG
	/* verifiying MFM value*/
	for (i = 0; i < MFM_DATA_NUM; i++) {
		reg = i2c_smbus_read_byte_data(data->client,
				 REG_MFM_INIT_REF0 + i);
		if (reg < 0) {
			pr_err("%s : i2c read fail, err=%d, %d line\n",
				__func__, reg, __LINE__);
			return;
		}
		pr_info("%s: verify reg(0x%x) = 0x%x\n", __func__,
			REG_MFM_INIT_REF0 + i, reg);
	}
#endif
	return;
}

static void asp01_delay_work_func(struct work_struct *work)
{
	struct asp01_data *data
		= container_of(work, struct asp01_data, d_work.work);

	if (data->init_touch_needed && !data->is_first_close) {
		data->init_touch_needed = false;
		data->first_close_check = false;
		asp01_init_touch_onoff(data, false);
		pr_info("%s: disable init touch mode\n", __func__);
	} else
		pr_info("%s: skip disable init touch mode\n", __func__);
}

static void asp01_initdev_work_func(struct work_struct *work)
{
	struct asp01_data *data
		= container_of(work, struct asp01_data, d_work_initdev.work);
	int err;

	pr_info("%s, is called\n", __func__);

	mutex_lock(&data->data_mutex);
	/* eeprom reset */
	err = i2c_smbus_write_byte_data(data->client,
		REG_UNLOCK, 0x5A);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
	err = i2c_smbus_write_byte_data(data->client,
		REG_EEP_ADDR1, 0xFF);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
	err = i2c_smbus_write_byte_data(data->client,
		REG_EEP_ST_CON, 0x42);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
	usleep_range(15000, 15100);
	err = i2c_smbus_write_byte_data(data->client,
		REG_EEP_ST_CON, 0x06);
	if (err)
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);

	if (asp01_apply_hw_dep_set(data) < 0)
		pr_err("%s, asp01_apply_hw_dep_set fail\n", __func__);

	asp01_init_code_set(data);

	asp01_grip_disable(data);

	mutex_unlock(&data->data_mutex);
	pr_info("%s, is done\n", __func__);
}

static void asp01_work_func(struct work_struct *work)
{
	int touch;
	struct asp01_data *data
		= container_of(work, struct asp01_data, work);

	disable_irq(data->pdata->irq);

	if (unlikely(!data->pdata)) {
		pr_err("%s: can't get pdata\n", __func__);
		goto done;
	}
	/* gpio_get_value returns 256, when the pin is high */
	touch = gpio_get_value(data->pdata->gpio);
	if (data->init_touch_needed) {
		if (!touch && data->first_close_check) {
			data->is_first_close = true;
			data->first_close_check = false;
			pr_info("%s, first close!!\n", __func__);
		}

		if (touch && data->is_first_close) {
			data->is_first_close = false;
			data->init_touch_needed = false;
			asp01_init_touch_onoff(data, false);
			pr_info("%s: init touch off\n", __func__);
		} else
			pr_info("%s: skip init touch off\n", __func__);
	} else {
		/* To avoid reset(chip internal) by CR ERR mode,
		 * enable init touch mode
		 * CR ERR mode off is the only difference from normal mode
		 */
		if (!touch)
			asp01_init_touch_onoff(data, true);
		if (touch)
			asp01_init_touch_onoff(data, false);
	}
	/* check skip data cmd for factory test */
	if (!data->skip_data) {
		pr_info("%s: touch INT : %d!!\n", __func__, touch);
	} else
		pr_info("%s: skip input report skip cmd: %d\n",
			__func__, data->skip_data);
done:
	enable_irq(data->pdata->irq);
}

static irqreturn_t asp01_irq_thread(int irq, void *dev)
{
	struct asp01_data *data = (struct asp01_data *)dev;
	int err;

	wake_lock_timeout(&data->gr_wake_lock, 3 * HZ);
	if (data->skip_data) {
		pr_info("%s: irq skip_data\n", __func__);
	} else {
		pr_info("%s: irq created\n", __func__);
		err = iio_push_event(iio_priv_to_dev(data),
			IIO_UNMOD_EVENT_CODE(IIO_GRIP,
				0,
				IIO_EV_TYPE_THRESH,
				IIO_EV_DIR_EITHER),
			iio_get_time_ns());
	}

	return IRQ_HANDLED;
}

static ssize_t asp01_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct asp01_data *data = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int enable = 0;
	int err, touch;
	static bool isFirst = true;

	if (data->shutdown)
		return -ENXIO;

	if (kstrtoint(buf, 10, &enable))
		return -EINVAL;
	pr_err("%s, enable = %d,  %d\n", __func__, enable, atomic_read(&data->enable));
	switch (this_attr->address) {
		case IIO_ATTR_ENABLE:
			mutex_lock(&data->data_mutex);
			if (enable && !atomic_read(&data->enable)) {
				if (isFirst) {
					data->init_touch_needed = true;
					isFirst = false;
				}

				err = asp01_grip_enable(data);
				if (err < 0) {
					mutex_unlock(&data->data_mutex);
					goto done;
				}
				/* turn on time is needed. */
				usleep_range(10000, 11000);

				if (data->init_touch_needed) {
					asp01_open_calibration(data);
					data->first_close_check = true;
					pr_info("%s, first_close_check is set\n", __func__);
				}

				touch = gpio_get_value(data->pdata->gpio);
				if (!data->skip_data) {
					pr_info("%s: touch INT : %d!!\n", __func__, touch);
					err = iio_push_event(iio_priv_to_dev(data),
								IIO_UNMOD_EVENT_CODE(IIO_GRIP,
									0,
									IIO_EV_TYPE_THRESH,
									IIO_EV_DIR_EITHER),
								iio_get_time_ns());
				} else
					pr_info("%s: skipped input report skip cmd: %d\n",
					__func__, data->skip_data);

				enable_irq(data->pdata->irq);
				if (data->init_touch_needed)
					schedule_delayed_work(&data->d_work,
						msecs_to_jiffies(700));
			} else if (!enable && atomic_read(&data->enable)) {
				if (data->is_first_close == true)
					data->init_touch_needed = true;
				cancel_delayed_work_sync(&data->d_work);
				disable_irq(data->pdata->irq);
				err = asp01_grip_disable(data);
				if (err < 0) {
					mutex_unlock(&data->data_mutex);
					goto done;
				}
			} else
				pr_err("%s: Invalid enable status\n", __func__);

			atomic_set(&data->enable, enable);
			mutex_unlock(&data->data_mutex);
			pr_err("%s, enable = %d,  %d\n", __func__, enable, atomic_read(&data->enable));

		default:
			return -EINVAL;
	}

done:
	return count;
}

static ssize_t asp01_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct asp01_data *data = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);

	switch (this_attr->address) {
		case IIO_ATTR_ENABLE:
			pr_info("%s, sensor enable : %d\n",
				__func__, atomic_read(&data->enable));
			return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->enable));

		default:
			return -EINVAL;
	}
	return 0;
}

static IIO_DEVICE_ATTR(enable, S_IRUGO | S_IWUSR,
	asp01_attr_show, asp01_attr_store, IIO_ATTR_ENABLE);

static struct attribute *asp01_attributes[] = {
	&iio_dev_attr_enable.dev_attr.attr,
	NULL,
};

static struct attribute_group asp01_attribute_group = {
	.name = "grip",
	.attrs = asp01_attributes,
};

static int asp01_raw_data(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long m)
{
	struct asp01_data *data = iio_priv(indio_dev);
	int touch;
	pr_err("%s, \n", __func__);

	/* gpio_get_value returns 256, when the pin is high */
	touch = gpio_get_value(data->pdata->gpio);
	if (data->init_touch_needed) {
		if (!touch && data->first_close_check) {
			data->is_first_close = true;
			data->first_close_check = false;
			pr_info("%s, first close!!\n", __func__);
		}

		if (touch && data->is_first_close) {
			data->is_first_close = false;
			data->init_touch_needed = false;
			asp01_init_touch_onoff(data, false);
			pr_info("%s: init touch off\n", __func__);
		} else
			pr_info("%s: skip init touch off\n", __func__);
	} else {
		/* To avoid reset(chip internal) by CR ERR mode,
		 * enable init touch mode
		 * CR ERR mode off is the only difference from normal mode
		 */
		if (!touch)
			asp01_init_touch_onoff(data, true);
		if (touch)
			asp01_init_touch_onoff(data, false);
	}
	/* check skip data cmd for factory test */
	if (!data->skip_data) {
		pr_info("%s: touch INT : %d!!\n", __func__, touch);
		*val = (touch ? GRIP_FAR : GRIP_CLOSE);
	} else {
		pr_info("%s: skip input report skip cmd: %d\n",
			__func__, data->skip_data);
		*val = GRIP_FAR;
	}

	return IIO_VAL_INT;
}


static struct iio_info asp01_info = {
	.driver_module = THIS_MODULE,
	.attrs = &asp01_attribute_group,
	.read_raw = asp01_raw_data,
};

static const struct iio_chan_spec asp01_channels[] = {
	{
		.type = IIO_GRIP,
		.indexed = 1,
		.channel = 0,
		.processed_val = IIO_PROCESSED,
		.event_mask = (IIO_EV_BIT(IIO_EV_TYPE_THRESH,
				IIO_EV_DIR_EITHER)),
	}
};

#ifdef CONFIG_SENSORS
static int asp01_save_caldata(struct asp01_data *data)
{
	struct file *cal_filp = NULL;
	int err = 0;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(CALIBRATION_FILE_PATH,
			O_CREAT | O_TRUNC | O_WRONLY | O_SYNC,
			S_IRUGO | S_IWUSR | S_IWGRP);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		return err;
	}

	err = cal_filp->f_op->write(cal_filp,
		(char *)&data->cal_data,
		CAL_DATA_NUM * sizeof(u8), &cal_filp->f_pos);
	if (err != CAL_DATA_NUM * sizeof(u8)) {
		pr_err("%s: Can't write the cal data to file\n", __func__);
		err = -EIO;
	}

	filp_close(cal_filp, current->files);
	set_fs(old_fs);

	return err;
}

static ssize_t asp01_calibration_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct asp01_data *data = dev_get_drvdata(dev);
	int success = ASP01_WRONG_RANGE, i, count = MFM_REF_NUM;
	u16 cs_per_2nd, cr_per_2nd;
	u16 mfm_ref[MFM_REF_NUM];

	asp01_load_caldata(data);

	for (i = 0; i < MFM_REF_NUM; i++) {
		mfm_ref[i] =
			(data->cal_data[2 * i + 1] << BYTE_SFT) |
				data->cal_data[2 * i];
		if (!mfm_ref[i])
			count -= 1;
	}

	/* If MFM is not calibrated, count will be zero. */
	if (count)
		success = 1;

	cs_per_2nd = data->cal_data[MFM_DATA_NUM + 1] << 8 |\
		data->cal_data[MFM_DATA_NUM];
	cr_per_2nd = data->cal_data[MFM_DATA_NUM + 3] << 8 |\
		data->cal_data[MFM_DATA_NUM + 2];
	pr_info("%s, count = %d, cs : %d, cr : %d\n", __func__,
		count, cs_per_2nd, cr_per_2nd);

	return sprintf(buf, "%d, %d, %d\n", success, cs_per_2nd, cr_per_2nd);
}

static void asp01_write_to_eeprom(struct asp01_data *data)
{
	int err, count;
	u8 reg;

	/* write into eeprom */
	err = i2c_smbus_write_byte_data(data->client,
			REG_EEP_ST_CON, 0x02);
	if (err) {
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
		return;
	}
	count = 20;
	do {
		reg = i2c_smbus_read_byte_data(data->client, REG_EEP_ST_CON);
		if (reg < 0) {
			pr_err("%s : i2c read fail, err=%d, %d line\n",
				__func__, reg, __LINE__);
			return;
		}
		pr_debug("%s: check eeprom reg(0x%x) = 0x%x count=%d\n",
			__func__, REG_EEP_ST_CON, reg, count);
		msleep(50);
	} while ((reg & 0x1) && count--);
	if (!count)
		pr_err("%s: use all count! need to check eeprom\n",
			__func__);
}

static int asp01_do_calibrate(struct asp01_data *data, bool do_calib)
{
	int err, i;
	int count = 100;
	u8 reg;
	u32 cr_ref[4] = {0,};
	u32 cs_ref[4] = {0,};
	u8 tmp_data[2] = {0,};
	u16 cs_per[4], cr_per[4];

	pr_info("%s: do_calib=%d\n", __func__, do_calib);
	if (do_calib) {
		/* 1. To block wrong data write 0x03 to 0x03. */
		err = i2c_smbus_write_byte_data(data->client,
				REG_SETMFM, 0x03);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			return err;
		}
		usleep_range(20000, 21000);
		for (i = 0; i < 4; i++) {
			/* 2. Select MFM unmber (MFM0~MFM3)*/
			err = i2c_smbus_write_byte_data(data->client,
					REG_SETMFM, i);
			if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return err;
			}
			/* 3. wait until data ready */
			do {
				reg = i2c_smbus_read_byte_data(data->client,
					REG_SETMFM);
				if (reg < 0) {
					pr_err("%s : i2c read fail, err=%d, %d line\n",
						__func__, reg, __LINE__);
					return reg;
				}
				usleep_range(1000, 1100);
				if (reg & SET_MFM_DONE) {
					pr_info("%s: count is %d\n", __func__,
						100 - count);
					count = 100;
					break;
				}
				if (!count)
					pr_err("%s: full count state\n",
						__func__);
			} while (count--);

			usleep_range(20000, 21000);
			/* 4. read each CR CS ref value */
			err = i2c_smbus_write_byte_data(data->client,
					control_reg[CMD_CLK_OFF][REG],
					control_reg[CMD_CLK_OFF][CMD]);
			if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return err;
			}
			err = i2c_smbus_read_i2c_block_data(data->client,
				REG_CR_REF0, sizeof(u8)*2,
				&data->cal_data[i * 4]);
			if (err != sizeof(u8)*2) {
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return -EIO;
			}
			err = i2c_smbus_read_i2c_block_data(data->client,
				REG_CS_REF0, sizeof(u8)*2,
				&data->cal_data[(i * 4) + 2]);
			if (err != sizeof(u8)*2) {
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return -EIO;
			}
			cr_ref[i] =
				(data->cal_data[(i * 4) + 1] << BYTE_SFT
					| data->cal_data[(i * 4)]);
			cs_ref[i] =
				(data->cal_data[(i * 4) + 3] << BYTE_SFT
					| data->cal_data[(i * 4) + 2]);

			/* 5. multiply cr, cs constant */
			/* cs ref x cs cont */
			cs_ref[i] =
				(cs_ref[i] * data->cs_cosnt) / DIV;
			pr_info("%s, cs_ref[%d] = %d\n", __func__, i,
				cs_ref[i]);
			/* cr ref x cr cont */
			cr_ref[i] =
				(cr_ref[i] * data->cr_cosnt) / DIV;
			pr_info("%s, cr_ref[%d] = %d\n", __func__, i,
				cr_ref[i]);

			/* 6. separate high low reg of cr cs ref */
			data->cal_data[(i * 4)] =
				(u8)(BYTE_MSK & cr_ref[i]);

			data->cal_data[(i * 4) + 1] =
				(u8)(BYTE_MSK & (cr_ref[i] >> BYTE_SFT));

			data->cal_data[(i * 4) + 2] =
				(u8)(BYTE_MSK & cs_ref[i]);

			data->cal_data[(i * 4) + 3] =
				(u8)(BYTE_MSK & (cs_ref[i] >> BYTE_SFT));

			/* 7. read CS, CR percent value */
			err = i2c_smbus_read_i2c_block_data(data->client,
				REG_CS_PERL, sizeof(u8)*2,
				tmp_data);
			if (err != sizeof(u8)*2) {
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return -EIO;
			}

			cs_per[i] = (tmp_data[1] << 8) | tmp_data[0];
			pr_info("%s, cs : %d\n", __func__, cs_per[i]);

			err = i2c_smbus_read_i2c_block_data(data->client,
				REG_CR_PER_L, sizeof(u8)*2,
				tmp_data);
			if (err != sizeof(u8)*2) {
				pr_err("%s :i2c read fail. err=%d, %d line\n",
					__func__, err, __LINE__);
				return -EIO;
			}
			cr_per[i] = (tmp_data[1] << 8) | tmp_data[0];
			pr_info("%s, cr : %d\n", __func__, cr_per[i]);

			err = i2c_smbus_write_byte_data(data->client,
				control_reg[CMD_CLK_ON][REG],
				control_reg[CMD_CLK_ON][CMD]);
			if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return err;
			}
		}

		/* save avg cs, cr percent */
		data->cal_data[MFM_DATA_NUM] =
			(u8)(cs_per[1] & BYTE_MSK);
		data->cal_data[MFM_DATA_NUM + 1] =
			(u8)(cs_per[1] >> BYTE_SFT & BYTE_MSK);
		data->cal_data[MFM_DATA_NUM + 2] =
			(u8)(cr_per[1] & BYTE_MSK);
		data->cal_data[MFM_DATA_NUM + 3] =
			(u8)(cr_per[1] >> BYTE_SFT & BYTE_MSK);

		/* 9. write MFM ref value */
		for (i = 0; i < MFM_DATA_NUM; i++) {
			err = i2c_smbus_write_byte_data(data->client,
					REG_MFM_INIT_REF0 + i,
					data->cal_data[i]);
			if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return err;
			}
			pr_debug("%s: wr reg(0x%x) = 0x%x\n", __func__,
				REG_MFM_INIT_REF0 + i,
				data->cal_data[i]);
		}
		asp01_init_touch_onoff(data, true);
		asp01_write_to_eeprom(data);
		asp01_init_touch_onoff(data, false);
		pr_info("%s, init_touch_needed is set\n", __func__);
	} else {
		/* reset MFM ref value */
		memset(data->cal_data, 0, ARRAY_SIZE(data->cal_data));
		for (i = 0; i < MFM_DATA_NUM; i++) {
			err = i2c_smbus_write_byte_data(data->client,
					REG_MFM_INIT_REF0 + i, 0);
			if (err) {
				pr_err("%s: i2c write fail, err=%d, %d line\n",
					__func__, err, __LINE__);
				return err;
			}
			pr_debug("%s: reset reg(0x%x) = 0x%x\n", __func__,
				REG_MFM_INIT_REF0 + i, data->cal_data[i]);
		}
		data->init_touch_needed = false;
		asp01_write_to_eeprom(data);
	}

	return err;
}

static ssize_t asp01_calibration_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int err;
	bool do_calib;
	struct asp01_data *data = dev_get_drvdata(dev);
	u8 tmp_reg;

	if (sysfs_streq(buf, "1"))
		do_calib = true;
	else if (sysfs_streq(buf, "0"))
		do_calib = false;
	else {
		pr_info("%s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	pr_info("%s, do_calib = %d\n", __func__, do_calib);
#if defined(CONFIG_TARGET_TAB3_LTE8) || defined(CONFIG_TARGET_TAB3_3G8)
	if (do_calib) {
		pr_info("%s, reset before calibration start\n", __func__);
		asp01_restore_from_eeprom(data);

		err = asp01_reset(data);
		if (err) {
			pr_err("%s: asp01_reset, err = %d\n",
				__func__, err);
		}
		msleep(500);

		err = asp01_init_code_set(data);
		if (err < ASP01_DEV_WORKING)
			pr_err("%s: asp01_init_code_set, err = %d\n",
				__func__, err);
		pr_info("%s, reset before calibration done\n", __func__);
	}
#endif
	mutex_lock(&data->data_mutex);

	asp01_grip_enable(data);
	msleep(1000);

	/* check if the device is working */
	tmp_reg = i2c_smbus_read_byte_data(data->client,
		0x10);
	pr_info("%s: reg 0x10 = 0x%x\n",
		__func__, tmp_reg);
	if (tmp_reg & 0x02) {
		if (do_calib)
			pr_info("%s, asp01 is working. calibration fail\n",
				__func__);
		else
			pr_info("%s, asp01 is working. cal erase fail\n",
				__func__);
		err = ASP01_DEV_WORKING;
		goto exit;
	}

	asp01_restore_from_eeprom(data);

	err = asp01_reset(data);
	if (err < 0) {
		pr_err("%s, asp01_reset fail(%d)\n", __func__, err);
		goto exit;
	}
	msleep(500);

	err = asp01_init_code_set(data);
	if (err < ASP01_DEV_WORKING) {
		pr_err("%s, asp01_init_code_set fail(%d)\n", __func__, err);
		goto exit;
	}

	err = asp01_do_calibrate(data, do_calib);
	if (err < 0) {
		pr_err("%s, asp01_do_calibrate fail(%d)\n", __func__, err);
		goto exit;
	}

	err = asp01_save_caldata(data);
	if (err < 0) {
		pr_err("%s, asp01_save_caldata fail(%d)\n", __func__, err);
		goto exit;
	}
exit:
	asp01_grip_disable(data);
	mutex_unlock(&data->data_mutex);

	if (err >= 0)
		pr_info("%s, do_calib = %d done\n", __func__, do_calib);

	return count;
}
static DEVICE_ATTR(calibration, S_IRUGO | S_IWUSR | S_IWGRP,
	asp01_calibration_show, asp01_calibration_store);

static ssize_t asp01_grip_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", VENDOR);
}
static DEVICE_ATTR(vendor, S_IRUGO,
	asp01_grip_vendor_show, NULL);

static ssize_t asp01_grip_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", CHIP_ID);
}
static DEVICE_ATTR(name, S_IRUGO,
	asp01_grip_name_show, NULL);

static ssize_t asp01_grip_raw_data_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	u8 reg;
	u8 cs_per[2] = {0,};
	u8 cr_per[2] = {0,};
	u16 mlt_cs_per[4] = {0,};
	u16 mlt_cr_per[4] = {0,};
	u16 avg_cs_per, avg_cr_per;
	u32 sum_cr_per = 0;
	u32 sum_cs_per = 0;
	int count = 100;
	int err, i;
	struct asp01_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->data_mutex);
	if (!atomic_read(&data->enable)) {
		asp01_grip_enable(data);
		usleep_range(10000, 11000);
	}

	err = i2c_smbus_write_byte_data(data->client,
				REG_SETMFM, 0x03);
	if (err) {
		pr_err("%s: i2c write fail, err=%d, %d line\n",
			__func__, err, __LINE__);
		mutex_unlock(&data->data_mutex);
		return err;
	}
	usleep_range(20000, 21000);
	for (i = 0; i < 4; i++) {
		/* 1. set MFM unmber (MFM0~MFM3)*/
		err = i2c_smbus_write_byte_data(data->client,
				REG_SETMFM, i);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			mutex_unlock(&data->data_mutex);
			return err;
		}
		/* 2. wait until data ready */
		do {
			reg = i2c_smbus_read_byte_data(data->client,
					 REG_SETMFM);
			if (reg < 0) {
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, reg, __LINE__);
				mutex_unlock(&data->data_mutex);
				return reg;
			}
			usleep_range(1000, 1100);
			if (reg & SET_MFM_DONE) {
				pr_debug("%s: count is %d\n", __func__,
					100 - count);
				count = 100;
				break;
			}
			if (!count)
				pr_err("%s: full count state\n",
					__func__);
		} while (count--);
		/* 3. read CS percent value */
		err = i2c_smbus_write_byte_data(data->client,
				control_reg[CMD_CLK_OFF][REG],
				control_reg[CMD_CLK_OFF][CMD]);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			mutex_unlock(&data->data_mutex);
			return err;
		}
		err = i2c_smbus_read_i2c_block_data(data->client,
			REG_CS_PERL, sizeof(u8)*2,
			cs_per);
		if (err != sizeof(reg)*2) {
			pr_err("%s : i2c read fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			mutex_unlock(&data->data_mutex);
			return -EIO;
		}

		mlt_cs_per[i] = (cs_per[1] << 8) | cs_per[0];
		sum_cs_per += mlt_cs_per[i];
		/* 4. read CR percent value */
		err = i2c_smbus_read_i2c_block_data(data->client,
			REG_CR_PER_L, sizeof(u8)*2,
			cr_per);
		if (err != sizeof(reg)*2) {
			pr_err("%s :i2c read fail. err=%d, %d line\n",
				__func__, err, __LINE__);
			mutex_unlock(&data->data_mutex);
			return -EIO;
		}
		err = i2c_smbus_write_byte_data(data->client,
				control_reg[CMD_CLK_ON][REG],
				control_reg[CMD_CLK_ON][CMD]);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			mutex_unlock(&data->data_mutex);
			return err;
		}
		mlt_cr_per[i] = (cr_per[1] << 8) | cr_per[0];
		sum_cr_per += mlt_cr_per[i];
	}

#ifdef GRIP_DEBUG
		for (i = 0; i < SET_REG_NUM; i++) {
			reg = i2c_smbus_read_byte_data(data->client,
					 init_reg[i][REG]);
			if (reg < 0) {
				mutex_unlock(&data->data_mutex);
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, reg, __LINE__);
				return reg;
			}
			pr_info("%s: reg(0x%x) = 0x%x, default 0x%x\n",
				__func__,
				init_reg[i][REG], reg, init_reg[i][CMD]);
		}
		/* verifiying MFM value */
		for (i = 0; i < MFM_DATA_NUM; i++) {
			reg = i2c_smbus_read_byte_data(data->client,
					 REG_MFM_INIT_REF0 + i);
			if (reg < 0) {
				mutex_unlock(&data->data_mutex);
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, reg, __LINE__);
				return reg;
			}
			pr_info("%s: verify reg(0x%x) = 0x%x, data: 0x%x\n",
				__func__, REG_MFM_INIT_REF0 + i,
				reg, data->cal_data[i]);
		}
#endif
	if (!atomic_read(&data->enable))
		asp01_grip_disable(data);
	mutex_unlock(&data->data_mutex);

	avg_cs_per = (u16)(sum_cs_per / 4);
	avg_cr_per = (u16)(sum_cr_per / 4);

	return sprintf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",\
		mlt_cs_per[0], mlt_cs_per[1], mlt_cs_per[2], mlt_cs_per[3],\
		avg_cs_per,\
		mlt_cr_per[0], mlt_cr_per[1], mlt_cr_per[2], mlt_cr_per[3],\
		avg_cr_per);
}
static DEVICE_ATTR(raw_data, S_IRUGO,
	asp01_grip_raw_data_show, NULL);

static ssize_t asp01_grip_threshold_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", init_reg[SET_PROX_PER][CMD]);
}
static DEVICE_ATTR(threshold, S_IRUGO,
	asp01_grip_threshold_show, NULL);

static ssize_t asp01_grip_check_crcs_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	u8 reg;
	u8 crcs[48] = {0,};
	int count = 100;
	int err, i, j;
	struct asp01_data *data = dev_get_drvdata(dev);

	for (i = 0; i < 4; i++) {
		/* 1. set MFM unmber (MFM0~MFM3)*/
		err = i2c_smbus_write_byte_data(data->client,
				REG_SETMFM, i);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			return err;
		}
		/* 2. wait until data ready */
		do {
			reg = i2c_smbus_read_byte_data(data->client,
				REG_SETMFM);
			if (reg < 0) {
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__, reg, __LINE__);
				return reg;
			}
			usleep_range(10000, 11000);
			if (reg & SET_MFM_DONE) {
				pr_debug("%s: count is %d\n", __func__,
					100 - count);
				count = 100;
				break;
			}
			if (!count)
				pr_err("%s: full count state\n",
					__func__);
		} while (count--);
		/* 3. read CR CS registers */
		for (j = REG_CR_CNT; j < (REG_CS_PERH + 1); j++) {
			crcs[(12 * i) + j - REG_CR_CNT] =
				i2c_smbus_read_byte_data(data->client, j);
			if (crcs[(12 * i) + j - REG_CR_CNT] < 0) {
				pr_err("%s : i2c read fail, err=%d, %d line\n",
					__func__,
					crcs[(12 * i) + j - REG_CR_CNT],
					__LINE__);
				return -EIO;
			}
			pr_info("%s: SEL %d, reg (0x%x) = 0x%x",
				__func__, i, j,
				crcs[(12 * i) + j - REG_CR_CNT]);
		}
	}

	return sprintf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
		"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
		"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
		"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		crcs[0], crcs[1], crcs[2], crcs[3], crcs[4], crcs[5],
		crcs[6], crcs[7], crcs[8], crcs[9], crcs[10], crcs[11],
		crcs[12], crcs[13], crcs[14], crcs[15], crcs[16], crcs[17],
		crcs[18], crcs[19], crcs[20], crcs[21], crcs[22], crcs[23],
		crcs[24], crcs[25], crcs[26], crcs[27], crcs[28], crcs[29],
		crcs[30], crcs[31], crcs[32], crcs[33], crcs[34], crcs[35],
		crcs[36], crcs[37], crcs[38], crcs[39], crcs[40], crcs[41],
		crcs[42], crcs[43], crcs[44], crcs[45], crcs[46], crcs[47]);
}
static DEVICE_ATTR(check_crcs, S_IRUGO,
	asp01_grip_check_crcs_show, NULL);

static ssize_t asp01_grip_onoff_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct asp01_data *data = dev_get_drvdata(dev);

	pr_info("%s, onoff = %d\n", __func__, !data->skip_data);

	return sprintf(buf, "%d\n", !data->skip_data);
}

static ssize_t asp01_grip_onoff_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct asp01_data *data = dev_get_drvdata(dev);

	if (sysfs_streq(buf, "1"))
		data->skip_data = false;
	else if (sysfs_streq(buf, "0")) {
		iio_push_event(iio_priv_to_dev(data),
					IIO_UNMOD_EVENT_CODE(IIO_GRIP,
						0,
						IIO_EV_TYPE_THRESH,
						IIO_EV_DIR_EITHER),
					iio_get_time_ns());
		data->skip_data = true;
		pr_info("%s: onoff = 0, input report as GRIP_FAR\n",
			__func__);
	} else {
		pr_info("%s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}
	pr_info("%s, onoff = %d\n", __func__, !data->skip_data);

	return count;
}

static DEVICE_ATTR(onoff, S_IRUGO | S_IWUSR | S_IWGRP,
		   asp01_grip_onoff_show, asp01_grip_onoff_store);

static ssize_t asp01_grip_reset_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct asp01_data *data = dev_get_drvdata(dev);
	int err;

	if (sysfs_streq(buf, "1")) {
		pr_info("%s is called\n", __func__);

		mutex_lock(&data->data_mutex);
		if (atomic_read(&data->enable)) {
			disable_irq(data->pdata->irq);
		} else {
			asp01_grip_enable(data);
			usleep_range(10000, 11000);
		}

		asp01_restore_from_eeprom(data);

		err = asp01_reset(data);
		if (err) {
			pr_err("%s: asp01_reset, err = %d\n",
				__func__, err);
		}
		msleep(500);

		err = asp01_init_code_set(data);
		if (err < ASP01_DEV_WORKING)
			pr_err("%s: asp01_init_code_set, err = %d\n",
				__func__, err);
		if (atomic_read(&data->enable)) {
			enable_irq(data->pdata->irq);
		} else
			asp01_grip_disable(data);
		mutex_unlock(&data->data_mutex);

		data->init_touch_needed = true;
	} else {
		pr_err("%s: invalid value %d\n", __func__, *buf);
	}

	return count;
}
static DEVICE_ATTR(reset,    S_IWUSR | S_IWGRP,
		   NULL, asp01_grip_reset_store);

static ssize_t asp01_grip_cscr_const_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct asp01_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "cs const = %d, cr_const= %d\n",
		data->cs_cosnt, data->cr_cosnt);
}

static ssize_t asp01_grip_cscr_const_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int cs_divnd, cs_divsr, cr_divnd, cr_divsr;
	struct asp01_data *data = dev_get_drvdata(dev);

	sscanf(buf, "%d %d %d %d",
		&cs_divnd, &cs_divsr, &cr_divnd, &cr_divsr);

	if (cs_divsr && cr_divsr) {
		data->cs_cosnt = (cs_divnd * DIV) / cs_divsr;
		data->cr_cosnt = (cr_divnd * DIV) / cr_divsr;
	}
	return count;
}
static DEVICE_ATTR(cscr_const, S_IRUGO | S_IWUSR | S_IWGRP,
		   asp01_grip_cscr_const_show, asp01_grip_cscr_const_store);

static ssize_t asp01_reg_debug_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct asp01_data *data = dev_get_drvdata(dev);
	u8 tmp_data[12];
	u16 reg_data[6];
	int i;

	/* 0x04 ~ 0x0F */
	i2c_smbus_read_i2c_block_data(data->client,
		REG_CR_CNT, sizeof(u8)*12, tmp_data);

	for (i = 0; i < 6; i++) {
		reg_data[i] = (u16)(tmp_data[i * 2] | tmp_data[i * 2 + 1] << 8);
		pr_info("%s, reg_data[%d] = %d\n", __func__, 4 + i * 2,
			reg_data[i]);
	}

	/* 0x23 */
	tmp_data[0] = i2c_smbus_read_byte_data(data->client, REG_SYS_FUNC);
	pr_info("%s, sys_func = 0x%x\n", __func__, tmp_data[0]);

	/* 0x1F */
	tmp_data[0] = i2c_smbus_read_byte_data(data->client, REG_INIT_REF);
	pr_info("%s, init_ref = 0x%x\n", __func__, tmp_data[0]);

	return sprintf(buf, "0x%x,0x%x,0x%x,0x%x,0x%x,0x%x\n",
		reg_data[0], reg_data[1], reg_data[2], reg_data[3],
		reg_data[4], reg_data[5]);
}
static DEVICE_ATTR(reg_debug, S_IRUGO,
		   asp01_reg_debug_show, NULL);

static ssize_t asp01_erase_cal_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct asp01_data *data = dev_get_drvdata(dev);
	int err, i;

	if (sysfs_streq(buf, "1")) {
		pr_info("%s is called\n", __func__);
	mutex_lock(&data->data_mutex);

	asp01_grip_enable(data);
	usleep_range(10000, 11000);
	asp01_restore_from_eeprom(data);
	asp01_init_touch_onoff(data, false);

	/* reset MFM ref value */
	memset(data->cal_data, 0, ARRAY_SIZE(data->cal_data));
	for (i = 0; i < MFM_DATA_NUM; i++) {
		err = i2c_smbus_write_byte_data(data->client,
				REG_MFM_INIT_REF0 + i, 0);
		if (err) {
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
			goto exit;
		}
		pr_debug("%s: reset reg(0x%x) = 0x%x\n", __func__,
			REG_MFM_INIT_REF0 + i, data->cal_data[i]);
	}
	data->init_touch_needed = false;
	asp01_write_to_eeprom(data);

	err = asp01_save_caldata(data);
	if (err < 0) {
		pr_err("%s, asp01_save_caldata fail(%d)\n",
			__func__, err);
	}
exit:
	asp01_grip_disable(data);
	mutex_unlock(&data->data_mutex);
	} else {
		pr_err("%s: invalid value %d\n", __func__, *buf);
	}

	return count;
}
static DEVICE_ATTR(erase_cal, S_IWUSR | S_IWGRP,
		   NULL, asp01_erase_cal_store);

static struct device_attribute *asp01_sensor_attrs[] = {
	&dev_attr_erase_cal,
	&dev_attr_reg_debug,
	&dev_attr_cscr_const,
	&dev_attr_reset,
	&dev_attr_onoff,
	&dev_attr_check_crcs,
	&dev_attr_threshold,
	&dev_attr_calibration,
	&dev_attr_name,
	&dev_attr_vendor,
	&dev_attr_raw_data,
	NULL,
};
#endif

static int asp01_parse_dt(struct asp01_platform_data *data,
			struct device *dev)
{
	struct device_node *this_node= dev->of_node;
	enum of_gpio_flags flags;
	u32 cr_cs_div[4];
	u32 temp_init_code[SET_REG_NUM], i = 0;

	if (this_node == NULL)
		return -ENODEV;

	data->gpio = of_get_named_gpio_flags(this_node,
						"asp01,irq_gpio", 0, &flags);
	if (data->gpio < 0) {
		pr_err("%s : get irq_gpio(%d) error\n", __func__, data->gpio);
		return -ENODEV;
	}

	if (of_property_read_u32_array(this_node,
		"asp01,cr_cs_div", cr_cs_div, 4) < 0) {
		pr_err("%s : get temp_init_code(%d) error\n", __func__, cr_cs_div[0]);
		return -ENODEV;
	}
	data->cr_divsr = cr_cs_div[0];
	data->cr_divnd = cr_cs_div[1];
	data->cs_divsr = cr_cs_div[2];
	data->cs_divnd = cr_cs_div[3];

	if (of_property_read_u32_array(this_node,
		"asp01,init_code", temp_init_code, SET_REG_NUM) < 0) {
		pr_err("%s : get temp_init_code(%d) error\n", __func__, temp_init_code[0]);
		return -ENODEV;
	}
	for (i = 0 ; i < SET_REG_NUM ; i++) {
		data->init_code[i] = temp_init_code[i];
		pr_info("%s : %x\n", __func__, temp_init_code[i]);
	}

	return 0;
}

static int asp01_regulator_onoff(struct device *dev, bool onoff)
{
	struct regulator *asp01_vcc, *asp01_lvs1;

	asp01_vcc = devm_regulator_get(dev, "asp01-vcc");
	if (IS_ERR(asp01_vcc)) {
		pr_err("%s: cannot get asp01_vcc\n", __func__);
		return -ENOMEM;
	}

	asp01_lvs1 = devm_regulator_get(dev, "asp01-lvs1");
	if (IS_ERR(asp01_lvs1)) {
		pr_err("%s: cannot get asp01_vcc\n", __func__);
		devm_regulator_put(asp01_vcc);
		return -ENOMEM;
	}

	if (onoff) {
		regulator_enable(asp01_vcc);
		regulator_enable(asp01_lvs1);
	} else {
		regulator_disable(asp01_vcc);
		regulator_disable(asp01_lvs1);
	}

	devm_regulator_put(asp01_vcc);
	devm_regulator_put(asp01_lvs1);
	msleep(20);

	return 0;
}

static int asp01_setup_irq(struct asp01_data *data)
{
	int rc = -EIO;

	pr_err("%s, start\n", __func__);

	rc = gpio_request(data->pdata->gpio, "gpio_grip_out");
	if (unlikely(rc < 0)) {
		pr_err("%s: gpio %d request failed (%d)\n",
				__func__, data->pdata->gpio, rc);
		goto done;
	}

	rc = gpio_direction_input(data->pdata->gpio);
	if (unlikely(rc < 0)) {
		pr_err("%s: failed to set gpio %d as input (%d)\n",
				__func__, data->pdata->gpio, rc);
		goto err_gpio_direction_input;
	}

	data->pdata->irq = gpio_to_irq(data->pdata->gpio);
	rc = request_threaded_irq(data->pdata->irq, NULL,
			asp01_irq_thread,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"grip_int", data);
	if (unlikely(rc < 0)) {
		pr_err("%s: request_irq(%d) failed for gpio %d (%d)\n",
				__func__, data->pdata->irq, data->pdata->gpio, rc);
		goto err_request_irq;
	}

	disable_irq(data->pdata->irq);

	pr_info("%s, success\n", __func__);

	goto done;

err_request_irq:
err_gpio_direction_input:
	gpio_free(data->pdata->gpio);
done:
	return rc;
}

static int asp01_suspend(struct device *dev)
{
	int err = 0;
	struct asp01_data *data = dev_get_drvdata(dev);

	pr_info("%s, enable = %d\n", __func__, atomic_read(&data->enable));
	if (atomic_read(&data->enable))
		enable_irq_wake(data->pdata->irq);
	else {
		mutex_lock(&data->data_mutex);
		err = i2c_smbus_write_byte_data(data->client,
				control_reg[CMD_CLK_OFF][REG],
				control_reg[CMD_CLK_OFF][CMD]);
		if (err)
			pr_err("%s: i2c write fail, err=%d, %d line\n",
				__func__, err, __LINE__);
		mutex_unlock(&data->data_mutex);
	}
	return err;
}

static int asp01_resume(struct device *dev)
{
	struct asp01_data *data = dev_get_drvdata(dev);

	pr_info("%s, enable = %d\n", __func__, atomic_read(&data->enable));
	if (atomic_read(&data->enable))
		disable_irq_wake(data->pdata->irq);

	return 0;
}

static const struct dev_pm_ops asp01_pm_ops = {
	.suspend = asp01_suspend,
	.resume = asp01_resume,
};

static int asp01_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct asp01_data *data;
	int err;
	u8 reg;

	pr_info("%s, is started.\n", __func__);
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA |
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK)) {
		pr_err("%s: i2c functionality check failed!\n", __func__);
		err = -ENODEV;
		goto exit;
	}

	indio_dev = iio_allocate_device(sizeof(*data));
	if (indio_dev == NULL) {
		pr_err("memory allocation failed\n");
		err =  -ENOMEM;
		goto exit;
	}

	data = iio_priv(indio_dev);
	data->indio_dev = indio_dev;
	data->client = client;

	data->pdata = kzalloc(sizeof(struct asp01_platform_data), GFP_KERNEL);
	if (data->pdata == NULL) {
		pr_err("failed to allocate memory for module data\n");
		err = -ENOMEM;
		goto pdata_kzalloc_err;
	}

	asp01_regulator_onoff(&client->dev, true);

	err = asp01_parse_dt(data->pdata, &client->dev);
	if (err) {
		pr_err("Could not initialize device.\n");
		goto pdata_kzalloc_err;
	}

	reg = i2c_smbus_read_byte_data(client,
		REG_ID_CHECK);
	if (reg < 0) {
		pr_err("%s : i2c read fail, err=%d, %d line\n",
			__func__, reg, __LINE__);
		err = reg;
		goto err_read_reg;
	}
	if (reg != SLAVE_ADDR) {
		pr_err("%s : Invalid slave address(0x%x)\n", __func__, reg);
		err = -EIO;
		goto err_read_reg;
	}

	data->client = client;
	i2c_set_clientdata(client, data);

	atomic_set(&data->enable, 0);
	data->skip_data = false;

	indio_dev->name = "grip-level";
	indio_dev->channels = asp01_channels;
	indio_dev->num_channels = ARRAY_SIZE(asp01_channels);
	indio_dev->dev.parent = &client->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &asp01_info;

	err = iio_device_register(indio_dev);
	if (err) {
		pr_err("%s: iio_device_register fail\n", __func__);
		goto err_iio_register_device_err;
	}

	err = asp01_setup_irq(data);
	if (err) {
		pr_err("%s: asp01_setup_irq fail\n", __func__);
		goto err_setup_irq_err;
	}

#ifdef CONFIG_SENSORS
	err = sensors_register(data->dev,
		data, asp01_sensor_attrs, "grip_sensor");
	if (err) {
		pr_err("%s: cound not register prox sensor device(%d).\n",
		__func__, err);
		goto err_grip_sensor_register_failed;
	}

#endif

	mutex_init(&data->data_mutex);
	wake_lock_init(&data->gr_wake_lock, WAKE_LOCK_SUSPEND,
		       "grip_wake_lock");

	INIT_DELAYED_WORK(&data->d_work, asp01_delay_work_func);
	INIT_DELAYED_WORK(&data->d_work_initdev, asp01_initdev_work_func);
	INIT_WORK(&data->work, asp01_work_func);

	schedule_delayed_work(&data->d_work_initdev,
				msecs_to_jiffies(10));

	goto exit;
#ifdef CONFIG_SENSORS
err_grip_sensor_register_failed:
	gpio_free(data->pdata->gpio);
#endif
err_setup_irq_err:
	iio_device_unregister(indio_dev);
err_iio_register_device_err:
err_read_reg:
	kfree(data->pdata);
pdata_kzalloc_err:
	iio_free_device(indio_dev);
exit:
	return err;
}

static struct of_device_id asp01_device_id[] = {
	{ .compatible = "asp01",},
	{},
};

static const struct i2c_device_id asp01_id[] = {
	{ "asp01", 0 },
	{}
};

MODULE_DEVICE_TABLE(i2c, asp01_id);

static struct i2c_driver asp01_driver = {
	.probe = asp01_probe,
	.id_table = asp01_id,
	.driver = {
		.pm = &asp01_pm_ops,
		.owner = THIS_MODULE,
		.name = "asp01",
		.of_match_table = asp01_device_id,
	},
};

static int __init asp01_init(void)
{
	return i2c_add_driver(&asp01_driver);
}

static void __exit asp01_exit(void)
{
	i2c_del_driver(&asp01_driver);
}

module_init(asp01_init);
module_exit(asp01_exit);

MODULE_DESCRIPTION("asp01 grip sensor driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
