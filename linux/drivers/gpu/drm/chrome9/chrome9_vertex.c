// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "chrome9_fragment.h"
#include "chrome9_vertex.h"

#define CHROME9_VERTEX_PHYSICAL_TEMPS	16U
/* Hidden virtual temp collecting lane-wise POSITION writes. */
#define CHROME9_VERTEX_POSITION_TEMP	DRM_CHROME9_VERTEX_MAX_TEMPS
#define CHROME9_VERTEX_NO_PHYSICAL	0xff
#define CHROME9_VERTEX_FINALIZER_RECORDS 5U
#define CHROME9_VERTEX_INACTIVE_TAILS	2U
/* D3D9 vs_2_0 static flow control nesting limit. */
#define CHROME9_VERTEX_MAX_FLOW_DEPTH	4U
#define CHROME9_VERTEX_NO_DEF		0xffffU

#define CHROME9_VS_UNIFIED_INPUT	32U
#define CHROME9_VS_UNIFIED_TEMP		64U
#define CHROME9_VS_UNIFIED_INT		96U
#define CHROME9_VS_UNIFIED_BOOL		112U
#define CHROME9_VS_UNIFIED_LITERAL	129U
#define CHROME9_VS_UNIFIED_CONSTANT	256U
/* MOVA destination; its mask selects the a0 lanes. */
#define CHROME9_VS_DEST_ADDRESS		0x01U
#define CHROME9_VS_DEST_TEMP		0x10U
#define CHROME9_VS_DEST_D0		0x22U
#define CHROME9_VS_DEST_D1		0x23U
/* Back-face colors oBFD0/oBFD1 (vendor oD2/oD3, unified outputs 36/37). */
#define CHROME9_VS_DEST_BFD0		0x24U
#define CHROME9_VS_DEST_BFD1		0x25U
/* D3D oPts, unified output 39 (A25 bit 19 in the primitive vertex). */
#define CHROME9_VS_DEST_POINT_SIZE	0x27U
#define CHROME9_VS_DEST_T		0x28U
#define CHROME9_VS_DEST_POSITION	0xa0U
#define CHROME9_VS_LITERAL_ONE		0x00206490U
#define CHROME9_VS_LITERAL_ZERO		0x00206db4U
/* (0, 0, 0, 1): selectors ZERO, ZERO, ZERO, ONE. */
#define CHROME9_VS_LITERAL_W1		0x00206db0U
/* Op 28/29: taken and not-taken displacements in bits 18:10 of dwords 1
 * and 2; DRM_CHROME9_VERTEX_MAX_RECORDS keeps them below 512. */
#define CHROME9_VS_FLOW_TAKEN		0x00280000U
#define CHROME9_VS_FLOW_NOT_TAKEN	0x00300000U
#define CHROME9_VS_ELSE_SOURCE		0x0020a490U
#define CHROME9_VS_IDENTITY		0x14cU
/* REP reads i#.xyz0 (vendor rep/loop records). */
#define CHROME9_VS_INT_SELECT		0x154U
/* dword3 bits 27:24 select a0.x..a0.w as 4..7 for the constant source. */
#define CHROME9_VS_RELATIVE_SHIFT	24
#define CHROME9_VS_RELATIVE_A0_X	4U

enum {
	CHROME9_VS_NATIVE_MAD = 2,
	CHROME9_VS_NATIVE_DP = 3,
	CHROME9_VS_NATIVE_MOV = 4,
	CHROME9_VS_NATIVE_MOVA = 5,
	CHROME9_VS_NATIVE_FLR = 6,
	CHROME9_VS_NATIVE_FRC = 7,
	CHROME9_VS_NATIVE_MAX = 8,
	CHROME9_VS_NATIVE_MIN = 9,
	CHROME9_VS_NATIVE_SGE = 10,
	CHROME9_VS_NATIVE_SLT = 11,
	CHROME9_VS_NATIVE_SGN = 12,
	CHROME9_VS_NATIVE_EXP2 = 16,
	CHROME9_VS_NATIVE_LOG2 = 18,
	CHROME9_VS_NATIVE_RCP = 20,
	CHROME9_VS_NATIVE_RSQ = 21,
	CHROME9_VS_NATIVE_LOOP = 28,
	CHROME9_VS_NATIVE_BRANCH = 29,
};

static const u8 chrome9_vertex_source_count[DRM_CHROME9_VERTEX_OP_COUNT] = {
	[DRM_CHROME9_VERTEX_OP_MOV] = 1, [DRM_CHROME9_VERTEX_OP_ADD] = 2,
	[DRM_CHROME9_VERTEX_OP_SUB] = 2, [DRM_CHROME9_VERTEX_OP_MUL] = 2,
	[DRM_CHROME9_VERTEX_OP_MAD] = 3, [DRM_CHROME9_VERTEX_OP_DP3] = 2,
	[DRM_CHROME9_VERTEX_OP_DP4] = 2, [DRM_CHROME9_VERTEX_OP_DPH] = 2,
	[DRM_CHROME9_VERTEX_OP_MIN] = 2, [DRM_CHROME9_VERTEX_OP_MAX] = 2,
	[DRM_CHROME9_VERTEX_OP_ABS] = 1, [DRM_CHROME9_VERTEX_OP_SGE] = 2,
	[DRM_CHROME9_VERTEX_OP_SLT] = 2, [DRM_CHROME9_VERTEX_OP_SGN] = 1,
	[DRM_CHROME9_VERTEX_OP_FRC] = 1, [DRM_CHROME9_VERTEX_OP_FLR] = 1,
	[DRM_CHROME9_VERTEX_OP_RCP] = 1, [DRM_CHROME9_VERTEX_OP_RSQ] = 1,
	[DRM_CHROME9_VERTEX_OP_EXP2] = 1, [DRM_CHROME9_VERTEX_OP_LOG2] = 1,
	[DRM_CHROME9_VERTEX_OP_ARL] = 1,
};

static const u8 chrome9_vertex_native_op[DRM_CHROME9_VERTEX_OP_COUNT] = {
	[DRM_CHROME9_VERTEX_OP_MOV] = CHROME9_VS_NATIVE_MOV,
	[DRM_CHROME9_VERTEX_OP_ADD] = CHROME9_VS_NATIVE_MAD,
	[DRM_CHROME9_VERTEX_OP_SUB] = CHROME9_VS_NATIVE_MAD,
	[DRM_CHROME9_VERTEX_OP_MUL] = CHROME9_VS_NATIVE_MAD,
	[DRM_CHROME9_VERTEX_OP_MAD] = CHROME9_VS_NATIVE_MAD,
	[DRM_CHROME9_VERTEX_OP_DP3] = CHROME9_VS_NATIVE_DP,
	[DRM_CHROME9_VERTEX_OP_DP4] = CHROME9_VS_NATIVE_DP,
	[DRM_CHROME9_VERTEX_OP_DPH] = CHROME9_VS_NATIVE_DP,
	[DRM_CHROME9_VERTEX_OP_MIN] = CHROME9_VS_NATIVE_MIN,
	[DRM_CHROME9_VERTEX_OP_MAX] = CHROME9_VS_NATIVE_MAX,
	[DRM_CHROME9_VERTEX_OP_ABS] = CHROME9_VS_NATIVE_MAX,
	[DRM_CHROME9_VERTEX_OP_SGE] = CHROME9_VS_NATIVE_SGE,
	[DRM_CHROME9_VERTEX_OP_SLT] = CHROME9_VS_NATIVE_SLT,
	[DRM_CHROME9_VERTEX_OP_SGN] = CHROME9_VS_NATIVE_SGN,
	[DRM_CHROME9_VERTEX_OP_FRC] = CHROME9_VS_NATIVE_FRC,
	[DRM_CHROME9_VERTEX_OP_FLR] = CHROME9_VS_NATIVE_FLR,
	[DRM_CHROME9_VERTEX_OP_RCP] = CHROME9_VS_NATIVE_RCP,
	[DRM_CHROME9_VERTEX_OP_RSQ] = CHROME9_VS_NATIVE_RSQ,
	[DRM_CHROME9_VERTEX_OP_EXP2] = CHROME9_VS_NATIVE_EXP2,
	[DRM_CHROME9_VERTEX_OP_LOG2] = CHROME9_VS_NATIVE_LOG2,
	[DRM_CHROME9_VERTEX_OP_ARL] = CHROME9_VS_NATIVE_MOVA,
};

/* RCP r0.w = 1/w, op14 r0.xyz, viewport MAD into output 33, two tails. */
static const u32 chrome9_vertex_finalizer[CHROME9_VERTEX_FINALIZER_RECORDS][4] = {
	{ 0x00201b6c, 0x00000000, 0x00000000, 0x00280108 },
	{ 0x00200150, 0x00101b6c, 0x00201b6c, 0x001c0107 },
	{ 0x0010014c, 0x00380150, 0x00384154, 0x0004021f },
	{ 0x0020014c, 0x00000000, 0x00000000, 0x0008020f },
	{ 0x0020014c, 0x00000000, 0x00000000, 0x0008020f },
};

/* One open REP or IF: defined lanes at entry and after the first arm,
 * and the native records to patch. */
