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

/*
 * 128 bytes should be enogh to cover all the parameters for all resource
 * types, in case you add a resource with a lot of parameters please
 * update this value to cover this new resource
 */
#define MAX_RES_SIZE 128
#define MAX_MSG (sizeof(struct rprm_msg) + sizeof(struct rprm_request) +\
		MAX_RES_SIZE)

/* size of the buffer to get the resource information */
#define MAX_RES_BUF 512

/**
 * struct rprm_elem - resource manager element structure
 * @next:	pointer to next element.
 * @res:	pointer to the rprm_res structure which this element is
 *		derived from.
 * @handle:	stores handle returned by low level modules.
 * @id:		unique id for this element.
 *
 * Each resource requested by a remoteproc is represented by an instance
 * of this structure. A list of resources or elements are created per
 * each remoteproc connection with the RM. This list is useful to keep
 * track of resources based on connections. The id is an unique id inside
 * the connection, that means 2 resources of different connections could
 * have the same id.
 */
struct rprm_elem {
	struct list_head next;
	struct rprm_res *res;
	void *handle;
	u32 id;
};

/**
 * struct rprm - basic remoteproc resource manager structure
 * @res_list:	list of resources allocated.
 * @id_list:	list of resource ids.
 * @lock:	protection for both lists, res_list and id_list.
 * @rpdev:	pointer to the rpmsg channel.
 * @mgr:	pointer to the manager which the remoteproc is connected to.
 * @dentry:	debugfs entry for dumping resource information.
 *
 * Every time a remoteproc wants to connect with the resource manager server
 * running in the host will send a channel creation request (Using Name Map
 * Server) which will create a rpmsg channel which will be probed against this
 * resource manager server (driver). The struct rprm represents the connection
 * between the RM server and a remoteproc client, each instance represents a
 * new connection and each remoteproc can have whatever number of connections
 * with the RM server. The name of the channel should match with a specific RM
 * implemented by the level drivers previously registered and once there is a
 * match the manager which matched is stored in the @mgr element.
 */
struct rprm {
	struct list_head res_list;
	struct idr id_list;
	struct mutex lock;
	struct rpmsg_channel *rpdev;
	struct rprm_manager *mgr;
	struct dentry *dentry;
};

/* List of availabe managers */
static LIST_HEAD(mgr_table);
/* lock for mgr_table */
static DEFINE_SPINLOCK(table_lock);

/* rproc resource manager debugfs parent dir */
static struct dentry *rprm_dbg;

/* this function must be called with table_lock held */
static struct rprm_manager *__find_mgr_by_name(const char *name)
{
	struct rprm_manager *mgr;

	list_for_each_entry(mgr, &mgr_table, next)
		if (!strcmp(mgr->name, name))
			return mgr;

	return NULL;
}

static int _resource_release(struct rprm *rprm, struct rprm_elem *e)
{
	struct device *dev = &rprm->rpdev->dev;
	struct rprm_res *res = e->res;
	int ret;

	dev_dbg(dev, "releasing %s resource\n", res->name);

	ret = res->release(rprm->mgr, e->handle);
	if (ret) {
		dev_err(dev, "failed to release %s ret %d\n", res->name, ret);
		return ret;
	}

	list_del(&e->next);
	kfree(e);

	return 0;
}

static int rprm_resource_release(struct rprm *rprm, int res_id)
{
	struct device *dev = &rprm->rpdev->dev;
	struct rprm_manager *mgr = rprm->mgr;
	struct rprm_elem *e;

	dev_dbg(dev, "releasing id %d from manager %s\n", res_id, mgr->name);

	mutex_lock(&rprm->lock);
	e = idr_find(&rprm->id_list, res_id);
	if (!e) {
		mutex_unlock(&rprm->lock);
		dev_err(dev, "invalid resource id\n");
		return -ENOENT;
	}

	idr_remove(&rprm->id_list, res_id);
	mutex_unlock(&rprm->lock);

	return _resource_release(rprm, e);
}

