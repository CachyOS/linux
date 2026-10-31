// SPDX-License-Identifier: GPL-2.0
/*
 * nap_fpu.c -- scalar float work: features, decision, label construction.
 * The network itself lives in the per-ISA files (nap_nn_*.c), selected
 * through nap_ops.
 */
#include <linux/cpuidle.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/crc32.h>
#include "nap.h"
#include "nap_weights.h"
#include "nap_selftest.h"

/*
 * ln(x) for x > 0 via exponent split + atanh series (error ~1e-8);
 * log2 = ln * log2(e).
 */
static float nap_log2(float x)
{
	union { float f; u32 i; } u = { .f = x < 1.0f ? 1.0f : x };
	int e = (int)((u.i >> 23) & 0xFF) - 127;
	float t;

	u.i = (u.i & 0x7FFFFFu) | (127u << 23);
	t = u.f - 1.0f;
	return (float)e + t * (1.44247531f + t * (-0.717557872f + t * (0.455527088f + t * (-0.274623258f +
		t * (0.119298238f + t * -0.0251232033f)))));
}

static float nap_log1p(float x)
{
	return nap_log2(1.0f + (x > 0.0f ? x : 0.0f)) * 0.69314718f;
}

/*
 * 1 / (1 + 2^(-log2(e) x)) for four logits at a time (SSE2 vector extension):
 * 2^y = 2^floor(y) * poly(frac(y)), the integer power through the exponent
 * field.  Error ~1e-7, clamped to y in [-60, 60].
 */
typedef float v4f __attribute__((vector_size(16)));
typedef int v4i __attribute__((vector_size(16)));
static inline v4f nap_sigmoid4(v4f x)
{
	const v4f hi = { 60.0f, 60.0f, 60.0f, 60.0f }, lo = -hi;
	v4f a = x * -1.44269504f, f, p;
	v4i xi, m;

	m = a > hi;
	a = (v4f)(((v4i)hi & m) | ((v4i)a & ~m));
	m = a < lo;
	a = (v4f)(((v4i)lo & m) | ((v4i)a & ~m));
	xi = __builtin_convertvector(a, v4i);		/* truncates */
	xi -= (v4i)(a < __builtin_convertvector(xi, v4f)) & 1;	/* floor */
	f = a - __builtin_convertvector(xi, v4f);
	p = 1.0f + f * (0.6931472f + f * (0.2402265f +
					   f * (0.0555041f + f * 0.0096181f)));
	m = (xi + 127) << 23;
	return 1.0f / (1.0f + (v4f)m * p);
}

static float flt(u64 v)
{
	return (float)(s64)v;
}

/*
 * The raw features, in the order used for training.  Seven are times
 * (log2 ns) and are embedded piecewise linearly, four are dense (see
 * nap_encode):
 *
 *   0 time to the next timer      1..5 the last five idles (net)
 *   6 length of the busy period   7 time to the timer / time to the tick
 *   8 busy CPUs (log1p)           9 device irqs in the last idle (log1p)
 *   10 local timer irqs in the last idle (log1p)
 */
#define NAP_NFEAT 11
static void nap_features(const struct nap_raw *r, float x[NAP_NFEAT])
{
	float ls = nap_log2(flt(r->sleep_ns));
	int i;

	x[0] = ls > 30.0f ? 30.0f : ls;
	for (i = 0; i < NAP_HIST; i++)
		x[1 + i] = nap_log2(flt(r->hist_net[i]));
	x[6] = nap_log2(flt(r->busy_ns));
	x[7] = ls - nap_log2(flt(r->tick_ns));
	x[8] = nap_log1p(flt(r->nr_cpus > r->idle_now ?
			     r->nr_cpus - r->idle_now : 0));
	x[9] = nap_log1p(flt(r->prev_d_dev));
	x[10] = nap_log1p(flt(r->prev_d_loc));
}

/* position in the raw feature vector of the time / dense features */
static const u8 nap_ti[NAP_NT] = { 0, 1, 2, 3, 4, 5, 6 };
static const u8 nap_di[NAP_ND] = { 7, 8, 9, 10 };

