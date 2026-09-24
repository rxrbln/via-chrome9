/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_public.h"
#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_fragment.h"
#include "chrome9_resource.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_debug.h"
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

struct smoke_two_texture_vertex {
   float position[4];
   float color[4];
   float texcoord0[4];
   float texcoord1[4];
};

struct smoke_projected_texture_vertex {
   float position[4];
   float color[4];
   float texcoord[4];
};

#define CHROME9_SMOKE_GUARD_WORD 0x13579bdfU

static bool
smoke_guard_pages_match(const struct chrome9_resource *resource)
{
   const uint32_t *first = resource->bo.map;
   const uint32_t *level0_last;
   const uint32_t *final;
   size_t page_size = resource->data_offset;
   size_t aligned_data_size;

   if (!page_size || (page_size & 3) || resource->data_size > SIZE_MAX ||
       resource->bo.size < 2 * page_size)
      return false;
   aligned_data_size = ALIGN_POT(resource->data_size, page_size);
   if (aligned_data_size < resource->data_size ||
       aligned_data_size > resource->bo.size - 2 * page_size)
      return false;
   level0_last =
      (const uint32_t *)((const uint8_t *)resource->bo.map + page_size +
                         aligned_data_size);
   final = (const uint32_t *)((const uint8_t *)resource->bo.map +
                              resource->bo.size - page_size);
   for (size_t i = 0; i < page_size / sizeof(uint32_t); i++)
      if (first[i] != CHROME9_SMOKE_GUARD_WORD ||
          level0_last[i] != CHROME9_SMOKE_GUARD_WORD ||
          final[i] != CHROME9_SMOKE_GUARD_WORD)
         return false;
   return true;
}

static bool
smoke_initialize_guard_pages(struct chrome9_resource *resource)
{
   const size_t page_size = resource->data_offset;
   size_t aligned_data_size;
   uint32_t *first, *level0_last, *final;

   if (!page_size || (page_size & 3) || !resource->bo.map ||
       resource->data_size > SIZE_MAX ||
       resource->bo.size < 2 * page_size)
      return false;
   aligned_data_size = ALIGN_POT(resource->data_size, page_size);
   if (aligned_data_size < resource->data_size ||
       aligned_data_size > resource->bo.size - 2 * page_size)
      return false;
   first = resource->bo.map;
   level0_last = (uint32_t *)((uint8_t *)resource->bo.map + page_size +
                              aligned_data_size);
   final = (uint32_t *)((uint8_t *)resource->bo.map + resource->bo.size -
                        page_size);
   for (size_t i = 0; i < page_size / sizeof(uint32_t); i++) {
      first[i] = CHROME9_SMOKE_GUARD_WORD;
      level0_last[i] = CHROME9_SMOKE_GUARD_WORD;
      final[i] = CHROME9_SMOKE_GUARD_WORD;
   }
   return smoke_guard_pages_match(resource);
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

static bool
smoke_copy_rect(struct pipe_screen *screen)
{
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 32,
      .height0 = 32,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
              PIPE_BIND_LINEAR,
   };
   struct pipe_resource *destination = NULL;
   struct pipe_resource *unguarded_destination = NULL;
   struct pipe_resource *source = NULL;
   struct chrome9_resource *cdestination;
   struct chrome9_resource *cunguarded;
   struct chrome9_resource *csource;
   struct drm_chrome9_copy_rect copy = { 0 };
   uint32_t *destination_pixels;
   uint32_t *source_pixels;
   bool passed = false;

   if (!chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_COPY_RECT))
      return true;
   destination = screen->resource_create(screen, &templ);
   source = screen->resource_create(screen, &templ);
   if (!destination || !source)
      goto out;
   cdestination = chrome9_resource(destination);
   csource = chrome9_resource(source);
   if (!cdestination->guarded || !csource->guarded ||
       cdestination->stride != 128 || csource->stride != 128)
      goto out;
   for (size_t i = 0; i < cdestination->bo.size / sizeof(uint32_t); i++)
      ((uint32_t *)cdestination->bo.map)[i] = CHROME9_SMOKE_GUARD_WORD;
   for (size_t i = 0; i < csource->bo.size / sizeof(uint32_t); i++)
      ((uint32_t *)csource->bo.map)[i] = CHROME9_SMOKE_GUARD_WORD;
   destination_pixels = (uint32_t *)((uint8_t *)cdestination->bo.map +
                                     cdestination->data_offset);
   source_pixels = (uint32_t *)((uint8_t *)csource->bo.map +
                                csource->data_offset);
   for (unsigned y = 0; y < 32; y++)
      for (unsigned x = 0; x < 32; x++) {
         destination_pixels[y * 32 + x] = 0xdeadbeef;
         source_pixels[y * 32 + x] =
            ((x + y) & 0xff) << 24 | x | y << 8 | (x ^ y) << 16;
      }
   copy.destination_handle = cdestination->bo.handle;
   copy.source_handle = csource->bo.handle;
   copy.width = 32;
   copy.height = 32;
   copy.destination_width = 32;
   copy.destination_height = 32;
   copy.destination_pitch = 128;
   copy.source_width = 32;
   copy.source_height = 32;
   copy.source_pitch = 128;
   copy.format = DRM_CHROME9_TEXTURE_FORMAT_BGRX8888;
   if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
      goto out;
   for (unsigned i = 0; i < 32 * 32; i++)
      if (destination_pixels[i] != source_pixels[i])
         goto out;
   for (unsigned i = 0; i < 32 * 32; i++)
      destination_pixels[i] = 0xdeadbeef;
   copy.flags = DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE;
   if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
      goto out;
   for (unsigned i = 0; i < 32 * 32; i++)
      if (destination_pixels[i] != (source_pixels[i] | 0xff000000)) {
         fprintf(stderr,
                 "copy alpha mismatch pixel=%u got=%08x expected=%08x "
                 "source=%08x\n", i, destination_pixels[i],
                 source_pixels[i] | 0xff000000, source_pixels[i]);
         goto out;
      }
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_COPY_RECT_PRESERVE_ALPHA)) {
      for (unsigned i = 0; i < 32 * 32; i++)
         destination_pixels[i] = 0x44000000 | (i & 0x00ffffff);
      copy.flags = DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA;
      if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
         goto out;
      for (unsigned i = 0; i < 32 * 32; i++) {
         uint32_t expected = 0x44000000 |
                             (source_pixels[i] & 0x00ffffff);

         if (destination_pixels[i] != expected) {
            fprintf(stderr,
                    "copy preserve-alpha mismatch pixel=%u got=%08x "
                    "expected=%08x source=%08x\n", i,
                    destination_pixels[i], expected, source_pixels[i]);
            goto out;
         }
      }

      /* Characterize a non-origin subrectangle, every outside pixel, and
       * the following unmasked command so the H6 byte mask cannot leak. */
      for (unsigned i = 0; i < 32 * 32; i++)
         destination_pixels[i] = 0x66000000 | i;
      copy.destination_x = 3;
      copy.destination_y = 5;
      copy.source_x = 7;
      copy.source_y = 9;
      copy.width = 11;
      copy.height = 13;
      if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
         goto out;
      for (unsigned y = 0; y < 32; y++)
         for (unsigned x = 0; x < 32; x++) {
            unsigned i = y * 32 + x;
            bool inside = x >= 3 && x < 14 && y >= 5 && y < 18;
            unsigned source_i = (y - 5 + 9) * 32 + (x - 3 + 7);
            uint32_t expected = inside ?
               0x66000000 | (source_pixels[source_i] & 0x00ffffff) :
               0x66000000 | i;

            if (destination_pixels[i] != expected)
               goto out;
         }
      copy.flags = 0;
      if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
         goto out;
      for (unsigned y = 0; y < 32; y++)
         for (unsigned x = 0; x < 32; x++) {
            unsigned i = y * 32 + x;
            bool inside = x >= 3 && x < 14 && y >= 5 && y < 18;
            unsigned source_i = (y - 5 + 9) * 32 + (x - 3 + 7);
            uint32_t expected = inside ? source_pixels[source_i] :
                                         0x66000000 | i;

            if (destination_pixels[i] != expected)
               goto out;
         }
      copy.destination_x = 0;
      copy.destination_y = 0;
      copy.source_x = 0;
      copy.source_y = 0;
      copy.width = 32;
      copy.height = 32;
   }
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_COPY_RECT_UNGUARDED_DESTINATION)) {
      struct pipe_resource external_templ = templ;
      uint32_t *unguarded_pixels;

      external_templ.bind |= PIPE_BIND_DISPLAY_TARGET |
                             PIPE_BIND_SCANOUT | PIPE_BIND_SHARED;
      unguarded_destination =
         screen->resource_create(screen, &external_templ);
      if (!unguarded_destination)
         goto out;
      cunguarded = chrome9_resource(unguarded_destination);
      if (cunguarded->guarded || cunguarded->data_offset ||
          cunguarded->stride != 128)
         goto out;
      unguarded_pixels = cunguarded->bo.map;
      for (unsigned i = 0; i < 32 * 32; i++)
         unguarded_pixels[i] = 0xdeadbeef;
      copy.destination_handle = cunguarded->bo.handle;
      copy.flags = DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE |
                   DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED;
      if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
         goto out;
      for (unsigned i = 0; i < 32 * 32; i++)
         if (unguarded_pixels[i] != (source_pixels[i] | 0xff000000))
            goto out;
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_COPY_RECT_PRESERVE_ALPHA)) {
         for (unsigned i = 0; i < 32 * 32; i++)
            unguarded_pixels[i] = 0x55000000 | (i & 0x00ffffff);
         copy.flags =
            DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA |
            DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED;
         if (!chrome9_copy_rect(screen->get_screen_fd(screen), &copy))
            goto out;
         for (unsigned i = 0; i < 32 * 32; i++) {
            uint32_t expected = 0x55000000 |
                                (source_pixels[i] & 0x00ffffff);

            if (unguarded_pixels[i] != expected)
               goto out;
         }
      }
   }
   passed = smoke_guard_pages_match(cdestination) &&
            smoke_guard_pages_match(csource);
out:
   if (unguarded_destination)
      screen->resource_destroy(screen, unguarded_destination);
   if (source)
      screen->resource_destroy(screen, source);
   if (destination)
      screen->resource_destroy(screen, destination);
   return passed;
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
                    .io_semantics.location = emit_color ?
                       VARYING_SLOT_TEX0 : VARYING_SLOT_VAR0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_texture_fragment_shader_slot(struct pipe_screen *screen,
                                  struct pipe_context *context,
                                  bool modulate, unsigned texcoord_location)
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
      .io_semantics.location = texcoord_location);
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
make_texture_fragment_shader(struct pipe_screen *screen,
                             struct pipe_context *context, bool modulate)
{
   return make_texture_fragment_shader_slot(
      screen, context, modulate,
      modulate ? VARYING_SLOT_TEX0 : VARYING_SLOT_VAR0);
}

