/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_resource.h"

#include "chrome9_public.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"

#define CHROME9_MAX_BUFFER_SIZE (256ULL * 1024 * 1024)
#define CHROME9_MAX_SURFACE_DIMENSION 2048
#define CHROME9_MIP_ALIGNMENT 256

struct chrome9_resource_layout {
   uint32_t level_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_offset[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_size[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t allocation_size;
   bool guarded;
};

static bool
chrome9_is_surface_target(enum pipe_texture_target target)
{
   return target == PIPE_TEXTURE_2D || target == PIPE_TEXTURE_RECT;
}

static bool
chrome9_u64_add(uint64_t a, uint64_t b, uint64_t *result)
{
   if (a > UINT64_MAX - b)
      return false;
   *result = a + b;
   return true;
}

static bool
chrome9_u64_align(uint64_t value, uint64_t alignment, uint64_t *result)
{
   if (!alignment || !util_is_power_of_two_or_zero(alignment) ||
       value > UINT64_MAX - (alignment - 1))
      return false;
   *result = ALIGN_POT(value, alignment);
   return true;
}

static bool
chrome9_resource_layout(const struct pipe_resource *templ,
                        struct chrome9_resource_layout *layout)
{
   uint64_t aligned_level0_size;
   uint64_t cursor;
   unsigned cpp;
   long page_size;

   memset(layout, 0, sizeof(*layout));

   if (templ->target == PIPE_BUFFER) {
      if (!templ->width0 || templ->width0 > CHROME9_MAX_BUFFER_SIZE)
         return false;
      layout->level_size[0] = templ->width0;
      layout->allocation_size = templ->width0;
      return true;
   }

   if (!chrome9_is_surface_target(templ->target) ||
       (templ->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        templ->format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        templ->format != PIPE_FORMAT_Z16_UNORM) ||
       !templ->width0 || templ->width0 > CHROME9_MAX_SURFACE_DIMENSION ||
       !templ->height0 || templ->height0 > CHROME9_MAX_SURFACE_DIMENSION ||
       templ->depth0 != 1 || templ->array_size != 1 ||
       templ->last_level >= PIPE_MAX_TEXTURE_LEVELS ||
       (templ->target == PIPE_TEXTURE_RECT && templ->last_level) ||
       templ->last_level > util_logbase2(MAX2(templ->width0, templ->height0)) ||
       templ->nr_samples > 1 || templ->nr_storage_samples > 1)
      return false;

   page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 || !util_is_power_of_two_or_zero(page_size) ||
       page_size > UINT32_MAX || page_size < CHROME9_MIP_ALIGNMENT)
      return false;

   cpp = templ->format == PIPE_FORMAT_Z16_UNORM ? 2 : 4;
   for (unsigned level = 0; level <= templ->last_level; level++) {
      const unsigned width = u_minify(templ->width0, level);
      const unsigned height = u_minify(templ->height0, level);

      layout->level_stride[level] = ALIGN_POT(width * cpp, 32);
      layout->level_size[level] =
         (uint64_t)layout->level_stride[level] * height;
   }

   layout->level_offset[0] = page_size;
   if (!chrome9_u64_align(layout->level_size[0], page_size,
                          &aligned_level0_size) ||
       !chrome9_u64_add(layout->level_offset[0], aligned_level0_size,
                        &cursor) ||
       !chrome9_u64_add(cursor, page_size, &cursor))
      return false;

   /* Keep the old level-zero object byte-for-byte: one leading guard,
    * canonical L0 rows, page padding, then its guard.  Mip levels begin only
    * after that internal guard.  A final guard terminates the complete BO.
    * Before submitting such a BO, the kernel must accept/map its actual size
    * instead of requiring the smaller two-guard L0 size, while continuing to
    * protect the internal L0 guard as well as the final BO guard. */
   for (unsigned level = 1; level <= templ->last_level; level++) {
      if (!chrome9_u64_align(cursor, CHROME9_MIP_ALIGNMENT,
                             &layout->level_offset[level]) ||
          !chrome9_u64_add(layout->level_offset[level],
                           layout->level_size[level], &cursor))
         return false;
   }

   if (templ->last_level &&
       (!chrome9_u64_align(cursor, page_size, &cursor) ||
        !chrome9_u64_add(cursor, page_size, &cursor)))
      return false;
   if (cursor > CHROME9_MAX_BUFFER_SIZE)
      return false;

   layout->allocation_size = cursor;
   layout->guarded = true;
   return true;
}

static void
chrome9_resource_apply_layout(struct chrome9_resource *resource,
                              const struct chrome9_resource_layout *layout)
{
   memcpy(resource->level_stride, layout->level_stride,
          sizeof(resource->level_stride));
   memcpy(resource->level_offset, layout->level_offset,
          sizeof(resource->level_offset));
   memcpy(resource->level_size, layout->level_size,
          sizeof(resource->level_size));
   resource->stride = layout->level_stride[0];
   resource->data_offset = layout->level_offset[0];
   resource->data_size = layout->level_size[0];
   resource->guarded = layout->guarded;
}

static bool
chrome9_format_supported(struct pipe_screen *screen,
                         enum pipe_format format,
                         enum pipe_texture_target target,
                         unsigned sample_count,
                         unsigned storage_sample_count,
                         unsigned bindings)
{
   const unsigned buffer_bindings =
      PIPE_BIND_CUSTOM | PIPE_BIND_LINEAR | PIPE_BIND_VERTEX_BUFFER |
      PIPE_BIND_INDEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
      PIPE_BIND_VERTEX_STATE;
   const unsigned surface_bindings =
      PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR | PIPE_BIND_DISPLAY_TARGET |
      PIPE_BIND_SCANOUT | PIPE_BIND_SHARED | PIPE_BIND_SAMPLER_VIEW;
   const unsigned share_bindings = PIPE_BIND_DISPLAY_TARGET |
                                    PIPE_BIND_SCANOUT | PIPE_BIND_SHARED;
   /* Mesa adds SAMPLER_VIEW to user renderbuffers so that common meta paths
    * may address them.  Accept that allocation contract for Z16 even though
    * create_sampler_view keeps rejecting Z16 until depth sampling is wired.
    * Direct hardware depth use remains gated by DEPTH_STENCIL below. */
   const unsigned depth_bindings = PIPE_BIND_DEPTH_STENCIL |
                                   PIPE_BIND_SAMPLER_VIEW |
                                   PIPE_BIND_LINEAR;
   unsigned bgra_bindings = PIPE_BIND_LINEAR;
   bool bgra_render;

   if (sample_count > 1 || storage_sample_count > 1)
      return false;
   if (target == PIPE_BUFFER)
      return !(bindings & ~buffer_bindings);
   /* GBM asks for an ARGB allocation carrying its complete render, sampler,
    * display, scanout, and sharing usage set.  The modesetting path presents
    * it through the same linear XRGB scanout contract; actual BGRA draws
    * remain independently capability-gated in chrome9_draw_state_supported.
    */
   if (chrome9_is_surface_target(target) &&
       format == PIPE_FORMAT_B8G8R8A8_UNORM &&
       (bindings & share_bindings)) {
      if (bindings & ~surface_bindings)
         return false;
      return chrome9_format_supported(screen,
                                      PIPE_FORMAT_B8G8R8X8_UNORM,
                                      target, sample_count,
                                      storage_sample_count,
                                      PIPE_BIND_SCANOUT);
   }
   if ((bindings & PIPE_BIND_SAMPLER_VIEW) &&
       !chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE))
      return false;
   if (chrome9_is_surface_target(target) &&
       format == PIPE_FORMAT_Z16_UNORM)
      return chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_D16) &&
             (bindings & PIPE_BIND_DEPTH_STENCIL) &&
             !(bindings & ~depth_bindings);
   if (!chrome9_is_surface_target(target))
      return false;
   if (format == PIPE_FORMAT_B8G8R8X8_UNORM)
      return !(bindings & ~surface_bindings);
   if (format != PIPE_FORMAT_B8G8R8A8_UNORM)
      return false;
   if (chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE))
      bgra_bindings |= PIPE_BIND_SAMPLER_VIEW;
   bgra_render =
      chrome9_screen_has_capability(
         screen, DRM_CHROME9_CAP_BLEND_BGRA8888) &&
      chrome9_screen_has_capability(
         screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND) &&
      (!chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE) ||
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND));
   if (bgra_render)
      bgra_bindings |= PIPE_BIND_RENDER_TARGET;
   return !(bindings & ~bgra_bindings);
}

