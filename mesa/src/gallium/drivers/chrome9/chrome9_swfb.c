/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * llvmpipe fallback for draws the Chrome9 pixel engine cannot execute
 * (fragment programs beyond 96 ALU / 32 TEX / 12 temps).  llvmpipe renders
 * directly into the chrome9 BO mappings: level-0 surfaces are wrapped as
 * llvmpipe display targets, mipmapped textures are copied.  All chrome9
 * ioctls are synchronous and the kernel flushes CPU caches around every
 * job, so ordering only needs "finish llvmpipe before the next GPU job"
 * (chrome9_sw_finish, called from chrome9_draw_flush_batch) and "retire GPU
 * batches before llvmpipe starts" (chrome9_draw_flush_batch here).
 * docs/research/chrome9-llvmpipe-fragment-fallback-design.md.
 */

#include "chrome9_swfb.h"

#include "chrome9_context.h"
#include "chrome9_draw.h"
#include "chrome9_public.h"
#include "chrome9_query.h"
#include "chrome9_resource.h"
#include "chrome9_specialise.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "frontend/sw_winsys.h"
#include "frontend/winsys_handle.h"
#include "llvmpipe/lp_public.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/hash_table.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct chrome9_sw_dt {
   uint8_t *map;
};

/* The display target is the chrome9 mapping of one level. */
static struct sw_displaytarget *
chrome9_sw_dt_from_handle(struct sw_winsys *ws,
                          const struct pipe_resource *templ,
                          struct winsys_handle *whandle, unsigned *stride)
{
   struct chrome9_resource *res = whandle->com_obj;
   struct chrome9_sw_dt *dt = CALLOC_STRUCT(chrome9_sw_dt);

   (void)ws;
   (void)templ;
   if (!dt)
      return NULL;
   dt->map = (uint8_t *)res->bo.map + whandle->offset;
   *stride = whandle->stride;
   return (struct sw_displaytarget *)dt;
}

static void *
chrome9_sw_dt_map(struct sw_winsys *ws, struct sw_displaytarget *dt,
                  unsigned flags)
{
   (void)ws;
   (void)flags;
   return ((struct chrome9_sw_dt *)dt)->map;
}

static void
chrome9_sw_dt_unmap(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
   (void)ws;
   (void)dt;
}

static void
chrome9_sw_dt_destroy(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
   (void)ws;
   FREE(dt);
}

static bool
chrome9_sw_dt_format_supported(struct sw_winsys *ws, unsigned usage,
                               enum pipe_format format)
{
   (void)ws;
   (void)usage;
   (void)format;
   return false;
}

static void
chrome9_sw_winsys_destroy(struct sw_winsys *ws)
{
   FREE(ws);
}

/* displaytarget_create_mapped stays NULL: llvmpipe would otherwise treat
 * the handle as a dma-buf. */
struct pipe_screen *
chrome9_sw_screen_create(void)
{
   struct sw_winsys *ws = CALLOC_STRUCT(sw_winsys);
   struct pipe_screen *screen;

   if (!ws)
      return NULL;
   ws->destroy = chrome9_sw_winsys_destroy;
   ws->is_displaytarget_format_supported = chrome9_sw_dt_format_supported;
   ws->displaytarget_from_handle = chrome9_sw_dt_from_handle;
   ws->displaytarget_map = chrome9_sw_dt_map;
   ws->displaytarget_unmap = chrome9_sw_dt_unmap;
   ws->displaytarget_destroy = chrome9_sw_dt_destroy;
   screen = llvmpipe_create_screen(ws);
   if (!screen)
      FREE(ws);
   return screen;
}

/* llvmpipe copy of a level whose 4x4-block overhang does not fit the
 * chrome9 object; rendered rows are copied back when it is released. */
struct chrome9_sw_bounce {
   struct pipe_resource *target;
   struct pipe_resource *copy;
   unsigned level;
   bool render;
   bool staged;
};

/*
 * Linear staging image of a tiled local level: llvmpipe renders into its
 * pages through alias, the engine copies it to and from the level.  It
 * equals the level while the level's device write_seq and CPU write
 * generation are unchanged.
 */
struct chrome9_sw_kept {
   struct pipe_resource *target;
   struct pipe_resource *staging;
   struct pipe_resource *alias;
   unsigned level;
   unsigned width;
   unsigned height;
   bool valid;
   uint64_t write_seq;
   uint32_t cpu_generation;
};

struct chrome9_sw_context {
   struct chrome9_context *ctx;
   struct pipe_screen *screen;
   struct pipe_context *lp;
   /* chrome9 CSO/shader/resource pointer -> llvmpipe object. */
   struct hash_table *objects;
   /* Per-level aliases of the pending draws, released by chrome9_sw_finish
    * so none outlives its chrome9 BO. */
   struct hash_table_u64 *aliases;
   /* chrome9 references held while llvmpipe may still access them. */
   struct util_dynarray held;
   struct util_dynarray bounces;
   struct util_dynarray kept;
   bool pending;
   /* The chrome9 screen reports integer fragment-position centers. */
   bool pixel_center_integer;
   uint64_t draws;
   uint64_t us;
   /* Staged levels uploaded and reused. */
   uint64_t staged_uploads;
   uint64_t staged_reuses;
};

static bool
chrome9_sw_debug(void)
{
   return debug_get_bool_option("CHROME9_SW_DEBUG", false);
}

bool
chrome9_sw_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = debug_get_bool_option("CHROME9_SW_FALLBACK", true);
   return enabled;
}

/* llvmpipe cannot address tiles: a tiled local level goes through a
 * linear staging image the engine converts. */
static bool
chrome9_sw_staged(const struct chrome9_resource *res)
{
   return res->local && chrome9_resource_is_tiled(res) &&
          (res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ||
           res->base.format == PIPE_FORMAT_B8G8R8X8_UNORM) &&
          debug_get_bool_option("CHROME9_SW_GPU_DETILE", true);
}

/* The device write count of a staged target, false without one. */
static bool
chrome9_sw_write_seq(struct chrome9_sw_context *sw,
                     const struct chrome9_resource *res, uint64_t *seq)
{
   struct pipe_screen *screen = sw->ctx->base.screen;

   return chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_WRITE_SEQ) &&
          chrome9_bo_write_seq(screen->get_screen_fd(screen), &res->bo, seq);
}

