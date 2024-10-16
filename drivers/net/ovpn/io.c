// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include <crypto/aead.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/gro_cells.h>
#include <net/gso.h>
#include <net/ip.h>

#include "ovpnstruct.h"
#include "peer.h"
#include "io.h"
#include "bind.h"
#include "crypto.h"
#include "crypto_aead.h"
#include "netlink.h"
#include "proto.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "skb.h"
#include "socket.h"

const unsigned char ovpn_keepalive_message[OVPN_KEEPALIVE_SIZE] = {
	0x2a, 0x18, 0x7b, 0xf3, 0x64, 0x1e, 0xb4, 0xcb,
	0x07, 0xed, 0x2d, 0x0a, 0x98, 0x1f, 0xc7, 0x48
};

/**
 * ovpn_is_keepalive - check if skb contains a keepalive message
 * @skb: packet to check
 *
 * Assumes that the first byte of skb->data is defined.
 *
 * Return: true if skb contains a keepalive or false otherwise
 */
static bool ovpn_is_keepalive(struct sk_buff *skb)
{
	if (*skb->data != ovpn_keepalive_message[0])
		return false;

	if (skb->len != OVPN_KEEPALIVE_SIZE)
		return false;

	if (!pskb_may_pull(skb, OVPN_KEEPALIVE_SIZE))
		return false;

	return !memcmp(skb->data, ovpn_keepalive_message, OVPN_KEEPALIVE_SIZE);
}

/* Called after decrypt to write the IP packet to the device.
 * This method is expected to manage/free the skb.
 */
static void ovpn_netdev_write(struct ovpn_peer *peer, struct sk_buff *skb)
{
	unsigned int pkt_len;

	/* we can't guarantee the packet wasn't corrupted before entering the
	 * VPN, therefore we give other layers a chance to check that
	 */
	skb->ip_summed = CHECKSUM_NONE;

	/* skb hash for transport packet no longer valid after decapsulation */
	skb_clear_hash(skb);

	/* post-decrypt scrub -- prepare to inject encapsulated packet onto the
	 * interface, based on __skb_tunnel_rx() in dst.h
	 */
	skb->dev = peer->ovpn->dev;
	skb_set_queue_mapping(skb, 0);
	skb_scrub_packet(skb, true);

	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb_probe_transport_header(skb);
	skb_reset_inner_headers(skb);

	memset(skb->cb, 0, sizeof(skb->cb));

	/* cause packet to be "received" by the interface */
	pkt_len = skb->len;
	if (likely(gro_cells_receive(&peer->ovpn->gro_cells,
				     skb) == NET_RX_SUCCESS))
		/* update RX stats with the size of decrypted packet */
		dev_sw_netstats_rx_add(peer->ovpn->dev, pkt_len);
}

