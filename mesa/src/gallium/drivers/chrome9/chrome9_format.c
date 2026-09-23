/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_format.h"

#include <math.h>

#include "drm-uapi/chrome9_drm.h"
#include "pipe/p_state.h"
#include "util/macros.h"

static const struct chrome9_texture_format_info texture_formats[] = {
   { PIPE_FORMAT_B8G8R8X8_UNORM,
     DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888, 4 },
   { PIPE_FORMAT_B8G8R8A8_UNORM,
     DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888, 4 },
   { PIPE_FORMAT_A8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_A8, 1 },
   { PIPE_FORMAT_L8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_L8, 1 },
   { PIPE_FORMAT_L8A8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_L8A8, 2 },
   { PIPE_FORMAT_B5G6R5_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_RGB565, 2 },
   { PIPE_FORMAT_X8Z24_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24, 4 },
};

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
   if (format == PIPE_FORMAT_X8Z24_UNORM)
      return 4;
   return 0;
}

bool
chrome9_pack_depth_clear(enum pipe_format format, double depth,
                         uint32_t *word)
{
   if (!word || (format != PIPE_FORMAT_Z16_UNORM &&
                 format != PIPE_FORMAT_X8Z24_UNORM))
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
   if (format == PIPE_FORMAT_X8Z24_UNORM)
      return r <= PIPE_SWIZZLE_1 && g <= PIPE_SWIZZLE_1 &&
             b <= PIPE_SWIZZLE_1 && a <= PIPE_SWIZZLE_1;
   if (r != PIPE_SWIZZLE_X ||
       g != PIPE_SWIZZLE_Y || b != PIPE_SWIZZLE_Z)
      return false;

   /* The native decoders already implement Gallium's A8, luminance,
    * luminance-alpha, and RGB565 missing-channel semantics.  Their canonical
    * view is therefore identity.  XRGB may also spell its decoder-supplied
    * alpha-one explicitly.
    */
   return a == PIPE_SWIZZLE_W ||
          (format == PIPE_FORMAT_B8G8R8X8_UNORM && a == PIPE_SWIZZLE_1);
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
