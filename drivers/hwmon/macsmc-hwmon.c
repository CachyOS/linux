// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC hwmon driver for Apple Silicon platforms
 *
 * The System Management Controller on Apple Silicon And Intel Macs is responsible for
 * measuring data from sensors across the SoC and machine. These include power,
 * temperature, voltage and current sensors. Some "sensors" actually expose
 * derived values. An example of this is the key PHPC, which is an estimate
 * of the heat energy being dissipated by the SoC.
 *
 * While each SoC only has one SMC variant, each platform exposes a different
 * set of sensors. For example, M1 MacBooks expose battery telemetry sensors
 * which are not present on the M1 Mac mini. For this reason, the available
 * sensors for a given platform are described in the device tree in a child
 * node of the SMC device. We must walk this list of available sensors and
 * populate the required hwmon data structures at runtime.
 *
 * Originally based on a concept by Jean-Francois Bortolotti <jeff@borto.fr>
 *
 * Copyright The Asahi Linux Contributors
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/hwmon.h>
#include <linux/math64.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MAX_LABEL_LENGTH	32

/* Temperature, voltage, current, power, fan(s) */
#define NUM_SENSOR_TYPES	5

#define FLT_EXP_BIAS	127
#define FLT_EXP_MASK	GENMASK(30, 23)
#define FLT_MANT_BIAS	23
#define FLT_MANT_MASK	GENMASK(22, 0)
#define FLT_SIGN_MASK	BIT(31)

static bool fan_control;
module_param_unsafe(fan_control, bool, 0644);
MODULE_PARM_DESC(fan_control,
		 "Override the SMC to set your own fan speeds on supported machines");

struct macsmc_hwmon_sensor {
	struct apple_smc_key_info info;
	smc_key macsmc_key;
	char *label;
	u32 attrs;
};

struct macsmc_hwmon_fan {
	struct macsmc_hwmon_sensor now;
	struct macsmc_hwmon_sensor min;
	struct macsmc_hwmon_sensor max;
	struct macsmc_hwmon_sensor set;
	struct macsmc_hwmon_sensor mode;
	char label[MAX_LABEL_LENGTH];
	u32 attrs;
	bool manual;
};

struct macsmc_hwmon_sensors {
	struct hwmon_channel_info channel_info;
	struct macsmc_hwmon_sensor *sensors;
	u32 count;
};

struct macsmc_hwmon_fans {
	struct hwmon_channel_info channel_info;
	struct macsmc_hwmon_fan *fans;
	u8 count;
	bool has_old_manual;
};

struct macsmc_hwmon {
	struct device *dev;
	struct apple_smc *smc;
	struct device *hwmon_dev;
	struct hwmon_chip_info chip_info;
	/* Chip + sensor types + NULL */
	const struct hwmon_channel_info *channel_infos[1 + NUM_SENSOR_TYPES + 1];
	struct macsmc_hwmon_sensors temp;
	struct macsmc_hwmon_sensors volt;
	struct macsmc_hwmon_sensors curr;
	struct macsmc_hwmon_sensors power;
	struct macsmc_hwmon_fans fan;
};

static int macsmc_hwmon_read_label(struct device *dev,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel, const char **str)
{
	struct macsmc_hwmon *hwmon = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		*str = hwmon->temp.sensors[channel].label;
		break;
	case hwmon_in:
		*str = hwmon->volt.sensors[channel].label;
		break;
	case hwmon_curr:
		*str = hwmon->curr.sensors[channel].label;
		break;
	case hwmon_power:
		*str = hwmon->power.sensors[channel].label;
		break;
	case hwmon_fan:
		*str = hwmon->fan.fans[channel].label;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * A number of sensors report data in a 48.16 fixed-point decimal format that is
 * not used by any other function of the SMC.
 */
static int macsmc_hwmon_read_ioft_scaled(struct apple_smc *smc, smc_key key,
					 u64 *p, int scale)
{
	u64 val;
	int ret;

	ret = apple_smc_read_u64(smc, key, &val);
	if (ret < 0)
		return ret;

	*p = mul_u64_u32_div(val, scale, 65536);

	return 0;
}

/*
 * Many sensors report their data as IEEE-754 floats. No other SMC function uses
 * them.
 */
static int macsmc_hwmon_read_f32_scaled(struct apple_smc *smc, smc_key key,
					long *p, int scale)
{
	u32 fval;
	u64 val;
	int ret, exp;

	ret = apple_smc_read_u32(smc, key, &fval);
	if (ret < 0)
		return ret;

	val = ((u64)((fval & FLT_MANT_MASK) | BIT(23)));
	exp = ((fval >> 23) & 0xff) - FLT_EXP_BIAS - FLT_MANT_BIAS;

	/* We never have negatively scaled SMC floats */
	val *= scale;

	if (exp > 63)
		val = U64_MAX;
	else if (exp < -63)
		val = 0;
	else if (exp < 0)
		val >>= -exp;
	else if (exp != 0 && (val & ~((1ULL << (64 - exp)) - 1))) /* overflow */
		val = U64_MAX;
	else
		val <<= exp;

	if (fval & FLT_SIGN_MASK) {
		if (val > (u64)LONG_MAX + 1)
			*p = LONG_MIN;
		else
			*p = -(long)val;
	} else {
		if (val > (u64)LONG_MAX)
			*p = LONG_MAX;
		else
			*p = (long)val;
	}

	return 0;
}

/*
 * The SMC has keys of multiple types, denoted by a FourCC of the same format
 * as the key ID. We don't know what data type a key encodes until we poke at it.
 */
static int macsmc_hwmon_read_key(struct apple_smc *smc,
				 struct macsmc_hwmon_sensor *sensor, int scale,
				 long *val)
{
	int ret;

	switch (sensor->info.type_code) {
	/* 32-bit IEEE 754 float */
	case __SMC_KEY('f', 'l', 't', ' '): {
		long flt_ = 0;

		ret = macsmc_hwmon_read_f32_scaled(smc, sensor->macsmc_key,
						   &flt_, scale);
		if (ret)
			return ret;

		*val = flt_;
		break;
	}
	/* 48.16 fixed point decimal */
	case __SMC_KEY('i', 'o', 'f', 't'): {
		u64 ioft = 0;

		ret = macsmc_hwmon_read_ioft_scaled(smc, sensor->macsmc_key,
						    &ioft, scale);
		if (ret)
			return ret;

		if (ioft > LONG_MAX)
			*val = LONG_MAX;
		else
			*val = (long)ioft;
		break;
	}
	/* unsigned 8-bit integer */
	case __SMC_KEY('u', 'i', '8', ' '): {
		u8 ui8 = 0;

		ret = apple_smc_read_u8(smc, sensor->macsmc_key, &ui8);
		if (ret)
			return ret;

		*val = ui8 * scale;
		break;
	}
	/* signed 8-bit integer */
	case __SMC_KEY('s', 'i', '8', ' '): {
		s8 si8 = 0;

		ret = apple_smc_read_s8(smc, sensor->macsmc_key, &si8);
		if (ret)
			return ret;

		*val = si8 * scale;
		break;
	}
	/* unsigned 16-bit integer */
	case __SMC_KEY('u', 'i', '1', '6'): {
		u16 ui16 = 0;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &ui16);
		if (ret)
			return ret;

		*val = ui16 * scale;
		break;
	}
	/* signed 16-bit integer */
	case __SMC_KEY('s', 'i', '1', '6'): {
		s16 si16 = 0;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &si16);
		if (ret)
			return ret;

		*val = si16 * scale;
		break;
	}
	/* unsigned 32-bit integer */
	case __SMC_KEY('u', 'i', '3', '2'): {
		u32 ui32 = 0;

		ret = apple_smc_read_u32(smc, sensor->macsmc_key, &ui32);
		if (ret)
			return ret;

		*val = ui32 * scale;
		break;
	}
	/* signed 32-bit integer */
	case __SMC_KEY('s', 'i', '3', '2'): {
		s32 si32;

		ret = apple_smc_read_s32(smc, sensor->macsmc_key, &si32);
		if (ret)
			return ret;

		*val = si32 * scale;
		break;
	}
	/* 2.14 unsigned fixed point decimal */
	case __SMC_KEY('f', 'p', '2', 'e'): {
		u16 fp2e;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fp2e);
		if (ret)
			return ret;

		*val = mult_frac(be16_to_cpu(fp2e), scale, BIT(14));
		break;
	}

	/* 3.13 unsigned fixed point decimal */
	case __SMC_KEY('f', 'p', '3', 'd'): {
		u16 fp3d;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fp3d);
		if (ret)
			return ret;

		*val = mult_frac(be16_to_cpu(fp3d), scale, BIT(13));
		break;
	}

	/* 4.12 unsigned fixed point decimal */
	case __SMC_KEY('f', 'p', '4', 'c'): {
		u16 fp4c;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fp4c);
		if (ret)
			return ret;

		*val = mult_frac(be16_to_cpu(fp4c), scale, BIT(12));
		break;
	}

	/* 5.11 unsigned fixed point decimal */
	case __SMC_KEY('f', 'p', '5', 'b'): {
		u16 fp5b;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fp5b);
		if (ret)
			return ret;

		*val = mult_frac(be16_to_cpu(fp5b), scale, BIT(11));
		break;
	}

	/* 6.10 unsigned fixed point decimal */
	case __SMC_KEY('f', 'p', '6', 'a'): {
		u16 fp6a;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fp6a);
		if (ret)
			return ret;

		*val = mult_frac(be16_to_cpu(fp6a), scale, BIT(10));
		break;
	}

	/* 8.8 unsigned fixed point decimal */
	case __SMC_KEY('f', 'p', '8', '8'): {
		u16 fp88;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fp88);
		if (ret)
			return ret;

		*val = mult_frac(be16_to_cpu(fp88), scale, BIT(8));
		break;
	}
	/* 14.2 fixed point decimal */
	case __SMC_KEY('f', 'p', 'e', '2'): {
		/*
		 * fpe2 is only used for fan stats and fan control
		 * so precision is not needed
		 */
		u16 fpe2;

		ret = apple_smc_read_u16(smc, sensor->macsmc_key, &fpe2);
		if (ret)
			return ret;

		*val = (be16_to_cpu(fpe2) >> 2);
		break;
	}
	/* 1.14 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '1', 'e'): {
		s16 sp1e;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp1e);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp1e), scale, BIT(14));
		break;
	}

	/* 2.13 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '2', 'd'): {
		s16 sp2d;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp2d);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp2d), scale, BIT(13));
		break;
	}

	/* 3.12 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '3', 'c'): {
		s16 sp3c;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp3c);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp3c), scale, BIT(12));
		break;
	}

	/* 4.11 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '4', 'b'): {
		s16 sp4b;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp4b);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp4b), scale, BIT(11));
		break;
	}

	/* 5.10 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '5', 'a'): {
		s16 sp5a;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp5a);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp5a), scale, BIT(10));
		break;
	}

	/* 6.9 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '6', '9'): {
		s16 sp69;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp69);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp69), scale, BIT(9));
		break;
	}

	/* 7.8 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '7', '8'): {
		s16 sp78;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp78);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp78), scale, BIT(8));
		break;
	}

	/* 8.7 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '8', '7'): {
		s16 sp87;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp87);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp87), scale, BIT(7));
		break;
	}

	/* 9.6 signed fixed point decimal */
	case __SMC_KEY('s', 'p', '9', '6'): {
		s16 sp96;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &sp96);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(sp96), scale, BIT(6));
		break;
	}

	/* 10.5 signed fixed point decimal */
	case __SMC_KEY('s', 'p', 'a', '5'): {
		s16 spa5;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &spa5);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(spa5), scale, BIT(5));
		break;
	}

	/* 12.3 signed fixed point decimal */
	case __SMC_KEY('s', 'p', 'c', '3'): {
		s16 spc3;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &spc3);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(spc3), scale, BIT(3));
		break;
	}

	/* 13.2 signed fixed point decimal */
	case __SMC_KEY('s', 'p', 'd', '2'): {
		s16 spd2;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &spd2);
		if (ret)
			return ret;

		*val = mult_frac((s16)be16_to_cpu(spd2), scale, BIT(2));
		break;
	}

	/* 15.0 signed fixed point decimal */
	case __SMC_KEY('s', 'p', 'f', '0'): {
		s16 spf0;

		ret = apple_smc_read_s16(smc, sensor->macsmc_key, &spf0);
		if (ret)
			return ret;

		*val = (s16)be16_to_cpu(spf0) * scale;
		break;
	}
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int macsmc_hwmon_write_f32(struct apple_smc *smc, smc_key key, long value)
{
	u64 val;
	u32 fval = 0;
	int exp, neg;

	neg = value < 0;
	val = abs(value);

	if (val) {
		exp = __fls(val);

		if (exp > 23)
			val >>= exp - 23;
		else
			val <<= 23 - exp;

		fval = FIELD_PREP(FLT_SIGN_MASK, neg) |
			FIELD_PREP(FLT_EXP_MASK, exp + FLT_EXP_BIAS) |
			FIELD_PREP(FLT_MANT_MASK, val & FLT_MANT_MASK);
	}

	return apple_smc_write_u32(smc, key, fval);
}

