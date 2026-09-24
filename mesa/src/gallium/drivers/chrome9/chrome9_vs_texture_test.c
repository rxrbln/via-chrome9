/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_vs_texture.h"

#include "chrome9_context.h"
#include "chrome9_resource.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "draw/draw_context.h"
#include "draw/draw_vs.h"
#include "nir_builder.h"
#include "pipe/p_screen.h"
#include "tgsi/tgsi_exec.h"

static void
set_identity_swizzle(struct pipe_sampler_view *view)
{
   view->swizzle_r = PIPE_SWIZZLE_X;
   view->swizzle_g = PIPE_SWIZZLE_Y;
   view->swizzle_b = PIPE_SWIZZLE_Z;
   view->swizzle_a = PIPE_SWIZZLE_W;
}

static bool
test_linear_mips(void)
{
   uint8_t bytes[2048] = {0};
   struct chrome9_resource resource = {0};
   struct pipe_sampler_view view = {0};
   struct chrome9_vs_texture_mapping mapping;
   struct chrome9_context ctx = {0};
   struct tgsi_sampler *sampler;
   const int x[4] = {0, INT_MAX, INT_MIN, 0};
   const int y[4] = {0, 0, 0, INT_MAX};
   const int zero[4] = {0};
   const int8_t offset[3] = {1, 0, 0};
   float rgba[4][4];
   int dims[4];
   bool sample_ok;

   resource.base.target = PIPE_TEXTURE_2D;
   resource.base.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   resource.base.width0 = 9;
   resource.base.height0 = 5;
   resource.base.depth0 = resource.base.array_size = 1;
   resource.base.last_level = 2;
   resource.bo.map = bytes;
   resource.bo.size = sizeof(bytes);
   resource.level_offset[0] = 128;
   resource.level_stride[0] = 64;
   resource.level_size[0] = 320;
   resource.level_offset[1] = 512;
   resource.level_stride[1] = 32;
   resource.level_size[1] = 64;
   resource.level_offset[2] = 768;
   resource.level_stride[2] = 32;
   resource.level_size[2] = 32;
   view.texture = &resource.base;
   view.target = resource.base.target;
   view.format = resource.base.format;
   set_identity_swizzle(&view);
   view.u.tex.first_level = 1;
   view.u.tex.last_level = 2;

   if (!chrome9_vs_texture_mapping(&view, &mapping) ||
       mapping.base != bytes + 512 || mapping.width != 9 ||
       mapping.height != 5 || mapping.first_level != 1 ||
       mapping.last_level != 2 || mapping.row_stride[1] != 32 ||
       mapping.img_stride[1] != 64 || mapping.mip_offset[1] != 0 ||
       mapping.mip_offset[2] != 256)
      return false;

   /* TXQ/TXF LOD is relative to the view.  A positive offset applied to
    * INT_MAX must be range-checked without overflowing signed int. */
   memcpy(bytes + 516, (const uint8_t[]){0, 0, 255, 255}, 4);
   ctx.vs_sampler_views[0] = &view;
   ctx.vs_texture_mappings[0] = mapping;
   sampler = chrome9_vs_texture_sampler_create(&ctx);
   if (!sampler)
      return false;
   sampler->get_dims(sampler, 0, 0, dims);
   sample_ok = dims[0] == 4 && dims[1] == 2 &&
      dims[2] == 1 && dims[3] == 2;
   sampler->get_texel(sampler, 0, x, y, zero, zero, offset, rgba);
   sample_ok &= fabsf(rgba[0][0] - 1.0f) < 0.0001f &&
      fabsf(rgba[1][0]) < 0.0001f && fabsf(rgba[2][0]) < 0.0001f &&
      fabsf(rgba[3][0] - 1.0f) < 0.0001f;
   for (unsigned lane = 1; lane < 4; lane++)
      for (unsigned c = 0; c < 4; c++)
         sample_ok &= rgba[c][lane] == 0.0f;
   {
      struct pipe_sampler_state state = {0};
      const float s[4] = {0.375f, 0.375f, 0.375f, 0.375f};
      const float t[4] = {0.25f, 0.25f, 0.25f, 0.25f};
      const float lod[4] = {0};
      float derivs[3][2][4] = {{{0}}};

      state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
      state.min_mip_filter = PIPE_TEX_MIPFILTER_NEAREST;
      state.wrap_s = state.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      state.max_lod = 1.0f;
      ctx.vs_sampler_states[0] = &state;
      for (unsigned lane = 0; lane < 4; lane++)
         derivs[0][0][lane] = 0.25f;
      /* The gradient spans one texel of view level zero (absolute L1).
       * Scaling against the resource's L0 would incorrectly choose L2. */
      sampler->get_samples(sampler, 0, 0, s, t, lod, lod, lod, derivs,
                           (const int8_t[3]){0},
                           TGSI_SAMPLER_DERIVS_EXPLICIT, rgba);
      sample_ok &= fabsf(rgba[0][0] - 1.0f) < 0.0001f &&
                   fabsf(rgba[1][0]) < 0.0001f &&
                   fabsf(rgba[2][0]) < 0.0001f;
   }
   chrome9_vs_texture_sampler_destroy(sampler);
   if (!sample_ok)
      return false;

   resource.level_size[2] = 16;
   if (chrome9_vs_texture_mapping(&view, &mapping))
      return false;
   resource.level_size[2] = 32;
   resource.level_offset[2] = sizeof(bytes) - 16;
   if (chrome9_vs_texture_mapping(&view, &mapping))
      return false;
   resource.level_offset[2] = 768;
   view.u.tex.last_level = 3;
   if (chrome9_vs_texture_mapping(&view, &mapping))
      return false;
   view.u.tex.last_level = 2;
   view.u.tex.first_layer = 1;
   if (chrome9_vs_texture_mapping(&view, &mapping))
      return false;
   view.u.tex.first_layer = 0;
   resource.bo.map = NULL;
   return !chrome9_vs_texture_mapping(&view, &mapping);
}

