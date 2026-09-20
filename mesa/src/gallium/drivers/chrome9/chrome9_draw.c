/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_draw.h"

#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_resource.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "util/blend.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

struct chrome9_vbuf_render {
   struct vbuf_render base;
   struct chrome9_context *ctx;
   struct vertex_info vinfo;
   enum mesa_prim prim;
   uint8_t *vertices;
   size_t capacity;
   uint16_t vertex_size;
   uint16_t vertex_count;
   bool textured;
   bool layout_valid;
};

static inline struct chrome9_vbuf_render *
chrome9_vbuf_render(struct vbuf_render *render)
{
   return (struct chrome9_vbuf_render *)render;
}

static bool
chrome9_vbuf_need_pipeline(const struct vbuf_render *render,
                           const struct pipe_rasterizer_state *rasterizer,
                           unsigned prim)
{
   (void)render;
   (void)rasterizer;
   (void)prim;
   /* The kernel ABI accepts triangles only.  This also makes draw perform
    * clipping, culling, flat-shade fixup and primitive decomposition. */
   return true;
}

static const struct vertex_info *
chrome9_vbuf_get_vertex_info(struct vbuf_render *render)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   int position, color0, color1, texcoord = -1;

   position = draw_find_shader_output(crender->ctx->draw,
                                      TGSI_SEMANTIC_POSITION, 0);
   color0 = draw_find_shader_output(crender->ctx->draw,
                                    TGSI_SEMANTIC_COLOR, 0);
   color1 = draw_find_shader_output(crender->ctx->draw,
                                    TGSI_SEMANTIC_COLOR, 1);
   crender->textured = crender->ctx->fs &&
      (crender->ctx->fs->fragment_program == CHROME9_FS_TEXTURE_REPLACE ||
       crender->ctx->fs->fragment_program == CHROME9_FS_TEXTURE_MODULATE);
   if (crender->textured) {
      texcoord = draw_find_shader_output(crender->ctx->draw,
                                         TGSI_SEMANTIC_GENERIC, 0);
      if (texcoord < 0)
         texcoord = draw_find_shader_output(crender->ctx->draw,
                                            TGSI_SEMANTIC_TEXCOORD, 0);
   }
   memset(&crender->vinfo, 0, sizeof(crender->vinfo));
   draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, position);
   draw_emit_vertex_attr(&crender->vinfo, EMIT_4UB_BGRA, color0);
   draw_emit_vertex_attr(&crender->vinfo, EMIT_4UB_BGRA, color1);
   if (crender->textured)
      draw_emit_vertex_attr(&crender->vinfo, EMIT_2F, texcoord);
   draw_compute_vertex_size(&crender->vinfo);
   crender->layout_valid = position >= 0 &&
                           (color0 >= 0 ||
                            crender->ctx->fs->fragment_program ==
                               CHROME9_FS_TEXTURE_REPLACE) &&
                           (!crender->textured || texcoord >= 0) &&
                           crender->vinfo.size ==
                              (crender->textured ? 8 : 6);
   return &crender->vinfo;
}

static bool
chrome9_vbuf_allocate_vertices(struct vbuf_render *render,
                               uint16_t vertex_size, uint16_t nr_vertices)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   size_t size;

   if (vertex_size != (crender->textured ? 8 : 6) * sizeof(uint32_t))
      return false;
   size = (size_t)vertex_size * nr_vertices;

   FREE(crender->vertices);
   crender->vertices = size ? MALLOC(size) : NULL;
   if (size && !crender->vertices) {
      crender->capacity = 0;
      return false;
   }
   crender->capacity = size;
   crender->vertex_size = vertex_size;
   crender->vertex_count = nr_vertices;
   return true;
}

static void *
chrome9_vbuf_map_vertices(struct vbuf_render *render)
{
   return chrome9_vbuf_render(render)->vertices;
}

static void
chrome9_vbuf_unmap_vertices(struct vbuf_render *render, uint16_t min_index,
                            uint16_t max_index)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);

   if (min_index > max_index || max_index >= crender->vertex_count)
      crender->layout_valid = false;
}

