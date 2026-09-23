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
build_uniform_loop(void)
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

   nir_store_var(&b, out, nir_fadd(&b, &phi->def, dynamic), 1);
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
   nir_shader *nir = build_uniform_loop();

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
