/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_FRAGMENT_H
#define CHROME9_FRAGMENT_H

#include "chrome9_shader_offline.h"
#include "drm-uapi/chrome9_drm.h"

unsigned chrome9_fragment_sampler_high_water(uint8_t sampler_mask);
bool chrome9_fragment_sampler_slots_dense(uint8_t sampler_mask);

bool chrome9_fragment_build_instructions(
   const struct chrome9_ps_ir *ir, uint8_t sampler_mask,
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS],
   unsigned *count);

#endif
