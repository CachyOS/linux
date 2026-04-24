// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC core framework
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

static const struct mfd_cell apple_smc_devs[] = {
	MFD_CELL_NAME("macsmc-input"),
	MFD_CELL_NAME("macsmc-power"),
	MFD_CELL_OF("macsmc-gpio", NULL, NULL, 0, 0, "apple,smc-gpio"),
	MFD_CELL_OF("macsmc-hwmon", NULL, NULL, 0, 0, "apple,smc-hwmon"),
	MFD_CELL_OF("macsmc-reboot", NULL, NULL, 0, 0, "apple,smc-reboot"),
	MFD_CELL_OF("macsmc-rtc", NULL, NULL, 0, 0, "apple,smc-rtc"),
};

static const struct mfd_cell apple_smc_acpi_devs[] = {
	MFD_CELL_NAME("macsmc-hwmon"),
	MFD_CELL_NAME("macsmc-light"),
	MFD_CELL_NAME("macsmc-power"),
	MFD_CELL_NAME("macsmc-rtc"),
};

int apple_smc_read(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	guard(mutex)(&smc->mutex);
	return smc->be->read(smc->be_cookie, key, buf, size);
}
EXPORT_SYMBOL(apple_smc_read);

int apple_smc_write(struct apple_smc *smc, smc_key key, const void *buf, size_t size)
{
	guard(mutex)(&smc->mutex);
	return smc->be->write(smc->be_cookie, key, buf, size);
}
EXPORT_SYMBOL(apple_smc_write);

int apple_smc_enter_atomic(struct apple_smc *smc)
{
	guard(mutex)(&smc->mutex);
	return smc->be->enter_atomic(smc->be_cookie);
}
EXPORT_SYMBOL(apple_smc_enter_atomic);

int apple_smc_write_atomic(struct apple_smc *smc, smc_key key, const void *buf, size_t size)
{
	guard(spinlock_irqsave)(&smc->lock);
	return smc->be->write_atomic(smc->be_cookie, key, buf, size);
}
EXPORT_SYMBOL(apple_smc_write_atomic);

int apple_smc_rw(struct apple_smc *smc, smc_key key, const void *wbuf, size_t wsize,
		void *rbuf, size_t rsize)
{
	guard(mutex)(&smc->mutex);
	return smc->be->rw(smc->be_cookie, key, wbuf, wsize, rbuf, rsize);
}
EXPORT_SYMBOL(apple_smc_rw);

int apple_smc_get_key_by_index(struct apple_smc *smc, int index, smc_key *key)
{
	guard(mutex)(&smc->mutex);
	int ret;

	ret = smc->be->get_key_by_index(smc->be_cookie, index, key);
	*key = swab32(*key);

	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_by_index);

int apple_smc_get_key_info(struct apple_smc *smc, smc_key key, struct apple_smc_key_info *info)
{
	guard(mutex)(&smc->mutex);
	return smc->be->get_key_info(smc->be_cookie, key, info);
}
EXPORT_SYMBOL(apple_smc_get_key_info);

static void apple_smc_disable_notifications(void *data)
{
	struct apple_smc *smc = data;

	apple_smc_write_flag(smc, SMC_KEY(NTAP), false);
}

int apple_smc_probe(struct device *dev, const struct apple_smc_backend_ops *ops, void *cookie)
{
	struct apple_smc *smc;
	u32 count;
	int ret;

	smc = devm_kzalloc(dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return -ENOMEM;

	smc->dev = dev;
	smc->be_cookie = cookie;
	smc->be = ops;
	mutex_init(&smc->mutex);
	spin_lock_init(&smc->lock);

	if (!strcmp(dev_driver_string(smc->dev), "macsmc-rtkit")) {
		apple_smc_write_flag(smc, SMC_KEY(NTAP), true);
		ret = devm_add_action_or_reset(dev, apple_smc_disable_notifications, smc);
		if (ret < 0)
			return ret;
	} else
		smc->is_acpi = true;

	ret = apple_smc_read_u32(smc, SMC_KEY(#KEY), &count);
	if (ret < 0)
		return ret;
	smc->key_count = be32_to_cpu(count);

	ret = apple_smc_get_key_by_index(smc, 0, &smc->first_key);
	if (ret < 0)
		return ret;

	ret = apple_smc_get_key_by_index(smc, smc->key_count - 1, &smc->last_key);
	if (ret < 0)
		return ret;

	dev_set_drvdata(dev, smc);

	if (smc->is_acpi)
		ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_devs,
			ARRAY_SIZE(apple_smc_acpi_devs), NULL, 0, NULL);
	else
		ret = devm_mfd_add_devices(dev, -1, apple_smc_devs,
			ARRAY_SIZE(apple_smc_devs), NULL, 0, NULL);
	if (ret < 0)
		return ret;

	dev_info(dev, "Successfully Initialized (%d keys First key: %p4ch Last key: %p4ch)\n",
			smc->key_count, &smc->first_key, &smc->last_key);
	return 0;
}
EXPORT_SYMBOL(apple_smc_probe);

MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC core");
