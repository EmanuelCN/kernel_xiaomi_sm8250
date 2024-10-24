/*
 * BQ2570x battery charging driver
 *
 * Copyright (C) 2017 Texas Instruments *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#define pr_fmt(fmt)	"[bq2597x] %s: " fmt, __func__

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/err.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/debugfs.h>
#include <linux/bitops.h>
#include <linux/math64.h>
#include "bq25970_reg.h"
/*#include "bq2597x.h"*/

enum {
	VBUS_ERROR_NONE,
	VBUS_ERROR_LOW,
	VBUS_ERROR_HIGH,
};

enum {
	ADC_IBUS,
	ADC_VBUS,
	ADC_VAC,
	ADC_VOUT,
	ADC_VBAT,
	ADC_IBAT,
	ADC_TBUS,
	ADC_TBAT,
	ADC_TDIE,
	ADC_MAX_NUM,
};

static int sc8551_adc_lsb[] = {
	[ADC_IBUS]	= SC8551_IBUS_ADC_LSB,
	[ADC_VBUS]	= SC8551_VBUS_ADC_LSB,
	[ADC_VAC]	= SC8551_VAC_ADC_LSB,
	[ADC_VOUT]	= SC8551_VOUT_ADC_LSB,
	[ADC_VBAT]	= SC8551_VBAT_ADC_LSB,
	[ADC_IBAT]	= SC8551_IBAT_ADC_LSB,
	[ADC_TBUS]	= SC8551_TSBUS_ADC_LSB,
	[ADC_TBAT]	= SC8551_TSBAT_ADC_LSB,
	[ADC_TDIE]	= SC8551_TDIE_ADC_LSB,
};

enum bq25970_role {
	BQ25970_ROLE_STDALONE      = 0x0,
	BQ25970_ROLE_MASTER          = 0x1,
	BQ25970_ROLE_SLAVE           = 0x2,
};


enum {
	BQ25968,
	BQ25970,
	SC8551,
	NU2105,
};

#define	BAT_OVP_ALARM		BIT(7)
#define BAT_OCP_ALARM		BIT(6)
#define	BUS_OVP_ALARM		BIT(5)
#define	BUS_OCP_ALARM		BIT(4)
#define	BAT_UCP_ALARM		BIT(3)
#define	VBUS_INSERT		BIT(2)
#define VBAT_INSERT		BIT(1)
#define	ADC_DONE		BIT(0)

#define BAT_OVP_FAULT		BIT(7)
#define BAT_OCP_FAULT		BIT(6)
#define BUS_OVP_FAULT		BIT(5)
#define BUS_OCP_FAULT		BIT(4)
#define TBUS_TBAT_ALARM		BIT(3)
#define TS_BAT_FAULT		BIT(2)
#define	TS_BUS_FAULT		BIT(1)
#define	TS_DIE_FAULT		BIT(0)

/*below used for comm with other module*/
#define	BAT_OVP_FAULT_SHIFT			0
#define	BAT_OCP_FAULT_SHIFT			1
#define	BUS_OVP_FAULT_SHIFT			2
#define	BUS_OCP_FAULT_SHIFT			3
#define	BAT_THERM_FAULT_SHIFT			4
#define	BUS_THERM_FAULT_SHIFT			5
#define	DIE_THERM_FAULT_SHIFT			6

#define	BAT_OVP_FAULT_MASK		(1 << BAT_OVP_FAULT_SHIFT)
#define	BAT_OCP_FAULT_MASK		(1 << BAT_OCP_FAULT_SHIFT)
#define	BUS_OVP_FAULT_MASK		(1 << BUS_OVP_FAULT_SHIFT)
#define	BUS_OCP_FAULT_MASK		(1 << BUS_OCP_FAULT_SHIFT)
#define	BAT_THERM_FAULT_MASK		(1 << BAT_THERM_FAULT_SHIFT)
#define	BUS_THERM_FAULT_MASK		(1 << BUS_THERM_FAULT_SHIFT)
#define	DIE_THERM_FAULT_MASK		(1 << DIE_THERM_FAULT_SHIFT)

#define	BAT_OVP_ALARM_SHIFT			0
#define	BAT_OCP_ALARM_SHIFT			1
#define	BUS_OVP_ALARM_SHIFT			2
#define	BUS_OCP_ALARM_SHIFT			3
#define	BAT_THERM_ALARM_SHIFT			4
#define	BUS_THERM_ALARM_SHIFT			5
#define	DIE_THERM_ALARM_SHIFT			6
#define BAT_UCP_ALARM_SHIFT			7

#define	BAT_OVP_ALARM_MASK		(1 << BAT_OVP_ALARM_SHIFT)
#define	BAT_OCP_ALARM_MASK		(1 << BAT_OCP_ALARM_SHIFT)
#define	BUS_OVP_ALARM_MASK		(1 << BUS_OVP_ALARM_SHIFT)
#define	BUS_OCP_ALARM_MASK		(1 << BUS_OCP_ALARM_SHIFT)
#define	BAT_THERM_ALARM_MASK		(1 << BAT_THERM_ALARM_SHIFT)
#define	BUS_THERM_ALARM_MASK		(1 << BUS_THERM_ALARM_SHIFT)
#define	DIE_THERM_ALARM_MASK		(1 << DIE_THERM_ALARM_SHIFT)
#define	BAT_UCP_ALARM_MASK		(1 << BAT_UCP_ALARM_SHIFT)

#define VBAT_REG_STATUS_SHIFT			0
#define IBAT_REG_STATUS_SHIFT			1

#define VBAT_REG_STATUS_MASK		(1 << VBAT_REG_STATUS_SHIFT)
#define IBAT_REG_STATUS_MASK		(1 << VBAT_REG_STATUS_SHIFT)

