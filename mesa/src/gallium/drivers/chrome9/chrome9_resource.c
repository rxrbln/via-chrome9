/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_resource.h"

#include "chrome9_public.h"

#include <limits.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"

#define CHROME9_MAX_BUFFER_SIZE (256ULL * 1024 * 1024)
#define CHROME9_MAX_SURFACE_DIMENSION 2048

static bool
chrome9_resource_layout(const struct pipe_resource *templ, uint32_t *stride,
                        uint64_t *data_offset, uint64_t *data_size,
                        uint64_t *allocation_size, bool *guarded)
{
   long page_size;
   uint64_t aligned_size;

   if (templ->target == PIPE_BUFFER) {
      if (!templ->width0 || templ->width0 > CHROME9_MAX_BUFFER_SIZE)
         return false;
      *stride = 0;
      *data_offset = 0;
      *data_size = templ->width0;
      *allocation_size = templ->width0;
      *guarded = false;
      return true;
   }

   if (templ->target != PIPE_TEXTURE_2D ||
       (templ->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        templ->format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        templ->format != PIPE_FORMAT_Z16_UNORM) ||
       !templ->width0 || templ->width0 > CHROME9_MAX_SURFACE_DIMENSION ||
       !templ->height0 || templ->height0 > CHROME9_MAX_SURFACE_DIMENSION ||
       templ->depth0 != 1 || templ->array_size != 1 || templ->last_level ||
       templ->nr_samples > 1 || templ->nr_storage_samples > 1)
      return false;

   page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 || !util_is_power_of_two_or_zero(page_size) ||
       page_size > UINT32_MAX)
      return false;

   *stride = ALIGN_POT(templ->width0 *
                          (templ->format == PIPE_FORMAT_Z16_UNORM ? 2 : 4),
                       32);
   *data_size = (uint64_t)*stride * templ->height0;
   aligned_size = ALIGN_POT(*data_size, (uint64_t)page_size);
   if (aligned_size > UINT64_MAX - 2 * (uint64_t)page_size)
      return false;
   *data_offset = page_size;
   *allocation_size = aligned_size + 2 * (uint64_t)page_size;
   *guarded = true;
   return true;
}

bool
chrome9_is_format_supported(struct pipe_screen *screen,
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
   const unsigned depth_bindings = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR;
   unsigned bgra_bindings = PIPE_BIND_LINEAR;
   bool bgra_render;

   if (sample_count > 1 || storage_sample_count > 1)
      return false;
   if (target == PIPE_BUFFER)
      return !(bindings & ~buffer_bindings);
   if ((bindings & PIPE_BIND_SAMPLER_VIEW) &&
       !chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE))
      return false;
   if (target == PIPE_TEXTURE_2D && format == PIPE_FORMAT_Z16_UNORM)
      return chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_TRIANGLE_D16) &&
             !(bindings & ~depth_bindings);
   if (target != PIPE_TEXTURE_2D)
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
chrome9_can_create_resource(struct pipe_screen *screen,
                            const struct pipe_resource *templ)
{
   uint64_t allocation_size;
   uint64_t data_offset;
   uint64_t data_size;
   uint32_t stride;
   bool guarded;

   if (templ->target != PIPE_BUFFER &&
       !chrome9_is_format_supported(screen, templ->format, templ->target,
                                    templ->nr_samples,
                                    templ->nr_storage_samples, templ->bind))
      return false;
   return chrome9_resource_layout(templ, &stride, &data_offset, &data_size,
                                  &allocation_size, &guarded);
}

struct pipe_resource *
chrome9_resource_create(struct pipe_screen *screen,
                        const struct pipe_resource *templ)
{
   struct chrome9_resource *resource;
   uint64_t allocation_size;
   int fd;

   resource = CALLOC_STRUCT(chrome9_resource);
   if (!resource)
      return NULL;
   if (!chrome9_can_create_resource(screen, templ) ||
       !chrome9_resource_layout(templ, &resource->stride,
                                &resource->data_offset,
                                &resource->data_size, &allocation_size,
                                &resource->guarded))
      goto fail;

   fd = screen->get_screen_fd(screen);
   if (fd < 0 || !chrome9_bo_create(fd, allocation_size, &resource->bo))
      goto fail;

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
   uint64_t allocation_size;
   uint64_t data_offset;
   uint64_t data_size;
   uint32_t native_stride;
   bool guarded;
   int fd;

   (void)usage;
   if (!whandle || whandle->type != WINSYS_HANDLE_TYPE_FD ||
       (whandle->modifier != DRM_FORMAT_MOD_LINEAR &&
        whandle->modifier != DRM_FORMAT_MOD_INVALID) ||
       !chrome9_resource_layout(templ, &native_stride, &data_offset,
                                &data_size, &allocation_size, &guarded) ||
       templ->target == PIPE_BUFFER ||
       templ->format != PIPE_FORMAT_B8G8R8X8_UNORM ||
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
   resource->stride = whandle->stride;
   resource->data_offset = whandle->offset;
   resource->data_size = data_size;
   resource->guarded = false;
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
   int prime_fd;

   (void)context;
   (void)usage;
   if (!whandle || pres->target == PIPE_BUFFER ||
       pres->format != PIPE_FORMAT_B8G8R8X8_UNORM || whandle->plane ||
       whandle->layer || whandle->type != WINSYS_HANDLE_TYPE_FD ||
       !chrome9_bo_export_fd(screen->get_screen_fd(screen), &resource->bo,
                             &prime_fd))
      return false;

   whandle->handle = prime_fd;
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
   if (level || !box || box->x < 0 || box->y < 0 || box->z < 0 ||
       box->width < 0 || box->height < 0 || box->depth < 0)
      return false;

   if (resource->target == PIPE_BUFFER)
      return box->y == 0 && box->z == 0 && box->height == 1 &&
             box->depth == 1 && (unsigned)box->x <= resource->width0 &&
             (unsigned)box->width <= resource->width0 - (unsigned)box->x;

   return resource->target == PIPE_TEXTURE_2D && box->z == 0 &&
          box->depth == 1 && (unsigned)box->x <= resource->width0 &&
          (unsigned)box->width <= resource->width0 - (unsigned)box->x &&
          (unsigned)box->y <= resource->height0 &&
          (unsigned)box->height <= resource->height0 - (unsigned)box->y;
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
      transfer->stride = cresource->stride;
      transfer->layer_stride = (uint64_t)cresource->stride * resource->height0;
      const unsigned cpp = resource->format == PIPE_FORMAT_Z16_UNORM ? 2 : 4;

      offset = cresource->data_offset + (uint64_t)box->y * cresource->stride +
               (uint64_t)box->x * cpp;
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
