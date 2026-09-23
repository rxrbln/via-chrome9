/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_shader_offline.h"

/* This is a verifier, including in release builds.  Meson's release profile
 * defines NDEBUG globally, which must not compile the checks (or the compiler
 * calls embedded in them) out of this executable. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
check_ps_encoder(void)
{
   static const uint32_t mov_v0[4] = {
      0x01c00530, 0x301c0053, 0x00f04005, 0x000d0820,
   };
   static const uint32_t mul_v0_c0[4] = {
      0x0140b6d0, 0x30080053, 0x00f04005, 0x00000820,
   };
   static const uint32_t add_v0_c0[4] = {
      0x00800530, 0x30140924, 0x00f04005, 0x00000020,
   };
   static const uint32_t max_v0_c0[4] = {
      0x01c00530, 0x30080053, 0x00f04005, 0x000b0820,
   };
   static const uint32_t dp3_v0_c0[4] = {
      0x01c00530, 0x50080055, 0x00f04005, 0x00020820,
   };
   static const uint32_t dp4_v0_c0[4] = {
      0x01c00530, 0x30080053, 0x00f04005, 0x00030820,
   };
   static const uint32_t rsq_c0_w[4] = {
      0x01c00530, 0xb01c0053, 0x00f080b6, 0x00070820,
   };
   static const uint32_t abs_v0[4] = {
      0x00400538, 0x30040053, 0x00f04005, 0x00090820,
   };
   static const uint32_t cmp_c0_v0_c0[4] = {
      0x00800530, 0x30040053, 0x00f08005, 0x00090820,
   };
   static const uint32_t frc_v0[4] = {
      0x01c00530, 0x301c0053, 0x00f04005, 0x000e0820,
   };
   struct chrome9_ps_source v0 = {
      .file = CHROME9_PS_SOURCE_INPUT,
      .swizzle = {
         CHROME9_PS_SWIZZLE_X, CHROME9_PS_SWIZZLE_Y,
         CHROME9_PS_SWIZZLE_Z, CHROME9_PS_SWIZZLE_W,
      },
   };
   struct chrome9_ps_source c0 = v0;
   struct chrome9_ps_source c0w;
   uint32_t record[4];

   c0.file = CHROME9_PS_SOURCE_CONSTANT;
   c0w = c0;
   for (unsigned lane = 0; lane < 3; lane++)
      c0w.swizzle[lane] = CHROME9_PS_SWIZZLE_ZERO;
   c0w.swizzle[3] = CHROME9_PS_SWIZZLE_W;
   assert(chrome9_encode_ps_mov(&v0, record));
   assert(!memcmp(record, mov_v0, sizeof(record)));
   assert(chrome9_encode_ps_mul(&v0, &c0, record));
   assert(!memcmp(record, mul_v0_c0, sizeof(record)));
   assert(chrome9_encode_ps_add(&v0, &c0, record));
   assert(!memcmp(record, add_v0_c0, sizeof(record)));
   assert(chrome9_encode_ps_max(&v0, &c0, record));
   assert(!memcmp(record, max_v0_c0, sizeof(record)));
   assert(chrome9_encode_ps_dp3(&v0, &c0, record));
   assert(!memcmp(record, dp3_v0_c0, sizeof(record)));
   assert(chrome9_encode_ps_dp4(&v0, &c0, record));
   assert(!memcmp(record, dp4_v0_c0, sizeof(record)));
   assert(chrome9_encode_ps_rsq(&c0w, record));
   assert(!memcmp(record, rsq_c0_w, sizeof(record)));
   assert(chrome9_encode_ps_abs(&v0, record));
   assert(!memcmp(record, abs_v0, sizeof(record)));
   assert(chrome9_encode_ps_cmp(&c0, &v0, &c0, record));
   assert(!memcmp(record, cmp_c0_v0_c0, sizeof(record)));
   assert(chrome9_encode_ps_frc(&v0, record));
   assert(!memcmp(record, frc_v0, sizeof(record)));

   assert(!chrome9_encode_ps_mov(NULL, record));
   assert(!chrome9_encode_ps_mov(&v0, NULL));
   assert(!chrome9_encode_ps_mul(NULL, &c0, record));
   assert(!chrome9_encode_ps_mul(&v0, NULL, record));
   assert(!chrome9_encode_ps_mul(&v0, &c0, NULL));
   assert(!chrome9_encode_ps_add(NULL, &c0, record));
   assert(!chrome9_encode_ps_add(&v0, NULL, record));
   assert(!chrome9_encode_ps_add(&v0, &c0, NULL));

   v0.id = 2;
   assert(!chrome9_encode_ps_mov(&v0, record));
   v0.id = 0;
   v0.swizzle[2] = 6;
   assert(!chrome9_encode_ps_mov(&v0, record));
   v0.swizzle[2] = CHROME9_PS_SWIZZLE_Z;
   v0.modifier = 1;
   assert(!chrome9_encode_ps_mov(&v0, record));
   v0.modifier = 0;
   c0.id = 32;
   assert(!chrome9_encode_ps_mul(&v0, &c0, record));
   assert(!chrome9_encode_ps_add(&v0, &c0, record));
}

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
      assert(p.texture_input_mask == corpus[i].sampler_mask);
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

static struct chrome9_ps_source
identity_source(enum chrome9_ps_source_file file, unsigned id,
                bool last_use)
{
   struct chrome9_ps_source source = {
      .file = file,
      .id = id,
      .swizzle = {
         CHROME9_PS_SWIZZLE_X, CHROME9_PS_SWIZZLE_Y,
         CHROME9_PS_SWIZZLE_Z, CHROME9_PS_SWIZZLE_W,
      },
      .last_use = last_use,
   };

   return source;
}

static struct chrome9_ps_source
scalar_source(enum chrome9_ps_source_file file, unsigned id,
              enum chrome9_ps_swizzle component)
{
   struct chrome9_ps_source source = {
      .file = file,
      .id = id,
      .swizzle = { component, component, component, component },
   };

   return source;
}

static void
check_jellyfish_ps_ir(void)
{
   static const uint32_t expected_alu[6][4] = {
      { 0x00c30550, 0x50140924, 0x0072c205, 0x000000a0 },
      { 0x02010550, 0x50140924, 0x00720205, 0x000000e0 },
      { 0x0140b6d0, 0x50000055, 0x00720305, 0x00000820 },
      { 0x0140b6d0, 0xd02c376d, 0x0010c376, 0x00000120 },
      { 0x020076d0, 0xd0140924, 0x00120416, 0x00000160 },
      { 0x0080b680, 0x80140924, 0x008205b6, 0x00000820 },
   };
   static const uint32_t expected_tau[2] = {
      0x301001b0, 0x311111b1,
   };
   static const uint32_t expected_control[12] = {
      0x9000800c, 0x91000003, 0x92020006, 0x93000002,
      0x94000000, 0x95000000, 0x96000000, 0x97000000,
      0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
   };
   struct chrome9_ps_ir ir = { 0 };
   struct chrome9_ps_program program;

   ir.texture_count = 2;
   ir.texture[0] = (struct chrome9_ps_texture_instruction) {
      .sampler = 0, .coordinate = 0, .destination = 0,
   };
   ir.texture[1] = (struct chrome9_ps_texture_instruction) {
      .sampler = 1, .coordinate = 1, .destination = 1,
   };
   ir.count = 6;

   /* r2.rgb = T2.rgb + T3.rgb */
   ir.instruction[0].op = CHROME9_PS_OPCODE_ADD;
   ir.instruction[0].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 2, .writemask = 0x7,
   };
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEXTURE, 2, false);
   ir.instruction[0].source[1] =
      identity_source(CHROME9_PS_SOURCE_TEXTURE, 3, false);

   /* r3.rgb = r2.rgb + r1.rgb */
   ir.instruction[1].op = CHROME9_PS_OPCODE_ADD;
   ir.instruction[1].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 3, .writemask = 0x7,
   };
   ir.instruction[1].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 2, false);
   ir.instruction[1].source[1] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 1, false);

   /* oC0.rgb = r3.rgb * r0.rgb */
   ir.instruction[2].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[2].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_COLOR, .writemask = 0x7,
   };
   ir.instruction[2].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 3, false);
   ir.instruction[2].source[1] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);

   /* r4.x = T3.w * T3.w */
   ir.instruction[3].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[3].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 4, .writemask = 0x1,
   };
   ir.instruction[3].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEXTURE, 3, CHROME9_PS_SWIZZLE_W);
   ir.instruction[3].source[1] = ir.instruction[3].source[0];

   /* r5.x = r4.x + r0.w */
   ir.instruction[4].op = CHROME9_PS_OPCODE_ADD;
   ir.instruction[4].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 5, .writemask = 0x1,
   };
   ir.instruction[4].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 4, CHROME9_PS_SWIZZLE_X);
   ir.instruction[4].source[1] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 0, CHROME9_PS_SWIZZLE_W);

   /* oC0.w = r5.x + c0.x */
   ir.instruction[5].op = CHROME9_PS_OPCODE_ADD;
   ir.instruction[5].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_COLOR, .writemask = 0x8,
   };
   ir.instruction[5].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 5, CHROME9_PS_SWIZZLE_X);
   ir.instruction[5].source[1] =
      scalar_source(CHROME9_PS_SOURCE_CONSTANT, 0, CHROME9_PS_SWIZZLE_X);

   assert(chrome9_compile_ps_ir(&ir, &program));
   assert(program.alu_count == 6 && program.tau_count == 2);
   assert(program.constant_mask == 1 && !program.varying_mask);
   assert(program.texture_input_mask == 0xf && program.sampler_mask == 3);
   assert(!memcmp(program.alu, expected_alu, sizeof(expected_alu)));
   assert(!memcmp(program.tau, expected_tau, sizeof(expected_tau)));
   assert(!memcmp(program.control, expected_control, sizeof(expected_control)));

   /* Partial definitions are component checked, and both color writes must
    * be disjoint and collectively complete. */
   ir.instruction[4].source[0].swizzle[0] = CHROME9_PS_SWIZZLE_Y;
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.instruction[4].source[0].swizzle[0] = CHROME9_PS_SWIZZLE_X;
   ir.instruction[5].destination.writemask = 0x1;
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.instruction[5].destination.writemask = 0x8;
   ir.instruction[3].destination.id = 2;
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.instruction[3].destination.id = 4;
   ir.texture[1].coordinate = 2;
   assert(!chrome9_compile_ps_ir(&ir, &program));
}

