/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_resource.h"

#include "chrome9_format.h"
#include "chrome9_context.h"
#include "chrome9_draw.h"
#include "chrome9_public.h"
#include "chrome9_swfb.h"

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
#include "util/streaming-load-memcpy.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_atomic.h"

static uint64_t chrome9_resource_serial;

#define CHROME9_MAX_BUFFER_SIZE (256ULL * 1024 * 1024)
#define CHROME9_MAX_SURFACE_DIMENSION 2048
#define CHROME9_MIP_ALIGNMENT 256
/* The level table holds eight successors of each face root. */
#define CHROME9_CUBE_MAX_LEVELS 9
/* A21 carries at most 2048 slices. */
#define CHROME9_MAX_VOLUME_DEPTH 2048

struct chrome9_resource_layout {
   uint32_t level_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_offset[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_size[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t layer_stride;
   uint64_t allocation_size;
   bool guarded;
   bool sampler_unsupported;
   /* Colour levels in 32-byte x 8-row tiles (ALIGN(h, 8) rows each). */
   bool tiled;
};

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

/*
 * Vendor cube layout (sfmCalcMipSize_inv, sfmCalcCubeTextureOffset_inv):
 * levels of one face packed with ALIGN(h, 8) rows, the face stride is
 * their 256-byte aligned sum, faces follow the leading guard page.
 */
static bool
chrome9_resource_cube_layout(const struct pipe_resource *templ,
                             uint64_t page_size,
                             struct chrome9_resource_layout *layout)
{
   uint64_t face = 0;

   /* DXT faces (llvmpipe only) hold their block rows. */
   const bool blocks = util_format_is_compressed(templ->format);

   for (unsigned level = 0; level <= templ->last_level; level++) {
      layout->level_offset[level] = page_size + face;
      face += blocks ? ALIGN_POT(layout->level_size[level],
                                 CHROME9_MIP_ALIGNMENT) :
              (uint64_t)layout->level_stride[level] *
              ALIGN_POT(u_minify(templ->height0, level), 8);
   }
   layout->layer_stride = ALIGN_POT(face, CHROME9_MIP_ALIGNMENT);
   layout->allocation_size = ALIGN_POT(page_size + 6 * layout->layer_stride,
                                       page_size) + page_size;
   layout->guarded = true;
   return layout->allocation_size <= CHROME9_MAX_BUFFER_SIZE;
}

/*
 * Volume layout (sfm3DTextureBlt_inv): the slices of a level follow each
 * other at the level's image size, levels follow their predecessor.  The
 * sampler reads level zero; lower levels serve llvmpipe and transfers.
 */
static bool
chrome9_resource_volume_layout(const struct pipe_resource *templ,
                               uint64_t page_size,
                               struct chrome9_resource_layout *layout)
{
   uint64_t cursor = page_size;
   uint64_t size;

   for (unsigned level = 0; level <= templ->last_level; level++) {
      layout->level_offset[level] = cursor;
      size = layout->level_size[level] * u_minify(templ->depth0, level);
      if (!chrome9_u64_add(cursor, size, &cursor))
         return false;
   }
   if (!chrome9_u64_align(cursor, page_size, &cursor) ||
       !chrome9_u64_add(cursor, page_size, &cursor) ||
       cursor > CHROME9_MAX_BUFFER_SIZE)
      return false;
   layout->allocation_size = cursor;
   layout->guarded = true;
   return true;
}

/* Shared window back buffers are tiled and marked GEM_CREATE_TILED. */
static bool
chrome9_tiled_back_buffer(struct pipe_screen *screen,
                          const struct pipe_resource *templ)
{
   return (templ->bind & PIPE_BIND_BACKBUFFER) &&
          (templ->bind & PIPE_BIND_SHARED) &&
          chrome9_screen_has_capability2(screen,
                                         DRM_CHROME9_CAP2_TILED_SHARED) &&
          debug_get_bool_option("CHROME9_TILED_SHARED", true);
}

static bool
chrome9_resource_layout(struct pipe_screen *screen,
                        const struct pipe_resource *templ,
                        struct chrome9_resource_layout *layout)
{
   const bool cube = templ->target == PIPE_TEXTURE_CUBE;
   const bool volume = templ->target == PIPE_TEXTURE_3D;
   const bool dxt = chrome9_format_is_dxt(templ->format);
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
   if ((!chrome9_is_surface_target(templ->target) && !cube && !volume) ||
       !cpp ||
       (chrome9_format_is_d24(templ->format) &&
        !chrome9_is_surface_target(templ->target)) ||
       (dxt && templ->target != PIPE_TEXTURE_2D && !cube) ||
       (volume && (!templ->depth0 ||
                   templ->depth0 > CHROME9_MAX_VOLUME_DEPTH ||
                   templ->last_level >
                      util_logbase2(MAX3(templ->width0, templ->height0,
                                         templ->depth0)))) ||
       /* NPOT cubes exist for llvmpipe (sw_only views). */
       (cube && (templ->width0 != templ->height0 ||
                 templ->last_level >= CHROME9_CUBE_MAX_LEVELS)) ||
       !templ->width0 || templ->width0 > CHROME9_MAX_SURFACE_DIMENSION ||
       !templ->height0 || templ->height0 > CHROME9_MAX_SURFACE_DIMENSION ||
       (!volume && templ->depth0 != 1) ||
       templ->array_size != (cube ? 6 : 1) ||
       templ->last_level >= PIPE_MAX_TEXTURE_LEVELS ||
       (templ->target == PIPE_TEXTURE_RECT && templ->last_level) ||
       (!volume &&
        templ->last_level > util_logbase2(MAX2(templ->width0,
                                               templ->height0))) ||
       templ->nr_samples > 1 || templ->nr_storage_samples > 1)
      return false;

   page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 || !util_is_power_of_two_or_zero(page_size) ||
       page_size > UINT32_MAX || page_size < CHROME9_MIP_ALIGNMENT)
      return false;

   /* Mipmapped D24 and DXT chains the sampler cannot address are sampled
    * by llvmpipe. */
   layout->sampler_unsupported =
      (chrome9_format_is_d24(templ->format) && templ->last_level) ||
      (dxt && (cube ||
               (templ->last_level &&
                (!chrome9_screen_has_capability2(screen,
                                                 DRM_CHROME9_CAP2_DXT_MIP) ||
                 !util_is_power_of_two_nonzero(templ->width0) ||
                 !util_is_power_of_two_nonzero(templ->height0)))));

   /* Private 2D colour render targets are tiled: the engine renders and
    * samples them about 30 % faster than linear ones.  So are window back
    * buffers, whose objects record the layout for their importers; a
    * scanout buffer that is not a back buffer stays linear. */
   layout->tiled = templ->target == PIPE_TEXTURE_2D &&
      (templ->format == PIPE_FORMAT_B8G8R8A8_UNORM ||
       templ->format == PIPE_FORMAT_B8G8R8X8_UNORM) &&
      ((templ->bind & PIPE_BIND_RENDER_TARGET) ||
       (templ->flags & CHROME9_RESOURCE_FLAG_TILED)) &&
      !(templ->bind & (PIPE_BIND_LINEAR | PIPE_BIND_DISPLAY_TARGET)) &&
      ((templ->flags & CHROME9_RESOURCE_FLAG_TILED) ||
       chrome9_tiled_back_buffer(screen, templ) ||
       (!(templ->bind & (PIPE_BIND_SHARED | PIPE_BIND_SCANOUT)) &&
        !(templ->flags & CHROME9_RESOURCE_FLAG_CANONICAL_SHARED))) &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_TILED_COLOR) &&
      debug_get_bool_option("CHROME9_TILED_COLOR", true);
   for (unsigned level = 0; level <= templ->last_level; level++) {
      const unsigned width = u_minify(templ->width0, level);
      const unsigned height = u_minify(templ->height0, level);

      layout->level_stride[level] =
         chrome9_format_level_pitch(templ->format, width);
      layout->level_size[level] =
         (uint64_t)layout->level_stride[level] *
         (layout->tiled ? ALIGN_POT(height, 8) :
          chrome9_format_level_rows(templ->format, height));
   }
   if (cube)
      return chrome9_resource_cube_layout(templ, page_size, layout);
   if (volume)
      return chrome9_resource_volume_layout(templ, page_size, layout);

   /* Cross-process image paths do not reliably preserve a non-zero plane
    * offset when they fall back from DRI3 modifiers to the legacy single-FD
    * protocol.  Keep shared level-zero images conventional: pixels begin at
    * byte zero and the allocation is page-rounded for KMS/GEM.  Such images
    * are deliberately unguarded and must render/sample through the guarded
    * shadows in chrome9_context.c; they must never reach a draw ioctl
    * directly.  Private resources retain the exact BO+PAGE kernel ABI below.
    */
   if ((templ->bind & PIPE_BIND_SHARED) &&
       !(templ->flags & CHROME9_RESOURCE_FLAG_CANONICAL_SHARED)) {
      if (templ->last_level || dxt ||
          !chrome9_u64_align(layout->level_size[0], page_size,
                             &layout->allocation_size) ||
          layout->allocation_size > CHROME9_MAX_BUFFER_SIZE)
         return false;
      layout->level_offset[0] = 0;
      layout->guarded = false;
      return true;
   }

