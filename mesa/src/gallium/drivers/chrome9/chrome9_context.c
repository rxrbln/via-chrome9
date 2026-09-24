/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_context.h"

#include "chrome9_draw.h"
#include "chrome9_swfb.h"
#include "chrome9_tcl.h"
#include "chrome9_drm.h"
#include "chrome9_format.h"
#include "chrome9_fragment.h"
#include "chrome9_ps_schedule.h"
#include "chrome9_resource.h"
#include "chrome9_specialise.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

#include "draw/draw_context.h"
#include "nir.h"
#include "nir_builder.h"
#include "tgsi/tgsi_parse.h"
#include "util/format/u_format.h"
#include "util/hash_table.h"
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
   unsigned cpp = util_format_get_blocksize(source->format);
   unsigned width = MIN2(destination->width0, source->width0) * cpp;
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
   if (ctx->framebuffer.nr_cbufs)
      ctx->render_target_dirty = true;
}

/* A synchronized CPU map observes all queued draws; the displayed target
 * additionally receives the shadow copy, and a CPU write makes it stale. */
void
chrome9_context_sync_resource(struct chrome9_context *ctx,
                              struct pipe_resource *resource, unsigned usage)
{
   struct pipe_resource *external;

   if (usage & PIPE_MAP_UNSYNCHRONIZED)
      return;
   chrome9_draw_flush_batch(ctx);
   external = ctx->framebuffer.nr_cbufs ?
              ctx->framebuffer.cbufs[0].texture : NULL;
   if (!external || resource != external || external == ctx->render_target)
      return;
   chrome9_flush_render_target(ctx);
   if (usage & PIPE_MAP_WRITE)
      ctx->render_target_stale = true;
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

void
chrome9_count_reject(struct chrome9_context *ctx, const char *site,
                     unsigned count)
{
   struct hash_entry *entry;

   ctx->rejected_triangles += count;
   if (!ctx->reject_sites)
      return;
   entry = _mesa_hash_table_search(ctx->reject_sites, site);
   if (entry)
      entry->data = (void *)((uintptr_t)entry->data + count);
   else
      _mesa_hash_table_insert(ctx->reject_sites, site,
                              (void *)(uintptr_t)count);
}

/* Print every drop site whenever the total changed since the last flush. */
static void
chrome9_report_rejects(struct chrome9_context *ctx)
{
   if (!ctx->reject_sites || ctx->rejected_triangles == ctx->reject_reported)
      return;
   fprintf(stderr, "chrome9: rejected %" PRIu64 " (+%" PRIu64 "):",
           ctx->rejected_triangles,
           ctx->rejected_triangles - ctx->reject_reported);
   hash_table_foreach(ctx->reject_sites, entry)
      fprintf(stderr, " %s=%" PRIuPTR, (const char *)entry->key,
              (uintptr_t)entry->data);
   fputc('\n', stderr);
   ctx->reject_reported = ctx->rejected_triangles;
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

   if (shader == MESA_SHADER_VERTEX) {
      struct pipe_sampler_state *new_states[CHROME9_VS_MAX_SAMPLERS];
      unsigned num_states = 0;

      if (start >= CHROME9_VS_MAX_SAMPLERS)
         return;
      memcpy(new_states, ctx->vs_sampler_states, sizeof(new_states));
      end = MIN2(start + count, CHROME9_VS_MAX_SAMPLERS);
      for (unsigned i = start; i < end; i++)
         new_states[i] = states ? states[i - start] : NULL;
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_states[i])
            num_states = i + 1;
      /* draw_set_samplers flushes pending work before copying state.  The
       * interpreter callback must see the old state during that flush. */
      draw_set_samplers(ctx->draw, MESA_SHADER_VERTEX,
                        new_states, num_states);
      memcpy(ctx->vs_sampler_states, new_states, sizeof(new_states));
      ctx->num_vs_sampler_states = num_states;
      return;
   }
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
   bool vs_changed = false;

   chrome9_sw_forget(ctx, state);
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->sampler_states[i] == state)
         ctx->sampler_states[i] = NULL;
   for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++) {
      if (ctx->vs_sampler_states[i] == state) {
         vs_changed = true;
      }
   }
   if (vs_changed) {
      struct pipe_sampler_state *new_states[CHROME9_VS_MAX_SAMPLERS];
      unsigned num_states = 0;

      memcpy(new_states, ctx->vs_sampler_states, sizeof(new_states));
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_states[i] == state)
            new_states[i] = NULL;
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_states[i])
            num_states = i + 1;
      draw_set_samplers(ctx->draw, MESA_SHADER_VERTEX,
                        new_states, num_states);
      memcpy(ctx->vs_sampler_states, new_states, sizeof(new_states));
      ctx->num_vs_sampler_states = num_states;
   }
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

   chrome9_sw_forget(ctx, state);
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
   /* Clone before draw takes the NIR; TCL compiles it lazily. */
   if (ctx->tcl_enabled && state->type == PIPE_SHADER_IR_NIR && state->ir.nir)
      shader->tcl = chrome9_tcl_shader_create(state->ir.nir);
   if (chrome9_sw_enabled() && state->type == PIPE_SHADER_IR_NIR &&
       state->ir.nir)
      shader->sw_nir = nir_shader_clone(NULL, state->ir.nir);
   shader->draw = draw_create_vertex_shader(ctx->draw, state);
   shader->supported = shader->draw != NULL;
   if (!shader->draw) {
      chrome9_tcl_shader_destroy(shader->tcl);
      ralloc_free(shader->sw_nir);
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
   chrome9_sw_forget(ctx, shader);
   draw_delete_vertex_shader(ctx->draw, shader->draw);
   chrome9_tcl_shader_destroy(shader->tcl);
   ralloc_free(shader->sw_nir);
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
chrome9_fs_smooth_pixel_load_range(const nir_intrinsic_instr *load,
                                   unsigned location)
{
   nir_instr *instr;
   nir_intrinsic_instr *bary;

   if (!load || load->intrinsic != nir_intrinsic_load_interpolated_input ||
       nir_intrinsic_io_semantics(load).location != location ||
       !load->src[0].ssa ||
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
chrome9_fs_smooth_pixel_load(const nir_intrinsic_instr *load,
                             unsigned location)
{
   return chrome9_fs_smooth_pixel_load_range(load, location) &&
          !nir_intrinsic_component(load);
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

struct chrome9_ps_proof {
   unsigned budget;
   void *mem_ctx;
   struct hash_table_u64 *memo;
};

struct chrome9_ps_nir_builder {
   struct chrome9_ps_ir ir;
   struct chrome9_ps_proof proof;
   struct {
      nir_def *def;
      struct chrome9_ps_source source;
      bool complete;
   } value[CHROME9_PS_MAX_INSTRUCTIONS];
   uint32_t immediate[32][4];
   uint32_t immediate_mask;
   uint32_t uniform_mask;
   uint8_t immediate_lane_mask[32];
   uint8_t value_count;
   uint8_t next_temp;
   uint8_t alu_count;
   uint8_t texture_count;
   uint32_t equal_zero_input_mask;
   uint32_t identity_projector_input_mask;
   uint32_t unit_interval_input_mask;
   struct {
      nir_def *predicate;
      uint8_t temp;
   } equal_zero[16];
   uint8_t equal_zero_count;
};

static struct chrome9_ps_instruction *
chrome9_ps_append_instruction(struct chrome9_ps_nir_builder *builder,
                              enum chrome9_ps_opcode op)
{
   struct chrome9_ps_instruction *instruction;

   if (builder->ir.count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return NULL;
   if (op == CHROME9_PS_OPCODE_TEX || op == CHROME9_PS_OPCODE_KILL) {
      if (builder->texture_count >= CHROME9_PS_MAX_TAU)
         return NULL;
      builder->texture_count++;
   }
   if (op != CHROME9_PS_OPCODE_TEX) {
      if (builder->alu_count >= CHROME9_PS_MAX_ALU)
         return NULL;
      builder->alu_count++;
   }
   instruction = &builder->ir.instruction[builder->ir.count++];
   memset(instruction, 0, sizeof(*instruction));
   instruction->op = op;
   return instruction;
}

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
chrome9_ps_lower_alu_source_depth(struct chrome9_ps_nir_builder *builder,
                                  const nir_alu_src *nir_source,
                                  unsigned components,
                                  struct chrome9_ps_source *source,
                                  unsigned depth,
                                  bool allow_modifier)
{
   struct chrome9_ps_source base;

   if (!nir_source->src.ssa || !components || components > 4 || depth > 8 ||
       nir_source->src.ssa->bit_size != 32)
      return false;
   nir_instr *instr = nir_def_instr(nir_source->src.ssa);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_fneg) {
         nir_alu_src folded = alu->src[0];

         for (unsigned c = 0; c < components; c++) {
            unsigned component = nir_source->swizzle[c];

            if (component >= alu->def.num_components)
               return false;
            folded.swizzle[c] = alu->src[0].swizzle[component];
         }
         if (chrome9_ps_lower_alu_source_depth(builder, &folded, components,
                                               source, depth + 1,
                                               allow_modifier)) {
            uint8_t modifier = source->modifier ==
               CHROME9_PS_SOURCE_NEGATE ? 0 : CHROME9_PS_SOURCE_NEGATE;

            /* MISC/POSITION has no qualified source-modifier encoding.  Its
             * fneg keeps the existing MUL(-1) fallback instead. */
            if ((!modifier || allow_modifier) &&
                (source->file != CHROME9_PS_SOURCE_POSITION || !modifier)) {
               source->modifier = modifier;
               return true;
            }
         }
      }
   }
   if (!chrome9_ps_lower_def(builder, nir_source->src.ssa, &base))
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
chrome9_ps_lower_alu_source(struct chrome9_ps_nir_builder *builder,
                            const nir_alu_src *nir_source,
                            unsigned components,
                            struct chrome9_ps_source *source)
{
   return chrome9_ps_lower_alu_source_depth(builder, nir_source, components,
                                             source, 0, false);
}

static bool
chrome9_ps_lower_alu_source_modifier(struct chrome9_ps_nir_builder *builder,
                                     const nir_alu_src *nir_source,
                                     unsigned components,
                                     struct chrome9_ps_source *source)
{
   return chrome9_ps_lower_alu_source_depth(builder, nir_source, components,
                                             source, 0, true);
}

static bool
chrome9_ps_source_modifier_allowed(enum chrome9_ps_opcode op,
                                   unsigned source)
{
   /* Only the A operand of MAD and CMP has a silicon-qualified NEG source
    * modifier.  Keep every other opcode/slot on the explicit MUL(-1)
    * fallback until its native modifier is qualified independently. */
   return source == 0 &&
          (op == CHROME9_PS_OPCODE_MAD || op == CHROME9_PS_OPCODE_CMP);
}

static bool
chrome9_ps_immediate(struct chrome9_ps_nir_builder *builder,
                     const uint32_t value[4], unsigned components,
                     struct chrome9_ps_source *source)
{
   uint32_t best_values[4] = { 0 };
   uint8_t best_swizzle[4] = { 0 };
   uint8_t best_mask = 0;
   unsigned best_new_lanes = 5;
   unsigned id = 32;

   if (!components || components > 4)
      return false;
   for (unsigned candidate = 32; candidate-- > 16;) {
      uint32_t packed[4];
      uint8_t swizzle[4] = { 0 };
      uint8_t mask = builder->immediate_lane_mask[candidate];
      unsigned new_lanes = 0;
      unsigned c;

      /* Uniform ownership was collected before lowering any literals. */
      if (builder->uniform_mask & (1u << candidate))
         continue;
      memcpy(packed, builder->immediate[candidate], sizeof(packed));
      for (c = 0; c < components; c++) {
         unsigned lane;

         for (lane = 0; lane < 4; lane++)
            if ((mask & (1u << lane)) && packed[lane] == value[c])
               break;
         if (lane == 4) {
            for (lane = 0; lane < 4; lane++)
               if (!(mask & (1u << lane)))
                  break;
            if (lane == 4)
               break;
            packed[lane] = value[c];
            mask |= 1u << lane;
            new_lanes++;
         }
         swizzle[c] = lane;
      }
      if (c == components && new_lanes < best_new_lanes) {
         id = candidate;
         best_new_lanes = new_lanes;
         best_mask = mask;
         memcpy(best_values, packed, sizeof(best_values));
         memcpy(best_swizzle, swizzle, sizeof(best_swizzle));
      }
   }
   if (id == 32)
      return false;
   memcpy(builder->immediate[id], best_values, sizeof(best_values));
   builder->immediate_lane_mask[id] = best_mask;
   builder->immediate_mask |= 1u << id;
   memset(source, 0, sizeof(*source));
   source->file = CHROME9_PS_SOURCE_CONSTANT;
   source->id = id;
   for (unsigned c = 0; c < 4; c++)
      source->swizzle[c] = c < components ? best_swizzle[c] :
                           CHROME9_PS_SWIZZLE_ZERO;
   return true;
}

static bool
chrome9_ps_fuse_source_saturate(struct chrome9_ps_nir_builder *builder,
                                nir_def *source_def,
                                const struct chrome9_ps_source *source)
{
   struct chrome9_ps_instruction *definition = NULL;
   bool direct = false;
   uint8_t selected = 0;

   if (source->file != CHROME9_PS_SOURCE_TEMP || source->modifier)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (source->swizzle[c] < CHROME9_PS_SWIZZLE_ONE)
         selected |= 1u << source->swizzle[c];
   if (!selected)
      return false;
   /* MOV/vec aliases may lower to an older physical TEMP.  Saturating that
    * TEMP would also clamp the producer's unsaturated users.  Require this
    * exact SSA definition to own the physical result being modified. */
   for (unsigned i = 0; i < builder->value_count; i++) {
      if (builder->value[i].complete &&
          builder->value[i].def == source_def &&
          builder->value[i].source.file == source->file &&
          builder->value[i].source.id == source->id) {
         direct = true;
         break;
      }
   }
   if (!direct)
      return false;
   for (unsigned i = 0; i < builder->ir.count; i++) {
      struct chrome9_ps_instruction *instruction =
         &builder->ir.instruction[i];

      if (instruction->destination.file != CHROME9_PS_DESTINATION_TEMP ||
          instruction->destination.id != source->id)
         continue;
      if (definition || instruction->op == CHROME9_PS_OPCODE_TEX ||
          instruction->op == CHROME9_PS_OPCODE_KILL)
         return false;
      definition = instruction;
   }
   if (!definition ||
       (selected & ~definition->destination.writemask))
      return false;
   definition->destination.saturate = true;
   return true;
}

/* The semantic ABI addresses c0..c31 as vec4 registers.  State tracker
 * usually emits load_ubo_vec4; direct Gallium NIR may use byte-addressed
 * load_ubo instead.  Accept only constant, in-register reads from CB0. */
static bool
chrome9_ps_ubo_load(const nir_intrinsic_instr *load,
                    unsigned *id, unsigned *component)
{
   unsigned word;

   if ((load->intrinsic != nir_intrinsic_load_ubo &&
        load->intrinsic != nir_intrinsic_load_ubo_vec4) ||
       !nir_src_is_const(load->src[0]) ||
       nir_src_as_uint(load->src[0]) ||
       !nir_src_is_const(load->src[1]) ||
       load->def.bit_size != 32 || !load->def.num_components ||
       load->def.num_components > 4)
      return false;
   if (load->intrinsic == nir_intrinsic_load_ubo) {
      unsigned offset = nir_src_as_uint(load->src[1]);

      if (offset & 3)
         return false;
      word = offset / 4;
   } else {
      unsigned offset = nir_src_as_uint(load->src[1]);
      unsigned lane = nir_intrinsic_component(load);

      if (offset >= 32 || nir_intrinsic_base(load) >= 32 - offset ||
          lane >= 4)
         return false;
      word = (nir_intrinsic_base(load) + offset) * 4 +
             lane;
   }
   *id = word / 4;
   *component = word % 4;
   return *id < 32 && *component + load->def.num_components <= 4;
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
      unsigned id, component;
      unsigned source_component = 0;

      /* Vector shrinking may turn a primary-color .aaaa read into a
       * one-component load at component 3.  The semantic backend must retain
       * that source lane even though the compatibility classifier requires a
       * full, unshifted primary-color load. */
      if (chrome9_fs_smooth_pixel_load_range(load, VARYING_SLOT_COL0) &&
          def->bit_size == 32 && def->num_components &&
          nir_intrinsic_component(load) < 4 &&
          def->num_components <= 4 - nir_intrinsic_component(load)) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_INPUT;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               nir_intrinsic_component(load) + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      if (load->intrinsic == nir_intrinsic_load_frag_coord &&
          def->bit_size == 32 && def->num_components == 4) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_POSITION;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c;
         return true;
      }

      /* The state tracker can lower gl_FragCoord to the POSITION input
       * rather than leave load_frag_coord in the linked fragment shader.
       * Accept only the same pixel-centered, smooth source; an indirect or
       * differently interpolated load is not the screen-position register. */
      if (chrome9_fs_smooth_pixel_load_range(load, VARYING_SLOT_POS) &&
          def->bit_size == 32 && def->num_components &&
          nir_intrinsic_component(load) < 4 &&
          def->num_components <= 4 - nir_intrinsic_component(load)) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_POSITION;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               nir_intrinsic_component(load) + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

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

      if (chrome9_ps_ubo_load(load, &id, &component)) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_CONSTANT;
         source->id = id;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               component + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }

      switch (load->intrinsic) {
      case nir_intrinsic_load_input:
         if (!nir_intrinsic_component(load))
            location = nir_intrinsic_io_semantics(load).location;
         break;
      case nir_intrinsic_load_interpolated_input:
         for (unsigned candidate = 0; candidate < 8; candidate++) {
            if (chrome9_fs_smooth_pixel_load_range(
                   load, VARYING_SLOT_VAR0 + candidate)) {
               location = VARYING_SLOT_VAR0 + candidate;
               source_component = nir_intrinsic_component(load);
               break;
            }
            if (chrome9_fs_smooth_pixel_load_range(
                   load, VARYING_SLOT_TEX0 + candidate)) {
               location = VARYING_SLOT_TEX0 + candidate;
               source_component = nir_intrinsic_component(load);
               break;
            }
         }
         break;
      case nir_intrinsic_load_deref: {
         nir_deref_instr *deref = nir_src_as_deref(load->src[0]);
         nir_variable *var = nir_intrinsic_get_var(load, 0);

         if (deref && deref->deref_type == nir_deref_type_var && var &&
             ((var->data.location >= VARYING_SLOT_VAR0 &&
               var->data.location < VARYING_SLOT_VAR0 + 8) ||
              (var->data.location >= VARYING_SLOT_TEX0 &&
               var->data.location < VARYING_SLOT_TEX0 + 8)) &&
             chrome9_fs_smooth_variable(var, var->data.location))
            location = var->data.location;
         break;
      }
      default:
         break;
      }
      if (((location >= VARYING_SLOT_VAR0 &&
            location < VARYING_SLOT_VAR0 + 8) ||
           (location >= VARYING_SLOT_TEX0 &&
            location < VARYING_SLOT_TEX0 + 8)) && def->bit_size == 32 &&
          def->num_components && source_component < 4 &&
          def->num_components <= 4 - source_component) {
         memset(source, 0, sizeof(*source));
         source->file = CHROME9_PS_SOURCE_TEXTURE;
         source->id = location >= VARYING_SLOT_VAR0 &&
                      location < VARYING_SLOT_VAR0 + 8 ?
                         location - VARYING_SLOT_VAR0 :
                         location - VARYING_SLOT_TEX0;
         for (unsigned c = 0; c < 4; c++)
            source->swizzle[c] = c < def->num_components ?
               source_component + c : CHROME9_PS_SWIZZLE_ZERO;
         return true;
      }
   }
   return false;
}

/* A defined FRC result lies in [0, 1], as does a selection between two
 * values already in that interval.  Bound the DAG walk independently of the
 * shader's size; an excessive graph must fail closed. */
static bool
chrome9_ps_unit_interval(nir_scalar scalar, unsigned *budget)
{
   nir_alu_instr *alu;

   if (!*budget)
      return false;
   (*budget)--;
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32 ||
       nir_def_instr(scalar.def)->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(nir_def_instr(scalar.def));
   if (alu->op == nir_op_ffract)
      return !alu->fp_math_ctrl;
   if (alu->op != nir_op_bcsel || alu->fp_math_ctrl ||
       scalar.comp >= alu->def.num_components)
      return false;
   for (unsigned arm = 1; arm <= 2; arm++) {
      nir_alu_src *input = &alu->src[arm];

      if (!input->src.ssa || input->src.ssa->bit_size != 32 ||
          input->swizzle[scalar.comp] >= input->src.ssa->num_components ||
          !chrome9_ps_unit_interval(
             (nir_scalar) { input->src.ssa, input->swizzle[scalar.comp] },
             budget))
         return false;
   }
   return true;
}

/* Generic draws reject non-finite constants, positions and varying lanes;
 * supported texture formats decode finite normalized values.  Prove that a
 * scalar DAG cannot introduce a *defined* exceptional value.  Arithmetic
 * without a local exceptional-value preservation request may still overflow
 * or hit an undefined domain, but that result is not observably constrained
 * by NIR and therefore does not invalidate a later default-mode comparison. */
/* Memoized DAG proofs: each (def, component, property) is derived once and
 * the budget bounds the number of distinct nodes, not the tree expansion. */
enum chrome9_ps_proof_kind {
   CHROME9_PS_PROOF_FINITE,
   CHROME9_PS_PROOF_NONNEGATIVE,
   CHROME9_PS_PROOF_BOUND,
};

#define CHROME9_PS_PROOF_NODES 4096

/* The memo is allocated lazily on the shader's ralloc context. */
static struct hash_table_u64 *
chrome9_ps_proof_table(struct chrome9_ps_proof *proof)
{
   if (!proof->memo)
      proof->memo = _mesa_hash_table_u64_create(proof->mem_ctx);
   return proof->memo;
}

static uint64_t
chrome9_ps_proof_key(nir_scalar scalar, enum chrome9_ps_proof_kind kind)
{
   return ((uint64_t)scalar.def->index << 8) | scalar.comp << 2 | kind;
}

/* Budget one node; bounds are recomputed but their nodes are counted. */
static bool
chrome9_ps_proof_visit(struct chrome9_ps_proof *proof, nir_scalar scalar,
                       enum chrome9_ps_proof_kind kind)
{
   (void)scalar;
   (void)kind;
   if (!proof->budget)
      return false;
   proof->budget--;
   return true;
}

static bool
chrome9_ps_proof_memo(struct chrome9_ps_proof *proof, nir_scalar scalar,
                      enum chrome9_ps_proof_kind kind,
                      bool (*derive)(nir_scalar, struct chrome9_ps_proof *))
{
   uint64_t key;
   void *known;
   bool result;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   key = chrome9_ps_proof_key(scalar, kind);
   if (!chrome9_ps_proof_table(proof))
      return false;
   known = _mesa_hash_table_u64_search(proof->memo, key);
   if (known)
      return known == (void *)(uintptr_t)2;
   if (!chrome9_ps_proof_visit(proof, scalar, kind))
      return false;
   result = derive(scalar, proof);
   _mesa_hash_table_u64_insert(proof->memo, key,
                               (void *)(uintptr_t)(result ? 2 : 1));
   return result;
}

static bool
chrome9_ps_default_finite_alu_source(nir_alu_instr *alu, unsigned source,
                                     unsigned components, struct chrome9_ps_proof *proof);

static nir_scalar
chrome9_ps_alu_scalar(nir_alu_instr *alu, unsigned source, unsigned comp)
{
   return nir_scalar_resolved(alu->src[source].src.ssa,
                              alu->src[source].swizzle[comp]);
}

/* True when a and b are the same value up to sign (fneg/fabs chains). */
static bool
chrome9_ps_same_magnitude(nir_scalar a, nir_scalar b)
{
   for (unsigned i = 0; i < 2; i++) {
      nir_scalar *s = i ? &b : &a;

      while (nir_def_instr(s->def)->type == nir_instr_type_alu) {
         nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(s->def));

         if (alu->op != nir_op_fneg && alu->op != nir_op_fabs &&
             alu->op != nir_op_mov)
            break;
         *s = chrome9_ps_alu_scalar(alu, 0, s->comp);
      }
   }
   return a.def == b.def && a.comp == b.comp;
}