static void
chrome9_sw_kept_drop(struct chrome9_sw_context *sw, unsigned index)
{
   struct chrome9_sw_kept *kept =
      util_dynarray_element(&sw->kept, struct chrome9_sw_kept, index);

   pipe_resource_reference(&kept->alias, NULL);
   pipe_resource_reference(&kept->staging, NULL);
   pipe_resource_reference(&kept->target, NULL);
   *kept = util_dynarray_pop(&sw->kept, struct chrome9_sw_kept);
}

static struct chrome9_sw_kept *
chrome9_sw_kept_find(struct chrome9_sw_context *sw,
                     const struct pipe_resource *target, unsigned level)
{
   util_dynarray_foreach(&sw->kept, struct chrome9_sw_kept, kept)
      if (kept->target == target && kept->level == level)
         return kept;
   return NULL;
}

/* Whether a pending fallback draw uses the image. */
static bool
chrome9_sw_kept_busy(struct chrome9_sw_context *sw,
                     const struct chrome9_sw_kept *kept)
{
   util_dynarray_foreach(&sw->bounces, struct chrome9_sw_bounce, bounce)
      if (bounce->staged && bounce->target == kept->target &&
          bounce->level == kept->level)
         return true;
   return false;
}

/* A new staging image of the level; images whose target nobody else
 * references go first, and at most four idle ones are kept. */
static struct chrome9_sw_kept *
chrome9_sw_kept_create(struct chrome9_sw_context *sw,
                       struct pipe_resource *target, unsigned level)
{
   const unsigned width = u_minify(target->width0, level);
   const unsigned height = u_minify(target->height0, level);
   /* llvmpipe touches whole 4x4 blocks. */
   struct pipe_resource staging_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = target->format,
      .width0 = width,
      .height0 = align(height, 4),
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW,
      /* llvmpipe writes local pages without cache flushes per copy; the
       * CPU reads the image of a CPU-sampled level, so that stays cached. */
      .flags = debug_get_bool_option("CHROME9_SW_STAGING_LOCAL", true) &&
               !chrome9_resource(target)->cpu_sampled ?
               CHROME9_RESOURCE_FLAG_LOCAL : 0,
   };
   struct pipe_resource alias_templ = staging_templ;
   struct chrome9_sw_kept kept = {
      .level = level,
      .width = width,
      .height = height,
   };
   struct chrome9_resource *staging;
   struct winsys_handle handle = { .type = WINSYS_HANDLE_TYPE_SHARED };

   for (unsigned i = 0;
        i < util_dynarray_num_elements(&sw->kept, struct chrome9_sw_kept);) {
      struct chrome9_sw_kept *old =
         util_dynarray_element(&sw->kept, struct chrome9_sw_kept, i);

      if ((old->target == target && old->level == level) ||
          p_atomic_read(&old->target->reference.count) == 1)
         chrome9_sw_kept_drop(sw, i);
      else
         i++;
   }
   for (unsigned i = 0;
        util_dynarray_num_elements(&sw->kept, struct chrome9_sw_kept) >= 4 &&
        i < util_dynarray_num_elements(&sw->kept, struct chrome9_sw_kept);) {
      if (chrome9_sw_kept_busy(sw, util_dynarray_element(
                                      &sw->kept, struct chrome9_sw_kept, i)))
         i++;
      else
         chrome9_sw_kept_drop(sw, i);
   }
   kept.staging = sw->ctx->base.screen->resource_create(sw->ctx->base.screen,
                                                         &staging_templ);
   if (!kept.staging)
      return NULL;
   staging = chrome9_resource(kept.staging);
   handle.com_obj = staging;
   handle.offset = staging->level_offset[0];
   handle.stride = staging->level_stride[0];
   alias_templ.height0 = height;
   alias_templ.flags = 0;
   alias_templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   if (!staging->bo.map || chrome9_resource_is_tiled(staging) ||
       !(kept.alias = sw->screen->resource_from_handle(sw->screen,
                                                       &alias_templ,
                                                       &handle, 0))) {
      pipe_resource_reference(&kept.staging, NULL);
      return NULL;
   }
   pipe_resource_reference(&kept.target, target);
   util_dynarray_append(&sw->kept, kept);
   return util_dynarray_top_ptr(&sw->kept, struct chrome9_sw_kept);
}

/* Whether the staging image still equals the level. */
static bool
chrome9_sw_kept_current(struct chrome9_sw_context *sw,
                        const struct chrome9_sw_kept *kept)
{
   const struct chrome9_resource *res = chrome9_resource(kept->target);
   uint64_t write_seq;

   return kept->valid &&
          kept->cpu_generation == res->cpu_write_generation &&
          chrome9_sw_write_seq(sw, res, &write_seq) &&
          write_seq == kept->write_seq;
}

/* The level's current state, recorded after the image equals it. */
static void
chrome9_sw_kept_mark(struct chrome9_sw_context *sw,
                     struct chrome9_sw_kept *kept)
{
   const struct chrome9_resource *res = chrome9_resource(kept->target);

   kept->cpu_generation = res->cpu_write_generation;
   kept->valid = chrome9_sw_write_seq(sw, res, &kept->write_seq);
}

/* The staging image of a tiled local level with its current contents:
 * the engine copies the level in only when it changed. */
static struct chrome9_sw_kept *
chrome9_sw_stage(struct chrome9_sw_context *sw, struct pipe_resource *target,
                 unsigned level)
{
   struct chrome9_sw_kept *kept = chrome9_sw_kept_find(sw, target, level);

   /* A CPU-sampled level needs a cached image (chrome9_sw_kept_create). */
   if (!kept || kept->width != u_minify(target->width0, level) ||
       kept->height != u_minify(target->height0, level) ||
       (chrome9_resource(target)->cpu_sampled &&
        chrome9_resource_is_local(chrome9_resource(kept->staging))))
      kept = chrome9_sw_kept_create(sw, target, level);
   if (!kept)
      return NULL;
   if (chrome9_sw_kept_current(sw, kept)) {
      sw->staged_reuses++;
      return kept;
   }
   if (!chrome9_resource_copy_gpu(sw->ctx, chrome9_resource(kept->staging), 0,
                                  chrome9_resource(target), level, NULL))
      return NULL;
   chrome9_sw_kept_mark(sw, kept);
   sw->staged_uploads++;
   return kept;
}

