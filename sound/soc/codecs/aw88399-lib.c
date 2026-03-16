// SPDX-License-Identifier: GPL-2.0-only
//
// aw88399-lib.c -- AW88399 device library shared between ASoC and HDA
//
// Copyright (c) 2023 AWINIC Technology CO., LTD
//
// Based on aw88399.c by Weidong Wang <wangweidong.a@awinic.com>
//

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/aw88399.h>

#include "aw88395/aw88395_device.h"
#include "aw88395/aw88395_lib.h"
#include "aw88395/aw88395_data_type.h"

/*
 * Additional bit field definitions not in shared header
 * (internal to library implementation)
 */

/* I2S TX enable - NOTE: uses I2SCTRL3_REG, not I2SCFG1_REG */
#define AW88399_I2STXEN_START_BIT	0
#define AW88399_I2STXEN_MASK		(~(1 << AW88399_I2STXEN_START_BIT))
#define AW88399_I2STXEN_ENABLE_VALUE	(1 << AW88399_I2STXEN_START_BIT)
#define AW88399_I2STXEN_DISABLE_VALUE	(0 << AW88399_I2STXEN_START_BIT)

/* Power down control */
#define AW88399_PWDN_START_BIT		0
#define AW88399_PWDN_MASK		(~(1 << AW88399_PWDN_START_BIT))
#define AW88399_PWDN_POWER_DOWN_VALUE	(1 << AW88399_PWDN_START_BIT)
#define AW88399_PWDN_WORKING_VALUE	(0 << AW88399_PWDN_START_BIT)

/* Amplifier power down */
#define AW88399_AMPPD_START_BIT		1
#define AW88399_AMPPD_MASK		(~(1 << AW88399_AMPPD_START_BIT))
#define AW88399_AMPPD_POWER_DOWN_VALUE	(1 << AW88399_AMPPD_START_BIT)
#define AW88399_AMPPD_WORKING_VALUE	(0 << AW88399_AMPPD_START_BIT)

/* DSP bypass */
#define AW88399_DSPBY_START_BIT		2
#define AW88399_DSPBY_MASK		(~(1 << AW88399_DSPBY_START_BIT))
#define AW88399_DSPBY_BYPASS_VALUE	(1 << AW88399_DSPBY_START_BIT)
#define AW88399_DSPBY_WORKING_VALUE	(0 << AW88399_DSPBY_START_BIT)

/* Memory clock select */
#define AW88399_MEM_CLKSEL_START_BIT	3
#define AW88399_MEM_CLKSEL_MASK		(~(1 << AW88399_MEM_CLKSEL_START_BIT))
#define AW88399_MEM_CLKSEL_OSCCLK_VALUE	(0 << AW88399_MEM_CLKSEL_START_BIT)
#define AW88399_MEM_CLKSEL_DAPHCLK_VALUE (1 << AW88399_MEM_CLKSEL_START_BIT)

/* Hardware mute */
#define AW88399_HMUTE_START_BIT		8
#define AW88399_HMUTE_MASK		(~(1 << AW88399_HMUTE_START_BIT))
#define AW88399_HMUTE_ENABLE_VALUE	(1 << AW88399_HMUTE_START_BIT)
#define AW88399_HMUTE_DISABLE_VALUE	(0 << AW88399_HMUTE_START_BIT)

/* PLL status bits */
#define AW88399_PLLS_START_BIT		0
#define AW88399_PLLS_LOCKED_VALUE	(1 << AW88399_PLLS_START_BIT)
#define AW88399_CLKS_START_BIT		4
#define AW88399_CLKS_STABLE_VALUE	(1 << AW88399_CLKS_START_BIT)

#define AW88399_BIT_PLL_CHECK \
	(AW88399_CLKS_STABLE_VALUE | AW88399_PLLS_LOCKED_VALUE)

