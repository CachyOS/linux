// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/netdevice.h>
#include <linux/types.h>
#include <net/genetlink.h>

#include <uapi/linux/ovpn.h>

#include "ovpnstruct.h"
#include "main.h"
#include "io.h"
#include "netlink.h"
#include "netlink-gen.h"
#include "bind.h"
#include "packet.h"
#include "peer.h"
#include "socket.h"

MODULE_ALIAS_GENL_FAMILY(OVPN_FAMILY_NAME);

/**
 * ovpn_get_dev_from_attrs - retrieve the ovpn private data from the netdevice
 *			     a netlink message is targeting
 * @net: network namespace where to look for the interface
 * @info: generic netlink info from the user request
 *
 * Return: the ovpn private data, if found, or an error otherwise
 */
static struct ovpn_struct *
ovpn_get_dev_from_attrs(struct net *net, const struct genl_info *info)
{
	struct ovpn_struct *ovpn;
	struct net_device *dev;
	int ifindex;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_IFINDEX))
		return ERR_PTR(-EINVAL);

	ifindex = nla_get_u32(info->attrs[OVPN_A_IFINDEX]);

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, ifindex);
	if (!dev) {
		rcu_read_unlock();
		NL_SET_ERR_MSG_MOD(info->extack,
				   "ifindex does not match any interface");
		return ERR_PTR(-ENODEV);
	}

	if (!ovpn_dev_is_valid(dev)) {
		rcu_read_unlock();
		NL_SET_ERR_MSG_MOD(info->extack,
				   "specified interface is not ovpn");
		NL_SET_BAD_ATTR(info->extack, info->attrs[OVPN_A_IFINDEX]);
		return ERR_PTR(-EINVAL);
	}

	ovpn = netdev_priv(dev);
	netdev_hold(dev, &ovpn->dev_tracker, GFP_KERNEL);
	rcu_read_unlock();

	return ovpn;
}

int ovpn_nl_pre_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		     struct genl_info *info)
{
	struct ovpn_struct *ovpn = ovpn_get_dev_from_attrs(genl_info_net(info),
							   info);

	if (IS_ERR(ovpn))
		return PTR_ERR(ovpn);

	info->user_ptr[0] = ovpn;

	return 0;
}

void ovpn_nl_post_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		       struct genl_info *info)
{
	struct ovpn_struct *ovpn = info->user_ptr[0];

	if (ovpn)
		netdev_put(ovpn->dev, &ovpn->dev_tracker);
}

static int ovpn_nl_attr_sockaddr_remote(struct nlattr **attrs,
					struct sockaddr_storage *ss)
{
	struct sockaddr_in6 *sin6;
	struct sockaddr_in *sin;
	struct in6_addr *in6;
	__be16 port = 0;
	__be32 *in;
	int af;

	ss->ss_family = AF_UNSPEC;

	if (attrs[OVPN_A_PEER_REMOTE_PORT])
		port = nla_get_be16(attrs[OVPN_A_PEER_REMOTE_PORT]);

	if (attrs[OVPN_A_PEER_REMOTE_IPV4]) {
		af = AF_INET;
		ss->ss_family = AF_INET;
		in = nla_data(attrs[OVPN_A_PEER_REMOTE_IPV4]);
	} else if (attrs[OVPN_A_PEER_REMOTE_IPV6]) {
		af = AF_INET6;
		ss->ss_family = AF_INET6;
		in6 = nla_data(attrs[OVPN_A_PEER_REMOTE_IPV6]);
	} else {
		return AF_UNSPEC;
	}

	switch (ss->ss_family) {
	case AF_INET6:
		/* If this is a regular IPv6 just break and move on,
		 * otherwise switch to AF_INET and extract the IPv4 accordingly
		 */
		if (!ipv6_addr_v4mapped(in6)) {
			sin6 = (struct sockaddr_in6 *)ss;
			sin6->sin6_port = port;
			memcpy(&sin6->sin6_addr, in6, sizeof(*in6));
			break;
		}

		/* v4-mapped-v6 address */
		ss->ss_family = AF_INET;
		in = &in6->s6_addr32[3];
		fallthrough;
	case AF_INET:
		sin = (struct sockaddr_in *)ss;
		sin->sin_port = port;
		sin->sin_addr.s_addr = *in;
		break;
	}

