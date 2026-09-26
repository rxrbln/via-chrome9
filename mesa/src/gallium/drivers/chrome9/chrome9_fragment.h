/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_FRAGMENT_H
#define CHROME9_FRAGMENT_H

#include "chrome9_shader_offline.h"
#include "drm-uapi/chrome9_drm.h"

unsigned chrome9_fragment_sampler_high_water(uint8_t sampler_mask);
bool chrome9_fragment_sampler_slots_dense(uint8_t sampler_mask);
unsigned chrome9_fragment_descriptor_unit(uint8_t sampler_mask,
                                          unsigned descriptor);

/* VX900 can read at most two distinct constant registers in one ALU record.
 * Materialize a third source before translating semantic IR to the UAPI. */
bool chrome9_fragment_legalize_constant_ports(struct chrome9_ps_ir *ir);

bool chrome9_fragment_build_instructions(
   const struct chrome9_ps_ir *ir, uint8_t sampler_mask,
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS],
   unsigned *count);

/* Compose fixed per-sampler result selectors into virtual-temp sources:
 * the decoded X8Z24 view (type-FE returns depth in every native lane) and
 * the opaque alpha of a DXT1 RGB view. */
bool chrome9_fragment_apply_depth_swizzles(
   struct drm_chrome9_fragment_instruction *instructions, unsigned count,
   uint8_t depth_sampler_mask,
   const uint8_t swizzle[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4]);

/* Remove TEX instructions whose temporary nothing reads (a depth view can
 * turn every read into a constant) and return the samplers still read. */
uint32_t chrome9_fragment_drop_dead_textures(
   struct drm_chrome9_fragment_instruction *instructions, unsigned *count);

/* Keep the texture descriptors of the samplers in used: dense descriptors
 * are indexed by sampler, sparse ones name it in reserved0. */
unsigned chrome9_fragment_trim_textures(
   struct drm_chrome9_fragment_texture_state *textures, unsigned count,
   uint32_t used, bool sparse);

#endif
