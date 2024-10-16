// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include <crypto/aead.h>
#include <linux/skbuff.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/udp.h>

#include "ovpnstruct.h"
#include "main.h"
#include "io.h"
#include "packet.h"
#include "pktid.h"
#include "crypto_aead.h"
#include "crypto.h"
#include "peer.h"
#include "proto.h"
#include "skb.h"

#define AUTH_TAG_SIZE	16

static int ovpn_aead_encap_overhead(const struct ovpn_crypto_key_slot *ks)
{
	return  OVPN_OP_SIZE_V2 +			/* OP header size */
		4 +					/* Packet ID */
		crypto_aead_authsize(ks->encrypt);	/* Auth Tag */
}

int ovpn_aead_encrypt(struct ovpn_peer *peer, struct ovpn_crypto_key_slot *ks,
		      struct sk_buff *skb)
{
	const unsigned int tag_size = crypto_aead_authsize(ks->encrypt);
	const unsigned int head_size = ovpn_aead_encap_overhead(ks);
	struct aead_request *req;
	struct sk_buff *trailer;
	struct scatterlist *sg;
	u8 iv[NONCE_SIZE];
	int nfrags, ret;
	u32 pktid, op;

	/* Sample AEAD header format:
	 * 48000001 00000005 7e7046bd 444a7e28 cc6387b1 64a4d6c1 380275a...
	 * [ OP32 ] [seq # ] [             auth tag            ] [ payload ... ]
	 *          [4-byte
	 *          IV head]
	 */

	/* check that there's enough headroom in the skb for packet
	 * encapsulation, after adding network header and encryption overhead
	 */
	if (unlikely(skb_cow_head(skb, OVPN_HEAD_ROOM + head_size)))
		return -ENOBUFS;

	/* get number of skb frags and ensure that packet data is writable */
	nfrags = skb_cow_data(skb, 0, &trailer);
	if (unlikely(nfrags < 0))
		return nfrags;

	if (unlikely(nfrags + 2 > (MAX_SKB_FRAGS + 2)))
		return -ENOSPC;

	ovpn_skb_cb(skb)->ctx = kmalloc(sizeof(*ovpn_skb_cb(skb)->ctx),
					GFP_ATOMIC);
	if (unlikely(!ovpn_skb_cb(skb)->ctx))
		return -ENOMEM;

	sg = ovpn_skb_cb(skb)->ctx->sg;

	/* sg table:
	 * 0: op, wire nonce (AD, len=OVPN_OP_SIZE_V2+NONCE_WIRE_SIZE),
	 * 1, 2, 3, ..., n: payload,
	 * n+1: auth_tag (len=tag_size)
	 */
	sg_init_table(sg, nfrags + 2);

	/* build scatterlist to encrypt packet payload */
	ret = skb_to_sgvec_nomark(skb, sg + 1, 0, skb->len);
	if (unlikely(nfrags != ret))
		return -EINVAL;

	/* append auth_tag onto scatterlist */
	__skb_push(skb, tag_size);
	sg_set_buf(sg + nfrags + 1, skb->data, tag_size);

	/* obtain packet ID, which is used both as a first
	 * 4 bytes of nonce and last 4 bytes of associated data.
	 */
	ret = ovpn_pktid_xmit_next(&ks->pid_xmit, &pktid);
	if (unlikely(ret < 0)) {
		kfree(ovpn_skb_cb(skb)->ctx);
		return ret;
	}

	/* concat 4 bytes packet id and 8 bytes nonce tail into 12 bytes
	 * nonce
	 */
	ovpn_pktid_aead_write(pktid, &ks->nonce_tail_xmit, iv);

	/* make space for packet id and push it to the front */
	__skb_push(skb, NONCE_WIRE_SIZE);
	memcpy(skb->data, iv, NONCE_WIRE_SIZE);

	/* add packet op as head of additional data */
	op = ovpn_opcode_compose(OVPN_DATA_V2, ks->key_id, peer->id);
	__skb_push(skb, OVPN_OP_SIZE_V2);
	BUILD_BUG_ON(sizeof(op) != OVPN_OP_SIZE_V2);
	*((__force __be32 *)skb->data) = htonl(op);

	/* AEAD Additional data */
	sg_set_buf(sg, skb->data, OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE);

	req = aead_request_alloc(ks->encrypt, GFP_ATOMIC);
	if (unlikely(!req)) {
		kfree(ovpn_skb_cb(skb)->ctx);
		return -ENOMEM;
	}

