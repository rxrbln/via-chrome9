/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_tex_lower.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "pipe/p_state.h"
#include "util/macros.h"

static bool
chrome9_is_derivative(nir_scalar s, bool y_axis)
{
   if (!nir_scalar_is_intrinsic(s))
      return false;
   switch (nir_scalar_intrinsic_op(s)) {
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddx_coarse:
      return !y_axis;
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddy_coarse:
      return y_axis;
   default:
      return false;
   }
}

/* Derivative operand of the scalar s = ddx(v) or ddy(v). */
static nir_scalar
chrome9_derivative_operand(nir_scalar s)
{
   nir_intrinsic_instr *intr = nir_def_as_intrinsic(s.def);

   return nir_scalar_resolved(intr->src[0].ssa, s.comp);
}

/*
 * Without native GL_CLAMP the state tracker samples txd(fsat(c), ddx(c),
 * ddy(c)).  The sampler has no TEXLDD; its implicit derivatives of fsat(c)
 * differ from ddx(c) only where c is clamped to the edge, so this selects
 * another LOD only there (docs/research/chrome9-texture-border-re.md).
 */
static bool
chrome9_lower_saturated_txd(nir_builder *b, nir_tex_instr *tex, void *data)
{
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *ddx = nir_get_tex_src(tex, nir_tex_src_ddx);
   nir_def *ddy = nir_get_tex_src(tex, nir_tex_src_ddy);

   (void)b;
   (void)data;
   if (tex->op != nir_texop_txd || !coord || !ddx || !ddy ||
       nir_get_tex_src(tex, nir_tex_src_projector) ||
       ddx->num_components != ddy->num_components ||
       ddx->num_components > coord->num_components)
      return false;
   for (unsigned i = 0; i < ddx->num_components; i++) {
      nir_scalar c = nir_scalar_resolved(coord, i);
      nir_scalar dx = nir_scalar_resolved(ddx, i);
      nir_scalar dy = nir_scalar_resolved(ddy, i);

      if (nir_scalar_is_alu(c) && nir_scalar_alu_op(c) == nir_op_fsat) {
         c = nir_scalar_chase_alu_src(c, 0);
         c = nir_scalar_resolved(c.def, c.comp);
      }
      if (!chrome9_is_derivative(dx, false) ||
          !chrome9_is_derivative(dy, true) ||
          !nir_scalar_equal(c, chrome9_derivative_operand(dx)) ||
          !nir_scalar_equal(c, chrome9_derivative_operand(dy)))
         return false;
   }
   nir_tex_instr_remove_src(tex, nir_tex_instr_src_index(tex,
                                                         nir_tex_src_ddx));
   nir_tex_instr_remove_src(tex, nir_tex_instr_src_index(tex,
                                                         nir_tex_src_ddy));
   tex->op = nir_texop_tex;
   return true;
}

bool
chrome9_nir_needs_hw_texture_lowering(const nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_tex_instr *tex;

            if (instr->type != nir_instr_type_tex)
               continue;
            tex = nir_instr_as_tex(instr);
            if (tex->sampler_dim == GLSL_SAMPLER_DIM_1D ||
                tex->op == nir_texop_txd)
               return true;
         }
      }
   }
   return false;
}

bool
chrome9_nir_lower_hw_textures(nir_shader *nir)
{
   const nir_lower_tex_options options = {
      .lower_1d = true,
   };
   bool progress = false;

   NIR_PASS(progress, nir, nir_lower_tex, &options);
   NIR_PASS(progress, nir, nir_shader_tex_pass, chrome9_lower_saturated_txd,
            nir_metadata_control_flow, NULL);
   if (progress)
      NIR_PASS(_, nir, nir_opt_dce);
   return progress;
}

