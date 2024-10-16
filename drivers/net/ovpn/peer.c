// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <net/ip6_route.h>

#include "ovpnstruct.h"
#include "bind.h"
#include "pktid.h"
#include "crypto.h"
#include "io.h"
#include "main.h"
#include "netlink.h"
#include "peer.h"
#include "socket.h"

/**
 * ovpn_peer_keepalive_set - configure keepalive values for peer
 * @peer: the peer to configure
 * @interval: outgoing keepalive interval
 * @timeout: incoming keepalive timeout
 */
void ovpn_peer_keepalive_set(struct ovpn_peer *peer, u32 interval, u32 timeout)
{
	time64_t now = ktime_get_real_seconds();

	netdev_dbg(peer->ovpn->dev,
		   "%s: scheduling keepalive for peer %u: interval=%u timeout=%u\n",
		   __func__, peer->id, interval, timeout);

	peer->keepalive_interval = interval;
	peer->last_sent = now;
	peer->keepalive_xmit_exp = now + interval;

	peer->keepalive_timeout = timeout;
	peer->last_recv = now;
	peer->keepalive_recv_exp = now + timeout;

	/* now that interval and timeout have been changed, kick
	 * off the worker so that the next delay can be recomputed
	 */
	mod_delayed_work(system_wq, &peer->ovpn->keepalive_work, 0);
}

/**
 * ovpn_peer_new - allocate and initialize a new peer object
 * @ovpn: the openvpn instance inside which the peer should be created
 * @id: the ID assigned to this peer
 *
 * Return: a pointer to the new peer on success or an error code otherwise
 */
struct ovpn_peer *ovpn_peer_new(struct ovpn_struct *ovpn, u32 id)
{
	struct ovpn_peer *peer;
	int ret;

	/* alloc and init peer object */
	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	peer->id = id;
	peer->halt = false;
	peer->ovpn = ovpn;

	peer->vpn_addrs.ipv4.s_addr = htonl(INADDR_ANY);
	peer->vpn_addrs.ipv6 = in6addr_any;

	RCU_INIT_POINTER(peer->bind, NULL);
	ovpn_crypto_state_init(&peer->crypto);
	spin_lock_init(&peer->lock);
	kref_init(&peer->refcount);
	ovpn_peer_stats_init(&peer->vpn_stats);
	ovpn_peer_stats_init(&peer->link_stats);

	ret = dst_cache_init(&peer->dst_cache, GFP_KERNEL);
	if (ret < 0) {
		netdev_err(ovpn->dev, "%s: cannot initialize dst cache\n",
			   __func__);
		kfree(peer);
		return ERR_PTR(ret);
	}

	netdev_hold(ovpn->dev, &ovpn->dev_tracker, GFP_KERNEL);

	return peer;
}

/**
 * ovpn_peer_reset_sockaddr - recreate binding for peer
 * @peer: peer to recreate the binding for
 * @ss: sockaddr to use as remote endpoint for the binding
 * @local_ip: local IP for the binding
 *
 * Return: 0 on success or a negative error code otherwise
 */
int ovpn_peer_reset_sockaddr(struct ovpn_peer *peer,
			     const struct sockaddr_storage *ss,
			     const u8 *local_ip)
	__must_hold(&peer->lock)
{
	struct ovpn_bind *bind;
	size_t ip_len;

	/* create new ovpn_bind object */
	bind = ovpn_bind_from_sockaddr(ss);
	if (IS_ERR(bind))
		return PTR_ERR(bind);

	if (local_ip) {
		if (ss->ss_family == AF_INET) {
			ip_len = sizeof(struct in_addr);
		} else if (ss->ss_family == AF_INET6) {
			ip_len = sizeof(struct in6_addr);
		} else {
			netdev_dbg(peer->ovpn->dev, "%s: invalid family for remote endpoint\n",
				   __func__);
			kfree(bind);
			return -EINVAL;
		}

		memcpy(&bind->local, local_ip, ip_len);
	}

	/* set binding */
	ovpn_bind_reset(peer, bind);

	return 0;
}

#define ovpn_get_hash_head(_tbl, _key, _key_len) ({		\
	typeof(_tbl) *__tbl = &(_tbl);				\
	(&(*__tbl)[jhash(_key, _key_len, 0) % HASH_SIZE(*__tbl)]); }) \

/**
 * ovpn_peer_float - update remote endpoint for peer
 * @peer: peer to update the remote endpoint for
 * @skb: incoming packet to retrieve the source address (remote) from
 */