static void *
make_projected_texture_vertex_shader(struct pipe_screen *screen,
                                     struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-projected-texture-primary-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);
   nir_def *texcoord = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_TEX0);

   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_COL0);
   nir_store_output(&b, texcoord, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_TEX0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_projected_texture_fragment_shader(struct pipe_screen *screen,
                                       struct pipe_context *context,
                                       bool preserve_primary_alpha)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      preserve_primary_alpha ?
         "chrome9-projected-texture-primary-rgb-alpha" :
         "chrome9-projected-texture-primary-rgba");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   nir_def *texcoord = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_TEX0);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   nir_def *coord = nir_trim_vector(&b, texcoord, 2);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);

   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_projector,
                                     nir_channel(&b, texcoord, 3));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *product = nir_fmul(&b, &tex->def, color);
   nir_def *result = product;

   if (preserve_primary_alpha)
      result = nir_vec4(&b, nir_channel(&b, product, 0),
                        nir_channel(&b, product, 1),
                        nir_channel(&b, product, 2),
                        nir_channel(&b, color, 3));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_projected_texture_constant_fragment_shader(struct pipe_screen *screen,
                                                 struct pipe_context *context,
                                                 bool preserve_constant_alpha)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      preserve_constant_alpha ?
         "chrome9-projected-texture-constant-rgb-alpha" :
         "chrome9-projected-texture-constant-rgba");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   nir_def *texcoord = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_TEX0);
   nir_def *coord = nir_trim_vector(&b, texcoord, 2);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);

   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_projector,
                                     nir_channel(&b, texcoord, 3));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *constant = nir_load_uniform(
      &b, 4, 32, nir_imm_int(&b, 0), .range = 1);
   nir_def *product = nir_fmul(&b, &tex->def, constant);
   nir_def *result = product;

   if (preserve_constant_alpha)
      result = nir_vec4(&b, nir_channel(&b, product, 0),
                        nir_channel(&b, product, 1),
                        nir_channel(&b, product, 2),
                        nir_channel(&b, constant, 3));

   nir_store_output(&b, result,
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_texture_constant_fragment_shader(struct pipe_screen *screen,
                                      struct pipe_context *context,
                                      bool alpha_replicate)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      alpha_replicate ? "chrome9-texture-constant-alpha-modulate" :
                        "chrome9-texture-constant-modulate");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   nir_def *coord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_TEX0);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *constant = nir_load_ubo(
      &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
      .align_mul = 16, .range = 16);
   if (alpha_replicate) {
      static const unsigned aaaa[4] = { 3, 3, 3, 3 };

      constant = nir_swizzle(&b, constant, aaaa, 4);
   }
   nir_store_output(&b, nir_fmul(&b, &tex->def, constant),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_texture_rgbx_fragment_shader(struct pipe_screen *screen,
                                  struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-texture-rgbx");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   nir_def *coord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *result = nir_vec4(&b, nir_channel(&b, &tex->def, 0),
                              nir_channel(&b, &tex->def, 1),
                              nir_channel(&b, &tex->def, 2),
                              nir_imm_float(&b, 1.0f));
   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_scalar_generic_vertex_shader(struct pipe_screen *screen,
                                  struct pipe_context *context,
                                  bool packed_texture)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      packed_texture ? "chrome9-packed-gray-uv-vertex" :
                       "chrome9-scalar-gray-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);
   nir_def *varying = nir_channel(&b, color, 0);
   if (packed_texture) {
      nir_def *coord = nir_load_input(
         &b, 2, 32, nir_imm_int(&b, 0),
         .io_semantics.location = VERT_ATTRIB_TEX0);

      varying = nir_vec3(&b, varying, nir_channel(&b, coord, 0),
                         nir_channel(&b, coord, 1));
   }
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, varying, nir_imm_int(&b, 0),
                    .write_mask = BITFIELD_MASK(varying->num_components),
                    .io_semantics.location = VARYING_SLOT_VAR0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static nir_def *
make_generic0_load(nir_builder *b, const struct glsl_type *type,
                   const char *name)
{
   nir_variable *varying = nir_variable_create(b->shader, nir_var_shader_in,
                                                type, name);

   varying->data.location = VARYING_SLOT_VAR0;
   varying->data.interpolation = INTERP_MODE_SMOOTH;
   return nir_load_var(b, varying);
}

static void *
make_generic_rgba_vertex_shader(struct pipe_screen *screen,
                                struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-glmark2-gouraud-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_VAR0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_generic_rgba_fragment_shader(struct pipe_screen *screen,
                                  struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glmark2-gouraud-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *color = make_generic0_load(
      &b, glsl_vector_type(GLSL_TYPE_FLOAT, 4), "Color");

   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_generic_rgbx_vertex_shader(struct pipe_screen *screen,
                                struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-glmark2-gouraud-rgb-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, nir_trim_vector(&b, color, 3), nir_imm_int(&b, 0),
                    .write_mask = 0x7,
                    .io_semantics.location = VARYING_SLOT_VAR0,
                    .src_type = nir_type_float32);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_generic_rgbx_fragment_shader(struct pipe_screen *screen,
                                  struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glmark2-gouraud-rgb-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *color = make_generic0_load(
      &b, glsl_vector_type(GLSL_TYPE_FLOAT, 3), "Color");
   nir_def *result = nir_vec4(&b, nir_channel(&b, color, 0),
                              nir_channel(&b, color, 1),
                              nir_channel(&b, color, 2),
                              nir_imm_float(&b, 1.0f));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_generic_xxy_vertex_shader(struct pipe_screen *screen,
                               struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-glmark2-ideas-xxy-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, nir_trim_vector(&b, color, 2), nir_imm_int(&b, 0),
                    .write_mask = 0x3,
                    .io_semantics.location = VARYING_SLOT_VAR0,
                    .src_type = nir_type_float32);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_generic_xxy_fragment_shader(struct pipe_screen *screen,
                                 struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glmark2-ideas-xxy-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *varying = make_generic0_load(
      &b, glsl_vector_type(GLSL_TYPE_FLOAT, 2), "Color");
   nir_def *result = nir_vec4(&b, nir_channel(&b, varying, 0),
                              nir_channel(&b, varying, 0),
                              nir_channel(&b, varying, 1),
                              nir_imm_float(&b, 1.0f));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_scalar_rgbx_fragment_shader(struct pipe_screen *screen,
                                 struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-scalar-gray-rgbx-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *gray = make_generic0_load(&b, glsl_float_type(), "Color");
   nir_def *result = nir_vec4(&b, gray, gray, gray,
                              nir_imm_float(&b, 1.0f));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_flat_scalar_rgbx_fragment_shader(struct pipe_screen *screen,
                                      struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-flat-scalar-gray-rgbx-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_variable *varying = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_float_type(), "Color");

   varying->data.location = VARYING_SLOT_VAR0;
   varying->data.interpolation = INTERP_MODE_FLAT;
   nir_def *gray = nir_load_var(&b, varying);
   nir_def *result = nir_vec4(&b, gray, gray, gray,
                              nir_imm_float(&b, 1.0f));
   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_literal_color_fragment_shader(struct pipe_screen *screen,
                                   struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-literal-color-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *color = nir_imm_vec4(&b, 0.25f, 0.5f, 0.75f, 1.0f);

   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_scalar_blue_rgbx_fragment_shader(struct pipe_screen *screen,
                                      struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glmark2-gouraud-blue-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *blue = make_generic0_load(&b, glsl_float_type(), "Color");
   nir_def *result = nir_vec4(&b, nir_imm_float(&b, 0.0f),
                              nir_imm_float(&b, 0.0f), blue,
                              nir_imm_float(&b, 1.0f));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_packed_gray_texture_fragment_shader(struct pipe_screen *screen,
                                         struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-packed-gray-uv-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *varying = make_generic0_load(
      &b, glsl_vector_type(GLSL_TYPE_FLOAT, 3), "ColorTexcoord");
   nir_def *coord = nir_vec2(&b, nir_channel(&b, varying, 1),
                             nir_channel(&b, varying, 2));
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *gray = nir_channel(&b, varying, 0);
   nir_def *diffuse = nir_vec4(&b, gray, gray, gray,
                               nir_imm_float(&b, 1.0f));
   nir_store_output(&b, nir_fmul(&b, &tex->def, diffuse),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_two_texture_vertex_shader(struct pipe_screen *screen,
                               struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-two-texture-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   for (unsigned unit = 0; unit < 2; unit++) {
      nir_def *coord = nir_load_input(
         &b, 2, 32, nir_imm_int(&b, 0),
         .io_semantics.location = VERT_ATTRIB_TEX0 + unit);

      coord = nir_vec4(&b, nir_channel(&b, coord, 0),
                       nir_channel(&b, coord, 1), nir_imm_float(&b, 0.0f),
                       nir_imm_float(&b, 1.0f));
      nir_store_output(&b, coord, nir_imm_int(&b, 0), .write_mask = 0xf,
                       .io_semantics.location = VARYING_SLOT_TEX0 + unit);
   }
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_two_texture_fragment_shader(struct pipe_screen *screen,
                                 struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-two-texture-modulate");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *sample[2];

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   for (unsigned unit = 0; unit < 2; unit++) {
      nir_def *coord = nir_load_interpolated_input(
         &b, 2, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_TEX0 + unit);
      nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);

      tex->op = nir_texop_tex;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->coord_components = 2;
      tex->dest_type = nir_type_float32;
      tex->texture_index = unit;
      tex->sampler_index = unit;
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      sample[unit] = &tex->def;
   }
   nir_store_output(&b, nir_fmul(&b, sample[0], sample[1]),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_alpha_kill_fragment_shader(struct pipe_screen *screen,
                                struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-alpha-kill-compile-boundary");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_tex_instr *tex;
   nir_def *barycentric, *coord, *alpha;

   b.shader->info.io_lowered = true;
   barycentric = nir_load_barycentric_pixel(&b, 32);
   coord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   alpha = nir_channel(&b, &tex->def, 3);
   nir_discard_if(&b, nir_flt(&b, alpha, nir_imm_float(&b, 0.5f)));
   nir_store_output(&b, &tex->def, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static bool
smoke_alpha_kill_compile(struct pipe_screen *screen)
{
   struct pipe_context *context = screen->context_create(screen, NULL, 0);
   struct chrome9_shader *shader;
   bool enabled, passed;

   if (!context)
      return false;
   shader = make_alpha_kill_fragment_shader(screen, context);
   enabled = debug_get_bool_option("CHROME9_EXPERIMENTAL_TEXKILL", false) &&
      chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_FRAGMENT_KILL);
   passed = shader && (!enabled ? !shader->semantic_fs_valid :
      shader->semantic_fs_valid && shader->supported &&
      shader->semantic_fs_ir.count == 4 &&
      shader->semantic_fs_ir.instruction[0].op == CHROME9_PS_OPCODE_TEX &&
      shader->semantic_fs_ir.instruction[1].op == CHROME9_PS_OPCODE_SUB &&
      shader->semantic_fs_ir.instruction[2].op == CHROME9_PS_OPCODE_KILL &&
      shader->semantic_fs_ir.instruction[3].op == CHROME9_PS_OPCODE_MOV);
   if (shader)
      context->delete_fs_state(context, shader);
   context->destroy(context);
   if (passed)
      puts(enabled ? "alpha KILL semantic NIR order passed" :
                     "alpha KILL remains gated");
   return passed;
}

/* Replay the captured straight-line varying arithmetic a prefix at a time.
 * A prefix failure identifies the first compiler operation that needs work;
 * the final prefix retains the full output expression and uses no GPU draw.
 */
static void *
make_varying_arithmetic_fs(struct pipe_screen *screen,
                           struct pipe_context *context, unsigned stage)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-varying-arithmetic-compile");
   nir_variable *output = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "fragment_color");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *varying = make_generic0_load(&b, glsl_vec4_type(), "varying");
   nir_def *x = nir_fmul(&b, nir_channel(&b, varying, 0),
                        nir_channel(&b, varying, 3));
   nir_def *y = nir_fmul(&b, nir_channel(&b, varying, 1),
                        nir_channel(&b, varying, 3));
   nir_def *z = nir_fmul(&b, nir_channel(&b, varying, 2),
                        nir_channel(&b, varying, 3));
   nir_def *minimum_yz = nir_fmin(&b, y, z);
   nir_def *minimum = nir_fmin(&b, x, minimum_yz);
   nir_def *twice = nir_fmul(&b, nir_imm_float(&b, 2.0f), minimum);
   nir_def *squared = nir_fmul(&b, twice, minimum);
   nir_def *negated = nir_fneg(&b, squared);
   nir_def *weight = nir_fexp2(&b, negated);
   nir_def *weighted = nir_fmul(
      &b, weight, nir_imm_vec4(&b, 1.0f, 0.5f, 0.2f, 0.2f));
   nir_def *final = nir_fadd(
      &b, nir_imm_vec4(&b, 0.0f, 0.5f, 0.8f, 0.8f), weighted);
   nir_def *prefix[] = {
      varying, x, y, z, minimum_yz, minimum, twice, squared,
      negated, weight, weighted, final,
   };
   nir_def *color = prefix[stage];

   output->data.location = FRAG_RESULT_COLOR;
   if (color->num_components == 1)
      color = nir_vec4(&b, color, color, color, color);
   nir_store_var(&b, output, color, 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static bool
smoke_varying_arithmetic_compile(struct pipe_screen *screen)
{
   static const char *const stage[] = {
      "varying", "mul-xw", "mul-yw", "mul-zw", "min-yz", "min-x-yz",
      "mul-two", "mul-square", "negate", "exp2", "vector-mul",
      "final-add",
   };
   struct pipe_context *context = screen->context_create(screen, NULL, 0);
   bool passed = true;

   if (!context)
      return false;
   for (unsigned i = 0; i < ARRAY_SIZE(stage); i++) {
      struct chrome9_shader *shader =
         make_varying_arithmetic_fs(screen, context, i);

      if (!shader) {
         fprintf(stderr, "varying arithmetic %s: create_fs_state failed\n",
                 stage[i]);
         passed = false;
         break;
      }
      if (!shader->semantic_fs_valid || !shader->supported) {
         fprintf(stderr,
                 "varying arithmetic %s: semantic=%u native=%u "
                 "native-from-nir=%u supported=%u program-cap=%u "
                 "ir=%u alu=%u tau=%u\n",
                 stage[i], shader->semantic_fs_valid,
                 shader->native_fs_valid, shader->native_fs_from_nir,
                 shader->supported,
                 chrome9_context(context)->draw_fragment_program,
                 shader->semantic_fs_ir.count,
                 shader->native_fs.alu_count, shader->native_fs.tau_count);
         passed = false;
      }
      context->delete_fs_state(context, shader);
      if (!passed)
         break;
   }
   context->destroy(context);
   if (passed)
      puts("varying arithmetic full fragment NIR accepted without draw");
   return passed;
}

/* Exercise the NIR-to-semantic path without submitting a GPU draw.  Keep
 * native Z/W out of the live sources until their MISC mapping is known. */
static bool
smoke_position_compile(struct pipe_screen *screen)
{
   struct pipe_context *context = screen->context_create(screen, NULL, 0);
   struct chrome9_shader *shader;
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   unsigned instruction_count = 0;
   nir_builder b;
   struct pipe_shader_state state;
   nir_def *position, *transform, *x, *y, *color;
   bool passed;

   if (!context)
      return false;
   b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-position-compile");
   b.shader->info.io_lowered = true;
   position = nir_load_frag_coord(&b);
   transform = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                 nir_imm_int(&b, 2));
   x = nir_fadd_imm(&b, nir_channel(&b, position, 0), 0.5f);
   y = nir_fadd_imm(&b, nir_channel(&b, position, 1), 0.5f);
   y = nir_ffma(&b, y, nir_channel(&b, transform, 0),
                nir_channel(&b, transform, 1));
   x = nir_fmul(&b, x,
                nir_imm_float(&b, 1.0f / 32.0f));
   y = nir_fmul(&b, y,
                nir_imm_float(&b, 1.0f / 32.0f));
   color = nir_vec4(&b, x, y, x, y);
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   state = (struct pipe_shader_state) {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   shader = context->create_fs_state(context, &state);
   passed = shader && shader->semantic_fs_valid && shader->supported &&
      shader->semantic_fs_ir.count >= 2 &&
      (shader->semantic_constant_mask & (1u << 2)) &&
      (shader->semantic_uniform_mask & (1u << 2)) &&
      chrome9_fragment_build_instructions(&shader->semantic_fs_ir, 0,
                                           instructions,
                                           &instruction_count) &&
      instruction_count == shader->semantic_fs_ir.count;
   if (passed) {
      bool position_found = false;

      for (unsigned i = 0; i < shader->semantic_fs_ir.count; i++)
         for (unsigned s = 0; s < 3; s++)
            position_found |= shader->semantic_fs_ir.instruction[i]
               .source[s].file == CHROME9_PS_SOURCE_POSITION;
      passed = position_found;
   }
   if (shader)
      context->delete_fs_state(context, shader);
   if (passed) {
      nir_builder boundary = nir_builder_init_simple_shader(
         MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
         "chrome9-ubo-boundary-compile");
      nir_def *vec4_lane, *byte_lane;

      boundary.shader->info.io_lowered = true;
      vec4_lane = nir_load_ubo_vec4(
         &boundary, 1, 32, nir_imm_int(&boundary, 0),
         nir_imm_int(&boundary, 1), .base = 4, .component = 2);
      byte_lane = nir_load_ubo(
         &boundary, 1, 32, nir_imm_int(&boundary, 0),
         nir_imm_int(&boundary, 31 * 16 + 12),
         .align_mul = 4, .range_base = 31 * 16 + 12, .range = 4);
      nir_store_output(&boundary,
                       nir_vec4(&boundary, vec4_lane, byte_lane,
                                vec4_lane, byte_lane),
                       nir_imm_int(&boundary, 0), .write_mask = 0xf,
                       .io_semantics.location = FRAG_RESULT_COLOR);
      nir_shader_gather_info(boundary.shader,
                             nir_shader_get_entrypoint(boundary.shader));
      nir_recompute_io_bases(boundary.shader,
                             nir_var_shader_in | nir_var_shader_out);
      state.ir.nir = boundary.shader;
      shader = context->create_fs_state(context, &state);
      passed = shader && shader->semantic_fs_valid && shader->supported &&
         (shader->semantic_uniform_mask & ((1u << 5) | (1u << 31))) ==
            ((1u << 5) | (1u << 31)) &&
         chrome9_fragment_build_instructions(&shader->semantic_fs_ir, 0,
                                              instructions,
                                              &instruction_count);
      if (shader)
         context->delete_fs_state(context, shader);
   }
   context->destroy(context);
   if (passed)
      puts("fragment POSITION XY and dynamic WPOS transform accepted without draw");
   return passed;
}

static void *
make_two_projected_texture_vertex_shader(struct pipe_screen *screen,
                                         struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-two-projected-texture-primary-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);
   nir_def *color = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_COLOR0);

   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_COL0);
   for (unsigned unit = 0; unit < 2; unit++) {
      nir_def *coord = nir_load_input(
         &b, 4, 32, nir_imm_int(&b, 0),
         .io_semantics.location = VERT_ATTRIB_TEX0 + unit);

      nir_store_output(&b, coord, nir_imm_int(&b, 0), .write_mask = 0xf,
                       .io_semantics.location = VARYING_SLOT_TEX0 + unit);
   }
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_two_projected_texture_fragment_shader(struct pipe_screen *screen,
                                           struct pipe_context *context,
                                           bool preserve_texture0_alpha)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      preserve_texture0_alpha ?
         "chrome9-two-projected-texture0-alpha-primary" :
         "chrome9-two-projected-texture-primary-rgb-alpha");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *sample[2];

   b.shader->info.io_lowered = true;
   nir_def *barycentric = nir_load_barycentric_pixel(&b, 32);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   for (unsigned unit = 0; unit < 2; unit++) {
      nir_def *texcoord = nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_TEX0 + unit);
      nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);

      tex->op = nir_texop_tex;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->coord_components = 2;
      tex->dest_type = nir_type_float32;
      tex->texture_index = unit;
      tex->sampler_index = unit;
      tex->src[0] = nir_tex_src_for_ssa(
         nir_tex_src_coord, nir_trim_vector(&b, texcoord, 2));
      tex->src[1] = nir_tex_src_for_ssa(
         nir_tex_src_projector, nir_channel(&b, texcoord, 3));
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      sample[unit] = &tex->def;
   }
   nir_def *first_product = nir_fmul(&b, sample[0], color);
   nir_def *first_rgb_primary_alpha = nir_vec4(
      &b, nir_channel(&b, first_product, 0),
      nir_channel(&b, first_product, 1),
      nir_channel(&b, first_product, 2), nir_channel(&b, color, 3));
   nir_def *product = nir_fmul(&b, sample[1], first_rgb_primary_alpha);
   nir_def *result = nir_vec4(
      &b, nir_channel(&b, product, 0), nir_channel(&b, product, 1),
      nir_channel(&b, product, 2),
      preserve_texture0_alpha ? nir_channel(&b, first_product, 3) :
                                nir_channel(&b, color, 3));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
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
   nir_variable *input = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "primary_color");
   nir_variable *output = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "fragment_color");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   input->data.location = VARYING_SLOT_COL0;
   output->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, output, nir_load_var(&b, input), 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_solid_vertex_shader(struct pipe_screen *screen,
                         struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-glamor-solid-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *position = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_POS);

   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_position_draw_fs(struct pipe_screen *screen,
                      struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-position-guarded-draw");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *position, *transform, *x, *y;

   b.shader->info.io_lowered = true;
   position = nir_load_frag_coord(&b);
   transform = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                 nir_imm_int(&b, 2));
   x = nir_fadd_imm(&b, nir_channel(&b, position, 0), 0.5f);
   y = nir_fadd_imm(&b, nir_channel(&b, position, 1), 0.5f);
   y = nir_ffma(&b, y, nir_channel(&b, transform, 0),
                nir_channel(&b, transform, 1));
   x = nir_fmul_imm(&b, x, 1.0f / 64.0f);
   y = nir_fmul_imm(&b, y, 1.0f / 64.0f);
   nir_store_output(&b, nir_vec4(&b, x, y, x, y), nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_constant_color_fs(struct pipe_screen *screen,
                       struct pipe_context *context, unsigned offset)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glamor-solid-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_def *color = nir_load_ubo(
      &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, offset),
      .align_mul = 16, .range_base = offset, .range = 16);

   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static void *
make_constant_product_fs(struct pipe_screen *screen,
                         struct pipe_context *context, unsigned mode,
                         unsigned second_offset)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-glamor-constant-product");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   static const unsigned aaaa[4] = { 3, 3, 3, 3 };

   b.shader->info.io_lowered = true;
   nir_def *c0 = nir_load_ubo(
      &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
      .align_mul = 16, .range_base = 0, .range = 16);
   nir_def *c1 = nir_load_ubo(
      &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, second_offset),
      .align_mul = 16, .range_base = second_offset, .range = 16);
   if (mode == 1)
      c1 = nir_swizzle(&b, c1, aaaa, 4);
   else if (mode == 2)
      c0 = nir_swizzle(&b, c0, aaaa, 4);
   nir_store_output(&b, nir_fmul(&b, c0, c1), nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

enum smoke_program_fs_mode {
   SMOKE_PROGRAM_FS_VALID,
   SMOKE_PROGRAM_FS_ADD,
   SMOKE_PROGRAM_FS_SATURATE,
   SMOKE_PROGRAM_FS_PARTIAL_STORE,
   SMOKE_PROGRAM_FS_DISCARD,
   SMOKE_PROGRAM_FS_NOPERSPECTIVE,
   SMOKE_PROGRAM_FS_INPUT_OFFSET,
   SMOKE_PROGRAM_FS_FLOAT_CONTROLS,
   SMOKE_PROGRAM_FS_SAME_FILE_MUL,
   SMOKE_PROGRAM_FS_ALPHA_SWIZZLE,
   SMOKE_PROGRAM_FS_CONDITIONAL,
   SMOKE_PROGRAM_FS_COUNT,
};

/* Exercise the NIR-driven backend independently of the older program-shape
 * classifier.  Only the first form is in the one-record cmd19 ABI; each
 * negative form differs by one semantic that the backend must reject. */
static void *
make_program_backend_fs(struct pipe_screen *screen,
                        struct pipe_context *context,
                        enum smoke_program_fs_mode mode)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-one-record-backend-boundary");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *barycentric;
   nir_def *color;
   nir_def *constant;
   static const unsigned aaaa[4] = { 3, 3, 3, 3 };

   b.shader->info.io_lowered = true;
   if (mode == SMOKE_PROGRAM_FS_FLOAT_CONTROLS)
      b.shader->info.float_controls_execution_mode =
         FLOAT_CONTROLS_DENORM_PRESERVE_FP32;
   if (mode == SMOKE_PROGRAM_FS_NOPERSPECTIVE)
      barycentric = nir_load_barycentric_pixel(
         &b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
   else
      barycentric = nir_load_barycentric_pixel(
         &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   color = nir_load_interpolated_input(
      &b, 4, 32, barycentric,
      nir_imm_int(&b, mode == SMOKE_PROGRAM_FS_INPUT_OFFSET ? 1 : 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   constant = nir_load_ubo(
      &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
      .align_mul = 16, .range_base = 0, .range = 16);
   if (mode == SMOKE_PROGRAM_FS_SAME_FILE_MUL)
      color = constant;
   else if (mode == SMOKE_PROGRAM_FS_ALPHA_SWIZZLE)
      color = nir_swizzle(&b, color, aaaa, 4);
   if (mode == SMOKE_PROGRAM_FS_SATURATE)
      constant = nir_fsat(&b, constant);
   if (mode == SMOKE_PROGRAM_FS_DISCARD)
      nir_discard_if(&b, nir_imm_false(&b));
   if (mode == SMOKE_PROGRAM_FS_CONDITIONAL)
      nir_push_if(&b, nir_imm_true(&b));
   nir_store_output(
      &b, mode == SMOKE_PROGRAM_FS_ADD ?
         nir_fadd(&b, color, constant) :
         nir_fmul(&b, color, constant), nir_imm_int(&b, 0),
      .write_mask = mode == SMOKE_PROGRAM_FS_PARTIAL_STORE ? 0x7 : 0xf,
      .io_semantics.location = FRAG_RESULT_COLOR);
   if (mode == SMOKE_PROGRAM_FS_CONDITIONAL)
      nir_pop_if(&b, NULL);
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
   static const struct smoke_vertex scalar_gray_vertices[] = {
      { { -1, -1, 0, 1 }, { 0.25, 0, 0, 0 } },
      { { -1,  1, 0, 1 }, { 0.25, 0, 0, 0 } },
      { {  1, -1, 0, 1 }, { 0.25, 0, 0, 0 } },
      { {  1,  1, 0, 1 }, { 0.25, 0, 0, 0 } },
   };
   static const struct smoke_vertex flat_gray_vertices[] = {
      { { -1, -1, 0, 1 }, { 0.125, 0, 0, 0 } },
      { { -1,  1, 0, 1 }, { 0.25, 0, 0, 0 } },
      { {  1, -1, 0, 1 }, { 0.5, 0, 0, 0 } },
      { {  1,  1, 0, 1 }, { 0.75, 0, 0, 0 } },
   };
   static const struct smoke_vertex scalar_perspective_vertices[] = {
      { { -1, -1, 0, 1 }, { 0, 0, 0, 0 } },
      { {  4, -4, 0, 4 }, { 1, 0, 0, 0 } },
      { {  4,  4, 0, 4 }, { 1, 0, 0, 0 } },
   };
   static const struct smoke_vertex generic_rgba_vertices[] = {
      { { -1, -1, 0, 1 }, { 0.25, 0.5, 0.75, 0.5 } },
      { { -1,  1, 0, 1 }, { 0.25, 0.5, 0.75, 0.5 } },
      { {  1, -1, 0, 1 }, { 0.25, 0.5, 0.75, 0.5 } },
      { {  1,  1, 0, 1 }, { 0.25, 0.5, 0.75, 0.5 } },
   };
   static const struct smoke_vertex alpha_near_kill_vertices[] = {
      { { -1, -1, -0.5, 1 }, { 1, 0, 0, 0.75 } },
      { { -1,  1, -0.5, 1 }, { 1, 0, 0, 0.75 } },
      { {  1, -1, -0.5, 1 }, { 1, 0, 0, 0.75 } },
      { {  1,  1, -0.5, 1 }, { 1, 0, 0, 0.75 } },
   };
   static const struct smoke_vertex alpha_far_pass_vertices[] = {
      { { -1, -1, 0, 1 }, { 0, 1, 0, 0.25 } },
      { { -1,  1, 0, 1 }, { 0, 1, 0, 0.25 } },
      { {  1, -1, 0, 1 }, { 0, 1, 0, 0.25 } },
      { {  1,  1, 0, 1 }, { 0, 1, 0, 0.25 } },
   };
   struct pipe_context *context = NULL;
   struct pipe_blend_state blend = { 0 };
   struct pipe_blend_state source_over = { 0 };
   struct pipe_blend_state multiply = { 0 };
   struct pipe_blend_state alpha_only = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_depth_stencil_alpha_state alpha_dsa;
   struct pipe_depth_stencil_alpha_state read_only_dsa;
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_rasterizer_state scissor_rasterizer;
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
   const float solid_color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
   const float solid_buffer_color[4] = { 1.0f, 0.0f, 0.5f, 1.0f };
   const float product_colors[8] = {
      0.8f, 0.4f, 0.2f, 0.75f,
      0.5f, 0.25f, 1.0f, 0.25f,
   };
   const float program_color[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
   const float add_program_color[4] = { 0.125f, 0.125f, 0.125f, 0.25f };
   struct pipe_constant_buffer solid_constant = {
      .buffer_size = sizeof(solid_color),
      .user_buffer = solid_color,
   };
   struct pipe_constant_buffer solid_buffer_constant = {
      .buffer_size = sizeof(solid_buffer_color),
   };
   struct pipe_constant_buffer product_constant = {
      .buffer_size = sizeof(product_colors),
      .user_buffer = product_colors,
   };
   struct pipe_constant_buffer program_constant = {
      .buffer_size = sizeof(program_color),
      .user_buffer = program_color,
   };
   struct pipe_constant_buffer add_program_constant = {
      .buffer_size = sizeof(add_program_color),
      .user_buffer = add_program_color,
   };
   const struct pipe_resource solid_buffer_template = {
      .target = PIPE_BUFFER,
      .format = PIPE_FORMAT_R8_UNORM,
      .width0 = sizeof(solid_buffer_color),
      .height0 = 1,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_CONSTANT_BUFFER,
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
   uint64_t initial_batches = 0;
   uint64_t initial_program_batches = 0;
   uint64_t initial_rhw_runs = 0;
   uint64_t initial_rhw_chains = 0;
   void *blend_handle = NULL;
   void *source_over_handle = NULL;
   void *multiply_handle = NULL;
   void *alpha_only_handle = NULL;
   void *dsa_handle = NULL;
   void *lequal_dsa_handle = NULL;
   void *read_only_dsa_handle = NULL;
   void *alpha_dsa_handle = NULL;
   void *rasterizer_handle = NULL;
   void *scissor_rasterizer_handle = NULL;
   void *elements_handle = NULL;
   void *solid_elements_handle = NULL;
   void *vs = NULL;
   void *fs = NULL;
   void *solid_vs = NULL;
   void *solid_fs = NULL;
   void *unsupported_solid_fs = NULL;
   void *product_fs[3] = { NULL };
   void *unsupported_product_fs = NULL;
   void *scalar_gray_vs = NULL, *scalar_gray_fs = NULL;
   void *flat_scalar_gray_fs = NULL;
   void *scalar_blue_fs = NULL;
   void *literal_color_fs = NULL;
   void *generic_rgba_vs = NULL, *generic_rgba_fs = NULL;
   void *generic_rgbx_vs = NULL, *generic_rgbx_fs = NULL;
   void *generic_xxy_vs = NULL, *generic_xxy_fs = NULL;
   void *program_fs = NULL;
   void *program_c0_fs = NULL;
   void *program_add_fs = NULL;
   void *rejected_program_fs[
      SMOKE_PROGRAM_FS_COUNT - SMOKE_PROGRAM_FS_SATURATE] = { NULL };
   bool rejected_programs_valid = true;
   struct pipe_resource *solid_buffer = NULL;
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
   multiply = source_over;
   multiply.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_DST_COLOR;
   multiply.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_SRC_COLOR;
   multiply.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_DST_COLOR;
   multiply.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_SRC_COLOR;
   if (!depth_resource &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS))
      multiply_handle = context->create_blend_state(context, &multiply);
   if (!depth_resource &&
       resource->format == PIPE_FORMAT_B8G8R8A8_UNORM) {
      alpha_only.rt[0].colormask = PIPE_MASK_A;
      alpha_only_handle = context->create_blend_state(context, &alpha_only);
   }
   if (depth_resource) {
      struct pipe_depth_stencil_alpha_state lequal_dsa;

      dsa.depth_enabled = true;
      dsa.depth_writemask = true;
      dsa.depth_func = PIPE_FUNC_LESS;
      framebuffer.zsbuf.format = PIPE_FORMAT_Z16_UNORM;
      framebuffer.zsbuf.texture = depth_resource;
      vertex_buffer.buffer.user = far_vertices;
      if (chrome9_screen_has_capability(screen,
                                        DRM_CHROME9_CAP_DEPTH_FUNC)) {
         lequal_dsa = dsa;
         lequal_dsa.depth_func = PIPE_FUNC_LEQUAL;
         lequal_dsa_handle = context->create_depth_stencil_alpha_state(
            context, &lequal_dsa);
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_DEPTH_READ_ONLY)) {
         read_only_dsa = dsa;
         read_only_dsa.depth_writemask = false;
         read_only_dsa.depth_func = PIPE_FUNC_LEQUAL;
         read_only_dsa_handle = context->create_depth_stencil_alpha_state(
            context, &read_only_dsa);
      }
   }
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   alpha_dsa = dsa;
   alpha_dsa.alpha_enabled = true;
   alpha_dsa.alpha_func = PIPE_FUNC_LESS;
   alpha_dsa.alpha_ref_value = 0.5f;
   if (chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_ALPHA_TEST))
      alpha_dsa_handle =
         context->create_depth_stencil_alpha_state(context, &alpha_dsa);
   rasterizer.half_pixel_center = true;
   rasterizer.bottom_edge_rule = true;
   rasterizer.depth_clip_near = true;
   rasterizer.depth_clip_far = true;
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   scissor_rasterizer = rasterizer;
   scissor_rasterizer.scissor = true;
   if (!depth_resource &&
       chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_SCISSOR))
      scissor_rasterizer_handle =
         context->create_rasterizer_state(context, &scissor_rasterizer);
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
   if (depth_resource && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH)) {
      program_fs = make_program_backend_fs(screen, context,
                                           SMOKE_PROGRAM_FS_VALID);
      program_c0_fs = make_constant_color_fs(screen, context, 0);
      if (chrome9_screen_has_capability(screen,
                                        DRM_CHROME9_CAP_FRAGMENT_ADD))
         program_add_fs = make_program_backend_fs(
            screen, context, SMOKE_PROGRAM_FS_ADD);
      for (unsigned i = 0; i < ARRAY_SIZE(rejected_program_fs); i++)
         rejected_program_fs[i] = make_program_backend_fs(
            screen, context,
            (enum smoke_program_fs_mode)(SMOKE_PROGRAM_FS_SATURATE + i));
      for (unsigned i = 0; i < ARRAY_SIZE(rejected_program_fs); i++) {
         struct chrome9_shader *rejected = rejected_program_fs[i];
         enum smoke_program_fs_mode mode =
            (enum smoke_program_fs_mode)(SMOKE_PROGRAM_FS_SATURATE + i);
         bool exact_abi_reject = mode == SMOKE_PROGRAM_FS_SAME_FILE_MUL ||
                                 mode == SMOKE_PROGRAM_FS_ALPHA_SWIZZLE;
         /* c0*c0 exceeds cmd19's source-file rule but is valid in cmd22. */
         bool generic_supported = mode == SMOKE_PROGRAM_FS_SAME_FILE_MUL &&
            chrome9_screen_has_capability(
               screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM);

         if (!rejected || rejected->supported != generic_supported ||
             (generic_supported &&
              (rejected->fragment_program != CHROME9_FS_COMPILED_ALU ||
               !rejected->semantic_fs_valid ||
               rejected->semantic_input_mask)) ||
             (exact_abi_reject ?
                 (!rejected->native_fs_valid ||
                  !rejected->native_fs_from_nir) :
                  rejected->native_fs_valid))
            rejected_programs_valid = false;
      }
   }
   if (!depth_resource) {
      solid_buffer = screen->resource_create(screen, &solid_buffer_template);
      solid_elements_handle =
         context->create_vertex_elements_state(context, 1, elements);
      solid_vs = make_solid_vertex_shader(screen, context);
      solid_fs = make_constant_color_fs(screen, context, 0);
      /* c1 is legal; keep negative controls beyond the advertised bank. */
      unsupported_solid_fs = make_constant_color_fs(
         screen, context,
         DRM_CHROME9_FRAGMENT_MAX_CONSTANTS * 4 * sizeof(float));
      for (unsigned i = 0; i < ARRAY_SIZE(product_fs); i++)
         product_fs[i] = make_constant_product_fs(screen, context, i, 16);
      unsupported_product_fs = make_constant_product_fs(
         screen, context, 0,
         DRM_CHROME9_FRAGMENT_MAX_CONSTANTS * 4 * sizeof(float));
      scalar_blue_fs =
         make_scalar_blue_rgbx_fragment_shader(screen, context);
      literal_color_fs = make_literal_color_fragment_shader(screen, context);
      generic_rgba_vs = make_generic_rgba_vertex_shader(screen, context);
      generic_rgba_fs = make_generic_rgba_fragment_shader(screen, context);
      generic_rgbx_vs = make_generic_rgbx_vertex_shader(screen, context);
      generic_rgbx_fs = make_generic_rgbx_fragment_shader(screen, context);
      generic_xxy_vs = make_generic_xxy_vertex_shader(screen, context);
      generic_xxy_fs = make_generic_xxy_fragment_shader(screen, context);
      if (!generic_rgbx_vs || !generic_rgbx_fs ||
          ((struct chrome9_shader *)generic_rgbx_fs)->fragment_program !=
             CHROME9_FS_PRIMARY_GENERIC_RGBX)
         fprintf(stderr,
                 "vec3 VAR0 RGBX setup failed: vs=%p fs=%p program=%u\n",
                 generic_rgbx_vs, generic_rgbx_fs,
                 generic_rgbx_fs ?
                    ((struct chrome9_shader *)generic_rgbx_fs)->fragment_program :
                    CHROME9_FS_UNSUPPORTED);
      if (!generic_xxy_vs || !generic_xxy_fs ||
          ((struct chrome9_shader *)generic_xxy_fs)->fragment_program !=
             CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX)
         fprintf(stderr,
                 "vec2 VAR0 XXY RGBX setup failed: vs=%p fs=%p program=%u\n",
                 generic_xxy_vs, generic_xxy_fs,
                 generic_xxy_fs ?
                    ((struct chrome9_shader *)generic_xxy_fs)->fragment_program :
                    CHROME9_FS_UNSUPPORTED);
      if (!literal_color_fs ||
          ((struct chrome9_shader *)literal_color_fs)->fragment_program !=
             CHROME9_FS_LITERAL_COLOR)
         fprintf(stderr,
                 "literal color setup failed: fs=%p program=%u\n",
                 literal_color_fs,
                 literal_color_fs ?
                    ((struct chrome9_shader *)literal_color_fs)->fragment_program :
                    CHROME9_FS_UNSUPPORTED);
   }
   scalar_gray_vs = make_scalar_generic_vertex_shader(screen, context, false);
   scalar_gray_fs = make_scalar_rgbx_fragment_shader(screen, context);
   flat_scalar_gray_fs =
      make_flat_scalar_rgbx_fragment_shader(screen, context);
   if (!blend_handle ||
       (!depth_resource &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND) &&
        !source_over_handle) ||
       (!depth_resource &&
        chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS) &&
        !multiply_handle) ||
       (!depth_resource &&
        resource->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
        !alpha_only_handle) ||
       (!depth_resource &&
        chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_SCISSOR) &&
        !scissor_rasterizer_handle) ||
       !dsa_handle ||
       (depth_resource &&
       chrome9_screen_has_capability(screen,
                                      DRM_CHROME9_CAP_DEPTH_FUNC) &&
        !lequal_dsa_handle) ||
       (depth_resource &&
        chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DEPTH_READ_ONLY) &&
        !read_only_dsa_handle) ||
       !rasterizer_handle ||
       (chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_ALPHA_TEST) &&
        !alpha_dsa_handle) ||
       !elements_handle || !vs || !fs ||
       !((struct chrome9_shader *)fs)->native_fs_valid ||
       (depth_resource && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH) &&
        (!((struct chrome9_shader *)fs)->native_fs_from_nir ||
         !program_fs ||
         !program_c0_fs ||
         (chrome9_screen_has_capability(screen,
                                         DRM_CHROME9_CAP_FRAGMENT_ADD) &&
          (!program_add_fs ||
           !((struct chrome9_shader *)program_add_fs)->supported ||
           ((struct chrome9_shader *)program_add_fs)->fragment_program !=
              CHROME9_FS_COMPILED_ALU ||
           ((struct chrome9_shader *)program_add_fs)->semantic_fs_ir.count != 1 ||
           ((struct chrome9_shader *)program_add_fs)->
              semantic_fs_ir.instruction[0].op != CHROME9_PS_OPCODE_ADD)) ||
         !((struct chrome9_shader *)program_c0_fs)->supported ||
         !((struct chrome9_shader *)program_c0_fs)->native_fs_valid ||
         !((struct chrome9_shader *)program_c0_fs)->native_fs_from_nir ||
         ((struct chrome9_shader *)program_c0_fs)->native_fs_key.op !=
            CHROME9_FS_OP_MOV ||
         ((struct chrome9_shader *)program_c0_fs)->native_fs_key.src[0] !=
            CHROME9_FS_LEAF_C0 ||
         !((struct chrome9_shader *)program_fs)->supported ||
         ((struct chrome9_shader *)program_fs)->fragment_program !=
            CHROME9_FS_COMPILED_ALU ||
         !((struct chrome9_shader *)program_fs)->native_fs_valid ||
         !((struct chrome9_shader *)program_fs)->native_fs_from_nir ||
         ((struct chrome9_shader *)program_fs)->native_fs_key.op !=
            CHROME9_FS_OP_MUL ||
         ((struct chrome9_shader *)program_fs)->native_fs_key.src[0] !=
            CHROME9_FS_LEAF_V0 ||
         ((struct chrome9_shader *)program_fs)->native_fs_key.src[1] !=
            CHROME9_FS_LEAF_C0 ||
         !rejected_programs_valid)) ||
       !scalar_gray_vs ||
       !scalar_gray_fs ||
       ((struct chrome9_shader *)scalar_gray_fs)->fragment_program !=
          CHROME9_FS_PRIMARY_SCALAR_RGBX ||
       (!depth_resource &&
        (!solid_buffer || !solid_elements_handle || !solid_vs || !solid_fs ||
         !((struct chrome9_shader *)solid_fs)->supported ||
         !((struct chrome9_shader *)solid_fs)->native_fs_valid ||
         !unsupported_solid_fs ||
         ((struct chrome9_shader *)unsupported_solid_fs)->supported ||
         !product_fs[0] || !product_fs[1] || !product_fs[2] ||
         ((struct chrome9_shader *)product_fs[0])->fragment_program !=
            CHROME9_FS_CONSTANT_MODULATE ||
         ((struct chrome9_shader *)product_fs[1])->fragment_program !=
            CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE ||
         ((struct chrome9_shader *)product_fs[2])->fragment_program !=
            CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE ||
         !((struct chrome9_shader *)product_fs[0])->native_fs_valid ||
         !((struct chrome9_shader *)product_fs[1])->native_fs_valid ||
         !((struct chrome9_shader *)product_fs[2])->native_fs_valid ||
         !unsupported_product_fs ||
         ((struct chrome9_shader *)unsupported_product_fs)->supported ||
         !scalar_blue_fs ||
         ((struct chrome9_shader *)scalar_blue_fs)->fragment_program !=
            CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX ||
         !generic_rgba_vs || !generic_rgba_fs ||
         ((struct chrome9_shader *)generic_rgba_fs)->fragment_program !=
            CHROME9_FS_PRIMARY_GENERIC_RGBA ||
         !generic_rgbx_vs || !generic_rgbx_fs ||
         ((struct chrome9_shader *)generic_rgbx_fs)->fragment_program !=
            CHROME9_FS_PRIMARY_GENERIC_RGBX)))
      goto out;
   if (solid_buffer) {
      memcpy(chrome9_resource(solid_buffer)->bo.map, solid_buffer_color,
             sizeof(solid_buffer_color));
      solid_buffer_constant.buffer = solid_buffer;
   }

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
   initial_batches = chrome9_context(context)->submitted_batches;
   initial_program_batches =
      chrome9_context(context)->submitted_program_batches;
   initial_rhw_runs = chrome9_context(context)->submitted_rhw_runs;
   initial_rhw_chains = chrome9_context(context)->submitted_rhw_chains;
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
   passed = changed >= 30000 && changed <= 257U * 129U &&
            (!chrome9_context(context)->draw_triangle_batch ||
             chrome9_context(context)->submitted_batches ==
                initial_batches + 1) &&
            (!depth_resource ||
             (chrome9_context(context)->draw_triangle_d16_rhw_chain ?
                 chrome9_context(context)->submitted_rhw_chains ==
                       initial_rhw_chains + 1 &&
                    chrome9_context(context)->submitted_rhw_runs ==
                       initial_rhw_runs &&
                    chrome9_context(context)->submitted_program_batches ==
                       initial_program_batches :
              chrome9_context(context)->draw_triangle_d16_rhw_run ?
                 chrome9_context(context)->submitted_rhw_runs ==
                       initial_rhw_runs + 1 &&
                    chrome9_context(context)->submitted_program_batches ==
                       initial_program_batches :
                 chrome9_context(context)->submitted_program_batches ==
                       initial_program_batches &&
                    chrome9_context(context)->submitted_rhw_runs ==
                       initial_rhw_runs));
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
      if (passed && lequal_dsa_handle) {
         uint64_t submitted = chrome9_context(context)->submitted_triangles;
         uint64_t rejected = chrome9_context(context)->rejected_triangles;
         uint32_t equal_pixel, far_pixel, near_pixel;
         uint16_t equal_depth, far_depth, near_depth;

         context->bind_depth_stencil_alpha_state(context,
                                                  lequal_dsa_handle);
         context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                        PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 0.5, 0);
         vertex_buffer.buffer.user = rejected_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         equal_pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
         equal_depth = depth_pixels[32 * (depth->stride / 2) + 192];

         vertex_buffer.buffer.user = far_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         far_pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
         far_depth = depth_pixels[32 * (depth->stride / 2) + 192];

         vertex_buffer.buffer.user = near_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         near_pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
         near_depth = depth_pixels[32 * (depth->stride / 2) + 192];
         passed = equal_pixel == 0x000000ff &&
                  equal_depth >= 0x7ffe && equal_depth <= 0x8001 &&
                  far_pixel == equal_pixel &&
                  far_depth >= 0x7ffe && far_depth <= 0x8001 &&
                  near_pixel == 0x00ff0000 &&
                  near_depth >= 0x3ffe && near_depth <= 0x4001 &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 6 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface) &&
                  smoke_guard_pages_match(depth);
         if (passed)
            printf("Gallium D16 LEQUAL passed: "
                   "equal=%08x/%04x far=%08x/%04x near=%08x/%04x\n",
                   equal_pixel, equal_depth, far_pixel, far_depth,
                   near_pixel, near_depth);
         else
            fprintf(stderr,
                    "Gallium D16 LEQUAL mismatch: "
                    "equal=%08x/%04x far=%08x/%04x near=%08x/%04x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    equal_pixel, equal_depth, far_pixel, far_depth,
                    near_pixel, near_depth,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
         context->bind_depth_stencil_alpha_state(context, dsa_handle);
      }
      if (passed && read_only_dsa_handle) {
         uint64_t submitted = chrome9_context(context)->submitted_triangles;
         uint64_t rejected = chrome9_context(context)->rejected_triangles;
         uint32_t equal_pixel, near_pixel, far_pixel;
         uint16_t equal_depth, near_depth, far_depth;

         context->bind_depth_stencil_alpha_state(context,
                                                  read_only_dsa_handle);
         context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                        PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 0.5, 0);
         vertex_buffer.buffer.user = rejected_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         equal_pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
         equal_depth = depth_pixels[32 * (depth->stride / 2) + 192];

         vertex_buffer.buffer.user = near_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         near_pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
         near_depth = depth_pixels[32 * (depth->stride / 2) + 192];

         vertex_buffer.buffer.user = far_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         far_pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
         far_depth = depth_pixels[32 * (depth->stride / 2) + 192];
         passed = equal_pixel == 0x000000ff &&
                  near_pixel == 0x00ff0000 && far_pixel == near_pixel &&
                  equal_depth >= 0x7ffe && equal_depth <= 0x8001 &&
                  near_depth >= 0x7ffe && near_depth <= 0x8001 &&
                  far_depth >= 0x7ffe && far_depth <= 0x8001 &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 6 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface) &&
                  smoke_guard_pages_match(depth);
         if (passed)
            printf("Gallium D16 read-only LEQUAL passed: "
                   "equal=%08x/%04x near=%08x/%04x far=%08x/%04x\n",
                   equal_pixel, equal_depth, near_pixel, near_depth,
                   far_pixel, far_depth);
         else
            fprintf(stderr,
                    "Gallium D16 read-only LEQUAL mismatch: "
                    "equal=%08x/%04x near=%08x/%04x far=%08x/%04x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    equal_pixel, equal_depth, near_pixel, near_depth,
                    far_pixel, far_depth,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
         context->bind_depth_stencil_alpha_state(context, dsa_handle);
      }
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
      if (passed && multiply_handle) {
         unsigned alpha, red, green_channel, blue;

         context->bind_blend_state(context, multiply_handle);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &alpha_mask_base, 1.0, 0);
         vertex_buffer.buffer.user = generic_rgba_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         alpha = pixel >> 24;
         red = pixel >> 16 & 0xff;
         green_channel = pixel >> 8 & 0xff;
         blue = pixel & 0xff;
         passed = abs((int)red - 0x1a) <= 1 &&
                  abs((int)green_channel - 0x22) <= 1 &&
                  abs((int)blue - 0x1a) <= 1 &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   abs((int)alpha - 0x44) <= 1) &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium DST_COLOR/SRC_COLOR blend passed: "
                   "sample=%08x\n", pixel);
         else
            fprintf(stderr,
                    "Gallium DST_COLOR/SRC_COLOR blend mismatch: "
                    "sample=%08x\n", pixel);
      }
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
   if (passed && alpha_dsa_handle) {
      struct chrome9_resource *depth_surface = depth_resource ?
         chrome9_resource(depth_resource) : NULL;
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      uint32_t pixel;
      uint16_t alpha_depth = 0xffff;

      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, alpha_dsa_handle);
      context->bind_rasterizer_state(context, rasterizer_handle);
      context->clear(context, PIPE_CLEAR_COLOR0 |
                                (depth_resource ? PIPE_CLEAR_DEPTH : 0),
                     PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
      vertex_buffer.buffer.user = depth_resource ?
         alpha_near_kill_vertices : alpha_far_pass_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      vertex_buffer.buffer.user = depth_resource ?
         alpha_far_pass_vertices : alpha_near_kill_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      pixel = pixels[32 * (surface->stride / 4) + 192] & 0xffffff;
      if (depth_surface) {
         uint16_t *depth_pixels = (uint16_t *)
            ((uint8_t *)depth_surface->bo.map + depth_surface->data_offset);
         alpha_depth = depth_pixels[32 * (depth_surface->stride / 2) + 192];
      }
      passed = pixel == 0x0000ff00 &&
               (!depth_resource ||
                (alpha_depth >= 0x7ffe && alpha_depth <= 0x8001)) &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 4 &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface) &&
               (!depth_surface || smoke_guard_pages_match(depth_surface));
      if (passed)
         printf("Gallium alpha LESS/ref0.5%s passed: sample=%08x/%04x\n",
                depth_resource ? " before D16" : "", pixel, alpha_depth);
      else
         fprintf(stderr,
                 "Gallium alpha%s mismatch: sample=%08x/%04x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 depth_resource ? "/D16" : "", pixel, alpha_depth,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->rejected_triangles - rejected);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
   }
   if (passed && scissor_rasterizer_handle) {
      const struct pipe_scissor_state probe = {
         .minx = 64, .miny = 32, .maxx = 128, .maxy = 64,
      };
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      uint32_t inside, left, right;

      context->bind_blend_state(context, source_over_handle);
      context->bind_rasterizer_state(context, scissor_rasterizer_handle);
      context->set_scissor_states(context, 0, 1, &probe);
      vertex_buffer.buffer.user = blend_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear_color, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      inside = pixels[48 * (surface->stride / 4) + 96] & 0xffffff;
      left = pixels[48 * (surface->stride / 4) + 32] & 0xffffff;
      right = pixels[48 * (surface->stride / 4) + 192] & 0xffffff;
      passed = inside >= 0x007f0000 && inside <= 0x00800000 &&
               !left && !right &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 2 &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface);
      if (passed)
         printf("Gallium scissor passed: samples=%08x/%08x/%08x\n",
                left, inside, right);
      else
         fprintf(stderr,
                 "Gallium scissor mismatch: samples=%08x/%08x/%08x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 left, inside, right,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->rejected_triangles - rejected);
   }
   if (passed && !depth_resource) {
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      uint32_t pixel;

      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
      context->bind_rasterizer_state(context, rasterizer_handle);
      context->set_scissor_states(context, 0, 1, &scissor);
      context->bind_vertex_elements_state(context, solid_elements_handle);
      context->bind_vs_state(context, solid_vs);
      context->bind_fs_state(context, solid_fs);
      vertex_buffer.buffer.user = vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &solid_constant);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear_color, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      pixel = pixels[32 * (surface->stride / 4) + 192];
      passed = (pixel & 0xffffff) == 0x4080bf &&
               (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                (pixel >> 24) == 0xff) &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 2 &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface);
      if (passed)
         printf("Gallium c0 solid shader passed: sample=%08x\n", pixel);
      else
         fprintf(stderr,
                 "Gallium c0 solid shader mismatch: sample=%08x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 pixel,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->rejected_triangles - rejected);
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                      &solid_buffer_constant);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0xff0080 &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium resource c0 solid shader passed: sample=%08x\n",
                   pixel);
         else
            fprintf(stderr,
                    "Gallium resource c0 solid shader mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_vertex_elements_state(context, elements_handle);
         context->bind_vs_state(context, scalar_gray_vs);
         context->bind_fs_state(context, flat_scalar_gray_fs);
         vertex_buffer.buffer.user = flat_gray_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0xbfbfbf &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium flat scalar VAR0 RGBX passed: sample=%08x\n",
                   pixel);
         else
            fprintf(stderr,
                    "Gallium flat scalar VAR0 RGBX mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_vs_state(context, vs);
         context->bind_fs_state(context, literal_color_fs);
         vertex_buffer.buffer.user = vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0x4080bf &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium literal color passed: sample=%08x\n", pixel);
         else
            fprintf(stderr,
                    "Gallium literal color mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      for (unsigned mode = 0; passed && mode < ARRAY_SIZE(product_fs);
           mode++) {
         static const uint32_t expected[3] = {
            0x30661a33, 0x30331a0d, 0x306030bf,
         };
         bool channels_match = true;

         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_fs_state(context, product_fs[mode]);
         context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                      &product_constant);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         for (unsigned shift = 0; shift <= 16; shift += 8)
            channels_match &=
               abs((int)(pixel >> shift & 0xff) -
                   (int)(expected[mode] >> shift & 0xff)) <= 1;
         if (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM)
            channels_match &=
               abs((int)(pixel >> 24) -
                   (int)(expected[mode] >> 24)) <= 1;
         passed = channels_match &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium c0/c1 product mode %u passed: sample=%08x\n",
                   mode, pixel);
         else
            fprintf(stderr,
                    "Gallium c0/c1 product mode %u mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    mode, pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_vertex_elements_state(context, elements_handle);
         context->bind_vs_state(context, scalar_gray_vs);
         context->bind_fs_state(context, scalar_gray_fs);
         vertex_buffer.buffer.user = scalar_gray_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0x404040 &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium scalar VAR0 RGBX passed: sample=%08x\n", pixel);
         else
            fprintf(stderr,
                    "Gallium scalar VAR0 RGBX mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_fs_state(context, scalar_blue_fs);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0x000040 &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium scalar blue VAR0 RGBX passed: sample=%08x\n",
                   pixel);
         else
            fprintf(stderr,
                    "Gallium scalar blue VAR0 RGBX mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_vs_state(context, generic_rgba_vs);
         context->bind_fs_state(context, generic_rgba_fs);
         vertex_buffer.buffer.user = generic_rgba_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0x4080bf &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   ((pixel >> 24) >= 0x7f && (pixel >> 24) <= 0x80)) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium vec4 VAR0 RGBA passed: sample=%08x\n", pixel);
         else
            fprintf(stderr,
                    "Gallium vec4 VAR0 RGBA mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_vs_state(context, generic_rgbx_vs);
         context->bind_fs_state(context, generic_rgbx_fs);
         vertex_buffer.buffer.user = generic_rgba_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0x4080bf &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium vec3 VAR0 RGBX passed: sample=%08x\n", pixel);
         else
            fprintf(stderr,
                    "Gallium vec3 VAR0 RGBX mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
      if (passed) {
         submitted = chrome9_context(context)->submitted_triangles;
         rejected = chrome9_context(context)->rejected_triangles;
         context->bind_vs_state(context, generic_xxy_vs);
         context->bind_fs_state(context, generic_xxy_fs);
         vertex_buffer.buffer.user = generic_rgba_vertices;
         context->set_vertex_buffers(context, 1, &vertex_buffer);
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear_color, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush_resource(context, resource);
         context->flush(context, NULL, 0);
         pixel = pixels[32 * (surface->stride / 4) + 192];
         passed = (pixel & 0xffffff) == 0x404080 &&
                  (resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                   (pixel >> 24) == 0xff) &&
                  chrome9_context(context)->submitted_triangles ==
                     submitted + 2 &&
                  chrome9_context(context)->rejected_triangles == rejected &&
                  smoke_guard_pages_match(draw_surface);
         if (passed)
            printf("Gallium vec2 VAR0 XXY RGBX passed: sample=%08x\n", pixel);
         else
            fprintf(stderr,
                    "Gallium vec2 VAR0 XXY RGBX mismatch: sample=%08x "
                    "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                    pixel,
                    chrome9_context(context)->submitted_triangles - submitted,
                    chrome9_context(context)->rejected_triangles - rejected);
      }
   }
   if (passed && depth_resource &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_BATCH)) {
      const struct pipe_viewport_state perspective_viewport = {
         .scale = { 32.0f, 32.0f, 0.5f },
         .translate = { 31.5f, 31.5f, 0.5f },
      };
      const struct pipe_draw_info perspective_info = {
         .mode = MESA_PRIM_TRIANGLES,
         .index_bounds_valid = true,
         .instance_count = 1,
         .min_index = 0,
         .max_index = 2,
      };
      const struct pipe_draw_start_count_bias perspective_draw = {
         .start = 0,
         .count = 3,
      };
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      uint32_t pixel;
      unsigned red;

      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
      context->bind_vertex_elements_state(context, elements_handle);
      context->bind_vs_state(context, scalar_gray_vs);
      context->bind_fs_state(context, scalar_gray_fs);
      vertex_buffer.buffer.user = scalar_perspective_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_viewport_states(context, 0, 1, &perspective_viewport);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
      context->draw_vbo(context, &perspective_info, 0, NULL,
                        &perspective_draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      context->set_viewport_states(context, 0, 1, &viewport);
      pixel = pixels[16 * (surface->stride / 4) + 48];
      red = pixel >> 16 & 0xff;
      passed = red >= 0x6f && red <= 0x74 &&
               (pixel & 0xff) == red && (pixel >> 8 & 0xff) == red &&
               depth_pixels[16 * (depth->stride / 2) + 48] >= 0x7ffe &&
               depth_pixels[16 * (depth->stride / 2) + 48] <= 0x8001 &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 1 &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface) &&
               smoke_guard_pages_match(depth);
      if (passed)
         printf("Gallium untextured D16 varying-RHW passed: sample=%08x\n",
                pixel);
      else
         fprintf(stderr,
                 "Gallium untextured D16 varying-RHW mismatch: "
                 "sample=%08x expected=006f6f6f..00747474 "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 pixel,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->rejected_triangles - rejected);
   }
   if (passed && depth_resource && program_c0_fs) {
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t program_batches =
         chrome9_context(context)->submitted_program_batches;
      uint64_t rhw_runs = chrome9_context(context)->submitted_rhw_runs;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      /* Both the compiled and generic program paths retain the two
       * triangles in one immutable-state batch. */
      uint32_t pixel;

      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
      context->bind_rasterizer_state(context, rasterizer_handle);
      context->bind_vertex_elements_state(context, elements_handle);
      context->bind_vs_state(context, vs);
      context->bind_fs_state(context, program_c0_fs);
      vertex_buffer.buffer.user = generic_rgba_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &program_constant);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      pixel = pixels[32 * (surface->stride / 4) + 192];
      passed = (pixel & 0xffffff) >= 0x7f7f7f &&
               (pixel & 0xffffff) <= 0x808080 &&
               depth_pixels[32 * (depth->stride / 2) + 192] >= 0x7ffe &&
               depth_pixels[32 * (depth->stride / 2) + 192] <= 0x8001 &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 2 &&
               chrome9_context(context)->submitted_program_batches ==
                  program_batches + 1 &&
               chrome9_context(context)->submitted_rhw_runs == rhw_runs &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface) &&
               smoke_guard_pages_match(depth);
      if (passed)
         printf("Gallium compiled MOV c0 D16 passed: sample=%08x\n", pixel);
      else
         fprintf(stderr,
                 "Gallium compiled MOV c0 D16 mismatch: sample=%08x "
                 "submitted=+%" PRIu64 " program=+%" PRIu64
                 " runs=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 pixel,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->submitted_program_batches -
                    program_batches,
                 chrome9_context(context)->submitted_rhw_runs - rhw_runs,
                 chrome9_context(context)->rejected_triangles - rejected);
   }
   if (passed && depth_resource && program_fs) {
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t batches =
         chrome9_context(context)->submitted_program_batches;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      uint32_t pixel;
      unsigned red, green_channel, blue;

      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
      context->bind_rasterizer_state(context, rasterizer_handle);
      context->bind_vertex_elements_state(context, elements_handle);
      context->bind_vs_state(context, vs);
      context->bind_fs_state(context, program_fs);
      vertex_buffer.buffer.user = generic_rgba_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &program_constant);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      pixel = pixels[32 * (surface->stride / 4) + 192];
      red = pixel >> 16 & 0xff;
      green_channel = pixel >> 8 & 0xff;
      blue = pixel & 0xff;
      passed = red >= 0x1f && red <= 0x21 &&
               green_channel >= 0x3f && green_channel <= 0x41 &&
               blue >= 0x5f && blue <= 0x61 &&
               depth_pixels[32 * (depth->stride / 2) + 192] >= 0x7ffe &&
               depth_pixels[32 * (depth->stride / 2) + 192] <= 0x8001 &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 2 &&
               chrome9_context(context)->submitted_program_batches ==
                  batches + 1 &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface) &&
               smoke_guard_pages_match(depth);
      if (passed)
         printf("Gallium compiled v0*c0 D16 passed: sample=%08x\n", pixel);
      else
         fprintf(stderr,
                 "Gallium compiled v0*c0 D16 mismatch: sample=%08x "
                 "submitted=+%" PRIu64 " batches=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 pixel,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->submitted_program_batches -
                    batches,
                 chrome9_context(context)->rejected_triangles - rejected);
   }
   if (passed && depth_resource && program_add_fs) {
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);
      uint64_t submitted = chrome9_context(context)->submitted_triangles;
      uint64_t batches =
         chrome9_context(context)->submitted_program_batches;
      uint64_t rejected = chrome9_context(context)->rejected_triangles;
      uint32_t pixel;
      unsigned red, green_channel, blue;

      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
      context->bind_rasterizer_state(context, rasterizer_handle);
      context->bind_vertex_elements_state(context, elements_handle);
      context->bind_vs_state(context, vs);
      context->bind_fs_state(context, program_add_fs);
      vertex_buffer.buffer.user = generic_rgba_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &add_program_constant);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, resource);
      context->flush(context, NULL, 0);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      pixel = pixels[32 * (surface->stride / 4) + 192];
      red = pixel >> 16 & 0xff;
      green_channel = pixel >> 8 & 0xff;
      blue = pixel & 0xff;
      passed = red >= 0x5f && red <= 0x61 &&
               green_channel >= 0x9e && green_channel <= 0xa0 &&
               blue >= 0xde && blue <= 0xe0 &&
               depth_pixels[32 * (depth->stride / 2) + 192] >= 0x7ffe &&
               depth_pixels[32 * (depth->stride / 2) + 192] <= 0x8001 &&
               chrome9_context(context)->submitted_triangles ==
                  submitted + 2 &&
               chrome9_context(context)->submitted_program_batches ==
                  batches + 1 &&
               chrome9_context(context)->rejected_triangles == rejected &&
               smoke_guard_pages_match(draw_surface) &&
               smoke_guard_pages_match(depth);
      if (passed)
         printf("Gallium compiled v0+c0 D16 passed: sample=%08x\n", pixel);
      else
         fprintf(stderr,
                 "Gallium compiled v0+c0 D16 mismatch: sample=%08x "
                 "submitted=+%" PRIu64 " batches=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 pixel,
                 chrome9_context(context)->submitted_triangles - submitted,
                 chrome9_context(context)->submitted_program_batches -
                    batches,
                 chrome9_context(context)->rejected_triangles - rejected);
   }
   if (!passed)
      fprintf(stderr,
              "Gallium quad-strip coverage/depth mismatch: %zu "
              "sample=%08x/%04x batches=+%" PRIu64
              " program-batches=+%" PRIu64 " rhw-runs=+%" PRIu64
              " rhw-chains=+%" PRIu64 "\n",
              changed, pixels[32 * (surface->stride / 4) + 192],
              sample_depth,
              chrome9_context(context)->submitted_batches - initial_batches,
              chrome9_context(context)->submitted_program_batches -
                 initial_program_batches,
              chrome9_context(context)->submitted_rhw_runs -
                 initial_rhw_runs,
              chrome9_context(context)->submitted_rhw_chains -
                 initial_rhw_chains);
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
      if (solid_vs)
         context->delete_vs_state(context, solid_vs);
      if (solid_fs)
         context->delete_fs_state(context, solid_fs);
      if (unsupported_solid_fs)
         context->delete_fs_state(context, unsupported_solid_fs);
      for (unsigned i = 0; i < ARRAY_SIZE(product_fs); i++)
         if (product_fs[i])
            context->delete_fs_state(context, product_fs[i]);
      if (unsupported_product_fs)
         context->delete_fs_state(context, unsupported_product_fs);
      if (scalar_gray_vs)
         context->delete_vs_state(context, scalar_gray_vs);
      if (scalar_gray_fs)
         context->delete_fs_state(context, scalar_gray_fs);
      if (flat_scalar_gray_fs)
         context->delete_fs_state(context, flat_scalar_gray_fs);
      if (scalar_blue_fs)
         context->delete_fs_state(context, scalar_blue_fs);
      if (literal_color_fs)
         context->delete_fs_state(context, literal_color_fs);
      if (generic_rgba_vs)
         context->delete_vs_state(context, generic_rgba_vs);
      if (generic_rgba_fs)
         context->delete_fs_state(context, generic_rgba_fs);
      if (generic_rgbx_vs)
         context->delete_vs_state(context, generic_rgbx_vs);
      if (generic_rgbx_fs)
         context->delete_fs_state(context, generic_rgbx_fs);
      if (generic_xxy_vs)
         context->delete_vs_state(context, generic_xxy_vs);
      if (generic_xxy_fs)
         context->delete_fs_state(context, generic_xxy_fs);
      if (program_fs)
         context->delete_fs_state(context, program_fs);
      if (program_c0_fs)
         context->delete_fs_state(context, program_c0_fs);
      if (program_add_fs)
         context->delete_fs_state(context, program_add_fs);
      for (unsigned i = 0; i < ARRAY_SIZE(rejected_program_fs); i++)
         if (rejected_program_fs[i])
            context->delete_fs_state(context, rejected_program_fs[i]);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (solid_elements_handle)
         context->delete_vertex_elements_state(context,
                                               solid_elements_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (scissor_rasterizer_handle)
         context->delete_rasterizer_state(context,
                                          scissor_rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (lequal_dsa_handle)
         context->delete_depth_stencil_alpha_state(context,
                                                   lequal_dsa_handle);
      if (read_only_dsa_handle)
         context->delete_depth_stencil_alpha_state(context,
                                                   read_only_dsa_handle);
      if (alpha_dsa_handle)
         context->delete_depth_stencil_alpha_state(context,
                                                   alpha_dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      if (source_over_handle)
         context->delete_blend_state(context, source_over_handle);
      if (multiply_handle)
         context->delete_blend_state(context, multiply_handle);
      if (alpha_only_handle)
         context->delete_blend_state(context, alpha_only_handle);
      context->destroy(context);
   }
   if (solid_buffer)
      screen->resource_destroy(screen, solid_buffer);
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
   static const struct smoke_texture_vertex packed_gray_vertices[] = {
      { { -1, -1, 0, 1 }, { 0.5, 0, 0, 0 }, { 0, 0 } },
      { { -1,  1, 0, 1 }, { 0.5, 0, 0, 0 }, { 0, 1 } },
      { {  1, -1, 0, 1 }, { 0.5, 0, 0, 0 }, { 1, 0 } },
      { {  1,  1, 0, 1 }, { 0.5, 0, 0, 0 }, { 1, 1 } },
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
   static const struct smoke_projected_texture_vertex projected_vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 0.25 }, { 0, 0.5, 0, 1 } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 0.25 }, { 0, 0.5, 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 0.25 }, { 1, 0.5, 0, 1 } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 0.25 }, { 1, 0.5, 0, 1 } },
   };
   static const struct smoke_projected_texture_vertex bad_q_vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 0.25 }, { 0, 0.5, 0, 2 } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 0.25 }, { 0, 0.5, 0, 2 } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 0.25 }, { 1, 0.5, 0, 2 } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 0.25 }, { 1, 0.5, 0, 2 } },
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
      .last_level = 1,
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
   struct pipe_resource *shadow_source = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_templ, rhw_view_templ;
   struct pipe_sampler_view *view = NULL, *mip_view = NULL, *rhw_view = NULL;
   struct pipe_sampler_view *bad_view = NULL;
   struct pipe_sampler_view *shadow_view = NULL;
   struct winsys_handle shadow_handle = { .type = WINSYS_HANDLE_TYPE_FD };
   struct pipe_transfer *mip_transfer = NULL;
   struct pipe_box mip_box = {
      .x = 0, .width = 1, .y = 0, .height = 1, .z = 0, .depth = 1,
   };
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
   struct pipe_vertex_element projected_elements[3] = { 0 };
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
   const float texture_constant_color[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
   const float texture_alpha_constant_color[4] = {
      0.25f, 0.75f, 1.0f, 0.25f,
   };
   const float projected_exact_constant_color[4] = { 1, 1, 1, 1 };
   const float projected_inexact_constant_color[4] = {
      0.5f, 0.5f, 0.5f, 1,
   };
   struct pipe_constant_buffer texture_constant = {
      .buffer_size = sizeof(texture_constant_color),
      .user_buffer = texture_constant_color,
   };
   struct pipe_constant_buffer texture_alpha_constant = {
      .buffer_size = sizeof(texture_alpha_constant_color),
      .user_buffer = texture_alpha_constant_color,
   };
   struct pipe_constant_buffer projected_exact_constant = {
      .buffer_size = sizeof(projected_exact_constant_color),
      .user_buffer = projected_exact_constant_color,
   };
   struct pipe_constant_buffer projected_inexact_constant = {
      .buffer_size = sizeof(projected_inexact_constant_color),
      .user_buffer = projected_inexact_constant_color,
   };
   union pipe_color_union clear = { 0 };
   union pipe_color_union green = { .f = { 0, 1, 0, 0 } };
   struct chrome9_resource *ctarget, *ctexture, *cbgra_target = NULL;
   struct chrome9_resource *crhw_target, *crhw_texture;
   struct chrome9_context *ccontext;
   void *blend_handle = NULL, *dsa_handle = NULL, *rasterizer_handle = NULL;
   void *source_over_handle = NULL;
   void *elements_handle = NULL, *replace_elements_handle = NULL;
   void *projected_elements_handle = NULL;
   void *sampler_handle = NULL;
   void *replace_vs = NULL, *modulate_vs = NULL;
   void *replace = NULL, *modulate = NULL, *constant_modulate = NULL;
   void *constant_alpha_modulate = NULL, *rgbx = NULL;
   void *packed_gray_vs = NULL, *packed_gray_fs = NULL;
   void *projected_vs = NULL, *projected_fs = NULL;
   void *projected_modulate_fs = NULL;
   void *projected_constant_fs = NULL;
   void *projected_constant_alpha_fs = NULL;
   void *samplers[1];
   uint32_t source_copy[4], mip_copy = 0x5aa55aa5, *source, *pixels;
   uint32_t *mip_pixel;
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
   if (ctexture->level_stride[0] != 32 ||
       ctexture->level_offset[0] != ctexture->data_offset ||
       ctexture->level_size[0] != ctexture->data_size ||
       ctexture->level_stride[1] != 32 || ctexture->level_size[1] != 32 ||
       (ctexture->level_offset[1] & 0xff) ||
       ctexture->level_offset[1] !=
          ctexture->data_offset +
             ALIGN_POT(ctexture->data_size, ctexture->data_offset) +
             ctexture->data_offset ||
       ctexture->bo.size <
          ctexture->level_offset[1] + ctexture->level_size[1] +
             ctexture->data_offset) {
      fputs("guarded mip resource layout mismatch\n", stderr);
      goto out;
   }
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
   mip_pixel = context->texture_map(context, texture, 1, PIPE_MAP_WRITE,
                                    &mip_box, &mip_transfer);
   if (!mip_pixel || !mip_transfer ||
       mip_transfer->stride != ctexture->level_stride[1] ||
       (uint8_t *)mip_pixel !=
          (uint8_t *)ctexture->bo.map + ctexture->level_offset[1]) {
      fputs("guarded mip level transfer mismatch\n", stderr);
      if (mip_transfer)
         context->texture_unmap(context, mip_transfer);
      goto out;
   }
   *mip_pixel = mip_copy;
   context->texture_unmap(context, mip_transfer);
   mip_transfer = NULL;

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
   projected_elements[0].src_offset =
      offsetof(struct smoke_projected_texture_vertex, position);
   projected_elements[1].src_offset =
      offsetof(struct smoke_projected_texture_vertex, color);
   projected_elements[2].src_offset =
      offsetof(struct smoke_projected_texture_vertex, texcoord);
   for (unsigned i = 0; i < ARRAY_SIZE(projected_elements); i++) {
      projected_elements[i].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      projected_elements[i].src_stride =
         sizeof(struct smoke_projected_texture_vertex);
   }
   projected_elements_handle = context->create_vertex_elements_state(
      context, ARRAY_SIZE(projected_elements), projected_elements);
   sampler_handle = context->create_sampler_state(context, &sampler);
   u_sampler_view_default_template(&view_templ, texture, texture->format);
   mip_view = context->create_sampler_view(context, texture, &view_templ);
   if (!mip_view) {
      fputs("multi-level resource level-zero view rejected\n", stderr);
      goto out;
   }
   view_templ.u.tex.first_level = 0;
   view_templ.u.tex.last_level = 0;
   view = context->create_sampler_view(context, texture, &view_templ);
   view_templ.swizzle_a = PIPE_SWIZZLE_1;
   bad_view = context->create_sampler_view(context, texture, &view_templ);
   if (bad_view) {
      fputs("BGRA constant-one-alpha sampler view unexpectedly accepted\n",
            stderr);
      pipe_sampler_view_reference(&bad_view, NULL);
      goto out;
   }
   view_templ.swizzle_a = PIPE_SWIZZLE_W;
   u_sampler_view_default_template(&rhw_view_templ, rhw_texture,
                                   rhw_texture->format);
   /* Glamor presents XRGB images as identity RGB with constant-one alpha.
    * The native BGRX sampler implements exactly that fourth component. */
   rhw_view_templ.swizzle_a = PIPE_SWIZZLE_1;
   rhw_view = context->create_sampler_view(context, rhw_texture,
                                           &rhw_view_templ);
   if (screen->resource_get_handle(
          screen, NULL, rhw_texture, &shadow_handle,
          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE)) {
      shadow_handle.offset = 0;
      shadow_source = screen->resource_from_handle(
         screen, &rhw_texture_templ, &shadow_handle,
         PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      close(shadow_handle.handle);
   }
   if (shadow_source)
      shadow_view = context->create_sampler_view(context, shadow_source,
                                                 &rhw_view_templ);
   replace_vs = make_texture_vertex_shader(screen, context, false);
   modulate_vs = make_texture_vertex_shader(screen, context, true);
   replace = make_texture_fragment_shader(screen, context, false);
   modulate = make_texture_fragment_shader(screen, context, true);
   constant_modulate =
      make_texture_constant_fragment_shader(screen, context, false);
   constant_alpha_modulate =
      make_texture_constant_fragment_shader(screen, context, true);
   rgbx = make_texture_rgbx_fragment_shader(screen, context);
   packed_gray_vs =
      make_scalar_generic_vertex_shader(screen, context, true);
   packed_gray_fs =
      make_packed_gray_texture_fragment_shader(screen, context);
   projected_vs = make_projected_texture_vertex_shader(screen, context);
   projected_fs =
      make_projected_texture_fragment_shader(screen, context, true);
   projected_modulate_fs =
      make_projected_texture_fragment_shader(screen, context, false);
   projected_constant_fs =
      make_projected_texture_constant_fragment_shader(screen, context,
                                                       false);
   projected_constant_alpha_fs =
      make_projected_texture_constant_fragment_shader(screen, context, true);
   if (!blend_handle ||
       (chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND) &&
        !source_over_handle) ||
       !dsa_handle || !rasterizer_handle ||
       !elements_handle || !replace_elements_handle ||
       !projected_elements_handle || !sampler_handle ||
       !view || !rhw_view || !shadow_source || !shadow_view ||
       chrome9_resource(shadow_source)->guarded ||
       !chrome9_sampler_view(shadow_view)->shadow ||
       !chrome9_resource(
          chrome9_sampler_view(shadow_view)->shadow)->guarded ||
       !replace_vs ||
       !modulate_vs || !replace || !modulate || !constant_modulate ||
       !constant_alpha_modulate || !rgbx ||
       !packed_gray_vs || !packed_gray_fs || !projected_vs || !projected_fs ||
       !projected_modulate_fs || !projected_constant_fs ||
       !projected_constant_alpha_fs ||
       !((struct chrome9_shader *)replace)->native_fs_valid ||
       !((struct chrome9_shader *)modulate)->native_fs_valid ||
       !((struct chrome9_shader *)constant_modulate)->native_fs_valid ||
       !((struct chrome9_shader *)constant_alpha_modulate)->native_fs_valid ||
       !((struct chrome9_shader *)rgbx)->native_fs_valid ||
       !((struct chrome9_shader *)projected_modulate_fs)->native_fs_valid ||
       !((struct chrome9_shader *)projected_constant_fs)->native_fs_valid ||
       ((struct chrome9_shader *)constant_modulate)->fragment_program !=
          CHROME9_FS_TEXTURE_CONSTANT_MODULATE ||
       ((struct chrome9_shader *)constant_alpha_modulate)->fragment_program !=
          CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE ||
       ((struct chrome9_shader *)rgbx)->fragment_program !=
          CHROME9_FS_TEXTURE_RGBX ||
       ((struct chrome9_shader *)packed_gray_fs)->fragment_program !=
          CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE ||
       ((struct chrome9_shader *)projected_fs)->fragment_program !=
          CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA ||
       ((struct chrome9_shader *)projected_modulate_fs)->fragment_program !=
          CHROME9_FS_TEXTURE_PROJECTED_MODULATE ||
       ((struct chrome9_shader *)projected_constant_fs)->fragment_program !=
          CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
       ((struct chrome9_shader *)projected_constant_alpha_fs)->
          fragment_program !=
          CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA) {
      fprintf(stderr,
              "texture setup failed: target=%p texture=%p context=%p "
              "blend=%p dsa=%p rasterizer=%p elements=%p "
              "replace_elements=%p sampler=%p "
              "view=%p rhw_view=%p shadow_source=%p shadow_view=%p "
              "source_guarded=%u shadow=%p shadow_guarded=%u "
              "replace_vs=%p modulate_vs=%p "
              "replace=%p modulate=%p constant_modulate=%p "
              "constant_alpha_modulate=%p rgbx=%p packed=%p/%p\n",
              (void *)target, (void *)texture, (void *)context, blend_handle,
              dsa_handle, rasterizer_handle, elements_handle,
              replace_elements_handle, sampler_handle,
              (void *)view, (void *)rhw_view, (void *)shadow_source,
              (void *)shadow_view,
              shadow_source ? chrome9_resource(shadow_source)->guarded : 0,
              shadow_view ? (void *)chrome9_sampler_view(shadow_view)->shadow :
                            NULL,
              shadow_view && chrome9_sampler_view(shadow_view)->shadow ?
                 chrome9_resource(
                    chrome9_sampler_view(shadow_view)->shadow)->guarded : 0,
              replace_vs, modulate_vs,
              replace, modulate, constant_modulate,
              constant_alpha_modulate, rgbx, packed_gray_vs, packed_gray_fs);
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
                              &mip_view);
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
   {
      const uint32_t expect[4] = {
         0x800000, 0x008000, 0x000080, 0x808080,
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      vertex_buffer.buffer.user = packed_gray_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->bind_vertex_elements_state(context, elements_handle);
      context->bind_vs_state(context, packed_gray_vs);
      context->bind_fs_state(context, packed_gray_fs);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixels = (uint32_t *)((uint8_t *)ctarget->bo.map +
                            ctarget->data_offset);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++) {
         uint32_t pixel =
            pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] & 0xffffff;

         for (unsigned shift = 0; shift <= 16; shift += 8)
            pixels_match &=
               abs((int)(pixel >> shift & 0xff) -
                   (int)(expect[i] >> shift & 0xff)) <= 1;
      }
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctexture)) {
         fprintf(stderr,
                 "packed VAR0 gray+UV mismatch: submitted=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         goto out;
      }
      puts("Gallium packed VAR0 gray+UV MODULATE passed");
   }
   {
      const uint32_t expect[4] = {
         0x800000, 0x008000, 0x000080, 0x808080,
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      vertex_buffer.buffer.user = white_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->bind_vertex_elements_state(context, replace_elements_handle);
      context->bind_vs_state(context, replace_vs);
      context->bind_fs_state(context, constant_modulate);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &texture_constant);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixels = (uint32_t *)((uint8_t *)ctarget->bo.map +
                            ctarget->data_offset);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++) {
         uint32_t pixel =
            pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] & 0xffffff;

         for (unsigned shift = 0; shift <= 16; shift += 8)
            pixels_match &=
               abs((int)(pixel >> shift & 0xff) -
                   (int)(expect[i] >> shift & 0xff)) <= 1;
      }
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctexture)) {
         fprintf(stderr,
                 "texture*c0 MODULATE mismatch: submitted=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         goto out;
      }
      puts("Gallium texture*c0 MODULATE passed");
   }
   {
      const uint32_t expect[4] = {
         0x400000, 0x004000, 0x000040, 0x404040,
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      context->bind_fs_state(context, constant_alpha_modulate);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &texture_alpha_constant);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++) {
         uint32_t pixel =
            pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] & 0xffffff;

         for (unsigned shift = 0; shift <= 16; shift += 8)
            pixels_match &=
               abs((int)(pixel >> shift & 0xff) -
                   (int)(expect[i] >> shift & 0xff)) <= 1;
      }
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctexture)) {
         fprintf(stderr,
                 "texture*c0.aaaa MODULATE mismatch: submitted=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         goto out;
      }
      puts("Gallium texture*c0.aaaa MODULATE passed");
   }
   {
      const uint32_t expect[4] = {
         0xff0000, 0xffffff, 0xff0000, 0xffffff,
      };
      static const uint32_t rgbx_texels[8] = {
         0x00ff0000, 0x00ff0000, 0x00ff0000, 0x00ff0000,
         0x00ffffff, 0x00ffffff, 0x00ffffff, 0x00ffffff,
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      uint32_t *rgbx_source = (uint32_t *)
         ((uint8_t *)crhw_texture->bo.map + crhw_texture->data_offset);
      bool pixels_match = true;

      /* The main texture is deliberately BGRA for the sampled-alpha blend
       * oracle below.  RGBX lowering is legal only for a true XRGB sampler,
       * so exercise it with the separate XRGB source whose stored X bytes
       * are all zero. */
      memset(crhw_texture->bo.map, 0xa5, crhw_texture->bo.size);
      memcpy(rgbx_source, rgbx_texels, sizeof(rgbx_texels));
      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &rhw_view);
      context->bind_fs_state(context, rgbx);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++)
         pixels_match &=
            (pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
             0xffffff) == expect[i];
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(crhw_texture) ||
          memcmp(rgbx_source, rgbx_texels, sizeof(rgbx_texels))) {
         fprintf(stderr,
                 "texture RGBX mismatch: submitted=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         goto out;
      }
      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &view);
      puts("Gallium texture RGBX lowering passed");
   }
   {
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      const uint32_t expect[4] = {
         0xff0000, 0xffffff, 0xff0000, 0xffffff,
      };
      const uint32_t bgra_expect[4] = {
         0x0000ff, 0xffffff, 0x0000ff, 0xffffff,
      };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &rhw_view);
      context->bind_vertex_elements_state(context,
                                          projected_elements_handle);
      context->bind_vs_state(context, projected_vs);
      context->bind_fs_state(context, projected_fs);
      vertex_buffer.buffer.user = projected_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++)
         pixels_match &=
            (pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
             0xffffff) == expect[i];

      context->bind_fs_state(context, projected_modulate_fs);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++)
         pixels_match &=
            (pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
             0xffffff) == expect[i];

      vertex_buffer.buffer.user = bad_q_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      vertex_buffer.buffer.user = projected_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &view);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++)
         pixels_match &=
            (pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
             0xffffff) == bgra_expect[i];
      context->bind_fs_state(context, projected_fs);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      if (!pixels_match || ccontext->submitted_triangles != submitted + 6 ||
          ccontext->rejected_triangles != rejected + 4 ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctexture) ||
          !smoke_guard_pages_match(crhw_texture)) {
         fprintf(stderr,
                 "projected texture*primary mismatch: submitted=+%" PRIu64
                 " rejected=+%" PRIu64 " pixels=%u\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected, pixels_match);
         goto out;
      }
      puts("Gallium projected XRGB texture*primary q=1 lowerings passed");
   }
   {
      const uint32_t expect[4] = {
         0xff0000, 0xffffff, 0xff0000, 0xffffff,
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &rhw_view);
      context->bind_vertex_elements_state(context,
                                          projected_elements_handle);
      context->bind_vs_state(context, projected_vs);
      context->bind_fs_state(context, projected_constant_fs);
      vertex_buffer.buffer.user = projected_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &projected_exact_constant);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++)
         pixels_match &=
            (pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
             0xffffff) == expect[i];

      vertex_buffer.buffer.user = bad_q_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      vertex_buffer.buffer.user = projected_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &projected_inexact_constant);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected + 4 ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(crhw_texture)) {
         fprintf(stderr,
                 "projected texture*c0 mismatch: submitted=+%" PRIu64
                 " rejected=+%" PRIu64 " pixels=%u\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected, pixels_match);
         goto out;
      }
      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &view);
      puts("Gallium projected texture*c0 q=1 UNORM8 lowering passed");
   }
   {
      const uint32_t expect[4] = {
         0xff0000, 0xffffff, 0xff0000, 0xffffff,
      };
      const unsigned at[4][2] = { { 8, 8 }, { 24, 8 },
                                  { 8, 24 }, { 24, 24 } };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &rhw_view);
      context->bind_fs_state(context, projected_constant_alpha_fs);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0,
                                   &projected_exact_constant);
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(at); i++)
         pixels_match &=
            (pixels[at[i][1] * (ctarget->stride / 4) + at[i][0]] &
             0xffffff) == expect[i];
      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &view);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, NULL);
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected + 2 ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctexture) ||
          !smoke_guard_pages_match(crhw_texture)) {
         fprintf(stderr,
                 "projected texture RGB*c0 RGB + c0.a mismatch: "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64
                 " pixels=%u\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected, pixels_match);
         goto out;
      }
      puts("Gallium projected XRGB texture RGB*c0 RGB + c0.a passed");
   }
   bool source_pixels_match =
      source[0] == source_copy[0] && source[1] == source_copy[1] &&
      source[ctexture->stride / 4] == source_copy[2] &&
      source[ctexture->stride / 4 + 1] == source_copy[3];
   bool source_padding_match = true;
   bool mip_tail_match;
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
   mip_tail_match =
      *(const uint32_t *)((const uint8_t *)ctexture->bo.map +
                          ctexture->level_offset[1]) == mip_copy &&
      smoke_bytes_match((const uint8_t *)ctexture->bo.map +
                           ctexture->level_offset[1] + sizeof(mip_copy),
                        ctexture->bo.size - ctexture->data_offset -
                           ctexture->level_offset[1] - sizeof(mip_copy),
                        0xa5);
   bool target_guards_match = smoke_guard_pages_match(ctarget);
   bool texture_guards_match = smoke_guard_pages_match(ctexture);

   if (!source_pixels_match || !source_padding_match || !mip_tail_match ||
       !target_guards_match || !texture_guards_match) {
      fprintf(stderr,
              "texture preservation mismatch: pixels=%u padding=%u mip=%u "
              "target_guards=%u texture_guards=%u\n",
              source_pixels_match, source_padding_match, mip_tail_match,
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
   {
      struct chrome9_resource *source_resource =
         chrome9_resource(shadow_source);
      struct chrome9_resource *shadow_resource = chrome9_resource(
         chrome9_sampler_view(shadow_view)->shadow);
      uint32_t *shadow_source_pixels = (uint32_t *)
         ((uint8_t *)source_resource->bo.map + source_resource->data_offset);
      const uint32_t colors[2] = { 0x00ff0000, 0x0000ff00 };
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool updated = true;

      context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                                 &shadow_view);
      context->bind_vertex_elements_state(context, replace_elements_handle);
      context->bind_vs_state(context, replace_vs);
      context->bind_fs_state(context, replace);
      vertex_buffer.buffer.user = rhw_replace_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      for (unsigned pass = 0; pass < ARRAY_SIZE(colors); pass++) {
         for (unsigned i = 0; i < 8; i++)
            shadow_source_pixels[i] = colors[pass];
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear, 1.0, 0);
         context->draw_vbo(context, &rhw_info, 0, NULL, &rhw_draw, 1);
         context->flush(context, NULL, 0);
         for (unsigned i = 0; i < ARRAY_SIZE(rhw_y); i++)
            updated &=
               (rhw_pixels[rhw_y[i] * (crhw_target->stride / 4) + 48] &
                0xffffff) == colors[pass];
      }
      for (unsigned i = 0; i < 8; i++)
         updated &= shadow_source_pixels[i] == colors[1];
      updated &= ccontext->submitted_triangles == submitted + 2 &&
                 ccontext->rejected_triangles == rejected &&
                 smoke_guard_pages_match(crhw_target) &&
                 smoke_guard_pages_match(shadow_resource);
      if (!updated) {
         fprintf(stderr,
                 "draw-refreshed sampler shadow mismatch: "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64
                 " sample=%08x target_guards=%u shadow_guards=%u\n",
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected,
                 rhw_pixels[rhw_y[0] * (crhw_target->stride / 4) + 48],
                 smoke_guard_pages_match(crhw_target),
                 smoke_guard_pages_match(shadow_resource));
         goto out;
      }
      puts("Gallium draw-refreshed unguarded sampler shadow passed");
   }
   passed = true;

