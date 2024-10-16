// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/skbuff.h>
#include <net/hotdata.h>
#include <net/inet_common.h>
#include <net/tcp.h>
#include <net/route.h>
#include <trace/events/sock.h>

#include "ovpnstruct.h"
#include "main.h"
#include "io.h"
#include "packet.h"
#include "peer.h"
#include "proto.h"
#include "skb.h"
#include "tcp.h"

static struct proto ovpn_tcp_prot __ro_after_init;
static struct proto_ops ovpn_tcp_ops __ro_after_init;
static struct proto ovpn_tcp6_prot;
static struct proto_ops ovpn_tcp6_ops;
static DEFINE_MUTEX(tcp6_prot_mutex);

static int ovpn_tcp_parse(struct strparser *strp, struct sk_buff *skb)
{
	struct strp_msg *rxm = strp_msg(skb);
	__be16 blen;
	u16 len;
	int err;

	/* when packets are written to the TCP stream, they are prepended with
	 * two bytes indicating the actual packet size.
	 * Here we read those two bytes and move the skb data pointer to the
	 * beginning of the packet
	 */

	if (skb->len < rxm->offset + 2)
		return 0;

	err = skb_copy_bits(skb, rxm->offset, &blen, sizeof(blen));
	if (err < 0)
		return err;

	len = be16_to_cpu(blen);
	if (len < 2)
		return -EINVAL;

	return len + 2;
}

/* queue skb for sending to userspace via recvmsg on the socket */
static void ovpn_tcp_to_userspace(struct ovpn_socket *sock, struct sk_buff *skb)
{
	struct sock *sk = sock->sock->sk;

	skb_set_owner_r(skb, sk);
	memset(skb->cb, 0, sizeof(skb->cb));
	skb_queue_tail(&sock->peer->tcp.user_queue, skb);
	sock->peer->tcp.sk_cb.sk_data_ready(sk);
}

static void ovpn_tcp_rcv(struct strparser *strp, struct sk_buff *skb)
{
	struct ovpn_peer *peer = container_of(strp, struct ovpn_peer, tcp.strp);
	struct strp_msg *msg = strp_msg(skb);
	size_t pkt_len = msg->full_len - 2;
	size_t off = msg->offset + 2;

	/* ensure skb->data points to the beginning of the openvpn packet */
	if (!pskb_pull(skb, off)) {
		net_warn_ratelimited("%s: packet too small\n",
				     peer->ovpn->dev->name);
		goto err;
	}

	/* strparser does not trim the skb for us, therefore we do it now */
	if (pskb_trim(skb, pkt_len) != 0) {
		net_warn_ratelimited("%s: trimming skb failed\n",
				     peer->ovpn->dev->name);
		goto err;
	}

	/* we need the first byte of data to be accessible
	 * to extract the opcode and the key ID later on
	 */
	if (!pskb_may_pull(skb, 1)) {
		net_warn_ratelimited("%s: packet too small to fetch opcode\n",
				     peer->ovpn->dev->name);
		goto err;
	}

	/* DATA_V2 packets are handled in kernel, the rest goes to user space */
	if (likely(ovpn_opcode_from_skb(skb, 0) == OVPN_DATA_V2)) {
		/* hold reference to peer as required by ovpn_recv().
		 *
		 * NOTE: in this context we should already be holding a
		 * reference to this peer, therefore ovpn_peer_hold() is
		 * not expected to fail
		 */
		if (WARN_ON(!ovpn_peer_hold(peer)))
			goto err;

		ovpn_recv(peer, skb);
	} else {
		/* The packet size header must be there when sending the packet
		 * to userspace, therefore we put it back
		 */
		skb_push(skb, 2);
		ovpn_tcp_to_userspace(peer->sock, skb);
	}

	return;
err:
	netdev_err(peer->ovpn->dev,
		   "cannot process incoming TCP data for peer %u\n", peer->id);
	dev_core_stats_rx_dropped_inc(peer->ovpn->dev);
	kfree_skb(skb);
	ovpn_peer_del(peer, OVPN_DEL_PEER_REASON_TRANSPORT_ERROR);
}

