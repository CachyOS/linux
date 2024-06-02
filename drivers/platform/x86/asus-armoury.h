/* SPDX-License-Identifier: GPL-2.0
 *
 * Definitions for kernel modules using asus-armoury driver
 *
 *  Copyright (c) 2024 Luke Jones <luke@ljones.dev>
 */

#ifndef _ASUS_ARMOURY_H_
#define _ASUS_ARMOURY_H_

#include <linux/types.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "asus-armoury"

static ssize_t attr_uint_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count, u32 min, u32 max,
			      u32 *store_value, u32 wmi_dev);

static ssize_t int_type_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

static ssize_t enum_type_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "enumeration\n");
}

#define __ASUS_ATTR_RO(_func, _name)                                  \
	{                                                             \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _func##_##_name##_show,                       \
	}

#define __ASUS_ATTR_RO_AS(_name, _show)                               \
	{                                                             \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _show,                                        \
	}

#define __ASUS_ATTR_RW(_func, _name) \
	__ATTR(_name, 0644, _func##_##_name##_show, _func##_##_name##_store)

#define __WMI_STORE_INT(_attr, _min, _max, _wmi)                          \
	static ssize_t _attr##_store(struct kobject *kobj,                \
				     struct kobj_attribute *attr,         \
				     const char *buf, size_t count)       \
	{                                                                 \
		return attr_uint_store(kobj, attr, buf, count, _min, _max, \
				      NULL, _wmi);                        \
	}

#define WMI_SHOW_INT(_attr, _fmt, _wmi)                                     \
	static ssize_t _attr##_show(struct kobject *kobj,                   \
				    struct kobj_attribute *attr, char *buf) \
	{                                                                   \
		u32 result;                                                 \
		int err;                                                    \
		                                                            \
		err = asus_wmi_get_devstate_dsts(_wmi, &result);            \
		if (err)                                                    \
			return err;                                         \
		return sysfs_emit(buf, _fmt,                                \
				  result & ~ASUS_WMI_DSTS_PRESENCE_BIT);    \
	}

/* Create functions and attributes for use in other macros or on their own */

#define __ATTR_CURRENT_INT_RO(_attr, _wmi)                          \
	WMI_SHOW_INT(_attr##_current_value, "%d\n", _wmi);          \
	static struct kobj_attribute attr_##_attr##_current_value = \
		__ASUS_ATTR_RO(_attr, current_value)

#define __ATTR_CURRENT_INT_RW(_attr, _minv, _maxv, _wmi)            \
	__WMI_STORE_INT(_attr##_current_value, _minv, _maxv, _wmi); \
	WMI_SHOW_INT(_attr##_current_value, "%d\n", _wmi);          \
	static struct kobj_attribute attr_##_attr##_current_value = \
		__ASUS_ATTR_RW(_attr, current_value)

/* Shows a formatted static variable */
#define __ATTR_SHOW_FMT(_prop, _attrname, _fmt, _val)                         \
	static ssize_t _attrname##_##_prop##_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sysfs_emit(buf, _fmt, _val);                           \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_##_prop =             \
		__ASUS_ATTR_RO(_attrname, _prop)

/* Requires current_value_show */
#define __ATTR_GROUP_INT_VALUE_ONLY(_attrname, _fsname, _dispname)     \
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);   \
	static struct kobj_attribute attr_##_attrname##_type =         \
		__ASUS_ATTR_RO_AS(type, int_type_show);                \
	static struct attribute *_attrname##_attrs[] = {               \
		&attr_##_attrname##_current_value.attr,                \
		&attr_##_attrname##_display_name.attr,                 \
		&attr_##_attrname##_type.attr, NULL                    \
	};                                                             \
	static const struct attribute_group _attrname##_attr_group = { \
		.name = _fsname, .attrs = _attrname##_attrs            \
	}

/* Boolean style enumeration, base macro. Requires adding show/store */
#define __ATTR_GROUP_ENUM(_attrname, _fsname, _possible, _dispname)     \
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);    \
	__ATTR_SHOW_FMT(possible_values, _attrname, "%s\n", _possible); \
	static struct kobj_attribute attr_##_attrname##_type =          \
		__ASUS_ATTR_RO_AS(type, enum_type_show);                \
	static struct attribute *_attrname##_attrs[] = {                \
		&attr_##_attrname##_current_value.attr,                 \
		&attr_##_attrname##_display_name.attr,                  \
		&attr_##_attrname##_possible_values.attr,               \
		&attr_##_attrname##_type.attr,                          \
		NULL                                                    \
	};                                                              \
	static const struct attribute_group _attrname##_attr_group = {  \
		.name = _fsname, .attrs = _attrname##_attrs             \
	}

#define ATTR_GROUP_INT_VALUE_ONLY_RO(_attrname, _fsname, _wmi, _dispname) \
	__ATTR_CURRENT_INT_RO(_attrname, _wmi);                           \
	__ATTR_GROUP_INT_VALUE_ONLY(_attrname, _fsname, _dispname)

