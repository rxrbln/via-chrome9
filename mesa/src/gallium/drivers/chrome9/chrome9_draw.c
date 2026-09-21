/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_draw.h"

#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_resource.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
   struct drm_chrome9_draw_triangle_batch batch;
   struct drm_chrome9_vertex_d16
      batch_vertices[DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES * 3];
   struct drm_chrome9_draw_textured_triangle_batch batch_textured_state;
   struct drm_chrome9_textured_vertex
      batch_textured_vertices[
         DRM_CHROME9_TEXTURED_TRIANGLE_BATCH_MAX_TRIANGLES * 3];
   struct drm_chrome9_draw_triangle_d16_rhw_batch batch_rhw;
   struct drm_chrome9_draw_triangle_d16_rhw_run batch_rhw_run;
   struct drm_chrome9_draw_program_triangle_d16_rhw_batch batch_program;
   struct drm_chrome9_vertex_d16_rhw
      batch_vertices_rhw[DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES * 3];
   unsigned batch_count;
   bool batch_failed;
   bool batch_perspective;
   bool batch_large_run;
   bool batch_compiled_program;
   bool batch_textured;
   uint8_t texture_count;
   uint8_t packed_generic;
   bool textured;
   bool layout_valid;
};

static inline struct chrome9_vbuf_render *
chrome9_vbuf_render(struct vbuf_render *render)
{
   return (struct chrome9_vbuf_render *)render;
}

static bool
chrome9_fs_uses_constant_color(unsigned program)
{
   switch (program) {
   case CHROME9_FS_CONSTANT_COLOR:
   case CHROME9_FS_CONSTANT_MODULATE:
   case CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE:
   case CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE:
   case CHROME9_FS_TEXTURE_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA:
      return true;
   default:
      return false;
   }
}

static bool
chrome9_two_texture_coord_valid(const struct chrome9_context *ctx,
                                const struct chrome9_sampler_state *sampler,
                                bool s_axis, float coord)
{
   const unsigned wrap = s_axis ? sampler->wrap_s : sampler->wrap_t;

   if (!isfinite(coord))
      return false;
   if (coord >= 0.0f && coord <= 1.0f)
      return true;
   return ctx->two_texture_signed_repeat_coords && fabsf(coord) <= 32.0f &&
          wrap == DRM_CHROME9_TEXTURE_WRAP_REPEAT;
}

static bool
chrome9_program_source(enum chrome9_fs_leaf leaf,
                       struct drm_chrome9_ps_source *source)
{
   memset(source, 0, sizeof(*source));
   switch (leaf) {
   case CHROME9_FS_LEAF_V0:
      source->file = DRM_CHROME9_PS_SOURCE_VARYING;
      break;
   case CHROME9_FS_LEAF_C0:
      source->file = DRM_CHROME9_PS_SOURCE_CONSTANT;
      break;
   default:
      return false;
   }
   for (unsigned c = 0; c < ARRAY_SIZE(source->swizzle); c++)
      source->swizzle[c] = c;
   return true;
}

static bool
chrome9_program_instruction(const struct chrome9_shader *shader,
                            struct drm_chrome9_ps_instruction *instruction)
{
   if (!shader || !shader->native_fs_valid || !shader->native_fs_from_nir ||
       shader->native_fs.alu_count != 1 || shader->native_fs.tau_count ||
       shader->native_fs.sampler_mask ||
       (shader->native_fs.varying_mask & ~1u) ||
       (shader->native_fs.constant_mask & ~1u))
      return false;

   memset(instruction, 0, sizeof(*instruction));
   switch (shader->native_fs_key.op) {
   case CHROME9_FS_OP_MOV:
      instruction->op = DRM_CHROME9_PS_OP_MOV;
      return chrome9_program_source(shader->native_fs_key.src[0],
                                    &instruction->src[0]);
   case CHROME9_FS_OP_MUL:
      instruction->op = DRM_CHROME9_PS_OP_MUL;
      return chrome9_program_source(shader->native_fs_key.src[0],
                                    &instruction->src[0]) &&
             chrome9_program_source(shader->native_fs_key.src[1],
                                    &instruction->src[1]);
   default:
      return false;
   }
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
   int position, color0, color1, generic0 = -1;
   int generic1 = -1;
   int texcoord[2] = { -1, -1 };

   position = draw_find_shader_output(crender->ctx->draw,
                                      TGSI_SEMANTIC_POSITION, 0);
   color0 = draw_find_shader_output(crender->ctx->draw,
                                    TGSI_SEMANTIC_COLOR, 0);
   color1 = draw_find_shader_output(crender->ctx->draw,
                                    TGSI_SEMANTIC_COLOR, 1);
   unsigned program = crender->ctx->fs ?
      crender->ctx->fs->fragment_program : CHROME9_FS_UNSUPPORTED;
   bool compiled_primary = program == CHROME9_FS_COMPILED_ALU &&
      (crender->ctx->fs->native_fs.varying_mask & 1);

   crender->packed_generic =
      program == CHROME9_FS_PRIMARY_SCALAR_RGBX ? 1 :
      program == CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE ? 2 :
      program == CHROME9_FS_PRIMARY_GENERIC_RGBA ? 3 :
      program == CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX ? 4 :
      program == CHROME9_FS_PRIMARY_GENERIC_RGBX ? 5 :
      program == CHROME9_FS_IOQUAKE3_GENERIC_PACKED ? 6 :
      program == CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED ? 7 : 0;
   if (crender->packed_generic)
      generic0 = draw_find_shader_output(crender->ctx->draw,
                                         TGSI_SEMANTIC_GENERIC, 0);
   if (crender->packed_generic >= 6)
      generic1 = draw_find_shader_output(crender->ctx->draw,
                                         TGSI_SEMANTIC_GENERIC, 1);
   crender->texture_count =
      (program == CHROME9_FS_TWO_TEXTURE_MODULATE ||
       program == CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA ||
       program == CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY ||
       program == CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED) ? 2 :
      (program == CHROME9_FS_TEXTURE_REPLACE ||
       program == CHROME9_FS_TEXTURE_RGBX ||
       program == CHROME9_FS_TEXTURE_MODULATE ||
       program == CHROME9_FS_TEXTURE_PROJECTED_MODULATE ||
       program == CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
       program == CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA ||
       program == CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA ||
       program == CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE ||
       program == CHROME9_FS_TEXTURE_CONSTANT_MODULATE ||
       program == CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE ||
       program == CHROME9_FS_IOQUAKE3_GENERIC_PACKED) ? 1 : 0;
   crender->textured = crender->texture_count != 0;
   for (unsigned unit = 0; unit < crender->texture_count; unit++) {
      texcoord[unit] = draw_find_shader_output(crender->ctx->draw,
                                               TGSI_SEMANTIC_GENERIC, unit);
      if (texcoord[unit] < 0)
         texcoord[unit] = draw_find_shader_output(crender->ctx->draw,
                                                  TGSI_SEMANTIC_TEXCOORD,
                                                  unit);
   }
   memset(&crender->vinfo, 0, sizeof(crender->vinfo));
   draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, position);
   if (crender->packed_generic >= 6) {
      draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, generic0);
      draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, generic1);
   } else {
      draw_emit_vertex_attr(&crender->vinfo,
                            crender->packed_generic ? EMIT_4F : EMIT_4UB_BGRA,
                            crender->packed_generic ? generic0 : color0);
      draw_emit_vertex_attr(&crender->vinfo, EMIT_4UB_BGRA, color1);
   }
   /* Keep a fixed four-float slot for each coordinate set.  Ordinary 2D
    * sampling consumes only S/T; linked shaders are allowed to export a vec2
    * and leave the unused R/Q components at draw's defaults. */
   if (crender->packed_generic != 2 && crender->packed_generic < 6)
      for (unsigned unit = 0; unit < crender->texture_count; unit++)
         draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, texcoord[unit]);
   draw_compute_vertex_size(&crender->vinfo);
   crender->layout_valid = position >= 0 &&
                           (crender->packed_generic >= 6 ?
                            generic0 >= 0 && generic1 >= 0 :
                            crender->packed_generic ? generic0 >= 0 :
                            (color0 >= 0 || (!compiled_primary &&
                             (program != CHROME9_FS_PRIMARY_COLOR &&
                              program != CHROME9_FS_TEXTURE_MODULATE &&
                              program != CHROME9_FS_TEXTURE_PROJECTED_MODULATE &&
                              program !=
                                 CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA &&
                              program !=
                                 CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA &&
                              program !=
                                 CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY)))) &&
                           (!crender->textured ||
                            ((crender->packed_generic == 2 ||
                              crender->packed_generic >= 6 ||
                              texcoord[0] >= 0) &&
                             (crender->texture_count == 1 ||
                              crender->packed_generic >= 6 ||
                              texcoord[1] >= 0))) &&
                           crender->vinfo.size ==
                              (crender->packed_generic >= 6 ? 12 :
                               crender->packed_generic ?
                               9 : 6 + 4 * crender->texture_count);
   return &crender->vinfo;
}