static void
chrome9_vbuf_set_primitive(struct vbuf_render *render, enum mesa_prim prim)
{
   chrome9_vbuf_render(render)->prim = prim;
}

static int32_t
chrome9_coord_16_16(float value, unsigned extent)
{
   const double minimum = -0.5;
   const double maximum = (double)extent - 0.5;
   double scaled;

   if (!isfinite(value))
      value = 0.0f;
   value = CLAMP((double)value, minimum, maximum);
   scaled = value * 65536.0;
   return scaled < 0.0 ? (int32_t)(scaled - 0.5) :
                         (int32_t)(scaled + 0.5);
}

static int32_t
chrome9_depth_16_16(float value)
{
   double scaled = CLAMP((double)value, 0.0, 1.0) * 65536.0;

   return (int32_t)(scaled + 0.5);
}

static bool
chrome9_blend_factor(unsigned factor, uint32_t *result)
{
   switch (factor) {
   case PIPE_BLENDFACTOR_ZERO:
      *result = DRM_CHROME9_BLEND_FACTOR_ZERO;
      return true;
   case PIPE_BLENDFACTOR_ONE:
      *result = DRM_CHROME9_BLEND_FACTOR_ONE;
      return true;
   case PIPE_BLENDFACTOR_SRC_ALPHA:
      *result = DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA;
      return true;
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
      *result = DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA;
      return true;
   default:
      return false;
   }
}

static bool
chrome9_translate_blend(const struct pipe_blend_state *state,
                        struct drm_chrome9_blend_state *result,
                        bool *successor)
{
   const struct pipe_rt_blend_state *rt;

   memset(result, 0, sizeof(*result));
   result->rgb_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE;
   result->rgb_dst_factor = DRM_CHROME9_BLEND_FACTOR_ZERO;
   result->alpha_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE;
   result->alpha_dst_factor = DRM_CHROME9_BLEND_FACTOR_ZERO;
   result->color_mask = DRM_CHROME9_COLOR_MASK_R |
                        DRM_CHROME9_COLOR_MASK_G |
                        DRM_CHROME9_COLOR_MASK_B |
                        DRM_CHROME9_COLOR_MASK_A;
   *successor = false;
   if (!state)
      return true;
   if (state->independent_blend_enable || state->logicop_enable ||
       state->alpha_to_coverage ||
       state->alpha_to_coverage_dither || state->alpha_to_one ||
       state->max_rt || state->advanced_blend_func ||
       state->blend_coherent)
      return false;

   rt = &state->rt[0];
   result->color_mask = 0;
   if (rt->colormask & PIPE_MASK_R)
      result->color_mask |= DRM_CHROME9_COLOR_MASK_R;
   if (rt->colormask & PIPE_MASK_G)
      result->color_mask |= DRM_CHROME9_COLOR_MASK_G;
   if (rt->colormask & PIPE_MASK_B)
      result->color_mask |= DRM_CHROME9_COLOR_MASK_B;
   if (rt->colormask & PIPE_MASK_A)
      result->color_mask |= DRM_CHROME9_COLOR_MASK_A;
   *successor = rt->blend_enable || rt->colormask != PIPE_MASK_RGBA;
   if (!rt->blend_enable)
      return true;
   if (rt->rgb_func != PIPE_BLEND_ADD || rt->alpha_func != PIPE_BLEND_ADD ||
       !chrome9_blend_factor(rt->rgb_src_factor,
                             &result->rgb_src_factor) ||
       !chrome9_blend_factor(rt->rgb_dst_factor,
                             &result->rgb_dst_factor) ||
       !chrome9_blend_factor(rt->alpha_src_factor,
                             &result->alpha_src_factor) ||
       !chrome9_blend_factor(rt->alpha_dst_factor,
                             &result->alpha_dst_factor))
      return false;
   result->flags = DRM_CHROME9_BLEND_ENABLE;
   return true;
}

