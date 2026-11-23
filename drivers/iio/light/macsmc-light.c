// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Apple SMC Light Sensor Driver
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 *
 * Based on drivers/hwmom/applesmc.c:
 * Copyright (C) 2007 Nicolas Boichat <nicolas@boichat.ch>
 * Copyright (C) 2010 Henrik Rydberg <rydberg@euromail.se>
 */

#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/byteorder/generic.h>
#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>

enum {
	LEFT_LIGHT = 0,
	RIGHT_LIGHT
};

struct macsmc_light {
	struct apple_smc *smc;
	struct notifier_block nb;
	struct iio_dev *indio_dev;
	u8 size;
};

static int macsmc_light_read_raw(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan,
		int *val, int *val2, long mask)

{
	struct macsmc_light *drv = iio_priv(indio_dev);
	u8 buffer[10];
	int ret;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	switch (chan->channel) {
	case LEFT_LIGHT:
		ret = apple_smc_read(drv->smc, SMC_KEY(ALV0), buffer, drv->size);
		if (ret < 0)
			return ret;

		if (drv->size == 10)
			*val = be16_to_cpu(get_unaligned_be16(buffer + 6)) >> 10;
		else
			*val = buffer[2];

		break;
	case RIGHT_LIGHT:
		if (drv->size == 10) {
			*val = 0;
			break;
		}
		ret = apple_smc_read(drv->smc, SMC_KEY(ALV1), buffer, drv->size);
		if (ret < 0)
			return ret;

		*val = buffer[2];
		break;
	default:
		return -EINVAL;
	}

	return IIO_VAL_INT;
}

static int macsmc_light_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct macsmc_light *drv = container_of(nb, struct macsmc_light, nb);
	s64 timestamp = iio_get_time_ns(drv->indio_dev);

	/* check if event is for us */
	if (event != 0x2A)
		return NOTIFY_DONE;

	iio_push_event(drv->indio_dev,
		IIO_UNMOD_EVENT_CODE(IIO_LIGHT,
				LEFT_LIGHT,
				IIO_EV_TYPE_CHANGE,
				IIO_EV_DIR_EITHER),
		timestamp);

	if (drv->size != 10) {
		iio_push_event(drv->indio_dev,
			IIO_UNMOD_EVENT_CODE(IIO_LIGHT,
					RIGHT_LIGHT,
					IIO_EV_TYPE_CHANGE,
					IIO_EV_DIR_EITHER),
				timestamp);
	}

	return NOTIFY_OK;
}

static void macsmc_light_unregister_notifier(void *data)
{
	struct macsmc_light *drv = data;

	blocking_notifier_chain_unregister(drv->smc->event_handlers,
					   &drv->nb);
}

static const struct iio_event_spec macsmc_light_events[] = {
	{
		.type = IIO_EV_TYPE_CHANGE,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec macsmc_light_channels[] = {
	{
		.type = IIO_LIGHT,
		.channel = LEFT_LIGHT,
		.extend_name = "left",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.event_spec = macsmc_light_events,
		.num_event_specs = ARRAY_SIZE(macsmc_light_events),
	},
	{
		.type = IIO_LIGHT,
		.channel = RIGHT_LIGHT,
		.extend_name = "right",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.event_spec = macsmc_light_events,
		.num_event_specs = ARRAY_SIZE(macsmc_light_events),
	},
};

static const struct iio_info macsmc_light_info = {
	.read_raw = macsmc_light_read_raw,
};

static int macsmc_light_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct macsmc_light *drv;
	struct apple_smc_key_info info;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*drv));
	if (!indio_dev)
		return -ENOMEM;

	drv = iio_priv(indio_dev);
	drv->smc = dev_get_drvdata(pdev->dev.parent);

	ret = apple_smc_get_key_info(drv->smc, SMC_KEY(ALV0), &info);
	if (ret < 0)
		return -ENODEV;

	drv->size = info.size;
	drv->indio_dev = indio_dev;
	drv->nb.notifier_call = macsmc_light_event;
	ret = blocking_notifier_chain_register(drv->smc->event_handlers, &drv->nb);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&pdev->dev, macsmc_light_unregister_notifier, drv);
	if (ret)
		return ret;

	indio_dev->name = "macsmc-light";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &macsmc_light_info;
	indio_dev->channels = macsmc_light_channels;
	indio_dev->num_channels = ARRAY_SIZE(macsmc_light_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static struct platform_driver macsmc_light_driver = {
	.probe = macsmc_light_probe,
	.driver = {
		.name = "macsmc-light",
	},
};
module_platform_driver(macsmc_light_driver);

MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC Light Sensor Driver");
MODULE_ALIAS("platform:macsmc-light");
