/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * aw88399.h -- AW88399 Smart Amplifier shared definitions
 *
 * Copyright (c) 2023 AWINIC Technology CO., LTD
 *
 * Based on aw88399.c by Weidong Wang <wangweidong.a@awinic.com>
 */

#ifndef __SOUND_AW88399_H
#define __SOUND_AW88399_H

#include <linux/regmap.h>
#include <linux/types.h>

struct i2c_client;
struct aw_device;
struct aw_container;

/*
 * AW88399 Register Definitions
 */
#define AW88399_ID_REG			0x00
#define AW88399_SYSST_REG		0x01
#define AW88399_SYSINT_REG		0x02
#define AW88399_SYSINTM_REG		0x03
#define AW88399_SYSCTRL_REG		0x04
#define AW88399_SYSCTRL2_REG		0x05
#define AW88399_I2SCTRL1_REG		0x06
#define AW88399_I2SCTRL2_REG		0x07
#define AW88399_I2SCTRL3_REG		0x08
#define AW88399_DACCFG1_REG		0x09
#define AW88399_DACCFG2_REG		0x0A
#define AW88399_DACCFG3_REG		0x0B
#define AW88399_DACCFG4_REG		0x0C
#define AW88399_DACCFG5_REG		0x0D
#define AW88399_DACCFG6_REG		0x0E
#define AW88399_DACCFG7_REG		0x0F
#define AW88399_MPDCFG1_REG		0x10
#define AW88399_MPDCFG2_REG		0x11
#define AW88399_MPDCFG3_REG		0x12
#define AW88399_MPDCFG4_REG		0x13
#define AW88399_PWMCTRL1_REG		0x14
#define AW88399_PWMCTRL2_REG		0x15
#define AW88399_PWMCTRL3_REG		0x16
#define AW88399_I2SCFG1_REG		0x17
#define AW88399_DBGCTRL_REG		0x18
#define AW88399_HAGCST_REG		0x20
#define AW88399_VBAT_REG		0x21
#define AW88399_TEMP_REG		0x22
#define AW88399_PVDD_REG		0x23
#define AW88399_ISNDAT_REG		0x24
#define AW88399_VSNDAT_REG		0x25
#define AW88399_I2SINT_REG		0x26
#define AW88399_I2SCAPCNT_REG		0x27
#define AW88399_ANASTA1_REG		0x28
#define AW88399_ANASTA2_REG		0x29
#define AW88399_ANASTA3_REG		0x2A
#define AW88399_TESTDET_REG		0x2B
#define AW88399_DSMCFG1_REG		0x30
#define AW88399_DSMCFG2_REG		0x31
#define AW88399_DSMCFG3_REG		0x32
#define AW88399_DSMCFG4_REG		0x33
#define AW88399_DSMCFG5_REG		0x34
#define AW88399_DSMCFG6_REG		0x35
#define AW88399_DSMCFG7_REG		0x36
#define AW88399_DSMCFG8_REG		0x37
#define AW88399_TESTIN_REG		0x38
#define AW88399_TESTOUT_REG		0x39
#define AW88399_MEMTEST_REG		0x3A
#define AW88399_VSNCTRL1_REG		0x3B
#define AW88399_ISNCTRL1_REG		0x3C
#define AW88399_ISNCTRL2_REG		0x3D
#define AW88399_DSPMADD_REG		0x40
#define AW88399_DSPMDAT_REG		0x41
#define AW88399_WDT_REG			0x42
#define AW88399_ACR1_REG		0x43
#define AW88399_ACR2_REG		0x44
#define AW88399_ASR1_REG		0x45
#define AW88399_ASR2_REG		0x46
#define AW88399_DSPCFG_REG		0x47
#define AW88399_ASR3_REG		0x48
#define AW88399_ASR4_REG		0x49
#define AW88399_DSPVCALB_REG		0x4A
#define AW88399_CRCCTRL_REG		0x4B
#define AW88399_DSPDBG1_REG		0x4C
#define AW88399_DSPDBG2_REG		0x4D
#define AW88399_DSPDBG3_REG		0x4E
#define AW88399_PLLCTRL1_REG		0x50
#define AW88399_PLLCTRL2_REG		0x51
#define AW88399_PLLCTRL3_REG		0x52
#define AW88399_CDACTRL1_REG		0x53
#define AW88399_CDACTRL2_REG		0x54
#define AW88399_CDACTRL3_REG		0x55
#define AW88399_SADCCTRL1_REG		0x56
#define AW88399_SADCCTRL2_REG		0x57
#define AW88399_BOPCTRL1_REG		0x58
#define AW88399_BOPCTRL2_REG		0x5A
#define AW88399_BOPCTRL3_REG		0x5B
#define AW88399_BOPCTRL4_REG		0x5C
#define AW88399_BOPCTRL5_REG		0x5D
#define AW88399_BOPCTRL6_REG		0x5E
#define AW88399_BOPCTRL7_REG		0x5F
#define AW88399_BSTCTRL1_REG		0x60
#define AW88399_BSTCTRL2_REG		0x61
#define AW88399_BSTCTRL3_REG		0x62
#define AW88399_BSTCTRL4_REG		0x63
#define AW88399_BSTCTRL5_REG		0x64
#define AW88399_BSTCTRL6_REG		0x65
#define AW88399_BSTCTRL7_REG		0x66
#define AW88399_BSTCTRL8_REG		0x67
#define AW88399_BSTCTRL9_REG		0x68
#define AW88399_BSTCTRL10_REG		0x69
#define AW88399_CPCTRL_REG		0x6A
#define AW88399_EFWH_REG		0x6C
#define AW88399_EFWM2_REG		0x6D
#define AW88399_EFWM1_REG		0x6E
#define AW88399_EFWL_REG		0x6F
#define AW88399_TESTCTRL1_REG		0x70
#define AW88399_TESTCTRL2_REG		0x71
#define AW88399_EFCTRL1_REG		0x72
#define AW88399_EFCTRL2_REG		0x73
#define AW88399_EFRH4_REG		0x74
#define AW88399_EFRH3_REG		0x75
#define AW88399_EFRH2_REG		0x76
#define AW88399_EFRH1_REG		0x77
#define AW88399_EFRL4_REG		0x78
#define AW88399_EFRL3_REG		0x79
#define AW88399_EFRL2_REG		0x7A
#define AW88399_EFRL1_REG		0x7B
#define AW88399_TM_REG			0x7C
#define AW88399_TM2_REG			0x7D