/* Upper bound of |value| for a finite-input DAG, INFINITY when unknown.
 * normalize()'s v * rsq(dot(v, v)) is bounded by one plus rounding. */
static float
chrome9_ps_magnitude_bound_uncached(nir_scalar scalar,
                                    struct chrome9_ps_proof *proof);

static float
chrome9_ps_magnitude_bound(nir_scalar scalar, struct chrome9_ps_proof *proof)
{
   uint64_t key;
   void *known;
   float bound;
   uint32_t bits;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   key = chrome9_ps_proof_key(scalar, CHROME9_PS_PROOF_BOUND);
   if (!chrome9_ps_proof_table(proof))
      return INFINITY;
   known = _mesa_hash_table_u64_search(proof->memo, key);
   if (known) {
      bits = (uint32_t)(uintptr_t)known;
      memcpy(&bound, &bits, sizeof(bound));
      return bound;
   }
   bound = chrome9_ps_magnitude_bound_uncached(scalar, proof);
   memcpy(&bits, &bound, sizeof(bits));
   _mesa_hash_table_u64_insert(proof->memo, key,
                               (void *)(uintptr_t)((uint64_t)bits | 1ull << 32));
   return bound;
}

static float
chrome9_ps_magnitude_bound_uncached(nir_scalar scalar,
                                    struct chrome9_ps_proof *proof)
{
   nir_alu_instr *alu;
   float a, b;

   if (!chrome9_ps_proof_visit(proof, scalar, CHROME9_PS_PROOF_BOUND))
      return INFINITY;
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32)
      return INFINITY;
   if (nir_scalar_is_const(scalar)) {
      float value = fabsf(nir_scalar_as_float(scalar));

      return isfinite(value) ? value : INFINITY;
   }
   if (nir_def_instr(scalar.def)->type != nir_instr_type_alu)
      return INFINITY;
   alu = nir_instr_as_alu(nir_def_instr(scalar.def));
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fabs:
      return chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
   case nir_op_fsat:
      return 1.0f;
   case nir_op_fmin:
   case nir_op_fmax:
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return MAX2(a, b);
   case nir_op_fadd:
   case nir_op_fsub:
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return a + b;
   case nir_op_fmul:
      for (unsigned s = 0; s < 2; s++) {
         nir_scalar factor = chrome9_ps_alu_scalar(alu, 1 - s, scalar.comp);
         nir_scalar vector = chrome9_ps_alu_scalar(alu, s, scalar.comp);
         nir_alu_instr *rsq, *dot;
         unsigned n;
         bool matched = false;

         if (nir_def_instr(factor.def)->type != nir_instr_type_alu)
            continue;
         rsq = nir_instr_as_alu(nir_def_instr(factor.def));
         if (rsq->op != nir_op_frsq ||
             nir_def_instr(rsq->src[0].src.ssa)->type != nir_instr_type_alu)
            continue;
         dot = nir_instr_as_alu(nir_def_instr(rsq->src[0].src.ssa));
         if (dot->op != nir_op_fdot2 && dot->op != nir_op_fdot3 &&
             dot->op != nir_op_fdot4)
            continue;
         n = dot->op == nir_op_fdot2 ? 2 : dot->op == nir_op_fdot3 ? 3 : 4;
         for (unsigned c = 0; c < n && !matched; c++)
            matched = chrome9_ps_same_magnitude(
                         chrome9_ps_alu_scalar(dot, 0, c),
                         chrome9_ps_alu_scalar(dot, 1, c)) &&
                      chrome9_ps_same_magnitude(
                         chrome9_ps_alu_scalar(dot, 0, c), vector);
         for (unsigned c = 0; c < n && matched; c++)
            matched = chrome9_ps_same_magnitude(
               chrome9_ps_alu_scalar(dot, 0, c),
               chrome9_ps_alu_scalar(dot, 1, c));
         if (matched)
            return 1.0f + 4.0f * FLT_EPSILON;
      }
      a = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 0, scalar.comp), proof);
      b = chrome9_ps_magnitude_bound(
         chrome9_ps_alu_scalar(alu, 1, scalar.comp), proof);
      return a * b;
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned n = alu->op == nir_op_fdot2 ? 2 :
                   alu->op == nir_op_fdot3 ? 3 : 4;
      float sum = 0.0f;

      for (unsigned c = 0; c < n; c++)
         sum += chrome9_ps_magnitude_bound(
                   chrome9_ps_alu_scalar(alu, 0, c), proof) *
                chrome9_ps_magnitude_bound(
                   chrome9_ps_alu_scalar(alu, 1, c), proof);
      return sum;
   }
   default:
      return INFINITY;
   }
}

static bool
chrome9_ps_default_finite_uncached(nir_scalar scalar,
                                   struct chrome9_ps_proof *proof);

static bool
chrome9_ps_default_finite(nir_scalar scalar, struct chrome9_ps_proof *proof)
{
   return chrome9_ps_proof_memo(proof, scalar, CHROME9_PS_PROOF_FINITE,
                                chrome9_ps_default_finite_uncached);
}

