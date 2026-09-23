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
	u16 varying_last_alu[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
};

struct chrome9_fragment_resource_state {
	u8 external[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	u8 defined[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
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
	case DRM_CHROME9_FRAGMENT_OP_KILL:
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
	u32 kill_count = 0;
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
	memset(state->varying_last_alu, 0xff,
	       sizeof(state->varying_last_alu));
	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		unsigned int source_count =
			chrome9_fragment_op_sources(instruction->op);
		unsigned int source;

		if (!source_count || instruction->reserved)
			return -EINVAL;
		if (instruction->op == DRM_CHROME9_FRAGMENT_OP_KILL) {
			const struct drm_chrome9_fragment_source *predicate =
				&instruction->src[0];

			if (++kill_count > 1 || output_mask ||
			    ++alu_count > DRM_CHROME9_FRAGMENT_MAX_ALU ||
			    ++tau_count > DRM_CHROME9_FRAGMENT_MAX_TAU ||
			    instruction->sampler || instruction->dst.file ||
			    instruction->dst.index || instruction->dst.writemask ||
			    instruction->dst.modifier ||
			    predicate->file != DRM_CHROME9_FRAGMENT_FILE_TEMP ||
			    predicate->modifier || predicate->reserved ||
			    predicate->swizzle[0] != 0 ||
			    predicate->swizzle[1] != 1 ||
			    predicate->swizzle[2] != 2 ||
			    predicate->swizzle[3] != 3 ||
			    !chrome9_fragment_source_zero(&instruction->src[1]) ||
			    !chrome9_fragment_source_zero(&instruction->src[2]))
				return -EINVAL;
			state->source_count[i] = 1;
			if (chrome9_fragment_validate_source(state, predicate, i))
				return -EINVAL;
			continue;
		}
		if (instruction->dst.modifier &
		    ~DRM_CHROME9_FRAGMENT_DEST_SAT ||
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
	/* A vector operation defines its whole write mask even when a later
	 * swizzle consumes only some lanes (for example raw depth sampled as
	 * Gallium's (Z, 0, 0, 1)).  Reject a wholly dead temporary, not legal
	 * unused components of a live one. */
	for (i = 0; i < CHROME9_FRAGMENT_TEMP_COUNT; i++)
		if (state->defined_mask[i] && !state->used_mask[i])
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
	u8 kill_coordinate;
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
	kill_coordinate = coordinate_count;
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
		kill_coordinate = max(kill_coordinate, computed_coordinate);
		if (computed_coordinate == DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			computed_coordinate = coordinate_count;
	}
	/* TEXKILL's address register is internal and cannot alias a produced
	 * direct varying or a computed texture coordinate in this first subset.
	 */
	for (i = 0; i < instruction_count; i++) {
		if (state->instructions[i].op != DRM_CHROME9_FRAGMENT_OP_KILL)
			continue;
		if (kill_coordinate == DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
			return -E2BIG;
		state->coordinate_for_instruction[i] = kill_coordinate;
	}

	for (i = 0; i < instruction_count; i++) {
		const struct drm_chrome9_fragment_instruction *instruction =
			&state->instructions[i];
		unsigned int source;

		if (instruction->op != DRM_CHROME9_FRAGMENT_OP_KILL &&
		    instruction->dst.file == DRM_CHROME9_FRAGMENT_DEST_TEMP &&
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
		unsigned int source;
		u8 slot;

		if (instruction->op == DRM_CHROME9_FRAGMENT_OP_KILL) {
			slot = state->coordinate_for_instruction[i];
			state->coordinate_last_tau[slot] = i;
			continue;
		}
		if (instruction->op != DRM_CHROME9_FRAGMENT_OP_TEX) {
			for (source = 0; source < state->source_count[i]; source++)
				if (instruction->src[source].file ==
				    DRM_CHROME9_FRAGMENT_FILE_VARYING)
					state->varying_last_alu[
						instruction->src[source].index] = i;
			continue;
		}
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

static u8 chrome9_fragment_resource_swizzle_mask(u8 x, u8 y, u8 z, u8 w)
{
	const u8 swizzle[] = { x, y, z, w };
	u8 mask = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(swizzle); i++)
		if (swizzle[i] < 4)
			mask |= BIT(swizzle[i]);
	return mask;
}

static int chrome9_fragment_resource_note_source(
	struct chrome9_fragment_resource_state *resource, u32 id, u8 mask)
{
	if (id >= ARRAY_SIZE(resource->external))
		return -E2BIG;
	resource->external[id] |= mask & ~resource->defined[id];
	return 0;
}

static int chrome9_fragment_resource_note_destination(
	struct chrome9_fragment_resource_state *resource, u32 file, u32 id,
	u8 mask)
{
	if (file != 1)
		return 0;
	if (id >= ARRAY_SIZE(resource->defined))
		return -E2BIG;
	resource->defined[id] |= mask & ~resource->external[id];
	return 0;
}

static int chrome9_fragment_resource_note_alu(
	struct chrome9_fragment_resource_state *resource, const u32 record[4])
{
	u32 file, id;
	u8 mask;
	int ret;

	/* The production analyzer admits at most one texture-class source per
	 * ALU in native A, C, B priority.  Do not count every semantic operand:
	 * a one-record MAD of T0, T1 and T2 consumes one active resource. */
	file = FIELD_GET(GENMASK(16, 14), record[2]);
	if (file == 3) {
		id = FIELD_GET(GENMASK(13, 8), record[2]);
		mask = chrome9_fragment_resource_swizzle_mask(
			FIELD_GET(GENMASK(7, 5), record[2]),
			FIELD_GET(GENMASK(4, 2), record[2]),
			FIELD_GET(GENMASK(1, 0), record[2]) << 1 |
				FIELD_GET(BIT(31), record[1]),
			FIELD_GET(GENMASK(30, 28), record[1]));
		ret = chrome9_fragment_resource_note_source(resource, id, mask);
		if (ret)
			return ret;
	} else {
		file = FIELD_GET(GENMASK(20, 18), record[1]);
		if (file == 3) {
			id = FIELD_GET(GENMASK(17, 12), record[1]);
			mask = chrome9_fragment_resource_swizzle_mask(
				FIELD_GET(GENMASK(11, 9), record[1]),
				FIELD_GET(GENMASK(8, 6), record[1]),
				FIELD_GET(GENMASK(5, 3), record[1]),
				FIELD_GET(GENMASK(2, 0), record[1]));
			ret = chrome9_fragment_resource_note_source(resource, id,
							    mask);
			if (ret)
				return ret;
		} else {
			file = FIELD_GET(GENMASK(24, 22), record[0]);
			if (file == 3) {
				id = FIELD_GET(GENMASK(21, 16), record[0]);
				mask = chrome9_fragment_resource_swizzle_mask(
					FIELD_GET(GENMASK(15, 13), record[0]),
					FIELD_GET(GENMASK(12, 10), record[0]),
					FIELD_GET(GENMASK(9, 7), record[0]),
					FIELD_GET(GENMASK(6, 4), record[0]));
				ret = chrome9_fragment_resource_note_source(resource,
								    id, mask);
				if (ret)
					return ret;
			}
		}
	}

	mask = FIELD_GET(GENMASK(23, 20), record[2]);
	ret = chrome9_fragment_resource_note_destination(resource,
		FIELD_GET(GENMASK(11, 10), record[3]),
		FIELD_GET(GENMASK(9, 6), record[3]), mask);
	if (ret)
		return ret;
	return chrome9_fragment_resource_note_destination(resource,
		FIELD_GET(GENMASK(5, 4), record[3]),
		FIELD_GET(GENMASK(3, 0), record[3]), mask);
}

static int chrome9_fragment_resource_note_tau(
	struct chrome9_fragment_resource_state *resource, u32 tau)
{
	return chrome9_fragment_resource_note_source(resource,
		FIELD_GET(GENMASK(15, 12), tau), 0xf);
}

static u32 chrome9_fragment_active_texture_count(
	const struct chrome9_fragment_resource_state *resource)
{
	unsigned int i;

	for (i = ARRAY_SIZE(resource->external); i; i--)
		if (resource->external[i - 1])
			return i;
	return 0;
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

	if (source->file == DRM_CHROME9_FRAGMENT_FILE_VARYING) {
		if (state->varying_last_alu[source->index] != instruction_index)
			return false;
		/*
		 * The production finalizer assigns a repeated varying's release to
		 * its last semantic operand in the instruction.  This differs from
		 * TEMP aliases, whose A/C/B native-slot precedence is handled below.
		 */
		for (other = source_index + 1;
		     other < state->source_count[instruction_index]; other++)
			if (instruction->src[other].file ==
			    DRM_CHROME9_FRAGMENT_FILE_VARYING &&
			    instruction->src[other].index == source->index)
				return false;
		return true;
	}
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
	struct chrome9_fragment_resource_state resource = { 0 };
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

		if (instruction->op == DRM_CHROME9_FRAGMENT_OP_KILL) {
			const struct drm_chrome9_fragment_source *predicate =
				&instruction->src[0];
			enum chrome9_fragment_slot slots[3] = { 0 };
			u32 coordinate_id = state->coordinate_for_instruction[i];
			u32 *bridge = program->alu[alu_index];
			bool release;
			int ret;

			if (previous_phase == 1)
				program->controls[3 + (tau_index - 1) / 24] |=
					BIT((tau_index - 1) % 24);
			ret = chrome9_fragment_alu_template(DRM_CHROME9_FRAGMENT_OP_MOV,
							    bridge, slots);
			if (ret)
				return ret;
			release = chrome9_fragment_release_source(state, instruction,
								  i, 0, slots);
			chrome9_fragment_encode_source(bridge, state, predicate,
						       CHROME9_FRAGMENT_SLOT_A, release);
			bridge[2] = (bridge[2] & ~GENMASK(23, 20)) | 0xf << 20;
			bridge[3] = (bridge[3] & ~GENMASK(12, 6)) |
				BIT(10) | coordinate_id << 6;
			ret = chrome9_fragment_resource_note_alu(&resource, bridge);
			if (ret)
				return ret;
			alu_index++;
			program->controls[5 + (alu_index - 1) / 24] |=
				BIT((alu_index - 1) % 24);
			program->tau[tau_index] = 0x214001b1 | BIT(28) |
				coordinate_id << 12;
			ret = chrome9_fragment_resource_note_tau(&resource,
								 program->tau[tau_index]);
			if (ret)
				return ret;
			state->coordinate_mask |= BIT(coordinate_id);
			active0 = max(active0, coordinate_id + 1);
			tau_index++;
			previous_phase = 1;
			continue;
		}
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

			if (!instruction->sampler) {
				if (program->stage0_coordinate != 0xff &&
				    program->stage0_coordinate != coordinate_id)
					return -EOPNOTSUPP;
				program->stage0_coordinate = coordinate_id;
			}

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
				ret = chrome9_fragment_resource_note_alu(&resource,
								 bridge);
				if (ret)
					return ret;
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
			{
				int ret = chrome9_fragment_resource_note_tau(&resource,
							program->tau[tau_index]);

				if (ret)
					return ret;
			}
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
			ret = chrome9_fragment_resource_note_alu(&resource, record);
			if (ret)
				return ret;
		}
		alu_index++;
		previous_phase = 0;
	}
	program->alu_count = alu_index;
	program->tau_count = tau_index;
	program->active_texture_count =
		chrome9_fragment_active_texture_count(&resource);
	program->tau_coordinate_mask = state->coordinate_mask;
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
	program->stage0_coordinate = 0xff;
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

static bool
chrome9_fragment_kill_rejected(const struct drm_chrome9_fragment_instruction *instructions,
			       const u32 *constants,
			       struct chrome9_fragment_program *program)
{
	return chrome9_fragment_compile(instructions, 3, constants, 2, 0, 0,
		program) == -EINVAL;
}

static int chrome9_fragment_kill_selftest(struct chrome9_fragment_program *program,
					  const u32 *constants)
{
	static const struct drm_chrome9_fragment_instruction constant_kill[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_KILL,
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction sampled_kill[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_SUB,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 3, 3, 3, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0,
				  { 3, 3, 3, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_KILL,
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 1,
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
	static const u32 expected_constant_alu[][4] = {
		{ 0x01c00530, 0x301c0053, 0x00f08005, 0x000d0020 },
		{ 0x01c00530, 0x301c0053, 0x00f20005, 0x000d0420 },
		{ 0x01c00530, 0x301c0053, 0x00f08105, 0x000d0820 },
	};
	static const u32 expected_constant_controls[] = {
		0x90058000, 0x91000001, 0x92010003, 0x93000001,
		0x94000000, 0x95000002, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
	};
	static const u32 expected_sampled_alu[][4] = {
		{ 0x80006db0, 0xb0140924, 0x00f0806d, 0x00000060 },
		{ 0x01c00530, 0x301c0053, 0x00f20105, 0x000d0460 },
		{ 0x01c00530, 0x301c0053, 0x00f20005, 0x000d0820 },
	};
	static const u32 expected_sampled_controls[] = {
		0x90018000, 0x91000003, 0x92020003, 0x93000003,
		0x94000000, 0x95000002, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
	};
	struct drm_chrome9_fragment_instruction bad[ARRAY_SIZE(constant_kill)];
	struct drm_chrome9_fragment_instruction *overflow;
	int ret;
	unsigned int i;

	ret = chrome9_fragment_compile(constant_kill, ARRAY_SIZE(constant_kill),
				       constants, 2, 0, 0, program);
	if (ret || program->alu_count != ARRAY_SIZE(expected_constant_alu) ||
	    program->tau_count != 1 || program->tau[0] != 0x314001b1 ||
	    program->active_texture_count || program->tau_coordinate_mask != 1 ||
	    program->varying_count || program->texture_count ||
	    memcmp(program->alu, expected_constant_alu,
		   sizeof(expected_constant_alu)) ||
	    memcmp(program->controls, expected_constant_controls,
		   sizeof(expected_constant_controls)))
		return -EINVAL;
	ret = chrome9_fragment_compile(sampled_kill, ARRAY_SIZE(sampled_kill),
				       constants, 1, 1, 1, program);
	if (ret || program->alu_count != ARRAY_SIZE(expected_sampled_alu) ||
	    program->tau_count != 2 || program->tau[0] != 0x301001b0 ||
	    program->tau[1] != 0x314011b1 ||
	    program->active_texture_count != 1 ||
	    program->tau_coordinate_mask != 3 ||
	    program->varying_count != 1 || program->varying_width[0] != 2 ||
	    program->texture_count != 1 ||
	    memcmp(program->alu, expected_sampled_alu,
		   sizeof(expected_sampled_alu)) ||
	    memcmp(program->controls, expected_sampled_controls,
		   sizeof(expected_sampled_controls)))
		return -EINVAL;

	/* Every negative starts from a valid transaction, so each mutation
	 * isolates one destination-free KILL rule.
	 */
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].src[0].file = DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].src[0].index = 1;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[0].dst.writemask = 0x7;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].src[0].swizzle[3] = 0;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].src[0].modifier = DRM_CHROME9_FRAGMENT_MOD_NEG;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].dst.file = DRM_CHROME9_FRAGMENT_DEST_COLOR;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].dst.index = 1;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].dst.writemask = 1;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].dst.modifier = DRM_CHROME9_FRAGMENT_DEST_SAT;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].sampler = 1;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].reserved = 1;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[1].src[1].file = DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	memcpy(bad, constant_kill, sizeof(bad));
	bad[2].dst.writemask = 0x7;
	if (!chrome9_fragment_kill_rejected(bad, constants, program))
		return -EINVAL;
	{
		struct drm_chrome9_fragment_instruction twice[4] = {
			constant_kill[0], constant_kill[1],
			constant_kill[1], constant_kill[2],
		};

		if (chrome9_fragment_compile(twice, ARRAY_SIZE(twice), constants,
					     2, 0, 0, program) != -EINVAL)
			return -EINVAL;
	}
	{
		struct drm_chrome9_fragment_instruction late[3] = {
			constant_kill[0], constant_kill[2], constant_kill[1],
		};

		if (chrome9_fragment_compile(late, ARRAY_SIZE(late), constants,
					     2, 0, 0, program) != -EINVAL)
			return -EINVAL;
	}
	overflow = kcalloc(97, sizeof(*overflow), GFP_KERNEL);
	if (!overflow)
		return -ENOMEM;
	for (i = 0; i < 95; i++) {
		overflow[i].op = DRM_CHROME9_FRAGMENT_OP_MOV;
		overflow[i].dst.file = DRM_CHROME9_FRAGMENT_DEST_TEMP;
		overflow[i].dst.index = i;
		overflow[i].dst.writemask = 0xf;
		overflow[i].src[0].file = i ?
			DRM_CHROME9_FRAGMENT_FILE_TEMP :
			DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
		overflow[i].src[0].index = i ? i - 1 : 0;
		overflow[i].src[0].swizzle[0] = 0;
		overflow[i].src[0].swizzle[1] = 1;
		overflow[i].src[0].swizzle[2] = 2;
		overflow[i].src[0].swizzle[3] = 3;
	}
	overflow[95] = constant_kill[1];
	overflow[95].src[0].index = 94;
	overflow[96] = constant_kill[2];
	ret = chrome9_fragment_compile(overflow, 97, constants, 2, 0, 0,
				       program);
	kfree(overflow);
	if (ret != -EINVAL)
		return -EINVAL;
	return 0;
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
	static const struct drm_chrome9_fragment_instruction swizzled_texture[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 0, 1 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 5, 5, 4 }, 0, 0 },
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
	static const struct drm_chrome9_fragment_instruction mixed_coordinates[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 1,
				  { 0, 1, 0, 1 }, 0, 0 },
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
	static const struct drm_chrome9_fragment_instruction packed_gray[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 1, 2, 3, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0x7, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 0, 0, 5 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0x8, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 31,
				  { 5, 5, 5, 0 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MUL,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	/* The production compiler can preserve sampled alpha without a c31
	 * upload by leaving r1.w undefined and selecting literal ONE at use.
	 */
	static const struct drm_chrome9_fragment_instruction packed_gray_partial[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_TEX,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 1, 2, 3, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0x7, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 0, 0, 5 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_MUL,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 1,
				  { 0, 1, 2, 4 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction varying_reuse[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
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
	static const struct drm_chrome9_fragment_instruction varying_duplicate[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MUL,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction resource_mad[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MAD,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 2,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction resource_chain[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_ADD,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_ADD,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 2,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		},
	};
	static const struct drm_chrome9_fragment_instruction resource_repeat[] = {
		{
			.op = DRM_CHROME9_FRAGMENT_OP_MOV,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_ADD,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 1, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
			},
		}, {
			.op = DRM_CHROME9_FRAGMENT_OP_ADD,
			.dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
			.src = {
				{ DRM_CHROME9_FRAGMENT_FILE_TEMP, 1,
				  { 0, 1, 2, 3 }, 0, 0 },
				{ DRM_CHROME9_FRAGMENT_FILE_VARYING, 0,
				  { 0, 1, 2, 3 }, 0, 0 },
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
	static const u32 expected_packed_gray_alu[][4] = {
		{ 0x01c00530, 0x501c0053, 0x0072c000, 0x000d0060 },
		{ 0x01c00530, 0x801c0053, 0x0080afb6, 0x000d0060 },
		{ 0x0140b6d0, 0x30201053, 0x00f20005, 0x00000820 },
	};
	static const u32 expected_packed_gray_controls[] = {
		0x90018000, 0x91000001, 0x92010003, 0x93000001,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
	};
	static const u32 expected_packed_gray_partial_alu[][4] = {
		{ 0x01c00530, 0x501c0053, 0x0072c000, 0x000d0060 },
		{ 0x0140b6d0, 0x30201054, 0x00f20005, 0x00000820 },
	};
	static const u32 expected_packed_gray_partial_controls[] = {
		0x90018000, 0x91000001, 0x92010002, 0x93000001,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
	};
	static const u32 expected_varying_reuse_alu[][4] = {
		{ 0x01c00530, 0x301c0053, 0x00f0c005, 0x000d0020 },
		{ 0x0140b6d0, 0x302c0053, 0x00f20005, 0x00000820 },
	};
	static const u32 expected_varying_duplicate_alu[][4] = {
		{ 0x0140b6d0, 0x302c0053, 0x00f0c005, 0x00000820 },
	};
	static const u32 expected_resource_mad_alu[][4] = {
		{ 0x02c20530, 0x302c1053, 0x00f2c005, 0x00000820 },
	};
	static const u32 expected_resource_chain_alu[][4] = {
		{ 0x01c00530, 0x301c0053, 0x00f2c005, 0x000d0020 },
		{ 0x02c10530, 0x30140924, 0x00f20005, 0x00000060 },
		{ 0x02c20530, 0x30140924, 0x00f20105, 0x00000820 },
	};
	static const u32 constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS * 4];
	struct chrome9_fragment_program *program;
	int ret;

	program = kzalloc_obj(*program);
	if (!program)
		return -ENOMEM;
	ret = chrome9_fragment_compile(computed, ARRAY_SIZE(computed), constants,
		2, 1, 0, program);
	if (ret || program->alu_count != ARRAY_SIZE(expected_alu) ||
	    program->tau_count != 1 || program->tau[0] != 0x301101b0 ||
	    program->active_texture_count != 0 ||
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
	ret = chrome9_fragment_compile(swizzled_texture,
		ARRAY_SIZE(swizzled_texture), constants, 0, 1, 1, program);
	if (ret || program->alu_count != 1 || program->tau_count != 1 ||
	    program->active_texture_count != 1 || program->varying_count != 1 ||
	    program->varying_width[0] != 2) {
		ret = -EINVAL;
		goto out;
	}
	{
		struct drm_chrome9_fragment_instruction dead_texture[2];

		memcpy(dead_texture, swizzled_texture, sizeof(dead_texture));
		dead_texture[1].src[0].file = DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
		dead_texture[1].src[0].index = 0;
		if (chrome9_fragment_compile(dead_texture,
			ARRAY_SIZE(dead_texture), constants, 1, 1, 1,
			program) != -EINVAL) {
			ret = -EINVAL;
			goto out;
		}
	}
	ret = chrome9_fragment_compile(sparse_varyings,
		ARRAY_SIZE(sparse_varyings), constants, 0, 1, 6, program);
	if (ret || program->alu_count != 1 || program->tau_count != 1 ||
	    program->tau[0] != 0x301011b0 ||
	    program->stage0_coordinate != 1 ||
	    program->tau_coordinate_mask != BIT(1) ||
	    program->varying_count != 2 || program->varying_logical[0] != 0 ||
	    program->varying_width[0] != 4 ||
	    program->varying_logical[1] != 5 ||
	    program->varying_width[1] != 2 ||
	    memcmp(program->controls, expected_sparse_controls,
		   sizeof(expected_sparse_controls)))
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = chrome9_fragment_compile(mixed_coordinates,
		ARRAY_SIZE(mixed_coordinates), constants, 0, 1, 2, program);
	if (ret || program->varying_count != 2 ||
	    program->varying_logical[0] != 0 ||
	    program->varying_logical[1] != 1 ||
	    program->varying_width[0] != 4 ||
	    program->varying_width[1] != 2 ||
	    program->stage0_coordinate != 1 ||
	    program->tau_coordinate_mask != BIT(1) ||
	    program->active_texture_count != 2) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(packed_varying,
		ARRAY_SIZE(packed_varying), constants, 0, 1, 1, program);
	if (ret || program->alu_count != 1 || program->tau_count != 1 ||
	    program->tau[0] != 0x301006f0 || program->varying_count != 1 ||
	    program->stage0_coordinate != 0 ||
	    program->tau_coordinate_mask != BIT(0) ||
	    program->varying_logical[0] || program->varying_width[0] != 3 ||
	    program->alu[0][1] != 0x302c0000 ||
	    program->alu[0][2] != 0x00f20005 ||
	    program->controls[0] != 0x90018000 ||
	    program->controls[1] != 0x91000001 ||
	    program->controls[2] != 0x92010001 ||
	    program->controls[3] != 0x93000001)
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = chrome9_fragment_compile(packed_gray, ARRAY_SIZE(packed_gray),
		constants, DRM_CHROME9_FRAGMENT_MAX_CONSTANTS, 1, 1, program);
	if (ret ||
	    program->alu_count != ARRAY_SIZE(expected_packed_gray_alu) ||
	    program->tau_count != 1 || program->tau[0] != 0x301006f0 ||
	    program->active_texture_count != 1 ||
	    program->stage0_coordinate != 0 ||
	    program->tau_coordinate_mask != BIT(0) ||
	    program->varying_count != 1 || program->varying_width[0] != 3 ||
	    memcmp(program->alu, expected_packed_gray_alu,
		   sizeof(expected_packed_gray_alu)) ||
	    memcmp(program->controls, expected_packed_gray_controls,
		   sizeof(expected_packed_gray_controls))) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(packed_gray_partial,
		ARRAY_SIZE(packed_gray_partial), constants, 0, 1, 1, program);
	if (ret ||
	    program->alu_count != ARRAY_SIZE(expected_packed_gray_partial_alu) ||
	    program->tau_count != 1 || program->tau[0] != 0x301006f0 ||
	    program->active_texture_count != 1 ||
	    program->stage0_coordinate != 0 ||
	    program->tau_coordinate_mask != BIT(0) ||
	    program->varying_count != 1 || program->varying_width[0] != 3 ||
	    memcmp(program->alu, expected_packed_gray_partial_alu,
		   sizeof(expected_packed_gray_partial_alu)) ||
	    memcmp(program->controls, expected_packed_gray_partial_controls,
		   sizeof(expected_packed_gray_partial_controls))) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(varying_reuse,
		ARRAY_SIZE(varying_reuse), constants, 0, 0, 1, program);
	if (ret ||
	    program->alu_count != ARRAY_SIZE(expected_varying_reuse_alu) ||
	    memcmp(program->alu, expected_varying_reuse_alu,
		   sizeof(expected_varying_reuse_alu))) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(varying_duplicate,
		ARRAY_SIZE(varying_duplicate), constants, 0, 0, 1, program);
	if (ret ||
	    program->alu_count != ARRAY_SIZE(expected_varying_duplicate_alu) ||
	    memcmp(program->alu, expected_varying_duplicate_alu,
		   sizeof(expected_varying_duplicate_alu)))
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = chrome9_fragment_compile(resource_mad, ARRAY_SIZE(resource_mad),
		constants, 0, 0, 3, program);
	if (ret || program->active_texture_count != 1 ||
	    program->alu_count != ARRAY_SIZE(expected_resource_mad_alu) ||
	    memcmp(program->alu, expected_resource_mad_alu,
		   sizeof(expected_resource_mad_alu))) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(resource_chain,
		ARRAY_SIZE(resource_chain), constants, 0, 0, 3, program);
	if (ret || program->active_texture_count != 3 ||
	    program->alu_count != ARRAY_SIZE(expected_resource_chain_alu) ||
	    memcmp(program->alu, expected_resource_chain_alu,
		   sizeof(expected_resource_chain_alu))) {
		ret = -EINVAL;
		goto out;
	}
	ret = chrome9_fragment_compile(resource_repeat,
		ARRAY_SIZE(resource_repeat), constants, 0, 0, 1, program);
	if (ret || program->active_texture_count != 1)
		ret = -EINVAL;
	if (!ret)
		ret = chrome9_fragment_kill_selftest(program, constants);
out:
	kfree(program);
	return ret;
}
