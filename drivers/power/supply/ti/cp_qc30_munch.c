/*
 * State machine for qc3 when it works on cp
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#define pr_fmt(fmt)	"[FC2-PM]: %s: " fmt, __func__
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/pmic-voter.h>

#include "cp_qc30_munch.h"

#ifdef pr_debug
#undef pr_debug
#define pr_debug pr_err
#endif

#define BATT_MAX_CHG_VOLT		4400
#define BATT_FAST_CHG_CURR		5400
#define	BUS_OVP_THRESHOLD		12000
#define	BUS_OVP_ALARM_THRESHOLD		9500

#define BUS_VOLT_INIT_UP		400

#define BAT_VOLT_LOOP_LMT		BATT_MAX_CHG_VOLT
#define BAT_CURR_LOOP_LMT		BATT_FAST_CHG_CURR
#define BUS_VOLT_LOOP_LMT		BUS_OVP_THRESHOLD

#define VOLT_UP		true
#define VOLT_DOWN	false

#define ADC_ERR			1
#define CP_ENABLE_FAIL			2
#define TAPER_DONE			1

enum {
	VBUS_ERROR_NONE,
	VBUS_ERROR_LOW,
	VBUS_ERROR_HIGH,
};

static struct sys_config sys_config = {
	.bat_volt_lp_lmt		= BAT_VOLT_LOOP_LMT,
	.bat_curr_lp_lmt		= BAT_CURR_LOOP_LMT/* + 1000*/,
	.bus_volt_lp_lmt		= BUS_VOLT_LOOP_LMT,
	.bus_curr_lp_lmt		= BAT_CURR_LOOP_LMT >> 1,

	.ibus_minus_deviation_val = HVDCP3_IBUS_MINUS_DEV_VAL,
	.ibus_plus_deviation_val = HVDCP3_IBUS_PLUS_DEV_VAL,
	.ibat_minus_deviation_val = HVDCP3_IBAT_MINUS_DEV_VAL,
	.ibat_plus_deviation_val = HVDCP3_IBAT_PLUS_DEV_VAL,

	.fc2_taper_current		= 2200,
	.flash2_policy.down_steps	= -1,
	.flash2_policy.volt_hysteresis	= 50,

	.min_vbat_start_flash2		= 3500,
	.cp_sec_enable			= false,
	.qc3p5_supported		= false,
};

struct cp_qc30_data {
	struct device *dev;
	int			bat_volt_max;
	int			bat_curr_max;
	int			bus_volt_max;
	int			bus_curr_max;
	int			battery_warm_th;
	bool			cp_sec_enable;
	bool			use_qcom_gauge;
	bool			qc3p5_supported;

	/* notifiers */
	struct notifier_block	nb;
};

static pm_t pm_state;

static int fc2_taper_timer;
static int ibus_lmt_change_timer;
static int chg_enable_k81;

static struct power_supply *cp_get_sw_psy(void)
{

	if (!pm_state.sw_psy)
		pm_state.sw_psy = power_supply_get_by_name("battery");

	return pm_state.sw_psy;
}

static struct power_supply *cp_get_usb_psy(void)
{

	if (!pm_state.usb_psy)
		pm_state.usb_psy = power_supply_get_by_name("usb");

	return pm_state.usb_psy;
}

static struct power_supply *cp_get_bms_psy(void)
{
	if (!pm_state.bms_psy)
		pm_state.bms_psy = power_supply_get_by_name("bms");

	return pm_state.bms_psy;
}
static struct power_supply *cp_get_wireless_psy(void)
{
	if (!pm_state.wireless_psy)
		pm_state.wireless_psy = power_supply_get_by_name("wireless");

	return pm_state.wireless_psy;
}

static int cp_get_effective_fcc_val(pm_t pm_state)
{
	int effective_fcc_val = 0;

	if (!pm_state.fcc_votable)
		pm_state.fcc_votable = find_votable("FCC");

	if (!pm_state.fcc_votable)
		return -EINVAL;

	effective_fcc_val = get_effective_result(pm_state.fcc_votable);
	effective_fcc_val = effective_fcc_val / 1000;
	pr_info("effective_fcc_val: %d\n", effective_fcc_val);
	return effective_fcc_val;
}

static struct power_supply *cp_get_fc_psy(void)
{
	if (!pm_state.fc_psy) {
		if (sys_config.cp_sec_enable)
			pm_state.fc_psy = power_supply_get_by_name("bq2597x-master");
		else
			pm_state.fc_psy = power_supply_get_by_name("bq2597x-standalone");
	}

	return pm_state.fc_psy;
}

static void cp_update_bms_ibat(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_bms_psy();
	if (!psy)
		return;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (!ret)
		pm_state.ibat_now = -(val.intval / 1000);
    
    ret = power_supply_get_property(psy,
		    POWER_SUPPLY_PROP_I2C_ERROR_COUNT, &val);
	if (!ret) {
		pm_state.bms_i2c_error_count = val.intval;	
	}

}

/* set bq27z561 fastcharge mode to enable or disabled */
static int qc3_set_bms_fastcharge_mode(bool enable)
{
	union power_supply_propval pval = {0,};
	int rc;

	cp_get_usb_psy();

	if (!pm_state.usb_psy)
		return 0;

	pval.intval = enable;

	rc = power_supply_set_property(pm_state.usb_psy,
				POWER_SUPPLY_PROP_FASTCHARGE_MODE, &pval);
	if (rc < 0) {
		pr_info("Couldn't write fastcharge mode:%d\n", rc);
		return rc;
	}

	pm_state.bms_fastcharge_mode = enable;

	return 0;
}

/* get thermal level from battery power supply property */
static int qc3_get_batt_current_thermal_level(int *level)
{
	int ret, rc = 0;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return 0;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, &val);

	if (rc < 0) {
		pr_info("Couldn't get fastcharge mode:%d\n", rc);
		return rc;
	}

	pr_info("val.intval: %d\n", val.intval);
    pm_state.thermal_l = val.intval;
	*level = val.intval;
	return rc;
}

/* determine whether to disable cp according to jeita status */
static bool qc3_disable_cp_by_jeita_status(void)
{
	int batt_temp = 0, bq_input_suspend = 0;
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return false;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
	if (!ret)
		bq_input_suspend = !!val.intval;

	psy = cp_get_bms_psy();
	if (!psy)
		return false;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_TEMP, &val);

	if (ret < 0) {
		pr_info("Couldn't get batt temp prop:%d\n", ret);
		return false;
	}

	batt_temp = val.intval;
	pm_state.bms_temp = val.intval;
	pr_info("batt_temp: %d\n", batt_temp);

	if (bq_input_suspend) {
		return true;
	} else {
		if (batt_temp >= pm_state.warm_threshold_temp && !pm_state.jeita_triggered) {
			pm_state.jeita_triggered = true;
			return true;
		} else if (batt_temp <= JEITA_COOL_NOT_ALLOW_CP_THR
				&& !pm_state.jeita_triggered) {
			pm_state.jeita_triggered = true;
			return true;
		} else if ((batt_temp <= (pm_state.warm_threshold_temp - JEITA_HYSTERESIS))
					&& (batt_temp >= (JEITA_COOL_NOT_ALLOW_CP_THR + JEITA_HYSTERESIS))
				&& pm_state.jeita_triggered) {
			pm_state.jeita_triggered = false;
			return false;
		} else {
			return pm_state.jeita_triggered;
		}
	}
}

