// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/i2c-algo-bit.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vgaarb.h>

#include <video/vga.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "chrome9_drv.h"
#include "chrome9_engine.h"
#include "chrome9_vx900_display.h"

#define CHROME9_FIXED_WIDTH		1024U
#define CHROME9_FIXED_HEIGHT		768U
#define CHROME9_FIXED_PITCH		4096U
#define CHROME9_FIXED_SIZE		(CHROME9_FIXED_PITCH * \
					 CHROME9_FIXED_HEIGHT)

static bool chrome9_direct_flips;
module_param_named(direct_flips, chrome9_direct_flips, bool, 0400);
MODULE_PARM_DESC(direct_flips,
		 "Enable experimental direct IGA1 SF page flips");

/* Every indexed register written by chrome9_iga1_program(). */
static const u8 chrome9_saved_sr[] = {
	0x01, 0x02, 0x03, 0x04, 0x10, 0x15, 0x16, 0x17, 0x18,
	0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x22, 0x2a, 0x31,
	0x2d, 0x40, 0x44, 0x45, 0x46, 0x58, 0x59,
};

static const u8 chrome9_saved_cr[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
	0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x32, 0x33, 0x34,
	0x35, 0x36, 0x45, 0x47, 0x48, 0x69, 0x6a, 0x6b, 0x6c,
	0x88, 0x89, 0x8a, 0x91, 0x96, 0x97, 0x99, 0x9b, 0xd2, 0xd4,
};

static const u8 chrome9_vpit_gfx[VGA_GFX_C] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0f, 0xff,
};

static const u8 chrome9_vpit_attr[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x01, 0x00, 0x0f, 0x00,
};

struct chrome9_kms_state {
	/* Serializes ownership of the saved legacy VGA state. */
	struct mutex lock;
	bool fixed_iga1;
	bool staging_scanout;
	bool snapshot_valid;
	bool iga1_enabled;
	bool flip_blocked;
	/* Owns a second SF pin until a complete IGA1 restore stops fetch. */
	struct drm_framebuffer *active_fb;
	/* A blanked old scanout remains pinned through the next base takeover. */
	struct drm_framebuffer *retired_fb;
	/* A failed post-publication flip retains every possibly fetched BO. */
	struct drm_framebuffer *blocked_fb;
	struct drm_pending_vblank_event *blocked_event;
	u8 seq_index;
	u8 crt_index;
	u8 gfx_index;
	u8 misc;
	u8 sr[256];
	u8 cr[256];
	u8 gfx[VGA_GFX_C];
	u8 attr[ARRAY_SIZE(chrome9_vpit_attr)];
	u8 dac_mask;
	u8 lut[256][3];
	struct i2c_algo_bit_data ddc_bit;
	struct i2c_adapter ddc;
};

static void chrome9_sr_mask(u8 index, u8 mask, u8 value)
{
	u8 old = vga_io_rseq(index);

	vga_io_wseq(index, (old & ~mask) | (value & mask));
}

static void chrome9_cr_mask(u8 index, u8 mask, u8 value)
{
	u8 old = vga_io_rcrt(index);

	vga_io_wcrt(index, (old & ~mask) | (value & mask));
}

static void chrome9_ddc_setscl(void *data, int state)
{
	u8 value = vga_io_rseq(0x31) & 0xf0;

	(void)data;
	if (state)
		value |= BIT(5);
	else
		value &= ~BIT(5);
	vga_io_wseq(0x31, value | BIT(0));
}

static void chrome9_ddc_setsda(void *data, int state)
{
	u8 value = vga_io_rseq(0x31) & 0xf0;

	(void)data;
	if (state)
		value |= BIT(4);
	else
		value &= ~BIT(4);
	vga_io_wseq(0x31, value | BIT(0));
}

static int chrome9_ddc_getscl(void *data)
{
	(void)data;
	return !!(vga_io_rseq(0x31) & BIT(3));
}

static int chrome9_ddc_getsda(void *data)
{
	(void)data;
	return !!(vga_io_rseq(0x31) & BIT(2));
}

static int chrome9_ddc_pre_xfer(struct i2c_adapter *adapter)
{
	struct chrome9_device *cdev = i2c_get_adapdata(adapter);
	struct chrome9_kms_state *kms = cdev->kms_state;
	int ret;

	mutex_lock(&kms->lock);
	ret = chrome9_vga_seq_get(cdev);
	if (ret) {
		mutex_unlock(&kms->lock);
		return ret;
	}
	return ret;
}

static void chrome9_ddc_post_xfer(struct i2c_adapter *adapter)
{
	struct chrome9_device *cdev = i2c_get_adapdata(adapter);

	chrome9_vga_seq_put(cdev);
	mutex_unlock(&cdev->kms_state->lock);
}

static void chrome9_ddc_release(struct drm_device *drm, void *res)
{
	struct chrome9_kms_state *kms = res;

	(void)drm;
	i2c_del_adapter(&kms->ddc);
}

static int chrome9_ddc_init(struct chrome9_device *cdev)
{
	struct chrome9_kms_state *kms = cdev->kms_state;
	struct i2c_algo_bit_data *bit = &kms->ddc_bit;
	struct i2c_adapter *adapter = &kms->ddc;
	int ret;

	bit->data = cdev;
	bit->setsda = chrome9_ddc_setsda;
	bit->setscl = chrome9_ddc_setscl;
	bit->getsda = chrome9_ddc_getsda;
	bit->getscl = chrome9_ddc_getscl;
	bit->pre_xfer = chrome9_ddc_pre_xfer;
	bit->post_xfer = chrome9_ddc_post_xfer;
	bit->udelay = 10;
	bit->timeout = usecs_to_jiffies(100000);

	adapter->owner = THIS_MODULE;
	adapter->algo_data = bit;
	adapter->dev.parent = &cdev->pdev->dev;
	snprintf(adapter->name, sizeof(adapter->name),
		 "Chrome9 DVP1 DDC (SR31)");
	i2c_set_adapdata(adapter, cdev);

	ret = i2c_bit_add_bus(adapter);
	if (ret)
		return ret;
	return drmm_add_action_or_reset(&cdev->drm, chrome9_ddc_release, kms);
}

static u8 chrome9_attr_read(u8 index)
{
	vga_io_r(VGA_IS1_RC);
	return vga_io_rattr(index);
}

