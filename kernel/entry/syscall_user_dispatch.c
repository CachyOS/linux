// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Collabora Ltd.
 */
#include <linux/elf.h>
#include <linux/entry-common.h>
#include <linux/prctl.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/signal.h>
#include <linux/syscall_user_dispatch.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>

#include <asm/syscall.h>

static bool syscall_user_dispatch_allowed __read_mostly = true;

static void trigger_sigsys(struct pt_regs *regs)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGSYS;
	info.si_code = SYS_USER_DISPATCH;
	info.si_call_addr = (void __user *)KSTK_EIP(current);
	info.si_errno = 0;
	info.si_arch = syscall_get_arch(current);
	info.si_syscall = syscall_get_nr(current, regs);

	force_sig_info(&info);
}

#ifdef CONFIG_X86_64
/*
 * The upcall contract mirrors the 64-bit SYSCALL instruction, so it is
 * only used for native 64-bit syscalls that are not being single-stepped.
 * ia32 syscalls (int $0x80 / sysenter / 32-bit SYSCALL, including int
 * $0x80 issued from 64-bit code) and TF-traced ones keep using SIGSYS:
 * the handler could neither tell the ABI apart nor recover the argument
 * register that RCX overwrites, a 64-bit handler cannot run on a 32-bit
 * CS, and with TF set the first handler instruction would trap before it
 * could save the single-step state the way a SIGSYS handler does.
 */
static bool can_upcall(struct syscall_user_dispatch *sd, struct pt_regs *regs)
{
	return sd->on_upcall && user_64bit_mode(regs) && !in_ia32_syscall() &&
	       !(regs->flags & X86_EFLAGS_TF);
}

static void trigger_upcall(struct pt_regs *regs)
{
	/*
	 * Hand off to the user-mode handler installed via
	 * PR_SET_SYSCALL_USER_DISPATCH_HANDLER, emulating what SYSCALL
	 * would do if LSTAR pointed at user code: RCX = address of the
	 * instruction after the syscall, R11 = RFLAGS.  RAX holds the
	 * syscall number (restored by syscall_rollback()).  No signal
	 * frame, no FPU save, signal mask untouched.
	 *
	 * regs->cx != regs->ip makes do_syscall_64() take the IRET exit
	 * path, so every other register is restored unchanged.
	 */
	regs->cx = regs->ip;
	regs->r11 = regs->flags;
	regs->ip = current->syscall_dispatch.handler;
}
#else
static bool can_upcall(struct syscall_user_dispatch *sd, struct pt_regs *regs)
{
	return false;
}

static void trigger_upcall(struct pt_regs *regs)
{
}
#endif

bool syscall_user_dispatch(struct pt_regs *regs)
{
	struct syscall_user_dispatch *sd = &current->syscall_dispatch;
	char state;

	if (likely(instruction_pointer(regs) - sd->offset < sd->len))
		return false;

	if (unlikely(arch_syscall_is_vdso_sigreturn(regs)))
		return false;

	if (likely(sd->selector)) {
		/*
		 * access_ok() is performed once, at prctl time, when
		 * the selector is loaded by userspace.
		 */
		if (unlikely(__get_user(state, sd->selector))) {
			force_exit_sig(SIGSEGV);
			return true;
		}

		if (likely(state == SYSCALL_DISPATCH_FILTER_ALLOW))
			return false;

		if (state != SYSCALL_DISPATCH_FILTER_BLOCK) {
			force_exit_sig(SIGSYS);
			return true;
		}
	}

	sd->on_dispatch = true;
	syscall_rollback(current, regs);
	if (can_upcall(sd, regs))
		trigger_upcall(regs);
	else
		trigger_sigsys(regs);

	return true;
}

