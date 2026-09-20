/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_public.h"
#include "chrome9_context.h"
#include "chrome9_resource.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_sampler.h"

struct smoke_vertex {
   float position[4];
   float color[4];
};

struct smoke_texture_vertex {
   float position[4];
   float color[4];
   float texcoord[2];
};

#define CHROME9_SMOKE_GUARD_WORD 0x13579bdfU

static bool
smoke_guard_pages_match(const struct chrome9_resource *resource)
{
   const uint32_t *first = resource->bo.map;
   const uint32_t *last;
   size_t page_size = resource->data_offset;
   size_t aligned_data_size;

   if (!page_size || (page_size & 3) || resource->data_size > SIZE_MAX ||
       resource->bo.size < 2 * page_size)
      return false;
   aligned_data_size = ALIGN_POT(resource->data_size, page_size);
   if (aligned_data_size < resource->data_size ||
       aligned_data_size > resource->bo.size - 2 * page_size)
      return false;
   last = (const uint32_t *)((const uint8_t *)resource->bo.map + page_size +
                            aligned_data_size);
   for (size_t i = 0; i < page_size / sizeof(uint32_t); i++)
      if (first[i] != CHROME9_SMOKE_GUARD_WORD ||
          last[i] != CHROME9_SMOKE_GUARD_WORD)
         return false;
   return true;
}

static bool
smoke_bytes_match(const void *data, size_t size, uint8_t expected)
{
   const uint8_t *bytes = data;

   for (size_t i = 0; i < size; i++)
      if (bytes[i] != expected)
         return false;
   return true;
}

