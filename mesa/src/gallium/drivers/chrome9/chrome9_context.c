/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_context.h"

#include "chrome9_draw.h"
#include "chrome9_drm.h"
#include "chrome9_resource.h"

#include <math.h>
#include <stdio.h>

#include "draw/draw_context.h"
#include "nir.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_draw.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"

static void
chrome9_copy_surface(struct pipe_resource *destination,
                     struct pipe_resource *source)
{
   struct chrome9_resource *dst = chrome9_resource(destination);
   struct chrome9_resource *src = chrome9_resource(source);
   unsigned width = MIN2(destination->width0, source->width0) * 4;
   unsigned height = MIN2(destination->height0, source->height0);
   uint8_t *dst_map = (uint8_t *)dst->bo.map + dst->data_offset;
   const uint8_t *src_map = (const uint8_t *)src->bo.map + src->data_offset;

   for (unsigned y = 0; y < height; y++)
      memcpy(dst_map + (size_t)y * dst->stride,
             src_map + (size_t)y * src->stride, width);
}

static void
chrome9_flush_render_target(struct chrome9_context *ctx)
{
   struct pipe_resource *external;

   if (!ctx->render_target_dirty || !ctx->render_target ||
       !ctx->framebuffer.nr_cbufs ||
       !(external = ctx->framebuffer.cbufs[0].texture))
      return;
   if (external != ctx->render_target)
      chrome9_copy_surface(external, ctx->render_target);
   ctx->render_target_dirty = false;
}

struct chrome9_resource *
chrome9_context_render_target(struct chrome9_context *ctx)
{
   return ctx->render_target ? chrome9_resource(ctx->render_target) : NULL;
}

struct chrome9_resource *
chrome9_context_depth_target(struct chrome9_context *ctx)
{
   return ctx->framebuffer.zsbuf.texture ?
             chrome9_resource(ctx->framebuffer.zsbuf.texture) : NULL;
}

void
chrome9_context_render_target_written(struct chrome9_context *ctx)
{
   ctx->render_target_dirty = true;
}

void
chrome9_context_render_target_direct_written(struct chrome9_context *ctx)
{
   ctx->render_target_dirty = false;
   ctx->render_target_stale = true;
}

void
chrome9_context_prepare_render_target(struct chrome9_context *ctx)
{
   struct pipe_resource *external;

   if (!ctx->render_target_stale || !ctx->render_target ||
       !ctx->framebuffer.nr_cbufs ||
       !(external = ctx->framebuffer.cbufs[0].texture))
      return;
   if (external != ctx->render_target)
      chrome9_copy_surface(ctx->render_target, external);
   ctx->render_target_stale = false;
}

static void
chrome9_warn_once(struct chrome9_context *ctx, uint32_t warning,
                  const char *message)
{
   if (ctx->warnings & warning)
      return;
   ctx->warnings |= warning;
   fprintf(stderr, "chrome9: %s\n", message);
}

static void *
chrome9_dup_state(const void *state, size_t size)
{
   void *copy = MALLOC(size);

   if (copy)
      memcpy(copy, state, size);
   return copy;
}

static void *
chrome9_create_blend_state(struct pipe_context *pipe,
                           const struct pipe_blend_state *state)
{
   (void)pipe;
   return chrome9_dup_state(state, sizeof(*state));
}

static void
chrome9_bind_blend_state(struct pipe_context *pipe, void *state)
{
   chrome9_context(pipe)->blend = state;
}

static void *
chrome9_create_dsa_state(struct pipe_context *pipe,
                         const struct pipe_depth_stencil_alpha_state *state)
{
   (void)pipe;
   return chrome9_dup_state(state, sizeof(*state));
}

static void
chrome9_bind_dsa_state(struct pipe_context *pipe, void *state)
{
   chrome9_context(pipe)->dsa = state;
}

static void *
chrome9_create_rasterizer_state(struct pipe_context *pipe,
                                const struct pipe_rasterizer_state *state)
{
   (void)pipe;
   return chrome9_dup_state(state, sizeof(*state));
}

static void
chrome9_bind_rasterizer_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   ctx->rasterizer = state;
   draw_set_rasterizer_state(ctx->draw, state, state);
}

static void *
chrome9_create_sampler_state(struct pipe_context *pipe,
                             const struct pipe_sampler_state *state)
{
   struct chrome9_sampler_state *sampler =
      CALLOC_STRUCT(chrome9_sampler_state);

   (void)pipe;
   if (!sampler)
      return NULL;
   sampler->base = *state;
   sampler->supported = true;

   switch (state->min_img_filter) {
   case PIPE_TEX_FILTER_NEAREST: sampler->min_filter = 0; break;
   case PIPE_TEX_FILTER_LINEAR: sampler->min_filter = 1; break;
   default: sampler->supported = false; break;
   }
   switch (state->mag_img_filter) {
   case PIPE_TEX_FILTER_NEAREST: sampler->mag_filter = 0; break;
   case PIPE_TEX_FILTER_LINEAR: sampler->mag_filter = 1; break;
   default: sampler->supported = false; break;
   }
   switch (state->min_mip_filter) {
   case PIPE_TEX_MIPFILTER_NONE: sampler->mip_filter = 0; break;
   case PIPE_TEX_MIPFILTER_NEAREST:
      sampler->mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST;
      break;
   case PIPE_TEX_MIPFILTER_LINEAR:
      sampler->mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR;
      break;
   default: sampler->supported = false; break;
   }
   switch (state->wrap_s) {
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE: sampler->wrap_s = 1; break;
   case PIPE_TEX_WRAP_REPEAT: sampler->wrap_s = 2; break;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: sampler->wrap_s = 3; break;
   default: sampler->supported = false; break;
   }
   switch (state->wrap_t) {
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE: sampler->wrap_t = 1; break;
   case PIPE_TEX_WRAP_REPEAT: sampler->wrap_t = 2; break;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: sampler->wrap_t = 3; break;
   default: sampler->supported = false; break;
   }
   if (state->compare_mode != PIPE_TEX_COMPARE_NONE ||
       state->unnormalized_coords || !isfinite(state->lod_bias) ||
       state->lod_bias < -16.0f || state->lod_bias > 511.0f / 32.0f ||
       state->min_lod != 0.0f || !isfinite(state->max_lod) ||
       state->max_lod < 0.0f ||
       state->max_anisotropy > 1 || state->seamless_cube_map ||
       state->reduction_mode != PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE)
      sampler->supported = false;
   return sampler;
}

static void
chrome9_bind_sampler_states(struct pipe_context *pipe,
                            mesa_shader_stage shader, unsigned start,
                            unsigned count, void **states)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   unsigned end;

   if (shader != MESA_SHADER_FRAGMENT || start >= PIPE_MAX_SAMPLERS)
      return;
   end = MIN2(start + count, PIPE_MAX_SAMPLERS);
   for (unsigned i = start; i < end; i++)
      ctx->sampler_states[i] = states ? states[i - start] : NULL;
   ctx->num_sampler_states = 0;
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i])
         ctx->num_sampler_states = i + 1;
}

static void
chrome9_delete_sampler_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i] == state)
         ctx->sampler_states[i] = NULL;
   ctx->num_sampler_states = 0;
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i])
         ctx->num_sampler_states = i + 1;
   FREE(state);
}

static void
chrome9_delete_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (ctx->blend == state)
      ctx->blend = NULL;
   if (ctx->dsa == state)
      ctx->dsa = NULL;
   if (ctx->rasterizer == state)
      ctx->rasterizer = NULL;
   FREE(state);
}

static void *
chrome9_create_vs_state(struct pipe_context *pipe,
                        const struct pipe_shader_state *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = CALLOC_STRUCT(chrome9_shader);

   if (!shader)
      return NULL;
   shader->draw = draw_create_vertex_shader(ctx->draw, state);
   shader->supported = shader->draw != NULL;
   if (!shader->draw) {
      FREE(shader);
      return NULL;
   }
   return shader;
}

static void
chrome9_bind_vs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   ctx->vs = shader;
   draw_bind_vertex_shader(ctx->draw, shader ? shader->draw : NULL);
}

static void
chrome9_delete_vs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   if (!shader)
      return;
   if (ctx->vs == shader) {
      ctx->vs = NULL;
      draw_bind_vertex_shader(ctx->draw, NULL);
   }
   draw_delete_vertex_shader(ctx->draw, shader->draw);
   FREE(shader);
}

/* The known Chrome9 pixel program is MOV oC0, v0.  Only accept the NIR form
 * whose sole result is the unmodified primary color; accepting arbitrary NIR
 * here would silently execute different fragment semantics. */
static bool
chrome9_fs_straight_line_entrypoint(nir_shader *nir,
                                    nir_function_impl **impl_out,
                                    nir_block **block_out)
{
   nir_function_impl *impl;
   nir_cf_node *node;

   if (!nir || nir->info.stage != MESA_SHADER_FRAGMENT ||
       !exec_list_is_singular(&nir->functions))
      return false;
   impl = nir_shader_get_entrypoint(nir);
   if (!impl || impl->preamble || !exec_list_is_singular(&impl->body))
      return false;
   node = exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node);
   if (node->type != nir_cf_node_block)
      return false;
   if (impl_out)
      *impl_out = impl;
   if (block_out)
      *block_out = nir_cf_node_as_block(node);
   return true;
}

static bool
chrome9_fs_smooth_variable(const nir_variable *var, unsigned location)
{
   return var && var->data.mode == nir_var_shader_in &&
          var->data.location == location && !var->data.index &&
          !var->data.location_frac && !var->data.centroid &&
          !var->data.sample && !var->data.patch && !var->data.compact &&
          !var->data.per_primitive && !var->data.per_vertex &&
          (var->data.interpolation == INTERP_MODE_NONE ||
           var->data.interpolation == INTERP_MODE_SMOOTH);
}

static bool
chrome9_fs_flat_primary_variable(const nir_variable *var)
{
   return var && var->data.mode == nir_var_shader_in &&
          var->data.location == VARYING_SLOT_COL0 && !var->data.index &&
          !var->data.location_frac && !var->data.centroid &&
          !var->data.sample && !var->data.patch && !var->data.compact &&
          !var->data.per_primitive && !var->data.per_vertex &&
          var->data.interpolation == INTERP_MODE_FLAT;
}

static bool
chrome9_fs_has_flat_primary(nir_shader *nir)
{
   nir_foreach_shader_in_variable(var, nir) {
      if (chrome9_fs_flat_primary_variable(var))
         return true;
   }
   return false;
}

static bool
chrome9_fs_smooth_pixel_load(const nir_intrinsic_instr *load,
                             unsigned location)
{
   nir_instr *instr;
   nir_intrinsic_instr *bary;

   if (!load || load->intrinsic != nir_intrinsic_load_interpolated_input ||
       nir_intrinsic_io_semantics(load).location != location ||
       nir_intrinsic_component(load) || !load->src[0].ssa ||
       !nir_src_is_const(load->src[1]) || nir_src_as_uint(load->src[1]))
      return false;
   instr = nir_def_instr(load->src[0].ssa);
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   bary = nir_instr_as_intrinsic(instr);
   return bary->intrinsic == nir_intrinsic_load_barycentric_pixel &&
          (nir_intrinsic_interp_mode(bary) == INTERP_MODE_NONE ||
           nir_intrinsic_interp_mode(bary) == INTERP_MODE_SMOOTH);
}

static bool
chrome9_fs_is_primary_color(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      nir_variable *var;

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
         return nir_intrinsic_io_semantics(intr).location ==
                   VARYING_SLOT_COL0 &&
                !nir_intrinsic_component(intr);
      case nir_intrinsic_load_interpolated_input:
         return chrome9_fs_smooth_pixel_load(intr, VARYING_SLOT_COL0);
      case nir_intrinsic_load_deref:
         var = nir_intrinsic_get_var(intr, 0);
         return chrome9_fs_smooth_variable(var, VARYING_SLOT_COL0);
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_primary_color(alu->src[0].src.ssa, depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *source = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != source ||
                alu->src[c].swizzle[0] != c)
               return false;
         return chrome9_fs_is_primary_color(source, depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_flat_primary_color(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      return intr->intrinsic == nir_intrinsic_load_deref &&
             chrome9_fs_flat_primary_variable(
                nir_intrinsic_get_var(intr, 0));
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_flat_primary_color(alu->src[0].src.ssa,
                                                  depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *source = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != source ||
                alu->src[c].swizzle[0] != c)
               return false;
         return chrome9_fs_is_flat_primary_color(source, depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_texcoord(nir_def *def, unsigned unit, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   unsigned location;

   if (depth > 8 || def->num_components < 2)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      nir_variable *var;

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
         location = nir_intrinsic_io_semantics(intr).location;
         return (location == VARYING_SLOT_TEX0 + unit ||
                 location == VARYING_SLOT_VAR0 + unit) &&
                !nir_intrinsic_component(intr);
      case nir_intrinsic_load_interpolated_input:
         return chrome9_fs_smooth_pixel_load(intr,
                                              VARYING_SLOT_TEX0 + unit) ||
                chrome9_fs_smooth_pixel_load(intr,
                                              VARYING_SLOT_VAR0 + unit);
      case nir_intrinsic_load_deref:
         var = nir_intrinsic_get_var(intr, 0);
         return chrome9_fs_smooth_variable(var,
                                            VARYING_SLOT_TEX0 + unit) ||
                chrome9_fs_smooth_variable(var,
                                            VARYING_SLOT_VAR0 + unit);
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov)
         return alu->src[0].swizzle[0] == 0 &&
                alu->src[0].swizzle[1] == 1 &&
                chrome9_fs_is_texcoord(alu->src[0].src.ssa, unit, depth + 1);
      if (alu->op == nir_op_vec2) {
         nir_def *source = alu->src[0].src.ssa;

         return alu->src[1].src.ssa == source &&
                alu->src[0].swizzle[0] == 0 &&
                alu->src[1].swizzle[0] == 1 &&
                chrome9_fs_is_texcoord(source, unit, depth + 1);
      }
   }
   return false;
}

/* Glamor's simplest solid shader is an unmodified vec4 from the first
 * fragment constant.  Keep this deliberately narrower than general uniform
 * support: one full 32-bit vec4, buffer zero, byte/vec4 offset zero. */
static bool
chrome9_fs_is_constant_color(nir_def *def, unsigned index, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      switch (intr->intrinsic) {
      case nir_intrinsic_load_uniform:
         return nir_src_is_const(intr->src[0]) &&
                nir_intrinsic_base(intr) <= index &&
                nir_src_as_uint(intr->src[0]) ==
                   index - nir_intrinsic_base(intr);
      case nir_intrinsic_load_ubo:
         return nir_src_is_const(intr->src[0]) &&
                !nir_src_as_uint(intr->src[0]) &&
                nir_src_is_const(intr->src[1]) &&
                nir_src_as_uint(intr->src[1]) == index * 16;
      case nir_intrinsic_load_ubo_vec4:
         return !nir_intrinsic_component(intr) &&
                nir_src_is_const(intr->src[0]) &&
                !nir_src_as_uint(intr->src[0]) &&
                nir_src_is_const(intr->src[1]) &&
                nir_intrinsic_base(intr) <= index &&
                nir_src_as_uint(intr->src[1]) ==
                   index - nir_intrinsic_base(intr);
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_constant_color(alu->src[0].src.ssa, index,
                                             depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_texture(nir_def *def, unsigned unit)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   int coord;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   return tex->op == nir_texop_tex &&
          tex->sampler_dim == GLSL_SAMPLER_DIM_2D && !tex->is_array &&
          !tex->is_shadow && tex->texture_index == unit &&
          tex->sampler_index == unit && tex->coord_components == 2 &&
          coord >= 0 && tex->num_srcs == 1 &&
          chrome9_fs_is_texcoord(tex->src[coord].src.ssa, unit, 0);
}

static bool
chrome9_fs_is_texcoord_q(nir_def *def, unsigned unit, unsigned depth)
{
   nir_instr *instr;

   if (depth > 8 || def->num_components != 1 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov && alu->src[0].swizzle[0] == 3 &&
          chrome9_fs_is_texcoord(alu->src[0].src.ssa, unit, depth + 1);
}

static bool
chrome9_fs_is_projected_texture(nir_def *def, unsigned unit)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   int coord, projector;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   projector = nir_tex_instr_src_index(tex, nir_tex_src_projector);
   return tex->op == nir_texop_tex &&
          tex->sampler_dim == GLSL_SAMPLER_DIM_2D && !tex->is_array &&
          !tex->is_shadow && tex->texture_index == unit &&
          tex->sampler_index == unit &&
          tex->coord_components == 2 && coord >= 0 && projector >= 0 &&
          tex->num_srcs == 2 &&
          chrome9_fs_is_texcoord(tex->src[coord].src.ssa, unit, 0) &&
          chrome9_fs_is_texcoord_q(tex->src[projector].src.ssa, unit, 0);
}

enum chrome9_fs_class_leaf {
   CHROME9_FS_LEAF_INVALID,
   CHROME9_FS_LEAF_TEXTURE0,
   CHROME9_FS_LEAF_TEXTURE1,
   CHROME9_FS_LEAF_PRIMARY,
   CHROME9_FS_LEAF_CONSTANT0,
   CHROME9_FS_LEAF_CONSTANT1,
   CHROME9_FS_LEAF_TEXTURE0_ALPHA,
   CHROME9_FS_LEAF_CONSTANT0_ALPHA,
   CHROME9_FS_LEAF_CONSTANT1_ALPHA,
};

static unsigned
chrome9_fs_alpha_leaf(unsigned leaf)
{
   switch (leaf) {
   case CHROME9_FS_LEAF_TEXTURE0:
      return CHROME9_FS_LEAF_TEXTURE0_ALPHA;
   case CHROME9_FS_LEAF_CONSTANT0:
      return CHROME9_FS_LEAF_CONSTANT0_ALPHA;
   case CHROME9_FS_LEAF_CONSTANT1:
      return CHROME9_FS_LEAF_CONSTANT1_ALPHA;
   default:
      return CHROME9_FS_LEAF_INVALID;
   }
}

static unsigned
chrome9_fs_leaf(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return CHROME9_FS_LEAF_INVALID;
   if (chrome9_fs_is_texture(def, 0))
      return CHROME9_FS_LEAF_TEXTURE0;
   if (chrome9_fs_is_texture(def, 1))
      return CHROME9_FS_LEAF_TEXTURE1;
   if (chrome9_fs_is_primary_color(def, depth))
      return CHROME9_FS_LEAF_PRIMARY;
   if (chrome9_fs_is_constant_color(def, 0, depth))
      return CHROME9_FS_LEAF_CONSTANT0;
   if (chrome9_fs_is_constant_color(def, 1, depth))
      return CHROME9_FS_LEAF_CONSTANT1;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         bool identity = true, alpha = true;

         for (unsigned c = 0; c < 4; c++) {
            identity &= alu->src[0].swizzle[c] == c;
            alpha &= alu->src[0].swizzle[c] == 3;
         }
         if (!identity && !alpha)
            return CHROME9_FS_LEAF_INVALID;
         unsigned leaf = chrome9_fs_leaf(alu->src[0].src.ssa, depth + 1);

         return identity ? leaf : chrome9_fs_alpha_leaf(leaf);
      }
   }
   return CHROME9_FS_LEAF_INVALID;
}

static unsigned
chrome9_fs_source_leaf(const nir_alu_src *src, unsigned depth)
{
   unsigned leaf;
   bool identity = true, alpha = true;

   if (!src->src.ssa || src->src.ssa->num_components != 4)
      return CHROME9_FS_LEAF_INVALID;
   for (unsigned c = 0; c < 4; c++) {
      identity &= src->swizzle[c] == c;
      alpha &= src->swizzle[c] == 3;
   }
   if (!identity && !alpha)
      return CHROME9_FS_LEAF_INVALID;
   leaf = chrome9_fs_leaf(src->src.ssa, depth + 1);
   if (identity)
      return leaf;
   return chrome9_fs_alpha_leaf(leaf);
}

static bool
chrome9_fs_is_one(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 1 || def->bit_size != 32)
      return false;
   if (instr->type == nir_instr_type_load_const)
      return nir_instr_as_load_const(instr)->value[0].u32 == 0x3f800000;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      return alu->op == nir_op_mov && !alu->src[0].swizzle[0] &&
             chrome9_fs_is_one(alu->src[0].src.ssa, depth + 1);
   }
   return false;
}

