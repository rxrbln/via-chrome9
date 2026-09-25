/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_drm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm-uapi/chrome9_drm.h"
#include "util/macros.h"
#include "xf86drm.h"

_Static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_rhw_run) == 128,
               "Chrome9 RHW run ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_rhw_chain) == 128,
               "Chrome9 RHW chain ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_batch) == 160,
               "Chrome9 textured batch ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_run) == 160,
               "Chrome9 textured run ABI size");
_Static_assert(offsetof(struct drm_chrome9_draw_textured_triangle_run,
                        vertices_ptr) == 128,
               "Chrome9 textured run vertex pointer offset");
_Static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_mip) == 176,
               "Chrome9 mipmapped textured run ABI size");
_Static_assert(offsetof(struct drm_chrome9_draw_textured_triangle_mip,
                        vertices_ptr) == 144,
               "Chrome9 mipmapped textured run vertex pointer offset");
_Static_assert(sizeof(struct drm_chrome9_copy_rect) == 96,
               "Chrome9 copy rectangle ABI size");
_Static_assert(sizeof(struct drm_chrome9_gem_create) == 24,
               "Chrome9 GEM create ABI size");
_Static_assert(sizeof(struct drm_chrome9_fragment_source) == 8,
               "Chrome9 fragment source ABI size");
_Static_assert(sizeof(struct drm_chrome9_fragment_destination) == 4,
               "Chrome9 fragment destination ABI size");
_Static_assert(sizeof(struct drm_chrome9_fragment_instruction) == 32,
               "Chrome9 fragment instruction ABI size");
_Static_assert(sizeof(struct drm_chrome9_fragment_vertex) == 152,
               "Chrome9 fragment vertex ABI size");
_Static_assert(sizeof(struct drm_chrome9_fragment_texture_state) == 176,
               "Chrome9 fragment texture ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_fragment_program) == 184,
               "Chrome9 fragment draw ABI size");
_Static_assert(sizeof(struct drm_chrome9_vertex_instruction) == 32,
               "Chrome9 vertex instruction ABI size");
_Static_assert(sizeof(struct drm_chrome9_draw_vertex_program) == 344,
               "Chrome9 vertex draw ABI size");
_Static_assert(sizeof(struct drm_chrome9_query) == 48,
               "Chrome9 query ABI size");

/* 0 or the negative errno; interrupted waits restart. */
int
chrome9_query_ioctl(int fd, struct drm_chrome9_query *args)
{
   int ret;

   do {
      ret = ioctl(fd, DRM_IOCTL_CHROME9_QUERY, args);
   } while (ret && (errno == EINTR || errno == EAGAIN));
   return ret ? -errno : 0;
}

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
chrome9_bo_cpu_access_range(int fd, const struct chrome9_bo *bo,
                            uint32_t flags, uint64_t offset, uint64_t size)
{
   struct drm_chrome9_gem_cpu_access args = {
      .handle = bo->handle,
      .flags = flags,
      .offset = offset,
      .size = size,
   };

   return drmIoctl(fd, DRM_IOCTL_CHROME9_GEM_CPU_ACCESS, &args) == 0;
}

bool
chrome9_bo_busy(int fd, const struct chrome9_bo *bo, uint32_t *busy)
{
   struct drm_chrome9_gem_busy args = { .handle = bo->handle };

   if (drmIoctl(fd, DRM_IOCTL_CHROME9_GEM_BUSY, &args))
      return false;
   *busy = args.busy;
   return true;
}