	/* don't return ss->ss_family as it may have changed in case of
	 * v4-mapped-v6 address
	 */
	return af;
}

static u8 *ovpn_nl_attr_local_ip(struct nlattr **attrs)
{
	u8 *addr6;

	if (!attrs[OVPN_A_PEER_LOCAL_IPV4] && !attrs[OVPN_A_PEER_LOCAL_IPV6])
		return NULL;

	if (attrs[OVPN_A_PEER_LOCAL_IPV4])
		return nla_data(attrs[OVPN_A_PEER_LOCAL_IPV4]);

	addr6 = nla_data(attrs[OVPN_A_PEER_LOCAL_IPV6]);
	/* this is an IPv4-mapped IPv6 address, therefore extract the actual
	 * v4 address from the last 4 bytes
	 */
	if (ipv6_addr_v4mapped((struct in6_addr *)addr6))
		return addr6 + 12;

	return addr6;
}

static int ovpn_nl_peer_precheck(struct ovpn_struct *ovpn,
				 struct genl_info *info,
				 struct nlattr **attrs)
{
	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_PEER], attrs,
			      OVPN_A_PEER_ID))
		return -EINVAL;

	if (attrs[OVPN_A_PEER_REMOTE_IPV4] && attrs[OVPN_A_PEER_REMOTE_IPV6]) {
		NL_SET_ERR_MSG_MOD(info->extack,
				   "cannot specify both remote IPv4 or IPv6 address");
		return -EINVAL;
	}

	if (!attrs[OVPN_A_PEER_REMOTE_IPV4] &&
	    !attrs[OVPN_A_PEER_REMOTE_IPV6] && attrs[OVPN_A_PEER_REMOTE_PORT]) {
		NL_SET_ERR_MSG_MOD(info->extack,
				   "cannot specify remote port without IP address");
		return -EINVAL;
	}

	if (!attrs[OVPN_A_PEER_REMOTE_IPV4] &&
	    attrs[OVPN_A_PEER_LOCAL_IPV4]) {
		NL_SET_ERR_MSG_MOD(info->extack,
				   "cannot specify local IPv4 address without remote");
		return -EINVAL;
	}

	if (!attrs[OVPN_A_PEER_REMOTE_IPV6] &&
	    attrs[OVPN_A_PEER_LOCAL_IPV6]) {
		NL_SET_ERR_MSG_MOD(info->extack,
				   "cannot specify local IPV6 address without remote");
		return -EINVAL;
	}

	if (!attrs[OVPN_A_PEER_REMOTE_IPV6] &&
	    attrs[OVPN_A_PEER_REMOTE_IPV6_SCOPE_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack,
				   "cannot specify scope id without remote IPv6 address");
		return -EINVAL;
	}

	/* VPN IPs are needed only in MP mode for selecting the right peer */
	if (ovpn->mode == OVPN_MODE_P2P && (attrs[OVPN_A_PEER_VPN_IPV4] ||
					    attrs[OVPN_A_PEER_VPN_IPV6])) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "VPN IP unexpected in P2P mode");
		return -EINVAL;
	}

	if ((attrs[OVPN_A_PEER_KEEPALIVE_INTERVAL] &&
	     !attrs[OVPN_A_PEER_KEEPALIVE_TIMEOUT]) ||
	    (!attrs[OVPN_A_PEER_KEEPALIVE_INTERVAL] &&
	     attrs[OVPN_A_PEER_KEEPALIVE_TIMEOUT])) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "keepalive interval and timeout are required together");
		return -EINVAL;
	}

	return 0;
}

