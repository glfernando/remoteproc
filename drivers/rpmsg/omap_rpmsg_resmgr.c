/*
 * OMAP Remote processor resource manager resources
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
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg_resmgr.h>
#include <linux/pm_runtime.h>
#include <plat/dma.h>
#include <plat/dmtimer.h>
#include <plat/rpmsg_resmgr.h>
#include "omap_rpmsg_resmgr.h"

struct rprm_gpt_depot {
	struct rprm_gpt args;
	struct omap_rprm_gpt *ogpt;
	struct omap_dm_timer *gpt;
};

struct rprm_auxclk_depot {
	struct rprm_auxclk args;
	struct omap_rprm_auxclk *oauxclk;
	struct clk *clk;
	struct clk *old_pclk;
};

static int rprm_gptimer_request(struct rprm_manager *mgr, void **handle,
				void *data, size_t len)
{
	struct device *dev = mgr->dev;
	struct omap_rprm_pdata *pdata = dev->platform_data;
	struct omap_rprm_gpt *ogpt = NULL;
	struct rprm_gpt_depot *gptd;
	struct rprm_gpt *gpt = data;
	int ret, i;

	if (len != sizeof *gpt) {
		dev_err(dev, "invalid data size %u\n", len);
		return -EINVAL;
	}

	dev_dbg(dev, "requesting gpt id %d , source id %d\n",
						gpt->id, gpt->src_clk);
	/* check if it is a valid id */
	for (i = 0; i < pdata->gpt_cnt; i++) {
		if (pdata->gpts[i].id == gpt->id) {
			ogpt = &pdata->gpts[i];
			break;
		}
	}

	if (!ogpt) {
		dev_err(dev, "invalid gptimer id %d\n", gpt->id);
		return -EINVAL;
	}

	gptd = kmalloc(sizeof *gptd, GFP_KERNEL);
	if (!gptd)
		return -ENOMEM;

	gptd->ogpt = ogpt;
	gptd->gpt = omap_dm_timer_request_specific(ogpt->gptn);
	if (!gptd->gpt) {
		ret = -EBUSY;
		goto free_handle;
	}

	ret = omap_dm_timer_set_source(gptd->gpt, gpt->src_clk);
	if (ret) {
		dev_err(dev, "invalid source %i\n", gpt->src_clk);
		goto free_gpt;
	}

	omap_dm_timer_enable(gptd->gpt);
	memcpy(&gptd->args, gpt, sizeof *gpt);
	*handle = gptd;
	return 0;

free_gpt:
	omap_dm_timer_free(gptd->gpt);
free_handle:
	kfree(gptd);
	return ret;
}

static int rprm_gptimer_release(struct rprm_manager *mgr, void *handle)
{
	struct rprm_gpt_depot *gptd = handle;

	dev_dbg(mgr->dev, "releasing gpt id %d, source id %d\n",
		gptd->args.id, gptd->args.src_clk);

	omap_dm_timer_disable(gptd->gpt);
	omap_dm_timer_free(gptd->gpt);
	kfree(gptd);

	return 0;
}

static int rprm_gptimer_get_info(struct rprm_manager *mgr, void *handle,
				char *buf, size_t len)
{
	struct rprm_gpt_depot *gptd = handle;
	struct rprm_gpt *gpt = &gptd->args;
	struct omap_rprm_gpt *ogpt = gptd->ogpt;

	return snprintf(buf, len,
		"Id:%d\n"
		"Gptimer%d\n"
		"Source:%d\n",
		gpt->id, ogpt->gptn, gpt->src_clk);
}