bool
chrome9_bo_cpu_access(int fd, const struct chrome9_bo *bo, uint32_t flags)
{
   return chrome9_bo_cpu_access_range(fd, bo, flags, 0, 0);
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
chrome9_fence_last(int fd, uint64_t *seqno)
{
   struct drm_chrome9_fence args = { .op = DRM_CHROME9_FENCE_OP_LAST };

   if (drmIoctl(fd, DRM_IOCTL_CHROME9_FENCE, &args))
      return false;
   *seqno = args.seqno;
   return true;
}

bool
chrome9_fence_wait(int fd, uint64_t seqno, uint64_t timeout_ns)
{
   struct drm_chrome9_fence args = {
      .op = DRM_CHROME9_FENCE_OP_WAIT,
      .seqno = seqno,
      .timeout_ns = timeout_ns,
   };

   return drmIoctl(fd, DRM_IOCTL_CHROME9_FENCE, &args) == 0;
}

bool
chrome9_bo_info(int fd, const struct chrome9_bo *bo, uint32_t *flags)
{
   struct drm_chrome9_gem_info args = { .handle = bo->handle };

   if (drmIoctl(fd, DRM_IOCTL_CHROME9_GEM_INFO, &args))
      return false;
   *flags = args.flags;
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
chrome9_draw_triangle_d16_rhw_chain(
   int fd, const struct drm_chrome9_draw_triangle_d16_rhw_chain *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_CHAIN, draw) == 0;
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

/* CHROME9_DUMP_FAILED_CMD22=<dir> saves a rejected or timed-out cmd22
 * submission (header struct, then instructions, constants, textures and
 * vertices) for chrome9_cmd22_replay. */
static void
chrome9_dump_failed_fragment_program(
   const struct drm_chrome9_draw_fragment_program *draw, int error)
{
   static unsigned sequence;
   const char *dir = getenv("CHROME9_DUMP_FAILED_CMD22");
   const struct {
      uint64_t ptr;
      size_t size;
   } arrays[] = {
      { draw->instructions_ptr, (size_t)draw->instruction_count *
        sizeof(struct drm_chrome9_fragment_instruction) },
      { draw->constants_ptr, (size_t)draw->constant_count * 16 },
      { draw->textures_ptr, (size_t)draw->texture_count *
        sizeof(struct drm_chrome9_fragment_texture_state) },
      /* A corrupt count must not make the dump read unbounded memory. */
      { draw->vertices_ptr,
        (size_t)MIN2(draw->triangle_count,
                     DRM_CHROME9_FRAGMENT_MAX_TRIANGLES) * 3 *
        sizeof(struct drm_chrome9_fragment_vertex) },
   };
   const uint32_t magic[2] = { 0xc9d02201, (uint32_t)error };
   char path[4096];
   FILE *file;

   if (!dir)
      return;
   snprintf(path, sizeof(path), "%s/cmd22-%d-%u.bin", dir, (int)getpid(),
            sequence++);
   file = fopen(path, "wb");
   if (!file)
      return;
   fwrite(magic, sizeof(magic), 1, file);
   fwrite(draw, sizeof(*draw), 1, file);
   for (unsigned i = 0; i < sizeof(arrays) / sizeof(arrays[0]); i++)
      if (arrays[i].ptr && arrays[i].size)
         fwrite((const void *)(uintptr_t)arrays[i].ptr, arrays[i].size, 1,
                file);
   fclose(file);
   fprintf(stderr, "chrome9: failed cmd22 (%d) saved to %s\n", error, path);
}

bool
chrome9_draw_fragment_program(
   int fd, const struct drm_chrome9_draw_fragment_program *draw)
{
   if (!draw)
      return false;
   if (ioctl(fd, DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM, draw) == 0)
      return true;
   chrome9_dump_failed_fragment_program(draw, errno);
   return false;
}

bool
chrome9_draw_vertex_program(
   int fd, const struct drm_chrome9_draw_vertex_program *draw)
{
   return draw && ioctl(fd, DRM_IOCTL_CHROME9_DRAW_VERTEX_PROGRAM, draw) == 0;
}

bool
chrome9_draw_vertex_programs(
   int fd, const struct drm_chrome9_draw_vertex_program *draws,
   const struct drm_chrome9_draw_colors *colors, unsigned count)
{
   struct drm_chrome9_draw_vertex_programs args = {
      .count = count,
      .draws_ptr = (uintptr_t)draws,
      .colors_ptr = (uintptr_t)colors,
   };

   return draws && count &&
          ioctl(fd, DRM_IOCTL_CHROME9_DRAW_VERTEX_PROGRAMS, &args) == 0;
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
chrome9_draw_textured_triangle_mip(
   int fd, const struct drm_chrome9_draw_textured_triangle_mip *draw)
{
   if (!draw)
      return false;
   return ioctl(fd, DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP, draw) == 0;
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