	/* setup async crypto operation */
	aead_request_set_tfm(req, ks->encrypt);
	aead_request_set_callback(req, 0, ovpn_encrypt_post, skb);
	aead_request_set_crypt(req, sg, sg, skb->len - head_size, iv);
	aead_request_set_ad(req, OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE);

	ovpn_skb_cb(skb)->ctx->peer = peer;
	ovpn_skb_cb(skb)->ctx->req = req;
	ovpn_skb_cb(skb)->ctx->ks = ks;

	/* encrypt it */
	return crypto_aead_encrypt(req);
}

int ovpn_aead_decrypt(struct ovpn_peer *peer, struct ovpn_crypto_key_slot *ks,
		      struct sk_buff *skb)
{
	const unsigned int tag_size = crypto_aead_authsize(ks->decrypt);
	int ret, payload_len, nfrags;
	unsigned int payload_offset;
	struct aead_request *req;
	struct sk_buff *trailer;
	struct scatterlist *sg;
	unsigned int sg_len;
	u8 iv[NONCE_SIZE];

	payload_offset = OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE + tag_size;
	payload_len = skb->len - payload_offset;

	/* sanity check on packet size, payload size must be >= 0 */
	if (unlikely(payload_len < 0))
		return -EINVAL;

	/* Prepare the skb data buffer to be accessed up until the auth tag.
	 * This is required because this area is directly mapped into the sg
	 * list.
	 */
	if (unlikely(!pskb_may_pull(skb, payload_offset)))
		return -ENODATA;

	/* get number of skb frags and ensure that packet data is writable */
	nfrags = skb_cow_data(skb, 0, &trailer);
	if (unlikely(nfrags < 0))
		return nfrags;

	if (unlikely(nfrags + 2 > (MAX_SKB_FRAGS + 2)))
		return -ENOSPC;

	ovpn_skb_cb(skb)->ctx = kmalloc(sizeof(*ovpn_skb_cb(skb)->ctx),
					GFP_ATOMIC);
	if (unlikely(!ovpn_skb_cb(skb)->ctx))
		return -ENOMEM;

	sg = ovpn_skb_cb(skb)->ctx->sg;

	/* sg table:
	 * 0: op, wire nonce (AD, len=OVPN_OP_SIZE_V2+NONCE_WIRE_SIZE),
	 * 1, 2, 3, ..., n: payload,
	 * n+1: auth_tag (len=tag_size)
	 */
	sg_init_table(sg, nfrags + 2);

	/* packet op is head of additional data */
	sg_len = OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE;
	sg_set_buf(sg, skb->data, sg_len);

	/* build scatterlist to decrypt packet payload */
	ret = skb_to_sgvec_nomark(skb, sg + 1, payload_offset, payload_len);
	if (unlikely(nfrags != ret)) {
		kfree(ovpn_skb_cb(skb)->ctx);
		return -EINVAL;
	}

	/* append auth_tag onto scatterlist */
	sg_set_buf(sg + nfrags + 1, skb->data + sg_len, tag_size);

	/* copy nonce into IV buffer */
	memcpy(iv, skb->data + OVPN_OP_SIZE_V2, NONCE_WIRE_SIZE);
	memcpy(iv + NONCE_WIRE_SIZE, ks->nonce_tail_recv.u8,
	       sizeof(struct ovpn_nonce_tail));

	req = aead_request_alloc(ks->decrypt, GFP_ATOMIC);
	if (unlikely(!req)) {
		kfree(ovpn_skb_cb(skb)->ctx);
		return -ENOMEM;
	}

	/* setup async crypto operation */
	aead_request_set_tfm(req, ks->decrypt);
	aead_request_set_callback(req, 0, ovpn_decrypt_post, skb);
	aead_request_set_crypt(req, sg, sg, payload_len + tag_size, iv);

	aead_request_set_ad(req, NONCE_WIRE_SIZE + OVPN_OP_SIZE_V2);

	ovpn_skb_cb(skb)->ctx->payload_offset = payload_offset;
	ovpn_skb_cb(skb)->ctx->peer = peer;
	ovpn_skb_cb(skb)->ctx->req = req;
	ovpn_skb_cb(skb)->ctx->ks = ks;

	/* decrypt it */
	return crypto_aead_decrypt(req);
}