static void chrome9_attr_write(u8 index, u8 value)
{
	vga_io_r(VGA_IS1_RC);
	vga_io_wattr(index, value);
}

static void chrome9_attr_enable(void)
{
	vga_io_r(VGA_IS1_RC);
	vga_io_w(VGA_ATT_IW, VGA_AR_ENABLE_DISPLAY);
}

static void chrome9_vpit_program(void)
{
	unsigned int i;

	/* Exact viafb graphics-mode subset; keep the screen blank. */
	vga_io_wseq(0x01, 0x21);
	vga_io_wseq(0x02, 0x0f);
	vga_io_wseq(0x03, 0x00);
	vga_io_wseq(0x04, 0x0e);
	for (i = 0; i < ARRAY_SIZE(chrome9_vpit_gfx); i++)
		vga_io_wgfx(i, chrome9_vpit_gfx[i]);
	for (i = 0; i < ARRAY_SIZE(chrome9_vpit_attr); i++)
		chrome9_attr_write(i, chrome9_vpit_attr[i]);
	chrome9_attr_enable();
}

static void chrome9_vx900_common_program(void)
{
	/* Exact VX855/VX900 common graphics-mode image from in-tree viafb. */
	vga_io_wseq(0x10, 0x01);
	chrome9_sr_mask(0x15, 0x02, 0x02);
	chrome9_sr_mask(0x16, 0xbf, 0x08);
	vga_io_wseq(0x17, 0x1f);
	chrome9_sr_mask(0x18, 0xbf, 0x4e);
	chrome9_sr_mask(0x1a, 0xfb, 0x08);
	vga_io_wseq(0x1b, 0xf0);
	chrome9_sr_mask(0x1e, 0x07, 0x01);
	chrome9_sr_mask(0x2a, 0xf0, 0x00);
	vga_io_wseq(0x58, 0x00);
	vga_io_wseq(0x59, 0x00);
	chrome9_sr_mask(0x2d, 0xc0, 0xc0);

	vga_io_wcrt(0x32, 0x00);
	chrome9_cr_mask(0x33, 0x7f, 0x00);
	vga_io_wcrt(0x35, 0x00);
	chrome9_cr_mask(0x36, BIT(3), 0);
	vga_io_wcrt(0x69, 0x00);
	chrome9_cr_mask(0x6a, 0xfd, 0x60);
	vga_io_wcrt(0x6b, 0x00);
	vga_io_wcrt(0x88, 0x40);
	vga_io_wcrt(0x89, 0x00);
	vga_io_wcrt(0x8a, 0x88);
	vga_io_wcrt(0xd4, 0x81);
	vga_io_wcrt(0x91, 0x80);
	vga_io_wcrt(0x96, 0x00);
	vga_io_wcrt(0x97, 0x00);
	vga_io_wcrt(0x99, 0x00);
	vga_io_wcrt(0x9b, 0x00);
	vga_io_wcrt(0xd2, 0xff);
}

static void chrome9_lut_save(struct chrome9_kms_state *kms)
{
	u8 sr1a = vga_io_rseq(0x1a);
	u8 sr1b = vga_io_rseq(0x1b);
	unsigned int i;

	/* Select IGA1's LUT and stop its lock clock exactly as viafb does. */
	vga_io_wseq(0x1a, sr1a & ~BIT(0));
	vga_io_wseq(0x1b, sr1b & ~GENMASK(7, 6));
	kms->dac_mask = vga_io_r(VGA_PEL_MSK);
	vga_io_w(VGA_PEL_IR, 0);
	for (i = 0; i < ARRAY_SIZE(kms->lut); i++) {
		kms->lut[i][0] = vga_io_r(VGA_PEL_D);
		kms->lut[i][1] = vga_io_r(VGA_PEL_D);
		kms->lut[i][2] = vga_io_r(VGA_PEL_D);
	}
	vga_io_wseq(0x1b, sr1b);
	vga_io_wseq(0x1a, sr1a);
}

static void chrome9_lut_restore(struct chrome9_kms_state *kms)
{
	u8 sr1a = vga_io_rseq(0x1a);
	u8 sr1b = vga_io_rseq(0x1b);
	unsigned int i;

	vga_io_wseq(0x1a, sr1a & ~BIT(0));
	vga_io_wseq(0x1b, sr1b & ~GENMASK(7, 6));
	vga_io_w(VGA_PEL_MSK, kms->dac_mask);
	vga_io_w(VGA_PEL_IW, 0);
	for (i = 0; i < ARRAY_SIZE(kms->lut); i++) {
		vga_io_w(VGA_PEL_D, kms->lut[i][0]);
		vga_io_w(VGA_PEL_D, kms->lut[i][1]);
		vga_io_w(VGA_PEL_D, kms->lut[i][2]);
	}
	vga_io_wseq(0x1b, sr1b);
	vga_io_wseq(0x1a, sr1a);
}

static void chrome9_lut_program_identity(void)
{
	u8 sr1a = vga_io_rseq(0x1a);
	u8 sr1b = vga_io_rseq(0x1b);
	unsigned int i;

	vga_io_wseq(0x1a, sr1a & ~BIT(0));
	vga_io_wseq(0x1b, sr1b & ~GENMASK(7, 6));
	vga_io_w(VGA_PEL_MSK, 0xff);
	vga_io_w(VGA_PEL_IW, 0);
	for (i = 0; i < 256; i++) {
		vga_io_w(VGA_PEL_D, i);
		vga_io_w(VGA_PEL_D, i);
		vga_io_w(VGA_PEL_D, i);
	}
	vga_io_wseq(0x1b, sr1b);
	vga_io_wseq(0x1a, sr1a);
}

