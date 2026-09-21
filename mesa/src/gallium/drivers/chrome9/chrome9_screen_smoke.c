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
#include <stdlib.h>
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
   float texcoord0[2];
   float texcoord1[2];
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
                    .io_semantics.location = emit_color ?
                       VARYING_SLOT_TEX0 : VARYING_SLOT_VAR0);
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
      .io_semantics.location = modulate ?
         VARYING_SLOT_TEX0 : VARYING_SLOT_VAR0);
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
   struct pipe_blend_state alpha_only = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_depth_stencil_alpha_state alpha_dsa;
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
   void *blend_handle = NULL;
   void *source_over_handle = NULL;
   void *alpha_only_handle = NULL;
   void *dsa_handle = NULL;
   void *lequal_dsa_handle = NULL;
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
   void *generic_rgba_vs = NULL, *generic_rgba_fs = NULL;
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
   if (!depth_resource) {
      solid_buffer = screen->resource_create(screen, &solid_buffer_template);
      solid_elements_handle =
         context->create_vertex_elements_state(context, 1, elements);
      solid_vs = make_solid_vertex_shader(screen, context);
      solid_fs = make_constant_color_fs(screen, context, 0);
      unsupported_solid_fs = make_constant_color_fs(screen, context, 16);
      for (unsigned i = 0; i < ARRAY_SIZE(product_fs); i++)
         product_fs[i] = make_constant_product_fs(screen, context, i, 16);
      unsupported_product_fs =
         make_constant_product_fs(screen, context, 0, 32);
      scalar_gray_vs =
         make_scalar_generic_vertex_shader(screen, context, false);
      scalar_gray_fs = make_scalar_rgbx_fragment_shader(screen, context);
      generic_rgba_vs = make_generic_rgba_vertex_shader(screen, context);
      generic_rgba_fs = make_generic_rgba_fragment_shader(screen, context);
   }
   if (!blend_handle ||
       (!depth_resource &&
        chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND) &&
        !source_over_handle) ||
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
       !rasterizer_handle ||
       (chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_ALPHA_TEST) &&
        !alpha_dsa_handle) ||
       !elements_handle || !vs || !fs ||
       (!depth_resource &&
        (!solid_buffer || !solid_elements_handle || !solid_vs || !solid_fs ||
         !((struct chrome9_shader *)solid_fs)->supported ||
         !unsupported_solid_fs ||
         ((struct chrome9_shader *)unsupported_solid_fs)->supported ||
         !product_fs[0] || !product_fs[1] || !product_fs[2] ||
         ((struct chrome9_shader *)product_fs[0])->fragment_program !=
            CHROME9_FS_CONSTANT_MODULATE ||
         ((struct chrome9_shader *)product_fs[1])->fragment_program !=
            CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE ||
         ((struct chrome9_shader *)product_fs[2])->fragment_program !=
            CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE ||
         !unsupported_product_fs ||
         ((struct chrome9_shader *)unsupported_product_fs)->supported ||
         !scalar_gray_vs || !scalar_gray_fs ||
         ((struct chrome9_shader *)scalar_gray_fs)->fragment_program !=
            CHROME9_FS_PRIMARY_SCALAR_RGBX ||
         !generic_rgba_vs || !generic_rgba_fs ||
         ((struct chrome9_shader *)generic_rgba_fs)->fragment_program !=
            CHROME9_FS_PRIMARY_GENERIC_RGBA)))
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
      if (generic_rgba_vs)
         context->delete_vs_state(context, generic_rgba_vs);
      if (generic_rgba_fs)
         context->delete_fs_state(context, generic_rgba_fs);
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
      if (alpha_dsa_handle)
         context->delete_depth_stencil_alpha_state(context,
                                                   alpha_dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      if (source_over_handle)
         context->delete_blend_state(context, source_over_handle);
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
   struct pipe_constant_buffer texture_constant = {
      .buffer_size = sizeof(texture_constant_color),
      .user_buffer = texture_constant_color,
   };
   struct pipe_constant_buffer texture_alpha_constant = {
      .buffer_size = sizeof(texture_alpha_constant_color),
      .user_buffer = texture_alpha_constant_color,
   };
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
   void *replace = NULL, *modulate = NULL, *constant_modulate = NULL;
   void *constant_alpha_modulate = NULL, *rgbx = NULL;
   void *packed_gray_vs = NULL, *packed_gray_fs = NULL;
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
   sampler_handle = context->create_sampler_state(context, &sampler);
   u_sampler_view_default_template(&view_templ, texture, texture->format);
   mip_view = context->create_sampler_view(context, texture, &view_templ);
   if (mip_view) {
      fputs("multi-level sampler view unexpectedly accepted\n", stderr);
      pipe_sampler_view_reference(&mip_view, NULL);
      goto out;
   }
   view_templ.u.tex.first_level = 0;
   view_templ.u.tex.last_level = 0;
   view = context->create_sampler_view(context, texture, &view_templ);
   view_templ.swizzle_a = PIPE_SWIZZLE_1;
   mip_view = context->create_sampler_view(context, texture, &view_templ);
   if (mip_view) {
      fputs("BGRA constant-one-alpha sampler view unexpectedly accepted\n",
            stderr);
      pipe_sampler_view_reference(&mip_view, NULL);
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
   if (!blend_handle ||
       (chrome9_screen_has_capability(
           screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND) &&
        !source_over_handle) ||
       !dsa_handle || !rasterizer_handle ||
       !elements_handle || !replace_elements_handle || !sampler_handle ||
       !view || !rhw_view || !shadow_source || !shadow_view ||
       chrome9_resource(shadow_source)->guarded ||
       !chrome9_sampler_view(shadow_view)->shadow ||
       !chrome9_resource(
          chrome9_sampler_view(shadow_view)->shadow)->guarded ||
       !replace_vs ||
       !modulate_vs || !replace || !modulate || !constant_modulate ||
       !constant_alpha_modulate || !rgbx ||
       !packed_gray_vs || !packed_gray_fs ||
       ((struct chrome9_shader *)constant_modulate)->fragment_program !=
          CHROME9_FS_TEXTURE_CONSTANT_MODULATE ||
       ((struct chrome9_shader *)constant_alpha_modulate)->fragment_program !=
          CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE ||
       ((struct chrome9_shader *)rgbx)->fragment_program !=
          CHROME9_FS_TEXTURE_RGBX ||
       ((struct chrome9_shader *)packed_gray_fs)->fragment_program !=
          CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE) {
      fprintf(stderr,
              "texture setup failed: target=%p texture=%p context=%p "
              "blend=%p dsa=%p rasterizer=%p elements=%p "
              "replace_elements=%p sampler=%p "
              "view=%p rhw_view=%p replace_vs=%p modulate_vs=%p "
              "replace=%p modulate=%p constant_modulate=%p "
              "constant_alpha_modulate=%p rgbx=%p packed=%p/%p\n",
              (void *)target, (void *)texture, (void *)context, blend_handle,
              dsa_handle, rasterizer_handle, elements_handle,
              replace_elements_handle, sampler_handle,
              (void *)view, (void *)rhw_view, replace_vs, modulate_vs,
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
      if (view) pipe_sampler_view_reference(&view, NULL);
      if (rhw_view) pipe_sampler_view_reference(&rhw_view, NULL);
      if (shadow_view) pipe_sampler_view_reference(&shadow_view, NULL);
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
   struct pipe_depth_stencil_alpha_state dsa = {
      .depth_enabled = true,
      .depth_writemask = true,
      .depth_func = PIPE_FUNC_LEQUAL,
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
   void *blend_handle = NULL, *dsa_handle = NULL;
   void *rasterizer_handle = NULL, *elements_handle = NULL;
   void *vs = NULL, *fs = NULL, *sampler_handle = NULL;
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
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !sampler_handle || !view || !vs || !fs)
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
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (sampler_handle)
         context->delete_sampler_state(context, sampler_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (blend_handle) context->delete_blend_state(context, blend_handle);
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
run_pipe_two_npot_textures(struct pipe_screen *screen)
{
   const struct pipe_resource target_template = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   const struct pipe_resource texture_templates[2] = {
      { .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8X8_UNORM,
        .width0 = 30, .height0 = 18, .depth0 = 1, .array_size = 1,
        .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR },
      { .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8X8_UNORM,
        .width0 = 22, .height0 = 14, .depth0 = 1, .array_size = 1,
        .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR },
   };
   static const unsigned sample_xy[4][2] = {
      { 16, 16 }, { 48, 16 }, { 16, 48 }, { 48, 48 },
   };
   static const uint32_t expected[4] = {
      0x800000, 0x000080, 0x008000, 0x808080,
   };
   struct smoke_two_texture_vertex vertices[4] = {
      { { -1, -1, 0, 1 }, { 0, 0 }, { 0, 0 } },
      { { -1,  1, 0, 1 }, { 0, 1 }, { 0, 1 } },
      { {  1, -1, 0, 1 }, { 1, 0 }, { 1, 0 } },
      { {  1,  1, 0, 1 }, { 1, 1 }, { 1, 1 } },
   };
   struct pipe_resource *target = NULL, *textures[2] = { NULL, NULL };
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
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[3] = { 0 };
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
   const union pipe_color_union clear = { 0 };
   struct chrome9_resource *ctarget, *ctextures[2];
   struct chrome9_context *ccontext;
   void *blend_handle = NULL, *dsa_handle = NULL, *rasterizer_handle = NULL;
   void *elements_handle = NULL, *sampler_handles[2] = { NULL, NULL };
   void *vs = NULL, *fs = NULL;
   bool passed = false;

   target = screen->resource_create(screen, &target_template);
   for (unsigned i = 0; i < 2; i++)
      textures[i] = screen->resource_create(screen, &texture_templates[i]);
   context = screen->context_create(screen, NULL, 0);
   if (!target || !textures[0] || !textures[1] || !context)
      goto out;
   ctarget = chrome9_resource(target);
   ccontext = chrome9_context(context);
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
            y < 9 ? (x < 15 ? 0x00ff0000 : 0x000000ff) :
                    (x < 15 ? 0x0000ff00 : 0x00ffffff);
   for (unsigned y = 0; y < textures[1]->height0; y++)
      for (unsigned x = 0; x < textures[1]->width0; x++)
         source1[y * (ctextures[1]->stride / 4) + x] = 0x00808080;

   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer.half_pixel_center = true;
   rasterizer.bottom_edge_rule = true;
   rasterizer.depth_clip_near = true;
   rasterizer.depth_clip_far = true;
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_two_texture_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[1].src_offset = offsetof(struct smoke_two_texture_vertex, texcoord0);
   elements[1].src_format = PIPE_FORMAT_R32G32_FLOAT;
   elements[2].src_offset = offsetof(struct smoke_two_texture_vertex, texcoord1);
   elements[2].src_format = PIPE_FORMAT_R32G32_FLOAT;
   for (unsigned i = 0; i < 3; i++)
      elements[i].src_stride = sizeof(struct smoke_two_texture_vertex);
   elements_handle =
      context->create_vertex_elements_state(context, 3, elements);
   for (unsigned unit = 0; unit < 2; unit++) {
      sampler_handles[unit] = context->create_sampler_state(context, &sampler);
      u_sampler_view_default_template(&view_template, textures[unit],
                                      textures[unit]->format);
      views[unit] =
         context->create_sampler_view(context, textures[unit], &view_template);
   }
   vs = make_two_texture_vertex_shader(screen, context);
   fs = make_two_texture_fragment_shader(screen, context);
   if (!blend_handle || !dsa_handle || !rasterizer_handle || !elements_handle ||
       !sampler_handles[0] || !sampler_handles[1] || !views[0] || !views[1] ||
       !vs || !fs ||
       ((struct chrome9_shader *)fs)->fragment_program !=
          CHROME9_FS_TWO_TEXTURE_MODULATE)
      goto out;

   framebuffer.cbufs[0].texture = target;
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
   context->clear(context, PIPE_CLEAR_COLOR0, PIPE_MASK_RGBA, 0xff, NULL,
                  &clear, 1.0, 0);
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
   for (unsigned i = 0; i < 2; i++)
      if (textures[i]) screen->resource_destroy(screen, textures[i]);
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
   struct pipe_resource gl_depth_templ = depth_templ;
   struct pipe_resource bgra_templ = surface_templ;
   struct chrome9_resource *surface;
   struct chrome9_resource *shared_surface;
   struct pipe_resource *shared_resource;
   struct pipe_resource *offset_source = NULL;
   struct pipe_resource *offset_resource = NULL;
   struct pipe_resource *depth_resource = NULL;
   struct winsys_handle whandle = { .type = WINSYS_HANDLE_TYPE_FD };
   struct winsys_handle kms_handle = { .type = WINSYS_HANDLE_TYPE_KMS };
   struct winsys_handle offset_handle = { .type = WINSYS_HANDLE_TYPE_FD };
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
          screen->caps.mixed_colorbuffer_formats ||
          !screen->caps.mixed_color_depth_bits ||
          screen->caps.npot_textures !=
             debug_get_bool_option("CHROME9_FORCE_NPOT", false)) {
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
          !run_pipe_two_npot_textures(screen)) {
         fputs("Chrome9 Gallium two-unit NPOT texture draw failed\n", stderr);
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
      struct chrome9_resource *bgra;
      struct winsys_handle bgra_kms = { .type = WINSYS_HANDLE_TYPE_KMS };
      struct winsys_handle bgra_fd = { .type = WINSYS_HANDLE_TYPE_FD };
      bool bgra_exported;

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
      if (!screen->resource_get_handle(
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
      bgra_exported = screen->resource_get_handle(
         screen, NULL, bgra_resource, &bgra_fd,
         PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (bgra_exported)
         close(bgra_fd.handle);
      if (bgra_exported ||
          (chrome9_screen_has_capability(
              screen, DRM_CHROME9_CAP_BLEND_BGRA8888) &&
           !run_pipe_triangle(screen, bgra_resource, NULL))) {
         fputs("Chrome9 Gallium BGRA8888 contract failed\n", stderr);
         screen->resource_destroy(screen, bgra_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      screen->resource_destroy(screen, bgra_resource);
   }
   screen->resource_destroy(screen, resource);
   screen->destroy(screen);
   puts("Mesa Chrome9 context, Gallium triangle/D16, guarded resources, and same-device dma-buf sharing smoke passed");
   return 0;
}
