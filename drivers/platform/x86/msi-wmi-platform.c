// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for WMI platform features on MSI notebooks and handhelds.
 *
 * Copyright (C) 2024-2025 Armin Wolf <W_Armin@gmx.de>
 * Copyright (C) 2025 Antheas Kapenekakis <lkml@antheas.dev>
 */

#define pr_format(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/fixp-arith.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/wmi.h>
#include <acpi/battery.h>

#include <linux/unaligned.h>

#include "firmware_attributes_class.h"

#define DRIVER_NAME	"msi-wmi-platform"

#define MSI_PLATFORM_GUID	"ABBC0F6E-8EA1-11d1-00A0-C90629100000"

#define MSI_WMI_PLATFORM_INTERFACE_VERSION	2

/* Get_WMI() WMI method */
#define MSI_PLATFORM_WMI_MAJOR_OFFSET	1
#define MSI_PLATFORM_WMI_MINOR_OFFSET	2

/* Get_EC() and Set_EC() WMI methods */
#define MSI_PLATFORM_EC_FLAGS_OFFSET	1
#define MSI_PLATFORM_EC_MINOR_MASK	GENMASK(3, 0)
#define MSI_PLATFORM_EC_MAJOR_MASK	GENMASK(5, 4)
#define MSI_PLATFORM_EC_CHANGED_PAGE	BIT(6)
#define MSI_PLATFORM_EC_IS_TIGERLAKE	BIT(7)
#define MSI_PLATFORM_EC_VERSION_OFFSET	2

/* Get_Fan() and Set_Fan() WMI methods */
#define MSI_PLATFORM_FAN_SUBFEATURE_FAN_SPEED		0x0
#define MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE	0x1
#define MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE	0x2
#define MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE	0x1
#define MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE	0x2

/* Get_AP() and Set_AP() WMI methods */
#define MSI_PLATFORM_AP_SUBFEATURE_FAN_MODE	0x1
#define MSI_PLATFORM_AP_FAN_FLAGS_OFFSET	1
#define MSI_PLATFORM_AP_ENABLE_FAN_TABLES	BIT(7)

/* Get_Data() and Set_Data() Shift Mode Register */
#define MSI_PLATFORM_SHIFT_ADDR		0xd2
#define MSI_PLATFORM_SHIFT_DISABLE	BIT(7)
#define MSI_PLATFORM_SHIFT_ENABLE	(BIT(7) | BIT(6))
#define MSI_PLATFORM_SHIFT_SPORT	(MSI_PLATFORM_SHIFT_ENABLE + 4)
#define MSI_PLATFORM_SHIFT_COMFORT	(MSI_PLATFORM_SHIFT_ENABLE + 0)
#define MSI_PLATFORM_SHIFT_GREEN	(MSI_PLATFORM_SHIFT_ENABLE + 1)
#define MSI_PLATFORM_SHIFT_ECO		(MSI_PLATFORM_SHIFT_ENABLE + 2)
#define MSI_PLATFORM_SHIFT_USER		(MSI_PLATFORM_SHIFT_ENABLE + 3)

/* Get_Data() and Set_Data() Params */
#define MSI_PLATFORM_PL1_ADDR	0x50
#define MSI_PLATFORM_PL2_ADDR	0x51
#define MSI_PLATFORM_BAT_ADDR	0xd7

static bool force;
module_param_unsafe(force, bool, 0);
MODULE_PARM_DESC(force, "Force loading without checking for supported WMI interface versions");

enum msi_wmi_platform_method {
	MSI_PLATFORM_GET_PACKAGE	= 0x01,
	MSI_PLATFORM_SET_PACKAGE	= 0x02,
	MSI_PLATFORM_GET_EC		= 0x03,
	MSI_PLATFORM_SET_EC		= 0x04,
	MSI_PLATFORM_GET_BIOS		= 0x05,
	MSI_PLATFORM_SET_BIOS		= 0x06,
	MSI_PLATFORM_GET_SMBUS		= 0x07,
	MSI_PLATFORM_SET_SMBUS		= 0x08,
	MSI_PLATFORM_GET_MASTER_BATTERY = 0x09,
	MSI_PLATFORM_SET_MASTER_BATTERY = 0x0a,
	MSI_PLATFORM_GET_SLAVE_BATTERY	= 0x0b,
	MSI_PLATFORM_SET_SLAVE_BATTERY	= 0x0c,
	MSI_PLATFORM_GET_TEMPERATURE	= 0x0d,
	MSI_PLATFORM_SET_TEMPERATURE	= 0x0e,
	MSI_PLATFORM_GET_THERMAL	= 0x0f,
	MSI_PLATFORM_SET_THERMAL	= 0x10,
	MSI_PLATFORM_GET_FAN		= 0x11,
	MSI_PLATFORM_SET_FAN		= 0x12,
	MSI_PLATFORM_GET_DEVICE		= 0x13,
	MSI_PLATFORM_SET_DEVICE		= 0x14,
	MSI_PLATFORM_GET_POWER		= 0x15,
	MSI_PLATFORM_SET_POWER		= 0x16,
	MSI_PLATFORM_GET_DEBUG		= 0x17,
	MSI_PLATFORM_SET_DEBUG		= 0x18,
	MSI_PLATFORM_GET_AP		= 0x19,
	MSI_PLATFORM_SET_AP		= 0x1a,
	MSI_PLATFORM_GET_DATA		= 0x1b,
	MSI_PLATFORM_SET_DATA		= 0x1c,
	MSI_PLATFORM_GET_WMI		= 0x1d,
};

