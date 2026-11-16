/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared body of the SIMD implementations.  The including file defines
 * NAP_VL (floats per vector) and NAP_SYM(name) before including.  GCC vector
 * extensions are used (no <immintrin.h> in the kernel); the compiler lowers
 * them to SSE2 / AVX2+FMA / AVX-512 according to the flags of the including
 * file.
 */
#include <linux/string.h>
#include "nap.h"

typedef float vf __attribute__((vector_size(NAP_VL * 4)));
typedef int   vi __attribute__((vector_size(NAP_VL * 4)));

#define HV (NAP_NHID / NAP_VL)
#define OV (NAP_NOUTP / NAP_VL)

static inline vf ld(const float *p)
{
	vf v;

	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

static inline void st(float *p, vf v)
{
	__builtin_memcpy(p, &v, sizeof(v));
}

static inline vf bc(float x)
{
	vf v;
	int l;

	for (l = 0; l < NAP_VL; l++)
		v[l] = x;
	return v;
}

static inline vf relu(vf a)
{
	vi m = a > (vf){ 0 };		/* all-ones where a > 0 */

	return (vf)((vi)a & m);
}

/*
 * Both layers accumulate into two independent sets of vectors (even and odd
 * terms) that are added at the end.  With a single set, the chain of
 * dependent FMAs (one per input / hidden neuron) is limited by the FMA
 * latency instead of the throughput.
 */
static_assert(NAP_NNZ % 2 == 0 && NAP_NHID % 2 == 0);

static void NAP_SYM(forward)(struct nap_cpu *d)
{
	vf h[HV], h2[HV], o[OV], o2[OV];
	int i, j, k;

	for (k = 0; k < HV; k++) {
		h[k] = ld(&d->b1[k * NAP_VL]);
		h2[k] = (vf){ 0 };
	}
	for (i = 0; i < NAP_NNZ; i += 2) {
		vf zi = bc(d->coef[i]), zj = bc(d->coef[i + 1]);
		const float *ri = d->w1c[d->idx[i]];
		const float *rj = d->w1c[d->idx[i + 1]];

		for (k = 0; k < HV; k++) {
			h[k] += zi * ld(&ri[k * NAP_VL]);
			h2[k] += zj * ld(&rj[k * NAP_VL]);
		}
	}
	for (k = 0; k < HV; k++) {
		h[k] = relu(h[k] + h2[k]);
		st(&d->hid[k * NAP_VL], h[k]);
	}
	for (k = 0; k < OV; k++) {
		o[k] = ld(&d->b2[k * NAP_VL]);
		o2[k] = (vf){ 0 };
	}
	for (j = 0; j < NAP_NHID; j += 2) {
		vf hj = bc(d->hid[j]), hk = bc(d->hid[j + 1]);

		for (k = 0; k < OV; k++) {
			o[k] += hj * ld(&d->w2r[j][k * NAP_VL]);
			o2[k] += hk * ld(&d->w2r[j + 1][k * NAP_VL]);
		}
	}
	for (k = 0; k < OV; k++)
		st(&d->logit[k * NAP_VL], o[k] + o2[k]);
}

#include "nap_nn_gh.h"

static void NAP_SYM(learn)(struct nap_cpu *d, float lr)
{
	vf g[OV], gh[HV];
	int i, j, k;

	for (k = 0; k < OV; k++)
		g[k] = ld(&d->g[k * NAP_VL]);
	NAP_SYM(gh)(d, g);
	for (k = 0; k < HV; k++)
		gh[k] = ld(&d->gh[k * NAP_VL]);

	/* output layer, then bias */
	for (j = 0; j < NAP_NHID; j++) {
		vf c = bc(lr * d->hid[j]);

		for (k = 0; k < OV; k++)
			st(&d->w2r[j][k * NAP_VL],
			   ld(&d->w2r[j][k * NAP_VL]) - c * g[k]);
	}
	for (k = 0; k < OV; k++)
		st(&d->b2[k * NAP_VL],
		   ld(&d->b2[k * NAP_VL]) - bc(lr) * g[k]);

	/* hidden layer */
	for (i = 0; i < NAP_NNZ; i++) {
		vf c = bc(lr * d->coef[i]);
		float *r = d->w1c[d->idx[i]];

		for (k = 0; k < HV; k++)
			st(&r[k * NAP_VL], ld(&r[k * NAP_VL]) - c * gh[k]);
	}
	for (k = 0; k < HV; k++)
		st(&d->b1[k * NAP_VL],
		   ld(&d->b1[k * NAP_VL]) - bc(lr) * gh[k]);
}