static bool
chrome9_fragment_constant_color(struct chrome9_context *ctx, uint32_t *color)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants[0];
   const uint8_t *map;
   float constant[2][4], rgba[4];
   unsigned count;

   if (!ctx->fs || !chrome9_fs_uses_constant_color(
                      ctx->fs->fragment_program))
      return false;
   count = (ctx->fs->fragment_program == CHROME9_FS_CONSTANT_MODULATE ||
            ctx->fs->fragment_program ==
               CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE ||
            ctx->fs->fragment_program ==
               CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE) ? 2 : 1;
   if (cb->buffer_size < count * sizeof(constant[0]) ||
       cb->buffer_offset > UINT32_MAX - count * sizeof(constant[0]))
      return false;
   if (cb->user_buffer) {
      map = (const uint8_t *)cb->user_buffer + cb->buffer_offset;
   } else if (cb->buffer && cb->buffer->target == PIPE_BUFFER &&
              cb->buffer->width0 >= count * sizeof(constant[0]) &&
              cb->buffer_offset <=
                 cb->buffer->width0 - count * sizeof(constant[0])) {
      map = chrome9_resource(cb->buffer)->bo.map;
      if (!map)
         return false;
      map += cb->buffer_offset;
   } else {
      return false;
   }
   memcpy(constant, map, count * sizeof(constant[0]));
   for (unsigned i = 0; i < count; i++)
      for (unsigned c = 0; c < ARRAY_SIZE(constant[i]); c++)
         if (!isfinite(constant[i][c]))
            return false;
   switch (ctx->fs->fragment_program) {
   case CHROME9_FS_CONSTANT_COLOR:
   case CHROME9_FS_TEXTURE_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA:
      memcpy(rgba, constant[0], sizeof(rgba));
      break;
   case CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE:
      for (unsigned c = 0; c < ARRAY_SIZE(rgba); c++)
         rgba[c] = constant[0][3];
      break;
   case CHROME9_FS_CONSTANT_MODULATE:
      for (unsigned c = 0; c < ARRAY_SIZE(rgba); c++)
         rgba[c] = constant[0][c] * constant[1][c];
      break;
   case CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE:
      for (unsigned c = 0; c < ARRAY_SIZE(rgba); c++)
         rgba[c] = constant[0][c] * constant[1][3];
      break;
   case CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE:
      for (unsigned c = 0; c < ARRAY_SIZE(rgba); c++)
         rgba[c] = constant[0][3] * constant[1][c];
      break;
   default:
      return false;
   }
   for (unsigned i = 0; i < ARRAY_SIZE(rgba); i++)
      if (!isfinite(rgba[i]))
         return false;
   if (ctx->fs->fragment_program ==
          CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
       ctx->fs->fragment_program ==
          CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA) {
      /* This bounded fallback supplies c0 through normalized D1.  Admit
       * only constants represented exactly by that UNORM8 path; general
       * 32-bit c0 needs the native fragment-constant successor. */
      for (unsigned i = 0; i < ARRAY_SIZE(rgba); i++) {
         uint8_t channel = float_to_ubyte(rgba[i]);

         if (rgba[i] != (float)channel / 255.0f)
            return false;
      }
   }
   *color = (uint32_t)float_to_ubyte(rgba[2]) |
            (uint32_t)float_to_ubyte(rgba[1]) << 8 |
            (uint32_t)float_to_ubyte(rgba[0]) << 16 |
            (uint32_t)float_to_ubyte(rgba[3]) << 24;
   return true;
}

static bool
chrome9_fragment_constant_c0(struct chrome9_context *ctx, uint32_t words[4])
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants[0];
   const uint8_t *map;
   float value[4];

   if (cb->buffer_size < sizeof(value) ||
       cb->buffer_offset > UINT32_MAX - sizeof(value))
      return false;
   if (cb->user_buffer) {
      map = (const uint8_t *)cb->user_buffer + cb->buffer_offset;
   } else if (cb->buffer && cb->buffer->target == PIPE_BUFFER &&
              cb->buffer->width0 >= sizeof(value) &&
              cb->buffer_offset <= cb->buffer->width0 - sizeof(value)) {
      map = chrome9_resource(cb->buffer)->bo.map;
      if (!map)
         return false;
      map += cb->buffer_offset;
   } else {
      return false;
   }
   memcpy(value, map, sizeof(value));
   for (unsigned i = 0; i < ARRAY_SIZE(value); i++)
      if (!isfinite(value[i]))
         return false;
   memcpy(words, value, sizeof(value));
   return true;
}

static bool
chrome9_vbuf_allocate_vertices(struct vbuf_render *render,
                               uint16_t vertex_size, uint16_t nr_vertices)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   size_t size;

   if (vertex_size != (crender->packed_generic >= 6 ? 12 :
                       crender->packed_generic ? 9 :
                       6 + 4 * crender->texture_count) * sizeof(uint32_t))
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
chrome9_triangle_nondegenerate(const int32_t x[3], const int32_t y[3])
{
   const int64_t dx10 = (int64_t)x[1] - x[0];
   const int64_t dy10 = (int64_t)y[1] - y[0];
   const int64_t dx20 = (int64_t)x[2] - x[0];
   const int64_t dy20 = (int64_t)y[2] - y[0];

   return dx10 * dy20 != dy10 * dx20;
}

static bool
chrome9_blend_factor(unsigned factor, bool extended, uint32_t *result)
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
   case PIPE_BLENDFACTOR_SRC_COLOR:
      *result = DRM_CHROME9_BLEND_FACTOR_SRC_COLOR;
      return extended;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR:
      *result = DRM_CHROME9_BLEND_FACTOR_INV_SRC_COLOR;
      return extended;
   case PIPE_BLENDFACTOR_DST_ALPHA:
      *result = DRM_CHROME9_BLEND_FACTOR_DST_ALPHA;
      return extended;
   case PIPE_BLENDFACTOR_INV_DST_ALPHA:
      *result = DRM_CHROME9_BLEND_FACTOR_INV_DST_ALPHA;
      return extended;
   case PIPE_BLENDFACTOR_DST_COLOR:
      *result = DRM_CHROME9_BLEND_FACTOR_DST_COLOR;
      return extended;
   case PIPE_BLENDFACTOR_INV_DST_COLOR:
      *result = DRM_CHROME9_BLEND_FACTOR_INV_DST_COLOR;
      return extended;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
      *result = DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      return extended;
   default:
      return false;
   }
}

static bool
chrome9_translate_blend(const struct pipe_blend_state *state,
                        struct drm_chrome9_blend_state *result,
                        bool extended, bool *successor)
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
       !chrome9_blend_factor(rt->rgb_src_factor, extended,
                             &result->rgb_src_factor) ||
       !chrome9_blend_factor(rt->rgb_dst_factor, extended,
                             &result->rgb_dst_factor) ||
       !chrome9_blend_factor(rt->alpha_src_factor, extended,
                             &result->alpha_src_factor) ||
       !chrome9_blend_factor(rt->alpha_dst_factor, extended,
                             &result->alpha_dst_factor))
      return false;
   result->flags = DRM_CHROME9_BLEND_ENABLE;
   return true;
}

static bool
chrome9_blend_is_source_over(const struct drm_chrome9_blend_state *blend)
{
   return blend->flags == DRM_CHROME9_BLEND_ENABLE &&
          blend->rgb_src_factor == DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA &&
          blend->rgb_dst_factor ==
             DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA &&
          blend->alpha_src_factor == DRM_CHROME9_BLEND_FACTOR_ONE &&
          blend->alpha_dst_factor ==
             DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA &&
          blend->rgb_func == DRM_CHROME9_BLEND_FUNC_ADD &&
          blend->alpha_func == DRM_CHROME9_BLEND_FUNC_ADD &&
          blend->color_mask == (DRM_CHROME9_COLOR_MASK_R |
                                DRM_CHROME9_COLOR_MASK_G |
                                DRM_CHROME9_COLOR_MASK_B |
                                DRM_CHROME9_COLOR_MASK_A);
}

static bool
chrome9_fragment_uniform_u32(struct chrome9_context *ctx, unsigned offset,
                             uint32_t *value)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants[0];
   const uint8_t *map;

   if ((offset & 3) || cb->buffer_size < sizeof(*value) ||
       offset > cb->buffer_size - sizeof(*value) ||
       cb->buffer_offset > UINT32_MAX - offset - sizeof(*value))
      return false;
   if (cb->user_buffer) {
      map = (const uint8_t *)cb->user_buffer + cb->buffer_offset + offset;
   } else if (cb->buffer && cb->buffer->target == PIPE_BUFFER &&
              cb->buffer->width0 >= sizeof(*value) &&
              cb->buffer_offset + offset <=
                 cb->buffer->width0 - sizeof(*value)) {
      map = chrome9_resource(cb->buffer)->bo.map;
      if (!map)
         return false;
      map += cb->buffer_offset + offset;
   } else {
      return false;
   }
   memcpy(value, map, sizeof(*value));
   return true;
}

