/*
 * Board support file for OMAP5430 based EVM.
 *
 * Copyright (C) 2010-2011 Texas Instruments
 * Author: Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * Based on mach-omap2/board-4430sdp.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/gpio.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include <mach/hardware.h>
#include "common.h"
#include <asm/hardware/gic.h>
#include <plat/common.h>
#include <plat/remoteproc.h>
#include "common-board-devices.h"
#include "mux.h"

#ifdef CONFIG_OMAP_MUX
static struct omap_board_mux board_mux[] __initdata = {
	{ .reg_offset = OMAP_MUX_TERMINATOR },
};
#else
#define board_mux NULL
#endif

#if defined(CONFIG_TI_EMIF) || defined(CONFIG_TI_EMIF_MODULE)
#ifndef CONFIG_MACH_OMAP_5430ZEBU
static struct __devinitdata emif_custom_configs custom_configs = {
	.mask	= EMIF_CUSTOM_CONFIG_LPMODE,
	.lpmode	= EMIF_LP_MODE_DISABLE
};
#endif
#endif

static void __init omap_5430evm_init(void)
{

#if defined(CONFIG_TI_EMIF) || defined(CONFIG_TI_EMIF_MODULE)
#ifndef CONFIG_MACH_OMAP_5430ZEBU
	omap_emif_set_device_details(1, &lpddr2_elpida_4G_S4_x2_info,
			lpddr2_elpida_4G_S4_timings,
			ARRAY_SIZE(lpddr2_elpida_4G_S4_timings),
			&lpddr2_elpida_S4_min_tck,
			&custom_configs);

	omap_emif_set_device_details(2, &lpddr2_elpida_4G_S4_x2_info,
			lpddr2_elpida_4G_S4_timings,
			ARRAY_SIZE(lpddr2_elpida_4G_S4_timings),
			&lpddr2_elpida_S4_min_tck,
			&custom_configs);
#endif
#endif
	omap5_mux_init(board_mux, NULL, OMAP_PACKAGE_CBL);
	omap_sdrc_init(NULL, NULL);
	omap_serial_init();
}

static void __init omap_5430evm_reserve(void)
{
	omap_rproc_reserve_cma(RPROC_CMA_OMAP5);
	omap_reserve();
}

MACHINE_START(OMAP5_SEVM, "OMAP5430 evm board")
	/* Maintainer: Santosh Shilimkar - Texas Instruments Inc */
	.atag_offset	= 0x100,
	.reserve	= omap_5430evm_reserve,
	.map_io		= omap5_map_io,
	.init_early	= omap_5430evm_init_early,
	.init_irq	= gic_init_irq,
	.handle_irq	= gic_handle_irq,
	.init_machine	= omap_5430evm_init,
	.timer		= &omap5_timer,
MACHINE_END