/* System status check masks */
#define AW88399_SWS_START_BIT		8
#define AW88399_SWS_VALUE		(1 << AW88399_SWS_START_BIT)
#define AW88399_BIT_SYSST_NOSWS_CHECK \
	(AW88399_CLKS_STABLE_VALUE | AW88399_PLLS_LOCKED_VALUE)
#define AW88399_BIT_SYSST_SWS_CHECK \
	(AW88399_SWS_VALUE | AW88399_CLKS_STABLE_VALUE | AW88399_PLLS_LOCKED_VALUE)
#define AW88399_BIT_SYSST_CHECK_MASK	(~0x01FF)

/* Noise gate enable bit for sysst check mode selection */
#define AW88399_NOISE_GATE_EN_START_BIT	13
#define AW88399_NOISE_GATE_EN_MASK	(~(1 << AW88399_NOISE_GATE_EN_START_BIT))

/* System interrupt bits */
#define AW88399_PLLI_START_BIT		0
#define AW88399_CLKI_START_BIT		4
#define AW88399_NOCLKI_START_BIT	5
#define AW88399_BIT_SYSINT_CHECK \
	((1 << AW88399_PLLI_START_BIT) | \
	 (1 << AW88399_CLKI_START_BIT) | \
	 (1 << AW88399_NOCLKI_START_BIT))

/* CCO MUX for PLL mode switching */
#define AW88399_CCO_MUX_START_BIT	14
#define AW88399_CCO_MUX_MASK		(~(3 << AW88399_CCO_MUX_START_BIT))
#define AW88399_CCO_MUX_BYPASS_VALUE	(0 << AW88399_CCO_MUX_START_BIT)
#define AW88399_CCO_MUX_DIVIDED_VALUE	(1 << AW88399_CCO_MUX_START_BIT)

/* DSP watchdog for status check */
#define AW88399_WDT_CNT_MASK		(~0xFF)

/* Volume control */
#define AW88399_VOLUME_STEP_DB		64
#define AW88399_VOL_DEFAULT_VALUE	0
#define AW88399_MUTE_VOL		1023
#define AW88399_VOL_START_BIT		0
#define AW88399_VOL_MASK		(~0x3FF)

/* Retry and timing constants */
#define AW88399_START_RETRIES		5
#define AW88399_DEV_SYSST_CHECK_MAX	10
#define AW88399_DEV_DSP_CHECK_MAX	10
#define AW88399_DEV_DEFAULT_CH		0

/* Dither enable bit */
#define AW88399_DITHER_EN_START_BIT	15
#define AW88399_DITHER_EN_MASK		(~(1 << AW88399_DITHER_EN_START_BIT))
#define AW88399_DITHER_EN_ENABLE_VALUE	(1 << AW88399_DITHER_EN_START_BIT)
#define AW88399_DITHER_EN_DISABLE_VALUE	(0 << AW88399_DITHER_EN_START_BIT)

/**
 * struct aw88399_dev - AW88399 device context
 * @dev: parent device
 * @regmap: register map
 * @aw_pa: underlying aw_device from aw88395 library
 * @aw_cfg: loaded firmware container
 * @lock: mutex for device access
 * @fw_status: firmware load status
 * @dither_st: dither state from firmware (to restore after start)
 */
struct aw88399_dev {
	struct device *dev;
	struct regmap *regmap;
	struct aw_device *aw_pa;
	struct aw_container *aw_cfg;
	struct mutex lock;
	int fw_status;
	unsigned int dither_st;
};

/* Low-level register helpers */
static void aw88399_dev_set_volume(struct aw_device *aw_dev, unsigned short vol)
{
	regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL2_REG,
			   ~AW88399_VOL_MASK, vol);
}

static void aw88399_lib_dev_mute(struct aw_device *aw_dev, bool mute)
{
	if (mute) {
		aw88399_dev_set_volume(aw_dev, AW88399_MUTE_VOL);
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_HMUTE_MASK, AW88399_HMUTE_ENABLE_VALUE);
	} else {
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_HMUTE_MASK, AW88399_HMUTE_DISABLE_VALUE);
		aw88399_dev_set_volume(aw_dev, aw_dev->volume_desc.ctl_volume);
	}
}

