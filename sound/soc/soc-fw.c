/*
 * soc-fw.c  --  ALSA SoC Firmware
 *
 * Copyright (C) 2012 Texas Instruments Inc.
 *
 * Author: Liam Girdwood <lrg@ti.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  Support for audio fimrware to contain kcontrols, DAPM graphs, widgets,
 *  DAIs, equalizers, firmware, coefficienst etc.
 *
 *  This file only manages the DAPM and Kcontrol components, all other firmware
 *  data is passed to component drivers for bespoke handling.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/soc-fw.h>

/*
 * We make several passes over the data (since it wont necessarily be ordered)
 * and process objects in the following order. This guarantees the component
 * drivers will be ready with any vendor data before the mixers and DAPM objects
 * are loaded (that may make use of the vemndor data).
 */
#define SOC_FW_PASS_VENDOR	0
#define SOC_FW_PASS_MIXER	1
#define SOC_FW_PASS_WIDGET	2
#define SOC_FW_PASS_GRAPH	3
#define SOC_FW_PASS_PINS	4

#define SOC_FW_PASS_START	SOC_FW_PASS_VENDOR
#define SOC_FW_PASS_END	SOC_FW_PASS_PINS

struct soc_fw {
	const char *file;
	const struct firmware *fw;
	char *pos;
	unsigned int pass;

	struct device *dev;
	struct snd_soc_codec *codec;
	struct snd_soc_platform *platform;
	struct snd_soc_card *card;

	union {
		struct snd_soc_fw_codec_ops *codec_ops;
		struct snd_soc_fw_platform_ops *platform_ops;
		struct snd_soc_fw_card_ops *card_ops;
	};
};

static inline void soc_fw_list_add_enum(struct soc_fw *sfw, struct soc_enum *se)
{
	if (sfw->codec)
		list_add(&se->list, &sfw->codec->denums);
	else if (sfw->platform)
		list_add(&se->list, &sfw->platform->denums);
	else if (sfw->card)
		list_add(&se->list, &sfw->card->denums);
}

static inline void soc_fw_list_add_mixer(struct soc_fw *sfw,
	struct soc_mixer_control *mc)
{
	if (sfw->codec)
		list_add(&mc->list, &sfw->codec->dmixers);
	else if (sfw->platform)
		list_add(&mc->list, &sfw->platform->dmixers);
	else if (sfw->card)
		list_add(&mc->list, &sfw->card->dmixers);
}

static inline struct snd_soc_dapm_context *soc_fw_dapm_get(struct soc_fw *sfw)
{
	if (sfw->codec)
		return &sfw->codec->dapm;
	else if (sfw->platform)
		return &sfw->platform->dapm;
	else if (sfw->card)
		return &sfw->card->dapm;
	BUG();
}

static int soc_fw_load_data(struct soc_fw *sfw)
{
	int ret;

	ret = request_firmware(&sfw->fw, sfw->file, sfw->dev);
	if (ret != 0)
		dev_err(sfw->dev, "Failed to load : %s %d\n", sfw->file, ret);

	return ret;
}

static inline void soc_fw_release_data(struct soc_fw *sfw)
{
	release_firmware(sfw->fw);
}

/* check we dont overflow the data for this chunk */
static inline int soc_fw_check_count(struct soc_fw *sfw, size_t elem_size,
	unsigned int count, size_t bytes)
{
	const u8 *end = sfw->pos + elem_size * count;

	if (end > sfw->fw->data + sfw->fw->size) {
		dev_err(sfw->dev, "controls overflow end of data\n");
		return -EINVAL;
	}

	if (elem_size * count != bytes) {
		dev_err(sfw->dev, "controls do not match size\n");
		return -EINVAL;
	}

	return 0;
}

static inline int soc_fw_eof(struct soc_fw *sfw, size_t bytes)
{
	const u8 *end = sfw->pos + bytes;

	if (end >= sfw->fw->data + sfw->fw->size)
		return 1;
	return 0;
}

/* pass vendor data to component driver for processing */
static int soc_fw_vendor_load(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	if (sfw->codec && sfw->codec_ops && sfw->codec_ops->vendor_load)
		return sfw->codec_ops->vendor_load(sfw->codec, hdr);

	if (sfw->platform && sfw->platform_ops && sfw->platform_ops->vendor_load)
		return sfw->platform_ops->vendor_load(sfw->platform, hdr);

	if (sfw->card && sfw->card_ops && sfw->card_ops->vendor_load)
		return sfw->card_ops->vendor_load(sfw->card, hdr);

	dev_info(sfw->dev, "no load handler specified for vendor %d:%d\n",
		hdr->type, hdr->vendor_type);
	return 0;
}

static int soc_fw_vendor_unload(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	if (sfw->codec && sfw->codec_ops && sfw->codec_ops->vendor_unload)
		return sfw->codec_ops->vendor_unload(sfw->codec, hdr);

	if (sfw->platform && sfw->platform_ops && sfw->platform_ops->vendor_unload)
		return sfw->platform_ops->vendor_unload(sfw->platform, hdr);

	if (sfw->card && sfw->card_ops && sfw->card_ops->vendor_unload)
		return sfw->card_ops->vendor_unload(sfw->card, hdr);

	dev_info(sfw->dev, "no unload handler specified for vendor %d:%d\n",
		hdr->type, hdr->vendor_type);
	return 0;
}

