/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_fragment.h"

#include <string.h>


#define CHECK(condition) do { if (!(condition)) return false; } while (0)

static struct chrome9_ps_source
source(enum chrome9_ps_source_file file, unsigned id,
       enum chrome9_ps_swizzle x, enum chrome9_ps_swizzle y,
       enum chrome9_ps_swizzle z, enum chrome9_ps_swizzle w)
{
   return (struct chrome9_ps_source) {
      .file = file,
      .id = id,
      .swizzle = { x, y, z, w },
   };
}

static struct chrome9_ps_destination
temporary(unsigned id)
{
   return (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP,
      .id = id,
      .writemask = 0xf,
   };
}

static struct chrome9_ps_destination
color(void)
{
   return (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_COLOR,
      .writemask = 0xf,
   };
}

static bool
check_direct_texture_order(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 2 };
   unsigned count = 0;

   ir.instruction[0].op = CHROME9_PS_OPCODE_TEX;
   ir.instruction[0].sampler = 2;
   ir.instruction[0].destination = temporary(4);
   ir.instruction[0].source[0] = source(
      CHROME9_PS_SOURCE_TEXTURE, 3, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_W,
      CHROME9_PS_SWIZZLE_W);
   ir.instruction[1].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[1].destination = color();
   ir.instruction[1].source[0] = source(
      CHROME9_PS_SOURCE_TEMP, 4, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);

   CHECK(chrome9_fragment_build_instructions(&ir, 0x5, native, &count));
   CHECK(count == 2);
   CHECK(native[0].op == DRM_CHROME9_FRAGMENT_OP_TEX);
   CHECK(native[0].sampler == 2);
   CHECK(native[0].src[0].file == DRM_CHROME9_FRAGMENT_FILE_VARYING);
   CHECK(native[0].src[0].index == 3);
   CHECK(native[0].src[0].swizzle[0] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_X);
   CHECK(native[0].src[0].swizzle[1] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_Y);
   CHECK(native[1].op == DRM_CHROME9_FRAGMENT_OP_MOV);
   CHECK(native[1].src[0].file == DRM_CHROME9_FRAGMENT_FILE_TEMP);
   CHECK(native[1].src[0].index == 4);
   return true;
}

static bool
check_sparse_sampler_ids(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 5 };
   const unsigned logical[] = { 0, 1, 2, 4, 5 };
   unsigned count = 0;

   CHECK(chrome9_fragment_sampler_high_water(0) == 0);
   CHECK(chrome9_fragment_sampler_high_water(0x37) == 6);
   CHECK(chrome9_fragment_sampler_high_water(0x80) == 8);
   CHECK(chrome9_fragment_sampler_slots_dense(0));
   CHECK(chrome9_fragment_sampler_slots_dense(0x3f));
   CHECK(chrome9_fragment_sampler_slots_dense(0xff));
   CHECK(!chrome9_fragment_sampler_slots_dense(0x37));
   CHECK(!chrome9_fragment_sampler_slots_dense(0x80));
   for (unsigned i = 0; i < 5; i++)
      CHECK(chrome9_fragment_descriptor_unit(0x37, i) == logical[i]);
   CHECK(chrome9_fragment_descriptor_unit(0x20, 0) == 5);
   CHECK(chrome9_fragment_descriptor_unit(0x07, 2) == 2);
   CHECK(chrome9_fragment_descriptor_unit(0x37, 5) ==
         DRM_CHROME9_FRAGMENT_MAX_TEXTURES);
   CHECK(chrome9_fragment_descriptor_unit(0, 0) ==
         DRM_CHROME9_FRAGMENT_MAX_TEXTURES);
   for (unsigned i = 0; i < 5; i++) {
      ir.instruction[i].op = CHROME9_PS_OPCODE_TEX;
      ir.instruction[i].sampler = logical[i];
      ir.instruction[i].destination = temporary(i);
      ir.instruction[i].source[0] = source(
         CHROME9_PS_SOURCE_TEXTURE, 0, CHROME9_PS_SWIZZLE_X,
         CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
         CHROME9_PS_SWIZZLE_W);
   }
   CHECK(chrome9_fragment_build_instructions(&ir, 0x37, native, &count));
   CHECK(count == 5);
   for (unsigned i = 0; i < 5; i++)
      CHECK(native[i].sampler == logical[i]);
   CHECK(!chrome9_fragment_build_instructions(&ir, 0x17, native, &count));
   return true;
}