struct chrome9_vertex_frame {
	u8 op;
	bool has_else;
	u32 start;
	u32 record;
	u32 else_record;
	u8 entry[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u8 then[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u8 address_entry;
	u8 address_then;
};

struct chrome9_vertex_state {
	const struct drm_chrome9_vertex_instruction *instructions;
	u32 instruction_count;
	u32 input_count;
	const u8 *input_lanes;
	u32 constant_first;
	u32 constant_end;
	u8 defined[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u16 last_touch[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u16 first_def[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u8 physical[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u16 physical_live;
	u32 physical_high_water;
	u8 position_mask;
	u8 primary_mask;
	u8 back_mask;
	bool two_side;
	/* Enabled planes, written planes and the first UCP coordinate. */
	u32 clip_mask;
	u32 clip_written;
	u32 clip_base;
	/* Point size output used, written; sprite-replaced logical varyings. */
	bool point_size;
	bool point_size_written;
	u32 sprite_mask;
	u8 varying_mask[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 varying_packed[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u32 position_writes;
	/* Effective destination mask; zero drops an unconsumed output. */
	u8 writemask[DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS];
	bool position_direct;
	int last_input_record;
	struct chrome9_vertex_frame *frames;
	u32 max_depth;
	u32 depth;
	bool dynamic;
	bool relative;
	bool relative_raw;
	bool arl;
	/* Defined a0 lanes, lanes read relatively and their base rows. */
	u8 address;
	u8 address_read;
	u8 base_min[4];
	u8 base_max[4];
};

static bool chrome9_vertex_scalar_op(u8 op)
{
	return op >= DRM_CHROME9_VERTEX_OP_RCP && op <= DRM_CHROME9_VERTEX_OP_LOG2;
}

static bool chrome9_vertex_flow_op(u8 op)
{
	return op >= DRM_CHROME9_VERTEX_OP_REP && op <= DRM_CHROME9_VERTEX_OP_ENDIF;
}

/* DP3 ignores W on both sources, DPH forces source 0 W to one. */
static u8 chrome9_vertex_selector(const struct drm_chrome9_vertex_instruction *insn,
				  unsigned int source, unsigned int lane)
{
	if (lane == 3 && insn->op == DRM_CHROME9_VERTEX_OP_DP3)
		return DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO;
	if (lane == 3 && source == 0 && insn->op == DRM_CHROME9_VERTEX_OP_DPH)
		return DRM_CHROME9_FRAGMENT_SWIZZLE_ONE;
	return insn->src[source].swizzle[lane];
}

static u8 chrome9_vertex_selected(const struct drm_chrome9_vertex_instruction *insn,
				  unsigned int source)
{
	u8 selected = 0;
	unsigned int lane;

	for (lane = 0; lane < 4; lane++) {
		u8 selector = chrome9_vertex_selector(insn, source, lane);

		if (selector < DRM_CHROME9_FRAGMENT_SWIZZLE_ONE)
			selected |= BIT(selector);
	}
	return selected;
}

/* c[index + a0.lane] after an ARL of that lane; records the base range. */
static int chrome9_vertex_validate_relative(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_source *src, u8 selected)
{
	unsigned int lane = src->relative - DRM_CHROME9_VERTEX_RELATIVE_A0_X;

	if (!(state->address & BIT(lane)))
		return -EINVAL;
	if (!selected)
		return 0;
	if (!(state->address_read & BIT(lane))) {
		state->base_min[lane] = src->index;
		state->base_max[lane] = src->index;
	}
	state->address_read |= BIT(lane);
	state->base_min[lane] = min(state->base_min[lane], src->index);
	state->base_max[lane] = max(state->base_max[lane], src->index);
	return 0;
}

static int chrome9_vertex_validate_source(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, unsigned int source,
	u32 instruction)
{
	const struct drm_chrome9_vertex_source *src = &insn->src[source];
	u8 selected;
	unsigned int lane;

	if (src->file > DRM_CHROME9_VERTEX_FILE_CONSTANT ||
	    src->modifier > DRM_CHROME9_FRAGMENT_MOD_NEG ||
	    src->relative > DRM_CHROME9_VERTEX_RELATIVE_A0_W ||
	    (src->relative && (src->file != DRM_CHROME9_VERTEX_FILE_CONSTANT ||
			       insn->op == DRM_CHROME9_VERTEX_OP_ARL)))
		return -EINVAL;
	for (lane = 0; lane < 4; lane++)
		if (src->swizzle[lane] > DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO)
			return -EINVAL;
	/* Silicon: the scalar unit reads constants incorrectly (RCP(1.25) =
	 * 1/1.5); temps and inputs are exact. */
	if (chrome9_vertex_scalar_op(insn->op) &&
	    src->file == DRM_CHROME9_VERTEX_FILE_CONSTANT)
		return -EINVAL;
	if (chrome9_vertex_scalar_op(insn->op))
		for (lane = 1; lane < 4; lane++)
			if (src->swizzle[lane] != src->swizzle[0])
				return -EINVAL;
	selected = chrome9_vertex_selected(insn, source);
	switch (src->file) {
	case DRM_CHROME9_VERTEX_FILE_TEMP:
		if (src->index >= DRM_CHROME9_VERTEX_MAX_TEMPS ||
		    (selected & ~state->defined[src->index]))
			return -EINVAL;
		if (selected)
			state->last_touch[src->index] = instruction;
		break;
	case DRM_CHROME9_VERTEX_FILE_INPUT:
		if (src->index >= state->input_count ||
		    (state->input_lanes &&
		     (selected & ~state->input_lanes[src->index])))
			return -EINVAL;
		break;
	case DRM_CHROME9_VERTEX_FILE_CONSTANT:
		if (src->relative)
			return chrome9_vertex_validate_relative(state, src, selected);
		if (src->index < state->constant_first ||
		    src->index >= state->constant_end)
			return -EINVAL;
		break;
	}
	return 0;
}

/*
 * Silicon: two distinct constants or two distinct inputs in one record
 * both read the last one (MAD v,c0,c1 = v*c1+c1), so one row of each per
 * record.  Distinct temps are fine.  The a0 selection is record-wide
 * (vendor rel_mixed_abs and rel_diff_comp), so a relative read is a row
 * of its own.
 */
static int chrome9_vertex_validate_ports(
	const struct drm_chrome9_vertex_instruction *insn, unsigned int count)
{
	unsigned int file, i, j, distinct;

	for (file = DRM_CHROME9_VERTEX_FILE_INPUT;
	     file <= DRM_CHROME9_VERTEX_FILE_CONSTANT; file++) {
		distinct = 0;
		for (i = 0; i < count; i++) {
			if (insn->src[i].file != file ||
			    !chrome9_vertex_selected(insn, i))
				continue;
			for (j = 0; j < i; j++)
				if (insn->src[j].file == file &&
				    insn->src[j].index == insn->src[i].index &&
				    insn->src[j].relative == insn->src[i].relative &&
				    chrome9_vertex_selected(insn, j))
					break;
			if (j == i)
				distinct++;
		}
		if (distinct > 1)
			return -EINVAL;
	}
	return 0;
}

static int chrome9_vertex_packed_varying(const struct chrome9_fragment_program *fragment,
					 u8 logical)
{
	unsigned int i;

	for (i = 0; i < fragment->varying_count; i++)
		if (fragment->varying_logical[i] == logical)
			return i;
	return -EINVAL;
}

/* Enabled plane k lands in UCP coordinate rank / 3, lane rank % 3. */
static u32 chrome9_vertex_clip_rank(const struct chrome9_vertex_state *state,
				    u32 plane)
{
	return hweight32(state->clip_mask & (BIT(plane) - 1));
}

static int chrome9_vertex_validate_dest(struct chrome9_vertex_state *state,
	const struct chrome9_fragment_program *fragment,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction)
{
	const struct drm_chrome9_vertex_destination *dst = &insn->dst;
	u8 mask = dst->writemask;
	u8 *defined;
	int packed;

	if (!mask || mask > 0xf || dst->modifier & ~DRM_CHROME9_FRAGMENT_DEST_SAT ||
	    (insn->op == DRM_CHROME9_VERTEX_OP_ARL) !=
	    (dst->file == DRM_CHROME9_VERTEX_DEST_ADDRESS))
		return -EINVAL;
	switch (dst->file) {
	case DRM_CHROME9_VERTEX_DEST_ADDRESS:
		if (!state->relative || dst->index || dst->modifier)
			return -EINVAL;
		state->arl = true;
		state->address |= mask;
		state->writemask[instruction] = mask;
		return 0;
	case DRM_CHROME9_VERTEX_DEST_TEMP:
		if (dst->index >= DRM_CHROME9_VERTEX_MAX_TEMPS)
			return -EINVAL;
		state->last_touch[dst->index] = instruction;
		if (state->first_def[dst->index] == CHROME9_VERTEX_NO_DEF)
			state->first_def[dst->index] = instruction;
		/* Flow regions may rewrite a temp (loop-carried values). */
		if (state->defined[dst->index] & mask && !state->depth)
			return -EINVAL;
		state->defined[dst->index] |= mask;
		state->writemask[instruction] = mask;
		return 0;
	case DRM_CHROME9_VERTEX_DEST_POSITION:
		if (dst->index)
			return -EINVAL;
		defined = &state->position_mask;
		state->position_writes++;
		break;
	case DRM_CHROME9_VERTEX_DEST_PRIMARY:
		if (dst->index)
			return -EINVAL;
		/* Outputs are write-only: drop what the fragment ignores. */
		if (!fragment->primary_used)
			mask = 0;
		defined = &state->primary_mask;
		break;
	case DRM_CHROME9_VERTEX_DEST_VARYING:
		if (dst->index >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			return -EINVAL;
		packed = chrome9_vertex_packed_varying(fragment, dst->index);
		/* Sprite coordinates replace the VS value. */
		if (packed < 0 || state->sprite_mask & BIT(dst->index)) {
			mask = 0;
			defined = &state->primary_mask;
			break;
		}
		mask &= GENMASK(fragment->varying_width[packed] - 1, 0);
		state->varying_packed[dst->index] = packed;
		defined = &state->varying_mask[packed];
		break;
	case DRM_CHROME9_VERTEX_DEST_BACK_PRIMARY:
		if (dst->index || !state->two_side)
			return -EINVAL;
		if (!fragment->primary_used)
			mask = 0;
		defined = &state->back_mask;
		break;
	case DRM_CHROME9_VERTEX_DEST_CLIP_DISTANCE:
		if (dst->index >= DRM_CHROME9_VERTEX_MAX_CLIP_DISTANCES ||
		    mask != 1)
			return -EINVAL;
		if (!(state->clip_mask & BIT(dst->index))) {
			state->writemask[instruction] = 0;
			return 0;
		}
		if (state->clip_written & BIT(dst->index))
			return -EINVAL;
		state->clip_written |= BIT(dst->index);
		state->writemask[instruction] =
			BIT(chrome9_vertex_clip_rank(state, dst->index) % 3);
		return 0;
	case DRM_CHROME9_VERTEX_DEST_POINT_SIZE:
		if (dst->index || mask != 1 || dst->modifier ||
		    (state->point_size && state->point_size_written))
			return -EINVAL;
		state->point_size_written = state->point_size;
		/* The vendor writes oPts with all lanes (0008027f). */
		state->writemask[instruction] = state->point_size ? 0xf : 0;
		return 0;
	default:
		return -EINVAL;
	}
	if (*defined & mask)
		return -EINVAL;
	*defined |= mask;
	state->writemask[instruction] = mask;
	return 0;
}

/* REP reads i#, IF b# or, with the RE knob, a temporary. */
static int chrome9_vertex_validate_condition(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction)
{
	const struct drm_chrome9_vertex_source *src = &insn->src[0];
	u8 file = insn->op == DRM_CHROME9_VERTEX_OP_REP ?
		  DRM_CHROME9_VERTEX_FILE_INT : DRM_CHROME9_VERTEX_FILE_BOOL;
	unsigned int lane;

	if (src->modifier || src->relative)
		return -EINVAL;
	if (src->file == file) {
		for (lane = 0; lane < 4; lane++)
			if (src->swizzle[lane] != lane)
				return -EINVAL;
		return src->index < (file == DRM_CHROME9_VERTEX_FILE_INT ?
				     DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS :
				     DRM_CHROME9_VERTEX_MAX_BOOL_CONSTANTS) ?
		       0 : -EINVAL;
	}
	/* The lane op 29 tests is unknown; the knob passes the swizzle. */
	if (insn->op != DRM_CHROME9_VERTEX_OP_IF || !state->dynamic ||
	    src->file != DRM_CHROME9_VERTEX_FILE_TEMP ||
	    !chrome9_vertex_selected(insn, 0))
		return -EINVAL;
	return chrome9_vertex_validate_source(state, insn, 0, instruction);
}

static int chrome9_vertex_validate_flow(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction)
{
	bool opens = insn->op == DRM_CHROME9_VERTEX_OP_REP ||
		     insn->op == DRM_CHROME9_VERTEX_OP_IF;
	struct chrome9_vertex_frame *frame;
	unsigned int s, t;
	int ret;

	if (!state->max_depth || memchr_inv(&insn->dst, 0, sizeof(insn->dst)))
		return -EINVAL;
	for (s = opens; s < ARRAY_SIZE(insn->src); s++)
		if (memchr_inv(&insn->src[s], 0, sizeof(insn->src[s])))
			return -EINVAL;
	if (opens) {
		if (state->depth == state->max_depth)
			return -EINVAL;
		ret = chrome9_vertex_validate_condition(state, insn, instruction);
		if (ret)
			return ret;
		frame = &state->frames[state->depth++];
		frame->op = insn->op;
		frame->has_else = false;
		frame->start = instruction;
		memcpy(frame->entry, state->defined, sizeof(frame->entry));
		frame->address_entry = state->address;
		return 0;
	}
	if (!state->depth)
		return -EINVAL;
	frame = &state->frames[state->depth - 1];
	switch (insn->op) {
	case DRM_CHROME9_VERTEX_OP_ELSE:
		if (frame->op != DRM_CHROME9_VERTEX_OP_IF || frame->has_else)
			return -EINVAL;
		frame->has_else = true;
		memcpy(frame->then, state->defined, sizeof(frame->then));
		memcpy(state->defined, frame->entry, sizeof(frame->entry));
		frame->address_then = state->address;
		state->address = frame->address_entry;
		return 0;
	case DRM_CHROME9_VERTEX_OP_ENDIF:
		if (frame->op != DRM_CHROME9_VERTEX_OP_IF)
			return -EINVAL;
		/* Only lanes written on both arms become defined. */
		for (t = 0; t < ARRAY_SIZE(state->defined); t++)
			state->defined[t] = frame->has_else ?
				state->defined[t] & frame->then[t] :
				frame->entry[t];
		state->address = frame->has_else ?
			state->address & frame->address_then :
			frame->address_entry;
		break;
	default:
		if (frame->op != DRM_CHROME9_VERTEX_OP_REP)
			return -EINVAL;
		/* The body may run zero times; a temp defined before the loop
		 * and touched inside stays live until its end. */
		for (t = 0; t < ARRAY_SIZE(state->defined); t++)
			if (state->first_def[t] < frame->start &&
			    state->last_touch[t] > frame->start)
				state->last_touch[t] = instruction;
		memcpy(state->defined, frame->entry, sizeof(frame->entry));
		state->address = frame->address_entry;
		break;
	}
	state->depth--;
	return 0;
}

static int chrome9_vertex_validate(struct chrome9_vertex_state *state,
	const struct chrome9_fragment_program *fragment)
{
	u32 i;
	int ret;

	for (i = 0; i < state->instruction_count; i++) {
		const struct drm_chrome9_vertex_instruction *insn =
			&state->instructions[i];
		unsigned int count, s;

		if (insn->op >= DRM_CHROME9_VERTEX_OP_COUNT ||
		    insn->reserved0 || insn->reserved1)
			return -EINVAL;
		if (chrome9_vertex_flow_op(insn->op)) {
			ret = chrome9_vertex_validate_flow(state, insn, i);
			if (ret)
				return ret;
			continue;
		}
		count = chrome9_vertex_source_count[insn->op];
		/* Inside flow control: temps and a0 only, inputs read before. */
		if (state->depth &&
		    insn->dst.file != DRM_CHROME9_VERTEX_DEST_TEMP &&
		    insn->dst.file != DRM_CHROME9_VERTEX_DEST_ADDRESS)
			return -EINVAL;
		for (s = 0; s < ARRAY_SIZE(insn->src); s++) {
			if (s >= count) {
				if (memchr_inv(&insn->src[s], 0, sizeof(insn->src[s])))
					return -EINVAL;
				continue;
			}
			if (state->depth &&
			    insn->src[s].file == DRM_CHROME9_VERTEX_FILE_INPUT &&
			    chrome9_vertex_selected(insn, s))
				return -EINVAL;
			ret = chrome9_vertex_validate_source(state, insn, s, i);
			if (ret)
				return ret;
		}
		ret = chrome9_vertex_validate_ports(insn, count);
		if (ret)
			return ret;
		ret = chrome9_vertex_validate_dest(state, fragment, insn, i);
		if (ret)
			return ret;
	}
	if (state->depth || state->position_mask != 0xf ||
	    (fragment->primary_used && state->primary_mask != 0xf) ||
	    (state->two_side && fragment->primary_used &&
	     state->back_mask != 0xf) ||
	    state->clip_written != state->clip_mask ||
	    state->point_size_written != state->point_size)
		return -EINVAL;
	for (i = 0; i < fragment->varying_count; i++)
		if (!(state->sprite_mask & BIT(fragment->varying_logical[i])) &&
		    state->varying_mask[i] !=
		    GENMASK(fragment->varying_width[i] - 1, 0))
			return -EINVAL;
	return 0;
}

/* Float bits of a small integer, exact (the kernel does not use the FPU). */
static u32 chrome9_vertex_float_bits(s32 value)
{
	u32 magnitude = value < 0 ? -value : value;
	u32 exponent;

	if (!value)
		return 0;
	exponent = fls(magnitude) - 1;
	return (value < 0 ? BIT(31) : 0) | (127 + exponent) << 23 |
	       ((magnitude << (23 - exponent)) & GENMASK(22, 0));
}

/*
 * Per a0 lane the ARL clamp [first - min base, end - 1 - max base] keeps
 * every relative read of the lane inside the uploaded rows; the kernel
 * rows c[end] and c[end + 1] hold the low and high bounds.
 */
static int chrome9_vertex_bounds(const struct chrome9_vertex_state *state,
				 struct chrome9_vertex_program *program)
{
	unsigned int lane;
	s32 low, high;

	if (!state->arl)
		return 0;
	if (state->constant_end + 2 > DRM_CHROME9_VERTEX_MAX_CONSTANTS)
		return -E2BIG;
	for (lane = 0; lane < 4; lane++) {
		if (!(state->address_read & BIT(lane)))
			continue;
		low = (s32)state->constant_first - state->base_min[lane];
		high = (s32)state->constant_end - 1 - state->base_max[lane];
		if (low > high)
			return -EINVAL;
		program->bounds[0][lane] = chrome9_vertex_float_bits(low);
		program->bounds[1][lane] = chrome9_vertex_float_bits(high);
	}
	program->bound_rows = true;
	return 0;
}

static u32 chrome9_vertex_source_word(const struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, unsigned int source,
	bool negate)
{
	const struct drm_chrome9_vertex_source *src = &insn->src[source];
	u32 unified, word = 0;
	unsigned int lane;

	for (lane = 0; lane < 4; lane++)
		word |= (u32)chrome9_vertex_selector(insn, source, lane) <<
			(11 - 3 * lane);
	if (!chrome9_vertex_selected(insn, source))
		unified = CHROME9_VS_UNIFIED_LITERAL;
	else if (src->file == DRM_CHROME9_VERTEX_FILE_TEMP)
		unified = CHROME9_VS_UNIFIED_TEMP + state->physical[src->index];
	else if (src->file == DRM_CHROME9_VERTEX_FILE_INPUT)
		unified = CHROME9_VS_UNIFIED_INPUT + src->index;
	else
		unified = CHROME9_VS_UNIFIED_CONSTANT + src->index;
	return unified << 14 | word |
	       ((src->modifier == DRM_CHROME9_FRAGMENT_MOD_NEG) ^ negate);
}

static u32 chrome9_vertex_dest_word(u32 op, bool sat, u32 dest, u32 mask)
{
	return op << 17 | (u32)sat << 14 | dest << 4 | mask;
}

/* The lowest free physical register; the caller marks it live or uses
 * it only within the records it emits next. */
static int chrome9_vertex_scratch(struct chrome9_vertex_state *state,
				  u32 *physical)
{
	u32 p;

	for (p = 0; p < CHROME9_VERTEX_PHYSICAL_TEMPS; p++)
		if (!(state->physical_live & BIT(p)))
			break;
	if (p == CHROME9_VERTEX_PHYSICAL_TEMPS)
		return -E2BIG;
	state->physical_high_water = max(state->physical_high_water, p + 1);
	*physical = p;
	return 0;
}

static int chrome9_vertex_allocate(struct chrome9_vertex_state *state, u32 temp)
{
	u32 physical;
	int ret;

	if (state->physical[temp] != CHROME9_VERTEX_NO_PHYSICAL)
		return 0;
	ret = chrome9_vertex_scratch(state, &physical);
	if (ret)
		return ret;
	state->physical[temp] = physical;
	state->physical_live |= BIT(physical);
	return 0;
}

static void chrome9_vertex_release(struct chrome9_vertex_state *state,
				   u32 temp, u32 instruction)
{
	if (state->last_touch[temp] == instruction &&
	    state->physical[temp] != CHROME9_VERTEX_NO_PHYSICAL)
		state->physical_live &= ~BIT(state->physical[temp]);
}

static int chrome9_vertex_emit(struct chrome9_vertex_state *state,
			       struct chrome9_vertex_program *program,
			       const u32 record[4])
{
	if (program->record_count >= DRM_CHROME9_VERTEX_MAX_RECORDS)
		return -E2BIG;
	memcpy(program->records[program->record_count++], record, 4 * sizeof(u32));
	return 0;
}

/*
 * ARL: a0 = floor(clamp(src, low, high)) as MAX and MIN against the bound
 * rows, FLR and MOVA in a free physical register; the vendor ARL also
 * floors before MOVA (PVS_ARL_ICD).  A constant source is copied first to
 * keep one constant row per record.  The RE knob emits the bare MOVA.
 */
static int chrome9_vertex_encode_arl(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction,
	struct chrome9_vertex_program *program)
{
	const u32 mask = state->writemask[instruction];
	const u32 low = (CHROME9_VS_UNIFIED_CONSTANT + state->constant_end) << 14 |
			CHROME9_VS_IDENTITY;
	u32 record[4] = { 0 }, temp, source;
	int ret;

	record[0] = chrome9_vertex_source_word(state, insn, 0, false);
	if (state->relative_raw) {
		record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MOVA, false,
						     CHROME9_VS_DEST_ADDRESS, mask);
		return chrome9_vertex_emit(state, program, record);
	}
	ret = chrome9_vertex_scratch(state, &temp);
	if (ret)
		return ret;
	source = (CHROME9_VS_UNIFIED_TEMP + temp) << 14 | CHROME9_VS_IDENTITY;
	if (insn->src[0].file == DRM_CHROME9_VERTEX_FILE_CONSTANT &&
	    chrome9_vertex_selected(insn, 0)) {
		record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MOV, false,
						     CHROME9_VS_DEST_TEMP + temp, mask);
		ret = chrome9_vertex_emit(state, program, record);
		if (ret)
			return ret;
		record[0] = source;
	}
	record[1] = low;
	record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MAX, false,
					     CHROME9_VS_DEST_TEMP + temp, mask);
	ret = chrome9_vertex_emit(state, program, record);
	if (ret)
		return ret;
	record[0] = source;
	record[1] = low + BIT(14);
	record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MIN, false,
					     CHROME9_VS_DEST_TEMP + temp, mask);
	ret = chrome9_vertex_emit(state, program, record);
	if (ret)
		return ret;
	record[1] = 0;
	record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_FLR, false,
					     CHROME9_VS_DEST_TEMP + temp, mask);
	ret = chrome9_vertex_emit(state, program, record);
	if (ret)
		return ret;
	record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MOVA, false,
					     CHROME9_VS_DEST_ADDRESS, mask);
	return chrome9_vertex_emit(state, program, record);
}

static int chrome9_vertex_encode(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *user, u32 instruction,
	struct chrome9_vertex_program *program)
{
	const struct drm_chrome9_vertex_instruction *insn = user;
	struct drm_chrome9_vertex_instruction moved;
	u32 count = chrome9_vertex_source_count[insn->op];
	bool sat = insn->dst.modifier & DRM_CHROME9_FRAGMENT_DEST_SAT;
	u32 temp = U32_MAX, dest, rank, record[4] = { 0 };
	unsigned int s;
	int ret;

	if (insn->dst.file == DRM_CHROME9_VERTEX_DEST_TEMP)
		temp = insn->dst.index;
	/* A dropped output still ends the lifetime of its sources. */
	if (!state->writemask[instruction]) {
		for (s = 0; s < count; s++)
			if (insn->src[s].file == DRM_CHROME9_VERTEX_FILE_TEMP)
				chrome9_vertex_release(state, insn->src[s].index,
						       instruction);
		return 0;
	}
	else if (insn->dst.file == DRM_CHROME9_VERTEX_DEST_POSITION &&
		 !state->position_direct)
		temp = CHROME9_VERTEX_POSITION_TEMP;
	for (s = 0; s < count; s++) {
		if (insn->src[s].file == DRM_CHROME9_VERTEX_FILE_INPUT &&
		    chrome9_vertex_selected(insn, s))
			state->last_input_record = program->record_count;
		if (insn->src[s].file == DRM_CHROME9_VERTEX_FILE_TEMP &&
		    insn->src[s].index != temp)
			chrome9_vertex_release(state, insn->src[s].index,
					       instruction);
	}
	switch (insn->dst.file) {
	case DRM_CHROME9_VERTEX_DEST_ADDRESS:
		return chrome9_vertex_encode_arl(state, insn, instruction, program);
	case DRM_CHROME9_VERTEX_DEST_POSITION:
		if (state->position_direct) {
			dest = CHROME9_VS_DEST_POSITION;
			break;
		}
		fallthrough;
	case DRM_CHROME9_VERTEX_DEST_TEMP:
		ret = chrome9_vertex_allocate(state, temp);
		if (ret)
			return ret;
		dest = CHROME9_VS_DEST_TEMP + state->physical[temp];
		break;
	case DRM_CHROME9_VERTEX_DEST_PRIMARY:
		dest = CHROME9_VS_DEST_D0;
		sat = true;
		break;
	case DRM_CHROME9_VERTEX_DEST_BACK_PRIMARY:
		/* The vendor compiler omits bit 14 here; clamp like oD0. */
		dest = CHROME9_VS_DEST_BFD0;
		sat = true;
		break;
	case DRM_CHROME9_VERTEX_DEST_CLIP_DISTANCE:
		rank = chrome9_vertex_clip_rank(state, insn->dst.index);
		dest = CHROME9_VS_DEST_T + state->clip_base + rank / 3;
		/* Lane-wise ops compute the written lane from its own selectors;
		 * dot products broadcast. */
		if (rank % 3 && insn->op != DRM_CHROME9_VERTEX_OP_DP3 &&
		    insn->op != DRM_CHROME9_VERTEX_OP_DP4 &&
		    insn->op != DRM_CHROME9_VERTEX_OP_DPH) {
			moved = *user;
			for (s = 0; s < count; s++)
				moved.src[s].swizzle[rank % 3] =
					user->src[s].swizzle[0];
			insn = &moved;
		}
		break;
	case DRM_CHROME9_VERTEX_DEST_POINT_SIZE:
		dest = CHROME9_VS_DEST_POINT_SIZE;
		/* Broadcast lane x so every written lane holds the size. */
		if (insn->op != DRM_CHROME9_VERTEX_OP_DP3 &&
		    insn->op != DRM_CHROME9_VERTEX_OP_DP4 &&
		    insn->op != DRM_CHROME9_VERTEX_OP_DPH) {
			moved = *user;
			for (s = 0; s < count; s++)
				memset(&moved.src[s].swizzle[1],
				       user->src[s].swizzle[0], 3);
			insn = &moved;
		}
		break;
	default:
		dest = CHROME9_VS_DEST_T + state->varying_packed[insn->dst.index];
		break;
	}

	record[0] = chrome9_vertex_source_word(state, insn, 0, false);
	switch (insn->op) {
	case DRM_CHROME9_VERTEX_OP_ADD:
	case DRM_CHROME9_VERTEX_OP_SUB:
		record[1] = CHROME9_VS_LITERAL_ONE;
		record[2] = chrome9_vertex_source_word(state, insn, 1,
				insn->op == DRM_CHROME9_VERTEX_OP_SUB);
		break;
	case DRM_CHROME9_VERTEX_OP_MUL:
		record[1] = chrome9_vertex_source_word(state, insn, 1, false);
		record[2] = CHROME9_VS_LITERAL_ZERO;
		break;
	case DRM_CHROME9_VERTEX_OP_MAD:
		record[1] = chrome9_vertex_source_word(state, insn, 1, false);
		record[2] = chrome9_vertex_source_word(state, insn, 2, false);
		break;
	case DRM_CHROME9_VERTEX_OP_ABS:
		record[1] = chrome9_vertex_source_word(state, insn, 0, true);
		break;
	default:
		if (count > 1)
			record[1] = chrome9_vertex_source_word(state, insn, 1,
							       false);
		break;
	}
	record[3] = chrome9_vertex_dest_word(chrome9_vertex_native_op[insn->op],
					     sat, dest, state->writemask[instruction]);
	/* Ports allow one relative row, so one a0 lane per record. */
	for (s = 0; s < count; s++)
		if (insn->src[s].relative && chrome9_vertex_selected(insn, s))
			record[3] |= (CHROME9_VS_RELATIVE_A0_X - 1 +
				      insn->src[s].relative) <<
				     CHROME9_VS_RELATIVE_SHIFT;
	if (temp != U32_MAX)
		chrome9_vertex_release(state, temp, instruction);
	return chrome9_vertex_emit(state, program, record);
}

/* Op 28 (REP) and op 29 (IF, ELSE) records; ENDIF/ENDREP patch them. */
static int chrome9_vertex_encode_flow(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction,
	struct chrome9_vertex_program *program)
{
	const struct drm_chrome9_vertex_source *src = &insn->src[0];
	u32 here = program->record_count, record[4] = { 0 }, t;
	struct chrome9_vertex_frame *frame;
	int ret = 0;

	switch (insn->op) {
	case DRM_CHROME9_VERTEX_OP_REP:
	case DRM_CHROME9_VERTEX_OP_IF:
		frame = &state->frames[state->depth++];
		frame->record = here;
		frame->has_else = false;
		if (insn->op == DRM_CHROME9_VERTEX_OP_REP) {
			record[0] = (CHROME9_VS_UNIFIED_INT + src->index) << 14 |
				    CHROME9_VS_INT_SELECT;
			record[3] = CHROME9_VS_NATIVE_LOOP << 17;
		} else {
			record[0] = src->file == DRM_CHROME9_VERTEX_FILE_BOOL ?
				(CHROME9_VS_UNIFIED_BOOL + src->index) << 14 |
				CHROME9_VS_IDENTITY :
				chrome9_vertex_source_word(state, insn, 0, false);
			record[3] = CHROME9_VS_NATIVE_BRANCH << 17;
		}
		record[1] = CHROME9_VS_FLOW_TAKEN | 1 << 10;
		record[2] = CHROME9_VS_FLOW_NOT_TAKEN;
		ret = chrome9_vertex_emit(state, program, record);
		break;
	case DRM_CHROME9_VERTEX_OP_ELSE:
		frame = &state->frames[state->depth - 1];
		program->records[frame->record][2] = CHROME9_VS_FLOW_NOT_TAKEN |
			(here + 1 - frame->record) << 10;
		frame->has_else = true;
		frame->else_record = here;
		record[0] = CHROME9_VS_ELSE_SOURCE;
		record[1] = CHROME9_VS_FLOW_TAKEN;
		record[3] = CHROME9_VS_NATIVE_BRANCH << 17;
		ret = chrome9_vertex_emit(state, program, record);
		break;
	case DRM_CHROME9_VERTEX_OP_ENDIF:
		frame = &state->frames[--state->depth];
		if (frame->has_else)
			program->records[frame->else_record][1] =
				CHROME9_VS_FLOW_TAKEN |
				(here - frame->else_record) << 10;
		else
			program->records[frame->record][2] =
				CHROME9_VS_FLOW_NOT_TAKEN |
				(here - frame->record) << 10;
		break;
	default:
		frame = &state->frames[--state->depth];
		/* Zero-length bodies are not in the vendor corpus. */
		if (here == frame->record + 1)
			return -EINVAL;
		program->records[frame->record][2] = CHROME9_VS_FLOW_NOT_TAKEN |
			(here - frame->record - 1) << 10;
		break;
	}
	if (ret)
		return ret;
	for (t = 0; t < DRM_CHROME9_VERTEX_MAX_TEMPS; t++)
		chrome9_vertex_release(state, t, instruction);
	return 0;
}

/*
 * Compile a validated semantic vertex program into native records: user
 * records, oD0/oD1 literal fills, the position move and the vendor
 * finalizer.  Virtual temps get the lowest free physical register at their
 * first definition; a register whose last read is the defining record may
 * be reused, since every record reads its sources before writing.
 */
int chrome9_vertex_compile(
	const struct drm_chrome9_vertex_instruction *instructions,
	u32 instruction_count, const struct chrome9_fragment_program *fragment,
	u32 input_count, const u8 *input_lanes, u32 constant_first,
	u32 constant_count, bool two_side, u32 clip_mask, bool point_size,
	u32 sprite_mask, const struct chrome9_vertex_flow *flow,
	struct chrome9_vertex_program *program)
{
	static const u32 fill_d0[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0,
		CHROME9_VS_NATIVE_MOV << 17 | BIT(14) | CHROME9_VS_DEST_D0 << 4 | 0xf };
	static const u32 fill_d1[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0,
		CHROME9_VS_NATIVE_MOV << 17 | BIT(14) | CHROME9_VS_DEST_D1 << 4 | 0xf };
	static const u32 fill_bfd1[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0,
		CHROME9_VS_NATIVE_MOV << 17 | BIT(14) | CHROME9_VS_DEST_BFD1 << 4 | 0xf };
	bool back;
	struct chrome9_vertex_state *state;
	u32 i, record[4] = { 0 };
	int ret;

	if (!instruction_count ||
	    instruction_count > DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS ||
	    !input_count || input_count > DRM_CHROME9_VERTEX_MAX_INPUTS ||
	    constant_first > DRM_CHROME9_VERTEX_MAX_CONSTANTS ||
	    constant_count > DRM_CHROME9_VERTEX_MAX_CONSTANTS - constant_first ||
	    clip_mask & ~GENMASK(DRM_CHROME9_VERTEX_MAX_CLIP_DISTANCES - 1, 0) ||
	    sprite_mask & ~GENMASK(DRM_CHROME9_FRAGMENT_MAX_VARYINGS - 1, 0))
		return -EINVAL;
	/* UCP coordinates follow the packed varyings inside A..H. */
	if (fragment->varying_count + DIV_ROUND_UP(hweight32(clip_mask), 3) >
	    DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
		return -E2BIG;
	/* A01[21] replaces every texture coordinate, A2F[9:0] selects none
	 * (rung sprite P3b): all varyings are sprite coordinates, no UCP. */
	if (sprite_mask) {
		u32 all = 0;

		for (i = 0; i < fragment->varying_count; i++)
			all |= BIT(fragment->varying_logical[i]);
		if (sprite_mask != all || clip_mask)
			return -EINVAL;
	}
	back = two_side && fragment->primary_used;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->instructions = instructions;
	state->instruction_count = instruction_count;
	state->input_count = input_count;
	state->input_lanes = input_lanes;
	state->constant_first = constant_first;
	state->constant_end = constant_first + constant_count;
	state->last_input_record = -1;
	state->two_side = two_side;
	state->clip_mask = clip_mask;
	state->clip_base = fragment->varying_count;
	state->point_size = point_size;
	state->sprite_mask = sprite_mask;
	memset(state->physical, CHROME9_VERTEX_NO_PHYSICAL,
	       sizeof(state->physical));
	memset(state->first_def, 0xff, sizeof(state->first_def));
	memset(program, 0, sizeof(*program));
	if (flow && flow->max_depth) {
		state->max_depth = min(flow->max_depth,
				       CHROME9_VERTEX_MAX_FLOW_DEPTH);
		state->dynamic = flow->dynamic;
		state->frames = kcalloc(state->max_depth,
					sizeof(*state->frames), GFP_KERNEL);
		if (!state->frames) {
			ret = -ENOMEM;
			goto out;
		}
	}
	if (flow && flow->relative) {
		state->relative = true;
		state->relative_raw = flow->relative_raw;
	}

	ret = chrome9_vertex_validate(state, fragment);
	if (ret)
		goto out;
	ret = chrome9_vertex_bounds(state, program);
	if (ret)
		goto out;
	state->position_direct = state->position_writes == 1;
	/* The deferred position move is the last reader of the hidden temp. */
	state->last_touch[CHROME9_VERTEX_POSITION_TEMP] = instruction_count;
	for (i = 0; i < instruction_count; i++) {
		ret = chrome9_vertex_flow_op(instructions[i].op) ?
		      chrome9_vertex_encode_flow(state, &instructions[i], i,
						 program) :
		      chrome9_vertex_encode(state, &instructions[i], i, program);
		if (ret)
			goto out;
	}
	ret = -EINVAL;
	if (state->last_input_record < 0)
		goto out;
	if (!fragment->primary_used) {
		ret = chrome9_vertex_emit(state, program, fill_d0);
		if (ret)
			goto out;
	}
	ret = chrome9_vertex_emit(state, program, fill_d1);
	if (ret)
		goto out;
	/* A61[19] selects oBFD0 and oBFD1 together; define the unused one. */
	if (back) {
		ret = chrome9_vertex_emit(state, program, fill_bfd1);
		if (ret)
			goto out;
	}
	/* Sprite-replaced coordinates default to the GL (s, t, 0, 1) form. */
	for (i = 0; i < fragment->varying_count; i++) {
		if (!(sprite_mask & BIT(fragment->varying_logical[i])))
			continue;
		record[0] = CHROME9_VS_LITERAL_W1;
		record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MOV, false,
				CHROME9_VS_DEST_T + i,
				GENMASK(fragment->varying_width[i] - 1, 0));
		ret = chrome9_vertex_emit(state, program, record);
		if (ret)
			goto out;
	}
	if (!state->position_direct) {
		record[0] = (CHROME9_VS_UNIFIED_TEMP +
			     state->physical[CHROME9_VERTEX_POSITION_TEMP]) << 14 |
			    0x14c;
		record[3] = chrome9_vertex_dest_word(CHROME9_VS_NATIVE_MOV, false,
						     CHROME9_VS_DEST_POSITION, 0xf);
		ret = chrome9_vertex_emit(state, program, record);
		if (ret)
			goto out;
	}
	for (i = 0; i < CHROME9_VERTEX_FINALIZER_RECORDS; i++) {
		ret = chrome9_vertex_emit(state, program,
					  chrome9_vertex_finalizer[i]);
		if (ret)
			goto out;
	}
	program->active_count = program->record_count -
				CHROME9_VERTEX_INACTIVE_TAILS;
	/* The finalizer writes r0. */
	program->physical_temps = max(state->physical_high_water, 1U);
	program->input_count = input_count;
	program->a60 = 0x60800000 | (program->active_count - 1);
	program->a61 = program->physical_temps <= 8 ? 0x61800000 : 0x61000000;
	/* Vendor: A61[19] = (object +0x0c != 0), set by any oD2/oD3 write. */
	if (back)
		program->a61 |= BIT(19);
	/* A62[8:0] names the last record that reads an input (corpus rule). */
	program->a62 = 0x62008000 | (input_count - 1) << 20 |
		       state->last_input_record;
	ret = 0;
out:
	kfree(state->frames);
	kfree(state);
	return ret;
}

#define VS_X	DRM_CHROME9_FRAGMENT_SWIZZLE_X
#define VS_Y	DRM_CHROME9_FRAGMENT_SWIZZLE_Y
#define VS_Z	DRM_CHROME9_FRAGMENT_SWIZZLE_Z
#define VS_W	DRM_CHROME9_FRAGMENT_SWIZZLE_W
#define VS_SRC(f, i, x, y, z, w, m) \
	{ DRM_CHROME9_VERTEX_FILE_##f, i, { VS_##x, VS_##y, VS_##z, VS_##w }, m, 0 }
#define VS_R(f, i)	VS_SRC(f, i, X, Y, Z, W, 0)
#define VS_N(f, i)	VS_SRC(f, i, X, Y, Z, W, DRM_CHROME9_FRAGMENT_MOD_NEG)
#define VS_S(f, i, c)	VS_SRC(f, i, c, c, c, c, 0)
#define VS_DST(f, i, mask, m)	{ DRM_CHROME9_VERTEX_DEST_##f, i, mask, m }
#define VS_I(o, d, ...) \
	{ .op = DRM_CHROME9_VERTEX_OP_##o, .dst = d, .src = { __VA_ARGS__ } }
#define VS_POS		VS_DST(POSITION, 0, 0xf, 0)
#define VS_T(i)		VS_DST(TEMP, i, 0xf, 0)

struct chrome9_vertex_case {
	const char *name;
	const struct drm_chrome9_vertex_instruction *insn;
	u32 count;
	u32 inputs;
	u32 constants;
	bool primary;
	u8 varyings;
	u8 width[2];
	/* User records plus the position move; fills/finalizer are implied. */
	const u32 (*expected)[4];
	u32 expected_count;
	u32 a62_low;
	int result;
	/* Lane-wise position: the last expected record follows the fills. */
	bool deferred;
};

#define VS_ONE(n, o, a, b, c, n_src, rec0, rec1, rec2, rec3)		\
	static const struct drm_chrome9_vertex_instruction n##_insn[] = {	\
		VS_I(o, VS_T(0), a, b, c), VS_I(MOV, VS_POS, VS_R(TEMP, 0)) }; \
	static const u32 n##_rec[][4] = { { rec0, rec1, rec2, rec3 },		\
		{ 0x0010014c, 0, 0, 0x00080a0f } }

VS_ONE(add, ADD, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x00206490, 0x0040014c, 0x0004010f);
VS_ONE(sub, SUB, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x00206490, 0x0040014d, 0x0004010f);
VS_ONE(mul, MUL, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014c, 0x00206db4, 0x0004010f);
VS_ONE(mad, MAD, VS_R(INPUT, 0), VS_R(CONSTANT, 0), VS_R(INPUT, 0), 3,
       0x0008014c, 0x0040014c, 0x0008014c, 0x0004010f);
static const struct drm_chrome9_vertex_instruction bad_constants_insn[] = {
	VS_I(MAD, VS_POS, VS_R(INPUT, 0), VS_R(CONSTANT, 0), VS_R(CONSTANT, 1)),
};
static const struct drm_chrome9_vertex_instruction bad_inputs_insn[] = {
	VS_I(ADD, VS_POS, VS_R(INPUT, 0), VS_R(INPUT, 1)),
};
static const struct drm_chrome9_vertex_instruction bad_scalar_constant_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(RCP, VS_DST(VARYING, 0, 3, 0), VS_S(CONSTANT, 0, X)),
};
VS_ONE(dp3, DP3, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x00080154, 0x00400154, 0, 0x0006010f);
VS_ONE(dp4, DP4, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014c, 0, 0x0006010f);
VS_ONE(min, MIN, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014c, 0, 0x0012010f);
VS_ONE(max, MAX, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014c, 0, 0x0010010f);
VS_ONE(slt, SLT, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014c, 0, 0x0016010f);
VS_ONE(sge, SGE, VS_R(INPUT, 0), VS_R(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014c, 0, 0x0014010f);
VS_ONE(rcp, RCP, VS_S(INPUT, 0, W), {}, {}, 1,
       0x00081b6c, 0, 0, 0x0028010f);
VS_ONE(rsq, RSQ, VS_S(INPUT, 0, X), {}, {}, 1,
       0x00080000, 0, 0, 0x002a010f);
VS_ONE(exp2, EXP2, VS_S(INPUT, 0, X), {}, {}, 1,
       0x00080000, 0, 0, 0x0020010f);
VS_ONE(log2, LOG2, VS_S(INPUT, 0, X), {}, {}, 1,
       0x00080000, 0, 0, 0x0024010f);
VS_ONE(frc, FRC, VS_R(INPUT, 0), {}, {}, 1,
       0x0008014c, 0, 0, 0x000e010f);
VS_ONE(abs, ABS, VS_R(INPUT, 0), {}, {}, 1,
       0x0008014c, 0x0008014d, 0, 0x0010010f);
VS_ONE(sgn, SGN, VS_R(INPUT, 0), {}, {}, 1,
       0x0008014c, 0, 0, 0x0018010f);
VS_ONE(neg_src0, MOV, VS_N(INPUT, 0), {}, {}, 1,
       0x0008014d, 0, 0, 0x0008010f);
VS_ONE(neg_src1_mul, MUL, VS_R(INPUT, 0), VS_N(CONSTANT, 0), {}, 2,
       0x0008014c, 0x0040014d, 0x00206db4, 0x0004010f);
VS_ONE(neg_src2_mad, MAD, VS_R(INPUT, 0), VS_R(CONSTANT, 0),
       VS_N(INPUT, 0), 3,
       0x0008014c, 0x0040014c, 0x0008014d, 0x0004010f);
VS_ONE(swz_yzxw, MOV, VS_SRC(INPUT, 0, Y, Z, X, W, 0), {}, {}, 1,
       0x00080a0c, 0, 0, 0x0008010f);
VS_ONE(swz_wzyx, MOV, VS_SRC(INPUT, 0, W, Z, Y, X, 0), {}, {}, 1,
       0x00081a20, 0, 0, 0x0008010f);
VS_ONE(swz_xxyy, MOV, VS_SRC(INPUT, 0, X, X, Y, Y, 0), {}, {}, 1,
       0x00080024, 0, 0, 0x0008010f);

static const struct drm_chrome9_vertex_instruction sat_mad_insn[] = {
	VS_I(MAD, VS_DST(TEMP, 0, 0xf, DRM_CHROME9_FRAGMENT_DEST_SAT),
	     VS_R(INPUT, 0), VS_R(CONSTANT, 0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const u32 sat_mad_rec[][4] = {
	{ 0x0008014c, 0x0040014c, 0x0008014c, 0x0004410f },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};

static const struct drm_chrome9_vertex_instruction mov_opos_v0_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
};
static const u32 mov_opos_v0_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
};

/* out_d0_d1 without the v2 specular source: oD1 gets the literal fill. */
static const struct drm_chrome9_vertex_instruction out_d0_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(PRIMARY, 0, 0xf, 0), VS_R(INPUT, 1)),
};
static const u32 out_d0_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ 0x0008414c, 0, 0, 0x0008422f },
};