static int ovpn_tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len,
			    int flags, int *addr_len)
{
	int err = 0, off, copied = 0, ret;
	struct ovpn_socket *sock;
	struct ovpn_peer *peer;
	struct sk_buff *skb;

	rcu_read_lock();
	sock = rcu_dereference_sk_user_data(sk);
	if (!sock || !sock->peer) {
		rcu_read_unlock();
		return -EBADF;
	}
	/* we take a reference to the peer linked to this TCP socket, because
	 * in turn the peer holds a reference to the socket itself.
	 * By doing so we also ensure that the peer stays alive along with
	 * the socket while executing this function
	 */
	ovpn_peer_hold(sock->peer);
	peer = sock->peer;
	rcu_read_unlock();

	skb = __skb_recv_datagram(sk, &peer->tcp.user_queue, flags, &off, &err);
	if (!skb) {
		if (err == -EAGAIN && sk->sk_shutdown & RCV_SHUTDOWN) {
			ret = 0;
			goto out;
		}
		ret = err;
		goto out;
	}

	copied = len;
	if (copied > skb->len)
		copied = skb->len;
	else if (copied < skb->len)
		msg->msg_flags |= MSG_TRUNC;

	err = skb_copy_datagram_msg(skb, 0, msg, copied);
	if (unlikely(err)) {
		kfree_skb(skb);
		ret = err;
		goto out;
	}

	if (flags & MSG_TRUNC)
		copied = skb->len;
	kfree_skb(skb);
	ret = copied;
out:
	ovpn_peer_put(peer);
	return ret;
}

void ovpn_tcp_socket_detach(struct socket *sock)
{
	struct ovpn_socket *ovpn_sock;
	struct ovpn_peer *peer;

	if (!sock)
		return;

	rcu_read_lock();
	ovpn_sock = rcu_dereference_sk_user_data(sock->sk);

	if (!ovpn_sock->peer) {
		rcu_read_unlock();
		return;
	}

	peer = ovpn_sock->peer;
	strp_stop(&peer->tcp.strp);

	skb_queue_purge(&peer->tcp.user_queue);

	/* restore CBs that were saved in ovpn_sock_set_tcp_cb() */
	sock->sk->sk_data_ready = peer->tcp.sk_cb.sk_data_ready;
	sock->sk->sk_write_space = peer->tcp.sk_cb.sk_write_space;
	sock->sk->sk_prot = peer->tcp.sk_cb.prot;
	sock->sk->sk_socket->ops = peer->tcp.sk_cb.ops;
	rcu_assign_sk_user_data(sock->sk, NULL);

	rcu_read_unlock();

	/* cancel any ongoing work. Done after removing the CBs so that these
	 * workers cannot be re-armed
	 */
	cancel_work_sync(&peer->tcp.tx_work);
	strp_done(&peer->tcp.strp);
}

static void ovpn_tcp_send_sock(struct ovpn_peer *peer)
{
	struct sk_buff *skb = peer->tcp.out_msg.skb;

	if (!skb)
		return;

	if (peer->tcp.tx_in_progress)
		return;

	peer->tcp.tx_in_progress = true;

	do {
		int ret = skb_send_sock_locked(peer->sock->sock->sk, skb,
					       peer->tcp.out_msg.offset,
					       peer->tcp.out_msg.len);
		if (unlikely(ret < 0)) {
			if (ret == -EAGAIN)
				goto out;

			net_warn_ratelimited("%s: TCP error to peer %u: %d\n",
					     peer->ovpn->dev->name, peer->id,
					     ret);

			/* in case of TCP error we can't recover the VPN
			 * stream therefore we abort the connection
			 */
			ovpn_peer_del(peer,
				      OVPN_DEL_PEER_REASON_TRANSPORT_ERROR);
			break;
		}

		peer->tcp.out_msg.len -= ret;
		peer->tcp.out_msg.offset += ret;
	} while (peer->tcp.out_msg.len > 0);

	if (!peer->tcp.out_msg.len)
		dev_sw_netstats_tx_add(peer->ovpn->dev, 1, skb->len);

	kfree_skb(peer->tcp.out_msg.skb);
	peer->tcp.out_msg.skb = NULL;
	peer->tcp.out_msg.len = 0;
	peer->tcp.out_msg.offset = 0;

out:
	peer->tcp.tx_in_progress = false;
}