static void cp_update_sw_status(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);
	if (!ret)
		pm_state.sw_chager.charge_enabled = val.intval;
}

static int qc3_check_night_charging_enabled(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return false;

	ret = power_supply_get_property(psy,
		POWER_SUPPLY_PROP_BATTERY_INPUT_SUSPEND, &val);
	if (!ret)
		pm_state.night_charging = !!val.intval;

	return ret;
}

static void cp_get_batt_capacity(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_CAPACITY, &val);
	if (!ret)
		pm_state.capacity = val.intval;
	pr_info("capacity %d\n", pm_state.capacity);
}

static void cp_update_fc_status(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_fc_psy();
	if (!psy)
		return;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BATTERY_VOLTAGE, &val);
	if (!ret)
		pm_state.bq2597x.vbat_volt = val.intval;

	/*ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BATTERY_CURRENT, &val);
	if (!ret)
		pm_state.bq2597x.ibat_curr = val.intval; */

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BUS_VOLTAGE, &val);
	if (!ret)
		pm_state.bq2597x.vbus_volt = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BUS_CURRENT, &val);
	if (!ret)
		pm_state.bq2597x.ibus_curr = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BUS_TEMPERATURE, &val);
	if (!ret)
		pm_state.bq2597x.bus_temp = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BATTERY_TEMPERATURE, &val);
	if (!ret)
		pm_state.bq2597x.bat_temp = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_DIE_TEMPERATURE, &val);
	if (!ret)
		pm_state.bq2597x.die_temp = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BATTERY_PRESENT, &val);
	if (!ret)
		pm_state.bq2597x.batt_pres = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_VBUS_PRESENT, &val);
	if (!ret)
		pm_state.bq2597x.vbus_pres = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_BUS_ERROR_STATUS, &val);
	if (!ret)
		pm_state.bq2597x.bus_error_status = val.intval;

	if (pm_state.bq2597x.vbus_pres == 1) {
		cp_update_bms_ibat();
		pm_state.bq2597x.ibat_curr = pm_state.ibat_now;
	}

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret)
		pm_state.bq2597x.charge_enabled = val.intval;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_ALARM_STATUS, &val);
	if (!ret) {
		pm_state.bq2597x.bat_ovp_alarm = !!(val.intval & BAT_OVP_ALARM_MASK);
		pm_state.bq2597x.bat_ocp_alarm = !!(val.intval & BAT_OCP_ALARM_MASK);
		pm_state.bq2597x.bus_ovp_alarm = !!(val.intval & BUS_OVP_ALARM_MASK);
		pm_state.bq2597x.bus_ocp_alarm = !!(val.intval & BUS_OCP_ALARM_MASK);
		pm_state.bq2597x.bat_ucp_alarm = !!(val.intval & BAT_UCP_ALARM_MASK);
		pm_state.bq2597x.bat_therm_alarm = !!(val.intval & BAT_THERM_ALARM_MASK);
		pm_state.bq2597x.bus_therm_alarm = !!(val.intval & BUS_THERM_ALARM_MASK);
		pm_state.bq2597x.die_therm_alarm = !!(val.intval & DIE_THERM_ALARM_MASK);
	}

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_FAULT_STATUS, &val);
	if (!ret) {
		pm_state.bq2597x.bat_ovp_fault = !!(val.intval & BAT_OVP_FAULT_MASK);
		pm_state.bq2597x.bat_ocp_fault = !!(val.intval & BAT_OCP_FAULT_MASK);
		pm_state.bq2597x.bus_ovp_fault = !!(val.intval & BUS_OVP_FAULT_MASK);
		pm_state.bq2597x.bus_ocp_fault = !!(val.intval & BUS_OCP_FAULT_MASK);
		pm_state.bq2597x.bat_therm_fault = !!(val.intval & BAT_THERM_FAULT_MASK);
		pm_state.bq2597x.bus_therm_fault = !!(val.intval & BUS_THERM_FAULT_MASK);
		pm_state.bq2597x.die_therm_fault = !!(val.intval & DIE_THERM_FAULT_MASK);
	}

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TI_REG_STATUS, &val);
	if (!ret) {
		pm_state.bq2597x.vbat_reg = !!(val.intval & VBAT_REG_STATUS_MASK);
		pm_state.bq2597x.ibat_reg = !!(val.intval & IBAT_REG_STATUS_MASK);
	}
}


static int cp_enable_fc(bool enable)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_fc_psy();
	if (!psy)
		return -ENODEV;

	val.intval = enable;
	ret = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);

	return ret;
}

static int cp_set_qc_bus_protections(int hvdcp3_type)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_fc_psy();
	if (!psy)
		return -ENODEV;

	val.intval = hvdcp3_type;
	ret = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_TI_SET_BUS_PROTECTION_FOR_QC3, &val);

	return ret;
}

static int cp_enable_sw(bool enable)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return -ENODEV;

	val.intval = enable;
	ret = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);

	return ret;
}

static int cp_check_fc_enabled(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_fc_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret)
		pm_state.bq2597x.charge_enabled = !!val.intval;

	pr_info("pm_state.bq2597x.charge_enabled: %d\n",
			pm_state.bq2597x.charge_enabled);
	return ret;
}

static int cp_check_sw_enabled(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);
	if (!ret)
		pm_state.sw_chager.charge_enabled = !!val.intval;

	pr_info("pm_state.sw_chager.charge_enabled: %d\n",
			pm_state.sw_chager.charge_enabled);
	return ret;
}

static int cp_tune_vbus_volt(bool up)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return -ENODEV;

	if (up)
		val.intval = POWER_SUPPLY_DP_DM_DP_PULSE;
	else
		val.intval = POWER_SUPPLY_DP_DM_DM_PULSE;

	ret = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_DP_DM_BQ, &val);

	pr_info("tune adapter voltage %s %s\n", up ? "up" : "down",
			ret ? "fail" : "successfully");

	return ret;

}

