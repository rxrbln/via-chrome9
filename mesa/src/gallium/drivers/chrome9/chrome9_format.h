/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_FORMAT_H
#define CHROME9_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_formats.h"

struct chrome9_texture_format_info {
   enum pipe_format pipe_format;
   uint32_t drm_format;
   /* Bytes per texel, or per 4x4 block of a DXT format. */
   uint8_t bytes_per_pixel;
   /* Decoded RGBA lanes: CHROME9_LANE_ZERO/ONE or CHROME9_LANE_DATA + n. */
   uint8_t lanes[4];
};

#define CHROME9_LANE_ZERO 0
#define CHROME9_LANE_ONE 1
#define CHROME9_LANE_DATA 2

const struct chrome9_texture_format_info *
chrome9_texture_format_info(enum pipe_format format);

/* Both share native D24S8 storage: depth in bits 31:8, stencil in 7:0. */
static inline bool
chrome9_format_is_d24(enum pipe_format format)
{
   return format == PIPE_FORMAT_X8Z24_UNORM ||
          format == PIPE_FORMAT_S8_UINT_Z24_UNORM;
}

/* Depth formats a depth-only pass draws on the hardware. */
static inline bool
chrome9_depth_only_format(enum pipe_format format)
{
   return chrome9_format_is_d24(format) || format == PIPE_FORMAT_Z16_UNORM;
}

/* Storage support is independent of sampling and render capability. */
unsigned chrome9_format_storage_cpp(enum pipe_format format);

/* S3TC DXT1/DXT3/DXT5 (4x4 blocks, bytes_per_pixel is the block size). */
bool chrome9_format_is_dxt(enum pipe_format format);
/* Texel edge of one storage block: 4 for DXT, 1 otherwise. */
unsigned chrome9_format_block_dim(enum pipe_format format);
/* Byte pitch of one row of blocks; zero for unknown formats. */
unsigned chrome9_format_level_pitch(enum pipe_format format, unsigned width);
/* Rows of blocks in a level of the given texel height. */
unsigned chrome9_format_level_rows(enum pipe_format format, unsigned height);
bool chrome9_pack_depth_clear(enum pipe_format format, double depth,
                              uint32_t *word);
bool chrome9_pack_rgb565_clear(const float rgba[4], uint16_t *word);
void chrome9_fill_rgb565_rect(uint8_t *pixels, unsigned stride,
                             unsigned x, unsigned y, unsigned width,
                             unsigned height, uint16_t word);

bool chrome9_texture_swizzle_supported(enum pipe_format format,
                                       unsigned r, unsigned g,
                                       unsigned b, unsigned a);
bool chrome9_depth_view_swizzle(unsigned pipe_swizzle, uint8_t *native);

#endif
