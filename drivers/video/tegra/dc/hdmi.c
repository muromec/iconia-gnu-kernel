/*
 * drivers/video/tegra/dc/hdmi.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gcd.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <mach/clk.h>
#include <mach/dc.h>
#include <mach/fb.h>
#include <mach/nvhost.h>

#include <video/tegrafb.h>

#include "dc_reg.h"
#include "dc_priv.h"
#include "hdmi_reg.h"
#include "hdmi.h"
#include "edid.h"
#include "nvhdcp.h"

/* datasheet claims this will always be 216MHz */
#define HDMI_AUDIOCLK_FREQ		216000000

#define HDMI_REKEY_DEFAULT		56

struct tegra_dc_hdmi_data {
	struct tegra_dc			*dc;
	struct tegra_edid		*edid;
	struct tegra_nvhdcp		*nvhdcp;
	struct work_struct		hpd_debounce_edge_wq;
	struct delayed_work		hpd_debounce_stable_wq;

	struct resource			*base_res;
	void __iomem			*base;
	struct clk			*clk;

	struct clk			*disp1_clk;
	struct clk			*disp2_clk;

	spinlock_t			suspend_lock;
	bool				suspended;
	bool				hpd_pending;

	bool				dvi;

	unsigned			audio_freq;
};

/* table of electrical settings, must be in acending order. */
struct tdms_config {
	int pclk;
	u32 pll0;
	u32 pll1;
	u32 pe_current; /* pre-emphasis */
	u32 drive_current;
};

