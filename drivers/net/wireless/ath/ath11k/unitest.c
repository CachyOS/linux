// SPDX-License-Identifier: BSD-3-Clause-Clear
/**
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 */

#include <net/netlink.h>
#include "debug.h"
#include "unitest.h"

const struct nla_policy ath11k_unitest_policy[UNITEST_MAX + 1] = {
	[UNITEST_MODULE_ID] = {.type = NLA_U32},
	[UNITEST_ARGS_NUM] = {.type = NLA_U32},
	[UNITEST_ARGS] = {.type = NLA_BINARY,
		.len = MAX_UNITEST_MEMORY_LEN},
};

/**
 * ath11k_unit_test() - send unit test command
 * @wiphy:    wiphy structure pointer
 * @wdev:     Wireless device structure pointer
 * @data:     Pointer to the data received
 * @data_len: Length of @data
 *
 * Return: 0 on success; errno on failure
 */

int ath11k_unit_test(struct wiphy *wiphy, struct wireless_dev *wdev,
		     const void *data, int data_len)
{
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct ath11k *ar = hw->priv;
	struct ath11k_base *ab = ar->ab;
	struct nlattr *tb[UNITEST_MAX + 1];
	int status = 0;
	u32 module_id, num_args, temp_num_args, len;
	bool sta_found = false;
	struct ath11k_vif *arvif;
	struct unit_test_cmd utest_cmd = {0};

	list_for_each_entry(arvif, &ar->arvifs, list) {
		if (arvif->vdev_type != WMI_VDEV_TYPE_STA ||
		    arvif->vdev_subtype != WMI_VDEV_SUBTYPE_NONE)
			continue;
		sta_found = true;
		break;
	}
	if (!sta_found) {
		ath11k_warn(ar->ab, "no sta found.");
		return -EINVAL;
	}

	utest_cmd.vdev_id = arvif->vdev_id;

	if (nla_parse(tb, UNITEST_MAX, data, data_len, ath11k_unitest_policy, NULL)) {
		ath11k_warn(ab, "Invalid ATTR");
		return -EINVAL;
	}

	if (!tb[UNITEST_MODULE_ID]) {
		ath11k_warn(ab, "attr unitest module id failed");
		return -EINVAL;
	}
	module_id = nla_get_u32(tb[UNITEST_MODULE_ID]);
	utest_cmd.module_id = module_id;

	if (!tb[UNITEST_ARGS_NUM]) {
		ath11k_warn(ab, "attr unitest args num failed");
		return -EINVAL;
	}
	num_args = nla_get_u32(tb[UNITEST_ARGS_NUM]);
	utest_cmd.num_args = num_args;
	if (num_args > UNIT_TEST_MAX_NUM_ARGS) {
		ath11k_warn(ab, "num args exceed");
		return -EINVAL;
	}

	if (!tb[UNITEST_ARGS]) {
		ath11k_warn(ab, "attr unitest args failed");
		return -EINVAL;
	}
	len = nla_len(tb[UNITEST_ARGS]);
	temp_num_args = len / sizeof(u32);
	if (num_args != temp_num_args) {
		ath11k_warn(ab, "num args mismatch");
		return -EINVAL;
	}
	nla_memcpy(utest_cmd.args, tb[UNITEST_ARGS], len);

	status = ath11k_wmi_set_unit_test(ar, &utest_cmd);
	if (status) {
		ath11k_warn(ab, "Unable to post unit test message (status-%d)", status);
		return -EINVAL;
	}

	return 0;
}