static void
chrome9_sw_bounce_back(struct chrome9_sw_context *sw,
                       struct chrome9_sw_bounce *bounce)
{
   struct chrome9_resource *res = chrome9_resource(bounce->target);
   const unsigned level = bounce->level;
   const unsigned rows =
      util_format_get_nblocksy(bounce->target->format,
                               u_minify(bounce->target->height0, level));
   const size_t row_bytes =
      util_format_get_stride(bounce->target->format,
                             u_minify(bounce->target->width0, level));
   struct pipe_transfer *transfer;
   struct pipe_box box;
   const uint8_t *src;

   if (bounce->staged) {
      struct chrome9_sw_kept *kept =
         chrome9_sw_kept_find(sw, bounce->target, level);

      /* llvmpipe wrote the staging pages; the engine copies them back. */
      if (kept && bounce->render) {
         struct pipe_box box;

         /* The staging image has rows up to the 4x4 block boundary. */
         u_box_2d(0, 0, kept->width, kept->height, &box);
         chrome9_resource_cpu_access(chrome9_resource(kept->staging), true);
         if (chrome9_resource_copy_gpu(sw->ctx, res, level,
                                       chrome9_resource(kept->staging), 0,
                                       &box)) {
            chrome9_sw_kept_mark(sw, kept);
         } else {
            const struct chrome9_resource *staging =
               chrome9_resource(kept->staging);

            chrome9_resource_cpu_access(res, true);
            chrome9_tiled_copy((uint8_t *)res->bo.map +
                               res->level_offset[level],
                               res->level_stride[level],
                               chrome9_resource_is_tiled(res), 0, 0,
                               row_bytes, rows,
                               (uint8_t *)staging->bo.map +
                                  staging->level_offset[0],
                               staging->level_stride[0], true);
            kept->valid = false;
         }
      }
   } else if (bounce->render) {
      u_box_2d(0, 0, bounce->copy->width0, bounce->copy->height0, &box);
      src = sw->lp->texture_map(sw->lp, bounce->copy, 0, PIPE_MAP_READ,
                                &box, &transfer);
      if (src) {
         chrome9_resource_cpu_access(res, true);
         chrome9_tiled_copy((uint8_t *)res->bo.map + res->level_offset[level],
                            res->level_stride[level],
                            chrome9_resource_is_tiled(res), 0, 0, row_bytes,
                            rows, (uint8_t *)src, transfer->stride, true);
         sw->lp->texture_unmap(sw->lp, transfer);
      }
   }
   pipe_resource_reference(&bounce->target, NULL);
}

static void
chrome9_sw_release_held(struct chrome9_sw_context *sw)
{
   struct pipe_framebuffer_state empty = { 0 };

   /* Drop every llvmpipe binding of an alias before releasing it. */
   sw->lp->set_framebuffer_state(sw->lp, &empty);
   sw->lp->set_sampler_views(sw->lp, MESA_SHADER_FRAGMENT, 0, 0,
                             PIPE_MAX_SHADER_SAMPLER_VIEWS, NULL);
   sw->lp->set_sampler_views(sw->lp, MESA_SHADER_VERTEX, 0, 0,
                             PIPE_MAX_SHADER_SAMPLER_VIEWS, NULL);
   util_dynarray_foreach(&sw->bounces, struct chrome9_sw_bounce, bounce)
      chrome9_sw_bounce_back(sw, bounce);
   util_dynarray_clear(&sw->bounces);
   hash_table_u64_foreach(sw->aliases, entry) {
      struct pipe_resource *alias = entry.data;

      pipe_resource_reference(&alias, NULL);
   }
   _mesa_hash_table_u64_clear(sw->aliases);
   util_dynarray_foreach(&sw->held, struct pipe_resource *, res)
      pipe_resource_reference(res, NULL);
   util_dynarray_clear(&sw->held);
}

/* The fallback's staging image of the level while it equals the level,
 * with llvmpipe's writes to it finished; NULL otherwise. */
struct pipe_resource *
chrome9_sw_kept_image(struct chrome9_context *ctx,
                      struct pipe_resource *resource, unsigned level)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct chrome9_sw_kept *kept =
      sw ? chrome9_sw_kept_find(sw, resource, level) : NULL;

   if (!kept || !chrome9_sw_kept_current(sw, kept))
      return NULL;
   chrome9_sw_finish(ctx);
   return kept->staging;
}

/* The fallback's staging image of the level equals the level now. */
bool
chrome9_sw_kept_valid(struct chrome9_context *ctx,
                      struct pipe_resource *resource, unsigned level)
{
   struct chrome9_sw_context *sw = ctx->sw;
   const struct chrome9_sw_kept *kept =
      sw ? chrome9_sw_kept_find(sw, resource, level) : NULL;

   return kept && chrome9_sw_kept_current(sw, kept);
}

/* A device clear just submitted to a level whose kept copy was valid:
 * clear the copy alike so it stays valid. */
void
chrome9_sw_kept_clear(struct chrome9_context *ctx,
                      struct pipe_resource *resource, unsigned level,
                      const union pipe_color_union *color,
                      unsigned x, unsigned y, unsigned width,
                      unsigned height)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct chrome9_sw_kept *kept =
      sw ? chrome9_sw_kept_find(sw, resource, level) : NULL;
   struct pipe_surface templ = {
      .format = resource->format,
   };

   if (!kept)
      return;
   if (width && height) {
      chrome9_resource_cpu_access(chrome9_resource(kept->staging), true);
      templ.texture = kept->alias;
      sw->lp->clear_render_target(sw->lp, &templ, color, x, y, width, height,
                                  false);
   }
   chrome9_sw_kept_mark(sw, kept);
}

void
chrome9_sw_finish(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct pipe_fence_handle *fence = NULL;

   if (!sw || !sw->pending)
      return;
   sw->lp->flush(sw->lp, &fence, 0);
   if (fence) {
      sw->screen->fence_finish(sw->screen, NULL, fence, OS_TIMEOUT_INFINITE);
      sw->screen->fence_reference(sw->screen, &fence, NULL);
   }
   chrome9_sw_release_held(sw);
   sw->pending = false;
}

static void
chrome9_sw_hold(struct chrome9_sw_context *sw, struct pipe_resource *res)
{
   struct pipe_resource *ref = NULL;

   if (!res)
      return;
   pipe_resource_reference(&ref, res);
   util_dynarray_append(&sw->held, ref);
}

