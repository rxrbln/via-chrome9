/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_shader_offline.h"

#include <string.h>

static void set_controls(struct chrome9_ps_program *p, uint32_t a90,
                         uint32_t a91, uint32_t a92, uint32_t a93,
                         uint32_t a95);

static bool
source_valid(const struct chrome9_ps_source *source)
{
   unsigned limit;

   if (!source)
      return false;
   switch (source->file) {
   case CHROME9_PS_SOURCE_TEMP:
      limit = 12;
      break;
   case CHROME9_PS_SOURCE_INPUT:
      limit = 2;
      break;
   case CHROME9_PS_SOURCE_CONSTANT:
      limit = 32;
      break;
   case CHROME9_PS_SOURCE_TEXTURE:
      limit = 8;
      break;
   default:
      return false;
   }
   if (source->id >= limit ||
       (source->modifier != 0 &&
        source->modifier != CHROME9_PS_SOURCE_NEGATE))
      return false;
   for (unsigned i = 0; i < 4; i++) {
      if (source->swizzle[i] > CHROME9_PS_SWIZZLE_ZERO)
         return false;
   }
   return true;
}

static bool
destination_valid(const struct chrome9_ps_destination *destination)
{
   if (!destination || !destination->writemask ||
       destination->writemask > 0xf)
      return false;
   switch (destination->file) {
   case CHROME9_PS_DESTINATION_TEMP:
      return destination->id < 12;
   case CHROME9_PS_DESTINATION_COLOR:
      return destination->id == 0;
   default:
      return false;
   }
}

static void
encode_destination(uint32_t record[4],
                   const struct chrome9_ps_destination *destination)
{
   record[2] &= ~0x00f00000;
   record[3] &= ~0x00001fc0;
   record[2] |= (uint32_t)destination->writemask << 20;
   record[3] |= (uint32_t)destination->id << 6 |
                (uint32_t)destination->file << 10 |
                (uint32_t)destination->saturate << 12;
}

static void
encode_source_a(uint32_t record[4], const struct chrome9_ps_source *source)
{
   record[1] &= 0x00ffffff;
   record[2] &= ~0x0003ffff;
   record[1] |= (uint32_t)source->modifier << 24 |
                (uint32_t)source->swizzle[3] << 28 |
                (uint32_t)(source->swizzle[2] & 1) << 31;
   record[2] |= (uint32_t)source->swizzle[0] << 5 |
                (uint32_t)source->swizzle[1] << 2 |
                (uint32_t)source->swizzle[2] >> 1 |
                (uint32_t)source->id << 8 |
                (uint32_t)source->file << 14 |
                (uint32_t)source->last_use << 17;
}

static void
encode_source_c(uint32_t record[4], const struct chrome9_ps_source *source)
{
   record[0] &= 0x0fffffff;
   record[1] &= ~0x003fffff;
   record[3] &= ~0x00100000;
   record[0] |= (uint32_t)source->modifier << 28;
   record[1] |= (uint32_t)source->swizzle[0] << 9 |
                (uint32_t)source->swizzle[1] << 6 |
                (uint32_t)source->swizzle[2] << 3 |
                (uint32_t)source->swizzle[3] |
                (uint32_t)source->id << 12 |
                (uint32_t)source->file << 18 |
                (uint32_t)source->last_use << 21;
   if (source->modifier)
      record[3] |= 0x00100000;
}

static void
encode_source_b(uint32_t record[4], const struct chrome9_ps_source *source)
{
   record[0] &= ~0x03ffffff;
   record[0] |= (uint32_t)source->modifier |
                (uint32_t)source->swizzle[0] << 13 |
                (uint32_t)source->swizzle[1] << 10 |
                (uint32_t)source->swizzle[2] << 7 |
                (uint32_t)source->swizzle[3] << 4 |
                (uint32_t)source->id << 16 |
                (uint32_t)source->file << 22 |
                (uint32_t)source->last_use << 25;
}

bool
chrome9_encode_ps_mov(const struct chrome9_ps_source *source,
                      uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x01c00530, 0x301c0053, 0x00f08005, 0x000d0820,
   };

   if (!record || !source_valid(source))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source);
   return true;
}

bool
chrome9_encode_ps_mul(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_c,
                      uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x0140b6d0, 0x30081053, 0x00f08005, 0x00000820,
   };

   if (!record || !source_valid(source_a) || !source_valid(source_c))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source_a);
   encode_source_c(record, source_c);
   return true;
}

bool
chrome9_encode_ps_add(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_b,
                      uint32_t record[4])
{
   /* The pixel ALU is a three-source A*C+B datapath.  ADD fixes C to the
    * native literal-one selector, leaving A and B as the semantic inputs. */
   static const uint32_t base[4] = {
      0x00810530, 0x30140924, 0x00f08005, 0x00000020,
   };

   if (!record || !source_valid(source_a) || !source_valid(source_b))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source_a);
   encode_source_b(record, source_b);
   return true;
}

bool
chrome9_encode_ps_sub(const struct chrome9_ps_source *minuend,
                      const struct chrome9_ps_source *subtrahend,
                      uint32_t record[4])
{
   /* B + A*(-1): source order retains the semantic minuend/subtrahend. */
   static const uint32_t base[4] = {
      0x80000530, 0x00140924, 0x00f00000, 0x00000020,
   };

   if (!record || !source_valid(minuend) || !source_valid(subtrahend))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, subtrahend);
   encode_source_b(record, minuend);
   return true;
}