static bool
test_sampler(void)
{
   uint8_t image3[160] = {0}, image5[160] = {0};
   struct chrome9_resource resource3 = {0}, resource5 = {0};
   struct pipe_sampler_view view3 = {0}, view5 = {0};
   struct pipe_sampler_state state = {0};
   struct chrome9_context ctx = {0};
   struct tgsi_sampler *sampler;
   const float s[4] = {0.25f, 0.75f, 0.5f, 0.25f};
   const float t[4] = {0.25f, 0.25f, 0.5f, 0.75f};
   const float zero[4] = {0};
   const int8_t offsets[3] = {0};
   float derivs[3][2][4] = {{{0}}};
   float rgba[4][4];
   int dims[4];
   bool success;

   resource3.base.target = resource5.base.target = PIPE_TEXTURE_2D;
   resource3.base.format = resource5.base.format =
      PIPE_FORMAT_B8G8R8A8_UNORM;
   resource3.base.width0 = resource5.base.width0 = 2;
   resource3.base.height0 = resource5.base.height0 = 2;
   resource3.base.depth0 = resource5.base.depth0 = 1;
   resource3.base.array_size = resource5.base.array_size = 1;
   resource3.bo.map = image3;
   resource5.bo.map = image5;
   resource3.bo.size = sizeof(image3);
   resource5.bo.size = sizeof(image5);
   resource3.level_offset[0] = resource5.level_offset[0] = 64;
   resource3.level_stride[0] = resource5.level_stride[0] = 32;
   resource3.level_size[0] = resource5.level_size[0] = 64;
   /* BGRA: red, green, blue, white at the four texel centers. */
   memcpy(image3 + 64, (const uint8_t[]){0, 0, 255, 255,
                                         0, 255, 0, 255}, 8);
   memcpy(image3 + 96, (const uint8_t[]){255, 0, 0, 255,
                                         255, 255, 255, 255}, 8);
   memcpy(image5 + 64, (const uint8_t[]){153, 102, 51, 255}, 4);
   view3.texture = &resource3.base;
   view5.texture = &resource5.base;
   view3.target = view5.target = PIPE_TEXTURE_2D;
   view3.format = view5.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   set_identity_swizzle(&view3);
   set_identity_swizzle(&view5);
   ctx.vs_sampler_views[3] = &view3;
   ctx.vs_sampler_views[5] = &view5;
   if (!chrome9_vs_texture_mapping(&view3, &ctx.vs_texture_mappings[3]) ||
       !chrome9_vs_texture_mapping(&view5, &ctx.vs_texture_mappings[5]))
      return false;
   state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   state.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   state.wrap_s = state.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   ctx.vs_sampler_states[3] = ctx.vs_sampler_states[5] = &state;
   sampler = chrome9_vs_texture_sampler_create(&ctx);
   if (!sampler)
      return false;

   sampler->get_samples(sampler, 3, 3, s, t, zero, zero, zero,
                        derivs, offsets, TGSI_SAMPLER_LOD_EXPLICIT, rgba);
   success = fabsf(rgba[0][0] - 1.0f) < 0.0001f &&
             fabsf(rgba[1][0]) < 0.0001f &&
             fabsf(rgba[1][1] - 1.0f) < 0.0001f &&
             fabsf(rgba[2][3] - 1.0f) < 0.0001f &&
             fabsf(rgba[0][2] - 0.5f) < 0.0001f &&
             fabsf(rgba[1][2] - 0.5f) < 0.0001f &&
             fabsf(rgba[2][2] - 0.5f) < 0.0001f;
   if (!success)
      fprintf(stderr, "initial sampler rgba0=%g,%g,%g,%g center=%g,%g,%g,%g\n",
              rgba[0][0], rgba[1][0], rgba[2][0], rgba[3][0],
              rgba[0][2], rgba[1][2], rgba[2][2], rgba[3][2]);
   sampler->get_samples(sampler, 5, 5, s, t, zero, zero, zero,
                        derivs, offsets, TGSI_SAMPLER_LOD_EXPLICIT, rgba);
   success &= fabsf(rgba[0][0] - 0.2f) < 0.0001f &&
              fabsf(rgba[1][0] - 0.4f) < 0.0001f &&
              fabsf(rgba[2][0] - 0.6f) < 0.0001f;
   sampler->get_dims(sampler, 5, 0, dims);
   success &= dims[0] == 2 && dims[1] == 2 && dims[2] == 1;
   /* Failed maps and unbound views/states must not reuse the prior texel. */
   memset(&ctx.vs_texture_mappings[3], 0,
          sizeof(ctx.vs_texture_mappings[3]));
   sampler->get_samples(sampler, 3, 3, s, t, zero, zero, zero,
                        derivs, offsets, TGSI_SAMPLER_LOD_EXPLICIT, rgba);
   success &= rgba[0][0] == 0.0f && rgba[1][0] == 0.0f;
   ctx.vs_texture_mappings[3] = ctx.vs_texture_mappings[5];
   ctx.vs_sampler_views[3] = NULL;
   sampler->get_samples(sampler, 3, 3, s, t, zero, zero, zero,
                        derivs, offsets, TGSI_SAMPLER_LOD_EXPLICIT, rgba);
   success &= rgba[0][0] == 0.0f && rgba[1][0] == 0.0f;
   ctx.vs_sampler_states[5] = NULL;
   sampler->get_samples(sampler, 5, 5, s, t, zero, zero, zero,
                        derivs, offsets, TGSI_SAMPLER_LOD_EXPLICIT, rgba);
   success &= rgba[0][0] == 0.0f && rgba[1][0] == 0.0f;
   chrome9_vs_texture_sampler_destroy(sampler);
   return success;
}

