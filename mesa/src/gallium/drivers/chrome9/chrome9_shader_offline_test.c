/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_shader_offline.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
check_fs(void)
{
   static const struct {
      struct chrome9_fs_key key;
      uint32_t a90, a91, a92, a93;
      uint32_t alu[4];
      uint32_t tau[2];
      unsigned alu_count, tau_count;
      uint32_t constant_mask;
      uint32_t constant_x_one_mask;
      uint16_t varying_mask, sampler_mask;
   } corpus[] = {
      { { CHROME9_FS_OP_MOV, { CHROME9_FS_LEAF_V0, 0 } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x01c00530, 0x301c0053, 0x00f04005, 0x000d0820 }, { 0 }, 1, 0,
        0, 0, 1, 0 },
      { { CHROME9_FS_OP_MOV, { CHROME9_FS_LEAF_C0, 0 } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x01c00530, 0x301c0053, 0x00f08005, 0x000d0820 }, { 0 }, 1, 0,
        1, 0, 0, 0 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_V0, CHROME9_FS_LEAF_C0 } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x0140b6d0, 0x30080053, 0x00f04005, 0x00000820 }, { 0 }, 1, 0,
        1, 0, 1, 0 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_C1 } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x0140b6d0, 0x30081053, 0x00f08005, 0x00000820 }, { 0 }, 1, 0,
        3, 0, 0, 0 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_V0, CHROME9_FS_LEAF_V1_A } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x0140b6d0, 0x300416db, 0x00f04005, 0x00000820 }, { 0 }, 1, 0,
        0, 0, 3, 0 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_V0_A, CHROME9_FS_LEAF_V1 } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x0140b6d0, 0xb0041053, 0x00f0406d, 0x00000820 }, { 0 }, 1, 0,
        0, 0, 3, 0 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_C1_A } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x0140b6d0, 0x300816db, 0x00f08005, 0x00000820 }, { 0 }, 1, 0,
        3, 0, 0, 0 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_C0_A, CHROME9_FS_LEAF_C1 } },
        0x90058000, 0x91000000, 0x92000001, 0x93000000,
        { 0x0140b6d0, 0xb0081053, 0x00f0806d, 0x00000820 }, { 0 }, 1, 0,
        3, 0, 0, 0 },
      { { CHROME9_FS_OP_MOV, { CHROME9_FS_LEAF_TEX0, 0 } },
        0x90018000, 0x91000001, 0x92010001, 0x93000001,
        { 0x01c00530, 0x301c0053, 0x00f20005, 0x000d0820 },
        { 0x301001b0 }, 1, 1, 0, 0, 0, 1 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_V0 } },
        0x90018000, 0x91000001, 0x92010001, 0x93000001,
        { 0x0140b6d0, 0x30040053, 0x00f20005, 0x00000820 },
        { 0x301001b0 }, 1, 1, 0, 0, 1, 1 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0 } },
        0x90018000, 0x91000001, 0x92010001, 0x93000001,
        { 0x0140b6d0, 0x30080053, 0x00f20005, 0x00000820 },
        { 0x301001b0 }, 1, 1, 1, 0, 0, 1 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0_A } },
        0x90018000, 0x91000001, 0x92010001, 0x93000001,
        { 0x0140b6d0, 0xb0200053, 0x00f0806d, 0x00000820 },
        { 0x301001b0 }, 1, 1, 1, 0, 0, 1 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_TEX0_A, CHROME9_FS_LEAF_C0 } },
        0x90018000, 0x91000001, 0x92010001, 0x93000001,
        { 0x0140b6d0, 0xb0080053, 0x00f2006d, 0x00000820 },
        { 0x301001b0 }, 1, 1, 1, 0, 0, 1 },
      { { CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_TEX1 } },
        0x90000000, 0x91000001, 0x92020001, 0x93000002,
        { 0x0140b6d0, 0x3003f000, 0x00f20005, 0x00000820 },
        { 0xa01001b0, 0x311001b1 }, 1, 2, 0, 0, 0, 3 },
      { { CHROME9_FS_OP_RGBX_C0X,
          { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0 } },
        0x90018000, 0x91000001, 0x92010002, 0x93000001,
        { 0x01c00530, 0x501c0053, 0x00720005, 0x000d0820 },
        { 0x301001b0 }, 2, 1, 1, 1, 0, 1 },
   };
   struct chrome9_ps_program p;
   unsigned i;

   for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
      assert(chrome9_compile_fs_offline(&corpus[i].key, &p));
      assert(p.control[0] == corpus[i].a90);
      assert(p.control[1] == corpus[i].a91);
      assert(p.control[2] == corpus[i].a92);
      assert(p.control[3] == corpus[i].a93);
      assert(p.alu_count == corpus[i].alu_count);
      assert(p.tau_count == corpus[i].tau_count);
      assert(p.constant_mask == corpus[i].constant_mask);
      assert(p.constant_x_one_mask == corpus[i].constant_x_one_mask);
      assert(p.varying_mask == corpus[i].varying_mask);
      assert(p.sampler_mask == corpus[i].sampler_mask);
      assert(!memcmp(p.alu[0], corpus[i].alu, sizeof(corpus[i].alu)));
      assert(!memcmp(p.tau, corpus[i].tau, sizeof(corpus[i].tau)));
   }

   {
      const struct chrome9_fs_key reversed = {
         CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_TEX0 }
      };
      assert(chrome9_compile_fs_offline(&reversed, &p));
      assert(p.alu[0][1] == 0x30080053 && p.sampler_mask == 1);
   }

   {
      static const uint32_t alpha_write[4] = {
         0x01c00530, 0x801c0053, 0x008080b6, 0x000d0820
      };
      const struct chrome9_fs_key rgbx = {
         CHROME9_FS_OP_RGBX_C0X,
         { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0 }
      };
      assert(chrome9_compile_fs_offline(&rgbx, &p));
      assert(!memcmp(p.alu[1], alpha_write, sizeof(alpha_write)));
      assert(p.constant_x_one_mask == 1);
   }

   {
      const struct chrome9_fs_key rejected = {
         CHROME9_FS_OP_MUL, { CHROME9_FS_LEAF_TEX1, CHROME9_FS_LEAF_C1 }
      };
      assert(!chrome9_compile_fs_offline(&rejected, &p));
   }
}