struct msi_wmi_platform_quirk {
	bool shift_mode;	/* Shift mode is supported */
	bool charge_threshold;	/* Charge threshold is supported */
	bool dual_fans;		/* For devices with two hwmon fans */
	bool restore_curves;	/* Restore factory curves on unload */
	int pl_min;		/* Minimum PLx value */
	int pl1_max;		/* Maximum PL1 value */
	int pl2_max;		/* Maximum PL2 value */
};

struct msi_wmi_platform_factory_curves {
	u8 cpu_fan_table[32];
	u8 gpu_fan_table[32];
	u8 cpu_temp_table[32];
	u8 gpu_temp_table[32];
};

struct msi_wmi_platform_data {
	struct wmi_device *wdev;
	struct msi_wmi_platform_quirk *quirks;
	struct mutex wmi_lock;	/* Necessary when calling WMI methods */
	struct device *ppdev;
	struct msi_wmi_platform_factory_curves factory_curves;
	struct acpi_battery_hook battery_hook;
	struct device *fw_attrs_dev;
	struct kset *fw_attrs_kset;
};

enum msi_fw_attr_id {
	MSI_ATTR_PPT_PL1_SPL,
	MSI_ATTR_PPT_PL2_SPPT,
};

static const char *const msi_fw_attr_name[] = {
	[MSI_ATTR_PPT_PL1_SPL] = "ppt_pl1_spl",
	[MSI_ATTR_PPT_PL2_SPPT] = "ppt_pl2_sppt",
};

static const char *const msi_fw_attr_desc[] = {
	[MSI_ATTR_PPT_PL1_SPL] = "CPU Steady package limit (PL1/SPL)",
	[MSI_ATTR_PPT_PL2_SPPT] = "CPU Boost slow package limit (PL2/SPPT)",
};

#define MSI_ATTR_LANGUAGE_CODE "en_US.UTF-8"

struct msi_fw_attr {
	struct msi_wmi_platform_data *data;
	enum msi_fw_attr_id fw_attr_id;
	struct attribute_group attr_group;
	struct kobj_attribute display_name;
	struct kobj_attribute current_value;
	struct kobj_attribute min_value;
	struct kobj_attribute max_value;

	u32 min;
	u32 max;

	int (*get_value)(struct msi_wmi_platform_data *data,
			 struct msi_fw_attr *fw_attr, char *buf);
	ssize_t (*set_value)(struct msi_wmi_platform_data *data,
			     struct msi_fw_attr *fw_attr, const char *buf,
			     size_t count);
};

struct msi_wmi_platform_debugfs_data {
	struct msi_wmi_platform_data *data;
	enum msi_wmi_platform_method method;
	struct rw_semaphore buffer_lock;	/* Protects debugfs buffer */
	size_t length;
	u8 buffer[32];
};

static const char * const msi_wmi_platform_debugfs_names[] = {
	"get_package",
	"set_package",
	"get_ec",
	"set_ec",
	"get_bios",
	"set_bios",
	"get_smbus",
	"set_smbus",
	"get_master_battery",
	"set_master_battery",
	"get_slave_battery",
	"set_slave_battery",
	"get_temperature",
	"set_temperature",
	"get_thermal",
	"set_thermal",
	"get_fan",
	"set_fan",
	"get_device",
	"set_device",
	"get_power",
	"set_power",
	"get_debug",
	"set_debug",
	"get_ap",
	"set_ap",
	"get_data",
	"set_data",
	"get_wmi"
};

static struct msi_wmi_platform_quirk quirk_default = {};
static struct msi_wmi_platform_quirk quirk_gen1 = {
	.shift_mode = true,
	.charge_threshold = true,
	.dual_fans = true,
	.restore_curves = true,
	.pl_min = 8,
	.pl1_max = 43,
	.pl2_max = 45
};
static struct msi_wmi_platform_quirk quirk_gen2 = {
	.shift_mode = true,
	.charge_threshold = true,
	.dual_fans = true,
	.restore_curves = true,
	.pl_min = 8,
	.pl1_max = 30,
	.pl2_max = 37
};

static const struct dmi_system_id msi_quirks[] = {
	{
		.ident = "MSI Claw (gen 1)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Micro-Star International Co., Ltd."),
			DMI_MATCH(DMI_BOARD_NAME, "MS-1T41"),
		},
		.driver_data = &quirk_gen1,
	},
	{
		.ident = "MSI Claw AI+ 7",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Micro-Star International Co., Ltd."),
			DMI_MATCH(DMI_BOARD_NAME, "MS-1T42"),
		},
		.driver_data = &quirk_gen2,
	},
	{
		.ident = "MSI Claw AI+ 8",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Micro-Star International Co., Ltd."),
			DMI_MATCH(DMI_BOARD_NAME, "MS-1T52"),
		},
		.driver_data = &quirk_gen2,
	},
};

static int msi_wmi_platform_parse_buffer(union acpi_object *obj, u8 *output, size_t length)
{
	if (obj->type != ACPI_TYPE_BUFFER)
		return -ENOMSG;

	if (obj->buffer.length != length)
		return -EPROTO;

	if (!obj->buffer.pointer[0])
		return -EIO;

	memcpy(output, obj->buffer.pointer, obj->buffer.length);

	return 0;
}

static int msi_wmi_platform_query_unlocked(struct msi_wmi_platform_data *data,
				  enum msi_wmi_platform_method method, u8 *buffer,
				  size_t length)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in = {
		.length = length,
		.pointer = buffer
	};
	union acpi_object *obj;
	acpi_status status;
	int ret;

	if (!length)
		return -EINVAL;

	status = wmidev_evaluate_method(data->wdev, 0x0, method, &in, &out);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = out.pointer;
	if (!obj)
		return -ENODATA;

	ret = msi_wmi_platform_parse_buffer(obj, buffer, length);
	kfree(obj);

	return ret;
}

