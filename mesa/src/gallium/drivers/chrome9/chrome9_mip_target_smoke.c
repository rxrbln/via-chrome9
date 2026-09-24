/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

/* Gallium L0 rendering into a guarded BGRA resource with a private mip tail. */
#include "chrome9_context.h"
#include "chrome9_public.h"
#include "chrome9_resource.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

#define WIDTH 64U
#define HEIGHT 32U
#define PITCH (WIDTH * sizeof(uint32_t))
#define PAGE_BYTES 0x1000U
#define REQUIRED_BYTES 0x4000U
#define ALLOCATION_BYTES 0x6000U
#define LEVEL1_OFFSET 0x4000U
#define LEVEL1_SIZE 0x0800U
#define FINAL_GUARD_OFFSET 0x5000U
#define GUARD_WORD 0x13579bdfU
#define MIP_POISON 0xa55aa55aU
#define COLOR_RED 0xffff0000U

struct smoke_vertex {
   float position[4];
};

static void *
make_vertex_shader(struct pipe_screen *screen, struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, screen->nir_options[MESA_SHADER_VERTEX],
      "chrome9-mip-target-vertex");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *position;

   b.shader->info.io_lowered = true;
   position = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0),
                             .io_semantics.location = VERT_ATTRIB_POS);
   nir_store_output(&b, position, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_vs_state(context, &state);
}

static void *
make_fragment_shader(struct pipe_screen *screen, struct pipe_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, screen->nir_options[MESA_SHADER_FRAGMENT],
      "chrome9-mip-target-fragment");
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };
   nir_def *color;

   b.shader->info.io_lowered = true;
   color = nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                        .align_mul = 16, .range = 16);
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return context->create_fs_state(context, &state);
}

static bool
fill_words(uint8_t *allocation, size_t offset, size_t size, uint32_t value)
{
   uint32_t *words;

   if ((offset | size) & (sizeof(uint32_t) - 1))
      return false;
   words = (uint32_t *)(allocation + offset);
   for (size_t i = 0; i < size / sizeof(*words); i++)
      words[i] = value;
   return true;
}

static bool
words_match(const uint8_t *allocation, size_t offset, size_t size,
            uint32_t expected, const char *name)
{
   const uint32_t *words;

   if ((offset | size) & (sizeof(uint32_t) - 1))
      return false;
   words = (const uint32_t *)(allocation + offset);
   for (size_t i = 0; i < size / sizeof(*words); i++) {
      if (words[i] != expected) {
         fprintf(stderr, "%s changed at byte %#zx: %08x != %08x\n",
                 name, offset + i * sizeof(*words), words[i], expected);
         return false;
      }
   }
   return true;
}

