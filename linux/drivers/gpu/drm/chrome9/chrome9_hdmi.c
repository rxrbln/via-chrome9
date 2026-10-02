// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 René Rebe <rene@exactco.de>
 *
 * VX900 integrated HDMI transmitter: the integrated TMDS transmitter fed
 * through DVP1 and the combined DisplayPort/HDMI PHY in MMIO 0xC610-0xC7D4,
 * with its dedicated hardware DDC master (0xC000, 0xC0B4-0xC0C4).  Register
 * semantics follow the VT3410 VBIOS as reverse engineered for the
 * xf86-video-openchrome HDMI support (vx900-hdmi*.patch in T2).
 */

#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/vgaarb.h>
#include <video/vga.h>

#include <drm/drm_atomic.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_encoder.h>

#include "chrome9_drv.h"
#include "chrome9_hdmi.h"

/* DP/HDMI controller master enable. */
#define HDMI_CONTROL		0xC280
#define HDMI_CONTROL_ENABLE	(BIT(1) | BIT(6))
/* PHY configuration: bits [29:18] and 0 survive a modeset. */
#define HDMI_PHY_CFG		0xC618
#define HDMI_PHY_CFG_KEEP	0x3FFC0001
/* PLL output stage enable. */
#define HDMI_PLL_AUX		0xC640
#define HDMI_PLL_AUX_EN		0x09
/* TMDS clock: [31:30] range, [26:25]+[2:1] PLL/bias enable and lock, [0]
 * PLL enable; the DDC bit clock divider sits in the low bits. */
#define HDMI_CLOCK		0xC740
#define HDMI_CLOCK_RANGE	GENMASK(31, 30)
#define HDMI_CLOCK_PLL_EN	BIT(0)
#define HDMI_CLOCK_LOCK_KEEP	0xF9FFFFF9
#define HDMI_CLOCK_LOCK_EN	0x06000006
#define HDMI_CLOCK_PLL_RUN	0x06000000
#define HDMI_CLOCK_DDC_KEEP	0xFFF800E7
#define HDMI_CLOCK_DDC_DIV	0x00029E18
/* PHY calibration pattern [31:24]. */
#define HDMI_PATTERN		0xC744
#define HDMI_PATTERN_KEEP	0x00FFFFFF
#define HDMI_PATTERN_CALIB	0x55000000
/* PHY bias, pulsed [15:14] while the PLL locks. */
#define HDMI_BIAS		0xC748
#define HDMI_BIAS_KEEP		0x00003FFF
/* Transmitter output enable. */
#define HDMI_OUTPUT		0xC74C
#define HDMI_OUTPUT_ENABLE	BIT(6)

/* Hardware DDC master. */
#define DDC_ENABLE		0xC000
#define DDC_DATA		0xC0B4
#define DDC_TX_SHIFT		16
#define DDC_TX_MASK		GENMASK(23, 16)
#define DDC_RX_SHIFT		8
#define DDC_RX_MASK		GENMASK(15, 8)
#define DDC_CMD			0xC0B8
#define DDC_CMD_INIT		0x01
#define DDC_CMD_START_PREP	0x11
#define DDC_CMD_START		0x19
#define DDC_CMD_XFER		0x09
#define DDC_CMD_STOP_PREP	0x21
#define DDC_CMD_STOP		0x29
#define DDC_BUSY_BYTE		0x08
#define DDC_BUSY_START		0x18
#define DDC_BUSY_STOP		0x828
#define DDC_RX_READY		0x80
#define DDC_CFG			0xC0C4
#define DDC_CFG_MASK		GENMASK(25, 23)
#define DDC_CFG_VALUE		BIT(23)

struct chrome9_hdmi {
	struct chrome9_device *cdev;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct i2c_adapter ddc;
	/* Serializes the DDC master and PHY clock register users. */
	struct mutex lock;
	bool enabled;
};