static bool
chrome9_fs_is_zero(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 1 || def->bit_size != 32)
      return false;
   if (instr->type == nir_instr_type_load_const)
      return nir_instr_as_load_const(instr)->value[0].u32 == 0;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      return alu->op == nir_op_mov && !alu->src[0].swizzle[0] &&
             chrome9_fs_is_zero(alu->src[0].src.ssa, depth + 1);
   }
   return false;
}

static bool
chrome9_fs_is_generic0(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);

   if (!def->num_components || def->num_components > 4 ||
       def->bit_size != 32 || instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_variable *var;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      return nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_VAR0 &&
             !nir_intrinsic_component(intr);
   case nir_intrinsic_load_interpolated_input:
      return chrome9_fs_smooth_pixel_load(intr, VARYING_SLOT_VAR0);
   case nir_intrinsic_load_deref:
      var = nir_intrinsic_get_var(intr, 0);
      return chrome9_fs_smooth_variable(var, VARYING_SLOT_VAR0);
   default:
      return false;
   }
}

static bool
chrome9_fs_is_flat_generic0(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *intr;
   nir_variable *var;

   if (def->num_components != 1 || def->bit_size != 32 ||
       instr->type != nir_instr_type_intrinsic)
      return false;
   intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   var = nir_intrinsic_get_var(intr, 0);
   return var && var->data.mode == nir_var_shader_in &&
          var->data.location == VARYING_SLOT_VAR0 && !var->data.index &&
          !var->data.location_frac && !var->data.centroid &&
          !var->data.sample && !var->data.patch && !var->data.compact &&
          !var->data.per_primitive && !var->data.per_vertex &&
          var->data.interpolation == INTERP_MODE_FLAT;
}

static bool
chrome9_fs_is_flat_generic0_rgbx(nir_def *def, unsigned depth)
{
   nir_instr *instr;

   if (depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_flat_generic0_rgbx(alu->src[0].src.ssa,
                                               depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   nir_def *source = alu->src[0].src.ssa;
   for (unsigned c = 0; c < 3; c++)
      if (alu->src[c].src.ssa != source || alu->src[c].swizzle[0])
         return false;
   return alu->src[3].swizzle[0] == 0 &&
          chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1) &&
          chrome9_fs_is_flat_generic0(source);
}

/* GLSL user varyings are linked to VAR0 rather than the legacy COL0 slot.
 * Accept only a complete, identity-ordered vec4 so it can be represented by
 * the same normalized D1 diffuse attribute as the proven COL0 program. */
static bool
chrome9_fs_is_generic0_rgba(nir_def *def, unsigned depth)
{
   nir_instr *instr;

   if (depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   if (chrome9_fs_is_generic0(def))
      return true;
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return chrome9_fs_is_generic0_rgba(alu->src[0].src.ssa,
                                             depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *source = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != source ||
                alu->src[c].swizzle[0] != c)
               return false;
         return chrome9_fs_is_generic0_rgba(source, depth + 1);
      }
   }
   return false;
}

static bool
chrome9_fs_is_generic0_component(const nir_alu_src *src, unsigned component,
                                 nir_def **base, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (chrome9_fs_is_generic0(def)) {
      if (component >= def->num_components || src->swizzle[0] != component)
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == nir_op_mov &&
          chrome9_fs_is_generic0_component(&alu->src[0], component, base,
                                           depth + 1);
}

static bool
chrome9_fs_is_generic0_rgbx(nir_def *def, nir_def **base, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *found = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_rgbx(alu->src[0].src.ssa, base,
                                         depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *source = NULL;

      if (!chrome9_fs_is_generic0_component(&alu->src[c], 0, &source,
                                            depth + 1) ||
          (found && source != found))
         return false;
      found = source;
   }
   if (alu->src[3].swizzle[0] ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = found;
   return found != NULL;
}

static bool
chrome9_fs_is_generic0_vec3_rgbx(nir_def *def, nir_def **base,
                                 unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *found = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_vec3_rgbx(alu->src[0].src.ssa, base,
                                               depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *source = NULL;

      if (!chrome9_fs_is_generic0_component(&alu->src[c], c, &source,
                                            depth + 1) ||
          source->num_components != 3 || (found && source != found))
         return false;
      found = source;
   }
   if (alu->src[3].swizzle[0] ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = found;
   return found != NULL;
}

/* glmark2 ideas uses a two-component vertex result as
 * vec4(VAR0.x, VAR0.x, VAR0.y, 1).  Preserve that exact component mapping in
 * the post-TNL D1 packing path; this is not permission to accept a general
 * varying swizzle. */
static bool
chrome9_fs_is_generic0_xxy_rgbx(nir_def *def, nir_def **base,
                                unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *found = NULL;
   static const unsigned component[3] = { 0, 0, 1 };

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_xxy_rgbx(alu->src[0].src.ssa, base,
                                              depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *source = NULL;

      if (!chrome9_fs_is_generic0_component(&alu->src[c], component[c],
                                             &source, depth + 1) ||
          source->num_components != 2 || (found && source != found))
         return false;
      found = source;
   }
   if (alu->src[3].swizzle[0] ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = found;
   return found != NULL;
}

/* glmark2's default Gouraud material is (0, 0, 1, 1).  Linking therefore
 * reduces Color to one user varying and reconstructs blue in the FS. */
static bool
chrome9_fs_is_generic0_blue_rgbx(nir_def *def, nir_def **base,
                                 unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *source = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_generic0_blue_rgbx(alu->src[0].src.ssa, base,
                                               depth + 1);
   }
   if (alu->op != nir_op_vec4 || alu->src[0].swizzle[0] ||
       alu->src[1].swizzle[0] || alu->src[3].swizzle[0] ||
       !chrome9_fs_is_zero(alu->src[0].src.ssa, depth + 1) ||
       !chrome9_fs_is_zero(alu->src[1].src.ssa, depth + 1) ||
       !chrome9_fs_is_generic0_component(&alu->src[2], 0, &source,
                                         depth + 1) ||
       !chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1))
      return false;
   *base = source;
   return source && source->num_components == 1;
}

static bool
chrome9_fs_is_literal_color(nir_def *def, uint32_t *color)
{
   nir_instr *instr;
   nir_load_const_instr *load;
   float rgba[4];

   if (!def || def->num_components != 4 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_load_const)
      return false;
   load = nir_instr_as_load_const(instr);
   for (unsigned c = 0; c < 4; c++) {
      rgba[c] = uif(load->value[c].u32);
      if (!isfinite(rgba[c]))
         return false;
   }
   *color = (uint32_t)float_to_ubyte(rgba[2]) |
            (uint32_t)float_to_ubyte(rgba[1]) << 8 |
            (uint32_t)float_to_ubyte(rgba[0]) << 16 |
            (uint32_t)float_to_ubyte(rgba[3]) << 24;
   return true;
}

static bool
chrome9_fs_is_packed_texcoord(nir_def *def, nir_def **base, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *first = NULL, *second = NULL;

   if (depth > 8 || def->num_components != 2 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_vec2 ||
       !chrome9_fs_is_generic0_component(&alu->src[0], 1, &first,
                                         depth + 1) ||
       !chrome9_fs_is_generic0_component(&alu->src[1], 2, &second,
                                         depth + 1) ||
       first != second)
      return false;
   *base = first;
   return true;
}

static bool
chrome9_fs_is_packed_texture(nir_def *def, nir_def **base)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   int coord;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   return tex->op == nir_texop_tex &&
          tex->sampler_dim == GLSL_SAMPLER_DIM_2D && !tex->is_array &&
          !tex->is_shadow && !tex->texture_index && !tex->sampler_index &&
          tex->coord_components == 2 && coord >= 0 && tex->num_srcs == 1 &&
          chrome9_fs_is_packed_texcoord(tex->src[coord].src.ssa, base, 0);
}

static bool
chrome9_fs_is_packed_gray_modulate(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *texture_base = NULL, *color_base = NULL;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_packed_texture(alu->src[0].src.ssa,
                                                &texture_base);
   if (texture_first) {
      if (!chrome9_fs_is_generic0_rgbx(alu->src[1].src.ssa, &color_base, 0))
         return false;
   } else {
      if (!chrome9_fs_is_packed_texture(alu->src[1].src.ssa, &texture_base) ||
          !chrome9_fs_is_generic0_rgbx(alu->src[0].src.ssa, &color_base, 0))
         return false;
   }
   return texture_base == color_base && texture_base &&
          texture_base->num_components == 3;
}

static bool
chrome9_fs_is_texture_component(const nir_alu_src *src, unsigned component,
                                unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4)
      return src->swizzle[0] == component && chrome9_fs_is_texture(def, 0);
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == nir_op_mov &&
          chrome9_fs_is_texture_component(&alu->src[0], component,
                                          depth + 1);
}

static bool
chrome9_fs_is_primary_component(const nir_alu_src *src, unsigned component,
                                unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4)
      return src->swizzle[0] == component &&
             chrome9_fs_is_primary_color(def, depth + 1);
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_primary_component(&alu->src[0], component,
                                          depth + 1);
}

static bool
chrome9_fs_is_projected_primary_product(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 0);
   return texture_first ?
      chrome9_fs_is_primary_color(alu->src[1].src.ssa, 0) :
      (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 0) &&
       chrome9_fs_is_primary_color(alu->src[0].src.ssa, 0));
}

static bool
chrome9_fs_is_projected_constant_product(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 0);
   return texture_first ?
      chrome9_fs_is_constant_color(alu->src[1].src.ssa, 0, 0) :
      (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 0) &&
       chrome9_fs_is_constant_color(alu->src[0].src.ssa, 0, 0));
}

static bool
chrome9_fs_is_projected_constant_product_component(const nir_alu_src *src,
                                                   unsigned component,
                                                   nir_def **base,
                                                   unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_projected_constant_product(def))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_projected_constant_product_component(
             &alu->src[0], component, base, depth + 1);
}

static bool
chrome9_fs_is_constant_component(const nir_alu_src *src, unsigned index,
                                 unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4)
      return src->swizzle[0] == component &&
             chrome9_fs_is_constant_color(def, index, depth + 1);
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_constant_component(&alu->src[0], index, component,
                                           depth + 1);
}

static bool
chrome9_fs_is_projected_constant_rgb_alpha(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_projected_constant_rgb_alpha(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_projected_constant_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product && chrome9_fs_is_constant_component(&alu->src[3], 0, 3,
                                                       depth + 1);
}

static bool
chrome9_fs_is_projected_product_component(const nir_alu_src *src,
                                          unsigned component,
                                          nir_def **base, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_projected_primary_product(def))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_projected_product_component(&alu->src[0], component,
                                                     base, depth + 1);
}

static bool
chrome9_fs_is_projected_primary_rgb_alpha(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_projected_primary_rgb_alpha(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_projected_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product &&
          chrome9_fs_is_primary_component(&alu->src[3], 3, depth + 1);
}

static bool
chrome9_fs_is_two_projected_primary_product(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 1);
   return texture_first ?
      chrome9_fs_is_projected_primary_rgb_alpha(alu->src[1].src.ssa, 0) :
      (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 1) &&
       chrome9_fs_is_projected_primary_rgb_alpha(alu->src[0].src.ssa, 0));
}

static bool
chrome9_fs_is_two_projected_product_component(const nir_alu_src *src,
                                              unsigned component,
                                              nir_def **base,
                                              unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_two_projected_primary_product(def))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_two_projected_product_component(
             &alu->src[0], component, base, depth + 1);
}

