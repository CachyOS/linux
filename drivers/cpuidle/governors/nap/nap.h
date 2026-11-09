/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NAP_H
#define NAP_H
#include <linux/cpuidle.h>
#include "nap_dims.h"

#define NAP_HIST      5
#define NAP_MAXSTATE  CPUIDLE_STATE_MAX
/* outputs padded to the widest vector (AVX-512: 16 lanes) */
#define NAP_NOUTP     (((NAP_NOUT + 15) / 16) * 16)

/* ISA ladder: the user sets the maximum, the CPU may cap it lower. */
enum nap_isa {
	NAP_ISA_SCALAR,
	NAP_ISA_SSE2,
	NAP_ISA_AVX2,
	NAP_ISA_AVX512,
	NAP_ISA_MAX
};

/*
 * Integer signals gathered by the non-FPU side; all float work happens in
 * the FPU translation units.
 */
struct nap_raw {
	u64 sleep_ns, tick_ns, busy_ns;
	/* net idle of the previous idles, [0] = most recent */
	u64 hist_net[NAP_HIST];
	u32 idle_now, nr_cpus;
	u32 prev_d_dev, prev_d_loc;
	s64 latency_req;
	bool have_sample;
	u64  last_T_net, last_O, last_tick, last_sleep, last_cycle;
	bool last_stop, last_short;
};

/*
 * Network state in SIMD-friendly layout.  The first layer is a table of rows
 * (w1c[row][hidden]) and the input is sparse: NAP_NNZ (row, coefficient)
 * pairs.  A time feature selects the two table rows around its value with
 * linear-interpolation weights (a piecewise-linear embedding); a dense
 * feature selects its own row with its standardised value.  Each pair is
 * broadcast and FMA'd across all hidden neurons.  The output layer is
 * w2r[hidden][out]: the forward pass broadcasts the hidden activation across
 * the output vector; backprop dots each row with the output gradient (see
 * nap_nn_gh.h).  The structure is ~80 KB, too big for alloc_percpu().
 */
struct nap_cpu {
	float w1c[NAP_NROW][NAP_NHID];
	float b1[NAP_NHID];
	float w2r[NAP_NHID][NAP_NOUTP];
	float b2[NAP_NOUTP];
	float coef[NAP_NNZ];
	u16 idx[NAP_NNZ];
	float hid[NAP_NHID];
	float logit[NAP_NOUTP];
	float g[NAP_NOUTP];
	float gh[NAP_NHID];
	float S[NAP_NOUT];
	float lambda, o_ewma, c_ewma;
	bool fwd_valid, inited;

	/* ---- integer side (nap.c) ---- */
	int cached_min_state;		/* cached shallowest valid state */
	s64 cached_min_state_latency;	/* latency_req at that time */
	unsigned long cached_min_state_jiffies; /* jiffies at that time */
	u64 total_residency_ns;
	bool reset_pending;		/* set by sysfs, see nap_select */
	u64 hist_net[NAP_HIST];
	u64 prev_ref_ts;
	u32 prev_d_dev, prev_d_loc;
	u64 ts_sel, sleep_ns, tick_ns;
	u32 dev0, loc0;
	int sel_k;
	bool sel_stop, active, short_circuited;
	bool have_sample;
	u64 last_T_net, last_O, last_tick, last_sleep, last_cycle;
	bool last_stop, last_short;
	u64 n_select, n_overshoot, n_learn, n_stop;
	u64 lat_ns, lat_last;		/* avoidable wakeup latency */
	u64 n_state[NAP_MAXSTATE];
} __aligned(64);

/* one per possible CPU, allocated node-local at init */
extern struct nap_cpu **nap_cpus;

/* one implementation per ISA; all operate on the same data layout */
struct nap_nn_ops {
	const char *name;
	/* idx, coef -> hid, logit */
	void (*forward)(struct nap_cpu *d);
	/* SGD step; uses g (zero-padded), hid, idx, coef */
	void (*learn)(struct nap_cpu *d, float lr);
};
extern const struct nap_nn_ops nap_nn_scalar, nap_nn_sse2, nap_nn_avx2,
			       nap_nn_avx512;

extern unsigned int nap_rho_ppm;	/* share of the time lost to avoidable wakeup latency */
extern unsigned int nap_lr_micro;	/* online learning rate * 1e6 */
extern unsigned int nap_pow_milli[NAP_MAXSTATE];
extern unsigned int nap_c_stop_ns, nap_c_wake_ns;
extern bool nap_learn;
/* currently selected implementation */
extern const struct nap_nn_ops *nap_ops;
extern const char * const nap_isa_name[NAP_ISA_MAX];

/* FPU entry points: call only inside kernel_fpu_begin()/end() */
int  nap_fpu_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		    struct nap_cpu *d, const struct nap_raw *raw, bool *stop);
void nap_fpu_init(struct nap_cpu *d);
u32  nap_fpu_input_sig(void);
int  nap_fpu_selftest(unsigned int isa, unsigned int *worst_fwd_e6,
		      unsigned int *worst_learn_e6);
const struct nap_nn_ops *nap_isa_ops(enum nap_isa isa);
void nap_fpu_bench(unsigned int isa, u64 *fwd_ns10, u64 *learn_ns10);
#endif