/* pass new dynamic widget to component driver. mainly for external widgets */
static int soc_fw_widget_load(struct soc_fw *sfw, struct snd_soc_dapm_widget *w)
{
	if (sfw->codec && sfw->codec_ops && sfw->codec_ops->widget_load)
		return sfw->codec_ops->widget_load(sfw->codec, w);

	if (sfw->platform && sfw->platform_ops && sfw->platform_ops->widget_load)
		return sfw->platform_ops->widget_load(sfw->platform, w);

	if (sfw->card && sfw->card_ops && sfw->card_ops->widget_load)
		return sfw->card_ops->widget_load(sfw->card, w);

	dev_info(sfw->dev, "no handler specified for ext widget %s\n", w->name);
	return 0;
}

/* add a dynamic kcontrol */
static int soc_fw_add_dcontrol(struct snd_card *card, struct device *dev,
	const struct snd_kcontrol_new *control_new, const char *prefix,
	void *data, struct snd_kcontrol **kcontrol)
{
	int err;

	*kcontrol = snd_soc_cnew(control_new, data, control_new->name, prefix);
	if (*kcontrol)
		return -ENOMEM;

	err = snd_ctl_add(card, *kcontrol);
	if (err < 0) {
		kfree(*kcontrol);
		dev_err(dev, "Failed to add %s: %d\n", control_new->name, err);
		return err;
	}

	return 0;
}

/* add a dynamic kcontrol for component driver */
static int soc_fw_add_kcontrol(struct soc_fw *sfw, struct snd_kcontrol_new *k,
	struct snd_kcontrol **kcontrol)
{
	if (sfw->codec) {
		struct snd_soc_codec *codec = sfw->codec;

		return soc_fw_add_dcontrol(codec->card->snd_card, codec->dev,
				k, codec->name_prefix, codec, kcontrol);
	} else if (sfw->platform) {
		struct snd_soc_platform *platform = sfw->platform;

		return soc_fw_add_dcontrol(platform->card->snd_card,
				platform->dev, k, NULL, platform, kcontrol);
	} else if (sfw->card) {
		struct snd_soc_card *card = sfw->card;

		return soc_fw_add_dcontrol(card->snd_card, card->dev,
				k, NULL, card, kcontrol);
	} else
		dev_info(sfw->dev, "no handler specified for kcontrol %s\n",
			k->name);
	return 0;
}

/* pass new dynamic kcontrol to component driver. mainly for external kcontrols */
static int soc_fw_init_kcontrol(struct soc_fw *sfw, struct snd_kcontrol_new *k)
{
	if (sfw->codec && sfw->codec_ops && sfw->codec_ops->control_load)
		return sfw->codec_ops->control_load(sfw->codec, k);

	if (sfw->platform && sfw->platform_ops && sfw->platform_ops->control_load)
		return sfw->platform_ops->control_load(sfw->platform, k);

	if (sfw->card && sfw->card_ops && sfw->card_ops->control_load)
		return sfw->card_ops->control_load(sfw->card, k);

	dev_info(sfw->dev, "no handler specified for kcontrol %s\n", k->name);
	return 0;
}

static void soc_fw_dmixer_codec_remove(struct soc_fw *sfw, const char *name)
{
	struct snd_soc_codec *codec = sfw->codec;
	struct soc_mixer_control *sm, *next_sm;
	struct snd_card *card = codec->card->snd_card;

	list_for_each_entry_safe(sm, next_sm, &codec->dmixers, list) {

		/* if name is not NULL then remove matching kcontrols */
		if (name && strcmp(name, sm->dcontrol->id.name))
			continue;

		snd_ctl_remove(card, sm->dcontrol);
		list_del(&sm->list);
		kfree(sm);
	}
}

static void soc_fw_dmixer_platform_remove(struct soc_fw *sfw, const char *name)
{
	struct snd_soc_platform *platform = sfw->platform;
	struct soc_mixer_control *sm, *next_sm;
	struct snd_card *card = platform->card->snd_card;

	list_for_each_entry_safe(sm, next_sm, &platform->dmixers, list) {

		/* if name is not NULL then remove matching kcontrols */
		if (name && strcmp(name, sm->dcontrol->id.name))
			continue;

		snd_ctl_remove(card, sm->dcontrol);
		list_del(&sm->list);
		kfree(sm);
	}
}

static void soc_fw_dmixer_card_remove(struct soc_fw *sfw, const char *name)
{
	struct snd_soc_card *soc_card = sfw->card;
	struct soc_mixer_control *sm, *next_sm;
	struct snd_card *card = soc_card->snd_card;

	list_for_each_entry_safe(sm, next_sm, &soc_card->dmixers, list) {

		/* if name is not NULL then remove matching kcontrols */
		if (name && strcmp(name, sm->dcontrol->id.name))
			continue;

		snd_ctl_remove(card, sm->dcontrol);
		list_del(&sm->list);
		kfree(sm);
	}
}

