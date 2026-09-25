/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_vs_texture.h"

#include "chrome9_context.h"
#include "chrome9_format.h"
#include "chrome9_resource.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
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
   void *staging;
   size_t staging_size;
   uint64_t first_offset, span = 0;
   unsigned cpp, first, last;

   if (!mapping)
      return false;
   staging = mapping->staging;
   staging_size = mapping->staging_size;
   memset(mapping, 0, sizeof(*mapping));
   mapping->staging = staging;
   mapping->staging_size = staging_size;
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
      span = MAX2(span, offset - first_offset + (uint64_t)stride * height);
   }

   mapping->base = (const uint8_t *)resource->bo.map + first_offset;
   /* Tiled levels are detiled; uncached local levels are read once. */
   if (chrome9_resource_is_tiled(resource) ||
       chrome9_resource_is_local(resource)) {
      if (span > SIZE_MAX)
         return false;
      if (mapping->staging_size < span) {
         void *grown = realloc(mapping->staging, span);

         if (!grown)
            return false;
         mapping->staging = grown;
         mapping->staging_size = span;
      }
      for (unsigned level = first; level <= last; level++)
         chrome9_tiled_copy((uint8_t *)resource->bo.map +
                               resource->level_offset[level],
                            resource->level_stride[level],
                            chrome9_resource_is_tiled(resource), 0, 0,
                            u_minify(texture->width0, level) * cpp,
                            u_minify(texture->height0, level),
                            (uint8_t *)mapping->staging +
                               mapping->mip_offset[level],
                            resource->level_stride[level], false);
      mapping->base = mapping->staging;
   }
   mapping->width = texture->width0;
   mapping->height = texture->height0;
   mapping->first_level = first;
   mapping->last_level = last;
   return true;
}