static int macsmc_hwmon_write_key(struct apple_smc *smc,
				  struct macsmc_hwmon_sensor *sensor, long val)
{
	switch (sensor->info.type_code) {
	/* 32-bit IEEE 754 float */
	case __SMC_KEY('f', 'l', 't', ' '):
		return macsmc_hwmon_write_f32(smc, sensor->macsmc_key, val);
	/* unsigned 8-bit integer */
	case __SMC_KEY('u', 'i', '8', ' '):
		return apple_smc_write_u8(smc, sensor->macsmc_key, val);
	/* 14.2 fixed point decimal */
	case __SMC_KEY('f', 'p', 'e', '2'):
		return apple_smc_write_u16(smc, sensor->macsmc_key, cpu_to_be16(val << 2));
	default:
		return -EOPNOTSUPP;
	}
}

static int macsmc_hwmon_read_fan(struct macsmc_hwmon *hwmon, u32 attr, int chan,
				 long *val)
{
	switch (attr) {
	case hwmon_fan_input:
		return macsmc_hwmon_read_key(hwmon->smc,
					     &hwmon->fan.fans[chan].now, 1, val);
	case hwmon_fan_min:
		return macsmc_hwmon_read_key(hwmon->smc,
					     &hwmon->fan.fans[chan].min, 1, val);
	case hwmon_fan_max:
		return macsmc_hwmon_read_key(hwmon->smc,
					     &hwmon->fan.fans[chan].max, 1, val);
	case hwmon_fan_target:
		return macsmc_hwmon_read_key(hwmon->smc,
					     &hwmon->fan.fans[chan].set, 1, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int macsmc_hwmon_write_old_fan_manual(struct macsmc_hwmon *hwmon, u8 val, int channel)
{
	int ret;
	u16 buf;

	ret = apple_smc_read_u16(hwmon->smc, __SMC_KEY('F', 'S', '!', ' '), &buf);
	if (ret)
		return ret;

	if (val)
		buf |= BIT(channel);
	else
		buf &= ~BIT(channel);

	return apple_smc_write_u16(hwmon->smc, __SMC_KEY('F', 'S', '!', ' '), cpu_to_be16(buf));
}

static int macsmc_hwmon_write_fan(struct device *dev, u32 attr, int channel,
				  long val)
{
	struct macsmc_hwmon *hwmon = dev_get_drvdata(dev);
	long min, max;
	int ret;

	if (!fan_control || (hwmon->fan.fans[channel].mode.macsmc_key == 0 &&
				!hwmon->fan.has_old_manual))
		return -EOPNOTSUPP;

	/*
	 * The SMC does no sanity checks on requested fan speeds, so we need to.
	 */
	ret = macsmc_hwmon_read_key(hwmon->smc, &hwmon->fan.fans[channel].min,
				    1, &min);
	if (ret)
		return ret;

	ret = macsmc_hwmon_read_key(hwmon->smc, &hwmon->fan.fans[channel].max,
				    1, &max);
	if (ret)
		return ret;

	if (val >= min && val <= max) {
		if (!hwmon->fan.fans[channel].manual) {
			/* Write 1 to mode key for manual control */
			if (hwmon->fan.has_old_manual)
				ret = macsmc_hwmon_write_old_fan_manual(hwmon, 1, channel);
			else
				ret = macsmc_hwmon_write_key(hwmon->smc,
						     &hwmon->fan.fans[channel].mode, 1);
			if (ret < 0)
				return ret;

			hwmon->fan.fans[channel].manual = true;
		}
		return macsmc_hwmon_write_key(hwmon->smc,
					      &hwmon->fan.fans[channel].set, val) > 0;
	} else if (!val) {
		if (hwmon->fan.fans[channel].manual) {
			if (hwmon->fan.has_old_manual)
				ret = macsmc_hwmon_write_old_fan_manual(hwmon, 0, channel);
			else
				ret = macsmc_hwmon_write_key(hwmon->smc,
						     &hwmon->fan.fans[channel].mode, 0);
			if (ret < 0)
				return ret;

			hwmon->fan.fans[channel].manual = false;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static int macsmc_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct macsmc_hwmon *hwmon = dev_get_drvdata(dev);
	int ret = 0;

	switch (type) {
	case hwmon_temp:
		ret = macsmc_hwmon_read_key(hwmon->smc,
					    &hwmon->temp.sensors[channel], 1000, val);
		break;
	case hwmon_in:
		ret = macsmc_hwmon_read_key(hwmon->smc,
					    &hwmon->volt.sensors[channel], 1000, val);
		break;
	case hwmon_curr:
		ret = macsmc_hwmon_read_key(hwmon->smc,
					    &hwmon->curr.sensors[channel], 1000, val);
		break;
	case hwmon_power:
		/* SMC returns power in Watts with acceptable precision to scale to uW */
		ret = macsmc_hwmon_read_key(hwmon->smc,
					    &hwmon->power.sensors[channel],
					    1000000, val);
		break;
	case hwmon_fan:
		ret = macsmc_hwmon_read_fan(hwmon, attr, channel, val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

static int macsmc_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_fan:
		return macsmc_hwmon_write_fan(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t macsmc_hwmon_fan_is_visible(const struct macsmc_hwmon_fan *fan,
					   u32 attr, bool has_old_manual)
{
	if (fan->attrs & BIT(attr)) {
		if (attr == hwmon_fan_target &&
				(fan->mode.macsmc_key || has_old_manual))
			return 0644;

		return 0444;
	}

	return 0;
}

static umode_t macsmc_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type, u32 attr,
					int channel)
{
	const struct macsmc_hwmon *hwmon = data;
	struct macsmc_hwmon_sensor *sensor;

	switch (type) {
	case hwmon_in:
		sensor = &hwmon->volt.sensors[channel];
		break;
	case hwmon_curr:
		sensor = &hwmon->curr.sensors[channel];
		break;
	case hwmon_power:
		sensor = &hwmon->power.sensors[channel];
		break;
	case hwmon_temp:
		sensor = &hwmon->temp.sensors[channel];
		break;
	case hwmon_fan:
		return macsmc_hwmon_fan_is_visible(&hwmon->fan.fans[channel],
				attr, hwmon->fan.has_old_manual);
	default:
		return 0;
	}

	/* Sensors only register ro attributes */
	if (sensor->attrs & BIT(attr))
		return 0444;

	return 0;
}

static const struct hwmon_ops macsmc_hwmon_ops = {
	.is_visible = macsmc_hwmon_is_visible,
	.read = macsmc_hwmon_read,
	.read_string = macsmc_hwmon_read_label,
	.write = macsmc_hwmon_write,
};

/*
 * Get the key metadata, including key data type, from the SMC.
 */
static int macsmc_hwmon_parse_key(struct device *dev, struct apple_smc *smc,
				  struct macsmc_hwmon_sensor *sensor,
				  const char *key)
{
	int ret;

	ret = apple_smc_get_key_info(smc, _SMC_KEY(key), &sensor->info);
	if (ret) {
		dev_dbg(dev, "Failed to retrieve key info for %s\n", key);
		return ret;
	}

	sensor->macsmc_key = _SMC_KEY(key);

	return 0;
}

/*
 * A sensor is a single key-value pair as made available by the SMC.
 * The devicetree gives us the SMC key ID and a friendly name where the
 * purpose of the sensor is known.
 */
static int macsmc_hwmon_create_sensor(struct device *dev, struct apple_smc *smc,
				      struct device_node *sensor_node,
				      struct macsmc_hwmon_sensor *sensor)
{
	const char *key, *label;
	int ret;

	ret = of_property_read_string(sensor_node, "apple,key-id", &key);
	if (ret) {
		dev_dbg(dev, "Could not find apple,key-id in sensor node\n");
		return ret;
	}

	ret = macsmc_hwmon_parse_key(dev, smc, sensor, key);
	if (ret)
		return ret;

	ret = of_property_read_string(sensor_node, "label", &label);
	if (ret)
		dev_dbg(dev, "No label found for sensor %s\n", key);
	else
		strscpy_pad(sensor->label, label, sizeof(sensor->label));

	return 0;
}

struct macsmc_sensor_desc {
	char *name;
	smc_key key;
};

/*
 * Key's name taken from:
 * https://github.com/kaiT2en/KaiT2en-Fedora/blob/main/apps/t2-smc-control/src/main.rs
 * https://github.com/acidanthera/VirtualSMC/tree/master/Docs/
 */

static const struct macsmc_sensor_desc temp_sensors[] = {
	{ "Ambient 2 Raw Temperature (TA1p)", SMC_KEY(TA1p) },
	{ "Ambient 3 Raw Temperature (TA2p)", SMC_KEY(TA2p) },
	{ "Ambient Cooked Temperature (TA0P)", SMC_KEY(TA0P) },
	{ "Ambient Cooked Temperature 1 (TA1P)", SMC_KEY(TA1P) },
	{ "Ambient Cooked Temperature 2 (TA2P)", SMC_KEY(TA2P) },
	{ "Ambient Raw Temperature (TA0p)", SMC_KEY(TA0p) },
	{ "Ambient Virtual Temperature (TA0V)", SMC_KEY(TA0V) },
	{ "Audio L Temperature (TaLC)", SMC_KEY(TaLC) },
	{ "Audio R Temperature (TaRC)", SMC_KEY(TaRC) },
	{ "Backlight Controller Proximity Cooked Temperature (Tb0P)", SMC_KEY(Tb0P) },
	{ "Backlight Controller Proximity Raw Temperature (Tb0p)", SMC_KEY(Tb0p) },
	{ "Battery Ambient Temperature (TB3T)", SMC_KEY(TB3T) },
	{ "Battery Temperature (TB0T)", SMC_KEY(TB0T) },
	{ "Battery Temperature, Same As TB0T (TBXT)", SMC_KEY(TBXT) },
	{ "Battery TS1 Temperature (TB1T)", SMC_KEY(TB1T) },
	{ "Battery TS2 Temperature (TB2T)", SMC_KEY(TB2T) },
	{ "Charger Proximity Temperature (TCHP)", SMC_KEY(TCHP) },
	{ "CPU 1 Heatsink Alt. Temperature (TCAH)", SMC_KEY(TCAH) },
	{ "CPU 1 Heatsink Temperature (TC0H)", SMC_KEY(TC0H) },
	{ "CPU 1 Package Temperature (TCAD)", SMC_KEY(TCAD) },
	{ "CPU 2 Diode Filtered Temperature (TC1F)", SMC_KEY(TC1F) },
	{ "CPU 2 Diode Virtual Temperature (TC1E)", SMC_KEY(TC1E) },
	{ "CPU 2 Heatsink Alt. Temperature (TCBH)", SMC_KEY(TCBH) },
	{ "CPU 2 Heatsink Temperature (TC1H)", SMC_KEY(TC1H) },
	{ "CPU 2 Package Alt. Temperature (TCBD)", SMC_KEY(TCBD) },
	{ "CPU 2 Package Temperature (TC1D)", SMC_KEY(TC1D) },
	{ "CPU 2 Proximity Raw Temperature (TC1p)", SMC_KEY(TC1p) },
	{ "CPU Core 0 Absolute Raw Temperature PECI (TC0c)", SMC_KEY(TC0c) },
	{ "CPU Core 1 Absolute Cooked Temperature PECI (TC1C)", SMC_KEY(TC1C) },
	{ "CPU Core 1 Absolute Raw Temperature PECI (TC1c)", SMC_KEY(TC1c) },
	{ "CPU Core 2 Absolute Cooked Temperature PECI (TC2C)", SMC_KEY(TC2C) },
	{ "CPU Core 2 Absolute Raw Temperature PECI (TC2c)", SMC_KEY(TC2c) },
	{ "CPU Core 3 Absolute Cooked Temperature PECI (TC3C)", SMC_KEY(TC3C) },
	{ "CPU Core 3 Absolute Raw Temperature PECI (TC3c)", SMC_KEY(TC3c) },
	{ "CPU Core 3 Temperature PECI (TC4C)", SMC_KEY(TC4C) },
	{ "CPU Core 4 Absolute Raw Temperature PECI (TC4c)", SMC_KEY(TC4c) },
	{ "CPU Core 5 Absolute Raw Temperature PECI (TC5c)", SMC_KEY(TC5c) },
	{ "CPU Core 6 Absolute Raw Temperature PECI (TC6c)", SMC_KEY(TC6c) },
	{ "CPU Core 7 Absolute Raw Temperature PECI (TC7c)", SMC_KEY(TC7c) },
	{ "CPU Core 8 Absolute Raw Temperature PECI (TC8c)", SMC_KEY(TC8c) },
	{ "CPU Core 9 Absolute Raw Temperature PECI (TC9c)", SMC_KEY(TC9c) },
	{ "CPU Core PECI Temperature (TC0C)", SMC_KEY(TC0C) },
	{ "CPU Die Raw Temperature (TC0d)", SMC_KEY(TC0d) },
	{ "CPU Die Temperature (TC0D)", SMC_KEY(TC0D) },
	{ "CPU eDRAM Hot Indicator PECI (TCMc)", SMC_KEY(TCMc) },
	{ "CPU Gfx Core Absolute Cooked Temperature PECI (TCGC)", SMC_KEY(TCGC) },
	{ "CPU Gfx Core Absolute Raw Temperature PECI (TCGc)", SMC_KEY(TCGc) },
	{ "CPU Heatpipe Temperature (Th0H)", SMC_KEY(Th0H) },
	{ "CPU Max Package Core Absolute Cooked Temperature PECI (TCXC)", SMC_KEY(TCXC) },
	{ "CPU Max Package Core Absolute Raw Temperature PECI (TCXc)", SMC_KEY(TCXc) },
	{ "CPU Max Package Core Relative Cooked Temperature PECI (TCXR)", SMC_KEY(TCXR) },
	{ "CPU Max Package Core Relative Raw Temperature PECI (TCXr)", SMC_KEY(TCXr) },
	{ "CPU PECI Die Filtered And Adjusted Temperature (TC0F)", SMC_KEY(TC0F) },
	{ "CPU PECI Die Filtered Temperature (TC0E)", SMC_KEY(TC0E) },
	{ "CPU PECI Die Temperature Adjustment (TC0G)", SMC_KEY(TC0G) },
	{ "CPU PECI Die Temperature Filter Coeff (TCFC)", SMC_KEY(TCFC) },
	{ "CPU PECI Die Temperature Max Error (TC0J)", SMC_KEY(TC0J) },
	{ "CPU PECI Die Temperature Trend (TC0T)", SMC_KEY(TC0T) },
	{ "CPU PECI Die Temperature Trend (TCTD)", SMC_KEY(TCTD) },
	{ "CPU Proximity Raw Temperature (TC0p)", SMC_KEY(TC0p) },
	{ "CPU Proximity Temperature (TC0P)", SMC_KEY(TC0P) },
	{ "CPU Relative To ProcHot 1 (TCAC)", SMC_KEY(TCAC) },
	{ "CPU Relative To ProcHot 2 (TCBC)", SMC_KEY(TCBC) },
	{ "CPU System Agent Core Absolute Cooked Temperature PECI (TCSC)", SMC_KEY(TCSC) },
	{ "CPU System Agent Core Absolute Raw Temperature PECI (TCSc)", SMC_KEY(TCSc) },
	{ "CPU System Agent Core Temperature PECI (TCSA)", SMC_KEY(TCSA) },
	{ "CPU VR Proximity Cooked Temperature (TC1P)", SMC_KEY(TC1P) },
	{ "DCIn Air Flow Adjusted Temperature (TaSP)", SMC_KEY(TaSP) },
	{ "DCIn Air Flow Temperature (Ta0P)", SMC_KEY(Ta0P) },
	{ "DIMM 0 PECI Cooked Temperature (TM0D)", SMC_KEY(TM0D) },
	{ "DIMM 0 Proximity Raw Temperature (TM0p)", SMC_KEY(TM0p) },
	{ "DIMM 1 PECI Cooked Temperature (TM1D)", SMC_KEY(TM1D) },
	{ "DIMM 1 Proximity Cooked Temperature (TM1P)", SMC_KEY(TM1P) },
	{ "DIMM 1 Proximity Raw Temperature (TM1p)", SMC_KEY(TM1p) },
	{ "DIMM 2 PECI Cooked Temperature (TM2D)", SMC_KEY(TM2D) },
	{ "DIMM 2 Proximity Cooked Temperature (TM2P)", SMC_KEY(TM2P) },
	{ "DIMM 2 Proximity Raw Temperature (TM2p)", SMC_KEY(TM2p) },
	{ "DIMM 3 PECI Cooked Temperature (TM3D)", SMC_KEY(TM3D) },
	{ "DIMM 3 Proximity Cooked Temperature (TM3P)", SMC_KEY(TM3P) },
	{ "DIMM 3 Proximity Raw Temperature (TM3p)", SMC_KEY(TM3p) },
	{ "DIMM Bandwidth Estimate (TMBS)", SMC_KEY(TMBS) },
	{ "DIMM Max Cooked Temperature (TMXD)", SMC_KEY(TMXD) },
	{ "DIMM Proximity Max Cooked Temperature (TMXP)", SMC_KEY(TMXP) },
	{ "DIMM Virtual Temperature (TM0V)", SMC_KEY(TM0V) },
	{ "DIMM Virtual Temperature For 2x DIMM Slot 1+2 Or 2+3 Config (TM2b)", SMC_KEY(TM2b) },
	{ "DIMM Virtual Temperature For 2x DIMM Slot 1+2+3+4 Config (TM1a)", SMC_KEY(TM1a) },
	{ "DIMM Virtual Temperature For 2x DIMM Slot 1+3 Config (TM2a)", SMC_KEY(TM2a) },
	{ "DIMM Virtual Temperature For 2x DIMM Slot 1+4 Or 2+4 Config (TM2c)", SMC_KEY(TM2c) },
	{ "DIMM Virtual Temperature For 2x DIMM Slot 3+4 Config (TM2d)", SMC_KEY(TM2d) },
	{ "DIMM Virtual Temperature For 3x DIMM Slot 1+2+3 Config (TM3d)", SMC_KEY(TM3d) },
	{ "DIMM Virtual Temperature For 3x DIMM Slot 1+2+4 Config (TM3c)", SMC_KEY(TM3c) },
	{ "DIMM Virtual Temperature For 3x DIMM Slot 1+3+4 Config (TM3b)", SMC_KEY(TM3b) },
	{ "DIMM Virtual Temperature For 3x DIMM Slot 2+3+4 Config (TM3a)", SMC_KEY(TM3a) },
	{ "DIMM Virtual Temperature For 4x DIMM Slot 1+2+3+4 Config (TM4a)", SMC_KEY(TM4a) },
	{ "Drive 0 Backup Virtual DRAM Temperature (TH0V)", SMC_KEY(TH0V) },
	{ "Drive 0 OOBv1 Mapped Temperature (TH0O)", SMC_KEY(TH0O) },
	{ "Drive 0 OOBv1 Raw Bit-Mapped Temperature (TH0o)", SMC_KEY(TH0o) },
	{ "Drive 0 OOBv3 Absolute Cooked Temperature A (TH0A)", SMC_KEY(TH0A) },
	{ "Drive 0 OOBv3 Absolute Cooked Temperature B (TH0B)", SMC_KEY(TH0B) },
	{ "Drive 0 OOBv3 Absolute Cooked Temperature C (TH0C)", SMC_KEY(TH0C) },
	{ "Drive 0 OOBv3 Absolute Raw Temperature A (TH0a)", SMC_KEY(TH0a) },
	{ "Drive 0 OOBv3 Absolute Raw Temperature B (TH0b)", SMC_KEY(TH0b) },
	{ "Drive 0 OOBv3 Absolute Raw Temperature C (TH0c)", SMC_KEY(TH0c) },
	{ "Drive 0 OOBv3 Raw Temperature Max (TH0x)", SMC_KEY(TH0x) },
	{ "Drive 0 OOBv3 Relative Cooked Temperature Max (TH0R)", SMC_KEY(TH0R) },
	{ "Drive 0 OOBv3 Relative Filtered Temperature Max (TH0F)", SMC_KEY(TH0F) },
	{ "Drive 0 Temperature Sensor Existence Used For TM0V (TH0e)", SMC_KEY(TH0e) },
	{ "Drive 1 OOBv1 Filtered Temperature (TH1P)", SMC_KEY(TH1P) },
	{ "Drive 1 OOBv1 Mapped Temperature (TH1O)", SMC_KEY(TH1O) },
	{ "Drive 1 OOBv1 Raw Bit-Mapped Temperature (TH1o)", SMC_KEY(TH1o) },
	{ "Drive 1 OOBv3 Absolute Cooked Temperature A (TH1A)", SMC_KEY(TH1A) },
	{ "Drive 1 OOBv3 Absolute Cooked Temperature B (TH1B)", SMC_KEY(TH1B) },
	{ "Drive 1 OOBv3 Absolute Cooked Temperature C (TH1C)", SMC_KEY(TH1C) },
	{ "Drive 1 OOBv3 Absolute Raw Temperature A (TH1a)", SMC_KEY(TH1a) },
	{ "Drive 1 OOBv3 Absolute Raw Temperature B (TH1b)", SMC_KEY(TH1b) },
	{ "Drive 1 OOBv3 Absolute Raw Temperature C (TH1c)", SMC_KEY(TH1c) },
	{ "Drive 1 OOBv3 Relative Cooked Temperature Max (TH1R)", SMC_KEY(TH1R) },
	{ "Drive 1 OOBv3 Relative Filtered Temperature Max (TH1F)", SMC_KEY(TH1F) },
	{ "EMC1428 Internal Diode Raw Temperature (Tm0p)", SMC_KEY(Tm0p) },
	{ "Expansion Slots Temperature (TS0C)", SMC_KEY(TS0C) },
	{ "Fan 0 Synthetic Temperature (TF0S)", SMC_KEY(TF0S) },
	{ "Fan Target Temperature (TTF0)", SMC_KEY(TTF0) },
	{ "Filtered Power Supply 1 Proximity Temperature (TpFP)", SMC_KEY(TpFP) },
	{ "Filtered Vent Proximity Temperature (TVFP)", SMC_KEY(TVFP) },
	{ "Fin Stack Temperature (THFH)", SMC_KEY(THFH) },
	{ "GFX Memory Temperature (TG0M)", SMC_KEY(TG0M) },
	{ "GPU 0 Die Cooked Temperature (TG0D)", SMC_KEY(TG0D) },
	{ "GPU 0 Die Filtered And Adjusted Temperature (TG0F)", SMC_KEY(TG0F) },
	{ "GPU 0 Die Filtered Temperature (TG0E)", SMC_KEY(TG0E) },
	{ "GPU 0 Die Raw Temperature (TG0d)", SMC_KEY(TG0d) },
	{ "GPU 0 Die Temperature Max Error (TG0J)", SMC_KEY(TG0J) },
	{ "GPU 0 Die Temperature Trend (TG0T)", SMC_KEY(TG0T) },
	{ "GPU 0 Proximity Cooked Temperature (TG0P)", SMC_KEY(TG0P) },
	{ "GPU 0 Proximity Raw Temperature (TG0p)", SMC_KEY(TG0p) },
	{ "GPU AMD Die Digital Temperature (TGDD)", SMC_KEY(TGDD) },
	{ "GPU B VR Proximity Cooked Temperature (TG5P)", SMC_KEY(TG5P) },
	{ "GPU B VRAM Proximity Cooked Temperature (TG4P)", SMC_KEY(TG4P) },
	{ "GPU Die Analog Temperature (TGDF)", SMC_KEY(TGDF) },
	{ "GPU Digital Die Filtered Temperature (TG1F)", SMC_KEY(TG1F) },
	{ "GPU Digital Die Raw Temperature Without Offset (TG1d)", SMC_KEY(TG1d) },
	{ "GPU Digital Die Temperature (TG1D)", SMC_KEY(TG1D) },
	{ "GPU FB VR Proximity 3 Cooked Temperature (TG3P)", SMC_KEY(TG3P) },
	{ "GPU FB VR Proximity 3 Raw Temperature (TG3p)", SMC_KEY(TG3p) },
	{ "GPU GDDR5 Proximity 1 Cooked Temperature (TG1P)", SMC_KEY(TG1P) },
	{ "GPU GDDR5 Proximity 1 Raw Temperature (TG1p)", SMC_KEY(TG1p) },
	{ "GPU VR Proximity 2 Cooked Temperature (TG2P)", SMC_KEY(TG2P) },
	{ "GPU VR Proximity 2 Raw Temperature (TG2p)", SMC_KEY(TG2p) },
	{ "GPU VR Temperature (TGVP)", SMC_KEY(TGVP) },
	{ "HDD Bay 3 Temperature (TH2P)", SMC_KEY(TH2P) },
	{ "HDD Bay 4 Temperature (TH3P)", SMC_KEY(TH3P) },
	{ "HDD Proximity Temperature (TH0P)", SMC_KEY(TH0P) },
	{ "Heat Pipe Temperature (TG0H)", SMC_KEY(TG0H) },
	{ "Heat Pipe Temperature 2 (TG1H)", SMC_KEY(TG1H) },
	{ "Heat Spreader Proximity Temperature (Th0P)", SMC_KEY(Th0P) },
	{ "High Speed I/O Proximity 0 Temperature (TI0P)", SMC_KEY(TI0P) },
	{ "High Speed I/O Proximity 1 Temperature (TI1P)", SMC_KEY(TI1P) },
	{ "LCD Filtered Center Front-Of-Screen Temperature Feeding TL1V (TL1v)", SMC_KEY(TL1v) },
	{ "LCD Filtered Center Front-Of-Screen Temperature For Color Compensation (TL1V)", SMC_KEY(TL1V) },
	{ "LCD Filtered Front-Of-Screen Hotspot Virtual Temperature (TL0V)", SMC_KEY(TL0V) },
	{ "LCD IR Target Hotspot Calculated Temperature (TL2p)", SMC_KEY(TL2p) },
	{ "LCD IR Temperature Sensor Die Raw Temperature (TL3p)", SMC_KEY(TL3p) },
	{ "LCD TCON Proximity Cooked Temperature (TL1P)", SMC_KEY(TL1P) },
	{ "LCD TCON Proximity Raw Temperature (TL1p)", SMC_KEY(TL1p) },
	{ "LCD Unfiltered Front-Of-Screen Cooked Temperature (TL0P)", SMC_KEY(TL0P) },
	{ "LCD Unfiltered Front-Of-Screen Raw Temperature (TL0p)", SMC_KEY(TL0p) },
	{ "Left Fin Stack Temperature (Th2H)", SMC_KEY(Th2H) },
	{ "Max PECI Reported Temperature (TCMX)", SMC_KEY(TCMX) },
	{ "MCH Diode Temperature (TN0C)", SMC_KEY(TN0C) },
	{ "MCH Heatsink Temperature (TN0H)", SMC_KEY(TN0H) },
	{ "MCP Die 2 Temperature (TN1D)", SMC_KEY(TN1D) },
	{ "MCP Die Temperature (TN0D)", SMC_KEY(TN0D) },
	{ "MCP Proximity Temperature (TN0P)", SMC_KEY(TN0P) },
	{ "Mem Bank B1 Temperature (TM8P)", SMC_KEY(TM8P) },
	{ "Mem Bank B2 Temperature (TM9P)", SMC_KEY(TM9P) },
	{ "Mem Module A2 Temperature (TM1S)", SMC_KEY(TM1S) },
	{ "Mem Module A3 Temperature (TM2S)", SMC_KEY(TM2S) },
	{ "Mem Module A4 Temperature (TM3S)", SMC_KEY(TM3S) },
	{ "Mem Module B1 Temperature (TM8S)", SMC_KEY(TM8S) },
	{ "Mem Module B2 Temperature (TM9S)", SMC_KEY(TM9S) },
	{ "Memory Controller/Integrated Graphics PECI Temperature (TMCD)", SMC_KEY(TMCD) },
	{ "Memory Proximity Temperature (TM0P)", SMC_KEY(TM0P) },
	{ "MLB Bottom Side Temperature (Tm1P)", SMC_KEY(Tm1P) },
	{ "MLB Proximity 0 Cooked Temperature (Tm0P)", SMC_KEY(Tm0P) },
	{ "MLB Proximity 1 Raw Temperature (Tm1p)", SMC_KEY(Tm1p) },
	{ "MLB Proximity 2 Cooked Temperature (Tm2P)", SMC_KEY(Tm2P) },
	{ "MLB Proximity 2 Raw Temperature (Tm2p)", SMC_KEY(Tm2p) },
	{ "MLB Proximity Temperature (TMLB)", SMC_KEY(TMLB) },
	{ "MLB Temperature (Tp0P)", SMC_KEY(Tp0P) },
	{ "NAND Temperature (Th0N)", SMC_KEY(Th0N) },
	{ "Northbridge 2 Temperature (TN1P)", SMC_KEY(TN1P) },
	{ "Optical Drive Temperature (TO0P)", SMC_KEY(TO0P) },
	{ "Palm Rest Temperature (Ts0P)", SMC_KEY(Ts0P) },
	{ "Palm Rest Temperature 2 (Ts1P)", SMC_KEY(Ts1P) },
	{ "PCH Die Cooked Temperature (TPCD)", SMC_KEY(TPCD) },
	{ "PCH Digital Die Raw Temperature (TPCd)", SMC_KEY(TPCd) },
	{ "PCH Diode Temperature (TP0D)", SMC_KEY(TP0D) },
	{ "PCH Proximity Temperature (TP0P)", SMC_KEY(TP0P) },
	{ "PCI Slot 1 Pos 1 Temperature (TA0S)", SMC_KEY(TA0S) },
	{ "PCI Slot 1 Pos 2 Temperature (TA1S)", SMC_KEY(TA1S) },
	{ "PCI Slot 2 Pos 1 Temperature (TA2S)", SMC_KEY(TA2S) },
	{ "PCI Slot 2 Pos 2 Temperature (TA3S)", SMC_KEY(TA3S) },
	{ "Power Supply 1 Alt. Temperature (Tp0C)", SMC_KEY(Tp0C) },
	{ "Power Supply 2 Alt. Temperature (Tp1C)", SMC_KEY(Tp1C) },
	{ "Power Supply 3 Temperature (Tp2P)", SMC_KEY(Tp2P) },
	{ "Power Supply 4 Temperature (Tp3P)", SMC_KEY(Tp3P) },
	{ "Power Supply 5 Temperature (Tp4P)", SMC_KEY(Tp4P) },
	{ "Power Supply 6 Temperature (Tp5P)", SMC_KEY(Tp5P) },
	{ "Power Supply T2 Secondary Heatsink Cooked Temperature (Tp2H)", SMC_KEY(Tp2H) },
	{ "Power Supply T2 Secondary Heatsink Filtered Temperature (Tp2F)", SMC_KEY(Tp2F) },
	{ "Power Supply T2 Secondary Heatsink Raw Temperature (Tp2h)", SMC_KEY(Tp2h) },
	{ "PSU Secondary Heatsink Diode Cooked Temperature (Tp0T)", SMC_KEY(Tp0T) },
	{ "Right Fin Stack Temperature (Th1H)", SMC_KEY(Th1H) },
	{ "RIO Temperature (Th0R)", SMC_KEY(Th0R) },
	{ "S2 Camera Proximity Temperature (TS2P)", SMC_KEY(TS2P) },
	{ "Secondary Power Supply Proximity Temperature (Tp1P)", SMC_KEY(Tp1P) },
	{ "Skin 0 Raw Temperature (TS0p)", SMC_KEY(TS0p) },
	{ "Skin 0 Virtual Temperature (TS0V)", SMC_KEY(TS0V) },
	{ "Speaker Proximity 2 Temperature (TS1P)", SMC_KEY(TS1P) },
	{ "Speaker Proximity Temperature (TS0P)", SMC_KEY(TS0P) },
	{ "Speaker Remote 2 Temperature (TS1D)", SMC_KEY(TS1D) },
	{ "Speaker Remote Temperature (TS0D)", SMC_KEY(TS0D) },
	{ "SSD 0 OOBv3 Cooked Temperature Max (TH0X)", SMC_KEY(TH0X) },
	{ "SSD 1 OOBv3 Cooked Temperature Max (TH1X)", SMC_KEY(TH1X) },
	{ "Synthetic Bottom Skin Temperature (Ts0S)", SMC_KEY(Ts0S) },
	{ "Synthetic DIMM Estimate (TM0S)", SMC_KEY(TM0S) },
	{ "Synthetic Top Skin Temperature (Ts1S)", SMC_KEY(Ts1S) },
	{ "TBT Proximity Raw Temperature (TI0p)", SMC_KEY(TI0p) },
	{ "TBT Thermal Diode Raw Temperature (Te0t)", SMC_KEY(Te0t) },
	{ "TBT Thermal Diode Temperature (Te0T)", SMC_KEY(Te0T) },
	{ "Thermal Trap Hysteresis (TsTH)", SMC_KEY(TsTH) },
	{ "Thermal Trap Threshold (TsTP)", SMC_KEY(TsTP) },
	{ "Thermal Warning Hysteresis (TsHS)", SMC_KEY(TsHS) },
	{ "Thermal Warning Threshold (TsWS)", SMC_KEY(TsWS) },
	{ "Thunderbolt Die Temperature (THSP)", SMC_KEY(THSP) },
	{ "Thunderbolt L Temperature (TTLD)", SMC_KEY(TTLD) },
	{ "Thunderbolt Proximity Cooked Temperature (TI0T)", SMC_KEY(TI0T) },
	{ "Thunderbolt R Temperature (TTRD)", SMC_KEY(TTRD) },
	{ "Touchpad Temperature (Ts2S)", SMC_KEY(Ts2S) },
	{ "Vent Proximity Temperature (TV0P)", SMC_KEY(TV0P) },
	{ "WiFi Proximity Raw Temperature (TW0p)", SMC_KEY(TW0p) },
	{ "Wireless Proximity Temperature (TW0P)", SMC_KEY(TW0P) },
};

static const struct macsmc_sensor_desc power_sensors[] = {
	{ "12 V Rail (Pp0R)", SMC_KEY(Pp0R) },
	{ "2nd GPU Plimit (MSTi)", SMC_KEY(MSTi) },
	{ "3.3V In S0 Power (PR3C)", SMC_KEY(PR3C) },
	{ "3.3V S0 Power (PR0C)", SMC_KEY(PR0C) },
	{ "3.3V S3 Power (PR1C)", SMC_KEY(PR1C) },
	{ "3.3V S5 Power (PR5C)", SMC_KEY(PR5C) },
	{ "5V Lowside Power (P50R)", SMC_KEY(P50R) },
	{ "A8 Load-Side Rail (PA8C)", SMC_KEY(PA8C) },
	{ "Audio ScratchPad Register (MST1)", SMC_KEY(MST1) },
	{ "Audio ScratchPad2 Register (MST2)", SMC_KEY(MST2) },
	{ "Average Main Chipset Power (Pc0R)", SMC_KEY(Pc0R) },
	{ "Battery Rail (PB0R)", SMC_KEY(PB0R) },
	{ "Battery Rail (PBLC)", SMC_KEY(PBLC) },
	{ "BL Rail 12 V (PBLR)", SMC_KEY(PBLR) },
	{ "Chipset IMon Power (PCS1)", SMC_KEY(PCS1) },
	{ "Chipset Lowside Power (PCS0)", SMC_KEY(PCS0) },
	{ "CPU 1V5 Power (PD1R)", SMC_KEY(PD1R) },
	{ "CPU Core (PCAC)", SMC_KEY(PCAC) },
	{ "CPU Core 1 (PC0C)", SMC_KEY(PC0C) },
	{ "CPU Core 2 (PC1C)", SMC_KEY(PC1C) },
	{ "CPU Core 3 (PC2C)", SMC_KEY(PC2C) },
	{ "CPU Core 4 (PC3C)", SMC_KEY(PC3C) },
	{ "CPU Core 5 (PC4C)", SMC_KEY(PC4C) },
	{ "CPU Core 6 (PC5C)", SMC_KEY(PC5C) },
	{ "CPU Core 7 (PC6C)", SMC_KEY(PC6C) },
	{ "CPU Core 8 (PC7C)", SMC_KEY(PC7C) },
	{ "CPU Core IMON (PCAM)", SMC_KEY(PCAM) },
	{ "CPU Cores (PCPC)", SMC_KEY(PCPC) },
	{ "CPU DRAM (PCPD)", SMC_KEY(PCPD) },
	{ "CPU GFX (PCPG)", SMC_KEY(PCPG) },
	{ "CPU High-Side Average (PC0R)", SMC_KEY(PC0R) },
	{ "CPU I/O High-Side (PC0I)", SMC_KEY(PC0I) },
	{ "CPU I/O High-Side 2 (PC0M)", SMC_KEY(PC0M) },
	{ "CPU I/O Load-Side Rail (PCIC)", SMC_KEY(PCIC) },
	{ "CPU Integrated GPU (PC0G)", SMC_KEY(PC0G) },
	{ "CPU Package Total PECI (PCPT)", SMC_KEY(PCPT) },
	{ "CPU Package Total Power SMC (PCPR)", SMC_KEY(PCPR) },
	{ "CPU Package Total Power SMC Load Sensors (PCLT)", SMC_KEY(PCLT) },
	{ "CPU Plimit (MSTc)", SMC_KEY(MSTc) },
	{ "CPU Rail (PC1R)", SMC_KEY(PC1R) },
	{ "CPU Raw Package (PC0c)", SMC_KEY(PC0c) },
	{ "CPU S0 Rail (PC5R)", SMC_KEY(PC5R) },
	{ "CPU System Agent (PC0S)", SMC_KEY(PC0S) },
	{ "CPU Total (PCPL)", SMC_KEY(PCPL) },
	{ "CPU Total (PCTR)", SMC_KEY(PCTR) },
	{ "CPU VccEDRAM (PCEC)", SMC_KEY(PCEC) },
	{ "CPU VCCSA (PCSC)", SMC_KEY(PCSC) },
	{ "CT Load-Side Rail (PCTC)", SMC_KEY(PCTC) },
	{ "DC-In MLB S5 Rail (PD5R)", SMC_KEY(PD5R) },
	{ "DC-In MLB Total (PDMR)", SMC_KEY(PDMR) },
	{ "DDR (PM1C)", SMC_KEY(PM1C) },
	{ "DDR Power 1.8V CPU (PM2C)", SMC_KEY(PM2C) },
	{ "DDR S3 Power (PM3C)", SMC_KEY(PM3C) },
	{ "DIMM Load-Side Power Per Fan RPM Squared (PM0f)", SMC_KEY(PM0f) },
	{ "Discrete BMON On Battery Power (PB1R)", SMC_KEY(PB1R) },
	{ "Drive 0 (PH0R)", SMC_KEY(PH0R) },
	{ "Drive 0 Filtered Total Power (PH0F)", SMC_KEY(PH0F) },
	{ "Drive 1 (PH1R)", SMC_KEY(PH1R) },
	{ "Drive 1 Filtered Power (PH1F)", SMC_KEY(PH1F) },
	{ "Drive 2 Power (PH2R)", SMC_KEY(PH2R) },
	{ "External GPU 1.05 V (PG2C)", SMC_KEY(PG2C) },
	{ "External GPU 1.35 V (PG3C)", SMC_KEY(PG3C) },
	{ "External GPU 1.8 V (PG1C)", SMC_KEY(PG1C) },
	{ "External GPU Rail 4 (PG4C)", SMC_KEY(PG4C) },
	{ "F3 Load-Side Rail (PF3C)", SMC_KEY(PF3C) },
	{ "Forced Idle Limit (MSTf)", SMC_KEY(MSTf) },
	{ "GFX B Total Power (PG1R)", SMC_KEY(PG1R) },
	{ "GPU (PG0C)", SMC_KEY(PG0C) },
	{ "GPU 0 Rail (PG0R)", SMC_KEY(PG0R) },
	{ "GPU A VDDCI Low Side Power (PG0S)", SMC_KEY(PG0S) },
	{ "GPU Aux Input-Side Power (PG1A)", SMC_KEY(PG1A) },
	{ "GPU Aux Load-Side Power (PG0A)", SMC_KEY(PG0A) },
	{ "GPU B VDDCI Low Side Power (PG1S)", SMC_KEY(PG1S) },
	{ "GPU Frame Buffer Input-Side Power (PG0F)", SMC_KEY(PG0F) },
	{ "GPU Frame Buffer Input-Side Power (PG1F)", SMC_KEY(PG1F) },
	{ "GPU Mem Ctl Load-Side Power (PG0M)", SMC_KEY(PG0M) },
	{ "GPU Package Total Power SMC (PGPR)", SMC_KEY(PGPR) },
	{ "GPU Plimit (MSTg)", SMC_KEY(MSTg) },
	{ "GPU Total (PGTR)", SMC_KEY(PGTR) },
	{ "GPU Uncore High-Side Power (PG0U)", SMC_KEY(PG0U) },
	{ "GPU VDDCI Load-Side Power (PG0I)", SMC_KEY(PG0I) },
	{ "GPU VRAM Load-Side Power (PG0V)", SMC_KEY(PG0V) },
	{ "Heatpipe (PHPC)", SMC_KEY(PHPC) },
	{ "HP Power Target (PTHC)", SMC_KEY(PTHC) },
	{ "HP Power Throttle (MSLT)", SMC_KEY(MSLT) },
	{ "I/O Board Total Power (PI0R)", SMC_KEY(PI0R) },
	{ "Intel GPU IMON (PCGC)", SMC_KEY(PCGC) },
	{ "Intel GPU IMON 2 (PCGM)", SMC_KEY(PCGM) },
	{ "LCD Panel (PLDC)", SMC_KEY(PLDC) },
	{ "Left Thunderbolt/I/O Rail (POLR)", SMC_KEY(POLR) },
	{ "Left USB-C Load-Side Rail (PULC)", SMC_KEY(PULC) },
	{ "Main 3.3 V Rail (PH02)", SMC_KEY(PH02) },
	{ "Main 5 V Rail (PH05)", SMC_KEY(PH05) },
	{ "MC Load-Side Rail (PMCC)", SMC_KEY(PMCC) },
	{ "MCH (PN0C)", SMC_KEY(PN0C) },
	{ "MCP Memory Power (PN1C)", SMC_KEY(PN1C) },
	{ "MCP VCore Power (PN0R)", SMC_KEY(PN0R) },
	{ "MEM Plimit (MSTm)", SMC_KEY(MSTm) },
	{ "Memory Average (PM0C)", SMC_KEY(PM0C) },
	{ "Memory Power (PZDM)", SMC_KEY(PZDM) },
	{ "Memory Rail (PM0R)", SMC_KEY(PM0R) },
	{ "Memory Total Power (PMTR)", SMC_KEY(PMTR) },
	{ "Misc. Rail (PO0R)", SMC_KEY(PO0R) },
	{ "ODD Power (PZOD)", SMC_KEY(PZOD) },
	{ "Other 3.3V High Side Power (PO3R)", SMC_KEY(PO3R) },
	{ "Other 5 V High-Side (PO5R)", SMC_KEY(PO5R) },
	{ "PBus (PP0R)", SMC_KEY(PP0R) },
	{ "PBus Battery Discharge (PPBR)", SMC_KEY(PPBR) },
	{ "PCH Core Power (PSBC)", SMC_KEY(PSBC) },
	{ "PCH Rail (PN1R)", SMC_KEY(PN1R) },
	{ "Power Supply Filter Coefficient (PD0K)", SMC_KEY(PD0K) },
	{ "Power Supply Filtered Power (PD0E)", SMC_KEY(PD0E) },
	{ "Power Supply Max Error Filtered Output (PD0J)", SMC_KEY(PD0J) },
	{ "Power Supply Power Filtered Then Adjusted (PD0F)", SMC_KEY(PD0F) },
	{ "Power Supply Power Trend (PD0D)", SMC_KEY(PD0D) },
	{ "Power Zone AP (PZAP)", SMC_KEY(PZAP) },
	{ "Power Zone Backlight (PZBL)", SMC_KEY(PZBL) },
	{ "Power Zone Storage (PZHD)", SMC_KEY(PZHD) },
	{ "Right Thunderbolt/I/O Rail (PORR)", SMC_KEY(PORR) },
	{ "Right USB-C Load-Side Rail (PURC)", SMC_KEY(PURC) },
	{ "S0 5.0V Power (PR0R)", SMC_KEY(PR0R) },
	{ "S1 Load-Side Rail (PS1C)", SMC_KEY(PS1C) },
	{ "S2 Camera Power (PCMC)", SMC_KEY(PCMC) },
	{ "S2 Camera Power (PS2C)", SMC_KEY(PS2C) },
	{ "S3 3.30V Power (PR1R)", SMC_KEY(PR1R) },
	{ "S3 5.0V Power (PR2R)", SMC_KEY(PR2R) },
	{ "S5 3.30V Power (PR3R)", SMC_KEY(PR3R) },
	{ "SL Rail 3.3 V (PSLC)", SMC_KEY(PSLC) },
	{ "SSD 3.3 V Load-Side Rail 0 (PH0C)", SMC_KEY(PH0C) },
	{ "SSD 3.3 V Load-Side Rail 1 (PH1C)", SMC_KEY(PH1C) },
	{ "SSD Power (PHDC)", SMC_KEY(PHDC) },
	{ "SSD Power (PSDC)", SMC_KEY(PSDC) },
	{ "System Total, 1 s Delayed (PSTR)", SMC_KEY(PSTR) },
	{ "T101 Accuator Power (PTPC)", SMC_KEY(PTPC) },
	{ "T25 Track Pad Power (PTPR)", SMC_KEY(PTPR) },
	{ "Thunderbolt Power (PHSC)", SMC_KEY(PHSC) },
	{ "Thunderbolt Power (PHSP)", SMC_KEY(PHSP) },
	{ "WiFi (PAPC)", SMC_KEY(PAPC) },
	{ "Wifi Load-Side Power (PW0R)", SMC_KEY(PW0R) },
	{ "Heatpipe Average (PZ0G)", SMC_KEY(PZ0G) },
	{ "Heatpipe Average Target (PZ0E)", SMC_KEY(PZ0E) },
	{ "Heatpipe Filtered (PZ0F)", SMC_KEY(PZ0F) },
	{ "Heatpipe Throttle (PZ0T)", SMC_KEY(PZ0T) },
	{ "CPU Average (PZ1G)", SMC_KEY(PZ1G) },
	{ "CPU Filtered (PZ1F)", SMC_KEY(PZ1F) },
	{ "CPU Target (PZ1E)", SMC_KEY(PZ1E) },
	{ "CPU Throttle (PZ1T)", SMC_KEY(PZ1T) },
	{ "dGPU Average (PZ2G)", SMC_KEY(PZ2G) },
	{ "dGPU Filtered (PZ2F)", SMC_KEY(PZ2F) },
	{ "dGPU Target (PZ2E)", SMC_KEY(PZ2E) },
	{ "dGPU Throttle (PZ2T)", SMC_KEY(PZ2T) },
	{ "Batt aux Average (PZ3G)", SMC_KEY(PZ3G) },
	{ "Batt aux Filtered (PZ3F)", SMC_KEY(PZ3F) },
	{ "Batt aux Target (PZ3E)", SMC_KEY(PZ3E) },
	{ "Batt aux Throttle (PZ3T)", SMC_KEY(PZ3T) },
	{ "Battery Average (PZ4G)", SMC_KEY(PZ4G) },
	{ "Battery Filtered (PZ4F)", SMC_KEY(PZ4F) },
	{ "Battery Target (PZ4E)", SMC_KEY(PZ4E) },
	{ "Battery Throttle (PZ4T)", SMC_KEY(PZ4T) },
	{ "Zone 5 Abstract Throttle (PZ5T)", SMC_KEY(PZ5T) },
	{ "Zone 5 Average Power (PZ5G)", SMC_KEY(PZ5G) },
	{ "Zone 5 Filtered Power (PZ5F)", SMC_KEY(PZ5F) },
	{ "Zone 5 Target Power (PZ5E)", SMC_KEY(PZ5E) },
};

static const struct macsmc_sensor_desc current_sensors[] = {
	{ "3.3V S0 Current (IR0C)", SMC_KEY(IR0C) },
	{ "3.3V S0 Current (IR3C)", SMC_KEY(IR3C) },
	{ "5.0V S0 Current (IR5C)", SMC_KEY(IR5C) },
	{ "5V Lowside Current (I50R)", SMC_KEY(I50R) },
	{ "Airport Current (IZOP)", SMC_KEY(IZOP) },
	{ "Backlight Current (IBLC)", SMC_KEY(IBLC) },
	{ "Battery Current (IBAC)", SMC_KEY(IBAC) },
	{ "Battery Current Discrete (IP0R)", SMC_KEY(IP0R) },
	{ "Battery Discharge Current MLB (IB0R)", SMC_KEY(IB0R) },
	{ "Camera S2 Current (ICMC)", SMC_KEY(ICMC) },
	{ "CPU 1V5 Current (ID1R)", SMC_KEY(ID1R) },
	{ "CPU And MEM Current (IM0C)", SMC_KEY(IM0C) },
	{ "CPU AXG Low-Side Current (IC0G)", SMC_KEY(IC0G) },
	{ "CPU Core Current (IC0C)", SMC_KEY(IC0C) },
	{ "CPU Core Current (ICS0)", SMC_KEY(ICS0) },
	{ "CPU Current From CPU IMON (ICS1)", SMC_KEY(ICS1) },
	{ "CPU DDR Current (IC3C)", SMC_KEY(IC3C) },
	{ "CPU DDR Current (IM1C)", SMC_KEY(IM1C) },
	{ "CPU High Side Current (IC0R)", SMC_KEY(IC0R) },
	{ "CPU High Side Current From EMC1704 (IC1R)", SMC_KEY(IC1R) },
	{ "CPU I/O High-Side Current (IC0I)", SMC_KEY(IC0I) },
	{ "CPU Mem Low-Side Current (IC0M)", SMC_KEY(IC0M) },
	{ "CPU Riser 12V High Side Current (ICTR)", SMC_KEY(ICTR) },
	{ "CPU VCCIO Current (IC1C)", SMC_KEY(IC1C) },
	{ "CPU VCCSA Current (IC2C)", SMC_KEY(IC2C) },
	{ "CPU VSA Low Side Current (IC0S)", SMC_KEY(IC0S) },
	{ "CSreg Current (Ic0R)", SMC_KEY(Ic0R) },
	{ "DDR S3 Current (IM3C)", SMC_KEY(IM3C) },
	{ "DIMM Current (IZDM)", SMC_KEY(IZDM) },
	{ "DIMM Load-Side Current (IM0R)", SMC_KEY(IM0R) },
	{ "Discrete Battery Current (IPB1)", SMC_KEY(IPB1) },
	{ "Drive 0 12V Load-Side Current (IH02)", SMC_KEY(IH02) },
	{ "Drive 0 5V Load-Side Current (IH05)", SMC_KEY(IH05) },
	{ "Drive 1 3.3V Load-Side Current (IH1R)", SMC_KEY(IH1R) },
	{ "Ext GPU 1.05V Current (IG2C)", SMC_KEY(IG2C) },
	{ "Ext GPU 1.35V Current (IG3C)", SMC_KEY(IG3C) },
	{ "Ext GPU High Side Current (IG0R)", SMC_KEY(IG0R) },
	{ "GFX B Riser 12V High Side Current (IG1R)", SMC_KEY(IG1R) },
	{ "GFX Vcore Current (IN0R)", SMC_KEY(IN0R) },
	{ "GPU A VDDCI Low Side Current (IG0S)", SMC_KEY(IG0S) },
	{ "GPU Aux Input-Side Current (IG1A)", SMC_KEY(IG1A) },
	{ "GPU B VDDCI Low Side Current (IG1S)", SMC_KEY(IG1S) },
	{ "GPU Core Current (IG0C)", SMC_KEY(IG0C) },
	{ "GPU Core Input-Side Current (IG1C)", SMC_KEY(IG1C) },
	{ "GPU FB High-Side Current (IG0F)", SMC_KEY(IG0F) },
	{ "GPU FB Input-Side Current (IG1F)", SMC_KEY(IG1F) },
	{ "GPU Uncore High-Side Current (IG0U)", SMC_KEY(IG0U) },
	{ "GPU VDDCI Load-Side Current (IG0I)", SMC_KEY(IG0I) },
	{ "HDD Current (IZHD)", SMC_KEY(IZHD) },
	{ "I/O Board 12V High Side Current (II0R)", SMC_KEY(II0R) },
	{ "Internal GFX Core Current (IN0C)", SMC_KEY(IN0C) },
	{ "LCD Backlight Current (IZBL)", SMC_KEY(IZBL) },
	{ "LCD Panel Current (ILDC)", SMC_KEY(ILDC) },
	{ "LPDDR Current 1.8V (IM2C)", SMC_KEY(IM2C) },
	{ "MCP DDR Current (IN1C)", SMC_KEY(IN1C) },
	{ "Memory 1V5 High Side Current (IMTR)", SMC_KEY(IMTR) },
	{ "ODD Current (IODC)", SMC_KEY(IODC) },
	{ "ODD Current (IZOD)", SMC_KEY(IZOD) },
	{ "Other 3.3V High Side Current (IO3R)", SMC_KEY(IO3R) },
	{ "Other 5V High Side Current (IO5R)", SMC_KEY(IO5R) },
	{ "Other High Side Current (IO0R)", SMC_KEY(IO0R) },
	{ "Other High Side Current (IR1C)", SMC_KEY(IR1C) },
	{ "PBus On Battery Current (IPBR)", SMC_KEY(IPBR) },
	{ "PCH 1V05 Current (IN1R)", SMC_KEY(IN1R) },
	{ "PCH Core Current (ISBC)", SMC_KEY(ISBC) },
	{ "PCH/GPU/TBT 1.05V High-Side Current (IR1R)", SMC_KEY(IR1R) },
	{ "Power Supply 12V Current Dynamic Filter Coeff (ID2L)", SMC_KEY(ID2L) },
	{ "Power Supply 12V Current Filter Input (ID2I)", SMC_KEY(ID2I) },
	{ "Power Supply 12V Current Filtered (ID2E)", SMC_KEY(ID2E) },
	{ "Power Supply 12V Current Filtered And Adjusted (ID2F)", SMC_KEY(ID2F) },
	{ "Power Supply 12V Current Max Error (ID2J)", SMC_KEY(ID2J) },
	{ "Power Supply 12V Current Trend (ID2T)", SMC_KEY(ID2T) },
	{ "S2 Camera Current (IS2C)", SMC_KEY(IS2C) },
	{ "S3 5.0V Current (IR2C)", SMC_KEY(IR2C) },
	{ "SSD 3V3 Low Side Current (IH0R)", SMC_KEY(IH0R) },
	{ "SSD Current (IHDC)", SMC_KEY(IHDC) },
	{ "SSD Current (ISDC)", SMC_KEY(ISDC) },
	{ "Thunderbolt Current (IHSC)", SMC_KEY(IHSC) },
	{ "Thunderbolt Current (IHSP)", SMC_KEY(IHSP) },
	{ "Trackpad Actuator Current (ITPC)", SMC_KEY(ITPC) },
	{ "Wifi Load-Side Current (IW0R)", SMC_KEY(IW0R) },
	{ "WLAN Current (IZAP)", SMC_KEY(IZAP) },
	{ "WLAN Lowside Current (IAPC)", SMC_KEY(IAPC) },
};

static const struct macsmc_sensor_desc voltage_sensors[] = {
	{ "1.05V To CPU/PCH Lowside Voltage (VC1C)", SMC_KEY(VC1C) },
	{ "1.2V To CPU/MEM Lowside Voltage (VM0C)", SMC_KEY(VM0C) },
	{ "1.8V S3 Lowside Voltage (VM1C)", SMC_KEY(VM1C) },
	{ "3.3V Load-Side Voltage (VR3R)", SMC_KEY(VR3R) },
	{ "5V Lowside Voltage (V50R)", SMC_KEY(V50R) },
	{ "ACC Voltage (VACC)", SMC_KEY(VACC) },
	{ "ACDC Cooked Voltage (Vp0C)", SMC_KEY(Vp0C) },
	{ "Backlight Output Voltage (VBLC)", SMC_KEY(VBLC) },
	{ "Backlight Voltage (VZBL)", SMC_KEY(VZBL) },
	{ "CPU AXG Low-Side Voltage (VC0G)", SMC_KEY(VC0G) },
	{ "CPU Core Voltage (VC0C)", SMC_KEY(VC0C) },
	{ "CPU Core Voltage (VCS0)", SMC_KEY(VCS0) },
	{ "CPU High Side Voltage From EMC1704 (VC1R)", SMC_KEY(VC1R) },
	{ "CPU Mem Load-Side Voltage (VC0M)", SMC_KEY(VC0M) },
	{ "CPU Ripple Voltage (VCRC)", SMC_KEY(VCRC) },
	{ "CPU Ripple Voltage (VCRP)", SMC_KEY(VCRP) },
	{ "CPU VCCSA Voltage (VC2C)", SMC_KEY(VC2C) },
	{ "CPU VSA Low Side Voltage (VC0S)", SMC_KEY(VC0S) },
	{ "DIMM Load-Side Voltage (VM0R)", SMC_KEY(VM0R) },
	{ "Drive 0 5V Load-Side Voltage (VH05)", SMC_KEY(VH05) },
	{ "FIVR CPU Supply Voltage (VCFR)", SMC_KEY(VCFR) },
	{ "GFX Vcore Voltage (VN0R)", SMC_KEY(VN0R) },
	{ "GPU A VDDCI Low Side Voltage (VG0S)", SMC_KEY(VG0S) },
	{ "GPU B VDDCI Low Side Voltage (VG1S)", SMC_KEY(VG1S) },
	{ "GPU Core Input-Side Voltage (VG1C)", SMC_KEY(VG1C) },
	{ "GPU Core Voltage (VG0C)", SMC_KEY(VG0C) },
	{ "GPU Frame Buffer Input-Side Voltage (VG0F)", SMC_KEY(VG0F) },
	{ "GPU Frame Buffer Input-Side Voltage (VG1F)", SMC_KEY(VG1F) },
	{ "GPU Uncore High-Side Voltage (VG0U)", SMC_KEY(VG0U) },
	{ "GPU VDDCI Load-Side Voltage (VG0I)", SMC_KEY(VG0I) },
	{ "LCD IR Temp Sensor Voltage Raw Register (VL3t)", SMC_KEY(VL3t) },
	{ "MCP Core Voltage (VN0C)", SMC_KEY(VN0C) },
	{ "MCP Memory Voltage (VN1C)", SMC_KEY(VN1C) },
	{ "Memory Voltage (VZDM)", SMC_KEY(VZDM) },
	{ "ODD Voltage (VODC)", SMC_KEY(VODC) },
	{ "ODD Voltage (VZOD)", SMC_KEY(VZOD) },
	{ "PBus Rail Voltage (VP0R)", SMC_KEY(VP0R) },
	{ "PCH Low-Side Voltage (VN1R)", SMC_KEY(VN1R) },
	{ "PCH/GPU/TBT Load-Side Voltage (VR1R)", SMC_KEY(VR1R) },
	{ "PSU Cooked Temperature As Voltage (VP0T)", SMC_KEY(VP0T) },
	{ "SD Card Voltage (VSDC)", SMC_KEY(VSDC) },
	{ "SSD 3.3V Low-Side Voltage (VH1R)", SMC_KEY(VH1R) },
	{ "SSD 3V3 Low Side Voltage (VH0R)", SMC_KEY(VH0R) },
	{ "SSD Voltage (VZHD)", SMC_KEY(VZHD) },
	{ "T101 Accuator Voltage (VTPC)", SMC_KEY(VTPC) },
	{ "WIFI Voltage (VZAP)", SMC_KEY(VZAP) },
	{ "Wireless Voltage (VAPC)", SMC_KEY(VAPC) },
};

static int macsmc_hwmon_create_sensor_acpi(struct apple_smc *smc,
		struct macsmc_hwmon_sensor *sensor, smc_key key, char *name, u32 attrs)
{
	int ret;

	ret = apple_smc_get_key_info(smc, key, &sensor->info);
	if (ret)
		return ret;

	sensor->macsmc_key = key;
	sensor->label = name;
	sensor->attrs = attrs;

	return 0;
}
/*
 * Fan data is exposed by the SMC as multiple sensors.
 *
 * The devicetree schema reuses apple,key-id for the actual fan speed sensor.
 * Min, max and target keys do not need labels, so we can reuse label
 * for naming the entire fan.
 */
static int macsmc_hwmon_create_fan(struct device *dev, struct apple_smc *smc,
				   struct device_node *fan_node,
				   struct macsmc_hwmon_fan *fan)
{
	const char *label, *now, *min, *max, *set, *mode;
	int ret;

	ret = of_property_read_string(fan_node, "apple,key-id", &now);
	if (ret) {
		dev_err(dev, "apple,key-id not found in fan node!\n");
		return ret;
	}

	ret = macsmc_hwmon_parse_key(dev, smc, &fan->now, now);
	if (ret)
		return ret;

	fan->attrs = HWMON_F_INPUT;

	ret = of_property_read_string(fan_node, "label", &label);
	if (ret) {
		dev_dbg(dev, "No label found for fan %s\n", now);
	} else {
		strscpy_pad(fan->label, label, sizeof(fan->label));
		fan->attrs |= HWMON_F_LABEL;
	}

	/* The following keys are not required to simply monitor fan speed */
	if (!of_property_read_string(fan_node, "apple,fan-minimum", &min)) {
		ret = macsmc_hwmon_parse_key(dev, smc, &fan->min, min);
		if (ret)
			return ret;

		fan->attrs |= HWMON_F_MIN;
	}

	if (!of_property_read_string(fan_node, "apple,fan-maximum", &max)) {
		ret = macsmc_hwmon_parse_key(dev, smc, &fan->max, max);
		if (ret)
			return ret;

		fan->attrs |= HWMON_F_MAX;
	}

	if (!of_property_read_string(fan_node, "apple,fan-target", &set)) {
		ret = macsmc_hwmon_parse_key(dev, smc, &fan->set, set);
		if (ret)
			return ret;

		fan->attrs |= HWMON_F_TARGET;
	}

	if (!of_property_read_string(fan_node, "apple,fan-mode", &mode)) {
		ret = macsmc_hwmon_parse_key(dev, smc, &fan->mode, mode);
		if (ret)
			return ret;
	}

	/* Initialise fan control mode to automatic */
	fan->manual = false;

	return 0;
}

static int macsmc_hwmon_create_fan_acpi(struct apple_smc *smc,
					struct macsmc_hwmon_fan *fan, int num)
{
	const char num_char = num + '0';
	int ret;

	ret = macsmc_hwmon_create_sensor_acpi(smc, &fan->now,
			__SMC_KEY('F', num_char, 'A', 'c'), NULL, 0);
	if (ret)
		return ret;

	if (apple_smc_key_exists(smc, __SMC_KEY('F', num_char, 'I', 'D'))) {
		char buf[17];

		ret = apple_smc_read(smc, __SMC_KEY('F', num_char, 'I', 'D'), buf, 16);
		if (ret)
			return ret;

		buf[16] = '\0';
		snprintf(fan->label, sizeof(fan->label), "%s", buf + 4);
	} else
		snprintf(fan->label, sizeof(fan->label), "Fan %d", num);

	ret = macsmc_hwmon_create_sensor_acpi(smc, &fan->min,
			__SMC_KEY('F', num_char, 'M', 'n'), NULL, 0);
	if (ret)
		return ret;

	ret = macsmc_hwmon_create_sensor_acpi(smc, &fan->max,
			__SMC_KEY('F', num_char, 'M', 'x'), NULL, 0);
	if (ret)
		return ret;

	ret = macsmc_hwmon_create_sensor_acpi(smc, &fan->set,
			__SMC_KEY('F', num_char, 'T', 'g'), NULL, 0);
	if (ret)
		return ret;

	if (apple_smc_key_exists(smc, __SMC_KEY('F', num_char, 'M', 'd'))) {
		ret = macsmc_hwmon_create_sensor_acpi(smc, &fan->mode,
				__SMC_KEY('F', num_char, 'M', 'd'), NULL, 0);
		if (ret)
			return ret;
	}

	fan->attrs = HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MIN | HWMON_F_MAX | HWMON_F_TARGET;
	fan->manual = false;

	return 0;
}

static int macsmc_hwmon_populate_sensors(struct macsmc_hwmon *hwmon,
					 struct device_node *hwmon_node)
{
	struct device_node *key_node __maybe_unused;
	struct macsmc_hwmon_sensor *sensor;
	u32 n_current = 0, n_fan = 0, n_power = 0, n_temperature = 0, n_voltage = 0;

	for_each_child_of_node_with_prefix(hwmon_node, key_node, "current-") {
		n_current++;
	}

	if (n_current) {
		hwmon->curr.sensors = devm_kcalloc(hwmon->dev, n_current,
						   sizeof(struct macsmc_hwmon_sensor), GFP_KERNEL);
		if (!hwmon->curr.sensors)
			return -ENOMEM;

		for_each_child_of_node_with_prefix(hwmon_node, key_node, "current-") {
			sensor = &hwmon->curr.sensors[hwmon->curr.count];
			if (!macsmc_hwmon_create_sensor(hwmon->dev, hwmon->smc, key_node, sensor)) {
				sensor->attrs = HWMON_C_INPUT;

				if (*sensor->label)
					sensor->attrs |= HWMON_C_LABEL;

				hwmon->curr.count++;
			}
		}
	}

	for_each_child_of_node_with_prefix(hwmon_node, key_node, "fan-") {
		n_fan++;
	}

	if (n_fan) {
		hwmon->fan.fans = devm_kcalloc(hwmon->dev, n_fan,
						sizeof(struct macsmc_hwmon_fan), GFP_KERNEL);
		if (!hwmon->fan.fans)
			return -ENOMEM;

		for_each_child_of_node_with_prefix(hwmon_node, key_node, "fan-") {
			if (!macsmc_hwmon_create_fan(hwmon->dev, hwmon->smc, key_node,
						     &hwmon->fan.fans[hwmon->fan.count]))
				hwmon->fan.count++;
		}
	}

	for_each_child_of_node_with_prefix(hwmon_node, key_node, "power-") {
		n_power++;
	}

	if (n_power) {
		hwmon->power.sensors = devm_kcalloc(hwmon->dev, n_power,
						    sizeof(struct macsmc_hwmon_sensor), GFP_KERNEL);
		if (!hwmon->power.sensors)
			return -ENOMEM;

		for_each_child_of_node_with_prefix(hwmon_node, key_node, "power-") {
			sensor = &hwmon->power.sensors[hwmon->power.count];
			if (!macsmc_hwmon_create_sensor(hwmon->dev, hwmon->smc, key_node, sensor)) {
				sensor->attrs = HWMON_P_INPUT;

				if (*sensor->label)
					sensor->attrs |= HWMON_P_LABEL;

				hwmon->power.count++;
			}
		}
	}

	for_each_child_of_node_with_prefix(hwmon_node, key_node, "temperature-") {
		n_temperature++;
	}

	if (n_temperature) {
		hwmon->temp.sensors = devm_kcalloc(hwmon->dev, n_temperature,
						   sizeof(struct macsmc_hwmon_sensor), GFP_KERNEL);
		if (!hwmon->temp.sensors)
			return -ENOMEM;

		for_each_child_of_node_with_prefix(hwmon_node, key_node, "temperature-") {
			sensor = &hwmon->temp.sensors[hwmon->temp.count];
			if (!macsmc_hwmon_create_sensor(hwmon->dev, hwmon->smc, key_node, sensor)) {
				sensor->attrs = HWMON_T_INPUT;

				if (*sensor->label)
					sensor->attrs |= HWMON_T_LABEL;

				hwmon->temp.count++;
			}
		}
	}

	for_each_child_of_node_with_prefix(hwmon_node, key_node, "voltage-") {
		n_voltage++;
	}

	if (n_voltage) {
		hwmon->volt.sensors = devm_kcalloc(hwmon->dev, n_voltage,
						   sizeof(struct macsmc_hwmon_sensor), GFP_KERNEL);
		if (!hwmon->volt.sensors)
			return -ENOMEM;

		for_each_child_of_node_with_prefix(hwmon_node, key_node, "voltage-") {
			sensor = &hwmon->volt.sensors[hwmon->volt.count];
			if (!macsmc_hwmon_create_sensor(hwmon->dev, hwmon->smc, key_node, sensor)) {
				sensor->attrs = HWMON_I_INPUT;

				if (*sensor->label)
					sensor->attrs |= HWMON_I_LABEL;

				hwmon->volt.count++;
			}
		}
	}

	return 0;
}

static int macsmc_hwmon_register_sensors_acpi(
	struct macsmc_hwmon *hwmon,
	struct macsmc_hwmon_sensors *sensors,
	const struct macsmc_sensor_desc *descs,
	size_t num_descs,
	u32 attrs)
{
	size_t count = 0;
	size_t i;
	int ret;

	for (i = 0; i < num_descs; i++) {
		if (apple_smc_key_exists(hwmon->smc, descs[i].key))
			count++;
	}

	if (!count)
		return 0;

	sensors->sensors = devm_kcalloc(hwmon->dev, count,
					sizeof(*sensors->sensors),
					GFP_KERNEL);
	if (!sensors->sensors)
		return -ENOMEM;

	sensors->count = 0;

	for (i = 0; i < num_descs; i++) {
		if (!apple_smc_key_exists(hwmon->smc, descs[i].key))
			continue;

		ret = macsmc_hwmon_create_sensor_acpi(hwmon->smc,
						&sensors->sensors[sensors->count],
						descs[i].key,
						descs[i].name,
						attrs);
		if (ret)
			return ret;

		sensors->count++;
	}

	return 0;
}

static int macsmc_hwmon_populate_sensors_acpi(struct macsmc_hwmon *hwmon)
{
	int ret, i;

	ret = apple_smc_read_u8(hwmon->smc, SMC_KEY(FNum), &hwmon->fan.count);
	if (hwmon->fan.count && !ret) {
		hwmon->fan.fans = devm_kcalloc(hwmon->dev, hwmon->fan.count,
				sizeof(struct macsmc_hwmon_fan), GFP_KERNEL);
		if (!hwmon->fan.fans)
			return -ENOMEM;

		hwmon->fan.has_old_manual = apple_smc_key_exists(hwmon->smc,
				__SMC_KEY('F', 'S', '!', ' '));

		for (i = 0; i < hwmon->fan.count; i++) {
			macsmc_hwmon_create_fan_acpi(hwmon->smc, &hwmon->fan.fans[i], i);
			if (ret)
				return ret;
		}
	}
	ret = macsmc_hwmon_register_sensors_acpi(hwmon, &hwmon->temp,
						temp_sensors,
						ARRAY_SIZE(temp_sensors),
						HWMON_T_INPUT | HWMON_T_LABEL);
	if (ret)
		return ret;

	ret = macsmc_hwmon_register_sensors_acpi(hwmon, &hwmon->power,
						power_sensors,
						ARRAY_SIZE(power_sensors),
						HWMON_P_INPUT | HWMON_P_LABEL);
	if (ret)
		return ret;

	ret = macsmc_hwmon_register_sensors_acpi(hwmon, &hwmon->curr,
						current_sensors,
						ARRAY_SIZE(current_sensors),
						HWMON_C_INPUT | HWMON_C_LABEL);
	if (ret)
		return ret;

	ret = macsmc_hwmon_register_sensors_acpi(hwmon, &hwmon->volt,
						voltage_sensors,
						ARRAY_SIZE(voltage_sensors),
						HWMON_I_INPUT | HWMON_I_LABEL);
	if (ret)
		return ret;

	return 0;
}
/* Create NULL-terminated config arrays */
static void macsmc_hwmon_populate_configs(u32 *configs, const struct macsmc_hwmon_sensors *sensors)
{
	int idx;

	for (idx = 0; idx < sensors->count; idx++)
		configs[idx] = sensors->sensors[idx].attrs;
}

static void macsmc_hwmon_populate_fan_configs(u32 *configs, const struct macsmc_hwmon_fans *fans)
{
	int idx;

	for (idx = 0; idx < fans->count; idx++)
		configs[idx] = fans->fans[idx].attrs;
}

static const struct hwmon_channel_info *const macsmc_chip_channel_info =
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ);

static int macsmc_hwmon_create_infos(struct macsmc_hwmon *hwmon)
{
	struct hwmon_channel_info *channel_info;
	int i = 0;

	/* chip */
	hwmon->channel_infos[i++] = macsmc_chip_channel_info;

	if (hwmon->curr.count) {
		channel_info = &hwmon->curr.channel_info;
		channel_info->type = hwmon_curr;
		channel_info->config = devm_kcalloc(hwmon->dev, hwmon->curr.count + 1,
						    sizeof(u32), GFP_KERNEL);
		if (!channel_info->config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)channel_info->config, &hwmon->curr);
		hwmon->channel_infos[i++] = channel_info;
	}

	if (hwmon->fan.count) {
		channel_info = &hwmon->fan.channel_info;
		channel_info->type = hwmon_fan;
		channel_info->config = devm_kcalloc(hwmon->dev, hwmon->fan.count + 1,
						    sizeof(u32), GFP_KERNEL);
		if (!channel_info->config)
			return -ENOMEM;

		macsmc_hwmon_populate_fan_configs((u32 *)channel_info->config, &hwmon->fan);
		hwmon->channel_infos[i++] = channel_info;
	}

	if (hwmon->power.count) {
		channel_info = &hwmon->power.channel_info;
		channel_info->type = hwmon_power;
		channel_info->config = devm_kcalloc(hwmon->dev, hwmon->power.count + 1,
						    sizeof(u32), GFP_KERNEL);
		if (!channel_info->config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)channel_info->config, &hwmon->power);
		hwmon->channel_infos[i++] = channel_info;
	}

	if (hwmon->temp.count) {
		channel_info = &hwmon->temp.channel_info;
		channel_info->type = hwmon_temp;
		channel_info->config = devm_kcalloc(hwmon->dev, hwmon->temp.count + 1,
						    sizeof(u32), GFP_KERNEL);
		if (!channel_info->config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)channel_info->config, &hwmon->temp);
		hwmon->channel_infos[i++] = channel_info;
	}

	if (hwmon->volt.count) {
		channel_info = &hwmon->volt.channel_info;
		channel_info->type = hwmon_in;
		channel_info->config = devm_kcalloc(hwmon->dev, hwmon->volt.count + 1,
						    sizeof(u32), GFP_KERNEL);
		if (!channel_info->config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)channel_info->config, &hwmon->volt);
		hwmon->channel_infos[i++] = channel_info;
	}

	return 0;
}

static int macsmc_hwmon_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct macsmc_hwmon *hwmon;
	int ret;

	/*
	 * The MFD driver will try to probe us unconditionally. Some devices
	 * with the SMC do not have hwmon capabilities. Only probe if we have
	 * a hwmon node or the machine is a intel mac.
	 */
	if (!pdev->dev.of_node && !smc->is_acpi)
		return -ENODEV;

	hwmon = devm_kzalloc(&pdev->dev, sizeof(*hwmon),
			     GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	hwmon->dev = &pdev->dev;
	hwmon->smc = smc;
	platform_set_drvdata(pdev, hwmon);

	if (hwmon->smc->is_acpi)
		ret = macsmc_hwmon_populate_sensors_acpi(hwmon);
	else
		ret = macsmc_hwmon_populate_sensors(hwmon, hwmon->dev->of_node);
	if (ret) {
		dev_err(hwmon->dev, "Could not parse sensors\n");
		return ret;
	}

	if (!hwmon->curr.count && !hwmon->fan.count &&
	    !hwmon->power.count && !hwmon->temp.count &&
	    !hwmon->volt.count) {
		dev_err(hwmon->dev,
			"No valid sensors found of any supported type\n");
		return -ENODEV;
	}

	ret = macsmc_hwmon_create_infos(hwmon);
	if (ret)
		return ret;

	hwmon->chip_info.ops = &macsmc_hwmon_ops;
	hwmon->chip_info.info =
		(const struct hwmon_channel_info *const *)&hwmon->channel_infos;

	hwmon->hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
								"macsmc_hwmon", hwmon,
								&hwmon->chip_info, NULL);
	if (IS_ERR(hwmon->hwmon_dev))
		return dev_err_probe(hwmon->dev, PTR_ERR(hwmon->hwmon_dev),
				     "Probing SMC hwmon device failed\n");

	dev_dbg(hwmon->dev, "Registered SMC hwmon device. Sensors:\n");
	dev_dbg(hwmon->dev,
		"Current: %d, Fans: %d, Power: %d, Temperature: %d, Voltage: %d",
		hwmon->curr.count, hwmon->fan.count,
		hwmon->power.count, hwmon->temp.count,
		hwmon->volt.count);

	return 0;
}

static int macsmc_hwmon_resume(struct platform_device *pdev)
{
	struct macsmc_hwmon *hwmon = dev_get_drvdata(&pdev->dev);

	if (hwmon->smc->is_acpi && !hwmon->fan.has_old_manual) {
		int idx;
		int ret;

		for (idx = 0; idx < hwmon->fan.count; idx++)
			if (hwmon->fan.fans[idx].manual) {
				ret = macsmc_hwmon_write_key(hwmon->smc,
						&hwmon->fan.fans[idx].mode, 1);
				if (ret < 0) {
					dev_warn(&pdev->dev,
						"Unable to reenable fan manual, disabling\n");
					hwmon->fan.fans[idx].manual = false;
				}
			}

	}

	return 0;
}

static const struct of_device_id macsmc_hwmon_of_table[] = {
	{ .compatible = "apple,smc-hwmon" },
	{}
};
MODULE_DEVICE_TABLE(of, macsmc_hwmon_of_table);

static struct platform_driver macsmc_hwmon_driver = {
	.probe = macsmc_hwmon_probe,
	.resume = macsmc_hwmon_resume,
	.driver = {
		.name = "macsmc-hwmon",
		.of_match_table = macsmc_hwmon_of_table,
	},
};
module_platform_driver(macsmc_hwmon_driver);

MODULE_DESCRIPTION("Apple Silicon SMC hwmon driver");
MODULE_AUTHOR("James Calligeros <jcalligeros99@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_ALIAS("platform:macsmc-hwmon");
