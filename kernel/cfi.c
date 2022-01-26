// SPDX-License-Identifier: GPL-2.0
/*
 * Clang Control Flow Integrity (CFI) error handling.
 *
 * Copyright (C) 2022 Google LLC
 */

#include <linux/cfi.h>

#ifdef CONFIG_MODULES
/* Populates `kcfi_trap(_end)?` fields in `struct module`. */
void module_cfi_finalize(const Elf_Ehdr *hdr, const Elf_Shdr *sechdrs,
			 struct module *mod)
{
	char *secstrings;
	unsigned int i;

	mod->kcfi_traps = NULL;
	mod->kcfi_traps_end = NULL;

	secstrings = (char *)hdr + sechdrs[hdr->e_shstrndx].sh_offset;

	for (i = 1; i < hdr->e_shnum; i++) {
		if (strcmp(secstrings + sechdrs[i].sh_name, "__kcfi_traps"))
			continue;

		mod->kcfi_traps = (unsigned long *)sechdrs[i].sh_addr;
		mod->kcfi_traps_end = (unsigned long *)(sechdrs[i].sh_addr +
							sechdrs[i].sh_size);
		break;
	}
}

static bool is_module_cfi_trap(unsigned long addr)
{
	bool found = false;
	struct module *mod;
	unsigned long *p;

	rcu_read_lock_sched_notrace();

	mod = __module_address(addr);
	if (mod)
		for (p = mod->kcfi_traps; !found && p < mod->kcfi_traps_end;
		     ++p)
			found = (*p == addr);

	rcu_read_unlock_sched_notrace();

	return found;
}

#else /* CONFIG_MODULES */

static inline bool is_module_cfi_trap(unsigned long addr)
{
	return false;
}

#endif /* CONFIG_MODULES */

extern unsigned long __start___kcfi_traps[];
extern unsigned long __stop___kcfi_traps[];

bool is_cfi_trap(unsigned long addr)
{
	unsigned long *p;

	for (p = __start___kcfi_traps; p < __stop___kcfi_traps; ++p)
		if (*p == addr)
			return true;

	return is_module_cfi_trap(addr);
}

enum bug_trap_type report_cfi_failure(struct pt_regs *regs, unsigned long addr,
				      unsigned long target, unsigned long type)
{
	pr_err("CFI failure at %pS (target: %pS; expected type: 0x%08x)\n",
	       (void *)addr, (void *)target, (u32)type);

	if (IS_ENABLED(CONFIG_CFI_PERMISSIVE)) {
		__warn(NULL, 0, (void *)addr, 0, regs, NULL);
		return BUG_TRAP_TYPE_WARN;
	}

	return BUG_TRAP_TYPE_BUG;
}