void ovpn_decrypt_post(void *data, int ret)
{
	struct ovpn_crypto_key_slot *ks = NULL;
	unsigned int payload_offset = 0;
	struct ovpn_peer *peer = NULL;
	struct sk_buff *skb = data;
	unsigned int orig_len = 0;
	__be16 proto;
	__be32 *pid;

	/* crypto is happening asynchronously. this function will be called
	 * again later by the crypto callback with a proper return code
	 */
	if (unlikely(ret == -EINPROGRESS))
		return;

	/* crypto is done, cleanup skb CB and its members */
	if (likely(ovpn_skb_cb(skb)->ctx)) {
		payload_offset = ovpn_skb_cb(skb)->ctx->payload_offset;
		ks = ovpn_skb_cb(skb)->ctx->ks;
		peer = ovpn_skb_cb(skb)->ctx->peer;
		orig_len = ovpn_skb_cb(skb)->ctx->orig_len;

		aead_request_free(ovpn_skb_cb(skb)->ctx->req);
		kfree(ovpn_skb_cb(skb)->ctx);
		ovpn_skb_cb(skb)->ctx = NULL;
	}

	if (unlikely(ret < 0))
		goto drop;

	/* PID sits after the op */
	pid = (__force __be32 *)(skb->data + OVPN_OP_SIZE_V2);
	ret = ovpn_pktid_recv(&ks->pid_recv, ntohl(*pid), 0);
	if (unlikely(ret < 0)) {
		net_err_ratelimited("%s: PKT ID RX error: %d\n",
				    peer->ovpn->dev->name, ret);
		goto drop;
	}

	/* keep track of last received authenticated packet for keepalive */
	peer->last_recv = ktime_get_real_seconds();

	if (peer->sock->sock->sk->sk_protocol == IPPROTO_UDP) {
		/* check if this peer changed it's IP address and update
		 * state
		 */
		ovpn_peer_float(peer, skb);
		/* update source endpoint for this peer */
		ovpn_peer_update_local_endpoint(peer, skb);
	}

	/* point to encapsulated IP packet */
	__skb_pull(skb, payload_offset);

	/* check if this is a valid datapacket that has to be delivered to the
	 * ovpn interface
	 */
	skb_reset_network_header(skb);
	proto = ovpn_ip_check_protocol(skb);
	if (unlikely(!proto)) {
		/* check if null packet */
		if (unlikely(!pskb_may_pull(skb, 1))) {
			net_info_ratelimited("%s: NULL packet received from peer %u\n",
					     peer->ovpn->dev->name, peer->id);
			goto drop;
		}

		if (ovpn_is_keepalive(skb)) {
			net_dbg_ratelimited("%s: ping received from peer %u\n",
					    peer->ovpn->dev->name, peer->id);
			goto drop;
		}

		net_info_ratelimited("%s: unsupported protocol received from peer %u\n",
				     peer->ovpn->dev->name, peer->id);
		goto drop;
	}
	skb->protocol = proto;

	/* perform Reverse Path Filtering (RPF) */
	if (unlikely(!ovpn_peer_check_by_src(peer->ovpn, skb, peer))) {
		if (skb_protocol_to_family(skb) == AF_INET6)
			net_dbg_ratelimited("%s: RPF dropped packet from peer %u, src: %pI6c\n",
					    peer->ovpn->dev->name, peer->id,
					    &ipv6_hdr(skb)->saddr);
		else
			net_dbg_ratelimited("%s: RPF dropped packet from peer %u, src: %pI4\n",
					    peer->ovpn->dev->name, peer->id,
					    &ip_hdr(skb)->saddr);
		goto drop;
	}

	/* increment RX stats */
	ovpn_peer_stats_increment_rx(&peer->vpn_stats, skb->len);
	ovpn_peer_stats_increment_rx(&peer->link_stats, orig_len);

	ovpn_netdev_write(peer, skb);
	/* skb is passed to upper layer - don't free it */
	skb = NULL;
drop:
	if (unlikely(skb))
		dev_core_stats_rx_dropped_inc(peer->ovpn->dev);
	if (likely(peer))
		ovpn_peer_put(peer);
	if (likely(ks))
		ovpn_crypto_key_slot_put(ks);
	kfree_skb(skb);
}

/* pick next packet from RX queue, decrypt and forward it to the device */
void ovpn_recv(struct ovpn_peer *peer, struct sk_buff *skb)
{
	struct ovpn_crypto_key_slot *ks;
	u8 key_id;

	/* get the key slot matching the key ID in the received packet */
	key_id = ovpn_key_id_from_skb(skb);
	ks = ovpn_crypto_key_id_to_slot(&peer->crypto, key_id);
	if (unlikely(!ks)) {
		net_info_ratelimited("%s: no available key for peer %u, key-id: %u\n",
				     peer->ovpn->dev->name, peer->id, key_id);
		dev_core_stats_rx_dropped_inc(peer->ovpn->dev);
		kfree_skb(skb);
		return;
	}

	ovpn_skb_cb(skb)->ctx = NULL;
	ovpn_decrypt_post(skb, ovpn_aead_decrypt(peer, ks, skb));
}