static bool
chrome9_draw_state_supported(struct chrome9_context *ctx,
                             struct chrome9_resource **target,
                             struct chrome9_resource **depth,
                             struct chrome9_resource **texture,
                             struct chrome9_sampler_state **sampler,
                             bool textured, bool *depth_active,
                             struct drm_chrome9_blend_state *blend,
                             bool *blend_successor)
{
   const struct pipe_surface *surface;
   struct pipe_resource *resource;

   *depth = NULL;
   *texture = NULL;
   *sampler = NULL;
   *depth_active = false;
   if (!ctx->vs || !ctx->fs || !ctx->fs->supported ||
       !(*target = chrome9_context_render_target(ctx)))
      return false;
   resource = &(*target)->base;
   surface = ctx->framebuffer.nr_cbufs == 1 ?
             &ctx->framebuffer.cbufs[0] : NULL;
   if (!surface || !surface->texture || surface->format != resource->format ||
       surface->level || surface->first_layer || surface->last_layer ||
       resource->target != PIPE_TEXTURE_2D ||
       (resource->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        resource->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
        !ctx->blend_bgra8888) ||
       !(*target)->guarded || !(*target)->bo.handle)
      return false;

   if (!chrome9_translate_blend(ctx->blend, blend, blend_successor))
      return false;
   if (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM)
      *blend_successor = true;
   if (*blend_successor &&
        !(textured ? ctx->draw_textured_triangle_blend :
                     ctx->draw_triangle_blend))
      return false;
   if (ctx->dsa && (ctx->dsa->stencil[0].enabled ||
                    ctx->dsa->stencil[1].enabled ||
                    ctx->dsa->alpha_enabled ||
                    ctx->dsa->depth_bounds_test))
      return false;
   if (ctx->dsa && ctx->dsa->depth_enabled) {
      const struct pipe_surface *surface = &ctx->framebuffer.zsbuf;

      *depth = chrome9_context_depth_target(ctx);
      if (!ctx->draw_triangle_d16 || !ctx->dsa->depth_writemask ||
          ctx->dsa->depth_func != PIPE_FUNC_LESS || !*depth ||
          surface->format != PIPE_FORMAT_Z16_UNORM || surface->level ||
          surface->first_layer || surface->last_layer ||
          (*depth)->base.target != PIPE_TEXTURE_2D ||
          (*depth)->base.format != PIPE_FORMAT_Z16_UNORM ||
          !(*depth)->guarded || !(*depth)->bo.handle ||
          (*depth)->base.width0 != resource->width0 ||
          (*depth)->base.height0 != resource->height0)
         return false;
      *depth_active = true;
      if (*blend_successor)
         return false;
   }
   if (textured) {
      struct pipe_sampler_view *view;
      struct pipe_resource *source;

      if (!ctx->draw_textured_triangle || *depth_active ||
          ctx->num_sampler_states != 1 ||
          ctx->num_sampler_views != 1 ||
          !(*sampler = ctx->sampler_states[0]) || !(*sampler)->supported ||
          !(view = ctx->sampler_views[0]) || !(source = view->texture))
         return false;
      *texture = chrome9_resource(source);
      if (source == resource || source->target != PIPE_TEXTURE_2D ||
          (source->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
           source->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
          source->last_level || !(*texture)->guarded ||
          !(*texture)->bo.handle || ((*texture)->data_offset & 0xff) ||
          (*texture)->stride != ALIGN_POT(source->width0 * 4, 32) ||
          (*texture)->data_size <
             (uint64_t)(*texture)->stride * source->height0)
         return false;
   }
   if (ctx->rasterizer && ctx->rasterizer->scissor && ctx->scissor_valid &&
       (ctx->scissor.minx || ctx->scissor.miny ||
        ctx->scissor.maxx < resource->width0 ||
        ctx->scissor.maxy < resource->height0))
      return false;
   return true;
}

static void
chrome9_emit_triangle(struct chrome9_vbuf_render *crender,
                      unsigned ia, unsigned ib, unsigned ic)
{
   struct chrome9_context *ctx = crender->ctx;
   struct chrome9_resource *target;
   struct chrome9_resource *depth;
   struct chrome9_resource *texture;
   struct chrome9_sampler_state *sampler;
   struct drm_chrome9_draw_triangle args = { 0 };
   struct drm_chrome9_draw_triangle_blend blend_args = { 0 };
   struct drm_chrome9_draw_triangle_d16 depth_args = { 0 };
   struct drm_chrome9_draw_textured_triangle texture_args = { 0 };
   struct drm_chrome9_draw_textured_triangle_blend texture_blend_args = { 0 };
   struct drm_chrome9_blend_state blend_state;
   const unsigned indices[3] = { ia, ib, ic };
   bool blend_successor;
   bool depth_active;

   if (!crender->layout_valid ||
       !chrome9_draw_state_supported(ctx, &target, &depth, &texture, &sampler,
                                     crender->textured, &depth_active,
                                     &blend_state, &blend_successor)) {
      ctx->rejected_triangles++;
      if (!(ctx->warnings & CHROME9_WARN_DRAW_STATE)) {
         fprintf(stderr, "chrome9: triangle rejected by the bounded "
                         "color/D16 draw state\n");
         ctx->warnings |= CHROME9_WARN_DRAW_STATE;
      }
      return;
   }
   (void)texture;
   (void)sampler;
   for (unsigned i = 0; i < 3; i++) {
      const uint8_t *vertex;
      const float *position;
      uint32_t color;

      if (indices[i] >= crender->vertex_count ||
          (size_t)(indices[i] + 1) * crender->vertex_size >
          crender->capacity) {
         ctx->rejected_triangles++;
         return;
      }
      vertex = crender->vertices + (size_t)indices[i] * crender->vertex_size;
      position = (const float *)vertex;
      if (!isfinite(position[0]) || !isfinite(position[1]) ||
          (depth_active && !isfinite(position[2]))) {
         ctx->rejected_triangles++;
         return;
      }
      memcpy(&color, vertex + 4 * sizeof(float), sizeof(color));
      if (crender->textured) {
         const float *texcoord = (const float *)(vertex + 6 * sizeof(uint32_t));

         if (!isfinite(position[2]) || !isfinite(position[3]) ||
             position[3] <= 0.0f || !isfinite(texcoord[0]) ||
             !isfinite(texcoord[1])) {
            ctx->rejected_triangles++;
            return;
         }
         texture_args.vertices[i].x_16_16 =
            chrome9_coord_16_16(position[0], target->base.width0);
         texture_args.vertices[i].y_16_16 =
            chrome9_coord_16_16(position[1], target->base.height0);
         texture_args.vertices[i].z_16_16 =
            chrome9_depth_16_16(position[2]);
         memcpy(&texture_args.vertices[i].rhw_float, &position[3],
                sizeof(position[3]));
         texture_args.vertices[i].color = color;
         memcpy(&texture_args.vertices[i].specular,
                vertex + 5 * sizeof(uint32_t), sizeof(uint32_t));
         memcpy(&texture_args.vertices[i].s_float, &texcoord[0],
                sizeof(texcoord[0]));
         memcpy(&texture_args.vertices[i].t_float, &texcoord[1],
                sizeof(texcoord[1]));
         texture_blend_args.vertices[i] = texture_args.vertices[i];
      } else if (depth_active) {
         depth_args.vertices[i].x_16_16 =
            chrome9_coord_16_16(position[0], target->base.width0);
         depth_args.vertices[i].y_16_16 =
            chrome9_coord_16_16(position[1], target->base.height0);
         depth_args.vertices[i].z_16_16 = chrome9_depth_16_16(position[2]);
         depth_args.vertices[i].color = color;
      } else {
         args.vertices[i].x_16_16 =
            chrome9_coord_16_16(position[0], target->base.width0);
         args.vertices[i].y_16_16 =
            chrome9_coord_16_16(position[1], target->base.height0);
         args.vertices[i].color = color;
         blend_args.vertices[i] = args.vertices[i];
      }
   }

   if (crender->textured) {
      texture_args.destination_handle = target->bo.handle;
      texture_args.texture_handle = texture->bo.handle;
      texture_args.program = ctx->fs->fragment_program ==
                                CHROME9_FS_TEXTURE_REPLACE ?
                                DRM_CHROME9_TEXTURE_PROGRAM_REPLACE :
                                DRM_CHROME9_TEXTURE_PROGRAM_MODULATE;
      texture_args.width = target->base.width0;
      texture_args.height = target->base.height0;
      texture_args.destination_pitch = target->stride;
      texture_args.texture_width = texture->base.width0;
      texture_args.texture_height = texture->base.height0;
      texture_args.texture_pitch = texture->stride;
      texture_args.texture_format = texture->base.format ==
                                      PIPE_FORMAT_B8G8R8X8_UNORM ?
                                      DRM_CHROME9_TEXTURE_FORMAT_BGRX8888 :
                                      DRM_CHROME9_TEXTURE_FORMAT_BGRA8888;
      texture_args.min_filter = sampler->min_filter;
      texture_args.mag_filter = sampler->mag_filter;
      texture_args.wrap_s = sampler->wrap_s;
      texture_args.wrap_t = sampler->wrap_t;
      if (blend_successor) {
         texture_blend_args.destination_handle =
            texture_args.destination_handle;
         texture_blend_args.texture_handle = texture_args.texture_handle;
         texture_blend_args.program = texture_args.program;
         texture_blend_args.width = texture_args.width;
         texture_blend_args.height = texture_args.height;
         texture_blend_args.destination_pitch =
            texture_args.destination_pitch;
         texture_blend_args.texture_width = texture_args.texture_width;
         texture_blend_args.texture_height = texture_args.texture_height;
         texture_blend_args.texture_pitch = texture_args.texture_pitch;
         texture_blend_args.texture_format = texture_args.texture_format;
         texture_blend_args.min_filter = texture_args.min_filter;
         texture_blend_args.mag_filter = texture_args.mag_filter;
         texture_blend_args.wrap_s = texture_args.wrap_s;
         texture_blend_args.wrap_t = texture_args.wrap_t;
         texture_blend_args.blend = blend_state;
         texture_blend_args.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
      }
   } else if (depth_active) {
      depth_args.destination_handle = target->bo.handle;
      depth_args.depth_handle = depth->bo.handle;
      depth_args.width = target->base.width0;
      depth_args.height = target->base.height0;
      depth_args.destination_pitch = target->stride;
      depth_args.depth_pitch = depth->stride;
   } else {
      args.destination_handle = target->bo.handle;
      args.width = target->base.width0;
      args.height = target->base.height0;
      args.pitch = target->stride;
      if (blend_successor) {
         blend_args.destination_handle = args.destination_handle;
         blend_args.width = args.width;
         blend_args.height = args.height;
         blend_args.pitch = args.pitch;
         blend_args.blend = blend_state;
         blend_args.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
      }
   }
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   const bool submitted = crender->textured && blend_successor ?
      chrome9_draw_textured_triangle_blend(fd, &texture_blend_args) :
      crender->textured ?
      chrome9_draw_textured_triangle(fd, &texture_args) :
      depth_active ? chrome9_draw_triangle_d16(fd, &depth_args) :
      blend_successor ? chrome9_draw_triangle_blend(fd, &blend_args) :
                     chrome9_draw_triangle(fd, &args);

   if (submitted) {
      ctx->submitted_triangles++;
      chrome9_context_render_target_written(ctx);
   } else {
      ctx->rejected_triangles++;
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr,
                 "chrome9: DRAW_%sTRIANGLE%s failed: %s target=%ux%u/%u\n",
                 crender->textured ? "TEXTURED_" : "",
                 depth_active ? "_D16" : "", strerror(errno),
                 target->base.width0, target->base.height0, target->stride);
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
   }
}

