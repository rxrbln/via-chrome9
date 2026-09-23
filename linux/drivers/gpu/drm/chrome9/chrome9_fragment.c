// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "chrome9_fragment.h"

#define CHROME9_FRAGMENT_TEMP_COUNT	128U
#define CHROME9_FRAGMENT_PHYS_TEMPS	12U

enum chrome9_fragment_slot {
	CHROME9_FRAGMENT_SLOT_B,
	CHROME9_FRAGMENT_SLOT_C,
	CHROME9_FRAGMENT_SLOT_A,
};

struct chrome9_fragment_state {
	const struct drm_chrome9_fragment_instruction *instructions;
	u16 defined_mask[CHROME9_FRAGMENT_TEMP_COUNT];
	u16 used_mask[CHROME9_FRAGMENT_TEMP_COUNT];
	u16 last_use[CHROME9_FRAGMENT_TEMP_COUNT];
	u8 physical[CHROME9_FRAGMENT_TEMP_COUNT];
	u16 physical_last_use[CHROME9_FRAGMENT_PHYS_TEMPS];
	u8 source_count[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
	u32 constant_mask;
	u32 sampler_mask;
	u32 varying_mask;
	u32 alu_varying_mask;
	u32 coordinate_mask;
	u32 physical_high_water;
	u8 varying_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 coordinate_for_varying[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 coordinate_for_instruction[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
	u8 coordinate_last_tau[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
};

static bool chrome9_fragment_float_finite(u32 value)
{
	return (value & GENMASK(30, 23)) != GENMASK(30, 23);
}

static unsigned int chrome9_fragment_op_sources(u8 op)
{
	switch (op) {
	case DRM_CHROME9_FRAGMENT_OP_MOV:
	case DRM_CHROME9_FRAGMENT_OP_EXP2:
	case DRM_CHROME9_FRAGMENT_OP_LOG2:
	case DRM_CHROME9_FRAGMENT_OP_RCP:
	case DRM_CHROME9_FRAGMENT_OP_RSQ:
	case DRM_CHROME9_FRAGMENT_OP_ABS:
	case DRM_CHROME9_FRAGMENT_OP_FRC:
	case DRM_CHROME9_FRAGMENT_OP_TEX:
		return 1;
	case DRM_CHROME9_FRAGMENT_OP_MUL:
	case DRM_CHROME9_FRAGMENT_OP_ADD:
	case DRM_CHROME9_FRAGMENT_OP_SUB:
	case DRM_CHROME9_FRAGMENT_OP_MIN:
	case DRM_CHROME9_FRAGMENT_OP_MAX:
	case DRM_CHROME9_FRAGMENT_OP_DP3:
	case DRM_CHROME9_FRAGMENT_OP_DP4:
		return 2;
	case DRM_CHROME9_FRAGMENT_OP_MAD:
	case DRM_CHROME9_FRAGMENT_OP_CMP:
		return 3;
	default:
		return 0;
	}
}

static bool chrome9_fragment_source_zero(
	const struct drm_chrome9_fragment_source *source)
{
	static const struct drm_chrome9_fragment_source zero;

	return !memcmp(source, &zero, sizeof(zero));
}

static int chrome9_fragment_validate_source(
	struct chrome9_fragment_state *state,
	const struct drm_chrome9_fragment_source *source, u32 instruction)
{
	u16 selected = 0;
	unsigned int i;

	if (source->file > DRM_CHROME9_FRAGMENT_FILE_CONSTANT ||
	    source->modifier > DRM_CHROME9_FRAGMENT_MOD_NEG ||
	    source->reserved)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(source->swizzle); i++) {
		if (source->swizzle[i] > DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO)
			return -EINVAL;
		if (source->swizzle[i] < DRM_CHROME9_FRAGMENT_SWIZZLE_ONE)
			selected |= BIT(source->swizzle[i]);
	}
	switch (source->file) {
	case DRM_CHROME9_FRAGMENT_FILE_TEMP:
		if (source->index >= CHROME9_FRAGMENT_TEMP_COUNT ||
		    !state->defined_mask[source->index] ||
		    (selected & ~state->defined_mask[source->index]))
			return -EINVAL;
		state->used_mask[source->index] |= selected;
		state->last_use[source->index] = instruction;
		break;
	case DRM_CHROME9_FRAGMENT_FILE_VARYING:
		if (source->index >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			return -EINVAL;
		break;
	case DRM_CHROME9_FRAGMENT_FILE_CONSTANT:
		if (source->index >= DRM_CHROME9_FRAGMENT_MAX_CONSTANTS)
			return -EINVAL;
		state->constant_mask |= BIT(source->index);
		break;
	}
	return 0;
}

static void chrome9_fragment_note_varying(struct chrome9_fragment_state *state,
	const struct drm_chrome9_fragment_source *source, u8 component_mask)
{
	u8 width = 1;
	unsigned int component;

	if (source->file != DRM_CHROME9_FRAGMENT_FILE_VARYING)
		return;
	for (component = 0; component < ARRAY_SIZE(source->swizzle); component++)
		if ((component_mask & BIT(component)) &&
		    source->swizzle[component] < DRM_CHROME9_FRAGMENT_SWIZZLE_ONE)
			width = max(width, source->swizzle[component] + 1);
	state->varying_mask |= BIT(source->index);
	state->varying_width[source->index] =
		max(state->varying_width[source->index], width);
}

static u8 chrome9_fragment_alu_component_mask(
	const struct drm_chrome9_fragment_instruction *instruction)
{
	switch (instruction->op) {
	case DRM_CHROME9_FRAGMENT_OP_DP3:
		return 0x7;
	case DRM_CHROME9_FRAGMENT_OP_DP4:
		return 0xf;
	case DRM_CHROME9_FRAGMENT_OP_EXP2:
	case DRM_CHROME9_FRAGMENT_OP_LOG2:
	case DRM_CHROME9_FRAGMENT_OP_RCP:
	case DRM_CHROME9_FRAGMENT_OP_RSQ:
		return 0x1;
	default:
		return instruction->dst.writemask;
	}
}

static int chrome9_fragment_validate(struct chrome9_fragment_state *state,
	u32 instruction_count, u32 constant_count, u32 texture_count,
	u32 varying_count)
{
	u8 output_mask = 0;
	u32 alu_count = 0;
	u32 tau_count = 0;
	u32 bridge_count = 0;
	unsigned int i;

	if (!instruction_count ||
	    instruction_count > DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS ||
	    constant_count > DRM_CHROME9_FRAGMENT_MAX_CONSTANTS ||
	    texture_count > DRM_CHROME9_FRAGMENT_MAX_TEXTURES ||
	    varying_count > DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
		return -EINVAL;
	memset(state->physical, 0xff, sizeof(state->physical));
	memset(state->last_use, 0xff, sizeof(state->last_use));
	memset(state->coordinate_for_varying, 0xff,
	       sizeof(state->coordinate_for_varying));
	memset(state->coordinate_for_instruction, 0xff,
	       sizeof(state->coordinate_for_instruction));
	memset(state->coordinate_last_tau, 0xff,
	       sizeof(state->coordinate_last_tau));
	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		unsigned int source_count =
			chrome9_fragment_op_sources(instruction->op);
		unsigned int source;

		if (!source_count || instruction->reserved ||
		    instruction->dst.modifier & ~DRM_CHROME9_FRAGMENT_DEST_SAT ||
		    !instruction->dst.writemask ||
		    instruction->dst.writemask & ~0xf)
			return -EINVAL;
		if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX) {
			if (++tau_count > DRM_CHROME9_FRAGMENT_MAX_TAU ||
			    instruction->sampler >= texture_count ||
			    instruction->dst.file != DRM_CHROME9_FRAGMENT_DEST_TEMP ||
			    instruction->dst.modifier ||
			    instruction->dst.writemask != 0xf)
				return -EINVAL;
			state->sampler_mask |= BIT(instruction->sampler);
		} else if (++alu_count > DRM_CHROME9_FRAGMENT_MAX_ALU ||
			   instruction->sampler) {
			return -EINVAL;
		}
		if (instruction->dst.file == DRM_CHROME9_FRAGMENT_DEST_TEMP) {
			if (instruction->dst.index >= CHROME9_FRAGMENT_TEMP_COUNT ||
			    state->defined_mask[instruction->dst.index] &
				instruction->dst.writemask)
				return -EINVAL;
		} else if (instruction->dst.file ==
			   DRM_CHROME9_FRAGMENT_DEST_COLOR) {
			if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX ||
			    instruction->dst.index ||
			    output_mask & instruction->dst.writemask)
				return -EINVAL;
			output_mask |= instruction->dst.writemask;
		} else {
			return -EINVAL;
		}
		state->source_count[i] = source_count;
		for (source = 0; source < source_count; source++) {
			int ret = chrome9_fragment_validate_source(state,
				&instruction->src[source], i);

			if (ret)
				return ret;
		}
		for (; source < ARRAY_SIZE(instruction->src); source++)
			if (!chrome9_fragment_source_zero(&instruction->src[source]))
				return -EINVAL;
		if (instruction->op != DRM_CHROME9_FRAGMENT_OP_TEX)
			for (source = 0; source < source_count; source++)
				chrome9_fragment_note_varying(state,
					&instruction->src[source],
					chrome9_fragment_alu_component_mask(instruction));
		if (instruction->dst.file == DRM_CHROME9_FRAGMENT_DEST_TEMP)
			state->defined_mask[instruction->dst.index] |=
				instruction->dst.writemask;
		if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX) {
			const struct drm_chrome9_fragment_source *coordinate =
				&instruction->src[0];

			if ((coordinate->file != DRM_CHROME9_FRAGMENT_FILE_VARYING &&
			     coordinate->file != DRM_CHROME9_FRAGMENT_FILE_TEMP) ||
			    coordinate->modifier)
				return -EINVAL;
			if (coordinate->file == DRM_CHROME9_FRAGMENT_FILE_VARYING) {
				for (source = 0;
				     source < ARRAY_SIZE(coordinate->swizzle); source++)
					if (coordinate->swizzle[source] >
					    DRM_CHROME9_FRAGMENT_SWIZZLE_W)
						return -EINVAL;
				chrome9_fragment_note_varying(state, coordinate, 0x3);
			} else {
				bridge_count++;
			}
		}
	}
	if (output_mask != 0xf || !alu_count ||
	    alu_count + bridge_count > DRM_CHROME9_FRAGMENT_MAX_ALU)
		return -EINVAL;
	for (i = 0; i < CHROME9_FRAGMENT_TEMP_COUNT; i++)
		if (state->defined_mask[i] & ~state->used_mask[i])
			return -EINVAL;
	if (constant_count != (state->constant_mask ?
		fls(state->constant_mask) : 0) ||
	    texture_count != (state->sampler_mask ? fls(state->sampler_mask) : 0))
		return -EINVAL;
	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		unsigned int source;

		for (source = 0; source < state->source_count[i]; source++)
			if (instruction->src[source].file ==
			    DRM_CHROME9_FRAGMENT_FILE_VARYING &&
			    instruction->src[source].index >= varying_count)
				return -EINVAL;
	}
	return 0;
}