static void
check_vs(void)
{
   static const uint32_t mvp_tex2[7][4] = {
      { 0x0008014c, 0x0040014c, 0x00000000, 0x00060801 },
      { 0x0008014c, 0x0040414c, 0x00000000, 0x00060802 },
      { 0x0008014c, 0x0040814c, 0x00000000, 0x00060804 },
      { 0x0008014c, 0x0040c14c, 0x0000014c, 0x0034010f },
      { 0x0010014c, 0x00000000, 0x00000000, 0x00080a0f },
      { 0x0008414c, 0x00000000, 0x00000000, 0x00080283 },
      { 0x0008814c, 0x00000000, 0x00000000, 0x00080293 },
   };
   static const uint32_t mvp_color_tex2[8][4] = {
      { 0x0008014c, 0x0040014c, 0x00000000, 0x00060801 },
      { 0x0008014c, 0x0040414c, 0x00000000, 0x00060802 },
      { 0x0008014c, 0x0040814c, 0x00000000, 0x00060804 },
      { 0x0008014c, 0x0040c14c, 0x0000014c, 0x0034010f },
      { 0x0010014c, 0x00000000, 0x00000000, 0x00080a0f },
      { 0x0008414c, 0x00000000, 0x00000000, 0x0008422f },
      { 0x0008814c, 0x00000000, 0x00000000, 0x00080283 },
      { 0x0008c14c, 0x00000000, 0x00000000, 0x00080293 },
   };
   struct chrome9_vs_program p;

   assert(chrome9_compile_vs_offline(CHROME9_VS_POSITION, &p));
   assert(p.upload_count == 6 && p.active_count == 4);
   assert(p.output_metadata == 0x00000004 && p.a62_low == 0);
   assert(p.a25 == 0x25f00000 && p.a26 == 0x26000000);
   assert(p.a60 == 0x60800003);
   assert(p.instruction[0][3] == 0x00080a0f);

   assert(chrome9_compile_vs_offline(CHROME9_VS_POSITION_TEX0_TEX1_XY, &p));
   assert(p.upload_count == 8 && p.active_count == 6);
   assert(p.output_metadata == 0x00000204 && p.a62_low == 2);
   assert(p.a25 == 0x25f0cc00 && p.a60 == 0x60800005);
   assert(p.instruction[1][3] == 0x00080283);
   assert(p.instruction[2][3] == 0x00080293);

   assert(chrome9_compile_vs_offline(CHROME9_VS_MVP_POSITION, &p));
   assert(p.upload_count == 10 && p.active_count == 8);
   assert(p.output_metadata == 0x00000004 && p.a62_low == 3);
   assert(p.a25 == 0x25f00000 && p.a60 == 0x60800007);

   assert(chrome9_compile_vs_offline(CHROME9_VS_MVP_POSITION_TEX0_XY, &p));
   assert(p.upload_count == 11 && p.active_count == 9);
   assert(p.output_metadata == 0x00000104 && p.a62_low == 5);
   assert(p.a25 == 0x25f0c000 && p.a60 == 0x60800008);

   assert(chrome9_compile_vs_offline(
             CHROME9_VS_MVP_POSITION_TEX0_TEX1_XY, &p));
   assert(p.upload_count == 12 && p.active_count == 10);
   assert(p.output_metadata == 0x00000204 && p.a62_low == 6);
   assert(p.a25 == 0x25f0cc00 && p.a60 == 0x60800009);
   assert(!memcmp(p.instruction, mvp_tex2, sizeof(mvp_tex2)));

   assert(chrome9_compile_vs_offline(CHROME9_VS_MVP_COLOR_TEX0_XY, &p));
   assert(p.upload_count == 12 && p.active_count == 10);
   assert(p.output_metadata == 0x00000144 && p.a62_low == 6);
   assert(p.a25 == 0x25f4c000 && p.a60 == 0x60800009);
   assert(p.constant_mask[0] == 0x0f && p.input_mask == 0x07);
   assert(p.instruction[3][1] == 0x0040c14c);
   assert(p.instruction[6][0] == 0x0008814c);
   assert(p.instruction[6][3] == 0x00080283);

   assert(chrome9_compile_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &p));
   assert(p.upload_count == 13 && p.active_count == 11);
   assert(p.output_metadata == 0x00000244 && p.a62_low == 7);
   assert(p.a25 == 0x25f4cc00 && p.a60 == 0x6080000a);
   assert(p.constant_mask[0] == 0x0f && p.input_mask == 0x0f);
   assert(!memcmp(p.instruction, mvp_color_tex2, sizeof(mvp_color_tex2)));
   assert(!chrome9_compile_vs_offline((enum chrome9_vs_key)-1, &p));
}

int
main(void)
{
   check_fs();
   check_vs();
   puts("Chrome9 offline shader corpus: 15 PS + 8 VS templates passed");
   return 0;
}