uint32_t
chrome9_nir_shadow_sampler_mask(const nir_shader *nir)
{
   uint32_t mask = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_tex_instr *tex;

            if (instr->type != nir_instr_type_tex)
               continue;
            tex = nir_instr_as_tex(instr);
            if (tex->is_shadow && tex->texture_index < 32)
               mask |= 1u << tex->texture_index;
         }
      }
   }
   return mask;
}

/* Bits 2:0 compare function, 14:3 swizzle, 15 set for a valid key. */
uint16_t
chrome9_shadow_key(const struct pipe_sampler_state *sampler,
                   const struct pipe_sampler_view *view)
{
   /* GL leaves shadow sampling without compare mode undefined. */
   unsigned func = sampler &&
                   sampler->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE ?
                   sampler->compare_func : PIPE_FUNC_ALWAYS;
   unsigned swizzle[4] = {
      PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W,
   };

   if (view) {
      swizzle[0] = view->swizzle_r;
      swizzle[1] = view->swizzle_g;
      swizzle[2] = view->swizzle_b;
      swizzle[3] = view->swizzle_a;
   }
   return 1u << 15 | (func & 7) | (swizzle[0] & 7) << 3 |
          (swizzle[1] & 7) << 6 | (swizzle[2] & 7) << 9 |
          (swizzle[3] & 7) << 12;
}

/*
 * The sampler returns D24 as raw / 2^24, GL defines raw / (2^24 - 1)
 * (chrome9-shadow-compare-re.md).  Project and clamp the reference like GL,
 * then scale it onto the sampler's grid so equal depths compare equal.
 */
static bool
chrome9_prepare_shadow_reference(nir_builder *b, nir_tex_instr *tex,
                                 void *data)
{
   nir_def *coord, *ref, *proj;

   (void)data;
   if (!tex->is_shadow ||
       (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
        tex->op != nir_texop_txl && tex->op != nir_texop_txd))
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   ref = nir_steal_tex_src(tex, nir_tex_src_comparator);
   if (!ref)
      return false;
   proj = nir_steal_tex_src(tex, nir_tex_src_projector);
   if (proj) {
      nir_def *rcp = nir_frcp(b, proj);
      nir_def *c[4];

      coord = nir_steal_tex_src(tex, nir_tex_src_coord);
      for (unsigned i = 0; i < coord->num_components; i++) {
         c[i] = nir_channel(b, coord, i);
         if (!tex->is_array || i + 1 < coord->num_components)
            c[i] = nir_fmul(b, c[i], rcp);
      }
      nir_tex_instr_add_src(tex, nir_tex_src_coord,
                            nir_vec(b, c, coord->num_components));
      ref = nir_fmul(b, ref, rcp);
   }
   ref = nir_fmul_imm(b, nir_fsat(b, ref), 16777215.0 / 16777216.0);
   nir_tex_instr_add_src(tex, nir_tex_src_comparator, ref);
   return true;
}

bool
chrome9_nir_lower_shadow(nir_shader *nir, unsigned count,
                         const uint16_t *keys)
{
   enum compare_func funcs[32];
   nir_lower_tex_shadow_swizzle swizzles[32];
   bool progress = false;

   count = MIN2(count, ARRAY_SIZE(funcs));
   for (unsigned i = 0; i < count; i++) {
      /* PIPE_FUNC_* and COMPARE_FUNC_* share the order NEVER..ALWAYS. */
      funcs[i] = (enum compare_func)(keys[i] & 7);
      swizzles[i] = (nir_lower_tex_shadow_swizzle) {
         .swizzle_r = keys[i] >> 3 & 7,
         .swizzle_g = keys[i] >> 6 & 7,
         .swizzle_b = keys[i] >> 9 & 7,
         .swizzle_a = keys[i] >> 12 & 7,
      };
   }
   NIR_PASS(progress, nir, nir_shader_tex_pass,
            chrome9_prepare_shadow_reference, nir_metadata_control_flow,
            NULL);
   NIR_PASS(progress, nir, nir_lower_tex_shadow, count, funcs, swizzles,
            false);
   return progress;
}
