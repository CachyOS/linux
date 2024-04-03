// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

/* data that is private to the hid-asus-rog module */

#include <linux/hid.h>
#include <linux/types.h>

#define BTN_CODE_LEN 11
#define MAPPING_BLOCK_LEN 44

#define TURBO_BLOCK_LEN 32
#define TURBO_BLOCK_STEP 2

#define PAD_A "pad_a"
#define PAD_B "pad_b"
#define PAD_X "pad_x"
#define PAD_Y "pad_y"
#define PAD_LB "pad_lb"
#define PAD_RB "pad_rb"
#define PAD_LS "pad_ls"
#define PAD_RS "pad_rs"
#define PAD_DPAD_UP "pad_dpad_up"
#define PAD_DPAD_DOWN "pad_dpad_down"
#define PAD_DPAD_LEFT "pad_dpad_left"
#define PAD_DPAD_RIGHT "pad_dpad_right"
#define PAD_VIEW "pad_view"
#define PAD_MENU "pad_menu"
#define PAD_XBOX "pad_xbox"

#define KB_M1 "kb_m1"
#define KB_M2 "kb_m2"
#define KB_ESC "kb_esc"
#define KB_F1 "kb_f1"
#define KB_F2 "kb_f2"
#define KB_F3 "kb_f3"
#define KB_F4 "kb_f4"
#define KB_F5 "kb_f5"
#define KB_F6 "kb_f6"
#define KB_F7 "kb_f7"
#define KB_F8 "kb_f8"
#define KB_F9 "kb_f9"
#define KB_F10 "kb_f10"
#define KB_F11 "kb_f11"
#define KB_F12 "kb_f12"
#define KB_F14 "kb_f14"
#define KB_F15 "kb_f15"

#define KB_BACKTICK "kb_backtick"
#define KB_1 "kb_1"
#define KB_2 "kb_2"
#define KB_3 "kb_3"
#define KB_4 "kb_4"
#define KB_5 "kb_5"
#define KB_6 "kb_6"
#define KB_7 "kb_7"
#define KB_8 "kb_8"
#define KB_9 "kb_9"
#define KB_0 "kb_0"
#define KB_HYPHEN "kb_hyphen"
#define KB_EQUALS "kb_equals"
#define KB_BACKSPACE "kb_backspace"

#define KB_TAB "kb_tab"
#define KB_Q "kb_q"
#define KB_W "kb_w"
#define KB_E "kb_e"
#define KB_R "kb_r"
#define KB_T "kb_t"
#define KB_Y "kb_y"
#define KB_U "kb_u"
#define KB_I "kb_i"
#define KB_O "kb_o"
#define KB_P "kb_p"
#define KB_LBRACKET "kb_lbracket"
#define KB_RBRACKET "kb_rbracket"
#define KB_BACKSLASH "kb_bkslash"

#define KB_CAPS "kb_caps"
#define KB_A "kb_a"
#define KB_S "kb_s"
#define KB_D "kb_d"
#define KB_F "kb_f"
#define KB_G "kb_g"
#define KB_H "kb_h"
#define KB_J "kb_j"
#define KB_K "kb_k"
#define KB_L "kb_l"
#define KB_SEMI "kb_semicolon"
#define KB_QUOTE "kb_quote"
#define KB_RET "kb_enter"

#define KB_LSHIFT "kb_lshift"
#define KB_Z "kb_z"
#define KB_X "kb_x"
#define KB_C "kb_c"
#define KB_V "kb_v"
#define KB_B "kb_b"
#define KB_N "kb_n"
#define KB_M "kb_m"
#define KB_COMMA "kb_comma"
#define KB_PERIOD "kb_period"
#define KB_FWDSLASH "kb_fwdslash"
#define KB_RSHIFT "kb_rshift"

#define KB_LCTL "kb_lctl"
#define KB_META "kb_meta"
#define KB_LALT "kb_lalt"
#define KB_SPACE "kb_space"
#define KB_RALT "kb_ralt"
#define KB_MENU "kb_menu"
#define KB_RCTL "kb_rctl"

#define KB_PRNTSCN "kb_prntscn"
#define KB_SCRLCK "kb_scrlck"
#define KB_PAUSE "kb_pause"
#define KB_INS "kb_ins"
#define KB_HOME "kb_home"
#define KB_PGUP "kb_pgup"
#define KB_DEL "kb_del"
#define KB_END "kb_end"
#define KB_PGDWN "kb_pgdwn"

#define KB_UP_ARROW "kb_up_arrow"
#define KB_DOWN_ARROW "kb_down_arrow"
#define KB_LEFT_ARROW "kb_left_arrow"
#define KB_RIGHT_ARROW "kb_right_arrow"