static void chrome9_iga1_save(struct chrome9_kms_state *kms)
{
	unsigned int i;

	kms->seq_index = vga_io_r(VGA_SEQ_I);
	kms->crt_index = vga_io_r(VGA_CRT_IC);
	kms->gfx_index = vga_io_r(VGA_GFX_I);
	kms->misc = vga_io_r(VGA_MIS_R);
	kms->sr[0x10] = vga_io_rseq(0x10);
	vga_io_wseq(0x10, 0x01);
	for (i = 0; i < ARRAY_SIZE(chrome9_saved_sr); i++) {
		if (chrome9_saved_sr[i] != 0x10)
			kms->sr[chrome9_saved_sr[i]] =
				vga_io_rseq(chrome9_saved_sr[i]);
	}
	/* Read both protection controls before unlocking extended CRTC state. */
	kms->cr[0x11] = vga_io_rcrt(0x11);
	kms->cr[0x47] = vga_io_rcrt(0x47);
	chrome9_cr_mask(0x11, BIT(7), 0);
	chrome9_cr_mask(0x47, BIT(4) | BIT(0), 0);
	for (i = 0; i < ARRAY_SIZE(chrome9_saved_cr); i++) {
		if (chrome9_saved_cr[i] != 0x11 &&
		    chrome9_saved_cr[i] != 0x47)
			kms->cr[chrome9_saved_cr[i]] =
				vga_io_rcrt(chrome9_saved_cr[i]);
	}
	for (i = 0; i < ARRAY_SIZE(kms->gfx); i++)
		kms->gfx[i] = vga_io_rgfx(i);
	for (i = 0; i < ARRAY_SIZE(kms->attr); i++)
		kms->attr[i] = chrome9_attr_read(i);
	chrome9_attr_enable();
	chrome9_lut_save(kms);
	kms->snapshot_valid = true;
}

static void chrome9_iga1_restore(struct chrome9_kms_state *kms)
{
	unsigned int i;
	u8 index;

	if (!kms->snapshot_valid)
		return;

	/* Keep the output dark until timing, fetch and clock state is restored. */
	chrome9_sr_mask(0x01, BIT(5), BIT(5));
	vga_io_wseq(0x10, 0x01);
	chrome9_cr_mask(0x11, BIT(7), 0);
	chrome9_cr_mask(0x47, BIT(4) | BIT(0), 0);
	chrome9_cr_mask(0x36, GENMASK(5, 4), GENMASK(5, 4));
	chrome9_cr_mask(0x17, BIT(7), 0);
	vga_io_rcrt(0x17);
	for (i = 0; i < ARRAY_SIZE(chrome9_saved_cr); i++) {
		index = chrome9_saved_cr[i];
		if (index != 0x11 && index != 0x17 && index != 0x36 &&
		    index != 0x47)
			vga_io_wcrt(index, kms->cr[index]);
	}
	for (i = 0; i < ARRAY_SIZE(chrome9_saved_sr); i++) {
		index = chrome9_saved_sr[i];
		if (index != 0x01 && index != 0x10 && index != 0x40 &&
		    index != 0x44 &&
		    index != 0x45 && index != 0x46)
			vga_io_wseq(index, kms->sr[index]);
	}
	for (i = 0; i < ARRAY_SIZE(kms->gfx); i++)
		vga_io_wgfx(i, kms->gfx[i]);
	for (i = 0; i < ARRAY_SIZE(kms->attr); i++)
		chrome9_attr_write(i, kms->attr[i]);
	chrome9_attr_enable();
	chrome9_lut_restore(kms);

	/* Restore the old PLL while its programming reset is asserted. */
	chrome9_sr_mask(0x40, BIT(1), BIT(1));
	vga_io_wseq(0x44, kms->sr[0x44]);
	vga_io_wseq(0x45, kms->sr[0x45]);
	vga_io_wseq(0x46, kms->sr[0x46]);
	vga_io_wseq(0x40, kms->sr[0x40]);
	vga_io_w(VGA_MIS_W, kms->misc);
	/* Latch the restored timing with the same CR17 reset edge as viafb. */
	vga_io_wcrt(0x17, kms->cr[0x17] & ~BIT(7));
	vga_io_wcrt(0x17, kms->cr[0x17]);

	/* Restore output state, then both register-protection controls. */
	vga_io_wcrt(0x36, kms->cr[0x36]);
	vga_io_wcrt(0x11, kms->cr[0x11]);
	vga_io_wcrt(0x47, kms->cr[0x47]);
	vga_io_wseq(0x10, kms->sr[0x10]);
	vga_io_wseq(0x01, kms->sr[0x01]);
	vga_io_w(VGA_GFX_I, kms->gfx_index);
	vga_io_w(VGA_CRT_IC, kms->crt_index);
	vga_io_w(VGA_SEQ_I, kms->seq_index);
	kms->snapshot_valid = false;
}