#define bq_err(fmt, ...)								\
do {											\
	if (bq->mode == BQ25970_ROLE_MASTER)						\
		printk(KERN_ERR "[bq2597x-MASTER]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else if (bq->mode == BQ25970_ROLE_SLAVE)					\
		printk(KERN_ERR "[bq2597x-SLAVE]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else										\
		printk(KERN_ERR "[bq2597x-STANDALONE]:%s:" fmt, __func__, ##__VA_ARGS__);\
} while (0);

#define bq_info(fmt, ...)								\
do {											\
	if (bq->mode == BQ25970_ROLE_MASTER)						\
		printk(KERN_INFO "[bq2597x-MASTER]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else if (bq->mode == BQ25970_ROLE_SLAVE)					\
		printk(KERN_INFO "[bq2597x-SLAVE]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else										\
		printk(KERN_INFO "[bq2597x-STANDALONE]:%s:" fmt, __func__, ##__VA_ARGS__);\
} while (0);

#define bq_dbg(fmt, ...)								\
do {											\
	if (bq->mode == BQ25970_ROLE_MASTER)						\
		printk(KERN_DEBUG "[bq2597x-MASTER]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else if (bq->mode == BQ25970_ROLE_SLAVE)					\
		printk(KERN_DEBUG "[bq2597x-SLAVE]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else										\
		printk(KERN_DEBUG "[bq2597x-STANDALONE]:%s:" fmt, __func__, ##__VA_ARGS__);\
} while (0);

enum hvdcp3_type {
	HVDCP3_NONE = 0,
	HVDCP3_CLASSA_18W,
	HVDCP3_CLASSB_27W,
	HVDCP3P5_CLASSA_18W,
	HVDCP3P5_CLASSB_27W,
};

#define BUS_OVP_FOR_QC			10500
#define BUS_OVP_ALARM_FOR_QC			9500
#define BUS_OCP_FOR_QC_CLASS_A			3250
#define BUS_OCP_ALARM_FOR_QC_CLASS_A			2000
#define BUS_OCP_FOR_QC_CLASS_B			4000
#define BUS_OCP_ALARM_FOR_QC_CLASS_B			3000
#define BUS_OCP_FOR_QC3P5_CLASS_A			3000
#define BUS_OCP_ALARM_FOR_QC3P5_CLASS_A		2500
#define BUS_OCP_FOR_QC3P5_CLASS_B			3500
#define BUS_OCP_ALARM_FOR_QC3P5_CLASS_B		3200

/*end*/

struct bq2597x_cfg {
	bool bat_ovp_disable;
	bool bat_ocp_disable;
	bool bat_ovp_alm_disable;
	bool bat_ocp_alm_disable;

	int bat_ovp_th;
	int bat_ovp_alm_th;
	int bat_ocp_th;
	int bat_ocp_alm_th;

	bool bus_ovp_alm_disable;
	bool bus_ocp_disable;
	bool bus_ocp_alm_disable;

	int bus_ovp_th;
	int bus_ovp_alm_th;
	int bus_ocp_th;
	int bus_ocp_alm_th;

	bool bat_ucp_alm_disable;

	int bat_ucp_alm_th;
	int ac_ovp_th;

	bool bat_therm_disable;
	bool bus_therm_disable;
	bool die_therm_disable;

	int bat_therm_th; /*in %*/
	int bus_therm_th; /*in %*/
	int die_therm_th; /*in degC*/

	int sense_r_mohm;
};

struct bq2597x {
	struct device *dev;
	struct i2c_client *client;

	int part_no;
	int revision;

	int chip_vendor;
	int mode;

	struct mutex data_lock;
	struct mutex i2c_rw_lock;
	struct mutex charging_disable_lock;
	struct mutex irq_complete;

	bool irq_waiting;
	bool irq_disabled;
	bool resume_completed;

	bool batt_present;
	bool vbus_present;

	bool usb_present;
	bool charge_enabled;	/* Register bit status */

	/* ADC reading */
	int vbat_volt;
	int vbus_volt;
	int vout_volt;
	int vac_volt;

	int ibat_curr;
	int ibus_curr;

	int bat_temp;
	int bus_temp;
	int die_temp;

	/* alarm/fault status */
	bool bat_ovp_fault;
	bool bat_ocp_fault;
	bool bus_ovp_fault;
	bool bus_ocp_fault;

	bool bat_ovp_alarm;
	bool bat_ocp_alarm;
	bool bus_ovp_alarm;
	bool bus_ocp_alarm;

	bool bat_ucp_alarm;

	bool bat_therm_alarm;
	bool bus_therm_alarm;
	bool die_therm_alarm;

	bool bat_therm_fault;
	bool bus_therm_fault;
	bool die_therm_fault;

	bool therm_shutdown_flag;
	bool therm_shutdown_stat;

	bool vbat_reg;
	bool ibat_reg;

	int  prev_alarm;
	int  prev_fault;

	int chg_ma;
	int chg_mv;

	int charge_state;

	struct bq2597x_cfg *cfg;

	int skip_writes;
	int skip_reads;

	struct bq2597x_platform_data *platform_data;

	struct delayed_work monitor_work;

	struct dentry *debug_root;

	struct power_supply_desc psy_desc;
	struct power_supply_config psy_cfg;
	struct power_supply *fc2_psy;
};

static int bq2597x_set_acovp_th(struct bq2597x *bq, int threshold);
static int bq2597x_set_busovp_th(struct bq2597x *bq, int threshold);

/************************************************************************/
static int __bq2597x_read_byte(struct bq2597x *bq, u8 reg, u8 *data)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(bq->client, reg);
	if (ret < 0) {
		bq_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u8) ret;

	return 0;
}

static int __bq2597x_write_byte(struct bq2597x *bq, int reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(bq->client, reg, val);
	if (ret < 0) {
		bq_err("i2c write fail: can't write 0x%02X to reg 0x%02X: %d\n",
		       val, reg, ret);
		return ret;
	}
	return 0;
}

static int __bq2597x_read_word(struct bq2597x *bq, u8 reg, u16 *data)
{
	s32 ret;

	ret = i2c_smbus_read_word_data(bq->client, reg);
	if (ret < 0) {
		bq_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u16) ret;

	return 0;
}

static int bq2597x_read_byte(struct bq2597x *bq, u8 reg, u8 *data)
{
	int ret;

	if (bq->skip_reads) {
		*data = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_read_byte(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int bq2597x_write_byte(struct bq2597x *bq, u8 reg, u8 data)
{
	int ret;

	if (bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_write_byte(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int bq2597x_read_word(struct bq2597x *bq, u8 reg, u16 *data)
{
	int ret;

	if (bq->skip_reads) {
		*data = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_read_word(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int bq2597x_update_bits(struct bq2597x *bq, u8 reg,
				    u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	if (bq->skip_reads || bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_read_byte(bq, reg, &tmp);
	if (ret) {
		bq_err("Failed: reg=%02X, ret=%d\n", reg, ret);
		goto out;
	}

	tmp &= ~mask;
	tmp |= data & mask;

	ret = __bq2597x_write_byte(bq, reg, tmp);
	if (ret)
		bq_err("Failed: reg=%02X, ret=%d\n", reg, ret);

out:
	mutex_unlock(&bq->i2c_rw_lock);
	return ret;
}

/*********************************************************************/
//don't use 1:1 mode of sc8551A
static int sc8551_set_charge_mode(struct bq2597x *bq, int mode)
{
	int ret;
	u8 val;
//	if (bq->chip_vendor != SC8551)
//		return ret;
//	if (mode != SC8551_CHARGE_MODE_BYPASS && mode != SC8551_CHARGE_MODE_DIV2)
//		return ret;
	pr_err("%s mode:%d \n", __func__, mode);
	val = mode << SC8551_CHARGE_MODE_SHIFT;
	ret = bq2597x_update_bits(bq, SC8551_REG_31,
				SC8551_CHARGE_MODE_MASK, val);
	/* in bypass mode, ovp will be set to half value automatically */
	/* in charge_pump mode, should set it manually */
	if (mode == SC8551_CHARGE_MODE_DIV2) {
		ret = bq2597x_set_acovp_th(bq, bq->cfg->ac_ovp_th);
		ret = bq2597x_set_busovp_th(bq, bq->cfg->bus_ovp_th);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_charge_mode);
static int sc8551_get_charge_mode(struct bq2597x *bq)
{
	int ret;
	u8 val;
	if (bq->chip_vendor != SC8551)
		return -1;
	ret = bq2597x_read_byte(bq, SC8551_REG_31, &val);
	pr_err("%s val:%d \n", __func__, val);
	return (int)(val & SC8551_CHARGE_MODE_MASK);
}
EXPORT_SYMBOL_GPL(sc8551_get_charge_mode);

/*********************************************************************/

static int bq2597x_enable_charge(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_CHG_ENABLE;
	else
		val = BQ2597X_CHG_DISABLE;

	val <<= BQ2597X_CHG_EN_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_CHG_EN_MASK, val);
	pr_err("%s enable:%d \n", __func__, enable);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_charge);

static int bq2597x_check_charge_enabled(struct bq2597x *bq, bool *enabled)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0C, &val);
	if (!ret)
		*enabled = !!(val & BQ2597X_CHG_EN_MASK);
	return ret;
}

static int bq2597x_enable_wdt(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_WATCHDOG_ENABLE;
	else
		val = BQ2597X_WATCHDOG_DISABLE;

	val <<= BQ2597X_WATCHDOG_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0B,
				BQ2597X_WATCHDOG_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_wdt);

static int bq2597x_set_wdt(struct bq2597x *bq, int ms)
{
	int ret;
	u8 val;

	if (ms == 500)
		val = BQ2597X_WATCHDOG_0P5S;
	else if (ms == 1000)
		val = BQ2597X_WATCHDOG_1S;
	else if (ms == 5000)
		val = BQ2597X_WATCHDOG_5S;
	else if (ms == 30000)
		val = BQ2597X_WATCHDOG_30S;
	else
		val = BQ2597X_WATCHDOG_30S;

	val <<= BQ2597X_WATCHDOG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0B,
				BQ2597X_WATCHDOG_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_wdt);

static int bq2597x_enable_batovp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OVP_ENABLE;
	else
		val = BQ2597X_BAT_OVP_DISABLE;

	val <<= BQ2597X_BAT_OVP_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_00,
				BQ2597X_BAT_OVP_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_batovp);

static int bq2597x_set_batovp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OVP_BASE)
		threshold = BQ2597X_BAT_OVP_BASE;

	val = (threshold - BQ2597X_BAT_OVP_BASE) / BQ2597X_BAT_OVP_LSB;

	val <<= BQ2597X_BAT_OVP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_00,
				BQ2597X_BAT_OVP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_batovp_th);

static int bq2597x_enable_batovp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OVP_ALM_ENABLE;
	else
		val = BQ2597X_BAT_OVP_ALM_DISABLE;

	val <<= BQ2597X_BAT_OVP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_01,
				BQ2597X_BAT_OVP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_batovp_alarm);

static int bq2597x_set_batovp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OVP_ALM_BASE)
		threshold = BQ2597X_BAT_OVP_ALM_BASE;

	val = (threshold - BQ2597X_BAT_OVP_ALM_BASE) / BQ2597X_BAT_OVP_ALM_LSB;

	val <<= BQ2597X_BAT_OVP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_01,
				BQ2597X_BAT_OVP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_batovp_alarm_th);

static int bq2597x_enable_batocp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OCP_ENABLE;
	else
		val = BQ2597X_BAT_OCP_DISABLE;

	val <<= BQ2597X_BAT_OCP_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_02,
				BQ2597X_BAT_OCP_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_batocp);

static int bq2597x_set_batocp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OCP_BASE)
		threshold = BQ2597X_BAT_OCP_BASE;

	val = (threshold - BQ2597X_BAT_OCP_BASE) / BQ2597X_BAT_OCP_LSB;

	val <<= BQ2597X_BAT_OCP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_02,
				BQ2597X_BAT_OCP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_batocp_th);

static int bq2597x_enable_batocp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OCP_ALM_ENABLE;
	else
		val = BQ2597X_BAT_OCP_ALM_DISABLE;

	val <<= BQ2597X_BAT_OCP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_03,
				BQ2597X_BAT_OCP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_batocp_alarm);

static int bq2597x_set_batocp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OCP_ALM_BASE)
		threshold = BQ2597X_BAT_OCP_ALM_BASE;

	val = (threshold - BQ2597X_BAT_OCP_ALM_BASE) / BQ2597X_BAT_OCP_ALM_LSB;

	val <<= BQ2597X_BAT_OCP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_03,
				BQ2597X_BAT_OCP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_batocp_alarm_th);


static int bq2597x_set_busovp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OVP_BASE)
		threshold = BQ2597X_BUS_OVP_BASE;

	val = (threshold - BQ2597X_BUS_OVP_BASE) / BQ2597X_BUS_OVP_LSB;

	val <<= BQ2597X_BUS_OVP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_06,
				BQ2597X_BUS_OVP_MASK, val);


	//ret = bq2597x_update_bits(bq, 0x31,
	//			0x20, 0x20);
	bq_err("diable busovp %d,val:%d,ret:%d\n", threshold,val,ret);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_busovp_th);

static int bq2597x_enable_busovp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BUS_OVP_ALM_ENABLE;
	else
		val = BQ2597X_BUS_OVP_ALM_DISABLE;

	val <<= BQ2597X_BUS_OVP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_07,
				BQ2597X_BUS_OVP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_busovp_alarm);

