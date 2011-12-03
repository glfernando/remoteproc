/*
 * OMAP Remote Processor driver
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Brian Swetland <swetland@google.com>
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 * Mark Grosen <mgrosen@ti.com>
 * Suman Anna <s-anna@ti.com>
 * Hari Kanigeri <h-kanigeri2@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/remoteproc.h>

#include <plat/mailbox.h>
#include <linux/platform_data/remoteproc-omap.h>

#include "omap_remoteproc.h"
#include "remoteproc_internal.h"

/* 1 sec is fair enough time for suspending an OMAP device */
#define DEF_SUSPEND_TIMEOUT 1000

/**
 * struct omap_rproc - omap remote processor state
 * @mbox: omap mailbox handle
 * @nb: notifier block that will be invoked on inbound mailbox messages
 * @rproc: rproc handle
 * @pm_comp: completion needed for suspend respond
 * @idle: address to the idle register
 * @idle_mask: mask of the idle register
 * @suspend_timeout: max time it can wait for the suspend respond
 * @suspend_acked: flag that says if the suspend request was acked
 * @suspended: flag that says if rproc suspended
 * @need_kick: flag that says if vrings need to be kicked on resume
 */
struct omap_rproc {
	struct omap_mbox *mbox;
	struct notifier_block nb;
	struct rproc *rproc;
	struct completion pm_comp;
	void __iomem *idle;
	u32 idle_mask;
	unsigned long suspend_timeout;
	bool suspend_acked;
	bool suspended;
	bool need_kick;
};

/**
 * omap_rproc_mbox_callback() - inbound mailbox message handler
 * @this: notifier block
 * @index: unused
 * @data: mailbox payload
 *
 * This handler is invoked by omap's mailbox driver whenever a mailbox
 * message is received. Usually, the mailbox payload simply contains
 * the index of the virtqueue that is kicked by the remote processor,
 * and we let remoteproc core handle it.
 *
 * In addition to virtqueue indices, we also have some out-of-band values
 * that indicates different events. Those values are deliberately very
 * big so they don't coincide with virtqueue indices.
 */
static int omap_rproc_mbox_callback(struct notifier_block *this,
					unsigned long index, void *data)
{
	mbox_msg_t msg = (mbox_msg_t) data;
	struct omap_rproc *oproc = container_of(this, struct omap_rproc, nb);
	struct device *dev = oproc->rproc->dev.parent;
	const char *name = oproc->rproc->name;

	dev_dbg(dev, "mbox msg: 0x%x\n", msg);

	switch (msg) {
	case RP_MBOX_CRASH:
		/* just log this for now. later, we'll also do recovery */
		dev_err(dev, "omap rproc %s crashed\n", name);
		break;
	case RP_MBOX_ECHO_REPLY:
		dev_info(dev, "received echo reply from %s\n", name);
		break;
	case RP_MBOX_SUSPEND_ACK:
	case RP_MBOX_SUSPEND_CANCEL:
		oproc->suspend_acked = msg == RP_MBOX_SUSPEND_ACK;
		complete(&oproc->pm_comp);
		break;
	default:
		/* msg contains the index of the triggered vring */
		if (rproc_vq_interrupt(oproc->rproc, msg) == IRQ_NONE)
			dev_dbg(dev, "no message was found in vqid %d\n", msg);
	}

	return NOTIFY_DONE;
}

/* kick a virtqueue */
static void omap_rproc_kick(struct rproc *rproc, int vqid)
{
	struct omap_rproc *oproc = rproc->priv;
	struct device *dev = rproc->dev.parent;
	int ret;

	/* if suspended set need kick flag to kick on resume */
	if (oproc->suspended) {
		oproc->need_kick = true;
		return;
	}
	/* send the index of the triggered virtqueue in the mailbox payload */
	ret = omap_mbox_msg_send(oproc->mbox, vqid);
	if (ret)
		dev_err(dev, "omap_mbox_msg_send failed: %d\n", ret);
}

/*
 * Power up the remote processor.
 *
 * This function will be invoked only after the firmware for this rproc
 * was loaded, parsed successfully, and all of its resource requirements
 * were met.
 */
static int omap_rproc_start(struct rproc *rproc)
{
	struct omap_rproc *oproc = rproc->priv;
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc_pdata *pdata = pdev->dev.platform_data;
	int ret;

	if (pdata->set_bootaddr)
		pdata->set_bootaddr(rproc->bootaddr);

	oproc->nb.notifier_call = omap_rproc_mbox_callback;

	/* every omap rproc is assigned a mailbox instance for messaging */
	oproc->mbox = omap_mbox_get(pdata->mbox_name, &oproc->nb);
	if (IS_ERR(oproc->mbox)) {
		ret = PTR_ERR(oproc->mbox);
		dev_err(dev, "omap_mbox_get failed: %d\n", ret);
		return ret;
	}

	/*
	 * Ping the remote processor. this is only for sanity-sake;
	 * there is no functional effect whatsoever.
	 *
	 * Note that the reply will _not_ arrive immediately: this message
	 * will wait in the mailbox fifo until the remote processor is booted.
	 */
	ret = omap_mbox_msg_send(oproc->mbox, RP_MBOX_ECHO_REQUEST);
	if (ret) {
		dev_err(dev, "omap_mbox_get failed: %d\n", ret);
		goto put_mbox;
	}

	ret = pdata->deassert_reset(pdev, "cpu0");
	if (ret) {
		dev_err(dev, "deassert_hardreset failed: %d\n", ret);
		goto put_mbox;
	}

	ret = pdata->device_enable(pdev);
	if (ret) {
		dev_err(dev, "omap_device_enable failed: %d\n", ret);
		goto assert_reset;
	}

	return 0;

assert_reset:
	pdata->assert_reset(pdev, "cpu0");
put_mbox:
	omap_mbox_put(oproc->mbox, &oproc->nb);
	return ret;
}