static int chrome9_fragment_allocate(struct chrome9_fragment_state *state,
	u32 instruction_count)
{
	u16 free_mask = GENMASK(CHROME9_FRAGMENT_PHYS_TEMPS - 1, 0);
	u8 coordinate_count = 0;
	u8 computed_coordinate;
	unsigned int i;

	/*
	 * The production allocator compacts every direct varying used by either
	 * pipeline by logical rank.  Computed TEMP coordinates are allocated from
	 * the remaining coordinate namespace below.
	 */
	for (i = 0; i < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; i++) {
		if (state->varying_mask & BIT(i))
			state->coordinate_for_varying[i] = coordinate_count++;
	}
	computed_coordinate = coordinate_count;
	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		const struct drm_chrome9_fragment_source *coordinate;

		if (instruction->op != DRM_CHROME9_FRAGMENT_OP_TEX)
			continue;
		coordinate = &instruction->src[0];
		if (coordinate->file == DRM_CHROME9_FRAGMENT_FILE_VARYING) {
			state->coordinate_for_instruction[i] =
				state->coordinate_for_varying[coordinate->index];
			continue;
		}
		if (coordinate_count == DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			return -E2BIG;
		state->coordinate_for_instruction[i] = computed_coordinate++;
		if (computed_coordinate == DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			computed_coordinate = coordinate_count;
	}

	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		unsigned int source;

		if (instruction->dst.file == DRM_CHROME9_FRAGMENT_DEST_TEMP &&
		    state->physical[instruction->dst.index] == 0xff) {
			unsigned int physical;

			if (!free_mask)
				return -E2BIG;
			physical = __ffs(free_mask);
			free_mask &= ~BIT(physical);
			state->physical[instruction->dst.index] = physical;
			state->physical_high_water = max(state->physical_high_water,
							physical + 1);
		}
		for (source = 0; source < state->source_count[i]; source++) {
			const struct drm_chrome9_fragment_source *src =
				&instruction->src[source];

			if (src->file == DRM_CHROME9_FRAGMENT_FILE_TEMP &&
			    state->last_use[src->index] == i)
				free_mask |= BIT(state->physical[src->index]);
		}
	}
	memset(state->physical_last_use, 0xff,
	       sizeof(state->physical_last_use));
	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		unsigned int source;

		for (source = 0; source < state->source_count[i]; source++)
			if (instruction->src[source].file ==
			    DRM_CHROME9_FRAGMENT_FILE_TEMP)
				state->physical_last_use[state->physical[
					instruction->src[source].index]] = i;
	}
	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		const struct drm_chrome9_fragment_source *coordinate;
		u8 slot;

		if (instruction->op != DRM_CHROME9_FRAGMENT_OP_TEX)
			continue;
		coordinate = &instruction->src[0];
		slot = state->coordinate_for_instruction[i];
		state->coordinate_last_tau[slot] = i;
	}
	return 0;
}