static u32 hdmi_read(struct chrome9_hdmi *hdmi, u32 reg)
{
	return readl(hdmi->cdev->mmio + reg);
}

static void hdmi_write(struct chrome9_hdmi *hdmi, u32 reg, u32 value)
{
	writel(value, hdmi->cdev->mmio + reg);
}

static void hdmi_mask(struct chrome9_hdmi *hdmi, u32 reg, u32 mask, u32 value)
{
	hdmi_write(hdmi, reg, (hdmi_read(hdmi, reg) & ~mask) | (value & mask));
}

/* Enable and lock the PHY PLL (VBIOS 0x9D19 -> 0x9D64 -> 0x9D74). */
static void chrome9_hdmi_pll_enable(struct chrome9_hdmi *hdmi)
{
	u32 clock;

	hdmi_mask(hdmi, HDMI_CLOCK, HDMI_CLOCK_PLL_EN, HDMI_CLOCK_PLL_EN);
	udelay(1);
	clock = hdmi_read(hdmi, HDMI_CLOCK);
	if ((clock & HDMI_CLOCK_LOCK_EN) == HDMI_CLOCK_LOCK_EN)
		return;
	hdmi_write(hdmi, HDMI_CLOCK,
		   (clock & HDMI_CLOCK_LOCK_KEEP) | HDMI_CLOCK_LOCK_EN);
	hdmi_mask(hdmi, HDMI_BIAS, ~HDMI_BIAS_KEEP, BIT(14));
	hdmi_mask(hdmi, HDMI_BIAS, ~HDMI_BIAS_KEEP, BIT(15));
}

static int chrome9_hdmi_ddc_wait(struct chrome9_hdmi *hdmi, u32 mask, bool set)
{
	u32 status;

	return readl_poll_timeout(hdmi->cdev->mmio + DDC_CMD, status,
				  set ? (status & mask) : !(status & mask),
				  10, 4000);
}

static int chrome9_hdmi_ddc_send(struct chrome9_hdmi *hdmi, u8 byte)
{
	hdmi_mask(hdmi, DDC_DATA, DDC_TX_MASK, (u32)byte << DDC_TX_SHIFT);
	hdmi_write(hdmi, DDC_CMD, DDC_CMD_XFER);
	return chrome9_hdmi_ddc_wait(hdmi, DDC_BUSY_BYTE, false);
}

static int chrome9_hdmi_ddc_start(struct chrome9_hdmi *hdmi)
{
	hdmi_write(hdmi, DDC_CMD, DDC_CMD_START_PREP);
	hdmi_write(hdmi, DDC_CMD, DDC_CMD_START);
	return chrome9_hdmi_ddc_wait(hdmi, DDC_BUSY_START, false);
}

static int chrome9_hdmi_ddc_msg(struct chrome9_hdmi *hdmi,
				const struct i2c_msg *msg)
{
	const bool read = msg->flags & I2C_M_RD;
	int ret;

	ret = chrome9_hdmi_ddc_start(hdmi);
	if (!ret)
		ret = chrome9_hdmi_ddc_send(hdmi, (msg->addr << 1) | read);
	for (u16 i = 0; !ret && i < msg->len; i++) {
		if (!read) {
			ret = chrome9_hdmi_ddc_send(hdmi, msg->buf[i]);
			continue;
		}
		hdmi_write(hdmi, DDC_CMD, DDC_CMD_XFER);
		ret = chrome9_hdmi_ddc_wait(hdmi, DDC_BUSY_BYTE, false);
		if (!ret)
			ret = chrome9_hdmi_ddc_wait(hdmi, DDC_RX_READY, true);
		if (ret)
			break;
		msg->buf[i] = (hdmi_read(hdmi, DDC_DATA) & DDC_RX_MASK) >>
			      DDC_RX_SHIFT;
		/* Acknowledge all but the last byte to continue the burst. */
		if (i + 1 < msg->len)
			hdmi_mask(hdmi, DDC_CMD, DDC_RX_READY, 0);
	}
	return ret;
}

