/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include <stdio.h>
#include <stdlib.h>

#include "nir.h"
#include "nir_builder.h"

#include "chrome9_specialise.h"

static const nir_shader_compiler_options options = {
   .lower_uniforms_to_ubo = true,
   .max_unroll_iterations = 31,
   .no_integers = true,
};

static uint32_t
float_bits(float value)
{
   union {
      float f;
      uint32_t u;
   } bits = { .f = value };

   return bits.u;
}

/* Build the float-lowered form of:
 *
 *   for (float i = 0; i < cb0[0].x; i++)
 *      sum += 1;
 *   out = sum + cb0[1].x;
 *
 * The second load proves that a dynamic, non-control uniform does not enter
 * the specialization key. */
static nir_shader *
build_uniform_loop(bool with_selects)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "uniform-loop");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_float_type(), "out");
   nir_def *initial = nir_imm_float(&b, 0.0f);
   nir_def *bound = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                      nir_imm_int(&b, 0));
   nir_def *dynamic = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                        nir_imm_int(&b, 1));
   nir_def *select_sum = nir_imm_float(&b, 0.0f);

   if (with_selects) {
      for (unsigned i = 0; i < 4; i++) {
         nir_def *selector = nir_load_ubo_vec4(
            &b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, i + 2));
         nir_def *condition = nir_flt(
            &b, nir_imm_float(&b, 0.0f), selector);

         select_sum = nir_fadd(&b, select_sum,
            nir_bcsel(&b, condition, nir_imm_float(&b, 1.0f),
                      nir_imm_float(&b, 0.0f)));
      }
   }
   nir_phi_instr *phi = nir_phi_instr_create(b.shader);
   nir_loop *loop = nir_push_loop(&b);

   nir_def_init(&phi->instr, &phi->def, 1, 32);
   nir_phi_instr_add_src(phi, nir_def_block(initial), initial);

   nir_if *stop = nir_push_if(&b, nir_fge(&b, &phi->def, bound));
   nir_jump(&b, nir_jump_break);
   nir_pop_if(&b, stop);

   nir_def *next = nir_fadd_imm(&b, &phi->def, 1.0f);
   nir_phi_instr_add_src(phi, nir_def_block(next), next);
   nir_pop_loop(&b, loop);

   nir_cursor after_loop = b.cursor;
   b.cursor = nir_before_block(nir_loop_first_block(loop));
   nir_builder_instr_insert(&b, &phi->instr);
   b.cursor = after_loop;

   nir_store_var(&b, out,
                 nir_fadd(&b, nir_fadd(&b, &phi->def, dynamic), select_sum),
                 1);
   nir_validate_shader(b.shader, "Chrome9 specialization test input");
   return b.shader;
}

static nir_shader *
build_integer_condition(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "integer-condition");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_float_type(), "out");
   nir_def *bound = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                      nir_imm_int(&b, 0));
   nir_if *nif = nir_push_if(&b, nir_ige_imm(&b, bound, 1));

   nir_store_var(&b, out, nir_imm_float(&b, 1.0f), 1);
   nir_push_else(&b, nif);
   nir_store_var(&b, out, nir_imm_float(&b, 0.0f), 1);
   nir_pop_if(&b, nif);
   nir_validate_shader(b.shader, "Chrome9 integer specialization input");
   return b.shader;
}

/* A uniform controls a select, but the selected data is a separate dynamic
 * uniform.  Only the predicate may enter the variant key. */
static nir_shader *
build_uniform_select(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "uniform-select");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_float_type(), "out");
   nir_def *distance = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                          nir_imm_int(&b, 0));
   nir_def *dynamic = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                         nir_imm_int(&b, 1));
   nir_def *condition = nir_flt(&b, nir_imm_float(&b, 0.0f), distance);
   nir_def *length = nir_fsqrt(&b, nir_fmul(&b, dynamic, dynamic));
   nir_def *falloff = nir_fmin(&b,
      nir_fmul(&b, length, nir_frcp(&b, distance)),
      nir_imm_float(&b, 1.0f));

   nir_store_var(&b, out,
                 nir_bcsel(&b, condition, falloff, dynamic), 1);
   nir_validate_shader(b.shader, "Chrome9 uniform select input");
   return b.shader;
}

