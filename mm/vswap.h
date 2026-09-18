/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual swap space
 *
 * Copyright (C) 2026 Nhat Pham
 */
#ifndef _MM_VSWAP_H
#define _MM_VSWAP_H

#include <linux/jump_label.h>
#include <linux/swap.h>
#include "swap.h"

#ifdef CONFIG_SWAP

DECLARE_STATIC_KEY_FALSE(vswap_key);

/*
 * Only true once vswap_init() has published vswap_si, so callers never
 * see the device half built.
 */
static inline bool vswap_is_enabled(void)
{
	return static_branch_unlikely(&vswap_key);
}

static inline bool is_vswap_entry(swp_entry_t entry)
{
	return swap_is_vswap(__swap_entry_to_info(entry));
}

#endif /* CONFIG_SWAP */

#endif /* _MM_VSWAP_H */