static int chrome9_iga1_program(struct chrome9_device *cdev,
				const struct drm_display_mode *mode, u32 pitch,
				u32 address, bool system_framebuffer)
{
	u32 base = address >> 1;
	int ret;

	/* Blank first, power off VGA, select IGA1, and disable IGA2. */
	chrome9_sr_mask(0x01, BIT(5), BIT(5));
	vga_io_wseq(0x10, 0x01);
	chrome9_cr_mask(0x11, BIT(7), 0);
	chrome9_cr_mask(0x47, BIT(4) | BIT(0), 0);
	chrome9_cr_mask(0x36, GENMASK(5, 4), GENMASK(5, 4));
	chrome9_cr_mask(0x17, BIT(7), 0);
	if (vga_io_rcrt(0x17) & BIT(7))
		return -EIO;
	chrome9_sr_mask(0x16, BIT(6), 0);
	chrome9_cr_mask(0x6a, BIT(6), 0);
	chrome9_cr_mask(0x6a, BIT(7), 0);
	chrome9_cr_mask(0x6a, BIT(6), BIT(6));
	chrome9_vx900_common_program();
	chrome9_vpit_program();
	/* viafb marks this bit as required for correct VX900 IGA1 modes. */
	chrome9_cr_mask(0x45, BIT(0), 0);

	/* XRGB8888 uses viafb's depth-24 selector. */
	chrome9_sr_mask(0x15, GENMASK(7, 1), 0xae);
	/* DVP1 uses IGA1's LUT; establish an explicit linear 8-bit ramp. */
	chrome9_lut_program_identity();

	/* IGA1 uses two-byte units; CR48[7:6]=01 selects translated SF. */
	chrome9_cr_mask(0x48, GENMASK(7, 6),
			system_framebuffer ? BIT(6) : 0);
	vga_io_wcrt(0x0d, base);
	vga_io_wcrt(0x0c, base >> 8);
	vga_io_wcrt(0x34, base >> 16);
	chrome9_cr_mask(0x48, GENMASK(4, 0), base >> 24);
	if (vga_io_rcrt(0x0d) != (u8)base ||
	    vga_io_rcrt(0x0c) != (u8)(base >> 8) ||
	    vga_io_rcrt(0x34) != (u8)(base >> 16) ||
	    (vga_io_rcrt(0x48) & (GENMASK(7, 6) | GENMASK(4, 0))) !=
		((system_framebuffer ? BIT(6) : 0) |
		 ((base >> 24) & GENMASK(4, 0))))
		return -EIO;

	/* Common VGA prerequisites not derived from the requested mode. */
	vga_io_wcrt(0x08, 0x00);
	chrome9_cr_mask(0x09, 0xdf, 0x40);
	vga_io_wcrt(0x0a, 0x1e);
	vga_io_wcrt(0x0b, 0x00);
	vga_io_wcrt(0x0e, 0x00);
	vga_io_wcrt(0x0f, 0x00);
	vga_io_wcrt(0x14, 0x00);
	vga_io_wcrt(0x18, 0xff);
	chrome9_cr_mask(0x33, 0x07, 0x06);
	/* Direct 0214 base updates latch by frame, never by scan line. */
	chrome9_cr_mask(CHROME9_CR_IGA1_FLIP_MODE,
			CHROME9_CR_IGA1_FLIP_BY_LINE, 0);
	ret = chrome9_vx900_iga1_program_mode(mode, pitch);
	if (ret)
		return ret;

	/*
	 * VX900 DVI on the HP t5550 is DVP1 fed from IGA1.  These are the
	 * in-tree viafb source, polarity and pad-power operations, reproduced
	 * without its global fbdev policy.  The board exposes DDC directly on
	 * SR31 and has no responding VT1632 transmitter to program.
	 */
	chrome9_cr_mask(0x91, GENMASK(7, 5), BIT(7));
	/*
	 * The vendor VIADVIPatchSkew_DVP1 path uses CR9B[3:0]=0xa.
	 * This is required on the t5550: the viafb default of zero produces
	 * swapped colors and extensive black corruption on an otherwise valid
	 * XRGB8888 scanout.
	 */
	chrome9_cr_mask(0x9b, GENMASK(6, 0),
			(mode->flags & DRM_MODE_FLAG_PHSYNC ? 0 : BIT(5)) |
			(mode->flags & DRM_MODE_FLAG_PVSYNC ? 0 : BIT(6)) |
			0x0a);
	chrome9_sr_mask(0x1e, GENMASK(5, 4), GENMASK(5, 4));

	/* DVI-only: keep the analog CRT DAC off while DVP1 carries IGA1. */
	chrome9_cr_mask(0x36, GENMASK(5, 4), GENMASK(5, 4));
	vga_io_wseq(0x10, cdev->kms_state->sr[0x10]);
	chrome9_sr_mask(0x01, BIT(5), 0);
	return 0;
}

static bool chrome9_state_valid(struct chrome9_device *cdev,
				const struct drm_plane_state *plane_state,
				const struct drm_crtc_state *crtc_state)
{
	const struct drm_framebuffer *fb = plane_state->fb;

	if (!cdev->kms_state->fixed_iga1 ||
	    READ_ONCE(cdev->kms_state->flip_blocked) ||
	    !chrome9_vx900_iga1_mode_valid(&crtc_state->adjusted_mode) ||
	    !fb ||
	    plane_state->crtc != &cdev->display_pipe.crtc)
		return false;
	if (fb->format->format != DRM_FORMAT_XRGB8888 ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    fb->width < crtc_state->adjusted_mode.hdisplay ||
	    fb->height < crtc_state->adjusted_mode.vdisplay ||
	    fb->pitches[0] < fb->width * 4 ||
	    !IS_ALIGNED(fb->pitches[0], 32) || fb->pitches[0] / 8 > 0x7ff ||
	    (fb->offsets[0] && fb->offsets[0] != PAGE_SIZE))
		return false;
	if (cdev->kms_state->staging_scanout &&
	    (u64)fb->pitches[0] * fb->height > cdev->scanout_size)
		return false;
	if (plane_state->src_x || plane_state->src_y ||
	    plane_state->src_w != crtc_state->adjusted_mode.hdisplay << 16 ||
	    plane_state->src_h != crtc_state->adjusted_mode.vdisplay << 16 ||
	    plane_state->crtc_x || plane_state->crtc_y ||
	    plane_state->crtc_w != crtc_state->adjusted_mode.hdisplay ||
	    plane_state->crtc_h != crtc_state->adjusted_mode.vdisplay ||
	    plane_state->rotation != DRM_MODE_ROTATE_0)
		return false;
	return true;
}

static struct drm_pending_vblank_event *
chrome9_pipe_take_event(struct drm_simple_display_pipe *pipe)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&drm->event_lock, flags);
	event = pipe->crtc.state->event;
	pipe->crtc.state->event = NULL;
	spin_unlock_irqrestore(&drm->event_lock, flags);
	return event;
}

static void
chrome9_pipe_send_event(struct drm_simple_display_pipe *pipe,
			struct drm_pending_vblank_event *event)
{
	struct drm_device *drm = pipe->crtc.dev;
	unsigned long flags;

	if (!event)
		return;
	spin_lock_irqsave(&drm->event_lock, flags);
	drm_crtc_send_vblank_event(&pipe->crtc, event);
	spin_unlock_irqrestore(&drm->event_lock, flags);
}

static void chrome9_pipe_complete_event(struct drm_simple_display_pipe *pipe)
{
	chrome9_pipe_send_event(pipe, chrome9_pipe_take_event(pipe));
}

static int chrome9_wait_iga1_flip(struct chrome9_device *cdev,
				  bool armed_in_blank, u32 expected)
{
	u32 value;
	int ret;

	/*
	 * A write made during blank must first leave that blank.  In either
	 * case, observing the next rising blank level proves a post-arm frame
	 * boundary without enabling the still-unproven interrupt path.
	 */
	if (armed_in_blank) {
		ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_DISPLAY_STATUS,
					 value,
					 !(value & CHROME9_DISPLAY_IGA1_BLANK),
					 10, 100000);
		if (ret)
			return ret;
	}
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_DISPLAY_STATUS,
				 value, value & CHROME9_DISPLAY_IGA1_BLANK,
				 10, 100000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_BASE_UPDATE_STATUS,
				 value,
				 !(value & CHROME9_BASE_UPDATE_IGA1_PENDING),
				 10, 100000);
	if (ret)
		return ret;
	value = readl(cdev->mmio + CHROME9_REG_IGA1_BASE);
	return (value & CHROME9_IGA1_BASE_ADDR_MASK) ==
	       (expected & CHROME9_IGA1_BASE_ADDR_MASK) ? 0 : -EIO;
}