/* m4x4 as four DP4 lanes (dp4_r0x_v0_c0 form) without op 26. */
static const struct drm_chrome9_vertex_instruction m4x4_insn[] = {
	VS_I(DP4, VS_DST(TEMP, 0, 1, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 0)),
	VS_I(DP4, VS_DST(TEMP, 0, 2, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 1)),
	VS_I(DP4, VS_DST(TEMP, 0, 4, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 2)),
	VS_I(DP4, VS_DST(TEMP, 0, 8, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 3)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const u32 m4x4_rec[][4] = {
	{ 0x0008014c, 0x0040014c, 0, 0x00060101 },
	{ 0x0008014c, 0x0040414c, 0, 0x00060102 },
	{ 0x0008014c, 0x0040814c, 0, 0x00060104 },
	{ 0x0008014c, 0x0040c14c, 0, 0x00060108 },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};

/* mvp_d0: lane-wise position writes go through a temp and one move. */
static const struct drm_chrome9_vertex_instruction mvp_d0_insn[] = {
	VS_I(DP4, VS_DST(POSITION, 0, 1, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 0)),
	VS_I(DP4, VS_DST(POSITION, 0, 2, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 1)),
	VS_I(DP4, VS_DST(POSITION, 0, 4, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 2)),
	VS_I(DP4, VS_DST(POSITION, 0, 8, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 3)),
	VS_I(MOV, VS_DST(PRIMARY, 0, 0xf, 0), VS_R(INPUT, 1)),
};
static const u32 mvp_d0_rec[][4] = {
	{ 0x0008014c, 0x0040014c, 0, 0x00060101 },
	{ 0x0008014c, 0x0040414c, 0, 0x00060102 },
	{ 0x0008014c, 0x0040814c, 0, 0x00060104 },
	{ 0x0008014c, 0x0040c14c, 0, 0x00060108 },
	{ 0x0008414c, 0, 0, 0x0008422f },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};

/* SSA temp_reuse: the defining record may reuse its last source register. */
static const struct drm_chrome9_vertex_instruction temp_reuse_insn[] = {
	VS_I(MOV, VS_T(5), VS_R(INPUT, 0)),
	VS_I(ADD, VS_T(7), VS_R(TEMP, 5), VS_R(CONSTANT, 0)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 7)),
};
static const u32 temp_reuse_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x0008010f },
	{ 0x0010014c, 0x00206490, 0x0040014c, 0x0004010f },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};