static void soc_fw_dmixer_component_remove(struct soc_fw *sfw, const char *name)
{
	if (sfw->codec)
		soc_fw_dmixer_codec_remove(sfw, name);
	else if (sfw->platform)
		soc_fw_dmixer_platform_remove(sfw, name);
	else if (sfw->card)
		soc_fw_dmixer_card_remove(sfw, name);
}

static int soc_fw_dmixer_remove(struct soc_fw *sfw, unsigned int count,
	size_t size)
{
	struct snd_soc_fw_mixer_control *mc;
	int i;

	if (soc_fw_check_count(sfw,
		sizeof(struct snd_soc_fw_mixer_control), count, size)) {
		dev_err(sfw->dev, "invalid count %d for mixer controls\n", count);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		mc = (struct snd_soc_fw_mixer_control*)sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_enum_control);

		soc_fw_dmixer_component_remove(sfw, mc->name);
	}
	return 0;
}

static int soc_fw_dmixer_create(struct soc_fw *sfw, unsigned int count,
	size_t size)
{
	struct snd_soc_fw_mixer_control *mc;
	struct soc_mixer_control *sm;
	struct snd_kcontrol_new kc;
	int ret, i;

	if (soc_fw_check_count(sfw,
		sizeof(struct snd_soc_fw_mixer_control), count, size)) {
		dev_err(sfw->dev, "invalid count %d for controls\n", count);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		mc = (struct snd_soc_fw_mixer_control*)sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_mixer_control);

		/* validate kcontrol */
		if (strnlen(mc->name, SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE)
			return -EINVAL;

		sm = kzalloc(sizeof(*sm), GFP_KERNEL);
		if (!sm)
			return -ENOMEM;

		kc.name = mc->name;
		kc.private_value = (long)sm;
		kc.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		sm->reg = mc->reg;
		sm->rreg = mc->rreg;
		sm->shift = mc->shift;
		sm->rshift = mc->rshift;
		sm->max = mc->max;
		sm->min = mc->min;
		sm->invert = mc->invert;
		sm->platform_max = mc->platform_max;
		INIT_LIST_HEAD(&sm->list);

		switch (mc->type) {
		case SND_SOC_FW_MIXER_SINGLE_VALUE:
		case SND_SOC_FW_MIXER_DOUBLE_VALUE:
			kc.get = snd_soc_get_volsw;
			kc.put = snd_soc_put_volsw;
			kc.info = snd_soc_info_volsw;
			break;
		case SND_SOC_FW_ENUM_SINGLE_T_EXT:
		case SND_SOC_FW_ENUM_DOUBLE_T_EXT:
			/* set default values - component driver can override */
			kc.get = snd_soc_get_volsw;
			kc.put = snd_soc_put_volsw;
			kc.info = snd_soc_info_volsw;

			ret = soc_fw_init_kcontrol(sfw, &kc);
			if (ret < 0)
				goto err;

			break;
		}
		/* register control here */
		ret = soc_fw_add_kcontrol(sfw, &kc, &sm->dcontrol);
		if (ret < 0)
			goto err;
		soc_fw_list_add_mixer(sfw, sm);
	}

	return 0;

err:
	kfree(sm);
	/* remove other enum controls */
	sfw->pos -= sizeof(struct snd_soc_fw_mixer_control) * (i + 1);
	soc_fw_dmixer_remove(sfw, count, size);
	return ret;
}

static inline void soc_fw_denum_free_data(struct soc_enum *se)
{
	int i;

	if (se->dvalues)
		kfree(se->dvalues);
	else {
		for (i = 0; i < se->max - 1; i++)
			kfree(se->dtexts[i]);
	}
}

static void soc_fw_denum_codec_remove(struct soc_fw *sfw, const char *name)
{
	struct snd_soc_codec *codec = sfw->codec;
	struct soc_enum *se, *next_se;
	struct snd_card *card = codec->card->snd_card;

	list_for_each_entry_safe(se, next_se, &codec->denums, list) {

		/* if name is not NULL then remove matching kcontrols */
		if (name && strcmp(name, se->dcontrol->id.name))
			continue;

		snd_ctl_remove(card, se->dcontrol);
		list_del(&se->list);
		soc_fw_denum_free_data(se);
		kfree(se);
	}
}

static void soc_fw_denum_platform_remove(struct soc_fw *sfw, const char *name)
{
	struct snd_soc_platform *platform = sfw->platform;
	struct soc_enum *se, *next_se;
	struct snd_card *card = platform->card->snd_card;

	list_for_each_entry_safe(se, next_se, &platform->denums, list) {

		/* if name is not NULL then remove matching kcontrols */
		if (name && strcmp(name, se->dcontrol->id.name))
			continue;

		snd_ctl_remove(card, se->dcontrol);
		list_del(&se->list);
		soc_fw_denum_free_data(se);
		kfree(se);
	}
}

static void soc_fw_denum_card_remove(struct soc_fw *sfw, const char *name)
{
	struct snd_soc_card *soc_card = sfw->card;
	struct soc_enum *se, *next_se;
	struct snd_card *card = soc_card->snd_card;

	list_for_each_entry_safe(se, next_se, &soc_card->denums, list) {

		/* if name is not NULL then remove matching kcontrols */
		if (name && strcmp(name, se->dcontrol->id.name))
			continue;

		snd_ctl_remove(card, se->dcontrol);
		list_del(&se->list);
		soc_fw_denum_free_data(se);
		kfree(se);
	}
}

