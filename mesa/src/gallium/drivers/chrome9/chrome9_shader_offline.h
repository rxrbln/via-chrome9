/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_SHADER_OFFLINE_H
#define CHROME9_SHADER_OFFLINE_H

#include <stdbool.h>
#include <stdint.h>

#define CHROME9_PS_MAX_ALU 96
#define CHROME9_PS_MAX_TAU 32
#define CHROME9_VS_POLICY_MAX 256

enum chrome9_fs_leaf {
   CHROME9_FS_LEAF_V0,
   CHROME9_FS_LEAF_V1,
   CHROME9_FS_LEAF_C0,
   CHROME9_FS_LEAF_C1,
   CHROME9_FS_LEAF_TEX0,
   CHROME9_FS_LEAF_TEX1,
   CHROME9_FS_LEAF_V0_A,
   CHROME9_FS_LEAF_V1_A,
   CHROME9_FS_LEAF_C0_A,
   CHROME9_FS_LEAF_C1_A,
   CHROME9_FS_LEAF_TEX0_A,
};

enum chrome9_fs_op {
   CHROME9_FS_OP_MOV,
   CHROME9_FS_OP_MUL,
   /* Copy TEX0.rgb and force output alpha from reserved c0.x == 1.0. */
   CHROME9_FS_OP_RGBX_C0X,
};

/* Canonical output expression after NIR scalarization, copy propagation and
 * DCE.  This is deliberately not a general shader IR: an unrepresented field
 * or operation must be rejected by the NIR-facing classifier. */
struct chrome9_fs_key {
   enum chrome9_fs_op op;
   enum chrome9_fs_leaf src[2];
};

enum chrome9_ps_source_file {
   CHROME9_PS_SOURCE_TEMP = 0,
   CHROME9_PS_SOURCE_INPUT = 1,
   CHROME9_PS_SOURCE_CONSTANT = 2,
   /* Full-precision class-3 texture/misc interpolator T0..T7. */
   CHROME9_PS_SOURCE_TEXTURE = 3,
};

enum chrome9_ps_destination_file {
   CHROME9_PS_DESTINATION_TEMP = 0,
   CHROME9_PS_DESTINATION_COLOR = 2,
};

enum chrome9_ps_swizzle {
   CHROME9_PS_SWIZZLE_X,
   CHROME9_PS_SWIZZLE_Y,
   CHROME9_PS_SWIZZLE_Z,
   CHROME9_PS_SWIZZLE_W,
   CHROME9_PS_SWIZZLE_ONE,
   CHROME9_PS_SWIZZLE_ZERO,
};

#define CHROME9_PS_SOURCE_NEGATE 8

struct chrome9_ps_source {
   enum chrome9_ps_source_file file;
   uint8_t id;
   uint8_t swizzle[4];
   uint8_t modifier;
   /* Filled by the compiler finalizer for the last use of a temporary. */
   bool last_use;
};

struct chrome9_ps_destination {
   enum chrome9_ps_destination_file file;
   uint8_t id;
   uint8_t writemask;
   bool saturate;
};

enum chrome9_ps_opcode {
   CHROME9_PS_OPCODE_MOV,
   CHROME9_PS_OPCODE_MUL,
   CHROME9_PS_OPCODE_ADD,
   CHROME9_PS_OPCODE_SUB,
   CHROME9_PS_OPCODE_MAD,
   CHROME9_PS_OPCODE_MIN,
   CHROME9_PS_OPCODE_EXP2,
};

struct chrome9_ps_instruction {
   enum chrome9_ps_opcode op;
   struct chrome9_ps_destination destination;
   struct chrome9_ps_source source[3];
};

struct chrome9_ps_texture_instruction {
   uint8_t sampler;
   uint8_t coordinate;
   uint8_t destination;
   uint8_t reserved;
};

/* Straight-line compiler IR.  This is deliberately semantic rather than a
 * native-word container: every record is range checked and packed here. */
struct chrome9_ps_ir {
   struct chrome9_ps_texture_instruction texture[CHROME9_PS_MAX_TAU];
   struct chrome9_ps_instruction instruction[CHROME9_PS_MAX_ALU];
   uint8_t texture_count;
   uint8_t count;
};

