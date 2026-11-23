// SPDX-License-Identifier: GPL-2.0
/*
 * nap.c — Neural Adaptive Predictor cpuidle governor
 *
 * A small online-learning network (11 features, the time features embedded
 * piecewise linearly, 32 hidden units, a 16-point time grid) predicts the
 * survival function of the upcoming idle time,
 * P(idle >= 2^(9+g) ns), i.e. 0.5 us .. 16 ms in octave steps.  Because the
 * output is a distribution over time rather than a fixed set of per-state
 * probabilities, the network does not depend on the idle-state table of the
 * machine: any state's target_residency is read off the grid by
 * interpolation.
 *
 * The decision is a joint (idle state, stop-tick) choice that maximises
 *
 *     -E[energy] - lambda * E[avoidable wakeup latency]
 *
 * over the predicted distribution.  The avoidable wakeup latency of an idle
 * is how much longer the wakeup took (exit_latency) than it would have from
 * the state that suits the idle, i.e. the deepest one whose target_residency
 * fits.  lambda is adapted per CPU so that the share of the wall-clock time
 * lost to it follows the budget set in
 * /sys/devices/system/cpu/nap/budget_ppm.  A budget per time rather than per
 * idle lets a CPU that wakes rarely take deep states and holds one that
 * wakes often to short exit latencies.  Energy is modelled from the
 * declared break-even (target_residency) and relative per-state powers
 * (power_milli), which are machine dependent.
 *
 * Weights are pre-trained offline (distilled from a larger model) and
 * refined by online SGD on every idle.  The network runs on scalar, SSE2,
 * AVX2+FMA or AVX-512 code selected at run time (max_simd); every
 * implementation is checked against the offline model and against the scalar
 * reference at boot, and an implementation that disagrees is never used.
 *
 * IMPORTANT: This file is compiled WITHOUT FPU/SSE flags (normal kernel
 * compilation).  All floating-point and SIMD code lives in nap_fpu.c and
 * nap_nn_*.c, which are compiled with the matching ISA flags.  This
 * separation ensures the compiler cannot emit SSE instructions in governor
 * callbacks (nap_select, nap_reflect, etc.), which would corrupt userspace
 * FPU state.
 */

#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/version.h>
#include <linux/kobject.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sched/clock.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/atomic.h>
#include <linux/crc32.h>
#include <linux/mutex.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <asm/simd.h>
#include <asm/fpu/api.h>
#include <asm/cpufeature.h>
#include <asm/hardirq.h>

#include "nap.h"

#ifdef NAP_OOT
/*
 * Out-of-tree test build: the cpuidle core symbols below are not exported to
 * modules, so the harness provides them.  The governor and its sysfs
 * directory get a different name so that a test build can be loaded next to
 * the built-in one.
 */
#include <linux/module.h>
extern ktime_t nap_oot_sleep_length(ktime_t *delta_next);
extern s64 nap_oot_latency_req(unsigned int cpu);
extern int nap_oot_register_governor(struct cpuidle_governor *gov);
#define nap_sleep_length		nap_oot_sleep_length
#define nap_latency_req			nap_oot_latency_req
#define nap_register_governor		nap_oot_register_governor
#define RESIDENCY_THRESHOLD_NS		(15 * NSEC_PER_USEC)
#else
#include "../gov.h"
#define nap_sleep_length		tick_nohz_get_sleep_length
#define nap_latency_req			cpuidle_governor_latency_req
#define nap_register_governor		cpuidle_register_governor
#endif

#ifndef NAP_GOV_NAME
#define NAP_GOV_NAME	"nap"
#endif
#ifndef NAP_SYSFS_NAME
#define NAP_SYSFS_NAME	"nap"
#endif
#ifndef NAP_RATING
#define NAP_RATING	26
#endif

/**************************************************************
 * Version Information:
 */

#define CPUIDLE_NAP_PROGNAME "Nap CPUIdle Governor"
#define CPUIDLE_NAP_AUTHOR   "Masahito Suzuki"

#define CPUIDLE_NAP_VERSION  "0.6.6"

/* ================================================================
 * Tunables (sysfs)
 * ================================================================ */

/* budget: share of the wall-clock time lost to avoidable wakeup latency */
unsigned int nap_rho_ppm = 4000;
/* online learning rate * 1e6 (0.02) */
unsigned int nap_lr_micro = 20000;
/*
 * Relative power of each idle state, shallowest first.  Machine dependent:
 * calibrate with RAPL or similar.
 */
unsigned int nap_pow_milli[NAP_MAXSTATE] = {
	1000, 500, 200, 50, 30, 20, 14, 10, 7, 5
};
/* cost of stopping + restarting the tick */
unsigned int nap_c_stop_ns = 3000;
/* cost of one extra tick wake-up (re-entry) */
unsigned int nap_c_wake_ns = 5000;
bool nap_learn = true;

/*
 * POLL short-circuit tunables: dev->poll_limit_ns floor and margin (the POLL
 * state samples its own timeout only every ~1 us).
 */
#define NAP_POLL_LIMIT_MIN_NS      1000ULL
#define NAP_POLL_LIMIT_MARGIN_NS   1000ULL
/*
 * Refresh interval of the cached minimum-valid-state lookup; bounds the
 * staleness from sysfs state-disable events.
 */