   layout->level_offset[0] = page_size;
   if (!templ->last_level) {
      /* One leading guard, L0 rows, page padding, trailing guard: the
       * kernel requires this exact size. */
      if (!chrome9_u64_align(layout->level_size[0], page_size,
                             &aligned_level0_size) ||
          !chrome9_u64_add(layout->level_offset[0], aligned_level0_size,
                           &cursor) ||
          !chrome9_u64_add(cursor, page_size, &cursor))
         return false;
   } else {
      /* The sampler derives each level of a POT texture right after the
       * previous one's rows; NPOT levels come from its 256-byte granular
       * address table. */
      const bool pot = !layout->tiled &&
                       util_is_power_of_two_nonzero(templ->width0) &&
                       util_is_power_of_two_nonzero(templ->height0);

      cursor = layout->level_offset[0];
      for (unsigned level = 0; level <= templ->last_level; level++) {
         if (level && !pot &&
             !chrome9_u64_align(cursor, CHROME9_MIP_ALIGNMENT, &cursor))
            return false;
         layout->level_offset[level] = cursor;
         if (!chrome9_u64_add(cursor, layout->level_size[level], &cursor))
            return false;
      }
      if (!chrome9_u64_align(cursor, page_size, &cursor) ||
          !chrome9_u64_add(cursor, page_size, &cursor))
         return false;
   }
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
   resource->layer_stride = layout->layer_stride;
   resource->guarded = layout->guarded;
   resource->sampler_unsupported = layout->sampler_unsupported;
   resource->tiled = layout->tiled && layout->guarded;
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
   /* Z16 is a depth target and a texture that llvmpipe samples (the
    * sampler has no Z16 encoding). */
   const unsigned depth_bindings = PIPE_BIND_DEPTH_STENCIL |
                                   PIPE_BIND_SAMPLER_VIEW |
                                   PIPE_BIND_LINEAR;
   const unsigned sampler_bindings = PIPE_BIND_SAMPLER_VIEW |
                                     PIPE_BIND_LINEAR;
   unsigned bgra_bindings = PIPE_BIND_LINEAR;
   bool bgra_render;

   /* A usage hint, not a capability. */
   bindings &= ~PIPE_BIND_BACKBUFFER;
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
      return chrome9_is_surface_target(target) &&
             /* Enable only for guarded validation until the complete
              * Gallium FBO/draw/clear/rebind path passes on silicon. */
             debug_get_bool_option("CHROME9_EXPERIMENTAL_X8Z24", false) &&
             chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM) &&
             !(bindings & ~depth_bindings);
   /* Stencil shares the D24 gate until its guarded silicon oracles pass. */
   if (format == PIPE_FORMAT_S8_UINT_Z24_UNORM)
      return chrome9_is_surface_target(target) &&
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
             ((bindings & PIPE_BIND_DEPTH_STENCIL) || chrome9_sw_enabled()) &&
             !(bindings & ~depth_bindings);
   /* Cube maps and DXT need the generic fragment program, which carries
    * the texture target and block layout.  The state tracker also asks
    * for RENDER_TARGET on cube maps; faces are never hardware targets
    * (chrome9_set_framebuffer_state). */
   if (target == PIPE_TEXTURE_CUBE)
      return chrome9_screen_has_capability2(screen,
                                            DRM_CHROME9_CAP2_CUBE_MAP) &&
             chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM) &&
             /* DXT cube faces are sampled by llvmpipe only. */
             (!chrome9_format_is_dxt(format) ||
              (chrome9_sw_enabled() &&
               !(bindings & PIPE_BIND_RENDER_TARGET))) &&
             !(bindings & ~(sampler_bindings | PIPE_BIND_RENDER_TARGET)) &&
             chrome9_format_supported(screen, format, PIPE_TEXTURE_2D,
                                      sample_count, storage_sample_count,
                                      bindings);
   /* The pixel engine samples POT level zero of a 3D texture, llvmpipe
    * every other volume; slices render through a 2D shadow like cube
    * faces. */
   if (target == PIPE_TEXTURE_3D)
      return (chrome9_screen_has_capability2(screen,
                                             DRM_CHROME9_CAP2_TEXTURE_3D) ||
              chrome9_sw_enabled()) &&
             !chrome9_format_is_dxt(format) &&
             !util_format_is_depth_or_stencil(format) &&
             !(bindings & ~(sampler_bindings | PIPE_BIND_RENDER_TARGET)) &&
             chrome9_format_supported(screen, format, PIPE_TEXTURE_2D,
                                      sample_count, storage_sample_count,
                                      bindings);
   if (chrome9_format_is_dxt(format))
      return target == PIPE_TEXTURE_2D &&
             chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_DXT) &&
             chrome9_screen_has_capability2(screen,
                                            DRM_CHROME9_CAP2_DXT_MIP) &&
             chrome9_screen_has_capability(
                screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM) &&
             !(bindings & ~sampler_bindings);
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
   return chrome9_resource_layout(screen, templ, &layout);
}

/*
 * Shared single-level colour surfaces keep the guarded draw layout when the
 * kernel resolves an external offset of 0 to the first row, so they need no
 * render or sampler shadow.
 */
static bool
chrome9_guarded_shared(struct pipe_screen *screen,
                       const struct pipe_resource *templ)
{
   return (templ->bind & PIPE_BIND_SHARED) &&
          !(templ->flags & CHROME9_RESOURCE_FLAG_CANONICAL_SHARED) &&
          templ->target == PIPE_TEXTURE_2D && !templ->last_level &&
          (templ->format == PIPE_FORMAT_B8G8R8X8_UNORM ||
           templ->format == PIPE_FORMAT_B8G8R8A8_UNORM) &&
          chrome9_screen_has_capability2(screen,
                                         DRM_CHROME9_CAP2_GUARDED_SHARED) &&
          debug_get_bool_option("CHROME9_GUARDED_SHARED", true);
}

static bool
chrome9_resource_local(struct pipe_screen *screen,
                       const struct pipe_resource *templ,
                       const struct chrome9_resource_layout *layout,
                       bool guarded_shared)
{
   static int enabled = -1, textures = -1;
   unsigned bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET;

   if (enabled < 0) {
      enabled = debug_get_bool_option("CHROME9_LOCAL_MEMORY", true);
      textures = debug_get_bool_option("CHROME9_LOCAL_TEXTURES", false);
   }
   if (textures)
      bind |= PIPE_BIND_SAMPLER_VIEW;
   /* Shared and scanout surfaces too: same-device PRIME import returns
    * the object and KMS scans local framebuffers out of SL.  Linear shadows
    * are copied by the CPU to the drawable. */
   (void)guarded_shared;
   return enabled && layout->guarded &&
          templ->target == PIPE_TEXTURE_2D &&
          ((!(templ->bind & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_LINEAR)) &&
            (templ->bind & bind)) ||
           (templ->flags & CHROME9_RESOURCE_FLAG_LOCAL)) &&
          chrome9_screen_has_capability2(screen,
                                         DRM_CHROME9_CAP2_LOCAL_MEMORY);
}

