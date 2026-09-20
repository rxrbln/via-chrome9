/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_public.h"
#include "chrome9_resource.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"
#include "util/u_simple_shaders.h"

struct smoke_vertex {
   float position[4];
   float color[4];
};

static bool
run_pipe_triangle(struct pipe_screen *screen, struct pipe_resource *resource,
                  struct pipe_resource *depth_resource)
{
   static const enum tgsi_semantic semantics[] = {
      TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_COLOR,
   };
   static const unsigned semantic_indices[] = { 0, 0 };
   static const struct smoke_vertex vertices[] = {
      { { -1, -1, 0, 1 }, { 1, 0, 0, 1 } },
      { {  1, -1, 0, 1 }, { 0, 1, 0, 1 } },
      { {  1,  1, 0, 1 }, { 0, 0, 1, 1 } },
   };
   static const struct smoke_vertex far_vertices[] = {
      { { -1, -1,  0.5, 1 }, { 0, 1, 0, 1 } },
      { {  1, -1,  0.5, 1 }, { 0, 1, 0, 1 } },
      { {  1,  1,  0.5, 1 }, { 0, 1, 0, 1 } },
   };
   static const struct smoke_vertex near_vertices[] = {
      { { -1, -1, -0.5, 1 }, { 1, 0, 0, 1 } },
      { {  1, -1, -0.5, 1 }, { 1, 0, 0, 1 } },
      { {  1,  1, -0.5, 1 }, { 1, 0, 0, 1 } },
   };
   static const struct smoke_vertex rejected_vertices[] = {
      { { -1, -1, 0, 1 }, { 0, 0, 1, 1 } },
      { {  1, -1, 0, 1 }, { 0, 0, 1, 1 } },
      { {  1,  1, 0, 1 }, { 0, 0, 1, 1 } },
   };
   struct pipe_context *context = NULL;
   struct pipe_blend_state blend = { 0 };
   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   struct pipe_rasterizer_state rasterizer = { 0 };
   struct pipe_vertex_element elements[2] = { 0 };
   struct pipe_vertex_buffer vertex_buffer = {
      .is_user_buffer = true,
      .buffer.user = vertices,
   };
   struct pipe_framebuffer_state framebuffer = {
      .width = 257,
      .height = 129,
      .nr_cbufs = 1,
      .cbufs[0] = {
         .format = PIPE_FORMAT_B8G8R8X8_UNORM,
         .texture = resource,
      },
   };
   struct pipe_viewport_state viewport = {
      .scale = { 128.5, 64.5, 0.5 },
      .translate = { 128.5, 64.5, 0.5 },
   };
   struct pipe_scissor_state scissor = {
      .minx = 0,
      .miny = 0,
      .maxx = 257,
      .maxy = 129,
   };
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES,
      .index_bounds_valid = true,
      .instance_count = 1,
      .min_index = 0,
      .max_index = 2,
   };
   struct pipe_draw_start_count_bias draw = {
      .start = 0,
      .count = 3,
   };
   union pipe_color_union clear_color = { 0 };
   struct chrome9_resource *surface = chrome9_resource(resource);
   uint32_t *pixels;
   size_t changed = 0;
   uint16_t sample_depth = 0;
   void *blend_handle = NULL;
   void *dsa_handle = NULL;
   void *rasterizer_handle = NULL;
   void *elements_handle = NULL;
   void *vs = NULL;
   void *fs = NULL;
   bool passed = false;

   context = screen->context_create(screen, NULL, 0);
   if (!context)
      goto out;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend_handle = context->create_blend_state(context, &blend);
   if (depth_resource) {
      dsa.depth_enabled = true;
      dsa.depth_writemask = true;
      dsa.depth_func = PIPE_FUNC_LESS;
      framebuffer.zsbuf.format = PIPE_FORMAT_Z16_UNORM;
      framebuffer.zsbuf.texture = depth_resource;
      vertex_buffer.buffer.user = far_vertices;
   }
   dsa_handle = context->create_depth_stencil_alpha_state(context, &dsa);
   rasterizer.half_pixel_center = true;
   rasterizer.bottom_edge_rule = true;
   rasterizer.depth_clip_near = true;
   rasterizer.depth_clip_far = true;
   rasterizer_handle = context->create_rasterizer_state(context, &rasterizer);
   elements[0].src_offset = offsetof(struct smoke_vertex, position);
   elements[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[0].src_stride = sizeof(struct smoke_vertex);
   elements[1].src_offset = offsetof(struct smoke_vertex, color);
   elements[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   elements[1].src_stride = sizeof(struct smoke_vertex);
   elements_handle = context->create_vertex_elements_state(context, 2,
                                                            elements);
   vs = util_make_vertex_passthrough_shader(context, 2, semantics,
                                             semantic_indices, false);
   fs = util_make_fragment_passthrough_shader(context, TGSI_SEMANTIC_COLOR,
                                               TGSI_INTERPOLATE_COLOR, false);
   if (!blend_handle || !dsa_handle || !rasterizer_handle ||
       !elements_handle || !vs || !fs)
      goto out;

   context->bind_blend_state(context, blend_handle);
   context->bind_depth_stencil_alpha_state(context, dsa_handle);
   context->bind_rasterizer_state(context, rasterizer_handle);
   context->bind_vertex_elements_state(context, elements_handle);
   context->bind_vs_state(context, vs);
   context->bind_fs_state(context, fs);
   context->set_vertex_buffers(context, 1, &vertex_buffer);
   context->set_framebuffer_state(context, &framebuffer);
   context->set_viewport_states(context, 0, 1, &viewport);
   context->set_scissor_states(context, 0, 1, &scissor);
   context->clear(context, PIPE_CLEAR_COLOR0 |
                             (depth_resource ? PIPE_CLEAR_DEPTH : 0),
                  PIPE_MASK_RGBA, 0xff, NULL,
                  &clear_color, 1.0, 0);
   context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   if (depth_resource) {
      vertex_buffer.buffer.user = near_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
      vertex_buffer.buffer.user = rejected_vertices;
      context->set_vertex_buffers(context, 1, &vertex_buffer);
      context->draw_vbo(context, &info, 0, NULL, &draw, 1);
   }
   context->flush(context, NULL, 0);

   pixels = (uint32_t *)((uint8_t *)surface->bo.map + surface->data_offset);
   for (unsigned y = 0; y < resource->height0; y++)
      for (unsigned x = 0; x < resource->width0; x++)
         changed += pixels[(size_t)y * surface->stride / 4 + x] != 0;
   passed = changed >= 12000 && changed <= 20000;
   if (depth_resource) {
      struct chrome9_resource *depth = chrome9_resource(depth_resource);
      uint16_t *depth_pixels = (uint16_t *)
         ((uint8_t *)depth->bo.map + depth->data_offset);

      sample_depth = depth_pixels[32 * (depth->stride / 2) + 192];
      passed = passed &&
               (pixels[32 * (surface->stride / 4) + 192] & 0xffffff) ==
                  0xff0000 &&
               sample_depth >= 0x3ffe && sample_depth <= 0x4001;
   }
   if (!passed)
      fprintf(stderr,
              "Gallium triangle coverage/depth mismatch: %zu sample=%08x/%04x\n",
              changed, pixels[32 * (surface->stride / 4) + 192],
              sample_depth);
   else if (depth_resource)
      printf("Gallium D16 LESS/write passed: changed=%zu sample=%08x/%04x\n",
             changed, pixels[32 * (surface->stride / 4) + 192],
             sample_depth);
   else
      printf("Gallium draw/vbuf triangle passed: changed=%zu sample=%08x\n",
             changed, pixels[32 * (surface->stride / 4) + 192]);

out:
   if (context) {
      if (vs)
         context->delete_vs_state(context, vs);
      if (fs)
         context->delete_fs_state(context, fs);
      if (elements_handle)
         context->delete_vertex_elements_state(context, elements_handle);
      if (rasterizer_handle)
         context->delete_rasterizer_state(context, rasterizer_handle);
      if (dsa_handle)
         context->delete_depth_stencil_alpha_state(context, dsa_handle);
      if (blend_handle)
         context->delete_blend_state(context, blend_handle);
      context->destroy(context);
   }
   return passed;
}

int
main(int argc, char **argv)
{
   const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
   struct pipe_resource templ = {
      .target = PIPE_BUFFER,
      .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 4096,
      .height0 = 1,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_CUSTOM,
   };
   struct pipe_resource *resource;
   struct pipe_resource surface_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8X8_UNORM,
      .width0 = 257,
      .height0 = 129,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
   };
   struct pipe_resource depth_templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_Z16_UNORM,
      .width0 = 257,
      .height0 = 129,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_LINEAR,
   };
   struct chrome9_resource *surface;
   struct chrome9_resource *shared_surface;
   struct pipe_resource *shared_resource;
   struct pipe_resource *depth_resource = NULL;
   struct winsys_handle whandle = { .type = WINSYS_HANDLE_TYPE_FD };
   struct pipe_screen *screen;
   uint32_t *pixels;
   long page_size;
   int fd;

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

   printf("name: %s\nvendor: %s\ndevice vendor: %s\n",
          screen->get_name(screen), screen->get_vendor(screen),
          screen->get_device_vendor(screen));
   printf("PCI: %04x:%04x accelerated=%d\n",
          screen->caps.vendor_id, screen->caps.device_id,
          screen->caps.accelerated);

   resource = screen->resource_create(screen, &templ);
   if (!resource) {
      fputs("PIPE_BUFFER creation failed\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   screen->resource_destroy(screen, resource);

   if (!screen->is_format_supported(screen, surface_templ.format,
                                    surface_templ.target, 1, 1,
                                    surface_templ.bind)) {
      fputs("XRGB8888 render-target format rejected\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   resource = screen->resource_create(screen, &surface_templ);
   if (!resource) {
      fputs("guarded XRGB8888 resource creation failed\n", stderr);
      screen->destroy(screen);
      return 1;
   }
   surface = chrome9_resource(resource);
   page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 || surface->stride != 1056 ||
       surface->data_offset != (uint64_t)page_size ||
       surface->data_size != 1056U * 129U || !surface->guarded ||
       !surface->bo.map) {
      fputs("guarded XRGB8888 layout mismatch\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   memset(surface->bo.map, 0x5a, surface->bo.size);
   pixels = (uint32_t *)((uint8_t *)surface->bo.map + surface->data_offset);
   pixels[0] = 0x00112233;
   pixels[(128U * surface->stride) / 4 + 256U] = 0x00aabbcc;
   if (pixels[0] != 0x00112233 ||
       pixels[(128U * surface->stride) / 4 + 256U] != 0x00aabbcc ||
       ((uint8_t *)surface->bo.map)[0] != 0x5a ||
       ((uint8_t *)surface->bo.map)[surface->bo.size - 1] != 0x5a) {
      fputs("guarded XRGB8888 mmap round trip failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }

   if (!screen->resource_get_handle(
          screen, NULL, resource, &whandle,
          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE) ||
       whandle.stride != surface->stride ||
       whandle.offset != surface->data_offset ||
       whandle.modifier != DRM_FORMAT_MOD_LINEAR) {
      fputs("XRGB8888 PRIME export failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   shared_resource = screen->resource_from_handle(
      screen, &surface_templ, &whandle,
      PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   close(whandle.handle);
   if (!shared_resource) {
      fputs("XRGB8888 PRIME import failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   shared_surface = chrome9_resource(shared_resource);
   ((uint32_t *)((uint8_t *)shared_surface->bo.map +
                 shared_surface->data_offset))[1] = 0x0055aa77;
   if (pixels[1] != 0x0055aa77) {
      fputs("XRGB8888 PRIME alias mismatch\n", stderr);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   if (!run_pipe_triangle(screen, shared_resource, NULL)) {
      fputs("Chrome9 imported-shadow Gallium triangle failed\n", stderr);
      screen->resource_destroy(screen, shared_resource);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   screen->resource_destroy(screen, shared_resource);
   if (!run_pipe_triangle(screen, resource, NULL)) {
      fputs("Chrome9 Gallium triangle failed\n", stderr);
      screen->resource_destroy(screen, resource);
      screen->destroy(screen);
      return 1;
   }
   if (screen->is_format_supported(screen, depth_templ.format,
                                   depth_templ.target, 1, 1,
                                   depth_templ.bind)) {
      struct chrome9_resource *depth;

      depth_resource = screen->resource_create(screen, &depth_templ);
      if (!depth_resource) {
         fputs("guarded D16 resource creation failed\n", stderr);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      depth = chrome9_resource(depth_resource);
      if (depth->stride != 544 ||
          depth->data_offset != (uint64_t)page_size ||
          depth->data_size != 544U * 129U || !depth->guarded ||
          !depth->bo.map ||
          !run_pipe_triangle(screen, resource, depth_resource)) {
         fputs("Chrome9 Gallium D16 triangle failed\n", stderr);
         screen->resource_destroy(screen, depth_resource);
         screen->resource_destroy(screen, resource);
         screen->destroy(screen);
         return 1;
      }
      screen->resource_destroy(screen, depth_resource);
   } else {
      puts("D16 triangle capability unavailable; depth smoke skipped");
   }
   screen->resource_destroy(screen, resource);
   screen->destroy(screen);
   puts("Mesa Chrome9 context, Gallium triangle/D16, guarded resources, and PRIME sharing smoke passed");
   return 0;
}