static struct chrome9_sw_context *
chrome9_sw_context(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;

   if (sw)
      return sw;
   sw = CALLOC_STRUCT(chrome9_sw_context);
   if (!sw)
      return NULL;
   sw->ctx = ctx;
   sw->screen = chrome9_screen_sw(ctx->base.screen);
   sw->pixel_center_integer =
      ctx->base.screen->caps.fs_coord_pixel_center_integer;
   if (sw->screen)
      sw->lp = sw->screen->context_create(sw->screen, NULL, 0);
   util_dynarray_init(&sw->held, NULL);
   util_dynarray_init(&sw->bounces, NULL);
   util_dynarray_init(&sw->kept, NULL);
   sw->objects = _mesa_pointer_hash_table_create(NULL);
   sw->aliases = _mesa_hash_table_u64_create(NULL);
   if (!sw->lp || !sw->objects || !sw->aliases) {
      if (sw->lp)
         sw->lp->destroy(sw->lp);
      _mesa_hash_table_destroy(sw->objects, NULL);
      _mesa_hash_table_u64_destroy(sw->aliases);
      FREE(sw);
      return NULL;
   }
   ctx->sw = sw;
   return sw;
}

enum chrome9_sw_kind {
   CHROME9_SW_BLEND,
   CHROME9_SW_DSA,
   CHROME9_SW_RASTERIZER,
   CHROME9_SW_SAMPLER,
   CHROME9_SW_VELEMS,
   CHROME9_SW_VS,
   CHROME9_SW_FS,
};

struct chrome9_sw_object {
   enum chrome9_sw_kind kind;
   void *cso;
};

static void
chrome9_sw_delete_object(struct pipe_context *lp, struct chrome9_sw_object *o)
{
   switch (o->kind) {
   case CHROME9_SW_BLEND:
      lp->delete_blend_state(lp, o->cso);
      break;
   case CHROME9_SW_DSA:
      lp->delete_depth_stencil_alpha_state(lp, o->cso);
      break;
   case CHROME9_SW_RASTERIZER:
      lp->delete_rasterizer_state(lp, o->cso);
      break;
   case CHROME9_SW_SAMPLER:
      lp->delete_sampler_state(lp, o->cso);
      break;
   case CHROME9_SW_VELEMS:
      lp->delete_vertex_elements_state(lp, o->cso);
      break;
   case CHROME9_SW_VS:
      lp->delete_vs_state(lp, o->cso);
      break;
   case CHROME9_SW_FS:
      lp->delete_fs_state(lp, o->cso);
      break;
   }
   FREE(o);
}

/* Called from every chrome9 delete hook: drop the llvmpipe twin.  Bound
 * twins are unbound first so llvmpipe never keeps a dangling pointer. */
void
chrome9_sw_forget(struct chrome9_context *ctx, const void *object)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct hash_entry *entry;
   struct chrome9_sw_object *o;

   if (!sw || !object)
      return;
   entry = _mesa_hash_table_search(sw->objects, object);
   if (!entry)
      return;
   o = entry->data;
   chrome9_sw_finish(ctx);
   switch (o->kind) {
   case CHROME9_SW_BLEND:
      sw->lp->bind_blend_state(sw->lp, NULL);
      break;
   case CHROME9_SW_DSA:
      sw->lp->bind_depth_stencil_alpha_state(sw->lp, NULL);
      break;
   case CHROME9_SW_RASTERIZER:
      sw->lp->bind_rasterizer_state(sw->lp, NULL);
      break;
   case CHROME9_SW_SAMPLER: {
      void *none[PIPE_MAX_SAMPLERS] = { 0 };

      sw->lp->bind_sampler_states(sw->lp, MESA_SHADER_FRAGMENT, 0,
                                  PIPE_MAX_SAMPLERS, none);
      sw->lp->bind_sampler_states(sw->lp, MESA_SHADER_VERTEX, 0,
                                  PIPE_MAX_SAMPLERS, none);
      break;
   }
   case CHROME9_SW_VELEMS:
      sw->lp->bind_vertex_elements_state(sw->lp, NULL);
      break;
   case CHROME9_SW_VS:
      sw->lp->bind_vs_state(sw->lp, NULL);
      break;
   case CHROME9_SW_FS:
      sw->lp->bind_fs_state(sw->lp, NULL);
      break;
   }
   chrome9_sw_delete_object(sw->lp, o);
   _mesa_hash_table_remove(sw->objects, entry);
}

struct pipe_context *
chrome9_sw_pipe(struct chrome9_context *ctx)
{
   return ctx->sw ? ctx->sw->lp : NULL;
}

void
chrome9_sw_context_destroy(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;

   if (!sw)
      return;
   chrome9_sw_finish(ctx);
   chrome9_sw_release_held(sw);
   sw->lp->bind_blend_state(sw->lp, NULL);
   sw->lp->bind_depth_stencil_alpha_state(sw->lp, NULL);
   sw->lp->bind_rasterizer_state(sw->lp, NULL);
   sw->lp->bind_vertex_elements_state(sw->lp, NULL);
   sw->lp->bind_vs_state(sw->lp, NULL);
   sw->lp->bind_fs_state(sw->lp, NULL);
   hash_table_foreach(sw->objects, entry)
      chrome9_sw_delete_object(sw->lp, entry->data);
   _mesa_hash_table_destroy(sw->objects, NULL);
   _mesa_hash_table_u64_destroy(sw->aliases);
   util_dynarray_fini(&sw->held);
   util_dynarray_fini(&sw->bounces);
   while (util_dynarray_num_elements(&sw->kept, struct chrome9_sw_kept))
      chrome9_sw_kept_drop(sw, 0);
   util_dynarray_fini(&sw->kept);
   if (chrome9_sw_debug())
      fprintf(stderr, "chrome9: sw fallback draws=%" PRIu64 " time=%" PRIu64
                      " us, staged uploads=%" PRIu64 " reuses=%" PRIu64 "\n",
              sw->draws, sw->us, sw->staged_uploads, sw->staged_reuses);
   sw->lp->destroy(sw->lp);
   FREE(sw);
   ctx->sw = NULL;
}

/* Chrome9 fragment shaders read the facing flag as the FACE varying;
 * llvmpipe provides it as the front-face system value. */
static bool
chrome9_sw_lower_face_instr(nir_builder *b, nir_intrinsic_instr *intr,
                            void *data)
{
   nir_variable *var;
   nir_def *face, *value;

   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   var = nir_intrinsic_get_var(intr, 0);
   if (!var || var->data.mode != nir_var_shader_in ||
       var->data.location != VARYING_SLOT_FACE ||
       intr->def.bit_size != 32)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   face = nir_load_front_face(b, 1);
   value = glsl_get_base_type(var->type) == GLSL_TYPE_FLOAT ?
           nir_bcsel(b, face, nir_imm_float(b, 1.0f),
                     nir_imm_float(b, -1.0f)) :
           nir_b2b32(b, face);
   nir_def_replace(&intr->def,
                   nir_replicate(b, value, intr->def.num_components));
   return true;
}

