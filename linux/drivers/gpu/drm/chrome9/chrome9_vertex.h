/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_VERTEX_H__
#define __CHROME9_VERTEX_H__

#include <linux/types.h>
#include <drm/chrome9_drm.h>

struct chrome9_fragment_program;

/* Static flow control and relative addressing accepted by
 * chrome9_vertex_compile(). */
struct chrome9_vertex_flow {
	/* Maximum REP/IF nesting; zero rejects every flow op. */
	u32 max_depth;
	/* ARL and a0-relative constant reads. */
	bool relative;
	/* RE: ARL as one bare MOVA, without floor and bounds clamp. */
	bool relative_raw;
};

struct chrome9_vertex_program {
	u32 record_count;
	u32 active_count;
	u32 physical_temps;
	u32 input_count;
	u32 a60;
	u32 a61;
	u32 a62;
	/* REP or IF records: not relocated in the VS store. */
	bool flow;
	/* ARL clamp rows c[constant_end] (low) and c[constant_end + 1]
	 * (high) as float bits per a0 lane, uploaded when bound_rows. */
	bool bound_rows;
	u32 bounds[2][4];
	u32 records[][4];
};

/* Bytes of a program holding n records; compile output needs the maximum. */
#define CHROME9_VERTEX_PROGRAM_BYTES(n) \
	(sizeof(struct chrome9_vertex_program) + (n) * 4 * sizeof(u32))

/* program holds CHROME9_VERTEX_PROGRAM_BYTES(DRM_CHROME9_VERTEX_MAX_RECORDS). */
int chrome9_vertex_compile(
	const struct drm_chrome9_vertex_instruction *instructions,
	u32 instruction_count, const struct chrome9_fragment_program *fragment,
	u32 input_count, const u8 *input_lanes, u32 constant_first,
	u32 constant_count, bool two_side, u32 clip_mask, bool point_size,
	u32 sprite_mask, const struct chrome9_vertex_flow *flow,
	struct chrome9_vertex_program *program);
int chrome9_vertex_selftest(void);

#endif