static bool
chrome9_ps_default_finite_uncached(nir_scalar scalar,
                                   struct chrome9_ps_proof *proof)
{
   nir_instr *instr;
   nir_alu_instr *alu;
   unsigned sources = 0;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32 || scalar.comp >= scalar.def->num_components)
      return false;
   instr = nir_def_instr(scalar.def);
   if (instr->type == nir_instr_type_load_const) {
      nir_load_const_instr *load = nir_instr_as_load_const(instr);
      float value = load->value[scalar.comp].f32;

      return isfinite(value);
   }
   /* The normal semantic-leaf validation remains authoritative.  These are
    * merely the leaf classes whose values the draw path proves finite. */
   if (instr->type == nir_instr_type_intrinsic ||
       instr->type == nir_instr_type_tex)
      return true;
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   /* Value-preserving ops cannot create an exceptional value from finite
    * inputs, so their preserve request is vacuous.  Other flagged arithmetic
    * is admitted only when a magnitude bound excludes overflow. */
   if (alu->fp_math_ctrl & nir_fp_preserve_sz_inf_nan) {
      switch (alu->op) {
      case nir_op_mov:
      case nir_op_fneg:
      case nir_op_fabs:
      case nir_op_fsat:
      case nir_op_fmin:
      case nir_op_fmax:
         break;
      case nir_op_fadd:
      case nir_op_fsub:
      case nir_op_fmul:
      case nir_op_fdot2:
      case nir_op_fdot3:
      case nir_op_fdot4:
         if (chrome9_ps_magnitude_bound(scalar, proof) < FLT_MAX)
            break;
         FALLTHROUGH;
      default:
         return false;
      }
   }
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fabs:
   case nir_op_fsat:
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt:
   case nir_op_ffract:
      sources = 1;
      break;
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_fpow:
      sources = 2;
      break;
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned components = alu->op == nir_op_fdot2 ? 2 :
                            alu->op == nir_op_fdot3 ? 3 : 4;

      for (unsigned s = 0; s < 2; s++)
         if (!chrome9_ps_default_finite_alu_source(alu, s, components,
                                                    proof))
            return false;
      return true;
   }
   default:
      return false;
   }
   for (unsigned s = 0; s < sources; s++) {
      if (!alu->src[s].src.ssa ||
          alu->src[s].swizzle[scalar.comp] >=
             alu->src[s].src.ssa->num_components ||
          !chrome9_ps_default_finite(
             (nir_scalar) { alu->src[s].src.ssa,
                            alu->src[s].swizzle[scalar.comp] }, proof))
         return false;
   }
   return true;
}

static bool
chrome9_ps_default_finite_alu_source(nir_alu_instr *alu, unsigned source,
                                     unsigned components, struct chrome9_ps_proof *proof)
{
   if (!alu->src[source].src.ssa ||
       alu->src[source].src.ssa->bit_size != 32)
      return false;
   for (unsigned c = 0; c < components; c++) {
      unsigned component = alu->src[source].swizzle[c];

      if (component >= alu->src[source].src.ssa->num_components ||
          !chrome9_ps_default_finite(
             (nir_scalar) { alu->src[source].src.ssa, component }, proof))
         return false;
   }
   return true;
}

/* Prove a nonnegative result for the lighting DAG shapes emitted by generic
 * NIR.  This is a mathematical property, not an application/shader match. */
static bool
chrome9_ps_default_nonnegative_uncached(nir_scalar scalar,
                                        struct chrome9_ps_proof *proof);

static bool
chrome9_ps_default_nonnegative(nir_scalar scalar,
                               struct chrome9_ps_proof *proof)
{
   return chrome9_ps_proof_memo(proof, scalar, CHROME9_PS_PROOF_NONNEGATIVE,
                                chrome9_ps_default_nonnegative_uncached);
}

static bool
chrome9_ps_default_nonnegative_uncached(nir_scalar scalar,
                                        struct chrome9_ps_proof *proof)
{
   nir_instr *instr;
   nir_alu_instr *alu;

   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.def->bit_size != 32 || scalar.comp >= scalar.def->num_components)
      return false;
   instr = nir_def_instr(scalar.def);
   if (instr->type == nir_instr_type_load_const) {
      float value = nir_instr_as_load_const(instr)->value[scalar.comp].f32;

      return isfinite(value) && value >= 0.0f;
   }
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   if ((alu->fp_math_ctrl & nir_fp_preserve_sz_inf_nan) &&
       alu->op != nir_op_fmax)
      return false;
   switch (alu->op) {
   case nir_op_mov:
      if (!alu->src[0].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components)
         return false;
      return chrome9_ps_default_nonnegative(
         (nir_scalar) { alu->src[0].src.ssa,
                        alu->src[0].swizzle[scalar.comp] }, proof);
   case nir_op_fabs:
   case nir_op_fsat:
      if (!alu->src[0].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components)
         return false;
      return chrome9_ps_default_finite(
         (nir_scalar) { alu->src[0].src.ssa,
                        alu->src[0].swizzle[scalar.comp] }, proof);
   case nir_op_fsqrt:
   case nir_op_frsq:
      if (!alu->src[0].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components)
         return false;
      return chrome9_ps_default_nonnegative(
         (nir_scalar) { alu->src[0].src.ssa,
                        alu->src[0].swizzle[scalar.comp] }, proof);
   case nir_op_fmul:
      if (!alu->src[0].src.ssa || !alu->src[1].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components ||
          alu->src[1].swizzle[scalar.comp] >=
             alu->src[1].src.ssa->num_components)
         return false;
      return chrome9_ps_default_nonnegative(
                (nir_scalar) { alu->src[0].src.ssa,
                               alu->src[0].swizzle[scalar.comp] }, proof) &&
             chrome9_ps_default_nonnegative(
                (nir_scalar) { alu->src[1].src.ssa,
                               alu->src[1].swizzle[scalar.comp] }, proof);
   case nir_op_fmax: {
      if (!alu->src[0].src.ssa || !alu->src[1].src.ssa ||
          alu->src[0].swizzle[scalar.comp] >=
             alu->src[0].src.ssa->num_components ||
          alu->src[1].swizzle[scalar.comp] >=
             alu->src[1].src.ssa->num_components)
         return false;
      nir_scalar a = { alu->src[0].src.ssa,
                       alu->src[0].swizzle[scalar.comp] };
      nir_scalar b = { alu->src[1].src.ssa,
                       alu->src[1].swizzle[scalar.comp] };
      bool a_nonnegative = chrome9_ps_default_nonnegative(a, proof);
      bool b_nonnegative = chrome9_ps_default_nonnegative(b, proof);

      if (!chrome9_ps_default_finite(a, proof) ||
          !chrome9_ps_default_finite(b, proof))
         return false;
      return a_nonnegative || b_nonnegative;
   }
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned components = alu->op == nir_op_fdot2 ? 2 :
                            alu->op == nir_op_fdot3 ? 3 : 4;

      for (unsigned c = 0; c < components; c++)
         if (alu->src[0].swizzle[c] != alu->src[1].swizzle[c])
            return false;
      if (alu->src[0].src.ssa != alu->src[1].src.ssa)
         return false;
      return chrome9_ps_default_finite_alu_source(alu, 0, components,
                                                   proof);
   }
   default:
      return false;
   }
}

static bool
chrome9_ps_emit_scalar_select(struct chrome9_ps_nir_builder *builder,
                              nir_alu_instr *select, nir_def *def,
                              uint8_t condition_temp,
                              struct chrome9_ps_source *source)
{
   struct chrome9_ps_source true_value, false_value;
   struct chrome9_ps_instruction *conditional;
   unsigned components = def->num_components, value;
   uint8_t result_temp;

   if (!chrome9_ps_lower_alu_source(builder, &select->src[1], components,
                                    &true_value) ||
       !chrome9_ps_lower_alu_source(builder, &select->src[2], components,
                                    &false_value) ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   result_temp = builder->next_temp++;
   conditional = chrome9_ps_append_instruction(builder,
                                                CHROME9_PS_OPCODE_CMP);
   if (!conditional)
      return false;
   conditional->destination.file = CHROME9_PS_DESTINATION_TEMP;
   conditional->destination.id = result_temp;
   conditional->destination.writemask = (1u << components) - 1;
   conditional->source[0].file = CHROME9_PS_SOURCE_TEMP;
   conditional->source[0].id = condition_temp;
   for (unsigned c = 0; c < 4; c++)
      conditional->source[0].swizzle[c] = c < components ?
         CHROME9_PS_SWIZZLE_X : CHROME9_PS_SWIZZLE_ZERO;
   conditional->source[1] = true_value;
   conditional->source[2] = false_value;

   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
   builder->value[value].source.id = result_temp;
   for (unsigned c = 0; c < 4; c++)
      builder->value[value].source.swizzle[c] = c < components ?
         c : CHROME9_PS_SWIZZLE_ZERO;
   builder->value[value].complete = true;
   *source = builder->value[value].source;
   return true;
}

/* CMP selects its C operand when A >= 0.  A subtraction can implement an
 * ordered floating comparison only when its sign is preserved.  Admit a
 * proven [0, 1] value around a central normal threshold, or a nonnegative
 * defined-finite DAG against a positive normal constant.  The latter cannot
 * overflow and default-mode FTZ is qualified consistently for producer and
 * SUB/CMP paths.  Other comparisons remain closed. */
static bool
chrome9_ps_lower_unit_interval_select(struct chrome9_ps_nir_builder *builder,
                                       nir_alu_instr *select, nir_def *def,
                                       struct chrome9_ps_source *source)
{
   nir_alu_instr *compare;
   nir_scalar predicate, bounded, threshold;
   struct chrome9_ps_source lhs, rhs;
   struct chrome9_ps_instruction *subtract;
   uint8_t condition_temp;
   unsigned components = def->num_components;
   unsigned range_budget = CHROME9_PS_MAX_INSTRUCTIONS;
   float boundary;

   if (select->op != nir_op_bcsel || !components || components > 4 ||
       !select->src[0].src.ssa ||
       select->src[0].src.ssa->bit_size != 1 ||
       select->src[0].swizzle[0] >=
          select->src[0].src.ssa->num_components)
      return false;
   /* One scalar predicate is broadcast across the live result lanes.
    * A vector predicate requires distinct comparisons and cannot use this
    * single native CMP record. */
   for (unsigned c = 1; c < components; c++)
      if (select->src[0].swizzle[c] != select->src[0].swizzle[0])
         return false;
   predicate = nir_scalar_resolved(select->src[0].src.ssa,
                                   select->src[0].swizzle[0]);
   if (predicate.comp || nir_def_instr(predicate.def)->type !=
                            nir_instr_type_alu)
      return false;
   compare = nir_instr_as_alu(nir_def_instr(predicate.def));
   if (compare->op != nir_op_fge ||
       (compare->fp_math_ctrl & ~nir_fp_preserve_inf) ||
       !compare->src[0].src.ssa || !compare->src[1].src.ssa ||
       compare->src[0].src.ssa->bit_size != 32 ||
       compare->src[1].src.ssa->bit_size != 32 ||
       compare->src[0].swizzle[0] >=
          compare->src[0].src.ssa->num_components ||
       compare->src[1].swizzle[0] >=
          compare->src[1].src.ssa->num_components)
      return false;
   bounded = (nir_scalar) { compare->src[0].src.ssa,
                            compare->src[0].swizzle[0] };
   threshold = nir_scalar_resolved(compare->src[1].src.ssa,
                                   compare->src[1].swizzle[0]);
   if (!nir_scalar_is_const(threshold))
      return false;
   boundary = nir_scalar_as_float(threshold);
   if (!((boundary >= 0.25f && boundary <= 0.75f &&
          chrome9_ps_unit_interval(bounded, &range_budget)) ||
         (boundary > 0.0f && isnormal(boundary) &&
          chrome9_ps_default_nonnegative(bounded, &builder->proof))))
      return false;
   if (!chrome9_ps_lower_alu_source(builder, &compare->src[0], 1, &lhs) ||
       !chrome9_ps_lower_alu_source(builder, &compare->src[1], 1, &rhs) ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   condition_temp = builder->next_temp++;
   subtract = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_SUB);
   if (!subtract)
      return false;
   subtract->destination.file = CHROME9_PS_DESTINATION_TEMP;
   subtract->destination.id = condition_temp;
   subtract->destination.writemask = 1;
   subtract->source[0] = lhs;
   subtract->source[1] = rhs;
   return chrome9_ps_emit_scalar_select(builder, select, def,
                                         condition_temp, source);
}

/* Ordered FEQ against zero is true for either signed zero and false for every
 * finite nonzero value.  ABS followed by 0-ABS gives CMP that sign test, but
 * only if the compared varying cannot interpolate through zero or enter an
 * exceptional/denormal domain.  The recorded lane is checked across the
 * complete post-vertex draw before any command is submitted. */
static bool
chrome9_ps_lower_equal_zero_select(struct chrome9_ps_nir_builder *builder,
                                   nir_alu_instr *select, nir_def *def,
                                   struct chrome9_ps_source *source)
{
   static const uint32_t zero[4] = { 0 };
   nir_scalar predicate, lhs, rhs;
   nir_alu_instr *compare;
   struct chrome9_ps_source input, zero_source;
   struct chrome9_ps_instruction *absolute, *negative_absolute;
   uint8_t absolute_temp, condition_temp;
   unsigned components = def->num_components;
   unsigned dynamic_side;

   if (select->op != nir_op_bcsel || !components || components > 4 ||
       !select->src[0].src.ssa ||
       select->src[0].src.ssa->bit_size != 1 ||
       select->src[0].swizzle[0] >=
          select->src[0].src.ssa->num_components)
      return false;
   for (unsigned c = 1; c < components; c++)
      if (select->src[0].swizzle[c] != select->src[0].swizzle[0])
         return false;
   predicate = nir_scalar_resolved(select->src[0].src.ssa,
                                   select->src[0].swizzle[0]);
   if (predicate.comp || nir_def_instr(predicate.def)->type !=
                            nir_instr_type_alu)
      return false;
   compare = nir_instr_as_alu(nir_def_instr(predicate.def));
   if (compare->op != nir_op_feq ||
       compare->def.num_components != 1 ||
       (compare->fp_math_ctrl & ~(nir_fp_preserve_signed_zero |
                                  nir_fp_preserve_inf |
                                  nir_fp_preserve_nan)) ||
       !compare->src[0].src.ssa || !compare->src[1].src.ssa ||
       compare->src[0].src.ssa->bit_size != 32 ||
       compare->src[1].src.ssa->bit_size != 32 ||
       compare->src[0].swizzle[0] >=
          compare->src[0].src.ssa->num_components ||
       compare->src[1].swizzle[0] >=
          compare->src[1].src.ssa->num_components)
      return false;
   lhs = nir_scalar_resolved(compare->src[0].src.ssa,
                             compare->src[0].swizzle[0]);
   rhs = nir_scalar_resolved(compare->src[1].src.ssa,
                             compare->src[1].swizzle[0]);
   if (nir_scalar_is_const(lhs) &&
       (nir_scalar_as_uint(lhs) & 0x7fffffff) == 0)
      dynamic_side = 1;
   else if (nir_scalar_is_const(rhs) &&
            (nir_scalar_as_uint(rhs) & 0x7fffffff) == 0)
      dynamic_side = 0;
   else
      return false;