static enum drm_connector_status
chrome9_connector_detect(struct drm_connector *connector, bool force)
{
	struct chrome9_device *cdev = to_chrome9(connector->dev);

	return cdev->kms_state->fixed_iga1 ? connector_status_connected :
		connector_status_disconnected;
}

static int chrome9_connector_get_modes(struct drm_connector *connector)
{
	struct chrome9_device *cdev = to_chrome9(connector->dev);
	const struct drm_edid *drm_edid;
	struct drm_display_mode *mode;
	int count;

	if (!cdev->kms_state->fixed_iga1)
		return 0;
	drm_edid = drm_edid_read(connector);
	if (drm_edid) {
		count = drm_edid_connector_update(connector, drm_edid);
		drm_edid_free(drm_edid);
		if (count)
			return count;
		return drm_edid_connector_add_modes(connector);
	}
	drm_edid_connector_update(connector, NULL);
	count = drm_add_modes_noedid(connector, 2048, 2048);
	list_for_each_entry(mode, &connector->probed_modes, head)
		if (mode->hdisplay == CHROME9_FIXED_WIDTH &&
		    mode->vdisplay == CHROME9_FIXED_HEIGHT &&
		    drm_mode_vrefresh(mode) == 60) {
			mode->type |= DRM_MODE_TYPE_PREFERRED;
			break;
		}
	return count;
}

static const struct drm_connector_helper_funcs chrome9_connector_helper_funcs = {
	.get_modes = chrome9_connector_get_modes,
};

static const struct drm_connector_funcs chrome9_connector_funcs = {
	.detect = chrome9_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static enum drm_mode_status
chrome9_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			const struct drm_display_mode *mode)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);

	if (!cdev->kms_state->fixed_iga1)
		return MODE_BAD;
	return chrome9_vx900_iga1_mode_valid(mode) ? MODE_OK : MODE_BAD;
}

static int chrome9_pipe_check(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *plane_state,
			      struct drm_crtc_state *crtc_state)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);
	struct drm_plane_state *old_plane_state = pipe->plane.state;

	if (crtc_state->enable &&
	    !chrome9_state_valid(cdev, plane_state, crtc_state))
		return -EINVAL;
	if (crtc_state->async_flip)
		return -EINVAL;
	/*
	 * A live VX900 probe accepted the new 0214 address but left 0204[1]
	 * asserted indefinitely.  Keep ordinary modesets and direct scanout,
	 * but make Xorg fall back to its native Present copy until a real
	 * vblank/latch completion path is proven.  The opt-in remains available
	 * for bounded display-lab work with both mappings retained on failure.
	 */
	if (crtc_state->enable && !cdev->kms_state->staging_scanout &&
	    !chrome9_direct_flips && old_plane_state &&
	    old_plane_state->fb != plane_state->fb &&
	    !drm_atomic_crtc_needs_modeset(crtc_state))
		return -EOPNOTSUPP;
	if (crtc_state->enable && READ_ONCE(cdev->kms_state->flip_blocked) &&
	    old_plane_state && old_plane_state->fb != plane_state->fb &&
	    !drm_atomic_crtc_needs_modeset(crtc_state))
		return -EIO;
	return 0;
}

static int chrome9_pipe_prepare_fb(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *plane_state)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);
	int ret;

	ret = drm_gem_plane_helper_prepare_fb(&pipe->plane, plane_state);
	if (ret || !plane_state->fb || cdev->kms_state->staging_scanout)
		return ret;
	ret = chrome9_gem_fb_pin(plane_state->fb);
	if (ret)
		dev_err(&cdev->pdev->dev,
			"cannot pin framebuffer into SF aperture: %d\n", ret);
	return ret;
}

static void chrome9_pipe_cleanup_fb(struct drm_simple_display_pipe *pipe,
				    struct drm_plane_state *plane_state)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);

	if (plane_state->fb && !cdev->kms_state->staging_scanout)
		chrome9_gem_fb_unpin(plane_state->fb);
}

static void chrome9_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);
	struct chrome9_kms_state *kms = cdev->kms_state;
	struct drm_framebuffer *display_fb = NULL;
	struct drm_framebuffer *retired_fb = NULL;
	u32 scanout_base = 0;
	bool system_framebuffer;
	bool enabled = false;
	int ret;

	if (!chrome9_state_valid(cdev, plane_state, crtc_state)) {
		dev_err(&cdev->pdev->dev, "refusing invalid IGA1 enable\n");
		return;
	}
	if (kms->staging_scanout) {
		ret = chrome9_gem_copy_fb_to_scanout(cdev, plane_state->fb);
		if (ret) {
			dev_err(&cdev->pdev->dev,
				"initial BAR2 scanout copy failed: %d\n", ret);
			return;
		}
		scanout_base = cdev->scanout_offset;
		system_framebuffer = false;
	} else {
		ret = chrome9_gem_fb_sf_base(plane_state->fb, &scanout_base);
		if (ret) {
			dev_err(&cdev->pdev->dev,
				"direct framebuffer has no valid SF base: %d\n", ret);
			return;
		}
		chrome9_gem_fb_sync_for_device(plane_state->fb);
		system_framebuffer = true;
		/* Atomic cleanup alone is not proof that display stopped fetching. */
		ret = chrome9_gem_fb_pin(plane_state->fb);
		if (ret) {
			dev_err(&cdev->pdev->dev,
				"cannot acquire direct display mapping: %d\n", ret);
			return;
		}
		drm_framebuffer_get(plane_state->fb);
		display_fb = plane_state->fb;
	}

	ret = chrome9_engine_display_begin(cdev);
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"cannot serialize IGA1 with command reader: %d\n", ret);
		goto out_put_display;
	}

	mutex_lock(&kms->lock);
	if (kms->iga1_enabled)
		goto out_unlock_engine;
	ret = chrome9_vga_seq_get(cdev);
	if (ret) {
		dev_err(&cdev->pdev->dev, "cannot lock VGA I/O: %d\n", ret);
		goto out_unlock_engine;
	}
	if (!(vga_io_r(VGA_MIS_R) & VGA_MIS_COLOR)) {
		dev_err(&cdev->pdev->dev,
			"refusing IGA1 enable while VGA color I/O decode is off\n");
		chrome9_vga_seq_put(cdev);
		goto out_unlock_engine;
	}
	/* Preserve firmware state only once, for final ownership handoff. */
	if (!kms->snapshot_valid)
		chrome9_iga1_save(kms);
	ret = chrome9_iga1_program(cdev, &crtc_state->adjusted_mode,
				  plane_state->fb->pitches[0], scanout_base,
				  system_framebuffer);
	if (ret) {
		kms->flip_blocked = true;
		chrome9_vga_seq_put(cdev);
		goto out_unlock_engine;
	}
	kms->iga1_enabled = true;
	enabled = true;
	if (system_framebuffer) {
		WARN_ON(kms->active_fb);
		kms->active_fb = display_fb;
		display_fb = NULL;
	}
	/* Base readback occurred while blanked; CR17 has now latched new timing. */
	retired_fb = kms->retired_fb;
	kms->retired_fb = NULL;
	chrome9_vga_seq_put(cdev);
	dev_info(&cdev->pdev->dev,
		 "IGA1 %ux%u@%u kHz output enabled at %s %#x pitch=%u\n",
		 crtc_state->adjusted_mode.hdisplay,
		 crtc_state->adjusted_mode.vdisplay,
		 crtc_state->adjusted_mode.clock,
		 system_framebuffer ? "SF" : "BAR2", scanout_base,
		 plane_state->fb->pitches[0]);