void
chrome9_vs_texture_mapping_release(struct chrome9_vs_texture_mapping *mapping)
{
   free(mapping->staging);
   memset(mapping, 0, sizeof(*mapping));
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

   if (chrome9_format_is_d24(view->format)) {
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
   /* x - floor(x) is exact below 2^23, like fmodf(x, 1) + 1 for x < 0. */
   if (wrap == PIPE_TEX_WRAP_REPEAT)
      return coordinate - floorf(coordinate);
   if (wrap == PIPE_TEX_WRAP_MIRROR_REPEAT) {
      float mirrored = coordinate - 2.0f * floorf(coordinate * 0.5f);

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

/* The CPU sampler below implements the view and sampler state. */
bool
chrome9_vs_texture_samplable(const struct chrome9_context *ctx,
                             unsigned unit, unsigned sampler)
{
   const struct pipe_sampler_view *view = unit < CHROME9_VS_MAX_SAMPLERS ?
      ctx->vs_sampler_views[unit] : NULL;
   const struct pipe_sampler_state *state =
      sampler < CHROME9_VS_MAX_SAMPLERS ? ctx->vs_sampler_states[sampler] :
      NULL;

   return view && state && ctx->vs_texture_mappings[unit].base &&
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
      !state->unnormalized_coords;
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
   bool valid = chrome9_vs_texture_samplable(ctx, sview_index,
                                             sampler_index) &&
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

/* Four explicit LOD 0 lookups, as the draw module's vertex sampler. */
void
chrome9_vs_texture_sample(struct chrome9_context *ctx, unsigned unit,
                          unsigned sampler, const float s[4],
                          const float t[4], float rgba[4][4])
{
   static const float zero[TGSI_QUAD_SIZE];
   static const int8_t offset[3];
   float derivs[3][2][TGSI_QUAD_SIZE] = { 0 };

   ctx->vs_tgsi_sampler->get_samples(ctx->vs_tgsi_sampler, unit, sampler,
                                     s, t, zero, zero, zero, derivs, offset,
                                     TGSI_SAMPLER_LOD_EXPLICIT, rgba);
}

/* Byte of a 32-bit texel holding R, G, B and A (4: constant one). */
static bool
chrome9_vs_unorm8_layout(enum pipe_format format, uint8_t byte[4])
{
   static const struct {
      enum pipe_format format;
      uint8_t byte[4];
   } table[] = {
      { PIPE_FORMAT_B8G8R8A8_UNORM, { 2, 1, 0, 3 } },
      { PIPE_FORMAT_B8G8R8X8_UNORM, { 2, 1, 0, 4 } },
      { PIPE_FORMAT_R8G8B8A8_UNORM, { 0, 1, 2, 3 } },
      { PIPE_FORMAT_R8G8B8X8_UNORM, { 0, 1, 2, 4 } },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(table); i++)
      if (table[i].format == format) {
         memcpy(byte, table[i].byte, 4);
         return true;
      }
   return false;
}

/*
 * count explicit LOD 0 lookups at (s[i], t[i]) into out + i * out_stride.
 * 8-bit RGBA textures at the base level take a direct path with the
 * arithmetic of chrome9_vs_sample_level(); other states use the generic
 * sampler four lookups at a time.
 */
void
chrome9_vs_texture_sample_array(struct chrome9_context *ctx, unsigned unit,
                                unsigned sampler, unsigned count,
                                const float *s, const float *t, float *out,
                                unsigned out_stride)
{
   const struct pipe_sampler_view *view = ctx->vs_sampler_views[unit];
   const struct pipe_sampler_state *state = ctx->vs_sampler_states[sampler];
   const struct chrome9_vs_texture_mapping *mapping =
      &ctx->vs_texture_mappings[unit];
   const unsigned swizzle[4] = {
      view->swizzle_r, view->swizzle_g, view->swizzle_b, view->swizzle_a,
   };
   uint8_t byte[4];
   float unorm[256];

   /* LOD 0 plus bias, clamped: the base level with the MAG filter. */
   if (!chrome9_vs_unorm8_layout(view->format, byte) ||
       !(CLAMP(state->lod_bias, state->min_lod, state->max_lod) <= 0.0f)) {
      for (unsigned i = 0; i < count; i += 4) {
         float ss[4] = { 0 }, tt[4] = { 0 }, rgba[4][4];
         const unsigned lanes = MIN2(4, count - i);

         memcpy(ss, s + i, lanes * sizeof(float));
         memcpy(tt, t + i, lanes * sizeof(float));
         chrome9_vs_texture_sample(ctx, unit, sampler, ss, tt, rgba);
         for (unsigned l = 0; l < lanes; l++)
            for (unsigned c = 0; c < 4; c++)
               out[(size_t)(i + l) * out_stride + c] = rgba[c][l];
      }
      return;
   }
   for (unsigned v = 0; v < 256; v++)
      unorm[v] = (float)v * (1.0f / 255.0f);
   /* The view swizzle folds into the byte selection. */
   {
      uint8_t swizzled[4];

      for (unsigned c = 0; c < 4; c++)
         swizzled[c] = swizzle[c] <= PIPE_SWIZZLE_W ? byte[swizzle[c]] :
                       swizzle[c] == PIPE_SWIZZLE_1 ? 4 : 5;
      memcpy(byte, swizzled, sizeof(byte));
   }

   const unsigned level = mapping->first_level;
   const unsigned width = u_minify(mapping->width, level);
   const unsigned height = u_minify(mapping->height, level);
   const uint8_t *base = (const uint8_t *)mapping->base +
                         mapping->mip_offset[level];
   const unsigned pitch = mapping->row_stride[level];
   const unsigned filter = state->mag_img_filter;
   float zero_one[2] = { 0.0f, 1.0f };

   for (unsigned i = 0; i < count; i++) {
      float *rgba = out + (size_t)i * out_stride;
      float x, y;

      if (!isfinite(s[i]) || !isfinite(t[i])) {
         memset(rgba, 0, 4 * sizeof(float));
         continue;
      }
      x = chrome9_vs_wrap_coordinate(s[i], state->wrap_s) * width;
      y = chrome9_vs_wrap_coordinate(t[i], state->wrap_t) * height;
      if (filter == PIPE_TEX_FILTER_NEAREST) {
         unsigned ix = chrome9_vs_wrap_texel((int)floorf(x), width,
                                             state->wrap_s);
         unsigned iy = chrome9_vs_wrap_texel((int)floorf(y), height,
                                             state->wrap_t);
         const uint8_t *pixel = base + (size_t)iy * pitch + ix * 4;

         for (unsigned c = 0; c < 4; c++)
            rgba[c] = byte[c] < 4 ? unorm[pixel[byte[c]]] :
                      zero_one[byte[c] == 4];
         continue;
      }
      x -= 0.5f;
      y -= 0.5f;
      int x0 = floorf(x), y0 = floorf(y);
      float fx = x - x0, fy = y - y0;
      unsigned ix[2], iy[2];
      float texels[2][2][4];

      for (unsigned j = 0; j < 2; j++) {
         ix[j] = chrome9_vs_wrap_texel(x0 + j, width, state->wrap_s);
         iy[j] = chrome9_vs_wrap_texel(y0 + j, height, state->wrap_t);
      }
      for (unsigned j = 0; j < 2; j++)
         for (unsigned k = 0; k < 2; k++) {
            const uint8_t *pixel = base + (size_t)iy[j] * pitch + ix[k] * 4;

            for (unsigned c = 0; c < 4; c++)
               texels[j][k][c] = byte[c] < 4 ? unorm[pixel[byte[c]]] :
                                 zero_one[byte[c] == 4];
         }
      for (unsigned c = 0; c < 4; c++) {
         float top = texels[0][0][c] * (1.0f - fx) + texels[0][1][c] * fx;
         float bottom = texels[1][0][c] * (1.0f - fx) + texels[1][1][c] * fx;

         rgba[c] = top * (1.0f - fy) + bottom * fy;
      }
   }
}
