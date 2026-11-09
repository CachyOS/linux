# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the NAP cpuidle governor
#

obj-$(CONFIG_CPU_IDLE_GOV_NAP) += cpuidle_gov_nap.o

cpuidle_gov_nap-y := nap.o nap_fpu.o nap_nn_scalar.o nap_nn_sse2.o nap_nn_avx2.o nap_nn_avx512.o

# Kernel builds with -mno-sse -mno-sse2 -mno-avx -msoft-float -mno-80387
# -mno-fp-ret-in-387.  FPU/SIMD-using files need these removed and ISA
# flags explicitly added.
#
# CRITICAL: nap.o is intentionally compiled with NORMAL kernel flags
# (no FPU/SSE).  All floating-point code lives in nap_fpu.o and the
# nap_nn_*.o files.  This ensures the compiler cannot emit SSE instructions
# in governor callbacks (nap_select, nap_reflect, etc.), which would
# silently corrupt userspace FPU register state.
#
# Do NOT add CFLAGS_REMOVE/CFLAGS for nap.o — it must stay FPU-free.
#
# One network implementation per SIMD level, all built from nap_nn_impl.h:
# nap_nn_scalar (no vectorisation, the reference), _sse2, _avx2 (+FMA), _avx512.
FPU_KILL_FLAGS := -mno-sse -mno-sse2 -mno-mmx -mno-avx -mno-3dnow \
                  -mno-sse4a -msoft-float -mno-80387 -mno-fp-ret-in-387

CFLAGS_REMOVE_nap_fpu.o        += $(FPU_KILL_FLAGS)
CFLAGS_REMOVE_nap_nn_scalar.o  += $(FPU_KILL_FLAGS)
CFLAGS_REMOVE_nap_nn_sse2.o    += $(FPU_KILL_FLAGS)
CFLAGS_REMOVE_nap_nn_avx2.o    += $(FPU_KILL_FLAGS)
CFLAGS_REMOVE_nap_nn_avx512.o  += $(FPU_KILL_FLAGS)

CFLAGS_nap_fpu.o       += $(CC_FLAGS_FPU)
CFLAGS_nap_nn_scalar.o += $(CC_FLAGS_FPU) -fno-tree-vectorize -fno-tree-slp-vectorize
CFLAGS_nap_nn_sse2.o   += $(CC_FLAGS_FPU)
CFLAGS_nap_nn_avx2.o   += $(CC_FLAGS_FPU) -mavx -mavx2 -mfma
CFLAGS_nap_nn_avx512.o += $(CC_FLAGS_FPU) -mavx -mavx2 -mfma -mavx512f -mprefer-vector-width=512