static u32 chrome9_fragment_native_constant(u8 index)
{
	return index < 16 ? index : index + 16;
}

static u32 chrome9_fragment_source_file(
	const struct drm_chrome9_fragment_source *source)
{
	switch (source->file) {
	case DRM_CHROME9_FRAGMENT_FILE_TEMP:
		return 0;
	case DRM_CHROME9_FRAGMENT_FILE_CONSTANT:
		return 2;
	default:
		return 3;
	}
}

static u32 chrome9_fragment_source_id(const struct chrome9_fragment_state *state,
	const struct drm_chrome9_fragment_source *source)
{
	if (source->file == DRM_CHROME9_FRAGMENT_FILE_TEMP)
		return state->physical[source->index];
	if (source->file == DRM_CHROME9_FRAGMENT_FILE_CONSTANT)
		return chrome9_fragment_native_constant(source->index);
	return state->coordinate_for_varying[source->index];
}

static void chrome9_fragment_encode_source(u32 record[4],
	const struct chrome9_fragment_state *state,
	const struct drm_chrome9_fragment_source *source,
	enum chrome9_fragment_slot slot, bool release)
{
	u32 modifier = source->modifier == DRM_CHROME9_FRAGMENT_MOD_NEG ? 8 : 0;
	u32 file = chrome9_fragment_source_file(source);
	u32 id = chrome9_fragment_source_id(state, source);