static bool
chrome9_fs_is_two_projected_primary_rgb_alpha(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_two_projected_primary_rgb_alpha(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_two_projected_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product &&
          chrome9_fs_is_primary_component(&alu->src[3], 3, depth + 1);
}

static bool
chrome9_fs_is_two_projected_full_primary_product(nir_def *def,
                                                 nir_def **inner_product)
{
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *alu;
   bool texture_first;

   if (def->num_components != 4 || instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return false;
   for (unsigned source = 0; source < 2; source++)
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[source].swizzle[c] != c)
            return false;
   texture_first = chrome9_fs_is_projected_texture(alu->src[0].src.ssa, 1);
   nir_def *inner = texture_first ? alu->src[1].src.ssa :
                                    alu->src[0].src.ssa;

   if (!(texture_first ?
         chrome9_fs_is_projected_primary_product(inner) :
         (chrome9_fs_is_projected_texture(alu->src[1].src.ssa, 1) &&
          chrome9_fs_is_projected_primary_product(inner))))
      return false;
   if (inner_product)
      *inner_product = inner;
   return true;
}

static bool
chrome9_fs_is_two_projected_full_product_component(const nir_alu_src *src,
                                                   unsigned component,
                                                   nir_def **base,
                                                   unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def->num_components == 4) {
      if (src->swizzle[0] != component ||
          !chrome9_fs_is_two_projected_full_primary_product(def, NULL))
         return false;
      *base = def;
      return true;
   }
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_two_projected_full_product_component(
             &alu->src[0], component, base, depth + 1);
}

static bool
chrome9_fs_is_def_component(const nir_alu_src *src, nir_def *base,
                            unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;

   if (depth > 8 || !def)
      return false;
   if (def == base)
      return def->num_components == 4 && src->swizzle[0] == component;
   if (def->num_components != 1 || src->swizzle[0])
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   return alu->op == nir_op_mov &&
          chrome9_fs_is_def_component(&alu->src[0], base, component,
                                      depth + 1);
}

static bool
chrome9_fs_is_two_projected_texture0_alpha_primary(nir_def *def,
                                                   unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   nir_def *product = NULL;
   nir_def *inner_product = NULL;

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_two_projected_texture0_alpha_primary(
         alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      nir_def *component_product = NULL;

      if (!chrome9_fs_is_two_projected_full_product_component(
             &alu->src[c], c, &component_product, depth + 1) ||
          (product && product != component_product))
         return false;
      product = component_product;
   }
   return product &&
          chrome9_fs_is_two_projected_full_primary_product(
             product, &inner_product) &&
          chrome9_fs_is_def_component(&alu->src[3], inner_product, 3,
                                      depth + 1);
}

static bool
chrome9_fs_is_rgbx(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4 ||
       instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_rgbx(alu->src[0].src.ssa, depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++)
      if (!chrome9_fs_is_texture_component(&alu->src[c], c, depth + 1))
         return false;
   return !alu->src[3].swizzle[0] &&
          chrome9_fs_is_one(alu->src[3].src.ssa, depth + 1);
}

static bool
chrome9_fs_is_input_at(nir_def *def, unsigned location)
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *intr;
   nir_variable *var;

   if (instr->type != nir_instr_type_intrinsic)
      return false;
   intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      return nir_intrinsic_io_semantics(intr).location == location &&
             !nir_intrinsic_component(intr);
   case nir_intrinsic_load_interpolated_input:
      return chrome9_fs_smooth_pixel_load(intr, location);
   case nir_intrinsic_load_deref:
      var = nir_intrinsic_get_var(intr, 0);
      return chrome9_fs_smooth_variable(var, location);
   default:
      return false;
   }
}

static bool
chrome9_fs_is_input_component(const nir_alu_src *src, unsigned location,
                              unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   unsigned lane;

   if (depth > 12 || !def)
      return false;
   lane = src->swizzle[0];
   if (chrome9_fs_is_input_at(def, location))
      return lane == component && component < def->num_components;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov) {
      nir_alu_src next = alu->src[0];

      next.swizzle[0] = alu->src[0].swizzle[lane];
      return chrome9_fs_is_input_component(&next, location, component,
                                            depth + 1);
   }
   if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
       alu->op == nir_op_vec4) {
      nir_alu_src next;

      if (lane >= nir_op_infos[alu->op].num_inputs)
         return false;
      next = alu->src[lane];
      return chrome9_fs_is_input_component(&next, location, component,
                                            depth + 1);
   }
   return false;
}

static bool
chrome9_fs_texture_component_base(const nir_alu_src *src, unsigned component,
                                  nir_def **base, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   unsigned lane;

   if (depth > 12 || !def)
      return false;
   lane = src->swizzle[0];
   if (def->num_components == 4 && lane == component &&
       chrome9_fs_is_texture(def, 0)) {
      *base = def;
      return true;
   }
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_mov)
      return false;
   nir_alu_src next = alu->src[0];

   next.swizzle[0] = alu->src[0].swizzle[lane];
   return chrome9_fs_texture_component_base(&next, component, base,
                                             depth + 1);
}

static bool
chrome9_fs_is_packed_texture_primary_component(const nir_alu_src *src,
                                               unsigned texture_component,
                                               unsigned input_location,
                                               unsigned input_component,
                                               nir_def **texture,
                                               unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   nir_alu_src a, b;
   unsigned lane;

   if (depth > 12 || !def)
      return false;
   lane = src->swizzle[0];
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov) {
      nir_alu_src next = alu->src[0];

      next.swizzle[0] = alu->src[0].swizzle[lane];
      return chrome9_fs_is_packed_texture_primary_component(
         &next, texture_component, input_location, input_component,
         texture, depth + 1);
   }
   if (alu->op != nir_op_fmul)
      return false;
   a = alu->src[0];
   b = alu->src[1];
   a.swizzle[0] = alu->src[0].swizzle[lane];
   b.swizzle[0] = alu->src[1].swizzle[lane];
   return (chrome9_fs_texture_component_base(&a, texture_component,
                                              texture, depth + 1) &&
           chrome9_fs_is_input_component(&b, input_location,
                                          input_component, depth + 1)) ||
          (chrome9_fs_texture_component_base(&b, texture_component,
                                              texture, depth + 1) &&
           chrome9_fs_is_input_component(&a, input_location,
                                          input_component, depth + 1));
}

static bool
chrome9_fs_is_ioquake3_generic_result(nir_def *def, unsigned depth)
{
   static const unsigned location[4] = {
      VARYING_SLOT_VAR0, VARYING_SLOT_VAR0,
      VARYING_SLOT_VAR1, VARYING_SLOT_VAR1,
   };
   static const unsigned component[4] = { 2, 3, 0, 1 };
   nir_instr *instr;
   nir_def *texture = NULL;

   if (depth > 12 || def->num_components != 4)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_ioquake3_generic_result(alu->src[0].src.ssa,
                                                    depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 4; c++) {
      nir_def *component_texture = NULL;

      if (!chrome9_fs_is_packed_texture_primary_component(
             &alu->src[c], c, location[c], component[c],
             &component_texture, depth + 1) ||
          (texture && texture != component_texture))
         return false;
      texture = component_texture;
   }
   return texture != NULL;
}

static bool
chrome9_fs_is_ioquake3_generic_packed(nir_shader *nir)
{
   unsigned derefs = 0, loads = 0, uniforms = 0, terminates = 0, stores = 0;
   unsigned constants = 0, textures = 0;
   unsigned vec2 = 0, vec4 = 0, fmul = 0, ieq = 0;
   unsigned feq = 0, fge = 0, flt = 0, iand = 0;
   unsigned constant_mask = 0;
   nir_def *result = NULL;

   if (nir->info.stage != MESA_SHADER_FRAGMENT ||
       nir->info.num_textures != 1 || !nir->info.fs.uses_discard)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_deref:
               derefs++;
               break;
            case nir_instr_type_load_const: {
               nir_load_const_instr *load = nir_instr_as_load_const(instr);
               uint32_t value;

               if (load->def.num_components != 1 || load->def.bit_size != 32)
                  return false;
               value = load->value[0].u32;
               if (value == 0)
                  constant_mask |= 1u << 0;
               else if (value == 1)
                  constant_mask |= 1u << 1;
               else if (value == 2)
                  constant_mask |= 1u << 2;
               else if (value == 3)
                  constant_mask |= 1u << 3;
               else if (value == 0x3f000000)
                  constant_mask |= 1u << 4;
               else
                  return false;
               constants++;
               break;
            }
            case nir_instr_type_tex:
               if (!chrome9_fs_is_texture(&nir_instr_as_tex(instr)->def, 0))
                  return false;
               textures++;
               break;
            case nir_instr_type_alu: {
               nir_op op = nir_instr_as_alu(instr)->op;

               if (op == nir_op_vec2)
                  vec2++;
               else if (op == nir_op_vec4)
                  vec4++;
               else if (op == nir_op_fmul)
                  fmul++;
               else if (op == nir_op_ieq)
                  ieq++;
               else if (op == nir_op_feq)
                  feq++;
               else if (op == nir_op_fge)
                  fge++;
               else if (op == nir_op_flt)
                  flt++;
               else if (op == nir_op_iand)
                  iand++;
               else
                  return false;
               break;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               if (intr->intrinsic == nir_intrinsic_load_deref) {
                  loads++;
               } else if (intr->intrinsic == nir_intrinsic_load_uniform) {
                  if (!nir_src_is_const(intr->src[0]) ||
                      nir_src_as_uint(intr->src[0]) ||
                      nir_intrinsic_base(intr) || intr->def.num_components != 1)
                     return false;
                  uniforms++;
               } else if (intr->intrinsic == nir_intrinsic_terminate_if) {
                  terminates++;
               } else if (intr->intrinsic == nir_intrinsic_store_deref) {
                  nir_variable *var = nir_intrinsic_get_var(intr, 0);

                  if (!var || var->data.mode != nir_var_shader_out ||
                      (var->data.location != FRAG_RESULT_COLOR &&
                       var->data.location != FRAG_RESULT_DATA0) || stores++)
                     return false;
                  result = intr->src[1].ssa;
               } else {
                  return false;
               }
               break;
            }
            default:
               return false;
            }
         }
      }
   }
   return derefs == 3 && loads == 2 && uniforms == 1 && terminates == 3 &&
          stores == 1 && constants == 5 && constant_mask == 0x1f &&
          textures == 1 && vec2 == 1 && vec4 == 2 && fmul == 2 &&
          ieq == 3 && feq == 1 && fge == 1 && flt == 1 && iand == 1 &&
          result && chrome9_fs_is_ioquake3_generic_result(result, 0);
}

static bool
chrome9_fs_is_ioquake3_packed_texture(nir_def *def, unsigned unit)
{
   nir_instr *instr = nir_def_instr(def);
   nir_tex_instr *tex;
   nir_instr *coord_instr;
   nir_alu_instr *coord_alu;
   int coord;
   unsigned first = unit ? 2 : 0;

   if (def->num_components != 4 || instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->is_array || tex->is_shadow || tex->texture_index != unit ||
       tex->sampler_index != unit || tex->coord_components != 2 || coord < 0 ||
       tex->num_srcs != 1)
      return false;
   coord_instr = nir_def_instr(tex->src[coord].src.ssa);
   if (coord_instr->type != nir_instr_type_alu)
      return false;
   coord_alu = nir_instr_as_alu(coord_instr);
   return coord_alu->op == nir_op_vec2 &&
          chrome9_fs_is_input_component(&coord_alu->src[0],
                                        VARYING_SLOT_VAR0, first, 0) &&
          chrome9_fs_is_input_component(&coord_alu->src[1],
                                        VARYING_SLOT_VAR0, first + 1, 0);
}

static unsigned
chrome9_fs_ioquake3_lightmap_factors(const nir_alu_src *src,
                                     unsigned component, unsigned depth)
{
   nir_def *def = src->src.ssa;
   nir_instr *instr;
   unsigned lane;

   if (depth > 12 || !def)
      return 0;
   lane = src->swizzle[0];
   if (def->num_components == 4 && lane == component) {
      if (chrome9_fs_is_ioquake3_packed_texture(def, 0))
         return 1u << 0;
      if (chrome9_fs_is_ioquake3_packed_texture(def, 1))
         return 1u << 1;
   }
   if (chrome9_fs_is_input_component(src, VARYING_SLOT_VAR1, component, 0))
      return 1u << 2;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return 0;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov) {
      nir_alu_src next = alu->src[0];

      next.swizzle[0] = alu->src[0].swizzle[lane];
      return chrome9_fs_ioquake3_lightmap_factors(&next, component,
                                                  depth + 1);
   }
   if (alu->op == nir_op_fmul) {
      nir_alu_src a = alu->src[0];
      nir_alu_src b = alu->src[1];
      unsigned ma, mb;

      /* The fixed program evaluates (tex0 * tex1) * primary.  Admit the
       * captured tex0 * (primary * tex1) tree only when NIR explicitly
       * permits reassociation. */
      if (nir_alu_instr_no_reassoc(alu))
         return 0;
      a.swizzle[0] = alu->src[0].swizzle[lane];
      b.swizzle[0] = alu->src[1].swizzle[lane];
      ma = chrome9_fs_ioquake3_lightmap_factors(&a, component, depth + 1);
      mb = chrome9_fs_ioquake3_lightmap_factors(&b, component, depth + 1);
      return ma && mb && !(ma & mb) ? ma | mb : 0;
   }
   return 0;
}

static bool
chrome9_fs_is_ioquake3_lightmap_result(nir_def *def, unsigned depth)
{
   nir_instr *instr;

   if (depth > 12 || def->num_components != 4)
      return false;
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
      for (unsigned c = 0; c < 4; c++)
         if (alu->src[0].swizzle[c] != c)
            return false;
      return chrome9_fs_is_ioquake3_lightmap_result(alu->src[0].src.ssa,
                                                     depth + 1);
   }
   if (alu->op != nir_op_vec4)
      return false;
   for (unsigned c = 0; c < 3; c++)
      if (chrome9_fs_ioquake3_lightmap_factors(&alu->src[c], c,
                                               depth + 1) != 0x7)
         return false;
   return chrome9_fs_ioquake3_lightmap_factors(&alu->src[3], 3,
                                                depth + 1) == 0x5;
}