struct pipe_resource *
chrome9_resource_create(struct pipe_screen *screen,
                        const struct pipe_resource *templ)
{
   struct chrome9_resource *resource;
   struct chrome9_resource_layout layout;
   struct pipe_resource layout_templ = *templ;
   bool guarded_shared;
   uint32_t create_flags = 0;
   int fd;

   if (getenv("CHROME9_DEBUG_RESOURCES"))
      fprintf(stderr,
              "chrome9: resource target=%u format=%s %ux%ux%u array=%u "
              "last=%u samples=%u/%u bind=%#x\n",
              templ->target, util_format_short_name(templ->format),
              templ->width0, templ->height0,
              templ->depth0, templ->array_size, templ->last_level,
              templ->nr_samples, templ->nr_storage_samples, templ->bind);

   resource = CALLOC_STRUCT(chrome9_resource);
   if (!resource)
      return NULL;
   resource->serial = p_atomic_inc_return(&chrome9_resource_serial);
   guarded_shared = chrome9_guarded_shared(screen, templ);
   if (guarded_shared) {
      layout_templ.flags |= CHROME9_RESOURCE_FLAG_CANONICAL_SHARED;
      create_flags |= DRM_CHROME9_GEM_CREATE_GUARDED_SHARED;
   }
   if (!chrome9_can_create_resource(screen, templ) ||
       !chrome9_resource_layout(screen, &layout_templ, &layout))
      goto fail;

   /* Only the exported, offset-zero KMS allocation needs constrained pages.
    * Keeping private guarded render/depth resources in ordinary memory avoids
    * consuming DMA32 for data which never becomes the persistent root scanout.
    */
   if ((!layout.guarded || guarded_shared) &&
       (templ->bind & PIPE_BIND_SCANOUT) &&
       chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_GEM_DMA32_SCANOUT))
      create_flags |= DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT;

   /* Importers learn the tiled layout of a shared object from GEM_INFO. */
   if (layout.tiled && layout.guarded && (templ->bind & PIPE_BIND_SHARED))
      create_flags |= DRM_CHROME9_GEM_CREATE_TILED;
   fd = screen->get_screen_fd(screen);
   if (fd < 0)
      goto fail;
   /* Private depth buffers and render targets go to the local frame
    * buffer, which the engine reads and writes about 2.7 times faster than
    * GART pages; GART pages when the pool is full. */
   if (chrome9_resource_local(screen, templ, &layout, guarded_shared) &&
       chrome9_bo_create(fd, align64(layout.allocation_size, 4096),
                         DRM_CHROME9_GEM_CREATE_LOCAL |
                         (create_flags & (DRM_CHROME9_GEM_CREATE_GUARDED_SHARED |
                                          DRM_CHROME9_GEM_CREATE_TILED)),
                         &resource->bo)) {
      resource->local = true;
   } else if (templ->target == PIPE_BUFFER &&
              chrome9_screen_bo_cache_get(screen, layout.allocation_size,
                                          create_flags, &resource->bo)) {
      /* Only objects already opted in to explicit access are cached. */
      resource->explicit_access = true;
   } else if (!chrome9_bo_create(fd, layout.allocation_size, create_flags,
                                 &resource->bo)) {
      goto fail;
   }

   if (getenv("CHROME9_DEBUG_RESOURCES"))
      fprintf(stderr, "chrome9: resource %u bytes %s%s\n",
              (unsigned)layout.allocation_size,
              resource->local ? "local" : "gart",
              layout.tiled && layout.guarded ? " tiled" : "");
   chrome9_resource_apply_layout(resource, &layout);
   resource->guarded_shared = guarded_shared;
   /* Tiled rows come in eights: other heights stay linear so the object
    * size is the same either way. */
   resource->tiled = resource->tiled ||
      (resource->guarded && !guarded_shared &&
      util_format_is_depth_or_stencil(templ->format) &&
      templ->target == PIPE_TEXTURE_2D && !templ->last_level &&
      !(templ->height0 % 8) &&
      !(templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) &&
      chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_TILED_DEPTH) &&
      debug_get_bool_option("CHROME9_TILED_DEPTH", true));
   resource->base = *templ;
   resource->base.screen = screen;
   pipe_reference_init(&resource->base.reference, 1);
   /* Resources opt in to deferred cache maintenance: every CPU access
    * goes through chrome9_resource_cpu_access(), which lets the kernel skip
    * flushes and finite checks of unchanged vertex buffers.  Shared
    * surfaces stay in the protocol because every importer on this device
    * is this driver; the kernel reverts an object that becomes a KMS
    * framebuffer. */
   if (!resource->explicit_access &&
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
   struct chrome9_resource_layout layout, guarded;
   struct pipe_resource guarded_templ = *templ;
   uint32_t bo_flags = 0;
   uint64_t data_size;
   int fd;

   (void)usage;
   if (!whandle || whandle->type != WINSYS_HANDLE_TYPE_FD ||
       (whandle->modifier != DRM_FORMAT_MOD_LINEAR &&
        whandle->modifier != DRM_FORMAT_MOD_INVALID) ||
       templ->last_level ||
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
   resource->shared = true;
   if (!chrome9_bo_info(fd, &resource->bo, &bo_flags))
      bo_flags = 0;
   resource->local = bo_flags & DRM_CHROME9_GEM_CREATE_LOCAL;
   /* A tiled object has only its tiled guarded layout. */
   if (bo_flags & DRM_CHROME9_GEM_CREATE_TILED) {
      if (whandle->modifier != DRM_FORMAT_MOD_INVALID)
         goto fail_bo;
      guarded_templ.flags |= CHROME9_RESOURCE_FLAG_TILED;
   }
   if (!chrome9_resource_layout(screen, &guarded_templ, &layout))
      goto fail_bo;
   /* A native Chrome9 DRI3 export retains the complete canonical guarded
    * allocation.  Preserve that fact after same-device PRIME import so the
    * consumer can submit the BO directly and share its reservation object;
    * this avoids a stale userspace sampler copy.  Every tuple which is not
    * exact remains an ordinary unguarded external resource. */
   guarded_templ.flags |= CHROME9_RESOURCE_FLAG_CANONICAL_SHARED;
   if (!(bo_flags & DRM_CHROME9_GEM_CREATE_TILED) &&
       whandle->stride == layout.level_stride[0] &&
       whandle->offset == layout.level_offset[0] &&
       resource->bo.size == layout.allocation_size) {
      chrome9_resource_apply_layout(resource, &layout);
   } else if (!whandle->offset &&
              chrome9_screen_has_capability2(
                 screen, DRM_CHROME9_CAP2_GUARDED_SHARED) &&
              (bo_flags & DRM_CHROME9_GEM_CREATE_GUARDED_SHARED) &&
              chrome9_resource_layout(screen, &guarded_templ, &guarded) &&
              whandle->stride == guarded.level_stride[0] &&
              resource->bo.size == guarded.allocation_size) {
      chrome9_resource_apply_layout(resource, &guarded);
      resource->guarded_shared = true;
   } else if (bo_flags & DRM_CHROME9_GEM_CREATE_TILED) {
      goto fail_bo;
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

   /* The CPU reads and writes materialized depths only. */
   if (resource->fast_z_index)
      chrome9_resource_fast_z_resolve(resource);

   if (write) {
      resource->cpu_write_generation++;
      resource->coarse_z_valid = false;
   }
   /* Every access also waits for asynchronously retiring jobs. */
   if (chrome9_screen_has_capability(screen, DRM_CHROME9_CAP_GEM_CPU_ACCESS) &&
       !chrome9_bo_cpu_access_range(screen->get_screen_fd(screen),
                                    &resource->bo,
                                    write ? DRM_CHROME9_GEM_CPU_PREPARE_WRITE :
                                            DRM_CHROME9_GEM_CPU_PREPARE_READ,
                                    offset, size))
      fprintf(stderr, "chrome9: GEM_CPU_ACCESS failed: %s\n", strerror(errno));
}

/* Rewrite the words of older Fast-Z clears (stale index) as 1.0. */
void
chrome9_resource_fast_z_resolve(struct chrome9_resource *depth)
{
   const uint32_t index = depth->fast_z_index;

   if (!index)
      return;
   depth->fast_z_index = 0;
   chrome9_resource_cpu_access(depth, true);
   /* Tiled rows mix columns: every word of the pitch holds depth. */
   const unsigned words = depth->tiled ? depth->stride / 4 :
                          depth->base.width0;
   /* Local memory is write-combined: stream each row into a cached copy
    * instead of reading it word by word. */
   uint32_t *copy = malloc(words * sizeof(*copy));

   for (unsigned y = 0; y < depth->base.height0; y++) {
      uint32_t *row = (uint32_t *)((uint8_t *)depth->bo.map +
                                   depth->data_offset +
                                   (uint64_t)y * depth->stride);
      const uint32_t *read = row;

      if (copy) {
         util_streaming_load_memcpy(copy, row, words * sizeof(*copy));
         read = copy;
      }
      for (unsigned x = 0; x < words; x++)
         if (((read[x] >> 8) & 7) != index)
            row[x] = 0xffffff00;
   }
   free(copy);
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

   /* The object now has other users: it keeps its storage. */
   resource->shared = true;
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
   whandle->offset = resource->guarded_shared ? 0 : resource->data_offset;
   /* The tiled layout is implicit, recorded in the object. */
   whandle->modifier = resource->tiled ? DRM_FORMAT_MOD_INVALID :
                                         DRM_FORMAT_MOD_LINEAR;
   return true;
}

void
chrome9_resource_destroy(struct pipe_screen *screen,
                         struct pipe_resource *resource)
{
   struct chrome9_resource *cresource = chrome9_resource(resource);
   /* Private explicit buffers go to the screen's object cache. */
   const bool cacheable = resource->target == PIPE_BUFFER &&
                          cresource->explicit_access && !cresource->shared &&
                          !cresource->local;

   /* A view borrows its parent's BO. */
   if (cresource->parent) {
      chrome9_resource(cresource->parent)->views--;
      pipe_resource_reference(&cresource->parent, NULL);
   }
   else if (!cacheable ||
            !chrome9_screen_bo_cache_put(screen, &cresource->bo))
      chrome9_screen_bo_destroy(screen, &cresource->bo);
   util_dynarray_foreach(&cresource->spare_bos, struct chrome9_bo, bo)
      if (!cacheable || !chrome9_screen_bo_cache_put(screen, bo))
         chrome9_screen_bo_destroy(screen, bo);
   util_dynarray_fini(&cresource->spare_bos);
   if (cresource->coarse_z.handle)
      chrome9_screen_bo_destroy(screen, &cresource->coarse_z);
   FREE(cresource);
}

/*
 * Single-level render view of one level and layer of a guarded BGRA/BGRX
 * resource, sharing its BO: the pixel engine takes a 256-byte aligned base
 * (A50) and the window must end before the trailing guard page.  NULL when
 * the image is not addressable that way.
 */
struct pipe_resource *
chrome9_resource_image_view(struct pipe_resource *pres, unsigned level,
                            unsigned layer)
{
   struct chrome9_resource *res = chrome9_resource(pres);
   const unsigned width = u_minify(pres->width0, level);
   const unsigned height = u_minify(pres->height0, level);
   const long page_size = sysconf(_SC_PAGESIZE);
   struct chrome9_resource *view;
   uint64_t offset, end;

   if (!chrome9_screen_has_capability2(pres->screen,
                                       DRM_CHROME9_CAP2_DESTINATION_OFFSET) ||
       !res->guarded || res->guarded_shared || res->parent ||
       !res->bo.handle || page_size <= 0 || level > pres->last_level ||
       (pres->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        pres->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       res->level_stride[level] != ALIGN_POT(width * 4, 32))
      return NULL;
   offset = res->level_offset[level] + (uint64_t)layer *
      (res->layer_stride ? res->layer_stride : res->level_size[level]);
   end = offset + (uint64_t)res->level_stride[level] * height;
   if (offset < (uint64_t)page_size || offset % 256 || offset > UINT32_MAX ||
       end > res->bo.size - page_size)
      return NULL;
   view = CALLOC_STRUCT(chrome9_resource);
   if (!view)
      return NULL;
   view->base = *pres;
   pipe_reference_init(&view->base.reference, 1);
   view->base.next = NULL;
   view->base.target = PIPE_TEXTURE_2D;
   view->base.width0 = width;
   view->base.height0 = height;
   view->base.depth0 = 1;
   view->base.array_size = 1;
   view->base.last_level = 0;
   view->bo = res->bo;
   view->bo.handle_owned = false;
   view->level_stride[0] = res->level_stride[level];
   view->level_offset[0] = offset;
   view->level_size[0] = (uint64_t)res->level_stride[level] * height;
   view->stride = view->level_stride[0];
   view->data_offset = offset;
   view->data_size = view->level_size[0];
   view->guarded = true;
   view->explicit_access = res->explicit_access;
   pipe_resource_reference(&view->parent, pres);
   res->views++;
   return &view->base;
}

static bool
chrome9_transfer_box_valid(const struct pipe_resource *resource,
                           unsigned level, const struct pipe_box *box)
{
   unsigned width;
   unsigned height;
   unsigned layers;
   unsigned block;

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
   layers = resource->target == PIPE_TEXTURE_CUBE ? resource->array_size :
            resource->target == PIPE_TEXTURE_3D ?
            u_minify(resource->depth0, level) : 1;
   block = chrome9_format_block_dim(resource->format);
   /* Block formats map whole blocks; the last one may overhang the edge. */
   return (chrome9_is_surface_target(resource->target) ||
           resource->target == PIPE_TEXTURE_CUBE ||
           resource->target == PIPE_TEXTURE_3D) &&
          (unsigned)box->z < layers && box->depth >= 1 &&
          (unsigned)box->depth <= layers - (unsigned)box->z &&
          (unsigned)box->x <= width &&
          (unsigned)box->width <= width - (unsigned)box->x &&
          (unsigned)box->y <= height &&
          (unsigned)box->height <= height - (unsigned)box->y &&
          !(box->x % block) && !(box->y % block) &&
          (!(box->width % block) ||
           (unsigned)(box->x + box->width) == width) &&
          (!(box->height % block) ||
           (unsigned)(box->y + box->height) == height);
}

/* A mapping of a tiled image: a linear copy of the box. */
struct chrome9_transfer {
   struct pipe_transfer base;
   uint8_t *staging;
   uint8_t *level_base;
   unsigned pitch;
   unsigned bytes;
};

/* A synchronized write to a buffer that queued jobs only read moves to
 * fresh storage instead of waiting for them; the jobs keep the old object.
 * The next map of the idle new storage writes in place. */
static void
chrome9_buffer_rename(struct chrome9_resource *resource, unsigned usage,
                      const struct pipe_box *box)
{
   struct pipe_screen *screen = resource->base.screen;
   const int fd = screen->get_screen_fd(screen);
   struct chrome9_bo old = resource->bo, bo;
   const uint64_t copy = (usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE) ? 0 :
                         (usage & PIPE_MAP_DISCARD_RANGE) ?
                            old.size - box->width : old.size;
   uint32_t busy;

   if (resource->base.target != PIPE_BUFFER ||
       (usage & (PIPE_MAP_READ | PIPE_MAP_UNSYNCHRONIZED |
                 PIPE_MAP_PERSISTENT | PIPE_MAP_COHERENT)) ||
       !(usage & PIPE_MAP_WRITE) || !old.handle_owned || old.shared ||
       /* Copying more than the map writes costs more than the wait. */
       copy > (uint64_t)box->width ||
       !old.map || chrome9_resource_is_local(resource) ||
       !chrome9_screen_has_capability2(screen, DRM_CHROME9_CAP2_GEM_BUSY) ||
       !chrome9_bo_busy(fd, &old, &busy) ||
       (busy & (DRM_CHROME9_GEM_BUSY_USE | DRM_CHROME9_GEM_BUSY_WRITE)) !=
          DRM_CHROME9_GEM_BUSY_USE)
      return;
   /* An idle spare, else new storage. */
   bo.handle = 0;
   util_dynarray_foreach(&resource->spare_bos, struct chrome9_bo, spare) {
      if (chrome9_bo_busy(fd, spare, &busy) && !busy) {
         bo = *spare;
         *spare = util_dynarray_pop(&resource->spare_bos, struct chrome9_bo);
         break;
      }
   }
   if (!bo.handle) {
      if (!chrome9_bo_create(fd, old.size, old.create_flags, &bo))
         return;
      if (resource->explicit_access &&
          !chrome9_bo_cpu_access(fd, &bo, DRM_CHROME9_GEM_CPU_EXPLICIT)) {
         chrome9_bo_destroy(fd, &bo);
         return;
      }
   }
   resource->bo = bo;
   util_dynarray_append(&resource->spare_bos, old);
   /* The copied bytes are CPU writes to the new object. */
   if (usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE) {
      /* Nothing survives. */
   } else if (usage & PIPE_MAP_DISCARD_RANGE) {
      const uint64_t end = box->x + box->width;

      memcpy(bo.map, old.map, box->x);
      memcpy((uint8_t *)bo.map + end, (const uint8_t *)old.map + end,
             old.size - end);
      if (box->x)
         chrome9_resource_cpu_access_range(resource, true, 0, box->x);
      if (end < old.size)
         chrome9_resource_cpu_access_range(resource, true, end,
                                           old.size - end);
   } else {
      memcpy(bo.map, old.map, old.size);
      chrome9_resource_cpu_access_range(resource, true, 0, old.size);
   }
}

/* glBufferData(NULL) of the same size: fresh or idle storage, the queued
 * jobs keep reading the old object. */
static void
chrome9_invalidate_resource(struct pipe_context *pipe,
                            struct pipe_resource *resource)
{
   const struct pipe_box box = {
      .width = resource->width0, .height = 1, .depth = 1,
   };

   if (resource->target != PIPE_BUFFER)
      return;
   chrome9_tcl_batch_flush_resource(chrome9_context(pipe), resource);
   chrome9_buffer_rename(chrome9_resource(resource),
                         PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                         &box);
}

/*
 * A linear GART image holding a box of a tiled local BGRA/BGRX level at its
 * top left, copied by the engine for a CPU read: the CPU then neither
 * detiles nor reads write-combined memory, which pays off from about 64 KiB
 * on.
 */
static struct chrome9_resource *
chrome9_transfer_readback(struct chrome9_context *ctx,
                          struct chrome9_resource *res, unsigned level,
                          const struct pipe_box *box)
{
   struct pipe_screen *screen = ctx->base.screen;
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = res->base.format,
      .width0 = box->width,
      .height0 = box->height,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW,
   };
   struct chrome9_resource *image;

   if (!res->local || !chrome9_resource_is_tiled(res) ||
       (res->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        res->base.format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
       (uint64_t)box->width * box->height * 4 < 64 * 1024 ||
       !debug_get_bool_option("CHROME9_GPU_READBACK", true))
      return NULL;
   if (!ctx->readback || ctx->readback->format != templ.format ||
       ctx->readback->width0 < templ.width0 ||
       ctx->readback->height0 < templ.height0) {
      templ.width0 = MAX2(templ.width0,
                          ctx->readback ? ctx->readback->width0 : 0);
      templ.height0 = MAX2(templ.height0,
                           ctx->readback ? ctx->readback->height0 : 0);
      pipe_resource_reference(&ctx->readback, NULL);
      ctx->readback = screen->resource_create(screen, &templ);
      if (!ctx->readback)
         return NULL;
   }
   image = chrome9_resource(ctx->readback);
   if (image->local || chrome9_resource_is_tiled(image) || !image->bo.map ||
       !chrome9_resource_copy_gpu(ctx, image, 0, res, level, box))
      return NULL;
   chrome9_resource_cpu_access(image, false);
   return image;
}

static void *
chrome9_transfer_map(struct pipe_context *pipe, struct pipe_resource *resource,
                     unsigned level, unsigned usage,
                     const struct pipe_box *box,
                     struct pipe_transfer **out_transfer)
{
   struct chrome9_resource *cresource = chrome9_resource(resource);
   struct chrome9_transfer *ctransfer;
   struct pipe_transfer *transfer;
   uint64_t offset;

   if (!out_transfer || !chrome9_transfer_box_valid(resource, level, box))
      return NULL;

   /* Reads of uncached local memory: move the resource to GART pages. */
   if ((usage & PIPE_MAP_READ) && chrome9_resource_is_local(cresource))
      chrome9_context_demote_local(chrome9_context(pipe), resource);
   /* Retained triangle batches and the shadow color target are not visible
    * through a direct persistent mapping until submitted and resolved. */
   if (resource->target != PIPE_BUFFER)
      chrome9_context_sync_resource(chrome9_context(pipe), resource, usage);
   else if (!(usage & PIPE_MAP_UNSYNCHRONIZED)) {
      /* Queued draws may still read the buffer. */
      chrome9_tcl_batch_flush_resource(chrome9_context(pipe), resource);
      chrome9_buffer_rename(cresource, usage, box);
   }
   /* An unsynchronized buffer write touches no bytes a queued job reads:
    * record the range without waiting for the device. */
   if (resource->target == PIPE_BUFFER &&
       (usage & (PIPE_MAP_UNSYNCHRONIZED | PIPE_MAP_READ | PIPE_MAP_WRITE)) ==
          (PIPE_MAP_UNSYNCHRONIZED | PIPE_MAP_WRITE) &&
       chrome9_screen_has_capability2(resource->screen,
                                      DRM_CHROME9_CAP2_GEM_CPU_NOWAIT)) {
      cresource->cpu_write_generation++;
      if (!chrome9_bo_cpu_access_range(
             resource->screen->get_screen_fd(resource->screen),
             &cresource->bo,
             DRM_CHROME9_GEM_CPU_PREPARE_WRITE | DRM_CHROME9_GEM_CPU_NOWAIT,
             box->x, box->width))
         fprintf(stderr, "chrome9: GEM_CPU_ACCESS failed: %s\n",
                 strerror(errno));
   } else if (resource->target == PIPE_BUFFER) {
      /* Only the mapped range becomes dirty for the next device use. */
      chrome9_resource_cpu_access_range(cresource, usage & PIPE_MAP_WRITE,
                                        box->x, box->width);
   } else {
      chrome9_resource_cpu_access(cresource, usage & PIPE_MAP_WRITE);
   }

   ctransfer = CALLOC_STRUCT(chrome9_transfer);
   if (!ctransfer)
      return NULL;
   transfer = &ctransfer->base;
   pipe_resource_reference(&transfer->resource, resource);
   transfer->level = level;
   transfer->usage = usage;
   transfer->box = *box;

   if (resource->target == PIPE_BUFFER) {
      offset = box->x;
   } else {
      const unsigned cpp = chrome9_format_storage_cpp(resource->format);
      const unsigned block = chrome9_format_block_dim(resource->format);

      if (!cpp) {
         pipe_resource_reference(&transfer->resource, NULL);
         FREE(transfer);
         return NULL;
      }

      transfer->stride = cresource->level_stride[level];
      transfer->layer_stride = cresource->layer_stride ?
         cresource->layer_stride : cresource->level_size[level];

      offset = cresource->level_offset[level] +
               (uint64_t)box->z * transfer->layer_stride +
               (uint64_t)(box->y / block) * cresource->level_stride[level] +
               (uint64_t)(box->x / block) * cpp;
   }
   if (offset > cresource->bo.size) {
      pipe_resource_reference(&transfer->resource, NULL);
      FREE(transfer);
      return NULL;
   }
   /* Tiled images map through a linear copy of the box, stored back on
    * unmap after a write. */
   if (chrome9_resource_is_tiled(cresource)) {
      const unsigned cpp = chrome9_format_storage_cpp(resource->format);

      ctransfer->level_base = (uint8_t *)cresource->bo.map +
                              cresource->level_offset[level];
      ctransfer->pitch = cresource->level_stride[level];
      ctransfer->bytes = box->width * cpp;
      transfer->stride = ctransfer->bytes;
      transfer->layer_stride = (size_t)ctransfer->bytes * box->height;
      ctransfer->staging = malloc(transfer->layer_stride);
      if (!ctransfer->staging) {
         pipe_resource_reference(&transfer->resource, NULL);
         FREE(transfer);
         return NULL;
      }
      if (!(usage & PIPE_MAP_DISCARD_RANGE) &&
          !(usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE)) {
         struct chrome9_resource *image =
            chrome9_transfer_readback(chrome9_context(pipe), cresource,
                                      level, box);

         if (image) {
            const uint8_t *src = (const uint8_t *)image->bo.map +
                                 image->level_offset[0];

            for (int y = 0; y < box->height; y++)
               memcpy(ctransfer->staging + (size_t)y * ctransfer->bytes,
                      src + (size_t)y * image->level_stride[0],
                      ctransfer->bytes);
         } else {
            chrome9_tiled_copy(ctransfer->level_base, ctransfer->pitch, true,
                               box->x * cpp, box->y, ctransfer->bytes,
                               box->height, ctransfer->staging,
                               ctransfer->bytes, false);
         }
      }
      *out_transfer = transfer;
      return ctransfer->staging;
   }
   *out_transfer = transfer;
   return (uint8_t *)cresource->bo.map + offset;
}

static void
chrome9_transfer_unmap(struct pipe_context *pipe,
                       struct pipe_transfer *transfer)
{
   struct chrome9_transfer *ctransfer = (struct chrome9_transfer *)transfer;

   (void)pipe;
   if (!transfer)
      return;
   if (ctransfer->staging) {
      if (transfer->usage & PIPE_MAP_WRITE)
         chrome9_tiled_copy(ctransfer->level_base, ctransfer->pitch, true,
                            transfer->box.x *
                               chrome9_format_storage_cpp(
                                  transfer->resource->format),
                            transfer->box.y, ctransfer->bytes,
                            transfer->box.height, ctransfer->staging,
                            ctransfer->bytes, true);
      free(ctransfer->staging);
   }
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
                           unsigned level, unsigned layer, unsigned cpp)
{
   const unsigned width = u_minify(resource->base.width0, level);
   const unsigned height = u_minify(resource->base.height0, level);
   const uint64_t offset = resource->level_offset[level] +
                           (uint64_t)layer * resource->layer_stride;

   return resource->level_stride[level] >= width * cpp &&
          resource->level_size[level] >=
             (uint64_t)resource->level_stride[level] * height &&
          offset <= resource->bo.size &&
          resource->level_size[level] <= resource->bo.size - offset;
}

static bool
chrome9_mipmap_map_level(struct pipe_context *pipe,
                         struct chrome9_resource *resource, unsigned level,
                         unsigned layer, unsigned usage, unsigned cpp,
                         struct chrome9_mipmap_mapping *mapping)
{
   const unsigned width = u_minify(resource->base.width0, level);
   const unsigned height = u_minify(resource->base.height0, level);
   const struct pipe_box box = {
      .z = layer,
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
       !chrome9_mipmap_level_valid(resource, level, layer, cpp))
      return false;
   mapping->data = (uint8_t *)resource->bo.map +
                   resource->level_offset[level] +
                   (uint64_t)layer * resource->layer_stride;
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

/* Box-filter one level of one layer from its predecessor. */
static bool
chrome9_mipmap_downsample(struct pipe_context *pipe,
                          struct chrome9_resource *resource,
                          enum pipe_format format, unsigned cpp,
                          unsigned level, unsigned layer)
{
   const struct pipe_resource *pres = &resource->base;
   const unsigned source_width = u_minify(pres->width0, level - 1);
   const unsigned source_height = u_minify(pres->height0, level - 1);
   const unsigned destination_width = u_minify(pres->width0, level);
   const unsigned destination_height = u_minify(pres->height0, level);
   struct chrome9_mipmap_mapping source;
   struct chrome9_mipmap_mapping destination;

   if (!chrome9_mipmap_map_level(pipe, resource, level - 1, layer,
                                 PIPE_MAP_READ, cpp, &source))
      return false;
   if (!chrome9_mipmap_map_level(pipe, resource, level, layer,
                                 PIPE_MAP_WRITE, cpp, &destination)) {
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
   return true;
}

/* 2x2x2 box filter of each 3D level from its predecessor. */
static bool
chrome9_generate_volume_mipmap(struct pipe_context *pipe,
                               struct pipe_resource *pres,
                               enum pipe_format format,
                               unsigned base_level, unsigned last_level)
{
   const unsigned cpp = chrome9_format_storage_cpp(format);

   if (!pipe->texture_map || !pipe->texture_unmap || format != pres->format ||
       !cpp || util_format_is_compressed(format) ||
       base_level > last_level || last_level > pres->last_level)
      return false;
   for (unsigned level = base_level + 1; level <= last_level; level++) {
      const unsigned sw = u_minify(pres->width0, level - 1);
      const unsigned sh = u_minify(pres->height0, level - 1);
      const unsigned sd = u_minify(pres->depth0, level - 1);
      const unsigned dw = u_minify(pres->width0, level);
      const unsigned dh = u_minify(pres->height0, level);
      const unsigned dd = u_minify(pres->depth0, level);
      struct pipe_transfer *src_transfer, *dst_transfer;
      struct pipe_box src_box, dst_box;
      const uint8_t *src;
      uint8_t *dst = NULL;

      u_box_3d(0, 0, 0, sw, sh, sd, &src_box);
      u_box_3d(0, 0, 0, dw, dh, dd, &dst_box);
      src = pipe->texture_map(pipe, pres, level - 1, PIPE_MAP_READ, &src_box,
                              &src_transfer);
      if (src)
         dst = pipe->texture_map(pipe, pres, level, PIPE_MAP_WRITE,
                                 &dst_box, &dst_transfer);
      if (!dst) {
         if (src)
            pipe->texture_unmap(pipe, src_transfer);
         return false;
      }
      for (unsigned z = 0; z < dd; z++)
         for (unsigned y = 0; y < dh; y++)
            for (unsigned x = 0; x < dw; x++) {
               float sum[4] = { 0 };
               float texel[4];

               for (unsigned i = 0; i < 8; i++) {
                  const unsigned sx = MIN2(2 * x + (i & 1), sw - 1);
                  const unsigned sy = MIN2(2 * y + (i >> 1 & 1), sh - 1);
                  const unsigned sz = MIN2(2 * z + (i >> 2), sd - 1);

                  util_format_unpack_rgba(format, texel,
                     src + sz * src_transfer->layer_stride +
                        sy * src_transfer->stride + sx * cpp, 1);
                  for (unsigned c = 0; c < 4; c++)
                     sum[c] += texel[c] * 0.125f;
               }
               util_format_pack_rgba(format,
                  dst + z * dst_transfer->layer_stride +
                     y * dst_transfer->stride + x * cpp, sum, 1);
            }
      pipe->texture_unmap(pipe, dst_transfer);
      pipe->texture_unmap(pipe, src_transfer);
   }
   return true;
}

/* Level of a guarded chain as a draw window at a 256-byte aligned base; a
 * level off that alignment starts whole rows after the rounded base. */
static bool
chrome9_mipmap_window(const struct chrome9_resource *resource, unsigned level,
                      uint32_t *base, unsigned *first_row)
{
   const uint64_t offset = resource->level_offset[level];
   const uint32_t delta = offset & 255;

   if (offset > UINT32_MAX || delta % resource->level_stride[level])
      return false;
   *base = offset - delta;
   *first_row = delta / resource->level_stride[level];
   return true;
}

/*
 * Draw each level from the one above in the same BO: the mean of four
 * NEAREST samples at the centres of each 2x2 texel group, LOD clamped to
 * the source level.  A bilinear sample at the group corner is off by up
 * to 15 LSB from 1600-texel sources (coordinate precision); centres keep
 * half a texel of margin (within 1 LSB, rung mipgen4).  Like the CPU path
 * a single row or column is used alone and the last of an odd count is
 * dropped.  Nothing waits for the GPU or touches the CPU mapping.  Returns
 * the first level not generated.
 */
/*
 * Fill the coarse-Z buffer of a depth surface just cleared to 1.0: every
 * entry all ones, drawn as a BGRA8888 surface of pitch / 4 pixels by one
 * row per 8 depth rows.  The buffer is created on the first fill.
 */
bool
chrome9_resource_coarse_z_fill(struct pipe_context *pipe,
                               struct chrome9_resource *depth)
{
   static const unsigned corner[6][2] = {
      { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 },
   };
   static const struct drm_chrome9_fragment_instruction mov = {
      .op = DRM_CHROME9_FRAGMENT_OP_MOV,
      .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
      .src = { { DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0, { 0, 1, 2, 3 },
                 0, 0 } },
   };
   const uint32_t one[4] = {
      fui(1.0f), fui(1.0f), fui(1.0f), fui(1.0f),
   };
   struct pipe_screen *screen = pipe->screen;
   const int fd = screen->get_screen_fd(screen);
   const long page = sysconf(_SC_PAGESIZE);
   const unsigned pitch = align(DIV_ROUND_UP(depth->base.width0, 8) * 16, 32);
   const unsigned rows = DIV_ROUND_UP(depth->base.height0, 8);
   struct drm_chrome9_fragment_vertex vertices[6];
   struct drm_chrome9_draw_fragment_program draw = {
      .triangle_count = 2,
      .width = pitch / 4,
      .height = rows,
      .destination_pitch = pitch,
      .destination_format = DRM_CHROME9_DESTINATION_FORMAT_BGRA8888,
      .instruction_count = 1,
      .constant_count = 1,
      .abi_version = DRM_CHROME9_FRAGMENT_ABI_VERSION,
      .instructions_ptr = (uintptr_t)&mov,
      .constants_ptr = (uintptr_t)one,
      .vertices_ptr = (uintptr_t)vertices,
   };

   depth->coarse_z_valid = false;
   if (pitch >= 16384 || pitch / 4 > 2048 || rows > 2048)
      return false;
   if (!depth->coarse_z.handle) {
      const uint64_t size = align64((uint64_t)pitch * rows, page) + 2 * page;

      if (!chrome9_bo_create(fd, size, 0, &depth->coarse_z))
         return false;
      /* Only the fill below writes it; announce no CPU writes. */
      if (!chrome9_bo_cpu_access(fd, &depth->coarse_z,
                                 DRM_CHROME9_GEM_CPU_EXPLICIT)) {
         chrome9_screen_bo_destroy(screen, &depth->coarse_z);
         memset(&depth->coarse_z, 0, sizeof(depth->coarse_z));
         return false;
      }
      depth->coarse_z_pitch = pitch;
   }
   memset(vertices, 0, sizeof(vertices));
   for (unsigned v = 0; v < 6; v++) {
      vertices[v].x_16_16 = (int32_t)(corner[v][0] * draw.width << 16) - 0x8000;
      vertices[v].y_16_16 = (int32_t)(corner[v][1] * rows << 16) - 0x8000;
      vertices[v].rhw_float = fui(1.0f);
   }
   draw.destination_handle = depth->coarse_z.handle;
   if (!chrome9_draw_fragment_program(fd, &draw))
      return false;
   depth->coarse_z_valid = true;
   depth->coarse_z_reset = true;
   return true;
}

static unsigned
chrome9_generate_mipmap_gpu(struct chrome9_context *ctx,
                            struct chrome9_resource *resource,
                            unsigned base_level, unsigned last_level)
{
   static const unsigned corner[6][2] = {
      { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 },
   };
   static const struct drm_chrome9_fragment_source temp[7] = {
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 0, { 0, 1, 2, 3 }, 0, 0 },
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 1, { 0, 1, 2, 3 }, 0, 0 },
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 2, { 0, 1, 2, 3 }, 0, 0 },
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 3, { 0, 1, 2, 3 }, 0, 0 },
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 4, { 0, 1, 2, 3 }, 0, 0 },
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 5, { 0, 1, 2, 3 }, 0, 0 },
      { DRM_CHROME9_FRAGMENT_FILE_TEMP, 6, { 0, 1, 2, 3 }, 0, 0 },
   };
   /* Temps are written once: tk = TEX vk, t4..t6 sums, colour t6 / 4. */
   const struct drm_chrome9_fragment_instruction program[8] = {
#define CHROME9_MIP_TEX(k) \
      { .op = DRM_CHROME9_FRAGMENT_OP_TEX, \
        .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, k, 0xf, 0 }, \
        .src = { { DRM_CHROME9_FRAGMENT_FILE_VARYING, k, { 0, 1, 2, 3 }, \
                   0, 0 } } }
      CHROME9_MIP_TEX(0), CHROME9_MIP_TEX(1),
      CHROME9_MIP_TEX(2), CHROME9_MIP_TEX(3),