   for (unsigned i = 0; i < builder->equal_zero_count; i++)
      if (builder->equal_zero[i].predicate == predicate.def)
         return chrome9_ps_emit_scalar_select(builder, select, def,
                                               builder->equal_zero[i].temp,
                                               source);
   if (builder->equal_zero_count >= ARRAY_SIZE(builder->equal_zero) ||
       builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - 2 ||
       !chrome9_ps_lower_alu_source(builder, &compare->src[dynamic_side],
                                    1, &input) ||
       input.file != CHROME9_PS_SOURCE_TEXTURE || input.id >= 8 ||
       input.swizzle[0] > CHROME9_PS_SWIZZLE_W ||
       !chrome9_ps_immediate(builder, zero, 1, &zero_source))
      return false;

   absolute_temp = builder->next_temp++;
   condition_temp = builder->next_temp++;
   absolute = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_ABS);
   negative_absolute = chrome9_ps_append_instruction(builder,
                                                      CHROME9_PS_OPCODE_SUB);
   if (!absolute || !negative_absolute)
      return false;
   absolute->destination.file = CHROME9_PS_DESTINATION_TEMP;
   absolute->destination.id = absolute_temp;
   absolute->destination.writemask = 1;
   absolute->source[0] = input;
   negative_absolute->destination.file = CHROME9_PS_DESTINATION_TEMP;
   negative_absolute->destination.id = condition_temp;
   negative_absolute->destination.writemask = 1;
   negative_absolute->source[0] = zero_source;
   negative_absolute->source[1].file = CHROME9_PS_SOURCE_TEMP;
   negative_absolute->source[1].id = absolute_temp;
   negative_absolute->source[1].swizzle[0] = CHROME9_PS_SWIZZLE_X;
   for (unsigned c = 1; c < 4; c++)
      negative_absolute->source[1].swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;

   builder->equal_zero[builder->equal_zero_count].predicate = predicate.def;
   builder->equal_zero[builder->equal_zero_count++].temp = condition_temp;
   builder->equal_zero_input_mask |=
      1u << (input.id * 4 + input.swizzle[0]);
   return chrome9_ps_emit_scalar_select(builder, select, def,
                                         condition_temp, source);
}

static bool
chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder, nir_def *def,
                     struct chrome9_ps_source *source);

/* Emit CMP(condition.x, when_nonnegative, when_negative) into a new temp. */
static bool
chrome9_ps_emit_cmp(struct chrome9_ps_nir_builder *builder,
                    uint8_t condition_temp,
                    const struct chrome9_ps_source *when_nonnegative,
                    const struct chrome9_ps_source *when_negative,
                    unsigned components, struct chrome9_ps_source *result)
{
   struct chrome9_ps_instruction *conditional;
   uint8_t temp;

   if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   temp = builder->next_temp++;
   conditional = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_CMP);
   if (!conditional)
      return false;
   conditional->destination.file = CHROME9_PS_DESTINATION_TEMP;
   conditional->destination.id = temp;
   conditional->destination.writemask = (1u << components) - 1;
   conditional->source[0].file = CHROME9_PS_SOURCE_TEMP;
   conditional->source[0].id = condition_temp;
   for (unsigned c = 0; c < 4; c++)
      conditional->source[0].swizzle[c] = c < components ?
         CHROME9_PS_SWIZZLE_X : CHROME9_PS_SWIZZLE_ZERO;
   conditional->source[1] = *when_nonnegative;
   conditional->source[2] = *when_negative;
   memset(result, 0, sizeof(*result));
   result->file = CHROME9_PS_SOURCE_TEMP;
   result->id = temp;
   for (unsigned c = 0; c < 4; c++)
      result->swizzle[c] = c < components ? c : CHROME9_PS_SWIZZLE_ZERO;
   return true;
}

/* Scalar lhs - rhs into a new temp; its sign orders the two operands. */
static bool
chrome9_ps_emit_difference(struct chrome9_ps_nir_builder *builder,
                           nir_alu_src *lhs, nir_alu_src *rhs,
                           uint8_t *condition_temp)
{
   struct chrome9_ps_source a, b;
   struct chrome9_ps_instruction *subtract;

   if (!chrome9_ps_lower_alu_source(builder, lhs, 1, &a) ||
       !chrome9_ps_lower_alu_source(builder, rhs, 1, &b) ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
      return false;
   *condition_temp = builder->next_temp++;
   subtract = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_SUB);
   if (!subtract)
      return false;
   subtract->destination.file = CHROME9_PS_DESTINATION_TEMP;
   subtract->destination.id = *condition_temp;
   subtract->destination.writemask = 1;
   subtract->source[0] = a;
   subtract->source[1] = b;
   return true;
}

/*
 * Select between two lowered values under a scalar boolean predicate without
 * materializing integers: ordered float compares become SUB+CMP, and
 * iand/ior/inot become nested or exchanged CMP arms.  Compares are admitted
 * only in the default float mode on operands proven free of defined
 * exceptional values; NaN preservation is never admitted.
 */
static bool
chrome9_ps_emit_predicate_select(struct chrome9_ps_nir_builder *builder,
                                 nir_scalar predicate,
                                 const struct chrome9_ps_source *when_true,
                                 const struct chrome9_ps_source *when_false,
                                 unsigned components, unsigned depth,
                                 struct chrome9_ps_source *result)
{
   nir_alu_instr *alu;
   struct chrome9_ps_source inner;
   uint8_t condition, reverse;

   if (depth > 8)
      return false;
   predicate = nir_scalar_resolved(predicate.def, predicate.comp);
   if (predicate.def->bit_size != 1)
      return false;
   if (nir_scalar_is_const(predicate)) {
      *result = nir_scalar_as_bool(predicate) ? *when_true : *when_false;
      return true;
   }
   if (nir_def_instr(predicate.def)->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(nir_def_instr(predicate.def));
   if (predicate.comp >= alu->def.num_components)
      return false;

   switch (alu->op) {
   case nir_op_inot:
      return chrome9_ps_emit_predicate_select(
         builder, nir_scalar_chase_alu_src(predicate, 0), when_false,
         when_true, components, depth + 1, result);
   case nir_op_iand:
      return chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 1), when_true,
                when_false, components, depth + 1, &inner) &&
             chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 0), &inner,
                when_false, components, depth + 1, result);
   case nir_op_ior:
      return chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 1), when_true,
                when_false, components, depth + 1, &inner) &&
             chrome9_ps_emit_predicate_select(
                builder, nir_scalar_chase_alu_src(predicate, 0), when_true,
                &inner, components, depth + 1, result);
   case nir_op_fge:
   case nir_op_flt:
   case nir_op_feq:
   case nir_op_fneu:
      break;
   default:
      return false;
   }
   if ((alu->fp_math_ctrl & nir_fp_preserve_nan) ||
       !alu->src[0].src.ssa || !alu->src[1].src.ssa ||
       alu->src[0].src.ssa->bit_size != 32 ||
       alu->src[1].src.ssa->bit_size != 32 ||
       !chrome9_ps_default_finite(nir_scalar_chase_alu_src(predicate, 0),
                                  &builder->proof) ||
       !chrome9_ps_default_finite(nir_scalar_chase_alu_src(predicate, 1),
                                  &builder->proof))
      return false;
   {
      /* Lower one component of each operand as a scalar source. */
      nir_alu_src lhs = alu->src[0], rhs = alu->src[1];

      lhs.swizzle[0] = alu->src[0].swizzle[predicate.comp];
      rhs.swizzle[0] = alu->src[1].swizzle[predicate.comp];
      if (!chrome9_ps_emit_difference(builder, &lhs, &rhs, &condition))
         return false;
      switch (alu->op) {
      case nir_op_fge:
         return chrome9_ps_emit_cmp(builder, condition, when_true,
                                    when_false, components, result);
      case nir_op_flt:
         return chrome9_ps_emit_cmp(builder, condition, when_false,
                                    when_true, components, result);
      default:
         /* Equality holds when both differences are nonnegative. */
         if (!chrome9_ps_emit_difference(builder, &rhs, &lhs, &reverse))
            return false;
         if (alu->op == nir_op_feq)
            return chrome9_ps_emit_cmp(builder, reverse, when_true,
                                       when_false, components, &inner) &&
                   chrome9_ps_emit_cmp(builder, condition, &inner,
                                       when_false, components, result);
         return chrome9_ps_emit_cmp(builder, reverse, when_false,
                                    when_true, components, &inner) &&
                chrome9_ps_emit_cmp(builder, condition, &inner,
                                    when_true, components, result);
      }
   }
}

static bool
chrome9_ps_record_value(struct chrome9_ps_nir_builder *builder, nir_def *def,
                        const struct chrome9_ps_source *result,
                        struct chrome9_ps_source *source)
{
   unsigned value;

   if (builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   value = builder->value_count++;
   builder->value[value].def = def;
   builder->value[value].source = *result;
   builder->value[value].complete = true;
   *source = *result;
   return true;
}

/* General bcsel and b2f32 under one broadcast scalar predicate. */
static bool
chrome9_ps_lower_general_select(struct chrome9_ps_nir_builder *builder,
                                nir_alu_instr *alu, nir_def *def,
                                struct chrome9_ps_source *source)
{
   static const uint32_t one[4] = {
      0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
   };
   static const uint32_t zero[4] = { 0 };
   const unsigned predicate_src = 0;
   unsigned components = def->num_components;
   struct chrome9_ps_source when_true, when_false, result;

   if ((alu->op != nir_op_bcsel && alu->op != nir_op_b2f32) ||
       !components || components > 4 ||
       !alu->src[predicate_src].src.ssa ||
       alu->src[predicate_src].src.ssa->bit_size != 1)
      return false;
   for (unsigned c = 1; c < components; c++)
      if (alu->src[predicate_src].swizzle[c] !=
          alu->src[predicate_src].swizzle[0])
         return false;
   if (alu->op == nir_op_bcsel) {
      if (!chrome9_ps_lower_alu_source(builder, &alu->src[1], components,
                                       &when_true) ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[2], components,
                                       &when_false))
         return false;
   } else if (!chrome9_ps_immediate(builder, one, components, &when_true) ||
              !chrome9_ps_immediate(builder, zero, components, &when_false)) {
      return false;
   }
   return chrome9_ps_emit_predicate_select(
             builder,
             (nir_scalar) { alu->src[predicate_src].src.ssa,
                            alu->src[predicate_src].swizzle[0] },
             &when_true, &when_false, components, 0, &result) &&
          chrome9_ps_record_value(builder, def, &result, source);
}

static bool
chrome9_ps_lower_def_impl(struct chrome9_ps_nir_builder *builder,
                          nir_def *def, struct chrome9_ps_source *source);
/* The hardware's guarded projector form: coordinate varying.xy over the
 * same varying's W. */
static bool
chrome9_ps_identity_projector(const nir_tex_instr *tex, int coord,
                              int projector,
                              const struct chrome9_ps_source *coordinate)
{
   nir_scalar x = nir_scalar_resolved(tex->src[coord].src.ssa, 0);
   nir_scalar y = nir_scalar_resolved(tex->src[coord].src.ssa, 1);
   nir_scalar q = nir_scalar_resolved(tex->src[projector].src.ssa, 0);

   return coordinate->file == CHROME9_PS_SOURCE_TEXTURE &&
          coordinate->swizzle[0] == CHROME9_PS_SWIZZLE_X &&
          coordinate->swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
          x.def == q.def && y.def == q.def &&
          x.comp == 0 && y.comp == 1 && q.comp == 3;
}


/* CHROME9_DEBUG_REJECT prints each rejected def, innermost first. */
static bool
chrome9_ps_lower_def(struct chrome9_ps_nir_builder *builder, nir_def *def,
                     struct chrome9_ps_source *source)
{
   if (chrome9_ps_lower_def_impl(builder, def, source))
      return true;
   if (def && debug_get_bool_option("CHROME9_DEBUG_REJECT", false)) {
      fprintf(stderr, "chrome9: reject ir=%u temps=%u: ",
              builder->ir.count, builder->next_temp);
      nir_print_instr(nir_def_instr(def), stderr);
      fputc('\n', stderr);
   }
   return false;
}