/*
 * The DDC master is clocked by the PHY PLL: when no mode runs on the PHY,
 * bring the PLL up with the DDC divider for the transfer and put the clock
 * and bias registers back exactly afterwards (VBIOS 0x9855 -> 0x9C42).
 */
static int chrome9_hdmi_ddc_xfer(struct i2c_adapter *adapter,
				 struct i2c_msg *msgs, int num)
{
	struct chrome9_hdmi *hdmi = i2c_get_adapdata(adapter);
	u32 saved_clock, saved_bias, saved_enable;
	bool pll_off;
	int ret = 0;
	int i;

	mutex_lock(&hdmi->lock);
	saved_clock = hdmi_read(hdmi, HDMI_CLOCK);
	saved_bias = hdmi_read(hdmi, HDMI_BIAS);
	pll_off = !(saved_clock & HDMI_CLOCK_PLL_EN);
	if (pll_off) {
		chrome9_hdmi_pll_enable(hdmi);
		hdmi_mask(hdmi, HDMI_CLOCK, ~HDMI_CLOCK_DDC_KEEP,
			  HDMI_CLOCK_DDC_DIV);
		udelay(1);
	}
	saved_enable = hdmi_read(hdmi, DDC_ENABLE);
	hdmi_mask(hdmi, DDC_ENABLE, BIT(0), BIT(0));
	hdmi_mask(hdmi, DDC_CFG, DDC_CFG_MASK, DDC_CFG_VALUE);
	hdmi_write(hdmi, DDC_CMD, DDC_CMD_INIT);

	for (i = 0; !ret && i < num; i++)
		ret = chrome9_hdmi_ddc_msg(hdmi, &msgs[i]);

	/* STOP regardless of the outcome. */
	hdmi_mask(hdmi, DDC_CMD, DDC_RX_READY, 0);
	hdmi_write(hdmi, DDC_CMD, DDC_CMD_STOP_PREP);
	hdmi_write(hdmi, DDC_CMD, DDC_CMD_STOP);
	chrome9_hdmi_ddc_wait(hdmi, DDC_BUSY_STOP, false);
	hdmi_mask(hdmi, DDC_ENABLE, BIT(0), saved_enable);
	if (pll_off) {
		hdmi_write(hdmi, HDMI_BIAS, saved_bias);
		hdmi_write(hdmi, HDMI_CLOCK, saved_clock);
	}
	mutex_unlock(&hdmi->lock);
	return ret ? -ENXIO : num;
}

static u32 chrome9_hdmi_ddc_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm chrome9_hdmi_ddc_algo = {
	.master_xfer = chrome9_hdmi_ddc_xfer,
	.functionality = chrome9_hdmi_ddc_func,
};

static void chrome9_sr_mask(u8 index, u8 mask, u8 value)
{
	vga_io_wseq(index, (vga_io_rseq(index) & ~mask) | (value & mask));
}

static void chrome9_cr_mask(u8 index, u8 mask, u8 value)
{
	vga_io_wcrt(index, (vga_io_rcrt(index) & ~mask) | (value & mask));
}

/* TMDS clock range for a pixel clock in kHz (VBIOS thresholds). */
static u32 chrome9_hdmi_clock_range(int clock)
{
	if (clock > 135000)
		return 0;
	if (clock > 67500)
		return 1;
	if (clock > 33750)
		return 2;
	return 3;
}

/* DVP1 pads (SR1E[5:4]), with the extended sequencer bank (SR5A[0])
 * cleared while they are programmed. */
static void chrome9_hdmi_pads(bool on)
{
	u8 sr5a = vga_io_rseq(0x5a);

	vga_io_wseq(0x5a, sr5a & ~BIT(0));
	if (!(vga_io_rseq(0x13) & BIT(6)))
		chrome9_sr_mask(0x1e, 0x30, on ? 0x30 : 0);
	vga_io_wseq(0x5a, sr5a);
}