#undef CHROME9_MIP_TEX
      { .op = DRM_CHROME9_FRAGMENT_OP_ADD,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 4, 0xf, 0 },
        .src = { temp[0], temp[1] } },
      { .op = DRM_CHROME9_FRAGMENT_OP_ADD,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 5, 0xf, 0 },
        .src = { temp[2], temp[3] } },
      { .op = DRM_CHROME9_FRAGMENT_OP_ADD,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 6, 0xf, 0 },
        .src = { temp[4], temp[5] } },
      { .op = DRM_CHROME9_FRAGMENT_OP_MUL,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
        .src = { temp[6], { DRM_CHROME9_FRAGMENT_FILE_CONSTANT, 0,
                            { 0, 1, 2, 3 }, 0, 0 } } },
   };
   /* One bilinear sample at the shared corner of each 2x2 group
    * averages it in the filter unit. */
   static const struct drm_chrome9_fragment_instruction bilinear[2] = {
      { .op = DRM_CHROME9_FRAGMENT_OP_TEX,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
        .src = { { DRM_CHROME9_FRAGMENT_FILE_VARYING, 0, { 0, 1, 2, 3 },
                   0, 0 } } },
      { .op = DRM_CHROME9_FRAGMENT_OP_MOV,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
        .src = { { DRM_CHROME9_FRAGMENT_FILE_TEMP, 0, { 0, 1, 2, 3 },
                   0, 0 } } },
   };
   const bool linear = debug_get_bool_option("CHROME9_MIPMAP_BILINEAR",
                                             true);
   const uint32_t quarter[4] = {
      fui(0.25f), fui(0.25f), fui(0.25f), fui(0.25f),
   };
   struct pipe_resource *pres = &resource->base;
   const bool bgra = pres->format == PIPE_FORMAT_B8G8R8A8_UNORM;
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   struct drm_chrome9_fragment_texture_state texture = { 0 };
   struct drm_chrome9_fragment_vertex vertices[6];
   unsigned first_row = 0;
   uint32_t base = 0;

   if (!ctx->draw_fragment_program ||
       !chrome9_screen_has_capability2(ctx->base.screen,
                                       DRM_CHROME9_CAP2_DESTINATION_OFFSET) ||
       !resource->guarded || resource->guarded_shared ||
       resource->sampler_unsupported || resource->parent ||
       (pres->target != PIPE_TEXTURE_2D && pres->target != PIPE_TEXTURE_1D) ||
       (!bgra && pres->format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
       pres->last_level >= DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS ||
       !debug_get_bool_option("CHROME9_GPU_MIPMAP", true))
      return base_level + 1;
   for (unsigned level = base_level + 1; level <= last_level; level++)
      if (!chrome9_mipmap_window(resource, level, &base, &first_row))
         return base_level + 1;

   texture.handle = resource->bo.handle;
   texture.flags = chrome9_resource_texture_flags(resource);
   texture.format = bgra ? DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888 :
                           DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888;
   texture.width = pres->width0;
   texture.height = pres->height0;
   texture.last_level = pres->last_level;
   texture.min_filter = linear ? DRM_CHROME9_TEXTURE_FILTER_LINEAR :
                                 DRM_CHROME9_TEXTURE_FILTER_NEAREST;
   texture.mag_filter = texture.min_filter;
   texture.mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST;
   texture.wrap_s = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   texture.wrap_t = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   for (unsigned level = 0; level <= pres->last_level; level++) {
      if (resource->level_offset[level] > UINT32_MAX)
         return base_level + 1;
      texture.level_offset[level] = resource->level_offset[level];
      texture.level_pitch[level] = resource->level_stride[level];
   }

   /* Retained draws and a render shadow of the source land first. */
   chrome9_context_sync_resource(ctx, pres, PIPE_MAP_READ);
   for (unsigned level = base_level + 1; level <= last_level; level++) {
      const unsigned sw = u_minify(pres->width0, level - 1);
      const unsigned sh = u_minify(pres->height0, level - 1);
      const unsigned w = u_minify(pres->width0, level);
      const unsigned h = u_minify(pres->height0, level);
      const float s1 = sw == 1 ? 1.0f : 2.0f * w / sw;
      const float t1 = sh == 1 ? 1.0f : 2.0f * h / sh;
      /* Half a source texel to either side of the group corner. */
      const float ds = sw == 1 ? 0.0f : 0.5f / sw;
      const float dt = sh == 1 ? 0.0f : 0.5f / sh;
      struct drm_chrome9_draw_fragment_program draw = {
         .destination_handle = resource->bo.handle,
         .flags = DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED |
                  (resource->tiled ?
                   DRM_CHROME9_FRAGMENT_DESTINATION_TILED : 0),
         .triangle_count = 2,
         .width = w,
         .destination_format = bgra ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888,
         .instruction_count = linear ? ARRAY_SIZE(bilinear) :
                                       ARRAY_SIZE(program),
         .constant_count = !linear,
         .texture_count = 1,
         .varying_count = linear ? 1 : 4,
         .abi_version = DRM_CHROME9_FRAGMENT_ABI_VERSION,
         .vertices_ptr = (uintptr_t)vertices,
         .instructions_ptr = (uintptr_t)(linear ? bilinear : program),
         .constants_ptr = linear ? 0 : (uintptr_t)quarter,
         .textures_ptr = (uintptr_t)&texture,
      };

      chrome9_mipmap_window(resource, level, &base, &first_row);
      draw.destination_offset = base;
      draw.destination_pitch = resource->level_stride[level];
      draw.height = h + first_row;
      if (first_row) {
         draw.scissor.flags = DRM_CHROME9_SCISSOR_ENABLE;
         draw.scissor.max_x = w;
         draw.scissor.min_y = first_row;
         draw.scissor.max_y = h + first_row;
      }
      texture.min_lod_s5_5 = (level - 1) * 32;
      texture.max_lod_s5_5 = (level - 1) * 32;
      memset(vertices, 0, sizeof(vertices));
      for (unsigned v = 0; v < 6; v++) {
         /* Pixel centres are at integer window coordinates. */
         vertices[v].x_16_16 = (int32_t)(corner[v][0] * w << 16) - 0x8000;
         vertices[v].y_16_16 =
            (int32_t)((first_row + corner[v][1] * h) << 16) - 0x8000;
         vertices[v].rhw_float = fui(1.0f);
         for (unsigned k = 0; k < draw.varying_count; k++) {
            vertices[v].varying[k][0] =
               fui(corner[v][0] * s1 + (linear ? 0 : k & 1 ? ds : -ds));
            vertices[v].varying[k][1] =
               fui(corner[v][1] * t1 + (linear ? 0 : k & 2 ? dt : -dt));
            vertices[v].varying[k][3] = fui(1.0f);
         }
      }
      if (!chrome9_draw_fragment_program(fd, &draw))
         return level;
   }
   return last_level + 1;
}

