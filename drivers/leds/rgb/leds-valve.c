// SPDX-License-Identifier: GPL-2.0+

/*
 * Valve LEDs driver
 *
 * Copyright (C) 2025 Valve Corporation
 *
 */

#include <linux/stringify.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kstrtox.h>
#include <linux/led-class-multicolor.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define DRVNAME "valve-leds"

#define VALVE_NUM_LEDS                   17
#define VALVE_NUM_COMPONENTS             3
#define VALVE_BRIGHTNESS_DEFAULT         255
#define VALVE_BRIGHTNESS_HALF            56 /* Gamma 2.2 */
#define VALVE_BRIGHTNESS_MAX             255
#define VALVE_INTENSITY_DEFAULT          255
#define VALVE_DELAY_RANGE_MIN            0
#define VALVE_DELAY_RANGE_MAX            20
#define VALVE_BIOS_CTRL_ENABLE_BIT       BIT(0)
#define VALVE_BRIGHTNESS_CTRL_SCALE_BIT  BIT(0)
#define VALVE_BRIGHTNESS_CTRL_PWRBTN_BIT BIT(1)

#define VALVE_PORT_EC_CMD                0x6c
#define VALVE_EC_CMD_COMMIT_SETTINGS     0xc6

#define VALVE_PORT_BASE                  0xde8
#define VALVE_PORT_BIOS_LED_CTRL         (0xde8 - VALVE_PORT_BASE)
#define VALVE_PORT_INTENSITY_STARTUP     (0xde9 - VALVE_PORT_BASE)
#define VALVE_PORT_BRIGHTNESS_STARTUP    (0xdec - VALVE_PORT_BASE)
#define VALVE_PORT_STRIP_ENABLE          (0xdef - VALVE_PORT_BASE)
#define VALVE_PORT_INTENSITY             (0xe39 - VALVE_PORT_BASE)
#define VALVE_PORT_MODE                  (0xe6c - VALVE_PORT_BASE)
#define VALVE_PORT_DELAY                 (0xe6e - VALVE_PORT_BASE)
#define VALVE_PORT_BREATH_OFFSET         (0xe6f - VALVE_PORT_BASE)
#define VALVE_PORT_BREATH_LEVEL          (0xe70 - VALVE_PORT_BASE)
#define VALVE_PORT_PATROL_NUM            (0xe71 - VALVE_PORT_BASE)
#define VALVE_PORT_COLOR_SHIFT           (0xe75 - VALVE_PORT_BASE)
#define VALVE_PORT_BRIGHTNESS_CTRL       (0xe78 - VALVE_PORT_BASE)
#define VALVE_PORT_BRIGHTNESS_SCALE      (0xe79 - VALVE_PORT_BASE)
#define VALVE_PORT_BRIGHTNESS_PWRBTN     (0xe7a - VALVE_PORT_BASE)
#define VALVE_NR_PORTS                   (VALVE_PORT_BRIGHTNESS_PWRBTN + 1)

#define VALVE_PORT_STRIDE                3
#define VALVE_LED_PORT(n) (VALVE_PORT_INTENSITY + n * VALVE_PORT_STRIDE)

static bool test_interface;
module_param(test_interface, bool, 0644);
MODULE_PARM_DESC(test_interface, "Just test the interface, don't write LEDs");

static int test_reg_write(void *ctx, unsigned int reg, unsigned int val)
{
	pr_info(DRVNAME " %s(): 0x%03x=0x%x\n", __func__, VALVE_PORT_BASE + reg, val);
	return 0;
}

static int test_raw_write(void *ctx, const void *data, size_t count)
{
	int i;
	const u8 *bytes = data;

	pr_info(DRVNAME " %s(): 0x%03x: ", __func__, VALVE_PORT_BASE + bytes[0]);
	for (i = 1; i < count; i++)
		pr_cont("%02x ", bytes[i]);
	pr_cont("\n");

	return 0;
}

static int test_reg_read(void *ctx, unsigned int reg, unsigned int *val)
{
	*val = 0;
	pr_info(DRVNAME " %s(): 0x%03x\n", __func__, VALVE_PORT_BASE + reg);
	return 0;
}

