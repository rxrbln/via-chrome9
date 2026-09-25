/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_TEX_LOWER_H
#define CHROME9_TEX_LOWER_H

#include <stdbool.h>
#include <stdint.h>

struct nir_shader;
struct pipe_sampler_state;
struct pipe_sampler_view;

/* Sampler-state independent texture lowering of hardware fragment shaders:
 * 1D as height-1 2D, and GL_CLAMP's saturated TXD back to TEX. */
bool chrome9_nir_needs_hw_texture_lowering(const struct nir_shader *nir);
bool chrome9_nir_lower_hw_textures(struct nir_shader *nir);

/* Samplers read by a depth-compare (shadow) texture instruction. */
uint32_t chrome9_nir_shadow_sampler_mask(const struct nir_shader *nir);

/* Variant key of one shadow sampler: compare function and view swizzle. */
uint16_t chrome9_shadow_key(const struct pipe_sampler_state *sampler,
                            const struct pipe_sampler_view *view);

/* Replace every shadow sample by a raw depth sample and its comparison. */
bool chrome9_nir_lower_shadow(struct nir_shader *nir, unsigned count,
                              const uint16_t *keys);

#endif