static void
check_buffer_ps_ir(void)
{
   static const uint32_t expected_alu[8][4] = {
      { 0x0140b6d0, 0x502c06dd, 0x0070c005, 0x00000020 },
      { 0x01c00530, 0xd000056d, 0x00100036, 0x000c0060 },
      { 0x01c00530, 0xd020116d, 0x00120016, 0x000c00a0 },
      { 0x0140b6d0, 0xd000216d, 0x00120216, 0x000000e0 },
      { 0x0140b6d0, 0xd008016d, 0x00120316, 0x00000120 },
      { 0x01c00530, 0x801c0053, 0x001204b6, 0x00040160 },
      { 0x80810530, 0x30140924, 0x00f08205, 0x000001a0 },
      { 0x00820530, 0x00206053, 0x00f20500, 0x00000820 },
   };
   static const uint32_t expected_control[12] = {
      0x90040001, 0x91000000, 0x92000008, 0x93000000,
      0x94000000, 0x95000000, 0x96000000, 0x97000000,
      0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
   };
   struct chrome9_ps_ir ir = { 0 };
   struct chrome9_ps_program program;

   ir.count = 8;

   /* r0.xyz = T0.xyz * T0.www */
   ir.instruction[0].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[0].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 0, .writemask = 0x7,
   };
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEXTURE, 0, false);
   ir.instruction[0].source[1] =
      scalar_source(CHROME9_PS_SOURCE_TEXTURE, 0, CHROME9_PS_SWIZZLE_W);

   /* r1.x = min(r0.y, r0.z) */
   ir.instruction[1].op = CHROME9_PS_OPCODE_MIN;
   ir.instruction[1].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 1, .writemask = 0x1,
   };
   ir.instruction[1].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 0, CHROME9_PS_SWIZZLE_Y);
   ir.instruction[1].source[1] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 0, CHROME9_PS_SWIZZLE_Z);

   /* r2.x = min(r0.x, r1.x) */
   ir.instruction[2].op = CHROME9_PS_OPCODE_MIN;
   ir.instruction[2].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 2, .writemask = 0x1,
   };
   ir.instruction[2].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 0, CHROME9_PS_SWIZZLE_X);
   ir.instruction[2].source[1] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 1, CHROME9_PS_SWIZZLE_X);

   /* r3.x = r2.x * r2.x */
   ir.instruction[3].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[3].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 3, .writemask = 0x1,
   };
   ir.instruction[3].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 2, CHROME9_PS_SWIZZLE_X);
   ir.instruction[3].source[1] = ir.instruction[3].source[0];

   /* r4.x = r3.x * c0.x; r5.x = exp2(r4.x) */
   ir.instruction[4].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[4].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 4, .writemask = 0x1,
   };
   ir.instruction[4].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 3, CHROME9_PS_SWIZZLE_X);
   ir.instruction[4].source[1] =
      scalar_source(CHROME9_PS_SOURCE_CONSTANT, 0, CHROME9_PS_SWIZZLE_X);
   ir.instruction[5].op = CHROME9_PS_OPCODE_EXP2;
   ir.instruction[5].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 5, .writemask = 0x1,
   };
   ir.instruction[5].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 4, CHROME9_PS_SWIZZLE_X);

   /* mix(c2,c1,r5) = c2 + r5*(c1-c2). */
   ir.instruction[6].op = CHROME9_PS_OPCODE_SUB;
   ir.instruction[6].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_TEMP, .id = 6, .writemask = 0xf,
   };
   ir.instruction[6].source[0] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 1, false);
   ir.instruction[6].source[1] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 2, false);
   ir.instruction[7].op = CHROME9_PS_OPCODE_MAD;
   ir.instruction[7].destination = (struct chrome9_ps_destination) {
      .file = CHROME9_PS_DESTINATION_COLOR, .writemask = 0xf,
   };
   ir.instruction[7].source[0] =
      scalar_source(CHROME9_PS_SOURCE_TEMP, 5, CHROME9_PS_SWIZZLE_X);
   ir.instruction[7].source[1] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 6, false);
   ir.instruction[7].source[2] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 2, false);

   assert(chrome9_compile_ps_ir(&ir, &program));
   assert(program.alu_count == 8 && !program.tau_count);
   assert(program.constant_mask == 0x7 && !program.varying_mask);
   assert(program.texture_input_mask == 1 && !program.sampler_mask);
   if (memcmp(program.alu, expected_alu, sizeof(expected_alu)))
      for (unsigned i = 0; i < 8; i++)
         fprintf(stderr, "%u: %08x %08x %08x %08x\n", i,
                 program.alu[i][0], program.alu[i][1],
                 program.alu[i][2], program.alu[i][3]);
   assert(!memcmp(program.alu, expected_alu, sizeof(expected_alu)));
   assert(!memcmp(program.control, expected_control,
                  sizeof(expected_control)));

   /* Direct inputs and temporary ids are generic resources, but the active
    * temporary namespace must remain dense and bounded. */
   ir.instruction[6].destination.id = 7;
   assert(!chrome9_compile_ps_ir(&ir, &program));
}