static int cp_set_fake_hvdcp3(bool enable)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_usb_psy();
	if (!psy)
		return -ENODEV;

	val.intval = enable;

	if (enable)
		pr_err("cp_set_fake_hvdcp3:set fake_hvdcp3.\n");

	ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_FAKE_HVDCP3, &val);

	return ret;
}

static int cp_get_qc_pulse_cnt(void)
{
	int ret = 0, cnt = 0;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_DP_DM_BQ, &val);
	if (!ret)
		cnt = val.intval;

	pr_info("pulse_cnt:%d\n", cnt);
	return cnt;
}

static int cp_reset_vbus_volt(void)
{
	int ret = 0;
	int qc3p5_reset_vbus_retry = 0;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
		cp_update_fc_status();
		pr_err("vbat=%d,vbus=%d\n", pm_state.bq2597x.vbat_volt, pm_state.bq2597x.vbus_volt);
		while (pm_state.bq2597x.vbus_volt > 6550) {
			cp_tune_vbus_volt(VOLT_DOWN);
			cp_update_fc_status();
			qc3p5_reset_vbus_retry++;
			usleep_range(10000, 10010);
			pr_err("qc3p5_reset_vbus_retry=%d,vbus=%d\n", qc3p5_reset_vbus_retry, pm_state.bq2597x.vbus_volt);
			if (qc3p5_reset_vbus_retry > 300) {
				pr_err("Failed to reset qc3.5 adapter volt to 6.55v or less\n");
				break;
			}
		}
	} else {
		psy = cp_get_sw_psy();
		if (!psy)
			return -ENODEV;

		val.intval = POWER_SUPPLY_DP_DM_FORCE_5V;

		ret = power_supply_set_property(psy,
				POWER_SUPPLY_PROP_DP_DM_BQ, &val);

		pr_err("reset vbus volt %s\n", ret ? "fail" : "successfully");
	}

	return ret;

}

static int cp_notifier_vbus_raise_done(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return -ENODEV;

	val.intval = POWER_SUPPLY_DP_DM_RAISE_VOLT_DONE;

	ret = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_DP_DM_BQ, &val);

	pr_err("raise vbus done\n");

	return ret;
}

static int cp_notifier_vbus_raise_start(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_sw_psy();
	if (!psy)
		return -ENODEV;

	val.intval = POWER_SUPPLY_DP_DM_RAISE_VOLT_START;

	ret = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_DP_DM_BQ, &val);

	pr_err("raise vbus start\n");

	return ret;

}

static int cp_get_wireless_reverse_mode(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_wireless_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_REVERSE_CHG_MODE, &val);
	if (!ret)
		pm_state.reverse_mode = val.intval;

	return ret;
}
static int cp_get_usb_type(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_usb_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_REAL_TYPE, &val);
	if (!ret)
		pm_state.usb_type = val.intval;

	return ret;
}

static int cp_get_usb_present(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_usb_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_PRESENT, &val);
	if (!ret)
		pm_state.usb_present = val.intval;

	return ret;
}

static int cp_get_qc_hvdcp3_type(void)
{
	int ret;
	struct power_supply *psy;
	union power_supply_propval val = {0,};

	psy = cp_get_usb_psy();
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy,
			POWER_SUPPLY_PROP_HVDCP3_TYPE, &val);
	if (!ret)
		pm_state.hvdcp3_type = val.intval;
	pr_info("hvdcp3 type %d\n", pm_state.hvdcp3_type);
	return ret;
}