static bool
run_smoke(struct pipe_screen *screen)
{
   static const struct smoke_vertex vertices[4] = {
      { { -1.0f, -1.0f, 0.0f, 1.0f } },
      { { -1.0f,  1.0f, 0.0f, 1.0f } },
      { {  1.0f, -1.0f, 0.0f, 1.0f } },
      { {  1.0f,  1.0f, 0.0f, 1.0f } },
   };
   static const float color[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
   const struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = WIDTH,
      .height0 = HEIGHT,
      .depth0 = 1,
      .array_size = 1,
      .last_level = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
              PIPE_BIND_LINEAR,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = WIDTH,
      .height = HEIGHT,
      .nr_cbufs = 1,
      .cbufs[0] = {
         .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      },
   };
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = {
      .half_pixel_center = true,
      .bottom_edge_rule = true,
      .depth_clip_near = true,
      .depth_clip_far = true,
   };
   struct pipe_vertex_element element = {
      .src_offset = offsetof(struct smoke_vertex, position),
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      .src_stride = sizeof(struct smoke_vertex),
   };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_viewport_state viewport = {
      .scale = { WIDTH / 2.0f, HEIGHT / 2.0f, 0.5f },
      .translate = { WIDTH / 2.0f, HEIGHT / 2.0f, 0.5f },
   };
   struct pipe_scissor_state scissor = {
      .maxx = WIDTH,
      .maxy = HEIGHT,
   };
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_QUAD_STRIP,
      .index_bounds_valid = true,
      .instance_count = 1,
      .min_index = 0,
      .max_index = 3,
   };
   struct pipe_draw_start_count_bias draw = {
      .count = 4,
   };
   struct pipe_constant_buffer constant = {
      .buffer_size = sizeof(color),
      .user_buffer = color,
   };
   struct pipe_context *context = NULL;
   struct pipe_resource *resource = NULL;
   struct chrome9_resource *target;
   uint8_t *allocation;
   uint32_t *pixels;
   void *blend_state = NULL;
   void *dsa_state = NULL;
   void *rasterizer_state = NULL;
   void *element_state = NULL;
   void *vs = NULL;
   void *fs = NULL;
   uint64_t submitted;
   uint64_t rejected;
   uint64_t batches;
   bool passed = false;

   if (!chrome9_screen_has_capability(
          screen, DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM |
                  DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED)) {
      fputs("kernel lacks extended generic fragment destination\n", stderr);
      return false;
   }
   resource = screen->resource_create(screen, &templ);
   if (!resource)
      goto out;
   target = chrome9_resource(resource);
   if (!target->guarded || !target->bo.map || target->stride != PITCH ||
       target->data_offset != PAGE_BYTES || target->data_size != PITCH * HEIGHT ||
       target->level_offset[1] != LEVEL1_OFFSET ||
       target->level_size[1] != LEVEL1_SIZE ||
       target->bo.size != ALLOCATION_BYTES) {
      fprintf(stderr,
              "mip target layout mismatch: guarded=%u stride=%u data=%#llx/%#llx "
              "L1=%#llx/%#llx BO=%#llx\n",
              target->guarded, target->stride,
              (unsigned long long)target->data_offset,
              (unsigned long long)target->data_size,
              (unsigned long long)target->level_offset[1],
              (unsigned long long)target->level_size[1],
              (unsigned long long)target->bo.size);
      goto out;
   }
   allocation = target->bo.map;
   if (!fill_words(allocation, 0, ALLOCATION_BYTES, MIP_POISON) ||
       !fill_words(allocation, PAGE_BYTES, PITCH * HEIGHT, 0) ||
       !fill_words(allocation, 0, PAGE_BYTES, GUARD_WORD) ||
       !fill_words(allocation, REQUIRED_BYTES - PAGE_BYTES, PAGE_BYTES,
                   GUARD_WORD) ||
       !fill_words(allocation, FINAL_GUARD_OFFSET, PAGE_BYTES, GUARD_WORD))
      goto out;

   context = screen->context_create(screen, NULL, 0);
   if (!context)
      goto out;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_state = context->create_blend_state(context, &blend);
   dsa_state = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer_state =
      context->create_rasterizer_state(context, &rasterizer);
   element_state = context->create_vertex_elements_state(context, 1,
                                                          &element);
   vs = make_vertex_shader(screen, context);
   fs = make_fragment_shader(screen, context);
   if (!blend_state || !dsa_state || !rasterizer_state || !element_state ||
       !vs || !fs || !((struct chrome9_shader *)fs)->supported ||
       !((struct chrome9_shader *)fs)->semantic_fs_valid ||
       ((struct chrome9_shader *)fs)->semantic_input_mask ||
       !context->flush_resource)
      goto out;

   framebuffer.cbufs[0].texture = resource;
   context->bind_blend_state(context, blend_state);
   context->bind_depth_stencil_alpha_state(context, dsa_state);
   context->bind_rasterizer_state(context, rasterizer_state);
   context->bind_vertex_elements_state(context, element_state);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_constant_buffer(context, MESA_SHADER_FRAGMENT, 0, &constant);
   context->set_framebuffer_state(context, &framebuffer);
   if (chrome9_context_render_target(chrome9_context(context)) != target)
      goto out;
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);

   submitted = chrome9_context(context)->submitted_triangles;
   rejected = chrome9_context(context)->rejected_triangles;
   batches = chrome9_context(context)->submitted_program_batches;
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   context->flush_resource(context, resource);
   context->flush(context, NULL, 0);

   pixels = (uint32_t *)(allocation + target->data_offset);
   for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
      if (pixels[i] != COLOR_RED) {
         fprintf(stderr, "L0 pixel %zu: %08x != %08x\n",
                 i, pixels[i], COLOR_RED);
         goto out;
      }
   }
   if (chrome9_context(context)->submitted_triangles != submitted + 2 ||
       chrome9_context(context)->rejected_triangles != rejected ||
       chrome9_context(context)->submitted_program_batches != batches + 1 ||
       !words_match(allocation, 0, PAGE_BYTES, GUARD_WORD,
                    "leading guard") ||
       !words_match(allocation, REQUIRED_BYTES - PAGE_BYTES, PAGE_BYTES,
                    GUARD_WORD, "internal L0 guard") ||
       !words_match(allocation, FINAL_GUARD_OFFSET, PAGE_BYTES, GUARD_WORD,
                    "final guard") ||
       !words_match(allocation, LEVEL1_OFFSET, LEVEL1_SIZE, MIP_POISON,
                    "L1 payload") ||
       !words_match(allocation, LEVEL1_OFFSET + LEVEL1_SIZE,
                    FINAL_GUARD_OFFSET - LEVEL1_OFFSET - LEVEL1_SIZE,
                    MIP_POISON, "mip padding")) {
      fprintf(stderr,
              "submission mismatch: triangles=+%" PRIu64
              " rejected=+%" PRIu64 " batches=+%" PRIu64 "\n",
              chrome9_context(context)->submitted_triangles - submitted,
              chrome9_context(context)->rejected_triangles - rejected,
              chrome9_context(context)->submitted_program_batches - batches);
      goto out;
   }

   puts("Gallium mip-capable BGRA L0 target passed: one generic batch, "
        "three guards and L1/padding poison intact");
   passed = true;

out:
   if (context) {
      if (fs)
         context->delete_fs_state(context, fs);
      if (vs)
         context->delete_vs_state(context, vs);
      if (element_state)
         context->delete_vertex_elements_state(context, element_state);
      if (rasterizer_state)
         context->delete_rasterizer_state(context, rasterizer_state);
      if (dsa_state)
         context->delete_depth_stencil_alpha_state(context, dsa_state);
      if (blend_state)
         context->delete_blend_state(context, blend_state);
      context->destroy(context);
   }
   if (resource)
      screen->resource_destroy(screen, resource);
   return passed;
}

int
main(int argc, char **argv)
{
   const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
   struct pipe_screen *screen;
   int fd;
   bool passed;

   if (argc > 2) {
      fprintf(stderr, "usage: %s [render-node]\n", argv[0]);
      return 1;
   }
   if (sysconf(_SC_PAGESIZE) != PAGE_BYTES) {
      fputs("test requires 4096-byte pages\n", stderr);
      return 1;
   }
   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      perror(path);
      return 1;
   }
   screen = chrome9_screen_create(fd, NULL);
   close(fd);
   if (!screen) {
      fputs("chrome9_screen_create failed\n", stderr);
      return 1;
   }
   passed = run_smoke(screen);
   screen->destroy(screen);
   return passed ? 0 : 1;
}