static bool
chrome9_fs_is_ioquake3_lightmap_packed(nir_shader *nir)
{
   unsigned derefs = 0, loads = 0, uniforms = 0, terminates = 0, stores = 0;
   unsigned constants = 0, textures = 0, texture_mask = 0;
   unsigned vec2 = 0, vec3 = 0, vec4 = 0, fmul = 0, ieq = 0;
   unsigned feq = 0, fge = 0, flt = 0, iand = 0;
   unsigned constant_mask = 0;
   unsigned fmul_no_reassoc = 0, fmul_ctrl = 0;
   nir_def *result = NULL;

   if (nir->info.stage != MESA_SHADER_FRAGMENT ||
       nir->info.num_textures != 2 || !nir->info.fs.uses_discard)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_deref:
               derefs++;
               break;
            case nir_instr_type_load_const: {
               nir_load_const_instr *load = nir_instr_as_load_const(instr);
               uint32_t value;

               if (load->def.num_components != 1 || load->def.bit_size != 32)
                  return false;
               value = load->value[0].u32;
               if (value == 0)
                  constant_mask |= 1u << 0;
               else if (value == 1)
                  constant_mask |= 1u << 1;
               else if (value == 2)
                  constant_mask |= 1u << 2;
               else if (value == 3)
                  constant_mask |= 1u << 3;
               else if (value == 0x3f000000)
                  constant_mask |= 1u << 4;
               else
                  return false;
               constants++;
               break;
            }
            case nir_instr_type_tex: {
               nir_tex_instr *tex = nir_instr_as_tex(instr);

               if (tex->texture_index > 1 ||
                   !chrome9_fs_is_ioquake3_packed_texture(&tex->def,
                                                          tex->texture_index))
                  return false;
               texture_mask |= 1u << tex->texture_index;
               textures++;
               break;
            }
            case nir_instr_type_alu: {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               nir_op op = alu->op;

               if (op == nir_op_vec2)
                  vec2++;
               else if (op == nir_op_vec3)
                  vec3++;
               else if (op == nir_op_vec4)
                  vec4++;
               else if (op == nir_op_fmul) {
                  fmul++;
                  fmul_ctrl |= alu->fp_math_ctrl;
                  fmul_no_reassoc += nir_alu_instr_no_reassoc(alu);
               }
               else if (op == nir_op_ieq)
                  ieq++;
               else if (op == nir_op_feq)
                  feq++;
               else if (op == nir_op_fge)
                  fge++;
               else if (op == nir_op_flt)
                  flt++;
               else if (op == nir_op_iand)
                  iand++;
               else
                  return false;
               break;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               if (intr->intrinsic == nir_intrinsic_load_deref) {
                  loads++;
               } else if (intr->intrinsic == nir_intrinsic_load_uniform) {
                  if (!nir_src_is_const(intr->src[0]) ||
                      nir_src_as_uint(intr->src[0]) ||
                      nir_intrinsic_base(intr) || intr->def.num_components != 1)
                     return false;
                  uniforms++;
               } else if (intr->intrinsic == nir_intrinsic_terminate_if) {
                  terminates++;
               } else if (intr->intrinsic == nir_intrinsic_store_deref) {
                  nir_variable *var = nir_intrinsic_get_var(intr, 0);

                  if (!var || var->data.mode != nir_var_shader_out ||
                      (var->data.location != FRAG_RESULT_COLOR &&
                       var->data.location != FRAG_RESULT_DATA0) || stores++)
                     return false;
                  result = intr->src[1].ssa;
               } else {
                  return false;
               }
               break;
            }
            default:
               return false;
            }
         }
      }
   }
   bool result_match = result &&
      chrome9_fs_is_ioquake3_lightmap_result(result, 0);
   bool match = derefs == 3 && loads == 2 && uniforms == 1 &&
      terminates == 3 && stores == 1 && constants == 5 &&
      constant_mask == 0x1f && textures == 2 && texture_mask == 0x3 &&
      vec2 == 2 && vec3 == 0 && vec4 == 1 && fmul == 3 && ieq == 3 &&
      feq == 1 && fge == 1 && flt == 1 && iand == 1 && result_match;

   if (debug_get_bool_option("CHROME9_DEBUG_LIGHTMAP_MATCH", false))
      fprintf(stderr, "chrome9: lightmap match=%u result=%u "
              "deref=%u load=%u uniform=%u term=%u store=%u const=%u/%#x "
              "tex=%u/%#x vec=%u/%u/%u mul=%u(no-reassoc=%u ctrl=%#x) "
              "cmp=%u/%u/%u/%u/%u\n",
              match, result_match, derefs, loads, uniforms, terminates, stores,
              constants, constant_mask, textures, texture_mask, vec2, vec3,
              vec4, fmul, fmul_no_reassoc, fmul_ctrl, ieq, feq, fge, flt,
              iand);
   return match;
}

static bool
chrome9_fs_pair(unsigned a, unsigned b, unsigned x, unsigned y)
{
   return (a == x && b == y) || (a == y && b == x);
}

static unsigned
chrome9_fs_result(nir_def *def, uint32_t *literal_color)
{
   nir_instr *instr;
   nir_alu_instr *alu;
   nir_def *generic = NULL;
   unsigned a, b;

   if (chrome9_fs_is_flat_primary_color(def, 0))
      return CHROME9_FS_PRIMARY_COLOR;

   if (chrome9_fs_is_literal_color(def, literal_color))
      return CHROME9_FS_LITERAL_COLOR;

   if (chrome9_fs_is_flat_generic0_rgbx(def, 0))
      return CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX;

   if (chrome9_fs_is_generic0_rgba(def, 0))
      return CHROME9_FS_PRIMARY_GENERIC_RGBA;
   if (chrome9_fs_is_generic0_blue_rgbx(def, &generic, 0))
      return CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX;
   if (chrome9_fs_is_generic0_vec3_rgbx(def, &generic, 0))
      return CHROME9_FS_PRIMARY_GENERIC_RGBX;
   if (chrome9_fs_is_generic0_xxy_rgbx(def, &generic, 0))
      return CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX;
   if (chrome9_fs_is_generic0_rgbx(def, &generic, 0) &&
       generic->num_components == 1)
      return CHROME9_FS_PRIMARY_SCALAR_RGBX;
   if (chrome9_fs_is_packed_gray_modulate(def))
      return CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE;
   if (chrome9_fs_is_projected_constant_product(def))
      return CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE;
   if (chrome9_fs_is_projected_constant_rgb_alpha(def, 0))
      return CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA;
   if (chrome9_fs_is_projected_primary_rgb_alpha(def, 0))
      return CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA;
   if (chrome9_fs_is_two_projected_primary_rgb_alpha(def, 0))
      return CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA;
   if (chrome9_fs_is_two_projected_texture0_alpha_primary(def, 0))
      return CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY;
   if (chrome9_fs_is_projected_primary_product(def))
      return CHROME9_FS_TEXTURE_PROJECTED_MODULATE;
   if (chrome9_fs_is_rgbx(def, 0))
      return CHROME9_FS_TEXTURE_RGBX;
   switch (chrome9_fs_leaf(def, 0)) {
   case CHROME9_FS_LEAF_PRIMARY:
      return CHROME9_FS_PRIMARY_COLOR;
   case CHROME9_FS_LEAF_CONSTANT0:
      return CHROME9_FS_CONSTANT_COLOR;
   case CHROME9_FS_LEAF_TEXTURE0:
      return CHROME9_FS_TEXTURE_REPLACE;
   default:
      break;
   }
   instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu ||
       (alu = nir_instr_as_alu(instr))->op != nir_op_fmul)
      return CHROME9_FS_UNSUPPORTED;
   a = chrome9_fs_source_leaf(&alu->src[0], 0);
   b = chrome9_fs_source_leaf(&alu->src[1], 0);
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_PRIMARY))
      return CHROME9_FS_TEXTURE_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_CONSTANT0))
      return CHROME9_FS_TEXTURE_CONSTANT_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_CONSTANT0_ALPHA))
      return CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_TEXTURE0,
                       CHROME9_FS_LEAF_TEXTURE1))
      return CHROME9_FS_TWO_TEXTURE_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_CONSTANT0,
                       CHROME9_FS_LEAF_CONSTANT1))
      return CHROME9_FS_CONSTANT_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_CONSTANT0,
                       CHROME9_FS_LEAF_CONSTANT1_ALPHA))
      return CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE;
   if (chrome9_fs_pair(a, b, CHROME9_FS_LEAF_CONSTANT0_ALPHA,
                       CHROME9_FS_LEAF_CONSTANT1))
      return CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE;
   return CHROME9_FS_UNSUPPORTED;
}

static unsigned
chrome9_fs_classify(nir_shader *nir, uint32_t *literal_color)
{
   unsigned result = CHROME9_FS_UNSUPPORTED;
   unsigned texture_count = 0;
   unsigned constant_count = 0;
   bool written = false, valid = true;

   /* Linked generic_fp packs texture ST in VAR0.xy and primary RGBA in
    * VAR0.zw/VAR1.xy.  The semantic matcher closes both the color DAG and
    * complete integer alpha-test tree; draw time admits proven modes 0/1. */
   if (chrome9_fs_is_ioquake3_generic_packed(nir))
      return CHROME9_FS_IOQUAKE3_GENERIC_PACKED;
   if (chrome9_fs_is_ioquake3_lightmap_packed(nir))
      return CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            nir_variable *var;
            unsigned location;

            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               nir_def *packed_base = NULL;

               texture_count++;
               if ((!chrome9_fs_is_texture(&tex->def, tex->texture_index) &&
                    !chrome9_fs_is_projected_texture(&tex->def,
                                                     tex->texture_index) &&
                    !chrome9_fs_is_packed_texture(&tex->def,
                                                  &packed_base)) ||
                   tex->texture_index > 1)
                  valid = false;
               continue;
            }
            if (instr->type == nir_instr_type_deref ||
                instr->type == nir_instr_type_load_const ||
                instr->type == nir_instr_type_alu)
               continue;
            if (instr->type != nir_instr_type_intrinsic) {
               valid = false;
               continue;
            }
            intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_uniform:
            case nir_intrinsic_load_ubo:
            case nir_intrinsic_load_ubo_vec4:
               constant_count++;
               break;
            case nir_intrinsic_load_input:
               /* With pipe_caps.flatshade=false, state tracker flat color
                * arrives as a direct load.  The immediate packet currently
                * emits Gouraud A23 state and has no provoking-vertex fixup,
                * so fail closed until that topology-aware path exists. */
               valid = false;
               break;
            case nir_intrinsic_load_interpolated_input:
            case nir_intrinsic_load_barycentric_pixel:
            case nir_intrinsic_load_barycentric_centroid:
            case nir_intrinsic_load_barycentric_sample:
            case nir_intrinsic_load_deref:
               break;
            case nir_intrinsic_store_output:
               location = nir_intrinsic_io_semantics(intr).location;
               if ((location != FRAG_RESULT_COLOR &&
                    location != FRAG_RESULT_DATA0) ||
                   written ||
                   !(result = chrome9_fs_result(intr->src[0].ssa,
                                                literal_color)))
                  valid = false;
               written = true;
               break;
            case nir_intrinsic_store_deref:
               var = nir_intrinsic_get_var(intr, 0);
               if (!var || var->data.mode != nir_var_shader_out ||
                   (var->data.location != FRAG_RESULT_COLOR &&
                    var->data.location != FRAG_RESULT_DATA0) ||
                   written ||
                   !(result = chrome9_fs_result(intr->src[1].ssa,
                                                literal_color)))
                  valid = false;
               written = true;
               break;
            default:
               valid = false;
               break;
            }
         }
      }
   }
   if (!valid || !written)
      return CHROME9_FS_UNSUPPORTED;
   switch (result) {
   case CHROME9_FS_PRIMARY_COLOR:
   case CHROME9_FS_PRIMARY_GENERIC_RGBA:
   case CHROME9_FS_PRIMARY_GENERIC_RGBX:
   case CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX:
   case CHROME9_FS_PRIMARY_SCALAR_RGBX:
   case CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX:
   case CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX:
   case CHROME9_FS_LITERAL_COLOR:
      valid = !texture_count && !constant_count;
      break;
   case CHROME9_FS_CONSTANT_COLOR:
      valid = !texture_count && constant_count == 1;
      break;
   case CHROME9_FS_CONSTANT_MODULATE:
   case CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE:
   case CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE:
      valid = !texture_count && constant_count == 2;
      break;
   case CHROME9_FS_TEXTURE_REPLACE:
   case CHROME9_FS_TEXTURE_RGBX:
   case CHROME9_FS_TEXTURE_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA:
   case CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA:
   case CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE:
      valid = texture_count == 1 &&
              ((result == CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE ||
                result == CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA) ?
                 constant_count == 1 : !constant_count);
      break;
   case CHROME9_FS_TEXTURE_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE:
      valid = texture_count == 1 && constant_count == 1;
      break;
   case CHROME9_FS_TWO_TEXTURE_MODULATE:
   case CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA:
   case CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY:
      valid = texture_count == 2 && !constant_count;
      break;
   default:
      valid = false;
      break;
   }
   if (!valid)
      return CHROME9_FS_UNSUPPORTED;
   return result;
}

static bool
chrome9_fs_native_key(unsigned program, struct chrome9_fs_key *key)
{
   memset(key, 0, sizeof(*key));

   switch (program) {
   case CHROME9_FS_PRIMARY_COLOR:
      key->op = CHROME9_FS_OP_MOV;
      key->src[0] = CHROME9_FS_LEAF_V0;
      return true;
   case CHROME9_FS_CONSTANT_COLOR:
      key->op = CHROME9_FS_OP_MOV;
      key->src[0] = CHROME9_FS_LEAF_C0;
      return true;
   case CHROME9_FS_CONSTANT_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_C0;
      key->src[1] = CHROME9_FS_LEAF_C1;
      return true;
   case CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_C0;
      key->src[1] = CHROME9_FS_LEAF_C1_A;
      return true;
   case CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_C0_A;
      key->src[1] = CHROME9_FS_LEAF_C1;
      return true;
   case CHROME9_FS_TEXTURE_REPLACE:
      key->op = CHROME9_FS_OP_MOV;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      return true;
   case CHROME9_FS_TEXTURE_RGBX:
      key->op = CHROME9_FS_OP_RGBX_C0X;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_C0;
      return true;
   case CHROME9_FS_TEXTURE_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_V0;
      return true;
   case CHROME9_FS_TEXTURE_CONSTANT_MODULATE:
   case CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_C0;
      return true;
   case CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_C0_A;
      return true;
   case CHROME9_FS_TWO_TEXTURE_MODULATE:
      key->op = CHROME9_FS_OP_MUL;
      key->src[0] = CHROME9_FS_LEAF_TEX0;
      key->src[1] = CHROME9_FS_LEAF_TEX1;
      return true;
   default:
      return false;
   }
}

static bool
chrome9_backend_alpha_leaf(enum chrome9_fs_leaf *leaf)
{
   switch (*leaf) {
   case CHROME9_FS_LEAF_V0:
      *leaf = CHROME9_FS_LEAF_V0_A;
      return true;
   case CHROME9_FS_LEAF_C0:
      *leaf = CHROME9_FS_LEAF_C0_A;
      return true;
   case CHROME9_FS_LEAF_C1:
      *leaf = CHROME9_FS_LEAF_C1_A;
      return true;
   default:
      return false;
   }
}

static bool
chrome9_backend_primary(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *load;
   nir_deref_instr *deref;
   nir_variable *var;

   if (def->num_components != 4 || def->bit_size != 32 ||
       instr->type != nir_instr_type_intrinsic)
      return false;
   load = nir_instr_as_intrinsic(instr);
   if (load->intrinsic == nir_intrinsic_load_deref) {
      deref = nir_src_as_deref(load->src[0]);
      var = nir_intrinsic_get_var(load, 0);
      return deref && deref->deref_type == nir_deref_type_var &&
             chrome9_fs_smooth_variable(var, VARYING_SLOT_COL0);
   }
   return chrome9_fs_smooth_pixel_load(load, VARYING_SLOT_COL0);
}

/* The backend deliberately unwraps MOV only.  Treating FSAT as a copy would
 * miscompile an FMUL operand whenever the source lies outside [0,1]. */
static bool
chrome9_backend_leaf(nir_def *def, unsigned depth,
                     enum chrome9_fs_leaf *leaf)
{
   nir_instr *instr;

   if (!def || depth > 8 || def->num_components != 4 || def->bit_size != 32)
      return false;
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      bool alpha = true, identity = true;

      if (alu->op != nir_op_mov)
         return false;
      for (unsigned c = 0; c < 4; c++) {
         identity &= alu->src[0].swizzle[c] == c;
         alpha &= alu->src[0].swizzle[c] == 3;
      }
      if ((!identity && !alpha) ||
          !chrome9_backend_leaf(alu->src[0].src.ssa, depth + 1, leaf))
         return false;
      return identity || chrome9_backend_alpha_leaf(leaf);
   }
   if (chrome9_backend_primary(def)) {
      *leaf = CHROME9_FS_LEAF_V0;
      return true;
   }
   if (chrome9_fs_is_constant_color(def, 0, 0)) {
      *leaf = CHROME9_FS_LEAF_C0;
      return true;
   }
   if (chrome9_fs_is_constant_color(def, 1, 0)) {
      *leaf = CHROME9_FS_LEAF_C1;
      return true;
   }
   return false;
}

static bool
chrome9_backend_source(const nir_alu_src *source,
                       enum chrome9_fs_leaf *leaf)
{
   bool alpha = true, identity = true;

   if (!source->src.ssa || source->src.ssa->num_components != 4)
      return false;
   for (unsigned c = 0; c < 4; c++) {
      identity &= source->swizzle[c] == c;
      alpha &= source->swizzle[c] == 3;
   }
   if ((!identity && !alpha) ||
       !chrome9_backend_leaf(source->src.ssa, 0, leaf))
      return false;
   return identity || chrome9_backend_alpha_leaf(leaf);
}

struct chrome9_ps_nir_builder {
   struct chrome9_ps_ir ir;
   struct {
      nir_def *def;
      struct chrome9_ps_source source;
      bool complete;
   } value[CHROME9_PS_MAX_INSTRUCTIONS];
   uint32_t immediate[32][4];
   uint32_t immediate_mask;
   uint32_t uniform_mask;
   uint8_t value_count;
   uint8_t next_temp;
};

static bool
chrome9_ps_source_from_leaf(enum chrome9_fs_leaf leaf,
                            struct chrome9_ps_source *source)
{
   bool alpha = false;