#define NUMPAD_LOCK "numpad_lock"
#define NUMPAD_FWDSLASH "numpad_fwdslash"
#define NUMPAD_ASTERISK "numpad_asterisk"
#define NUMPAD_HYPHEN "numpad_hyphen"
#define NUMPAD_0 "numpad_0"
#define NUMPAD_1 "numpad_1"
#define NUMPAD_2 "numpad_2"
#define NUMPAD_3 "numpad_3"
#define NUMPAD_4 "numpad_4"
#define NUMPAD_5 "numpad_5"
#define NUMPAD_6 "numpad_6"
#define NUMPAD_7 "numpad_7"
#define NUMPAD_8 "numpad_8"
#define NUMPAD_9 "numpad_9"
#define NUMPAD_PLUS "numpad_plus"
#define NUMPAD_ENTER "numpad_enter"
#define NUMPAD_PERIOD "numpad_."

#define RAT_LCLICK "rat_lclick"
#define RAT_RCLICK "rat_rclick"
#define RAT_MCLICK "rat_mclick"
#define RAT_WHEEL_UP "rat_wheel_up"
#define RAT_WHEEL_DOWN "rat_wheel_down"

#define MEDIA_SCREENSHOT "media_screenshot"
#define MEDIA_SHOW_KEYBOARD "media_show_keyboard"
#define MEDIA_SHOW_DESKTOP "media_show_desktop"
#define MEDIA_START_RECORDING "media_start_recording"
#define MEDIA_MIC_OFF "media_mic_off"
#define MEDIA_VOL_DOWN "media_vol_down"
#define MEDIA_VOL_UP "media_vol_up"

/* required so we can have nested attributes with same name but different functions */
#define ALLY_DEVICE_ATTR_RW(_name, _sysfs_name)    \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_sysfs_name, 0644, _name##_show, _name##_store)

#define ALLY_DEVICE_ATTR_RO(_name, _sysfs_name) \
	struct device_attribute dev_attr_##_name = __ATTR(_sysfs_name, 0444, _name##_show, NULL)

#define ALLY_DEVICE_ATTR_WO(_name, _sysfs_name) \
	struct device_attribute dev_attr_##_name = __ATTR(_sysfs_name, 0200, NULL, _name##_store)

/* response curve macros */
#define ALLY_RESP_CURVE_SHOW(_name, _point_n)                                                    \
	static ssize_t _name##_show(struct device *raw_dev, struct device_attribute *attr,       \
				    char *buf)                                                   \
	{                                                                                        \
		struct asus_rog_ally *rog_ally = __rog_ally_data(raw_dev);                       \
		int idx = (_point_n - 1) * 2;                                                    \
		return sysfs_emit(                                                               \
			buf, "%d %d\n",                                                          \
			rog_ally->response_curve[rog_ally->mode][btn_pair_side_left][idx],       \
			rog_ally->response_curve[rog_ally->mode][btn_pair_side_right][idx + 1]); \
	}

#define ALLY_RESP_CURVE_STORE(_name, _side, _point_n)                                         \
	static ssize_t _name##_store(struct device *raw_dev, struct device_attribute *attr,   \
				     const char *buf, size_t count)                           \
	{                                                                                     \
		int ret = __gamepad_store_response_curve(raw_dev, buf, btn_pair_side_##_side, \
							 _point_n);                           \
		if (ret < 0)                                                                  \
			return ret;                                                           \
		return count;                                                                 \
	}

