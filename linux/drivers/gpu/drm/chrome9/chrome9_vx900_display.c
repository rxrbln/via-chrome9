// SPDX-License-Identifier: GPL-2.0-only
/*
 * VX900 IGA1 timing, fetch, FIFO and PLL programming.
 *
 * The register packing and PLL search are adapted from the in-tree viafb
 * driver (Copyright VIA Technologies, S3 Graphics and Florian Tobias
 * Schandinat).  This file deliberately contains no fbdev, global-device or
 * output-routing state: DRM atomic KMS owns those policy decisions.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 */

#include <linux/kernel.h>
#include <linux/math64.h>

#include <video/vga.h>

#include <drm/drm_modes.h>

#include "chrome9_vx900_display.h"

struct chrome9_pll_limit {
	u16 multiplier_min;
	u16 multiplier_max;
	u8 divisor;
	u8 rshift;
};

struct chrome9_pll {
	u16 multiplier;
	u8 divisor;
	u8 rshift;
};

/* Immutable, fully validated IGA1 register image built before hardware I/O. */
struct chrome9_iga1_mode_state {
	u8 cr00, cr01, cr02, cr03, cr04, cr05, cr06, cr07;
	u8 cr09, cr10, cr11, cr12, cr13, cr15, cr16;
	u8 cr33, cr35_timing, cr35_pitch, cr36, cr45;
	u8 sr1c, sr1d, sr16, sr17, sr18, sr22;
	u8 sr44, sr45, sr46;
	u8 misc;
};

/* viafb's experimentally qualified VX855/VX900 limits. */
static const struct chrome9_pll_limit chrome9_vx900_pll_limits[] = {
	{ 86,  86, 4, 1 },
	{108, 208, 5, 1 },
	{110, 208, 5, 2 },
	{ 83, 112, 3, 3 },
	{103, 161, 4, 3 },
	{112, 209, 5, 3 },
	{142, 161, 4, 4 },
	{141, 176, 5, 4 },
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

static u32 chrome9_pll_rate(const struct chrome9_pll *pll)
{
	u64 rate = 14318180ULL * pll->multiplier;

	do_div(rate, (u32)pll->divisor << pll->rshift);
	return rate;
}

static struct chrome9_pll chrome9_vx900_find_pll(u32 clock_hz)
{
	struct chrome9_pll best = { .multiplier = 86, .divisor = 4,
				    .rshift = 1 };
	u32 best_error = U32_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(chrome9_vx900_pll_limits); i++) {
		const struct chrome9_pll_limit *limit =
			&chrome9_vx900_pll_limits[i];
		struct chrome9_pll pll = {
			.divisor = limit->divisor,
			.rshift = limit->rshift,
		};
		u64 numerator = (u64)clock_hz * pll.divisor << pll.rshift;
		u32 error;

		pll.multiplier = DIV_ROUND_CLOSEST_ULL(numerator, 14318180);
		pll.multiplier = clamp_t(u16, pll.multiplier,
					 limit->multiplier_min,
					 limit->multiplier_max);
		error = abs((s32)chrome9_pll_rate(&pll) - (s32)clock_hz);
		if (error < best_error) {
			best = pll;
			best_error = error;
		}
	}
	return best;
}

static int
chrome9_vx900_iga1_mode_build(const struct drm_display_mode *mode, u32 pitch,
			      struct chrome9_iga1_mode_state *state)
{
	struct chrome9_pll pll;
	u32 htotal, hdisplay, hblank_start, hblank_end;
	u32 hsync_start, hsync_end;
	u32 vtotal, vdisplay, vblank_start, vblank_end;
	u32 vsync_start, vsync_end;
	u32 clock_hz;
	u32 error;
	u32 fetch;

	if (!mode || mode->clock < 20000 || mode->clock > 230000 ||
	    mode->vscan > 1 ||
	    drm_mode_vrefresh(mode) < 40 || drm_mode_vrefresh(mode) > 200 ||
	    mode->flags & ~(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			    DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC) ||
	    (mode->flags & (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC)) ==
		(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC) ||
	    (mode->flags & (DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC)) ==
		(DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC) ||
	    mode->hdisplay < 320 || mode->hdisplay > 2048 ||
	    mode->vdisplay < 200 || mode->vdisplay > 2048 ||
	    mode->htotal < mode->hsync_end ||
	    mode->hsync_end <= mode->hsync_start ||
	    mode->hsync_start < mode->hdisplay ||
	    mode->vtotal < mode->vsync_end ||
	    mode->vsync_end <= mode->vsync_start ||
	    mode->vsync_start < mode->vdisplay ||
	    /* Sync and total program in 8-pixel characters, truncated like
	     * the vendor drivers do (CEA 720p and 1080p are not aligned). */
	    mode->hdisplay & 7 ||
	    mode->hsync_end / 8 - 1 <= mode->hsync_start / 8 ||
	    mode->htotal - mode->hdisplay > 127 * 8 ||
	    mode->hsync_end - mode->hsync_start > 31 * 8 ||
	    mode->vtotal - mode->vdisplay > 255 ||
	    mode->vsync_end - mode->vsync_start > 15 ||
	    !IS_ALIGNED(pitch, 8) || pitch < mode->hdisplay * 4 ||
	    pitch / 8 > 0x7ff)
		return -EINVAL;