   memset(source, 0, sizeof(*source));
   switch (leaf) {
   case CHROME9_FS_LEAF_V0_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_V0:
      source->file = CHROME9_PS_SOURCE_INPUT;
      source->id = 0;
      break;
   case CHROME9_FS_LEAF_V1_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_V1:
      source->file = CHROME9_PS_SOURCE_INPUT;
      source->id = 1;
      break;
   case CHROME9_FS_LEAF_C0_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_C0:
      source->file = CHROME9_PS_SOURCE_CONSTANT;
      source->id = 0;
      break;
   case CHROME9_FS_LEAF_C1_A:
      alpha = true;
      FALLTHROUGH;
   case CHROME9_FS_LEAF_C1:
      source->file = CHROME9_PS_SOURCE_CONSTANT;
      source->id = 1;
      break;
   default:
      return false;
   }
   for (unsigned c = 0; c < 4; c++)
      source->swizzle[c] = alpha ? CHROME9_PS_SWIZZLE_W : c;
   return true;
}

static bool chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder,
                                 nir_def *def,
                                 struct chrome9_ps_source *source);

static bool
chrome9_ps_lower_alu_source(struct chrome9_ps_nir_builder *builder,
                            const nir_alu_src *nir_source,
                            unsigned components,
                            struct chrome9_ps_source *source)
{
   struct chrome9_ps_source base;

   if (!nir_source->src.ssa || !components || components > 4 ||
       nir_source->src.ssa->bit_size != 32 ||
       !chrome9_ps_lower_def(builder, nir_source->src.ssa, &base))
      return false;
   *source = base;
   for (unsigned c = 0; c < components; c++) {
      unsigned swizzle = nir_source->swizzle[c];

      if (swizzle >= nir_source->src.ssa->num_components)
         return false;
      source->swizzle[c] = base.swizzle[swizzle];
   }
   for (unsigned c = components; c < 4; c++)
      source->swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
   return true;
}

static bool
chrome9_ps_immediate(struct chrome9_ps_nir_builder *builder,
                     const uint32_t value[4], unsigned components,
                     struct chrome9_ps_source *source)
{
   unsigned id = 32;

   if (!components || components > 4)
      return false;
   for (unsigned candidate = 16; candidate < 32; candidate++) {
      if (!(builder->immediate_mask & (1u << candidate)))
         continue;
      if (!memcmp(builder->immediate[candidate], value,
                  sizeof(builder->immediate[candidate]))) {
         id = candidate;
         break;
      }
   }
   if (id == 32) {
      for (unsigned candidate = 32; candidate-- > 16;) {
         if (!((builder->immediate_mask | builder->uniform_mask) &
               (1u << candidate))) {
            id = candidate;
            break;
         }
      }
      if (id == 32)
         return false;
      memcpy(builder->immediate[id], value,
             sizeof(builder->immediate[id]));
      builder->immediate_mask |= 1u << id;
   }
   memset(source, 0, sizeof(*source));
   source->file = CHROME9_PS_SOURCE_CONSTANT;
   source->id = id;
   for (unsigned c = 0; c < 4; c++)
      source->swizzle[c] = c < components ? c : CHROME9_PS_SWIZZLE_ZERO;
   return true;
}

static bool
chrome9_ps_leaf(struct chrome9_ps_nir_builder *builder, nir_def *def,
                struct chrome9_ps_source *source)
{
   enum chrome9_fs_leaf leaf;
   nir_instr *instr = nir_def_instr(def);

   if (chrome9_backend_leaf(def, 0, &leaf))
      return chrome9_ps_source_from_leaf(leaf, source);
   if (instr->type == nir_instr_type_load_const) {
      nir_load_const_instr *load = nir_instr_as_load_const(instr);
      uint32_t value[4] = { 0 };

      if (def->bit_size != 32 || !def->num_components ||
          def->num_components > 4)
         return false;
      for (unsigned c = 0; c < def->num_components; c++)
         value[c] = load->value[c].u32;
      return chrome9_ps_immediate(builder, value, def->num_components,
                                  source);
   }
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
      unsigned location = ~0u;

      if (load->intrinsic == nir_intrinsic_load_uniform &&
          nir_src_is_const(load->src[0]) && !nir_intrinsic_base(load) &&
          def->bit_size == 32 && def->num_components &&
          def->num_components <= 4) {
         unsigned id = nir_src_as_uint(load->src[0]);

         if (id >= 32 || !(builder->uniform_mask & (1u << id)))
            return false;
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_CONSTANT;
         source->id = id;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      switch (load->intrinsic) {
      case nir_intrinsic_load_input:
         if (!nir_intrinsic_component(load))
            location = nir_intrinsic_io_semantics(load).location;
         break;
      case nir_intrinsic_load_interpolated_input:
         for (unsigned candidate = 0; candidate < 8; candidate++) {
            if (chrome9_fs_smooth_pixel_load(load,
                                             VARYING_SLOT_VAR0 + candidate)) {
               location = VARYING_SLOT_VAR0 + candidate;
               break;
            }
         }
         break;
      case nir_intrinsic_load_deref: {
         nir_deref_instr *deref = nir_src_as_deref(load->src[0]);
         nir_variable *var = nir_intrinsic_get_var(load, 0);

         if (deref && deref->deref_type == nir_deref_type_var && var &&
             var->data.location >= VARYING_SLOT_VAR0 &&
             var->data.location < VARYING_SLOT_VAR0 + 8 &&
             chrome9_fs_smooth_variable(var, var->data.location))
            location = var->data.location;
         break;
      }
      default:
         break;
      }
      if (location >= VARYING_SLOT_VAR0 &&
          location < VARYING_SLOT_VAR0 + 8 && def->bit_size == 32 &&
          def->num_components && def->num_components <= 4) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_TEXTURE;
         source->id = location - VARYING_SLOT_VAR0;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }
   }
   return false;
}

static bool
chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder, nir_def *def,
                     struct chrome9_ps_source *source)
{
   nir_instr *instr;
   nir_alu_instr *alu;
   struct chrome9_ps_instruction *native;
   struct chrome9_ps_source operands[3] = { 0 };
   enum chrome9_ps_opcode op;
   unsigned source_count;
   unsigned source_components[3];
   unsigned components;
   unsigned value;
   uint8_t temp;

   if (!def || def->bit_size != 32 || !def->num_components ||
       def->num_components > 4)
      return false;
   for (value = 0; value < builder->value_count; value++) {
      if (builder->value[value].def != def)
         continue;
      if (!builder->value[value].complete)
         return false;
      *source = builder->value[value].source;
      return true;
   }
   if (chrome9_ps_leaf(builder, def, source))
      return true;
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *vec = nir_instr_as_alu(instr);
      unsigned vector_components = 0;
      struct chrome9_ps_source component[4] = { 0 };
      struct chrome9_ps_source vector = { 0 };
      bool alias = true;

      if (vec->op == nir_op_vec2)
         vector_components = 2;
      else if (vec->op == nir_op_vec3)
         vector_components = 3;
      else if (vec->op == nir_op_vec4)
         vector_components = 4;
      if (vector_components) {
         for (unsigned c = 0; c < vector_components; c++) {
            if (!chrome9_ps_lower_alu_source(builder, &vec->src[c], 1,
                                              &component[c]))
               return false;
            if (!c) {
               vector = component[c];
            } else if (component[c].file != vector.file ||
                       component[c].id != vector.id ||
                       component[c].modifier != vector.modifier) {
               alias = false;
            }
            vector.swizzle[c] = component[c].swizzle[0];
         }
         if (alias) {
            for (unsigned c = vector_components; c < 4; c++)
               vector.swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
            *source = vector;
            return true;
         }
         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
             builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
            return false;
         temp = builder->next_temp++;
         value = builder->value_count++;
         builder->value[value].def = def;
         builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
         builder->value[value].source.id = temp;
         for (unsigned c = 0; c < 4; c++)
            builder->value[value].source.swizzle[c] = c < vector_components ?
               c : CHROME9_PS_SWIZZLE_ZERO;
         for (unsigned remaining = (1u << vector_components) - 1;
              remaining;) {
            struct chrome9_ps_instruction *move;
            unsigned first = __builtin_ctz(remaining);
            unsigned mask = 0;

            for (unsigned c = 0; c < vector_components; c++) {
               if (!(remaining & (1u << c)))
                  continue;
               if (component[c].file == component[first].file &&
                   component[c].id == component[first].id &&
                   component[c].modifier == component[first].modifier)
                  mask |= 1u << c;
            }
            if (builder->ir.count >= CHROME9_PS_MAX_ALU)
               return false;
            move = &builder->ir.instruction[builder->ir.count++];
            memset(move, 0, sizeof(*move));
            move->op = CHROME9_PS_OPCODE_MOV;
            move->destination.file = CHROME9_PS_DESTINATION_TEMP;
            move->destination.id = temp;
            move->destination.writemask = mask;
            move->source[0] = component[first];
            for (unsigned c = 0; c < 4; c++)
               move->source[0].swizzle[c] =
                  (mask & (1u << c)) ? component[c].swizzle[0] :
                  CHROME9_PS_SWIZZLE_ZERO;
            remaining &= ~mask;
         }
         builder->value[value].complete = true;
         *source = builder->value[value].source;
         return true;
      }
   }
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      struct chrome9_ps_source coordinate;
      int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);

      if (tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
          tex->is_array || tex->is_shadow || tex->texture_index >= 8 ||
          tex->sampler_index != tex->texture_index || coord < 0 ||
          tex->src[coord].src.ssa->num_components != 2 ||
          builder->ir.texture_count >= CHROME9_PS_MAX_TAU ||
          builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
          builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
         return false;
      for (unsigned s = 0; s < tex->num_srcs; s++)
         if ((int)s != coord)
            return false;
      if (!chrome9_ps_lower_def(builder, tex->src[coord].src.ssa,
                                &coordinate) ||
          coordinate.file != CHROME9_PS_SOURCE_TEXTURE ||
          coordinate.swizzle[0] > CHROME9_PS_SWIZZLE_W ||
          coordinate.swizzle[1] > CHROME9_PS_SWIZZLE_W)
         return false;
      temp = builder->next_temp++;
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
      builder->value[value].source.id = temp;
      for (unsigned c = 0; c < 4; c++)
         builder->value[value].source.swizzle[c] = c;
      builder->ir.texture[builder->ir.texture_count++] =
         (struct chrome9_ps_texture_instruction) {
            .sampler = tex->sampler_index,
            .coordinate = coordinate.id,
            .destination = temp,
            .swizzle = ((uint16_t)coordinate.swizzle[0] << 10) |
                       ((uint16_t)coordinate.swizzle[1] << 8) |
                       ((uint16_t)CHROME9_PS_SWIZZLE_W << 6) |
                       ((uint16_t)CHROME9_PS_SWIZZLE_W << 4),
         };
      builder->value[value].complete = true;
      *source = builder->value[value].source;
      return true;
   }
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   components = alu->def.num_components;
   for (unsigned s = 0; s < ARRAY_SIZE(source_components); s++)
      source_components[s] = components;
   if (alu->op == nir_op_fpow) {
      struct chrome9_ps_instruction *log2, *multiply, *exp2;
      uint8_t log_temp, multiply_temp;

      /* Chrome9 implements POW as its native scalar LOG2, MUL and EXP2
       * sequence.  GLSL already leaves non-positive bases undefined, so this
       * is the general semantic lowering rather than an application-specific
       * integer-exponent substitution.  Vector POW is scalarized by the NIR
       * frontend before this backend grows multi-lane scalar scheduling. */
      if (components != 1 ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[0], 1,
                                       &operands[0]) ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[1], 1,
                                       &operands[1]) ||
          builder->ir.count > CHROME9_PS_MAX_ALU - 3 ||
          builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - 3 ||
          builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
         return false;

      log_temp = builder->next_temp++;
      multiply_temp = builder->next_temp++;
      temp = builder->next_temp++;
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
      builder->value[value].source.id = temp;
      builder->value[value].source.swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         builder->value[value].source.swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;

      log2 = &builder->ir.instruction[builder->ir.count++];
      memset(log2, 0, sizeof(*log2));
      log2->op = CHROME9_PS_OPCODE_LOG2;
      log2->destination.file = CHROME9_PS_DESTINATION_TEMP;
      log2->destination.id = log_temp;
      log2->destination.writemask = 1;
      log2->source[0] = operands[0];

      multiply = &builder->ir.instruction[builder->ir.count++];
      memset(multiply, 0, sizeof(*multiply));
      multiply->op = CHROME9_PS_OPCODE_MUL;
      multiply->destination.file = CHROME9_PS_DESTINATION_TEMP;
      multiply->destination.id = multiply_temp;
      multiply->destination.writemask = 1;
      multiply->source[0].file = CHROME9_PS_SOURCE_TEMP;
      multiply->source[0].id = log_temp;
      multiply->source[0].swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         multiply->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
      multiply->source[1] = operands[1];

      exp2 = &builder->ir.instruction[builder->ir.count++];
      memset(exp2, 0, sizeof(*exp2));
      exp2->op = CHROME9_PS_OPCODE_EXP2;
      exp2->destination.file = CHROME9_PS_DESTINATION_TEMP;
      exp2->destination.id = temp;
      exp2->destination.writemask = 1;
      exp2->source[0].file = CHROME9_PS_SOURCE_TEMP;
      exp2->source[0].id = multiply_temp;
      exp2->source[0].swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         exp2->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;

      builder->value[value].complete = true;
      *source = builder->value[value].source;
      return true;
   }
   switch (alu->op) {
   case nir_op_mov:
      return chrome9_ps_lower_alu_source(builder, &alu->src[0], components,
                                         source);
   case nir_op_fmul:
      op = CHROME9_PS_OPCODE_MUL;
      source_count = 2;
      break;
   case nir_op_fadd:
      op = CHROME9_PS_OPCODE_ADD;
      source_count = 2;
      break;
   case nir_op_fsub:
      op = CHROME9_PS_OPCODE_SUB;
      source_count = 2;
      break;
   case nir_op_ffma:
      op = CHROME9_PS_OPCODE_MAD;
      source_count = 3;
      break;
   case nir_op_fmin:
      op = CHROME9_PS_OPCODE_MIN;
      source_count = 2;
      break;
   case nir_op_fmax:
      op = CHROME9_PS_OPCODE_MAX;
      source_count = 2;
      break;
   case nir_op_fdot3:
      op = CHROME9_PS_OPCODE_DP3;
      source_count = 2;
      source_components[0] = source_components[1] = 3;
      break;
   case nir_op_fdot4:
      op = CHROME9_PS_OPCODE_DP4;
      source_count = 2;
      source_components[0] = source_components[1] = 4;
      break;
   case nir_op_fexp2:
      op = CHROME9_PS_OPCODE_EXP2;
      source_count = 1;
      break;
   case nir_op_flog2:
      op = CHROME9_PS_OPCODE_LOG2;
      source_count = 1;
      break;
   case nir_op_frcp:
      op = CHROME9_PS_OPCODE_RCP;
      source_count = 1;
      break;
   case nir_op_frsq:
      op = CHROME9_PS_OPCODE_RSQ;
      source_count = 1;
      break;
   case nir_op_fabs:
      op = CHROME9_PS_OPCODE_ABS;
      source_count = 1;
      break;
   case nir_op_ffract:
      op = CHROME9_PS_OPCODE_FRC;
      source_count = 1;
      break;
   case nir_op_fneg:
      op = CHROME9_PS_OPCODE_MUL;
      source_count = 1;
      break;
   default:
      return false;
   }
   if (builder->ir.count >= CHROME9_PS_MAX_ALU ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   temp = builder->next_temp++;
   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
   builder->value[value].source.id = temp;
   for (unsigned c = 0; c < 4; c++)
      builder->value[value].source.swizzle[c] = c < components ?
         c : CHROME9_PS_SWIZZLE_ZERO;
   for (unsigned s = 0; s < source_count; s++)
      if (!chrome9_ps_lower_alu_source(builder, &alu->src[s],
                                       source_components[s], &operands[s]))
         return false;
   if (alu->op == nir_op_fneg) {
      static const uint32_t negative_one[4] = {
         0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000,
      };

      if (!chrome9_ps_immediate(builder, negative_one, components,
                                &operands[1]))
         return false;
      source_count = 2;
   }
   if (builder->ir.count >= CHROME9_PS_MAX_ALU)
      return false;
   native = &builder->ir.instruction[builder->ir.count];
   memset(native, 0, sizeof(*native));
   native->op = op;
   native->destination.file = CHROME9_PS_DESTINATION_TEMP;
   native->destination.id = temp;
   native->destination.writemask = (1u << components) - 1;
   for (unsigned s = 0; s < source_count; s++)
      native->source[s] = operands[s];
   builder->ir.count++;
   builder->value[value].complete = true;
   *source = builder->value[value].source;
   return true;
}

