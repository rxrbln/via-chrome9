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

/* Compose the decoded X8Z24 view selectors into virtual-temp sources.
 * Hardware type-FE returns depth in every native lane. */
bool chrome9_fragment_apply_depth_swizzles(
   struct drm_chrome9_fragment_instruction *instructions, unsigned count,
   uint8_t depth_sampler_mask,
   const uint8_t swizzle[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4]);

#endif
