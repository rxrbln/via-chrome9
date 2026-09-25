/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_FRAGMENT_H__
#define __CHROME9_FRAGMENT_H__

#include <linux/types.h>
#include <drm/chrome9_drm.h>

/* A95..A9A address 144 ALU records; ps_alu_max probes the store above 96. */
#define CHROME9_FRAGMENT_STORE_ALU	144U
#define CHROME9_FRAGMENT_STORE_INSTRUCTIONS \
	(CHROME9_FRAGMENT_STORE_ALU + DRM_CHROME9_FRAGMENT_MAX_TAU)

u32 chrome9_fragment_alu_limit(void);
u32 chrome9_fragment_instruction_limit(void);

struct chrome9_fragment_program {
	u32 alu_count;
	u32 tau_count;
	u32 constant_count;
	u32 texture_count;
	u32 varying_count;
	u32 active_texture_count;
	u8 tau_coordinate_mask;
	bool primary_used;
	/* Shader depth replaces raster depth (A13 bit 23, HZSrcPS). */
	bool depth_output;
	/* oC0..oC(color_outputs - 1): A01 target count. */
	u8 color_outputs;
	u8 varying_logical[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 varying_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 varying_alu_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u32 controls[12];
	u32 alu[CHROME9_FRAGMENT_STORE_ALU][4];
	u32 tau[DRM_CHROME9_FRAGMENT_MAX_TAU];
};

bool chrome9_fragment_constants_valid(const u32 *constants, u32 constant_count);

/* Constants are validated, not copied: the draw emits the caller's copy. */
int chrome9_fragment_compile(
	const struct drm_chrome9_fragment_instruction *instructions,
	u32 instruction_count, const u32 *constants, u32 constant_count,
	u32 texture_count, u32 varying_count,
	struct chrome9_fragment_program *program);
/* str_mask: samplers of TARGET_CUBE and TARGET_3D textures (TEX reads STR). */
int chrome9_fragment_compile_targets(
	const struct drm_chrome9_fragment_instruction *instructions,
	u32 instruction_count, const u32 *constants, u32 constant_count,
	u32 texture_count, u32 descriptor_mask, bool sparse_textures,
	u32 str_mask, u32 varying_count,
	struct chrome9_fragment_program *program);
int chrome9_fragment_compile_sparse(
	const struct drm_chrome9_fragment_instruction *instructions,
	u32 instruction_count, const u32 *constants, u32 constant_count,
	u32 texture_count, u32 descriptor_mask, u32 varying_count,
	struct chrome9_fragment_program *program);
int chrome9_fragment_selftest(void);

#endif
