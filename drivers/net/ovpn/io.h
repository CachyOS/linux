/* SPDX-License-Identifier: GPL-2.0-only */
/* OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_OVPN_H_
#define _NET_OVPN_OVPN_H_

netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev);

#endif /* _NET_OVPN_OVPN_H_ */
