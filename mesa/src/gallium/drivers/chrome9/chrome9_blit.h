/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_BLIT_H
#define CHROME9_BLIT_H

struct pipe_context;
struct pipe_blit_info;
struct pipe_surface;
union pipe_color_union;

void chrome9_blit(struct pipe_context *pipe, const struct pipe_blit_info *info);
void chrome9_clear_surface_masked(struct pipe_context *pipe,
                                  struct pipe_surface *surface,
                                  const union pipe_color_union *color,
                                  unsigned mask, unsigned x, unsigned y,
                                  unsigned width, unsigned height);

#endif