static bool
chrome9_compile_ps_nir(nir_shader *nir, struct chrome9_ps_ir *ir,
                       struct chrome9_ps_program *program,
                       uint32_t *uniform_mask, bool *lowered)
{
   struct chrome9_ps_nir_builder builder = { 0 };
   struct chrome9_ps_instruction *terminal;
   struct chrome9_ps_source operands[3] = { 0 };
   enum chrome9_ps_opcode op;
   unsigned source_count = 0;
   nir_block *block;
   nir_def *result = NULL;

   *lowered = false;
   if (!chrome9_fs_straight_line_entrypoint(nir, NULL, &block) ||
       nir->info.float_controls_execution_mode)
      return false;
   nir_foreach_instr(instr, block) {
      nir_intrinsic_instr *intr;
      nir_variable *var;
      unsigned location;
      nir_def *stored;

      if (instr->type == nir_instr_type_alu ||
          instr->type == nir_instr_type_deref ||
          instr->type == nir_instr_type_load_const ||
          instr->type == nir_instr_type_tex)
         continue;
      if (instr->type != nir_instr_type_intrinsic)
         return false;
      intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic == nir_intrinsic_store_output) {
         struct nir_io_semantics semantics =
            nir_intrinsic_io_semantics(intr);

         location = semantics.location;
         if (nir_intrinsic_component(intr) ||
             semantics.dual_source_blend_index ||
             nir_intrinsic_write_mask(intr) != 0xf ||
             !nir_src_is_const(intr->src[1]) ||
             nir_src_as_uint(intr->src[1]))
            return false;
         stored = intr->src[0].ssa;
      } else if (intr->intrinsic == nir_intrinsic_store_deref) {
         nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);

         var = nir_intrinsic_get_var(intr, 0);
         if (!deref || deref->deref_type != nir_deref_type_var ||
             !var || var->data.mode != nir_var_shader_out ||
             var->data.index || var->data.location_frac ||
             nir_intrinsic_write_mask(intr) != 0xf)
            return false;
         location = var->data.location;
         stored = intr->src[1].ssa;
      } else if (intr->intrinsic == nir_intrinsic_load_uniform) {
         unsigned id;

         if (!nir_src_is_const(intr->src[0]) || nir_intrinsic_base(intr) ||
             intr->def.bit_size != 32 || !intr->def.num_components ||
             intr->def.num_components > 4)
            return false;
         id = nir_src_as_uint(intr->src[0]);
         if (id >= 32)
            return false;
         builder.uniform_mask |= 1u << id;
         continue;
      } else if (intr->intrinsic ==
                    nir_intrinsic_load_barycentric_pixel ||
                 intr->intrinsic ==
                    nir_intrinsic_load_interpolated_input ||
                 intr->intrinsic == nir_intrinsic_load_deref ||
                 intr->intrinsic == nir_intrinsic_load_ubo ||
                 intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
         continue;
      } else {
         return false;
      }
      if ((location != FRAG_RESULT_COLOR &&
           location != FRAG_RESULT_DATA0) || result)
         return false;
      result = stored;
   }
   if (!result || result->num_components != 4 || result->bit_size != 32)
      return false;

   nir_instr *result_instr = nir_def_instr(result);

   if (result_instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(result_instr);

      switch (alu->op) {
      case nir_op_fmul:
         op = CHROME9_PS_OPCODE_MUL;
         source_count = 2;
         break;
      case nir_op_fadd:
         op = CHROME9_PS_OPCODE_ADD;
         source_count = 2;
         break;
      case nir_op_fsub:
         op = CHROME9_PS_OPCODE_SUB;
         source_count = 2;
         break;
      case nir_op_ffma:
         op = CHROME9_PS_OPCODE_MAD;
         source_count = 3;
         break;
      case nir_op_fmin:
         op = CHROME9_PS_OPCODE_MIN;
         source_count = 2;
         break;
      default:
         source_count = 0;
         break;
      }
      for (unsigned s = 0; s < source_count; s++)
         if (!chrome9_ps_lower_alu_source(&builder, &alu->src[s], 4,
                                          &operands[s]))
            return false;
   }
   if (!source_count) {
      op = CHROME9_PS_OPCODE_MOV;
      if (!chrome9_ps_lower_def(&builder, result, &operands[0]))
         return false;
      source_count = 1;
   }
   if (builder.ir.count >= CHROME9_PS_MAX_ALU)
      return false;
   terminal = &builder.ir.instruction[builder.ir.count];
   memset(terminal, 0, sizeof(*terminal));
   terminal->op = op;
   terminal->destination.file = CHROME9_PS_DESTINATION_COLOR;
   terminal->destination.writemask = 0xf;
   for (unsigned s = 0; s < source_count; s++)
      terminal->source[s] = operands[s];
   builder.ir.count++;
   *ir = builder.ir;
   memset(program, 0, sizeof(*program));
   memcpy(program->immediate, builder.immediate,
          sizeof(program->immediate));
   program->immediate_mask = builder.immediate_mask;
   *uniform_mask = builder.uniform_mask;
   *lowered = true;
   bool compiled = chrome9_compile_ps_ir(&builder.ir, program);

   memcpy(program->immediate, builder.immediate,
          sizeof(program->immediate));
   program->immediate_mask = builder.immediate_mask;
   return compiled;
}

/*
 * Lower the canonical NIR output expression to the first native backend IR.
 * Unlike the compatibility program classifier, this follows the output DAG
 * and derives opcode and operands directly.  The scheduler accepts a bounded
 * straight-line DAG and assigns dense hardware temporaries; unsupported NIR
 * operations fail compilation instead of being approximated or classified by
 * application-shaped program enums.
 */
static bool
chrome9_fs_key_from_nir(nir_shader *nir, struct chrome9_fs_key *key)
{
   nir_def *result = NULL;

   memset(key, 0, sizeof(*key));
   /* The current hardware envelope does not select an IEEE execution mode.
    * Default GLSL is admissible; explicit preserve/FTZ or rounding promises
    * must wait for a characterized Chrome9 control word. */
   if (nir->info.float_controls_execution_mode)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            nir_variable *var;
            unsigned location;
            nir_def *stored;

            if (instr->type == nir_instr_type_alu ||
                instr->type == nir_instr_type_deref ||
                instr->type == nir_instr_type_load_const)
               continue;
            if (instr->type != nir_instr_type_intrinsic)
               return false;
            intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output) {
               struct nir_io_semantics semantics =
                  nir_intrinsic_io_semantics(intr);

               location = semantics.location;
               if (nir_intrinsic_component(intr) ||
                   semantics.dual_source_blend_index ||
                   nir_intrinsic_write_mask(intr) != 0xf ||
                   !nir_src_is_const(intr->src[1]) ||
                   nir_src_as_uint(intr->src[1]))
                  return false;
               stored = intr->src[0].ssa;
            } else if (intr->intrinsic == nir_intrinsic_store_deref) {
               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);

               var = nir_intrinsic_get_var(intr, 0);
               if (!deref || deref->deref_type != nir_deref_type_var ||
                   !var || var->data.mode != nir_var_shader_out ||
                   var->data.index || var->data.location_frac ||
                   nir_intrinsic_write_mask(intr) != 0xf)
                  return false;
               location = var->data.location;
               stored = intr->src[1].ssa;
            } else if (intr->intrinsic ==
                          nir_intrinsic_load_barycentric_pixel ||
                       intr->intrinsic ==
                          nir_intrinsic_load_interpolated_input ||
                       intr->intrinsic == nir_intrinsic_load_deref ||
                       intr->intrinsic == nir_intrinsic_load_uniform ||
                       intr->intrinsic == nir_intrinsic_load_ubo ||
                       intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
               continue;
            } else {
               /* Discard, demote, memory, atomics, and extra outputs are
                * outside this one-record backend and must not disappear. */
               return false;
            }
            if ((location != FRAG_RESULT_COLOR &&
                 location != FRAG_RESULT_DATA0) || result)
               return false;
            result = stored;
         }
      }
   }
   if (!result || result->num_components != 4 || result->bit_size != 32)
      return false;

   if (chrome9_backend_leaf(result, 0, &key->src[0])) {
      key->op = CHROME9_FS_OP_MOV;
      return true;
   }

   nir_instr *instr = nir_def_instr(result);
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_fmul ||
       !chrome9_backend_source(&alu->src[0], &key->src[0]) ||
       !chrome9_backend_source(&alu->src[1], &key->src[1]))
      return false;
   key->op = CHROME9_FS_OP_MUL;
   return true;
}

static bool
chrome9_compile_native_fs(struct chrome9_shader *shader, nir_shader *nir)
{
   struct chrome9_fs_key key;
   enum chrome9_shader_verify_error error;
   bool from_nir, lowered;

   memset(&shader->native_fs_ir, 0, sizeof(shader->native_fs_ir));
   from_nir = chrome9_compile_ps_nir(nir, &shader->native_fs_ir,
                                     &shader->native_fs,
                                     &shader->semantic_uniform_mask,
                                     &lowered);
   shader->semantic_fs_valid = lowered;
   if (lowered) {
      for (unsigned i = 0; i < shader->native_fs_ir.texture_count; i++) {
         const struct chrome9_ps_texture_instruction *texture =
            &shader->native_fs_ir.texture[i];

         shader->semantic_sampler_mask |= 1u << texture->sampler;
         shader->semantic_varying_mask |= 1u << texture->coordinate;
      }
      for (unsigned i = 0; i < shader->native_fs_ir.count; i++) {
         const struct chrome9_ps_instruction *instruction =
            &shader->native_fs_ir.instruction[i];

         for (unsigned s = 0; s < 3; s++) {
            const struct chrome9_ps_source *source = &instruction->source[s];

            if (source->file == CHROME9_PS_SOURCE_CONSTANT)
               shader->semantic_constant_mask |= 1u << source->id;
            else if (source->file == CHROME9_PS_SOURCE_INPUT) {
               shader->semantic_input_mask |= 1u << source->id;
               shader->semantic_varying_mask |= 1u << source->id;
            } else if (source->file == CHROME9_PS_SOURCE_TEXTURE) {
               shader->semantic_varying_mask |= 1u << source->id;
            }
         }
      }
   }
   if (from_nir) {
      if (chrome9_fs_key_from_nir(nir, &key))
         shader->native_fs_key = key;
      shader->native_fs_from_nir = true;
      return true;
   }

   if (!chrome9_fs_native_key(shader->fragment_program, &key) ||
       !chrome9_compile_fs_offline(&key, &shader->native_fs))
      return false;

   if (!chrome9_verify_fs_offline(&key, &shader->native_fs, &error))
      return false;
   shader->native_fs_key = key;
   shader->native_fs_from_nir = false;
   return true;
}

static bool
chrome9_native_program_source(const struct chrome9_ps_source *source)
{
   if (!source || source->id || source->modifier || source->last_use ||
       (source->file != CHROME9_PS_SOURCE_INPUT &&
        source->file != CHROME9_PS_SOURCE_CONSTANT))
      return false;
   for (unsigned c = 0; c < ARRAY_SIZE(source->swizzle); c++) {
      if (source->swizzle[c] != c)
         return false;
   }
   return true;
}

static bool
chrome9_native_fs_is_program_batch(const struct chrome9_shader *shader,
                                   bool fragment_add)
{
   const struct chrome9_ps_instruction *instruction;

   if (!shader->native_fs_valid || !shader->native_fs_from_nir ||
       shader->native_fs_ir.count != 1 ||
       shader->native_fs.alu_count != 1 || shader->native_fs.tau_count ||
       shader->native_fs.sampler_mask ||
       (shader->native_fs.varying_mask & ~1u) ||
       (shader->native_fs.constant_mask & ~1u))
      return false;
   instruction = &shader->native_fs_ir.instruction[0];
   if (instruction->destination.file != CHROME9_PS_DESTINATION_COLOR ||
       instruction->destination.id ||
       instruction->destination.writemask != 0xf ||
       instruction->destination.saturate ||
       !chrome9_native_program_source(&instruction->source[0]))
      return false;
   switch (instruction->op) {
   case CHROME9_PS_OPCODE_MOV:
      return true;
   case CHROME9_PS_OPCODE_MUL:
      return chrome9_native_program_source(&instruction->source[1]) &&
             instruction->source[0].file != instruction->source[1].file;
   case CHROME9_PS_OPCODE_ADD:
      return fragment_add &&
             chrome9_native_program_source(&instruction->source[1]) &&
             instruction->source[0].file != instruction->source[1].file;
   default:
      return false;
   }
}

static bool
chrome9_tgsi_fs_passthrough(const struct tgsi_token *tokens)
{
   struct tgsi_parse_context parse;
   int input = -1, output = -1;
   unsigned moves = 0;
   bool valid = tokens && tgsi_parse_init(&parse, tokens) == TGSI_PARSE_OK;

   if (!valid)
      return false;
   while (!tgsi_parse_end_of_tokens(&parse) && valid) {
      const union tgsi_full_token *token;

      tgsi_parse_token(&parse);
      token = &parse.FullToken;
      switch (token->Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION: {
         const struct tgsi_full_declaration *decl = &token->FullDeclaration;

         if (decl->Declaration.File == TGSI_FILE_INPUT &&
             decl->Declaration.Semantic &&
             decl->Semantic.Name == TGSI_SEMANTIC_COLOR &&
             decl->Semantic.Index == 0 &&
             decl->Range.First == decl->Range.Last)
            input = decl->Range.First;
         else if (decl->Declaration.File == TGSI_FILE_OUTPUT &&
                  decl->Declaration.Semantic &&
                  decl->Semantic.Name == TGSI_SEMANTIC_COLOR &&
                  decl->Semantic.Index == 0 &&
                  decl->Range.First == decl->Range.Last)
            output = decl->Range.First;
         else if (decl->Declaration.File != TGSI_FILE_INPUT &&
                  decl->Declaration.File != TGSI_FILE_OUTPUT)
            valid = false;
         break;
      }
      case TGSI_TOKEN_TYPE_INSTRUCTION: {
         const struct tgsi_full_instruction *inst = &token->FullInstruction;
         const struct tgsi_dst_register *dst = &inst->Dst[0].Register;
         const struct tgsi_src_register *src = &inst->Src[0].Register;

         if (inst->Instruction.Opcode == TGSI_OPCODE_END)
            break;
         if (inst->Instruction.Opcode != TGSI_OPCODE_MOV || moves++ ||
             inst->Instruction.NumDstRegs != 1 ||
             inst->Instruction.NumSrcRegs != 1 ||
             dst->File != TGSI_FILE_OUTPUT || dst->Index != output ||
             dst->WriteMask != TGSI_WRITEMASK_XYZW || dst->Indirect ||
             src->File != TGSI_FILE_INPUT || src->Index != input ||
             src->Indirect || src->Negate || src->Absolute ||
             src->SwizzleX != TGSI_SWIZZLE_X ||
             src->SwizzleY != TGSI_SWIZZLE_Y ||
             src->SwizzleZ != TGSI_SWIZZLE_Z ||
             src->SwizzleW != TGSI_SWIZZLE_W)
            valid = false;
         break;
      }
      case TGSI_TOKEN_TYPE_PROPERTY:
         break;
      default:
         valid = false;
         break;
      }
   }
   tgsi_parse_free(&parse);
   return valid && input >= 0 && output >= 0 && moves == 1;
}

