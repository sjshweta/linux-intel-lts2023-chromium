// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2021-2022 Intel Corporation
//
// Authors: Cezary Rojewski <cezary.rojewski@intel.com>
//          Amadeusz Slawinski <amadeuszx.slawinski@linux.intel.com>
//

#include <linux/module.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include <sound/soc-dapm.h>
#include "../utils.h"

static int avs_create_dai_link(struct device *dev, const char *platform_name, int ssp_port,
			       int tdm_slot, struct snd_soc_dai_link **dai_link)
{
	struct snd_soc_dai_link_component *platform;
	struct snd_soc_dai_link *dl;

	dl = devm_kzalloc(dev, sizeof(*dl), GFP_KERNEL);
	platform = devm_kzalloc(dev, sizeof(*platform), GFP_KERNEL);
	if (!dl || !platform)
		return -ENOMEM;

	platform->name = platform_name;

	dl->name = devm_kasprintf(dev, GFP_KERNEL,
				  AVS_STRING_FMT("SSP", "-Codec", ssp_port, tdm_slot));
	dl->cpus = devm_kzalloc(dev, sizeof(*dl->cpus), GFP_KERNEL);
	if (!dl->name || !dl->cpus)
		return -ENOMEM;

	dl->cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL,
					    AVS_STRING_FMT("SSP", " Pin", ssp_port, tdm_slot));
	dl->codecs = &snd_soc_dummy_dlc;
	if (!dl->cpus->dai_name || !dl->codecs->name || !dl->codecs->dai_name)
		return -ENOMEM;

	dl->num_cpus = 1;
	dl->num_codecs = 1;
	dl->platforms = platform;
	dl->num_platforms = 1;
	dl->id = 0;
	dl->nonatomic = 1;
	dl->no_pcm = 1;
	dl->dpcm_capture = 1;
	dl->dpcm_playback = 1;

	*dai_link = dl;

	return 0;
}

static int avs_create_dapm_routes(struct device *dev, int ssp_port, int tdm_slot,
				  struct snd_soc_dapm_route **routes, int *num_routes)
{
	struct snd_soc_dapm_route *dr;
	const int num_dr = 2;

	dr = devm_kcalloc(dev, num_dr, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	dr[0].sink = devm_kasprintf(dev, GFP_KERNEL,
				    AVS_STRING_FMT("ssp", "pb", ssp_port, tdm_slot));
	dr[0].source = devm_kasprintf(dev, GFP_KERNEL,
				      AVS_STRING_FMT("ssp", " Tx", ssp_port, tdm_slot));
	if (!dr[0].sink || !dr[0].source)
		return -ENOMEM;

	dr[1].sink = devm_kasprintf(dev, GFP_KERNEL,
				    AVS_STRING_FMT("ssp", " Rx", ssp_port, tdm_slot));
	dr[1].source = devm_kasprintf(dev, GFP_KERNEL,
				      AVS_STRING_FMT("ssp", "cp", ssp_port, tdm_slot));
	if (!dr[1].sink || !dr[1].source)
		return -ENOMEM;

	*routes = dr;
	*num_routes = num_dr;

	return 0;
}

static int avs_create_dapm_widgets(struct device *dev, int ssp_port, int tdm_slot,
				   struct snd_soc_dapm_widget **widgets, int *num_widgets)
{
	struct snd_soc_dapm_widget *dw;
	const int num_dw = 2;

	dw = devm_kcalloc(dev, num_dw, sizeof(*dw), GFP_KERNEL);
	if (!dw)
		return -ENOMEM;

	dw[0].id = snd_soc_dapm_hp;
	dw[0].reg = SND_SOC_NOPM;
	dw[0].name = devm_kasprintf(dev, GFP_KERNEL,
				    AVS_STRING_FMT("ssp", "pb", ssp_port, tdm_slot));
	if (!dw[0].name)
		return -ENOMEM;

	dw[1].id = snd_soc_dapm_mic;
	dw[1].reg = SND_SOC_NOPM;
	dw[1].name = devm_kasprintf(dev, GFP_KERNEL,
				    AVS_STRING_FMT("ssp", "cp", ssp_port, tdm_slot));
	if (!dw[1].name)
		return -ENOMEM;

	*widgets = dw;
	*num_widgets = num_dw;

	return 0;
}

static int avs_i2s_test_probe(struct platform_device *pdev)
{
	struct snd_soc_dapm_widget *widgets;
	struct snd_soc_dapm_route *routes;
	struct snd_soc_dai_link *dai_link;
	struct snd_soc_acpi_mach *mach;
	struct snd_soc_card *card;
	struct device *dev = &pdev->dev;
	const char *pname;
	int num_routes, num_widgets;
	int ssp_port, tdm_slot, ret;

	mach = dev_get_platdata(dev);
	pname = mach->mach_params.platform;

	if (!avs_mach_singular_ssp(mach)) {
		dev_err(dev, "Invalid SSP configuration\n");
		return -EINVAL;
	}
	ssp_port = avs_mach_ssp_port(mach);

	if (!avs_mach_singular_tdm(mach, ssp_port)) {
		dev_err(dev, "Invalid TDM configuration\n");
		return -EINVAL;
	}
	tdm_slot = avs_mach_ssp_tdm(mach, ssp_port);

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->name = devm_kasprintf(dev, GFP_KERNEL,
				    AVS_STRING_FMT("ssp", "-loopback", ssp_port, tdm_slot));
	if (!card->name)
		return -ENOMEM;

	ret = avs_create_dai_link(dev, pname, ssp_port, tdm_slot, &dai_link);
	if (ret) {
		dev_err(dev, "Failed to create dai link: %d\n", ret);
		return ret;
	}

	ret = avs_create_dapm_routes(dev, ssp_port, tdm_slot, &routes, &num_routes);
	if (ret) {
		dev_err(dev, "Failed to create dapm routes: %d\n", ret);
		return ret;
	}

	ret = avs_create_dapm_widgets(dev, ssp_port, tdm_slot, &widgets, &num_widgets);
	if (ret) {
		dev_err(dev, "Failed to create dapm widgets: %d\n", ret);
		return ret;
	}

	card->dev = dev;
	card->owner = THIS_MODULE;
	card->dai_link = dai_link;
	card->num_links = 1;
	card->dapm_routes = routes;
	card->num_dapm_routes = num_routes;
	card->dapm_widgets = widgets;
	card->num_dapm_widgets = num_widgets;
	card->fully_routed = true;

	ret = snd_soc_fixup_dai_links_platform_name(card, pname);
	if (ret)
		return ret;

	return devm_snd_soc_register_card(dev, card);
}

static const struct platform_device_id avs_i2s_test_driver_ids[] = {
	{
		.name = "avs_i2s_test",
	},
	{},
};
MODULE_DEVICE_TABLE(platform, avs_i2s_test_driver_ids);

static struct platform_driver avs_i2s_test_driver = {
	.probe = avs_i2s_test_probe,
	.driver = {
		.name = "avs_i2s_test",
		.pm = &snd_soc_pm_ops,
	},
	.id_table = avs_i2s_test_driver_ids,
};

module_platform_driver(avs_i2s_test_driver);

MODULE_DESCRIPTION("Intel i2s test machine driver");
MODULE_LICENSE("GPL");