static void soc_fw_denum_component_remove(struct soc_fw *sfw, const char *name)
{
	if (sfw->codec)
		soc_fw_denum_codec_remove(sfw, name);
	else if (sfw->platform)
		soc_fw_denum_platform_remove(sfw, name);
	else if (sfw->card)
		soc_fw_denum_card_remove(sfw, name);
}

static int soc_fw_denum_remove(struct soc_fw *sfw, unsigned int count,
	size_t size)
{
	struct snd_soc_fw_enum_control *ec;
	int i;

	if (soc_fw_check_count(sfw,
		sizeof(struct snd_soc_fw_enum_control), count, size)) {
		dev_err(sfw->dev, "invalid count %d for enum controls\n", count);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		ec = (struct snd_soc_fw_enum_control*)sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_enum_control);

		soc_fw_denum_component_remove(sfw, ec->name);
	}
	return 0;
}

static int soc_fw_denum_create_texts(struct soc_enum *se,
	struct snd_soc_fw_enum_control *ec)
{
	int i, ret;

	for (i = 0; i < ec->max - 1; i++) {

		if (strnlen(ec->texts[i], SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE) {
			ret = -EINVAL;
			goto err;
		}

		se->dtexts[i] = kstrdup(ec->texts[i], GFP_KERNEL);
		if (!se->dtexts[i]) {
			ret = -ENOMEM;
			goto err;
		}
	}
	return 0;

err:
	for (--i; i >= 0; i--)
		kfree(se->dtexts[i]);
	return ret;
}

static int soc_fw_denum_create_values(struct soc_enum *se,
	struct snd_soc_fw_enum_control *ec)
{
	if (ec->max > sizeof(*ec->values))
		return -EINVAL;

	se->dvalues = kmalloc(ec->max * sizeof(u32), GFP_KERNEL);
	if (!se->dvalues)
		return -ENOMEM;

	memcpy(se->dvalues, ec->values, ec->max * sizeof(u32));
	return 0;
}

static int soc_fw_denum_create(struct soc_fw *sfw, unsigned int count,
	size_t size)
{
	struct snd_soc_fw_enum_control *ec;
	struct soc_enum *se;
	struct snd_kcontrol_new kc;
	int i, ret;

	if (soc_fw_check_count(sfw,
		sizeof(struct snd_soc_fw_enum_control), count, size)) {
		dev_err(sfw->dev, "invalid count %d for enum controls\n", count);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		ec = (struct snd_soc_fw_enum_control*)sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_enum_control);

		/* validate kcontrol */
		if (strnlen(ec->name, SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE)
			return -EINVAL;

		se = kzalloc(sizeof(*se), GFP_KERNEL);
		if (!se)
			return -ENOMEM;

		kc.name = ec->name;
		kc.private_value = (long)se;
		kc.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		se->reg = ec->reg;
		se->reg2 = ec->reg2;
		se->shift_l = ec->shift_l;
		se->shift_r = ec->shift_r;
		se->max = ec->max;
		se->mask = ec->mask;
		INIT_LIST_HEAD(&se->list);

		switch (ec->type) {
		case SND_SOC_FW_ENUM_SINGLE_T:
		case SND_SOC_FW_ENUM_DOUBLE_T:
			kc.get = snd_soc_get_enum_double;
			kc.put = snd_soc_put_enum_double;
			kc.info = snd_soc_info_enum_double;

			ret = soc_fw_denum_create_texts(se, ec);
			if (ret < 0)
				goto err;

			break;
		case SND_SOC_FW_ENUM_SINGLE_T_EXT:
		case SND_SOC_FW_ENUM_DOUBLE_T_EXT:
			/* set default values - component driver can override */
			kc.get = snd_soc_get_enum_double;
			kc.put = snd_soc_put_enum_double;
			kc.info = snd_soc_info_enum_double;

			ret = soc_fw_denum_create_texts(se, ec);
			if (ret < 0)
				goto err;

			ret = soc_fw_init_kcontrol(sfw, &kc);
			if (ret < 0)
				goto err;

			break;
		case SND_SOC_FW_ENUM_SINGLE_V:
		case SND_SOC_FW_ENUM_DOUBLE_V:
			kc.get = snd_soc_get_value_enum_double;
			kc.put = snd_soc_put_value_enum_double;
			kc.info = snd_soc_info_enum_double;

			ret = soc_fw_denum_create_values(se, ec);
			if (ret < 0)
				goto err;

			break;
		case SND_SOC_FW_ENUM_SINGLE_V_EXT:
		case SND_SOC_FW_ENUM_DOUBLE_V_EXT:
			/* set default values - component driver can override */
			kc.get = snd_soc_get_value_enum_double;
			kc.put = snd_soc_put_value_enum_double;
			kc.info = snd_soc_info_enum_double;

			ret = soc_fw_denum_create_values(se, ec);
			if (ret < 0)
				goto err;

			ret = soc_fw_init_kcontrol(sfw, &kc);
			if (ret < 0)
				goto err;

			break;
		}
		/* register control here */
		ret = soc_fw_add_kcontrol(sfw, &kc, &se->dcontrol);
		if (ret < 0)
			goto err;
		soc_fw_list_add_enum(sfw, se);
	}

	return 0;

err:
	/* free texts */
	if (se->dvalues)
		kfree(se->dvalues);
	else {
		for (i = 0; i < ec->max; i++)
			kfree(se->dtexts[i]);
	}
	kfree(se);

	/* remove other enum controls */
	sfw->pos -= sizeof(struct snd_soc_fw_enum_control) * (i + 1);
	soc_fw_denum_remove(sfw, count, size);
	return ret;
}

static int soc_fw_kcontrol_load(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	struct snd_soc_fw_kcontrol *sfwk =
		(struct snd_soc_fw_kcontrol*)sfw->pos;

	if (sfw->pass != SOC_FW_PASS_MIXER) {
		sfw->pos += sizeof(struct snd_soc_fw_kcontrol) + hdr->size;
		return 0;
	}

	sfw->pos += sizeof(struct snd_soc_fw_kcontrol);

	switch (sfwk->type) {
	case SND_SOC_FW_MIXER_VALUE:
		return soc_fw_dmixer_create(sfw, sfwk->count, hdr->size);
	case SND_SOC_FW_MIXER_ENUM:
		return soc_fw_denum_create(sfw, sfwk->count, hdr->size);
	default:
		dev_err(sfw->dev, "invalid control type %d count %d\n",
			sfwk->type, sfwk->count);
		return -EINVAL;
	}
}

static int soc_fw_kcontrol_unload(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	struct snd_soc_fw_kcontrol *sfwk =
		(struct snd_soc_fw_kcontrol*)sfw->pos;

	if (sfw->pass != SOC_FW_PASS_MIXER) {
		sfw->pos += sizeof(struct snd_soc_fw_kcontrol) + hdr->size;
		return 0;
	}

	sfw->pos += sizeof(struct snd_soc_fw_kcontrol);

	switch (sfwk->type) {
	case SND_SOC_FW_MIXER_VALUE:
		return soc_fw_dmixer_remove(sfw, sfwk->count, hdr->size);
	case SND_SOC_FW_MIXER_ENUM:
		return soc_fw_denum_remove(sfw, sfwk->count, hdr->size);
	default:
		dev_err(sfw->dev, "invalid control type %d count %d\n",
			sfwk->type, sfwk->count);
		return -EINVAL;
	}
}

static int soc_fw_dapm_graph_load(struct soc_fw *sfw,
	struct snd_soc_fw_hdr *hdr)
{
	struct snd_soc_dapm_context *dapm = soc_fw_dapm_get(sfw);
	struct snd_soc_dapm_route route;
	struct snd_soc_fw_dapm_elems *elem_info =
		(struct snd_soc_fw_dapm_elems*)sfw->pos;
	struct snd_soc_fw_dapm_graph_elem *elem;
	int ret, count = elem_info->count, i;

