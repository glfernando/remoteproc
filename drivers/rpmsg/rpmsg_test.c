/*
 * Remote processor messaging - test driver
 *
 * Copyright (C) 2012 Texas Instruments, Inc.
 *
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/rpmsg.h>
#include <linux/remoteproc.h>

/* maximum OMX devices this driver can handle */
#define MAX_RPMSG_TEST_DEVICES         256

struct rpmsg_test {
	struct list_head next;
	struct cdev cdev;
	struct device *dev;
	struct rpmsg_channel *rpdev;
	int minor;
};

static LIST_HEAD(rpmsg_test_list);
static struct class *rpmsg_test_class;
static dev_t rpmsg_test_dev;

static void rpmsg_test_cb(struct rpmsg_channel *rpdev, void *data, int len,
		void *priv, u32 src)
{
	dev_err(&rpdev->dev, "incoming msg \n");
}

static int rpmsg_test_open(struct inode *inode, struct file *filp)
{
	struct rpmsg_test *rpt;

	pr_err("Enter open\n");

	rpt = container_of(inode->i_cdev, struct rpmsg_test, cdev);

	filp->private_data = rpt;

	return 0;
}

static int rpmsg_test_release(struct inode *inode, struct file *filp)
{
	pr_err("Enter release\n");

	return 0;
}

static ssize_t rpmsg_test_write(struct file *filp, const char __user *ubuf,
		size_t len, loff_t *offp)
{
	struct rpmsg_test *rpt = filp->private_data;
	int use, ret;
	char kbuf[512];

	pr_err("Enter write\n");

	use = min(sizeof kbuf, len);

	if (copy_from_user(kbuf, ubuf, use))
		return -EMSGSIZE;

	ret = rpmsg_send(rpt->rpdev, kbuf, use);
	if (ret) {
		dev_err(rpt->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	return use;
}

static const struct file_operations rpmsg_test_fops = {
	.open           = rpmsg_test_open,
	.release        = rpmsg_test_release,
	//.unlocked_ioctl = rpmsg_test_ioctl,
	//.read           = rpmsg_test_read,
	.write          = rpmsg_test_write,
	//.poll           = rpmsg_test_poll,
	.owner          = THIS_MODULE,
};

static int rpmsg_test_probe(struct rpmsg_channel *rpdev)
{
	struct rproc *rproc = vdev_to_rproc(rpdev->vrp->vdev);
	int ret;
	struct rpmsg_test *rpt;
	static int  minor;
	int major;

	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n",
			rpdev->src, rpdev->dst);

	list_for_each_entry(rpt, &rpmsg_test_list, next) {
		if (!strcmp(dev_name(rpt->dev) + 11, rproc->name)) {
			pr_err("FOUND\n");
			goto found;
		}
	}

	rpt = kzalloc(sizeof *rpt, GFP_KERNEL);
	if (!rpt) {
		dev_err(&rpdev->dev, "kzalloc failed\n");
		return -ENOMEM;
	}

	major = MAJOR(rpmsg_test_dev);
	rpt->minor = ++minor;

	cdev_init(&rpt->cdev, &rpmsg_test_fops);
	rpt->cdev.owner = THIS_MODULE;
	ret = cdev_add(&rpt->cdev, MKDEV(major, minor), 1);
	if (ret) {
		dev_err(&rpdev->dev, "cdev_add failed: %d\n", ret);
		goto free_test;
	}

	rpt->dev = device_create(rpmsg_test_class, &rpdev->dev,
			MKDEV(major, minor), NULL,
			"rpmsg-test-%s", rproc->name);
	if (IS_ERR(rpt->dev)) {
		ret = PTR_ERR(rpt->dev);
		dev_err(&rpdev->dev, "device_create failed: %d\n", ret);
		goto clean_cdev;
	}

	list_add(&rpt->next, &rpmsg_test_list);
found:
	rpt->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, rpt);

	return 0;

clean_cdev:
	cdev_del(&rpt->cdev);
free_test:
	kfree(rpt);

	return ret;
}

static void __devexit rpmsg_test_remove(struct rpmsg_channel *rpdev)
{
	struct rproc *rproc = vdev_to_rproc(rpdev->vrp->vdev);
	struct rpmsg_test *rpt = dev_get_drvdata(&rpdev->dev);
	int major = MAJOR(rpmsg_test_dev);

	dev_info(&rpdev->dev, "rpmsg test driver is removed\n");

	if (rproc->state == RPROC_CRASHED)
		return;

	device_destroy(rpmsg_test_class, MKDEV(major, rpt->minor));
	cdev_del(&rpt->cdev);
	list_del(&rpt->next);
	kfree(rpt);
}

static struct rpmsg_device_id rpmsg_driver_test_id_table[] = {
	{ .name	= "rpmsg-test" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_test_id_table);

static struct rpmsg_driver rpmsg_test_driver = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rpmsg_driver_test_id_table,
	.probe		= rpmsg_test_probe,
	.callback	= rpmsg_test_cb,
	.remove		= __devexit_p(rpmsg_test_remove),
};

static int __init rpmsg_test_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&rpmsg_test_dev, 0, MAX_RPMSG_TEST_DEVICES,
			KBUILD_MODNAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto out;
	}

	rpmsg_test_class = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(rpmsg_test_class)) {
		ret = PTR_ERR(rpmsg_test_class);
		pr_err("class_create failed: %d\n", ret);
		goto unreg_region;
	}

	return register_rpmsg_driver(&rpmsg_test_driver);

unreg_region:
	unregister_chrdev_region(rpmsg_test_dev, MAX_RPMSG_TEST_DEVICES);
out:
	return ret;
}
module_init(rpmsg_test_init);

static void __exit rpmsg_test_fini(void)
{
	unregister_rpmsg_driver(&rpmsg_test_driver);
	class_destroy(rpmsg_test_class);
	unregister_chrdev_region(rpmsg_test_dev, MAX_RPMSG_TEST_DEVICES);
}
module_exit(rpmsg_test_fini);

MODULE_DESCRIPTION("Remote processor messaging test driver");
MODULE_LICENSE("GPL v2");