/**
 * ovpn_nl_peer_modify - modify the peer attributes according to the incoming msg
 * @peer: the peer to modify
 * @info: generic netlink info from the user request
 * @attrs: the attributes from the user request
 *
 * Return: a negative error code in case of failure, 0 on success or 1 on
 *	   success and the VPN IPs have been modified (requires rehashing in MP
 *	   mode)
 */
static int ovpn_nl_peer_modify(struct ovpn_peer *peer, struct genl_info *info,
			       struct nlattr **attrs)
{
	struct sockaddr_storage ss = {};
	u32 sockfd, interv, timeout;
	struct socket *sock = NULL;
	u8 *local_ip = NULL;
	bool rehash = false;
	int ret;

	if (attrs[OVPN_A_PEER_SOCKET]) {
		/* lookup the fd in the kernel table and extract the socket
		 * object
		 */
		sockfd = nla_get_u32(attrs[OVPN_A_PEER_SOCKET]);
		/* sockfd_lookup() increases sock's refcounter */
		sock = sockfd_lookup(sockfd, &ret);
		if (!sock) {
			NL_SET_ERR_MSG_FMT_MOD(info->extack,
					       "cannot lookup peer socket (fd=%u): %d",
					       sockfd, ret);
			return -ENOTSOCK;
		}

		/* Only when using UDP as transport protocol the remote endpoint
		 * can be configured so that ovpn knows where to send packets
		 * to.
		 *
		 * In case of TCP, the socket is connected to the peer and ovpn
		 * will just send bytes over it, without the need to specify a
		 * destination.
		 */
		if (sock->sk->sk_protocol != IPPROTO_UDP &&
		    (attrs[OVPN_A_PEER_REMOTE_IPV4] ||
		     attrs[OVPN_A_PEER_REMOTE_IPV6])) {
			NL_SET_ERR_MSG_FMT_MOD(info->extack,
					       "unexpected remote IP address for non UDP socket");
			sockfd_put(sock);
			return -EINVAL;
		}

		if (peer->sock)
			ovpn_socket_put(peer->sock);

		peer->sock = ovpn_socket_new(sock, peer);
		if (IS_ERR(peer->sock)) {
			NL_SET_ERR_MSG_FMT_MOD(info->extack,
					       "cannot encapsulate socket: %ld",
					       PTR_ERR(peer->sock));
			sockfd_put(sock);
			peer->sock = NULL;
			return -ENOTSOCK;
		}
	}

	if (ovpn_nl_attr_sockaddr_remote(attrs, &ss) != AF_UNSPEC) {
		/* we carry the local IP in a generic container.
		 * ovpn_peer_reset_sockaddr() will properly interpret it
		 * based on ss.ss_family
		 */
		local_ip = ovpn_nl_attr_local_ip(attrs);

		spin_lock_bh(&peer->lock);
		/* set peer sockaddr */
		ret = ovpn_peer_reset_sockaddr(peer, &ss, local_ip);
		if (ret < 0) {
			NL_SET_ERR_MSG_FMT_MOD(info->extack,
					       "cannot set peer sockaddr: %d",
					       ret);
			spin_unlock_bh(&peer->lock);
			return ret;
		}
		spin_unlock_bh(&peer->lock);
	}

	if (attrs[OVPN_A_PEER_VPN_IPV4]) {
		rehash = true;
		peer->vpn_addrs.ipv4.s_addr =
			nla_get_in_addr(attrs[OVPN_A_PEER_VPN_IPV4]);
	}

	if (attrs[OVPN_A_PEER_VPN_IPV6]) {
		rehash = true;
		peer->vpn_addrs.ipv6 =
			nla_get_in6_addr(attrs[OVPN_A_PEER_VPN_IPV6]);
	}

	/* when setting the keepalive, both parameters have to be configured */
	if (attrs[OVPN_A_PEER_KEEPALIVE_INTERVAL] &&
	    attrs[OVPN_A_PEER_KEEPALIVE_TIMEOUT]) {
		interv = nla_get_u32(attrs[OVPN_A_PEER_KEEPALIVE_INTERVAL]);
		timeout = nla_get_u32(attrs[OVPN_A_PEER_KEEPALIVE_TIMEOUT]);
		ovpn_peer_keepalive_set(peer, interv, timeout);
	}