static bool
chrome9_ps_lower_def_impl(struct chrome9_ps_nir_builder *builder,
                          nir_def *def, struct chrome9_ps_source *source)
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
            move = chrome9_ps_append_instruction(builder,
                                                  CHROME9_PS_OPCODE_MOV);
            if (!move)
               return false;
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
      struct chrome9_ps_instruction *texture;
      struct chrome9_ps_source coordinate;
      int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      int projector = nir_tex_instr_src_index(tex, nir_tex_src_projector);

      if (tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
          tex->is_array || tex->is_shadow || tex->texture_index >= 8 ||
          tex->sampler_index != tex->texture_index || coord < 0 ||
          tex->src[coord].src.ssa->num_components != 2 ||
          builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
          builder->value_count >= CHROME9_PS_MAX_INSTRUCTIONS)
         return false;
      for (unsigned s = 0; s < tex->num_srcs; s++)
         if ((int)s != coord && (int)s != projector)
            return false;
      if (!chrome9_ps_lower_def(builder, tex->src[coord].src.ssa,
                                &coordinate) ||
          coordinate.swizzle[0] > CHROME9_PS_SWIZZLE_W ||
          coordinate.swizzle[1] > CHROME9_PS_SWIZZLE_W)
         return false;
      if (projector >= 0 &&
          !chrome9_ps_identity_projector(tex, coord, projector,
                                         &coordinate)) {
         struct chrome9_ps_source q;
         struct chrome9_ps_instruction *rcp, *mul;
         uint8_t rcp_temp, coord_temp;

         /* General projective lookup: TEX samples coord.xy * (1 / q)
          * through the computed-coordinate bridge. */
         if (tex->num_srcs != 2 ||
             tex->src[projector].src.ssa->num_components != 1 ||
             !chrome9_ps_lower_def(builder, tex->src[projector].src.ssa,
                                   &q) ||
             builder->next_temp > CHROME9_PS_MAX_VIRTUAL_TEMPS - 2)
            return false;
         rcp_temp = builder->next_temp++;
         coord_temp = builder->next_temp++;
         rcp = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_RCP);
         if (!rcp)
            return false;
         rcp->destination.file = CHROME9_PS_DESTINATION_TEMP;
         rcp->destination.id = rcp_temp;
         rcp->destination.writemask = 0x1;
         rcp->source[0] = q;
         mul = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MUL);
         if (!mul)
            return false;
         mul->destination.file = CHROME9_PS_DESTINATION_TEMP;
         mul->destination.id = coord_temp;
         mul->destination.writemask = 0x3;
         mul->source[0] = coordinate;
         mul->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         mul->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         memset(&mul->source[1], 0, sizeof(mul->source[1]));
         mul->source[1].file = CHROME9_PS_SOURCE_TEMP;
         mul->source[1].id = rcp_temp;
         for (unsigned c = 0; c < 4; c++)
            mul->source[1].swizzle[c] = c < 2 ? CHROME9_PS_SWIZZLE_X :
                                        CHROME9_PS_SWIZZLE_ZERO;
         memset(&coordinate, 0, sizeof(coordinate));
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = coord_temp;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
         projector = -1;
      }
      if (coordinate.file != CHROME9_PS_SOURCE_TEXTURE &&
          coordinate.file != CHROME9_PS_SOURCE_TEMP) {
         /* Constant or uniform coordinates reach TEX through a TEMP. */
         struct chrome9_ps_instruction *move;

         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = builder->next_temp++;
         move->destination.writemask = 0x3;
         move->source[0] = coordinate;
         move->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         move->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         memset(&coordinate, 0, sizeof(coordinate));
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = move->destination.id;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
      }
      if (projector >= 0) {
         struct chrome9_ps_source q;

         /* A native 2D TEX has no projective divide.  Accept only the
          * matching direct varying's W lane and guard its post-TNL value
          * across the entire draw before submitting any triangle. */
         if (tex->num_srcs != 2 ||
             tex->src[projector].src.ssa->num_components != 1 ||
             coordinate.file != CHROME9_PS_SOURCE_TEXTURE ||
             coordinate.id >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS ||
             coordinate.swizzle[0] != CHROME9_PS_SWIZZLE_X ||
             coordinate.swizzle[1] != CHROME9_PS_SWIZZLE_Y ||
             !chrome9_ps_lower_def(builder,
                                   tex->src[projector].src.ssa, &q) ||
             q.file != coordinate.file || q.id != coordinate.id ||
             q.swizzle[0] != CHROME9_PS_SWIZZLE_W)
            return false;
         builder->identity_projector_input_mask |=
            1u << (coordinate.id * 4 + 3);
      } else if (coordinate.file == CHROME9_PS_SOURCE_TEXTURE &&
                 (coordinate.swizzle[0] != CHROME9_PS_SWIZZLE_X ||
                  coordinate.swizzle[1] != CHROME9_PS_SWIZZLE_Y)) {
         /* Like D3D9 texld, a direct coordinate is the varying's XY; the
          * FE consumer is capped at three lanes.  Route a swizzled one
          * (e.g. packed lightmap .zw) through the computed-TEMP bridge. */
         struct chrome9_ps_instruction *move;

         if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         move = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_MOV);
         if (!move)
            return false;
         move->destination.file = CHROME9_PS_DESTINATION_TEMP;
         move->destination.id = builder->next_temp++;
         move->destination.writemask = 0x3;
         move->source[0] = coordinate;
         move->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         move->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ZERO;
         coordinate.file = CHROME9_PS_SOURCE_TEMP;
         coordinate.id = move->destination.id;
         coordinate.swizzle[0] = CHROME9_PS_SWIZZLE_X;
         coordinate.swizzle[1] = CHROME9_PS_SWIZZLE_Y;
      }
      if (builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
         return false;
      texture = chrome9_ps_append_instruction(builder,
                                               CHROME9_PS_OPCODE_TEX);
      if (!texture)
         return false;
      temp = builder->next_temp++;
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source.file = CHROME9_PS_SOURCE_TEMP;
      builder->value[value].source.id = temp;
      for (unsigned c = 0; c < 4; c++)
         builder->value[value].source.swizzle[c] = c;
      texture->sampler = tex->sampler_index;
      texture->destination.file = CHROME9_PS_DESTINATION_TEMP;
      texture->destination.id = temp;
      texture->destination.writemask = 0xf;
      texture->source[0] = coordinate;
      if (coordinate.file == CHROME9_PS_SOURCE_TEMP) {
         /* 2D TEX reads XY.  An XY-only computed TEMP must not appear to
          * read undefined Z/W lanes during kernel validation. */
         texture->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_ZERO;
         texture->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_ONE;
      } else {
         /* Direct varyings retain their native four-lane coordinate map;
          * the kernel's direct-input gate does not admit literals here. */
         texture->source[0].swizzle[2] = CHROME9_PS_SWIZZLE_W;
         texture->source[0].swizzle[3] = CHROME9_PS_SWIZZLE_W;
      }
      builder->value[value].complete = true;
      *source = builder->value[value].source;
      return true;
   }
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   components = alu->def.num_components;
   if (alu->op == nir_op_fsat && alu->src[0].src.ssa &&
       list_is_singular(&alu->src[0].src.ssa->uses) &&
       !nir_def_used_by_if(alu->src[0].src.ssa) &&
       chrome9_ps_lower_alu_source(builder, &alu->src[0], components,
                                    &operands[0]) &&
       builder->value_count < CHROME9_PS_MAX_INSTRUCTIONS &&
       chrome9_ps_fuse_source_saturate(builder, alu->src[0].src.ssa,
                                        &operands[0])) {
      value = builder->value_count++;
      builder->value[value].def = def;
      builder->value[value].source = operands[0];
      builder->value[value].complete = true;
      *source = operands[0];
      return true;
   }
   if (alu->op == nir_op_bcsel)
      return chrome9_ps_lower_unit_interval_select(builder, alu, def,
                                                    source) ||
             chrome9_ps_lower_equal_zero_select(builder, alu, def, source) ||
             chrome9_ps_lower_general_select(builder, alu, def, source);
   if (alu->op == nir_op_b2f32)
      return chrome9_ps_lower_general_select(builder, alu, def, source);
   for (unsigned s = 0; s < ARRAY_SIZE(source_components); s++)
      source_components[s] = components;
   if (alu->op == nir_op_fpow) {
      struct chrome9_ps_instruction *log2, *multiply, *exp2;
      uint32_t integer_exponent = 0;
      uint8_t log_temp, multiply_temp;

      /* Integer powers use multiplication, which also preserves zero raised
       * to a positive exponent.  Follow bit-exact MOV/vec aliases before
       * deciding whether the exponent is constant.  Other scalar powers use
       * the native LOG2, MUL and EXP2 sequence. */
      if (components == 1 && alu->src[1].src.ssa &&
          alu->src[1].src.ssa->bit_size == 32 &&
          alu->src[1].swizzle[0] <
             alu->src[1].src.ssa->num_components) {
         nir_scalar exponent = nir_scalar_resolved(
            alu->src[1].src.ssa, alu->src[1].swizzle[0]);

         if (nir_scalar_is_const(exponent)) {
            double power = nir_scalar_as_float(exponent);

            /* Every finite float32 value this large is integral.  The
             * multiplication chain uses a uint32_t exponent; rejecting a
             * larger integer avoids losing pow(0, positive) semantics. */
            if (power >= 0x1p32 && power < 0x1p128)
               return false;
            if (power >= 1.0 && power < 0x1p32 &&
                power == (double)(uint32_t)power)
               integer_exponent = (uint32_t)power;
         }
      }
      if (integer_exponent &&
          (integer_exponent == 1 ||
           builder->value_count < CHROME9_PS_MAX_INSTRUCTIONS)) {
         struct chrome9_ps_source powered;

         if (!chrome9_ps_lower_alu_source(builder, &alu->src[0], 1,
                                          &operands[0]))
            return false;
         if (chrome9_ps_emit_integer_pow(&builder->ir, operands[0],
                                         integer_exponent,
                                         &builder->next_temp,
                                         &builder->alu_count, &powered)) {
            if (integer_exponent > 1) {
               value = builder->value_count++;
               builder->value[value].def = def;
               builder->value[value].source = powered;
               builder->value[value].complete = true;
            }
            *source = powered;
            return true;
         }
      }
      if (integer_exponent)
         return false;
      /* Vector POW is scalarized by NIR before this scalar backend. */
      if (components != 1 ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[0], 1,
                                       &operands[0]) ||
          !chrome9_ps_lower_alu_source(builder, &alu->src[1], 1,
                                       &operands[1]) ||
          builder->alu_count > CHROME9_PS_MAX_ALU - 3 ||
          builder->ir.count > CHROME9_PS_MAX_INSTRUCTIONS - 3 ||
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

      log2 = chrome9_ps_append_instruction(builder,
                                            CHROME9_PS_OPCODE_LOG2);
      multiply = chrome9_ps_append_instruction(builder,
                                                CHROME9_PS_OPCODE_MUL);
      exp2 = chrome9_ps_append_instruction(builder,
                                            CHROME9_PS_OPCODE_EXP2);
      if (!log2 || !multiply || !exp2)
         return false;
      log2->destination.file = CHROME9_PS_DESTINATION_TEMP;
      log2->destination.id = log_temp;
      log2->destination.writemask = 1;
      log2->source[0] = operands[0];

      multiply->destination.file = CHROME9_PS_DESTINATION_TEMP;
      multiply->destination.id = multiply_temp;
      multiply->destination.writemask = 1;
      multiply->source[0].file = CHROME9_PS_SOURCE_TEMP;
      multiply->source[0].id = log_temp;
      multiply->source[0].swizzle[0] = CHROME9_PS_SWIZZLE_X;
      for (unsigned c = 1; c < 4; c++)
         multiply->source[0].swizzle[c] = CHROME9_PS_SWIZZLE_ZERO;
      multiply->source[1] = operands[1];

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
   case nir_op_fsat:
      /* Keep saturation on a distinct instruction: the producer may have
       * other unsaturated users or only some lanes may be clamped. */
      op = CHROME9_PS_OPCODE_MOV;
      source_count = 1;
      break;
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
   case nir_op_fdot2:
      /* The native dot product consumes XYZ.  Lowering two components pads
       * Z/W with the literal ZERO selector in lower_alu_source(). */
      op = CHROME9_PS_OPCODE_DP3;
      source_count = 2;
      source_components[0] = source_components[1] = 2;
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
   if (builder->alu_count >= CHROME9_PS_MAX_ALU ||
       builder->ir.count >= CHROME9_PS_MAX_INSTRUCTIONS ||
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
   for (unsigned s = 0; s < source_count; s++) {
      bool modifier = chrome9_ps_source_modifier_allowed(op, s);

      if (!(modifier ?
            chrome9_ps_lower_alu_source_modifier(builder, &alu->src[s],
                                                  source_components[s],
                                                  &operands[s]) :
            chrome9_ps_lower_alu_source(builder, &alu->src[s],
                                        source_components[s], &operands[s])))
         return false;
   }
   if (alu->op == nir_op_fneg) {
      static const uint32_t negative_one[4] = {
         0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000,
      };

      if (!chrome9_ps_immediate(builder, negative_one, components,
                                &operands[1]))
         return false;
      source_count = 2;
   }
   native = chrome9_ps_append_instruction(builder, op);
   if (!native)
      return false;
   native->destination.file = CHROME9_PS_DESTINATION_TEMP;
   native->destination.id = temp;
   native->destination.writemask = (1u << components) - 1;
   native->destination.saturate = alu->op == nir_op_fsat;
   for (unsigned s = 0; s < source_count; s++)
      native->source[s] = operands[s];
   builder->value[value].complete = true;
   *source = builder->value[value].source;
   return true;
}

static bool
chrome9_ps_sample_alpha(nir_scalar scalar, nir_tex_instr **texture)
{
   scalar = nir_scalar_resolved(scalar.def, scalar.comp);
   if (scalar.comp != 3 || scalar.def->num_components != 4 ||
       scalar.def->bit_size != 32 ||
       nir_def_instr(scalar.def)->type != nir_instr_type_tex)
      return false;
   *texture = nir_instr_as_tex(nir_def_instr(scalar.def));
   return true;
}

static bool
chrome9_ps_lower_alpha_terminate(struct chrome9_ps_nir_builder *builder,
                                 nir_def *condition)
{
   static const uint32_t half[4] = { 0x3f000000 };
   nir_scalar predicate, alpha, reference;
   nir_alu_instr *compare, *multiply = NULL;
   nir_tex_instr *texture;
   struct chrome9_ps_source sampled, threshold, varying;
   struct chrome9_ps_instruction *subtract, *kill;
   unsigned sampled_lane, threshold_lane;
   unsigned temp;
   bool product_factor = false;

   if (!condition || condition->num_components != 1)
      return false;
   predicate = nir_scalar_resolved(condition, 0);
   if (nir_def_instr(predicate.def)->type != nir_instr_type_alu)
      return false;
   compare = nir_instr_as_alu(nir_def_instr(predicate.def));
   if (predicate.comp || compare->op != nir_op_flt ||
       compare->src[0].src.ssa->bit_size != 32 ||
       compare->src[1].src.ssa->bit_size != 32)
      return false;
   alpha = nir_scalar_resolved(compare->src[0].src.ssa,
                               compare->src[0].swizzle[0]);
   reference = nir_scalar_resolved(compare->src[1].src.ssa,
                                   compare->src[1].swizzle[0]);
   if (!nir_scalar_is_const(reference) ||
       nir_scalar_as_uint(reference) != half[0])
      return false;
   if (!chrome9_ps_sample_alpha(alpha, &texture)) {
      nir_scalar sample, factor;

      if (nir_def_instr(alpha.def)->type != nir_instr_type_alu)
         return false;
      multiply = nir_instr_as_alu(nir_def_instr(alpha.def));
      if (multiply->op != nir_op_fmul || multiply->fp_math_ctrl ||
          alpha.comp >= multiply->def.num_components)
         return false;
      for (unsigned sampler_source = 0; sampler_source < 2;
           sampler_source++) {
         sample = nir_scalar_chase_alu_src(alpha, sampler_source);
         factor = nir_scalar_chase_alu_src(alpha, 1 - sampler_source);
         factor = nir_scalar_resolved(factor.def, factor.comp);
         if (!chrome9_ps_sample_alpha(sample, &texture) ||
             factor.def->bit_size != 32 ||
             factor.comp >= factor.def->num_components ||
             !chrome9_ps_leaf(builder, factor.def, &varying) ||
             varying.file != CHROME9_PS_SOURCE_TEXTURE || varying.id >= 8 ||
             varying.swizzle[factor.comp] > CHROME9_PS_SWIZZLE_W)
            continue;
         builder->unit_interval_input_mask |=
            1u << (varying.id * 4 + varying.swizzle[factor.comp]);
         product_factor = true;
         break;
      }
      if (!product_factor)
         return false;
   }
   if (texture->op != nir_texop_tex ||
       texture->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       texture->texture_index != 0 || texture->sampler_index != 0 ||
       texture->is_array || texture->is_shadow ||
       texture->def.num_components != 4 || texture->def.bit_size != 32 ||
       alpha.def->bit_size != 32 ||
       builder->next_temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
       !chrome9_ps_lower_def(builder, alpha.def, &sampled) ||
       !chrome9_ps_immediate(builder, half, 1, &threshold))
      return false;
   if (alpha.comp >= alpha.def->num_components ||
       sampled.swizzle[alpha.comp] > CHROME9_PS_SWIZZLE_W)
      return false;
   sampled_lane = sampled.swizzle[alpha.comp];
   threshold_lane = threshold.swizzle[0];
   for (unsigned c = 0; c < 4; c++) {
      sampled.swizzle[c] = sampled_lane;
      threshold.swizzle[c] = threshold_lane;
   }
   temp = builder->next_temp++;
   subtract = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_SUB);
   kill = chrome9_ps_append_instruction(builder, CHROME9_PS_OPCODE_KILL);
   if (!subtract || !kill)
      return false;
   subtract->destination.file = CHROME9_PS_DESTINATION_TEMP;
   subtract->destination.id = temp;
   subtract->destination.writemask = 0xf;
   subtract->source[0] = sampled;
   subtract->source[1] = threshold;
   kill->source[0].file = CHROME9_PS_SOURCE_TEMP;
   kill->source[0].id = temp;
   for (unsigned c = 0; c < 4; c++)
      kill->source[0].swizzle[c] = c;
   return true;
}

