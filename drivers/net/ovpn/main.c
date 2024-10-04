// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 *		James Yonan <james@openvpn.net>
 */

#include <linux/ethtool.h>
#include <linux/genetlink.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
//#include <linux/rcupdate.h>
#include <linux/version.h>
#include <net/gro_cells.h>
#include <net/ip.h>
#include <net/rtnetlink.h>
#include <uapi/linux/if_arp.h>
#include <uapi/linux/ovpn.h>

#include "ovpnstruct.h"
#include "main.h"
#include "netlink.h"
#include "io.h"
#include "packet.h"
#include "peer.h"
#include "tcp.h"

/* Driver info */
#define DRV_DESCRIPTION	"OpenVPN data channel offload (ovpn)"
#define DRV_COPYRIGHT	"(C) 2020-2024 OpenVPN, Inc."

/**
 * ovpn_struct_init - Initialize the netdevice private area
 * @dev: the device to initialize
 * @mode: device operation mode (i.e. p2p, mp, ..)
 */
static void ovpn_struct_init(struct net_device *dev, enum ovpn_mode mode)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);

	ovpn->dev = dev;
	ovpn->mode = mode;
	spin_lock_init(&ovpn->lock);
	INIT_DELAYED_WORK(&ovpn->keepalive_work, ovpn_peer_keepalive_work);
}

static void ovpn_struct_free(struct net_device *net)
{
	struct ovpn_struct *ovpn = netdev_priv(net);

	gro_cells_destroy(&ovpn->gro_cells);
	kfree(ovpn->peers);
}

static int ovpn_net_init(struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	struct in_device *dev_v4;
	int i, err;

	err = gro_cells_init(&ovpn->gro_cells, dev);
	if (err)
		return err;

	if (ovpn->mode == OVPN_MODE_MP) {
		dev_v4 = __in_dev_get_rtnl(dev);
		if (dev_v4) {
			/* disable redirects as Linux gets confused by ovpn
			 * handling same-LAN routing.
			 * This happens because a multipeer interface is used as
			 * relay point between hosts in the same subnet, while
			 * in a classic LAN this would not be needed because the
			 * two hosts would be able to talk directly.
			 */
			IN_DEV_CONF_SET(dev_v4, SEND_REDIRECTS, false);
			IPV4_DEVCONF_ALL(dev_net(dev), SEND_REDIRECTS) = false;
		}

		/* the peer container is fairly large, therefore we dynamically
		 * allocate it only when needed
		 */
		ovpn->peers = kzalloc(sizeof(*ovpn->peers), GFP_KERNEL);
		if (!ovpn->peers) {
			gro_cells_destroy(&ovpn->gro_cells);
			return -ENOMEM;
		}

		spin_lock_init(&ovpn->peers->lock);

		for (i = 0; i < ARRAY_SIZE(ovpn->peers->by_id); i++) {
			INIT_HLIST_HEAD(&ovpn->peers->by_id[i]);
			INIT_HLIST_NULLS_HEAD(&ovpn->peers->by_vpn_addr[i], i);
			INIT_HLIST_NULLS_HEAD(&ovpn->peers->by_transp_addr[i],
					      i);
		}
	}

	return 0;
}

static int ovpn_net_open(struct net_device *dev)
{
	/* ovpn keeps the carrier always on to avoid losing IP or route
	 * configuration upon disconnection. This way it can prevent leaks
	 * of traffic outside of the VPN tunnel.
	 * The user may override this behaviour by tearing down the interface
	 * manually.
	 */
	netif_carrier_on(dev);
	netif_tx_start_all_queues(dev);
	return 0;
}

static int ovpn_net_stop(struct net_device *dev)
{
	netif_tx_stop_all_queues(dev);
	return 0;
}

static const struct net_device_ops ovpn_netdev_ops = {
	.ndo_init		= ovpn_net_init,
	.ndo_open		= ovpn_net_open,
	.ndo_stop		= ovpn_net_stop,
	.ndo_start_xmit		= ovpn_net_xmit,
};

/**
 * ovpn_dev_is_valid - check if the netdevice is of type 'ovpn'
 * @dev: the interface to check
 *
 * Return: whether the netdevice is of type 'ovpn'
 */
bool ovpn_dev_is_valid(const struct net_device *dev)
{
	return dev->netdev_ops->ndo_start_xmit == ovpn_net_xmit;
}

static void ovpn_get_drvinfo(struct net_device *dev,
			     struct ethtool_drvinfo *info)
{
	strscpy(info->driver, OVPN_FAMILY_NAME, sizeof(info->driver));
	strscpy(info->bus_info, "ovpn", sizeof(info->bus_info));
}

static const struct ethtool_ops ovpn_ethtool_ops = {
	.get_drvinfo		= ovpn_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_ts_info		= ethtool_op_get_ts_info,
};

static struct rtnl_link_ops ovpn_link_ops = {
	.kind = OVPN_FAMILY_NAME,
	.netns_refund = false,
	.dellink = unregister_netdevice_queue,
};

