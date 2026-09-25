/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_RESOURCE_H
#define CHROME9_RESOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "chrome9_drm.h"
#include "pipe/p_state.h"

struct pipe_context;
struct pipe_screen;

/* Single-image targets; 1D is a height-1 2D image. */
static inline bool
chrome9_is_surface_target(enum pipe_texture_target target)
{
   return target == PIPE_TEXTURE_2D || target == PIPE_TEXTURE_RECT ||
          target == PIPE_TEXTURE_1D;
}

/* Created through a LINEAR modifier: shared yet guarded (BO + PAGE). */
#define CHROME9_RESOURCE_FLAG_CANONICAL_SHARED PIPE_RESOURCE_FLAG_DRV_PRIV

struct chrome9_resource {
   struct pipe_resource base;
   struct chrome9_bo bo;
   uint32_t level_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_offset[PIPE_MAX_TEXTURE_LEVELS];
   uint64_t level_size[PIPE_MAX_TEXTURE_LEVELS];
   /* Cube: bytes between equal levels of adjacent faces, else zero. */
   uint64_t layer_stride;
   /* Level-zero aliases retained for the existing draw/ioctl contract. */
   uint32_t stride;
   uint64_t data_offset;
   uint64_t data_size;
   bool guarded;
   /* A layout the sampler cannot read (NPOT DXT mip chains, DXT cubes);
    * only llvmpipe samples it. */
   bool sampler_unsupported;
   /* Guarded, yet external layout offset 0 names the first row. */
   bool guarded_shared;
   /* The kernel defers cache maintenance; CPU accesses are bracketed with
    * chrome9_resource_cpu_access(). */
   bool explicit_access;
   /* Counts announced CPU writes, so a render shadow can tell whether the
    * surface changed since it was last synchronized. */
   uint32_t cpu_write_generation;
   /* Unique per created resource, unlike a reusable address. */
   uint64_t serial;
   /* CAP2_COARSE_Z buffer of a depth surface, one 16-byte entry per 8x8
    * block after a guard page.  Valid from a full clear to 1.0 until a
    * depth write that may move a depth farther or a CPU write; the tag
    * cache is reset on its next use after a fill. */
   struct chrome9_bo coarse_z;
   uint32_t coarse_z_pitch;
   bool coarse_z_valid;
   bool coarse_z_reset;
   /* Single-image render view of a level or layer of parent: same BO,
    * pixels at data_offset. */
   struct pipe_resource *parent;
};

/* Drawn with DESTINATION_EXTENDED: other images share the object. */
static inline bool
chrome9_resource_extended(const struct chrome9_resource *resource)
{
   return resource->base.last_level || resource->parent;
}

/* destination_offset of a fragment draw into resource. */
static inline uint32_t
chrome9_resource_destination_offset(const struct chrome9_resource *resource)
{
   return resource->parent ? (uint32_t)resource->data_offset : 0;
}

void chrome9_resource_cpu_access(struct chrome9_resource *resource,
                                 bool write);
bool chrome9_resource_coarse_z_fill(struct pipe_context *pipe,
                                    struct chrome9_resource *depth);
void chrome9_resource_cpu_access_range(struct chrome9_resource *resource,
                                       bool write, uint64_t offset,
                                       uint64_t size);

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
struct pipe_resource *
chrome9_resource_image_view(struct pipe_resource *resource, unsigned level,
                            unsigned layer);
bool chrome9_generate_mipmap(struct pipe_context *pipe,
                             struct pipe_resource *resource,
                             enum pipe_format format,
                             unsigned base_level, unsigned last_level,
                             unsigned first_layer, unsigned last_layer);

void chrome9_init_screen_resource_functions(struct pipe_screen *screen);
void chrome9_init_context_resource_functions(struct pipe_context *pipe);

#endif