static const struct tdms_config tdms_config[] = {
	{ /* 480p modes */
		.pclk = 27000000,
		.pll0 = SOR_PLL_BG_V17_S(3) | SOR_PLL_ICHPMP(1) |
			SOR_PLL_RESISTORSEL | SOR_PLL_VCOCAP(0) |
			SOR_PLL_TX_REG_LOAD(3),
		.pll1 = SOR_PLL_TMDS_TERM_ENABLE,
		.pe_current = PE_CURRENT0(PE_CURRENT_0_0_mA) |
			PE_CURRENT1(PE_CURRENT_0_0_mA) |
			PE_CURRENT2(PE_CURRENT_0_0_mA) |
			PE_CURRENT3(PE_CURRENT_0_0_mA),
		.drive_current = DRIVE_CURRENT_LANE0(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE1(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE2(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE3(DRIVE_CURRENT_7_125_mA),
	},
	{ /* 720p modes */
		.pclk = 74250000,
		.pll0 = SOR_PLL_BG_V17_S(3) | SOR_PLL_ICHPMP(1) |
			SOR_PLL_RESISTORSEL | SOR_PLL_VCOCAP(1) |
			SOR_PLL_TX_REG_LOAD(3),
		.pll1 = SOR_PLL_TMDS_TERM_ENABLE | SOR_PLL_PE_EN,
		.pe_current = PE_CURRENT0(PE_CURRENT_6_0_mA) |
			PE_CURRENT1(PE_CURRENT_6_0_mA) |
			PE_CURRENT2(PE_CURRENT_6_0_mA) |
			PE_CURRENT3(PE_CURRENT_6_0_mA),
		.drive_current = DRIVE_CURRENT_LANE0(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE1(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE2(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE3(DRIVE_CURRENT_7_125_mA),
	},
	{ /* 1080p modes */
		.pclk = INT_MAX,
		.pll0 = SOR_PLL_BG_V17_S(3) | SOR_PLL_ICHPMP(1) |
			SOR_PLL_RESISTORSEL | SOR_PLL_VCOCAP(1) |
			SOR_PLL_TX_REG_LOAD(3),
		.pll1 = SOR_PLL_TMDS_TERM_ENABLE | SOR_PLL_PE_EN,
		.pe_current = PE_CURRENT0(PE_CURRENT_6_0_mA) |
			PE_CURRENT1(PE_CURRENT_6_0_mA) |
			PE_CURRENT2(PE_CURRENT_6_0_mA) |
			PE_CURRENT3(PE_CURRENT_6_0_mA),
		.drive_current = DRIVE_CURRENT_LANE0(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE1(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE2(DRIVE_CURRENT_7_125_mA) |
			DRIVE_CURRENT_LANE3(DRIVE_CURRENT_7_125_mA),
	},
};

static int tegra_hdmi_calc_audio_cts_n(unsigned fs,
				       unsigned pll_rate, unsigned pll_div,
				       unsigned *cts_out, unsigned *n_out)
{
	unsigned n_ideal = (128 * fs) / 1000;
	unsigned cts = pll_rate;
	unsigned n = 128 * fs * pll_div;
	unsigned common_divisor = gcd(cts, n);
	unsigned mult;

	cts /= common_divisor;
	n /= common_divisor;
	mult = n_ideal / n;
	if (mult) {
		n *= mult;
		cts *= mult;
	}

	if (cts < (1 << 20)) {
		*cts_out = cts;
		*n_out = n;
		return 0;
	}

	return -EINVAL;
}

unsigned long tegra_hdmi_readl(struct tegra_dc_hdmi_data *hdmi,
					     unsigned long reg)
{
	return readl(hdmi->base + reg * 4);
}

void tegra_hdmi_writel(struct tegra_dc_hdmi_data *hdmi,
				     unsigned long val, unsigned long reg)
{
	writel(val, hdmi->base + reg * 4);
}

static inline void tegra_hdmi_clrsetbits(struct tegra_dc_hdmi_data *hdmi,
					 unsigned long reg, unsigned long clr,
					 unsigned long set)
{
	unsigned long val = tegra_hdmi_readl(hdmi, reg);
	val &= ~clr;
	val |= set;
	tegra_hdmi_writel(hdmi, val, reg);
}

#define DUMP_REG(a) do {						\
		printk("HDMI %-32s\t%03x\t%08lx\n",			\
		       #a, a, tegra_hdmi_readl(hdmi, a));		\
	} while (0)

#ifdef DEBUG
static void hdmi_dumpregs(struct tegra_dc_hdmi_data *hdmi)
{
	DUMP_REG(HDMI_CTXSW);
	DUMP_REG(HDMI_NV_PDISP_SOR_STATE0);
	DUMP_REG(HDMI_NV_PDISP_SOR_STATE1);
	DUMP_REG(HDMI_NV_PDISP_SOR_STATE2);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_AN_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_AN_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CN_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CN_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_AKSV_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_AKSV_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_BKSV_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_BKSV_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CKSV_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CKSV_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_DKSV_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_DKSV_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CTRL);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CMODE);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_MPRIME_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_MPRIME_LSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_SPRIME_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_SPRIME_LSB2);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_SPRIME_LSB1);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_RI);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CS_MSB);
	DUMP_REG(HDMI_NV_PDISP_RG_HDCP_CS_LSB);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_EMU0);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_EMU_RDATA0);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_EMU1);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_EMU2);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_CTRL);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_STATUS);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_HEADER);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_SUBPACK0_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_SUBPACK0_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_CTRL);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_STATUS);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_HEADER);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_SUBPACK0_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_SUBPACK0_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_SUBPACK1_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_SUBPACK1_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_CTRL);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_STATUS);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_HEADER);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK0_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK0_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK1_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK1_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK2_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK2_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK3_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GENERIC_SUBPACK3_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_CTRL);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0320_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0320_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0441_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0441_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0882_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0882_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_1764_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_1764_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0480_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0480_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0960_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_0960_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_1920_SUBPACK_LOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_ACR_1920_SUBPACK_HIGH);
	DUMP_REG(HDMI_NV_PDISP_HDMI_CTRL);
	DUMP_REG(HDMI_NV_PDISP_HDMI_VSYNC_KEEPOUT);
	DUMP_REG(HDMI_NV_PDISP_HDMI_VSYNC_WINDOW);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GCP_CTRL);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GCP_STATUS);
	DUMP_REG(HDMI_NV_PDISP_HDMI_GCP_SUBPACK);
	DUMP_REG(HDMI_NV_PDISP_HDMI_CHANNEL_STATUS1);
	DUMP_REG(HDMI_NV_PDISP_HDMI_CHANNEL_STATUS2);
	DUMP_REG(HDMI_NV_PDISP_HDMI_EMU0);
	DUMP_REG(HDMI_NV_PDISP_HDMI_EMU1);
	DUMP_REG(HDMI_NV_PDISP_HDMI_EMU1_RDATA);
	DUMP_REG(HDMI_NV_PDISP_HDMI_SPARE);
	DUMP_REG(HDMI_NV_PDISP_HDMI_SPDIF_CHN_STATUS1);
	DUMP_REG(HDMI_NV_PDISP_HDMI_SPDIF_CHN_STATUS2);
	DUMP_REG(HDMI_NV_PDISP_HDCPRIF_ROM_CTRL);
	DUMP_REG(HDMI_NV_PDISP_SOR_CAP);
	DUMP_REG(HDMI_NV_PDISP_SOR_PWR);
	DUMP_REG(HDMI_NV_PDISP_SOR_TEST);
	DUMP_REG(HDMI_NV_PDISP_SOR_PLL0);
	DUMP_REG(HDMI_NV_PDISP_SOR_PLL1);
	DUMP_REG(HDMI_NV_PDISP_SOR_PLL2);
	DUMP_REG(HDMI_NV_PDISP_SOR_CSTM);
	DUMP_REG(HDMI_NV_PDISP_SOR_LVDS);
	DUMP_REG(HDMI_NV_PDISP_SOR_CRCA);
	DUMP_REG(HDMI_NV_PDISP_SOR_CRCB);
	DUMP_REG(HDMI_NV_PDISP_SOR_BLANK);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_CTL);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST0);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST1);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST2);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST3);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST4);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST5);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST6);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST7);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST8);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INST9);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INSTA);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INSTB);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INSTC);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INSTD);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INSTE);
	DUMP_REG(HDMI_NV_PDISP_SOR_SEQ_INSTF);
	DUMP_REG(HDMI_NV_PDISP_SOR_VCRCA0);
	DUMP_REG(HDMI_NV_PDISP_SOR_VCRCA1);
	DUMP_REG(HDMI_NV_PDISP_SOR_CCRCA0);
	DUMP_REG(HDMI_NV_PDISP_SOR_CCRCA1);
	DUMP_REG(HDMI_NV_PDISP_SOR_EDATAA0);
	DUMP_REG(HDMI_NV_PDISP_SOR_EDATAA1);
	DUMP_REG(HDMI_NV_PDISP_SOR_COUNTA0);
	DUMP_REG(HDMI_NV_PDISP_SOR_COUNTA1);
	DUMP_REG(HDMI_NV_PDISP_SOR_DEBUGA0);
	DUMP_REG(HDMI_NV_PDISP_SOR_DEBUGA1);
	DUMP_REG(HDMI_NV_PDISP_SOR_TRIG);
	DUMP_REG(HDMI_NV_PDISP_SOR_MSCHECK);
	DUMP_REG(HDMI_NV_PDISP_SOR_LANE_DRIVE_CURRENT);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_DEBUG0);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_DEBUG1);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_DEBUG2);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(0));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(1));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(2));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(3));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(4));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(5));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_FS(6));
	DUMP_REG(HDMI_NV_PDISP_AUDIO_PULSE_WIDTH);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_THRESHOLD);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_CNTRL0);
	DUMP_REG(HDMI_NV_PDISP_AUDIO_N);
	DUMP_REG(HDMI_NV_PDISP_HDCPRIF_ROM_TIMING);
	DUMP_REG(HDMI_NV_PDISP_SOR_REFCLK);
	DUMP_REG(HDMI_NV_PDISP_CRC_CONTROL);
	DUMP_REG(HDMI_NV_PDISP_INPUT_CONTROL);
	DUMP_REG(HDMI_NV_PDISP_SCRATCH);
	DUMP_REG(HDMI_NV_PDISP_PE_CURRENT);
	DUMP_REG(HDMI_NV_PDISP_KEY_CTRL);
	DUMP_REG(HDMI_NV_PDISP_KEY_DEBUG0);
	DUMP_REG(HDMI_NV_PDISP_KEY_DEBUG1);
	DUMP_REG(HDMI_NV_PDISP_KEY_DEBUG2);
	DUMP_REG(HDMI_NV_PDISP_KEY_HDCP_KEY_0);
	DUMP_REG(HDMI_NV_PDISP_KEY_HDCP_KEY_1);
	DUMP_REG(HDMI_NV_PDISP_KEY_HDCP_KEY_2);
	DUMP_REG(HDMI_NV_PDISP_KEY_HDCP_KEY_3);
	DUMP_REG(HDMI_NV_PDISP_KEY_HDCP_KEY_TRIG);
	DUMP_REG(HDMI_NV_PDISP_KEY_SKEY_INDEX);
}
#endif