/*
 * Blit a box of a BGRA/BGRX level (of cube face src_face) to the top left
 * of a level (of cube face dst_face), scaled to dw x dh, on the 3D engine
 * with NEAREST samples at texel centres: exact when unscaled, a nearest
 * resample otherwise.  Either side may be tiled or local, so the CPU never
 * detiles nor reads write-combined memory.  The blit is queued behind
 * submitted jobs, the caller flushes retained work first; CPU access waits
 * for it.
 */
static bool
chrome9_resource_blit_gpu(struct chrome9_context *ctx,
                          struct chrome9_resource *dst, unsigned dst_level,
                          unsigned dst_face, unsigned dw, unsigned dh,
                          struct chrome9_resource *src, unsigned level,
                          unsigned src_face, const struct pipe_box *box)
{
   static const unsigned corner[6][2] = {
      { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 },
   };
   static const struct drm_chrome9_fragment_instruction program[2] = {
      { .op = DRM_CHROME9_FRAGMENT_OP_TEX,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_TEMP, 0, 0xf, 0 },
        .src = { { DRM_CHROME9_FRAGMENT_FILE_VARYING, 0, { 0, 1, 2, 3 },
                   0, 0 } } },
      { .op = DRM_CHROME9_FRAGMENT_OP_MOV,
        .dst = { DRM_CHROME9_FRAGMENT_DEST_COLOR, 0, 0xf, 0 },
        .src = { { DRM_CHROME9_FRAGMENT_FILE_TEMP, 0, { 0, 1, 2, 3 },
                   0, 0 } } },
   };
   const struct pipe_resource *spres = &src->base, *dpres = &dst->base;
   const bool bgra = spres->format == PIPE_FORMAT_B8G8R8A8_UNORM;
   const unsigned lw = u_minify(spres->width0, level);
   const unsigned lh = u_minify(spres->height0, level);
   const unsigned x = box->x, y = box->y, w = box->width, h = box->height;
   const uint64_t src_face_offset = (uint64_t)src_face * src->layer_stride;
   const uint64_t dst_offset = dst->level_offset[dst_level] +
                               (uint64_t)dst_face * dst->layer_stride;
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   struct drm_chrome9_fragment_texture_state texture = { 0 };
   struct drm_chrome9_fragment_vertex vertices[6];
   struct drm_chrome9_draw_fragment_program draw = {
      .destination_handle = dst->bo.handle,
      .flags = DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED |
               (chrome9_resource_is_tiled(dst) ?
                DRM_CHROME9_FRAGMENT_DESTINATION_TILED : 0),
      .triangle_count = 2,
      .width = dw,
      .height = dh,
      .destination_format = bgra ? DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
                                   DRM_CHROME9_DESTINATION_FORMAT_BGRX8888,
      .instruction_count = ARRAY_SIZE(program),
      .texture_count = 1,
      .varying_count = 1,
      .abi_version = DRM_CHROME9_FRAGMENT_ABI_VERSION,
      .vertices_ptr = (uintptr_t)vertices,
      .instructions_ptr = (uintptr_t)program,
      .textures_ptr = (uintptr_t)&texture,
   };

   if (!ctx->draw_fragment_program ||
       !chrome9_screen_has_capability2(ctx->base.screen,
                                       DRM_CHROME9_CAP2_DESTINATION_OFFSET) ||
       !src->guarded || !dst->guarded || src->sampler_unsupported ||
       spres->format != dpres->format ||
       (!bgra && spres->format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
       (spres->target != PIPE_TEXTURE_2D &&
        spres->target != PIPE_TEXTURE_CUBE) ||
       (dpres->target != PIPE_TEXTURE_2D &&
        dpres->target != PIPE_TEXTURE_CUBE) ||
       src_face >= spres->array_size || dst_face >= dpres->array_size ||
       level > spres->last_level || dst_level > dpres->last_level ||
       box->x < 0 || box->y < 0 || !w || !h || x + w > lw || y + h > lh ||
       !dw || !dh || u_minify(dpres->width0, dst_level) < dw ||
       u_minify(dpres->height0, dst_level) < dh || dw > 2048 || dh > 2048 ||
       spres->last_level >= DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS ||
       dst_offset > UINT32_MAX)
      return false;
   texture.handle = src->bo.handle;
   texture.flags = chrome9_resource_texture_flags(src);
   texture.format = bgra ? DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888 :
                           DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888;
   texture.width = spres->width0;
   texture.height = spres->height0;
   texture.last_level = spres->last_level;
   texture.min_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
   texture.mag_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
   texture.mip_filter = spres->last_level ?
                        DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST :
                        DRM_CHROME9_TEXTURE_MIP_FILTER_NONE;
   texture.wrap_s = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   texture.wrap_t = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   texture.min_lod_s5_5 = level * 32;
   texture.max_lod_s5_5 = level * 32;
   /* A cube face is sampled as the 2D image at its face offset. */
   for (unsigned l = 0; l <= spres->last_level; l++) {
      if (src->level_offset[l] + src_face_offset > UINT32_MAX)
         return false;
      texture.level_offset[l] = src->level_offset[l] + src_face_offset;
      texture.level_pitch[l] = src->level_stride[l];
   }
   draw.destination_offset = dst_offset;
   draw.destination_pitch = dst->level_stride[dst_level];
   memset(vertices, 0, sizeof(vertices));
   for (unsigned v = 0; v < 6; v++) {
      /* Pixel centres at integer window coordinates, texel centres at
       * (i + 0.5) / size. */
      vertices[v].x_16_16 = (int32_t)(corner[v][0] * dw << 16) - 0x8000;
      vertices[v].y_16_16 = (int32_t)(corner[v][1] * dh << 16) - 0x8000;
      vertices[v].rhw_float = fui(1.0f);
      vertices[v].varying[0][0] =
         fui((float)(x + corner[v][0] * w) / (float)lw);
      vertices[v].varying[0][1] =
         fui((float)(y + corner[v][1] * h) / (float)lh);
      vertices[v].varying[0][3] = fui(1.0f);
   }
   return chrome9_draw_fragment_program(fd, &draw);
}

