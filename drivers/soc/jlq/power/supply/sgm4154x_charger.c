// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */
#define pr_fmt(fmt)    "[SGM4154X]:" fmt

#include <linux/types.h>
#include <linux/init.h>		/* For init/exit macros */
#include <linux/module.h>	/* For MODULE_ marcros  */
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/reboot.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/kernel.h>
#include <linux/extcon-provider.h>
//#include <jlq_charger_manager.h>
#include <soc/jlq/jlq_extcon.h>

#include "sgm4154x_charger.h"

//#define TRIGGER_CHARGE_TYPE_DETECTION
//#define SGM4154X_OTG

#define SGM4154X_DEBUG_WORK_POLL

#define MASK_TO_SHIFT(_x) (ffs(_x) - 1)
#define SGM4154X_VBUS_DEFUALT_VOLT_MV	5000
#define SGM4154X_IIC_LOCK_TIMEOUT  usecs_to_jiffies(200) //200US

static const unsigned int sgm4154x_extcon_cables[] = {
	EXTCON_USB,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_CDP,
	EXTCON_CHG_USB_DCP,
	JLQ_EXTCON_CHG_USB_FLOAT,
	EXTCON_CHG_USB_FAST,
	EXTCON_NONE,
};

enum sgm4154x_port_stat {
	SGM4154X_PORTSTAT_NO_INPUT = 0,
	SGM4154X_PORTSTAT_SDP,
	SGM4154X_PORTSTAT_CDP,
	SGM4154X_PORTSTAT_DCP,
	SGM4154X_PORTSTAT_HVDCP,
	SGM4154X_PORTSTAT_UNKNOWN,
	SGM4154X_PORTSTAT_NON_STANDARD,
	SGM4154X_PORTSTAT_OTG,
	SGM4154X_PORTSTAT_MAX,
};

struct sgm4154x_chip {
	struct device *dev;
	struct i2c_client *client;
	struct mutex i2c_rw_lock;
	struct mutex adc_lock;
	const char *chg_dev_name;
	const char *chg_psy_name;
	const char *input_regulator_name;
	const char *chg_regulator_name;
	const char *otg_regulator_name;
	const char *battery_profile_psy;
	struct delayed_work sgm4154x_work;
	struct delayed_work sgm4154x_batt_pro_work;
	struct delayed_work hvdcp_work;
	struct delayed_work unkown_type_work;
	struct mutex bc12_lock;
	uint32_t debug_mask;
	atomic_t vbus_gd;
	bool attach;
	bool discard;
	bool hvdcp_deting;
	enum sgm4154x_port_stat port;
	struct power_supply *psy;
	struct power_supply *sgm4154x_psy;
	struct power_supply *sgm4154x_usb_psy;
	enum power_supply_type chg_type;
	struct delayed_work psy_dwork;
	struct extcon_dev *edev;
	struct regulator *phy_regulator;
	const struct attribute_group **sysfs_groups;
	struct attribute_group attr_grp;
	struct device_attribute attr_usb_type;
	struct device_attribute attr_usb_discard;
	struct device_attribute attr_ibat;
	struct device_attribute attr_vbat;
	struct device_attribute attr_vsys;
	struct device_attribute attr_dbg_mask;
	struct attribute *attrs[16];

#ifdef TRIGGER_CHARGE_TYPE_DETECTION
	wait_queue_head_t bc12_en_req;
	atomic_t bc12_en_req_cnt;
	struct task_struct *bc12_en_kthread;
#endif

	struct notifier_block reboot_nb;
	int32_t	component_id;
	int32_t  irq_gpio;
	int32_t  irq;
	int32_t  ce_gpio;
	uint32_t chg_state;
	uint32_t vbus_state;
	int32_t unkown_type_retry_cnt;

	uint32_t ilimit_ma;
	uint32_t vlimit_mv;
	uint32_t vlimit_offset_mv;
	uint32_t cc_ma;
	uint32_t cv_mv;
	uint32_t pre_ma;
	uint32_t eoc_ma;
	uint32_t rechg_mv;
	uint32_t vsysmin_mv;
	uint32_t bat_ir_mohm;
	uint32_t vchg_comp_max_mv;
	unsigned long last_update;
	atomic_t boost_fault_cnt;
	bool vbus_bypass;
};

struct sgm4154x_irq_info {
	const char	*irq_name;
	int32_t		(*irq_func)(struct sgm4154x_chip *chip, uint8_t rt_stat);
	uint8_t irq_mask;
	uint8_t		irq_shift;
};
struct sgm4154x_irq_handle {
	uint8_t reg;
	uint8_t value;
	uint8_t pre_value;
	struct sgm4154x_irq_info irq_info[5];
};

const static char *vbus_type[] = {
	"NO INPUT",
	"SDP",
	"CDP",
	"DCP",
	"HVDCP",
	"UNKNOWN",
	"NON-STANDARD",
	"OTG"
};
const static char *charge_state[] = {
	"NOT CHARGING",
	"PRE-CHARGE",
	"FAST CHARGING",
	"CHARGE TERMINATION"
};
//for SGM4154XAD only
#if 0
const static char *adc_channel[] = {
	"vbat",
	"vsys",
	"tbat",
	"vbus",
	"ibat",
	"ibus",
};
#endif
static struct sgm4154x_irq_handle irq_handles[];
const static unsigned long unkown_type_retry_intvs[] = {
	3, 3, 3, 5, 10, 15, 30, 60, 300
};

#define SGM4154X_PR_ERR  (1 << 0)
#define SGM4154X_PR_WARN (1 << 1)
#define SGM4154X_PR_INFO (1 << 2)
#define SGM4154X_PR_DBG  (1 << 3)

#define SGM4154X_PR(_level, fmt, ...) \
	do { \
		if ((_level) & chip->debug_mask) { \
			dev_err(chip->dev, "%s:" fmt, __func__, ##__VA_ARGS__); \
		} else { \
			dev_info(chip->dev, "%s:" fmt, __func__, ##__VA_ARGS__); \
		}\
	} while(0)

//NOTE: pointer "chip", must have a struct sgm4154x_chip *chip in context
#define sgm4154x_err(fmt, ...)   SGM4154X_PR(SGM4154X_PR_ERR, fmt, ##__VA_ARGS__);
#define sgm4154x_warn(fmt, ...)   SGM4154X_PR(SGM4154X_PR_WARN, fmt, ##__VA_ARGS__);
#define sgm4154x_info(fmt, ...)  SGM4154X_PR(SGM4154X_PR_INFO, fmt, ##__VA_ARGS__);
#define sgm4154x_dbg(fmt, ...)   SGM4154X_PR(SGM4154X_PR_DBG, fmt, ##__VA_ARGS__);

static int32_t	sgm4154x_psy_notify(struct sgm4154x_chip *chip);
static int32_t	sgm4154x_extcon_clear_types(struct sgm4154x_chip *chip);
static int32_t	sgm4154x_extcon_notify(struct sgm4154x_chip *chip, unsigned int extcon_id, bool data);

#define PINCTL_BASE 0x34501000
#define I2C_4_SCL_GPIO 446
#define I2C_4_SDA_GPIO 447
#define I2C_4_SCL_PINCTL 0x50
#define I2C_4_SDA_PINCTL 0x54

static inline u32 i2c_read32(void __iomem *addr)
{
        return readl(addr);
}

static inline void i2c_write32(void __iomem *addr, u32 v)
{
        writel(v, addr);
}

static s32 i2cbus_recovery(void)
{
	unsigned int v_scl, i, v_sda;
	void __iomem *i2c_pinctl_base = ioremap_nocache(PINCTL_BASE, 0x100);

	v_sda = i2c_read32(i2c_pinctl_base + I2C_4_SDA_PINCTL);
	v_scl = i2c_read32(i2c_pinctl_base + I2C_4_SCL_PINCTL);
	gpio_request(I2C_4_SCL_GPIO, "i2c_4_scl");
	//gpio_request(I2C_4_SDA_GPIO, "i2c_4_sda");

	for (i = 0; i < 10; i++) {
		gpio_direction_output(I2C_4_SCL_GPIO, 1);
		//gpio_direction_output(I2C_4_SDA_GPIO, 1);
		udelay(5);
		gpio_direction_output(I2C_4_SCL_GPIO, 0);
		//gpio_direction_output(I2C_4_SDA_GPIO, 0);
		udelay(5);
	}

	//send stop
	gpio_direction_output(I2C_4_SCL_GPIO, 1);
	udelay(1);
	//gpio_direction_output(I2C_4_SDA_GPIO, 1);

	gpio_free(I2C_4_SCL_GPIO);
	//gpio_free(I2C_4_SDA_GPIO);
	i2c_write32(i2c_pinctl_base + I2C_4_SDA_PINCTL, v_sda);
	i2c_write32(i2c_pinctl_base + I2C_4_SCL_PINCTL, v_scl);
	iounmap(i2c_pinctl_base);

	return 0;
}

static int32_t sgm4154x_read_byte(struct sgm4154x_chip *chip, uint8_t reg, uint8_t *data)
{
	int32_t ret = 0;
	int32_t i = 0;

	pm_stay_awake(chip->dev);
	mutex_lock(&chip->i2c_rw_lock);
	if (time_is_after_eq_jiffies(chip->last_update + SGM4154X_IIC_LOCK_TIMEOUT))
		udelay(200);
	for (i = 0; i < 4; i++) {
		ret = i2c_smbus_read_byte_data(chip->client, reg);
		if (ret < 0)
		{
			dev_err(chip->dev, "%s: reg[0x%02X] err %d, %d times\n",
				__func__, reg, ret, i);
			if ( -110 == ret)
				i2cbus_recovery();
		}
		else
			break;
	}
	chip->last_update = jiffies;
	mutex_unlock(&chip->i2c_rw_lock);
	pm_relax(chip->dev);

	if (i >= 4) {
		*data = 0;
		return -1;
	}
	*data = (uint8_t)ret;
	//sgm4154x_info("%s: reg[0x%02X]  val[0x%02x]\n", __func__, reg, (uint8_t)ret);
	return 0;
}

static int32_t sgm4154x_write_byte(
		struct sgm4154x_chip *chip, int32_t reg, uint8_t val)
{
	int32_t ret = 0;
	int32_t i = 0;
	//sgm4154x_info("%s: reg[0x%02X]  val[0x%02x]\n", __func__, reg, val);
	pm_stay_awake(chip->dev);
	mutex_lock(&chip->i2c_rw_lock);
	if (time_is_after_eq_jiffies(chip->last_update + SGM4154X_IIC_LOCK_TIMEOUT))
		udelay(200);
	for (i = 0; i < 4; i++) {
		ret = i2c_smbus_write_byte_data(chip->client, reg, val);
		if (ret < 0) {
			dev_err(chip->dev, "%s ERROR: reg[0x%02X] err %d, %d times\n",
				__func__, reg, ret, i);
			if ( -110 == ret)
				i2cbus_recovery();
		}
		else
			break;
	}
	chip->last_update = jiffies;
	mutex_unlock(&chip->i2c_rw_lock);
	pm_relax(chip->dev);
	return (i >= 4) ? -1 : 0;
}
static int32_t sgm4154x_update(
	struct sgm4154x_chip *chip, uint8_t RegNum,
	uint8_t val, uint8_t MASK, uint8_t SHIFT)
{
	uint8_t data = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, RegNum, &data);
	if (ret < 0)
		return ret;

	if ((data & (MASK << SHIFT)) != (val << SHIFT)) {
		data &= ~(MASK << SHIFT);
		data |= (val << SHIFT);
		return sgm4154x_write_byte(chip, RegNum, data);
	}
	return 0;
}
/**********************************************************
 *
 *   [Internal Function]
 *
 *********************************************************/
#if 0
//enter debug mode:write 0x64 0x62 0x67 into 0xE0,then read 0xE0,if 0xE0=0x03,success
//exit debug mode,write 0x00 into 0xE0
static int32_t sgm4154x_enter_debug_mode(struct sgm4154x_chip *chip, int32_t enable)
{
	uint8_t i    = 0;
	int32_t ret  = 0;
	uint8_t data = 0;
	uint8_t check_val = 0;

	for (i = 0; i < 5; i++)
	{
		if (!!enable)
		{
			sgm4154x_write_byte(chip, 0xE0, 0x64);
			sgm4154x_write_byte(chip, 0xE0, 0x62);
			sgm4154x_write_byte(chip, 0xE0, 0x67);
			check_val = 0x03;
		} else {
			sgm4154x_write_byte(chip, 0xE0, 0x00);
			check_val = 0;
		}

		ret = sgm4154x_read_byte(chip, 0xE0, &data);
		dev_dbg(chip->dev, "check: ret 0x%02X, data = 0x%02X\n", ret, data);
		if ((0 == ret) && (check_val == data))
		{
			dev_info(chip->dev,"%s debug mode success\n",(!!enable) ? "enter": "exit");
			return 0;
		} else {
			dev_err(chip->dev, "%s debug mode failed,retry %d\n",(!!enable) ? "enter":  "exit", i);
			msleep(10);
		}
	}
	return -1;
}
//enable 0xE3=0x00,disable 0xE3=0x08

static int32_t sgm4154x_enable_sysovp(struct sgm4154x_chip *chip, int32_t enable)
{
	int32_t ret  = 0;
	uint8_t i    = 0;
	uint8_t data = 0;
	uint8_t val  = (!!enable) ? 0x00 : 0x08;

	ret = sgm4154x_read_byte(chip, 0xE3, &data);
	dev_dbg(chip->dev, "check: ret 0x%02X, 0xE3 = 0x%02X\n",ret,data);
	if ((0 == ret) && (val == data)) {
		dev_info(chip->dev, "%s sysovp success\n", (!!enable) ? "enable" : "disable");
		return 0;
	}
	for (i = 0 ; i < 5; i++) {
		dev_dbg(chip->dev, "%s sysovp: write 0x%02X to 0xE3\n", (!!enable) ? "enable" : "disable", val);
		sgm4154x_write_byte(chip, 0xE3, val);

		ret = sgm4154x_read_byte(chip, 0xE3, &data);
		dev_dbg(chip->dev, "check: ret 0x%02X, 0xE3 = 0x%02X\n",ret,data);
		if ((0 == ret) && (val == data))
		{
			dev_info(chip->dev, "%s sysovp success\n", (!!enable) ? "enable" : "disable");
			msleep(10);
			return 0;
		} else {
			dev_err(chip->dev, "%s sysovp failed,retry %d\n", (!!enable) ? "enable" : "disable", i);
			msleep(10);
		}
	}
	return -1;
}
#endif
static int sgm4154x_get_main_psy_prop(struct sgm4154x_chip *chip,
	enum power_supply_property psp,
	union power_supply_propval *val)
{
	struct power_supply *main_psy;
	int ret;

	if (!chip->battery_profile_psy)
		return -EBUSY;

	main_psy = power_supply_get_by_name(chip->battery_profile_psy);
	if (IS_ERR_OR_NULL(main_psy)) {
		sgm4154x_dbg("Cannot find power supply- \"%s\"\n",
				chip->battery_profile_psy);
		return -ENODEV;
	}
	ret = power_supply_get_property(main_psy, psp, val);
	if (ret < 0) {
		sgm4154x_dbg("Cannot read psp:%d value from \"%s\"\n",
			psp, chip->battery_profile_psy);
		power_supply_put(main_psy);
		return ret;
	}
	power_supply_put(main_psy);
	return ret;
}
static int32_t sgm4154x_set_en_hiz(struct sgm4154x_chip *chip, int32_t enable)
{
	return sgm4154x_update(chip, SGM4154X_R00, !!enable,
			CON0_EN_HIZ_MASK, CON0_EN_HIZ_SHIFT);
}
static int32_t sgm4154x_get_en_hiz(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R00, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg & (CON0_EN_HIZ_MASK << CON0_EN_HIZ_SHIFT)) >>
			CON0_EN_HIZ_SHIFT;
	return !!data_reg;
}
static int32_t sgm4154x_set_input_curr_lim(
		struct sgm4154x_chip *chip, uint32_t ilimit_ma)
{
	uint8_t  data_reg = 0;
	//uint32_t input_current = 0;
	
	ilimit_ma = ilimit_ma * 1000;

	if (ilimit_ma < SGM4154x_IINDPM_I_MIN_uA  ||
			ilimit_ma > SGM4154x_IINDPM_I_MAX_uA)
		return -1;	
	
	if (ilimit_ma >= SGM4154x_IINDPM_I_MIN_uA  && ilimit_ma <= 3100000)//default
		data_reg = (ilimit_ma-SGM4154x_IINDPM_I_MIN_uA ) / SGM4154x_IINDPM_STEP_uA;
	else if (ilimit_ma > 3100000 && ilimit_ma < SGM4154x_IINDPM_I_MAX_uA)
		data_reg = 0x1E;
	else if(ilimit_ma ==  SGM4154x_IINDPM_I_MAX_uA)
		data_reg =0x1F;

	return sgm4154x_update(chip, SGM4154X_R00, data_reg,
			CON0_IINLIM_MASK, CON0_IINLIM_SHIFT);
}
static int32_t sgm4154x_get_input_curr_lim(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R00, &data_reg);
	if (ret < 0)
		return ret;

	if (0x1F == data_reg)
		return (SGM4154x_IINDPM_I_MAX_uA/1000);
	else
		return ((data_reg*SGM4154x_IINDPM_STEP_uA + SGM4154x_IINDPM_I_MIN_uA)/1000);	
	
	//return ((data_reg * INPUT_CURRT_STEP + INPUT_CURRT_MIN)/1000);
}
static int32_t sgm4154x_get_iterm(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;
  
	ret = sgm4154x_read_byte(chip, SGM4154X_R03, &data_reg);
	if (ret < 0)
		return ret;
/*
	data_reg = (data_reg & (CON3_ITERM_MASK << CON3_ITERM_SHIFT)) >> CON3_ITERM_SHIFT;

	return (data_reg * EOC_CURRT_STEP + EOC_CURRT_MIN);*/
	
	data_reg &= SGM4154x_TERMCHRG_CUR_MASK;
	data_reg = data_reg * 60000 + 60000;
	return (data_reg/1000);
}
static int32_t sgm4154x_set_iterm(struct sgm4154x_chip *chip, uint32_t iterm)
{
	uint8_t data_reg = 0;
	//uint32_t data_temp = 0;
	uint32_t offset = SGM4154x_TERMCHRG_I_MIN_uA;
	
	iterm = iterm*1000;
	
	if (iterm < SGM4154x_TERMCHRG_I_MIN_uA)
		iterm = SGM4154x_TERMCHRG_I_MIN_uA;
	else if (iterm > SGM4154x_TERMCHRG_I_MAX_uA)
		iterm = SGM4154x_TERMCHRG_I_MAX_uA;

	data_reg = (iterm - offset) / SGM4154x_TERMCHRG_CURRENT_STEP_uA;

	return sgm4154x_update(chip, SGM4154X_R03, data_reg, CON3_ITERM_MASK, CON3_ITERM_SHIFT);
}

