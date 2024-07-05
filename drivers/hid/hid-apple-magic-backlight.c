// SPDX-License-Identifier: GPL-2.0
/*
 * Apple Magic Keyboard Backlight Driver
 *
 * For Intel Macs with internal Magic Keyboard (MacBookPro16,1-4 and MacBookAir9,1)
 *
 * Copyright (c) 2022 Kerem Karabay <kekrby@gmail.com>
 * Copyright (c) 2023 Orlando Chamberlain <orlandoch.dev@gmail.com>
 */

#include <linux/hid.h>
#include <linux/leds.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <dt-bindings/leds/common.h>

#include "hid-ids.h"

#define HID_USAGE_MAGIC_BL	0xff00000f

#define APPLE_MAGIC_REPORT_ID_POWER 3
#define APPLE_MAGIC_REPORT_ID_BRIGHTNESS 1

struct apple_magic_backlight {
	struct led_classdev cdev;
	struct hid_report *brightness;
	struct hid_report *power;
};

static void apple_magic_backlight_report_set(struct hid_report *rep, s32 value, u8 rate)
{
	rep->field[0]->value[0] = value;
	rep->field[1]->value[0] = 0x5e; /* Mimic Windows */
	rep->field[1]->value[0] |= rate << 8;

	hid_hw_request(rep->device, rep, HID_REQ_SET_REPORT);
}

static void apple_magic_backlight_set(struct apple_magic_backlight *backlight,
				     int brightness, char rate)
{
	apple_magic_backlight_report_set(backlight->power, brightness ? 1 : 0, rate);
	if (brightness)
		apple_magic_backlight_report_set(backlight->brightness, brightness, rate);
}

static int apple_magic_backlight_led_set(struct led_classdev *led_cdev,
					 enum led_brightness brightness)
{
	struct apple_magic_backlight *backlight = container_of(led_cdev,
			struct apple_magic_backlight, cdev);

	apple_magic_backlight_set(backlight, brightness, 1);
	return 0;
}

static int apple_magic_backlight_probe(struct hid_device *hdev,
				       const struct hid_device_id *id)
{
	struct apple_magic_backlight *backlight;
	int rc;

	rc = hid_parse(hdev);
	if (rc)
		return rc;

	/*
	 * Ensure this usb endpoint is for the keyboard backlight, not touchbar
	 * backlight.
	 */
	if (hdev->collection[0].usage != HID_USAGE_MAGIC_BL)
		return -ENODEV;

	backlight = devm_kzalloc(&hdev->dev, sizeof(*backlight), GFP_KERNEL);
	if (!backlight)
		return -ENOMEM;

	rc = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (rc)
		return rc;

	backlight->brightness = hid_register_report(hdev, HID_FEATURE_REPORT,
			APPLE_MAGIC_REPORT_ID_BRIGHTNESS, 0);
	backlight->power = hid_register_report(hdev, HID_FEATURE_REPORT,
			APPLE_MAGIC_REPORT_ID_POWER, 0);

	if (!backlight->brightness || !backlight->power) {
		rc = -ENODEV;
		goto hw_stop;
	}

	backlight->cdev.name = ":white:" LED_FUNCTION_KBD_BACKLIGHT;
	backlight->cdev.max_brightness = backlight->brightness->field[0]->logical_maximum;
	backlight->cdev.brightness_set_blocking = apple_magic_backlight_led_set;

	apple_magic_backlight_set(backlight, 0, 0);

	return devm_led_classdev_register(&hdev->dev, &backlight->cdev);

hw_stop:
	hid_hw_stop(hdev);
	return rc;
}

static const struct hid_device_id apple_magic_backlight_hid_ids[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_TOUCHBAR_BACKLIGHT) },
	{ }
};
MODULE_DEVICE_TABLE(hid, apple_magic_backlight_hid_ids);

static struct hid_driver apple_magic_backlight_hid_driver = {
	.name = "hid-apple-magic-backlight",
	.id_table = apple_magic_backlight_hid_ids,
	.probe = apple_magic_backlight_probe,
};
module_hid_driver(apple_magic_backlight_hid_driver);

MODULE_DESCRIPTION("MacBook Magic Keyboard Backlight");
MODULE_AUTHOR("Orlando Chamberlain <orlandoch.dev@gmail.com>");
MODULE_LICENSE("GPL");