	switch (slot) {
	case CHROME9_FRAGMENT_SLOT_A:
		record[1] &= 0x00ffffff;
		record[2] &= ~0x0003ffff;
		record[1] |= modifier << 24 | (u32)source->swizzle[3] << 28 |
			     (u32)(source->swizzle[2] & 1) << 31;
		record[2] |= (u32)source->swizzle[0] << 5 |
			     (u32)source->swizzle[1] << 2 |
			     (u32)source->swizzle[2] >> 1 | id << 8 |
			     file << 14 | (release ? BIT(17) : 0);
		break;
	case CHROME9_FRAGMENT_SLOT_C:
		record[0] &= 0x0fffffff;
		record[1] &= ~0x003fffff;
		record[3] &= ~BIT(20);
		record[0] |= modifier << 28;
		record[1] |= (u32)source->swizzle[0] << 9 |
			     (u32)source->swizzle[1] << 6 |
			     (u32)source->swizzle[2] << 3 |
			     source->swizzle[3] | id << 12 | file << 18 |
			     (release ? BIT(21) : 0);
		if (source->modifier)
			record[3] |= BIT(20);
		break;
	case CHROME9_FRAGMENT_SLOT_B:
		record[0] &= ~0x03ffffff;
		record[0] |= modifier | (u32)source->swizzle[0] << 13 |
			     (u32)source->swizzle[1] << 10 |
			     (u32)source->swizzle[2] << 7 |
			     (u32)source->swizzle[3] << 4 | id << 16 |
			     file << 22 | (release ? BIT(25) : 0);
		break;
	}
}