static int32_t sgm4154x_set_wd_reset(struct sgm4154x_chip *chip)
{
	return sgm4154x_update(chip, SGM4154X_R01, 1, CON1_WD_MASK, CON1_WD_SHIFT);
}

static int sgm4154x_set_otg_en(struct sgm4154x_chip *chip, int32_t enable)
{
	return sgm4154x_update(chip, SGM4154X_R01, !!enable,
			CON1_OTG_CONFIG_MASK, CON1_OTG_CONFIG_SHIFT);
}

static int32_t sgm4154x_set_charger_en(struct sgm4154x_chip *chip, int32_t enable)
{
	int ret;

	ret =  sgm4154x_update(chip, SGM4154X_R01, !!enable,
			CON1_CHG_CONFIG_MASK, CON1_CHG_CONFIG_SHIFT);

	return ret;
}
static int32_t sgm4154x_get_charger_en(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R01, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg & (CON1_CHG_CONFIG_MASK << CON1_CHG_CONFIG_SHIFT))
		>> CON1_CHG_CONFIG_SHIFT;

	return !!data_reg;
}
const static s_sys_min[] = {2600,2800,3000,3200,3400,3500,3600,3700};

static int32_t sgm4154x_set_sys_min_volt(struct sgm4154x_chip *chip, uint32_t vsys_mv)
{
	//uint8_t  data_reg = 0;
	uint32_t vsys = 0;
	uint8_t  index = 0;

	if (vsys_mv < 2600/*CON1_SYS_V_LIMIT_MIN*/)
		vsys = 2600;//CON1_SYS_V_LIMIT_MIN;
	else if (vsys_mv > CON1_SYS_V_LIMIT_MAX)
		vsys = CON1_SYS_V_LIMIT_MAX;
	else
		vsys = vsys_mv;
#if 0
	data_reg = (vsys - CON1_SYS_V_LIMIT_MIN) / CON1_SYS_V_LIMIT_STEP;
	
	if(data_reg < 0x02)
		data_reg = 0x02;
	else if(data_reg < 0x04)
		data_reg = 0x03;
	else if(data_reg < 0x05)
		data_reg = 0x04;
	else if(data_reg < 0x06)
		data_reg = 0x05;
	else if(data_reg < 0x07)
		data_reg = 0x06;
	else if(data_reg < 0x08)
		data_reg = 0x07;
	else
		data_reg = 0x05;				//default 
#endif
	for(index = 1;index < 8 && vsys_mv < s_sys_min[index] ;index++)
		index = index -1;
	
	return sgm4154x_update(chip, SGM4154X_R01, index,
				CON1_SYS_V_LIMIT_MASK, CON1_SYS_V_LIMIT_SHIFT);
}

int32_t sgm4154x_set_boost_ilim(struct sgm4154x_chip *chip, uint32_t boost_ilimit)
{
	uint8_t data_reg = 0;

	if (boost_ilimit <= 1200)		//500
		data_reg = 0x01;
	else
		data_reg = 0x00;

	return sgm4154x_update(chip, SGM4154X_R02, data_reg,
			CON2_BOOST_ILIM_MASK, CON2_BOOST_ILIM_SHIFT);
}

static int32_t sgm4154x_get_boost_ilim(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R02, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg & (CON2_BOOST_ILIM_MASK << CON2_BOOST_ILIM_SHIFT)) >>
				CON2_BOOST_ILIM_SHIFT;
				
	return (data_reg == 0) ? 2000 : 1200;
//	return (data_reg == 1) ? 1200 : 500;
}

static int32_t sgm4154x_set_ichg_current(struct sgm4154x_chip *chip, uint32_t ichg)
{
#if 0
	uint8_t data_reg = 0;
	uint32_t data_temp = 0;
	uint8_t  r08_data = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R08, &r08_data);
	//sgm4154x_dbg("read reg 0x%02X , ret = %d, data = 0x%02d\n", SGM4154X_R08, ret, r08_data);
	if (ret < 0 || (r08_data & 0x01))
		ichg = 1020;

	if (ichg < ICHG_CURR_MIN)
		data_temp = ICHG_CURR_MIN;
	else if (ichg > ICHG_CURR_MAX)
		data_temp = ICHG_CURR_MAX;
	else
		data_temp = ichg;

	data_reg = (data_temp - ICHG_CURR_MIN) / ICHG_CURR_STEP;

	return sgm4154x_update(chip, SGM4154X_R02, data_reg, CON2_ICHG_MASK, CON2_ICHG_SHIFT);
#endif
	uint8_t ret;
	uint8_t reg_val;

	ichg = ichg*1000;
	
	if (ichg < SGM4154x_ICHRG_I_MIN_uA)
		ichg = SGM4154x_ICHRG_I_MIN_uA;
	else if ( ichg > SGM4154x_ICHRG_I_MAX_uA)
		ichg = SGM4154x_ICHRG_I_MAX_uA;

	reg_val = ichg / SGM4154x_ICHRG_CURRENT_STEP_uA;

	ret = sgm4154x_update(chip, SGM4154X_R02, reg_val, CON2_ICHG_MASK, CON2_ICHG_SHIFT);;
	
	return ret;
}
static int32_t sgm4154x_get_ichg_current(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R02, &data_reg);
	if (ret < 0)
		return ret;
		
	data_reg &= SGM4154x_ICHRG_CUR_MASK;

	return (data_reg * SGM4154x_ICHRG_CURRENT_STEP_uA/1000);
	
/*
	data_reg = (data_reg & (CON2_ICHG_MASK << CON2_ICHG_SHIFT)) >> CON2_ICHG_SHIFT;

	return (data_reg * ICHG_CURR_STEP + ICHG_CURR_MIN);*/
}
// /* CON3---------------------------------------------------- */

static int32_t sgm4154x_set_iprechg_current(
		struct sgm4154x_chip *chip, uint32_t iprechg_curr)
{
	uint8_t data_reg = 0;
	//uint32_t data_temp = 0;
#if 0
	if (iprechg_curr < IPRECHG_CURRT_MIN)
		data_temp = IPRECHG_CURRT_MIN;
	else if (iprechg_curr > IPRECHG_CURRT_MAX)
		data_temp = IPRECHG_CURRT_MAX;
	else
		data_temp = iprechg_curr;

	data_reg = (data_temp - IPRECHG_CURRT_MIN) / IPRECHG_CURRT_STEP;
#endif

	uint32_t offset = SGM4154x_PRECHRG_I_MIN_uA;
	iprechg_curr = iprechg_curr*1000;
	
	if (iprechg_curr < SGM4154x_PRECHRG_I_MIN_uA)
		iprechg_curr = SGM4154x_PRECHRG_I_MIN_uA;
	else if (iprechg_curr > SGM4154x_PRECHRG_I_MAX_uA)
		iprechg_curr = SGM4154x_PRECHRG_I_MAX_uA;

	data_reg = (iprechg_curr - offset) / SGM4154x_PRECHRG_CURRENT_STEP_uA;

	return sgm4154x_update(chip, SGM4154X_R03, data_reg, CON3_IPRECHG_MASK, CON3_IPRECHG_SHIFT);
}
static int32_t sgm4154x_get_iprechg_current(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R03, &data_reg);
	if (ret < 0)
		return ret;
		
	data_reg = (data_reg & SGM4154x_PRECHRG_CUR_MASK) >> 4;

	return (data_reg * SGM4154x_PRECHRG_CURRENT_STEP_uA/1000);

#if 0
	data_reg = (data_reg & (CON3_IPRECHG_MASK << CON3_IPRECHG_SHIFT)) >> CON3_IPRECHG_SHIFT;
	return (data_reg * IPRECHG_CURRT_STEP + IPRECHG_CURRT_MIN);
#endif
}

// /* CON4---------------------------------------------------- */
#if 0
static int32_t sgm4154x_set_vreg_volt(struct sgm4154x_chip *chip, uint32_t vreg_chg_vol)
{
	uint8_t data_reg = 0;
	uint32_t data_temp = 0;
	pr_info("set vreg:%d\n",vreg_chg_vol);
	if (vreg_chg_vol < VREG_VOL_MIN)
		data_temp = VREG_VOL_MIN;
	else if (vreg_chg_vol > VREG_VOL_MAX)
		data_temp = VREG_VOL_MAX;
	else
		data_temp = vreg_chg_vol;
	
	data_reg = (data_temp - VREG_VOL_MIN) / VREG_VOL_STEP;
	pr_info("set vreg data:%d\n",data_reg);
	return sgm4154x_update(chip, SGM4154X_R04, data_reg, CON4_VREG_MASK, CON4_VREG_SHIFT);
}

static int32_t sgm4154x_get_vreg_volt(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R04, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg & (CON4_VREG_MASK << CON4_VREG_SHIFT)) >> CON4_VREG_SHIFT;

	return (data_reg * VREG_VOL_STEP + VREG_VOL_MIN);
}
#else
static int32_t sgm4154x_set_vreg_volt(struct sgm4154x_chip *chip, uint32_t vreg_chg_vol)
{
	uint8_t data_reg = 0;
	uint32_t vreg_chg_vol_uV;
	vreg_chg_vol_uV = vreg_chg_vol*1000; //uv
	int32_t ret;

	if(vreg_chg_vol==4450)// special value
	{
		ret = sgm4154x_update(chip, SGM4154X_R04, 0x13, CON4_VREG_MASK, CON4_VREG_SHIFT);
		sgm4154x_update(chip, SGM4154X_R0F, 3, 0x03 ,6);
		return ret;
	}
	
	if (vreg_chg_vol_uV < SGM4154x_VREG_V_MIN_uV)
		vreg_chg_vol_uV = SGM4154x_VREG_V_MIN_uV;
	else if (vreg_chg_vol_uV > SGM4154x_VREG_V_MAX_uV)
		vreg_chg_vol_uV = SGM4154x_VREG_V_MAX_uV;
	
	if (((4352*1000) <= vreg_chg_vol_uV) && ((4368*1000) >= vreg_chg_vol_uV)) //spec data
		data_reg = 0xF;
	else
		data_reg = (vreg_chg_vol_uV-SGM4154x_VREG_V_MIN_uV) / SGM4154x_VREG_V_STEP_uV;
	//printk("sgm4154x %s Set REG04 to %#x \n", __func__, data_reg);
	ret = sgm4154x_update(chip, SGM4154X_R04, data_reg, CON4_VREG_MASK, CON4_VREG_SHIFT);
	if(ret<0)
	{
		return ret;
	}
	sgm4154x_update(chip, SGM4154X_R0F, 0, 0x03 ,6);
	return ret;
}

static int32_t sgm4154x_get_vreg_volt(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t chrg_volt = 0;
	int32_t ret = 0;
	int32_t chrg_volt_ft_mV = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R04, &data_reg);
	if (ret < 0)
		return ret;
	
	data_reg = (data_reg & SGM4154x_VREG_V_MASK)>>3;  
	if (15 == data_reg)
		chrg_volt = 4352000; //default
	else if (data_reg < 25)	
		chrg_volt = data_reg*SGM4154x_VREG_V_STEP_uV + SGM4154x_VREG_V_MIN_uV;	

	ret = sgm4154x_read_byte(chip, SGM4154X_R0F, &data_reg);
	if (ret < 0)
	{
		return chrg_volt/1000;
	}
	data_reg = (data_reg & SGM4154x_VREG_FT_MASK) >> 6;
	switch(data_reg)
	{
		case 3:
			chrg_volt_ft_mV = -16;
			break;
		case 2:
			chrg_volt_ft_mV = -8;
			break;
		case 1:
			chrg_volt_ft_mV = 8;
			break;
		default:
			chrg_volt_ft_mV = 0;
			break;
	}

	return (chrg_volt/1000+chrg_volt_ft_mV);
}

#endif

#if 0
static int32_t sgm4154x_set_ir_comp(struct sgm4154x_chip *chip, uint32_t mohm)
{
	uint8_t data_reg = 0;
	uint32_t data_temp = 0;

	if (mohm > COND_BAT_COMP_MAX)
		data_temp = COND_BAT_COMP_MAX;
	else
		data_temp = mohm;

	data_reg = (data_temp) / COND_BAT_COMP_STEP;
	sgm4154x_info("Not support sgm4154x_set_ir_comp\n");
	return 0;
	return sgm4154x_update(chip, SGM4154X_R0D, data_reg, COND_BAT_COMP_MASK, COND_BAT_COMP_SHIFT);
}

static int32_t sgm4154x_get_ir_comp(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R0D, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg >> COND_BAT_COMP_SHIFT) & COND_BAT_COMP_MASK;
	sgm4154x_info("Not support sgm4154x_get_ir_comp\n");
	return 0;
	return (data_reg * COND_BAT_COMP_STEP);
}

static int32_t sgm4154x_set_comp_max(struct sgm4154x_chip *chip, uint32_t mv)
{
	uint8_t data_reg = 0;
	uint32_t data_temp = 0;
	if (mv > COND_COMP_MAX_MAX)
		data_temp = COND_COMP_MAX_MAX;
	else
		data_temp = mv;

	data_reg = (data_temp) / COND_COMP_MAX_STEP;
	sgm4154x_info("Not support sgm4154x_set_comp_max\n");
	return 0;
	return sgm4154x_update(chip, SGM4154X_R0D, data_reg, COND_COMP_MAX_MASK, COND_COMP_MAX_SHIFT);
}

static int32_t sgm4154x_get_comp_max(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R0D, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg >> COND_COMP_MAX_SHIFT) & COND_COMP_MAX_MASK;
	sgm4154x_info("Not support sgm4154x_get_comp_max\n");
	return 0;
	return (data_reg * COND_COMP_MAX_STEP);
}
#endif

static int32_t sgm4154x_set_rechg_volt(
		struct sgm4154x_chip *chip, uint32_t rechg_volt)
{
	uint8_t data_reg = 0;

	if (rechg_volt >= 200)
		data_reg = 0x01;
	else
		data_reg = 0x00;

	return sgm4154x_update(chip, SGM4154X_R04, data_reg, CON4_VRECHG_MASK, CON4_VRECHG_SHIFT);
}
// /* CON5---------------------------------------------------- */
static int32_t sgm4154x_set_en_term_chg(struct sgm4154x_chip *chip, int32_t enable)
{
	return sgm4154x_update(chip, SGM4154X_R05, !!enable,
		CON5_EN_TERM_CHG_MASK, CON5_EN_TERM_CHG_SHIFT);
}
static int32_t sgm4154x_set_wd_timer(struct sgm4154x_chip *chip, uint32_t second)
{
	uint8_t data_reg = 0;

	if (second < 40)		//second < 40s, disable wdt
		data_reg = 0x00;
	else if (second < 80)	//40 <= second < 80, set 40s
		data_reg = 0x01;
	else if (second < 160) //80 <= second < 160, set 80s
		data_reg = 0x02;
	else
		data_reg = 0x03;	//second >= 160, set 160s

	return sgm4154x_update(chip, SGM4154X_R05, data_reg,
		CON5_WTG_TIM_SET_MASK, CON5_WTG_TIM_SET_SHIFT);
}
static int32_t sgm4154x_set_en_chg_timer(struct sgm4154x_chip *chip, int32_t enable)
{
	return sgm4154x_update(chip, SGM4154X_R05, !!enable,
					CON5_EN_TIMER_MASK, CON5_EN_TIMER_SHIFT);
}
static int32_t sgm4154x_get_en_chg_timer(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R05, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg & (CON5_EN_TIMER_MASK << CON5_EN_TIMER_SHIFT)) >>
			CON5_EN_TIMER_SHIFT;

	return !!data_reg;
}
// /* CON6---------------------------------------------------- */

enum SGM4154x_VINDPM_OS{
	VINDPM_OS_3900mV,
	VINDPM_OS_5900mV,
	VINDPM_OS_7500mV,
	VINDPM_OS_10500mV,
};

static int sgm4154x_set_vindpm_offset_os(struct sgm4154x_chip *chip,
												enum SGM4154x_VINDPM_OS os_val)
{	
	
	return sgm4154x_update(chip, SGM4154X_R0F, os_val, 0x03, 0);

}

