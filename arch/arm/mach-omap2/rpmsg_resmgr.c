/*
 * Remote processor Resource Manager  machine-specific module for OMAP4
 *
 * Copyright (C) 2012 Texas Instruments, Inc.
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

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include <plat/rpmsg_resmgr.h>

static struct omap_rprm_gpt omap4_ducati_gpts[] = {
	{
		.id = 3,
		.gptn = 3,
	},
	{
		.id = 4,
		.gptn = 4,
	},
	{
		.id = 9,
		.gptn = 9,
	},
	{
		.id = 11,
		.gptn = 11,
	},
};

static const char * const omap4_pauxclks[] = {
	"sys_clkin_ck",
	"dpll_core_m3x2_ck",
	"dpll_per_m3x2_ck",
};

static struct omap_rprm_auxclk omap4_auxclks[] = {
	{
		.id = 0,
		.name = "auxclk0_ck",
		.parents = omap4_pauxclks,
		.parents_cnt = ARRAY_SIZE(omap4_pauxclks),
	},
	{
		.id = 1,
		.name = "auxclk1_ck",
		.parents = omap4_pauxclks,
		.parents_cnt = ARRAY_SIZE(omap4_pauxclks),
	},
	{
		.id = 2,
		.name = "auxclk2_ck",
		.parents = omap4_pauxclks,
		.parents_cnt = ARRAY_SIZE(omap4_pauxclks),
	},
	{
		.id = 3,
		.name = "auxclk3_ck",
		.parents = omap4_pauxclks,
		.parents_cnt = ARRAY_SIZE(omap4_pauxclks),
	},
};

static struct omap_rprm_pdata omap2_rprm_ducati_pdata = {
	.mgr_name = "rprm-ducati",
	.port = 100,
	.gpts = omap4_ducati_gpts,
	.gpt_cnt = ARRAY_SIZE(omap4_ducati_gpts),
	.auxclks = omap4_auxclks,
	.auxclk_cnt = ARRAY_SIZE(omap4_auxclks),
};

static struct platform_device omap2_rprm_ducati = {
	.name	= "omap-rprm",
	.id	= 0,
	.dev	= {
		.platform_data = &omap2_rprm_ducati_pdata,
	},
};

static struct platform_device *omap2_rprm_devices[] = {
	&omap2_rprm_ducati,
};

static int __init omap2_rprm_init(void)
{
	return platform_add_devices(omap2_rprm_devices,
				ARRAY_SIZE(omap2_rprm_devices));
}

device_initcall(omap2_rprm_init);
