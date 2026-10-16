# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_T2BCE_AUDIO) += t2bce_audio.o
t2bce_audio-y := audio.o protocol.o protocol_bce.o pcm.o

ccflags-y += -DWITHOUT_NVME_PATCH
ccflags-y += -I$(src)
ccflags-y += -I$(src)/../t2bce_core/include
