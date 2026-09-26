/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_fragment.h"

#include <string.h>

#include "util/bitscan.h"
#include "util/macros.h"


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

unsigned
chrome9_fragment_descriptor_unit(uint8_t sampler_mask, unsigned descriptor)
{
   unsigned mask = sampler_mask;

   for (unsigned i = 0; i < descriptor && mask; i++)
      mask &= mask - 1;
   return mask ? __builtin_ctz(mask) : DRM_CHROME9_FRAGMENT_MAX_TEXTURES;
}

static unsigned chrome9_fragment_source_count(enum chrome9_ps_opcode op);

static bool
chrome9_fragment_three_distinct_constants(
   const struct chrome9_ps_instruction *instruction)
{
   const struct chrome9_ps_source *a = &instruction->source[0];
   const struct chrome9_ps_source *c = &instruction->source[1];
   const struct chrome9_ps_source *b = &instruction->source[2];

   return (instruction->op == CHROME9_PS_OPCODE_MAD ||
           instruction->op == CHROME9_PS_OPCODE_CMP) &&
          a->file == CHROME9_PS_SOURCE_CONSTANT &&
          c->file == CHROME9_PS_SOURCE_CONSTANT &&
          b->file == CHROME9_PS_SOURCE_CONSTANT &&
          a->id != c->id && a->id != b->id && c->id != b->id;
}

