/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_fragment.h"

#include <string.h>

unsigned
chrome9_fragment_sampler_high_water(uint8_t sampler_mask)
{
   return sampler_mask ? 32 - __builtin_clz(sampler_mask) : 0;
}

bool
chrome9_fragment_sampler_slots_dense(uint8_t sampler_mask)
{
   return sampler_mask == (1u << chrome9_fragment_sampler_high_water(
      sampler_mask)) - 1;
}

static bool
chrome9_fragment_swizzle(enum chrome9_ps_swizzle swizzle, uint8_t *result)
{
   switch (swizzle) {
   case CHROME9_PS_SWIZZLE_X:
      *result = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
      return true;
   case CHROME9_PS_SWIZZLE_Y:
      *result = DRM_CHROME9_FRAGMENT_SWIZZLE_Y;
      return true;
   case CHROME9_PS_SWIZZLE_Z:
      *result = DRM_CHROME9_FRAGMENT_SWIZZLE_Z;
      return true;
   case CHROME9_PS_SWIZZLE_W:
      *result = DRM_CHROME9_FRAGMENT_SWIZZLE_W;
      return true;
   case CHROME9_PS_SWIZZLE_ONE:
      *result = DRM_CHROME9_FRAGMENT_SWIZZLE_ONE;
      return true;
   case CHROME9_PS_SWIZZLE_ZERO:
      *result = DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO;
      return true;
   default:
      return false;
   }
}

static bool
chrome9_fragment_source(const struct chrome9_ps_source *source,
                        struct drm_chrome9_fragment_source *result)
{
   memset(result, 0, sizeof(*result));
   switch (source->file) {
   case CHROME9_PS_SOURCE_TEMP:
      result->file = DRM_CHROME9_FRAGMENT_FILE_TEMP;
      break;
   case CHROME9_PS_SOURCE_INPUT:
   case CHROME9_PS_SOURCE_TEXTURE:
      result->file = DRM_CHROME9_FRAGMENT_FILE_VARYING;
      break;
   case CHROME9_PS_SOURCE_CONSTANT:
      result->file = DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
      break;
   default:
      return false;
   }
   if (source->modifier != 0 &&
       source->modifier != CHROME9_PS_SOURCE_NEGATE)
      return false;
   result->index = source->id;
   result->modifier = source->modifier ? DRM_CHROME9_FRAGMENT_MOD_NEG :
                                        DRM_CHROME9_FRAGMENT_MOD_NONE;
   for (unsigned c = 0; c < 4; c++)
      if (!chrome9_fragment_swizzle(source->swizzle[c],
                                    &result->swizzle[c]))
         return false;
   return true;
}

static bool
chrome9_fragment_opcode(enum chrome9_ps_opcode op, uint8_t *result)
{
   switch (op) {
   case CHROME9_PS_OPCODE_TEX:
      *result = DRM_CHROME9_FRAGMENT_OP_TEX;
      return true;
   case CHROME9_PS_OPCODE_MOV:
      *result = DRM_CHROME9_FRAGMENT_OP_MOV;
      return true;
   case CHROME9_PS_OPCODE_MUL:
      *result = DRM_CHROME9_FRAGMENT_OP_MUL;
      return true;
   case CHROME9_PS_OPCODE_ADD:
      *result = DRM_CHROME9_FRAGMENT_OP_ADD;
      return true;
   case CHROME9_PS_OPCODE_SUB:
      *result = DRM_CHROME9_FRAGMENT_OP_SUB;
      return true;
   case CHROME9_PS_OPCODE_MAD:
      *result = DRM_CHROME9_FRAGMENT_OP_MAD;
      return true;
   case CHROME9_PS_OPCODE_MIN:
      *result = DRM_CHROME9_FRAGMENT_OP_MIN;
      return true;
   case CHROME9_PS_OPCODE_MAX:
      *result = DRM_CHROME9_FRAGMENT_OP_MAX;
      return true;
   case CHROME9_PS_OPCODE_DP3:
      *result = DRM_CHROME9_FRAGMENT_OP_DP3;
      return true;
   case CHROME9_PS_OPCODE_DP4:
      *result = DRM_CHROME9_FRAGMENT_OP_DP4;
      return true;
   case CHROME9_PS_OPCODE_EXP2:
      *result = DRM_CHROME9_FRAGMENT_OP_EXP2;
      return true;
   case CHROME9_PS_OPCODE_LOG2:
      *result = DRM_CHROME9_FRAGMENT_OP_LOG2;
      return true;
   case CHROME9_PS_OPCODE_RCP:
      *result = DRM_CHROME9_FRAGMENT_OP_RCP;
      return true;
   case CHROME9_PS_OPCODE_RSQ:
      *result = DRM_CHROME9_FRAGMENT_OP_RSQ;
      return true;
   case CHROME9_PS_OPCODE_ABS:
      *result = DRM_CHROME9_FRAGMENT_OP_ABS;
      return true;
   case CHROME9_PS_OPCODE_CMP:
      *result = DRM_CHROME9_FRAGMENT_OP_CMP;
      return true;
   case CHROME9_PS_OPCODE_FRC:
      *result = DRM_CHROME9_FRAGMENT_OP_FRC;
      return true;
   default:
      return false;
   }
}

