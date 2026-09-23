/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_public.h"
#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_resource.h"
#include "chrome9_specialise.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "draw/draw_context.h"
#include "drm-uapi/chrome9_drm.h"
#include "xf86drm.h"
#include "nir.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_screen.h"
#include "util/simple_mtx.h"

struct chrome9_shared_handle {
   struct chrome9_shared_handle *next;
   uint32_t handle;
   uint32_t references;
   bool owns_handle;
};

struct chrome9_screen {
   struct pipe_screen base;
   int fd;
   uint32_t device_id;
   uint32_t revision;
   uint64_t capabilities;
   simple_mtx_t shared_lock;
   struct chrome9_shared_handle *shared_handles;
};

static inline struct chrome9_screen *
chrome9_screen(struct pipe_screen *screen)
{
   return (struct chrome9_screen *)screen;
}

bool
chrome9_screen_bo_import_fd(struct pipe_screen *screen, int dma_buf_fd,
                            struct chrome9_bo *bo)
{
   struct chrome9_screen *cscreen = chrome9_screen(screen);
   struct chrome9_shared_handle *entry, *spare;
   uint32_t handle;
   off_t size;
   void *map;

   if (!bo || dma_buf_fd < 0)
      return false;
   size = lseek(dma_buf_fd, 0, SEEK_END);
   if (size <= 0 || (uint64_t)size > SIZE_MAX)
      return false;
   spare = CALLOC_STRUCT(chrome9_shared_handle);
   if (!spare)
      return false;

   simple_mtx_lock(&cscreen->shared_lock);
   if (drmPrimeFDToHandle(cscreen->fd, dma_buf_fd, &handle))
      goto fail_unlock;
   for (entry = cscreen->shared_handles; entry; entry = entry->next)
      if (entry->handle == handle)
         break;
   if (entry && entry->references == UINT32_MAX)
      goto fail_unlock;
   map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd, 0);
   if (map == MAP_FAILED) {
      if (!entry) {
         struct drm_gem_close close_bo = { .handle = handle };

         ioctl(cscreen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      }
      goto fail_unlock;
   }
   if (entry) {
      entry->references++;
   } else {
      entry = spare;
      spare = NULL;
      entry->handle = handle;
      entry->references = 1;
      entry->owns_handle = true;
      entry->next = cscreen->shared_handles;
      cscreen->shared_handles = entry;
   }
   simple_mtx_unlock(&cscreen->shared_lock);

   memset(bo, 0, sizeof(*bo));
   bo->handle = handle;
   bo->size = size;
   bo->map = map;
   bo->shared = entry;
   FREE(spare);
   return true;

fail_unlock:
   simple_mtx_unlock(&cscreen->shared_lock);
   FREE(spare);
   return false;
}

bool
chrome9_screen_bo_export_fd(struct pipe_screen *screen, struct chrome9_bo *bo,
                            int *dma_buf_fd)
{
   struct chrome9_screen *cscreen = chrome9_screen(screen);
   struct chrome9_shared_handle *entry, *spare = NULL;
   int exported_fd;

   if (!bo || !bo->handle || !dma_buf_fd)
      return false;
   if (!bo->shared) {
      spare = CALLOC_STRUCT(chrome9_shared_handle);
      if (!spare)
         return false;
   }
   simple_mtx_lock(&cscreen->shared_lock);
   if (!chrome9_bo_export_fd(cscreen->fd, bo, &exported_fd))
      goto fail_unlock;
   if (!bo->shared) {
      for (entry = cscreen->shared_handles; entry; entry = entry->next)
         if (entry->handle == bo->handle)
            break;
      if (entry) {
         if (entry->references == UINT32_MAX) {
            close(exported_fd);
            goto fail_unlock;
         }
         entry->references++;
      } else {
         entry = spare;
         spare = NULL;
         entry->handle = bo->handle;
         entry->references = 1;
         entry->owns_handle = bo->handle_owned;
         entry->next = cscreen->shared_handles;
         cscreen->shared_handles = entry;
      }
      bo->shared = entry;
   }
   simple_mtx_unlock(&cscreen->shared_lock);
   FREE(spare);
   *dma_buf_fd = exported_fd;
   return true;

fail_unlock:
   simple_mtx_unlock(&cscreen->shared_lock);
   FREE(spare);
   return false;
}