bool
chrome9_fragment_legalize_constant_ports(struct chrome9_ps_ir *ir)
{
   unsigned next_temp = 0;
   unsigned alu_count = 0;

   if (!ir || ir->count > CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction = &ir->instruction[i];
      unsigned source_count = chrome9_fragment_source_count(instruction->op);

      alu_count += instruction->op != CHROME9_PS_OPCODE_TEX;
      if (instruction->op != CHROME9_PS_OPCODE_KILL &&
          instruction->destination.file == CHROME9_PS_DESTINATION_TEMP)
         next_temp = MAX2(next_temp, (unsigned)instruction->destination.id + 1);
      for (unsigned s = 0; s < source_count; s++)
         if (instruction->source[s].file == CHROME9_PS_SOURCE_TEMP)
            next_temp = MAX2(next_temp, (unsigned)instruction->source[s].id + 1);
   }
   for (unsigned i = 0; i < ir->count; i++) {
      struct chrome9_ps_instruction *move, *consumer;
      unsigned temp, mask;

      if (!chrome9_fragment_three_distinct_constants(&ir->instruction[i]))
         continue;
      if (ir->count >= CHROME9_PS_MAX_INSTRUCTIONS ||
          alu_count >= CHROME9_PS_MAX_ALU ||
          next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
         return false;
      memmove(&ir->instruction[i + 1], &ir->instruction[i],
              (ir->count - i) * sizeof(ir->instruction[0]));
      move = &ir->instruction[i];
      consumer = &ir->instruction[i + 1];
      temp = next_temp++;
      mask = consumer->destination.writemask;
      memset(move, 0, sizeof(*move));
      move->op = CHROME9_PS_OPCODE_MOV;
      move->destination.file = CHROME9_PS_DESTINATION_TEMP;
      move->destination.id = temp;
      move->destination.writemask = mask;
      move->source[0] = consumer->source[1];
      memset(&consumer->source[1], 0, sizeof(consumer->source[1]));
      consumer->source[1].file = CHROME9_PS_SOURCE_TEMP;
      consumer->source[1].id = temp;
      for (unsigned c = 0; c < 4; c++)
         consumer->source[1].swizzle[c] = (mask & (1u << c)) ?
            c : CHROME9_PS_SWIZZLE_ZERO;
      ir->count++;
      alu_count++;
      i++;
   }
   return true;
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
      if (source->id)
         return false;
      result->file = DRM_CHROME9_FRAGMENT_FILE_PRIMARY;
      break;
   case CHROME9_PS_SOURCE_TEXTURE:
      result->file = DRM_CHROME9_FRAGMENT_FILE_VARYING;
      break;
   case CHROME9_PS_SOURCE_CONSTANT:
      result->file = DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
      break;
   case CHROME9_PS_SOURCE_POSITION:
      if (source->id)
         return false;
      result->file = DRM_CHROME9_FRAGMENT_FILE_POSITION;
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
   case CHROME9_PS_OPCODE_KILL:
      *result = DRM_CHROME9_FRAGMENT_OP_KILL;
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
   case CHROME9_PS_OPCODE_KILL:
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

static bool
chrome9_fragment_source_empty(const struct chrome9_ps_source *source)
{
   if (source->file || source->id || source->modifier || source->last_use)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (source->swizzle[c])
         return false;
   return true;
}

bool
chrome9_fragment_build_instructions(
   const struct chrome9_ps_ir *ir, uint8_t sampler_mask,
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS],
   unsigned *count)
{
   struct chrome9_ps_ir expanded;

   if (!ir || !instructions || !count || !ir->count ||
       ir->count > DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS)
      return false;
   if (ir->depth_output) {
      struct chrome9_ps_instruction *depth;

      if (ir->count >= CHROME9_PS_MAX_INSTRUCTIONS ||
          ir->count >= DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS)
         return false;
      expanded = *ir;
      expanded.instruction[ir->count] = ir->instruction[ir->count - 1];
      depth = &expanded.instruction[ir->count - 1];
      memset(depth, 0, sizeof(*depth));
      depth->op = CHROME9_PS_OPCODE_MOV;
      depth->destination.file = CHROME9_PS_DESTINATION_DEPTH;
      depth->destination.writemask = 0xf;
      depth->source[0] = ir->depth;
      expanded.count++;
      expanded.depth_output = false;
      ir = &expanded;
   }

   memset(instructions, 0,
          DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS * sizeof(*instructions));
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *source = &ir->instruction[i];
      struct drm_chrome9_fragment_instruction *instruction =
         &instructions[i];
      unsigned source_count = chrome9_fragment_source_count(source->op);

      if (!source_count ||
          chrome9_fragment_three_distinct_constants(source) ||
          !chrome9_fragment_opcode(source->op, &instruction->op))
         return false;
      if (source->op == CHROME9_PS_OPCODE_KILL) {
         if (source->destination.file || source->destination.id ||
             source->destination.writemask ||
             source->destination.saturate || source->sampler ||
             source->source[0].file != CHROME9_PS_SOURCE_TEMP ||
             source->source[0].modifier ||
             !chrome9_fragment_source_empty(&source->source[1]) ||
             !chrome9_fragment_source_empty(&source->source[2]))
            return false;
         for (unsigned c = 0; c < 4; c++)
            if (source->source[0].swizzle[c] != c)
               return false;
      } else {
         switch (source->destination.file) {
         case CHROME9_PS_DESTINATION_TEMP:
            instruction->dst.file = DRM_CHROME9_FRAGMENT_DEST_TEMP;
            break;
         case CHROME9_PS_DESTINATION_COLOR:
            instruction->dst.file = DRM_CHROME9_FRAGMENT_DEST_COLOR;
            break;
         case CHROME9_PS_DESTINATION_DEPTH:
            instruction->dst.file = DRM_CHROME9_FRAGMENT_DEST_DEPTH;
            break;
         default:
            return false;
         }
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
         instruction->flags = source->flags;
      } else if (source->flags) {
         return false;
      }
      for (unsigned s = 0; s < source_count; s++)
         if (!chrome9_fragment_source(&source->source[s],
                                      &instruction->src[s]))
            return false;
      for (unsigned s = 0; s < source_count; s++) {
         const struct drm_chrome9_fragment_source *input =
            &instruction->src[s];
         unsigned used_lanes = source->destination.writemask;

         if (input->file != DRM_CHROME9_FRAGMENT_FILE_POSITION)
            continue;
         if (source->op == CHROME9_PS_OPCODE_TEX ||
             source->op == CHROME9_PS_OPCODE_KILL)
            return false;
         if (source->op == CHROME9_PS_OPCODE_DP3)
            used_lanes = 0x7;
         else if (source->op == CHROME9_PS_OPCODE_DP4)
            used_lanes = 0xf;
         else if (source->op == CHROME9_PS_OPCODE_EXP2 ||
                  source->op == CHROME9_PS_OPCODE_LOG2 ||
                  source->op == CHROME9_PS_OPCODE_RCP ||
                  source->op == CHROME9_PS_OPCODE_RSQ)
            used_lanes = 0x1;
         for (unsigned lane = 0; lane < 4; lane++)
            if ((used_lanes & (1u << lane)) &&
                input->swizzle[lane] > DRM_CHROME9_FRAGMENT_SWIZZLE_Y)
               return false;
      }
   }
   *count = ir->count;
   return true;
}