static int sgm4154x_get_vindpm_offset_os(struct sgm4154x_chip *chip)
{	
	int32_t ret = 0;
	uint8_t data_reg = 0;
	
	ret = sgm4154x_read_byte(chip, SGM4154X_R0F, &data_reg);
	if (ret < 0)
		return ret;
		
	data_reg = data_reg & SGM4154x_VINDPM_OS_MASK;	
	if (VINDPM_OS_3900mV == data_reg)
		ret = 3900000; //uv
	else if (VINDPM_OS_5900mV == data_reg)
		ret = 5900000;
	else if (VINDPM_OS_7500mV == data_reg)
		ret = 7500000;
	else if (VINDPM_OS_10500mV == data_reg)
		ret = 10500000;

	return ret;

}
static int32_t sgm4154x_get_vlimit(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;
	uint32_t vlimit = 0;
	uint32_t offset = 0;
		
	ret = sgm4154x_read_byte(chip, SGM4154X_R06, &data_reg);
	if (ret < 0)
		return ret;

	ret = sgm4154x_get_vindpm_offset_os(chip);
	if (ret < 0)
		return ret;
	offset = ret;
	
	vlimit = offset + (data_reg & 0x0F) * SGM4154x_VINDPM_STEP_uV;
	return (vlimit/1000);

}
static int32_t sgm4154x_set_vlimit(struct sgm4154x_chip *chip, uint32_t vlimit_mv)
{
	uint8_t data_reg = 0;	
//	int32_t ret = 0;	
	enum SGM4154x_VINDPM_OS os_val;
	int32_t offset = 0;
	vlimit_mv = vlimit_mv*1000;
	if (vlimit_mv < SGM4154x_VINDPM_V_MIN_uV)
		vlimit_mv = SGM4154x_VINDPM_V_MIN_uV;
	else if (vlimit_mv > SGM4154x_VINDPM_V_MAX_uV)
		vlimit_mv = SGM4154x_VINDPM_V_MAX_uV;		

	/* fix no usb port when echo 1 to charging_disable */
	if(chip->discard)
	{
		sgm4154x_dbg("charging discard is set!!!!!\n");
		vlimit_mv = SGM4154x_VINDPM_V_MAX_uV;
	}
	/* fix end */
	
	if (vlimit_mv < 5900000)
	{
		os_val = VINDPM_OS_3900mV;
		offset = 3900000; //uv
	}
	else if (vlimit_mv < 7500000)
	{
		os_val = VINDPM_OS_5900mV;
		offset = 5900000; //uv
	}
	else if (vlimit_mv < 10500000)
	{
		os_val = VINDPM_OS_7500mV;
		offset = 7500000; //uv
	}
	else
	{
		os_val = VINDPM_OS_10500mV;
		offset = 10500000; //uv
	}
	
	
	sgm4154x_set_vindpm_offset_os(chip,os_val);	
	data_reg = (vlimit_mv - offset) / SGM4154x_VINDPM_STEP_uV;
	sgm4154x_dbg("sgm4154x_set_vlimit %d %d\n.",vlimit_mv,os_val);
	data_reg &= 0x0F;
	return sgm4154x_update(chip, SGM4154X_R06, data_reg, 0x0F, 0);
}

enum HVDCP_QC_VOLT{
	HVDCP_QC_DEFAULT,
	HVDCP_QC_9V,
	HVDCP_QC_12V,
};
static bool sgm4154x_is_hvdcp(struct sgm4154x_chip *chip,enum HVDCP_QC_VOLT volt)
{
    int i = 20;
	int vlimt = 0;
	int ilimt = 0;
	//int temp = 0;
	uint8_t data_reg = 0;
	
	vlimt = sgm4154x_get_vlimit(chip);
	ilimt = sgm4154x_get_input_curr_lim(chip);
	sgm4154x_dbg(">>>>>sgm4154x_is_hvdcp vlimt:%d ilimt:%d volt:%d\n.", vlimt, ilimt, volt);
	if (HVDCP_QC_9V == volt)
	{
		sgm4154x_set_vlimit(chip, 8300);
	}
	else if (HVDCP_QC_12V == volt)
	{
		sgm4154x_set_vlimit(chip, 11000);
	}
	else{
		sgm4154x_set_vlimit(chip, 4600);
		return 0;
	}
	sgm4154x_set_input_curr_lim(chip, 500);

	while(i--){
		sgm4154x_read_byte(chip, SGM4154X_R0A, &data_reg);
		//sgm4154x_dbg(">>>>>%d:sgm4154x_is_hvdcp REG0A: %#x\n.", i, data_reg);
		if (1 == !!(data_reg&0x40) && 1 == i){
			sgm4154x_set_vlimit(chip,vlimt);
			sgm4154x_set_input_curr_lim(chip, ilimt);
			sgm4154x_dbg(">>>>>sgm4154x_is_hvdcp  fail!!!.");
			return 0;
		}
		mdelay(10);	
	}
	sgm4154x_set_input_curr_lim(chip, ilimt);
	sgm4154x_dbg(">>>>>>>>sgm4154x_is_hvdcp  success.");
	return 1;		
}

// /* CON8---------------------------------------------------- */
static int32_t sgm4154x_get_charger_status(struct sgm4154x_chip *chip)
{
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R08, &data_reg);
	if (ret < 0)
		return ret;

	data_reg = (data_reg & (CON8_CHRG_STAT_MASK << CON8_CHRG_STAT_SHIFT)) >>
			CON8_CHRG_STAT_SHIFT;

	return data_reg;
}
#if 0
static int32_t sgm4154x_get_adc_value(struct sgm4154x_chip *chip, enum sgm4154x_adc_channel adc)
{
	uint8_t val = 0, pg_good = 0;
	int32_t ret = 0, wait_cnt = 0, adc_value = 0, ilimit = 0;
		return 0;
	if (chip->component_id != SGM4154XAD_COMPINENT_ID &&
			chip->component_id != SGM4154XAD1_COMPINENT_ID)
		return -1;

	mutex_lock(&chip->adc_lock);
#if 1
	// one shot ac conversion
	ret = sgm4154x_write_byte(chip,SGM4154X_R11,0x00);
	if (ret < 0)
		goto error;
	//start adc conversion
	ret = sgm4154x_update(chip,SGM4154X_R11,1,CON11_START_ADC_MASK,CON11_START_ADC_SHIFT);
	if (ret < 0)
		goto error;
	do {
		msleep(10);
		wait_cnt++;
		ret = sgm4154x_read_byte(chip,SGM4154X_R11,&val);
		if (ret < 0)
			goto error;
	} while ((val & (CON11_START_ADC_MASK << CON11_START_ADC_SHIFT)) && wait_cnt < 100);
	//	pr_err("wait cnt %d\n",wait_cnt);
#else
		//start 1s continuous conversion
		ret = sgm4154x_update(chip,SGM4154X_R11,1,CON11_CONV_RATE_MASK,CON11_CONV_RATE_SHIFT);
		if (ret < 0)
			goto error;
#endif
	ret = sgm4154x_read_byte(chip,SGM4154X_R08,&val);
	if (ret < 0)
		goto error;

	pg_good = (val & (CON8_PG_STAT_MASK << CON8_PG_STAT_SHIFT)) >> CON8_PG_STAT_SHIFT;
	if (wait_cnt < 100) {
		ret = sgm4154x_read_byte(chip, SGM4154X_ADC_CHANNEL_START + adc, &val);
		if (ret < 0)
			goto error;
		switch (adc) {
		case vbat_channel:
		case vsys_channel:
			adc_value = 2304 + val * 20;
			break;

		case tbat_channel:
			adc_value = 21000 + 465 * val;
			break;

		case vbus_channel:
			adc_value = (val == 0) ? 0 : (2600 + val * 100);
			break;

		case ibat_channel:
			adc_value = val * 25;
			break;

		case ibus_channel:
			if (!!pg_good) {
				ilimit = sgm4154x_get_input_curr_lim(chip);
				if (ilimit < 0)
					goto error;
				adc_value = val * ilimit / 127;
			} else
				adc_value = 0;
			break;

		default:
			adc_value = 0;
			break;
		}
	} else {
		goto error;
	}
	sgm4154x_dbg("%s[0x%02X]: Regval = 0x%02X, Adcval = %d PG=%d\n",
		adc_channel[adc], SGM4154X_ADC_CHANNEL_START + adc, val, adc_value, !!pg_good);
	mutex_unlock(&chip->adc_lock);
	return adc_value;

error:
	mutex_unlock(&chip->adc_lock);
	return -1;
}
#endif
static int32_t sgm4154x_dump_msg(struct sgm4154x_chip *chip)
{
	uint8_t i = 0;
	uint8_t data[SGM4154X_REG_NUM] = { 0 };
	int32_t ret = 0;

	sgm4154x_set_wd_timer(chip,0);
	for (i = 0; i < SGM4154X_REG_NUM; i++) {
		ret = sgm4154x_read_byte(chip, i, &data[i]);
		if (ret < 0) {
			dev_err(chip->dev, "i2c transfor error\n");
			return -1;
		}
	}

	sgm4154x_info("[0x00] = 0x%02X, [0x01] = 0x%02X, [0x02] = 0x%02X, [0x03] = 0x%02X, [0x04] = 0x%02X, [0x05] = 0x%02X, [0x06] = 0x%02X, [0x07] = 0x%02X\n",
			 data[0x00], data[0x01], data[0x02], data[0x03],
			 data[0x04], data[0x05], data[0x06], data[0x07]);

	sgm4154x_info("[0x08] = 0x%02X, [0x09] = 0x%02X, [0x0A] = 0x%02X, [0x0B] = 0x%02X, [0x0C] = 0x%02X, [0x0D] = 0x%02X, [0x0E] = 0x%02X, [0x0F] = 0x%02X\n",
			data[0x08], data[0x09], data[0x0A], data[0x0B],
			data[0x0C], data[0x0D], data[0x0E], data[0x0F]);
	/*
	sgm4154x_info("[0x10] = 0x%02X\n", data[0x10]);
	*/
	//charge parameter
	sgm4154x_info("vchg:%d, ichg:%d, ilimit:%d, iterm %d, vlimit %d, sftimer %d, iboost %d\n",
		sgm4154x_get_vreg_volt(chip),
		sgm4154x_get_ichg_current(chip),
		sgm4154x_get_input_curr_lim(chip),
		sgm4154x_get_iterm(chip),
		sgm4154x_get_vlimit(chip),
		sgm4154x_get_en_chg_timer(chip),
		sgm4154x_get_boost_ilim(chip)
	);

	//fault and state
	sgm4154x_info("hiz:%d, chg_en:%d, chg_state:%s, vbus_type:%s, pg:%d, thm:%d, vsys_state:%d\n",
		sgm4154x_get_en_hiz(chip),
		sgm4154x_get_charger_en(chip),
		charge_state[(data[SGM4154X_R08] & (CON8_CHRG_STAT_MASK << CON8_CHRG_STAT_SHIFT)) >>
					CON8_CHRG_STAT_SHIFT],
		vbus_type[(data[SGM4154X_R08] & (CON8_VBUS_STAT_MASK << CON8_VBUS_STAT_SHIFT)) >>
					CON8_VBUS_STAT_SHIFT],
		(data[SGM4154X_R08] & (CON8_PG_STAT_MASK << CON8_PG_STAT_SHIFT)) >>
				CON8_PG_STAT_SHIFT,
		(data[SGM4154X_R08] & (CON8_THM_STAT_MASK << CON8_THM_STAT_SHIFT)) >>
				CON8_THM_STAT_SHIFT,
		(data[SGM4154X_R08] & (CON8_VSYS_STAT_MASK << CON8_VSYS_STAT_SHIFT)) >>
				CON8_VSYS_STAT_SHIFT
	);

	sgm4154x_info("fault:%d, vbus_gd:%d, vdpm:%d, idpm:%d, acov:%d\n",
		data[SGM4154X_R09],
		(data[SGM4154X_R0A] & (CONA_VBUS_GD_MASK << CONA_VBUS_GD_SHIFT)) >>
				CONA_VBUS_GD_SHIFT,
		(data[SGM4154X_R0A] & (CONA_VINDPM_STAT_MASK << CONA_VINDPM_STAT_SHIFT)) >>
					CONA_VINDPM_STAT_SHIFT,
		(data[SGM4154X_R0A] & (CONA_IDPM_STAT_MASK << CONA_IDPM_STAT_SHIFT)) >>
					CONA_IDPM_STAT_SHIFT,
		(data[SGM4154X_R0A] & (CONA_ACOV_STAT_MASK << CONA_ACOV_STAT_SHIFT)) >>
					CONA_ACOV_STAT_SHIFT
	);

#if 0
	if (chip->component_id == SGM4154XAD_COMPINENT_ID ||
			chip->component_id == SGM4154XAD1_COMPINENT_ID) {
		for (i = vbat_channel; i < max_channel; i++)
			sgm4154x_get_adc_value(chip, i);
	}
#endif
	return 0;
}
static int32_t sgm4154x_set_dpdm(
	struct sgm4154x_chip *chip, uint8_t dp_val, uint8_t dm_val)
{
	uint8_t data_reg = 0;
#if 0
	return 0;
	data_reg  = (dp_val & CONC_DP_VOLT_MASK) << CONC_DP_VOLT_SHIFT;
	data_reg |= (dm_val & CONC_DM_VOLT_MASK) << CONC_DM_VOLT_SHIFT;
	//pr_info("data_reg 0x%02X\n", data_reg);
	return sgm4154x_update(chip, SGM4154X_R0C, data_reg, CONC_DP_DM_MASK, CONC_DP_DM_SHIFT);
#endif
	data_reg  = (dp_val & CONC_DP_VOLT_MASK) << (CONC_DP_VOLT_SHIFT+1);
	data_reg |= (dm_val & CONC_DM_VOLT_MASK) << (CONC_DM_VOLT_SHIFT+1);
	//pr_info("data_reg 0x%02X\n", data_reg);
	return sgm4154x_update(chip, SGM4154X_R0D, data_reg, CONC_DP_DM_MASK, CONC_DP_DM_SHIFT);

}

static int32_t sgm4154x_charging_set_hvdcp20(
		struct sgm4154x_chip *chip, uint32_t vbus_target);
static int sgm4154x_bc12_postprocess(struct sgm4154x_chip *chip,bool force)
{
	int ret = 0;
	bool attach = false, inform_psy = true, usbdev_state = false;
	u8 port = SGM4154X_PORTSTAT_NO_INPUT;
        /*
	if (chip->component_id != SGM4154XD_COMPINENT_ID &&
				chip->component_id != SGM4154XAD_COMPINENT_ID &&
				chip->component_id != SGM4154XAD1_COMPINENT_ID)
		return -ENODEV;
	*/
	attach = atomic_read(&chip->vbus_gd);
	if (!force && chip->attach == attach) {
		sgm4154x_dbg("attach(%d) is the same\n", attach);
		inform_psy = !attach;
		goto out;
	}
	chip->attach = attach;
//	dev_info(chip->dev, "attach = %d\n", attach);
	sgm4154x_set_vlimit(chip, DEFAULT_VLIMIT);

	if (!attach) {
		chip->port = SGM4154X_PORTSTAT_NO_INPUT;
		chip->chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
//		sgm4154x_set_charger_en(chip, 0);
		sgm4154x_extcon_clear_types(chip);
		usbdev_state = false;
		goto out;
	}

	ret = sgm4154x_read_byte(chip, SGM4154X_R08, &port);
	sgm4154x_dbg("sgm4154x_bc12_postprocess SGM4154X_R08 >>>>>> (%d)\n", port);
	
	if (ret < 0)
		chip->port = SGM4154X_PORTSTAT_NO_INPUT;
	else
		chip->port = (port & (CON8_VBUS_STAT_MASK << CON8_VBUS_STAT_SHIFT)) >>
					CON8_VBUS_STAT_SHIFT;

	sgm4154x_info("Chg Type [%s]\n", vbus_type[chip->port]);

	switch (chip->port) {
	case SGM4154X_PORTSTAT_NO_INPUT:
		chip->chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
//		sgm4154x_set_charger_en(chip, 0);
		sgm4154x_extcon_clear_types(chip);
		break;
	case SGM4154X_PORTSTAT_SDP:
		chip->chg_type = POWER_SUPPLY_TYPE_USB;
		sgm4154x_set_input_curr_lim(chip, 500);						
		sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_SDP, true);
		break;
	case SGM4154X_PORTSTAT_CDP:
		chip->chg_type = POWER_SUPPLY_TYPE_USB_CDP;
		sgm4154x_set_input_curr_lim(chip, 1500);						
		sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_CDP, true);
		break;
	case SGM4154X_PORTSTAT_DCP:
		chip->chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		sgm4154x_set_input_curr_lim(chip, 2000);						
		sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_DCP, true);
		break;
	case SGM4154X_PORTSTAT_HVDCP:
		chip->chg_type = POWER_SUPPLY_TYPE_USB_HVDCP;
//		sgm4154x_set_vlimit(chip, 8500);
		sgm4154x_charging_set_hvdcp20(chip, 5);
//		msleep(1800);
		sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_FAST, true);
//		sgm4154x_charging_set_hvdcp20(chip, 9);
//		msleep(200);
		break;
	case SGM4154X_PORTSTAT_NON_STANDARD:
	case SGM4154X_PORTSTAT_UNKNOWN:
		chip->chg_type = POWER_SUPPLY_TYPE_USB_FLOAT;
//		sgm4154x_set_input_curr_lim(chip, 500);						
		sgm4154x_extcon_notify(chip, JLQ_EXTCON_CHG_USB_FLOAT, true);
		if (chip->unkown_type_retry_cnt >= ARRAY_SIZE(unkown_type_retry_intvs) - 1)
			chip->unkown_type_retry_cnt =  ARRAY_SIZE(unkown_type_retry_intvs) - 1;
		schedule_delayed_work(&chip->unkown_type_work,
				msecs_to_jiffies(unkown_type_retry_intvs[chip->unkown_type_retry_cnt] * 1000));
		chip->unkown_type_retry_cnt++;
		break;
//	case SGM4154X_PORTSTAT_NON_STANDARD:
//		chip->chg_type = POWER_SUPPLY_TYPE_USB;
//		sgm4154x_set_input_curr_lim(chip, 2000);						
//		sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_SDP, true);
//		usbdev_state = true;
//		break;
	case SGM4154X_PORTSTAT_OTG:
		break;
	default:
		chip->chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	}
out:
	if (chip->chg_type == POWER_SUPPLY_TYPE_USB ||
		chip->chg_type == POWER_SUPPLY_TYPE_USB_CDP)
		usbdev_state = true;
	sgm4154x_extcon_notify(chip, EXTCON_USB, usbdev_state);