out:
   if (context) {
      if (replace_vs) context->delete_vs_state(context, replace_vs);
      if (modulate_vs) context->delete_vs_state(context, modulate_vs);
      if (replace) context->delete_fs_state(context, replace);
      if (modulate) context->delete_fs_state(context, modulate);
      if (constant_modulate)
         context->delete_fs_state(context, constant_modulate);
      if (constant_alpha_modulate)
         context->delete_fs_state(context, constant_alpha_modulate);
      if (rgbx)
         context->delete_fs_state(context, rgbx);
      if (packed_gray_vs)
         context->delete_vs_state(context, packed_gray_vs);
      if (packed_gray_fs)
         context->delete_fs_state(context, packed_gray_fs);
      if (projected_vs)
         context->delete_vs_state(context, projected_vs);
      if (projected_fs)
         context->delete_fs_state(context, projected_fs);
      if (projected_modulate_fs)
         context->delete_fs_state(context, projected_modulate_fs);
      if (projected_constant_fs)
         context->delete_fs_state(context, projected_constant_fs);
      if (projected_constant_alpha_fs)
         context->delete_fs_state(context, projected_constant_alpha_fs);
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (mip_view) pipe_sampler_view_reference(&mip_view, NULL);
      if (rhw_view) pipe_sampler_view_reference(&rhw_view, NULL);
      if (shadow_view) pipe_sampler_view_reference(&shadow_view, NULL);
      if (sampler_handle)
         context->delete_sampler_state(context, sampler_handle);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (replace_elements_handle)
         context->delete_vertex_elements_state(context,
                                               replace_elements_handle);
      if (projected_elements_handle)
         context->delete_vertex_elements_state(context,
                                               projected_elements_handle);
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
   if (shadow_source) screen->resource_destroy(screen, shadow_source);
   if (rhw_texture) screen->resource_destroy(screen, rhw_texture);
   if (rhw_target) screen->resource_destroy(screen, rhw_target);
   return passed;
}

