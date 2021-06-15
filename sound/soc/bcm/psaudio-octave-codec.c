/*
 * Driver for PSAudio Octave Streamer I2S
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>

/* Divide by 2 for GPIO bits, use bit 0 to set clockrate */
enum psaudio_fpga_rates {
        FPGA_44100 = 0,
        FPGA_48000,
        FPGA_88200,
        FPGA_96000,
        FPGA_176400,
        FPGA_192000,
        FPGA_352800,
        FPGA_384000,
};

struct psaudio_gpios {
        struct gpio_desc *fpga_mute_gpiod;
        struct gpio_desc *fpga_dsd_gpiod;
        struct gpio_desc *fpga_48s_gpiod;
        struct gpio_desc *fpga_rate_0_gpiod;    /* LSB */
        struct gpio_desc *fpga_rate_1_gpiod;
        struct gpio_desc *fpga_rate_2_gpiod;
};

struct psaudio_priv {
	struct device *dev;
	struct psaudio_gpios psa_gpios;
	bool is_master_mode;
};

static void psaudio_fpga_mute(struct psaudio_priv *i2s, bool mute)
{
        struct psaudio_gpios *psa_gpa = &i2s->psa_gpios;

        if (!psa_gpa->fpga_mute_gpiod)
                return;
        gpiod_set_value(psa_gpa->fpga_mute_gpiod, !!mute);
};

static void psaudio_set_dsd(struct psaudio_priv *i2s, bool enable)
{
        struct psaudio_gpios *psa_gpa = &i2s->psa_gpios;

        if (!psa_gpa->fpga_mute_gpiod)
                return;
        gpiod_set_value(psa_gpa->fpga_dsd_gpiod, !!enable);
};

static void psaudio_set_clockrate(struct psaudio_priv *i2s, bool is_48_series)
{
        struct psaudio_gpios *psa_gpa = &i2s->psa_gpios;

        if (!psa_gpa->fpga_mute_gpiod)
                return;
        gpiod_set_value(psa_gpa->fpga_48s_gpiod, !!is_48_series);
};

/* Probably should mute before calling this, as it's non-atomic */
static void psaudio_set_fpga_bitrate(struct psaudio_priv *i2s, uint8_t fpga_br)
{
        struct psaudio_gpios *psa_gpa = &i2s->psa_gpios;

        if (!psa_gpa->fpga_mute_gpiod)
                return;

        dev_dbg(i2s->dev, "%s(): setting bitrate GPIO to 0x%02X\n", __func__, fpga_br & 0x7);

        gpiod_set_value(psa_gpa->fpga_rate_0_gpiod, fpga_br & (1 << 0));
        gpiod_set_value(psa_gpa->fpga_rate_1_gpiod, fpga_br & (1 << 1));
        gpiod_set_value(psa_gpa->fpga_rate_2_gpiod, fpga_br & (1 << 2));
};

/* Probably should mute (before calling this), as it's non-atomic */
static int psaudio_freq_to_gpio(struct psaudio_priv *i2s, unsigned freq)
{
        struct psaudio_gpios *psa_gpa = &i2s->psa_gpios;
        enum psaudio_fpga_rates fpga_rate;

        if (!psa_gpa->fpga_mute_gpiod)
                return 0;

        switch (freq) {
                case 44100:
                        fpga_rate = FPGA_44100;
                        break;
                case 48000:
                        fpga_rate = FPGA_48000;
                        break;
                case 88200:
                        fpga_rate = FPGA_88200;
                        break;
                case 96000:
                        fpga_rate = FPGA_96000;
                        break;
                case 176400:
                        fpga_rate = FPGA_176400;
                        break;
                case 192000:
                        fpga_rate = FPGA_192000;
                        break;
                case 352800:
                        fpga_rate = FPGA_352800;
                        break;
                case 384000:
                        fpga_rate = FPGA_384000;
                        break;
                default:
                        dev_err(i2s->dev, "%s(): bad FPGA freq %u\n", __func__, freq);
                        return EINVAL;
        };

        psaudio_fpga_mute(i2s, 1);
        psaudio_set_clockrate(i2s, fpga_rate & 1);
        psaudio_set_fpga_bitrate(i2s, fpga_rate >> 1);
        psaudio_fpga_mute(i2s, 0);

        return 0;
}