//	extcon_sync(chip->edev, EXTCON_CHG_USB_SDP);
	if (inform_psy)
		schedule_delayed_work(&chip->psy_dwork, 0);

	return 0;
}
#ifdef TRIGGER_CHARGE_TYPE_DETECTION
static void sgm4154x_chg_type_hvdcp_work(struct work_struct *work)
{
	struct sgm4154x_chip *chip =
		container_of(work, struct sgm4154x_chip, hvdcp_work.work);
//	int32_t ret;
	bool is_hvdcp = false;

	//chip->hvdcp_deting = false;

	if (!atomic_read(&chip->vbus_gd))
		goto done;
		
	//ret = sgm4154x_read_byte(chip, SGM4154X_R0D, &temp2);
	//temp2 = temp2&0x1E;

	sgm4154x_charging_set_hvdcp20(chip, 9);
	is_hvdcp = sgm4154x_is_hvdcp(chip, HVDCP_QC_9V);
	if (false == is_hvdcp)
		goto done;	
	#if 0
    ret = sgm4154x_read_byte(chip, SGM4154X_R06, &temp);
	temp = temp&0x0F;
	if(ret < 0)
		return ;
	ret = sgm4154x_read_byte(chip, SGM4154X_R0F, &temp3);
	temp3 =  temp3&0x03;
	if(ret < 0)
		return ;
	#endif	
//	if(ret < 0)
//		return -1;
		
	//ret = sgm4154x_update(chip, SGM4154X_R0A, 1, CONA_VINDPM_INT_MASK, CONA_VINDPM_INT_SHIFT);
//	if(ret < 0)
//		return -1;	
	#if 0		
	//sgm4154x_set_vlimit(chip, 8000);//mV		
	//msleep(300);
	do {
		ret = sgm4154x_read_byte(chip, SGM4154X_R0A,&temp1);
		sgm4154x_dbg("sgm4154x_chg_type_hvdcp_work SGM4154X_R0A >>>>>> (%d)\n", temp1);
		if (ret < 0 || tmout >= 3) {
			sgm4154x_err("Read Vbus voltage Failed.");
			goto done;
		}
		if(!((temp1>>CONA_VINDPM_STAT_SHIFT)&CONA_VINDPM_STAT_MASK)){
//			sgm4154x_write_byte(chip, SGM4154X_R06, temp);
//			sgm4154x_write_byte(chip, SGM4154X_R0F, temp3);
//			sgm4154x_write_byte(chip, SGM4154X_R0D, temp2);
//			sgm4154x_update(chip, SGM4154X_R06, temp, SGM4154x_VINDPM_V_MASK, 0);
//			sgm4154x_update(chip, SGM4154X_R0F, temp3, SGM4154x_VINDPM_OS_MASK, 0);
//			sgm4154x_update(chip, SGM4154X_R0D, temp2, SGM4154x_DP_VSEL_MASK|SGM4154x_DM_VSEL_MASK, 1);
			//sgm4154x_set_input_curr_lim(chip, 2000);						
			break;
		}
		else if(tmout > 3)
			goto done;
		
		
		tmout++;
		msleep(10);
	
	}while(ret < 0 && tmout <= 3);
	#endif
	sgm4154x_set_input_curr_lim(chip, 2000);
#if 0	
	do {
		ret = sgm4154x_get_adc_value(chip, vbus_channel);
		if (ret < 0 && tmout >= 3) {
			sgm4154x_err("Read Vbus voltage Failed.");
			goto done;
		}
		tmout++;
	} while(ret < 0 && tmout <= 3);
	if (ret < 8400 || ret > 9500) {
		sgm4154x_dbg("Set 9V.Not in Rangs[%dmv]", ret);
		sgm4154x_charging_set_hvdcp20(chip, 0);
		goto done;
	}
#endif

	sgm4154x_dbg(">>>>>>>>>>>>>>>>> HVDCP detect Done.");
//	sgm4154x_charging_set_hvdcp20(chip, 9);
	chip->chg_type = POWER_SUPPLY_TYPE_USB_HVDCP;
	chip->port = SGM4154X_PORTSTAT_HVDCP;
	sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_FAST, true);
	sgm4154x_extcon_notify(chip, EXTCON_CHG_USB_DCP, false);
	sgm4154x_psy_notify(chip);
done:
	chip->hvdcp_deting = false;
//	sgm4154x_write_byte(chip, SGM4154X_R06, temp);
//	sgm4154x_write_byte(chip, SGM4154X_R0F, temp3);
//	sgm4154x_write_byte(chip, SGM4154X_R0D, temp2);
//	sgm4154x_update(chip, SGM4154X_R06, temp, SGM4154x_VINDPM_V_MASK, 0);
//	sgm4154x_update(chip, SGM4154X_R0F, temp3, SGM4154x_VINDPM_OS_MASK, 0);
//	sgm4154x_update(chip, SGM4154X_R0D, temp2, SGM4154x_DP_VSEL_MASK|SGM4154x_DM_VSEL_MASK, 1);
	pm_relax(chip->dev);
}

static int32_t sgm4154x_chg_type_det(struct sgm4154x_chip *chip)
{
	int32_t ret = 0;
	uint8_t val = 0;
	int32_t wait_cnt = 0;
	uint8_t chg_type = 0;

	mutex_lock(&chip->bc12_lock);
	if (chip->hvdcp_deting) {
		ret = -EBUSY;
		goto out;
	}
	//enable hvdcp, set dp dm  hiz
	sgm4154x_write_byte(chip, SGM4154X_R0D, 0x00);

	ret = sgm4154x_update(chip, SGM4154X_R07, 1, FORCE_IINDET_MASK, FORCE_IINDET_SHIFT);
	if (ret < 0)
		goto out;
	do {
		msleep(10);
		wait_cnt++;
		ret = sgm4154x_read_byte(chip, SGM4154X_R07, &val);
		if (ret < 0)
			goto out;

	} while ((val & (FORCE_IINDET_MASK << FORCE_IINDET_SHIFT)) && wait_cnt < 300);
//	dev_info(chip->dev, "wait cnt %d\n", wait_cnt);
	if (wait_cnt < 300) {
		ret = sgm4154x_read_byte(chip, SGM4154X_R08, &val);
		if (ret < 0)
			goto out;

		chg_type = (val & (CON8_VBUS_STAT_MASK << CON8_VBUS_STAT_SHIFT)) >>
				CON8_VBUS_STAT_SHIFT;
		if (chg_type == SGM4154X_PORTSTAT_DCP) {
			if (chip->hvdcp_deting)
				cancel_delayed_work(&chip->hvdcp_work);
			else
				pm_stay_awake(chip->dev);
			chip->hvdcp_deting = true;
			ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_HIZ);
			sgm4154x_dbg("Start HVDCP deting\n");
			schedule_delayed_work(&chip->hvdcp_work, msecs_to_jiffies(1400));
			ret = 0;
		}
		sgm4154x_info(">>>>>>>>>>>>>>> chg_type %s\n", vbus_type[chg_type]);
	} else {
		ret = -ETIMEDOUT;
		goto out;
	}
out:
	mutex_unlock(&chip->bc12_lock);
	return ret;
}

static int sgm4154x_bc12_en_kthread(void *data)
{
	int ret = 0;
	struct sgm4154x_chip *chip = data;
	while (1) {
		wait_event(chip->bc12_en_req,
			atomic_read(&chip->bc12_en_req_cnt) > 0 || kthread_should_stop());

		if (atomic_read(&chip->bc12_en_req_cnt) <= 0 && kthread_should_stop()) {
			dev_info(chip->dev, "Exit!\n");
			return 0;
		}
		atomic_dec(&chip->bc12_en_req_cnt);

		pm_stay_awake(chip->dev);
		sgm4154x_extcon_clear_types(chip);
		msleep(500);
		ret = sgm4154x_set_en_hiz(chip, false);
		if (ret < 0)
			dev_info(chip->dev, "dis hz fail(%d)\n", ret);

		ret = sgm4154x_chg_type_det(chip);
		if (ret < 0)
			dev_info(chip->dev, "fail(%d)\n", ret);
		if (ret != -EBUSY)
			sgm4154x_bc12_postprocess(chip,true);

		pm_relax(chip->dev);
	}

	return 0;
}

int32_t sgm4154x_charge_type_detection(struct sgm4154x_chip *chip)
{
	/*
	if (chip->component_id != SGM4154XD_COMPINENT_ID &&
				chip->component_id != SGM4154XAD_COMPINENT_ID &&
				chip->component_id != SGM4154XAD1_COMPINENT_ID)
		return -ENODEV;
	*/

	if (atomic_read(&chip->vbus_gd)) {
		atomic_inc(&chip->bc12_en_req_cnt);
		wake_up(&chip->bc12_en_req);
	}
	return 0;
}
#endif

static void sgm4154x_inform_psy_dwork_handler(struct work_struct *work)
{
	struct sgm4154x_chip *chip = container_of(work, struct sgm4154x_chip, psy_dwork.work);
	bool attach = false;
	enum power_supply_type chg_type = POWER_SUPPLY_TYPE_UNKNOWN;

	mutex_lock(&chip->bc12_lock);
	attach = chip->attach;
	chg_type = chip->chg_type;
	mutex_unlock(&chip->bc12_lock);

	sgm4154x_info("attach = %d, type = %d\n", attach, chg_type);
	sgm4154x_psy_notify(chip);
}

static void sgm4154x_unkown_type_dwork_handler(struct work_struct *work)
{
	struct sgm4154x_chip *chip = container_of(work, struct sgm4154x_chip, unkown_type_work.work);
	pm_stay_awake(chip->dev);
#ifdef TRIGGER_CHARGE_TYPE_DETECTION
	sgm4154x_charge_type_detection(chip);
#endif
	pm_relax(chip->dev);
}
static int32_t sgm4154x_charging_set_hvdcp20(
		struct sgm4154x_chip *chip, uint32_t vbus_target)
{
	int32_t ret = 0;
	uint32_t vlimit_mv;

	if (!mutex_trylock(&chip->bc12_lock))
		return -EBUSY;
	if(chip->port == SGM4154X_PORTSTAT_DCP)
		vlimit_mv = DEFAULT_DCP_VLIMIT;
	else
		vlimit_mv = DEFAULT_VLIMIT;
	sgm4154x_info("Set vbus target %dv\n", vbus_target);
	//dump_stack();
	switch (vbus_target) {
	case 5:
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_0P0);
		sgm4154x_set_vlimit(chip, vlimit_mv);
		break;
	case 9:
		//sgm4154x_set_vlimit(chip, 9000 - chip->vlimit_offset_mv);
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_3P3, CONC_DP_DM_VOL_0P6);
		break;
	case 12:
		sgm4154x_set_vlimit(chip, 12000 - chip->vlimit_offset_mv);
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_0P6);
		break;
	default:
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_HIZ, CONC_DP_DM_VOL_HIZ);
		sgm4154x_set_vlimit(chip, vlimit_mv);
		break;
	}
	mutex_unlock(&chip->bc12_lock);
	return (ret < 0) ? ret : 0;
}

int32_t sgm4154x_charging_enable_hvdcp30(struct sgm4154x_chip *chip, bool enable)
{
//	sgm4154x_dbg("enable %d\n", enable);
	//enter continuous mode DP 0.6, DM 3.3
	return sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_3P3);
}

int32_t sgm4154x_charging_set_hvdcp30(struct sgm4154x_chip *chip, bool increase)
{
	int32_t ret = 0;

	mutex_lock(&chip->bc12_lock);
//	sgm4154x_dbg("increase %d\n", increase);
	if (increase) {
		//DP 3.3, DM 3.3
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_3P3, CONC_DP_DM_VOL_3P3);
		if (ret < 0)
			goto out;
		//need test
		msleep(100);
		//DP 0.6, DM 3.3
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_3P3);
		if (ret < 0)
			goto out;
		msleep(100);
	} else {
		//DP 0.6, DM 3.3
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_0P6);
		if (ret < 0)
			goto out;
		//need test
		msleep(100);
		//DP 0.6, DM 3.3
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_0P6, CONC_DP_DM_VOL_3P3);
		if (ret < 0)
			goto out;
		msleep(100);
	}
out:
	mutex_unlock(&chip->bc12_lock);
	return 0;
}

#ifndef SD7601AD2_COMPINENT_ID
#define SD7601_COMPINENT_ID		0x0100
#define SD7601D_COMPINENT_ID		0x01A3
#define SD7601AD_COMPINENT_ID		0x0100
#define SD7601AD1_COMPINENT_ID		0x02A1
#define SD7601AD2_COMPINENT_ID		0x02A2
#endif


#define SGM4154x_PN_41542_ID    (BIT(6)| BIT(5)| BIT(3))

static int32_t sgm4154x_hw_component_detect(struct sgm4154x_chip *chip)
{
	uint8_t chip_id = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, SGM4154X_R0B, &chip_id);
	if (ret < 0)
		return ret;

	chip->component_id = chip_id;
	
	if ((chip->component_id & SGM4154x_PN_MASK) != SGM4154x_PN_41542_ID){
		pr_info("[%s] device not found !!!\n", __func__);
		return ret;
	}
	
	return 0;
}

static void charging_base_hw_init(struct sgm4154x_chip *chip)
{
	//sgm4154x_set_charger_en(chip, 0);
	//set ilimit 500ma as default
	sgm4154x_write_byte(chip, SGM4154X_R00, 0x0A);
		//set as default, chg enable, vsys 3500mv, otg 2.8v falling
	sgm4154x_write_byte(chip, SGM4154X_R01, 0x0A);
	//set ichg 2040ma as default
	sgm4154x_write_byte(chip, SGM4154X_R02, 0xA2);
	//set pre_chg and iterm current 180ma
	sgm4154x_write_byte(chip, SGM4154X_R03, 0x22);
	//set as default, cv 4208mv
	sgm4154x_write_byte(chip, SGM4154X_R04, 0x78);
	//enable iterm, disable wdt, enable charge timer, chg timer 10hrs, jeita 20% ichg
	sgm4154x_write_byte(chip, SGM4154X_R05, 0x8F);
	//set vindpm 4500mv as default
	sgm4154x_write_byte(chip, SGM4154X_R06, 0xE6);
	//set as default
	sgm4154x_write_byte(chip, SGM4154X_R07, 0x4C);
	/* sgm4154x_write_byte(chip, SGM4154X_R08, 0x00);
	sgm4154x_write_byte(chip, SGM4154X_R09, 0x00);  */
	//disable iindpm interrupt & Vindpm interrupt
	sgm4154x_write_byte(chip, SGM4154X_R0A, 0x03);
	//sgm4154x_write_byte(chip, SGM4154X_R0B, 0x64);
	sgm4154x_write_byte(chip, SGM4154X_R0C, 0x75);
	sgm4154x_write_byte(chip, SGM4154X_R0D, 0x00);
	//sgm4154x_write_byte(chip, SGM4154X_R0E, 0x00);
	sgm4154x_write_byte(chip, SGM4154X_R0F, 0x00);

	sgm4154x_set_en_hiz(chip, 0);
	sgm4154x_set_vlimit(chip, DEFAULT_VLIMIT);
//	sgm4154x_set_input_curr_lim(chip, chip->ilimit_ma);
	sgm4154x_set_wd_reset(chip);

	return;
}

static void charging_profile_hw_init(struct sgm4154x_chip *chip)
{
	sgm4154x_set_sys_min_volt(chip, chip->vsysmin_mv);
	sgm4154x_set_iprechg_current(chip, chip->pre_ma);
	sgm4154x_set_iterm(chip, chip->eoc_ma);
	sgm4154x_set_rechg_volt(chip, chip->rechg_mv);
	sgm4154x_set_en_term_chg(chip, 1);
	//sgm4154x_set_wd_timer(chip, 0);
	sgm4154x_set_en_chg_timer(chip, 1);

	if (chip->vbus_bypass) {
		chip->discard = 1;
		sgm4154x_set_en_hiz(chip, 1);
		sgm4154x_set_charger_en(chip, 0);
	}
	sgm4154x_dump_msg(chip);
}
static void charging_hw_init(struct sgm4154x_chip *chip)
{
#if 0
	//sgm4154x_set_charger_en(chip, 0);
	//set ilimit 500ma as default
	sgm4154x_write_byte(chip, SGM4154X_R00, 0x0A);
		//set as default, chg enable, vsys 3500mv, otg 2.8v falling
	sgm4154x_write_byte(chip, SGM4154X_R01, 0x0A);
	//set ichg 2040ma as default
	sgm4154x_write_byte(chip, SGM4154X_R02, 0xA2);
	//set pre_chg and iterm current 180ma
	sgm4154x_write_byte(chip, SGM4154X_R03, 0x22);
	//set as default, cv 4208mv
	sgm4154x_write_byte(chip, SGM4154X_R04, 0x78);
	//enable iterm, disable wdt, enable charge timer, chg timer 10hrs, jeita 20% ichg
	sgm4154x_write_byte(chip, SGM4154X_R05, 0x8F);
	//set vindpm 4500mv as default
	sgm4154x_write_byte(chip, SGM4154X_R06, 0xE6);
	//set as default
	sgm4154x_write_byte(chip, SGM4154X_R07, 0x4C);
	/* sgm4154x_write_byte(chip, SGM4154X_R08, 0x00);
	sgm4154x_write_byte(chip, SGM4154X_R09, 0x00);  */
	//disable iindpm interrupt & Vindpm interrupt
	sgm4154x_write_byte(chip, SGM4154X_R0A, 0x03);
	//sgm4154x_write_byte(chip, SGM4154X_R0B, 0x64);
	sgm4154x_write_byte(chip, SGM4154X_R0C, 0x75);
	sgm4154x_write_byte(chip, SGM4154X_R0D, 0x00);
	//sgm4154x_write_byte(chip, SGM4154X_R0E, 0x00);
	sgm4154x_write_byte(chip, SGM4154X_R0F, 0x00);

	sgm4154x_set_en_hiz(chip, 0);
	sgm4154x_set_vlimit(chip, DEFAULT_VLIMIT);
//	sgm4154x_set_input_curr_lim(chip, chip->ilimit_ma);
	sgm4154x_set_wd_reset(chip);
#endif
	sgm4154x_set_sys_min_volt(chip, chip->vsysmin_mv);
	sgm4154x_set_iprechg_current(chip, chip->pre_ma);
	sgm4154x_set_iterm(chip, chip->eoc_ma);
	sgm4154x_set_vreg_volt(chip, chip->cv_mv);
//	sgm4154x_set_ichg_current(chip, chip->cc_ma);
	sgm4154x_set_rechg_volt(chip, chip->rechg_mv);
  /*	sgm4154x_set_ir_comp(chip, chip->bat_ir_mohm); */
/*	sgm4154x_set_comp_max(chip, chip->vchg_comp_max_mv); */
	sgm4154x_set_en_term_chg(chip, 1);
	//sgm4154x_set_wd_timer(chip, 0);
	sgm4154x_set_en_chg_timer(chip, 1);
	sgm4154x_set_otg_en(chip, 0);

	if (chip->vbus_bypass) {
		sgm4154x_set_en_hiz(chip, 1);
		sgm4154x_set_charger_en(chip, 0);
		chip->discard = 1;
	}
	sgm4154x_dump_msg(chip);
}