static const struct drm_chrome9_vertex_instruction out_t0_t1_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 3, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(VARYING, 1, 3, 0), VS_R(INPUT, 2)),
};
static const u32 out_t0_t1_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ 0x0008414c, 0, 0, 0x00080283 },
	{ 0x0008814c, 0, 0, 0x00080293 },
};

static const struct drm_chrome9_vertex_instruction out_t0_const_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 0xf, 0), VS_R(CONSTANT, 3)),
};
static const u32 out_t0_const_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ 0x0040c14c, 0, 0, 0x0008028f },
};

static const struct drm_chrome9_vertex_instruction bad_undefined_insn[] = {
	VS_I(MOV, VS_DST(TEMP, 0, 7, 0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_ports_insn[] = {
	VS_I(MAD, VS_T(0), VS_R(CONSTANT, 0), VS_R(CONSTANT, 1),
	     VS_R(CONSTANT, 2)),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_R(INPUT, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_scalar_insn[] = {
	VS_I(RCP, VS_POS, VS_R(INPUT, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_window_insn[] = {
	VS_I(ADD, VS_POS, VS_R(INPUT, 0), VS_R(CONSTANT, 4)),
};
static const struct drm_chrome9_vertex_instruction bad_twice_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(POSITION, 0, 1, 0), VS_R(INPUT, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_no_input_insn[] = {
	VS_I(MOV, VS_POS, VS_R(CONSTANT, 0)),
};
/* Lanes and outputs the fragment program does not read are dropped. */
static const struct drm_chrome9_vertex_instruction narrow_varying_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 0xf, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(VARYING, 1, 3, 0), VS_R(INPUT, 2)),
	VS_I(MOV, VS_DST(VARYING, 5, 0xf, 0), VS_R(INPUT, 1)),
};
#define narrow_varying_rec out_t0_t1_rec
#define drop_primary_insn out_d0_insn
#define drop_primary_rec mov_opos_v0_rec
static const struct drm_chrome9_vertex_instruction bad_unused_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0), VS_R(INPUT, 0)),
};

#define VS_CASE_W(n, in, k, prim, var, w, low, deferred) \
	{ #n, n##_insn, ARRAY_SIZE(n##_insn), in, k, prim, var, { w, w }, \
	  n##_rec, ARRAY_SIZE(n##_rec), low, 0, deferred }
#define VS_CASE(n, in, k, prim, var, low) \
	VS_CASE_W(n, in, k, prim, var, 2, low, false)
#define VS_BAD(n, in, k, prim, var) \
	{ #n, n##_insn, ARRAY_SIZE(n##_insn), in, k, prim, var, { 2, 2 }, \
	  NULL, 0, 0, -EINVAL }

static const struct chrome9_vertex_case chrome9_vertex_cases[] = {
	VS_CASE(add, 1, 1, false, 0, 0),
	VS_CASE(sub, 1, 1, false, 0, 0),
	VS_CASE(mul, 1, 1, false, 0, 0),
	VS_CASE(mad, 1, 1, false, 0, 0),
	VS_CASE(dp3, 1, 1, false, 0, 0),
	VS_CASE(dp4, 1, 1, false, 0, 0),
	VS_CASE(min, 1, 1, false, 0, 0),
	VS_CASE(max, 1, 1, false, 0, 0),
	VS_CASE(slt, 1, 1, false, 0, 0),
	VS_CASE(sge, 1, 1, false, 0, 0),
	VS_CASE(rcp, 1, 0, false, 0, 0),
	VS_CASE(rsq, 1, 0, false, 0, 0),
	VS_CASE(exp2, 1, 0, false, 0, 0),
	VS_CASE(log2, 1, 0, false, 0, 0),
	VS_CASE(frc, 1, 0, false, 0, 0),
	VS_CASE(abs, 1, 0, false, 0, 0),
	VS_CASE(sgn, 1, 0, false, 0, 0),
	VS_CASE(neg_src0, 1, 0, false, 0, 0),
	VS_CASE(neg_src1_mul, 1, 1, false, 0, 0),
	VS_CASE(neg_src2_mad, 1, 1, false, 0, 0),
	VS_CASE(swz_yzxw, 1, 0, false, 0, 0),
	VS_CASE(swz_wzyx, 1, 0, false, 0, 0),
	VS_CASE(swz_xxyy, 1, 0, false, 0, 0),
	VS_CASE(sat_mad, 1, 1, false, 0, 0),
	VS_BAD(bad_constants, 1, 2, false, 0),
	VS_BAD(bad_inputs, 2, 0, false, 0),
	VS_BAD(bad_scalar_constant, 1, 1, false, 1),
	VS_CASE(mov_opos_v0, 1, 0, false, 0, 0),
	VS_CASE(out_d0, 2, 0, true, 0, 1),
	VS_CASE(m4x4, 1, 4, false, 0, 3),
	VS_CASE_W(mvp_d0, 2, 4, true, 0, 2, 4, true),
	VS_CASE(temp_reuse, 1, 1, false, 0, 0),
	VS_CASE(out_t0_t1, 3, 0, false, 2, 2),
	VS_CASE_W(out_t0_const, 1, 4, false, 1, 4, 0, false),
	VS_BAD(bad_undefined, 1, 0, false, 0),
	VS_BAD(bad_ports, 1, 3, false, 0),
	VS_BAD(bad_scalar, 1, 0, false, 0),
	VS_BAD(bad_window, 1, 4, false, 0),
	VS_BAD(bad_twice, 1, 0, false, 0),
	VS_BAD(bad_no_input, 1, 1, false, 0),
	VS_CASE(narrow_varying, 3, 0, false, 2, 2),
	VS_CASE(drop_primary, 2, 0, false, 0, 0),
	VS_BAD(bad_unused, 1, 0, false, 0),
};

static int chrome9_vertex_check(const struct chrome9_vertex_case *c,
				struct chrome9_fragment_program *fragment,
				struct chrome9_vertex_program *program)
{
	static const u32 fill_d0[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008422f };
	static const u32 fill_d1[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008423f };
	u32 i, user, total;
	int ret;

	memset(fragment, 0, sizeof(*fragment));
	fragment->primary_used = c->primary;
	fragment->varying_count = c->varyings;
	for (i = 0; i < c->varyings; i++) {
		fragment->varying_logical[i] = i;
		fragment->varying_width[i] = c->width[i];
	}
	ret = chrome9_vertex_compile(c->insn, c->count, fragment, c->inputs,
				     NULL, 0, c->constants, false, 0, false, 0,
				     NULL, program);
	if (ret != c->result)
		return -EIO;
	if (ret)
		return 0;
	user = c->expected_count - c->deferred;
	total = c->expected_count + !c->primary + 1 +
		CHROME9_VERTEX_FINALIZER_RECORDS;
	if (program->record_count != total)
		return -EIO;
	for (i = 0; i < user; i++)
		if (memcmp(program->records[i], c->expected[i], sizeof(fill_d0)))
			return -EIO;
	if (!c->primary &&
	    memcmp(program->records[i++], fill_d0, sizeof(fill_d0)))
		return -EIO;
	if (memcmp(program->records[i++], fill_d1, sizeof(fill_d1)))
		return -EIO;
	if (user != c->expected_count &&
	    memcmp(program->records[i++], c->expected[user], sizeof(fill_d0)))
		return -EIO;
	if (memcmp(program->records[i], chrome9_vertex_finalizer,
		   sizeof(chrome9_vertex_finalizer)))
		return -EIO;
	if (program->a60 != (0x60800000 | (total - 3)) ||
	    program->a62 != (0x62008000 | (c->inputs - 1) << 20 | c->a62_low))
		return -EIO;
	return 0;
}

/* temps_12_live / temps_16_live corpus chains in SSA form. */
static int chrome9_vertex_live_selftest(u32 live,
					struct chrome9_fragment_program *fragment,
					struct chrome9_vertex_program *program)
{
	struct drm_chrome9_vertex_instruction *insn;
	static const struct drm_chrome9_vertex_source input = VS_R(INPUT, 0);
	u32 count = 2 * live, i, first;
	int ret = -EIO;

	insn = kcalloc(count, sizeof(*insn), GFP_KERNEL);
	if (!insn)
		return -ENOMEM;
	for (i = 0; i < live; i++) {
		insn[i].op = DRM_CHROME9_VERTEX_OP_ADD;
		insn[i].dst = (struct drm_chrome9_vertex_destination)VS_T(i);
		insn[i].src[0] = input;
		insn[i].src[1] = (struct drm_chrome9_vertex_source)VS_R(CONSTANT, i);
	}
	for (i = live; i < count - 1; i++) {
		insn[i].op = DRM_CHROME9_VERTEX_OP_ADD;
		insn[i].dst = (struct drm_chrome9_vertex_destination)VS_T(i);
		insn[i].src[0] = (struct drm_chrome9_vertex_source)
			VS_R(TEMP, i == live ? 0 : i - 1);
		insn[i].src[1] = (struct drm_chrome9_vertex_source)
			VS_R(TEMP, i - live + 1);
	}
	insn[i].op = DRM_CHROME9_VERTEX_OP_MOV;
	insn[i].dst = (struct drm_chrome9_vertex_destination)VS_POS;
	insn[i].src[0] = (struct drm_chrome9_vertex_source)VS_R(TEMP, i - 1);

	memset(fragment, 0, sizeof(*fragment));
	if (chrome9_vertex_compile(insn, count, fragment, 1, NULL, 0, live,
				   false, 0, false, 0, NULL, program)) {
		ret = live > CHROME9_VERTEX_PHYSICAL_TEMPS ? 0 : -EIO;
		goto out;
	}
	if (live > CHROME9_VERTEX_PHYSICAL_TEMPS ||
	    program->physical_temps != live ||
	    program->a62 != (0x62008000 | (live - 1)) ||
	    program->a61 != (live <= 8 ? 0x61800000 : 0x61000000))
		goto out;
	for (i = 0; i < live; i++)
		if (program->records[i][0] != 0x0008014c ||
		    program->records[i][1] != CHROME9_VS_LITERAL_ONE ||
		    program->records[i][2] != (0x0040014c | i << 14) ||
		    program->records[i][3] != (0x0004010f | i << 4))
			goto out;
	for (first = 1; i < count - 1; i++, first++)
		if (program->records[i][0] != 0x0010014c ||
		    program->records[i][2] != (0x0010014c | first << 14) ||
		    program->records[i][3] != 0x0004010f)
			goto out;
	if (program->records[i][0] != 0x0010014c ||
	    program->records[i][3] != 0x00080a0f)
		goto out;
	ret = 0;
out:
	kfree(insn);
	return ret;
}

/* back_primary_from_c0 corpus row plus the oD0 source; bit 14 added. */
static const struct drm_chrome9_vertex_instruction back_primary_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(PRIMARY, 0, 0xf, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(BACK_PRIMARY, 0, 0xf, 0), VS_R(CONSTANT, 0)),
};
static const u32 back_primary_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ 0x0008414c, 0, 0, 0x0008422f },
	{ 0x0040014c, 0, 0, 0x0008424f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008423f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008425f },
};

/* Planes 0, 1, 3 enabled (ranks 0..2 in oT1.xyz), plane 2 dropped; the
 * lane-wise ADD for rank 2 moves its X selectors to Z. */
static const struct drm_chrome9_vertex_instruction clip_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 3, 0), VS_R(INPUT, 1)),
	VS_I(DP4, VS_DST(CLIP_DISTANCE, 0, 1, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 0)),
	VS_I(DP4, VS_DST(CLIP_DISTANCE, 1, 1, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 1)),
	VS_I(DP4, VS_DST(CLIP_DISTANCE, 2, 1, 0), VS_R(INPUT, 0), VS_R(CONSTANT, 3)),
	VS_I(ADD, VS_DST(CLIP_DISTANCE, 3, 1, 0), VS_R(INPUT, 1), VS_R(CONSTANT, 2)),
};
static const u32 clip_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ 0x0008414c, 0, 0, 0x00080283 },
	{ 0x0008014c, 0x0040014c, 0, 0x00060291 },
	{ 0x0008014c, 0x0040414c, 0, 0x00060292 },
	{ 0x0008410c, CHROME9_VS_LITERAL_ONE, 0x0040810c, 0x00040294 },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008422f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008423f },
};