static nir_shader *
build_varying_select(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "varying-select");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                           glsl_float_type(), "in");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_float_type(), "out");
   nir_def *threshold = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                           nir_imm_int(&b, 0));
   nir_def *condition = nir_flt(&b, nir_load_var(&b, in), threshold);

   nir_store_var(&b, out,
                 nir_bcsel(&b, condition, nir_imm_float(&b, 1.0f),
                           nir_imm_float(&b, 0.0f)), 1);
   nir_validate_shader(b.shader, "Chrome9 varying select input");
   return b.shader;
}

static nir_shader *
build_vector_uniform_select(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "vector-uniform-select");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec2_type(), "out");
   nir_def *a = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                  nir_imm_int(&b, 0));
   nir_def *c = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                  nir_imm_int(&b, 1));
   nir_def *conditions = nir_vec2(&b,
      nir_flt(&b, nir_imm_float(&b, 0.0f), a),
      nir_flt(&b, nir_imm_float(&b, 0.0f), c));
   nir_def *selected = nir_bcsel(&b, conditions,
      nir_vec2(&b, a, c), nir_imm_vec2(&b, 0.0f, 0.0f));

   nir_store_var(&b, out, selected, 0x3);
   nir_validate_shader(b.shader, "Chrome9 vector uniform select input");
   return b.shader;
}

/* Float-lowered shape of ioquake3's integer alpha-mode tree.  The mode is a
 * direct CB0 leaf while alpha remains a per-fragment input. */
static nir_shader *
build_alpha_mode_terminates(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "alpha-mode-terminates");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                           glsl_float_type(), "alpha");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_float_type(), "out");
   nir_def *mode = nir_load_ubo_vec4(&b, 1, 32, nir_imm_int(&b, 0),
                                     nir_imm_int(&b, 0));
   nir_def *alpha = nir_load_var(&b, in);

   nir_discard_if(&b, nir_iand(&b, nir_ieq_imm(&b, mode, 1),
                                nir_feq_imm(&b, alpha, 0.0f)));
   nir_discard_if(&b, nir_iand(&b, nir_ieq_imm(&b, mode, 2),
                                nir_fge_imm(&b, alpha, 0.5f)));
   nir_discard_if(&b, nir_iand(&b, nir_ieq_imm(&b, mode, 3),
                                nir_flt_imm(&b, alpha, 0.5f)));
   nir_store_var(&b, out, alpha, 1);
   nir_validate_shader(b.shader, "Chrome9 alpha mode specialization input");
   return b.shader;
}

/* Exercise system-value, fsign, IO-lowered, and deref FACE forms.  Each
 * boolean form chooses the same numeric sign exposed directly by fsign. */
static nir_shader *
build_front_face_pair(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &options,
                                                   "front-face-pair");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                           glsl_bool_type(), "face");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out");

   in->data.location = VARYING_SLOT_FACE;
   nir_def *face = nir_load_front_face(&b, 1);
   nir_def *from_bool = nir_bcsel(&b, face, nir_imm_float(&b, 1.0f),
                                  nir_imm_float(&b, -1.0f));
   nir_def *from_fsign = nir_load_front_face_fsign(&b);
   nir_def *input_face = nir_load_input(
      &b, 1, 32, nir_imm_int(&b, 0), .dest_type = nir_type_bool32,
      .io_semantics.location = VARYING_SLOT_FACE);
   nir_def *from_input = nir_bcsel(&b, nir_b2b1(&b, input_face),
                                   nir_imm_float(&b, 1.0f),
                                   nir_imm_float(&b, -1.0f));
   nir_def *from_deref = nir_bcsel(&b, nir_load_var(&b, in),
                                   nir_imm_float(&b, 1.0f),
                                   nir_imm_float(&b, -1.0f));

   nir_store_var(&b, out,
                 nir_vec4(&b, from_bool, from_fsign,
                          from_input, from_deref), 0xf);
   nir_validate_shader(b.shader, "Chrome9 FACE specialization input");
   return b.shader;
}

