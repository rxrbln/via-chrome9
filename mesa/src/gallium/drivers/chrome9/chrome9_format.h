/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_FORMAT_H
#define CHROME9_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_formats.h"

struct chrome9_texture_format_info {
   enum pipe_format pipe_format;
   uint32_t drm_format;
   uint8_t bytes_per_pixel;
};

const struct chrome9_texture_format_info *
chrome9_texture_format_info(enum pipe_format format);

bool chrome9_texture_swizzle_supported(enum pipe_format format,
                                       unsigned r, unsigned g,
                                       unsigned b, unsigned a);

#endif
