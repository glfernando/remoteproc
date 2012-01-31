/*
 * Remote Processor Resource Manager - OMAP specific
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

#ifndef _PLAT_RPMSG_RESMGR_H
#define _PLAT_RPMSG_RESMGR_H
#include <linux/device.h>

/*
 * struct omap_rprm_gpt - omap resmgr gptimer data
 * @id		id used by rproc to request a specific gptimer
 * @gptn	gptimer number to be requested
 */
struct omap_rprm_gpt {
	int id;
	unsigned gptn;
};

/*
 * struct omap_rprm_auxclk - omap resmgr auxclk data
 * @id		id used by rproc to request a specific auxlck
 * @name	name of the auxiliary clock
 * @parents	array of possible parents names for the auxclk
 * @parents_cnt number of possible parents
 */
struct omap_rprm_auxclk {
	int id;
	const char *name;
	const char * const *parents;
	u32 parents_cnt;
};

/*
 * struct omap_rprm_regulator - omap resmgr regulator data
 * @id		id used by rproc to request a specific regulator
 * @name:	name of the regulator
 * @fixed:	true if the voltage is fixed and therefore is not programmable
 */
struct omap_rprm_regulator {
	int id;
	const char *name;
	bool fixed;
};

/*
 * struct omap_rprm_pdata - omap resmgr platform data
 * @mgr_name:	name of resource manager
 * @port:	port for the rpmsg channel for the manager server
 * @gpts:	array of valid gptimer resources
 * @gpt_cnt:	number of gptimers
 * @auxclks:	array of valid auxclks resources
 * @auxclk_cnt: number of auxclks
 * @regs:	array of valid regulator resources
 * @reg_cnt:	number of regulators
 *
 * This structure contains all information related to the valid resources
 * for a specific omap resource manager. Different omap managers can have
 * different valid resources of the same type (gptimers for example). So,
 * one manager can allow to request gptimer 3 but not gptimer 4 and another
 * manager can request gptimer 4 but not gptimer 3.
 */
struct omap_rprm_pdata {
	const char *mgr_name;
	int port;
	struct omap_rprm_gpt *gpts;
	unsigned gpt_cnt;
	struct omap_rprm_auxclk *auxclks;
	unsigned auxclk_cnt;
	struct omap_rprm_regulator *regs;
	unsigned reg_cnt;
};

#endif /* _PLAT_RPMSG_RESMGR_H */