/* DVP1 routing of IGA1 to the integrated TMDS transmitter. */
static void chrome9_hdmi_route(const struct drm_display_mode *mode)
{
	u8 sr5a = vga_io_rseq(0x5a);
	u8 polarity = 0;

	vga_io_wseq(0x5a, sr5a & ~BIT(0));
	if (!(vga_io_rseq(0x13) & BIT(6))) {
		/* CR9B[4]: DVP1 source IGA1; SR65: highest clock and data drive. */
		chrome9_cr_mask(0x9b, BIT(4), 0);
		chrome9_sr_mask(0x65, 0x0f, 0x0f);
	}
	vga_io_wseq(0x5a, sr5a);
	/* CR99[4]: TMDS source IGA1; CR97[6:5]: VSYNC/HSYNC negative. */
	chrome9_cr_mask(0x99, BIT(4), 0);
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		polarity |= BIT(5);
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		polarity |= BIT(6);
	chrome9_cr_mask(0x97, BIT(6) | BIT(5), polarity);
	/* Clear a stale DVI sense interrupt (SR2B[6], write one). */
	chrome9_sr_mask(0x2b, BIT(6), BIT(6));
}

/* PHY power up for a pixel clock (VBIOS 0xAD06, 0x98FD, 0x996E, 0x9D36). */
static void chrome9_hdmi_phy_on(struct chrome9_hdmi *hdmi, int clock)
{
	chrome9_hdmi_pll_enable(hdmi);
	hdmi_mask(hdmi, HDMI_CONTROL, HDMI_CONTROL_ENABLE, HDMI_CONTROL_ENABLE);
	hdmi_write(hdmi, HDMI_PHY_CFG,
		   hdmi_read(hdmi, HDMI_PHY_CFG) & HDMI_PHY_CFG_KEEP);
	hdmi_mask(hdmi, HDMI_OUTPUT, HDMI_OUTPUT_ENABLE, HDMI_OUTPUT_ENABLE);
	/* Like openchrome: stop the PLL, set the TMDS range, restart it. */
	hdmi_mask(hdmi, HDMI_CLOCK, HDMI_CLOCK_PLL_RUN, 0);
	udelay(1);
	hdmi_mask(hdmi, HDMI_CLOCK, HDMI_CLOCK_RANGE,
		  chrome9_hdmi_clock_range(clock) << 30);
	hdmi_mask(hdmi, HDMI_CLOCK, HDMI_CLOCK_PLL_RUN, HDMI_CLOCK_PLL_RUN);
	udelay(1);
	hdmi_mask(hdmi, HDMI_PATTERN, ~HDMI_PATTERN_KEEP, HDMI_PATTERN_CALIB);
	udelay(5);
	hdmi_mask(hdmi, HDMI_PATTERN, ~HDMI_PATTERN_KEEP, 0);
	hdmi_mask(hdmi, HDMI_PLL_AUX, HDMI_PLL_AUX_EN, HDMI_PLL_AUX_EN);
}

static void chrome9_hdmi_phy_off(struct chrome9_hdmi *hdmi)
{
	hdmi_mask(hdmi, HDMI_OUTPUT, HDMI_OUTPUT_ENABLE, 0);
	hdmi_mask(hdmi, HDMI_CONTROL, HDMI_CONTROL_ENABLE, 0);
}