static void
chrome9_sw_lower_face_input(nir_shader *nir)
{
   if (!nir_shader_intrinsics_pass(nir, chrome9_sw_lower_face_instr,
                                   nir_metadata_control_flow, NULL))
      return;
   nir_foreach_shader_in_variable(var, nir)
      if (var->data.location == VARYING_SLOT_FACE)
         var->data.mode = nir_var_shader_temp;
   nir_fixup_deref_modes(nir);
   nir_opt_dce(nir);
   nir_remove_dead_variables(nir, nir_var_shader_temp, NULL);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static bool
chrome9_sw_scalarize_deriv_instr(nir_builder *b, nir_intrinsic_instr *intr,
                                 void *data)
{
   (void)data;
   switch (intr->intrinsic) {
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy_coarse:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddy_fine:
      break;
   default:
      return false;
   }
   if (intr->def.num_components == 1)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def,
                   nir_build_deriv(b, intr->src[0].ssa, intr->intrinsic));
   return true;
}

/* gallivm emits one channel per derivative intrinsic; llvmpipe relies on
 * scalarize_ddx at NIR build time, which the chrome9 options do not set. */
static void
chrome9_sw_scalarize_derivs(nir_shader *nir)
{
   nir_shader_intrinsics_pass(nir, chrome9_sw_scalarize_deriv_instr,
                              nir_metadata_control_flow, NULL);
}

/* Uniforms are float-encoded: integer operations become float ones, as in
 * the chrome9 backends.  The lowering leaves integer texture queries
 * untouched; llvmpipe returns them as integers. */
static bool
chrome9_sw_float_query_instr(nir_builder *b, nir_instr *instr, void *data)
{
   nir_tex_instr *tex;
   nir_def *value;

   (void)data;
   if (instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float)
      return false;
   b->cursor = nir_after_instr(instr);
   value = nir_i2f32(b, &tex->def);
   nir_def_rewrite_uses_after(&tex->def, value);
   return true;
}

static void
chrome9_sw_float_queries(nir_shader *nir)
{
   if (!nir->options->no_integers)
      return;
   chrome9_nir_lower_integers(nir);
   nir_shader_instructions_pass(nir, chrome9_sw_float_query_instr,
                                nir_metadata_control_flow, NULL);
}

static void *
chrome9_sw_object(struct chrome9_sw_context *sw, const void *key,
                  enum chrome9_sw_kind kind, const void *state)
{
   struct hash_entry *entry = _mesa_hash_table_search(sw->objects, key);
   struct chrome9_sw_object *o;

   if (entry)
      return ((struct chrome9_sw_object *)entry->data)->cso;
   o = CALLOC_STRUCT(chrome9_sw_object);
   if (!o)
      return NULL;
   o->kind = kind;
   switch (kind) {
   case CHROME9_SW_BLEND:
      o->cso = sw->lp->create_blend_state(sw->lp, state);
      break;
   case CHROME9_SW_DSA:
      o->cso = sw->lp->create_depth_stencil_alpha_state(sw->lp, state);
      break;
   case CHROME9_SW_RASTERIZER:
      o->cso = sw->lp->create_rasterizer_state(sw->lp, state);
      break;
   case CHROME9_SW_SAMPLER:
      o->cso = sw->lp->create_sampler_state(sw->lp, state);
      break;
   case CHROME9_SW_VELEMS: {
      const struct chrome9_vertex_elements *velems = state;

      o->cso = sw->lp->create_vertex_elements_state(sw->lp, velems->count,
                                                    velems->elements);
      break;
   }
   case CHROME9_SW_VS:
   case CHROME9_SW_FS: {
      struct pipe_shader_state shader = {
         .type = PIPE_SHADER_IR_NIR,
         .ir.nir = nir_shader_clone(NULL, state),
      };

      /* The chrome9 lowering float-encodes integer uniforms but keeps
       * native 1-bit booleans; gallivm's no_integers path cannot take
       * those, so compile under llvmpipe's own options. */
      if (shader.ir.nir) {
         chrome9_sw_float_queries(shader.ir.nir);
         shader.ir.nir->options =
            sw->screen->nir_options[kind == CHROME9_SW_VS ?
                                    MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT];
         if (kind == CHROME9_SW_FS) {
            /* Chrome9 takes the fragment position as a system value,
             * llvmpipe as the POS input. */
            const nir_lower_sysvals_to_varyings_options pos = {
               .frag_coord = true,
            };

            chrome9_sw_lower_face_input(shader.ir.nir);
            /* The state tracker biased gl_FragCoord for those centers. */
            shader.ir.nir->info.fs.pixel_center_integer =
               sw->pixel_center_integer;
            chrome9_sw_scalarize_derivs(shader.ir.nir);
            if (nir_lower_sysvals_to_varyings(shader.ir.nir, &pos))
               nir_shader_gather_info(shader.ir.nir,
                  nir_shader_get_entrypoint(shader.ir.nir));
         }
      }
      if (shader.ir.nir && debug_get_bool_option("CHROME9_SW_DUMP", false))
         nir_print_shader(shader.ir.nir, stderr);
      if (shader.ir.nir)
         o->cso = kind == CHROME9_SW_VS ?
                  sw->lp->create_vs_state(sw->lp, &shader) :
                  sw->lp->create_fs_state(sw->lp, &shader);
      break;
   }
   }
   if (!o->cso) {
      FREE(o);
      return NULL;
   }
   _mesa_hash_table_insert(sw->objects, key, o);
   return o->cso;
}

/* CHROME9_SW_DEBUG names the state llvmpipe could not take. */
static bool
chrome9_sw_unavailable(const char *what)
{
   if (chrome9_sw_debug())
      fprintf(stderr, "chrome9: sw fallback: no %s\n", what);
   return false;
}