#define NAP_MIN_STATE_REFRESH_JIFFIES  HZ

/* ================================================================
 * SIMD level: max_simd = scalar | sse2 | avx2 | avx512
 * (the processor may cap it lower)
 * ================================================================ */

const char * const nap_isa_name[NAP_ISA_MAX] = {
	"scalar", "sse2", "avx2", "avx512"
};
/* what the user asked for */
static int nap_isa_req = NAP_ISA_AVX512;
/* what the processor and the boot self-test allow */
static int nap_isa_cpu = NAP_ISA_SCALAR;
const struct nap_nn_ops *nap_ops = &nap_nn_scalar;

static void nap_apply_isa(void)
{
	WRITE_ONCE(nap_ops, nap_isa_ops(min(nap_isa_req, nap_isa_cpu)));
}

/* ================================================================
 * Per-CPU data and global signals
 * ================================================================ */

struct nap_cpu **nap_cpus;

/* Number of CPUs currently idle (cross-CPU context feature). */
static atomic_t nap_nr_idle = ATOMIC_INIT(0);

static void nap_irq_counts(u32 *dev, u32 *loc)
{
	*dev = (u32)this_cpu_read(kstat.irqs_sum);
#ifdef CONFIG_X86
	/*
	 * x86 keeps the local timer count outside kstat; the weights were
	 * trained to cope without it.
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	/* 7.2 folded the per-class irq_stat fields into counts[] */
	*loc = this_cpu_read(irq_stat.counts[IRQ_COUNT_APIC_TIMER]);
#else
	*loc = this_cpu_read(irq_stat.apic_timer_irqs);
#endif
#else
	*loc = 0;
#endif
}

/* ================================================================
 * Governor callbacks
 * ================================================================ */

static int nap_fallback_heuristic(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev,
				  u64 sleep_length_ns, s64 latency_req)
{
	int i;

	for (i = drv->state_count - 1; i > 0; i--) {
		if (dev->states_usage[i].disable)
			continue;
		if ((s64)drv->states[i].exit_latency_ns > latency_req)
			continue;
		if (drv->states[i].target_residency_ns > sleep_length_ns)
			continue;
		return i;
	}
	return 0;
}

/*
 * Return the shallowest enabled C-state that satisfies the current latency
 * request, or 0 if none exists (POLL is the only option).  Does not consult
 * the NN.
 */
static int nap_find_min_valid_state(struct cpuidle_driver *drv,
				    struct cpuidle_device *dev,
				    s64 latency_req)
{
	int i;

	for (i = 1; i < drv->state_count; i++) {
		if (dev->states_usage[i].disable)
			continue;
		if ((s64)drv->states[i].exit_latency_ns > latency_req)
			continue;
		return i;
	}
	return 0;
}

/*
 * Cached wrapper.  Invalidated when latency_req changes (immediate PM QoS
 * propagation) or every NAP_MIN_STATE_REFRESH_JIFFIES (bounded staleness for
 * rare sysfs / runtime state-disable events).
 */
static inline int nap_get_min_valid_state(struct nap_cpu *d,
					  struct cpuidle_driver *drv,
					  struct cpuidle_device *dev,
					  s64 latency_req)
{
	if (unlikely(latency_req != d->cached_min_state_latency ||
		     time_after(jiffies, d->cached_min_state_jiffies +
					 NAP_MIN_STATE_REFRESH_JIFFIES))) {
		d->cached_min_state = nap_find_min_valid_state(drv, dev,
							       latency_req);
		d->cached_min_state_latency = latency_req;
		d->cached_min_state_jiffies = jiffies;
	}
	return d->cached_min_state;
}

/*
 * dev->poll_limit_ns for a POLL selection: predicted wake time plus a 1 us
 * margin (absorbs timer jitter so a slightly late wake does not retrigger
 * select/enter/reflect), floored at NAP_POLL_LIMIT_MIN_NS and capped at the
 * minimum state's target residency (beyond which the C-state would have been
 * the better choice).
 */
static inline u64 nap_compute_poll_limit(u64 sleep_length_ns,
					 u64 min_state_target_ns)
{
	return clamp_t(u64, sleep_length_ns + NAP_POLL_LIMIT_MARGIN_NS,
		       NAP_POLL_LIMIT_MIN_NS, min_state_target_ns);
}