static int rprm_auxclk_request(struct rprm_manager *mgr, void **handle,
				void *args, size_t len)
{
	struct device *dev = mgr->dev;
	struct omap_rprm_pdata *pdata = dev->platform_data;
	struct omap_rprm_auxclk *oauxclk = NULL;
	struct rprm_auxclk *auxclk = args;
	struct rprm_auxclk_depot *acd;
	struct clk *src;
	struct clk *parent;
	char const *name;
	char const *pname;
	int ret, i;

	if (len != sizeof *auxclk) {
		dev_err(dev, "invalid data size %u\n", len);
		return -EINVAL;
	}

	dev_dbg(dev, "requesting auxclk id %d , parent id %d\n",
			auxclk->clk_id, auxclk->pclk_id);

	for (i = 0; i < pdata->auxclk_cnt; i++) {
		if (pdata->auxclks[i].id == auxclk->clk_id) {
			oauxclk = &pdata->auxclks[i];
			break;
		}
	}

	if (!oauxclk) {
		dev_err(dev, "invalid auxclk id %d\n", auxclk->clk_id);
		return -EINVAL;
	}

	if (auxclk->pclk_id >= oauxclk->parents_cnt) {
		dev_err(dev, "invalid parent id %d for %s\n",
				auxclk->pclk_id, oauxclk->name);
		return -ENOENT;
	}

	name = oauxclk->name;
	pname = oauxclk->parents[auxclk->pclk_id];
	/* Create auxclks depot */
	acd = kmalloc(sizeof *acd, GFP_KERNEL);
	if (!acd)
		return -ENOMEM;

	acd->oauxclk = oauxclk;
	acd->clk = clk_get(dev, name);
	if (!acd->clk) {
		dev_err(dev, "unable to get clock %s\n", name);
		ret = -EIO;
		goto error;
	}
	/*
	 * The parent for an auxiliar clock is set to the auxclkX_ck_src
	 * clock which is the parent of auxclkX_ck
	 */
	src = clk_get_parent(acd->clk);
	if (!src) {
		dev_err(dev, "unable to get %s source clock\n", name);
		ret = -EIO;
		goto error_aux;
	}

	/* get clk requested by rproc to be used as parent */
	parent = clk_get(dev, pname);
	if (!parent) {
		dev_err(dev, "unable to get parent clock %s\n", pname);
		ret = -EIO;
		goto error_aux;
	}

	/* save old parent in order to restore at release time*/
	acd->old_pclk = clk_get_parent(src);
	ret = clk_set_parent(src, parent);
	if (ret) {
		dev_err(dev, "unable to set clk %s as parent of %s\n",
			 pname, name);
		goto error_aux_parent;
	}

	ret = clk_set_rate(parent, auxclk->pclk_rate);
	if (ret) {
		dev_err(dev, "rate %u not supported by %s\n",
			auxclk->pclk_rate, pname);
		goto error_set_parent;
	}

	ret = clk_set_rate(acd->clk, auxclk->clk_rate);
	if (ret) {
		dev_err(dev, "rate %u not supported by %s\n",
			auxclk->clk_rate, name);
		goto error_set_parent;
	}

	ret = clk_enable(acd->clk);
	if (ret) {
		dev_err(dev, "error enabling %s\n", name);
		goto error_set_parent;
	}
	clk_put(parent);

	memcpy(&acd->args, auxclk, sizeof *auxclk);

	*handle = acd;

	return 0;
error_set_parent:
	clk_set_parent(src, acd->old_pclk);
error_aux_parent:
	clk_put(parent);
error_aux:
	clk_put(acd->clk);
error:
	kfree(acd);

	return ret;
}

static int rprm_auxclk_release(struct rprm_manager *mgr, void *handle)
{
	struct rprm_auxclk_depot *acd = handle;

	dev_dbg(mgr->dev, "releasing auxclk id %d , parent id %d\n",
			acd->args.clk_id, acd->args.pclk_id);

	clk_set_parent(clk_get_parent(acd->clk), acd->old_pclk);
	clk_disable(acd->clk);
	clk_put(acd->clk);

	kfree(acd);
	return 0;
}

static int rprm_auxclk_get_info(struct rprm_manager *mgr, void *handle,
				char *buf, size_t len)
{
	struct rprm_auxclk_depot *acd = handle;
	struct rprm_auxclk *auxclk = &acd->args;
	struct omap_rprm_auxclk *oauxclk = acd->oauxclk;

	return snprintf(buf, len,
		"id:%d\n"
		"name:%s\n"
		"rate:%u\n"
		"parent id:%d\n"
		"parent name:%s\n"
		"parent rate:%u\n",
		auxclk->clk_id, oauxclk->name, auxclk->clk_rate,
		auxclk->clk_id, oauxclk->parents[auxclk->clk_id],
		auxclk->pclk_rate);
	return 0;
}