static void
chrome9_vbuf_draw_arrays(struct vbuf_render *render, unsigned start,
                         unsigned nr)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   unsigned end;

   if (start > crender->vertex_count || nr > crender->vertex_count - start)
      return;
   end = start + nr;
   switch (crender->prim) {
   case MESA_PRIM_TRIANGLES:
      for (unsigned i = start; i + 2 < end; i += 3)
         chrome9_emit_triangle(crender, i, i + 1, i + 2);
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      for (unsigned i = start; i + 2 < end; i++)
         if ((i - start) & 1)
            chrome9_emit_triangle(crender, i + 1, i, i + 2);
         else
            chrome9_emit_triangle(crender, i, i + 1, i + 2);
      break;
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      for (unsigned i = start + 1; i + 1 < end; i++)
         chrome9_emit_triangle(crender, start, i, i + 1);
      break;
   case MESA_PRIM_QUADS:
      for (unsigned i = start; i + 3 < end; i += 4) {
         chrome9_emit_triangle(crender, i, i + 1, i + 3);
         chrome9_emit_triangle(crender, i + 1, i + 2, i + 3);
      }
      break;
   case MESA_PRIM_QUAD_STRIP:
      for (unsigned i = start; i + 3 < end; i += 2) {
         chrome9_emit_triangle(crender, i, i + 1, i + 3);
         chrome9_emit_triangle(crender, i, i + 3, i + 2);
      }
      break;
   default:
      crender->ctx->rejected_triangles++;
      break;
   }
}