static int nap_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		      bool *stop_tick)
{
	struct nap_cpu *d = nap_cpus[raw_smp_processor_id()];
	struct nap_raw raw;
	ktime_t dt = 0;
	s64 lat;
	int idx, i, min_state;

	if (unlikely(drv->state_count <= 1))
		return 0;

	d->ts_sel = local_clock();
	d->sleep_ns = ktime_to_ns(nap_sleep_length(&dt));
	d->tick_ns = ktime_to_ns(dt);
	lat = nap_latency_req(dev->cpu);
	min_state = nap_get_min_valid_state(d, drv, dev, lat);
	nap_irq_counts(&d->dev0, &d->loc0);
	d->active = true;
	d->short_circuited = false;
	atomic_inc(&nap_nr_idle);
	d->n_select++;

	/*
	 * Fast path: when no C-state can amortize its target residency
	 * within the predicted sleep length, the answer is deterministically
	 * POLL.  Skip NN inference; nap_reflect still records the idle
	 * (history) but the NN does not learn from it.
	 */
	if (min_state == 0 ||
	    d->sleep_ns < drv->states[min_state].target_residency_ns) {
		if (min_state > 0)
			dev->poll_limit_ns = nap_compute_poll_limit(
				d->sleep_ns,
				drv->states[min_state].target_residency_ns);
		else
			dev->poll_limit_ns = max_t(u64, d->sleep_ns,
						   NAP_POLL_LIMIT_MIN_NS);
		*stop_tick = false;
		d->short_circuited = true;
		d->sel_k = 0;
		d->sel_stop = false;
		return 0;
	}

	raw.sleep_ns = d->sleep_ns;
	raw.tick_ns = d->tick_ns;
	raw.busy_ns = d->prev_ref_ts ? d->ts_sel - d->prev_ref_ts : 1000000;
	for (i = 0; i < NAP_HIST; i++)
		raw.hist_net[i] = d->hist_net[i];
	raw.idle_now = atomic_read(&nap_nr_idle);
	raw.nr_cpus = num_online_cpus();
	raw.prev_d_dev = d->prev_d_dev;
	raw.prev_d_loc = d->prev_d_loc;
	raw.latency_req = lat;
	raw.have_sample = d->have_sample;
	raw.last_T_net = d->last_T_net;
	raw.last_O = d->last_O;
	raw.last_cycle = d->last_cycle;
	raw.last_tick = d->last_tick;
	raw.last_sleep = d->last_sleep;
	raw.last_stop = d->last_stop;
	raw.last_short = d->last_short;
	d->have_sample = false;

	if (likely(drv->state_count <= NAP_MAXSTATE && may_use_simd())) {
		bool st = false;

		kernel_fpu_begin();
		idx = nap_fpu_select(drv, dev, d, &raw, &st);
		kernel_fpu_end();
		*stop_tick = st;
	} else {
		idx = nap_fallback_heuristic(drv, dev, d->sleep_ns, lat);
		*stop_tick = drv->states[idx].target_residency_ns >
			     RESIDENCY_THRESHOLD_NS;
	}
	if (idx == 0)
		dev->poll_limit_ns = nap_compute_poll_limit(
			d->sleep_ns,
			drv->states[min_state].target_residency_ns);
	d->sel_k = idx;
	d->sel_stop = *stop_tick;
	if (*stop_tick)
		d->n_stop++;
	return idx;
}

static void nap_reflect(struct cpuidle_device *dev, int index)
{
	struct nap_cpu *d = nap_cpus[raw_smp_processor_id()];
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	u32 dd, dl;
	u64 now, meas, net, tgt, cyc;
	bool expired;
	int i;

	if (!d->active || !drv)
		return;
	d->active = false;
	/*
	 * POLL was left because its time limit expired, not because of a
	 * wakeup: the idle period is not over.
	 */
	expired = dev->poll_time_limit && index >= 0 &&
		  index < drv->state_count &&
		  (drv->states[index].flags & CPUIDLE_FLAG_POLLING);
	dev->poll_time_limit = false;
	if (atomic_dec_return(&nap_nr_idle) < 0)
		atomic_set(&nap_nr_idle, 0);

	now = local_clock();
	nap_irq_counts(&dd, &dl);
	d->prev_d_dev = dd - d->dev0;
	d->prev_d_loc = dl - d->loc0;
	/* busy period + idle: the wall-clock time this idle accounts for */
	cyc = d->prev_ref_ts ? now - d->prev_ref_ts : 0;
	d->prev_ref_ts = now;

	if (index < 0 || index >= drv->state_count)
		return;
	meas = dev->last_residency_ns;
	/*
	 * Like teo and menu, assume that the CPU could have been idle for
	 * the whole predicted sleep length.  Taking the truncated time as
	 * the measurement teaches the model that idles are short and it
	 * keeps selecting POLL.
	 */
	if (expired && d->sleep_ns > meas)
		meas = d->sleep_ns;
	net = meas > drv->states[index].exit_latency_ns ?
	      meas - drv->states[index].exit_latency_ns : 0;
	tgt = drv->states[index].target_residency_ns;

	for (i = NAP_HIST - 1; i > 0; i--)
		d->hist_net[i] = d->hist_net[i - 1];
	d->hist_net[0] = net;

	d->total_residency_ns += meas;
	if (!d->short_circuited && index > 0 && meas < tgt)
		d->n_overshoot++;
	/*
	 * Avoidable wakeup latency: how much slower the wakeup was than it
	 * would have been from the state that fits this idle.
	 */
	{
		u64 lat_e = drv->states[index].exit_latency_ns, lat_i;
		int ks = 0;

		for (i = drv->state_count - 1; i > 0; i--) {
			if (dev->states_usage[i].disable)
				continue;
			if (drv->states[i].target_residency_ns <= net) {
				ks = i;
				break;
			}
		}
		lat_i = drv->states[ks].exit_latency_ns;
		d->lat_last = lat_e > lat_i ? lat_e - lat_i : 0;
		d->lat_ns += d->lat_last;
	}
	if (index < NAP_MAXSTATE)
		d->n_state[index]++;

	/*
	 * Feedback for the next select: the learning label and the input of
	 * the latency-budget controller.
	 */
	d->have_sample = true;
	d->last_T_net = net;
	d->last_O = d->lat_last;
	d->last_cycle = cyc;
	d->last_tick = d->tick_ns;
	d->last_sleep = d->sleep_ns;
	d->last_stop = d->sel_stop;
	d->last_short = d->short_circuited;
}