static void aw88399_dev_set_dither(struct aw88399_dev *aw88399_dev, bool dither)
{
	struct aw_device *aw_dev = aw88399_dev->aw_pa;

	if (dither)
		regmap_update_bits(aw_dev->regmap, AW88399_DBGCTRL_REG,
				   ~AW88399_DITHER_EN_MASK, AW88399_DITHER_EN_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88399_DBGCTRL_REG,
				   ~AW88399_DITHER_EN_MASK, AW88399_DITHER_EN_DISABLE_VALUE);
}

static void aw_dev_pwd(struct aw_device *aw_dev, bool pwd)
{
	if (pwd)
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_PWDN_MASK, AW88399_PWDN_POWER_DOWN_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_PWDN_MASK, AW88399_PWDN_WORKING_VALUE);
}

static void aw_dev_amppd(struct aw_device *aw_dev, bool amppd)
{
	if (amppd)
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_AMPPD_MASK, AW88399_AMPPD_POWER_DOWN_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_AMPPD_MASK, AW88399_AMPPD_WORKING_VALUE);
}

static void aw_dev_dsp_enable(struct aw_device *aw_dev, bool dsp)
{
	if (dsp)
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_DSPBY_MASK, AW88399_DSPBY_WORKING_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88399_SYSCTRL_REG,
				   ~AW88399_DSPBY_MASK, AW88399_DSPBY_BYPASS_VALUE);
}

static void aw_dev_i2s_tx_enable(struct aw_device *aw_dev, bool flag)
{
	/* NOTE: Uses I2SCTRL3_REG (not I2SCFG1) per vendor driver */
	if (flag)
		regmap_update_bits(aw_dev->regmap, AW88399_I2SCTRL3_REG,
				   ~AW88399_I2STXEN_MASK, AW88399_I2STXEN_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88399_I2SCTRL3_REG,
				   ~AW88399_I2STXEN_MASK, AW88399_I2STXEN_DISABLE_VALUE);
}

static int aw_dev_get_int_status(struct aw_device *aw_dev, u16 *int_status)
{
	unsigned int val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88399_SYSINT_REG, &val);
	if (ret)
		return ret;

	*int_status = (u16)val;
	return 0;
}

static void aw_dev_clear_int_status(struct aw_device *aw_dev)
{
	u16 int_status = 0;

	/* Read int status to clear, then verify */
	aw_dev_get_int_status(aw_dev, &int_status);
	aw_dev_get_int_status(aw_dev, &int_status);
	if (int_status)
		dev_dbg(aw_dev->dev, "int status not cleared: 0x%04x", int_status);
}

static int aw_dev_check_sysint(struct aw_device *aw_dev)
{
	u16 reg_val = 0;
	int ret;

	ret = aw_dev_get_int_status(aw_dev, &reg_val);
	if (ret)
		return ret;

	if (reg_val & AW88399_BIT_SYSINT_CHECK) {
		dev_err(aw_dev->dev, "pa stop check fail:0x%04x", reg_val);
		return -EINVAL;
	}

	return 0;
}

/* PLL lock check - mode 1 (direct IIS check) */
static int aw_dev_get_iis_status(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88399_SYSST_REG, &reg_val);
	if (ret)
		return ret;

	if ((reg_val & AW88399_BIT_PLL_CHECK) != AW88399_BIT_PLL_CHECK) {
		dev_dbg(aw_dev->dev, "pll check fail, reg_val:0x%04x", reg_val);
		return -EINVAL;
	}

	return 0;
}