static void
chrome9_vbuf_draw_elements(struct vbuf_render *render,
                           const uint16_t *indices, unsigned nr_indices)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);

   if (!indices)
      return;
   switch (crender->prim) {
   case MESA_PRIM_TRIANGLES:
      for (unsigned i = 0; i + 2 < nr_indices; i += 3)
         chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                               indices[i + 2]);
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      for (unsigned i = 0; i + 2 < nr_indices; i++)
         if (i & 1)
            chrome9_emit_triangle(crender, indices[i + 1], indices[i],
                                  indices[i + 2]);
         else
            chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                                  indices[i + 2]);
      break;
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      for (unsigned i = 1; i + 1 < nr_indices; i++)
         chrome9_emit_triangle(crender, indices[0], indices[i],
                               indices[i + 1]);
      break;
   case MESA_PRIM_QUADS:
      for (unsigned i = 0; i + 3 < nr_indices; i += 4) {
         chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                               indices[i + 3]);
         chrome9_emit_triangle(crender, indices[i + 1], indices[i + 2],
                               indices[i + 3]);
      }
      break;
   case MESA_PRIM_QUAD_STRIP:
      for (unsigned i = 0; i + 3 < nr_indices; i += 2) {
         chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                               indices[i + 3]);
         chrome9_emit_triangle(crender, indices[i], indices[i + 3],
                               indices[i + 2]);
      }
      break;
   default:
      crender->ctx->rejected_triangles++;
      break;
   }
}

