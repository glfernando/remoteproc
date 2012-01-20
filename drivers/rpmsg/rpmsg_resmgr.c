/*
 * Remote processor resource manager
 *
 * Copyright (C) 2012 Texas Instruments, Inc.
 *
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 * Miguel Vadillo <vadillo@ti.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/rpmsg.h>
#include <linux/idr.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg_resmgr.h>

#define MAX_RES_SIZE 128
#define MAX_MSG (sizeof(struct rprm_msg) + sizeof(struct rprm_request) +\
		MAX_RES_SIZE)
#define MAX_RES_BUF 512
#define MAX_DENTRY_SIZE 128

struct rprm_elem {
	struct list_head next;
	struct rprm_res *res;
	void *handle;
	u32 id;
	u32 base;
};

struct rprm {
	struct list_head next;
	struct list_head res_list;
	struct idr id_list;
	struct mutex lock;
	struct dentry *dentry;
	struct rpmsg_channel *rpdev;
	u32 src;
};

/* List of availabe resources */
static LIST_HEAD(res_table);
/* Global list of resmgr channels */
static LIST_HEAD(ch_table);
static DECLARE_RWSEM(table_lock);

static struct rprm_res *__find_res_by_name(const char *name)
{
	struct rprm_res *res;

	list_for_each_entry(res, &res_table, next)
		if (!strcmp(res->name, name))
			return res;

	return NULL;
}

static int _resource_release(struct rprm *rprm, struct rprm_elem *e)
{
	int ret;
	struct device *dev = &rprm->rpdev->dev;

	dev_dbg(dev, "releasing %s resource\n", e->res->name);

	if (e->res->ops->release) {
		ret = e->res->ops->release(e->handle);
		if (ret) {
			dev_err(dev, "failed to release %s ret %d\n",
							e->res->name, ret);
			return ret;
		}
	}

	list_del(&e->next);
	kfree(e);

	return 0;
}

static int rprm_resource_release(struct rprm *rprm, int res_id)
{
	struct device *dev = &rprm->rpdev->dev;
	struct rprm_elem *e;
	int ret;

	mutex_lock(&rprm->lock);
	e = idr_find(&rprm->id_list, res_id);
	if (!e) {
		dev_err(dev, "invalid resource id\n");
		ret = -ENOENT;
		goto out;
	}

	ret = _resource_release(rprm, e);
	if (ret)
		goto out;

	idr_remove(&rprm->id_list, res_id);
out:
	mutex_unlock(&rprm->lock);

	return ret;
}

