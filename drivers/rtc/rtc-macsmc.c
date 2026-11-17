// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC RTC driver
 * Copyright The Asahi Linux Contributors
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/slab.h>

/* 48-bit RTC */
#define RTC_BYTES 6
#define RTC_BITS (8 * RTC_BYTES)

/* 32768 Hz clock */
#define RTC_SEC_SHIFT 15

static bool force_clkm;
module_param(force_clkm, bool, 0644);
MODULE_PARM_DESC(force_clkm, "Force CLKM Key, instead of trying to use CLKL");

struct macsmc_rtc {
	struct device *dev;
	struct apple_smc *smc;
	struct rtc_device *rtc_dev;
	struct nvmem_cell *rtc_offset;
	uint8_t shift;
	bool async_rtc;
};

static int macsmc_rtc_read_clkl(struct macsmc_rtc *rtc, u64 *ctr)
{
	u64 request = 0xffffffffffffULL;
	u64 response = 0;
	int i, j, ret;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 40; j++) {
			ret = apple_smc_write(rtc->smc, SMC_KEY(CLKL), &request, RTC_BYTES);
			if (ret == RTC_BYTES)
				break;
		}
		if (ret != RTC_BYTES)
			continue;

		msleep(20);

		for (j = 0; j < 20; j++) {
			ret = apple_smc_read(rtc->smc, SMC_KEY(CLKL), &response, RTC_BYTES);
			if (ret == RTC_BYTES) {
				if ((response & 0x7fff) == 5 || (response & 0x7fff) == 6)
					return -EAGAIN;

				if (response != 0xffffffffffffULL) {
					*ctr = response;
					return RTC_BYTES;
				}
			}
			fsleep(1000);
		}
	}

	return -EIO;
}

static int macsmc_rtc_get_time(struct device *dev, struct rtc_time *tm)
{
	struct macsmc_rtc *rtc = dev_get_drvdata(dev);
	u64 ctr = 0, off = 0;
	time64_t now;
	void *p_off;
	size_t len;
	int ret;

	if (rtc->async_rtc && !force_clkm)
		ret = macsmc_rtc_read_clkl(rtc, &ctr);
	else
		ret = apple_smc_read(rtc->smc, SMC_KEY(CLKM), &ctr, RTC_BYTES);

	if (ret < 0)
		return ret;
	if (ret != RTC_BYTES)
		return -EIO;

	if (rtc->smc->is_acpi) {
		ret = apple_smc_read(rtc->smc, SMC_KEY(CLKO), &off, RTC_BYTES);
		if (ret < 0)
			return ret;
		if (ret != RTC_BYTES)
			return -EIO;
	} else {
		p_off = nvmem_cell_read(rtc->rtc_offset, &len);
		if (IS_ERR(p_off))
			return PTR_ERR(p_off);
		if (len < RTC_BYTES) {
			kfree(p_off);
			return -EIO;
		}

		memcpy(&off, p_off, RTC_BYTES);
		kfree(p_off);
	}
	/* Sign extend from 48 to 64 bits, then arithmetic shift right 15 bits to get seconds */
	now = sign_extend64(ctr + off, RTC_BITS - 1) >> rtc->shift;
	rtc_time64_to_tm(now, tm);

	return ret;
}

static int macsmc_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct macsmc_rtc *rtc = dev_get_drvdata(dev);
	u64 ctr = 0, off = 0;
	int ret;

	if (rtc->async_rtc && !force_clkm)
		ret = macsmc_rtc_read_clkl(rtc, &ctr);
	else
		ret = apple_smc_read(rtc->smc, SMC_KEY(CLKM), &ctr, RTC_BYTES);
	if (ret < 0)
		return ret;
	if (ret != RTC_BYTES)
		return -EIO;

	/* This sets the offset such that the set second begins now */
	off = (rtc_tm_to_time64(tm) << rtc->shift) - ctr;
	if (rtc->smc->is_acpi)
		return (apple_smc_write(rtc->smc, SMC_KEY(CLKO), &off, RTC_BYTES) != RTC_BYTES);

	return nvmem_cell_write(rtc->rtc_offset, &off, RTC_BYTES);
}

static const struct rtc_class_ops macsmc_rtc_ops = {
	.read_time = macsmc_rtc_get_time,
	.set_time = macsmc_rtc_set_time,
};

static int macsmc_rtc_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct macsmc_rtc *rtc;
	int ret;
	uint32_t clkr = 0xFFFFFFFF;

	/*
	 * MFD will probe this device even without a node in the device tree,
	 * thus bail out early if the SMC on the current machines does not
	 * support RTC and has no node in the device tree.
	 */
	if (!pdev->dev.of_node && !smc->is_acpi)
		return -ENODEV;

	rtc = devm_kzalloc(&pdev->dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->dev = &pdev->dev;
	rtc->smc = smc;

	if (!rtc->smc->is_acpi) {
		rtc->rtc_offset = devm_nvmem_cell_get(&pdev->dev, "rtc_offset");
		if (IS_ERR(rtc->rtc_offset))
			return dev_err_probe(&pdev->dev, PTR_ERR(rtc->rtc_offset),
					     "Failed to get rtc_offset NVMEM cell\n");
	}

	if (rtc->smc->is_acpi) {
		ret = apple_smc_read_u32(rtc->smc, SMC_KEY(CLKR), &clkr);
		if (ret < 0 || clkr == 0xFFFFFFFF)
			rtc->shift = RTC_SEC_SHIFT;
		else
			rtc->shift = ffs(clkr) - 1;

		if (apple_smc_key_exists(rtc->smc, SMC_KEY(CLKL))) {
			u64 clkl;

			ret = apple_smc_read(rtc->smc, SMC_KEY(CLKL), &clkl, RTC_BYTES);
			if (ret == RTC_BYTES && clkl != 0xFFFFFFFFFFFFFFFFULL)
				rtc->async_rtc = true;
		}
	} else
		rtc->shift = RTC_SEC_SHIFT;

	dev_info(&pdev->dev, "%u clock, using %s key\n", (clkr != 0xFFFFFFFF) ? clkr : 32768,
				rtc->async_rtc ? "CLKL" : "CLKM");

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc_dev))
		return PTR_ERR(rtc->rtc_dev);

	rtc->rtc_dev->ops = &macsmc_rtc_ops;
	rtc->rtc_dev->range_min = S64_MIN >> (rtc->shift + (64 - RTC_BITS));
	rtc->rtc_dev->range_max = S64_MAX >> (rtc->shift + (64 - RTC_BITS));

	platform_set_drvdata(pdev, rtc);

	return devm_rtc_register_device(rtc->rtc_dev);
}

static const struct of_device_id macsmc_rtc_of_table[] = {
	{ .compatible = "apple,smc-rtc", },
	{}
};
MODULE_DEVICE_TABLE(of, macsmc_rtc_of_table);

static struct platform_driver macsmc_rtc_driver = {
	.driver = {
		.name = "macsmc-rtc",
		.of_match_table = macsmc_rtc_of_table,
	},
	.probe = macsmc_rtc_probe,
};
module_platform_driver(macsmc_rtc_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC RTC driver");
MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_ALIAS("platform:macsmc-rtc");