static void chrome9_fragment_encode_destination(u32 record[4],
	const struct chrome9_fragment_state *state,
	const struct drm_chrome9_fragment_destination *destination)
{
	u32 file = destination->file == DRM_CHROME9_FRAGMENT_DEST_COLOR ? 2 : 0;
	u32 id = destination->file == DRM_CHROME9_FRAGMENT_DEST_COLOR ? 0 :
		 state->physical[destination->index];

	record[2] &= ~GENMASK(23, 20);
	record[3] &= ~(GENMASK(12, 6));
	record[2] |= (u32)destination->writemask << 20;
	record[3] |= id << 6 | file << 10;
	if (destination->modifier & DRM_CHROME9_FRAGMENT_DEST_SAT)
		record[3] |= BIT(12);
}

static int chrome9_fragment_alu_template(u8 op, u32 record[4],
	enum chrome9_fragment_slot slots[3])
{
	static const u32 mov[4] = {
		0x01c00530, 0x301c0053, 0x00f00005, 0x000d0020,
	};
	static const u32 mul[4] = {
		0x0140b6d0, 0x30080053, 0x00f00005, 0x00000020,
	};
	static const u32 add[4] = {
		0x00800530, 0x30140924, 0x00f00005, 0x00000020,
	};
	static const u32 sub[4] = {
		0x80400530, 0x30140924, 0x00f00005, 0x00000020,
	};
	static const u32 mad[4] = {
		0x00800530, 0x30080053, 0x00f00005, 0x00000020,
	};
	static const u32 pair[4] = {
		0x01c00530, 0x30080053, 0x00f00005, 0x00000020,
	};
	static const u32 scalar[4] = {
		0x01c00530, 0x301c0053, 0x00f00005, 0x00000020,
	};
	static const u32 abs[4] = {
		0x00400538, 0x30040053, 0x00f00005, 0x00090020,
	};
	static const u32 cmp[4] = {
		0x00800530, 0x30040053, 0x00f00005, 0x00090020,
	};
	u32 subop = 0;

	switch (op) {
	case DRM_CHROME9_FRAGMENT_OP_MOV:
	case DRM_CHROME9_FRAGMENT_OP_FRC:
		memcpy(record, mov, sizeof(mov));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		subop = op == DRM_CHROME9_FRAGMENT_OP_FRC ? 0xe : 0xd;
		break;
	case DRM_CHROME9_FRAGMENT_OP_MUL:
		memcpy(record, mul, sizeof(mul));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		slots[1] = CHROME9_FRAGMENT_SLOT_C;
		break;
	case DRM_CHROME9_FRAGMENT_OP_ADD:
		memcpy(record, add, sizeof(add));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		slots[1] = CHROME9_FRAGMENT_SLOT_B;
		break;
	case DRM_CHROME9_FRAGMENT_OP_SUB:
		memcpy(record, sub, sizeof(sub));
		slots[0] = CHROME9_FRAGMENT_SLOT_B;
		slots[1] = CHROME9_FRAGMENT_SLOT_A;
		break;
	case DRM_CHROME9_FRAGMENT_OP_MAD:
		memcpy(record, mad, sizeof(mad));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		slots[1] = CHROME9_FRAGMENT_SLOT_C;
		slots[2] = CHROME9_FRAGMENT_SLOT_B;
		break;
	case DRM_CHROME9_FRAGMENT_OP_MIN:
	case DRM_CHROME9_FRAGMENT_OP_MAX:
	case DRM_CHROME9_FRAGMENT_OP_DP3:
	case DRM_CHROME9_FRAGMENT_OP_DP4:
		memcpy(record, pair, sizeof(pair));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		slots[1] = CHROME9_FRAGMENT_SLOT_C;
		subop = op == DRM_CHROME9_FRAGMENT_OP_MIN ? 0xc :
			op == DRM_CHROME9_FRAGMENT_OP_MAX ? 0xb :
			op == DRM_CHROME9_FRAGMENT_OP_DP3 ? 2 : 3;
		break;
	case DRM_CHROME9_FRAGMENT_OP_EXP2:
	case DRM_CHROME9_FRAGMENT_OP_LOG2:
	case DRM_CHROME9_FRAGMENT_OP_RCP:
	case DRM_CHROME9_FRAGMENT_OP_RSQ:
		memcpy(record, scalar, sizeof(scalar));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		subop = op - DRM_CHROME9_FRAGMENT_OP_EXP2 + 4;
		break;
	case DRM_CHROME9_FRAGMENT_OP_ABS:
		memcpy(record, abs, sizeof(abs));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		slots[1] = CHROME9_FRAGMENT_SLOT_C;
		slots[2] = CHROME9_FRAGMENT_SLOT_B;
		break;
	case DRM_CHROME9_FRAGMENT_OP_CMP:
		memcpy(record, cmp, sizeof(cmp));
		slots[0] = CHROME9_FRAGMENT_SLOT_A;
		slots[1] = CHROME9_FRAGMENT_SLOT_C;
		slots[2] = CHROME9_FRAGMENT_SLOT_B;
		break;
	default:
		return -EINVAL;
	}
	record[3] = (record[3] & ~GENMASK(19, 16)) | subop << 16;
	return 0;
}

