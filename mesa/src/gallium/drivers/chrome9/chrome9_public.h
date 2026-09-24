/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_PUBLIC_H
#define CHROME9_PUBLIC_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_screen;
struct pipe_screen_config;
struct chrome9_bo;

struct pipe_screen *
chrome9_screen_create(int fd, const struct pipe_screen_config *config);

bool chrome9_screen_has_capability(struct pipe_screen *screen,
                                   uint64_t capability);
bool chrome9_screen_bo_import_fd(struct pipe_screen *screen, int dma_buf_fd,
                                 struct chrome9_bo *bo);
bool chrome9_screen_bo_export_fd(struct pipe_screen *screen,
                                 struct chrome9_bo *bo, int *dma_buf_fd);
void chrome9_screen_bo_destroy(struct pipe_screen *screen,
                               struct chrome9_bo *bo);
/* Lazily created llvmpipe screen for the fragment fallback, or NULL. */
struct pipe_screen *chrome9_screen_sw(struct pipe_screen *screen);

#endif