bool
chrome9_encode_ps_mad(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_c,
                      const struct chrome9_ps_source *source_b,
                      uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x00000000, 0x00000000, 0x00f00000, 0x00000020,
   };

   if (!record || !source_valid(source_a) || !source_valid(source_c) ||
       !source_valid(source_b))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source_a);
   encode_source_c(record, source_c);
   encode_source_b(record, source_b);
   return true;
}

bool
chrome9_encode_ps_min(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_c,
                      uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x01c00530, 0x30080053, 0x00f08005, 0x000c0820,
   };

   if (!record || !source_valid(source_a) || !source_valid(source_c))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source_a);
   encode_source_c(record, source_c);
   return true;
}

static bool
encode_binary_subop(const struct chrome9_ps_source *source_a,
                    const struct chrome9_ps_source *source_c,
                    unsigned subop, bool dot3, uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x01c00530, 0x30080053, 0x00f08005, 0x00000820,
   };

   if (!record || !source_valid(source_a) || !source_valid(source_c) ||
       subop > 0xf)
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source_a);
   encode_source_c(record, source_c);
   record[3] |= subop << 16;
   if (dot3) {
      /* DP3 consumes XYZ and fixes W to zero, independent of the NIR
       * destination width. */
      record[1] &= ~0x7u;
      record[1] |= (uint32_t)CHROME9_PS_SWIZZLE_ZERO << 0;
      record[1] &= ~(7u << 28);
      record[1] |= (uint32_t)CHROME9_PS_SWIZZLE_ZERO << 28;
   }
   return true;
}

bool
chrome9_encode_ps_max(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_c,
                      uint32_t record[4])
{
   return encode_binary_subop(source_a, source_c, 0xb, false, record);
}

bool
chrome9_encode_ps_dp3(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_c,
                      uint32_t record[4])
{
   return encode_binary_subop(source_a, source_c, 0x2, true, record);
}

bool
chrome9_encode_ps_dp4(const struct chrome9_ps_source *source_a,
                      const struct chrome9_ps_source *source_c,
                      uint32_t record[4])
{
   return encode_binary_subop(source_a, source_c, 0x3, false, record);
}

static bool
encode_scalar_subop(const struct chrome9_ps_source *source, unsigned subop,
                    uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x01c00530, 0xb01c0053, 0x00f080b6, 0x00000820,
   };

   if (!record || !source_valid(source) || subop < 4 || subop > 7)
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source);
   record[3] |= subop << 16;
   return true;
}

bool
chrome9_encode_ps_exp2(const struct chrome9_ps_source *source,
                       uint32_t record[4])
{
   return encode_scalar_subop(source, 4, record);
}

bool
chrome9_encode_ps_log2(const struct chrome9_ps_source *source,
                       uint32_t record[4])
{
   return encode_scalar_subop(source, 5, record);
}

bool
chrome9_encode_ps_rcp(const struct chrome9_ps_source *source,
                      uint32_t record[4])
{
   return encode_scalar_subop(source, 6, record);
}

bool
chrome9_encode_ps_rsq(const struct chrome9_ps_source *source,
                      uint32_t record[4])
{
   return encode_scalar_subop(source, 7, record);
}

bool
chrome9_encode_ps_abs(const struct chrome9_ps_source *source,
                      uint32_t record[4])
{
   struct chrome9_ps_source negated;
   static const uint32_t base[4] = {
      0x00000000, 0x00000000, 0x00f00000, 0x00090820,
   };

   if (!record || !source_valid(source) || source->modifier)
      return false;
   negated = *source;
   negated.modifier = CHROME9_PS_SOURCE_NEGATE;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source);
   encode_source_c(record, source);
   encode_source_b(record, &negated);
   return true;
}

bool
chrome9_encode_ps_cmp(const struct chrome9_ps_source *condition,
                      const struct chrome9_ps_source *if_true,
                      const struct chrome9_ps_source *if_false,
                      uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x00000000, 0x00000000, 0x00f00000, 0x00090820,
   };

   if (!record || !source_valid(condition) || !source_valid(if_true) ||
       !source_valid(if_false))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, condition);
   encode_source_c(record, if_true);
   encode_source_b(record, if_false);
   return true;
}

bool
chrome9_encode_ps_frc(const struct chrome9_ps_source *source,
                      uint32_t record[4])
{
   static const uint32_t base[4] = {
      0x01c00530, 0x301c0053, 0x00f08005, 0x000e0820,
   };

   if (!record || !source_valid(source))
      return false;
   memcpy(record, base, sizeof(base));
   encode_source_a(record, source);
   return true;
}

