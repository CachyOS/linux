// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Apple SMC Keyboard backlight driver
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/leds.h>
#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>

struct macsmc_led {
	struct led_classdev cdev;
	struct apple_smc *smc;
	u8 brightness;
};

static enum led_brightness macsmc_led_get_brightness(struct led_classdev *cdev)
{
	struct macsmc_led *drv = container_of(cdev, struct macsmc_led, cdev);

	return drv->brightness;
}

static void macsmc_led_set_brightness(struct led_classdev *cdev, enum led_brightness value)
{
	struct macsmc_led *drv = container_of(cdev, struct macsmc_led, cdev);

	drv->brightness = (u8) value;

	apple_smc_write_u16(drv->smc, SMC_KEY(LKSB), drv->brightness);
}

static int macsmc_led_probe(struct platform_device *pdev)
{
	struct macsmc_led *drv;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->smc = dev_get_drvdata(pdev->dev.parent);

	drv->cdev.name = "macsmc-led";
	drv->cdev.max_brightness = 255;
	drv->cdev.brightness_get = macsmc_led_get_brightness;
	drv->cdev.brightness_set = macsmc_led_set_brightness;
	drv->cdev.dev = &pdev->dev;

	return devm_led_classdev_register(&pdev->dev, &drv->cdev);
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
