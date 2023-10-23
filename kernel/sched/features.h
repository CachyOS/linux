/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Using the avg_vruntime, do the right thing and preserve lag across
 * sleep+wake cycles. EEVDF placement strategy #1, #2 if disabled.
 */
SCHED_FEAT(PLACE_LAG, true)
/*
 * Give new tasks half a slice to ease into the competition.
 */
SCHED_FEAT(PLACE_DEADLINE_INITIAL, true)
/*
 * Inhibit (wakeup) preemption until the current task has either matched the
 * 0-lag point or until is has exhausted it's slice.
 */
#ifdef CONFIG_SCHED_BORE
SCHED_FEAT(RUN_TO_PARITY, false)
#else // CONFIG_SCHED_BORE
SCHED_FEAT(RUN_TO_PARITY, true)
#endif // CONFIG_SCHED_BORE
/*
 * Allow tasks with a shorter slice to disregard RUN_TO_PARITY
 */
#ifdef CONFIG_SCHED_BORE
SCHED_FEAT(PREEMPT_SHORT, false)
#else // CONFIG_SCHED_BORE
SCHED_FEAT(PREEMPT_SHORT, true)
#endif // CONFIG_SCHED_BORE
/*
 * Let sleepers earn back lag, but not more than 0-lag. GENTLE_SLEEPERS earn at
 * half the speed.
 */
#ifdef CONFIG_SCHED_BORE
SCHED_FEAT(PLACE_SLEEPER, true)
SCHED_FEAT(GENTLE_SLEEPER, false)
#else // CONFIG_SCHED_BORE
SCHED_FEAT(PLACE_SLEEPER, false)
SCHED_FEAT(GENTLE_SLEEPER, true)
#endif // CONFIG_SCHED_BORE
/*
 * Disable the eligibility check -- always true.
 *
 * Selecting this allows short tasks, in the presence of a long task, to walk
 * far past 0-lag and create a window where newly placed tasks will come in and
 * starve the long task.
 *
 * Behaves quite terrible for mixed slice workloads as a result, very much not
 * recommended.
 */
SCHED_FEAT(EVDF, false)
/*
 * Delay dequeueing tasks until they get selected or woken.
 *
 * By delaying the dequeue for non-eligible tasks, they remain in the
 * competition and can burn off their negative lag. When they get selected
 * they'll have positive lag by definition.
 *
 * GENTLE_DELAY clips the lag on dequeue (or wakeup) to 0.
 */
#ifdef CONFIG_SCHED_BORE
SCHED_FEAT(DELAY_DEQUEUE, false)
SCHED_FEAT(GENTLE_DELAY, false)
#else // CONFIG_SCHED_BORE
SCHED_FEAT(DELAY_DEQUEUE, true)
SCHED_FEAT(GENTLE_DELAY, true)
#endif // CONFIG_SCHED_BORE

/*
 * Prefer to schedule the task we woke last (assuming it failed
 * wakeup-preemption), since its likely going to consume data we
 * touched, increases cache locality.
 */
SCHED_FEAT(NEXT_BUDDY, false)

/*
 * Consider buddies to be cache hot, decreases the likeliness of a
 * cache buddy being migrated away, increases cache locality.
 */
SCHED_FEAT(CACHE_HOT_BUDDY, true)

/*
 * Allow wakeup-time preemption of the current task:
 */
SCHED_FEAT(WAKEUP_PREEMPTION, true)

SCHED_FEAT(HRTICK, false)
SCHED_FEAT(HRTICK_DL, false)
SCHED_FEAT(DOUBLE_TICK, false)

/*
 * Decrement CPU capacity based on time not spent running tasks
 */
SCHED_FEAT(NONTASK_CAPACITY, true)

#ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(TTWU_QUEUE, false)
#else

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
SCHED_FEAT(TTWU_QUEUE, true)
#endif

/*
 * When doing wakeups, attempt to limit superfluous scans of the LLC domain.
 */
SCHED_FEAT(SIS_PROP, false)
SCHED_FEAT(SIS_UTIL, true)

/*
 * Issue a WARN when we do multiple update_rq_clock() calls
 * in a single rq->lock section. Default disabled because the
 * annotations are not complete.
 */
SCHED_FEAT(WARN_DOUBLE_CLOCK, false)

#ifdef HAVE_RT_PUSH_IPI
/*
 * In order to avoid a thundering herd attack of CPUs that are
 * lowering their priorities at the same time, and there being
 * a single CPU that has an RT task that can migrate and is waiting
 * to run, where the other CPUs will try to take that CPUs
 * rq lock and possibly create a large contention, sending an
 * IPI to that CPU and let that CPU push the RT task to where
 * it should go may be a better scenario.
 */
SCHED_FEAT(RT_PUSH_IPI, true)
#endif

SCHED_FEAT(RT_RUNTIME_SHARE, false)
SCHED_FEAT(LB_MIN, false)
SCHED_FEAT(ATTACH_AGE_LOAD, true)

SCHED_FEAT(WA_IDLE, true)
SCHED_FEAT(WA_WEIGHT, true)
SCHED_FEAT(WA_BIAS, true)

/*
 * UtilEstimation. Use estimated CPU utilization.
 */
SCHED_FEAT(UTIL_EST, true)
SCHED_FEAT(UTIL_EST_FASTUP, true)

SCHED_FEAT(LATENCY_WARN, false)
