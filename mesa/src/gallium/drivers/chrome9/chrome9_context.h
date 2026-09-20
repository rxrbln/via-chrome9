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
};

struct chrome9_shader {
   void *draw;
   void *nir;
   bool supported;
};

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
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];
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
void chrome9_context_render_target_written(struct chrome9_context *ctx);

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags);

#endif