static nir_shader *
build_ubo_index_shift(unsigned shift, bool dynamic_shift, bool extra_use)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
                                                   &options,
                                                   "ubo-index-shift");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_float_type(), "out");
   nir_def *zero = nir_imm_int(&b, 0);
   /* Models ioquake's unnormalized UBYTE attr_BoneIndexes after vertex fetch:
    * four exact float32 values in the inclusive range 0..255. */
   nir_def *bone = nir_load_input(
      &b, 4, 32, zero, .dest_type = nir_type_float32,
      .io_semantics.location = VERT_ATTRIB_GENERIC8);
   nir_def *amount = dynamic_shift ?
      nir_f2i32(&b, nir_load_input(
         &b, 1, 32, zero, .base = 1, .dest_type = nir_type_float32,
         .io_semantics.location = VERT_ATTRIB_GENERIC9)) :
      nir_imm_int(&b, shift);
   nir_def *indices = nir_ishl(&b, nir_f2i32(&b, bone), amount);
   nir_def *sum = nir_imm_float(&b, 0.0f);

   for (unsigned lane = 0; lane < 4; lane++) {
      nir_def *index = nir_channel(&b, indices, lane);

      for (unsigned row = 0; row < 4; row++) {
         nir_def *value = nir_load_ubo_vec4(
            &b, 1, 32, zero, index, .base = 6 + row);
         sum = nir_fadd(&b, sum, value);
      }
      if (!lane && extra_use)
         sum = nir_fadd(&b, sum, nir_i2f32(&b, index));
   }
   nir_store_var(&b, out, sum, 1);
   nir_validate_shader(b.shader, "Chrome9 UBO index shift input");
   return b.shader;
}

static unsigned
alu_op_count(nir_shader *nir, nir_op op)
{
   unsigned count = 0;

   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               count++;
   return count;
}

static bool
check_ubo_index_shift(void)
{
   nir_shader *nir = build_ubo_index_shift(2, false, false);
   bool valid = chrome9_nir_lower_ubo_index_shifts(nir) &&
                alu_op_count(nir, nir_op_ishl) == 0 &&
                alu_op_count(nir, nir_op_imul) == 0 &&
                alu_op_count(nir, nir_op_fmul) == 1;

   if (!valid) {
      fprintf(stderr, "UBO index shift prepass rejected positive case: "
                      "ishl=%u imul=%u fmul=%u\n",
              alu_op_count(nir, nir_op_ishl),
              alu_op_count(nir, nir_op_imul),
              alu_op_count(nir, nir_op_fmul));
      nir_print_shader(nir, stderr);
      ralloc_free(nir);
      return false;
   }
   nir_validate_shader(nir, "Chrome9 lowered UBO index shift");
   nir_opt_algebraic(nir);
   nir_opt_constant_folding(nir);
   nir_opt_dce(nir);
   valid &= alu_op_count(nir, nir_op_ishl) == 0 &&
            alu_op_count(nir, nir_op_fmul) == 1;
   nir_lower_int_to_float(nir);
   valid &= alu_op_count(nir, nir_op_ishl) == 0 &&
            alu_op_count(nir, nir_op_imul) == 0 &&
            alu_op_count(nir, nir_op_fmul) == 1;
   if (!valid) {
      fprintf(stderr, "UBO index shift did not lower exactly\n");
      nir_print_shader(nir, stderr);
   }
   ralloc_free(nir);

   nir = build_ubo_index_shift(1, false, false);
   valid &= !chrome9_nir_lower_ubo_index_shifts(nir) &&
            alu_op_count(nir, nir_op_ishl) == 1;
   ralloc_free(nir);

   nir = build_ubo_index_shift(2, true, false);
   valid &= !chrome9_nir_lower_ubo_index_shifts(nir) &&
            alu_op_count(nir, nir_op_ishl) == 1;
   ralloc_free(nir);

   nir = build_ubo_index_shift(2, false, true);
   valid &= !chrome9_nir_lower_ubo_index_shifts(nir) &&
            alu_op_count(nir, nir_op_ishl) == 1;
   ralloc_free(nir);
   return valid;
}