static void ovpn_tcp_tx_work(struct work_struct *work)
{
	struct ovpn_peer *peer;

	peer = container_of(work, struct ovpn_peer, tcp.tx_work);

	lock_sock(peer->sock->sock->sk);
	ovpn_tcp_send_sock(peer);
	release_sock(peer->sock->sock->sk);
}

void ovpn_tcp_send_sock_skb(struct ovpn_peer *peer, struct sk_buff *skb)
{
	if (peer->tcp.out_msg.skb)
		return;

	peer->tcp.out_msg.skb = skb;
	peer->tcp.out_msg.len = skb->len;
	peer->tcp.out_msg.offset = 0;

	ovpn_tcp_send_sock(peer);
}

static int ovpn_tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
	struct ovpn_socket *sock;
	int ret, linear = PAGE_SIZE;
	struct ovpn_peer *peer;
	struct sk_buff *skb;

	rcu_read_lock();
	sock = rcu_dereference_sk_user_data(sk);
	peer = sock->peer;
	if (unlikely(!ovpn_peer_hold(peer))) {
		rcu_read_unlock();
		return -EIO;
	}
	rcu_read_unlock();

	if (msg->msg_flags & ~MSG_DONTWAIT) {
		ret = -EOPNOTSUPP;
		goto peer_free;
	}

	lock_sock(sk);

	if (peer->tcp.out_msg.skb) {
		ret = -EAGAIN;
		goto unlock;
	}

	if (size < linear)
		linear = size;

	skb = sock_alloc_send_pskb(sk, linear, size - linear,
				   msg->msg_flags & MSG_DONTWAIT, &ret, 0);
	if (!skb) {
		net_err_ratelimited("%s: skb alloc failed: %d\n",
				    sock->peer->ovpn->dev->name, ret);
		goto unlock;
	}

	skb_put(skb, linear);
	skb->len = size;
	skb->data_len = size - linear;

	ret = skb_copy_datagram_from_iter(skb, 0, &msg->msg_iter, size);
	if (ret) {
		kfree_skb(skb);
		net_err_ratelimited("%s: skb copy from iter failed: %d\n",
				    sock->peer->ovpn->dev->name, ret);
		goto unlock;
	}

	ovpn_tcp_send_sock_skb(sock->peer, skb);
	ret = size;
unlock:
	release_sock(sk);
peer_free:
	ovpn_peer_put(peer);
	return ret;
}

static void ovpn_tcp_data_ready(struct sock *sk)
{
	struct ovpn_socket *sock;

	trace_sk_data_ready(sk);

	rcu_read_lock();
	sock = rcu_dereference_sk_user_data(sk);
	strp_data_ready(&sock->peer->tcp.strp);
	rcu_read_unlock();
}

static void ovpn_tcp_write_space(struct sock *sk)
{
	struct ovpn_socket *sock;

	rcu_read_lock();
	sock = rcu_dereference_sk_user_data(sk);
	schedule_work(&sock->peer->tcp.tx_work);
	sock->peer->tcp.sk_cb.sk_write_space(sk);
	rcu_read_unlock();
}

static void ovpn_tcp_build_protos(struct proto *new_prot,
				  struct proto_ops *new_ops,
				  const struct proto *orig_prot,
				  const struct proto_ops *orig_ops);

