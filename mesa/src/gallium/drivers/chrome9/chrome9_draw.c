/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_draw.h"
#include "util/u_dynarray.h"
#include "util/u_inlines.h"

#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_format.h"
#include "chrome9_fragment.h"
#include "chrome9_fs_split.h"
#include "chrome9_public.h"
#include "chrome9_query.h"
#include "chrome9_resource.h"
#include "chrome9_swfb.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "draw/draw_context.h"
#include "draw/draw_private.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "util/blend.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

/* Triangles per generic fragment-program submission: the kernel limit. */
#define CHROME9_FRAGMENT_BATCH_TRIANGLES DRM_CHROME9_FRAGMENT_MAX_TRIANGLES
#define CHROME9_DEFERRED_FRAGMENT_MAX_TRIANGLES 4096U

struct chrome9_deferred_fragment_batch {
   struct chrome9_deferred_fragment_batch *next;
   struct drm_chrome9_draw_fragment_program draw;
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
   struct drm_chrome9_fragment_texture_state
      textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   struct drm_chrome9_fragment_vertex
      vertices[CHROME9_FRAGMENT_BATCH_TRIANGLES * 3];
};

struct chrome9_vbuf_render {
   struct vbuf_render base;
   struct chrome9_context *ctx;
   struct vertex_info vinfo;
   enum mesa_prim prim;
   uint8_t *vertices;
   size_t capacity;
   /* Bytes allocated at vertices; kept across batches. */
   size_t allocated;
   uint16_t vertex_size;
   uint16_t vertex_count;
   struct drm_chrome9_draw_triangle_batch batch;
   struct drm_chrome9_vertex_d16
      batch_vertices[DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES * 3];
   struct drm_chrome9_draw_textured_triangle_batch batch_textured_state;
   struct drm_chrome9_textured_vertex
      batch_textured_vertices[
         DRM_CHROME9_TEXTURED_TRIANGLE_BATCH_MAX_TRIANGLES * 3];
   struct drm_chrome9_draw_textured_triangle_run batch_textured_run_state;
   struct drm_chrome9_draw_textured_triangle_mip batch_textured_mip_state;
   struct drm_chrome9_textured_vertex
      batch_textured_run_vertices[DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES * 3];
   struct drm_chrome9_draw_triangle_d16_rhw_batch batch_rhw;
   struct drm_chrome9_draw_triangle_d16_rhw_run batch_rhw_run;
   struct drm_chrome9_draw_triangle_d16_rhw_chain batch_rhw_chain;
   struct drm_chrome9_draw_program_triangle_d16_rhw_batch batch_program;
   struct drm_chrome9_vertex_d16_rhw
      batch_vertices_rhw[DRM_CHROME9_TRIANGLE_CHAIN_MAX_TRIANGLES * 3];
   struct drm_chrome9_draw_fragment_program batch_fragment;
   struct drm_chrome9_fragment_instruction
      batch_fragment_instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   uint32_t batch_fragment_constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
   struct drm_chrome9_fragment_texture_state
      batch_fragment_textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   struct drm_chrome9_fragment_vertex
      batch_fragment_vertices[CHROME9_FRAGMENT_BATCH_TRIANGLES * 3];
   unsigned batch_count;
   /* Generic fragment state of the current vbuf draw call: state cannot
    * change within one call, so its triangles share it. */
   bool generic_state_valid;
   struct chrome9_resource *generic_target;
   struct chrome9_resource *generic_depth;
   bool generic_depth_active;
   struct drm_chrome9_draw_fragment_program generic_draw;
   struct drm_chrome9_fragment_instruction
      generic_instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   uint32_t generic_constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
   struct drm_chrome9_fragment_texture_state
      generic_textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   bool batch_failed;
   bool batch_perspective;
   bool batch_large_run;
   bool batch_large_chain;
   bool batch_compiled_program;
   bool batch_generic_program;
   bool defer_guarded_domain;
   bool deferred_failed;
   unsigned deferred_triangles;
   uint64_t deferred_rejected_before;
   struct chrome9_deferred_fragment_batch *deferred_head;
   struct chrome9_deferred_fragment_batch *deferred_tail;
   bool batch_textured;
   bool batch_textured_run;
   bool batch_textured_mip;
   uint8_t texture_count;
   uint8_t varying_count;
   uint8_t packed_generic;
   bool generic_program;
   bool textured;
   bool layout_valid;
   /* Multi-pass fragment program: triangles with disjoint interiors, run
    * through every job when flushed, and the draw state of the call. */
   const struct chrome9_fs_split *split;
   struct util_dynarray split_vertices;
   unsigned split_count;
   struct chrome9_resource *split_textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   struct chrome9_sampler_state
      *split_samplers[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   bool split_blend_active;
   struct drm_chrome9_blend_state split_blend;
   struct drm_chrome9_scissor_state split_scissor;
   uint32_t split_depth_flags;
   struct drm_chrome9_stencil_state split_stencil;
};

static inline struct chrome9_vbuf_render *
chrome9_vbuf_render(struct vbuf_render *render)
{
   return (struct chrome9_vbuf_render *)render;
}

static bool
chrome9_fragment_uses_sparse_abi(const struct chrome9_context *ctx)
{
   return ctx->fragment_sparse_textures &&
      !chrome9_fragment_sampler_slots_dense(ctx->fs->semantic_sampler_mask);
}

/* ABI v2 packs live descriptors but leaves shader TAU sampler IDs intact. */
static unsigned
chrome9_fragment_texture_unit(const struct chrome9_context *ctx,
                              unsigned descriptor)
{
   return chrome9_fragment_uses_sparse_abi(ctx) ?
      chrome9_fragment_descriptor_unit(ctx->fs->semantic_sampler_mask,
                                       descriptor) : descriptor;
}

bool
chrome9_generic_fragment_program(const struct chrome9_context *ctx)
{
   return ctx->draw_fragment_program && ctx->fs &&
          ctx->fs->semantic_fs_valid &&
          (!ctx->fs->semantic_input_mask ||
           (ctx->fragment_primary && ctx->fs->semantic_input_mask == 1));
}

static bool
chrome9_blend_factor_constant(unsigned factor)
{
   return factor == PIPE_BLENDFACTOR_CONST_COLOR ||
          factor == PIPE_BLENDFACTOR_INV_CONST_COLOR ||
          factor == PIPE_BLENDFACTOR_CONST_ALPHA ||
          factor == PIPE_BLENDFACTOR_INV_CONST_ALPHA;
}

static bool
chrome9_blend_uses_constant(const struct pipe_blend_state *state)
{
   const struct pipe_rt_blend_state *rt = state ? &state->rt[0] : NULL;

   return rt && rt->blend_enable &&
          (chrome9_blend_factor_constant(rt->rgb_src_factor) ||
           chrome9_blend_factor_constant(rt->rgb_dst_factor) ||
           chrome9_blend_factor_constant(rt->alpha_src_factor) ||
           chrome9_blend_factor_constant(rt->alpha_dst_factor));
}

static bool
chrome9_draw_fixed_unaddressable(const struct chrome9_resource *resource)
{
   return chrome9_resource_is_tiled(resource) ||
          chrome9_resource_is_local(resource);
}

/* The fixed draw builders address linear surfaces in GART pages only. */
bool
chrome9_draw_tiled_surfaces(const struct chrome9_context *ctx)
{
   const struct pipe_framebuffer_state *fb = &ctx->framebuffer;

   if ((fb->nr_cbufs && fb->cbufs[0].texture &&
        chrome9_draw_fixed_unaddressable(
           chrome9_resource(fb->cbufs[0].texture))) ||
       (fb->zsbuf.texture &&
        chrome9_draw_fixed_unaddressable(
           chrome9_resource(fb->zsbuf.texture))))
      return true;
   for (unsigned i = 0; i < ctx->num_sampler_views; i++) {
      const struct pipe_sampler_view *view = ctx->sampler_views[i];

      if (view && view->texture &&
          chrome9_draw_fixed_unaddressable(
             chrome9_resource_const(view->texture)))
         return true;
   }
   return false;
}

/* Counting query intervals and the blend colour exist only on this path. */
static bool
chrome9_use_generic_fragment_program(const struct chrome9_context *ctx)
{
   return chrome9_generic_fragment_program(ctx) &&
          (!ctx->fs->semantic_passthrough || chrome9_query_counting(ctx) ||
           chrome9_blend_uses_constant(ctx->blend) ||
           chrome9_draw_tiled_surfaces(ctx));
}

/* A shadow TEX whose comparison the current FS variant computes. */
static bool
chrome9_depth_compare(const struct chrome9_context *ctx, unsigned unit)
{
   return ctx->fs && (ctx->fs->shadow_sampler_mask & (1u << unit));
}

/*
 * A shader-visible 3D view the sampler cannot take sends the draw to
 * llvmpipe: native volumes are POT, level zero, without mip filtering
 * (docs/research/chrome9-queries-msaa-3d-flow-re.md section 4).
 */
bool
chrome9_draw_needs_sw_textures(const struct chrome9_context *ctx)
{
   const bool generic = chrome9_generic_fragment_program(ctx);

   for (unsigned i = 0; i < ctx->num_sampler_views; i++) {
      const struct pipe_sampler_view *view = ctx->sampler_views[i];
      const struct pipe_resource *res = view ? view->texture : NULL;
      const struct chrome9_sampler_state *sampler =
         i < ctx->num_sampler_states ? ctx->sampler_states[i] : NULL;

      if (!res ||
          (generic && !(ctx->fs->semantic_sampler_mask & (1u << i))))
         continue;
      if (chrome9_sampler_view((struct pipe_sampler_view *)view)->sw_only)
         return true;
      /* Sampler states beyond the hardware (fractional LINEAR LOD limits,
       * filtered D24 without a compare) sample in llvmpipe. */
      if ((sampler && !sampler->supported) ||
          (sampler && chrome9_format_is_d24(res->format) &&
           !chrome9_depth_compare(ctx, i) &&
           (sampler->min_filter != DRM_CHROME9_TEXTURE_FILTER_NEAREST ||
            sampler->mag_filter != DRM_CHROME9_TEXTURE_FILTER_NEAREST)))
         return true;
      if (res->target != PIPE_TEXTURE_3D)
         continue;
      if (!ctx->texture_3d || !generic ||
          !(ctx->fs->semantic_volume_sampler_mask & (1u << i)) ||
          !chrome9_texture_format_info(res->format) ||
          !util_is_power_of_two_nonzero(res->width0) ||
          !util_is_power_of_two_nonzero(res->height0) ||
          !util_is_power_of_two_nonzero(res->depth0) ||
          res->depth0 > 2048 || res->last_level ||
          view->u.tex.first_level ||
          !sampler || !sampler->supported || !sampler->wrap_r ||
          (view->u.tex.last_level &&
           sampler->mip_filter != DRM_CHROME9_TEXTURE_MIP_FILTER_NONE) ||
          !chrome9_texture_swizzle_supported(view->format, view->swizzle_r,
                                             view->swizzle_g,
                                             view->swizzle_b,
                                             view->swizzle_a))
         return true;
   }
   return false;
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
chrome9_program_source(const struct chrome9_ps_source *native,
                       struct drm_chrome9_ps_source *source)
{
   memset(source, 0, sizeof(*source));
   if (!native || native->id || native->modifier || native->last_use)
      return false;
   switch (native->file) {
   case CHROME9_PS_SOURCE_INPUT:
      source->file = DRM_CHROME9_PS_SOURCE_VARYING;
      break;
   case CHROME9_PS_SOURCE_CONSTANT:
      source->file = DRM_CHROME9_PS_SOURCE_CONSTANT;
      break;
   default:
      return false;
   }
   for (unsigned c = 0; c < ARRAY_SIZE(source->swizzle); c++) {
      if (native->swizzle[c] != c)
         return false;
      source->swizzle[c] = native->swizzle[c];
   }
   return true;
}

static bool
chrome9_program_instruction(const struct chrome9_shader *shader,
                            bool fragment_add,
                            struct drm_chrome9_ps_instruction *instruction)
{
   const struct chrome9_ps_instruction *native;

   if (!shader || !shader->native_fs_valid || !shader->native_fs_from_nir ||
       shader->semantic_fs_ir.count != 1 ||
       shader->native_fs.alu_count != 1 || shader->native_fs.tau_count ||
       shader->native_fs.sampler_mask ||
       (shader->native_fs.varying_mask & ~1u) ||
       (shader->native_fs.constant_mask & ~1u))
      return false;

   native = &shader->semantic_fs_ir.instruction[0];
   if (native->destination.file != CHROME9_PS_DESTINATION_COLOR ||
       native->destination.id || native->destination.writemask != 0xf ||
       native->destination.saturate)
      return false;
   memset(instruction, 0, sizeof(*instruction));
   switch (native->op) {
   case CHROME9_PS_OPCODE_MOV:
      instruction->op = DRM_CHROME9_PS_OP_MOV;
      return chrome9_program_source(&native->source[0],
                                    &instruction->src[0]);
   case CHROME9_PS_OPCODE_MUL:
      if (native->source[0].file == native->source[1].file)
         return false;
      instruction->op = DRM_CHROME9_PS_OP_MUL;
      return chrome9_program_source(&native->source[0],
                                    &instruction->src[0]) &&
             chrome9_program_source(&native->source[1],
                                    &instruction->src[1]);
   case CHROME9_PS_OPCODE_ADD:
      if (!fragment_add ||
          native->source[0].file == native->source[1].file)
         return false;
      instruction->op = DRM_CHROME9_PS_OP_ADD;
      return chrome9_program_source(&native->source[0],
                                    &instruction->src[0]) &&
             chrome9_program_source(&native->source[1],
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
   int generic[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
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

   crender->generic_program =
      chrome9_use_generic_fragment_program(crender->ctx);
   if (crender->generic_program) {
      uint16_t varying_mask = crender->ctx->fs->semantic_varying_mask;
      bool primary = crender->ctx->fs->semantic_input_mask == 1;

      crender->varying_count = varying_mask ? 32 - __builtin_clz(varying_mask) : 0;
      crender->texture_count = crender->ctx->fragment_sparse_textures ?
         __builtin_popcount(crender->ctx->fs->semantic_sampler_mask) :
         chrome9_fragment_sampler_high_water(
            crender->ctx->fs->semantic_sampler_mask);
      crender->textured = crender->texture_count != 0;
      crender->packed_generic = 0;
      memset(&crender->vinfo, 0, sizeof(crender->vinfo));
      draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, position);
      crender->layout_valid = position >= 0;
      if (primary) {
         draw_emit_vertex_attr(&crender->vinfo, EMIT_4UB_BGRA, color0);
         crender->layout_valid &= color0 >= 0;
      }
      for (unsigned i = 0; i < crender->varying_count; i++) {
         const unsigned slot = crender->ctx->fs->varying_slot[i];

         /* The draw module names TEXn TEXCOORD n and VARn GENERIC n. */
         if (slot >= VARYING_SLOT_VAR0)
            generic[i] = draw_find_shader_output(crender->ctx->draw,
               TGSI_SEMANTIC_GENERIC, slot - VARYING_SLOT_VAR0);
         else if (slot >= VARYING_SLOT_TEX0) {
            const unsigned n = slot - VARYING_SLOT_TEX0;

            /* The wide-point stage adds replaced sprite coordinates as an
             * extra GENERIC n output behind the shader outputs. */
            generic[i] = draw_find_shader_output(crender->ctx->draw,
                                                 TGSI_SEMANTIC_GENERIC, n);
            if (generic[i] >= 0 && (unsigned)generic[i] <
                draw_current_shader_outputs(crender->ctx->draw))
               generic[i] = -1;
            if (generic[i] < 0)
               generic[i] = draw_find_shader_output(crender->ctx->draw,
                                                     TGSI_SEMANTIC_TEXCOORD, n);
         }
         else
            generic[i] = draw_find_shader_output(crender->ctx->draw,
                                                  TGSI_SEMANTIC_GENERIC, i);
         /* A varying the VS never writes (or a replaced sprite
          * coordinate) is undefined: any output fills its lanes. */
         if (generic[i] < 0)
            generic[i] = position;
         draw_emit_vertex_attr(&crender->vinfo, EMIT_4F, generic[i]);
      }
      draw_compute_vertex_size(&crender->vinfo);
      crender->layout_valid &= crender->vinfo.size ==
         4 + (primary ? 1 : 0) + 4 * crender->varying_count;
      return &crender->vinfo;
   }
   crender->varying_count = 0;

   crender->packed_generic =
      (program == CHROME9_FS_PRIMARY_SCALAR_RGBX ||
       program == CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX) ? 1 :
      program == CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE ? 2 :
      program == CHROME9_FS_PRIMARY_GENERIC_RGBA ? 3 :
      program == CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX ? 4 :
      (program == CHROME9_FS_PRIMARY_GENERIC_RGBX ||
       program == CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX) ? 5 :
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
                              program != CHROME9_FS_LITERAL_COLOR &&
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
   const bool primary = crender->ctx->fs &&
      crender->ctx->fs->semantic_input_mask == 1;

   unsigned expected_words = crender->generic_program ?
      4 + (primary ? 1 : 0) + 4 * crender->varying_count :
      crender->packed_generic >= 6 ? 12 :
      crender->packed_generic ? 9 : 6 + 4 * crender->texture_count;

   if (vertex_size != expected_words * sizeof(uint32_t)) {
      if (crender->ctx->debug_draws)
         fprintf(stderr, "chrome9: vbuf vertex size %u, layout expects %u "
                         "(generic=%u packed=%u varyings=%u textures=%u)\n",
                 vertex_size, expected_words * 4,
                 crender->generic_program, crender->packed_generic,
                 crender->varying_count, crender->texture_count);
      return false;
   }
   size = (size_t)vertex_size * nr_vertices;

   /* Reuse the buffer: allocating and zero-faulting megabytes per batch
    * dominated the software vertex path. */
   if (size > crender->allocated) {
      void *vertices = REALLOC(crender->vertices, crender->allocated, size);

      if (!vertices) {
         crender->capacity = 0;
         return false;
      }
      crender->vertices = vertices;
      crender->allocated = size;
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
chrome9_blend_factor(unsigned factor, bool extended, bool constant,
                     uint32_t *result)
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
   case PIPE_BLENDFACTOR_CONST_COLOR:
      *result = DRM_CHROME9_BLEND_FACTOR_CONST_COLOR;
      return constant;
   case PIPE_BLENDFACTOR_INV_CONST_COLOR:
      *result = DRM_CHROME9_BLEND_FACTOR_INV_CONST_COLOR;
      return constant;
   case PIPE_BLENDFACTOR_CONST_ALPHA:
      *result = DRM_CHROME9_BLEND_FACTOR_CONST_ALPHA;
      return constant;
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
      *result = DRM_CHROME9_BLEND_FACTOR_INV_CONST_ALPHA;
      return constant;
   default:
      return false;
   }
}

static bool
chrome9_blend_func(unsigned func, bool equation, uint32_t *result)
{
   switch (func) {
   case PIPE_BLEND_ADD:
      *result = DRM_CHROME9_BLEND_FUNC_ADD;
      return true;
   case PIPE_BLEND_SUBTRACT:
      *result = DRM_CHROME9_BLEND_FUNC_SUBTRACT;
      return equation;
   case PIPE_BLEND_REVERSE_SUBTRACT:
      *result = DRM_CHROME9_BLEND_FUNC_REVERSE_SUBTRACT;
      return equation;
   case PIPE_BLEND_MIN:
      *result = DRM_CHROME9_BLEND_FUNC_MIN;
      return equation;
   case PIPE_BLEND_MAX:
      *result = DRM_CHROME9_BLEND_FUNC_MAX;
      return equation;
   default:
      return false;
   }
}

static bool
chrome9_translate_blend(const struct pipe_blend_state *state,
                        struct drm_chrome9_blend_state *result,
                        bool extended, bool equation, bool constant,
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
   if (state->logicop_enable ||
       state->alpha_to_coverage ||
       state->alpha_to_coverage_dither || state->alpha_to_one ||
       state->max_rt > 3 || state->advanced_blend_func ||
       state->blend_coherent)
      return false;
   /* Colour targets share RT0's blend equation and write mask. */
   for (unsigned i = 1; state->independent_blend_enable &&
                        i <= state->max_rt; i++)
      if (memcmp(&state->rt[i], &state->rt[0], sizeof(state->rt[0])))
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
   if (!chrome9_blend_func(rt->rgb_func, equation, &result->rgb_func) ||
       !chrome9_blend_func(rt->alpha_func, equation, &result->alpha_func) ||
       !chrome9_blend_factor(rt->rgb_src_factor, extended, constant,
                             &result->rgb_src_factor) ||
       !chrome9_blend_factor(rt->rgb_dst_factor, extended, constant,
                             &result->rgb_dst_factor) ||
       !chrome9_blend_factor(rt->alpha_src_factor, extended, constant,
                             &result->alpha_src_factor) ||
       !chrome9_blend_factor(rt->alpha_dst_factor, extended, constant,
                             &result->alpha_dst_factor))
      return false;
   result->flags = DRM_CHROME9_BLEND_ENABLE;
   return true;
}

/* The pixel engine takes the bound blend state (no logic op, equation
 * and factor limits). */
bool
chrome9_draw_blend_supported(const struct chrome9_context *ctx)
{
   struct drm_chrome9_blend_state blend;
   bool successor;

   return chrome9_translate_blend(ctx->blend, &blend,
                                  ctx->blend_extended_factors,
                                  ctx->blend_equation,
                                  ctx->blend_color_factors &&
                                  chrome9_generic_fragment_program(ctx),
                                  &successor);
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
chrome9_mip_view_supported(const struct chrome9_context *ctx,
                           const struct chrome9_sampler_state *sampler,
                           const struct pipe_sampler_view *view,
                           const struct chrome9_resource *texture)
{
   const struct pipe_resource *resource = view ? view->texture : NULL;
   long page_size;
   unsigned last_level;
   uint64_t cursor;

   if (!ctx->draw_textured_triangle_mip || !sampler || !view || !resource ||
       sampler->mip_filter == DRM_CHROME9_TEXTURE_MIP_FILTER_NONE ||
       resource->target != PIPE_TEXTURE_2D ||
       resource->format != PIPE_FORMAT_B8G8R8X8_UNORM ||
       resource->width0 != resource->height0 || resource->width0 > 512 ||
       !util_is_power_of_two_or_zero(resource->width0) ||
       sampler->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE ||
       sampler->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE ||
       view->u.tex.first_level || view->u.tex.first_layer ||
       view->u.tex.last_layer || !texture->guarded)
      return false;

   last_level = view->u.tex.last_level;
   if (!last_level || last_level > 9 || last_level > resource->last_level ||
       last_level > util_logbase2(resource->width0) ||
       (last_level == 9 && resource->width0 != 512) ||
       sampler->min_level != 0.0f ||
       sampler->max_level < (float)last_level)
      return false;

   page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 || (uint64_t)page_size > UINT32_MAX ||
       texture->level_offset[0] != (uint64_t)page_size)
      return false;
   cursor = ALIGN_POT(texture->level_size[0], (uint64_t)page_size) +
            2 * (uint64_t)page_size;
   for (unsigned level = 1; level <= last_level; level++) {
      const unsigned width = u_minify(resource->width0, level);
      const unsigned height = u_minify(resource->height0, level);

      cursor = ALIGN_POT(cursor, 256);
      if (texture->level_offset[level] != cursor ||
          texture->level_stride[level] != ALIGN_POT(width * 4, 32) ||
          texture->level_size[level] !=
             (uint64_t)texture->level_stride[level] * height)
         return false;
      cursor += texture->level_size[level];
   }
   return true;
}

static bool
chrome9_stencil_active(const struct chrome9_context *ctx)
{
   return ctx->dsa && ctx->dsa->stencil[0].enabled;
}

/*
 * LESS, LEQUAL and EQUAL writes only move depths nearer, which keeps a
 * coarse-Z buffer conservative even when the draw does not update it;
 * any other depth write invalidates it.
 */
static void
chrome9_draw_coarse_z_write(struct chrome9_context *ctx, uint32_t depth_flags)
{
   struct chrome9_resource *depth = chrome9_context_depth_target(ctx);
   const unsigned func = (depth_flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
                         DRM_CHROME9_DEPTH_FUNC_SHIFT;

   if (depth && depth->coarse_z_valid &&
       !(depth_flags & DRM_CHROME9_DEPTH_READ_ONLY) &&
       func != PIPE_FUNC_LESS && func != PIPE_FUNC_LEQUAL &&
       func != PIPE_FUNC_EQUAL && func != PIPE_FUNC_NEVER)
      depth->coarse_z_valid = false;
}

/* Coarse-Z may reject and update for this draw: a nearer-wins depth test
 * whose passing fragments are all written. */
static bool
chrome9_draw_coarse_z_usable(const struct chrome9_context *ctx, uint32_t flags,
                             const struct drm_chrome9_blend_state *blend)
{
   const unsigned func = (flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
                         DRM_CHROME9_DEPTH_FUNC_SHIFT;
   const struct chrome9_ps_ir *ir = &ctx->fs->semantic_fs_ir;

   if (!ctx->draw_filled_triangles || ir->depth_output ||
       (func != PIPE_FUNC_LESS && func != PIPE_FUNC_LEQUAL) ||
       (flags & DRM_CHROME9_FRAGMENT_STENCIL) ||
       (blend && (blend->flags & DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE)))
      return false;
   for (unsigned i = 0; i < ir->count; i++)
      if (ir->instruction[i].op == CHROME9_PS_OPCODE_KILL)
         return false;
   return true;
}

static uint32_t
chrome9_draw_depth_flags(struct chrome9_context *ctx, bool depth_active)
{
   uint32_t depth_flags = 0;

   if (depth_active && !ctx->dsa->depth_enabled) {
      depth_flags = DRM_CHROME9_DEPTH_FUNC(DRM_CHROME9_COMPARE_ALWAYS) |
                    DRM_CHROME9_DEPTH_READ_ONLY;
   } else if (depth_active) {
      if (ctx->depth_func_supported || ctx->depth_equal_supported ||
          ctx->depth_always_supported)
         depth_flags = DRM_CHROME9_DEPTH_FUNC(ctx->dsa->depth_func);
      if (!ctx->dsa->depth_writemask)
         depth_flags |= DRM_CHROME9_DEPTH_READ_ONLY;
   }
   if (depth_active)
      chrome9_draw_coarse_z_write(ctx, depth_flags);
   return depth_flags;
}

/* Viewport edge along one axis, rounded to the pixel grid of the target. */
static unsigned
chrome9_viewport_edge(float translate, float scale, float side,
                      unsigned size)
{
   return CLAMP(lroundf(translate + side * fabsf(scale)), 0, (long)size);
}

static void
chrome9_draw_scissor(const struct chrome9_context *ctx,
                     const struct chrome9_resource *target,
                     struct drm_chrome9_scissor_state *scissor)
{
   const struct pipe_viewport_state *vp = &ctx->viewport;
   const unsigned width = target->base.width0;
   const unsigned height = target->base.height0;
   unsigned min_x = 0, min_y = 0, max_x = width, max_y = height;

   memset(scissor, 0, sizeof(*scissor));
   if (ctx->rasterizer && ctx->rasterizer->scissor) {
      min_x = MIN2(ctx->scissor.minx, width);
      max_x = MIN2(ctx->scissor.maxx, width);
      min_y = MIN2(ctx->scissor.miny, height);
      max_y = MIN2(ctx->scissor.maxy, height);
      scissor->flags = DRM_CHROME9_SCISSOR_ENABLE;
   }
   /* Hardware TCL clips to the guard band only: pixels outside the
    * viewport are scissored. */
   if (ctx->scissor_supported) {
      min_x = MAX2(min_x, chrome9_viewport_edge(vp->translate[0],
                                                vp->scale[0], -1, width));
      max_x = MIN2(max_x, chrome9_viewport_edge(vp->translate[0],
                                                vp->scale[0], 1, width));
      min_y = MAX2(min_y, chrome9_viewport_edge(vp->translate[1],
                                                vp->scale[1], -1, height));
      max_y = MIN2(max_y, chrome9_viewport_edge(vp->translate[1],
                                                vp->scale[1], 1, height));
      if (min_x || min_y || max_x < width || max_y < height)
         scissor->flags = DRM_CHROME9_SCISSOR_ENABLE;
   }
   if (scissor->flags) {
      scissor->min_x = min_x;
      scissor->max_x = MAX2(min_x, max_x);
      scissor->min_y = min_y;
      scissor->max_y = MAX2(min_y, max_y);
   }
}

/* Stencil needs the kernel stencil block and a D24S8 surface; without a
 * depth test it runs as depth ALWAYS with writes disabled. */
bool
chrome9_draw_stencil_supported(const struct chrome9_context *ctx)
{
   if (ctx->dsa && (ctx->dsa->depth_bounds_test ||
                    (ctx->dsa->stencil[1].enabled &&
                     !ctx->dsa->stencil[0].enabled)))
      return false;
   return !chrome9_stencil_active(ctx) ||
          (ctx->fragment_stencil &&
           chrome9_use_generic_fragment_program(ctx) &&
           ctx->framebuffer.zsbuf.format == PIPE_FORMAT_S8_UINT_Z24_UNORM &&
           (ctx->dsa->depth_enabled ||
            (ctx->depth_always_supported && ctx->depth_read_only_supported)));
}

static bool
chrome9_draw_state_supported(struct chrome9_context *ctx,
                             struct chrome9_resource **target,
                             struct chrome9_resource **depth,
                             struct chrome9_resource *textures[
                                DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
                             struct chrome9_sampler_state *samplers[
                                DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
                             unsigned texture_count, bool *depth_active,
                             struct drm_chrome9_blend_state *blend,
                             bool *blend_successor, const char **reason)
{
   const struct pipe_surface *surface;
   struct pipe_resource *resource;
   const bool depth_only = !ctx->framebuffer.nr_cbufs &&
      ctx->framebuffer.zsbuf.texture &&
      chrome9_format_is_d24(ctx->framebuffer.zsbuf.format);

   *depth = NULL;
   memset(textures, 0,
          DRM_CHROME9_FRAGMENT_MAX_TEXTURES * sizeof(*textures));
   memset(samplers, 0,
          DRM_CHROME9_FRAGMENT_MAX_TEXTURES * sizeof(*samplers));
   *depth_active = false;
   *reason = "context/target";
   if (!ctx->vs || !ctx->fs || !ctx->fs->supported ||
       !(*target = chrome9_context_render_target(ctx)))
      return false;
   resource = &(*target)->base;
   surface = ctx->framebuffer.nr_cbufs ?
             &ctx->framebuffer.cbufs[0] : NULL;
   *reason = "color-surface";
   if ((!depth_only &&
        (!surface || !surface->texture ||
         surface->format != resource->format ||
         /* Other levels and layers render through a level-0 shadow. */
         (resource == surface->texture &&
          (surface->level || surface->first_layer)) ||
         surface->first_layer != surface->last_layer)) ||
       (depth_only && ctx->render_target !=
                      ctx->depth_only_scratch) ||
       !chrome9_is_surface_target(resource->target) ||
       (resource->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        resource->format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       (resource->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
        !ctx->blend_bgra8888) ||
       !(*target)->guarded || !(*target)->bo.handle)
      return false;

   *reason = "blend-state";
   if (!chrome9_translate_blend(ctx->blend, blend,
                                ctx->blend_extended_factors,
                                ctx->blend_equation,
                                ctx->blend_color_factors &&
                                chrome9_generic_fragment_program(ctx),
                                blend_successor))
      return false;
   if (depth_only) {
      /* Keep a real guarded A50 target but suppress every color store.  A
       * blending equation would read undefined scratch contents. */
      if (blend->flags & DRM_CHROME9_BLEND_ENABLE)
         return false;
      blend->color_mask = 0;
      *blend_successor = true;
   }
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
   /*
    * The production one-ALU MODULATE shader object completes ordinary UI
    * traffic, but one repeatable ioquake3 shutdown state still stops VX900
    * before its completion cookie.  Fail the exact combination closed until
    * the remaining vendor state difference is understood, so one client
    * cannot poison the persistent ring for every later X client.
    */
   *reason = "textured-modulate-blend-unproven";
   if (!chrome9_use_generic_fragment_program(ctx) && texture_count == 1 &&
       (blend->flags & DRM_CHROME9_BLEND_ENABLE) &&
       ctx->fs->fragment_program != CHROME9_FS_TEXTURE_REPLACE &&
       ctx->fs->fragment_program != CHROME9_FS_TEXTURE_RGBX)
      return false;
   *reason = "stencil/depth-bounds";
   if (!chrome9_draw_stencil_supported(ctx))
      return false;
   if (ctx->dsa && (ctx->dsa->depth_enabled || chrome9_stencil_active(ctx))) {
      const struct pipe_surface *surface = &ctx->framebuffer.zsbuf;
      const bool d24 = chrome9_format_is_d24(surface->format);
      const bool depth_test = ctx->dsa->depth_enabled;

      *reason = "depth-state";
      *depth = chrome9_context_depth_target(ctx);
      if ((d24 ?
           !chrome9_use_generic_fragment_program(ctx) :
           !ctx->draw_triangle_d16) ||
          (depth_test && !ctx->dsa->depth_writemask &&
           !ctx->depth_read_only_supported) ||
          (depth_test && ctx->dsa->depth_func != PIPE_FUNC_LESS &&
           !(ctx->depth_func_supported &&
             ctx->dsa->depth_func == PIPE_FUNC_LEQUAL) &&
           !(ctx->depth_equal_supported &&
             ctx->dsa->depth_func == PIPE_FUNC_EQUAL) &&
           !(ctx->depth_always_supported &&
             ctx->dsa->depth_func == PIPE_FUNC_ALWAYS)) || !*depth ||
          (surface->format != PIPE_FORMAT_Z16_UNORM && !d24) ||
          surface->level ||
          surface->first_layer || surface->last_layer ||
          !chrome9_is_surface_target((*depth)->base.target) ||
          (*depth)->base.format != surface->format ||
          !(*depth)->guarded || !(*depth)->bo.handle ||
          (*depth)->base.width0 != resource->width0 ||
          (*depth)->base.height0 != resource->height0)
         return false;
      *depth_active = true;
   }
   if (depth_only && !*depth_active) {
      *reason = "depth-only-without-depth-state";
      return false;
   }
   *reason = "compiled-program-depth";
   if (ctx->fs->fragment_program == CHROME9_FS_COMPILED_ALU &&
       !chrome9_use_generic_fragment_program(ctx) &&
       (!*depth_active || !ctx->draw_program_triangle_d16_rhw_batch ||
        !ctx->fs->native_fs_valid || !ctx->fs->native_fs_from_nir))
      return false;
   *reason = "two-texture-successor";
   if (!chrome9_use_generic_fragment_program(ctx) && texture_count == 2) {
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
   if (!chrome9_use_generic_fragment_program(ctx) && texture_count &&
       *depth_active &&
       *blend_successor) {
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
   if (!chrome9_use_generic_fragment_program(ctx) && *blend_successor &&
       (*depth_active ?
        (texture_count == 2 ?
           !ctx->draw_two_textured_triangle_d16_blend :
         texture_count ? !ctx->draw_textured_triangle_d16_blend :
                         !ctx->draw_triangle_d16_blend) :
        !(texture_count ? ctx->draw_textured_triangle_blend :
                     ctx->draw_triangle_blend)))
      return false;
   if (texture_count) {
      /* ABI v1 requires every descriptor below the highest sampler ID. */
      *reason = "sparse-sampler-abi";
      if (chrome9_use_generic_fragment_program(ctx) &&
          !ctx->fragment_sparse_textures &&
          !chrome9_fragment_sampler_slots_dense(
             ctx->fs->semantic_sampler_mask))
         return false;
      *reason = "texture-count/depth";
      if ((!chrome9_use_generic_fragment_program(ctx) &&
           !(texture_count == 2 ?
             (*depth_active ?
                (*blend_successor ?
                   ctx->draw_two_textured_triangle_d16_blend :
                   ctx->draw_two_textured_triangle_d16) :
                              ctx->draw_two_textured_triangle) :
             (*depth_active ?
                (*blend_successor ?
                   ctx->draw_textured_triangle_d16_blend :
                   ctx->draw_textured_triangle_d16) :
                              ctx->draw_textured_triangle))) ||
          /* Gallium may retain an unused trailing sampler after unbinding
           * the corresponding view.  Only active view units are encoded. */
          ctx->num_sampler_states <
             (chrome9_use_generic_fragment_program(ctx) ?
              32 - __builtin_clz(ctx->fs->semantic_sampler_mask) :
              texture_count) ||
          ctx->num_sampler_views <
             (chrome9_use_generic_fragment_program(ctx) ?
              32 - __builtin_clz(ctx->fs->semantic_sampler_mask) :
              texture_count))
         return false;
      for (unsigned unit = 0; unit < texture_count; unit++) {
         unsigned logical = chrome9_use_generic_fragment_program(ctx) ?
            chrome9_fragment_texture_unit(ctx, unit) : unit;
         struct pipe_sampler_view *view = ctx->sampler_views[logical];
         struct pipe_resource *original = view ? view->texture : NULL;
         struct pipe_resource *source = view ?
            chrome9_sampler_view_resource(view) : NULL;
         const struct chrome9_texture_format_info *format = source ?
            chrome9_texture_format_info(source->format) : NULL;

         samplers[unit] = ctx->sampler_states[logical];
         *reason = "sampler-state";
         if (!samplers[unit] || !samplers[unit]->supported || !source)
            return false;
         /* Border and mirror-once exist in the fragment program ABI only. */
         if (!chrome9_use_generic_fragment_program(ctx) &&
             (samplers[unit]->wrap_s >
                 DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT ||
              samplers[unit]->wrap_t >
                 DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT))
            return false;
         textures[unit] = chrome9_resource(source);
         *reason = "texture-resource";
         if (original == resource || source == resource ||
             textures[unit]->bo.handle == (*target)->bo.handle ||
             (*depth_active &&
              textures[unit]->bo.handle == (*depth)->bo.handle) ||
             (!chrome9_is_surface_target(source->target) &&
              ((source->target != PIPE_TEXTURE_CUBE &&
                source->target != PIPE_TEXTURE_3D) ||
               !chrome9_use_generic_fragment_program(ctx))) ||
             (!chrome9_use_generic_fragment_program(ctx) ?
                (source->format != PIPE_FORMAT_B8G8R8X8_UNORM &&
                 source->format != PIPE_FORMAT_B8G8R8A8_UNORM) : !format) ||
             (view->u.tex.first_level &&
              !chrome9_use_generic_fragment_program(ctx)) ||
             view->u.tex.first_level > view->u.tex.last_level ||
             view->u.tex.last_level > original->last_level ||
             !textures[unit]->guarded ||
             !textures[unit]->bo.handle ||
             (textures[unit]->data_offset & 0xff) ||
             textures[unit]->stride !=
                chrome9_format_level_pitch(source->format, source->width0) ||
             textures[unit]->data_size <
                (uint64_t)textures[unit]->stride *
                chrome9_format_level_rows(source->format, source->height0))
            return false;
         /* The TEX coordinate width follows the shader's sampler type. */
         *reason = "texture-target";
         if (chrome9_use_generic_fragment_program(ctx) &&
             (!!(ctx->fs->semantic_cube_sampler_mask & (1u << logical)) !=
                 (source->target == PIPE_TEXTURE_CUBE) ||
              !!(ctx->fs->semantic_volume_sampler_mask & (1u << logical)) !=
                 (source->target == PIPE_TEXTURE_3D)))
            return false;
         *reason = "mip-view-state";
         /* D24 is point sampled only; a compare may request LINEAR, whose
          * result GL leaves implementation-dependent (vendor: nearest). */
         if (chrome9_format_is_d24(source->format) &&
             ((!chrome9_depth_compare(ctx, logical) &&
               (samplers[unit]->min_filter !=
                   DRM_CHROME9_TEXTURE_FILTER_NEAREST ||
                samplers[unit]->mag_filter !=
                   DRM_CHROME9_TEXTURE_FILTER_NEAREST)) ||
              samplers[unit]->mip_filter !=
                 DRM_CHROME9_TEXTURE_MIP_FILTER_NONE ||
              view->u.tex.last_level || source->last_level))
            return false;
         if (samplers[unit]->mip_filter !=
                DRM_CHROME9_TEXTURE_MIP_FILTER_NONE &&
             !chrome9_use_generic_fragment_program(ctx) &&
             (texture_count != 1 || unit ||
              !chrome9_mip_view_supported(ctx, samplers[unit], view,
                                          textures[unit])))
            return false;
         /* The fixed programs below read one source layout; the generic
          * program is compiled for the bound formats. */
         if (chrome9_use_generic_fragment_program(ctx))
            continue;
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
      /* The fixed two-texture ioctls take distinct objects; the generic
       * program shares one object between read-only units. */
      *reason = "texture-alias";
      for (unsigned a = 0;
           !chrome9_use_generic_fragment_program(ctx) && a < texture_count;
           a++)
         for (unsigned b = a + 1; b < texture_count; b++)
            if (textures[a]->bo.handle == textures[b]->bo.handle)
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

static bool
chrome9_fragment_constants(struct chrome9_context *ctx, uint32_t count,
                           uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4])
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants[0];
   const uint32_t uniform_mask = ctx->fs->semantic_constant_mask &
      ~ctx->fs->native_fs.immediate_mask;
   const unsigned uniform_count = uniform_mask ?
      32 - __builtin_clz(uniform_mask) : 0;
   const uint8_t *map = NULL;

   memset(constants, 0,
          DRM_CHROME9_FRAGMENT_MAX_CONSTANTS * 4 * sizeof(uint32_t));
   if (uniform_count) {
      uint64_t size = (uint64_t)uniform_count * 4 * sizeof(uint32_t);

      if (cb->buffer_size >= size && cb->buffer_offset <= UINT32_MAX - size) {
         if (cb->user_buffer) {
            map = (const uint8_t *)cb->user_buffer + cb->buffer_offset;
         } else if (cb->buffer && cb->buffer->target == PIPE_BUFFER &&
                    cb->buffer_offset <= cb->buffer->width0 &&
                    size <= cb->buffer->width0 - cb->buffer_offset) {
            map = chrome9_resource(cb->buffer)->bo.map;
            if (map)
               map += cb->buffer_offset;
         }
      }
      if (map)
         memcpy(constants, map, size);
   }
   for (unsigned i = 0; i < count; i++) {
      if (ctx->fs->native_fs.immediate_mask & (1u << i))
         memcpy(constants[i], ctx->fs->native_fs.immediate[i],
                sizeof(constants[i]));
      else if ((uniform_mask & (1u << i)) && !map)
         return false;
      chrome9_finite_constants(constants[i], 4);
   }
   return true;
}

static bool
chrome9_equal_zero_value(uint32_t bits)
{
   const uint32_t magnitude = bits & 0x7fffffff;

   /* Signed zero stays zero; +/-1 times positive finite-normal RHW cannot
    * underflow or overflow before perspective interpolation. */
   return magnitude == 0 || magnitude == 0x3f800000;
}

bool
chrome9_fragment_unit_interval_value(uint32_t bits)
{
   float value;

   memcpy(&value, &bits, sizeof(value));
   return isfinite(value) && value >= 0.0f && value <= 1.0f;
}

static bool
chrome9_fragment_unit_interval_texture(enum pipe_format format)
{
   /* Keep the range-dependent KILL proof explicit if the generic sampler
    * table later grows a float or signed-normalized format.  Filtering and
    * the canonical view swizzles preserve [0, 1] for these UNORM decoders. */
   switch (format) {
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_A8_UNORM:
   case PIPE_FORMAT_L8_UNORM:
   case PIPE_FORMAT_L8A8_UNORM:
   case PIPE_FORMAT_B5G6R5_UNORM:
   case PIPE_FORMAT_X8Z24_UNORM:
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
   case PIPE_FORMAT_DXT1_RGB:
   case PIPE_FORMAT_DXT1_RGBA:
   case PIPE_FORMAT_DXT3_RGBA:
   case PIPE_FORMAT_DXT5_RGBA:
      return true;
   default:
      return false;
   }
}

/* Preflight the entire post-VS vbuf callback before its first batch can be
 * submitted.  Equality-to-zero lanes must be bit-identical in their proven
 * finite domain, omitted projectors must be exactly one, and unit-interval
 * lanes must remain finite and within [0, 1] at every vertex.
 * The batches are deferred until the complete Gallium draw ends, so a late
 * callback cannot invalidate an earlier cmd22 submission. */
static bool
chrome9_guarded_draw_domain(const struct chrome9_vbuf_render *crender,
                            const uint16_t *indices, unsigned start,
                            unsigned count)
{
   const uint32_t equal_zero_mask = crender->ctx->fs ?
      crender->ctx->fs->semantic_equal_zero_input_mask : 0;
   const uint32_t projector_mask = crender->ctx->fs ?
      crender->ctx->fs->semantic_identity_projector_input_mask : 0;
   const uint32_t unit_interval_mask = crender->ctx->fs ?
      crender->ctx->fs->semantic_unit_interval_input_mask : 0;
   const unsigned primary = crender->ctx->fs &&
      crender->ctx->fs->semantic_input_mask == 1;
   uint32_t reference[32] = { 0 };

   if (!equal_zero_mask && !projector_mask && !unit_interval_mask)
      return true;
   if (!crender->generic_program || !crender->layout_valid)
      return false;
   for (unsigned i = 0; i < count; i++) {
      const unsigned index = indices ? indices[i] : start + i;
      const float *vertex;
      uint32_t rhw;

      if (index >= crender->vertex_count ||
          (size_t)(index + 1) * crender->vertex_size > crender->capacity)
         return false;
      vertex = (const float *)(crender->vertices +
                               (size_t)index * crender->vertex_size);
      memcpy(&rhw, vertex + 3, sizeof(rhw));
      if ((rhw & 0x80000000) || !(rhw & 0x7f800000) ||
          (rhw & 0x7f800000) == 0x7f800000)
         return false;
      for (uint32_t remaining = equal_zero_mask; remaining;
           remaining &= remaining - 1) {
         unsigned lane = __builtin_ctz(remaining);
         uint32_t bits;

         if (lane / 4 >= crender->varying_count)
            return false;
         memcpy(&bits, vertex + 4 + primary + lane, sizeof(bits));
         if (!chrome9_equal_zero_value(bits) ||
             (i && bits != reference[lane]))
            return false;
         reference[lane] = bits;
      }
      for (uint32_t remaining = projector_mask; remaining;
           remaining &= remaining - 1) {
         unsigned lane = __builtin_ctz(remaining);
         uint32_t bits;

         if (lane / 4 >= crender->varying_count)
            return false;
         memcpy(&bits, vertex + 4 + primary + lane, sizeof(bits));
         if (bits != 0x3f800000)
            return false;
      }
      for (uint32_t remaining = unit_interval_mask; remaining;
           remaining &= remaining - 1) {
         unsigned lane = __builtin_ctz(remaining);
         uint32_t bits;

         if (lane / 4 >= crender->varying_count)
            return false;
         memcpy(&bits, vertex + 4 + primary + lane, sizeof(bits));
         if (!chrome9_fragment_unit_interval_value(bits))
            return false;
      }
   }
   return true;
}

static void chrome9_flush_triangle_batch(struct chrome9_vbuf_render *crender);

/* Resolve the GL face of one post-viewport triangle exactly as Draw's cull
 * stage does and mirror that face into both native winding blocks, so the
 * result does not depend on the hardware's A23/A25 orientation. */
static void
chrome9_triangle_stencil(const struct chrome9_context *ctx,
                         const struct drm_chrome9_fragment_vertex v[3],
                         struct drm_chrome9_stencil_state *state)
{
   static const uint8_t native_op[8] = {
      [PIPE_STENCIL_OP_KEEP] = DRM_CHROME9_STENCIL_OP_KEEP,
      [PIPE_STENCIL_OP_ZERO] = DRM_CHROME9_STENCIL_OP_ZERO,
      [PIPE_STENCIL_OP_REPLACE] = DRM_CHROME9_STENCIL_OP_REPLACE,
      [PIPE_STENCIL_OP_INCR] = DRM_CHROME9_STENCIL_OP_INCR_SAT,
      [PIPE_STENCIL_OP_DECR] = DRM_CHROME9_STENCIL_OP_DECR_SAT,
      [PIPE_STENCIL_OP_INCR_WRAP] = DRM_CHROME9_STENCIL_OP_INCR_WRAP,
      [PIPE_STENCIL_OP_DECR_WRAP] = DRM_CHROME9_STENCIL_OP_DECR_WRAP,
      [PIPE_STENCIL_OP_INVERT] = DRM_CHROME9_STENCIL_OP_INVERT,
   };
   const int64_t ex = (int64_t)v[0].x_16_16 - v[2].x_16_16;
   const int64_t ey = (int64_t)v[0].y_16_16 - v[2].y_16_16;
   const int64_t fx = (int64_t)v[1].x_16_16 - v[2].x_16_16;
   const int64_t fy = (int64_t)v[1].y_16_16 - v[2].y_16_16;
   const bool ccw = ex * fy - ey * fx < 0;
   const bool front_ccw = ctx->rasterizer && ctx->rasterizer->front_ccw;
   const unsigned face = ccw == front_ccw || !ctx->dsa->stencil[1].enabled ?
      0 : 1;
   const struct pipe_stencil_state *s = &ctx->dsa->stencil[face];
   const struct drm_chrome9_stencil_face native = {
      .func = s->func,
      .fail_op = native_op[s->fail_op & 7],
      .zfail_op = native_op[s->zfail_op & 7],
      .zpass_op = native_op[s->zpass_op & 7],
      .ref = ctx->stencil_ref.ref_value[face],
      .value_mask = s->valuemask,
      .write_mask = s->writemask,
   };

   state->cw = native;
   state->ccw = native;
}

/* Everything of a generic cmd22 request except its vertices and stencil
 * faces; shared by the post-TNL batches and the hardware TCL path. */
/* Destination, depth, blend, query and scissor state of a generic draw. */
static bool
chrome9_fragment_draw_target(
   struct chrome9_vbuf_render *crender, struct chrome9_resource *target,
   struct chrome9_resource *depth, bool depth_active, bool blend_active,
   const struct drm_chrome9_blend_state *blend,
   const struct drm_chrome9_scissor_state *scissor, uint32_t depth_flags,
   struct drm_chrome9_draw_fragment_program *draw)
{
   draw->destination_handle = target->bo.handle;
   draw->depth_handle = depth_active ? depth->bo.handle : 0;
   draw->flags = (depth_active ? DRM_CHROME9_FRAGMENT_DEPTH | depth_flags : 0) |
                (blend_active ? DRM_CHROME9_FRAGMENT_BLEND : 0);
   if (depth_active && chrome9_stencil_active(crender->ctx))
      draw->flags |= DRM_CHROME9_FRAGMENT_STENCIL;
   if (depth_active && depth->tiled)
      draw->flags |= DRM_CHROME9_FRAGMENT_DEPTH_TILED;
   if (chrome9_resource_is_tiled(target))
      draw->flags |= DRM_CHROME9_FRAGMENT_DESTINATION_TILED;
   if (depth_active && depth->fast_z_index &&
       (draw->flags & DRM_CHROME9_FRAGMENT_STENCIL)) {
      chrome9_resource_fast_z_resolve(depth);
      depth->fast_z_disabled = true;
   }
   if (depth_active && depth->fast_z_index)
      draw->flags |= DRM_CHROME9_FRAGMENT_FAST_Z |
                     DRM_CHROME9_FRAGMENT_FAST_Z_INDEX(depth->fast_z_index);
   if (depth_active && depth->coarse_z.handle && crender->ctx->coarse_z) {
      struct chrome9_context *ctx = crender->ctx;

      /* Draws of one submission name the buffer even when not using it. */
      draw->coarse_z_handle = depth->coarse_z.handle;
      draw->coarse_z_pitch = depth->coarse_z_pitch;
      if (depth->coarse_z_valid &&
          chrome9_draw_coarse_z_usable(ctx, draw->flags,
                                       blend_active ? blend : NULL)) {
         draw->flags |= DRM_CHROME9_FRAGMENT_COARSE_Z;
         if (depth->coarse_z_reset || !ctx->coarse_z_last)
            draw->flags |= DRM_CHROME9_FRAGMENT_COARSE_Z_RESET;
         depth->coarse_z_reset = false;
         ctx->coarse_z_last = true;
      } else {
         ctx->coarse_z_last = false;
      }
   }
   if (chrome9_resource_extended(target)) {
      if (!target->guarded ||
          !chrome9_screen_has_capability(
             crender->ctx->base.screen,
             DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED))
         return false;
      draw->flags |= DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED;
      draw->destination_offset = chrome9_resource_destination_offset(target);
   }
   /* The count and user pointers are filled only when the batch is flushed.
    * Keeping them zero makes the rest of this structure a comparable state
    * key, including target, depth, blend and scissor state. */
   draw->width = target->base.width0;
   draw->height = target->base.height0;
   draw->destination_pitch = target->stride;
   draw->depth_pitch = depth_active ? depth->stride : 0;
   draw->destination_format =
      target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
      DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
      DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
   draw->depth_format = depth_active ?
      (chrome9_format_is_d24(depth->base.format) ?
       DRM_CHROME9_FRAGMENT_DEPTH_X8Z24 : DRM_CHROME9_FRAGMENT_DEPTH_D16) :
      0;
   draw->varying_count = crender->varying_count;
   draw->query = chrome9_query_draw_id(crender->ctx);
   if (draw->query)
      draw->flags |= DRM_CHROME9_FRAGMENT_QUERY;
   if (blend_active) {
      draw->blend = *blend;
      if (chrome9_blend_uses_constant(crender->ctx->blend)) {
         /* Unsigned 1.10 as the vendor converts it: floor(c * 1024). */
         for (unsigned c = 0; c < 4; c++)
            draw->blend_color[c] = MIN2(
               (unsigned)(CLAMP(crender->ctx->blend_color.color[c],
                                0.0f, 1.0f) * 1024.0f), 0x400);
      }
   }
   draw->scissor = *scissor;
   return true;
}

/* Descriptor of one application texture at logical unit. */
static bool
chrome9_fragment_texture_descriptor(
   const struct chrome9_context *ctx, unsigned logical, bool sparse,
   struct chrome9_resource *texture,
   const struct chrome9_sampler_state *sampler,
   struct drm_chrome9_fragment_texture_state *state)
{
   const struct pipe_resource *resource = &texture->base;
   const struct chrome9_texture_format_info *format =
      chrome9_texture_format_info(resource->format);
   const struct pipe_sampler_view *view = ctx->sampler_views[logical];
   const bool volume = resource->target == PIPE_TEXTURE_3D;
   unsigned last_level;
   unsigned view_first_level;
   unsigned view_last_level;
   int max_lod;

   if (!view || !format)
      return false;
   view_first_level = view->u.tex.first_level;
   view_last_level = view->u.tex.last_level;
   /* The volume sampler reads level zero only (no volume mips). */
   last_level = volume ? 0 : resource->last_level;
   /*
    * The descriptor always carries the whole chain; a partial view
    * clamps the hardware LOD, which counts from level 0, to its levels.
    * Above its base level the hardware never takes the magnification
    * filter, so that must equal the minification filter.  Without mip
    * sampling the hardware LOD is 0, so only a level-0 base works.
    */
   if (last_level >= DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS ||
       (view_first_level &&
        (sampler->mip_filter == DRM_CHROME9_TEXTURE_MIP_FILTER_NONE ||
         sampler->mag_filter != sampler->min_filter)) ||
       (volume && (view_first_level || !sampler->wrap_r)))
      return false;
   max_lod = volume ? 0 : view_last_level * 32;
   state->handle = texture->bo.handle;
   state->flags = chrome9_resource_texture_flags(texture);
   if (sparse)
      state->reserved0 = logical;
   state->format = format->drm_format;
   state->width = resource->width0;
   state->height = resource->height0;
   state->last_level = last_level;
   state->min_filter = sampler->min_filter;
   state->mag_filter = sampler->mag_filter;
   if (chrome9_format_is_d24(resource->format)) {
      state->min_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
      state->mag_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
   }
   state->mip_filter = sampler->mip_filter;
   state->wrap_s = sampler->wrap_s;
   state->wrap_t = sampler->wrap_t;
   /* 1D samples its single row at T = 0.5 whatever the T mode. */
   if (resource->target == PIPE_TEXTURE_1D)
      state->wrap_t = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   if (resource->target == PIPE_TEXTURE_CUBE) {
      /* No seamless filtering: face coordinates clamp to the edge. */
      state->target = DRM_CHROME9_FRAGMENT_TARGET_CUBE;
      state->face_stride = texture->layer_stride;
      state->wrap_s = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
      state->wrap_t = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   }
   if (volume) {
      state->target = DRM_CHROME9_FRAGMENT_TARGET_3D;
      state->face_stride = texture->level_size[0];
      state->depth = resource->depth0;
      state->wrap_r = sampler->wrap_r;
      state->mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_NONE;
   }
   if (state->wrap_s == DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER ||
       state->wrap_t == DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER ||
       state->wrap_r == DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_BORDER)
      state->border_color = sampler->border_color;
   state->lod_bias_s5_5 = CLAMP(
      lroundf(sampler->base.lod_bias * 32.0f), -512, 511);
   /* Sampler LOD limits count from the view's base level. */
   state->min_lod_s5_5 = CLAMP(
      lroundf((sampler->min_level + view_first_level) * 32.0f),
      (int)view_first_level * 32, max_lod);
   state->max_lod_s5_5 = CLAMP(
      lroundf((sampler->max_level + view_first_level) * 32.0f),
      state->min_lod_s5_5, max_lod);
   for (unsigned level = 0; level <= last_level; level++) {
      if (texture->level_offset[level] > UINT32_MAX)
         return false;
      state->level_offset[level] = texture->level_offset[level];
      state->level_pitch[level] = texture->level_stride[level];
   }
   return true;
}

static bool
chrome9_generic_fragment_state(
   struct chrome9_vbuf_render *crender, struct chrome9_resource *target,
   struct chrome9_resource *depth,
   struct chrome9_resource *textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
   struct chrome9_sampler_state *samplers[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
   bool depth_active, bool blend_active,
   const struct drm_chrome9_blend_state *blend,
   const struct drm_chrome9_scissor_state *scissor, uint32_t depth_flags,
   struct drm_chrome9_draw_fragment_program *draw,
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS],
   uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4],
   struct drm_chrome9_fragment_texture_state
      texture_state[DRM_CHROME9_FRAGMENT_MAX_TEXTURES])
{
   uint8_t depth_swizzle[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4] = { 0 };
   uint8_t depth_sampler_mask = 0;
   const unsigned constant_count = crender->ctx->fs->semantic_constant_mask ?
      32 - __builtin_clz(crender->ctx->fs->semantic_constant_mask) : 0;
   const struct chrome9_ps_ir *ir = &crender->ctx->fs->semantic_fs_ir;
   struct chrome9_ps_ir color_only;
   unsigned instruction_count;

   /* Without a depth buffer gl_FragDepth has no effect. */
   if (ir->depth_output && !depth_active) {
      color_only = *ir;
      color_only.depth_output = false;
      ir = &color_only;
   }
   /* Fast-Z stored depths are not the shader's to replace. */
   if (ir->depth_output && depth->fast_z_index) {
      chrome9_resource_fast_z_resolve(depth);
      depth->fast_z_disabled = true;
   }
   memset(draw, 0, sizeof(*draw));
   memset(texture_state, 0,
          DRM_CHROME9_FRAGMENT_MAX_TEXTURES * sizeof(*texture_state));
   if (!chrome9_fragment_build_instructions(
          ir,
          crender->ctx->fs->semantic_sampler_mask, instructions,
          &instruction_count) ||
       !chrome9_fragment_constants(crender->ctx, constant_count, constants))
      return false;
   for (unsigned unit = 0; unit < crender->texture_count; unit++) {
      const unsigned logical = chrome9_fragment_texture_unit(crender->ctx,
                                                              unit);
      const struct pipe_sampler_view *view =
         crender->ctx->sampler_views[logical];

      if (textures[unit]->base.format == PIPE_FORMAT_DXT1_RGB) {
         static const uint8_t opaque[4] = {
            DRM_CHROME9_FRAGMENT_SWIZZLE_X, DRM_CHROME9_FRAGMENT_SWIZZLE_Y,
            DRM_CHROME9_FRAGMENT_SWIZZLE_Z, DRM_CHROME9_FRAGMENT_SWIZZLE_ONE,
         };

         depth_sampler_mask |= 1u << logical;
         memcpy(depth_swizzle[logical], opaque, sizeof(opaque));
         continue;
      }
      /* The lowered compare reads the raw depth and swizzles itself. */
      if (!chrome9_format_is_d24(textures[unit]->base.format) ||
          chrome9_depth_compare(crender->ctx, logical))
         continue;
      if (!view)
         return false;
      depth_sampler_mask |= 1u << logical;
      if (!chrome9_depth_view_swizzle(view->swizzle_r,
                                      &depth_swizzle[logical][0]) ||
          !chrome9_depth_view_swizzle(view->swizzle_g,
                                      &depth_swizzle[logical][1]) ||
          !chrome9_depth_view_swizzle(view->swizzle_b,
                                      &depth_swizzle[logical][2]) ||
          !chrome9_depth_view_swizzle(view->swizzle_a,
                                      &depth_swizzle[logical][3]))
         return false;
   }
   if (depth_sampler_mask &&
       !chrome9_fragment_apply_depth_swizzles(instructions,
                                               instruction_count,
                                               depth_sampler_mask,
                                               depth_swizzle))
      return false;
   for (unsigned unit = 0; unit < crender->texture_count; unit++) {
      const unsigned logical = chrome9_fragment_texture_unit(crender->ctx,
                                                              unit);

      if (crender->ctx->fs->semantic_unit_interval_input_mask &&
          logical == 0 &&
          !chrome9_fragment_unit_interval_texture(textures[unit]->base.format))
         return false;
      if (!chrome9_fragment_texture_descriptor(
             crender->ctx, logical,
             chrome9_fragment_uses_sparse_abi(crender->ctx),
             textures[unit], samplers[unit], &texture_state[unit]))
         return false;
   }
   if (!chrome9_fragment_draw_target(crender, target, depth, depth_active,
                                     blend_active, blend, scissor,
                                     depth_flags, draw))
      return false;
   draw->instruction_count = instruction_count;
   draw->constant_count = constant_count;
   draw->texture_count = crender->texture_count;
   draw->abi_version = chrome9_fragment_uses_sparse_abi(crender->ctx) ?
      DRM_CHROME9_FRAGMENT_ABI_SPARSE_TEXTURES :
      DRM_CHROME9_FRAGMENT_ABI_VERSION;
   return true;
}

/* Kernel vertices of one draw-module triangle, domain checks included. */
static bool
chrome9_fragment_vertices(const struct chrome9_vbuf_render *crender,
                          const unsigned indices[3],
                          const struct chrome9_resource *target,
                          struct drm_chrome9_fragment_vertex vertices[3])
{
   const float pixel_center_bias = crender->ctx->rasterizer &&
      crender->ctx->rasterizer->half_pixel_center ? 0.5f : 0.0f;
   const bool primary = crender->ctx->fs->semantic_input_mask == 1;

   memset(vertices, 0, 3 * sizeof(*vertices));
   for (unsigned i = 0; i < 3; i++) {
      const float *source;

      if (indices[i] >= crender->vertex_count ||
          (size_t)(indices[i] + 1) * crender->vertex_size > crender->capacity)
         return false;
      source = (const float *)(crender->vertices +
         (size_t)indices[i] * crender->vertex_size);
      for (unsigned c = 0; c < 4; c++)
         if (!isfinite(source[c]))
            return false;
      for (unsigned c = 0; c < 4 * crender->varying_count; c++)
         if (!isfinite(source[4 + primary + c]))
            return false;
      vertices[i].x_16_16 = chrome9_coord_16_16(
         source[0] - pixel_center_bias, target->base.width0);
      vertices[i].y_16_16 = chrome9_coord_16_16(
         source[1] - pixel_center_bias, target->base.height0);
      vertices[i].z_16_16 = chrome9_depth_16_16(source[2]);
      memcpy(&vertices[i].rhw_float, &source[3], sizeof(uint32_t));
      if (primary)
         memcpy(&vertices[i].color, source + 4, sizeof(vertices[i].color));
      memcpy(vertices[i].varying, source + 4 + primary,
             crender->varying_count * 4 * sizeof(uint32_t));
   }
   for (uint32_t remaining = crender->ctx->fs->semantic_equal_zero_input_mask;
        remaining; remaining &= remaining - 1) {
      const unsigned lane = __builtin_ctz(remaining);
      const uint32_t bits = vertices[0].varying[lane / 4][lane % 4];

      if (lane / 4 >= crender->varying_count ||
          !chrome9_equal_zero_value(bits) ||
          vertices[1].varying[lane / 4][lane % 4] != bits ||
          vertices[2].varying[lane / 4][lane % 4] != bits)
         return false;
   }
   for (uint32_t remaining =
           crender->ctx->fs->semantic_identity_projector_input_mask;
        remaining; remaining &= remaining - 1) {
      const unsigned lane = __builtin_ctz(remaining);

      if (lane / 4 >= crender->varying_count ||
          vertices[0].varying[lane / 4][lane % 4] != 0x3f800000 ||
          vertices[1].varying[lane / 4][lane % 4] != 0x3f800000 ||
          vertices[2].varying[lane / 4][lane % 4] != 0x3f800000)
         return false;
   }
   for (uint32_t remaining =
           crender->ctx->fs->semantic_unit_interval_input_mask;
        remaining; remaining &= remaining - 1) {
      const unsigned lane = __builtin_ctz(remaining);

      if (lane / 4 >= crender->varying_count ||
          !chrome9_fragment_unit_interval_value(
             vertices[0].varying[lane / 4][lane % 4]) ||
          !chrome9_fragment_unit_interval_value(
             vertices[1].varying[lane / 4][lane % 4]) ||
          !chrome9_fragment_unit_interval_value(
             vertices[2].varying[lane / 4][lane % 4]))
         return false;
   }
   return true;
}


/* Carrier objects of at least width x height, count of them. */
static bool
chrome9_split_carriers(struct chrome9_context *ctx, unsigned count,
                       unsigned width, unsigned height)
{
   struct pipe_screen *screen = ctx->base.screen;
   unsigned have = util_dynarray_num_elements(&ctx->split_carriers,
                                              struct pipe_resource *);

   if (width > ctx->split_carrier_width ||
       height > ctx->split_carrier_height) {
      util_dynarray_foreach(&ctx->split_carriers, struct pipe_resource *,
                            carrier)
         pipe_resource_reference(carrier, NULL);
      util_dynarray_clear(&ctx->split_carriers);
      ctx->split_carrier_width = MAX2(width, ctx->split_carrier_width);
      ctx->split_carrier_height = MAX2(height, ctx->split_carrier_height);
      have = 0;
   }
   for (; have < count; have++) {
      /* Linear 32-bit texels; draws name the object as R32F. */
      const struct pipe_resource templ = {
         .target = PIPE_TEXTURE_2D,
         .format = PIPE_FORMAT_B8G8R8A8_UNORM,
         .width0 = ctx->split_carrier_width,
         .height0 = ctx->split_carrier_height,
         .depth0 = 1,
         .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      struct pipe_resource *carrier = screen->resource_create(screen, &templ);

      if (!carrier)
         return false;
      if (!chrome9_resource(carrier)->guarded ||
          chrome9_resource(carrier)->tiled) {
         pipe_resource_reference(&carrier, NULL);
         return false;
      }
      util_dynarray_append(&ctx->split_carriers, carrier);
   }
   return true;
}

/* A split program's draw can run: its carriers exist for the target and
 * every job's samplers are expressible.  Else llvmpipe renders it. */
bool
chrome9_split_draw_prepare(struct chrome9_context *ctx)
{
   const struct chrome9_fs_split *split = ctx->fs->split;
   struct chrome9_resource *target = chrome9_context_render_target(ctx);

   if (!target)
      return false;
   for (unsigned j = 0; j < split->job_count; j++) {
      const uint8_t used = split->jobs[j].program->semantic_sampler_mask;

      if (used != BITFIELD_MASK(util_last_bit(used)) &&
          !ctx->fragment_sparse_textures)
         return false;
   }
   u_foreach_bit(unit, split->app_sampler_mask)
      if (unit >= ctx->num_sampler_views || !ctx->sampler_views[unit])
         return false;
   return chrome9_split_carriers(ctx, split->carrier_count,
                                 target->base.width0, target->base.height0);
}

static void
chrome9_split_carrier_texture(struct chrome9_resource *carrier,
                              unsigned unit, bool sparse,
                              struct drm_chrome9_fragment_texture_state *state)
{
   memset(state, 0, sizeof(*state));
   state->handle = carrier->bo.handle;
   state->flags = chrome9_resource_texture_flags(carrier);
   if (sparse)
      state->reserved0 = unit;
   state->format = DRM_CHROME9_FRAGMENT_TEXTURE_R32F;
   state->width = carrier->base.width0;
   state->height = carrier->base.height0;
   state->min_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
   state->mag_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
   state->mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_NONE;
   state->wrap_s = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   state->wrap_t = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
   state->level_offset[0] = carrier->level_offset[0];
   state->level_pitch[0] = carrier->level_stride[0];
}

/* Uniforms, immediates and the carrier inverse size of one job. */
static bool
chrome9_split_constants(struct chrome9_context *ctx,
                        const struct chrome9_fs_split *split,
                        const struct chrome9_shader *program,
                        unsigned carrier_width, unsigned carrier_height,
                        uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4],
                        unsigned *count)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants[0];
   const uint32_t mask = program->semantic_constant_mask;
   const uint8_t *map = NULL;
   size_t size = 0;

   memset(constants, 0,
          DRM_CHROME9_FRAGMENT_MAX_CONSTANTS * 4 * sizeof(uint32_t));
   *count = util_last_bit(mask);
   if (cb->user_buffer) {
      map = (const uint8_t *)cb->user_buffer + cb->buffer_offset;
      size = cb->buffer_size;
   } else if (cb->buffer && cb->buffer->target == PIPE_BUFFER &&
              cb->buffer_offset <= cb->buffer->width0) {
      map = chrome9_resource(cb->buffer)->bo.map;
      if (map)
         map += cb->buffer_offset;
      size = MIN2(cb->buffer_size, cb->buffer->width0 - cb->buffer_offset);
   }
   for (unsigned i = 0; i < *count; i++) {
      if (program->native_fs.immediate_mask & (1u << i)) {
         memcpy(constants[i], program->native_fs.immediate[i],
                sizeof(constants[i]));
      } else if (i == split->inverse_register) {
         const float inverse[4] = {
            1.0f / carrier_width, 1.0f / carrier_height,
            0.5f / carrier_width, 0.5f / carrier_height,
         };

         memcpy(constants[i], inverse, sizeof(constants[i]));
      } else if (mask & (1u << i)) {
         if (!map || size < (i + 1) * sizeof(constants[i]))
            return false;
         memcpy(constants[i], map + i * sizeof(constants[i]),
                sizeof(constants[i]));
      }
      chrome9_finite_constants(constants[i], 4);
   }
   return true;
}

/* State of job j of the split batch: carriers are job-local samplers
 * carrier_base.., the last job draws into the application target. */
static bool
chrome9_split_job_state(
   struct chrome9_vbuf_render *crender, unsigned j,
   struct drm_chrome9_draw_fragment_program *draw,
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS],
   uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4],
   struct drm_chrome9_fragment_texture_state
      texture_state[DRM_CHROME9_FRAGMENT_MAX_TEXTURES])
{
   struct chrome9_context *ctx = crender->ctx;
   const struct chrome9_fs_split *split = crender->split;
   const struct chrome9_fs_split_job *job = &split->jobs[j];
   const struct chrome9_shader *program = job->program;
   struct pipe_resource **carriers =
      util_dynarray_begin(&ctx->split_carriers);
   const bool final = job->carrier < 0;
   const uint8_t used = program->semantic_sampler_mask;
   const bool dense = used == BITFIELD_MASK(util_last_bit(used));
   const unsigned units = dense ? BITFIELD_MASK(util_last_bit(used)) : used;
   uint8_t depth_swizzle[DRM_CHROME9_FRAGMENT_MAX_TEXTURES][4] = { 0 };
   uint8_t depth_sampler_mask = 0;
   const struct chrome9_ps_ir *ir = &program->semantic_fs_ir;
   struct chrome9_ps_ir color_only;
   unsigned instruction_count, constant_count, descriptors = 0;

   if (!dense && !ctx->fragment_sparse_textures)
      return false;
   if (ir->depth_output && (!final || !crender->generic_depth_active)) {
      color_only = *ir;
      color_only.depth_output = false;
      ir = &color_only;
   }
   /* Fast-Z stored depths are not the shader's to replace. */
   if (ir->depth_output && crender->generic_depth->fast_z_index) {
      chrome9_resource_fast_z_resolve(crender->generic_depth);
      crender->generic_depth->fast_z_disabled = true;
   }
   memset(draw, 0, sizeof(*draw));
   memset(texture_state, 0,
          DRM_CHROME9_FRAGMENT_MAX_TEXTURES * sizeof(*texture_state));
   if (!chrome9_fragment_build_instructions(ir, used, instructions,
                                            &instruction_count) ||
       !chrome9_split_constants(ctx, split, program, ctx->split_carrier_width,
                                ctx->split_carrier_height, constants,
                                &constant_count))
      return false;
   u_foreach_bit(unit, units) {
      if (unit >= split->carrier_base) {
         const unsigned read = unit - split->carrier_base;

         if (read >= job->read_count)
            return false;
         chrome9_split_carrier_texture(
            chrome9_resource(carriers[job->reads[read]]), unit, !dense,
            &texture_state[descriptors++]);
         continue;
      }

      struct chrome9_resource *texture = crender->split_textures[unit];
      const struct pipe_sampler_view *view = ctx->sampler_views[unit];

      if (!texture || !crender->split_samplers[unit] || !view)
         return false;
      if (texture->base.format == PIPE_FORMAT_DXT1_RGB) {
         static const uint8_t opaque[4] = {
            DRM_CHROME9_FRAGMENT_SWIZZLE_X, DRM_CHROME9_FRAGMENT_SWIZZLE_Y,
            DRM_CHROME9_FRAGMENT_SWIZZLE_Z, DRM_CHROME9_FRAGMENT_SWIZZLE_ONE,
         };

         depth_sampler_mask |= 1u << unit;
         memcpy(depth_swizzle[unit], opaque, sizeof(opaque));
      } else if (chrome9_format_is_d24(texture->base.format) &&
                 !chrome9_depth_compare(ctx, unit)) {
         depth_sampler_mask |= 1u << unit;
         if (!chrome9_depth_view_swizzle(view->swizzle_r,
                                         &depth_swizzle[unit][0]) ||
             !chrome9_depth_view_swizzle(view->swizzle_g,
                                         &depth_swizzle[unit][1]) ||
             !chrome9_depth_view_swizzle(view->swizzle_b,
                                         &depth_swizzle[unit][2]) ||
             !chrome9_depth_view_swizzle(view->swizzle_a,
                                         &depth_swizzle[unit][3]))
            return false;
      }
      if (!chrome9_fragment_texture_descriptor(
             ctx, unit, !dense, texture, crender->split_samplers[unit],
             &texture_state[descriptors++]))
         return false;
   }
   if (depth_sampler_mask &&
       !chrome9_fragment_apply_depth_swizzles(instructions, instruction_count,
                                               depth_sampler_mask,
                                               depth_swizzle))
      return false;
   if (final) {
      if (!chrome9_fragment_draw_target(crender, crender->generic_target,
                                        crender->generic_depth,
                                        crender->generic_depth_active,
                                        crender->split_blend_active,
                                        &crender->split_blend,
                                        &crender->split_scissor,
                                        crender->split_depth_flags, draw))
         return false;
      draw->stencil = crender->split_stencil;
   } else {
      struct chrome9_resource *carrier =
         chrome9_resource(carriers[job->carrier]);

      /* No depth, stencil, blend or query: only the last job counts. */
      draw->destination_handle = carrier->bo.handle;
      draw->destination_pitch = carrier->stride;
      draw->destination_format = DRM_CHROME9_DESTINATION_FORMAT_R32F;
      draw->width = carrier->base.width0;
      draw->height = carrier->base.height0;
      draw->varying_count = crender->varying_count;
      draw->scissor = crender->split_scissor;
   }
   draw->instruction_count = instruction_count;
   draw->constant_count = constant_count;
   draw->texture_count = descriptors;
   draw->abi_version = dense ? DRM_CHROME9_FRAGMENT_ABI_VERSION :
                               DRM_CHROME9_FRAGMENT_ABI_SPARSE_TEXTURES;
   return true;
}

/* Run every job over the queued triangles. */
static void
chrome9_split_flush(struct chrome9_vbuf_render *crender)
{
   struct chrome9_context *ctx = crender->ctx;
   const struct chrome9_fs_split *split = crender->split;
   const unsigned count = crender->split_count;
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   struct drm_chrome9_fragment_instruction
      instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
   uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4];
   struct drm_chrome9_fragment_texture_state
      textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   bool submitted = true;

   if (!count)
      return;
   crender->split_count = 0;
   chrome9_tcl_batch_flush(ctx);
   if (!chrome9_split_carriers(ctx, split->carrier_count,
                               crender->generic_target->base.width0,
                               crender->generic_target->base.height0))
      submitted = false;
   for (unsigned j = 0; submitted && j < split->job_count; j++) {
      struct drm_chrome9_draw_fragment_program draw;

      if (!chrome9_split_job_state(crender, j, &draw, instructions, constants,
                                   textures)) {
         submitted = false;
         break;
      }
      draw.triangle_count = count;
      draw.vertices_ptr =
         (uintptr_t)util_dynarray_begin(&crender->split_vertices);
      draw.instructions_ptr = (uintptr_t)instructions;
      draw.constants_ptr = draw.constant_count ? (uintptr_t)constants : 0;
      draw.textures_ptr = draw.texture_count ? (uintptr_t)textures : 0;
      if (split->jobs[j].carrier < 0)
         chrome9_context_prepare_render_target(ctx);
      submitted = chrome9_draw_fragment_program(fd, &draw);
   }
   util_dynarray_clear(&crender->split_vertices);
   if (submitted) {
      ctx->submitted_triangles += count;
      ctx->submitted_batches++;
      ctx->submitted_program_batches++;
      chrome9_context_render_target_written(ctx);
   } else {
      CHROME9_REJECT(ctx, count);
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr, "chrome9: split DRAW_FRAGMENT_PROGRAM failed "
                 "count=%u: %s\n", count, strerror(errno));
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
   }
}

static int64_t
chrome9_edge(const int32_t a[2], const int32_t b[2], const int32_t p[2])
{
   return (int64_t)(b[0] - a[0]) * (p[1] - a[1]) -
          (int64_t)(b[1] - a[1]) * (p[0] - a[0]);
}

/* Some edge of t has all of other on its outer closed side. */
static bool
chrome9_separating_edge(const int32_t t[3][2], const int32_t other[3][2])
{
   const int64_t area = chrome9_edge(t[0], t[1], t[2]);

   for (unsigned e = 0; e < 3; e++) {
      const int32_t *a = t[e], *b = t[(e + 1) % 3];
      unsigned outside = 0;

      for (unsigned v = 0; v < 3; v++) {
         const int64_t side = chrome9_edge(a, b, other[v]);

         outside += area > 0 ? side <= 0 : side >= 0;
      }
      if (outside == 3)
         return true;
   }
   return false;
}

/* The interiors of two triangles overlap: a pixel could take both. */
static bool
chrome9_triangles_overlap(const struct drm_chrome9_fragment_vertex a[3],
                          const struct drm_chrome9_fragment_vertex b[3])
{
   int32_t ta[3][2], tb[3][2];

   for (unsigned i = 0; i < 3; i++) {
      ta[i][0] = a[i].x_16_16;
      ta[i][1] = a[i].y_16_16;
      tb[i][0] = b[i].x_16_16;
      tb[i][1] = b[i].y_16_16;
   }
   if (MAX3(ta[0][0], ta[1][0], ta[2][0]) <= MIN3(tb[0][0], tb[1][0], tb[2][0]) ||
       MAX3(tb[0][0], tb[1][0], tb[2][0]) <= MIN3(ta[0][0], ta[1][0], ta[2][0]) ||
       MAX3(ta[0][1], ta[1][1], ta[2][1]) <= MIN3(tb[0][1], tb[1][1], tb[2][1]) ||
       MAX3(tb[0][1], tb[1][1], tb[2][1]) <= MIN3(ta[0][1], ta[1][1], ta[2][1]))
      return false;
   return !chrome9_separating_edge(ta, tb) && !chrome9_separating_edge(tb, ta);
}

/* Queue a triangle of a split program; triangles whose interiors overlap
 * an earlier one of the batch start the next batch. */
static bool
chrome9_split_queue_triangle(
   struct chrome9_vbuf_render *crender, const unsigned indices[3],
   struct chrome9_resource *target, struct chrome9_resource *depth,
   struct chrome9_resource *textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
   struct chrome9_sampler_state *samplers[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
   bool depth_active, bool blend_active,
   const struct drm_chrome9_blend_state *blend,
   const struct drm_chrome9_scissor_state *scissor, uint32_t depth_flags,
   bool *queued)
{
   struct chrome9_context *ctx = crender->ctx;
   struct drm_chrome9_fragment_vertex vertices[3];
   struct drm_chrome9_stencil_state stencil = { 0 };
   const struct drm_chrome9_fragment_vertex *queued_vertices;
   int32_t x[3], y[3];

   *queued = false;
   if (crender->batch_count)
      chrome9_flush_triangle_batch(crender);
   if (!crender->generic_state_valid) {
      if (!blend || !scissor)
         return false;
      memset(crender->split_textures, 0, sizeof(crender->split_textures));
      memset(crender->split_samplers, 0, sizeof(crender->split_samplers));
      for (unsigned d = 0; d < crender->texture_count; d++) {
         const unsigned unit = chrome9_fragment_texture_unit(ctx, d);

         crender->split_textures[unit] = textures[d];
         crender->split_samplers[unit] = samplers[d];
      }
      crender->split = ctx->fs->split;
      crender->generic_target = target;
      crender->generic_depth = depth;
      crender->generic_depth_active = depth_active;
      crender->split_blend_active = blend_active;
      crender->split_blend = *blend;
      crender->split_scissor = *scissor;
      crender->split_depth_flags = depth_flags;
      crender->generic_state_valid = true;
   }
   if (!chrome9_fragment_vertices(crender, indices, crender->generic_target,
                                  vertices))
      return false;
   for (unsigned i = 0; i < 3; i++) {
      x[i] = vertices[i].x_16_16;
      y[i] = vertices[i].y_16_16;
   }
   if (!chrome9_triangle_nondegenerate(x, y))
      return true;
   if (crender->generic_depth_active && chrome9_stencil_active(ctx))
      chrome9_triangle_stencil(ctx, vertices, &stencil);
   if (crender->split_count &&
       memcmp(&stencil, &crender->split_stencil, sizeof(stencil)))
      chrome9_split_flush(crender);
   crender->split_stencil = stencil;
   queued_vertices = util_dynarray_begin(&crender->split_vertices);
   for (unsigned t = 0; t < crender->split_count; t++)
      if (chrome9_triangles_overlap(&queued_vertices[t * 3], vertices)) {
         chrome9_split_flush(crender);
         break;
      }
   util_dynarray_append_array(&crender->split_vertices,
                              struct drm_chrome9_fragment_vertex, vertices, 3);
   crender->split_count++;
   *queued = true;
   if (crender->split_count == DRM_CHROME9_FRAGMENT_MAX_TRIANGLES)
      chrome9_split_flush(crender);
   return true;
}

static bool
chrome9_queue_generic_triangle(
   struct chrome9_vbuf_render *crender, const unsigned indices[3],
   struct chrome9_resource *target, struct chrome9_resource *depth,
   struct chrome9_resource *textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
   struct chrome9_sampler_state *samplers[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
   bool depth_active, bool blend_active,
   const struct drm_chrome9_blend_state *blend,
   const struct drm_chrome9_scissor_state *scissor, uint32_t depth_flags,
   bool *queued)
{
   struct drm_chrome9_fragment_instruction *instructions =
      crender->generic_instructions;
   struct drm_chrome9_fragment_texture_state *texture_state =
      crender->generic_textures;
   struct drm_chrome9_fragment_vertex vertices[3] = { 0 };
   uint32_t (*constants)[4] = crender->generic_constants;
   struct drm_chrome9_draw_fragment_program draw;
   bool cached = crender->generic_state_valid;
   unsigned constant_count;
   unsigned instruction_count;

   if (crender->ctx->fs->split)
      return chrome9_split_queue_triangle(crender, indices, target, depth,
                                          textures, samplers, depth_active,
                                          blend_active, blend, scissor,
                                          depth_flags, queued);
   chrome9_split_flush(crender);
   *queued = false;
   /* The laboratory draw limit applies to cmd22 as well as the older
    * immediate paths.  Include deferred batches: none has been submitted
    * yet, but each already consumes one slot in this draw's limit. */
   if (crender->ctx->draw_limit &&
       crender->ctx->submitted_triangles + crender->deferred_triangles +
          crender->batch_count >= crender->ctx->draw_limit) {
      if (!crender->ctx->draw_limit_reported) {
         fprintf(stderr, "chrome9: stopped before triangle=%" PRIu64
                         " by CHROME9_DRAW_LIMIT=%" PRIu64 "\n",
                 crender->ctx->submitted_triangles +
                    crender->deferred_triangles + crender->batch_count,
                 crender->ctx->draw_limit);
         crender->ctx->draw_limit_reported = true;
      }
      return true;
   }
   crender->ctx->draw_filled_triangles =
      crender->prim == MESA_PRIM_TRIANGLES ||
      crender->prim == MESA_PRIM_TRIANGLE_STRIP ||
      crender->prim == MESA_PRIM_TRIANGLE_FAN;
   if (!cached) {
      if (!chrome9_generic_fragment_state(crender, target, depth, textures,
             samplers, depth_active, blend_active, blend, scissor,
             depth_flags, &crender->generic_draw, instructions, constants,
             texture_state))
         return false;
      crender->generic_target = target;
      crender->generic_depth = depth;
      crender->generic_depth_active = depth_active;
      crender->generic_state_valid = true;
   }
   draw = crender->generic_draw;
   instruction_count = draw.instruction_count;
   constant_count = draw.constant_count;
   if (!chrome9_fragment_vertices(crender, indices, target, vertices))
      return false;
   {
      int32_t x[3], y[3];

      for (unsigned i = 0; i < 3; i++) {
         x[i] = vertices[i].x_16_16;
         y[i] = vertices[i].y_16_16;
      }
      if (!chrome9_triangle_nondegenerate(x, y))
         return true;
   }
   if (depth_active && chrome9_stencil_active(crender->ctx))
      chrome9_triangle_stencil(crender->ctx, vertices, &draw.stencil);
   /* Within one call only the per-face stencil can differ. */
   if (crender->batch_count &&
       (!crender->batch_generic_program ||
        memcmp(&crender->batch_fragment, &draw, sizeof(draw)) ||
        (!cached &&
         (memcmp(crender->batch_fragment_instructions, instructions,
                 instruction_count * sizeof(instructions[0])) ||
          memcmp(crender->batch_fragment_constants, constants,
                 constant_count * sizeof(constants[0])) ||
          memcmp(crender->batch_fragment_textures, texture_state,
                 crender->texture_count * sizeof(texture_state[0]))))))
      chrome9_flush_triangle_batch(crender);
   if (crender->batch_failed)
      return false;
   if (!crender->batch_count) {
      crender->batch_fragment = draw;
      memcpy(crender->batch_fragment_instructions, instructions,
             instruction_count * sizeof(instructions[0]));
      memcpy(crender->batch_fragment_constants, constants,
             constant_count * sizeof(constants[0]));
      memcpy(crender->batch_fragment_textures, texture_state,
             crender->texture_count * sizeof(texture_state[0]));
      crender->batch_generic_program = true;
   }
   /* A signal handler that draws while a flush is still in its ioctl
    * re-enters with the batch full; fail closed instead of overrunning. */
   if (crender->batch_count >= CHROME9_FRAGMENT_BATCH_TRIANGLES)
      return false;
   memcpy(&crender->batch_fragment_vertices[crender->batch_count * 3],
          vertices, sizeof(vertices));
   crender->batch_count++;
   *queued = true;
   if (crender->batch_count == CHROME9_FRAGMENT_BATCH_TRIANGLES)
      chrome9_flush_triangle_batch(crender);
   return !crender->batch_failed;
}

static void
chrome9_u32_as_float(uint32_t bits, float *value)
{
   memcpy(value, &bits, sizeof(*value));
}

static bool
chrome9_q16_raster_edge(int32_t value, bool half_pixel_center,
                        uint32_t *integer)
{
   const int64_t unbiased = (int64_t)value +
      (half_pixel_center ? 0x8000 : 0);
   int64_t rounded;
   int64_t error;

   /* Undo the GL half-pixel bias, when present, before deciding whether a
    * pair of textured triangles is an exact integer copy rectangle. */
   if (unbiased < 0)
      return false;
   rounded = (unbiased + 0x8000) >> 16;
   error = unbiased - (rounded << 16);
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
   bool half_pixel_edges = false;

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

      if (!i) {
         if (!chrome9_q16_raster_edge(vertex->x_16_16, false, &dx[i]) ||
             !chrome9_q16_raster_edge(vertex->y_16_16, false, &dy[i])) {
            half_pixel_edges = true;
            if (!chrome9_q16_raster_edge(vertex->x_16_16, true, &dx[i]) ||
                !chrome9_q16_raster_edge(vertex->y_16_16, true, &dy[i]))
               return false;
         }
      } else if (!chrome9_q16_raster_edge(vertex->x_16_16,
                                           half_pixel_edges, &dx[i]) ||
                 !chrome9_q16_raster_edge(vertex->y_16_16,
                                           half_pixel_edges, &dy[i])) {
         return false;
      }
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

   chrome9_split_flush(crender);
   if (!count)
      return;
   /* Queued TCL draws precede these triangles. */
   chrome9_tcl_batch_flush(ctx);
   if (crender->defer_guarded_domain && crender->batch_generic_program) {
      struct chrome9_deferred_fragment_batch *batch = NULL;

      if (!crender->deferred_failed &&
          count <= CHROME9_DEFERRED_FRAGMENT_MAX_TRIANGLES -
                   crender->deferred_triangles)
         batch = CALLOC_STRUCT(chrome9_deferred_fragment_batch);
      if (batch) {
         batch->draw = crender->batch_fragment;
         batch->draw.triangle_count = count;
         memcpy(batch->instructions, crender->batch_fragment_instructions,
                sizeof(batch->instructions));
         memcpy(batch->constants, crender->batch_fragment_constants,
                sizeof(batch->constants));
         memcpy(batch->textures, crender->batch_fragment_textures,
                sizeof(batch->textures));
         memcpy(batch->vertices, crender->batch_fragment_vertices,
                count * 3 * sizeof(batch->vertices[0]));
         if (crender->deferred_tail)
            crender->deferred_tail->next = batch;
         else
            crender->deferred_head = batch;
         crender->deferred_tail = batch;
         crender->deferred_triangles += count;
      } else {
         crender->deferred_failed = true;
         crender->batch_failed = true;
         CHROME9_REJECT(crender->ctx, count);
      }
      crender->batch_count = 0;
      crender->batch_generic_program = false;
      return;
   }
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);
   bool submitted;
   bool direct_external = false;

   if (crender->batch_generic_program) {
      struct drm_chrome9_draw_fragment_program submit =
         crender->batch_fragment;

      chrome9_context_prepare_render_target(ctx);
      submit.triangle_count = count;
      submit.vertices_ptr = (uintptr_t)crender->batch_fragment_vertices;
      submit.instructions_ptr =
         (uintptr_t)crender->batch_fragment_instructions;
      submit.constants_ptr = submit.constant_count ?
         (uintptr_t)crender->batch_fragment_constants : 0;
      submit.textures_ptr = submit.texture_count ?
         (uintptr_t)crender->batch_fragment_textures : 0;
      submitted = chrome9_draw_fragment_program(fd, &submit);
   } else if (crender->batch_textured && crender->batch_textured_mip) {
      struct drm_chrome9_draw_textured_triangle_mip submit =
         crender->batch_textured_mip_state;

      chrome9_context_prepare_render_target(ctx);
      submit.triangle_count = count;
      submit.vertices_ptr =
         (uintptr_t)crender->batch_textured_run_vertices;
      submitted = chrome9_draw_textured_triangle_mip(fd, &submit);
   } else if (crender->batch_textured && crender->batch_textured_run) {
      struct drm_chrome9_draw_textured_triangle_run submit =
         crender->batch_textured_run_state;

      chrome9_context_prepare_render_target(ctx);
      submit.triangle_count = count;
      submit.vertices_ptr =
         (uintptr_t)crender->batch_textured_run_vertices;
      submitted = chrome9_draw_textured_triangle_run(fd, &submit);
   } else if (crender->batch_textured) {
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
            /* The opt-in is restricted to a verified opaque depth-24 root:
             * PIPE_BIND_SCANOUT alone cannot distinguish it from a logical
             * ARGB scanout.  Preserve the root's unused X byte and copy RGB
             * in one H6 pass instead of repainting alpha afterward. */
            if (ctx->copy_rect_preserve_alpha &&
                (copy.flags & DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE)) {
               copy.flags &= ~DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE;
               copy.flags |=
                  DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA;
            }
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
   } else if (crender->batch_large_chain) {
      chrome9_context_prepare_render_target(ctx);
      crender->batch_rhw_chain.triangle_count = count;
      crender->batch_rhw_chain.vertices_ptr =
         (uintptr_t)crender->batch_vertices_rhw;
      submitted = chrome9_draw_triangle_d16_rhw_chain(
         fd, &crender->batch_rhw_chain);
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
      if (crender->batch_compiled_program || crender->batch_generic_program)
         ctx->submitted_program_batches++;
      if (crender->batch_large_run)
         ctx->submitted_rhw_runs++;
      if (crender->batch_large_chain)
         ctx->submitted_rhw_chains++;
      if (direct_external)
         chrome9_context_render_target_direct_written(ctx);
      else
         chrome9_context_render_target_written(ctx);
      crender->batch_failed = false;
   } else {
      CHROME9_REJECT(ctx, count);
      crender->batch_failed = true;
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr,
                 "chrome9: DRAW_%sTRIANGLE%s_BATCH failed after=%" PRIu64
                 " count=%u: %s\n",
                 crender->batch_generic_program ? "FRAGMENT_PROGRAM_" :
                 crender->batch_textured_mip ? "TEXTURED_MIP_" :
                 crender->batch_textured_run ? "TEXTURED_RUN_" :
                 crender->batch_textured ? "TEXTURED_" :
                 crender->batch_compiled_program ? "PROGRAM_" :
                 crender->batch_large_chain ? "RHW_CHAIN_" :
                 crender->batch_large_run ? "RHW_RUN_" : "",
                 crender->batch_perspective ? "_D16_RHW" : "",
                 ctx->submitted_triangles, count, strerror(errno));
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
   }
   crender->batch_count = 0;
   crender->batch_textured = false;
   crender->batch_textured_run = false;
   crender->batch_textured_mip = false;
   crender->batch_perspective = false;
   crender->batch_large_run = false;
   crender->batch_large_chain = false;
   crender->batch_compiled_program = false;
   crender->batch_generic_program = false;
}

static void
chrome9_emit_triangle_state(struct chrome9_vbuf_render *crender,
                            unsigned ia, unsigned ib, unsigned ic,
                            unsigned provoking);

/* Later triangles of a generic draw call reuse its validated state. */
static void
chrome9_emit_triangle(struct chrome9_vbuf_render *crender,
                      unsigned ia, unsigned ib, unsigned ic,
                      unsigned provoking)
{
   const unsigned indices[3] = { ia, ib, ic };
   struct chrome9_context *ctx = crender->ctx;
   bool queued;

   if (!crender->generic_program || !crender->generic_state_valid ||
       crender->batch_failed) {
      chrome9_emit_triangle_state(crender, ia, ib, ic, provoking);
      return;
   }
   if (!chrome9_queue_generic_triangle(crender, indices,
          crender->generic_target, crender->generic_depth, NULL, NULL,
          crender->generic_depth_active, false, NULL, NULL, 0, &queued)) {
      if (!queued)
         CHROME9_REJECT(ctx, 1);
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr, "chrome9: DRAW_FRAGMENT_PROGRAM failed after=%"
                 PRIu64 ": %s\n", ctx->submitted_triangles, strerror(errno));
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
   }
}

static void
chrome9_emit_triangle_state(struct chrome9_vbuf_render *crender,
                            unsigned ia, unsigned ib, unsigned ic,
                            unsigned provoking)
{
   struct chrome9_context *ctx = crender->ctx;
   const float pixel_center_bias =
      ctx->rasterizer && ctx->rasterizer->half_pixel_center ? 0.5f : 0.0f;
   struct chrome9_resource *target = NULL;
   struct chrome9_resource *depth = NULL;
   struct chrome9_resource
      *textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   struct chrome9_sampler_state
      *samplers[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
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
   uint32_t flat_color = 0;
   const char *state_reason = "vertex-layout";
   bool blend_successor = false;
   bool depth_active = false;
   bool packed_lightmap_tex0_alpha = false;
   bool perspective_batch;
   bool rhw_run_batch = false;
   bool rhw_chain_batch = false;
   bool program_batch = false;
   const bool two_projected_primary = ctx->fs &&
      (ctx->fs->fragment_program ==
          CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA ||
       ctx->fs->fragment_program ==
          CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY);
   uint32_t depth_flags = 0;
   const bool flat_primary = ctx->fs && ctx->fs->flat_primary &&
      ctx->fs->fragment_program == CHROME9_FS_PRIMARY_COLOR;
   const bool flat_generic = ctx->fs && ctx->fs->flat_generic &&
      ctx->fs->fragment_program == CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX;

   if (((!crender->textured &&
         (ctx->draw_triangle_batch ||
          ctx->draw_program_triangle_d16_rhw_batch)) ||
        (crender->textured &&
         (ctx->draw_textured_triangle_batch ||
          ctx->draw_textured_triangle_run ||
          ctx->draw_textured_triangle_mip))) &&
       crender->batch_failed) {
      CHROME9_REJECT(ctx, 1);
      return;
   }
   if (!crender->generic_program && crender->batch_generic_program) {
      chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
   }
   if (!crender->layout_valid ||
       !chrome9_draw_state_supported(ctx, &target, &depth, textures, samplers,
                                     crender->texture_count, &depth_active,
                                     &blend_state, &blend_successor,
                                     &state_reason)) {
      CHROME9_REJECT(ctx, 1);
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
       ctx->draw_triangle_d16_rhw_run ||
       ctx->draw_triangle_d16_rhw_chain);
   depth_flags = chrome9_draw_depth_flags(ctx, depth_active);
   if (chrome9_fs_uses_constant_color(ctx->fs->fragment_program) &&
       !chrome9_fragment_constant_color(ctx, &constant_color)) {
      CHROME9_REJECT(ctx, 1);
      return;
   }
   program_batch = perspective_batch &&
      ctx->draw_program_triangle_d16_rhw_batch &&
      /* The fixed primary path and compiler MOV v0 are semantically
       * identical.  Keep both smooth and software-flat primary draws in the
       * larger RHW run so state changes can coalesce across GL draw calls;
       * reserve cmd19 for programs which actually need uploaded ALU state. */
      ctx->fs->fragment_program != CHROME9_FS_PRIMARY_COLOR &&
      chrome9_program_instruction(ctx->fs, ctx->fragment_add,
                                  &program_instruction) &&
      (!(ctx->fs->native_fs.constant_mask & 1) ||
       chrome9_fragment_constant_c0(ctx, program_constant_c0));
   rhw_chain_batch = perspective_batch &&
      ctx->draw_triangle_d16_rhw_chain && !program_batch;
   rhw_run_batch = perspective_batch && !rhw_chain_batch &&
      ctx->draw_triangle_d16_rhw_run && !program_batch;
   if (ctx->fs->fragment_program == CHROME9_FS_COMPILED_ALU &&
       !program_batch && !crender->generic_program) {
      CHROME9_REJECT(ctx, 1);
      return;
   }
   chrome9_draw_scissor(ctx, target, &scissor);
   /* The fixed draw builders leave Fast-Z off. */
   if (depth_active && depth->fast_z_index && !crender->generic_program) {
      chrome9_resource_fast_z_resolve(depth);
      depth->fast_z_disabled = true;
   }
   if (crender->generic_program) {
      bool queued;

      if (crender->batch_failed) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      if (chrome9_queue_generic_triangle(
             crender, indices, target, depth, textures, samplers,
             depth_active, blend_successor, &blend_state, &scissor,
             depth_flags, &queued)) {
         /* Submission and accounting happen when the batch is flushed. */
      } else {
         /* A failed flush has already counted every queued triangle. */
         if (!queued)
            CHROME9_REJECT(ctx, 1);
         if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
            fprintf(stderr,
                    "chrome9: DRAW_FRAGMENT_PROGRAM failed after=%" PRIu64
                    ": %s\n", ctx->submitted_triangles, strerror(errno));
            ctx->warnings |= CHROME9_WARN_SUBMIT;
         }
      }
      return;
   }
   if (flat_primary) {
      if (provoking >= crender->vertex_count ||
          (size_t)(provoking + 1) * crender->vertex_size >
             crender->capacity) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      memcpy(&flat_color,
             crender->vertices + (size_t)provoking * crender->vertex_size +
                4 * sizeof(float),
             sizeof(flat_color));
   }
   if (flat_generic) {
      float gray_float;
      unsigned gray;

      if (provoking >= crender->vertex_count ||
          (size_t)(provoking + 1) * crender->vertex_size >
             crender->capacity) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      memcpy(&gray_float,
             crender->vertices + (size_t)provoking * crender->vertex_size +
                4 * sizeof(float),
             sizeof(gray_float));
      if (!isfinite(gray_float)) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      gray = float_to_ubyte(gray_float);
      flat_color = gray | gray << 8 | gray << 16 | 0xff000000;
   }
   for (unsigned i = 0; i < 3; i++) {
      const uint8_t *vertex;
      const float *position;
      const float *packed = NULL;
      uint32_t color;

      if (indices[i] >= crender->vertex_count ||
          (size_t)(indices[i] + 1) * crender->vertex_size >
          crender->capacity) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      vertex = crender->vertices + (size_t)indices[i] * crender->vertex_size;
      position = (const float *)vertex;
      if (!isfinite(position[0]) || !isfinite(position[1]) ||
          (depth_active && !isfinite(position[2])) ||
          (perspective_batch &&
           (!isfinite(position[3]) || position[3] <= 0.0f))) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      if (crender->packed_generic) {
         packed = (const float *)(vertex + 4 * sizeof(float));
         if (crender->packed_generic >= 6) {
            const unsigned components = crender->packed_generic == 7 ? 8 : 6;

            for (unsigned c = 0; c < components; c++)
               if (!isfinite(packed[c])) {
                  CHROME9_REJECT(ctx, 1);
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
            const bool xxy = ctx->fs->fragment_program ==
               CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX;
            unsigned components = crender->packed_generic == 3 ? 4 :
                                  xxy ? 2 : 3;

            for (unsigned c = 0; c < components; c++)
               if (!isfinite(packed[c])) {
                  CHROME9_REJECT(ctx, 1);
                  return;
               }
            color = (uint32_t)float_to_ubyte(packed[xxy ? 1 : 2]) |
                    (uint32_t)float_to_ubyte(packed[xxy ? 0 : 1]) << 8 |
                    (uint32_t)float_to_ubyte(packed[0]) << 16 |
                    (crender->packed_generic == 3 ?
                     (uint32_t)float_to_ubyte(packed[3]) << 24 :
                     0xff000000);
         } else {
            unsigned gray;

            if (!isfinite(packed[0])) {
               CHROME9_REJECT(ctx, 1);
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
      if (flat_primary || flat_generic)
         color = flat_color;
      if (chrome9_fs_uses_constant_color(ctx->fs->fragment_program))
         color = constant_color;
      if (ctx->fs->fragment_program == CHROME9_FS_LITERAL_COLOR)
         color = ctx->fs->literal_color;
      if (ctx->fs->fragment_program ==
             CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA &&
          color != 0xffffffff) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      /* Chrome9 inherits D3D9 integer pixel centers.  Gallium requests GL
       * half-integer centers explicitly, so translate post-viewport XY while
       * leaving texture coordinates and every other varying untouched. */
      x[i] = chrome9_coord_16_16(position[0] - pixel_center_bias,
                                 target->base.width0);
      y[i] = chrome9_coord_16_16(position[1] - pixel_center_bias,
                                 target->base.height0);
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
            CHROME9_REJECT(ctx, 1);
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
               CHROME9_REJECT(ctx, 1);
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
   if (crender->textured && crender->texture_count == 1 &&
       samplers[0]->mip_filter != DRM_CHROME9_TEXTURE_MIP_FILTER_NONE) {
      const bool mip_blend = blend_successor ||
         (depth_active && chrome9_blend_is_source_over(&blend_state));
      struct pipe_sampler_view *view = ctx->sampler_views[0];
      struct drm_chrome9_draw_textured_triangle_mip next = {
         .destination_handle = texture_args.destination_handle,
         .depth_handle = depth_active ? depth->bo.handle : 0,
         .flags = depth_active ?
            DRM_CHROME9_TEXTURE_RUN_DEPTH | depth_flags : 0,
         .program = texture_args.program,
         .width = texture_args.width,
         .height = texture_args.height,
         .destination_pitch = texture_args.destination_pitch,
         .depth_pitch = depth_active ? depth->stride : 0,
         .texture = {
            .handle = texture_args.texture_handle,
            .width = texture_args.texture_width,
            .height = texture_args.texture_height,
            .pitch = texture_args.texture_pitch,
            .format = texture_args.texture_format,
            .min_filter = texture_args.min_filter,
            .mag_filter = texture_args.mag_filter,
            .wrap_s = texture_args.wrap_s,
            .wrap_t = texture_args.wrap_t,
         },
         .scissor = texture_args.scissor,
         .texture_last_level = view->u.tex.last_level,
         .mip_filter = samplers[0]->mip_filter,
         .lod_bias_s5_5 = lroundf(samplers[0]->base.lod_bias * 32.0f),
      };

      if (mip_blend) {
         next.flags |= DRM_CHROME9_TEXTURE_RUN_BLEND;
         next.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
         next.blend = blend_state;
      }
      if (crender->batch_count &&
          (!crender->batch_textured || !crender->batch_textured_mip ||
           memcmp(&crender->batch_textured_mip_state, &next, sizeof(next))))
         chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      if (!crender->batch_count) {
         crender->batch_textured = true;
         crender->batch_textured_run = false;
         crender->batch_textured_mip = true;
         crender->batch_perspective = false;
         crender->batch_large_run = false;
         crender->batch_large_chain = false;
         crender->batch_compiled_program = false;
         crender->batch_textured_mip_state = next;
      }
      const unsigned base = crender->batch_count * 3;
      for (unsigned i = 0; i < 3; i++)
         crender->batch_textured_run_vertices[base + i] =
            texture_args.vertices[i];
      crender->batch_count++;
      if (crender->batch_count == DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES)
         chrome9_flush_triangle_batch(crender);
      return;
   }
   if (crender->textured && crender->texture_count == 1 &&
       ctx->draw_textured_triangle_run &&
       /* Preserve the no-depth batch's COPY_RECT recognition.  The run is
        * the D16 batching successor; use it without depth only when the
        * predecessor batch is unavailable. */
       (depth_active || !ctx->draw_textured_triangle_batch)) {
      const bool run_blend = blend_successor ||
         (depth_active && chrome9_blend_is_source_over(&blend_state));
      struct drm_chrome9_draw_textured_triangle_run next = {
         .destination_handle = texture_args.destination_handle,
         .depth_handle = depth_active ? depth->bo.handle : 0,
         .flags = depth_active ?
            DRM_CHROME9_TEXTURE_RUN_DEPTH | depth_flags : 0,
         .program = texture_args.program,
         .width = texture_args.width,
         .height = texture_args.height,
         .destination_pitch = texture_args.destination_pitch,
         .depth_pitch = depth_active ? depth->stride : 0,
         .texture = {
            .handle = texture_args.texture_handle,
            .width = texture_args.texture_width,
            .height = texture_args.texture_height,
            .pitch = texture_args.texture_pitch,
            .format = texture_args.texture_format,
            .min_filter = texture_args.min_filter,
            .mag_filter = texture_args.mag_filter,
            .wrap_s = texture_args.wrap_s,
            .wrap_t = texture_args.wrap_t,
         },
         .scissor = texture_args.scissor,
      };

      if (run_blend) {
         next.flags |= DRM_CHROME9_TEXTURE_RUN_BLEND;
         next.destination_format =
            target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
            DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
            DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
         next.blend = blend_state;
      }
      if (crender->batch_count &&
          (!crender->batch_textured || !crender->batch_textured_run ||
           memcmp(&crender->batch_textured_run_state, &next, sizeof(next))))
         chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      if (!crender->batch_count) {
         crender->batch_textured = true;
         crender->batch_textured_run = true;
         crender->batch_textured_mip = false;
         crender->batch_perspective = false;
         crender->batch_large_run = false;
         crender->batch_large_chain = false;
         crender->batch_compiled_program = false;
         crender->batch_textured_run_state = next;
      }
      const unsigned base = crender->batch_count * 3;
      for (unsigned i = 0; i < 3; i++)
         crender->batch_textured_run_vertices[base + i] =
            texture_args.vertices[i];
      crender->batch_count++;
      if (crender->batch_count == DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES)
         chrome9_flush_triangle_batch(crender);
      return;
   }
   /*
    * Keep source-over and other blended texture draws on the independently
    * retired one-triangle command.  A live ioquake3 UI reached a valid
    * 16-triangle MODULATE+blend packet but VX900 timed out after the fetch
    * cursor reached the post-draw H2 boundary and before the cookie.
    * Non-blended cmd1a batching remains proven and is still used for Present
    * and ordinary opaque texture work.
    */
   if (crender->textured && crender->texture_count == 1 && !depth_active &&
       !blend_successor && ctx->draw_textured_triangle_batch) {
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
          (!crender->batch_textured || crender->batch_textured_run ||
           memcmp(&crender->batch_textured_state, &next, sizeof(next))))
         chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      if (!crender->batch_count) {
         crender->batch_textured = true;
         crender->batch_textured_run = false;
         crender->batch_textured_mip = false;
         crender->batch_perspective = false;
         crender->batch_large_run = false;
         crender->batch_large_chain = false;
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
       (ctx->draw_triangle_batch || program_batch || rhw_run_batch ||
        rhw_chain_batch)) {
      struct drm_chrome9_draw_triangle_batch next = { 0 };
      struct drm_chrome9_draw_triangle_d16_rhw_batch next_rhw = { 0 };
      struct drm_chrome9_draw_triangle_d16_rhw_run next_rhw_run = { 0 };
      struct drm_chrome9_draw_triangle_d16_rhw_chain next_rhw_chain = { 0 };
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
         } else if (rhw_chain_batch) {
            next_rhw_chain.destination_handle = next_rhw.destination_handle;
            next_rhw_chain.depth_handle = next_rhw.depth_handle;
            next_rhw_chain.flags = next_rhw.flags;
            next_rhw_chain.width = next_rhw.width;
            next_rhw_chain.height = next_rhw.height;
            next_rhw_chain.destination_pitch = next_rhw.destination_pitch;
            next_rhw_chain.depth_pitch = next_rhw.depth_pitch;
            next_rhw_chain.destination_format = next_rhw.destination_format;
            next_rhw_chain.blend = next_rhw.blend;
            next_rhw_chain.scissor = next_rhw.scissor;
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
          (crender->batch_textured || crender->batch_generic_program ||
           crender->batch_perspective != perspective_batch ||
           crender->batch_large_run != rhw_run_batch ||
           crender->batch_large_chain != rhw_chain_batch ||
           crender->batch_compiled_program != program_batch ||
           (program_batch ?
              memcmp(&crender->batch_program, &next_program,
                     sizeof(next_program)) : rhw_chain_batch ?
              memcmp(&crender->batch_rhw_chain, &next_rhw_chain,
                     sizeof(next_rhw_chain)) : rhw_run_batch ?
              memcmp(&crender->batch_rhw_run, &next_rhw_run,
                     sizeof(next_rhw_run)) : perspective_batch ?
              memcmp(&crender->batch_rhw, &next_rhw, sizeof(next_rhw)) :
              memcmp(&crender->batch, &next, sizeof(next)))))
         chrome9_flush_triangle_batch(crender);
      if (crender->batch_failed) {
         CHROME9_REJECT(ctx, 1);
         return;
      }
      if (!crender->batch_count) {
         crender->batch_textured = false;
         crender->batch_textured_run = false;
         crender->batch_textured_mip = false;
         crender->batch_perspective = perspective_batch;
         crender->batch_large_run = rhw_run_batch;
         crender->batch_large_chain = rhw_chain_batch;
         crender->batch_compiled_program = program_batch;
         if (program_batch)
            crender->batch_program = next_program;
         else if (rhw_chain_batch)
            crender->batch_rhw_chain = next_rhw_chain;
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
          (rhw_chain_batch ? DRM_CHROME9_TRIANGLE_CHAIN_MAX_TRIANGLES :
           (rhw_run_batch || program_batch) ?
                           DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES :
                           DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES))
         chrome9_flush_triangle_batch(crender);
      return;
   }
   /* Preserve API ordering when a non-batch draw follows retained triangles
    * from earlier Gallium draws. */
   chrome9_flush_triangle_batch(crender);
   if (crender->batch_failed) {
      CHROME9_REJECT(ctx, 1);
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
      CHROME9_REJECT(ctx, 1);
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr,
                 "chrome9: DRAW_%sTRIANGLE%s failed after=%" PRIu64
                 ": %s target=%ux%u/%u flags=%#x format=%u program=%u "
                 "filter=%u/%u wrap=%u/%u blend=%#x/%u/%u/%u/%u/%#x "
                 "xy=(%d,%d)(%d,%d)(%d,%d)\n",
                 crender->textured ? "TEXTURED_" : "",
                 depth_active ? "_D16" : "", ctx->submitted_triangles,
                 strerror(errno), target->base.width0, target->base.height0,
                 target->stride, depth_flags,
                 target->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
                    DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
                    DRM_CHROME9_DESTINATION_FORMAT_BGRX8888,
                 crender->textured ? texture_args.program : 0,
                 crender->textured ? texture_args.min_filter : 0,
                 crender->textured ? texture_args.mag_filter : 0,
                 crender->textured ? texture_args.wrap_s : 0,
                 crender->textured ? texture_args.wrap_t : 0,
                 blend_state.flags, blend_state.rgb_src_factor,
                 blend_state.rgb_dst_factor, blend_state.alpha_src_factor,
                 blend_state.alpha_dst_factor, blend_state.color_mask,
                 x[0], y[0], x[1], y[1], x[2], y[2]);
         if (crender->textured)
            for (unsigned i = 0; i < 3; i++)
               fprintf(stderr,
                       "chrome9: failed textured vertex[%u] "
                       "z=%d rhw=%08x color=%08x specular=%08x "
                       "st=%08x/%08x\n", i,
                       texture_args.vertices[i].z_16_16,
                       texture_args.vertices[i].rhw_float,
                       texture_args.vertices[i].color,
                       texture_args.vertices[i].specular,
                       texture_args.vertices[i].s_float,
                       texture_args.vertices[i].t_float);
         if (crender->textured)
            fprintf(stderr,
                    "chrome9: failed objects target=%u/%" PRIu64
                    " texture=%u/%" PRIu64 " depth=%u/%" PRIu64
                    " texture-size=%ux%u/%u depth-pitch=%u "
                    "scissor=%#x/%u,%u-%u,%u\n",
                    target->bo.handle, target->bo.size,
                    textures[0]->bo.handle, textures[0]->bo.size,
                    depth ? depth->bo.handle : 0,
                    depth ? depth->bo.size : 0,
                    textures[0]->base.width0, textures[0]->base.height0,
                    textures[0]->stride, depth ? depth->stride : 0,
                    scissor.flags, scissor.min_x, scissor.min_y,
                    scissor.max_x, scissor.max_y);
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
   }
}

static void
chrome9_vbuf_draw_arrays(struct vbuf_render *render, unsigned start,
                         unsigned nr)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   const bool first = crender->ctx->rasterizer &&
      crender->ctx->rasterizer->flatshade_first;
   unsigned end;

   crender->generic_state_valid = false;
   if (start > crender->vertex_count || nr > crender->vertex_count - start)
      return;
   if (!chrome9_guarded_draw_domain(crender, NULL, start, nr)) {
      CHROME9_REJECT(crender->ctx, 1);
      return;
   }
   end = start + nr;
   switch (crender->prim) {
   case MESA_PRIM_TRIANGLES:
      for (unsigned i = start; i + 2 < end; i += 3)
         chrome9_emit_triangle(crender, i, i + 1, i + 2,
                               first ? i : i + 2);
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      for (unsigned i = start; i + 2 < end; i++)
         if ((i - start) & 1)
            chrome9_emit_triangle(crender, i + 1, i, i + 2,
                                  first ? i : i + 2);
         else
            chrome9_emit_triangle(crender, i, i + 1, i + 2,
                                  first ? i : i + 2);
      break;
   case MESA_PRIM_TRIANGLE_FAN:
      for (unsigned i = start + 1; i + 1 < end; i++)
         chrome9_emit_triangle(crender, start, i, i + 1,
                               first ? i : i + 1);
      break;
   case MESA_PRIM_POLYGON:
      for (unsigned i = start + 1; i + 1 < end; i++)
         chrome9_emit_triangle(crender, start, i, i + 1, start);
      break;
   case MESA_PRIM_QUADS:
      for (unsigned i = start; i + 3 < end; i += 4) {
         chrome9_emit_triangle(crender, i, i + 1, i + 3, i + 3);
         chrome9_emit_triangle(crender, i + 1, i + 2, i + 3, i + 3);
      }
      break;
   case MESA_PRIM_QUAD_STRIP:
      for (unsigned i = start; i + 3 < end; i += 2) {
         chrome9_emit_triangle(crender, i, i + 1, i + 3, i + 3);
         chrome9_emit_triangle(crender, i, i + 3, i + 2, i + 3);
      }
      break;
   default:
      CHROME9_REJECT(crender->ctx, 1);
      break;
   }
   if (crender->batch_generic_program)
      chrome9_flush_triangle_batch(crender);
   chrome9_split_flush(crender);
}

static void
chrome9_vbuf_draw_elements(struct vbuf_render *render,
                           const uint16_t *indices, unsigned nr_indices)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   const bool first = crender->ctx->rasterizer &&
      crender->ctx->rasterizer->flatshade_first;

   crender->generic_state_valid = false;
   if (!indices)
      return;
   if (!chrome9_guarded_draw_domain(crender, indices, 0, nr_indices)) {
      CHROME9_REJECT(crender->ctx, 1);
      return;
   }
   switch (crender->prim) {
   case MESA_PRIM_TRIANGLES:
      for (unsigned i = 0; i + 2 < nr_indices; i += 3)
         chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                               indices[i + 2],
                               first ? indices[i] : indices[i + 2]);
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      for (unsigned i = 0; i + 2 < nr_indices; i++)
         if (i & 1)
            chrome9_emit_triangle(crender, indices[i + 1], indices[i],
                                  indices[i + 2],
                                  first ? indices[i] : indices[i + 2]);
         else
            chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                                  indices[i + 2],
                                  first ? indices[i] : indices[i + 2]);
      break;
   case MESA_PRIM_TRIANGLE_FAN:
      for (unsigned i = 1; i + 1 < nr_indices; i++)
         chrome9_emit_triangle(crender, indices[0], indices[i],
                               indices[i + 1],
                               first ? indices[i] : indices[i + 1]);
      break;
   case MESA_PRIM_POLYGON:
      for (unsigned i = 1; i + 1 < nr_indices; i++)
         chrome9_emit_triangle(crender, indices[0], indices[i],
                               indices[i + 1], indices[0]);
      break;
   case MESA_PRIM_QUADS:
      for (unsigned i = 0; i + 3 < nr_indices; i += 4) {
         chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                               indices[i + 3], indices[i + 3]);
         chrome9_emit_triangle(crender, indices[i + 1], indices[i + 2],
                               indices[i + 3], indices[i + 3]);
      }
      break;
   case MESA_PRIM_QUAD_STRIP:
      for (unsigned i = 0; i + 3 < nr_indices; i += 2) {
         chrome9_emit_triangle(crender, indices[i], indices[i + 1],
                               indices[i + 3], indices[i + 3]);
         chrome9_emit_triangle(crender, indices[i], indices[i + 3],
                               indices[i + 2], indices[i + 3]);
      }
      break;
   default:
      CHROME9_REJECT(crender->ctx, 1);
      break;
   }
   /* Shader and sampler bindings can change immediately after this call.
    * Do not retain generic BO handles across the Gallium draw boundary. */
   if (crender->batch_generic_program)
      chrome9_flush_triangle_batch(crender);
   chrome9_split_flush(crender);
}

static void
chrome9_vbuf_release_vertices(struct vbuf_render *render)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);

   crender->capacity = 0;
   crender->vertex_count = 0;
}

static void
chrome9_vbuf_destroy(struct vbuf_render *render)
{
   struct chrome9_vbuf_render *crender = chrome9_vbuf_render(render);
   struct chrome9_deferred_fragment_batch *batch = crender->deferred_head;

   if (!crender->defer_guarded_domain)
      chrome9_flush_triangle_batch(crender);
   while (batch) {
      struct chrome9_deferred_fragment_batch *next = batch->next;

      FREE(batch);
      batch = next;
   }
   if (crender->ctx->vbuf_render == crender)
      crender->ctx->vbuf_render = NULL;
   util_dynarray_fini(&crender->split_vertices);
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
   if (!ctx)
      return;
   chrome9_sw_finish(ctx);
   chrome9_draw_flush_gpu_batch(ctx);
}

/* Submit queued GPU triangles only; pending llvmpipe work may continue. */
void
chrome9_draw_flush_gpu_batch(struct chrome9_context *ctx)
{
   if (ctx)
      chrome9_tcl_batch_flush(ctx);
   if (ctx && ctx->vbuf_render) {
      struct chrome9_vbuf_render *render = ctx->vbuf_render;

      chrome9_flush_triangle_batch(render);
      /* Match the former per-vbuf-draw recovery boundary: one rejected batch
       * does not permanently disable later frames after an explicit flush. */
      render->batch_failed = false;
   }
}

void
chrome9_draw_begin_guarded_domain(struct chrome9_context *ctx)
{
   struct chrome9_vbuf_render *render;

   if (!ctx || !ctx->vbuf_render)
      return;
   chrome9_draw_flush_batch(ctx);
   render = ctx->vbuf_render;
   if (render->defer_guarded_domain || render->deferred_head)
      return;
   render->defer_guarded_domain = true;
   render->deferred_failed = false;
   render->deferred_triangles = 0;
   render->deferred_rejected_before = ctx->rejected_triangles;
}

/* False when a value left the proven domain: nothing of the draw was
 * submitted and the caller renders it another way. */
bool
chrome9_draw_end_guarded_domain(struct chrome9_context *ctx)
{
   struct chrome9_vbuf_render *render;
   struct chrome9_deferred_fragment_batch *batch;
   bool failed;

   if (!ctx || !ctx->vbuf_render)
      return true;
   render = ctx->vbuf_render;
   if (!render->defer_guarded_domain)
      return true;
   chrome9_flush_triangle_batch(render);
   failed = render->deferred_failed ||
      ctx->rejected_triangles != render->deferred_rejected_before;
   render->defer_guarded_domain = false;
   render->deferred_failed = false;
   render->batch_failed = false;
   batch = render->deferred_head;
   render->deferred_head = NULL;
   render->deferred_tail = NULL;
   render->deferred_triangles = 0;
   if (failed)
      for (struct chrome9_deferred_fragment_batch *item = batch;
           item; item = item->next)
         ctx->rejected_triangles += item->draw.triangle_count;
   while (batch) {
      struct chrome9_deferred_fragment_batch *next = batch->next;

      if (!failed && !render->batch_failed) {
         render->batch_fragment = batch->draw;
         memcpy(render->batch_fragment_instructions, batch->instructions,
                sizeof(batch->instructions));
         memcpy(render->batch_fragment_constants, batch->constants,
                sizeof(batch->constants));
         memcpy(render->batch_fragment_textures, batch->textures,
                sizeof(batch->textures));
         memcpy(render->batch_fragment_vertices, batch->vertices,
                batch->draw.triangle_count * 3 * sizeof(batch->vertices[0]));
         render->batch_count = batch->draw.triangle_count;
         render->batch_generic_program = true;
         chrome9_flush_triangle_batch(render);
      } else if (!failed) {
         ctx->rejected_triangles += batch->draw.triangle_count;
      }
      FREE(batch);
      batch = next;
   }
   return !failed;
}

static void
chrome9_tcl_stencil_face(const struct chrome9_context *ctx, unsigned face,
                         struct drm_chrome9_stencil_face *native)
{
   static const uint8_t native_op[8] = {
      [PIPE_STENCIL_OP_KEEP] = DRM_CHROME9_STENCIL_OP_KEEP,
      [PIPE_STENCIL_OP_ZERO] = DRM_CHROME9_STENCIL_OP_ZERO,
      [PIPE_STENCIL_OP_REPLACE] = DRM_CHROME9_STENCIL_OP_REPLACE,
      [PIPE_STENCIL_OP_INCR] = DRM_CHROME9_STENCIL_OP_INCR_SAT,
      [PIPE_STENCIL_OP_DECR] = DRM_CHROME9_STENCIL_OP_DECR_SAT,
      [PIPE_STENCIL_OP_INCR_WRAP] = DRM_CHROME9_STENCIL_OP_INCR_WRAP,
      [PIPE_STENCIL_OP_DECR_WRAP] = DRM_CHROME9_STENCIL_OP_DECR_WRAP,
      [PIPE_STENCIL_OP_INVERT] = DRM_CHROME9_STENCIL_OP_INVERT,
   };
   const struct pipe_stencil_state *s = &ctx->dsa->stencil[face];

