/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_VERTEX_H__
#define __CHROME9_VERTEX_H__

#include <linux/types.h>
#include <drm/chrome9_drm.h>

struct chrome9_fragment_program;

struct chrome9_vertex_program {
	u32 record_count;
	u32 active_count;
	u32 physical_temps;
	u32 input_count;
	u32 a60;
	u32 a61;
	u32 a62;
	u32 records[DRM_CHROME9_VERTEX_MAX_RECORDS][4];
};

int chrome9_vertex_compile(
	const struct drm_chrome9_vertex_instruction *instructions,
	u32 instruction_count, const struct chrome9_fragment_program *fragment,
	u32 input_count, const u8 *input_lanes, u32 constant_first,
	u32 constant_count, bool two_side, u32 clip_mask, bool point_size,
	u32 sprite_mask, struct chrome9_vertex_program *program);
int chrome9_vertex_selftest(void);

#endif
