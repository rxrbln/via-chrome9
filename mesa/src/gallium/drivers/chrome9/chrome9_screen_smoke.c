/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_public.h"
#include "chrome9_resource.h"

#include <fcntl.h>
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

static void *
make_texture_vertex_shader(struct pipe_screen *screen,
                           struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-one-texture-vertex");
   nir_def *position, *color, *texcoord;
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   position = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0),
                             .io_semantics.location = VERT_ATTRIB_POS);
   color = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0),
                          .io_semantics.location = VERT_ATTRIB_COLOR0);
   texcoord = nir_load_input(&b, 2, 32, nir_imm_int(&b, 0),
                             .io_semantics.location = VERT_ATTRIB_TEX0);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_COL0);
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
   struct pipe_context *context = NULL;
   struct pipe_blend_state blend = { 0 };
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
   struct chrome9_resource *surface = chrome9_resource(resource);
   uint32_t *pixels;
   size_t changed = 0;
   uint16_t sample_depth = 0;
   void *blend_handle = NULL;
   void *dsa_handle = NULL;
   void *rasterizer_handle = NULL;
   void *elements_handle = NULL;
   void *vs = NULL;
   void *fs = NULL;
   bool passed = false;

   context = screen->context_create(screen, NULL, 0);
   if (!context)
      goto out;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
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
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
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
   if (depth_resource) {
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);

      sample_depth = depth_pixels[32 * (depth->stride / 2) + 192];
      passed = passed &&
               (pixels[32 * (surface->stride / 4) + 192] & 0xffffff) ==
                  0xff0000 &&
               sample_depth >= 0x3ffe && sample_depth <= 0x4001;
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
   struct pipe_resource target_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource texture_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 2, .height0 = 2, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *target = NULL, *texture = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_templ, *view = NULL;
   struct pipe_sampler_state sampler = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .compare_mode = PIPE_TEX_COMPARE_NONE,
      .max_anisotropy = 1,
      .reduction_mode = PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE,
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[3] = { 0 };
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
   struct chrome9_resource *ctarget, *ctexture;
   void *blend_handle = NULL, *dsa_handle = NULL, *rasterizer_handle = NULL;
   void *elements_handle = NULL, *sampler_handle = NULL;
   void *vs = NULL, *replace = NULL, *modulate = NULL;
   void *samplers[1];
   uint32_t source_copy[4], *source, *pixels;
   bool passed = false;

   target = screen->resource_create(screen, &target_templ);
   texture = screen->resource_create(screen, &texture_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!target || !texture || !context)
      goto out;
   ctarget = chrome9_resource(target);
   ctexture = chrome9_resource(texture);
   memset(ctarget->bo.map, 0x5a, ctarget->bo.size);
   memset(ctexture->bo.map, 0xa5, ctexture->bo.size);
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
   sampler_handle = context->create_sampler_state(context, &sampler);
   u_sampler_view_default_template(&view_templ, texture, texture->format);
   view = context->create_sampler_view(context, texture, &view_templ);
   vs = make_texture_vertex_shader(screen, context);
   replace = make_texture_fragment_shader(screen, context, false);
   modulate = make_texture_fragment_shader(screen, context, true);
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !sampler_handle || !view || !vs || !replace ||
       !modulate)
      goto out;

   framebuffer.cbufs[0].texture = target;
   samplers[0] = sampler_handle;
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                samplers);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                              &view);
   context->bind_vs_state(context, vs);
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

      vertex_buffer.buffer.user = program ? red_vertices : white_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->bind_fs_state(context, program ? modulate : replace);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixels = (uint32_t *)((uint8_t *)ctarget->bo.map +
                            ctarget->data_offset);
      for (unsigned i = 0; i < 4; i++)
         if ((pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
              0xffffff) != expect[program][i])
            goto out;
   }
   if (source[0] != source_copy[0] || source[1] != source_copy[1] ||
       source[ctexture->stride / 4] != source_copy[2] ||
       source[ctexture->stride / 4 + 1] != source_copy[3] ||
       ((uint8_t *)ctarget->bo.map)[0] != 0x5a ||
       ((uint8_t *)ctarget->bo.map)[ctarget->bo.size - 1] != 0x5a ||
       ((uint8_t *)ctexture->bo.map)[0] != 0xa5 ||
       ((uint8_t *)ctexture->bo.map)[ctexture->bo.size - 1] != 0xa5)
      goto out;
   puts("Gallium texture REPLACE/MODULATE and guards passed");
   passed = true;

out:
   if (context) {
      if (vs) context->delete_vs_state(context, vs);
      if (replace) context->delete_fs_state(context, replace);
      if (modulate) context->delete_fs_state(context, modulate);
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (sampler_handle)
         context->delete_sampler_state(context, sampler_handle);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      context->destroy(context);
   }
   if (texture) screen->resource_destroy(screen, texture);
   if (target) screen->resource_destroy(screen, target);
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
   screen->resource_destroy(screen, resource);
   screen->destroy(screen);
   puts("Mesa Chrome9 context, Gallium triangle/D16, guarded resources, and PRIME sharing smoke passed");
   return 0;
}
