#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2020-2024 OpenVPN, Inc.
#
#  Author:	Antonio Quartulli <antonio@openvpn.net>

#set -x
set -e

UDP_PEERS_FILE=${UDP_PEERS_FILE:-udp_peers.txt}
TCP_PEERS_FILE=${TCP_PEERS_FILE:-tcp_peers.txt}
OVPN_CLI=${OVPN_CLI:-./ovpn-cli}
ALG=${ALG:-aes}
PROTO=${PROTO:-UDP}

create_ns() {
	ip netns add peer${1}
}

setup_ns() {
	MODE="P2P"

	if [ ${1} -eq 0 ]; then
		MODE="MP"
		for p in $(seq 1 ${NUM_PEERS}); do
			ip link add veth${p} netns peer0 type veth peer name veth${p} netns peer${p}

			ip -n peer0 addr add 10.10.${p}.1/24 dev veth${p}
			ip -n peer0 link set veth${p} up

			ip -n peer${p} addr add 10.10.${p}.2/24 dev veth${p}
			ip -n peer${p} link set veth${p} up
		done
	fi

	ip netns exec peer${1} ${OVPN_CLI} new_iface tun${1} $MODE
	ip -n peer${1} addr add ${2} dev tun${1}
	ip -n peer${1} link set tun${1} up
}

add_peer() {
	if [ "${PROTO}" == "UDP" ]; then
		if [ ${1} -eq 0 ]; then
			ip netns exec peer0 ${OVPN_CLI} new_multi_peer tun0 1 ${UDP_PEERS_FILE}

			for p in $(seq 1 ${NUM_PEERS}); do
				ip netns exec peer0 ${OVPN_CLI} new_key tun0 ${p} 1 0 ${ALG} 0 \
					data64.key
			done
		else
			ip netns exec peer${1} ${OVPN_CLI} new_peer tun${1} 1 ${1} 10.10.${1}.1 1
			ip netns exec peer${1} ${OVPN_CLI} new_key tun${1} ${1} 1 0 ${ALG} 1 \
				data64.key
		fi
	else
		if [ ${1} -eq 0 ]; then
			(ip netns exec peer0 ${OVPN_CLI} listen tun0 1 ${TCP_PEERS_FILE} && {
				for p in $(seq 1 ${NUM_PEERS}); do
					ip netns exec peer0 ${OVPN_CLI} new_key tun0 ${p} 1 0 \
						${ALG} 0 data64.key
				done
			}) &
			sleep 5
		else
			ip netns exec peer${1} ${OVPN_CLI} connect tun${1} ${1} 10.10.${1}.1 1 \
				5.5.5.1 data64.key
		fi
	fi
}

cleanup() {
	for p in $(seq 1 10); do
		ip -n peer0 link del veth${p} 2>/dev/null || true
	done
	for p in $(seq 0 10); do
		ip netns exec peer${p} ${OVPN_CLI} del_iface tun${p} 2>/dev/null || true
		ip netns del peer${p} 2>/dev/null || true
	done
}

if [ "${PROTO}" == "UDP" ]; then
	NUM_PEERS=${NUM_PEERS:-$(wc -l ${UDP_PEERS_FILE} | awk '{print $1}')}
else
	NUM_PEERS=${NUM_PEERS:-$(wc -l ${TCP_PEERS_FILE} | awk '{print $1}')}
fi

cleanup

modprobe -q ovpn || true

for p in $(seq 0 ${NUM_PEERS}); do
	create_ns ${p}
done

for p in $(seq 0 ${NUM_PEERS}); do
	setup_ns ${p} 5.5.5.$((${p} + 1))/24
done

for p in $(seq 0 ${NUM_PEERS}); do
	add_peer ${p}
done

for p in $(seq 1 ${NUM_PEERS}); do
	ip netns exec peer0 ${OVPN_CLI} set_peer tun0 ${p} 60 120
	ip netns exec peer${p} ${OVPN_CLI} set_peer tun${p} ${p} 60 120
done

for p in $(seq 1 ${NUM_PEERS}); do
	ip netns exec peer0 ping -qfc 1000 -w 5 5.5.5.$((${p} + 1))
done
# make clients float..
for p in $(seq 1 ${NUM_PEERS}); do
	ip -n peer${p} addr del 10.10.${p}.2/24 dev veth${p}
	ip -n peer${p} addr add 10.10.${p}.3/24 dev veth${p}
done
for p in $(seq 1 ${NUM_PEERS}); do
	ip netns exec peer${p} ping -qfc 1000 -w 5 5.5.5.1
done

cleanup

modprobe -r ovpn || true