static void *
chrome9_create_fs_state(struct pipe_context *pipe,
                        const struct pipe_shader_state *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = CALLOC_STRUCT(chrome9_shader);

   if (!shader)
      return NULL;
   shader->draw = draw_create_fragment_shader(ctx->draw, state);
   if (!shader->draw) {
      FREE(shader);
      return NULL;
   }
   if (state->type == PIPE_SHADER_IR_NIR && state->ir.nir) {
      bool straight_line =
         chrome9_fs_straight_line_entrypoint(state->ir.nir, NULL, NULL);

      shader->nir = state->ir.nir;
      if (debug_get_bool_option("CHROME9_DUMP_NIR", false))
         nir_print_shader(state->ir.nir, stderr);
      shader->fragment_program = straight_line ?
         chrome9_fs_classify(state->ir.nir, &shader->literal_color) :
         CHROME9_FS_UNSUPPORTED;
      shader->flat_primary =
         shader->fragment_program == CHROME9_FS_PRIMARY_COLOR &&
         chrome9_fs_has_flat_primary(state->ir.nir);
      shader->flat_generic = shader->fragment_program ==
         CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX;
      shader->native_fs_valid = straight_line &&
         chrome9_compile_native_fs(shader, state->ir.nir);
      if (shader->semantic_fs_valid && ctx->draw_fragment_program &&
          !shader->semantic_input_mask &&
          shader->fragment_program == CHROME9_FS_UNSUPPORTED)
         shader->fragment_program = CHROME9_FS_COMPILED_ALU;
      if (shader->fragment_program == CHROME9_FS_UNSUPPORTED &&
          ctx->draw_program_triangle_d16_rhw_batch &&
          chrome9_native_fs_is_program_batch(shader, ctx->fragment_add))
         shader->fragment_program = CHROME9_FS_COMPILED_ALU;
      shader->supported =
         shader->fragment_program != CHROME9_FS_UNSUPPORTED;
      if (!shader->supported &&
          debug_get_bool_option("CHROME9_DUMP_UNSUPPORTED_NIR", false)) {
         fprintf(stderr, "chrome9: unsupported FS %p\n", (void *)shader);
         nir_print_shader(state->ir.nir, stderr);
      }
      if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false))
         fprintf(stderr, "chrome9: create FS %p program=%u supported=%u "
                 "semantic=%u native=%u nir=%u ir=%u/%u alu=%u tau=%u "
                 "constants=%08x immediates=%08x\n",
                 (void *)shader, shader->fragment_program,
                 shader->supported, shader->semantic_fs_valid,
                 shader->native_fs_valid,
                 shader->native_fs_from_nir,
                 shader->native_fs_ir.count,
                 shader->native_fs_ir.texture_count,
                 shader->native_fs.alu_count,
                 shader->native_fs.tau_count,
                 shader->native_fs.constant_mask,
                 shader->native_fs.immediate_mask);
   } else if (state->type == PIPE_SHADER_IR_TGSI) {
      shader->supported = chrome9_tgsi_fs_passthrough(state->tokens);
      shader->fragment_program = shader->supported ?
         CHROME9_FS_PRIMARY_COLOR : CHROME9_FS_UNSUPPORTED;
   }
   if (shader->supported && !shader->native_fs_valid)
      shader->native_fs_valid = chrome9_compile_native_fs(
         shader, state->type == PIPE_SHADER_IR_NIR ? state->ir.nir : NULL);
   if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false) &&
       shader->supported)
      fprintf(stderr, "chrome9: native FS valid=%u nir=%u alu=%u tau=%u\n",
              shader->native_fs_valid, shader->native_fs_from_nir,
              shader->native_fs.alu_count, shader->native_fs.tau_count);
   return shader;
}

static void
chrome9_bind_fs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   ctx->fs = shader;
   if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false))
      fprintf(stderr, "chrome9: bind FS %p program=%u supported=%u\n",
              (void *)shader,
              shader ? shader->fragment_program : 0,
              shader ? shader->supported : 0);
   draw_bind_fragment_shader(ctx->draw, shader ? shader->draw : NULL);
}

static void
chrome9_delete_fs_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = state;

   if (!shader)
      return;
   if (ctx->fs == shader) {
      ctx->fs = NULL;
      draw_bind_fragment_shader(ctx->draw, NULL);
   }
   draw_delete_fragment_shader(ctx->draw, shader->draw);
   ralloc_free(shader->nir);
   FREE(shader);
}

static void *
chrome9_create_vertex_elements_state(struct pipe_context *pipe,
                                     unsigned count,
                                     const struct pipe_vertex_element *elements)
{
   struct chrome9_vertex_elements *state;

   (void)pipe;
   if (count > PIPE_MAX_ATTRIBS)
      return NULL;
   state = CALLOC_STRUCT(chrome9_vertex_elements);
   if (!state)
      return NULL;
   state->count = count;
   memcpy(state->elements, elements, count * sizeof(*elements));
   return state;
}

static void
chrome9_bind_vertex_elements_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_vertex_elements *elements = state;

   ctx->vertex_elements = elements;
   draw_set_vertex_elements(ctx->draw, elements ? elements->count : 0,
                            elements ? elements->elements : NULL);
}

static void
chrome9_delete_vertex_elements_state(struct pipe_context *pipe, void *state)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (ctx->vertex_elements == state)
      ctx->vertex_elements = NULL;
   FREE(state);
}

static void
chrome9_set_vertex_buffers(struct pipe_context *pipe, unsigned count,
                           const struct pipe_vertex_buffer *buffers)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   util_set_vertex_buffers_count(ctx->vertex_buffers,
                                 &ctx->num_vertex_buffers, buffers, count);
   draw_set_vertex_buffers(ctx->draw, count, buffers);
}

static void
chrome9_set_constant_buffer(struct pipe_context *pipe,
                            mesa_shader_stage shader, unsigned index,
                            const struct pipe_constant_buffer *buffer)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (index >= PIPE_MAX_CONSTANT_BUFFERS)
      return;
   if (shader == MESA_SHADER_VERTEX)
      util_copy_constant_buffer(&ctx->vs_constants[index], buffer);
   else if (shader == MESA_SHADER_FRAGMENT)
      util_copy_constant_buffer(&ctx->fs_constants[index], buffer);
}

static struct pipe_sampler_view *
chrome9_create_sampler_view(struct pipe_context *pipe,
                            struct pipe_resource *texture,
                            const struct pipe_sampler_view *templ)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_sampler_view *cview = CALLOC_STRUCT(chrome9_sampler_view);
   struct pipe_sampler_view *view = cview ? &cview->base : NULL;
   bool supported = view && texture && texture->target == PIPE_TEXTURE_2D &&
                    templ->target == PIPE_TEXTURE_2D &&
                    templ->format == texture->format &&
                    (templ->format == PIPE_FORMAT_B8G8R8X8_UNORM ||
                     templ->format == PIPE_FORMAT_B8G8R8A8_UNORM) &&
                    !templ->u.tex.first_level &&
                    templ->u.tex.last_level <= texture->last_level &&
                    !templ->u.tex.first_layer &&
                    !templ->u.tex.last_layer &&
                    templ->swizzle_r == PIPE_SWIZZLE_X &&
                    templ->swizzle_g == PIPE_SWIZZLE_Y &&
                    templ->swizzle_b == PIPE_SWIZZLE_Z &&
                    (templ->swizzle_a == PIPE_SWIZZLE_W ||
                     (templ->format == PIPE_FORMAT_B8G8R8X8_UNORM &&
                      templ->swizzle_a == PIPE_SWIZZLE_1));

   if (supported && !chrome9_resource(texture)->guarded) {
      struct pipe_resource shadow_templ = *texture;

      shadow_templ.last_level = 0;
      shadow_templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
      shadow_templ.flags = 0;
      cview->shadow = pipe->screen->resource_create(pipe->screen,
                                                     &shadow_templ);
      supported = cview->shadow != NULL;
   }

   if (!supported) {
      if (!(ctx->warnings & CHROME9_WARN_SAMPLER_VIEW_CREATE)) {
         fprintf(stderr,
                 "chrome9: sampler view rejected: alloc=%u texture=%p "
                 "resource_target=%u resource_format=%u size=%ux%u "
                 "last=%u bind=%#x view_target=%u view_format=%u "
                 "levels=%u..%u layers=%u..%u swizzle=%u/%u/%u/%u\n",
                 view != NULL, (void *)texture,
                 texture ? texture->target : 0,
                 texture ? texture->format : 0,
                 texture ? texture->width0 : 0,
                 texture ? texture->height0 : 0,
                 texture ? texture->last_level : 0,
                 texture ? texture->bind : 0,
                 templ->target, templ->format,
                 templ->u.tex.first_level, templ->u.tex.last_level,
                 templ->u.tex.first_layer, templ->u.tex.last_layer,
                 templ->swizzle_r, templ->swizzle_g,
                 templ->swizzle_b, templ->swizzle_a);
         ctx->warnings |= CHROME9_WARN_SAMPLER_VIEW_CREATE;
      }
      if (cview && cview->shadow)
         pipe_resource_reference(&cview->shadow, NULL);
      FREE(cview);
      return NULL;
   }
   *view = *templ;
   pipe_reference_init(&view->reference, 1);
   view->texture = NULL;
   pipe_resource_reference(&view->texture, texture);
   view->context = pipe;
   return view;
}

static void
chrome9_sampler_view_destroy(struct pipe_context *pipe,
                             struct pipe_sampler_view *view)
{
   struct chrome9_sampler_view *cview = chrome9_sampler_view(view);

   (void)pipe;
   pipe_resource_reference(&view->texture, NULL);
   pipe_resource_reference(&cview->shadow, NULL);
   FREE(cview);
}

static void
chrome9_set_sampler_views(struct pipe_context *pipe, mesa_shader_stage shader,
                          unsigned start, unsigned count,
                          unsigned unbind_trailing,
                          struct pipe_sampler_view **views)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   bool report = count && !(ctx->warnings & CHROME9_WARN_SAMPLER_VIEW_BIND);
   unsigned end;

   if (shader != MESA_SHADER_FRAGMENT || start >= PIPE_MAX_SHADER_SAMPLER_VIEWS)
      return;
   end = MIN2(start + count, PIPE_MAX_SHADER_SAMPLER_VIEWS);
   for (unsigned i = start; i < end; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i],
                                  views ? views[i - start] : NULL);
   end = MIN2(end + unbind_trailing, PIPE_MAX_SHADER_SAMPLER_VIEWS);
   for (unsigned i = start + count; i < end; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   ctx->num_sampler_views = 0;
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      if (ctx->sampler_views[i])
         ctx->num_sampler_views = i + 1;
   if (report) {
      struct pipe_sampler_view *view = views ? views[0] : NULL;
      struct pipe_resource *texture = view ? view->texture : NULL;

      fprintf(stderr,
              "chrome9: sampler views set: shader=%u start=%u count=%u "
              "unbind=%u first_view=%p texture=%p target=%u format=%u "
              "levels=%u..%u resulting=%u\n",
              shader, start, count, unbind_trailing, (void *)view,
              (void *)texture, texture ? texture->target : 0,
              texture ? texture->format : 0,
              view ? view->u.tex.first_level : 0,
              view ? view->u.tex.last_level : 0,
              ctx->num_sampler_views);
      ctx->warnings |= CHROME9_WARN_SAMPLER_VIEW_BIND;
   }
}

static void
chrome9_set_framebuffer_state(struct pipe_context *pipe,
                              const struct pipe_framebuffer_state *framebuffer)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   const struct pipe_surface *surface = framebuffer->nr_cbufs == 1 ?
                                        &framebuffer->cbufs[0] : NULL;
   struct pipe_resource *color = surface ? surface->texture : NULL;
   bool shadow_attempted = false;

   chrome9_draw_flush_batch(ctx);
   chrome9_flush_render_target(ctx);
   ctx->render_target_stale = false;
   pipe_resource_reference(&ctx->render_target, NULL);
   util_copy_framebuffer_state(&ctx->framebuffer, framebuffer);
   if (color && surface->format == color->format && !surface->level &&
       !surface->first_layer && !surface->last_layer) {
      struct chrome9_resource *resource = chrome9_resource(color);

      if (resource->guarded) {
         pipe_resource_reference(&ctx->render_target, color);
      } else {
         struct pipe_resource templ = *color;

         templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR;
         templ.flags = 0;
         shadow_attempted = true;
         ctx->render_target = pipe->screen->resource_create(pipe->screen,
                                                             &templ);
         if (ctx->render_target)
            chrome9_copy_surface(ctx->render_target, color);
      }
   }
   if (color && !ctx->render_target &&
       !(ctx->warnings & CHROME9_WARN_FRAMEBUFFER)) {
      struct chrome9_resource *resource = chrome9_resource(color);

      fprintf(stderr,
              "chrome9: framebuffer target rejected: cbufs=%u surface=%p "
              "surface_format=%u level=%u layers=%u..%u texture=%p "
              "target=%u format=%u size=%ux%ux%u array=%u last=%u "
              "samples=%u/%u bind=%#x flags=%#x guarded=%u bo=%u "
              "stride=%u offset=%" PRIu64 " shadow_attempted=%u\n",
              framebuffer->nr_cbufs, (void *)surface,
              surface ? surface->format : 0,
              surface ? surface->level : 0,
              surface ? surface->first_layer : 0,
              surface ? surface->last_layer : 0,
              (void *)color, color->target, color->format,
              color->width0, color->height0, color->depth0,
              color->array_size, color->last_level,
              color->nr_samples, color->nr_storage_samples,
              color->bind, color->flags, resource->guarded,
              resource->bo.handle, resource->stride,
              resource->data_offset, shadow_attempted);
      ctx->warnings |= CHROME9_WARN_FRAMEBUFFER;
   }
   draw_set_zs_format(ctx->draw, framebuffer->zsbuf.texture ?
                                framebuffer->zsbuf.format : PIPE_FORMAT_NONE);
}

static void
chrome9_set_viewport_states(struct pipe_context *pipe, unsigned start,
                            unsigned count,
                            const struct pipe_viewport_state *viewports)
{
   draw_set_viewport_states(chrome9_context(pipe)->draw, start, count,
                            viewports);
}

static void
chrome9_set_scissor_states(struct pipe_context *pipe, unsigned start,
                           unsigned count,
                           const struct pipe_scissor_state *scissors)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (!start && count) {
      ctx->scissor = scissors[0];
      ctx->scissor_valid = true;
   }
}

static void
chrome9_set_clip_state(struct pipe_context *pipe,
                       const struct pipe_clip_state *clip)
{
   draw_set_clip_state(chrome9_context(pipe)->draw, clip);
}

static void
chrome9_clear_target(struct chrome9_resource *target,
                     const union pipe_color_union *color,
                     unsigned x, unsigned y, unsigned width, unsigned height)
{
   union util_color packed;

   if (!target ||
       (target->base.format != PIPE_FORMAT_B8G8R8X8_UNORM &&
        target->base.format != PIPE_FORMAT_B8G8R8A8_UNORM) ||
       x >= target->base.width0 || y >= target->base.height0)
      return;
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   util_pack_color_union(target->base.format, &packed, color);
   util_fill_rect((uint8_t *)target->bo.map + target->data_offset,
                  target->base.format, target->stride, x, y, width, height,
                  &packed);
}

static void
chrome9_clear_depth_target(struct chrome9_resource *target, double depth,
                           unsigned x, unsigned y, unsigned width,
                           unsigned height)
{
   uint16_t value;

   if (!target || target->base.format != PIPE_FORMAT_Z16_UNORM ||
       x >= target->base.width0 || y >= target->base.height0)
      return;
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   if (!isfinite(depth))
      depth = 0.0;
   value = (uint16_t)(CLAMP(depth, 0.0, 1.0) * 65535.0 + 0.5);
   for (unsigned row = 0; row < height; row++) {
      uint16_t *pixels = (uint16_t *)
         ((uint8_t *)target->bo.map + target->data_offset +
          (uint64_t)(y + row) * target->stride) + x;

      for (unsigned column = 0; column < width; column++)
         pixels[column] = value;
   }
}

