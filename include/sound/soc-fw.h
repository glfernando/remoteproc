/*
 * linux/sound/soc-fw.h -- ALSA SoC Firmware Controls and DAPM
 *
 * Copyright:	2012 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Simple file API to load FW that includes mixers, coefficients, DAPM graphs,
 * algorithms, equalisers, DAIs, widgets etc.
 */

#ifndef __LINUX_SND_SOC_FW_H
#define __LINUX_SND_SOC_FW_H

#define SND_SOC_FW_MAGIC	0x41536F43 /* ASoC */
#define SND_SOC_FW_TEXT_SIZE	16
#define SND_SOC_FW_NUM_TEXTS	16

/* 
 * File andBlock header data types.
 * Add new generic and vendor types to end of list.
 * Generic types are handled by the core whilst vendors types are passed
 * to the component drivers for handling.
 */
#define SND_SOC_FW_MIXER		1
#define SND_SOC_FW_DAPM_GRAPH		2
#define SND_SOC_FW_DAPM_PINS		3
#define SND_SOC_FW_DAPM_WIDGET	4
#define SND_SOC_FW_DAI_LINK		5

#define SND_SOC_FW_VENDOR_FW		1000
#define SND_SOC_FW_VENDOR_CONFIG	1001
#define SND_SOC_FW_VENDOR_COEFF	1002
#define SND_SOC_FW_VENDOR_CODEC	1003

/*
 * File and Block Header
 */
struct snd_soc_fw_hdr {
	u32 magic;
	u32 type;
	u32 vendor_type; /* optional vendor specific type info */
	u32 version; /* optional vendor specific version details */
	size_t size; /* data bytes, excluding this header */
	/* file data contents start here */
};

/*
 * Mixer KControl Types.
 * Simple ones atm, add new ones to end.
 */
#define SND_SOC_FW_MIXER_SINGLE_VALUE		0
#define SND_SOC_FW_MIXER_DOUBLE_VALUE		1
#define SND_SOC_FW_MIXER_SINGLE_VALUE_EXT	2
#define SND_SOC_FW_MIXER_DOUBLE_VALUE_EXT	3

/* 
 * Mixer KControl.
 */
struct snd_soc_fw_mixer_control {
	char name[SND_SOC_FW_TEXT_SIZE];
	u32 type;
	s32 min;
	s32 max;
	s32 platform_max;
	u32 reg;
	u32 rreg;
	u32 shift;
	u32 rshift;
	u32 invert;
};

/*
 * Enum KControl Types.
 * Simple ones atm, add new ones to end.
 */
#define SND_SOC_FW_ENUM_SINGLE_T	0	/* text */
#define SND_SOC_FW_ENUM_DOUBLE_T	1
#define SND_SOC_FW_ENUM_SINGLE_T_EXT	2
#define SND_SOC_FW_ENUM_DOUBLE_T_EXT	3
#define SND_SOC_FW_ENUM_SINGLE_V	4	/* value */
#define SND_SOC_FW_ENUM_DOUBLE_V	5
#define SND_SOC_FW_ENUM_SINGLE_V_EXT	6
#define SND_SOC_FW_ENUM_DOUBLE_V_EXT	7

/* 
 * Enumerated KControl
 */
struct snd_soc_fw_enum_control {
	char name[SND_SOC_FW_TEXT_SIZE];
	u32 type;
	u32 reg;
	u32 reg2;
	u32 shift_l;
	u32 shift_r;
	u32 max;
	u32 mask;
	union {	/* both texts and values are the same size */
		char texts[SND_SOC_FW_NUM_TEXTS][SND_SOC_FW_TEXT_SIZE];
		u32 values[SND_SOC_FW_NUM_TEXTS * SND_SOC_FW_TEXT_SIZE / 4];
	};
};

/*
 * Kcontrol Header Types.
 */
#define SND_SOC_FW_MIXER_VALUE	0
#define SND_SOC_FW_MIXER_ENUM		1

/*
 * Kcontrol Header
 */
struct snd_soc_fw_kcontrol {
	u32 type;
	u32 count; /* in kcontrols (based on type) */
	/* kcontrols here */
};

