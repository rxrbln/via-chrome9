/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
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

void
chrome9_nir_lower_integers(nir_shader *nir)
{
   if (!nir->options->no_integers)
      return;
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
      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){ .limit = 8 });
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
