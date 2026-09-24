/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_context.h"
#include "chrome9_format.h"
#include "chrome9_resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/u_math.h"

#define TEST_WIDTH 9
#define TEST_HEIGHT 6
#define TEST_LAST_LEVEL 3
#define TEST_SENTINEL 0x5a

struct test_map_record {
   unsigned level;
   unsigned usage;
   struct pipe_box box;
};

struct test_context {
   struct chrome9_context chrome9;
   void *(*original_map)(struct pipe_context *, struct pipe_resource *,
                         unsigned, unsigned, const struct pipe_box *,
                         struct pipe_transfer **);
   void (*original_unmap)(struct pipe_context *, struct pipe_transfer *);
   struct test_map_record maps[TEST_LAST_LEVEL * 2];
   unsigned map_count;
   unsigned unmap_count;
};

static void *
test_texture_map(struct pipe_context *pipe, struct pipe_resource *resource,
                 unsigned level, unsigned usage, const struct pipe_box *box,
                 struct pipe_transfer **transfer)
{
   struct test_context *test = (struct test_context *)pipe;

   if (test->map_count < ARRAY_SIZE(test->maps)) {
      struct test_map_record *record = &test->maps[test->map_count];

      record->level = level;
      record->usage = usage;
      record->box = *box;
   }
   test->map_count++;
   return test->original_map(pipe, resource, level, usage, box, transfer);
}

static void
test_texture_unmap(struct pipe_context *pipe, struct pipe_transfer *transfer)
{
   struct test_context *test = (struct test_context *)pipe;

   test->unmap_count++;
   test->original_unmap(pipe, transfer);
}

static bool
test_layout(struct chrome9_resource *resource, enum pipe_format format)
{
   const struct chrome9_texture_format_info *info =
      chrome9_texture_format_info(format);
   const unsigned cpp = info ? info->bytes_per_pixel : 2;
   const long page_size = sysconf(_SC_PAGESIZE);
   uint64_t cursor;

   if (page_size <= 0 || !util_is_power_of_two_or_zero(page_size))
      return false;
   resource->level_offset[0] = page_size;
   for (unsigned level = 0; level <= TEST_LAST_LEVEL; level++) {
      const unsigned width = u_minify(TEST_WIDTH, level);
      const unsigned height = u_minify(TEST_HEIGHT, level);

      resource->level_stride[level] = ALIGN_POT(width * cpp, 32);
      resource->level_size[level] =
         (uint64_t)resource->level_stride[level] * height;
   }
   cursor = resource->level_offset[0] +
            ALIGN_POT(resource->level_size[0], page_size) + page_size;
   for (unsigned level = 1; level <= TEST_LAST_LEVEL; level++) {
      resource->level_offset[level] = ALIGN_POT(cursor, 256);
      cursor = resource->level_offset[level] + resource->level_size[level];
   }
   resource->bo.size = ALIGN_POT(cursor, page_size) + page_size;
   resource->stride = resource->level_stride[0];
   resource->data_offset = resource->level_offset[0];
   resource->data_size = resource->level_size[0];
   resource->guarded = true;
   return true;
}

static void
test_fill_level0(struct chrome9_resource *resource, uint8_t *allocation,
                 enum pipe_format format)
{
   uint8_t *level = allocation + resource->level_offset[0];

   for (unsigned y = 0; y < TEST_HEIGHT; y++) {
      if (format == PIPE_FORMAT_Z16_UNORM) {
         uint16_t *row =
            (uint16_t *)(level + (uint64_t)y * resource->level_stride[0]);

         for (unsigned x = 0; x < TEST_WIDTH; x++)
            row[x] = 1000 + y * 1171 + x * 313;
      } else {
         const struct chrome9_texture_format_info *info =
            chrome9_texture_format_info(format);
         uint8_t *row = level + (uint64_t)y * resource->level_stride[0];

         for (unsigned x = 0; x < TEST_WIDTH; x++) {
            if (info->bytes_per_pixel == 4) {
               row[x * 4 + 0] = 3 + x * 17 + y * 5;
               row[x * 4 + 1] = 7 + x * 3 + y * 29;
               row[x * 4 + 2] = 11 + x * 11 + y * 13;
               row[x * 4 + 3] = 19 + x * 7 + y * 23;
            } else {
               const float value[4] = {
                  (3 + x * 17 + y * 5) / 255.0f,
                  (3 + x * 17 + y * 5) / 255.0f,
                  (3 + x * 17 + y * 5) / 255.0f,
                  (19 + x * 7 + y * 23) / 255.0f,
               };

               util_format_pack_rgba(format, row + x * info->bytes_per_pixel,
                                     value, 1);
            }
         }
      }
   }
}