static void
check_ps_ir(void)
{
   static const uint32_t add_c0_c1_oc0[4] = {
      0x00810530, 0x30140924, 0x00f08005, 0x00000820,
   };
   static const uint32_t mul_c0_c1_r0[4] = {
      0x0140b6d0, 0x30081053, 0x00f08005, 0x00000020,
   };
   static const uint32_t mul_r0_c2_oc0[4] = {
      0x0140b6d0, 0x30082053, 0x00f20005, 0x00000820,
   };
   struct chrome9_ps_ir ir = { 0 };
   struct chrome9_ps_program program;

   ir.count = 1;
   ir.instruction[0].op = CHROME9_PS_OPCODE_ADD;
   ir.instruction[0].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[0].destination.writemask = 0xf;
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 0, false);
   ir.instruction[0].source[1] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 1, false);
   assert(chrome9_compile_ps_ir(&ir, &program));
   assert(program.alu_count == 1 && program.constant_mask == 3);
   assert(!memcmp(program.alu[0], add_c0_c1_oc0,
                  sizeof(add_c0_c1_oc0)));

   memset(&ir, 0, sizeof(ir));

   ir.count = 2;
   ir.instruction[0].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[0].destination.file = CHROME9_PS_DESTINATION_TEMP;
   ir.instruction[0].destination.writemask = 0xf;
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 0, false);
   ir.instruction[0].source[1] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 1, false);
   ir.instruction[1].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[1].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[1].destination.writemask = 0xf;
   ir.instruction[1].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);
   ir.instruction[1].source[1] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 2, false);

   assert(chrome9_compile_ps_ir(&ir, &program));
   assert(program.alu_count == 2 && !program.tau_count);
   assert(program.control[0] == 0x90058000);
   assert(program.control[2] == 0x92000002);
   assert(program.constant_mask == 0x7 && !program.varying_mask);
   assert(!memcmp(program.alu[0], mul_c0_c1_r0,
                  sizeof(mul_c0_c1_r0)));
   assert(!memcmp(program.alu[1], mul_r0_c2_oc0,
                  sizeof(mul_r0_c2_oc0)));

   ir.instruction[1].source[0].last_use = true;
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.instruction[1].source[0].last_use = false;
   ir.instruction[0].destination.id = 12;
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.instruction[0].destination.id = 0;
   ir.instruction[0].destination.file = CHROME9_PS_DESTINATION_COLOR;
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.count = 7;
   assert(!chrome9_compile_ps_ir(&ir, &program));

   /* A temporary must be defined before use and must not be left dead. */
   memset(&ir, 0, sizeof(ir));
   ir.count = 1;
   ir.instruction[0].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[0].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[0].destination.writemask = 0xf;
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);
   assert(!chrome9_compile_ps_ir(&ir, &program));
   ir.count = 2;
   ir.instruction[0].destination.file = CHROME9_PS_DESTINATION_TEMP;
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 0, false);
   ir.instruction[1].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[1].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[1].destination.writemask = 0xf;
   ir.instruction[1].source[0] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 1, false);
   assert(!chrome9_compile_ps_ir(&ir, &program));

   /* The finalizer marks only the last read.  Different final temporaries
    * may release together in A and C. */
   memset(&ir, 0, sizeof(ir));
   ir.count = 3;
   ir.instruction[0].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[0].destination.file = CHROME9_PS_DESTINATION_TEMP;
   ir.instruction[0].destination.id = 0;
   ir.instruction[0].destination.writemask = 0xf;
   ir.instruction[0].source[0] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 0, false);
   ir.instruction[0].source[1] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 1, false);
   ir.instruction[1].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[1].destination.file = CHROME9_PS_DESTINATION_TEMP;
   ir.instruction[1].destination.id = 1;
   ir.instruction[1].destination.writemask = 0xf;
   ir.instruction[1].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);
   ir.instruction[1].source[1] =
      identity_source(CHROME9_PS_SOURCE_CONSTANT, 2, false);
   ir.instruction[2].op = CHROME9_PS_OPCODE_MUL;
   ir.instruction[2].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[2].destination.writemask = 0xf;
   ir.instruction[2].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);
   ir.instruction[2].source[1] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 1, false);
   assert(chrome9_compile_ps_ir(&ir, &program));
   assert(!(program.alu[1][2] & (1u << 17)));
   assert(program.alu[2][2] & (1u << 17));
   assert(program.alu[2][1] & (1u << 21));

   /* When A and C make the final read of one temporary, the vendor encoding
    * gives the single release marker to A. */
   ir.count = 2;
   ir.instruction[1].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[1].destination.id = 0;
   ir.instruction[1].source[1] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);
   assert(chrome9_compile_ps_ir(&ir, &program));
   assert(program.alu[1][2] & (1u << 17));
   assert(!(program.alu[1][1] & (1u << 21)));

   /* Re-defining a live SSA-style temporary is rejected. */
   ir.count = 3;
   ir.instruction[1].destination.file = CHROME9_PS_DESTINATION_TEMP;
   ir.instruction[1].destination.id = 0;
   ir.instruction[2].op = CHROME9_PS_OPCODE_MOV;
   ir.instruction[2].destination.file = CHROME9_PS_DESTINATION_COLOR;
   ir.instruction[2].destination.writemask = 0xf;
   ir.instruction[2].source[0] =
      identity_source(CHROME9_PS_SOURCE_TEMP, 0, false);
   assert(!chrome9_compile_ps_ir(&ir, &program));
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

