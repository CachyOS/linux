/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Clang Control Flow Integrity (CFI) type definitions.
 */
#ifndef _LINUX_CFI_TYPES_H
#define _LINUX_CFI_TYPES_H

#ifdef CONFIG_CFI_CLANG
#include <linux/linkage.h>

#ifdef __ASSEMBLY__
/*
 * Use the __kcfi_typeid_<function> type identifier symbol to
 * annotate indirectly called assembly functions. The compiler emits
 * these symbols for all address-taken function declarations in C
 * code.
 */
#ifndef __CFI_TYPE
#define __CFI_TYPE(name)				\
	.4byte __kcfi_typeid_##name
#endif

#define SYM_TYPED_ENTRY(name, fname, linkage, align...)	\
	linkage(name) ASM_NL				\
	align ASM_NL					\
	__CFI_TYPE(fname) ASM_NL			\
	name:

#define __SYM_TYPED_FUNC_START_ALIAS(name, fname) \
	SYM_TYPED_ENTRY(name, fname, SYM_L_GLOBAL, SYM_A_ALIGN)

#define __SYM_TYPED_FUNC_START(name, fname) \
	SYM_TYPED_ENTRY(name, fname, SYM_L_GLOBAL, SYM_A_ALIGN)

#endif /* __ASSEMBLY__ */

#else /* CONFIG_CFI_CLANG */

#ifdef __ASSEMBLY__
#define __SYM_TYPED_FUNC_START_ALIAS(name, fname) \
	SYM_FUNC_START_ALIAS(name)

#define __SYM_TYPED_FUNC_START(name, fname) \
	SYM_FUNC_START(name)
#endif /* __ASSEMBLY__ */

#endif /* CONFIG_CFI_CLANG */

#ifdef __ASSEMBLY__
#define SYM_TYPED_FUNC_START_ALIAS(name) \
	__SYM_TYPED_FUNC_START_ALIAS(name, name)

#define SYM_TYPED_FUNC_START(name) \
	__SYM_TYPED_FUNC_START(name, name)
#endif /* __ASSEMBLY__ */

#endif /* _LINUX_CFI_TYPES_H */
