// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Apple SMC Keyboard backlight driver
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/leds.h>
#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>

#define BACKLIGHT_KEY SMC_KEY(LKSB)

struct macsmc_led {
	struct led_classdev cdev;
	struct apple_smc *smc;
	u8 brightness;
};

static enum led_brightness macsmc_led_get_brightness(struct led_classdev *cdev)
{
	struct macsmc_led *smc_led = container_of(cdev, struct macsmc_led, cdev);

	return smc_led->brightness;
}

static void macsmc_led_set_brightness(struct led_classdev *cdev, enum led_brightness value)
{
	struct macsmc_led *smc_led = container_of(cdev, struct macsmc_led, cdev);

	smc_led->brightness = (u8) value;

	apple_smc_write_u16(smc_led->smc, BACKLIGHT_KEY, smc_led->brightness);
}

static int macsmc_led_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct macsmc_led *smc_led;

	if (!apple_smc_key_exists(smc, BACKLIGHT_KEY))
		return -ENODEV;

	smc_led = devm_kzalloc(&pdev->dev, sizeof(*smc_led), GFP_KERNEL);
	if (!smc_led)
		return -ENOMEM;

	smc_led->smc = smc;

	smc_led->cdev.name = "macsmc-led";
	smc_led->cdev.max_brightness = 255;
	smc_led->cdev.brightness_get = macsmc_led_get_brightness;
	smc_led->cdev.brightness_set = macsmc_led_set_brightness;
	smc_led->cdev.dev = &pdev->dev;

	return devm_led_classdev_register(&pdev->dev, &smc_led->cdev);
}

static struct platform_driver macsmc_led_driver = {
	.probe = macsmc_led_probe,
	.driver = {
		.name = "macsmc-led",
	},
};
module_platform_driver(macsmc_led_driver);

MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_DESCRIPTION("LED keyboard backlight driver for Intel-macs using macsmc");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_ALIAS("platform:macsmc-led");