static bool
chrome9_compile_ps_nir(nir_shader *nir, struct chrome9_ps_ir *ir,
                       struct chrome9_ps_program *program,
                       uint32_t *uniform_mask,
                       uint32_t *equal_zero_input_mask,
                       uint32_t *identity_projector_input_mask,
                       uint32_t *unit_interval_input_mask,
                       bool allow_kill,
                       bool *lowered)
{
   struct chrome9_ps_nir_builder builder = { 0 };
   struct chrome9_ps_instruction *terminal;
   struct chrome9_ps_source operands[3] = { 0 };
   enum chrome9_ps_opcode op;
   unsigned source_count = 0;
   unsigned kill_count = 0;
   nir_block *block;
   nir_def *result = NULL;

   *lowered = false;
   builder.proof.budget = CHROME9_PS_PROOF_NODES;
   builder.proof.mem_ctx = nir;
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
      if (intr->intrinsic == nir_intrinsic_terminate_if) {
         /* The kernel capability admits only the qualified sampled-alpha
          * form; reject other discard predicates and sample domains. */
         if (!allow_kill || result || kill_count++ ||
             !chrome9_ps_lower_alpha_terminate(&builder, intr->src[0].ssa))
            return false;
         continue;
      }
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
      } else if (intr->intrinsic == nir_intrinsic_load_ubo ||
                 intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
         unsigned id, component;

         if (!chrome9_ps_ubo_load(intr, &id, &component))
            return false;
         builder.uniform_mask |= 1u << id;
         continue;
      } else if (intr->intrinsic ==
                    nir_intrinsic_load_barycentric_pixel ||
                 intr->intrinsic == nir_intrinsic_load_frag_coord ||
                 intr->intrinsic ==
                    nir_intrinsic_load_interpolated_input ||
                 intr->intrinsic == nir_intrinsic_load_deref) {
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
      for (unsigned s = 0; s < source_count; s++) {
         bool modifier = chrome9_ps_source_modifier_allowed(op, s);

         if (!(modifier ?
               chrome9_ps_lower_alu_source_modifier(&builder, &alu->src[s],
                                                     4, &operands[s]) :
               chrome9_ps_lower_alu_source(&builder, &alu->src[s], 4,
                                           &operands[s])))
            return false;
      }
   }
   if (!source_count) {
      op = CHROME9_PS_OPCODE_MOV;
      if (!chrome9_ps_lower_def(&builder, result, &operands[0]))
         return false;
      source_count = 1;
   }
   terminal = chrome9_ps_append_instruction(&builder, op);
   if (!terminal)
      return false;
   terminal->destination.file = CHROME9_PS_DESTINATION_COLOR;
   terminal->destination.writemask = 0xf;
   for (unsigned s = 0; s < source_count; s++)
      terminal->source[s] = operands[s];
   if (!chrome9_fragment_legalize_constant_ports(&builder.ir))
      return false;
   *ir = builder.ir;
   memset(program, 0, sizeof(*program));
   memcpy(program->immediate, builder.immediate,
          sizeof(program->immediate));
   program->immediate_mask = builder.immediate_mask;
   *uniform_mask = builder.uniform_mask;
   *equal_zero_input_mask = builder.equal_zero_input_mask;
   *identity_projector_input_mask =
      builder.identity_projector_input_mask;
   *unit_interval_input_mask = builder.unit_interval_input_mask;
   *lowered = true;
   bool compiled = chrome9_compile_ps_ir(&builder.ir, program);

   if (!compiled && debug_get_bool_option("CHROME9_DEBUG_REJECT", false))
      fprintf(stderr, "chrome9: reject native compile ir=%u temps=%u\n",
              builder.ir.count, builder.next_temp);

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
chrome9_nir_identity_source(const nir_alu_src *source, unsigned components)
{
   if (!source->src.ssa || source->src.ssa->num_components != components)
      return false;
   for (unsigned c = 0; c < components; c++)
      if (source->swizzle[c] != c)
         return false;
   return true;
}

static bool
chrome9_nir_source_is_quarter(const nir_alu_src *source,
                              unsigned components)
{
   if (!source->src.ssa)
      return false;
   for (unsigned c = 0; c < components; c++) {
      nir_scalar scalar = nir_scalar_resolved(source->src.ssa,
                                               source->swizzle[c]);

      if (!nir_scalar_is_const(scalar) ||
          nir_scalar_as_uint(scalar) != 0x3e800000)
         return false;
   }
   return true;
}

