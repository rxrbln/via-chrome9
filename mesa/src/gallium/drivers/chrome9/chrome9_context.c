/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_context.h"
#include "chrome9_blit.h"

#include "chrome9_draw.h"
#include "chrome9_query.h"
#include "chrome9_swfb.h"
#include "chrome9_tcl.h"
#include "chrome9_drm.h"
#include "chrome9_public.h"
#include "chrome9_format.h"
#include "chrome9_fragment.h"
#include "chrome9_ps_schedule.h"
#include "chrome9_resource.h"
#include "chrome9_specialise.h"
#include "chrome9_tex_lower.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

#include "draw/draw_context.h"
#include "nir.h"
#include "nir_builder.h"
#include "tgsi/tgsi_parse.h"
#include "util/format/u_format.h"
#include "util/hash_table.h"
#include "util/u_draw.h"
#include "util/u_dynarray.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_prim.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"

/* Guarded stand-in for one level and layer of a render target that the
 * hardware cannot address directly, or a direct view of that image; one
 * list per resource. */
struct chrome9_shadow {
   struct chrome9_shadow *next;
   struct pipe_resource *shadow;
   unsigned level;
   unsigned layer;
   /* shadow is a view sharing the resource's BO (and referencing it). */
   bool direct;
   /* Drawable write generation both surfaces last agreed on. */
   uint32_t synced_generation;
   bool synced;
};

/* Level, layer and row layout of one image of a resource. */
struct chrome9_subimage {
   struct chrome9_resource *resource;
   uint64_t offset;
   uint32_t stride;
   unsigned width;
   unsigned height;
};

static void
chrome9_subimage(struct pipe_resource *resource, unsigned level,
                 unsigned layer, struct chrome9_subimage *image)
{
   struct chrome9_resource *res = chrome9_resource(resource);

   image->resource = res;
   /* 3D slices follow each other at the level's image size. */
   image->offset = res->level_offset[level] + layer *
      (res->layer_stride ? res->layer_stride : res->level_size[level]);
   image->stride = res->level_stride[level];
   image->width = u_minify(resource->width0, level);
   image->height = u_minify(resource->height0, level);
}

static struct chrome9_shadow *
chrome9_shadow_entry(struct chrome9_context *ctx, struct pipe_resource *color,
                     unsigned level, unsigned layer)
{
   struct hash_entry *entry = ctx->shadows ?
      _mesa_hash_table_search(ctx->shadows, color) : NULL;

   for (struct chrome9_shadow *shadow = entry ? entry->data : NULL; shadow;
        shadow = shadow->next)
      if (shadow->level == level && shadow->layer == layer)
         return shadow;
   return NULL;
}

/* The bound colour image when it renders through a shadow. */
static const struct pipe_surface *
chrome9_external_surface(struct chrome9_context *ctx)
{
   const struct pipe_surface *surface = ctx->framebuffer.nr_cbufs ?
                                        &ctx->framebuffer.cbufs[0] : NULL;

   return surface && surface->texture && ctx->render_target &&
          surface->texture != ctx->render_target &&
          chrome9_resource(ctx->render_target)->parent != surface->texture ?
          surface : NULL;
}

/* Record that the shadow and the drawable hold the same pixels. */
static void
chrome9_shadow_synced(struct chrome9_context *ctx,
                      const struct pipe_surface *surface)
{
   struct pipe_resource *color = surface->texture;
   struct chrome9_shadow *shadow =
      chrome9_shadow_entry(ctx, color, surface->level, surface->first_layer);

   if (shadow) {
      shadow->synced_generation =
         chrome9_resource(color)->cpu_write_generation;
      shadow->synced = true;
   }
}

/* CPU copy of the pixel rectangle rect between equal-format images. */
static void
chrome9_copy_image_rect(const struct chrome9_subimage *dst,
                        const struct chrome9_subimage *src,
                        const struct pipe_scissor_state *rect)
{
   unsigned cpp = util_format_get_blocksize(src->resource->base.format);
   unsigned maxx = MIN3(rect->maxx, dst->width, src->width);
   unsigned maxy = MIN3(rect->maxy, dst->height, src->height);
   uint8_t *dst_map = (uint8_t *)dst->resource->bo.map + dst->offset;
   const uint8_t *src_map =
      (const uint8_t *)src->resource->bo.map + src->offset;

   const bool src_tiled = chrome9_resource_is_tiled(src->resource);
   const bool dst_tiled = chrome9_resource_is_tiled(dst->resource);
   const unsigned bytes = (maxx - rect->minx) * cpp;
   uint8_t *line;

   if (rect->minx >= maxx || rect->miny >= maxy)
      return;
   /* Tiled rows spread over whole tile rows: announce the whole image. */
   if (src_tiled)
      chrome9_resource_cpu_access(src->resource, false);
   else
      chrome9_resource_cpu_access_range(src->resource, false,
         src->offset + (uint64_t)rect->miny * src->stride,
         (uint64_t)(maxy - rect->miny) * src->stride);
   if (dst_tiled)
      chrome9_resource_cpu_access(dst->resource, true);
   else
      chrome9_resource_cpu_access_range(dst->resource, true,
         dst->offset + (uint64_t)rect->miny * dst->stride,
         (uint64_t)(maxy - rect->miny) * dst->stride);
   if (!src_tiled && !dst_tiled) {
      for (unsigned y = rect->miny; y < maxy; y++)
         memcpy(dst_map + (size_t)y * dst->stride + rect->minx * cpp,
                src_map + (size_t)y * src->stride + rect->minx * cpp, bytes);
      return;
   }
   line = malloc(bytes);
   if (!line)
      return;
   for (unsigned y = rect->miny; y < maxy; y++) {
      chrome9_tiled_copy((uint8_t *)src_map, src->stride, src_tiled,
                         rect->minx * cpp, y, bytes, 1, line, bytes, false);
      chrome9_tiled_copy(dst_map, dst->stride, dst_tiled, rect->minx * cpp,
                         y, bytes, 1, line, bytes, true);
   }
   free(line);
}

static void
chrome9_copy_surface_rect(struct pipe_resource *destination,
                          struct pipe_resource *source,
                          const struct pipe_scissor_state *rect)
{
   struct chrome9_subimage dst, src;

   chrome9_subimage(destination, 0, 0, &dst);
   chrome9_subimage(source, 0, 0, &src);
   chrome9_copy_image_rect(&dst, &src, rect);
}

static void
chrome9_copy_surface(struct pipe_resource *destination,
                     struct pipe_resource *source)
{
   const struct pipe_scissor_state all = {
      0, 0, MIN2(destination->width0, source->width0),
      MIN2(destination->height0, source->height0),
   };

   chrome9_copy_surface_rect(destination, source, &all);
}

/* Publish the guarded shadow into the unguarded drawable with the 2D
 * engine; false leaves the copy to the CPU. */
static bool
chrome9_copy_surface_gpu(struct chrome9_context *ctx,
                         struct pipe_resource *destination,
                         struct pipe_resource *source)
{
   struct chrome9_resource *dst = chrome9_resource(destination);
   struct chrome9_resource *src = chrome9_resource(source);
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   struct drm_chrome9_copy_rect copy = { 0 };

   if (!ctx->copy_rect || !ctx->copy_rect_unguarded_destination ||
       dst->guarded || !src->guarded || dst->data_offset ||
       chrome9_resource_is_tiled(src) ||
       destination->format != source->format ||
       (source->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        source->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       destination->width0 != source->width0 ||
       destination->height0 != source->height0 ||
       dst->stride != ALIGN_POT(destination->width0 * 4, 32) ||
       src->stride != ALIGN_POT(source->width0 * 4, 32) ||
       !debug_get_bool_option("CHROME9_GPU_PUBLISH", false))
      return false;
   copy.destination_handle = dst->bo.handle;
   copy.source_handle = src->bo.handle;
   copy.flags = DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED;
   copy.width = source->width0;
   copy.height = source->height0;
   copy.destination_width = destination->width0;
   copy.destination_height = destination->height0;
   copy.destination_pitch = dst->stride;
   copy.source_width = source->width0;
   copy.source_height = source->height0;
   copy.source_pitch = src->stride;
   copy.format = source->format == PIPE_FORMAT_B8G8R8A8_UNORM ?
                 DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 :
                 DRM_CHROME9_TEXTURE_FORMAT_BGRX8888;
   return chrome9_copy_rect(fd, &copy);
}

/*
 * Uncached CPU reads of a local resource cost more than the faster engine
 * access saves: a resource the CPU reads moves to GART pages once.  Views
 * share the BO, so a resource with live views stays local.
 */
void
chrome9_context_demote_local(struct chrome9_context *ctx,
                             struct pipe_resource *pres)
{
   struct chrome9_resource *res = chrome9_resource(pres);
   struct pipe_screen *screen = ctx->base.screen;
   const int fd = screen->get_screen_fd(screen);
   struct chrome9_bo bo;

   if (!res->local || res->shared || res->parent || res->views)
      return;
   chrome9_draw_flush_batch(ctx);
   chrome9_tcl_batch_flush(ctx);
   chrome9_sw_finish(ctx);
   chrome9_resource_cpu_access(res, false);
   if (!chrome9_bo_create(fd, res->bo.size, 0, &bo))
      return;
   memcpy(bo.map, res->bo.map, res->bo.size);
   chrome9_screen_bo_destroy(screen, &res->bo);
   res->bo = bo;
   res->local = false;
   if (res->explicit_access)
      res->explicit_access =
         chrome9_bo_cpu_access(fd, &res->bo, DRM_CHROME9_GEM_CPU_EXPLICIT);
   chrome9_resource_cpu_access(res, true);
}

static void
chrome9_flush_render_target(struct chrome9_context *ctx)
{
   const struct pipe_surface *surface = chrome9_external_surface(ctx);

   if (!ctx->render_target_dirty)
      return;
   if (surface) {
      struct pipe_resource *external = surface->texture;

      if (!surface->level && !surface->first_layer &&
          chrome9_copy_surface_gpu(ctx, external, ctx->render_target)) {
         /* A device write: the drawable changed, but not by the CPU. */
         chrome9_resource(external)->cpu_write_generation++;
      } else {
         struct chrome9_subimage dst, src;

         chrome9_subimage(external, surface->level, surface->first_layer,
                          &dst);
         chrome9_subimage(ctx->render_target, 0, 0, &src);
         chrome9_copy_image_rect(&dst, &src, &ctx->render_dirty);
      }
      chrome9_shadow_synced(ctx, surface);
   }
   ctx->render_target_dirty = false;
}

struct chrome9_resource *
chrome9_context_render_target(struct chrome9_context *ctx)
{
   return ctx->render_target ? chrome9_resource(ctx->render_target) : NULL;
}

struct chrome9_resource *
chrome9_context_depth_target(struct chrome9_context *ctx)
{
   return ctx->framebuffer.zsbuf.texture ?
             chrome9_resource(ctx->framebuffer.zsbuf.texture) : NULL;
}

/* A draw or clear wrote the bound target: grow the dirty rectangle by the
 * scissor box, or to the whole target without scissoring. */
void
chrome9_context_render_target_written(struct chrome9_context *ctx)
{
   struct pipe_scissor_state box = {
      0, 0, ctx->framebuffer.width, ctx->framebuffer.height,
   };

   if (!ctx->framebuffer.nr_cbufs)
      return;
   if (ctx->last_target != ctx->render_target)
      pipe_resource_reference(&ctx->last_target, ctx->render_target);
   if (ctx->rasterizer && ctx->rasterizer->scissor && ctx->scissor_valid) {
      box.minx = MIN2(ctx->scissor.minx, box.maxx);
      box.miny = MIN2(ctx->scissor.miny, box.maxy);
      box.maxx = MIN2(ctx->scissor.maxx, box.maxx);
      box.maxy = MIN2(ctx->scissor.maxy, box.maxy);
   }
   if (!ctx->render_target_dirty) {
      ctx->render_dirty = box;
   } else {
      ctx->render_dirty.minx = MIN2(ctx->render_dirty.minx, box.minx);
      ctx->render_dirty.miny = MIN2(ctx->render_dirty.miny, box.miny);
      ctx->render_dirty.maxx = MAX2(ctx->render_dirty.maxx, box.maxx);
      ctx->render_dirty.maxy = MAX2(ctx->render_dirty.maxy, box.maxy);
   }
   ctx->render_target_dirty = true;
}

/* A synchronized CPU map observes all queued draws; the displayed target
 * additionally receives the shadow copy, and a CPU write makes it stale. */
void
chrome9_context_sync_resource(struct chrome9_context *ctx,
                              struct pipe_resource *resource, unsigned usage)
{
   struct pipe_resource *external;

   if (usage & PIPE_MAP_UNSYNCHRONIZED)
      return;
   chrome9_draw_flush_batch(ctx);
   external = ctx->framebuffer.nr_cbufs ?
              ctx->framebuffer.cbufs[0].texture : NULL;
   if (!external || resource != external || external == ctx->render_target)
      return;
   chrome9_flush_render_target(ctx);
   if (usage & PIPE_MAP_WRITE)
      ctx->render_target_stale = true;
}

void
chrome9_context_render_target_direct_written(struct chrome9_context *ctx)
{
   ctx->render_target_dirty = false;
   ctx->render_target_stale = true;
}

void
chrome9_context_prepare_render_target(struct chrome9_context *ctx)
{
   const struct pipe_surface *surface = chrome9_external_surface(ctx);

   if (!ctx->render_target_stale)
      return;
   if (surface) {
      struct chrome9_subimage dst, src;
      struct pipe_scissor_state all = { 0, 0, 0, 0 };

      chrome9_subimage(ctx->render_target, 0, 0, &dst);
      chrome9_subimage(surface->texture, surface->level,
                       surface->first_layer, &src);
      all.maxx = MIN2(dst.width, src.width);
      all.maxy = MIN2(dst.height, src.height);
      chrome9_copy_image_rect(&dst, &src, &all);
      chrome9_shadow_synced(ctx, surface);
   }
   ctx->render_target_stale = false;
}

void
chrome9_count_reject(struct chrome9_context *ctx, const char *site,
                     unsigned count)
{
   struct hash_entry *entry;

   ctx->rejected_triangles += count;
   /* Tagged draws may have been lost: the counting interval is not exact. */
   chrome9_query_mark_inexact(ctx);
   if (!ctx->reject_sites)
      return;
   entry = _mesa_hash_table_search(ctx->reject_sites, site);
   if (entry)
      entry->data = (void *)((uintptr_t)entry->data + count);
   else
      _mesa_hash_table_insert(ctx->reject_sites, site,
                              (void *)(uintptr_t)count);
}

/* Print every drop site whenever the total changed since the last flush. */
static void
chrome9_report_rejects(struct chrome9_context *ctx)
{
   if (!ctx->reject_sites || ctx->rejected_triangles == ctx->reject_reported)
      return;
   fprintf(stderr, "chrome9: rejected %" PRIu64 " (+%" PRIu64 "):",
           ctx->rejected_triangles,
           ctx->rejected_triangles - ctx->reject_reported);
   hash_table_foreach(ctx->reject_sites, entry)
      fprintf(stderr, " %s=%" PRIuPTR, (const char *)entry->key,
              (uintptr_t)entry->data);
   fputc('\n', stderr);
   ctx->reject_reported = ctx->rejected_triangles;
}

static void
chrome9_warn_once(struct chrome9_context *ctx, uint32_t warning,
                  const char *message)
{
   if (ctx->warnings & warning)
      return;
   ctx->warnings |= warning;
   fprintf(stderr, "chrome9: %s\n", message);
}

static void *
chrome9_dup_state(const void *state, size_t size)
{
   void *copy = MALLOC(size);

   if (copy)
      memcpy(copy, state, size);
   return copy;
}

static void *
chrome9_create_blend_state(struct pipe_context *pipe,
                           const struct pipe_blend_state *state)
{
   (void)pipe;
   return chrome9_dup_state(state, sizeof(*state));
}

static void
chrome9_bind_blend_state(struct pipe_context *pipe, void *state)
{
   chrome9_context(pipe)->blend = state;
}

static void *
chrome9_create_dsa_state(struct pipe_context *pipe,
                         const struct pipe_depth_stencil_alpha_state *state)
{
   (void)pipe;
   return chrome9_dup_state(state, sizeof(*state));
}

static void
chrome9_bind_dsa_state(struct pipe_context *pipe, void *state)
{
   chrome9_context(pipe)->dsa = state;
}

static void *
chrome9_create_rasterizer_state(struct pipe_context *pipe,
                                const struct pipe_rasterizer_state *state)
{
   struct pipe_rasterizer_state *copies;

   (void)pipe;
   /* The second copy is the draw module's for lower-left sprites. */
   copies = MALLOC(2 * sizeof(*state));
   if (!copies)
      return NULL;
   copies[0] = copies[1] = *state;
   copies[1].sprite_coord_mode = PIPE_SPRITE_COORD_UPPER_LEFT;
   return copies;
}

static void
chrome9_bind_rasterizer_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   /* The flush in draw_set_rasterizer_state() lets the wide point and line
    * stages rebind the previous state through this hook; take the new one
    * last or the hardware paths keep drawing with the old one. */
   draw_set_rasterizer_state(ctx->draw, state, state);
   ctx->rasterizer = state;
   ctx->draw_rasterizer = state;
}

/* DRM_CHROME9_TEXTURE_WRAP_* of a Gallium wrap mode, zero without one. */
static uint8_t
chrome9_sampler_wrap(struct pipe_screen *screen, unsigned wrap)
{
   switch (wrap) {
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      return DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_REPEAT:
      return DRM_CHROME9_TEXTURE_WRAP_REPEAT;
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      return DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      return chrome9_screen_has_capability2(screen,
                DRM_CHROME9_CAP2_TEXTURE_BORDER) ?
             DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER : 0;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      return chrome9_screen_has_capability2(screen,
                DRM_CHROME9_CAP2_TEXTURE_MIRROR_ONCE) ?
             DRM_CHROME9_TEXTURE_WRAP_MIRROR_CLAMP_TO_EDGE : 0;
   default:
      return 0;
   }
}

/* The sampler takes an ARGB8888 border, as the vendor drivers do. */
static uint32_t
chrome9_border_argb(const union pipe_color_union *color)
{
   uint32_t argb = 0;

   for (unsigned i = 0; i < 4; i++) {
      static const unsigned shift[4] = { 16, 8, 0, 24 };

      argb |= (uint32_t)lroundf(CLAMP(color->f[i], 0.0f, 1.0f) * 255.0f) <<
              shift[i];
   }
   return argb;
}

/*
 * The A30 LOD limits are whole levels.  A fractional limit under NEAREST mip
 * selection becomes the level GL selects at that LOD; LINEAR blends need it.
 */
static bool
chrome9_lod_level(float lod, unsigned mip_filter, float *level)
{
   if (!isfinite(lod))
      return false;
   if (lod <= 0.0f || lod == floorf(lod)) {
      *level = MAX2(lod, 0.0f);
      return true;
   }
   if (mip_filter == PIPE_TEX_MIPFILTER_LINEAR)
      return false;
   *level = lod <= 0.5f ? 0.0f : ceilf(lod + 0.5f) - 1.0f;
   return true;
}

static void *
chrome9_create_sampler_state(struct pipe_context *pipe,
                             const struct pipe_sampler_state *state)
{
   struct chrome9_sampler_state *sampler =
      CALLOC_STRUCT(chrome9_sampler_state);

   if (!sampler)
      return NULL;
   sampler->base = *state;
   sampler->supported = true;

   switch (state->min_img_filter) {
   case PIPE_TEX_FILTER_NEAREST: sampler->min_filter = 0; break;
   case PIPE_TEX_FILTER_LINEAR: sampler->min_filter = 1; break;
   default: sampler->supported = false; break;
   }
   switch (state->mag_img_filter) {
   case PIPE_TEX_FILTER_NEAREST: sampler->mag_filter = 0; break;
   case PIPE_TEX_FILTER_LINEAR: sampler->mag_filter = 1; break;
   default: sampler->supported = false; break;
   }
   switch (state->min_mip_filter) {
   case PIPE_TEX_MIPFILTER_NONE: sampler->mip_filter = 0; break;
   case PIPE_TEX_MIPFILTER_NEAREST:
      sampler->mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST;
      break;
   case PIPE_TEX_MIPFILTER_LINEAR:
      sampler->mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR;
      break;
   default: sampler->supported = false; break;
   }
   sampler->wrap_s = chrome9_sampler_wrap(pipe->screen, state->wrap_s);
   sampler->wrap_t = chrome9_sampler_wrap(pipe->screen, state->wrap_t);
   sampler->wrap_r = chrome9_sampler_wrap(pipe->screen, state->wrap_r);
   if (!sampler->wrap_s || !sampler->wrap_t)
      sampler->supported = false;
   if (sampler->wrap_s == DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER ||
       sampler->wrap_t == DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER ||
       sampler->wrap_r == DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER) {
      if (state->border_color_is_integer)
         sampler->supported = false;
      sampler->border_color = chrome9_border_argb(&state->border_color);
   }
   /* The comparison runs in the fragment shader (chrome9_nir_lower_shadow). */
   if (state->unnormalized_coords || !isfinite(state->lod_bias) ||
       state->lod_bias < -16.0f || state->lod_bias > 511.0f / 32.0f ||
       !chrome9_lod_level(state->min_lod, state->min_mip_filter,
                          &sampler->min_level) ||
       !chrome9_lod_level(state->max_lod, state->min_mip_filter,
                          &sampler->max_level) ||
       state->max_anisotropy > 1 || state->seamless_cube_map ||
       state->reduction_mode != PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE)
      sampler->supported = false;
   return sampler;
}

static void
chrome9_bind_sampler_states(struct pipe_context *pipe,
                            mesa_shader_stage shader, unsigned start,
                            unsigned count, void **states)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   unsigned end;

   if (shader == MESA_SHADER_VERTEX) {
      struct pipe_sampler_state *new_states[CHROME9_VS_MAX_SAMPLERS];
      unsigned num_states = 0;

      if (start >= CHROME9_VS_MAX_SAMPLERS)
         return;
      memcpy(new_states, ctx->vs_sampler_states, sizeof(new_states));
      end = MIN2(start + count, CHROME9_VS_MAX_SAMPLERS);
      for (unsigned i = start; i < end; i++)
         new_states[i] = states ? states[i - start] : NULL;
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_states[i])
            num_states = i + 1;
      /* draw_set_samplers flushes pending work before copying state.  The
       * interpreter callback must see the old state during that flush. */
      draw_set_samplers(ctx->draw, MESA_SHADER_VERTEX,
                        new_states, num_states);
      memcpy(ctx->vs_sampler_states, new_states, sizeof(new_states));
      ctx->num_vs_sampler_states = num_states;
      return;
   }
   if (shader != MESA_SHADER_FRAGMENT || start >= PIPE_MAX_SAMPLERS)
      return;
   end = MIN2(start + count, PIPE_MAX_SAMPLERS);
   for (unsigned i = start; i < end; i++)
      ctx->sampler_states[i] = states ? states[i - start] : NULL;
   ctx->num_sampler_states = 0;
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i])
         ctx->num_sampler_states = i + 1;
}

static void
chrome9_delete_sampler_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   bool vs_changed = false;

   chrome9_sw_forget(ctx, state);
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i] == state)
         ctx->sampler_states[i] = NULL;
   for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++) {
      if (ctx->vs_sampler_states[i] == state) {
         vs_changed = true;
      }
   }
   if (vs_changed) {
      struct pipe_sampler_state *new_states[CHROME9_VS_MAX_SAMPLERS];
      unsigned num_states = 0;

      memcpy(new_states, ctx->vs_sampler_states, sizeof(new_states));
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_states[i] == state)
            new_states[i] = NULL;
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_states[i])
            num_states = i + 1;
      draw_set_samplers(ctx->draw, MESA_SHADER_VERTEX,
                        new_states, num_states);
      memcpy(ctx->vs_sampler_states, new_states, sizeof(new_states));
      ctx->num_vs_sampler_states = num_states;
   }
   ctx->num_sampler_states = 0;
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i])
         ctx->num_sampler_states = i + 1;
   FREE(state);
}

static void
chrome9_delete_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   chrome9_sw_forget(ctx, state);
   if (ctx->blend == state)
      ctx->blend = NULL;
   if (ctx->dsa == state)
      ctx->dsa = NULL;
   if (ctx->rasterizer == state)
      ctx->rasterizer = NULL;
   FREE(state);
}

static void *
chrome9_create_vs_state(struct pipe_context *pipe,
                        const struct pipe_shader_state *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = CALLOC_STRUCT(chrome9_shader);

   if (!shader)
      return NULL;
   /* Clone before draw takes the NIR; TCL compiles it lazily. */
   if (ctx->tcl_enabled && state->type == PIPE_SHADER_IR_NIR && state->ir.nir)
      shader->tcl = chrome9_tcl_shader_create(state->ir.nir);
   if (chrome9_sw_enabled() && state->type == PIPE_SHADER_IR_NIR &&
       state->ir.nir)
      shader->sw_nir = nir_shader_clone(NULL, state->ir.nir);
   shader->draw = draw_create_vertex_shader(ctx->draw, state);
   shader->supported = shader->draw != NULL;
   if (!shader->draw) {
      chrome9_tcl_shader_destroy(shader->tcl);
      ralloc_free(shader->sw_nir);
      FREE(shader);
      return NULL;
   }
   return shader;
}

static void
chrome9_bind_vs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   ctx->vs = shader;
   draw_bind_vertex_shader(ctx->draw, shader ? shader->draw : NULL);
}

static void
chrome9_delete_vs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   if (!shader)
      return;
   if (ctx->vs == shader) {
      ctx->vs = NULL;
      draw_bind_vertex_shader(ctx->draw, NULL);
   }
   chrome9_sw_forget(ctx, shader);
   draw_delete_vertex_shader(ctx->draw, shader->draw);
   chrome9_tcl_shader_destroy(shader->tcl);
   ralloc_free(shader->sw_nir);
   FREE(shader);
}

/* The known Chrome9 pixel program is MOV oC0, v0.  Only accept the NIR form
 * whose sole result is the unmodified primary color; accepting arbitrary NIR
 * here would silently execute different fragment semantics. */
static bool
chrome9_fs_straight_line_entrypoint(nir_shader *nir,
                                    nir_function_impl **impl_out,
                                    nir_block **block_out)
{
   nir_function_impl *impl;
   nir_cf_node *node;

   if (!nir || nir->info.stage != MESA_SHADER_FRAGMENT ||
       !exec_list_is_singular(&nir->functions))
      return false;
   impl = nir_shader_get_entrypoint(nir);
   if (!impl || impl->preamble || !exec_list_is_singular(&impl->body))
      return false;
   node = exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node);
   if (node->type != nir_cf_node_block)
      return false;
   if (impl_out)
      *impl_out = impl;
   if (block_out)
      *block_out = nir_cf_node_as_block(node);
   return true;
}

/* GLSL 1.20 cannot declare noperspective inputs: only nir_opt_varyings
 * assigns it, to convergent varyings, whose value any mode interpolates. */
static bool
chrome9_fs_interpolated_mode(enum glsl_interp_mode mode)
{
   return mode == INTERP_MODE_NONE || mode == INTERP_MODE_SMOOTH ||
          mode == INTERP_MODE_NOPERSPECTIVE;
}

static bool
chrome9_fs_smooth_variable(const nir_variable *var, unsigned location)
{
   return var && var->data.mode == nir_var_shader_in &&
          var->data.location == location && !var->data.index &&
          !var->data.location_frac && !var->data.centroid &&
          !var->data.sample && !var->data.patch && !var->data.compact &&
          !var->data.per_primitive && !var->data.per_vertex &&
          chrome9_fs_interpolated_mode(var->data.interpolation);
}

static bool
chrome9_fs_flat_primary_variable(const nir_variable *var)
{
   return var && var->data.mode == nir_var_shader_in &&
          var->data.location == VARYING_SLOT_COL0 && !var->data.index &&
          !var->data.location_frac && !var->data.centroid &&
          !var->data.sample && !var->data.patch && !var->data.compact &&
          !var->data.per_primitive && !var->data.per_vertex &&
          var->data.interpolation == INTERP_MODE_FLAT;
}

static bool
chrome9_fs_has_flat_primary(nir_shader *nir)
{
   nir_foreach_shader_in_variable(var, nir) {
      if (chrome9_fs_flat_primary_variable(var))
         return true;
   }
   return false;
}

static bool
chrome9_fs_smooth_pixel_load_range(const nir_intrinsic_instr *load,
                                   unsigned location)
{
   nir_instr *instr;
   nir_intrinsic_instr *bary;

   if (!load || load->intrinsic != nir_intrinsic_load_interpolated_input ||
       nir_intrinsic_io_semantics(load).location != location ||
       !load->src[0].ssa ||
       !nir_src_is_const(load->src[1]) || nir_src_as_uint(load->src[1]))
      return false;
   instr = nir_def_instr(load->src[0].ssa);
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   bary = nir_instr_as_intrinsic(instr);
   return bary->intrinsic == nir_intrinsic_load_barycentric_pixel &&
          chrome9_fs_interpolated_mode(nir_intrinsic_interp_mode(bary));
}

static bool
chrome9_fs_smooth_pixel_load(const nir_intrinsic_instr *load,
                             unsigned location)
{
   return chrome9_fs_smooth_pixel_load_range(load, location) &&
          !nir_intrinsic_component(load);
}

static bool
chrome9_fs_is_primary_color(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      nir_variable *var;

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
         return nir_intrinsic_io_semantics(intr).location ==
                   VARYING_SLOT_COL0 &&
                !nir_intrinsic_component(intr);
      case nir_intrinsic_load_interpolated_input:
         return chrome9_fs_smooth_pixel_load(intr, VARYING_SLOT_COL0);
      case nir_intrinsic_load_deref:
         var = nir_intrinsic_get_var(intr, 0);
         return chrome9_fs_smooth_variable(var, VARYING_SLOT_COL0);
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_primary_color(alu->src[0].src.ssa, depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *source = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != source ||
                alu->src[c].swizzle[0] != c)
               return false;
         return chrome9_fs_is_primary_color(source, depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_flat_primary_color(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      return intr->intrinsic == nir_intrinsic_load_deref &&
             chrome9_fs_flat_primary_variable(
                nir_intrinsic_get_var(intr, 0));
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_flat_primary_color(alu->src[0].src.ssa,
                                                  depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *source = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != source ||
                alu->src[c].swizzle[0] != c)
               return false;
         return chrome9_fs_is_flat_primary_color(source, depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_texcoord(nir_def *def, unsigned unit, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   unsigned location;

   if (depth > 8 || def->num_components < 2)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      nir_variable *var;

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
         location = nir_intrinsic_io_semantics(intr).location;
         return (location == VARYING_SLOT_TEX0 + unit ||
                 location == VARYING_SLOT_VAR0 + unit) &&
                !nir_intrinsic_component(intr);
      case nir_intrinsic_load_interpolated_input:
         return chrome9_fs_smooth_pixel_load(intr,
                                              VARYING_SLOT_TEX0 + unit) ||
                chrome9_fs_smooth_pixel_load(intr,
                                              VARYING_SLOT_VAR0 + unit);
      case nir_intrinsic_load_deref:
         var = nir_intrinsic_get_var(intr, 0);
         return chrome9_fs_smooth_variable(var,
                                            VARYING_SLOT_TEX0 + unit) ||
                chrome9_fs_smooth_variable(var,
                                            VARYING_SLOT_VAR0 + unit);
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov)
         return alu->src[0].swizzle[0] == 0 &&
                alu->src[0].swizzle[1] == 1 &&
                chrome9_fs_is_texcoord(alu->src[0].src.ssa, unit, depth + 1);
      if (alu->op == nir_op_vec2) {
         nir_def *source = alu->src[0].src.ssa;

         return alu->src[1].src.ssa == source &&
                alu->src[0].swizzle[0] == 0 &&
                alu->src[1].swizzle[0] == 1 &&
                chrome9_fs_is_texcoord(source, unit, depth + 1);
      }
   }
   return false;
}

/* Glamor's simplest solid shader is an unmodified vec4 from the first
 * fragment constant.  Keep this deliberately narrower than general uniform
 * support: one full 32-bit vec4, buffer zero, byte/vec4 offset zero. */
static bool
chrome9_fs_is_constant_color(nir_def *def, unsigned index, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      switch (intr->intrinsic) {
      case nir_intrinsic_load_uniform:
         return nir_src_is_const(intr->src[0]) &&
                nir_intrinsic_base(intr) <= index &&
                nir_src_as_uint(intr->src[0]) ==
                   index - nir_intrinsic_base(intr);
      case nir_intrinsic_load_ubo:
         return nir_src_is_const(intr->src[0]) &&
                !nir_src_as_uint(intr->src[0]) &&
                nir_src_is_const(intr->src[1]) &&
                nir_src_as_uint(intr->src[1]) == index * 16;
      case nir_intrinsic_load_ubo_vec4:
         return !nir_intrinsic_component(intr) &&
                nir_src_is_const(intr->src[0]) &&
                !nir_src_as_uint(intr->src[0]) &&
                nir_src_is_const(intr->src[1]) &&
                nir_intrinsic_base(intr) <= index &&
                nir_src_as_uint(intr->src[1]) ==
                   index - nir_intrinsic_base(intr);
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_constant_color(alu->src[0].src.ssa, index,
                                             depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_texture(nir_def *def, unsigned unit)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   int coord;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   return tex->op == nir_texop_tex &&
          tex->sampler_dim == GLSL_SAMPLER_DIM_2D && !tex->is_array &&
          !tex->is_shadow && tex->texture_index == unit &&
          tex->sampler_index == unit && tex->coord_components == 2 &&
          coord >= 0 && tex->num_srcs == 1 &&
          chrome9_fs_is_texcoord(tex->src[coord].src.ssa, unit, 0);
}

static bool
chrome9_fs_is_texcoord_q(nir_def *def, unsigned unit, unsigned depth)
{
   nir_instr *instr;

   if (depth > 8 || def->num_components != 1 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov && alu->src[0].swizzle[0] == 3 &&
          chrome9_fs_is_texcoord(alu->src[0].src.ssa, unit, depth + 1);
}

static bool
chrome9_fs_is_projected_texture(nir_def *def, unsigned unit)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   int coord, projector;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   projector = nir_tex_instr_src_index(tex, nir_tex_src_projector);
   return tex->op == nir_texop_tex &&
          tex->sampler_dim == GLSL_SAMPLER_DIM_2D && !tex->is_array &&
          !tex->is_shadow && tex->texture_index == unit &&
          tex->sampler_index == unit &&
          tex->coord_components == 2 && coord >= 0 && projector >= 0 &&
          tex->num_srcs == 2 &&
          chrome9_fs_is_texcoord(tex->src[coord].src.ssa, unit, 0) &&
          chrome9_fs_is_texcoord_q(tex->src[projector].src.ssa, unit, 0);
}

enum chrome9_fs_class_leaf {
   CHROME9_FS_LEAF_INVALID,
   CHROME9_FS_LEAF_TEXTURE0,
   CHROME9_FS_LEAF_TEXTURE1,
   CHROME9_FS_LEAF_PRIMARY,
   CHROME9_FS_LEAF_CONSTANT0,
   CHROME9_FS_LEAF_CONSTANT1,
   CHROME9_FS_LEAF_TEXTURE0_ALPHA,
   CHROME9_FS_LEAF_CONSTANT0_ALPHA,
   CHROME9_FS_LEAF_CONSTANT1_ALPHA,
};

static unsigned
chrome9_fs_alpha_leaf(unsigned leaf)
{
   switch (leaf) {
   case CHROME9_FS_LEAF_TEXTURE0:
      return CHROME9_FS_LEAF_TEXTURE0_ALPHA;
   case CHROME9_FS_LEAF_CONSTANT0:
      return CHROME9_FS_LEAF_CONSTANT0_ALPHA;
   case CHROME9_FS_LEAF_CONSTANT1:
      return CHROME9_FS_LEAF_CONSTANT1_ALPHA;
   default:
      return CHROME9_FS_LEAF_INVALID;
   }
}

static unsigned
chrome9_fs_leaf(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return CHROME9_FS_LEAF_INVALID;
   if (chrome9_fs_is_texture(def, 0))
      return CHROME9_FS_LEAF_TEXTURE0;
   if (chrome9_fs_is_texture(def, 1))
      return CHROME9_FS_LEAF_TEXTURE1;
   if (chrome9_fs_is_primary_color(def, depth))
      return CHROME9_FS_LEAF_PRIMARY;
   if (chrome9_fs_is_constant_color(def, 0, depth))
      return CHROME9_FS_LEAF_CONSTANT0;
   if (chrome9_fs_is_constant_color(def, 1, depth))
      return CHROME9_FS_LEAF_CONSTANT1;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         bool identity = true, alpha = true;

         for (unsigned c = 0; c < 4; c++) {
            identity &= alu->src[0].swizzle[c] == c;
            alpha &= alu->src[0].swizzle[c] == 3;
         }
         if (!identity && !alpha)
            return CHROME9_FS_LEAF_INVALID;
         unsigned leaf = chrome9_fs_leaf(alu->src[0].src.ssa, depth + 1);

         return identity ? leaf : chrome9_fs_alpha_leaf(leaf);
      }
   }
   return CHROME9_FS_LEAF_INVALID;
}

static unsigned
chrome9_fs_source_leaf(const nir_alu_src *src, unsigned depth)
{
   unsigned leaf;
   bool identity = true, alpha = true;

   if (!src->src.ssa || src->src.ssa->num_components != 4)
      return CHROME9_FS_LEAF_INVALID;
   for (unsigned c = 0; c < 4; c++) {
      identity &= src->swizzle[c] == c;
      alpha &= src->swizzle[c] == 3;
   }
   if (!identity && !alpha)
      return CHROME9_FS_LEAF_INVALID;
   leaf = chrome9_fs_leaf(src->src.ssa, depth + 1);
   if (identity)
      return leaf;
   return chrome9_fs_alpha_leaf(leaf);
}

static bool
chrome9_fs_is_one(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 1 || def->bit_size != 32)
      return false;
   if (instr->type == nir_instr_type_load_const)
      return nir_instr_as_load_const(instr)->value[0].u32 == 0x3f800000;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      return alu->op == nir_op_mov && !alu->src[0].swizzle[0] &&
             chrome9_fs_is_one(alu->src[0].src.ssa, depth + 1);
   }
   return false;
}

static bool
chrome9_fs_is_zero(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 1 || def->bit_size != 32)
      return false;
   if (instr->type == nir_instr_type_load_const)
      return nir_instr_as_load_const(instr)->value[0].u32 == 0;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      return alu->op == nir_op_mov && !alu->src[0].swizzle[0] &&
             chrome9_fs_is_zero(alu->src[0].src.ssa, depth + 1);
   }
   return false;
}

static bool
chrome9_fs_is_generic0(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);

   if (!def->num_components || def->num_components > 4 ||
       def->bit_size != 32 || instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_variable *var;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      return nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_VAR0 &&
             !nir_intrinsic_component(intr);
   case nir_intrinsic_load_interpolated_input:
      return chrome9_fs_smooth_pixel_load(intr, VARYING_SLOT_VAR0);
   case nir_intrinsic_load_deref:
      var = nir_intrinsic_get_var(intr, 0);
      return chrome9_fs_smooth_variable(var, VARYING_SLOT_VAR0);
   default:
      return false;
   }
}

static bool
chrome9_fs_is_flat_generic0(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *intr;
   nir_variable *var;

   if (def->num_components != 1 || def->bit_size != 32 ||
       instr->type != nir_instr_type_intrinsic)
      return false;
   intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   var = nir_intrinsic_get_var(intr, 0);
   return var && var->data.mode == nir_var_shader_in &&
          var->data.location == VARYING_SLOT_VAR0 && !var->data.index &&
          !var->data.location_frac && !var->data.centroid &&
          !var->data.sample && !var->data.patch && !var->data.compact &&
          !var->data.per_primitive && !var->data.per_vertex &&
          var->data.interpolation == INTERP_MODE_FLAT;
}

static bool
chrome9_fs_is_flat_generic0_rgbx(nir_def *def, unsigned depth)
{
   nir_instr *instr;

   if (depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_flat_generic0_rgbx(alu->src[0].src.ssa,
                                               depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   nir_def *source = alu->src[0].src.ssa;
   for (unsigned c = 0; c < 3; c++)
      if (alu->src[c].src.ssa != source || alu->src[c].swizzle[0])
         return false;
   return alu->src[3].swizzle[0] == 0 &&
          chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1) &&
          chrome9_fs_is_flat_generic0(source);
}

/* GLSL user varyings are linked to VAR0 rather than the legacy COL0 slot.
 * Accept only a complete, identity-ordered vec4 so it can be represented by
 * the same normalized D1 diffuse attribute as the proven COL0 program. */
static bool
chrome9_fs_is_generic0_rgba(nir_def *def, unsigned depth)
{
   nir_instr *instr;

   if (depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   if (chrome9_fs_is_generic0(def))
      return true;
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_generic0_rgba(alu->src[0].src.ssa,
                                             depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *source = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != source ||
                alu->src[c].swizzle[0] != c)
               return false;
         return chrome9_fs_is_generic0_rgba(source, depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_generic0_component(const nir_alu_src *src, unsigned component,
                                 nir_def **base, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (chrome9_fs_is_generic0(def)) {
      if (component >= def->num_components || src->swizzle[0] != component)
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == nir_op_mov &&
          chrome9_fs_is_generic0_component(&alu->src[0], component, base,
                                           depth + 1);
}

static bool
chrome9_fs_is_generic0_rgbx(nir_def *def, nir_def **base, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *found = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_rgbx(alu->src[0].src.ssa, base,
                                         depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *source = NULL;

      if (!chrome9_fs_is_generic0_component(&alu->src[c], 0, &source,
                                            depth + 1) ||
          (found && source != found))
         return false;
      found = source;
   }
   if (alu->src[3].swizzle[0] ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = found;
   return found != NULL;
}

static bool
chrome9_fs_is_generic0_vec3_rgbx(nir_def *def, nir_def **base,
                                 unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *found = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_vec3_rgbx(alu->src[0].src.ssa, base,
                                               depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *source = NULL;

      if (!chrome9_fs_is_generic0_component(&alu->src[c], c, &source,
                                            depth + 1) ||
          source->num_components != 3 || (found && source != found))
         return false;
      found = source;
   }
   if (alu->src[3].swizzle[0] ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = found;
   return found != NULL;
}

/* glmark2 ideas uses a two-component vertex result as
 * vec4(VAR0.x, VAR0.x, VAR0.y, 1).  Preserve that exact component mapping in
 * the post-TNL D1 packing path; this is not permission to accept a general
 * varying swizzle. */
static bool
chrome9_fs_is_generic0_xxy_rgbx(nir_def *def, nir_def **base,
                                unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *found = NULL;
   static const unsigned component[3] = { 0, 0, 1 };

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_xxy_rgbx(alu->src[0].src.ssa, base,
                                              depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *source = NULL;

      if (!chrome9_fs_is_generic0_component(&alu->src[c], component[c],
                                             &source, depth + 1) ||
          source->num_components != 2 || (found && source != found))
         return false;
      found = source;
   }
   if (alu->src[3].swizzle[0] ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = found;
   return found != NULL;
}

/* glmark2's default Gouraud material is (0, 0, 1, 1).  Linking therefore
 * reduces Color to one user varying and reconstructs blue in the FS. */
static bool
chrome9_fs_is_generic0_blue_rgbx(nir_def *def, nir_def **base,
                                 unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *source = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_blue_rgbx(alu->src[0].src.ssa, base,
                                               depth + 1);
   }
   if (alu->op != nir_op_vec4 || alu->src[0].swizzle[0] ||
       alu->src[1].swizzle[0] || alu->src[3].swizzle[0] ||
       !chrome9_fs_is_zero(alu->src[0].src.ssa, depth + 1) ||
       !chrome9_fs_is_zero(alu->src[1].src.ssa, depth + 1) ||
       !chrome9_fs_is_generic0_component(&alu->src[2], 0, &source,
                                         depth + 1) ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = source;
   return source && source->num_components == 1;
}

static bool
chrome9_fs_is_literal_color(nir_def *def, uint32_t *color)
{
   nir_instr *instr;
   nir_load_const_instr *load;
   float rgba[4];

   if (!def || def->num_components != 4 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_load_const)
      return false;
   load = nir_instr_as_load_const(instr);
   for (unsigned c = 0; c < 4; c++) {
      rgba[c] = uif(load->value[c].u32);
      if (!isfinite(rgba[c]))
         return false;
   }
   *color = (uint32_t)float_to_ubyte(rgba[2]) |
            (uint32_t)float_to_ubyte(rgba[1]) << 8 |
            (uint32_t)float_to_ubyte(rgba[0]) << 16 |
            (uint32_t)float_to_ubyte(rgba[3]) << 24;
   return true;
}

static bool
chrome9_fs_is_packed_texcoord(nir_def *def, nir_def **base, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *first = NULL, *second = NULL;

   if (depth > 8 || def->num_components != 2 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_vec2 ||
       !chrome9_fs_is_generic0_component(&alu->src[0], 1, &first,
                                         depth + 1) ||
       !chrome9_fs_is_generic0_component(&alu->src[1], 2, &second,
                                         depth + 1) ||
       first != second)
      return false;
   *base = first;
   return true;
}

static bool
chrome9_fs_is_packed_texture(nir_def *def, nir_def **base)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   int coord;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   return tex->op == nir_texop_tex &&
          tex->sampler_dim == GLSL_SAMPLER_DIM_2D && !tex->is_array &&
          !tex->is_shadow && !tex->texture_index && !tex->sampler_index &&
          tex->coord_components == 2 && coord >= 0 && tex->num_srcs == 1 &&
          chrome9_fs_is_packed_texcoord(tex->src[coord].src.ssa, base, 0);
}

static bool
chrome9_fs_is_packed_gray_modulate(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *texture_base = NULL, *color_base = NULL;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_packed_texture(alu->src[0].src.ssa,
                                                &texture_base);
   if (texture_first) {
      if (!chrome9_fs_is_generic0_rgbx(alu->src[1].src.ssa, &color_base, 0))
         return false;
   } else {
      if (!chrome9_fs_is_packed_texture(alu->src[1].src.ssa, &texture_base) ||
          !chrome9_fs_is_generic0_rgbx(alu->src[0].src.ssa, &color_base, 0))
         return false;
   }
   return texture_base == color_base && texture_base &&
          texture_base->num_components == 3;
}

static bool
chrome9_fs_is_texture_component(const nir_alu_src *src, unsigned component,
                                unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4)
      return src->swizzle[0] == component && chrome9_fs_is_texture(def, 0);
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == nir_op_mov &&
          chrome9_fs_is_texture_component(&alu->src[0], component,
                                          depth + 1);
}

static bool
chrome9_fs_is_primary_component(const nir_alu_src *src, unsigned component,
                                unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4)
      return src->swizzle[0] == component &&
             chrome9_fs_is_primary_color(def, depth + 1);
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_primary_component(&alu->src[0], component,
                                          depth + 1);
}

static bool
chrome9_fs_is_projected_primary_product(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 0);
   return texture_first ?
      chrome9_fs_is_primary_color(alu->src[1].src.ssa, 0) :
      (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 0) &&
       chrome9_fs_is_primary_color(alu->src[0].src.ssa, 0));
}

static bool
chrome9_fs_is_projected_constant_product(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 0);
   return texture_first ?
      chrome9_fs_is_constant_color(alu->src[1].src.ssa, 0, 0) :
      (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 0) &&
       chrome9_fs_is_constant_color(alu->src[0].src.ssa, 0, 0));
}

static bool
chrome9_fs_is_projected_constant_product_component(const nir_alu_src *src,
                                                   unsigned component,
                                                   nir_def **base,
                                                   unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_projected_constant_product(def))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_projected_constant_product_component(
             &alu->src[0], component, base, depth + 1);
}

static bool
chrome9_fs_is_constant_component(const nir_alu_src *src, unsigned index,
                                 unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4)
      return src->swizzle[0] == component &&
             chrome9_fs_is_constant_color(def, index, depth + 1);
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_constant_component(&alu->src[0], index, component,
                                           depth + 1);
}

static bool
chrome9_fs_is_projected_constant_rgb_alpha(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_projected_constant_rgb_alpha(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_projected_constant_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product && chrome9_fs_is_constant_component(&alu->src[3], 0, 3,
                                                       depth + 1);
}

static bool
chrome9_fs_is_projected_product_component(const nir_alu_src *src,
                                          unsigned component,
                                          nir_def **base, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_projected_primary_product(def))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_projected_product_component(&alu->src[0], component,
                                                     base, depth + 1);
}

static bool
chrome9_fs_is_projected_primary_rgb_alpha(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_projected_primary_rgb_alpha(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_projected_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product &&
          chrome9_fs_is_primary_component(&alu->src[3], 3, depth + 1);
}

static bool
chrome9_fs_is_two_projected_primary_product(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 1);
   return texture_first ?
      chrome9_fs_is_projected_primary_rgb_alpha(alu->src[1].src.ssa, 0) :
      (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 1) &&
       chrome9_fs_is_projected_primary_rgb_alpha(alu->src[0].src.ssa, 0));
}

static bool
chrome9_fs_is_two_projected_product_component(const nir_alu_src *src,
                                              unsigned component,
                                              nir_def **base,
                                              unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_two_projected_primary_product(def))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_two_projected_product_component(
             &alu->src[0], component, base, depth + 1);
}

static bool
chrome9_fs_is_two_projected_primary_rgb_alpha(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_two_projected_primary_rgb_alpha(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_two_projected_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product &&
          chrome9_fs_is_primary_component(&alu->src[3], 3, depth + 1);
}

static bool
chrome9_fs_is_two_projected_full_primary_product(nir_def *def,
                                                 nir_def **inner_product)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 1);
   nir_def *inner = texture_first ? alu->src[1].src.ssa :
                                    alu->src[0].src.ssa;

   if (!(texture_first ?
         chrome9_fs_is_projected_primary_product(inner) :
         (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 1) &&
          chrome9_fs_is_projected_primary_product(inner))))
      return false;
   if (inner_product)
      *inner_product = inner;
   return true;
}

static bool
chrome9_fs_is_two_projected_full_product_component(const nir_alu_src *src,
                                                   unsigned component,
                                                   nir_def **base,
                                                   unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_two_projected_full_primary_product(def, NULL))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_two_projected_full_product_component(
             &alu->src[0], component, base, depth + 1);
}

static bool
chrome9_fs_is_def_component(const nir_alu_src *src, nir_def *base,
                            unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def == base)
      return def->num_components == 4 && src->swizzle[0] == component;
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_def_component(&alu->src[0], base, component,
                                      depth + 1);
}

static bool
chrome9_fs_is_two_projected_texture0_alpha_primary(nir_def *def,
                                                   unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;
   nir_def *inner_product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_two_projected_texture0_alpha_primary(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_two_projected_full_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product &&
          chrome9_fs_is_two_projected_full_primary_product(
             product, &inner_product) &&
          chrome9_fs_is_def_component(&alu->src[3], inner_product, 3,
                                      depth + 1);
}

static bool
chrome9_fs_is_rgbx(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_rgbx(alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++)
      if (!chrome9_fs_is_texture_component(&alu->src[c], c, depth + 1))
         return false;
   return !alu->src[3].swizzle[0] &&
          chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1);
}

static bool
chrome9_fs_is_input_at(nir_def *def, unsigned location)
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *intr;
   nir_variable *var;

   if (instr->type != nir_instr_type_intrinsic)
      return false;
   intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      return nir_intrinsic_io_semantics(intr).location == location &&
             !nir_intrinsic_component(intr);
   case nir_intrinsic_load_interpolated_input:
      return chrome9_fs_smooth_pixel_load(intr, location);
   case nir_intrinsic_load_deref:
      var = nir_intrinsic_get_var(intr, 0);
      return chrome9_fs_smooth_variable(var, location);
   default:
      return false;
   }
}

static bool
chrome9_fs_is_input_component(const nir_alu_src *src, unsigned location,
                              unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   unsigned lane;

   if (depth > 12 || !def)
      return false;
   lane = src->swizzle[0];
   if (chrome9_fs_is_input_at(def, location))
      return lane == component && component < def->num_components;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov) {
      nir_alu_src next = alu->src[0];

      next.swizzle[0] = alu->src[0].swizzle[lane];
      return chrome9_fs_is_input_component(&next, location, component,
                                            depth + 1);
   }
   if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
       alu->op == nir_op_vec4) {
      nir_alu_src next;

      if (lane >= nir_op_infos[alu->op].num_inputs)
         return false;
      next = alu->src[lane];
      return chrome9_fs_is_input_component(&next, location, component,
                                            depth + 1);
   }
   return false;
}

static bool
chrome9_fs_texture_component_base(const nir_alu_src *src, unsigned component,
                                  nir_def **base, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   unsigned lane;

   if (depth > 12 || !def)
      return false;
   lane = src->swizzle[0];
   if (def->num_components == 4 && lane == component &&
       chrome9_fs_is_texture(def, 0)) {
      *base = def;
      return true;
   }
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_mov)
      return false;
   nir_alu_src next = alu->src[0];

   next.swizzle[0] = alu->src[0].swizzle[lane];
   return chrome9_fs_texture_component_base(&next, component, base,
                                             depth + 1);
}

static bool
chrome9_fs_is_packed_texture_primary_component(const nir_alu_src *src,
                                               unsigned texture_component,
                                               unsigned input_location,
                                               unsigned input_component,
                                               nir_def **texture,
                                               unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   nir_alu_src a, b;
   unsigned lane;

   if (depth > 12 || !def)
      return false;
   lane = src->swizzle[0];
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov) {
      nir_alu_src next = alu->src[0];

      next.swizzle[0] = alu->src[0].swizzle[lane];
      return chrome9_fs_is_packed_texture_primary_component(
         &next, texture_component, input_location, input_component,
         texture, depth + 1);
   }
   if (alu->op != nir_op_fmul)
      return false;
   a = alu->src[0];
   b = alu->src[1];
   a.swizzle[0] = alu->src[0].swizzle[lane];
   b.swizzle[0] = alu->src[1].swizzle[lane];
   return (chrome9_fs_texture_component_base(&a, texture_component,
                                              texture, depth + 1) &&
           chrome9_fs_is_input_component(&b, input_location,
                                          input_component, depth + 1)) ||
          (chrome9_fs_texture_component_base(&b, texture_component,
                                              texture, depth + 1) &&
           chrome9_fs_is_input_component(&a, input_location,
                                          input_component, depth + 1));
}

static bool
chrome9_fs_is_ioquake3_generic_result(nir_def *def, unsigned depth)
{
   static const unsigned location[4] = {
      VARYING_SLOT_VAR0, VARYING_SLOT_VAR0,
      VARYING_SLOT_VAR1, VARYING_SLOT_VAR1,
   };
   static const unsigned component[4] = { 2, 3, 0, 1 };
   nir_instr *instr;
   nir_def *texture = NULL;

   if (depth > 12 || def->num_components != 4)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_ioquake3_generic_result(alu->src[0].src.ssa,
                                                    depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 4; c++) {
      nir_def *component_texture = NULL;

      if (!chrome9_fs_is_packed_texture_primary_component(
             &alu->src[c], c, location[c], component[c],
             &component_texture, depth + 1) ||
          (texture && texture != component_texture))
         return false;
      texture = component_texture;
   }
   return texture != NULL;
}

static bool
chrome9_fs_is_ioquake3_generic_packed(nir_shader *nir)
{
   unsigned derefs = 0, loads = 0, uniforms = 0, terminates = 0, stores = 0;
   unsigned constants = 0, textures = 0;
   unsigned vec2 = 0, vec4 = 0, fmul = 0, ieq = 0;
   unsigned feq = 0, fge = 0, flt = 0, iand = 0;
   unsigned constant_mask = 0;
   nir_def *result = NULL;

   if (nir->info.stage != MESA_SHADER_FRAGMENT ||
       nir->info.num_textures != 1 || !nir->info.fs.uses_discard)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_deref:
               derefs++;
               break;
            case nir_instr_type_load_const: {
               nir_load_const_instr *load = nir_instr_as_load_const(instr);
               uint32_t value;

               if (load->def.num_components != 1 || load->def.bit_size != 32)
                  return false;
               value = load->value[0].u32;
               if (value == 0)
                  constant_mask |= 1u << 0;
               else if (value == 1)
                  constant_mask |= 1u << 1;
               else if (value == 2)
                  constant_mask |= 1u << 2;
               else if (value == 3)
                  constant_mask |= 1u << 3;
               else if (value == 0x3f000000)
                  constant_mask |= 1u << 4;
               else
                  return false;
               constants++;
               break;
            }
            case nir_instr_type_tex:
               if (!chrome9_fs_is_texture(&nir_instr_as_tex(instr)->def, 0))
                  return false;
               textures++;
               break;
            case nir_instr_type_alu: {
               nir_op op = nir_instr_as_alu(instr)->op;

               if (op == nir_op_vec2)
                  vec2++;
               else if (op == nir_op_vec4)
                  vec4++;
               else if (op == nir_op_fmul)
                  fmul++;
               else if (op == nir_op_ieq)
                  ieq++;
               else if (op == nir_op_feq)
                  feq++;
               else if (op == nir_op_fge)
                  fge++;
               else if (op == nir_op_flt)
                  flt++;
               else if (op == nir_op_iand)
                  iand++;
               else
                  return false;
               break;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               if (intr->intrinsic == nir_intrinsic_load_deref) {
                  loads++;
               } else if (intr->intrinsic == nir_intrinsic_load_uniform) {
                  if (!nir_src_is_const(intr->src[0]) ||
                      nir_src_as_uint(intr->src[0]) ||
                      nir_intrinsic_base(intr) || intr->def.num_components != 1)
                     return false;
                  uniforms++;
               } else if (intr->intrinsic == nir_intrinsic_terminate_if) {
                  terminates++;
               } else if (intr->intrinsic == nir_intrinsic_store_deref) {
                  nir_variable *var = nir_intrinsic_get_var(intr, 0);

                  if (!var || var->data.mode != nir_var_shader_out ||
                      (var->data.location != FRAG_RESULT_COLOR &&
                       var->data.location != FRAG_RESULT_DATA0) || stores++)
                     return false;
                  result = intr->src[1].ssa;
               } else {
                  return false;
               }
               break;
            }
            default:
               return false;
            }
         }
      }
   }
   return derefs == 3 && loads == 2 && uniforms == 1 && terminates == 3 &&
          stores == 1 && constants == 5 && constant_mask == 0x1f &&
          textures == 1 && vec2 == 1 && vec4 == 2 && fmul == 2 &&
          ieq == 3 && feq == 1 && fge == 1 && flt == 1 && iand == 1 &&
          result && chrome9_fs_is_ioquake3_generic_result(result, 0);
}

static bool
chrome9_fs_is_ioquake3_packed_texture(nir_def *def, unsigned unit)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   nir_instr *coord_instr;
   nir_alu_instr *coord_alu;
   int coord;
   unsigned first = unit ? 2 : 0;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->is_array || tex->is_shadow || tex->texture_index != unit ||
       tex->sampler_index != unit || tex->coord_components != 2 || coord < 0 ||
       tex->num_srcs != 1)
      return false;
   coord_instr = nir_def_instr(tex->src[coord].src.ssa);
   if (coord_instr->type != nir_instr_type_alu)
      return false;
   coord_alu = nir_instr_as_alu(coord_instr);
   return coord_alu->op == nir_op_vec2 &&
          chrome9_fs_is_input_component(&coord_alu->src[0],
                                        VARYING_SLOT_VAR0, first, 0) &&
          chrome9_fs_is_input_component(&coord_alu->src[1],
                                        VARYING_SLOT_VAR0, first + 1, 0);
}

static unsigned
chrome9_fs_ioquake3_lightmap_factors(const nir_alu_src *src,
                                     unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   unsigned lane;

   if (depth > 12 || !def)
      return 0;
   lane = src->swizzle[0];
   if (def->num_components == 4 && lane == component) {
      if (chrome9_fs_is_ioquake3_packed_texture(def, 0))
         return 1u << 0;
      if (chrome9_fs_is_ioquake3_packed_texture(def, 1))
         return 1u << 1;
   }
   if (chrome9_fs_is_input_component(src, VARYING_SLOT_VAR1, component, 0))
      return 1u << 2;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return 0;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov) {
      nir_alu_src next = alu->src[0];

      next.swizzle[0] = alu->src[0].swizzle[lane];
      return chrome9_fs_ioquake3_lightmap_factors(&next, component,
                                                  depth + 1);
   }
   if (alu->op == nir_op_fmul) {
      nir_alu_src a = alu->src[0];
      nir_alu_src b = alu->src[1];
      unsigned ma, mb;

      /* The fixed program evaluates (tex0 * tex1) * primary.  Admit the
       * captured tex0 * (primary * tex1) tree only when NIR explicitly
       * permits reassociation. */
      if (nir_alu_instr_no_reassoc(alu))
         return 0;
      a.swizzle[0] = alu->src[0].swizzle[lane];
      b.swizzle[0] = alu->src[1].swizzle[lane];
      ma = chrome9_fs_ioquake3_lightmap_factors(&a, component, depth + 1);
      mb = chrome9_fs_ioquake3_lightmap_factors(&b, component, depth + 1);
      return ma && mb && !(ma & mb) ? ma | mb : 0;
   }
   return 0;
}

static bool
chrome9_fs_is_ioquake3_lightmap_result(nir_def *def, unsigned depth)
{
   nir_instr *instr;

   if (depth > 12 || def->num_components != 4)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_ioquake3_lightmap_result(alu->src[0].src.ssa,
                                                     depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++)
      if (chrome9_fs_ioquake3_lightmap_factors(&alu->src[c], c,
                                               depth + 1) != 0x7)
         return false;
   return chrome9_fs_ioquake3_lightmap_factors(&alu->src[3], 3,
                                                depth + 1) == 0x5;
}

static bool
chrome9_fs_is_ioquake3_lightmap_packed(nir_shader *nir)
{
   unsigned derefs = 0, loads = 0, uniforms = 0, terminates = 0, stores = 0;
   unsigned constants = 0, textures = 0, texture_mask = 0;
   unsigned vec2 = 0, vec3 = 0, vec4 = 0, fmul = 0, ieq = 0;
   unsigned feq = 0, fge = 0, flt = 0, iand = 0;
   unsigned constant_mask = 0;
   unsigned fmul_no_reassoc = 0, fmul_ctrl = 0;
   nir_def *result = NULL;

   if (nir->info.stage != MESA_SHADER_FRAGMENT ||
       nir->info.num_textures != 2 || !nir->info.fs.uses_discard)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_deref:
               derefs++;
               break;
            case nir_instr_type_load_const: {
               nir_load_const_instr *load = nir_instr_as_load_const(instr);
               uint32_t value;

               if (load->def.num_components != 1 || load->def.bit_size != 32)
                  return false;
               value = load->value[0].u32;
               if (value == 0)
                  constant_mask |= 1u << 0;
               else if (value == 1)
                  constant_mask |= 1u << 1;
               else if (value == 2)
                  constant_mask |= 1u << 2;
               else if (value == 3)
                  constant_mask |= 1u << 3;
               else if (value == 0x3f000000)
                  constant_mask |= 1u << 4;
               else
                  return false;
               constants++;
               break;
            }
            case nir_instr_type_tex: {
               nir_tex_instr *tex = nir_instr_as_tex(instr);

               if (tex->texture_index > 1 ||
                   !chrome9_fs_is_ioquake3_packed_texture(&tex->def,
                                                          tex->texture_index))
                  return false;
               texture_mask |= 1u << tex->texture_index;
               textures++;
               break;
            }
            case nir_instr_type_alu: {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               nir_op op = alu->op;

               if (op == nir_op_vec2)
                  vec2++;
               else if (op == nir_op_vec3)
                  vec3++;
               else if (op == nir_op_vec4)
                  vec4++;
               else if (op == nir_op_fmul) {
                  fmul++;
                  fmul_ctrl |= alu->fp_math_ctrl;
                  fmul_no_reassoc += nir_alu_instr_no_reassoc(alu);
               }
               else if (op == nir_op_ieq)
                  ieq++;
               else if (op == nir_op_feq)
                  feq++;
               else if (op == nir_op_fge)
                  fge++;
               else if (op == nir_op_flt)
                  flt++;
               else if (op == nir_op_iand)
                  iand++;
               else
                  return false;
               break;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               if (intr->intrinsic == nir_intrinsic_load_deref) {
                  loads++;
               } else if (intr->intrinsic == nir_intrinsic_load_uniform) {
                  if (!nir_src_is_const(intr->src[0]) ||
                      nir_src_as_uint(intr->src[0]) ||
                      nir_intrinsic_base(intr) || intr->def.num_components != 1)
                     return false;
                  uniforms++;
               } else if (intr->intrinsic == nir_intrinsic_terminate_if) {
                  terminates++;
               } else if (intr->intrinsic == nir_intrinsic_store_deref) {
                  nir_variable *var = nir_intrinsic_get_var(intr, 0);

                  if (!var || var->data.mode != nir_var_shader_out ||
                      (var->data.location != FRAG_RESULT_COLOR &&
                       var->data.location != FRAG_RESULT_DATA0) || stores++)
                     return false;
                  result = intr->src[1].ssa;
               } else {
                  return false;
               }
               break;
            }
            default:
               return false;
            }
         }
      }
   }
   bool result_match = result &&
      chrome9_fs_is_ioquake3_lightmap_result(result, 0);
   bool match = derefs == 3 && loads == 2 && uniforms == 1 &&
      terminates == 3 && stores == 1 && constants == 5 &&
      constant_mask == 0x1f && textures == 2 && texture_mask == 0x3 &&
      vec2 == 2 && vec3 == 0 && vec4 == 1 && fmul == 3 && ieq == 3 &&
      feq == 1 && fge == 1 && flt == 1 && iand == 1 && result_match;

   if (debug_get_bool_option("CHROME9_DEBUG_LIGHTMAP_MATCH", false))
      fprintf(stderr, "chrome9: lightmap match=%u result=%u "
              "deref=%u load=%u uniform=%u term=%u store=%u const=%u/%#x "
              "tex=%u/%#x vec=%u/%u/%u mul=%u(no-reassoc=%u ctrl=%#x) "
              "cmp=%u/%u/%u/%u/%u\n",
              match, result_match, derefs, loads, uniforms, terminates, stores,
              constants, constant_mask, textures, texture_mask, vec2, vec3,
              vec4, fmul, fmul_no_reassoc, fmul_ctrl, ieq, feq, fge, flt,
              iand);
   return match;
}

static bool
chrome9_fs_pair(unsigned a, unsigned b, unsigned x, unsigned y)
{
   return (a == x && b == y) || (a == y && b == x);
}

static unsigned
chrome9_fs_result(nir_def *def, uint32_t *literal_color)
{
   nir_instr *instr;
   nir_alu_instr *alu;
   nir_def *generic = NULL;
   unsigned a, b;

   if (chrome9_fs_is_flat_primary_color(def, 0))
      return CHROME9_FS_PRIMARY_COLOR;

   if (chrome9_fs_is_literal_color(def, literal_color))
      return CHROME9_FS_LITERAL_COLOR;

   if (chrome9_fs_is_flat_generic0_rgbx(def, 0))
      return CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX;

   if (chrome9_fs_is_generic0_rgba(def, 0))
      return CHROME9_FS_PRIMARY_GENERIC_RGBA;
   if (chrome9_fs_is_generic0_blue_rgbx(def, &generic, 0))
      return CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX;
   if (chrome9_fs_is_generic0_vec3_rgbx(def, &generic, 0))
      return CHROME9_FS_PRIMARY_GENERIC_RGBX;
   if (chrome9_fs_is_generic0_xxy_rgbx(def, &generic, 0))
      return CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX;
   if (chrome9_fs_is_generic0_rgbx(def, &generic, 0) &&
       generic->num_components == 1)
      return CHROME9_FS_PRIMARY_SCALAR_RGBX;
   if (chrome9_fs_is_packed_gray_modulate(def))
      return CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE;
   if (chrome9_fs_is_projected_constant_product(def))
      return CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE;
   if (chrome9_fs_is_projected_constant_rgb_alpha(def, 0))
      return CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA;
   if (chrome9_fs_is_projected_primary_rgb_alpha(def, 0))
      return CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA;
   if (chrome9_fs_is_two_projected_primary_rgb_alpha(def, 0))
      return CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA;
   if (chrome9_fs_is_two_projected_texture0_alpha_primary(def, 0))
      return CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY;
   if (chrome9_fs_is_projected_primary_product(def))
      return CHROME9_FS_TEXTURE_PROJECTED_MODULATE;
   if (chrome9_fs_is_rgbx(def, 0))
      return CHROME9_FS_TEXTURE_RGBX;
   switch (chrome9_fs_leaf(def, 0)) {
   case CHROME9_FS_LEAF_PRIMARY:
      return CHROME9_FS_PRIMARY_COLOR;
   case CHROME9_FS_LEAF_CONSTANT0:
      return CHROME9_FS_CONSTANT_COLOR;
   case CHROME9_FS_LEAF_TEXTURE0:
      return CHROME9_FS_TEXTURE_REPLACE;
   default:
      break;
   }
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return CHROME9_FS_UNSUPPORTED;
   a = chrome9_fs_source_leaf(&alu->src[0], 0);
   b = chrome9_fs_source_leaf(&alu->src[1], 0);
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_PRIMARY))
      return CHROME9_FS_TEXTURE_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_CONSTANT0))
      return CHROME9_FS_TEXTURE_CONSTANT_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_CONSTANT0_ALPHA))
      return CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_TEXTURE1))
      return CHROME9_FS_TWO_TEXTURE_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_CONSTANT0,
                       CHROME9_FS_LEAF_CONSTANT1))
      return CHROME9_FS_CONSTANT_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_CONSTANT0,
                       CHROME9_FS_LEAF_CONSTANT1_ALPHA))
      return CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_CONSTANT0_ALPHA,
                       CHROME9_FS_LEAF_CONSTANT1))
      return CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE;
   return CHROME9_FS_UNSUPPORTED;
}

static unsigned
chrome9_fs_classify(nir_shader *nir, uint32_t *literal_color)
{
   unsigned result = CHROME9_FS_UNSUPPORTED;
   unsigned texture_count = 0;
   unsigned constant_count = 0;
   bool written = false, valid = true;

   /* Linked generic_fp packs texture ST in VAR0.xy and primary RGBA in
    * VAR0.zw/VAR1.xy.  The semantic matcher closes both the color DAG and
    * complete integer alpha-test tree; draw time admits proven modes 0/1. */
   if (chrome9_fs_is_ioquake3_generic_packed(nir))
      return CHROME9_FS_IOQUAKE3_GENERIC_PACKED;
   if (chrome9_fs_is_ioquake3_lightmap_packed(nir))
      return CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            nir_variable *var;
            unsigned location;

            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               nir_def *packed_base = NULL;

               texture_count++;
               if ((!chrome9_fs_is_texture(&tex->def, tex->texture_index) &&
                    !chrome9_fs_is_projected_texture(&tex->def,
                                                     tex->texture_index) &&
                    !chrome9_fs_is_packed_texture(&tex->def,
                                                  &packed_base)) ||
                   tex->texture_index > 1)
                  valid = false;
               continue;
            }
            if (instr->type == nir_instr_type_deref ||
                instr->type == nir_instr_type_load_const ||
                instr->type == nir_instr_type_alu)
               continue;
            if (instr->type != nir_instr_type_intrinsic) {
               valid = false;
               continue;
            }
            intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_uniform:
            case nir_intrinsic_load_ubo:
            case nir_intrinsic_load_ubo_vec4:
               constant_count++;
               break;
            case nir_intrinsic_load_input:
               /* With pipe_caps.flatshade=false, state tracker flat color
                * arrives as a direct load.  The immediate packet currently
                * emits Gouraud A23 state and has no provoking-vertex fixup,
                * so fail closed until that topology-aware path exists. */
               valid = false;
               break;
            case nir_intrinsic_load_interpolated_input:
            case nir_intrinsic_load_barycentric_pixel:
            case nir_intrinsic_load_barycentric_centroid:
            case nir_intrinsic_load_barycentric_sample:
            case nir_intrinsic_load_deref:
               break;
            case nir_intrinsic_store_output:
               location = nir_intrinsic_io_semantics(intr).location;
               if ((location != FRAG_RESULT_COLOR &&
                    location != FRAG_RESULT_DATA0) ||
                   written ||
                   !(result = chrome9_fs_result(intr->src[0].ssa,
                                                literal_color)))
                  valid = false;
               written = true;
               break;
            case nir_intrinsic_store_deref:
               var = nir_intrinsic_get_var(intr, 0);
               if (!var || var->data.mode != nir_var_shader_out ||
                   (var->data.location != FRAG_RESULT_COLOR &&
                    var->data.location != FRAG_RESULT_DATA0) ||
                   written ||
                   !(result = chrome9_fs_result(intr->src[1].ssa,
                                                literal_color)))
                  valid = false;
               written = true;
               break;
            default:
               valid = false;
               break;
            }
         }
      }
   }
   if (!valid || !written)
      return CHROME9_FS_UNSUPPORTED;
   switch (result) {
   case CHROME9_FS_PRIMARY_COLOR:
   case CHROME9_FS_PRIMARY_GENERIC_RGBA:
   case CHROME9_FS_PRIMARY_GENERIC_RGBX:
   case CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX:
   case CHROME9_FS_PRIMARY_SCALAR_RGBX:
   case CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX:
   case CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX:
   case CHROME9_FS_LITERAL_COLOR:
      valid = !texture_count && !constant_count;
      break;
   case CHROME9_FS_CONSTANT_COLOR:
      valid = !texture_count && constant_count == 1;
      break;
   case CHROME9_FS_CONSTANT_MODULATE:
   case CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE:
   case CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE:
      valid = !texture_count && constant_count == 2;
      break;
   case CHROME9_FS_TEXTURE_REPLACE:
   case CHROME9_FS_TEXTURE_RGBX:
   case CHROME9_FS_TEXTURE_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA:
   case CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA:
   case CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE:
      valid = texture_count == 1 &&
              ((result == CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
                result == CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA) ?
                 constant_count == 1 : !constant_count);
      break;
   case CHROME9_FS_TEXTURE_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE:
      valid = texture_count == 1 && constant_count == 1;
      break;
   case CHROME9_FS_TWO_TEXTURE_MODULATE:
   case CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA:
   case CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY:
      valid = texture_count == 2 && !constant_count;
      break;
   default:
      valid = false;
      break;
   }
   if (!valid)
      return CHROME9_FS_UNSUPPORTED;
   return result;
}

static bool
chrome9_fs_native_key(unsigned program, struct chrome9_fs_key *key)
{
   memset(key, 0, sizeof(*key));

   switch (program) {
   case CHROME9_FS_PRIMARY_COLOR:
      key->op = CHROME9_FS_OP_MOV;
      key->src[0] = CHROME9_FS_LEAF_V0;
      return true;
   case CHROME9_FS_CONSTANT_COLOR:
      key->op = CHROME9_FS_OP_MOV;
      key->src[0] = CHROME9_FS_LEAF_C0;
      return true;
   case CHROME9_FS_CONSTANT_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_C0;
      key->src[1] = CHROME9_FS_LEAF_C1;
      return true;
   case CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_C0;
      key->src[1] = CHROME9_FS_LEAF_C1_A;
      return true;
   case CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_C0_A;
      key->src[1] = CHROME9_FS_LEAF_C1;
      return true;
   case CHROME9_FS_TEXTURE_REPLACE:
      key->op = CHROME9_FS_OP_MOV;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      return true;
   case CHROME9_FS_TEXTURE_RGBX:
      key->op = CHROME9_FS_OP_RGBX_C0X;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_C0;
      return true;
   case CHROME9_FS_TEXTURE_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_V0;
      return true;
   case CHROME9_FS_TEXTURE_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_C0;
      return true;
   case CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_C0_A;
      return true;
   case CHROME9_FS_TWO_TEXTURE_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_TEX1;
      return true;
   default:
      return false;
   }
}

static bool
chrome9_backend_alpha_leaf(enum chrome9_fs_leaf *leaf)
{
   switch (*leaf) {
   case CHROME9_FS_LEAF_V0:
      *leaf = CHROME9_FS_LEAF_V0_A;
      return true;
   case CHROME9_FS_LEAF_C0:
      *leaf = CHROME9_FS_LEAF_C0_A;
      return true;
   case CHROME9_FS_LEAF_C1:
      *leaf = CHROME9_FS_LEAF_C1_A;
      return true;
   default:
      return false;
   }
}

static bool
chrome9_backend_primary(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *load;
   nir_deref_instr *deref;
   nir_variable *var;

   if (def->num_components != 4 || def->bit_size != 32 ||
       instr->type != nir_instr_type_intrinsic)
      return false;
   load = nir_instr_as_intrinsic(instr);
   if (load->intrinsic == nir_intrinsic_load_deref) {
      deref = nir_src_as_deref(load->src[0]);
      var = nir_intrinsic_get_var(load, 0);
      return deref && deref->deref_type == nir_deref_type_var &&
             chrome9_fs_smooth_variable(var, VARYING_SLOT_COL0);
   }
   return chrome9_fs_smooth_pixel_load(load, VARYING_SLOT_COL0);
}

/* The backend deliberately unwraps MOV only.  Treating FSAT as a copy would
 * miscompile an FMUL operand whenever the source lies outside [0,1]. */
static bool
chrome9_backend_leaf(nir_def *def, unsigned depth,
                     enum chrome9_fs_leaf *leaf)
{
   nir_instr *instr;

   if (!def || depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      bool alpha = true, identity = true;

      if (alu->op != nir_op_mov)
         return false;
      for (unsigned c = 0; c < 4; c++) {
         identity &= alu->src[0].swizzle[c] == c;
         alpha &= alu->src[0].swizzle[c] == 3;
      }
      if ((!identity && !alpha) ||
          !chrome9_backend_leaf(alu->src[0].src.ssa, depth + 1, leaf))
         return false;
      return identity || chrome9_backend_alpha_leaf(leaf);
   }
   if (chrome9_backend_primary(def)) {
      *leaf = CHROME9_FS_LEAF_V0;
      return true;
   }
   if (chrome9_fs_is_constant_color(def, 0, 0)) {
      *leaf = CHROME9_FS_LEAF_C0;
      return true;
   }
   if (chrome9_fs_is_constant_color(def, 1, 0)) {
      *leaf = CHROME9_FS_LEAF_C1;
      return true;
   }
   return false;
}

static bool
chrome9_backend_source(const nir_alu_src *source,
                       enum chrome9_fs_leaf *leaf)
{
   bool alpha = true, identity = true;

   if (!source->src.ssa || source->src.ssa->num_components != 4)
      return false;
   for (unsigned c = 0; c < 4; c++) {
      identity &= source->swizzle[c] == c;
      alpha &= source->swizzle[c] == 3;
   }
   if ((!identity && !alpha) ||
       !chrome9_backend_leaf(source->src.ssa, 0, leaf))
      return false;
   return identity || chrome9_backend_alpha_leaf(leaf);
}

struct chrome9_ps_proof {
   unsigned budget;
   void *mem_ctx;
   struct hash_table_u64 *memo;
};

/* A lowered Boolean: (negate ? -temp : temp) >= 0, complemented by invert.
 * CMP evaluates it with its qualified A-operand NEG. */
struct chrome9_ps_condition {
   uint8_t temp;
   bool negate;
   bool invert;
};

struct chrome9_ps_nir_builder {
   struct chrome9_ps_ir ir;
   struct chrome9_ps_proof proof;
   struct {
      nir_def *def;
      struct chrome9_ps_source source;
      bool complete;
   } value[CHROME9_PS_MAX_INSTRUCTIONS];
   uint32_t immediate[32][4];
   uint32_t immediate_mask;
   uint32_t uniform_mask;
   uint8_t immediate_lane_mask[32];
   uint8_t value_count;
   uint8_t next_temp;
   uint8_t alu_count;
   uint8_t texture_count;
   uint32_t equal_zero_input_mask;
   uint32_t identity_projector_input_mask;
   uint32_t unit_interval_input_mask;
   uint8_t cube_sampler_mask;
   uint8_t volume_sampler_mask;
   bool allow_bias;
   struct {
      nir_scalar predicate;
      struct chrome9_ps_condition condition;
   } condition[CHROME9_PS_MAX_INSTRUCTIONS];
   uint8_t condition_count;
};

static struct chrome9_ps_instruction *
chrome9_ps_append_instruction(struct chrome9_ps_nir_builder *builder,
                              enum chrome9_ps_opcode op)
{
   struct chrome9_ps_instruction *instruction;

   if (builder->ir.count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return NULL;
   if (op == CHROME9_PS_OPCODE_TEX || op == CHROME9_PS_OPCODE_KILL) {
      if (builder->texture_count >= CHROME9_PS_MAX_TAU)
         return NULL;
      builder->texture_count++;
   }
   if (op != CHROME9_PS_OPCODE_TEX) {
      if (builder->alu_count >= CHROME9_PS_MAX_ALU)
         return NULL;
      builder->alu_count++;
   }
   instruction = &builder->ir.instruction[builder->ir.count++];
   memset(instruction, 0, sizeof(*instruction));
   instruction->op = op;
   return instruction;
}

static bool
chrome9_ps_source_from_leaf(enum chrome9_fs_leaf leaf,
                            struct chrome9_ps_source *source)
{
   bool alpha = false;

   memset(source, 0, sizeof(*source));
   switch (leaf) {
   case CHROME9_FS_LEAF_V0_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_V0:
      source->file = CHROME9_PS_SOURCE_INPUT;
      source->id = 0;
      break;
   case CHROME9_FS_LEAF_V1_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_V1:
      source->file = CHROME9_PS_SOURCE_INPUT;
      source->id = 1;
      break;
   case CHROME9_FS_LEAF_C0_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_C0:
      source->file = CHROME9_PS_SOURCE_CONSTANT;
      source->id = 0;
      break;
   case CHROME9_FS_LEAF_C1_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_C1:
      source->file = CHROME9_PS_SOURCE_CONSTANT;
      source->id = 1;
      break;
   default:
      return false;
   }
   for (unsigned c = 0; c < 4; c++)
      source->swizzle[c] = alpha ? CHROME9_PS_SWIZZLE_W : c;
   return true;
}

static bool chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder,
                                 nir_def *def,
                                 struct chrome9_ps_source *source);
static bool chrome9_ps_lower_round(struct chrome9_ps_nir_builder *builder,
                                   nir_alu_instr *alu, nir_def *def,
                                   struct chrome9_ps_source *source);

static bool
chrome9_ps_alu_src_is_fneg(const nir_alu_src *source)
{
   nir_instr *instr = nir_def_instr(source->src.ssa);

   return instr->type == nir_instr_type_alu &&
          nir_instr_as_alu(instr)->op == nir_op_fneg;
}

static bool
chrome9_ps_lower_alu_source_depth(struct chrome9_ps_nir_builder *builder,
                                  const nir_alu_src *nir_source,
                                  unsigned components,
                                  struct chrome9_ps_source *source,
                                  unsigned depth,
                                  bool allow_modifier)
{
   struct chrome9_ps_source base;

   if (!nir_source->src.ssa || !components || components > 4 || depth > 8 ||
       nir_source->src.ssa->bit_size != 32)
      return false;
   nir_instr *instr = nir_def_instr(nir_source->src.ssa);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_fneg) {
         nir_alu_src folded = alu->src[0];

         for (unsigned c = 0; c < components; c++) {
            unsigned component = nir_source->swizzle[c];

            if (component >= alu->def.num_components)
               return false;
            folded.swizzle[c] = alu->src[0].swizzle[component];
         }
         /* Without a modifier only a double negation folds; do not lower
          * the operand ahead of the negation's own lowering. */
         if ((allow_modifier || chrome9_ps_alu_src_is_fneg(&folded)) &&
             chrome9_ps_lower_alu_source_depth(builder, &folded, components,
                                               source, depth + 1,
                                               allow_modifier)) {
            uint8_t modifier = source->modifier ==
               CHROME9_PS_SOURCE_NEGATE ? 0 : CHROME9_PS_SOURCE_NEGATE;

            /* MISC/POSITION has no qualified source-modifier encoding.  Its
             * fneg keeps the existing MUL(-1) fallback instead. */
            if ((!modifier || allow_modifier) &&
                (source->file != CHROME9_PS_SOURCE_POSITION || !modifier)) {
               source->modifier = modifier;
               return true;
            }
         }
      }
   }
   if (!chrome9_ps_lower_def(builder, nir_source->src.ssa, &base))
      return false;
   *source = base;
   for (unsigned c = 0; c < components; c++) {
      unsigned swizzle = nir_source->swizzle[c];

      if (swizzle >= nir_source->src.ssa->num_components)
         return false;
      source->swizzle[c] = base.swizzle[swizzle];
   }
   for (unsigned c = components; c < 4; c++)
      source->swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
   return true;
}

static bool
chrome9_ps_lower_alu_source(struct chrome9_ps_nir_builder *builder,
                            const nir_alu_src *nir_source,
                            unsigned components,
                            struct chrome9_ps_source *source)
{
   return chrome9_ps_lower_alu_source_depth(builder, nir_source, components,
                                             source, 0, false);
}

static bool
chrome9_ps_lower_alu_source_modifier(struct chrome9_ps_nir_builder *builder,
                                     const nir_alu_src *nir_source,
                                     unsigned components,
                                     struct chrome9_ps_source *source)
{
   return chrome9_ps_lower_alu_source_depth(builder, nir_source, components,
                                             source, 0, true);
}

static bool
chrome9_ps_source_modifier_allowed(enum chrome9_ps_opcode op,
                                   unsigned source)
{
   /* NEG is silicon-qualified on A of every op but FRC (FRC(-0) is 1.0)
    * and on B of ADD, SUB, MAD and CMP; C has none.  CHROME9_PS_NEG_SLOTS=0
    * keeps only the MAD/CMP A operand. */
   static int slots = -1;

   if (slots < 0)
      slots = debug_get_bool_option("CHROME9_PS_NEG_SLOTS", true);
   switch (op) {
   case CHROME9_PS_OPCODE_MAD:
   case CHROME9_PS_OPCODE_CMP:
      return source == 0 || (slots && source == 2);
   case CHROME9_PS_OPCODE_ADD:
   case CHROME9_PS_OPCODE_SUB:
      return slots;
   case CHROME9_PS_OPCODE_MOV:
   case CHROME9_PS_OPCODE_MUL:
   case CHROME9_PS_OPCODE_MIN:
   case CHROME9_PS_OPCODE_MAX:
   case CHROME9_PS_OPCODE_DP3:
   case CHROME9_PS_OPCODE_DP4:
   case CHROME9_PS_OPCODE_EXP2:
   case CHROME9_PS_OPCODE_LOG2:
   case CHROME9_PS_OPCODE_RCP:
   case CHROME9_PS_OPCODE_RSQ:
      return slots && source == 0;
   default:
      return false;
   }
}

/* Commutative factors: move a negated operand out of a NEG-less slot. */
static void
chrome9_ps_source_order(enum chrome9_ps_opcode op, const nir_alu_src *src,
                        unsigned count, unsigned order[3])
{
   for (unsigned s = 0; s < 3; s++)
      order[s] = s;
   switch (op) {
   case CHROME9_PS_OPCODE_MUL:
   case CHROME9_PS_OPCODE_ADD:
   case CHROME9_PS_OPCODE_MAD:
   case CHROME9_PS_OPCODE_MIN:
   case CHROME9_PS_OPCODE_MAX:
   case CHROME9_PS_OPCODE_DP3:
   case CHROME9_PS_OPCODE_DP4:
      break;
   default:
      return;
   }
   if (count >= 2 && !chrome9_ps_source_modifier_allowed(op, 1) &&
       chrome9_ps_source_modifier_allowed(op, 0) &&
       chrome9_ps_alu_src_is_fneg(&src[1]) &&
       !chrome9_ps_alu_src_is_fneg(&src[0])) {
      order[0] = 1;
      order[1] = 0;
   }
}

static bool
chrome9_ps_immediate(struct chrome9_ps_nir_builder *builder,
                     const uint32_t value[4], unsigned components,
                     struct chrome9_ps_source *source);

/* Resolve a NEG modifier the consumer slot cannot encode into a temp. */
static bool
chrome9_ps_settle_modifier(struct chrome9_ps_nir_builder *builder,
                           enum chrome9_ps_opcode op, unsigned slot,
                           struct chrome9_ps_source *source)
{
   static const uint32_t negative_one[4] = {
      0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000,
   };
   struct chrome9_ps_instruction *instruction;
   struct chrome9_ps_source operand = *source;

   /* MISC/POSITION has no qualified source-modifier encoding. */
   if (!source->modifier ||
       (chrome9_ps_source_modifier_allowed(op, slot) &&
        source->file != CHROME9_PS_SOURCE_POSITION))
      return true;
   if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   if (chrome9_ps_source_modifier_allowed(CHROME9_PS_OPCODE_MOV, 0) &&
       source->file != CHROME9_PS_SOURCE_POSITION) {
      instruction = chrome9_ps_append_instruction(builder,
                                                  CHROME9_PS_OPCODE_MOV);
      if (!instruction)
         return false;
      instruction->source[0] = operand;
   } else {
      struct chrome9_ps_source minus;

      operand.modifier = 0;
      if (!chrome9_ps_immediate(builder, negative_one, 4, &minus))
         return false;
      instruction = chrome9_ps_append_instruction(builder,
                                                  CHROME9_PS_OPCODE_MUL);
      if (!instruction)
         return false;
      instruction->source[0] = operand;
      instruction->source[1] = minus;
   }
   instruction->destination.file = CHROME9_PS_DESTINATION_TEMP;
   instruction->destination.id = builder->next_temp++;
   instruction->destination.writemask = 0xf;
   memset(source, 0, sizeof(*source));
   source->file = CHROME9_PS_SOURCE_TEMP;
   source->id = instruction->destination.id;
   for (unsigned c = 0; c < 4; c++)
      source->swizzle[c] = c;
   return true;
}

/* -(a*b), -(a*b+c), -(a+b) and -(a-b) of a single-use, not yet lowered
 * value: the negation moves onto source modifiers of that one record. */
static bool
chrome9_ps_fold_negation(const struct chrome9_ps_nir_builder *builder,
                         const nir_alu_instr *negation, unsigned components,
                         nir_alu_src folded[3], enum chrome9_ps_opcode *op,
                         unsigned *count, unsigned *negate)
{
   nir_instr *instr = nir_def_instr(negation->src[0].src.ssa);
   enum chrome9_ps_opcode native;
   unsigned sources, mask;
   nir_alu_instr *inner;

   if (instr->type != nir_instr_type_alu)
      return false;
   inner = nir_instr_as_alu(instr);
   if (!list_is_singular(&inner->def.uses) || inner->def.bit_size != 32)
      return false;
   for (unsigned v = 0; v < builder->value_count; v++)
      if (builder->value[v].def == &inner->def)
         return false;
   switch (inner->op) {
   case nir_op_fmul:
      native = CHROME9_PS_OPCODE_MUL;
      sources = 2;
      mask = 1;
      break;
   case nir_op_ffma:
      native = CHROME9_PS_OPCODE_MAD;
      sources = 3;
      mask = 5;
      break;
   case nir_op_fadd:
      native = CHROME9_PS_OPCODE_ADD;
      sources = 2;
      mask = 3;
      break;
   case nir_op_fsub:
      native = CHROME9_PS_OPCODE_SUB;
      sources = 2;
      mask = 3;
      break;
   default:
      return false;
   }
   for (unsigned s = 0; s < sources; s++) {
      if ((mask & (1u << s)) &&
          !chrome9_ps_source_modifier_allowed(native, s))
         return false;
      folded[s] = inner->src[s];
      for (unsigned c = 0; c < components; c++) {
         unsigned lane = negation->src[0].swizzle[c];

         if (lane >= inner->def.num_components)
            return false;
         folded[s].swizzle[c] = inner->src[s].swizzle[lane];
      }
   }
   *op = native;
   *count = sources;
   *negate = mask;
   return true;
}

static bool
chrome9_ps_immediate(struct chrome9_ps_nir_builder *builder,
                     const uint32_t value[4], unsigned components,
                     struct chrome9_ps_source *source)
{
   uint32_t best_values[4] = { 0 };
   uint8_t best_swizzle[4] = { 0 };
   uint8_t best_mask = 0;
   unsigned best_new_lanes = 5;
   unsigned id = 32;

   if (!components || components > 4)
      return false;
   for (unsigned candidate = 32; candidate-- > 16;) {
      uint32_t packed[4];
      uint8_t swizzle[4] = { 0 };
      uint8_t mask = builder->immediate_lane_mask[candidate];
      unsigned new_lanes = 0;
      unsigned c;

      /* Uniform ownership was collected before lowering any literals. */
      if (builder->uniform_mask & (1u << candidate))
         continue;
      memcpy(packed, builder->immediate[candidate], sizeof(packed));
      for (c = 0; c < components; c++) {
         unsigned lane;

         for (lane = 0; lane < 4; lane++)
            if ((mask & (1u << lane)) && packed[lane] == value[c])
               break;
         if (lane == 4) {
            for (lane = 0; lane < 4; lane++)
               if (!(mask & (1u << lane)))
                  break;
            if (lane == 4)
               break;
            packed[lane] = value[c];
            mask |= 1u << lane;
            new_lanes++;
         }
         swizzle[c] = lane;
      }
      if (c == components && new_lanes < best_new_lanes) {
         id = candidate;
         best_new_lanes = new_lanes;
         best_mask = mask;
         memcpy(best_values, packed, sizeof(best_values));
         memcpy(best_swizzle, swizzle, sizeof(best_swizzle));
      }
   }
   if (id == 32)
      return false;
   memcpy(builder->immediate[id], best_values, sizeof(best_values));
   builder->immediate_lane_mask[id] = best_mask;
   builder->immediate_mask |= 1u << id;
   memset(source, 0, sizeof(*source));
   source->file = CHROME9_PS_SOURCE_CONSTANT;
   source->id = id;
   for (unsigned c = 0; c < 4; c++)
      source->swizzle[c] = c < components ? best_swizzle[c] :
                           CHROME9_PS_SWIZZLE_ZERO;
   return true;
}

static bool
chrome9_ps_fuse_source_saturate(struct chrome9_ps_nir_builder *builder,
                                nir_def *source_def,
                                const struct chrome9_ps_source *source)
{
   struct chrome9_ps_instruction *definition = NULL;
   bool direct = false;
   uint8_t selected = 0;

   if (source->file != CHROME9_PS_SOURCE_TEMP || source->modifier)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (source->swizzle[c] < CHROME9_PS_SWIZZLE_ONE)
         selected |= 1u << source->swizzle[c];
   if (!selected)
      return false;
   /* MOV/vec aliases may lower to an older physical TEMP.  Saturating that
    * TEMP would also clamp the producer's unsaturated users.  Require this
    * exact SSA definition to own the physical result being modified. */
   for (unsigned i = 0; i < builder->value_count; i++) {
      if (builder->value[i].complete &&
          builder->value[i].def == source_def &&
          builder->value[i].source.file == source->file &&
          builder->value[i].source.id == source->id) {
         direct = true;
         break;
      }
   }
   if (!direct)
      return false;
   for (unsigned i = 0; i < builder->ir.count; i++) {
      struct chrome9_ps_instruction *instruction =
         &builder->ir.instruction[i];

      if (instruction->destination.file != CHROME9_PS_DESTINATION_TEMP ||
          instruction->destination.id != source->id)
         continue;
      if (definition || instruction->op == CHROME9_PS_OPCODE_TEX ||
          instruction->op == CHROME9_PS_OPCODE_KILL)
         return false;
      definition = instruction;
   }
   if (!definition ||
       (selected & ~definition->destination.writemask))
      return false;
   definition->destination.saturate = true;
   return true;
}

/* The semantic ABI addresses c0..c31 as vec4 registers.  State tracker
 * usually emits load_ubo_vec4; direct Gallium NIR may use byte-addressed
 * load_ubo instead.  Accept only constant, in-register reads from CB0. */
static bool
chrome9_ps_ubo_load(const nir_intrinsic_instr *load,
                    unsigned *id, unsigned *component)
{
   unsigned word;

   if ((load->intrinsic != nir_intrinsic_load_ubo &&
        load->intrinsic != nir_intrinsic_load_ubo_vec4) ||
       !nir_src_is_const(load->src[0]) ||
       nir_src_as_uint(load->src[0]) ||
       !nir_src_is_const(load->src[1]) ||
       load->def.bit_size != 32 || !load->def.num_components ||
       load->def.num_components > 4)
      return false;
   if (load->intrinsic == nir_intrinsic_load_ubo) {
      unsigned offset = nir_src_as_uint(load->src[1]);

      if (offset & 3)
         return false;
      word = offset / 4;
   } else {
      unsigned offset = nir_src_as_uint(load->src[1]);
      unsigned lane = nir_intrinsic_component(load);

      if (offset >= 32 || nir_intrinsic_base(load) >= 32 - offset ||
          lane >= 4)
         return false;
      word = (nir_intrinsic_base(load) + offset) * 4 +
             lane;
   }
   *id = word / 4;
   *component = word % 4;
   return *id < 32 && *component + load->def.num_components <= 4;
}

static bool
chrome9_ps_leaf(struct chrome9_ps_nir_builder *builder, nir_def *def,
                struct chrome9_ps_source *source)
{
   enum chrome9_fs_leaf leaf;
   nir_instr *instr = nir_def_instr(def);

   if (chrome9_backend_leaf(def, 0, &leaf))
      return chrome9_ps_source_from_leaf(leaf, source);
   if (instr->type == nir_instr_type_load_const) {
      nir_load_const_instr *load = nir_instr_as_load_const(instr);
      uint32_t value[4] = { 0 };

      if (def->bit_size != 32 || !def->num_components ||
          def->num_components > 4)
         return false;
      for (unsigned c = 0; c < def->num_components; c++)
         value[c] = load->value[c].u32;
      return chrome9_ps_immediate(builder, value, def->num_components,
                                  source);
   }
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
      unsigned location = ~0u;
      unsigned id, component;
      unsigned source_component = 0;

      /* Vector shrinking may turn a primary-color .aaaa read into a
       * one-component load at component 3.  The semantic backend must retain
       * that source lane even though the compatibility classifier requires a
       * full, unshifted primary-color load. */
      if (chrome9_fs_smooth_pixel_load_range(load, VARYING_SLOT_COL0) &&
          def->bit_size == 32 && def->num_components &&
          nir_intrinsic_component(load) < 4 &&
          def->num_components <= 4 - nir_intrinsic_component(load)) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_INPUT;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               nir_intrinsic_component(load) + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      if (load->intrinsic == nir_intrinsic_load_frag_coord &&
          def->bit_size == 32 && def->num_components == 4) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_POSITION;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c;
         return true;
      }

      /* The state tracker can lower gl_FragCoord to the POSITION input
       * rather than leave load_frag_coord in the linked fragment shader.
       * Accept only the same pixel-centered, smooth source; an indirect or
       * differently interpolated load is not the screen-position register. */
      if (chrome9_fs_smooth_pixel_load_range(load, VARYING_SLOT_POS) &&
          def->bit_size == 32 && def->num_components &&
          nir_intrinsic_component(load) < 4 &&
          def->num_components <= 4 - nir_intrinsic_component(load)) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_POSITION;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               nir_intrinsic_component(load) + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      if (load->intrinsic == nir_intrinsic_load_uniform &&
          nir_src_is_const(load->src[0]) && !nir_intrinsic_base(load) &&
          def->bit_size == 32 && def->num_components &&
          def->num_components <= 4) {
         unsigned id = nir_src_as_uint(load->src[0]);

         if (id >= 32 || !(builder->uniform_mask & (1u << id)))
            return false;
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_CONSTANT;
         source->id = id;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      if (chrome9_ps_ubo_load(load, &id, &component)) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_CONSTANT;
         source->id = id;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               component + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      switch (load->intrinsic) {
      case nir_intrinsic_load_input:
         if (!nir_intrinsic_component(load))
            location = nir_intrinsic_io_semantics(load).location;
         break;
      case nir_intrinsic_load_interpolated_input:
         for (unsigned candidate = 0; candidate < 8; candidate++) {
            if (chrome9_fs_smooth_pixel_load_range(
                   load, VARYING_SLOT_VAR0 + candidate)) {
               location = VARYING_SLOT_VAR0 + candidate;
               source_component = nir_intrinsic_component(load);
               break;
            }
            if (chrome9_fs_smooth_pixel_load_range(
                   load, VARYING_SLOT_TEX0 + candidate)) {
               location = VARYING_SLOT_TEX0 + candidate;
               source_component = nir_intrinsic_component(load);
               break;
            }
         }
         break;
      case nir_intrinsic_load_deref: {
         nir_deref_instr *deref = nir_src_as_deref(load->src[0]);
         nir_variable *var = nir_intrinsic_get_var(load, 0);

         if (deref && deref->deref_type == nir_deref_type_var && var &&
             ((var->data.location >= VARYING_SLOT_VAR0 &&
               var->data.location < VARYING_SLOT_VAR0 + 8) ||
              (var->data.location >= VARYING_SLOT_TEX0 &&
               var->data.location < VARYING_SLOT_TEX0 + 8)) &&
             chrome9_fs_smooth_variable(var, var->data.location))
            location = var->data.location;
         break;
      }
      default:
         break;
      }
      if (((location >= VARYING_SLOT_VAR0 &&
            location < VARYING_SLOT_VAR0 + 8) ||
           (location >= VARYING_SLOT_TEX0 &&
            location < VARYING_SLOT_TEX0 + 8)) && def->bit_size == 32 &&
          def->num_components && source_component < 4 &&
          def->num_components <= 4 - source_component) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_TEXTURE;
         source->id = location >= VARYING_SLOT_VAR0 &&
                      location < VARYING_SLOT_VAR0 + 8 ?
                         location - VARYING_SLOT_VAR0 :
                         location - VARYING_SLOT_TEX0;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               source_component + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }
   }
   return false;
}

/* A defined FRC result lies in [0, 1], as does a selection between two
 * values already in that interval.  Bound the DAG walk independently of the
 * shader's size; an excessive graph must fail closed. */
static bool
chrome9_ps_unit_interval(nir_scalar scalar, unsigned *budget)
{
   nir_alu_instr *alu;

   if (!*budget)
      return false;
   (*budget)--;
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32 ||
       nir_def_instr(scalar.def)->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(nir_def_instr(scalar.def));
   if (alu->op == nir_op_ffract)
      return !alu->fp_math_ctrl;
   if (alu->op != nir_op_bcsel || alu->fp_math_ctrl ||
       scalar.comp >= alu->def.num_components)
      return false;
   for (unsigned arm = 1; arm <= 2; arm++) {
      nir_alu_src *input = &alu->src[arm];

      if (!input->src.ssa || input->src.ssa->bit_size != 32 ||
          input->swizzle[scalar.comp] >= input->src.ssa->num_components ||
          !chrome9_ps_unit_interval(
             (nir_scalar) { input->src.ssa, input->swizzle[scalar.comp] },
             budget))
         return false;
   }
   return true;
}

/* Generic draws reject non-finite constants, positions and varying lanes;
 * supported texture formats decode finite normalized values.  Prove that a
 * scalar DAG cannot introduce a *defined* exceptional value.  Arithmetic
 * without a local exceptional-value preservation request may still overflow
 * or hit an undefined domain, but that result is not observably constrained
 * by NIR and therefore does not invalidate a later default-mode comparison. */
/* Memoized DAG proofs: each (def, component, property) is derived once and
 * the budget bounds the number of distinct nodes, not the tree expansion. */
enum chrome9_ps_proof_kind {
   CHROME9_PS_PROOF_FINITE,
   CHROME9_PS_PROOF_NONNEGATIVE,
   CHROME9_PS_PROOF_BOUND,
   CHROME9_PS_PROOF_UNINTERPOLATED,
};

#define CHROME9_PS_PROOF_NODES 4096

/* The memo is allocated lazily on the shader's ralloc context. */
static struct hash_table_u64 *
chrome9_ps_proof_table(struct chrome9_ps_proof *proof)
{
   if (!proof->memo)
      proof->memo = _mesa_hash_table_u64_create(proof->mem_ctx);
   return proof->memo;
}

static uint64_t
chrome9_ps_proof_key(nir_scalar scalar, enum chrome9_ps_proof_kind kind)
{
   return ((uint64_t)scalar.def->index << 8) | scalar.comp << 2 | kind;
}

/* Budget one node; bounds are recomputed but their nodes are counted. */
static bool
chrome9_ps_proof_visit(struct chrome9_ps_proof *proof, nir_scalar scalar,
                       enum chrome9_ps_proof_kind kind)
{
   (void)scalar;
   (void)kind;
   if (!proof->budget)
      return false;
   proof->budget--;
   return true;
}

static bool
chrome9_ps_proof_memo(struct chrome9_ps_proof *proof, nir_scalar scalar,
                      enum chrome9_ps_proof_kind kind,
                      bool (*derive)(nir_scalar, struct chrome9_ps_proof *))
{
   uint64_t key;
   void *known;
   bool result;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   key = chrome9_ps_proof_key(scalar, kind);
   if (!chrome9_ps_proof_table(proof))
      return false;
   known = _mesa_hash_table_u64_search(proof->memo, key);
   if (known)
      return known == (void *)(uintptr_t)2;
   if (!chrome9_ps_proof_visit(proof, scalar, kind))
      return false;
   result = derive(scalar, proof);
   _mesa_hash_table_u64_insert(proof->memo, key,
                               (void *)(uintptr_t)(result ? 2 : 1));
   return result;
}

static bool
chrome9_ps_default_finite_alu_source(nir_alu_instr *alu, unsigned source,
                                     unsigned components, struct chrome9_ps_proof *proof);

static nir_scalar
chrome9_ps_alu_scalar(nir_alu_instr *alu, unsigned source, unsigned comp)
{
   return nir_scalar_resolved(alu->src[source].src.ssa,
                              alu->src[source].swizzle[comp]);
}

/* True when a and b are the same value up to sign (fneg/fabs chains). */
static bool
chrome9_ps_same_magnitude(nir_scalar a, nir_scalar b)
{
   for (unsigned i = 0; i < 2; i++) {
      nir_scalar *s = i ? &b : &a;

      while (nir_def_instr(s->def)->type == nir_instr_type_alu) {
         nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(s->def));

         if (alu->op != nir_op_fneg && alu->op != nir_op_fabs &&
             alu->op != nir_op_mov)
            break;
         *s = chrome9_ps_alu_scalar(alu, 0, s->comp);
      }
   }
   return a.def == b.def && a.comp == b.comp;
}

/* Upper bound of |value| for a finite-input DAG, INFINITY when unknown.
 * normalize()'s v * rsq(dot(v, v)) is bounded by one plus rounding. */
static float
chrome9_ps_magnitude_bound_uncached(nir_scalar scalar,
                                    struct chrome9_ps_proof *proof);

static float
chrome9_ps_magnitude_bound(nir_scalar scalar, struct chrome9_ps_proof *proof)
{
   uint64_t key;
   void *known;
   float bound;
   uint32_t bits;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   key = chrome9_ps_proof_key(scalar, CHROME9_PS_PROOF_BOUND);
   if (!chrome9_ps_proof_table(proof))
      return INFINITY;
   known = _mesa_hash_table_u64_search(proof->memo, key);
   if (known) {
      bits = (uint32_t)(uintptr_t)known;
      memcpy(&bound, &bits, sizeof(bound));
      return bound;
   }
   bound = chrome9_ps_magnitude_bound_uncached(scalar, proof);
   memcpy(&bits, &bound, sizeof(bits));
   _mesa_hash_table_u64_insert(proof->memo, key,
                               (void *)(uintptr_t)((uint64_t)bits | 1ull << 32));
   return bound;
}

static float
chrome9_ps_magnitude_bound_uncached(nir_scalar scalar,
                                    struct chrome9_ps_proof *proof)
{
   nir_alu_instr *alu;
   float a, b;

   if (!chrome9_ps_proof_visit(proof, scalar, CHROME9_PS_PROOF_BOUND))
      return INFINITY;
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32)
      return INFINITY;
   if (nir_scalar_is_const(scalar)) {
      float value = fabsf(nir_scalar_as_float(scalar));

      return isfinite(value) ? value : INFINITY;
   }
   if (nir_def_instr(scalar.def)->type != nir_instr_type_alu)
      return INFINITY;
   alu = nir_instr_as_alu(nir_def_instr(scalar.def));
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fabs:
      return chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
   case nir_op_fsat:
      return 1.0f;
   case nir_op_fmin:
   case nir_op_fmax:
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return MAX2(a, b);
   case nir_op_fadd:
   case nir_op_fsub:
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return a + b;
   case nir_op_fmul:
      for (unsigned s = 0; s < 2; s++) {
         nir_scalar factor = chrome9_ps_alu_scalar(alu, 1 - s, scalar.comp);
         nir_scalar vector = chrome9_ps_alu_scalar(alu, s, scalar.comp);
         nir_alu_instr *rsq, *dot;
         unsigned n;
         bool matched = false;

         if (nir_def_instr(factor.def)->type != nir_instr_type_alu)
            continue;
         rsq = nir_instr_as_alu(nir_def_instr(factor.def));
         if (rsq->op != nir_op_frsq ||
             nir_def_instr(rsq->src[0].src.ssa)->type != nir_instr_type_alu)
            continue;
         dot = nir_instr_as_alu(nir_def_instr(rsq->src[0].src.ssa));
         if (dot->op != nir_op_fdot2 && dot->op != nir_op_fdot3 &&
             dot->op != nir_op_fdot4)
            continue;
         n = dot->op == nir_op_fdot2 ? 2 : dot->op == nir_op_fdot3 ? 3 : 4;
         for (unsigned c = 0; c < n && !matched; c++)
            matched = chrome9_ps_same_magnitude(
                         chrome9_ps_alu_scalar(dot, 0, c),
                         chrome9_ps_alu_scalar(dot, 1, c)) &&
                      chrome9_ps_same_magnitude(
                         chrome9_ps_alu_scalar(dot, 0, c), vector);
         for (unsigned c = 0; c < n && matched; c++)
            matched = chrome9_ps_same_magnitude(
               chrome9_ps_alu_scalar(dot, 0, c),
               chrome9_ps_alu_scalar(dot, 1, c));
         if (matched)
            return 1.0f + 4.0f * FLT_EPSILON;
      }
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return a * b;
   case nir_op_ffma:
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return a * b + chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 2, scalar.comp), proof);
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned n = alu->op == nir_op_fdot2 ? 2 :
                   alu->op == nir_op_fdot3 ? 3 : 4;
      float sum = 0.0f;

      for (unsigned c = 0; c < n; c++)
         sum += chrome9_ps_magnitude_bound(
                   chrome9_ps_alu_scalar(alu, 0, c), proof) *
                chrome9_ps_magnitude_bound(
                   chrome9_ps_alu_scalar(alu, 1, c), proof);
      return sum;
   }
   default:
      return INFINITY;
   }
}

static bool
chrome9_ps_default_finite_uncached(nir_scalar scalar,
                                   struct chrome9_ps_proof *proof);

static bool
chrome9_ps_default_finite(nir_scalar scalar, struct chrome9_ps_proof *proof)
{
   return chrome9_ps_proof_memo(proof, scalar, CHROME9_PS_PROOF_FINITE,
                                chrome9_ps_default_finite_uncached);
}

static bool
chrome9_ps_default_finite_uncached(nir_scalar scalar,
                                   struct chrome9_ps_proof *proof)
{
   nir_instr *instr;
   nir_alu_instr *alu;
   unsigned sources = 0;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32 || scalar.comp >= scalar.def->num_components)
      return false;
   instr = nir_def_instr(scalar.def);
   if (instr->type == nir_instr_type_load_const) {
      nir_load_const_instr *load = nir_instr_as_load_const(instr);
      float value = load->value[scalar.comp].f32;

      return isfinite(value);
   }
   /* The normal semantic-leaf validation remains authoritative.  These are
    * merely the leaf classes whose values the draw path proves finite. */
   if (instr->type == nir_instr_type_intrinsic ||
       instr->type == nir_instr_type_tex)
      return true;
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   /* Value-preserving ops cannot create an exceptional value from finite
    * inputs, so their preserve request is vacuous.  Flagged arithmetic of
    * finite operands can reach infinity only by overflow, which GLSL leaves
    * undefined, so the operands decide. */
   if (alu->fp_math_ctrl & nir_fp_preserve_sz_inf_nan) {
      switch (alu->op) {
      case nir_op_mov:
      case nir_op_fneg:
      case nir_op_fabs:
      case nir_op_fsat:
      case nir_op_fmin:
      case nir_op_fmax:
      case nir_op_fadd:
      case nir_op_fsub:
      case nir_op_fmul:
      case nir_op_ffma:
      case nir_op_fdot2:
      case nir_op_fdot3:
      case nir_op_fdot4:
      case nir_op_fsign:
         break;
      default:
         return false;
      }
   }
   switch (alu->op) {
   case nir_op_b2f32:
      return true;
   case nir_op_bcsel:
      /* Either arm is finite; the predicate only chooses. */
      for (unsigned s = 1; s < 3; s++)
         if (!alu->src[s].src.ssa ||
             alu->src[s].swizzle[scalar.comp] >=
                alu->src[s].src.ssa->num_components ||
             !chrome9_ps_default_finite(
                (nir_scalar) { alu->src[s].src.ssa,
                               alu->src[s].swizzle[scalar.comp] }, proof))
            return false;
      return true;
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fabs:
   case nir_op_fsign:
   case nir_op_fsat:
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt:
   case nir_op_ffract:
      sources = 1;
      break;
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_fpow:
      sources = 2;
      break;
   case nir_op_ffma:
      sources = 3;
      break;
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned components = alu->op == nir_op_fdot2 ? 2 :
                            alu->op == nir_op_fdot3 ? 3 : 4;

      for (unsigned s = 0; s < 2; s++)
         if (!chrome9_ps_default_finite_alu_source(alu, s, components,
                                                    proof))
            return false;
      return true;
   }
   default:
      return false;
   }
   for (unsigned s = 0; s < sources; s++) {
      if (!alu->src[s].src.ssa ||
          alu->src[s].swizzle[scalar.comp] >=
             alu->src[s].src.ssa->num_components ||
          !chrome9_ps_default_finite(
             (nir_scalar) { alu->src[s].src.ssa,
                            alu->src[s].swizzle[scalar.comp] }, proof))
         return false;
   }
   return true;
}

static bool
chrome9_ps_default_finite_alu_source(nir_alu_instr *alu, unsigned source,
                                     unsigned components, struct chrome9_ps_proof *proof)
{
   if (!alu->src[source].src.ssa ||
       alu->src[source].src.ssa->bit_size != 32)
      return false;
   for (unsigned c = 0; c < components; c++) {
      unsigned component = alu->src[source].swizzle[c];

      if (component >= alu->src[source].src.ssa->num_components ||
          !chrome9_ps_default_finite(
             (nir_scalar) { alu->src[source].src.ssa, component }, proof))
         return false;
   }
   return true;
}

/* Perspective interpolation of a triangle-constant value is not exact in
 * general, so float equality is lowered only on values that no interpolated
 * varying reaches: constants, uniforms, texels, the pixel position, and
 * 0.0/1.0 selections. */
static bool
chrome9_ps_uninterpolated_uncached(nir_scalar scalar,
                                   struct chrome9_ps_proof *proof);

static bool
chrome9_ps_uninterpolated(nir_scalar scalar, struct chrome9_ps_proof *proof)
{
   return chrome9_ps_proof_memo(proof, scalar,
                                CHROME9_PS_PROOF_UNINTERPOLATED,
                                chrome9_ps_uninterpolated_uncached);
}

static bool
chrome9_ps_uninterpolated_uncached(nir_scalar scalar,
                                   struct chrome9_ps_proof *proof)
{
   nir_instr *instr;
   nir_alu_instr *alu;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   instr = nir_def_instr(scalar.def);
   switch (instr->type) {
   case nir_instr_type_load_const:
   case nir_instr_type_tex:
      return true;
   case nir_instr_type_intrinsic:
      switch (nir_instr_as_intrinsic(instr)->intrinsic) {
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ubo_vec4:
      case nir_intrinsic_load_frag_coord:
      case nir_intrinsic_load_front_face:
         return true;
      default:
         return false;
      }
   case nir_instr_type_alu:
      break;
   default:
      return false;
   }
   alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_b2f32)
      return true;
   for (unsigned s = alu->op == nir_op_bcsel ? 1 : 0;
        s < nir_op_infos[alu->op].num_inputs; s++) {
      const unsigned size = nir_op_infos[alu->op].input_sizes[s];

      if (!alu->src[s].src.ssa)
         return false;
      /* Reductions read every lane of their sources. */
      for (unsigned c = 0; c < (size ? size : 1); c++) {
         const unsigned lane = size ? c : scalar.comp;

         if (alu->src[s].swizzle[lane] >= alu->src[s].src.ssa->num_components ||
             !chrome9_ps_uninterpolated(
                (nir_scalar) { alu->src[s].src.ssa,
                               alu->src[s].swizzle[lane] }, proof))
            return false;
      }
   }
   return true;
}

/* Prove a nonnegative result for the lighting DAG shapes emitted by generic
 * NIR.  This is a mathematical property, not an application/shader match. */
static bool
chrome9_ps_default_nonnegative_uncached(nir_scalar scalar,
                                        struct chrome9_ps_proof *proof);

static bool
chrome9_ps_default_nonnegative(nir_scalar scalar,
                               struct chrome9_ps_proof *proof)
{
   return chrome9_ps_proof_memo(proof, scalar, CHROME9_PS_PROOF_NONNEGATIVE,
                                chrome9_ps_default_nonnegative_uncached);
}

static bool
chrome9_ps_default_nonnegative_uncached(nir_scalar scalar,
                                        struct chrome9_ps_proof *proof)
{
   nir_instr *instr;
   nir_alu_instr *alu;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32 || scalar.comp >= scalar.def->num_components)
      return false;
   instr = nir_def_instr(scalar.def);
   if (instr->type == nir_instr_type_load_const) {
      float value = nir_instr_as_load_const(instr)->value[scalar.comp].f32;

      return isfinite(value) && value >= 0.0f;
   }
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   if ((alu->fp_math_ctrl & nir_fp_preserve_sz_inf_nan) &&
       alu->op != nir_op_fmax)
      return false;
   switch (alu->op) {
   case nir_op_mov:
      if (!alu->src[0].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components)
         return false;
      return chrome9_ps_default_nonnegative(
         (nir_scalar) { alu->src[0].src.ssa,
                        alu->src[0].swizzle[scalar.comp] }, proof);
   case nir_op_fabs:
   case nir_op_fsat:
      if (!alu->src[0].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components)
         return false;
      return chrome9_ps_default_finite(
         (nir_scalar) { alu->src[0].src.ssa,
                        alu->src[0].swizzle[scalar.comp] }, proof);
   case nir_op_fsqrt:
   case nir_op_frsq:
      if (!alu->src[0].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components)
         return false;
      return chrome9_ps_default_nonnegative(
         (nir_scalar) { alu->src[0].src.ssa,
                        alu->src[0].swizzle[scalar.comp] }, proof);
   case nir_op_fmul:
      if (!alu->src[0].src.ssa || !alu->src[1].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components ||
          alu->src[1].swizzle[scalar.comp] >=
             alu->src[1].src.ssa->num_components)
         return false;
      return chrome9_ps_default_nonnegative(
                (nir_scalar) { alu->src[0].src.ssa,
                               alu->src[0].swizzle[scalar.comp] }, proof) &&
             chrome9_ps_default_nonnegative(
                (nir_scalar) { alu->src[1].src.ssa,
                               alu->src[1].swizzle[scalar.comp] }, proof);
   case nir_op_fmax: {
      if (!alu->src[0].src.ssa || !alu->src[1].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components ||
          alu->src[1].swizzle[scalar.comp] >=
             alu->src[1].src.ssa->num_components)
         return false;
      nir_scalar a = { alu->src[0].src.ssa,
                       alu->src[0].swizzle[scalar.comp] };
      nir_scalar b = { alu->src[1].src.ssa,
                       alu->src[1].swizzle[scalar.comp] };
      bool a_nonnegative = chrome9_ps_default_nonnegative(a, proof);
      bool b_nonnegative = chrome9_ps_default_nonnegative(b, proof);

      if (!chrome9_ps_default_finite(a, proof) ||
          !chrome9_ps_default_finite(b, proof))
         return false;
      return a_nonnegative || b_nonnegative;
   }
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned components = alu->op == nir_op_fdot2 ? 2 :
                            alu->op == nir_op_fdot3 ? 3 : 4;

      for (unsigned c = 0; c < components; c++)
         if (alu->src[0].swizzle[c] != alu->src[1].swizzle[c])
            return false;
      if (alu->src[0].src.ssa != alu->src[1].src.ssa)
         return false;
      return chrome9_ps_default_finite_alu_source(alu, 0, components,
                                                   proof);
   }
   default:
      return false;
   }
}

static bool
chrome9_ps_emit_scalar_select(struct chrome9_ps_nir_builder *builder,
                              nir_alu_instr *select, nir_def *def,
                              uint8_t condition_temp,
                              struct chrome9_ps_source *source)
{
   struct chrome9_ps_source true_value, false_value;
   struct chrome9_ps_instruction *conditional;
   unsigned components = def->num_components, value;
   uint8_t result_temp;

   if (!chrome9_ps_lower_alu_source(builder, &select->src[1], components,
                                    &true_value) ||
       !(chrome9_ps_source_modifier_allowed(CHROME9_PS_OPCODE_CMP, 2) ?
         chrome9_ps_lower_alu_source_modifier(builder, &select->src[2],
                                              components, &false_value) :
         chrome9_ps_lower_alu_source(builder, &select->src[2], components,
                                     &false_value)) ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   result_temp = builder->next_temp++;
   conditional = chrome9_ps_append_instruction(builder,
                                                CHROME9_PS_OPCODE_CMP);
   if (!conditional)
      return false;
   conditional->destination.file = CHROME9_PS_DESTINATION_TEMP;
   conditional->destination.id = result_temp;
   conditional->destination.writemask = (1u << components) - 1;
   conditional->source[0].file = CHROME9_PS_SOURCE_TEMP;
   conditional->source[0].id = condition_temp;
   for (unsigned c = 0; c < 4; c++)
      conditional->source[0].swizzle[c] = c < components ?
         CHROME9_PS_SWIZZLE_X : CHROME9_PS_SWIZZLE_ZERO;
   conditional->source[1] = true_value;
   conditional->source[2] = false_value;

   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
   builder->value[value].source.id = result_temp;
   for (unsigned c = 0; c < 4; c++)
      builder->value[value].source.swizzle[c] = c < components ?
         c : CHROME9_PS_SWIZZLE_ZERO;
   builder->value[value].complete = true;
   *source = builder->value[value].source;
   return true;
}

/* CMP selects its C operand when A >= 0.  A subtraction can implement an
 * ordered floating comparison only when its sign is preserved.  Admit a
 * proven [0, 1] value around a central normal threshold, or a nonnegative
 * defined-finite DAG against a positive normal constant.  The latter cannot
 * overflow and default-mode FTZ is qualified consistently for producer and
 * SUB/CMP paths.  Other comparisons remain closed. */
static bool
chrome9_ps_lower_unit_interval_select(struct chrome9_ps_nir_builder *builder,
                                       nir_alu_instr *select, nir_def *def,
                                       struct chrome9_ps_source *source)
{
   nir_alu_instr *compare;
   nir_scalar predicate, bounded, threshold;
   struct chrome9_ps_source lhs, rhs;
   struct chrome9_ps_instruction *subtract;
   uint8_t condition_temp;
   unsigned components = def->num_components;
   unsigned range_budget = CHROME9_PS_MAX_INSTRUCTIONS;
   float boundary;

   if (select->op != nir_op_bcsel || !components || components > 4 ||
       !select->src[0].src.ssa ||
       select->src[0].src.ssa->bit_size != 1 ||
       select->src[0].swizzle[0] >=
          select->src[0].src.ssa->num_components)
      return false;
   /* One scalar predicate is broadcast across the live result lanes.
    * A vector predicate requires distinct comparisons and cannot use this
    * single native CMP record. */
   for (unsigned c = 1; c < components; c++)
      if (select->src[0].swizzle[c] != select->src[0].swizzle[0])
         return false;
   predicate = nir_scalar_resolved(select->src[0].src.ssa,
                                   select->src[0].swizzle[0]);
   if (predicate.comp || nir_def_instr(predicate.def)->type !=
                            nir_instr_type_alu)
      return false;
   compare = nir_instr_as_alu(nir_def_instr(predicate.def));
   if (compare->op != nir_op_fge ||
       (compare->fp_math_ctrl & ~nir_fp_preserve_inf) ||
       !compare->src[0].src.ssa || !compare->src[1].src.ssa ||
       compare->src[0].src.ssa->bit_size != 32 ||
       compare->src[1].src.ssa->bit_size != 32 ||
       compare->src[0].swizzle[0] >=
          compare->src[0].src.ssa->num_components ||
       compare->src[1].swizzle[0] >=
          compare->src[1].src.ssa->num_components)
      return false;
   bounded = (nir_scalar) { compare->src[0].src.ssa,
                            compare->src[0].swizzle[0] };
   threshold = nir_scalar_resolved(compare->src[1].src.ssa,
                                   compare->src[1].swizzle[0]);
   if (!nir_scalar_is_const(threshold))
      return false;
   boundary = nir_scalar_as_float(threshold);
   if (!((boundary >= 0.25f && boundary <= 0.75f &&
          chrome9_ps_unit_interval(bounded, &range_budget)) ||
         (boundary > 0.0f && isnormal(boundary) &&
          chrome9_ps_default_nonnegative(bounded, &builder->proof))))
      return false;
   if (!chrome9_ps_lower_alu_source(builder, &compare->src[0], 1, &lhs) ||
       !chrome9_ps_lower_alu_source(builder, &compare->src[1], 1, &rhs) ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   condition_temp = builder->next_temp++;
   subtract = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_SUB);
   if (!subtract)
      return false;
   subtract->destination.file = CHROME9_PS_DESTINATION_TEMP;
   subtract->destination.id = condition_temp;
   subtract->destination.writemask = 1;
   subtract->source[0] = lhs;
   subtract->source[1] = rhs;
   return chrome9_ps_emit_scalar_select(builder, select, def,
                                         condition_temp, source);
}

static bool
chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder, nir_def *def,
                     struct chrome9_ps_source *source);

/* Append op into a new temp with the given writemask. */
static bool
chrome9_ps_emit_temp(struct chrome9_ps_nir_builder *builder,
                     enum chrome9_ps_opcode op,
                     const struct chrome9_ps_source *sources, unsigned count,
                     unsigned writemask, uint8_t *temp)
{
   struct chrome9_ps_instruction *instruction;

   if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   *temp = builder->next_temp++;
   instruction = chrome9_ps_append_instruction(builder, op);
   if (!instruction)
      return false;
   instruction->destination.file = CHROME9_PS_DESTINATION_TEMP;
   instruction->destination.id = *temp;
   instruction->destination.writemask = writemask;
   for (unsigned s = 0; s < count; s++)
      instruction->source[s] = sources[s];
   return true;
}

static struct chrome9_ps_source
chrome9_ps_temp_source(uint8_t temp, const uint8_t swizzle[4])
{
   struct chrome9_ps_source source = {
      .file = CHROME9_PS_SOURCE_TEMP,
      .id = temp,
   };

   memcpy(source.swizzle, swizzle, sizeof(source.swizzle));
   return source;
}

static bool
chrome9_ps_scalar_is_zero(nir_scalar scalar)
{
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   return nir_scalar_is_const(scalar) &&
          !(nir_scalar_as_uint(scalar) & 0x7fffffff);
}

static bool
chrome9_ps_emit_lane_select(struct chrome9_ps_nir_builder *builder,
                            const nir_alu_src *predicate,
                            const struct chrome9_ps_source *when_true,
                            const struct chrome9_ps_source *when_false,
                            unsigned components,
                            struct chrome9_ps_source *result);

/* GL stores Boolean uniforms of a driver without integers as exactly 0.0
 * or 1.0, so b2b1 of such a load is its float value. */
static bool
chrome9_ps_canonical_bool(nir_scalar scalar)
{
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (nir_scalar_is_const(scalar))
      return nir_scalar_as_uint(scalar) == 0 ||
             nir_scalar_as_uint(scalar) == 0x3f800000;
   return nir_scalar_is_intrinsic(scalar) &&
          (nir_scalar_intrinsic_op(scalar) == nir_intrinsic_load_ubo ||
           nir_scalar_intrinsic_op(scalar) == nir_intrinsic_load_ubo_vec4);
}

/* A 1-bit vector as 1.0/0.0 lanes: canonical Boolean uniforms directly,
 * anything else through a per-lane select. */
static bool
chrome9_ps_lower_bool_float(struct chrome9_ps_nir_builder *builder,
                            const nir_alu_src *value, unsigned count,
                            struct chrome9_ps_source *source)
{
   static const uint32_t one[4] = {
      0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
   };
   static const uint32_t zero[4] = { 0 };
   struct chrome9_ps_source when_true, when_false;
   nir_alu_instr *convert = NULL;
   uint32_t literal[4] = { 0 };
   bool constant = true;
   nir_alu_src stored;

   for (unsigned c = 0; c < count; c++) {
      nir_scalar lane = nir_scalar_resolved(value->src.ssa, value->swizzle[c]);

      constant &= nir_scalar_is_const(lane);
      if (constant && nir_scalar_as_bool(lane))
         literal[c] = 0x3f800000;
   }
   if (constant)
      return chrome9_ps_immediate(builder, literal, count, source);
   for (unsigned c = 0; c < count; c++) {
      nir_scalar lane = nir_scalar_resolved(value->src.ssa, value->swizzle[c]);

      if (!nir_scalar_is_alu(lane) || nir_scalar_alu_op(lane) != nir_op_b2b1 ||
          (convert && convert != nir_instr_as_alu(nir_def_instr(lane.def))) ||
          !chrome9_ps_canonical_bool(nir_scalar_chase_alu_src(lane, 0))) {
         convert = NULL;
         break;
      }
      convert = nir_instr_as_alu(nir_def_instr(lane.def));
   }
   if (convert) {
      stored = convert->src[0];
      for (unsigned c = 0; c < count; c++)
         stored.swizzle[c] = convert->src[0].swizzle[nir_scalar_resolved(
            value->src.ssa, value->swizzle[c]).comp];
      return chrome9_ps_lower_alu_source(builder, &stored, count, source);
   }
   return chrome9_ps_immediate(builder, one, count, &when_true) &&
          chrome9_ps_immediate(builder, zero, count, &when_false) &&
          chrome9_ps_emit_lane_select(builder, value, &when_true,
                                      &when_false, count, source);
}

/* Condition lanes of lhs op rhs, one per result lane (lanes[c] selects the
 * compare component); ordered compares test the difference, equality its
 * negated magnitude.  Float operands must be proven finite; Boolean
 * operands compare as 1.0/0.0. */
static bool
chrome9_ps_lower_compare_lanes(struct chrome9_ps_nir_builder *builder,
                               nir_alu_instr *alu, const uint8_t *lanes,
                               unsigned count,
                               struct chrome9_ps_condition *condition)
{
   nir_alu_src operand[2] = { alu->src[0], alu->src[1] };
   struct chrome9_ps_source source[2];
   static const uint8_t identity[4] = { 0, 1, 2, 3 };
   uint8_t swizzle[4] = {
      CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO,
      CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO,
   };
   const bool boolean = alu->op == nir_op_ieq || alu->op == nir_op_ine ||
                        alu->op == nir_op_ixor;
   const bool convert = alu->op == nir_op_b2b1;
   const bool equality = alu->op == nir_op_feq || alu->op == nir_op_fneu ||
                         boolean || convert;
   const unsigned sources = convert ? 1 : 2;
   const unsigned mask = BITFIELD_MASK(count);
   int zero_side = convert ? 1 : -1;
   bool interpolated = false;
   uint8_t temp = 0;

   if (alu->op != nir_op_fge && alu->op != nir_op_flt && !equality)
      return false;
   for (unsigned s = 0; s < sources; s++) {
      if (!alu->src[s].src.ssa ||
          alu->src[s].src.ssa->bit_size != (boolean ? 1 : 32))
         return false;
      for (unsigned c = 0; c < count; c++) {
         nir_scalar scalar = nir_scalar_chase_alu_src(
            (nir_scalar) { &alu->def, lanes[c] }, s);

         if (lanes[c] >= alu->def.num_components ||
             (convert ? !chrome9_ps_canonical_bool(scalar) :
              !boolean && !chrome9_ps_default_finite(scalar,
                                                     &builder->proof)))
            return false;
         interpolated |= equality && !boolean && !convert &&
                         !chrome9_ps_uninterpolated(scalar, &builder->proof);
         operand[s].swizzle[c] = alu->src[s].swizzle[lanes[c]];
      }
   }
   /* Equality with a literal zero needs only the magnitude. */
   if (equality && !boolean && !convert) {
      for (int s = 1; s >= 0 && zero_side < 0; s--) {
         bool zero = true;

         for (unsigned c = 0; c < count; c++)
            zero &= chrome9_ps_scalar_is_zero(nir_scalar_chase_alu_src(
               (nir_scalar) { &alu->def, lanes[c] }, s));
         if (zero)
            zero_side = s;
      }
   }
   if (zero_side >= 0) {
      if (!chrome9_ps_lower_alu_source(builder, &operand[!zero_side], count,
                                       &source[0]))
         return false;
      /* A varying compared to zero is exact when the draw proves each
       * triangle's lane bit-identical 0 or +-1 (equal_zero_input_mask). */
      if (interpolated) {
         if (source[0].file != CHROME9_PS_SOURCE_TEXTURE ||
             source[0].id >= 8 || source[0].modifier)
            return false;
         for (unsigned c = 0; c < count; c++) {
            if (source[0].swizzle[c] > CHROME9_PS_SWIZZLE_W)
               return false;
            builder->equal_zero_input_mask |=
               1u << (source[0].id * 4 + source[0].swizzle[c]);
         }
      }
   } else if (interpolated) {
      return false;
   } else {
      for (unsigned s = 0; s < 2; s++)
         if (!(boolean ?
               chrome9_ps_lower_bool_float(builder, &operand[s], count,
                                           &source[s]) :
               chrome9_ps_lower_alu_source(builder, &operand[s], count,
                                           &source[s])))
            return false;
      if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_SUB, source, 2,
                                mask, &temp))
         return false;
      memcpy(swizzle, identity, count);
      source[0] = chrome9_ps_temp_source(temp, swizzle);
   }
   if (equality) {
      if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_ABS, source, 1,
                                mask, &temp))
         return false;
      condition->negate = true;
   } else {
      condition->negate = false;
   }
   condition->temp = temp;
   condition->invert = alu->op == nir_op_flt || alu->op == nir_op_fneu ||
                       alu->op == nir_op_ine || alu->op == nir_op_ixor ||
                       convert;
   return true;
}

/* all(equal(a, b)) and any(notEqual(a, b)) of floats or Booleans: the
 * summed magnitude of the differences is zero exactly when every lane is
 * equal. */
static bool
chrome9_ps_lower_vector_equality(struct chrome9_ps_nir_builder *builder,
                                 nir_alu_instr *alu, unsigned count,
                                 struct chrome9_ps_condition *condition)
{
   static const uint32_t one[4] = {
      0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
   };
   static const uint8_t identity[4] = { 0, 1, 2, 3 };
   struct chrome9_ps_source source[2];
   uint8_t swizzle[4] = {
      CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO,
      CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO,
   };
   const bool boolean = alu->src[0].src.ssa &&
                        alu->src[0].src.ssa->bit_size == 1;
   uint8_t difference, magnitude, sum;

   for (unsigned s = 0; s < 2; s++) {
      if (!alu->src[s].src.ssa ||
          alu->src[s].src.ssa->bit_size != (boolean ? 1 : 32) ||
          (!boolean &&
           !chrome9_ps_default_finite_alu_source(alu, s, count,
                                                 &builder->proof)))
         return false;
      for (unsigned c = 0; !boolean && c < count; c++)
         if (!chrome9_ps_uninterpolated(
                (nir_scalar) { alu->src[s].src.ssa, alu->src[s].swizzle[c] },
                &builder->proof))
            return false;
   }
   for (unsigned s = 0; s < 2; s++)
      if (!(boolean ?
            chrome9_ps_lower_bool_float(builder, &alu->src[s], count,
                                        &source[s]) :
            chrome9_ps_lower_alu_source(builder, &alu->src[s], count,
                                        &source[s])))
         return false;
   if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_SUB, source, 2,
                             BITFIELD_MASK(count), &difference))
      return false;
   memcpy(swizzle, identity, count);
   source[0] = chrome9_ps_temp_source(difference, swizzle);
   if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_ABS, source, 1,
                             BITFIELD_MASK(count), &magnitude))
      return false;
   if (count == 2) {
      source[0] = chrome9_ps_temp_source(magnitude, (uint8_t[4]) {
         CHROME9_PS_SWIZZLE_X, CHROME9_PS_SWIZZLE_ZERO,
         CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO });
      source[1] = chrome9_ps_temp_source(magnitude, (uint8_t[4]) {
         CHROME9_PS_SWIZZLE_Y, CHROME9_PS_SWIZZLE_ZERO,
         CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO });
      if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_MAX, source, 2, 1,
                                &sum))
         return false;
   } else {
      source[0] = chrome9_ps_temp_source(magnitude, swizzle);
      if (!chrome9_ps_immediate(builder, one, count, &source[1]) ||
          !chrome9_ps_emit_temp(builder, count == 3 ? CHROME9_PS_OPCODE_DP3 :
                                                      CHROME9_PS_OPCODE_DP4,
                                source, 2, 1, &sum))
         return false;
   }
   condition->temp = sum;
   condition->negate = true;
   condition->invert = alu->op == nir_op_bany_fnequal2 ||
                       alu->op == nir_op_bany_fnequal3 ||
                       alu->op == nir_op_bany_fnequal4 ||
                       alu->op == nir_op_bany_inequal2 ||
                       alu->op == nir_op_bany_inequal3 ||
                       alu->op == nir_op_bany_inequal4;
   return true;
}

/*
 * Lower a scalar Boolean to one CMP condition, memoized per scalar so a
 * predicate shared by several selects is computed once.  A conjunction or
 * disjunction of two conditions of the same form is their MIN or MAX.
 */
static bool
chrome9_ps_lower_condition(struct chrome9_ps_nir_builder *builder,
                           nir_scalar predicate, unsigned depth,
                           struct chrome9_ps_condition *condition)
{
   struct chrome9_ps_condition operand[2];
   struct chrome9_ps_source source[2];
   nir_alu_instr *alu;
   bool lowered;

   predicate = nir_scalar_resolved(predicate.def, predicate.comp);
   if (depth > 8 || predicate.def->bit_size != 1 ||
       nir_def_instr(predicate.def)->type != nir_instr_type_alu)
      return false;
   for (unsigned i = 0; i < builder->condition_count; i++) {
      if (nir_scalar_equal(builder->condition[i].predicate, predicate)) {
         *condition = builder->condition[i].condition;
         return true;
      }
   }
   alu = nir_instr_as_alu(nir_def_instr(predicate.def));
   if (predicate.comp >= alu->def.num_components)
      return false;
   switch (alu->op) {
   case nir_op_fge:
   case nir_op_flt:
   case nir_op_feq:
   case nir_op_fneu:
   case nir_op_ieq:
   case nir_op_ine:
   case nir_op_ixor:
   case nir_op_b2b1: {
      const uint8_t lane = predicate.comp;

      lowered = chrome9_ps_lower_compare_lanes(builder, alu, &lane, 1,
                                               condition);
      break;
   }
   case nir_op_ball_fequal2:
   case nir_op_bany_fnequal2:
   case nir_op_ball_iequal2:
   case nir_op_bany_inequal2:
      lowered = chrome9_ps_lower_vector_equality(builder, alu, 2, condition);
      break;
   case nir_op_ball_fequal3:
   case nir_op_bany_fnequal3:
   case nir_op_ball_iequal3:
   case nir_op_bany_inequal3:
      lowered = chrome9_ps_lower_vector_equality(builder, alu, 3, condition);
      break;
   case nir_op_ball_fequal4:
   case nir_op_bany_fnequal4:
   case nir_op_ball_iequal4:
   case nir_op_bany_inequal4:
      lowered = chrome9_ps_lower_vector_equality(builder, alu, 4, condition);
      break;
   case nir_op_inot:
      lowered = chrome9_ps_lower_condition(
         builder, nir_scalar_chase_alu_src(predicate, 0), depth + 1,
         condition);
      condition->invert = !condition->invert;
      break;
   case nir_op_iand:
   case nir_op_ior:
      if (!chrome9_ps_lower_condition(
             builder, nir_scalar_chase_alu_src(predicate, 0), depth + 1,
             &operand[0]) ||
          !chrome9_ps_lower_condition(
             builder, nir_scalar_chase_alu_src(predicate, 1), depth + 1,
             &operand[1]) ||
          operand[0].negate != operand[1].negate ||
          operand[0].invert != operand[1].invert)
         return false;
      /* Both negation and complement exchange MIN and MAX. */
      for (unsigned s = 0; s < 2; s++)
         source[s] = chrome9_ps_temp_source(operand[s].temp, (uint8_t[4]) {
            CHROME9_PS_SWIZZLE_X, CHROME9_PS_SWIZZLE_ZERO,
            CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO });
      *condition = operand[0];
      lowered = chrome9_ps_emit_temp(
         builder, (alu->op == nir_op_iand) ^ operand[0].negate ^
                  operand[0].invert ? CHROME9_PS_OPCODE_MIN :
                                      CHROME9_PS_OPCODE_MAX,
         source, 2, 1, &condition->temp);
      break;
   default:
      return false;
   }
   if (!lowered || builder->condition_count >= ARRAY_SIZE(builder->condition))
      return lowered;
   builder->condition[builder->condition_count].predicate = predicate;
   builder->condition[builder->condition_count++].condition = *condition;
   return true;
}

/* CMP(condition, when_true, when_false) over the written lanes; lane c of
 * the condition is read from condition_swizzle[c]. */
static bool
chrome9_ps_emit_condition_cmp(struct chrome9_ps_nir_builder *builder,
                              const struct chrome9_ps_condition *condition,
                              const uint8_t condition_swizzle[4],
                              const struct chrome9_ps_source *when_true,
                              const struct chrome9_ps_source *when_false,
                              unsigned writemask, uint8_t temp)
{
   struct chrome9_ps_instruction *conditional;
   struct chrome9_ps_source arm[2] = {
      condition->invert ? *when_false : *when_true,
      condition->invert ? *when_true : *when_false,
   };

   if (!chrome9_ps_settle_modifier(builder, CHROME9_PS_OPCODE_CMP, 1,
                                   &arm[0]) ||
       !chrome9_ps_settle_modifier(builder, CHROME9_PS_OPCODE_CMP, 2,
                                   &arm[1]))
      return false;
   conditional = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_CMP);
   if (!conditional)
      return false;
   conditional->destination.file = CHROME9_PS_DESTINATION_TEMP;
   conditional->destination.id = temp;
   conditional->destination.writemask = writemask;
   conditional->source[0] = chrome9_ps_temp_source(condition->temp,
                                                   condition_swizzle);
   conditional->source[0].modifier = condition->negate ?
      CHROME9_PS_SOURCE_NEGATE : 0;
   conditional->source[1] = arm[0];
   conditional->source[2] = arm[1];
   return true;
}

/*
 * Select between two lowered values under a scalar boolean predicate without
 * materializing integers: ordered float compares become SUB+CMP, equality
 * SUB+ABS and a negated CMP condition.  Mixed conjunctions fall back to
 * nested or exchanged CMP arms.  Compares are admitted only on operands
 * proven free of defined exceptional values.
 */
static bool
chrome9_ps_emit_predicate_select(struct chrome9_ps_nir_builder *builder,
                                 nir_scalar predicate,
                                 const struct chrome9_ps_source *when_true,
                                 const struct chrome9_ps_source *when_false,
                                 unsigned components, unsigned depth,
                                 struct chrome9_ps_source *result)
{
   struct chrome9_ps_condition condition;
   struct chrome9_ps_source inner;
   uint8_t swizzle[4], temp;
   nir_alu_instr *alu;

   if (depth > 8)
      return false;
   predicate = nir_scalar_resolved(predicate.def, predicate.comp);
   if (predicate.def->bit_size != 1)
      return false;
   if (nir_scalar_is_const(predicate)) {
      *result = nir_scalar_as_bool(predicate) ? *when_true : *when_false;
      return true;
   }
   if (chrome9_ps_lower_condition(builder, predicate, 0, &condition)) {
      if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
         return false;
      temp = builder->next_temp++;
      for (unsigned c = 0; c < 4; c++)
         swizzle[c] = c < components ? CHROME9_PS_SWIZZLE_X :
                                       CHROME9_PS_SWIZZLE_ZERO;
      if (!chrome9_ps_emit_condition_cmp(builder, &condition, swizzle,
                                         when_true, when_false,
                                         BITFIELD_MASK(components), temp))
         return false;
      for (unsigned c = 0; c < 4; c++)
         swizzle[c] = c < components ? c : CHROME9_PS_SWIZZLE_ZERO;
      *result = chrome9_ps_temp_source(temp, swizzle);
      return true;
   }
   if (nir_def_instr(predicate.def)->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(nir_def_instr(predicate.def));
   switch (alu->op) {
   case nir_op_inot:
      return chrome9_ps_emit_predicate_select(
         builder, nir_scalar_chase_alu_src(predicate, 0), when_false,
         when_true, components, depth + 1, result);
   case nir_op_iand:
      return chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 1), when_true,
                when_false, components, depth + 1, &inner) &&
             chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 0), &inner,
                when_false, components, depth + 1, result);
   case nir_op_ior:
      return chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 1), when_true,
                when_false, components, depth + 1, &inner) &&
             chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 0), when_true,
                &inner, components, depth + 1, result);
   default:
      return false;
   }
}

/* Per-lane predicates: lanes of one compare share a vector condition, other
 * lanes are grouped by predicate, one CMP per group into the result. */
static bool
chrome9_ps_emit_lane_select(struct chrome9_ps_nir_builder *builder,
                            const nir_alu_src *predicate,
                            const struct chrome9_ps_source *when_true,
                            const struct chrome9_ps_source *when_false,
                            unsigned components,
                            struct chrome9_ps_source *result)
{
   struct chrome9_ps_condition condition;
   nir_scalar lane[4];
   uint8_t compare_lane[4], swizzle[4], temp;
   nir_alu_instr *compare = NULL;
   unsigned remaining = BITFIELD_MASK(components);

   if (components > 4)
      return false;
   for (unsigned c = 0; c < components; c++) {
      lane[c] = nir_scalar_resolved(predicate->src.ssa, predicate->swizzle[c]);
      compare_lane[c] = lane[c].comp;
   }
   if (nir_scalar_is_alu(lane[0]))
      compare = nir_instr_as_alu(nir_def_instr(lane[0].def));
   for (unsigned c = 1; c < components && compare; c++)
      if (lane[c].def != lane[0].def)
         compare = NULL;
   if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   temp = builder->next_temp++;
   for (unsigned c = 0; c < 4; c++)
      swizzle[c] = c < components ? c : CHROME9_PS_SWIZZLE_ZERO;
   if (compare &&
       chrome9_ps_lower_compare_lanes(builder, compare, compare_lane,
                                      components, &condition)) {
      remaining = 0;
      if (!chrome9_ps_emit_condition_cmp(builder, &condition, swizzle,
                                         when_true, when_false,
                                         BITFIELD_MASK(components), temp))
         return false;
   }
   while (remaining) {
      const unsigned first = __builtin_ctz(remaining);
      unsigned mask = 0;

      if (nir_scalar_is_const(lane[first])) {
         struct chrome9_ps_source arm = nir_scalar_as_bool(lane[first]) ?
            *when_true : *when_false;
         struct chrome9_ps_instruction *move;

         if (!chrome9_ps_settle_modifier(builder, CHROME9_PS_OPCODE_MOV, 0,
                                         &arm))
            return false;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         for (unsigned c = first; c < components; c++)
            if (nir_scalar_is_const(lane[c]) &&
                nir_scalar_as_bool(lane[c]) ==
                   nir_scalar_as_bool(lane[first]))
               mask |= 1u << c;
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = temp;
         move->destination.writemask = mask;
         move->source[0] = arm;
      } else {
         uint8_t condition_swizzle[4];

         if (!chrome9_ps_lower_condition(builder, lane[first], 0,
                                         &condition))
            return false;
         for (unsigned c = first; c < components; c++)
            if (nir_scalar_equal(lane[c], lane[first]))
               mask |= 1u << c;
         for (unsigned c = 0; c < 4; c++)
            condition_swizzle[c] = mask & (1u << c) ?
               CHROME9_PS_SWIZZLE_X : CHROME9_PS_SWIZZLE_ZERO;
         if (!chrome9_ps_emit_condition_cmp(builder, &condition,
                                            condition_swizzle, when_true,
                                            when_false, mask, temp))
            return false;
      }
      remaining &= ~mask;
   }
   *result = chrome9_ps_temp_source(temp, swizzle);
   return true;
}

static bool
chrome9_ps_record_value(struct chrome9_ps_nir_builder *builder, nir_def *def,
                        const struct chrome9_ps_source *result,
                        struct chrome9_ps_source *source)
{
   unsigned value;

   if (builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source = *result;
   builder->value[value].complete = true;
   *source = *result;
   return true;
}

/* General bcsel and b2f32 under a broadcast or per-lane predicate. */
static bool
chrome9_ps_lower_general_select(struct chrome9_ps_nir_builder *builder,
                                nir_alu_instr *alu, nir_def *def,
                                struct chrome9_ps_source *source)
{
   static const uint32_t one[4] = {
      0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
   };
   static const uint32_t zero[4] = { 0 };
   const unsigned predicate_src = 0;
   unsigned components = def->num_components;
   struct chrome9_ps_source when_true, when_false, result;
   bool lane_predicates = false;

   if ((alu->op != nir_op_bcsel && alu->op != nir_op_b2f32) ||
       !components || components > 4 ||
       !alu->src[predicate_src].src.ssa ||
       alu->src[predicate_src].src.ssa->bit_size != 1)
      return false;
   for (unsigned c = 1; c < components; c++)
      if (alu->src[predicate_src].swizzle[c] !=
          alu->src[predicate_src].swizzle[0])
         lane_predicates = true;
   if (alu->op == nir_op_bcsel) {
      /* Arms keep NEG for the B slot; other slots settle it. */
      if (!chrome9_ps_lower_alu_source_modifier(builder, &alu->src[1],
                                                components, &when_true) ||
          !chrome9_ps_lower_alu_source_modifier(builder, &alu->src[2],
                                                components, &when_false))
         return false;
   } else if (!chrome9_ps_immediate(builder, one, components, &when_true) ||
              !chrome9_ps_immediate(builder, zero, components, &when_false)) {
      return false;
   }
   if (lane_predicates)
      return chrome9_ps_emit_lane_select(builder, &alu->src[predicate_src],
                                         &when_true, &when_false,
                                         components, &result) &&
             chrome9_ps_record_value(builder, def, &result, source);
   /* A constant predicate forwards an arm; cached values carry no NEG. */
   return chrome9_ps_emit_predicate_select(
             builder,
             (nir_scalar) { alu->src[predicate_src].src.ssa,
                            alu->src[predicate_src].swizzle[0] },
             &when_true, &when_false, components, 0, &result) &&
          chrome9_ps_settle_modifier(builder, CHROME9_PS_OPCODE_KILL, 0,
                                     &result) &&
          chrome9_ps_record_value(builder, def, &result, source);
}

/* sign(x) of a finite x: CMP(-x, 0, 1) is 1 for x > 0 and 0 otherwise,
 * CMP(x, that, -1) adds -1 for x < 0; both signed zeros give 0. */
static bool
chrome9_ps_lower_sign(struct chrome9_ps_nir_builder *builder,
                      nir_alu_instr *alu, nir_def *def,
                      struct chrome9_ps_source *source)
{
   static const uint32_t zero[4] = { 0 };
   static const uint32_t one[4] = {
      0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
   };
   static const uint32_t minus_one[4] = {
      0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000,
   };
   static const uint8_t identity[4] = { 0, 1, 2, 3 };
   const unsigned components = def->num_components;
   struct chrome9_ps_source operand[3], value;
   uint8_t swizzle[4] = {
      CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO,
      CHROME9_PS_SWIZZLE_ZERO, CHROME9_PS_SWIZZLE_ZERO,
   };
   uint8_t positive, sign;

   if (!chrome9_ps_default_finite_alu_source(alu, 0, components,
                                             &builder->proof) ||
       !chrome9_ps_lower_alu_source(builder, &alu->src[0], components,
                                    &value))
      return false;
   operand[0] = value;
   operand[0].modifier ^= CHROME9_PS_SOURCE_NEGATE;
   if (operand[0].modifier & ~CHROME9_PS_SOURCE_NEGATE ||
       value.file == CHROME9_PS_SOURCE_POSITION ||
       !chrome9_ps_immediate(builder, zero, components, &operand[1]) ||
       !chrome9_ps_immediate(builder, one, components, &operand[2]) ||
       !chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_CMP, operand, 3,
                             BITFIELD_MASK(components), &positive))
      return false;
   memcpy(swizzle, identity, components);
   operand[0] = value;
   operand[1] = chrome9_ps_temp_source(positive, swizzle);
   if (!chrome9_ps_immediate(builder, minus_one, components, &operand[2]) ||
       !chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_CMP, operand, 3,
                             BITFIELD_MASK(components), &sign))
      return false;
   operand[0] = chrome9_ps_temp_source(sign, swizzle);
   return chrome9_ps_record_value(builder, def, &operand[0], source);
}

static bool
chrome9_ps_lower_def_impl(struct chrome9_ps_nir_builder *builder,
                          nir_def *def, struct chrome9_ps_source *source);
/* The hardware's guarded projector form: coordinate varying.xy over the
 * same varying's W. */
static bool
chrome9_ps_identity_projector(const nir_tex_instr *tex, int coord,
                              int projector,
                              const struct chrome9_ps_source *coordinate)
{
   nir_scalar x = nir_scalar_resolved(tex->src[coord].src.ssa, 0);
   nir_scalar y = nir_scalar_resolved(tex->src[coord].src.ssa, 1);
   nir_scalar q = nir_scalar_resolved(tex->src[projector].src.ssa, 0);

   return coordinate->file == CHROME9_PS_SOURCE_TEXTURE &&
          coordinate->swizzle[0] == CHROME9_PS_SWIZZLE_X &&
          coordinate->swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
          x.def == q.def && y.def == q.def &&
          x.comp == 0 && y.comp == 1 && q.comp == 3;
}


/* CHROME9_DEBUG_REJECT prints each rejected def, innermost first. */
static bool
chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder, nir_def *def,
                     struct chrome9_ps_source *source)
{
   if (chrome9_ps_lower_def_impl(builder, def, source))
      return true;
   if (def && debug_get_bool_option("CHROME9_DEBUG_REJECT", false)) {
      fprintf(stderr, "chrome9: reject ir=%u temps=%u: ",
              builder->ir.count, builder->next_temp);
      nir_print_instr(nir_def_instr(def), stderr);
      fputc('\n', stderr);
   }
   return false;
}

static bool
chrome9_ps_lower_def_impl(struct chrome9_ps_nir_builder *builder,
                          nir_def *def, struct chrome9_ps_source *source)
{
   nir_instr *instr;
   nir_alu_instr *alu;
   struct chrome9_ps_instruction *native;
   struct chrome9_ps_source operands[3] = { 0 };
   enum chrome9_ps_opcode op;
   unsigned source_count;
   unsigned source_components[3], order[3], negate = 0;
   nir_alu_src folded[3];
   const nir_alu_src *alu_sources;
   unsigned components;
   unsigned value;
   uint8_t temp;

   if (!def || def->bit_size != 32 || !def->num_components ||
       def->num_components > 4)
      return false;
   for (value = 0; value < builder->value_count; value++) {
      if (builder->value[value].def != def)
         continue;
      if (!builder->value[value].complete)
         return false;
      *source = builder->value[value].source;
      return true;
   }
   if (chrome9_ps_leaf(builder, def, source))
      return true;
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *vec = nir_instr_as_alu(instr);
      unsigned vector_components = 0;
      struct chrome9_ps_source component[4] = { 0 };
      struct chrome9_ps_source vector = { 0 };
      bool alias = true;

      if (vec->op == nir_op_vec2)
         vector_components = 2;
      else if (vec->op == nir_op_vec3)
         vector_components = 3;
      else if (vec->op == nir_op_vec4)
         vector_components = 4;
      if (vector_components) {
         uint8_t selector[4] = { 0 };
         int base = -1;

         /* Literal 0.0 and 1.0 lanes are ZERO/ONE selectors of the
          * vector built from the other lanes. */
         for (unsigned c = 0; c < vector_components; c++) {
            nir_scalar lane = nir_scalar_resolved(vec->src[c].src.ssa,
                                                  vec->src[c].swizzle[0]);

            if (nir_scalar_is_const(lane)) {
               const uint32_t bits = nir_scalar_as_uint(lane);

               if (bits == 0)
                  selector[c] = CHROME9_PS_SWIZZLE_ZERO;
               else if (bits == 0x3f800000)
                  selector[c] = CHROME9_PS_SWIZZLE_ONE;
            }
         }
         for (unsigned c = 0; c < vector_components; c++) {
            if (!chrome9_ps_lower_alu_source(builder, &vec->src[c], 1,
                                              &component[c]))
               return false;
            if (selector[c])
               continue;
            if (base < 0) {
               base = c;
               vector = component[c];
            } else if (component[c].file != vector.file ||
                       component[c].id != vector.id ||
                       component[c].modifier != vector.modifier) {
               alias = false;
            }
         }
         /* All-literal vectors alias one constant row or take MOVs. */
         if (base < 0) {
            vector = component[0];
            for (unsigned c = 1; c < vector_components; c++)
               if (component[c].file != vector.file ||
                   component[c].id != vector.id ||
                   component[c].modifier != vector.modifier)
                  alias = false;
            memset(selector, 0, sizeof(selector));
         }
         for (unsigned c = 0; c < vector_components && alias; c++)
            vector.swizzle[c] = selector[c] ? selector[c] :
                                component[c].swizzle[0];
         if (alias) {
            for (unsigned c = vector_components; c < 4; c++)
               vector.swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
            *source = vector;
            return true;
         }
         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
             builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
            return false;
         temp = builder->next_temp++;
         value = builder->value_count++;
         builder->value[value].def = def;
         builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
         builder->value[value].source.id = temp;
         for (unsigned c = 0; c < 4; c++)
            builder->value[value].source.swizzle[c] = c < vector_components ?
               c : CHROME9_PS_SWIZZLE_ZERO;
         for (unsigned remaining = (1u << vector_components) - 1;
              remaining;) {
            struct chrome9_ps_instruction *move;
            unsigned first = __builtin_ctz(remaining);
            unsigned mask = 0;

            for (unsigned c = 0; c < vector_components; c++) {
               if (!(remaining & (1u << c)))
                  continue;
               if (component[c].file == component[first].file &&
                   component[c].id == component[first].id &&
                   component[c].modifier == component[first].modifier)
                  mask |= 1u << c;
            }
            move = chrome9_ps_append_instruction(builder,
                                                  CHROME9_PS_OPCODE_MOV);
            if (!move)
               return false;
            move->destination.file = CHROME9_PS_DESTINATION_TEMP;
            move->destination.id = temp;
            move->destination.writemask = mask;
            move->source[0] = component[first];
            for (unsigned c = 0; c < 4; c++)
               move->source[0].swizzle[c] =
                  (mask & (1u << c)) ? component[c].swizzle[0] :
                  CHROME9_PS_SWIZZLE_ZERO;
            remaining &= ~mask;
         }
         builder->value[value].complete = true;
         *source = builder->value[value].source;
         return true;
      }
   }
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      struct chrome9_ps_instruction *texture;
      struct chrome9_ps_source coordinate;
      int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      int projector = nir_tex_instr_src_index(tex, nir_tex_src_projector);
      int bias = nir_tex_instr_src_index(tex, nir_tex_src_bias);
      /* Cube and 3D lookups are selected by the texture state; TEX reads
       * STR. */
      const bool volume = tex->sampler_dim == GLSL_SAMPLER_DIM_3D;
      const bool str = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE || volume;
      const unsigned lanes = str ? 3 : 2;

      /* TEXLDB: 2D only, LOD bias in coordinate lane W. */
      if ((tex->op != nir_texop_tex &&
           (tex->op != nir_texop_txb || !builder->allow_bias || str ||
            projector >= 0 || bias < 0 ||
            tex->src[bias].src.ssa->num_components != 1)) ||
          (tex->sampler_dim != GLSL_SAMPLER_DIM_2D && !str) ||
          tex->is_array || tex->is_shadow || tex->texture_index >= 8 ||
          tex->sampler_index != tex->texture_index || coord < 0 ||
          tex->src[coord].src.ssa->num_components != lanes ||
          (str && projector >= 0) ||
          builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
          builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
         return false;
      for (unsigned s = 0; s < tex->num_srcs; s++)
         if ((int)s != coord && (int)s != projector && (int)s != bias)
            return false;
      if (!chrome9_ps_lower_def(builder, tex->src[coord].src.ssa,
                                &coordinate) ||
          coordinate.swizzle[0] > CHROME9_PS_SWIZZLE_W ||
          coordinate.swizzle[1] > CHROME9_PS_SWIZZLE_W ||
          (str && coordinate.swizzle[2] > CHROME9_PS_SWIZZLE_W))
         return false;
      if (str && coordinate.file != CHROME9_PS_SOURCE_TEMP &&
          (coordinate.file != CHROME9_PS_SOURCE_TEXTURE ||
           coordinate.swizzle[0] != CHROME9_PS_SWIZZLE_X ||
           coordinate.swizzle[1] != CHROME9_PS_SWIZZLE_Y ||
           coordinate.swizzle[2] != CHROME9_PS_SWIZZLE_Z)) {
         /* A direct STR coordinate is the varying's XYZ; anything else
          * reaches TEX as STR of a computed TEMP. */
         struct chrome9_ps_instruction *move;

         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = builder->next_temp++;
         move->destination.writemask = 0x7;
         move->source[0] = coordinate;
         move->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         memset(&coordinate, 0, sizeof(coordinate));
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = move->destination.id;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
         coordinate.swizzle[2] = CHROME9_PS_SWIZZLE_Z;
      }
      if (projector >= 0 &&
          !chrome9_ps_identity_projector(tex, coord, projector,
                                         &coordinate)) {
         struct chrome9_ps_source q;
         struct chrome9_ps_instruction *rcp, *mul;
         uint8_t rcp_temp, coord_temp;

         /* General projective lookup: TEX samples coord.xy * (1 / q)
          * through the computed-coordinate bridge. */
         if (tex->num_srcs != 2 ||
             tex->src[projector].src.ssa->num_components != 1 ||
             !chrome9_ps_lower_def(builder, tex->src[projector].src.ssa,
                                   &q) ||
             builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - 2)
            return false;
         rcp_temp = builder->next_temp++;
         coord_temp = builder->next_temp++;
         rcp = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_RCP);
         if (!rcp)
            return false;
         rcp->destination.file = CHROME9_PS_DESTINATION_TEMP;
         rcp->destination.id = rcp_temp;
         rcp->destination.writemask = 0x1;
         rcp->source[0] = q;
         mul = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MUL);
         if (!mul)
            return false;
         mul->destination.file = CHROME9_PS_DESTINATION_TEMP;
         mul->destination.id = coord_temp;
         mul->destination.writemask = 0x3;
         mul->source[0] = coordinate;
         mul->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         mul->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         memset(&mul->source[1], 0, sizeof(mul->source[1]));
         mul->source[1].file = CHROME9_PS_SOURCE_TEMP;
         mul->source[1].id = rcp_temp;
         for (unsigned c = 0; c < 4; c++)
            mul->source[1].swizzle[c] = c < 2 ? CHROME9_PS_SWIZZLE_X :
                                        CHROME9_PS_SWIZZLE_ZERO;
         memset(&coordinate, 0, sizeof(coordinate));
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = coord_temp;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
         projector = -1;
      }
      if (!str && coordinate.file != CHROME9_PS_SOURCE_TEXTURE &&
          coordinate.file != CHROME9_PS_SOURCE_TEMP) {
         /* Constant or uniform coordinates reach TEX through a TEMP. */
         struct chrome9_ps_instruction *move;

         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = builder->next_temp++;
         move->destination.writemask = 0x3;
         move->source[0] = coordinate;
         move->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         move->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         memset(&coordinate, 0, sizeof(coordinate));
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = move->destination.id;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
      }
      if (projector >= 0) {
         struct chrome9_ps_source q;

         /* A native 2D TEX has no projective divide.  Accept only the
          * matching direct varying's W lane and guard its post-TNL value
          * across the entire draw before submitting any triangle. */
         if (tex->num_srcs != 2 ||
             tex->src[projector].src.ssa->num_components != 1 ||
             coordinate.file != CHROME9_PS_SOURCE_TEXTURE ||
             coordinate.id >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS ||
             coordinate.swizzle[0] != CHROME9_PS_SWIZZLE_X ||
             coordinate.swizzle[1] != CHROME9_PS_SWIZZLE_Y ||
             !chrome9_ps_lower_def(builder,
                                   tex->src[projector].src.ssa, &q) ||
             q.file != coordinate.file || q.id != coordinate.id ||
             q.swizzle[0] != CHROME9_PS_SWIZZLE_W)
            return false;
         builder->identity_projector_input_mask |=
            1u << (coordinate.id * 4 + 3);
      } else if (!str && coordinate.file == CHROME9_PS_SOURCE_TEXTURE &&
                 (coordinate.swizzle[0] != CHROME9_PS_SWIZZLE_X ||
                  coordinate.swizzle[1] != CHROME9_PS_SWIZZLE_Y)) {
         /* Like D3D9 texld, a direct coordinate is the varying's XY; the
          * FE consumer is capped at three lanes.  Route a swizzled one
          * (e.g. packed lightmap .zw) through the computed-TEMP bridge. */
         struct chrome9_ps_instruction *move;

         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = builder->next_temp++;
         move->destination.writemask = 0x3;
         move->source[0] = coordinate;
         move->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         move->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = move->destination.id;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
      }
      if (bias >= 0) {
         struct chrome9_ps_instruction *move;
         struct chrome9_ps_source lod;
         uint8_t biased;

         /* (s, t, 0, bias) through the computed-coordinate bridge */
         if (!chrome9_ps_lower_def(builder, tex->src[bias].src.ssa, &lod) ||
             builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         biased = builder->next_temp++;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = biased;
         move->destination.writemask = 0x3;
         move->source[0] = coordinate;
         move->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         move->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = biased;
         move->destination.writemask = 0x8;
         move->source[0] = lod;
         for (unsigned c = 1; c < 4; c++)
            move->source[0].swizzle[c] = lod.swizzle[0];
         memset(&coordinate, 0, sizeof(coordinate));
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = biased;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
      }
      if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
         return false;
      texture = chrome9_ps_append_instruction(builder,
                                               CHROME9_PS_OPCODE_TEX);
      if (!texture)
         return false;
      temp = builder->next_temp++;
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
      builder->value[value].source.id = temp;
      for (unsigned c = 0; c < 4; c++)
         builder->value[value].source.swizzle[c] = c;
      texture->sampler = tex->sampler_index;
      texture->destination.file = CHROME9_PS_DESTINATION_TEMP;
      texture->destination.id = temp;
      texture->destination.writemask = 0xf;
      texture->source[0] = coordinate;
      if (str) {
         if (volume)
            builder->volume_sampler_mask |= 1u << tex->sampler_index;
         else
            builder->cube_sampler_mask |= 1u << tex->sampler_index;
         texture->source[0].swizzle[3] =
            coordinate.file == CHROME9_PS_SOURCE_TEMP ?
            CHROME9_PS_SWIZZLE_ONE : CHROME9_PS_SWIZZLE_W;
      } else if (coordinate.file == CHROME9_PS_SOURCE_TEMP) {
         /* 2D TEX reads XY.  An XY-only computed TEMP must not appear to
          * read undefined Z/W lanes during kernel validation. */
         texture->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         texture->source[0].swizzle[3] = bias >= 0 ?
            CHROME9_PS_SWIZZLE_W : CHROME9_PS_SWIZZLE_ONE;
         if (bias >= 0)
            texture->flags = DRM_CHROME9_FRAGMENT_TEX_BIAS;
      } else {
         /* Direct varyings retain their native four-lane coordinate map;
          * the kernel's direct-input gate does not admit literals here. */
         texture->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_W;
         texture->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_W;
      }
      builder->value[value].complete = true;
      *source = builder->value[value].source;
      return true;
   }
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   alu_sources = alu->src;
   components = alu->def.num_components;
   if (alu->op == nir_op_fsat && alu->src[0].src.ssa &&
       list_is_singular(&alu->src[0].src.ssa->uses) &&
       !nir_def_used_by_if(alu->src[0].src.ssa) &&
       chrome9_ps_lower_alu_source(builder, &alu->src[0], components,
                                    &operands[0]) &&
       builder->value_count < CHROME9_PS_MAX_INSTRUCTIONS &&
       chrome9_ps_fuse_source_saturate(builder, alu->src[0].src.ssa,
                                        &operands[0])) {
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source = operands[0];
      builder->value[value].complete = true;
      *source = operands[0];
      return true;
   }
   if (alu->op == nir_op_bcsel)
      return chrome9_ps_lower_unit_interval_select(builder, alu, def,
                                                    source) ||
             chrome9_ps_lower_general_select(builder, alu, def, source);
   if (alu->op == nir_op_b2f32)
      return chrome9_ps_lower_general_select(builder, alu, def, source);
   if (alu->op == nir_op_ftrunc || alu->op == nir_op_fceil)
      return chrome9_ps_lower_round(builder, alu, def, source);
   if (alu->op == nir_op_fsign)
      return chrome9_ps_lower_sign(builder, alu, def, source);
   for (unsigned s = 0; s < ARRAY_SIZE(source_components); s++)
      source_components[s] = components;
   if (alu->op == nir_op_fpow) {
      struct chrome9_ps_instruction *log2, *multiply, *exp2;
      uint32_t integer_exponent = 0;
      uint8_t log_temp, multiply_temp;

      /* Integer powers use multiplication, which also preserves zero raised
       * to a positive exponent.  Follow bit-exact MOV/vec aliases before
       * deciding whether the exponent is constant.  Other scalar powers use
       * the native LOG2, MUL and EXP2 sequence. */
      if (components == 1 && alu->src[1].src.ssa &&
          alu->src[1].src.ssa->bit_size == 32 &&
          alu->src[1].swizzle[0] <
             alu->src[1].src.ssa->num_components) {
         nir_scalar exponent = nir_scalar_resolved(
            alu->src[1].src.ssa, alu->src[1].swizzle[0]);

         if (nir_scalar_is_const(exponent)) {
            double power = nir_scalar_as_float(exponent);

            /* Every finite float32 value this large is integral.  The
             * multiplication chain uses a uint32_t exponent; rejecting a
             * larger integer avoids losing pow(0, positive) semantics. */
            if (power >= 0x1p32 && power < 0x1p128)
               return false;
            if (power >= 1.0 && power < 0x1p32 &&
                power == (double)(uint32_t)power)
               integer_exponent = (uint32_t)power;
            /* Chains longer than LOG2, MUL, EXP2 take those three; on
             * silicon they give pow(0, n) = 0 and match pow(x, 100) to
             * the 8-bit result. */
            if (integer_exponent &&
                util_logbase2(integer_exponent) +
                   util_bitcount(integer_exponent) - 1 > 3)
               integer_exponent = 0;
         }
      }
      if (integer_exponent &&
          (integer_exponent == 1 ||
           builder->value_count < CHROME9_PS_MAX_INSTRUCTIONS)) {
         struct chrome9_ps_source powered;

         if (!chrome9_ps_lower_alu_source(builder, &alu->src[0], 1,
                                          &operands[0]))
            return false;
         if (chrome9_ps_emit_integer_pow(&builder->ir, operands[0],
                                         integer_exponent,
                                         &builder->next_temp,
                                         &builder->alu_count, &powered)) {
            if (integer_exponent > 1) {
               value = builder->value_count++;
               builder->value[value].def = def;
               builder->value[value].source = powered;
               builder->value[value].complete = true;
            }
            *source = powered;
            return true;
         }
      }
      if (integer_exponent)
         return false;
      /* Vector POW is scalarized by NIR before this scalar backend. */
      if (components != 1 ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[0], 1,
                                       &operands[0]) ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[1], 1,
                                       &operands[1]) ||
          builder->alu_count > CHROME9_PS_MAX_ALU - 3 ||
          builder->ir.count > CHROME9_PS_MAX_INSTRUCTIONS - 3 ||
          builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - 3 ||
          builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
         return false;

      log_temp = builder->next_temp++;
      multiply_temp = builder->next_temp++;
      temp = builder->next_temp++;
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
      builder->value[value].source.id = temp;
      builder->value[value].source.swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         builder->value[value].source.swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;

      log2 = chrome9_ps_append_instruction(builder,
                                            CHROME9_PS_OPCODE_LOG2);
      multiply = chrome9_ps_append_instruction(builder,
                                                CHROME9_PS_OPCODE_MUL);
      exp2 = chrome9_ps_append_instruction(builder,
                                            CHROME9_PS_OPCODE_EXP2);
      if (!log2 || !multiply || !exp2)
         return false;
      log2->destination.file = CHROME9_PS_DESTINATION_TEMP;
      log2->destination.id = log_temp;
      log2->destination.writemask = 1;
      log2->source[0] = operands[0];

      multiply->destination.file = CHROME9_PS_DESTINATION_TEMP;
      multiply->destination.id = multiply_temp;
      multiply->destination.writemask = 1;
      multiply->source[0].file = CHROME9_PS_SOURCE_TEMP;
      multiply->source[0].id = log_temp;
      multiply->source[0].swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         multiply->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
      multiply->source[1] = operands[1];

      exp2->destination.file = CHROME9_PS_DESTINATION_TEMP;
      exp2->destination.id = temp;
      exp2->destination.writemask = 1;
      exp2->source[0].file = CHROME9_PS_SOURCE_TEMP;
      exp2->source[0].id = multiply_temp;
      exp2->source[0].swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         exp2->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;

      builder->value[value].complete = true;
      *source = builder->value[value].source;
      return true;
   }
   switch (alu->op) {
   case nir_op_mov:
      return chrome9_ps_lower_alu_source(builder, &alu->src[0], components,
                                         source);
   case nir_op_fsat:
      /* Keep saturation on a distinct instruction: the producer may have
       * other unsaturated users or only some lanes may be clamped. */
      op = CHROME9_PS_OPCODE_MOV;
      source_count = 1;
      break;
   case nir_op_fmul:
      op = CHROME9_PS_OPCODE_MUL;
      source_count = 2;
      break;
   case nir_op_fadd:
      op = CHROME9_PS_OPCODE_ADD;
      source_count = 2;
      break;
   case nir_op_fsub:
      op = CHROME9_PS_OPCODE_SUB;
      source_count = 2;
      break;
   case nir_op_ffma:
      op = CHROME9_PS_OPCODE_MAD;
      source_count = 3;
      break;
   case nir_op_fmin:
      op = CHROME9_PS_OPCODE_MIN;
      source_count = 2;
      break;
   case nir_op_fmax:
      op = CHROME9_PS_OPCODE_MAX;
      source_count = 2;
      break;
   case nir_op_fdot2:
      /* The native dot product consumes XYZ.  Lowering two components pads
       * Z/W with the literal ZERO selector in lower_alu_source(). */
      op = CHROME9_PS_OPCODE_DP3;
      source_count = 2;
      source_components[0] = source_components[1] = 2;
      break;
   case nir_op_fdot3:
      op = CHROME9_PS_OPCODE_DP3;
      source_count = 2;
      source_components[0] = source_components[1] = 3;
      break;
   case nir_op_fdot4:
      op = CHROME9_PS_OPCODE_DP4;
      source_count = 2;
      source_components[0] = source_components[1] = 4;
      break;
   case nir_op_fexp2:
      op = CHROME9_PS_OPCODE_EXP2;
      source_count = 1;
      break;
   case nir_op_flog2:
      op = CHROME9_PS_OPCODE_LOG2;
      source_count = 1;
      break;
   case nir_op_frcp:
      op = CHROME9_PS_OPCODE_RCP;
      source_count = 1;
      break;
   case nir_op_frsq:
      op = CHROME9_PS_OPCODE_RSQ;
      source_count = 1;
      break;
   case nir_op_fabs:
      op = CHROME9_PS_OPCODE_ABS;
      source_count = 1;
      break;
   case nir_op_ffract:
      op = CHROME9_PS_OPCODE_FRC;
      source_count = 1;
      break;
   case nir_op_fneg:
      op = CHROME9_PS_OPCODE_MUL;
      source_count = 1;
      break;
   default:
      return false;
   }
   if (builder->alu_count >= CHROME9_PS_MAX_ALU ||
       builder->ir.count >= CHROME9_PS_MAX_INSTRUCTIONS ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   temp = builder->next_temp++;
   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
   builder->value[value].source.id = temp;
   for (unsigned c = 0; c < 4; c++)
      builder->value[value].source.swizzle[c] = c < components ?
         c : CHROME9_PS_SWIZZLE_ZERO;
   if (alu->op == nir_op_fneg) {
      if (chrome9_ps_fold_negation(builder, alu, components, folded, &op,
                                   &source_count, &negate)) {
         alu_sources = folded;
      } else if (chrome9_ps_source_modifier_allowed(CHROME9_PS_OPCODE_MOV,
                                                    0)) {
         op = CHROME9_PS_OPCODE_MOV;
         negate = 1;
      }
   }
   chrome9_ps_source_order(op, alu_sources, source_count, order);
   for (unsigned s = 0; s < source_count; s++) {
      bool modifier = chrome9_ps_source_modifier_allowed(op, s);
      const nir_alu_src *nir_source = &alu_sources[order[s]];

      if (!(modifier ?
            chrome9_ps_lower_alu_source_modifier(builder, nir_source,
                                                  source_components[s],
                                                  &operands[s]) :
            chrome9_ps_lower_alu_source(builder, nir_source,
                                        source_components[s], &operands[s])))
         return false;
      if (negate & (1u << s)) {
         operands[s].modifier ^= CHROME9_PS_SOURCE_NEGATE;
         if (!chrome9_ps_settle_modifier(builder, op, s, &operands[s]))
            return false;
      }
   }
   if (alu->op == nir_op_fneg && op == CHROME9_PS_OPCODE_MUL && !negate) {
      static const uint32_t negative_one[4] = {
         0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000,
      };

      if (!chrome9_ps_immediate(builder, negative_one, components,
                                &operands[1]))
         return false;
      source_count = 2;
   }
   /* EXP2/LOG2/RCP/RSQ read one source lane and replicate the result:
    * vector forms become one record per distinct lane. */
   if ((op == CHROME9_PS_OPCODE_EXP2 || op == CHROME9_PS_OPCODE_LOG2 ||
        op == CHROME9_PS_OPCODE_RCP || op == CHROME9_PS_OPCODE_RSQ) &&
       components > 1) {
      for (unsigned c = 0; c < components; c++) {
         native = chrome9_ps_append_instruction(builder, op);
         if (!native)
            return false;
         native->destination.file = CHROME9_PS_DESTINATION_TEMP;
         native->destination.id = temp;
         native->destination.writemask = 1u << c;
         native->source[0] = operands[0];
         for (unsigned l = 0; l < 4; l++)
            native->source[0].swizzle[l] = operands[0].swizzle[c];
      }
      builder->value[value].complete = true;
      *source = builder->value[value].source;
      return true;
   }
   native = chrome9_ps_append_instruction(builder, op);
   if (!native)
      return false;
   native->destination.file = CHROME9_PS_DESTINATION_TEMP;
   native->destination.id = temp;
   native->destination.writemask = (1u << components) - 1;
   native->destination.saturate = alu->op == nir_op_fsat;
   for (unsigned s = 0; s < source_count; s++)
      native->source[s] = operands[s];
   builder->value[value].complete = true;
   *source = builder->value[value].source;
   return true;
}

/*
 * Integer rounding from FRC of |x| only: on negative inputs FRC returns 1.0
 * just below an integer (silicon), which makes x - FRC(x) one too small.
 * t = |x| - FRC(|x|), c = t + (FRC(|x|) > 0); ftrunc = x >= 0 ? t : -t,
 * fceil = x >= 0 ? c : -t.  NEG is qualified on the CMP A operand only.
 */
static bool
chrome9_ps_lower_round(struct chrome9_ps_nir_builder *builder,
                       nir_alu_instr *alu, nir_def *def,
                       struct chrome9_ps_source *source)
{
   static const uint32_t zero_one[4] = { 0, 0x3f800000 };
   static const enum chrome9_ps_opcode trunc_ops[] = {
      CHROME9_PS_OPCODE_ABS, CHROME9_PS_OPCODE_FRC, CHROME9_PS_OPCODE_SUB,
      CHROME9_PS_OPCODE_SUB, CHROME9_PS_OPCODE_CMP,
   };
   static const enum chrome9_ps_opcode ceil_ops[] = {
      CHROME9_PS_OPCODE_ABS, CHROME9_PS_OPCODE_FRC, CHROME9_PS_OPCODE_SUB,
      CHROME9_PS_OPCODE_SUB, CHROME9_PS_OPCODE_CMP, CHROME9_PS_OPCODE_ADD,
      CHROME9_PS_OPCODE_CMP,
   };
   const bool ceil = alu->op == nir_op_fceil;
   const enum chrome9_ps_opcode *ops = ceil ? ceil_ops : trunc_ops;
   const unsigned count = ceil ? ARRAY_SIZE(ceil_ops) : ARRAY_SIZE(trunc_ops);
   const unsigned components = def->num_components;
   struct chrome9_ps_instruction *native[ARRAY_SIZE(ceil_ops)];
   struct chrome9_ps_source x, constant, zero, one;
   struct chrome9_ps_source temp[ARRAY_SIZE(ceil_ops)];
   unsigned value;

   if (!chrome9_ps_lower_alu_source(builder, &alu->src[0], components, &x) ||
       (ceil && !chrome9_ps_immediate(builder, zero_one, 2, &constant)) ||
       builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - count ||
       builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   for (unsigned i = 0; i < count; i++) {
      native[i] = chrome9_ps_append_instruction(builder, ops[i]);
      if (!native[i])
         return false;
      memset(&temp[i], 0, sizeof(temp[i]));
      temp[i].file = CHROME9_PS_SOURCE_TEMP;
      temp[i].id = builder->next_temp++;
      for (unsigned c = 0; c < 4; c++)
         temp[i].swizzle[c] = c < components ? c : CHROME9_PS_SWIZZLE_ZERO;
      native[i]->destination.file = CHROME9_PS_DESTINATION_TEMP;
      native[i]->destination.id = temp[i].id;
      native[i]->destination.writemask = (1u << components) - 1;
   }
   /* a = |x|, f = FRC a, t = a - f, n = f - a */
   native[0]->source[0] = x;
   native[1]->source[0] = temp[0];
   native[2]->source[0] = temp[0];
   native[2]->source[1] = temp[1];
   native[3]->source[0] = temp[1];
   native[3]->source[1] = temp[0];
   if (ceil) {
      zero = one = constant;
      for (unsigned c = 0; c < 4; c++) {
         zero.swizzle[c] = c < components ? constant.swizzle[0] :
                           CHROME9_PS_SWIZZLE_ZERO;
         one.swizzle[c] = c < components ? constant.swizzle[1] :
                          CHROME9_PS_SWIZZLE_ZERO;
      }
      /* s = -f >= 0 ? 0 : 1, c = t + s, r = x >= 0 ? c : n */
      native[4]->source[0] = temp[1];
      native[4]->source[0].modifier = CHROME9_PS_SOURCE_NEGATE;
      native[4]->source[1] = zero;
      native[4]->source[2] = one;
      native[5]->source[0] = temp[2];
      native[5]->source[1] = temp[4];
      native[6]->source[0] = x;
      native[6]->source[1] = temp[5];
      native[6]->source[2] = temp[3];
   } else {
      /* r = x >= 0 ? t : n */
      native[4]->source[0] = x;
      native[4]->source[1] = temp[2];
      native[4]->source[2] = temp[3];
   }

   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source = temp[count - 1];
   builder->value[value].complete = true;
   *source = temp[count - 1];
   return true;
}

static bool
chrome9_ps_sample_alpha(nir_scalar scalar, nir_tex_instr **texture)
{
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.comp != 3 || scalar.def->num_components != 4 ||
       scalar.def->bit_size != 32 ||
       nir_def_instr(scalar.def)->type != nir_instr_type_tex)
      return false;
   *texture = nir_instr_as_tex(nir_def_instr(scalar.def));
   return true;
}

/*
 * discard if (a == 0): ABS t0 = |a|; CMP t1 = (-t0 >= 0) ? -1 : +1; KILL t1.
 * Only +0 and -0 give -|a| >= 0, so no draw-time range proof is needed.
 */
static bool
chrome9_ps_lower_kill_zero(struct chrome9_ps_nir_builder *builder,
                           const struct chrome9_ps_source *alpha)
{
   static const uint32_t signs[4] = { 0xbf800000, 0x3f800000 };
   struct chrome9_ps_instruction *absolute, *conditional, *kill;
   struct chrome9_ps_source sign, negative_one, positive_one;
   uint8_t absolute_temp, condition_temp;

   if (builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - 2 ||
       !chrome9_ps_immediate(builder, signs, 2, &sign))
      return false;
   negative_one = sign;
   positive_one = sign;
   for (unsigned c = 0; c < 4; c++) {
      negative_one.swizzle[c] = sign.swizzle[0];
      positive_one.swizzle[c] = sign.swizzle[1];
   }
   absolute_temp = builder->next_temp++;
   condition_temp = builder->next_temp++;
   absolute = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_ABS);
   conditional = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_CMP);
   kill = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_KILL);
   if (!absolute || !conditional || !kill)
      return false;
   absolute->destination.file = CHROME9_PS_DESTINATION_TEMP;
   absolute->destination.id = absolute_temp;
   absolute->destination.writemask = 1;
   absolute->source[0] = *alpha;
   conditional->destination.file = CHROME9_PS_DESTINATION_TEMP;
   conditional->destination.id = condition_temp;
   conditional->destination.writemask = 0xf;
   conditional->source[0].file = CHROME9_PS_SOURCE_TEMP;
   conditional->source[0].id = absolute_temp;
   conditional->source[0].modifier = CHROME9_PS_SOURCE_NEGATE;
   for (unsigned c = 0; c < 4; c++)
      conditional->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_X;
   conditional->source[1] = negative_one;
   conditional->source[2] = positive_one;
   kill->source[0].file = CHROME9_PS_SOURCE_TEMP;
   kill->source[0].id = condition_temp;
   for (unsigned c = 0; c < 4; c++)
      kill->source[0].swizzle[c] = c;
   return true;
}

static bool
chrome9_ps_lower_alpha_terminate(struct chrome9_ps_nir_builder *builder,
                                 nir_def *condition)
{
   static const uint32_t half[4] = { 0x3f000000 };
   nir_scalar predicate, alpha, reference;
   nir_alu_instr *compare, *multiply = NULL;
   nir_tex_instr *texture;
   struct chrome9_ps_source sampled, threshold, varying;
   struct chrome9_ps_instruction *subtract, *kill;
   unsigned sampled_lane, threshold_lane;
   unsigned temp;
   bool product_factor = false;
   bool equal_zero;

   if (!condition || condition->num_components != 1)
      return false;
   predicate = nir_scalar_resolved(condition, 0);
   if (nir_def_instr(predicate.def)->type != nir_instr_type_alu)
      return false;
   compare = nir_instr_as_alu(nir_def_instr(predicate.def));
   equal_zero = compare->op == nir_op_feq;
   if (predicate.comp || (compare->op != nir_op_flt && !equal_zero) ||
       compare->src[0].src.ssa->bit_size != 32 ||
       compare->src[1].src.ssa->bit_size != 32)
      return false;
   alpha = nir_scalar_resolved(compare->src[0].src.ssa,
                               compare->src[0].swizzle[0]);
   reference = nir_scalar_resolved(compare->src[1].src.ssa,
                                   compare->src[1].swizzle[0]);
   /* feq is commutative: accept the constant zero on either side. */
   if (equal_zero && nir_scalar_is_const(alpha)) {
      nir_scalar swap = alpha;

      alpha = reference;
      reference = swap;
   }
   if (!nir_scalar_is_const(reference) ||
       (equal_zero ? (nir_scalar_as_uint(reference) & 0x7fffffff) != 0 :
                     nir_scalar_as_uint(reference) != half[0]))
      return false;
   if (!chrome9_ps_sample_alpha(alpha, &texture)) {
      nir_scalar sample, factor;

      if (nir_def_instr(alpha.def)->type != nir_instr_type_alu)
         return false;
      multiply = nir_instr_as_alu(nir_def_instr(alpha.def));
      if (multiply->op != nir_op_fmul || multiply->fp_math_ctrl ||
          alpha.comp >= multiply->def.num_components)
         return false;
      for (unsigned sampler_source = 0; sampler_source < 2;
           sampler_source++) {
         sample = nir_scalar_chase_alu_src(alpha, sampler_source);
         factor = nir_scalar_chase_alu_src(alpha, 1 - sampler_source);
         factor = nir_scalar_resolved(factor.def, factor.comp);
         if (!chrome9_ps_sample_alpha(sample, &texture) ||
             factor.def->bit_size != 32 ||
             factor.comp >= factor.def->num_components ||
             !chrome9_ps_leaf(builder, factor.def, &varying) ||
             varying.file != CHROME9_PS_SOURCE_TEXTURE || varying.id >= 8 ||
             varying.swizzle[factor.comp] > CHROME9_PS_SWIZZLE_W)
            continue;
         /* The 0.5 threshold needs the factor proven within [0, 1];
          * an exact zero test does not. */
         if (!equal_zero)
            builder->unit_interval_input_mask |=
               1u << (varying.id * 4 + varying.swizzle[factor.comp]);
         product_factor = true;
         break;
      }
      if (!product_factor)
         return false;
   }
   if (texture->op != nir_texop_tex ||
       texture->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       texture->texture_index != 0 || texture->sampler_index != 0 ||
       texture->is_array || texture->is_shadow ||
       texture->def.num_components != 4 || texture->def.bit_size != 32 ||
       alpha.def->bit_size != 32 ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       !chrome9_ps_lower_def(builder, alpha.def, &sampled) ||
       !chrome9_ps_immediate(builder, half, 1, &threshold))
      return false;
   if (alpha.comp >= alpha.def->num_components ||
       sampled.swizzle[alpha.comp] > CHROME9_PS_SWIZZLE_W)
      return false;
   sampled_lane = sampled.swizzle[alpha.comp];
   threshold_lane = threshold.swizzle[0];
   for (unsigned c = 0; c < 4; c++) {
      sampled.swizzle[c] = sampled_lane;
      threshold.swizzle[c] = threshold_lane;
   }
   if (equal_zero)
      return chrome9_ps_lower_kill_zero(builder, &sampled);
   temp = builder->next_temp++;
   subtract = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_SUB);
   kill = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_KILL);
   if (!subtract || !kill)
      return false;
   subtract->destination.file = CHROME9_PS_DESTINATION_TEMP;
   subtract->destination.id = temp;
   subtract->destination.writemask = 0xf;
   subtract->source[0] = sampled;
   subtract->source[1] = threshold;
   kill->source[0].file = CHROME9_PS_SOURCE_TEMP;
   kill->source[0].id = temp;
   for (unsigned c = 0; c < 4; c++)
      kill->source[0].swizzle[c] = c;
   return true;
}

/*
 * discard if any predicate holds: each becomes CMP(condition, -1, +1) over
 * four lanes, several merge with MIN, and one KILL tests the result.  A
 * NULL predicate is an unconditional discard.
 */
static bool
chrome9_ps_lower_terminates(struct chrome9_ps_nir_builder *builder,
                            const struct util_dynarray *predicates)
{
   static const uint32_t signs[4] = { 0xbf800000, 0x3f800000 };
   static const uint8_t broadcast[4] = { 0, 0, 0, 0 };
   static const uint8_t identity[4] = { 0, 1, 2, 3 };
   struct chrome9_ps_source sign, operand[3];
   struct chrome9_ps_instruction *kill;
   uint8_t kill_value = 0, temp;
   bool have = false;

   /* A single discard keeps the qualified sampled-alpha form. */
   if (util_dynarray_num_elements(predicates, nir_def *) == 1 &&
       *util_dynarray_element(predicates, nir_def *, 0) &&
       chrome9_ps_lower_alpha_terminate(
          builder, *util_dynarray_element(predicates, nir_def *, 0)))
      return true;
   if (!chrome9_ps_immediate(builder, signs, 2, &sign))
      return false;
   operand[1] = operand[2] = sign;
   for (unsigned c = 0; c < 4; c++) {
      operand[1].swizzle[c] = sign.swizzle[0];
      operand[2].swizzle[c] = sign.swizzle[1];
   }
   util_dynarray_foreach(predicates, nir_def *, entry) {
      struct chrome9_ps_condition condition;
      nir_scalar predicate;

      if (*entry) {
         predicate = nir_scalar_resolved(*entry, 0);
         if (nir_scalar_is_const(predicate)) {
            if (!nir_scalar_as_bool(predicate))
               continue;
            *entry = NULL;
         }
      }
      if (!*entry) {
         if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_MOV,
                                   &operand[1], 1, 0xf, &temp))
            return false;
      } else {
         if (!chrome9_ps_lower_condition(builder, predicate, 0, &condition) ||
             builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         temp = builder->next_temp++;
         if (!chrome9_ps_emit_condition_cmp(builder, &condition, broadcast,
                                            &operand[1], &operand[2], 0xf,
                                            temp))
            return false;
      }
      if (have) {
         struct chrome9_ps_source merge[2] = {
            chrome9_ps_temp_source(kill_value, identity),
            chrome9_ps_temp_source(temp, identity),
         };

         if (!chrome9_ps_emit_temp(builder, CHROME9_PS_OPCODE_MIN, merge, 2,
                                   0xf, &temp))
            return false;
      }
      kill_value = temp;
      have = true;
   }
   if (!have)
      return true;
   kill = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_KILL);
   if (!kill)
      return false;
   kill->source[0] = chrome9_ps_temp_source(kill_value, identity);
   return true;
}

static bool
chrome9_compile_ps_nir(nir_shader *nir, struct chrome9_ps_ir *ir,
                       struct chrome9_ps_program *program,
                       uint32_t *uniform_mask,
                       uint32_t *equal_zero_input_mask,
                       uint32_t *identity_projector_input_mask,
                       uint32_t *unit_interval_input_mask,
                       uint8_t *cube_sampler_mask,
                       uint8_t *volume_sampler_mask,
                       bool allow_kill, bool allow_bias, bool allow_depth,
                       bool *lowered)
{
   struct chrome9_ps_nir_builder builder = { .allow_bias = allow_bias };
   nir_def *depth = NULL;
   struct chrome9_ps_source operands[4][3] = { 0 };
   enum chrome9_ps_opcode op[4];
   unsigned source_count[4] = { 0 }, order[3];
   struct util_dynarray terminates;
   bool zero_output = false;
   nir_block *block;
   /* oCn of FRAG_RESULT_DATAn (COLOR is oC0). */
   nir_def *results[4] = { NULL };
   unsigned outputs = 0;

   *lowered = false;
   builder.proof.budget = CHROME9_PS_PROOF_NODES;
   builder.proof.mem_ctx = nir;
   util_dynarray_init(&terminates, nir);
   if (!chrome9_fs_straight_line_entrypoint(nir, NULL, &block) ||
       nir->info.float_controls_execution_mode)
      return false;
   nir_foreach_instr(instr, block) {
      nir_intrinsic_instr *intr;
      nir_variable *var;
      unsigned location;
      nir_def *stored;

      if (instr->type == nir_instr_type_alu ||
          instr->type == nir_instr_type_deref ||
          instr->type == nir_instr_type_load_const ||
          instr->type == nir_instr_type_tex)
         continue;
      if (instr->type != nir_instr_type_intrinsic)
         return false;
      intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic == nir_intrinsic_terminate_if ||
          intr->intrinsic == nir_intrinsic_terminate) {
         nir_def *predicate =
            intr->intrinsic == nir_intrinsic_terminate_if ?
            intr->src[0].ssa : NULL;

         if (!allow_kill)
            return false;
         util_dynarray_append(&terminates, predicate);
         continue;
      }
      if (intr->intrinsic == nir_intrinsic_store_output &&
          nir_intrinsic_io_semantics(intr).location == FRAG_RESULT_DEPTH) {
         /* gl_FragDepth: one scalar store, written before the color. */
         if (!allow_depth || depth || nir_intrinsic_component(intr) ||
             nir_intrinsic_write_mask(intr) != 0x1 ||
             intr->src[0].ssa->num_components != 1 ||
             intr->src[0].ssa->bit_size != 32 ||
             !nir_src_is_const(intr->src[1]) ||
             nir_src_as_uint(intr->src[1]))
            return false;
         depth = intr->src[0].ssa;
         continue;
      }
      if (intr->intrinsic == nir_intrinsic_store_output) {
         struct nir_io_semantics semantics =
            nir_intrinsic_io_semantics(intr);

         location = semantics.location;
         if (nir_intrinsic_component(intr) ||
             semantics.dual_source_blend_index ||
             nir_intrinsic_write_mask(intr) != 0xf ||
             !nir_src_is_const(intr->src[1]) ||
             nir_src_as_uint(intr->src[1]))
            return false;
         stored = intr->src[0].ssa;
      } else if (intr->intrinsic == nir_intrinsic_store_deref) {
         nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);

         var = nir_intrinsic_get_var(intr, 0);
         if (deref && deref->deref_type == nir_deref_type_var && var &&
             var->data.mode == nir_var_shader_out &&
             var->data.location == FRAG_RESULT_DEPTH) {
            if (!allow_depth || depth || var->data.index ||
                var->data.location_frac ||
                nir_intrinsic_write_mask(intr) != 0x1 ||
                intr->src[1].ssa->num_components != 1 ||
                intr->src[1].ssa->bit_size != 32)
               return false;
            depth = intr->src[1].ssa;
            continue;
         }
         if (!deref || deref->deref_type != nir_deref_type_var ||
             !var || var->data.mode != nir_var_shader_out ||
             var->data.index || var->data.location_frac ||
             nir_intrinsic_write_mask(intr) != 0xf)
            return false;
         location = var->data.location;
         stored = intr->src[1].ssa;
      } else if (intr->intrinsic == nir_intrinsic_load_uniform) {
         unsigned id;

         if (!nir_src_is_const(intr->src[0]) || nir_intrinsic_base(intr) ||
             intr->def.bit_size != 32 || !intr->def.num_components ||
             intr->def.num_components > 4)
            return false;
         id = nir_src_as_uint(intr->src[0]);
         if (id >= 32)
            return false;
         builder.uniform_mask |= 1u << id;
         continue;
      } else if (intr->intrinsic == nir_intrinsic_load_ubo ||
                 intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
         unsigned id, component;

         if (!chrome9_ps_ubo_load(intr, &id, &component))
            return false;
         builder.uniform_mask |= 1u << id;
         continue;
      } else if (intr->intrinsic ==
                    nir_intrinsic_load_barycentric_pixel ||
                 intr->intrinsic == nir_intrinsic_load_frag_coord ||
                 intr->intrinsic ==
                    nir_intrinsic_load_interpolated_input ||
                 intr->intrinsic == nir_intrinsic_load_deref) {
         continue;
      } else {
         return false;
      }
      const unsigned n = location == FRAG_RESULT_COLOR ? 0 :
                         location - FRAG_RESULT_DATA0;

      if ((location != FRAG_RESULT_COLOR &&
           (location < FRAG_RESULT_DATA0 ||
            location > FRAG_RESULT_DATA3)) || results[n])
         return false;
      results[n] = stored;
      outputs = MAX2(outputs, n + 1);
   }
   /* The KILL precedes every output, as the kernel requires. */
   if (!chrome9_ps_lower_terminates(&builder, &terminates))
      return false;
   /* Dense outputs; oDepth only with one colour output.  A program that
    * only discards writes an undefined colour: zero. */
   if (!outputs && !depth &&
       util_dynarray_num_elements(&terminates, nir_def *)) {
      static const uint32_t zero[4] = { 0 };

      if (!chrome9_ps_immediate(&builder, zero, 4, &operands[0][0]))
         return false;
      op[0] = CHROME9_PS_OPCODE_MOV;
      source_count[0] = 1;
      zero_output = true;
   }
   if ((!outputs && !zero_output) || (depth && outputs > 1))
      return false;
   for (unsigned n = 0; n < outputs; n++) {
      nir_def *result = results[n];

      if (!result || result->num_components != 4 || result->bit_size != 32)
         return false;

      nir_instr *result_instr = nir_def_instr(result);

      source_count[n] = 0;
      if (result_instr->type == nir_instr_type_alu) {
         nir_alu_instr *alu = nir_instr_as_alu(result_instr);

         switch (alu->op) {
         case nir_op_fmul:
            op[n] = CHROME9_PS_OPCODE_MUL;
            source_count[n] = 2;
            break;
         case nir_op_fadd:
            op[n] = CHROME9_PS_OPCODE_ADD;
            source_count[n] = 2;
            break;
         case nir_op_fsub:
            op[n] = CHROME9_PS_OPCODE_SUB;
            source_count[n] = 2;
            break;
         case nir_op_ffma:
            op[n] = CHROME9_PS_OPCODE_MAD;
            source_count[n] = 3;
            break;
         case nir_op_fmin:
            op[n] = CHROME9_PS_OPCODE_MIN;
            source_count[n] = 2;
            break;
         default:
            source_count[n] = 0;
            break;
         }
         chrome9_ps_source_order(op[n], alu->src, source_count[n], order);
         for (unsigned s = 0; s < source_count[n]; s++) {
            bool modifier = chrome9_ps_source_modifier_allowed(op[n], s);
            const nir_alu_src *nir_source = &alu->src[order[s]];

            if (!(modifier ?
                  chrome9_ps_lower_alu_source_modifier(&builder, nir_source,
                                                        4, &operands[n][s]) :
                  chrome9_ps_lower_alu_source(&builder, nir_source, 4,
                                              &operands[n][s])))
               return false;
         }
      }
      if (!source_count[n]) {
         op[n] = CHROME9_PS_OPCODE_MOV;
         if (!chrome9_ps_lower_def(&builder, result, &operands[n][0]))
            return false;
         source_count[n] = 1;
      }
   }
   if (depth) {
      /* One lane, replicated: the depth MOV reads a scalar. */
      if (!chrome9_ps_lower_def(&builder, depth, &builder.ir.depth))
         return false;
      for (unsigned c = 1; c < 4; c++)
         builder.ir.depth.swizzle[c] = builder.ir.depth.swizzle[0];
      builder.ir.depth_output = true;
   }
   /* Terminal RGBA writes oC0, oC1, ... in order, after all other work. */
   for (unsigned n = 0; n < MAX2(outputs, zero_output); n++) {
      struct chrome9_ps_instruction *terminal =
         chrome9_ps_append_instruction(&builder, op[n]);

      if (!terminal)
         return false;
      terminal->destination.file = CHROME9_PS_DESTINATION_COLOR;
      terminal->destination.id = n;
      terminal->destination.writemask = 0xf;
      for (unsigned s = 0; s < source_count[n]; s++)
         terminal->source[s] = operands[n][s];
   }
   if (!chrome9_fragment_legalize_constant_ports(&builder.ir))
      return false;
   *ir = builder.ir;
   memset(program, 0, sizeof(*program));
   memcpy(program->immediate, builder.immediate,
          sizeof(program->immediate));
   program->immediate_mask = builder.immediate_mask;
   *uniform_mask = builder.uniform_mask;
   *equal_zero_input_mask = builder.equal_zero_input_mask;
   *identity_projector_input_mask =
      builder.identity_projector_input_mask;
   *unit_interval_input_mask = builder.unit_interval_input_mask;
   *cube_sampler_mask = builder.cube_sampler_mask;
   *volume_sampler_mask = builder.volume_sampler_mask;
   *lowered = true;
   bool compiled = chrome9_compile_ps_ir(&builder.ir, program);

   /* Only the optional offline encoder; the kernel compiles the semantic
    * program, whose validity is decided after scheduling. */
   if (!compiled && debug_get_bool_option("CHROME9_DEBUG_REJECT", false))
      fprintf(stderr, "chrome9: offline native encoder skipped ir=%u "
              "ssa temps=%u\n", builder.ir.count, builder.next_temp);

   memcpy(program->immediate, builder.immediate,
          sizeof(program->immediate));
   program->immediate_mask = builder.immediate_mask;
   return compiled;
}

/*
 * Lower the canonical NIR output expression to the first native backend IR.
 * Unlike the compatibility program classifier, this follows the output DAG
 * and derives opcode and operands directly.  The scheduler accepts a bounded
 * straight-line DAG and assigns dense hardware temporaries; unsupported NIR
 * operations fail compilation instead of being approximated or classified by
 * application-shaped program enums.
 */
static bool
chrome9_fs_key_from_nir(nir_shader *nir, struct chrome9_fs_key *key)
{
   nir_def *result = NULL;

   memset(key, 0, sizeof(*key));
   /* The current hardware envelope does not select an IEEE execution mode.
    * Default GLSL is admissible; explicit preserve/FTZ or rounding promises
    * must wait for a characterized Chrome9 control word. */
   if (nir->info.float_controls_execution_mode)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            nir_variable *var;
            unsigned location;
            nir_def *stored;

            if (instr->type == nir_instr_type_alu ||
                instr->type == nir_instr_type_deref ||
                instr->type == nir_instr_type_load_const)
               continue;
            if (instr->type != nir_instr_type_intrinsic)
               return false;
            intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output) {
               struct nir_io_semantics semantics =
                  nir_intrinsic_io_semantics(intr);

               location = semantics.location;
               if (nir_intrinsic_component(intr) ||
                   semantics.dual_source_blend_index ||
                   nir_intrinsic_write_mask(intr) != 0xf ||
                   !nir_src_is_const(intr->src[1]) ||
                   nir_src_as_uint(intr->src[1]))
                  return false;
               stored = intr->src[0].ssa;
            } else if (intr->intrinsic == nir_intrinsic_store_deref) {
               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);

               var = nir_intrinsic_get_var(intr, 0);
               if (!deref || deref->deref_type != nir_deref_type_var ||
                   !var || var->data.mode != nir_var_shader_out ||
                   var->data.index || var->data.location_frac ||
                   nir_intrinsic_write_mask(intr) != 0xf)
                  return false;
               location = var->data.location;
               stored = intr->src[1].ssa;
            } else if (intr->intrinsic ==
                          nir_intrinsic_load_barycentric_pixel ||
                       intr->intrinsic ==
                          nir_intrinsic_load_interpolated_input ||
                       intr->intrinsic == nir_intrinsic_load_deref ||
                       intr->intrinsic == nir_intrinsic_load_uniform ||
                       intr->intrinsic == nir_intrinsic_load_ubo ||
                       intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
               continue;
            } else {
               /* Discard, demote, memory, atomics, and extra outputs are
                * outside this one-record backend and must not disappear. */
               return false;
            }
            if ((location != FRAG_RESULT_COLOR &&
                 location != FRAG_RESULT_DATA0) || result)
               return false;
            result = stored;
         }
      }
   }
   if (!result || result->num_components != 4 || result->bit_size != 32)
      return false;

   if (chrome9_backend_leaf(result, 0, &key->src[0])) {
      key->op = CHROME9_FS_OP_MOV;
      return true;
   }

   nir_instr *instr = nir_def_instr(result);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_fmul ||
       !chrome9_backend_source(&alu->src[0], &key->src[0]) ||
       !chrome9_backend_source(&alu->src[1], &key->src[1]))
      return false;
   key->op = CHROME9_FS_OP_MUL;
   return true;
}

static bool
chrome9_nir_identity_source(const nir_alu_src *source, unsigned components)
{
   if (!source->src.ssa || source->src.ssa->num_components != components)
      return false;
   for (unsigned c = 0; c < components; c++)
      if (source->swizzle[c] != c)
         return false;
   return true;
}

static bool
chrome9_nir_source_is_quarter(const nir_alu_src *source,
                              unsigned components)
{
   if (!source->src.ssa)
      return false;
   for (unsigned c = 0; c < components; c++) {
      nir_scalar scalar = nir_scalar_resolved(source->src.ssa,
                                               source->swizzle[c]);

      if (!nir_scalar_is_const(scalar) ||
          nir_scalar_as_uint(scalar) != 0x3e800000)
         return false;
   }
   return true;
}

static bool
chrome9_nir_rewrite_shared_saturate_max(nir_builder *b, nir_instr *instr,
                                        void *data)
{
   (void)b;
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *outer = nir_instr_as_alu(instr);
   if (outer->op != nir_op_fsat ||
       !chrome9_nir_identity_source(&outer->src[0],
                                     outer->def.num_components) ||
       nir_def_instr(outer->src[0].src.ssa)->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *maximum = nir_instr_as_alu(
      nir_def_instr(outer->src[0].src.ssa));
   if (maximum->op != nir_op_fmax ||
       !list_is_singular(&maximum->def.uses) ||
       nir_def_used_by_if(&maximum->def))
      return false;
   for (unsigned raw_source = 0; raw_source < 2; raw_source++) {
      unsigned constant_source = 1 - raw_source;
      nir_def *raw = maximum->src[raw_source].src.ssa;
      nir_alu_instr *inner = NULL;
      unsigned uses = 0;

      if (!chrome9_nir_identity_source(&maximum->src[raw_source],
                                        maximum->def.num_components) ||
          !chrome9_nir_source_is_quarter(&maximum->src[constant_source],
                                          maximum->def.num_components) ||
          nir_def_used_by_if(raw))
         continue;
      nir_foreach_use(use, raw) {
         nir_instr *user = nir_src_use_instr(use);

         uses++;
         if (use == &maximum->src[raw_source].src ||
             user->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *candidate = nir_instr_as_alu(user);
         if (candidate->op == nir_op_fsat &&
             use == &candidate->src[0].src &&
             candidate->def.num_components == raw->num_components &&
             chrome9_nir_identity_source(&candidate->src[0],
                                          raw->num_components))
            inner = inner ? NULL : candidate;
      }
      if (uses != 2 || !inner)
         continue;
      if (inner->instr.block != maximum->instr.block ||
          !nir_instr_is_before(&inner->instr, &maximum->instr))
         continue;
      /* sat(max(x, .25)) == sat(max(sat(x), .25)).  Redirecting the
       * shared MAX input makes x single-use, so both SATs can fuse into
       * their producers without changing the finite or exceptional result. */
      nir_src_rewrite(&maximum->src[raw_source].src, &inner->def);
      return true;
   }
   return false;
}

/* The pixel MAD rounds like the unfused pair it replaces. */
static bool
chrome9_nir_fmad_to_ffma(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)b;
   (void)data;
   if (alu->op != nir_op_fmad)
      return false;
   alu->op = nir_op_ffma;
   return true;
}

/* ALU source src reads the literal value in every used lane. */
static bool
chrome9_nir_src_is_literal(const nir_alu_instr *alu, unsigned src,
                           unsigned count, float value)
{
   if (!nir_src_is_const(alu->src[src].src))
      return false;
   for (unsigned c = 0; c < count; c++)
      if (nir_src_comp_as_float(alu->src[src].src,
                                alu->src[src].swizzle[c]) != value)
         return false;
   return true;
}

/* lower_fsat turns clamp(x, 0, 1) into MAX+MIN for the vertex stage; the
 * fragment ALU has a saturate bit, so re-form fsat: one record or none. */
static bool
chrome9_nir_form_fsat(nir_builder *b, nir_alu_instr *alu, void *data)
{
   const unsigned count = alu->def.num_components;
   nir_op inner_op;
   float outer_value, inner_value;

   (void)data;
   if (alu->op == nir_op_fmin) {
      inner_op = nir_op_fmax;
      outer_value = 1.0f;
      inner_value = 0.0f;
   } else if (alu->op == nir_op_fmax) {
      inner_op = nir_op_fmin;
      outer_value = 0.0f;
      inner_value = 1.0f;
   } else {
      return false;
   }
   for (unsigned o = 0; o < 2; o++) {
      nir_alu_instr *inner;
      unsigned swizzle[NIR_MAX_VEC_COMPONENTS];

      if (!chrome9_nir_src_is_literal(alu, o, count, outer_value) ||
          !nir_src_is_alu(alu->src[!o].src))
         continue;
      inner = nir_src_as_alu(alu->src[!o].src);
      if (inner->op != inner_op)
         continue;
      for (unsigned i = 0; i < 2; i++) {
         bool literal = true;

         if (!nir_src_is_const(inner->src[i].src))
            continue;
         for (unsigned c = 0; c < count; c++)
            literal &= nir_src_comp_as_float(inner->src[i].src,
                          inner->src[i].swizzle[alu->src[!o].swizzle[c]]) ==
                       inner_value;
         if (!literal)
            continue;
         for (unsigned c = 0; c < count; c++)
            swizzle[c] =
               inner->src[!i].swizzle[alu->src[!o].swizzle[c]];
         b->cursor = nir_before_instr(&alu->instr);
         nir_def_replace(&alu->def,
            nir_fsat(b, nir_swizzle(b, inner->src[!i].src.ssa, swizzle,
                                    count)));
         return true;
      }
   }
   return false;
}

/* Lanes of a factor vector fold into an immediate or one swizzled value. */
static bool
chrome9_nir_factor_folds(const nir_scalar *lane, unsigned count)
{
   bool constant = true, single = true;

   for (unsigned c = 0; c < count; c++) {
      if (!lane[c].def)
         continue;
      constant &= nir_scalar_is_const(lane[c]);
      for (unsigned d = 0; d < c; d++)
         if (lane[d].def && lane[d].def != lane[c].def)
            single = false;
   }
   return constant || single;
}

/* vecN of products and literal zeros feeding one ADD -> product of two
 * vecN, so the ADD fuses into one MAD (per-tap offsets of blur kernels).
 * Zero lanes become 0 * 0, exactly +0. */
static bool
chrome9_nir_vec_of_products(nir_builder *b, nir_alu_instr *vec, void *data)
{
   const unsigned count = vec->def.num_components;
   nir_scalar factor[2][NIR_MAX_VEC_COMPONENTS] = { 0 };
   nir_def *lanes[2][NIR_MAX_VEC_COMPONENTS];
   bool neg[NIR_MAX_VEC_COMPONENTS] = { 0 };
   unsigned products = 0, ctrl = 0;
   nir_instr *user;

   (void)data;
   if (!nir_op_is_vec(vec->op) || vec->def.bit_size != 32 || count < 2 ||
       !list_is_singular(&vec->def.uses))
      return false;
   user = nir_src_use_instr(list_first_entry(&vec->def.uses, nir_src,
                                              use_link));
   if (user->type != nir_instr_type_alu ||
       nir_instr_as_alu(user)->op != nir_op_fadd)
      return false;
   for (unsigned c = 0; c < count; c++) {
      nir_scalar lane = nir_scalar_resolved(vec->src[c].src.ssa,
                                            vec->src[c].swizzle[0]);
      nir_alu_instr *alu;

      if (nir_scalar_is_const(lane)) {
         if (nir_scalar_as_uint(lane) != 0)
            return false;
         continue;
      }
      if (!nir_scalar_is_alu(lane))
         return false;
      alu = nir_def_as_alu(lane.def);
      if (alu->op == nir_op_fneg) {
         ctrl |= alu->fp_math_ctrl;
         lane = nir_scalar_chase_alu_src(lane, 0);
         if (!nir_scalar_is_alu(lane))
            return false;
         alu = nir_def_as_alu(lane.def);
         neg[c] = true;
      }
      if (alu->op != nir_op_fmul)
         return false;
      ctrl |= alu->fp_math_ctrl;
      factor[0][c] = nir_scalar_chase_alu_src(lane, 0);
      factor[1][c] = nir_scalar_chase_alu_src(lane, 1);
      /* Keep the constant factor first. */
      if (nir_scalar_is_const(factor[1][c])) {
         nir_scalar swap = factor[0][c];

         factor[0][c] = factor[1][c];
         factor[1][c] = swap;
      }
      products++;
   }
   if (!products || !chrome9_nir_factor_folds(factor[0], count) ||
       !chrome9_nir_factor_folds(factor[1], count))
      return false;
   b->cursor = nir_before_instr(&vec->instr);
   for (unsigned c = 0; c < count; c++) {
      if (!factor[0][c].def) {
         lanes[0][c] = lanes[1][c] = nir_imm_float(b, 0.0f);
         continue;
      }
      lanes[0][c] = nir_mov_scalar(b, factor[0][c]);
      lanes[1][c] = nir_mov_scalar(b, factor[1][c]);
      if (neg[c])
         lanes[0][c] = nir_fneg(b, lanes[0][c]);
   }
   nir_def *product = nir_fmul(b, nir_vec(b, lanes[0], count),
                               nir_vec(b, lanes[1], count));
   nir_def_as_alu(product)->fp_math_ctrl = ctrl;
   nir_def_replace(&vec->def, product);
   return true;
}

/* Lanes form one source record: one swizzled value plus literal 0/1. */
static bool
chrome9_nir_factor_literal(const nir_scalar *lane, unsigned count)
{
   nir_def *def = NULL;

   for (unsigned c = 0; c < count; c++) {
      if (nir_scalar_is_const(lane[c])) {
         const uint32_t bits = nir_scalar_as_uint(lane[c]);

         if (bits != 0 && bits != 0x3f800000)
            return false;
      } else if (def && lane[c].def != def) {
         return false;
      } else {
         def = lane[c].def;
      }
   }
   return true;
}

/* vecN of products and pass-through lanes -> one product: the passed
 * lane x becomes 1 * x (tex.rgb * col.rgb with col.a: one MUL record). */
static bool
chrome9_nir_vec_product_passthrough(nir_builder *b, nir_alu_instr *vec,
                                    void *data)
{
   const unsigned count = vec->def.num_components;
   nir_scalar factor[2][NIR_MAX_VEC_COMPONENTS];
   nir_def *lanes[2][NIR_MAX_VEC_COMPONENTS];
   unsigned products = 0, ctrl = 0;

   (void)data;
   if (!nir_op_is_vec(vec->op) || vec->def.bit_size != 32 || count < 2)
      return false;
   for (unsigned c = 0; c < count; c++) {
      nir_scalar lane = nir_scalar_resolved(vec->src[c].src.ssa,
                                            vec->src[c].swizzle[0]);

      if (nir_scalar_is_alu(lane) &&
          nir_def_as_alu(lane.def)->op == nir_op_fmul) {
         ctrl |= nir_def_as_alu(lane.def)->fp_math_ctrl;
         factor[0][c] = nir_scalar_chase_alu_src(lane, 0);
         factor[1][c] = nir_scalar_chase_alu_src(lane, 1);
         products++;
      } else {
         factor[0][c] = nir_get_scalar(NULL, 0);
         factor[1][c] = lane;
      }
   }
   if (products < 2 || products == count)
      return false;
   /* Put each product's factor from the pass-through value second. */
   for (unsigned c = 0; c < count; c++) {
      if (!factor[0][c].def)
         continue;
      for (unsigned d = 0; d < count; d++) {
         if (factor[0][d].def || nir_scalar_is_const(factor[1][d]))
            continue;
         if (factor[0][c].def == factor[1][d].def) {
            nir_scalar swap = factor[0][c];

            factor[0][c] = factor[1][c];
            factor[1][c] = swap;
         }
         break;
      }
   }
   b->cursor = nir_before_instr(&vec->instr);
   for (unsigned c = 0; c < count; c++) {
      lanes[0][c] = factor[0][c].def ? nir_mov_scalar(b, factor[0][c]) :
                                       nir_imm_float(b, 1.0f);
      lanes[1][c] = nir_mov_scalar(b, factor[1][c]);
   }
   {
      nir_scalar check[2][NIR_MAX_VEC_COMPONENTS];

      for (unsigned c = 0; c < count; c++) {
         check[0][c] = nir_scalar_resolved(lanes[0][c], 0);
         check[1][c] = nir_scalar_resolved(lanes[1][c], 0);
      }
      if (!chrome9_nir_factor_literal(check[0], count) ||
          !chrome9_nir_factor_literal(check[1], count))
         return false;
   }
   nir_def *product = nir_fmul(b, nir_vec(b, lanes[0], count),
                               nir_vec(b, lanes[1], count));
   nir_def_as_alu(product)->fp_math_ctrl = ctrl;
   nir_def_replace(&vec->def, product);
   return true;
}

/* Fuse MUL+ADD into MAD and fold negations into SUB and the MAD A
 * operand before the one-record-per-op backend sees the program. */
static void
chrome9_nir_optimize_semantic(nir_shader *nir)
{
   bool progress;

   if (nir_shader_alu_pass(nir, chrome9_nir_vec_of_products,
                           nir_metadata_control_flow, NULL))
      nir_opt_dce(nir);
   if (nir_shader_alu_pass(nir, chrome9_nir_vec_product_passthrough,
                           nir_metadata_control_flow, NULL))
      nir_opt_dce(nir);
   do {
      progress = false;
      progress |= nir_opt_algebraic(nir);
      progress |= nir_opt_constant_folding(nir);
      progress |= nir_opt_copy_prop(nir);
      progress |= nir_opt_cse(nir);
      progress |= nir_opt_dce(nir);
   } while (progress);
   do {
      progress = false;
      progress |= nir_opt_algebraic_late(nir);
      progress |= nir_opt_constant_folding(nir);
      progress |= nir_opt_copy_prop(nir);
      progress |= nir_opt_cse(nir);
      progress |= nir_opt_dce(nir);
   } while (progress);
   nir_shader_alu_pass(nir, chrome9_nir_fmad_to_ffma,
                       nir_metadata_control_flow, NULL);
   if (nir_shader_alu_pass(nir, chrome9_nir_form_fsat,
                           nir_metadata_control_flow, NULL))
      nir_opt_dce(nir);
}

static bool
chrome9_ps_componentwise(enum chrome9_ps_opcode op)
{
   switch (op) {
   case CHROME9_PS_OPCODE_MOV:
   case CHROME9_PS_OPCODE_MUL:
   case CHROME9_PS_OPCODE_ADD:
   case CHROME9_PS_OPCODE_SUB:
   case CHROME9_PS_OPCODE_MAD:
   case CHROME9_PS_OPCODE_MIN:
   case CHROME9_PS_OPCODE_MAX:
   case CHROME9_PS_OPCODE_CMP:
      return true;
   default:
      return false;
   }
}

static unsigned
chrome9_ps_source_count(enum chrome9_ps_opcode op)
{
   switch (op) {
   case CHROME9_PS_OPCODE_MAD:
   case CHROME9_PS_OPCODE_CMP:
      return 3;
   case CHROME9_PS_OPCODE_MUL:
   case CHROME9_PS_OPCODE_ADD:
   case CHROME9_PS_OPCODE_SUB:
   case CHROME9_PS_OPCODE_MIN:
   case CHROME9_PS_OPCODE_MAX:
   case CHROME9_PS_OPCODE_DP3:
   case CHROME9_PS_OPCODE_DP4:
      return 2;
   default:
      return 1;
   }
}

static bool
chrome9_ps_reads_temp(const struct chrome9_ps_instruction *instruction,
                      unsigned id)
{
   for (unsigned s = 0; s < chrome9_ps_source_count(instruction->op); s++)
      if (instruction->source[s].file == CHROME9_PS_SOURCE_TEMP &&
          instruction->source[s].id == id)
         return true;
   return false;
}

static void
chrome9_ps_ir_remove(struct chrome9_ps_ir *ir, unsigned index)
{
   memmove(&ir->instruction[index], &ir->instruction[index + 1],
           (ir->count - index - 1) * sizeof(ir->instruction[0]));
   ir->count--;
}

/* The one instruction writing temporary id, if exactly one does. */
static bool
chrome9_ps_single_writer(const struct chrome9_ps_ir *ir, unsigned id,
                         unsigned *writer)
{
   bool found = false;

   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *in = &ir->instruction[i];

      if (in->op == CHROME9_PS_OPCODE_KILL ||
          in->destination.file != CHROME9_PS_DESTINATION_TEMP ||
          in->destination.id != id)
         continue;
      if (found)
         return false;
      *writer = i;
      found = true;
   }
   return found;
}

/*
 * vecN assembly: MOV D.L = S.l, where the only producer of S writes just
 * lane l, becomes the producer writing D.L directly and every read of S.l
 * reads D.L.  A terminal colour copy takes literal lanes from their MOV as
 * ONE/ZERO selectors first.
 */
/* The program still translates to the DRM UAPI. */
static bool
chrome9_ps_ir_translates(const struct chrome9_ps_ir *ir)
{
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   unsigned count, samplers = 0;

   for (unsigned i = 0; i < ir->count; i++)
      if (ir->instruction[i].op == CHROME9_PS_OPCODE_TEX)
         samplers |= 1u << ir->instruction[i].sampler;
   return chrome9_fragment_build_instructions(ir, samplers, instructions,
                                              &count);
}

static void
chrome9_ps_ir_coalesce_lanes(struct chrome9_ps_ir *ir)
{
   struct chrome9_ps_instruction *last;

   if (!ir->count)
      return;
   last = &ir->instruction[ir->count - 1];
   if (last->op == CHROME9_PS_OPCODE_MOV &&
       last->destination.file == CHROME9_PS_DESTINATION_COLOR &&
       last->source[0].file == CHROME9_PS_SOURCE_TEMP &&
       !last->source[0].modifier) {
      const struct chrome9_ps_ir before = *ir;

      for (unsigned c = 0; c < 4; c++) {
         const uint8_t lane = last->source[0].swizzle[c];

         if (lane > CHROME9_PS_SWIZZLE_W)
            continue;
         for (unsigned i = 0; i + 1 < ir->count; i++) {
            struct chrome9_ps_instruction *mov = &ir->instruction[i];
            uint8_t literal;
            bool other_reader = false;

            if (mov->op != CHROME9_PS_OPCODE_MOV ||
                mov->destination.file != CHROME9_PS_DESTINATION_TEMP ||
                mov->destination.id != last->source[0].id ||
                !(mov->destination.writemask & (1u << lane)) ||
                mov->destination.saturate)
               continue;
            literal = mov->source[0].swizzle[lane];
            if (literal != CHROME9_PS_SWIZZLE_ONE &&
                literal != CHROME9_PS_SWIZZLE_ZERO)
               break;
            for (unsigned j = 0; j + 1 < ir->count; j++)
               other_reader |= chrome9_ps_reads_temp(&ir->instruction[j],
                                                     mov->destination.id);
            if (other_reader ||
                (ir->depth_output &&
                 ir->depth.file == CHROME9_PS_SOURCE_TEMP &&
                 ir->depth.id == mov->destination.id))
               break;
            last->source[0].swizzle[c] = literal;
            break;
         }
      }
      /* Drop literal lanes nobody reads any more. */
      for (unsigned i = 0; i + 1 < ir->count; i++) {
         struct chrome9_ps_instruction *mov = &ir->instruction[i];
         uint8_t read = 0;

         if (mov->op != CHROME9_PS_OPCODE_MOV ||
             mov->destination.file != CHROME9_PS_DESTINATION_TEMP ||
             mov->destination.id != last->source[0].id)
            continue;
         for (unsigned c = 0; c < 4; c++)
            if (last->source[0].swizzle[c] <= CHROME9_PS_SWIZZLE_W)
               read |= 1u << last->source[0].swizzle[c];
         for (unsigned c = 0; c < 4; c++)
            if ((mov->destination.writemask & (1u << c)) && !(read & (1u << c)) &&
                mov->source[0].swizzle[c] >= CHROME9_PS_SWIZZLE_ONE)
               mov->destination.writemask &= ~(1u << c);
         if (!mov->destination.writemask) {
            chrome9_ps_ir_remove(ir, i--);
            last = &ir->instruction[ir->count - 1];
         }
      }
      if (!chrome9_ps_ir_translates(ir))
         *ir = before;
   }
   for (unsigned i = 0; i < ir->count; i++) {
      struct chrome9_ps_instruction *mov = &ir->instruction[i];
      struct chrome9_ps_instruction *producer;
      const struct chrome9_ps_source *from = &mov->source[0];
      uint8_t map[4] = { 0xff, 0xff, 0xff, 0xff };
      unsigned writer, read = 0;
      bool lane_wise, injective = true;

      if (mov->op != CHROME9_PS_OPCODE_MOV ||
          mov->destination.file != CHROME9_PS_DESTINATION_TEMP ||
          mov->destination.saturate || !mov->destination.writemask ||
          from->file != CHROME9_PS_SOURCE_TEMP || from->modifier ||
          from->id == mov->destination.id)
         continue;
      /* Source lane l of the copy lands in destination lane map[l]. */
      u_foreach_bit(d, mov->destination.writemask) {
         const uint8_t l = from->swizzle[d];

         if (l > CHROME9_PS_SWIZZLE_W || map[l] != 0xff)
            injective = false;
         else
            map[l] = d;
         read |= 1u << (l & 3);
      }
      if (!injective ||
          !chrome9_ps_single_writer(ir, from->id, &writer) || writer >= i)
         continue;
      producer = &ir->instruction[writer];
      if (producer->op == CHROME9_PS_OPCODE_TEX ||
          producer->destination.writemask != read)
         continue;
      lane_wise = chrome9_ps_componentwise(producer->op) ||
                  producer->op == CHROME9_PS_OPCODE_ABS ||
                  producer->op == CHROME9_PS_OPCODE_FRC;
      if (!lane_wise && util_bitcount(read) != 1)
         continue;
      /* TEX coordinates and KILL keep their own temporaries. */
      {
         bool special = false;

         for (unsigned j = writer + 1; j < ir->count; j++)
            special |= (ir->instruction[j].op == CHROME9_PS_OPCODE_TEX ||
                        ir->instruction[j].op == CHROME9_PS_OPCODE_KILL) &&
                       chrome9_ps_reads_temp(&ir->instruction[j], from->id);
         if (special)
            continue;
      }
      /* The target lanes must not be written before this MOV. */
      {
         bool written = false;

         for (unsigned j = 0; j < i; j++)
            written |= ir->instruction[j].op != CHROME9_PS_OPCODE_KILL &&
                       ir->instruction[j].destination.file ==
                          CHROME9_PS_DESTINATION_TEMP &&
                       ir->instruction[j].destination.id ==
                          mov->destination.id &&
                       (ir->instruction[j].destination.writemask &
                        mov->destination.writemask);
         if (written)
            continue;
      }
      {
         const unsigned from_id = from->id, to_id = mov->destination.id;
         const struct chrome9_ps_ir before = *ir;

         if (lane_wise) {
            for (unsigned s = 0; s < chrome9_ps_source_count(producer->op);
                 s++) {
               uint8_t swizzle[4];

               memcpy(swizzle, producer->source[s].swizzle, 4);
               for (unsigned l = 0; l < 4; l++)
                  if (map[l] != 0xff)
                     producer->source[s].swizzle[map[l]] = swizzle[l];
            }
         }
         producer->destination.id = to_id;
         producer->destination.writemask = mov->destination.writemask;
         for (unsigned j = writer + 1; j < ir->count; j++) {
            struct chrome9_ps_instruction *reader = &ir->instruction[j];

            for (unsigned s = 0; s < 3; s++) {
               struct chrome9_ps_source *src = &reader->source[s];

               if (src->file != CHROME9_PS_SOURCE_TEMP || src->id != from_id)
                  continue;
               src->id = to_id;
               for (unsigned c = 0; c < 4; c++)
                  if (src->swizzle[c] <= CHROME9_PS_SWIZZLE_W &&
                      map[src->swizzle[c]] != 0xff)
                     src->swizzle[c] = map[src->swizzle[c]];
            }
         }
         if (ir->depth_output && ir->depth.file == CHROME9_PS_SOURCE_TEMP &&
             ir->depth.id == from_id) {
            ir->depth.id = to_id;
            for (unsigned c = 0; c < 4; c++)
               if (ir->depth.swizzle[c] <= CHROME9_PS_SWIZZLE_W &&
                   map[ir->depth.swizzle[c]] != 0xff)
                  ir->depth.swizzle[c] = map[ir->depth.swizzle[c]];
         }
         chrome9_ps_ir_remove(ir, i--);
         if (!chrome9_ps_ir_translates(ir)) {
            *ir = before;
            i++;
         }
      }
   }
}

static bool
chrome9_ps_same_operand(const struct chrome9_ps_source *a,
                        const struct chrome9_ps_source *b)
{
   return a->file == b->file && a->id == b->id && a->modifier == b->modifier;
}

/*
 * A temporary gathered by MOVs from varyings or constants and read only by
 * componentwise ALU ops is substituted into its readers, splitting a reader
 * into one record per source combination when that saves records.
 */
static void
chrome9_ps_ir_split_gathers(struct chrome9_ps_ir *ir)
{
   for (unsigned first = 0; first < ir->count; first++) {
      const struct chrome9_ps_instruction *seed = &ir->instruction[first];
      const unsigned g = seed->destination.id;
      uint8_t origin[4] = { 0xff, 0xff, 0xff, 0xff };
      unsigned writers = 0, added = 0;
      bool ok = true;

      if (seed->op != CHROME9_PS_OPCODE_MOV ||
          seed->destination.file != CHROME9_PS_DESTINATION_TEMP)
         continue;
      if (ir->depth_output && ir->depth.file == CHROME9_PS_SOURCE_TEMP &&
          ir->depth.id == g)
         continue;
      for (unsigned i = 0; i < ir->count && ok; i++) {
         const struct chrome9_ps_instruction *in = &ir->instruction[i];

         if (in->op != CHROME9_PS_OPCODE_KILL &&
             in->destination.file == CHROME9_PS_DESTINATION_TEMP &&
             in->destination.id == g) {
            if (i < first || in->op != CHROME9_PS_OPCODE_MOV ||
                in->destination.saturate || in->source[0].modifier ||
                in->source[0].file == CHROME9_PS_SOURCE_TEMP ||
                in->source[0].file == CHROME9_PS_SOURCE_POSITION) {
               ok = false;
               break;
            }
            u_foreach_bit(l, in->destination.writemask)
               origin[l] = i;
            writers++;
            continue;
         }
         if (!chrome9_ps_reads_temp(in, g))
            continue;
         if (!chrome9_ps_componentwise(in->op) ||
             in->destination.file != CHROME9_PS_DESTINATION_TEMP) {
            ok = false;
            break;
         }
      }
      if (!ok)
         continue;
      /* Count the records each reader splits into. */
      for (unsigned i = 0; i < ir->count && ok; i++) {
         const struct chrome9_ps_instruction *in = &ir->instruction[i];
         uint32_t keys[4];
         unsigned groups = 0;

         if (!chrome9_ps_reads_temp(in, g))
            continue;
         u_foreach_bit(c, in->destination.writemask) {
            uint32_t key = 0;
            bool seen = false;

            for (unsigned s = 0; s < chrome9_ps_source_count(in->op); s++) {
               const struct chrome9_ps_source *src = &in->source[s];
               uint8_t l = src->swizzle[c];

               if (src->file != CHROME9_PS_SOURCE_TEMP || src->id != g ||
                   l > CHROME9_PS_SWIZZLE_W)
                  continue;
               if (origin[l] == 0xff)
                  ok = false;
               key |= (origin[l] + 1u) << (8 * s);
            }
            for (unsigned k = 0; k < groups; k++)
               seen |= keys[k] == key;
            if (!seen)
               keys[groups++] = key;
         }
         added += groups - 1;
      }
      if (!ok || added >= writers)
         continue;
      {
         const struct chrome9_ps_ir before = *ir;
         struct chrome9_ps_ir out = *ir;

         out.count = 0;
         for (unsigned i = 0; i < before.count; i++) {
            const struct chrome9_ps_instruction *in = &before.instruction[i];
            uint8_t done = 0;

            if (in->op != CHROME9_PS_OPCODE_KILL &&
                in->destination.file == CHROME9_PS_DESTINATION_TEMP &&
                in->destination.id == g)
               continue;
            if (!chrome9_ps_reads_temp(in, g)) {
               out.instruction[out.count++] = *in;
               continue;
            }
            u_foreach_bit(c, in->destination.writemask) {
               struct chrome9_ps_instruction split = *in;

               if (done & (1u << c))
                  continue;
               split.destination.writemask = 0;
               /* Lanes whose g operands come from the same MOVs. */
               u_foreach_bit(d, in->destination.writemask) {
                  bool same = true;

                  for (unsigned s = 0; s < chrome9_ps_source_count(in->op);
                       s++) {
                     const struct chrome9_ps_source *src = &in->source[s];
                     uint8_t lc = src->swizzle[c], ld = src->swizzle[d];

                     if (src->file != CHROME9_PS_SOURCE_TEMP || src->id != g)
                        continue;
                     if ((lc <= CHROME9_PS_SWIZZLE_W) !=
                         (ld <= CHROME9_PS_SWIZZLE_W) ||
                         (lc <= CHROME9_PS_SWIZZLE_W &&
                          origin[lc] != origin[ld]))
                        same = false;
                  }
                  if (same)
                     split.destination.writemask |= 1u << d;
               }
               done |= split.destination.writemask;
               for (unsigned s = 0; s < chrome9_ps_source_count(in->op); s++) {
                  struct chrome9_ps_source *src = &split.source[s];
                  const struct chrome9_ps_source *from = NULL;

                  if (src->file != CHROME9_PS_SOURCE_TEMP || src->id != g)
                     continue;
                  u_foreach_bit(d, split.destination.writemask)
                     if (src->swizzle[d] <= CHROME9_PS_SWIZZLE_W)
                        from = &before.instruction[
                           origin[src->swizzle[d]]].source[0];
                  /* Only literal lanes: any gathered operand reads them. */
                  for (unsigned l = 0; !from && l < 4; l++)
                     if (origin[l] != 0xff)
                        from = &before.instruction[origin[l]].source[0];
                  src->file = from->file;
                  src->id = from->id;
                  u_foreach_bit(d, split.destination.writemask)
                     if (src->swizzle[d] <= CHROME9_PS_SWIZZLE_W)
                        src->swizzle[d] = from->swizzle[src->swizzle[d]];
               }
               out.instruction[out.count++] = split;
            }
         }
         *ir = out;
         if (!chrome9_ps_ir_translates(ir))
            *ir = before;
         else
            first = UINT_MAX;
      }
   }
}

/*
 * Two partial writes of one temporary by the same op on the same operands
 * become one record, e.g. MUL t.z = a.z * T1.x and MUL t.w = a.w * T1.y.
 */
static void
chrome9_ps_ir_merge_lanes(struct chrome9_ps_ir *ir)
{
   for (unsigned j = 1; j < ir->count; j++) {
      struct chrome9_ps_instruction *b = &ir->instruction[j];

      if (!chrome9_ps_componentwise(b->op) ||
          b->destination.file != CHROME9_PS_DESTINATION_TEMP)
         continue;
      for (unsigned i = 0; i < j; i++) {
         struct chrome9_ps_instruction *a = &ir->instruction[i];
         bool ok = a->op == b->op &&
                   a->destination.file == b->destination.file &&
                   a->destination.id == b->destination.id &&
                   a->destination.saturate == b->destination.saturate &&
                   !(a->destination.writemask & b->destination.writemask);

         for (unsigned s = 0; ok && s < chrome9_ps_source_count(b->op); s++)
            ok = chrome9_ps_same_operand(&a->source[s], &b->source[s]) &&
                 !(b->source[s].file == CHROME9_PS_SOURCE_TEMP &&
                   b->source[s].id == b->destination.id);
         /* b's operands must be final at a, and nothing in between may
          * read or write the temporary b defines. */
         for (unsigned k = i; ok && k < j; k++) {
            const struct chrome9_ps_instruction *in = &ir->instruction[k];

            if (k > i && chrome9_ps_reads_temp(in, b->destination.id))
               ok = false;
            if (in->op == CHROME9_PS_OPCODE_KILL ||
                in->destination.file != CHROME9_PS_DESTINATION_TEMP)
               continue;
            for (unsigned s = 0; ok && s < chrome9_ps_source_count(b->op); s++)
               if (b->source[s].file == CHROME9_PS_SOURCE_TEMP &&
                   b->source[s].id == in->destination.id)
                  ok = false;
         }
         if (!ok)
            continue;
         {
            const struct chrome9_ps_ir before = *ir;

            u_foreach_bit(c, b->destination.writemask)
               for (unsigned s = 0; s < chrome9_ps_source_count(b->op); s++)
                  a->source[s].swizzle[c] = b->source[s].swizzle[c];
            a->destination.writemask |= b->destination.writemask;
            chrome9_ps_ir_remove(ir, j--);
            if (!chrome9_ps_ir_translates(ir)) {
               *ir = before;
               j++;
            }
         }
         break;
      }
   }
}

/*
 * Pixel throughput follows the ALU record count, so remove copies the
 * backend's one-record-per-NIR-op lowering leaves: a MOV of literal 1.0
 * lanes merges into the MOV defining the other lanes of that temporary,
 * and the terminal oC0 copy of a temporary folds into its producer.
 */
static void
chrome9_ps_ir_fold_copies(struct chrome9_ps_ir *ir,
                          const struct chrome9_ps_program *program)
{
   struct chrome9_ps_instruction *last, *producer = NULL;
   unsigned producer_index = 0, id;

   /* Literal ONE for compiler immediates of exactly 1.0 in MOVs. */
   for (unsigned i = 0; i < ir->count; i++) {
      struct chrome9_ps_instruction *mov = &ir->instruction[i];
      struct chrome9_ps_source *source = &mov->source[0];
      bool literal = true;

      if (mov->op != CHROME9_PS_OPCODE_MOV ||
          source->file != CHROME9_PS_SOURCE_CONSTANT || source->modifier ||
          !(program->immediate_mask & (1u << source->id)))
         continue;
      for (unsigned c = 0; c < 4; c++) {
         if (!(mov->destination.writemask & (1u << c)))
            continue;
         if (source->swizzle[c] > CHROME9_PS_SWIZZLE_W ||
             program->immediate[source->id][source->swizzle[c]] !=
                0x3f800000)
            literal = false;
      }
      if (!literal || mov->destination.file != CHROME9_PS_DESTINATION_TEMP)
         continue;
      /* Merge into an earlier modifier-free MOV of the same temporary. */
      for (unsigned j = 0; j < i; j++) {
         struct chrome9_ps_instruction *into = &ir->instruction[j];

         if (into->op != CHROME9_PS_OPCODE_MOV ||
             into->destination.file != CHROME9_PS_DESTINATION_TEMP ||
             into->destination.id != mov->destination.id ||
             into->destination.saturate != mov->destination.saturate ||
             into->source[0].modifier ||
             into->source[0].file == CHROME9_PS_SOURCE_POSITION)
            continue;
         for (unsigned c = 0; c < 4; c++)
            if (mov->destination.writemask & (1u << c))
               into->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_ONE;
         into->destination.writemask |= mov->destination.writemask;
         chrome9_ps_ir_remove(ir, i--);
         break;
      }
   }

   if (!ir->count)
      return;
   last = &ir->instruction[ir->count - 1];
   /* Several colour outputs keep their MOVs: the writes stay terminal. */
   if (ir->count > 1 &&
       ir->instruction[ir->count - 2].destination.file ==
       CHROME9_PS_DESTINATION_COLOR)
      return;
   if (last->op != CHROME9_PS_OPCODE_MOV ||
       last->destination.file != CHROME9_PS_DESTINATION_COLOR ||
       last->destination.writemask != 0xf ||
       last->source[0].file != CHROME9_PS_SOURCE_TEMP ||
       last->source[0].modifier)
      return;
   id = last->source[0].id;
   /* The depth MOV inserted before the color reads its temporary too. */
   if (ir->depth_output && ir->depth.file == CHROME9_PS_SOURCE_TEMP &&
       ir->depth.id == id)
      return;
   for (unsigned i = 0; i + 1 < ir->count; i++) {
      struct chrome9_ps_instruction *instruction = &ir->instruction[i];

      if (chrome9_ps_reads_temp(instruction, id))
         return;
      if (instruction->op == CHROME9_PS_OPCODE_TEX ||
          instruction->op == CHROME9_PS_OPCODE_KILL ||
          instruction->destination.file != CHROME9_PS_DESTINATION_TEMP ||
          instruction->destination.id != id)
         continue;
      if (producer)
         return;
      producer = instruction;
      producer_index = i;
   }
   if (!producer)
      return;
   for (unsigned i = 0; i < ir->count - 1; i++)
      if (ir->instruction[i].destination.file ==
             CHROME9_PS_DESTINATION_TEMP &&
          ir->instruction[i].destination.id == id &&
          &ir->instruction[i] != producer)
         return;
   if (!chrome9_ps_componentwise(producer->op))
      return;
   for (unsigned c = 0; c < 4; c++) {
      const uint8_t lane = last->source[0].swizzle[c];

      if (lane == CHROME9_PS_SWIZZLE_ONE) {
         /* 1 * 1 or a copy of 1 keeps a literal lane exact. */
         if (producer->op != CHROME9_PS_OPCODE_MOV &&
             producer->op != CHROME9_PS_OPCODE_MUL)
            return;
         for (unsigned s = 0; s < chrome9_ps_source_count(producer->op); s++)
            if (producer->source[s].modifier ||
                producer->source[s].file == CHROME9_PS_SOURCE_POSITION)
               return;
      } else if (lane > CHROME9_PS_SWIZZLE_W ||
                 !(producer->destination.writemask & (1u << lane))) {
         return;
      }
   }
   {
      struct chrome9_ps_instruction folded = *producer;

      for (unsigned s = 0; s < chrome9_ps_source_count(producer->op); s++)
         for (unsigned c = 0; c < 4; c++) {
            const uint8_t lane = last->source[0].swizzle[c];

            folded.source[s].swizzle[c] =
               lane == CHROME9_PS_SWIZZLE_ONE ? CHROME9_PS_SWIZZLE_ONE :
               producer->source[s].swizzle[lane];
         }
      folded.destination = last->destination;
      folded.destination.saturate |= producer->destination.saturate;
      /* Temporaries are written once per lane, so the producer's sources
       * still hold its operands at the end of the program. */
      ir->instruction[ir->count - 1] = folded;
      chrome9_ps_ir_remove(ir, producer_index);
   }
}

static unsigned
chrome9_ps_config_tier(unsigned peak)
{
   return peak <= 3 ? 3 : peak <= 4 ? 2 : peak <= 6 ? 1 : 0;
}

static bool
chrome9_compile_native_fs(struct chrome9_context *ctx,
                          struct chrome9_shader *shader, nir_shader *nir)
{
   struct chrome9_fs_key key;
   enum chrome9_shader_verify_error error;
   nir_shader *semantic_nir;
   bool from_nir, lowered;

   /* The semantic backend lowers vector constructors eagerly.  Remove dead
    * lanes first so a vec4 used only through .xy cannot materialize its
    * unused POSITION.zw components in the command stream.  Keep the
    * canonical shader intact for classification and variant compilation. */
   semantic_nir = nir_shader_clone(NULL, nir);
   if (!semantic_nir)
      return false;
   nir_shader_instructions_pass(semantic_nir,
      chrome9_nir_rewrite_shared_saturate_max,
      nir_metadata_control_flow, NULL);
   chrome9_nir_optimize_semantic(semantic_nir);
   nir_opt_shrink_vectors(semantic_nir, true);
   nir_opt_dce(semantic_nir);

   memset(&shader->semantic_fs_ir, 0, sizeof(shader->semantic_fs_ir));
   from_nir = chrome9_compile_ps_nir(semantic_nir, &shader->semantic_fs_ir,
                                     &shader->native_fs,
                                     &shader->semantic_uniform_mask,
                                     &shader->semantic_equal_zero_input_mask,
                                     &shader->semantic_identity_projector_input_mask,
                                     &shader->semantic_unit_interval_input_mask,
                                     &shader->semantic_cube_sampler_mask,
                                     &shader->semantic_volume_sampler_mask,
                                     ctx->fragment_kill,
                                     ctx->texture_bias,
                                     ctx->depth_output,
                                     &lowered);
   if (lowered && debug_get_bool_option("CHROME9_PS_FOLD", true)) {
      struct chrome9_ps_ir folded = shader->semantic_fs_ir;
      unsigned folded_peak;

      chrome9_ps_ir_fold_copies(&folded, &shader->native_fs);
      if (debug_get_bool_option("CHROME9_PS_COALESCE", true)) {
         chrome9_ps_ir_coalesce_lanes(&folded);
         chrome9_ps_ir_split_gathers(&folded);
         chrome9_ps_ir_merge_lanes(&folded);
         chrome9_ps_ir_fold_copies(&folded, &shader->native_fs);
      }
      if (chrome9_ps_ir_temp_pressure(&folded, &folded_peak))
         shader->semantic_fs_ir = folded;
   }
   if (lowered) {
      unsigned source_peak;

      if (!chrome9_ps_ir_temp_pressure(&shader->semantic_fs_ir,
                                       &source_peak)) {
         lowered = false;
      } else if (source_peak > 12) {
         struct chrome9_ps_ir scheduled;
         unsigned scheduled_peak;

         if (!chrome9_schedule_ps_ir(&shader->semantic_fs_ir, &scheduled,
                                     &scheduled_peak) ||
             scheduled_peak > 12)
            lowered = false;
         else
            shader->semantic_fs_ir = scheduled;
      } else if (source_peak > 3) {
         /* A90 HPSConFig keeps 2x, 3x or 4x the pixels in flight for
          * programs within 6, 4 or 3 physical temps. */
         struct chrome9_ps_ir scheduled;
         unsigned scheduled_peak;

         if (chrome9_schedule_ps_ir_target(&shader->semantic_fs_ir,
                                           &scheduled, &scheduled_peak,
                                           3, 512) &&
             chrome9_ps_config_tier(scheduled_peak) >
             chrome9_ps_config_tier(source_peak))
            shader->semantic_fs_ir = scheduled;
      }
   }
   ralloc_free(semantic_nir);
   shader->semantic_fs_valid = lowered;
   if (lowered) {
      for (unsigned i = 0; i < shader->semantic_fs_ir.count; i++) {
         const struct chrome9_ps_instruction *instruction =
            &shader->semantic_fs_ir.instruction[i];

         if (instruction->op == CHROME9_PS_OPCODE_TEX)
            shader->semantic_sampler_mask |= 1u << instruction->sampler;
         for (unsigned s = 0; s < 3; s++) {
            const struct chrome9_ps_source *source = &instruction->source[s];

            if (source->file == CHROME9_PS_SOURCE_CONSTANT)
               shader->semantic_constant_mask |= 1u << source->id;
            else if (source->file == CHROME9_PS_SOURCE_INPUT) {
               shader->semantic_input_mask |= 1u << source->id;
            } else if (source->file == CHROME9_PS_SOURCE_TEXTURE) {
               shader->semantic_varying_mask |= 1u << source->id;
            }
         }
      }
      /* A lowered program is usable only if every instruction can be
       * represented by the DRM UAPI.  Reject it at compile time, before
       * any draw can attempt to submit a partially translated program. */
      struct drm_chrome9_fragment_instruction
         instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
      unsigned instruction_count;

      if (!chrome9_fragment_build_instructions(
             &shader->semantic_fs_ir, shader->semantic_sampler_mask,
             instructions, &instruction_count))
         shader->semantic_fs_valid = false;
   }
   if (from_nir) {
      if (chrome9_fs_key_from_nir(nir, &key))
         shader->native_fs_key = key;
      shader->native_fs_from_nir = true;
      return true;
   }

   if (!chrome9_fs_native_key(shader->fragment_program, &key) ||
       !chrome9_compile_fs_offline(&key, &shader->native_fs))
      return false;

   if (!chrome9_verify_fs_offline(&key, &shader->native_fs, &error))
      return false;
   shader->native_fs_key = key;
   shader->native_fs_from_nir = false;
   return true;
}

static bool
chrome9_native_program_source(const struct chrome9_ps_source *source)
{
   if (!source || source->id || source->modifier || source->last_use ||
       (source->file != CHROME9_PS_SOURCE_INPUT &&
        source->file != CHROME9_PS_SOURCE_CONSTANT))
      return false;
   for (unsigned c = 0; c < ARRAY_SIZE(source->swizzle); c++) {
      if (source->swizzle[c] != c)
         return false;
   }
   return true;
}

static bool
chrome9_native_fs_is_program_batch(const struct chrome9_shader *shader,
                                   bool fragment_add)
{
   const struct chrome9_ps_instruction *instruction;

   if (!shader->native_fs_valid || !shader->native_fs_from_nir ||
       shader->semantic_fs_ir.count != 1 ||
       shader->native_fs.alu_count != 1 || shader->native_fs.tau_count ||
       shader->native_fs.sampler_mask ||
       (shader->native_fs.varying_mask & ~1u) ||
       (shader->native_fs.constant_mask & ~1u))
      return false;
   instruction = &shader->semantic_fs_ir.instruction[0];
   if (instruction->destination.file != CHROME9_PS_DESTINATION_COLOR ||
       instruction->destination.id ||
       instruction->destination.writemask != 0xf ||
       instruction->destination.saturate ||
       !chrome9_native_program_source(&instruction->source[0]))
      return false;
   switch (instruction->op) {
   case CHROME9_PS_OPCODE_MOV:
      return true;
   case CHROME9_PS_OPCODE_MUL:
      return chrome9_native_program_source(&instruction->source[1]) &&
             instruction->source[0].file != instruction->source[1].file;
   case CHROME9_PS_OPCODE_ADD:
      return fragment_add &&
             chrome9_native_program_source(&instruction->source[1]) &&
             instruction->source[0].file != instruction->source[1].file;
   default:
      return false;
   }
}

static bool
chrome9_tgsi_fs_passthrough(const struct tgsi_token *tokens)
{
   struct tgsi_parse_context parse;
   int input = -1, output = -1;
   unsigned moves = 0;
   bool valid = tokens && tgsi_parse_init(&parse, tokens) == TGSI_PARSE_OK;

   if (!valid)
      return false;
   while (!tgsi_parse_end_of_tokens(&parse) && valid) {
      const union tgsi_full_token *token;

      tgsi_parse_token(&parse);
      token = &parse.FullToken;
      switch (token->Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION: {
         const struct tgsi_full_declaration *decl = &token->FullDeclaration;

         if (decl->Declaration.File == TGSI_FILE_INPUT &&
             decl->Declaration.Semantic &&
             decl->Semantic.Name == TGSI_SEMANTIC_COLOR &&
             decl->Semantic.Index == 0 &&
             decl->Range.First == decl->Range.Last)
            input = decl->Range.First;
         else if (decl->Declaration.File == TGSI_FILE_OUTPUT &&
                  decl->Declaration.Semantic &&
                  decl->Semantic.Name == TGSI_SEMANTIC_COLOR &&
                  decl->Semantic.Index == 0 &&
                  decl->Range.First == decl->Range.Last)
            output = decl->Range.First;
         else if (decl->Declaration.File != TGSI_FILE_INPUT &&
                  decl->Declaration.File != TGSI_FILE_OUTPUT)
            valid = false;
         break;
      }
      case TGSI_TOKEN_TYPE_INSTRUCTION: {
         const struct tgsi_full_instruction *inst = &token->FullInstruction;
         const struct tgsi_dst_register *dst = &inst->Dst[0].Register;
         const struct tgsi_src_register *src = &inst->Src[0].Register;

         if (inst->Instruction.Opcode == TGSI_OPCODE_END)
            break;
         if (inst->Instruction.Opcode != TGSI_OPCODE_MOV || moves++ ||
             inst->Instruction.NumDstRegs != 1 ||
             inst->Instruction.NumSrcRegs != 1 ||
             dst->File != TGSI_FILE_OUTPUT || dst->Index != output ||
             dst->WriteMask != TGSI_WRITEMASK_XYZW || dst->Indirect ||
             src->File != TGSI_FILE_INPUT || src->Index != input ||
             src->Indirect || src->Negate || src->Absolute ||
             src->SwizzleX != TGSI_SWIZZLE_X ||
             src->SwizzleY != TGSI_SWIZZLE_Y ||
             src->SwizzleZ != TGSI_SWIZZLE_Z ||
             src->SwizzleW != TGSI_SWIZZLE_W)
            valid = false;
         break;
      }
      case TGSI_TOKEN_TYPE_PROPERTY:
         break;
      default:
         valid = false;
         break;
      }
   }
   tgsi_parse_free(&parse);
   return valid && input >= 0 && output >= 0 && moves == 1;
}

/*
 * The blitter's TGSI clear programs: no output at all (depth/stencil
 * clears) or MOV OUT[color0], CONST[n] (color clears).  Returns -1 when
 * neither, else the constant register (0 for the empty program with
 * *empty set).
 */
static int
chrome9_tgsi_fs_clear(const struct tgsi_token *tokens, bool *empty)
{
   struct tgsi_parse_context parse;
   int output = -1, constant = -1;
   unsigned instructions = 0;
   bool valid = tokens && tgsi_parse_init(&parse, tokens) == TGSI_PARSE_OK;

   if (!valid)
      return -1;
   while (!tgsi_parse_end_of_tokens(&parse) && valid) {
      const union tgsi_full_token *token;

      tgsi_parse_token(&parse);
      token = &parse.FullToken;
      switch (token->Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION: {
         const struct tgsi_full_declaration *decl = &token->FullDeclaration;

         if (decl->Declaration.File != TGSI_FILE_OUTPUT)
            break;
         if (output >= 0 || !decl->Declaration.Semantic ||
             decl->Semantic.Name != TGSI_SEMANTIC_COLOR ||
             decl->Semantic.Index || decl->Range.First != decl->Range.Last)
            valid = false;
         output = decl->Range.First;
         break;
      }
      case TGSI_TOKEN_TYPE_INSTRUCTION: {
         const struct tgsi_full_instruction *inst =
            &token->FullInstruction;
         const struct tgsi_dst_register *dst = &inst->Dst[0].Register;
         const struct tgsi_src_register *src = &inst->Src[0].Register;

         if (inst->Instruction.Opcode == TGSI_OPCODE_END)
            break;
         if (inst->Instruction.Opcode != TGSI_OPCODE_MOV || instructions++ ||
             dst->File != TGSI_FILE_OUTPUT || dst->Index != output ||
             dst->WriteMask != TGSI_WRITEMASK_XYZW || dst->Indirect ||
             src->File != TGSI_FILE_CONSTANT || src->Indirect ||
             src->Negate || src->Absolute ||
             (src->Dimension && inst->Src[0].Dimension.Index) ||
             src->SwizzleX != TGSI_SWIZZLE_X ||
             src->SwizzleY != TGSI_SWIZZLE_Y ||
             src->SwizzleZ != TGSI_SWIZZLE_Z ||
             src->SwizzleW != TGSI_SWIZZLE_W || src->Index >= 32)
            valid = false;
         constant = src->Index;
         break;
      }
      case TGSI_TOKEN_TYPE_PROPERTY:
      case TGSI_TOKEN_TYPE_IMMEDIATE:
         break;
      default:
         valid = false;
         break;
      }
   }
   tgsi_parse_free(&parse);
   if (!valid)
      return -1;
   *empty = output < 0 && !instructions;
   if (*empty)
      return 0;
   return output >= 0 && instructions == 1 ? constant : -1;
}

/* MOV oC0, primary: passthrough programs the semantic lowering skips. */
static void
chrome9_semantic_primary_passthrough(struct chrome9_shader *shader)
{
   struct chrome9_ps_instruction *mov = &shader->semantic_fs_ir.instruction[0];

   memset(&shader->semantic_fs_ir, 0, sizeof(shader->semantic_fs_ir));
   mov->op = CHROME9_PS_OPCODE_MOV;
   mov->destination.file = CHROME9_PS_DESTINATION_COLOR;
   mov->destination.writemask = 0xf;
   mov->source[0].file = CHROME9_PS_SOURCE_INPUT;
   for (unsigned i = 0; i < 4; i++)
      mov->source[0].swizzle[i] = i;
   shader->semantic_fs_ir.count = 1;
   shader->semantic_input_mask = 1;
   shader->semantic_fs_valid = true;
   shader->semantic_passthrough = true;
}

/* MOV oC0, CB0[constant]: the blitter's color clear. */
static void
chrome9_semantic_constant_color(struct chrome9_shader *shader,
                                unsigned constant)
{
   struct chrome9_ps_instruction *mov = &shader->semantic_fs_ir.instruction[0];

   memset(&shader->semantic_fs_ir, 0, sizeof(shader->semantic_fs_ir));
   mov->op = CHROME9_PS_OPCODE_MOV;
   mov->destination.file = CHROME9_PS_DESTINATION_COLOR;
   mov->destination.writemask = 0xf;
   mov->source[0].file = CHROME9_PS_SOURCE_CONSTANT;
   mov->source[0].id = constant;
   for (unsigned i = 0; i < 4; i++)
      mov->source[0].swizzle[i] = i;
   shader->semantic_fs_ir.count = 1;
   shader->semantic_input_mask = 0;
   shader->semantic_constant_mask = 1u << constant;
   shader->semantic_fs_valid = true;
   shader->fragment_program = CHROME9_FS_COMPILED_ALU;
   shader->supported = true;
}

void
chrome9_compile_fs_nir(struct chrome9_context *ctx,
                       struct chrome9_shader *shader, nir_shader *nir)
{
   bool straight_line = chrome9_fs_straight_line_entrypoint(nir, NULL, NULL);

   /* No color, depth or discard output (depth/stencil-only passes such as
    * the blitter's clears): the color the program writes is masked off. */
   if (straight_line && !nir->info.fs.uses_discard &&
       !(nir->info.outputs_written &
         (BITFIELD64_BIT(FRAG_RESULT_COLOR) |
          BITFIELD64_BIT(FRAG_RESULT_DATA0) |
          BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
          BITFIELD64_BIT(FRAG_RESULT_STENCIL)))) {
      chrome9_semantic_primary_passthrough(shader);
      shader->fragment_program = CHROME9_FS_PRIMARY_COLOR;
      shader->supported = true;
      return;
   }
   shader->fragment_program = straight_line ?
      chrome9_fs_classify(nir, &shader->literal_color) :
      CHROME9_FS_UNSUPPORTED;
   shader->flat_primary =
      shader->fragment_program == CHROME9_FS_PRIMARY_COLOR &&
      chrome9_fs_has_flat_primary(nir);
   shader->flat_generic = shader->fragment_program ==
      CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX;
   shader->native_fs_valid = straight_line &&
      chrome9_compile_native_fs(ctx, shader, nir);
   /* Flat colors arrive per vertex already (draw module flat stage or
    * the hardware flat mode), so the program is a plain primary MOV. */
   if (!shader->semantic_fs_valid && shader->flat_primary)
      chrome9_semantic_primary_passthrough(shader);
   if (shader->semantic_fs_valid && ctx->draw_fragment_program &&
       (!shader->semantic_input_mask ||
        (ctx->fragment_primary && shader->semantic_input_mask == 1)) &&
       shader->fragment_program == CHROME9_FS_UNSUPPORTED)
      shader->fragment_program = CHROME9_FS_COMPILED_ALU;
   if (shader->fragment_program == CHROME9_FS_UNSUPPORTED &&
       ctx->draw_program_triangle_d16_rhw_batch &&
       chrome9_native_fs_is_program_batch(shader, ctx->fragment_add))
      shader->fragment_program = CHROME9_FS_COMPILED_ALU;
   shader->supported = shader->fragment_program != CHROME9_FS_UNSUPPORTED;
}

static void
chrome9_capture_fs_variant(struct chrome9_fs_variant *variant,
                           const struct chrome9_shader *shader)
{
   variant->fragment_program = shader->fragment_program;
   variant->literal_color = shader->literal_color;
   variant->native_fs_key = shader->native_fs_key;
   variant->semantic_fs_ir = shader->semantic_fs_ir;
   variant->native_fs = shader->native_fs;
   variant->semantic_constant_mask = shader->semantic_constant_mask;
   variant->semantic_uniform_mask = shader->semantic_uniform_mask;
   variant->semantic_input_mask = shader->semantic_input_mask;
   variant->semantic_varying_mask = shader->semantic_varying_mask;
   variant->semantic_sampler_mask = shader->semantic_sampler_mask;
   variant->semantic_cube_sampler_mask = shader->semantic_cube_sampler_mask;
   variant->semantic_volume_sampler_mask =
      shader->semantic_volume_sampler_mask;
   variant->semantic_equal_zero_input_mask =
      shader->semantic_equal_zero_input_mask;
   variant->semantic_identity_projector_input_mask =
      shader->semantic_identity_projector_input_mask;
   variant->semantic_unit_interval_input_mask =
      shader->semantic_unit_interval_input_mask;
   variant->semantic_fs_valid = shader->semantic_fs_valid;
   variant->semantic_passthrough = shader->semantic_passthrough;
   variant->native_fs_valid = shader->native_fs_valid;
   variant->native_fs_from_nir = shader->native_fs_from_nir;
   variant->flat_primary = shader->flat_primary;
   variant->flat_generic = shader->flat_generic;
   variant->supported = shader->supported;
}

static void
chrome9_apply_fs_variant(struct chrome9_shader *shader,
                         const struct chrome9_fs_variant *variant)
{
   if (shader->applied == variant)
      return;
   shader->applied = variant;
   shader->fragment_program = variant->fragment_program;
   shader->literal_color = variant->literal_color;
   shader->native_fs_key = variant->native_fs_key;
   shader->semantic_fs_ir = variant->semantic_fs_ir;
   shader->native_fs = variant->native_fs;
   shader->semantic_constant_mask = variant->semantic_constant_mask;
   shader->semantic_uniform_mask = variant->semantic_uniform_mask;
   shader->semantic_input_mask = variant->semantic_input_mask;
   shader->semantic_varying_mask = variant->semantic_varying_mask;
   shader->semantic_sampler_mask = variant->semantic_sampler_mask;
   shader->semantic_cube_sampler_mask = variant->semantic_cube_sampler_mask;
   shader->semantic_volume_sampler_mask =
      variant->semantic_volume_sampler_mask;
   shader->semantic_equal_zero_input_mask =
      variant->semantic_equal_zero_input_mask;
   shader->semantic_identity_projector_input_mask =
      variant->semantic_identity_projector_input_mask;
   shader->semantic_unit_interval_input_mask =
      variant->semantic_unit_interval_input_mask;
   shader->semantic_fs_valid = variant->semantic_fs_valid;
   shader->semantic_passthrough = variant->semantic_passthrough;
   shader->native_fs_valid = variant->native_fs_valid;
   shader->native_fs_from_nir = variant->native_fs_from_nir;
   shader->flat_primary = variant->flat_primary;
   shader->flat_generic = variant->flat_generic;
   shader->supported = variant->supported;
}

#define CHROME9_MAX_FS_VARIANTS 64

static void
chrome9_dump_semantic_fs(const struct chrome9_shader *shader)
{
   const struct chrome9_ps_ir *ir = &shader->semantic_fs_ir;

   if (!debug_get_bool_option("CHROME9_DUMP_SEMANTIC_FS", false) ||
       !shader->semantic_fs_valid)
      return;
   unsigned peak = 0;

   chrome9_ps_ir_temp_pressure(ir, &peak);
   fprintf(stderr, "chrome9: semantic FS count=%u peak=%u constants=%08x "
           "uniforms=%08x immediates=%08x\n", ir->count, peak,
           shader->semantic_constant_mask, shader->semantic_uniform_mask,
           shader->native_fs.immediate_mask);
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &ir->instruction[i];

      fprintf(stderr, "chrome9: semantic %u op=%u dst=%u.%u/%x%s", i,
              instruction->op, instruction->destination.file,
              instruction->destination.id,
              instruction->destination.writemask,
              instruction->destination.saturate ? "_sat" : "");
      for (unsigned s = 0; s < 3; s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];

         fprintf(stderr, " s%u=%s%u.%u/%u%u%u%u", s,
                 source->modifier ? "-" : "", source->file,
                 source->id, source->swizzle[0], source->swizzle[1],
                 source->swizzle[2], source->swizzle[3]);
      }
      fputc('\n', stderr);
   }
}

/* Choose a fragment program only when all control-flow uniforms have their
 * draw-time values.  Failed variants are cached too, so an unsupported bound
 * is deterministic and never leaves a partially compiled program active. */
static bool
chrome9_select_fs_variant(struct chrome9_context *ctx)
{
   struct chrome9_shader *shader = ctx->fs;
   uint16_t shadow_keys[PIPE_MAX_SAMPLERS] = { 0 };
   nir_shader *canonical;
   unsigned count;
   uint8_t face_mode = 0;
   size_t bytes;

   if (!shader || !shader->nir)
      return shader && shader->supported;
   u_foreach_bit(unit, shader->shadow_sampler_mask & BITFIELD_MASK(PIPE_MAX_SAMPLERS))
      shadow_keys[unit] = chrome9_shadow_key(
         unit < ctx->num_sampler_states && ctx->sampler_states[unit] ?
         &ctx->sampler_states[unit]->base : NULL,
         unit < ctx->num_sampler_views ? ctx->sampler_views[unit] : NULL);
   /* A dynamic canonical program needs no variants.  In particular, a
    * natively lowered select must not exhaust the bounded variant cache as
    * its control uniform takes on many different values. */
   if (shader->canonical_supported && !ctx->fs_sprite_flip) {
      chrome9_apply_fs_variant(shader, shader->canonical);
      return true;
   }
   canonical = shader->nir;
   count = canonical->info.num_inlinable_uniforms;
   if (ctx->rasterizer) {
      if (ctx->rasterizer->cull_face == PIPE_FACE_BACK)
         face_mode = 1;
      else if (ctx->rasterizer->cull_face == PIPE_FACE_FRONT)
         face_mode = 2;
   }
   if (!count && !face_mode && !shader->shadow_sampler_mask &&
       !ctx->fs_sprite_flip) {
      /* A previous cull-specialized variant must not survive a transition
       * back to dynamic FACE with no uniform specialization key. */
      shader->applied = NULL;
      shader->supported = false;
      return false;
   }
   if (count && ctx->fs_inline_count != count) {
      shader->applied = NULL;
      shader->supported = false;
      return false;
   }
   bytes = count * sizeof(ctx->fs_inline[0]);

   for (struct chrome9_fs_variant *variant = shader->variants;
        variant; variant = variant->next) {
      if (variant != shader->canonical && variant->face_mode == face_mode &&
          variant->sprite_flip == ctx->fs_sprite_flip &&
          !memcmp(variant->inline_values, ctx->fs_inline, bytes) &&
          !memcmp(variant->shadow_keys, shadow_keys, sizeof(shadow_keys))) {
         chrome9_apply_fs_variant(shader, variant);
         return shader->supported;
      }
   }
   if (shader->variant_count >= CHROME9_MAX_FS_VARIANTS) {
      shader->applied = NULL;
      shader->supported = false;
      return false;
   }

   struct chrome9_fs_variant *variant = CALLOC_STRUCT(chrome9_fs_variant);
   nir_shader *clone = variant ? nir_shader_clone(NULL, canonical) : NULL;

   if (!clone) {
      FREE(variant);
      shader->applied = NULL;
      shader->supported = false;
      return false;
   }
   struct chrome9_shader compiled = { 0 };

   variant->face_mode = face_mode;
   variant->sprite_flip = ctx->fs_sprite_flip;
   memcpy(variant->inline_values, ctx->fs_inline, bytes);
   memcpy(variant->shadow_keys, shadow_keys, sizeof(shadow_keys));
   if (count)
      chrome9_nir_inline_uniforms(clone, variant->inline_values);
   if (shader->shadow_sampler_mask)
      chrome9_nir_lower_shadow(clone, PIPE_MAX_SAMPLERS, shadow_keys);
   if (face_mode)
      chrome9_nir_specialize_front_face(clone, face_mode == 1);
   if (ctx->fs_sprite_flip)
      chrome9_nir_flip_sprite_coords(clone, ctx->fs_sprite_flip);
   chrome9_compile_fs_nir(ctx, &compiled, clone);
   chrome9_capture_fs_variant(variant, &compiled);
   variant->next = shader->variants;
   shader->variants = variant;
   shader->variant_count++;
   chrome9_apply_fs_variant(shader, variant);
   chrome9_dump_semantic_fs(shader);

   if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false)) {
      fprintf(stderr, "chrome9: FS variant %u face=%u",
              shader->variant_count, face_mode);
      for (unsigned i = 0; i < count; i++)
         fprintf(stderr, " dw%u=%08x",
                 canonical->info.inlinable_uniform_dw_offsets[i],
                 variant->inline_values[i]);
      fprintf(stderr, " supported=%u ir=%u alu=%u tau=%u\n",
              shader->supported, shader->semantic_fs_ir.count,
              shader->native_fs.alu_count, shader->native_fs.tau_count);
   }
   if (!shader->supported &&
       debug_get_bool_option("CHROME9_DUMP_UNSUPPORTED_NIR", false)) {
      fprintf(stderr, "chrome9: unsupported specialized FS %p\n",
              (void *)shader);
      nir_print_shader(clone, stderr);
   }
   ralloc_free(clone);
   return shader->supported;
}

/* CHROME9_DEBUG_FS_MARK=lo[-hi] replaces the color output of the fragment
 * shaders created in that index range by magenta; "id" paints every shader
 * with its index as R=bits 2:0, G=5:3, B=7:6, to locate a material. */
static void
chrome9_debug_mark_fs(nir_shader *nir, unsigned index)
{
   const char *range = getenv("CHROME9_DEBUG_FS_MARK");
   float color[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
   unsigned lo, hi;

   if (range && !strcmp(range, "id")) {
      lo = 0;
      hi = UINT_MAX;
      color[0] = (index & 7) / 7.0f;
      color[1] = ((index >> 3) & 7) / 7.0f;
      color[2] = ((index >> 6) & 3) / 3.0f;
   } else if (!range || sscanf(range, "%u-%u", &lo, &hi) < 1) {
      return;
   } else if (!strchr(range, '-')) {
      hi = lo;
   }
   fprintf(stderr, "chrome9: FS index %u%s\n", index,
           index >= lo && index <= hi ? " marked" : "");
   if (index < lo || index > hi)
      return;
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            unsigned value;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            intr = nir_instr_as_intrinsic(instr);
            value = intr->intrinsic == nir_intrinsic_store_deref;

            if ((intr->intrinsic != nir_intrinsic_store_output &&
                 (!value || !nir_deref_mode_is(nir_src_as_deref(intr->src[0]),
                                               nir_var_shader_out))) ||
                intr->src[value].ssa->num_components != 4)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&intr->src[value],
                            nir_imm_vec4(&b, color[0], color[1], color[2],
                                         color[3]));
         }
      }
      nir_opt_dce(nir);
   }
}

/* Fragment varying n reads TEXn or VARn.  A VARn read beside TEXn moves
 * to a free index k: the NIR input becomes VARk and varying_slot[k] keeps
 * the VARn the vertex stage writes. */
static void
chrome9_fs_assign_varyings(struct chrome9_shader *shader, nir_shader *nir)
{
   const uint64_t read = nir->info.inputs_read;
   uint8_t used = 0, move[DRM_CHROME9_FRAGMENT_MAX_VARYINGS] = { 0 };
   bool moved = false;

   memset(shader->varying_slot, 0, sizeof(shader->varying_slot));
   for (unsigned n = 0; n < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; n++) {
      if (read & BITFIELD64_BIT(VARYING_SLOT_TEX0 + n)) {
         shader->varying_slot[n] = VARYING_SLOT_TEX0 + n;
         used |= BITFIELD_BIT(n);
      } else if (read & BITFIELD64_BIT(VARYING_SLOT_VAR0 + n)) {
         shader->varying_slot[n] = VARYING_SLOT_VAR0 + n;
         used |= BITFIELD_BIT(n);
      }
   }
   for (unsigned n = 0; n < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; n++) {
      unsigned k;

      if (!(read & BITFIELD64_BIT(VARYING_SLOT_TEX0 + n)) ||
          !(read & BITFIELD64_BIT(VARYING_SLOT_VAR0 + n)) ||
          nir->info.io_lowered)
         continue;
      k = ffs(~used & BITFIELD_MASK(DRM_CHROME9_FRAGMENT_MAX_VARYINGS));
      if (!k)
         break;
      k--;
      shader->varying_slot[k] = VARYING_SLOT_VAR0 + n;
      used |= BITFIELD_BIT(k);
      move[n] = VARYING_SLOT_VAR0 + k;
      moved = true;
   }
   if (!moved)
      return;
   /* The llvmpipe fallback links by the original slots. */
   if (chrome9_sw_enabled() && !shader->sw_nir)
      shader->sw_nir = nir_shader_clone(NULL, nir);
   nir_foreach_shader_in_variable(var, nir) {
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location < VARYING_SLOT_VAR0 +
                               DRM_CHROME9_FRAGMENT_MAX_VARYINGS &&
          move[var->data.location - VARYING_SLOT_VAR0])
         var->data.location = move[var->data.location - VARYING_SLOT_VAR0];
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static void *
chrome9_create_fs_state(struct pipe_context *pipe,
                        const struct pipe_shader_state *state)
{
   static unsigned fs_index;
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = CALLOC_STRUCT(chrome9_shader);

   if (!shader)
      return NULL;
   if (state->type == PIPE_SHADER_IR_NIR && state->ir.nir)
      chrome9_debug_mark_fs(state->ir.nir, fs_index);
   fs_index++;
   shader->draw = draw_create_fragment_shader(ctx->draw, state);
   if (!shader->draw) {
      FREE(shader);
      return NULL;
   }
   if (state->type == PIPE_SHADER_IR_NIR && state->ir.nir) {
      if (chrome9_nir_needs_hw_texture_lowering(state->ir.nir)) {
         if (chrome9_sw_enabled())
            shader->sw_nir = nir_shader_clone(NULL, state->ir.nir);
         chrome9_nir_lower_hw_textures(state->ir.nir);
      }
      shader->shadow_sampler_mask =
         chrome9_nir_shadow_sampler_mask(state->ir.nir);
      chrome9_fs_assign_varyings(shader, state->ir.nir);
      shader->nir = state->ir.nir;
      if (debug_get_bool_option("CHROME9_DUMP_NIR", false))
         nir_print_shader(state->ir.nir, stderr);
      chrome9_compile_fs_nir(ctx, shader, state->ir.nir);
      shader->canonical_supported = shader->supported;
      if (shader->canonical_supported) {
         struct chrome9_fs_variant *variant =
            CALLOC_STRUCT(chrome9_fs_variant);

         /* Without the copy every draw selects a variant. */
         if (variant) {
            chrome9_capture_fs_variant(variant, shader);
            variant->next = shader->variants;
            shader->variants = variant;
            shader->canonical = shader->applied = variant;
         } else {
            shader->canonical_supported = false;
         }
      }
      chrome9_dump_semantic_fs(shader);
      if (!state->ir.nir->info.num_inlinable_uniforms && !shader->supported &&
          debug_get_bool_option("CHROME9_DUMP_UNSUPPORTED_NIR", false)) {
         fprintf(stderr, "chrome9: unsupported FS %p\n", (void *)shader);
         nir_print_shader(state->ir.nir, stderr);
      }
      if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false))
         fprintf(stderr, "chrome9: create FS %p program=%u supported=%u "
                 "semantic=%u native=%u nir=%u ir=%u/%u alu=%u tau=%u "
                 "constants=%08x immediates=%08x\n",
                 (void *)shader, shader->fragment_program,
                 shader->supported, shader->semantic_fs_valid,
                 shader->native_fs_valid,
                 shader->native_fs_from_nir,
                 shader->semantic_fs_ir.count,
                 __builtin_popcount(shader->semantic_sampler_mask),
                 shader->native_fs.alu_count,
                 shader->native_fs.tau_count,
                 shader->native_fs.constant_mask,
                 shader->native_fs.immediate_mask);
   } else if (state->type == PIPE_SHADER_IR_TGSI) {
      bool empty = false;
      int constant = chrome9_tgsi_fs_clear(state->tokens, &empty);

      shader->supported = chrome9_tgsi_fs_passthrough(state->tokens) ||
                          empty;
      shader->fragment_program = shader->supported ?
         CHROME9_FS_PRIMARY_COLOR : CHROME9_FS_UNSUPPORTED;
      /* The same MOV oC0, primary the NIR path lowers, for generic draws;
       * an empty program's color is masked off by its blend state. */
      if (shader->supported)
         chrome9_semantic_primary_passthrough(shader);
      else if (constant >= 0 && ctx->draw_fragment_program)
         chrome9_semantic_constant_color(shader, constant);
   }
   if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false) &&
       shader->supported)
      fprintf(stderr, "chrome9: native FS valid=%u nir=%u alu=%u tau=%u\n",
              shader->native_fs_valid, shader->native_fs_from_nir,
              shader->native_fs.alu_count, shader->native_fs.tau_count);
   return shader;
}

static void
chrome9_bind_fs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   ctx->fs = shader;
   if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false))
      fprintf(stderr, "chrome9: bind FS %p program=%u supported=%u\n",
              (void *)shader,
              shader ? shader->fragment_program : 0,
              shader ? shader->supported : 0);
   draw_bind_fragment_shader(ctx->draw, shader ? shader->draw : NULL);
}

static void
chrome9_delete_fs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   if (!shader)
      return;
   chrome9_sw_forget(ctx, shader);
   if (ctx->fs == shader) {
      ctx->fs = NULL;
      draw_bind_fragment_shader(ctx->draw, NULL);
   }
   draw_delete_fragment_shader(ctx->draw, shader->draw);
   while (shader->variants) {
      struct chrome9_fs_variant *variant = shader->variants;

      shader->variants = variant->next;
      FREE(variant);
   }
   ralloc_free(shader->sw_nir);
   ralloc_free(shader->nir);
   FREE(shader);
}

static void *
chrome9_create_vertex_elements_state(struct pipe_context *pipe,
                                     unsigned count,
                                     const struct pipe_vertex_element *elements)
{
   struct chrome9_vertex_elements *state;

   (void)pipe;
   if (count > PIPE_MAX_ATTRIBS)
      return NULL;
   state = CALLOC_STRUCT(chrome9_vertex_elements);
   if (!state)
      return NULL;
   state->count = count;
   memcpy(state->elements, elements, count * sizeof(*elements));
   return state;
}

static void
chrome9_bind_vertex_elements_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_vertex_elements *elements = state;

   ctx->vertex_elements = elements;
   draw_set_vertex_elements(ctx->draw, elements ? elements->count : 0,
                            elements ? elements->elements : NULL);
}

static void
chrome9_delete_vertex_elements_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   chrome9_sw_forget(ctx, state);
   if (ctx->vertex_elements == state)
      ctx->vertex_elements = NULL;
   FREE(state);
}

static void
chrome9_set_vertex_buffers(struct pipe_context *pipe, unsigned count,
                           const struct pipe_vertex_buffer *buffers)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   util_set_vertex_buffers_count(ctx->vertex_buffers,
                                 &ctx->num_vertex_buffers, buffers, count);
   draw_set_vertex_buffers(ctx->draw, count, buffers);
}

static void
chrome9_set_constant_buffer(struct pipe_context *pipe,
                            mesa_shader_stage shader, unsigned index,
                            const struct pipe_constant_buffer *buffer)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (index >= PIPE_MAX_CONSTANT_BUFFERS)
      return;
   if (shader == MESA_SHADER_VERTEX)
      util_copy_constant_buffer(&ctx->vs_constants[index], buffer);
   else if (shader == MESA_SHADER_FRAGMENT)
      util_copy_constant_buffer(&ctx->fs_constants[index], buffer);
}

static void
chrome9_set_inlinable_constants(struct pipe_context *pipe,
                                mesa_shader_stage shader,
                                unsigned num_values, uint32_t *values)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   unsigned count = MIN2(num_values, MAX_INLINABLE_UNIFORMS);

   if (shader == MESA_SHADER_FRAGMENT) {
      memset(ctx->fs_inline, 0, sizeof(ctx->fs_inline));
      if (count)
         memcpy(ctx->fs_inline, values, count * sizeof(*values));
      ctx->fs_inline_count = count;
   }
}

static struct pipe_sampler_view *
chrome9_create_sampler_view(struct pipe_context *pipe,
                            struct pipe_resource *texture,
                            const struct pipe_sampler_view *templ)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_sampler_view *cview = CALLOC_STRUCT(chrome9_sampler_view);
   struct pipe_sampler_view *view = cview ? &cview->base : NULL;

   /* The sampler reads memory: sampled depth stays materialized. */
   if (texture && util_format_is_depth_or_stencil(texture->format)) {
      chrome9_resource_fast_z_resolve(chrome9_resource(texture));
      chrome9_resource(texture)->fast_z_disabled = true;
   }
   /* A cube view covers all six faces of a guarded cube resource. */
   const bool cube = texture && texture->target == PIPE_TEXTURE_CUBE &&
                     templ->target == PIPE_TEXTURE_CUBE &&
                     chrome9_resource(texture)->guarded;
   /* Any 3D view is valid: the draw decides between the sampler and
    * llvmpipe (chrome9_draw_needs_sw_textures). */
   const bool volume = view && texture &&
                       texture->target == PIPE_TEXTURE_3D &&
                       templ->target == PIPE_TEXTURE_3D &&
                       templ->format == texture->format &&
                       templ->u.tex.first_level <= templ->u.tex.last_level &&
                       templ->u.tex.last_level <= texture->last_level &&
                       chrome9_resource(texture)->guarded;
   bool supported = view && texture && !volume &&
                    ((chrome9_is_surface_target(texture->target) &&
                      templ->target == texture->target) || cube) &&
                    templ->format == texture->format &&
                    /* Formats without a sampler encoding (Z16) are
                     * sampled by llvmpipe. */
                    (chrome9_texture_format_info(templ->format) ||
                     chrome9_sw_enabled()) &&
                    templ->u.tex.first_level <= templ->u.tex.last_level &&
                    (!templ->u.tex.first_level ||
                     chrome9_resource(texture)->guarded) &&
                    templ->u.tex.last_level <= texture->last_level &&
                    !templ->u.tex.first_layer &&
                    templ->u.tex.last_layer ==
                       (cube ? texture->array_size - 1 : 0);

   supported |= volume;
   /* llvmpipe samples swizzles the native decoders cannot express. */
   if (supported)
      cview->sw_only = !chrome9_texture_swizzle_supported(
         templ->format, templ->swizzle_r, templ->swizzle_g,
         templ->swizzle_b, templ->swizzle_a) ||
         /* The cube sampler takes POT faces only. */
         (cube && !util_is_power_of_two_nonzero(texture->width0)) ||
         chrome9_resource(texture)->sampler_unsupported;
   if (supported && !chrome9_resource(texture)->guarded) {
      struct pipe_resource shadow_templ = *texture;

      /* Imported images expose only their level-zero plane layout. */
      if (templ->u.tex.last_level) {
         supported = false;
      } else {
         shadow_templ.last_level = 0;
         shadow_templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
         shadow_templ.flags = 0;
         cview->shadow = pipe->screen->resource_create(pipe->screen,
                                                        &shadow_templ);
         supported = cview->shadow != NULL;
      }
   }

   if (!supported) {
      if (!(ctx->warnings & CHROME9_WARN_SAMPLER_VIEW_CREATE)) {
         fprintf(stderr,
                 "chrome9: sampler view rejected: alloc=%u texture=%p "
                 "resource_target=%u resource_format=%u size=%ux%u "
                 "last=%u bind=%#x view_target=%u view_format=%u "
                 "levels=%u..%u layers=%u..%u swizzle=%u/%u/%u/%u\n",
                 view != NULL, (void *)texture,
                 texture ? texture->target : 0,
                 texture ? texture->format : 0,
                 texture ? texture->width0 : 0,
                 texture ? texture->height0 : 0,
                 texture ? texture->last_level : 0,
                 texture ? texture->bind : 0,
                 templ->target, templ->format,
                 templ->u.tex.first_level, templ->u.tex.last_level,
                 templ->u.tex.first_layer, templ->u.tex.last_layer,
                 templ->swizzle_r, templ->swizzle_g,
                 templ->swizzle_b, templ->swizzle_a);
         ctx->warnings |= CHROME9_WARN_SAMPLER_VIEW_CREATE;
      }
      if (cview && cview->shadow)
         pipe_resource_reference(&cview->shadow, NULL);
      FREE(cview);
      return NULL;
   }
   *view = *templ;
   pipe_reference_init(&view->reference, 1);
   view->texture = NULL;
   pipe_resource_reference(&view->texture, texture);
   view->context = pipe;
   return view;
}

static void
chrome9_sampler_view_destroy(struct pipe_context *pipe,
                             struct pipe_sampler_view *view)
{
   struct chrome9_sampler_view *cview = chrome9_sampler_view(view);

   (void)pipe;
   pipe_resource_reference(&view->texture, NULL);
   pipe_resource_reference(&cview->shadow, NULL);
   FREE(cview);
}

static void
chrome9_set_sampler_views(struct pipe_context *pipe, mesa_shader_stage shader,
                          unsigned start, unsigned count,
                          unsigned unbind_trailing,
                          struct pipe_sampler_view **views)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   bool report = count && !(ctx->warnings & CHROME9_WARN_SAMPLER_VIEW_BIND) &&
                 debug_get_bool_option("CHROME9_DEBUG_SAMPLERS", false);
   unsigned end;

   if (shader == MESA_SHADER_VERTEX) {
      struct pipe_sampler_view *new_views[CHROME9_VS_MAX_SAMPLERS];
      unsigned num_views = 0;

      if (start >= CHROME9_VS_MAX_SAMPLERS)
         return;
      memcpy(new_views, ctx->vs_sampler_views, sizeof(new_views));
      end = MIN2(start + count, CHROME9_VS_MAX_SAMPLERS);
      for (unsigned i = start; i < end; i++)
         new_views[i] = views ? views[i - start] : NULL;
      end = MIN2(end + unbind_trailing, CHROME9_VS_MAX_SAMPLERS);
      for (unsigned i = start + count; i < end; i++)
         new_views[i] = NULL;
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_views[i])
            num_views = i + 1;
      /* draw_set_sampler_views flushes old draw state.  Keep old views alive
       * until after that flush before replacing our references. */
      draw_set_sampler_views(ctx->draw, MESA_SHADER_VERTEX,
                             new_views, num_views);
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++) {
         if (new_views[i] != ctx->vs_sampler_views[i])
            pipe_sampler_view_reference(&ctx->vs_sampler_views[i],
                                        new_views[i]);
         if (!new_views[i])
            chrome9_vs_texture_mapping_release(&ctx->vs_texture_mappings[i]);
      }
      ctx->num_vs_sampler_views = num_views;
      return;
   }
   if (shader != MESA_SHADER_FRAGMENT || start >= PIPE_MAX_SHADER_SAMPLER_VIEWS)
      return;
   end = MIN2(start + count, PIPE_MAX_SHADER_SAMPLER_VIEWS);
   for (unsigned i = start; i < end; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i],
                                  views ? views[i - start] : NULL);
   end = MIN2(end + unbind_trailing, PIPE_MAX_SHADER_SAMPLER_VIEWS);
   for (unsigned i = start + count; i < end; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   ctx->num_sampler_views = 0;
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      if (ctx->sampler_views[i])
         ctx->num_sampler_views = i + 1;
   if (report) {
      struct pipe_sampler_view *view = views ? views[0] : NULL;
      struct pipe_resource *texture = view ? view->texture : NULL;

      fprintf(stderr,
              "chrome9: sampler views set: shader=%u start=%u count=%u "
              "unbind=%u first_view=%p texture=%p target=%u format=%u "
              "levels=%u..%u resulting=%u\n",
              shader, start, count, unbind_trailing, (void *)view,
              (void *)texture, texture ? texture->target : 0,
              texture ? texture->format : 0,
              view ? view->u.tex.first_level : 0,
              view ? view->u.tex.last_level : 0,
              ctx->num_sampler_views);
      ctx->warnings |= CHROME9_WARN_SAMPLER_VIEW_BIND;
   }
}

static void
chrome9_shadow_list_free(struct chrome9_shadow *shadow)
{
   while (shadow) {
      struct chrome9_shadow *next = shadow->next;

      pipe_resource_reference(&shadow->shadow, NULL);
      FREE(shadow);
      shadow = next;
   }
}

/*
 * One guarded shadow per image (level and layer) of a render target the
 * hardware cannot address, reused across binds.  Entries whose resource
 * only this cache still references are dropped first.
 */
static struct chrome9_shadow *
chrome9_render_shadow(struct chrome9_context *ctx, struct pipe_resource *color,
                      unsigned level, unsigned layer)
{
   struct pipe_screen *screen = ctx->base.screen;
   struct pipe_resource templ = *color;
   struct pipe_resource *shadow;
   struct chrome9_shadow *record;
   struct hash_entry *entry;

   if (!ctx->shadows &&
       !(ctx->shadows = _mesa_pointer_hash_table_create(NULL)))
      return NULL;
   hash_table_foreach(ctx->shadows, stale) {
      struct pipe_resource *key = (struct pipe_resource *)stale->key;
      int held = 1;

      for (struct chrome9_shadow *entry = stale->data; entry;
           entry = entry->next)
         held += entry->direct;
      if (p_atomic_read(&key->reference.count) > held)
         continue;
      chrome9_shadow_list_free(stale->data);
      _mesa_hash_table_remove(ctx->shadows, stale);
      pipe_resource_reference(&key, NULL);
   }
   record = chrome9_shadow_entry(ctx, color, level, layer);
   if (record)
      return record;
   shadow = chrome9_resource_image_view(color, level, layer);
   if (shadow)
      goto insert;
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = u_minify(color->width0, level);
   templ.height0 = u_minify(color->height0, level);
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR;
   templ.flags = 0;
   templ.next = NULL;
   shadow = screen->resource_create(screen, &templ);
   if (!shadow)
      return NULL;
insert:
   record = CALLOC_STRUCT(chrome9_shadow);
   if (!record) {
      pipe_resource_reference(&shadow, NULL);
      return NULL;
   }
   record->shadow = shadow;
   record->direct = chrome9_resource(shadow)->parent == color;
   record->level = level;
   record->layer = layer;
   entry = _mesa_hash_table_search(ctx->shadows, color);
   if (entry) {
      record->next = entry->data;
      entry->data = record;
   } else {
      pipe_reference(NULL, &color->reference);
      _mesa_hash_table_insert(ctx->shadows, color, record);
   }
   return record;
}

static void
chrome9_set_framebuffer_state(struct pipe_context *pipe,
                              const struct pipe_framebuffer_state *framebuffer)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   /* Colour buffers 1..3 are colour targets of the same draw. */
   const struct pipe_surface *surface = framebuffer->nr_cbufs ?
                                        &framebuffer->cbufs[0] : NULL;
   struct pipe_resource *color = surface ? surface->texture : NULL;
   bool shadow_attempted = false;

   chrome9_draw_flush_batch(ctx);
   chrome9_flush_render_target(ctx);
   ctx->render_target_dirty = false;
   ctx->render_target_stale = false;
   pipe_resource_reference(&ctx->render_target, NULL);
   util_copy_framebuffer_state(&ctx->framebuffer, framebuffer);
   if (color && surface->format == color->format &&
       surface->first_layer == surface->last_layer) {
      struct chrome9_resource *resource = chrome9_resource(color);

      /* Other levels, cube faces and 3D slices render through a view of
       * the same BO, or else a shadow. */
      if (resource->guarded && color->target != PIPE_TEXTURE_CUBE &&
          color->target != PIPE_TEXTURE_3D &&
          !surface->level && !surface->first_layer) {
         pipe_resource_reference(&ctx->render_target, color);
      } else {
         struct chrome9_shadow *shadow =
            chrome9_render_shadow(ctx, color, surface->level,
                                  surface->first_layer);

         shadow_attempted = true;
         pipe_resource_reference(&ctx->render_target,
                                 shadow ? shadow->shadow : NULL);
         /* The drawable's contents are copied in only when a draw or a
          * partial clear needs them and the drawable changed since the
          * two last agreed. */
         if (ctx->render_target && !shadow->direct)
            ctx->render_target_stale = !shadow->synced ||
               shadow->synced_generation !=
               chrome9_resource(color)->cpu_write_generation;
      }
   }
   if (!framebuffer->nr_cbufs && framebuffer->zsbuf.texture &&
       chrome9_format_is_d24(framebuffer->zsbuf.format) &&
       framebuffer->zsbuf.texture->format == framebuffer->zsbuf.format &&
       !framebuffer->zsbuf.level && !framebuffer->zsbuf.first_layer &&
       !framebuffer->zsbuf.last_layer && framebuffer->width &&
       framebuffer->height &&
       framebuffer->width == framebuffer->zsbuf.texture->width0 &&
       framebuffer->height == framebuffer->zsbuf.texture->height0 &&
       chrome9_resource(framebuffer->zsbuf.texture)->guarded) {
      struct pipe_resource templ = {
         .target = PIPE_TEXTURE_2D,
         .format = PIPE_FORMAT_B8G8R8X8_UNORM,
         .width0 = framebuffer->width,
         .height0 = framebuffer->height,
         .depth0 = 1,
         .array_size = 1,
         .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
      };

      if (ctx->depth_only_scratch &&
          (ctx->depth_only_scratch->width0 != framebuffer->width ||
           ctx->depth_only_scratch->height0 != framebuffer->height))
         pipe_resource_reference(&ctx->depth_only_scratch, NULL);
      if (!ctx->depth_only_scratch)
         ctx->depth_only_scratch =
            pipe->screen->resource_create(pipe->screen, &templ);
      if (ctx->depth_only_scratch)
         pipe_resource_reference(&ctx->render_target,
                                 ctx->depth_only_scratch);
   }
   if (color && !ctx->render_target &&
       !(ctx->warnings & CHROME9_WARN_FRAMEBUFFER)) {
      struct chrome9_resource *resource = chrome9_resource(color);

      fprintf(stderr,
              "chrome9: framebuffer target rejected: cbufs=%u surface=%p "
              "surface_format=%u level=%u layers=%u..%u texture=%p "
              "target=%u format=%u size=%ux%ux%u array=%u last=%u "
              "samples=%u/%u bind=%#x flags=%#x guarded=%u bo=%u "
              "stride=%u offset=%" PRIu64 " shadow_attempted=%u\n",
              framebuffer->nr_cbufs, (void *)surface,
              surface ? surface->format : 0,
              surface ? surface->level : 0,
              surface ? surface->first_layer : 0,
              surface ? surface->last_layer : 0,
              (void *)color, color->target, color->format,
              color->width0, color->height0, color->depth0,
              color->array_size, color->last_level,
              color->nr_samples, color->nr_storage_samples,
              color->bind, color->flags, resource->guarded,
              resource->bo.handle, resource->stride,
              resource->data_offset, shadow_attempted);
      ctx->warnings |= CHROME9_WARN_FRAMEBUFFER;
   }
   draw_set_zs_format(ctx->draw, framebuffer->zsbuf.texture ?
                                framebuffer->zsbuf.format : PIPE_FORMAT_NONE);
}

static void
chrome9_set_viewport_states(struct pipe_context *pipe, unsigned start,
                            unsigned count,
                            const struct pipe_viewport_state *viewports)
{
   if (!start && count)
      chrome9_context(pipe)->viewport = viewports[0];
   draw_set_viewport_states(chrome9_context(pipe)->draw, start, count,
                            viewports);
}

static void
chrome9_set_scissor_states(struct pipe_context *pipe, unsigned start,
                           unsigned count,
                           const struct pipe_scissor_state *scissors)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (!start && count) {
      ctx->scissor = scissors[0];
      ctx->scissor_valid = true;
   }
}

static void
chrome9_set_clip_state(struct pipe_context *pipe,
                       const struct pipe_clip_state *clip)
{
   chrome9_context(pipe)->clip = *clip;
   draw_set_clip_state(chrome9_context(pipe)->draw, clip);
}

static void
chrome9_clear_target(struct chrome9_resource *target,
                     const union pipe_color_union *color,
                     unsigned x, unsigned y, unsigned width, unsigned height)
{
   union util_color packed;

   if (!target ||
       (target->base.format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        target->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        target->base.format != PIPE_FORMAT_B5G6R5_UNORM) ||
       x >= target->base.width0 || y >= target->base.height0)
      return;
   chrome9_resource_cpu_access(target, true);
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   /* A tiled target is filled through a linear copy of the rectangle. */
   if (chrome9_resource_is_tiled(target)) {
      const unsigned bytes = width *
         util_format_get_blocksize(target->base.format);
      uint8_t *copy = malloc((size_t)bytes * height);

      if (!copy)
         return;
      util_pack_color_union(target->base.format, &packed, color);
      util_fill_rect(copy, target->base.format, bytes, 0, 0, width, height,
                     &packed);
      chrome9_tiled_copy((uint8_t *)target->bo.map + target->data_offset,
                         target->stride, true,
                         x * util_format_get_blocksize(target->base.format),
                         y, bytes, height, copy, bytes, true);
      free(copy);
      return;
   }
   if (target->base.format == PIPE_FORMAT_B5G6R5_UNORM) {
      uint16_t word;

      if (!chrome9_pack_rgb565_clear(color->f, &word))
         return;
      chrome9_fill_rgb565_rect((uint8_t *)target->bo.map + target->data_offset,
                               target->stride, x, y, width, height, word);
      return;
   } else {
      util_pack_color_union(target->base.format, &packed, color);
   }
   util_fill_rect((uint8_t *)target->bo.map + target->data_offset,
                  target->base.format, target->stride, x, y, width, height,
                  &packed);
}

static void
chrome9_clear_zs_target(struct chrome9_resource *target, unsigned buffers,
                        double depth, unsigned stencil, uint8_t stencil_mask,
                        unsigned x, unsigned y, unsigned width,
                        unsigned height)
{
   uint32_t value = 0, keep = 0xffffffff;

   if (!target || x >= target->base.width0 || y >= target->base.height0)
      return;
   if (buffers & PIPE_CLEAR_DEPTH) {
      if (!chrome9_pack_depth_clear(target->base.format, depth, &value))
         return;
      /* X8Z24 has no stencil: a depth clear defines the whole word. */
      keep = target->base.format == PIPE_FORMAT_S8_UINT_Z24_UNORM ?
         0xff : 0;
   }
   if ((buffers & PIPE_CLEAR_STENCIL) &&
       target->base.format == PIPE_FORMAT_S8_UINT_Z24_UNORM) {
      keep &= ~(uint32_t)stencil_mask;
      value |= stencil & stencil_mask;
   }
   if (keep == 0xffffffff)
      return;
   chrome9_resource_cpu_access(target, true);
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   const bool tiled = chrome9_resource_is_tiled(target);
   const unsigned cpp = target->base.format == PIPE_FORMAT_Z16_UNORM ? 2 : 4;
   uint8_t *line = tiled ? malloc((size_t)width * cpp) : NULL;

   if (tiled && !line)
      return;
   for (unsigned row = 0; row < height; row++) {
      uint8_t *base = (uint8_t *)target->bo.map + target->data_offset;
      /* Tiled rows are edited in a linear copy starting at column x. */
      uint8_t *pixels = tiled ? line - (size_t)x * cpp :
                        base + (uint64_t)(y + row) * target->stride;

      if (tiled)
         chrome9_tiled_copy(base, target->stride, true, x * cpp, y + row,
                            width * cpp, 1, line, width * cpp, false);
      if (target->base.format == PIPE_FORMAT_Z16_UNORM) {
         uint16_t *z16 = (uint16_t *)pixels + x;

         for (unsigned column = 0; column < width; column++)
            z16[column] = (uint16_t)value;
      } else {
         uint32_t *d24 = (uint32_t *)pixels + x;

         for (unsigned column = 0; column < width; column++)
            d24[column] = (d24[column] & keep) | value;
      }
      if (tiled)
         chrome9_tiled_copy(base, target->stride, true, x * cpp, y + row,
                            width * cpp, 1, line, width * cpp, true);
   }
   free(line);
}

/* Per-channel masked clear of a 32-bpp BGRA/BGRX target. */
static void
chrome9_clear_target_masked(struct chrome9_resource *target,
                            const union pipe_color_union *color,
                            unsigned color_mask, unsigned x, unsigned y,
                            unsigned width, unsigned height)
{
   union util_color packed;
   uint32_t keep = 0;

   if (!target || x >= target->base.width0 || y >= target->base.height0 ||
       (target->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        target->base.format != PIPE_FORMAT_B8G8R8X8_UNORM))
      return;
   util_pack_color_union(target->base.format, &packed, color);
   /* BGRA8888 stores R in bits 23:16, G 15:8, B 7:0 and A 31:24. */
   if (!(color_mask & PIPE_MASK_R))
      keep |= 0x00ff0000;
   if (!(color_mask & PIPE_MASK_G))
      keep |= 0x0000ff00;
   if (!(color_mask & PIPE_MASK_B))
      keep |= 0x000000ff;
   if (!(color_mask & PIPE_MASK_A))
      keep |= 0xff000000;
   chrome9_resource_cpu_access(target, true);
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   const bool tiled = chrome9_resource_is_tiled(target);
   uint32_t *line = tiled ? malloc((size_t)width * 4) : NULL;

   if (tiled && !line)
      return;
   for (unsigned row = 0; row < height; row++) {
      uint8_t *base = (uint8_t *)target->bo.map + target->data_offset;
      uint32_t *pixels = tiled ? line :
         (uint32_t *)(base + (uint64_t)(y + row) * target->stride) + x;

      if (tiled)
         chrome9_tiled_copy(base, target->stride, true, x * 4, y + row,
                            width * 4, 1, (uint8_t *)line, width * 4, false);
      for (unsigned column = 0; column < width; column++)
         pixels[column] = (pixels[column] & keep) | (packed.ui[0] & ~keep);
      if (tiled)
         chrome9_tiled_copy(base, target->stride, true, x * 4, y + row,
                            width * 4, 1, (uint8_t *)line, width * 4, true);
   }
   free(line);
}

/* An X8Z24 depth surface Fast-Z may clear to depth: not sampled, one level. */
static bool
chrome9_fast_z_surface(const struct chrome9_context *ctx,
                       const struct chrome9_resource *zs, double depth)
{
   return ctx->fast_z && zs && depth == 1.0 && zs->guarded &&
          zs->base.format == PIPE_FORMAT_X8Z24_UNORM &&
          !zs->base.last_level && !zs->parent && !zs->fast_z_disabled;
}

/*
 * Full-surface clears as one DRAW_FRAGMENT_PROGRAM rectangle, so the CPU
 * neither writes the surfaces nor forces whole-surface cache flushes: the
 * program writes CB constant 0, depth runs ALWAYS with writes and stencil
 * REPLACEs.  Scissored, masked or inexact clears stay on the CPU.
 */
static bool
chrome9_clear_gpu(struct chrome9_context *ctx, unsigned buffers,
                  uint32_t color_mask, uint8_t stencil_mask,
                  const struct pipe_scissor_state *scissor,
                  const union pipe_color_union *color, double depth,
                  unsigned stencil)
{
   const struct pipe_framebuffer_state *fb = &ctx->framebuffer;
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   const bool clear_color = buffers & PIPE_CLEAR_COLOR0;
   const bool clear_depth = buffers & PIPE_CLEAR_DEPTH;
   const bool clear_stencil = buffers & PIPE_CLEAR_STENCIL;
   struct chrome9_resource *target = ctx->render_target ?
      chrome9_resource(ctx->render_target) : NULL;
   struct chrome9_resource *zs = fb->zsbuf.texture ?
      chrome9_resource(fb->zsbuf.texture) : NULL;
   struct drm_chrome9_fragment_instruction mov = {
      .op = DRM_CHROME9_FRAGMENT_OP_MOV,
      .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
      .src = { { DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0, { 0, 1, 2, 3 },
                 0, 0 } },
   };
   struct drm_chrome9_draw_fragment_program draw = { 0 };
   struct drm_chrome9_fragment_vertex vertices[6];
   static const unsigned corner[6][2] = {
      { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 },
   };
   uint32_t constant[4];
   int32_t z = 0;

   /* A mipmapped target is drawn at level zero; other levels render
    * through a view or a shadow. */
   if (!ctx->draw_fragment_program || !target || !target->guarded ||
       (target->base.last_level &&
        (!fb->nr_cbufs || fb->cbufs[0].level ||
         &target->base != fb->cbufs[0].texture ||
         !chrome9_screen_has_capability(
            ctx->base.screen,
            DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED))) ||
       !fb->width || !fb->height ||
       target->base.width0 != fb->width || target->base.height0 != fb->height ||
       (target->base.format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        target->base.format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       (scissor && (scissor->minx || scissor->miny ||
                    scissor->maxx < fb->width || scissor->maxy < fb->height)) ||
       (clear_color && ((color_mask & 0xf) != 0xf || !fb->nr_cbufs)) ||
       (clear_stencil && stencil_mask != 0xff))
      return false;
   /* Fast-Z clear: advance the index instead of writing every depth. */
   if (clear_depth && !clear_stencil && zs &&
       zs->base.width0 == fb->width && zs->base.height0 == fb->height &&
       chrome9_fast_z_surface(ctx, zs, depth) && zs->fast_z_index &&
       zs->fast_z_index < CHROME9_FAST_Z_LAST_INDEX) {
      zs->fast_z_index++;
      if (ctx->coarse_z)
         chrome9_resource_coarse_z_fill(&ctx->base, zs);
      return !clear_color ||
             chrome9_clear_gpu(ctx, buffers & ~PIPE_CLEAR_DEPTH, color_mask,
                               stencil_mask, scissor, color, depth, stencil);
   }
   if (clear_depth || clear_stencil) {
      const bool d24 = zs && chrome9_format_is_d24(zs->base.format);

      if (!zs || !zs->guarded || zs->base.last_level ||
          zs->base.width0 != fb->width || zs->base.height0 != fb->height ||
          (!d24 && zs->base.format != PIPE_FORMAT_Z16_UNORM) ||
          (clear_stencil &&
           (zs->base.format != PIPE_FORMAT_S8_UINT_Z24_UNORM ||
            !ctx->fragment_stencil)) ||
          (!clear_depth && (!ctx->depth_read_only_supported ||
                            !ctx->depth_always_supported)) ||
          depth < 0.0 || depth > 1.0)
         return false;
      /* Window z is 16.16: a D24 value must be exact in 16 bits. */
      z = (int32_t)(depth * 65536.0 + 0.5);
      if (clear_depth && d24 && fabs(z / 65536.0 - depth) > 1e-9)
         return false;
      draw.depth_handle = zs->bo.handle;
      if (zs->tiled)
         draw.flags |= DRM_CHROME9_FRAGMENT_DEPTH_TILED;
      draw.depth_pitch = zs->stride;
      draw.depth_format = d24 ? DRM_CHROME9_FRAGMENT_DEPTH_X8Z24 :
                                DRM_CHROME9_FRAGMENT_DEPTH_D16;
      draw.flags |= DRM_CHROME9_FRAGMENT_DEPTH |
                    DRM_CHROME9_COMPARE_ALWAYS << DRM_CHROME9_DEPTH_FUNC_SHIFT |
                    (clear_depth ? 0 : DRM_CHROME9_DEPTH_READ_ONLY);
      if (clear_stencil) {
         struct drm_chrome9_stencil_face face = {
            .func = DRM_CHROME9_COMPARE_ALWAYS,
            .fail_op = DRM_CHROME9_STENCIL_OP_REPLACE,
            .zfail_op = DRM_CHROME9_STENCIL_OP_REPLACE,
            .zpass_op = DRM_CHROME9_STENCIL_OP_REPLACE,
            .ref = stencil & 0xff,
            .value_mask = 0xff,
            .write_mask = 0xff,
         };

         draw.flags |= DRM_CHROME9_FRAGMENT_STENCIL;
         draw.stencil.cw = face;
         draw.stencil.ccw = face;
      }
   }
   /* Keep the color store off unless color is cleared. */
   draw.flags |= DRM_CHROME9_FRAGMENT_BLEND;
   draw.blend.rgb_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE;
   draw.blend.rgb_dst_factor = DRM_CHROME9_BLEND_FACTOR_ZERO;
   draw.blend.alpha_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE;
   draw.blend.alpha_dst_factor = DRM_CHROME9_BLEND_FACTOR_ZERO;
   draw.blend.color_mask = clear_color ?
      DRM_CHROME9_COLOR_MASK_R | DRM_CHROME9_COLOR_MASK_G |
      DRM_CHROME9_COLOR_MASK_B | DRM_CHROME9_COLOR_MASK_A : 0;
   for (unsigned c = 0; c < 4; c++)
      memcpy(&constant[c], &color->f[c], sizeof(constant[c]));
   memset(vertices, 0, sizeof(vertices));
   for (unsigned v = 0; v < 6; v++) {
      vertices[v].x_16_16 = (int32_t)(corner[v][0] * fb->width << 16) - 0x8000;
      vertices[v].y_16_16 = (int32_t)(corner[v][1] * fb->height << 16) - 0x8000;
      vertices[v].z_16_16 = z;
      vertices[v].rhw_float = 0x3f800000;
   }
   if (chrome9_resource_extended(target))
      draw.flags |= DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED;
   draw.destination_offset = chrome9_resource_destination_offset(target);
   draw.abi_version = DRM_CHROME9_FRAGMENT_ABI_VERSION;
   draw.destination_handle = target->bo.handle;
   if (chrome9_resource_is_tiled(target))
      draw.flags |= DRM_CHROME9_FRAGMENT_DESTINATION_TILED;
   draw.width = target->base.width0;
   draw.height = target->base.height0;
   draw.destination_pitch = target->stride;
   draw.destination_format =
      target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
      DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
      DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
   draw.instruction_count = 1;
   draw.instructions_ptr = (uintptr_t)&mov;
   draw.constant_count = 1;
   draw.constants_ptr = (uintptr_t)constant;
   draw.triangle_count = 2;
   draw.vertices_ptr = (uintptr_t)vertices;
   if (clear_color) {
      ctx->render_target_stale = false;
   } else {
      /* The color store is masked off, but the target stays bound. */
      chrome9_context_prepare_render_target(ctx);
   }
   if (!chrome9_draw_fragment_program(fd, &draw))
      return false;
   if (clear_color)
      chrome9_context_render_target_written(ctx);
   /* The written 1.0 carries index 7: stale for the indices 1..6. */
   if (clear_depth && chrome9_fast_z_surface(ctx, zs, depth))
      zs->fast_z_index = 1;
   /* A full clear to 1.0 makes every coarse-Z entry all ones. */
   if (clear_depth && ctx->coarse_z && depth == 1.0)
      chrome9_resource_coarse_z_fill(&ctx->base, zs);
   else if (clear_depth)
      zs->coarse_z_valid = false;
   return true;
}

static void
chrome9_clear(struct pipe_context *pipe, unsigned buffers,
              uint32_t color_mask, uint8_t stencil_mask,
              const struct pipe_scissor_state *scissor,
              const union pipe_color_union *color, double depth,
              unsigned stencil)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct pipe_resource *resource;
   unsigned x = 0, y = 0, width, height;

   if (!chrome9_render_condition_passes(ctx))
      return;
   chrome9_draw_flush_batch(ctx);
   /* Further color buffers (GL_FRONT_AND_BACK) are cleared through their
    * mappings; the pixel engine writes one target. */
   for (unsigned i = 1; i < ctx->framebuffer.nr_cbufs; i++) {
      struct pipe_surface *cbuf = &ctx->framebuffer.cbufs[i];
      unsigned cx = 0, cy = 0;
      unsigned cw = ctx->framebuffer.width, ch = ctx->framebuffer.height;

      if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !cbuf->texture)
         continue;
      if (scissor) {
         cx = MIN2(scissor->minx, cw);
         cy = MIN2(scissor->miny, ch);
         cw = MIN2(scissor->maxx, cw) - cx;
         ch = MIN2(scissor->maxy, ch) - cy;
      }
      if (((color_mask >> (4 * i)) & 0xf) == 0xf)
         util_clear_render_target(pipe, cbuf, color, cx, cy, cw, ch);
      else
         chrome9_clear_surface_masked(pipe, cbuf, color,
                                      (color_mask >> (4 * i)) & 0xf,
                                      cx, cy, cw, ch);
   }
   buffers &= PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTHSTENCIL;
   if (!buffers)
      return;
   if (debug_get_bool_option("CHROME9_GPU_CLEAR", true) &&
       chrome9_clear_gpu(ctx, buffers, color_mask, stencil_mask, scissor,
                         color, depth, stencil))
      return;
   width = ctx->framebuffer.width;
   height = ctx->framebuffer.height;
   if (scissor) {
      x = MIN2(scissor->minx, width);
      y = MIN2(scissor->miny, height);
      width = MIN2(scissor->maxx, width) - x;
      height = MIN2(scissor->maxy, height) - y;
   }
   if ((buffers & PIPE_CLEAR_COLOR0) && ctx->framebuffer.nr_cbufs &&
       (resource = ctx->render_target)) {
      /* A clear of every pixel needs no copy of the drawable's contents. */
      if ((color_mask & 0xf) == 0xf && !x && !y &&
          width >= resource->width0 && height >= resource->height0)
         ctx->render_target_stale = false;
      chrome9_context_prepare_render_target(ctx);
      if ((color_mask & 0xf) == 0xf)
         chrome9_clear_target(chrome9_resource(resource), color, x, y, width,
                              height);
      else
         chrome9_clear_target_masked(chrome9_resource(resource), color,
                                     color_mask, x, y, width, height);
      chrome9_context_render_target_written(ctx);
   }
   if (buffers & PIPE_CLEAR_DEPTHSTENCIL)
      chrome9_clear_zs_target(chrome9_context_depth_target(ctx), buffers,
                              depth, stencil, stencil_mask,
                              x, y, width, height);
}

static void
chrome9_clear_depth_stencil(struct pipe_context *pipe,
                            struct pipe_surface *surface,
                            unsigned clear_flags, double depth,
                            unsigned stencil, unsigned x, unsigned y,
                            unsigned width, unsigned height,
                            bool render_condition)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (render_condition && !chrome9_render_condition_passes(ctx))
      return;
   chrome9_draw_flush_batch(ctx);
   if (!surface || !surface->texture ||
       !(clear_flags & PIPE_CLEAR_DEPTHSTENCIL)) {
      chrome9_warn_once(ctx, CHROME9_WARN_CLEAR,
                        "unsupported depth/stencil clear");
      return;
   }
   chrome9_clear_zs_target(chrome9_resource(surface->texture), clear_flags,
                           depth, stencil, 0xff, x, y, width, height);
}

static void
chrome9_clear_render_target(struct pipe_context *pipe,
                            struct pipe_surface *surface,
                            const union pipe_color_union *color,
                            unsigned x, unsigned y, unsigned width,
                            unsigned height, bool render_condition)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct pipe_resource *target;

   if (render_condition && !chrome9_render_condition_passes(ctx))
      return;
   chrome9_draw_flush_batch(ctx);
   if (!surface || !surface->texture)
      return;
   target = surface->texture;
   if (ctx->framebuffer.nr_cbufs &&
       surface->texture == ctx->framebuffer.cbufs[0].texture &&
       ctx->render_target)
      target = ctx->render_target;
   if (target == ctx->render_target)
      chrome9_context_prepare_render_target(ctx);
   chrome9_clear_target(chrome9_resource(target), color,
                        x, y, width, height);
   if (target == ctx->render_target) {
      chrome9_context_render_target_written(ctx);
      chrome9_flush_render_target(ctx);
   }
}

static void
chrome9_refresh_sampler_shadows(struct chrome9_context *ctx)
{
   for (unsigned i = 0; i < ctx->num_sampler_views; i++) {
      struct pipe_sampler_view *view = ctx->sampler_views[i];
      struct chrome9_sampler_view *cview;

      if (!view)
         continue;
      cview = chrome9_sampler_view(view);
      if (cview->shadow) {
         /* A texture may be the result of an earlier untextured pass.  Retire
          * retained triangles before refreshing its CPU-visible shadow. */
         chrome9_draw_flush_batch(ctx);
         chrome9_copy_surface(cview->shadow, view->texture);
      }
   }
}

static void
chrome9_refresh_vs_textures(struct chrome9_context *ctx)
{
   static const uint8_t zero_texel[16];
   uint32_t zero_stride[PIPE_MAX_TEXTURE_LEVELS] = {0};
   struct pipe_resource *external = ctx->framebuffer.nr_cbufs ?
      ctx->framebuffer.cbufs[0].texture : NULL;

   if (!ctx->num_vs_sampler_views)
      return;
   /* Vertex fetch happens on the CPU.  Retire any preceding GPU writes
    * before publishing the BO's persistent mapping to the draw module. */
   chrome9_draw_flush_batch(ctx);
   for (unsigned i = 0; i < ctx->num_vs_sampler_views; i++) {
      struct pipe_sampler_view *view = ctx->vs_sampler_views[i];
      struct chrome9_vs_texture_mapping *mapping =
         &ctx->vs_texture_mappings[i];
      struct pipe_sampler_view effective;

      if (view && view->texture == external)
         chrome9_flush_render_target(ctx);
      if (view && chrome9_resource_is_local(chrome9_resource(view->texture)))
         chrome9_context_demote_local(ctx, view->texture);
      if (view && chrome9_sampler_view(view)->shadow) {
         chrome9_copy_surface(chrome9_sampler_view(view)->shadow,
                              view->texture);
         effective = *view;
         effective.texture = chrome9_sampler_view(view)->shadow;
         view = &effective;
      }
      if (chrome9_vs_texture_mapping(view, mapping)) {
         /* The draw module samples the pages on the CPU. */
         chrome9_resource_cpu_access(chrome9_resource(view->texture), false);
         draw_set_mapped_texture(ctx->draw, MESA_SHADER_VERTEX, i,
                                 mapping->width, mapping->height, 1,
                                 mapping->first_level, mapping->last_level,
                                 0, 0, mapping->base, mapping->row_stride,
                                 mapping->img_stride, mapping->mip_offset);
      } else {
         draw_set_mapped_texture(ctx->draw, MESA_SHADER_VERTEX, i,
                                 1, 1, 1, 0, 0, 0, 0, zero_texel,
                                 zero_stride, zero_stride, zero_stride);
      }
   }
}

static unsigned
chrome9_framebuffer_color_count(const struct pipe_framebuffer_state *fb)
{
   unsigned count = 0;

   for (unsigned i = 0; i < fb->nr_cbufs; i++)
      count += fb->cbufs[i].texture != NULL;
   return count;
}


/* Non-multisample smooth lines, also from polygon mode LINE: draw would
 * hand them to the vbuf render unwidened, which draws triangles only. */
static bool
chrome9_draw_smooth_lines(const struct chrome9_context *ctx,
                          const struct pipe_draw_info *info)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   const enum mesa_prim prim = u_reduced_prim(info->mode);

   if (!rs || !rs->line_smooth || rs->multisample)
      return false;
   /* One-pixel smooth lines use TCL edge antialiasing. */
   if (prim == MESA_PRIM_LINES && ctx->tcl_edge_aa && ctx->tcl_lines &&
       !rs->line_stipple_enable && rs->line_width < 1.5f)
      return false;
   return prim == MESA_PRIM_LINES ||
          (prim == MESA_PRIM_TRIANGLES &&
           (rs->fill_front == PIPE_POLYGON_MODE_LINE ||
            rs->fill_back == PIPE_POLYGON_MODE_LINE));
}
/* Lower-left sprite coordinates on point rasterization: the draw module
 * gets the upper-left copy of the rasterizer like the hardware, the FS
 * variant flips t. */
static void
chrome9_update_sprite_flip(struct chrome9_context *ctx,
                           const struct pipe_draw_info *info)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   const struct pipe_rasterizer_state *draw_rs = rs;
   bool points;

   ctx->fs_sprite_flip = 0;
   if (!rs)
      return;
   points = info->mode == MESA_PRIM_POINTS ||
            (u_reduced_prim(info->mode) == MESA_PRIM_TRIANGLES &&
             rs->fill_front == PIPE_POLYGON_MODE_POINT &&
             rs->fill_back == PIPE_POLYGON_MODE_POINT);
   if (points && rs->point_quad_rasterization &&
       rs->sprite_coord_mode == PIPE_SPRITE_COORD_LOWER_LEFT) {
      ctx->fs_sprite_flip = rs->sprite_coord_enable | BITFIELD_BIT(31);
      draw_rs = rs + 1;
   }
   if (ctx->draw_rasterizer != draw_rs) {
      draw_set_rasterizer_state(ctx->draw, draw_rs, ctx->rasterizer);
      ctx->draw_rasterizer = draw_rs;
   }
}

static bool
chrome9_fs_kills(const struct chrome9_shader *fs)
{
   for (unsigned i = 0; i < fs->semantic_fs_ir.count; i++)
      if (fs->semantic_fs_ir.instruction[i].op == CHROME9_PS_OPCODE_KILL)
         return true;
   return false;
}

static void
chrome9_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   const void *indices = NULL;

   /* Imported drawable contents may change after a view is created or
    * bound.  Refresh immediately before every Gallium draw so hardware never
    * samples a create-time/set-time snapshot. */
   chrome9_refresh_sampler_shadows(ctx);

   if (num_draws > 1) {
      util_draw_multi(pipe, info, drawid_offset, indirect, draws, num_draws);
      return;
   }
   if (!num_draws || !draws[0].count || !info->instance_count)
      return;
   if (indirect) {
      chrome9_warn_once(ctx, CHROME9_WARN_INDIRECT,
                        "indirect draws are not implemented");
      return;
   }
   if (!chrome9_render_condition_passes(ctx))
      return;
   chrome9_update_sprite_flip(ctx, info);
   if (!ctx->vs || !ctx->fs || !chrome9_select_fs_variant(ctx)) {
      /* Beyond the pixel engine: llvmpipe renders into the same BOs. */
      if (chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
         return;
      chrome9_warn_once(ctx, CHROME9_WARN_DRAW_STATE,
                        "draw rejected: unsupported bounded fragment program");
      CHROME9_REJECT(ctx, draws[0].count);
      return;
   }
   /* Only generic fragment programs carry the query tag, and only with
    * the hardware counter, which also counts the pixels a KILL discards
    * (occlusion-query-discard); 3D textures outside the sampler's contract
    * need llvmpipe.  llvmpipe's query counts. */
   const bool counting = chrome9_query_counting(ctx);
   const bool counted = ctx->occlusion_query &&
                        chrome9_generic_fragment_program(ctx) &&
                        !(counting && chrome9_fs_kills(ctx->fs));

   if ((counting && !counted) ||
       chrome9_draw_needs_sw_textures(ctx)) {
      if (chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
         return;
      if (!counted)
         chrome9_query_mark_inexact(ctx);
   }

   /* Interpreted vertex texturing is slower than llvmpipe; JIT-compiled
    * vertex shaders keep the fragments on the hardware. */
   if (!ctx->draw_llvm && ctx->vs->sw_nir &&
       BITSET_COUNT(ctx->vs->sw_nir->info.textures_used) &&
       chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
      return;
   /* The pixel engine writes exactly one colour target and blends without
    * logic ops or polygon stipple, and draw has no AA line stage: other
    * states render in llvmpipe. */
   /* Depth-only passes draw into the masked D24 scratch target. */
   if (((ctx->framebuffer.nr_cbufs ? !ctx->framebuffer.cbufs[0].texture :
         !ctx->render_target ||
         ctx->render_target != ctx->depth_only_scratch ||
         !chrome9_draw_stencil_supported(ctx)) ||
        /* Several colour buffers only as hardware TCL colour targets. */
        (chrome9_framebuffer_color_count(&ctx->framebuffer) > 1 &&
         (!ctx->tcl_enabled ||
          !chrome9_screen_has_capability2(ctx->base.screen,
                                          DRM_CHROME9_CAP2_COLOR_TARGETS))) ||
        /* The kernel binds single-level depth objects only. */
        (ctx->framebuffer.zsbuf.texture &&
         ctx->framebuffer.zsbuf.texture->last_level) ||
        /* and targets sharing their object only in fragment programs. */
        (ctx->render_target &&
         chrome9_resource_extended(chrome9_resource(ctx->render_target)) &&
         !chrome9_generic_fragment_program(ctx)) ||
        !chrome9_draw_blend_supported(ctx) ||
        (ctx->rasterizer && ctx->rasterizer->poly_stipple_enable) ||
        (!chrome9_generic_fragment_program(ctx) &&
         chrome9_draw_tiled_surfaces(ctx)) ||
        chrome9_draw_smooth_lines(ctx, info)) &&
       chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
      return;
   chrome9_refresh_vs_textures(ctx);
   /* No GPU job may start while llvmpipe still accesses a BO. */
   chrome9_sw_finish(ctx);
   if (chrome9_tcl_draw(ctx, info, &draws[0]))
      return;
   /* The draw module writes one colour buffer. */
   if (chrome9_framebuffer_color_count(&ctx->framebuffer) > 1) {
      if (!chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
         CHROME9_REJECT(ctx, draws[0].count);
      return;
   }
   /* Smooth lines TCL declined: the draw module cannot widen them. */
   if (ctx->rasterizer && ctx->rasterizer->line_smooth &&
       u_reduced_prim(info->mode) == MESA_PRIM_LINES &&
       chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
      return;

   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];
      const void *map = vb->is_user_buffer ? vb->buffer.user :
                        vb->buffer.resource ?
                        chrome9_resource(vb->buffer.resource)->bo.map : NULL;
      size_t size = vb->is_user_buffer || !vb->buffer.resource ?
                    ~(size_t)0 : vb->buffer.resource->width0;

      draw_set_mapped_vertex_buffer(ctx->draw, i, map, size);
   }
   if (info->index_size) {
      indices = info->has_user_indices ? info->index.user :
                info->index.resource ?
                chrome9_resource(info->index.resource)->bo.map : NULL;
      if (!indices)
         goto unmap;
      draw_set_indexes(ctx->draw, indices, info->index_size,
                       info->has_user_indices ? ~(size_t)0 :
                       info->index.resource->width0);
   }
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct pipe_constant_buffer *cb = &ctx->vs_constants[i];
      const uint8_t *map = cb->user_buffer;

      if (!map && cb->buffer)
         map = chrome9_resource(cb->buffer)->bo.map;
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, i,
                                      map ? map + cb->buffer_offset : NULL,
                                      map ? cb->buffer_size : 0);
   }
   const bool guarded_domain =
      ctx->fs->semantic_equal_zero_input_mask != 0 ||
      ctx->fs->semantic_identity_projector_input_mask != 0 ||
      ctx->fs->semantic_unit_interval_input_mask != 0;

   const uint64_t rejected = ctx->rejected_triangles;

   if (guarded_domain)
      chrome9_draw_begin_guarded_domain(ctx);
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(ctx->draw);
   /* A varying outside the program's proven domain (e.g. a projector that
    * is not 1): llvmpipe renders the whole draw instead. */
   if (guarded_domain && !chrome9_draw_end_guarded_domain(ctx) &&
       chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
      ctx->rejected_triangles = rejected;
   /* Keep compatible immediate triangles across Gallium draw calls.  The
    * flush/present boundary retires them and publishes a guarded drawable
    * shadow once rather than copying a full image after every draw. */

unmap:
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++)
      draw_set_mapped_vertex_buffer(ctx->draw, i, NULL, 0);
   if (info->index_size)
      draw_set_indexes(ctx->draw, NULL, 0, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++)
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, i,
                                      NULL, 0);
}

static void
chrome9_flush(struct pipe_context *pipe, struct pipe_fence_handle **fence,
              unsigned flags)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   chrome9_draw_flush_batch(ctx);
   chrome9_flush_render_target(ctx);
   chrome9_report_rejects(ctx);
   if (ctx->tcl_enabled && !(++ctx->tcl_flushes % 16) &&
       debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL triangles=%" PRIu64 " (buffers %" PRIu64
                      ", rejects %" PRIu64 ") fallback draws=%" PRIu64
                      " all triangles=%" PRIu64 "\n",
              ctx->tcl_triangles, ctx->tcl_buffer_triangles,
              ctx->tcl_buffer_rejects, ctx->tcl_fallback_draws,
              ctx->submitted_triangles);
   (void)flags;
   if (fence) {
      struct chrome9_fence *f = CALLOC_STRUCT(chrome9_fence);

      *fence = NULL;
      if (f) {
         pipe_reference_init(&f->reference, 1);
         /* A sequence number covers exactly the jobs flushed so far;
          * waiting for the target to idle would also wait for later
          * frames and serialise CPU and GPU. */
         if (chrome9_screen_has_capability2(pipe->screen,
                                            DRM_CHROME9_CAP2_FENCE_SEQNO))
            f->has_seqno = chrome9_fence_last(
               pipe->screen->get_screen_fd(pipe->screen), &f->seqno);
         if (!f->has_seqno)
            pipe_resource_reference(&f->resource, ctx->last_target);
         *fence = (struct pipe_fence_handle *)f;
      }
   }
}

static void
chrome9_flush_resource(struct pipe_context *pipe,
                       struct pipe_resource *resource)
{
   (void)resource;
   /*
    * Rendering is synchronous, but an imported DRI3 render target may use a
    * guarded shadow.  Presenting it still requires the shadow copy-back.
    */
   chrome9_draw_flush_batch(chrome9_context(pipe));
   chrome9_flush_render_target(chrome9_context(pipe));
}

/* No stream output: the blitter still calls this around its draws. */
static void
chrome9_set_stream_output_targets(struct pipe_context *pipe, unsigned count,
                                  struct pipe_stream_output_target **targets,
                                  const unsigned *offsets,
                                  enum mesa_prim output_prim)
{
   (void)pipe;
   (void)count;
   (void)targets;
   (void)offsets;
   (void)output_prim;
}

static void
chrome9_set_blend_color(struct pipe_context *pipe,
                        const struct pipe_blend_color *color)
{
   chrome9_context(pipe)->blend_color = *color;
}

static void
chrome9_set_stencil_ref(struct pipe_context *pipe,
                        const struct pipe_stencil_ref reference)
{
   chrome9_context(pipe)->stencil_ref = reference;
}

static void
chrome9_set_sample_mask(struct pipe_context *pipe, unsigned mask)
{
   (void)pipe;
   (void)mask;
}

static void
chrome9_set_polygon_stipple(struct pipe_context *pipe,
                            const struct pipe_poly_stipple *stipple)
{
   chrome9_context(pipe)->poly_stipple = *stipple;
}

static void
chrome9_context_destroy(struct pipe_context *pipe)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   chrome9_draw_flush_batch(ctx);
   chrome9_flush_render_target(ctx);
   chrome9_report_rejects(ctx);
   if (ctx->tcl_enabled && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL triangles=%" PRIu64 " (buffers %" PRIu64
                      ", rejects %" PRIu64 ") fallback draws=%" PRIu64
                      " all triangles=%" PRIu64 "\n",
              ctx->tcl_triangles, ctx->tcl_buffer_triangles,
              ctx->tcl_buffer_rejects, ctx->tcl_fallback_draws,
              ctx->submitted_triangles);
   chrome9_tcl_batch_destroy(ctx);
   chrome9_query_context_destroy(ctx);
   chrome9_sw_context_destroy(ctx);
   if (ctx->reject_sites)
      _mesa_hash_table_destroy(ctx->reject_sites, NULL);
   ctx->reject_sites = NULL;
   if (ctx->draw)
      draw_destroy(ctx->draw);
   if (pipe->stream_uploader)
      u_upload_destroy(pipe->stream_uploader);
   util_unreference_framebuffer_state(&ctx->framebuffer);
   pipe_resource_reference(&ctx->render_target, NULL);
   pipe_resource_reference(&ctx->depth_only_scratch, NULL);
   pipe_resource_reference(&ctx->last_target, NULL);
   if (ctx->shadows) {
      hash_table_foreach(ctx->shadows, entry) {
         struct pipe_resource *key = (struct pipe_resource *)entry->key;

         chrome9_shadow_list_free(entry->data);
         pipe_resource_reference(&key, NULL);
      }
      _mesa_hash_table_destroy(ctx->shadows, NULL);
   }
   util_set_vertex_buffers_count(ctx->vertex_buffers,
                                 &ctx->num_vertex_buffers, NULL, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      util_copy_constant_buffer(&ctx->vs_constants[i], NULL);
      util_copy_constant_buffer(&ctx->fs_constants[i], NULL);
   }
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++) {
      pipe_sampler_view_reference(&ctx->vs_sampler_views[i], NULL);
      chrome9_vs_texture_mapping_release(&ctx->vs_texture_mappings[i]);
   }
   chrome9_vs_texture_sampler_destroy(ctx->vs_tgsi_sampler);
   chrome9_tcl_state_destroy(ctx->tcl_state);
   FREE(ctx);
}

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags)
{
   struct chrome9_context *ctx = CALLOC_STRUCT(chrome9_context);
   struct draw_stage *stage;
   uint64_t capabilities;

   (void)flags;
   if (!ctx)
      return NULL;
   if (!chrome9_get_param(screen->get_screen_fd(screen),
                          DRM_CHROME9_PARAM_CAPABILITIES, &capabilities) ||
       !(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE))
      goto fail;
   ctx->draw_triangle_d16 =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16);
   ctx->draw_triangle_d16_blend =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND);
   ctx->depth_func_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_FUNC);
   ctx->depth_equal_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_EQUAL);
   ctx->depth_always_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_ALWAYS);
   ctx->depth_read_only_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_READ_ONLY);
   ctx->draw_triangle_batch =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_BATCH);
   ctx->draw_triangle_d16_rhw_batch =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_BATCH);
   ctx->draw_triangle_d16_rhw_run =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_RUN) ||
      debug_get_bool_option("CHROME9_FORCE_RHW_RUN", false);
   ctx->draw_triangle_d16_rhw_chain =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_CHAIN);
   ctx->draw_program_triangle_d16_rhw_batch =
      !!(capabilities &
         DRM_CHROME9_CAP_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH);
   ctx->draw_fragment_program =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM);
   ctx->fragment_sparse_textures =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_SPARSE_TEXTURES);
   ctx->tcl_enabled = (capabilities & DRM_CHROME9_CAP_VERTEX_PROGRAM) &&
      debug_get_bool_option("CHROME9_TCL", false);
   ctx->tcl_batching = ctx->tcl_enabled &&
      (capabilities & DRM_CHROME9_CAP_VERTEX_BATCH) &&
      debug_get_bool_option("CHROME9_TCL_BATCH", true);
   ctx->tcl_buffers = ctx->tcl_enabled &&
      (capabilities & DRM_CHROME9_CAP_VERTEX_BUFFERS) &&
      debug_get_bool_option("CHROME9_TCL_BUFFERS", true);
   ctx->tcl_dedupe = ctx->tcl_buffers &&
      debug_get_bool_option("CHROME9_TCL_DEDUPE", true);
   ctx->tcl_edge_aa = ctx->tcl_enabled &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_EDGE_AA) &&
      debug_get_bool_option("CHROME9_TCL_SMOOTH_LINES", true);
   ctx->depth_output = ctx->draw_fragment_program &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_DEPTH_OUTPUT) &&
      debug_get_bool_option("CHROME9_DEPTH_OUTPUT", true);
   ctx->fast_z = ctx->draw_fragment_program &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_FAST_Z) &&
      debug_get_bool_option("CHROME9_FAST_Z", true);
   ctx->coarse_z = ctx->draw_fragment_program &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_COARSE_Z) &&
      debug_get_bool_option("CHROME9_COARSE_Z", true);
   ctx->tcl_twoside = ctx->tcl_enabled &&
      (capabilities & DRM_CHROME9_CAP_VERTEX_TWO_SIDE);
   ctx->tcl_clip_distance = ctx->tcl_enabled &&
      (capabilities & DRM_CHROME9_CAP_VERTEX_CLIP_DISTANCE);
   ctx->tcl_topology = ctx->tcl_enabled &&
      chrome9_screen_has_capability2(screen,
                                     DRM_CHROME9_CAP2_VERTEX_TOPOLOGY) &&
      debug_get_bool_option("CHROME9_TCL_TOPOLOGY", true);
   /* Rungs points, pedge, sprite, lines, wire, polymode, psize and the GL
    * smoke pass. */
   ctx->tcl_points = ctx->tcl_enabled &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_NATIVE_POINTS) &&
      debug_get_bool_option("CHROME9_TCL_POINTS", true);
   ctx->tcl_point_size = ctx->tcl_points &&
      chrome9_screen_has_capability2(screen,
                                     DRM_CHROME9_CAP2_POINT_SIZE_VERTEX) &&
      debug_get_bool_option("CHROME9_TCL_POINT_SIZE", true);
   ctx->tcl_lines = ctx->tcl_enabled &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_NATIVE_LINES) &&
      debug_get_bool_option("CHROME9_TCL_LINES", true);
   ctx->tcl_polygon_mode = ctx->tcl_enabled &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_POLYGON_MODE) &&
      debug_get_bool_option("CHROME9_TCL_POLYGON_MODE", true);
   /* The kernel reports the IF nesting depth; CHROME9_TCL_FLOW_DEPTH may
    * lower it. */
   if (ctx->tcl_enabled &&
       chrome9_screen_has_capability2(screen,
                                      DRM_CHROME9_CAP2_VERTEX_STATIC_FLOW) &&
       debug_get_bool_option("CHROME9_TCL_FLOW", true)) {
      const unsigned depth = MAX2(chrome9_screen_vs_flow_depth(screen), 1);

      ctx->tcl_flow.max_depth =
         CLAMP(debug_get_num_option("CHROME9_TCL_FLOW_DEPTH", depth), 1,
               depth);
   }
   /* CHROME9_TCL_RELATIVE=0 leaves indirect uniform reads to the draw
    * module. */
   ctx->tcl_flow.relative = ctx->tcl_enabled &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_VERTEX_RELATIVE) &&
      debug_get_bool_option("CHROME9_TCL_RELATIVE", true);
   ctx->fragment_primary =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_PRIMARY);
   ctx->fragment_kill =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_KILL);
   ctx->texture_bias = chrome9_screen_has_capability2(
      screen, DRM_CHROME9_CAP2_TEXTURE_BIAS);
   ctx->fragment_add =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_ADD);
   ctx->draw_textured_triangle =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE);
   ctx->draw_textured_triangle_batch =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BATCH);
   ctx->draw_textured_triangle_run =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_RUN);
   ctx->draw_textured_triangle_mip =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_MIP);
   ctx->copy_rect = !!(capabilities & DRM_CHROME9_CAP_COPY_RECT);
   ctx->copy_rect_unguarded_destination =
      !!(capabilities & DRM_CHROME9_CAP_COPY_RECT_UNGUARDED_DESTINATION);
   ctx->copy_rect_preserve_alpha =
      !!(capabilities & DRM_CHROME9_CAP_COPY_RECT_PRESERVE_ALPHA) &&
      debug_get_bool_option("CHROME9_OPAQUE_SCANOUT_PRESENT", false);
   ctx->draw_textured_triangle_d16 =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16);
   ctx->draw_textured_triangle_d16_blend =
      !!(capabilities &
         DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND);
   ctx->textured_d16_bgra8888 =
      !!(capabilities & DRM_CHROME9_CAP_TEXTURED_D16_BGRA8888);
   ctx->textured_d16_source_over =
      !!(capabilities & DRM_CHROME9_CAP_TEXTURED_D16_SOURCE_OVER);
   ctx->draw_two_textured_triangle =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE);
   ctx->draw_two_textured_triangle_d16 =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16);
   ctx->draw_two_textured_triangle_d16_blend =
      !!(capabilities &
         DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND);
   ctx->two_texture_tex0_alpha_primary =
      !!(capabilities &
         DRM_CHROME9_CAP_TWO_TEXTURE_TEX0_ALPHA_PRIMARY);
   ctx->two_texture_signed_repeat_coords =
      !!(capabilities &
         DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS);
   ctx->draw_triangle_blend =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND);
   ctx->draw_textured_triangle_blend =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND);
   ctx->blend_bgra8888 =
      !!(capabilities & DRM_CHROME9_CAP_BLEND_BGRA8888);
   ctx->blend_extended_factors =
      !!(capabilities & DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS);
   ctx->blend_equation = chrome9_screen_has_capability2(
      screen, DRM_CHROME9_CAP2_BLEND_EQUATION);
   ctx->blend_color_factors = chrome9_screen_has_capability2(
      screen, DRM_CHROME9_CAP2_BLEND_COLOR);
   ctx->alpha_test_supported =
      !!(capabilities & DRM_CHROME9_CAP_ALPHA_TEST);
   ctx->scissor_supported =
      !!(capabilities & DRM_CHROME9_CAP_SCISSOR);
   ctx->occlusion_query = chrome9_screen_has_capability2(
      screen, DRM_CHROME9_CAP2_OCCLUSION_QUERY);
   ctx->texture_3d = chrome9_screen_has_capability2(
      screen, DRM_CHROME9_CAP2_TEXTURE_3D);
   ctx->fragment_stencil =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_STENCIL);
   ctx->draw_limit = MAX2(debug_get_num_option("CHROME9_DRAW_LIMIT", 0), 0);
   ctx->debug_draws = debug_get_bool_option("CHROME9_DEBUG_DRAWS", false);

   ctx->base.screen = screen;
   ctx->base.priv = priv;
   if (debug_get_bool_option("CHROME9_DEBUG_REJECT", false))
      ctx->reject_sites = _mesa_pointer_hash_table_create(NULL);
   ctx->base.destroy = chrome9_context_destroy;
   ctx->base.flush = chrome9_flush;
   ctx->base.flush_resource = chrome9_flush_resource;
   ctx->base.draw_vbo = chrome9_draw_vbo;
   ctx->base.clear = chrome9_clear;
   ctx->base.blit = chrome9_blit;
   ctx->base.clear_render_target = chrome9_clear_render_target;
   ctx->base.clear_depth_stencil = chrome9_clear_depth_stencil;
   ctx->base.create_blend_state = chrome9_create_blend_state;
   ctx->base.bind_blend_state = chrome9_bind_blend_state;
   ctx->base.delete_blend_state = chrome9_delete_state;
   ctx->base.create_depth_stencil_alpha_state = chrome9_create_dsa_state;
   ctx->base.bind_depth_stencil_alpha_state = chrome9_bind_dsa_state;
   ctx->base.delete_depth_stencil_alpha_state = chrome9_delete_state;
   ctx->base.create_rasterizer_state = chrome9_create_rasterizer_state;
   ctx->base.bind_rasterizer_state = chrome9_bind_rasterizer_state;
   ctx->base.delete_rasterizer_state = chrome9_delete_state;
   ctx->base.create_sampler_state = chrome9_create_sampler_state;
   ctx->base.bind_sampler_states = chrome9_bind_sampler_states;
   ctx->base.delete_sampler_state = chrome9_delete_sampler_state;
   ctx->base.create_sampler_view = chrome9_create_sampler_view;
   ctx->base.sampler_view_destroy = chrome9_sampler_view_destroy;
   ctx->base.sampler_view_release = u_default_sampler_view_release;
   ctx->base.set_sampler_views = chrome9_set_sampler_views;
   ctx->base.create_vs_state = chrome9_create_vs_state;
   ctx->base.bind_vs_state = chrome9_bind_vs_state;
   ctx->base.delete_vs_state = chrome9_delete_vs_state;
   ctx->base.create_fs_state = chrome9_create_fs_state;
   ctx->base.bind_fs_state = chrome9_bind_fs_state;
   ctx->base.delete_fs_state = chrome9_delete_fs_state;
   ctx->base.create_vertex_elements_state =
      chrome9_create_vertex_elements_state;
   ctx->base.bind_vertex_elements_state = chrome9_bind_vertex_elements_state;
   ctx->base.delete_vertex_elements_state =
      chrome9_delete_vertex_elements_state;
   ctx->base.set_vertex_buffers = chrome9_set_vertex_buffers;
   ctx->base.set_constant_buffer = chrome9_set_constant_buffer;
   ctx->base.set_inlinable_constants = chrome9_set_inlinable_constants;
   ctx->base.set_framebuffer_state = chrome9_set_framebuffer_state;
   ctx->base.set_viewport_states = chrome9_set_viewport_states;
   ctx->base.set_scissor_states = chrome9_set_scissor_states;
   ctx->base.set_clip_state = chrome9_set_clip_state;
   ctx->base.set_blend_color = chrome9_set_blend_color;
   ctx->base.set_stencil_ref = chrome9_set_stencil_ref;
   ctx->base.set_sample_mask = chrome9_set_sample_mask;
   ctx->base.set_polygon_stipple = chrome9_set_polygon_stipple;
   chrome9_init_query_functions(ctx);
   ctx->base.set_stream_output_targets = chrome9_set_stream_output_targets;
   ctx->base.set_debug_callback = u_default_set_debug_callback;
   ctx->base.resource_release = u_default_resource_release;
   chrome9_init_context_resource_functions(&ctx->base);

   /* LLVM vertex shaders sample the levels chrome9_refresh_vs_textures
    * maps; the TGSI-exec callback below serves the interpreter. */
   ctx->draw_llvm = draw_get_option_use_llvm() &&
      debug_get_bool_option("CHROME9_DRAW_LLVM", true);
   ctx->draw = ctx->draw_llvm ? draw_create(&ctx->base) :
                                draw_create_no_llvm(&ctx->base);
   if (!ctx->draw)
      goto fail;
   ctx->vs_tgsi_sampler = chrome9_vs_texture_sampler_create(ctx);
   if (!ctx->vs_tgsi_sampler)
      goto fail;
   draw_texture_sampler(ctx->draw, MESA_SHADER_VERTEX,
                        ctx->vs_tgsi_sampler);
   draw_set_constant_buffer_stride(ctx->draw, sizeof(float));
   /* The kernel draws triangles only: let draw turn every point and line
    * (including sprites and stipple) into triangles. */
   draw_wide_point_threshold(ctx->draw, 0.0f);
   draw_wide_line_threshold(ctx->draw, 0.0f);
   draw_enable_point_sprites(ctx->draw, true);
   draw_wide_point_gl(ctx->draw, true);
   draw_enable_line_stipple(ctx->draw, true);
   stage = chrome9_draw_vbuf_stage(ctx);
   if (!stage)
      goto fail;
   draw_set_rasterize_stage(ctx->draw, stage);

   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   if (!ctx->base.stream_uploader)
      goto fail;
   ctx->base.const_uploader = ctx->base.stream_uploader;
   return &ctx->base;

fail:
   chrome9_context_destroy(&ctx->base);
   return NULL;
}
