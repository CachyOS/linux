/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DRM_SYSFS_H_
#define _DRM_SYSFS_H_
#include <linux/sched.h>

#define DRM_GPU_RESET_FLAG_VRAM_INVALID (1 << 0)

struct drm_device;
struct device;
struct drm_connector;
struct drm_property;

struct drm_reset_event {
	uint32_t pid;
	uint32_t flags;
	char pname[TASK_COMM_LEN];
};

int drm_class_device_register(struct device *dev);
void drm_class_device_unregister(struct device *dev);

void drm_sysfs_hotplug_event(struct drm_device *dev);
void drm_sysfs_connector_hotplug_event(struct drm_connector *connector);
void drm_sysfs_reset_event(struct drm_device *dev, struct drm_reset_event *reset_info);
void drm_sysfs_connector_property_event(struct drm_connector *connector,
					struct drm_property *property);
#endif