static bool
check_computed_texture_order(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 3 };
   unsigned count = 0;

   ir.instruction[0].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[0].destination = temporary(0);
   ir.instruction[0].source[0] = source(
      CHROME9_PS_SOURCE_TEXTURE, 0, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);
   ir.instruction[1].op = CHROME9_PS_OPCODE_TEX;
   ir.instruction[1].sampler = 0;
   ir.instruction[1].destination = temporary(1);
   ir.instruction[1].source[0] = source(
      CHROME9_PS_SOURCE_TEMP, 0, CHROME9_PS_SWIZZLE_Y,
      CHROME9_PS_SWIZZLE_X, CHROME9_PS_SWIZZLE_W,
      CHROME9_PS_SWIZZLE_W);
   ir.instruction[2].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[2].destination = color();
   ir.instruction[2].source[0] = source(
      CHROME9_PS_SOURCE_TEMP, 1, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);
   ir.instruction[2].source[1] = source(
      CHROME9_PS_SOURCE_CONSTANT, 0, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);

   CHECK(chrome9_fragment_build_instructions(&ir, 1, native, &count));
   CHECK(count == 3);
   CHECK(native[0].op == DRM_CHROME9_FRAGMENT_OP_MOV);
   CHECK(native[1].op == DRM_CHROME9_FRAGMENT_OP_TEX);
   CHECK(native[2].op == DRM_CHROME9_FRAGMENT_OP_MUL);
   CHECK(native[1].src[0].file == DRM_CHROME9_FRAGMENT_FILE_TEMP);
   CHECK(native[1].src[0].index == 0);
   CHECK(native[1].src[0].swizzle[0] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_Y);
   CHECK(native[1].src[0].swizzle[1] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_X);
   return true;
}

static bool
check_kill_conversion(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 3 };
   unsigned count = 0;

   ir.instruction[0].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[0].destination = temporary(0);
   ir.instruction[0].source[0] = source(
      CHROME9_PS_SOURCE_CONSTANT, 0, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);
   ir.instruction[1].op = CHROME9_PS_OPCODE_KILL;
   ir.instruction[1].source[0] = source(
      CHROME9_PS_SOURCE_TEMP, 0, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);
   ir.instruction[2].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[2].destination = color();
   ir.instruction[2].source[0] = source(
      CHROME9_PS_SOURCE_CONSTANT, 1, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);

   CHECK(chrome9_fragment_build_instructions(&ir, 0, native, &count));
   CHECK(count == 3);
   CHECK(native[1].op == DRM_CHROME9_FRAGMENT_OP_KILL);
   CHECK(!native[1].dst.file && !native[1].dst.writemask &&
         !native[1].sampler && native[1].src[0].file ==
            DRM_CHROME9_FRAGMENT_FILE_TEMP);
   ir.instruction[1].destination = color();
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   memset(&ir.instruction[1].destination, 0,
          sizeof(ir.instruction[1].destination));
   ir.instruction[1].source[0].swizzle[3] = CHROME9_PS_SWIZZLE_X;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   ir.instruction[1].source[0].swizzle[3] = CHROME9_PS_SWIZZLE_W;
   ir.instruction[1].source[0].modifier = CHROME9_PS_SOURCE_NEGATE;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   ir.instruction[1].source[0].modifier = 0;
   ir.instruction[1].sampler = 1;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   ir.instruction[1].sampler = 0;
   ir.instruction[1].source[1].id = 1;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   return true;
}

