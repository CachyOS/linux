/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_OVPNSTRUCT_H_
#define _NET_OVPN_OVPNSTRUCT_H_

#include <net/gro_cells.h>
#include <net/net_trackers.h>
#include <uapi/linux/if_link.h>
#include <uapi/linux/ovpn.h>

/**
 * struct ovpn_peer_collection - container of peers for MultiPeer mode
 * @by_id: table of peers index by ID
 * @by_vpn_addr: table of peers indexed by VPN IP address (items can be
 *		 rehashed on the fly due to peer IP change)
 * @by_transp_addr: table of peers indexed by transport address (items can be
 *		    rehashed on the fly due to peer IP change)
 * @lock: protects writes to peer tables
 */
struct ovpn_peer_collection {
	DECLARE_HASHTABLE(by_id, 12);
	struct hlist_nulls_head by_vpn_addr[1 << 12];
	struct hlist_nulls_head by_transp_addr[1 << 12];

	spinlock_t lock; /* protects writes to peer tables */
};

/**
 * struct ovpn_struct - per ovpn interface state
 * @dev: the actual netdev representing the tunnel
 * @dev_tracker: reference tracker for associated dev
 * @registered: whether dev is still registered with netdev or not
 * @mode: device operation mode (i.e. p2p, mp, ..)
 * @lock: protect this object
 * @peers: data structures holding multi-peer references
 * @peer: in P2P mode, this is the only remote peer
 * @dev_list: entry for the module wide device list
 * @gro_cells: pointer to the Generic Receive Offload cell
 * @keepalive_work: struct used to schedule keepalive periodic job
 */
struct ovpn_struct {
	struct net_device *dev;
	netdevice_tracker dev_tracker;
	bool registered;
	enum ovpn_mode mode;
	spinlock_t lock; /* protect writing to the ovpn_struct object */
	struct ovpn_peer_collection *peers;
	struct ovpn_peer __rcu *peer;
	struct list_head dev_list;
	struct gro_cells gro_cells;
	struct delayed_work keepalive_work;
};

#endif /* _NET_OVPN_OVPNSTRUCT_H_ */