static void chrome9_hdmi_encoder_enable(struct drm_encoder *encoder,
					struct drm_atomic_commit *state)
{
	struct chrome9_hdmi *hdmi = container_of(encoder, struct chrome9_hdmi,
						 encoder);
	struct chrome9_device *cdev = hdmi->cdev;
	const struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, encoder->crtc);
	const struct drm_display_mode *mode = &crtc_state->adjusted_mode;

	if (chrome9_vga_seq_get(cdev)) {
		dev_err(&cdev->pdev->dev, "cannot lock VGA I/O for HDMI\n");
		return;
	}
	chrome9_hdmi_route(mode);
	chrome9_vga_seq_put(cdev);
	mutex_lock(&hdmi->lock);
	chrome9_hdmi_phy_on(hdmi, mode->clock);
	hdmi->enabled = true;
	mutex_unlock(&hdmi->lock);
	/* The pads drive TMDS only once the PHY PLL runs at the new range. */
	if (chrome9_vga_seq_get(cdev)) {
		dev_err(&cdev->pdev->dev, "cannot lock VGA I/O for HDMI\n");
		return;
	}
	chrome9_hdmi_pads(true);
	chrome9_vga_seq_put(cdev);
	dev_info(&cdev->pdev->dev, "HDMI %ux%u@%u kHz on\n", mode->hdisplay,
		 mode->vdisplay, mode->clock);
}

static void chrome9_hdmi_encoder_disable(struct drm_encoder *encoder,
					 struct drm_atomic_commit *state)
{
	struct chrome9_hdmi *hdmi = container_of(encoder, struct chrome9_hdmi,
						 encoder);
	struct chrome9_device *cdev = hdmi->cdev;

	if (!chrome9_vga_seq_get(cdev)) {
		chrome9_hdmi_pads(false);
		chrome9_vga_seq_put(cdev);
	}
	mutex_lock(&hdmi->lock);
	chrome9_hdmi_phy_off(hdmi);
	hdmi->enabled = false;
	mutex_unlock(&hdmi->lock);
}

static const struct drm_encoder_helper_funcs chrome9_hdmi_encoder_helpers = {
	.atomic_enable = chrome9_hdmi_encoder_enable,
	.atomic_disable = chrome9_hdmi_encoder_disable,
};

static int chrome9_hdmi_get_modes(struct drm_connector *connector)
{
	const struct drm_edid *drm_edid = drm_edid_read(connector);
	int count;

	drm_edid_connector_update(connector, drm_edid);
	count = drm_edid_connector_add_modes(connector);
	drm_edid_free(drm_edid);
	return count;
}

static enum drm_mode_status
chrome9_hdmi_mode_valid(struct drm_connector *connector,
			const struct drm_display_mode *mode)
{
	return mode->clock <= 165000 ? MODE_OK : MODE_CLOCK_HIGH;
}

static const struct drm_connector_helper_funcs chrome9_hdmi_connector_helpers = {
	.get_modes = chrome9_hdmi_get_modes,
	.mode_valid = chrome9_hdmi_mode_valid,
};

/* A sink answers the EDID header read. */
static enum drm_connector_status
chrome9_hdmi_detect(struct drm_connector *connector, bool force)
{
	struct chrome9_hdmi *hdmi = container_of(connector, struct chrome9_hdmi,
						 connector);

	return drm_probe_ddc(&hdmi->ddc) ? connector_status_connected :
					   connector_status_disconnected;
}