static unsigned
instruction_source_count(enum chrome9_ps_opcode op)
{
   switch (op) {
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

static uint32_t
resource_bucket(unsigned direct_inputs, unsigned temporaries)
{
   if (direct_inputs <= 4 && temporaries <= 3)
      return 0x00018000;
   if (direct_inputs <= 5 && temporaries <= 4)
      return 0x00010000;
   if (direct_inputs <= 8 && temporaries <= 6)
      return 0x00008000;
   return 0;
}

bool
chrome9_ps_emit_integer_pow(struct chrome9_ps_ir *ir,
                            struct chrome9_ps_source base,
                            uint32_t exponent, uint8_t *next_temp,
                            uint8_t *alu_count,
                            struct chrome9_ps_source *result)
{
   struct chrome9_ps_source current = base;
   unsigned operations = 0;
   unsigned highest = 0;

   if (!ir || !next_temp || !alu_count || !result || !exponent)
      return false;

   /* The leading one is the base itself.  Each remaining bit squares the
    * current value and multiplies by the base when that bit is set. */
   for (uint32_t bits = exponent; bits > 1; bits >>= 1) {
      operations += 1 + (bits & 1);
      highest++;
   }
   if (ir->count > CHROME9_PS_MAX_INSTRUCTIONS ||
       *alu_count > CHROME9_PS_MAX_ALU ||
       *next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       *next_temp > CHROME9_PS_MAX_RESOURCE_TEMPS ||
       operations > CHROME9_PS_MAX_INSTRUCTIONS - ir->count ||
       operations > CHROME9_PS_MAX_ALU - *alu_count ||
       operations > CHROME9_PS_MAX_VIRTUAL_TEMPS - *next_temp ||
       operations > CHROME9_PS_MAX_RESOURCE_TEMPS - *next_temp)
      return false;

   for (int bit = (int)highest - 1; bit >= 0; bit--) {
      struct chrome9_ps_instruction *square =
         &ir->instruction[ir->count++];

      memset(square, 0, sizeof(*square));
      square->op = CHROME9_PS_OPCODE_MUL;
      square->destination.file = CHROME9_PS_DESTINATION_TEMP;
      square->destination.id = (*next_temp)++;
      square->destination.writemask = 1;
      square->source[0] = current;
      square->source[1] = current;
      current.file = CHROME9_PS_SOURCE_TEMP;
      current.id = square->destination.id;
      current.modifier = 0;
      current.last_use = false;
      current.swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         current.swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;

      if (exponent & (1u << bit)) {
         struct chrome9_ps_instruction *multiply =
            &ir->instruction[ir->count++];

         memset(multiply, 0, sizeof(*multiply));
         multiply->op = CHROME9_PS_OPCODE_MUL;
         multiply->destination.file = CHROME9_PS_DESTINATION_TEMP;
         multiply->destination.id = (*next_temp)++;
         multiply->destination.writemask = 1;
         multiply->source[0] = current;
         multiply->source[1] = base;
         current.id = multiply->destination.id;
      }
   }
   *alu_count += operations;
   *result = current;
   return true;
}

bool
chrome9_compile_ps_ir(const struct chrome9_ps_ir *ir,
                      struct chrome9_ps_program *p)
{
   const struct chrome9_ps_instruction *textures[CHROME9_PS_MAX_TAU];
   const struct chrome9_ps_instruction *alu[CHROME9_PS_MAX_ALU];
   uint16_t defined_temps = 0;
   uint8_t defined_components[12] = { 0 };
   uint8_t temp_uses[12] = { 0 };
   uint8_t remaining_uses[12];
   uint8_t texture_uses[8] = { 0 };
   uint8_t remaining_texture_uses[8];
   uint8_t color_writemask = 0;
   uint16_t direct_texture_mask = 0;
   uint8_t coordinate_map[8];
   uint8_t coordinate_uses[8] = { 0 };
   uint8_t coordinate_count = 0;
   unsigned texture_count = 0;
   unsigned alu_count = 0;
   bool saw_alu = false;

   if (!ir || !p || !ir->count ||
       ir->count > CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   memset(coordinate_map, 0xff, sizeof(coordinate_map));
   /* The offline native encoder covers the characterized TAU-first subset.
    * Keep the semantic IR ordered even when this optional encoder cannot
    * represent an ALU-generated coordinate; cmd22 consumes that full stream
    * directly and lets the kernel schedule the required bridge operations. */
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction = &ir->instruction[i];

      if (instruction->op != CHROME9_PS_OPCODE_TEX) {
         if (alu_count >= CHROME9_PS_MAX_ALU)
            return false;
         saw_alu = true;
         alu[alu_count++] = instruction;
         continue;
      }
      if (saw_alu || texture_count >= CHROME9_PS_MAX_TAU ||
          instruction->sampler >= 8 ||
          instruction->destination.file != CHROME9_PS_DESTINATION_TEMP ||
          instruction->destination.id >= 12 ||
          instruction->destination.writemask != 0xf ||
          instruction->destination.saturate ||
          instruction->source[0].file != CHROME9_PS_SOURCE_TEXTURE ||
          instruction->source[0].id >= 8 ||
          instruction->source[0].modifier ||
          instruction->source[0].last_use ||
          instruction->source[0].swizzle[0] > CHROME9_PS_SWIZZLE_W ||
          instruction->source[0].swizzle[1] > CHROME9_PS_SWIZZLE_W ||
          (defined_temps & (1u << instruction->destination.id)))
         return false;
      if (coordinate_map[instruction->source[0].id] == 0xff) {
         if (coordinate_count >= 8)
            return false;
         coordinate_map[instruction->source[0].id] = coordinate_count++;
      }
      coordinate_uses[coordinate_map[instruction->source[0].id]]++;
      defined_temps |= 1u << instruction->destination.id;
      defined_components[instruction->destination.id] = 0xf;
      textures[texture_count++] = instruction;
   }
   if (!alu_count)
      return false;
   /* Validate the semantic IR and calculate temporary lifetimes before
    * packing any hardware words.  Release bits are an encoder result, not a
    * property callers may guess. */
   for (unsigned i = 0; i < alu_count; i++) {
      const struct chrome9_ps_instruction *instruction = alu[i];
      unsigned source_count = instruction_source_count(instruction->op);

      if (!destination_valid(&instruction->destination) || !source_count)
         return false;
      /* The C constant port aliases A when all three ALU inputs name
       * different constant registers.  The NIR path materializes C first;
       * standalone offline callers must not encode a silent wrong result. */
      if (source_count == 3 &&
          instruction->source[0].file == CHROME9_PS_SOURCE_CONSTANT &&
          instruction->source[1].file == CHROME9_PS_SOURCE_CONSTANT &&
          instruction->source[2].file == CHROME9_PS_SOURCE_CONSTANT &&
          instruction->source[0].id != instruction->source[1].id &&
          instruction->source[0].id != instruction->source[2].id &&
          instruction->source[1].id != instruction->source[2].id)
         return false;
      for (unsigned s = 0; s < source_count; s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];
         bool reads_register = false;

         if (!source_valid(source) || source->last_use)
            return false;
         for (unsigned lane = 0; lane < 4; lane++) {
            unsigned component;

            if (!(instruction->destination.writemask & (1u << lane)))
               continue;
            component = source->swizzle[lane];
            if (component <= CHROME9_PS_SWIZZLE_W) {
               reads_register = true;
               if (source->file == CHROME9_PS_SOURCE_TEMP &&
                   !(defined_components[source->id] & (1u << component)))
                  return false;
            }
         }
         if (source->file == CHROME9_PS_SOURCE_TEMP && reads_register)
            temp_uses[source->id]++;
         else if (source->file == CHROME9_PS_SOURCE_TEXTURE &&
                  reads_register)
            texture_uses[source->id]++;
      }
      if (instruction->destination.file == CHROME9_PS_DESTINATION_TEMP) {
         if (defined_components[instruction->destination.id] &
             instruction->destination.writemask)
            return false;
         defined_temps |= 1u << instruction->destination.id;
         defined_components[instruction->destination.id] |=
            instruction->destination.writemask;
      } else {
         if (color_writemask & instruction->destination.writemask)
            return false;
         color_writemask |= instruction->destination.writemask;
      }
   }
   if (color_writemask != 0xf)
      return false;
   for (unsigned temp = 0; temp < 12; temp++) {
      if ((defined_temps & (1u << temp)) && !temp_uses[temp])
         return false;
   }

   memset(p, 0, sizeof(*p));
   memcpy(remaining_uses, temp_uses, sizeof(remaining_uses));
   memcpy(remaining_texture_uses, texture_uses,
          sizeof(remaining_texture_uses));
   for (unsigned i = 0; i < alu_count; i++) {
      const struct chrome9_ps_instruction *instruction = alu[i];
      struct chrome9_ps_source sources[3] = { 0 };
      uint8_t instruction_uses[12] = { 0 };
      unsigned source_count = instruction_source_count(instruction->op);
      uint8_t scalar_component = 0;

      for (unsigned s = 0; s < source_count; s++)
         sources[s] = instruction->source[s];
      if (instruction->op == CHROME9_PS_OPCODE_EXP2 ||
          instruction->op == CHROME9_PS_OPCODE_LOG2 ||
          instruction->op == CHROME9_PS_OPCODE_RCP ||
          instruction->op == CHROME9_PS_OPCODE_RSQ) {
         unsigned lane = __builtin_ctz(instruction->destination.writemask);

         if (instruction->destination.writemask &
             (instruction->destination.writemask - 1))
            return false;
         scalar_component = sources[0].swizzle[lane];
      }
      for (unsigned s = 0; s < source_count; s++) {
         bool reads_register = false;

         for (unsigned lane = 0; lane < 4; lane++) {
            if ((instruction->destination.writemask & (1u << lane)) &&
                sources[s].swizzle[lane] <= CHROME9_PS_SWIZZLE_W)
               reads_register = true;
         }
         if (sources[s].file == CHROME9_PS_SOURCE_TEMP && reads_register)
            instruction_uses[sources[s].id]++;
         else if (sources[s].file == CHROME9_PS_SOURCE_TEXTURE &&
                  reads_register) {
            unsigned texture = sources[s].id;

            remaining_texture_uses[texture]--;
            if (!remaining_texture_uses[texture])
               sources[s].last_use = true;
         }
      }
      for (unsigned s = 0; s < source_count; s++) {
         unsigned temp = sources[s].id;

         if (sources[s].file == CHROME9_PS_SOURCE_TEMP &&
             remaining_uses[temp] == instruction_uses[temp]) {
            /* If A and C consume the same final value, the vendor compiler
             * marks A only. */
            if (s == 0 || sources[0].file != CHROME9_PS_SOURCE_TEMP ||
                sources[0].id != temp)
               sources[s].last_use = true;
         }
      }
      for (unsigned temp = 0; temp < 12; temp++)
         remaining_uses[temp] -= instruction_uses[temp];

      /* Vendor vector records use literal zero selectors for lanes excluded
       * by the destination mask.  Scalar transcendental inputs instead live
       * in native A.W, with A.XYZ fixed to literal zero. */
      for (unsigned s = 0; s < source_count; s++)
         for (unsigned lane = 0; lane < 4; lane++)
            if (!(instruction->destination.writemask & (1u << lane)))
               sources[s].swizzle[lane] = CHROME9_PS_SWIZZLE_ZERO;
      if (instruction->op == CHROME9_PS_OPCODE_EXP2 ||
          instruction->op == CHROME9_PS_OPCODE_LOG2 ||
          instruction->op == CHROME9_PS_OPCODE_RCP ||
          instruction->op == CHROME9_PS_OPCODE_RSQ) {
         sources[0].swizzle[0] = CHROME9_PS_SWIZZLE_ZERO;
         sources[0].swizzle[1] = CHROME9_PS_SWIZZLE_ZERO;
         sources[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         sources[0].swizzle[3] = scalar_component;
      }

      bool encoded;

      switch (instruction->op) {
      case CHROME9_PS_OPCODE_MOV:
         encoded = chrome9_encode_ps_mov(&sources[0], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_MUL:
         encoded = chrome9_encode_ps_mul(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_ADD:
         encoded = chrome9_encode_ps_add(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_SUB:
         encoded = chrome9_encode_ps_sub(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_MAD:
         encoded = chrome9_encode_ps_mad(&sources[0], &sources[1],
                                         &sources[2], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_MIN:
         encoded = chrome9_encode_ps_min(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_MAX:
         encoded = chrome9_encode_ps_max(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_DP3:
         encoded = chrome9_encode_ps_dp3(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_DP4:
         encoded = chrome9_encode_ps_dp4(&sources[0], &sources[1],
                                         p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_EXP2:
         encoded = chrome9_encode_ps_exp2(&sources[0], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_LOG2:
         encoded = chrome9_encode_ps_log2(&sources[0], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_RCP:
         encoded = chrome9_encode_ps_rcp(&sources[0], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_RSQ:
         encoded = chrome9_encode_ps_rsq(&sources[0], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_ABS:
         encoded = chrome9_encode_ps_abs(&sources[0], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_CMP:
         encoded = chrome9_encode_ps_cmp(&sources[0], &sources[1],
                                         &sources[2], p->alu[i]);
         break;
      case CHROME9_PS_OPCODE_FRC:
         encoded = chrome9_encode_ps_frc(&sources[0], p->alu[i]);
         break;
      default:
         encoded = false;
         break;
      }
      if (!encoded)
         return false;
      encode_destination(p->alu[i], &instruction->destination);
      for (unsigned s = 0; s < source_count; s++) {
         if (sources[s].file == CHROME9_PS_SOURCE_INPUT)
            p->varying_mask |= 1u << sources[s].id;
         else if (sources[s].file == CHROME9_PS_SOURCE_CONSTANT)
            p->constant_mask |= 1u << sources[s].id;
         else if (sources[s].file == CHROME9_PS_SOURCE_TEXTURE) {
            p->texture_input_mask |= 1u << sources[s].id;
            direct_texture_mask |= 1u << sources[s].id;
         }
      }
   }
   p->alu_count = alu_count;
   if (texture_count) {
      unsigned temp_count = __builtin_popcount(defined_temps);
      unsigned direct_input_count = direct_texture_mask ?
         32 - __builtin_clz(direct_texture_mask) : 0;
      uint32_t bucket;
      uint32_t t2a[2] = { 0 };

      if (defined_temps != ((1u << temp_count) - 1) ||
          direct_input_count > 8)
         return false;
      bucket = resource_bucket(direct_input_count, temp_count);
      for (unsigned i = 0; i < texture_count; i++) {
         const struct chrome9_ps_instruction *texture = textures[i];
         const struct chrome9_ps_source *coordinate_source =
            &texture->source[0];
         unsigned coordinate = coordinate_map[coordinate_source->id];
         uint32_t tau = 0x20000000 |
            (uint32_t)texture->sampler << 24 |
            1u << 20 |
            (uint32_t)texture->destination.id << 16 |
            (uint32_t)coordinate << 12 |
            (uint32_t)coordinate_source->swizzle[0] << 10 |
            (uint32_t)coordinate_source->swizzle[1] << 8 |
            (uint32_t)coordinate_source->swizzle[2] << 6 |
            (uint32_t)coordinate_source->swizzle[3] << 4 |
            texture->sampler;

         if (!--coordinate_uses[coordinate])
            tau |= 1u << 28;
         p->tau[i] = tau;
         p->texture_input_mask |= 1u << coordinate_source->id;
         p->sampler_mask |= 1u << texture->sampler;
      }
      p->tau_count = texture_count;
      t2a[(texture_count - 1) / 24] |=
         1u << ((texture_count - 1) % 24);
      set_controls(p, 0x90000000 | bucket | direct_texture_mask,
                   0x91000000 | ((1u << coordinate_count) - 1),
                   0x92000000 | (uint32_t)texture_count << 16 |
                      alu_count,
                   0x93000000 | t2a[0], 0x95000000);
      p->control[4] |= t2a[1];
   } else {
      unsigned temp_count = __builtin_popcount(defined_temps);
      unsigned direct_input_count = direct_texture_mask ?
         32 - __builtin_clz(direct_texture_mask) : 0;
      uint32_t bucket;

      if (defined_temps != ((1u << temp_count) - 1) ||
          direct_input_count > 8)
         return false;
      bucket = resource_bucket(direct_input_count, temp_count);
      set_controls(p, 0x90040000 | bucket | direct_texture_mask,
                   0x91000000, 0x92000000 | alu_count,
                   0x93000000, 0x95000000);
   }
   return true;
}

static struct chrome9_ps_source
source(enum chrome9_ps_source_file file, unsigned id, bool alpha,
       bool last_use)
{
   struct chrome9_ps_source result = {
      .file = file,
      .id = id,
      .swizzle = {
         alpha ? CHROME9_PS_SWIZZLE_W : CHROME9_PS_SWIZZLE_X,
         alpha ? CHROME9_PS_SWIZZLE_W : CHROME9_PS_SWIZZLE_Y,
         alpha ? CHROME9_PS_SWIZZLE_W : CHROME9_PS_SWIZZLE_Z,
         CHROME9_PS_SWIZZLE_W,
      },
      .last_use = last_use,
   };

   return result;
}

static bool
is_pair(const struct chrome9_fs_key *key, enum chrome9_fs_leaf a,
        enum chrome9_fs_leaf b)
{
   return key->op == CHROME9_FS_OP_MUL &&
          ((key->src[0] == a && key->src[1] == b) ||
           (key->src[0] == b && key->src[1] == a));
}

static void
set_controls(struct chrome9_ps_program *p, uint32_t a90, uint32_t a91,
             uint32_t a92, uint32_t a93, uint32_t a95)
{
   static const uint32_t base[12] = {
      0x90000000, 0x91000000, 0x92000000, 0x93000000,
      0x94000000, 0x95000000, 0x96000000, 0x97000000,
      0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
   };

   memcpy(p->control, base, sizeof(base));
   p->control[0] = a90;
   p->control[1] = a91;
   p->control[2] = a92;
   p->control[3] = a93;
   p->control[5] = a95;
}

static bool
compile_alu_only(const struct chrome9_fs_key *key,
                 struct chrome9_ps_program *p)
{
   const struct chrome9_ps_source v0 =
      source(CHROME9_PS_SOURCE_INPUT, 0, false, false);
   const struct chrome9_ps_source v0a =
      source(CHROME9_PS_SOURCE_INPUT, 0, true, false);
   const struct chrome9_ps_source v1 =
      source(CHROME9_PS_SOURCE_INPUT, 1, false, false);
   const struct chrome9_ps_source v1a =
      source(CHROME9_PS_SOURCE_INPUT, 1, true, false);
   const struct chrome9_ps_source c0 =
      source(CHROME9_PS_SOURCE_CONSTANT, 0, false, false);
   const struct chrome9_ps_source c0a =
      source(CHROME9_PS_SOURCE_CONSTANT, 0, true, false);
   const struct chrome9_ps_source c1 =
      source(CHROME9_PS_SOURCE_CONSTANT, 1, false, false);
   const struct chrome9_ps_source c1a =
      source(CHROME9_PS_SOURCE_CONSTANT, 1, true, false);

   if (key->op == CHROME9_FS_OP_MOV && key->src[0] == CHROME9_FS_LEAF_V0) {
      if (!chrome9_encode_ps_mov(&v0, p->alu[0]))
         return false;
      p->varying_mask = 1;
   } else if (key->op == CHROME9_FS_OP_MOV &&
              key->src[0] == CHROME9_FS_LEAF_C0) {
      if (!chrome9_encode_ps_mov(&c0, p->alu[0]))
         return false;
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_V0, CHROME9_FS_LEAF_C0)) {
      if (!chrome9_encode_ps_mul(&v0, &c0, p->alu[0]))
         return false;
      p->varying_mask = 1;
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_C1)) {
      if (!chrome9_encode_ps_mul(&c0, &c1, p->alu[0]))
         return false;
      p->constant_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_V0, CHROME9_FS_LEAF_V1_A)) {
      if (!chrome9_encode_ps_mul(&v0, &v1a, p->alu[0]))
         return false;
      p->varying_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_V0_A, CHROME9_FS_LEAF_V1)) {
      if (!chrome9_encode_ps_mul(&v0a, &v1, p->alu[0]))
         return false;
      p->varying_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_C1_A)) {
      if (!chrome9_encode_ps_mul(&c0, &c1a, p->alu[0]))
         return false;
      p->constant_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_C0_A, CHROME9_FS_LEAF_C1)) {
      if (!chrome9_encode_ps_mul(&c0a, &c1, p->alu[0]))
         return false;
      p->constant_mask = 3;
   } else {
      return false;
   }

   p->alu_count = 1;
   set_controls(p, 0x90058000, 0x91000000, 0x92000001,
                0x93000000, 0x95000000);
   return true;
}

static bool
compile_one_texture(const struct chrome9_fs_key *key,
                    struct chrome9_ps_program *p)
{
   const struct chrome9_ps_source texture =
      source(CHROME9_PS_SOURCE_TEMP, 0, false, true);
   const struct chrome9_ps_source texture_alpha =
      source(CHROME9_PS_SOURCE_TEMP, 0, true, true);
   const struct chrome9_ps_source v0 =
      source(CHROME9_PS_SOURCE_INPUT, 0, false, false);
   const struct chrome9_ps_source c0 =
      source(CHROME9_PS_SOURCE_CONSTANT, 0, false, false);
   const struct chrome9_ps_source c0a =
      source(CHROME9_PS_SOURCE_CONSTANT, 0, true, false);

   if (key->op == CHROME9_FS_OP_MOV &&
       key->src[0] == CHROME9_FS_LEAF_TEX0) {
      if (!chrome9_encode_ps_mov(&texture, p->alu[0]))
         return false;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_V0)) {
      if (!chrome9_encode_ps_mul(&texture, &v0, p->alu[0]))
         return false;
      p->varying_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0)) {
      if (!chrome9_encode_ps_mul(&texture, &c0, p->alu[0]))
         return false;
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0_A)) {
      if (!chrome9_encode_ps_mul(&c0a, &texture, p->alu[0]))
         return false;
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0_A, CHROME9_FS_LEAF_C0)) {
      if (!chrome9_encode_ps_mul(&texture_alpha, &c0, p->alu[0]))
         return false;
      p->constant_mask = 1;
   } else {
      return false;
   }

   p->tau[0] = 0x301001b0;
   p->alu_count = 1;
   p->tau_count = 1;
   p->texture_input_mask = 1;
   p->sampler_mask = 1;
   set_controls(p, 0x90018000, 0x91000001, 0x92010001,
                0x93000001, 0x95000000);
   return true;
}

bool
chrome9_compile_fs_offline(const struct chrome9_fs_key *key,
                           struct chrome9_ps_program *p)
{
   if (!key || !p)
      return false;
   memset(p, 0, sizeof(*p));

   if (key->op == CHROME9_FS_OP_RGBX_C0X &&
       key->src[0] == CHROME9_FS_LEAF_TEX0 &&
       key->src[1] == CHROME9_FS_LEAF_C0) {
      const uint32_t rgb[4] = { 0x01c00530, 0x501c0053,
                                0x00720005, 0x000d0820 };
      const uint32_t alpha[4] = { 0x01c00530, 0x801c0053,
                                  0x008080b6, 0x000d0820 };
      memcpy(p->alu[0], rgb, sizeof(rgb));
      memcpy(p->alu[1], alpha, sizeof(alpha));
      p->tau[0] = 0x301001b0;
      p->alu_count = 2;
      p->tau_count = 1;
      p->constant_mask = 1;
      p->constant_x_one_mask = 1;
      p->texture_input_mask = 1;
      p->sampler_mask = 1;
      set_controls(p, 0x90018000, 0x91000001, 0x92010002,
                   0x93000001, 0x95000000);
      return true;
   }

   if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_TEX1)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x3003f000,
                                0x00f20005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->tau[0] = 0xa01001b0;
      p->tau[1] = 0x311001b1;
      p->alu_count = 1;
      p->tau_count = 2;
      p->texture_input_mask = 3;
      p->sampler_mask = 3;
      set_controls(p, 0x90000000, 0x91000001, 0x92020001,
                   0x93000002, 0x95000000);
      return true;
   }
   if (key->src[0] == CHROME9_FS_LEAF_TEX0 ||
       key->src[1] == CHROME9_FS_LEAF_TEX0 ||
       key->src[0] == CHROME9_FS_LEAF_TEX0_A ||
       key->src[1] == CHROME9_FS_LEAF_TEX0_A)
      return compile_one_texture(key, p);
   return compile_alu_only(key, p);
}

static const uint32_t vs_tail[5][4] = {
   { 0x00201b6c, 0x00000000, 0x00000000, 0x00280108 },
   { 0x00200150, 0x00101b6c, 0x00201b6c, 0x001c0107 },
   { 0x0010014c, 0x00380150, 0x00384154, 0x0004021f },
   { 0x0020014c, 0x00000000, 0x00000000, 0x0008020f },
   { 0x0020014c, 0x00000000, 0x00000000, 0x0008020f },
};

static void
append_vs(struct chrome9_vs_program *p, const uint32_t instruction[4])
{
   memcpy(p->instruction[p->upload_count++], instruction,
          sizeof(p->instruction[0]));
}

bool
chrome9_compile_vs_offline(enum chrome9_vs_key key,
                           struct chrome9_vs_program *p)
{
   static const uint32_t pos[4] = {
      0x0008014c, 0x00000000, 0x00000000, 0x00080a0f
   };
   static const uint32_t pos_tmp[4] = {
      0x0010014c, 0x00000000, 0x00000000, 0x00080a0f
   };
   static const uint32_t color[4] = {
      0x0008414c, 0x00000000, 0x00000000, 0x0008422f
   };
   static const uint32_t tex0[4] = {
      0x0008414c, 0x00000000, 0x00000000, 0x00080283
   };
   static const uint32_t tex1[4] = {
      0x0008814c, 0x00000000, 0x00000000, 0x00080293
   };
   static const uint32_t tex0_v2[4] = {
      0x0008814c, 0x00000000, 0x00000000, 0x00080283
   };
   static const uint32_t tex1_v3[4] = {
      0x0008c14c, 0x00000000, 0x00000000, 0x00080293
   };
   static const uint32_t mvp[4][4] = {
      { 0x0008014c, 0x0040014c, 0x00000000, 0x00060801 },
      { 0x0008014c, 0x0040414c, 0x00000000, 0x00060802 },
      { 0x0008014c, 0x0040814c, 0x00000000, 0x00060804 },
      { 0x0008014c, 0x0040c14c, 0x0000014c, 0x0034010f },
   };
   unsigned i;

   if (!p || (unsigned)key > CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY)
      return false;
   memset(p, 0, sizeof(*p));

   if (key >= CHROME9_VS_MVP_POSITION) {
      for (i = 0; i < 4; i++)
         append_vs(p, mvp[i]);
      append_vs(p, pos_tmp);
      p->constant_mask[0] = 0x0000000f;
      p->input_mask = 0x0001;
      p->output_metadata = 0x00000004;
      p->a62_low = 3;
      if (key == CHROME9_VS_MVP_POSITION_TEX0_XY) {
         append_vs(p, tex0);
         p->output_metadata = 0x00000104;
         p->input_mask |= 0x0002;
         p->a62_low = 5;
      } else if (key == CHROME9_VS_MVP_POSITION_TEX0_TEX1_XY) {
         append_vs(p, tex0);
         append_vs(p, tex1);
         p->output_metadata = 0x00000204;
         p->input_mask |= 0x0006;
         p->a62_low = 6;
      } else if (key == CHROME9_VS_MVP_COLOR_TEX0_XY) {
         append_vs(p, color);
         append_vs(p, tex0_v2);
         p->output_metadata = 0x00000144;
         p->input_mask |= 0x0006;
         p->a62_low = 6;
      } else if (key == CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY) {
         append_vs(p, color);
         append_vs(p, tex0_v2);
         append_vs(p, tex1_v3);
         p->output_metadata = 0x00000244;
         p->input_mask |= 0x000e;
         p->a62_low = 7;
      }
   } else {
      append_vs(p, pos);
      p->output_metadata = 0x00000004;
      p->input_mask = 0x0001;
      if (key >= CHROME9_VS_POSITION_TEX0_XY) {
         append_vs(p, tex0);
         p->output_metadata = 0x00000104;
         p->input_mask |= 0x0002;
         p->a62_low = 1;
      }
      if (key >= CHROME9_VS_POSITION_TEX0_TEX1_XY) {
         append_vs(p, tex1);
         p->output_metadata = 0x00000204;
         p->input_mask |= 0x0004;
         p->a62_low = 2;
      }
   }

   p->active_count = p->upload_count + 3;
   for (i = 0; i < 5; i++)
      append_vs(p, vs_tail[i]);
   switch (p->output_metadata) {
   case 0x00000004: p->a25 = 0x25f00000; break;
   case 0x00000104: p->a25 = 0x25f0c000; break;
   case 0x00000204: p->a25 = 0x25f0cc00; break;
   case 0x00000144: p->a25 = 0x25f4c000; break;
   case 0x00000244: p->a25 = 0x25f4cc00; break;
   default: return false;
   }
   p->a26 = 0x26000000;
   p->a60 = 0x60800000 | (p->active_count - 1);
   return p->active_count <= p->upload_count &&
          p->upload_count <= CHROME9_VS_POLICY_MAX;
}

static bool
verify_fail(enum chrome9_shader_verify_error *error,
            enum chrome9_shader_verify_error value)
{
   if (error)
      *error = value;
   return false;
}

bool
chrome9_verify_fs_offline(const struct chrome9_fs_key *key,
                          const struct chrome9_ps_program *p,
                          enum chrome9_shader_verify_error *error)
{
   struct chrome9_ps_program expected;
   size_t alu_bytes, tau_bytes;

   if (error)
      *error = CHROME9_SHADER_VERIFY_OK;
   if (!key || !p)
      return verify_fail(error, CHROME9_SHADER_VERIFY_NULL);
   if (!chrome9_compile_fs_offline(key, &expected))
      return verify_fail(error, CHROME9_SHADER_VERIFY_KEY);
   if (p->alu_count != expected.alu_count ||
       p->tau_count != expected.tau_count ||
       p->alu_count > CHROME9_PS_MAX_ALU ||
       p->tau_count > CHROME9_PS_MAX_TAU)
      return verify_fail(error, CHROME9_SHADER_VERIFY_COUNT);
   if (p->constant_mask != expected.constant_mask ||
       p->constant_x_one_mask != expected.constant_x_one_mask ||
       p->varying_mask != expected.varying_mask ||
       p->texture_input_mask != expected.texture_input_mask ||
       p->sampler_mask != expected.sampler_mask)
      return verify_fail(error, CHROME9_SHADER_VERIFY_RESOURCE);
   if (memcmp(p->control, expected.control, sizeof(p->control)))
      return verify_fail(error, CHROME9_SHADER_VERIFY_CONTROL);

   alu_bytes = (size_t)p->alu_count * sizeof(p->alu[0]);
   tau_bytes = (size_t)p->tau_count * sizeof(p->tau[0]);
   if (memcmp(p->alu, expected.alu, alu_bytes) ||
       memcmp(p->tau, expected.tau, tau_bytes))
      return verify_fail(error, CHROME9_SHADER_VERIFY_INSTRUCTION);
   if (memcmp(p, &expected, sizeof(*p)))
      return verify_fail(error, CHROME9_SHADER_VERIFY_SLACK);
   return true;
}

bool
chrome9_verify_vs_offline(enum chrome9_vs_key key,
                          const struct chrome9_vs_program *p,
                          enum chrome9_shader_verify_error *error)
{
   struct chrome9_vs_program expected;
   size_t instruction_bytes;

   if (error)
      *error = CHROME9_SHADER_VERIFY_OK;
   if (!p)
      return verify_fail(error, CHROME9_SHADER_VERIFY_NULL);
   if (!chrome9_compile_vs_offline(key, &expected))
      return verify_fail(error, CHROME9_SHADER_VERIFY_KEY);
   if (p->active_count != expected.active_count ||
       p->upload_count != expected.upload_count ||
       !p->active_count || p->active_count > p->upload_count ||
       p->upload_count > CHROME9_VS_POLICY_MAX)
      return verify_fail(error, CHROME9_SHADER_VERIFY_COUNT);
   if (p->output_metadata != expected.output_metadata ||
       p->constant_mask[0] != expected.constant_mask[0] ||
       memcmp(&p->constant_mask[1], &expected.constant_mask[1],
              sizeof(p->constant_mask) - sizeof(p->constant_mask[0])) ||
       p->input_mask != expected.input_mask)
      return verify_fail(error, CHROME9_SHADER_VERIFY_RESOURCE);
   if (p->a25 != expected.a25 || p->a26 != expected.a26 ||
       p->a60 != expected.a60 || p->a62_low != expected.a62_low ||
       (p->a60 & 0x1ff) != (uint32_t)p->active_count - 1u)
      return verify_fail(error, CHROME9_SHADER_VERIFY_CONTROL);

   instruction_bytes = (size_t)p->upload_count * sizeof(p->instruction[0]);
   if (memcmp(p->instruction, expected.instruction, instruction_bytes))
      return verify_fail(error, CHROME9_SHADER_VERIFY_INSTRUCTION);
   if (memcmp(p, &expected, sizeof(*p)))
      return verify_fail(error, CHROME9_SHADER_VERIFY_SLACK);
   return true;
}