static int aw_dev_check_mode1_pll(struct aw_device *aw_dev)
{
	int ret, i;

	for (i = 0; i < AW88399_DEV_SYSST_CHECK_MAX; i++) {
		ret = aw_dev_get_iis_status(aw_dev);
		if (!ret)
			return 0;
		usleep_range(AW88399_2000_US, AW88399_2000_US + 10);
	}

	dev_err(aw_dev->dev, "mode1 iis signal check failed");
	return -EPERM;
}

/* PLL lock check - mode 2 (with CCO MUX switch) */
static int aw_dev_check_mode2_pll(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret, i;

	ret = regmap_read(aw_dev->regmap, AW88399_PLLCTRL2_REG, &reg_val);
	if (ret)
		return ret;

	reg_val &= (~AW88399_CCO_MUX_MASK);
	if (reg_val == AW88399_CCO_MUX_DIVIDED_VALUE) {
		dev_dbg(aw_dev->dev, "CCO_MUX already in divided mode");
		return -EPERM;
	}

	/* Switch to mode2 (divided) */
	ret = regmap_update_bits(aw_dev->regmap, AW88399_PLLCTRL2_REG,
				 ~AW88399_CCO_MUX_MASK, AW88399_CCO_MUX_DIVIDED_VALUE);
	if (ret)
		return ret;

	for (i = 0; i < AW88399_DEV_SYSST_CHECK_MAX; i++) {
		ret = aw_dev_get_iis_status(aw_dev);
		if (!ret)
			break;
		usleep_range(AW88399_2000_US, AW88399_2000_US + 10);
	}

	/* Switch back to mode1 (bypass) */
	regmap_update_bits(aw_dev->regmap, AW88399_PLLCTRL2_REG,
			   ~AW88399_CCO_MUX_MASK, AW88399_CCO_MUX_BYPASS_VALUE);

	if (!ret) {
		usleep_range(AW88399_2000_US, AW88399_2000_US + 10);
		for (i = 0; i < AW88399_DEV_SYSST_CHECK_MAX; i++) {
			ret = aw_dev_get_iis_status(aw_dev);
			if (!ret)
				break;
			usleep_range(AW88399_2000_US, AW88399_2000_US + 10);
		}
	}

	if (ret)
		dev_err(aw_dev->dev, "mode2 pll check failed");

	return ret;
}

static int aw_dev_check_syspll(struct aw_device *aw_dev)
{
	int ret;

	ret = aw_dev_check_mode1_pll(aw_dev);
	if (ret) {
		dev_dbg(aw_dev->dev, "mode1 pll failed, trying mode2");
		ret = aw_dev_check_mode2_pll(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "both pll modes failed");
			return ret;
		}
	}

	return 0;
}

/* Full system status check with noise gate awareness */
static int aw_dev_check_sysst(struct aw_device *aw_dev)
{
	unsigned int check_val;
	unsigned int reg_val;
	int ret, i;

	/* Determine check mode based on noise gate setting */
	ret = regmap_read(aw_dev->regmap, AW88399_PWMCTRL3_REG, &reg_val);
	if (ret)
		return ret;

	if (reg_val & (~AW88399_NOISE_GATE_EN_MASK))
		check_val = AW88399_BIT_SYSST_NOSWS_CHECK;
	else
		check_val = AW88399_BIT_SYSST_SWS_CHECK;

	for (i = 0; i < AW88399_DEV_SYSST_CHECK_MAX; i++) {
		ret = regmap_read(aw_dev->regmap, AW88399_SYSST_REG, &reg_val);
		if (ret)
			return ret;

		if ((reg_val & (~AW88399_BIT_SYSST_CHECK_MASK) & check_val) == check_val) {
			dev_dbg(aw_dev->dev, "sysst check pass, val=0x%x", reg_val);
			return 0;
		}
		dev_dbg(aw_dev->dev, "sysst check fail cnt=%d, val=0x%04x, check=0x%x",
			i, reg_val, check_val);
		usleep_range(AW88399_2000_US, AW88399_2000_US + 10);
	}

	dev_err(aw_dev->dev, "sysst check failed, val=0x%x", reg_val);
	return -EPERM;
}

