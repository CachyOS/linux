// SPDX-License-Identifier: GPL-2.0-only
/*
 * aw88399_hda_property.c -- AW88399 HDA property driver
 *
 * Based on cs35l41_hda_property.c
 *
 * Author: Marco Giunta
 */

#include <linux/string.h>
#include "aw88399_hda_property.h"

static int aw88399_swap_channels(struct aw88399_hda *aw88399)
{
	/*
	 * Certain Lenovo Legion laptops have their
	 * I2C wiring reversed: 0x34 is physically right speaker,
	 * 0x35 is left. Swap channels to correct L/R assignment.
	 * This is a model-specific hardware wiring issue, not a driver bug.
	 */
	aw88399->channel = 1 - aw88399->channel;
	dev_info(aw88399->dev,
		 "AW88399: Channel swap applied: index %d -> channel %d\n",
		 aw88399->index, aw88399->channel);
	return 0;
}

static int aw88399_skip_bsts_check(struct aw88399_hda *aw88399)
{
	/*
	 * BSTS (boost-finished) status bit does not reliably report on
	 * some hardware. On certain Lenovo Legion laptops, both amps
	 * report BSTS=0 (boost not finished) during normal playback with
	 * active boost mode confirmed (ADPS=1). Skip BSTS in the
	 * startup status check to avoid false init failures.
	 */
	aw88399->bsts_unreliable = true;
	dev_info(aw88399->dev, "AW88399: BSTS status check disabled\n");
	return 0;
}

static int aw88399_apply_legion_quirks(struct aw88399_hda *aw88399)
{
	aw88399_swap_channels(aw88399);
	aw88399_skip_bsts_check(aw88399);
	return 0;
}

struct aw88399_prop_model {
	const char *ssid;
	int (*add_prop)(struct aw88399_hda *aw88399);
};

static const struct aw88399_prop_model aw88399_prop_model_table[] = {
	{ "17AA3906", aw88399_apply_legion_quirks },
	{ "17AA3907", aw88399_apply_legion_quirks },
	{ "17AA3927", aw88399_apply_legion_quirks },
	{ "17AA3928", aw88399_apply_legion_quirks },
	{ "17AA3938", aw88399_apply_legion_quirks },
	{ "17AA3939", aw88399_apply_legion_quirks },
	{}
};

int aw88399_add_properties(struct aw88399_hda *aw88399)
{
	const struct aw88399_prop_model *model;

	for (model = aw88399_prop_model_table; model->ssid; model++) {
		if (!strcasecmp(model->ssid, aw88399->acpi_subsystem_id)) {
			dev_info(aw88399->dev, "Picked up properties for ACPI SSID %s\n",
				 aw88399->acpi_subsystem_id);
			return model->add_prop(aw88399);
		}
	}

	return -ENOENT;
}