static bool
check_sample(struct tgsi_sampler *sampler, const char *name, float s0,
             float t0, float lod0, enum tgsi_sampler_control control,
             const float expected[4])
{
   const float s[4] = {s0, s0, s0, s0};
   const float t[4] = {t0, t0, t0, t0};
   const float lod[4] = {lod0, lod0, lod0, lod0};
   const float zero[4] = {0};
   const int8_t offset[3] = {0};
   float derivs[3][2][4] = {{{0}}};
   float rgba[4][4];

   sampler->get_samples(sampler, 0, 0, s, t, zero, zero, lod,
                        derivs, offset, control, rgba);
   for (unsigned lane = 0; lane < 4; lane++) {
      for (unsigned c = 0; c < 4; c++) {
         if (fabsf(rgba[c][lane] - expected[c]) > 0.0001f) {
            fprintf(stderr, "%s: lane %u channel %u got %g expected %g\n",
                    name, lane, c, rgba[c][lane], expected[c]);
            return false;
         }
      }
   }
   return true;
}

static bool
test_sampler_states(void)
{
   uint8_t image[256] = {0};
   struct chrome9_resource resource = {0};
   struct pipe_sampler_view view = {0};
   struct pipe_sampler_state state = {0};
   struct chrome9_context ctx = {0};
   struct tgsi_sampler *sampler;
   bool success = true;

   resource.base.target = PIPE_TEXTURE_2D;
   resource.base.format = PIPE_FORMAT_B8G8R8X8_UNORM;
   resource.base.width0 = resource.base.height0 = 2;
   resource.base.depth0 = resource.base.array_size = 1;
   resource.base.last_level = 1;
   resource.bo.map = image;
   resource.bo.size = sizeof(image);
   resource.level_offset[0] = 64;
   resource.level_stride[0] = 32;
   resource.level_size[0] = 64;
   resource.level_offset[1] = 128;
   resource.level_stride[1] = 32;
   resource.level_size[1] = 32;
   /* Base 2x2 is red, green, blue, white; mip 1 is blue.  Deliberately
    * non-one X bytes distinguish BGRX alpha semantics from raw BGRA. */
   memcpy(image + 64, (const uint8_t[]){0, 0, 255, 64,
                                        0, 255, 0, 128}, 8);
   memcpy(image + 96, (const uint8_t[]){255, 0, 0, 192,
                                        255, 255, 255, 0}, 8);
   memcpy(image + 128, (const uint8_t[]){255, 0, 0, 255}, 4);
   view.texture = &resource.base;
   view.target = PIPE_TEXTURE_2D;
   view.format = resource.base.format;
   view.u.tex.last_level = 1;
   set_identity_swizzle(&view);
   ctx.vs_sampler_views[0] = &view;
   ctx.vs_sampler_states[0] = &state;
   if (!chrome9_vs_texture_mapping(&view, &ctx.vs_texture_mappings[0]))
      return false;
   sampler = chrome9_vs_texture_sampler_create(&ctx);
   if (!sampler)
      return false;

   state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   state.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   state.wrap_s = state.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   state.max_lod = 1.0f;
   success &= check_sample(sampler, "BGRX alpha one", 0.25f, 0.25f,
                           0.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){1, 0, 0, 1});
   view.swizzle_a = PIPE_SWIZZLE_1;
   success &= check_sample(sampler, "BGRX alpha-one view swizzle",
                           0.25f, 0.25f, 0.0f,
                           TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){1, 0, 0, 1});
   view.swizzle_a = PIPE_SWIZZLE_W;
   resource.base.format = view.format = PIPE_FORMAT_B8G8R8A8_UNORM;

   success &= check_sample(sampler, "nearest clamp", -0.25f, 0.25f,
                           0.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){1, 0, 0, 64.0f / 255.0f});
   state.wrap_s = PIPE_TEX_WRAP_REPEAT;
   success &= check_sample(sampler, "nearest repeat", -0.25f, 0.25f,
                           0.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){0, 1, 0, 128.0f / 255.0f});
   state.wrap_s = PIPE_TEX_WRAP_MIRROR_REPEAT;
   success &= check_sample(sampler, "nearest mirror repeat", -0.25f,
                           0.25f, 0.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){1, 0, 0, 64.0f / 255.0f});
   state.wrap_s = PIPE_TEX_WRAP_REPEAT;
   state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   success &= check_sample(sampler, "linear repeat seam", 0.0f, 0.25f,
                           0.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){0.5f, 0.5f, 0,
                                           96.0f / 255.0f});
   state.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   state.min_mip_filter = PIPE_TEX_MIPFILTER_NEAREST;
   success &= check_sample(sampler, "nearest mip level one", 0.25f,
                           0.25f, 1.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){0, 0, 1, 1});
   success &= check_sample(sampler, "nearest mip half rounds up", 0.25f,
                           0.25f, 0.5f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){0, 0, 1, 1});
   state.min_mip_filter = PIPE_TEX_MIPFILTER_LINEAR;
   success &= check_sample(sampler, "linear mip quarter", 0.25f, 0.25f,
                           0.25f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){0.75f, 0, 0.25f,
                                           (0.75f * 64.0f + 0.25f * 255.0f) /
                                           255.0f});
   state.lod_bias = 0.25f;
   success &= check_sample(sampler, "linear mip LOD bias", 0.25f, 0.25f,
                           0.0f, TGSI_SAMPLER_LOD_ZERO,
                           (const float[]){0.75f, 0, 0.25f,
                                           (0.75f * 64.0f + 0.25f * 255.0f) /
                                           255.0f});
   state.lod_bias = 0.0f;
   state.max_lod = 0.25f;
   success &= check_sample(sampler, "linear mip max LOD", 0.25f, 0.25f,
                           1.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){0.75f, 0, 0.25f,
                                           (0.75f * 64.0f + 0.25f * 255.0f) /
                                           255.0f});
   chrome9_vs_texture_sampler_destroy(sampler);
   return success;
}