bool
chrome9_is_format_supported(struct pipe_screen *screen,
                            enum pipe_format format,
                            enum pipe_texture_target target,
                            unsigned sample_count,
                            unsigned storage_sample_count,
                            unsigned bindings)
{
   bool supported = chrome9_format_supported(screen, format, target,
                                             sample_count,
                                             storage_sample_count, bindings);

   if (getenv("CHROME9_DEBUG_FORMATS"))
      fprintf(stderr,
              "chrome9: format %s target=%u samples=%u/%u bind=%#x: %s\n",
              util_format_name(format), target, sample_count,
              storage_sample_count, bindings,
              supported ? "supported" : "rejected");
   return supported;
}

bool
chrome9_can_create_resource(struct pipe_screen *screen,
                            const struct pipe_resource *templ)
{
   struct chrome9_resource_layout layout;

   if (getenv("CHROME9_DEBUG_RESOURCES"))
      fprintf(stderr,
              "chrome9: can-create target=%u format=%s %ux%ux%u array=%u "
              "last=%u samples=%u/%u bind=%#x\n",
              templ->target, util_format_name(templ->format), templ->width0,
              templ->height0, templ->depth0, templ->array_size,
              templ->last_level, templ->nr_samples,
              templ->nr_storage_samples, templ->bind);

   if (templ->target != PIPE_BUFFER &&
       !chrome9_is_format_supported(screen, templ->format, templ->target,
                                    templ->nr_samples,
                                    templ->nr_storage_samples, templ->bind))
      return false;
   return chrome9_resource_layout(templ, &layout);
}