static bool
chrome9_nir_rewrite_shared_saturate_max(nir_builder *b, nir_instr *instr,
                                        void *data)
{
   (void)b;
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *outer = nir_instr_as_alu(instr);
   if (outer->op != nir_op_fsat ||
       !chrome9_nir_identity_source(&outer->src[0],
                                     outer->def.num_components) ||
       nir_def_instr(outer->src[0].src.ssa)->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *maximum = nir_instr_as_alu(
      nir_def_instr(outer->src[0].src.ssa));
   if (maximum->op != nir_op_fmax ||
       !list_is_singular(&maximum->def.uses) ||
       nir_def_used_by_if(&maximum->def))
      return false;
   for (unsigned raw_source = 0; raw_source < 2; raw_source++) {
      unsigned constant_source = 1 - raw_source;
      nir_def *raw = maximum->src[raw_source].src.ssa;
      nir_alu_instr *inner = NULL;
      unsigned uses = 0;

      if (!chrome9_nir_identity_source(&maximum->src[raw_source],
                                        maximum->def.num_components) ||
          !chrome9_nir_source_is_quarter(&maximum->src[constant_source],
                                          maximum->def.num_components) ||
          nir_def_used_by_if(raw))
         continue;
      nir_foreach_use(use, raw) {
         nir_instr *user = nir_src_use_instr(use);

         uses++;
         if (use == &maximum->src[raw_source].src ||
             user->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *candidate = nir_instr_as_alu(user);
         if (candidate->op == nir_op_fsat &&
             use == &candidate->src[0].src &&
             candidate->def.num_components == raw->num_components &&
             chrome9_nir_identity_source(&candidate->src[0],
                                          raw->num_components))
            inner = inner ? NULL : candidate;
      }
      if (uses != 2 || !inner)
         continue;
      if (inner->instr.block != maximum->instr.block ||
          !nir_instr_is_before(&inner->instr, &maximum->instr))
         continue;
      /* sat(max(x, .25)) == sat(max(sat(x), .25)).  Redirecting the
       * shared MAX input makes x single-use, so both SATs can fuse into
       * their producers without changing the finite or exceptional result. */
      nir_src_rewrite(&maximum->src[raw_source].src, &inner->def);
      return true;
   }
   return false;
}

static bool
chrome9_compile_native_fs(struct chrome9_context *ctx,
                          struct chrome9_shader *shader, nir_shader *nir)
{
   struct chrome9_fs_key key;
   enum chrome9_shader_verify_error error;
   nir_shader *semantic_nir;
   bool from_nir, lowered;

   /* The semantic backend lowers vector constructors eagerly.  Remove dead
    * lanes first so a vec4 used only through .xy cannot materialize its
    * unused POSITION.zw components in the command stream.  Keep the
    * canonical shader intact for classification and variant compilation. */
   semantic_nir = nir_shader_clone(NULL, nir);
   if (!semantic_nir)
      return false;
   nir_shader_instructions_pass(semantic_nir,
      chrome9_nir_rewrite_shared_saturate_max,
      nir_metadata_control_flow, NULL);
   nir_opt_shrink_vectors(semantic_nir, true);
   nir_opt_dce(semantic_nir);

   memset(&shader->semantic_fs_ir, 0, sizeof(shader->semantic_fs_ir));
   from_nir = chrome9_compile_ps_nir(semantic_nir, &shader->semantic_fs_ir,
                                     &shader->native_fs,
                                     &shader->semantic_uniform_mask,
                                     &shader->semantic_equal_zero_input_mask,
                                     &shader->semantic_identity_projector_input_mask,
                                     &shader->semantic_unit_interval_input_mask,
                                     ctx->fragment_kill,
                                     &lowered);
   if (lowered) {
      unsigned source_peak;

      if (!chrome9_ps_ir_temp_pressure(&shader->semantic_fs_ir,
                                       &source_peak)) {
         lowered = false;
      } else if (source_peak > 12) {
         struct chrome9_ps_ir scheduled;
         unsigned scheduled_peak;

         if (!chrome9_schedule_ps_ir(&shader->semantic_fs_ir, &scheduled,
                                     &scheduled_peak) ||
             scheduled_peak > 12)
            lowered = false;
         else
            shader->semantic_fs_ir = scheduled;
      }
   }
   ralloc_free(semantic_nir);
   shader->semantic_fs_valid = lowered;
   if (lowered) {
      for (unsigned i = 0; i < shader->semantic_fs_ir.count; i++) {
         const struct chrome9_ps_instruction *instruction =
            &shader->semantic_fs_ir.instruction[i];

         if (instruction->op == CHROME9_PS_OPCODE_TEX)
            shader->semantic_sampler_mask |= 1u << instruction->sampler;
         for (unsigned s = 0; s < 3; s++) {
            const struct chrome9_ps_source *source = &instruction->source[s];

            if (source->file == CHROME9_PS_SOURCE_CONSTANT)
               shader->semantic_constant_mask |= 1u << source->id;
            else if (source->file == CHROME9_PS_SOURCE_INPUT) {
               shader->semantic_input_mask |= 1u << source->id;
            } else if (source->file == CHROME9_PS_SOURCE_TEXTURE) {
               shader->semantic_varying_mask |= 1u << source->id;
            }
         }
      }
      /* A lowered program is usable only if every instruction can be
       * represented by the DRM UAPI.  Reject it at compile time, before
       * any draw can attempt to submit a partially translated program. */
      struct drm_chrome9_fragment_instruction
         instructions[DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS];
      unsigned instruction_count;

      if (!chrome9_fragment_build_instructions(
             &shader->semantic_fs_ir, shader->semantic_sampler_mask,
             instructions, &instruction_count))
         shader->semantic_fs_valid = false;
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
       shader->semantic_fs_ir.count != 1 ||
       shader->native_fs.alu_count != 1 || shader->native_fs.tau_count ||
       shader->native_fs.sampler_mask ||
       (shader->native_fs.varying_mask & ~1u) ||
       (shader->native_fs.constant_mask & ~1u))
      return false;
   instruction = &shader->semantic_fs_ir.instruction[0];
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

/* MOV oC0, primary: passthrough programs the semantic lowering skips. */
static void
chrome9_semantic_primary_passthrough(struct chrome9_shader *shader)
{
   struct chrome9_ps_instruction *mov = &shader->semantic_fs_ir.instruction[0];

   memset(&shader->semantic_fs_ir, 0, sizeof(shader->semantic_fs_ir));
   mov->op = CHROME9_PS_OPCODE_MOV;
   mov->destination.file = CHROME9_PS_DESTINATION_COLOR;
   mov->destination.writemask = 0xf;
   mov->source[0].file = CHROME9_PS_SOURCE_INPUT;
   for (unsigned i = 0; i < 4; i++)
      mov->source[0].swizzle[i] = i;
   shader->semantic_fs_ir.count = 1;
   shader->semantic_input_mask = 1;
   shader->semantic_fs_valid = true;
   shader->semantic_passthrough = true;
}

void
chrome9_compile_fs_nir(struct chrome9_context *ctx,
                       struct chrome9_shader *shader, nir_shader *nir)
{
   bool straight_line = chrome9_fs_straight_line_entrypoint(nir, NULL, NULL);

   shader->fragment_program = straight_line ?
      chrome9_fs_classify(nir, &shader->literal_color) :
      CHROME9_FS_UNSUPPORTED;
   shader->flat_primary =
      shader->fragment_program == CHROME9_FS_PRIMARY_COLOR &&
      chrome9_fs_has_flat_primary(nir);
   shader->flat_generic = shader->fragment_program ==
      CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX;
   shader->native_fs_valid = straight_line &&
      chrome9_compile_native_fs(ctx, shader, nir);
   /* Flat colors arrive per vertex already (draw module flat stage or
    * the hardware flat mode), so the program is a plain primary MOV. */
   if (!shader->semantic_fs_valid && shader->flat_primary)
      chrome9_semantic_primary_passthrough(shader);
   if (shader->semantic_fs_valid && ctx->draw_fragment_program &&
       (!shader->semantic_input_mask ||
        (ctx->fragment_primary && shader->semantic_input_mask == 1)) &&
       shader->fragment_program == CHROME9_FS_UNSUPPORTED)
      shader->fragment_program = CHROME9_FS_COMPILED_ALU;
   if (shader->fragment_program == CHROME9_FS_UNSUPPORTED &&
       ctx->draw_program_triangle_d16_rhw_batch &&
       chrome9_native_fs_is_program_batch(shader, ctx->fragment_add))
      shader->fragment_program = CHROME9_FS_COMPILED_ALU;
   shader->supported = shader->fragment_program != CHROME9_FS_UNSUPPORTED;
}

static void
chrome9_capture_fs_variant(struct chrome9_fs_variant *variant,
                           const struct chrome9_shader *shader)
{
   variant->fragment_program = shader->fragment_program;
   variant->literal_color = shader->literal_color;
   variant->native_fs_key = shader->native_fs_key;
   variant->semantic_fs_ir = shader->semantic_fs_ir;
   variant->native_fs = shader->native_fs;
   variant->semantic_constant_mask = shader->semantic_constant_mask;
   variant->semantic_uniform_mask = shader->semantic_uniform_mask;
   variant->semantic_input_mask = shader->semantic_input_mask;
   variant->semantic_varying_mask = shader->semantic_varying_mask;
   variant->semantic_sampler_mask = shader->semantic_sampler_mask;
   variant->semantic_equal_zero_input_mask =
      shader->semantic_equal_zero_input_mask;
   variant->semantic_identity_projector_input_mask =
      shader->semantic_identity_projector_input_mask;
   variant->semantic_unit_interval_input_mask =
      shader->semantic_unit_interval_input_mask;
   variant->semantic_fs_valid = shader->semantic_fs_valid;
   variant->semantic_passthrough = shader->semantic_passthrough;
   variant->native_fs_valid = shader->native_fs_valid;
   variant->native_fs_from_nir = shader->native_fs_from_nir;
   variant->flat_primary = shader->flat_primary;
   variant->flat_generic = shader->flat_generic;
   variant->supported = shader->supported;
}

static void
chrome9_apply_fs_variant(struct chrome9_shader *shader,
                         const struct chrome9_fs_variant *variant)
{
   shader->fragment_program = variant->fragment_program;
   shader->literal_color = variant->literal_color;
   shader->native_fs_key = variant->native_fs_key;
   shader->semantic_fs_ir = variant->semantic_fs_ir;
   shader->native_fs = variant->native_fs;
   shader->semantic_constant_mask = variant->semantic_constant_mask;
   shader->semantic_uniform_mask = variant->semantic_uniform_mask;
   shader->semantic_input_mask = variant->semantic_input_mask;
   shader->semantic_varying_mask = variant->semantic_varying_mask;
   shader->semantic_sampler_mask = variant->semantic_sampler_mask;
   shader->semantic_equal_zero_input_mask =
      variant->semantic_equal_zero_input_mask;
   shader->semantic_identity_projector_input_mask =
      variant->semantic_identity_projector_input_mask;
   shader->semantic_unit_interval_input_mask =
      variant->semantic_unit_interval_input_mask;
   shader->semantic_fs_valid = variant->semantic_fs_valid;
   shader->semantic_passthrough = variant->semantic_passthrough;
   shader->native_fs_valid = variant->native_fs_valid;
   shader->native_fs_from_nir = variant->native_fs_from_nir;
   shader->flat_primary = variant->flat_primary;
   shader->flat_generic = variant->flat_generic;
   shader->supported = variant->supported;
}

#define CHROME9_MAX_FS_VARIANTS 64

static void
chrome9_dump_semantic_fs(const struct chrome9_shader *shader)
{
   const struct chrome9_ps_ir *ir = &shader->semantic_fs_ir;

   if (!debug_get_bool_option("CHROME9_DUMP_SEMANTIC_FS", false) ||
       !shader->semantic_fs_valid)
      return;
   fprintf(stderr, "chrome9: semantic FS count=%u constants=%08x "
           "uniforms=%08x immediates=%08x\n", ir->count,
           shader->semantic_constant_mask, shader->semantic_uniform_mask,
           shader->native_fs.immediate_mask);
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &ir->instruction[i];

      fprintf(stderr, "chrome9: semantic %u op=%u dst=%u.%u/%x", i,
              instruction->op, instruction->destination.file,
              instruction->destination.id,
              instruction->destination.writemask);
      for (unsigned s = 0; s < 3; s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];

         fprintf(stderr, " s%u=%u.%u/%u%u%u%u", s, source->file,
                 source->id, source->swizzle[0], source->swizzle[1],
                 source->swizzle[2], source->swizzle[3]);
      }
      fputc('\n', stderr);
   }
}

/* Choose a fragment program only when all control-flow uniforms have their
 * draw-time values.  Failed variants are cached too, so an unsupported bound
 * is deterministic and never leaves a partially compiled program active. */
static bool
chrome9_select_fs_variant(struct chrome9_context *ctx)
{
   struct chrome9_shader *shader = ctx->fs;
   nir_shader *canonical;
   unsigned count;
   uint8_t face_mode = 0;
   size_t bytes;

   if (!shader || !shader->nir)
      return shader && shader->supported;
   /* A dynamic canonical program needs no variants.  In particular, a
    * natively lowered select must not exhaust the bounded variant cache as
    * its control uniform takes on many different values. */
   if (shader->canonical_supported)
      return true;
   canonical = shader->nir;
   count = canonical->info.num_inlinable_uniforms;
   if (ctx->rasterizer) {
      if (ctx->rasterizer->cull_face == PIPE_FACE_BACK)
         face_mode = 1;
      else if (ctx->rasterizer->cull_face == PIPE_FACE_FRONT)
         face_mode = 2;
   }
   if (!count && !face_mode) {
      /* A previous cull-specialized variant must not survive a transition
       * back to dynamic FACE with no uniform specialization key. */
      shader->supported = false;
      return false;
   }
   if (count && ctx->fs_inline_count != count) {
      shader->supported = false;
      return false;
   }
   bytes = count * sizeof(ctx->fs_inline[0]);

   for (struct chrome9_fs_variant *variant = shader->variants;
        variant; variant = variant->next) {
      if (variant->face_mode == face_mode &&
          !memcmp(variant->inline_values, ctx->fs_inline, bytes)) {
         chrome9_apply_fs_variant(shader, variant);
         return shader->supported;
      }
   }
   if (shader->variant_count >= CHROME9_MAX_FS_VARIANTS) {
      shader->supported = false;
      return false;
   }

   struct chrome9_fs_variant *variant = CALLOC_STRUCT(chrome9_fs_variant);
   nir_shader *clone = variant ? nir_shader_clone(NULL, canonical) : NULL;

   if (!clone) {
      FREE(variant);
      shader->supported = false;
      return false;
   }
   struct chrome9_shader compiled = { 0 };

   variant->face_mode = face_mode;
   memcpy(variant->inline_values, ctx->fs_inline, bytes);
   if (count)
      chrome9_nir_inline_uniforms(clone, variant->inline_values);
   if (face_mode)
      chrome9_nir_specialize_front_face(clone, face_mode == 1);
   chrome9_compile_fs_nir(ctx, &compiled, clone);
   chrome9_capture_fs_variant(variant, &compiled);
   variant->next = shader->variants;
   shader->variants = variant;
   shader->variant_count++;
   chrome9_apply_fs_variant(shader, variant);
   chrome9_dump_semantic_fs(shader);

   if (debug_get_bool_option("CHROME9_DEBUG_SHADERS", false)) {
      fprintf(stderr, "chrome9: FS variant %u face=%u",
              shader->variant_count, face_mode);
      for (unsigned i = 0; i < count; i++)
         fprintf(stderr, " dw%u=%08x",
                 canonical->info.inlinable_uniform_dw_offsets[i],
                 variant->inline_values[i]);
      fprintf(stderr, " supported=%u ir=%u alu=%u tau=%u\n",
              shader->supported, shader->semantic_fs_ir.count,
              shader->native_fs.alu_count, shader->native_fs.tau_count);
   }
   if (!shader->supported &&
       debug_get_bool_option("CHROME9_DUMP_UNSUPPORTED_NIR", false)) {
      fprintf(stderr, "chrome9: unsupported specialized FS %p\n",
              (void *)shader);
      nir_print_shader(clone, stderr);
   }
   ralloc_free(clone);
   return shader->supported;
}

/* CHROME9_DEBUG_FS_MARK=lo[-hi] replaces the color output of the fragment
 * shaders created in that index range by magenta; "id" paints every shader
 * with its index as R=bits 2:0, G=5:3, B=7:6, to locate a material. */
static void
chrome9_debug_mark_fs(nir_shader *nir, unsigned index)
{
   const char *range = getenv("CHROME9_DEBUG_FS_MARK");
   float color[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
   unsigned lo, hi;

   if (range && !strcmp(range, "id")) {
      lo = 0;
      hi = UINT_MAX;
      color[0] = (index & 7) / 7.0f;
      color[1] = ((index >> 3) & 7) / 7.0f;
      color[2] = ((index >> 6) & 3) / 3.0f;
   } else if (!range || sscanf(range, "%u-%u", &lo, &hi) < 1) {
      return;
   } else if (!strchr(range, '-')) {
      hi = lo;
   }
   fprintf(stderr, "chrome9: FS index %u%s\n", index,
           index >= lo && index <= hi ? " marked" : "");
   if (index < lo || index > hi)
      return;
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            unsigned value;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            intr = nir_instr_as_intrinsic(instr);
            value = intr->intrinsic == nir_intrinsic_store_deref;

            if ((intr->intrinsic != nir_intrinsic_store_output &&
                 (!value || !nir_deref_mode_is(nir_src_as_deref(intr->src[0]),
                                               nir_var_shader_out))) ||
                intr->src[value].ssa->num_components != 4)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&intr->src[value],
                            nir_imm_vec4(&b, color[0], color[1], color[2],
                                         color[3]));
         }
      }
      nir_opt_dce(nir);
   }
}

static void *
chrome9_create_fs_state(struct pipe_context *pipe,
                        const struct pipe_shader_state *state)
{
   static unsigned fs_index;
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_shader *shader = CALLOC_STRUCT(chrome9_shader);

   if (!shader)
      return NULL;
   if (state->type == PIPE_SHADER_IR_NIR && state->ir.nir)
      chrome9_debug_mark_fs(state->ir.nir, fs_index);
   fs_index++;
   shader->draw = draw_create_fragment_shader(ctx->draw, state);
   if (!shader->draw) {
      FREE(shader);
      return NULL;
   }
   if (state->type == PIPE_SHADER_IR_NIR && state->ir.nir) {
      shader->nir = state->ir.nir;
      if (debug_get_bool_option("CHROME9_DUMP_NIR", false))
         nir_print_shader(state->ir.nir, stderr);
      chrome9_compile_fs_nir(ctx, shader, state->ir.nir);
      shader->canonical_supported = shader->supported;
      chrome9_dump_semantic_fs(shader);
      if (!state->ir.nir->info.num_inlinable_uniforms && !shader->supported &&
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
                 shader->semantic_fs_ir.count,
                 __builtin_popcount(shader->semantic_sampler_mask),
                 shader->native_fs.alu_count,
                 shader->native_fs.tau_count,
                 shader->native_fs.constant_mask,
                 shader->native_fs.immediate_mask);
   } else if (state->type == PIPE_SHADER_IR_TGSI) {
      shader->supported = chrome9_tgsi_fs_passthrough(state->tokens);
      shader->fragment_program = shader->supported ?
         CHROME9_FS_PRIMARY_COLOR : CHROME9_FS_UNSUPPORTED;
      /* The same MOV oC0, primary the NIR path lowers, for generic draws. */
      if (shader->supported)
         chrome9_semantic_primary_passthrough(shader);
   }
   if (!shader->nir && shader->supported && !shader->native_fs_valid)
      shader->native_fs_valid = chrome9_compile_native_fs(
         ctx, shader, state->type == PIPE_SHADER_IR_NIR ?
                      state->ir.nir : NULL);
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
   chrome9_sw_forget(ctx, shader);
   if (ctx->fs == shader) {
      ctx->fs = NULL;
      draw_bind_fragment_shader(ctx->draw, NULL);
   }
   draw_delete_fragment_shader(ctx->draw, shader->draw);
   while (shader->variants) {
      struct chrome9_fs_variant *variant = shader->variants;

      shader->variants = variant->next;
      FREE(variant);
   }
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

   chrome9_sw_forget(ctx, state);
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

static void
chrome9_set_inlinable_constants(struct pipe_context *pipe,
                                mesa_shader_stage shader,
                                unsigned num_values, uint32_t *values)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   unsigned count = MIN2(num_values, MAX_INLINABLE_UNIFORMS);

   if (shader == MESA_SHADER_FRAGMENT) {
      memset(ctx->fs_inline, 0, sizeof(ctx->fs_inline));
      if (count)
         memcpy(ctx->fs_inline, values, count * sizeof(*values));
      ctx->fs_inline_count = count;
   }
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
                    chrome9_texture_format_info(templ->format) &&
                    !templ->u.tex.first_level &&
                    templ->u.tex.last_level <= texture->last_level &&
                    !templ->u.tex.first_layer &&
                    !templ->u.tex.last_layer &&
                    chrome9_texture_swizzle_supported(
                       templ->format, templ->swizzle_r, templ->swizzle_g,
                       templ->swizzle_b, templ->swizzle_a);

   if (supported && !chrome9_resource(texture)->guarded) {
      struct pipe_resource shadow_templ = *texture;

      /* Imported images expose only their level-zero plane layout. */
      if (templ->u.tex.last_level) {
         supported = false;
      } else {
         shadow_templ.last_level = 0;
         shadow_templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
         shadow_templ.flags = 0;
         cview->shadow = pipe->screen->resource_create(pipe->screen,
                                                        &shadow_templ);
         supported = cview->shadow != NULL;
      }
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

   if (shader == MESA_SHADER_VERTEX) {
      struct pipe_sampler_view *new_views[CHROME9_VS_MAX_SAMPLERS];
      unsigned num_views = 0;

      if (start >= CHROME9_VS_MAX_SAMPLERS)
         return;
      memcpy(new_views, ctx->vs_sampler_views, sizeof(new_views));
      end = MIN2(start + count, CHROME9_VS_MAX_SAMPLERS);
      for (unsigned i = start; i < end; i++)
         new_views[i] = views ? views[i - start] : NULL;
      end = MIN2(end + unbind_trailing, CHROME9_VS_MAX_SAMPLERS);
      for (unsigned i = start + count; i < end; i++)
         new_views[i] = NULL;
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
         if (new_views[i])
            num_views = i + 1;
      /* draw_set_sampler_views flushes old draw state.  Keep old views alive
       * until after that flush before replacing our references. */
      draw_set_sampler_views(ctx->draw, MESA_SHADER_VERTEX,
                             new_views, num_views);
      for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++) {
         if (new_views[i] != ctx->vs_sampler_views[i])
            pipe_sampler_view_reference(&ctx->vs_sampler_views[i],
                                        new_views[i]);
         if (!new_views[i])
            memset(&ctx->vs_texture_mappings[i], 0,
                   sizeof(ctx->vs_texture_mappings[i]));
      }
      ctx->num_vs_sampler_views = num_views;
      return;
   }
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
   ctx->render_target_dirty = false;
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
   if (!framebuffer->nr_cbufs && framebuffer->zsbuf.texture &&
       chrome9_format_is_d24(framebuffer->zsbuf.format) &&
       framebuffer->zsbuf.texture->format == framebuffer->zsbuf.format &&
       !framebuffer->zsbuf.level && !framebuffer->zsbuf.first_layer &&
       !framebuffer->zsbuf.last_layer && framebuffer->width &&
       framebuffer->height &&
       framebuffer->width == framebuffer->zsbuf.texture->width0 &&
       framebuffer->height == framebuffer->zsbuf.texture->height0 &&
       chrome9_resource(framebuffer->zsbuf.texture)->guarded) {
      struct pipe_resource templ = {
         .target = PIPE_TEXTURE_2D,
         .format = PIPE_FORMAT_B8G8R8X8_UNORM,
         .width0 = framebuffer->width,
         .height0 = framebuffer->height,
         .depth0 = 1,
         .array_size = 1,
         .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
      };

      if (ctx->depth_only_scratch &&
          (ctx->depth_only_scratch->width0 != framebuffer->width ||
           ctx->depth_only_scratch->height0 != framebuffer->height))
         pipe_resource_reference(&ctx->depth_only_scratch, NULL);
      if (!ctx->depth_only_scratch)
         ctx->depth_only_scratch =
            pipe->screen->resource_create(pipe->screen, &templ);
      if (ctx->depth_only_scratch)
         pipe_resource_reference(&ctx->render_target,
                                 ctx->depth_only_scratch);
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
   if (!start && count)
      chrome9_context(pipe)->viewport = viewports[0];
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
   chrome9_context(pipe)->clip = *clip;
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
        target->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        target->base.format != PIPE_FORMAT_B5G6R5_UNORM) ||
       x >= target->base.width0 || y >= target->base.height0)
      return;
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   if (target->base.format == PIPE_FORMAT_B5G6R5_UNORM) {
      uint16_t word;

      if (!chrome9_pack_rgb565_clear(color->f, &word))
         return;
      chrome9_fill_rgb565_rect((uint8_t *)target->bo.map + target->data_offset,
                               target->stride, x, y, width, height, word);
      return;
   } else {
      util_pack_color_union(target->base.format, &packed, color);
   }
   util_fill_rect((uint8_t *)target->bo.map + target->data_offset,
                  target->base.format, target->stride, x, y, width, height,
                  &packed);
}