/*
 * Copy a BGRA/BGRX level, or a box of it, to the top left of a level at
 * least as large on the 3D engine (chrome9_resource_blit_gpu, unscaled).
 */
bool
chrome9_resource_copy_gpu(struct chrome9_context *ctx,
                          struct chrome9_resource *dst, unsigned dst_level,
                          struct chrome9_resource *src, unsigned level,
                          const struct pipe_box *box)
{
   struct pipe_box whole;

   if (!box) {
      u_box_2d(0, 0, u_minify(src->base.width0, level),
               u_minify(src->base.height0, level), &whole);
      box = &whole;
   }
   return src->base.target == PIPE_TEXTURE_2D &&
          dst->base.target == PIPE_TEXTURE_2D &&
          chrome9_resource_blit_gpu(ctx, dst, dst_level, 0, box->width,
                                    box->height, src, level, 0, box);
}

/*
 * Resample every face and level of an NPOT cube into the POT cube pot
 * (same format) on the engine: the cube sampler takes power-of-two faces
 * only.
 */
bool
chrome9_resource_cube_resample_gpu(struct chrome9_context *ctx,
                                   struct chrome9_resource *pot,
                                   struct chrome9_resource *src)
{
   const unsigned levels = MIN2(src->base.last_level, pot->base.last_level);

