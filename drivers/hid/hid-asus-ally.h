/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include <linux/hid.h>
#include <linux/types.h>

/* the xpad_cmd determines which feature is set or queried */
enum xpad_cmd {
	xpad_cmd_set_mode = 0x01,
	xpad_cmd_set_mapping = 0x02,
	xpad_cmd_set_js_dz = 0x04, /* deadzones */
	xpad_cmd_set_tr_dz = 0x05, /* deadzones */
	xpad_cmd_set_vibe_intensity = 0x06,
	xpad_cmd_set_leds = 0x08,
	xpad_cmd_check_ready = 0x0A,
	xpad_cmd_set_calibration = 0x0D,
	xpad_cmd_set_turbo = 0x0F,
	xpad_cmd_set_response_curve = 0x13,
	xpad_cmd_set_adz = 0x18,
};

/* the xpad_cmd determines which feature is set or queried */
enum xpad_cmd_len {
	xpad_cmd_len_mode = 0x01,
	xpad_cmd_len_mapping = 0x2c,
	xpad_cmd_len_deadzone = 0x04,
	xpad_cmd_len_vibe_intensity = 0x02,
	xpad_cmd_len_leds = 0x0C,
	xpad_cmd_len_calibration2 = 0x01,
	xpad_cmd_len_calibration3 = 0x01,
	xpad_cmd_len_turbo = 0x20,
	xpad_cmd_len_response_curve = 0x09,
	xpad_cmd_len_adz = 0x02,
};

/* required so we can have nested attributes with same name but different functions */
#define ALLY_DEVICE_ATTR_RW(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0644, _name##_show, _name##_store)
