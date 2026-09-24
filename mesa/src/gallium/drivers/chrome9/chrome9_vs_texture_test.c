/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_vs_texture.h"

#include "chrome9_context.h"
#include "chrome9_resource.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tgsi/tgsi_exec.h"

static bool
test_linear_mips(void)
{
   uint8_t bytes[2048] = {0};
   struct chrome9_resource resource = {0};
   struct pipe_sampler_view view = {0};
   struct chrome9_vs_texture_mapping mapping;

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
   view.u.tex.first_level = 1;
   view.u.tex.last_level = 2;

   if (!chrome9_vs_texture_mapping(&view, &mapping) ||
       mapping.base != bytes + 512 || mapping.width != 9 ||
       mapping.height != 5 || mapping.first_level != 1 ||
       mapping.last_level != 2 || mapping.row_stride[1] != 32 ||
       mapping.img_stride[1] != 64 || mapping.mip_offset[1] != 0 ||
       mapping.mip_offset[2] != 256)
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

int
main(void)
{
   if (!test_linear_mips() || !test_sampler()) {
      fputs("chrome9 vertex texture mapping failed\n", stderr);
      return 1;
   }
   puts("chrome9 vertex texture mapping passed");
   return 0;
}
