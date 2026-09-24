/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_TCL_H
#define CHROME9_TCL_H

#include <stdbool.h>
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

/* Kernel vertex IR compiled from one VS for one clip-z convention. */
struct chrome9_tcl_program {
   bool compiled;
   bool supported;
   unsigned instruction_count;
   unsigned input_count;
   /* Rows [0, uniform_rows) come from CB0, then the immediates. */
   unsigned uniform_rows;
   unsigned immediate_count;
   uint32_t immediates[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4];
   struct drm_chrome9_vertex_instruction
      instructions[DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS];
   uint8_t origin[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   uint8_t origin_input[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   uint8_t origin_lane[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   /* Enabled planes the VS writes, and whether it writes BFC0. */
   uint8_t clip_written;
   bool back_primary;
};

#define CHROME9_TCL_MAX_KEY 16
#define CHROME9_TCL_MAX_VARIANTS 32

/* One compiled form: clip-z convention, output routing and the folded
 * control uniforms. */
struct chrome9_tcl_variant {
   struct chrome9_tcl_variant *next;
   bool clip_halfz;
   bool twoside;
   uint8_t clip_enable;
   uint32_t key[CHROME9_TCL_MAX_KEY];
   struct chrome9_tcl_program program;
};

struct chrome9_tcl_shader {
   struct nir_shader *nir;
   /* CB0 dwords that feed branch or select conditions. */
   unsigned key_count;
   uint16_t key_dword[CHROME9_TCL_MAX_KEY];
   unsigned variant_count;
   struct chrome9_tcl_variant *variants;
};

struct chrome9_tcl_shader *chrome9_tcl_shader_create(const struct nir_shader *nir);
void chrome9_tcl_shader_destroy(struct chrome9_tcl_shader *shader);
bool chrome9_tcl_compile(const struct nir_shader *nir, bool clip_halfz,
                         bool twoside, unsigned clip_enable,
                         unsigned key_count, const uint16_t *key_dword,
                         const uint32_t *key_value,
                         struct chrome9_tcl_program *program);
bool chrome9_tcl_draw(struct chrome9_context *ctx,
                      const struct pipe_draw_info *info,
                      const struct pipe_draw_start_count_bias *draw);

#endif