#define ATTR_GROUP_BOOL_RO(_attrname, _fsname, _wmi, _dispname) \
	__ATTR_CURRENT_INT_RO(_attrname, _wmi);                 \
	__ATTR_GROUP_ENUM(_attrname, _fsname, "0;1", _dispname)

#define ATTR_GROUP_BOOL_RW(_attrname, _fsname, _wmi, _dispname) \
	__ATTR_CURRENT_INT_RW(_attrname, 0, 1, _wmi);           \
	__ATTR_GROUP_ENUM(_attrname, _fsname, "0;1", _dispname)

/*
 * Requires <name>_current_value_show(), <name>_current_value_show()
 */
#define ATTR_GROUP_BOOL_CUSTOM(_attrname, _fsname, _dispname)           \
	static struct kobj_attribute attr_##_attrname##_current_value = \
		__ASUS_ATTR_RW(_attrname, current_value);               \
	__ATTR_GROUP_ENUM(_attrname, _fsname, "0;1", _dispname)

#define ATTR_GROUP_ENUM_INT_RO(_attrname, _fsname, _wmi, _possible, _dispname) \
	__ATTR_CURRENT_INT_RO(_attrname, _wmi);                                \
	__ATTR_GROUP_ENUM(_attrname, _fsname, _possible, _dispname)

/*
 * Requires <name>_current_value_show(), <name>_current_value_show()
 * and <name>_possible_values_show()
 */
#define ATTR_GROUP_ENUM_CUSTOM(_attrname, _fsname, _dispname)             \
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);      \
	static struct kobj_attribute attr_##_attrname##_current_value =   \
		__ASUS_ATTR_RW(_attrname, current_value);                 \
	static struct kobj_attribute attr_##_attrname##_possible_values = \
		__ASUS_ATTR_RO(_attrname, possible_values);               \
	static struct kobj_attribute attr_##_attrname##_type =            \
		__ASUS_ATTR_RO_AS(type, enum_type_show);                  \
	static struct attribute *_attrname##_attrs[] = {                  \
		&attr_##_attrname##_current_value.attr,                   \
		&attr_##_attrname##_display_name.attr,                    \
		&attr_##_attrname##_possible_values.attr,                 \
		&attr_##_attrname##_type.attr,                            \
		NULL                                                      \
	};                                                                \
	static const struct attribute_group _attrname##_attr_group = {    \
		.name = _fsname, .attrs = _attrname##_attrs               \
	}

/*
 * ROG PPT attributes need a little different in setup as they
 * require rog_tunables members.
 */

#define __ROG_TUNABLE_RW(_attr, _min, _max, _wmi)                             \
	static ssize_t _attr##_current_value_store(                           \
		struct kobject *kobj, struct kobj_attribute *attr,            \
		const char *buf, size_t count)                                \
	{                                                                     \
		return attr_uint_store(kobj, attr, buf, count,                 \
				      asus_armoury.rog_tunables->_min,        \
				      asus_armoury.rog_tunables->_max,        \
				      &asus_armoury.rog_tunables->_attr,      \
				      _wmi);                                  \
	}                                                                     \
	static ssize_t _attr##_current_value_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sysfs_emit(buf, "%u\n",                                \
				  asus_armoury.rog_tunables->_attr);          \
	}                                                                     \
	static struct kobj_attribute attr_##_attr##_current_value =           \
		__ASUS_ATTR_RW(_attr, current_value)

#define __ROG_TUNABLE_SHOW(_prop, _attrname, _val)                            \
	static ssize_t _attrname##_##_prop##_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sysfs_emit(buf, "%d\n",                                \
				  asus_armoury.rog_tunables->_val);           \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_##_prop =             \
		__ASUS_ATTR_RO(_attrname, _prop)

#define ATTR_GROUP_ROG_TUNABLE(_attrname, _fsname, _wmi, _default, _min, _max, \
			       _incstep, _dispname)                            \
	__ROG_TUNABLE_SHOW(default_value, _attrname, _default);                \
	__ROG_TUNABLE_RW(_attrname, _min, _max, _wmi);                         \
	__ROG_TUNABLE_SHOW(min_value, _attrname, _min);                        \
	__ROG_TUNABLE_SHOW(max_value, _attrname, _max);                        \
	__ATTR_SHOW_FMT(scalar_increment, _attrname, "%d\n", _incstep);        \
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);           \
	static struct kobj_attribute attr_##_attrname##_type =                 \
		__ASUS_ATTR_RO_AS(type, int_type_show);                        \
	static struct attribute *_attrname##_attrs[] = {                       \
		&attr_##_attrname##_current_value.attr,                        \
		&attr_##_attrname##_default_value.attr,                        \
		&attr_##_attrname##_min_value.attr,                            \
		&attr_##_attrname##_max_value.attr,                            \
		&attr_##_attrname##_scalar_increment.attr,                     \
		&attr_##_attrname##_display_name.attr,                         \
		&attr_##_attrname##_type.attr,                                 \
		NULL                                                           \
	};                                                                     \
	static const struct attribute_group _attrname##_attr_group = {         \
		.name = _fsname, .attrs = _attrname##_attrs                    \
	}

#endif /* _ASUS_BIOSCFG_H_ */
