// SPDX-License-Identifier: GPL-2.0
/* sse2 implementation: 4-lane vectors (flags set in the Makefile). */
#define NAP_VL 4
#define NAP_SYM(n) nap_sse2_##n
#include "nap_nn_impl.h"

const struct nap_nn_ops nap_nn_sse2 = {
	.name = "sse2",
	.forward = nap_sse2_forward,
	.learn = nap_sse2_learn,
};