static int bq2597x_set_busovp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OVP_ALM_BASE)
		threshold = BQ2597X_BUS_OVP_ALM_BASE;

	val = (threshold - BQ2597X_BUS_OVP_ALM_BASE) / BQ2597X_BUS_OVP_ALM_LSB;

	val <<= BQ2597X_BUS_OVP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_07,
				BQ2597X_BUS_OVP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_busovp_alarm_th);

static int bq2597x_enable_busocp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BUS_OCP_ENABLE;
	else
		val = BQ2597X_BUS_OCP_DISABLE;

	val <<= BQ2597X_BUS_OCP_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_08,
				BQ2597X_BUS_OCP_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_busocp);


static int bq2597x_set_busocp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OCP_BASE)
		threshold = BQ2597X_BUS_OCP_BASE;

	val = (threshold - BQ2597X_BUS_OCP_BASE) / BQ2597X_BUS_OCP_LSB;

	val <<= BQ2597X_BUS_OCP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_08,
				BQ2597X_BUS_OCP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_busocp_th);

static int bq2597x_enable_busocp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BUS_OCP_ALM_ENABLE;
	else
		val = BQ2597X_BUS_OCP_ALM_DISABLE;

	val <<= BQ2597X_BUS_OCP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_09,
				BQ2597X_BUS_OCP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_busocp_alarm);

static int bq2597x_set_busocp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OCP_ALM_BASE)
		threshold = BQ2597X_BUS_OCP_ALM_BASE;

	val = (threshold - BQ2597X_BUS_OCP_ALM_BASE) / BQ2597X_BUS_OCP_ALM_LSB;

	val <<= BQ2597X_BUS_OCP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_09,
				BQ2597X_BUS_OCP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_busocp_alarm_th);

static int bq2597x_enable_batucp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_UCP_ALM_ENABLE;
	else
		val = BQ2597X_BAT_UCP_ALM_DISABLE;

	val <<= BQ2597X_BAT_UCP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_04,
				BQ2597X_BAT_UCP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_batucp_alarm);

static int bq2597x_set_batucp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_UCP_ALM_BASE)
		threshold = BQ2597X_BAT_UCP_ALM_BASE;

	val = (threshold - BQ2597X_BAT_UCP_ALM_BASE) / BQ2597X_BAT_UCP_ALM_LSB;

	val <<= BQ2597X_BAT_UCP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_04,
				BQ2597X_BAT_UCP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_batucp_alarm_th);

static int bq2597x_set_acovp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_AC_OVP_BASE)
		threshold = BQ2597X_AC_OVP_BASE;

	if (threshold == BQ2597X_AC_OVP_6P5V)
		val = 0x07;
	else
		val = (threshold - BQ2597X_AC_OVP_BASE) /  BQ2597X_AC_OVP_LSB;

	val <<= BQ2597X_AC_OVP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_05,
				BQ2597X_AC_OVP_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(bq2597x_set_acovp_th);

static int bq2597x_set_vdrop_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold == 300)
		val = BQ2597X_VDROP_THRESHOLD_300MV;
	else
		val = BQ2597X_VDROP_THRESHOLD_400MV;

	val <<= BQ2597X_VDROP_THRESHOLD_SET_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_05,
				BQ2597X_VDROP_THRESHOLD_SET_MASK,
				val);

	return ret;
}

static int bq2597x_set_vdrop_deglitch(struct bq2597x *bq, int us)
{
	int ret;
	u8 val;

	if (us == 8)
		val = BQ2597X_VDROP_DEGLITCH_8US;
	else
		val = BQ2597X_VDROP_DEGLITCH_5MS;

	val <<= BQ2597X_VDROP_DEGLITCH_SET_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_05,
				BQ2597X_VDROP_DEGLITCH_SET_MASK,
				val);
	return ret;
}

static int bq2597x_enable_bat_therm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_TSBAT_ENABLE;
	else
		val = BQ2597X_TSBAT_DISABLE;

	val <<= BQ2597X_TSBAT_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_TSBAT_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_bat_therm);

/*
 * the input threshold is the raw value that would write to register directly.
 */
static int bq2597x_set_bat_therm_th(struct bq2597x *bq, u8 threshold)
{
	int ret;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_29, threshold);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_bat_therm_th);

static int bq2597x_enable_bus_therm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_TSBUS_ENABLE;
	else
		val = BQ2597X_TSBUS_DISABLE;

	val <<= BQ2597X_TSBUS_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_TSBUS_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_bus_therm);

/*
 * the input threshold is the raw value that would write to register directly.
 */
static int bq2597x_set_bus_therm_th(struct bq2597x *bq, u8 threshold)
{
	int ret;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_28, threshold);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_bus_therm_th);


static int bq2597x_enable_die_therm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_TDIE_ENABLE;
	else
		val = BQ2597X_TDIE_DISABLE;

	val <<= BQ2597X_TDIE_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_TDIE_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_die_therm);

/*
 * please be noted that the unit here is degC
 */
static int bq2597x_set_die_therm_th(struct bq2597x *bq, u8 threshold)
{
	int ret;
	u8 val;

	/*BE careful, LSB is here is 1/LSB, so we use multiply here*/
	val = (threshold - BQ2597X_TDIE_ALM_BASE) * BQ2597X_TDIE_ALM_LSB;
	val <<= BQ2597X_TDIE_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2A,
				BQ2597X_TDIE_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_die_therm_th);

static int bq2597x_enable_adc(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_ADC_ENABLE;
	else
		val = BQ2597X_ADC_DISABLE;

	val <<= BQ2597X_ADC_EN_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_EN_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_enable_adc);

static int bq2597x_set_adc_average(struct bq2597x *bq, bool avg)
{
	int ret;
	u8 val;

	if (avg)
		val = BQ2597X_ADC_AVG_ENABLE;
	else
		val = BQ2597X_ADC_AVG_DISABLE;

	val <<= BQ2597X_ADC_AVG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_AVG_MASK, val);
	return 0;
}
EXPORT_SYMBOL_GPL(bq2597x_set_adc_average);

static int bq2597x_set_adc_scanrate(struct bq2597x *bq, bool oneshot)
{
	int ret;
	u8 val;

	if (oneshot)
		val = BQ2597X_ADC_RATE_ONESHOT;
	else
		val = BQ2597X_ADC_RATE_CONTINOUS;

	val <<= BQ2597X_ADC_RATE_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_EN_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_adc_scanrate);

static int bq2597x_set_adc_bits(struct bq2597x *bq, int bits)
{
	int ret;
	u8 val;

	if (bits > 15)
		bits = 15;
	if (bits < 12)
		bits = 12;
	val = 15 - bits;

	val <<= BQ2597X_ADC_SAMPLE_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_SAMPLE_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_adc_bits);

#define ADC_REG_BASE 0x16
static int bq2597x_get_adc_data(struct bq2597x *bq, int channel,  int *result)
{
	int ret;
	u16 val;
	u8 val_l, val_h;
	s16 t;

	if (channel < 0 || channel >= ADC_MAX_NUM)
		return -EINVAL;

	if (bq->chip_vendor == NU2105) {
		ret = bq2597x_read_byte(bq, ADC_REG_BASE + (channel << 1), &val_h);
		ret |= bq2597x_read_byte(bq, ADC_REG_BASE + (channel << 1) + 1, &val_l);
		if (ret < 0)
			return ret;
		t = val_l + (val_h << 8);
		*result = t;
	} else {
		ret = bq2597x_read_word(bq, ADC_REG_BASE + (channel << 1), &val);
		if (ret < 0)
			return ret;
		t = val & 0xFF;
		t <<= 8;
		t |= (val >> 8) & 0xFF;
		*result = t;

		if (bq->chip_vendor == SC8551) {
			*result = (u64)t * (u64)sc8551_adc_lsb[channel] / 10000000;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(bq2597x_get_adc_data);

static int bq2597x_set_adc_scan(struct bq2597x *bq, int channel, bool enable)
{
	int ret;
	u8 reg;
	u8 mask;
	u8 shift;
	u8 val;

	if (channel > ADC_MAX_NUM)
		return -EINVAL;

	if (channel == ADC_IBUS) {
		reg = BQ2597X_REG_14;
		shift = BQ2597X_IBUS_ADC_DIS_SHIFT;
		mask = BQ2597X_IBUS_ADC_DIS_MASK;
	} else {
		reg = BQ2597X_REG_15;
		shift = 8 - channel;
		mask = 1 << shift;
	}

	if (enable)
		val = 0 << shift;
	else
		val = 1 << shift;

	ret = bq2597x_update_bits(bq, reg, mask, val);

	return ret;
}

static int sc8551_init_adc(struct bq2597x *bq)
{
	int ret;
	/* improve adc accuracy */
	ret = bq2597x_write_byte(bq, SC8551_REG_34, 0x01);
	return ret;
}

static int bq2597x_set_alarm_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0F, &val);
	if (ret)
		return ret;

	val |= mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_0F, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_alarm_int_mask);

static int bq2597x_clear_alarm_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0F, &val);
	if (ret)
		return ret;

	val &= ~mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_0F, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_clear_alarm_int_mask);

static int bq2597x_set_fault_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_12, &val);
	if (ret)
		return ret;

	val |= mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_12, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_set_fault_int_mask);

static int bq2597x_clear_fault_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_12, &val);
	if (ret)
		return ret;

	val &= ~mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_12, val);

	return ret;
}
EXPORT_SYMBOL_GPL(bq2597x_clear_fault_int_mask);