static void *
make_texture_vertex_shader(struct pipe_screen *screen,
                           struct pipe_context *context, bool emit_color)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      emit_color ? "chrome9-one-texture-color-vertex" :
                   "chrome9-one-texture-replace-vertex");
   nir_def *position, *texcoord;
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   position = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0),
                             .io_semantics.location = VERT_ATTRIB_POS);
   texcoord = nir_load_input(&b, 2, 32, nir_imm_int(&b, 0),
                             .io_semantics.location = VERT_ATTRIB_TEX0);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   if (emit_color) {
      nir_def *color = nir_load_input(
         &b, 4, 32, nir_imm_int(&b, 0),
         .io_semantics.location = VERT_ATTRIB_COLOR0);

      nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                       .io_semantics.location = VARYING_SLOT_COL0);
   }
   nir_store_output(&b, texcoord, nir_imm_int(&b, 0), .write_mask = 0x3,
                    .io_semantics.location = VARYING_SLOT_TEX0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_texture_fragment_shader(struct pipe_screen *screen,
                             struct pipe_context *context, bool modulate)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      modulate ? "chrome9-texture-modulate" : "chrome9-texture-replace");
   nir_def *barycentric, *texcoord, *result;
   nir_tex_instr *tex;
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   barycentric = nir_load_barycentric_pixel(&b, 32);
   texcoord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_TEX0);
   tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, texcoord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   result = &tex->def;
   if (modulate) {
      nir_def *color = nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_COL0);

      result = nir_fmul(&b, result, color);
   }
   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_ff_vertex_shader(struct pipe_screen *screen,
                      struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-glxgears-vertex-color");

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   /* Fixed-function lighting similarly produces a clamped primary color. */
   color = nir_fsat(&b, color);
   nir_store_output(&b, position, nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, color, nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_COL0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_ff_primary_color_fs(struct pipe_screen *screen,
                         struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glxgears-primary-color");

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   nir_store_output(&b, color, nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static bool
run_pipe_triangle(struct pipe_screen *screen, struct pipe_resource *resource,
                  struct pipe_resource *depth_resource)
{
   static const struct smoke_vertex vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 0, 0, 1 } },
      { { -1,  1, 0, 1 }, { 0, 1, 0, 1 } },
      { {  1, -1, 0, 1 }, { 0, 0, 1, 1 } },
      { {  1,  1, 0, 1 }, { 0, 0, 1, 1 } },
   };
   static const struct smoke_vertex far_vertices[] = {
      { { -1, -1,  0.5, 1 }, { 0, 1, 0, 1 } },
      { { -1,  1,  0.5, 1 }, { 0, 1, 0, 1 } },
      { {  1, -1,  0.5, 1 }, { 0, 1, 0, 1 } },
      { {  1,  1,  0.5, 1 }, { 0, 1, 0, 1 } },
   };
   static const struct smoke_vertex near_vertices[] = {
      { { -1, -1, -0.5, 1 }, { 1, 0, 0, 1 } },
      { { -1,  1, -0.5, 1 }, { 1, 0, 0, 1 } },
      { {  1, -1, -0.5, 1 }, { 1, 0, 0, 1 } },
      { {  1,  1, -0.5, 1 }, { 1, 0, 0, 1 } },
   };
   static const struct smoke_vertex rejected_vertices[] = {
      { { -1, -1, 0, 1 }, { 0, 0, 1, 1 } },
      { { -1,  1, 0, 1 }, { 0, 0, 1, 1 } },
      { {  1, -1, 0, 1 }, { 0, 0, 1, 1 } },
      { {  1,  1, 0, 1 }, { 0, 0, 1, 1 } },
   };
   static const struct smoke_vertex blend_vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 0, 0, 0.5 } },
      { { -1,  1, 0, 1 }, { 1, 0, 0, 0.5 } },
      { {  1, -1, 0, 1 }, { 1, 0, 0, 0.5 } },
      { {  1,  1, 0, 1 }, { 1, 0, 0, 0.5 } },
   };
   struct pipe_context *context = NULL;
   struct pipe_blend_state blend = { 0 };
   struct pipe_blend_state source_over = { 0 };
   struct pipe_blend_state alpha_only = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[2] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 257,
      .height = 129,
      .nr_cbufs = 1,
      .cbufs[0] = {
         .format = PIPE_FORMAT_B8G8R8X8_UNORM,
         .texture = resource,
      },
   };
   struct pipe_viewport_state viewport = {
      .scale = { 128.5, 64.5, 0.5 },
      .translate = { 128.5, 64.5, 0.5 },
   };
   struct pipe_scissor_state scissor = {
      .minx = 0,
      .miny = 0,
      .maxx = 257,
      .maxy = 129,
   };
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP,
      .index_bounds_valid = true,
      .instance_count = 1,
      .min_index = 0,
      .max_index = 3,
   };
   struct pipe_draw_start_count_bias draw = {
      .start = 0,
      .count = 4,
   };
   union pipe_color_union clear_color = { 0 };
   union pipe_color_union green = { .f = { 0, 1, 0, 0 } };
   union pipe_color_union alpha_mask_base = {
      .f = { 0x33 / 255.0f, 0x22 / 255.0f,
             0x11 / 255.0f, 0x44 / 255.0f },
   };
   struct chrome9_resource *surface = chrome9_resource(resource);
   struct chrome9_resource *draw_surface;
   uint32_t *pixels;
   size_t changed = 0;
   uint16_t sample_depth = 0;
   void *blend_handle = NULL;
   void *source_over_handle = NULL;
   void *alpha_only_handle = NULL;
   void *dsa_handle = NULL;
   void *rasterizer_handle = NULL;
   void *elements_handle = NULL;
   void *vs = NULL;
   void *fs = NULL;
   bool passed = false;

   context = screen->context_create(screen, NULL, 0);
   if (!context)
      goto out;
   framebuffer.cbufs[0].format = resource->format;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   source_over.rt[0].blend_enable = true;
   source_over.rt[0].rgb_func = PIPE_BLEND_ADD;
   source_over.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA;
   source_over.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   source_over.rt[0].alpha_func = PIPE_BLEND_ADD;
   source_over.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   source_over.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   source_over.rt[0].colormask = PIPE_MASK_RGBA;
   if (!depth_resource &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND))
      source_over_handle =
         context->create_blend_state(context, &source_over);
   if (!depth_resource &&
       resource->format == PIPE_FORMAT_B8G8R8A8_UNORM) {
      alpha_only.rt[0].colormask = PIPE_MASK_A;
      alpha_only_handle = context->create_blend_state(context, &alpha_only);
   }
   if (depth_resource) {
      dsa.depth_enabled = true;
      dsa.depth_writemask = true;
      dsa.depth_func = PIPE_FUNC_LESS;
      framebuffer.zsbuf.format = PIPE_FORMAT_Z16_UNORM;
      framebuffer.zsbuf.texture = depth_resource;
      vertex_buffer.buffer.user = far_vertices;
   }
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer.half_pixel_center = true;
   rasterizer.bottom_edge_rule = true;
   rasterizer.depth_clip_near = true;
   rasterizer.depth_clip_far = true;
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[0].src_stride = sizeof(struct smoke_vertex);
   elements[1].src_offset = offsetof(struct smoke_vertex, color);
   elements[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_vertex);
   elements_handle = context->create_vertex_elements_state(context, 2,
                                                            elements);
   vs = make_ff_vertex_shader(screen, context);
   fs = make_ff_primary_color_fs(screen, context);
   if (!blend_handle ||
       (!depth_resource &&
        chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND) &&
        !source_over_handle) ||
       (!depth_resource &&
        resource->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
        !alpha_only_handle) ||
       !dsa_handle || !rasterizer_handle ||
       !elements_handle || !vs || !fs)
      goto out;

   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_framebuffer_state(context, &framebuffer);
   draw_surface = chrome9_context_render_target(chrome9_context(context));
   if (!draw_surface)
      goto out;
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   context->clear(context, PIPE_CLEAR_COLOR0 |
                             (depth_resource ? PIPE_CLEAR_DEPTH : 0),
                  PIPE_MASK_RGBA, 0xff, NULL,
                  &clear_color, 1.0, 0);
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   if (depth_resource) {
      vertex_buffer.buffer.user = near_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      vertex_buffer.buffer.user = rejected_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   }
   if (!context->flush_resource)
      goto out;
   context->flush_resource(context, resource);
   context->flush(context, NULL, 0);

   pixels = (uint32_t *)((uint8_t *)surface->bo.map + surface->data_offset);
   for (unsigned y = 0; y < resource->height0; y++)
      for (unsigned x = 0; x < resource->width0; x++)
         changed += pixels[(size_t)y * surface->stride / 4 + x] != 0;
   passed = changed >= 30000 && changed <= 257U * 129U;
   if (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM)
      passed = passed &&
               (pixels[32 * (surface->stride / 4) + 192] >> 24) == 0xff;
   if (depth_resource) {
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);

      sample_depth = depth_pixels[32 * (depth->stride / 2) + 192];
      passed = passed &&
               (pixels[32 * (surface->stride / 4) + 192] & 0xffffff) ==
                  0xff0000 &&
               sample_depth >= 0x3ffe && sample_depth <= 0x4001;
   } else if (passed && source_over_handle) {
      uint32_t pixel;
      unsigned red, green_channel;

      context->bind_blend_state(context, source_over_handle);
      vertex_buffer.buffer.user = blend_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &green, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixel = pixels[32 * (surface->stride / 4) + 192];
      red = (pixel >> 16) & 0xff;
      green_channel = pixel >> 8 & 0xff;
      passed = !(pixel & 0xff) && red >= 0x7f && red <= 0x80 &&
               green_channel >= 0x7f && green_channel <= 0x80 &&
               (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                ((pixel >> 24) >= 0x7f && (pixel >> 24) <= 0x80)) &&
               smoke_guard_pages_match(draw_surface);
      if (passed)
         printf("Gallium source-over blend passed: sample=%08x\n", pixel);
      else
         fprintf(stderr,
                 "Gallium source-over blend mismatch: sample=%08x\n",
                 pixel);
      if (passed && alpha_only_handle) {
         context->bind_blend_state(context, alpha_only_handle);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &alpha_mask_base, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel == 0x7f332211 || pixel == 0x80332211) &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium BGRA alpha-only mask passed: sample=%08x\n",
                   pixel);
         else
            fprintf(stderr,
                    "Gallium BGRA alpha-only mask mismatch: sample=%08x\n",
                    pixel);
      }
   }
   if (!passed)
      fprintf(stderr,
              "Gallium quad-strip coverage/depth mismatch: %zu sample=%08x/%04x\n",
              changed, pixels[32 * (surface->stride / 4) + 192],
              sample_depth);
   else if (depth_resource)
      printf("Gallium D16 quad strip passed: changed=%zu sample=%08x/%04x\n",
             changed, pixels[32 * (surface->stride / 4) + 192],
             sample_depth);
   else
      printf("Gallium draw/vbuf quad strip passed: changed=%zu sample=%08x\n",
             changed, pixels[32 * (surface->stride / 4) + 192]);