static int nap_enable(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct nap_cpu *d = nap_cpus[dev->cpu];

	/*
	 * Weight initialisation is deferred to the first nap_select() FPU
	 * section (inited == false): nap_enable() is called from the cpuidle
	 * core and may run on a different CPU than dev->cpu during governor
	 * switch.
	 */
	/*
	 * Keep the weights and the controller state across a re-enable (CPU
	 * hotplug, suspend, governor switch): they are what has been learned.
	 * Everything after them is runtime state.
	 */
	{
		bool inited = d->inited;

		memset((char *)d + offsetof(struct nap_cpu, fwd_valid), 0,
		       sizeof(*d) - offsetof(struct nap_cpu, fwd_valid));
		d->inited = inited;
	}
	d->cached_min_state_latency = S64_MIN;
	d->cached_min_state_jiffies = jiffies - NAP_MIN_STATE_REFRESH_JIFFIES;
	return 0;
}

/* ================================================================
 * sysfs interface  (/sys/devices/system/cpu/nap/)
 * ================================================================ */

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CPUIDLE_NAP_VERSION);
}

static ssize_t simd_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", nap_ops->name);
}

static ssize_t max_simd_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", nap_isa_name[nap_isa_req]);
}

static ssize_t max_simd_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	int i;

	for (i = 0; i < NAP_ISA_MAX; i++) {
		if (sysfs_streq(buf, nap_isa_name[i])) {
			nap_isa_req = i;
			nap_apply_isa();
			return count;
		}
	}
	return -EINVAL;
}

static ssize_t stats_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	u64 sel = 0, res = 0, over = 0, learn = 0, stop = 0;
	u64 lat_ns = 0;
	u64 st[NAP_MAXSTATE] = { 0 };
	int cpu, i, len = 0;

	for_each_possible_cpu(cpu) {
		struct nap_cpu *d = nap_cpus[cpu];

		sel += d->n_select;
		res += d->total_residency_ns;
		over += d->n_overshoot;
		lat_ns += d->lat_ns;
		learn += d->n_learn;
		stop += d->n_stop;
		for (i = 0; i < NAP_MAXSTATE; i++)
			st[i] += d->n_state[i];
	}
	len += sysfs_emit_at(buf, len, "total_selects: %llu\n", sel);
	len += sysfs_emit_at(buf, len, "total_residency_ms: %llu\n",
			     div_u64(res, NSEC_PER_MSEC));
	len += sysfs_emit_at(buf, len, "overshoot_count: %llu\n", over);
	len += sysfs_emit_at(buf, len, "overshoot_rate_permil: %llu\n",
			     sel ? div64_u64(over * 1000, sel) : 0);
	len += sysfs_emit_at(buf, len, "avoidable_wake_latency_ns: %llu\n",
			     lat_ns);
	len += sysfs_emit_at(buf, len,
			     "avoidable_wake_latency_ns_per_select: %llu\n",
			     sel ? div64_u64(lat_ns, sel) : 0);
	len += sysfs_emit_at(buf, len, "learn_count: %llu\n", learn);
	len += sysfs_emit_at(buf, len, "stop_tick_count: %llu\n", stop);
	len += sysfs_emit_at(buf, len, "entered:");
	for (i = 0; i < NAP_MAXSTATE; i++)
		len += sysfs_emit_at(buf, len, " %llu", st[i]);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

/* learning_rate in millths (20 = 0.02), as in earlier versions */
static ssize_t learning_rate_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(nap_lr_micro) / 1000);
}

static ssize_t learning_rate_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) || val == 0 || val > 100)
		return -EINVAL;
	WRITE_ONCE(nap_lr_micro, val * 1000);
	return count;
}

static ssize_t learn_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(nap_learn));
}

static ssize_t learn_store(struct kobject *kobj,
			   struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	bool v;

	if (kstrtobool(buf, &v))
		return -EINVAL;
	WRITE_ONCE(nap_learn, v);
	return count;
}

static ssize_t uint_show(const unsigned int *p, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(*p));
}

static ssize_t uint_store(unsigned int *p, const char *buf, size_t count,
			  unsigned int lo, unsigned int hi)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) || val < lo || val > hi)
		return -EINVAL;
	WRITE_ONCE(*p, val);
	return count;
}

/*
 * budget_ppm: share of the wall-clock time (parts per million) a CPU may lose
 * to avoidable wakeup latency; the single responsiveness dial.
 */
static ssize_t budget_ppm_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return uint_show(&nap_rho_ppm, buf);
}

static ssize_t budget_ppm_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	return uint_store(&nap_rho_ppm, buf, count, 1, 1000000);
}

static ssize_t tick_stop_cost_ns_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return uint_show(&nap_c_stop_ns, buf);
}

static ssize_t tick_stop_cost_ns_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	return uint_store(&nap_c_stop_ns, buf, count, 0, 1000000);
}

static ssize_t tick_wake_cost_ns_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return uint_show(&nap_c_wake_ns, buf);
}

static ssize_t tick_wake_cost_ns_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	return uint_store(&nap_c_wake_ns, buf, count, 0, 1000000);
}

/*
 * power_milli: comma-separated relative power of each idle state, shallowest
 * first.
 */