	netdev_dbg(peer->ovpn->dev,
		   "%s: peer id=%u endpoint=%pIScp/%s VPN-IPv4=%pI4 VPN-IPv6=%pI6c\n",
		   __func__, peer->id, &ss,
		   peer->sock->sock->sk->sk_prot_creator->name,
		   &peer->vpn_addrs.ipv4.s_addr, &peer->vpn_addrs.ipv6);

	return rehash ? 1 : 0;
}

int ovpn_nl_peer_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[OVPN_A_PEER_MAX + 1];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_PEER))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_PEER_MAX, info->attrs[OVPN_A_PEER],
			       ovpn_peer_nl_policy, info->extack);
	if (ret)
		return ret;

	ret = ovpn_nl_peer_precheck(ovpn, info, attrs);
	if (ret < 0)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_PEER], attrs,
			      OVPN_A_PEER_SOCKET))
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);
	peer = ovpn_peer_new(ovpn, peer_id);
	if (IS_ERR(peer)) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "cannot create new peer object for peer %u: %ld",
				       peer_id, PTR_ERR(peer));
		return PTR_ERR(peer);
	}

	ret = ovpn_nl_peer_modify(peer, info, attrs);
	if (ret < 0)
		goto peer_release;

	ret = ovpn_peer_add(ovpn, peer);
	if (ret < 0) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "cannot add new peer (id=%u) to hashtable: %d\n",
				       peer->id, ret);
		goto peer_release;
	}

	return 0;

peer_release:
	/* release right away because peer is not used in any context */
	ovpn_peer_release(peer);

	return ret;
}

int ovpn_nl_peer_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[OVPN_A_PEER_MAX + 1];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_PEER))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_PEER_MAX, info->attrs[OVPN_A_PEER],
			       ovpn_peer_nl_policy, info->extack);
	if (ret)
		return ret;

	ret = ovpn_nl_peer_precheck(ovpn, info, attrs);
	if (ret < 0)
		return ret;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);
	peer = ovpn_peer_get_by_id(ovpn, peer_id);
	if (!peer)
		return -ENOENT;

	ret = ovpn_nl_peer_modify(peer, info, attrs);
	if (ret < 0) {
		ovpn_peer_put(peer);
		return ret;
	}

	/* ret == 1 means that VPN IPv4/6 has been modified and rehashing
	 * is required
	 */
	if (ret > 0) {
		spin_lock_bh(&ovpn->peers->lock);
		ovpn_peer_hash_vpn_ip(peer);
		spin_unlock_bh(&ovpn->peers->lock);
	}

	ovpn_peer_put(peer);

	return 0;
}