/* Display-target alias of one level of a chrome9 surface, created once. */
/* Copy one chrome9 level (tiled or linear) into an llvmpipe level. */
static bool
chrome9_sw_upload(struct chrome9_sw_context *sw, struct pipe_resource *copy,
                  unsigned copy_level, struct chrome9_resource *res,
                  unsigned level)
{
   const struct pipe_resource *resource = &res->base;
   struct pipe_transfer *transfer;
   struct pipe_box box;
   uint8_t *dst;

   u_box_2d(0, 0, u_minify(resource->width0, level),
            u_minify(resource->height0, level), &box);
   dst = sw->lp->texture_map(sw->lp, copy, copy_level,
                             PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                             &box, &transfer);
   if (!dst)
      return false;
   chrome9_tiled_copy((uint8_t *)res->bo.map + res->level_offset[level],
                         res->level_stride[level],
                         chrome9_resource_is_tiled(res), 0, 0,
                         util_format_get_stride(resource->format, box.width),
                         util_format_get_nblocksy(resource->format,
                                                  box.height),
                         dst, transfer->stride, false);
   sw->lp->texture_unmap(sw->lp, transfer);
   return true;
}

static struct pipe_resource *
chrome9_sw_alias(struct chrome9_sw_context *sw, struct pipe_resource *resource,
                 unsigned level, unsigned bind)
{
   struct chrome9_resource *res = chrome9_resource(resource);
   /* Allocations are 16-byte aligned; levels fit in the low bits. */
   const uint64_t key = (uint64_t)(uintptr_t)resource << 4 | level;
   struct pipe_resource *alias = _mesa_hash_table_u64_search(sw->aliases, key);
   struct pipe_resource templ = *resource;
   const unsigned height = u_minify(resource->height0, level);
   struct winsys_handle handle = {
      .type = WINSYS_HANDLE_TYPE_SHARED,
      .com_obj = res,
      .offset = res->level_offset[level],
      .stride = res->level_stride[level],
   };

   /* llvmpipe accesses the pages directly in this draw; a staged level
    * only through the engine's copies. */
   if (!chrome9_sw_staged(res))
      chrome9_resource_cpu_access(res, bind != 0);
   if (alias) {
      /* A staged image first bound for sampling must still be written. */
      if (bind && chrome9_sw_staged(res)) {
         struct chrome9_sw_kept *kept = chrome9_sw_kept_find(sw, resource,
                                                             level);

         if (kept)
            chrome9_resource_cpu_access(chrome9_resource(kept->staging),
                                        true);
      }
      /* A copy first bound for sampling must still be written back. */
      if (bind)
         util_dynarray_foreach(&sw->bounces, struct chrome9_sw_bounce, bounce)
            if (bounce->copy == alias)
               bounce->render = true;
      return alias;
   }
   /* Cube faces and 3D slices have no display-target form. */
   if (!res->bo.map || !chrome9_is_surface_target(resource->target) ||
       level > resource->last_level || level >= 16) {
      chrome9_sw_unavailable("surface form of the target");
      return NULL;
   }
   templ.width0 = u_minify(resource->width0, level);
   templ.height0 = height;
   templ.last_level = 0;
   /* One alias serves sampling and rendering within a draw sequence. */
   templ.bind = PIPE_BIND_SAMPLER_VIEW |
                (util_format_is_depth_or_stencil(resource->format) ?
                 PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET);
   templ.next = NULL;
   pipe_reference_init(&templ.reference, 1);
   /* llvmpipe touches whole 4x4 blocks: without room for the row overhang
    * before the trailing guard page it renders into a padded copy. */
   if (chrome9_sw_staged(res)) {
      struct chrome9_sw_bounce bounce = {
         .level = level, .render = bind, .staged = true,
      };
      struct chrome9_sw_kept *kept = chrome9_sw_stage(sw, resource, level);

      if (!kept) {
         chrome9_sw_unavailable("staging image of the target");
         return NULL;
      }
      chrome9_resource_cpu_access(chrome9_resource(kept->staging), bind != 0);
      pipe_resource_reference(&alias, kept->alias);
      pipe_resource_reference(&bounce.target, resource);
      bounce.copy = alias;
      util_dynarray_append(&sw->bounces, bounce);
   } else if (chrome9_resource_is_tiled(res) ||
       ((height & 3) &&
        res->level_offset[level] +
           (uint64_t)res->level_stride[level] * align(height, 4) >
           res->bo.size - (res->guarded ? 4096 : 0))) {
      struct chrome9_sw_bounce bounce = { .level = level, .render = bind };

      alias = sw->screen->resource_create(sw->screen, &templ);
      if (!alias) {
         chrome9_sw_unavailable("padded copy of the target");
         return NULL;
      }
      if (!chrome9_sw_upload(sw, alias, 0, res, level)) {
         pipe_resource_reference(&alias, NULL);
         chrome9_sw_unavailable("padded copy of the target");
         return NULL;
      }
      pipe_resource_reference(&bounce.target, resource);
      bounce.copy = alias;
      util_dynarray_append(&sw->bounces, bounce);
   } else {
      alias = sw->screen->resource_from_handle(sw->screen, &templ, &handle,
                                               0);
   }
   if (alias)
      _mesa_hash_table_u64_insert(sw->aliases, key, alias);
   return alias;
}

/* Mipmapped, cube, 3D and block-compressed textures: llvmpipe needs its
 * own layout; copy the current contents for every fallback draw.  *copied
 * tells the caller that it owns the returned reference. */
static struct pipe_resource *
chrome9_sw_texture(struct chrome9_sw_context *sw,
                   struct pipe_resource *resource, bool *copied)
{
   struct chrome9_resource *res = chrome9_resource(resource);
   struct pipe_resource templ = *resource;
   struct pipe_resource *copy;

   *copied = false;
   if (!resource->last_level && resource->target != PIPE_TEXTURE_CUBE &&
       resource->target != PIPE_TEXTURE_3D &&
       !util_format_is_compressed(resource->format))
      return chrome9_sw_alias(sw, resource, 0, 0);
   chrome9_resource_cpu_access(res, false);
   templ.bind = PIPE_BIND_SAMPLER_VIEW;
   templ.next = NULL;
   pipe_reference_init(&templ.reference, 1);
   copy = sw->screen->resource_create(sw->screen, &templ);
   if (!copy)
      return NULL;
   *copied = true;
   /* Volume slices are contiguous at each level's image size. */
   if (resource->target == PIPE_TEXTURE_3D) {
      for (unsigned level = 0; level <= resource->last_level; level++) {
         struct pipe_box box;

         u_box_3d(0, 0, 0, u_minify(resource->width0, level),
                  u_minify(resource->height0, level),
                  u_minify(resource->depth0, level), &box);
         sw->lp->texture_subdata(sw->lp, copy, level, PIPE_MAP_WRITE, &box,
                                 (uint8_t *)res->bo.map +
                                    res->level_offset[level],
                                 res->level_stride[level],
                                 res->level_size[level]);
      }
      return copy;
   }
   for (unsigned layer = 0; layer < resource->array_size; layer++)
      for (unsigned level = 0; level <= resource->last_level; level++) {
         struct pipe_box box;
         unsigned height = util_format_get_nblocksy(resource->format,
            u_minify(resource->height0, level));

         if (chrome9_resource_is_tiled(res)) {
            if (!chrome9_sw_upload(sw, copy, level, res, level)) {
               pipe_resource_reference(&copy, NULL);
               *copied = false;
               return NULL;
            }
            continue;
         }
         u_box_2d_zslice(0, 0, layer, u_minify(resource->width0, level),
                         u_minify(resource->height0, level), &box);
         sw->lp->texture_subdata(sw->lp, copy, level, PIPE_MAP_WRITE, &box,
                                 (uint8_t *)res->bo.map +
                                    res->level_offset[level] +
                                    layer * res->layer_stride,
                                 res->level_stride[level],
                                 res->level_stride[level] * height);
      }
   return copy;
}

