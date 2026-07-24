// SPDX-License-Identifier: GPL-2.0-only
/*
 * apple.c - Apple ACPI quirks
 * Copyright (C) 2017 Lukas Wunner <lukas@wunner.de>
 */

#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/cpu.h>
#include <linux/cpuhplock.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/platform_data/x86/apple.h>
#include <linux/suspend.h>
#include <linux/uuid.h>
#include "../internal.h"

/* Apple _DSM device properties GUID */
static const guid_t apple_prp_guid =
	GUID_INIT(0xa0b5b7c6, 0x1318, 0x441c,
		  0xb0, 0xc9, 0xfe, 0x69, 0x5e, 0xaf, 0x94, 0x9b);

/**
 * acpi_extract_apple_properties - retrieve and convert Apple _DSM properties
 * @adev: ACPI device for which to retrieve the properties
 *
 * Invoke Apple's custom _DSM once to check the protocol version and once more
 * to retrieve the properties.  They are marshalled up in a single package as
 * alternating key/value elements, unlike _DSD which stores them as a package
 * of 2-element packages.  Convert to _DSD format and make them available under
 * the primary fwnode.
 */
void acpi_extract_apple_properties(struct acpi_device *adev)
{
	unsigned int i, j = 0, newsize = 0, numprops, numvalid;
	union acpi_object *props, *newprops;
	unsigned long *valid = NULL;
	void *free_space;

	if (!x86_apple_machine)
		return;

	props = acpi_evaluate_dsm_typed(adev->handle, &apple_prp_guid, 1, 0,
					NULL, ACPI_TYPE_BUFFER);
	if (!props)
		return;

	if (!props->buffer.length)
		goto out_free;

	if (props->buffer.pointer[0] != 3) {
		acpi_handle_info(adev->handle, FW_INFO
				 "unsupported properties version %*ph\n",
				 props->buffer.length, props->buffer.pointer);
		goto out_free;
	}

	ACPI_FREE(props);
	props = acpi_evaluate_dsm_typed(adev->handle, &apple_prp_guid, 1, 1,
					NULL, ACPI_TYPE_PACKAGE);
	if (!props)
		return;

	numprops = props->package.count / 2;
	if (!numprops)
		goto out_free;

	valid = bitmap_zalloc(numprops, GFP_KERNEL);
	if (!valid)
		goto out_free;

	/* newsize = key length + value length of each tuple */
	for (i = 0; i < numprops; i++) {
		union acpi_object *key = &props->package.elements[i * 2];
		union acpi_object *val = &props->package.elements[i * 2 + 1];

		if ( key->type != ACPI_TYPE_STRING ||
		    (val->type != ACPI_TYPE_INTEGER &&
		     val->type != ACPI_TYPE_BUFFER &&
		     val->type != ACPI_TYPE_STRING))
			continue; /* skip invalid properties */

		__set_bit(i, valid);
		newsize += key->string.length + 1;
		if ( val->type == ACPI_TYPE_BUFFER)
			newsize += val->buffer.length;
		else if (val->type == ACPI_TYPE_STRING)
			newsize += val->string.length + 1;
	}

	numvalid = bitmap_weight(valid, numprops);
	if (numprops > numvalid)
		acpi_handle_info(adev->handle, FW_INFO
				 "skipped %u properties: wrong type\n",
				 numprops - numvalid);
	if (numvalid == 0)
		goto out_free;

	/* newsize += top-level package + 3 objects for each key/value tuple */
	newsize	+= (1 + 3 * numvalid) * sizeof(union acpi_object);
	newprops = ACPI_ALLOCATE_ZEROED(newsize);
	if (!newprops)
		goto out_free;

	/* layout: top-level package | packages | key/value tuples | strings */
	newprops->type = ACPI_TYPE_PACKAGE;
	newprops->package.count = numvalid;
	newprops->package.elements = &newprops[1];
	free_space = &newprops[1 + 3 * numvalid];

	for_each_set_bit(i, valid, numprops) {
		union acpi_object *key = &props->package.elements[i * 2];
		union acpi_object *val = &props->package.elements[i * 2 + 1];
		unsigned int k = 1 + numvalid + j * 2; /* index into newprops */
		unsigned int v = k + 1;

		newprops[1 + j].type = ACPI_TYPE_PACKAGE;
		newprops[1 + j].package.count = 2;
		newprops[1 + j].package.elements = &newprops[k];

		newprops[k].type = ACPI_TYPE_STRING;
		newprops[k].string.length = key->string.length;
		newprops[k].string.pointer = free_space;
		memcpy(free_space, key->string.pointer, key->string.length);
		free_space += key->string.length + 1;

		newprops[v].type = val->type;
		if (val->type == ACPI_TYPE_INTEGER) {
			newprops[v].integer.value = val->integer.value;
		} else if (val->type == ACPI_TYPE_STRING) {
			newprops[v].string.length = val->string.length;
			newprops[v].string.pointer = free_space;
			memcpy(free_space, val->string.pointer,
			       val->string.length);
			free_space += val->string.length + 1;
		} else {
			newprops[v].buffer.length = val->buffer.length;
			newprops[v].buffer.pointer = free_space;
			memcpy(free_space, val->buffer.pointer,
			       val->buffer.length);
			free_space += val->buffer.length;
		}
		j++; /* count valid properties */
	}
	WARN_ON(free_space != (void *)newprops + newsize);

	adev->data.pointer = newprops;
	acpi_data_add_props(&adev->data, &apple_prp_guid, newprops);

out_free:
	ACPI_FREE(props);
	bitmap_free(valid);
}

