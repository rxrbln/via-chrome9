/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Draw-time fragment shader specialization for Chrome9.
 *
 * The recovered pixel ISA is straight-line.  Uniform loop bounds and wholly
 * uniform branch conditions therefore become part of a bounded shader
 * variant key; this file performs the target-independent NIR part of that
 * operation.  It intentionally contains no application or source-shape keys.
 */
#include "chrome9_specialise.h"

#include <string.h>

#include "compiler/shader_info.h"
#include "nir.h"
#include "nir_builder.h"

static bool
chrome9_ubo_index_use(const nir_def *def, unsigned depth, bool saw_iadd)
{
   bool used = false;

   if (depth > 8 || nir_def_used_by_if(def))
      return false;

   nir_foreach_use(use, def) {
      nir_instr *instr = nir_src_use_instr(use);

      used = true;
      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
             use != &intr->src[1])
            return false;
         continue;
      }
      if (instr->type != nir_instr_type_alu)
         return false;

      nir_alu_instr *alu = nir_instr_as_alu(instr);
      if (alu->op == nir_op_iadd) {
         unsigned source = use == &alu->src[0].src ? 0 : 1;
         unsigned other = 1 - source;

         /* The recovered bone-address DAG has at most one row offset on
          * each lane.  Reject a chain even when every individual immediate
          * is small: otherwise +2,+2 would bypass the +3 bound. */
         if (saw_iadd || use != &alu->src[source].src ||
             !nir_src_is_const(alu->src[other].src))
            return false;
         for (unsigned c = 0; c < alu->def.num_components; c++) {
            unsigned swizzle = alu->src[other].swizzle[c];

            if (nir_src_comp_as_uint(alu->src[other].src, swizzle) > 3)
               return false;
         }
      } else if (alu->op != nir_op_mov) {
         return false;
      }
      if (!chrome9_ubo_index_use(&alu->def, depth + 1,
                                 saw_iadd || alu->op == nir_op_iadd))
         return false;
   }
   return used;
}

static bool
chrome9_lower_ubo_index_shift(nir_builder *b, nir_instr *instr, void *data)
{
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_ishl || alu->def.bit_size != 32 ||
       alu->src[0].src.ssa->bit_size != 32 ||
       !nir_src_is_const(alu->src[1].src))
      return false;
   for (unsigned c = 0; c < alu->def.num_components; c++) {
      unsigned swizzle = alu->src[1].swizzle[c];

      if (nir_src_comp_as_uint(alu->src[1].src, swizzle) != 2)
         return false;
   }
   if (!chrome9_ubo_index_use(&alu->def, 0, false))
      return false;

   /* The use walk admitted only defined load_ubo_vec4 addresses.  Their valid
    * vec4 indices are bounded by the advertised constant file, far below
    * float32's exact-integer limit.  In ioquake the source is additionally an
    * unnormalized UBYTE bone-index vector (0..255).  Keep the scale explicitly
    * float-backed: nir_to_tgsi otherwise canonicalizes integer multiply by
    * four back into the unsupported shift. */
   b->cursor = nir_before_instr(instr);
   nir_def *source = nir_i2f32(b, nir_ssa_for_alu_src(b, alu, 0));
   nir_def_replace(&alu->def, nir_fmul_imm(b, source, 4.0f));
   return true;
}

bool
chrome9_nir_lower_ubo_index_shifts(nir_shader *nir)
{
   bool progress = nir_shader_instructions_pass(
      nir, chrome9_lower_ubo_index_shift, nir_metadata_control_flow, NULL);

   if (progress)
      NIR_PASS(_, nir, nir_opt_dce);
   return progress;
}

static bool
chrome9_condition_dwords(nir_scalar scalar, uint16_t *dwords,
                         unsigned *count, unsigned depth)
{
   if (nir_scalar_is_const(scalar))
      return true;
   if (depth > 16)
      return false;
   if (nir_scalar_is_alu(scalar)) {
      nir_alu_instr *alu = nir_def_as_alu(scalar.def);

      if (nir_op_infos[alu->op].output_size && !nir_op_is_vec(alu->op))
         return false;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         if (!chrome9_condition_dwords(nir_scalar_chase_alu_src(scalar, i),
                                       dwords, count, depth + 1))
            return false;
      }
      return true;
   }
   if (nir_scalar_is_intrinsic(scalar)) {
      nir_intrinsic_instr *intr = nir_def_as_intrinsic(scalar.def);
      unsigned word;

      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]))
         return false;
      word = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
             nir_intrinsic_component(intr) + scalar.comp;
      for (unsigned i = 0; i < *count; i++) {
         if (dwords[i] == word)
            return true;
      }
      if (*count == MAX_INLINABLE_UNIFORMS)
         return false;
      dwords[(*count)++] = word;
      return true;
   }
   return false;
}

