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

#define OVPN_KEEPALIVE_SIZE 16
extern const unsigned char ovpn_keepalive_message[OVPN_KEEPALIVE_SIZE];

netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev);

void ovpn_recv(struct ovpn_peer *peer, struct sk_buff *skb);
void ovpn_xmit_special(struct ovpn_peer *peer, const void *data,
		       const unsigned int len);

void ovpn_encrypt_post(void *data, int ret);
void ovpn_decrypt_post(void *data, int ret);

#endif /* _NET_OVPN_OVPN_H_ */
