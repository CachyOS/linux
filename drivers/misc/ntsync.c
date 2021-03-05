// SPDX-License-Identifier: GPL-2.0-only
/*
 * ntsync.c - Kernel driver for NT synchronization primitives
 *
 * Copyright (C) 2021-2022 Elizabeth Figura
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <uapi/linux/ntsync.h>

#define NTSYNC_NAME	"ntsync"

enum ntsync_type {
	NTSYNC_TYPE_SEM,
};

struct ntsync_obj {
	struct rcu_head rhead;
	struct kref refcount;

	enum ntsync_type type;

	union {
		struct {
			__u32 count;
			__u32 max;
		} sem;
	} u;
};

struct ntsync_device {
	struct xarray objects;
};

static void destroy_obj(struct kref *ref)
{
	struct ntsync_obj *obj = container_of(ref, struct ntsync_obj, refcount);

	kfree_rcu(obj, rhead);
}

static void put_obj(struct ntsync_obj *obj)
{
	kref_put(&obj->refcount, destroy_obj);
}

static int ntsync_char_open(struct inode *inode, struct file *file)
{
	struct ntsync_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	xa_init_flags(&dev->objects, XA_FLAGS_ALLOC);

	file->private_data = dev;
	return nonseekable_open(inode, file);
}

static int ntsync_char_release(struct inode *inode, struct file *file)
{
	struct ntsync_device *dev = file->private_data;
	struct ntsync_obj *obj;
	unsigned long id;

	xa_for_each(&dev->objects, id, obj)
		put_obj(obj);

	xa_destroy(&dev->objects);

	kfree(dev);

	return 0;
}

static void init_obj(struct ntsync_obj *obj)
{
	kref_init(&obj->refcount);
}

static int ntsync_create_sem(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_sem_args __user *user_args = argp;
	struct ntsync_sem_args args;
	struct ntsync_obj *sem;
	__u32 id;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.count > args.max)
		return -EINVAL;

	sem = kzalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return -ENOMEM;

	init_obj(sem);
	sem->type = NTSYNC_TYPE_SEM;
	sem->u.sem.count = args.count;
	sem->u.sem.max = args.max;

	ret = xa_alloc(&dev->objects, &id, sem, xa_limit_32b, GFP_KERNEL);
	if (ret < 0) {
		kfree(sem);
		return ret;
	}

	return put_user(id, &user_args->sem);
}

static int ntsync_delete(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_obj *obj;
	__u32 id;

	if (get_user(id, (__u32 __user *)argp))
		return -EFAULT;

	obj = xa_erase(&dev->objects, id);
	if (!obj)
		return -EINVAL;

	put_obj(obj);
	return 0;
}

static long ntsync_char_ioctl(struct file *file, unsigned int cmd,
			      unsigned long parm)
{
	struct ntsync_device *dev = file->private_data;
	void __user *argp = (void __user *)parm;

	switch (cmd) {
	case NTSYNC_IOC_CREATE_SEM:
		return ntsync_create_sem(dev, argp);
	case NTSYNC_IOC_DELETE:
		return ntsync_delete(dev, argp);
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations ntsync_fops = {
	.owner		= THIS_MODULE,
	.open		= ntsync_char_open,
	.release	= ntsync_char_release,
	.unlocked_ioctl	= ntsync_char_ioctl,
	.compat_ioctl	= ntsync_char_ioctl,
	.llseek		= no_llseek,
};

static struct miscdevice ntsync_misc = {
	.minor		= NTSYNC_MINOR,
	.name		= NTSYNC_NAME,
	.fops		= &ntsync_fops,
};

module_misc_device(ntsync_misc);

MODULE_AUTHOR("Elizabeth Figura");
MODULE_DESCRIPTION("Kernel driver for NT synchronization primitives");
MODULE_LICENSE("GPL");
MODULE_ALIAS("devname:" NTSYNC_NAME);
MODULE_ALIAS_MISCDEV(NTSYNC_MINOR);