static bool tegra_dc_hdmi_hpd(struct tegra_dc *dc)
{
	int sense;
	int level;

	level = gpio_get_value(dc->out->hotplug_gpio);

	sense = dc->out->flags & TEGRA_DC_OUT_HOTPLUG_MASK;

	return (sense == TEGRA_DC_OUT_HOTPLUG_HIGH && level) ||
		(sense == TEGRA_DC_OUT_HOTPLUG_LOW && !level);
}

static bool tegra_dc_hdmi_detect(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	struct fb_monspecs specs;
	int err;

	if (!tegra_dc_hdmi_hpd(dc))
		goto fail;

	err = tegra_edid_get_monspecs(hdmi->edid, &specs);
	if (err < 0) {
		dev_err(&dc->ndev->dev, "error reading edid\n");
		goto fail;
	}

	/* monitors like to lie about these but they are still useful for
	 * detecting aspect ratios
	 */
	dc->out->h_size = specs.max_x * 1000;
	dc->out->v_size = specs.max_y * 1000;


	hdmi->dvi = !(specs.misc & FB_MISC_HDMI);

	tegra_fb_update_monspecs(dc->fb, &specs, tegra_dc_mode_filter);
	dev_info(&dc->ndev->dev, "display detected\n");

	dc->connected = true;
	tegra_dc_ext_process_hotplug(dc->ndev->id);

	return true;

fail:
	tegra_nvhdcp_set_plug(hdmi->nvhdcp, 0);
	return false;
}

static void tegra_dc_hdmi_enqueue_detect(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);

	queue_work(system_nrt_wq, &hdmi->hpd_debounce_edge_wq);
}

/* This should only get called once the hotplug state is stable */
static void tegra_dc_hdmi_detect_worker(struct work_struct *work)
{
	struct tegra_dc_hdmi_data *hdmi =
		container_of(to_delayed_work(work),
			struct tegra_dc_hdmi_data, hpd_debounce_stable_wq);
	struct tegra_dc *dc = hdmi->dc;

	if (!tegra_dc_hdmi_detect(dc)) {
		tegra_dc_disable(dc);
		tegra_fb_update_monspecs(dc->fb, NULL, NULL);

		dc->connected = false;
		tegra_dc_ext_process_hotplug(dc->ndev->id);
	}
}

/*
 * This is called asap in response to a hpd interrupt.  For debouncing, we
 * don't actually report the hotplug immediately; instead, we queue up
 * detection for later.  If we get another hpd interrupt before it's complete,
 * we cancel the previously-queued work and requeue for even later.  This way,
 * we'll only enable if we get no hotplug interrupts for ~100 ms and it still
 * looks connected.
 */
static void tegra_dc_hdmi_hpd_debounce_edge(struct work_struct *work)
{
	struct tegra_dc_hdmi_data *hdmi =
		container_of(work, struct tegra_dc_hdmi_data,
			     hpd_debounce_edge_wq);
	struct tegra_dc *dc = hdmi->dc;
	unsigned long delay;

	cancel_delayed_work_sync(&hdmi->hpd_debounce_stable_wq);

	if (tegra_dc_hdmi_hpd(dc))
		delay = msecs_to_jiffies(100);
	else
		delay = msecs_to_jiffies(30);

	queue_delayed_work(system_nrt_wq, &hdmi->hpd_debounce_stable_wq, delay);
}

static irqreturn_t tegra_dc_hdmi_irq(int irq, void *ptr)
{
	struct tegra_dc *dc = ptr;
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	unsigned long flags;

	spin_lock_irqsave(&hdmi->suspend_lock, flags);
	if (hdmi->suspended)
		hdmi->hpd_pending = true;
	else
		tegra_dc_hdmi_enqueue_detect(dc);

	spin_unlock_irqrestore(&hdmi->suspend_lock, flags);

	return IRQ_HANDLED;
}