static int rprm_resource_request(struct rprm *rprm, const char *name,
				int *res_id, void *data, size_t len)
{
	struct device *dev = &rprm->rpdev->dev;
	struct rprm_elem *e;
	struct rprm_res *res;
	int ret;

	dev_dbg(dev, "requesting %s resoruce\n", name);

	res = __find_res_by_name(name);
	if (!res) {
		dev_err(dev, "resource no valid %s\n", name);
		return -ENOENT;
	}

	if (!res->ops->request)
		return -ENOSYS;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	mutex_lock(&rprm->lock);
	ret = res->ops->request(&e->handle, data, len);
	if (ret) {
		dev_err(dev, "request for %s failed: %d\n", name, ret);
		goto err_free;
	}
	/*
	 * Create a resource id to avoid sending kernel address to the
	 * remote processor.
	 */
	if (!idr_pre_get(&rprm->id_list, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_release;
	}

	ret = idr_get_new(&rprm->id_list, e, res_id);
	if (ret)
		goto err_release;

	e->id = *res_id;
	e->res = res;
	list_add(&e->next, &rprm->res_list);
	mutex_unlock(&rprm->lock);

	return 0;
err_release:
	if (res->ops->release)
		res->ops->release(e->handle);
err_free:
	kfree(e);
	mutex_unlock(&rprm->lock);

	return ret;
}

static void rprm_cb(struct rpmsg_channel *rpdev, void *data, int len,
			void *priv, u32 src)
{
	struct device *dev = &rpdev->dev;
	struct rprm *rprm = dev_get_drvdata(dev);
	struct rprm_msg *msg = data;
	struct rprm_request *req;
	struct rprm_release *rel;
	char ack_msg[MAX_MSG];
	struct rprm_ack *ack = (void *)ack_msg;
	struct rprm_request_ack *rack = (void *)ack->data;
	int res_id;
	int ret;

	len -= sizeof(*msg);
	if (len < 0) {
		dev_err(dev, "Bad message\n");
		return;
	}

	/* we only accept action request from established channels */
	if (rpdev->dst != src) {
		dev_err(dev, "remote endpoint %d not connected to this resmgr "
			"channel, expected %d endpoint\n", src, rpdev->dst);
		ret = -ENOTCONN;
		goto out;
	}

	dev_dbg(dev, "resmgr action %d\n" , msg->action);

	switch (msg->action) {
	case RPRM_REQUEST:
		len -= sizeof(*req);
		if (len < 0) {
			dev_err(dev, "Bad message\n");
			ret = -EINVAL;
			break;
		}

		req = (void *)msg->data;
		req->res_name[sizeof(req->res_name) - 1] = '\0';
		down_read(&table_lock);
		ret = rprm_resource_request(rprm, req->res_name, &res_id,
								req->data, len);
		up_read(&table_lock);
		if (ret)
			dev_err(dev, "resource allocation failed %d!\n", ret);

		rack->res_id = res_id;
		memcpy(rack->data, req->data, len);
		len += sizeof(*rack);
		break;
	case RPRM_RELEASE:
		len -= sizeof(*rel);
		if (len < 0) {
			dev_err(dev, "Bad message\n");
			return;
		}

		rel = (void *)msg->data;
		down_read(&table_lock);
		ret = rprm_resource_release(rprm, rel->res_id);
		up_read(&table_lock);
		if (ret)
			dev_err(dev, "resource release failed %d!\n", ret);
		/* no ack for release resource */
		return;
	default:
		dev_err(dev, "Unknow action\n");
		ret = -EINVAL;
	}
out:
	ack->action = msg->action;
	ack->ret = ret;
	len = ret ? 0 : len;
	ret = rpmsg_sendto(rpdev, ack, sizeof(*ack) + len, src);
	if (ret)
		dev_err(dev, "rprm ack failed: %d\n", ret);
}

static ssize_t rprm_dbg_read(struct file *filp, char __user *userbuf,
			 size_t count, loff_t *ppos)
{
	struct rprm *rprm = filp->private_data;
	struct rprm_elem *e;
	char buf[MAX_RES_BUF];
	int total = 0, c, tmp;
	loff_t p = 0, pt;

	mutex_lock(&rprm->lock);
	c = sprintf(buf, "## resource list for remote endpoint %d ##\n",
							rprm->rpdev->src);
	list_for_each_entry(e, &rprm->res_list, next) {
		c += sprintf(buf + c, "\nresource name:%s\n", e->res->name);

		if (e->res->ops->get_info)
			c += e->res->ops->get_info(e->handle, buf + c);

		p += c;
		if (*ppos >= p) {
			c = 0;
			continue;
		}

		pt = c - p + *ppos;
		tmp = simple_read_from_buffer(userbuf + total, count, &pt,
						 buf, c);
		total += tmp;
		*ppos += tmp;
		if (tmp - c)
			break;
		c = 0;
	}
	mutex_unlock(&rprm->lock);

	return total;
}

static int rprm_dbg_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations rprm_dbg_ops = {
	.read = rprm_dbg_read,
	.open = rprm_dbg_open,
	.llseek	= generic_file_llseek,
};

/**
 * rprm_resource_register - register a new resource
 * @res: pointer to the resource structure
 *
 * This function registers a new resource @res so that, a remote processor
 * can request it and then release it when it is not needed anymore.
 *
 * On success 0 is returned. Otherwise, will return proper error.
 */
int rprm_resource_register(struct rprm_res *res)
{
	int ret = 0;

	if (!res || !res->name || !res->ops)
		return -EINVAL;

	down_write(&table_lock);
	if (__find_res_by_name(res->name)) {
		pr_err("resource %s already exists!\n", res->name);
		ret = -EEXIST;
		goto unlock;
	}

	list_add_tail(&res->next, &res_table);
	pr_debug("registering resource %s\n", res->name);
unlock:
	up_write(&table_lock);

	return ret;
}
EXPORT_SYMBOL(rprm_resource_register);

/**
 * rprm_resource_unregister - unregister a resource
 * @res: pointer to the resource structure
 *
 * This function unregisters a resource @res previously registered. After this
 * the remote processor cannot request @res anymore.
 *
 * On success 0 is returned. Otherwise, -EINVAL is returned.
 */
int rprm_resource_unregister(struct rprm_res *res)
{
	struct rprm *rprm;
	struct rprm_elem *e, *tmp;

	if (!res)
		return -EINVAL;

	/* clean up resources of type @res for all channels */
	down_write(&table_lock);
	list_for_each_entry(rprm, &ch_table, next) {
		mutex_lock(&rprm->lock);
		list_for_each_entry_safe(e, tmp, &rprm->res_list, next) {
			if (e->res != res)
				continue;
			_resource_release(rprm, e);
			idr_remove(&rprm->id_list, e->id);
		}
		mutex_unlock(&rprm->lock);
		list_del(&res->next);
	}
	up_write(&table_lock);
	pr_debug("unregistering resource %s\n", res->name);

	return 0;
}
EXPORT_SYMBOL(rprm_resource_unregister);

static int rprm_probe(struct rpmsg_channel *rpdev)
{
	struct rprm *rprm;
	struct rprm_ack ack;
	struct rproc *rproc = vdev_to_rproc(rpdev->vrp->vdev);
	char name[MAX_DENTRY_SIZE];
	int ret = 0;

	rprm = kmalloc(sizeof(*rprm), GFP_KERNEL);
	if (!rprm) {
		ret = -ENOMEM;
		goto out;
	}

	mutex_init(&rprm->lock);
	idr_init(&rprm->id_list);
	/*
	 * Beside the idr we create a linked list, that way we can cleanup the
	 * resources allocated in reverse order as they were requested which
	 * is safer.
	 */
	INIT_LIST_HEAD(&rprm->res_list);
	rprm->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, rprm);

	snprintf(name, MAX_DENTRY_SIZE, "resmgr-%s", dev_name(&rpdev->dev));
	name[MAX_DENTRY_SIZE - 1] = '\0';
	rprm->dentry = debugfs_create_file(name, 0400, rproc->dbg_dir, rprm,
							&rprm_dbg_ops);
	down_write(&table_lock);
	list_add(&rprm->next, &ch_table);
	up_write(&table_lock);
out:
	ack.action = RPRM_CONNECT;
	ack.ret = ret;
	if (rpmsg_send(rpdev, &ack, sizeof(ack)))
		dev_err(&rpdev->dev, "error sending respond!\n");

	return ret;
}

