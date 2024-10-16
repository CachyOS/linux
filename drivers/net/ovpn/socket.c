// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/net.h>
#include <linux/netdevice.h>

#include "ovpnstruct.h"
#include "main.h"
#include "io.h"
#include "peer.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"

static void ovpn_socket_detach(struct socket *sock)
{
	if (!sock)
		return;

	if (sock->sk->sk_protocol == IPPROTO_UDP)
		ovpn_udp_socket_detach(sock);
	else if (sock->sk->sk_protocol == IPPROTO_TCP)
		ovpn_tcp_socket_detach(sock);

	sockfd_put(sock);
}

static void ovpn_socket_release_work(struct work_struct *work)
{
	struct ovpn_socket *sock = container_of(work, struct ovpn_socket, work);

	ovpn_socket_detach(sock->sock);
	kfree_rcu(sock, rcu);
}

static void ovpn_socket_schedule_release(struct ovpn_socket *sock)
{
	INIT_WORK(&sock->work, ovpn_socket_release_work);
	schedule_work(&sock->work);
}

/**
 * ovpn_socket_release_kref - kref_put callback
 * @kref: the kref object
 */
void ovpn_socket_release_kref(struct kref *kref)
{
	struct ovpn_socket *sock = container_of(kref, struct ovpn_socket,
						refcount);

	ovpn_socket_schedule_release(sock);
}

static bool ovpn_socket_hold(struct ovpn_socket *sock)
{
	return kref_get_unless_zero(&sock->refcount);
}

static struct ovpn_socket *ovpn_socket_get(struct socket *sock)
{
	struct ovpn_socket *ovpn_sock;

	rcu_read_lock();
	ovpn_sock = rcu_dereference_sk_user_data(sock->sk);
	if (!ovpn_socket_hold(ovpn_sock)) {
		pr_warn("%s: found ovpn_socket with ref = 0\n", __func__);
		ovpn_sock = NULL;
	}
	rcu_read_unlock();

	return ovpn_sock;
}

static int ovpn_socket_attach(struct socket *sock, struct ovpn_peer *peer)
{
	int ret = -EOPNOTSUPP;

	if (!sock || !peer)
		return -EINVAL;

	if (sock->sk->sk_protocol == IPPROTO_UDP)
		ret = ovpn_udp_socket_attach(sock, peer->ovpn);
	else if (sock->sk->sk_protocol == IPPROTO_TCP)
		ret = ovpn_tcp_socket_attach(sock, peer);

	return ret;
}

/* Retrieve the corresponding ovpn object from a UDP socket
 * rcu_read_lock must be held on entry
 */
struct ovpn_struct *ovpn_from_udp_sock(struct sock *sk)
{
	struct ovpn_socket *ovpn_sock;

	if (unlikely(READ_ONCE(udp_sk(sk)->encap_type) != UDP_ENCAP_OVPNINUDP))
		return NULL;

	ovpn_sock = rcu_dereference_sk_user_data(sk);
	if (unlikely(!ovpn_sock))
		return NULL;

	/* make sure that sk matches our stored transport socket */
	if (unlikely(!ovpn_sock->sock || sk != ovpn_sock->sock->sk))
		return NULL;

	return ovpn_sock->ovpn;
}

/**
 * ovpn_socket_new - create a new socket and initialize it
 * @sock: the kernel socket to embed
 * @peer: the peer reachable via this socket
 *
 * Return: an openvpn socket on success or a negative error code otherwise
 */
struct ovpn_socket *ovpn_socket_new(struct socket *sock, struct ovpn_peer *peer)
{
	struct ovpn_socket *ovpn_sock;
	int ret;

	ret = ovpn_socket_attach(sock, peer);
	if (ret < 0 && ret != -EALREADY)
		return ERR_PTR(ret);

	/* if this socket is already owned by this interface, just increase the
	 * refcounter and use it as expected.
	 *
	 * Since UDP sockets can be used to talk to multiple remote endpoints,
	 * openvpn normally instantiates only one socket and shares it among all
	 * its peers. For this reason, when we find out that a socket is already
	 * used for some other peer in *this* instance, we can happily increase
	 * its refcounter and use it normally.
	 */
	if (ret == -EALREADY) {
		/* caller is expected to increase the sock refcounter before
		 * passing it to this function. For this reason we drop it if
		 * not needed, like when this socket is already owned.
		 */
		ovpn_sock = ovpn_socket_get(sock);
		sockfd_put(sock);
		return ovpn_sock;
	}

	ovpn_sock = kzalloc(sizeof(*ovpn_sock), GFP_KERNEL);
	if (!ovpn_sock) {
		ret = -ENOMEM;
		goto err;
	}

	ovpn_sock->sock = sock;
	kref_init(&ovpn_sock->refcount);

	/* TCP sockets are per-peer, therefore they are linked to their unique
	 * peer
	 */
	if (sock->sk->sk_protocol == IPPROTO_TCP) {
		ovpn_sock->peer = peer;
	} else {
		/* in UDP we only link the ovpn instance since the socket is
		 * shared among multiple peers
		 */
		ovpn_sock->ovpn = peer->ovpn;
	}

	rcu_assign_sk_user_data(sock->sk, ovpn_sock);

	return ovpn_sock;
err:
	ovpn_socket_detach(sock);
	return ERR_PTR(ret);
}