static bool
chrome9_add_dword(uint16_t *dwords, unsigned *count, unsigned word)
{
   for (unsigned i = 0; i < *count; i++) {
      if (dwords[i] == word)
         return true;
   }
   if (*count == MAX_INLINABLE_UNIFORMS)
      return false;
   dwords[(*count)++] = word;
   return true;
}

/* Unlike an if condition, a loop-exit condition also reads its induction
 * variable and cannot fold as a whole.  Collect only its uniform leaves. */
static void
chrome9_bound_dwords(nir_scalar scalar, uint16_t *dwords, unsigned *count,
                     unsigned depth)
{
   if (depth > 16 || nir_scalar_is_const(scalar))
      return;
   if (nir_scalar_is_alu(scalar)) {
      nir_alu_instr *alu = nir_def_as_alu(scalar.def);

      if (nir_op_infos[alu->op].output_size && !nir_op_is_vec(alu->op))
         return;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         chrome9_bound_dwords(nir_scalar_chase_alu_src(scalar, i), dwords,
                              count, depth + 1);
      return;
   }
   if (nir_scalar_is_intrinsic(scalar)) {
      nir_intrinsic_instr *intr = nir_def_as_intrinsic(scalar.def);

      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]))
         return;
      chrome9_add_dword(
         dwords, count,
         (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
         nir_intrinsic_component(intr) + scalar.comp);
   }
}

static bool
chrome9_branch_breaks(struct exec_list *list)
{
   nir_block *last = list->head_sentinel.next == &list->tail_sentinel ? NULL :
      nir_cf_node_as_block(exec_node_data(nir_cf_node,
                                          list->tail_sentinel.prev, node));

   return last && nir_block_ends_in_break(last);
}

static void
chrome9_loop_bound_uniforms(struct exec_list *body, bool in_loop,
                            uint16_t *dwords, unsigned *count)
{
   foreach_list_typed(nir_cf_node, node, node, body) {
      switch (node->type) {
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);

         if (in_loop && (chrome9_branch_breaks(&nif->then_list) ||
                         chrome9_branch_breaks(&nif->else_list)))
            chrome9_bound_dwords(nir_get_scalar(nif->condition.ssa, 0),
                                 dwords, count, 0);
         chrome9_loop_bound_uniforms(&nif->then_list, in_loop, dwords, count);
         chrome9_loop_bound_uniforms(&nif->else_list, in_loop, dwords, count);
         break;
      }
      case nir_cf_node_loop:
         chrome9_loop_bound_uniforms(&nir_cf_node_as_loop(node)->body, true,
                                     dwords, count);
         break;
      default:
         break;
      }
   }
}

/* A terminate predicate normally combines a uniform mode with per-fragment
 * data, so it cannot fold as a whole until the uniform leaf is inlined.  Like
 * a loop bound, collect only direct CB0 leaves and leave every dynamic value
 * untouched. */
static void
chrome9_terminate_uniforms(nir_shader *nir, uint16_t *dwords,
                           unsigned *count)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            if (intr->intrinsic != nir_intrinsic_terminate_if)
               continue;
            chrome9_bound_dwords(nir_get_scalar(intr->src[0].ssa, 0),
                                  dwords, count, 0);
         }
      }
   }
}

/* nir_lower_int_to_float also converts the integer element index of a
 * load_ubo_vec4.  It is an address, not shader data, and must stay integer. */
static bool
chrome9_ubo_offset_to_int(nir_builder *b, nir_intrinsic_instr *intr,
                          void *data)
{
   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_ubo_vec4)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_src_rewrite(&intr->src[1], nir_f2i32(b, intr->src[1].ssa));
   return true;
}

/* x * rcp(y) truncates exact quotients one low (76 / 76 = 0), so correct
 * the magnitude from the exact remainder of float-held integers. */
static bool
chrome9_lower_idiv(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)data;
   if (alu->op != nir_op_idiv)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   nir_def *y = nir_ssa_for_alu_src(b, alu, 1);
   nir_def *ax = nir_iabs(b, x), *ay = nir_iabs(b, y);
   nir_def *q = nir_f2i32(b, nir_ftrunc(b, nir_fmul(b, nir_i2f32(b, ax),
                                        nir_frcp(b, nir_i2f32(b, ay)))));
   nir_def *r = nir_isub(b, ax, nir_imul(b, q, ay));

   q = nir_iadd(b, q, nir_isub(b, nir_b2i32(b, nir_ige(b, r, ay)),
                               nir_b2i32(b, nir_ilt_imm(b, r, 0))));
   q = nir_bcsel(b, nir_ilt_imm(b, nir_imul(b, x, y), 0), nir_ineg(b, q), q);
   nir_def_replace(&alu->def, q);
   return true;
}