static int ovpn_nl_send_peer(struct sk_buff *skb, const struct genl_info *info,
			     const struct ovpn_peer *peer, u32 portid, u32 seq,
			     int flags)
{
	const struct ovpn_bind *bind;
	struct nlattr *attr;
	void *hdr;

	hdr = genlmsg_put(skb, portid, seq, &ovpn_nl_family, flags,
			  OVPN_CMD_PEER_GET);
	if (!hdr)
		return -ENOBUFS;

	attr = nla_nest_start(skb, OVPN_A_PEER);
	if (!attr)
		goto err;

	if (nla_put_u32(skb, OVPN_A_PEER_ID, peer->id))
		goto err;

	if (peer->vpn_addrs.ipv4.s_addr != htonl(INADDR_ANY))
		if (nla_put_in_addr(skb, OVPN_A_PEER_VPN_IPV4,
				    peer->vpn_addrs.ipv4.s_addr))
			goto err;

	if (!ipv6_addr_equal(&peer->vpn_addrs.ipv6, &in6addr_any))
		if (nla_put_in6_addr(skb, OVPN_A_PEER_VPN_IPV6,
				     &peer->vpn_addrs.ipv6))
			goto err;

	if (nla_put_u32(skb, OVPN_A_PEER_KEEPALIVE_INTERVAL,
			peer->keepalive_interval) ||
	    nla_put_u32(skb, OVPN_A_PEER_KEEPALIVE_TIMEOUT,
			peer->keepalive_timeout))
		goto err;

	rcu_read_lock();
	bind = rcu_dereference(peer->bind);
	if (bind) {
		if (bind->remote.in4.sin_family == AF_INET) {
			if (nla_put_in_addr(skb, OVPN_A_PEER_REMOTE_IPV4,
					    bind->remote.in4.sin_addr.s_addr) ||
			    nla_put_net16(skb, OVPN_A_PEER_REMOTE_PORT,
					  bind->remote.in4.sin_port) ||
			    nla_put_in_addr(skb, OVPN_A_PEER_LOCAL_IPV4,
					    bind->local.ipv4.s_addr))
				goto err_unlock;
		} else if (bind->remote.in4.sin_family == AF_INET6) {
			if (nla_put_in6_addr(skb, OVPN_A_PEER_REMOTE_IPV6,
					     &bind->remote.in6.sin6_addr) ||
			    nla_put_u32(skb, OVPN_A_PEER_REMOTE_IPV6_SCOPE_ID,
					bind->remote.in6.sin6_scope_id) ||
			    nla_put_net16(skb, OVPN_A_PEER_REMOTE_PORT,
					  bind->remote.in6.sin6_port) ||
			    nla_put_in6_addr(skb, OVPN_A_PEER_LOCAL_IPV6,
					     &bind->local.ipv6))
				goto err_unlock;
		}
	}
	rcu_read_unlock();

	if (nla_put_net16(skb, OVPN_A_PEER_LOCAL_PORT,
			  inet_sk(peer->sock->sock->sk)->inet_sport) ||
	    /* VPN RX stats */
	    nla_put_uint(skb, OVPN_A_PEER_VPN_RX_BYTES,
			 atomic64_read(&peer->vpn_stats.rx.bytes)) ||
	    nla_put_uint(skb, OVPN_A_PEER_VPN_RX_PACKETS,
			 atomic64_read(&peer->vpn_stats.rx.packets)) ||
	    /* VPN TX stats */
	    nla_put_uint(skb, OVPN_A_PEER_VPN_TX_BYTES,
			 atomic64_read(&peer->vpn_stats.tx.bytes)) ||
	    nla_put_uint(skb, OVPN_A_PEER_VPN_TX_PACKETS,
			 atomic64_read(&peer->vpn_stats.tx.packets)) ||
	    /* link RX stats */
	    nla_put_uint(skb, OVPN_A_PEER_LINK_RX_BYTES,
			 atomic64_read(&peer->link_stats.rx.bytes)) ||
	    nla_put_uint(skb, OVPN_A_PEER_LINK_RX_PACKETS,
			 atomic64_read(&peer->link_stats.rx.packets)) ||
	    /* link TX stats */
	    nla_put_uint(skb, OVPN_A_PEER_LINK_TX_BYTES,
			 atomic64_read(&peer->link_stats.tx.bytes)) ||
	    nla_put_uint(skb, OVPN_A_PEER_LINK_TX_PACKETS,
			 atomic64_read(&peer->link_stats.tx.packets)))
		goto err;

	nla_nest_end(skb, attr);
	genlmsg_end(skb, hdr);

	return 0;
err_unlock:
	rcu_read_unlock();
err:
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

int ovpn_nl_peer_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[OVPN_A_PEER_MAX + 1];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	struct sk_buff *msg;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_PEER))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_PEER_MAX, info->attrs[OVPN_A_PEER],
			       ovpn_peer_nl_policy, info->extack);
	if (ret)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_PEER], attrs,
			      OVPN_A_PEER_ID))
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);
	peer = ovpn_peer_get_by_id(ovpn, peer_id);
	if (!peer) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "cannot find peer with id %u", peer_id);
		return -ENOENT;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}

	ret = ovpn_nl_send_peer(msg, info, peer, info->snd_portid,
				info->snd_seq, 0);
	if (ret < 0) {
		nlmsg_free(msg);
		goto err;
	}

	ret = genlmsg_reply(msg, info);