out_unlock_engine:
	mutex_unlock(&kms->lock);
	chrome9_engine_display_end(cdev);
out_put_display:
	if (display_fb) {
		chrome9_gem_fb_unpin(display_fb);
		drm_framebuffer_put(display_fb);
	}
	if (retired_fb) {
		chrome9_gem_fb_unpin(retired_fb);
		drm_framebuffer_put(retired_fb);
	}
	if (enabled)
		chrome9_pipe_complete_event(pipe);
}

static void chrome9_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);
	struct chrome9_kms_state *kms = cdev->kms_state;
	struct drm_pending_vblank_event *blocked_event = NULL;
	bool disabled = false;
	int ret;

	if (!READ_ONCE(kms->iga1_enabled))
		return;
	ret = chrome9_engine_display_begin(cdev);
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"cannot serialize IGA1 disable with command reader: %d\n",
			ret);
		return;
	}
	mutex_lock(&kms->lock);
	if (!kms->iga1_enabled)
		goto out_unlock_engine;
	ret = chrome9_vga_seq_get(cdev);
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"cannot lock VGA I/O to disable IGA1: %d\n", ret);
		goto out_unlock_engine;
	}
	/*
	 * Atomic disable keeps driver ownership and never revives firmware
	 * scanout.  VX900 CR17[7]=0 holds both timing generators in hardware
	 * reset; keep the old SF mapping until a new base is read back and timing
	 * is re-enabled by chrome9_iga1_program().
	 */
	chrome9_cr_mask(0x45, BIT(5), BIT(5));
	chrome9_sr_mask(0x01, BIT(5), BIT(5));
	chrome9_sr_mask(0x1e, GENMASK(5, 4), 0);
	chrome9_cr_mask(0x36, GENMASK(5, 4), GENMASK(5, 4));
	chrome9_cr_mask(0x17, BIT(7), 0);
	vga_io_rseq(0x01);
	if (vga_io_rcrt(0x17) & BIT(7)) {
		kms->flip_blocked = true;
		dev_crit(&cdev->pdev->dev,
			 "IGA1 timing reset did not latch; scanout mapping retained\n");
		chrome9_vga_seq_put(cdev);
		goto out_unlock_engine;
	}
	kms->iga1_enabled = false;
	if (kms->active_fb) {
		if (WARN_ON(kms->retired_fb)) {
			kms->flip_blocked = true;
		} else {
			kms->retired_fb = kms->active_fb;
			kms->active_fb = NULL;
		}
	}
	if (!kms->blocked_fb) {
		blocked_event = kms->blocked_event;
		kms->blocked_event = NULL;
	}
	disabled = true;
	chrome9_vga_seq_put(cdev);
	dev_info(&cdev->pdev->dev, "driver-owned IGA1 output disabled\n");

out_unlock_engine:
	mutex_unlock(&kms->lock);
	chrome9_engine_display_end(cdev);
	if (disabled) {
		chrome9_pipe_send_event(pipe, blocked_event);
		/* A full modeset which remains enabled completes after re-enable. */
		if (!pipe->crtc.state->enable)
			chrome9_pipe_complete_event(pipe);
	}
}

static void chrome9_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_plane_state)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);
	struct chrome9_kms_state *kms = cdev->kms_state;
	struct device *dev = &cdev->pdev->dev;
	struct drm_plane_state *plane_state = pipe->plane.state;
	struct drm_pending_vblank_event *event;
	struct drm_framebuffer *display_fb = NULL;
	struct drm_framebuffer *old_display_fb = NULL;
	u32 scanout_base = 0;
	u32 old_base;
	u32 expected;
	u32 value;
	bool armed_in_blank;
	bool published = false;
	int ret;

	if (!plane_state || !plane_state->fb)
		return;
	/* Initial activation is completed by pipe_enable after programming IGA1. */
	if (!READ_ONCE(kms->iga1_enabled))
		return;
	if (!old_plane_state || old_plane_state->fb == plane_state->fb) {
		chrome9_pipe_complete_event(pipe);
		return;
	}
	if (kms->staging_scanout) {
		ret = chrome9_gem_copy_fb_to_scanout(cdev, plane_state->fb);
		if (ret) {
			dev_err_ratelimited(dev,
					    "BAR2 scanout staging copy failed: %d\n",
					    ret);
			return;
		}
		chrome9_pipe_complete_event(pipe);
		return;
	}

	event = chrome9_pipe_take_event(pipe);
	ret = chrome9_gem_fb_sf_base(plane_state->fb, &scanout_base);
	if (ret)
		goto out_block;
	chrome9_gem_fb_sync_for_device(plane_state->fb);
	ret = chrome9_gem_fb_pin(plane_state->fb);
	if (ret)
		goto out_block;
	drm_framebuffer_get(plane_state->fb);
	display_fb = plane_state->fb;

	ret = chrome9_engine_display_begin(cdev);
	if (ret)
		goto out_block;
	mutex_lock(&kms->lock);
	if (!kms->iga1_enabled || kms->flip_blocked ||
	    kms->active_fb != old_plane_state->fb) {
		ret = -EIO;
		goto out_block_locked;
	}
	value = readl(cdev->mmio + CHROME9_REG_BASE_UPDATE_STATUS);
	if (value & CHROME9_BASE_UPDATE_IGA1_PENDING) {
		ret = -EBUSY;
		goto out_block_locked;
	}

	old_base = readl(cdev->mmio + CHROME9_REG_IGA1_BASE);
	expected = chrome9_iga1_base_pack(old_base, scanout_base);
	armed_in_blank = !!(readl(cdev->mmio + CHROME9_REG_DISPLAY_STATUS) &
			    CHROME9_DISPLAY_IGA1_BLANK);
	/*
	 * The persistent SF PTEs and DMA device ownership are visible before the
	 * direct base.  The posted read confirms programming, not display latch.
	 */
	dma_wmb();
	writel(expected, cdev->mmio + CHROME9_REG_IGA1_BASE);
	published = true;
	value = readl(cdev->mmio + CHROME9_REG_IGA1_BASE);
	if ((value & CHROME9_IGA1_BASE_ADDR_MASK) !=
	    (expected & CHROME9_IGA1_BASE_ADDR_MASK)) {
		ret = -EIO;
		goto out_block_locked;
	}
	ret = chrome9_wait_iga1_flip(cdev, armed_in_blank, expected);
	if (ret)
		goto out_block_locked;

	old_display_fb = kms->active_fb;
	kms->active_fb = display_fb;
	display_fb = NULL;
	mutex_unlock(&kms->lock);
	chrome9_engine_display_end(cdev);

	/*
	 * Event completion releases the helper's flip_done dependency only after
	 * a post-arm frame boundary and the exact hardware latch proof.
	 */
	chrome9_pipe_send_event(pipe, event);
	if (old_display_fb) {
		chrome9_gem_fb_unpin(old_display_fb);
		drm_framebuffer_put(old_display_fb);
	}
	dev_dbg(dev, "direct SF flip latched at %#x (0214=%08x)\n",
		scanout_base, expected);
	return;