static void
check_verifier(void)
{
   const struct chrome9_fs_key fs_key = {
      CHROME9_FS_OP_MUL,
      { CHROME9_FS_LEAF_TEX0, CHROME9_FS_LEAF_TEX1 },
   };
   struct chrome9_ps_program ps;
   struct chrome9_vs_program vs;
   enum chrome9_shader_verify_error error;

   assert(chrome9_compile_fs_offline(&fs_key, &ps));
   assert(!chrome9_verify_fs_offline(NULL, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_NULL);
   assert(chrome9_verify_fs_offline(&fs_key, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_OK);
   ps.alu_count++;
   assert(!chrome9_verify_fs_offline(&fs_key, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_COUNT);
   ps.alu_count--;
   ps.sampler_mask ^= 1;
   assert(!chrome9_verify_fs_offline(&fs_key, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_RESOURCE);
   ps.sampler_mask ^= 1;
   ps.control[3] ^= 1;
   assert(!chrome9_verify_fs_offline(&fs_key, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_CONTROL);
   ps.control[3] ^= 1;
   ps.tau[1] ^= 1;
   assert(!chrome9_verify_fs_offline(&fs_key, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_INSTRUCTION);
   ps.tau[1] ^= 1;
   ps.tau[CHROME9_PS_MAX_TAU - 1] = 1;
   assert(!chrome9_verify_fs_offline(&fs_key, &ps, &error));
   assert(error == CHROME9_SHADER_VERIFY_SLACK);

   assert(chrome9_compile_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs));
   assert(!chrome9_verify_vs_offline((enum chrome9_vs_key)-1,
                                     &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_KEY);
   assert(chrome9_verify_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_OK);
   vs.active_count--;
   assert(!chrome9_verify_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_COUNT);
   vs.active_count++;
   vs.input_mask ^= 1;
   assert(!chrome9_verify_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_RESOURCE);
   vs.input_mask ^= 1;
   vs.a62_low ^= 1;
   assert(!chrome9_verify_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_CONTROL);
   vs.a62_low ^= 1;
   vs.instruction[7][0] ^= 1;
   assert(!chrome9_verify_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_INSTRUCTION);
   vs.instruction[7][0] ^= 1;
   vs.instruction[CHROME9_VS_POLICY_MAX - 1][0] = 1;
   assert(!chrome9_verify_vs_offline(
             CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY, &vs, &error));
   assert(error == CHROME9_SHADER_VERIFY_SLACK);
}

int
main(void)
{
   check_ps_encoder();
   check_fs();
   check_ps_ir();
   check_buffer_ps_ir();
   check_jellyfish_ps_ir();
   check_vs();
   check_verifier();
   puts("Chrome9 PS encoder + offline corpus: multi-record PS + "
        "8 VS templates and verifier passed");
   return 0;
}
