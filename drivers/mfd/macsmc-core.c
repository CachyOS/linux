// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC core framework
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

static const struct mfd_cell apple_smc_rtkit_devs[] = {
	MFD_CELL_NAME("macsmc-input"),
	MFD_CELL_NAME("macsmc-power"),
	MFD_CELL_OF("macsmc-gpio", NULL, NULL, 0, 0, "apple,smc-gpio"),
	MFD_CELL_OF("macsmc-hwmon", NULL, NULL, 0, 0, "apple,smc-hwmon"),
	MFD_CELL_OF("macsmc-reboot", NULL, NULL, 0, 0, "apple,smc-reboot"),
	MFD_CELL_OF("macsmc-rtc", NULL, NULL, 0, 0, "apple,smc-rtc"),
};

/*
 * There isn't device tree on acpi backend, so we need to use
 * apple_smc_key_exists to register mfd devs
 */
static const struct mfd_cell apple_smc_acpi_accel[] = {
	MFD_CELL_NAME("macsmc-accel"),
};

static const struct mfd_cell apple_smc_acpi_common[] = {
	MFD_CELL_NAME("macsmc-hwmon"),
	MFD_CELL_NAME("macsmc-power"),
};

static const struct mfd_cell apple_smc_acpi_led[] = {
	MFD_CELL_NAME("macsmc-led"),
};

static const struct mfd_cell apple_smc_acpi_light[] = {
	MFD_CELL_NAME("macsmc-light"),
};

static const struct mfd_cell apple_smc_acpi_rtc[] = {
	MFD_CELL_NAME("macsmc-rtc"),
};

static const struct mfd_cell apple_smc_acpi_wdt[] = {
	MFD_CELL_NAME("macsmc-wdt"),
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

	guard(mutex)(&smc->mutex);
	smc->be->disable_notifications(smc->be_cookie);
}

static long apple_smc_fops_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct apple_smc *smc = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret;

	switch (cmd) {
	case IOCTL_SMC_READ: {
		struct ioctl_smc_rw_payload payload;
		struct apple_smc_key_info info;

		if (copy_from_user(&payload, argp, sizeof(payload)))
			return -EFAULT;

		ret = apple_smc_get_key_info(smc, payload.key, &info);
		if (ret < 0)
			return ret;

		if (payload.size != info.size)
			return -EINVAL;

		if (payload.size > 32)
			return -EINVAL;

		ret = apple_smc_read(smc, payload.key, payload.buf, payload.size);
		if (ret < 0)
			return ret;

		if (copy_to_user(argp, &payload, sizeof(payload)))
			return -EFAULT;
		return 0;
	}
	case IOCTL_SMC_WRITE: {
		struct ioctl_smc_rw_payload payload;
		struct apple_smc_key_info info;

		if (copy_from_user(&payload, argp, sizeof(payload)))
			return -EFAULT;

		ret = apple_smc_get_key_info(smc, payload.key, &info);
		if (ret < 0)
			return ret;

		if (payload.size != info.size)
			return -EINVAL;

		if (payload.size > 32)
			return -EINVAL;

		switch (payload.key) {
		case __SMC_KEY('C', 'H', '0', 'I'):
		case __SMC_KEY('C', 'H', '0', 'C'):
		case __SMC_KEY('C', 'H', '0', 'K'):
		case __SMC_KEY('C', 'H', '0', 'B'):
		case __SMC_KEY('C', 'H', 'T', 'E'):
		case __SMC_KEY('C', 'H', 'L', 'S'):
		case __SMC_KEY('B', 'C', 'L', 'M'):
		case __SMC_KEY('M', 'B', 'S', 'E'):
		case __SMC_KEY('C', 'L', 'K', 'L'):
		case __SMC_KEY('C', 'L', 'K', 'O'):
		case __SMC_KEY('O', 'S', 'W', 'D'):
		case __SMC_KEY('N', 'A', 'T', 'J'):
		case __SMC_KEY('N', 'A', 'T', 'i'):
		case __SMC_KEY('N', 'E', 'S', 'N'):
		case __SMC_KEY('M', 'O', 'C', 'N'):
		case __SMC_KEY('L', 'K', 'S', 'B'):
		case __SMC_KEY('N', 'T', 'O', 'K'):
		case __SMC_KEY('N', 'T', 'A', 'P'):
		case __SMC_KEY('F', '0', 'T', 'g'):
		case __SMC_KEY('F', '1', 'T', 'g'):
		case __SMC_KEY('F', '0', 'M', 'd'):
		case __SMC_KEY('F', '1', 'M', 'd'):
		case __SMC_KEY('F', 'S', '!', ' '):
			return -EPERM;
		}

		ret = apple_smc_write(smc, payload.key, payload.buf, payload.size);
		if (ret < 0)
			return ret;

		return 0;
	}
	case IOCTL_SMC_GET_KEY_INDEX: {
		struct ioctl_smc_index_payload payload;

		if (copy_from_user(&payload, argp, sizeof(payload)))
			return -EFAULT;

		ret = apple_smc_get_key_by_index(smc, payload.index, &payload.key);
		if (ret < 0)
			return ret;

		if (copy_to_user(argp, &payload, sizeof(payload)))
			return -EFAULT;

		return 0;
	}
	case IOCTL_SMC_GET_KEY_INFO: {
		struct ioctl_smc_info_payload payload;

		if (copy_from_user(&payload, argp, sizeof(payload)))
			return -EFAULT;

		ret = apple_smc_get_key_info(smc, payload.key, &payload.info);
		if (ret < 0)
			return ret;

		if (copy_to_user(argp, &payload, sizeof(payload)))
			return -EFAULT;

		return 0;
	}
	default:
		return -EINVAL;
	}
}