static bool
chrome9_sw_bind_views(struct chrome9_sw_context *sw,
                      mesa_shader_stage stage,
                      struct pipe_sampler_view **views, unsigned count)
{
   struct pipe_sampler_view *lp_views[PIPE_MAX_SHADER_SAMPLER_VIEWS] = { 0 };
   bool ok = true;

   for (unsigned i = 0; i < count; i++) {
      struct pipe_sampler_view templ;
      struct pipe_resource *texture;
      bool copied;

      if (!views[i])
         continue;
      texture = chrome9_sw_texture(sw, views[i]->texture, &copied);
      if (!texture) {
         ok = false;
         break;
      }
      templ = *views[i];
      templ.texture = NULL;
      templ.context = NULL;
      lp_views[i] = sw->lp->create_sampler_view(sw->lp, texture, &templ);
      if (copied)
         pipe_resource_reference(&texture, NULL);
      chrome9_sw_hold(sw, views[i]->texture);
   }
   sw->lp->set_sampler_views(sw->lp, stage, 0, ok ? count : 0,
                             PIPE_MAX_SHADER_SAMPLER_VIEWS -
                                (ok ? count : 0), ok ? lp_views : NULL);
   for (unsigned i = 0; i < count; i++)
      pipe_sampler_view_reference(&lp_views[i], NULL);
   return ok;
}

static bool
chrome9_sw_bind_constants(struct chrome9_sw_context *sw,
                          mesa_shader_stage stage,
                          const struct pipe_constant_buffer *buffers)
{
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct pipe_constant_buffer *cb = &buffers[i];
      struct pipe_constant_buffer copy = { 0 };
      const uint8_t *data = cb->user_buffer;

      if (!data && cb->buffer)
         data = chrome9_resource(cb->buffer)->bo.map;
      if (!data || !cb->buffer_size) {
         sw->lp->set_constant_buffer(sw->lp, stage, i, NULL);
         continue;
      }
      /* llvmpipe keeps constant buffers by reference until rasterized. */
      u_upload_data_ref(sw->lp->const_uploader, 0, cb->buffer_size, 16,
                    data + cb->buffer_offset, &copy.buffer_offset,
                    &copy.buffer);
      if (!copy.buffer)
         return false;
      copy.buffer_size = cb->buffer_size;
      sw->lp->set_constant_buffer(sw->lp, stage, i, &copy);
      pipe_resource_reference(&copy.buffer, NULL);
   }
   u_upload_unmap(sw->lp->const_uploader);
   return true;
}