/* Point size from v1.y: lanes broadcast, full mask (vendor 0008027f). */
static const struct drm_chrome9_vertex_instruction point_size_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 3, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(POINT_SIZE, 0, 1, 0), VS_SRC(INPUT, 1, Y, Z, W, X, 0)),
};
static const u32 point_size_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ 0x0008414c, 0, 0, 0x00080283 },
	{ 0x00084924, 0, 0, 0x0008027f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008422f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008423f },
};
/* Sprite coordinate T0: the VS write is dropped and (0, 0, 0, 1)-filled. */
static const u32 sprite_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x00080a0f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008422f },
	{ CHROME9_VS_LITERAL_ZERO, 0, 0, 0x0008423f },
	{ CHROME9_VS_LITERAL_W1, 0, 0, 0x00080283 },
};
/* T0 and T1 (or clip distance 0) written: complete without sprites. */
static const struct drm_chrome9_vertex_instruction sprite_pair_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 3, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(VARYING, 1, 3, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(CLIP_DISTANCE, 0, 1, 0), VS_R(INPUT, 1)),
};
static const struct drm_chrome9_vertex_instruction bad_point_size_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(POINT_SIZE, 0, 3, 0), VS_R(INPUT, 1)),
};

static int chrome9_vertex_expect(const struct chrome9_vertex_program *program,
				 const u32 (*expected)[4], u32 count, u32 a61,
				 u32 a62)
{
	if (program->record_count != count + CHROME9_VERTEX_FINALIZER_RECORDS ||
	    memcmp(program->records, expected, count * sizeof(expected[0])) ||
	    memcmp(program->records[count], chrome9_vertex_finalizer,
		   sizeof(chrome9_vertex_finalizer)) ||
	    program->a61 != a61 || program->a62 != a62)
		return -EIO;
	return 0;
}

