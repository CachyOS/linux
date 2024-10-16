// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/netdevice.h>
#include <net/genetlink.h>

#include <uapi/linux/ovpn.h>

#include "ovpnstruct.h"
#include "main.h"
#include "io.h"
#include "netlink.h"
#include "netlink-gen.h"

MODULE_ALIAS_GENL_FAMILY(OVPN_FAMILY_NAME);

/**
 * ovpn_get_dev_from_attrs - retrieve the ovpn private data from the netdevice
 *			     a netlink message is targeting
 * @net: network namespace where to look for the interface
 * @info: generic netlink info from the user request
 *
 * Return: the ovpn private data, if found, or an error otherwise
 */
static struct ovpn_struct *
ovpn_get_dev_from_attrs(struct net *net, const struct genl_info *info)
{
	struct ovpn_struct *ovpn;
	struct net_device *dev;
	int ifindex;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_IFINDEX))
		return ERR_PTR(-EINVAL);

	ifindex = nla_get_u32(info->attrs[OVPN_A_IFINDEX]);

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, ifindex);
	if (!dev) {
		rcu_read_unlock();
		NL_SET_ERR_MSG_MOD(info->extack,
				   "ifindex does not match any interface");
		return ERR_PTR(-ENODEV);
	}

	if (!ovpn_dev_is_valid(dev)) {
		rcu_read_unlock();
		NL_SET_ERR_MSG_MOD(info->extack,
				   "specified interface is not ovpn");
		NL_SET_BAD_ATTR(info->extack, info->attrs[OVPN_A_IFINDEX]);
		return ERR_PTR(-EINVAL);
	}

	ovpn = netdev_priv(dev);
	netdev_hold(dev, &ovpn->dev_tracker, GFP_KERNEL);
	rcu_read_unlock();

	return ovpn;
}

int ovpn_nl_pre_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		     struct genl_info *info)
{
	struct ovpn_struct *ovpn = ovpn_get_dev_from_attrs(genl_info_net(info),
							   info);

	if (IS_ERR(ovpn))
		return PTR_ERR(ovpn);

	info->user_ptr[0] = ovpn;

	return 0;
}

void ovpn_nl_post_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		       struct genl_info *info)
{
	struct ovpn_struct *ovpn = info->user_ptr[0];

	if (ovpn)
		netdev_put(ovpn->dev, &ovpn->dev_tracker);
}

int ovpn_nl_peer_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_peer_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_peer_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_peer_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_peer_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_key_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_key_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_key_swap_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_key_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

/**
 * ovpn_nl_register - perform any needed registration in the NL subsustem
 *
 * Return: 0 on success, a negative error code otherwise
 */
int __init ovpn_nl_register(void)
{
	int ret = genl_register_family(&ovpn_nl_family);

	if (ret) {
		pr_err("ovpn: genl_register_family failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/**
 * ovpn_nl_unregister - undo any module wide netlink registration
 */
void ovpn_nl_unregister(void)
{
	genl_unregister_family(&ovpn_nl_family);
}