static int psaudio_alloc_pins(struct platform_device *pdev)
{

        struct psaudio_priv *i2s;
        struct psaudio_gpios *psa_gpa;
        struct gpio_desc *gd = NULL;

        i2s = dev_get_drvdata(&pdev->dev);
        if (!i2s)
                return -ENODEV;

        psa_gpa = &i2s->psa_gpios;

        gd = psa_gpa->fpga_mute_gpiod = gpiod_get(&pdev->dev, "fpga_mute", GPIOD_OUT_LOW);
        if (IS_ERR(gd)) {
                if (IS_ERR_VALUE(gd) == -ENOENT) {
                        dev_info(&pdev->dev, "No PS Audio properties\n");
                        psa_gpa->fpga_mute_gpiod = NULL;
                        return 0;
                } else {
                        dev_err(&pdev->dev, "Error getting PS Audio GPIO descriptor\n");
                        return -ENOENT;
                }
        }

        gd = psa_gpa->fpga_dsd_gpiod = gpiod_get(&pdev->dev, "fpga_dsd", GPIOD_OUT_LOW);
        if (IS_ERR(gd)) {
                dev_err(&pdev->dev, "Error getting PS Audio DSD GPIO descriptor\n");
                return -ENOENT;
        }

        gd = psa_gpa->fpga_48s_gpiod = gpiod_get(&pdev->dev, "fpga_48s", GPIOD_OUT_LOW);
        if (IS_ERR(gd)) {
                dev_err(&pdev->dev, "Error getting PS Audio Clockrate GPIO descriptor\n");
                return -ENOENT;
        }

        gd = psa_gpa->fpga_rate_0_gpiod = gpiod_get(&pdev->dev, "fpga_rate_bit0", GPIOD_OUT_LOW);
        if (IS_ERR(gd)) {
                dev_err(&pdev->dev, "Error getting PS Audio Bit0 GPIO descriptor\n");
                return -ENOENT;
        }

        gd = psa_gpa->fpga_rate_1_gpiod = gpiod_get(&pdev->dev, "fpga_rate_bit1", GPIOD_OUT_LOW);
        if (IS_ERR(gd)) {
                dev_err(&pdev->dev, "Error getting PS Audio Bit1 GPIO descriptor\n");
                return -ENOENT;
        }

        gd = psa_gpa->fpga_rate_2_gpiod = gpiod_get(&pdev->dev, "fpga_rate_bit2", GPIOD_OUT_LOW);
        if (IS_ERR(gd)) {
                dev_err(&pdev->dev, "Error getting PS Audio Bit2 GPIO descriptor\n");
                return -ENOENT;
        }

        return 0;
}

static int psaudio_component_probe(struct snd_soc_component *component)
{
	struct psaudio_priv *psaudio =
				snd_soc_component_get_drvdata(component);

	dev_dbg(psaudio->dev, "%s(): Enter\n", __func__);
	return 0;
}

static void psaudio_component_remove(struct snd_soc_component *component)
{
	struct psaudio_priv *psaudio =
				snd_soc_component_get_drvdata(component);
	dev_dbg(psaudio->dev, "%s(): Enter\n", __func__);
}

static const struct snd_soc_dapm_widget psaudio_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("I2S"),
};

static const struct snd_soc_dapm_route psaudio_dapm_routes[] = {
	{"I2S", NULL, "HiFi Playback"},
};

static const struct snd_soc_component_driver psaudio_component_driver = {
	.probe = psaudio_component_probe,
	.remove = psaudio_component_remove,
	.dapm_widgets = psaudio_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(psaudio_dapm_widgets),
	.dapm_routes = psaudio_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(psaudio_dapm_routes),
};