static bool chrome9_fragment_release_source(
	const struct chrome9_fragment_state *state,
	const struct drm_chrome9_fragment_instruction *instruction,
	u32 instruction_index, unsigned int source_index,
	const enum chrome9_fragment_slot slots[3])
{
	const struct drm_chrome9_fragment_source *source =
		&instruction->src[source_index];
	u32 physical;
	unsigned int other;

	if (source->file != DRM_CHROME9_FRAGMENT_FILE_TEMP)
		return false;
	physical = state->physical[source->index];
	if (state->physical_last_use[physical] != instruction_index)
		return false;
	for (other = 0; other < state->source_count[instruction_index]; other++) {
		const struct drm_chrome9_fragment_source *candidate =
			&instruction->src[other];

		if (candidate->file != DRM_CHROME9_FRAGMENT_FILE_TEMP ||
		    state->physical[candidate->index] != physical)
			continue;
		if (slots[other] > slots[source_index])
			return false;
	}
	return true;
}

static int chrome9_fragment_encode(struct chrome9_fragment_state *state,
	struct chrome9_fragment_program *program, u32 instruction_count)
{
	u32 previous_phase = U32_MAX;
	u32 alu_index = 0;
	u32 tau_index = 0;
	u32 active0 = 0;
	u32 native_tau_varying_mask = 0;
	bool starts_alu = false;
	unsigned int i;

	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];

		if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX) {
			const struct drm_chrome9_fragment_source *coordinate =
				&instruction->src[0];
			bool computed = coordinate->file ==
				DRM_CHROME9_FRAGMENT_FILE_TEMP;
			u32 coordinate_id = state->coordinate_for_instruction[i];
			u32 destination = state->physical[instruction->dst.index];
			u32 native_coordinate = coordinate_id |
				((destination & 3) << 4);
			u32 tau_swizzle = computed ? 0x1b0 :
				(u32)coordinate->swizzle[0] << 10 |
				(u32)coordinate->swizzle[1] << 8 |
				(u32)coordinate->swizzle[2] << 6 |
				(u32)coordinate->swizzle[3] << 4;

			if (computed) {
				enum chrome9_fragment_slot slots[3] = { 0 };
				u32 *bridge;
				int ret;

				if (previous_phase == 1)
					program->controls[3 + (tau_index - 1) / 24] |=
						BIT((tau_index - 1) % 24);
				bridge = program->alu[alu_index];
				ret = chrome9_fragment_alu_template(
					DRM_CHROME9_FRAGMENT_OP_MOV, bridge, slots);
				if (ret)
					return ret;
				chrome9_fragment_encode_source(bridge, state, coordinate,
					CHROME9_FRAGMENT_SLOT_A,
					chrome9_fragment_release_source(state,
						instruction, i, 0, slots));
				bridge[2] = (bridge[2] & ~GENMASK(23, 20)) | 0xf << 20;
				bridge[3] = (bridge[3] & ~GENMASK(12, 6)) |
					BIT(10) | coordinate_id << 6;
				alu_index++;
				previous_phase = 0;
			}

			if (previous_phase == 0)
				program->controls[5 + (alu_index - 1) / 24] |=
					BIT((alu_index - 1) % 24);
			program->tau[tau_index] = 0x20100000 |
				(state->coordinate_last_tau[coordinate_id] == i ?
				 BIT(28) : 0) |
				(u32)instruction->sampler << 24 |
				destination << 16 | native_coordinate << 12 |
				tau_swizzle |
				instruction->sampler;
			state->coordinate_mask |= BIT(coordinate_id);
			active0 = max(active0, coordinate_id + 1);
			if (!computed)
				native_tau_varying_mask |= BIT(coordinate_id);
			tau_index++;
			previous_phase = 1;
			continue;
		}
		if (previous_phase == 1)
			program->controls[3 + (tau_index - 1) / 24] |=
				BIT((tau_index - 1) % 24);
		{
			enum chrome9_fragment_slot slots[3] = { 0 };
			u32 *record = program->alu[alu_index];
			unsigned int source;
			int ret;

			ret = chrome9_fragment_alu_template(instruction->op, record,
							 slots);
			if (ret)
				return ret;
			for (source = 0; source < state->source_count[i]; source++)
				if (slots[source] == CHROME9_FRAGMENT_SLOT_C &&
				    instruction->src[source].modifier)
					return -EINVAL;
			for (source = 0; source < state->source_count[i]; source++) {
				struct drm_chrome9_fragment_source packed =
					instruction->src[source];
				const struct drm_chrome9_fragment_source *src = &packed;

				if (instruction->op >= DRM_CHROME9_FRAGMENT_OP_EXP2 &&
				    instruction->op <= DRM_CHROME9_FRAGMENT_OP_RSQ) {
					packed.swizzle[3] = packed.swizzle[0];
					packed.swizzle[0] = 0;
					packed.swizzle[1] = 0;
					packed.swizzle[2] = 0;
				}
				if (instruction->op == DRM_CHROME9_FRAGMENT_OP_DP3)
					packed.swizzle[3] =
						DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO;

				chrome9_fragment_encode_source(record, state, src,
					slots[source],
					chrome9_fragment_release_source(state,
						instruction, i, source, slots));
				if (src->file == DRM_CHROME9_FRAGMENT_FILE_VARYING) {
					u32 coordinate_id =
						state->coordinate_for_varying[src->index];

					state->alu_varying_mask |= BIT(coordinate_id);
					active0 = max(active0, coordinate_id + 1);
				}
			}
			if (instruction->op == DRM_CHROME9_FRAGMENT_OP_ABS) {
				struct drm_chrome9_fragment_source negative =
					instruction->src[0];

				negative.modifier = DRM_CHROME9_FRAGMENT_MOD_NEG;
				chrome9_fragment_encode_source(record, state,
					&instruction->src[0], CHROME9_FRAGMENT_SLOT_C,
					false);
				chrome9_fragment_encode_source(record, state, &negative,
					CHROME9_FRAGMENT_SLOT_B, false);
			}
			chrome9_fragment_encode_destination(record, state,
							&instruction->dst);
		}
		alu_index++;
		previous_phase = 0;
	}
	program->alu_count = alu_index;
	program->tau_count = tau_index;
	program->controls[0] = 0x90000000 |
		(state->alu_varying_mask & ~native_tau_varying_mask);
	starts_alu = state->instructions[0].op != DRM_CHROME9_FRAGMENT_OP_TEX ||
		state->instructions[0].src[0].file ==
		DRM_CHROME9_FRAGMENT_FILE_TEMP;
	if (starts_alu)
		program->controls[0] |= BIT(18);
	if (active0 <= 4 && state->physical_high_water <= 3)
		program->controls[0] |= 0x18000;
	else if (active0 <= 5 && state->physical_high_water <= 4)
		program->controls[0] |= 0x10000;
	else if (active0 <= 8 && state->physical_high_water <= 6)
		program->controls[0] |= 0x08000;
	program->controls[1] = 0x91000000 | state->coordinate_mask;
	program->controls[2] = 0x92000000 | tau_index << 16 | alu_index;
	for (i = 3; i < ARRAY_SIZE(program->controls); i++)
		program->controls[i] |= (0x90 + i) << 24;
	return 0;
}