static void tegra_dc_hdmi_suspend(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	unsigned long flags;

	tegra_nvhdcp_suspend(hdmi->nvhdcp);
	spin_lock_irqsave(&hdmi->suspend_lock, flags);
	hdmi->suspended = true;
	spin_unlock_irqrestore(&hdmi->suspend_lock, flags);
}

static void tegra_dc_hdmi_resume(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	unsigned long flags;

	spin_lock_irqsave(&hdmi->suspend_lock, flags);
	hdmi->suspended = false;
	if (hdmi->hpd_pending) {
		tegra_dc_hdmi_enqueue_detect(dc);
		hdmi->hpd_pending = false;
	}
	spin_unlock_irqrestore(&hdmi->suspend_lock, flags);
}

static int tegra_dc_hdmi_init(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi;
	struct resource *res;
	struct resource *base_res;
	void __iomem *base;
	struct clk *clk = NULL;
	struct clk *disp1_clk = NULL;
	struct clk *disp2_clk = NULL;
	int err;

	hdmi = kzalloc(sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	res = nvhost_get_resource_byname(dc->ndev, IORESOURCE_MEM, "hdmi_regs");
	if (!res) {
		dev_err(&dc->ndev->dev, "hdmi: no mem resource\n");
		err = -ENOENT;
		goto err_free_hdmi;
	}

	base_res = request_mem_region(res->start, resource_size(res), dc->ndev->name);
	if (!base_res) {
		dev_err(&dc->ndev->dev, "hdmi: request_mem_region failed\n");
		err = -EBUSY;
		goto err_free_hdmi;
	}

	base = ioremap(res->start, resource_size(res));
	if (!base) {
		dev_err(&dc->ndev->dev, "hdmi: registers can't be mapped\n");
		err = -EBUSY;
		goto err_release_resource_reg;
	}

	clk = clk_get(&dc->ndev->dev, "hdmi");
	if (IS_ERR_OR_NULL(clk)) {
		dev_err(&dc->ndev->dev, "hdmi: can't get clock\n");
		err = -ENOENT;
		goto err_iounmap_reg;
	}

	disp1_clk = clk_get_sys("tegradc.0", NULL);
	if (IS_ERR_OR_NULL(disp1_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: can't disp1 clock\n");
		err = -ENOENT;
		goto err_put_clock;
	}

	disp2_clk = clk_get_sys("tegradc.1", NULL);
	if (IS_ERR_OR_NULL(disp2_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: can't disp2 clock\n");
		err = -ENOENT;
		goto err_put_clock;
	}

	/* TODO: support non-hotplug */
	if (request_irq(gpio_to_irq(dc->out->hotplug_gpio), tegra_dc_hdmi_irq,
			IRQF_DISABLED | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			dev_name(&dc->ndev->dev), dc)) {
		dev_err(&dc->ndev->dev, "hdmi: request_irq %d failed\n",
			gpio_to_irq(dc->out->hotplug_gpio));
		err = -EBUSY;
		goto err_put_clock;
	}
	enable_irq_wake(gpio_to_irq(dc->out->hotplug_gpio));

	hdmi->edid = tegra_edid_create(dc->out->dcc_bus);
	if (IS_ERR_OR_NULL(hdmi->edid)) {
		dev_err(&dc->ndev->dev, "hdmi: can't create edid\n");
		err = PTR_ERR(hdmi->edid);
		goto err_free_irq;
	}

	hdmi->nvhdcp = tegra_nvhdcp_create(hdmi, dc->ndev->id,
			dc->out->dcc_bus);
	if (IS_ERR_OR_NULL(hdmi->nvhdcp)) {
		dev_err(&dc->ndev->dev, "hdmi: can't create nvhdcp\n");
		err = PTR_ERR(hdmi->nvhdcp);
		goto err_edid_destroy;
	}

	INIT_WORK(&hdmi->hpd_debounce_edge_wq, tegra_dc_hdmi_hpd_debounce_edge);
	INIT_DELAYED_WORK(&hdmi->hpd_debounce_stable_wq,
		tegra_dc_hdmi_detect_worker);

	hdmi->dc = dc;
	hdmi->base = base;
	hdmi->base_res = base_res;
	hdmi->clk = clk;
	hdmi->disp1_clk = disp1_clk;
	hdmi->disp2_clk = disp2_clk;
	hdmi->suspended = false;
	hdmi->hpd_pending = false;
	spin_lock_init(&hdmi->suspend_lock);
	hdmi->audio_freq = 44100;

	dc->out->depth = 24;

	tegra_dc_set_outdata(dc, hdmi);

	/* boards can select default content protection policy */
	if (dc->out->flags & TEGRA_DC_OUT_NVHDCP_POLICY_ON_DEMAND) {
		tegra_nvhdcp_set_policy(hdmi->nvhdcp,
			TEGRA_NVHDCP_POLICY_ON_DEMAND);
	} else {
		tegra_nvhdcp_set_policy(hdmi->nvhdcp,
			TEGRA_NVHDCP_POLICY_ALWAYS_ON);
	}

	/* HDMI maximum is 165MHz per specification */
	if (!dc->out->max_pclk_khz || dc->out->max_pclk_khz > 165000)
		dc->out->max_pclk_khz = 165000;

	return 0;

err_edid_destroy:
	tegra_edid_destroy(hdmi->edid);
err_free_irq:
	disable_irq_wake(gpio_to_irq(dc->out->hotplug_gpio));
	free_irq(gpio_to_irq(dc->out->hotplug_gpio), dc);
err_put_clock:
	if (!IS_ERR_OR_NULL(disp2_clk))
		clk_put(disp2_clk);
	if (!IS_ERR_OR_NULL(disp1_clk))
		clk_put(disp1_clk);
	if (!IS_ERR_OR_NULL(clk))
		clk_put(clk);
err_iounmap_reg:
	iounmap(base);
err_release_resource_reg:
	release_resource(base_res);
err_free_hdmi:
	kfree(hdmi);
	return err;
}

static void tegra_dc_hdmi_destroy(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);

	disable_irq_wake(gpio_to_irq(dc->out->hotplug_gpio));
	free_irq(gpio_to_irq(dc->out->hotplug_gpio), dc);
	cancel_work_sync(&hdmi->hpd_debounce_edge_wq);
	cancel_delayed_work_sync(&hdmi->hpd_debounce_stable_wq);
	iounmap(hdmi->base);
	release_resource(hdmi->base_res);
	clk_put(hdmi->clk);
	clk_put(hdmi->disp1_clk);
	clk_put(hdmi->disp2_clk);
	tegra_edid_destroy(hdmi->edid);
	tegra_nvhdcp_destroy(hdmi->nvhdcp);

	kfree(hdmi);

}

static void tegra_dc_hdmi_setup_audio_fs_tables(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	int i;
	unsigned freqs[] = {
		32000,
		44100,
		48000,
		88200,
		96000,
		176400,
		192000,
        };

	for (i = 0; i < ARRAY_SIZE(freqs); i++) {
		unsigned f = freqs[i];
		unsigned eight_half;
		unsigned delta;;

		if (f > 96000)
			delta = 2;
		else if (f > 48000)
			delta = 6;
		else
			delta = 9;

		eight_half = (8 * HDMI_AUDIOCLK_FREQ) / (f * 128);
		tegra_hdmi_writel(hdmi, AUDIO_FS_LOW(eight_half - delta) |
				  AUDIO_FS_HIGH(eight_half + delta),
				  HDMI_NV_PDISP_AUDIO_FS(i));
	}
}

static int tegra_dc_hdmi_setup_audio(struct tegra_dc *dc, unsigned audio_freq)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	int ret;
	unsigned cts, n;
	unsigned long audio_n;

	tegra_hdmi_writel(hdmi,
			  AUDIO_CNTRL0_ERROR_TOLERANCE(6) |
			  AUDIO_CNTRL0_FRAMES_PER_BLOCK(0xc0) |
			  AUDIO_CNTRL0_SOURCE_SELECT_AUTO,
			  HDMI_NV_PDISP_AUDIO_CNTRL0);

	ret = tegra_hdmi_calc_audio_cts_n(audio_freq, dc->pll_rate,
					  dc->divider, &cts, &n);
	if (ret < 0) {
		dev_err(&dc->ndev->dev,
			"hdmi: can't set audio to %d at %d pix_clock",
			audio_freq, dc->mode.pclk);
		return -EINVAL;
	}

	tegra_hdmi_writel(hdmi, 0, HDMI_NV_PDISP_HDMI_ACR_CTRL);

	audio_n = AUDIO_N_RESETF | AUDIO_N_GENERATE_ALTERNALTE |
		AUDIO_N_VALUE(n - 1);
	tegra_hdmi_writel(hdmi, audio_n, HDMI_NV_PDISP_AUDIO_N);

	tegra_hdmi_writel(hdmi, ACR_SUBPACK_N(n) | ACR_ENABLE,
			  HDMI_NV_PDISP_HDMI_ACR_0441_SUBPACK_HIGH);

	tegra_hdmi_writel(hdmi, ACR_SUBPACK_CTS(cts),
			  HDMI_NV_PDISP_HDMI_ACR_0441_SUBPACK_LOW);

	tegra_hdmi_writel(hdmi, SPARE_HW_CTS | SPARE_FORCE_SW_CTS |
			  SPARE_CTS_RESET_VAL(1),
			  HDMI_NV_PDISP_HDMI_SPARE);

	audio_n &= ~AUDIO_N_RESETF;
	tegra_hdmi_writel(hdmi, audio_n, HDMI_NV_PDISP_AUDIO_N);

	tegra_dc_hdmi_setup_audio_fs_tables(dc);

	return 0;
}

int tegra_dc_hdmi_set_audio_sample_rate(unsigned audio_freq)
{
	int i, ret;
	struct tegra_dc *dc;
	struct tegra_dc_hdmi_data *hdmi;

	for (i = 0; i < TEGRA_MAX_DC; i++) {
		dc = tegra_dc_get_dc(i);
		if (!dc)
			continue;
		hdmi = tegra_dc_get_outdata(dc);
		if (!hdmi)
			continue;
		ret = tegra_dc_hdmi_setup_audio(dc, audio_freq);
		if (ret)
			return ret;
		hdmi->audio_freq = audio_freq;
	}

	return 0;
}
EXPORT_SYMBOL(tegra_dc_hdmi_set_audio_sample_rate);

static void tegra_dc_hdmi_write_infopack(struct tegra_dc *dc, int header_reg,
					 u8 type, u8 version, void *data, int len)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	u32 subpack[2];  /* extra byte for zero padding of subpack */
	int i;
	u8 csum;

	/* first byte of data is the checksum */
	csum = type + version + len - 1;
	for (i = 1; i < len; i++)
		csum +=((u8 *)data)[i];
	((u8 *)data)[0] = 0x100 - csum;

	tegra_hdmi_writel(hdmi, INFOFRAME_HEADER_TYPE(type) |
			  INFOFRAME_HEADER_VERSION(version) |
			  INFOFRAME_HEADER_LEN(len - 1),
			  header_reg);

	/* The audio inforame only has one set of subpack registers.  The hdmi
	 * block pads the rest of the data as per the spec so we have to fixup
	 * the length before filling in the subpacks.
	 */
	if (header_reg == HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_HEADER)
		len = 6;

	/* each subpack 7 bytes devided into:
	 *   subpack_low - bytes 0 - 3
	 *   subpack_high - bytes 4 - 6 (with byte 7 padded to 0x00)
	 */
	for (i = 0; i < len; i++) {
		int subpack_idx = i % 7;

		if (subpack_idx == 0)
			memset(subpack, 0x0, sizeof(subpack));

		((u8 *)subpack)[subpack_idx] = ((u8 *)data)[i];

		if (subpack_idx == 6 || (i + 1 == len)) {
			int reg = header_reg + 1 + (i / 7) * 2;

			tegra_hdmi_writel(hdmi, subpack[0], reg);
			tegra_hdmi_writel(hdmi, subpack[1], reg + 1);
		}
	}
}

