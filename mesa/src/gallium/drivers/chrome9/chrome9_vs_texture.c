/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_vs_texture.h"

#include "chrome9_context.h"
#include "chrome9_resource.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "tgsi/tgsi_exec.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"

struct chrome9_vs_sampler {
   struct tgsi_sampler base;
   struct chrome9_context *ctx;
};

bool
chrome9_vs_texture_mapping(const struct pipe_sampler_view *view,
                           struct chrome9_vs_texture_mapping *mapping)
{
   const struct pipe_resource *texture = view ? view->texture : NULL;
   const struct chrome9_resource *resource;
   uint64_t first_offset;
   unsigned cpp, first, last;

   if (!mapping)
      return false;
   memset(mapping, 0, sizeof(*mapping));
   if (!texture || texture->target != PIPE_TEXTURE_2D ||
       view->target != PIPE_TEXTURE_2D || view->format != texture->format ||
       texture->depth0 != 1 || texture->array_size != 1 ||
       view->u.tex.first_layer || view->u.tex.last_layer)
      return false;

   first = view->u.tex.first_level;
   last = view->u.tex.last_level;
   if (first > last || last > texture->last_level ||
       last >= PIPE_MAX_TEXTURE_LEVELS ||
       util_format_get_blockwidth(view->format) != 1 ||
       util_format_get_blockheight(view->format) != 1)
      return false;
   cpp = util_format_get_blocksize(view->format);
   resource = chrome9_resource_const(texture);
   if (!cpp || !resource->bo.map)
      return false;
   first_offset = resource->level_offset[first];
   if (first_offset > resource->bo.size || first_offset > SIZE_MAX)
      return false;

   for (unsigned level = first; level <= last; level++) {
      uint64_t offset = resource->level_offset[level];
      uint64_t size = resource->level_size[level];
      uint32_t width = u_minify(texture->width0, level);
      uint32_t height = u_minify(texture->height0, level);
      uint32_t stride = resource->level_stride[level];

      if (!width || !height || width > UINT32_MAX / cpp ||
          stride < width * cpp || offset < first_offset ||
          offset > resource->bo.size || size > resource->bo.size - offset ||
          (uint64_t)stride * height > size ||
          (uint64_t)stride * height > UINT32_MAX ||
          offset - first_offset > UINT32_MAX)
         return false;
      mapping->row_stride[level] = stride;
      mapping->img_stride[level] = stride * height;
      mapping->mip_offset[level] = offset - first_offset;
   }

   mapping->base = (const uint8_t *)resource->bo.map + first_offset;
   mapping->width = texture->width0;
   mapping->height = texture->height0;
   mapping->first_level = first;
   mapping->last_level = last;
   return true;
}

static void
chrome9_vs_fetch_bgra(const struct chrome9_vs_texture_mapping *mapping,
                      unsigned level, unsigned x, unsigned y, float rgba[4])
{
   const uint8_t *pixel = (const uint8_t *)mapping->base +
      mapping->mip_offset[level] + (size_t)y * mapping->row_stride[level] +
      (size_t)x * 4;

   rgba[0] = pixel[2] * (1.0f / 255.0f);
   rgba[1] = pixel[1] * (1.0f / 255.0f);
   rgba[2] = pixel[0] * (1.0f / 255.0f);
   rgba[3] = pixel[3] * (1.0f / 255.0f);
}

/* A bounded first software sampler contract: terrain's normalized 2D BGRA,
 * explicit LOD zero, linear/clamp-to-edge state.  Other states return zero;
 * public VS texture caps remain zero until their envelope is implemented. */
static void
chrome9_vs_get_samples(struct tgsi_sampler *sampler,
                       unsigned sview_index, unsigned sampler_index,
                       const float s[TGSI_QUAD_SIZE],
                       const float t[TGSI_QUAD_SIZE],
                       const float r[TGSI_QUAD_SIZE],
                       const float c0[TGSI_QUAD_SIZE],
                       const float lod[TGSI_QUAD_SIZE],
                       float derivs[3][2][TGSI_QUAD_SIZE],
                       const int8_t offset[3],
                       enum tgsi_sampler_control control,
                       float rgba[TGSI_NUM_CHANNELS][TGSI_QUAD_SIZE])
{
   struct chrome9_context *ctx = ((struct chrome9_vs_sampler *)sampler)->ctx;
   const struct pipe_sampler_view *view = sview_index < CHROME9_VS_MAX_SAMPLERS ?
      ctx->vs_sampler_views[sview_index] : NULL;
   const struct pipe_sampler_state *state =
      sampler_index < CHROME9_VS_MAX_SAMPLERS ?
      ctx->vs_sampler_states[sampler_index] : NULL;
   const struct chrome9_vs_texture_mapping *mapping =
      view ? &ctx->vs_texture_mappings[sview_index] : NULL;
   bool valid = view && state && mapping->base &&
      view->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
      mapping->first_level == 0 &&
      state->min_img_filter == PIPE_TEX_FILTER_LINEAR &&
      state->mag_img_filter == PIPE_TEX_FILTER_LINEAR &&
      state->min_mip_filter == PIPE_TEX_MIPFILTER_NONE &&
      state->wrap_s == PIPE_TEX_WRAP_CLAMP_TO_EDGE &&
      state->wrap_t == PIPE_TEX_WRAP_CLAMP_TO_EDGE &&
      state->compare_mode == PIPE_TEX_COMPARE_NONE &&
      !state->unnormalized_coords &&
      (control == TGSI_SAMPLER_LOD_EXPLICIT ||
       control == TGSI_SAMPLER_LOD_ZERO) &&
      !offset[0] && !offset[1] && !offset[2];

   (void)r;
   (void)c0;
   (void)derivs;
   for (unsigned lane = 0; lane < TGSI_QUAD_SIZE; lane++) {
      float result[4] = {0};

      if (valid && isfinite(s[lane]) && isfinite(t[lane]) &&
          (control == TGSI_SAMPLER_LOD_ZERO ||
           (isfinite(lod[lane]) && lod[lane] == 0.0f))) {
         const unsigned width = mapping->width, height = mapping->height;
         float x = CLAMP(s[lane], 0.0f, 1.0f) * width - 0.5f;
         float y = CLAMP(t[lane], 0.0f, 1.0f) * height - 0.5f;
         int x0 = floorf(x), y0 = floorf(y);
         float fx = x - x0, fy = y - y0;
         unsigned xs[2] = {CLAMP(x0, 0, (int)width - 1),
                           CLAMP(x0 + 1, 0, (int)width - 1)};
         unsigned ys[2] = {CLAMP(y0, 0, (int)height - 1),
                           CLAMP(y0 + 1, 0, (int)height - 1)};
         float texels[2][2][4];

         for (unsigned j = 0; j < 2; j++)
            for (unsigned i = 0; i < 2; i++)
               chrome9_vs_fetch_bgra(mapping, 0, xs[i], ys[j],
                                     texels[j][i]);
         for (unsigned c = 0; c < 4; c++) {
            float top = texels[0][0][c] * (1.0f - fx) +
                        texels[0][1][c] * fx;
            float bottom = texels[1][0][c] * (1.0f - fx) +
                           texels[1][1][c] * fx;
            result[c] = top * (1.0f - fy) + bottom * fy;
         }
         if (view->swizzle_a == PIPE_SWIZZLE_1)
            result[3] = 1.0f;
      }
      for (unsigned c = 0; c < 4; c++)
         rgba[c][lane] = result[c];
   }
}