/*
 * Raw features -> sparse input of the first layer.  A time feature
 * (log2 ns) selects the two knots around it, one octave apart starting at
 * 2^NAP_GRID_LOG2_FIRST, with linear-interpolation weights; a dense
 * feature is standardised.
 */
static void nap_encode(struct nap_cpu *d, const float x[NAP_NFEAT])
{
	const float top = (float)(NAP_NK - 1) - 1e-4f;
	int t, i, n = 0;

	for (t = 0; t < NAP_NT; t++) {
		float u = x[nap_ti[t]] - (float)NAP_GRID_LOG2_FIRST;
		int k;

		u = u < 0.0f ? 0.0f : (u > top ? top : u);
		k = (int)u;
		d->idx[n] = t * NAP_NK + k;
		d->coef[n++] = 1.0f - (u - (float)k);
		d->idx[n] = t * NAP_NK + k + 1;
		d->coef[n++] = u - (float)k;
	}
	for (i = 0; i < NAP_ND; i++) {
		float c = (x[nap_di[i]] - nap_mu[i]) / nap_sd[i];

#ifndef CONFIG_X86
		/*
		 * x86-only feature (local timer irqs): the weights were
		 * dropout-trained for its absence, so feed the training mean.
		 */
		if (nap_di[i] == 10)
			c = 0.0f;
#endif
		d->idx[n] = NAP_NT * NAP_NK + i;
		d->coef[n++] = c;
	}
}

/*
 * Encode the features, run the selected network, turn the logits into
 * survival probabilities.
 */
static void nap_forward(const struct nap_nn_ops *ops, struct nap_cpu *d,
			const float x[NAP_NFEAT])
{
	int i;

	nap_encode(d, x);
	ops->forward(d);
	for (i = 0; i + 4 <= NAP_NOUT; i += 4) {
		v4f l, r;
		memcpy(&l, &d->logit[i], sizeof(l));
		r = nap_sigmoid4(l);
		memcpy(&d->S[i], &r, sizeof(r));
	}
}

void nap_fpu_init(struct nap_cpu *d)
{
	int i, j, o;

	memset(d->w1c, 0, sizeof(d->w1c));
	memset(d->w2r, 0, sizeof(d->w2r));
	memset(d->b2, 0, sizeof(d->b2));
	memset(d->g, 0, sizeof(d->g));
	for (i = 0; i < NAP_NROW; i++)
		/* header: rows of the first layer, [row][hidden] */
		memcpy(d->w1c[i], &nap_w1[i * NAP_NHID], sizeof(d->w1c[i]));
	memcpy(d->b1, nap_b1, sizeof(d->b1));
	for (o = 0; o < NAP_NOUT; o++) {
		d->b2[o] = nap_b2[o];
		for (j = 0; j < NAP_NHID; j++)
			/* header: [out][hidden] */
			d->w2r[j][o] = nap_w2[o * NAP_NHID + j];
	}
	d->lambda = 1.0f;
	d->o_ewma = 0.0f;
	d->c_ewma = 0.0f;
	d->fwd_valid = false;
	d->inited = true;
}

/* identifies the input scaling the weights were trained with */
u32 nap_fpu_input_sig(void)
{
	u32 c = crc32_le(~0, (const u8 *)nap_mu, sizeof(nap_mu));

	return crc32_le(c, (const u8 *)nap_sd, sizeof(nap_sd));
}

const struct nap_nn_ops *nap_isa_ops(enum nap_isa isa)
{
	switch (isa) {
	case NAP_ISA_AVX512:
		return &nap_nn_avx512;
	case NAP_ISA_AVX2:
		return &nap_nn_avx2;
	case NAP_ISA_SSE2:
		return &nap_nn_sse2;
	default:
		return &nap_nn_scalar;
	}
}

static float fabs_(float x)
{
	return x < 0.0f ? -x : x;
}

/*
 * Self-test of one implementation against the offline model (forward) and
 * against the scalar reference (one SGD step).  Returns the number of
 * forward vectors checked.
 */