static bool
test_sampler_formats(void)
{
   static const struct {
      enum pipe_format format;
      uint8_t pixel[4];
      float expected[4];
   } cases[] = {
      {PIPE_FORMAT_A8_UNORM, {128}, {0, 0, 0, 128.0f / 255.0f}},
      {PIPE_FORMAT_L8_UNORM, {128},
       {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1}},
      {PIPE_FORMAT_L8A8_UNORM, {128, 64},
       {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f,
        64.0f / 255.0f}},
      {PIPE_FORMAT_B5G6R5_UNORM, {0, 248}, {1, 0, 0, 1}},
      {PIPE_FORMAT_X8Z24_UNORM, {0, 0, 0, 128},
       {8388608.0f / 16777215.0f, 0, 0, 1}},
   };
   uint8_t image[96] = {0};
   struct chrome9_resource resource = {0};
   struct pipe_sampler_view view = {0};
   struct pipe_sampler_state state = {0};
   struct chrome9_context ctx = {0};
   struct tgsi_sampler *sampler;
   bool success = true;

   resource.base.target = PIPE_TEXTURE_2D;
   resource.base.width0 = resource.base.height0 = 1;
   resource.base.depth0 = resource.base.array_size = 1;
   resource.bo.map = image;
   resource.bo.size = sizeof(image);
   resource.level_offset[0] = 32;
   resource.level_stride[0] = 32;
   resource.level_size[0] = 32;
   view.texture = &resource.base;
   view.target = PIPE_TEXTURE_2D;
   set_identity_swizzle(&view);
   state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   state.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   state.wrap_s = state.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   ctx.vs_sampler_views[0] = &view;
   ctx.vs_sampler_states[0] = &state;
   sampler = chrome9_vs_texture_sampler_create(&ctx);
   if (!sampler)
      return false;
   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      resource.base.format = view.format = cases[i].format;
      memcpy(image + 32, cases[i].pixel, sizeof(cases[i].pixel));
      if (!chrome9_vs_texture_mapping(&view, &ctx.vs_texture_mappings[0])) {
         success = false;
         break;
      }
      success &= check_sample(sampler, util_format_name(cases[i].format),
                              0.5f, 0.5f, 0.0f,
                              TGSI_SAMPLER_LOD_EXPLICIT, cases[i].expected);
   }
   view.swizzle_r = PIPE_SWIZZLE_1;
   view.swizzle_g = PIPE_SWIZZLE_X;
   view.swizzle_b = PIPE_SWIZZLE_0;
   view.swizzle_a = PIPE_SWIZZLE_Y;
   success &= check_sample(sampler, "depth view swizzle", 0.5f, 0.5f,
                           0.0f, TGSI_SAMPLER_LOD_EXPLICIT,
                           (const float[]){1, 8388608.0f / 16777215.0f,
                                           0, 0});
   chrome9_vs_texture_sampler_destroy(sampler);
   return success;
}

