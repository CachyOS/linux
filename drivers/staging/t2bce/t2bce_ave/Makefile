# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_T2BCE_AVE) += t2bce_ave.o
t2bce_ave-y := ave.o protocol.o encoder.o video.o

ccflags-y += -DWITHOUT_NVME_PATCH
ccflags-y += -I$(src)
ccflags-y += -I$(src)/../t2bce_core/include
