/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Kernel support for NT synchronization primitive emulation
 *
 * Copyright (C) 2021-2022 Elizabeth Figura
 */

#ifndef __LINUX_NTSYNC_H
#define __LINUX_NTSYNC_H

#include <linux/types.h>

struct ntsync_sem_args {
	__u32 sem;
	__u32 count;
	__u32 max;
};

struct ntsync_wait_args {
	__u64 timeout;
	__u64 objs;
	__u32 count;
	__u32 owner;
	__u32 index;
	__u32 pad;
};

#define NTSYNC_MAX_WAIT_COUNT 64

#define NTSYNC_IOC_BASE 0xf7

#define NTSYNC_IOC_CREATE_SEM		_IOWR(NTSYNC_IOC_BASE, 0, \
					      struct ntsync_sem_args)
#define NTSYNC_IOC_DELETE		_IOW (NTSYNC_IOC_BASE, 1, __u32)
#define NTSYNC_IOC_PUT_SEM		_IOWR(NTSYNC_IOC_BASE, 2, \
					      struct ntsync_sem_args)
#define NTSYNC_IOC_WAIT_ANY		_IOWR(NTSYNC_IOC_BASE, 3, \
					      struct ntsync_wait_args)

#endif
