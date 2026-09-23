/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_format.h"

#include "drm-uapi/chrome9_drm.h"
#include "pipe/p_state.h"

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

int
main(void)
{
   static const struct {
      enum pipe_format pipe;
      uint32_t drm;
      uint8_t cpp;
   } expected[] = {
      { PIPE_FORMAT_B8G8R8X8_UNORM,
        DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888, 4 },
      { PIPE_FORMAT_B8G8R8A8_UNORM,
        DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888, 4 },
      { PIPE_FORMAT_A8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_A8, 1 },
      { PIPE_FORMAT_L8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_L8, 1 },
      { PIPE_FORMAT_L8A8_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_L8A8, 2 },
      { PIPE_FORMAT_B5G6R5_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_RGB565, 2 },
   };

   for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
      const struct chrome9_texture_format_info *info =
         chrome9_texture_format_info(expected[i].pipe);

      CHECK(info);
      CHECK(info->drm_format == expected[i].drm);
      CHECK(info->bytes_per_pixel == expected[i].cpp);
      CHECK(chrome9_texture_swizzle_supported(
         expected[i].pipe, PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
         PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W));
   }
   CHECK(chrome9_texture_swizzle_supported(
      PIPE_FORMAT_B8G8R8X8_UNORM, PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
      PIPE_SWIZZLE_Z, PIPE_SWIZZLE_1));
   CHECK(!chrome9_texture_swizzle_supported(
      PIPE_FORMAT_A8_UNORM, PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
      PIPE_SWIZZLE_Z, PIPE_SWIZZLE_1));
   CHECK(!chrome9_texture_swizzle_supported(
      PIPE_FORMAT_R8_UNORM, PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
      PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W));
   return 0;
}
