/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Collabora Ltd.
 */
#ifndef _SYSCALL_USER_DISPATCH_H
#define _SYSCALL_USER_DISPATCH_H

#include <linux/thread_info.h>
#include <linux/sched.h>
#include <linux/syscall_user_dispatch_types.h>

struct pt_regs;

#ifdef CONFIG_SYSCALL_USER_DISPATCH

bool syscall_user_dispatch(struct pt_regs *regs);

static __always_inline bool syscall_user_dispatch_clear_on_dispatch(void)
{
	if (likely(!current->syscall_dispatch.on_dispatch))
		return false;

	current->syscall_dispatch.on_dispatch = false;
	return true;
}

int set_syscall_user_dispatch(unsigned long mode, unsigned long offset,
			      unsigned long len, char __user *selector);

int set_syscall_user_dispatch_handler(unsigned long handler);

/*
 * Reset per-task Syscall User Dispatch state on clone (copy_process) and
 * exec.  The upcall handler is per-task state and is never inherited: a
 * stale handler address must not carry into a different binary (think
 * setuid exec), and each new thread registers its own handler before
 * arming upcall mode.
 */
static inline void clear_syscall_work_syscall_user_dispatch(struct task_struct *tsk)
{
	clear_task_syscall_work(tsk, SYSCALL_USER_DISPATCH);
	tsk->syscall_dispatch.handler = 0;
	tsk->syscall_dispatch.on_dispatch = false;
	tsk->syscall_dispatch.on_upcall = false;
}

int syscall_user_dispatch_get_config(struct task_struct *task, unsigned long size,
				     void __user *data);

int syscall_user_dispatch_set_config(struct task_struct *task, unsigned long size,
				     void __user *data);

#else

static __always_inline bool syscall_user_dispatch(struct pt_regs *regs)
{
	return false;
}

static __always_inline bool syscall_user_dispatch_clear_on_dispatch(void)
{
	return false;
}

static inline int set_syscall_user_dispatch(unsigned long mode, unsigned long offset,
					    unsigned long len, char __user *selector)
{
	return -EINVAL;
}

static inline int set_syscall_user_dispatch_handler(unsigned long handler)
{
	return -EINVAL;
}

static inline void clear_syscall_work_syscall_user_dispatch(struct task_struct *tsk)
{
}

static inline int syscall_user_dispatch_get_config(struct task_struct *task,
						   unsigned long size, void __user *data)
{
	return -EINVAL;
}

static inline int syscall_user_dispatch_set_config(struct task_struct *task,
						   unsigned long size, void __user *data)
{
	return -EINVAL;
}

#endif /* CONFIG_SYSCALL_USER_DISPATCH */

#endif /* _SYSCALL_USER_DISPATCH_H */