void ovpn_peer_float(struct ovpn_peer *peer, struct sk_buff *skb)
{
	struct hlist_nulls_head *nhead;
	struct sockaddr_storage ss;
	const u8 *local_ip = NULL;
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa;
	struct ovpn_bind *bind;
	sa_family_t family;
	size_t salen;

	rcu_read_lock();
	bind = rcu_dereference(peer->bind);
	if (unlikely(!bind)) {
		rcu_read_unlock();
		return;
	}

	spin_lock_bh(&peer->lock);
	if (likely(ovpn_bind_skb_src_match(bind, skb)))
		goto unlock;

	family = skb_protocol_to_family(skb);

	if (bind->remote.in4.sin_family == family)
		local_ip = (u8 *)&bind->local;

	switch (family) {
	case AF_INET:
		sa = (struct sockaddr_in *)&ss;
		sa->sin_family = AF_INET;
		sa->sin_addr.s_addr = ip_hdr(skb)->saddr;
		sa->sin_port = udp_hdr(skb)->source;
		salen = sizeof(*sa);
		break;
	case AF_INET6:
		sa6 = (struct sockaddr_in6 *)&ss;
		sa6->sin6_family = AF_INET6;
		sa6->sin6_addr = ipv6_hdr(skb)->saddr;
		sa6->sin6_port = udp_hdr(skb)->source;
		sa6->sin6_scope_id = ipv6_iface_scope_id(&ipv6_hdr(skb)->saddr,
							 skb->skb_iif);
		salen = sizeof(*sa6);
		break;
	default:
		goto unlock;
	}

	netdev_dbg(peer->ovpn->dev, "%s: peer %d floated to %pIScp", __func__,
		   peer->id, &ss);
	ovpn_peer_reset_sockaddr(peer, (struct sockaddr_storage *)&ss,
				 local_ip);

	/* rehashing is required only in MP mode as P2P has one peer
	 * only and thus there is no hashtable
	 */
	if (peer->ovpn->mode == OVPN_MODE_MP) {
		spin_lock_bh(&peer->ovpn->peers->lock);
		/* remove old hashing */
		hlist_nulls_del_init_rcu(&peer->hash_entry_transp_addr);
		/* re-add with new transport address */
		nhead = ovpn_get_hash_head(peer->ovpn->peers->by_transp_addr,
					   &ss, salen);
		hlist_nulls_add_head_rcu(&peer->hash_entry_transp_addr, nhead);
		spin_unlock_bh(&peer->ovpn->peers->lock);
	}
unlock:
	spin_unlock_bh(&peer->lock);
	rcu_read_unlock();
}

void ovpn_peer_release(struct ovpn_peer *peer)
{
	if (peer->sock)
		ovpn_socket_put(peer->sock);

	ovpn_crypto_state_release(&peer->crypto);
	spin_lock_bh(&peer->lock);
	ovpn_bind_reset(peer, NULL);
	spin_unlock_bh(&peer->lock);

	dst_cache_destroy(&peer->dst_cache);
	netdev_put(peer->ovpn->dev, &peer->ovpn->dev_tracker);
	kfree_rcu(peer, rcu);
}

/**
 * ovpn_peer_release_kref - callback for kref_put
 * @kref: the kref object belonging to the peer
 */
void ovpn_peer_release_kref(struct kref *kref)
{
	struct ovpn_peer *peer = container_of(kref, struct ovpn_peer, refcount);

	ovpn_peer_release(peer);
}

/**
 * ovpn_peer_skb_to_sockaddr - fill sockaddr with skb source address
 * @skb: the packet to extract data from
 * @ss: the sockaddr to fill
 *
 * Return: true on success or false otherwise
 */
static bool ovpn_peer_skb_to_sockaddr(struct sk_buff *skb,
				      struct sockaddr_storage *ss)
{
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa4;

	ss->ss_family = skb_protocol_to_family(skb);
	switch (ss->ss_family) {
	case AF_INET:
		sa4 = (struct sockaddr_in *)ss;
		sa4->sin_family = AF_INET;
		sa4->sin_addr.s_addr = ip_hdr(skb)->saddr;
		sa4->sin_port = udp_hdr(skb)->source;
		break;
	case AF_INET6:
		sa6 = (struct sockaddr_in6 *)ss;
		sa6->sin6_family = AF_INET6;
		sa6->sin6_addr = ipv6_hdr(skb)->saddr;
		sa6->sin6_port = udp_hdr(skb)->source;
		break;
	default:
		return false;
	}

	return true;
}

/**
 * ovpn_nexthop_from_skb4 - retrieve IPv4 nexthop for outgoing skb
 * @skb: the outgoing packet
 *
 * Return: the IPv4 of the nexthop
 */
static __be32 ovpn_nexthop_from_skb4(struct sk_buff *skb)
{
	const struct rtable *rt = skb_rtable(skb);

	if (rt && rt->rt_uses_gateway)
		return rt->rt_gw4;

	return ip_hdr(skb)->daddr;
}

/**
 * ovpn_nexthop_from_skb6 - retrieve IPv6 nexthop for outgoing skb
 * @skb: the outgoing packet
 *
 * Return: the IPv6 of the nexthop
 */
static struct in6_addr ovpn_nexthop_from_skb6(struct sk_buff *skb)
{
	const struct rt6_info *rt = skb_rt6_info(skb);

	if (!rt || !(rt->rt6i_flags & RTF_GATEWAY))
		return ipv6_hdr(skb)->daddr;

	return rt->rt6i_gateway;
}