static int rprm_sdma_request(struct rprm_manager *mgr, void **handle,
				void *data, size_t len)
{
	struct device *dev = mgr->dev;
	struct rprm_sdma *sd;
	struct rprm_sdma *sdma = data;
	int ret, ch, i;

	if (len != sizeof *sdma) {
		dev_err(dev, "invalid data size %u\n", len);
		return -EINVAL;
	}

	dev_dbg(dev, "requesting %d sdma channels\n", sdma->num_chs);

	if (sdma->num_chs > MAX_NUM_SDMA_CHANNELS) {
		dev_err(dev, "not able to provide %u channels\n",
			sdma->num_chs);
		return -EINVAL;
	}

	/* Create sdma depot */
	sd = kmalloc(sizeof *sd, GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	for (i = 0; i < sdma->num_chs; i++) {
		ret = omap_request_dma(0, dev_name(dev), NULL, NULL, &ch);
		if (ret) {
			dev_err(dev, "error %d providing sdma channel %d\n",
				ret, ch);
			goto err;
		}

		sdma->channels[i] = ch;
		dev_dbg(dev, "providing sdma ch %d\n", ch);
	}

	*handle = memcpy(sd, sdma, sizeof *sdma);

	return 0;
err:
	while (i--)
		omap_free_dma(sdma->channels[i]);
	kfree(sd);
	return ret;
}

static int rprm_sdma_release(struct rprm_manager *mgr, void *handle)
{
	struct device *dev = mgr->dev;
	struct rprm_sdma *sd = handle;
	int i = sd->num_chs;

	while (i--) {
		omap_free_dma(sd->channels[i]);
		dev_dbg(dev, "releasing sdma ch %d\n", sd->channels[i]);
	}
	kfree(sd);

	return 0;
}

static int rprm_sdma_get_info(struct rprm_manager *mgr, void *handle,
				char *buf, size_t len)
{
	struct rprm_sdma *sd = handle;
	int i, ret = 0;

	ret += snprintf(buf, len, "NumChannels:%d\n", sd->num_chs);
	for (i = 0 ; i < sd->num_chs; i++)
		ret += snprintf(buf + ret, len - ret, "Channel[%d]:%d\n", i,
							sd->channels[i]);
	return ret;
}

static struct rprm_res omap_rprm_resources[] = {
	{
		.name = "omap-gptimer",
		.request = rprm_gptimer_request,
		.release = rprm_gptimer_release,
		.get_info = rprm_gptimer_get_info,
	},
	{
		.name = "omap-auxclk",
		.request = rprm_auxclk_request,
		.release = rprm_auxclk_release,
		.get_info = rprm_auxclk_get_info,
	},
	{
		.name = "omap-sdma",
		.request = rprm_sdma_request,
		.release = rprm_sdma_release,
		.get_info = rprm_sdma_get_info,
	},
};

static int omap_rprm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct omap_rprm_pdata *pdata = dev->platform_data;
	struct rprm_manager *mgr;
	int ret;

	dev_dbg(dev, "probing omap resmngr %s\n", pdata->mgr_name);

	mgr = kmalloc(sizeof *mgr, GFP_KERNEL);
	if (!mgr) {
		dev_err(dev, "failed to allocate manager memory\n");
		return -ENOMEM;
	}

	mgr->dev = dev;
	mgr->owner = THIS_MODULE;
	mgr->name = pdata->mgr_name;
	mgr->resources = omap_rprm_resources;
	mgr->res_cnt = ARRAY_SIZE(omap_rprm_resources);
	platform_set_drvdata(pdev, mgr);

	ret = rprm_manager_register(mgr);
	if (ret)
		goto err;

	return 0;
err:
	kfree(mgr);
	return ret;
}

static int omap_rprm_remove(struct platform_device *pdev)
{
	struct rprm_manager *mgr = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret;

	dev_dbg(dev, "removing omap resmngr %s\n", mgr->name);

	ret = rprm_manager_unregister(mgr);
	if (ret)
		return ret;

	kfree(mgr);
	return 0;
}

static struct platform_driver omap_rprm_driver = {
	.probe = omap_rprm_probe,
	.remove = __devexit_p(omap_rprm_remove),
	.driver = {
		.name = "omap-rprm",
		.owner = THIS_MODULE,
	},
};

module_platform_driver(omap_rprm_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Remote Processor Resource Manager OMAP resources");
