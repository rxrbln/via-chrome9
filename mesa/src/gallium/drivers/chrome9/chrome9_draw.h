/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_DRAW_H
#define CHROME9_DRAW_H

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "drm-uapi/chrome9_drm.h"

struct chrome9_context;
struct draw_stage;

struct draw_stage *chrome9_draw_vbuf_stage(struct chrome9_context *ctx);
void chrome9_draw_flush_batch(struct chrome9_context *ctx);
void chrome9_draw_flush_gpu_batch(struct chrome9_context *ctx);
void chrome9_draw_begin_guarded_domain(struct chrome9_context *ctx);
bool chrome9_draw_end_guarded_domain(struct chrome9_context *ctx);
bool chrome9_fragment_unit_interval_value(uint32_t bits);
bool chrome9_generic_fragment_program(const struct chrome9_context *ctx);
bool chrome9_draw_needs_sw_textures(const struct chrome9_context *ctx);
bool chrome9_draw_tiled_surfaces(const struct chrome9_context *ctx);
bool chrome9_draw_blend_supported(const struct chrome9_context *ctx);

/* Pixel state of one hardware TCL draw, shared by all of its chunks. */
struct chrome9_tcl_fragment {
   struct drm_chrome9_draw_fragment_program draw;
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
   struct drm_chrome9_fragment_texture_state
      textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   /* Colour targets 1..3 (CAP2_COLOR_TARGETS), count <= 1 without. */
   struct drm_chrome9_draw_colors colors;
   /* Target, depth, colour target and texture resources behind the
    * handles above. */
   struct pipe_resource *resources[5 + DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   unsigned resource_count;
};

bool chrome9_draw_tcl_fragment(struct chrome9_context *ctx,
                               struct chrome9_tcl_fragment *fragment,
                               const char **why);
bool chrome9_draw_stencil_supported(const struct chrome9_context *ctx);
void chrome9_tcl_batch_flush(struct chrome9_context *ctx);
void chrome9_tcl_batch_flush_resource(struct chrome9_context *ctx,
                                      const struct pipe_resource *resource);
void chrome9_tcl_batch_destroy(struct chrome9_context *ctx);
bool chrome9_draw_tcl_submit(struct chrome9_context *ctx,
                             const struct chrome9_tcl_fragment *fragment,
                             struct drm_chrome9_draw_vertex_program *draw,
                             unsigned triangles,
                             struct pipe_resource *const *buffers,
                             unsigned buffer_count);

/* The kernel takes finite program constants only: NaN (e.g. folded from
 * an undefined value) becomes zero, infinities the largest finite value. */
static inline void
chrome9_finite_constants(uint32_t *words, unsigned count)
{
   for (unsigned i = 0; i < count; i++) {
      float value;

      memcpy(&value, &words[i], sizeof(value));
      if (isfinite(value))
         continue;
      value = isnan(value) ? 0.0f : copysignf(FLT_MAX, value);
      memcpy(&words[i], &value, sizeof(value));
   }
}

#endif