void
chrome9_nir_lower_integers(nir_shader *nir)
{
   if (!nir->options->no_integers)
      return;
   /* Local array indices must stay integer: with derefs still present,
    * int_to_float also converts an index constant CSE shared with an
    * integer operand (m[1] became m[0x3f800000]). */
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS(_, nir, nir_shader_alu_pass, chrome9_lower_idiv,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_lower_int_to_float);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, chrome9_ubo_offset_to_int,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);
}

void
chrome9_nir_find_inlinable_uniforms(nir_shader *nir)
{
   uint16_t dwords[MAX_INLINABLE_UNIFORMS] = { 0 };
   unsigned count = 0;

   NIR_PASS(_, nir, nir_opt_constant_folding);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_if *nif = nir_block_get_following_if(block);
         if (nif) {
            uint16_t trial[MAX_INLINABLE_UNIFORMS];
            unsigned trial_count = count;

            memcpy(trial, dwords, sizeof(dwords));
            if (chrome9_condition_dwords(
                   nir_get_scalar(nif->condition.ssa, 0),
                   trial, &trial_count, 0)) {
               memcpy(dwords, trial, sizeof(dwords));
               count = trial_count;
            }
         }
      }
   }
   nir_foreach_function_impl(impl, nir)
      chrome9_loop_bound_uniforms(&impl->body, false, dwords, &count);
   chrome9_terminate_uniforms(nir, dwords, &count);
   /* Loop bounds are required for an unrolled straight-line program.  Give
    * them priority over terminate and select predicates in the four-dword
    * variant key. */
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);

            if (alu->op != nir_op_bcsel)
               continue;
            /* The pixel ISA has no dynamic select.  Only key on a wholly
             * uniform predicate; never inline data from either arm. */
            for (unsigned c = 0; c < alu->def.num_components; c++) {
               uint16_t trial[MAX_INLINABLE_UNIFORMS];
               unsigned trial_count = count;
               nir_scalar predicate = nir_scalar_chase_alu_src(
                  nir_get_scalar(&alu->def, c), 0);

               memcpy(trial, dwords, sizeof(dwords));
               if (chrome9_condition_dwords(predicate, trial,
                                            &trial_count, 0)) {
                  memcpy(dwords, trial, sizeof(dwords));
                  count = trial_count;
               }
            }
         }
      }
   }
   memcpy(nir->info.inlinable_uniform_dw_offsets, dwords, sizeof(dwords));
   nir->info.num_inlinable_uniforms = count;
}

struct chrome9_inline_set {
   const uint16_t *dwords;
   const uint32_t *values;
   unsigned count;
};

static int
chrome9_ubo_base_dword(const nir_intrinsic_instr *intr)
{
   if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
       !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
       !nir_src_is_const(intr->src[1]) || intr->def.bit_size != 32)
      return -1;
   return (int)((nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) *
                4 + nir_intrinsic_component(intr));
}

static bool
chrome9_inline_ubo_vec4(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct chrome9_inline_set *set = data;
   nir_scalar components[NIR_MAX_VEC_COMPONENTS];
   int base = chrome9_ubo_base_dword(intr);
   bool hit = false;

   if (base < 0)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      components[c] = nir_get_scalar(&intr->def, c);
      for (unsigned i = 0; i < set->count; i++) {
         if (set->dwords[i] != base + c)
            continue;
         components[c] = nir_get_scalar(
            nir_build_imm(b, 1, 32,
                          &(nir_const_value){ .u32 = set->values[i] }), 0);
         hit = true;
      }
   }
   if (!hit)
      return false;
   nir_def *vector = nir_vec_scalars(b, components, intr->def.num_components);
   nir_def_rewrite_uses_after(&intr->def, vector);
   return true;
}

/* The pixel engine has no flow control: every if whose arms hold no loop
 * becomes selects, discards inside it conditional. */
static const nir_opt_peephole_select_options chrome9_flatten = {
   .limit = ~0u,
   .indirect_load_ok = true,
   .expensive_alu_ok = true,
   .discard_ok = true,
};

void
chrome9_nir_flatten_ifs(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_peephole_select, &chrome9_flatten);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);
}

static void
chrome9_fold_after_inline(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_loop);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_peephole_select, &chrome9_flatten);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);
}