   for (unsigned face = 0; face < 6; face++)
      for (unsigned level = 0; level <= levels; level++) {
         struct pipe_box box;

         u_box_2d(0, 0, u_minify(src->base.width0, level),
                  u_minify(src->base.height0, level), &box);
         if (!chrome9_resource_blit_gpu(ctx, pot, level, face,
                                        u_minify(pot->base.width0, level),
                                        u_minify(pot->base.height0, level),
                                        src, level, face, &box))
            return false;
      }
   return true;
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

   /* util_gen_mipmap would need pipe->blit, which this driver lacks. */
   if (pipe && pres && pres->target == PIPE_TEXTURE_3D)
      return chrome9_generate_volume_mipmap(pipe, pres, format, base_level,
                                            last_level);
   format_info = chrome9_texture_format_info(format);
   if (!pipe || !pres || !pipe->texture_map || !pipe->texture_unmap ||
       (pres->target != PIPE_TEXTURE_2D &&
        pres->target != PIPE_TEXTURE_1D &&
        pres->target != PIPE_TEXTURE_CUBE) || pres->depth0 != 1 ||
       pres->nr_samples > 1 ||
       pres->nr_storage_samples > 1 || format != pres->format ||
       chrome9_format_is_d24(format) || chrome9_format_is_dxt(format) ||
       (!format_info && format != PIPE_FORMAT_Z16_UNORM) ||
       base_level > last_level || last_level > pres->last_level ||
       first_layer > last_layer || last_layer >= pres->array_size)
      return false;