int chrome9_fragment_compile(
	const struct drm_chrome9_fragment_instruction *instructions,
	u32 instruction_count, const u32 *constants, u32 constant_count,
	u32 texture_count, u32 varying_count,
	struct chrome9_fragment_program *program)
{
	struct chrome9_fragment_state state = {
		.instructions = instructions,
	};
	unsigned int i;
	int ret;

	memset(program, 0, sizeof(*program));
	ret = chrome9_fragment_validate(&state, instruction_count,
		constant_count, texture_count, varying_count);
	if (ret)
		return ret;
	for (i = 0; i < constant_count * 4; i++)
		if (!chrome9_fragment_float_finite(constants[i]))
			return -EINVAL;
	ret = chrome9_fragment_allocate(&state, instruction_count);
	if (ret)
		return ret;
	program->constant_count = constant_count;
	program->texture_count = texture_count;
	for (i = 0; i < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; i++) {
		u32 packed = program->varying_count;

		if (!(state.varying_mask & BIT(i)))
			continue;
		program->varying_logical[packed] = i;
		program->varying_width[packed] = state.varying_width[i];
		program->varying_count++;
	}
	if (constant_count)
		memcpy(program->constants, constants,
		       constant_count * 4 * sizeof(*constants));
	return chrome9_fragment_encode(&state, program, instruction_count);
}