static const struct drm_connector_funcs chrome9_hdmi_connector_funcs = {
	.detect = chrome9_hdmi_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* PHY, DDC and routing registers, for bring-up. */
static int chrome9_hdmi_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct chrome9_device *cdev = to_chrome9(entry->dev);
	struct chrome9_hdmi *hdmi = entry->file.data;
	static const u32 mmio[] = {
		HDMI_CONTROL, HDMI_PHY_CFG, HDMI_PLL_AUX, HDMI_CLOCK,
		HDMI_PATTERN, HDMI_BIAS, HDMI_OUTPUT, DDC_ENABLE, DDC_DATA,
		DDC_CMD, DDC_CFG,
	};
	static const u8 sr[] = { 0x12, 0x13, 0x1e, 0x2b, 0x5a, 0x65 };
	static const u8 cr[] = { 0x97, 0x99, 0x9b, 0xd2 };
	u8 sr5a;

	mutex_lock(&hdmi->lock);
	for (unsigned int i = 0; i < ARRAY_SIZE(mmio); i++)
		seq_printf(m, "mmio %04x: %08x\n", mmio[i],
			   hdmi_read(hdmi, mmio[i]));
	seq_printf(m, "enabled: %u\n", hdmi->enabled);
	mutex_unlock(&hdmi->lock);
	if (chrome9_vga_seq_get(cdev))
		return -EBUSY;
	sr5a = vga_io_rseq(0x5a);
	vga_io_wseq(0x5a, sr5a & ~BIT(0));
	for (unsigned int i = 0; i < ARRAY_SIZE(sr); i++)
		seq_printf(m, "SR%02x: %02x\n", sr[i],
			   sr[i] == 0x5a ? sr5a : vga_io_rseq(sr[i]));
	vga_io_wseq(0x5a, sr5a);
	for (unsigned int i = 0; i < ARRAY_SIZE(cr); i++)
		seq_printf(m, "CR%02x: %02x\n", cr[i], vga_io_rcrt(cr[i]));
	chrome9_vga_seq_put(cdev);
	return 0;
}

static void chrome9_hdmi_release(struct drm_device *drm, void *res)
{
	struct chrome9_hdmi *hdmi = res;

	i2c_del_adapter(&hdmi->ddc);
}

int chrome9_hdmi_init(struct chrome9_device *cdev, struct drm_crtc *crtc,
		      struct drm_encoder *clone)
{
	struct drm_device *drm = &cdev->drm;
	struct chrome9_hdmi *hdmi;
	int ret;

	if (pci_resource_len(cdev->pdev, 1) <= HDMI_OUTPUT)
		return -ENODEV;
	hdmi = drmm_kzalloc(drm, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;
	hdmi->cdev = cdev;
	mutex_init(&hdmi->lock);

	hdmi->ddc.owner = THIS_MODULE;
	hdmi->ddc.algo = &chrome9_hdmi_ddc_algo;
	hdmi->ddc.dev.parent = &cdev->pdev->dev;
	hdmi->ddc.retries = 2;
	strscpy(hdmi->ddc.name, "Chrome9 HDMI DDC", sizeof(hdmi->ddc.name));
	i2c_set_adapdata(&hdmi->ddc, hdmi);
	ret = i2c_add_adapter(&hdmi->ddc);
	if (ret)
		return ret;
	ret = drmm_add_action_or_reset(drm, chrome9_hdmi_release, hdmi);
	if (ret)
		return ret;

	/* Managed: cleaned up before the drmm allocation holding them. */
	ret = drmm_encoder_init(drm, &hdmi->encoder, NULL,
				DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	drm_encoder_helper_add(&hdmi->encoder, &chrome9_hdmi_encoder_helpers);
	hdmi->encoder.possible_crtcs = drm_crtc_mask(crtc);
	hdmi->encoder.possible_clones = drm_encoder_mask(&hdmi->encoder) |
					drm_encoder_mask(clone);
	clone->possible_clones |= drm_encoder_mask(clone) |
				  drm_encoder_mask(&hdmi->encoder);

	drm_connector_helper_add(&hdmi->connector,
				 &chrome9_hdmi_connector_helpers);
	ret = drmm_connector_init(drm, &hdmi->connector,
				  &chrome9_hdmi_connector_funcs,
				  DRM_MODE_CONNECTOR_HDMIA, &hdmi->ddc);
	if (ret)
		return ret;
	drm_debugfs_add_file(drm, "chrome9_hdmi", chrome9_hdmi_show, hdmi);
	hdmi->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				 DRM_CONNECTOR_POLL_DISCONNECT;
	return drm_connector_attach_encoder(&hdmi->connector, &hdmi->encoder);
}