static int bq2597x_set_sense_resistor(struct bq2597x *bq, int r_mohm)
{
	int ret;
	u8 val;

	if (r_mohm == 2)
		val = BQ2597X_SET_IBAT_SNS_RES_2MHM;
	else if (r_mohm == 5)
		val = BQ2597X_SET_IBAT_SNS_RES_5MHM;
	else
		return -EINVAL;

	val <<= BQ2597X_SET_IBAT_SNS_RES_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_SET_IBAT_SNS_RES_MASK,
				val);
	return ret;
}

static int bq2597x_set_ibus_ucp_thr(struct bq2597x *bq, int ibus_ucp_thr)
{
	int ret;
	u8 val;

	if (ibus_ucp_thr == 300)
		val = BQ2597X_IBUS_UCP_RISE_300MA;
	else if (ibus_ucp_thr == 500)
		val = BQ2597X_IBUS_UCP_RISE_500MA;
	else
		return -EINVAL;

	val <<= BQ2597X_IBUS_UCP_RISE_TH_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_IBUS_UCP_RISE_TH_MASK,
				val);
	return ret;
}

static int bq2597x_enable_regulation(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_EN_REGULATION_ENABLE;
	else
		val = BQ2597X_EN_REGULATION_DISABLE;

	val <<= BQ2597X_EN_REGULATION_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_EN_REGULATION_MASK,
				val);

	return ret;

}

static int bq2597x_enable_ucp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_IBUS_LOW_DG_5MS;
	else
		val = BQ2597X_IBUS_LOW_DG_10US;

	val <<= BQ2597X_IBUS_LOW_DG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2E,
				BQ2597X_IBUS_LOW_DG_MASK,
				val);

	return ret;

}

static int bq2597x_set_ss_timeout(struct bq2597x *bq, int timeout)
{
	int ret;
	u8 val;

	switch (timeout) {
	case 0:
		val = BQ2597X_SS_TIMEOUT_DISABLE;
		break;
	case 12:
		val = BQ2597X_SS_TIMEOUT_12P5MS;
		break;
	case 25:
		val = BQ2597X_SS_TIMEOUT_25MS;
		break;
	case 50:
		val = BQ2597X_SS_TIMEOUT_50MS;
		break;
	case 100:
		val = BQ2597X_SS_TIMEOUT_100MS;
		break;
	case 400:
		val = BQ2597X_SS_TIMEOUT_400MS;
		break;
	case 1500:
		val = BQ2597X_SS_TIMEOUT_1500MS;
		break;
	case 100000:
		val = BQ2597X_SS_TIMEOUT_100000MS;
		break;
	default:
		val = BQ2597X_SS_TIMEOUT_DISABLE;
		break;
	}

	val <<= BQ2597X_SS_TIMEOUT_SET_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_SS_TIMEOUT_SET_MASK,
				val);

	return ret;
}

static int bq2597x_set_ibat_reg_th(struct bq2597x *bq, int th_ma)
{
	int ret;
	u8 val;

	if (th_ma == 200)
		val = BQ2597X_IBAT_REG_200MA;
	else if (th_ma == 300)
		val = BQ2597X_IBAT_REG_300MA;
	else if (th_ma == 400)
		val = BQ2597X_IBAT_REG_400MA;
	else if (th_ma == 500)
		val = BQ2597X_IBAT_REG_500MA;
	else
		val = BQ2597X_IBAT_REG_500MA;

	val <<= BQ2597X_IBAT_REG_SHIFT;
	ret = bq2597x_update_bits(bq, BQ2597X_REG_2C,
				BQ2597X_IBAT_REG_MASK,
				val);

	return ret;

}

static int bq2597x_set_vbat_reg_th(struct bq2597x *bq, int th_mv)
{
	int ret;
	u8 val;

	if (th_mv == 50)
		val = BQ2597X_VBAT_REG_50MV;
	else if (th_mv == 100)
		val = BQ2597X_VBAT_REG_100MV;
	else if (th_mv == 150)
		val = BQ2597X_VBAT_REG_150MV;
	else
		val = BQ2597X_VBAT_REG_200MV;

	val <<= BQ2597X_VBAT_REG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2C,
				BQ2597X_VBAT_REG_MASK,
				val);

	return ret;
}


static int bq2597x_check_reg_status(struct bq2597x *bq)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_2C, &val);
	if (!ret) {
		bq->vbat_reg = !!(val & BQ2597X_VBAT_REG_ACTIVE_STAT_MASK);
		bq->ibat_reg = !!(val & BQ2597X_IBAT_REG_ACTIVE_STAT_MASK);
	}

	return ret;
}


static int bq2597x_detect_device(struct bq2597x *bq)
{
	int ret;
	u8 data;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_13, &data);
	if (ret == 0) {
		bq->part_no = (data & BQ2597X_DEV_ID_MASK);
		bq->part_no >>= BQ2597X_DEV_ID_SHIFT;

		pr_err("detect device:%d\n", data);
		if (data == SC8551_DEVICE_ID || data == SC8551A_DEVICE_ID)
			bq->chip_vendor = SC8551;
		else if (data == NU2105_DEVICE_ID)
			bq->chip_vendor = NU2105;
		else if (data == BQ25968_DEV_ID)
			bq->chip_vendor = BQ25968;
		else
			bq->chip_vendor = BQ25970;
	}

	return ret;
}