struct pipe_resource *
chrome9_resource_create(struct pipe_screen *screen,
                        const struct pipe_resource *templ)
{
   struct chrome9_resource *resource;
   struct chrome9_resource_layout layout;
   int fd;

   if (getenv("CHROME9_DEBUG_RESOURCES"))
      fprintf(stderr,
              "chrome9: resource target=%u format=%u %ux%ux%u array=%u "
              "last=%u samples=%u/%u bind=%#x\n",
              templ->target, templ->format, templ->width0, templ->height0,
              templ->depth0, templ->array_size, templ->last_level,
              templ->nr_samples, templ->nr_storage_samples, templ->bind);

   resource = CALLOC_STRUCT(chrome9_resource);
   if (!resource)
      return NULL;
   if (!chrome9_can_create_resource(screen, templ) ||
       !chrome9_resource_layout(templ, &layout))
      goto fail;

   fd = screen->get_screen_fd(screen);
   if (fd < 0 ||
       !chrome9_bo_create(fd, layout.allocation_size, &resource->bo))
      goto fail;

   chrome9_resource_apply_layout(resource, &layout);
   resource->base = *templ;
   resource->base.screen = screen;
   pipe_reference_init(&resource->base.reference, 1);
   return &resource->base;

fail:
   FREE(resource);
   return NULL;
}

static struct pipe_resource *
chrome9_resource_from_handle(struct pipe_screen *screen,
                             const struct pipe_resource *templ,
                             struct winsys_handle *whandle, unsigned usage)
{
   struct chrome9_resource *resource;
   struct chrome9_resource_layout layout;
   uint64_t data_size;
   int fd;

   (void)usage;
   if (!whandle || whandle->type != WINSYS_HANDLE_TYPE_FD ||
       (whandle->modifier != DRM_FORMAT_MOD_LINEAR &&
        whandle->modifier != DRM_FORMAT_MOD_INVALID) ||
       !chrome9_resource_layout(templ, &layout) || templ->last_level ||
       templ->target == PIPE_BUFFER ||
       (templ->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        templ->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       whandle->stride < templ->width0 * 4 ||
       whandle->stride > 16352 || (whandle->stride & 31))
      return NULL;

   resource = CALLOC_STRUCT(chrome9_resource);
   if (!resource)
      return NULL;
   fd = screen->get_screen_fd(screen);
   if (fd < 0 || !chrome9_bo_import_fd(fd, whandle->handle, &resource->bo))
      goto fail;
   data_size = (uint64_t)whandle->stride * templ->height0;
   if (whandle->offset > resource->bo.size ||
       data_size > resource->bo.size - whandle->offset)
      goto fail_bo;

   resource->base = *templ;
   resource->base.screen = screen;
   pipe_reference_init(&resource->base.reference, 1);
   /* A native Chrome9 DRI3 export retains the complete canonical guarded
    * allocation.  Preserve that fact after same-device PRIME import so the
    * consumer can submit the BO directly and share its reservation object;
    * this avoids a stale userspace sampler copy.  Every tuple which is not
    * exact remains an ordinary unguarded external resource. */
   if (whandle->stride == layout.level_stride[0] &&
       whandle->offset == layout.level_offset[0] &&
       resource->bo.size == layout.allocation_size) {
      chrome9_resource_apply_layout(resource, &layout);
   } else {
      resource->stride = whandle->stride;
      resource->data_offset = whandle->offset;
      resource->data_size = data_size;
      resource->level_stride[0] = resource->stride;
      resource->level_offset[0] = resource->data_offset;
      resource->level_size[0] = resource->data_size;
      resource->guarded = false;
   }
   return &resource->base;

fail_bo:
   chrome9_bo_destroy(fd, &resource->bo);
fail:
   FREE(resource);
   return NULL;
}

static bool
chrome9_resource_get_handle(struct pipe_screen *screen,
                            struct pipe_context *context,
                            struct pipe_resource *pres,
                            struct winsys_handle *whandle, unsigned usage)
{
   struct chrome9_resource *resource = chrome9_resource(pres);
   bool kms;
   int prime_fd;