#define TAPER_TIMEOUT	10
#define IBUS_CHANGE_TIMEOUT  5
static int cp_flash2_charge(unsigned int port)
{
	static int ibus_limit, ibat_limit;
	int thermal_level = 0;
	int rc = -1;
	uint16_t effective_fcc_val = cp_get_effective_fcc_val(pm_state);
	uint16_t effective_ibus_val = cp_get_effective_fcc_val(pm_state)/2;
	union power_supply_propval pval = {0, };

	if(pm_state.sw_psy) {
		rc = power_supply_get_property(pm_state.sw_psy,
					POWER_SUPPLY_PROP_SMART_BATT, &pval);
		if (rc < 0) {
			pr_err("Get samrt batt failed, rc = %d\n", rc);
		}
	}

	qc3_get_batt_current_thermal_level(&thermal_level);
	qc3_check_night_charging_enabled();

	if (ibus_limit == 0)
		ibus_limit = pm_state.ibus_lmt_curr;

	ibus_limit = min(effective_ibus_val, pm_state.ibus_lmt_curr);
	pm_state.ibus_limits = ibus_limit;
	pm_state.effective_ibus = effective_ibus_val;
	pr_info("ibus_limit: %d,smart_batt:%d\n", ibus_limit,pval.intval);

	pr_info("vbus=%d, ibus=%d, vbat=%d, ibat=%d, ibus_target_val=%d\n",
				pm_state.bq2597x.vbus_volt,
				pm_state.bq2597x.ibus_curr,
				pm_state.bq2597x.vbat_volt,
				pm_state.bq2597x.ibat_curr,
				effective_ibus_val);

	pm_state.is_temp_out_fc2_range = qc3_disable_cp_by_jeita_status();
	pr_info("is_temp_out_fc2_range:%d\n", pm_state.is_temp_out_fc2_range);

	pr_info("bq2597x.bus_ocp_alarm: %d\n", pm_state.bq2597x.bus_ocp_alarm);

	ibat_limit = min(effective_fcc_val, sys_config.bat_curr_lp_lmt);

	if (!pm_state.batt_cell_volt_triggered) {
		cp_get_batt_capacity();
		if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
			if (pm_state.bq2597x.vbat_volt >= 4250 || pm_state.capacity > 40) {
				sys_config.ibus_minus_deviation_val = HVDCP3P5_IBUS_MINUS_DEV_VAL;
				sys_config.ibus_plus_deviation_val = HVDCP3P5_IBUS_PLUS_DEV_VAL;
				sys_config.ibat_minus_deviation_val = HVDCP3P5_IBAT_MINUS_DEV_VAL + 100;
				sys_config.ibat_plus_deviation_val = HVDCP3P5_IBAT_PLUS_DEV_VAL - 50;
				pm_state.batt_cell_volt_triggered = true;
				pr_info("for qc3.5, cell_vbat > 4250mv or soc > 40%, modify bq adjust params\n");
			}
		} else if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
			if (pm_state.bq2597x.vbat_volt >= 4200 || pm_state.capacity > 29) {
				sys_config.ibus_minus_deviation_val = HVDCP3_IBUS_MINUS_DEV_VAL;
				sys_config.ibus_plus_deviation_val = HVDCP3_IBUS_PLUS_DEV_VAL;
				sys_config.ibat_minus_deviation_val = HVDCP3_IBAT_MINUS_DEV_VAL + 450;
				sys_config.ibat_plus_deviation_val = HVDCP3_IBAT_PLUS_DEV_VAL - 450;
				pm_state.batt_cell_volt_triggered = true;
				pr_info("for qc3.0, cell_vbat > 4200mv or soc > 29%, modify bq adjust params\n");
			}
		 }
	}

	pr_info("target: t_vbus=%d, t_ibus=%d(m:%d, p:%d), t_vbat=%d, t_ibat=%d(m:%d, p:%d)\n",
			9500,
			ibus_limit, sys_config.ibus_minus_deviation_val, sys_config.ibus_plus_deviation_val,
			sys_config.bat_volt_lp_lmt,
			ibat_limit, sys_config.ibat_minus_deviation_val, sys_config.ibat_plus_deviation_val);

	if (pm_state.bq2597x.vbus_volt < 10000
		&& pm_state.bq2597x.ibus_curr < ibus_limit
		&& !pm_state.bq2597x.bus_ocp_alarm
		&& !pm_state.bq2597x.bus_ovp_alarm
		&& pm_state.bq2597x.vbat_volt < sys_config.bat_volt_lp_lmt - 50 - pval.intval
		&& pm_state.bq2597x.ibat_curr < sys_config.bat_curr_lp_lmt
		&& pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5)

		cp_tune_vbus_volt(VOLT_UP);

	if (pm_state.bq2597x.vbus_volt < 10000
		&& pm_state.bq2597x.ibus_curr < ibus_limit - 300
		&& !pm_state.bq2597x.bus_ocp_alarm
		&& !pm_state.bq2597x.bus_ovp_alarm
		&& pm_state.bq2597x.vbat_volt < sys_config.bat_volt_lp_lmt - 50 - pval.intval
		&& pm_state.bq2597x.ibat_curr < sys_config.bat_curr_lp_lmt - 600)

		cp_tune_vbus_volt(VOLT_UP);

	if (pm_state.bq2597x.bus_ocp_alarm
		|| pm_state.bq2597x.bus_ovp_alarm
		|| pm_state.bq2597x.vbat_reg
		|| pm_state.bq2597x.vbat_volt > sys_config.bat_volt_lp_lmt - pval.intval
		|| pm_state.bq2597x.ibat_curr > sys_config.bat_curr_lp_lmt + 500
		|| pm_state.bq2597x.ibus_curr > ibus_limit + 310) {

		cp_tune_vbus_volt(VOLT_DOWN);
	}

	cp_check_fc_enabled();

	/* battery overheat, stop charge */
	if (pm_state.bq2597x.bat_therm_fault)
		return -ADC_ERR;
	else if (!pm_state.bq2597x.charge_enabled)
		return -CP_ENABLE_FAIL;
	else if (pm_state.bq2597x.bus_ocp_fault
			|| pm_state.bq2597x.bat_ovp_fault
			|| pm_state.bq2597x.bus_ovp_fault)
		return -ADC_ERR;
	else if (pm_state.night_charging) {
		pr_info("night charging feature is enabled!\n");
		return CP_ENABLE_FAIL; 
	} else if (thermal_level >= MAX_THERMAL_LEVEL
			|| pm_state.is_temp_out_fc2_range ||pm_state.bms_i2c_error_count) {
		pr_info("thermal level too high or batt temp is out of fc2 range or bms i2c error!\n");
		return CP_ENABLE_FAIL;
	}

	if (pm_state.bq2597x.vbat_volt > sys_config.bat_volt_lp_lmt - 100 - pval.intval &&
			pm_state.bq2597x.ibat_curr < sys_config.fc2_taper_current) {
		if (fc2_taper_timer++ > TAPER_TIMEOUT) {
			fc2_taper_timer = 0;
			return TAPER_DONE;
		}
	} else {
		fc2_taper_timer = 0;
	}

	return 0;
}

const unsigned char *pm_state_str[] = {
	"CP_STATE_ENTRY",
	"CP_STATE_DISCONNECT",
	"CP_STATE_SW_ENTRY",
	"CP_STATE_SW_ENTRY_2",
//	"CP_STATE_SW_ENTRY_3",
	"CP_STATE_SW_LOOP",
	"CP_STATE_FLASH2_ENTRY",
	"CP_STATE_FLASH2_ENTRY_1",
//	"CP_STATE_FLASH2_ENTRY_2",
	"CP_STATE_FLASH2_ENTRY_3",
//	"CP_STATE_FLASH2_GET_PPS_STATUS",
	"CP_STATE_FLASH2_TUNE",
	"CP_STATE_FLASH2_DELAY",
	"CP_STATE_STOP_CHARGE",
};

static void cp_move_state(pm_sm_state_t state)
{
#if 1
	pr_info("pm_state change:%s -> %s\n",
		pm_state_str[pm_state.state], pm_state_str[state]);
	pm_state.state_log[pm_state.log_idx] = pm_state.state;
	pm_state.log_idx++;
	pm_state.log_idx %= PM_STATE_LOG_MAX;
#endif
	pm_state.state = state;
}