static bool
run_pipe_textured_depth(struct pipe_screen *screen,
                        enum pipe_format target_format, bool source_over)
{
   static const struct smoke_texture_vertex vertices[3][4] = {
      {
         { { -1, -1,  0.0, 1 }, { 0, 0, 1, 0.5 }, { 0, 0 } },
         { { -1,  1,  0.0, 1 }, { 0, 0, 1, 0.5 }, { 0, 1 } },
         { {  1, -1,  0.0, 1 }, { 0, 0, 1, 0.5 }, { 1, 0 } },
         { {  1,  1,  0.0, 1 }, { 0, 0, 1, 0.5 }, { 1, 1 } },
      }, {
         { { -1, -1,  0.5, 1 }, { 0, 1, 0, 1 }, { 0, 0 } },
         { { -1,  1,  0.5, 1 }, { 0, 1, 0, 1 }, { 0, 1 } },
         { {  1, -1,  0.5, 1 }, { 0, 1, 0, 1 }, { 1, 0 } },
         { {  1,  1,  0.5, 1 }, { 0, 1, 0, 1 }, { 1, 1 } },
      }, {
         { { -1, -1, -0.5, 1 }, { 1, 0, 0, 0.25 }, { 0, 0 } },
         { { -1,  1, -0.5, 1 }, { 1, 0, 0, 0.25 }, { 0, 1 } },
         { {  1, -1, -0.5, 1 }, { 1, 0, 0, 0.25 }, { 1, 0 } },
         { {  1,  1, -0.5, 1 }, { 1, 0, 0, 0.25 }, { 1, 1 } },
      },
   };
   static const struct smoke_texture_vertex blend_vertices[2][4] = {
      {
         { { -1, -1, -0.5, 1 }, { 1, 0, 0, 0.5 }, { 0, 0 } },
         { { -1,  1, -0.5, 1 }, { 1, 0, 0, 0.5 }, { 0, 1 } },
         { {  1, -1, -0.5, 1 }, { 1, 0, 0, 0.5 }, { 1, 0 } },
         { {  1,  1, -0.5, 1 }, { 1, 0, 0, 0.5 }, { 1, 1 } },
      }, {
         { { -1, -1,  0.5, 1 }, { 0, 0, 1, 1 }, { 0, 0 } },
         { { -1,  1,  0.5, 1 }, { 0, 0, 1, 1 }, { 0, 1 } },
         { {  1, -1,  0.5, 1 }, { 0, 0, 1, 1 }, { 1, 0 } },
         { {  1,  1,  0.5, 1 }, { 0, 0, 1, 1 }, { 1, 1 } },
      },
   };
   static const struct smoke_texture_vertex multiply_vertices[4] = {
      { { -1, -1, 0.0, 1 }, { 0.25, 0.5, 0.75, 0.5 }, { 0, 0 } },
      { { -1,  1, 0.0, 1 }, { 0.25, 0.5, 0.75, 0.5 }, { 0, 1 } },
      { {  1, -1, 0.0, 1 }, { 0.25, 0.5, 0.75, 0.5 }, { 1, 0 } },
      { {  1,  1, 0.0, 1 }, { 0.25, 0.5, 0.75, 0.5 }, { 1, 1 } },
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
   struct pipe_resource depth_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *target = NULL, *texture = NULL, *depth = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_templ;
   struct pipe_sampler_view *view = NULL;
   struct pipe_sampler_state sampler = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .max_lod = 1000.0f,
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_blend_state multiply = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LEQUAL,
   };
   struct pipe_depth_stencil_alpha_state read_only_dsa = {
      .depth_enabled = true,
      .depth_writemask = false,
      .depth_func = PIPE_FUNC_LEQUAL,
   };
   struct pipe_depth_stencil_alpha_state equal_read_only_dsa = {
      .depth_enabled = true,
      .depth_writemask = false,
      .depth_func = PIPE_FUNC_EQUAL,
   };
   struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true,
      .bottom_edge_rule = true,
      .depth_clip_near = true,
      .depth_clip_far = true,
   };
   struct pipe_vertex_element elements[3] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices[0],
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 32, .height = 32, .nr_cbufs = 1,
   };
   struct pipe_viewport_state viewport = {
      .scale = { 16, 16, 0.5 }, .translate = { 16, 16, 0.5 },
   };
   struct pipe_scissor_state scissor = { .maxx = 32, .maxy = 32 };
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   struct pipe_draw_start_count_bias draw = { .count = 4 };
   union pipe_color_union clear = { 0 };
   union pipe_color_union multiply_destination = {
      .f = { 0x33 / 255.0f, 0x22 / 255.0f,
             0x11 / 255.0f, 0x44 / 255.0f },
   };
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *multiply_handle = NULL;
   void *read_only_dsa_handle = NULL;
   void *equal_read_only_dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *vs = NULL, *fs = NULL, *replace_fs = NULL, *sampler_handle = NULL;
   struct chrome9_resource *ctarget, *ctexture, *cdepth;
   struct chrome9_context *ccontext;
   uint32_t texture_copy[4] = { 0xffffffff, 0xffffffff,
                                0xffffffff, 0xffffffff };
   uint32_t samples[3] = { 0 };
   uint16_t depth_samples[3] = { 0 };
   uint64_t submitted, rejected;
   bool passed = false;

   target_templ.format = target_format;
   if (source_over) {
      blend.rt[0].blend_enable = true;
      blend.rt[0].rgb_func = PIPE_BLEND_ADD;
      blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA;
      blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
      blend.rt[0].alpha_func = PIPE_BLEND_ADD;
      blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
      blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
      clear.f[1] = 1.0f;
   }
   multiply.rt[0].blend_enable = true;
   multiply.rt[0].rgb_func = PIPE_BLEND_ADD;
   multiply.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_DST_COLOR;
   multiply.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_SRC_COLOR;
   multiply.rt[0].alpha_func = PIPE_BLEND_ADD;
   multiply.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_DST_COLOR;
   multiply.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_SRC_COLOR;
   multiply.rt[0].colormask = PIPE_MASK_RGBA;

   target = screen->resource_create(screen, &target_templ);
   texture = screen->resource_create(screen, &texture_templ);
   depth = screen->resource_create(screen, &depth_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!target || !texture || !depth || !context)
      goto out;
   ctarget = chrome9_resource(target);
   ctexture = chrome9_resource(texture);
   cdepth = chrome9_resource(depth);
   ccontext = chrome9_context(context);
   memset(ctarget->bo.map, 0x5a, ctarget->bo.size);
   memset(ctexture->bo.map, 0xa5, ctexture->bo.size);
   memset(cdepth->bo.map, 0x5a, cdepth->bo.size);
   uint32_t *texels = (uint32_t *)
      ((uint8_t *)ctexture->bo.map + ctexture->data_offset);
   texels[0] = texels[1] = texture_copy[0];
   texels[ctexture->stride / 4] = texture_copy[2];
   texels[ctexture->stride / 4 + 1] = texture_copy[3];

   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   if (!source_over && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DEPTH_READ_ONLY))
      read_only_dsa_handle =
         context->create_depth_stencil_alpha_state(context, &read_only_dsa);
   if (!source_over && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DEPTH_EQUAL))
      equal_read_only_dsa_handle = context->create_depth_stencil_alpha_state(
         context, &equal_read_only_dsa);
   if (!source_over &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND) &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS))
      multiply_handle = context->create_blend_state(context, &multiply);
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
   vs = make_texture_vertex_shader(screen, context, true);
   fs = make_texture_fragment_shader(screen, context, true);
   if (!source_over)
      replace_fs = make_texture_fragment_shader_slot(
         screen, context, false, VARYING_SLOT_TEX0);
   if (!blend_handle || !dsa_handle ||
       (!source_over && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DEPTH_READ_ONLY) &&
        !read_only_dsa_handle) ||
       (!source_over && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DEPTH_EQUAL) &&
        !equal_read_only_dsa_handle) ||
       (!source_over &&
        chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND) &&
        chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS) &&
        !multiply_handle) ||
       !rasterizer_handle ||
       !elements_handle || !sampler_handle || !view || !vs || !fs ||
       (!source_over && !replace_fs))
      goto out;

   framebuffer.cbufs[0].format = target->format;
   framebuffer.cbufs[0].texture = target;
   framebuffer.zsbuf.format = depth->format;
   framebuffer.zsbuf.texture = depth;
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                &sampler_handle);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
   context->set_framebuffer_state(context, &framebuffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                  PIPE_MASK_RGBA, 0xff, NULL, &clear, 0.5, 0);
   submitted = ccontext->submitted_triangles;
   rejected = ccontext->rejected_triangles;
   unsigned pass_count = source_over ? ARRAY_SIZE(blend_vertices) :
                                       ARRAY_SIZE(vertices);
   for (unsigned pass = 0; pass < pass_count; pass++) {
      uint32_t *pixels = (uint32_t *)
         ((uint8_t *)ctarget->bo.map + ctarget->data_offset);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)cdepth->bo.map + cdepth->data_offset);

      vertex_buffer.buffer.user = source_over ? blend_vertices[pass] :
                                                vertices[pass];
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      samples[pass] = pixels[16 * (ctarget->stride / 4) + 16];
      depth_samples[pass] = depth_pixels[16 * (cdepth->stride / 2) + 16];
   }
   if (source_over) {
      unsigned alpha = samples[0] >> 24;
      unsigned red = samples[0] >> 16 & 0xff;
      unsigned green = samples[0] >> 8 & 0xff;

      passed = alpha >= 0x7e && alpha <= 0x80 &&
               red >= 0x7e && red <= 0x80 &&
               green >= 0x7f && green <= 0x81 &&
               !(samples[0] & 0xff) && samples[1] == samples[0] &&
               depth_samples[0] >= 0x3ffe && depth_samples[0] <= 0x4001 &&
               depth_samples[1] >= 0x3ffe && depth_samples[1] <= 0x4001 &&
               ccontext->submitted_triangles == submitted + 4;
   } else {
      passed = (target_format == PIPE_FORMAT_B8G8R8A8_UNORM ?
             samples[0] == 0x7f0000ff &&
             samples[1] == samples[0] &&
             samples[2] == 0x3fff0000 :
             (samples[0] & 0xffffff) == 0x000000ff &&
             (samples[1] & 0xffffff) ==
                (samples[0] & 0xffffff) &&
             (samples[2] & 0xffffff) == 0x00ff0000) &&
            depth_samples[0] >= 0x7ffe && depth_samples[0] <= 0x8001 &&
            depth_samples[1] >= 0x7ffe && depth_samples[1] <= 0x8001 &&
            depth_samples[2] >= 0x3ffe && depth_samples[2] <= 0x4001 &&
            ccontext->submitted_triangles == submitted + 6;
   }
   passed = passed && ccontext->rejected_triangles == rejected &&
            texels[0] == texture_copy[0] && texels[1] == texture_copy[1] &&
            texels[ctexture->stride / 4] == texture_copy[2] &&
            texels[ctexture->stride / 4 + 1] == texture_copy[3] &&
            smoke_guard_pages_match(ctarget) &&
            smoke_guard_pages_match(ctexture) &&
            smoke_guard_pages_match(cdepth);
   if (passed && replace_fs) {
      uint64_t replace_submitted = ccontext->submitted_triangles;
      uint64_t replace_rejected = ccontext->rejected_triangles;
      uint32_t *pixels = (uint32_t *)
         ((uint8_t *)ctarget->bo.map + ctarget->data_offset);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)cdepth->bo.map + cdepth->data_offset);

      context->bind_fs_state(context, replace_fs);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear, 0.5, 0);
      vertex_buffer.buffer.user = vertices[0];
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      passed = (pixels[16 * (ctarget->stride / 4) + 16] & 0xffffff) ==
                  0xffffff &&
               depth_pixels[16 * (cdepth->stride / 2) + 16] >= 0x7ffe &&
               depth_pixels[16 * (cdepth->stride / 2) + 16] <= 0x8001 &&
               ccontext->submitted_triangles == replace_submitted + 2 &&
               ccontext->rejected_triangles == replace_rejected &&
               smoke_guard_pages_match(ctarget) &&
               smoke_guard_pages_match(ctexture) &&
               smoke_guard_pages_match(cdepth);
      if (passed)
         printf("Gallium textured D16 REPLACE format %u passed\n",
                target_format);
      else
         fprintf(stderr,
                 "Gallium textured D16 REPLACE format %u mismatch: "
                 "sample=%08x/%04x submitted=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 target_format,
                 pixels[16 * (ctarget->stride / 4) + 16],
                 depth_pixels[16 * (cdepth->stride / 2) + 16],
                 ccontext->submitted_triangles - replace_submitted,
                 ccontext->rejected_triangles - replace_rejected);
      context->bind_fs_state(context, fs);
   }
   if (passed && read_only_dsa_handle) {
      uint32_t equal_pixel, near_pixel, far_pixel;
      uint16_t equal_depth, near_depth, far_depth;
      uint64_t read_only_submitted = ccontext->submitted_triangles;
      uint64_t read_only_rejected = ccontext->rejected_triangles;
      uint32_t *pixels = (uint32_t *)
         ((uint8_t *)ctarget->bo.map + ctarget->data_offset);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)cdepth->bo.map + cdepth->data_offset);

      context->bind_depth_stencil_alpha_state(context, read_only_dsa_handle);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear, 0.5, 0);
      vertex_buffer.buffer.user = vertices[0];
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      equal_pixel = pixels[16 * (ctarget->stride / 4) + 16] & 0xffffff;
      equal_depth = depth_pixels[16 * (cdepth->stride / 2) + 16];

      vertex_buffer.buffer.user = vertices[2];
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      near_pixel = pixels[16 * (ctarget->stride / 4) + 16] & 0xffffff;
      near_depth = depth_pixels[16 * (cdepth->stride / 2) + 16];

      vertex_buffer.buffer.user = vertices[1];
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      far_pixel = pixels[16 * (ctarget->stride / 4) + 16] & 0xffffff;
      far_depth = depth_pixels[16 * (cdepth->stride / 2) + 16];
      passed = equal_pixel == 0x000000ff && near_pixel == 0x00ff0000 &&
               far_pixel == near_pixel &&
               equal_depth >= 0x7ffe && equal_depth <= 0x8001 &&
               near_depth >= 0x7ffe && near_depth <= 0x8001 &&
               far_depth >= 0x7ffe && far_depth <= 0x8001 &&
               ccontext->submitted_triangles == read_only_submitted + 6 &&
               ccontext->rejected_triangles == read_only_rejected &&
               smoke_guard_pages_match(ctarget) &&
               smoke_guard_pages_match(ctexture) &&
               smoke_guard_pages_match(cdepth);
      if (passed)
         printf("Gallium textured D16 read-only LEQUAL passed: "
                "equal=%08x/%04x near=%08x/%04x far=%08x/%04x\n",
                equal_pixel, equal_depth, near_pixel, near_depth,
                far_pixel, far_depth);
      else
         fprintf(stderr,
                 "Gallium textured D16 read-only LEQUAL mismatch: "
                 "equal=%08x/%04x near=%08x/%04x far=%08x/%04x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 equal_pixel, equal_depth, near_pixel, near_depth,
                 far_pixel, far_depth,
                 ccontext->submitted_triangles - read_only_submitted,
                 ccontext->rejected_triangles - read_only_rejected);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
   }
   if (passed && multiply_handle && equal_read_only_dsa_handle) {
      uint64_t equal_submitted = ccontext->submitted_triangles;
      uint64_t equal_rejected = ccontext->rejected_triangles;
      uint32_t *pixels = (uint32_t *)
         ((uint8_t *)ctarget->bo.map + ctarget->data_offset);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)cdepth->bo.map + cdepth->data_offset);

      context->bind_blend_state(context, multiply_handle);
      context->bind_depth_stencil_alpha_state(context,
                                              equal_read_only_dsa_handle);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &multiply_destination,
                     0.5, 0);
      vertex_buffer.buffer.user = multiply_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      uint32_t pixel = pixels[16 * (ctarget->stride / 4) + 16];
      uint16_t depth_pixel = depth_pixels[16 * (cdepth->stride / 2) + 16];

      passed = abs((int)(pixel >> 16 & 0xff) - 0x1a) <= 1 &&
               abs((int)(pixel >> 8 & 0xff) - 0x22) <= 1 &&
               abs((int)(pixel & 0xff) - 0x1a) <= 1 &&
               depth_pixel >= 0x7ffe && depth_pixel <= 0x8001 &&
               ccontext->submitted_triangles == equal_submitted + 2 &&
               ccontext->rejected_triangles == equal_rejected &&
               smoke_guard_pages_match(ctarget) &&
               smoke_guard_pages_match(ctexture) &&
               smoke_guard_pages_match(cdepth);
      if (passed)
         printf("Gallium textured D16 EQUAL read-only blend passed: "
                "sample=%08x/%04x\n", pixel, depth_pixel);
      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
   }
   if (passed && multiply_handle && read_only_dsa_handle) {
      uint64_t blend_submitted = ccontext->submitted_triangles;
      uint64_t blend_rejected = ccontext->rejected_triangles;
      uint32_t *pixels = (uint32_t *)
         ((uint8_t *)ctarget->bo.map + ctarget->data_offset);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)cdepth->bo.map + cdepth->data_offset);
      uint32_t pixel;
      uint16_t depth_pixel;
      unsigned alpha, red, green, blue;

      context->bind_blend_state(context, multiply_handle);
      context->bind_depth_stencil_alpha_state(context, read_only_dsa_handle);
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &multiply_destination,
                     0.5, 0);
      vertex_buffer.buffer.user = multiply_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      pixel = pixels[16 * (ctarget->stride / 4) + 16];
      depth_pixel = depth_pixels[16 * (cdepth->stride / 2) + 16];
      alpha = pixel >> 24;
      red = pixel >> 16 & 0xff;
      green = pixel >> 8 & 0xff;
      blue = pixel & 0xff;
      passed = abs((int)red - 0x1a) <= 1 &&
               abs((int)green - 0x22) <= 1 &&
               abs((int)blue - 0x1a) <= 1 &&
               (target_format != PIPE_FORMAT_B8G8R8A8_UNORM ||
                abs((int)alpha - 0x44) <= 1) &&
               depth_pixel >= 0x7ffe && depth_pixel <= 0x8001 &&
               ccontext->submitted_triangles == blend_submitted + 2 &&
               ccontext->rejected_triangles == blend_rejected &&
               smoke_guard_pages_match(ctarget) &&
               smoke_guard_pages_match(ctexture) &&
               smoke_guard_pages_match(cdepth);
      if (passed)
         printf("Gallium textured D16 read-only DST_COLOR/SRC_COLOR "
                "passed: sample=%08x/%04x\n", pixel, depth_pixel);
      else
         fprintf(stderr,
                 "Gallium textured D16 read-only DST_COLOR/SRC_COLOR "
                 "mismatch: sample=%08x/%04x submitted=+%" PRIu64
                 " rejected=+%" PRIu64 "\n",
                 pixel, depth_pixel,
                 ccontext->submitted_triangles - blend_submitted,
                 ccontext->rejected_triangles - blend_rejected);
      context->bind_blend_state(context, blend_handle);
      context->bind_depth_stencil_alpha_state(context, dsa_handle);
   }
   if (passed && source_over)
      printf("Gallium textured D16 source-over passed: "
             "near=%08x/%04x far=%08x/%04x\n",
             samples[0], depth_samples[0], samples[1], depth_samples[1]);
   else if (passed)
      printf("Gallium textured D16 LEQUAL format %u passed: "
             "equal=%08x/%04x far=%08x/%04x near=%08x/%04x\n",
             target_format, samples[0], depth_samples[0], samples[1],
             depth_samples[1], samples[2], depth_samples[2]);
   else if (source_over)
      fprintf(stderr,
              "Gallium textured D16 source-over mismatch: "
              "near=%08x/%04x far=%08x/%04x "
              "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
              samples[0], depth_samples[0], samples[1], depth_samples[1],
              ccontext->submitted_triangles - submitted,
              ccontext->rejected_triangles - rejected);
   else
      fprintf(stderr,
              "Gallium textured D16 LEQUAL format %u mismatch: "
              "equal=%08x/%04x far=%08x/%04x near=%08x/%04x "
              "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
              target_format, samples[0], depth_samples[0], samples[1],
              depth_samples[1], samples[2], depth_samples[2],
              ccontext->submitted_triangles - submitted,
              ccontext->rejected_triangles - rejected);

