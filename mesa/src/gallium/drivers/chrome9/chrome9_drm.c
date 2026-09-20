/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_drm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm-uapi/chrome9_drm.h"
#include "xf86drm.h"

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
   bo->handle_owned = true;
   return true;
}

bool
chrome9_bo_import_fd(int fd, int prime_fd, struct chrome9_bo *bo)
{
   uint32_t handle;
   off_t size;
   void *map;

   if (!bo || prime_fd < 0)
      return false;
   memset(bo, 0, sizeof(*bo));
   size = lseek(prime_fd, 0, SEEK_END);
   if (size <= 0 || (uint64_t)size > SIZE_MAX ||
       drmPrimeFDToHandle(fd, prime_fd, &handle))
      return false;

   map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, prime_fd, 0);
   if (map == MAP_FAILED) {
      struct drm_gem_close close_args = { .handle = handle };

      ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_args);
      return false;
   }
   bo->handle = handle;
   bo->size = size;
   bo->map = map;
   /* drmPrimeFDToHandle can return an existing handle in this drm_file.
    * Closing it from an alias would invalidate the original resource.  Keep
    * imported handles until the screen fd closes; add a handle cache before
    * making repeated imports a long-lived production path. */
   bo->handle_owned = false;
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
chrome9_draw_textured_triangle(
   int fd, const struct drm_chrome9_draw_textured_triangle *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE, draw) == 0;
}
