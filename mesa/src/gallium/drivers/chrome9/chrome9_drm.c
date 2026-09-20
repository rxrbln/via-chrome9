/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_drm.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "drm-uapi/chrome9_drm.h"

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
chrome9_bo_create(int fd, uint64_t size, struct chrome9_bo *bo)
{
   struct drm_chrome9_gem_create args = { .size = size };

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
   bo->size = args.size;
   bo->mmap_offset = args.mmap_offset;
   return true;
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
   if (args.handle)
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