static int test_raw_read(void *ctx, const void *reg_buf, size_t reg_size, void *val_buf, size_t val_size)
{
	int i;
	const u8 *regs = reg_buf;

	pr_info(DRVNAME " %s(): ", __func__);
	for (i = 0; i < reg_size; i++)
		pr_cont("%03x ", VALVE_PORT_BASE + regs[i]);
	pr_cont("\n");

	return 0;
}

static const struct regmap_bus test_bus = {
	.reg_write = test_reg_write,
	.write = test_raw_write,
	.reg_read = test_reg_read,
	.read = test_raw_read,
};

struct valve_leds {
	struct platform_device *pdev;
	struct regmap *regmap;
	struct valve_led {
		struct led_classdev_mc mcdev;
		struct mc_subled rgb[VALVE_NUM_COMPONENTS];
		int index;
	} leds[VALVE_NUM_LEDS];

	int enabled;
	int effect_index;
	int delay;
	int breath_offset;
	int breath_level;
	int patrol_num;
	int color_shift;
	int brightness_ctrl;
	int brightness_scale;
};

static struct platform_device *pdev;

static const char *const effect_names[] = {
	"patrol",
	"breath",
	"factory",
	"normal",
	"off",
	"rainbow",
	"demo",
	"manual",
};

static void valve_leds_send_ec_cmd(unsigned char cmd)
{
	outb(cmd, VALVE_PORT_EC_CMD);
}

static int valve_leds_has_persistence(struct valve_leds *leds, bool *out)
{
	unsigned int val;
	int ret;

	/*
	 * Read the LED control register, and determine if persistent BIOS control
	 * is enabled.
	 */
	ret = regmap_read(leds->regmap, VALVE_PORT_BIOS_LED_CTRL, &val);
	if (ret)
		return ret;

	*out = (val & VALVE_BIOS_CTRL_ENABLE_BIT) != 0;

	return 0;
}

static int valve_leds_ensure_persistence(struct device *dev)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	unsigned int val;
	int ret;

	/* Read the LED control register. */
	ret = regmap_read(leds->regmap, VALVE_PORT_BIOS_LED_CTRL, &val);
	if (ret)
		return ret;

	/* Ensure that persistent BIOS control is enabled. */
	if ((val & VALVE_BIOS_CTRL_ENABLE_BIT) == 0) {
		ret = regmap_write(leds->regmap, VALVE_PORT_BIOS_LED_CTRL, val | VALVE_BIOS_CTRL_ENABLE_BIT);
		if (ret) {
			dev_err(dev, "Failed to enable persistent LED control\n");
			return ret;
		}
	}

	/* Success. */
	return 0;
}

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	unsigned int val;
	ssize_t len = 0;
	int ret;

	ret = regmap_read(leds->regmap, VALVE_PORT_STRIP_ENABLE, &val);
	if (ret)
		return ret;

	len = sysfs_emit(buf, "%d\n", val);

	return len;
}

static ssize_t enabled_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	int ret;
	bool enabled;

	ret = kstrtobool(buf, &enabled);
	if (ret < 0)
		return ret;

	ret = regmap_write(leds->regmap, VALVE_PORT_STRIP_ENABLE, (int)enabled);
	if (ret)
		return ret;

	return count;
}

static ssize_t effect_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	unsigned int mode;
	ssize_t len = 0;
	int ret;

	ret = regmap_read(leds->regmap, VALVE_PORT_MODE, &mode);
	if (ret)
		return ret;

	if (mode >= 0 && mode < ARRAY_SIZE(effect_names))
		len = sysfs_emit(buf, "%s\n", effect_names[mode]);

	return len;
}

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	u8 rgb[VALVE_NUM_LEDS * VALVE_PORT_STRIDE];
	int led, comp, mode, ret;

	mode = __sysfs_match_string(effect_names, ARRAY_SIZE(effect_names), buf);
	if (mode < 0)
		return mode;

	ret = regmap_write(leds->regmap, VALVE_PORT_MODE, mode);
	if (ret)
		return ret;

	/*
	 * Some effects animate and clobber colors, thus re-write colors after
	 * setting the mode.
	 */
	for (led = 0; led < VALVE_NUM_LEDS; led++)
		for (comp = 0; comp < VALVE_NUM_COMPONENTS; comp++)
			rgb[led * VALVE_NUM_COMPONENTS + comp] = leds->leds[led].rgb[comp].intensity;

	ret = regmap_bulk_write(leds->regmap, VALVE_LED_PORT(0), rgb, ARRAY_SIZE(rgb));
	if (ret)
		return ret;

	return count;
}