/* _point_n must start at 1 */
#define ALLY_JS_RC_POINT(_side, _point_n, _sysfs_label)                        \
	ALLY_RESP_CURVE_SHOW(rc_point_##_side##_##_point_n, _point_n);         \
	ALLY_RESP_CURVE_STORE(rc_point_##_side##_##_point_n, _side, _point_n); \
	ALLY_DEVICE_ATTR_RW(rc_point_##_side##_##_point_n, _sysfs_label##_point_n);

/* deadzone macros */
#define ALLY_AXIS_DEADZONE_SHOW(_axis)                                                    \
	static ssize_t _axis##_deadzone_show(struct device *raw_dev,                      \
					     struct device_attribute *attr, char *buf)    \
	{                                                                                 \
		struct asus_rog_ally *rog_ally = __rog_ally_data(raw_dev);                \
		int side, is_tr;                                                          \
                                                                                          \
		is_tr = _axis > xpad_axis_xy_right;                                       \
		side = _axis == xpad_axis_xy_right || _axis == xpad_axis_z_right ? 2 : 0; \
                                                                                          \
		return sysfs_emit(buf, "%d %d\n",                                         \
				  rog_ally->deadzones[rog_ally->mode][is_tr][side],       \
				  rog_ally->deadzones[rog_ally->mode][is_tr][side + 1]);  \
	}

#define ALLY_AXIS_DEADZONE_STORE(_axis)                                                       \
	static ssize_t _axis##_deadzone_store(struct device *raw_dev,                         \
					      struct device_attribute *attr, const char *buf, \
					      size_t count)                                   \
	{                                                                                     \
		int ret = __gamepad_store_deadzones(raw_dev, _axis, buf);                     \
		if (ret < 0)                                                                  \
			return ret;                                                           \
		return count;                                                                 \
	}

#define ALLY_AXIS_DEADZONE(_axis, _sysfs_label) \
	ALLY_AXIS_DEADZONE_SHOW(_axis);         \
	ALLY_AXIS_DEADZONE_STORE(_axis);        \
	ALLY_DEVICE_ATTR_RW(_axis##_deadzone, _sysfs_label);

/* button specific macros */
#define ALLY_BTN_SHOW(_fname, _pair, _side, _secondary)                                     \
	static ssize_t _fname##_show(struct device *raw_dev, struct device_attribute *attr, \
				     char *buf)                                             \
	{                                                                                   \
		return sysfs_emit(buf, "%s\n",                                              \
				  __btn_map_to_string(raw_dev, _pair, _side, _secondary));  \
	}

#define ALLY_BTN_STORE(_fname, _pair, _side, _secondary)                                     \
	static ssize_t _fname##_store(struct device *raw_dev, struct device_attribute *attr, \
				      const char *buf, size_t count)                         \
	{                                                                                    \
		int ret = __gamepad_mapping_store(raw_dev, buf, _pair, _side, _secondary);   \
		if (ret < 0)                                                                 \
			return ret;                                                          \
		return count;                                                                \
	}

#define ALLY_BTN_TURBO_SHOW(_fname, _pair, _side)                                                 \
	static ssize_t _fname##_turbo_show(struct device *raw_dev, struct device_attribute *attr, \
					   char *buf)                                             \
	{                                                                                         \
		return sysfs_emit(buf, "%d\n", __gamepad_turbo_show(raw_dev, _pair, _side));      \
	}

#define ALLY_BTN_TURBO_STORE(_fname, _pair, _side)                                                 \
	static ssize_t _fname##_turbo_store(struct device *raw_dev, struct device_attribute *attr, \
					    const char *buf, size_t count)                         \
	{                                                                                          \
		int ret = __gamepad_turbo_store(raw_dev, buf, _pair, _side);                       \
		if (ret < 0)                                                                       \
			return ret;                                                                \
		return count;                                                                      \
	}

#define ALLY_BTN_ATTRS_GROUP(_name, _fname)                                                    \
	static struct attribute *_fname##_attrs[] = { &dev_attr_##_fname.attr,                 \
						      &dev_attr_##_fname##_macro.attr,         \
						      &dev_attr_##_fname##_turbo.attr, NULL }; \
	static const struct attribute_group _fname##_attr_group = {                            \
		.name = __stringify(_name),                                                    \
		.attrs = _fname##_attrs,                                                       \
	};

