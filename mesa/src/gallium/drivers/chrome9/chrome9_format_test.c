/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_format.h"

#include <math.h>

#include "drm-uapi/chrome9_drm.h"
#include "pipe/p_state.h"

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

int
main(void)
{
   uint32_t word;
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
      { PIPE_FORMAT_X8Z24_UNORM, DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24, 4 },
   };

   for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
      const struct chrome9_texture_format_info *info =
         chrome9_texture_format_info(expected[i].pipe);

      CHECK(info);
      CHECK(info->drm_format == expected[i].drm);
      CHECK(info->bytes_per_pixel == expected[i].cpp);
      CHECK(chrome9_format_storage_cpp(expected[i].pipe) == expected[i].cpp);
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
   CHECK(chrome9_format_storage_cpp(PIPE_FORMAT_Z16_UNORM) == 2);
   CHECK(chrome9_format_storage_cpp(PIPE_FORMAT_X8Z24_UNORM) == 4);
   CHECK(!chrome9_format_storage_cpp(PIPE_FORMAT_Z24X8_UNORM));
   CHECK(chrome9_texture_swizzle_supported(
      PIPE_FORMAT_X8Z24_UNORM, PIPE_SWIZZLE_X, PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_X, PIPE_SWIZZLE_1));
   CHECK(chrome9_texture_swizzle_supported(
      PIPE_FORMAT_X8Z24_UNORM, PIPE_SWIZZLE_X, PIPE_SWIZZLE_0,
      PIPE_SWIZZLE_0, PIPE_SWIZZLE_1));
   CHECK(!chrome9_texture_swizzle_supported(
      PIPE_FORMAT_X8Z24_UNORM, PIPE_SWIZZLE_X, 255,
      PIPE_SWIZZLE_0, PIPE_SWIZZLE_1));
   {
      uint8_t lane;

      CHECK(chrome9_depth_view_swizzle(PIPE_SWIZZLE_X, &lane));
      CHECK(lane == DRM_CHROME9_FRAGMENT_SWIZZLE_X);
      CHECK(chrome9_depth_view_swizzle(PIPE_SWIZZLE_Y, &lane));
      CHECK(lane == DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO);
      CHECK(chrome9_depth_view_swizzle(PIPE_SWIZZLE_Z, &lane));
      CHECK(lane == DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO);
      CHECK(chrome9_depth_view_swizzle(PIPE_SWIZZLE_W, &lane));
      CHECK(lane == DRM_CHROME9_FRAGMENT_SWIZZLE_ONE);
      CHECK(chrome9_depth_view_swizzle(PIPE_SWIZZLE_0, &lane));
      CHECK(lane == DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO);
      CHECK(chrome9_depth_view_swizzle(PIPE_SWIZZLE_1, &lane));
      CHECK(lane == DRM_CHROME9_FRAGMENT_SWIZZLE_ONE);
      CHECK(!chrome9_depth_view_swizzle(255, &lane));
      CHECK(!chrome9_depth_view_swizzle(PIPE_SWIZZLE_X, NULL));
   }
   CHECK(chrome9_pack_depth_clear(PIPE_FORMAT_X8Z24_UNORM, 0.0, &word));
   CHECK(word == 0x00000000);
   CHECK(chrome9_pack_depth_clear(PIPE_FORMAT_X8Z24_UNORM, 0.5, &word));
   CHECK(word == 0x80000000);
   CHECK(chrome9_pack_depth_clear(PIPE_FORMAT_X8Z24_UNORM, 1.0, &word));
   CHECK(word == 0xffffff00);
   CHECK(chrome9_pack_depth_clear(PIPE_FORMAT_X8Z24_UNORM, -1.0, &word));
   CHECK(word == 0);
   CHECK(chrome9_pack_depth_clear(PIPE_FORMAT_X8Z24_UNORM, INFINITY, &word));
   CHECK(word == 0);
   CHECK(chrome9_pack_depth_clear(PIPE_FORMAT_Z16_UNORM, 1.0, &word));
   CHECK(word == 0xffff);
   CHECK(!chrome9_pack_depth_clear(PIPE_FORMAT_Z24X8_UNORM, 1.0, &word));
   CHECK(!chrome9_pack_depth_clear(PIPE_FORMAT_X8Z24_UNORM, 1.0, NULL));
   {
      uint16_t rgb565;
      const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
      const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
      const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
      const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
      const float midpoint[4] = { 0.5f, 0.5f, 0.5f, 1.0f };

      CHECK(chrome9_pack_rgb565_clear(red, &rgb565));
      CHECK(rgb565 == 0xf800);
      CHECK(chrome9_pack_rgb565_clear(green, &rgb565));
      CHECK(rgb565 == 0x07e0);
      CHECK(chrome9_pack_rgb565_clear(blue, &rgb565));
      CHECK(rgb565 == 0x001f);
      CHECK(chrome9_pack_rgb565_clear(white, &rgb565));
      CHECK(rgb565 == 0xffff);
      CHECK(chrome9_pack_rgb565_clear(black, &rgb565));
      CHECK(rgb565 == 0x0000);
      CHECK(chrome9_pack_rgb565_clear(midpoint, &rgb565));
      CHECK(rgb565 == 0x8410);
      CHECK(!chrome9_pack_rgb565_clear(NULL, &rgb565));
      CHECK(!chrome9_pack_rgb565_clear(red, NULL));
      {
         uint16_t pixels[4][8];

         for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 8; x++)
               pixels[y][x] = 0xa55a;
         chrome9_fill_rgb565_rect((uint8_t *)pixels, sizeof(pixels[0]),
                                  2, 1, 3, 2, 0xf800);
         for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 8; x++)
               CHECK(pixels[y][x] ==
                     ((y == 1 || y == 2) && x >= 2 && x < 5 ?
                      0xf800 : 0xa55a));
      }
   }
   return 0;
}
