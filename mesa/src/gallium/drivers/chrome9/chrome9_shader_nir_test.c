/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_context.h"

#include <stdio.h>

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
   puts("chrome9 shrunk primary alpha compiler boundary passed");
   return 0;
}