static bool
check_depth_view_swizzles(void)
{
   struct drm_chrome9_fragment_instruction instructions[2] = {
      {
         .op = DRM_CHROME9_FRAGMENT_OP_TEX,
         .sampler = 0,
         .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 3, 0xf, 0 },
         .src = {
            { DRM_CHROME9_FRAGMENT_FILE_VARYING, 0, { 0, 1, 2, 3 }, 0, 0 },
         },
      }, {
         .op = DRM_CHROME9_FRAGMENT_OP_MOV,
         .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
         .src = {
            { DRM_CHROME9_FRAGMENT_FILE_TEMP, 3, { 0, 1, 2, 3 }, 0, 0 },
         },
      },
   };
   uint8_t view[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4] = { 0 };

   view[0][0] = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
   view[0][1] = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
   view[0][2] = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
   view[0][3] = DRM_CHROME9_FRAGMENT_SWIZZLE_ONE;
   CHECK(chrome9_fragment_apply_depth_swizzles(instructions, 2, 1, view));
   CHECK(instructions[1].src[0].swizzle[0] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_X);
   CHECK(instructions[1].src[0].swizzle[1] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_X);
   CHECK(instructions[1].src[0].swizzle[2] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_X);
   CHECK(instructions[1].src[0].swizzle[3] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_ONE);

   instructions[1].src[0].swizzle[0] = 0;
   instructions[1].src[0].swizzle[1] = 1;
   instructions[1].src[0].swizzle[2] = 2;
   instructions[1].src[0].swizzle[3] = 3;
   view[0][1] = DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO;
   view[0][2] = DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO;
   view[0][3] = DRM_CHROME9_FRAGMENT_SWIZZLE_ONE;
   CHECK(chrome9_fragment_apply_depth_swizzles(instructions, 2, 1, view));
   CHECK(instructions[1].src[0].swizzle[0] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_X);
   CHECK(instructions[1].src[0].swizzle[1] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO);
   CHECK(instructions[1].src[0].swizzle[2] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO);
   CHECK(instructions[1].src[0].swizzle[3] ==
         DRM_CHROME9_FRAGMENT_SWIZZLE_ONE);

   instructions[0].dst.file = DRM_CHROME9_FRAGMENT_DEST_COLOR;
   CHECK(!chrome9_fragment_apply_depth_swizzles(instructions, 1, 1, view));
   return true;
}

static bool
check_depth_kill_swizzles(void)
{
   struct drm_chrome9_fragment_instruction instructions[4] = {
      {
         .op = DRM_CHROME9_FRAGMENT_OP_TEX,
         .sampler = 0,
         .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 3, 0xf, 0 },
         .src = {
            { DRM_CHROME9_FRAGMENT_FILE_VARYING, 0, { 0, 1, 2, 3 }, 0, 0 },
         },
      }, {
         .op = DRM_CHROME9_FRAGMENT_OP_SUB,
         .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 4, 0xf, 0 },
         .src = {
            { DRM_CHROME9_FRAGMENT_FILE_TEMP, 3, { 3, 3, 3, 3 }, 0, 0 },
            { DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0, { 0, 0, 0, 0 }, 0, 0 },
         },
      }, {
         .op = DRM_CHROME9_FRAGMENT_OP_KILL,
         .src = {
            { DRM_CHROME9_FRAGMENT_FILE_TEMP, 4, { 0, 1, 2, 3 }, 0, 0 },
         },
      }, {
         .op = DRM_CHROME9_FRAGMENT_OP_MOV,
         .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
         .src = {
            { DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 1, { 0, 1, 2, 3 }, 0, 0 },
         },
      },
   };
   uint8_t view[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4] = { 0 };

   view[0][0] = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
   view[0][1] = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
   view[0][2] = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
   view[0][3] = DRM_CHROME9_FRAGMENT_SWIZZLE_ONE;
   CHECK(chrome9_fragment_apply_depth_swizzles(instructions, 4, 1, view));
   for (unsigned lane = 0; lane < 4; lane++) {
      CHECK(instructions[1].src[0].swizzle[lane] ==
            DRM_CHROME9_FRAGMENT_SWIZZLE_ONE);
      CHECK(instructions[2].src[0].swizzle[lane] == lane);
   }

   /* KILL cannot directly apply a depth swizzle to its identity TEMP read. */
   instructions[1] = instructions[2];
   instructions[1].src[0].index = 3;
   CHECK(!chrome9_fragment_apply_depth_swizzles(instructions, 2, 1, view));
   return true;
}

static bool
check_position_xy(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 1 };
   unsigned count = 0;

   ir.instruction[0].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[0].destination = color();
   ir.instruction[0].source[0] = source(
      CHROME9_PS_SOURCE_POSITION, 0, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y);
   CHECK(chrome9_fragment_build_instructions(&ir, 0, native, &count));
   CHECK(count == 1);
   CHECK(native[0].src[0].file == DRM_CHROME9_FRAGMENT_FILE_POSITION);
   CHECK(native[0].src[0].index == 0);
   ir.instruction[0].source[0].swizzle[2] = CHROME9_PS_SWIZZLE_Z;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   ir.instruction[0].source[0].swizzle[2] = CHROME9_PS_SWIZZLE_X;
   ir.instruction[0].source[0].id = 1;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   return true;
}