static int apple_smc_fops_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;

	file->private_data = container_of(misc, struct apple_smc, miscdev);
	return 0;
}

static const struct file_operations apple_smc_fops = {
	.owner		= THIS_MODULE,
	.open		= apple_smc_fops_open,
	.unlocked_ioctl	= apple_smc_fops_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static void apple_smc_misc_unregister(void *data)
{
	misc_deregister(data);
}

int apple_smc_probe(struct device *dev, const struct apple_smc_backend_ops *ops,
		void *cookie, struct blocking_notifier_head *event_handlers, bool is_acpi)
{
	struct apple_smc *smc;
	__be32 count;
	int ret;

	smc = devm_kzalloc(dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return -ENOMEM;

	smc->dev = dev;
	smc->be_cookie = cookie;
	smc->be = ops;
	smc->is_acpi = is_acpi;
	smc->event_handlers = event_handlers;
	mutex_init(&smc->mutex);
	spin_lock_init(&smc->lock);

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

	ret = devm_add_action_or_reset(dev, apple_smc_disable_notifications, smc);
	if (ret)
		return ret;

	smc->miscdev.minor = MISC_DYNAMIC_MINOR;
	smc->miscdev.name = "smc";
	smc->miscdev.fops = &apple_smc_fops;
	smc->miscdev.parent = dev;
	smc->miscdev.mode = 0600;

	ret = misc_register(&smc->miscdev);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, apple_smc_misc_unregister, &smc->miscdev);
	if (ret)
		return ret;

	if (smc->is_acpi) {
		ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_common,
			ARRAY_SIZE(apple_smc_acpi_common), NULL, 0, NULL);
		if (ret < 0)
			return ret;

		if (apple_smc_key_exists(smc, SMC_KEY(CLKO))) {
			ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_rtc,
				ARRAY_SIZE(apple_smc_acpi_rtc), NULL, 0, NULL);
			if (ret < 0)
				return ret;
		}

		if (apple_smc_key_exists(smc, SMC_KEY(NATJ))) {
			ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_wdt,
				ARRAY_SIZE(apple_smc_acpi_wdt), NULL, 0, NULL);
			if (ret < 0)
				return ret;
		}

		if (apple_smc_key_exists(smc, SMC_KEY(ALV0))) {
			ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_light,
				ARRAY_SIZE(apple_smc_acpi_light), NULL, 0, NULL);
			if (ret < 0)
				return ret;
		}

		if (apple_smc_key_exists(smc, SMC_KEY(MOCN))) {
			ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_accel,
				ARRAY_SIZE(apple_smc_acpi_accel), NULL, 0, NULL);
			if (ret < 0)
				return ret;
		}

		if (apple_smc_key_exists(smc, SMC_KEY(LKSB))) {
			ret = devm_mfd_add_devices(dev, -1, apple_smc_acpi_led,
				ARRAY_SIZE(apple_smc_acpi_led), NULL, 0, NULL);
			if (ret < 0)
				return ret;
		}
	} else
		ret = devm_mfd_add_devices(dev, -1, apple_smc_rtkit_devs,
			ARRAY_SIZE(apple_smc_rtkit_devs), NULL, 0, NULL);
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
