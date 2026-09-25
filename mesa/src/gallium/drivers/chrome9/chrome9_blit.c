/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * pipe->blit on the CPU: colour (any sampled source format, including
 * compressed ones, to any renderable destination), depth and stencil, with
 * scaling, flipping, nearest or linear filtering, channel masks, swizzle,
 * scissor, alpha blending and the render condition.  Used for
 * glCopyTexSubImage, glGetTexImage, glCopyPixels and framebuffer blits.
 * Also masked clears of color buffers beyond the hardware target.
 */

#include "chrome9_blit.h"

#include "chrome9_context.h"
#include "chrome9_query.h"
#include "pipe/p_context.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <math.h>
#include <string.h>

struct chrome9_blit_map {
   struct pipe_transfer *transfer;
   uint8_t *map;
   struct pipe_box box;
   enum pipe_format format;
   unsigned block_w, block_h, block_bytes;
};

static bool
chrome9_blit_map(struct pipe_context *pipe, struct pipe_resource *resource,
                 unsigned level, const struct pipe_box *box,
                 enum pipe_format format, unsigned usage,
                 struct chrome9_blit_map *m)
{
   const struct util_format_description *desc =
      util_format_description(resource->format);

   m->box = *box;
   m->format = format;
   m->block_w = desc->block.width;
   m->block_h = desc->block.height;
   m->block_bytes = desc->block.bits / 8;
   m->map = pipe->texture_map(pipe, resource, level, usage, &m->box,
                              &m->transfer);
   return m->map != NULL;
}

/* Address of the block holding texel (x, y, z) relative to the box. */
static uint8_t *
chrome9_blit_texel(const struct chrome9_blit_map *m, int x, int y, int z)
{
   return m->map + (size_t)z * m->transfer->layer_stride +
          (size_t)(y / m->block_h) * m->transfer->stride +
          (size_t)(x / m->block_w) * m->block_bytes;
}

static void
chrome9_blit_fetch(const struct chrome9_blit_map *m,
                   util_format_fetch_rgba_func_ptr fetch, int x, int y,
                   int z, float rgba[4])
{
   x = CLAMP(x, 0, m->box.width - 1);
   y = CLAMP(y, 0, m->box.height - 1);
   fetch(rgba, chrome9_blit_texel(m, x, y, z), x % m->block_w,
         y % m->block_h);
}

/* Source texel coordinate (relative to the source box) of destination
 * pixel d, and its bilinear weight. */
static float
chrome9_blit_coord(int d, int dst_size, int src_size)
{
   return (d + 0.5f) * (float)src_size / (float)dst_size;
}

static bool
chrome9_blit_inside(const struct pipe_blit_info *info, int x, int y)
{
   if (info->scissor_enable &&
       (x < (int)info->scissor.minx || x >= (int)info->scissor.maxx ||
        y < (int)info->scissor.miny || y >= (int)info->scissor.maxy))
      return false;
   if (info->num_window_rectangles) {
      bool in = false;

      for (unsigned i = 0; i < info->num_window_rectangles; i++) {
         const struct pipe_scissor_state *r = &info->window_rectangles[i];

         in |= x >= (int)r->minx && x < (int)r->maxx &&
               y >= (int)r->miny && y < (int)r->maxy;
      }
      if (in != info->window_rectangle_include)
         return false;
   }
   return true;
}

/* Normalised source box: positive extents, flips reported separately. */
static void
chrome9_blit_src_box(const struct pipe_box *in, struct pipe_box *out,
                     bool *flip_x, bool *flip_y)
{
   *out = *in;
   *flip_x = in->width < 0;
   *flip_y = in->height < 0;
   if (*flip_x) {
      out->x = in->x + in->width;
      out->width = -in->width;
   }
   if (*flip_y) {
      out->y = in->y + in->height;
      out->height = -in->height;
   }
   if (out->depth < 0) {
      out->z = in->z + in->depth;
      out->depth = -in->depth;
   }
}