static int chrome9_vertex_twoside_clip_selftest(
	struct chrome9_fragment_program *fragment,
	struct chrome9_vertex_program *program)
{
	memset(fragment, 0, sizeof(*fragment));
	fragment->primary_used = true;
	if (chrome9_vertex_compile(back_primary_insn, 3, fragment, 2, NULL, 0, 1,
				   true, 0, false, 0, NULL, program) ||
	    chrome9_vertex_expect(program, back_primary_rec,
				  ARRAY_SIZE(back_primary_rec), 0x61880000,
				  0x62108001))
		return -EIO;
	/* BACK_PRIMARY needs a BFC flag; the flag needs a complete write. */
	if (chrome9_vertex_compile(back_primary_insn, 3, fragment, 2, NULL, 0, 1,
				   false, 0, false, 0, NULL, program) != -EINVAL ||
	    chrome9_vertex_compile(back_primary_insn, 2, fragment, 2, NULL, 0, 1,
				   true, 0, false, 0, NULL, program) != -EINVAL)
		return -EIO;
	/* Without a PRIMARY consumer both colors and A61[19] disappear. */
	fragment->primary_used = false;
	if (chrome9_vertex_compile(back_primary_insn, 3, fragment, 2, NULL, 0, 1,
				   true, 0, false, 0, NULL, program) ||
	    program->a61 != 0x61800000 || program->record_count != 8)
		return -EIO;

	fragment->varying_count = 1;
	fragment->varying_width[0] = 2;
	if (chrome9_vertex_compile(clip_insn, 6, fragment, 2, NULL, 0, 4, false,
				   0xb, false, 0, NULL, program) ||
	    chrome9_vertex_expect(program, clip_rec, ARRAY_SIZE(clip_rec),
				  0x61800000, 0x62108004))
		return -EIO;
	/* Enabled but unwritten plane, invalid plane, UCP budget. */
	if (chrome9_vertex_compile(clip_insn, 6, fragment, 2, NULL, 0, 4, false,
				   0x1b, false, 0, NULL, program) != -EINVAL ||
	    chrome9_vertex_compile(clip_insn, 6, fragment, 2, NULL, 0, 4, false,
				   0x4b, false, 0, NULL, program) != -EINVAL)
		return -EIO;
	fragment->varying_count = 7;
	if (chrome9_vertex_compile(clip_insn, 6, fragment, 2, NULL, 0, 4, false,
				   0x0f, false, 0, NULL, program) != -E2BIG)
		return -EIO;
	return 0;
}

static int chrome9_vertex_point_selftest(
	struct chrome9_fragment_program *fragment,
	struct chrome9_vertex_program *program)
{
	memset(fragment, 0, sizeof(*fragment));
	fragment->varying_count = 1;
	fragment->varying_width[0] = 2;
	if (chrome9_vertex_compile(point_size_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, true, 0, NULL, program) ||
	    chrome9_vertex_expect(program, point_size_rec,
				  ARRAY_SIZE(point_size_rec), 0x61800000,
				  0x62108002))
		return -EIO;
	/* Unused point size is dropped; a used one must be written once. */
	if (chrome9_vertex_compile(point_size_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0, NULL, program) ||
	    program->record_count != 4 + CHROME9_VERTEX_FINALIZER_RECORDS ||
	    chrome9_vertex_compile(point_size_insn, 2, fragment, 2, NULL, 0, 0,
				   false, 0, true, 0, NULL, program) != -EINVAL ||
	    chrome9_vertex_compile(bad_point_size_insn, 2, fragment, 2, NULL,
				   0, 0, false, 0, true, 0, NULL, program) != -EINVAL)
		return -EIO;
	if (chrome9_vertex_compile(point_size_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0x1, NULL, program) ||
	    chrome9_vertex_expect(program, sprite_rec, ARRAY_SIZE(sprite_rec),
				  0x61800000, 0x62108000) ||
	    chrome9_vertex_compile(point_size_insn, 1, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0x1, NULL, program) ||
	    chrome9_vertex_compile(point_size_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0x100, NULL, program) != -EINVAL)
		return -EIO;
	/* A second, ordinary coordinate would be replaced too. */
	fragment->varying_count = 2;
	fragment->varying_width[1] = 2;
	fragment->varying_logical[1] = 1;
	if (chrome9_vertex_compile(sprite_pair_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0, NULL, program) ||
	    chrome9_vertex_compile(sprite_pair_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0x1, NULL, program) != -EINVAL ||
	    chrome9_vertex_compile(sprite_pair_insn, 3, fragment, 2, NULL, 0, 0,
				   false, 0, false, 0x3, NULL, program))
		return -EIO;
	fragment->varying_count = 1;
	if (chrome9_vertex_compile(sprite_pair_insn, 4, fragment, 2, NULL, 0, 0,
				   false, 0x1, false, 0, NULL, program) ||
	    chrome9_vertex_compile(sprite_pair_insn, 4, fragment, 2, NULL, 0, 0,
				   false, 0x1, false, 0x1, NULL, program) != -EINVAL)
		return -EIO;
	return 0;
}