out:
   if (context) {
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (vs) context->delete_vs_state(context, vs);
      if (fs) context->delete_fs_state(context, fs);
      if (replace_fs) context->delete_fs_state(context, replace_fs);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (sampler_handle)
         context->delete_sampler_state(context, sampler_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (read_only_dsa_handle)
         context->delete_depth_stencil_alpha_state(context,
                                                   read_only_dsa_handle);
      if (equal_read_only_dsa_handle)
         context->delete_depth_stencil_alpha_state(
            context, equal_read_only_dsa_handle);
      if (blend_handle) context->delete_blend_state(context, blend_handle);
      if (multiply_handle)
         context->delete_blend_state(context, multiply_handle);
      context->destroy(context);
   }
   if (depth) screen->resource_destroy(screen, depth);
   if (texture) screen->resource_destroy(screen, texture);
   if (target) screen->resource_destroy(screen, target);
   return passed;
}

static bool
run_pipe_npot_texture(struct pipe_screen *screen)
{
   static const struct {
      const char *name;
      enum pipe_tex_wrap wrap;
      enum pipe_tex_filter filter;
      float s, t;
      uint32_t expected;
      unsigned tolerance;
   } cases[] = {
      { "clamp-nearest", PIPE_TEX_WRAP_CLAMP_TO_EDGE,
        PIPE_TEX_FILTER_NEAREST, -0.25f, 1.25f, 0x00ff00, 0 },
      { "repeat-nearest", PIPE_TEX_WRAP_REPEAT,
        PIPE_TEX_FILTER_NEAREST, 1.25f, 1.25f, 0xff0000, 0 },
      { "mirror-nearest", PIPE_TEX_WRAP_MIRROR_REPEAT,
        PIPE_TEX_FILTER_NEAREST, 1.25f, 1.25f, 0xffffff, 0 },
      { "clamp-linear", PIPE_TEX_WRAP_CLAMP_TO_EDGE,
        PIPE_TEX_FILTER_LINEAR, -0.25f, 1.25f, 0x00ff00, 0 },
      { "repeat-linear", PIPE_TEX_WRAP_REPEAT,
        PIPE_TEX_FILTER_LINEAR, 1.5f, 1.5f, 0x808080, 1 },
      { "mirror-linear", PIPE_TEX_WRAP_MIRROR_REPEAT,
        PIPE_TEX_FILTER_LINEAR, 1.5f, 1.5f, 0x808080, 1 },
   };
   const struct pipe_resource target_template = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   const struct pipe_resource texture_template = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 30, .height0 = 18, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *target = NULL, *texture = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_template;
   struct pipe_sampler_view *view = NULL;
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[2] = { 0 };
   struct smoke_texture_vertex vertices[4] = {
      { { -1, -1, 0, 1 } }, { { -1, 1, 0, 1 } },
      { { 1, -1, 0, 1 } }, { { 1, 1, 0, 1 } },
   };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 64, .height = 64, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8X8_UNORM },
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 32, 32, 0.5 }, .translate = { 32, 32, 0.5 },
   };
   const struct pipe_scissor_state scissor = { .maxx = 64, .maxy = 64 };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   const struct pipe_draw_start_count_bias draw = { .count = 4 };
   const union pipe_color_union clear = { 0 };
   struct chrome9_resource *ctarget, *ctexture;
   struct chrome9_context *ccontext;
   void *blend_handle = NULL, *dsa_handle = NULL, *rasterizer_handle = NULL;
   void *elements_handle = NULL, *vs = NULL, *fs = NULL;
   uint32_t *source, *pixels;
   bool passed = false;

   target = screen->resource_create(screen, &target_template);
   texture = screen->resource_create(screen, &texture_template);
   context = screen->context_create(screen, NULL, 0);
   if (!target || !texture || !context)
      goto out;
   ctarget = chrome9_resource(target);
   ctexture = chrome9_resource(texture);
   ccontext = chrome9_context(context);
   if (ctexture->stride != 128 || ctexture->data_size != 128 * 18)
      goto out;

   memset(ctarget->bo.map, 0x5a, ctarget->bo.size);
   memset(ctexture->bo.map, 0xa5, ctexture->bo.size);
   source = (uint32_t *)((uint8_t *)ctexture->bo.map + ctexture->data_offset);
   for (unsigned y = 0; y < texture->height0; y++) {
      for (unsigned x = 0; x < texture->width0; x++) {
         source[y * (ctexture->stride / 4) + x] =
            y < 9 ? (x < 15 ? 0x00ff0000 : 0x000000ff) :
                    (x < 15 ? 0x0000ff00 : 0x00ffffff);
      }
   }

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
   elements[1].src_offset = offsetof(struct smoke_texture_vertex, texcoord);
   elements[1].src_format = PIPE_FORMAT_R32G32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_texture_vertex);
   elements_handle =
      context->create_vertex_elements_state(context, 2, elements);
   u_sampler_view_default_template(&view_template, texture, texture->format);
   view = context->create_sampler_view(context, texture, &view_template);
   vs = make_texture_vertex_shader(screen, context, false);
   fs = make_texture_fragment_shader(screen, context, false);
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !view || !vs || !fs)
      goto out;

   framebuffer.cbufs[0].texture = target;
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_framebuffer_state(context, &framebuffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   pixels = (uint32_t *)((uint8_t *)ctarget->bo.map + ctarget->data_offset);

   for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
      struct pipe_sampler_state sampler = {
         .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
         .compare_mode = PIPE_TEX_COMPARE_NONE,
         .max_anisotropy = 1,
         .max_lod = 1000.0f,
         .reduction_mode = PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE,
      };
      void *sampler_handle;
      void *samplers[1];
      uint64_t submitted = ccontext->submitted_triangles;
      uint64_t rejected = ccontext->rejected_triangles;
      bool pixels_match = true;

      sampler.wrap_s = sampler.wrap_t = cases[c].wrap;
      sampler.min_img_filter = sampler.mag_img_filter = cases[c].filter;
      sampler_handle = context->create_sampler_state(context, &sampler);
      if (!sampler_handle)
         goto out;
      samplers[0] = sampler_handle;
      context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                   samplers);
      for (unsigned v = 0; v < ARRAY_SIZE(vertices); v++) {
         vertices[v].texcoord[0] = cases[c].s;
         vertices[v].texcoord[1] = cases[c].t;
      }
      context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                     &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      const unsigned sample_xy[3][2] = { { 8, 8 }, { 32, 32 }, { 55, 55 } };
      for (unsigned i = 0; i < ARRAY_SIZE(sample_xy); i++) {
         uint32_t pixel =
            pixels[sample_xy[i][1] * (ctarget->stride / 4) +
                   sample_xy[i][0]] & 0xffffff;
         for (unsigned shift = 0; shift <= 16; shift += 8) {
            int actual = pixel >> shift & 0xff;
            int expected = cases[c].expected >> shift & 0xff;

            pixels_match &= abs(actual - expected) <= cases[c].tolerance;
         }
      }
      void *none[1] = { NULL };
      context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1, none);
      context->delete_sampler_state(context, sampler_handle);
      if (!pixels_match || ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected) {
         fprintf(stderr,
                 "NPOT %s mismatch: sample=%06x expected=%06x tol=%u "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 cases[c].name,
                 pixels[32 * (ctarget->stride / 4) + 32] & 0xffffff,
                 cases[c].expected, cases[c].tolerance,
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         goto out;
      }
   }

   for (unsigned y = 0; y < texture->height0; y++) {
      for (unsigned x = 0; x < texture->width0; x++) {
         const uint32_t expected =
            y < 9 ? (x < 15 ? 0x00ff0000 : 0x000000ff) :
                    (x < 15 ? 0x0000ff00 : 0x00ffffff);

         if (source[y * (ctexture->stride / 4) + x] != expected)
            goto out;
      }
      if (!smoke_bytes_match((const uint8_t *)source +
                                (size_t)y * ctexture->stride + 30 * 4,
                             ctexture->stride - 30 * 4, 0xa5))
         goto out;
   }
   if (!smoke_guard_pages_match(ctarget) ||
       !smoke_guard_pages_match(ctexture))
      goto out;
   puts("Gallium NPOT 30x18 clamp/repeat/mirror nearest/linear passed");
   passed = true;