int nap_fpu_selftest(unsigned int isa, unsigned int *worst_fwd_e6,
		     unsigned int *worst_learn_e6)
{
	static struct nap_cpu a, b;	/* init-time only, single caller */
	const struct nap_nn_ops *ops = nap_isa_ops(isa);
	float worst = 0.0f, wl = 0.0f;
	const float *pa, *pb;
	int v, k;
	size_t n;

	nap_fpu_init(&a);
	for (v = 0; v < NAP_NSELFTEST; v++) {
		nap_forward(ops, &a, &nap_st_in[v * NAP_NFEAT]);
		for (k = 0; k < NAP_NOUT; k++) {
			float e = fabs_(a.S[k] -
					nap_st_out[v * NAP_NOUT + k]);

			if (e > worst)
				worst = e;
		}
	}
	/* one learning step from the same state: scalar vs this ISA */
	nap_fpu_init(&a);
	nap_fpu_init(&b);
	nap_forward(&nap_nn_scalar, &a, &nap_st_in[0]);
	nap_forward(ops, &b, &nap_st_in[0]);
	for (k = 0; k < NAP_NOUT; k++)
		a.g[k] = b.g[k] = (k & 1) ? -0.37f : 0.52f;
	nap_nn_scalar.learn(&a, 0.01f);
	ops->learn(&b, 0.01f);
	pa = (const float *)a.w1c;
	pb = (const float *)b.w1c;
	n = (sizeof(a.w1c) + sizeof(a.b1) + sizeof(a.w2r) + sizeof(a.b2)) /
	    sizeof(float);
	/* the four weight arrays are contiguous at the start of nap_cpu */
	for (k = 0; k < (int)n; k++) {
		float e = fabs_(pa[k] - pb[k]);

		if (e > wl)
			wl = e;
	}
	*worst_fwd_e6 = (unsigned int)(worst * 1e6f);
	*worst_learn_e6 = (unsigned int)(wl * 1e6f);
	return NAP_NSELFTEST;
}

/*
 * Bins of the idle-time distribution implied by the grid: bin 0 = below
 * 2^FIRST, bin b (1..G-1) = [2^(FIRST+b-1), 2^(FIRST+b)), bin G = beyond the
 * last point (its representative time, NAP_TAIL_REP_NS, comes from the
 * training data).
 */
#define NB (NAP_NOUT + 1)

static void nap_bin_times(float Tb[NB])
{
	int b;

	Tb[0] = (float)(1ULL << (NAP_GRID_LOG2_FIRST - 1));
	for (b = 1; b < NAP_NOUT; b++)
		/* mean of a log-uniform bin [t, 2t) */
		Tb[b] = 1.4427f *
			(float)(1ULL << (NAP_GRID_LOG2_FIRST - 1 + b));
	Tb[NAP_NOUT] = (float)NAP_TAIL_REP_NS;
}

/*
 * Masked BCE SGD step on the previous idle's stored z / hid / S.
 * Label g = [idle >= 2^(FIRST+g)].
 */
static void nap_learn_step(const struct nap_nn_ops *ops, struct nap_cpu *d,
			   const struct nap_raw *r)
{
	float lr = (float)nap_lr_micro * 1e-6f;
	float T = flt(r->last_T_net), tick = flt(r->last_tick);
	bool cens = !r->last_stop && r->last_tick < r->last_sleep &&
		    T >= 0.9f * tick;
	int o;

	for (o = 0; o < NAP_NOUT; o++) {
		float thr = (float)(1ULL << (NAP_GRID_LOG2_FIRST + o));
		float y = thr <= T ? 1.0f : 0.0f;
		/* unknown above the tick when censored */
		float m = (cens && thr > T) ? 0.0f : 1.0f;

		d->g[o] = m * (d->S[o] - y);
	}
	ops->learn(d, lr);
}

/* deepest enabled state whose target residency fits into T */
static int nap_ideal(const float *tgt, int ns, struct cpuidle_device *dev,
		     float T)
{
	int j;

	for (j = ns - 1; j > 0; j--)
		if (!dev->states_usage[j].disable && tgt[j] <= T)
			return j;
	return 0;
}

