/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_DRAW_H
#define CHROME9_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "drm-uapi/chrome9_drm.h"

struct chrome9_context;
struct draw_stage;

struct draw_stage *chrome9_draw_vbuf_stage(struct chrome9_context *ctx);
void chrome9_draw_flush_batch(struct chrome9_context *ctx);
void chrome9_draw_begin_guarded_domain(struct chrome9_context *ctx);
void chrome9_draw_end_guarded_domain(struct chrome9_context *ctx);
bool chrome9_fragment_unit_interval_value(uint32_t bits);

/* Pixel state of one hardware TCL draw, shared by all of its chunks. */
struct chrome9_tcl_fragment {
   struct drm_chrome9_draw_fragment_program draw;
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
   struct drm_chrome9_fragment_texture_state
      textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
};

bool chrome9_draw_tcl_fragment(struct chrome9_context *ctx,
                               struct chrome9_tcl_fragment *fragment,
                               const char **why);
bool chrome9_draw_tcl_submit(struct chrome9_context *ctx,
                             const struct chrome9_tcl_fragment *fragment,
                             struct drm_chrome9_draw_vertex_program *draw,
                             unsigned triangles);

#endif