#define AW88399_REG_MAX			0x7E

/*
 * Chip identification
 */
#define AW88399_CHIP_ID			0x2183
#define AW88399_ACF_FILE		"aw88399_acf.bin"

/*
 * Device status
 */
enum aw88399_dev_status {
	AW88399_DEV_PW_OFF = 0,
	AW88399_DEV_PW_ON,
};

enum aw88399_dev_fw_status {
	AW88399_DEV_FW_FAILED = 0,
	AW88399_DEV_FW_OK,
};

/*
 * Start mode
 */
enum aw88399_start_mode {
	AW88399_SYNC_START = 0,
	AW88399_ASYNC_START,
};

/*
 * DSP update mode
 */
enum {
	AW88399_DSP_FW_UPDATE_OFF = 0,
	AW88399_DSP_FW_UPDATE_ON = 1,
};

enum {
	AW88399_FORCE_UPDATE_OFF = 0,
	AW88399_FORCE_UPDATE_ON = 1,
};

/*
 * Timing constants (microseconds)
 */
enum {
	AW88399_1000_US = 1000,
	AW88399_2000_US = 2000,
	AW88399_3000_US = 3000,
	AW88399_4000_US = 4000,
};

/*
 * Memory clock selection
 */
enum aw88399_dev_memclk {
	AW88399_DEV_MEMCLK_OSC = 0,
	AW88399_DEV_MEMCLK_PLL = 1,
};

/*
 * AW88399 device context - opaque handle for device library
 */
struct aw88399_dev;

/*
 * Device library functions - shared between ASoC and HDA drivers
 */

int aw88399_dev_init(struct device *dev, struct i2c_client *i2c,
		     struct regmap *regmap, struct aw88399_dev **aw88399_dev);
void aw88399_dev_deinit(struct aw88399_dev *aw88399_dev);

int aw88399_dev_request_firmware(struct aw88399_dev *aw88399_dev);

int aw88399_dev_start(struct aw88399_dev *aw88399_dev);
int aw88399_dev_stop(struct aw88399_dev *aw88399_dev);

int aw88399_dev_fw_update(struct aw88399_dev *aw88399_dev,
			  bool up_dsp_fw_en, bool force_up_en);

void aw88399_dev_set_channel(struct aw88399_dev *aw88399_dev,
			     unsigned int channel);
unsigned int aw88399_dev_get_channel(struct aw88399_dev *aw88399_dev);

bool aw88399_dev_is_fw_ready(struct aw88399_dev *aw88399_dev);
int aw88399_dev_get_status(struct aw88399_dev *aw88399_dev);

struct aw_device *aw88399_dev_get_aw_device(struct aw88399_dev *aw88399_dev);

#endif /* __SOUND_AW88399_H */
