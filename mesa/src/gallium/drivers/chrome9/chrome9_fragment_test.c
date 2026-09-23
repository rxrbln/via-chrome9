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
   CHECK(native[0].sampler == 1);
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

int
main(void)
{
   return !(check_direct_texture_order() && check_computed_texture_order());
}
