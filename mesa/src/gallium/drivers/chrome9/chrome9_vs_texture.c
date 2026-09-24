/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_vs_texture.h"

#include "chrome9_context.h"
#include "chrome9_format.h"
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
chrome9_vs_fetch_texel(const struct pipe_sampler_view *view,
                       const struct chrome9_vs_texture_mapping *mapping,
                       unsigned level, unsigned x, unsigned y, float rgba[4])
{
   unsigned cpp = util_format_get_blocksize(view->format);
   const uint8_t *pixel = (const uint8_t *)mapping->base +
      mapping->mip_offset[level] + (size_t)y * mapping->row_stride[level] +
      (size_t)x * cpp;
   float decoded[4];
   const unsigned swizzle[4] = {
      view->swizzle_r, view->swizzle_g, view->swizzle_b, view->swizzle_a,
   };

   if (view->format == PIPE_FORMAT_X8Z24_UNORM) {
      util_format_unpack_z_float(view->format, &decoded[0], pixel, 1);
      decoded[1] = decoded[2] = 0.0f;
      decoded[3] = 1.0f;
   } else {
      util_format_unpack_rgba(view->format, decoded, pixel, 1);
   }
   for (unsigned c = 0; c < 4; c++) {
      switch (swizzle[c]) {
      case PIPE_SWIZZLE_X:
      case PIPE_SWIZZLE_Y:
      case PIPE_SWIZZLE_Z:
      case PIPE_SWIZZLE_W:
         rgba[c] = decoded[swizzle[c]];
         break;
      case PIPE_SWIZZLE_0:
         rgba[c] = 0.0f;
         break;
      case PIPE_SWIZZLE_1:
         rgba[c] = 1.0f;
         break;
      default:
         rgba[c] = 0.0f;
         break;
      }
   }
}

static float
chrome9_vs_wrap_coordinate(float coordinate, unsigned wrap)
{
   if (wrap == PIPE_TEX_WRAP_CLAMP_TO_EDGE)
      return CLAMP(coordinate, 0.0f, 1.0f);
   if (wrap == PIPE_TEX_WRAP_REPEAT) {
      float fraction = fmodf(coordinate, 1.0f);

      return fraction < 0.0f ? fraction + 1.0f : fraction;
   }
   if (wrap == PIPE_TEX_WRAP_MIRROR_REPEAT) {
      float mirrored = fmodf(coordinate, 2.0f);

      if (mirrored < 0.0f)
         mirrored += 2.0f;
      return mirrored > 1.0f ? 2.0f - mirrored : mirrored;
   }
   return 0.0f;
}

static unsigned
chrome9_vs_wrap_texel(int index, unsigned size, unsigned wrap)
{
   if (wrap == PIPE_TEX_WRAP_REPEAT) {
      int wrapped = index % (int)size;

      return wrapped < 0 ? wrapped + size : wrapped;
   }
   if (wrap == PIPE_TEX_WRAP_MIRROR_REPEAT) {
      int period = 2 * (int)size;
      int wrapped = index % period;

      if (wrapped < 0)
         wrapped += period;
      return wrapped >= (int)size ? period - 1 - wrapped : wrapped;
   }
   return CLAMP(index, 0, (int)size - 1);
}

static void
chrome9_vs_sample_level(const struct pipe_sampler_view *view,
                        const struct chrome9_vs_texture_mapping *mapping,
                        const struct pipe_sampler_state *state,
                        unsigned level, float s, float t,
                        const int8_t offset[3], unsigned filter,
                        float rgba[4])
{
   unsigned width = u_minify(mapping->width, level);
   unsigned height = u_minify(mapping->height, level);
   float x = chrome9_vs_wrap_coordinate(s, state->wrap_s) * width;
   float y = chrome9_vs_wrap_coordinate(t, state->wrap_t) * height;

   if (filter == PIPE_TEX_FILTER_NEAREST) {
      unsigned ix = chrome9_vs_wrap_texel((int)floorf(x) + offset[0],
                                          width, state->wrap_s);
      unsigned iy = chrome9_vs_wrap_texel((int)floorf(y) + offset[1],
                                          height, state->wrap_t);

      chrome9_vs_fetch_texel(view, mapping, level, ix, iy, rgba);
      return;
   }

   x -= 0.5f;
   y -= 0.5f;
   int x0 = floorf(x), y0 = floorf(y);
   float fx = x - x0, fy = y - y0;
   float texels[2][2][4];

   for (unsigned j = 0; j < 2; j++) {
      unsigned iy = chrome9_vs_wrap_texel(y0 + j + offset[1], height,
                                          state->wrap_t);

      for (unsigned i = 0; i < 2; i++) {
         unsigned ix = chrome9_vs_wrap_texel(x0 + i + offset[0], width,
                                             state->wrap_s);

         chrome9_vs_fetch_texel(view, mapping, level, ix, iy, texels[j][i]);
      }
   }
   for (unsigned c = 0; c < 4; c++) {
      float top = texels[0][0][c] * (1.0f - fx) + texels[0][1][c] * fx;
      float bottom = texels[1][0][c] * (1.0f - fx) + texels[1][1][c] * fx;

      rgba[c] = top * (1.0f - fy) + bottom * fy;
   }
}

static float
chrome9_vs_sample_lod(const struct chrome9_vs_texture_mapping *mapping,
                      const struct pipe_sampler_state *state,
                      const float lod[TGSI_QUAD_SIZE],
                      float derivs[3][2][TGSI_QUAD_SIZE],
                      enum tgsi_sampler_control control, unsigned lane)
{
   float value = state->lod_bias;