/* power off the remote processor */
static int omap_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc_pdata *pdata = pdev->dev.platform_data;
	struct omap_rproc *oproc = rproc->priv;
	int ret;

	ret = pdata->device_shutdown(pdev);
	if (ret)
		return ret;

	ret = pdata->assert_reset(pdev, "cpu0");
	if (ret)
		return ret;

	omap_mbox_put(oproc->mbox, &oproc->nb);

	return 0;
}

static bool _rproc_idled(struct omap_rproc *oproc)
{
	return !oproc->idle || readl(oproc->idle) & oproc->idle_mask;
}

static int _suspend(struct rproc *rproc, bool auto_suspend)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc_pdata *pdata = dev->platform_data;
	struct omap_rproc *oproc = rproc->priv;
	unsigned long to = msecs_to_jiffies(oproc->suspend_timeout);
	unsigned long ta = jiffies + to;
	int ret;

	init_completion(&oproc->pm_comp);
	oproc->suspend_acked = false;
	omap_mbox_msg_send(oproc->mbox,
		auto_suspend ? RP_MBOX_SUSPEND : RP_MBOX_SUSPEND_FORCED);
	ret = wait_for_completion_timeout(&oproc->pm_comp, to);
	if (!oproc->suspend_acked)
		return -EBUSY;

	/*
	 * FIXME: Ducati side is returning the ACK message before saving the
	 * context, becuase the function which saves the context is a
	 * SYSBIOS function that can not be modified until a new SYSBIOS
	 * release is done. However, we can know that Ducati already saved
	 * the context once it reaches idle again (after saving the context
	 * ducati executes WFI instruction), so this way we can workaround
	 * this problem.
	 */
	if (oproc->idle) {
		while (!_rproc_idled(oproc)) {
			if (time_after(jiffies, ta))
				return -ETIME;
			schedule();
		}
	}

	ret = pdata->device_shutdown(pdev);
	if (ret)
		return ret;

	ret = pdata->assert_reset(pdev, "cpu0");
	if (ret)
		return ret;

	oproc->suspended = true;

	return 0;
}

static int omap_rproc_suspend(struct rproc *rproc, bool auto_suspend)
{
	struct omap_rproc *oproc = rproc->priv;

	if (auto_suspend && !_rproc_idled(oproc))
		return -EBUSY;

	return _suspend(rproc, auto_suspend);
}

static int _resume_kick(int id, void *p, void *rproc)
{
	omap_rproc_kick(rproc, id);
	return 0;
}

static int omap_rproc_resume(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc_pdata *pdata = dev->platform_data;
	struct omap_rproc *oproc = rproc->priv;
	int ret;

	if (pdata->set_bootaddr)
		pdata->set_bootaddr(rproc->bootaddr);

	ret = pdata->deassert_reset(pdev, "cpu0");
	if (ret) {
		dev_err(dev, "deassert_hardreset failed: %d\n", ret);
		return ret;
	}

	ret = pdata->device_enable(pdev);
	if (ret) {
		dev_err(dev, "omap_device_enable failed: %d\n", ret);
		return ret;
	}

	oproc->suspended = false;
	/*
	 * if need_kick flag is true, we need to kick all the vrings as
	 * we do not know which vrings were tried to be kicked while the
	 * rproc was suspended. We can optimize later, however this scenario
	 * is very rarely, so it is not big deal.
	 */
	if (oproc->need_kick) {
		idr_for_each(&rproc->notifyids, _resume_kick, rproc);
		oproc->need_kick = false;
	}

	return 0;
}

static struct rproc_ops omap_rproc_ops = {
	.start		= omap_rproc_start,
	.stop		= omap_rproc_stop,
	.kick		= omap_rproc_kick,
	.suspend	= omap_rproc_suspend,
	.resume		= omap_rproc_resume,
};

static int __devinit omap_rproc_probe(struct platform_device *pdev)
{
	struct omap_rproc_pdata *pdata = pdev->dev.platform_data;
	struct omap_rproc *oproc;
	struct rproc *rproc;
	int ret;

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "dma_set_coherent_mask: %d\n", ret);
		return ret;
	}

	rproc = rproc_alloc(&pdev->dev, pdata->name, &omap_rproc_ops,
				pdata->firmware, sizeof(*oproc));
	if (!rproc)
		return -ENOMEM;

	oproc = rproc->priv;
	oproc->rproc = rproc;
	init_completion(&oproc->pm_comp);
	oproc->suspend_timeout = pdata->suspend_timeout ? : DEF_SUSPEND_TIMEOUT;

	if (pdata->idle_addr) {
		oproc->idle = ioremap(pdata->idle_addr, sizeof(u32));
		if (!oproc->idle)
			goto free_rproc;
		oproc->idle_mask = pdata->idle_mask;
	}

	platform_set_drvdata(pdev, rproc);

	ret = rproc_add(rproc);
	if (ret)
		goto iounmap;

	return 0;
iounmap:
	if (oproc->idle)
		iounmap(oproc->idle);
free_rproc:
	rproc_put(rproc);
	return ret;
}

static int __devexit omap_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct omap_rproc *oproc = rproc->priv;

	if (oproc->idle)
		iounmap(oproc->idle);

	rproc_del(rproc);
	rproc_put(rproc);

	return 0;
}

static struct platform_driver omap_rproc_driver = {
	.probe = omap_rproc_probe,
	.remove = __devexit_p(omap_rproc_remove),
	.driver = {
		.name = "omap-rproc",
		.owner = THIS_MODULE,
	},
};

module_platform_driver(omap_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OMAP Remote Processor control driver");