static ssize_t power_milli_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	int i, len = 0;

	for (i = 0; i < NAP_MAXSTATE; i++)
		len += sysfs_emit_at(buf, len, "%s%u", i ? "," : "",
				     READ_ONCE(nap_pow_milli[i]));
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t power_milli_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int v[NAP_MAXSTATE];
	char *copy, *p, *tok;
	int n = 0, i;

	copy = kstrndup(buf, count, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	p = strim(copy);
	while ((tok = strsep(&p, ",")) && n < NAP_MAXSTATE) {
		if (kstrtouint(strim(tok), 10, &v[n]) || v[n] == 0 ||
		    v[n] > 10000) {
			kfree(copy);
			return -EINVAL;
		}
		n++;
	}
	kfree(copy);
	if (n == 0)
		return -EINVAL;
	/* states beyond the list keep their value */
	for (i = 0; i < n; i++)
		WRITE_ONCE(nap_pow_milli[i], v[i]);
	return count;
}

static ssize_t reset_weights_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	cpumask_var_t mask;
	int cpu;

	/*
	 * Set a per-CPU flag; each CPU reinitialises its own weights inside
	 * nap_select() within its own kernel_fpu_begin/end context, which
	 * avoids cross-CPU data races on the weight arrays.
	 *
	 * Accepts "all" to reset every online CPU, or a cpulist
	 * (e.g. "0-3,5,7") to reset specific CPUs.
	 */
	if (sysfs_streq(buf, "all")) {
		for_each_online_cpu(cpu)
			nap_cpus[cpu]->reset_pending = true;
		pr_info("nap: weight reset scheduled for all CPUs\n");
		return count;
	}

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;
	if (cpulist_parse(buf, mask)) {
		free_cpumask_var(mask);
		return -EINVAL;
	}
	for_each_cpu_and(cpu, mask, cpu_online_mask)
		nap_cpus[cpu]->reset_pending = true;
	pr_info("nap: weight reset scheduled for CPUs %*pbl\n",
		cpumask_pr_args(mask));
	free_cpumask_var(mask);
	return count;
}

static ssize_t reset_stats_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int cpu, i;

	for_each_possible_cpu(cpu) {
		struct nap_cpu *d = nap_cpus[cpu];

		d->n_select = d->n_overshoot = d->n_learn = d->n_stop =
			d->total_residency_ns = 0;
		d->lat_ns = 0;
		for (i = 0; i < NAP_MAXSTATE; i++)
			d->n_state[i] = 0;
	}
	return count;
}

static struct kobj_attribute version_attr = __ATTR_RO(version);
static struct kobj_attribute simd_attr = __ATTR_RO(simd);
static struct kobj_attribute max_simd_attr = __ATTR_RW(max_simd);
static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct kobj_attribute learning_rate_attr = __ATTR_RW(learning_rate);
static struct kobj_attribute learn_attr = __ATTR_RW(learn);
static struct kobj_attribute budget_ppm_attr = __ATTR_RW(budget_ppm);
static struct kobj_attribute tick_stop_cost_attr =
	__ATTR(tick_stop_cost_ns, 0644, tick_stop_cost_ns_show,
	       tick_stop_cost_ns_store);
static struct kobj_attribute tick_wake_cost_attr =
	__ATTR(tick_wake_cost_ns, 0644, tick_wake_cost_ns_show,
	       tick_wake_cost_ns_store);
static struct kobj_attribute power_milli_attr = __ATTR_RW(power_milli);
static struct kobj_attribute reset_weights_attr = __ATTR_WO(reset_weights);
static struct kobj_attribute reset_stats_attr = __ATTR_WO(reset_stats);


/* ================================================================
 * Weight dump and sideload (sysfs binary attribute "weights")
 *
 * Layout: header, then the float32 weights as little-endian words:
 * first layer rows [NROW][NHID], bias [NHID], output layer [NOUT][NHID],
 * bias [NOUT].  The same layout as the arrays of nap_weights.h.
 * ================================================================ */

#define NAP_BLOB_MAGIC	"NAPWGT01"
#define NAP_BLOB_FMT	1
#define NAP_PAYLOAD_WORDS \
	(NAP_NROW * NAP_NHID + NAP_NHID + NAP_NOUT * NAP_NHID + NAP_NOUT)

struct nap_blob_hdr {
	char magic[8];
	__le32 fmt;
	__le32 nt, nk, nd, nhid, nout, first, tail_rep;
	__le32 payload_bytes;
	__le32 crc;		/* of the payload */
	__le32 input_sig;	/* of the input standardisation constants */
	__le32 reserved;
};
#define NAP_BLOB_SIZE \
	(sizeof(struct nap_blob_hdr) + NAP_PAYLOAD_WORDS * sizeof(__le32))

static DEFINE_MUTEX(nap_wt_lock);
static int nap_wt_cpu;			/* selected CPU, -1 = all */
static u8 *nap_wt_snap;			/* last dump */
static u8 *nap_wt_stage;		/* write in progress */
static size_t nap_wt_stage_len;

struct nap_wt_xfer {
	struct nap_cpu *d;
	__le32 *w;
	bool ok;
};

/*
 * The two functions below run on the CPU that owns the weights, with
 * interrupts off; nap_select() also runs with interrupts off, so they
 * cannot interleave with a learning step.  No floating point: words are
 * copied as bits.
 */