#define ALLY_BTN_MAPPING(_fname, _pair, _side)                            \
	ALLY_BTN_SHOW(btn_mapping_##_fname, _pair, _side, false);         \
	ALLY_BTN_STORE(btn_mapping_##_fname, _pair, _side, false);        \
                                                                          \
	ALLY_BTN_SHOW(btn_mapping_##_fname##_macro, _pair, _side, true);  \
	ALLY_BTN_STORE(btn_mapping_##_fname##_macro, _pair, _side, true); \
                                                                          \
	ALLY_BTN_TURBO_SHOW(btn_mapping_##_fname, _pair, _side);          \
	ALLY_BTN_TURBO_STORE(btn_mapping_##_fname, _pair, _side);         \
                                                                          \
	ALLY_DEVICE_ATTR_RW(btn_mapping_##_fname, remap);                 \
	ALLY_DEVICE_ATTR_RW(btn_mapping_##_fname##_macro, macro_remap);   \
	ALLY_DEVICE_ATTR_RW(btn_mapping_##_fname##_turbo, turbo);         \
                                                                          \
	ALLY_BTN_ATTRS_GROUP(btn_##_fname, btn_mapping_##_fname);

/* calibration macros */
#define ALLY_CAL_STORE(_fname, _axis)                                                  \
	static ssize_t _fname##_store(struct device *raw_dev, struct device_attribute *attr, \
				      const char *buf, size_t count)                         \
	{                                                                                    \
		int ret = __gamepad_cal_store(raw_dev, buf, _axis);                          \
		if (ret < 0)                                                                 \
			return ret;                                                          \
		return count;                                                                \
	};

#define ALLY_CAL_SHOW(_fname, _axis)                                     \
	static ssize_t _fname##_show(struct device *raw_dev, struct device_attribute *attr, \
				     char *buf)                                             \
	{                                                                                   \
		return __gamepad_cal_show(raw_dev, buf, _axis);  \
	}

#define ALLY_CAL_ATTR(_fname, _axis, _sysfs_label) \
	ALLY_CAL_STORE(_fname, _axis);             \
	ALLY_CAL_SHOW(_fname, _axis);             \
	ALLY_DEVICE_ATTR_RW(_fname, _sysfs_label);

#define ALLY_CAL_RESET_STORE(_fname, _axis)                                                  \
	static ssize_t _fname##_store(struct device *raw_dev, struct device_attribute *attr, \
				      const char *buf, size_t count)                         \
	{                                                                                    \
		int ret = __gamepad_cal_reset(raw_dev, buf, _axis);                          \
		if (ret < 0)                                                                 \
			return ret;                                                          \
		return count;                                                                \
	};

#define ALLY_CAL_RESET_ATTR(_fname, _axis, _sysfs_label) \
	ALLY_CAL_RESET_STORE(_fname, _axis);             \
	ALLY_DEVICE_ATTR_WO(_fname, _sysfs_label);

/* Default blocks for the xpad mode */
static const u8 XPAD_DEF1[MAPPING_BLOCK_LEN] = {
	0x01, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x8c, 0x88, 0x76, 0x00, 0x00
};
static const u8 XPAD_DEF2[MAPPING_BLOCK_LEN] = {
	0x01, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x82, 0x23, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x82, 0x0d, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF3[MAPPING_BLOCK_LEN] = {
	0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF4[MAPPING_BLOCK_LEN] = {
	0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF5[MAPPING_BLOCK_LEN] = {
	0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x82, 0x31, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF6[MAPPING_BLOCK_LEN] = {
	0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x82, 0x4d, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF7[MAPPING_BLOCK_LEN] = {
	0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF8[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x8e, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 XPAD_DEF9[MAPPING_BLOCK_LEN] = {
	0x01, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* default blocks for the wasd mode */
static const u8 WASD_DEF1[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x8c, 0x88, 0x76, 0x00, 0x00
};
static const u8 WASD_DEF2[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x9a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x82, 0x23, 0x00, 0x00, 0x00, 0x02, 0x00, 0x9b, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x82, 0x0d, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF3[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF4[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF5[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x82, 0x31, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF6[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x97, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x82, 0x4d, 0x00, 0x00, 0x00, 0x02, 0x00, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF7[MAPPING_BLOCK_LEN] = {
	0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF8[MAPPING_BLOCK_LEN] = {
	0x02, 0x00, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x8e, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 WASD_DEF9[MAPPING_BLOCK_LEN] = {
	0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x88, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

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
	xpad_cmd_set_mode = 0x01,
	xpad_cmd_set_mapping = 0x02,
	xpad_cmd_set_js_dz = 0x04, /* deadzones */
	xpad_cmd_set_tr_dz = 0x05, /* deadzones */
	xpad_cmd_set_vibe_intensity = 0x06,
	xpad_cmd_check_ready = 0x0A,
	xpad_cmd_set_calibration = 0x0D,
	xpad_cmd_set_turbo = 0x0F,
	xpad_cmd_set_response_curve = 0x13,
	xpad_cmd_set_adz = 0x18,
};

/*
 * the xpad_mode is used in various set and query HID packets and is
 * used for indexing (xpad_axis - 1)
 */
enum xpad_axis {
	xpad_axis_xy_left = 0x01,
	xpad_axis_xy_right = 0x02,
	xpad_axis_z_left = 0x03,
	xpad_axis_z_right = 0x04,
};

enum btn_pair {
	btn_pair_dpad_u_d = 0x01,
	btn_pair_dpad_l_r = 0x02,
	btn_pair_ls_rs = 0x03,
	btn_pair_lb_rb = 0x04,
	btn_pair_a_b = 0x05,
	btn_pair_x_y = 0x06,
	btn_pair_view_menu = 0x07,
	btn_pair_m1_m2 = 0x08,
	btn_pair_lt_rt = 0x09,
};

enum btn_pair_side {
	btn_pair_side_left = 0x00,
	btn_pair_side_right = 0x01,
};

static int __gamepad_write_all_to_mcu(struct device *raw_dev);