static void
check_select(nir_shader *canonical, float distance, bool positive)
{
   uint32_t value = float_bits(distance);
   nir_shader *variant = nir_shader_clone(NULL, canonical);

   chrome9_nir_inline_uniforms(variant, &value);
   nir_validate_shader(variant, "Chrome9 specialized select variant");
   if (alu_op_count(variant, nir_op_bcsel) ||
       (positive && !alu_op_count(variant, nir_op_fsqrt)) ||
       (!positive && (alu_op_count(variant, nir_op_fsqrt) ||
                     alu_op_count(variant, nir_op_frcp) ||
                     alu_op_count(variant, nir_op_fmin)))) {
      fprintf(stderr, "distance %g: select did not fold correctly: "
                      "bcsel=%u sqrt=%u rcp=%u min=%u\n",
              distance, alu_op_count(variant, nir_op_bcsel),
              alu_op_count(variant, nir_op_fsqrt),
              alu_op_count(variant, nir_op_frcp),
              alu_op_count(variant, nir_op_fmin));
      nir_print_shader(variant, stderr);
      abort();
   }
   ralloc_free(variant);
}

static void
count_cf(struct exec_list *body, unsigned *loops)
{
   foreach_list_typed(nir_cf_node, node, node, body) {
      if (node->type == nir_cf_node_loop) {
         (*loops)++;
         count_cf(&nir_cf_node_as_loop(node)->body, loops);
      } else if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);

         count_cf(&nif->then_list, loops);
         count_cf(&nif->else_list, loops);
      }
   }
}

static unsigned
loop_count(nir_shader *nir)
{
   unsigned loops = 0;

   nir_foreach_function_impl(impl, nir)
      count_cf(&impl->body, &loops);
   return loops;
}

static unsigned
if_count(nir_shader *nir)
{
   unsigned count = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         if (nir_block_get_following_if(block))
            count++;
      }
   }
   return count;
}

static unsigned
terminate_count(nir_shader *nir)
{
   unsigned count = 0;

   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(instr)->intrinsic ==
                   nir_intrinsic_terminate_if)
               count++;
   return count;
}

static bool
check_front_face(nir_shader *canonical, bool front_face)
{
   nir_shader *variant = nir_shader_clone(NULL, canonical);
   uint32_t expected = float_bits(front_face ? 1.0f : -1.0f);
   bool saw_store = false;
   bool valid = chrome9_nir_specialize_front_face(variant, front_face);

   nir_validate_shader(variant, "Chrome9 specialized FACE");
   nir_foreach_function_impl(impl, variant) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            if (intr->intrinsic == nir_intrinsic_load_front_face ||
                intr->intrinsic == nir_intrinsic_load_front_face_fsign)
               valid = false;
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_def *value = intr->src[1].ssa;

            saw_store = true;
            if (!nir_def_is_const(value) || value->num_components != 4)
               valid = false;
            else
               for (unsigned c = 0; c < 4; c++)
                  if (nir_src_comp_as_uint(nir_src_for_ssa(value), c) !=
                      expected)
                     valid = false;
         }
      }
   }
   if (!saw_store)
      valid = false;
   if (!valid) {
      fprintf(stderr, "FACE %u did not specialize correctly\n", front_face);
      nir_print_shader(variant, stderr);
   }
   ralloc_free(variant);
   return valid;
}

static bool
check_alpha_mode(nir_shader *canonical, unsigned mode,
                 unsigned expected_terminates)
{
   uint32_t value = float_bits((float)mode);
   nir_shader *variant = nir_shader_clone(NULL, canonical);
   bool valid;

   chrome9_nir_inline_uniforms(variant, &value);
   nir_validate_shader(variant, "Chrome9 specialized alpha mode");
   valid = terminate_count(variant) == expected_terminates;
   if (!valid) {
      fprintf(stderr, "alpha mode %u: terminates=%u expected=%u\n",
              mode, terminate_count(variant), expected_terminates);
      nir_print_shader(variant, stderr);
   }
   ralloc_free(variant);
   return valid;
}

static void
check_count(nir_shader *canonical, float count, bool expect_loop)
{
   uint32_t value = float_bits(count);
   nir_shader *variant = nir_shader_clone(NULL, canonical);

   chrome9_nir_inline_uniforms(variant, &value);
   nir_validate_shader(variant, "Chrome9 specialized variant");
   if (!!loop_count(variant) != expect_loop) {
      fprintf(stderr, "count %g: loops=%u, expected loops=%u\n",
              count, loop_count(variant), expect_loop);
      abort();
   }
   ralloc_free(variant);
}