static void
chrome9_clear(struct pipe_context *pipe, unsigned buffers,
              uint32_t color_mask, uint8_t stencil_mask,
              const struct pipe_scissor_state *scissor,
              const union pipe_color_union *color, double depth,
              unsigned stencil)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct pipe_resource *resource;
   unsigned x = 0, y = 0, width, height;

   (void)stencil_mask;
   (void)stencil;
   chrome9_draw_flush_batch(ctx);
   if (!(buffers & (PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH)))
      return;
   if (((buffers & PIPE_CLEAR_COLOR0) && (color_mask & 0xf) != 0xf) ||
       (buffers & ~(PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH))) {
      chrome9_warn_once(ctx, CHROME9_WARN_CLEAR,
                        "masked color or stencil clears are not implemented");
      return;
   }
   width = ctx->framebuffer.width;
   height = ctx->framebuffer.height;
   if (scissor) {
      x = MIN2(scissor->minx, width);
      y = MIN2(scissor->miny, height);
      width = MIN2(scissor->maxx, width) - x;
      height = MIN2(scissor->maxy, height) - y;
   }
   if ((buffers & PIPE_CLEAR_COLOR0) &&
       (resource = ctx->render_target)) {
      chrome9_context_prepare_render_target(ctx);
      chrome9_clear_target(chrome9_resource(resource), color, x, y, width,
                           height);
      chrome9_context_render_target_written(ctx);
      /* Imported drawable targets render through a guarded shadow.  Clears
       * are synchronous CPU writes, so publish the completed image now even
       * when the state tracker keeps this framebuffer bound. */
      chrome9_flush_render_target(ctx);
   }
   if (buffers & PIPE_CLEAR_DEPTH)
      chrome9_clear_depth_target(chrome9_context_depth_target(ctx), depth,
                                 x, y, width, height);
}

static void
chrome9_clear_depth_stencil(struct pipe_context *pipe,
                            struct pipe_surface *surface,
                            unsigned clear_flags, double depth,
                            unsigned stencil, unsigned x, unsigned y,
                            unsigned width, unsigned height,
                            bool render_condition)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   (void)stencil;
   (void)render_condition;
   chrome9_draw_flush_batch(ctx);
   if (!surface || !surface->texture || clear_flags != PIPE_CLEAR_DEPTH) {
      chrome9_warn_once(ctx, CHROME9_WARN_CLEAR,
                        "stencil clears are not implemented");
      return;
   }
   chrome9_clear_depth_target(chrome9_resource(surface->texture), depth,
                              x, y, width, height);
}

static void
chrome9_clear_render_target(struct pipe_context *pipe,
                            struct pipe_surface *surface,
                            const union pipe_color_union *color,
                            unsigned x, unsigned y, unsigned width,
                            unsigned height, bool render_condition)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct pipe_resource *target;

   (void)render_condition;
   chrome9_draw_flush_batch(ctx);
   if (!surface || !surface->texture)
      return;
   target = surface->texture;
   if (ctx->framebuffer.nr_cbufs &&
       surface->texture == ctx->framebuffer.cbufs[0].texture &&
       ctx->render_target)
      target = ctx->render_target;
   if (target == ctx->render_target)
      chrome9_context_prepare_render_target(ctx);
   chrome9_clear_target(chrome9_resource(target), color,
                        x, y, width, height);
   if (target == ctx->render_target) {
      chrome9_context_render_target_written(ctx);
      chrome9_flush_render_target(ctx);
   }
}

static void
chrome9_refresh_sampler_shadows(struct chrome9_context *ctx)
{
   for (unsigned i = 0; i < ctx->num_sampler_views; i++) {
      struct pipe_sampler_view *view = ctx->sampler_views[i];
      struct chrome9_sampler_view *cview;

      if (!view)
         continue;
      cview = chrome9_sampler_view(view);
      if (cview->shadow) {
         /* A texture may be the result of an earlier untextured pass.  Retire
          * retained triangles before refreshing its CPU-visible shadow. */
         chrome9_draw_flush_batch(ctx);
         chrome9_copy_surface(cview->shadow, view->texture);
      }
   }
}

static void
chrome9_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   const void *indices = NULL;

   /* Imported drawable contents may change after a view is created or
    * bound.  Refresh immediately before every Gallium draw so hardware never
    * samples a create-time/set-time snapshot. */
   chrome9_refresh_sampler_shadows(ctx);

   if (num_draws > 1) {
      util_draw_multi(pipe, info, drawid_offset, indirect, draws, num_draws);
      return;
   }
   if (!num_draws || !draws[0].count || !info->instance_count)
      return;
   if (indirect) {
      chrome9_warn_once(ctx, CHROME9_WARN_INDIRECT,
                        "indirect draws are not implemented");
      return;
   }
   if (!ctx->vs || !ctx->fs || !ctx->fs->supported) {
      chrome9_warn_once(ctx, CHROME9_WARN_DRAW_STATE,
                        "draw rejected: unsupported bounded fragment program");
      return;
   }

   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];
      const void *map = vb->is_user_buffer ? vb->buffer.user :
                        vb->buffer.resource ?
                        chrome9_resource(vb->buffer.resource)->bo.map : NULL;
      size_t size = vb->is_user_buffer || !vb->buffer.resource ?
                    ~(size_t)0 : vb->buffer.resource->width0;

      draw_set_mapped_vertex_buffer(ctx->draw, i, map, size);
   }
   if (info->index_size) {
      indices = info->has_user_indices ? info->index.user :
                info->index.resource ?
                chrome9_resource(info->index.resource)->bo.map : NULL;
      if (!indices)
         goto unmap;
      draw_set_indexes(ctx->draw, indices, info->index_size,
                       info->has_user_indices ? ~(size_t)0 :
                       info->index.resource->width0);
   }
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct pipe_constant_buffer *cb = &ctx->vs_constants[i];
      const uint8_t *map = cb->user_buffer;

      if (!map && cb->buffer)
         map = chrome9_resource(cb->buffer)->bo.map;
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, i,
                                      map ? map + cb->buffer_offset : NULL,
                                      map ? cb->buffer_size : 0);
   }
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(ctx->draw);
   /* Keep compatible immediate triangles across Gallium draw calls.  The
    * flush/present boundary retires them and publishes a guarded drawable
    * shadow once rather than copying a full image after every draw. */

unmap:
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++)
      draw_set_mapped_vertex_buffer(ctx->draw, i, NULL, 0);
   if (info->index_size)
      draw_set_indexes(ctx->draw, NULL, 0, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++)
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, i,
                                      NULL, 0);
}

static void
chrome9_flush(struct pipe_context *pipe, struct pipe_fence_handle **fence,
              unsigned flags)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   chrome9_draw_flush_batch(ctx);
   chrome9_flush_render_target(ctx);
   (void)flags;
   /* DRAW_TRIANGLE completes synchronously. */
   if (fence)
      *fence = NULL;
}

static void
chrome9_flush_resource(struct pipe_context *pipe,
                       struct pipe_resource *resource)
{
   (void)resource;
   /*
    * Rendering is synchronous, but an imported DRI3 render target may use a
    * guarded shadow.  Presenting it still requires the shadow copy-back.
    */
   chrome9_draw_flush_batch(chrome9_context(pipe));
   chrome9_flush_render_target(chrome9_context(pipe));
}

static void
chrome9_set_blend_color(struct pipe_context *pipe,
                        const struct pipe_blend_color *color)
{
   (void)pipe;
   (void)color;
}

static void
chrome9_set_stencil_ref(struct pipe_context *pipe,
                        const struct pipe_stencil_ref reference)
{
   (void)pipe;
   (void)reference;
}

static void
chrome9_set_sample_mask(struct pipe_context *pipe, unsigned mask)
{
   (void)pipe;
   (void)mask;
}

static void
chrome9_set_polygon_stipple(struct pipe_context *pipe,
                            const struct pipe_poly_stipple *stipple)
{
   (void)pipe;
   (void)stipple;
}

static void
chrome9_context_destroy(struct pipe_context *pipe)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   chrome9_draw_flush_batch(ctx);
   chrome9_flush_render_target(ctx);
   if (ctx->draw)
      draw_destroy(ctx->draw);
   if (pipe->stream_uploader)
      u_upload_destroy(pipe->stream_uploader);
   util_unreference_framebuffer_state(&ctx->framebuffer);
   pipe_resource_reference(&ctx->render_target, NULL);
   util_set_vertex_buffers_count(ctx->vertex_buffers,
                                 &ctx->num_vertex_buffers, NULL, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      util_copy_constant_buffer(&ctx->vs_constants[i], NULL);
      util_copy_constant_buffer(&ctx->fs_constants[i], NULL);
   }
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   FREE(ctx);
}

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags)
{
   struct chrome9_context *ctx = CALLOC_STRUCT(chrome9_context);
   struct draw_stage *stage;
   uint64_t capabilities;

   (void)flags;
   if (!ctx)
      return NULL;
   if (!chrome9_get_param(screen->get_screen_fd(screen),
                          DRM_CHROME9_PARAM_CAPABILITIES, &capabilities) ||
       !(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE))
      goto fail;
   ctx->draw_triangle_d16 =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16);
   ctx->draw_triangle_d16_blend =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND);
   ctx->depth_func_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_FUNC);
   ctx->depth_equal_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_EQUAL);
   ctx->depth_always_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_ALWAYS);
   ctx->depth_read_only_supported =
      !!(capabilities & DRM_CHROME9_CAP_DEPTH_READ_ONLY);
   ctx->draw_triangle_batch =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_BATCH);
   ctx->draw_triangle_d16_rhw_batch =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_BATCH);
   ctx->draw_triangle_d16_rhw_run =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_RUN) ||
      debug_get_bool_option("CHROME9_FORCE_RHW_RUN", false);
   ctx->draw_triangle_d16_rhw_chain =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_CHAIN);
   ctx->draw_program_triangle_d16_rhw_batch =
      !!(capabilities &
         DRM_CHROME9_CAP_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH);
   ctx->draw_fragment_program =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM);
   ctx->fragment_add =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_ADD);
   ctx->draw_textured_triangle =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE);
   ctx->draw_textured_triangle_batch =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BATCH);
   ctx->draw_textured_triangle_run =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_RUN);
   ctx->draw_textured_triangle_mip =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_MIP);
   ctx->copy_rect = !!(capabilities & DRM_CHROME9_CAP_COPY_RECT);
   ctx->copy_rect_unguarded_destination =
      !!(capabilities & DRM_CHROME9_CAP_COPY_RECT_UNGUARDED_DESTINATION);
   ctx->copy_rect_preserve_alpha =
      !!(capabilities & DRM_CHROME9_CAP_COPY_RECT_PRESERVE_ALPHA) &&
      debug_get_bool_option("CHROME9_OPAQUE_SCANOUT_PRESENT", false);
   ctx->draw_textured_triangle_d16 =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16);
   ctx->draw_textured_triangle_d16_blend =
      !!(capabilities &
         DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND);
   ctx->textured_d16_bgra8888 =
      !!(capabilities & DRM_CHROME9_CAP_TEXTURED_D16_BGRA8888);
   ctx->textured_d16_source_over =
      !!(capabilities & DRM_CHROME9_CAP_TEXTURED_D16_SOURCE_OVER);
   ctx->draw_two_textured_triangle =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE);
   ctx->draw_two_textured_triangle_d16 =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16);
   ctx->draw_two_textured_triangle_d16_blend =
      !!(capabilities &
         DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND);
   ctx->two_texture_tex0_alpha_primary =
      !!(capabilities &
         DRM_CHROME9_CAP_TWO_TEXTURE_TEX0_ALPHA_PRIMARY);
   ctx->two_texture_signed_repeat_coords =
      !!(capabilities &
         DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS);
   ctx->draw_triangle_blend =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND);
   ctx->draw_textured_triangle_blend =
      !!(capabilities & DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND);
   ctx->blend_bgra8888 =
      !!(capabilities & DRM_CHROME9_CAP_BLEND_BGRA8888);
   ctx->blend_extended_factors =
      !!(capabilities & DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS);
   ctx->alpha_test_supported =
      !!(capabilities & DRM_CHROME9_CAP_ALPHA_TEST);
   ctx->scissor_supported =
      !!(capabilities & DRM_CHROME9_CAP_SCISSOR);
   ctx->draw_limit = MAX2(debug_get_num_option("CHROME9_DRAW_LIMIT", 0), 0);
   ctx->debug_draws = debug_get_bool_option("CHROME9_DEBUG_DRAWS", false);

   ctx->base.screen = screen;
   ctx->base.priv = priv;
   ctx->base.destroy = chrome9_context_destroy;
   ctx->base.flush = chrome9_flush;
   ctx->base.flush_resource = chrome9_flush_resource;
   ctx->base.draw_vbo = chrome9_draw_vbo;
   ctx->base.clear = chrome9_clear;
   ctx->base.clear_render_target = chrome9_clear_render_target;
   ctx->base.clear_depth_stencil = chrome9_clear_depth_stencil;
   ctx->base.create_blend_state = chrome9_create_blend_state;
   ctx->base.bind_blend_state = chrome9_bind_blend_state;
   ctx->base.delete_blend_state = chrome9_delete_state;
   ctx->base.create_depth_stencil_alpha_state = chrome9_create_dsa_state;
   ctx->base.bind_depth_stencil_alpha_state = chrome9_bind_dsa_state;
   ctx->base.delete_depth_stencil_alpha_state = chrome9_delete_state;
   ctx->base.create_rasterizer_state = chrome9_create_rasterizer_state;
   ctx->base.bind_rasterizer_state = chrome9_bind_rasterizer_state;
   ctx->base.delete_rasterizer_state = chrome9_delete_state;
   ctx->base.create_sampler_state = chrome9_create_sampler_state;
   ctx->base.bind_sampler_states = chrome9_bind_sampler_states;
   ctx->base.delete_sampler_state = chrome9_delete_sampler_state;
   ctx->base.create_sampler_view = chrome9_create_sampler_view;
   ctx->base.sampler_view_destroy = chrome9_sampler_view_destroy;
   ctx->base.sampler_view_release = u_default_sampler_view_release;
   ctx->base.set_sampler_views = chrome9_set_sampler_views;
   ctx->base.create_vs_state = chrome9_create_vs_state;
   ctx->base.bind_vs_state = chrome9_bind_vs_state;
   ctx->base.delete_vs_state = chrome9_delete_vs_state;
   ctx->base.create_fs_state = chrome9_create_fs_state;
   ctx->base.bind_fs_state = chrome9_bind_fs_state;
   ctx->base.delete_fs_state = chrome9_delete_fs_state;
   ctx->base.create_vertex_elements_state =
      chrome9_create_vertex_elements_state;
   ctx->base.bind_vertex_elements_state = chrome9_bind_vertex_elements_state;
   ctx->base.delete_vertex_elements_state =
      chrome9_delete_vertex_elements_state;
   ctx->base.set_vertex_buffers = chrome9_set_vertex_buffers;
   ctx->base.set_constant_buffer = chrome9_set_constant_buffer;
   ctx->base.set_framebuffer_state = chrome9_set_framebuffer_state;
   ctx->base.set_viewport_states = chrome9_set_viewport_states;
   ctx->base.set_scissor_states = chrome9_set_scissor_states;
   ctx->base.set_clip_state = chrome9_set_clip_state;
   ctx->base.set_blend_color = chrome9_set_blend_color;
   ctx->base.set_stencil_ref = chrome9_set_stencil_ref;
   ctx->base.set_sample_mask = chrome9_set_sample_mask;
   ctx->base.set_polygon_stipple = chrome9_set_polygon_stipple;
   ctx->base.set_debug_callback = u_default_set_debug_callback;
   ctx->base.resource_release = u_default_resource_release;
   chrome9_init_context_resource_functions(&ctx->base);

   ctx->draw = draw_create(&ctx->base);
   if (!ctx->draw)
      goto fail;
   draw_set_constant_buffer_stride(ctx->draw, sizeof(float));
   stage = chrome9_draw_vbuf_stage(ctx);
   if (!stage)
      goto fail;
   draw_set_rasterize_stage(ctx->draw, stage);

   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   if (!ctx->base.stream_uploader)
      goto fail;
   ctx->base.const_uploader = ctx->base.stream_uploader;
   return &ctx->base;

fail:
   chrome9_context_destroy(&ctx->base);
   return NULL;
}