out:
   if (context) {
      if (vs) context->delete_vs_state(context, vs);
      if (fs) context->delete_fs_state(context, fs);
      if (view) pipe_sampler_view_reference(&view, NULL);
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

static bool
run_pipe_resource_copy(struct pipe_screen *screen)
{
   const struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 8, .height0 = 8, .depth0 = 1, .array_size = 1,
      .last_level = 2,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *src = NULL, *dst = NULL;
   struct pipe_context *context = NULL;
   struct chrome9_resource *csrc, *cdst;
   bool passed = false;

   src = screen->resource_create(screen, &templ);
   dst = screen->resource_create(screen, &templ);
   context = screen->context_create(screen, NULL, 0);
   if (!src || !dst || !context || !context->resource_copy_region)
      goto out;
   csrc = chrome9_resource(src);
   cdst = chrome9_resource(dst);
   memset(csrc->bo.map, 0xa5, csrc->bo.size);
   memset(cdst->bo.map, 0x5a, cdst->bo.size);
   for (unsigned level = 0; level <= templ.last_level; level++) {
      const unsigned width = u_minify(templ.width0, level);
      const unsigned height = u_minify(templ.height0, level);
      uint32_t *pixels = (uint32_t *)((uint8_t *)csrc->bo.map +
                                      csrc->level_offset[level]);

      for (unsigned y = 0; y < height; y++)
         for (unsigned x = 0; x < width; x++)
            pixels[y * (csrc->level_stride[level] / 4) + x] =
               0x5b000000 | level << 16 | y << 8 | x;
      const struct pipe_box box = {
         .x = 0, .y = 0, .z = 0,
         .width = width, .height = height, .depth = 1,
      };

      context->resource_copy_region(context, dst, level, 0, 0, 0,
                                    src, level, &box);
   }

   passed = true;
   for (unsigned level = 0; level <= templ.last_level; level++) {
      const unsigned width = u_minify(templ.width0, level);
      const unsigned height = u_minify(templ.height0, level);
      const uint8_t *src_data = (const uint8_t *)csrc->bo.map +
                                csrc->level_offset[level];
      const uint8_t *dst_data = (const uint8_t *)cdst->bo.map +
                                cdst->level_offset[level];

      for (unsigned y = 0; y < height; y++) {
         const uint32_t *src_pixels =
            (const uint32_t *)(src_data + y * csrc->level_stride[level]);
         const uint32_t *dst_pixels =
            (const uint32_t *)(dst_data + y * cdst->level_stride[level]);

         for (unsigned x = 0; x < width; x++)
            passed &= src_pixels[x] ==
                         (0x5b000000 | level << 16 | y << 8 | x) &&
                      dst_pixels[x] == src_pixels[x];
         passed &= smoke_bytes_match(
            dst_data + y * cdst->level_stride[level] + width * 4,
            cdst->level_stride[level] - width * 4, 0x5a);
      }
   }
   size_t page_size = cdst->data_offset;
   size_t level0_end = page_size + ALIGN_POT(cdst->data_size, page_size);

   passed &= smoke_bytes_match(cdst->bo.map, page_size, 0x5a) &&
             smoke_bytes_match((uint8_t *)cdst->bo.map + level0_end,
                               page_size, 0x5a) &&
             smoke_bytes_match((uint8_t *)cdst->bo.map + cdst->bo.size -
                                  page_size,
                               page_size, 0x5a);
   if (passed)
      puts("Gallium CPU resource_copy_region levels 0..2 passed");
   else
      fputs("Gallium CPU resource_copy_region mismatch\n", stderr);

out:
   if (context) context->destroy(context);
   if (dst) screen->resource_destroy(screen, dst);
   if (src) screen->resource_destroy(screen, src);
   return passed;
}

static bool
run_pipe_two_npot_textures(struct pipe_screen *screen, bool depth_test,
                           bool projected_blend, bool texture0_alpha)
{
   struct pipe_resource target_template = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource texture_templates[2] = {
      { .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8X8_UNORM,
        .width0 = 30, .height0 = 18, .depth0 = 1, .array_size = 1,
        .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR },
      { .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8X8_UNORM,
        .width0 = 22, .height0 = 14, .depth0 = 1, .array_size = 1,
        .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR },
   };
   const struct pipe_resource depth_template = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   static const unsigned sample_xy[4][2] = {
      { 16, 16 }, { 48, 16 }, { 16, 48 }, { 48, 48 },
   };
   static const uint32_t expected[4] = {
      0x800000, 0x000080, 0x008000, 0x808080,
   };
   struct smoke_two_texture_vertex vertices[4] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 1 },
        { 0, 0, 0, 1 }, { 0, 0, 0, 1 } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 1 },
        { 0, 1, 0, 1 }, { 0, 1, 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 1 },
        { 1, 0, 0, 1 }, { 1, 0, 0, 1 } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 1 },
        { 1, 1, 0, 1 }, { 1, 1, 0, 1 } },
   };
   struct pipe_resource *target = NULL, *textures[2] = { NULL, NULL };
   struct pipe_resource *depth = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view *views[2] = { NULL, NULL };
   struct pipe_sampler_view view_template;
   struct pipe_sampler_state sampler = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .compare_mode = PIPE_TEX_COMPARE_NONE,
      .max_anisotropy = 1, .max_lod = 1000.0f,
      .reduction_mode = PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE,
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_depth_stencil_alpha_state less_dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LESS,
   };
   struct pipe_depth_stencil_alpha_state read_only_dsa = {
      .depth_enabled = true,
      .depth_writemask = false,
      .depth_func = PIPE_FUNC_LEQUAL,
   };
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[4] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true, .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 64, .height = 64, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8X8_UNORM },
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 32, 32, 0.5 }, .translate = { 32, 32, 0.5 },
   };
   const struct pipe_scissor_state scissor = { .maxx = 64, .maxy = 64 };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   const struct pipe_draw_start_count_bias draw = { .count = 4 };
   const union pipe_color_union clear = {
      .f = { 1.0f, 1.0f, 1.0f, 1.0f },
   };
   struct chrome9_resource *ctarget, *ctextures[2], *cdepth = NULL;
   struct chrome9_context *ccontext;
   void *blend_handle = NULL, *dsa_handle = NULL, *less_dsa_handle = NULL;
   void *read_only_dsa_handle = NULL;
   void *rasterizer_handle = NULL;
   void *elements_handle = NULL, *sampler_handles[2] = { NULL, NULL };
   void *repeat_sampler_handle = NULL;
   void *vs = NULL, *fs = NULL;
   bool passed = false;

   if (texture0_alpha) {
      target_template.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      texture_templates[0].format = PIPE_FORMAT_B8G8R8A8_UNORM;
      framebuffer.cbufs[0].format = target_template.format;
   }

   target = screen->resource_create(screen, &target_template);
   for (unsigned i = 0; i < 2; i++)
      textures[i] = screen->resource_create(screen, &texture_templates[i]);
   if (depth_test)
      depth = screen->resource_create(screen, &depth_template);
   context = screen->context_create(screen, NULL, 0);
   if (!target || !textures[0] || !textures[1] || !context ||
       (depth_test && !depth))
      goto out;
   ctarget = chrome9_resource(target);
   ccontext = chrome9_context(context);
   if (depth_test)
      cdepth = chrome9_resource(depth);
   memset(ctarget->bo.map, 0x5a, ctarget->bo.size);
   for (unsigned unit = 0; unit < 2; unit++) {
      ctextures[unit] = chrome9_resource(textures[unit]);
      memset(ctextures[unit]->bo.map, 0xa5, ctextures[unit]->bo.size);
   }
   uint32_t *source0 = (uint32_t *)((uint8_t *)ctextures[0]->bo.map +
                                    ctextures[0]->data_offset);
   uint32_t *source1 = (uint32_t *)((uint8_t *)ctextures[1]->bo.map +
                                    ctextures[1]->data_offset);
   for (unsigned y = 0; y < textures[0]->height0; y++)
      for (unsigned x = 0; x < textures[0]->width0; x++)
         source0[y * (ctextures[0]->stride / 4) + x] =
            (texture0_alpha ? 0x80000000 : 0) |
            (y < 9 ? (x < 15 ? 0x00ff0000 : 0x000000ff) :
                     (x < 15 ? 0x0000ff00 : 0x00ffffff));
   for (unsigned y = 0; y < textures[1]->height0; y++)
      for (unsigned x = 0; x < textures[1]->width0; x++)
         source1[y * (ctextures[1]->stride / 4) + x] = 0x00808080;

   blend.rt[0].colormask = PIPE_MASK_RGBA;
   if (projected_blend) {
      blend.rt[0].blend_enable = true;
      blend.rt[0].rgb_func = PIPE_BLEND_ADD;
      blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_DST_COLOR;
      blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ZERO;
      blend.rt[0].alpha_func = PIPE_BLEND_ADD;
      blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_DST_COLOR;
      blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
   }
   blend_handle = context->create_blend_state(context, &blend);
   if (depth_test) {
      dsa.depth_enabled = true;
      dsa.depth_writemask = true;
      dsa.depth_func = PIPE_FUNC_LEQUAL;
      less_dsa_handle =
         context->create_depth_stencil_alpha_state(context, &less_dsa);
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_DEPTH_READ_ONLY))
         read_only_dsa_handle =
            context->create_depth_stencil_alpha_state(context,
                                                       &read_only_dsa);
   }
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer.half_pixel_center = true;
   rasterizer.bottom_edge_rule = true;
   rasterizer.depth_clip_near = true;
   rasterizer.depth_clip_far = true;
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_two_texture_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   unsigned element_count = projected_blend ? 4 : 3;
   unsigned texcoord_element = projected_blend ? 2 : 1;
   if (projected_blend) {
      elements[1].src_offset = offsetof(struct smoke_two_texture_vertex, color);
      elements[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   }
   elements[texcoord_element].src_offset =
      offsetof(struct smoke_two_texture_vertex, texcoord0);
   elements[texcoord_element].src_format = projected_blend ?
      PIPE_FORMAT_R32G32B32A32_FLOAT : PIPE_FORMAT_R32G32_FLOAT;
   elements[texcoord_element + 1].src_offset =
      offsetof(struct smoke_two_texture_vertex, texcoord1);
   elements[texcoord_element + 1].src_format = projected_blend ?
      PIPE_FORMAT_R32G32B32A32_FLOAT : PIPE_FORMAT_R32G32_FLOAT;
   for (unsigned i = 0; i < element_count; i++)
      elements[i].src_stride = sizeof(struct smoke_two_texture_vertex);
   elements_handle =
      context->create_vertex_elements_state(context, element_count, elements);
   for (unsigned unit = 0; unit < 2; unit++) {
      sampler_handles[unit] = context->create_sampler_state(context, &sampler);
      u_sampler_view_default_template(&view_template, textures[unit],
                                      textures[unit]->format);
      views[unit] =
         context->create_sampler_view(context, textures[unit], &view_template);
   }
   vs = projected_blend ?
      make_two_projected_texture_vertex_shader(screen, context) :
      make_two_texture_vertex_shader(screen, context);
   fs = projected_blend ?
      make_two_projected_texture_fragment_shader(screen, context,
                                                 texture0_alpha) :
      make_two_texture_fragment_shader(screen, context);
   if (!blend_handle || !dsa_handle || !rasterizer_handle || !elements_handle ||
       !sampler_handles[0] || !sampler_handles[1] || !views[0] || !views[1] ||
       (depth_test && !less_dsa_handle) ||
       (depth_test && chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DEPTH_READ_ONLY) &&
        !read_only_dsa_handle) ||
       !vs || !fs ||
       (!projected_blend &&
        !((struct chrome9_shader *)fs)->native_fs_valid) ||
       ((struct chrome9_shader *)fs)->fragment_program !=
          (projected_blend ?
             (texture0_alpha ?
                CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY :
                CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA) :
             CHROME9_FS_TWO_TEXTURE_MODULATE))
      goto out;

   framebuffer.cbufs[0].texture = target;
   if (depth_test) {
      framebuffer.zsbuf.format = depth->format;
      framebuffer.zsbuf.texture = depth;
   }
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 2,
                                sampler_handles);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 2, 0, views);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_framebuffer_state(context, &framebuffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   context->clear(context,
                  PIPE_CLEAR_COLOR0 |
                  (depth_test ? PIPE_CLEAR_DEPTH : 0),
                  PIPE_MASK_RGBA, 0xff, NULL, &clear,
                  depth_test ? 0.5 : 1.0, 0);
   uint64_t submitted = ccontext->submitted_triangles;
   uint64_t rejected = ccontext->rejected_triangles;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush(context, NULL, 0);
   uint32_t *pixels = (uint32_t *)((uint8_t *)ctarget->bo.map +
                                   ctarget->data_offset);
   for (unsigned i = 0; i < ARRAY_SIZE(sample_xy); i++) {
      uint32_t pixel =
         pixels[sample_xy[i][1] * (ctarget->stride / 4) + sample_xy[i][0]] &
         0xffffff;

      for (unsigned shift = 0; shift <= 16; shift += 8)
         if (abs((int)(pixel >> shift & 0xff) -
                 (int)(expected[i] >> shift & 0xff)) > 1)
            goto out;
   }
   if (projected_blend) {
      const unsigned center = 32 * (ctarget->stride / 4) + 32;

      if (!depth_test ||
          ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctextures[0]) ||
          !smoke_guard_pages_match(ctextures[1]) ||
          !smoke_guard_pages_match(cdepth))
         goto out;
      if (texture0_alpha) {
         static const struct {
            float coord;
            uint32_t expected_rgb;
         } repeat_cases[] = {
            { 1.109375f, 0x800000 }, { -1.109375f, 0x808000 },
            { 16.109375f, 0x800000 }, { -16.109375f, 0x808000 },
            { 31.109375f, 0x800000 }, { -31.109375f, 0x808000 },
         };
         struct pipe_sampler_state repeat_sampler = sampler;
         void *repeat_samplers[2];
         unsigned baseline_alpha = pixels[center] >> 24;

         if (baseline_alpha < 0x7f || baseline_alpha > 0x81)
            goto out;
         repeat_sampler.wrap_s = PIPE_TEX_WRAP_REPEAT;
         repeat_sampler.wrap_t = PIPE_TEX_WRAP_REPEAT;
         repeat_sampler_handle =
            context->create_sampler_state(context, &repeat_sampler);
         if (!repeat_sampler_handle)
            goto out;
         repeat_samplers[0] = repeat_sampler_handle;
         repeat_samplers[1] = sampler_handles[1];
         context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 2,
                                      repeat_samplers);
         for (unsigned v = 0; v < ARRAY_SIZE(vertices); v++) {
            vertices[v].color[0] = 1.0f;
            vertices[v].color[1] = 1.0f;
            vertices[v].color[2] = 0.0f;
            vertices[v].color[3] = 0.5f;
         }
         for (unsigned c = 0; c < ARRAY_SIZE(repeat_cases); c++) {
            uint64_t repeat_submitted = ccontext->submitted_triangles;
            uint64_t repeat_rejected = ccontext->rejected_triangles;

            for (unsigned v = 0; v < ARRAY_SIZE(vertices); v++) {
               vertices[v].texcoord0[0] = repeat_cases[c].coord;
               vertices[v].texcoord0[1] = repeat_cases[c].coord;
            }
            context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                           NULL, &clear, 1.0, 0);
            context->draw_vbo(context, &info, 0, NULL, &draw, 1);
            context->flush(context, NULL, 0);
            uint32_t pixel = pixels[center];

            for (unsigned shift = 0; shift <= 16; shift += 8)
               if (abs((int)(pixel >> shift & 0xff) -
                       (int)(repeat_cases[c].expected_rgb >> shift & 0xff)) >
                   1)
                  goto out;
            if ((pixel >> 24) < 0x3e || (pixel >> 24) > 0x41 ||
                ccontext->submitted_triangles != repeat_submitted + 2 ||
                ccontext->rejected_triangles != repeat_rejected ||
                !smoke_guard_pages_match(ctarget) ||
                !smoke_guard_pages_match(ctextures[0]) ||
                !smoke_guard_pages_match(ctextures[1]) ||
                !smoke_guard_pages_match(cdepth))
               goto out;
         }
         {
            struct drm_chrome9_draw_two_textured_triangle_d16_blend oracle = {
               .destination_handle = ctarget->bo.handle,
               .depth_handle = cdepth->bo.handle,
               .flags = DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY |
                        DRM_CHROME9_DEPTH_FUNC(
                           DRM_CHROME9_COMPARE_LEQUAL),
               .width = target->width0,
               .height = target->height0,
               .destination_pitch = ctarget->stride,
               .depth_pitch = cdepth->stride,
               .blend = {
                  .rgb_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE,
                  .rgb_dst_factor = DRM_CHROME9_BLEND_FACTOR_ZERO,
                  .alpha_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE,
                  .alpha_dst_factor = DRM_CHROME9_BLEND_FACTOR_ZERO,
                  .color_mask = DRM_CHROME9_COLOR_MASK_R |
                                DRM_CHROME9_COLOR_MASK_G |
                                DRM_CHROME9_COLOR_MASK_B |
                                DRM_CHROME9_COLOR_MASK_A,
               },
               .vertices = {
                  {
                     .x_16_16 = -0x8000, .y_16_16 = -0x8000,
                     .z_16_16 = 0x8000, .rhw_float = 0x3f800000,
                     .color = 0x80000000, .s0_float = 0x41f8e000,
                     .t0_float = 0x3f000000, .s1_float = 0x3f000000,
                     .t1_float = 0x3f000000,
                  },
                  {
                     .x_16_16 = 0x3f8000, .y_16_16 = -0x8000,
                     .z_16_16 = 0x8000, .rhw_float = 0x3e800000,
                     .color = 0x80ff0000, .s0_float = 0x41f8e000,
                     .t0_float = 0x3f000000, .s1_float = 0x3f000000,
                     .t1_float = 0x3f000000,
                  },
                  {
                     .x_16_16 = 0x3f8000, .y_16_16 = 0x3f8000,
                     .z_16_16 = 0x8000, .rhw_float = 0x3e800000,
                     .color = 0x80ff0000, .s0_float = 0x41f8e000,
                     .t0_float = 0x3f000000, .s1_float = 0x3f000000,
                     .t1_float = 0x3f000000,
                  },
               },
               .destination_format =
                  DRM_CHROME9_DESTINATION_FORMAT_BGRA8888,
            };
            uint16_t *depth_pixels =
               (uint16_t *)((uint8_t *)cdepth->bo.map + cdepth->data_offset);

            for (unsigned unit = 0; unit < 2; unit++) {
               struct drm_chrome9_texture_state *state =
                  &oracle.textures[unit];

               state->handle = ctextures[unit]->bo.handle;
               state->width = textures[unit]->width0;
               state->height = textures[unit]->height0;
               state->pitch = ctextures[unit]->stride;
               state->format = unit ? DRM_CHROME9_TEXTURE_FORMAT_BGRX8888 :
                                      DRM_CHROME9_TEXTURE_FORMAT_BGRA8888;
               state->min_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
               state->mag_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
               state->wrap_s = unit ? DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE :
                                      DRM_CHROME9_TEXTURE_WRAP_REPEAT;
               state->wrap_t = state->wrap_s;
            }
            for (unsigned y = 0; y < textures[0]->height0; y++)
               for (unsigned x = 0; x < textures[0]->width0; x++)
                  source0[y * (ctextures[0]->stride / 4) + x] = 0xffffffff;
            for (unsigned y = 0; y < textures[1]->height0; y++)
               for (unsigned x = 0; x < textures[1]->width0; x++)
                  source1[y * (ctextures[1]->stride / 4) + x] = 0x00ffffff;
            memset((uint8_t *)ctarget->bo.map + ctarget->data_offset, 0,
                   ctarget->data_size);
            memset(depth_pixels, 0xff, cdepth->data_size);
            if (!chrome9_draw_two_textured_triangle_d16_blend(
                   screen->get_screen_fd(screen), &oracle))
               goto out;
            uint32_t pixel = pixels[16 * (ctarget->stride / 4) + 48];

            if ((pixel >> 24) != 0x80 || (pixel & 0xffff) ||
                (pixel >> 16 & 0xff) < 0x6f ||
                (pixel >> 16 & 0xff) > 0x74 ||
                !smoke_guard_pages_match(ctarget) ||
                !smoke_guard_pages_match(ctextures[0]) ||
                !smoke_guard_pages_match(ctextures[1]) ||
                !smoke_guard_pages_match(cdepth))
               goto out;
            puts("Gallium program24-equivalent unequal-RHW primary "
                 "perspective passed");
         }
         puts("Gallium program24-equivalent TEX0*primary alpha with signed "
              "REPEAT passed");
      } else {
         puts("Gallium two projected XRGB textures D16+blend passed");
      }
      passed = true;
      goto out;
   }
   if (depth_test) {
      uint16_t *depth_pixels = (uint16_t *)((uint8_t *)cdepth->bo.map +
                                            cdepth->data_offset);
      const unsigned center = 32 * (ctarget->stride / 4) + 32;
      const unsigned depth_center = 32 * (cdepth->stride / 2) + 32;
      const uint32_t equal_pixel = pixels[center] & 0xffffff;
      const uint16_t equal_depth = depth_pixels[depth_center];

      for (unsigned y = 0; y < textures[1]->height0; y++)
         for (unsigned x = 0; x < textures[1]->width0; x++)
            source1[y * (ctextures[1]->stride / 4) + x] = 0x00404040;
      context->bind_depth_stencil_alpha_state(context, less_dsa_handle);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      if ((pixels[center] & 0xffffff) != equal_pixel ||
          depth_pixels[depth_center] != equal_depth)
         goto out;

      for (unsigned i = 0; i < ARRAY_SIZE(vertices); i++)
         vertices[i].position[2] = -0.5f;
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush(context, NULL, 0);
      for (unsigned i = 0; i < ARRAY_SIZE(sample_xy); i++) {
         uint32_t pixel =
            pixels[sample_xy[i][1] * (ctarget->stride / 4) + sample_xy[i][0]] &
            0xffffff;
         uint32_t near_expected = expected[i] >> 1;

         for (unsigned shift = 0; shift <= 16; shift += 8)
            if (abs((int)(pixel >> shift & 0xff) -
                    (int)(near_expected >> shift & 0xff)) > 1)
               goto out;
      }
      if (equal_depth < 0x7ffe || equal_depth > 0x8001 ||
          depth_pixels[depth_center] < 0x3ffe ||
          depth_pixels[depth_center] > 0x4001 ||
          ccontext->submitted_triangles != submitted + 6 ||
          ccontext->rejected_triangles != rejected ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(ctextures[0]) ||
          !smoke_guard_pages_match(ctextures[1]) ||
          !smoke_guard_pages_match(cdepth))
         goto out;
      puts("Gallium two-unit NPOT MODULATE D16 LEQUAL/LESS passed");
      if (read_only_dsa_handle) {
         uint64_t read_only_submitted = ccontext->submitted_triangles;
         uint64_t read_only_rejected = ccontext->rejected_triangles;

         for (unsigned i = 0; i < ARRAY_SIZE(vertices); i++)
            vertices[i].position[2] = 0.0f;
         context->bind_depth_stencil_alpha_state(context,
                                                  read_only_dsa_handle);
         context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                        PIPE_MASK_RGBA, 0xff, NULL, &clear, 0.5, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         for (unsigned i = 0; i < ARRAY_SIZE(sample_xy); i++) {
            uint32_t pixel =
               pixels[sample_xy[i][1] * (ctarget->stride / 4) +
                      sample_xy[i][0]] & 0xffffff;
            uint32_t read_only_expected = expected[i] >> 1;

            for (unsigned shift = 0; shift <= 16; shift += 8)
               if (abs((int)(pixel >> shift & 0xff) -
                       (int)(read_only_expected >> shift & 0xff)) > 1)
                  goto out;
         }
         if (depth_pixels[depth_center] < 0x7ffe ||
             depth_pixels[depth_center] > 0x8001 ||
             ccontext->submitted_triangles != read_only_submitted + 2 ||
             ccontext->rejected_triangles != read_only_rejected ||
             !smoke_guard_pages_match(ctarget) ||
             !smoke_guard_pages_match(ctextures[0]) ||
             !smoke_guard_pages_match(ctextures[1]) ||
             !smoke_guard_pages_match(cdepth))
            goto out;
         puts("Gallium two-unit NPOT MODULATE D16 read-only passed");
      }
      passed = true;
      goto out;
   }
   for (unsigned y = 0; y < textures[0]->height0; y++) {
      for (unsigned x = 0; x < textures[0]->width0; x++) {
         const uint32_t expected_source =
            y < 9 ? (x < 15 ? 0x00ff0000 : 0x000000ff) :
                    (x < 15 ? 0x0000ff00 : 0x00ffffff);

         if (source0[y * (ctextures[0]->stride / 4) + x] != expected_source)
            goto out;
      }
   }
   for (unsigned y = 0; y < textures[1]->height0; y++)
      for (unsigned x = 0; x < textures[1]->width0; x++)
         if (source1[y * (ctextures[1]->stride / 4) + x] != 0x00808080)
            goto out;
   if (ccontext->submitted_triangles != submitted + 2 ||
       ccontext->rejected_triangles != rejected ||
       !smoke_guard_pages_match(ctarget) ||
       !smoke_guard_pages_match(ctextures[0]) ||
       !smoke_guard_pages_match(ctextures[1]))
      goto out;
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS)) {
      static const struct {
         float coord;
         uint32_t expected;
      } repeat_cases[] = {
         { 1.109375f, 0x800000 }, { -1.109375f, 0x808080 },
         { 16.109375f, 0x800000 }, { -16.109375f, 0x808080 },
         { 31.109375f, 0x800000 }, { -31.109375f, 0x808080 },
      };
      struct pipe_sampler_state repeat_sampler = sampler;
      void *repeat_samplers[2];

      repeat_sampler.wrap_s = PIPE_TEX_WRAP_REPEAT;
      repeat_sampler.wrap_t = PIPE_TEX_WRAP_REPEAT;
      repeat_sampler_handle =
         context->create_sampler_state(context, &repeat_sampler);
      if (!repeat_sampler_handle)
         goto out;
      repeat_samplers[0] = repeat_sampler_handle;
      repeat_samplers[1] = sampler_handles[1];
      context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 2,
                                   repeat_samplers);
      for (unsigned c = 0; c < ARRAY_SIZE(repeat_cases); c++) {
         uint64_t repeat_submitted = ccontext->submitted_triangles;
         uint64_t repeat_rejected = ccontext->rejected_triangles;

         for (unsigned v = 0; v < ARRAY_SIZE(vertices); v++) {
            vertices[v].texcoord0[0] = repeat_cases[c].coord;
            vertices[v].texcoord0[1] = repeat_cases[c].coord;
         }
         context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff,
                        NULL, &clear, 1.0, 0);
         context->draw_vbo(context, &info, 0, NULL, &draw, 1);
         context->flush(context, NULL, 0);
         uint32_t pixel = pixels[32 * (ctarget->stride / 4) + 32] &
                          0xffffff;

         for (unsigned shift = 0; shift <= 16; shift += 8)
            if (abs((int)(pixel >> shift & 0xff) -
                    (int)(repeat_cases[c].expected >> shift & 0xff)) > 1)
               goto out;
         if (ccontext->submitted_triangles != repeat_submitted + 2 ||
             ccontext->rejected_triangles != repeat_rejected ||
             !smoke_guard_pages_match(ctarget) ||
             !smoke_guard_pages_match(ctextures[0]) ||
             !smoke_guard_pages_match(ctextures[1]))
            goto out;
      }
      puts("Gallium two-unit signed REPEAT coordinates through +/-31.109375 "
           "passed");
   }
   puts("Gallium two-unit NPOT 30x18*22x14 MODULATE passed");
   passed = true;

out:
   if (context) {
      if (vs) context->delete_vs_state(context, vs);
      if (fs) context->delete_fs_state(context, fs);
      for (unsigned i = 0; i < 2; i++) {
         if (views[i]) pipe_sampler_view_reference(&views[i], NULL);
         if (sampler_handles[i])
            context->delete_sampler_state(context, sampler_handles[i]);
      }
      if (repeat_sampler_handle)
         context->delete_sampler_state(context, repeat_sampler_handle);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (less_dsa_handle)
         context->delete_depth_stencil_alpha_state(context, less_dsa_handle);
      if (read_only_dsa_handle)
         context->delete_depth_stencil_alpha_state(context,
                                                   read_only_dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      context->destroy(context);
   }
   for (unsigned i = 0; i < 2; i++)
      if (textures[i]) screen->resource_destroy(screen, textures[i]);
   if (depth) screen->resource_destroy(screen, depth);
   if (target) screen->resource_destroy(screen, target);
   return passed;
}

static bool
smoke_a8_generic_texture(struct pipe_screen *screen)
{
   static const struct smoke_texture_vertex vertices[4] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0 } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 1, 0 } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 1, 1 } },
   };
   static const uint8_t alphas[4] = { 0x24, 0x69, 0xb6, 0xf3 };
   static const unsigned sample_xy[4][2] = {
      { 8, 8 }, { 24, 8 }, { 8, 24 }, { 24, 24 },
   };
   struct pipe_resource texture_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_A8_UNORM,
      .width0 = 2, .height0 = 2, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource target_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *texture = NULL, *target = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_templ;
   struct pipe_sampler_view *view = NULL;
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
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true,
      .bottom_edge_rule = true,
      .depth_clip_near = true,
      .depth_clip_far = true,
   };
   struct pipe_vertex_element elements[2] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 32, .height = 32, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8A8_UNORM },
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 16, 16, 0.5 }, .translate = { 16, 16, 0.5 },
   };
   const struct pipe_scissor_state scissor = { .maxx = 32, .maxy = 32 };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   const struct pipe_draw_start_count_bias draw = { .count = 4 };
   struct chrome9_resource *ctexture, *ctarget;
   struct chrome9_context *ccontext;
   const uint32_t *pixels;
   uint8_t *source, *source_copy = NULL;
   uint32_t actual[4] = { 0 };
   uint32_t mismatch_actual = 0, mismatch_expected = 0;
   unsigned mismatch_x = 32, mismatch_y = 32;
   uint64_t submitted, rejected, batches;
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *sampler_handle = NULL, *vs = NULL, *fs = NULL;
   bool passed = false;

   if (!screen->is_format_supported(screen, texture_templ.format,
                                    texture_templ.target, 1, 1,
                                    texture_templ.bind) ||
       !screen->is_format_supported(screen, target_templ.format,
                                    target_templ.target, 1, 1,
                                    target_templ.bind) ||
       !chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM |
                  DRM_CHROME9_CAP_FRAGMENT_TEXTURE_LEGACY_FORMATS))
      goto out;
   texture = screen->resource_create(screen, &texture_templ);
   target = screen->resource_create(screen, &target_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!texture || !target || !context)
      goto out;
   ctexture = chrome9_resource(texture);
   ctarget = chrome9_resource(target);
   ccontext = chrome9_context(context);
   if (!ctexture->guarded || !ctarget->guarded ||
       ctexture->stride < 2 || ctarget->stride < 32 * sizeof(uint32_t) ||
       !smoke_initialize_guard_pages(ctexture) ||
       !smoke_initialize_guard_pages(ctarget))
      goto out;
   source = (uint8_t *)ctexture->bo.map + ctexture->data_offset;
   memset(source, 0xa5, ctexture->data_size);
   for (unsigned y = 0; y < 2; y++)
      for (unsigned x = 0; x < 2; x++)
         source[y * ctexture->stride + x] = alphas[y * 2 + x];
   source_copy = malloc(ctexture->data_size);
   if (!source_copy)
      goto out;
   memcpy(source_copy, source, ctexture->data_size);
   memset((uint8_t *)ctarget->bo.map + ctarget->data_offset, 0x5a,
          ctarget->data_size);

   u_sampler_view_default_template(&view_templ, texture, texture->format);
   view = context->create_sampler_view(context, texture, &view_templ);
   if (!view || view->swizzle_r != PIPE_SWIZZLE_X ||
       view->swizzle_g != PIPE_SWIZZLE_Y ||
       view->swizzle_b != PIPE_SWIZZLE_Z ||
       view->swizzle_a != PIPE_SWIZZLE_W)
      goto out;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_texture_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[0].src_stride = sizeof(struct smoke_texture_vertex);
   elements[1].src_offset = offsetof(struct smoke_texture_vertex, texcoord);
   elements[1].src_format = PIPE_FORMAT_R32G32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_texture_vertex);
   elements_handle = context->create_vertex_elements_state(context, 2,
                                                            elements);
   sampler_handle = context->create_sampler_state(context, &sampler);
   vs = make_texture_vertex_shader(screen, context, false);
   fs = make_texture_fragment_shader(screen, context, false);
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !sampler_handle || !vs || !fs ||
       !((struct chrome9_shader *)fs)->semantic_fs_valid ||
       ((struct chrome9_shader *)fs)->semantic_input_mask ||
       ((struct chrome9_shader *)fs)->semantic_sampler_mask != 1)
      goto out;

   framebuffer.cbufs[0].texture = target;
   context->set_framebuffer_state(context, &framebuffer);
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                &sampler_handle);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                              &view);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   submitted = ccontext->submitted_triangles;
   rejected = ccontext->rejected_triangles;
   batches = ccontext->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, target);
   context->flush(context, NULL, 0);

   pixels = (const uint32_t *)((const uint8_t *)ctarget->bo.map +
                               ctarget->data_offset);
   for (unsigned i = 0; i < 4; i++)
      actual[i] = pixels[sample_xy[i][1] * (ctarget->stride / 4) +
                         sample_xy[i][0]];
   passed = ccontext->submitted_triangles == submitted + 2 &&
            ccontext->rejected_triangles == rejected &&
            ccontext->submitted_program_batches == batches + 1 &&
            smoke_guard_pages_match(ctarget) &&
            smoke_guard_pages_match(ctexture) &&
            !memcmp(source, source_copy, ctexture->data_size);
   for (unsigned i = 0; i < 4; i++)
      passed &= actual[i] == (uint32_t)alphas[i] << 24;
   for (unsigned y = 0; y < 32; y++)
      for (unsigned x = 0; x < 32; x++) {
         uint32_t expected =
            (uint32_t)alphas[(y >= 16) * 2 + (x >= 16)] << 24;
         uint32_t pixel = pixels[y * (ctarget->stride / 4) + x];

         if (pixel == expected)
            continue;
         if (mismatch_y == 32) {
            mismatch_x = x;
            mismatch_y = y;
            mismatch_actual = pixel;
            mismatch_expected = expected;
         }
         passed = false;
      }
   if (!passed)
      fprintf(stderr, "A8 generic TEX mismatch: samples=%08x,%08x,%08x,%08x "
              "submitted=+%" PRIu64 " rejected=+%" PRIu64
              " batches=+%" PRIu64 " source=%u guards=%u/%u "
              "first_bad=(%u,%u):%08x/%08x\n",
              actual[0], actual[1], actual[2], actual[3],
              ccontext->submitted_triangles - submitted,
              ccontext->rejected_triangles - rejected,
              ccontext->submitted_program_batches - batches,
              !memcmp(source, source_copy, ctexture->data_size),
              smoke_guard_pages_match(ctarget),
              smoke_guard_pages_match(ctexture),
              mismatch_x, mismatch_y, mismatch_actual, mismatch_expected);
   else
      puts("Gallium guarded A8 nearest generic TEX passed");

out:
   free(source_copy);
   if (context) {
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (fs) context->delete_fs_state(context, fs);
      if (vs) context->delete_vs_state(context, vs);
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
   if (target) screen->resource_destroy(screen, target);
   if (texture) screen->resource_destroy(screen, texture);
   return passed;
}