/* DSP status check */
static int aw_dev_get_dsp_status(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88399_WDT_REG, &reg_val);
	if (ret)
		return ret;

	if (!(reg_val & (~AW88399_WDT_CNT_MASK)))
		return -EPERM;

	return 0;
}

static int aw_dev_dsp_check(struct aw_device *aw_dev)
{
	int ret, i;

	if (aw_dev->dsp_cfg == AW88395_DEV_DSP_BYPASS) {
		dev_dbg(aw_dev->dev, "dsp bypass mode");
		return 0;
	}

	/* Toggle DSP to start watchdog */
	aw_dev_dsp_enable(aw_dev, false);
	aw_dev_dsp_enable(aw_dev, true);
	usleep_range(AW88399_1000_US, AW88399_1000_US + 10);

	for (i = 0; i < AW88399_DEV_DSP_CHECK_MAX; i++) {
		ret = aw_dev_get_dsp_status(aw_dev);
		if (!ret) {
			dev_dbg(aw_dev->dev, "dsp status check pass");
			return 0;
		}
		usleep_range(AW88399_2000_US, AW88399_2000_US + 10);
	}

	dev_err(aw_dev->dev, "dsp status check failed");
	return -EPERM;
}

/*
 * Complete device start implementation
 * Follows vendor driver sequence from aw88399.c:aw88399_dev_start()
 */
static int aw88399_dev_start_internal(struct aw88399_dev *aw88399_dev)
{
	struct aw_device *aw_dev = aw88399_dev->aw_pa;
	int ret;

	if (aw_dev->status == AW88399_DEV_PW_ON) {
		dev_dbg(aw_dev->dev, "already powered on");
		return 0;
	}

	/* Disable dither during power-up to prevent noise */
	aw88399_dev_set_dither(aw88399_dev, false);

	/* Step 1: Power on */
	aw_dev_pwd(aw_dev, false);
	usleep_range(AW88399_2000_US, AW88399_2000_US + 10);

	/* Step 2: Check PLL lock */
	ret = aw_dev_check_syspll(aw_dev);
	if (ret) {
		dev_err(aw_dev->dev, "pll check failed, cannot start");
		goto pll_check_fail;
	}

	/* Step 3: Amp power on */
	aw_dev_amppd(aw_dev, false);
	usleep_range(AW88399_1000_US, AW88399_1000_US + 50);

	/* Step 4: Check system status */
	ret = aw_dev_check_sysst(aw_dev);
	if (ret) {
		dev_err(aw_dev->dev, "sysst check failed");
		goto sysst_check_fail;
	}

	/* Step 5: DSP check (if DSP is enabled) */
	if (aw_dev->dsp_cfg == AW88395_DEV_DSP_WORK) {
		aw_dev_dsp_enable(aw_dev, false);
		ret = aw_dev_dsp_check(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "dsp status check failed");
			goto dsp_check_fail;
		}
	} else {
		dev_dbg(aw_dev->dev, "starting with dsp bypass");
	}

	/* Step 6: Enable I2S TX feedback */
	aw_dev_i2s_tx_enable(aw_dev, true);

	/* Step 7: Restore dither if it was enabled in firmware */
	if (aw88399_dev->dither_st)
		aw88399_dev_set_dither(aw88399_dev, true);

	/* Step 8: Unmute */
	aw88399_lib_dev_mute(aw_dev, false);

	/* Step 9: Clear interrupts */
	aw_dev_clear_int_status(aw_dev);

	aw_dev->status = AW88399_DEV_PW_ON;
	dev_dbg(aw_dev->dev, "device started successfully");
	return 0;

dsp_check_fail:
	aw_dev_dsp_enable(aw_dev, false);
sysst_check_fail:
	aw_dev_clear_int_status(aw_dev);
	aw_dev_amppd(aw_dev, true);