static int bq2597x_parse_dt(struct bq2597x *bq, struct device *dev)
{
	int ret;
	struct device_node *np = dev->of_node;

	bq->cfg = devm_kzalloc(dev, sizeof(struct bq2597x_cfg),
					GFP_KERNEL);

	if (!bq->cfg)
		return -ENOMEM;

	bq->cfg->bat_ovp_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ovp-disable");
	bq->cfg->bat_ocp_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ocp-disable");
	bq->cfg->bat_ovp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ovp-alarm-disable");
	bq->cfg->bat_ocp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ocp-alarm-disable");
	bq->cfg->bus_ocp_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-ocp-disable");
	bq->cfg->bus_ovp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-ovp-alarm-disable");
	bq->cfg->bus_ocp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-ocp-alarm-disable");
	bq->cfg->bat_ucp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ucp-alarm-disable");
	bq->cfg->bat_therm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-therm-disable");
	bq->cfg->bus_therm_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-therm-disable");
	bq->cfg->die_therm_disable = of_property_read_bool(np,
			"ti,bq2597x,die-therm-disable");

	ret = of_property_read_u32(np, "ti,bq2597x,bat-ovp-threshold",
			&bq->cfg->bat_ovp_th);
	if (ret) {
		bq_err("failed to read bat-ovp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bat-ovp-alarm-threshold",
			&bq->cfg->bat_ovp_alm_th);
	if (ret) {
		bq_err("failed to read bat-ovp-alarm-threshold\n");
		return ret;
	}
	/*ret = of_property_read_u32(np, "ti,bq2597x,bat-ocp-threshold",
			&bq->cfg->bat_ocp_th);
	if (ret) {
		bq_err("failed to read bat-ocp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bat-ocp-alarm-threshold",
			&bq->cfg->bat_ocp_alm_th);
	if (ret) {
		bq_err("failed to read bat-ocp-alarm-threshold\n");
		return ret;
	}*/
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ovp-threshold",
			&bq->cfg->bus_ovp_th);
	if (ret) {
		bq_err("failed to read bus-ovp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ovp-alarm-threshold",
			&bq->cfg->bus_ovp_alm_th);
	if (ret) {
		bq_err("failed to read bus-ovp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ocp-threshold",
			&bq->cfg->bus_ocp_th);
	if (ret) {
		bq_err("failed to read bus-ocp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ocp-alarm-threshold",
			&bq->cfg->bus_ocp_alm_th);
	if (ret) {
		bq_err("failed to read bus-ocp-alarm-threshold\n");
		return ret;
	}
	/*ret = of_property_read_u32(np, "ti,bq2597x,bat-ucp-alarm-threshold",
			&bq->cfg->bat_ucp_alm_th);
	if (ret) {
		bq_err("failed to read bat-ucp-alarm-threshold\n");
		return ret;
	}*/
	ret = of_property_read_u32(np, "ti,bq2597x,bat-therm-threshold",
			&bq->cfg->bat_therm_th);
	if (ret) {
		bq_err("failed to read bat-therm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-therm-threshold",
			&bq->cfg->bus_therm_th);
	if (ret) {
		bq_err("failed to read bus-therm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,die-therm-threshold",
			&bq->cfg->die_therm_th);
	if (ret) {
		bq_err("failed to read die-therm-threshold\n");
		return ret;
	}

	ret = of_property_read_u32(np, "ti,bq2597x,ac-ovp-threshold",
			&bq->cfg->ac_ovp_th);
	if (ret) {
		bq_err("failed to read ac-ovp-threshold\n");
		return ret;
	}

	if (bq->chip_vendor == SC8551) {
		ret = of_property_read_u32(np, "sc8551,ac-ovp-threshold",
				&bq->cfg->ac_ovp_th);
		if (ret) {
			bq_err("failed to read sc8551 ac-ovp-threshold\n");
			return ret;
		}
	}

	/*ret = of_property_read_u32(np, "ti,bq2597x,sense-resistor-mohm",
			&bq->cfg->sense_r_mohm);
	if (ret) {
		bq_err("failed to read sense-resistor-mohm\n");
		return ret;
	}*/


	return 0;
}

static int bq2597x_init_protection(struct bq2597x *bq)
{
	int ret;

	ret = bq2597x_enable_batovp(bq, !bq->cfg->bat_ovp_disable);
	bq_info("%s bat ovp %s\n",
		bq->cfg->bat_ovp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batocp(bq, !bq->cfg->bat_ocp_disable);
	bq_info("%s bat ocp %s\n",
		bq->cfg->bat_ocp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batovp_alarm(bq, !bq->cfg->bat_ovp_alm_disable);
	bq_info("%s bat ovp alarm %s\n",
		bq->cfg->bat_ovp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batocp_alarm(bq, !bq->cfg->bat_ocp_alm_disable);
	bq_info("%s bat ocp alarm %s\n",
		bq->cfg->bat_ocp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batucp_alarm(bq, !bq->cfg->bat_ucp_alm_disable);
	bq_info("%s bat ocp alarm %s\n",
		bq->cfg->bat_ucp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_busovp_alarm(bq, !bq->cfg->bus_ovp_alm_disable);
	bq_info("%s bus ovp alarm %s\n",
		bq->cfg->bus_ovp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_busocp(bq, !bq->cfg->bus_ocp_disable);
	bq_info("%s bus ocp %s\n",
		bq->cfg->bus_ocp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_busocp_alarm(bq, !bq->cfg->bus_ocp_alm_disable);
	bq_info("%s bus ocp alarm %s\n",
		bq->cfg->bus_ocp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_bat_therm(bq, !bq->cfg->bat_therm_disable);
	bq_info("%s bat therm %s\n",
		bq->cfg->bat_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_bus_therm(bq, !bq->cfg->bus_therm_disable);
	bq_info("%s bus therm %s\n",
		bq->cfg->bus_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_die_therm(bq, !bq->cfg->die_therm_disable);
	bq_info("%s die therm %s\n",
		bq->cfg->die_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_set_batovp_th(bq, bq->cfg->bat_ovp_th);
	bq_info("set bat ovp th %d %s\n", bq->cfg->bat_ovp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batovp_alarm_th(bq, bq->cfg->bat_ovp_alm_th);
	bq_info("set bat ovp alarm threshold %d %s\n", bq->cfg->bat_ovp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batocp_th(bq, bq->cfg->bat_ocp_th);
	bq_info("set bat ocp threshold %d %s\n", bq->cfg->bat_ocp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batocp_alarm_th(bq, bq->cfg->bat_ocp_alm_th);
	bq_info("set bat ocp alarm threshold %d %s\n", bq->cfg->bat_ocp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busovp_th(bq, bq->cfg->bus_ovp_th);
	bq_info("set bus ovp threshold %d %s\n", bq->cfg->bus_ovp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busovp_alarm_th(bq, bq->cfg->bus_ovp_alm_th);
	bq_info("set bus ovp alarm threshold %d %s\n", bq->cfg->bus_ovp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busocp_th(bq, bq->cfg->bus_ocp_th);
	bq_info("set bus ocp threshold %d %s\n", bq->cfg->bus_ocp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busocp_alarm_th(bq, bq->cfg->bus_ocp_alm_th);
	bq_info("set bus ocp alarm th %d %s\n", bq->cfg->bus_ocp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batucp_alarm_th(bq, bq->cfg->bat_ucp_alm_th);
	bq_info("set bat ucp threshold %d %s\n", bq->cfg->bat_ucp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_bat_therm_th(bq, bq->cfg->bat_therm_th);
	bq_info("set die therm threshold %d %s\n", bq->cfg->bat_therm_th,
		!ret ? "successfully" : "failed");
	ret = bq2597x_set_bus_therm_th(bq, bq->cfg->bus_therm_th);
	bq_info("set bus therm threshold %d %s\n", bq->cfg->bus_therm_th,
		!ret ? "successfully" : "failed");
	ret = bq2597x_set_die_therm_th(bq, bq->cfg->die_therm_th);
	bq_info("set die therm threshold %d %s\n", bq->cfg->die_therm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_acovp_th(bq, bq->cfg->ac_ovp_th);
	bq_info("set ac ovp threshold %d %s\n", bq->cfg->ac_ovp_th,
		!ret ? "successfully" : "failed");

	return 0;
}

static int bq2597x_set_bus_protection(struct bq2597x *bq, int hvdcp3_type)
{
	/* just return now, to do later */
	//return 0;

	pr_err("hvdcp3_type: %d\n", hvdcp3_type);
	if (hvdcp3_type == HVDCP3_CLASSA_18W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC_CLASS_A);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC_CLASS_A);
	} else if (hvdcp3_type == HVDCP3_CLASSB_27W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC_CLASS_B);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC_CLASS_B);
	} else if (hvdcp3_type == HVDCP3P5_CLASSA_18W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC3P5_CLASS_A);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC3P5_CLASS_A);
	} else if (hvdcp3_type == HVDCP3P5_CLASSB_27W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC3P5_CLASS_B);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC3P5_CLASS_B);
	} else {
		bq2597x_set_busovp_th(bq, bq->cfg->bus_ovp_th);
		bq2597x_set_busovp_alarm_th(bq, bq->cfg->bus_ovp_alm_th);
		bq2597x_set_busocp_th(bq, bq->cfg->bus_ocp_th);
		bq2597x_set_busocp_alarm_th(bq, bq->cfg->bus_ocp_alm_th);
	}
	return 0;
}

static int bq2597x_init_adc(struct bq2597x *bq)
{
	bq2597x_set_adc_scanrate(bq, false);
	bq2597x_set_adc_bits(bq, 13);
	bq2597x_set_adc_average(bq, true);
	bq2597x_set_adc_scan(bq, ADC_IBUS, true);
	bq2597x_set_adc_scan(bq, ADC_VBUS, true);
	bq2597x_set_adc_scan(bq, ADC_VOUT, false);
	bq2597x_set_adc_scan(bq, ADC_VBAT, true);
	bq2597x_set_adc_scan(bq, ADC_IBAT, false);
	bq2597x_set_adc_scan(bq, ADC_TBUS, false);
	bq2597x_set_adc_scan(bq, ADC_TBAT, false);
	bq2597x_set_adc_scan(bq, ADC_TDIE, false);
	bq2597x_set_adc_scan(bq, ADC_VAC, true);

	if (bq->chip_vendor == SC8551)
		sc8551_init_adc(bq);

	bq2597x_enable_adc(bq, true);

	return 0;
}

static int bq2597x_init_int_src(struct bq2597x *bq)
{
	int ret;
	/*TODO:be careful ts bus and ts bat alarm bit mask is in
	 *	fault mask register, so you need call
	 *	bq2597x_set_fault_int_mask for tsbus and tsbat alarm
	 */
	ret = bq2597x_set_alarm_int_mask(bq, ADC_DONE
					| BAT_OCP_ALARM | BAT_UCP_ALARM
					| BAT_OVP_ALARM);
	if (ret) {
		bq_err("failed to set alarm mask:%d\n", ret);
		return ret;
	}
//#if 0
	ret = bq2597x_set_fault_int_mask(bq,
			TS_BUS_FAULT | TS_DIE_FAULT | TS_BAT_FAULT | BAT_OCP_FAULT);
	if (ret) {
		bq_err("failed to set fault mask:%d\n", ret);
		return ret;
	}
//#endif
	return ret;
}

static int bq2597x_init_regulation(struct bq2597x *bq)
{
	bq2597x_set_ibat_reg_th(bq, 200);
	bq2597x_set_vbat_reg_th(bq, 50);

	bq2597x_set_vdrop_deglitch(bq, 5000);
	bq2597x_set_vdrop_th(bq, 400);

	if (bq->chip_vendor == NU2105)
		bq2597x_enable_regulation(bq, false);
	else
		bq2597x_enable_regulation(bq, true);

	return 0;
}

static int bq2597x_init_device(struct bq2597x *bq)
{
	int ret;
	u8 val;

	if (bq->chip_vendor == NU2105) {
		//Set AC_OVP [0:2] = 0
		bq2597x_set_acovp_th(bq, 12);

		/*nu2105 init*/
		ret = bq2597x_write_byte(bq, 0x00, 0x3d);
		ret = bq2597x_write_byte(bq, 0x01, 0x34);
		ret = bq2597x_write_byte(bq, 0x02, 0x3c);
		ret = bq2597x_write_byte(bq, 0x03, 0x37);
		ret = bq2597x_write_byte(bq, 0x04, 0x28);
		ret = bq2597x_write_byte(bq, 0x05, 0x1d);
		ret = bq2597x_write_byte(bq, 0x06, 0x51);
		ret = bq2597x_write_byte(bq, 0x07, 0x46);
		ret = bq2597x_write_byte(bq, 0x08, 0x0c);
		ret = bq2597x_write_byte(bq, 0x09, 0x4c);
		ret = bq2597x_write_byte(bq, 0x0B, 0x44);
		ret = bq2597x_write_byte(bq, 0x0C, 0x17);
		ret = bq2597x_write_byte(bq, 0x0F, 0x01);
		ret = bq2597x_write_byte(bq, 0x12, 0x0e);
		ret = bq2597x_write_byte(bq, 0x14, 0xa8);
		ret = bq2597x_write_byte(bq, 0x15, 0x06);
		ret = bq2597x_write_byte(bq, 0x28, 0x28);
		ret = bq2597x_write_byte(bq, 0x29, 0x28);
		ret = bq2597x_write_byte(bq, 0x2A, 0xc8);
		ret = bq2597x_write_byte(bq, 0x2B, 0xe4);
		ret = bq2597x_write_byte(bq, 0x2C, 0x00);
		ret = bq2597x_write_byte(bq, 0x2D, 0x00);
		ret = bq2597x_write_byte(bq, 0x2E, 0x18);
		ret = bq2597x_write_byte(bq, 0x2F, 0x20);
		/*nu2105 init*/
	}

	bq2597x_enable_wdt(bq, false);
	bq2597x_set_ss_timeout(bq, 1500);
	bq2597x_set_ibus_ucp_thr(bq, 300);
	bq2597x_enable_ucp(bq,1);
	bq2597x_set_sense_resistor(bq, bq->cfg->sense_r_mohm);

	bq2597x_init_protection(bq);
	bq2597x_init_adc(bq);
	bq2597x_init_int_src(bq);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_13, &val);
	bq_err("Bq device ID = 0x%02X\n", val);
	if (!ret && val == BQ25968_DEV_ID) {
		bq_err("Bq device ID = 0x%02X\n", val);
		return 0;
	}

	bq2597x_init_regulation(bq);

	return 0;
}

static int bq2597x_set_present(struct bq2597x *bq, bool present)
{
	bq->usb_present = present;

	if (present)
		bq2597x_init_device(bq);

	return 0;
}

static ssize_t bq2597x_show_registers(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct bq2597x *bq = dev_get_drvdata(dev);
	u8 addr;
	u8 val;
	u8 tmpbuf[300];
	int len;
	int idx = 0;
	int ret;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "bq25970");
	for (addr = 0x0; addr <= 0x36; addr++) {
		ret = bq2597x_read_byte(bq, addr, &val);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,
					"Reg[%.2X] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static ssize_t bq2597x_store_register(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct bq2597x *bq = dev_get_drvdata(dev);
	int ret;
	unsigned int reg;
	unsigned int val;

	ret = sscanf(buf, "%x %x", &reg, &val);
	if (ret == 2 && reg <= 0x36)
		bq2597x_write_byte(bq, (unsigned char)reg, (unsigned char)val);

	return count;
}

static DEVICE_ATTR(registers, 0660, bq2597x_show_registers, bq2597x_store_register);

#ifdef CONFIG_DUAL_BQ2597X
static ssize_t bq2597x_show_diff_ti_bus_current(struct device *dev,struct device_attribute *attr,char *buf)
{
	struct bq2597x *bq = dev_get_drvdata(dev);
	static struct power_supply *bq2597x_slave = NULL;
	int diff_ti_bus_current = -1;
	int ti_bus_current_master = 0;
	int ti_bus_current_slave = 0;
	int result = 0;
	int rc;
	int len;
	union power_supply_propval pval = {
		0,
	};
	if(bq->mode == BQ25970_ROLE_MASTER){
		/*get bq2597x_slave ti_bus_current*/
		if(!bq2597x_slave){
			bq2597x_slave = power_supply_get_by_name("bq2597x-slave");
			if(!bq2597x_slave){
				bq_dbg("failed get bq2597x-slave \n");
				return 0;
			}
			bq_dbg("success get bq2597x-slave \n");
		}
		rc = power_supply_get_property(bq2597x_slave,POWER_SUPPLY_PROP_TI_BUS_CURRENT,&pval);
		if (rc < 0) {
			bq_dbg("failed get bq2597x-slave ti_bus_current \n");
			return -EINVAL;
		}
		ti_bus_current_slave = pval.intval;
		/*get bq2597x_master ti_bus_current*/
		rc = bq2597x_get_adc_data(bq, ADC_IBUS, &result);
		if (!rc)
			ti_bus_current_master = result;
		else
			ti_bus_current_master = bq->ibus_curr;
		/* get diff_ti_bus_current = ti_bus_current_master - ti_bus_current_slave */
		if(ti_bus_current_master > ti_bus_current_slave)
			diff_ti_bus_current = ti_bus_current_master - ti_bus_current_slave;
		else
			diff_ti_bus_current = ti_bus_current_slave - ti_bus_current_master;
	} else if (bq->mode == BQ25970_ROLE_SLAVE) {
		diff_ti_bus_current = -1;
	}
	len = snprintf(buf, 1024, "%d\n", diff_ti_bus_current);
	return len;
}
static DEVICE_ATTR(diff_ti_bus_current,0660,bq2597x_show_diff_ti_bus_current,NULL);
#endif
static struct attribute *bq2597x_attributes[] = {
	&dev_attr_registers.attr,
#ifdef CONFIG_DUAL_BQ2597X
	&dev_attr_diff_ti_bus_current.attr,
#endif
	NULL,
};

static const struct attribute_group bq2597x_attr_group = {
	.attrs = bq2597x_attributes,
};

static enum power_supply_property bq2597x_charger_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TI_BATTERY_PRESENT,
	POWER_SUPPLY_PROP_TI_VBUS_PRESENT,
	POWER_SUPPLY_PROP_TI_BATTERY_VOLTAGE,
	POWER_SUPPLY_PROP_TI_BATTERY_CURRENT,
	POWER_SUPPLY_PROP_TI_BATTERY_TEMPERATURE,
	POWER_SUPPLY_PROP_TI_BUS_VOLTAGE,
	POWER_SUPPLY_PROP_TI_BUS_CURRENT,
	POWER_SUPPLY_PROP_TI_BUS_TEMPERATURE,
	POWER_SUPPLY_PROP_TI_DIE_TEMPERATURE,
	POWER_SUPPLY_PROP_TI_ALARM_STATUS,
	POWER_SUPPLY_PROP_TI_FAULT_STATUS,
	POWER_SUPPLY_PROP_TI_REG_STATUS,
	POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_QC3,
	POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_PD,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_CHARGING_MODE,
};
static void bq2597x_check_alarm_status(struct bq2597x *bq);
static void bq2597x_check_fault_status(struct bq2597x *bq);
static int bq2597x_check_vbus_error_status(struct bq2597x *bq);

static int bq2597x_charger_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct bq2597x *bq = power_supply_get_drvdata(psy);
	int result;
	int ret;
	u8 reg_val;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		bq2597x_check_charge_enabled(bq, &bq->charge_enabled);
		val->intval = bq->charge_enabled;
		//bq_err("POWER_SUPPLY_PROP_CHARGING_ENABLED: %s\n",
				//val->intval ? "enable" : "disable");
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bq->usb_present;
		break;
	case POWER_SUPPLY_PROP_TI_BATTERY_PRESENT:
		ret = bq2597x_read_byte(bq, BQ2597X_REG_0D, &reg_val);
		if (!ret)
			bq->batt_present  = !!(reg_val & VBAT_INSERT);
		val->intval = bq->batt_present;
		break;
	case POWER_SUPPLY_PROP_TI_VBUS_PRESENT:
		ret = bq2597x_read_byte(bq, BQ2597X_REG_0D, &reg_val);
		if (!ret)
			bq->vbus_present  = !!(reg_val & VBUS_INSERT);
		val->intval = bq->vbus_present;
		break;
	case POWER_SUPPLY_PROP_TI_BATTERY_VOLTAGE:
		ret = bq2597x_get_adc_data(bq, ADC_VBAT, &result);
		if (!ret)
			bq->vbat_volt = result;

		val->intval = bq->vbat_volt;
		break;
	case POWER_SUPPLY_PROP_TI_BATTERY_CURRENT:
		ret = bq2597x_get_adc_data(bq, ADC_IBAT, &result);
		if (!ret)
			bq->ibat_curr = result;

		val->intval = bq->ibat_curr;
		break;
	case POWER_SUPPLY_PROP_TI_BATTERY_TEMPERATURE:
		ret = bq2597x_get_adc_data(bq, ADC_TBAT, &result);
		if (!ret)
			bq->bat_temp = result;

		val->intval = bq->bat_temp;
		break;
	case POWER_SUPPLY_PROP_TI_BUS_VOLTAGE:
		ret = bq2597x_get_adc_data(bq, ADC_VBUS, &result);
		if (!ret)
			bq->vbus_volt = result;

		val->intval = bq->vbus_volt;
		break;
	case POWER_SUPPLY_PROP_TI_BUS_CURRENT:
		ret = bq2597x_get_adc_data(bq, ADC_IBUS, &result);
		if (!ret)
			bq->ibus_curr = result;

		val->intval = bq->ibus_curr;
		break;
	case POWER_SUPPLY_PROP_TI_BUS_TEMPERATURE:
		ret = bq2597x_get_adc_data(bq, ADC_TBUS, &result);
		if (!ret)
			bq->bus_temp = result;

		val->intval = bq->bus_temp;
		break;
	case POWER_SUPPLY_PROP_TI_DIE_TEMPERATURE:
		ret = bq2597x_get_adc_data(bq, ADC_TDIE, &result);
		if (!ret)
			bq->die_temp = result;

		val->intval = bq->die_temp;
		break;
	case POWER_SUPPLY_PROP_TI_ALARM_STATUS:

		bq2597x_check_alarm_status(bq);

		val->intval = ((bq->bat_ovp_alarm << BAT_OVP_ALARM_SHIFT)
			| (bq->bat_ocp_alarm << BAT_OCP_ALARM_SHIFT)
			| (bq->bat_ucp_alarm << BAT_UCP_ALARM_SHIFT)
			| (bq->bus_ovp_alarm << BUS_OVP_ALARM_SHIFT)
			| (bq->bus_ocp_alarm << BUS_OCP_ALARM_SHIFT)
			| (bq->bat_therm_alarm << BAT_THERM_ALARM_SHIFT)
			| (bq->bus_therm_alarm << BUS_THERM_ALARM_SHIFT)
			| (bq->die_therm_alarm << DIE_THERM_ALARM_SHIFT));
		break;

	case POWER_SUPPLY_PROP_TI_FAULT_STATUS:
		bq2597x_check_fault_status(bq);

		val->intval = ((bq->bat_ovp_fault << BAT_OVP_FAULT_SHIFT)
			| (bq->bat_ocp_fault << BAT_OCP_FAULT_SHIFT)
			| (bq->bus_ovp_fault << BUS_OVP_FAULT_SHIFT)
			| (bq->bus_ocp_fault << BUS_OCP_FAULT_SHIFT)
			| (bq->bat_therm_fault << BAT_THERM_FAULT_SHIFT)
			| (bq->bus_therm_fault << BUS_THERM_FAULT_SHIFT)
			| (bq->die_therm_fault << DIE_THERM_FAULT_SHIFT));
		break;

	case POWER_SUPPLY_PROP_TI_REG_STATUS:
		bq2597x_check_reg_status(bq);
		val->intval = (bq->vbat_reg << VBAT_REG_STATUS_SHIFT) |
				(bq->ibat_reg << IBAT_REG_STATUS_SHIFT);
		break;
	case POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_QC3:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		if (bq->mode == BQ25970_ROLE_MASTER)
			val->strval = "bq2597x-master";
		else if (bq->mode == BQ25970_ROLE_SLAVE)
			val->strval = "bq2597x-slave";
		else
			val->strval = "bq2597x-standalone";
		break;
	case POWER_SUPPLY_PROP_TI_BUS_ERROR_STATUS:
		val->intval = bq2597x_check_vbus_error_status(bq);
		break;
	case POWER_SUPPLY_PROP_CHARGING_MODE:
		val->intval = sc8551_get_charge_mode(bq);
		break;
	default:
		return -EINVAL;

	}

	return 0;
}

static int bq2597x_charger_set_property(struct power_supply *psy,
				       enum power_supply_property prop,
				       const union power_supply_propval *val)
{
	struct bq2597x *bq = power_supply_get_drvdata(psy);

	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		bq2597x_enable_charge(bq, val->intval);
		bq2597x_check_charge_enabled(bq, &bq->charge_enabled);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		bq2597x_set_present(bq, !!val->intval);
		break;
	case POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_QC3:
		bq2597x_set_bus_protection(bq, val->intval);
		break;
	case POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_PD:
		bq_info("set bus ovp: %d,ac ovp:%d\n",bq->cfg->bus_ovp_th,bq->cfg->ac_ovp_th);
		bq2597x_set_busovp_th(bq, bq->cfg->bus_ovp_th);
		bq2597x_set_acovp_th(bq, bq->cfg->ac_ovp_th);
		break;
	case POWER_SUPPLY_PROP_CHARGING_MODE:
		sc8551_set_charge_mode(bq, val->intval);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int bq2597x_charger_is_writeable(struct power_supply *psy,
				       enum power_supply_property prop)
{
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
	case POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_QC3:
	case POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_PD:
	case POWER_SUPPLY_PROP_CHARGING_MODE:
		ret = 1;
		break;
	default:
		ret = 0;
		break;
	}
	return ret;
}


static int bq2597x_psy_register(struct bq2597x *bq)
{
	int ret = 0;

	bq->psy_cfg.drv_data = bq;
	bq->psy_cfg.of_node = bq->dev->of_node;

	if (bq->mode == BQ25970_ROLE_MASTER)
		bq->psy_desc.name = "bq2597x-master";
	else if (bq->mode == BQ25970_ROLE_SLAVE)
		bq->psy_desc.name = "bq2597x-slave";
	else
		bq->psy_desc.name = "bq2597x-standalone";

	bq->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	bq->psy_desc.properties = bq2597x_charger_props;
	bq->psy_desc.num_properties = ARRAY_SIZE(bq2597x_charger_props);
	bq->psy_desc.get_property = bq2597x_charger_get_property;
	bq->psy_desc.set_property = bq2597x_charger_set_property;
	bq->psy_desc.property_is_writeable = bq2597x_charger_is_writeable;


	bq->fc2_psy = devm_power_supply_register(bq->dev,
			&bq->psy_desc, &bq->psy_cfg);
	if (IS_ERR(bq->fc2_psy)) {
		bq_err("failed to register fc2_psy:%d\n", ret);
		return PTR_ERR(bq->fc2_psy);
	}

	bq_info("%s power supply register successfully\n", bq->psy_desc.name);

	return 0;
}

static void bq2597x_dump_reg(struct bq2597x *bq)
{

	int ret;
	u8 val;
	u8 addr;

	for (addr = 0x00; addr <= 0x2B; addr++) {
		ret = bq2597x_read_byte(bq, addr, &val);
		if (!ret)
			bq_err("Reg[%02X] = 0x%02X\n", addr, val);
	}

}
EXPORT_SYMBOL_GPL(bq2597x_dump_reg);

static void bq2597x_dump_important_regs(struct bq2597x *bq)
{

	int ret;
	u8 val;
	int result;

	bq2597x_get_adc_data(bq, ADC_VBUS, &result);
	bq_err("dump VBUS = %d\n",result);
	
	ret = bq2597x_read_byte(bq, BQ2597X_REG_0A, &val);
	if (!ret)
		bq_err("dump converter state Reg [%02X] = 0x%02X\n",
				BQ2597X_REG_0A, val);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0D, &val);
	if (!ret)
		bq_err("dump int stat Reg[%02X] = 0x%02X\n",
				BQ2597X_REG_0D, val);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0E, &val);
	if (!ret)
		bq_err("dump int flag Reg[%02X] = 0x%02X\n",
				BQ2597X_REG_0E, val);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_10, &val);
	if (!ret)
		bq_err("dump fault stat Reg[%02X] = 0x%02X\n",
				BQ2597X_REG_10, val);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_11, &val);
	if (!ret)
		bq_err("dump fault flag Reg[%02X] = 0x%02X\n",
				BQ2597X_REG_11, val);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_06, &val);
	if (!ret)
		bq_err("dump fault flag Reg[%02X] = 0x%02X\n",
				BQ2597X_REG_06, val);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_2D, &val);
	if (!ret)
		bq_err("dump regulation flag Reg[%02X] = 0x%02X\n",
				BQ2597X_REG_2D, val);

	ret = bq2597x_read_byte(bq, 0x31, &val);
	if (!ret)
		bq_err("dump regulation flag Reg0x31 = 0x%02X\n",
				 val);
}

static void bq2597x_check_alarm_status(struct bq2597x *bq)
{
	int ret;
	u8 flag = 0;
	u8 stat = 0;

	mutex_lock(&bq->data_lock);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_08, &flag);
	if (!ret && (flag & BQ2597X_IBUS_UCP_FALL_FLAG_MASK))
		bq_info("UCP_FLAG =0x%02X\n",
			!!(flag & BQ2597X_IBUS_UCP_FALL_FLAG_MASK));

	ret = bq2597x_read_byte(bq, BQ2597X_REG_2D, &flag);
	if (!ret && (flag & BQ2597X_VDROP_OVP_FLAG_MASK))
		bq_info("VDROP_OVP_FLAG =0x%02X\n",
			!!(flag & BQ2597X_VDROP_OVP_FLAG_MASK));

	/*read to clear alarm flag*/
	ret = bq2597x_read_byte(bq, BQ2597X_REG_0E, &flag);
	if (!ret && flag)
		bq_info("INT_FLAG =0x%02X\n", flag);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0D, &stat);
	if (!ret && stat != bq->prev_alarm) {
		bq_info("INT_STAT = 0X%02x\n", stat);
		bq->prev_alarm = stat;
		bq->bat_ovp_alarm = !!(stat & BAT_OVP_ALARM);
		bq->bat_ocp_alarm = !!(stat & BAT_OCP_ALARM);
		bq->bus_ovp_alarm = !!(stat & BUS_OVP_ALARM);
		bq->bus_ocp_alarm = !!(stat & BUS_OCP_ALARM);
		bq->batt_present  = !!(stat & VBAT_INSERT);
		bq->vbus_present  = !!(stat & VBUS_INSERT);
		bq->bat_ucp_alarm = !!(stat & BAT_UCP_ALARM);
	}


	ret = bq2597x_read_byte(bq, BQ2597X_REG_08, &stat);
	if (!ret && (stat & 0x50))
		bq_err("Reg[08]BUS_UCPOVP = 0x%02X\n", stat);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0A, &stat);
	if (!ret && (stat & 0x02))
		bq_err("Reg[0A]CONV_OCP = 0x%02X\n", stat);

	mutex_unlock(&bq->data_lock);
}

static void bq2597x_check_fault_status(struct bq2597x *bq)
{
	int ret;
	u8 flag = 0;
	u8 stat = 0;
	bool changed = false;

	mutex_lock(&bq->data_lock);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_10, &stat);
	if (!ret && stat)
		bq_err("FAULT_STAT = 0x%02X\n", stat);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_11, &flag);
	if (!ret && flag)
		bq_err("FAULT_FLAG = 0x%02X\n", flag);

	if (!ret && flag != bq->prev_fault) {
		changed = true;
		bq->prev_fault = flag;
		bq->bat_ovp_fault = !!(flag & BAT_OVP_FAULT);
		bq->bat_ocp_fault = !!(flag & BAT_OCP_FAULT);
		bq->bus_ovp_fault = !!(flag & BUS_OVP_FAULT);
		bq->bus_ocp_fault = !!(flag & BUS_OCP_FAULT);
		bq->bat_therm_fault = !!(flag & TS_BAT_FAULT);
		bq->bus_therm_fault = !!(flag & TS_BUS_FAULT);

		bq->bat_therm_alarm = !!(flag & TBUS_TBAT_ALARM);
		bq->bus_therm_alarm = !!(flag & TBUS_TBAT_ALARM);
	}

	mutex_unlock(&bq->data_lock);
}


static int bq2597x_check_vbus_error_status(struct bq2597x *bq)
{
	int ret;
	u8 stat = 0;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0A, &stat);
	bq_err("BQ2597X_REG_0A:0x%02x\n", stat);
	if (!ret) {
		bq_err("BQ2597X_REG_0A:0x%02x\n", stat);
		if (stat & VBUS_ERROR_LOW_MASK)
			return VBUS_ERROR_LOW;
		else if (stat & VBUS_ERROR_HIGH_MASK)
			return VBUS_ERROR_HIGH;
		else
			return VBUS_ERROR_NONE;
	}

	return VBUS_ERROR_NONE;
}

/*
 * interrupt does nothing, just info event chagne, other module could get info
 * through power supply interface
 */
static irqreturn_t bq2597x_charger_interrupt(int irq, void *dev_id)
{
	struct bq2597x *bq = dev_id;

	bq_info("INT OCCURED\n");

	mutex_lock(&bq->irq_complete);
	bq->irq_waiting = true;
	if (!bq->resume_completed) {
		dev_dbg(bq->dev, "IRQ triggered before device-resume\n");
		if (!bq->irq_disabled) {
			disable_irq_nosync(irq);
			bq->irq_disabled = true;
		}
		bq->irq_waiting = false;
		mutex_unlock(&bq->irq_complete);
		return IRQ_HANDLED;
	}
	bq->irq_waiting = false;
	/* dump some impoartant registers and alarm fault status for debug */
	bq2597x_dump_important_regs(bq);
	bq2597x_check_alarm_status(bq);
	bq2597x_check_fault_status(bq);
	mutex_unlock(&bq->irq_complete);

	/* power_supply_changed(bq->fc2_psy); */

	return IRQ_HANDLED;
}


static void determine_initial_status(struct bq2597x *bq)
{
	if (bq->client->irq)
		bq2597x_charger_interrupt(bq->client->irq, bq);
}

static int show_registers(struct seq_file *m, void *data)
{
	struct bq2597x *bq = m->private;
	u8 addr;
	int ret;
	u8 val;

	for (addr = 0x0; addr <= 0x36; addr++) {
		ret = bq2597x_read_byte(bq, addr, &val);
		if (!ret)
			seq_printf(m, "Reg[%02X] = 0x%02X\n", addr, val);
	}
	return 0;
}


static int reg_debugfs_open(struct inode *inode, struct file *file)
{
	struct bq2597x *bq = inode->i_private;

	return single_open(file, show_registers, bq);
}


static const struct file_operations reg_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= reg_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void create_debugfs_entry(struct bq2597x *bq)
{
	if (bq->mode == BQ25970_ROLE_MASTER)
		bq->debug_root = debugfs_create_dir("bq2597x-master", NULL);
	else if (bq->mode == BQ25970_ROLE_SLAVE)
		bq->debug_root = debugfs_create_dir("bq2597x-slave", NULL);
	else
		bq->debug_root = debugfs_create_dir("bq2597x-standalone", NULL);

	if (!bq->debug_root)
		bq_err("Failed to create debug dir\n");

	if (bq->debug_root) {
		debugfs_create_file("registers",
					S_IFREG | S_IRUGO,
					bq->debug_root, bq, &reg_debugfs_ops);

		debugfs_create_x32("skip_reads",
					S_IFREG | S_IWUSR | S_IRUGO,
					bq->debug_root,
					&(bq->skip_reads));
		debugfs_create_x32("skip_writes",
					S_IFREG | S_IWUSR | S_IRUGO,
					bq->debug_root,
					&(bq->skip_writes));
	}
}
static const struct i2c_device_id bq2597x_charger_id[] = {
	{"bq2597x-standalone", BQ25970_ROLE_STDALONE},
	{"bq2597x-master", BQ25970_ROLE_MASTER},
	{"bq2597x-slave", BQ25970_ROLE_SLAVE},
	{},
};
static struct of_device_id bq2597x_charger_match_table[] = {
	{
		.compatible = "ti,bq2597x-standalone",
		.data = (void *) BQ25970_ROLE_STDALONE
	},
	{
		.compatible = "ti,bq2597x-master",
		.data = (void *) BQ25970_ROLE_MASTER
	},

	{
		.compatible = "ti,bq2597x-slave",
		.data = (void *) BQ25970_ROLE_SLAVE
	},
	{},
};
MODULE_DEVICE_TABLE(of, bq2597x_charger_match_table);


static int bq2597x_get_dev_role(struct i2c_client *client)
{
	const struct of_device_id *of_id;

	of_id = of_match_device(of_match_ptr(bq2597x_charger_match_table), &client->dev);
	if (of_id == NULL) {
		dev_err(&client->dev, "%s: fail to matched of_device_id\n", __func__);
		return -EINVAL;
	}

	dev_info(&client->dev, "%s: matched to %s\n", __func__, of_id->compatible);

	return (uintptr_t)of_id->data;
}

static int bq2597x_charger_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct bq2597x *bq;

	//struct device_node *node = client->dev.of_node;
	int ret;
	pr_err("%s [mi_chg] poussin start\n", __func__);
	bq = devm_kzalloc(&client->dev, sizeof(struct bq2597x), GFP_KERNEL);
	if (!bq)
		return -ENOMEM;
	bq->mode = bq2597x_get_dev_role(client);
	bq->dev = &client->dev;

	bq->client = client;
	i2c_set_clientdata(client, bq);

	mutex_init(&bq->i2c_rw_lock);
	mutex_init(&bq->data_lock);
	mutex_init(&bq->charging_disable_lock);
	mutex_init(&bq->irq_complete);

	bq->resume_completed = true;
	bq->irq_waiting = false;

	ret = bq2597x_detect_device(bq);
	if (ret) {
		bq_err("No bq2597x device found!\n");
		return -ENODEV;
	}

	ret = bq2597x_parse_dt(bq, &client->dev);
	if (ret)
		return -EIO;

	ret = bq2597x_init_device(bq);

	if (ret) {
		bq_err("Failed to init device\n");
		return ret;
	}

	determine_initial_status(bq);

	ret = bq2597x_psy_register(bq);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
				NULL, bq2597x_charger_interrupt,
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"bq2597x charger irq", bq);
		if (ret < 0) {
			bq_err("request irq for irq=%d failed, ret =%d\n",
							client->irq, ret);
			goto err_1;
		}
		/* no need to enable this irq as a wakeup source */
		/* enable_irq_wake(client->irq); */
	}

	device_init_wakeup(bq->dev, 1);
	create_debugfs_entry(bq);

	ret = sysfs_create_group(&bq->dev->kobj, &bq2597x_attr_group);
	if (ret) {
		bq_err("failed to register sysfs. err: %d\n", ret);
		goto err_1;
	}

	/* determine_initial_status(bq); */
	sc8551_set_charge_mode(bq, 0);
	bq_info("bq2597x probe successfully, Part Num:%d\n!",
				bq->part_no);

	return 0;