static void nap_wt_dump_fn(void *info)
{
	struct nap_wt_xfer *x = info;
	struct nap_cpu *d = x->d;
	const u32 *src;
	__le32 *w = x->w;
	int i, o;

	x->ok = d->inited;
	if (!x->ok)
		return;
	src = (const u32 *)d->w1c;
	for (i = 0; i < NAP_NROW * NAP_NHID; i++)
		*w++ = cpu_to_le32(src[i]);
	src = (const u32 *)d->b1;
	for (i = 0; i < NAP_NHID; i++)
		*w++ = cpu_to_le32(src[i]);
	for (o = 0; o < NAP_NOUT; o++)
		for (i = 0; i < NAP_NHID; i++)
			*w++ = cpu_to_le32(((const u32 *)d->w2r[i])[o]);
	src = (const u32 *)d->b2;
	for (o = 0; o < NAP_NOUT; o++)
		*w++ = cpu_to_le32(src[o]);
}

static void nap_wt_load_fn(void *info)
{
	struct nap_wt_xfer *x = info;
	struct nap_cpu *d = x->d;
	const __le32 *w = x->w;
	u32 *dst;
	int i, o;

	dst = (u32 *)d->w1c;
	for (i = 0; i < NAP_NROW * NAP_NHID; i++)
		dst[i] = le32_to_cpu(*w++);
	dst = (u32 *)d->b1;
	for (i = 0; i < NAP_NHID; i++)
		dst[i] = le32_to_cpu(*w++);
	for (o = 0; o < NAP_NOUT; o++)
		for (i = 0; i < NAP_NHID; i++)
			((u32 *)d->w2r[i])[o] = le32_to_cpu(*w++);
	dst = (u32 *)d->b2;
	for (o = 0; o < NAP_NOUT; o++)
		dst[o] = le32_to_cpu(*w++);
	if (!d->inited) {
		/* what nap_fpu_init() would set: lambda = 1.0f, the rest 0 */
		*(u32 *)&d->lambda = 0x3f800000;
		d->inited = true;
	}
	/* the stored activations belong to the old weights */
	d->fwd_valid = false;
	d->reset_pending = false;
	x->ok = true;
}

static bool nap_wt_run(int cpu, smp_call_func_t fn, struct nap_wt_xfer *x)
{
	x->d = nap_cpus[cpu];
	x->ok = false;
	if (cpu_online(cpu))
		smp_call_function_single(cpu, fn, x, 1);
	else
		fn(x);		/* nobody else touches an offline CPU's data */
	return x->ok;
}

static void nap_blob_fill_hdr(struct nap_blob_hdr *h, u32 crc)
{
	memcpy(h->magic, NAP_BLOB_MAGIC, sizeof(h->magic));
	h->fmt = cpu_to_le32(NAP_BLOB_FMT);
	h->nt = cpu_to_le32(NAP_NT);
	h->nk = cpu_to_le32(NAP_NK);
	h->nd = cpu_to_le32(NAP_ND);
	h->nhid = cpu_to_le32(NAP_NHID);
	h->nout = cpu_to_le32(NAP_NOUT);
	h->first = cpu_to_le32(NAP_GRID_LOG2_FIRST);
	h->tail_rep = cpu_to_le32(NAP_TAIL_REP_NS);
	h->payload_bytes = cpu_to_le32(NAP_PAYLOAD_WORDS * sizeof(__le32));
	h->crc = cpu_to_le32(crc);
	h->input_sig = cpu_to_le32(nap_fpu_input_sig());
	h->reserved = 0;
}

/* 6.16 made the bin_attribute pointers const throughout sysfs */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define NAP_BIN_ATTR_CONST const
#else
#define NAP_BIN_ATTR_CONST
#endif

static ssize_t weights_read(struct file *file, struct kobject *kobj,
			    NAP_BIN_ATTR_CONST struct bin_attribute *attr,
			    char *buf, loff_t off, size_t count)
{
	ssize_t ret = 0;

	mutex_lock(&nap_wt_lock);
	if (off == 0) {
		struct nap_blob_hdr *h = (struct nap_blob_hdr *)nap_wt_snap;
		struct nap_wt_xfer x;

		if (nap_wt_cpu < 0 || !nap_cpus || !nap_cpus[nap_wt_cpu]) {
			ret = -EINVAL;
			goto out;
		}
		if (!nap_wt_snap) {
			nap_wt_snap = kvmalloc(NAP_BLOB_SIZE, GFP_KERNEL);
			if (!nap_wt_snap) {
				ret = -ENOMEM;
				goto out;
			}
			h = (struct nap_blob_hdr *)nap_wt_snap;
		}
		x.w = (__le32 *)(nap_wt_snap + sizeof(*h));
		if (!nap_wt_run(nap_wt_cpu, nap_wt_dump_fn, &x)) {
			/* never ran: nothing learned yet, nothing to dump */
			ret = -ENODATA;
			goto out;
		}
		nap_blob_fill_hdr(h, crc32_le(~0, (u8 *)x.w,
					      NAP_PAYLOAD_WORDS * 4));
	}
	if (!nap_wt_snap || off >= NAP_BLOB_SIZE)
		goto out;
	if (count > NAP_BLOB_SIZE - off)
		count = NAP_BLOB_SIZE - off;
	memcpy(buf, nap_wt_snap + off, count);
	ret = count;
out:
	mutex_unlock(&nap_wt_lock);
	return ret;
}

