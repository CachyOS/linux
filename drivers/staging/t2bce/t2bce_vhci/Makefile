# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_T2BCE_VHCI) += t2bce_vhci.o
t2bce_vhci-y := vhci.o queue.o transfer.o

ccflags-y += -DWITHOUT_NVME_PATCH
ccflags-y += -I$(src)
ccflags-y += -I$(src)/../t2bce_core/include