static int msi_wmi_platform_query(struct msi_wmi_platform_data *data,
				  enum msi_wmi_platform_method method, u8 *buffer,
				  size_t length)
{
	/*
	 * The ACPI control method responsible for handling the WMI method calls
	 * is not thread-safe. Because of this we have to do the locking ourself.
	 */
	scoped_guard(mutex, &data->wmi_lock) {
		return msi_wmi_platform_query_unlocked(data, method, buffer, length);
	}
}

static ssize_t msi_wmi_platform_fan_table_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { sattr->nr };
	u8 fan_percent;
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_FAN, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	fan_percent = buffer[sattr->index + 1];
	if (fan_percent > 100)
		return -EIO;

	return sysfs_emit(buf, "%d\n", fixp_linear_interpolate(0, 0, 100, 255, fan_percent));
}

static ssize_t msi_wmi_platform_fan_table_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { sattr->nr };
	long speed;
	int ret;

	ret = kstrtol(buf, 10, &speed);
	if (ret < 0)
		return ret;

	speed = clamp_val(speed, 0, 255);

	guard(mutex)(&data->wmi_lock);

	ret = msi_wmi_platform_query_unlocked(data, MSI_PLATFORM_GET_FAN,
					      buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	buffer[0] = sattr->nr;
	buffer[sattr->index + 1] = fixp_linear_interpolate(0, 0, 255, 100, speed);

	ret = msi_wmi_platform_query_unlocked(data, MSI_PLATFORM_SET_FAN,
					      buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t msi_wmi_platform_temp_table_show(struct device *dev, struct device_attribute *attr,
						char *buf)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { sattr->nr };
	u8 temp_c;
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_TEMPERATURE,
				     buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	temp_c = buffer[sattr->index + 1];

	return sysfs_emit(buf, "%d\n", temp_c);
}

static ssize_t msi_wmi_platform_temp_table_store(struct device *dev, struct device_attribute *attr,
						 const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { sattr->nr };
	long temp_c;
	int ret;

	ret = kstrtol(buf, 10, &temp_c);
	if (ret < 0)
		return ret;

	temp_c = clamp_val(temp_c, 0, 255);

	guard(mutex)(&data->wmi_lock);

	ret = msi_wmi_platform_query_unlocked(data, MSI_PLATFORM_GET_TEMPERATURE,
					      buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	buffer[0] = sattr->nr;
	buffer[sattr->index + 1] = temp_c;

	ret = msi_wmi_platform_query_unlocked(data, MSI_PLATFORM_SET_TEMPERATURE,
					      buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	return count;
}

static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point1_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE, 0x0);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point2_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE, 0x3);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point3_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE, 0x4);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point4_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE, 0x5);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point5_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE, 0x6);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point6_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE, 0x7);

static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point1_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE, 0x1);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point2_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE, 0x2);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point3_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE, 0x3);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point4_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE, 0x4);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point5_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE, 0x5);
static SENSOR_DEVICE_ATTR_2_RW(pwm1_auto_point6_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE, 0x6);

static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point1_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE, 0x0);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point2_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE, 0x3);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point3_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE, 0x4);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point4_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE, 0x5);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point5_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE, 0x6);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point6_temp, msi_wmi_platform_temp_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE, 0x7);

static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point1_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE, 0x1);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point2_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE, 0x2);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point3_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE, 0x3);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point4_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE, 0x4);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point5_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE, 0x5);
static SENSOR_DEVICE_ATTR_2_RW(pwm2_auto_point6_pwm, msi_wmi_platform_fan_table,
			       MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE, 0x6);

static struct attribute *msi_wmi_platform_hwmon_attrs[] = {
	&sensor_dev_attr_pwm1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_temp.dev_attr.attr,

	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_pwm.dev_attr.attr,

	&sensor_dev_attr_pwm2_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point5_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point6_temp.dev_attr.attr,