/* Sources a kernel instruction reads, -1 for an unknown operation. */
static int
chrome9_fragment_native_source_count(unsigned op)
{
   switch (op) {
   case DRM_CHROME9_FRAGMENT_OP_TEX:
   case DRM_CHROME9_FRAGMENT_OP_MOV:
   case DRM_CHROME9_FRAGMENT_OP_EXP2:
   case DRM_CHROME9_FRAGMENT_OP_LOG2:
   case DRM_CHROME9_FRAGMENT_OP_RCP:
   case DRM_CHROME9_FRAGMENT_OP_RSQ:
   case DRM_CHROME9_FRAGMENT_OP_ABS:
   case DRM_CHROME9_FRAGMENT_OP_FRC:
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
      return -1;
   }
}

uint32_t
chrome9_fragment_drop_dead_textures(
   struct drm_chrome9_fragment_instruction *instructions, unsigned *count)
{
   bool read[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { false };
   uint32_t used = 0;
   unsigned kept = 0;

   for (unsigned i = 0; i < *count; i++) {
      const struct drm_chrome9_fragment_instruction *instruction =
         &instructions[i];
      const int sources =
         chrome9_fragment_native_source_count(instruction->op);

      for (int s = 0; s < sources; s++) {
         const struct drm_chrome9_fragment_source *input =
            &instruction->src[s];
         bool lane_read = false;

         for (unsigned lane = 0; lane < 4; lane++)
            lane_read |= input->swizzle[lane] < 4;
         if (input->file == DRM_CHROME9_FRAGMENT_FILE_TEMP &&
             input->index < CHROME9_PS_MAX_VIRTUAL_TEMPS &&
             (lane_read || instruction->op == DRM_CHROME9_FRAGMENT_OP_KILL))
            read[input->index] = true;
      }
   }
   /* Lane-free reads of a removed temporary move to a constant register
    * the program already reads, which leaves the constant count alone. */
   int constant = -1;

   for (unsigned i = 0; i < *count && constant < 0; i++) {
      const int sources =
         chrome9_fragment_native_source_count(instructions[i].op);

      for (int s = 0; s < sources; s++)
         if (instructions[i].src[s].file == DRM_CHROME9_FRAGMENT_FILE_CONSTANT)
            constant = instructions[i].src[s].index;
   }
   for (unsigned i = 0; i < *count; i++) {
      struct drm_chrome9_fragment_instruction *instruction = &instructions[i];

      if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX &&
          instruction->dst.file == DRM_CHROME9_FRAGMENT_DEST_TEMP &&
          instruction->dst.index < CHROME9_PS_MAX_VIRTUAL_TEMPS &&
          !read[instruction->dst.index] && constant >= 0) {
         const unsigned temp = instruction->dst.index;

         for (unsigned j = i + 1; j < *count; j++) {
            const int sources =
               chrome9_fragment_native_source_count(instructions[j].op);

            for (int s = 0; s < sources; s++) {
               struct drm_chrome9_fragment_source *input =
                  &instructions[j].src[s];

               if (input->file == DRM_CHROME9_FRAGMENT_FILE_TEMP &&
                   input->index == temp) {
                  input->file = DRM_CHROME9_FRAGMENT_FILE_CONSTANT;
                  input->index = constant;
               }
            }
         }
         continue;
      }
      if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX)
         used |= 1u << instruction->sampler;
      instructions[kept++] = *instruction;
   }
   *count = kept;
   return used;
}

unsigned
chrome9_fragment_trim_textures(
   struct drm_chrome9_fragment_texture_state *textures, unsigned count,
   uint32_t used, bool sparse)
{
   unsigned kept = 0;

   if (!sparse)
      return MIN2(count, used ? util_last_bit(used) : 0);
   for (unsigned i = 0; i < count; i++)
      if (used & (1u << textures[i].reserved0))
         textures[kept++] = textures[i];
   return kept;
}

