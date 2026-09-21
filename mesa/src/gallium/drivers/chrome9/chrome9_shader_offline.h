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
   CHROME9_FS_LEAF_C0,
   CHROME9_FS_LEAF_C1,
   CHROME9_FS_LEAF_TEX0,
   CHROME9_FS_LEAF_TEX1,
};

enum chrome9_fs_op {
   CHROME9_FS_OP_MOV,
   CHROME9_FS_OP_MUL,
};

/* Canonical output expression after NIR scalarization, copy propagation and
 * DCE.  This is deliberately not a general shader IR: an unrepresented field
 * or operation must be rejected by the NIR-facing classifier. */
struct chrome9_fs_key {
   enum chrome9_fs_op op;
   enum chrome9_fs_leaf src[2];
};

struct chrome9_ps_program {
   uint32_t control[12];       /* complete A90..A9b words */
   uint32_t alu[CHROME9_PS_MAX_ALU][4];
   uint32_t tau[CHROME9_PS_MAX_TAU];
   uint32_t constant_mask;     /* c0..c31 */
   uint16_t varying_mask;      /* bit 0 is primary color v0 */
   uint16_t sampler_mask;      /* s0..s15 */
   uint8_t alu_count;
   uint8_t tau_count;
};

enum chrome9_vs_key {
   CHROME9_VS_POSITION,
   CHROME9_VS_POSITION_TEX0_XY,
   CHROME9_VS_POSITION_TEX0_TEX1_XY,
   CHROME9_VS_MVP_COLOR_TEX0_XY,
};

struct chrome9_vs_program {
   uint32_t instruction[CHROME9_VS_POLICY_MAX][4];
   uint32_t output_metadata;
   uint32_t constant_mask[8];  /* c0..c255 */
   uint16_t input_mask;        /* v0..v15 */
   uint16_t output_count;      /* A62 low nine bits */
   uint16_t active_count;      /* A60 is 60800000|(active_count-1) */
   uint16_t upload_count;
};

bool chrome9_compile_fs_offline(const struct chrome9_fs_key *key,
                                struct chrome9_ps_program *program);
bool chrome9_compile_vs_offline(enum chrome9_vs_key key,
                                struct chrome9_vs_program *program);

#endif