static int psaudio_daiops_trigger(struct snd_pcm_substream *substream, int cmd,
				   struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct psaudio_priv *psaudio =
				snd_soc_component_get_drvdata(component);

	dev_dbg(psaudio->dev, "%s(): Enter\n", __func__);

	dev_dbg(dai->dev, "CMD             %d", cmd);
	dev_dbg(dai->dev, "Playback Active %d", dai->stream_active[SNDRV_PCM_STREAM_PLAYBACK]);
	dev_dbg(dai->dev, "Capture Active  %d", dai->stream_active[SNDRV_PCM_STREAM_CAPTURE]);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (dai->stream_active[SNDRV_PCM_STREAM_PLAYBACK]) {
			dev_dbg(dai->dev, "Enabling audio ...\n");
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (dai->stream_active[SNDRV_PCM_STREAM_PLAYBACK]) {
			dev_dbg(dai->dev, "Disabling audio ...\n");
		}
		break;
	}
	return 0;
}

static int psaudio_daiops_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	struct psaudio_priv *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned rate = params_rate(params);

	dev_dbg(i2s->dev, "%s(): Enter, %s mode rate %u\n", __func__,
	    i2s->is_master_mode ? "master" : "slave", rate);
	if (i2s->is_master_mode) {
		psaudio_freq_to_gpio(i2s, rate);
		//FIXME: I don't think we can do DSD
		psaudio_set_dsd(i2s, 0);
	}

	return 0;
}

static int psaudio_daiops_set_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{

	struct psaudio_priv *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	switch(fmt & SND_SOC_DAIFMT_MASTER_MASK) {
		case SND_SOC_DAIFMT_CBS_CFS:
			i2s->is_master_mode = true;
			break;
		case SND_SOC_DAIFMT_CBM_CFM:
			i2s->is_master_mode = false;
			break;
		default:
			return -EINVAL;

	}

	return 0;
}

static const struct snd_soc_dai_ops psaudio_dai_ops = {
	.trigger = psaudio_daiops_trigger,
	.hw_params = psaudio_daiops_hw_params,
	.set_fmt = psaudio_daiops_set_fmt,
};

static struct snd_soc_dai_driver psaudio_dai = {

	.name = "ps-octave-hifi",
	.playback = {
		.stream_name = "HiFi Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates =        SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min =     8000,
		.rate_max =     384000,
		.formats =      SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &psaudio_dai_ops,
	.symmetric_rate = 1
};

#ifdef CONFIG_OF
static const struct of_device_id psaudio_ids[] = {
		{ .compatible = "psaudio,octave", }, {}
	};
	MODULE_DEVICE_TABLE(of, psaudio_ids);
#endif

static int psaudio_platform_probe(struct platform_device *pdev)
{
	struct psaudio_priv *psaudio;
	int ret = 0;

	psaudio = devm_kzalloc(&pdev->dev, sizeof(*psaudio), GFP_KERNEL);
	if (!psaudio)
		return -ENOMEM;

	psaudio->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, psaudio);

	ret = psaudio_alloc_pins(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Could not register PS Audio GPIOs\n");
		return ret;
	}

	return snd_soc_register_component(&pdev->dev,
					  &psaudio_component_driver,
					  &psaudio_dai,
					  1);
}

static int psaudio_platform_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static struct platform_driver psaudio_driver = {
	.driver = {
		.name = "ps-octave-codec",
		.of_match_table = of_match_ptr(psaudio_ids),
	},
	.probe = psaudio_platform_probe,
	.remove = psaudio_platform_remove,
};

module_platform_driver(psaudio_driver);

MODULE_DESCRIPTION("PS Audio I2S Stub Codec");
MODULE_AUTHOR("Kenneth Crudup <kenny@panix.com>");
MODULE_LICENSE("GPL v2");