static void
chrome9_vbuf_release_vertices(struct vbuf_render *render)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);

   FREE(crender->vertices);
   crender->vertices = NULL;
   crender->capacity = 0;
   crender->vertex_count = 0;
}

static void
chrome9_vbuf_destroy(struct vbuf_render *render)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);

   FREE(crender->vertices);
   FREE(crender);
}

static struct vbuf_render *
chrome9_vbuf_create(struct chrome9_context *ctx)
{
   struct chrome9_vbuf_render *render = CALLOC_STRUCT(chrome9_vbuf_render);

   if (!render)
      return NULL;
   render->ctx = ctx;
   render->base.max_indices = UINT16_MAX;
   render->base.max_vertex_buffer_bytes = UINT16_MAX * 8 * sizeof(uint32_t);
   render->base.need_pipeline = chrome9_vbuf_need_pipeline;
   render->base.get_vertex_info = chrome9_vbuf_get_vertex_info;
   render->base.allocate_vertices = chrome9_vbuf_allocate_vertices;
   render->base.map_vertices = chrome9_vbuf_map_vertices;
   render->base.unmap_vertices = chrome9_vbuf_unmap_vertices;
   render->base.set_primitive = chrome9_vbuf_set_primitive;
   render->base.draw_elements = chrome9_vbuf_draw_elements;
   render->base.draw_arrays = chrome9_vbuf_draw_arrays;
   render->base.release_vertices = chrome9_vbuf_release_vertices;
   render->base.destroy = chrome9_vbuf_destroy;
   return &render->base;
}

struct draw_stage *
chrome9_draw_vbuf_stage(struct chrome9_context *ctx)
{
   struct vbuf_render *render = chrome9_vbuf_create(ctx);
   struct draw_stage *stage;

   if (!render)
      return NULL;
   stage = draw_vbuf_stage(ctx->draw, render);
   if (!stage) {
      render->destroy(render);
      return NULL;
   }
   draw_set_render(ctx->draw, render);
   return stage;
}