	if (sfw->pass != SOC_FW_PASS_GRAPH) {
		sfw->pos += sizeof(struct snd_soc_fw_dapm_elems) + hdr->size;
		return 0;
	}

	sfw->pos += sizeof(struct snd_soc_fw_dapm_elems);

	if (soc_fw_check_count(sfw,
		sizeof(struct snd_soc_fw_dapm_graph_elem), count, hdr->size)) {
		dev_err(sfw->dev, "invalid count %d for controls\n", count);
		return -EINVAL;
	}

	/* tear down exsiting widgets and graph for this context */
	soc_dapm_free_widgets(dapm);

	for (i = 0; i < count; i++) {
		elem = (struct snd_soc_fw_dapm_graph_elem *)sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_dapm_graph_elem);

		/* validate routes */
		if (strnlen(elem->source, SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE)
			return -EINVAL;
		if (strnlen(elem->sink, SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE)
			return -EINVAL;
		if (strnlen(elem->control, SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE)
			return -EINVAL;

		route.source = elem->source;
		route.sink = elem->sink;
		route.control = elem->control;

		ret = snd_soc_dapm_add_routes(dapm, &route, 1);
		if (ret < 0) {
			dev_err(sfw->dev, "failed to add DAPM route\n");
			goto err;
		}
	}

	return 0;

err:
	soc_dapm_free_widgets(dapm);
	return ret;
}

static struct snd_kcontrol_new *soc_fw_dapm_widget_dmixer_create(struct soc_fw *sfw,
	struct snd_soc_fw_kcontrol *kcontrol, int num_kcontrols)
{
	struct snd_kcontrol_new *kc;
	struct soc_mixer_control *sm;
	struct snd_soc_fw_mixer_control *mc;
	int i, err;

	kc = kzalloc(sizeof(*kc) * num_kcontrols, GFP_KERNEL);
	if (!kc)
		return NULL;

	for (i = 0; i < num_kcontrols; i++) {
		sm = kzalloc(sizeof(*sm), GFP_KERNEL);
		if (!sm)
			goto err;

		mc = (struct snd_soc_fw_mixer_control*)sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_mixer_control);

		/* validate kcontrol */
		if (strnlen(mc->name, SND_SOC_FW_TEXT_SIZE) ==
			SND_SOC_FW_TEXT_SIZE)
			goto err_str;

		kc[i].name = mc->name;
		kc[i].private_value = (long)sm;
		kc[i].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		sm->reg = mc->reg;
		sm->rreg = mc->rreg;
		sm->shift = mc->shift;
		sm->rshift = mc->rshift;
		sm->max = mc->max;
		sm->min = mc->min;
		sm->invert = mc->invert;
		sm->platform_max = mc->platform_max;
		INIT_LIST_HEAD(&sm->list);

		switch (mc->type) {
		case SND_SOC_FW_MIXER_SINGLE_VALUE:
		case SND_SOC_FW_MIXER_DOUBLE_VALUE:
			kc[i].get = snd_soc_get_volsw;
			kc[i].put = snd_soc_put_volsw;
			kc[i].info = snd_soc_info_volsw;
			break;
		case SND_SOC_FW_ENUM_SINGLE_T_EXT:
		case SND_SOC_FW_ENUM_DOUBLE_T_EXT:
			/* set default values - component driver can override */
			kc[i].get = snd_soc_get_volsw;
			kc[i].put = snd_soc_put_volsw;
			kc[i].info = snd_soc_info_volsw;

			err = soc_fw_init_kcontrol(sfw, kc);
			if (err < 0)
				goto err_str;

			break;
		}
	}
	return kc;
err_str:
	kfree(sm);
err:
	for (--i; i >= 0; i--)
		kfree((void*)kc[i].private_value);
	kfree(kc);
	return NULL;
}

static struct snd_kcontrol_new *soc_fw_dapm_widget_denum_create(struct soc_fw *sfw,
	struct snd_soc_fw_kcontrol *kcontrol)
{
	struct snd_kcontrol_new *kc;
	struct snd_soc_fw_enum_control *ec;
	struct soc_enum *se;
	int i, ret;