void
chrome9_screen_bo_destroy(struct pipe_screen *screen, struct chrome9_bo *bo)
{
   struct chrome9_screen *cscreen = chrome9_screen(screen);
   struct chrome9_shared_handle *entry, **link;

   if (!bo || !bo->shared) {
      chrome9_bo_destroy(cscreen->fd, bo);
      return;
   }
   entry = bo->shared;
   bo->handle_owned = false;
   chrome9_bo_destroy(cscreen->fd, bo);

   simple_mtx_lock(&cscreen->shared_lock);
   for (link = &cscreen->shared_handles; *link; link = &(*link)->next) {
      if (*link != entry)
         continue;
      if (--entry->references == 0) {
         struct drm_gem_close close_bo = { .handle = entry->handle };

         *link = entry->next;
         if (entry->owns_handle)
            ioctl(cscreen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         FREE(entry);
      }
      break;
   }
   simple_mtx_unlock(&cscreen->shared_lock);
}

bool
chrome9_screen_has_capability(struct pipe_screen *screen, uint64_t capability)
{
   return (chrome9_screen(screen)->capabilities & capability) == capability;
}

static int
chrome9_screen_fd(struct pipe_screen *screen)
{
   return chrome9_screen(screen)->fd;
}

static const char *
chrome9_screen_name(struct pipe_screen *screen)
{
   (void)screen;
   return "VIA Chrome9 HD (validated triangle bring-up)";
}

static const char *
chrome9_screen_vendor(struct pipe_screen *screen)
{
   (void)screen;
   return "Mesa";
}

static const char *
chrome9_device_vendor(struct pipe_screen *screen)
{
   (void)screen;
   return "VIA Technologies";
}

static void
chrome9_screen_destroy(struct pipe_screen *screen)
{
   struct chrome9_screen *cscreen = chrome9_screen(screen);

   while (cscreen->shared_handles) {
      struct chrome9_shared_handle *entry = cscreen->shared_handles;
      struct drm_gem_close close_bo = { .handle = entry->handle };

      cscreen->shared_handles = entry->next;
      if (entry->owns_handle)
         ioctl(cscreen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      FREE(entry);
   }
   simple_mtx_destroy(&cscreen->shared_lock);
   close(cscreen->fd);
   FREE(cscreen);
}

static void
chrome9_fence_reference(struct pipe_screen *screen,
                        struct pipe_fence_handle **ptr,
                        struct pipe_fence_handle *fence)
{
   (void)screen;
   (void)fence;
   /* DRAW_TRIANGLE completes synchronously, so the driver never emits a fence. */
   *ptr = NULL;
}

static bool
chrome9_fence_finish(struct pipe_screen *screen, struct pipe_context *ctx,
                     struct pipe_fence_handle *fence, uint64_t timeout)
{
   (void)screen;
   (void)ctx;
   (void)fence;
   (void)timeout;
   return true;
}

static const struct nir_shader_compiler_options chrome9_nir_options = {
   .lower_fceil = true,
   .lower_fdiv = true,
   .lower_ffloor = true,
   .lower_flrp32 = true,
   .lower_fmod = true,
   .lower_fsat = true,
   .lower_sincos = true,
   .lower_uniforms_to_ubo = true,
   /* For a top-tested i < N loop, NIR reports the last body trip as N - 1.
    * A limit of 31 therefore admits exactly 32 body executions; the focused
    * test proves that 32 unrolls and 33 remains explicit control flow. */
   .max_unroll_iterations = 31,
   .no_integers = true,
};

/* Keep specialization metadata on the linked program: the state tracker
 * uses it to deliver exactly the current control-uniform values to the
 * context.  Chrome9 fragment programs are float-only, so integer lowering
 * happens before the metadata is discovered. */
static void
chrome9_finalize_nir(struct pipe_screen *screen, nir_shader *nir,
                     bool optimize)
{
   (void)screen;
   (void)optimize;
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return;
   chrome9_nir_lower_integers(nir);
   chrome9_nir_find_inlinable_uniforms(nir);
}

static void
chrome9_init_shader_caps(struct chrome9_screen *screen)
{
   struct pipe_shader_caps *vs =
      (struct pipe_shader_caps *)&screen->base.shader_caps[MESA_SHADER_VERTEX];
   struct pipe_shader_caps *fs =
      (struct pipe_shader_caps *)&screen->base.shader_caps[MESA_SHADER_FRAGMENT];
   unsigned fs_samplers;
   int debug_fs_samplers;

   draw_init_shader_caps(vs);
   vs->supported_irs = 1 << PIPE_SHADER_IR_NIR;
   /* The draw module executes vertex ALU and constant-buffer accesses, but
    * Chrome9 does not wire its sampler, image, or shader-buffer interfaces.
    * In particular, cso_unbind_context() calls those interfaces whenever the
    * corresponding limits are nonzero.  Do not retain tgsi_exec's generic
    * limits for interfaces which this pipe context does not install. */
   vs->max_tex_instructions = 0;
   vs->max_tex_indirections = 0;
   vs->max_texture_samplers = 0;
   vs->max_sampler_views = 0;
   vs->max_shader_buffers = 0;
   vs->max_shader_images = 0;
   vs->max_hw_atomic_counters = 0;
   vs->max_hw_atomic_counter_buffers = 0;

   fs->supported_irs = 1 << PIPE_SHADER_IR_NIR;
   fs->max_instructions = 32;
   fs->max_alu_instructions = 32;
   fs->max_inputs =
      (screen->capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE) ?
      3 : 2;
   fs->max_outputs = 1;
   fs->max_temps = 8;
   /* The full two-bank hardware store is independently capability-gated;
    * shader execution remains restricted to closed semantic programs. */
   fs->max_const_buffer0_size =
      (screen->capabilities & DRM_CHROME9_CAP_FRAGMENT_CONSTANTS_32) ?
      32 * 4 * sizeof(float) : 4 * sizeof(float);
   fs->max_const_buffers = 1;
   /* Mesa's compatibility context requires at least one logical unit. */
   fs_samplers =
      (screen->capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE) ?
      2 : 1;
   /* Diagnostic-only GL2 link probe.  Draw classification remains closed
    * to live-proven one-/two-unit programs, so this cannot submit an
    * unvalidated higher-unit packet. */
   debug_fs_samplers =
      debug_get_num_option("CHROME9_DEBUG_MAX_FS_SAMPLERS", 0);
   if (debug_fs_samplers > (int)fs_samplers && debug_fs_samplers <= 8)
      fs_samplers = debug_fs_samplers;
   fs->max_texture_samplers = fs_samplers;
   fs->max_sampler_views = fs_samplers;
   if (screen->capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE) {
      fs->max_tex_instructions = fs->max_texture_samplers;
      fs->max_tex_indirections = 1;
   }
}

static void
chrome9_restrict_generic_caps(struct pipe_caps *caps)
{
   /* u_init_pipe_screen_caps() supplies defaults intended for complete
    * Gallium drivers.  Keep this bounded bring-up driver from selecting
    * unimplemented callbacks or advertising unvalidated fixed-function
    * behavior.  Mesa's compatibility state tracker requires GLSL 1.20 and a
    * GL 1.3 floor, so those API-level claims cannot yet be made fully
    * truthful with pipe_caps alone. */
   caps->texture_transfer_modes = PIPE_TEXTURE_TRANSFER_DEFAULT;
   caps->texture_shadow_map = false;
   caps->gl_clamp = false;
   caps->texrect = false;
   caps->native_fp32_depth = false;
   caps->buffer_map_persistent_coherent = false;
   caps->dest_surface_srgb_control = false;
   caps->packed_stream_output = false;
   caps->two_sided_color = false;
   caps->flatshade = false;
   caps->clip_planes = 0;
   caps->dithering = false;
   caps->mixed_colorbuffer_formats = false;
   /* CPU generation uses the guarded, persistently mapped per-level layout
    * and covers every local 2D format which this driver can allocate. */
   caps->generate_mipmap = true;
}

struct pipe_screen *
chrome9_screen_create(int fd, const struct pipe_screen_config *config)
{
   struct chrome9_screen *screen;
   struct pipe_caps *caps;
   uint64_t value;

   (void)config;
   screen = CALLOC_STRUCT(chrome9_screen);
   if (!screen)
      return NULL;
   simple_mtx_init(&screen->shared_lock, mtx_plain);

   screen->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (screen->fd < 0)
      goto fail;

   if (!chrome9_get_param(screen->fd, DRM_CHROME9_PARAM_DEVICE_ID, &value) ||
       value != 0x7122)
      goto fail_fd;
   screen->device_id = value;
   if (!chrome9_get_param(screen->fd, DRM_CHROME9_PARAM_REVISION, &value))
      goto fail_fd;
   screen->revision = value;
   if (!chrome9_get_param(screen->fd, DRM_CHROME9_PARAM_CAPABILITIES, &value) ||
       (value & (DRM_CHROME9_CAP_GEM |
                 DRM_CHROME9_CAP_GEM_SHMEM_CACHED)) !=
          (DRM_CHROME9_CAP_GEM | DRM_CHROME9_CAP_GEM_SHMEM_CACHED))
      goto fail_fd;
   screen->capabilities = value;

   screen->base.get_screen_fd = chrome9_screen_fd;
   u_init_pipe_screen_caps(&screen->base,
                           !!(screen->capabilities &
                              DRM_CHROME9_CAP_DRAW_TRIANGLE));
   screen->base.nir_options[MESA_SHADER_VERTEX] = &chrome9_nir_options;
   screen->base.nir_options[MESA_SHADER_FRAGMENT] = &chrome9_nir_options;
   chrome9_init_shader_caps(screen);
   caps = (struct pipe_caps *)&screen->base.caps;
   chrome9_restrict_generic_caps(caps);
   /* Normalized NPOT sampling is part of the validated textured-triangle
    * contract.  Besides describing the hardware truthfully, this makes DRI
    * import drawable images as PIPE_TEXTURE_2D.  Advertising RECT instead
    * makes st_bind_egl_image reject a GL_TEXTURE_2D pixmap before the driver
    * can create its guarded sampler shadow. */
   caps->npot_textures = chrome9_screen_has_capability(
      &screen->base, DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE);
   caps->vendor_id = 0x1106;
   caps->device_id = screen->device_id;
   caps->endianness = PIPE_ENDIAN_NATIVE;
   caps->max_texture_2d_size = 2048;
   caps->max_render_targets = 1;
   /* The bounded draw paths support a 16-bit Z buffer with both 32-bit
    * XRGB and BGRA color targets.  DRI needs this explicit contract or it
    * filters the otherwise supported Z16 format from every 32-bit visual. */
   caps->mixed_color_depth_bits = true;
   caps->max_vertex_buffers = 16;
   caps->max_varyings = 2;
   caps->constant_buffer_offset_alignment = 16;
   caps->user_vertex_buffers = true;
   caps->vertex_color_clamped = true;
   caps->fs_coord_origin_upper_left = true;
   if (screen->capabilities & DRM_CHROME9_CAP_FRAGMENT_POSITION) {
      /* VX900 vPos is raw D3D9 integer-center XY.  State tracker inserts
       * GL's half-pixel bias and framebuffer-dependent Y transform. */
      caps->fs_coord_pixel_center_integer = true;
      caps->fs_position_is_sysval = true;
   } else {
      caps->fs_coord_pixel_center_half_integer = true;
   }
   caps->alpha_test =
      (screen->capabilities &
       (DRM_CHROME9_CAP_ALPHA_TEST |
        DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND)) ==
      (DRM_CHROME9_CAP_ALPHA_TEST |
       DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND);
   /* Fragment variants are owned by the creating context and selected by its
    * inlinable-constant state. */
   caps->shareable_shaders = false;
   caps->call_finalize_nir_in_linker = true;

   screen->base.destroy = chrome9_screen_destroy;
   screen->base.finalize_nir = chrome9_finalize_nir;
   screen->base.get_name = chrome9_screen_name;
   screen->base.get_vendor = chrome9_screen_vendor;
   screen->base.get_device_vendor = chrome9_device_vendor;
   screen->base.fence_reference = chrome9_fence_reference;
   screen->base.fence_finish = chrome9_fence_finish;
   screen->base.context_create = chrome9_context_create;
   chrome9_init_screen_resource_functions(&screen->base);
   return &screen->base;

fail_fd:
   close(screen->fd);
fail:
   simple_mtx_destroy(&screen->shared_lock);
   FREE(screen);
   return NULL;
}