#ifdef CONFIG_PM_SLEEP_SMP
#define PCI_DEVICE_ID_APPLE_T2_BRIDGE 0x1801

/*
 * The ACPI path selected by _OSI("Darwin") leaves Apple T2 systems in a
 * state where bringing secondary CPUs online during early resume may take
 * several seconds per CPU. Normal CPU hotplug after platform resume is not
 * affected, so move it outside the generic suspend CPU hotplug window.
 */
static cpumask_var_t apple_t2_offlined_cpus;

static void apple_t2_restore_cpus(void)
{
	unsigned int cpu;
	int ret;

	for_each_cpu(cpu, apple_t2_offlined_cpus) {
		ret = add_cpu(cpu);
		if (ret) {
			pr_err("ACPI: Apple T2 failed to restore CPU%u: %d\n",
			       cpu, ret);
			continue;
		}

		cpumask_clear_cpu(cpu, apple_t2_offlined_cpus);
	}
}

static void apple_t2_offline_cpus(void)
{
	unsigned int cpu;
	int ret;

	if (!cpumask_empty(apple_t2_offlined_cpus)) {
		pr_err("ACPI: Apple T2 CPUs from the previous suspend remain offline\n");
		apple_t2_restore_cpus();
		if (!cpumask_empty(apple_t2_offlined_cpus)) {
			pr_err("ACPI: Apple T2 early CPU offlining skipped\n");
			return;
		}
	}

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;

		ret = remove_cpu(cpu);
		if (ret) {
			pr_err("ACPI: Apple T2 failed to offline CPU%u: %d\n",
			       cpu, ret);
			continue;
		}

		cpumask_set_cpu(cpu, apple_t2_offlined_cpus);
	}
}

static int apple_t2_cpu_prepare(struct notifier_block *nb,
				unsigned long action, void *unused)
{
	if (action == PM_SUSPEND_PREPARE)
		apple_t2_offline_cpus();

	return NOTIFY_OK;
}

static int apple_t2_cpu_restore(struct notifier_block *nb,
				unsigned long action, void *unused)
{
	if (action == PM_POST_SUSPEND)
		apple_t2_restore_cpus();

	return NOTIFY_OK;
}

/*
 * The CPU core PM notifier runs at priority 0. Offline CPUs before it blocks
 * hotplug, then restore them after it enables hotplug again.
 */
static struct notifier_block apple_t2_cpu_prepare_nb = {
	.notifier_call = apple_t2_cpu_prepare,
	.priority = 1,
};

static struct notifier_block apple_t2_cpu_restore_nb = {
	.notifier_call = apple_t2_cpu_restore,
	.priority = -1,
};

static int __init apple_t2_cpu_pm_init(void)
{
	struct pci_dev *t2;
	int ret;

	if (!x86_apple_machine)
		return 0;

	t2 = pci_get_device(PCI_VENDOR_ID_APPLE,
			    PCI_DEVICE_ID_APPLE_T2_BRIDGE, NULL);
	if (!t2)
		return 0;
	pci_dev_put(t2);

	if (!alloc_cpumask_var(&apple_t2_offlined_cpus, GFP_KERNEL))
		return -ENOMEM;

	ret = register_pm_notifier(&apple_t2_cpu_prepare_nb);
	if (ret)
		goto free_mask;

	ret = register_pm_notifier(&apple_t2_cpu_restore_nb);
	if (ret)
		goto unregister_prepare;

	return 0;

unregister_prepare:
	unregister_pm_notifier(&apple_t2_cpu_prepare_nb);
free_mask:
	free_cpumask_var(apple_t2_offlined_cpus);
	return ret;
}
late_initcall(apple_t2_cpu_pm_init);
#endif