pll_check_fail:
	aw_dev_pwd(aw_dev, true);
	aw_dev->status = AW88399_DEV_PW_OFF;
	return ret;
}

/**
 * aw88399_dev_init - Initialize AW88399 device
 */
int aw88399_dev_init(struct device *dev, struct i2c_client *i2c,
		     struct regmap *regmap, struct aw88399_dev **aw88399_dev)
{
	struct aw88399_dev *aw_dev;
	struct aw_device *aw_pa;
	unsigned int chip_id;
	int ret;

	/* Verify chip ID */
	ret = regmap_read(regmap, AW88399_ID_REG, &chip_id);
	if (ret) {
		dev_err(dev, "failed to read chip id: %d", ret);
		return ret;
	}

	if (chip_id != AW88399_CHIP_ID) {
		dev_err(dev, "unsupported chip id: 0x%x", chip_id);
		return -ENODEV;
	}

	dev_dbg(dev, "chip id verified: 0x%x", chip_id);

	/* Allocate device context */
	aw_dev = devm_kzalloc(dev, sizeof(*aw_dev), GFP_KERNEL);
	if (!aw_dev)
		return -ENOMEM;

	aw_dev->dev = dev;
	aw_dev->regmap = regmap;
	mutex_init(&aw_dev->lock);
	aw_dev->fw_status = AW88399_DEV_FW_FAILED;

	/* Allocate underlying aw_device */
	aw_pa = devm_kzalloc(dev, sizeof(*aw_pa), GFP_KERNEL);
	if (!aw_pa)
		return -ENOMEM;

	aw_pa->i2c = i2c;
	aw_pa->dev = dev;
	aw_pa->regmap = regmap;
	mutex_init(&aw_pa->dsp_lock);
	aw_pa->chip_id = chip_id;
	aw_pa->acf = NULL;
	aw_pa->prof_info.prof_desc = NULL;
	aw_pa->prof_info.count = 0;
	aw_pa->prof_info.prof_type = AW88395_DEV_NONE_TYPE_ID;
	aw_pa->channel = AW88399_DEV_DEFAULT_CH;
	aw_pa->fw_status = AW88399_DEV_FW_FAILED;
	aw_pa->fade_step = AW88399_VOLUME_STEP_DB;
	aw_pa->volume_desc.ctl_volume = AW88399_VOL_DEFAULT_VALUE;

	aw_dev->aw_pa = aw_pa;
	*aw88399_dev = aw_dev;

	return 0;
}
EXPORT_SYMBOL_GPL(aw88399_dev_init);

/**
 * aw88399_dev_deinit - Deinitialize AW88399 device
 */
void aw88399_dev_deinit(struct aw88399_dev *aw88399_dev)
{
	if (!aw88399_dev)
		return;

	mutex_destroy(&aw88399_dev->lock);
	if (aw88399_dev->aw_pa)
		mutex_destroy(&aw88399_dev->aw_pa->dsp_lock);
}
EXPORT_SYMBOL_GPL(aw88399_dev_deinit);

/**
 * aw88399_dev_request_firmware - Load firmware for AW88399
 */