err_1:
	power_supply_unregister(bq->fc2_psy);
	return ret;
}


static inline bool is_device_suspended(struct bq2597x *bq)
{
	return !bq->resume_completed;
}

static int bq2597x_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2597x *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->irq_complete);
	bq->resume_completed = false;
	mutex_unlock(&bq->irq_complete);
	bq2597x_enable_adc(bq, false);
	bq_err("Suspend successfully!");

	return 0;
}

static int bq2597x_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2597x *bq = i2c_get_clientdata(client);

	if (bq->irq_waiting) {
		pr_err_ratelimited("Aborting suspend, an interrupt was detected while suspending\n");
		return -EBUSY;
	}
	return 0;
}

static int bq2597x_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2597x *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->irq_complete);
	bq->resume_completed = true;
	if (bq->irq_waiting) {
		bq->irq_disabled = false;
		enable_irq(client->irq);
		mutex_unlock(&bq->irq_complete);
		//bq2597x_charger_interrupt(client->irq, bq);
	} else {
		mutex_unlock(&bq->irq_complete);
	}
	bq2597x_enable_adc(bq, true);
	power_supply_changed(bq->fc2_psy);
	bq_err("Resume successfully!");

	return 0;
}
static int bq2597x_charger_remove(struct i2c_client *client)
{
	struct bq2597x *bq = i2c_get_clientdata(client);

	bq2597x_enable_adc(bq, false);

	power_supply_unregister(bq->fc2_psy);

	mutex_destroy(&bq->charging_disable_lock);
	mutex_destroy(&bq->data_lock);
	mutex_destroy(&bq->i2c_rw_lock);
	mutex_destroy(&bq->irq_complete);

	debugfs_remove_recursive(bq->debug_root);

	sysfs_remove_group(&bq->dev->kobj, &bq2597x_attr_group);

	return 0;
}


static void bq2597x_charger_shutdown(struct i2c_client *client)
{
	struct bq2597x *bq = i2c_get_clientdata(client);

	bq2597x_enable_adc(bq, false);
}

static const struct dev_pm_ops bq2597x_pm_ops = {
	.resume		= bq2597x_resume,
	.suspend_noirq = bq2597x_suspend_noirq,
	.suspend	= bq2597x_suspend,
};



static struct i2c_driver bq2597x_charger_driver = {
	.driver		= {
		.name	= "bq2597x-charger",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(bq2597x_charger_match_table),
		.pm	= &bq2597x_pm_ops,
	},
	.id_table	= bq2597x_charger_id,

	.probe		= bq2597x_charger_probe,
	.remove		= bq2597x_charger_remove,
	.shutdown	= bq2597x_charger_shutdown,
};

module_i2c_driver(bq2597x_charger_driver);

MODULE_DESCRIPTION("TI BQ2597x Charger Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Texas Instruments");