   (void)context;
   (void)usage;
   kms = whandle && whandle->type == WINSYS_HANDLE_TYPE_KMS;
   if (!whandle || pres->target == PIPE_BUFFER ||
       pres->last_level ||
       (pres->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        pres->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       whandle->plane ||
       whandle->layer ||
       (!kms && whandle->type != WINSYS_HANDLE_TYPE_FD) ||
       !resource->bo.handle)
      return false;

   if (kms) {
      /* KMS handles stay owned by this resource and are valid only on the
       * screen's DRM file.  PRIME FD export retains its separate ownership
       * transfer below. */
      whandle->handle = resource->bo.handle;
   } else {
      if (!chrome9_bo_export_fd(screen->get_screen_fd(screen), &resource->bo,
                                &prime_fd))
         return false;
      whandle->handle = prime_fd;
   }
   whandle->stride = resource->stride;
   whandle->offset = resource->data_offset;
   whandle->modifier = DRM_FORMAT_MOD_LINEAR;
   return true;
}

void
chrome9_resource_destroy(struct pipe_screen *screen,
                         struct pipe_resource *resource)
{
   struct chrome9_resource *cresource = chrome9_resource(resource);

   chrome9_bo_destroy(screen->get_screen_fd(screen), &cresource->bo);
   FREE(cresource);
}

static bool
chrome9_transfer_box_valid(const struct pipe_resource *resource,
                           unsigned level, const struct pipe_box *box)
{
   unsigned width;
   unsigned height;

   if (level > resource->last_level || !box || box->x < 0 || box->y < 0 ||
       box->z < 0 ||
       box->width < 0 || box->height < 0 || box->depth < 0)
      return false;

   if (resource->target == PIPE_BUFFER)
      return level == 0 && box->y == 0 && box->z == 0 && box->height == 1 &&
             box->depth == 1 && (unsigned)box->x <= resource->width0 &&
             (unsigned)box->width <= resource->width0 - (unsigned)box->x;

   width = u_minify(resource->width0, level);
   height = u_minify(resource->height0, level);
   return chrome9_is_surface_target(resource->target) && box->z == 0 &&
          box->depth == 1 && (unsigned)box->x <= width &&
          (unsigned)box->width <= width - (unsigned)box->x &&
          (unsigned)box->y <= height &&
          (unsigned)box->height <= height - (unsigned)box->y;
}

static void *
chrome9_transfer_map(struct pipe_context *pipe, struct pipe_resource *resource,
                     unsigned level, unsigned usage,
                     const struct pipe_box *box,
                     struct pipe_transfer **out_transfer)
{
   struct chrome9_resource *cresource = chrome9_resource(resource);
   struct pipe_transfer *transfer;
   uint64_t offset;

   (void)pipe;
   if (!out_transfer || !chrome9_transfer_box_valid(resource, level, box))
      return NULL;

   transfer = CALLOC_STRUCT(pipe_transfer);
   if (!transfer)
      return NULL;
   pipe_resource_reference(&transfer->resource, resource);
   transfer->level = level;
   transfer->usage = usage;
   transfer->box = *box;

   if (resource->target == PIPE_BUFFER) {
      offset = box->x;
   } else {
      transfer->stride = cresource->level_stride[level];
      transfer->layer_stride = cresource->level_size[level];
      const unsigned cpp = resource->format == PIPE_FORMAT_Z16_UNORM ? 2 : 4;

      offset = cresource->level_offset[level] +
               (uint64_t)box->y * cresource->level_stride[level] +
               (uint64_t)box->x * cpp;
   }
   if (offset > cresource->bo.size) {
      pipe_resource_reference(&transfer->resource, NULL);
      FREE(transfer);
      return NULL;
   }
   *out_transfer = transfer;
   return (uint8_t *)cresource->bo.map + offset;
}

static void
chrome9_transfer_unmap(struct pipe_context *pipe,
                       struct pipe_transfer *transfer)
{
   (void)pipe;
   if (!transfer)
      return;
   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}

void
chrome9_init_screen_resource_functions(struct pipe_screen *screen)
{
   screen->is_format_supported = chrome9_is_format_supported;
   screen->can_create_resource = chrome9_can_create_resource;
   screen->resource_create = chrome9_resource_create;
   screen->resource_from_handle = chrome9_resource_from_handle;
   screen->resource_get_handle = chrome9_resource_get_handle;
   screen->resource_destroy = chrome9_resource_destroy;
}

void
chrome9_init_context_resource_functions(struct pipe_context *pipe)
{
   pipe->buffer_map = chrome9_transfer_map;
   pipe->buffer_unmap = chrome9_transfer_unmap;
   pipe->texture_map = chrome9_transfer_map;
   pipe->texture_unmap = chrome9_transfer_unmap;
   pipe->transfer_flush_region = u_default_transfer_flush_region;
   pipe->buffer_subdata = u_default_buffer_subdata;
   pipe->texture_subdata = u_default_texture_subdata;
}