void cp_statemachine(unsigned int port)
{
	int ret;
	static int tune_vbus_retry, tune_vbus_count, retry_enable_bq_count;
	int thermal_level = 0;
	static bool recovery;
	static int cp_enable_fail_count;
	union power_supply_propval pval = {0, };
	int rc = -1;

	if (!pm_state.bq2597x.vbus_pres) {
		pm_state.state = CP_STATE_DISCONNECT;
		recovery = true;
		cp_set_fake_hvdcp3(false);
		pr_info("vbus disconnected\n");
	} else if (pm_state.state == CP_STATE_DISCONNECT && pm_state.usb_type != 0) {
		pr_err("vbus connected\n");
		recovery = true;
		pm_state.jeita_triggered = false;
		pm_state.is_temp_out_fc2_range = false;
		cp_set_fake_hvdcp3(false);
		cp_move_state(CP_STATE_ENTRY);
	}

	if(pm_state.sw_psy) {
		rc = power_supply_get_property(pm_state.sw_psy,
					POWER_SUPPLY_PROP_SMART_BATT, &pval);
		if (rc < 0) {
			pr_err("Get samrt batt failed, rc = %d\n", rc);
		}
		pr_info("smart_batt:%d\n",pval.intval);
	}
	switch (pm_state.state) {
	case CP_STATE_DISCONNECT:
		if (pm_state.bq2597x.charge_enabled) {
			cp_enable_fc(false);
			cp_check_fc_enabled();
		}

		if (!pm_state.sw_chager.charge_enabled) {
			cp_reset_vbus_volt();
			cp_enable_sw(true);
			cp_check_sw_enabled();
		}

		if (pm_state.bms_fastcharge_mode)
			qc3_set_bms_fastcharge_mode(false);

		tune_vbus_count = 0;
		retry_enable_bq_count = 0;
		cp_enable_fail_count = 0;
		pm_state.usb_type = 0;
		pm_state.sw_fc2_init_fail = false;
		pm_state.sw_near_cv = false;
		sys_config.bat_curr_lp_lmt = HVDCP3_CLASS_A_BAT_CURRENT_MA;
		sys_config.bus_curr_lp_lmt = HVDCP3_CLASS_A_BUS_CURRENT_MA;
		pm_state.ibus_lmt_curr = HVDCP3_CLASS_A_BUS_CURRENT_MA;
		sys_config.ibus_minus_deviation_val = HVDCP3_IBUS_MINUS_DEV_VAL;
		sys_config.ibus_plus_deviation_val = HVDCP3_IBUS_PLUS_DEV_VAL;
		sys_config.ibat_minus_deviation_val = HVDCP3_IBAT_MINUS_DEV_VAL;
		sys_config.ibat_plus_deviation_val = HVDCP3_IBAT_PLUS_DEV_VAL;
		pm_state.batt_cell_volt_triggered = false;
		cp_set_qc_bus_protections(HVDCP3_NONE);
		break;

	case CP_STATE_ENTRY:
		cp_get_usb_type();
		qc3_get_batt_current_thermal_level(&thermal_level);
		qc3_check_night_charging_enabled();
		pm_state.is_temp_out_fc2_range = qc3_disable_cp_by_jeita_status();
		pr_info("is_temp_out_fc2_range:%d\n", pm_state.is_temp_out_fc2_range);
		cp_get_batt_capacity();
		if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
				|| pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
			pr_err("vbus_volt:%d\n", pm_state.bq2597x.vbus_volt);
			cp_reset_vbus_volt();
			msleep(100);
			if (thermal_level >= MAX_THERMAL_LEVEL
					||pm_state.night_charging|| pm_state.is_temp_out_fc2_range) {
				cp_move_state(CP_STATE_SW_ENTRY);
				pr_info("thermal too high or batt temp out of range or slowly charging, waiting...\n");
			} else if (pm_state.bq2597x.vbat_volt < sys_config.min_vbat_start_flash2)
				cp_move_state(CP_STATE_SW_ENTRY);
			else if (pm_state.bq2597x.vbat_volt > sys_config.bat_volt_lp_lmt - 100 - pval.intval
					|| pm_state.capacity >= HIGH_CAPACITY_TRH) {
				pm_state.sw_near_cv = true;
				cp_move_state(CP_STATE_SW_ENTRY);
			} else {
				cp_move_state(CP_STATE_FLASH2_ENTRY);
			}
		}
		break;

	case CP_STATE_SW_ENTRY:
		cp_reset_vbus_volt();
		if (pm_state.bq2597x.charge_enabled) {
			cp_enable_fc(false);
			cp_check_fc_enabled();
		}

		if (!pm_state.bq2597x.charge_enabled)
			cp_move_state(CP_STATE_SW_ENTRY_2);
		break;

	case CP_STATE_SW_ENTRY_2:
		pr_err("enable sw charger and check enable\n");
		cp_enable_sw(true);
		cp_check_sw_enabled();
		if (pm_state.sw_chager.charge_enabled)
			cp_move_state(CP_STATE_SW_LOOP);
		break;

	case CP_STATE_SW_LOOP:
		qc3_get_batt_current_thermal_level(&thermal_level);
		if ((tune_vbus_count >= 2) || retry_enable_bq_count >= 5) {
			pr_info("unsupport qc3 or retry enable bq failed in 5 times\n");
			break;
		}

		if (cp_enable_fail_count > 3) {
			pr_info("cp closed 3 times\n");
			break;
		}
		pm_state.is_temp_out_fc2_range = qc3_disable_cp_by_jeita_status();
		qc3_check_night_charging_enabled();
		if (thermal_level < MAX_THERMAL_LEVEL && !pm_state.night_charging && !pm_state.is_temp_out_fc2_range && recovery) {
			pr_info("thermal or batt temp recovery...\n");
			recovery = false;
		} else {
			pr_info("thermal(%d) too high or batt temp out of range\n", thermal_level);
		}
		cp_get_batt_capacity();
		if (pm_state.bq2597x.vbat_volt > sys_config.bat_volt_lp_lmt - 100 - pval.intval
					|| pm_state.capacity >= HIGH_CAPACITY_TRH) {
				pm_state.sw_near_cv = true;
		} else {
			pr_info("capacity(%d) too high or vbat_volt(%d) too low.\n",
					pm_state.capacity, pm_state.bq2597x.vbat_volt);
		}
		if (!pm_state.sw_near_cv && !recovery) {
			if (pm_state.bq2597x.vbat_volt > sys_config.min_vbat_start_flash2) {
				pr_err("battery volt: %d is ok, proceeding to flash charging...\n",
					pm_state.bq2597x.vbat_volt);
				cp_move_state(CP_STATE_FLASH2_ENTRY);
			}
		}
		break;

	case CP_STATE_FLASH2_ENTRY:
		if (pm_state.sw_chager.charge_enabled) {
			cp_notifier_vbus_raise_start();
			cp_enable_sw(false);
			cp_check_sw_enabled();
		}

		if (!pm_state.sw_chager.charge_enabled) {
			cp_move_state(CP_STATE_FLASH2_ENTRY_1);
			tune_vbus_retry = 0;
		}

		cp_get_qc_hvdcp3_type();
		if (pm_state.hvdcp3_type == HVDCP3_CLASSB_27W) {				// QC3.0 27W
			sys_config.bat_curr_lp_lmt = HVDCP3_CLASS_B_BAT_CURRENT_MA;
			sys_config.bus_curr_lp_lmt = HVDCP3_CLASS_B_BUS_CURRENT_MA;
			pm_state.ibus_lmt_curr = sys_config.bus_curr_lp_lmt;
			cp_set_qc_bus_protections(HVDCP3_CLASSB_27W);
		} else if (pm_state.hvdcp3_type == HVDCP3_CLASSA_18W) {			// QC3.0 18W
			sys_config.bat_curr_lp_lmt = HVDCP3_CLASS_A_BAT_CURRENT_MA;
			sys_config.bus_curr_lp_lmt = HVDCP3_CLASS_A_BUS_CURRENT_MA;
			pm_state.ibus_lmt_curr = sys_config.bus_curr_lp_lmt;
			cp_set_qc_bus_protections(HVDCP3_CLASSA_18W);
		} else if (pm_state.hvdcp3_type == HVDCP3P5_CLASSB_27W) {		// QC3.5 27W
			sys_config.ibus_minus_deviation_val = HVDCP3P5_IBUS_MINUS_DEV_VAL;
			sys_config.ibus_plus_deviation_val = HVDCP3P5_IBUS_PLUS_DEV_VAL;
			sys_config.ibat_minus_deviation_val = HVDCP3P5_IBAT_MINUS_DEV_VAL;
			sys_config.ibat_plus_deviation_val = HVDCP3P5_IBAT_PLUS_DEV_VAL;
			sys_config.bat_curr_lp_lmt = HVDCP3P5_CLASS_B_BAT_CURRENT_MA;
			sys_config.bus_curr_lp_lmt = HVDCP3P5_CLASS_B_BUS_CURRENT_MA;
			pm_state.ibus_lmt_curr = sys_config.bus_curr_lp_lmt;
			cp_set_qc_bus_protections(HVDCP3P5_CLASSB_27W);
		} else if (pm_state.hvdcp3_type == HVDCP3P5_CLASSA_18W) {		// QC3.5 18W
			sys_config.ibus_minus_deviation_val = HVDCP3P5_IBUS_MINUS_DEV_VAL;
			sys_config.ibus_plus_deviation_val = HVDCP3P5_IBUS_PLUS_DEV_VAL;
			sys_config.ibat_minus_deviation_val = HVDCP3P5_IBAT_MINUS_DEV_VAL;
			sys_config.ibat_plus_deviation_val = HVDCP3P5_IBAT_PLUS_DEV_VAL;
			sys_config.bat_curr_lp_lmt = HVDCP3P5_CLASS_A_BAT_CURRENT_MA;
			sys_config.bus_curr_lp_lmt = HVDCP3P5_CLASS_A_BUS_CURRENT_MA;
			pm_state.ibus_lmt_curr = sys_config.bus_curr_lp_lmt;
			cp_set_qc_bus_protections(HVDCP3P5_CLASSA_18W);
		} else {
			cp_set_qc_bus_protections(HVDCP3_NONE);
		}
		break;

	case CP_STATE_FLASH2_ENTRY_1:
		cp_update_fc_status();

		if (sys_config.qc3p5_supported) {
			if (pm_state.bq2597x.bus_error_status == VBUS_ERROR_LOW) {
				tune_vbus_retry = cp_get_qc_pulse_cnt();
				tune_vbus_retry++;
				cp_tune_vbus_volt(VOLT_UP);
				pr_info("vbus:%d, retry_times:%d, tuning...\n",
						pm_state.bq2597x.vbus_volt, tune_vbus_retry);
//#ifdef CONFIG_CHARGER_LN8000
#if 1
			} else if (pm_state.bq2597x.vbus_volt < (pm_state.bq2597x.vbat_volt * 2 + BUS_VOLT_INIT_UP - 50)) {
				tune_vbus_retry = cp_get_qc_pulse_cnt();
				tune_vbus_retry++;
				cp_tune_vbus_volt(VOLT_UP);
				pr_info("ln vbus:%d, retry_times:%d, tuning...\n",
						pm_state.bq2597x.vbus_volt, tune_vbus_retry);
#endif
			} else {
				pr_err("vbus:%d, tuned above expected voltage, retry_times:%d\n",
						pm_state.bq2597x.vbus_volt, tune_vbus_retry);
				cp_move_state(CP_STATE_FLASH2_ENTRY_3);
				break;
			}

			if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
					&& tune_vbus_retry == FAKE_HVDCP3_DP_COUNT
					&& pm_state.bq2597x.vbus_volt <= FAKE_HVDCP3_VBUS) {
				cp_move_state(CP_STATE_SW_ENTRY);
				cp_set_fake_hvdcp3(true);
			}

			if((pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
						&& tune_vbus_retry > MAX_PLUSE_COUNT_ALLOWED)
					|| (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5
						&& tune_vbus_retry > MAX_HVDCP3P5_PLUSE_COUNT_ALLOWED)) {
				pr_err("Failed to tune adapter volt into valid range, charge with switching charger\n");
				pm_state.sw_fc2_init_fail = true;
				cp_move_state(CP_STATE_SW_ENTRY);
			}
			break;

		} else {
			if (pm_state.bq2597x.vbus_volt < (pm_state.bq2597x.vbat_volt * 2 + BUS_VOLT_INIT_UP - 50)) {
				tune_vbus_retry++;
				cp_tune_vbus_volt(VOLT_UP);
			} else {
				pr_err("voltage tuned above expected voltage, retry %d times\n", tune_vbus_retry);
				cp_move_state(CP_STATE_FLASH2_ENTRY_3);
				break;
			}
			if (tune_vbus_retry > 23) {
				pr_err("Failed to tune adapter volt into valid range, charge with switching charger\n");
				tune_vbus_count++;
				pm_state.sw_fc2_init_fail = true;
				cp_move_state(CP_STATE_SW_ENTRY);
			}
			break;

		}

	case CP_STATE_FLASH2_ENTRY_3:
		if (sys_config.qc3p5_supported) {
			if (pm_state.bq2597x.bus_error_status == VBUS_ERROR_HIGH) {
				pr_err("vbus=%d, too high to open cp switcher, decrease it.\n",
						pm_state.bq2597x.vbus_volt);
				cp_tune_vbus_volt(VOLT_DOWN);
			} else if (pm_state.bq2597x.bus_error_status == VBUS_ERROR_LOW) {
				pr_err("vbus=%d, too low to open cp switcher, increase it.\n",
						pm_state.bq2597x.vbus_volt);
				cp_tune_vbus_volt(VOLT_UP);
			} else {
				pr_err("vbat volt is ok, enable flash charging\n");
				if (!pm_state.bq2597x.charge_enabled) {
					cp_enable_fc(true);
					cp_check_fc_enabled();
					if (pm_state.bq2597x.charge_enabled) {
						if (retry_enable_bq_count > 0)
							retry_enable_bq_count = 0;
						cp_move_state(CP_STATE_FLASH2_TUNE);
						cp_enable_sw(false);
						cp_update_sw_status();
					} else {
						retry_enable_bq_count++;
						if (retry_enable_bq_count < 5)
							cp_move_state(CP_STATE_FLASH2_ENTRY_3);
						else
							cp_move_state(CP_STATE_SW_ENTRY);
					}
				} else {
					if (retry_enable_bq_count > 0)
						retry_enable_bq_count = 0;
					cp_move_state(CP_STATE_FLASH2_TUNE);
					cp_enable_sw(false);
					cp_update_sw_status();
				}
				ibus_lmt_change_timer = 0;
				fc2_taper_timer = 0;
			}
			break;
		} else {
			if (pm_state.bq2597x.vbus_volt >
					(pm_state.bq2597x.vbat_volt * 2 + BUS_VOLT_INIT_UP + 200)) {
				pr_err("vbat volt is too high, wait it down\n");
				/* voltage is too high, wait for voltage down, keep charge disabled to discharge */
			} else {
				pr_err("vbat volt is ok, enable flash charging\n");
				if (!pm_state.bq2597x.charge_enabled) {
					cp_enable_fc(true);
					cp_check_fc_enabled();
					if (pm_state.bq2597x.charge_enabled) {
						/*WA for zmi power strip(max 2500w)*/
						cp_notifier_vbus_raise_done();
						cp_enable_sw(false);
						cp_check_sw_enabled();
						cp_move_state(CP_STATE_FLASH2_TUNE);
					} else {
						if (retry_enable_bq_count < 5)
							cp_move_state(CP_STATE_FLASH2_ENTRY_3);
						else
							cp_move_state(CP_STATE_SW_ENTRY);
					}
				}
				ibus_lmt_change_timer = 0;
				fc2_taper_timer = 0;
			}
			break;
		}

	case CP_STATE_FLASH2_TUNE:
		ret = cp_flash2_charge(port);
		if (ret == -CP_ENABLE_FAIL) {
			retry_enable_bq_count++;
		} else {
			if (retry_enable_bq_count > 0)
				retry_enable_bq_count = 0;
		}
		if (ret == -ADC_ERR) {
			pr_err("Move to stop charging:%d\n", ret);
			cp_move_state(CP_STATE_STOP_CHARGE);
			break;
		} else if (ret == -CP_ENABLE_FAIL || ret == TAPER_DONE) {
			pr_err("Move to switch charging:%d\n", ret);
			cp_enable_fail_count++;
			pr_err("cp_enable_fail_count now:%d\n", cp_enable_fail_count);
			cp_move_state(CP_STATE_SW_ENTRY);
			break;
		} else if (ret == CP_ENABLE_FAIL) {
			tune_vbus_count = 0;
			pr_err("Move to switch charging, will try to recover to flash charging:%d\n",
					ret);
			recovery = true;
			cp_move_state(CP_STATE_SW_ENTRY);
		} else {// normal tune adapter output
			cp_move_state(CP_STATE_FLASH2_DELAY);
		}
		break;

	case CP_STATE_FLASH2_DELAY:
		cp_move_state(CP_STATE_FLASH2_TUNE);
		break;

	case CP_STATE_STOP_CHARGE:
		pr_err("Stop charging\n");
		if (pm_state.bq2597x.charge_enabled) {
			cp_enable_fc(false);
			cp_check_fc_enabled();
		}
		if (pm_state.sw_chager.charge_enabled) {
			cp_enable_sw(false);
			cp_check_sw_enabled();
		}
		if (pm_state.bms_fastcharge_mode)
			qc3_set_bms_fastcharge_mode(false);
		break;

	default:
		pr_err("No state defined! Move to stop charging\n");
		if (pm_state.bms_fastcharge_mode)
			qc3_set_bms_fastcharge_mode(false);
		cp_move_state(CP_STATE_STOP_CHARGE);
		break;
	}
}
int log_count = 0;
static void pm_state_print_log(void)
{
	pr_err("michglog:batt[%d-%d-%d-%d-%d-%d],bus[%d-%d-%d-%d-%d]usb[%d-%d-%d-%d-%d]ilimit[%d-%d-%d]therm[%d-%d-%d-%d-%d-%d]sys_config[%d-%d-%d-%d][%d-%d-%d-%d][%d-%d-%d-%d]\n",
	pm_state.bq2597x.batt_pres, pm_state.bms_temp, pm_state.capacity, pm_state.bq2597x.vbat_volt, pm_state.bq2597x.ibat_curr, pm_state.ibat_now, \
	pm_state.bq2597x.vbus_pres, pm_state.bq2597x.vbus_volt, pm_state.bq2597x.ibus_curr, pm_state.bq2597x.charge_enabled, pm_state.sw_chager.charge_enabled, \
	pm_state.usb_present, pm_state.usb_type, pm_state.hvdcp3_type, pm_state.bms_fastcharge_mode, pm_state.night_charging, \
	pm_state.ibus_limits, pm_state.effective_ibus, pm_state.ibus_lmt_curr, \
	pm_state.thermal_l, pm_state.warm_threshold_temp, pm_state.jeita_triggered, pm_state.batt_cell_volt_triggered, pm_state.is_temp_out_fc2_range, pm_state.reverse_mode, \
	sys_config.qc3p5_supported, sys_config.cp_sec_enable, sys_config.min_vbat_start_flash2, sys_config.fc2_taper_current, \
	sys_config.bat_volt_lp_lmt, sys_config.bat_curr_lp_lmt, sys_config.bus_volt_lp_lmt, sys_config.bus_curr_lp_lmt, \
	sys_config.ibus_plus_deviation_val, sys_config.ibus_minus_deviation_val, sys_config.ibat_plus_deviation_val, sys_config.ibat_minus_deviation_val);
}
static void cp_workfunc(struct work_struct *work)
{
	static int last_usb_type = 0;
	static int usb_present;
	cp_get_usb_type();

	cp_update_sw_status();
	cp_update_fc_status();

	cp_get_usb_present();
	pr_info("pm_state.usb_present: %d, pm_state.usb_type:%d\n", pm_state.usb_present, pm_state.usb_type);

	/* fix QC3 will stay in CP_STATE_STOP_CHARGE and caused not charge issue */
	if (pm_state.usb_present == 1 && usb_present == 0) {
		pm_state.state = CP_STATE_ENTRY;
	} else if (pm_state.usb_present == 0 && usb_present == 1) {
		pm_state.state = CP_STATE_DISCONNECT;
		cp_enable_fc(false);
	}

	usb_present = pm_state.usb_present;
	/* check whether usb is present */
	if (pm_state.usb_present == 0) {
		cp_set_qc_bus_protections(HVDCP3_NONE);
		cp_set_fake_hvdcp3(false);
		if (pm_state.bms_fastcharge_mode)
			qc3_set_bms_fastcharge_mode(false);
		cp_move_state(CP_STATE_DISCONNECT);
	}
	cp_statemachine(0);
	log_count++;
	if (log_count >= 20) {
		log_count = 0;
		pm_state_print_log();
	}
	if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 || (last_usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 
	&& pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP)) {
		schedule_delayed_work(&pm_state.qc3_pm_work, msecs_to_jiffies(PM_WORK_TIME_500MS));
	} else if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5){
		schedule_delayed_work(&pm_state.qc3_pm_work, msecs_to_jiffies(PM_WORK_TIME_100MS));
	}
	last_usb_type = pm_state.usb_type;
}

