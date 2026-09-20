/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_CONTEXT_H
#define CHROME9_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"

struct draw_context;

struct chrome9_shader {
   void *draw;
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
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];

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

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags);

#endif