err:
	ovpn_peer_put(peer);
	return ret;
}

int ovpn_nl_peer_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	int bkt, last_idx = cb->args[1], dumped = 0;
	struct ovpn_struct *ovpn;
	struct ovpn_peer *peer;

	ovpn = ovpn_get_dev_from_attrs(sock_net(cb->skb->sk), info);
	if (IS_ERR(ovpn))
		return PTR_ERR(ovpn);

	if (ovpn->mode == OVPN_MODE_P2P) {
		/* if we already dumped a peer it means we are done */
		if (last_idx)
			goto out;

		rcu_read_lock();
		peer = rcu_dereference(ovpn->peer);
		if (peer) {
			if (ovpn_nl_send_peer(skb, info, peer,
					      NETLINK_CB(cb->skb).portid,
					      cb->nlh->nlmsg_seq,
					      NLM_F_MULTI) == 0)
				dumped++;
		}
		rcu_read_unlock();
	} else {
		rcu_read_lock();
		hash_for_each_rcu(ovpn->peers->by_id, bkt, peer,
				  hash_entry_id) {
			/* skip already dumped peers that were dumped by
			 * previous invocations
			 */
			if (last_idx > 0) {
				last_idx--;
				continue;
			}

			if (ovpn_nl_send_peer(skb, info, peer,
					      NETLINK_CB(cb->skb).portid,
					      cb->nlh->nlmsg_seq,
					      NLM_F_MULTI) < 0)
				break;

			/* count peers being dumped during this invocation */
			dumped++;
		}
		rcu_read_unlock();
	}

out:
	netdev_put(ovpn->dev, &ovpn->dev_tracker);

	/* sum up peers dumped in this message, so that at the next invocation
	 * we can continue from where we left
	 */
	cb->args[1] += dumped;
	return skb->len;
}

int ovpn_nl_peer_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[OVPN_A_PEER_MAX + 1];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_PEER))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_PEER_MAX, info->attrs[OVPN_A_PEER],
			       ovpn_peer_nl_policy, info->extack);
	if (ret)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_PEER], attrs,
			      OVPN_A_PEER_ID))
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);

	peer = ovpn_peer_get_by_id(ovpn, peer_id);
	if (!peer)
		return -ENOENT;

	netdev_dbg(ovpn->dev, "%s: peer id=%u\n", __func__, peer->id);
	ret = ovpn_peer_del(peer, OVPN_DEL_PEER_REASON_USERSPACE);
	ovpn_peer_put(peer);

	return ret;
}

static int ovpn_nl_get_key_dir(struct genl_info *info, struct nlattr *key,
			       enum ovpn_cipher_alg cipher,
			       struct ovpn_key_direction *dir)
{
	struct nlattr *attrs[OVPN_A_KEYDIR_MAX + 1];
	int ret;

	ret = nla_parse_nested(attrs, OVPN_A_KEYDIR_MAX, key,
			       ovpn_keydir_nl_policy, info->extack);
	if (ret)
		return ret;

	switch (cipher) {
	case OVPN_CIPHER_ALG_AES_GCM:
	case OVPN_CIPHER_ALG_CHACHA20_POLY1305:
		if (NL_REQ_ATTR_CHECK(info->extack, key, attrs,
				      OVPN_A_KEYDIR_CIPHER_KEY) ||
		    NL_REQ_ATTR_CHECK(info->extack, key, attrs,
				      OVPN_A_KEYDIR_NONCE_TAIL))
			return -EINVAL;

		dir->cipher_key = nla_data(attrs[OVPN_A_KEYDIR_CIPHER_KEY]);
		dir->cipher_key_size = nla_len(attrs[OVPN_A_KEYDIR_CIPHER_KEY]);

		/* These algorithms require a 96bit nonce,
		 * Construct it by combining 4-bytes packet id and
		 * 8-bytes nonce-tail from userspace
		 */
		dir->nonce_tail = nla_data(attrs[OVPN_A_KEYDIR_NONCE_TAIL]);
		dir->nonce_tail_size = nla_len(attrs[OVPN_A_KEYDIR_NONCE_TAIL]);
		break;
	default:
		NL_SET_ERR_MSG_MOD(info->extack, "unsupported cipher");
		return -EINVAL;
	}