void ovpn_encrypt_post(void *data, int ret)
{
	struct ovpn_crypto_key_slot *ks = NULL;
	struct ovpn_peer *peer = NULL;
	struct sk_buff *skb = data;
	unsigned int orig_len = 0;

	/* encryption is happening asynchronously. This function will be
	 * called later by the crypto callback with a proper return value
	 */
	if (unlikely(ret == -EINPROGRESS))
		return;

	/* crypto is done, cleanup skb CB and its members */
	if (likely(ovpn_skb_cb(skb)->ctx)) {
		ks = ovpn_skb_cb(skb)->ctx->ks;
		peer = ovpn_skb_cb(skb)->ctx->peer;
		orig_len = ovpn_skb_cb(skb)->ctx->orig_len;

		aead_request_free(ovpn_skb_cb(skb)->ctx->req);
		kfree(ovpn_skb_cb(skb)->ctx);
		ovpn_skb_cb(skb)->ctx = NULL;
	}

	if (unlikely(ret == -ERANGE)) {
		/* we ran out of IVs and we must kill the key as it can't be
		 * use anymore
		 */
		netdev_warn(peer->ovpn->dev,
			    "killing key %u for peer %u\n", ks->key_id,
			    peer->id);
		ovpn_crypto_kill_key(&peer->crypto, ks->key_id);
		/* let userspace know so that a new key must be negotiated */
		ovpn_nl_key_swap_notify(peer, ks->key_id);
		goto err;
	}

	if (unlikely(ret < 0))
		goto err;

	skb_mark_not_on_list(skb);
	ovpn_peer_stats_increment_tx(&peer->link_stats, skb->len);
	ovpn_peer_stats_increment_tx(&peer->vpn_stats, orig_len);

	switch (peer->sock->sock->sk->sk_protocol) {
	case IPPROTO_UDP:
		ovpn_udp_send_skb(peer->ovpn, peer, skb);
		break;
	case IPPROTO_TCP:
		ovpn_tcp_send_skb(peer, skb);
		break;
	default:
		/* no transport configured yet */
		goto err;
	}

	/* keep track of last sent packet for keepalive */
	peer->last_sent = ktime_get_real_seconds();

	/* skb passed down the stack - don't free it */
	skb = NULL;
err:
	if (unlikely(skb))
		dev_core_stats_tx_dropped_inc(peer->ovpn->dev);
	if (likely(peer))
		ovpn_peer_put(peer);
	if (likely(ks))
		ovpn_crypto_key_slot_put(ks);
	kfree_skb(skb);
}

static bool ovpn_encrypt_one(struct ovpn_peer *peer, struct sk_buff *skb)
{
	struct ovpn_crypto_key_slot *ks;

	if (unlikely(skb->ip_summed == CHECKSUM_PARTIAL &&
		     skb_checksum_help(skb))) {
		net_warn_ratelimited("%s: cannot compute checksum for outgoing packet\n",
				     peer->ovpn->dev->name);
		return false;
	}

	/* get primary key to be used for encrypting data */
	ks = ovpn_crypto_key_slot_primary(&peer->crypto);
	if (unlikely(!ks)) {
		net_warn_ratelimited("%s: error while retrieving primary key slot for peer %u\n",
				     peer->ovpn->dev->name, peer->id);
		return false;
	}

	/* take a reference to the peer because the crypto code may run async.
	 * ovpn_encrypt_post() will release it upon completion
	 */
	if (unlikely(!ovpn_peer_hold(peer))) {
		DEBUG_NET_WARN_ON_ONCE(1);
		return false;
	}

	ovpn_skb_cb(skb)->ctx = NULL;
	ovpn_encrypt_post(skb, ovpn_aead_encrypt(peer, ks, skb));
	return true;
}