static void
test_generate_expected(struct chrome9_resource *resource,
                       uint8_t *allocation, enum pipe_format format)
{
   for (unsigned level = 1; level <= TEST_LAST_LEVEL; level++) {
      const unsigned source_width = u_minify(TEST_WIDTH, level - 1);
      const unsigned source_height = u_minify(TEST_HEIGHT, level - 1);
      const unsigned destination_width = u_minify(TEST_WIDTH, level);
      const unsigned destination_height = u_minify(TEST_HEIGHT, level);
      const uint8_t *source = allocation + resource->level_offset[level - 1];
      uint8_t *destination = allocation + resource->level_offset[level];

      for (unsigned y = 0; y < destination_height; y++) {
         const unsigned sy[2] = {
            source_height == 1 ? 0 : y * 2,
            source_height == 1 ? 0 : y * 2 + 1,
         };

         for (unsigned x = 0; x < destination_width; x++) {
            const unsigned sx[2] = {
               source_width == 1 ? 0 : x * 2,
               source_width == 1 ? 0 : x * 2 + 1,
            };

            if (format == PIPE_FORMAT_Z16_UNORM) {
               uint16_t values[4];
               uint16_t *dst =
                  (uint16_t *)(destination +
                               (uint64_t)y * resource->level_stride[level]);

               for (unsigned row = 0; row < 2; row++) {
                  const uint16_t *src =
                     (const uint16_t *)(source +
                                       (uint64_t)sy[row] *
                                          resource->level_stride[level - 1]);
                  values[row * 2] = src[sx[0]];
                  values[row * 2 + 1] = src[sx[1]];
               }
               dst[x] = ((uint32_t)values[0] + values[1] + values[2] +
                         values[3]) / 4;
            } else {
               const struct chrome9_texture_format_info *info =
                  chrome9_texture_format_info(format);
               uint8_t *dst = destination + (uint64_t)y *
                              resource->level_stride[level] +
                              x * info->bytes_per_pixel;

               if (info->bytes_per_pixel == 4) {
                  for (unsigned channel = 0; channel < 4; channel++) {
                     unsigned sum = 0;

                     for (unsigned row = 0; row < 2; row++) {
                        const uint8_t *src =
                           source + (uint64_t)sy[row] *
                                       resource->level_stride[level - 1];

                        sum += src[sx[0] * 4 + channel];
                        sum += src[sx[1] * 4 + channel];
                     }
                     dst[channel] =
                        channel == 3 &&
                        format == PIPE_FORMAT_B8G8R8X8_UNORM ? 0xff :
                                                               sum / 4;
                  }
               } else {
                  float texels[4][4];
                  float result[4];

                  for (unsigned row = 0; row < 2; row++) {
                     const uint8_t *src = source + (uint64_t)sy[row] *
                        resource->level_stride[level - 1];

                     util_format_unpack_rgba(
                        format, texels[row * 2],
                        src + sx[0] * info->bytes_per_pixel, 1);
                     util_format_unpack_rgba(
                        format, texels[row * 2 + 1],
                        src + sx[1] * info->bytes_per_pixel, 1);
                  }
                  for (unsigned channel = 0; channel < 4; channel++)
                     result[channel] =
                        (texels[0][channel] + texels[1][channel] +
                         texels[2][channel] + texels[3][channel]) * 0.25f;
                  util_format_pack_rgba(format, dst, result, 1);
               }
            }
         }
      }
   }
}