	/* openchrome's IGA1 encoding: blank and sync starts are not biased. */
	htotal = mode->htotal / 8 - 5;
	hdisplay = mode->hdisplay / 8 - 1;
	hblank_start = mode->hdisplay / 8;
	hblank_end = mode->htotal / 8 - 1;
	hsync_start = mode->hsync_start / 8;
	hsync_end = mode->hsync_end / 8 - 1;
	vtotal = mode->vtotal - 2;
	vdisplay = mode->vdisplay - 1;
	vblank_start = mode->vdisplay;
	vblank_end = mode->vtotal - 1;
	vsync_start = mode->vsync_start;
	vsync_end = mode->vsync_end - 1;
	if (htotal > 0x1ff || hdisplay > U8_MAX ||
	    hblank_start > 0x1ff || hsync_start > 0x1ff ||
	    vtotal > 0x7ff || vdisplay > 0x7ff ||
	    vblank_start > 0x7ff || vsync_start > 0x7ff)
		return -EINVAL;

	clock_hz = mode->clock * 1000;
	pll = chrome9_vx900_find_pll(clock_hz);
	error = abs((s32)chrome9_pll_rate(&pll) - (s32)clock_hz);
	if (error > max_t(u32, clock_hz / 200, 100000))
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	state->cr00 = htotal;
	state->cr01 = hdisplay;
	state->cr02 = hblank_start;
	state->cr45 = ((hblank_start >> 8) & 1) ? BIT(2) : 0;
	state->cr03 = 0x80 | (hblank_end & 0x1f);
	state->cr04 = hsync_start;
	state->cr05 = (hsync_end & 0x1f) | ((hblank_end << 2) & 0x80);
	state->cr06 = vtotal;
	state->cr07 = BIT(4) |
		      BIT(0) * ((vtotal >> 8) & 1) |
		      BIT(1) * ((vdisplay >> 8) & 1) |
		      BIT(2) * ((vsync_start >> 8) & 1) |
		      BIT(3) * ((vblank_start >> 8) & 1) |
		      BIT(5) * ((vtotal >> 9) & 1) |
		      BIT(6) * ((vdisplay >> 9) & 1) |
		      BIT(7) * ((vsync_start >> 9) & 1);
	state->cr09 = ((vblank_start >> 9) & 1) ? BIT(5) : 0;
	state->cr10 = vsync_start;
	state->cr11 = vsync_end & 0x0f;
	state->cr12 = vdisplay;
	state->cr13 = pitch >> 3;
	state->cr15 = vblank_start;
	state->cr16 = vblank_end;
	state->cr33 = ((hsync_start >> 4) & BIT(4)) |
		      ((hblank_end >> 1) & BIT(5));
	state->cr35_timing = ((vtotal >> 10) & 1) |
			     (((vsync_start >> 10) & 1) << 1) |
			     (((vdisplay >> 10) & 1) << 2) |
			     (((vblank_start >> 10) & 1) << 3) |
			     /* Line compare bit 10: no split screen past line 1023. */
			     BIT(4);
	state->cr35_pitch = (pitch >> 6) & 0xe0;
	state->cr36 = ((htotal >> 8) & 1) ? BIT(3) : 0;
	fetch = mode->hdisplay / 4 + 4;
	if (fetch > 0x3ff)
		return -EINVAL;
	state->sr1c = fetch;
	state->sr1d = fetch >> 8;
	state->sr17 = 0xc7;
	state->sr16 = 0x90;
	state->sr18 = 0x90;
	state->sr22 = 0x08;
	state->sr44 = pll.multiplier;
	state->sr45 = pll.rshift << 2;
	state->sr46 = pll.divisor;
	state->misc = 0x0c;
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		state->misc |= BIT(6);
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		state->misc |= BIT(7);
	return 0;
}

bool chrome9_vx900_iga1_mode_valid(const struct drm_display_mode *mode)
{
	struct chrome9_iga1_mode_state state;
	u32 pitch;

	if (!mode || check_mul_overflow((u32)mode->hdisplay, 4U, &pitch))
		return false;
	pitch = ALIGN(pitch, 8);
	return !chrome9_vx900_iga1_mode_build(mode, pitch, &state);
}

