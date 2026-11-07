// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Apple SMC Watchdog Driver
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define WATCHDOG_TIMEOUT 60 /* 60 sec default timeout */

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int timeout = WATCHDOG_TIMEOUT; /* in secounds */
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds. 1 <= timeout <= 255, default="
		 __MODULE_STRING(WATCHDOG_TIMEOUT) ".");

struct macsmc_wdt {
	struct apple_smc *smc;
	bool has_oswd; /* Newer version of watchdogs in macsmc */
	struct watchdog_device wdd;
};

static int macsmc_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	struct macsmc_wdt *drv = watchdog_get_drvdata(wdd);
	int ret;

	if (timeout)
		wdd->timeout = timeout;

	if (drv->has_oswd) {
		ret = apple_smc_write_u16(drv->smc, SMC_KEY(OSWD), (u16)(timeout << 8));
		if (ret < 0)
			return ret;
	} else {
		/*
		 * NATJ = 2, makes the watchdog do force restart if NATi == 0
		 * NATJ = 0, makes the watchdog do nothing if NATi == 0
		 */
		if (timeout == 0) {
			ret = apple_smc_write_u16(drv->smc, SMC_KEY(NATJ), 0);
			if (ret < 0)
				return ret;
		}
		ret = apple_smc_write_u16(drv->smc, SMC_KEY(NATi), (u16)(timeout << 8));
		if (ret < 0)
			return ret;

		if (timeout != 0) {
			ret = apple_smc_write_u16(drv->smc, SMC_KEY(NATJ), 2);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}
static int macsmc_wdt_start(struct watchdog_device *wdd)
{
	return macsmc_wdt_set_timeout(wdd, wdd->timeout);
}
static int macsmc_wdt_stop(struct watchdog_device *wdd)
{
	return macsmc_wdt_set_timeout(wdd, 0);
}

static int macsmc_wdt_ping(struct watchdog_device *wdd)
{
	struct macsmc_wdt *drv = watchdog_get_drvdata(wdd);
	int ret;

	if (drv->has_oswd)
		ret = apple_smc_write_u16(drv->smc, SMC_KEY(OSWD), (u16)(wdd->timeout << 8));
	else
		ret = apple_smc_write_u16(drv->smc, SMC_KEY(NATi), (u16)(wdd->timeout << 8));

	if (ret < 0)
		return ret;

	return 0;
}

static const struct watchdog_ops macsmc_wdt_ops = {
	.owner =	THIS_MODULE,
	.start =	macsmc_wdt_start,
	.stop  =	macsmc_wdt_stop,
	.set_timeout =	macsmc_wdt_set_timeout,
	.ping =	macsmc_wdt_ping,
};

static const struct watchdog_info macsmc_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity = "Macsmc Watchdog timer",
};

static int macsmc_wdt_probe(struct platform_device *pdev)
{
	struct macsmc_wdt *drv;

	drv = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_wdt), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->smc = dev_get_drvdata(pdev->dev.parent);
	if (apple_smc_key_exists(drv->smc, SMC_KEY(OSWD)))
		drv->has_oswd = true;
	else
		drv->has_oswd = false;

	drv->wdd.ops = &macsmc_wdt_ops;
	drv->wdd.info = &macsmc_wdt_info;
	drv->wdd.min_timeout = 1;
	drv->wdd.max_timeout = 255;
	watchdog_init_timeout(&drv->wdd, timeout, &pdev->dev);
	watchdog_set_nowayout(&drv->wdd, nowayout);
	watchdog_set_drvdata(&drv->wdd, drv);

	return devm_watchdog_register_device(&pdev->dev, &drv->wdd);
}

static struct platform_driver macsmc_wdt_driver = {
	.probe = macsmc_wdt_probe,
	.driver = {
		.name = "macsmc-wdt"
	},
};
module_platform_driver(macsmc_wdt_driver);

MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC Watchdog Driver");
MODULE_ALIAS("platform:macsmc-wdt");
