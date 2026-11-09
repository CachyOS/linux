/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gh = relu'(hid) * (W2 g): the hidden-layer gradient, hand-tuned per vector
 * width.  Included by nap_nn_impl.h after the vector types and helpers.
 *
 * Each w2r row is dotted with the output gradient g, NAP_VL rows at a time;
 * the NAP_VL vectors of partial sums are then reduced to one vector (lane i =
 * sum of row i) with a transpose-add tree.  Keeping the output layer in a
 * single w2r[hidden][out] layout and reducing with a tree is cheaper than
 * keeping a transposed copy up to date (measured on Zen 4 and Zen 5).
 *
 * Built-ins are used instead of <immintrin.h>, which the kernel cannot use:
 * __builtin_ia32_haddps256 / __builtin_ia32_vperm2f128_ps256 exist in both
 * GCC and clang, and __builtin_shufflevector needs GCC 12 or clang (see
 * Kconfig).
 */

#if NAP_VL == 4
/* SSE2: 4 rows at a time, unpack tree */
static inline void NAP_SYM(gh)(struct nap_cpu *d, const vf g[OV])
{
	int j, r, k;

	for (j = 0; j < NAP_NHID; j += 4) {
		vf s[4], u0, u1, sum;
		vi m;

		for (r = 0; r < 4; r++) {
			vf a = ld(&d->w2r[j + r][0]) * g[0];

			for (k = 1; k < OV; k++)
				a += ld(&d->w2r[j + r][k * 4]) * g[k];
			s[r] = a;
		}
		u0 = __builtin_shufflevector(s[0], s[1], 0, 4, 1, 5) +
		     __builtin_shufflevector(s[0], s[1], 2, 6, 3, 7);
		u1 = __builtin_shufflevector(s[2], s[3], 0, 4, 1, 5) +
		     __builtin_shufflevector(s[2], s[3], 2, 6, 3, 7);
		sum = __builtin_shufflevector(u0, u1, 0, 1, 4, 5) +
		      __builtin_shufflevector(u0, u1, 2, 3, 6, 7);
		m = ld(&d->hid[j]) > (vf){ 0 };
		st(&d->gh[j], (vf)((vi)sum & m));
	}
}

#elif NAP_VL == 8 || NAP_VL == 16
/* AVX2: 8 rows at a time, hadd tree */
typedef float vf8 __attribute__((vector_size(32)));
typedef int   vi8 __attribute__((vector_size(32)));

static inline vf8 NAP_SYM(tree8)(vf8 s[8])
{
	vf8 t0 = __builtin_ia32_haddps256(s[0], s[1]);
	vf8 t1 = __builtin_ia32_haddps256(s[2], s[3]);
	vf8 t2 = __builtin_ia32_haddps256(s[4], s[5]);
	vf8 t3 = __builtin_ia32_haddps256(s[6], s[7]);
	vf8 u0 = __builtin_ia32_haddps256(t0, t1);
	vf8 u1 = __builtin_ia32_haddps256(t2, t3);

	return __builtin_ia32_vperm2f128_ps256(u0, u1, 0x20) +
	       __builtin_ia32_vperm2f128_ps256(u0, u1, 0x31);
}

static inline void NAP_SYM(gh)(struct nap_cpu *d, const vf g[OV])
{
	int j, r, k;

	for (j = 0; j < NAP_NHID; j += 8) {
		vf8 s[8], sum, h;
		vi8 m;

		for (r = 0; r < 8; r++) {
#if NAP_VL == 8
			vf a = ld(&d->w2r[j + r][0]) * g[0];

			for (k = 1; k < OV; k++)
				a += ld(&d->w2r[j + r][k * 8]) * g[k];
			s[r] = a;
#else
			/* AVX-512: one 16-lane row; fold it to 8 lanes first */
			vf p = ld(&d->w2r[j + r][0]) * g[0];

			(void)k;
			s[r] = __builtin_shufflevector(p, p, 0, 1, 2, 3, 4, 5,
						       6, 7) +
			       __builtin_shufflevector(p, p, 8, 9, 10, 11, 12,
						       13, 14, 15);
#endif
		}
		sum = NAP_SYM(tree8)(s);
		__builtin_memcpy(&h, &d->hid[j], sizeof(h));
		m = h > (vf8){ 0 };
		sum = (vf8)((vi8)sum & m);
		__builtin_memcpy(&d->gh[j], &sum, sizeof(sum));
	}
}
#else
#error "unsupported vector width"
#endif
