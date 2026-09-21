/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_drm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm-uapi/chrome9_drm.h"
#include "xf86drm.h"

_Static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_rhw_run) == 128,
               "Chrome9 RHW run ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_batch) == 160,
               "Chrome9 textured batch ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_run) == 160,
               "Chrome9 textured run ABI size");
_Static_assert(offsetof(struct drm_chrome9_draw_textured_triangle_run,
                        vertices_ptr) == 128,
               "Chrome9 textured run vertex pointer offset");
_Static_assert(sizeof(struct drm_chrome9_copy_rect) == 96,
               "Chrome9 copy rectangle ABI size");
_Static_assert(sizeof(struct drm_chrome9_gem_create) == 24,
               "Chrome9 GEM create ABI size");

bool
chrome9_get_param(int fd, uint32_t param, uint64_t *value)
{
   struct drm_chrome9_get_param args = { .param = param };

   if (!value || ioctl(fd, DRM_IOCTL_CHROME9_GET_PARAM, &args))
      return false;
   *value = args.value;
   return true;
}

bool
chrome9_bo_create(int fd, uint64_t size, uint32_t flags,
                  struct chrome9_bo *bo)
{
   struct drm_chrome9_gem_create args = { .size = size, .flags = flags };

   if (!bo || !size)
      return false;
   memset(bo, 0, sizeof(*bo));
   if (ioctl(fd, DRM_IOCTL_CHROME9_GEM_CREATE, &args))
      return false;

   bo->map = mmap(NULL, args.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, args.mmap_offset);
   if (bo->map == MAP_FAILED) {
      struct drm_gem_close close_args = { .handle = args.handle };

      bo->map = NULL;
      ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_args);
      return false;
   }

   bo->handle = args.handle;
   bo->create_flags = args.flags;
   bo->size = args.size;
   bo->mmap_offset = args.mmap_offset;
   bo->handle_owned = true;
   return true;
}

bool
chrome9_bo_export_fd(int fd, const struct chrome9_bo *bo, int *prime_fd)
{
   if (!bo || !bo->handle || !prime_fd)
      return false;
   return drmPrimeHandleToFD(fd, bo->handle, DRM_CLOEXEC | DRM_RDWR,
                             prime_fd) == 0;
}

void
chrome9_bo_destroy(int fd, struct chrome9_bo *bo)
{
   struct drm_gem_close args;

   if (!bo)
      return;
   if (bo->map)
      munmap(bo->map, bo->size);
   args.handle = bo->handle;
   args.pad = 0;
   if (args.handle && bo->handle_owned)
      ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
   memset(bo, 0, sizeof(*bo));
}

bool
chrome9_draw_triangle(int fd, const struct drm_chrome9_draw_triangle *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE, draw) == 0;
}

bool
chrome9_draw_triangle_d16(
   int fd, const struct drm_chrome9_draw_triangle_d16 *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16, draw) == 0;
}

bool
chrome9_draw_triangle_d16_blend(
   int fd, const struct drm_chrome9_draw_triangle_d16_blend *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_BLEND, draw) == 0;
}

bool
chrome9_draw_triangle_batch(
   int fd, const struct drm_chrome9_draw_triangle_batch *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_BATCH, draw) == 0;
}

bool
chrome9_draw_triangle_d16_rhw_batch(
   int fd, const struct drm_chrome9_draw_triangle_d16_rhw_batch *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_BATCH, draw) == 0;
}

bool
chrome9_draw_triangle_d16_rhw_run(
   int fd, const struct drm_chrome9_draw_triangle_d16_rhw_run *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN, draw) == 0;
}

bool
chrome9_draw_program_triangle_d16_rhw_batch(
   int fd,
   const struct drm_chrome9_draw_program_triangle_d16_rhw_batch *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH,
                draw) == 0;
}

bool
chrome9_draw_textured_triangle(
   int fd, const struct drm_chrome9_draw_textured_triangle *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE, draw) == 0;
}

bool
chrome9_draw_textured_triangle_batch(
   int fd, const struct drm_chrome9_draw_textured_triangle_batch *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_BATCH, draw) == 0;
}

bool
chrome9_draw_textured_triangle_run(
   int fd, const struct drm_chrome9_draw_textured_triangle_run *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_RUN, draw) == 0;
}

bool
chrome9_copy_rect(int fd, const struct drm_chrome9_copy_rect *copy)
{
   if (!copy)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_COPY_RECT, copy) == 0;
}

bool
chrome9_draw_textured_triangle_d16(
   int fd, const struct drm_chrome9_draw_textured_triangle_d16 *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_D16, draw) == 0;
}

bool
chrome9_draw_textured_triangle_d16_blend(
   int fd, const struct drm_chrome9_draw_textured_triangle_d16_blend *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_D16_BLEND,
                draw) == 0;
}

bool
chrome9_draw_two_textured_triangle(
   int fd, const struct drm_chrome9_draw_two_textured_triangle *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE, draw) == 0;
}

bool
chrome9_draw_two_textured_triangle_d16(
   int fd, const struct drm_chrome9_draw_two_textured_triangle_d16 *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16,
                draw) == 0;
}

bool
chrome9_draw_two_textured_triangle_d16_blend(
   int fd,
   const struct drm_chrome9_draw_two_textured_triangle_d16_blend *draw)
{
   if (!draw)
      return false;
   return ioctl(fd,
                DRM_IOCTL_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND,
                draw) == 0;
}

bool
chrome9_draw_triangle_blend(
   int fd, const struct drm_chrome9_draw_triangle_blend *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_BLEND, draw) == 0;
}

bool
chrome9_draw_textured_triangle_blend(
   int fd, const struct drm_chrome9_draw_textured_triangle_blend *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_BLEND, draw) == 0;
}