static ssize_t effect_index_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(effect_names); i++)
		len += sysfs_emit_at(buf, len, "%s ", effect_names[i]);

	buf[len - 1] = '\n';

	return len;
}

static ssize_t delay_range_show(struct device *dev, struct device_attribute *attr,
	                        char *buf)
{
	const ssize_t len = sysfs_emit(buf, "%d-%d ", VALVE_DELAY_RANGE_MIN,
		VALVE_DELAY_RANGE_MAX);

	buf[len - 1] = '\n';

	return len;
}

static ssize_t brightness_startup_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	unsigned int val;
	bool enabled;
	int ret;

	/*
	 * If persistent BIOS control is enabled return the startup brightness
	 * register value.
	 * Otherwise return the default value, which is half brightness.
	 */
	ret = valve_leds_has_persistence(leds, &enabled);
	if (ret)
		return ret;

	if (enabled) {
		ret = regmap_read(leds->regmap, VALVE_PORT_BRIGHTNESS_STARTUP, &val);
		if (ret)
			return ret;
	} else {
		val = VALVE_BRIGHTNESS_HALF;
	}

	return sysfs_emit(buf, "0x%02x\n", val);
}

static ssize_t brightness_startup_store(struct device *dev, struct device_attribute *attr,
	                                const char *buf, size_t count)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	/* Ensure that persistence is enabled. */
	ret = valve_leds_ensure_persistence(dev);
	if (ret)
		return ret;

	/* Write the persistent startup brightness value. */
	ret = regmap_write(leds->regmap, VALVE_PORT_BRIGHTNESS_STARTUP, val);
	if (ret)
		return ret;

	/* Command the EC to commit the new brightness value to non-volatile storage. */
	valve_leds_send_ec_cmd(VALVE_EC_CMD_COMMIT_SETTINGS);

	return count;
}

static ssize_t multi_intensity_startup_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	bool enabled;
	u8 rgb[VALVE_NUM_COMPONENTS] = { 0, 0, 255 }; /* pure blue by default */
	int ret;

	/*
	 * If persistent BIOS control is enabled return the startup color register
	 * values.
	 * Otherwise return the default values.
	 */
	ret = valve_leds_has_persistence(leds, &enabled);
	if (ret)
		return ret;

	if (enabled) {
		ret = regmap_bulk_read(leds->regmap, VALVE_PORT_INTENSITY_STARTUP, rgb, ARRAY_SIZE(rgb));
		if (ret)
			return ret;
	}

	return sysfs_emit(buf, "%hhu %hhu %hhu\n", rgb[0], rgb[1], rgb[2]);
}

static ssize_t multi_intensity_startup_store(struct device *dev, struct device_attribute *attr,
	                                const char *buf, size_t count)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	u8 rgb[VALVE_NUM_COMPONENTS];
	int ret;

	ret = sscanf(buf, "%hhu %hhu %hhu", &rgb[0], &rgb[1], &rgb[2]);
	if (ret != 3)
		return -EINVAL;

	/* Ensure that persistence is enabled. */
	ret = valve_leds_ensure_persistence(dev);
	if (ret)
		return ret;

	/* Write the persistent startup color values. */
	ret = regmap_bulk_write(leds->regmap, VALVE_PORT_INTENSITY_STARTUP, rgb, VALVE_NUM_COMPONENTS);
	if (ret)
		return ret;

	/* Command the EC to commit the new values to non-volatile storage. */
	valve_leds_send_ec_cmd(VALVE_EC_CMD_COMMIT_SETTINGS);

	return count;
}

static DEVICE_ATTR_RW(enabled);
static DEVICE_ATTR_RW(effect);
static DEVICE_ATTR_RO(effect_index);
static DEVICE_ATTR_RO(delay_range);
static DEVICE_ATTR_RW(brightness_startup);
static DEVICE_ATTR_RW(multi_intensity_startup);

