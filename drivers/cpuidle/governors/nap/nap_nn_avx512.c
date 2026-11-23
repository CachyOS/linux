// SPDX-License-Identifier: GPL-2.0
/* avx512 implementation: 16-lane vectors (flags set in the Makefile). */
#define NAP_VL 16
#define NAP_SYM(n) nap_avx512_##n
#include "nap_nn_impl.h"

const struct nap_nn_ops nap_nn_avx512 = {
	.name = "avx512",
	.forward = nap_avx512_forward,
	.learn = nap_avx512_learn,
};