int
main(void)
{
   if (!check_ubo_index_shift()) {
      fprintf(stderr, "UBO index shift lowering failed\n");
      return 1;
   }

   nir_shader *nir = build_uniform_loop(false);

   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms != 1 ||
       nir->info.inlinable_uniform_dw_offsets[0] != 0) {
      fprintf(stderr, "wrong specialization key: count=%u first=%u\n",
              nir->info.num_inlinable_uniforms,
              nir->info.inlinable_uniform_dw_offsets[0]);
      return 1;
   }

   check_count(nir, 0.0f, false);
   check_count(nir, -1.0f, false);
   check_count(nir, 1.0f, false);
   check_count(nir, 5.0f, false);
   check_count(nir, 32.0f, false);
   check_count(nir, 33.0f, true);

   ralloc_free(nir);

   nir = build_alpha_mode_terminates();
   chrome9_nir_lower_integers(nir);
   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms != 1 ||
       nir->info.inlinable_uniform_dw_offsets[0] != 0 ||
       !check_alpha_mode(nir, 0, 0) ||
       !check_alpha_mode(nir, 1, 1) ||
       !check_alpha_mode(nir, 2, 1) ||
       !check_alpha_mode(nir, 3, 1)) {
      fprintf(stderr, "alpha mode terminate specialization failed\n");
      return 1;
   }
   ralloc_free(nir);

   nir = build_front_face_pair();
   if (!check_front_face(nir, false) || !check_front_face(nir, true)) {
      fprintf(stderr, "FACE specialization failed\n");
      return 1;
   }
   ralloc_free(nir);

   nir = build_vector_uniform_select();
   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms != 2 ||
       nir->info.inlinable_uniform_dw_offsets[0] != 0 ||
       nir->info.inlinable_uniform_dw_offsets[1] != 4) {
      fprintf(stderr, "vector select has the wrong specialization key\n");
      return 1;
   }
   uint32_t vector_values[] = { float_bits(0.0f), float_bits(2.0f) };
   chrome9_nir_inline_uniforms(nir, vector_values);
   nir_validate_shader(nir, "Chrome9 vector select variant");
   if (alu_op_count(nir, nir_op_bcsel)) {
      fprintf(stderr, "vector select did not fold\n");
      return 1;
   }
   ralloc_free(nir);

   nir = build_uniform_loop(true);
   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms != MAX_INLINABLE_UNIFORMS ||
       nir->info.inlinable_uniform_dw_offsets[0] != 0) {
      fprintf(stderr, "selects displaced the required loop bound\n");
      return 1;
   }
   uint32_t loop_values[MAX_INLINABLE_UNIFORMS] = { 0 };
   loop_values[0] = float_bits(5.0f);
   chrome9_nir_inline_uniforms(nir, loop_values);
   nir_validate_shader(nir, "Chrome9 bounded select/loop variant");
   if (loop_count(nir)) {
      fprintf(stderr, "loop bound was not specialized first\n");
      return 1;
   }
   ralloc_free(nir);

   nir = build_uniform_select();
   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms != 1 ||
       nir->info.inlinable_uniform_dw_offsets[0] != 0) {
      fprintf(stderr, "uniform select has the wrong specialization key\n");
      return 1;
   }
   check_select(nir, 0.0f, false);
   check_select(nir, -1.0f, false);
   check_select(nir, 2.0f, true);
   ralloc_free(nir);

   nir = build_varying_select();
   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms) {
      fprintf(stderr, "varying select entered the specialization key\n");
      return 1;
   }
   ralloc_free(nir);

   nir = build_integer_condition();
   chrome9_nir_lower_integers(nir);
   chrome9_nir_find_inlinable_uniforms(nir);
   if (nir->info.num_inlinable_uniforms != 1 ||
       nir->info.inlinable_uniform_dw_offsets[0] != 0) {
      fprintf(stderr, "integer condition has the wrong specialization key\n");
      return 1;
   }
   uint32_t five = float_bits(5.0f);
   chrome9_nir_inline_uniforms(nir, &five);
   nir_validate_shader(nir, "Chrome9 specialized integer condition");
   if (if_count(nir)) {
      fprintf(stderr, "float-backed integer condition was not folded\n");
      return 1;
   }
   ralloc_free(nir);

   puts("chrome9 specialization tests passed");
   return 0;
}