static void
chrome9_vs_get_dims(struct tgsi_sampler *sampler,
                    unsigned sview_index, int level, int dims[4])
{
   struct chrome9_context *ctx = ((struct chrome9_vs_sampler *)sampler)->ctx;
   const struct chrome9_vs_texture_mapping *mapping =
      sview_index < CHROME9_VS_MAX_SAMPLERS ?
      &ctx->vs_texture_mappings[sview_index] : NULL;

   memset(dims, 0, 4 * sizeof(*dims));
   if (!mapping || !mapping->base || level < 0 ||
       (unsigned)level > mapping->last_level)
      return;
   dims[0] = u_minify(mapping->width, level);
   dims[1] = u_minify(mapping->height, level);
   dims[2] = 1;
}

static void
chrome9_vs_get_texel(struct tgsi_sampler *sampler,
                     unsigned sview_index,
                     const int i[TGSI_QUAD_SIZE],
                     const int j[TGSI_QUAD_SIZE],
                     const int k[TGSI_QUAD_SIZE],
                     const int lod[TGSI_QUAD_SIZE],
                     const int8_t offset[3],
                     float rgba[TGSI_NUM_CHANNELS][TGSI_QUAD_SIZE])
{
   struct chrome9_context *ctx = ((struct chrome9_vs_sampler *)sampler)->ctx;
   const struct pipe_sampler_view *view = sview_index < CHROME9_VS_MAX_SAMPLERS ?
      ctx->vs_sampler_views[sview_index] : NULL;
   const struct chrome9_vs_texture_mapping *mapping =
      view ? &ctx->vs_texture_mappings[sview_index] : NULL;

   for (unsigned lane = 0; lane < TGSI_QUAD_SIZE; lane++) {
      float result[4] = {0};

      if (view && mapping->base &&
          view->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
          lod[lane] >= (int)mapping->first_level &&
          lod[lane] <= (int)mapping->last_level && !k[lane] && !offset[2]) {
         unsigned level = lod[lane];
         unsigned width = u_minify(mapping->width, level);
         unsigned height = u_minify(mapping->height, level);
         int x = i[lane] + offset[0], y = j[lane] + offset[1];

         if (x >= 0 && (unsigned)x < width &&
             y >= 0 && (unsigned)y < height)
            chrome9_vs_fetch_bgra(mapping, level, x, y, result);
      }
      for (unsigned c = 0; c < 4; c++)
         rgba[c][lane] = result[c];
   }
}

static void
chrome9_vs_query_lod(const struct tgsi_sampler *sampler,
                     unsigned sview_index, unsigned sampler_index,
                     const float s[TGSI_QUAD_SIZE],
                     const float t[TGSI_QUAD_SIZE],
                     const float p[TGSI_QUAD_SIZE],
                     const float c0[TGSI_QUAD_SIZE],
                     enum tgsi_sampler_control control,
                     float mipmap[TGSI_QUAD_SIZE],
                     float lod[TGSI_QUAD_SIZE])
{
   (void)sampler;
   (void)sview_index;
   (void)sampler_index;
   (void)s;
   (void)t;
   (void)p;
   (void)c0;
   (void)control;
   for (unsigned lane = 0; lane < TGSI_QUAD_SIZE; lane++)
      mipmap[lane] = lod[lane] = 0.0f;
}

struct tgsi_sampler *
chrome9_vs_texture_sampler_create(struct chrome9_context *ctx)
{
   struct chrome9_vs_sampler *sampler = CALLOC_STRUCT(chrome9_vs_sampler);

   if (!sampler)
      return NULL;
   sampler->ctx = ctx;
   sampler->base.get_samples = chrome9_vs_get_samples;
   sampler->base.get_dims = chrome9_vs_get_dims;
   sampler->base.get_texel = chrome9_vs_get_texel;
   sampler->base.query_lod = chrome9_vs_query_lod;
   return &sampler->base;
}

void
chrome9_vs_texture_sampler_destroy(struct tgsi_sampler *sampler)
{
   FREE(sampler);
}