static void sgm4154x_work_func(struct work_struct *work)
{
	struct sgm4154x_chip *chip = container_of(work, struct sgm4154x_chip, sgm4154x_work.work);
	sgm4154x_set_wd_timer(chip,0);
	sgm4154x_dump_msg(chip);
	schedule_delayed_work(&chip->sgm4154x_work, 10*HZ);
}

static void sgm4154x_battery_profile_parse(struct work_struct *work)
{
	struct sgm4154x_chip *chip = container_of(work, struct sgm4154x_chip, sgm4154x_batt_pro_work.work);
	struct power_supply *main_psy;
	//struct jlq_charger_manager *jcm;
	struct device_node *np;
	int32_t ret = 0;

	main_psy = power_supply_get_by_name(chip->battery_profile_psy);
	if (!main_psy) {
	//	dev_info(chip->dev, "Not Find battery psy-node(%s),retry!.", chip->battery_profile_psy);
		schedule_delayed_work(&chip->sgm4154x_batt_pro_work, 2*HZ);
		return ;
	}
	//jcm = (struct jlq_charger_manager *)main_psy->drv_data;

//	if (jcm->best_battery_nd) {
	if (main_psy->of_node) {
		np = main_psy->of_node;
		ret = of_property_read_u32(np, "fastchg-current-ua", &chip->cc_ma);
		if (ret < 0)
			chip->cc_ma = DEFAULT_CC;
		else
			chip->cc_ma /= 1000;
		//cv
		ret = of_property_read_u32(np, "fastchg-volt-uv", &chip->cv_mv);
		if (ret < 0)
			chip->cv_mv = DEFAULT_CV;
		else
			chip->cv_mv /= 1000;
		//pre ma
		ret = of_property_read_u32(np, "precharge-ma", &chip->pre_ma);
		if (ret < 0)
			chip->pre_ma = DEFAULT_IPRECHG;
		//eoc ma
		ret = of_property_read_u32(np, "term-current-ma", &chip->eoc_ma);
		//vsys min mv
		if (ret < 0)
			chip->eoc_ma = DEFAULT_ITERM;
		ret = of_property_read_u32(np, "bigm,vsysmin_mv", &chip->vsysmin_mv);
		if (ret < 0)
		chip->vsysmin_mv = DEFAULT_VSYS_MIN;
		ret = of_property_read_u32(np, "batt-ir-mohm", &chip->bat_ir_mohm);
		if (ret < 0)
			chip->bat_ir_mohm = DEFAULT_IR_MHOM;
		ret = of_property_read_u32(np, "vchg-comp-max-mv", &chip->vchg_comp_max_mv);
		if (ret < 0)
			chip->vchg_comp_max_mv = DEFAULT_COMP_MAX;
	//recharge voltage
		ret = of_property_read_u32(np, "recharge-drop-mv", &chip->rechg_mv);
		if (ret < 0)
			chip->rechg_mv = DEFAULT_RECHG;

		charging_profile_hw_init(chip);

		dev_info(chip->dev, "ilimit:%dma, VlimitOffset:%dmv, cc:%dma, cv:%dmv, pre:%dma, eoc:%dma\n",
					chip->ilimit_ma, chip->vlimit_offset_mv, chip->cc_ma,
					chip->cv_mv, chip->pre_ma, chip->eoc_ma);
		dev_info(chip->dev, "vsysmin:%dmv, vrechg:%dmv bat_ir:%dmohm vchg_comp:%dmv\n",
					chip->vsysmin_mv,chip->rechg_mv, chip->bat_ir_mohm,
					chip->vchg_comp_max_mv);
	}
	power_supply_put(main_psy);
#ifdef TRIGGER_CHARGE_TYPE_DETECTION
#if 1
	msleep(3000);
	sgm4154x_charge_type_detection(chip);
#else
		sgm4154x_chg_type_det(chip);
		sgm4154x_bc12_postprocess(chip,true);
#endif
#endif

}

static int32_t sgm4154x_parse_dt(struct sgm4154x_chip *chip, struct device *dev)
{
	int32_t ret = 0;
	struct device_node *np = dev->of_node;


	if (!np) {
		dev_err(chip->dev, "no of node\n");
		return -ENODEV;
	}
	if (of_property_read_string(np, "charger_name", &chip->chg_dev_name) < 0) {
		chip->chg_dev_name = "primary_chg";
		dev_info(chip->dev, "no charger name\n");
	}

	if (of_property_read_string(np, "psy-name", &chip->chg_psy_name) < 0) {
		chip->chg_psy_name = "sgm4154x";
		dev_info(chip->dev, "no psy name\n");
	}

	if (of_property_read_string(np, "chg-regulator", &chip->chg_regulator_name) < 0) {
		chip->chg_dev_name = "sgm4154x-chg";
		dev_info(chip->dev, "no chg-regulator name\n");
	}

	if (of_property_read_string(np, "input-regulator", &chip->input_regulator_name) < 0) {
		dev_info(chip->dev, "no input-regulator name\n");
	}

	if (of_property_read_string(np, "otg-regulator", &chip->otg_regulator_name) < 0) {
		dev_info(chip->dev, "no otg-regulator name\n");
	}

	//irq gpio
	ret = of_get_named_gpio(np, "bigm,irq_gpio", 0);
	if (ret < 0) {
		dev_info(chip->dev, "no bigm,irq_gpio(%d)\n", ret);
		chip->irq_gpio = -1;
	} else
		chip->irq_gpio = ret;
	//if config ce gpio, output low
	ret = of_get_named_gpio(np, "bigm,ce_gpio", 0);
	if (ret < 0) {
		dev_info(chip->dev, "no bigm,ce_gpio(%d)\n", ret);
		chip->ce_gpio = -1;
	} else
		chip->ce_gpio = ret;

	dev_info(chip->dev, "irq_gpio = %d, ce_gpio = %d\n", chip->irq_gpio, chip->ce_gpio);
#if 1
	if (-1 != chip->ce_gpio) {
		ret = devm_gpio_request_one(chip->dev, chip->ce_gpio,
			GPIOF_OUT_INIT_LOW, devm_kasprintf(chip->dev, GFP_KERNEL,
				"sgm4154x_ce_gpio.%s", dev_name(chip->dev)));
		if (ret < 0) {
			dev_err(chip->dev, "ce gpio request fail(%d)\n", ret);
			return ret;
		}
		//enable ic
		dev_info(chip->dev, "irq_gpio = %d, ce_gpio = %d inited.\n", chip->irq_gpio, chip->ce_gpio);
		gpio_set_value(chip->ce_gpio, 0);
	}
#endif

	//ilimit
	ret = of_property_read_u32(np, "bigm,ilimit_ma", &chip->ilimit_ma);
	if (ret)
		chip->ilimit_ma = DEFAULT_ILIMIT;
#if 0
	//vlimit
	ret = of_property_read_u32(np, "bigm,vlimit_mv", &chip->vlimit_mv);
	if (ret)
		chip->vlimit_mv = DEFAULT_VLIMIT;
#endif
	chip->vbus_bypass = of_property_read_bool(np, "bigm,vbus-bypass");
	ret = of_property_read_u32(np, "bigm,vlimit_offset_mv", &chip->vlimit_offset_mv);
	if (ret)
		chip->vlimit_offset_mv = DEFAULT_VLIMIT_OFFSET;

	chip->phy_regulator = devm_regulator_get(dev, "phy");
	if (IS_ERR_OR_NULL(chip->phy_regulator)) {
		dev_err(chip->dev, "Not set USB Phy Regulator .\n");
		chip->phy_regulator = NULL;
	} else if(regulator_is_enabled(chip->phy_regulator)) {
		regulator_disable(chip->phy_regulator);
	}

	chip->cc_ma = DEFAULT_CC;
	chip->cv_mv = DEFAULT_CV;
	chip->pre_ma = DEFAULT_IPRECHG;
	chip->eoc_ma = DEFAULT_ITERM;
	chip->vsysmin_mv = DEFAULT_VSYS_MIN;
	chip->rechg_mv = DEFAULT_RECHG;
	charging_base_hw_init(chip);

	if (of_property_read_string(np, "battery-profile-psy", &chip->battery_profile_psy) < 0) {
		dev_info(chip->dev, "no batter profile psy\n");
		//cc
		ret = of_property_read_u32(np, "bigm,cc_ma", &chip->cc_ma);
		if (ret < 0)
			chip->cc_ma = DEFAULT_CC;
		//cv
		ret = of_property_read_u32(np, "bigm,cv_mv", &chip->cv_mv);
		if (ret < 0)
			chip->cv_mv = DEFAULT_CV;
		//pre ma
		ret = of_property_read_u32(np, "bigm,pre_ma", &chip->pre_ma);
		if (ret < 0)
			chip->pre_ma = DEFAULT_IPRECHG;
		//eoc ma
		ret = of_property_read_u32(np, "bigm,eoc_ma", &chip->eoc_ma);
		if (ret < 0)
			chip->eoc_ma = DEFAULT_ITERM;
		//vsys min mv
		ret = of_property_read_u32(np, "bigm,vsysmin_mv", &chip->vsysmin_mv);
		if (ret < 0)
			chip->vsysmin_mv = DEFAULT_VSYS_MIN;
		//recharge voltage
		ret = of_property_read_u32(np, "bigm,rechg_mv", &chip->rechg_mv);
		if (ret < 0)
			chip->rechg_mv = DEFAULT_RECHG;
		ret = of_property_read_u32(np, "bigm,batt_ir_mohm", &chip->bat_ir_mohm);
		if (ret < 0)
			chip->bat_ir_mohm = DEFAULT_IR_MHOM;
		ret = of_property_read_u32(np, "bigm,vchg-comp-max-mv", &chip->vchg_comp_max_mv);
		if (ret < 0)
			chip->vchg_comp_max_mv = DEFAULT_COMP_MAX;

		charging_hw_init(chip);
		dev_info(chip->dev, "ilimit:%dma, VlimitOffset:%dmv, cc:%dma, cv:%dmv, pre:%dma, eoc:%dma\n",
					chip->ilimit_ma, chip->vlimit_offset_mv, chip->cc_ma,
					chip->cv_mv, chip->pre_ma, chip->eoc_ma);
		dev_info(chip->dev, "vsysmin:%dmv, vrechg:%dmv bat_ir:%dmohm vchg_comp:%dmv\n",
					chip->vsysmin_mv,chip->rechg_mv, chip->bat_ir_mohm,
					chip->vchg_comp_max_mv);
	} else {
		INIT_DELAYED_WORK(&chip->sgm4154x_batt_pro_work, sgm4154x_battery_profile_parse);
		schedule_delayed_work(&chip->sgm4154x_batt_pro_work, 4 * HZ);
	}

	return 0;
}
static ssize_t registers_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	uint8_t i = 0, data = 0;
	int32_t ret  = 0;
	struct sgm4154x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	for (i = 0; i < SGM4154X_REG_NUM; i++) {
		data = 0;
		ret = sgm4154x_read_byte(chip, i, &data);
		count += sprintf(&buf[count],
			"Reg[0x%02X] = 0x%02X %s\n", i, data, (ret < 0) ? "failed":"successfully");
	}
/*
	if (chip->component_id == SGM4154XAD_COMPINENT_ID ||
				chip->component_id == SGM4154XAD1_COMPINENT_ID) {
		ret = sgm4154x_update(chip, SGM4154X_R11, 1,
				CON11_CONV_RATE_MASK, CON11_CONV_RATE_SHIFT);
		if (ret < 0) {
			count += sprintf(&buf[count], "write R11 failed\n");
		} else {
			for (i = SGM4154X_R11; i <= SGM4154X_R17; i++) {
				data = 0;
				ret = sgm4154x_read_byte(chip, i, &data);
				count += sprintf(&buf[count],
					"Reg[0x%02X] = 0x%02X %s\n", i, data,
							(ret < 0) ? "failed":"successfully");
			}
		}
	}*/
	return count;
}

static ssize_t registers_store(
	struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	int32_t ret = 0;
	char *pvalue = NULL, *addr = NULL, *val = NULL;
	uint32_t reg_value = 0, reg_address = 0;
	struct sgm4154x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	if (NULL == buf || 6 != size)
		goto msg;

	dev_info(chip->dev, "buf is %s and size is %zu\n", buf, size);

	pvalue = (char *)buf;
	addr = strsep(&pvalue, " ");
	val  = strsep(&pvalue, " ");

	ret = kstrtou32(addr, 16, (uint32_t *)&reg_address);
	ret = kstrtou32(val, 16, (uint32_t *)&reg_value);

	dev_info(chip->dev, "write sgm4154x reg 0x%02X with value 0x%02X !\n",
				(uint32_t) reg_address, reg_value);

	sgm4154x_write_byte(chip, reg_address, reg_value);

	return size;

msg:
	dev_info(chip->dev, "exsample: echo 10 22 > registers\n");
	dev_info(chip->dev, "means   : write reg 0x10 with value 0x22!\n");
	return size;
}

static DEVICE_ATTR_RW(registers);	/* 0644 */

static int32_t	sgm4154x_extcon_clear_types(struct sgm4154x_chip *chip)
{
	extcon_set_state_sync(chip->edev, EXTCON_CHG_USB_CDP, false);
	extcon_set_state_sync(chip->edev, EXTCON_CHG_USB_DCP, false);
	extcon_set_state_sync(chip->edev, EXTCON_CHG_USB_FAST, false);
	extcon_set_state_sync(chip->edev, EXTCON_CHG_USB_SDP, false);
	extcon_set_state_sync(chip->edev, JLQ_EXTCON_CHG_USB_FLOAT, false);
	extcon_set_state_sync(chip->edev, EXTCON_USB, false);
	return 0;
}

static int32_t	sgm4154x_extcon_notify(struct sgm4154x_chip *chip, unsigned int extcon_id, bool data)
{
	if (chip->edev)
		return extcon_set_state_sync(chip->edev, extcon_id, data);
	dev_info(chip->dev, "Notfiy(%d) Extcon %d Failed(No edev)\n", extcon_id, data);
	return -1;
}

static int32_t	sgm4154x_psy_notify(struct sgm4154x_chip *chip)
{
	power_supply_changed(chip->sgm4154x_psy);
	return 0;
}

static int32_t	sgm4154x_default_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	sgm4154x_psy_notify(chip);
//	dev_info(chip->dev, "%d\n", data);
	return 0;
}

static int32_t sgm4154x_bat_fault_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	uint8_t ret = 0;
	uint8_t val = 0;

	if(1 == data)
	{
		// ensure bat ovp
		ret = sgm4154x_read_byte(chip,SGM4154X_R09,&val);
		if(ret < 0)
			goto ovp_out;
		if(val & (CON9_BAT_STAT_MASK << CON9_BAT_STAT_SHIFT))
		{
			sgm4154x_set_charger_en(chip, 0);
			sgm4154x_set_vreg_volt(chip, chip->cv_mv);
			sgm4154x_dbg("BAT-OVP occur.\n");
		}
	}
ovp_out:
	//clear value
	irq_handles[2].value &= ~(CON9_BAT_STAT_MASK << CON9_BAT_STAT_SHIFT);

	return ret;
}

static int32_t	sgm4154x_vlimit_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	int32_t input_curr_lim;
	int chg_stat;

	return 0;
	if (data) {
		chg_stat = sgm4154x_get_charger_en(chip);
		if (chg_stat)
			return 0;
		sgm4154x_set_charger_en(chip, 0);
		input_curr_lim =  sgm4154x_get_input_curr_lim(chip);
		if (input_curr_lim <= INPUT_CURRT_MIN)
			return 0;
		input_curr_lim -= INPUT_CURRT_STEP;
		sgm4154x_set_input_curr_lim(chip, input_curr_lim);
		msleep(20);
		sgm4154x_set_charger_en(chip, 1);
		dev_info(chip->dev, "Input Vlimt error. dec input current:%d.\n",input_curr_lim);
	}
	return 0;
}

static int32_t	sgm4154x_boost_fault_irq(struct sgm4154x_chip *chip,uint8_t data)
{
	int32_t ret = 0;
	uint8_t val = 0;
	uint8_t retry_times = 0;
	uint8_t check_times = 0;

	if (data == 1) {
		if (atomic_read(&chip->boost_fault_cnt) < 100) {
			for (retry_times = 0; retry_times < 10; retry_times++) {
				sgm4154x_set_otg_en(chip, 0);
				sgm4154x_set_otg_en(chip, 1);
						//check result
				for (check_times = 0; check_times < 5; check_times++) {
					ret = sgm4154x_read_byte(chip,SGM4154X_R09, &val);
					if (ret < 0)
						continue;
					if (0 == (val & (CON9_BOOST_STAT_MASK << CON9_BOOST_STAT_SHIFT)))
						goto br_out;
				}
				if (check_times < 5)
					break;
			}
		} else {
			dev_emerg(chip->dev,
				"Boost fault.too many times,please check OTG device!!! .\n");
			return 0;
		}
br_out:
		atomic_inc(&chip->boost_fault_cnt);
		dev_info(chip->dev, "Boost fault.recovery %s .\n",
		retry_times >=10 ? "FAILED" : "Success");
	}
	return 0;
}

#if 0
static int32_t	sgm4154x_vbus_state_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	dev_info(chip->dev, "%d %s\n", data, vbus_type[data]);
	mutex_lock(&chip->bc12_lock);
	chip->vbus_state = data;
	mutex_unlock(&chip->bc12_lock);
#ifdef TRIGGER_CHARGE_TYPE_DETECTION
			sgm4154x_chg_type_det(chip);
#endif
	sgm4154x_psy_notify(chip);
	return 0;
}
#endif

static int32_t	sgm4154x_chg_state_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	sgm4154x_info("%d -> [%s]\n", data, charge_state[data]);
	chip->chg_state = data;
	sgm4154x_psy_notify(chip);
	return 0;
}

