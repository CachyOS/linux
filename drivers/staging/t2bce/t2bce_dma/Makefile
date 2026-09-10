# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_T2BCE_DMA) += t2bce_dma.o
t2bce_dma-y := queue.o

ccflags-y += -DWITHOUT_NVME_PATCH
ccflags-y += -I$(src)/include