#define VS_F(o, ...) { .op = DRM_CHROME9_VERTEX_OP_##o, .src = { __VA_ARGS__ } }

/* if_b0, if_else_b0 and rep_i0 vendor corpus rows (rep_i0 ends in v0). */
static const struct drm_chrome9_vertex_instruction if_b0_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const u32 if_b0_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x0008010f },
	{ 0x001c014c, 0x00280400, 0x00300800, 0x003a0000 },
	{ 0x0010014c, 0x00206490, 0x0040014c, 0x0004010f },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};
static const struct drm_chrome9_vertex_instruction if_else_b0_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(ELSE),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 1)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const u32 if_else_b0_rec[][4] = {
	{ 0x0008014c, 0, 0, 0x0008010f },
	{ 0x001c014c, 0x00280400, 0x00300c00, 0x003a0000 },
	{ 0x0010014c, 0x00206490, 0x0040014c, 0x0004010f },
	{ 0x0020a490, 0x00280800, 0x00000000, 0x003a0000 },
	{ 0x0010014c, 0x00206490, 0x0040414c, 0x0004010f },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};
static const struct drm_chrome9_vertex_instruction rep_i0_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(CONSTANT, 0)),
	VS_F(REP, VS_R(INT, 0)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 1)),
	VS_F(ENDREP),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_R(INPUT, 0)),
};
static const u32 rep_i0_rec[][4] = {
	{ 0x0040014c, 0, 0, 0x0008010f },
	{ 0x00180154, 0x00280400, 0x00300400, 0x00380000 },
	{ 0x0010014c, 0x00206490, 0x0040414c, 0x0004010f },
	{ 0x0010014c, 0x00206490, 0x0008014c, 0x00040a0f },
};
/* T0 is last read inside the loop; T2 must not take its register. */
static const struct drm_chrome9_vertex_instruction rep_carry_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_T(1), VS_R(CONSTANT, 0)),
	VS_F(REP, VS_R(INT, 0)),
	VS_I(ADD, VS_T(2), VS_R(TEMP, 0), VS_R(CONSTANT, 1)),
	VS_I(ADD, VS_T(1), VS_R(TEMP, 1), VS_R(TEMP, 2)),
	VS_F(ENDREP),
	VS_I(MOV, VS_POS, VS_R(TEMP, 1)),
};
/* RE rung F3 form: IF on r0.x. */
static const struct drm_chrome9_vertex_instruction if_temp_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_T(1), VS_R(CONSTANT, 0)),
	VS_F(IF, VS_S(TEMP, 0, X)),
	VS_I(ADD, VS_T(1), VS_R(TEMP, 1), VS_R(CONSTANT, 1)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 1)),
};
static const struct drm_chrome9_vertex_instruction nested_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(REP, VS_R(INT, 1)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(IF, VS_R(BOOL, 3)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 1)),
	VS_F(ENDIF),
	VS_F(ENDREP),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction both_arms_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(MOV, VS_T(1), VS_R(TEMP, 0)),
	VS_F(ELSE),
	VS_I(ADD, VS_T(1), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 1)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_endif_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_F(ENDIF),
};
static const struct drm_chrome9_vertex_instruction bad_flow_open_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_input_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(CONSTANT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(INPUT, 0)),
	VS_F(ENDIF),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_R(INPUT, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_output_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
	VS_F(ENDIF),
};
static const struct drm_chrome9_vertex_instruction bad_flow_one_arm_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(MOV, VS_T(1), VS_R(TEMP, 0)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 1)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_zero_trip_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(REP, VS_R(INT, 0)),
	VS_I(ADD, VS_T(1), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(ENDREP),
	VS_I(MOV, VS_POS, VS_R(TEMP, 1)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_empty_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(REP, VS_R(INT, 0)),
	VS_F(ENDREP),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_rep_bool_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(REP, VS_R(BOOL, 0)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(ENDREP),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_b16_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 16)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_R(CONSTANT, 0)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_flow_twice_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};

/* User records, oD0/oD1 fills and the finalizer of a flow program. */
static int chrome9_vertex_flow_expect(const struct chrome9_vertex_program *program,
				      const u32 (*expected)[4], u32 count)
{
	if (program->record_count != count + 2 + CHROME9_VERTEX_FINALIZER_RECORDS ||
	    memcmp(program->records, expected, count * sizeof(expected[0])) ||
	    memcmp(program->records[count + 2], chrome9_vertex_finalizer,
		   sizeof(chrome9_vertex_finalizer)))
		return -EIO;
	return 0;
}

#define VS_FLOW(insn, flow, constants) \
	chrome9_vertex_compile(insn, ARRAY_SIZE(insn), fragment, 1, NULL, 0, \
			       constants, false, 0, false, 0, flow, program)

static int chrome9_vertex_flow_selftest(struct chrome9_fragment_program *fragment,
					struct chrome9_vertex_program *program)
{
	static const struct chrome9_vertex_flow depth1 = { 1, false };
	static const struct chrome9_vertex_flow depth2 = { 2, false };
	static const struct chrome9_vertex_flow dynamic = { 1, true };
	static const u32 if_temp_rec[4] = {
		0x00100000, 0x00280400, 0x00300800, 0x003a0000,
	};

	memset(fragment, 0, sizeof(*fragment));
	if (VS_FLOW(if_b0_insn, &depth1, 1) ||
	    chrome9_vertex_flow_expect(program, if_b0_rec,
				       ARRAY_SIZE(if_b0_rec)) ||
	    program->a62 != 0x62008000 ||
	    VS_FLOW(if_else_b0_insn, &depth1, 2) ||
	    chrome9_vertex_flow_expect(program, if_else_b0_rec,
				       ARRAY_SIZE(if_else_b0_rec)) ||
	    VS_FLOW(rep_i0_insn, &depth1, 2) ||
	    chrome9_vertex_flow_expect(program, rep_i0_rec,
				       ARRAY_SIZE(rep_i0_rec)) ||
	    program->a62 != 0x62008003)
		return -EIO;
	/* Loop-carried T0/T1 keep r0/r1 up to ENDREP; T2 gets r2. */
	if (VS_FLOW(rep_carry_insn, &depth1, 2) ||
	    program->physical_temps != 3 ||
	    program->records[2][2] != 0x00300800 ||
	    program->records[3][0] != 0x0010014c ||
	    program->records[3][3] != 0x0004012f)
		return -EIO;
	if (VS_FLOW(if_temp_insn, &depth1, 2) != -EINVAL ||
	    VS_FLOW(if_temp_insn, &dynamic, 2) ||
	    memcmp(program->records[2], if_temp_rec, sizeof(if_temp_rec)))
		return -EIO;
	if (VS_FLOW(nested_insn, &depth1, 2) != -EINVAL ||
	    VS_FLOW(nested_insn, &depth2, 2) ||
	    program->records[1][0] != 0x00184154 ||
	    program->records[1][2] != 0x00300c00 ||
	    program->records[3][0] != 0x001cc14c ||
	    program->records[3][2] != 0x00300800)
		return -EIO;
	if (VS_FLOW(both_arms_insn, &depth1, 1) ||
	    VS_FLOW(if_b0_insn, NULL, 1) != -EINVAL ||
	    VS_FLOW(bad_flow_endif_insn, &depth1, 0) != -EINVAL ||
	    VS_FLOW(bad_flow_open_insn, &depth1, 1) != -EINVAL ||
	    VS_FLOW(bad_flow_input_insn, &depth1, 1) != -EINVAL ||
	    VS_FLOW(bad_flow_output_insn, &depth1, 0) != -EINVAL ||
	    VS_FLOW(bad_flow_one_arm_insn, &depth1, 0) != -EINVAL ||
	    VS_FLOW(bad_flow_zero_trip_insn, &depth1, 1) != -EINVAL ||
	    VS_FLOW(bad_flow_empty_insn, &depth1, 0) != -EINVAL ||
	    VS_FLOW(bad_flow_rep_bool_insn, &depth1, 1) != -EINVAL ||
	    VS_FLOW(bad_flow_b16_insn, &depth1, 1) != -EINVAL ||
	    VS_FLOW(bad_flow_twice_insn, &depth1, 0) != -EINVAL)
		return -EIO;
	return 0;
}

#define VS_REL(i, x, y, z, w, m, a) \
	{ DRM_CHROME9_VERTEX_FILE_CONSTANT, i, { VS_##x, VS_##y, VS_##z, VS_##w }, \
	  m, DRM_CHROME9_VERTEX_RELATIVE_A0_##a }
#define VS_RX(i)	VS_REL(i, X, Y, Z, W, 0, X)
#define VS_A0(mask, m)	VS_DST(ADDRESS, 0, mask, m)

/* ARL lowering (MAX/MIN against c16/c17 of 16 rows, FLR, MOVA) and the
 * vendor mova_c_x relative record. */
static const struct drm_chrome9_vertex_instruction arl_input_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, VS_RX(5)),
};
static const u32 arl_input_rec[][4] = {
	{ 0x00080000, 0x0044014c, 0, 0x00100101 },
	{ 0x0010014c, 0x0044414c, 0, 0x00120101 },
	{ 0x0010014c, 0, 0, 0x000c0101 },
	{ 0x0010014c, 0, 0, 0x000a0011 },
	{ 0x0041414c, 0, 0, 0x04080a0f },
};
/* Vendor mova_xy: a0.y selects nibble 5; a constant ARL source is
 * copied before the clamp. */
static const struct drm_chrome9_vertex_instruction arl_xy_insn[] = {
	VS_I(ARL, VS_A0(3, 0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_T(0), VS_RX(5)),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_REL(6, X, Y, Z, W, 0, Y)),
};
static const u32 arl_xy_rec[][4] = {
	{ 0x0008014c, 0x0044014c, 0, 0x00100103 },
	{ 0x0010014c, 0x0044414c, 0, 0x00120103 },
	{ 0x0010014c, 0, 0, 0x000c0103 },
	{ 0x0010014c, 0, 0, 0x000a0013 },
	{ 0x0041414c, 0, 0, 0x0408010f },
	{ 0x0010014c, 0x00206490, 0x0041814c, 0x05040a0f },
};
static const struct drm_chrome9_vertex_instruction arl_constant_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(CONSTANT, 2, X)),
	VS_I(ADD, VS_T(0), VS_R(INPUT, 1), VS_REL(3, X, Y, Z, W, 0, X)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const u32 arl_constant_rec[][4] = {
	{ 0x00408000, 0, 0, 0x00080101 },
	{ 0x0010014c, 0x0044014c, 0, 0x00100101 },
	{ 0x0010014c, 0x0044414c, 0, 0x00120101 },
	{ 0x0010014c, 0, 0, 0x000c0101 },
	{ 0x0010014c, 0, 0, 0x000a0011 },
	{ 0x0008414c, 0x00206490, 0x0040c14c, 0x0404010f },
	{ 0x0010014c, 0, 0, 0x00080a0f },
};
/* Vendor rel_neg_swz, and one relative row read twice (rel_two_same form). */
static const struct drm_chrome9_vertex_instruction rel_neg_swz_insn[] = {
	VS_I(ARL, VS_A0(8, 0), VS_S(INPUT, 0, W)),
	VS_I(MOV, VS_POS, VS_REL(5, Y, Z, W, X, DRM_CHROME9_FRAGMENT_MOD_NEG, W)),
};
static const struct drm_chrome9_vertex_instruction rel_same_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(ADD, VS_POS, VS_RX(3), VS_RX(3)),
};
/* Lane x bases 5 and 9: [0 - 5, 15 - 9] of 16 rows. */
static const struct drm_chrome9_vertex_instruction rel_bases_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_T(0), VS_RX(5)),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_RX(9)),
};
static const struct drm_chrome9_vertex_instruction rel_bases_rev_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_T(0), VS_RX(9)),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_RX(5)),
};
static const struct drm_chrome9_vertex_instruction rel_empty_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_T(0), VS_RX(0)),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_RX(20)),
};
static const struct drm_chrome9_vertex_instruction arl_only_insn[] = {
	VS_I(ARL, VS_A0(0xf, 0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_rel_temp_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_POS, { DRM_CHROME9_VERTEX_FILE_TEMP, 0,
			    { VS_X, VS_Y, VS_Z, VS_W }, 0, 1 }),
};
static const struct drm_chrome9_vertex_instruction bad_rel_input_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, { DRM_CHROME9_VERTEX_FILE_INPUT, 0,
			    { VS_X, VS_Y, VS_Z, VS_W }, 0, 1 }),
};
static const struct drm_chrome9_vertex_instruction bad_rel_lane_insn[] = {
	VS_I(ARL, VS_A0(0xf, 0), VS_R(INPUT, 0)),
	VS_I(MOV, VS_POS, { DRM_CHROME9_VERTEX_FILE_CONSTANT, 5,
			    { VS_X, VS_Y, VS_Z, VS_W }, 0, 5 }),
};
static const struct drm_chrome9_vertex_instruction bad_rel_before_insn[] = {
	VS_I(ADD, VS_POS, VS_R(INPUT, 0), VS_RX(5)),
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
};
static const struct drm_chrome9_vertex_instruction bad_rel_undefined_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, VS_REL(5, X, Y, Z, W, 0, Y)),
};
/* Vendor rel_mixed_abs and rel_diff_comp compile to one a0.x record. */
static const struct drm_chrome9_vertex_instruction bad_rel_mixed_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(ADD, VS_POS, VS_RX(3), VS_R(CONSTANT, 4)),
};
static const struct drm_chrome9_vertex_instruction bad_rel_two_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(ADD, VS_POS, VS_RX(3), VS_RX(4)),
};
static const struct drm_chrome9_vertex_instruction bad_rel_lanes_insn[] = {
	VS_I(ARL, VS_A0(3, 0), VS_R(INPUT, 0)),
	VS_I(ADD, VS_POS, VS_RX(3), VS_REL(3, X, Y, Z, W, 0, Y)),
};
static const struct drm_chrome9_vertex_instruction bad_rel_scalar_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(RCP, VS_DST(TEMP, 0, 1, 0), VS_REL(0, X, X, X, X, 0, X)),
	VS_I(MUL, VS_POS, VS_R(INPUT, 0), VS_S(TEMP, 0, X)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_sat_insn[] = {
	VS_I(ARL, VS_A0(1, DRM_CHROME9_FRAGMENT_DEST_SAT), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, VS_RX(5)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_index_insn[] = {
	VS_I(ARL, VS_DST(ADDRESS, 1, 1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, VS_RX(5)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_temp_insn[] = {
	VS_I(ARL, VS_T(0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_address_mov_insn[] = {
	VS_I(MOV, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(MOV, VS_POS, VS_RX(5)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_relative_insn[] = {
	VS_I(ARL, VS_A0(1, 0), VS_S(INPUT, 0, X)),
	VS_I(ARL, VS_A0(2, 0), VS_REL(1, X, X, X, X, 0, X)),
	VS_I(MOV, VS_POS, VS_REL(5, X, Y, Z, W, 0, Y)),
};
/* a0 lanes follow the temp rules of flow regions. */
static const struct drm_chrome9_vertex_instruction arl_both_arms_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ARL, VS_A0(1, 0), VS_S(TEMP, 0, X)),
	VS_F(ELSE),
	VS_I(ARL, VS_A0(1, 0), VS_S(CONSTANT, 1, X)),
	VS_F(ENDIF),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_RX(2)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_one_arm_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ARL, VS_A0(1, 0), VS_S(TEMP, 0, X)),
	VS_F(ENDIF),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_RX(2)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_then_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(IF, VS_R(BOOL, 0)),
	VS_I(ARL, VS_A0(1, 0), VS_S(TEMP, 0, X)),
	VS_F(ELSE),
	VS_I(ADD, VS_T(1), VS_R(TEMP, 0), VS_RX(2)),
	VS_F(ENDIF),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction arl_rep_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(REP, VS_R(INT, 0)),
	VS_I(ARL, VS_A0(1, 0), VS_S(TEMP, 0, X)),
	VS_I(ADD, VS_T(0), VS_R(TEMP, 0), VS_RX(2)),
	VS_F(ENDREP),
	VS_I(MOV, VS_POS, VS_R(TEMP, 0)),
};
static const struct drm_chrome9_vertex_instruction bad_arl_after_rep_insn[] = {
	VS_I(MOV, VS_T(0), VS_R(INPUT, 0)),
	VS_F(REP, VS_R(INT, 0)),
	VS_I(ARL, VS_A0(1, 0), VS_S(TEMP, 0, X)),
	VS_F(ENDREP),
	VS_I(ADD, VS_POS, VS_R(TEMP, 0), VS_RX(2)),
};

#define VS_ARL(insn, first, count, flow, inputs) \
	chrome9_vertex_compile(insn, ARRAY_SIZE(insn), fragment, inputs, NULL, \
			       first, count, false, 0, false, 0, flow, program)

static int chrome9_vertex_bounds_expect(const struct chrome9_vertex_program *program,
					const u32 low[4], const u32 high[4])
{
	if (!program->bound_rows ||
	    memcmp(program->bounds[0], low, sizeof(program->bounds[0])) ||
	    memcmp(program->bounds[1], high, sizeof(program->bounds[1])))
		return -EIO;
	return 0;
}

static int chrome9_vertex_relative_selftest(struct chrome9_fragment_program *fragment,
					    struct chrome9_vertex_program *program)
{
	static const struct chrome9_vertex_flow relative = { .relative = true };
	static const struct chrome9_vertex_flow raw = {
		.relative = true, .relative_raw = true,
	};
	static const struct chrome9_vertex_flow flow = {
		.max_depth = 1, .relative = true,
	};
	/* -5, 10 | -5, 10, -6, 9 | -3, 12 | -5, 6 | first 4: -1, 6 */
	static const u32 input_low[4] = { 0xc0a00000 };
	static const u32 input_high[4] = { 0x41200000 };
	static const u32 xy_low[4] = { 0xc0a00000, 0xc0c00000 };
	static const u32 xy_high[4] = { 0x41200000, 0x41100000 };
	static const u32 constant_low[4] = { 0xc0400000 };
	static const u32 constant_high[4] = { 0x41400000 };
	static const u32 bases_low[4] = { 0xc0a00000 };
	static const u32 bases_high[4] = { 0x40c00000 };
	static const u32 first4_low[4] = { 0xbf800000 };
	static const u32 unread[4] = { 0 };
	static const u32 neg_swz[4] = { 0x00414a61, 0, 0, 0x07080a0f };
	static const u32 same[4] = { 0x0040c14c, 0x00206490, 0x0040c14c, 0x04040a0f };
	static const u8 mova_masks[] = { 1, 3, 4, 8, 0xf };
	struct drm_chrome9_vertex_instruction raw_insn[2] = {
		VS_I(ARL, VS_A0(1, 0), VS_R(INPUT, 0)),
		VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	};
	unsigned int i;

	memset(fragment, 0, sizeof(*fragment));
	if (VS_ARL(arl_input_insn, 0, 16, &relative, 1) ||
	    chrome9_vertex_flow_expect(program, arl_input_rec,
				       ARRAY_SIZE(arl_input_rec)) ||
	    chrome9_vertex_bounds_expect(program, input_low, input_high) ||
	    program->physical_temps != 1 || program->a62 != 0x62008000)
		return -EIO;
	if (VS_ARL(arl_xy_insn, 0, 16, &relative, 1) ||
	    chrome9_vertex_flow_expect(program, arl_xy_rec,
				       ARRAY_SIZE(arl_xy_rec)) ||
	    chrome9_vertex_bounds_expect(program, xy_low, xy_high))
		return -EIO;
	if (VS_ARL(arl_constant_insn, 0, 16, &relative, 2) ||
	    chrome9_vertex_flow_expect(program, arl_constant_rec,
				       ARRAY_SIZE(arl_constant_rec)) ||
	    chrome9_vertex_bounds_expect(program, constant_low, constant_high) ||
	    program->a62 != 0x62108005)
		return -EIO;
	if (VS_ARL(rel_neg_swz_insn, 0, 16, &relative, 1) ||
	    memcmp(program->records[4], neg_swz, sizeof(neg_swz)) ||
	    program->records[3][3] != 0x000a0018 ||
	    VS_ARL(rel_same_insn, 0, 16, &relative, 1) ||
	    memcmp(program->records[4], same, sizeof(same)))
		return -EIO;
	if (VS_ARL(rel_bases_insn, 0, 16, &relative, 1) ||
	    chrome9_vertex_bounds_expect(program, bases_low, bases_high) ||
	    VS_ARL(rel_bases_rev_insn, 0, 16, &relative, 1) ||
	    chrome9_vertex_bounds_expect(program, bases_low, bases_high) ||
	    VS_ARL(rel_bases_insn, 4, 12, &relative, 1) ||
	    chrome9_vertex_bounds_expect(program, first4_low, bases_high) ||
	    VS_ARL(arl_only_insn, 0, 0, &relative, 1) ||
	    chrome9_vertex_bounds_expect(program, unread, unread))
		return -EIO;
	/* Without ARL no kernel rows; RE knob: bare MOVA per lane mask. */
	if (VS_ARL(mov_opos_v0_insn, 0, 0, &relative, 1) || program->bound_rows)
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(mova_masks); i++) {
		raw_insn[0].dst.writemask = mova_masks[i];
		if (VS_ARL(raw_insn, 0, 0, &raw, 1) ||
		    program->record_count != 4 + CHROME9_VERTEX_FINALIZER_RECORDS ||
		    program->records[0][0] != 0x0008014c ||
		    program->records[0][3] != (0x000a0010U | mova_masks[i]))
			return -EIO;
	}
	if (VS_ARL(arl_input_insn, 0, 16, NULL, 1) != -EINVAL ||
	    VS_ARL(arl_input_insn, 0, 254, &relative, 1) ||
	    VS_ARL(arl_input_insn, 0, 255, &relative, 1) != -E2BIG ||
	    VS_ARL(arl_input_insn, 250, 5, &relative, 1) != -E2BIG ||
	    VS_ARL(rel_empty_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(rel_empty_insn, 0, 32, &relative, 1))
		return -EIO;
	if (VS_ARL(bad_rel_temp_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_input_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_lane_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_before_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_undefined_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_mixed_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_two_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_lanes_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_rel_scalar_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_arl_sat_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_arl_index_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_arl_temp_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_address_mov_insn, 0, 16, &relative, 1) != -EINVAL ||
	    VS_ARL(bad_arl_relative_insn, 0, 16, &relative, 1) != -EINVAL)
		return -EIO;
	if (VS_ARL(arl_both_arms_insn, 0, 16, &flow, 1) ||
	    VS_ARL(bad_arl_one_arm_insn, 0, 16, &flow, 1) != -EINVAL ||
	    VS_ARL(bad_arl_then_insn, 0, 16, &flow, 1) != -EINVAL ||
	    VS_ARL(arl_rep_insn, 0, 16, &flow, 1) ||
	    VS_ARL(bad_arl_after_rep_insn, 0, 16, &flow, 1) != -EINVAL)
		return -EIO;
	return 0;
}

int chrome9_vertex_selftest(void)
{
	static const u32 live[] = { 12, 16, 17 };
	struct chrome9_fragment_program *fragment;
	struct chrome9_vertex_program *program;
	unsigned int i;
	int ret = 0;

	fragment = kzalloc(sizeof(*fragment), GFP_KERNEL);
	program = kzalloc(CHROME9_VERTEX_PROGRAM_BYTES(
		DRM_CHROME9_VERTEX_MAX_RECORDS), GFP_KERNEL);
	if (!fragment || !program) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(chrome9_vertex_cases); i++) {
		ret = chrome9_vertex_check(&chrome9_vertex_cases[i], fragment,
					   program);
		if (ret) {
			pr_err("chrome9: vertex self-test %s failed\n",
			       chrome9_vertex_cases[i].name);
			goto out;
		}
	}
	/* A FLOAT2 element must not expose its unfetched Z/W lanes. */
	memset(fragment, 0, sizeof(*fragment));
	if (chrome9_vertex_compile(mov_opos_v0_insn, 1, fragment, 1,
				   (const u8[]){ 0x3 }, 0, 0, false, 0, false, 0,
				   NULL, program) != -EINVAL ||
	    chrome9_vertex_compile(mov_opos_v0_insn, 1, fragment, 1,
				   (const u8[]){ 0xf }, 0, 0, false, 0, false, 0,
				   NULL, program)) {
		pr_err("chrome9: vertex self-test input lanes failed\n");
		ret = -EIO;
		goto out;
	}
	ret = chrome9_vertex_twoside_clip_selftest(fragment, program);
	if (ret) {
		pr_err("chrome9: vertex self-test two-side/clip failed\n");
		goto out;
	}
	ret = chrome9_vertex_point_selftest(fragment, program);
	if (ret) {
		pr_err("chrome9: vertex self-test point size/sprite failed\n");
		goto out;
	}
	ret = chrome9_vertex_flow_selftest(fragment, program);
	if (ret) {
		pr_err("chrome9: vertex self-test static flow failed\n");
		goto out;
	}
	ret = chrome9_vertex_relative_selftest(fragment, program);
	if (ret) {
		pr_err("chrome9: vertex self-test relative addressing failed\n");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(live); i++) {
		ret = chrome9_vertex_live_selftest(live[i], fragment, program);
		if (ret) {
			pr_err("chrome9: vertex self-test temps_%u_live failed\n",
			       live[i]);
			goto out;
		}
	}
out:
	kfree(program);
	kfree(fragment);
	return ret;
}
