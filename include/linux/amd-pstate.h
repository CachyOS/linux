/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Public interface for the AMD P-state driver.
 */
#ifndef __LINUX_AMD_PSTATE_H__
#define __LINUX_AMD_PSTATE_H__

#if IS_ENABLED(CONFIG_X86_AMD_PSTATE)
bool amd_pstate_cas_active(void);
#else
static inline bool amd_pstate_cas_active(void) { return false; }
#endif

#endif /* __LINUX_AMD_PSTATE_H__ */
