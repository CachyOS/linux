/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2012-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_OVPNBIND_H_
#define _NET_OVPN_OVPNBIND_H_

#include <net/ip.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

struct ovpn_peer;

/**
 * union ovpn_sockaddr - basic transport layer address
 * @in4: IPv4 address
 * @in6: IPv6 address
 */
union ovpn_sockaddr {
	struct sockaddr_in in4;
	struct sockaddr_in6 in6;
};

/**
 * struct ovpn_bind - remote peer binding
 * @remote: the remote peer sockaddress
 * @local: local endpoint used to talk to the peer
 * @local.ipv4: local IPv4 used to talk to the peer
 * @local.ipv6: local IPv6 used to talk to the peer
 * @rcu: used to schedule RCU cleanup job
 */
struct ovpn_bind {
	union ovpn_sockaddr remote;  /* remote sockaddr */

	union {
		struct in_addr ipv4;
		struct in6_addr ipv6;
	} local;

	struct rcu_head rcu;
};

/**
 * skb_protocol_to_family - translate skb->protocol to AF_INET or AF_INET6
 * @skb: the packet sk_buff to inspect
 *
 * Return: AF_INET, AF_INET6 or 0 in case of unknown protocol
 */
static inline unsigned short skb_protocol_to_family(const struct sk_buff *skb)
{
	switch (skb->protocol) {
	case htons(ETH_P_IP):
		return AF_INET;
	case htons(ETH_P_IPV6):
		return AF_INET6;
	default:
		return 0;
	}
}

/**
 * ovpn_bind_skb_src_match - match packet source with binding
 * @bind: the binding to match
 * @skb: the packet to match
 *
 * Return: true if the packet source matches the remote peer sockaddr
 * in the binding
 */
static inline bool ovpn_bind_skb_src_match(const struct ovpn_bind *bind,
					   const struct sk_buff *skb)
{
	const unsigned short family = skb_protocol_to_family(skb);
	const union ovpn_sockaddr *remote;

	if (unlikely(!bind))
		return false;

	remote = &bind->remote;

	if (unlikely(remote->in4.sin_family != family))
		return false;

	switch (family) {
	case AF_INET:
		if (unlikely(remote->in4.sin_addr.s_addr != ip_hdr(skb)->saddr))
			return false;

		if (unlikely(remote->in4.sin_port != udp_hdr(skb)->source))
			return false;
		break;
	case AF_INET6:
		if (unlikely(!ipv6_addr_equal(&remote->in6.sin6_addr,
					      &ipv6_hdr(skb)->saddr)))
			return false;

		if (unlikely(remote->in6.sin6_port != udp_hdr(skb)->source))
			return false;
		break;
	default:
		return false;
	}

	return true;
}

struct ovpn_bind *ovpn_bind_from_sockaddr(const struct sockaddr_storage *sa);
void ovpn_bind_reset(struct ovpn_peer *peer, struct ovpn_bind *bind);

#endif /* _NET_OVPN_OVPNBIND_H_ */