int aw88399_dev_request_firmware(struct aw88399_dev *aw88399_dev)
{
	struct aw_device *aw_dev = aw88399_dev->aw_pa;
	const struct firmware *cont = NULL;
	int ret;

	aw_dev->fw_status = AW88399_DEV_FW_FAILED;
	aw88399_dev->fw_status = AW88399_DEV_FW_FAILED;

	ret = request_firmware(&cont, AW88399_ACF_FILE, aw_dev->dev);
	if (ret) {
		dev_err(aw_dev->dev, "failed to load %s: %d", AW88399_ACF_FILE, ret);
		return ret;
	}

	dev_dbg(aw_dev->dev, "loaded %s, size: %zu", AW88399_ACF_FILE, cont->size);

	aw88399_dev->aw_cfg = devm_kzalloc(aw_dev->dev,
					   struct_size(aw88399_dev->aw_cfg, data, cont->size),
					   GFP_KERNEL);
	if (!aw88399_dev->aw_cfg) {
		release_firmware(cont);
		return -ENOMEM;
	}

	aw88399_dev->aw_cfg->len = cont->size;
	memcpy(aw88399_dev->aw_cfg->data, cont->data, cont->size);
	release_firmware(cont);

	ret = aw88395_dev_load_acf_check(aw_dev, aw88399_dev->aw_cfg);
	if (ret) {
		dev_err(aw_dev->dev, "firmware validation failed: %d", ret);
		return ret;
	}

	ret = aw88395_dev_cfg_load(aw_dev, aw88399_dev->aw_cfg);
	if (ret) {
		dev_err(aw_dev->dev, "firmware config load failed: %d", ret);
		return ret;
	}

	/* Capture dither state from firmware for later restore */
	{
		unsigned int reg_val;

		ret = regmap_read(aw_dev->regmap, AW88399_DBGCTRL_REG, &reg_val);
		if (!ret)
			aw88399_dev->dither_st = reg_val & (~AW88399_DITHER_EN_MASK);
	}

	aw_dev->fade_in_time = AW88399_1000_US / 10;
	aw_dev->fade_out_time = AW88399_1000_US >> 1;
	aw_dev->prof_cur = aw_dev->prof_info.prof_desc[0].id;
	aw_dev->prof_index = aw_dev->prof_info.prof_desc[0].id;

	aw_dev->fw_status = AW88399_DEV_FW_OK;
	aw88399_dev->fw_status = AW88399_DEV_FW_OK;

	dev_info(aw_dev->dev, "firmware loaded successfully");
	return 0;
}
EXPORT_SYMBOL_GPL(aw88399_dev_request_firmware);

/**
 * aw88399_dev_start - Start AW88399 playback
 */
