/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_resource.h"

#include "chrome9_format.h"
#include "chrome9_context.h"
#include "chrome9_draw.h"
#include "chrome9_public.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
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

   cpp = chrome9_format_storage_cpp(templ->format);
   if (!chrome9_is_surface_target(templ->target) ||
       !cpp ||
       (chrome9_format_is_d24(templ->format) &&
        (templ->target != PIPE_TEXTURE_2D || templ->last_level)) ||
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

   for (unsigned level = 0; level <= templ->last_level; level++) {
      const unsigned width = u_minify(templ->width0, level);
      const unsigned height = u_minify(templ->height0, level);

      layout->level_stride[level] = ALIGN_POT(width * cpp, 32);
      layout->level_size[level] =
         (uint64_t)layout->level_stride[level] * height;
   }

   /* Cross-process image paths do not reliably preserve a non-zero plane
    * offset when they fall back from DRI3 modifiers to the legacy single-FD
    * protocol.  Keep shared level-zero images conventional: pixels begin at
    * byte zero and the allocation is page-rounded for KMS/GEM.  Such images
    * are deliberately unguarded and must render/sample through the guarded
    * shadows in chrome9_context.c; they must never reach a draw ioctl
    * directly.  Private resources retain the exact BO+PAGE kernel ABI below.
    */
   if (templ->bind & PIPE_BIND_SHARED) {
      if (templ->last_level ||
          !chrome9_u64_align(layout->level_size[0], page_size,
                             &layout->allocation_size) ||
          layout->allocation_size > CHROME9_MAX_BUFFER_SIZE)
         return false;
      layout->level_offset[0] = 0;
      layout->guarded = false;
      return true;
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
   const unsigned sampler_bindings = PIPE_BIND_SAMPLER_VIEW |
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
   if (format == PIPE_FORMAT_X8Z24_UNORM)
      return target == PIPE_TEXTURE_2D &&
             /* Enable only for guarded validation until the complete
              * Gallium FBO/draw/clear/rebind path passes on silicon. */
             debug_get_bool_option("CHROME9_EXPERIMENTAL_X8Z24", false) &&
             chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM) &&
             !(bindings & ~depth_bindings);
   /* Stencil shares the D24 gate until its guarded silicon oracles pass. */
   if (format == PIPE_FORMAT_S8_UINT_Z24_UNORM)
      return target == PIPE_TEXTURE_2D &&
             debug_get_bool_option("CHROME9_EXPERIMENTAL_X8Z24", false) &&
             chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_FRAGMENT_STENCIL) &&
             !(bindings & ~depth_bindings);
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
   if (format == PIPE_FORMAT_A8_UNORM || format == PIPE_FORMAT_L8_UNORM ||
       format == PIPE_FORMAT_L8A8_UNORM ||
       format == PIPE_FORMAT_B5G6R5_UNORM)
      return chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM |
                        DRM_CHROME9_CAP_FRAGMENT_TEXTURE_LEGACY_FORMATS) &&
             !(bindings & ~sampler_bindings);
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
   uint32_t create_flags = 0;
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

   /* Only the exported, offset-zero KMS allocation needs constrained pages.
    * Keeping private guarded render/depth resources in ordinary memory avoids
    * consuming DMA32 for data which never becomes the persistent root scanout.
    */
   if (!layout.guarded && (templ->bind & PIPE_BIND_SCANOUT) &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_GEM_DMA32_SCANOUT))
      create_flags = DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT;

   fd = screen->get_screen_fd(screen);
   if (fd < 0 ||
       !chrome9_bo_create(fd, layout.allocation_size, create_flags,
                          &resource->bo))
      goto fail;

   chrome9_resource_apply_layout(resource, &layout);
   resource->base = *templ;
   resource->base.screen = screen;
   pipe_reference_init(&resource->base.reference, 1);
   /* Surfaces opt in to deferred cache maintenance; buffers are never
    * written by the device.  Shared surfaces stay in the protocol because
    * every importer on this device is this driver; the kernel reverts an
    * object that becomes a KMS framebuffer. */
   if (templ->target != PIPE_BUFFER &&
       !(templ->bind & PIPE_BIND_DISPLAY_TARGET) &&
       chrome9_screen_has_capability(screen,
                                     DRM_CHROME9_CAP_GEM_CPU_ACCESS) &&
       debug_get_bool_option("CHROME9_EXPLICIT_ACCESS", true))
      resource->explicit_access =
         chrome9_bo_cpu_access(fd, &resource->bo,
                               DRM_CHROME9_GEM_CPU_EXPLICIT);
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
   if (fd < 0 ||
       !chrome9_screen_bo_import_fd(screen, whandle->handle, &resource->bo))
      goto fail;
   data_size = (uint64_t)whandle->stride * templ->height0;
   if (whandle->offset > resource->bo.size ||
       data_size > resource->bo.size - whandle->offset)
      goto fail_bo;

   resource->base = *templ;
   resource->base.screen = screen;
   pipe_reference_init(&resource->base.reference, 1);
   /* The exporter may have opted the object in: bracket CPU accesses (a
    * no-op for implicit objects). */
   resource->explicit_access = true;
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
   chrome9_screen_bo_destroy(screen, &resource->bo);
