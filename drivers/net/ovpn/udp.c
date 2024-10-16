// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/netdevice.h>
#include <linux/socket.h>
#include <net/udp.h>

#include "ovpnstruct.h"
#include "main.h"
#include "socket.h"
#include "udp.h"

/**
 * ovpn_udp_socket_attach - set udp-tunnel CBs on socket and link it to ovpn
 * @sock: socket to configure
 * @ovpn: the openvp instance to link
 *
 * After invoking this function, the sock will be controlled by ovpn so that
 * any incoming packet may be processed by ovpn first.
 *
 * Return: 0 on success or a negative error code otherwise
 */
int ovpn_udp_socket_attach(struct socket *sock, struct ovpn_struct *ovpn)
{
	struct ovpn_socket *old_data;
	int ret = 0;

	/* sanity check */
	if (sock->sk->sk_protocol != IPPROTO_UDP) {
		DEBUG_NET_WARN_ON_ONCE(1);
		return -EINVAL;
	}

	/* make sure no pre-existing encapsulation handler exists */
	rcu_read_lock();
	old_data = rcu_dereference_sk_user_data(sock->sk);
	if (!old_data) {
		/* socket is currently unused - we can take it */
		rcu_read_unlock();
		return 0;
	}

	/* socket is in use. We need to understand if it's owned by this ovpn
	 * instance or by something else.
	 * In the former case, we can increase the refcounter and happily
	 * use it, because the same UDP socket is expected to be shared among
	 * different peers.
	 *
	 * Unlikely TCP, a single UDP socket can be used to talk to many remote
	 * hosts and therefore openvpn instantiates one only for all its peers
	 */
	if ((READ_ONCE(udp_sk(sock->sk)->encap_type) == UDP_ENCAP_OVPNINUDP) &&
	    old_data->ovpn == ovpn) {
		netdev_dbg(ovpn->dev,
			   "%s: provided socket already owned by this interface\n",
			   __func__);
		ret = -EALREADY;
	} else {
		netdev_err(ovpn->dev,
			   "%s: provided socket already taken by other user\n",
			   __func__);
		ret = -EBUSY;
	}
	rcu_read_unlock();

	return ret;
}