	ec = (struct snd_soc_fw_enum_control*)sfw->pos;
	sfw->pos += sizeof(struct snd_soc_fw_enum_control);

	/* validate kcontrol */
	if (strnlen(ec->name, SND_SOC_FW_TEXT_SIZE) ==
		SND_SOC_FW_TEXT_SIZE)
		return NULL;

	kc = kzalloc(sizeof(*kc), GFP_KERNEL);
	if (!kc)
		return NULL;

	se = kzalloc(sizeof(*se), GFP_KERNEL);
	if (!se)
		goto err_se;

	kc->name = ec->name;
	kc->private_value = (long)se;
	kc->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	se->reg = ec->reg;
	se->reg2 = ec->reg2;
	se->shift_l = ec->shift_l;
	se->shift_r = ec->shift_r;
	se->max = ec->max;
	se->mask = ec->mask;

	switch (ec->type) {
	case SND_SOC_FW_ENUM_SINGLE_T:
	case SND_SOC_FW_ENUM_DOUBLE_T:
		kc->get = snd_soc_get_enum_double;
		kc->put = snd_soc_put_enum_double;
		kc->info = snd_soc_info_enum_double;
		ret = soc_fw_denum_create_texts(se, ec);
		if (ret < 0)
			goto err;
		break;
	case SND_SOC_FW_ENUM_SINGLE_T_EXT:
	case SND_SOC_FW_ENUM_DOUBLE_T_EXT:
		/* set default values - component driver can override */
		kc->get = snd_soc_get_enum_double;
		kc->put = snd_soc_put_enum_double;
		kc->info = snd_soc_info_enum_double;

		ret = soc_fw_denum_create_texts(se, ec);
		if (ret < 0)
			goto err;

		ret = soc_fw_init_kcontrol(sfw, kc);
		if (ret < 0)
			goto err;

		break;
	case SND_SOC_FW_ENUM_SINGLE_V:
	case SND_SOC_FW_ENUM_DOUBLE_V:
		kc->get = snd_soc_get_value_enum_double;
		kc->put = snd_soc_put_value_enum_double;
		kc->info = snd_soc_info_enum_double;

		ret = soc_fw_denum_create_values(se, ec);
		if (ret < 0)
			goto err;

		break;
	case SND_SOC_FW_ENUM_SINGLE_V_EXT:
	case SND_SOC_FW_ENUM_DOUBLE_V_EXT:
		/* set default values - component driver can override */
		kc->get = snd_soc_get_value_enum_double;
		kc->put = snd_soc_put_value_enum_double;
		kc->info = snd_soc_info_enum_double;

		ret = soc_fw_denum_create_values(se, ec);
		if (ret < 0)
			goto err;

		ret = soc_fw_init_kcontrol(sfw, kc);
		if (ret < 0)
			goto err;

		break;
	}
	return kc;

err_se:
	kfree(kc);
err:
	/* free texts */
	if (se->dvalues)
		kfree(se->dvalues);
	else {
		for (i = 0; i < ec->max; i++)
			kfree(se->dtexts[i]);
	}
	kfree(se);
	return NULL;
}

static int soc_fw_dapm_widget_create(struct soc_fw *sfw,
	struct snd_soc_fw_dapm_widget *w)
{
	struct snd_soc_dapm_context *dapm = soc_fw_dapm_get(sfw);
	struct snd_soc_dapm_widget widget;
	struct snd_soc_fw_kcontrol *kcontrol;
	int ret = 0;