/**
 * ovpn_peer_get_by_vpn_addr4 - retrieve peer by its VPN IPv4 address
 * @ovpn: the openvpn instance to search
 * @addr: VPN IPv4 to use as search key
 *
 * Refcounter is not increased for the returned peer.
 *
 * Return: the peer if found or NULL otherwise
 */
static struct ovpn_peer *ovpn_peer_get_by_vpn_addr4(struct ovpn_struct *ovpn,
						    __be32 addr)
{
	struct hlist_nulls_head *nhead;
	struct hlist_nulls_node *ntmp;
	struct ovpn_peer *tmp;

	nhead = ovpn_get_hash_head(ovpn->peers->by_vpn_addr, &addr,
				   sizeof(addr));

	hlist_nulls_for_each_entry_rcu(tmp, ntmp, nhead, hash_entry_addr4)
		if (addr == tmp->vpn_addrs.ipv4.s_addr)
			return tmp;

	return NULL;
}

/**
 * ovpn_peer_get_by_vpn_addr6 - retrieve peer by its VPN IPv6 address
 * @ovpn: the openvpn instance to search
 * @addr: VPN IPv6 to use as search key
 *
 * Refcounter is not increased for the returned peer.
 *
 * Return: the peer if found or NULL otherwise
 */
static struct ovpn_peer *ovpn_peer_get_by_vpn_addr6(struct ovpn_struct *ovpn,
						    struct in6_addr *addr)
{
	struct hlist_nulls_head *nhead;
	struct hlist_nulls_node *ntmp;
	struct ovpn_peer *tmp;

	nhead = ovpn_get_hash_head(ovpn->peers->by_vpn_addr, addr,
				   sizeof(*addr));

	hlist_nulls_for_each_entry_rcu(tmp, ntmp, nhead, hash_entry_addr6)
		if (ipv6_addr_equal(addr, &tmp->vpn_addrs.ipv6))
			return tmp;

	return NULL;
}

/**
 * ovpn_peer_transp_match - check if sockaddr and peer binding match
 * @peer: the peer to get the binding from
 * @ss: the sockaddr to match
 *
 * Return: true if sockaddr and binding match or false otherwise
 */
static bool ovpn_peer_transp_match(const struct ovpn_peer *peer,
				   const struct sockaddr_storage *ss)
{
	struct ovpn_bind *bind = rcu_dereference(peer->bind);
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa4;

	if (unlikely(!bind))
		return false;

	if (ss->ss_family != bind->remote.in4.sin_family)
		return false;

	switch (ss->ss_family) {
	case AF_INET:
		sa4 = (struct sockaddr_in *)ss;
		if (sa4->sin_addr.s_addr != bind->remote.in4.sin_addr.s_addr)
			return false;
		if (sa4->sin_port != bind->remote.in4.sin_port)
			return false;
		break;
	case AF_INET6:
		sa6 = (struct sockaddr_in6 *)ss;
		if (!ipv6_addr_equal(&sa6->sin6_addr,
				     &bind->remote.in6.sin6_addr))
			return false;
		if (sa6->sin6_port != bind->remote.in6.sin6_port)
			return false;
		break;
	default:
		return false;
	}

	return true;
}

/**
 * ovpn_peer_get_by_transp_addr_p2p - get peer by transport address in a P2P
 *                                    instance
 * @ovpn: the openvpn instance to search
 * @ss: the transport socket address
 *
 * Return: the peer if found or NULL otherwise
 */
static struct ovpn_peer *
ovpn_peer_get_by_transp_addr_p2p(struct ovpn_struct *ovpn,
				 struct sockaddr_storage *ss)
{
	struct ovpn_peer *tmp, *peer = NULL;

	rcu_read_lock();
	tmp = rcu_dereference(ovpn->peer);
	if (likely(tmp && ovpn_peer_transp_match(tmp, ss) &&
		   ovpn_peer_hold(tmp)))
		peer = tmp;
	rcu_read_unlock();

	return peer;
}

/**
 * ovpn_peer_get_by_transp_addr - retrieve peer by transport address
 * @ovpn: the openvpn instance to search
 * @skb: the skb to retrieve the source transport address from
 *
 * Return: a pointer to the peer if found or NULL otherwise
 */
struct ovpn_peer *ovpn_peer_get_by_transp_addr(struct ovpn_struct *ovpn,
					       struct sk_buff *skb)
{
	struct ovpn_peer *tmp, *peer = NULL;
	struct sockaddr_storage ss = { 0 };
	struct hlist_nulls_head *nhead;
	struct hlist_nulls_node *ntmp;
	size_t sa_len;

	if (unlikely(!ovpn_peer_skb_to_sockaddr(skb, &ss)))
		return NULL;

	if (ovpn->mode == OVPN_MODE_P2P)
		return ovpn_peer_get_by_transp_addr_p2p(ovpn, &ss);

	switch (ss.ss_family) {
	case AF_INET:
		sa_len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		sa_len = sizeof(struct sockaddr_in6);
		break;
	default:
		return NULL;
	}

