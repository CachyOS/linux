// SPDX-License-Identifier: GPL-2.0
/* avx2 implementation: 8-lane vectors (flags set in the Makefile). */
#define NAP_VL 8
#define NAP_SYM(n) nap_avx2_##n
#include "nap_nn_impl.h"

const struct nap_nn_ops nap_nn_avx2 = {
	.name = "avx2",
	.forward = nap_avx2_forward,
	.learn = nap_avx2_learn,
};