static nir_shader *
make_two_sample_vertex_shader(bool explicit_lod, bool sparse_slots)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, &options, "chrome9-two-vertex-textures");
   nir_def *coord = nir_load_input(
      &b, 2, 32, nir_imm_int(&b, 0),
      .io_semantics.location = VERT_ATTRIB_TEX0);
   nir_def *sample[2];
   const unsigned slots[2] = {
      sparse_slots ? 3 : 0, sparse_slots ? 5 : 1,
   };

   b.shader->info.io_lowered = true;
   for (unsigned i = 0; i < 2; i++) {
      nir_tex_instr *tex = nir_tex_instr_create(b.shader,
                                                explicit_lod ? 2 : 1);
      nir_variable *uniform = nir_variable_create(
         b.shader, nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false,
                           GLSL_TYPE_FLOAT), "vertex_texture");

      uniform->data.binding = slots[i];

      tex->op = explicit_lod ? nir_texop_txl : nir_texop_tex;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->coord_components = 2;
      tex->dest_type = nir_type_float32;
      tex->texture_index = slots[i];
      tex->sampler_index = slots[i];
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
      if (explicit_lod)
         tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod,
                                           nir_imm_float(&b, 0.0f));
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      sample[i] = &tex->def;
   }
   nir_store_output(&b,
      nir_vec4(&b, nir_channel(&b, sample[0], 0),
               nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f),
               nir_imm_float(&b, 1.0f)),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = VARYING_SLOT_POS);
   nir_store_output(&b,
      nir_vec4(&b, nir_imm_float(&b, 0.0f),
               nir_channel(&b, sample[1], 1), nir_imm_float(&b, 0.0f),
               nir_imm_float(&b, 1.0f)),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
test_draw_vertex_samples(bool explicit_lod, bool sparse_slots)
{
   uint8_t image3[160] = {0}, image5[160] = {0};
   struct chrome9_resource resource3 = {0}, resource5 = {0};
   struct pipe_sampler_view view3 = {0}, view5 = {0};
   struct pipe_sampler_state state = {0};
   struct pipe_rasterizer_state rasterizer = {0};
   struct pipe_screen screen = {0};
   struct chrome9_context ctx = {0};
   struct draw_vertex_shader *shader = NULL;
   nir_shader *nir = NULL;
   const float input[4][1][4] = {
      {{0.25f, 0.25f, 0.0f, 1.0f}},
      {{0.75f, 0.25f, 0.0f, 1.0f}},
      {{0.50f, 0.50f, 0.0f, 1.0f}},
      {{0.25f, 0.75f, 0.0f, 1.0f}},
   };
   const float expected_position_x[4] = {1.0f, 0.0f, 0.5f, 0.0f};
   const float expected_varying_y[4] = {0.2f, 0.4f, 0.5f, 0.6f};
   struct draw_buffer_info constants[PIPE_MAX_CONSTANT_BUFFERS] = {0};
   float output[4][2][4] = {{{0}}};
   int position_slot = -1, varying_slot = -1;
   const unsigned slot3 = sparse_slots ? 3 : 0;
   const unsigned slot5 = sparse_slots ? 5 : 1;
   bool success = false;

   resource3.base.target = resource5.base.target = PIPE_TEXTURE_2D;
   resource3.base.format = resource5.base.format =
      PIPE_FORMAT_B8G8R8A8_UNORM;
   resource3.base.width0 = resource5.base.width0 = 2;
   resource3.base.height0 = resource5.base.height0 = 2;
   resource3.base.depth0 = resource5.base.depth0 = 1;
   resource3.base.array_size = resource5.base.array_size = 1;
   resource3.bo.map = image3;
   resource5.bo.map = image5;
   resource3.bo.size = sizeof(image3);
   resource5.bo.size = sizeof(image5);
   resource3.level_offset[0] = resource5.level_offset[0] = 64;
   resource3.level_stride[0] = resource5.level_stride[0] = 32;
   resource3.level_size[0] = resource5.level_size[0] = 64;
   memcpy(image3 + 64, (const uint8_t[]){0, 0, 255, 255,
                                         0, 255, 0, 255}, 8);
   memcpy(image3 + 96, (const uint8_t[]){255, 0, 0, 255,
                                         255, 255, 255, 255}, 8);
   memcpy(image5 + 64, (const uint8_t[]){0, 51, 0, 255,
                                         0, 102, 0, 255}, 8);
   memcpy(image5 + 96, (const uint8_t[]){0, 153, 0, 255,
                                         0, 204, 0, 255}, 8);
   view3.texture = &resource3.base;
   view5.texture = &resource5.base;
   view3.target = view5.target = PIPE_TEXTURE_2D;
   view3.format = view5.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   set_identity_swizzle(&view3);
   set_identity_swizzle(&view5);
   ctx.vs_sampler_views[slot3] = &view3;
   ctx.vs_sampler_views[slot5] = &view5;
   if (!chrome9_vs_texture_mapping(&view3, &ctx.vs_texture_mappings[slot3]) ||
       !chrome9_vs_texture_mapping(&view5, &ctx.vs_texture_mappings[slot5])) {
      fputs("draw vertex texture mapping failed\n", stderr);
      return false;
   }
   state.min_img_filter = state.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   state.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   state.wrap_s = state.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   ctx.vs_sampler_states[slot3] = ctx.vs_sampler_states[slot5] = &state;
   ctx.base.screen = &screen;
   ctx.draw = draw_create_no_llvm(&ctx.base);
   if (!ctx.draw) {
      fputs("draw context creation failed\n", stderr);
      return false;
   }
   ctx.vs_tgsi_sampler = chrome9_vs_texture_sampler_create(&ctx);
   if (!ctx.vs_tgsi_sampler) {
      fputs("draw sampler creation failed\n", stderr);
      goto out;
   }
   draw_texture_sampler(ctx.draw, MESA_SHADER_VERTEX, ctx.vs_tgsi_sampler);
   draw_set_rasterizer_state(ctx.draw, &rasterizer, &rasterizer);
   nir = make_two_sample_vertex_shader(explicit_lod, sparse_slots);
   if (!nir)
      goto out;
   struct pipe_shader_state shader_state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = nir,
   };

   shader = draw_create_vertex_shader(ctx.draw, &shader_state);
   /* nir_to_tgsi() consumes the NIR shader even when compilation fails. */
   nir = NULL;
   if (!shader || shader->info.num_inputs != 1 ||
       shader->info.num_outputs != 2) {
      fprintf(stderr, "vertex shader setup failed: explicit=%u sparse=%u "
              "shader=%p inputs=%u outputs=%u\n", explicit_lod,
              sparse_slots, (void *)shader,
              shader ? shader->info.num_inputs : 0,
              shader ? shader->info.num_outputs : 0);
      goto out;
   }
   for (unsigned slot = 0; slot < shader->info.num_outputs; slot++) {
      if (shader->info.output_semantic_name[slot] == TGSI_SEMANTIC_POSITION)
         position_slot = slot;
      if (shader->info.output_semantic_name[slot] == TGSI_SEMANTIC_GENERIC)
         varying_slot = slot;
   }
   if (position_slot < 0 || varying_slot < 0) {
      fprintf(stderr, "vertex semantics missing: position=%d varying=%d\n",
              position_slot, varying_slot);
      goto out;
   }
   shader->prepare(shader, ctx.draw);
   shader->run_linear(ctx.draw, shader, input[0], output[0], constants, 4,
                      sizeof(input[0]), sizeof(output[0]), NULL);
   success = true;
   for (unsigned vertex = 0; vertex < 4; vertex++) {
      const float *position = output[vertex][position_slot];
      const float *varying = output[vertex][varying_slot];

      success &= fabsf(position[0] - expected_position_x[vertex]) < 0.0001f &&
                 position[1] == 0.0f && position[2] == 0.0f &&
                 position[3] == 1.0f && varying[0] == 0.0f &&
                 fabsf(varying[1] - expected_varying_y[vertex]) < 0.0001f &&
                 varying[2] == 0.0f && varying[3] == 1.0f;
      if (!success)
         fprintf(stderr, "vertex %u explicit=%u sparse=%u "
                 "output pos=(%g,%g,%g,%g) varying=(%g,%g,%g,%g)\n",
                 vertex, explicit_lod, sparse_slots,
                 position[0], position[1], position[2], position[3],
                 varying[0], varying[1], varying[2], varying[3]);
   }
out:
   if (shader)
      draw_delete_vertex_shader(ctx.draw, shader);
   if (nir)
      ralloc_free(nir);
   draw_destroy(ctx.draw);
   chrome9_vs_texture_sampler_destroy(ctx.vs_tgsi_sampler);
   return success;
}

int
main(void)
{
   if (!test_linear_mips()) {
      fputs("vertex texture mip mapping failed\n", stderr);
      return 1;
   }
   if (!test_sampler()) {
      fputs("vertex texture callback failed\n", stderr);
      return 1;
   }
   if (!test_sampler_states()) {
      fputs("vertex texture state callback failed\n", stderr);
      return 1;
   }
   if (!test_sampler_formats()) {
      fputs("vertex texture format callback failed\n", stderr);
      return 1;
   }
   /* Raw NIR can name high slots; linked GL uses dense shader slots even
    * when its sampler uniforms point at higher application texture units.
    */
   if (!test_draw_vertex_samples(false, true) ||
       !test_draw_vertex_samples(true, true) ||
       !test_draw_vertex_samples(false, false) ||
       !test_draw_vertex_samples(true, false)) {
      fputs("chrome9 vertex texture mapping failed\n", stderr);
      return 1;
   }
   puts("chrome9 vertex texture mapping passed");
   return 0;
}