/* Initialize a struct crypto_aead object */
struct crypto_aead *ovpn_aead_init(const char *title, const char *alg_name,
				   const unsigned char *key,
				   unsigned int keylen)
{
	struct crypto_aead *aead;
	int ret;

	aead = crypto_alloc_aead(alg_name, 0, 0);
	if (IS_ERR(aead)) {
		ret = PTR_ERR(aead);
		pr_err("%s crypto_alloc_aead failed, err=%d\n", title, ret);
		aead = NULL;
		goto error;
	}

	ret = crypto_aead_setkey(aead, key, keylen);
	if (ret) {
		pr_err("%s crypto_aead_setkey size=%u failed, err=%d\n", title,
		       keylen, ret);
		goto error;
	}

	ret = crypto_aead_setauthsize(aead, AUTH_TAG_SIZE);
	if (ret) {
		pr_err("%s crypto_aead_setauthsize failed, err=%d\n", title,
		       ret);
		goto error;
	}

	/* basic AEAD assumption */
	if (crypto_aead_ivsize(aead) != NONCE_SIZE) {
		pr_err("%s IV size must be %d\n", title, NONCE_SIZE);
		ret = -EINVAL;
		goto error;
	}

	pr_debug("********* Cipher %s (%s)\n", alg_name, title);
	pr_debug("*** IV size=%u\n", crypto_aead_ivsize(aead));
	pr_debug("*** req size=%u\n", crypto_aead_reqsize(aead));
	pr_debug("*** block size=%u\n", crypto_aead_blocksize(aead));
	pr_debug("*** auth size=%u\n", crypto_aead_authsize(aead));
	pr_debug("*** alignmask=0x%x\n", crypto_aead_alignmask(aead));

	return aead;

error:
	crypto_free_aead(aead);
	return ERR_PTR(ret);
}

void ovpn_aead_crypto_key_slot_destroy(struct ovpn_crypto_key_slot *ks)
{
	if (!ks)
		return;

	crypto_free_aead(ks->encrypt);
	crypto_free_aead(ks->decrypt);
	kfree(ks);
}

struct ovpn_crypto_key_slot *
ovpn_aead_crypto_key_slot_new(const struct ovpn_key_config *kc)
{
	struct ovpn_crypto_key_slot *ks = NULL;
	const char *alg_name;
	int ret;

	/* validate crypto alg */
	switch (kc->cipher_alg) {
	case OVPN_CIPHER_ALG_AES_GCM:
		alg_name = "gcm(aes)";
		break;
	case OVPN_CIPHER_ALG_CHACHA20_POLY1305:
		alg_name = "rfc7539(chacha20,poly1305)";
		break;
	default:
		return ERR_PTR(-EOPNOTSUPP);
	}

	if (sizeof(struct ovpn_nonce_tail) != kc->encrypt.nonce_tail_size ||
	    sizeof(struct ovpn_nonce_tail) != kc->decrypt.nonce_tail_size)
		return ERR_PTR(-EINVAL);

	/* build the key slot */
	ks = kmalloc(sizeof(*ks), GFP_KERNEL);
	if (!ks)
		return ERR_PTR(-ENOMEM);

	ks->encrypt = NULL;
	ks->decrypt = NULL;
	kref_init(&ks->refcount);
	ks->key_id = kc->key_id;

	ks->encrypt = ovpn_aead_init("encrypt", alg_name,
				     kc->encrypt.cipher_key,
				     kc->encrypt.cipher_key_size);
	if (IS_ERR(ks->encrypt)) {
		ret = PTR_ERR(ks->encrypt);
		ks->encrypt = NULL;
		goto destroy_ks;
	}

	ks->decrypt = ovpn_aead_init("decrypt", alg_name,
				     kc->decrypt.cipher_key,
				     kc->decrypt.cipher_key_size);
	if (IS_ERR(ks->decrypt)) {
		ret = PTR_ERR(ks->decrypt);
		ks->decrypt = NULL;
		goto destroy_ks;
	}

	memcpy(ks->nonce_tail_xmit.u8, kc->encrypt.nonce_tail,
	       sizeof(struct ovpn_nonce_tail));
	memcpy(ks->nonce_tail_recv.u8, kc->decrypt.nonce_tail,
	       sizeof(struct ovpn_nonce_tail));

	/* init packet ID generation/validation */
	ovpn_pktid_xmit_init(&ks->pid_xmit);
	ovpn_pktid_recv_init(&ks->pid_recv);

	return ks;

destroy_ks:
	ovpn_aead_crypto_key_slot_destroy(ks);
	return ERR_PTR(ret);
}