static int32_t	sgm4154x_pg_state_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	int32_t ret = 0;
	/*
	if (chip->component_id != SGM4154XD_COMPINENT_ID &&
			chip->component_id != SGM4154XAD_COMPINENT_ID &&
			chip->component_id != SGM4154XAD1_COMPINENT_ID)
		return 0;
	*/
	sgm4154x_info("pg good %d\n", data);

	mutex_lock(&chip->bc12_lock);
	//plug out
	if (atomic_read(&chip->vbus_gd) && 0 == data)
		ret = sgm4154x_set_dpdm(chip, CONC_DP_DM_VOL_HIZ, CONC_DP_DM_VOL_HIZ);

	atomic_set(&chip->vbus_gd, data);
	mutex_unlock(&chip->bc12_lock);
	sgm4154x_bc12_postprocess(chip,false);
	sgm4154x_psy_notify(chip);

	return ret;
}

static int32_t	sgm4154x_vbus_good_irq(struct sgm4154x_chip *chip, uint8_t data)
{
	int32_t ret = 0;

	sgm4154x_info("VbusGood data %d\n", data);
	//mutex_lock(&chip->bc12_lock);
	atomic_set(&chip->vbus_gd, data);
	//mutex_unlock(&chip->bc12_lock);
	cancel_delayed_work(&chip->unkown_type_work);
	chip->unkown_type_retry_cnt = 0;
	sgm4154x_psy_notify(chip);
	if (data) {
		if (chip->phy_regulator) {
			sgm4154x_info("regulator_enable phy_regulator\n");
			/*
			* some phy may enable regulator to keep D+D- voltage work in normal.
			*/
			regulator_enable(chip->phy_regulator);
		}
	} else {
		if (chip->hvdcp_deting) {
			chip->hvdcp_deting = false;
			cancel_delayed_work(&chip->hvdcp_work);
			pm_relax(chip->dev);
		}
		if (chip->phy_regulator) {
			sgm4154x_info("regulator_disable phy_regulator\n");
			regulator_disable(chip->phy_regulator);
		}
	}

#ifdef TRIGGER_CHARGE_TYPE_DETECTION
	if (data)
		sgm4154x_chg_type_det(chip);
#endif
	if (!data) {
		sgm4154x_set_vlimit(chip, DEFAULT_VLIMIT);
		sgm4154x_extcon_clear_types(chip);
	}
	return ret;
}

static struct sgm4154x_irq_handle irq_handles[] = {
{
	SGM4154X_R0A, 0, 0,{
		{
		.irq_name	 = "vbus good",
		.irq_func	 = sgm4154x_vbus_good_irq,
		.irq_mask	= CONA_VBUS_GD_MASK,
		.irq_shift	 = CONA_VBUS_GD_SHIFT,
		},
		{
		.irq_name	 = "vdpm",
		.irq_func	 = sgm4154x_vlimit_irq,
		.irq_mask	= CONA_VINDPM_STAT_MASK,
		.irq_shift	 = CONA_VINDPM_STAT_SHIFT,
		},
		{
		.irq_name	 = "idpm",
		.irq_func	 = sgm4154x_default_irq,
		.irq_mask	= CONA_IDPM_STAT_MASK,
		.irq_shift	 = CONA_IDPM_STAT_SHIFT,
		},
		{
		.irq_name	 = "topoff active",
		.irq_func	 = sgm4154x_default_irq,
		.irq_mask	= CONA_TOPOFF_ACTIVE_MASK,
		.irq_shift	 = CONA_TOPOFF_ACTIVE_SHIFT,
		},
		{
		.irq_name	 = "acov state",
		.irq_func	 = sgm4154x_default_irq,
		.irq_mask	= CONA_ACOV_STAT_MASK,
		.irq_shift	 = CONA_ACOV_STAT_SHIFT,
		},
	},
},

{
	SGM4154X_R08, 0, 0,
	{
#if 0
		{
			.irq_name	 = "vbus state",
			.irq_func	 = sgm4154x_vbus_state_irq,
			.irq_mask   = CON8_VBUS_STAT_MASK,
			.irq_shift	 = CON8_VBUS_STAT_SHIFT,
		},
#endif
		{
			.irq_name	 = "chg state",
			.irq_func	 = sgm4154x_chg_state_irq,
			.irq_mask   = CON8_CHRG_STAT_MASK,
			.irq_shift	 = CON8_CHRG_STAT_SHIFT,
		},
		{
			.irq_name	 = "pg state",
			.irq_func	 = sgm4154x_pg_state_irq,
			.irq_mask   = CON8_PG_STAT_MASK,
			.irq_shift	 = CON8_PG_STAT_SHIFT,
		},
		{
			.irq_name	 = "therm state",
			.irq_func	 = sgm4154x_default_irq,
			.irq_mask   = CON8_THM_STAT_MASK,
			.irq_shift	 = CON8_THM_STAT_SHIFT,
		},
		{
			.irq_name	 = "vsys state",
			.irq_func	 = sgm4154x_default_irq,
			.irq_mask   = CON8_VSYS_STAT_MASK,
			.irq_shift	 = CON8_VSYS_STAT_SHIFT,
		},
	},
},
{
	SGM4154X_R09, 0, 0,
	{
		{
			.irq_name	 = "wathc dog fault",
			.irq_func	 = sgm4154x_default_irq,
			.irq_mask   = CON9_WATG_STAT_MASK,
			.irq_shift	 = CON9_WATG_STAT_SHIFT,
		},
		{
			.irq_name	 = "boost fault",
			.irq_func	 = sgm4154x_boost_fault_irq,
			.irq_mask   = CON9_BOOST_STAT_MASK,
			.irq_shift	 = CON9_BOOST_STAT_SHIFT,
		},
		{
			.irq_name	 = "chg fault",
			.irq_func	 = sgm4154x_default_irq,
			.irq_mask   = CON9_CHRG_FAULT_MASK,
			.irq_shift	 = CON9_CHRG_FAULT_SHIFT,
		},
		{
			.irq_name	 = "bat fault",
			.irq_func	 = sgm4154x_bat_fault_irq,
			.irq_mask   = CON9_BAT_STAT_MASK,
			.irq_shift	 = CON9_BAT_STAT_SHIFT,
		},
		{
			.irq_name	 = "ntc fault",
			.irq_func	 = sgm4154x_default_irq,
			.irq_mask   = CON9_NTC_STAT_MASK,
			.irq_shift	 = CON9_NTC_STAT_SHIFT,
		},
	},
},
};

static int32_t sgm4154x_process_irq(struct sgm4154x_chip *chip)
{
	int32_t i, j, ret = 0;
	uint8_t pre_state = 0, now_state = 0;
	int handler_count = 0;

	for (i = 0; i < ARRAY_SIZE(irq_handles); i++) {
		ret = sgm4154x_read_byte(chip, irq_handles[i].reg, &irq_handles[i].value);
		sgm4154x_info("sgm4154x_process_irq,reg 0x%02X, value now 0x%02X, value pre 0x%02X\n",
			irq_handles[i].reg, irq_handles[i].value, irq_handles[i].pre_value);

		if (ret < 0) {
			sgm4154x_info("Couldn't read reg 0x%02X ret = %d\n",
				irq_handles[i].reg, ret);
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(irq_handles[i].irq_info); j++) {
			now_state = irq_handles[i].value &
				(irq_handles[i].irq_info[j].irq_mask <<
					irq_handles[i].irq_info[j].irq_shift);
			pre_state = irq_handles[i].pre_value &
				(irq_handles[i].irq_info[j].irq_mask <<
					irq_handles[i].irq_info[j].irq_shift);

			if (now_state != pre_state) {
//				dev_info(chip->dev, "%s\n", irq_handles[i].irq_info[j].irq_name);

				if (irq_handles[i].irq_info[j].irq_func) {
					handler_count++;
					ret = irq_handles[i].irq_info[j].irq_func(chip,
						now_state >> irq_handles[i].irq_info[j].irq_shift);
					if (ret < 0)
						sgm4154x_warn("Couldn't handle %d irq for reg 0x%02X ret = %d\n",
							j, irq_handles[i].reg, ret);
				}
			}
		}
		irq_handles[i].pre_value = irq_handles[i].value;
	}

	return handler_count;
}

static irqreturn_t sgm4154x_irq_handler(int irq, void *data)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)data;

//	dev_info(chip->dev, "--------------------------\n");

	//wait for register modify
//	msleep(50);

	pm_stay_awake(chip->dev);
	sgm4154x_process_irq(chip);
	pm_relax(chip->dev);
//	dev_info(chip->dev, "========================\n");

	return IRQ_HANDLED;
}
int sgm4154x_otg_vbus_enable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	return sgm4154x_set_otg_en(chip, 1);
}

int sgm4154x_otg_vbus_disable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	return sgm4154x_set_otg_en(chip, 0);
}
int32_t sgm4154x_set_boost_ilim_1_2A_enable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	return sgm4154x_set_boost_ilim(chip, 2000);
}

int32_t sgm4154x_set_boost_ilim_1_2A_disable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	return sgm4154x_set_boost_ilim(chip, 2000);
}

int32_t sgm4154x_get_boost_ilim_1_2A_status(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	int32_t boost_ilim;

	boost_ilim  = sgm4154x_get_boost_ilim(chip);
	return boost_ilim == 2000 ? 1 : 0;
}

static int sgm4154x_enchg_enable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	sgm4154x_dbg("dev->name:%s enable_mask:%d,enable_reg:%d shift:%d",
			dev->desc->name, dev->desc->enable_mask,
			dev->desc->enable_reg, MASK_TO_SHIFT(dev->desc->enable_mask));
	return sgm4154x_set_charger_en(chip, 1);
}

static int sgm4154x_enchg_disable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	sgm4154x_dbg("dev->name:%s enable_mask:%d,enable_reg:%d shift:%d",
			dev->desc->name, dev->desc->enable_mask,
			dev->desc->enable_reg, MASK_TO_SHIFT(dev->desc->enable_mask));
	return sgm4154x_set_charger_en(chip, 0);
}

static int sgm4154x_regulator_enable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	sgm4154x_dbg("dev->name:%s enable_mask:%d,enable_reg:%d shift:%d",
			dev->desc->name, dev->desc->enable_mask,
			dev->desc->enable_reg, MASK_TO_SHIFT(dev->desc->enable_mask));
	return sgm4154x_update(chip, dev->desc->enable_reg, 1,
		dev->desc->enable_mask >> MASK_TO_SHIFT(dev->desc->enable_mask),
		MASK_TO_SHIFT(dev->desc->enable_mask));
}

static int sgm4154x_regulator_disable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	sgm4154x_dbg("dev->name:%s enable_mask:%d,enable_reg:%d shift:%d",
			dev->desc->name, dev->desc->enable_mask,
			dev->desc->enable_reg, MASK_TO_SHIFT(dev->desc->enable_mask));
	return sgm4154x_update(chip, dev->desc->enable_reg, 0,
		dev->desc->enable_mask >> MASK_TO_SHIFT(dev->desc->enable_mask),
		MASK_TO_SHIFT(dev->desc->enable_mask));
}

static int sgm4154x_regulator_is_enable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	uint8_t data_reg = 0;

	sgm4154x_read_byte(chip, dev->desc->enable_reg, &data_reg);
	sgm4154x_dbg("dev->name:%s enable_mask:0x%x,enable_reg:%d shift:%d :0x%x",
			dev->desc->name, dev->desc->enable_mask,
			dev->desc->enable_reg, MASK_TO_SHIFT(dev->desc->enable_mask), data_reg);
	return !!(data_reg & dev->desc->enable_mask) ;
}
#ifdef SGM4154X_OTG
static int sgm4154x_otg_reg_enable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	atomic_set(&chip->boost_fault_cnt, 0);
	return sgm4154x_regulator_enable(dev);
}

static int sgm4154x_otg_reg_disable(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	atomic_set(&chip->boost_fault_cnt, 0);
	return sgm4154x_regulator_disable(dev);
}
#endif
static int sgm4154x_regulator_set_cur_uA(
	struct regulator_dev *dev, int min_uA, int max_uA)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	int selector;

	if (min_uA >= dev->desc->curr_table[dev->desc->n_current_limits - 1]) {
		selector = dev->desc->n_current_limits - 1;
	} else if (min_uA <= dev->desc->curr_table[0]) {
		selector = 0;
	} else {
		for (selector = 0; selector < dev->desc->n_current_limits; selector++) {
			if (min_uA < dev->desc->curr_table[selector]) {
				selector--;
				break;
			}
		}
	}
	sgm4154x_dbg("dev->name:%s uA:%d.sel:%d",
		dev->desc->name, min_uA, selector);
	return sgm4154x_update(chip, dev->desc->csel_reg, selector,
				dev->desc->csel_mask >> MASK_TO_SHIFT(dev->desc->csel_mask),
				MASK_TO_SHIFT(dev->desc->csel_mask));
}

static int sgm4154x_regulator_get_cur_uA(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	uint8_t data_reg = 0;
	int32_t ret = 0;
	int selector;

	ret = sgm4154x_read_byte(chip, dev->desc->csel_reg, &data_reg);
	if (ret < 0)
		return ret;
	selector = (data_reg & dev->desc->csel_mask) >>
		MASK_TO_SHIFT(dev->desc->csel_mask);
	sgm4154x_dbg("dev->name:%s .sel:%d .uA:%d",
		dev->desc->name, selector, dev->desc->curr_table[selector]);
	return dev->desc->curr_table[selector];
}

static int sgm4154x_regulator_set_volt_sel(
	struct regulator_dev *dev, unsigned int selector)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);

	sgm4154x_dbg("dev->name:%s data_reg:[0x%x] Mask:%x selector:0x%x",
		dev->desc->name, dev->desc->vsel_reg, dev->desc->vsel_mask,
			selector);
	return sgm4154x_update(chip, dev->desc->vsel_reg, selector,
			dev->desc->vsel_mask >> MASK_TO_SHIFT(dev->desc->vsel_mask),
			MASK_TO_SHIFT(dev->desc->vsel_mask));
}

static int sgm4154x_regulator_get_volt_sel(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	uint8_t data_reg = 0;
	int32_t ret = 0;

	ret = sgm4154x_read_byte(chip, dev->desc->vsel_reg, &data_reg);
	if (ret < 0)
		return ret;
	sgm4154x_dbg("dev->name:%s data_reg:[0x%x]->%x Mask:%x selector:0x%x",
		dev->desc->name, dev->desc->vsel_reg, data_reg, dev->desc->vsel_mask,
			(data_reg & dev->desc->vsel_mask) >> MASK_TO_SHIFT(dev->desc->vsel_mask));
	return (data_reg & dev->desc->vsel_mask) >> MASK_TO_SHIFT(dev->desc->vsel_mask);
}

static int sgm4154x_regulator_set_vreg_sel(
	struct regulator_dev *dev, unsigned int selector)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	if(selector & 1)
	{
		selector += 1;
		sgm4154x_dbg("dev->name:%s selector:%d -> %d, update REG0F:%#x \n", dev->desc->name,(selector-1), selector, 3);
		// update REG0F
		sgm4154x_update(chip, SGM4154X_R0F, 3, 0x03 ,6);
	}
	
	sgm4154x_dbg("dev->name:%s selector:%d-> %dmv [R:0x%02x S:%d]:%d [R:0x%02x S:%d]:%d",
			dev->desc->name, selector,
			VREG_VOL_MIN + (selector * VREG_VOL_STEP / 2),
			SGM4154X_R04, CON4_VREG_SHIFT, selector >> 1,
			SGM4154X_R0D, COND_CV_SPP_SHIFT, selector & 1);

	return sgm4154x_update(chip, SGM4154X_R04, selector >> 1,
			CON4_VREG_MASK, CON4_VREG_SHIFT);
	//return sgm4154x_update(chip, SGM4154X_R0D, selector & 1,
			//COND_CV_SP_MASK, COND_CV_SPP_SHIFT);
}

static int sgm4154x_regulator_get_vreg_sel(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	uint8_t data_reg = 0;
	uint8_t data_reg1 = 0;
	int32_t ret = 0;
//	return 0;
	ret = sgm4154x_read_byte(chip, SGM4154X_R04, &data_reg);
	if (ret < 0)
		return ret;
	ret = sgm4154x_read_byte(chip, SGM4154X_R0F, &data_reg1);
	if (ret < 0)
		return ret;
	sgm4154x_dbg("dev->name:%s data_reg:[0x%02x]:%x data_reg:[0x%02x]:%x ",
		dev->desc->name, SGM4154X_R04, data_reg,SGM4154X_R0F, data_reg1);
	data_reg =(data_reg >> CON4_VREG_SHIFT) & CON4_VREG_MASK;
	data_reg <<= 1;
	data_reg1 = ((data_reg1 >> 6) == 0x03)?1:0;
	data_reg += data_reg1;
	sgm4154x_dbg("dev->name:%s selector:%d -> %dmv",
		dev->desc->name, data_reg,
		VREG_VOL_MIN + (data_reg * VREG_VOL_STEP / 2));
	return data_reg;
}


static int sgm4154x_regulator_set_vbus_input_volt(
	struct regulator_dev *dev, int min_uV, int max_uV, unsigned int *selector)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	unsigned int index;

	if (chip->port != SGM4154X_PORTSTAT_HVDCP && chip->port != SGM4154X_PORTSTAT_NO_INPUT)
		return 0;

	if (max_uV < 0)
		max_uV = 0;

	if (max_uV > 12000000)
		max_uV = 12000000;

	for (index = 0; index < dev->desc->n_voltages; index++) {
		if (max_uV < dev->desc->volt_table[index]) {
			index--;
			break;
		}
	}
	if (selector)
		*selector = index;

	sgm4154x_dbg("dev->name:%s Set:%d V", dev->desc->name, dev->desc->volt_table[index] / 1000000);
	return sgm4154x_charging_set_hvdcp20(chip, dev->desc->volt_table[index] / 1000000);
}

static int sgm4154x_regulator_get_vbus_input_volt(struct regulator_dev *dev)
{
	struct sgm4154x_chip *chip = (struct sgm4154x_chip *)rdev_get_drvdata(dev);
	//int32_t ret = 0;

/*	ret = sgm4154x_get_adc_value(chip, vbus_channel);
	if (ret < 0)
		return ret;*/
	sgm4154x_info("Not support sgm4154x_regulator_get_vbus_input_volt\n");
	return -EINVAL;
}

