/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_RESOURCE_H
#define CHROME9_RESOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "chrome9_drm.h"
#include "pipe/p_state.h"

struct pipe_context;
struct pipe_screen;

struct chrome9_resource {
   struct pipe_resource base;
   struct chrome9_bo bo;
   uint32_t level_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_offset[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_size[PIPE_MAX_TEXTURE_LEVELS];
   /* Level-zero aliases retained for the existing draw/ioctl contract. */
   uint32_t stride;
   uint64_t data_offset;
   uint64_t data_size;
   bool guarded;
};

static inline struct chrome9_resource *
chrome9_resource(struct pipe_resource *resource)
{
   return (struct chrome9_resource *)resource;
}

static inline const struct chrome9_resource *
chrome9_resource_const(const struct pipe_resource *resource)
{
   return (const struct chrome9_resource *)resource;
}

bool chrome9_is_format_supported(struct pipe_screen *screen,
                                 enum pipe_format format,
                                 enum pipe_texture_target target,
                                 unsigned sample_count,
                                 unsigned storage_sample_count,
                                 unsigned bindings);
bool chrome9_can_create_resource(struct pipe_screen *screen,
                                 const struct pipe_resource *templ);
struct pipe_resource *
chrome9_resource_create(struct pipe_screen *screen,
                        const struct pipe_resource *templ);
void chrome9_resource_destroy(struct pipe_screen *screen,
                              struct pipe_resource *resource);

void chrome9_init_screen_resource_functions(struct pipe_screen *screen);
void chrome9_init_context_resource_functions(struct pipe_context *pipe);

#endif