out_block_locked:
	kms->flip_blocked = true;
	kms->blocked_event = event;
	event = NULL;
	/*
	 * Once 0214 was written, either BO may be fetched.  Retain the new
	 * independent display pin alongside the unchanged active reference.
	 */
	if (published) {
		kms->blocked_fb = display_fb;
		display_fb = NULL;
	}
	mutex_unlock(&kms->lock);
	chrome9_engine_display_end(cdev);
out_block:
	if (event) {
		mutex_lock(&kms->lock);
		kms->flip_blocked = true;
		kms->blocked_event = event;
		mutex_unlock(&kms->lock);
	}
	if (display_fb) {
		chrome9_gem_fb_unpin(display_fb);
		drm_framebuffer_put(display_fb);
	}
	dev_crit(dev,
		 "direct SF flip failed: %d published=%u base=%#x status=%08x update=%08x readback=%08x; presentation blocked%s\n",
		 ret, published, scanout_base,
		 readl(cdev->mmio + CHROME9_REG_DISPLAY_STATUS),
		 readl(cdev->mmio + CHROME9_REG_BASE_UPDATE_STATUS),
		 readl(cdev->mmio + CHROME9_REG_IGA1_BASE),
		 published ? "; both mappings retained for restore/reboot" : "");
}

static const struct drm_simple_display_pipe_funcs chrome9_pipe_funcs = {
	.mode_valid = chrome9_pipe_mode_valid,
	.enable = chrome9_pipe_enable,
	.disable = chrome9_pipe_disable,
	.check = chrome9_pipe_check,
	.update = chrome9_pipe_update,
	.prepare_fb = chrome9_pipe_prepare_fb,
	.cleanup_fb = chrome9_pipe_cleanup_fb,
};

static const u32 chrome9_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 chrome9_primary_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static int chrome9_atomic_check(struct drm_device *drm,
				struct drm_atomic_commit *state)
{
	struct chrome9_device *cdev = to_chrome9(drm);
	struct drm_crtc_state *crtc_state;
	const struct drm_plane_state *old_plane_state;
	struct drm_plane_state *plane_state;
	int ret;