out:
   if (context) {
      if (vs)
         context->delete_vs_state(context, vs);
      if (fs)
         context->delete_fs_state(context, fs);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      if (source_over_handle)
         context->delete_blend_state(context, source_over_handle);
      if (alpha_only_handle)
         context->delete_blend_state(context, alpha_only_handle);
      context->destroy(context);
   }
   return passed;
}

static bool
run_pipe_texture(struct pipe_screen *screen)
{
   static const struct smoke_texture_vertex white_vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0 } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 1, 0 } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 1, 1 } },
   };
   static const struct smoke_texture_vertex red_vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 0, 0, 1 }, { 0, 0 } },
      { { -1,  1, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 0, 0, 1 }, { 1, 0 } },
      { {  1,  1, 0, 1 }, { 1, 0, 0, 1 }, { 1, 1 } },
   };
   static const struct smoke_texture_vertex rhw_replace_vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0.5 } },
      { {  4, -4, 0, 4 }, { 1, 1, 1, 1 }, { 1, 0.5 } },
      { {  4,  4, 0, 4 }, { 1, 1, 1, 1 }, { 1, 0.5 } },
   };
   static const struct smoke_texture_vertex rhw_modulate_vertices[] = {
      { { -1, -1, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0.5 } },
      { {  4, -4, 0, 4 }, { 1, 0, 0, 1 }, { 1, 0.5 } },
      { {  4,  4, 0, 4 }, { 1, 0, 0, 1 }, { 1, 0.5 } },
   };
   struct pipe_resource target_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource texture_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = 2, .height0 = 2, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource bgra_target_templ = target_templ;
   struct pipe_resource rhw_target_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource rhw_texture_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 8, .height0 = 1, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *target = NULL, *texture = NULL;
   struct pipe_resource *bgra_target = NULL;
   struct pipe_resource *rhw_target = NULL, *rhw_texture = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_templ, rhw_view_templ;
   struct pipe_sampler_view *view = NULL, *rhw_view = NULL;
   struct pipe_sampler_state sampler = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .compare_mode = PIPE_TEX_COMPARE_NONE,
      .max_anisotropy = 1,
      .max_lod = 1000.0f,
      .reduction_mode = PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE,
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_blend_state source_over = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[3] = { 0 };
   struct pipe_vertex_element replace_elements[2] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = white_vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 32, .height = 32, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8X8_UNORM },
   };
   struct pipe_viewport_state viewport = {
      .scale = { 16, 16, 0.5 }, .translate = { 16, 16, 0.5 },
   };
   struct pipe_scissor_state scissor = {
      .maxx = 32, .maxy = 32,
   };
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   struct pipe_draw_start_count_bias draw = { .count = 4 };
   union pipe_color_union clear = { 0 };
   union pipe_color_union green = { .f = { 0, 1, 0, 0 } };
   struct chrome9_resource *ctarget, *ctexture, *cbgra_target = NULL;
   struct chrome9_resource *crhw_target, *crhw_texture;
   struct chrome9_context *ccontext;
   void *blend_handle = NULL, *dsa_handle = NULL, *rasterizer_handle = NULL;
   void *source_over_handle = NULL;
   void *elements_handle = NULL, *replace_elements_handle = NULL;
   void *sampler_handle = NULL;
   void *replace_vs = NULL, *modulate_vs = NULL;
   void *replace = NULL, *modulate = NULL;
   void *samplers[1];
   uint32_t source_copy[4], *source, *pixels;
   bool bgra_render;
   bool passed = false;

   bgra_target_templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   bgra_render = screen->is_format_supported(
      screen, bgra_target_templ.format, bgra_target_templ.target, 1, 1,
      bgra_target_templ.bind);
   target = screen->resource_create(screen, &target_templ);
   texture = screen->resource_create(screen, &texture_templ);
   if (bgra_render)
      bgra_target = screen->resource_create(screen, &bgra_target_templ);
   rhw_target = screen->resource_create(screen, &rhw_target_templ);
   rhw_texture = screen->resource_create(screen, &rhw_texture_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!target || !texture || (bgra_render && !bgra_target) ||
       !rhw_target || !rhw_texture || !context)
      goto out;
   ccontext = chrome9_context(context);
   ctarget = chrome9_resource(target);
   ctexture = chrome9_resource(texture);
   if (bgra_target)
      cbgra_target = chrome9_resource(bgra_target);
   crhw_target = chrome9_resource(rhw_target);
   crhw_texture = chrome9_resource(rhw_texture);
   memset(ctarget->bo.map, 0x5a, ctarget->bo.size);
   memset(ctexture->bo.map, 0xa5, ctexture->bo.size);
   if (cbgra_target)
      memset(cbgra_target->bo.map, 0x5a, cbgra_target->bo.size);
   source = (uint32_t *)((uint8_t *)ctexture->bo.map + ctexture->data_offset);
   source[0] = 0x00ff0000;
   source[1] = 0x0000ff00;
   source[ctexture->stride / 4] = 0x000000ff;
   source[ctexture->stride / 4 + 1] = 0x00ffffff;
   source_copy[0] = source[0]; source_copy[1] = source[1];
   source_copy[2] = source[ctexture->stride / 4];
   source_copy[3] = source[ctexture->stride / 4 + 1];

   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   source_over.rt[0].blend_enable = true;
   source_over.rt[0].rgb_func = PIPE_BLEND_ADD;
   source_over.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA;
   source_over.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   source_over.rt[0].alpha_func = PIPE_BLEND_ADD;
   source_over.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   source_over.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   source_over.rt[0].colormask = PIPE_MASK_RGBA;
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND))
      source_over_handle =
         context->create_blend_state(context, &source_over);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer.half_pixel_center = true;
   rasterizer.bottom_edge_rule = true;
   rasterizer.depth_clip_near = true;
   rasterizer.depth_clip_far = true;
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_texture_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[0].src_stride = sizeof(struct smoke_texture_vertex);
   elements[1].src_offset = offsetof(struct smoke_texture_vertex, color);
   elements[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_texture_vertex);
   elements[2].src_offset = offsetof(struct smoke_texture_vertex, texcoord);
   elements[2].src_format = PIPE_FORMAT_R32G32_FLOAT;
   elements[2].src_stride = sizeof(struct smoke_texture_vertex);
   elements_handle = context->create_vertex_elements_state(context, 3,
                                                            elements);
   replace_elements[0] = elements[0];
   replace_elements[1] = elements[2];
   replace_elements_handle = context->create_vertex_elements_state(
      context, 2, replace_elements);
   sampler_handle = context->create_sampler_state(context, &sampler);
   u_sampler_view_default_template(&view_templ, texture, texture->format);
   view = context->create_sampler_view(context, texture, &view_templ);
   u_sampler_view_default_template(&rhw_view_templ, rhw_texture,
                                   rhw_texture->format);
   rhw_view = context->create_sampler_view(context, rhw_texture,
                                           &rhw_view_templ);
   replace_vs = make_texture_vertex_shader(screen, context, false);
   modulate_vs = make_texture_vertex_shader(screen, context, true);
   replace = make_texture_fragment_shader(screen, context, false);
   modulate = make_texture_fragment_shader(screen, context, true);
   if (!blend_handle ||
       (chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND) &&
        !source_over_handle) ||
       !dsa_handle || !rasterizer_handle ||
       !elements_handle || !replace_elements_handle || !sampler_handle ||
       !view || !rhw_view ||
       !replace_vs ||
       !modulate_vs || !replace || !modulate) {
      fprintf(stderr,
              "texture setup failed: target=%p texture=%p context=%p "
              "blend=%p dsa=%p rasterizer=%p elements=%p "
              "replace_elements=%p sampler=%p "
              "view=%p rhw_view=%p replace_vs=%p modulate_vs=%p "
              "replace=%p modulate=%p\n",
              (void *)target, (void *)texture, (void *)context, blend_handle,
              dsa_handle, rasterizer_handle, elements_handle,
              replace_elements_handle, sampler_handle,
              (void *)view, (void *)rhw_view, replace_vs, modulate_vs,
              replace, modulate);
      goto out;
   }

   framebuffer.cbufs[0].texture = target;
   samplers[0] = sampler_handle;
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                samplers);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                              &view);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_framebuffer_state(context, &framebuffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);

   for (unsigned program = 0; program < 2; program++) {
      const uint32_t expect[2][4] = {
         { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff },
         { 0xff0000, 0x000000, 0x000000, 0xff0000 },
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint32_t sampled[4];
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      vertex_buffer.buffer.user = program ? red_vertices : white_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->bind_vertex_elements_state(
         context, program ? elements_handle : replace_elements_handle);
      context->bind_vs_state(context, program ? modulate_vs : replace_vs);
      context->bind_fs_state(context, program ? modulate : replace);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixels = (uint32_t *)((uint8_t *)ctarget->bo.map +
                            ctarget->data_offset);
      for (unsigned i = 0; i < 4; i++) {
         sampled[i] =
            pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] & 0xffffff;
         pixels_match &= sampled[i] == expect[program][i];
      }
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected) {
         fprintf(stderr,
                 "texture %s mismatch: fs=%u submitted=%" PRIu64
                 "(+%" PRIu64 ") rejected=%" PRIu64 "(+%" PRIu64 ") "
                 "samples=%06x,%06x,%06x,%06x "
                 "expected=%06x,%06x,%06x,%06x\n",
                 program ? "MODULATE" : "REPLACE",
                 ((struct chrome9_shader *)(program ? modulate : replace))->
                    fragment_program,
                 ccontext->submitted_triangles,
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles,
                 ccontext->rejected_triangles - rejected,
                 sampled[0], sampled[1], sampled[2], sampled[3],
                 expect[program][0], expect[program][1], expect[program][2],
                 expect[program][3]);
         goto out;
      }
   }
   bool source_pixels_match =
      source[0] == source_copy[0] && source[1] == source_copy[1] &&
      source[ctexture->stride / 4] == source_copy[2] &&
      source[ctexture->stride / 4 + 1] == source_copy[3];
   bool source_padding_match = true;
   const uint8_t *texture_data =
      (const uint8_t *)ctexture->bo.map + ctexture->data_offset;
   size_t texture_aligned_size =
      ALIGN_POT(ctexture->data_size, ctexture->data_offset);

   for (unsigned y = 0; y < texture->height0; y++)
      source_padding_match &= smoke_bytes_match(
         texture_data + (size_t)y * ctexture->stride + texture->width0 * 4,
         ctexture->stride - texture->width0 * 4, 0xa5);
   source_padding_match &= smoke_bytes_match(
      texture_data + ctexture->data_size,
      texture_aligned_size - ctexture->data_size, 0xa5);
   bool target_guards_match = smoke_guard_pages_match(ctarget);
   bool texture_guards_match = smoke_guard_pages_match(ctexture);

   if (!source_pixels_match || !source_padding_match ||
       !target_guards_match || !texture_guards_match) {
      fprintf(stderr,
              "texture preservation mismatch: pixels=%u padding=%u "
              "target_guards=%u texture_guards=%u\n",
              source_pixels_match, source_padding_match,
              target_guards_match, texture_guards_match);
      goto out;
   }
   puts("Gallium texture REPLACE/MODULATE and guards passed");

   if (source_over_handle) {
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      uint32_t pixel;

      source[0] = source[1] = source[ctexture->stride / 4] =
         source[ctexture->stride / 4 + 1] = 0x80ff0000;
      vertex_buffer.buffer.user = white_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->bind_vertex_elements_state(context, replace_elements_handle);
      context->bind_vs_state(context, replace_vs);
      context->bind_fs_state(context, replace);
      context->bind_blend_state(context, source_over_handle);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &green, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixels = (uint32_t *)((uint8_t *)ctarget->bo.map +
                            ctarget->data_offset);
      pixel = pixels[8 * (ctarget->stride / 4) + 8] & 0xffffff;
      if (pixel != 0x00807f00 ||
          ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          source[0] != 0x80ff0000 || source[1] != 0x80ff0000 ||
          source[ctexture->stride / 4] != 0x80ff0000 ||
          source[ctexture->stride / 4 + 1] != 0x80ff0000 ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctexture)) {
         fprintf(stderr,
                 "Gallium textured source-over mismatch: sample=%08x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 pixel, ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         goto out;
      }
      puts("Gallium BGRA REPLACE source-over blend passed");
      if (bgra_target) {
         uint32_t *bgra_pixels;

         submitted = ccontext->submitted_triangles;
         rejected = ccontext->rejected_triangles;
         framebuffer.cbufs[0].format = PIPE_FORMAT_B8G8R8A8_UNORM;
         framebuffer.cbufs[0].texture = bgra_target;
         context->set_framebuffer_state(context, &framebuffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &green, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         bgra_pixels = (uint32_t *)((uint8_t *)cbgra_target->bo.map +
                                    cbgra_target->data_offset);
         pixel = bgra_pixels[8 * (cbgra_target->stride / 4) + 8];
         if (pixel != 0x80807f00 ||
             ccontext->submitted_triangles != submitted + 2 ||
             ccontext->rejected_triangles != rejected ||
             !smoke_guard_pages_match(cbgra_target) ||
             !smoke_guard_pages_match(ctexture)) {
            fprintf(stderr,
                    "Gallium textured BGRA target mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel, ccontext->submitted_triangles - submitted,
                    ccontext->rejected_triangles - rejected);
            goto out;
         }
         puts("Gallium textured BGRA render target passed");
      }
      context->bind_blend_state(context, blend_handle);
   }

   static const uint32_t rhw_texels[8] = {
      0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff,
      0x00ff00ff, 0x0000ffff, 0x00ffff00, 0x00808080,
   };
   const unsigned rhw_y[4] = { 8, 16, 24, 40 };
   uint32_t *rhw_source = (uint32_t *)
      ((uint8_t *)crhw_texture->bo.map + crhw_texture->data_offset);
   uint32_t *rhw_pixels = (uint32_t *)
      ((uint8_t *)crhw_target->bo.map + crhw_target->data_offset);
   struct pipe_viewport_state rhw_viewport = {
      .scale = { 32, 32, 0 }, .translate = { 31.5, 31.5, 0 },
   };
   struct pipe_scissor_state rhw_scissor = { .maxx = 64, .maxy = 64 };
   struct pipe_draw_info rhw_info = {
      .mode = MESA_PRIM_TRIANGLES, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 2,
   };
   struct pipe_draw_start_count_bias rhw_draw = { .count = 3 };

   memset(crhw_target->bo.map, 0x5a, crhw_target->bo.size);
   memset(crhw_texture->bo.map, 0xa5, crhw_texture->bo.size);
   memcpy(rhw_source, rhw_texels, sizeof(rhw_texels));
   framebuffer.width = framebuffer.height = 64;
   framebuffer.cbufs[0].format = PIPE_FORMAT_B8G8R8X8_UNORM;
   framebuffer.cbufs[0].texture = rhw_target;
   context->set_framebuffer_state(context, &framebuffer);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                              &rhw_view);
   context->set_viewport_states(context, 0, 1, &rhw_viewport);
   context->set_scissor_states(context, 0, 1, &rhw_scissor);
   vertex_buffer.buffer.user = rhw_replace_vertices;
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->bind_vertex_elements_state(context, replace_elements_handle);
   context->bind_vs_state(context, replace_vs);
   context->bind_fs_state(context, replace);
   context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                  &clear, 1.0, 0);
   uint64_t rhw_submitted = ccontext->submitted_triangles;
   uint64_t rhw_rejected = ccontext->rejected_triangles;

   context->draw_vbo(context, &rhw_info, 0, NULL, &rhw_draw, 1);
   context->flush(context, NULL, 0);
   bool rhw_replace_match = ccontext->submitted_triangles ==
                               rhw_submitted + 1 &&
                            ccontext->rejected_triangles == rhw_rejected;
   for (unsigned i = 0; i < ARRAY_SIZE(rhw_y); i++)
      rhw_replace_match &=
         (rhw_pixels[rhw_y[i] * (crhw_target->stride / 4) + 48] &
          0xffffff) == 0xffffff;
   if (!rhw_replace_match ||
       memcmp(rhw_source, rhw_texels, sizeof(rhw_texels))) {
      fprintf(stderr,
              "varying-RHW REPLACE mismatch: submitted=+%" PRIu64
              " rejected=+%" PRIu64 " x48=%06x,%06x,%06x,%06x "
              "expected=ffffff (affine diagnostic=ffff00) source=%u\n",
              ccontext->submitted_triangles - rhw_submitted,
              ccontext->rejected_triangles - rhw_rejected,
              rhw_pixels[rhw_y[0] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_pixels[rhw_y[1] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_pixels[rhw_y[2] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_pixels[rhw_y[3] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              !memcmp(rhw_source, rhw_texels, sizeof(rhw_texels)));
      goto out;
   }

   for (unsigned i = 0; i < 8; i++)
      rhw_source[i] = 0x00ffffff;
   vertex_buffer.buffer.user = rhw_modulate_vertices;
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, modulate_vs);
   context->bind_fs_state(context, modulate);
   context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                  &clear, 1.0, 0);
   rhw_submitted = ccontext->submitted_triangles;
   rhw_rejected = ccontext->rejected_triangles;
   context->draw_vbo(context, &rhw_info, 0, NULL, &rhw_draw, 1);
   context->flush(context, NULL, 0);
   bool rhw_modulate_match = ccontext->submitted_triangles ==
                                rhw_submitted + 1 &&
                             ccontext->rejected_triangles == rhw_rejected;
   for (unsigned i = 0; i < ARRAY_SIZE(rhw_y); i++) {
      uint32_t pixel =
         rhw_pixels[rhw_y[i] * (crhw_target->stride / 4) + 48] & 0xffffff;
      unsigned red = pixel >> 16;

      rhw_modulate_match &= !(pixel & 0xffff) && red >= 0x6f && red <= 0x74;
   }
   const uint8_t *rhw_texture_data =
      (const uint8_t *)crhw_texture->bo.map + crhw_texture->data_offset;
   size_t rhw_texture_aligned_size =
      ALIGN_POT(crhw_texture->data_size, crhw_texture->data_offset);
   bool rhw_source_match = true;

   for (unsigned i = 0; i < 8; i++)
      rhw_source_match &= rhw_source[i] == 0x00ffffff;
   rhw_source_match &= smoke_bytes_match(
      rhw_texture_data + crhw_texture->data_size,
      rhw_texture_aligned_size - crhw_texture->data_size, 0xa5);
   if (!rhw_modulate_match || !rhw_source_match ||
       !smoke_guard_pages_match(crhw_target) ||
       !smoke_guard_pages_match(crhw_texture)) {
      fprintf(stderr,
              "varying-RHW MODULATE mismatch: submitted=+%" PRIu64
              " rejected=+%" PRIu64 " x48=%06x,%06x,%06x,%06x "
              "expected red=6f..74 (affine diagnostic=c1) source=%u "
              "target_guards=%u texture_guards=%u\n",
              ccontext->submitted_triangles - rhw_submitted,
              ccontext->rejected_triangles - rhw_rejected,
              rhw_pixels[rhw_y[0] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_pixels[rhw_y[1] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_pixels[rhw_y[2] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_pixels[rhw_y[3] * (crhw_target->stride / 4) + 48] &
                 0xffffff,
              rhw_source_match, smoke_guard_pages_match(crhw_target),
              smoke_guard_pages_match(crhw_texture));
      goto out;
   }
   puts("Gallium varying-RHW REPLACE/MODULATE perspective passed");
   passed = true;

out:
   if (context) {
      if (replace_vs) context->delete_vs_state(context, replace_vs);
      if (modulate_vs) context->delete_vs_state(context, modulate_vs);
      if (replace) context->delete_fs_state(context, replace);
      if (modulate) context->delete_fs_state(context, modulate);
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (rhw_view) pipe_sampler_view_reference(&rhw_view, NULL);
      if (sampler_handle)
         context->delete_sampler_state(context, sampler_handle);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (replace_elements_handle)
         context->delete_vertex_elements_state(context,
                                               replace_elements_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      if (source_over_handle)
         context->delete_blend_state(context, source_over_handle);
      context->destroy(context);
   }
   if (texture) screen->resource_destroy(screen, texture);
   if (target) screen->resource_destroy(screen, target);
   if (bgra_target) screen->resource_destroy(screen, bgra_target);
   if (rhw_texture) screen->resource_destroy(screen, rhw_texture);
   if (rhw_target) screen->resource_destroy(screen, rhw_target);
   return passed;
}

int
main(int argc, char **argv)
{
   const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
   struct pipe_resource templ = {
      .target = PIPE_BUFFER,
      .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 4096,
      .height0 = 1,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_CUSTOM,
   };
   struct pipe_resource *resource;
   struct pipe_resource surface_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 257,
      .height0 = 129,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource depth_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 257,
      .height0 = 129,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   struct pipe_resource bgra_templ = surface_templ;
   struct chrome9_resource *surface;
   struct chrome9_resource *shared_surface;
   struct pipe_resource *shared_resource;
   struct pipe_resource *depth_resource = NULL;
   struct winsys_handle whandle = { .type = WINSYS_HANDLE_TYPE_FD };
   struct pipe_screen *screen;
   uint32_t *pixels;
   long page_size;
   int fd;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      perror(path);
      return 1;
   }

   screen = chrome9_screen_create(fd, NULL);
   close(fd);
   if (!screen) {
      fputs("chrome9_screen_create failed\n", stderr);
      return 1;
   }
   if (!screen->fence_reference || !screen->fence_finish ||
       !screen->shader_caps[MESA_SHADER_FRAGMENT].max_texture_samplers ||
       !screen->shader_caps[MESA_SHADER_FRAGMENT].max_sampler_views) {
      fprintf(stderr, "Gallium GL/DRI callback or sampler caps missing\n");
      screen->destroy(screen);
      return 1;
   }
   {
      const struct pipe_shader_caps *vs =
         &screen->shader_caps[MESA_SHADER_VERTEX];

      if (vs->max_tex_instructions || vs->max_tex_indirections ||
          vs->max_texture_samplers || vs->max_sampler_views ||
          vs->max_shader_buffers || vs->max_shader_images ||
          vs->max_hw_atomic_counters ||
          vs->max_hw_atomic_counter_buffers ||
          screen->caps.texture_transfer_modes !=
             PIPE_TEXTURE_TRANSFER_DEFAULT ||
          screen->caps.texture_shadow_map || screen->caps.gl_clamp ||
          screen->caps.texrect || screen->caps.native_fp32_depth ||
          screen->caps.buffer_map_persistent_coherent ||
          screen->caps.dest_surface_srgb_control ||
          screen->caps.packed_stream_output ||
          screen->caps.two_sided_color || screen->caps.clip_planes ||
          screen->caps.dithering ||
          screen->caps.mixed_colorbuffer_formats) {
         fprintf(stderr, "bounded Gallium capability contract failed\n");
         screen->destroy(screen);
         return 1;
      }
   }
   {
      struct pipe_fence_handle *fence = NULL;

      screen->fence_reference(screen, &fence, NULL);
      if (fence || !screen->fence_finish(screen, NULL, NULL, 0)) {
         fprintf(stderr, "Gallium synchronous fence contract failed\n");
         screen->destroy(screen);
         return 1;
      }
   }

   printf("name: %s\nvendor: %s\ndevice vendor: %s\n",
          screen->get_name(screen), screen->get_vendor(screen),
          screen->get_device_vendor(screen));
   printf("PCI: %04x:%04x accelerated=%d\n",
          screen->caps.vendor_id, screen->caps.device_id,
          screen->caps.accelerated);

   resource = screen->resource_create(screen, &templ);
   if (!resource) {
      fputs("PIPE_BUFFER creation failed\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   screen->resource_destroy(screen, resource);

   if (!screen->is_format_supported(screen, surface_templ.format,
                                    surface_templ.target, 1, 1,
                                    surface_templ.bind)) {
      fputs("XRGB8888 render-target format rejected\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   resource = screen->resource_create(screen, &surface_templ);
   if (!resource) {
      fputs("guarded XRGB8888 resource creation failed\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   surface = chrome9_resource(resource);
   page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 || surface->stride != 1056 ||
       surface->data_offset != (uint64_t)page_size ||
       surface->data_size != 1056U * 129U || !surface->guarded ||
       !surface->bo.map) {
      fputs("guarded XRGB8888 layout mismatch\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   memset(surface->bo.map, 0x5a, surface->bo.size);
   pixels = (uint32_t *)((uint8_t *)surface->bo.map + surface->data_offset);
   pixels[0] = 0x00112233;
   pixels[(128U * surface->stride) / 4 + 256U] = 0x00aabbcc;
   if (pixels[0] != 0x00112233 ||
       pixels[(128U * surface->stride) / 4 + 256U] != 0x00aabbcc ||
       ((uint8_t *)surface->bo.map)[0] != 0x5a ||
       ((uint8_t *)surface->bo.map)[surface->bo.size - 1] != 0x5a) {
      fputs("guarded XRGB8888 mmap round trip failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }

   if (!screen->resource_get_handle(
          screen, NULL, resource, &whandle,
          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE) ||
       whandle.stride != surface->stride ||
       whandle.offset != surface->data_offset ||
       whandle.modifier != DRM_FORMAT_MOD_LINEAR) {
      fputs("XRGB8888 PRIME export failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   shared_resource = screen->resource_from_handle(
      screen, &surface_templ, &whandle,
      PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   close(whandle.handle);
   if (!shared_resource) {
      fputs("XRGB8888 PRIME import failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   shared_surface = chrome9_resource(shared_resource);
   ((uint32_t *)((uint8_t *)shared_surface->bo.map +
                 shared_surface->data_offset))[1] = 0x0055aa77;
   if (pixels[1] != 0x0055aa77) {
      fputs("XRGB8888 PRIME alias mismatch\n", stderr);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   if (!run_pipe_triangle(screen, shared_resource, NULL)) {
      fputs("Chrome9 imported-shadow Gallium triangle failed\n", stderr);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   screen->resource_destroy(screen, shared_resource);
   if (!run_pipe_triangle(screen, resource, NULL)) {
      fputs("Chrome9 Gallium triangle failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   if (screen->is_format_supported(screen, depth_templ.format,
                                   depth_templ.target, 1, 1,
                                   depth_templ.bind)) {
      struct chrome9_resource *depth;

      depth_resource = screen->resource_create(screen, &depth_templ);
      if (!depth_resource) {
         fputs("guarded D16 resource creation failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      depth = chrome9_resource(depth_resource);
      if (depth->stride != 544 ||
          depth->data_offset != (uint64_t)page_size ||
          depth->data_size != 544U * 129U || !depth->guarded ||
          !depth->bo.map ||
          !run_pipe_triangle(screen, resource, depth_resource)) {
         fputs("Chrome9 Gallium D16 triangle failed\n", stderr);
         screen->resource_destroy(screen, depth_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      screen->resource_destroy(screen, depth_resource);
   } else {
      puts("D16 triangle capability unavailable; depth smoke skipped");
   }
   if (screen->shader_caps[MESA_SHADER_FRAGMENT].max_tex_instructions) {
      if (!run_pipe_texture(screen)) {
         fputs("Chrome9 Gallium texture draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
   } else {
      puts("textured triangle capability unavailable; texture smoke skipped");
   }
   bgra_templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_BLEND_BGRA8888)) {
      struct pipe_resource *bgra_resource;
      struct winsys_handle bgra_handle = { .type = WINSYS_HANDLE_TYPE_FD };
      bool bgra_exported;

      if (!screen->is_format_supported(screen, bgra_templ.format,
                                       bgra_templ.target, 1, 1,
                                       bgra_templ.bind) ||
          screen->is_format_supported(screen, bgra_templ.format,
                                      bgra_templ.target, 1, 1,
                                      bgra_templ.bind | PIPE_BIND_SHARED)) {
         fputs("BGRA8888 bounded render-target bindings mismatch\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      bgra_resource = screen->resource_create(screen, &bgra_templ);
      bgra_exported = bgra_resource &&
         screen->resource_get_handle(
            screen, NULL, bgra_resource, &bgra_handle,
            PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (bgra_exported)
         close(bgra_handle.handle);
      if (!bgra_resource || bgra_exported ||
          !run_pipe_triangle(screen, bgra_resource, NULL)) {
         fputs("Chrome9 Gallium BGRA8888 triangle failed\n", stderr);
         if (bgra_resource)
            screen->resource_destroy(screen, bgra_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      screen->resource_destroy(screen, bgra_resource);
   } else {
      puts("BGRA8888 render-target capability unavailable; smoke skipped");
   }
   screen->resource_destroy(screen, resource);
   screen->destroy(screen);
   puts("Mesa Chrome9 context, Gallium triangle/D16, guarded resources, and PRIME sharing smoke passed");
   return 0;
}