static void ovpn_setup(struct net_device *dev)
{
	/* compute the overhead considering AEAD encryption */
	const int overhead = sizeof(u32) + NONCE_WIRE_SIZE + 16 +
			     sizeof(struct udphdr) +
			     max(sizeof(struct ipv6hdr), sizeof(struct iphdr));

	netdev_features_t feat = NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_RXCSUM |
				 NETIF_F_GSO | NETIF_F_GSO_SOFTWARE |
				 NETIF_F_HIGHDMA;

	dev->needs_free_netdev = true;

	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;

	dev->ethtool_ops = &ovpn_ethtool_ops;
	dev->netdev_ops = &ovpn_netdev_ops;
	dev->rtnl_link_ops = &ovpn_link_ops;

	dev->priv_destructor = ovpn_struct_free;

	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->mtu = ETH_DATA_LEN - overhead;
	dev->min_mtu = IPV4_MIN_MTU;
	dev->max_mtu = IP_MAX_MTU - overhead;

	dev->type = ARPHRD_NONE;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP;

	dev->lltx = true;
	dev->features |= feat;
	dev->hw_features |= feat;
	dev->hw_enc_features |= feat;

	dev->needed_headroom = OVPN_HEAD_ROOM;
	dev->needed_tailroom = OVPN_MAX_PADDING;
}

/**
 * ovpn_iface_create - create and initialize a new 'ovpn' netdevice
 * @name: the name of the new device
 * @mode: the OpenVPN mode to set this device to
 * @net: the netns this device should be created in
 *
 * A new netdevice is created and registered.
 * Its private area is initialized with an empty ovpn_struct object.
 *
 * Return: a pointer to the new device on success or a negative error code
 *         otherwise
 */
struct net_device *ovpn_iface_create(const char *name, enum ovpn_mode mode,
				     struct net *net)
{
	struct net_device *dev;
	int ret;

	dev = alloc_netdev(sizeof(struct ovpn_struct), name, NET_NAME_USER,
			   ovpn_setup);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev_net_set(dev, net);
	ovpn_struct_init(dev, mode);

	rtnl_lock();
	ret = register_netdevice(dev);
	if (ret < 0) {
		netdev_err(dev, "cannot register interface: %d\n", ret);
		rtnl_unlock();
		goto err;
	}
	/* turn carrier explicitly off after registration, this way state is
	 * clearly defined
	 */
	netif_carrier_off(dev);
	rtnl_unlock();

	return dev;

err:
	free_netdev(dev);
	return ERR_PTR(ret);
}

/**
 * ovpn_iface_destruct - tear down netdevice
 * @ovpn: the ovpn instance objected related to the interface to tear down
 *
 * This function takes care of tearing down an ovpn device and can be invoked
 * internally or upon UNREGISTER netdev event
 */
void ovpn_iface_destruct(struct ovpn_struct *ovpn)
{
	ASSERT_RTNL();

	netif_carrier_off(ovpn->dev);

	ovpn->registered = false;

	cancel_delayed_work_sync(&ovpn->keepalive_work);

	switch (ovpn->mode) {
	case OVPN_MODE_P2P:
		ovpn_peer_release_p2p(ovpn);
		break;
	default:
		ovpn_peers_free(ovpn);
		break;
	}
}

static int ovpn_netdev_notifier_call(struct notifier_block *nb,
				     unsigned long state, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct ovpn_struct *ovpn;

	if (!ovpn_dev_is_valid(dev))
		return NOTIFY_DONE;

	ovpn = netdev_priv(dev);

	switch (state) {
	case NETDEV_REGISTER:
		ovpn->registered = true;
		break;
	case NETDEV_UNREGISTER:
		/* twiddle thumbs on netns device moves */
		if (dev->reg_state != NETREG_UNREGISTERING)
			break;

		/* can be delivered multiple times, so check registered flag,
		 * then destroy the interface
		 */
		if (!ovpn->registered)
			return NOTIFY_DONE;

		ovpn_iface_destruct(ovpn);
		break;
	case NETDEV_POST_INIT:
	case NETDEV_GOING_DOWN:
	case NETDEV_DOWN:
	case NETDEV_UP:
	case NETDEV_PRE_UP:
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static struct notifier_block ovpn_netdev_notifier = {
	.notifier_call = ovpn_netdev_notifier_call,
};

static int __init ovpn_init(void)
{
	int err = register_netdevice_notifier(&ovpn_netdev_notifier);

	if (err) {
		pr_err("ovpn: can't register netdevice notifier: %d\n", err);
		return err;
	}

	err = rtnl_link_register(&ovpn_link_ops);
	if (err) {
		pr_err("ovpn: can't register rtnl link ops: %d\n", err);
		goto unreg_netdev;
	}

	err = ovpn_nl_register();
	if (err) {
		pr_err("ovpn: can't register netlink family: %d\n", err);
		goto unreg_rtnl;
	}

	ovpn_tcp_init();

	return 0;

unreg_rtnl:
	rtnl_link_unregister(&ovpn_link_ops);
unreg_netdev:
	unregister_netdevice_notifier(&ovpn_netdev_notifier);
	return err;
}

static __exit void ovpn_cleanup(void)
{
	ovpn_nl_unregister();
	rtnl_link_unregister(&ovpn_link_ops);
	unregister_netdevice_notifier(&ovpn_netdev_notifier);

	rcu_barrier();
}

module_init(ovpn_init);
module_exit(ovpn_cleanup);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_COPYRIGHT);
MODULE_LICENSE("GPL");
