/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_format.h"

#include <math.h>

#include "drm-uapi/chrome9_drm.h"
#include "pipe/p_state.h"
#include "util/macros.h"

#define Z CHROME9_LANE_ZERO
#define O CHROME9_LANE_ONE
#define D(n) (CHROME9_LANE_DATA + (n))
static const struct chrome9_texture_format_info texture_formats[] = {
   { PIPE_FORMAT_B8G8R8X8_UNORM,
     DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888, 4, { D(0), D(1), D(2), O } },
   { PIPE_FORMAT_B8G8R8A8_UNORM,
     DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888, 4, { D(0), D(1), D(2), D(3) } },
   { PIPE_FORMAT_A8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_A8, 1,
     { Z, Z, Z, D(0) } },
   { PIPE_FORMAT_L8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_L8, 1,
     { D(0), D(0), D(0), O } },
   { PIPE_FORMAT_L8A8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_L8A8, 2,
     { D(0), D(0), D(0), D(1) } },
   { PIPE_FORMAT_B5G6R5_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_RGB565, 2,
     { D(0), D(1), D(2), O } },
   { PIPE_FORMAT_X8Z24_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24, 4,
     { D(0), Z, Z, O } },
   /* Depth in [31:8] like X8Z24; the sampler ignores the stencil byte. */
   { PIPE_FORMAT_S8_UINT_Z24_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24, 4,
     { D(0), Z, Z, O } },
   /* The DXT1 decoder always applies punch-through alpha; the fragment
    * path forces A = 1 for the RGB view. */
   { PIPE_FORMAT_DXT1_RGB, DRM_CHROME9_FRAGMENT_TEXTURE_DXT1_RGB, 8,
     { D(0), D(1), D(2), O } },
   { PIPE_FORMAT_DXT1_RGBA, DRM_CHROME9_FRAGMENT_TEXTURE_DXT1_RGBA, 8,
     { D(0), D(1), D(2), D(3) } },
   { PIPE_FORMAT_DXT3_RGBA, DRM_CHROME9_FRAGMENT_TEXTURE_DXT3, 16,
     { D(0), D(1), D(2), D(3) } },
   { PIPE_FORMAT_DXT5_RGBA, DRM_CHROME9_FRAGMENT_TEXTURE_DXT5, 16,
     { D(0), D(1), D(2), D(3) } },
};
#undef Z
#undef O
#undef D

const struct chrome9_texture_format_info *
chrome9_texture_format_info(enum pipe_format format)
{
   for (unsigned i = 0; i < ARRAY_SIZE(texture_formats); i++)
      if (texture_formats[i].pipe_format == format)
         return &texture_formats[i];
   return NULL;
}

unsigned
chrome9_format_storage_cpp(enum pipe_format format)
{
   const struct chrome9_texture_format_info *info =
      chrome9_texture_format_info(format);

   if (info)
      return info->bytes_per_pixel;
   if (format == PIPE_FORMAT_Z16_UNORM)
      return 2;
   if (chrome9_format_is_d24(format))
      return 4;
   return 0;
}

bool
chrome9_format_is_dxt(enum pipe_format format)
{
   return format == PIPE_FORMAT_DXT1_RGB || format == PIPE_FORMAT_DXT1_RGBA ||
          format == PIPE_FORMAT_DXT3_RGBA || format == PIPE_FORMAT_DXT5_RGBA;
}

unsigned
chrome9_format_block_dim(enum pipe_format format)
{
   return chrome9_format_is_dxt(format) ? 4 : 1;
}

unsigned
chrome9_format_level_pitch(enum pipe_format format, unsigned width)
{
   const unsigned block = chrome9_format_block_dim(format);

   return ALIGN_POT((width + block - 1) / block *
                    chrome9_format_storage_cpp(format), 32);
}

unsigned
chrome9_format_level_rows(enum pipe_format format, unsigned height)
{
   const unsigned block = chrome9_format_block_dim(format);

   return (height + block - 1) / block;
}