static struct regulator_init_data sgm4154x_input_initdata = {
	.constraints = {
		.name = "Iuput",
		.min_uV = 100000,
		.max_uV = 12000000,
		.min_uA = 100000,
		.max_uA = 3200000,
		.always_on = 1,
		.valid_modes_mask = REGULATOR_MODE_NORMAL,
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_CURRENT,
	},
};

static const int sgm4154x_input_volt_table[] = {
	100000, 5000000, 9000000, 12000000
};

static const int sgm4154x_input_current_limts[] = {
	100000, 200000, 300000, 400000, 500000,
	600000, 700000, 800000, 900000, 1000000,
	1100000, 1200000, 1300000, 1400000, 1500000,
	1600000, 1700000, 1800000, 1900000, 2000000,
	2100000, 2200000, 2300000, 2400000, 2500000,
	2600000, 2700000, 2800000, 2900000, 3000000,
	3100000, 3200000
};

static const struct regulator_ops sgm4154x_input_charger_ops = {
	.list_voltage = regulator_list_voltage_table,
	.set_voltage = sgm4154x_regulator_set_vbus_input_volt,
	.get_voltage = sgm4154x_regulator_get_vbus_input_volt,
	.set_current_limit = sgm4154x_regulator_set_cur_uA,
	.get_current_limit = sgm4154x_regulator_get_cur_uA,
//	.map_voltage = regulator_map_voltage_ascend,
//	.list_voltage = regulator_list_voltage_table,
};

static struct regulator_desc sgm4154x_input_charger_desc = {
	.name = "SGM4154X-input-charger",
	.of_match = "SGM4154X-input-charger",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &sgm4154x_input_charger_ops,
	.csel_reg = SGM4154X_R00,
	.csel_mask = CON0_IINLIM_MASK << CON0_IINLIM_SHIFT,
	.volt_table = sgm4154x_input_volt_table,
	.n_voltages = ARRAY_SIZE(sgm4154x_input_volt_table),
	.curr_table = sgm4154x_input_current_limts,
	.n_current_limits = ARRAY_SIZE(sgm4154x_input_current_limts),
	.linear_min_sel = 0,
};

static struct regulator_init_data sgm4154x_charger_initdata = {
	.constraints = {
		.name = "charger",
		.min_uV = VREG_VOL_MIN*1000,
		.max_uV = VREG_VOL_MAX*1000,
		.min_uA = 0,
		.max_uA = 3000000,
		.always_on = 0,
		.valid_modes_mask = REGULATOR_MODE_NORMAL,
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_CURRENT | REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = 0,
	.consumer_supplies = NULL,
};

static const int sgm4154x_charger_current_limts[] = {
	0, 60000, 120000, 180000, 240000, 300000,
	360000, 420000, 480000, 540000, 600000,
	660000, 720000, 780000, 840000, 900000,
	960000, 1020000, 1080000, 1140000, 1200000,
	1260000, 1320000, 1380000, 1440000, 1500000,
	1560000, 1620000, 1680000, 1740000, 1800000,
	1860000, 1920000, 1980000, 2040000, 2100000,
	2160000, 2220000, 2280000, 2340000, 2400000,
	2460000, 2520000, 2580000, 2640000, 2700000,
	2760000, 2820000, 2880000, 2940000, 3000000,
};

static const struct regulator_ops sgm4154x_charger_ops = {
	.disable = sgm4154x_enchg_disable,
	.enable = sgm4154x_enchg_enable,
	.is_enabled = sgm4154x_regulator_is_enable,
	.set_voltage_sel = sgm4154x_regulator_set_vreg_sel,
	.get_voltage_sel = sgm4154x_regulator_get_vreg_sel,
	.set_current_limit = sgm4154x_regulator_set_cur_uA,
	.get_current_limit = sgm4154x_regulator_get_cur_uA,
	.map_voltage = regulator_map_voltage_linear,
	.list_voltage = regulator_list_voltage_linear,
};

static struct regulator_desc sgm4154x_charger_desc = {
	.name = "SGM4154X-charger",
	.of_match = "SGM4154X-charger",
	.supply_name = "charger",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &sgm4154x_charger_ops,
	.min_uV = VREG_VOL_MIN * 1000,
	.uV_step	 = (VREG_VOL_STEP / 2)*1000,
	.n_voltages = 48,
	.vsel_reg = SGM4154X_R04,
	.vsel_mask = CON4_VREG_MASK << CON4_VREG_SHIFT,
	.csel_reg = SGM4154X_R02,
	.csel_mask = CON2_ICHG_MASK << CON2_ICHG_SHIFT,
	.curr_table = sgm4154x_charger_current_limts,
	.n_current_limits = ARRAY_SIZE(sgm4154x_charger_current_limts),
	.enable_reg = SGM4154X_R01,
	.enable_mask = CON1_CHG_CONFIG_MASK << CON1_CHG_CONFIG_SHIFT,
	.linear_min_sel = 0,
};

#ifdef SGM4154X_OTG
static struct regulator_init_data sgm4154x_otg_vbus_initdata = {
	.constraints = {
		.name = "otg",
		.min_uV = 4850000,
		.max_uV = 5300000,
		.min_uA = 500000,
		.max_uA = 1200000,
		.always_on = 0,
		.valid_modes_mask = REGULATOR_MODE_NORMAL,
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_CURRENT | REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = 0,
	.consumer_supplies = NULL,
};

static const int sgm4154x_boost_current_limts[] = {
	1200000,2000000, 
};
static const struct regulator_ops sgm4154x_otg_vbus_ops = {
	.enable = sgm4154x_otg_reg_enable,
	.disable = sgm4154x_otg_reg_disable,
	.is_enabled = sgm4154x_regulator_is_enable,
	.set_voltage_sel = sgm4154x_regulator_set_volt_sel,
	.get_voltage_sel = sgm4154x_regulator_get_volt_sel,
	.map_voltage = regulator_map_voltage_linear,
	.list_voltage = regulator_list_voltage_linear,
	.set_current_limit = sgm4154x_regulator_set_cur_uA,
	.get_current_limit = sgm4154x_regulator_get_cur_uA,
};

static struct regulator_desc sgm4154x_otg_vbus_desc = {
	.name = "SGM4154X-otg-vbus",
	.of_match = "SGM4154X-otg-vbus",
	.supply_name = "otg",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &sgm4154x_otg_vbus_ops,
	.min_uV = 4850000,
	.uV_step = 150000,
	.n_voltages = 4,
	.vsel_reg = SGM4154X_R06,
	.vsel_mask = CON6_BOOST_VLIM_MASK << CON6_BOOST_VLIM_SHIFT,
	.linear_min_sel = 0,
	.enable_reg = SGM4154X_R01,
	.enable_mask = CON1_OTG_CONFIG_MASK << CON1_OTG_CONFIG_SHIFT,
	.curr_table = sgm4154x_boost_current_limts,
	.n_current_limits = ARRAY_SIZE(sgm4154x_boost_current_limts),
	.csel_reg = SGM4154X_R02,
	.csel_mask = CON2_BOOST_ILIM_MASK << CON2_BOOST_ILIM_SHIFT,
};
#endif

static enum power_supply_property sgm4154x_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_MAX, //cv
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, //vbus current
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, //cc
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_QUICK_CHARGE_TYPE,
	POWER_SUPPLY_PROP_PRECHARGE_CURRENT,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};
static int sgm4154x_get_prop(
		struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int32_t ret = 0;
	struct sgm4154x_chip *chip = power_supply_get_drvdata(psy);
	union power_supply_propval val_temp;

	val->intval = 0;
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!atomic_read(&chip->vbus_gd);
//		val->intval = (chip->chg_type != POWER_SUPPLY_TYPE_UNKNOWN);
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
			break;
		}
		ret = sgm4154x_get_charger_status(chip);
		if (ret == 0)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		else if (ret == 1)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		else if (2 == ret || 3 == ret)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		else // ret = -1
			val->intval = POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		ret = sgm4154x_get_charger_status(chip);
		if (ret == 3)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (ret == 0)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:	//cv
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 0;
			break;
		}
		ret = sgm4154x_get_vreg_volt(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 500*1000;
			break;
		}
		ret = sgm4154x_get_input_curr_lim(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 500*1000;
			break;
		}
		ret = sgm4154x_get_ichg_current(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:	//vbus adc
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 0;
			break;
		}
	//	ret = sgm4154x_get_adc_value(chip, vbus_channel);
		//sgm4154x_info("Not support POWER_SUPPLY_PROP_VOLTAGE_NOW\n");
		//val->intval = (ret < 0) ? 0 : (ret * 1000);
		val->intval = -1;
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:	//ibus adc
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 0;
			break;
		}
	//	ret = sgm4154x_get_adc_value(chip, ibus_channel);
		//sgm4154x_info("Not support POWER_SUPPLY_PROP_CURRENT_NOW\n");
		//val->intval = (ret < 0) ? 0 : (ret * 1000);
		val->intval = -1;
		break;
	case	POWER_SUPPLY_PROP_CURRENT_MAX:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 0;
			break;
		}
		ret = sgm4154x_get_ichg_current(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
	break;
	case	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 500*1000;
			break;
		}
		ret = sgm4154x_get_input_curr_lim(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
	break;
	case	POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = 500*1000;
			break;
		}
		ret = sgm4154x_get_vlimit(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
	break;
	case	POWER_SUPPLY_PROP_QUICK_CHARGE_TYPE:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = QUICK_CHARGE_NORMAL;
			break;
		}
		ret = sgm4154x_get_main_psy_prop(chip, POWER_SUPPLY_PROP_TEMP, &val_temp);
		if (!ret) {
			if (val_temp.intval < 50 || val_temp.intval >= 480) {
				val->intval = QUICK_CHARGE_NORMAL;
				break;
			}
		}
		val->intval = chip->chg_type == POWER_SUPPLY_TYPE_USB_HVDCP ?
			QUICK_CHARGE_FAST : QUICK_CHARGE_NORMAL;
		ret = 0;
	break;
	case	POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = chip->pre_ma * 1000;
			break;
		}
		ret = sgm4154x_get_iprechg_current(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
	break;
	case	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		if (!atomic_read(&chip->vbus_gd)) {
			val->intval = chip->eoc_ma * 1000;
			break;
		}
		ret = sgm4154x_get_iterm(chip);
		val->intval = (ret < 0) ? 0 : (ret * 1000);
	break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "sgm4154x";
		break;

	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "SG Micro Corp";
		break;

	default:
		sgm4154x_err("get prop %d is not supported in SGM4154X\n", psp);
		ret = -EINVAL;
		break;
	}

	if (ret < 0) {
		sgm4154x_err("Couldn't get prop %d rc = %d\n", psp, ret);
		return -ENODATA;
	}
	return 0;
}

static int sgm4154x_set_prop(
	struct power_supply *psy, enum power_supply_property psp,
	const union power_supply_propval *val)
{
	int32_t ret = 0;
	struct sgm4154x_chip *chip = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:		//cv
		ret = sgm4154x_set_vreg_volt(chip, val->intval / 1000);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT: //ilimit
		ret = sgm4154x_set_input_curr_lim(chip, val->intval / 1000);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX: //cc
		ret = sgm4154x_set_ichg_current(chip, val->intval / 1000);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:	//vbus in uv
		ret = sgm4154x_charging_set_hvdcp20(chip, val->intval / 1000000);
		break;
	case POWER_SUPPLY_PROP_QUICK_CHARGE_TYPE:
		ret = sgm4154x_charge_type_detection(chip);
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		if(val->intval == VILIMIT_VOL_MAX * 1000) {
			chip->discard = 1;
			sgm4154x_set_en_chg_timer(chip, 0);
			msleep(100);
			sgm4154x_set_en_chg_timer(chip, 1);
			ret = sgm4154x_set_vlimit(chip, VILIMIT_VOL_MAX);
		}
		else if(val->intval == 0) {
			chip->discard = 0;
			sgm4154x_set_en_chg_timer(chip, 0);
			msleep(100);
			sgm4154x_set_en_chg_timer(chip, 1);
			ret = sgm4154x_charge_type_detection(chip);
		}
		else
			ret = sgm4154x_set_vlimit(chip, val->intval / 1000);
		break;
	default:
		sgm4154x_err("set prop %d is not supported\n", psp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int sgm4154x_prop_is_writeable(
	struct power_supply *psy, enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_QUICK_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		return 1;
	default:
		break;
	}

	return 0;
}

static struct power_supply_desc sgm4154x_psy_desc = {
	.name = "sgm4154x",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sgm4154x_props,
	.num_properties = ARRAY_SIZE(sgm4154x_props),
	.get_property = sgm4154x_get_prop,
	.set_property = sgm4154x_set_prop,
	.property_is_writeable = sgm4154x_prop_is_writeable,
};

static enum power_supply_property sgm4154x_usb_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_QUICK_CHARGE_TYPE,
};

static int sgm4154x_get_usb_prop(
		struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int32_t ret = 0;
	struct sgm4154x_chip *chip = power_supply_get_drvdata(psy);

	val->intval = 0;
	switch (psp) {
		case POWER_SUPPLY_PROP_PRESENT:
		case POWER_SUPPLY_PROP_ONLINE:
			val->intval = !!atomic_read(&chip->vbus_gd);
			ret = 0;
			break;
		case	POWER_SUPPLY_PROP_QUICK_CHARGE_TYPE:
			if (!atomic_read(&chip->vbus_gd)) {
				val->intval = QUICK_CHARGE_NORMAL;
				break;
			}
			val->intval = chip->chg_type == POWER_SUPPLY_TYPE_USB_HVDCP ?
				QUICK_CHARGE_FAST : QUICK_CHARGE_NORMAL;
			ret = 0;
		break;
		default:
			ret = -EINVAL;
		break;
	}
	return ret;
}
static int sgm4154x_set_usb_prop(
	struct power_supply *psy, enum power_supply_property psp,
	const union power_supply_propval *val)
{
	return -EINVAL;
}

static int sgm4154x_usb_prop_is_writeable(
	struct power_supply *psy, enum power_supply_property psp)
{
	return 0;
}
static struct power_supply_desc sgm4154x_usb_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB_PD,
	.properties = sgm4154x_usb_props,
	.num_properties = ARRAY_SIZE(sgm4154x_usb_props),
	.get_property = sgm4154x_get_usb_prop,
	.set_property = sgm4154x_set_usb_prop,
	.property_is_writeable = sgm4154x_usb_prop_is_writeable,
};
static ssize_t sgm4154x_sysfs_ibat_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	//int ret;
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_ibat);
//	ret = sgm4154x_get_adc_value(chip, ibat_channel);
	sgm4154x_info("Not support sgm4154x_sysfs_ibat_show\n");
	//if (ret < 0)
		return sprintf(buf, "%s\n", "Error");
	//return sprintf(buf, "%d mA\n", ret);
}

static ssize_t sgm4154x_sysfs_vbat_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	//int ret;
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_vbat);
//	ret = sgm4154x_get_adc_value(chip, vbat_channel);
	sgm4154x_info("Not support sgm4154x_sysfs_vbat_show\n");
	//if (ret < 0)
		return sprintf(buf, "%s\n", "Error");
	//return sprintf(buf, "%d mV\n", ret);
}

static ssize_t sgm4154x_sysfs_vsys_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	//int ret;
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_vsys);
//	ret = sgm4154x_get_adc_value(chip, vsys_channel);
	sgm4154x_info("Not support sgm4154x_sysfs_vsys_show\n");
	//if (ret < 0)
		return sprintf(buf, "%s\n", "Error");
	//return sprintf(buf, "%d mV\n", ret);
}

static ssize_t sgm4154x_sysfs_usb_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_usb_type);
	switch (chip->port) {
	case SGM4154X_PORTSTAT_NO_INPUT:
		return sprintf(buf, "%s\n", "No input");
	break;
	case SGM4154X_PORTSTAT_SDP:
		return sprintf(buf, "%s\n", "USB");
	break;
	case SGM4154X_PORTSTAT_CDP:
		return sprintf(buf, "%s\n", "USB_CDP");
	break;
	case SGM4154X_PORTSTAT_DCP:
		return sprintf(buf, "%s\n", "USB_DCP");
	break;
	case SGM4154X_PORTSTAT_HVDCP:
		return sprintf(buf, "%s\n", "USB_HVDCP");
	break;
	default:
		return sprintf(buf, "%s\n", "unknow");
	break;
	}
	return sprintf(buf, "%s\n", "unknow");
}

static ssize_t sgm4154x_sysfs_usb_type_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_usb_type);
	int ret;

	if (strncmp(buf, "Redetect", strlen("Redetect"))) {
		ret = -EINVAL;
		return ret;
	}
	sgm4154x_charge_type_detection(chip);
	return count;
}


static ssize_t sgm4154x_sysfs_usb_discard_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_usb_discard);
	int ret;
	int discard;

	ret = kstrtoint(buf, 0, &discard);
	if (ret < 0) {
		ret = -EINVAL;
		return ret;
	}
	chip->discard = !!discard;
	sgm4154x_set_en_chg_timer(chip, 0);
	msleep(100);
	sgm4154x_set_en_chg_timer(chip, 1);
	if(chip->discard)
		sgm4154x_set_vlimit(chip, SGM4154x_VINDPM_V_MAX_uV);
	else
		sgm4154x_charge_type_detection(chip);
	//sgm4154x_set_en_hiz(chip, chip->discard);
	return count;
}

static ssize_t sgm4154x_sysfs_usb_discard_show(struct device *dev,struct device_attribute *attr, char *buf)
{
  struct sgm4154x_chip *chip
    = container_of(attr, struct sgm4154x_chip, attr_usb_discard);

  //chip->discard = sgm4154x_get_en_hiz(chip);
  return sprintf(buf, "%d\n", !!chip->discard);
}

static ssize_t sgm4154x_sysfs_dbg_mask_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct sgm4154x_chip *chip
		= container_of(attr, struct sgm4154x_chip, attr_dbg_mask);
	int ret;
	int debug_mask;

	ret = kstrtoint(buf, 0, &debug_mask);
	if (ret < 0) {
		ret = -EINVAL;
		return ret;
	}
	chip->debug_mask = debug_mask;
	return count;
}