	if (strnlen(w->name, SND_SOC_FW_TEXT_SIZE) ==
		SND_SOC_FW_TEXT_SIZE)
		return -EINVAL;
	if (strnlen(w->sname, SND_SOC_FW_TEXT_SIZE) ==
		SND_SOC_FW_TEXT_SIZE)
		return -EINVAL;

	widget.id = w->id;
	widget.name = w->name;
	widget.sname = w->sname;
	widget.reg = w->reg;
	widget.shift = w->shift;
	widget.mask = w->mask;
	widget.invert = w->invert;
	widget.ignore_suspend = w->ignore_suspend;

	kcontrol = (struct snd_soc_fw_kcontrol *) sfw->pos;
	sfw->pos += sizeof(struct snd_soc_fw_kcontrol);

	switch (kcontrol->type) {
	case SND_SOC_FW_MIXER_VALUE:
		widget.num_kcontrols = kcontrol->count;
		widget.kcontrol_news = soc_fw_dapm_widget_dmixer_create(sfw, kcontrol,
			widget.num_kcontrols);
		if (!widget.kcontrol_news)
			return -ENOMEM;
		ret = soc_fw_widget_load(sfw, &widget);
		if (ret < 0)
			goto err;
		break;
	case SND_SOC_FW_MIXER_ENUM:
		widget.num_kcontrols = 1;
		widget.kcontrol_news = soc_fw_dapm_widget_denum_create(sfw, kcontrol);
		if (!widget.kcontrol_news)
			return -ENOMEM;
		ret = soc_fw_widget_load(sfw, &widget);
		if (ret < 0)
			goto err;
		break;
	default:
		dev_err(sfw->dev, "invalid widget kcontrol type\n");
		return -EINVAL;
	}

	snd_soc_dapm_new_controls(dapm, &widget,1);
	snd_soc_dapm_new_widgets(dapm);
err:
	kfree(widget.kcontrol_news);
	return ret;
}

static int soc_fw_dapm_widget_load(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	struct snd_soc_dapm_context *dapm = soc_fw_dapm_get(sfw);
	struct snd_soc_fw_dapm_elems *elem_info =
		(struct snd_soc_fw_dapm_elems*)sfw->pos;
	struct snd_soc_fw_dapm_widget *widget;
	int ret, count = elem_info->count, i;

	if (sfw->pass != SOC_FW_PASS_WIDGET) {
		sfw->pos += sizeof(struct snd_soc_fw_dapm_elems) + hdr->size;
		return 0;
	}

	sfw->pos += sizeof(struct snd_soc_fw_dapm_elems);

	if (soc_fw_check_count(sfw,
		sizeof(struct snd_soc_fw_dapm_graph_elem), count, hdr->size)) {
		dev_err(sfw->dev, "invalid count %d for widgets\n", count);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		widget = (struct snd_soc_fw_dapm_widget*) sfw->pos;
		sfw->pos += sizeof(struct snd_soc_fw_dapm_widget);

		ret = soc_fw_dapm_widget_create(sfw, widget);
		if (ret < 0)
			goto err;
	}

	return 0;
err:
	soc_dapm_free_widgets(dapm);
	return ret;
}

static int soc_fw_dapm_pin_load(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	/* TODO: add static enabled/disabled pins */
	return 0;
}

static int soc_fw_dapm_unload(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	struct snd_soc_dapm_context *dapm = soc_fw_dapm_get(sfw);

	soc_dapm_free_widgets(dapm);
	return 0;
}

static int soc_fw_dai_link_load(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	/* TODO: add DAI links based on FW routing between components */
	return 0;
}

static int soc_fw_dai_link_unload(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	/* TODO: add DAI links based on FW routing between components */
	return 0;
}

static int soc_fw_load_header(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	if (hdr->magic != SND_SOC_FW_MAGIC) {
		dev_err(sfw->dev, "%s does not have a valid header.\n",
			sfw->file);
		return -EINVAL;
	}

	dev_dbg(sfw->dev, "Got %d bytes of type %d version %d\n", hdr->size,
		hdr->type, hdr->version);

	switch (hdr->type) {
	case SND_SOC_FW_MIXER:
		return soc_fw_kcontrol_load(sfw, hdr);
	case SND_SOC_FW_DAPM_GRAPH:
		return soc_fw_dapm_graph_load(sfw, hdr);
	case SND_SOC_FW_DAPM_PINS:
		return soc_fw_dapm_pin_load(sfw, hdr);
	case SND_SOC_FW_DAPM_WIDGET:
		return soc_fw_dapm_widget_load(sfw, hdr);
	case SND_SOC_FW_DAI_LINK:
		return soc_fw_dai_link_load(sfw, hdr);
	default:
		return soc_fw_vendor_load(sfw, hdr);
	}

	return 0;
}

static int soc_fw_load_headers(struct soc_fw *sfw)
{
	struct snd_soc_fw_hdr *hdr =
		(struct snd_soc_fw_hdr*)sfw->fw->data;
	int ret;

	sfw->pass = SOC_FW_PASS_START;
	sfw->pos += sizeof(struct snd_soc_fw_hdr);

	while (sfw->pass <= SOC_FW_PASS_END) {
		while (!soc_fw_eof(sfw, hdr->size)) {
			ret = soc_fw_load_header(sfw, hdr);
			if (ret < 0)
				return ret;
		}
		sfw->pass++;
	}
	return 0;
}

static int soc_fw_unload_header(struct soc_fw *sfw, struct snd_soc_fw_hdr *hdr)
{
	if (hdr->magic != SND_SOC_FW_MAGIC) {
		dev_err(sfw->dev, "%s does not have a valid header.\n",
			sfw->file);
		return -EINVAL;
	}

	dev_dbg(sfw->dev, "Got %d bytes of type %d version %d\n", hdr->size,
		hdr->type, hdr->version);

	switch (hdr->type) {
	case SND_SOC_FW_MIXER:
		return soc_fw_kcontrol_unload(sfw, hdr);
	case SND_SOC_FW_DAPM_GRAPH:
	case SND_SOC_FW_DAPM_PINS:
	case SND_SOC_FW_DAPM_WIDGET:
		return soc_fw_dapm_unload(sfw, hdr);
	case SND_SOC_FW_DAI_LINK:
		return soc_fw_dai_link_unload(sfw, hdr);
	default:
		return soc_fw_vendor_unload(sfw, hdr);
	}

	return 0;
}

static int soc_fw_unload_headers(struct soc_fw *sfw)
{
	struct snd_soc_fw_hdr *hdr =
		(struct snd_soc_fw_hdr*)sfw->fw->data;
	int ret;

	sfw->pass = SOC_FW_PASS_START;
	sfw->pos += sizeof(struct snd_soc_fw_hdr);

	while (sfw->pass <= SOC_FW_PASS_END) {
		while (!soc_fw_eof(sfw, hdr->size)) {
			ret = soc_fw_unload_header(sfw, hdr);
			if (ret < 0)
				return ret;
		}
		sfw->pass++;
	}
	return 0;
}

int snd_soc_fw_load_codec(struct snd_soc_codec *codec,
	struct snd_soc_fw_codec_ops *ops, const char *file)
{
	struct soc_fw sfw;
	int ret;