static bool
chrome9_draw_state_supported(struct chrome9_context *ctx,
                             struct chrome9_resource **target,
                             struct chrome9_resource **depth,
                             struct chrome9_resource *textures[2],
                             struct chrome9_sampler_state *samplers[2],
                             unsigned texture_count, bool *depth_active,
                             struct drm_chrome9_blend_state *blend,
                             bool *blend_successor, const char **reason)
{
   const struct pipe_surface *surface;
   struct pipe_resource *resource;

   *depth = NULL;
   memset(textures, 0, 2 * sizeof(*textures));
   memset(samplers, 0, 2 * sizeof(*samplers));
   *depth_active = false;
   *reason = "context/target";
   if (!ctx->vs || !ctx->fs || !ctx->fs->supported ||
       !(*target = chrome9_context_render_target(ctx)))
      return false;
   resource = &(*target)->base;
   surface = ctx->framebuffer.nr_cbufs == 1 ?
             &ctx->framebuffer.cbufs[0] : NULL;
   *reason = "color-surface";
   if (!surface || !surface->texture || surface->format != resource->format ||
       surface->level || surface->first_layer || surface->last_layer ||
       (resource->target != PIPE_TEXTURE_2D &&
        resource->target != PIPE_TEXTURE_RECT) ||
       (resource->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        resource->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
        !ctx->blend_bgra8888) ||
       !(*target)->guarded || !(*target)->bo.handle)
      return false;

   *reason = "blend-state";
   if (!chrome9_translate_blend(ctx->blend, blend,
                                ctx->blend_extended_factors,
                                blend_successor))
      return false;
   if (ctx->dsa && ctx->dsa->alpha_enabled) {
      *reason = "alpha-test";
      if (!ctx->alpha_test_supported ||
          ctx->dsa->alpha_func > PIPE_FUNC_ALWAYS)
         return false;
      blend->flags |= DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE;
      blend->alpha_test_func = ctx->dsa->alpha_func;
      blend->alpha_test_ref = float_to_ubyte(ctx->dsa->alpha_ref_value);
      *blend_successor = true;
   }
   if (ctx->fs->fragment_program == CHROME9_FS_IOQUAKE3_GENERIC_PACKED ||
       ctx->fs->fragment_program == CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED) {
      uint32_t mode;

      *reason = "shader-alpha-test";
      if ((ctx->dsa && ctx->dsa->alpha_enabled) ||
          !chrome9_fragment_uniform_u32(ctx, 0, &mode) || mode > 3)
         return false;
      if (mode) {
         if (!ctx->alpha_test_supported)
            return false;
         blend->flags |= DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE;
         blend->alpha_test_func = mode == 1 ? PIPE_FUNC_GREATER :
                                  mode == 2 ? PIPE_FUNC_LESS :
                                              PIPE_FUNC_GEQUAL;
         blend->alpha_test_ref = mode == 1 ? 0 : 128;
         *blend_successor = true;
      }
   }
   if (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM)
      *blend_successor = true;
   *reason = "stencil/depth-bounds";
   if (ctx->dsa && (ctx->dsa->stencil[0].enabled ||
                    ctx->dsa->stencil[1].enabled ||
                    ctx->dsa->depth_bounds_test))
      return false;
   if (ctx->dsa && ctx->dsa->depth_enabled) {
      const struct pipe_surface *surface = &ctx->framebuffer.zsbuf;

      *reason = "depth-state";
      *depth = chrome9_context_depth_target(ctx);
      if (!ctx->draw_triangle_d16 ||
          (!ctx->dsa->depth_writemask &&
           !ctx->depth_read_only_supported) ||
          (ctx->dsa->depth_func != PIPE_FUNC_LESS &&
           !(ctx->depth_func_supported &&
             ctx->dsa->depth_func == PIPE_FUNC_LEQUAL) &&
           !(ctx->depth_equal_supported &&
             ctx->dsa->depth_func == PIPE_FUNC_EQUAL) &&
           !(ctx->depth_always_supported &&
             ctx->dsa->depth_func == PIPE_FUNC_ALWAYS)) || !*depth ||
          surface->format != PIPE_FORMAT_Z16_UNORM || surface->level ||
          surface->first_layer || surface->last_layer ||
          ((*depth)->base.target != PIPE_TEXTURE_2D &&
           (*depth)->base.target != PIPE_TEXTURE_RECT) ||
          (*depth)->base.format != PIPE_FORMAT_Z16_UNORM ||
          !(*depth)->guarded || !(*depth)->bo.handle ||
          (*depth)->base.width0 != resource->width0 ||
          (*depth)->base.height0 != resource->height0)
         return false;
      *depth_active = true;
   }
   *reason = "compiled-program-depth";
   if (ctx->fs->fragment_program == CHROME9_FS_COMPILED_ALU &&
       (!*depth_active || !ctx->draw_program_triangle_d16_rhw_batch ||
        !ctx->fs->native_fs_valid || !ctx->fs->native_fs_from_nir))
      return false;
   *reason = "two-texture-successor";
   if (texture_count == 2) {
      const bool depth_blend = *depth_active && *blend_successor;

      if (!ctx->draw_two_textured_triangle ||
          (*blend_successor && !*depth_active) ||
          (*depth_active &&
           !(depth_blend ? ctx->draw_two_textured_triangle_d16_blend :
                           ctx->draw_two_textured_triangle_d16)) ||
          (*depth_active && !depth_blend &&
           resource->format != PIPE_FORMAT_B8G8R8X8_UNORM))
         return false;
   }
   if (texture_count && *depth_active && *blend_successor) {
      *reason = "texture-depth-target";
      if (texture_count == 2) {
         if (!ctx->draw_two_textured_triangle_d16_blend)
            return false;
      } else if (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
                 !ctx->textured_d16_bgra8888) {
         return false;
      }
      if (texture_count == 1 && !ctx->draw_textured_triangle_d16_blend) {
         if (blend->color_mask != (DRM_CHROME9_COLOR_MASK_R |
                                   DRM_CHROME9_COLOR_MASK_G |
                                   DRM_CHROME9_COLOR_MASK_B |
                                   DRM_CHROME9_COLOR_MASK_A) ||
             (blend->flags &&
              (!ctx->textured_d16_source_over ||
               !chrome9_blend_is_source_over(blend))))
            return false;
         /* The predecessor carries only one exact blend equation. */
         *blend_successor = false;
      }
   }
   *reason = "successor-capability";
   if (*blend_successor &&
       (*depth_active ?
        (texture_count == 2 ?
           !ctx->draw_two_textured_triangle_d16_blend :
         texture_count ? !ctx->draw_textured_triangle_d16_blend :
                         !ctx->draw_triangle_d16_blend) :
        !(texture_count ? ctx->draw_textured_triangle_blend :
                     ctx->draw_triangle_blend)))
      return false;
   if (texture_count) {
      *reason = "texture-count/depth";
      if (!(texture_count == 2 ?
             (*depth_active ?
                (*blend_successor ?
                   ctx->draw_two_textured_triangle_d16_blend :
                   ctx->draw_two_textured_triangle_d16) :
                              ctx->draw_two_textured_triangle) :
             (*depth_active ?
                (*blend_successor ?
                   ctx->draw_textured_triangle_d16_blend :
                   ctx->draw_textured_triangle_d16) :
                              ctx->draw_textured_triangle)) ||
          /* Gallium may retain an unused trailing sampler after unbinding
           * the corresponding view.  Only active view units are encoded. */
          ctx->num_sampler_states < texture_count ||
          ctx->num_sampler_views != texture_count)
         return false;
      for (unsigned unit = 0; unit < texture_count; unit++) {
         struct pipe_sampler_view *view = ctx->sampler_views[unit];
         struct pipe_resource *original = view ? view->texture : NULL;
         struct pipe_resource *source = view ?
            chrome9_sampler_view_resource(view) : NULL;

         samplers[unit] = ctx->sampler_states[unit];
         *reason = "sampler-state";
         if (!samplers[unit] || !samplers[unit]->supported || !source)
            return false;
         textures[unit] = chrome9_resource(source);
         *reason = "texture-resource";
         if (original == resource || source == resource ||
             textures[unit]->bo.handle == (*target)->bo.handle ||
             source->target != PIPE_TEXTURE_2D ||
             (source->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
              source->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
             view->u.tex.first_level ||
             view->u.tex.last_level > original->last_level ||
             !textures[unit]->guarded ||
             !textures[unit]->bo.handle ||
             (textures[unit]->data_offset & 0xff) ||
             textures[unit]->stride != ALIGN_POT(source->width0 * 4, 32) ||
             textures[unit]->data_size <
                (uint64_t)textures[unit]->stride * source->height0)
            return false;
         *reason = "rgbx-source-format";
         if (!unit && ctx->fs->fragment_program == CHROME9_FS_TEXTURE_RGBX &&
             source->format != PIPE_FORMAT_B8G8R8X8_UNORM)
            return false;
         *reason = "projected-source-format";
         if (!unit &&
             (ctx->fs->fragment_program ==
                 CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA ||
              ctx->fs->fragment_program ==
                 CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA) &&
             source->format != PIPE_FORMAT_B8G8R8X8_UNORM)
            return false;
         *reason = "two-projected-source-format";
         if (ctx->fs->fragment_program ==
                CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA &&
             source->format != PIPE_FORMAT_B8G8R8X8_UNORM)
            return false;
         if (ctx->fs->fragment_program ==
                CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY &&
             (source->format != (unit ? PIPE_FORMAT_B8G8R8X8_UNORM :
                                       PIPE_FORMAT_B8G8R8A8_UNORM) ||
              !ctx->two_texture_tex0_alpha_primary))
            return false;
         *reason = "packed-lightmap-source-format";
         if (ctx->fs->fragment_program ==
                CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED && unit == 1 &&
             source->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
             !ctx->two_texture_tex0_alpha_primary)
            return false;
      }
      *reason = "texture-alias";
      if (texture_count == 2 &&
          textures[0]->bo.handle == textures[1]->bo.handle)
         return false;
   }
   *reason = "scissor";
   if (ctx->rasterizer && ctx->rasterizer->scissor &&
       (!ctx->scissor_supported || !ctx->scissor_valid ||
        ctx->scissor.minx > ctx->scissor.maxx ||
        ctx->scissor.miny > ctx->scissor.maxy))
      return false;
   *reason = NULL;
   return true;
}

static void
chrome9_u32_as_float(uint32_t bits, float *value)
{
   memcpy(value, &bits, sizeof(*value));
}

static bool
chrome9_q16_integer(int32_t value, uint32_t *integer)
{
   int64_t rounded;
   int64_t error;

   if (value < 0)
      return false;
   rounded = ((int64_t)value + 0x8000) >> 16;
   error = (int64_t)value - (rounded << 16);
   if (llabs(error) > 4 || rounded > UINT32_MAX)
      return false;
   *integer = rounded;
   return true;
}

static bool
chrome9_textured_batch_copy_rect(const struct chrome9_vbuf_render *crender,
                                 struct drm_chrome9_copy_rect *copy)
{
   const struct drm_chrome9_draw_textured_triangle_batch *state =
      &crender->batch_textured_state;
   const uint32_t full_mask = DRM_CHROME9_COLOR_MASK_R |
                              DRM_CHROME9_COLOR_MASK_G |
                              DRM_CHROME9_COLOR_MASK_B |
                              DRM_CHROME9_COLOR_MASK_A;
   uint32_t dx[6], dy[6], sx[6], sy[6];
   uint32_t min_x = UINT32_MAX, min_y = UINT32_MAX;
   uint32_t max_x = 0, max_y = 0;
   int delta_x = 0, delta_y = 0;
   unsigned corners = 0;
   unsigned corner_count[4] = { 0 };
   uint32_t rhw_bits = 0;

   if (crender->ctx->debug_draws) {
      fprintf(stderr,
              "chrome9: copy candidate count=%u program=%u flags=%#x "
              "formats=%u/%u blend=%#x factors=%u/%u/%u/%u mask=%#x "
              "scissor=%#x\n",
              crender->batch_count, state->program, state->flags,
              state->destination_format, state->texture_format,
              state->blend.flags, state->blend.rgb_src_factor,
              state->blend.rgb_dst_factor, state->blend.alpha_src_factor,
              state->blend.alpha_dst_factor, state->blend.color_mask,
              state->scissor.flags);
      for (unsigned i = 0; i < crender->batch_count * 3; i++)
         fprintf(stderr,
                 "chrome9: copy vertex[%u]=%d,%d st=%08x,%08x\n", i,
                 crender->batch_textured_vertices[i].x_16_16,
                 crender->batch_textured_vertices[i].y_16_16,
                 crender->batch_textured_vertices[i].s_float,
                 crender->batch_textured_vertices[i].t_float);
   }
   if (!crender->ctx->copy_rect || crender->batch_count != 2 ||
       state->program != DRM_CHROME9_TEXTURE_PROGRAM_REPLACE ||
       state->min_filter != DRM_CHROME9_TEXTURE_FILTER_NEAREST ||
       state->mag_filter != DRM_CHROME9_TEXTURE_FILTER_NEAREST ||
       !((state->destination_format == state->texture_format) ||
         (state->destination_format ==
             DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 &&
          state->texture_format == DRM_CHROME9_TEXTURE_FORMAT_BGRX8888)) ||
       state->flags & ~DRM_CHROME9_TRIANGLE_BATCH_BLEND)
      return false;
   if (state->flags & DRM_CHROME9_TRIANGLE_BATCH_BLEND) {
      const struct drm_chrome9_blend_state *blend = &state->blend;

      if (blend->flags ||
          blend->rgb_src_factor != DRM_CHROME9_BLEND_FACTOR_ONE ||
          blend->rgb_dst_factor != DRM_CHROME9_BLEND_FACTOR_ZERO ||
          blend->alpha_src_factor != DRM_CHROME9_BLEND_FACTOR_ONE ||
          blend->alpha_dst_factor != DRM_CHROME9_BLEND_FACTOR_ZERO ||
          blend->rgb_func != DRM_CHROME9_BLEND_FUNC_ADD ||
          blend->alpha_func != DRM_CHROME9_BLEND_FUNC_ADD ||
          blend->color_mask != full_mask ||
          blend->alpha_test_func || blend->alpha_test_ref)
         return false;
   } else if (state->destination_format ||
              memcmp(&state->blend, &(struct drm_chrome9_blend_state){0},
                     sizeof(state->blend))) {
      return false;
   }
   for (unsigned i = 0; i < 6; i++) {
      const struct drm_chrome9_textured_vertex *vertex =
         &crender->batch_textured_vertices[i];
      float s, t;
      float rhw;
      float source_x, source_y;
      long rounded_x, rounded_y;

      if (!chrome9_q16_integer(vertex->x_16_16, &dx[i]) ||
          !chrome9_q16_integer(vertex->y_16_16, &dy[i]))
         return false;
      chrome9_u32_as_float(vertex->rhw_float, &rhw);
      if (!isfinite(rhw) || rhw <= 0.0f ||
          (i && vertex->rhw_float != rhw_bits))
         return false;
      rhw_bits = vertex->rhw_float;
      chrome9_u32_as_float(vertex->s_float, &s);
      chrome9_u32_as_float(vertex->t_float, &t);
      if (!isfinite(s) || !isfinite(t))
         return false;
      source_x = s * state->texture_width;
      source_y = t * state->texture_height;
      rounded_x = lroundf(source_x);
      rounded_y = lroundf(source_y);
      if (rounded_x < 0 || rounded_y < 0 ||
          rounded_x > state->texture_width ||
          rounded_y > state->texture_height ||
          fabsf(source_x - rounded_x) > 0.001f ||
          fabsf(source_y - rounded_y) > 0.001f)
         return false;
      sx[i] = rounded_x;
      sy[i] = rounded_y;
      min_x = MIN2(min_x, dx[i]);
      min_y = MIN2(min_y, dy[i]);
      max_x = MAX2(max_x, dx[i]);
      max_y = MAX2(max_y, dy[i]);
   }
   if (min_x >= max_x || min_y >= max_y)
      return false;
   for (unsigned i = 0; i < 6; i++) {
      unsigned corner;
      int current_delta_x;
      int current_delta_y;

      if ((dx[i] != min_x && dx[i] != max_x) ||
          (dy[i] != min_y && dy[i] != max_y))
         return false;
      corner = (dx[i] == max_x) | ((dy[i] == max_y) << 1);
      corners |= 1u << corner;
      corner_count[corner]++;
      current_delta_x = (int)sx[i] - (int)dx[i];
      current_delta_y = (int)sy[i] - (int)dy[i];
      if (i) {
         if (current_delta_x != delta_x || current_delta_y != delta_y)
            return false;
      } else {
         delta_x = current_delta_x;
         delta_y = current_delta_y;
      }
   }
   if (corners != 0xf ||
       !((corner_count[0] == 2 && corner_count[3] == 2 &&
          corner_count[1] == 1 && corner_count[2] == 1) ||
         (corner_count[1] == 2 && corner_count[2] == 2 &&
          corner_count[0] == 1 && corner_count[3] == 1)) ||
       delta_x < -(int)min_x ||
       delta_y < -(int)min_y)
      return false;
   if ((state->scissor.flags & DRM_CHROME9_SCISSOR_ENABLE) &&
       (state->scissor.min_x > min_x || state->scissor.min_y > min_y ||
        state->scissor.max_x < max_x || state->scissor.max_y < max_y))
      return false;
   copy->destination_handle = state->destination_handle;
   copy->source_handle = state->texture_handle;
   copy->destination_x = min_x;
   copy->destination_y = min_y;
   copy->source_x = min_x + delta_x;
   copy->source_y = min_y + delta_y;
   copy->width = max_x - min_x;
   copy->height = max_y - min_y;
   copy->destination_width = state->width;
   copy->destination_height = state->height;
   copy->destination_pitch = state->destination_pitch;
   copy->source_width = state->texture_width;
   copy->source_height = state->texture_height;
   copy->source_pitch = state->texture_pitch;
   copy->format = state->texture_format;
   if (state->destination_format ==
          DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 &&
       state->texture_format == DRM_CHROME9_TEXTURE_FORMAT_BGRX8888)
      copy->flags = DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE;
   return true;
}

static void
chrome9_flush_triangle_batch(struct chrome9_vbuf_render *crender)
{
   struct chrome9_context *ctx = crender->ctx;
   const unsigned count = crender->batch_count;

   if (!count)
      return;
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   bool submitted;
   bool direct_external = false;

   if (crender->batch_textured) {
      struct drm_chrome9_copy_rect copy = { 0 };

      if (chrome9_textured_batch_copy_rect(crender, &copy)) {
         struct drm_chrome9_copy_rect shadow_copy = copy;
         struct pipe_resource *external = ctx->framebuffer.nr_cbufs ?
            ctx->framebuffer.cbufs[0].texture : NULL;
         struct chrome9_resource *external_resource = external ?
            chrome9_resource(external) : NULL;

         submitted = false;
         if (ctx->copy_rect_unguarded_destination && external_resource &&
             external != ctx->render_target && !external_resource->guarded &&
             (external->bind & PIPE_BIND_SCANOUT) &&
             external->width0 == copy.destination_width &&
             external->height0 == copy.destination_height &&
             external_resource->data_offset == 0) {
            copy.destination_handle = external_resource->bo.handle;
            copy.destination_pitch = external_resource->stride;
            copy.flags |= DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED;
            submitted = chrome9_copy_rect(fd, &copy);
            direct_external = submitted;
         }
         if (!submitted) {
            copy = shadow_copy;
            chrome9_context_prepare_render_target(ctx);
            submitted = chrome9_copy_rect(fd, &copy);
         }
         if (!submitted && (errno == EINVAL || errno == EOPNOTSUPP)) {
            crender->batch_textured_state.triangle_count = count;
            crender->batch_textured_state.vertices_ptr =
               (uintptr_t)crender->batch_textured_vertices;
            submitted = chrome9_draw_textured_triangle_batch(
               fd, &crender->batch_textured_state);
         }
      } else {
         chrome9_context_prepare_render_target(ctx);
         crender->batch_textured_state.triangle_count = count;
         crender->batch_textured_state.vertices_ptr =
            (uintptr_t)crender->batch_textured_vertices;
         submitted = chrome9_draw_textured_triangle_batch(
            fd, &crender->batch_textured_state);
      }
   } else if (crender->batch_compiled_program) {
      chrome9_context_prepare_render_target(ctx);
      crender->batch_program.triangle_count = count;
      crender->batch_program.vertices_ptr =
         (uintptr_t)crender->batch_vertices_rhw;
      submitted = chrome9_draw_program_triangle_d16_rhw_batch(
         fd, &crender->batch_program);
   } else if (crender->batch_large_run) {
      chrome9_context_prepare_render_target(ctx);
      crender->batch_rhw_run.triangle_count = count;
      crender->batch_rhw_run.vertices_ptr =
         (uintptr_t)crender->batch_vertices_rhw;
      submitted = chrome9_draw_triangle_d16_rhw_run(
         fd, &crender->batch_rhw_run);
   } else if (crender->batch_perspective) {
      chrome9_context_prepare_render_target(ctx);
      crender->batch_rhw.triangle_count = count;
      crender->batch_rhw.vertices_ptr =
         (uintptr_t)crender->batch_vertices_rhw;
      submitted = chrome9_draw_triangle_d16_rhw_batch(fd,
                                                       &crender->batch_rhw);
   } else {
      chrome9_context_prepare_render_target(ctx);
      crender->batch.triangle_count = count;
      crender->batch.vertices_ptr = (uintptr_t)crender->batch_vertices;
      submitted = chrome9_draw_triangle_batch(fd, &crender->batch);
   }
   if (submitted) {
      ctx->submitted_triangles += count;
      ctx->submitted_batches++;
      if (crender->batch_compiled_program)
         ctx->submitted_program_batches++;
      if (crender->batch_large_run)
         ctx->submitted_rhw_runs++;
      if (direct_external)
         chrome9_context_render_target_direct_written(ctx);
      else
         chrome9_context_render_target_written(ctx);
      crender->batch_failed = false;
   } else {
      ctx->rejected_triangles += count;
      crender->batch_failed = true;
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr,
                 "chrome9: DRAW_%sTRIANGLE%s_BATCH failed after=%" PRIu64
                 " count=%u: %s\n",
                 crender->batch_textured ? "TEXTURED_" :
                 crender->batch_compiled_program ? "PROGRAM_" :
                 crender->batch_large_run ? "RHW_RUN_" : "",
                 crender->batch_perspective ? "_D16_RHW" : "",
                 ctx->submitted_triangles, count, strerror(errno));
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
   }
   crender->batch_count = 0;
}