static void
chrome9_clear_zs_target(struct chrome9_resource *target, unsigned buffers,
                        double depth, unsigned stencil, uint8_t stencil_mask,
                        unsigned x, unsigned y, unsigned width,
                        unsigned height)
{
   uint32_t value = 0, keep = 0xffffffff;

   if (!target || x >= target->base.width0 || y >= target->base.height0)
      return;
   if (buffers & PIPE_CLEAR_DEPTH) {
      if (!chrome9_pack_depth_clear(target->base.format, depth, &value))
         return;
      /* X8Z24 has no stencil: a depth clear defines the whole word. */
      keep = target->base.format == PIPE_FORMAT_S8_UINT_Z24_UNORM ?
         0xff : 0;
   }
   if ((buffers & PIPE_CLEAR_STENCIL) &&
       target->base.format == PIPE_FORMAT_S8_UINT_Z24_UNORM) {
      keep &= ~(uint32_t)stencil_mask;
      value |= stencil & stencil_mask;
   }
   if (keep == 0xffffffff)
      return;
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   for (unsigned row = 0; row < height; row++) {
      uint8_t *pixels = (uint8_t *)target->bo.map + target->data_offset +
                        (uint64_t)(y + row) * target->stride;

      if (target->base.format == PIPE_FORMAT_Z16_UNORM) {
         uint16_t *z16 = (uint16_t *)pixels + x;

         for (unsigned column = 0; column < width; column++)
            z16[column] = (uint16_t)value;
      } else {
         uint32_t *d24 = (uint32_t *)pixels + x;

         for (unsigned column = 0; column < width; column++)
            d24[column] = (d24[column] & keep) | value;
      }
   }
}

/* Per-channel masked clear of a 32-bpp BGRA/BGRX target. */
static void
chrome9_clear_target_masked(struct chrome9_resource *target,
                            const union pipe_color_union *color,
                            unsigned color_mask, unsigned x, unsigned y,
                            unsigned width, unsigned height)
{
   union util_color packed;
   uint32_t keep = 0;

   if (!target || x >= target->base.width0 || y >= target->base.height0 ||
       (target->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        target->base.format != PIPE_FORMAT_B8G8R8X8_UNORM))
      return;
   util_pack_color_union(target->base.format, &packed, color);
   /* BGRA8888 stores R in bits 23:16, G 15:8, B 7:0 and A 31:24. */
   if (!(color_mask & PIPE_MASK_R))
      keep |= 0x00ff0000;
   if (!(color_mask & PIPE_MASK_G))
      keep |= 0x0000ff00;
   if (!(color_mask & PIPE_MASK_B))
      keep |= 0x000000ff;
   if (!(color_mask & PIPE_MASK_A))
      keep |= 0xff000000;
   width = MIN2(width, target->base.width0 - x);
   height = MIN2(height, target->base.height0 - y);
   for (unsigned row = 0; row < height; row++) {
      uint32_t *pixels = (uint32_t *)((uint8_t *)target->bo.map +
         target->data_offset + (uint64_t)(y + row) * target->stride) + x;

      for (unsigned column = 0; column < width; column++)
         pixels[column] = (pixels[column] & keep) | (packed.ui[0] & ~keep);
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

   chrome9_draw_flush_batch(ctx);
   if (!(buffers & (PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTHSTENCIL)))
      return;
   if (buffers & ~(PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTHSTENCIL)) {
      chrome9_warn_once(ctx, CHROME9_WARN_CLEAR,
                        "clears of additional color buffers are not implemented");
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
   if ((buffers & PIPE_CLEAR_COLOR0) && ctx->framebuffer.nr_cbufs &&
       (resource = ctx->render_target)) {
      chrome9_context_prepare_render_target(ctx);
      if ((color_mask & 0xf) == 0xf)
         chrome9_clear_target(chrome9_resource(resource), color, x, y, width,
                              height);
      else
         chrome9_clear_target_masked(chrome9_resource(resource), color,
                                     color_mask, x, y, width, height);
      chrome9_context_render_target_written(ctx);
      /* Imported drawable targets render through a guarded shadow.  Clears
       * are synchronous CPU writes, so publish the completed image now even
       * when the state tracker keeps this framebuffer bound. */
      chrome9_flush_render_target(ctx);
   }
   if (buffers & PIPE_CLEAR_DEPTHSTENCIL)
      chrome9_clear_zs_target(chrome9_context_depth_target(ctx), buffers,
                              depth, stencil, stencil_mask,
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

   (void)render_condition;
   chrome9_draw_flush_batch(ctx);
   if (!surface || !surface->texture ||
       !(clear_flags & PIPE_CLEAR_DEPTHSTENCIL)) {
      chrome9_warn_once(ctx, CHROME9_WARN_CLEAR,
                        "unsupported depth/stencil clear");
      return;
   }
   chrome9_clear_zs_target(chrome9_resource(surface->texture), clear_flags,
                           depth, stencil, 0xff, x, y, width, height);
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
chrome9_refresh_vs_textures(struct chrome9_context *ctx)
{
   static const uint8_t zero_texel[16];
   uint32_t zero_stride[PIPE_MAX_TEXTURE_LEVELS] = {0};
   struct pipe_resource *external = ctx->framebuffer.nr_cbufs ?
      ctx->framebuffer.cbufs[0].texture : NULL;

   if (!ctx->num_vs_sampler_views)
      return;
   /* Vertex fetch happens on the CPU.  Retire any preceding GPU writes
    * before publishing the BO's persistent mapping to the draw module. */
   chrome9_draw_flush_batch(ctx);
   for (unsigned i = 0; i < ctx->num_vs_sampler_views; i++) {
      struct pipe_sampler_view *view = ctx->vs_sampler_views[i];
      struct chrome9_vs_texture_mapping *mapping =
         &ctx->vs_texture_mappings[i];
      struct pipe_sampler_view effective;

      if (view && view->texture == external)
         chrome9_flush_render_target(ctx);
      if (view && chrome9_sampler_view(view)->shadow) {
         chrome9_copy_surface(chrome9_sampler_view(view)->shadow,
                              view->texture);
         effective = *view;
         effective.texture = chrome9_sampler_view(view)->shadow;
         view = &effective;
      }
      if (chrome9_vs_texture_mapping(view, mapping)) {
         draw_set_mapped_texture(ctx->draw, MESA_SHADER_VERTEX, i,
                                 mapping->width, mapping->height, 1,
                                 mapping->first_level, mapping->last_level,
                                 0, 0, mapping->base, mapping->row_stride,
                                 mapping->img_stride, mapping->mip_offset);
      } else {
         draw_set_mapped_texture(ctx->draw, MESA_SHADER_VERTEX, i,
                                 1, 1, 1, 0, 0, 0, 0, zero_texel,
                                 zero_stride, zero_stride, zero_stride);
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
   if (!ctx->vs || !ctx->fs || !chrome9_select_fs_variant(ctx)) {
      /* Beyond the pixel engine: llvmpipe renders into the same BOs. */
      if (chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
         return;
      chrome9_warn_once(ctx, CHROME9_WARN_DRAW_STATE,
                        "draw rejected: unsupported bounded fragment program");
      CHROME9_REJECT(ctx, draws[0].count);
      return;
   }

   /* Vertex texturing runs in the interpreter on the draw path; llvmpipe
    * compiles it instead. */
   if (ctx->vs->sw_nir &&
       BITSET_COUNT(ctx->vs->sw_nir->info.textures_used) &&
       chrome9_sw_draw_vbo(ctx, info, drawid_offset, draws, num_draws))
      return;
   chrome9_refresh_vs_textures(ctx);
   /* No GPU job may start while llvmpipe still accesses a BO. */
   chrome9_sw_finish(ctx);
   if (chrome9_tcl_draw(ctx, info, &draws[0]))
      return;

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
   const bool guarded_domain =
      ctx->fs->semantic_equal_zero_input_mask != 0 ||
      ctx->fs->semantic_identity_projector_input_mask != 0 ||
      ctx->fs->semantic_unit_interval_input_mask != 0;

   if (guarded_domain)
      chrome9_draw_begin_guarded_domain(ctx);
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(ctx->draw);
   if (guarded_domain)
      chrome9_draw_end_guarded_domain(ctx);
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
   chrome9_report_rejects(ctx);
   if (ctx->tcl_enabled && !(++ctx->tcl_flushes % 16) &&
       debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL triangles=%" PRIu64 " fallback draws=%"
                      PRIu64 " all triangles=%" PRIu64 "\n",
              ctx->tcl_triangles, ctx->tcl_fallback_draws,
              ctx->submitted_triangles);
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
   chrome9_context(pipe)->blend_color = *color;
}

static void
chrome9_set_stencil_ref(struct pipe_context *pipe,
                        const struct pipe_stencil_ref reference)
{
   chrome9_context(pipe)->stencil_ref = reference;
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
   chrome9_report_rejects(ctx);
   if (ctx->tcl_enabled && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL triangles=%" PRIu64 " fallback draws=%"
                      PRIu64 " all triangles=%" PRIu64 "\n",
              ctx->tcl_triangles, ctx->tcl_fallback_draws,
              ctx->submitted_triangles);
   chrome9_sw_context_destroy(ctx);
   if (ctx->reject_sites)
      _mesa_hash_table_destroy(ctx->reject_sites, NULL);
   ctx->reject_sites = NULL;
   if (ctx->draw)
      draw_destroy(ctx->draw);
   if (pipe->stream_uploader)
      u_upload_destroy(pipe->stream_uploader);
   util_unreference_framebuffer_state(&ctx->framebuffer);
   pipe_resource_reference(&ctx->render_target, NULL);
   pipe_resource_reference(&ctx->depth_only_scratch, NULL);
   util_set_vertex_buffers_count(ctx->vertex_buffers,
                                 &ctx->num_vertex_buffers, NULL, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      util_copy_constant_buffer(&ctx->vs_constants[i], NULL);
      util_copy_constant_buffer(&ctx->fs_constants[i], NULL);
   }
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   for (unsigned i = 0; i < CHROME9_VS_MAX_SAMPLERS; i++)
      pipe_sampler_view_reference(&ctx->vs_sampler_views[i], NULL);
   chrome9_vs_texture_sampler_destroy(ctx->vs_tgsi_sampler);
   FREE(ctx->tcl_state);
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
   ctx->fragment_sparse_textures =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_SPARSE_TEXTURES);
   ctx->tcl_enabled = (capabilities & DRM_CHROME9_CAP_VERTEX_PROGRAM) &&
      debug_get_bool_option("CHROME9_TCL", false);
   ctx->fragment_primary =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_PRIMARY);
   ctx->fragment_kill =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_KILL);
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
   ctx->fragment_stencil =
      !!(capabilities & DRM_CHROME9_CAP_FRAGMENT_STENCIL);
   ctx->draw_limit = MAX2(debug_get_num_option("CHROME9_DRAW_LIMIT", 0), 0);
   ctx->debug_draws = debug_get_bool_option("CHROME9_DEBUG_DRAWS", false);

   ctx->base.screen = screen;
   ctx->base.priv = priv;
   if (debug_get_bool_option("CHROME9_DEBUG_REJECT", false))
      ctx->reject_sites = _mesa_pointer_hash_table_create(NULL);
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
   ctx->base.set_inlinable_constants = chrome9_set_inlinable_constants;
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

   /* The vertex sampler is a TGSI-exec callback.  LLVM draw shaders use
    * their own sampler ABI and cannot safely share that callback. */
   ctx->draw = draw_create_no_llvm(&ctx->base);
   if (!ctx->draw)
      goto fail;
   ctx->vs_tgsi_sampler = chrome9_vs_texture_sampler_create(ctx);
   if (!ctx->vs_tgsi_sampler)
      goto fail;
   draw_texture_sampler(ctx->draw, MESA_SHADER_VERTEX,
                        ctx->vs_tgsi_sampler);
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