	return 0;
}

/**
 * ovpn_nl_key_new_doit - configure a new key for the specified peer
 * @skb: incoming netlink message
 * @info: genetlink metadata
 *
 * This function allows the user to install a new key in the peer crypto
 * state.
 * Each peer has two 'slots', namely 'primary' and 'secondary', where
 * keys can be installed. The key in the 'primary' slot is used for
 * encryption, while both keys can be used for decryption by matching the
 * key ID carried in the incoming packet.
 *
 * The user is responsible for rotating keys when necessary. The user
 * may fetch peer traffic statistics via netlink in order to better
 * identify the right time to rotate keys.
 * The renegotiation follows these steps:
 * 1. a new key is computed by the user and is installed in the 'secondary'
 *    slot
 * 2. at user discretion (usually after a predetermined time) 'primary' and
 *    'secondary' contents are swapped and the new key starts being used for
 *    encryption, while the old key is kept around for decryption of late
 *    packets.
 *
 * Return: 0 on success or a negative error code otherwise.
 */
int ovpn_nl_key_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[OVPN_A_KEYCONF_MAX + 1];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer_key_reset pkr;
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_KEYCONF))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_KEYCONF_MAX,
			       info->attrs[OVPN_A_KEYCONF],
			       ovpn_keyconf_nl_policy, info->extack);
	if (ret)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_PEER_ID))
		return -EINVAL;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_SLOT) ||
	    NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_KEY_ID) ||
	    NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_CIPHER_ALG) ||
	    NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_ENCRYPT_DIR) ||
	    NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_DECRYPT_DIR))
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_KEYCONF_PEER_ID]);
	pkr.slot = nla_get_u8(attrs[OVPN_A_KEYCONF_SLOT]);
	pkr.key.key_id = nla_get_u16(attrs[OVPN_A_KEYCONF_KEY_ID]);
	pkr.key.cipher_alg = nla_get_u16(attrs[OVPN_A_KEYCONF_CIPHER_ALG]);

	ret = ovpn_nl_get_key_dir(info, attrs[OVPN_A_KEYCONF_ENCRYPT_DIR],
				  pkr.key.cipher_alg, &pkr.key.encrypt);
	if (ret < 0)
		return ret;

	ret = ovpn_nl_get_key_dir(info, attrs[OVPN_A_KEYCONF_DECRYPT_DIR],
				  pkr.key.cipher_alg, &pkr.key.decrypt);
	if (ret < 0)
		return ret;

	peer = ovpn_peer_get_by_id(ovpn, peer_id);
	if (!peer) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "no peer with id %u to set key for",
				       peer_id);
		return -ENOENT;
	}

	ret = ovpn_crypto_state_reset(&peer->crypto, &pkr);
	if (ret < 0) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "cannot install new key for peer %u",
				       peer_id);
		goto out;
	}

	netdev_dbg(ovpn->dev, "%s: new key installed (id=%u) for peer %u\n",
		   __func__, pkr.key.key_id, peer_id);
out:
	ovpn_peer_put(peer);
	return ret;
}