	crtc_state = drm_atomic_get_crtc_state(state, &cdev->display_pipe.crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	plane_state = drm_atomic_get_plane_state(state, &cdev->display_pipe.plane);
	if (IS_ERR(plane_state))
		return PTR_ERR(plane_state);
	old_plane_state = drm_atomic_get_old_plane_state(
		state, &cdev->display_pipe.plane);
	/*
	 * A DRM client takeover may replace fbdev's buffer at the same mode.
	 * Turn only ALLOW_MODESET commits into a full blank/program/unblank;
	 * ordinary Present flips remain rejected by chrome9_pipe_check().
	 */
	if (crtc_state->enable && !cdev->kms_state->staging_scanout &&
	    !chrome9_direct_flips && state->allow_modeset && old_plane_state &&
	    old_plane_state->fb != plane_state->fb &&
	    !drm_atomic_crtc_needs_modeset(crtc_state))
		crtc_state->mode_changed = true;
	ret = drm_atomic_helper_check(drm, state);
	if (ret || !crtc_state->enable)
		return ret;
	return chrome9_state_valid(cdev, plane_state, crtc_state) ? 0 :
		-EINVAL;
}

static const struct drm_mode_config_funcs chrome9_mode_config_funcs = {
	.fb_create = chrome9_gem_fb_create,
	.atomic_check = chrome9_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int chrome9_scanout_carveout_init(struct chrome9_device *cdev,
					 unsigned long offset,
					 unsigned long size)
{
	void __iomem * const *iomap_table;
	resource_size_t usable;

	mutex_init(&cdev->scanout_lock);
	if (offset == ULONG_MAX && !size)
		return 0;
	if (offset == ULONG_MAX || !size)
		return dev_err_probe(&cdev->pdev->dev, -EINVAL,
			"scanout_offset and scanout_size must be supplied together\n");
	if (!IS_ALIGNED(offset, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
		return dev_err_probe(&cdev->pdev->dev, -EINVAL,
			"scanout carveout must be page aligned\n");
	if (cdev->vram_size < SZ_64K)
		return dev_err_probe(&cdev->pdev->dev, -ENODEV,
			"BAR2 size is unavailable for scanout validation\n");

	usable = cdev->vram_size - SZ_64K;
	if (offset > usable || size > usable - offset)
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"scanout carveout [%#lx,%#lx) overlaps the final 64-KiB GART or exceeds BAR2\n",
			offset, offset + size);
	if (!chrome9_iga1_base_valid(offset) ||
	    offset + size > CHROME9_IGA1_BASE_MAX)
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"scanout carveout is outside the IGA1 addressable range\n");

	iomap_table = pcim_iomap_table(cdev->pdev);
	if (iomap_table && iomap_table[2])
		cdev->vram = iomap_table[2];
	else
		cdev->vram = pcim_iomap(cdev->pdev, 2, 0);
	if (!cdev->vram)
		return dev_err_probe(&cdev->pdev->dev, -ENOMEM,
				     "cannot map BAR2 scanout carveout\n");

	cdev->scanout_offset = offset;
	cdev->scanout_size = size;
	dev_info(&cdev->pdev->dev,
		 "lab-only BAR2 scanout staging enabled: [%#lx,%#lx), final 64 KiB reserved for GART\n",
		 offset, offset + size);
	return 0;
}

int chrome9_kms_init(struct chrome9_device *cdev,
		     unsigned long scanout_offset,
		     unsigned long scanout_size, bool fixed_iga1,
		     bool staging_scanout)
{
	struct drm_device *drm = &cdev->drm;
	struct chrome9_kms_state *kms;
	int ret;

	kms = drmm_kzalloc(drm, sizeof(*kms), GFP_KERNEL);
	if (!kms)
		return -ENOMEM;
	mutex_init(&kms->lock);
	cdev->kms_state = kms;
	kms->staging_scanout = staging_scanout;
	ret = chrome9_vx900_display_selftest();
	if (ret)
		return dev_err_probe(&cdev->pdev->dev, ret,
			"VX900 IGA1 mode builder failed the 1024x768 oracle\n");
	if (fixed_iga1 && !staging_scanout && !chrome9_engine_ready(cdev))
		return dev_err_probe(&cdev->pdev->dev, -ENODEV,
			"direct IGA1 requires a ready SF engine\n");

	if (staging_scanout) {
		ret = chrome9_scanout_carveout_init(cdev, scanout_offset,
						    scanout_size);
		if (ret)
			return ret;
	} else {
		mutex_init(&cdev->scanout_lock);
		if (scanout_offset != ULONG_MAX || scanout_size)
			dev_warn(&cdev->pdev->dev,
				 "ignoring BAR2 scanout carveout in direct-SF mode\n");
	}
	if (fixed_iga1 && staging_scanout &&
	    (!cdev->scanout_size || cdev->scanout_size < CHROME9_FIXED_SIZE))
		return dev_err_probe(&cdev->pdev->dev, -EINVAL,
			"staging IGA1 requires a legacy-addressable scanout carveout of at least %u bytes\n",
			CHROME9_FIXED_SIZE);
	kms->fixed_iga1 = fixed_iga1;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	ret = chrome9_ddc_init(cdev);
	if (ret)
		return dev_err_probe(&cdev->pdev->dev, ret,
				     "cannot register DVP1 DDC bus\n");
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 2048;
	drm->mode_config.max_height = 2048;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &chrome9_mode_config_funcs;

	drm_connector_helper_add(&cdev->display_connector,
				 &chrome9_connector_helper_funcs);
	ret = drm_connector_init_with_ddc(drm, &cdev->display_connector,
					  &chrome9_connector_funcs,
					  DRM_MODE_CONNECTOR_DVII,
					  &kms->ddc);
	if (ret)
		return ret;
	ret = drm_simple_display_pipe_init(drm, &cdev->display_pipe,
					   &chrome9_pipe_funcs,
					   chrome9_primary_formats,
					   ARRAY_SIZE(chrome9_primary_formats),
					   chrome9_primary_modifiers,
					   &cdev->display_connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	if (fixed_iga1) {
		if (!staging_scanout && chrome9_direct_flips)
			dev_warn(&cdev->pdev->dev,
				 "native IGA1 modesetting armed with direct SF scanout and experimental polled flips; no vblank IRQ support\n");
		else
			dev_info(&cdev->pdev->dev,
				 "native IGA1 modesetting armed with %s scanout; direct flips disabled\n",
				 staging_scanout ? "BAR2 staging" : "direct SF");
	}
	return 0;
}

int chrome9_kms_fini(struct chrome9_device *cdev)
{
	struct chrome9_kms_state *kms = cdev->kms_state;
	struct drm_framebuffer *display_fb = NULL;
	struct drm_framebuffer *retired_fb = NULL;
	struct drm_framebuffer *blocked_fb = NULL;
	struct drm_pending_vblank_event *blocked_event = NULL;
	int ret;

	if (!kms)
		return 0;
	if (!READ_ONCE(kms->snapshot_valid))
		return 0;
	ret = chrome9_engine_display_begin(cdev);
	if (ret) {
		if (kms->snapshot_valid)
			dev_crit(&cdev->pdev->dev,
				 "cannot serialize final IGA1 restore: %d\n", ret);
		return ret;
	}
	mutex_lock(&kms->lock);
	if (!kms->snapshot_valid)
		goto out_unlock;
	ret = chrome9_vga_seq_get(cdev);
	if (ret) {
		dev_crit(&cdev->pdev->dev,
			 "cannot lock VGA I/O for final IGA1 restore: %d\n", ret);
		goto out_unlock;
	}
	chrome9_iga1_restore(kms);
	kms->iga1_enabled = false;
	display_fb = kms->active_fb;
	kms->active_fb = NULL;
	retired_fb = kms->retired_fb;
	kms->retired_fb = NULL;
	blocked_fb = kms->blocked_fb;
	kms->blocked_fb = NULL;
	blocked_event = kms->blocked_event;
	kms->blocked_event = NULL;
	kms->flip_blocked = false;
	chrome9_vga_seq_put(cdev);

out_unlock:
	mutex_unlock(&kms->lock);
	chrome9_engine_display_end(cdev);
	if (display_fb) {
		chrome9_gem_fb_unpin(display_fb);
		drm_framebuffer_put(display_fb);
	}
	if (retired_fb) {
		chrome9_gem_fb_unpin(retired_fb);
		drm_framebuffer_put(retired_fb);
	}
	if (blocked_fb) {
		chrome9_gem_fb_unpin(blocked_fb);
		drm_framebuffer_put(blocked_fb);
	}
	chrome9_pipe_send_event(&cdev->display_pipe, blocked_event);
	return ret;
}