static void
chrome9_blit_color(const struct pipe_blit_info *info,
                   const struct chrome9_blit_map *src,
                   const struct chrome9_blit_map *dst, bool flip_x,
                   bool flip_y)
{
   util_format_fetch_rgba_func_ptr fetch_src =
      util_format_fetch_rgba_func(src->format);
   util_format_fetch_rgba_func_ptr fetch_dst =
      util_format_fetch_rgba_func(dst->format);
   const bool integer = util_format_is_pure_integer(src->format);
   const bool linear = info->filter == PIPE_TEX_FILTER_LINEAR && !integer;
   const unsigned mask = info->mask & PIPE_MASK_RGBA;
   const int dw = dst->box.width, dh = dst->box.height;
   const int sw = src->box.width, sh = src->box.height;

   for (int z = 0; z < dst->box.depth; z++) {
      const int sz = MIN2(z * src->box.depth / MAX2(dst->box.depth, 1),
                          src->box.depth - 1);

      for (int y = 0; y < dh; y++) {
         for (int x = 0; x < dw; x++) {
            float c[4], d[4], out[4];
            float fx = chrome9_blit_coord(x, dw, sw);
            float fy = chrome9_blit_coord(y, dh, sh);

            if (!chrome9_blit_inside(info, dst->box.x + x, dst->box.y + y))
               continue;
            if (flip_x)
               fx = sw - fx;
            if (flip_y)
               fy = sh - fy;
            if (linear) {
               const float ux = fx - 0.5f, uy = fy - 0.5f;
               const int x0 = (int)floorf(ux), y0 = (int)floorf(uy);
               const float ax = ux - x0, ay = uy - y0;
               float t[4][4];

               chrome9_blit_fetch(src, fetch_src, x0, y0, sz, t[0]);
               chrome9_blit_fetch(src, fetch_src, x0 + 1, y0, sz, t[1]);
               chrome9_blit_fetch(src, fetch_src, x0, y0 + 1, sz, t[2]);
               chrome9_blit_fetch(src, fetch_src, x0 + 1, y0 + 1, sz, t[3]);
               for (unsigned i = 0; i < 4; i++)
                  c[i] = (t[0][i] * (1 - ax) + t[1][i] * ax) * (1 - ay) +
                         (t[2][i] * (1 - ax) + t[3][i] * ax) * ay;
            } else {
               chrome9_blit_fetch(src, fetch_src, (int)floorf(fx),
                                  (int)floorf(fy), sz, c);
            }
            if (info->swizzle_enable) {
               float s[4];

               memcpy(s, c, sizeof(s));
               for (unsigned i = 0; i < 4; i++)
                  c[i] = info->swizzle[i] < 4 ? s[info->swizzle[i]] :
                         info->swizzle[i] == PIPE_SWIZZLE_1 ?
                            (integer ? uif(1) : 1.0f) : 0.0f;
            }
            uint8_t *texel = chrome9_blit_texel(dst, x, y, z);

            if (mask != PIPE_MASK_RGBA || info->alpha_blend) {
               fetch_dst(d, texel, 0, 0);
               for (unsigned i = 0; i < 4; i++) {
                  float v = c[i];

                  if (info->alpha_blend && i < 3)
                     v = c[i] * c[3] + d[i] * (1.0f - c[3]);
                  out[i] = mask & (1u << i) ? v : d[i];
               }
            } else {
               memcpy(out, c, sizeof(out));
            }
            util_format_pack_rgba(dst->format, texel, out, 1);
         }
      }
   }
}

static void
chrome9_blit_zs(const struct pipe_blit_info *info,
                const struct chrome9_blit_map *src,
                const struct chrome9_blit_map *dst, bool flip_x, bool flip_y)
{
   const int dw = dst->box.width, dh = dst->box.height;
   const int sw = src->box.width, sh = src->box.height;
   const bool depth = info->mask & PIPE_MASK_Z &&
                      util_format_has_depth(util_format_description(src->format)) &&
                      util_format_has_depth(util_format_description(dst->format));
   const bool stencil = info->mask & PIPE_MASK_S &&
                        util_format_has_stencil(util_format_description(src->format)) &&
                        util_format_has_stencil(util_format_description(dst->format));

   for (int z = 0; z < dst->box.depth; z++) {
      const int sz = MIN2(z * src->box.depth / MAX2(dst->box.depth, 1),
                          src->box.depth - 1);

      for (int y = 0; y < dh; y++) {
         for (int x = 0; x < dw; x++) {
            int sx = (int)floorf(chrome9_blit_coord(x, dw, sw));
            int sy = (int)floorf(chrome9_blit_coord(y, dh, sh));
            const uint8_t *s;
            uint8_t *d;

            if (!chrome9_blit_inside(info, dst->box.x + x, dst->box.y + y))
               continue;
            if (flip_x)
               sx = sw - 1 - sx;
            if (flip_y)
               sy = sh - 1 - sy;
            s = chrome9_blit_texel(src, CLAMP(sx, 0, sw - 1),
                                   CLAMP(sy, 0, sh - 1), sz);
            d = chrome9_blit_texel(dst, x, y, z);
            /* Packing one aspect keeps the other of a combined format. */
            if (depth) {
               float v;

               util_format_unpack_z_float(src->format, &v, s, 1);
               util_format_pack_z_float(dst->format, d, &v, 1);
            }
            if (stencil) {
               uint8_t v;

               util_format_unpack_s_8uint(src->format, &v, s, 1);
               util_format_pack_s_8uint(dst->format, d, &v, 1);
            }
         }
      }
   }
}