static bool
smoke_a8_alpha_kill(struct pipe_screen *screen)
{
   static const struct {
      const char *name;
      float s;
      bool linear;
      bool killed;
   } cases[] = {
      { "below", 0.25f, false, true },
      { "above", 0.75f, false, false },
      { "midpoint", 0.5f, true, false },
   };
   struct smoke_texture_vertex vertices[4] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0.5f } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0.5f } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0.5f } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0.5f } },
   };
   struct pipe_resource texture_templ = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_A8_UNORM,
      .width0 = 2, .height0 = 2, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR,
   };
   struct pipe_resource target_templ = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource depth_templ = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *texture = NULL, *target = NULL, *depth = NULL;
   struct pipe_context *context = NULL;
   struct pipe_sampler_view view_templ, *view = NULL;
   struct pipe_sampler_state sampler = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .compare_mode = PIPE_TEX_COMPARE_NONE,
      .max_anisotropy = 1,
      .max_lod = 1000.0f,
      .reduction_mode = PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE,
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LESS,
   };
   struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true, .bottom_edge_rule = true,
      .depth_clip_near = true, .depth_clip_far = true,
   };
   struct pipe_vertex_element elements[2] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true, .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 32, .height = 32, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8A8_UNORM },
      .zsbuf = { .format = PIPE_FORMAT_Z16_UNORM },
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 16, 16, 0.5f }, .translate = { 16, 16, 0.5f },
   };
   const struct pipe_scissor_state scissor = { .maxx = 32, .maxy = 32 };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   const struct pipe_draw_start_count_bias draw = { .count = 4 };
   const union pipe_color_union clear = { 0 };
   struct chrome9_resource *ctexture, *ctarget, *cdepth;
   struct chrome9_context *ccontext;
   uint8_t *texels, *texture_copy = NULL;
   uint32_t *pixels;
   uint16_t *depth_pixels;
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *sampler_handles[2] = { NULL, NULL };
   void *vs = NULL, *fs = NULL, *sample_fs = NULL;
   bool passed = false;

   if (!debug_get_bool_option("CHROME9_EXPERIMENTAL_TEXKILL", false) ||
       !chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_FRAGMENT_KILL) ||
       !chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM |
                  DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16 |
                  DRM_CHROME9_CAP_FRAGMENT_TEXTURE_LEGACY_FORMATS)) {
      fputs("--kill-a8-only requires TEXKILL opt-in and kernel capabilities\n",
            stderr);
      return false;
   }
   if (!screen->is_format_supported(screen, texture_templ.format,
                                    texture_templ.target, 1, 1,
                                    texture_templ.bind) ||
       !screen->is_format_supported(screen, target_templ.format,
                                    target_templ.target, 1, 1,
                                    target_templ.bind) ||
       !screen->is_format_supported(screen, depth_templ.format,
                                    depth_templ.target, 1, 1,
                                    depth_templ.bind))
      return false;
   texture = screen->resource_create(screen, &texture_templ);
   target = screen->resource_create(screen, &target_templ);
   depth = screen->resource_create(screen, &depth_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!texture || !target || !depth || !context)
      goto out;
   ctexture = chrome9_resource(texture);
   ctarget = chrome9_resource(target);
   cdepth = chrome9_resource(depth);
   ccontext = chrome9_context(context);
   if (!ctexture->guarded || !ctarget->guarded || !cdepth->guarded ||
       ctexture->stride < 2 || ctarget->stride < 32 * sizeof(uint32_t) ||
       cdepth->stride < 32 * sizeof(uint16_t) ||
       !smoke_initialize_guard_pages(ctexture) ||
       !smoke_initialize_guard_pages(ctarget) ||
       !smoke_initialize_guard_pages(cdepth))
      goto out;
   texels = (uint8_t *)ctexture->bo.map + ctexture->data_offset;
   memset(texels, 0xa5, ctexture->data_size);
   for (unsigned y = 0; y < 2; y++) {
      texels[y * ctexture->stride] = 0x7f;
      texels[y * ctexture->stride + 1] = 0x80;
   }
   texture_copy = malloc(ctexture->data_size);
   if (!texture_copy)
      goto out;
   memcpy(texture_copy, texels, ctexture->data_size);
   u_sampler_view_default_template(&view_templ, texture, texture->format);
   view = context->create_sampler_view(context, texture, &view_templ);
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_texture_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[0].src_stride = sizeof(struct smoke_texture_vertex);
   elements[1].src_offset = offsetof(struct smoke_texture_vertex, texcoord);
   elements[1].src_format = PIPE_FORMAT_R32G32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_texture_vertex);
   elements_handle = context->create_vertex_elements_state(context, 2,
                                                            elements);
   sampler.min_img_filter = sampler.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   sampler_handles[0] = context->create_sampler_state(context, &sampler);
   sampler.min_img_filter = sampler.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   sampler_handles[1] = context->create_sampler_state(context, &sampler);
   vs = make_texture_vertex_shader(screen, context, false);
   fs = make_alpha_kill_fragment_shader(screen, context);
   sample_fs = make_texture_fragment_shader(screen, context, false);
   if (!view || view->swizzle_a != PIPE_SWIZZLE_W ||
       !blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !sampler_handles[0] || !sampler_handles[1] ||
       !vs || !fs || !sample_fs ||
       !((struct chrome9_shader *)sample_fs)->supported ||
       !((struct chrome9_shader *)sample_fs)->semantic_fs_valid ||
       !((struct chrome9_shader *)fs)->supported ||
       !((struct chrome9_shader *)fs)->semantic_fs_valid ||
       ((struct chrome9_shader *)fs)->semantic_fs_ir.count != 4 ||
       ((struct chrome9_shader *)fs)->semantic_fs_ir.instruction[0].op !=
          CHROME9_PS_OPCODE_TEX ||
       ((struct chrome9_shader *)fs)->semantic_fs_ir.instruction[1].op !=
          CHROME9_PS_OPCODE_SUB ||
       ((struct chrome9_shader *)fs)->semantic_fs_ir.instruction[2].op !=
          CHROME9_PS_OPCODE_KILL ||
       ((struct chrome9_shader *)fs)->semantic_fs_ir.instruction[3].op !=
          CHROME9_PS_OPCODE_MOV)
      goto out;

   framebuffer.cbufs[0].texture = target;
   framebuffer.zsbuf.texture = depth;
   context->set_framebuffer_state(context, &framebuffer);
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   pixels = (uint32_t *)((uint8_t *)ctarget->bo.map + ctarget->data_offset);
   depth_pixels = (uint16_t *)((uint8_t *)cdepth->bo.map + cdepth->data_offset);
   for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
      void *sampler_handle = sampler_handles[cases[c].linear];
      uint64_t submitted, rejected;
      uint32_t sampled_pixel;
      uint8_t sampled_alpha;
      bool discarded, preserved;
      uint32_t pixel;
      uint16_t z;

      for (unsigned v = 0; v < ARRAY_SIZE(vertices); v++)
         vertices[v].texcoord[0] = cases[c].s;
      context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                   &sampler_handle);
      context->bind_fs_state(context, sample_fs);
      submitted = ccontext->submitted_triangles;
      rejected = ccontext->rejected_triangles;
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, target);
      context->flush_resource(context, depth);
      context->flush(context, NULL, 0);
      sampled_pixel = pixels[16 * (ctarget->stride / 4) + 16];
      sampled_alpha = sampled_pixel >> 24;
      z = depth_pixels[16 * (cdepth->stride / 2) + 16];
      if (ccontext->submitted_triangles != submitted + 2 ||
          ccontext->rejected_triangles != rejected ||
          (sampled_pixel & 0x00ffffff) ||
          (c == 0 && sampled_alpha != 0x7f) ||
          (c == 1 && sampled_alpha != 0x80) ||
          (c == 2 && sampled_alpha != 0x7f && sampled_alpha != 0x80) ||
          z < 0x7ffe || z > 0x8001 ||
          !smoke_guard_pages_match(ctarget) ||
          !smoke_guard_pages_match(cdepth) ||
          !smoke_guard_pages_match(ctexture) ||
          memcmp(texels, texture_copy, ctexture->data_size)) {
         fprintf(stderr, "A8 alpha sample %s mismatch: color=%08x depth=%04x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64 "\n",
                 cases[c].name, sampled_pixel, z,
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected);
         passed = false;
         break;
      }
      fprintf(stderr, "A8 alpha sample %s: %02x\n",
              cases[c].name, sampled_alpha);
      /* BGRA8 quantizes the sampled result.  At the filtered midpoint its
       * readback byte cannot establish the sign of the internal predicate.
       * Check color/depth coherence but report the classification separately.
       */
      context->bind_fs_state(context, fs);
      submitted = ccontext->submitted_triangles;
      rejected = ccontext->rejected_triangles;
      context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                     PIPE_MASK_RGBA, 0xff, NULL, &clear, 1.0, 0);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      context->flush_resource(context, target);
      context->flush_resource(context, depth);
      context->flush(context, NULL, 0);
      pixel = pixels[16 * (ctarget->stride / 4) + 16];
      z = depth_pixels[16 * (cdepth->stride / 2) + 16];
      discarded = pixel == 0 && z == 0xffff;
      preserved = !(pixel & 0x00ffffff) &&
                  (pixel >> 24) >= 0x7f && (pixel >> 24) <= 0x80 &&
                  z >= 0x7ffe && z <= 0x8001;
      passed = ccontext->submitted_triangles == submitted + 2 &&
               ccontext->rejected_triangles == rejected &&
               smoke_guard_pages_match(ctarget) &&
               smoke_guard_pages_match(cdepth) &&
               smoke_guard_pages_match(ctexture) &&
               !memcmp(texels, texture_copy, ctexture->data_size) &&
               (c == 2 ? discarded || preserved :
                  cases[c].killed ? discarded : preserved);
      if (!passed) {
         fprintf(stderr, "A8 alpha KILL %s mismatch: sample=%02x "
                 "color=%08x depth=%04x "
                 "submitted=+%" PRIu64 " rejected=+%" PRIu64
                 " guards=%u/%u/%u source=%u\n",
                 cases[c].name, sampled_alpha, pixel, z,
                 ccontext->submitted_triangles - submitted,
                 ccontext->rejected_triangles - rejected,
                 smoke_guard_pages_match(ctarget),
                 smoke_guard_pages_match(cdepth),
                 smoke_guard_pages_match(ctexture),
                 !memcmp(texels, texture_copy, ctexture->data_size));
         break;
      }
      if (c == 2)
         fprintf(stderr, "A8 alpha filtered midpoint: sample=%02x "
                 "KILL=%s (internal threshold unresolved)\n",
                 sampled_alpha, discarded ? "discarded" : "survived");
   }
   if (passed)
      puts("Gallium guarded A8 alpha KILL below/above D16 passed; "
           "midpoint recorded diagnostically");

out:
   free(texture_copy);
   if (context) {
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (sample_fs) context->delete_fs_state(context, sample_fs);
      if (fs) context->delete_fs_state(context, fs);
      if (vs) context->delete_vs_state(context, vs);
      for (unsigned i = 0; i < ARRAY_SIZE(sampler_handles); i++)
         if (sampler_handles[i])
            context->delete_sampler_state(context, sampler_handles[i]);
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
   if (depth) screen->resource_destroy(screen, depth);
   if (target) screen->resource_destroy(screen, target);
   if (texture) screen->resource_destroy(screen, texture);
   return passed;
}

static bool
smoke_position_draw(struct pipe_screen *screen)
{
   static const float vertices[3][4] = {
      { -0.75f, -0.75f, 0.0f, 1.0f },
      {  0.75f, -0.75f, 0.0f, 1.0f },
      { -0.75f,  0.75f, 0.0f, 1.0f },
   };
   static const struct {
      unsigned x, y, red, green;
   } samples[] = {
      { 16, 16, 66, 66 },
      { 24, 16, 98, 66 },
      { 16, 24, 66, 98 },
   };
   const float constants[3][4] = { { 0 }, { 0 }, { 1.0f, 0, 0, 0 } };
   const struct pipe_constant_buffer constant = {
      .buffer_size = sizeof(constants),
      .user_buffer = constants,
   };
   struct pipe_resource color_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource depth_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   const struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   const struct pipe_vertex_element element = {
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      .src_stride = sizeof(vertices[0]),
   };
   struct pipe_blend_state blend = { 0 };
   const struct pipe_depth_stencil_alpha_state dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LESS,
   };
   const struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true,
      .bottom_edge_rule = true,
      .depth_clip_near = true,
      .depth_clip_far = true,
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 32.0f, 32.0f, 0.5f },
      .translate = { 32.5f, 32.5f, 0.5f },
   };
   const struct pipe_scissor_state scissor = { .maxx = 64, .maxy = 64 };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES,
      .index_bounds_valid = true,
      .instance_count = 1,
      .min_index = 0,
      .max_index = 2,
   };
   const struct pipe_draw_start_count_bias draw = { .count = 3 };
   const union pipe_color_union clear_color = { 0 };
   struct pipe_framebuffer_state framebuffer = {
      .width = 64, .height = 64, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8X8_UNORM },
      .zsbuf = { .format = PIPE_FORMAT_Z16_UNORM },
   };
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   struct pipe_resource *color = NULL, *depth = NULL;
   struct pipe_context *context = NULL;
   struct chrome9_resource *ccolor, *cdepth;
   struct chrome9_shader *shader;
   const uint32_t *color_pixels;
   const uint16_t *depth_pixels;
   uint64_t submitted, rejected, batches;
   unsigned instruction_count = 0;
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *vs = NULL, *fs = NULL;
   bool position_seen = false, passed = false;

   if (!chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM |
                     DRM_CHROME9_CAP_FRAGMENT_POSITION) ||
       !screen->is_format_supported(screen, color_templ.format,
                                    color_templ.target, 1, 1,
                                    color_templ.bind) ||
       !screen->is_format_supported(screen, depth_templ.format,
                                    depth_templ.target, 1, 1,
                                    depth_templ.bind))
      goto out;
   color = screen->resource_create(screen, &color_templ);
   depth = screen->resource_create(screen, &depth_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!color || !depth || !context)
      goto out;
   ccolor = chrome9_resource(color);
   cdepth = chrome9_resource(depth);
   if (!ccolor->guarded || !cdepth->guarded ||
       ccolor->stride < 64 * sizeof(uint32_t) ||
       cdepth->stride < 64 * sizeof(uint16_t) ||
       ccolor->bo.handle == cdepth->bo.handle ||
       !smoke_initialize_guard_pages(ccolor) ||
       !smoke_initialize_guard_pages(cdepth))
      goto out;

   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements_handle = context->create_vertex_elements_state(context, 1,
                                                            &element);
   vs = make_solid_vertex_shader(screen, context);
   fs = make_position_draw_fs(screen, context);
   shader = fs;
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !vs || !shader || !shader->supported ||
       !shader->semantic_fs_valid || shader->semantic_input_mask ||
       shader->semantic_varying_mask || shader->semantic_sampler_mask ||
       !(shader->semantic_uniform_mask & (1u << 2)) ||
       !chrome9_fragment_build_instructions(&shader->semantic_fs_ir, 0,
                                            instructions,
                                            &instruction_count) ||
       instruction_count != shader->semantic_fs_ir.count)
      goto out;
   printf("POSITION semantic IR (%u):", instruction_count);
   for (unsigned i = 0; i < instruction_count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &shader->semantic_fs_ir.instruction[i];

      printf(" [%u op%u dst%u.%u/%x", i, instruction->op,
             instruction->destination.file, instruction->destination.id,
             instruction->destination.writemask);
      for (unsigned s = 0; s < 3; s++) {
         const struct chrome9_ps_source *source =
            &instruction->source[s];

         printf(" s%u=%u.%u/%u%u%u%u", s, source->file, source->id,
                source->swizzle[0], source->swizzle[1],
                source->swizzle[2], source->swizzle[3]);
         position_seen |= source->file == CHROME9_PS_SOURCE_POSITION;
      }
      putchar(']');
   }
   putchar('\n');
   fflush(stdout);
   if (!position_seen)
      goto out;
   if (debug_get_bool_option("CHROME9_POSITION_DRY_RUN", false)) {
      passed = true;
      goto out;
   }

   framebuffer.cbufs[0].texture = color;
   framebuffer.zsbuf.texture = depth;
   context->set_framebuffer_state(context, &framebuffer);
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, &constant);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                  PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
   submitted = chrome9_context(context)->submitted_triangles;
   rejected = chrome9_context(context)->rejected_triangles;
   batches = chrome9_context(context)->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, color);
   context->flush_resource(context, depth);
   context->flush(context, NULL, 0);

   color_pixels = (const uint32_t *)((const uint8_t *)ccolor->bo.map +
                                     ccolor->data_offset);
   depth_pixels = (const uint16_t *)((const uint8_t *)cdepth->bo.map +
                                     cdepth->data_offset);
   passed = chrome9_context(context)->submitted_triangles == submitted + 1 &&
            chrome9_context(context)->rejected_triangles == rejected &&
            chrome9_context(context)->submitted_program_batches ==
               batches + 1 &&
            smoke_guard_pages_match(ccolor) && smoke_guard_pages_match(cdepth) &&
            color_pixels[60 * (ccolor->stride / 4) + 60] == 0 &&
            depth_pixels[60 * (cdepth->stride / 2) + 60] == 0xffff;
   for (unsigned i = 0; i < ARRAY_SIZE(samples); i++) {
      uint32_t pixel = color_pixels[samples[i].y * (ccolor->stride / 4) +
                                    samples[i].x];
      uint16_t z = depth_pixels[samples[i].y * (cdepth->stride / 2) +
                                samples[i].x];
      unsigned red = pixel >> 16 & 0xff;
      unsigned green = pixel >> 8 & 0xff;
      unsigned blue = pixel & 0xff;

      passed &= abs((int)red - (int)samples[i].red) <= 1 &&
                abs((int)green - (int)samples[i].green) <= 1 &&
                abs((int)blue - (int)samples[i].red) <= 1 &&
                z >= 0x7ffe && z <= 0x8001;
      printf("POSITION sample (%u,%u): color=%08x depth=%04x\n",
             samples[i].x, samples[i].y, pixel, z);
   }
   if (passed)
      puts("Guarded 64x64 Gallium POSITION draw passed");
   else
      fprintf(stderr,
              "Guarded POSITION draw mismatch: outside=%08x/%04x "
              "submitted=+%" PRIu64 " rejected=+%" PRIu64
              " batches=+%" PRIu64 " guards=%u/%u\n",
              color_pixels[60 * (ccolor->stride / 4) + 60],
              depth_pixels[60 * (cdepth->stride / 2) + 60],
              chrome9_context(context)->submitted_triangles - submitted,
              chrome9_context(context)->rejected_triangles - rejected,
              chrome9_context(context)->submitted_program_batches - batches,
              smoke_guard_pages_match(ccolor), smoke_guard_pages_match(cdepth));

out:
   if (context) {
      if (fs) context->delete_fs_state(context, fs);
      if (vs) context->delete_vs_state(context, vs);
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
   if (depth) screen->resource_destroy(screen, depth);
   if (color) screen->resource_destroy(screen, color);
   return passed;
}

static bool
smoke_d24_combined_draw(struct pipe_screen *screen,
                        struct pipe_context *context,
                        struct pipe_resource *color,
                        struct pipe_resource *depth,
                        struct pipe_framebuffer_state *fb)
{
   static const float vertices[3][4] = {
      { -0.75f, -0.75f, 0.0f, 1.0f },
      {  0.75f, -0.75f, 0.0f, 1.0f },
      { -0.75f,  0.75f, 0.0f, 1.0f },
   };
   const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
   const struct pipe_constant_buffer constant = {
      .buffer_size = sizeof(red),
      .user_buffer = red,
   };
   const struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_vertex_element element = {
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      .src_stride = sizeof(vertices[0]),
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LESS,
   };
   struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true,
      .bottom_edge_rule = true,
      .depth_clip_near = true,
      .depth_clip_far = true,
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 32.0f, 32.0f, 0.5f },
      .translate = { 32.5f, 32.5f, 0.5f },
   };
   const struct pipe_scissor_state scissor = {
      .maxx = 64,
      .maxy = 64,
   };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES,
      .index_bounds_valid = true,
      .instance_count = 1,
      .min_index = 0,
      .max_index = 2,
   };
   const struct pipe_draw_start_count_bias draw = {
      .count = 3,
   };
   const union pipe_color_union clear_color = { 0 };
   struct chrome9_resource *ccolor = chrome9_resource(color);
   struct chrome9_resource *cdepth = chrome9_resource(depth);
   struct chrome9_resource *cscratch;
   const uint32_t *color_pixels, *depth_pixels;
   uint64_t submitted, rejected, program_batches;
   unsigned changed = 0;
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *vs = NULL, *fs = NULL;
   bool passed = false;

   if (!ccolor->guarded || ccolor->stride != 256 ||
       !smoke_initialize_guard_pages(ccolor))
      goto out;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements_handle = context->create_vertex_elements_state(context, 1,
                                                            &element);
   vs = make_solid_vertex_shader(screen, context);
   fs = make_constant_color_fs(screen, context, 0);
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !vs || !fs ||
       !((struct chrome9_shader *)fs)->supported ||
       !((struct chrome9_shader *)fs)->semantic_fs_valid ||
       ((struct chrome9_shader *)fs)->semantic_input_mask)
      goto out;

   fb->nr_cbufs = 1;
   fb->cbufs[0].format = color->format;
   fb->cbufs[0].texture = color;
   context->set_framebuffer_state(context, fb);
   if (chrome9_context(context)->render_target != color)
      goto out;
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, &constant);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                  PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
   submitted = chrome9_context(context)->submitted_triangles;
   rejected = chrome9_context(context)->rejected_triangles;
   program_batches = chrome9_context(context)->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, color);
   context->flush_resource(context, depth);
   context->flush(context, NULL, 0);

   color_pixels = (const uint32_t *)((const uint8_t *)ccolor->bo.map +
                                    ccolor->data_offset);
   depth_pixels = (const uint32_t *)((const uint8_t *)cdepth->bo.map +
                                    cdepth->data_offset);
   for (unsigned y = 0; y < 64; y++)
      for (unsigned x = 0; x < 64; x++) {
         uint32_t pixel = color_pixels[y * (ccolor->stride / 4) + x];
         uint32_t z = depth_pixels[y * (cdepth->stride / 4) + x];

         if (!pixel && z == 0xffffff00U)
            continue;
         if (pixel != 0x00ff0000U || (z >> 8) < 0x7fff00U ||
             (z >> 8) > 0x800100U) {
            fprintf(stderr, "X8Z24 draw pixel (%u,%u): color=%08x depth=%08x\n",
                    x, y, pixel, z);
            goto out;
         }
         changed++;
      }
   passed = changed >= 1100 && changed <= 1250 &&
            color_pixels[16 * (ccolor->stride / 4) + 16] == 0x00ff0000U &&
            depth_pixels[16 * (cdepth->stride / 4) + 16] != 0xffffff00U &&
            color_pixels[60 * (ccolor->stride / 4) + 60] == 0 &&
            depth_pixels[60 * (cdepth->stride / 4) + 60] == 0xffffff00U &&
            chrome9_context(context)->submitted_triangles == submitted + 1 &&
            chrome9_context(context)->rejected_triangles == rejected &&
            chrome9_context(context)->submitted_program_batches ==
               program_batches + 1 &&
            smoke_guard_pages_match(ccolor) && smoke_guard_pages_match(cdepth);
   if (passed)
      printf("X8Z24 combined Gallium draw passed: changed=%u center=%08x/%08x\n",
             changed,
             color_pixels[16 * (ccolor->stride / 4) + 16],
             depth_pixels[16 * (cdepth->stride / 4) + 16]);
   else
      fprintf(stderr,
              "X8Z24 combined Gallium draw mismatch: changed=%u "
              "center=%08x/%08x submitted=+%" PRIu64
              " rejected=+%" PRIu64 " program=+%" PRIu64 "\n",
              changed,
              color_pixels[16 * (ccolor->stride / 4) + 16],
              depth_pixels[16 * (cdepth->stride / 4) + 16],
              chrome9_context(context)->submitted_triangles - submitted,
              chrome9_context(context)->rejected_triangles - rejected,
              chrome9_context(context)->submitted_program_batches -
                 program_batches);
   if (!passed)
      goto out;

   /* A depth-only FBO still needs the hardware's color target.  Its private
    * scratch surface must survive the draw entirely unchanged. */
   passed = false;
   cscratch = chrome9_resource(chrome9_context(context)->depth_only_scratch);
   if (!cscratch || !cscratch->guarded || cscratch->stride != 256 ||
       !smoke_guard_pages_match(cscratch))
      goto out;
   memset((uint8_t *)cscratch->bo.map + cscratch->data_offset, 0xa5,
          cscratch->data_size);
   fb->nr_cbufs = 0;
   fb->cbufs[0].texture = NULL;
   context->set_framebuffer_state(context, fb);
   if (chrome9_context(context)->render_target !=
       &cscratch->base)
      goto out;
   context->clear(context, PIPE_CLEAR_DEPTH, 0, 0xff, NULL, NULL, 1.0, 0);
   submitted = chrome9_context(context)->submitted_triangles;
   rejected = chrome9_context(context)->rejected_triangles;
   program_batches = chrome9_context(context)->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, depth);
   context->flush(context, NULL, 0);

   changed = 0;
   for (unsigned y = 0; y < 64; y++)
      for (unsigned x = 0; x < 64; x++) {
         uint32_t z = depth_pixels[y * (cdepth->stride / 4) + x];
         uint32_t scratch_pixel = ((const uint32_t *)
            ((const uint8_t *)cscratch->bo.map + cscratch->data_offset))
               [y * (cscratch->stride / 4) + x];

         if (scratch_pixel != 0xa5a5a5a5U ||
             (z != 0xffffff00U &&
              ((z >> 8) < 0x7fff00U || (z >> 8) > 0x800100U))) {
            fprintf(stderr,
                    "X8Z24 depth-only pixel (%u,%u): scratch=%08x depth=%08x\n",
                    x, y, scratch_pixel, z);
            goto out;
         }
         changed += z != 0xffffff00U;
      }
   passed = changed >= 1100 && changed <= 1250 &&
            depth_pixels[16 * (cdepth->stride / 4) + 16] != 0xffffff00U &&
            depth_pixels[60 * (cdepth->stride / 4) + 60] == 0xffffff00U &&
            color_pixels[16 * (ccolor->stride / 4) + 16] == 0x00ff0000U &&
            chrome9_context(context)->submitted_triangles == submitted + 1 &&
            chrome9_context(context)->rejected_triangles == rejected &&
            chrome9_context(context)->submitted_program_batches ==
               program_batches + 1 &&
            smoke_guard_pages_match(ccolor) &&
            smoke_guard_pages_match(cdepth) &&
            smoke_guard_pages_match(cscratch);
   if (passed)
      printf("X8Z24 depth-only Gallium draw passed: changed=%u scratch intact\n",
             changed);
   else
      fprintf(stderr,
              "X8Z24 depth-only draw mismatch: changed=%u "
              "submitted=+%" PRIu64 " rejected=+%" PRIu64
              " program=+%" PRIu64 "\n",
              changed,
              chrome9_context(context)->submitted_triangles - submitted,
              chrome9_context(context)->rejected_triangles - rejected,
              chrome9_context(context)->submitted_program_batches -
                 program_batches);

out:
   if (fs) context->delete_fs_state(context, fs);
   if (vs) context->delete_vs_state(context, vs);
   if (elements_handle)
      context->delete_vertex_elements_state(context, elements_handle);
   if (rasterizer_handle)
      context->delete_rasterizer_state(context, rasterizer_handle);
   if (dsa_handle)
      context->delete_depth_stencil_alpha_state(context, dsa_handle);
   if (blend_handle)
      context->delete_blend_state(context, blend_handle);
   return passed;
}

