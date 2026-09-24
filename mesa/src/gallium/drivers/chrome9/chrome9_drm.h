/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_DRM_H
#define CHROME9_DRM_H

#include <stdbool.h>
#include <stdint.h>

#include "drm-uapi/chrome9_drm.h"

struct chrome9_shared_handle;

struct chrome9_bo {
   uint32_t handle;
   uint32_t create_flags;
   uint64_t size;
   uint64_t mmap_offset;
   void *map;
   bool handle_owned;
   struct chrome9_shared_handle *shared;
};

bool chrome9_get_param(int fd, uint32_t param, uint64_t *value);
bool chrome9_bo_create(int fd, uint64_t size, uint32_t flags,
                       struct chrome9_bo *bo);
bool chrome9_bo_export_fd(int fd, const struct chrome9_bo *bo, int *prime_fd);
void chrome9_bo_destroy(int fd, struct chrome9_bo *bo);
bool chrome9_draw_triangle(int fd,
                           const struct drm_chrome9_draw_triangle *draw);
bool chrome9_draw_triangle_d16(
   int fd, const struct drm_chrome9_draw_triangle_d16 *draw);
bool chrome9_draw_triangle_d16_blend(
   int fd, const struct drm_chrome9_draw_triangle_d16_blend *draw);
bool chrome9_draw_triangle_batch(
   int fd, const struct drm_chrome9_draw_triangle_batch *draw);
bool chrome9_draw_triangle_d16_rhw_batch(
   int fd, const struct drm_chrome9_draw_triangle_d16_rhw_batch *draw);
bool chrome9_draw_triangle_d16_rhw_run(
   int fd, const struct drm_chrome9_draw_triangle_d16_rhw_run *draw);
bool chrome9_draw_triangle_d16_rhw_chain(
   int fd, const struct drm_chrome9_draw_triangle_d16_rhw_chain *draw);
bool chrome9_draw_program_triangle_d16_rhw_batch(
   int fd,
   const struct drm_chrome9_draw_program_triangle_d16_rhw_batch *draw);
bool chrome9_draw_vertex_program(
   int fd, const struct drm_chrome9_draw_vertex_program *draw);
bool chrome9_draw_fragment_program(
   int fd, const struct drm_chrome9_draw_fragment_program *draw);
bool chrome9_draw_textured_triangle(
   int fd, const struct drm_chrome9_draw_textured_triangle *draw);
bool chrome9_draw_textured_triangle_batch(
   int fd, const struct drm_chrome9_draw_textured_triangle_batch *draw);
bool chrome9_draw_textured_triangle_run(
   int fd, const struct drm_chrome9_draw_textured_triangle_run *draw);
bool chrome9_draw_textured_triangle_mip(
   int fd, const struct drm_chrome9_draw_textured_triangle_mip *draw);
bool chrome9_copy_rect(int fd, const struct drm_chrome9_copy_rect *copy);
bool chrome9_draw_textured_triangle_d16(
   int fd, const struct drm_chrome9_draw_textured_triangle_d16 *draw);
bool chrome9_draw_textured_triangle_d16_blend(
   int fd, const struct drm_chrome9_draw_textured_triangle_d16_blend *draw);
bool chrome9_draw_two_textured_triangle(
   int fd, const struct drm_chrome9_draw_two_textured_triangle *draw);
bool chrome9_draw_two_textured_triangle_d16(
   int fd, const struct drm_chrome9_draw_two_textured_triangle_d16 *draw);
bool chrome9_draw_two_textured_triangle_d16_blend(
   int fd,
   const struct drm_chrome9_draw_two_textured_triangle_d16_blend *draw);
bool chrome9_draw_triangle_blend(
   int fd, const struct drm_chrome9_draw_triangle_blend *draw);
bool chrome9_draw_textured_triangle_blend(
   int fd, const struct drm_chrome9_draw_textured_triangle_blend *draw);

#endif