static ssize_t sgm4154x_sysfs_dbg_mask_show(struct device *dev,struct device_attribute *attr, char *buf)
{
  struct sgm4154x_chip *chip
    = container_of(attr, struct sgm4154x_chip, attr_dbg_mask);
  return sprintf(buf, "%d\n", chip->debug_mask);
}


static int32_t sgm4154x_sysfs_init(struct sgm4154x_chip *chip)
{
	int i = 0;
	chip->sysfs_groups = devm_kcalloc(chip->dev, 2,
				sizeof(*chip->sysfs_groups), GFP_KERNEL);

	chip->attr_grp.name = "usb";
	chip->attr_grp.attrs = chip->attrs;
	chip->sysfs_groups[0] = &chip->attr_grp;

	chip->attrs[i++] = &chip->attr_usb_type.attr;
	sysfs_attr_init(&chip->attr_usb_type.attr);
	chip->attr_usb_type.attr.name = "usb_type";
	chip->attr_usb_type.attr.mode = 0644;
	chip->attr_usb_type.show
			= sgm4154x_sysfs_usb_type_show;
	chip->attr_usb_type.store
			= sgm4154x_sysfs_usb_type_store;

	chip->attrs[i++] = &chip->attr_usb_discard.attr;
	sysfs_attr_init(&chip->attr_usb_discard.attr);
	chip->attr_usb_discard.attr.name = "charging_disable";
	chip->attr_usb_discard.attr.mode = 0644;
	chip->attr_usb_discard.show
			= sgm4154x_sysfs_usb_discard_show;
	chip->attr_usb_discard.store
			= sgm4154x_sysfs_usb_discard_store;

	chip->attrs[i++] = &chip->attr_ibat.attr;
	sysfs_attr_init(&chip->attr_ibat.attr);
	chip->attr_ibat.attr.name = "ibat";
	chip->attr_ibat.attr.mode = 0444;
	chip->attr_ibat.show
			= sgm4154x_sysfs_ibat_show;

	chip->attrs[i++] = &chip->attr_vbat.attr;
	sysfs_attr_init(&chip->attr_vbat.attr);
	chip->attr_vbat.attr.name = "vbat";
	chip->attr_vbat.attr.mode = 0444;
	chip->attr_vbat.show
			= sgm4154x_sysfs_vbat_show;

	chip->attrs[i++] = &chip->attr_vsys.attr;
	sysfs_attr_init(&chip->attr_vsys.attr);
	chip->attr_vsys.attr.name = "vsys";
	chip->attr_vsys.attr.mode = 0444;
	chip->attr_vsys.show
			= sgm4154x_sysfs_vsys_show;

	chip->attrs[i++] = &chip->attr_dbg_mask.attr;
	sysfs_attr_init(&chip->attr_dbg_mask.attr);
	chip->attr_dbg_mask.attr.name = "dbg_mask";
	chip->attr_dbg_mask.attr.mode = 0644;
	chip->attr_dbg_mask.show
			= sgm4154x_sysfs_dbg_mask_show;
	chip->attr_dbg_mask.store
			= sgm4154x_sysfs_dbg_mask_store;

	chip->attrs[i++] = NULL;
	return 0;
}

static int32_t sgm4154x_power_init(struct sgm4154x_chip *chip)
{
	struct power_supply_config cfg = {};

	cfg.drv_data = chip;
	cfg.of_node  = chip->dev->of_node;
	if (!sgm4154x_sysfs_init(chip))
		cfg.attr_grp = chip->sysfs_groups;
	sgm4154x_psy_desc.name = chip->chg_psy_name;

	chip->sgm4154x_psy = devm_power_supply_register(chip->dev, &sgm4154x_psy_desc, &cfg);

	if (IS_ERR(chip->sgm4154x_psy)) {
		dev_err(chip->dev, "SGM4154X Couldn't register power supply\n");
		return PTR_ERR(chip->sgm4154x_psy);
	}

	cfg.attr_grp = NULL;
	chip->sgm4154x_usb_psy = devm_power_supply_register(chip->dev, &sgm4154x_usb_psy_desc, &cfg);

	if (IS_ERR(chip->sgm4154x_usb_psy)) {
		dev_err(chip->dev, "SGM4154X:usb Couldn't register power supply\n");
	}

	return 0;
}

static int32_t sgm4154x_regulator_init(struct sgm4154x_chip *chip)
{
	struct regulator_dev *rdev;
	struct regulator_config config;

	config.dev = chip->dev;
	config.driver_data = chip;
#ifdef SGM4154X_OTG
	if (chip->otg_regulator_name) {
		config.init_data = &sgm4154x_otg_vbus_initdata;
		sgm4154x_otg_vbus_initdata.constraints.name = chip->otg_regulator_name;
		sgm4154x_otg_vbus_desc.name = chip->otg_regulator_name;
		rdev = devm_regulator_register(chip->dev,
						&sgm4154x_otg_vbus_desc, &config);
	}
#endif
	if (chip->input_regulator_name) {
		config.init_data = &sgm4154x_input_initdata;
		sgm4154x_input_charger_desc.name = chip->input_regulator_name;
		sgm4154x_input_initdata.constraints.name = chip->input_regulator_name;
		rdev = devm_regulator_register(chip->dev,
						&sgm4154x_input_charger_desc, &config);
	}
	#if 1
	if (chip->chg_regulator_name) {
		config.init_data = &sgm4154x_charger_initdata;
		sgm4154x_charger_desc.name = chip->chg_regulator_name;
		sgm4154x_charger_initdata.constraints.name = chip->chg_regulator_name;
		rdev = devm_regulator_register(chip->dev,
						&sgm4154x_charger_desc, &config);
	}
	#endif
	return 0;
}

static int32_t sgm4154x_extcon_init(struct sgm4154x_chip *chip)
{
	chip->edev = devm_extcon_dev_allocate(chip->dev,
									sgm4154x_extcon_cables);
	if (IS_ERR(chip->edev)) {
		chip->edev = NULL;
		return -1;
	}
	devm_extcon_dev_register(chip->dev, chip->edev);
	dev_info(chip->dev, "Register Extcon(%s) Success.", extcon_get_edev_name(chip->edev));
	if (chip->vbus_bypass)
		extcon_set_state_sync(chip->edev, EXTCON_USB, true);

	return 0;
}

static int32_t sgm4154x_register_irq(struct sgm4154x_chip *chip)
{
	int32_t ret = 0;

	if (-1 == chip->irq_gpio)
		return 0;

	ret = devm_gpio_request_one(chip->dev, chip->irq_gpio, GPIOF_DIR_IN,
					devm_kasprintf(chip->dev, GFP_KERNEL,
					"sgm4154x_irq_gpio.%s", dev_name(chip->dev)));
	if (ret < 0) {
		sgm4154x_err("gpio request fail(%d)\n", ret);
		return ret;
	}

	chip->irq = gpio_to_irq(chip->irq_gpio);
	if (chip->irq < 0) {
		sgm4154x_err("gpio2irq fail(%d)\n", chip->irq);
		return chip->irq;
	}

	sgm4154x_info("irq gpio:%d irq = %d\n", chip->irq_gpio, chip->irq);

	/* Request threaded IRQ */
	ret = devm_request_threaded_irq(chip->dev, chip->irq, NULL,
					sgm4154x_irq_handler,
					IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
					devm_kasprintf(chip->dev, GFP_KERNEL,
					"sgm4154x_irq.%s", dev_name(chip->dev)),
					chip);

	if (ret < 0) {
		sgm4154x_err("request threaded irq fail(%d)\n", ret);
		return ret;
	}
	enable_irq_wake(chip->irq);
	return ret;
}
static int sgm4154x_reboot_notify(struct notifier_block *this, unsigned long code,
			  void *unused)
{
	struct sgm4154x_chip *chip = container_of(this, struct sgm4154x_chip, reboot_nb);
	chip->discard = 0;
	sgm4154x_set_otg_en(chip, 0);
	sgm4154x_set_ichg_current(chip, 500);
	sgm4154x_set_input_curr_lim(chip, 500);
	sgm4154x_set_vlimit(chip, DEFAULT_VLIMIT);
	sgm4154x_set_vreg_volt(chip, DEFAULT_CV);
	sgm4154x_set_charger_en(chip, 1);
	sgm4154x_info("diable OTG, set chg(4400mV/500mA)\n");
	return NOTIFY_DONE;
}

static int32_t sgm4154x_register_reboot_nb(struct sgm4154x_chip *chip)
{
	chip->reboot_nb.notifier_call = sgm4154x_reboot_notify;
	chip->reboot_nb.priority = INT_MAX;
	register_reboot_notifier(&chip->reboot_nb);
	return 0;
}

static int32_t sgm4154x_driver_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	int32_t ret = 0;
	struct sgm4154x_chip *chip = NULL;
	pr_info("sgm4154x_driver_probe start\n");
	chip = devm_kzalloc(&client->dev, sizeof(struct sgm4154x_chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->debug_mask = SGM4154X_PR_ERR | SGM4154X_PR_INFO | SGM4154X_PR_WARN | SGM4154X_PR_DBG;
	//chip->debug_mask = SD7601_PR_ERR | SD7601_PR_WARN ;
	chip->client = client;
	chip->dev = &client->dev;
	chip->last_update = 0;
	chip->discard = 0;
	//init mutex asap, before iic operate
	mutex_init(&chip->i2c_rw_lock);
	mutex_init(&chip->adc_lock);


	i2c_set_clientdata(client, chip);

	ret = sgm4154x_hw_component_detect(chip);
	if (ret < 0) {
		dev_err(chip->dev, "do not detect ic, exit\n");
		ret = -ENODEV;
		goto err_nodev;
	}

	ret = sgm4154x_parse_dt(chip, &client->dev);
	if (ret < 0)
		dev_err(chip->dev, "sgm4154x_parse_dt failed ret %d\n", ret);

	//charging_hw_init(chip);
	atomic_set(&chip->boost_fault_cnt, 0);
#ifdef SGM4154X_DEBUG_WORK_POLL
	INIT_DELAYED_WORK(&chip->sgm4154x_work, sgm4154x_work_func);
    schedule_delayed_work(&chip->sgm4154x_work, 100);
#endif
	/*
	if ((chip->component_id == SGM4154XD_COMPINENT_ID) ||
			(chip->component_id == SGM4154XAD_COMPINENT_ID) ||
			(chip->component_id == SGM4154XAD1_COMPINENT_ID)) 
	*/
	{
		mutex_init(&chip->bc12_lock);
		atomic_set(&chip->vbus_gd, 0);
		chip->attach = false;
		chip->port = SGM4154X_PORTSTAT_NO_INPUT;
		INIT_DELAYED_WORK(&chip->psy_dwork,
				sgm4154x_inform_psy_dwork_handler);

#ifdef TRIGGER_CHARGE_TYPE_DETECTION
		init_waitqueue_head(&chip->bc12_en_req);
		atomic_set(&chip->bc12_en_req_cnt, 0);
		chip->bc12_en_kthread = kthread_run(sgm4154x_bc12_en_kthread, chip, "sgm4154x_bc12");
		if (IS_ERR_OR_NULL(chip->bc12_en_kthread)) {
			ret = PTR_ERR(chip->bc12_en_kthread);
			dev_err(chip->dev, "kthread run fail(%d)\n", ret);
			goto err_kthread_run;
		}
		INIT_DELAYED_WORK(&chip->hvdcp_work,
					sgm4154x_chg_type_hvdcp_work);
		INIT_DELAYED_WORK(&chip->unkown_type_work,
					sgm4154x_unkown_type_dwork_handler);
#endif
	}
	ret = sgm4154x_power_init(chip);
	if (ret < 0) {
		dev_err(chip->dev, "register power supply fail(%d)\n", ret);
		goto err_power;
	}
	ret = sgm4154x_regulator_init(chip);
	if (ret < 0) {
		dev_err(chip->dev, "register regulator fail(%d)\n", ret);
		goto err_power;
	}
	ret = sgm4154x_extcon_init(chip);
	if (ret < 0) {
		dev_err(chip->dev, "register extcon fail(%d)\n", ret);
		goto err_power;
	}
	ret = sgm4154x_register_reboot_nb(chip);
	if (ret < 0) {
		dev_err(chip->dev, "register reboot nb fail(%d)\n", ret);
		goto err_power;
	}
	//process irq once when probe
	ret = sgm4154x_process_irq(chip);
	if (ret < 0) {
		dev_err(chip->dev, "process irq fail(%d)\n", ret);
		goto err_process_irq;
	}
	ret = sgm4154x_register_irq(chip);
	if (ret < 0) {
		dev_err(chip->dev, "register irq fail(%d)\n", ret);
		goto err_register_irq;
	}

	device_init_wakeup(chip->dev, true);
	//sgm4154x_dump_msg(chip);

	ret = device_create_file(&client->dev, &dev_attr_registers);
	if (ret < 0) {
		dev_err(chip->dev, "create file fail(%d)\n", ret);
		goto err_create_file;
	}

	return 0;

err_create_file:
	if (chip->irq >= 0)
		disable_irq(chip->irq);
	if (-1 != chip->irq_gpio)
		devm_gpio_free(chip->dev, chip->irq_gpio);
err_register_irq:
err_process_irq:
err_power:
	cancel_delayed_work_sync(&chip->psy_dwork);
	if (chip->psy)
		power_supply_put(chip->psy);
#ifdef TRIGGER_CHARGE_TYPE_DETECTION
	kthread_stop(chip->bc12_en_kthread);
	cancel_delayed_work_sync(&chip->hvdcp_work);
err_kthread_run:
#endif
	mutex_destroy(&chip->bc12_lock);
err_nodev:
	mutex_destroy(&chip->adc_lock);
	mutex_destroy(&chip->i2c_rw_lock);
	return ret;
}

static int32_t sgm4154x_suspend(struct device *dev)
{
	struct sgm4154x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
#ifdef SGM4154X_DEBUG_WORK_POLL
	cancel_delayed_work(&chip->sgm4154x_work);
#endif
	cancel_delayed_work(&chip->unkown_type_work);
	if (device_may_wakeup(dev)) {
		pinctrl_pm_select_sleep_state(dev);
		if(chip->irq >= 0)
			enable_irq_wake(chip->irq);
		if(chip->ce_gpio != -1)
			gpio_direction_input(chip->ce_gpio);
	}
	
	if (chip->irq >= 0)
		disable_irq(chip->irq);

	return 0;
}
static int32_t sgm4154x_resume(struct device *dev)
{
	struct sgm4154x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	if (atomic_read(&chip->vbus_gd) &&
			(chip->port == SGM4154X_PORTSTAT_UNKNOWN ||
			chip->port == SGM4154X_PORTSTAT_NON_STANDARD))
		schedule_delayed_work(&chip->unkown_type_work,
				msecs_to_jiffies(unkown_type_retry_intvs[0] * 1000));

	if (chip->irq >= 0)
		enable_irq(chip->irq);
#ifdef SGM4154X_DEBUG_WORK_POLL
	schedule_delayed_work(&chip->sgm4154x_work, 10*HZ);
#endif
	if (device_may_wakeup(dev)) {
		pinctrl_pm_select_default_state(dev);
		if(chip->irq >= 0)
			disable_irq_wake(chip->irq);
		if(chip->ce_gpio != -1)
			gpio_direction_output(chip->ce_gpio, 0);
	}
	return 0;
}
static int32_t sgm4154x_driver_remove(struct i2c_client *client)
{
	struct sgm4154x_chip *chip = i2c_get_clientdata(client);

	if (chip->irq >= 0)
		disable_irq(chip->irq);
	if (-1 != chip->irq_gpio)
		devm_gpio_free(chip->dev, chip->irq_gpio);
	/*
	if ((chip->component_id == SGM4154XD_COMPINENT_ID) ||
				(chip->component_id == SGM4154XAD_COMPINENT_ID) ||
				(chip->component_id == SGM4154XAD1_COMPINENT_ID)) 
	*/
	{
		cancel_delayed_work_sync(&chip->psy_dwork);
		if (chip->psy)
			power_supply_put(chip->psy);

#ifdef TRIGGER_CHARGE_TYPE_DETECTION
		kthread_stop(chip->bc12_en_kthread);
#endif
		mutex_destroy(&chip->bc12_lock);
	}

	mutex_destroy(&chip->adc_lock);
	mutex_destroy(&chip->i2c_rw_lock);
	return 0;
}
static void sgm4154x_driver_shutdown(struct i2c_client *client)
{
	struct sgm4154x_chip *chip = i2c_get_clientdata(client);
	sgm4154x_reboot_notify(&chip->reboot_nb, 0, NULL);
	sgm4154x_dump_msg(chip);
}

static const struct dev_pm_ops sgm4154x_pm_ops = {
	.resume = sgm4154x_resume,
	.suspend = sgm4154x_suspend,
};
static const struct of_device_id sgm4154x_of_match[] = {
	{.compatible = "jlq,sgm4154x"},
	{.compatible = "jlq,sgm4154xd"},
	{.compatible = "jlq,sgm4154xad"},
	{},
};

static const struct i2c_device_id sgm4154x_i2c_id[] = {
	{"sgm4154x",   0},
	{"sgm4154xd",  1},
	{"sgm4154xad", 2},
};

static struct i2c_driver sgm4154x_driver = {
	.driver = {
		.name = "sgm4154x",
		.owner = THIS_MODULE,
		.pm = &sgm4154x_pm_ops,
		.of_match_table = sgm4154x_of_match,
	},
	.id_table = sgm4154x_i2c_id,
	.probe = sgm4154x_driver_probe,
	.remove = sgm4154x_driver_remove,
	.shutdown = sgm4154x_driver_shutdown,
};

static int32_t __init sgm4154x_init(void)
{
	if (i2c_add_driver(&sgm4154x_driver) != 0)
		pr_err("failed to register sgm4154x i2c driver.\n");
	else
		pr_info("Success to register sgm4154x i2c driver.\n");

	return 0;
}

static void __exit sgm4154x_exit(void)
{
	i2c_del_driver(&sgm4154x_driver);
}
module_init(sgm4154x_init);
module_exit(sgm4154x_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("I2C sgm4154x Driver");
MODULE_AUTHOR("marvin.xin <marvin.xin@bigmtech.com>");