int nap_fpu_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		   struct nap_cpu *d, const struct nap_raw *r, bool *stop)
{
	const struct nap_nn_ops *ops = READ_ONCE(nap_ops);
	float tgt[NAP_MAXSTATE], e0[NAP_MAXSTATE], pw[NAP_MAXSTATE];
	float Sm[NAP_NOUT], p[NB], Tb[NB];
	float ex[NAP_MAXSTATE];
	float x[NAP_NFEAT], tick, lam, best = -1e30f;
	float cs = (float)nap_c_stop_ns, cw = (float)nap_c_wake_ns;
	float rho = (float)nap_rho_ppm, cur;
	int k, b, bk = 0, i, ns = drv->state_count;
	bool bs = false, mono = true;

	if (ns > NAP_MAXSTATE)
		ns = NAP_MAXSTATE;
	for (k = 0; k < ns; k++) {
		tgt[k] = (float)drv->states[k].target_residency_ns;
		if (k && tgt[k] < tgt[k - 1])
			mono = false;
		ex[k] = (float)drv->states[k].exit_latency_ns;
		pw[k] = (float)nap_pow_milli[k] * 1e-3f;
	}
	e0[0] = 0.0f;
	for (k = 1; k < ns; k++)
		e0[k] = e0[k - 1] + (pw[k - 1] - pw[k]) * tgt[k];

	if (unlikely(!d->inited || d->reset_pending)) {
		nap_fpu_init(d);
		d->reset_pending = false;
	}

	/*
	 * Feedback from the previous idle: learning step and the controller
	 * that keeps the share of the time lost to avoidable wakeup latency at
	 * the budget.
	 */
	if (r->have_sample && !r->last_short) {
		if (nap_learn && d->fwd_valid) {
			nap_learn_step(ops, d, r);
			d->n_learn++;
			d->fwd_valid = false;
		}
		d->o_ewma += (flt(r->last_O) - d->o_ewma) * (1.0f / 64.0f);
		d->c_ewma += (flt(r->last_cycle) - d->c_ewma) * (1.0f / 64.0f);
		/* lost share of the time, in ppm */
		cur = d->c_ewma > 1.0f ? d->o_ewma / d->c_ewma * 1e6f : 0.0f;
		if (cur > rho * 1.05f)
			d->lambda *= 1.02f;
		else if (cur < rho)
			d->lambda *= 0.98f;
		if (d->lambda < 1e-4f)
			d->lambda = 1e-4f;
		if (d->lambda > 1e4f)
			d->lambda = 1e4f;
	}

	nap_features(r, x);
	nap_forward(ops, d, x);
	d->fwd_valid = true;

	/*
	 * The distribution of the tick-unbounded idle time: monotone
	 * survival -> bin probabilities.
	 */
	Sm[0] = d->S[0];
	for (i = 1; i < NAP_NOUT; i++)
		Sm[i] = d->S[i] < Sm[i - 1] ? d->S[i] : Sm[i - 1];
	p[0] = 1.0f - Sm[0];
	for (b = 1; b < NAP_NOUT; b++)
		p[b] = Sm[b - 1] - Sm[b];
	p[NAP_NOUT] = Sm[NAP_NOUT - 1];

	tick = flt(r->tick_ns);
	if (tick < 1.0f)
		tick = 1.0f;
	lam = d->lambda;
	nap_bin_times(Tb);
	/*
	 * Joint (state, stop-tick) choice maximising
	 * -E[energy] - lambda * E[avoidable wakeup latency].
	 *
	 * For every state and tick choice the expectation is linear in the
	 * bin probabilities, so the bins are first folded into a few sums
	 * (sp: total probability, spT: sum of p * T, spnf / sprem / spr: whole
	 * ticks, remainder and probability of a remainder when the tick is
	 * kept, Pb / Qc: probability of each state being the one that fits the
	 * whole idle / its last piece).  Each candidate then costs O(states)
	 * instead of O(bins).  Nothing to gain from stopping the tick when the
	 * next timer is closer than one tick period, and a stopped tick makes
	 * the wakeup slow (measured: ~100 us on average on Zen 4).
	 */
	{
		float sp = 0.0f, spT = 0.0f, spnf = 0.0f, sprem = 0.0f, spr = 0.0f;
		float Pb[NAP_MAXSTATE] = { 0 }, Qc[NAP_MAXSTATE] = { 0 };

		/*
		 * jb for the bins in order of T: with ascending target
		 * residencies the states that fit T are a prefix of the table,
		 * which only grows with T, so the deepest enabled one is found
		 * by one sweep for all bins instead of a scan from the deepest
		 * state for each. Otherwise nap_ideal() as it was.
		 */
		float Tprev = 0.0f;
		int jbp = 0, jn = 1;

		for (b = 0; b < NB; b++) {
			float T = Tb[b], pb = p[b], nfb, remb;
			int jb, jc;

			if (pb < 1e-4f)
				continue;
			nfb = (float)(s64)(T / tick);
			remb = T - nfb * tick;
			if (T < Tprev)
				mono = false;
			Tprev = T;
			if (mono) {
				for (; jn < ns && tgt[jn] <= T; jn++)
					if (!dev->states_usage[jn].disable)
						jbp = jn;
				jb = jbp;
			} else {
				jb = nap_ideal(tgt, ns, dev, T);
			}
			/* less than a tick: the remainder is T itself */
			if (nfb == 0.0f && remb > 0.0f)
				jc = jb;
			else
				jc = nap_ideal(tgt, ns, dev,
					       remb > 0.0f ? remb : tick);
			sp += pb;
			spT += pb * T;
			spnf += pb * nfb;
			if (remb > 0.0f) {
				sprem += pb * remb;
				spr += pb;
			}
			Pb[jb] += pb;
			Qc[jc] += pb;
		}
		for (k = 0; k < ns; k++) {
			float keep_e = e0[k] + pw[k] * tick + cw;

			for (i = 0; i < 2; i++) {
				bool st = i;
				float E, O = 0.0f, v;
				int j;

				if (k == 0 && st)
					continue;
				if (st && r->sleep_ns < TICK_NSEC)
					continue;
				if (k == 0) {
					E = pw[0] * spT;
				} else if (st) {
					E = (e0[k] + cs) * sp + pw[k] * spT;
					for (j = 0; j < ns; j++)
						if (ex[k] > ex[j])
							O += Pb[j] * (ex[k] - ex[j]);
				} else {
					E = keep_e * spnf + (e0[k] + cw) * spr + pw[k] * sprem;
					for (j = 0; j < ns; j++)
						if (ex[k] > ex[j])
							O += Qc[j] * (ex[k] - ex[j]);
				}
				v = -E - lam * O;
				if (v > best) {
					best = v;
					bk = k;
					bs = st;
				}
			}
		}
	}

	/*
	 * Honour PM QoS and disabled states: fall back to the next shallower
	 * valid one.
	 */
	for (k = bk; k >= 1; k--) {
		if (dev->states_usage[k].disable)
			continue;
		if ((s64)drv->states[k].exit_latency_ns > r->latency_req)
			continue;
		break;
	}
	if (k < 1) {
		k = 0;
		bs = false;
	}
	*stop = k ? bs : false;
	return k;
}

/*
 * Time forward and learn of one implementation (average over many calls,
 * FPU section held by the caller).
 */
void nap_fpu_bench(unsigned int isa, u64 *fwd_ns10, u64 *learn_ns10)
{
	static struct nap_cpu b;
	const struct nap_nn_ops *ops = nap_isa_ops(isa);
	const int n = 2000;
	u64 t0, t1, t2;
	int i, k;

	nap_fpu_init(&b);
	for (k = 0; k < NAP_NOUT; k++)
		b.g[k] = (k & 1) ? -0.01f : 0.01f;
	nap_forward(ops, &b, &nap_st_in[0]);
	t0 = ktime_get_ns();
	for (i = 0; i < n; i++) {
		b.coef[0] = (float)(i & 7);
		ops->forward(&b);
	}
	t1 = ktime_get_ns();
	for (i = 0; i < n; i++)
		ops->learn(&b, 1e-6f);
	t2 = ktime_get_ns();
	*fwd_ns10 = (t1 - t0) * 10 / n;
	*learn_ns10 = (t2 - t1) * 10 / n;
}
