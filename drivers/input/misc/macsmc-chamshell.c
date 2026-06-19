// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC Chamshell driver
 *
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#include <linux/input.h>
#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CHAMSHELL_KEY	SMC_KEY(MSLD)
#define CHAMSHELL_POLL_INTERVAL 500

struct macsmc_chamshell {
	struct apple_smc *smc;
	u8 last_state;
};

static void macsmc_chamshell_poll(struct input_dev *input)
{
	struct macsmc_chamshell *chamshell = input_get_drvdata(input);
	u8 val;
	int ret;

	ret = apple_smc_read_u8(chamshell->smc, CHAMSHELL_KEY, &val);
	if (ret)
		return;

	/* Normalize the lid state to 0 or 1.
	 * I'm not sure if the SMC reports values other than 0,
	 * but I do know that 0 means the lid is open.
	 */
	val = !!val;

	if (val != chamshell->last_state) {
		chamshell->last_state = val;
		input_report_switch(input, SW_LID, val);
		input_sync(input);
	}
}

static int macsmc_chamshell_probe(struct platform_device *pdev)
{
	struct macsmc_chamshell *chamshell;
	struct input_dev *input;
	int ret;

	chamshell = devm_kzalloc(&pdev->dev, sizeof(*chamshell), GFP_KERNEL);
	if (!chamshell)
		return -ENOMEM;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	chamshell->smc = dev_get_drvdata(pdev->dev.parent);
	if (!chamshell->smc)
		return -ENODEV;

	if (!apple_smc_key_exists(chamshell->smc, CHAMSHELL_KEY))
		return -ENODEV;

	input->name = "macsmc-chamshell";
	input->id.bustype = BUS_HOST;

	input_set_capability(input, EV_SW, SW_LID);
	input_set_drvdata(input, chamshell);

	ret = input_setup_polling(input, macsmc_chamshell_poll);
	if (ret)
		return ret;

	input_set_poll_interval(input, CHAMSHELL_POLL_INTERVAL);

	ret = input_register_device(input);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, chamshell);
	return 0;
}

static struct platform_driver macsmc_chamshell_platform_driver = {
	.probe = macsmc_chamshell_probe,
	.driver = {
		.name = "macsmc-chamshell",
	},
};

module_platform_driver(macsmc_chamshell_platform_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC Chamshell driver");
MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_ALIAS("platform:macsmc-chamshell");
