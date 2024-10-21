/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_UDP_H_
#define _NET_OVPN_UDP_H_

#include <linux/skbuff.h>
#include <net/sock.h>

struct ovpn_peer;
struct ovpn_struct;
struct sk_buff;
struct socket;

int ovpn_udp_socket_attach(struct socket *sock, struct ovpn_struct *ovpn);
void ovpn_udp_socket_detach(struct socket *sock);
void ovpn_udp_send_skb(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
		       struct sk_buff *skb);
struct ovpn_struct *ovpn_from_udp_sock(struct sock *sk);

#endif /* _NET_OVPN_UDP_H_ */
