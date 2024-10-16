/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 *		James Yonan <james@openvpn.net>
 */

#ifndef _NET_OVPN_PACKET_H_
#define _NET_OVPN_PACKET_H_

/* When the OpenVPN protocol is ran in AEAD mode, use
 * the OpenVPN packet ID as the AEAD nonce:
 *
 *    00000005 521c3b01 4308c041
 *    [seq # ] [  nonce_tail   ]
 *    [     12-byte full IV    ] -> NONCE_SIZE
 *    [4-bytes                   -> NONCE_WIRE_SIZE
 *    on wire]
 */

/* OpenVPN nonce size */
#define NONCE_SIZE 12

/* OpenVPN nonce size reduced by 8-byte nonce tail -- this is the
 * size of the AEAD Associated Data (AD) sent over the wire
 * and is normally the head of the IV
 */
#define NONCE_WIRE_SIZE (NONCE_SIZE - sizeof(struct ovpn_nonce_tail))

/* Last 8 bytes of AEAD nonce
 * Provided by userspace and usually derived from
 * key material generated during TLS handshake
 */
struct ovpn_nonce_tail {
	u8 u8[OVPN_NONCE_TAIL_SIZE];
};

#endif /* _NET_OVPN_PACKET_H_ */