static ssize_t byte_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	struct dev_ext_attribute *ext = container_of(attr, typeof(*ext), attr);
	unsigned int port = (long)ext->var;
	unsigned int val;
	int ret;

	ret = regmap_read(leds->regmap, port, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%02x\n", val);
}

static ssize_t byte_reg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct valve_leds *leds = dev_get_drvdata(dev->parent);
	struct dev_ext_attribute *ext = container_of(attr, typeof(*ext), attr);
	unsigned int port = (long)ext->var;
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	ret = regmap_write(leds->regmap, port, val);
	if (ret)
		return ret;

	return size;
}

#define VALVE_LEDS_BYTE_ATTR(_name, _mode, _port) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, byte_reg_show, byte_reg_store), ((void *)_port) }

VALVE_LEDS_BYTE_ATTR(delay, 0644, VALVE_PORT_DELAY);
VALVE_LEDS_BYTE_ATTR(breath_offset, 0644, VALVE_PORT_BREATH_OFFSET);
VALVE_LEDS_BYTE_ATTR(breath_level, 0644, VALVE_PORT_BREATH_LEVEL);
VALVE_LEDS_BYTE_ATTR(patrol_num, 0644, VALVE_PORT_PATROL_NUM);
VALVE_LEDS_BYTE_ATTR(color_shift, 0644, VALVE_PORT_COLOR_SHIFT);
VALVE_LEDS_BYTE_ATTR(brightness_scale, 0644, VALVE_PORT_BRIGHTNESS_SCALE);

static struct attribute *valve_leds_attrs[] = {
	&dev_attr_effect.attr,
	&dev_attr_effect_index.attr,
	&dev_attr_enabled.attr,
	&dev_attr_delay.attr.attr,
	&dev_attr_delay_range.attr,
	&dev_attr_breath_offset.attr.attr,
	&dev_attr_breath_level.attr.attr,
	&dev_attr_patrol_num.attr.attr,
	&dev_attr_color_shift.attr.attr,
	&dev_attr_brightness_scale.attr.attr,
	&dev_attr_brightness_startup.attr,
	&dev_attr_multi_intensity_startup.attr,
	NULL,
};
static const struct attribute_group valve_leds_attr_group = {
	.attrs = valve_leds_attrs,
};

static int valve_leds_init_brightness_control(struct platform_device *pdev, struct valve_leds *leds)
{
	unsigned int initval = VALVE_BRIGHTNESS_HALF;
	unsigned int val;
	int ret;

	/*
	 * Read the default brightness from BIOS settings, if BIOS control is enabled.
	 * If BIOS control is not enabled, fall back to half brightness.
	 */
	ret = regmap_read(leds->regmap, VALVE_PORT_BIOS_LED_CTRL, &val);
	if (ret)
		return ret;

	if ((val & VALVE_BIOS_CTRL_ENABLE_BIT) != 0) {
		ret = regmap_read(leds->regmap, VALVE_PORT_BRIGHTNESS_STARTUP, &val);
		if (ret)
			return ret;

		initval = val;

		dev_info(&pdev->dev, "Init with BIOS set brightness: %u\n", initval);
	} else {
		dev_info(&pdev->dev, "No BIOS set brightness. Falling back to: %u\n", initval);
	}

	/* Set the control bit to enable brightness control. */
	ret = regmap_write(leds->regmap, VALVE_PORT_BRIGHTNESS_CTRL, VALVE_BRIGHTNESS_CTRL_SCALE_BIT);
	if (ret)
		return ret;

	/* Initialize with the default brightness value. */
	ret = regmap_write(leds->regmap, VALVE_PORT_BRIGHTNESS_SCALE, initval);

	return ret;
}

static int valve_leds_set_brightness(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	struct valve_leds *leds = dev_get_drvdata(led_cdev->dev->parent);
	struct led_classdev_mc *mcdev = lcdev_to_mccdev(led_cdev);
	struct valve_led *led = container_of(mcdev, typeof(*led), mcdev);
	char rgb[3];
	int ret;

	led_mc_calc_color_components(mcdev, brightness);

	rgb[0] = led->rgb[0].brightness;
	rgb[1] = led->rgb[1].brightness;
	rgb[2] = led->rgb[2].brightness;

	ret = regmap_bulk_write(leds->regmap, VALVE_LED_PORT(led->index), rgb, 3);
	return ret;
}