static void
chrome9_emit_triangle(struct chrome9_vbuf_render *crender,
                      unsigned ia, unsigned ib, unsigned ic)
{
   struct chrome9_context *ctx = crender->ctx;
   struct chrome9_resource *target = NULL;
   struct chrome9_resource *depth = NULL;
   struct chrome9_resource *textures[2];
   struct chrome9_sampler_state *samplers[2];
   struct drm_chrome9_draw_triangle args = { 0 };
   struct drm_chrome9_draw_triangle_blend blend_args = { 0 };
   struct drm_chrome9_draw_triangle_d16 depth_args = { 0 };
   struct drm_chrome9_draw_triangle_d16_blend depth_blend_args = { 0 };
   struct drm_chrome9_draw_textured_triangle texture_args = { 0 };
   struct drm_chrome9_draw_textured_triangle_d16 texture_depth_args = { 0 };
   struct drm_chrome9_draw_textured_triangle_d16_blend
      texture_depth_blend_args = { 0 };
   struct drm_chrome9_draw_textured_triangle_blend texture_blend_args = { 0 };
   struct drm_chrome9_draw_two_textured_triangle two_texture_args = { 0 };
   struct drm_chrome9_draw_two_textured_triangle_d16
      two_texture_depth_args = { 0 };
   struct drm_chrome9_draw_two_textured_triangle_d16_blend
      two_texture_depth_blend_args = { 0 };
   struct drm_chrome9_blend_state blend_state;
   struct drm_chrome9_scissor_state scissor = { 0 };
   struct drm_chrome9_ps_instruction program_instruction = { 0 };
   uint32_t program_constant_c0[4] = { 0 };
   const unsigned indices[3] = { ia, ib, ic };
   int32_t x[3], y[3];
   uint32_t constant_color = 0;
   const char *state_reason = "vertex-layout";
   bool blend_successor = false;
   bool depth_active = false;
   bool packed_lightmap_tex0_alpha = false;
   bool perspective_batch;
   bool rhw_run_batch = false;
   bool program_batch = false;
   const bool two_projected_primary = ctx->fs &&
      (ctx->fs->fragment_program ==
          CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA ||
       ctx->fs->fragment_program ==
          CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY);
   uint32_t depth_flags = 0;

   if (((!crender->textured &&
         (ctx->draw_triangle_batch ||
          ctx->draw_program_triangle_d16_rhw_batch)) ||
        (crender->textured && ctx->draw_textured_triangle_batch)) &&
       crender->batch_failed) {
      ctx->rejected_triangles++;
      return;
   }
   if (!crender->layout_valid ||
       !chrome9_draw_state_supported(ctx, &target, &depth, textures, samplers,
                                     crender->texture_count, &depth_active,
                                     &blend_state, &blend_successor,
                                     &state_reason)) {
      ctx->rejected_triangles++;
      if (!(ctx->warnings & CHROME9_WARN_DRAW_STATE)) {
         const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
         const struct pipe_rt_blend_state *rt =
            ctx->blend ? &ctx->blend->rt[0] : NULL;

         fprintf(stderr,
                 "chrome9: triangle rejected by bounded draw state: "
                 "reason=%s fs=%u layout=%u tex=%u views=%u samplers=%u "
                 "target=%u depth=%u dsa=%u/%u/%u stencil=%u/%u "
                 "blend=%u/%u/%u/%u/%u/%u/%u mask=%#x "
                 "blend_successor=%u caps=d16:%u d16b:%u tex:%u texb:%u\n",
                 state_reason ? state_reason : "unknown",
                 ctx->fs ? ctx->fs->fragment_program : 0,
                 crender->layout_valid, crender->texture_count,
                 ctx->num_sampler_views, ctx->num_sampler_states,
                 target ? target->base.format : 0,
                 depth ? depth->base.format : 0,
                 dsa ? dsa->depth_enabled : 0,
                 dsa ? dsa->depth_writemask : 0,
                 dsa ? dsa->depth_func : 0,
                 dsa ? dsa->stencil[0].enabled : 0,
                 dsa ? dsa->stencil[1].enabled : 0,
                 rt ? rt->blend_enable : 0,
                 rt ? rt->rgb_func : 0,
                 rt ? rt->rgb_src_factor : 0,
                 rt ? rt->rgb_dst_factor : 0,
                 rt ? rt->alpha_func : 0,
                 rt ? rt->alpha_src_factor : 0,
                 rt ? rt->alpha_dst_factor : 0,
                 rt ? rt->colormask : 0,
                 blend_successor, ctx->draw_triangle_d16,
                 ctx->draw_triangle_d16_blend,
                 ctx->draw_textured_triangle,
                 ctx->draw_textured_triangle_blend);
         ctx->warnings |= CHROME9_WARN_DRAW_STATE;
      }
      return;
   }
   packed_lightmap_tex0_alpha =
      ctx->fs->fragment_program ==
         CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY ||
      (ctx->fs->fragment_program == CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED &&
       textures[1] &&
       textures[1]->base.format == PIPE_FORMAT_B8G8R8A8_UNORM);
   perspective_batch = depth_active &&
      (ctx->draw_triangle_d16_rhw_batch ||
       ctx->draw_triangle_d16_rhw_run);
   if (depth_active) {
      if (ctx->depth_func_supported || ctx->depth_equal_supported ||
          ctx->depth_always_supported)
         depth_flags = DRM_CHROME9_DEPTH_FUNC(ctx->dsa->depth_func);
      if (!ctx->dsa->depth_writemask)
         depth_flags |= DRM_CHROME9_DEPTH_READ_ONLY;
   }
   if (chrome9_fs_uses_constant_color(ctx->fs->fragment_program) &&
       !chrome9_fragment_constant_color(ctx, &constant_color)) {
      ctx->rejected_triangles++;
      return;
   }
   program_batch = perspective_batch &&
      ctx->draw_program_triangle_d16_rhw_batch &&
      (!ctx->draw_triangle_d16_rhw_run ||
       ctx->fs->fragment_program == CHROME9_FS_COMPILED_ALU) &&
      chrome9_program_instruction(ctx->fs, &program_instruction) &&
      (!(ctx->fs->native_fs.constant_mask & 1) ||
       chrome9_fragment_constant_c0(ctx, program_constant_c0));
   rhw_run_batch = perspective_batch &&
      ctx->draw_triangle_d16_rhw_run && !program_batch;
   if (ctx->fs->fragment_program == CHROME9_FS_COMPILED_ALU &&
       !program_batch) {
      ctx->rejected_triangles++;
      return;
   }
   if (ctx->rasterizer && ctx->rasterizer->scissor) {
      scissor.flags = DRM_CHROME9_SCISSOR_ENABLE;
      scissor.min_x = MIN2(ctx->scissor.minx, target->base.width0);
      scissor.max_x = MIN2(ctx->scissor.maxx, target->base.width0);
      scissor.min_y = MIN2(ctx->scissor.miny, target->base.height0);
      scissor.max_y = MIN2(ctx->scissor.maxy, target->base.height0);
   }
   for (unsigned i = 0; i < 3; i++) {
      const uint8_t *vertex;
      const float *position;
      const float *packed = NULL;
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
          (depth_active && !isfinite(position[2])) ||
          (perspective_batch &&
           (!isfinite(position[3]) || position[3] <= 0.0f))) {
         ctx->rejected_triangles++;
         return;
      }
      if (crender->packed_generic) {
         packed = (const float *)(vertex + 4 * sizeof(float));
         if (crender->packed_generic >= 6) {
            const unsigned components = crender->packed_generic == 7 ? 8 : 6;

            for (unsigned c = 0; c < components; c++)
               if (!isfinite(packed[c])) {
                  ctx->rejected_triangles++;
                  return;
               }
            if (crender->packed_generic == 7)
               color = (uint32_t)float_to_ubyte(packed[6]) |
                       (uint32_t)float_to_ubyte(packed[5]) << 8 |
                       (uint32_t)float_to_ubyte(packed[4]) << 16 |
                       (uint32_t)float_to_ubyte(packed[7]) << 24;
            else
               color = (uint32_t)float_to_ubyte(packed[4]) |
                       (uint32_t)float_to_ubyte(packed[3]) << 8 |
                       (uint32_t)float_to_ubyte(packed[2]) << 16 |
                       (uint32_t)float_to_ubyte(packed[5]) << 24;
         } else if (crender->packed_generic == 3 ||
             crender->packed_generic == 5) {
            unsigned components = crender->packed_generic == 3 ? 4 : 3;

            for (unsigned c = 0; c < components; c++)
               if (!isfinite(packed[c])) {
                  ctx->rejected_triangles++;
                  return;
               }
            color = (uint32_t)float_to_ubyte(packed[2]) |
                    (uint32_t)float_to_ubyte(packed[1]) << 8 |
                    (uint32_t)float_to_ubyte(packed[0]) << 16 |
                    (crender->packed_generic == 3 ?
                     (uint32_t)float_to_ubyte(packed[3]) << 24 :
                     0xff000000);
         } else {
            unsigned gray;

            if (!isfinite(packed[0])) {
               ctx->rejected_triangles++;
               return;
            }
            gray = float_to_ubyte(packed[0]);
            color = crender->packed_generic == 4 ?
               gray | 0xff000000 :
               gray | gray << 8 | gray << 16 | 0xff000000;
         }
      } else {
         memcpy(&color, vertex + 4 * sizeof(float), sizeof(color));
      }
      if (chrome9_fs_uses_constant_color(ctx->fs->fragment_program))
         color = constant_color;
      if (ctx->fs->fragment_program ==
             CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA &&
          color != 0xffffffff) {
         ctx->rejected_triangles++;
         return;
      }
      x[i] = chrome9_coord_16_16(position[0], target->base.width0);
      y[i] = chrome9_coord_16_16(position[1], target->base.height0);
      if (crender->textured) {
         const float *texcoord = crender->packed_generic == 2 ? packed + 1 :
            crender->packed_generic >= 6 ? packed :
            (const float *)(vertex + 6 * sizeof(uint32_t));
         uint32_t texcoord_q = 0;

         if (ctx->fs->fragment_program ==
                CHROME9_FS_TEXTURE_PROJECTED_MODULATE ||
             ctx->fs->fragment_program ==
                CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
             ctx->fs->fragment_program ==
                CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA ||
             ctx->fs->fragment_program ==
                CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA ||
             two_projected_primary)
            memcpy(&texcoord_q, &texcoord[3], sizeof(texcoord_q));

         if (!isfinite(position[2]) || !isfinite(position[3]) ||
             position[3] <= 0.0f || !isfinite(texcoord[0]) ||
             !isfinite(texcoord[1]) ||
             ((ctx->fs->fragment_program ==
                  CHROME9_FS_TEXTURE_PROJECTED_MODULATE ||
               ctx->fs->fragment_program ==
                  CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
               ctx->fs->fragment_program ==
                  CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA ||
               ctx->fs->fragment_program ==
                  CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA ||
               two_projected_primary) &&
              texcoord_q != 0x3f800000) ||
             (crender->texture_count == 2 &&
              (!chrome9_two_texture_coord_valid(ctx, samplers[0], true,
                                                texcoord[0]) ||
               !chrome9_two_texture_coord_valid(ctx, samplers[0], false,
                                                texcoord[1])))) {
            ctx->rejected_triangles++;
            return;
         }
         texture_args.vertices[i].x_16_16 = x[i];
         texture_args.vertices[i].y_16_16 = y[i];
         texture_args.vertices[i].z_16_16 =
            chrome9_depth_16_16(position[2]);
         memcpy(&texture_args.vertices[i].rhw_float, &position[3],
                sizeof(position[3]));
         texture_args.vertices[i].color = color;
         if (crender->packed_generic == 2 ||
             crender->packed_generic >= 6) {
            texture_args.vertices[i].specular = 0;
         } else {
            memcpy(&texture_args.vertices[i].specular,
                   vertex + 5 * sizeof(uint32_t), sizeof(uint32_t));
         }
         memcpy(&texture_args.vertices[i].s_float, &texcoord[0],
                sizeof(texcoord[0]));
         memcpy(&texture_args.vertices[i].t_float, &texcoord[1],
                sizeof(texcoord[1]));
         texture_blend_args.vertices[i] = texture_args.vertices[i];
         texture_depth_args.vertices[i] = texture_args.vertices[i];
         texture_depth_blend_args.vertices[i] = texture_args.vertices[i];
         if (crender->texture_count == 2) {
            struct drm_chrome9_two_textured_vertex *two_vertex =
               &two_texture_args.vertices[i];
            const float *texcoord1 = crender->packed_generic == 7 ?
               texcoord + 2 : texcoord + 4;
            uint32_t texcoord1_q = 0;

            if (two_projected_primary)
               memcpy(&texcoord1_q, &texcoord1[3], sizeof(texcoord1_q));

            if (!chrome9_two_texture_coord_valid(ctx, samplers[1], true,
                                                 texcoord1[0]) ||
                !chrome9_two_texture_coord_valid(ctx, samplers[1], false,
                                                 texcoord1[1]) ||
                (two_projected_primary && texcoord1_q != 0x3f800000)) {
               ctx->rejected_triangles++;
               return;
            }
            two_vertex->x_16_16 = texture_args.vertices[i].x_16_16;
            two_vertex->y_16_16 = texture_args.vertices[i].y_16_16;
            two_vertex->z_16_16 = texture_args.vertices[i].z_16_16;
            two_vertex->rhw_float = texture_args.vertices[i].rhw_float;
            two_vertex->color = texture_args.vertices[i].color;
            two_vertex->specular = texture_args.vertices[i].specular;
            two_vertex->s0_float = texture_args.vertices[i].s_float;
            two_vertex->t0_float = texture_args.vertices[i].t_float;
            memcpy(&two_vertex->s1_float, &texcoord1[0], sizeof(float));
            memcpy(&two_vertex->t1_float, &texcoord1[1], sizeof(float));
            two_texture_depth_args.vertices[i] = *two_vertex;
            two_texture_depth_blend_args.vertices[i] = *two_vertex;
         }
      } else if (depth_active) {
         depth_args.vertices[i].x_16_16 = x[i];
         depth_args.vertices[i].y_16_16 = y[i];
         depth_args.vertices[i].z_16_16 = chrome9_depth_16_16(position[2]);
         depth_args.vertices[i].color = color;
         depth_blend_args.vertices[i] = depth_args.vertices[i];
      } else {
         args.vertices[i].x_16_16 = x[i];
         args.vertices[i].y_16_16 = y[i];
         args.vertices[i].color = color;
         blend_args.vertices[i] = args.vertices[i];
      }
   }

   /* Clipping and fixed-point viewport conversion can collapse otherwise
    * valid primitives.  Do not submit zero-area immediate triangles. */
   if (!chrome9_triangle_nondegenerate(x, y))
      return;

   if (ctx->draw_limit &&
       ctx->submitted_triangles + crender->batch_count >= ctx->draw_limit) {
      if (!ctx->draw_limit_reported) {
         fprintf(stderr, "chrome9: stopped before triangle=%" PRIu64
                         " by CHROME9_DRAW_LIMIT=%" PRIu64 "\n",
                 ctx->submitted_triangles, ctx->draw_limit);
         ctx->draw_limit_reported = true;
      }
      return;
   }

   if (ctx->debug_draws) {
      fprintf(stderr,
              "chrome9: submit triangle=%" PRIu64 " kind=%s%s%s "
              "xy=(%d,%d)(%d,%d)(%d,%d)\n",
              ctx->submitted_triangles + crender->batch_count,
              crender->textured ? "textured" : "color",
              depth_active ? "+d16" : "",
              blend_successor ? "+blend" : "",
              x[0], y[0], x[1], y[1], x[2], y[2]);
   }

   if (crender->texture_count == 2) {
      two_texture_args.destination_handle = target->bo.handle;
      two_texture_args.flags = packed_lightmap_tex0_alpha ?
         DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY : 0;
      two_texture_args.width = target->base.width0;
      two_texture_args.height = target->base.height0;
      two_texture_args.destination_pitch = target->stride;
      two_texture_args.scissor = scissor;
      for (unsigned unit = 0; unit < 2; unit++) {
         struct drm_chrome9_texture_state *state =
            &two_texture_args.textures[unit];

         state->handle = textures[unit]->bo.handle;
         state->width = textures[unit]->base.width0;
         state->height = textures[unit]->base.height0;
         state->pitch = textures[unit]->stride;
         state->format = textures[unit]->base.format ==
                         PIPE_FORMAT_B8G8R8X8_UNORM ?
                         DRM_CHROME9_TEXTURE_FORMAT_BGRX8888 :
                         DRM_CHROME9_TEXTURE_FORMAT_BGRA8888;
         state->min_filter = samplers[unit]->min_filter;
         state->mag_filter = samplers[unit]->mag_filter;
         state->wrap_s = samplers[unit]->wrap_s;
         state->wrap_t = samplers[unit]->wrap_t;
         two_texture_depth_args.textures[unit] = *state;
         two_texture_depth_blend_args.textures[unit] = *state;
      }
      if (depth_active) {
         two_texture_depth_args.destination_handle =
            two_texture_args.destination_handle;
         two_texture_depth_args.depth_handle = depth->bo.handle;
         two_texture_depth_args.flags = depth_flags |
            two_texture_args.flags;
         two_texture_depth_args.width = two_texture_args.width;
         two_texture_depth_args.height = two_texture_args.height;
         two_texture_depth_args.destination_pitch =
            two_texture_args.destination_pitch;
         two_texture_depth_args.depth_pitch = depth->stride;
         two_texture_depth_args.scissor = two_texture_args.scissor;
         if (blend_successor) {
            two_texture_depth_blend_args.destination_handle =
               two_texture_args.destination_handle;
            two_texture_depth_blend_args.depth_handle = depth->bo.handle;
            two_texture_depth_blend_args.flags = depth_flags |
               two_texture_args.flags;
            two_texture_depth_blend_args.width = two_texture_args.width;
            two_texture_depth_blend_args.height = two_texture_args.height;
            two_texture_depth_blend_args.destination_pitch =
               two_texture_args.destination_pitch;
            two_texture_depth_blend_args.depth_pitch = depth->stride;
            two_texture_depth_blend_args.blend = blend_state;
            two_texture_depth_blend_args.scissor = two_texture_args.scissor;
            two_texture_depth_blend_args.destination_format =
               target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
               DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
               DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
         }
      }
   } else if (crender->textured) {
      texture_args.destination_handle = target->bo.handle;
      texture_args.texture_handle = textures[0]->bo.handle;
      texture_args.program =
         (ctx->fs->fragment_program == CHROME9_FS_TEXTURE_REPLACE ||
          ctx->fs->fragment_program == CHROME9_FS_TEXTURE_RGBX) ?
                                DRM_CHROME9_TEXTURE_PROGRAM_REPLACE :
                                DRM_CHROME9_TEXTURE_PROGRAM_MODULATE;
      texture_args.width = target->base.width0;
      texture_args.height = target->base.height0;
      texture_args.destination_pitch = target->stride;
      texture_args.texture_width = textures[0]->base.width0;
      texture_args.texture_height = textures[0]->base.height0;
      texture_args.texture_pitch = textures[0]->stride;
      texture_args.texture_format = textures[0]->base.format ==
                                      PIPE_FORMAT_B8G8R8X8_UNORM ?
                                      DRM_CHROME9_TEXTURE_FORMAT_BGRX8888 :
                                      DRM_CHROME9_TEXTURE_FORMAT_BGRA8888;
      texture_args.scissor = scissor;
      texture_args.min_filter = samplers[0]->min_filter;
      texture_args.mag_filter = samplers[0]->mag_filter;
      texture_args.wrap_s = samplers[0]->wrap_s;
      texture_args.wrap_t = samplers[0]->wrap_t;
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
         texture_blend_args.scissor = scissor;
         texture_blend_args.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
      }
      if (depth_active) {
         texture_depth_args.destination_handle =
            texture_args.destination_handle;
         texture_depth_args.texture_handle = texture_args.texture_handle;
         texture_depth_args.depth_handle = depth->bo.handle;
         texture_depth_args.flags = depth_flags;
         if (!blend_successor && chrome9_blend_is_source_over(&blend_state))
            texture_depth_args.flags |=
               DRM_CHROME9_TEXTURED_D16_SOURCE_OVER;
         texture_depth_args.program = texture_args.program;
         texture_depth_args.width = texture_args.width;
         texture_depth_args.height = texture_args.height;
         texture_depth_args.destination_pitch =
            texture_args.destination_pitch;
         texture_depth_args.depth_pitch = depth->stride;
         texture_depth_args.texture_width = texture_args.texture_width;
         texture_depth_args.texture_height = texture_args.texture_height;
         texture_depth_args.texture_pitch = texture_args.texture_pitch;
         texture_depth_args.texture_format = texture_args.texture_format;
         texture_depth_args.min_filter = texture_args.min_filter;
         texture_depth_args.mag_filter = texture_args.mag_filter;
         texture_depth_args.wrap_s = texture_args.wrap_s;
         texture_depth_args.wrap_t = texture_args.wrap_t;
         texture_depth_args.scissor = scissor;
         texture_depth_args.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
         if (blend_successor) {
            texture_depth_blend_args.destination_handle =
               texture_args.destination_handle;
            texture_depth_blend_args.texture_handle =
               texture_args.texture_handle;
            texture_depth_blend_args.depth_handle = depth->bo.handle;
            texture_depth_blend_args.flags = depth_flags;
            texture_depth_blend_args.program = texture_args.program;
            texture_depth_blend_args.width = texture_args.width;
            texture_depth_blend_args.height = texture_args.height;
            texture_depth_blend_args.destination_pitch =
               texture_args.destination_pitch;
            texture_depth_blend_args.depth_pitch = depth->stride;
            texture_depth_blend_args.texture_width =
               texture_args.texture_width;
            texture_depth_blend_args.texture_height =
               texture_args.texture_height;
            texture_depth_blend_args.texture_pitch =
               texture_args.texture_pitch;
            texture_depth_blend_args.texture_format =
               texture_args.texture_format;
            texture_depth_blend_args.min_filter = texture_args.min_filter;
            texture_depth_blend_args.mag_filter = texture_args.mag_filter;
            texture_depth_blend_args.wrap_s = texture_args.wrap_s;
            texture_depth_blend_args.wrap_t = texture_args.wrap_t;
            texture_depth_blend_args.blend = blend_state;
            texture_depth_blend_args.scissor = scissor;
            texture_depth_blend_args.destination_format =
               texture_depth_args.destination_format;
         }
      }
   } else if (depth_active) {
      depth_args.flags = depth_flags;
      depth_args.destination_handle = target->bo.handle;
      depth_args.depth_handle = depth->bo.handle;
      depth_args.width = target->base.width0;
      depth_args.height = target->base.height0;
      depth_args.destination_pitch = target->stride;
      depth_args.depth_pitch = depth->stride;
      depth_args.scissor = scissor;
      if (blend_successor) {
         depth_blend_args.destination_handle =
            depth_args.destination_handle;
         depth_blend_args.depth_handle = depth_args.depth_handle;
         depth_blend_args.width = depth_args.width;
         depth_blend_args.height = depth_args.height;
         depth_blend_args.destination_pitch =
            depth_args.destination_pitch;
         depth_blend_args.depth_pitch = depth_args.depth_pitch;
         depth_blend_args.flags = depth_args.flags;
         depth_blend_args.blend = blend_state;
         depth_blend_args.scissor = scissor;
         depth_blend_args.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
      }
   } else {
      args.destination_handle = target->bo.handle;
      args.width = target->base.width0;
      args.height = target->base.height0;
      args.pitch = target->stride;
      args.scissor = scissor;
      if (blend_successor) {
         blend_args.destination_handle = args.destination_handle;
         blend_args.width = args.width;
         blend_args.height = args.height;
         blend_args.pitch = args.pitch;
         blend_args.blend = blend_state;
         blend_args.scissor = scissor;
         blend_args.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
      }
   }
   if (crender->textured && crender->texture_count == 1 && !depth_active &&
       ctx->draw_textured_triangle_batch) {
      struct drm_chrome9_draw_textured_triangle_batch next = {
         .destination_handle = texture_args.destination_handle,
         .texture_handle = texture_args.texture_handle,
         .program = texture_args.program,
         .width = texture_args.width,
         .height = texture_args.height,
         .destination_pitch = texture_args.destination_pitch,
         .texture_width = texture_args.texture_width,
         .texture_height = texture_args.texture_height,
         .texture_pitch = texture_args.texture_pitch,
         .texture_format = texture_args.texture_format,
         .min_filter = texture_args.min_filter,
         .mag_filter = texture_args.mag_filter,
         .wrap_s = texture_args.wrap_s,
         .wrap_t = texture_args.wrap_t,
         .scissor = texture_args.scissor,
      };

      if (blend_successor) {
         next.flags = DRM_CHROME9_TRIANGLE_BATCH_BLEND;
         next.destination_format = texture_blend_args.destination_format;
         next.blend = texture_blend_args.blend;
      }
      if (crender->batch_count &&
          (!crender->batch_textured ||
           memcmp(&crender->batch_textured_state, &next, sizeof(next))))
         chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         ctx->rejected_triangles++;
         return;
      }
      if (!crender->batch_count) {
         crender->batch_textured = true;
         crender->batch_perspective = false;
         crender->batch_large_run = false;
         crender->batch_compiled_program = false;
         crender->batch_textured_state = next;
      }
      const unsigned base = crender->batch_count * 3;
      for (unsigned i = 0; i < 3; i++)
         crender->batch_textured_vertices[base + i] =
            texture_args.vertices[i];
      crender->batch_count++;
      if (crender->batch_count ==
          DRM_CHROME9_TEXTURED_TRIANGLE_BATCH_MAX_TRIANGLES)
         chrome9_flush_triangle_batch(crender);
      return;
   }
   if (!crender->textured &&
       (ctx->draw_triangle_batch || program_batch || rhw_run_batch)) {
      struct drm_chrome9_draw_triangle_batch next = { 0 };
      struct drm_chrome9_draw_triangle_d16_rhw_batch next_rhw = { 0 };
      struct drm_chrome9_draw_triangle_d16_rhw_run next_rhw_run = { 0 };
      struct drm_chrome9_draw_program_triangle_d16_rhw_batch next_program = {
         0
      };

      if (perspective_batch) {
         next_rhw.destination_handle = target->bo.handle;
         next_rhw.depth_handle = depth->bo.handle;
         next_rhw.flags = DRM_CHROME9_TRIANGLE_BATCH_DEPTH | depth_flags;
         next_rhw.width = target->base.width0;
         next_rhw.height = target->base.height0;
         next_rhw.destination_pitch = target->stride;
         next_rhw.depth_pitch = depth->stride;
         next_rhw.scissor = scissor;
         if (blend_successor) {
            next_rhw.flags |= DRM_CHROME9_TRIANGLE_BATCH_BLEND;
            next_rhw.blend = blend_state;
            next_rhw.destination_format =
               target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
               DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
               DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
         }
         if (program_batch) {
            next_program.destination_handle = next_rhw.destination_handle;
            next_program.depth_handle = next_rhw.depth_handle;
            next_program.flags = next_rhw.flags;
            next_program.width = next_rhw.width;
            next_program.height = next_rhw.height;
            next_program.destination_pitch = next_rhw.destination_pitch;
            next_program.depth_pitch = next_rhw.depth_pitch;
            next_program.destination_format = next_rhw.destination_format;
            next_program.blend = next_rhw.blend;
            next_program.scissor = next_rhw.scissor;
            next_program.instruction = program_instruction;
            memcpy(next_program.constant_c0, program_constant_c0,
                   sizeof(next_program.constant_c0));
         } else if (rhw_run_batch) {
            next_rhw_run.destination_handle = next_rhw.destination_handle;
            next_rhw_run.depth_handle = next_rhw.depth_handle;
            next_rhw_run.flags = next_rhw.flags;
            next_rhw_run.width = next_rhw.width;
            next_rhw_run.height = next_rhw.height;
            next_rhw_run.destination_pitch = next_rhw.destination_pitch;
            next_rhw_run.depth_pitch = next_rhw.depth_pitch;
            next_rhw_run.destination_format = next_rhw.destination_format;
            next_rhw_run.blend = next_rhw.blend;
            next_rhw_run.scissor = next_rhw.scissor;
         }
      } else {
         next.destination_handle = target->bo.handle;
         next.width = target->base.width0;
         next.height = target->base.height0;
         next.destination_pitch = target->stride;
         next.scissor = scissor;
         if (depth_active) {
            next.flags |= DRM_CHROME9_TRIANGLE_BATCH_DEPTH | depth_flags;
            next.depth_handle = depth->bo.handle;
            next.depth_pitch = depth->stride;
         }
         if (blend_successor) {
            next.flags |= DRM_CHROME9_TRIANGLE_BATCH_BLEND;
            next.blend = blend_state;
            next.destination_format =
               target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
               DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
               DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
         }
      }
      if (crender->batch_count &&
          (crender->batch_textured ||
           crender->batch_perspective != perspective_batch ||
           crender->batch_large_run != rhw_run_batch ||
           crender->batch_compiled_program != program_batch ||
           (program_batch ?
              memcmp(&crender->batch_program, &next_program,
                     sizeof(next_program)) : rhw_run_batch ?
              memcmp(&crender->batch_rhw_run, &next_rhw_run,
                     sizeof(next_rhw_run)) : perspective_batch ?
              memcmp(&crender->batch_rhw, &next_rhw, sizeof(next_rhw)) :
              memcmp(&crender->batch, &next, sizeof(next)))))
         chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         ctx->rejected_triangles++;
         return;
      }
      if (!crender->batch_count) {
         crender->batch_textured = false;
         crender->batch_perspective = perspective_batch;
         crender->batch_large_run = rhw_run_batch;
         crender->batch_compiled_program = program_batch;
         if (program_batch)
            crender->batch_program = next_program;
         else if (rhw_run_batch)
            crender->batch_rhw_run = next_rhw_run;
         else if (perspective_batch)
            crender->batch_rhw = next_rhw;
         else
            crender->batch = next;
      }
      const unsigned base = crender->batch_count * 3;
      for (unsigned i = 0; i < 3; i++) {
         if (perspective_batch) {
            struct drm_chrome9_vertex_d16_rhw *dst =
               &crender->batch_vertices_rhw[base + i];
            const uint8_t *vertex = crender->vertices +
               (size_t)indices[i] * crender->vertex_size;
            const float *position = (const float *)vertex;

            dst->x_16_16 = depth_args.vertices[i].x_16_16;
            dst->y_16_16 = depth_args.vertices[i].y_16_16;
            dst->z_16_16 = depth_args.vertices[i].z_16_16;
            memcpy(&dst->rhw_float, &position[3], sizeof(dst->rhw_float));
            dst->color = depth_args.vertices[i].color;
         } else {
            struct drm_chrome9_vertex_d16 *dst =
               &crender->batch_vertices[base + i];

            if (depth_active) {
               *dst = depth_args.vertices[i];
            } else {
               dst->x_16_16 = args.vertices[i].x_16_16;
               dst->y_16_16 = args.vertices[i].y_16_16;
               dst->z_16_16 = 0;
               dst->color = args.vertices[i].color;
            }
         }
      }
      crender->batch_count++;
      if (crender->batch_count ==
          (rhw_run_batch ? DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES :
                           DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES))
         chrome9_flush_triangle_batch(crender);
      return;
   }
   /* Preserve API ordering when a non-batch draw follows retained triangles
    * from earlier Gallium draws. */
   chrome9_flush_triangle_batch(crender);
   if (crender->batch_failed) {
      ctx->rejected_triangles++;
      return;
   }
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   chrome9_context_prepare_render_target(ctx);
   const bool submitted = crender->texture_count == 2 && depth_active &&
                          blend_successor ?
      chrome9_draw_two_textured_triangle_d16_blend(
         fd, &two_texture_depth_blend_args) :
      crender->texture_count == 2 && depth_active ?
      chrome9_draw_two_textured_triangle_d16(fd, &two_texture_depth_args) :
      crender->texture_count == 2 ?
      chrome9_draw_two_textured_triangle(fd, &two_texture_args) :
      crender->textured && depth_active && blend_successor ?
      chrome9_draw_textured_triangle_d16_blend(
         fd, &texture_depth_blend_args) :
      crender->textured && depth_active ?
      chrome9_draw_textured_triangle_d16(fd, &texture_depth_args) :
      crender->textured && blend_successor ?
      chrome9_draw_textured_triangle_blend(fd, &texture_blend_args) :
      crender->textured ?
      chrome9_draw_textured_triangle(fd, &texture_args) :
      depth_active && blend_successor ?
      chrome9_draw_triangle_d16_blend(fd, &depth_blend_args) :
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
                 "chrome9: DRAW_%sTRIANGLE%s failed after=%" PRIu64
                 ": %s target=%ux%u/%u xy=(%d,%d)(%d,%d)(%d,%d)\n",
                 crender->textured ? "TEXTURED_" : "",
                 depth_active ? "_D16" : "", ctx->submitted_triangles,
                 strerror(errno), target->base.width0, target->base.height0,
                 target->stride, x[0], y[0], x[1], y[1], x[2], y[2]);
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

   chrome9_flush_triangle_batch(crender);
   if (crender->ctx->vbuf_render == crender)
      crender->ctx->vbuf_render = NULL;
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
   ctx->vbuf_render = render;
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

void
chrome9_draw_flush_batch(struct chrome9_context *ctx)
{
   if (ctx && ctx->vbuf_render) {
      struct chrome9_vbuf_render *render = ctx->vbuf_render;

      chrome9_flush_triangle_batch(render);
      /* Match the former per-vbuf-draw recovery boundary: one rejected batch
       * does not permanently disable later frames after an explicit flush. */
      render->batch_failed = false;
   }
}
