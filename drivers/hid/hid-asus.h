// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include <linux/hid.h>
#include <linux/types.h>

#define FEATURE_KBD_REPORT_ID		0x5a
#define FEATURE_KBD_REPORT_SIZE		16
#define FEATURE_KBD_LED_REPORT_ID1	0x5d
#define FEATURE_KBD_LED_REPORT_ID2	0x5e
#define FEATURE_ROG_ALLY_REPORT_SIZE	64

#define QUIRK_FIX_NOTEBOOK_REPORT	BIT(0)
#define QUIRK_NO_INIT_REPORTS		BIT(1)
#define QUIRK_SKIP_INPUT_MAPPING	BIT(2)
#define QUIRK_IS_MULTITOUCH		BIT(3)
#define QUIRK_NO_CONSUMER_USAGES	BIT(4)
#define QUIRK_USE_KBD_BACKLIGHT		BIT(5)
#define QUIRK_T100_KEYBOARD		BIT(6)
#define QUIRK_T100CHI			BIT(7)
#define QUIRK_G752_KEYBOARD		BIT(8)
#define QUIRK_T90CHI			BIT(9)
#define QUIRK_MEDION_E1239T		BIT(10)
#define QUIRK_ROG_NKEY_KEYBOARD		BIT(11)
#define QUIRK_ROG_CLAYMORE_II_KEYBOARD	BIT(12)
#define QUIRK_ROG_ALLY_XPAD		BIT(13)

struct asus_drvdata {
	unsigned long quirks;
	struct hid_device *hdev;
	struct input_dev *input;
	struct input_dev *tp_kbd_input;
	struct asus_kbd_leds *kbd_backlight;
	const struct asus_touchpad_info *tp;
	bool enable_backlight;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	int battery_capacity;
	int battery_stat;
	bool battery_in_query;
	unsigned long battery_next_query;
	struct asus_rog_ally *rog_ally_data;
};

extern int asus_kbd_set_report(struct hid_device *hdev, const u8 *buf, size_t buf_size);

extern int asus_kbd_get_report(struct hid_device *hdev, u8 *out_buf, size_t out_buf_size);

struct rog_ops {
	int (*probe) (struct hid_device *hdev, const struct rog_ops *ops);
	void (*remove) (struct hid_device *hdev, const struct rog_ops *ops);
};

extern const struct rog_ops rog_ally;