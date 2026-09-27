/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_SWFB_H
#define CHROME9_SWFB_H

#include <stdbool.h>

struct chrome9_context;
struct pipe_context;
struct pipe_draw_info;
struct pipe_draw_start_count_bias;
struct pipe_resource;
union pipe_color_union;
struct pipe_screen;

#ifdef CHROME9_HAVE_LLVMPIPE
struct pipe_screen *chrome9_sw_screen_create(void);
bool chrome9_sw_enabled(void);
bool chrome9_sw_draw_vbo(struct chrome9_context *ctx,
                         const struct pipe_draw_info *info,
                         unsigned drawid_offset,
                         const struct pipe_draw_start_count_bias *draws,
                         unsigned num_draws);
void chrome9_sw_finish(struct chrome9_context *ctx);
bool chrome9_sw_kept_valid(struct chrome9_context *ctx,
                           struct pipe_resource *resource, unsigned level);
struct pipe_resource *chrome9_sw_kept_image(struct chrome9_context *ctx,
                                            struct pipe_resource *resource,
                                            unsigned level);
void chrome9_sw_kept_clear(struct chrome9_context *ctx,
                           struct pipe_resource *resource, unsigned level,
                           const union pipe_color_union *color,
                           unsigned x, unsigned y, unsigned width,
                           unsigned height);
void chrome9_sw_forget(struct chrome9_context *ctx, const void *object);
void chrome9_sw_context_destroy(struct chrome9_context *ctx);
struct pipe_context *chrome9_sw_pipe(struct chrome9_context *ctx);
#else
static inline struct pipe_screen *chrome9_sw_screen_create(void) { return 0; }
static inline bool chrome9_sw_enabled(void) { return false; }
static inline bool
chrome9_sw_draw_vbo(struct chrome9_context *ctx,
                    const struct pipe_draw_info *info, unsigned drawid_offset,
                    const struct pipe_draw_start_count_bias *draws,
                    unsigned num_draws)
{
   (void)ctx; (void)info; (void)drawid_offset; (void)draws; (void)num_draws;
   return false;
}
static inline void chrome9_sw_finish(struct chrome9_context *ctx) { (void)ctx; }
static inline void
chrome9_sw_forget(struct chrome9_context *ctx, const void *object)
{
   (void)ctx; (void)object;
}
static inline bool
chrome9_sw_kept_valid(struct chrome9_context *ctx,
                      struct pipe_resource *resource, unsigned level)
{
   (void)ctx; (void)resource; (void)level;
   return false;
}
static inline struct pipe_resource *
chrome9_sw_kept_image(struct chrome9_context *ctx,
                      struct pipe_resource *resource, unsigned level)
{
   (void)ctx; (void)resource; (void)level;
   return NULL;
}
static inline void
chrome9_sw_kept_clear(struct chrome9_context *ctx,
                      struct pipe_resource *resource, unsigned level,
                      const union pipe_color_union *color,
                      unsigned x, unsigned y, unsigned width, unsigned height)
{
   (void)ctx; (void)resource; (void)level; (void)color;
   (void)x; (void)y; (void)width; (void)height;
}
static inline void chrome9_sw_context_destroy(struct chrome9_context *ctx) { (void)ctx; }
static inline struct pipe_context *
chrome9_sw_pipe(struct chrome9_context *ctx)
{
   (void)ctx;
   return 0;
}
#endif

#endif
