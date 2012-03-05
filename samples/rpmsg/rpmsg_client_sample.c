/*
 * Remote processor messaging - sample client driver
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Brian Swetland <swetland@google.com>
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
#include <linux/rpmsg.h>
#include <linux/random.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>

static int msg_n = 100;
module_param(msg_n, int, 0);
static unsigned delay;
module_param(delay, uint, 0);
static bool rand;
module_param(rand, bool, 0);
static unsigned print_every = 1;
module_param(print_every, uint, 0);
static unsigned threads = 1;
module_param(threads, uint, 0);

struct client_sample
{
	struct rpmsg_channel *rpdev;
	struct task_struct *tsk;
	struct completion com;
	unsigned c;
};

struct client_msg {
	struct client_sample *cs;
	unsigned c;
};

static void rpmsg_sample_cb(struct rpmsg_channel *rpdev, void *data, int len,
						void *priv, u32 src)
{
	struct device *dev = &rpdev->dev;
	struct client_msg *cmsg = data;
	struct client_sample *c;
	unsigned i;

	if (len != sizeof(*cmsg)) {
		pr_err("message corrupted\n");
		return;
	}

	c = cmsg->cs;
	i = cmsg->c;
	if (!c)
		return;

	if (i != c->c++) {
		pr_err("####ERROR recived %d, expected %d####\n", i, c->c - 1);
		//BUG();
	}

	if (print_every && !(c->c % print_every))
		dev_err(dev, "incoming msg %u (src: 0x%x)\n", i, src);

	if (c->c == msg_n)
		dev_err(dev, "done!\n");

	complete(&c->com);
}

static int sample_thread(struct client_sample *c)
{
	int err;
	u32 r;
	struct client_msg cmsg;
	unsigned d = delay;
	struct rpmsg_channel *rpdev = c->rpdev;
	struct device *dev = &rpdev->dev;

	cmsg.cs = c;
	cmsg.c = c->c;

	while (!kthread_should_stop()) {
		if (c->c < msg_n) {
			if (rand) {
				get_random_bytes(&r, sizeof(r));
				d = r % delay;
			}
			msleep(d);
			cmsg.c = c->c;
			if (print_every && !(c->c % print_every))
				dev_err(dev,"sending %u\n", c->c);
			err = rpmsg_send(rpdev, &cmsg, sizeof(cmsg));
			if (err)
				pr_err("rpmsg_send failed: %d\n", err);
		}
		wait_for_completion_interruptible(&c->com);
	}

	return 0;
}

static int rpmsg_sample_probe(struct rpmsg_channel *rpdev)
{
	struct client_sample *c;
	int i;

	dev_info(&rpdev->dev, "new channel: 0x%x <-> 0x%x!\n",
			rpdev->src, rpdev->dst);

	rpdev->priv = c = kzalloc(sizeof(*c) * threads, GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	for (i = 0; i < threads; i++, c++) {
		init_completion(&c->com);
		c->rpdev = rpdev;
		c->tsk = kthread_run((void *)sample_thread, c,
					"rpmsg_sample%d\n", i);
	}

	return 0;
}

static void __devexit rpmsg_sample_remove(struct rpmsg_channel *rpdev)
{
	struct client_sample *c = rpdev->priv;
	int i;

	if (!c)
		return;

	for (i = 0; i < threads; i++, c++) {
		complete_all(&c->com);
		kthread_stop(c->tsk);
	}
	rpdev->priv = NULL;
	kfree(c);
	dev_info(&rpdev->dev, "rpmsg sample client driver is removed\n");
}

static struct rpmsg_device_id rpmsg_driver_sample_id_table[] = {
	{ .name	= "rpmsg-client-sample" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_sample_id_table);

static struct rpmsg_driver rpmsg_sample_client = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rpmsg_driver_sample_id_table,
	.probe		= rpmsg_sample_probe,
	.callback	= rpmsg_sample_cb,
	.remove		= __devexit_p(rpmsg_sample_remove),
};

static int __init rpmsg_client_sample_init(void)
{
	return register_rpmsg_driver(&rpmsg_sample_client);
}
module_init(rpmsg_client_sample_init);

static void __exit rpmsg_client_sample_fini(void)
{
	unregister_rpmsg_driver(&rpmsg_sample_client);
}
module_exit(rpmsg_client_sample_fini);

MODULE_DESCRIPTION("Remote processor messaging sample client driver");
MODULE_LICENSE("GPL v2");
