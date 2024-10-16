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

#include <net/net_trackers.h>

/**
 * struct ovpn_struct - per ovpn interface state
 * @dev: the actual netdev representing the tunnel
 * @dev_tracker: reference tracker for associated dev
 */
struct ovpn_struct {
	struct net_device *dev;
	netdevice_tracker dev_tracker;
};

#endif /* _NET_OVPN_OVPNSTRUCT_H_ */