static int task_set_syscall_user_dispatch(struct task_struct *task, unsigned long mode,
					  unsigned long offset, unsigned long len,
					  char __user *selector)
{
	bool upcall = false;

	switch (mode) {
	case PR_SYS_DISPATCH_OFF:
		if (offset || len || selector)
			return -EINVAL;
		break;
	case PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON:
		upcall = true;
		fallthrough;
	case PR_SYS_DISPATCH_EXCLUSIVE_ON:
		/*
		 * Validate the direct dispatcher region just for basic
		 * sanity against overflow and a 0-sized dispatcher
		 * region.  If the user is able to submit a syscall from
		 * an address, that address is obviously valid.
		 */
		if (offset && offset + len <= offset)
			return -EINVAL;
		break;
	case PR_SYS_DISPATCH_UPCALL_INCLUSIVE_ON:
		upcall = true;
		fallthrough;
	case PR_SYS_DISPATCH_INCLUSIVE_ON:
		if (len == 0 || offset + len <= offset)
			return -EINVAL;
		/*
		 * Invert the range, the check in syscall_user_dispatch()
		 * supports wrap-around.
		 */
		offset = offset + len;
		len = -len;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Upcall mode is x86_64-only and needs a user-space handler
	 * installed first (TASK_SIZE_MAX is the exclusive upper bound).
	 */
	if (upcall) {
		unsigned long handler = task->syscall_dispatch.handler;

		if (!IS_ENABLED(CONFIG_X86_64) || !handler || handler >= TASK_SIZE_MAX)
			return -EINVAL;
	}

	/* Arming can be denied at runtime via sysctl, disarming is allowed */
	if (mode != PR_SYS_DISPATCH_OFF && !syscall_user_dispatch_allowed)
		return -EPERM;

	/*
	 * access_ok() will clear memory tags for tagged addresses
	 * if current has memory tagging enabled.
	 *
	 * To enable a tracer to set a tracees selector the
	 * selector address must be untagged for access_ok(),
	 * otherwise an untagged tracer will always fail to set a
	 * tagged tracees selector.
	 */
	if (mode != PR_SYS_DISPATCH_OFF && selector &&
		!access_ok(untagged_addr(selector), sizeof(*selector)))
		return -EFAULT;

	task->syscall_dispatch.selector = selector;
	task->syscall_dispatch.offset = offset;
	task->syscall_dispatch.len = len;
	task->syscall_dispatch.on_dispatch = false;
	task->syscall_dispatch.on_upcall = upcall;

	if (mode != PR_SYS_DISPATCH_OFF)
		set_task_syscall_work(task, SYSCALL_USER_DISPATCH);
	else
		clear_task_syscall_work(task, SYSCALL_USER_DISPATCH);

	return 0;
}

int set_syscall_user_dispatch(unsigned long mode, unsigned long offset,
			      unsigned long len, char __user *selector)
{
	return task_set_syscall_user_dispatch(current, mode, offset, len, selector);
}

int set_syscall_user_dispatch_handler(unsigned long handler)
{
	struct syscall_user_dispatch *sd = &current->syscall_dispatch;

	/* Must be a user address (0 is the "clear" sentinel). */
	if (handler && handler >= TASK_SIZE_MAX)
		return -EINVAL;

	/* Can't yank the handler out from under an active upcall arming. */
	if (!handler && sd->on_upcall)
		return -EBUSY;

	sd->handler = handler;
	return 0;
}

int syscall_user_dispatch_get_config(struct task_struct *task, unsigned long size,
				     void __user *data)
{
	struct syscall_user_dispatch *sd = &task->syscall_dispatch;
	struct ptrace_sud_config cfg;

	if (size != sizeof(cfg))
		return -EINVAL;

	/*
	 * offset/len are stored in exclusive form (the inclusive modes are
	 * inverted at arm time), so report the exclusive-form mode; a
	 * get/set round trip then reproduces the exact same configuration.
	 */
	if (!test_task_syscall_work(task, SYSCALL_USER_DISPATCH))
		cfg.mode = PR_SYS_DISPATCH_OFF;
	else if (sd->on_upcall)
		cfg.mode = PR_SYS_DISPATCH_UPCALL_EXCLUSIVE_ON;
	else
		cfg.mode = PR_SYS_DISPATCH_ON;

	cfg.offset = sd->offset;
	cfg.len = sd->len;
	cfg.selector = (__u64)(uintptr_t)sd->selector;

	if (copy_to_user(data, &cfg, sizeof(cfg)))
		return -EFAULT;

	return 0;
}

int syscall_user_dispatch_set_config(struct task_struct *task, unsigned long size,
				     void __user *data)
{
	struct ptrace_sud_config cfg;

	if (size != sizeof(cfg))
		return -EINVAL;

	if (copy_from_user(&cfg, data, sizeof(cfg)))
		return -EFAULT;

	return task_set_syscall_user_dispatch(task, cfg.mode, cfg.offset, cfg.len,
					      (char __user *)(uintptr_t)cfg.selector);
}

#ifdef CONFIG_PROC_SYSCTL
static const struct ctl_table syscall_user_dispatch_sysctls[] = {
	{
		.procname	= "syscall_user_dispatch",
		.data		= &syscall_user_dispatch_allowed,
		.maxlen		= sizeof(syscall_user_dispatch_allowed),
		.mode		= 0644,
		.proc_handler	= proc_dobool,
	},
};

static int __init syscall_user_dispatch_sysctl_init(void)
{
	register_sysctl_init("kernel", syscall_user_dispatch_sysctls);
	return 0;
}
late_initcall(syscall_user_dispatch_sysctl_init);
#endif /* CONFIG_PROC_SYSCTL */