void
chrome9_nir_inline_uniforms(nir_shader *nir, const uint32_t *values)
{
   struct chrome9_inline_set set = {
      .dwords = nir->info.inlinable_uniform_dw_offsets,
      .values = values,
      .count = nir->info.num_inlinable_uniforms,
   };

   if (!set.count ||
       !nir_shader_intrinsics_pass(nir, chrome9_inline_ubo_vec4,
                                   nir_metadata_control_flow, &set))
      return;
   chrome9_fold_after_inline(nir);
}

static bool
chrome9_replace_front_face(nir_builder *b, nir_intrinsic_instr *intr,
                           void *data)
{
   bool front_face = *(const bool *)data;
   nir_def *replacement;

   b->cursor = nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_front_face:
      replacement = nir_imm_boolN_t(b, front_face, intr->def.bit_size);
      break;
   case nir_intrinsic_load_front_face_fsign:
      replacement = nir_imm_floatN_t(b, front_face ? 1.0 : -1.0,
                                     intr->def.bit_size);
      break;
   case nir_intrinsic_load_input:
      if (b->shader->info.stage != MESA_SHADER_FRAGMENT ||
          intr->def.num_components != 1 ||
          nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_FACE ||
          nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) !=
             nir_type_bool)
         return false;
      replacement = nir_imm_boolN_t(b, front_face, intr->def.bit_size);
      break;
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      nir_variable *var = nir_intrinsic_get_var(intr, 0);

      if (b->shader->info.stage != MESA_SHADER_FRAGMENT || !deref ||
          deref->deref_type != nir_deref_type_var || !var ||
          var->data.mode != nir_var_shader_in ||
          var->data.location != VARYING_SLOT_FACE ||
          !glsl_type_is_boolean(glsl_without_array(var->type)) ||
          intr->def.num_components != 1)
         return false;
      replacement = nir_imm_boolN_t(b, front_face, intr->def.bit_size);
      break;
   }
   default:
      return false;
   }
   nir_def_replace(&intr->def, replacement);
   return true;
}

bool
chrome9_nir_specialize_front_face(nir_shader *nir, bool front_face)
{
   if (!nir_shader_intrinsics_pass(nir, chrome9_replace_front_face,
                                   nir_metadata_control_flow, &front_face))
      return false;
   chrome9_fold_after_inline(nir);
   return true;
}

static bool
chrome9_sprite_coord_slot(unsigned location, uint32_t enable)
{
   if (location == VARYING_SLOT_PNTC)
      return true;
   if (location >= VARYING_SLOT_TEX0 && location <= VARYING_SLOT_TEX7)
      return enable & BITFIELD_BIT(location - VARYING_SLOT_TEX0);
   if (location >= VARYING_SLOT_VAR0 && location - VARYING_SLOT_VAR0 < 23)
      return enable & BITFIELD_BIT(9 + location - VARYING_SLOT_VAR0);
   return false;
}

static bool
chrome9_flip_sprite_coord(nir_builder *b, nir_intrinsic_instr *intr,
                          void *data)
{
   const uint32_t enable = *(const uint32_t *)data;
   unsigned location, component;
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];

   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      location = nir_intrinsic_io_semantics(intr).location;
      component = nir_intrinsic_component(intr);
      break;
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      nir_variable *var = nir_intrinsic_get_var(intr, 0);

      if (!deref || deref->deref_type != nir_deref_type_var || !var ||
          var->data.mode != nir_var_shader_in)
         return false;
      location = var->data.location;
      component = var->data.location_frac;
      break;
   }
   default:
      return false;
   }
   if (!chrome9_sprite_coord_slot(location, enable) || component > 1 ||
       1 - component >= intr->def.num_components ||
       intr->def.bit_size != 32)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned c = 0; c < intr->def.num_components; c++)
      comps[c] = nir_channel(b, &intr->def, c);
   comps[1 - component] = nir_fsub_imm(b, 1.0, comps[1 - component]);
   nir_def_rewrite_uses_after(&intr->def,
                              nir_vec(b, comps, intr->def.num_components));
   return true;
}

/* Lower-left point sprite origin: the hardware and the draw module generate
 * upper-left coordinates, t becomes 1 - t for the enabled coordinates. */
bool
chrome9_nir_flip_sprite_coords(nir_shader *nir, uint32_t enable)
{
   if (!nir_shader_intrinsics_pass(nir, chrome9_flip_sprite_coord,
                                   nir_metadata_control_flow, &enable))
      return false;
   chrome9_fold_after_inline(nir);
   return true;
}
