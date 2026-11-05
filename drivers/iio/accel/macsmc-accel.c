// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC sudden motion sensor (SMS) driver
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 *
 * Based on drivers/hwmon/applesmc.c:
 * Copyright (C) 2007 Nicolas Boichat <nicolas@boichat.ch>
 * Copyright (C) 2010 Henrik Rydberg <rydberg@euromail.se>
 */

#include <linux/delay.h>
#include <linux/byteorder/generic.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

struct macsmc_accel {
	struct apple_smc *smc;
	struct iio_dev *indio_dev;
	struct notifier_block nb;
};

enum {
	SMS_DROP = 0,
	SMS_ORIENTATION,
	SMS_SHOCK,
};

static int macsmc_accel_read(struct macsmc_accel *drv, smc_key key, s16 *p)
{
	int ret;

	ret = apple_smc_read_s16(drv->smc, key, p);
	if (ret < 0)
		return ret;

	*p = (s16)be16_to_cpu(*p);
	return 0;
}

static int macsmc_accel_dev_init(struct macsmc_accel *drv)
{
	int ret, total;
	u16 buf;

	for (total = 0; total < 100; total++) {
		ret = apple_smc_read_u16(drv->smc, SMC_KEY(MOCN), &buf);
		if (!ret && buf)
			return 0;

		buf = 0xE0;
		apple_smc_write_u16(drv->smc, SMC_KEY(MOCN), buf);
		msleep(50);
	}

	return -ENODEV;
}

static int macsmc_accel_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		int *val, int *val2, long mask)
{
	struct macsmc_accel *drv = iio_priv(indio_dev);
	int ret;
	s16 buf;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		ret = macsmc_accel_read(drv, SMC_KEY(MO_X), &buf);
		if (ret < 0)
			return ret;

		*val = buf;
		return IIO_VAL_INT;
	case IIO_MOD_Y:
		ret = macsmc_accel_read(drv, SMC_KEY(MO_Y), &buf);
		if (ret < 0)
			return ret;

		*val = buf;
		return IIO_VAL_INT;
	case IIO_MOD_Z:
		ret = macsmc_accel_read(drv, SMC_KEY(MO_Z), &buf);
		if (ret < 0)
			return ret;

		*val = buf;
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static int macsmc_accel_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct macsmc_accel *drv = container_of(nb, struct macsmc_accel, nb);
	u64 code;

	/* check if event is for us */
	if (event != 0x60 && event != 0x69 && event != 0x6F)
		return NOTIFY_DONE;

	switch (event) {
	case 0x60:
		code = IIO_UNMOD_EVENT_CODE(IIO_ACCEL, SMS_DROP,
					    IIO_EV_TYPE_CHANGE,
					    IIO_EV_DIR_NONE);
		break;
	case 0x69:
		code = IIO_UNMOD_EVENT_CODE(IIO_ACCEL, SMS_ORIENTATION,
					    IIO_EV_TYPE_CHANGE,
					    IIO_EV_DIR_NONE);
		break;
	case 0x6F:
		code = IIO_UNMOD_EVENT_CODE(IIO_ACCEL, SMS_SHOCK,
					    IIO_EV_TYPE_CHANGE,
					    IIO_EV_DIR_NONE);
		break;
	}

	iio_push_event(drv->indio_dev, code, iio_get_time_ns(drv->indio_dev));
	return NOTIFY_OK;
}

static void macsmc_accel_unregister_notifier(void *data)
{
	struct macsmc_accel *drv = data;

	blocking_notifier_chain_unregister(drv->smc->event_handlers,
					&drv->nb);
}

static const struct iio_event_spec macsmc_accel_event_spec = {
	.type = IIO_EV_TYPE_CHANGE,
	.dir = IIO_EV_DIR_NONE,
	.mask_separate = BIT(IIO_EV_INFO_ENABLE),
};

static const struct iio_chan_spec macsmc_accel_channels[] = {
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_ACCEL,
		.channel = SMS_DROP,
		.event_spec = &macsmc_accel_event_spec,
		.num_event_specs = 1,
	},
	{
		.type = IIO_ACCEL,
		.channel = SMS_ORIENTATION,
		.event_spec = &macsmc_accel_event_spec,
		.num_event_specs = 1,
	},
	{
		.type = IIO_ACCEL,
		.channel = SMS_SHOCK,
		.event_spec = &macsmc_accel_event_spec,
		.num_event_specs = 1,
	},
};

static const struct iio_info macsmc_accel_info = {
	.read_raw = macsmc_accel_read_raw
};

static int macsmc_accel_restore(struct device *dev)
{
	struct macsmc_accel *drv = dev_get_drvdata(dev);

	return macsmc_accel_dev_init(drv);
}

static const struct dev_pm_ops macsmc_accel_pm_ops = {
	.restore = macsmc_accel_restore,
};

static int macsmc_accel_probe(struct platform_device *pdev)
{
	struct macsmc_accel *drv;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*drv));
	if (!indio_dev)
		return -ENOMEM;

	drv = iio_priv(indio_dev);
	drv->smc = dev_get_drvdata(pdev->dev.parent);

	ret = macsmc_accel_dev_init(drv);
	if (ret)
		return ret;

	drv->indio_dev = indio_dev;
	drv->nb.notifier_call = macsmc_accel_event;
	ret = blocking_notifier_chain_register(drv->smc->event_handlers, &drv->nb);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&pdev->dev, macsmc_accel_unregister_notifier, drv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, drv);

	indio_dev->name = "macsmc-accel";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &macsmc_accel_info;
	indio_dev->channels = macsmc_accel_channels;
	indio_dev->num_channels = ARRAY_SIZE(macsmc_accel_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static struct platform_driver macsmc_accel_driver = {
	.probe = macsmc_accel_probe,
	.driver = {
		.name = "macsmc-accel",
		.pm = &macsmc_accel_pm_ops,
	},
};
module_platform_driver(macsmc_accel_driver);

MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_DESCRIPTION("Apple SMC Motion Sensor Driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_ALIAS("platform:macsmc-accel");
