/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_shader_offline.h"

#include <string.h>

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
                (uint32_t)source->dependency << 17;
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
                (uint32_t)source->dependency << 21;
   if (source->modifier)
      record[3] |= 0x00100000;
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

static struct chrome9_ps_source
source(enum chrome9_ps_source_file file, unsigned id, bool alpha,
       bool dependency)
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
      .dependency = dependency,
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
