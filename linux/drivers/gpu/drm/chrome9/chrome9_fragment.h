/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Chrome9 contributors */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_FRAGMENT_H__
#define __CHROME9_FRAGMENT_H__

#include <linux/types.h>
#include <drm/chrome9_drm.h>

struct chrome9_fragment_program {
	u32 alu_count;
	u32 tau_count;
	u32 constant_count;
	u32 texture_count;
	u32 varying_count;
	u32 active_texture_count;
	u8 tau_coordinate_mask;
	u8 varying_logical[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 varying_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 varying_alu_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u32 controls[12];
	u32 alu[DRM_CHROME9_FRAGMENT_MAX_ALU][4];
	u32 tau[DRM_CHROME9_FRAGMENT_MAX_TAU];
	u32 constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
};

int chrome9_fragment_compile(
	const struct drm_chrome9_fragment_instruction *instructions,
	u32 instruction_count, const u32 *constants, u32 constant_count,
	u32 texture_count, u32 varying_count,
	struct chrome9_fragment_program *program);
int chrome9_fragment_compile_sparse(
	const struct drm_chrome9_fragment_instruction *instructions,
	u32 instruction_count, const u32 *constants, u32 constant_count,
	u32 texture_count, u32 descriptor_mask, u32 varying_count,
	struct chrome9_fragment_program *program);
int chrome9_fragment_selftest(void);

#endif