static void tegra_dc_hdmi_setup_avi_infoframe(struct tegra_dc *dc, bool dvi)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	struct hdmi_avi_infoframe avi;

	if (dvi) {
		tegra_hdmi_writel(hdmi, 0x0,
				  HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_CTRL);
		return;
	}

	memset(&avi, 0x0, sizeof(avi));

	avi.r = HDMI_AVI_R_SAME;

	if (dc->mode.v_active == 480) {
		if (dc->mode.h_active == 640) {
			avi.m = HDMI_AVI_M_4_3;
			avi.vic = 1;
		} else {
			avi.m = HDMI_AVI_M_16_9;
			avi.vic = 3;
		}
	} else if (dc->mode.v_active == 576) {
		/* CEC modes 17 and 18 differ only by the pysical size of the
		 * screen so we have to calculation the physical aspect
		 * ratio.  4 * 10 / 3  is 13
		 */
		if ((dc->out->h_size * 10) / dc->out->v_size > 14) {
			avi.m = HDMI_AVI_M_16_9;
			avi.vic = 18;
		} else {
			avi.m = HDMI_AVI_M_16_9;
			avi.vic = 17;
		}
	} else if (dc->mode.v_active == 720) {
		avi.m = HDMI_AVI_M_16_9;
		if (dc->mode.h_front_porch == 110)
			avi.vic = 4; /* 60 Hz */
		else
			avi.vic = 19; /* 50 Hz */
	} else if (dc->mode.v_active == 1080) {
		avi.m = HDMI_AVI_M_16_9;
		if (dc->mode.h_front_porch == 88)
			avi.vic = 16; /* 60 Hz */
		else if (dc->mode.h_front_porch == 528)
			avi.vic = 31; /* 50 Hz */
		else
			avi.vic = 32; /* 24 Hz */
	} else {
		avi.m = HDMI_AVI_M_16_9;
		avi.vic = 0;
	}


	tegra_dc_hdmi_write_infopack(dc, HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_HEADER,
				     HDMI_INFOFRAME_TYPE_AVI,
				     HDMI_AVI_VERSION,
				     &avi, sizeof(avi));

	tegra_hdmi_writel(hdmi, INFOFRAME_CTRL_ENABLE,
			  HDMI_NV_PDISP_HDMI_AVI_INFOFRAME_CTRL);
}