struct chrome9_ps_program {
   uint32_t control[12];       /* complete A90..A9b words */
   uint32_t alu[CHROME9_PS_MAX_ALU][4];
   uint32_t tau[CHROME9_PS_MAX_TAU];
   uint32_t constant_mask;     /* c0..c31 */
   uint32_t constant_x_one_mask; /* constants whose X must be driver-owned 1 */
   uint16_t varying_mask;      /* bit 0 is primary color v0 */
   uint16_t texture_input_mask; /* T0..T7 used by TAU or directly by ALU */
   uint16_t sampler_mask;      /* s0..s15 */
   uint8_t alu_count;
   uint8_t tau_count;
};

enum chrome9_vs_key {
   CHROME9_VS_POSITION,
   CHROME9_VS_POSITION_TEX0_XY,
   CHROME9_VS_POSITION_TEX0_TEX1_XY,
   CHROME9_VS_MVP_POSITION,
   CHROME9_VS_MVP_POSITION_TEX0_XY,
   CHROME9_VS_MVP_POSITION_TEX0_TEX1_XY,
   CHROME9_VS_MVP_COLOR_TEX0_XY,
   CHROME9_VS_MVP_COLOR_TEX0_TEX1_XY,
};

struct chrome9_vs_program {
   uint32_t instruction[CHROME9_VS_POLICY_MAX][4];
   uint32_t output_metadata;
   uint32_t a25;
   uint32_t a26;
   uint32_t a60;
   uint32_t constant_mask[8];  /* c0..c255 */
   uint16_t input_mask;        /* v0..v15 */
   uint16_t a62_low;           /* compiler-derived A62 low nine bits */
   uint16_t active_count;      /* A60 is 60800000|(active_count-1) */
   uint16_t upload_count;
};

enum chrome9_shader_verify_error {
   CHROME9_SHADER_VERIFY_OK,
   CHROME9_SHADER_VERIFY_NULL,
   CHROME9_SHADER_VERIFY_KEY,
   CHROME9_SHADER_VERIFY_COUNT,
   CHROME9_SHADER_VERIFY_CONTROL,
   CHROME9_SHADER_VERIFY_RESOURCE,
   CHROME9_SHADER_VERIFY_INSTRUCTION,
   CHROME9_SHADER_VERIFY_SLACK,
};

bool chrome9_compile_fs_offline(const struct chrome9_fs_key *key,
                                struct chrome9_ps_program *program);
bool chrome9_compile_vs_offline(enum chrome9_vs_key key,
                                struct chrome9_vs_program *program);

/* Encode the first straight-line instruction subset.  These functions
 * validate logical operands before packing native fields; compile_ps_ir also
 * supports the larger offline-only tranche while live submission stays
 * separately gated. */
bool chrome9_encode_ps_mov(const struct chrome9_ps_source *source,
                           uint32_t record[4]);
bool chrome9_encode_ps_mul(const struct chrome9_ps_source *source_a,
                           const struct chrome9_ps_source *source_c,
                           uint32_t record[4]);
bool chrome9_encode_ps_add(const struct chrome9_ps_source *source_a,
                           const struct chrome9_ps_source *source_b,
                           uint32_t record[4]);
bool chrome9_encode_ps_sub(const struct chrome9_ps_source *minuend,
                           const struct chrome9_ps_source *subtrahend,
                           uint32_t record[4]);
bool chrome9_encode_ps_mad(const struct chrome9_ps_source *source_a,
                           const struct chrome9_ps_source *source_c,
                           const struct chrome9_ps_source *source_b,
                           uint32_t record[4]);
bool chrome9_encode_ps_min(const struct chrome9_ps_source *source_a,
                           const struct chrome9_ps_source *source_c,
                           uint32_t record[4]);
bool chrome9_encode_ps_exp2(const struct chrome9_ps_source *source,
                            uint32_t record[4]);
bool chrome9_compile_ps_ir(const struct chrome9_ps_ir *ir,
                           struct chrome9_ps_program *program);

/* These verify a complete canonical object for a closed key.  They are not
 * validators for arbitrary native shader words. */
bool chrome9_verify_fs_offline(const struct chrome9_fs_key *key,
                               const struct chrome9_ps_program *program,
                               enum chrome9_shader_verify_error *error);
bool chrome9_verify_vs_offline(enum chrome9_vs_key key,
                               const struct chrome9_vs_program *program,
                               enum chrome9_shader_verify_error *error);

#endif