/* Reject anything that could upset the network: the header must match this
 * build exactly, the checksum must hold and every weight must be finite and
 * below 1024 in magnitude. */
static int nap_blob_check(const u8 *blob)
{
	const struct nap_blob_hdr *h = (const struct nap_blob_hdr *)blob;
	const __le32 *w = (const __le32 *)(blob + sizeof(*h));
	int i;

	if (memcmp(h->magic, NAP_BLOB_MAGIC, sizeof(h->magic)) ||
	    le32_to_cpu(h->fmt) != NAP_BLOB_FMT)
		return -EPROTO;
	if (le32_to_cpu(h->nt) != NAP_NT || le32_to_cpu(h->nk) != NAP_NK ||
	    le32_to_cpu(h->nd) != NAP_ND || le32_to_cpu(h->nhid) != NAP_NHID ||
	    le32_to_cpu(h->nout) != NAP_NOUT ||
	    le32_to_cpu(h->first) != NAP_GRID_LOG2_FIRST ||
	    le32_to_cpu(h->payload_bytes) != NAP_PAYLOAD_WORDS * 4)
		return -EPROTO;
	if (le32_to_cpu(h->input_sig) != nap_fpu_input_sig())
		return -EPROTO;
	if (le32_to_cpu(h->crc) !=
	    crc32_le(~0, (const u8 *)w, NAP_PAYLOAD_WORDS * 4))
		return -EBADMSG;
	for (i = 0; i < NAP_PAYLOAD_WORDS; i++)
		if ((le32_to_cpu(w[i]) & 0x7fffffff) >= 0x44800000)
			return -ERANGE;	/* NaN, Inf or |w| >= 1024 */
	return 0;
}

static ssize_t weights_write(struct file *file, struct kobject *kobj,
			     NAP_BIN_ATTR_CONST struct bin_attribute *attr,
			     char *buf, loff_t off, size_t count)
{
	struct nap_wt_xfer x;
	ssize_t ret = count;
	int cpu, err, n = 0;

	mutex_lock(&nap_wt_lock);
	if (!nap_wt_stage) {
		nap_wt_stage = kvmalloc(NAP_BLOB_SIZE, GFP_KERNEL);
		if (!nap_wt_stage) {
			ret = -ENOMEM;
			goto out;
		}
	}
	if (off == 0)
		nap_wt_stage_len = 0;
	if (off != nap_wt_stage_len || off + count > NAP_BLOB_SIZE) {
		nap_wt_stage_len = 0;
		ret = -EINVAL;
		goto out;
	}
	memcpy(nap_wt_stage + off, buf, count);
	nap_wt_stage_len += count;
	if (nap_wt_stage_len < NAP_BLOB_SIZE)
		goto out;		/* wait for the rest */
	nap_wt_stage_len = 0;
	err = nap_blob_check(nap_wt_stage);
	if (err) {
		pr_warn("nap: weights rejected (%d)\n", err);
		ret = err;
		goto out;
	}
	x.w = (__le32 *)(nap_wt_stage + sizeof(struct nap_blob_hdr));
	if (nap_wt_cpu >= 0) {
		nap_wt_run(nap_wt_cpu, nap_wt_load_fn, &x);
		n = 1;
	} else {
		for_each_possible_cpu(cpu) {
			if (nap_cpus[cpu]) {
				nap_wt_run(cpu, nap_wt_load_fn, &x);
				n++;
			}
		}
	}
	pr_info("nap: weights loaded into %d CPU(s)\n", n);
out:
	mutex_unlock(&nap_wt_lock);
	return ret;
}

static ssize_t weights_cpu_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	if (READ_ONCE(nap_wt_cpu) < 0)
		return sysfs_emit(buf, "all\n");
	return sysfs_emit(buf, "%d\n", READ_ONCE(nap_wt_cpu));
}

static ssize_t weights_cpu_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int cpu;

	mutex_lock(&nap_wt_lock);
	if (sysfs_streq(buf, "all")) {
		nap_wt_cpu = -1;
	} else if (!kstrtouint(buf, 10, &cpu) && cpu < nr_cpu_ids &&
		   cpu_possible(cpu)) {
		nap_wt_cpu = cpu;
	} else {
		mutex_unlock(&nap_wt_lock);
		return -EINVAL;
	}
	mutex_unlock(&nap_wt_lock);
	return count;
}

static struct kobj_attribute weights_cpu_attr = __ATTR_RW(weights_cpu);
static struct bin_attribute bin_attr_weights =
	__BIN_ATTR(weights, 0600, weights_read, weights_write,
		   NAP_BLOB_SIZE);
static NAP_BIN_ATTR_CONST struct bin_attribute *
NAP_BIN_ATTR_CONST nap_bin_attrs[] = {
	&bin_attr_weights,
	NULL,
};

static struct attribute *nap_attrs[] = {
	&version_attr.attr,
	&simd_attr.attr,
	&max_simd_attr.attr,
	&stats_attr.attr,
	&learning_rate_attr.attr,
	&learn_attr.attr,
	&budget_ppm_attr.attr,
	&tick_stop_cost_attr.attr,
	&tick_wake_cost_attr.attr,
	&power_milli_attr.attr,
	&reset_weights_attr.attr,
	&reset_stats_attr.attr,
	&weights_cpu_attr.attr,
	NULL,
};