int ovpn_nl_key_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int ovpn_nl_key_swap_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct nlattr *attrs[OVPN_A_PEER_MAX + 1];
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_KEYCONF))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_KEYCONF_MAX,
			       info->attrs[OVPN_A_KEYCONF],
			       ovpn_keyconf_nl_policy, info->extack);
	if (ret)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_PEER_ID))
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_KEYCONF_PEER_ID]);

	peer = ovpn_peer_get_by_id(ovpn, peer_id);
	if (!peer) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "no peer with id %u to swap keys for",
				       peer_id);
		return -ENOENT;
	}

	ovpn_crypto_key_slots_swap(&peer->crypto);
	ovpn_peer_put(peer);

	return 0;
}

int ovpn_nl_key_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[OVPN_A_KEYCONF_MAX + 1];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	enum ovpn_key_slot slot;
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, OVPN_A_KEYCONF))
		return -EINVAL;

	ret = nla_parse_nested(attrs, OVPN_A_KEYCONF_MAX,
			       info->attrs[OVPN_A_KEYCONF],
			       ovpn_keyconf_nl_policy, info->extack);
	if (ret)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_PEER_ID))
		return -EINVAL;

	if (ret)
		return ret;

	if (NL_REQ_ATTR_CHECK(info->extack, info->attrs[OVPN_A_KEYCONF], attrs,
			      OVPN_A_KEYCONF_SLOT))
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_KEYCONF_PEER_ID]);
	slot = nla_get_u8(attrs[OVPN_A_KEYCONF_SLOT]);

	peer = ovpn_peer_get_by_id(ovpn, peer_id);
	if (!peer) {
		NL_SET_ERR_MSG_FMT_MOD(info->extack,
				       "no peer with id %u to delete key for",
				       peer_id);
		return -ENOENT;
	}

	ovpn_crypto_key_slot_delete(&peer->crypto, slot);
	ovpn_peer_put(peer);

	return 0;
}

/**
 * ovpn_nl_key_swap_notify - notify userspace peer's key must be renewed
 * @peer: the peer whose key needs to be renewed
 * @key_id: the ID of the key that needs to be renewed
 *
 * Return: 0 on success or a negative error code otherwise
 */
int ovpn_nl_key_swap_notify(struct ovpn_peer *peer, u8 key_id)
{
	struct nlattr *k_attr;
	struct sk_buff *msg;
	int ret = -EMSGSIZE;
	void *hdr;

	netdev_info(peer->ovpn->dev, "peer with id %u must rekey - primary key unusable.\n",
		    peer->id);

	msg = nlmsg_new(100, GFP_ATOMIC);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, 0, 0, &ovpn_nl_family, 0, OVPN_CMD_KEY_SWAP_NTF);
	if (!hdr) {
		ret = -ENOBUFS;
		goto err_free_msg;
	}

	if (nla_put_u32(msg, OVPN_A_IFINDEX, peer->ovpn->dev->ifindex))
		goto err_cancel_msg;

	k_attr = nla_nest_start(msg, OVPN_A_KEYCONF);
	if (!k_attr)
		goto err_cancel_msg;

	if (nla_put_u32(msg, OVPN_A_KEYCONF_PEER_ID, peer->id))
		goto err_cancel_msg;

	if (nla_put_u16(msg, OVPN_A_KEYCONF_KEY_ID, key_id))
		goto err_cancel_msg;

	nla_nest_end(msg, k_attr);
	genlmsg_end(msg, hdr);

	genlmsg_multicast_netns(&ovpn_nl_family, dev_net(peer->ovpn->dev), msg,
				0, OVPN_NLGRP_PEERS, GFP_ATOMIC);

	return 0;

err_cancel_msg:
	genlmsg_cancel(msg, hdr);
err_free_msg:
	nlmsg_free(msg);
	return ret;
}

/**
 * ovpn_nl_register - perform any needed registration in the NL subsustem
 *
 * Return: 0 on success, a negative error code otherwise
 */
int __init ovpn_nl_register(void)
{
	int ret = genl_register_family(&ovpn_nl_family);

	if (ret) {
		pr_err("ovpn: genl_register_family failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/**
 * ovpn_nl_unregister - undo any module wide netlink registration
 */
void ovpn_nl_unregister(void)
{
	genl_unregister_family(&ovpn_nl_family);
}
