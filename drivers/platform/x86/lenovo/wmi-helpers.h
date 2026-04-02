/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Copyright (C) 2025 Derek J. Clark <derekjohn.clark@gmail.com> */

#ifndef _LENOVO_WMI_HELPERS_H_
#define _LENOVO_WMI_HELPERS_H_

#include <linux/types.h>

struct device;
struct notifier_block;
struct wmi_device;

struct wmi_method_args_32 {
	u32 arg0;
	u32 arg1;
};

int lwmi_dev_evaluate_int(struct wmi_device *wdev, u8 instance, u32 method_id,
			  unsigned char *buf, size_t size, u32 *retval);

int lwmi_tm_register_notifier(struct notifier_block *nb);
int lwmi_tm_unregister_notifier(struct notifier_block *nb);
int devm_lwmi_tm_register_notifier(struct device *dev,
				   struct notifier_block *nb);
int lwmi_tm_notifier_call(enum thermal_mode *mode);

#endif /* !_LENOVO_WMI_HELPERS_H_ */