static void tegra_dc_hdmi_setup_audio_infoframe(struct tegra_dc *dc, bool dvi)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	struct hdmi_audio_infoframe audio;

	if (dvi) {
		tegra_hdmi_writel(hdmi, 0x0,
				  HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_CTRL);
		return;
	}

	memset(&audio, 0x0, sizeof(audio));

	audio.cc = HDMI_AUDIO_CC_2;
	tegra_dc_hdmi_write_infopack(dc, HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_HEADER,
				     HDMI_INFOFRAME_TYPE_AUDIO,
				     HDMI_AUDIO_VERSION,
				     &audio, sizeof(audio));

	tegra_hdmi_writel(hdmi, INFOFRAME_CTRL_ENABLE,
			  HDMI_NV_PDISP_HDMI_AUDIO_INFOFRAME_CTRL);
}

static void tegra_dc_hdmi_setup_tdms(struct tegra_dc_hdmi_data *hdmi,
		const struct tdms_config *tc)
{
	tegra_hdmi_writel(hdmi, tc->pll0, HDMI_NV_PDISP_SOR_PLL0);
	tegra_hdmi_writel(hdmi, tc->pll1, HDMI_NV_PDISP_SOR_PLL1);

	tegra_hdmi_writel(hdmi, tc->pe_current, HDMI_NV_PDISP_PE_CURRENT);

	tegra_hdmi_writel(hdmi,
			  tc->drive_current | DRIVE_CURRENT_FUSE_OVERRIDE,
			  HDMI_NV_PDISP_SOR_LANE_DRIVE_CURRENT);
}

