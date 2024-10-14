/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_SOCK_H_
#define _NET_OVPN_SOCK_H_

#include <linux/net.h>
#include <linux/kref.h>
#include <net/sock.h>

struct ovpn_struct;
struct ovpn_peer;

/**
 * struct ovpn_socket - a kernel socket referenced in the ovpn code
 * @ovpn: ovpn instance owning this socket (UDP only)
 * @peer: unique peer transmitting over this socket (TCP only)
 * @sock: the low level sock object
 * @refcount: amount of contexts currently referencing this object
 * @rcu: member used to schedule RCU destructor callback
 */
struct ovpn_socket {
	union {
		struct ovpn_struct *ovpn;
		struct ovpn_peer *peer;
	};

	struct socket *sock;
	struct kref refcount;
	struct rcu_head rcu;
};

void ovpn_socket_release_kref(struct kref *kref);

/**
 * ovpn_socket_put - decrease reference counter
 * @sock: the socket whose reference counter should be decreased
 */
static inline void ovpn_socket_put(struct ovpn_socket *sock)
{
	kref_put(&sock->refcount, ovpn_socket_release_kref);
}

struct ovpn_socket *ovpn_socket_new(struct socket *sock,
				    struct ovpn_peer *peer);

#endif /* _NET_OVPN_SOCK_H_ */
