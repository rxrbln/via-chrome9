/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

static const nir_shader_compiler_options options = {
   .lower_uniforms_to_ubo = true,
};

static nir_shader *
make_alpha_primary_multiply(void)
{
   static const unsigned aaaa[4] = { 3, 3, 3, 3 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-shrunk-primary-alpha");
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   nir_def *constant = nir_load_ubo(
      &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
      .align_mul = 16, .range_base = 0, .range = 16);

   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_fmul(&b, nir_swizzle(&b, color, aaaa, 4),
                                 constant), nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static nir_shader *
make_texture_primary_multiply(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-texture-primary-multiply");
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *coord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);

   b.shader->info.io_lowered = true;
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_output(&b, nir_fmul(&b, &tex->def, color),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static nir_shader *
make_scalar_literal_chain(bool reserve_c31, unsigned literal_count)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-packed-scalar-literals");
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *result = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);

   b.shader->info.io_lowered = true;
   for (unsigned i = 0; i < literal_count; i++)
      result = nir_fadd(&b, result,
                        nir_imm_float(&b, 1.0f + i / 16.0f));
   if (reserve_c31) {
      nir_def *uniform = nir_load_ubo(
         &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 31 * 16),
         .align_mul = 16, .range_base = 31 * 16, .range = 16);

      result = nir_fadd(&b, result, uniform);
   }
   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
check_scalar_literal_packing(struct chrome9_context *context,
                             bool reserve_c31, unsigned literal_count,
                             bool expect_success)
{
   struct chrome9_shader shader = { 0 };
   nir_shader *nir = make_scalar_literal_chain(reserve_c31, literal_count);
   bool valid = false;

   chrome9_compile_fs_nir(context, &shader, nir);
   if (!expect_success) {
      valid = !shader.semantic_fs_valid && !shader.supported;
      goto out;
   }
   if (!shader.supported || !shader.semantic_fs_valid ||
       shader.semantic_fs_ir.count != literal_count + reserve_c31 ||
       __builtin_popcount(shader.native_fs.immediate_mask) !=
          (literal_count + 3) / 4 ||
       (shader.native_fs.immediate_mask & shader.semantic_uniform_mask) ||
       (reserve_c31 && shader.semantic_uniform_mask != (1u << 31)))
      goto out;
   for (unsigned i = 0; i < literal_count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &shader.semantic_fs_ir.instruction[i];
      const struct chrome9_ps_source *source = &instruction->source[1];
      float literal = 1.0f + i / 16.0f;
      uint32_t bits;
      unsigned lane;

      memcpy(&bits, &literal, sizeof(bits));
      lane = source->swizzle[0];
      if (instruction->op != CHROME9_PS_OPCODE_ADD ||
          source->file != CHROME9_PS_SOURCE_CONSTANT ||
          source->id < 16 || source->id >= 32 || lane >= 4 ||
          !(shader.native_fs.immediate_mask & (1u << source->id)) ||
          shader.native_fs.immediate[source->id][lane] != bits)
         goto out;
      for (unsigned c = 1; c < 4; c++)
         if (source->swizzle[c] != lane)
            goto out;
   }
   valid = true;
out:
   if (!valid)
      fprintf(stderr, "scalar literal packing failed: reserve_c31=%u "
              "literal_count=%u expected=%u "
              "supported=%u semantic=%u native=%u ir=%u immediates=%08x "
              "uniforms=%08x\n", reserve_c31, literal_count,
              expect_success, shader.supported,
              shader.semantic_fs_valid, shader.native_fs_valid,
              shader.semantic_fs_ir.count, shader.native_fs.immediate_mask,
              shader.semantic_uniform_mask);
   ralloc_free(nir);
   return valid;
}

static bool
check_reordered_vector_literals(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-reordered-vector-literals");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   nir_def *first = nir_fadd(&b, color,
      nir_imm_vec4(&b, 1.0f, 2.0f, 0.0f, -0.0f));
   nir_def *second = nir_fadd(&b, first,
      nir_imm_vec4(&b, 2.0f, 1.0f, -0.0f, 0.0f));
   const struct chrome9_ps_source *a, *c;
   const uint32_t expected[4] = {
      0x3f800000, 0x40000000, 0x00000000, 0x80000000,
   };
   const uint8_t swapped[4] = { 1, 0, 3, 2 };
   bool valid = false;

   b.shader->info.io_lowered = true;
   nir_store_output(&b, second, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   if (!shader.supported || !shader.semantic_fs_valid ||
       shader.semantic_fs_ir.count != 2 ||
       shader.native_fs.immediate_mask != (1u << 31))
      goto out;
   a = &shader.semantic_fs_ir.instruction[0].source[1];
   c = &shader.semantic_fs_ir.instruction[1].source[1];
   if (a->file != CHROME9_PS_SOURCE_CONSTANT ||
       c->file != CHROME9_PS_SOURCE_CONSTANT ||
       a->id != 31 || c->id != 31 ||
       memcmp(shader.native_fs.immediate[31], expected,
              sizeof(expected)))
      goto out;
   for (unsigned lane = 0; lane < 4; lane++)
      if (a->swizzle[lane] != lane || c->swizzle[lane] != swapped[lane])
         goto out;
   valid = true;
out:
   if (!valid)
      fprintf(stderr, "reordered signed-zero vector literals failed: "
              "supported=%u semantic=%u ir=%u immediates=%08x\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_fs_ir.count, shader.native_fs.immediate_mask);
   ralloc_free(b.shader);
   return valid;
}

static bool
check_packed_alpha_kill(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-packed-alpha-kill");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *coord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   const struct chrome9_ps_source *threshold;
   bool valid = false;

   b.shader->info.io_lowered = true;
   coord = nir_fadd(&b, coord, nir_imm_float(&b, 0.25f));
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_discard_if(&b, nir_flt(&b, nir_channel(&b, &tex->def, 3),
                              nir_imm_float(&b, 0.5f)));
   nir_store_output(&b, &tex->def, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   if (setenv("CHROME9_EXPERIMENTAL_TEXKILL", "1", 1))
      goto out;
   context->fragment_kill = true;
   chrome9_compile_fs_nir(context, &shader, b.shader);
   if (!shader.supported || !shader.semantic_fs_valid ||
       shader.semantic_fs_ir.count < 4 ||
       shader.semantic_fs_ir.instruction[0].op != CHROME9_PS_OPCODE_ADD ||
       shader.semantic_fs_ir.instruction[2].op != CHROME9_PS_OPCODE_SUB)
      goto out;
   threshold = &shader.semantic_fs_ir.instruction[2].source[1];
   if (threshold->file != CHROME9_PS_SOURCE_CONSTANT ||
       threshold->id != 31 || threshold->swizzle[0] != 1 ||
       shader.native_fs.immediate[31][1] != 0x3f000000)
      goto out;
   for (unsigned c = 1; c < 4; c++)
      if (threshold->swizzle[c] != threshold->swizzle[0])
         goto out;
   valid = true;
out:
   if (!valid)
      fprintf(stderr, "packed alpha KILL threshold failed: "
              "supported=%u semantic=%u ir=%u immediates=%08x\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_fs_ir.count, shader.native_fs.immediate_mask);
   ralloc_free(b.shader);
   return valid;
}

int
main(void)
{
   struct chrome9_context context = {
      .draw_program_triangle_d16_rhw_batch = true,
   };
   struct chrome9_shader shader = { 0 };
   nir_shader *nir = make_alpha_primary_multiply();
   const struct chrome9_ps_instruction *instruction;
   const struct chrome9_ps_source *primary;

   chrome9_compile_fs_nir(&context, &shader, nir);
   if (!shader.native_fs_valid || !shader.native_fs_from_nir ||
       !shader.semantic_fs_valid || shader.supported ||
       shader.fragment_program != CHROME9_FS_UNSUPPORTED ||
       shader.semantic_fs_ir.count != 1 ||
       shader.semantic_input_mask != 1 ||
       shader.native_fs.alu_count != 1) {
      fprintf(stderr, "shrunk primary alpha compiler boundary failed: "
              "native=%u from_nir=%u semantic=%u supported=%u ir=%u "
              "inputs=%u alu=%u\n", shader.native_fs_valid,
              shader.native_fs_from_nir, shader.semantic_fs_valid,
              shader.supported, shader.semantic_fs_ir.count,
              shader.semantic_input_mask, shader.native_fs.alu_count);
      ralloc_free(nir);
      return 1;
   }
   instruction = &shader.semantic_fs_ir.instruction[0];
   primary = &instruction->source[0];
   if (instruction->op != CHROME9_PS_OPCODE_MUL ||
       primary->file != CHROME9_PS_SOURCE_INPUT || primary->id ||
       primary->swizzle[0] != CHROME9_PS_SWIZZLE_W ||
       primary->swizzle[1] != CHROME9_PS_SWIZZLE_W ||
       primary->swizzle[2] != CHROME9_PS_SWIZZLE_W ||
       primary->swizzle[3] != CHROME9_PS_SWIZZLE_W) {
      fprintf(stderr, "shrunk primary alpha source was not v0.wwww\n");
      ralloc_free(nir);
      return 1;
   }
   ralloc_free(nir);
   context.fragment_primary = true;
   context.draw_fragment_program = true;
   shader = (struct chrome9_shader) { 0 };
   nir = make_alpha_primary_multiply();
   chrome9_compile_fs_nir(&context, &shader, nir);
   if (!shader.supported ||
       shader.fragment_program != CHROME9_FS_COMPILED_ALU ||
       !shader.semantic_fs_valid || shader.semantic_input_mask != 1 ||
       shader.semantic_varying_mask) {
      fprintf(stderr, "primary-color capability did not enable semantic "
              "program without a T varying\n");
      ralloc_free(nir);
      return 1;
   }
   ralloc_free(nir);
   shader = (struct chrome9_shader) { 0 };
   nir = make_texture_primary_multiply();
   chrome9_compile_fs_nir(&context, &shader, nir);
   if (!shader.supported || !shader.semantic_fs_valid ||
       shader.semantic_input_mask != 1 ||
       shader.semantic_varying_mask != 1 ||
       shader.semantic_sampler_mask != 1 ||
       shader.semantic_fs_ir.count != 2) {
      fprintf(stderr, "texture times primary did not retain separate "
              "D1/T0 semantics: supported=%u valid=%u input=%u "
              "varying=%u sampler=%u instructions=%u\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_input_mask, shader.semantic_varying_mask,
              shader.semantic_sampler_mask, shader.semantic_fs_ir.count);
      ralloc_free(nir);
      return 1;
   }
   ralloc_free(nir);
   if (!check_scalar_literal_packing(&context, false, 17, true) ||
       !check_scalar_literal_packing(&context, true, 17, true) ||
       !check_scalar_literal_packing(&context, false, 64, true) ||
       !check_scalar_literal_packing(&context, false, 65, false) ||
       !check_reordered_vector_literals(&context) ||
       !check_packed_alpha_kill(&context))
      return 1;
   puts("chrome9 primary alpha, texture modulation, and packed literals passed");
   return 0;
}