static void tegra_dc_hdmi_enable(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);
	int pulse_start;
	int dispclk_div_8_2;
	int retries;
	int rekey;
	int err;
	unsigned long val;
	int i;

	/* enbale power, clocks, resets, etc. */

	/* The upstream DC needs to be clocked for accesses to HDMI to not
	 * hard lock the system.  Because we don't know if HDMI is conencted
	 * to disp1 or disp2 we need to enable both until we set the DC mux.
	 */
	clk_enable(hdmi->disp1_clk);
	clk_enable(hdmi->disp2_clk);
	tegra_dc_setup_clk(dc, hdmi->clk);
	/*
	 * Bias the pixel clock upwards by 1Hz, although we've already adjusted
	 * it to be a value that we can drive.  The Tegra clock code by design
	 * always rounds the divider up (i.e., it rounds down to the next
	 * available rate).  For rates that aren't exactly divisible by the
	 * divider such as 1 GHz / 7 == 166666666.66...Hz, passing the
	 * truncated value 166666666 to the clock API will give us 1 GHz / 6,
	 * which is not even close to what we want.
	 *
	 * Adding 1 here will ensure that the clock API will use the desired
	 * divider.
	 */
	clk_set_rate(hdmi->clk, dc->mode.pclk + 1);

	clk_enable(hdmi->clk);
	tegra_periph_reset_assert(hdmi->clk);
	mdelay(1);
	tegra_periph_reset_deassert(hdmi->clk);

	/* TODO: copy HDCP keys from KFUSE to HDMI */

	/* Program display timing registers: handled by dc */

	/* program HDMI registers and SOR sequencer */

	tegra_dc_writel(dc, VSYNC_H_POSITION(1), DC_DISP_DISP_TIMING_OPTIONS);
	tegra_dc_writel(dc, DITHER_CONTROL_DISABLE | BASE_COLOR_SIZE888,
			DC_DISP_DISP_COLOR_CONTROL);

	/* video_preamble uses h_pulse2 */
	pulse_start = dc->mode.h_ref_to_sync + dc->mode.h_sync_width +
		dc->mode.h_back_porch - 10;
	tegra_dc_writel(dc, H_PULSE_2_ENABLE, DC_DISP_DISP_SIGNAL_OPTIONS0);
	tegra_dc_writel(dc,
			PULSE_MODE_NORMAL |
			PULSE_POLARITY_HIGH |
			PULSE_QUAL_VACTIVE |
			PULSE_LAST_END_A,
			DC_DISP_H_PULSE2_CONTROL);
	tegra_dc_writel(dc, PULSE_START(pulse_start) | PULSE_END(pulse_start + 8),
		  DC_DISP_H_PULSE2_POSITION_A);

	tegra_hdmi_writel(hdmi,
			  VSYNC_WINDOW_END(0x210) |
			  VSYNC_WINDOW_START(0x200) |
			  VSYNC_WINDOW_ENABLE,
			  HDMI_NV_PDISP_HDMI_VSYNC_WINDOW);

	tegra_hdmi_writel(hdmi,
			  (dc->ndev->id ? HDMI_SRC_DISPLAYB : HDMI_SRC_DISPLAYA) |
			  ARM_VIDEO_RANGE_LIMITED,
			  HDMI_NV_PDISP_INPUT_CONTROL);

	clk_disable(hdmi->disp1_clk);
	clk_disable(hdmi->disp2_clk);

	dispclk_div_8_2 = clk_get_rate(hdmi->clk) / 1000000 * 4;
	tegra_hdmi_writel(hdmi,
			  SOR_REFCLK_DIV_INT(dispclk_div_8_2 >> 2) |
			  SOR_REFCLK_DIV_FRAC(dispclk_div_8_2),
			  HDMI_NV_PDISP_SOR_REFCLK);

	if (!hdmi->dvi) {
		err = tegra_dc_hdmi_setup_audio(dc, hdmi->audio_freq);

		if (err < 0)
			hdmi->dvi = true;
	}

	rekey = HDMI_REKEY_DEFAULT;
	val = HDMI_CTRL_REKEY(rekey);
	val |= HDMI_CTRL_MAX_AC_PACKET((dc->mode.h_sync_width +
					dc->mode.h_back_porch +
					dc->mode.h_front_porch -
					rekey - 18) / 32);
	if (!hdmi->dvi)
		val |= HDMI_CTRL_ENABLE;
	tegra_hdmi_writel(hdmi, val, HDMI_NV_PDISP_HDMI_CTRL);

	if (hdmi->dvi)
		tegra_hdmi_writel(hdmi, 0x0,
				  HDMI_NV_PDISP_HDMI_GENERIC_CTRL);
	else
		tegra_hdmi_writel(hdmi, GENERIC_CTRL_AUDIO,
				  HDMI_NV_PDISP_HDMI_GENERIC_CTRL);


	tegra_dc_hdmi_setup_avi_infoframe(dc, hdmi->dvi);
	tegra_dc_hdmi_setup_audio_infoframe(dc, hdmi->dvi);

	/* TMDS CONFIG */
	for (i = 0; i < ARRAY_SIZE(tdms_config); i++)
		if (dc->mode.pclk <= tdms_config[i].pclk) {
			tegra_dc_hdmi_setup_tdms(hdmi, &tdms_config[i]);
			break;
		}

	tegra_hdmi_writel(hdmi,
			  SOR_SEQ_CTL_PU_PC(0) |
			  SOR_SEQ_PU_PC_ALT(0) |
			  SOR_SEQ_PD_PC(8) |
			  SOR_SEQ_PD_PC_ALT(8),
			  HDMI_NV_PDISP_SOR_SEQ_CTL);

	val = SOR_SEQ_INST_WAIT_TIME(1) |
		SOR_SEQ_INST_WAIT_UNITS_VSYNC |
		SOR_SEQ_INST_HALT |
		SOR_SEQ_INST_PIN_A_LOW |
		SOR_SEQ_INST_PIN_B_LOW |
		SOR_SEQ_INST_DRIVE_PWM_OUT_LO;

	tegra_hdmi_writel(hdmi, val, HDMI_NV_PDISP_SOR_SEQ_INST0);
	tegra_hdmi_writel(hdmi, val, HDMI_NV_PDISP_SOR_SEQ_INST8);

	val = 0x1c800;
	val &= ~SOR_CSTM_ROTCLK(~0);
	val |= SOR_CSTM_ROTCLK(2);
	tegra_hdmi_writel(hdmi, val, HDMI_NV_PDISP_SOR_CSTM);


	tegra_dc_writel(dc, DISP_CTRL_MODE_STOP, DC_CMD_DISPLAY_COMMAND);
	tegra_dc_writel(dc, GENERAL_ACT_REQ << 8, DC_CMD_STATE_CONTROL);
	tegra_dc_writel(dc, GENERAL_ACT_REQ, DC_CMD_STATE_CONTROL);


	/* start SOR */
	tegra_hdmi_writel(hdmi,
			  SOR_PWR_NORMAL_STATE_PU |
			  SOR_PWR_NORMAL_START_NORMAL |
			  SOR_PWR_SAFE_STATE_PD |
			  SOR_PWR_SETTING_NEW_TRIGGER,
			  HDMI_NV_PDISP_SOR_PWR);
	tegra_hdmi_writel(hdmi,
			  SOR_PWR_NORMAL_STATE_PU |
			  SOR_PWR_NORMAL_START_NORMAL |
			  SOR_PWR_SAFE_STATE_PD |
			  SOR_PWR_SETTING_NEW_DONE,
			  HDMI_NV_PDISP_SOR_PWR);

	retries = 1000;
	do {
		BUG_ON(--retries < 0);
		val = tegra_hdmi_readl(hdmi, HDMI_NV_PDISP_SOR_PWR);
	} while (val & SOR_PWR_SETTING_NEW_PENDING);

	val = SOR_STATE_ASY_CRCMODE_COMPLETE |
		SOR_STATE_ASY_OWNER_HEAD0 |
		SOR_STATE_ASY_SUBOWNER_BOTH |
		SOR_STATE_ASY_PROTOCOL_SINGLE_TMDS_A |
		SOR_STATE_ASY_DEPOL_POS;

	if (dc->mode.flags & TEGRA_DC_MODE_FLAG_NEG_H_SYNC)
		val |= SOR_STATE_ASY_HSYNCPOL_NEG;
	else
		val |= SOR_STATE_ASY_HSYNCPOL_POS;

	if (dc->mode.flags & TEGRA_DC_MODE_FLAG_NEG_V_SYNC)
		val |= SOR_STATE_ASY_VSYNCPOL_NEG;
	else
		val |= SOR_STATE_ASY_VSYNCPOL_POS;

	tegra_hdmi_writel(hdmi, val, HDMI_NV_PDISP_SOR_STATE2);

	val = SOR_STATE_ASY_HEAD_OPMODE_AWAKE | SOR_STATE_ASY_ORMODE_NORMAL;
	tegra_hdmi_writel(hdmi, val, HDMI_NV_PDISP_SOR_STATE1);

	tegra_hdmi_writel(hdmi, 0, HDMI_NV_PDISP_SOR_STATE0);
	tegra_hdmi_writel(hdmi, SOR_STATE_UPDATE, HDMI_NV_PDISP_SOR_STATE0);
	tegra_hdmi_writel(hdmi, val | SOR_STATE_ATTACHED,
			  HDMI_NV_PDISP_SOR_STATE1);
	tegra_hdmi_writel(hdmi, 0, HDMI_NV_PDISP_SOR_STATE0);

	tegra_dc_writel(dc, HDMI_ENABLE, DC_DISP_DISP_WIN_OPTIONS);

	tegra_dc_writel(dc, PW0_ENABLE | PW1_ENABLE | PW2_ENABLE | PW3_ENABLE |
			PW4_ENABLE | PM0_ENABLE | PM1_ENABLE,
			DC_CMD_DISPLAY_POWER_CONTROL);

	tegra_dc_writel(dc, DISP_CTRL_MODE_C_DISPLAY, DC_CMD_DISPLAY_COMMAND);
	tegra_dc_writel(dc, GENERAL_ACT_REQ << 8, DC_CMD_STATE_CONTROL);
	tegra_dc_writel(dc, GENERAL_ACT_REQ, DC_CMD_STATE_CONTROL);

	if (!hdmi->dvi)
		tegra_nvhdcp_set_plug(hdmi->nvhdcp, 1);
}