static int rprm_resource_request(struct rprm *rprm, unsigned idx,
				int *res_id, void *data, size_t len)
{
	struct device *dev = &rprm->rpdev->dev;
	struct rprm_manager *mgr = rprm->mgr;
	struct rprm_elem *e;
	struct rprm_res *res;
	int ret;

	dev_dbg(dev, "requesting index %d from manager %s\n", idx, mgr->name);

	if (idx >= mgr->res_cnt) {
		dev_err(dev, "invalid index %d for manager %s\n",
			idx, mgr->name);
		return -EINVAL;
	}

	/* get resource structure based on the index */
	res = &mgr->resources[idx];

	dev_dbg(dev, "requesting resource %s data len %d\n", res->name, len);

	e = kzalloc(sizeof *e, GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	ret = res->request(mgr, &e->handle, data, len);
	if (ret) {
		dev_err(dev, "request for %s failed: %d\n", res->name, ret);
		goto err_free;
	}
	/*
	 * Create a resource id to avoid sending kernel address to the
	 * remote processor.
	 */
	mutex_lock(&rprm->lock);
	if (!idr_pre_get(&rprm->id_list, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	ret = idr_get_new(&rprm->id_list, e, res_id);
	if (ret)
		goto err_unlock;

	e->id = *res_id;
	e->res = res;
	list_add(&e->next, &rprm->res_list);
	mutex_unlock(&rprm->lock);

	return 0;
err_unlock:
	mutex_unlock(&rprm->lock);
	res->release(mgr, e->handle);
err_free:
	kfree(e);
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

	dev_dbg(dev, "resmgr msg from %u and len %d\n" , src, len);

	len -= sizeof(*msg);
	if (len < 0) {
		dev_err(dev, "Bad message: no message header\n");
		return;
	}

	/* we only accept action request from established channels */
	if (rpdev->dst != src) {
		dev_err(dev, "remote endpoint %d not connected to this resmgr\n"
			"channel, expected %d endpoint\n", src, rpdev->dst);
		ret = -ENOTCONN;
		goto out;
	}

	dev_dbg(dev, "resmgr action %d\n" , msg->action);

	switch (msg->action) {
	case RPRM_REQUEST:
		len -= sizeof(*req);
		if (len < 0) {
			dev_err(dev, "Bad message: no request header\n");
			ret = -EINVAL;
			break;
		}

		req = (void *)msg->data;
		ret = rprm_resource_request(rprm, req->idx, &res_id,
						req->data, len);
		if (ret) {
			dev_err(dev, "resource allocation failed %d\n", ret);
			break;
		}

		rack->res_id = res_id;
		memcpy(rack->data, req->data, len);
		len += sizeof(*rack);
		break;
	case RPRM_RELEASE:
		len -= sizeof(*rel);
		if (len < 0) {
			dev_err(dev, "Bad message: no release header\n");
			return;
		}

		rel = (void *)msg->data;
		ret = rprm_resource_release(rprm, rel->res_id);
		if (ret)
			dev_err(dev, "resource release failed %d\n", ret);
		/* no ack for release resource */
		return;
	default:
		dev_err(dev, "Unknow action %d\n", msg->action);
		ret = -EINVAL;
	}
out:
	ack->action = msg->action;
	ack->ret = ret;
	/* in case of error, no need for any payload but the ack header */
	len = ret ? 0 : len;
	ret = rpmsg_sendto(rpdev, ack, sizeof(*ack) + len, src);
	if (ret)
		dev_err(dev, "rprm send ack failed: %d\n", ret);
}

static ssize_t rprm_dbg_read(struct file *filp, char __user *userbuf,
		size_t count, loff_t *ppos)
{
	struct rprm *rprm = filp->private_data;
	struct rprm_elem *e;
	char buf[MAX_RES_BUF];
	int total = 0, c, tmp;
	loff_t p = 0, pt;
	size_t len = MAX_RES_BUF;

	c = snprintf(buf, len, "## resource list for remote endpoint %d ##\n",
			rprm->rpdev->src);

	mutex_lock(&rprm->lock);
	list_for_each_entry(e, &rprm->res_list, next) {
		c += snprintf(buf + c, len - c , "\n-resource name:%s\n",
				e->res->name);
		if (e->res->get_info)
			c += e->res->get_info(rprm->mgr, e->handle,
					buf + c, len - c);

		p += c;
		/* if resource was already read then continue */
		if (*ppos >= p) {
			c = 0;
			continue;
		}

		pt = c - p + *ppos;
		tmp = simple_read_from_buffer(userbuf + total, count, &pt,
				buf, c);
		total += tmp;
		*ppos += tmp;
		/* if userbuf is full then break */
		if (tmp - c)
			break;
		c = 0;
	}
	mutex_unlock(&rprm->lock);

	return total;
}

static const struct file_operations rprm_dbg_ops = {
	.read = rprm_dbg_read,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

/**
 * rprm_manager_register() - register a new resource manager
 * @mgr: pointer to the manager structure
 *
 * This function registers a new manager @mgr with the generic RM framework,
 * so that, all resource exported by the manager can be requested by a remote
 * processor and then release them when the resources are not needed anymore.
 *
 * All managers are stored in a linked list, so that when a remoteproc creates
 * a new channel with the RM server, it can match the channel with the suitable
 * manager implemented by a low lever driver.
 *
 * This function should be called inside low level driver probe function. Every
 * time a new connection with a specific manager is created the module ref
 * count will be increased to avoid removing the module when there are still
 * active connections with the manager.
 *
 * On success 0 is returned. Otherwise, will return proper error.
 */
int rprm_manager_register(struct rprm_manager *mgr)
{
	if (!mgr || !mgr->name)
		return -EINVAL;

	pr_debug("registering manager %s\n", mgr->name);
	spin_lock(&table_lock);
	/* managers cannot have the same name */
	if (__find_mgr_by_name(mgr->name)) {
		spin_unlock(&table_lock);
		pr_err("manager %s already exists!\n", mgr->name);
		return -EEXIST;
	}

	list_add_tail(&mgr->next, &mgr_table);
	spin_unlock(&table_lock);
	if (rprm_dbg)
		mgr->dentry = debugfs_create_dir(mgr->name, rprm_dbg);

	return 0;
}
EXPORT_SYMBOL(rprm_manager_register);

/**
 * rprm_manager_unregister - unregister a resource manager
 * @mgr: pointer to the manager structure
 *
 * This function unregisters a manager @mgr previously registered that means,
 * it will remove the manager from the manager list. After that if a remoteproc
 * creates a new channel to connect with this manager it will fail because
 * the manager is not in the list anymore. This function should be called
 * inside low level driver remove function. When there are connections to
 * a specific manager the module ref counter will be different from zero and
 * should avoid unregistering manager when they are still being used.
 *
 * On success 0 is returned. This function will return -EBUSY if the function
 * is called when there are still connections from the remoteproc to the
 * resource manager. Otherwise, -EINVAL is returned.
 */
int rprm_manager_unregister(struct rprm_manager *mgr)
{
	if (!mgr)
		return -EINVAL;

	pr_debug("unregistering manager %s\n", mgr->name);

	spin_lock(&table_lock);
	if (module_refcount(mgr->owner)) {
		spin_unlock(&table_lock);
		pr_err("connetions still using %s\n", mgr->name);
		return -EBUSY;
	}

	list_del(&mgr->next);
	spin_unlock(&table_lock);
	if (rprm_dbg)
		debugfs_remove(mgr->dentry);

	return 0;
}
EXPORT_SYMBOL(rprm_manager_unregister);

/* probe function is called every time a new connection(device) is created */
static int rprm_probe(struct rpmsg_channel *rpdev)
{
	struct rprm *rprm;
	struct rprm_ack ack;
	struct device *dev = &rpdev->dev;
	struct rprm_manager *mgr;
	int ret = 0;

	spin_lock(&table_lock);
	/*
	 * find the manager for this channel. The channel id name is used to
	 * match the manager, so remoteproc has to create the channel using the
	 * name of the manager it wants to connect to.
	 */
	mgr = __find_mgr_by_name(rpdev->id.name);
	if (!mgr) {
		spin_unlock(&table_lock);
		pr_err("manager %s does not exists!\n", rpdev->id.name);
		ret = -ENOENT;
		goto out;
	}

	/* prevent underlying manager implementation from being removed */
	if (!try_module_get(mgr->owner)) {
		spin_unlock(&table_lock);
		dev_err(dev, "can't get mgr module owner\n");
		ret = -EINVAL;
		goto out;
	}
	spin_unlock(&table_lock);

	rprm = kmalloc(sizeof *rprm, GFP_KERNEL);
	if (!rprm) {
		module_put(mgr->owner);
		ret = -ENOMEM;
		goto out;
	}

	rprm->mgr = mgr;
	mutex_init(&rprm->lock);
	idr_init(&rprm->id_list);
	/*
	 * beside the idr we create a linked list, that way we can cleanup the
	 * resources allocated in reverse order as they were requested which
	 * is safer
	 */
	INIT_LIST_HEAD(&rprm->res_list);
	rprm->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, rprm);
	/*
	 * create a debug entry which can be read to get resources associated
	 * to this connection
	 */
	if (rprm_dbg)
		rprm->dentry = debugfs_create_file(dev_name(dev), 0400,
				mgr->dentry, rprm, &rprm_dbg_ops);
out:
	/* send a message back to ack the connection */
	ack.ret = ret;
	if (rpmsg_send(rpdev, &ack, sizeof ack))
		dev_err(dev, "error sending respond!\n");

	return ret;
}

/* remove function is called when the connection is terminated */
static void __devexit rprm_remove(struct rpmsg_channel *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rprm *rprm = dev_get_drvdata(dev);
	struct rprm_elem *e, *tmp;

	/* clean up remaining resources when connection is closed */
	mutex_lock(&rprm->lock);
	list_for_each_entry_safe(e, tmp, &rprm->res_list, next)
		_resource_release(rprm, e);

	idr_remove_all(&rprm->id_list);
	idr_destroy(&rprm->id_list);
	mutex_unlock(&rprm->lock);
	if (rprm->dentry)
		debugfs_remove(rprm->dentry);

	module_put(rprm->mgr->owner);
	kfree(rprm);
}

/*
 * the low level driver implementing a new resource manager should register
 * its manager name in the rprm_id_table so that, the channels created by
 * the rproc can be probed against this driver and then linked to the manager
 */
static struct rpmsg_device_id rprm_id_table[] = {
	{ .name = "rprm-ducati" },
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
	if (debugfs_initialized()) {
		rprm_dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
		if (!rprm_dbg)
			pr_err("can't create resource manager debugfs dir\n");
	}

	return register_rpmsg_driver(&rprm_driver);
}
module_init(rprm_init);

static void __exit rprm_fini(void)
{
	unregister_rpmsg_driver(&rprm_driver);
	if (rprm_dbg)
		debugfs_remove(rprm_dbg);
}
module_exit(rprm_fini);

MODULE_DESCRIPTION("Remote Processor Resource Manager");
MODULE_LICENSE("GPL v2");