static bool
chrome9_sw_bind_state(struct chrome9_context *ctx, struct chrome9_sw_context *sw)
{
   struct pipe_context *lp = sw->lp;
   struct pipe_framebuffer_state fb = ctx->framebuffer;
   struct pipe_vertex_buffer vbs[PIPE_MAX_ATTRIBS];
   void *samplers[PIPE_MAX_SAMPLERS] = { 0 };
   void *cso;

   /* Color renders into the guarded target the hardware path uses. */
   for (unsigned i = 0; i < fb.nr_cbufs; i++) {
      struct pipe_resource *target = i == 0 && ctx->render_target ?
                                     ctx->render_target : fb.cbufs[i].texture;

      if (!target)
         continue;
      /* The shadow of a subimage target is its own level zero. */
      if (target == ctx->render_target && target != fb.cbufs[i].texture)
         fb.cbufs[i].level = 0;
      fb.cbufs[i].texture = chrome9_sw_alias(sw, target, fb.cbufs[i].level,
                                             PIPE_BIND_RENDER_TARGET);
      fb.cbufs[i].level = 0;
      if (!fb.cbufs[i].texture)
         return chrome9_sw_unavailable("color target");
      chrome9_sw_hold(sw, target);
   }
   if (fb.zsbuf.texture) {
      struct pipe_resource *zs = fb.zsbuf.texture;

      fb.zsbuf.texture = chrome9_sw_alias(sw, zs, fb.zsbuf.level,
                                          PIPE_BIND_DEPTH_STENCIL);
      fb.zsbuf.level = 0;
      if (!fb.zsbuf.texture)
         return chrome9_sw_unavailable("depth target");
      chrome9_sw_hold(sw, zs);
   }
   lp->set_framebuffer_state(lp, &fb);

   if (!ctx->blend || !ctx->dsa || !ctx->rasterizer || !ctx->vertex_elements)
      return chrome9_sw_unavailable("unbound state");
   if (!(cso = chrome9_sw_object(sw, ctx->blend, CHROME9_SW_BLEND, ctx->blend)))
      return chrome9_sw_unavailable("blend");
   lp->bind_blend_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->dsa, CHROME9_SW_DSA, ctx->dsa)))
      return chrome9_sw_unavailable("depth/stencil/alpha");
   lp->bind_depth_stencil_alpha_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->rasterizer, CHROME9_SW_RASTERIZER,
                                 ctx->rasterizer)))
      return chrome9_sw_unavailable("rasterizer");
   lp->bind_rasterizer_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->vertex_elements, CHROME9_SW_VELEMS,
                                 ctx->vertex_elements)))
      return chrome9_sw_unavailable("vertex elements");
   lp->bind_vertex_elements_state(lp, cso);
   if (!ctx->vs->sw_nir || !ctx->fs->nir ||
       !(cso = chrome9_sw_object(sw, ctx->vs, CHROME9_SW_VS, ctx->vs->sw_nir)))
      return chrome9_sw_unavailable("vertex shader");
   lp->bind_vs_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->fs, CHROME9_SW_FS,
                                 ctx->fs->sw_nir ? ctx->fs->sw_nir :
                                                   ctx->fs->nir)))
      return chrome9_sw_unavailable("fragment shader");
   lp->bind_fs_state(lp, cso);

   lp->set_stencil_ref(lp, ctx->stencil_ref);
   lp->set_blend_color(lp, &ctx->blend_color);
   lp->set_sample_mask(lp, ~0u);
   lp->set_viewport_states(lp, 0, 1, &ctx->viewport);
   lp->set_scissor_states(lp, 0, 1, &ctx->scissor);
   lp->set_clip_state(lp, &ctx->clip);
   lp->set_polygon_stipple(lp, &ctx->poly_stipple);

   for (unsigned i = 0; i < ctx->num_sampler_states; i++)
      if (ctx->sampler_states[i] &&
          !(samplers[i] = chrome9_sw_object(sw, ctx->sampler_states[i],
                                            CHROME9_SW_SAMPLER,
                                            &ctx->sampler_states[i]->base)))
         return chrome9_sw_unavailable("fragment sampler");
   lp->bind_sampler_states(lp, MESA_SHADER_FRAGMENT, 0,
                           ctx->num_sampler_states, samplers);
   memset(samplers, 0, sizeof(samplers));
   for (unsigned i = 0; i < ctx->num_vs_sampler_states; i++)
      if (ctx->vs_sampler_states[i] &&
          !(samplers[i] = chrome9_sw_object(sw, ctx->vs_sampler_states[i],
                                            CHROME9_SW_SAMPLER,
                                            ctx->vs_sampler_states[i])))
         return chrome9_sw_unavailable("vertex sampler");
   lp->bind_sampler_states(lp, MESA_SHADER_VERTEX, 0,
                           ctx->num_vs_sampler_states, samplers);
   if (!chrome9_sw_bind_views(sw, MESA_SHADER_FRAGMENT, ctx->sampler_views,
                              ctx->num_sampler_views) ||
       !chrome9_sw_bind_views(sw, MESA_SHADER_VERTEX, ctx->vs_sampler_views,
                              ctx->num_vs_sampler_views))
      return chrome9_sw_unavailable("sampler views");
   if (!chrome9_sw_bind_constants(sw, MESA_SHADER_VERTEX, ctx->vs_constants) ||
       !chrome9_sw_bind_constants(sw, MESA_SHADER_FRAGMENT, ctx->fs_constants))
      return chrome9_sw_unavailable("constants");

   /* Vertex data is consumed synchronously inside draw_vbo. */
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];

      vbs[i] = *vb;
      if (!vb->is_user_buffer) {
         vbs[i].is_user_buffer = true;
         vbs[i].buffer.user = vb->buffer.resource ?
            chrome9_resource(vb->buffer.resource)->bo.map : NULL;
      }
   }
   lp->set_vertex_buffers(lp, ctx->num_vertex_buffers, vbs);
   return true;
}

static void
chrome9_sw_demote(struct chrome9_context *ctx, struct pipe_resource *resource)
{
   /* Single-level staged images; other levels are aliased directly. */
   if (resource && !(chrome9_sw_staged(chrome9_resource(resource)) &&
                     resource->target == PIPE_TEXTURE_2D &&
                     !resource->last_level))
      chrome9_context_demote_local(ctx, resource);
}

bool
chrome9_sw_draw_vbo(struct chrome9_context *ctx,
                    const struct pipe_draw_info *info, unsigned drawid_offset,
                    const struct pipe_draw_start_count_bias *draws,
                    unsigned num_draws)
{
   struct pipe_draw_info lp_info = *info;
   struct chrome9_sw_context *sw;
   int64_t start = os_time_get();

   if (!chrome9_sw_enabled() || !ctx->vs || !ctx->fs)
      return false;
   /* Depth written by the pixel engine and llvmpipe differ in rounding:
    * an EQUAL test across the two would only partly pass. */
   if (ctx->dsa && ctx->dsa->depth_enabled &&
       ctx->dsa->depth_func == PIPE_FUNC_EQUAL)
      return false;
   /* llvmpipe reads its targets and textures on the CPU: none may stay
    * in uncached local memory, except levels it reaches only through a
    * staging image. */
   for (unsigned i = 0; i < ctx->framebuffer.nr_cbufs; i++)
      chrome9_sw_demote(ctx, ctx->framebuffer.cbufs[i].texture);
   chrome9_sw_demote(ctx, ctx->framebuffer.zsbuf.texture);
   for (unsigned i = 0; i < ctx->num_sampler_views; i++)
      if (ctx->sampler_views[i])
         chrome9_sw_demote(ctx, ctx->sampler_views[i]->texture);
   for (unsigned i = 0; i < ctx->num_vs_sampler_views; i++)
      if (ctx->vs_sampler_views[i])
         chrome9_sw_demote(ctx, ctx->vs_sampler_views[i]->texture);
   /* Retire GPU batches that wrote the targets or textures read here. */
   chrome9_draw_flush_gpu_batch(ctx);
   chrome9_context_prepare_render_target(ctx);
   sw = chrome9_sw_context(ctx);
   if (!sw)
      return false;
   if (!chrome9_sw_bind_state(ctx, sw) ||
       (info->index_size && !info->has_user_indices &&
        !info->index.resource)) {
      if (chrome9_sw_debug())
         fprintf(stderr, "chrome9: sw fallback state unavailable\n");
      if (!sw->pending)
         chrome9_sw_release_held(sw);
      return false;
   }
   if (info->index_size && !info->has_user_indices) {
      lp_info.has_user_indices = true;
      lp_info.index.user = chrome9_resource(info->index.resource)->bo.map;
   }
   chrome9_query_sw_begin(ctx, sw->lp);
   sw->lp->draw_vbo(sw->lp, &lp_info, drawid_offset, NULL, draws, num_draws);
   sw->pending = true;
   if (debug_get_bool_option("CHROME9_SW_SYNC", false))
      chrome9_sw_finish(ctx);
   chrome9_context_render_target_written(ctx);
   sw->draws++;
   sw->us += os_time_get() - start;
   if (chrome9_sw_debug())
      fprintf(stderr, "chrome9: sw fallback draw %" PRIu64 " count=%u "
                      "%" PRId64 " us\n", sw->draws, draws[0].count,
              os_time_get() - start);
   return true;
}