#define VALVE_REG_SYNC_FROM_HW(leds, reg, store) do { \
	int val; \
	int ret = regmap_read(leds->regmap, reg, &val); \
	if (ret) { \
		dev_err(&leds->pdev->dev, "Failed to sync %s from hw: %d\n", __stringify(reg), ret); \
		return ret; \
	} \
	store = val; \
} while (0)

static int valve_leds_sync_from_hw(struct valve_leds *leds)
{
	u8 rgb[VALVE_NUM_LEDS * VALVE_PORT_STRIDE];
	u8 *port;
	int ret;
	int led, comp;

	ret = regmap_bulk_read(leds->regmap, VALVE_LED_PORT(0), rgb, ARRAY_SIZE(rgb));
	if (ret) {
		dev_err(&leds->pdev->dev, "Failed to sync from hw: %d\n", ret);
		return ret;
	}

	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_STRIP_ENABLE, leds->enabled);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_MODE, leds->effect_index);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_DELAY, leds->delay);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_BREATH_OFFSET, leds->breath_offset);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_BREATH_LEVEL, leds->breath_level);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_PATROL_NUM, leds->patrol_num);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_COLOR_SHIFT, leds->color_shift);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_BRIGHTNESS_CTRL, leds->brightness_ctrl);
	VALVE_REG_SYNC_FROM_HW(leds, VALVE_PORT_BRIGHTNESS_SCALE, leds->brightness_scale);

	port = rgb;
	for (led = 0; led < VALVE_NUM_LEDS; led++)
		for (comp = 0; comp < VALVE_NUM_COMPONENTS; comp++)
			leds->leds[led].rgb[comp].intensity = *port++;

	return 0;
}

#define VALVE_REG_SYNC_TO_HW(leds, reg, val) do { \
	int ret = regmap_write(leds->regmap, reg, val); \
	if (ret) { \
		dev_err(&leds->pdev->dev, "Failed to sync %s to hw: %d\n", __stringify(reg), ret); \
		return ret; \
	} \
} while (0)

static int valve_leds_sync_to_hw(struct valve_leds *leds)
{
	u8 rgb[VALVE_NUM_LEDS * VALVE_PORT_STRIDE];
	u8 *port;
	int ret;
	int led, comp;

	port = &rgb[0];
	for (led = 0; led < VALVE_NUM_LEDS; led++)
		for (comp = 0; comp < VALVE_NUM_COMPONENTS; comp++)
			*port++ = leds->leds[led].rgb[comp].intensity;

	ret = regmap_bulk_write(leds->regmap, VALVE_LED_PORT(0), rgb, ARRAY_SIZE(rgb));
	if (ret) {
		dev_err(&leds->pdev->dev, "Failed to sync to hw: %d\n", ret);
		return ret;
	}

	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_DELAY, leds->delay);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_BREATH_OFFSET, leds->breath_offset);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_BREATH_LEVEL, leds->breath_level);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_PATROL_NUM, leds->patrol_num);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_COLOR_SHIFT, leds->color_shift);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_BRIGHTNESS_CTRL, leds->brightness_ctrl);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_BRIGHTNESS_SCALE, leds->brightness_scale);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_MODE, leds->effect_index);
	VALVE_REG_SYNC_TO_HW(leds, VALVE_PORT_STRIP_ENABLE, leds->enabled);

	return 0;
}

static int valve_leds_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct valve_leds *leds = platform_get_drvdata(pdev);

	return valve_leds_sync_from_hw(leds);
}

static int valve_leds_resume(struct platform_device *pdev)
{
	struct valve_leds *leds = platform_get_drvdata(pdev);

	return valve_leds_sync_to_hw(leds);
}

static const struct dmi_system_id valve_leds_dmi_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "OEM"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "F7F"),
		},
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Fremont"),
		},
	},
	{}
};

