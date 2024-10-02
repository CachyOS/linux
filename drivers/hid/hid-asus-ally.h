/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include <linux/hid.h>
#include <linux/types.h>

/*
 * the xpad_mode is used inside the mode setting packet and is used
 * for indexing (xpad_mode - 1)
 */
enum xpad_mode {
	xpad_mode_game = 0x01,
	xpad_mode_wasd = 0x02,
	xpad_mode_mouse = 0x03,
};

/* the xpad_cmd determines which feature is set or queried */
enum xpad_cmd {
	xpad_cmd_set_mapping = 0x02,
	xpad_cmd_set_leds = 0x08,
	xpad_cmd_check_ready = 0x0A,
};

/* the xpad_cmd determines which feature is set or queried */
enum xpad_cmd_len {
	xpad_cmd_len_mapping = 0x2c,
	xpad_cmd_len_leds = 0x0C,
};

/* Values correspond to the actual HID byte value required */
enum btn_pair_index {
	btn_pair_m1_m2 = 0x08,
};

#define BTN_KB_M2             0x02008E0000000000
#define BTN_KB_M1             0x02008F0000000000

#define ALLY_DEVICE_ATTR_WO(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0200, NULL, _name##_store)

/* required so we can have nested attributes with same name but different functions */
#define ALLY_DEVICE_ATTR_RW(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0644, _name##_show, _name##_store)
