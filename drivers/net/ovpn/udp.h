/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_UDP_H_
#define _NET_OVPN_UDP_H_

struct ovpn_struct;
struct socket;

int ovpn_udp_socket_attach(struct socket *sock, struct ovpn_struct *ovpn);

#endif /* _NET_OVPN_UDP_H_ */