int aw88399_dev_start(struct aw88399_dev *aw88399_dev)
{
	struct aw_device *aw_dev;
	int ret, i;

	if (!aw88399_dev || !aw88399_dev->aw_pa)
		return -EINVAL;

	aw_dev = aw88399_dev->aw_pa;

	if (aw88399_dev->fw_status != AW88399_DEV_FW_OK) {
		dev_err(aw88399_dev->dev, "firmware not ready");
		return -EPERM;
	}

	/* Already running? */
	if (aw_dev->status == AW88399_DEV_PW_ON)
		return 0;

	/* Update firmware/DSP config before starting (required for proper operation) */
	ret = aw88395_dev_fw_update(aw_dev, AW88395_DSP_FW_UPDATE_OFF, true);
	if (ret) {
		dev_err(aw88399_dev->dev, "fw update failed: %d", ret);
		return ret;
	}

	mutex_lock(&aw88399_dev->lock);

	for (i = 0; i < AW88399_START_RETRIES; i++) {
		ret = aw88399_dev_start_internal(aw88399_dev);
		if (!ret)
			break;
		dev_warn(aw88399_dev->dev, "start attempt %d failed: %d", i + 1, ret);
		/* On failure, try updating DSP firmware before retry */
		aw88395_dev_fw_update(aw_dev, AW88395_DSP_FW_UPDATE_ON, true);
		usleep_range(AW88399_2000_US, AW88399_2000_US + 100);
	}

	mutex_unlock(&aw88399_dev->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(aw88399_dev_start);

/**
 * aw88399_dev_stop - Stop AW88399 playback
 */
int aw88399_dev_stop(struct aw88399_dev *aw88399_dev)
{
	struct aw_device *aw_dev;
	int int_st;

	if (!aw88399_dev || !aw88399_dev->aw_pa)
		return -EINVAL;

	aw_dev = aw88399_dev->aw_pa;

	mutex_lock(&aw88399_dev->lock);

	if (aw_dev->status == AW88399_DEV_PW_OFF) {
		dev_dbg(aw_dev->dev, "already powered off");
		mutex_unlock(&aw88399_dev->lock);
		return 0;
	}

	aw_dev->status = AW88399_DEV_PW_OFF;

	aw88399_lib_dev_mute(aw_dev, true);
	usleep_range(AW88399_4000_US, AW88399_4000_US + 100);

	aw_dev_i2s_tx_enable(aw_dev, false);
	usleep_range(AW88399_1000_US, AW88399_1000_US + 100);

	int_st = aw_dev_check_sysint(aw_dev);

	aw_dev_dsp_enable(aw_dev, false);
	aw_dev_amppd(aw_dev, true);

	/* If stop detected errors, re-upload DSP firmware to recover */
	if (int_st) {
		dev_warn(aw_dev->dev, "stop error detected, re-uploading DSP");
		aw88395_dev_fw_update(aw_dev, AW88395_DSP_FW_UPDATE_ON, true);
	}

	aw_dev_pwd(aw_dev, true);

	mutex_unlock(&aw88399_dev->lock);

	dev_dbg(aw_dev->dev, "device stopped");
	return int_st;
}
EXPORT_SYMBOL_GPL(aw88399_dev_stop);

/**
 * aw88399_dev_fw_update - Update firmware if needed
 */
int aw88399_dev_fw_update(struct aw88399_dev *aw88399_dev,
			  bool up_dsp_fw_en, bool force_up_en)
{
	/* For now, just return success - full implementation would
	 * check profile changes and update DSP config accordingly.
	 * The current implementation loads firmware once at init.
	 */
	if (!aw88399_dev || aw88399_dev->fw_status != AW88399_DEV_FW_OK)
		return -EPERM;

	return 0;
}
EXPORT_SYMBOL_GPL(aw88399_dev_fw_update);

/**
 * aw88399_dev_set_channel - Set audio channel assignment
 */
void aw88399_dev_set_channel(struct aw88399_dev *aw88399_dev, unsigned int channel)
{
	if (aw88399_dev && aw88399_dev->aw_pa)
		aw88399_dev->aw_pa->channel = channel;
}
EXPORT_SYMBOL_GPL(aw88399_dev_set_channel);

/**
 * aw88399_dev_get_channel - Get audio channel assignment
 */
unsigned int aw88399_dev_get_channel(struct aw88399_dev *aw88399_dev)
{
	if (aw88399_dev && aw88399_dev->aw_pa)
		return aw88399_dev->aw_pa->channel;
	return 0;
}
EXPORT_SYMBOL_GPL(aw88399_dev_get_channel);

/**
 * aw88399_dev_is_fw_ready - Check if firmware is loaded and ready
 */
bool aw88399_dev_is_fw_ready(struct aw88399_dev *aw88399_dev)
{
	return aw88399_dev && aw88399_dev->fw_status == AW88399_DEV_FW_OK;
}
EXPORT_SYMBOL_GPL(aw88399_dev_is_fw_ready);

/**
 * aw88399_dev_get_status - Get device power status
 */
int aw88399_dev_get_status(struct aw88399_dev *aw88399_dev)
{
	if (aw88399_dev && aw88399_dev->aw_pa)
		return aw88399_dev->aw_pa->status;
	return AW88399_DEV_PW_OFF;
}
EXPORT_SYMBOL_GPL(aw88399_dev_get_status);

/**
 * aw88399_dev_get_aw_device - Get underlying aw_device for advanced use
 */
struct aw_device *aw88399_dev_get_aw_device(struct aw88399_dev *aw88399_dev)
{
	return aw88399_dev ? aw88399_dev->aw_pa : NULL;
}
EXPORT_SYMBOL_GPL(aw88399_dev_get_aw_device);

MODULE_DESCRIPTION("AW88399 device library");
MODULE_LICENSE("GPL");