	nhead = ovpn_get_hash_head(ovpn->peers->by_transp_addr, &ss, sa_len);

	rcu_read_lock();
	hlist_nulls_for_each_entry_rcu(tmp, ntmp, nhead,
				       hash_entry_transp_addr) {
		if (!ovpn_peer_transp_match(tmp, &ss))
			continue;

		if (!ovpn_peer_hold(tmp))
			continue;

		peer = tmp;
		break;
	}
	rcu_read_unlock();

	return peer;
}

/**
 * ovpn_peer_get_by_id_p2p - get peer by ID in a P2P instance
 * @ovpn: the openvpn instance to search
 * @peer_id: the ID of the peer to find
 *
 * Return: the peer if found or NULL otherwise
 */
static struct ovpn_peer *ovpn_peer_get_by_id_p2p(struct ovpn_struct *ovpn,
						 u32 peer_id)
{
	struct ovpn_peer *tmp, *peer = NULL;

	rcu_read_lock();
	tmp = rcu_dereference(ovpn->peer);
	if (likely(tmp && tmp->id == peer_id && ovpn_peer_hold(tmp)))
		peer = tmp;
	rcu_read_unlock();

	return peer;
}

/**
 * ovpn_peer_get_by_id - retrieve peer by ID
 * @ovpn: the openvpn instance to search
 * @peer_id: the unique peer identifier to match
 *
 * Return: a pointer to the peer if found or NULL otherwise
 */
struct ovpn_peer *ovpn_peer_get_by_id(struct ovpn_struct *ovpn, u32 peer_id)
{
	struct ovpn_peer *tmp, *peer = NULL;
	struct hlist_head *head;

	if (ovpn->mode == OVPN_MODE_P2P)
		return ovpn_peer_get_by_id_p2p(ovpn, peer_id);

	head = ovpn_get_hash_head(ovpn->peers->by_id, &peer_id,
				  sizeof(peer_id));

	rcu_read_lock();
	hlist_for_each_entry_rcu(tmp, head, hash_entry_id) {
		if (tmp->id != peer_id)
			continue;

		if (!ovpn_peer_hold(tmp))
			continue;

		peer = tmp;
		break;
	}
	rcu_read_unlock();

	return peer;
}

/**
 * ovpn_peer_update_local_endpoint - update local endpoint for peer
 * @peer: peer to update the endpoint for
 * @skb: incoming packet to retrieve the destination address (local) from
 */
void ovpn_peer_update_local_endpoint(struct ovpn_peer *peer,
				     struct sk_buff *skb)
{
	struct ovpn_bind *bind;

	rcu_read_lock();
	bind = rcu_dereference(peer->bind);
	if (unlikely(!bind))
		goto unlock;

	spin_lock_bh(&peer->lock);
	switch (skb_protocol_to_family(skb)) {
	case AF_INET:
		if (unlikely(bind->local.ipv4.s_addr != ip_hdr(skb)->daddr)) {
			netdev_dbg(peer->ovpn->dev,
				   "%s: learning local IPv4 for peer %d (%pI4 -> %pI4)\n",
				   __func__, peer->id, &bind->local.ipv4.s_addr,
				   &ip_hdr(skb)->daddr);
			bind->local.ipv4.s_addr = ip_hdr(skb)->daddr;
		}
		break;
	case AF_INET6:
		if (unlikely(!ipv6_addr_equal(&bind->local.ipv6,
					      &ipv6_hdr(skb)->daddr))) {
			netdev_dbg(peer->ovpn->dev,
				   "%s: learning local IPv6 for peer %d (%pI6c -> %pI6c\n",
				   __func__, peer->id, &bind->local.ipv6,
				   &ipv6_hdr(skb)->daddr);
			bind->local.ipv6 = ipv6_hdr(skb)->daddr;
		}
		break;
	default:
		break;
	}
	spin_unlock_bh(&peer->lock);

unlock:
	rcu_read_unlock();
}

/**
 * ovpn_peer_get_by_dst - Lookup peer to send skb to
 * @ovpn: the private data representing the current VPN session
 * @skb: the skb to extract the destination address from
 *
 * This function takes a tunnel packet and looks up the peer to send it to
 * after encapsulation. The skb is expected to be the in-tunnel packet, without
 * any OpenVPN related header.
 *
 * Assume that the IP header is accessible in the skb data.
 *
 * Return: the peer if found or NULL otherwise.
 */
struct ovpn_peer *ovpn_peer_get_by_dst(struct ovpn_struct *ovpn,
				       struct sk_buff *skb)
{
	struct ovpn_peer *peer = NULL;
	struct in6_addr addr6;
	__be32 addr4;

	/* in P2P mode, no matter the destination, packets are always sent to
	 * the single peer listening on the other side
	 */
	if (ovpn->mode == OVPN_MODE_P2P) {
		rcu_read_lock();
		peer = rcu_dereference(ovpn->peer);
		if (unlikely(peer && !ovpn_peer_hold(peer)))
			peer = NULL;
		rcu_read_unlock();
		return peer;
	}