static unsigned
chrome9_fragment_source_count(enum chrome9_ps_opcode op)
{
   switch (op) {
   case CHROME9_PS_OPCODE_TEX:
   case CHROME9_PS_OPCODE_MOV:
   case CHROME9_PS_OPCODE_EXP2:
   case CHROME9_PS_OPCODE_LOG2:
   case CHROME9_PS_OPCODE_RCP:
   case CHROME9_PS_OPCODE_RSQ:
   case CHROME9_PS_OPCODE_ABS:
   case CHROME9_PS_OPCODE_FRC:
      return 1;
   case CHROME9_PS_OPCODE_MUL:
   case CHROME9_PS_OPCODE_ADD:
   case CHROME9_PS_OPCODE_SUB:
   case CHROME9_PS_OPCODE_MIN:
   case CHROME9_PS_OPCODE_MAX:
   case CHROME9_PS_OPCODE_DP3:
   case CHROME9_PS_OPCODE_DP4:
      return 2;
   case CHROME9_PS_OPCODE_MAD:
   case CHROME9_PS_OPCODE_CMP:
      return 3;
   default:
      return 0;
   }
}

bool
chrome9_fragment_build_instructions(
   const struct chrome9_ps_ir *ir, uint8_t sampler_mask,
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS],
   unsigned *count)
{
   if (!ir || !instructions || !count || !ir->count ||
       ir->count > DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS)
      return false;

   memset(instructions, 0,
          DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS * sizeof(*instructions));
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *source = &ir->instruction[i];
      struct drm_chrome9_fragment_instruction *instruction =
         &instructions[i];
      unsigned source_count = chrome9_fragment_source_count(source->op);

      if (!source_count ||
          !chrome9_fragment_opcode(source->op, &instruction->op))
         return false;
      switch (source->destination.file) {
      case CHROME9_PS_DESTINATION_TEMP:
         instruction->dst.file = DRM_CHROME9_FRAGMENT_DEST_TEMP;
         break;
      case CHROME9_PS_DESTINATION_COLOR:
         instruction->dst.file = DRM_CHROME9_FRAGMENT_DEST_COLOR;
         break;
      default:
         return false;
      }
      instruction->dst.index = source->destination.id;
      instruction->dst.writemask = source->destination.writemask;
      instruction->dst.modifier = source->destination.saturate ?
         DRM_CHROME9_FRAGMENT_DEST_SAT : 0;
      if (source->op == CHROME9_PS_OPCODE_TEX) {
         if (source->sampler >= 8 ||
             !(sampler_mask & (1u << source->sampler)))
            return false;
         /* TAU records retain the logical sampler id, including gaps. */
         instruction->sampler = source->sampler;
      }
      for (unsigned s = 0; s < source_count; s++)
         if (!chrome9_fragment_source(&source->source[s],
                                      &instruction->src[s]))
            return false;
   }
   *count = ir->count;
   return true;
}