/*
 * DAPM Graph Element
 */
struct snd_soc_fw_dapm_graph_elem {
	char sink[SND_SOC_FW_TEXT_SIZE];
	char control[SND_SOC_FW_TEXT_SIZE];
	char source[SND_SOC_FW_TEXT_SIZE];
};

/*
 * DAPM Pin Element.
 */
struct snd_soc_fw_dapm_pin_elem {
	char name[SND_SOC_FW_TEXT_SIZE];
	u32 disconnect:1;
	u32 ignore_suspend:1;
};


/*
 * DAPM Widget.
 */
struct snd_soc_fw_dapm_widget {
	u32 id;		/* snd_soc_dapm_type */
	char name[SND_SOC_FW_TEXT_SIZE];
	char sname[SND_SOC_FW_TEXT_SIZE];
	
	s32 reg;		/* negative reg = no direct dapm */
	u32 shift;		/* bits to shift */
	u32 mask;		/* non-shifted mask */
	u32 invert:1;		/* invert the power bit */
	u32 ignore_suspend:1;	/* kept enabled over suspend */

	/* kcontrols that relate to this widget */
	struct snd_soc_fw_kcontrol kcontrol;
	/* controls follow here */
};

/*
 * DAPM Graph and Pins.
 */
struct snd_soc_fw_dapm_elems {
	u32 count; /* in elements */
	/* elements here */
};



/* 
 * Public API - Used by component drivers to load new mixers, DAPM, vendor
 * specific data.
 */
struct snd_soc_fw_codec_ops {

	/* external kcontrol init - can be used to set ext funcs + pdata */
	int (*control_load) (struct snd_soc_codec *, struct snd_kcontrol_new *);

	/* external widget init - can be used to set ext funcs + pdata */
	int (*widget_load) (struct snd_soc_codec *, struct snd_soc_dapm_widget *);

	/* callback to handle vendor data */
	int (*vendor_load) (struct snd_soc_codec *, struct snd_soc_fw_hdr *);
	int (*vendor_unload) (struct snd_soc_codec *, struct snd_soc_fw_hdr *);
};

int snd_soc_fw_load_codec(struct snd_soc_codec *codec,
	struct snd_soc_fw_codec_ops *ops, const char *file);
int snd_soc_fw_unload_codec(struct snd_soc_codec *codec,
	struct snd_soc_fw_codec_ops *ops, const char *file);

struct snd_soc_fw_platform_ops {

	/* external kcontrol init - can be used to set ext funcs + pdata */
	int (*control_load) (struct snd_soc_platform *, struct snd_kcontrol_new *);

	/* external widget init - can be used to set ext funcs + pdata */
	int (*widget_load) (struct snd_soc_platform *, struct snd_soc_dapm_widget *);

	/* callback to handle vendor data */
	int (*vendor_load) (struct snd_soc_platform *, struct snd_soc_fw_hdr *);
	int (*vendor_unload) (struct snd_soc_platform *, struct snd_soc_fw_hdr *);
};

int snd_soc_fw_load_platform(struct snd_soc_platform *platform,
	struct snd_soc_fw_platform_ops *ops, const char *file);
int snd_soc_fw_unload_platform(struct snd_soc_platform *platform,
	struct snd_soc_fw_platform_ops *ops, const char *file);

struct snd_soc_fw_card_ops {

	/* external kcontrol init - can be used to set ext funcs + pdata */
	int (*control_load) (struct snd_soc_card *, struct snd_kcontrol_new *);

	/* external widget init - can be used to set ext funcs + pdata */
	int (*widget_load) (struct snd_soc_card *, struct snd_soc_dapm_widget *);

	/* callback to handle vendor data */
	int (*vendor_load) (struct snd_soc_card *, struct snd_soc_fw_hdr *);
	int (*vendor_unload) (struct snd_soc_card *, struct snd_soc_fw_hdr *);
};

int snd_soc_fw_load_card(struct snd_soc_card *card,
	struct snd_soc_fw_card_ops *ops, const char *file);
int snd_soc_fw_unload_card(struct snd_soc_card *card,
	struct snd_soc_fw_card_ops *ops, const char *file);

#endif
