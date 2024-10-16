# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2020-2024 OpenVPN, Inc.
#
CFLAGS = -Wall -Wl,--no-as-needed -g -O2 $(KHDR_INCLUDES)
CFLAGS += $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)

LDFLAGS = -lmbedtls -lmbedcrypto
LDFLAGS += $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

TEST_PROGS = data-test.sh \
	data-test-tcp.sh \
	float-test.sh

TEST_GEN_FILES = ovpn-cli

include ../../lib.mk