bool
chrome9_pack_depth_clear(enum pipe_format format, double depth,
                         uint32_t *word)
{
   if (!word || (format != PIPE_FORMAT_Z16_UNORM &&
                 !chrome9_format_is_d24(format)))
      return false;
   if (!isfinite(depth))
      depth = 0.0;
   depth = depth < 0.0 ? 0.0 : depth > 1.0 ? 1.0 : depth;
   *word = format == PIPE_FORMAT_Z16_UNORM ?
      (uint32_t)(depth * 65535.0 + 0.5) :
      (uint32_t)(depth * 16777215.0 + 0.5) << 8;
   return true;
}

static unsigned
chrome9_pack_unorm(float value, unsigned maximum)
{
   if (!isfinite(value) || value <= 0.0f)
      return 0;
   if (value >= 1.0f)
      return maximum;
   return (unsigned)(value * maximum + 0.5f);
}

bool
chrome9_pack_rgb565_clear(const float rgba[4], uint16_t *word)
{
   if (!rgba || !word)
      return false;

   /* PIPE_FORMAT_B5G6R5_UNORM has blue in bits 4:0 and red in 15:11. */
   *word = (chrome9_pack_unorm(rgba[0], 31) << 11) |
           (chrome9_pack_unorm(rgba[1], 63) << 5) |
            chrome9_pack_unorm(rgba[2], 31);
   return true;
}

void
chrome9_fill_rgb565_rect(uint8_t *pixels, unsigned stride,
                         unsigned x, unsigned y, unsigned width,
                         unsigned height, uint16_t word)
{
   for (unsigned row = 0; row < height; row++) {
      uint16_t *dst = (uint16_t *)(pixels + (uint64_t)(y + row) * stride) + x;

      for (unsigned column = 0; column < width; column++)
         dst[column] = word;
   }
}

bool
chrome9_texture_swizzle_supported(enum pipe_format format,
                                  unsigned r, unsigned g,
                                  unsigned b, unsigned a)
{
   if (!chrome9_texture_format_info(format))
      return false;
   /* A raw D24 sample returns depth in every native lane.  The generic
    * fragment path composes the Gallium view swizzle (including constants)
    * into later source selectors before submitting the program. */
   if (chrome9_format_is_d24(format))
      return r <= PIPE_SWIZZLE_1 && g <= PIPE_SWIZZLE_1 &&
             b <= PIPE_SWIZZLE_1 && a <= PIPE_SWIZZLE_1;

   /* The native decoders implement Gallium's missing-channel semantics, so
    * any view that selects the same decoded value per lane is exact. */
   const struct chrome9_texture_format_info *info =
      chrome9_texture_format_info(format);
   const unsigned view[4] = { r, g, b, a };

   for (unsigned lane = 0; lane < 4; lane++) {
      unsigned selected;

      if (view[lane] <= PIPE_SWIZZLE_W)
         selected = info->lanes[view[lane]];
      else if (view[lane] == PIPE_SWIZZLE_0)
         selected = CHROME9_LANE_ZERO;
      else if (view[lane] == PIPE_SWIZZLE_1)
         selected = CHROME9_LANE_ONE;
      else
         return false;
      if (selected != info->lanes[lane])
         return false;
   }
   return true;
}

bool
chrome9_depth_view_swizzle(unsigned pipe_swizzle, uint8_t *native)
{
   if (!native)
      return false;
   /* The Gallium X8Z24 base format is (Z,0,0,1), while native type-FE
    * routes Z to every lane.  Resolve the base before applying view swizzle. */
   switch (pipe_swizzle) {
   case PIPE_SWIZZLE_X:
      *native = DRM_CHROME9_FRAGMENT_SWIZZLE_X;
      return true;
   case PIPE_SWIZZLE_Y:
   case PIPE_SWIZZLE_Z:
   case PIPE_SWIZZLE_0:
      *native = DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO;
      return true;
   case PIPE_SWIZZLE_W:
   case PIPE_SWIZZLE_1:
      *native = DRM_CHROME9_FRAGMENT_SWIZZLE_ONE;
      return true;
   default:
      return false;
   }
}