static void __devexit rprm_remove(struct rpmsg_channel *rpdev)
{
	struct rprm *rprm = dev_get_drvdata(&rpdev->dev);
	struct rprm_elem *e, *tmp;

	/* clean up remaining resources */
	mutex_lock(&rprm->lock);
	list_for_each_entry_safe(e, tmp, &rprm->res_list, next)
		_resource_release(rprm, e);

	idr_remove_all(&rprm->id_list);
	idr_destroy(&rprm->id_list);
	mutex_unlock(&rprm->lock);

	down_write(&table_lock);
	list_del(&rprm->next);
	up_write(&table_lock);

	kfree(rprm);

	if (rprm->dentry)
		debugfs_remove(rprm->dentry);
}

static struct rpmsg_device_id rprm_id_table[] = {
	{
		.name	= "rpmsg-resmgr",
	},
	{ },
};
MODULE_DEVICE_TABLE(platform, rprm_id_table);

static struct rpmsg_driver rprm_driver = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rprm_id_table,
	.probe		= rprm_probe,
	.callback	= rprm_cb,
	.remove		= __devexit_p(rprm_remove),
};

static int __init rprm_init(void)
{
	return register_rpmsg_driver(&rprm_driver);
}

static void __exit rprm_fini(void)
{
	unregister_rpmsg_driver(&rprm_driver);
}
module_init(rprm_init);
module_exit(rprm_fini);

MODULE_DESCRIPTION("Remote Processor Resource Manager");
MODULE_LICENSE("GPL v2");