/* Set TCP encapsulation callbacks */
int ovpn_tcp_socket_attach(struct socket *sock, struct ovpn_peer *peer)
{
	struct strp_callbacks cb = {
		.rcv_msg = ovpn_tcp_rcv,
		.parse_msg = ovpn_tcp_parse,
	};
	int ret;

	/* make sure no pre-existing encapsulation handler exists */
	if (sock->sk->sk_user_data)
		return -EBUSY;

	/* sanity check */
	if (sock->sk->sk_protocol != IPPROTO_TCP) {
		netdev_err(peer->ovpn->dev,
			   "provided socket is not TCP as expected\n");
		return -EINVAL;
	}

	/* only a fully connected socket are expected. Connection should be
	 * handled in userspace
	 */
	if (sock->sk->sk_state != TCP_ESTABLISHED) {
		netdev_err(peer->ovpn->dev,
			   "provided TCP socket is not in ESTABLISHED state: %d\n",
			   sock->sk->sk_state);
		return -EINVAL;
	}

	lock_sock(sock->sk);

	ret = strp_init(&peer->tcp.strp, sock->sk, &cb);
	if (ret < 0) {
		DEBUG_NET_WARN_ON_ONCE(1);
		release_sock(sock->sk);
		return ret;
	}

	INIT_WORK(&peer->tcp.tx_work, ovpn_tcp_tx_work);
	__sk_dst_reset(sock->sk);
	strp_check_rcv(&peer->tcp.strp);
	skb_queue_head_init(&peer->tcp.user_queue);

	/* save current CBs so that they can be restored upon socket release */
	peer->tcp.sk_cb.sk_data_ready = sock->sk->sk_data_ready;
	peer->tcp.sk_cb.sk_write_space = sock->sk->sk_write_space;
	peer->tcp.sk_cb.prot = sock->sk->sk_prot;
	peer->tcp.sk_cb.ops = sock->sk->sk_socket->ops;

	/* assign our static CBs and prot/ops */
	sock->sk->sk_data_ready = ovpn_tcp_data_ready;
	sock->sk->sk_write_space = ovpn_tcp_write_space;

	if (sock->sk->sk_family == AF_INET) {
		sock->sk->sk_prot = &ovpn_tcp_prot;
		sock->sk->sk_socket->ops = &ovpn_tcp_ops;
	} else {
		mutex_lock(&tcp6_prot_mutex);
		if (!ovpn_tcp6_prot.recvmsg)
			ovpn_tcp_build_protos(&ovpn_tcp6_prot, &ovpn_tcp6_ops,
					      sock->sk->sk_prot,
					      sock->sk->sk_socket->ops);
		mutex_unlock(&tcp6_prot_mutex);

		sock->sk->sk_prot = &ovpn_tcp6_prot;
		sock->sk->sk_socket->ops = &ovpn_tcp6_ops;
	}

	/* avoid using task_frag */
	sock->sk->sk_allocation = GFP_ATOMIC;
	sock->sk->sk_use_task_frag = false;

	release_sock(sock->sk);
	return 0;
}

static void ovpn_tcp_close(struct sock *sk, long timeout)
{
	struct ovpn_socket *sock;

	rcu_read_lock();
	sock = rcu_dereference_sk_user_data(sk);

	strp_stop(&sock->peer->tcp.strp);
	barrier();

	tcp_close(sk, timeout);

	ovpn_peer_del(sock->peer, OVPN_DEL_PEER_REASON_TRANSPORT_ERROR);
	rcu_read_unlock();
}

static __poll_t ovpn_tcp_poll(struct file *file, struct socket *sock,
			      poll_table *wait)
{
	__poll_t mask = datagram_poll(file, sock, wait);
	struct ovpn_socket *ovpn_sock;

	rcu_read_lock();
	ovpn_sock = rcu_dereference_sk_user_data(sock->sk);
	if (!skb_queue_empty(&ovpn_sock->peer->tcp.user_queue))
		mask |= EPOLLIN | EPOLLRDNORM;
	rcu_read_unlock();

	return mask;
}

static void ovpn_tcp_build_protos(struct proto *new_prot,
				  struct proto_ops *new_ops,
				  const struct proto *orig_prot,
				  const struct proto_ops *orig_ops)
{
	memcpy(new_prot, orig_prot, sizeof(*new_prot));
	memcpy(new_ops, orig_ops, sizeof(*new_ops));
	new_prot->recvmsg = ovpn_tcp_recvmsg;
	new_prot->sendmsg = ovpn_tcp_sendmsg;
	new_prot->close = ovpn_tcp_close;
	new_ops->poll = ovpn_tcp_poll;
}

/* Initialize TCP static objects */
void __init ovpn_tcp_init(void)
{
	ovpn_tcp_build_protos(&ovpn_tcp_prot, &ovpn_tcp_ops, &tcp_prot,
			      &inet_stream_ops);
}