int chrome9_fragment_selftest(void)
{
	static const struct drm_chrome9_fragment_instruction computed[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_ADD,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0x3, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0,
				  { 0, 1, 5, 5 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 1,
				  { 0, 1, 5, 5 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 5, 4 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction partial[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0x3, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xc, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction sparse_varyings[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 5,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MUL,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction packed_varying[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 1, 2, 3, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MUL,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 0, 0, 0 }, 0, 0 },
			},
		},
	};
	static const u32 expected_alu[][4] = {
		{ 0x008106d0, 0xd0140924, 0x00308006, 0x00000020 },
		{ 0x01c00530, 0xc01c0053, 0x00f20006, 0x000d0420 },
		{ 0x01c00530, 0x301c0053, 0x00f20105, 0x000d0820 },
	};
	static const u32 expected_controls[] = {
		0x90058000, 0x91000001, 0x92010003, 0x93000001,
		0x94000000, 0x95000002, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
	};
	static const u32 expected_sparse_controls[] = {
		0x90018001, 0x91000002, 0x92010001, 0x93000001,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
	};
	static const u32 constants[8];
	struct chrome9_fragment_program *program;
	int ret;

	program = kzalloc_obj(*program);
	if (!program)
		return -ENOMEM;
	ret = chrome9_fragment_compile(computed, ARRAY_SIZE(computed), constants,
		2, 1, 0, program);
	if (ret || program->alu_count != ARRAY_SIZE(expected_alu) ||
	    program->tau_count != 1 || program->tau[0] != 0x301101b0 ||
	    memcmp(program->alu, expected_alu, sizeof(expected_alu)) ||
	    memcmp(program->controls, expected_controls,
		   sizeof(expected_controls))) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(partial, ARRAY_SIZE(partial), constants,
		2, 0, 0, program);
	if (ret)
		goto out;
	ret = chrome9_fragment_compile(sparse_varyings,
		ARRAY_SIZE(sparse_varyings), constants, 0, 1, 6, program);
	if (ret || program->alu_count != 1 || program->tau_count != 1 ||
	    program->tau[0] != 0x301011b0 ||
	    program->varying_count != 2 || program->varying_logical[0] != 0 ||
	    program->varying_width[0] != 4 ||
	    program->varying_logical[1] != 5 ||
	    program->varying_width[1] != 2 ||
	    memcmp(program->controls, expected_sparse_controls,
		   sizeof(expected_sparse_controls)))
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = chrome9_fragment_compile(packed_varying,
		ARRAY_SIZE(packed_varying), constants, 0, 1, 1, program);
	if (ret || program->alu_count != 1 || program->tau_count != 1 ||
	    program->tau[0] != 0x301006f0 || program->varying_count != 1 ||
	    program->varying_logical[0] || program->varying_width[0] != 3 ||
	    program->controls[0] != 0x90018000 ||
	    program->controls[1] != 0x91000001 ||
	    program->controls[2] != 0x92010001 ||
	    program->controls[3] != 0x93000001)
		ret = -EINVAL;
out:
	kfree(program);
	return ret;
}
