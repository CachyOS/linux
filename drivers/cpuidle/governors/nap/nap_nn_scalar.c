// SPDX-License-Identifier: GPL-2.0
/*
 * Scalar implementation: plain loops, no vectorisation.  It is the
 * reference the other implementations are checked against at boot.
 */
#include "nap.h"

static void nap_scalar_forward(struct nap_cpu *d)
{
	int i, j, o;

	for (j = 0; j < NAP_NHID; j++) {
		float a = d->b1[j];

		for (i = 0; i < NAP_NNZ; i++)
			a += d->w1c[d->idx[i]][j] * d->coef[i];
		d->hid[j] = a > 0.0f ? a : 0.0f;
	}
	for (o = 0; o < NAP_NOUTP; o++) {
		float a = d->b2[o];

		for (j = 0; j < NAP_NHID; j++)
			a += d->w2r[j][o] * d->hid[j];
		d->logit[o] = a;
	}
}

static void nap_scalar_learn(struct nap_cpu *d, float lr)
{
	int i, j, o;

	for (j = 0; j < NAP_NHID; j++) {
		float a = 0.0f;

		for (o = 0; o < NAP_NOUT; o++)
			a += d->w2r[j][o] * d->g[o];
		d->gh[j] = d->hid[j] > 0.0f ? a : 0.0f;
	}
	for (j = 0; j < NAP_NHID; j++)
		for (o = 0; o < NAP_NOUTP; o++)
			d->w2r[j][o] -= lr * d->hid[j] * d->g[o];
	for (o = 0; o < NAP_NOUTP; o++)
		d->b2[o] -= lr * d->g[o];
	for (i = 0; i < NAP_NNZ; i++)
		for (j = 0; j < NAP_NHID; j++)
			d->w1c[d->idx[i]][j] -= lr * d->coef[i] * d->gh[j];
	for (j = 0; j < NAP_NHID; j++)
		d->b1[j] -= lr * d->gh[j];
}

const struct nap_nn_ops nap_nn_scalar = {
	.name = "scalar",
	.forward = nap_scalar_forward,
	.learn = nap_scalar_learn,
};