static void tegra_dc_hdmi_disable(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi = tegra_dc_get_outdata(dc);

	tegra_nvhdcp_set_plug(hdmi->nvhdcp, 0);

	tegra_periph_reset_assert(hdmi->clk);
	clk_disable(hdmi->clk);
	tegra_dvfs_set_rate(hdmi->clk, 0);
}

struct tegra_dc_out_ops tegra_dc_hdmi_ops = {
	.init = tegra_dc_hdmi_init,
	.destroy = tegra_dc_hdmi_destroy,
	.enable = tegra_dc_hdmi_enable,
	.disable = tegra_dc_hdmi_disable,
	.detect = tegra_dc_hdmi_enqueue_detect,
	.suspend = tegra_dc_hdmi_suspend,
	.resume = tegra_dc_hdmi_resume,
};

struct tegra_dc_edid *tegra_dc_get_edid(struct tegra_dc *dc)
{
	struct tegra_dc_hdmi_data *hdmi;

	/* TODO: Support EDID on non-HDMI devices */
	if (dc->out->type != TEGRA_DC_OUT_HDMI)
		return ERR_PTR(-ENODEV);

	hdmi = tegra_dc_get_outdata(dc);

	return tegra_edid_get_data(hdmi->edid);
}
EXPORT_SYMBOL(tegra_dc_get_edid);

void tegra_dc_put_edid(struct tegra_dc_edid *edid)
{
	tegra_edid_put_data(edid);
}
EXPORT_SYMBOL(tegra_dc_put_edid);