	memset(&sfw, sizeof(sfw), 0);
	sfw.file = file;
	sfw.codec = codec;

	ret = soc_fw_load_data(&sfw);
	if (ret != 0)
		return ret;

	ret = soc_fw_load_headers(&sfw);
	soc_fw_release_data(&sfw);
	
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_fw_load_codec);

int snd_soc_fw_unload_codec(struct snd_soc_codec *codec,
	struct snd_soc_fw_codec_ops *ops, const char *file)
{
	struct soc_fw sfw;
	int ret;

	memset(&sfw, sizeof(sfw), 0);
	sfw.file = file;
	sfw.codec = codec;

	ret = soc_fw_load_data(&sfw);
	if (ret != 0)
		return ret;

	ret = soc_fw_unload_headers(&sfw);
	soc_fw_release_data(&sfw);
	
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_fw_unload_codec);

int snd_soc_fw_load_platform(struct snd_soc_platform *platform,
	struct snd_soc_fw_platform_ops *ops, const char *file)
{
	struct soc_fw sfw;
	int ret;

	memset(&sfw, sizeof(sfw), 0);
	sfw.file = file;
	sfw.platform = platform;

	ret = soc_fw_load_data(&sfw);
	if (ret != 0)
		return ret;

	ret = soc_fw_load_headers(&sfw);
	soc_fw_release_data(&sfw);
	
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_fw_load_platform);

int snd_soc_fw_unload_platform(struct snd_soc_platform *platform,
	struct snd_soc_fw_platform_ops *ops, const char *file)
{
	struct soc_fw sfw;
	int ret;

	memset(&sfw, sizeof(sfw), 0);
	sfw.file = file;
	sfw.platform = platform;

	ret = soc_fw_load_data(&sfw);
	if (ret != 0)
		return ret;

	ret = soc_fw_unload_headers(&sfw);
	soc_fw_release_data(&sfw);
	
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_fw_unload_platform);

int snd_soc_fw_load_card(struct snd_soc_card *card,
	struct snd_soc_fw_card_ops *ops, const char *file)
{
	struct soc_fw sfw;
	int ret;

	memset(&sfw, sizeof(sfw), 0);
	sfw.file = file;
	sfw.card = card;

	ret = soc_fw_load_data(&sfw);
	if (ret != 0)
		return ret;

	ret = soc_fw_load_headers(&sfw);
	soc_fw_release_data(&sfw);
	
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_fw_load_card);

int snd_soc_fw_unload_card(struct snd_soc_card *card,
	struct snd_soc_fw_card_ops *ops, const char *file)
{
	struct soc_fw sfw;
	int ret;

	memset(&sfw, sizeof(sfw), 0);
	sfw.file = file;
	sfw.card = card;

	ret = soc_fw_load_data(&sfw);
	if (ret != 0)
		return ret;

	ret = soc_fw_unload_headers(&sfw);
	soc_fw_release_data(&sfw);
	
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_fw_unload_card);