static bool
check_primary_color(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 1 };
   unsigned count = 0;

   ir.instruction[0].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[0].destination = color();
   ir.instruction[0].source[0] = source(
      CHROME9_PS_SOURCE_INPUT, 0, CHROME9_PS_SWIZZLE_X,
      CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
      CHROME9_PS_SWIZZLE_W);
   CHECK(chrome9_fragment_build_instructions(&ir, 0, native, &count));
   CHECK(count == 1 && native[0].src[0].file ==
         DRM_CHROME9_FRAGMENT_FILE_PRIMARY && !native[0].src[0].index);
   ir.instruction[0].source[0].id = 1;
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   return true;
}

static bool
check_constant_ports(void)
{
   struct drm_chrome9_fragment_instruction
      native[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct chrome9_ps_ir ir = { .count = 1 };
   unsigned count = 0;

   ir.instruction[0].op = CHROME9_PS_OPCODE_CMP;
   ir.instruction[0].destination = color();
   for (unsigned s = 0; s < 3; s++)
      ir.instruction[0].source[s] = source(
         CHROME9_PS_SOURCE_CONSTANT, s, CHROME9_PS_SWIZZLE_X,
         CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_Z,
         CHROME9_PS_SWIZZLE_W);
   CHECK(!chrome9_fragment_build_instructions(&ir, 0, native, &count));
   CHECK(chrome9_fragment_legalize_constant_ports(&ir));
   CHECK(ir.count == 2);
   CHECK(ir.instruction[0].op == CHROME9_PS_OPCODE_MOV);
   CHECK(ir.instruction[0].destination.file == CHROME9_PS_DESTINATION_TEMP);
   CHECK(ir.instruction[0].destination.id == 0);
   CHECK(ir.instruction[0].source[0].file == CHROME9_PS_SOURCE_CONSTANT);
   CHECK(ir.instruction[0].source[0].id == 1);
   CHECK(ir.instruction[1].op == CHROME9_PS_OPCODE_CMP);
   CHECK(ir.instruction[1].source[1].file == CHROME9_PS_SOURCE_TEMP);
   CHECK(ir.instruction[1].source[1].id == 0);
   CHECK(chrome9_fragment_build_instructions(&ir, 0, native, &count));
   CHECK(count == 2 && native[1].op == DRM_CHROME9_FRAGMENT_OP_CMP);
   CHECK(chrome9_fragment_legalize_constant_ports(&ir) && ir.count == 2);

   ir.instruction[1].source[1] = ir.instruction[0].source[0];
   ir.instruction[1].source[2].id = 0;
   CHECK(chrome9_fragment_build_instructions(&ir, 0, native, &count));

   ir.instruction[1].source[2].id = 2;
   ir.instruction[1].destination.writemask = 0x3;
   ir.instruction[1].op = CHROME9_PS_OPCODE_MAD;
   ir.count = 1;
   ir.instruction[0] = ir.instruction[1];
   CHECK(chrome9_fragment_legalize_constant_ports(&ir));
   CHECK(ir.count == 2 && ir.instruction[0].destination.writemask == 0x3);
   CHECK(ir.instruction[1].source[1].swizzle[0] == CHROME9_PS_SWIZZLE_X);
   CHECK(ir.instruction[1].source[1].swizzle[1] == CHROME9_PS_SWIZZLE_Y);
   CHECK(ir.instruction[1].source[1].swizzle[2] == CHROME9_PS_SWIZZLE_ZERO);
   CHECK(ir.instruction[1].source[1].swizzle[3] == CHROME9_PS_SWIZZLE_ZERO);
   return true;
}

int
main(void)
{
   return !(check_direct_texture_order() && check_sparse_sampler_ids() &&
            check_computed_texture_order() && check_kill_conversion() &&
            check_depth_view_swizzles() && check_depth_kill_swizzles() &&
            check_position_xy() && check_primary_color() &&
            check_constant_ports());
}