   resource = chrome9_resource(pres);
   cpp = format_info ? format_info->bytes_per_pixel : 2;
   if (!resource->guarded || !resource->bo.map)
      return false;
   for (unsigned layer = first_layer; layer <= last_layer; layer++)
      for (unsigned level = base_level; level <= last_level; level++)
         if (!chrome9_mipmap_level_valid(resource, level, layer, cpp))
            return false;

   if (first_layer == last_layer && pres->target != PIPE_TEXTURE_CUBE) {
      unsigned level = chrome9_generate_mipmap_gpu(chrome9_context(pipe),
                                                   resource, base_level,
                                                   last_level);

      if (level > last_level)
         return true;
      base_level = level - 1;
   }

   /* The first level may still be queued in a retained GPU draw batch. */
   chrome9_draw_flush_batch(chrome9_context(pipe));
   chrome9_resource_cpu_access(resource, true);

   for (unsigned layer = first_layer; layer <= last_layer; layer++)
      for (unsigned level = base_level + 1; level <= last_level; level++)
         if (!chrome9_mipmap_downsample(pipe, resource, format, cpp, level,
                                        layer))
            return false;
   return true;
}

/*
 * LINEAR modifier imports and exports use the multi-plane DRI3 and KMS
 * paths, which carry the plane offset: such shared surfaces keep the
 * canonical guarded layout (pixels at BO + PAGE) and need no shadow.
 */
static bool
chrome9_modifier_format(enum pipe_format format)
{
   return format == PIPE_FORMAT_B8G8R8X8_UNORM ||
          format == PIPE_FORMAT_B8G8R8A8_UNORM;
}

static void
chrome9_query_dmabuf_modifiers(struct pipe_screen *screen,
                               enum pipe_format format, int max,
                               uint64_t *modifiers,
                               unsigned int *external_only, int *count)
{
   /* With implicitly tiled back buffers DRI3 must not ask for LINEAR. */
   *count = chrome9_modifier_format(format) &&
            !(chrome9_screen_has_capability2(screen,
                                             DRM_CHROME9_CAP2_TILED_SHARED) &&
              debug_get_bool_option("CHROME9_TILED_SHARED", true)) ? 1 : 0;
   if (max && *count) {
      modifiers[0] = DRM_FORMAT_MOD_LINEAR;
      if (external_only)
         external_only[0] = false;
   }
}

static bool
chrome9_is_dmabuf_modifier_supported(struct pipe_screen *screen,
                                     uint64_t modifier,
                                     enum pipe_format format,
                                     bool *external_only)
{
   (void)screen;
   if (external_only)
      *external_only = false;
   return modifier == DRM_FORMAT_MOD_LINEAR && chrome9_modifier_format(format);
}

static struct pipe_resource *
chrome9_resource_create_with_modifiers(struct pipe_screen *screen,
                                       const struct pipe_resource *templ,
                                       const uint64_t *modifiers, int count)
{
   struct pipe_resource canonical = *templ;
   bool linear = false;

   for (int i = 0; i < count; i++)
      linear |= modifiers[i] == DRM_FORMAT_MOD_LINEAR;
   if (!linear || templ->target != PIPE_TEXTURE_2D || templ->last_level ||
       !chrome9_modifier_format(templ->format) ||
       !debug_get_bool_option("CHROME9_GUARDED_SHARED", true))
      return NULL;
   canonical.flags |= CHROME9_RESOURCE_FLAG_CANONICAL_SHARED;
   return chrome9_resource_create(screen, &canonical);
}

void
chrome9_init_screen_resource_functions(struct pipe_screen *screen)
{
   screen->is_format_supported = chrome9_is_format_supported;
   screen->can_create_resource = chrome9_can_create_resource;
   screen->resource_create = chrome9_resource_create;
   screen->resource_create_with_modifiers =
      chrome9_resource_create_with_modifiers;
   screen->query_dmabuf_modifiers = chrome9_query_dmabuf_modifiers;
   screen->is_dmabuf_modifier_supported =
      chrome9_is_dmabuf_modifier_supported;
   screen->resource_from_handle = chrome9_resource_from_handle;
   screen->resource_get_handle = chrome9_resource_get_handle;
   screen->resource_destroy = chrome9_resource_destroy;
}

void
chrome9_init_context_resource_functions(struct pipe_context *pipe)
{
   pipe->buffer_map = chrome9_transfer_map;
   pipe->invalidate_resource = chrome9_invalidate_resource;
   pipe->buffer_unmap = chrome9_transfer_unmap;
   pipe->texture_map = chrome9_transfer_map;
   pipe->texture_unmap = chrome9_transfer_unmap;
   pipe->generate_mipmap = chrome9_generate_mipmap;
   pipe->resource_copy_region = util_resource_copy_region;
   pipe->transfer_flush_region = u_default_transfer_flush_region;
   pipe->buffer_subdata = u_default_buffer_subdata;
   pipe->texture_subdata = u_default_texture_subdata;
}
