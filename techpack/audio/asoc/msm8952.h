/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2018-2019, 2021, The Linux Foundation. All rights reserved.
 */

#ifndef __MSM8953_H
#define __MSM8953_H

#include <sound/soc.h>
#include <dsp/q6afe-v2.h>
#include <asoc/wcd-mbhc-v2.h>
#include "codecs/sdm660_cdc/msm-analog-cdc.h"
#include "codecs/sdm660_cdc/msm-digital-cdc.h"
#include "codecs/wsa881x-analog.h"
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>


#define BTSCO_RATE_8KHZ 8000
#define BTSCO_RATE_16KHZ 16000

#define SAMPLING_RATE_48KHZ     48000
#define SAMPLING_RATE_96KHZ     96000
#define SAMPLING_RATE_192KHZ    192000

#define PRI_MI2S_ID     (1 << 0)
#define SEC_MI2S_ID     (1 << 1)
#define TER_MI2S_ID     (1 << 2)
#define QUAT_MI2S_ID    (1 << 3)
#define QUIN_MI2S_ID    (1 << 4)

#define DEFAULT_MCLK_RATE 9600000

#define WCD_MBHC_DEF_RLOADS 5
#define MAX_WSA_CODEC_NAME_LENGTH 80
#define MSM_DT_MAX_PROP_SIZE 80

enum {
	DIG_CDC,
	ANA_CDC,
	CODECS_MAX,
};

enum {
	PRIM_MI2S = 0,
	SEC_MI2S,
	TERT_MI2S,
	QUAT_MI2S,
	QUIN_MI2S,
	MI2S_MAX,
};

enum {
	INT_SND_CARD,
	INT_DIG_SND_CARD,
	INT_MAX_SND_CARD = INT_DIG_SND_CARD,
	EXT_SND_CARD_TASHA,
	EXT_SND_CARD_TAVIL,
};

struct msm_asoc_mach_data {
	int codec_type;
	int ext_pa;
	int us_euro_gpio;
	int spk_ext_pa_gpio;
	int mclk_freq;
	bool native_clk_set;
	int lb_mode;
	int afe_clk_ver;
	int snd_card_val;
	u8 micbias1_cap_mode;
	u8 micbias2_cap_mode;
	atomic_t int_mclk0_rsc_ref;
	atomic_t int_mclk0_enabled;
	atomic_t wsa_int_mclk0_rsc_ref;
	struct mutex cdc_int_mclk0_mutex;
	struct mutex wsa_mclk_mutex;
	struct delayed_work disable_int_mclk0_work;
	struct afe_digital_clk_cfg digital_cdc_clk;
	struct afe_clk_set digital_cdc_core_clk;
	void __iomem *vaddr_gpio_mux_spkr_ctl;
	void __iomem *vaddr_gpio_mux_mic_ctl;
	void __iomem *vaddr_gpio_mux_quin_ctl;
	void __iomem *vaddr_gpio_mux_pcm_ctl;
	struct on_demand_supply wsa_switch_supply;
	struct device_node *spk_ext_pa_gpio_p;
	struct device_node *us_euro_gpio_p;
	struct device_node *comp_gpio_p;
	struct device_node *mi2s_gpio_p[MI2S_MAX];
	struct device_node *dmic_gpio_p; /* used by pinctrl API */
	struct snd_soc_codec *codec;
	struct snd_info_entry *codec_root;
};

#endif/*__MSM8953_H*/
