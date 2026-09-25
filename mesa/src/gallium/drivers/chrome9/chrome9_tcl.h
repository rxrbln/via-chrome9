/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_TCL_H
#define CHROME9_TCL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm-uapi/chrome9_drm.h"

struct chrome9_context;
struct nir_shader;
struct pipe_draw_info;
struct pipe_draw_start_count_bias;

/* Static origin of one varying lane, for guarded fragment domains. */
enum chrome9_tcl_origin {
   CHROME9_TCL_ORIGIN_UNKNOWN = 0,
   CHROME9_TCL_ORIGIN_ZERO,
   CHROME9_TCL_ORIGIN_ONE,
   /* Saturated arithmetic: always within [0, 1]. */
   CHROME9_TCL_ORIGIN_UNIT,
   /* Input lane, resolved against the vertex format at draw time. */
   CHROME9_TCL_ORIGIN_INPUT,
};

/* Hardware static flow control the kernel accepts (CAP2_VERTEX_STATIC_FLOW). */
struct chrome9_tcl_flow {
   /* REP/IF nesting; zero keeps the straight-line requirement. */
   unsigned max_depth;
   /* IF on a computed value (kernel RE knob vs_flow_dynamic, rung F3). */
   bool dynamic;
   /* ARL and a0-relative CB0 reads (CAP2_VERTEX_RELATIVE). */
   bool relative;
};

/* Scalar expression over CB0 dwords and literals, evaluated per draw for
 * b# conditions and REP bounds; sources are earlier nodes. */
struct chrome9_tcl_expr {
   uint16_t op;
   uint8_t src[3];
   uint32_t value;
};

/* REP trip count: the first k at which the NIR break condition holds for
 * counter = init + k * step. */
struct chrome9_tcl_rep {
   uint16_t compare;
   bool counter_first;
   bool break_on_true;
   uint8_t bound;
   float init;
   float step;
};

/* A VS texture lookup at a vertex attribute: the CPU samples it into an
 * extra FLOAT4 input before the draw (the engine has no vertex texture
 * fetch). */
struct chrome9_tcl_prefetch {
   uint8_t unit;
   uint8_t sampler;
   /* Attribute input and its components holding s and t. */
   uint8_t input;
   uint8_t s, t;
};

/* Kernel vertex IR compiled from one VS for one clip-z convention. */
struct chrome9_tcl_program {
   bool compiled;
   bool supported;
   unsigned instruction_count;
   unsigned input_count;
   /* Inputs [attrib_count, attrib_count + prefetch_count) are prefetched
    * texture lookups. */
   unsigned attrib_count;
   unsigned prefetch_count;
   struct chrome9_tcl_prefetch prefetch[DRM_CHROME9_VERTEX_MAX_INPUTS];
   /* Rows [0, uniform_rows) come from CB0, then the immediates; with
    * constant_pad all move up by that many zero rows, and as many zero
    * rows follow, so clamped a0-relative reads stay inside the draw. */
   unsigned uniform_rows;
   unsigned immediate_count;
   unsigned constant_pad;
   /* Unsupported because of an indirect CB0 read. */
   bool indirect;
   uint32_t immediates[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4];
   struct drm_chrome9_vertex_instruction
      instructions[DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS];
   uint8_t origin[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   uint8_t origin_input[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   uint8_t origin_lane[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   /* Enabled planes the VS writes, and whether it writes BFC0. */
   uint8_t clip_written;
   bool back_primary;
   /* The VS writes POINT_SIZE (compiled with point_size). */
   bool point_size;
   /* Flow control: b# condition nodes and the REP loops of i0.. */
   unsigned expr_count;
   struct chrome9_tcl_expr *expr;
   unsigned bool_count;
   uint8_t bool_expr[DRM_CHROME9_VERTEX_MAX_BOOL_CONSTANTS];
   unsigned rep_count;
   struct chrome9_tcl_rep reps[DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS];
};

/* Primary colour routing: front only, hardware back-colour selection, or
 * the back colour as primary (the back-face pass of a culled split). */
enum chrome9_tcl_colour {
   CHROME9_TCL_COLOUR_FRONT,
   CHROME9_TCL_COLOUR_SELECT,
   CHROME9_TCL_COLOUR_BACK,
};

#define CHROME9_TCL_MAX_KEY 16
#define CHROME9_TCL_MAX_VARIANTS 32
/* Keyed compiles after which a shader, with flow control available, gets
 * one variant that reads its control uniforms at draw time instead. */
#define CHROME9_TCL_KEY_CHURN 8

/* One compiled form: clip-z convention, output routing and the folded
 * control uniforms. */
struct chrome9_tcl_variant {
   struct chrome9_tcl_variant *next;
   bool clip_halfz;
   enum chrome9_tcl_colour colour;
   uint8_t clip_enable;
   /* CHROME9_TCL_POINT_* */
   uint8_t point_size;
   bool unkeyed;
   uint32_t key[CHROME9_TCL_MAX_KEY];
   /* Fragment varying slots (chrome9_shader::varying_slot), all zero
    * without a fragment map. */
   uint8_t varying_slot[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
   struct chrome9_tcl_program program;
};

struct chrome9_tcl_shader {
   struct nir_shader *nir;
   /* CB0 dwords that feed branch or select conditions. */
   unsigned key_count;
   uint16_t key_dword[CHROME9_TCL_MAX_KEY];
   unsigned variant_count;
   unsigned keyed_compiles;
   struct chrome9_tcl_variant *variants;
};

struct chrome9_tcl_shader *chrome9_tcl_shader_create(const struct nir_shader *nir);
void chrome9_tcl_shader_destroy(struct chrome9_tcl_shader *shader);
bool chrome9_tcl_compile(const struct nir_shader *nir, bool clip_halfz,
                         enum chrome9_tcl_colour colour,
                         unsigned clip_enable, unsigned point,
                         unsigned key_count, const uint16_t *key_dword,
                         const uint32_t *key_value,
                         const struct chrome9_tcl_flow *flow,
                         const uint8_t *varying_slot,
                         struct chrome9_tcl_program *program);
/* chrome9_tcl_compile() point forms: rounded PSIZ output, centre clip. */
#define CHROME9_TCL_POINT_SIZE 1u
#define CHROME9_TCL_POINT_CLIP 2u
void chrome9_tcl_program_fini(struct chrome9_tcl_program *program);

/* DRAW_VERTEX_PROGRAM constant rows of a program. */
static inline unsigned
chrome9_tcl_constant_count(const struct chrome9_tcl_program *program)
{
   return 2 * program->constant_pad + program->uniform_rows +
          program->immediate_count;
}
bool chrome9_tcl_flow_constants(const struct chrome9_tcl_program *program,
                                const uint8_t *cb0, size_t size,
                                uint32_t *bools, uint8_t (*ints)[4]);
bool chrome9_tcl_draw(struct chrome9_context *ctx,
                      const struct pipe_draw_info *info,
                      const struct pipe_draw_start_count_bias *draw);
struct chrome9_tcl_state;
void chrome9_tcl_state_destroy(struct chrome9_tcl_state *state);

#endif
