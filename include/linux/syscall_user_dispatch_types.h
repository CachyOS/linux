/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SYSCALL_USER_DISPATCH_TYPES_H
#define _SYSCALL_USER_DISPATCH_TYPES_H

#include <linux/types.h>

#ifdef CONFIG_SYSCALL_USER_DISPATCH

struct syscall_user_dispatch {
	char __user	*selector;
	unsigned long	offset;
	unsigned long	len;
	unsigned long	handler;	/* user address jumped to on block when on_upcall */
	bool		on_dispatch;
	bool		on_upcall;	/* dispatch bypasses SIGSYS, jumps to handler */
};

#else

struct syscall_user_dispatch {};

#endif

#endif /* _SYSCALL_USER_DISPATCH_TYPES_H */