bool
chrome9_fragment_apply_depth_swizzles(
   struct drm_chrome9_fragment_instruction *instructions, unsigned count,
   uint8_t depth_sampler_mask,
   const uint8_t swizzle[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4])
{
   uint8_t mapped[CHROME9_PS_MAX_VIRTUAL_TEMPS][4];
   uint8_t depth_lane[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4] = { 0 };

   if (!instructions || !swizzle || count > DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS)
      return false;
   for (unsigned unit = 0; unit < DRM_CHROME9_FRAGMENT_MAX_TEXTURES; unit++) {
      if (!(depth_sampler_mask & (1u << unit)))
         continue;
      for (unsigned lane = 0; lane < 4; lane++)
         if (swizzle[unit][lane] > DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO)
            return false;
         else
            depth_lane[unit][lane] = swizzle[unit][lane];
   }
   for (unsigned temp = 0; temp < CHROME9_PS_MAX_VIRTUAL_TEMPS; temp++)
      for (unsigned lane = 0; lane < 4; lane++)
         mapped[temp][lane] = lane;
   for (unsigned i = 0; i < count; i++) {
      struct drm_chrome9_fragment_instruction *instruction =
         &instructions[i];
      unsigned live_sources;

      switch (instruction->op) {
      case DRM_CHROME9_FRAGMENT_OP_TEX:
      case DRM_CHROME9_FRAGMENT_OP_MOV:
      case DRM_CHROME9_FRAGMENT_OP_EXP2:
      case DRM_CHROME9_FRAGMENT_OP_LOG2:
      case DRM_CHROME9_FRAGMENT_OP_RCP:
      case DRM_CHROME9_FRAGMENT_OP_RSQ:
      case DRM_CHROME9_FRAGMENT_OP_ABS:
      case DRM_CHROME9_FRAGMENT_OP_FRC:
      case DRM_CHROME9_FRAGMENT_OP_KILL:
         live_sources = 1;
         break;
      case DRM_CHROME9_FRAGMENT_OP_MUL:
      case DRM_CHROME9_FRAGMENT_OP_ADD:
      case DRM_CHROME9_FRAGMENT_OP_SUB:
      case DRM_CHROME9_FRAGMENT_OP_MIN:
      case DRM_CHROME9_FRAGMENT_OP_MAX:
      case DRM_CHROME9_FRAGMENT_OP_DP3:
      case DRM_CHROME9_FRAGMENT_OP_DP4:
         live_sources = 2;
         break;
      case DRM_CHROME9_FRAGMENT_OP_MAD:
      case DRM_CHROME9_FRAGMENT_OP_CMP:
         live_sources = 3;
         break;
      default:
         return false;
      }
      for (unsigned source = 0; source < live_sources; source++) {
         struct drm_chrome9_fragment_source *input =
            &instruction->src[source];

         if (input->file != DRM_CHROME9_FRAGMENT_FILE_TEMP) {
            if (instruction->op == DRM_CHROME9_FRAGMENT_OP_KILL)
               return false;
            continue;
         }
         if (input->index >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         if (instruction->op == DRM_CHROME9_FRAGMENT_OP_KILL) {
            /* KILL must read its TEMP as XYZW.  A depth-view swizzle has
             * to be consumed by an earlier ALU result, not by KILL itself.
             */
            for (unsigned lane = 0; lane < 4; lane++)
               if (input->swizzle[lane] != lane ||
                   mapped[input->index][lane] != lane)
                  return false;
            continue;
         }
         for (unsigned lane = 0; lane < 4; lane++) {
            if (input->swizzle[lane] < 4)
               input->swizzle[lane] =
                  mapped[input->index][input->swizzle[lane]];
         }
      }
      if (instruction->dst.file == DRM_CHROME9_FRAGMENT_DEST_TEMP) {
         if (instruction->dst.index >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         for (unsigned lane = 0; lane < 4; lane++) {
            if (!(instruction->dst.writemask & (1u << lane)))
               continue;
            /* A depth TEX writes Z to every lane: the depth channel is
             * read from the written lane itself (the TEX may not write X). */
            mapped[instruction->dst.index][lane] =
               instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX &&
               instruction->sampler < DRM_CHROME9_FRAGMENT_MAX_TEXTURES &&
               (depth_sampler_mask & (1u << instruction->sampler)) &&
               depth_lane[instruction->sampler][lane] !=
                  DRM_CHROME9_FRAGMENT_SWIZZLE_X ?
               depth_lane[instruction->sampler][lane] : lane;
         }
      } else if (instruction->op == DRM_CHROME9_FRAGMENT_OP_TEX &&
                 instruction->sampler < DRM_CHROME9_FRAGMENT_MAX_TEXTURES &&
                 (depth_sampler_mask & (1u << instruction->sampler))) {
         /* There is no source-color register for a post-TEX swizzle. */
         for (unsigned lane = 0; lane < 4; lane++)
            if ((instruction->dst.writemask & (1u << lane)) &&
                depth_lane[instruction->sampler][lane] !=
                   DRM_CHROME9_FRAGMENT_SWIZZLE_X)
               return false;
      }
   }
   return true;
}