static bool
smoke_d24_sample_produced_depth(struct pipe_screen *screen,
                                struct pipe_context *context,
                                struct pipe_resource *color,
                                struct pipe_resource *depth,
                                struct pipe_sampler_view *depth_view)
{
   static const struct smoke_texture_vertex vertices[4] = {
      { { -1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0 } },
      { { -1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 1, 1, 1 }, { 1, 0 } },
      { {  1,  1, 0, 1 }, { 1, 1, 1, 1 }, { 1, 1 } },
   };
   struct pipe_resource target_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource z16_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   struct pipe_resource *target = NULL, *z16 = NULL;
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
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_depth_stencil_alpha_state depth_dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LESS,
   };
   const union pipe_color_union clear_color = { 0 };
   struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true,
      .bottom_edge_rule = true,
      .depth_clip_near = true,
      .depth_clip_far = true,
   };
   struct pipe_vertex_element elements[2] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 64, .height = 64, .nr_cbufs = 1,
      .cbufs[0] = { .format = PIPE_FORMAT_B8G8R8A8_UNORM },
   };
   const struct pipe_viewport_state viewport = {
      .scale = { 32, 32, 0.5 }, .translate = { 32, 32, 0.5 },
   };
   const struct pipe_scissor_state scissor = { .maxx = 64, .maxy = 64 };
   const struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP, .index_bounds_valid = true,
      .instance_count = 1, .min_index = 0, .max_index = 3,
   };
   const struct pipe_draw_start_count_bias draw = { .count = 4 };
   struct chrome9_resource *ctarget, *cz16;
   struct chrome9_resource *ccolor = chrome9_resource(color);
   struct chrome9_resource *cdepth = chrome9_resource(depth);
   struct chrome9_resource *cscratch =
      chrome9_resource(chrome9_context(context)->depth_only_scratch);
   struct chrome9_context *ccontext = chrome9_context(context);
   const uint32_t *pixels;
   const uint16_t *z16_pixels;
   uint8_t *color_copy = NULL, *depth_copy = NULL;
   uint32_t center = 0, exterior = 0;
   uint64_t submitted, rejected, batches;
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *depth_dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *sampler_handle = NULL, *vs = NULL, *fs = NULL;
   bool passed = false;

   if (!screen->is_format_supported(screen, target_templ.format,
                                    target_templ.target, 1, 1,
                                    target_templ.bind) ||
       !depth_view || depth_view->texture != depth ||
       depth_view->swizzle_r != PIPE_SWIZZLE_X ||
       depth_view->swizzle_g != PIPE_SWIZZLE_Y ||
       depth_view->swizzle_b != PIPE_SWIZZLE_Z ||
       depth_view->swizzle_a != PIPE_SWIZZLE_W ||
       !cscratch || !smoke_guard_pages_match(cscratch))
      goto out;
   target = screen->resource_create(screen, &target_templ);
   if (!target)
      goto out;
   ctarget = chrome9_resource(target);
   if (!ctarget->guarded || !smoke_initialize_guard_pages(ctarget) ||
       ctarget->bo.handle == cdepth->bo.handle ||
       ctarget->bo.handle == ccolor->bo.handle ||
       cdepth->bo.handle == ccolor->bo.handle ||
       ctarget->bo.map == cdepth->bo.map ||
       ctarget->bo.map == ccolor->bo.map ||
       cdepth->bo.map == ccolor->bo.map)
      goto out;
   color_copy = malloc(ccolor->data_size);
   depth_copy = malloc(cdepth->data_size);
   if (!color_copy || !depth_copy)
      goto out;
   memcpy(color_copy, (const uint8_t *)ccolor->bo.map + ccolor->data_offset,
          ccolor->data_size);
   memcpy(depth_copy, (const uint8_t *)cdepth->bo.map + cdepth->data_offset,
          cdepth->data_size);
   memset((uint8_t *)ctarget->bo.map + ctarget->data_offset, 0x5a,
          ctarget->data_size);

   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_texture_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[0].src_stride = sizeof(struct smoke_texture_vertex);
   elements[1].src_offset = offsetof(struct smoke_texture_vertex, texcoord);
   elements[1].src_format = PIPE_FORMAT_R32G32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_texture_vertex);
   elements_handle = context->create_vertex_elements_state(context, 2,
                                                            elements);
   sampler_handle = context->create_sampler_state(context, &sampler);
   vs = make_texture_vertex_shader(screen, context, false);
   fs = make_texture_fragment_shader(screen, context, false);
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !sampler_handle || !vs || !fs ||
       !((struct chrome9_shader *)fs)->semantic_fs_valid ||
       ((struct chrome9_shader *)fs)->semantic_input_mask ||
       ((struct chrome9_shader *)fs)->semantic_sampler_mask != 1)
      goto out;

   /* Sampling the produced D24 image requires it to be detached from the
    * framebuffer.  The new color target is a distinct guarded BO. */
   framebuffer.cbufs[0].texture = target;
   context->set_framebuffer_state(context, &framebuffer);
   if (ccontext->render_target != target ||
       ccontext->framebuffer.zsbuf.texture)
      goto out;
   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->bind_sampler_states(context, MESA_SHADER_FRAGMENT, 0, 1,
                                &sampler_handle);
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 1, 0,
                              &depth_view);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   submitted = ccontext->submitted_triangles;
   rejected = ccontext->rejected_triangles;
   batches = ccontext->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, target);
   context->flush(context, NULL, 0);

   pixels = (const uint32_t *)((const uint8_t *)ctarget->bo.map +
                               ctarget->data_offset);
   center = pixels[16 * (ctarget->stride / 4) + 16];
   exterior = pixels[60 * (ctarget->stride / 4) + 60];
   /* X8Z24 is (Z,0,0,1) to Gallium.  The red byte may round at the
    * half-way depth, but green/blue must be zero and alpha must be one. */
   passed = (center == 0xff7f0000U || center == 0xff800000U) &&
            exterior == 0xffff0000U &&
            ccontext->submitted_triangles == submitted + 2 &&
            ccontext->rejected_triangles == rejected &&
            ccontext->submitted_program_batches == batches + 1 &&
            !memcmp((const uint8_t *)ccolor->bo.map + ccolor->data_offset,
                    color_copy, ccolor->data_size) &&
            !memcmp((const uint8_t *)cdepth->bo.map + cdepth->data_offset,
                    depth_copy, cdepth->data_size) &&
            smoke_bytes_match((const uint8_t *)cscratch->bo.map +
                                 cscratch->data_offset,
                              cscratch->data_size, 0xa5) &&
            smoke_guard_pages_match(ctarget) &&
            smoke_guard_pages_match(ccolor) &&
            smoke_guard_pages_match(cdepth) &&
            smoke_guard_pages_match(cscratch);
   if (passed)
      printf("X8Z24 Gallium render-to-texture passed: center=%08x exterior=%08x\n",
             center, exterior);
   else
      fprintf(stderr,
              "X8Z24 Gallium render-to-texture mismatch: "
              "center=%08x exterior=%08x submitted=+%" PRIu64
              " rejected=+%" PRIu64 " batches=+%" PRIu64
              " source=%u color=%u guards=%u/%u/%u/%u\n",
              center, exterior,
              ccontext->submitted_triangles - submitted,
              ccontext->rejected_triangles - rejected,
              ccontext->submitted_program_batches - batches,
              !memcmp((const uint8_t *)cdepth->bo.map + cdepth->data_offset,
                      depth_copy, cdepth->data_size),
              !memcmp((const uint8_t *)ccolor->bo.map + ccolor->data_offset,
                      color_copy, ccolor->data_size),
              smoke_guard_pages_match(ctarget),
              smoke_guard_pages_match(ccolor),
              smoke_guard_pages_match(cdepth),
              smoke_guard_pages_match(cscratch));
   if (!passed)
      goto out;

   /* The D24 source must also work with an unrelated active Z16 destination. */
   passed = false;
   if (!screen->is_format_supported(screen, z16_templ.format,
                                    z16_templ.target, 1, 1, z16_templ.bind))
      goto out;
   z16 = screen->resource_create(screen, &z16_templ);
   if (!z16)
      goto out;
   cz16 = chrome9_resource(z16);
   if (!cz16->guarded || !smoke_initialize_guard_pages(cz16) ||
       cz16->bo.handle == cdepth->bo.handle ||
       cz16->bo.handle == ctarget->bo.handle)
      goto out;
   depth_dsa_handle = context->create_depth_stencil_alpha_state(context,
                                                               &depth_dsa);
   if (!depth_dsa_handle)
      goto out;
   framebuffer.zsbuf.format = PIPE_FORMAT_Z16_UNORM;
   framebuffer.zsbuf.texture = z16;
   context->set_framebuffer_state(context, &framebuffer);
   context->bind_depth_stencil_alpha_state(context, depth_dsa_handle);
   context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH,
                  PIPE_MASK_RGBA, 0xff, NULL, &clear_color, 1.0, 0);
   submitted = ccontext->submitted_triangles;
   rejected = ccontext->rejected_triangles;
   batches = ccontext->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, target);
   context->flush_resource(context, z16);
   context->flush(context, NULL, 0);

   z16_pixels = (const uint16_t *)((const uint8_t *)cz16->bo.map +
                                   cz16->data_offset);
   center = pixels[16 * (ctarget->stride / 4) + 16];
   exterior = pixels[60 * (ctarget->stride / 4) + 60];
   passed = (center == 0xff7f0000U || center == 0xff800000U) &&
            exterior == 0xffff0000U &&
            z16_pixels[16 * (cz16->stride / 2) + 16] >= 0x7ffeU &&
            z16_pixels[16 * (cz16->stride / 2) + 16] <= 0x8001U &&
            z16_pixels[60 * (cz16->stride / 2) + 60] >= 0x7ffeU &&
            z16_pixels[60 * (cz16->stride / 2) + 60] <= 0x8001U &&
            ccontext->submitted_triangles == submitted + 2 &&
            ccontext->rejected_triangles == rejected &&
            ccontext->submitted_program_batches == batches + 1 &&
            !memcmp((const uint8_t *)cdepth->bo.map + cdepth->data_offset,
                    depth_copy, cdepth->data_size) &&
            !memcmp((const uint8_t *)ccolor->bo.map + ccolor->data_offset,
                    color_copy, ccolor->data_size) &&
            smoke_bytes_match((const uint8_t *)cscratch->bo.map +
                                 cscratch->data_offset,
                              cscratch->data_size, 0xa5) &&
            smoke_guard_pages_match(ctarget) &&
            smoke_guard_pages_match(cz16) &&
            smoke_guard_pages_match(ccolor) &&
            smoke_guard_pages_match(cdepth) &&
            smoke_guard_pages_match(cscratch);
   if (passed)
      printf("X8Z24 sample with Z16 destination passed: center=%08x depth=%04x\n",
             center, z16_pixels[16 * (cz16->stride / 2) + 16]);
   else
      fprintf(stderr,
              "X8Z24 sample with Z16 destination mismatch: "
              "center=%08x exterior=%08x depth=%04x/%04x "
              "submitted=+%" PRIu64 " rejected=+%" PRIu64
              " batches=+%" PRIu64 " guards=%u/%u/%u/%u/%u\n",
              center, exterior,
              z16_pixels[16 * (cz16->stride / 2) + 16],
              z16_pixels[60 * (cz16->stride / 2) + 60],
              ccontext->submitted_triangles - submitted,
              ccontext->rejected_triangles - rejected,
              ccontext->submitted_program_batches - batches,
              smoke_guard_pages_match(ctarget),
              smoke_guard_pages_match(cz16),
              smoke_guard_pages_match(ccolor),
              smoke_guard_pages_match(cdepth),
              smoke_guard_pages_match(cscratch));

out:
   free(color_copy);
   free(depth_copy);
   /* Drop the context's reference before the caller destroys its view. */
   context->set_sampler_views(context, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL);
   if (fs) context->delete_fs_state(context, fs);
   if (vs) context->delete_vs_state(context, vs);
   if (sampler_handle)
      context->delete_sampler_state(context, sampler_handle);
   if (elements_handle)
      context->delete_vertex_elements_state(context, elements_handle);
   if (rasterizer_handle)
      context->delete_rasterizer_state(context, rasterizer_handle);
   if (dsa_handle)
      context->delete_depth_stencil_alpha_state(context, dsa_handle);
   if (depth_dsa_handle)
      context->delete_depth_stencil_alpha_state(context, depth_dsa_handle);
   if (blend_handle)
      context->delete_blend_state(context, blend_handle);
   pipe_resource_reference(&target, NULL);
   pipe_resource_reference(&z16, NULL);
   return passed;
}

static bool
smoke_d24_framebuffer_contract(struct pipe_screen *screen)
{
   struct pipe_resource depth_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_X8Z24_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SAMPLER_VIEW |
              PIPE_BIND_LINEAR,
   };
   struct pipe_resource color_templ = depth_templ;
   struct pipe_framebuffer_state fb = {
      .width = 64, .height = 64,
   };
   struct pipe_sampler_view view_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_X8Z24_UNORM,
      .swizzle_r = PIPE_SWIZZLE_X,
      .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z,
      .swizzle_a = PIPE_SWIZZLE_W,
   };
   struct pipe_resource *depth = NULL, *color = NULL, *single_use = NULL;
   struct pipe_sampler_view *view = NULL;
   struct pipe_context *context = NULL;
   struct pipe_resource *scratch = NULL;
   const struct chrome9_resource *native;
   bool passed = false;
   unsigned stage = 1;

   if (!screen->is_format_supported(screen, depth_templ.format,
                                     depth_templ.target, 1, 1,
                                     depth_templ.bind) ||
       !screen->is_format_supported(screen, depth_templ.format,
                                     depth_templ.target, 1, 1,
                                     PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR) ||
       screen->is_format_supported(screen, PIPE_FORMAT_S8_UINT_Z24_UNORM,
                                    PIPE_TEXTURE_2D, 1, 1,
                                    depth_templ.bind) ||
       screen->is_format_supported(screen, depth_templ.format,
                                    depth_templ.target, 1, 1,
                                    depth_templ.bind | PIPE_BIND_SHARED))
      goto out;
   stage = 2;
   depth_templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
   single_use = screen->resource_create(screen, &depth_templ);
   if (!single_use)
      goto out;
   screen->resource_destroy(screen, single_use);
   single_use = NULL;
   stage = 3;
   depth_templ.bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR;
   single_use = screen->resource_create(screen, &depth_templ);
   if (!single_use)
      goto out;
   screen->resource_destroy(screen, single_use);
   single_use = NULL;
   stage = 4;
   depth_templ.bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SAMPLER_VIEW |
                      PIPE_BIND_LINEAR;
   depth_templ.last_level = 1;
   single_use = screen->resource_create(screen, &depth_templ);
   if (single_use)
      goto out;
   stage = 5;
   depth_templ.last_level = 0;
   depth = screen->resource_create(screen, &depth_templ);
   color_templ.format = PIPE_FORMAT_B8G8R8X8_UNORM;
   color_templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR;
   color = screen->resource_create(screen, &color_templ);
   context = screen->context_create(screen, NULL, 0);
   if (!depth || !color || !context)
      goto out;
   stage = 6;
   native = chrome9_resource(depth);
   if (!smoke_initialize_guard_pages(chrome9_resource(depth)))
      goto out;
   if (!native->guarded || native->stride != 256 ||
       native->data_size != 256 * 64 || !smoke_guard_pages_match(native)) {
      fprintf(stderr, "X8Z24 layout: guarded=%u stride=%u data=%zu first_guard=%08x\n",
              native->guarded, native->stride, native->data_size,
              native->bo.map ? ((const uint32_t *)native->bo.map)[0] : 0);
      goto out;
   }
   stage = 7;
   view = context->create_sampler_view(context, depth, &view_templ);
   if (!view)
      goto out;
   stage = 8;
   fb.zsbuf.format = PIPE_FORMAT_X8Z24_UNORM;
   fb.zsbuf.texture = depth;
   context->set_framebuffer_state(context, &fb);
   scratch = chrome9_context(context)->render_target;
   if (scratch && !smoke_initialize_guard_pages(chrome9_resource(scratch)))
      goto out;
   if (!scratch || scratch == depth ||
       scratch != chrome9_context(context)->depth_only_scratch ||
       !chrome9_resource(scratch)->guarded ||
       scratch->format != PIPE_FORMAT_B8G8R8X8_UNORM)
      goto out;
   stage = 9;
   context->clear(context, PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH, 0, 0xff,
                  NULL, NULL, 0.5, 0);
   if (((uint32_t *)((uint8_t *)native->bo.map + native->data_offset))[0] !=
          0x80000000 || !smoke_guard_pages_match(native) ||
       !smoke_guard_pages_match(chrome9_resource(scratch)))
      goto out;
   stage = 10;
   fb.nr_cbufs = 1;
   fb.cbufs[0].format = PIPE_FORMAT_B8G8R8X8_UNORM;
   fb.cbufs[0].texture = color;
   context->set_framebuffer_state(context, &fb);
   if (chrome9_context(context)->render_target != color)
      goto out;
   stage = 11;
   fb.nr_cbufs = 0;
   fb.cbufs[0].texture = NULL;
   context->set_framebuffer_state(context, &fb);
   if (chrome9_context(context)->render_target != scratch ||
       !smoke_guard_pages_match(native))
      goto out;
   stage = 12;
   if (!smoke_d24_combined_draw(screen, context, color, depth, &fb))
      goto out;
   stage = 13;
   if (!smoke_d24_sample_produced_depth(screen, context, color, depth, view))
      goto out;
   passed = true;

out:
   if (!passed)
      fprintf(stderr, "X8Z24 contract failed at stage %u\n", stage);
   if (single_use)
      screen->resource_destroy(screen, single_use);
   if (view)
      context->sampler_view_destroy(context, view);
   if (context)
      context->destroy(context);
   if (color)
      screen->resource_destroy(screen, color);
   if (depth)
      screen->resource_destroy(screen, depth);
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
   struct pipe_resource gl_depth_templ = depth_templ;
   struct pipe_resource bgra_templ = surface_templ;
   struct pipe_resource shared_xrgb_templ = surface_templ;
   struct chrome9_resource *surface;
   struct chrome9_resource *shared_surface;
   struct pipe_resource *shared_resource;
   struct pipe_resource *offset_source = NULL;
   struct pipe_resource *offset_resource = NULL;
   struct pipe_resource *shared_xrgb = NULL;
   struct pipe_resource *shared_xrgb_import = NULL;
   struct pipe_resource *depth_resource = NULL;
   struct winsys_handle whandle = { .type = WINSYS_HANDLE_TYPE_FD };
   struct winsys_handle kms_handle = { .type = WINSYS_HANDLE_TYPE_KMS };
   struct winsys_handle offset_handle = { .type = WINSYS_HANDLE_TYPE_FD };
   struct winsys_handle shared_xrgb_handle = {
      .type = WINSYS_HANDLE_TYPE_FD,
   };
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
   if (argc > 2 && !strcmp(argv[2], "--varying-arithmetic-compile-only")) {
      bool passed = smoke_varying_arithmetic_compile(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--position-compile-only")) {
      bool passed = smoke_position_compile(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--position-draw-only")) {
      bool passed = smoke_position_draw(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--kill-compile-only")) {
      bool passed = smoke_alpha_kill_compile(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--kill-a8-only")) {
      bool passed = smoke_a8_alpha_kill(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--a8-only")) {
      bool passed = smoke_a8_generic_texture(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--texture-only")) {
      bool passed = run_pipe_texture(screen);

      screen->destroy(screen);
      return passed ? 0 : 1;
   }
   if (getenv("CHROME9_EXPERIMENTAL_X8Z24") &&
       !smoke_d24_framebuffer_contract(screen)) {
      fputs("X8Z24 private framebuffer contract failed\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   if (argc > 2 && !strcmp(argv[2], "--d24-only")) {
      if (!getenv("CHROME9_EXPERIMENTAL_X8Z24")) {
         fputs("--d24-only requires CHROME9_EXPERIMENTAL_X8Z24\n", stderr);
         screen->destroy(screen);
         return 1;
      }
      screen->destroy(screen);
      puts("X8Z24 private framebuffer contract passed");
      return 0;
   }
   shared_xrgb_templ.bind = PIPE_BIND_RENDER_TARGET |
                             PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR |
                             PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
                             PIPE_BIND_SHARED;
   if (!screen->fence_reference || !screen->fence_finish ||
       !screen->shader_caps[MESA_SHADER_FRAGMENT].max_texture_samplers ||
       !screen->shader_caps[MESA_SHADER_FRAGMENT].max_sampler_views) {
      fprintf(stderr, "Gallium GL/DRI callback or sampler caps missing\n");
      screen->destroy(screen);
      return 1;
   }
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM) &&
       (screen->shader_caps[MESA_SHADER_FRAGMENT].max_texture_samplers < 3 ||
        screen->shader_caps[MESA_SHADER_FRAGMENT].max_sampler_views < 3 ||
        screen->shader_caps[MESA_SHADER_FRAGMENT].max_tex_instructions < 3)) {
      fputs("structured fragment three-sampler caps missing\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   {
      const struct pipe_shader_caps *vs =
         &screen->shader_caps[MESA_SHADER_VERTEX];

      if (vs->integers ||
          screen->shader_caps[MESA_SHADER_FRAGMENT].integers ||
          vs->max_tex_instructions || vs->max_tex_indirections ||
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
          screen->caps.mixed_colorbuffer_formats ||
          !screen->caps.generate_mipmap ||
          !screen->caps.mixed_color_depth_bits ||
          screen->caps.npot_textures !=
             chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE)) {
         fprintf(stderr, "bounded Gallium capability contract failed\n");
         screen->destroy(screen);
         return 1;
      }
   }
   if (argc > 2 && !strcmp(argv[2], "--caps-only")) {
      printf("fragment sampler/view/TEX caps: %u/%u/%u\n",
             screen->shader_caps[MESA_SHADER_FRAGMENT].max_texture_samplers,
             screen->shader_caps[MESA_SHADER_FRAGMENT].max_sampler_views,
             screen->shader_caps[MESA_SHADER_FRAGMENT].max_tex_instructions);
      screen->destroy(screen);
      return 0;
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
   if (!smoke_copy_rect(screen)) {
      fputs("bounded H6 SF copy rectangle failed\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   if (chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_COPY_RECT))
      puts("Gallium bounded H6 SF copy rectangle passed");

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
       surface->bo.create_flags || !surface->bo.map) {
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
          screen, NULL, resource, &kms_handle,
          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE) ||
       kms_handle.handle != surface->bo.handle ||
       kms_handle.stride != surface->stride ||
       kms_handle.offset != surface->data_offset ||
       kms_handle.modifier != DRM_FORMAT_MOD_LINEAR) {
      fputs("XRGB8888 same-fd KMS handle failed\n", stderr);
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
   {
      struct pipe_screen *foreign_screen = NULL;
      struct pipe_resource *foreign_resource = NULL;
      struct drm_gem_close close_probe = { 0 };
      int foreign_fd = open(path, O_RDWR | O_CLOEXEC);

      if (foreign_fd >= 0)
         foreign_screen = chrome9_screen_create(foreign_fd, NULL);
      if (foreign_fd >= 0)
         close(foreign_fd);
      if (foreign_screen)
         foreign_resource = foreign_screen->resource_from_handle(
            foreign_screen, &surface_templ, &whandle,
            PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (foreign_resource)
         close_probe.handle = chrome9_resource(foreign_resource)->bo.handle;
      if (foreign_resource)
         foreign_screen->resource_destroy(foreign_screen, foreign_resource);
      if (!foreign_screen || !close_probe.handle ||
          ioctl(foreign_screen ? foreign_screen->get_screen_fd(foreign_screen) :
                                 -1,
                DRM_IOCTL_GEM_CLOSE, &close_probe) == 0) {
         fputs("same-device second-DRM-fd dma-buf handle lifetime failed\n",
               stderr);
         if (foreign_screen)
            foreign_screen->destroy(foreign_screen);
         close(whandle.handle);
         if (shared_resource)
            screen->resource_destroy(screen, shared_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      foreign_screen->destroy(foreign_screen);
      puts("Gallium same-device second-DRM-fd dma-buf handle release passed");
   }
   close(whandle.handle);
   if (!shared_resource) {
      fputs("XRGB8888 PRIME import failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   shared_surface = chrome9_resource(shared_resource);
   if (!shared_surface->guarded ||
       shared_surface->stride != surface->stride ||
       shared_surface->data_offset != surface->data_offset ||
       shared_surface->bo.size != surface->bo.size) {
      fputs("canonical Chrome9 PRIME import lost guarded layout\n", stderr);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   ((uint32_t *)((uint8_t *)shared_surface->bo.map +
                 shared_surface->data_offset))[1] = 0x0055aa77;
   if (pixels[1] != 0x0055aa77) {
      fputs("XRGB8888 PRIME alias mismatch\n", stderr);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   offset_source = screen->resource_create(screen, &surface_templ);
   if (!offset_source ||
       !screen->resource_get_handle(
          screen, NULL, offset_source, &offset_handle,
          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE)) {
      fputs("noncanonical PRIME import setup failed\n", stderr);
      if (offset_source)
         screen->resource_destroy(screen, offset_source);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   offset_handle.offset = 0;
   offset_resource = screen->resource_from_handle(
      screen, &surface_templ, &offset_handle,
      PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   close(offset_handle.handle);
   if (!offset_resource || chrome9_resource(offset_resource)->guarded) {
      fputs("noncanonical offset PRIME import treated as guarded\n", stderr);
      if (offset_resource)
         screen->resource_destroy(screen, offset_resource);
      screen->resource_destroy(screen, offset_source);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   screen->resource_destroy(screen, offset_resource);
   screen->resource_destroy(screen, offset_source);
   shared_xrgb = screen->resource_create(screen, &shared_xrgb_templ);
   if (!shared_xrgb || chrome9_resource(shared_xrgb)->guarded ||
       chrome9_resource(shared_xrgb)->data_offset ||
       chrome9_resource(shared_xrgb)->bo.create_flags !=
          (chrome9_screen_has_capability(
              screen, DRM_CHROME9_CAP_GEM_DMA32_SCANOUT) ?
              DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT : 0) ||
       chrome9_resource(shared_xrgb)->bo.size !=
          ALIGN_POT(chrome9_resource(shared_xrgb)->data_size,
                    (uint64_t)page_size) ||
       !screen->resource_get_handle(
          screen, NULL, shared_xrgb, &shared_xrgb_handle,
          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE) ||
       shared_xrgb_handle.offset ||
       shared_xrgb_handle.stride != chrome9_resource(shared_xrgb)->stride ||
       shared_xrgb_handle.modifier != DRM_FORMAT_MOD_LINEAR) {
      fputs("shared XRGB8888 offset-zero layout/export failed\n", stderr);
      if (shared_xrgb_handle.handle)
         close(shared_xrgb_handle.handle);
      if (shared_xrgb)
         screen->resource_destroy(screen, shared_xrgb);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   /* Match the Xorg EGL-image consumer, which imports the shared FD with
    * render-target and sampler usage but without retaining PIPE_BIND_SHARED.
    */
   shared_xrgb_import = screen->resource_from_handle(
      screen, &surface_templ, &shared_xrgb_handle,
      PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   close(shared_xrgb_handle.handle);
   shared_xrgb_handle.handle = 0;
   if (!shared_xrgb_import ||
       chrome9_resource(shared_xrgb_import)->guarded ||
       chrome9_resource(shared_xrgb_import)->data_offset ||
       chrome9_resource(shared_xrgb_import)->bo.size !=
          chrome9_resource(shared_xrgb)->bo.size ||
       !run_pipe_triangle(screen, shared_xrgb_import, NULL)) {
      fputs("shared XRGB8888 guarded-shadow contract failed\n", stderr);
      if (shared_xrgb_import)
         screen->resource_destroy(screen, shared_xrgb_import);
      screen->resource_destroy(screen, shared_xrgb);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   screen->resource_destroy(screen, shared_xrgb_import);
   screen->resource_destroy(screen, shared_xrgb);
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

   gl_depth_templ.bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SAMPLER_VIEW;
   if (screen->is_format_supported(screen, gl_depth_templ.format,
                                   gl_depth_templ.target, 1, 1,
                                   gl_depth_templ.bind)) {
      struct pipe_resource *gl_depth_resource =
         screen->resource_create(screen, &gl_depth_templ);

      if (!gl_depth_resource) {
         fputs("GL renderbuffer Z16 allocation contract failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      screen->resource_destroy(screen, gl_depth_resource);
   } else {
      fputs("GL renderbuffer Z16 binding contract rejected\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   if (screen->shader_caps[MESA_SHADER_FRAGMENT].max_tex_instructions) {
      if (!run_pipe_texture(screen)) {
         fputs("Chrome9 Gallium texture draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_FRAGMENT_TEXTURE_LEGACY_FORMATS) &&
          !smoke_a8_generic_texture(screen)) {
         fputs("Chrome9 Gallium A8 texture draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (!run_pipe_resource_copy(screen)) {
         fputs("Chrome9 Gallium resource copy failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16) &&
          !run_pipe_textured_depth(screen,
                                   PIPE_FORMAT_B8G8R8X8_UNORM, false)) {
         fputs("Chrome9 Gallium textured D16 draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_TEXTURED_D16_BGRA8888) &&
          !run_pipe_textured_depth(screen,
                                   PIPE_FORMAT_B8G8R8A8_UNORM, false)) {
         fputs("Chrome9 Gallium BGRA textured D16 draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_TEXTURED_D16_SOURCE_OVER) &&
          !run_pipe_textured_depth(screen,
                                   PIPE_FORMAT_B8G8R8A8_UNORM, true)) {
         fputs("Chrome9 Gallium textured D16 source-over failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (!run_pipe_npot_texture(screen)) {
         fputs("Chrome9 Gallium NPOT texture draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (screen->shader_caps[MESA_SHADER_FRAGMENT].max_texture_samplers >= 2 &&
          !run_pipe_two_npot_textures(screen, false, false, false)) {
         fputs("Chrome9 Gallium two-unit NPOT texture draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16) &&
          !run_pipe_two_npot_textures(screen, true, false, false)) {
         fputs("Chrome9 Gallium two-unit textured D16 draw failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen,
             DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND) &&
          !run_pipe_two_npot_textures(screen, true, true, false)) {
         fputs("Chrome9 Gallium two-unit projected D16 blend draw failed\n",
               stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_TWO_TEXTURE_TEX0_ALPHA_PRIMARY) &&
          chrome9_screen_has_capability(
             screen, DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS) &&
          !run_pipe_two_npot_textures(screen, true, true, true)) {
         fputs("Chrome9 Gallium program24-equivalent signed-repeat draw "
               "failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
   } else {
      puts("textured triangle capability unavailable; texture smoke skipped");
   }
   bgra_templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   bgra_templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
                     PIPE_BIND_LINEAR | PIPE_BIND_DISPLAY_TARGET |
                     PIPE_BIND_SCANOUT | PIPE_BIND_SHARED;
   {
      struct pipe_resource *bgra_resource;
      struct pipe_resource *bgra_shared = NULL;
      struct chrome9_resource *bgra;
      struct chrome9_resource *shared_bgra;
      struct winsys_handle bgra_kms = { .type = WINSYS_HANDLE_TYPE_KMS };
      struct winsys_handle bgra_fd = { .type = WINSYS_HANDLE_TYPE_FD };

      if (!screen->is_format_supported(screen, bgra_templ.format,
                                       bgra_templ.target, 1, 1,
                                       bgra_templ.bind)) {
         fputs("BGRA8888 GBM allocation bindings rejected\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      bgra_resource = screen->resource_create(screen, &bgra_templ);
      if (!bgra_resource) {
         fputs("BGRA8888 GBM-style allocation failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      bgra = chrome9_resource(bgra_resource);
      if (bgra->bo.create_flags !=
             (chrome9_screen_has_capability(
                 screen, DRM_CHROME9_CAP_GEM_DMA32_SCANOUT) ?
                 DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT : 0) ||
          !screen->resource_get_handle(
             screen, NULL, bgra_resource, &bgra_kms,
             PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE) ||
          bgra_kms.handle != bgra->bo.handle ||
          bgra_kms.stride != bgra->stride ||
          bgra_kms.offset != bgra->data_offset ||
          bgra_kms.modifier != DRM_FORMAT_MOD_LINEAR) {
         fputs("BGRA8888 same-fd KMS handle failed\n", stderr);
         screen->resource_destroy(screen, bgra_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      if (!screen->resource_get_handle(
             screen, NULL, bgra_resource, &bgra_fd,
             PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE)) {
         fputs("BGRA8888 same-device dma-buf export failed\n", stderr);
         screen->resource_destroy(screen, bgra_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      bgra_shared = screen->resource_from_handle(
         screen, &bgra_templ, &bgra_fd,
         PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      close(bgra_fd.handle);
      shared_bgra = bgra_shared ? chrome9_resource(bgra_shared) : NULL;
      if (!shared_bgra || shared_bgra->guarded || bgra->guarded ||
          shared_bgra->stride != bgra->stride ||
          shared_bgra->data_offset || bgra->data_offset ||
          shared_bgra->bo.size != bgra->bo.size ||
          (chrome9_screen_has_capability(
              screen, DRM_CHROME9_CAP_BLEND_BGRA8888) &&
           !run_pipe_triangle(screen, bgra_shared, NULL))) {
         fputs("Chrome9 Gallium BGRA8888 contract failed\n", stderr);
         if (bgra_shared)
            screen->resource_destroy(screen, bgra_shared);
         screen->resource_destroy(screen, bgra_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      screen->resource_destroy(screen, bgra_shared);
      screen->resource_destroy(screen, bgra_resource);
   }
   screen->resource_destroy(screen, resource);
   screen->destroy(screen);
   puts("Mesa Chrome9 context, Gallium triangle/D16, guarded resources, and same-device dma-buf sharing smoke passed");
   return 0;
}
