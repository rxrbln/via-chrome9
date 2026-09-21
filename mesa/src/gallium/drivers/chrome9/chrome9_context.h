/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_CONTEXT_H
#define CHROME9_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"

struct draw_context;

enum chrome9_warning {
   CHROME9_WARN_SUBMIT = 1u << 0,
   CHROME9_WARN_DRAW_STATE = 1u << 1,
   CHROME9_WARN_INDIRECT = 1u << 2,
   CHROME9_WARN_CLEAR = 1u << 3,
   CHROME9_WARN_FRAMEBUFFER = 1u << 4,
   CHROME9_WARN_SAMPLER_VIEW_CREATE = 1u << 5,
   CHROME9_WARN_SAMPLER_VIEW_BIND = 1u << 6,
};

struct chrome9_shader {
   void *draw;
   void *nir;
   enum {
      CHROME9_FS_UNSUPPORTED,
      CHROME9_FS_PRIMARY_COLOR,
      CHROME9_FS_PRIMARY_GENERIC_RGBA,
      CHROME9_FS_PRIMARY_GENERIC_RGBX,
      CHROME9_FS_PRIMARY_SCALAR_RGBX,
      CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX,
      CHROME9_FS_CONSTANT_COLOR,
      CHROME9_FS_CONSTANT_MODULATE,
      CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE,
      CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE,
      CHROME9_FS_TEXTURE_REPLACE,
      CHROME9_FS_TEXTURE_RGBX,
      CHROME9_FS_TEXTURE_MODULATE,
      CHROME9_FS_TEXTURE_PROJECTED_MODULATE,
      CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE,
      CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA,
      CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA,
      CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE,
      CHROME9_FS_TEXTURE_CONSTANT_MODULATE,
      CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE,
      CHROME9_FS_TWO_TEXTURE_MODULATE,
      CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA,
      CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY,
   } fragment_program;
   bool supported;
};

struct chrome9_sampler_state {
   struct pipe_sampler_state base;
   uint8_t min_filter;
   uint8_t mag_filter;
   uint8_t mip_filter;
   uint8_t wrap_s;
   uint8_t wrap_t;
   bool supported;
};

struct chrome9_sampler_view {
   struct pipe_sampler_view base;
   /* Unguarded external images remain the Gallium-visible texture.  The
    * hardware samples this guarded, draw-refreshed copy instead. */
   struct pipe_resource *shadow;
};

static inline struct chrome9_sampler_view *
chrome9_sampler_view(struct pipe_sampler_view *view)
{
   return (struct chrome9_sampler_view *)view;
}

static inline struct pipe_resource *
chrome9_sampler_view_resource(struct pipe_sampler_view *view)
{
   struct chrome9_sampler_view *cview = chrome9_sampler_view(view);

   return cview->shadow ? cview->shadow : view->texture;
}

struct chrome9_vertex_elements {
   unsigned count;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

struct chrome9_context {
   struct pipe_context base;
   struct draw_context *draw;

   struct pipe_framebuffer_state framebuffer;
   /* DRAW_TRIANGLE requires a native guarded BO.  Imported DRI3 images use
    * this guarded shadow and are copied back on flush. */
   struct pipe_resource *render_target;
   bool render_target_dirty;
   bool draw_triangle_d16;
   bool draw_triangle_d16_blend;
   bool depth_func_supported;
   bool depth_equal_supported;
   bool depth_always_supported;
   bool depth_read_only_supported;
   bool draw_triangle_batch;
   bool draw_triangle_d16_rhw_batch;
   bool draw_textured_triangle;
   bool draw_textured_triangle_d16;
   bool draw_textured_triangle_d16_blend;
   bool textured_d16_bgra8888;
   bool textured_d16_source_over;
   bool draw_two_textured_triangle;
   bool draw_two_textured_triangle_d16;
   bool draw_two_textured_triangle_d16_blend;
   bool draw_triangle_blend;
   bool draw_textured_triangle_blend;
   bool blend_bgra8888;
   bool blend_extended_factors;
   bool alpha_test_supported;
   bool scissor_supported;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_constant_buffer fs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   struct chrome9_sampler_state *sampler_states[PIPE_MAX_SAMPLERS];
   unsigned num_sampler_states;
   struct pipe_sampler_view *sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   unsigned num_sampler_views;

   struct chrome9_shader *vs;
   struct chrome9_shader *fs;
   struct pipe_blend_state *blend;
   struct pipe_depth_stencil_alpha_state *dsa;
   struct pipe_rasterizer_state *rasterizer;
   struct chrome9_vertex_elements *vertex_elements;
   struct pipe_scissor_state scissor;
   bool scissor_valid;

   uint64_t submitted_triangles;
   uint64_t rejected_triangles;
   uint64_t draw_limit;
   bool debug_draws;
   bool draw_limit_reported;
   uint32_t warnings;
};

static inline struct chrome9_context *
chrome9_context(struct pipe_context *pipe)
{
   return (struct chrome9_context *)pipe;
}

struct chrome9_resource;
struct chrome9_resource *
chrome9_context_render_target(struct chrome9_context *ctx);
struct chrome9_resource *
chrome9_context_depth_target(struct chrome9_context *ctx);
void chrome9_context_render_target_written(struct chrome9_context *ctx);

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags);

#endif