	&sensor_dev_attr_pwm2_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point6_pwm.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(msi_wmi_platform_hwmon);

static int msi_wmi_platform_curves_save(struct msi_wmi_platform_data *data)
{
	int ret;

	data->factory_curves.cpu_fan_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE;
	ret = msi_wmi_platform_query_unlocked(
		data, MSI_PLATFORM_GET_FAN,
		data->factory_curves.cpu_fan_table,
		sizeof(data->factory_curves.cpu_fan_table));
	if (ret < 0)
		return ret;
	data->factory_curves.cpu_fan_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_CPU_FAN_TABLE;

	data->factory_curves.gpu_fan_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE;
	ret = msi_wmi_platform_query_unlocked(
		data, MSI_PLATFORM_GET_FAN,
		data->factory_curves.gpu_fan_table,
		sizeof(data->factory_curves.gpu_fan_table));
	if (ret < 0)
		return ret;
	data->factory_curves.gpu_fan_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_GPU_FAN_TABLE;

	data->factory_curves.cpu_temp_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE;
	ret = msi_wmi_platform_query_unlocked(
		data, MSI_PLATFORM_GET_TEMPERATURE,
		data->factory_curves.cpu_temp_table,
		sizeof(data->factory_curves.cpu_temp_table));
	if (ret < 0)
		return ret;
	data->factory_curves.cpu_temp_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_CPU_TEMP_TABLE;

	data->factory_curves.gpu_temp_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE;
	ret = msi_wmi_platform_query_unlocked(
		data, MSI_PLATFORM_GET_TEMPERATURE,
		data->factory_curves.gpu_temp_table,
		sizeof(data->factory_curves.gpu_temp_table));
	if (ret < 0)
		return ret;
	data->factory_curves.gpu_temp_table[0] =
		MSI_PLATFORM_FAN_SUBFEATURE_GPU_TEMP_TABLE;

	return 0;
}

static int msi_wmi_platform_curves_load(struct msi_wmi_platform_data *data)
{
	u8 buffer[32] = { };
	int ret;

	memcpy(buffer, data->factory_curves.cpu_fan_table,
	       sizeof(data->factory_curves.cpu_fan_table));
	ret = msi_wmi_platform_query_unlocked(data, MSI_PLATFORM_SET_FAN,
					      buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	memcpy(buffer, data->factory_curves.gpu_fan_table,
	       sizeof(data->factory_curves.gpu_fan_table));
	ret = msi_wmi_platform_query_unlocked(data, MSI_PLATFORM_SET_FAN,
					      buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	memcpy(buffer, data->factory_curves.cpu_temp_table,
	       sizeof(data->factory_curves.cpu_temp_table));
	ret = msi_wmi_platform_query_unlocked(
		data, MSI_PLATFORM_SET_TEMPERATURE, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	memcpy(buffer, data->factory_curves.gpu_temp_table,
	       sizeof(data->factory_curves.gpu_temp_table));
	ret = msi_wmi_platform_query_unlocked(
		data, MSI_PLATFORM_SET_TEMPERATURE, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	return 0;
}


static umode_t msi_wmi_platform_is_visible(const void *drvdata, enum hwmon_sensor_types type,
					   u32 attr, int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_enable)
		return 0644;

	return 0444;
}

static int msi_wmi_platform_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				 int channel, long *val)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { 0 };
	u16 value;
	u8 flags;
	int ret;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			buffer[0] = MSI_PLATFORM_FAN_SUBFEATURE_FAN_SPEED;
			ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_FAN, buffer,
						     sizeof(buffer));
			if (ret < 0)
				return ret;

			value = get_unaligned_be16(&buffer[channel * 2 + 1]);
			if (!value)
				*val = 0;
			else
				*val = 480000 / value;

			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			buffer[0] = MSI_PLATFORM_AP_SUBFEATURE_FAN_MODE;
			ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_AP, buffer,
						     sizeof(buffer));
			if (ret < 0)
				return ret;

			flags = buffer[MSI_PLATFORM_AP_FAN_FLAGS_OFFSET];
			if (flags & MSI_PLATFORM_AP_ENABLE_FAN_TABLES)
				*val = 1;
			else
				*val = 2;

			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int msi_wmi_platform_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				  int channel, long val)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { };
	int ret;

	guard(mutex)(&data->wmi_lock);

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			buffer[0] = MSI_PLATFORM_AP_SUBFEATURE_FAN_MODE;
			ret = msi_wmi_platform_query_unlocked(
				data, MSI_PLATFORM_GET_AP, buffer,
				sizeof(buffer));
			if (ret < 0)
				return ret;

			buffer[0] = MSI_PLATFORM_AP_SUBFEATURE_FAN_MODE;
			switch (val) {
			case 1:
				buffer[MSI_PLATFORM_AP_FAN_FLAGS_OFFSET] |=
					MSI_PLATFORM_AP_ENABLE_FAN_TABLES;
				break;
			case 2:
				buffer[MSI_PLATFORM_AP_FAN_FLAGS_OFFSET] &=
					~MSI_PLATFORM_AP_ENABLE_FAN_TABLES;
				break;
			default:
				return -EINVAL;
			}

			ret = msi_wmi_platform_query_unlocked(
				data, MSI_PLATFORM_SET_AP, buffer,
				sizeof(buffer));
			if (ret < 0)
				return ret;

			if (val == 2 && data->quirks->restore_curves) {
				ret = msi_wmi_platform_curves_load(data);
				if (ret < 0)
					return ret;
			}

			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops msi_wmi_platform_ops = {
	.is_visible = msi_wmi_platform_is_visible,
	.read = msi_wmi_platform_read,
	.write = msi_wmi_platform_write,
};

static const struct hwmon_channel_info * const msi_wmi_platform_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT
			   ),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_ENABLE,
			   HWMON_PWM_ENABLE
			   ),
	NULL
};

static const struct hwmon_chip_info msi_wmi_platform_chip_info = {
	.ops = &msi_wmi_platform_ops,
	.info = msi_wmi_platform_info,
};

static const struct hwmon_channel_info * const msi_wmi_platform_info_dual[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT
			   ),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_ENABLE,
			   HWMON_PWM_ENABLE
			   ),
	NULL
};

static const struct hwmon_chip_info msi_wmi_platform_chip_info_dual = {
	.ops = &msi_wmi_platform_ops,
	.info = msi_wmi_platform_info_dual,
};

static int msi_wmi_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int msi_wmi_platform_profile_get(struct device *dev,
					enum platform_profile_option *profile)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	int ret;

	u8 buffer[32] = { };

	buffer[0] = MSI_PLATFORM_SHIFT_ADDR;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_DATA, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	if (buffer[0] != 1)
		return -EINVAL;

	switch (buffer[1]) {
	case MSI_PLATFORM_SHIFT_SPORT:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		return 0;
	case MSI_PLATFORM_SHIFT_COMFORT:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		return 0;
	case MSI_PLATFORM_SHIFT_GREEN:
		*profile = PLATFORM_PROFILE_BALANCED;
		return 0;
	case MSI_PLATFORM_SHIFT_ECO:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		return 0;
	case MSI_PLATFORM_SHIFT_USER:
		*profile = PLATFORM_PROFILE_CUSTOM;
		return 0;
	default:
		return -EINVAL;
	}
}

