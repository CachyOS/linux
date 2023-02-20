# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Entropy Source and DRNG Manager.
#

obj-y					+= lrng_es_mgr.o lrng_drng_mgr.o \
					   lrng_es_aux.o
obj-$(CONFIG_LRNG_SHA256)		+= lrng_sha256.o
obj-$(CONFIG_LRNG_SHA1)			+= lrng_sha1.o

obj-$(CONFIG_SYSCTL)			+= lrng_proc.o
obj-$(CONFIG_NUMA)			+= lrng_numa.o

obj-$(CONFIG_LRNG_SWITCH)		+= lrng_switch.o
obj-$(CONFIG_LRNG_HASH_KCAPI)		+= lrng_hash_kcapi.o
obj-$(CONFIG_LRNG_DRNG_CHACHA20)	+= lrng_drng_chacha20.o
obj-$(CONFIG_LRNG_DRBG)			+= lrng_drng_drbg.o