static bool
test_map_contract(const struct test_context *test)
{
   if (test->map_count != TEST_LAST_LEVEL * 2 ||
       test->unmap_count != TEST_LAST_LEVEL * 2)
      return false;
   for (unsigned level = 1; level <= TEST_LAST_LEVEL; level++) {
      const struct test_map_record *source = &test->maps[(level - 1) * 2];
      const struct test_map_record *destination = source + 1;

      if (source->level != level - 1 || source->usage != PIPE_MAP_READ ||
          source->box.x || source->box.y || source->box.z ||
          source->box.width != (int)u_minify(TEST_WIDTH, level - 1) ||
          source->box.height != (int)u_minify(TEST_HEIGHT, level - 1) ||
          source->box.depth != 1 || destination->level != level ||
          destination->usage != PIPE_MAP_WRITE || destination->box.x ||
          destination->box.y || destination->box.z ||
          destination->box.width != (int)u_minify(TEST_WIDTH, level) ||
          destination->box.height != (int)u_minify(TEST_HEIGHT, level) ||
          destination->box.depth != 1)
         return false;
   }
   return true;
}

static bool
test_format(enum pipe_format format)
{
   struct pipe_screen screen = { 0 };
   struct test_context test = { 0 };
   struct chrome9_resource resource = {
      .base = {
         .target = PIPE_TEXTURE_2D,
         .width0 = TEST_WIDTH,
         .height0 = TEST_HEIGHT,
         .depth0 = 1,
         .array_size = 1,
         .last_level = TEST_LAST_LEVEL,
      },
   };
   uint8_t *expected = NULL;
   bool passed = false;

   resource.base.format = format;
   resource.base.screen = &screen;
   pipe_reference_init(&resource.base.reference, 1);
   if (!test_layout(&resource, format))
      goto out;
   resource.bo.map = malloc(resource.bo.size);
   expected = malloc(resource.bo.size);
   if (!resource.bo.map || !expected)
      goto out;
   memset(resource.bo.map, TEST_SENTINEL, resource.bo.size);
   memset(expected, TEST_SENTINEL, resource.bo.size);
   test_fill_level0(&resource, resource.bo.map, format);
   test_fill_level0(&resource, expected, format);
   test_generate_expected(&resource, expected, format);

   chrome9_init_context_resource_functions(&test.chrome9.base);
   test.original_map = test.chrome9.base.texture_map;
   test.original_unmap = test.chrome9.base.texture_unmap;
   test.chrome9.base.texture_map = test_texture_map;
   test.chrome9.base.texture_unmap = test_texture_unmap;
   if (!test.chrome9.base.generate_mipmap(
          &test.chrome9.base, &resource.base, format, 0,
          TEST_LAST_LEVEL, 0, 0)) {
      fprintf(stderr, "generate_mipmap rejected format %u\n", format);
      goto out;
   }
   if (!test_map_contract(&test)) {
      fprintf(stderr, "transfer contract failed for format %u\n", format);
      goto out;
   }
   if (memcmp(resource.bo.map, expected, resource.bo.size)) {
      const uint8_t *actual = resource.bo.map;

      for (uint64_t offset = 0; offset < resource.bo.size; offset++)
         if (actual[offset] != expected[offset]) {
            fprintf(stderr,
                    "format %u mismatch at offset %llu: got=%02x expected=%02x\n",
                    format, (unsigned long long)offset, actual[offset],
                    expected[offset]);
            break;
         }
      goto out;
   }
   if (p_atomic_read(&resource.base.reference.count) != 1) {
      fprintf(stderr, "resource reference leak for format %u\n", format);
      goto out;
   }
   passed = true;

out:
   free(resource.bo.map);
   free(expected);
   return passed;
}

int
main(void)
{
   if (!test_format(PIPE_FORMAT_B8G8R8X8_UNORM) ||
       !test_format(PIPE_FORMAT_B8G8R8A8_UNORM) ||
       !test_format(PIPE_FORMAT_A8_UNORM) ||
       !test_format(PIPE_FORMAT_L8_UNORM) ||
       !test_format(PIPE_FORMAT_L8A8_UNORM) ||
       !test_format(PIPE_FORMAT_B5G6R5_UNORM) ||
       !test_format(PIPE_FORMAT_Z16_UNORM))
      return 1;
   puts("Chrome9 CPU mipmap generation and guarded layout passed");
   return 0;
}