	rcu_read_lock();
	switch (skb_protocol_to_family(skb)) {
	case AF_INET:
		addr4 = ovpn_nexthop_from_skb4(skb);
		peer = ovpn_peer_get_by_vpn_addr4(ovpn, addr4);
		break;
	case AF_INET6:
		addr6 = ovpn_nexthop_from_skb6(skb);
		peer = ovpn_peer_get_by_vpn_addr6(ovpn, &addr6);
		break;
	}

	if (unlikely(peer && !ovpn_peer_hold(peer)))
		peer = NULL;
	rcu_read_unlock();

	return peer;
}

/**
 * ovpn_nexthop_from_rt4 - look up the IPv4 nexthop for the given destination
 * @ovpn: the private data representing the current VPN session
 * @dest: the destination to be looked up
 *
 * Looks up in the IPv4 system routing table the IP of the nexthop to be used
 * to reach the destination passed as argument. If no nexthop can be found, the
 * destination itself is returned as it probably has to be used as nexthop.
 *
 * Return: the IP of the next hop if found or dest itself otherwise
 */
static __be32 ovpn_nexthop_from_rt4(struct ovpn_struct *ovpn, __be32 dest)
{
	struct rtable *rt;
	struct flowi4 fl = {
		.daddr = dest
	};

	rt = ip_route_output_flow(dev_net(ovpn->dev), &fl, NULL);
	if (IS_ERR(rt)) {
		net_dbg_ratelimited("%s: no route to host %pI4\n", __func__,
				    &dest);
		/* if we end up here this packet is probably going to be
		 * thrown away later
		 */
		return dest;
	}

	if (!rt->rt_uses_gateway)
		goto out;

	dest = rt->rt_gw4;
out:
	ip_rt_put(rt);
	return dest;
}

/**
 * ovpn_nexthop_from_rt6 - look up the IPv6 nexthop for the given destination
 * @ovpn: the private data representing the current VPN session
 * @dest: the destination to be looked up
 *
 * Looks up in the IPv6 system routing table the IP of the nexthop to be used
 * to reach the destination passed as argument. If no nexthop can be found, the
 * destination itself is returned as it probably has to be used as nexthop.
 *
 * Return: the IP of the next hop if found or dest itself otherwise
 */
static struct in6_addr ovpn_nexthop_from_rt6(struct ovpn_struct *ovpn,
					     struct in6_addr dest)
{
#if IS_ENABLED(CONFIG_IPV6)
	struct dst_entry *entry;
	struct rt6_info *rt;
	struct flowi6 fl = {
		.daddr = dest,
	};

	entry = ipv6_stub->ipv6_dst_lookup_flow(dev_net(ovpn->dev), NULL, &fl,
						NULL);
	if (IS_ERR(entry)) {
		net_dbg_ratelimited("%s: no route to host %pI6c\n", __func__,
				    &dest);
		/* if we end up here this packet is probably going to be
		 * thrown away later
		 */
		return dest;
	}

	rt = dst_rt6_info(entry);

	if (!(rt->rt6i_flags & RTF_GATEWAY))
		goto out;

	dest = rt->rt6i_gateway;
out:
	dst_release((struct dst_entry *)rt);
#endif
	return dest;
}

/**
 * ovpn_peer_check_by_src - check that skb source is routed via peer
 * @ovpn: the openvpn instance to search
 * @skb: the packet to extract source address from
 * @peer: the peer to check against the source address
 *
 * Return: true if the peer is matching or false otherwise
 */
bool ovpn_peer_check_by_src(struct ovpn_struct *ovpn, struct sk_buff *skb,
			    struct ovpn_peer *peer)
{
	bool match = false;
	struct in6_addr addr6;
	__be32 addr4;

	if (ovpn->mode == OVPN_MODE_P2P) {
		/* in P2P mode, no matter the destination, packets are always
		 * sent to the single peer listening on the other side
		 */
		rcu_read_lock();
		match = (peer == rcu_dereference(ovpn->peer));
		rcu_read_unlock();
		return match;
	}

	/* This function performs a reverse path check, therefore we now
	 * lookup the nexthop we would use if we wanted to route a packet
	 * to the source IP. If the nexthop matches the sender we know the
	 * latter is valid and we allow the packet to come in
	 */

	switch (skb_protocol_to_family(skb)) {
	case AF_INET:
		addr4 = ovpn_nexthop_from_rt4(ovpn, ip_hdr(skb)->saddr);
		rcu_read_lock();
		match = (peer == ovpn_peer_get_by_vpn_addr4(ovpn, addr4));
		rcu_read_unlock();
		break;
	case AF_INET6:
		addr6 = ovpn_nexthop_from_rt6(ovpn, ipv6_hdr(skb)->saddr);
		rcu_read_lock();
		match = (peer == ovpn_peer_get_by_vpn_addr6(ovpn, &addr6));
		rcu_read_unlock();
		break;
	}

	return match;
}