void
chrome9_blit(struct pipe_context *pipe, const struct pipe_blit_info *info)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   const struct util_format_description *dst_desc =
      util_format_description(info->dst.format);
   const bool zs = util_format_is_depth_or_stencil(info->dst.format);
   struct chrome9_blit_map src, dst;
   struct pipe_box src_box;
   bool flip_x, flip_y;
   unsigned dst_usage = PIPE_MAP_WRITE;

   if (info->render_condition_enable &&
       !chrome9_render_condition_passes(ctx))
      return;
   if (!info->dst.box.width || !info->dst.box.height ||
       !info->dst.box.depth || !info->src.box.width ||
       !info->src.box.height ||
       (zs ? !(info->mask & (PIPE_MASK_Z | PIPE_MASK_S)) :
             !(info->mask & PIPE_MASK_RGBA)) ||
       dst_desc->layout == UTIL_FORMAT_LAYOUT_S3TC)
      return;
   chrome9_blit_src_box(&info->src.box, &src_box, &flip_x, &flip_y);
   /* Pixels outside the scissor, masked channels, blending and one aspect
    * of a combined depth/stencil format keep the destination's values. */
   if (info->scissor_enable || info->num_window_rectangles ||
       info->alpha_blend ||
       (!zs && (info->mask & PIPE_MASK_RGBA) != PIPE_MASK_RGBA) ||
       (zs && util_format_has_depth(dst_desc) &&
        util_format_has_stencil(dst_desc) &&
        (info->mask & (PIPE_MASK_Z | PIPE_MASK_S)) !=
           (PIPE_MASK_Z | PIPE_MASK_S)))
      dst_usage |= PIPE_MAP_READ;
   if (!chrome9_blit_map(pipe, info->src.resource, info->src.level, &src_box,
                         info->src.format, PIPE_MAP_READ, &src))
      return;
   if (!chrome9_blit_map(pipe, info->dst.resource, info->dst.level,
                         &info->dst.box, info->dst.format, dst_usage,
                         &dst)) {
      pipe->texture_unmap(pipe, src.transfer);
      return;
   }
   if (zs)
      chrome9_blit_zs(info, &src, &dst, flip_x, flip_y);
   else
      chrome9_blit_color(info, &src, &dst, flip_x, flip_y);
   pipe->texture_unmap(pipe, dst.transfer);
   pipe->texture_unmap(pipe, src.transfer);
}

void
chrome9_clear_surface_masked(struct pipe_context *pipe,
                             struct pipe_surface *surface,
                             const union pipe_color_union *color,
                             unsigned mask, unsigned x, unsigned y,
                             unsigned width, unsigned height)
{
   const enum pipe_format format = surface->format;
   struct pipe_transfer *transfer;
   struct pipe_box box;
   uint8_t *map;

   if (!width || !height || util_format_is_compressed(format))
      return;
   u_box_3d(x, y, surface->first_layer, width, height,
            surface->last_layer - surface->first_layer + 1, &box);
   map = pipe->texture_map(pipe, surface->texture, surface->level,
                           PIPE_MAP_READ | PIPE_MAP_WRITE, &box, &transfer);
   if (!map)
      return;
   for (int z = 0; z < box.depth; z++) {
      for (unsigned row = 0; row < height; row++) {
         uint8_t *texel = map + (size_t)z * transfer->layer_stride +
                          (size_t)row * transfer->stride;

         for (unsigned col = 0; col < width; col++) {
            union pipe_color_union value;

            /* The union carries float, signed or unsigned lanes alike. */
            util_format_unpack_rgba(format, value.ui, texel, 1);
            for (unsigned c = 0; c < 4; c++)
               if (mask & (1u << c))
                  value.ui[c] = color->ui[c];
            util_format_pack_rgba(format, texel, value.ui, 1);
            texel += util_format_get_blocksize(format);
         }
      }
   }
   pipe->texture_unmap(pipe, transfer);
}
