# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_T2BCE_CORE) += t2bce_core.o
t2bce_core-y := t2bce_main.o mailbox.o transport.o

ccflags-y += -DWITHOUT_NVME_PATCH
ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/../t2bce_dma/include
