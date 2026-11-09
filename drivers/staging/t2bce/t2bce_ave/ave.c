#define pr_fmt(fmt) "t2bce_ave: " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "t2bce_core_transport.h"
#include "video.h"

#define T2BCE_AVE_NAME "t2bce_ave"

struct t2bce_ave_module {
	struct device *dev;
	struct device *core_dev;
	struct t2bce_core_client *client;
	struct t2bce_ave_device *video;
};

static dev_t t2bce_ave_devt;
static struct class *t2bce_ave_class;
static struct t2bce_ave_module *t2bce_ave;

static void t2bce_ave_shutdown(void *userdata)
{
	struct t2bce_ave_module *ave = userdata;

	t2bce_ave_video_suspend(ave->video);
}

static int t2bce_ave_pm_prepare(void *userdata)
{
	struct t2bce_ave_module *ave = userdata;

	t2bce_ave_video_suspend(ave->video);
	return 0;
}

static const struct t2bce_core_client_pm_ops t2bce_ave_pm_ops = {
	.shutdown = t2bce_ave_shutdown,
	.pm_prepare = t2bce_ave_pm_prepare,
};

static int __init t2bce_ave_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&t2bce_ave_devt, 0, 1, T2BCE_AVE_NAME);
	if (ret)
		return ret;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	t2bce_ave_class = class_create(THIS_MODULE, T2BCE_AVE_NAME);
#else
	t2bce_ave_class = class_create(T2BCE_AVE_NAME);
#endif
	if (IS_ERR(t2bce_ave_class)) {
		ret = PTR_ERR(t2bce_ave_class);
		goto err_chrdev;
	}
	t2bce_ave = kzalloc(sizeof(*t2bce_ave), GFP_KERNEL);
	if (!t2bce_ave) {
		ret = -ENOMEM;
		goto err_class;
	}
	t2bce_ave->core_dev = t2bce_core_device_get();
	if (IS_ERR(t2bce_ave->core_dev)) {
		ret = PTR_ERR(t2bce_ave->core_dev);
		t2bce_ave->core_dev = NULL;
		goto err_state;
	}
	t2bce_ave->dev = device_create(t2bce_ave_class, t2bce_ave->core_dev,
				       t2bce_ave_devt, NULL, T2BCE_AVE_NAME);
	if (IS_ERR(t2bce_ave->dev)) {
		ret = PTR_ERR(t2bce_ave->dev);
		t2bce_ave->dev = NULL;
		goto err_core;
	}
	t2bce_ave->client = t2bce_core_client_get(t2bce_ave->dev);
	if (IS_ERR(t2bce_ave->client)) {
		ret = PTR_ERR(t2bce_ave->client);
		t2bce_ave->client = NULL;
		goto err_device;
	}
	t2bce_core_client_set_pm_ops(t2bce_ave->client, &t2bce_ave_pm_ops,
				     t2bce_ave);
	ret = t2bce_ave_video_create(t2bce_ave->client, &t2bce_ave->video);
	if (ret)
		goto err_client;
	pr_info("module initialized\n");
	return 0;

err_client:
	t2bce_core_client_set_pm_ops(t2bce_ave->client, NULL, NULL);
	t2bce_core_client_put(t2bce_ave->client);
err_device:
	device_destroy(t2bce_ave_class, t2bce_ave_devt);
err_core:
	t2bce_core_device_put(t2bce_ave->core_dev);
err_state:
	kfree(t2bce_ave);
	t2bce_ave = NULL;
err_class:
	class_destroy(t2bce_ave_class);
err_chrdev:
	unregister_chrdev_region(t2bce_ave_devt, 1);
	return ret;
}

static void __exit t2bce_ave_exit(void)
{
	t2bce_ave_video_destroy(t2bce_ave->video);
	t2bce_core_client_set_pm_ops(t2bce_ave->client, NULL, NULL);
	t2bce_core_client_put(t2bce_ave->client);
	device_destroy(t2bce_ave_class, t2bce_ave_devt);
	t2bce_core_device_put(t2bce_ave->core_dev);
	kfree(t2bce_ave);
	class_destroy(t2bce_ave_class);
	unregister_chrdev_region(t2bce_ave_devt, 1);
	pr_info("module exited\n");
}

module_init(t2bce_ave_init);
module_exit(t2bce_ave_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("André Eikmeyer <andre.eikmeyer@kait2en.org>");
MODULE_DESCRIPTION("Apple T2 AVE");
MODULE_VERSION("0.01");
