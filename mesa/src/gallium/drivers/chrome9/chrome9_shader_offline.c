/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_shader_offline.h"

#include <string.h>

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
   if (key->op == CHROME9_FS_OP_MOV && key->src[0] == CHROME9_FS_LEAF_V0) {
      const uint32_t alu[4] = { 0x01c00530, 0x301c0053,
                                0x00f04005, 0x000d0820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->varying_mask = 1;
   } else if (key->op == CHROME9_FS_OP_MOV &&
              key->src[0] == CHROME9_FS_LEAF_C0) {
      const uint32_t alu[4] = { 0x01c00530, 0x301c0053,
                                0x00f08005, 0x000d0820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_V0, CHROME9_FS_LEAF_C0)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x30080053,
                                0x00f04005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->varying_mask = 1;
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_C1)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x30081053,
                                0x00f08005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->constant_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_V0, CHROME9_FS_LEAF_V1_A)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x300416db,
                                0x00f04005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->varying_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_V0_A, CHROME9_FS_LEAF_V1)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0xb0041053,
                                0x00f0406d, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->varying_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_C0, CHROME9_FS_LEAF_C1_A)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x300816db,
                                0x00f08005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->constant_mask = 3;
   } else if (is_pair(key, CHROME9_FS_LEAF_C0_A, CHROME9_FS_LEAF_C1)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0xb0081053,
                                0x00f0806d, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
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
   if (key->op == CHROME9_FS_OP_MOV &&
       key->src[0] == CHROME9_FS_LEAF_TEX0) {
      const uint32_t alu[4] = { 0x01c00530, 0x301c0053,
                                0x00f20005, 0x000d0820 };
      memcpy(p->alu[0], alu, sizeof(alu));
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_V0)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x30040053,
                                0x00f20005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->varying_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0x30080053,
                                0x00f20005, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_C0_A)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0xb0200053,
                                0x00f0806d, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->constant_mask = 1;
   } else if (is_pair(key, CHROME9_FS_LEAF_TEX0_A, CHROME9_FS_LEAF_C0)) {
      const uint32_t alu[4] = { 0x0140b6d0, 0xb0080053,
                                0x00f2006d, 0x00000820 };
      memcpy(p->alu[0], alu, sizeof(alu));
      p->constant_mask = 1;
   } else {
      return false;
   }

   p->tau[0] = 0x301001b0;
   p->alu_count = 1;
   p->tau_count = 1;
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
   static const uint32_t mvp[4][4] = {
      { 0x0008014c, 0x0040014c, 0x00000000, 0x00060801 },
      { 0x0008014c, 0x0040414c, 0x00000000, 0x00060802 },
      { 0x0008014c, 0x0040814c, 0x00000000, 0x00060804 },
      { 0x0008014c, 0x0040c14c, 0x0000014c, 0x0034010f },
   };
   unsigned i;

   if (!p || (unsigned)key > CHROME9_VS_MVP_COLOR_TEX0_XY)
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
         append_vs(p, tex0);
         p->output_metadata = 0x00000144;
         p->input_mask |= 0x0006;
         p->a62_low = 6;
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
   default: return false;
   }
   p->a26 = 0x26000000;
   p->a60 = 0x60800000 | (p->active_count - 1);
   return p->active_count <= p->upload_count &&
          p->upload_count <= CHROME9_VS_POLICY_MAX;
}