fail:
   FREE(resource);
   return NULL;
}

/* Announce CPU access to bytes [offset, offset + size) of the BO (size 0:
 * the whole object). */
void
chrome9_resource_cpu_access_range(struct chrome9_resource *resource,
                                  bool write, uint64_t offset, uint64_t size)
{
   struct pipe_screen *screen = resource->base.screen;

   if (write)
      resource->cpu_write_generation++;
   if (resource->explicit_access &&
       !chrome9_bo_cpu_access_range(screen->get_screen_fd(screen),
                                    &resource->bo,
                                    write ? DRM_CHROME9_GEM_CPU_PREPARE_WRITE :
                                            DRM_CHROME9_GEM_CPU_PREPARE_READ,
                                    offset, size))
      fprintf(stderr, "chrome9: GEM_CPU_ACCESS failed: %s\n", strerror(errno));
}

void
chrome9_resource_cpu_access(struct chrome9_resource *resource, bool write)
{
   chrome9_resource_cpu_access_range(resource, write, 0, 0);
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
      if (!chrome9_screen_bo_export_fd(screen, &resource->bo, &prime_fd))
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

   chrome9_screen_bo_destroy(screen, &cresource->bo);
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

   if (!out_transfer || !chrome9_transfer_box_valid(resource, level, box))
      return NULL;

   /* Retained triangle batches and the shadow color target are not visible
    * through a direct persistent mapping until submitted and resolved. */
   if (resource->target != PIPE_BUFFER)
      chrome9_context_sync_resource(chrome9_context(pipe), resource, usage);
   else if (!(usage & PIPE_MAP_UNSYNCHRONIZED))
      /* Queued draws may still read the buffer. */
      chrome9_tcl_batch_flush_resource(chrome9_context(pipe), resource);
   chrome9_resource_cpu_access(cresource, usage & PIPE_MAP_WRITE);

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
      const unsigned cpp = chrome9_format_storage_cpp(resource->format);

      if (!cpp) {
         pipe_resource_reference(&transfer->resource, NULL);
         FREE(transfer);
         return NULL;
      }

      transfer->stride = cresource->level_stride[level];
      transfer->layer_stride = cresource->level_size[level];

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

struct chrome9_mipmap_mapping {
   uint8_t *data;
   struct pipe_transfer *transfer;
   unsigned stride;
};

static bool
chrome9_mipmap_level_valid(const struct chrome9_resource *resource,
                           unsigned level, unsigned cpp)
{
   const unsigned width = u_minify(resource->base.width0, level);
   const unsigned height = u_minify(resource->base.height0, level);

   return resource->level_stride[level] >= width * cpp &&
          resource->level_size[level] >=
             (uint64_t)resource->level_stride[level] * height &&
          resource->level_offset[level] <= resource->bo.size &&
          resource->level_size[level] <=
             resource->bo.size - resource->level_offset[level];
}

static bool
chrome9_mipmap_map_level(struct pipe_context *pipe,
                         struct chrome9_resource *resource, unsigned level,
                         unsigned usage, unsigned cpp,
                         struct chrome9_mipmap_mapping *mapping)
{
   const unsigned width = u_minify(resource->base.width0, level);
   const unsigned height = u_minify(resource->base.height0, level);
   const struct pipe_box box = {
      .width = width,
      .height = height,
      .depth = 1,
   };

   memset(mapping, 0, sizeof(*mapping));
   mapping->data = pipe->texture_map(pipe, &resource->base, level, usage,
                                     &box, &mapping->transfer);
   if (mapping->data) {
      if (mapping->transfer && mapping->transfer->stride >= width * cpp) {
         mapping->stride = mapping->transfer->stride;
         return true;
      }
      if (mapping->transfer)
         pipe->texture_unmap(pipe, mapping->transfer);
      memset(mapping, 0, sizeof(*mapping));
   }

   /* Chrome9 BOs are persistently mapped.  Retain a complete CPU path if
    * allocating the small pipe_transfer object fails: returning false here
    * would make the state tracker enter util_gen_mipmap(), while this bounded
    * driver intentionally has no generic blit callback yet. */
   if (!resource->bo.map ||
       !chrome9_mipmap_level_valid(resource, level, cpp))
      return false;
   mapping->data = (uint8_t *)resource->bo.map +
                   resource->level_offset[level];
   mapping->stride = resource->level_stride[level];
   return true;
}

static void
chrome9_mipmap_unmap_level(struct pipe_context *pipe,
                           struct chrome9_mipmap_mapping *mapping)
{
   if (mapping->transfer)
      pipe->texture_unmap(pipe, mapping->transfer);
   memset(mapping, 0, sizeof(*mapping));
}

static void
chrome9_mipmap_bgra_level(enum pipe_format format,
                          const struct chrome9_mipmap_mapping *source,
                          unsigned source_width, unsigned source_height,
                          const struct chrome9_mipmap_mapping *destination,
                          unsigned destination_width,
                          unsigned destination_height)
{
   for (unsigned y = 0; y < destination_height; y++) {
      const unsigned y0 = source_height == 1 ? 0 : y * 2;
      const unsigned y1 = source_height == 1 ? 0 : y0 + 1;
      const uint8_t *rows[2] = {
         source->data + (uint64_t)y0 * source->stride,
         source->data + (uint64_t)y1 * source->stride,
      };
      uint8_t *dst = destination->data + (uint64_t)y * destination->stride;

      for (unsigned x = 0; x < destination_width; x++, dst += 4) {
         const unsigned x0 = source_width == 1 ? 0 : x * 2;
         const unsigned x1 = source_width == 1 ? 0 : x0 + 1;

         for (unsigned channel = 0; channel < 3; channel++)
            dst[channel] =
               ((unsigned)rows[0][x0 * 4 + channel] +
                (unsigned)rows[0][x1 * 4 + channel] +
                (unsigned)rows[1][x0 * 4 + channel] +
                (unsigned)rows[1][x1 * 4 + channel]) / 4;
         if (format == PIPE_FORMAT_B8G8R8A8_UNORM)
            dst[3] = ((unsigned)rows[0][x0 * 4 + 3] +
                      (unsigned)rows[0][x1 * 4 + 3] +
                      (unsigned)rows[1][x0 * 4 + 3] +
                      (unsigned)rows[1][x1 * 4 + 3]) / 4;
         else
            dst[3] = 0xff;
      }
   }
}

static void
chrome9_mipmap_z16_level(const struct chrome9_mipmap_mapping *source,
                         unsigned source_width, unsigned source_height,
                         const struct chrome9_mipmap_mapping *destination,
                         unsigned destination_width,
                         unsigned destination_height)
{
   for (unsigned y = 0; y < destination_height; y++) {
      const unsigned y0 = source_height == 1 ? 0 : y * 2;
      const unsigned y1 = source_height == 1 ? 0 : y0 + 1;
      const uint16_t *row0 =
         (const uint16_t *)(source->data + (uint64_t)y0 * source->stride);
      const uint16_t *row1 =
         (const uint16_t *)(source->data + (uint64_t)y1 * source->stride);
      uint16_t *dst = (uint16_t *)(destination->data +
                                   (uint64_t)y * destination->stride);

      for (unsigned x = 0; x < destination_width; x++) {
         const unsigned x0 = source_width == 1 ? 0 : x * 2;
         const unsigned x1 = source_width == 1 ? 0 : x0 + 1;

         dst[x] = ((uint32_t)row0[x0] + row0[x1] +
                   row1[x0] + row1[x1]) / 4;
      }
   }
}

static void
chrome9_mipmap_packed_level(enum pipe_format format, unsigned cpp,
                            const struct chrome9_mipmap_mapping *source,
                            unsigned source_width, unsigned source_height,
                            const struct chrome9_mipmap_mapping *destination,
                            unsigned destination_width,
                            unsigned destination_height)
{
   for (unsigned y = 0; y < destination_height; y++) {
      const unsigned y0 = source_height == 1 ? 0 : y * 2;
      const unsigned y1 = source_height == 1 ? 0 : y0 + 1;
      const uint8_t *rows[2] = {
         source->data + (uint64_t)y0 * source->stride,
         source->data + (uint64_t)y1 * source->stride,
      };
      uint8_t *dst = destination->data + (uint64_t)y * destination->stride;

      for (unsigned x = 0; x < destination_width; x++, dst += cpp) {
         const unsigned x0 = source_width == 1 ? 0 : x * 2;
         const unsigned x1 = source_width == 1 ? 0 : x0 + 1;
         float texels[4][4];
         float result[4];

         util_format_unpack_rgba(format, texels[0], rows[0] + x0 * cpp, 1);
         util_format_unpack_rgba(format, texels[1], rows[0] + x1 * cpp, 1);
         util_format_unpack_rgba(format, texels[2], rows[1] + x0 * cpp, 1);
         util_format_unpack_rgba(format, texels[3], rows[1] + x1 * cpp, 1);
         for (unsigned channel = 0; channel < 4; channel++)
            result[channel] = (texels[0][channel] + texels[1][channel] +
                               texels[2][channel] + texels[3][channel]) *
                              0.25f;
         util_format_pack_rgba(format, dst, result, 1);
      }
   }
}

bool
chrome9_generate_mipmap(struct pipe_context *pipe,
                        struct pipe_resource *pres,
                        enum pipe_format format,
                        unsigned base_level, unsigned last_level,
                        unsigned first_layer, unsigned last_layer)
{
   struct chrome9_resource *resource;
   const struct chrome9_texture_format_info *format_info;
   unsigned cpp;

   format_info = chrome9_texture_format_info(format);
   if (!pipe || !pres || !pipe->texture_map || !pipe->texture_unmap ||
       pres->target != PIPE_TEXTURE_2D || pres->depth0 != 1 ||
       pres->array_size != 1 || pres->nr_samples > 1 ||
       pres->nr_storage_samples > 1 || format != pres->format ||
       format == PIPE_FORMAT_X8Z24_UNORM ||
       (!format_info && format != PIPE_FORMAT_Z16_UNORM) ||
       base_level > last_level || last_level > pres->last_level ||
       first_layer || last_layer)
      return false;

   resource = chrome9_resource(pres);
   cpp = format_info ? format_info->bytes_per_pixel : 2;
   if (!resource->guarded || !resource->bo.map)
      return false;
   for (unsigned level = base_level; level <= last_level; level++)
      if (!chrome9_mipmap_level_valid(resource, level, cpp))
         return false;

   /* The first level may still be queued in a retained GPU draw batch. */
   chrome9_draw_flush_batch(chrome9_context(pipe));
   chrome9_resource_cpu_access(resource, true);

   for (unsigned level = base_level + 1; level <= last_level; level++) {
      const unsigned source_width = u_minify(pres->width0, level - 1);
      const unsigned source_height = u_minify(pres->height0, level - 1);
      const unsigned destination_width = u_minify(pres->width0, level);
      const unsigned destination_height = u_minify(pres->height0, level);
      struct chrome9_mipmap_mapping source;
      struct chrome9_mipmap_mapping destination;

      if (!chrome9_mipmap_map_level(pipe, resource, level - 1,
                                    PIPE_MAP_READ, cpp, &source))
         return false;
      if (!chrome9_mipmap_map_level(pipe, resource, level, PIPE_MAP_WRITE,
                                    cpp, &destination)) {
         chrome9_mipmap_unmap_level(pipe, &source);
         return false;
      }

      if (format == PIPE_FORMAT_Z16_UNORM)
         chrome9_mipmap_z16_level(&source, source_width, source_height,
                                  &destination, destination_width,
                                  destination_height);
      else if (cpp == 4)
         chrome9_mipmap_bgra_level(format, &source, source_width,
                                   source_height, &destination,
                                   destination_width, destination_height);
      else
         chrome9_mipmap_packed_level(format, cpp, &source, source_width,
                                     source_height, &destination,
                                     destination_width, destination_height);
      chrome9_mipmap_unmap_level(pipe, &destination);
      chrome9_mipmap_unmap_level(pipe, &source);
   }
   return true;
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
   pipe->generate_mipmap = chrome9_generate_mipmap;
   pipe->resource_copy_region = util_resource_copy_region;
   pipe->transfer_flush_region = u_default_transfer_flush_region;
   pipe->buffer_subdata = u_default_buffer_subdata;
   pipe->texture_subdata = u_default_texture_subdata;
}