void ovpn_peer_hash_vpn_ip(struct ovpn_peer *peer)
	__must_hold(&peer->ovpn->peers->lock)
{
	struct hlist_nulls_head *nhead;

	if (peer->vpn_addrs.ipv4.s_addr != htonl(INADDR_ANY)) {
		/* remove potential old hashing */
		hlist_nulls_del_init_rcu(&peer->hash_entry_transp_addr);

		nhead = ovpn_get_hash_head(peer->ovpn->peers->by_vpn_addr,
					   &peer->vpn_addrs.ipv4,
					   sizeof(peer->vpn_addrs.ipv4));
		hlist_nulls_add_head_rcu(&peer->hash_entry_addr4, nhead);
	}

	if (!ipv6_addr_any(&peer->vpn_addrs.ipv6)) {
		/* remove potential old hashing */
		hlist_nulls_del_init_rcu(&peer->hash_entry_transp_addr);

		nhead = ovpn_get_hash_head(peer->ovpn->peers->by_vpn_addr,
					   &peer->vpn_addrs.ipv6,
					   sizeof(peer->vpn_addrs.ipv6));
		hlist_nulls_add_head_rcu(&peer->hash_entry_addr6, nhead);
	}
}

/**
 * ovpn_peer_add_mp - add peer to related tables in a MP instance
 * @ovpn: the instance to add the peer to
 * @peer: the peer to add
 *
 * Return: 0 on success or a negative error code otherwise
 */
static int ovpn_peer_add_mp(struct ovpn_struct *ovpn, struct ovpn_peer *peer)
{
	struct sockaddr_storage sa = { 0 };
	struct hlist_nulls_head *nhead;
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa4;
	struct ovpn_bind *bind;
	struct ovpn_peer *tmp;
	size_t salen;
	int ret = 0;

	spin_lock_bh(&ovpn->peers->lock);
	/* do not add duplicates */
	tmp = ovpn_peer_get_by_id(ovpn, peer->id);
	if (tmp) {
		ovpn_peer_put(tmp);
		ret = -EEXIST;
		goto out;
	}

	bind = rcu_dereference_protected(peer->bind, true);
	/* peers connected via TCP have bind == NULL */
	if (bind) {
		switch (bind->remote.in4.sin_family) {
		case AF_INET:
			sa4 = (struct sockaddr_in *)&sa;

			sa4->sin_family = AF_INET;
			sa4->sin_addr.s_addr = bind->remote.in4.sin_addr.s_addr;
			sa4->sin_port = bind->remote.in4.sin_port;
			salen = sizeof(*sa4);
			break;
		case AF_INET6:
			sa6 = (struct sockaddr_in6 *)&sa;

			sa6->sin6_family = AF_INET6;
			sa6->sin6_addr = bind->remote.in6.sin6_addr;
			sa6->sin6_port = bind->remote.in6.sin6_port;
			salen = sizeof(*sa6);
			break;
		default:
			ret = -EPROTONOSUPPORT;
			goto out;
		}

		nhead = ovpn_get_hash_head(ovpn->peers->by_transp_addr, &sa,
					   salen);
		hlist_nulls_add_head_rcu(&peer->hash_entry_transp_addr, nhead);
	}

	hlist_add_head_rcu(&peer->hash_entry_id,
			   ovpn_get_hash_head(ovpn->peers->by_id, &peer->id,
					      sizeof(peer->id)));

	ovpn_peer_hash_vpn_ip(peer);
out:
	spin_unlock_bh(&ovpn->peers->lock);
	return ret;
}

/**
 * ovpn_peer_add_p2p - add peer to related tables in a P2P instance
 * @ovpn: the instance to add the peer to
 * @peer: the peer to add
 *
 * Return: 0 on success or a negative error code otherwise
 */
static int ovpn_peer_add_p2p(struct ovpn_struct *ovpn, struct ovpn_peer *peer)
{
	struct ovpn_peer *tmp;

	spin_lock_bh(&ovpn->lock);
	/* in p2p mode it is possible to have a single peer only, therefore the
	 * old one is released and substituted by the new one
	 */
	tmp = rcu_dereference_protected(ovpn->peer,
					lockdep_is_held(&ovpn->lock));
	if (tmp) {
		tmp->delete_reason = OVPN_DEL_PEER_REASON_TEARDOWN;
		ovpn_peer_put(tmp);
	}

	rcu_assign_pointer(ovpn->peer, peer);
	spin_unlock_bh(&ovpn->lock);

	return 0;
}

/**
 * ovpn_peer_add - add peer to the related tables
 * @ovpn: the openvpn instance the peer belongs to
 * @peer: the peer object to add
 *
 * Assume refcounter was increased by caller
 *
 * Return: 0 on success or a negative error code otherwise
 */
