/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_public.h"
#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_resource.h"

#include <fcntl.h>
#include <unistd.h>

#include "draw/draw_context.h"
#include "drm-uapi/chrome9_drm.h"
#include "nir.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_memory.h"
#include "util/u_screen.h"

struct chrome9_screen {
   struct pipe_screen base;
   int fd;
   uint32_t device_id;
   uint32_t revision;
   uint64_t capabilities;
};

static inline struct chrome9_screen *
chrome9_screen(struct pipe_screen *screen)
{
   return (struct chrome9_screen *)screen;
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
   .lower_fdiv = true,
   .lower_flrp32 = true,
   .lower_fmod = true,
   .lower_fsat = true,
   .lower_sincos = true,
};

static void
chrome9_init_shader_caps(struct chrome9_screen *screen)
{
   struct pipe_shader_caps *vs =
      (struct pipe_shader_caps *)&screen->base.shader_caps[MESA_SHADER_VERTEX];
   struct pipe_shader_caps *fs =
      (struct pipe_shader_caps *)&screen->base.shader_caps[MESA_SHADER_FRAGMENT];

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
   /* Mesa's compatibility context requires at least one logical unit. */
   fs->max_texture_samplers =
      (screen->capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE) ?
      2 : 1;
   fs->max_sampler_views = fs->max_texture_samplers;
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
   caps->clip_planes = 0;
   caps->dithering = false;
   caps->mixed_colorbuffer_formats = false;
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
   caps->vendor_id = 0x1106;
   caps->device_id = screen->device_id;
   caps->endianness = PIPE_ENDIAN_NATIVE;
   caps->max_texture_2d_size = 2048;
   caps->max_render_targets = 1;
   caps->max_vertex_buffers = 16;
   caps->max_varyings = 2;
   caps->constant_buffer_offset_alignment = 16;
   caps->user_vertex_buffers = true;
   caps->vertex_color_clamped = true;
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->alpha_test =
      (screen->capabilities &
       (DRM_CHROME9_CAP_ALPHA_TEST |
        DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND)) ==
      (DRM_CHROME9_CAP_ALPHA_TEST |
       DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND);
   caps->shareable_shaders = false;

   screen->base.destroy = chrome9_screen_destroy;
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
   FREE(screen);
   return NULL;
}