static int msi_wmi_platform_profile_set(struct device *dev,
					enum platform_profile_option profile)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 buffer[32] = { };

	buffer[0] = MSI_PLATFORM_SHIFT_ADDR;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		buffer[1] = MSI_PLATFORM_SHIFT_SPORT;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		buffer[1] = MSI_PLATFORM_SHIFT_COMFORT;
		break;
	case PLATFORM_PROFILE_BALANCED:
		buffer[1] = MSI_PLATFORM_SHIFT_GREEN;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		buffer[1] = MSI_PLATFORM_SHIFT_ECO;
		break;
	case PLATFORM_PROFILE_CUSTOM:
		buffer[1] = MSI_PLATFORM_SHIFT_USER;
		break;
	default:
		return -EINVAL;
	}

	return msi_wmi_platform_query(data, MSI_PLATFORM_SET_DATA, buffer, sizeof(buffer));
}

static const struct platform_profile_ops msi_wmi_platform_profile_ops = {
	.probe = msi_wmi_platform_profile_probe,
	.profile_get = msi_wmi_platform_profile_get,
	.profile_set = msi_wmi_platform_profile_set,
};

/* Firmware Attributes setup */
static int data_get_addr(struct msi_wmi_platform_data *data,
			 const enum msi_fw_attr_id id)
{
	switch (id) {
	case MSI_ATTR_PPT_PL1_SPL:
		return MSI_PLATFORM_PL1_ADDR;
	case MSI_ATTR_PPT_PL2_SPPT:
		return MSI_PLATFORM_PL2_ADDR;
	default:
		pr_warn("Invalid attribute id %d\n", id);
		return -EINVAL;
	}
}

static ssize_t data_set_value(struct msi_wmi_platform_data *data,
			      struct msi_fw_attr *fw_attr, const char *buf,
			      size_t count)
{
	u8 buffer[32] = { 0 };
	int ret, fwid;
	u32 value;

	fwid = data_get_addr(data, fw_attr->fw_attr_id);
	if (fwid < 0)
		return fwid;

	ret = kstrtou32(buf, 10, &value);
	if (ret)
		return ret;

	if (fw_attr->min >= 0 && value < fw_attr->min)
		return -EINVAL;
	if (fw_attr->max >= 0 && value > fw_attr->max)
		return -EINVAL;

	buffer[0] = fwid;
	put_unaligned_le32(value, &buffer[1]);

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_SET_DATA, buffer, sizeof(buffer));
	if (ret) {
		pr_warn("Failed to set_data with id %d: %d\n",
			fw_attr->fw_attr_id, ret);
		return ret;
	}

	return count;
}

static int data_get_value(struct msi_wmi_platform_data *data,
			  struct msi_fw_attr *fw_attr, char *buf)
{
	u8 buffer[32] = { 0 };
	u32 value;
	int ret, addr;

	addr = data_get_addr(data, fw_attr->fw_attr_id);
	if (addr < 0)
		return addr;

	buffer[0] = addr;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_DATA, buffer, sizeof(buffer));
	if (ret) {
		pr_warn("Failed to show set_data for id %d: %d\n",
			fw_attr->fw_attr_id, ret);
		return ret;
	}

	value = get_unaligned_le32(&buffer[1]);

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t display_name_language_code_show(struct kobject *kobj, struct kobj_attribute *attr,
					       char *buf)
{
	return sysfs_emit(buf, "%s\n", MSI_ATTR_LANGUAGE_CODE);
}

static struct kobj_attribute fw_attr_display_name_language_code =
	__ATTR_RO(display_name_language_code);

static ssize_t scalar_increment_show(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static struct kobj_attribute fw_attr_scalar_increment =
	__ATTR_RO(scalar_increment);

static ssize_t pending_reboot_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static struct kobj_attribute fw_attr_pending_reboot = __ATTR_RO(pending_reboot);

static ssize_t display_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct msi_fw_attr *fw_attr =
		container_of(attr, struct msi_fw_attr, display_name);

	return sysfs_emit(buf, "%s\n", msi_fw_attr_desc[fw_attr->fw_attr_id]);
}

static ssize_t current_value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct msi_fw_attr *fw_attr =
		container_of(attr, struct msi_fw_attr, current_value);

	return fw_attr->get_value(fw_attr->data, fw_attr, buf);
}

static ssize_t current_value_store(struct kobject *kobj, struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	struct msi_fw_attr *fw_attr =
		container_of(attr, struct msi_fw_attr, current_value);

	return fw_attr->set_value(fw_attr->data, fw_attr, buf, count);
}

static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

static struct kobj_attribute fw_attr_type_int = {
	.attr = { .name = "type", .mode = 0444 },
	.show = type_show,
};

static ssize_t min_value_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct msi_fw_attr *fw_attr =
		container_of(attr, struct msi_fw_attr, min_value);

	return sysfs_emit(buf, "%d\n", fw_attr->min);
}

static ssize_t max_value_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct msi_fw_attr *fw_attr =
		container_of(attr, struct msi_fw_attr, max_value);

	return sysfs_emit(buf, "%d\n", fw_attr->max);
}

#define FW_ATTR_ENUM_MAX_ATTRS  7

