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

#define CHROME9_VS_UNIFIED_INPUT	32U
#define CHROME9_VS_UNIFIED_TEMP		64U
#define CHROME9_VS_UNIFIED_LITERAL	129U
#define CHROME9_VS_UNIFIED_CONSTANT	256U
#define CHROME9_VS_DEST_TEMP		0x10U
#define CHROME9_VS_DEST_D0		0x22U
#define CHROME9_VS_DEST_D1		0x23U
#define CHROME9_VS_DEST_T		0x28U
#define CHROME9_VS_DEST_POSITION	0xa0U
#define CHROME9_VS_LITERAL_ONE		0x00206490U
#define CHROME9_VS_LITERAL_ZERO		0x00206db4U

enum {
	CHROME9_VS_NATIVE_MAD = 2,
	CHROME9_VS_NATIVE_DP = 3,
	CHROME9_VS_NATIVE_MOV = 4,
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
};

/* RCP r0.w = 1/w, op14 r0.xyz, viewport MAD into output 33, two tails. */
static const u32 chrome9_vertex_finalizer[CHROME9_VERTEX_FINALIZER_RECORDS][4] = {
	{ 0x00201b6c, 0x00000000, 0x00000000, 0x00280108 },
	{ 0x00200150, 0x00101b6c, 0x00201b6c, 0x001c0107 },
	{ 0x0010014c, 0x00380150, 0x00384154, 0x0004021f },
	{ 0x0020014c, 0x00000000, 0x00000000, 0x0008020f },
	{ 0x0020014c, 0x00000000, 0x00000000, 0x0008020f },
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
	u8 physical[DRM_CHROME9_VERTEX_MAX_TEMPS + 1];
	u16 physical_live;
	u32 physical_high_water;
	u8 position_mask;
	u8 primary_mask;
	u8 varying_mask[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 varying_packed[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u32 position_writes;
	bool position_direct;
	int last_input_record;
};

static bool chrome9_vertex_scalar_op(u8 op)
{
	return op >= DRM_CHROME9_VERTEX_OP_RCP;
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

static int chrome9_vertex_validate_source(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, unsigned int source,
	u32 instruction)
{
	const struct drm_chrome9_vertex_source *src = &insn->src[source];
	u8 selected;
	unsigned int lane;

	if (src->file > DRM_CHROME9_VERTEX_FILE_CONSTANT ||
	    src->modifier > DRM_CHROME9_FRAGMENT_MOD_NEG || src->reserved)
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
 * record.  Distinct temps are fine.
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

static int chrome9_vertex_validate_dest(struct chrome9_vertex_state *state,
	const struct chrome9_fragment_program *fragment,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction)
{
	const struct drm_chrome9_vertex_destination *dst = &insn->dst;
	u8 mask = dst->writemask;
	u8 *defined;
	int packed;

	if (!mask || mask > 0xf || dst->modifier & ~DRM_CHROME9_FRAGMENT_DEST_SAT)
		return -EINVAL;
	switch (dst->file) {
	case DRM_CHROME9_VERTEX_DEST_TEMP:
		if (dst->index >= DRM_CHROME9_VERTEX_MAX_TEMPS)
			return -EINVAL;
		defined = &state->defined[dst->index];
		state->last_touch[dst->index] = instruction;
		break;
	case DRM_CHROME9_VERTEX_DEST_POSITION:
		if (dst->index)
			return -EINVAL;
		defined = &state->position_mask;
		state->position_writes++;
		break;
	case DRM_CHROME9_VERTEX_DEST_PRIMARY:
		if (dst->index || !fragment->primary_used)
			return -EINVAL;
		defined = &state->primary_mask;
		break;
	case DRM_CHROME9_VERTEX_DEST_VARYING:
		if (dst->index >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			return -EINVAL;
		packed = chrome9_vertex_packed_varying(fragment, dst->index);
		if (packed < 0 ||
		    (mask & ~GENMASK(fragment->varying_width[packed] - 1, 0)))
			return -EINVAL;
		state->varying_packed[dst->index] = packed;
		defined = &state->varying_mask[packed];
		break;
	default:
		return -EINVAL;
	}
	if (*defined & mask)
		return -EINVAL;
	*defined |= mask;
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
		count = chrome9_vertex_source_count[insn->op];
		for (s = 0; s < ARRAY_SIZE(insn->src); s++) {
			if (s >= count) {
				if (memchr_inv(&insn->src[s], 0, sizeof(insn->src[s])))
					return -EINVAL;
				continue;
			}
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
	if (state->position_mask != 0xf ||
	    (fragment->primary_used && state->primary_mask != 0xf))
		return -EINVAL;
	for (i = 0; i < fragment->varying_count; i++)
		if (state->varying_mask[i] !=
		    GENMASK(fragment->varying_width[i] - 1, 0))
			return -EINVAL;
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

static int chrome9_vertex_allocate(struct chrome9_vertex_state *state, u32 temp)
{
	unsigned int physical;

	if (state->physical[temp] != CHROME9_VERTEX_NO_PHYSICAL)
		return 0;
	for (physical = 0; physical < CHROME9_VERTEX_PHYSICAL_TEMPS; physical++)
		if (!(state->physical_live & BIT(physical)))
			break;
	if (physical == CHROME9_VERTEX_PHYSICAL_TEMPS)
		return -E2BIG;
	state->physical[temp] = physical;
	state->physical_live |= BIT(physical);
	state->physical_high_water = max(state->physical_high_water, physical + 1);
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

static int chrome9_vertex_encode(struct chrome9_vertex_state *state,
	const struct drm_chrome9_vertex_instruction *insn, u32 instruction,
	struct chrome9_vertex_program *program)
{
	u32 count = chrome9_vertex_source_count[insn->op];
	bool sat = insn->dst.modifier & DRM_CHROME9_FRAGMENT_DEST_SAT;
	u32 temp = U32_MAX, dest, record[4] = { 0 };
	unsigned int s;
	int ret;

	if (insn->dst.file == DRM_CHROME9_VERTEX_DEST_TEMP)
		temp = insn->dst.index;
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
					     sat, dest, insn->dst.writemask);
	if (temp != U32_MAX)
		chrome9_vertex_release(state, temp, instruction);
	return chrome9_vertex_emit(state, program, record);
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
	u32 constant_count, struct chrome9_vertex_program *program)
{
	static const u32 fill_d0[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0,
		CHROME9_VS_NATIVE_MOV << 17 | BIT(14) | CHROME9_VS_DEST_D0 << 4 | 0xf };
	static const u32 fill_d1[4] = { CHROME9_VS_LITERAL_ZERO, 0, 0,
		CHROME9_VS_NATIVE_MOV << 17 | BIT(14) | CHROME9_VS_DEST_D1 << 4 | 0xf };
	struct chrome9_vertex_state *state;
	u32 i, record[4] = { 0 };
	int ret;

	if (!instruction_count ||
	    instruction_count > DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS ||
	    !input_count || input_count > DRM_CHROME9_VERTEX_MAX_INPUTS ||
	    constant_first > DRM_CHROME9_VERTEX_MAX_CONSTANTS ||
	    constant_count > DRM_CHROME9_VERTEX_MAX_CONSTANTS - constant_first)
		return -EINVAL;
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
	memset(state->physical, CHROME9_VERTEX_NO_PHYSICAL,
	       sizeof(state->physical));
	memset(program, 0, sizeof(*program));

	ret = chrome9_vertex_validate(state, fragment);
	if (ret)
		goto out;
	state->position_direct = state->position_writes == 1;
	/* The deferred position move is the last reader of the hidden temp. */
	state->last_touch[CHROME9_VERTEX_POSITION_TEMP] = instruction_count;
	for (i = 0; i < instruction_count; i++) {
		ret = chrome9_vertex_encode(state, &instructions[i], i, program);
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
	/* A62[8:0] names the last record that reads an input (corpus rule). */
	program->a62 = 0x62008000 | (input_count - 1) << 20 |
		       state->last_input_record;
	ret = 0;
out:
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
static const struct drm_chrome9_vertex_instruction bad_varying_insn[] = {
	VS_I(MOV, VS_POS, VS_R(INPUT, 0)),
	VS_I(MOV, VS_DST(VARYING, 0, 7, 0), VS_R(INPUT, 1)),
	VS_I(MOV, VS_DST(VARYING, 1, 3, 0), VS_R(INPUT, 2)),
};
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
	VS_BAD(bad_varying, 3, 0, false, 2),
	VS_BAD(bad_unused, 1, 0, false, 0),
	/* PRIMARY write while the fragment program does not read it. */
	{ "bad_primary", out_d0_insn, ARRAY_SIZE(out_d0_insn), 2, 0, false, 0,
	  { 2, 2 }, NULL, 0, 0, -EINVAL },
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
				     NULL, 0, c->constants, program);
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
				   program)) {
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

int chrome9_vertex_selftest(void)
{
	static const u32 live[] = { 12, 16, 17 };
	struct chrome9_fragment_program *fragment;
	struct chrome9_vertex_program *program;
	unsigned int i;
	int ret = 0;

	fragment = kzalloc(sizeof(*fragment), GFP_KERNEL);
	program = kzalloc(sizeof(*program), GFP_KERNEL);
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
				   (const u8[]){ 0x3 }, 0, 0, program) != -EINVAL ||
	    chrome9_vertex_compile(mov_opos_v0_insn, 1, fragment, 1,
				   (const u8[]){ 0xf }, 0, 0, program)) {
		pr_err("chrome9: vertex self-test input lanes failed\n");
		ret = -EIO;
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