   native->func = s->func;
   native->fail_op = native_op[s->fail_op & 7];
   native->zfail_op = native_op[s->zfail_op & 7];
   native->zpass_op = native_op[s->zpass_op & 7];
   native->ref = ctx->stencil_ref.ref_value[face];
   native->value_mask = s->valuemask;
   native->write_mask = s->writemask;
}

/*
 * Resolve the complete pixel state of a hardware TCL draw.  Screen
 * winding follows chrome9_triangle_stencil(): front faces are CCW in
 * window coordinates when front_ccw is set.
 */
/*
 * Colour buffers 1..3 as kernel colour targets: guarded private BGRA/BGRX
 * level-zero images of RT0's size, one per fragment program output.
 */
static bool
chrome9_draw_color_targets(const struct chrome9_context *ctx,
                           const struct chrome9_resource *target,
                           struct drm_chrome9_draw_colors *colors)
{
   const struct pipe_framebuffer_state *fb = &ctx->framebuffer;

   unsigned outputs = 0;

   for (unsigned i = 0; i < ctx->fs->semantic_fs_ir.count; i++)
      outputs += ctx->fs->semantic_fs_ir.instruction[i].destination.file ==
                 CHROME9_PS_DESTINATION_COLOR;
   memset(colors, 0, sizeof(*colors));
   if (fb->nr_cbufs <= 1)
      return outputs <= 1;
   if (fb->nr_cbufs > 4 || outputs != fb->nr_cbufs ||
       !chrome9_screen_has_capability2(ctx->base.screen,
                                       DRM_CHROME9_CAP2_COLOR_TARGETS))
      return false;
   colors->count = fb->nr_cbufs;
   for (unsigned i = 1; i < fb->nr_cbufs; i++) {
      const struct pipe_surface *surface = &fb->cbufs[i];
      const struct chrome9_resource *res = surface->texture ?
         chrome9_resource_const(surface->texture) : NULL;
      struct drm_chrome9_color_target *c = &colors->target[i - 1];

      if (!res || !res->guarded || res->guarded_shared || res->parent ||
          surface->level || surface->first_layer ||
          res->base.target != PIPE_TEXTURE_2D ||
          res->base.width0 != target->base.width0 ||
          res->base.height0 != target->base.height0 ||
          (res->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
           res->base.format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
          res->data_offset != 4096)
         return false;
      c->handle = res->bo.handle;
      c->format = res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ?
         DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 :
         DRM_CHROME9_DESTINATION_FORMAT_BGRX8888;
      c->pitch = res->stride;
      c->flags = chrome9_resource_is_tiled(res) ?
         DRM_CHROME9_COLOR_TARGET_TILED : 0;
      c->object_size = res->bo.size;
   }
   return true;
}

bool
chrome9_draw_tcl_fragment(struct chrome9_context *ctx,
                          struct chrome9_tcl_fragment *fragment,
                          const char **why)
{
   struct chrome9_vbuf_render *crender = ctx->vbuf_render;
   bool saved_generic;
   uint8_t saved_varyings, saved_textures;
   struct chrome9_resource *target = NULL, *depth = NULL;
   struct chrome9_resource *textures[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   struct chrome9_sampler_state *samplers[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
   struct drm_chrome9_blend_state blend = { 0 };
   struct drm_chrome9_scissor_state scissor;
   const char *reason = NULL;
   bool depth_active = false, blend_successor = false, ok = false;

   *why = "no generic fragment program";
   if (!crender || !chrome9_generic_fragment_program(ctx))
      return false;
   chrome9_flush_triangle_batch(crender);
   *why = "failed batch";
   if (crender->batch_failed)
      return false;
   /* Borrow the vbuf layout fields the shared state builder reads. */
   saved_generic = crender->generic_program;
   saved_varyings = crender->varying_count;
   saved_textures = crender->texture_count;
   crender->generic_program = true;
   crender->varying_count = ctx->fs->semantic_varying_mask ?
      32 - __builtin_clz(ctx->fs->semantic_varying_mask) : 0;
   crender->texture_count = ctx->fragment_sparse_textures ?
      __builtin_popcount(ctx->fs->semantic_sampler_mask) :
      chrome9_fragment_sampler_high_water(ctx->fs->semantic_sampler_mask);
   if (!chrome9_draw_state_supported(ctx, &target, &depth, textures,
                                     samplers, crender->texture_count,
                                     &depth_active, &blend,
                                     &blend_successor, &reason)) {
      *why = reason ? reason : "draw state";
      goto out;
   }
   *why = "colour targets";
   if (!chrome9_draw_color_targets(ctx, target, &fragment->colors))
      goto out;
   *why = "fragment program state";
   fragment->resource_count = 0;
   fragment->resources[fragment->resource_count++] = &target->base;
   for (unsigned i = 1; i < ctx->framebuffer.nr_cbufs; i++)
      fragment->resources[fragment->resource_count++] =
         ctx->framebuffer.cbufs[i].texture;
   if (depth_active)
      fragment->resources[fragment->resource_count++] = &depth->base;
   for (unsigned unit = 0; unit < crender->texture_count; unit++)
      fragment->resources[fragment->resource_count++] = &textures[unit]->base;
   chrome9_draw_scissor(ctx, target, &scissor);
   if (!chrome9_generic_fragment_state(crender, target, depth, textures,
          samplers, depth_active, blend_successor, &blend, &scissor,
          chrome9_draw_depth_flags(ctx, depth_active), &fragment->draw,
          fragment->instructions, fragment->constants, fragment->textures))
      goto out;
   if (fragment->draw.flags & DRM_CHROME9_FRAGMENT_STENCIL) {
      const bool front_ccw = ctx->rasterizer && ctx->rasterizer->front_ccw;
      const unsigned back = ctx->dsa->stencil[1].enabled ? 1 : 0;

      chrome9_tcl_stencil_face(ctx, 0, front_ccw ? &fragment->draw.stencil.ccw :
                                                   &fragment->draw.stencil.cw);
      chrome9_tcl_stencil_face(ctx, back, front_ccw ?
                               &fragment->draw.stencil.cw :
                               &fragment->draw.stencil.ccw);
   }
   ok = true;
out:
   crender->generic_program = saved_generic;
   crender->varying_count = saved_varyings;
   crender->texture_count = saved_textures;
   return ok;
}

/*
 * Queued DRAW_VERTEX_PROGRAMS work: every draw owns one block with copies
 * of the arrays its pointers name, and the batch holds references on the
 * resources behind its GEM handles until it is submitted.
 */
struct chrome9_tcl_batch {
   unsigned count;
   struct drm_chrome9_draw_vertex_program draws[DRM_CHROME9_VERTEX_MAX_BATCH];
   struct drm_chrome9_draw_colors colors[DRM_CHROME9_VERTEX_MAX_BATCH];
   void *blocks[DRM_CHROME9_VERTEX_MAX_BATCH];
   unsigned triangles[DRM_CHROME9_VERTEX_MAX_BATCH];
   struct util_dynarray resources;
   /* Distinct kernel objects: destination, depth, textures, buffers. */
   uint32_t texture_handles[DRM_CHROME9_VERTEX_MAX_BATCH];
   uint32_t buffer_handles[DRM_CHROME9_VERTEX_MAX_BATCH];
   unsigned texture_handle_count, buffer_handle_count;
};

#define CHROME9_TCL_BATCH_MAX_OBJECTS 64

static bool
tcl_batch_has(const uint32_t *handles, unsigned count, uint32_t handle)
{
   for (unsigned i = 0; i < count; i++)
      if (handles[i] == handle)
         return true;
   return false;
}

/* Objects draw would add to the batch's distinct kernel objects. */
static unsigned
tcl_batch_new_objects(const struct chrome9_tcl_batch *b,
                      const struct drm_chrome9_draw_vertex_program *draw,
                      const struct drm_chrome9_fragment_texture_state *textures,
                      const struct drm_chrome9_vertex_buffer *buffers)
{
   unsigned added = 0;

   for (unsigned t = 0; t < draw->fragment.texture_count; t++)
      added += !tcl_batch_has(b->texture_handles, b->texture_handle_count,
                              textures[t].handle);
   for (unsigned v = 0; v < draw->buffer_count; v++)
      added += !tcl_batch_has(b->buffer_handles, b->buffer_handle_count,
                              buffers[v].handle);
   return added;
}

static void
tcl_batch_release(struct chrome9_tcl_batch *b)
{
   util_dynarray_foreach(&b->resources, struct pipe_resource *, resource)
      pipe_resource_reference(resource, NULL);
   util_dynarray_clear(&b->resources);
   for (unsigned i = 0; i < b->count; i++)
      FREE(b->blocks[i]);
   b->count = 0;
   b->texture_handle_count = 0;
   b->buffer_handle_count = 0;
}

void
chrome9_tcl_batch_flush(struct chrome9_context *ctx)
{
   struct chrome9_tcl_batch *b = ctx->tcl_batch;
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);

   if (!b || !b->count)
      return;
   /* A batch shares its colour targets (the batch flushes on a change). */
   const struct drm_chrome9_draw_colors *colors =
      b->colors[0].count > 1 ? b->colors : NULL;

   if (b->count == 1 && !colors ?
       !chrome9_draw_vertex_program(fd, &b->draws[0]) :
       !chrome9_draw_vertex_programs(fd, b->draws, colors, b->count)) {
      /* Retry singly so one rejected draw loses only its triangles; the
       * kernel may already have counted segments of the failed batch. */
      chrome9_query_mark_inexact(ctx);
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr, "chrome9: DRAW_VERTEX_PROGRAMS of %u failed: %s\n",
                 b->count, strerror(errno));
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
      for (unsigned i = 0; b->count > 1 && i < b->count; i++)
         if (colors ?
             !chrome9_draw_vertex_programs(fd, &b->draws[i], &colors[i], 1) :
             !chrome9_draw_vertex_program(fd, &b->draws[i]))
            CHROME9_REJECT(ctx, b->triangles[i]);
      if (b->count == 1)
         CHROME9_REJECT(ctx, b->triangles[0]);
   }
   ctx->submitted_batches++;
   tcl_batch_release(b);
}

/* Submit the queue if a queued draw reads resource. */
void
chrome9_tcl_batch_flush_resource(struct chrome9_context *ctx,
                                 const struct pipe_resource *resource)
{
   struct chrome9_tcl_batch *b = ctx->tcl_batch;

   if (!b || !b->count)
      return;
   util_dynarray_foreach(&b->resources, struct pipe_resource *, held) {
      if (*held == resource) {
         chrome9_tcl_batch_flush(ctx);
         return;
      }
   }
}

void
chrome9_tcl_batch_destroy(struct chrome9_context *ctx)
{
   if (!ctx->tcl_batch)
      return;
   chrome9_tcl_batch_flush(ctx);
   util_dynarray_fini(&ctx->tcl_batch->resources);
   FREE(ctx->tcl_batch);
   ctx->tcl_batch = NULL;
}

static void *
tcl_batch_copy(uint8_t **cursor, const void *source, size_t bytes)
{
   void *copy = *cursor;

   if (bytes)
      memcpy(copy, source, bytes);
   *cursor += align(bytes, 16);
   return copy;
}

/* Queue a copy of draw; false when it cannot be queued. */
static bool
tcl_batch_append(struct chrome9_context *ctx,
                 const struct drm_chrome9_draw_vertex_program *draw,
                 const struct drm_chrome9_draw_colors *colors,
                 unsigned triangles, struct pipe_resource *const *resources,
                 unsigned resource_count)
{
   struct chrome9_tcl_batch *b = ctx->tcl_batch;
   const struct drm_chrome9_draw_fragment_program *f = &draw->fragment;
   const size_t sizes[] = {
      f->instruction_count * sizeof(struct drm_chrome9_fragment_instruction),
      f->constant_count * 4 * sizeof(uint32_t),
      f->texture_count * sizeof(struct drm_chrome9_fragment_texture_state),
      draw->instruction_count * sizeof(struct drm_chrome9_vertex_instruction),
      draw->constant_count * 4 * sizeof(uint32_t),
      draw->element_count * sizeof(struct drm_chrome9_vertex_element),
      draw->buffer_count * sizeof(struct drm_chrome9_vertex_buffer),
      (size_t)draw->index_count * draw->index_size,
      (size_t)draw->vertex_count * draw->vertex_stride,
      draw->int_constants_ptr ?
         DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS * 4 * sizeof(uint8_t) : 0,
   };
   const struct drm_chrome9_fragment_texture_state *textures =
      (const void *)(uintptr_t)f->textures_ptr;
   const struct drm_chrome9_vertex_buffer *buffers =
      (const void *)(uintptr_t)draw->buffers_ptr;
   struct drm_chrome9_draw_vertex_program *copy;
   size_t total = 0;
   uint8_t *block, *cursor;

   if (!b) {
      b = ctx->tcl_batch = CALLOC_STRUCT(chrome9_tcl_batch);
      if (!b)
         return false;
      util_dynarray_init(&b->resources, NULL);
   }
   if (b->count &&
       (b->count == DRM_CHROME9_VERTEX_MAX_BATCH ||
        b->draws[0].fragment.destination_handle != f->destination_handle ||
        b->draws[0].fragment.depth_handle != f->depth_handle ||
        memcmp(&b->colors[0], colors, sizeof(*colors)) ||
        2 + b->texture_handle_count + b->buffer_handle_count +
           tcl_batch_new_objects(b, draw, textures, buffers) >
           CHROME9_TCL_BATCH_MAX_OBJECTS))
      chrome9_tcl_batch_flush(ctx);
   for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++)
      total += align(sizes[i], 16);
   block = cursor = MALLOC(MAX2(total, 16));
   if (!block)
      return false;
   copy = &b->draws[b->count];
   *copy = *draw;
   b->colors[b->count] = *colors;
   copy->fragment.instructions_ptr = (uintptr_t)tcl_batch_copy(&cursor,
      (const void *)(uintptr_t)f->instructions_ptr, sizes[0]);
   copy->fragment.constants_ptr = f->constant_count ?
      (uintptr_t)tcl_batch_copy(&cursor,
         (const void *)(uintptr_t)f->constants_ptr, sizes[1]) : 0;
   copy->fragment.textures_ptr = f->texture_count ?
      (uintptr_t)tcl_batch_copy(&cursor, textures, sizes[2]) : 0;
   copy->instructions_ptr = (uintptr_t)tcl_batch_copy(&cursor,
      (const void *)(uintptr_t)draw->instructions_ptr, sizes[3]);
   copy->constants_ptr = draw->constant_count ?
      (uintptr_t)tcl_batch_copy(&cursor,
         (const void *)(uintptr_t)draw->constants_ptr, sizes[4]) : 0;
   copy->elements_ptr = (uintptr_t)tcl_batch_copy(&cursor,
      (const void *)(uintptr_t)draw->elements_ptr, sizes[5]);
   copy->buffers_ptr = draw->buffer_count ?
      (uintptr_t)tcl_batch_copy(&cursor, buffers, sizes[6]) : 0;
   copy->indices_ptr = draw->index_count ?
      (uintptr_t)tcl_batch_copy(&cursor,
         (const void *)(uintptr_t)draw->indices_ptr, sizes[7]) : 0;
   copy->vertices_ptr = draw->vertex_count ?
      (uintptr_t)tcl_batch_copy(&cursor,
         (const void *)(uintptr_t)draw->vertices_ptr, sizes[8]) : 0;
   copy->int_constants_ptr = draw->int_constants_ptr ?
      (uintptr_t)tcl_batch_copy(&cursor,
         (const void *)(uintptr_t)draw->int_constants_ptr, sizes[9]) : 0;
   for (unsigned t = 0; t < f->texture_count; t++)
      if (!tcl_batch_has(b->texture_handles, b->texture_handle_count,
                         textures[t].handle))
         b->texture_handles[b->texture_handle_count++] = textures[t].handle;
   for (unsigned v = 0; v < draw->buffer_count; v++)
      if (!tcl_batch_has(b->buffer_handles, b->buffer_handle_count,
                         buffers[v].handle))
         b->buffer_handles[b->buffer_handle_count++] = buffers[v].handle;
   for (unsigned i = 0; i < resource_count; i++) {
      struct pipe_resource *held = NULL;

      if (!resources[i])
         continue;
      pipe_resource_reference(&held, resources[i]);
      util_dynarray_append(&b->resources, held);
   }
   b->blocks[b->count] = block;
   b->triangles[b->count] = triangles;
   b->count++;
   return true;
}

bool
chrome9_draw_tcl_submit(struct chrome9_context *ctx,
                        const struct chrome9_tcl_fragment *fragment,
                        struct drm_chrome9_draw_vertex_program *draw,
                        unsigned triangles,
                        struct pipe_resource *const *buffers,
                        unsigned buffer_count)
{
   const int fd = ctx->base.screen->get_screen_fd(ctx->base.screen);

   draw->fragment = fragment->draw;
   draw->fragment.instructions_ptr = (uintptr_t)fragment->instructions;
   draw->fragment.constants_ptr = fragment->draw.constant_count ?
      (uintptr_t)fragment->constants : 0;
   draw->fragment.textures_ptr = fragment->draw.texture_count ?
      (uintptr_t)fragment->textures : 0;
   chrome9_context_prepare_render_target(ctx);
   if (ctx->tcl_batching) {
      struct pipe_resource *resources[5 + DRM_CHROME9_FRAGMENT_MAX_TEXTURES +
                                      DRM_CHROME9_VERTEX_MAX_INPUTS];
      unsigned count = fragment->resource_count;

      memcpy(resources, fragment->resources, count * sizeof(resources[0]));
      for (unsigned i = 0; i < buffer_count; i++)
         resources[count++] = buffers[i];
      if (tcl_batch_append(ctx, draw, &fragment->colors, triangles,
                           resources, count)) {
         ctx->submitted_triangles += triangles;
         ctx->submitted_program_batches++;
         chrome9_context_render_target_written(ctx);
         return true;
      }
   }
   chrome9_tcl_batch_flush(ctx);
   /* Colour targets travel only with DRAW_VERTEX_PROGRAMS. */
   if (fragment->colors.count > 1 ?
       !chrome9_draw_vertex_programs(fd, draw, &fragment->colors, 1) :
       !chrome9_draw_vertex_program(fd, draw)) {
      /* The caller falls back or accounts the rejected triangles. */
      if (!(ctx->warnings & CHROME9_WARN_SUBMIT)) {
         fprintf(stderr, "chrome9: DRAW_VERTEX_PROGRAM failed after=%" PRIu64
                         " count=%u: %s\n", ctx->submitted_triangles,
                 triangles, strerror(errno));
         ctx->warnings |= CHROME9_WARN_SUBMIT;
      }
      return false;
   }
   ctx->submitted_triangles += triangles;
   ctx->submitted_batches++;
   ctx->submitted_program_batches++;
   chrome9_context_render_target_written(ctx);
   return true;
}