static int
msi_fw_attr_init(struct msi_wmi_platform_data *data,
		 const enum msi_fw_attr_id fw_attr_id,
		 struct kobj_attribute *fw_attr_type, const s32 min,
		 const s32 max,
		 int (*get_value)(struct msi_wmi_platform_data *data,
				  struct msi_fw_attr *fw_attr, char *buf),
		 ssize_t (*set_value)(struct msi_wmi_platform_data *data,
				      struct msi_fw_attr *fw_attr,
				      const char *buf, size_t count))
{
	struct msi_fw_attr *fw_attr;
	struct attribute **attrs;
	int idx = 0;

	fw_attr = devm_kzalloc(&data->wdev->dev, sizeof(*fw_attr), GFP_KERNEL);
	if (!fw_attr)
		return -ENOMEM;

	attrs = devm_kcalloc(&data->wdev->dev, FW_ATTR_ENUM_MAX_ATTRS + 1,
			     sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return -ENOMEM;

	fw_attr->data = data;
	fw_attr->fw_attr_id = fw_attr_id;
	fw_attr->attr_group.name = msi_fw_attr_name[fw_attr_id];
	fw_attr->attr_group.attrs = attrs;
	fw_attr->get_value = get_value;
	fw_attr->set_value = set_value;

	attrs[idx++] = &fw_attr_type->attr;
	if (fw_attr_type == &fw_attr_type_int)
		attrs[idx++] = &fw_attr_scalar_increment.attr;
	attrs[idx++] = &fw_attr_display_name_language_code.attr;

	sysfs_attr_init(&fw_attr->display_name.attr);
	fw_attr->display_name.attr.name = "display_name";
	fw_attr->display_name.attr.mode = 0444;
	fw_attr->display_name.show = display_name_show;
	attrs[idx++] = &fw_attr->display_name.attr;

	sysfs_attr_init(&fw_attr->current_value.attr);
	fw_attr->current_value.attr.name = "current_value";
	fw_attr->current_value.attr.mode = 0644;
	fw_attr->current_value.show = current_value_show;
	fw_attr->current_value.store = current_value_store;
	attrs[idx++] = &fw_attr->current_value.attr;

	if (min >= 0) {
		fw_attr->min = min;
		sysfs_attr_init(&fw_attr->min_value.attr);
		fw_attr->min_value.attr.name = "min_value";
		fw_attr->min_value.attr.mode = 0444;
		fw_attr->min_value.show = min_value_show;
		attrs[idx++] = &fw_attr->min_value.attr;
	} else {
		fw_attr->min = -1;
	}

	if (max >= 0) {
		fw_attr->max = max;
		sysfs_attr_init(&fw_attr->max_value.attr);
		fw_attr->max_value.attr.name = "max_value";
		fw_attr->max_value.attr.mode = 0444;
		fw_attr->max_value.show = max_value_show;
		attrs[idx++] = &fw_attr->max_value.attr;
	} else {
		fw_attr->max = -1;
	}

	attrs[idx] = NULL;
	return sysfs_create_group(&data->fw_attrs_kset->kobj, &fw_attr->attr_group);
}

static void msi_kset_unregister(void *data)
{
	struct kset *kset = data;

	sysfs_remove_file(&kset->kobj, &fw_attr_pending_reboot.attr);
	kset_unregister(kset);
}

static void msi_fw_attrs_dev_unregister(void *data)
{
	struct device *fw_attrs_dev = data;

	device_unregister(fw_attrs_dev);
}

static int msi_wmi_fw_attrs_init(struct msi_wmi_platform_data *data)
{
	int err;

	data->fw_attrs_dev = device_create(&firmware_attributes_class, NULL, MKDEV(0, 0),
						 NULL, "%s", DRIVER_NAME);
	if (IS_ERR(data->fw_attrs_dev))
		return PTR_ERR(data->fw_attrs_dev);

	err = devm_add_action_or_reset(&data->wdev->dev,
				       msi_fw_attrs_dev_unregister,
				       data->fw_attrs_dev);
	if (err)
		return err;

	data->fw_attrs_kset = kset_create_and_add("attributes", NULL,
						  &data->fw_attrs_dev->kobj);
	if (!data->fw_attrs_kset)
		return -ENOMEM;

	err = sysfs_create_file(&data->fw_attrs_kset->kobj,
				&fw_attr_pending_reboot.attr);
	if (err) {
		kset_unregister(data->fw_attrs_kset);
		return err;
	}

	err = devm_add_action_or_reset(&data->wdev->dev, msi_kset_unregister,
				       data->fw_attrs_kset);
	if (err)
		return err;

	if (data->quirks->pl1_max) {
		err = msi_fw_attr_init(data, MSI_ATTR_PPT_PL1_SPL,
					&fw_attr_type_int, data->quirks->pl_min,
					data->quirks->pl1_max, &data_get_value,
					&data_set_value);
		if (err)
			return err;
	}

	if (data->quirks->pl2_max) {
		err = msi_fw_attr_init(data, MSI_ATTR_PPT_PL2_SPPT,
				       &fw_attr_type_int, data->quirks->pl_min,
				       data->quirks->pl2_max, &data_get_value,
				       &data_set_value);
		if (err)
			return err;
	}

	return 0;
}

static int msi_platform_psy_ext_get_prop(struct power_supply *psy,
					 const struct power_supply_ext *ext,
					 void *data,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct msi_wmi_platform_data *msi = data;
	u8 buffer[32] = { 0 };
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		buffer[0] = MSI_PLATFORM_BAT_ADDR;
		ret = msi_wmi_platform_query(msi, MSI_PLATFORM_GET_DATA,
					     buffer, sizeof(buffer));
		if (ret)
			return ret;

		val->intval = buffer[1] & ~BIT(7);
		if (val->intval > 100)
			return -EINVAL;

		return 0;
	default:
		return -EINVAL;
	}
}

