/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_MAIN_H_
#define _NET_OVPN_MAIN_H_

bool ovpn_dev_is_valid(const struct net_device *dev);

#define SKB_HEADER_LEN                                       \
	(max(sizeof(struct iphdr), sizeof(struct ipv6hdr)) + \
	 sizeof(struct udphdr) + NET_SKB_PAD)

#define OVPN_HEAD_ROOM ALIGN(16 + SKB_HEADER_LEN, 4)
#define OVPN_MAX_PADDING 16

#endif /* _NET_OVPN_MAIN_H_ */