   switch (control) {
   case TGSI_SAMPLER_LOD_BIAS:
   case TGSI_SAMPLER_LOD_EXPLICIT:
      value += lod[lane];
      break;
   case TGSI_SAMPLER_DERIVS_EXPLICIT: {
      unsigned width = u_minify(mapping->width, mapping->first_level);
      unsigned height = u_minify(mapping->height, mapping->first_level);
      float sx = derivs[0][0][lane] * width;
      float tx = derivs[1][0][lane] * height;
      float sy = derivs[0][1][lane] * width;
      float ty = derivs[1][1][lane] * height;
      float rho = MAX2(hypotf(sx, tx), hypotf(sy, ty));

      value = rho > 0.0f ? log2f(rho) : -INFINITY;
      break;
   }
   case TGSI_SAMPLER_LOD_NONE:
   case TGSI_SAMPLER_LOD_ZERO:
      break;
   default:
      return NAN;
   }
   if (isnan(value))
      return NAN;
   return CLAMP(value, state->min_lod, state->max_lod);
}

/* Vertex programs execute in the draw module.  Sample from the same linear
 * guarded mip layout used by fragment resources, with Gallium's normalized
 * 2D filter, wrap, LOD, and view-swizzle semantics. */
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
      chrome9_texture_format_info(view->format) &&
      (state->min_img_filter == PIPE_TEX_FILTER_NEAREST ||
       state->min_img_filter == PIPE_TEX_FILTER_LINEAR) &&
      (state->mag_img_filter == PIPE_TEX_FILTER_NEAREST ||
       state->mag_img_filter == PIPE_TEX_FILTER_LINEAR) &&
      (state->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
       state->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST ||
       state->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR) &&
      (state->wrap_s == PIPE_TEX_WRAP_CLAMP_TO_EDGE ||
       state->wrap_s == PIPE_TEX_WRAP_REPEAT ||
       state->wrap_s == PIPE_TEX_WRAP_MIRROR_REPEAT) &&
      (state->wrap_t == PIPE_TEX_WRAP_CLAMP_TO_EDGE ||
       state->wrap_t == PIPE_TEX_WRAP_REPEAT ||
       state->wrap_t == PIPE_TEX_WRAP_MIRROR_REPEAT) &&
      state->compare_mode == PIPE_TEX_COMPARE_NONE &&
      !state->unnormalized_coords &&
      !offset[2];

   (void)r;
   (void)c0;
   for (unsigned lane = 0; lane < TGSI_QUAD_SIZE; lane++) {
      float result[4] = {0};

      if (valid && isfinite(s[lane]) && isfinite(t[lane])) {
         float lambda = chrome9_vs_sample_lod(mapping, state, lod, derivs,
                                               control, lane);

         if (isfinite(lambda)) {
            unsigned max_level = mapping->last_level - mapping->first_level;
            unsigned filter = lambda <= 0.0f ? state->mag_img_filter :
                                                   state->min_img_filter;
            float level = CLAMP(lambda, 0.0f, (float)max_level);
            unsigned first = mapping->first_level;

            if (state->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST)
               first += (unsigned)floorf(level + 0.5f);
            else if (state->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR)
               first += (unsigned)floorf(level);
            chrome9_vs_sample_level(view, mapping, state, first,
                                    s[lane], t[lane], offset, filter, result);
            if (state->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR &&
                first < mapping->last_level && level > 0.0f) {
               float next[4];
               float fraction = level - floorf(level);

               chrome9_vs_sample_level(view, mapping, state, first + 1,
                                       s[lane], t[lane], offset,
                                       state->min_img_filter, next);
               for (unsigned c = 0; c < 4; c++)
                  result[c] = result[c] * (1.0f - fraction) +
                              next[c] * fraction;
            }
         }
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
   const struct pipe_sampler_view *view = sview_index < CHROME9_VS_MAX_SAMPLERS ?
      ctx->vs_sampler_views[sview_index] : NULL;
   const struct chrome9_vs_texture_mapping *mapping =
      view ?
      &ctx->vs_texture_mappings[sview_index] : NULL;
   int64_t absolute = mapping ? (int64_t)mapping->first_level + level : -1;

   memset(dims, 0, 4 * sizeof(*dims));
   if (!mapping || !mapping->base || level < 0 ||
       absolute > mapping->last_level)
      return;
   dims[0] = u_minify(mapping->width, absolute);
   dims[1] = u_minify(mapping->height, absolute);
   dims[2] = 1;
   dims[3] = mapping->last_level - mapping->first_level + 1;
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

      int64_t level_index = mapping ?
         (int64_t)mapping->first_level + lod[lane] : -1;

      if (view && mapping->base &&
          chrome9_texture_format_info(view->format) &&
          lod[lane] >= 0 && level_index <= mapping->last_level &&
          !k[lane] && !offset[2]) {
         unsigned level = level_index;
         unsigned width = u_minify(mapping->width, level);
         unsigned height = u_minify(mapping->height, level);
         int64_t x = (int64_t)i[lane] + offset[0];
         int64_t y = (int64_t)j[lane] + offset[1];

         if (x >= 0 && (uint64_t)x < width &&
             y >= 0 && (uint64_t)y < height)
            chrome9_vs_fetch_texel(view, mapping, level, (unsigned)x,
                                   (unsigned)y, result);
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