static int msi_platform_psy_ext_set_prop(struct power_supply *psy,
					 const struct power_supply_ext *ext,
					 void *data,
					 enum power_supply_property psp,
					 const union power_supply_propval *val)
{
	struct msi_wmi_platform_data *msi = data;
	u8 buffer[32] = { 0 };

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (val->intval > 100)
			return -EINVAL;
		buffer[0] = MSI_PLATFORM_BAT_ADDR;
		buffer[1] = val->intval | BIT(7);
		return msi_wmi_platform_query(msi, MSI_PLATFORM_SET_DATA,
					      buffer, sizeof(buffer));
	default:
		return -EINVAL;
	}
}

static int
msi_platform_psy_prop_is_writeable(struct power_supply *psy,
				   const struct power_supply_ext *ext,
				   void *data, enum power_supply_property psp)
{
	return true;
}

static const enum power_supply_property oxp_psy_ext_props[] = {
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_ext msi_platform_psy_ext = {
	.name			= "msi-platform-charge-control",
	.properties		= oxp_psy_ext_props,
	.num_properties		= ARRAY_SIZE(oxp_psy_ext_props),
	.get_property		= msi_platform_psy_ext_get_prop,
	.set_property		= msi_platform_psy_ext_set_prop,
	.property_is_writeable	= msi_platform_psy_prop_is_writeable,
};

static int msi_wmi_platform_battery_add(struct power_supply *battery,
					struct acpi_battery_hook *hook)
{
	struct msi_wmi_platform_data *data =
		container_of(hook, struct msi_wmi_platform_data, battery_hook);

	return power_supply_register_extension(battery, &msi_platform_psy_ext,
					       &data->wdev->dev, data);
}

static int msi_wmi_platform_battery_remove(struct power_supply *battery,
					   struct acpi_battery_hook *hook)
{
	power_supply_unregister_extension(battery, &msi_platform_psy_ext);
	return 0;
}

static ssize_t msi_wmi_platform_debugfs_write(struct file *fp, const char __user *input,
					      size_t length, loff_t *offset)
{
	struct seq_file *seq = fp->private_data;
	struct msi_wmi_platform_debugfs_data *data = seq->private;
	u8 payload[32] = { };
	ssize_t ret;

	/* Do not allow partial writes */
	if (*offset != 0)
		return -EINVAL;

	/* Do not allow incomplete command buffers */
	if (length != data->length)
		return -EINVAL;

	ret = simple_write_to_buffer(payload, sizeof(payload), offset, input, length);
	if (ret < 0)
		return ret;

	down_write(&data->buffer_lock);
	ret = msi_wmi_platform_query(data->data, data->method, data->buffer,
				     data->length);
	up_write(&data->buffer_lock);

	if (ret < 0)
		return ret;

	down_write(&data->buffer_lock);
	memcpy(data->buffer, payload, data->length);
	up_write(&data->buffer_lock);

	return length;
}

static int msi_wmi_platform_debugfs_show(struct seq_file *seq, void *p)
{
	struct msi_wmi_platform_debugfs_data *data = seq->private;
	int ret;

	down_read(&data->buffer_lock);
	ret = seq_write(seq, data->buffer, data->length);
	up_read(&data->buffer_lock);

	return ret;
}

static int msi_wmi_platform_debugfs_open(struct inode *inode, struct file *fp)
{
	struct msi_wmi_platform_debugfs_data *data = inode->i_private;

	/* The seq_file uses the last byte of the buffer for detecting buffer overflows */
	return single_open_size(fp, msi_wmi_platform_debugfs_show, data, data->length + 1);
}

static const struct file_operations msi_wmi_platform_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = msi_wmi_platform_debugfs_open,
	.read = seq_read,
	.write = msi_wmi_platform_debugfs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void msi_wmi_platform_debugfs_remove(void *data)
{
	struct dentry *dir = data;

	debugfs_remove_recursive(dir);
}

static void msi_wmi_platform_debugfs_add(struct msi_wmi_platform_data *drvdata, struct dentry *dir,
					 const char *name, enum msi_wmi_platform_method method)
{
	struct msi_wmi_platform_debugfs_data *data;
	struct dentry *entry;

	data = devm_kzalloc(&drvdata->wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return;

	data->data = drvdata;
	data->method = method;
	init_rwsem(&data->buffer_lock);

	/* The ACPI firmware for now always requires a 32 byte input buffer due to
	 * a peculiarity in how Windows handles the CreateByteField() ACPI operator.
	 */
	data->length = 32;

	entry = debugfs_create_file(name, 0600, dir, data, &msi_wmi_platform_debugfs_fops);
	if (IS_ERR(entry))
		devm_kfree(&drvdata->wdev->dev, data);
}

static void msi_wmi_platform_debugfs_init(struct msi_wmi_platform_data *data)
{
	struct dentry *dir;
	char dir_name[64];
	int ret, method;

	scnprintf(dir_name, ARRAY_SIZE(dir_name), "%s-%s", DRIVER_NAME, dev_name(&data->wdev->dev));

	dir = debugfs_create_dir(dir_name, NULL);
	if (IS_ERR(dir))
		return;

	ret = devm_add_action_or_reset(&data->wdev->dev, msi_wmi_platform_debugfs_remove, dir);
	if (ret < 0)
		return;

	for (method = MSI_PLATFORM_GET_PACKAGE; method <= MSI_PLATFORM_GET_WMI; method++)
		msi_wmi_platform_debugfs_add(data, dir, msi_wmi_platform_debugfs_names[method - 1],
					     method);
}

static int msi_wmi_platform_hwmon_init(struct msi_wmi_platform_data *data)
{
	struct device *hdev;

	hdev = devm_hwmon_device_register_with_info(
		&data->wdev->dev, "msi_wmi_platform", data,
		data->quirks->dual_fans ? &msi_wmi_platform_chip_info_dual :
					&msi_wmi_platform_chip_info,
		msi_wmi_platform_hwmon_groups);

	return PTR_ERR_OR_ZERO(hdev);
}

static int msi_wmi_platform_ec_init(struct msi_wmi_platform_data *data)
{
	u8 buffer[32] = { 0 };
	u8 flags;
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_EC, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	flags = buffer[MSI_PLATFORM_EC_FLAGS_OFFSET];

	dev_dbg(&data->wdev->dev, "EC RAM version %lu.%lu\n",
		FIELD_GET(MSI_PLATFORM_EC_MAJOR_MASK, flags),
		FIELD_GET(MSI_PLATFORM_EC_MINOR_MASK, flags));
	dev_dbg(&data->wdev->dev, "EC firmware version %.28s\n",
		&buffer[MSI_PLATFORM_EC_VERSION_OFFSET]);

	if (!(flags & MSI_PLATFORM_EC_IS_TIGERLAKE)) {
		if (!force)
			return -ENODEV;

		dev_warn(&data->wdev->dev, "Loading on a non-Tigerlake platform\n");
	}

	return 0;
}

static int msi_wmi_platform_init(struct msi_wmi_platform_data *data)
{
	u8 buffer[32] = { 0 };
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_WMI, buffer, sizeof(buffer));
	if (ret < 0)
		return ret;

	dev_dbg(&data->wdev->dev, "WMI interface version %u.%u\n",
		buffer[MSI_PLATFORM_WMI_MAJOR_OFFSET],
		buffer[MSI_PLATFORM_WMI_MINOR_OFFSET]);

	if (buffer[MSI_PLATFORM_WMI_MAJOR_OFFSET] != MSI_WMI_PLATFORM_INTERFACE_VERSION) {
		if (!force)
			return -ENODEV;

		dev_warn(&data->wdev->dev,
			 "Loading despite unsupported WMI interface version (%u.%u)\n",
			 buffer[MSI_PLATFORM_WMI_MAJOR_OFFSET],
			 buffer[MSI_PLATFORM_WMI_MINOR_OFFSET]);
	}

	return 0;
}

static int msi_wmi_platform_profile_setup(struct msi_wmi_platform_data *data)
{
	if (!data->quirks->shift_mode)
		return 0;

	data->ppdev = devm_platform_profile_register(
		&data->wdev->dev, "msi-wmi-platform", data,
		&msi_wmi_platform_profile_ops);

	return PTR_ERR_OR_ZERO(data->ppdev);
}

static int msi_wmi_platform_probe(struct wmi_device *wdev, const void *context)
{
	struct msi_wmi_platform_data *data;
	const struct dmi_system_id *dmi_id;
	int ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->wdev = wdev;
	dev_set_drvdata(&wdev->dev, data);

	dmi_id = dmi_first_match(msi_quirks);
	if (dmi_id)
		data->quirks = dmi_id->driver_data;
	else
		data->quirks = &quirk_default;

	ret = devm_mutex_init(&wdev->dev, &data->wmi_lock);
	if (ret < 0)
		return ret;

	ret = msi_wmi_platform_init(data);
	if (ret < 0)
		return ret;

	ret = msi_wmi_platform_ec_init(data);
	if (ret < 0)
		return ret;

	ret = msi_wmi_fw_attrs_init(data);
	if (ret < 0)
		return ret;

	if (data->quirks->charge_threshold) {
		data->battery_hook.name = "MSI Battery";
		data->battery_hook.add_battery = msi_wmi_platform_battery_add;
		data->battery_hook.remove_battery = msi_wmi_platform_battery_remove;
		battery_hook_register(&data->battery_hook);
	}

	msi_wmi_platform_debugfs_init(data);

	msi_wmi_platform_profile_setup(data);

	if (data->quirks->restore_curves) {
		guard(mutex)(&data->wmi_lock);
		ret = msi_wmi_platform_curves_save(data);
		if (ret < 0)
			return ret;
	}

	return msi_wmi_platform_hwmon_init(data);
}

static void msi_wmi_platform_remove(struct wmi_device *wdev)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(&wdev->dev);

	if (data->quirks->charge_threshold)
		battery_hook_unregister(&data->battery_hook);

	if (data->quirks->restore_curves) {
		guard(mutex)(&data->wmi_lock);
		msi_wmi_platform_curves_load(data);
	}
}

static const struct wmi_device_id msi_wmi_platform_id_table[] = {
	{ MSI_PLATFORM_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, msi_wmi_platform_id_table);

static struct wmi_driver msi_wmi_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = msi_wmi_platform_id_table,
	.probe = msi_wmi_platform_probe,
	.remove = msi_wmi_platform_remove,
	.no_singleton = true,
};
module_wmi_driver(msi_wmi_platform_driver);

MODULE_AUTHOR("Armin Wolf <W_Armin@gmx.de>");
MODULE_DESCRIPTION("MSI WMI platform features");
MODULE_LICENSE("GPL");