static int valve_leds_probe(struct platform_device *pdev)
{
	struct valve_leds *vleds;
	int i, c;
	int ret;
	void __iomem *ports;

	static const struct regmap_config cfg = {
		.name = "leds",
		.reg_bits = 8,
		.val_bits = 8,
		.io_port = true,
	};

	if (!test_interface && !dmi_check_system(valve_leds_dmi_table))
		return -ENODEV;

	vleds = devm_kzalloc(&pdev->dev, sizeof(*vleds), GFP_KERNEL);
	if (!vleds)
		return -ENOMEM;

	platform_set_drvdata(pdev, vleds);

	if (test_interface) {
		vleds->regmap = devm_regmap_init(&pdev->dev, &test_bus, NULL, &cfg);
	} else {
		if (!devm_request_region(&pdev->dev, VALVE_PORT_BASE, VALVE_NR_PORTS, DRVNAME))
			return -EBUSY;

		ports = devm_ioport_map(&pdev->dev, VALVE_PORT_BASE, VALVE_NR_PORTS);
		if (!ports)
			return -ENOMEM;

		vleds->regmap = devm_regmap_init_mmio(&pdev->dev, ports, &cfg);

		/* Request the EC command I/O port. */
		if (!devm_request_region(&pdev->dev, VALVE_PORT_EC_CMD, 1, DRVNAME)) {
			dev_err(&pdev->dev, "Failed to request EC cmd i/o port\n");
			return -EBUSY;
		}
	}

	if (IS_ERR(vleds->regmap)) {
		dev_err(&pdev->dev, "Failed to init regmap\n");
		return PTR_ERR(vleds->regmap);
	}

	vleds->pdev = pdev;

	ret = valve_leds_sync_from_hw(vleds);
	if (ret) {
		dev_err(&pdev->dev, "%s(): Failed to read led state: %d\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < VALVE_NUM_LEDS; i++) {
		for (c = 0; c < VALVE_NUM_COMPONENTS; c++) {
			vleds->leds[i].rgb[c].color_index = LED_COLOR_ID_RED + c;
			vleds->leds[i].rgb[c].brightness = VALVE_BRIGHTNESS_DEFAULT;
			vleds->leds[i].rgb[c].channel = c;
		}

		vleds->leds[i].index = i;
		vleds->leds[i].mcdev.led_cdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s[%d]", DRVNAME, i);
		vleds->leds[i].mcdev.led_cdev.brightness = VALVE_BRIGHTNESS_DEFAULT;
		vleds->leds[i].mcdev.led_cdev.max_brightness = VALVE_BRIGHTNESS_MAX;
		vleds->leds[i].mcdev.num_colors = VALVE_NUM_COMPONENTS;
		vleds->leds[i].mcdev.subled_info = vleds->leds[i].rgb;
		vleds->leds[i].mcdev.led_cdev.brightness_set_blocking = valve_leds_set_brightness;

		ret = devm_led_classdev_multicolor_register(&pdev->dev, &vleds->leds[i].mcdev);
		if (ret)
			return ret;

		ret = devm_device_add_group(vleds->leds[i].mcdev.led_cdev.dev, &valve_leds_attr_group);
		if (ret)
			return ret;
	}

	/*
	 * The driver takes control of LED brightness. Enable the brightness control
	 * that affects the entire LED strip, and initialize it with the default
	 * brightness value set in BIOS.
	 */
	ret = valve_leds_init_brightness_control(pdev, vleds);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init brightness control\n");
		return ret;
	}

	return 0;
}

static struct platform_driver valve_leds_driver = {
	.probe = valve_leds_probe,
	.suspend = valve_leds_suspend,
	.resume = valve_leds_resume,
	.driver = {
		.name = DRVNAME,
	},
};

static int __init valve_leds_init(void)
{
	int ret;

	ret = platform_driver_register(&valve_leds_driver);
	if (ret < 0) {
		pr_err("%s(): Failed to register driver: %d\n", __func__, ret);
		return ret;
	}

	pdev = platform_device_register_simple(DRVNAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		pr_err("%s(): Failed to register device: %ld\n", __func__, PTR_ERR(pdev));
		platform_driver_unregister(&valve_leds_driver);
		return PTR_ERR(pdev);
	}

	return 0;
}

static void __exit valve_leds_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&valve_leds_driver);
}

module_init(valve_leds_init);
module_exit(valve_leds_exit);

MODULE_DEVICE_TABLE(dmi, valve_leds_dmi_table);
MODULE_AUTHOR("Robert Beckett <bob.beckett@collabora.com>");
MODULE_DESCRIPTION("Valve LEDs driver");
MODULE_LICENSE("GPL");
