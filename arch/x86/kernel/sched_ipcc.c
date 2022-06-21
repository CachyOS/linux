// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel support for scheduler IPC classes
 *
 * Copyright (c) 2023, Intel Corporation.
 *
 * Author: Ricardo Neri <ricardo.neri-calderon@linux.intel.com>
 *
 * On hybrid processors, the architecture differences between types of CPUs
 * lead to different number of retired instructions per cycle (IPC). IPCs may
 * differ further by classes of instructions.
 *
 * The scheduler assigns an IPC class to every task with arch_update_ipcc()
 * from data that hardware provides. Implement this interface for x86.
 *
 * See kernel/sched/sched.h for details.
 */

#include <linux/sched.h>

#include <asm/intel-family.h>
#include <asm/topology.h>

#define CLASS_DEBOUNCER_SKIPS 4

/**
 * debounce_and_update_class() - Process and update a task's classification
 *
 * @p:		The task of which the classification will be updated
 * @new_ipcc:	The new IPC classification
 *
 * Update the classification of @p with the new value that hardware provides.
 * Only update the classification of @p if it has been the same during
 * CLASS_DEBOUNCER_SKIPS consecutive ticks.
 */
static void debounce_and_update_class(struct task_struct *p, u8 new_ipcc)
{
	u16 debounce_skip;

	/* The class of @p changed. Only restart the debounce counter. */
	if (p->ipcc_tmp != new_ipcc) {
		p->ipcc_cntr = 1;
		goto out;
	}

	/*
	 * The class of @p did not change. Update it if it has been the same
	 * for CLASS_DEBOUNCER_SKIPS user ticks.
	 */
	debounce_skip = p->ipcc_cntr + 1;
	if (debounce_skip < CLASS_DEBOUNCER_SKIPS)
		p->ipcc_cntr++;
	else
		p->ipcc = new_ipcc;

out:
	p->ipcc_tmp = new_ipcc;
}

static bool classification_is_accurate(u8 hfi_class, bool smt_siblings_idle)
{
	switch (boot_cpu_data.x86_model) {
	case INTEL_FAM6_ALDERLAKE:
	case INTEL_FAM6_ALDERLAKE_L:
	case INTEL_FAM6_RAPTORLAKE:
	case INTEL_FAM6_RAPTORLAKE_P:
	case INTEL_FAM6_RAPTORLAKE_S:
		if (hfi_class == 3 || hfi_class == 2 || smt_siblings_idle)
			return true;

		return false;

	default:
		return false;
	}
}

void intel_update_ipcc(struct task_struct *curr)
{
	u8 hfi_class;
	bool idle;

	if (intel_hfi_read_classid(&hfi_class))
		return;

	/*
	 * 0 is a valid classification for Intel Thread Director. A scheduler
	 * IPCC class of 0 means that the task is unclassified. Adjust.
	 */
	idle = sched_smt_siblings_idle(task_cpu(curr));
	if (classification_is_accurate(hfi_class, idle))
		debounce_and_update_class(curr, hfi_class + 1);
}