int chrome9_vx900_iga1_program_mode(const struct drm_display_mode *mode,
				    u32 pitch)
{
	struct chrome9_iga1_mode_state state;
	u8 misc;
	int ret;

	ret = chrome9_vx900_iga1_mode_build(mode, pitch, &state);
	if (ret)
		return ret;

	chrome9_cr_mask(0x11, BIT(7), 0);
	vga_io_wcrt(0x00, state.cr00);
	vga_io_wcrt(0x01, state.cr01);
	vga_io_wcrt(0x02, state.cr02);
	chrome9_cr_mask(0x03, 0x9f, state.cr03);
	vga_io_wcrt(0x04, state.cr04);
	chrome9_cr_mask(0x05, 0x9f, state.cr05);
	vga_io_wcrt(0x06, state.cr06);
	vga_io_wcrt(0x07, state.cr07);
	chrome9_cr_mask(0x09, BIT(5), state.cr09);
	vga_io_wcrt(0x10, state.cr10);
	chrome9_cr_mask(0x11, 0x0f, state.cr11);
	vga_io_wcrt(0x12, state.cr12);
	vga_io_wcrt(0x15, state.cr15);
	vga_io_wcrt(0x16, state.cr16);
	chrome9_cr_mask(0x33, 0x30, state.cr33);
	chrome9_cr_mask(0x35, 0x1f, state.cr35_timing);
	chrome9_cr_mask(0x36, BIT(3), state.cr36);
	chrome9_cr_mask(0x45, BIT(2), state.cr45);
	chrome9_cr_mask(0x11, BIT(7), BIT(7));
	vga_io_wcrt(0x17, 0x63);
	vga_io_wcrt(0x17, 0xe3);

	/* viafb primary pitch and 32-bpp fetch-count packing. */
	vga_io_wcrt(0x13, state.cr13);
	chrome9_cr_mask(0x35, 0xe0, state.cr35_pitch);
	vga_io_wseq(0x1c, state.sr1c);
	chrome9_sr_mask(0x1d, 0x03, state.sr1d);

	/* VX900-qualified FIFO tuple: 400 / 320 / 320 / 160. */
	vga_io_wseq(0x17, state.sr17);
	chrome9_sr_mask(0x16, 0xbf, state.sr16);
	chrome9_sr_mask(0x18, 0xbf, state.sr18);
	chrome9_sr_mask(0x22, 0x1f, state.sr22);

	chrome9_sr_mask(0x40, BIT(1), BIT(1));
	vga_io_wseq(0x44, state.sr44);
	vga_io_wseq(0x45, state.sr45);
	vga_io_wseq(0x46, state.sr46);
	chrome9_sr_mask(0x40, BIT(1), 0);

	misc = vga_io_r(VGA_MIS_R) & ~0xcc;
	misc |= state.misc;
	vga_io_w(VGA_MIS_W, misc);
	chrome9_cr_mask(0x6c, 0xf0, 0x00);
	chrome9_sr_mask(0x2d, 0x30, 0x20);
	chrome9_sr_mask(0x1b, 0x30, 0x20);
	return 0;
}

int chrome9_vx900_display_selftest(void)
{
	const struct drm_display_mode mode = {
		.clock = 65000,
		.hdisplay = 1024, .hsync_start = 1048,
		.hsync_end = 1184, .htotal = 1344,
		.vdisplay = 768, .vsync_start = 771,
		.vsync_end = 777, .vtotal = 806,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	};
	const struct drm_display_mode wide_mode = {
		.clock = 162000,
		.hdisplay = 1600, .hsync_start = 1664,
		.hsync_end = 1856, .htotal = 2160,
		.vdisplay = 1200, .vsync_start = 1201,
		.vsync_end = 1204, .vtotal = 1250,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	};
	struct chrome9_iga1_mode_state state;
	int ret;

	ret = chrome9_vx900_iga1_mode_build(&mode, 4096, &state);
	if (ret)
		return ret;
	/* Timing bytes as captured from openchrome driving this mode. */
	if (state.cr00 != 0xa3 || state.cr01 != 0x7f ||
	    state.cr02 != 0x80 || state.cr03 != 0x87 ||
	    state.cr04 != 0x83 || state.cr05 != 0x93 ||
	    state.cr06 != 0x24 || state.cr07 != 0xfd ||
	    state.cr09 != 0x20 || state.cr10 != 0x03 ||
	    state.cr11 != 0x08 || state.cr12 != 0xff ||
	    state.cr13 != 0x00 || state.cr15 != 0x00 ||
	    state.cr16 != 0x25 || state.cr33 != 0x00 ||
	    state.cr35_timing != 0x10 || state.cr35_pitch != 0x40 ||
	    state.cr45 != 0x00 ||
	    state.cr36 != 0x00 || state.sr1c != 0x04 ||
	    state.sr1d != 0x01 || state.sr44 != 0x6d ||
	    state.sr45 != 0x0c || state.sr46 != 0x03 ||
	    state.misc != 0xcc)
		return -EINVAL;
	ret = chrome9_vx900_iga1_mode_build(&wide_mode, 6400, &state);
	if (ret || state.cr00 != 0x09 || state.cr36 != BIT(3))
		return -EINVAL;
	return 0;
}
