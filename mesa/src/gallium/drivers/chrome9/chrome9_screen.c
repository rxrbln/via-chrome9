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

   fs->supported_irs = 1 << PIPE_SHADER_IR_NIR;
   fs->max_instructions = 32;
   fs->max_alu_instructions = 32;
   fs->max_inputs = 2;
   fs->max_outputs = 1;
   fs->max_temps = 8;
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
   caps->shareable_shaders = false;

   screen->base.destroy = chrome9_screen_destroy;
   screen->base.get_name = chrome9_screen_name;
   screen->base.get_vendor = chrome9_screen_vendor;
   screen->base.get_device_vendor = chrome9_device_vendor;
   screen->base.context_create = chrome9_context_create;
   chrome9_init_screen_resource_functions(&screen->base);
   return &screen->base;

fail_fd:
   close(screen->fd);
fail:
   FREE(screen);
   return NULL;
}