static void disable_hvdcp3_workfunc(struct work_struct *work)
{
	cancel_delayed_work(&pm_state.qc3_pm_work);
	cp_reset_vbus_volt();
	if (pm_state.bq2597x.charge_enabled) {
		cp_enable_fc(false);
		cp_check_fc_enabled();
	}
	if (!pm_state.sw_chager.charge_enabled) {
		cp_enable_sw(true);
		cp_check_sw_enabled();
	}
}

static int cp_qc30_notifier_call(struct notifier_block *nb,
		unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	static bool usb_hvdcp3_on;

	if (ev != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	if (strcmp(psy->desc->name, "usb") == 0) {
		cp_get_usb_type();
		if (pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 && !pm_state.reverse_mode) {
			if (!usb_hvdcp3_on) {
				schedule_delayed_work(&pm_state.qc3_pm_work, 3*HZ);
				usb_hvdcp3_on = true;
			} else {
				schedule_delayed_work(&pm_state.qc3_pm_work, msecs_to_jiffies(300));
			}
		} else if (sys_config.qc3p5_supported
				&& pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
			if (!usb_hvdcp3_on) {
				schedule_delayed_work(&pm_state.qc3_pm_work, HZ);
				usb_hvdcp3_on = true;
			} else {
				schedule_delayed_work(&pm_state.qc3_pm_work, msecs_to_jiffies(100));
			}
		} else if (pm_state.usb_type == POWER_SUPPLY_TYPE_UNKNOWN && usb_hvdcp3_on == true) {
			cancel_delayed_work(&pm_state.qc3_pm_work);
			schedule_delayed_work(&pm_state.qc3_pm_work, 0);
			cp_set_fake_hvdcp3(false);
			pr_info("pm_state.usb_type: %d\n", pm_state.usb_type);
			usb_hvdcp3_on = false;
		}
	}
	if (strcmp(psy->desc->name, "wireless") == 0) {
		cp_get_wireless_reverse_mode();
		cp_get_usb_type();
		if (!pm_state.reverse_mode && pm_state.usb_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
			schedule_delayed_work(&pm_state.qc3_pm_work, 3*HZ);
			usb_hvdcp3_on = true;
		} else if (pm_state.reverse_mode && !chg_enable_k81) {
			schedule_work(&pm_state.disable_hvdcp3_work);
		}
	}
	return NOTIFY_OK;
}

static int cp_qc30_register_notifier(struct cp_qc30_data *chip)
{
	int rc;

	chip->nb.notifier_call = cp_qc30_notifier_call;
	rc = power_supply_reg_notifier(&chip->nb);
	if (rc < 0) {
		pr_err("Couldn't register psy notifier rc = %d\n", rc);
		return rc;
	}

	return 0;
}

static int cp_qc30_parse_dt(struct cp_qc30_data *chip)
{
	struct device_node *node = chip->dev->of_node;
	int rc = 0;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	rc = of_property_read_u32(node,
			"mi,qc3-bat-volt-max", &chip->bat_volt_max);
	if (rc < 0)
		pr_err("qc3-bat-volt-max property missing, use default val\n");
	else
		sys_config.bat_volt_lp_lmt = chip->bat_volt_max;

	rc = of_property_read_u32(node,
			"mi,qc3-bat-curr-max", &chip->bat_curr_max);
	if (rc < 0)
		pr_err("qc3-bat-curr-max property missing, use default val\n");
	else
		sys_config.bat_curr_lp_lmt = chip->bat_curr_max;

	rc = of_property_read_u32(node,
			"mi,qc3-bus-volt-max", &chip->bus_volt_max);
	if (rc < 0)
		pr_err("qc3-bus-volt-max property missing, use default val\n");
	else
		sys_config.bus_volt_lp_lmt = chip->bus_volt_max;

	rc = of_property_read_u32(node,
			"mi,qc3-bus-curr-max", &chip->bus_curr_max);
	if (rc < 0)
		pr_err("qc3-bus-curr-max property missing, use default val\n");
	else
		sys_config.bus_curr_lp_lmt = chip->bus_curr_max;

	chip->battery_warm_th = JEITA_WARM_THR;
	rc = of_property_read_u32(node,
			"mi,qc3-battery-warm-th", &chip->battery_warm_th);
	if (rc < 0)
		pr_err("qc3-battery-warm-th missing, use default val\n");

	chip->cp_sec_enable = of_property_read_bool(node,
				"mi,cp-sec-enable");

	sys_config.cp_sec_enable = chip->cp_sec_enable;

	chip->qc3p5_supported = of_property_read_bool(node,
			"mi,support-qc3p5-without-smb");
	sys_config.qc3p5_supported = chip->qc3p5_supported;

	chg_enable_k81 = of_property_read_bool(node,
			"mi,chg-enable-k81");

	chip->use_qcom_gauge = of_property_read_bool(node,
				"mi,use-qcom-gauge");
	return rc;
}

static int cp_qc30_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct cp_qc30_data *chip;

	pr_info("%s enter\n", __func__);

	chip = devm_kzalloc(dev, sizeof(struct cp_qc30_data), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	ret = cp_qc30_parse_dt(chip);
	if (ret < 0) {
		pr_err("Couldn't parse device tree rc=%d\n", ret);
		return ret;
	}
	platform_set_drvdata(pdev, chip);

	pm_state.warm_threshold_temp =  chip->battery_warm_th;
	pr_info("battery_warm_th = %d\n", chip->battery_warm_th);
	pm_state.state = CP_STATE_DISCONNECT;
	pm_state.usb_type = POWER_SUPPLY_TYPE_UNKNOWN;
	pm_state.ibus_lmt_curr = sys_config.bus_curr_lp_lmt;

	INIT_WORK(&pm_state.disable_hvdcp3_work, disable_hvdcp3_workfunc);
	INIT_DELAYED_WORK(&pm_state.qc3_pm_work, cp_workfunc);

	cp_qc30_register_notifier(chip);

	pr_info("charge pump qc3 probe\n");

	return ret;
}

static int cp_qc30_remove(struct platform_device *pdev)
{
	cancel_delayed_work(&pm_state.qc3_pm_work);
	cancel_work_sync(&pm_state.disable_hvdcp3_work);
	return 0;
}

static const struct of_device_id cp_qc30_of_match[] = {
	{ .compatible = "xiaomi,cp-qc30", },
	{},
};

static struct platform_driver cp_qc30_driver = {
	.driver = {
		.name = "cp-qc30",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(cp_qc30_of_match),
	},
	.probe = cp_qc30_probe,
	.remove = cp_qc30_remove,
};

static int __init cp_qc30_init(void)
{
	return platform_driver_register(&cp_qc30_driver);
}

late_initcall(cp_qc30_init);

static void __exit cp_qc30_exit(void)
{
	return platform_driver_unregister(&cp_qc30_driver);
}
module_exit(cp_qc30_exit);

MODULE_AUTHOR("Fei Jiang<jiangfei1@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi cp qc30");
MODULE_LICENSE("GPL");

