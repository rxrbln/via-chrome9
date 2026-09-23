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

bool
chrome9_texture_swizzle_supported(enum pipe_format format,
                                  unsigned r, unsigned g,
                                  unsigned b, unsigned a)
{
   if (!chrome9_texture_format_info(format) || r != PIPE_SWIZZLE_X ||
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