int ovpn_peer_add(struct ovpn_struct *ovpn, struct ovpn_peer *peer)
{
	switch (ovpn->mode) {
	case OVPN_MODE_MP:
		return ovpn_peer_add_mp(ovpn, peer);
	case OVPN_MODE_P2P:
		return ovpn_peer_add_p2p(ovpn, peer);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * ovpn_peer_unhash - remove peer reference from all hashtables
 * @peer: the peer to remove
 * @reason: the delete reason to attach to the peer
 */
static void ovpn_peer_unhash(struct ovpn_peer *peer,
			     enum ovpn_del_peer_reason reason)
	__must_hold(&ovpn->peers->lock)
{
	hlist_del_init_rcu(&peer->hash_entry_id);

	hlist_nulls_del_init_rcu(&peer->hash_entry_addr4);
	hlist_nulls_del_init_rcu(&peer->hash_entry_addr6);
	hlist_nulls_del_init_rcu(&peer->hash_entry_transp_addr);

	ovpn_peer_put(peer);
	peer->delete_reason = reason;
}

/**
 * ovpn_peer_del_mp - delete peer from related tables in a MP instance
 * @peer: the peer to delete
 * @reason: reason why the peer was deleted (sent to userspace)
 *
 * Return: 0 on success or a negative error code otherwise
 */
static int ovpn_peer_del_mp(struct ovpn_peer *peer,
			    enum ovpn_del_peer_reason reason)
	__must_hold(&peer->ovpn->peers->lock)
{
	struct ovpn_peer *tmp;
	int ret = -ENOENT;

	tmp = ovpn_peer_get_by_id(peer->ovpn, peer->id);
	if (tmp == peer) {
		ovpn_peer_unhash(peer, reason);
		ret = 0;
	}

	if (tmp)
		ovpn_peer_put(tmp);

	return ret;
}

/**
 * ovpn_peer_del_p2p - delete peer from related tables in a P2P instance
 * @peer: the peer to delete
 * @reason: reason why the peer was deleted (sent to userspace)
 *
 * Return: 0 on success or a negative error code otherwise
 */
static int ovpn_peer_del_p2p(struct ovpn_peer *peer,
			     enum ovpn_del_peer_reason reason)
	__must_hold(&peer->ovpn->lock)
{
	struct ovpn_peer *tmp;

	tmp = rcu_dereference_protected(peer->ovpn->peer,
					lockdep_is_held(&peer->ovpn->lock));
	if (tmp != peer) {
		DEBUG_NET_WARN_ON_ONCE(1);
		if (tmp)
			ovpn_peer_put(tmp);

		return -ENOENT;
	}

	tmp->delete_reason = reason;
	RCU_INIT_POINTER(peer->ovpn->peer, NULL);
	ovpn_peer_put(tmp);

	return 0;
}

/**
 * ovpn_peer_release_p2p - release peer upon P2P device teardown
 * @ovpn: the instance being torn down
 */
void ovpn_peer_release_p2p(struct ovpn_struct *ovpn)
{
	struct ovpn_peer *tmp;

	spin_lock_bh(&ovpn->lock);
	tmp = rcu_dereference_protected(ovpn->peer,
					lockdep_is_held(&ovpn->lock));
	if (tmp)
		ovpn_peer_del_p2p(tmp, OVPN_DEL_PEER_REASON_TEARDOWN);
	spin_unlock_bh(&ovpn->lock);
}

/**
 * ovpn_peer_del - delete peer from related tables
 * @peer: the peer object to delete
 * @reason: reason for deleting peer (will be sent to userspace)
 *
 * Return: 0 on success or a negative error code otherwise
 */
int ovpn_peer_del(struct ovpn_peer *peer, enum ovpn_del_peer_reason reason)
{
	int ret;

	switch (peer->ovpn->mode) {
	case OVPN_MODE_MP:
		spin_lock_bh(&peer->ovpn->peers->lock);
		ret = ovpn_peer_del_mp(peer, reason);
		spin_unlock_bh(&peer->ovpn->peers->lock);
		return ret;
	case OVPN_MODE_P2P:
		spin_lock_bh(&peer->ovpn->lock);
		ret = ovpn_peer_del_p2p(peer, reason);
		spin_unlock_bh(&peer->ovpn->lock);
		return ret;
	default:
		return -EOPNOTSUPP;
	}
}

static int ovpn_peer_del_nolock(struct ovpn_peer *peer,
				enum ovpn_del_peer_reason reason)
{
	switch (peer->ovpn->mode) {
	case OVPN_MODE_MP:
		return ovpn_peer_del_mp(peer, reason);
	case OVPN_MODE_P2P:
		return ovpn_peer_del_p2p(peer, reason);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * ovpn_peers_free - free all peers in the instance
 * @ovpn: the instance whose peers should be released
 */
void ovpn_peers_free(struct ovpn_struct *ovpn)
{
	struct hlist_node *tmp;
	struct ovpn_peer *peer;
	int bkt;

	spin_lock_bh(&ovpn->peers->lock);
	hash_for_each_safe(ovpn->peers->by_id, bkt, tmp, peer, hash_entry_id)
		ovpn_peer_unhash(peer, OVPN_DEL_PEER_REASON_TEARDOWN);
	spin_unlock_bh(&ovpn->peers->lock);
}

static time64_t ovpn_peer_keepalive_work_single(struct ovpn_peer *peer,
						time64_t now)
{
	time64_t next_run1, next_run2, delta;
	unsigned long timeout, interval;
	bool expired;

	spin_lock_bh(&peer->lock);
	/* we expect both timers to be configured at the same time,
	 * therefore bail out if either is not set
	 */
	if (!peer->keepalive_timeout || !peer->keepalive_interval) {
		spin_unlock_bh(&peer->lock);
		return 0;
	}

	/* check for peer timeout */
	expired = false;
	timeout = peer->keepalive_timeout;
	delta = now - peer->last_recv;
	if (delta < timeout) {
		peer->keepalive_recv_exp = now + timeout - delta;
		next_run1 = peer->keepalive_recv_exp;
	} else if (peer->keepalive_recv_exp > now) {
		next_run1 = peer->keepalive_recv_exp;
	} else {
		expired = true;
	}

	if (expired) {
		/* peer is dead -> kill it and move on */
		spin_unlock_bh(&peer->lock);
		netdev_dbg(peer->ovpn->dev, "peer %u expired\n",
			   peer->id);
		ovpn_peer_del_nolock(peer, OVPN_DEL_PEER_REASON_EXPIRED);
		return 0;
	}

	/* check for peer keepalive */
	expired = false;
	interval = peer->keepalive_interval;
	delta = now - peer->last_sent;
	if (delta < interval) {
		peer->keepalive_xmit_exp = now + interval - delta;
		next_run2 = peer->keepalive_xmit_exp;
	} else if (peer->keepalive_xmit_exp > now) {
		next_run2 = peer->keepalive_xmit_exp;
	} else {
		expired = true;
		next_run2 = now + interval;
	}
	spin_unlock_bh(&peer->lock);

	if (expired) {
		/* a keepalive packet is required */
		netdev_dbg(peer->ovpn->dev,
			   "sending keepalive to peer %u\n",
			   peer->id);
		ovpn_xmit_special(peer, ovpn_keepalive_message,
				  sizeof(ovpn_keepalive_message));
	}

	if (next_run1 < next_run2)
		return next_run1;

	return next_run2;
}

static time64_t ovpn_peer_keepalive_work_mp(struct ovpn_struct *ovpn,
					    time64_t now)
{
	time64_t tmp_next_run, next_run = 0;
	struct hlist_node *tmp;
	struct ovpn_peer *peer;
	int bkt;

	spin_lock_bh(&ovpn->peers->lock);
	hash_for_each_safe(ovpn->peers->by_id, bkt, tmp, peer, hash_entry_id) {
		tmp_next_run = ovpn_peer_keepalive_work_single(peer, now);
		if (!tmp_next_run)
			continue;

		/* the next worker run will be scheduled based on the shortest
		 * required interval across all peers
		 */
		if (!next_run || tmp_next_run < next_run)
			next_run = tmp_next_run;
	}
	spin_unlock_bh(&ovpn->peers->lock);

	return next_run;
}

static time64_t ovpn_peer_keepalive_work_p2p(struct ovpn_struct *ovpn,
					     time64_t now)
{
	struct ovpn_peer *peer;
	time64_t next_run;

	spin_lock_bh(&ovpn->lock);
	peer = rcu_dereference_protected(ovpn->peer,
					 lockdep_is_held(&ovpn->lock));
	next_run = ovpn_peer_keepalive_work_single(peer, now);
	spin_unlock_bh(&ovpn->lock);

	return next_run;
}

/**
 * ovpn_peer_keepalive_work - run keepalive logic on each known peer
 * @work: pointer to the work member of the related ovpn object
 *
 * Each peer has two timers (if configured):
 * 1. peer timeout: when no data is received for a certain interval,
 *    the peer is considered dead and it gets killed.
 * 2. peer keepalive: when no data is sent to a certain peer for a
 *    certain interval, a special 'keepalive' packet is explicitly sent.
 *
 * This function iterates across the whole peer collection while
 * checking the timers described above.
 */
void ovpn_peer_keepalive_work(struct work_struct *work)
{
	struct ovpn_struct *ovpn = container_of(work, struct ovpn_struct,
						keepalive_work.work);
	time64_t next_run = 0, now = ktime_get_real_seconds();

	switch (ovpn->mode) {
	case OVPN_MODE_MP:
		next_run = ovpn_peer_keepalive_work_mp(ovpn, now);
		break;
	case OVPN_MODE_P2P:
		next_run = ovpn_peer_keepalive_work_p2p(ovpn, now);
		break;
	}

	/* prevent rearming if the interface is being destroyed */
	if (next_run > 0 && ovpn->registered) {
		netdev_dbg(ovpn->dev,
			   "scheduling keepalive work: now=%llu next_run=%llu delta=%llu\n",
			   next_run, now, next_run - now);
		schedule_delayed_work(&ovpn->keepalive_work,
				      (next_run - now) * HZ);
	}
}