/* send skb to connected peer, if any */
static void ovpn_send(struct ovpn_struct *ovpn, struct sk_buff *skb,
		      struct ovpn_peer *peer)
{
	struct sk_buff *curr, *next;

	if (likely(!peer))
		/* retrieve peer serving the destination IP of this packet */
		peer = ovpn_peer_get_by_dst(ovpn, skb);
	if (unlikely(!peer)) {
		net_dbg_ratelimited("%s: no peer to send data to\n",
				    ovpn->dev->name);
		dev_core_stats_tx_dropped_inc(ovpn->dev);
		goto drop;
	}

	/* this might be a GSO-segmented skb list: process each skb
	 * independently
	 */
	skb_list_walk_safe(skb, curr, next)
		if (unlikely(!ovpn_encrypt_one(peer, curr))) {
			dev_core_stats_tx_dropped_inc(ovpn->dev);
			kfree_skb(curr);
		}

	/* skb passed over, no need to free */
	skb = NULL;
drop:
	if (likely(peer))
		ovpn_peer_put(peer);
	kfree_skb_list(skb);
}

/* Send user data to the network
 */
netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	struct sk_buff *segments, *curr, *next;
	struct sk_buff_head skb_list;
	__be16 proto;
	int ret;

	/* reset netfilter state */
	nf_reset_ct(skb);

	/* verify IP header size in network packet */
	proto = ovpn_ip_check_protocol(skb);
	if (unlikely(!proto || skb->protocol != proto)) {
		net_err_ratelimited("%s: dropping malformed payload packet\n",
				    dev->name);
		dev_core_stats_tx_dropped_inc(ovpn->dev);
		goto drop;
	}

	if (skb_is_gso(skb)) {
		segments = skb_gso_segment(skb, 0);
		if (IS_ERR(segments)) {
			ret = PTR_ERR(segments);
			net_err_ratelimited("%s: cannot segment packet: %d\n",
					    dev->name, ret);
			dev_core_stats_tx_dropped_inc(ovpn->dev);
			goto drop;
		}

		consume_skb(skb);
		skb = segments;
	}

	/* from this moment on, "skb" might be a list */

	__skb_queue_head_init(&skb_list);
	skb_list_walk_safe(skb, curr, next) {
		skb_mark_not_on_list(curr);

		curr = skb_share_check(curr, GFP_ATOMIC);
		if (unlikely(!curr)) {
			net_err_ratelimited("%s: skb_share_check failed\n",
					    dev->name);
			dev_core_stats_tx_dropped_inc(ovpn->dev);
			continue;
		}

		__skb_queue_tail(&skb_list, curr);
	}
	skb_list.prev->next = NULL;

	ovpn_send(ovpn, skb_list.next, NULL);

	return NETDEV_TX_OK;

drop:
	skb_tx_error(skb);
	kfree_skb_list(skb);
	return NET_XMIT_DROP;
}

/**
 * ovpn_xmit_special - encrypt and transmit an out-of-band message to peer
 * @peer: peer to send the message to
 * @data: message content
 * @len: message length
 *
 * Assumes that caller holds a reference to peer
 */
void ovpn_xmit_special(struct ovpn_peer *peer, const void *data,
		       const unsigned int len)
{
	struct ovpn_struct *ovpn;
	struct sk_buff *skb;

	ovpn = peer->ovpn;
	if (unlikely(!ovpn))
		return;

	skb = alloc_skb(256 + len, GFP_ATOMIC);
	if (unlikely(!skb))
		return;

	skb_reserve(skb, 128);
	skb->priority = TC_PRIO_BESTEFFORT;
	__skb_put_data(skb, data, len);

	/* increase reference counter when passing peer to sending queue */
	if (!ovpn_peer_hold(peer)) {
		netdev_dbg(ovpn->dev, "%s: cannot hold peer reference for sending special packet\n",
			   __func__);
		kfree_skb(skb);
		return;
	}

	ovpn_send(ovpn, skb, peer);
}
