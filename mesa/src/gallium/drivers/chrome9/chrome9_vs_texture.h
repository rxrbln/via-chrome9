/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_VS_TEXTURE_H
#define CHROME9_VS_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "pipe/p_state.h"

struct chrome9_context;
struct tgsi_sampler;

#define CHROME9_VS_MAX_SAMPLERS 8

/* The draw module's LLVM vertex sampler reads the resource's linear BO. */
struct chrome9_vs_texture_mapping {
   const void *base;
   uint32_t width, height;
   uint32_t first_level, last_level;
   uint32_t row_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint32_t img_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint32_t mip_offset[PIPE_MAX_TEXTURE_LEVELS];
};

bool chrome9_vs_texture_mapping(const struct pipe_sampler_view *view,
                                struct chrome9_vs_texture_mapping *mapping);
struct tgsi_sampler *chrome9_vs_texture_sampler_create(struct chrome9_context *ctx);
void chrome9_vs_texture_sampler_destroy(struct tgsi_sampler *sampler);

#endif