static const struct attribute_group nap_attr_group = {
	.attrs = nap_attrs,
	.bin_attrs = nap_bin_attrs,
};

static struct kobject *nap_kobj;

static int nap_sysfs_init(void)
{
	struct device *dev_root;
	int ret;

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (!dev_root)
		return -ENODEV;

	nap_kobj = kobject_create_and_add(NAP_SYSFS_NAME, &dev_root->kobj);
	put_device(dev_root);
	if (!nap_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(nap_kobj, &nap_attr_group);
	if (ret) {
		kobject_put(nap_kobj);
		nap_kobj = NULL;
	}
	return ret;
}

static void nap_sysfs_exit(void)
{
	if (nap_kobj) {
		sysfs_remove_group(nap_kobj, &nap_attr_group);
		kobject_put(nap_kobj);
		nap_kobj = NULL;
	}
}

/* ================================================================
 * Governor registration
 * ================================================================ */

static struct cpuidle_governor nap_governor = {
	.name    = NAP_GOV_NAME,
	.rating  = NAP_RATING,
	.enable  = nap_enable,
	.select  = nap_select,
	.reflect = nap_reflect,
};

/*
 * Pick the highest SIMD level the processor offers, then verify every level
 * up to it: forward pass against the offline model, one SGD step against the
 * scalar reference.  A level that fails (and everything above it) is not
 * used; if even the scalar reference disagrees with the offline model the
 * governor does not register.
 */
static int __init nap_detect_simd(void)
{
	int isa, cap = NAP_ISA_SSE2;

	if (boot_cpu_has(X86_FEATURE_AVX2) && boot_cpu_has(X86_FEATURE_FMA))
		cap = NAP_ISA_AVX2;
	if (cap == NAP_ISA_AVX2 && boot_cpu_has(X86_FEATURE_AVX512F))
		cap = NAP_ISA_AVX512;

	nap_isa_cpu = -1;
	for (isa = NAP_ISA_SCALAR; isa <= cap; isa++) {
		unsigned int wf, wl;
		u64 f10, l10;
		int n;

		kernel_fpu_begin();
		n = nap_fpu_selftest(isa, &wf, &wl);
		kernel_fpu_end();
		if (wf > 2000 || wl > 200) {
			pr_warn("nap: %s self-test FAILED (forward %u e-6, learn %u e-6): not used\n",
				nap_isa_name[isa], wf, wl);
			break;
		}
		kernel_fpu_begin();
		nap_fpu_bench(isa, &f10, &l10);
		kernel_fpu_end();
		pr_debug("nap: %-6s self-test ok (%d vectors, fwd %u e-6, learn %u e-6); forward %llu.%llu ns, learn %llu.%llu ns\n",
			 nap_isa_name[isa], n, wf, wl,
			 f10 / 10, f10 % 10, l10 / 10, l10 % 10);
		nap_isa_cpu = isa;
	}
	return nap_isa_cpu >= NAP_ISA_SCALAR ? 0 : -EINVAL;
}

/* ~80 KB each: more than alloc_percpu() hands out, so allocate by hand */
static void nap_free_cpus(void)
{
	int cpu;

	if (!nap_cpus)
		return;
	for_each_possible_cpu(cpu)
		kvfree(nap_cpus[cpu]);
	kfree(nap_cpus);
	nap_cpus = NULL;
}

static int __init nap_alloc_cpus(void)
{
	int cpu;

	nap_cpus = kcalloc(nr_cpu_ids, sizeof(*nap_cpus), GFP_KERNEL);
	if (!nap_cpus)
		return -ENOMEM;
	for_each_possible_cpu(cpu) {
		nap_cpus[cpu] = kvzalloc_node(sizeof(**nap_cpus), GFP_KERNEL,
					      cpu_to_node(cpu));
		if (!nap_cpus[cpu]) {
			nap_free_cpus();
			return -ENOMEM;
		}
	}
	return 0;
}

static int __init nap_init(void)
{
	int ret;

	if (!may_use_simd())
		return -EINVAL;
	ret = nap_alloc_cpus();
	if (ret)
		return ret;

	ret = nap_detect_simd();
	if (ret) {
		pr_err("nap: the network does not reproduce the offline model; not registering\n");
		nap_free_cpus();
		return ret;
	}
	nap_apply_isa();

	ret = nap_sysfs_init();
	if (ret)
		pr_warn("nap: sysfs init failed: %d (continuing without sysfs)\n",
			ret);

	ret = nap_register_governor(&nap_governor);
	if (ret) {
		pr_err("nap: register_governor failed: %d\n", ret);
		nap_sysfs_exit();
		nap_free_cpus();
		return ret;
	}

	pr_info("%s v%s by %s registered (rating=%u, simd=%s, processor supports up to %s)\n",
		CPUIDLE_NAP_PROGNAME, CPUIDLE_NAP_VERSION, CPUIDLE_NAP_AUTHOR,
		nap_governor.rating, nap_ops->name, nap_isa_name[nap_isa_cpu]);
	return 0;
}
postcore_initcall(nap_init);

#ifdef NAP_OOT
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nap cpuidle governor (out-of-tree test build; cannot be unloaded)");
#endif
