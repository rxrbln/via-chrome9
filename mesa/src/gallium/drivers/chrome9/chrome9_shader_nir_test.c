/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_draw.h"
#include "chrome9_ps_schedule.h"
#include "chrome9_public.h"
#include "chrome9_specialise.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nir.h"
#include "nir_builder.h"
#include "pipe/p_screen.h"

/* Exercise the real context callbacks without opening a DRM node.  No BOs
 * or GPU submissions are permitted by this deliberately narrow ioctl stub. */
#define CHROME9_TEST_FD 1909
static unsigned unexpected_ioctls;
static bool creating_test_screen;

int
ioctl(int fd, unsigned long request, ...)
{
   va_list arguments;
   void *payload;

   va_start(arguments, request);
   payload = va_arg(arguments, void *);
   va_end(arguments);
   if ((fd == CHROME9_TEST_FD || creating_test_screen) &&
       request == DRM_IOCTL_CHROME9_GET_PARAM) {
      struct drm_chrome9_get_param *param = payload;

      if (param && param->param == DRM_CHROME9_PARAM_DEVICE_ID) {
         param->value = 0x7122;
         return 0;
      }
      if (param && param->param == DRM_CHROME9_PARAM_REVISION) {
         param->value = 0;
         return 0;
      }
      if (param && param->param == DRM_CHROME9_PARAM_CAPABILITIES) {
         param->value = DRM_CHROME9_CAP_GEM |
                        DRM_CHROME9_CAP_GEM_SHMEM_CACHED |
                        DRM_CHROME9_CAP_DRAW_TRIANGLE |
                        DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM;
         return 0;
      }
   }
   unexpected_ioctls++;
   errno = ENOTTY;
   return -1;
}

static int
test_screen_fd(struct pipe_screen *screen)
{
   (void)screen;
   return CHROME9_TEST_FD;
}

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
make_projected_texture_primary_multiply(bool preserve_primary_alpha,
                                        bool wrong_projector_lane,
                                        bool legacy_io)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-projected-texture-primary");
   nir_variable *output = NULL;
   nir_def *tc, *color;

   if (legacy_io) {
      nir_variable *tc_var = nir_variable_create(
         b.shader, nir_var_shader_in, glsl_vec4_type(), "tc");
      nir_variable *color_var = nir_variable_create(
         b.shader, nir_var_shader_in, glsl_vec4_type(), "color");

      output = nir_variable_create(
         b.shader, nir_var_shader_out, glsl_vec4_type(), "result");
      tc_var->data.location = VARYING_SLOT_TEX0;
      tc_var->data.interpolation = INTERP_MODE_SMOOTH;
      color_var->data.location = VARYING_SLOT_COL0;
      output->data.location = FRAG_RESULT_COLOR;
      tc = nir_load_deref(&b, nir_build_deref_var(&b, tc_var));
      color = nir_load_deref(&b, nir_build_deref_var(&b, color_var));
   } else {
      nir_def *barycentric = nir_load_barycentric_pixel(
         &b, 32, .interp_mode = INTERP_MODE_SMOOTH);

      b.shader->info.io_lowered = true;
      tc = nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_VAR0);
      color = nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_COL0);
   }
   nir_def *coord = nir_vec2(&b, nir_channel(&b, tc, 0),
                             nir_channel(&b, tc, 1));
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);
   nir_def *product, *result;

   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   tex->src[1] = nir_tex_src_for_ssa(
      nir_tex_src_projector,
      nir_channel(&b, tc, wrong_projector_lane ? 2 : 3));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   product = nir_fmul(&b, &tex->def, color);
   result = preserve_primary_alpha ? nir_vec4(
      &b, nir_channel(&b, product, 0), nir_channel(&b, product, 1),
      nir_channel(&b, product, 2), nir_channel(&b, color, 3)) : product;
   if (legacy_io)
      nir_store_deref(&b, nir_build_deref_var(&b, output), result, 0xf);
   else
      nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                       .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static nir_shader *
make_two_projected_texture_primary_multiply(bool wrong_second_projector)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-two-projected-primary");
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   nir_def *sample[2];

   b.shader->info.io_lowered = true;
   for (unsigned unit = 0; unit < 2; unit++) {
      nir_def *tc = nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_VAR0 + unit);
      nir_def *coord = nir_vec2(&b, nir_channel(&b, tc, 0),
                                nir_channel(&b, tc, 1));
      nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);

      tex->op = nir_texop_tex;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->texture_index = unit;
      tex->sampler_index = unit;
      tex->coord_components = 2;
      tex->dest_type = nir_type_float32;
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
      tex->src[1] = nir_tex_src_for_ssa(
         nir_tex_src_projector,
         nir_channel(&b, tc,
                     unit == 1 && wrong_second_projector ? 2 : 3));
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      sample[unit] = &tex->def;
   }
   nir_def *product = nir_fmul(&b, sample[1],
                               nir_fmul(&b, sample[0], color));
   nir_def *result = nir_vec4(
      &b, nir_channel(&b, product, 0), nir_channel(&b, product, 1),
      nir_channel(&b, product, 2), nir_channel(&b, color, 3));

   nir_store_output(&b, result, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
check_identity_projector_compilation(struct chrome9_context *context)
{
   for (unsigned alpha = 0; alpha < 2; alpha++) {
      for (unsigned wrong_lane = 0; wrong_lane < 2; wrong_lane++) {
         for (unsigned legacy_io = 0; legacy_io < 2; legacy_io++) {
            struct chrome9_shader shader = { 0 };
            nir_shader *nir = make_projected_texture_primary_multiply(
               alpha, wrong_lane, legacy_io);
            bool valid;

            chrome9_compile_fs_nir(context, &shader, nir);
            /* A projector other than the coordinate varying's W takes the
             * general RCP/MUL computed-coordinate path. */
            valid = wrong_lane ?
               shader.supported && shader.semantic_fs_valid &&
               !shader.semantic_identity_projector_input_mask &&
               shader.semantic_fs_ir.count >= 4 &&
               shader.semantic_fs_ir.instruction[0].op ==
                  CHROME9_PS_OPCODE_RCP &&
               shader.semantic_fs_ir.instruction[1].op ==
                  CHROME9_PS_OPCODE_MUL &&
               shader.semantic_fs_ir.instruction[2].op ==
                  CHROME9_PS_OPCODE_TEX :
               shader.supported && shader.semantic_fs_valid &&
               shader.semantic_input_mask == 1 &&
               shader.semantic_varying_mask == 1 &&
               shader.semantic_sampler_mask == 1 &&
               shader.semantic_identity_projector_input_mask == (1u << 3) &&
               shader.semantic_fs_ir.count >= 2 &&
               shader.semantic_fs_ir.instruction[0].op == CHROME9_PS_OPCODE_TEX;
            if (!valid)
               fprintf(stderr, "projected texture alpha=%u wrong_lane=%u "
                       "legacy_io=%u: supported=%u semantic=%u "
                       "projector=%08x ir=%u\n",
                       alpha, wrong_lane, legacy_io, shader.supported,
                       shader.semantic_fs_valid,
                       shader.semantic_identity_projector_input_mask,
                       shader.semantic_fs_ir.count);
            ralloc_free(nir);
            if (!valid)
               return false;
         }
      }
   }
   for (unsigned wrong = 0; wrong < 2; wrong++) {
      struct chrome9_shader shader = { 0 };
      nir_shader *nir = make_two_projected_texture_primary_multiply(wrong);
      bool valid;

      chrome9_compile_fs_nir(context, &shader, nir);
      /* A mismatched second projector takes the general divide path. */
      valid = wrong ?
         shader.supported && shader.semantic_fs_valid &&
         shader.semantic_identity_projector_input_mask == (1u << 3) :
         shader.supported && shader.semantic_fs_valid &&
         shader.semantic_input_mask == 1 &&
         shader.semantic_varying_mask == 3 &&
         shader.semantic_sampler_mask == 3 &&
         shader.semantic_identity_projector_input_mask ==
            ((1u << 3) | (1u << 7));
      if (!valid)
         fprintf(stderr, "two projected textures wrong=%u: "
                 "supported=%u semantic=%u projector=%08x "
                 "varying=%x sampler=%x\n",
                 wrong, shader.supported, shader.semantic_fs_valid,
                 shader.semantic_identity_projector_input_mask,
                 shader.semantic_varying_mask,
                 shader.semantic_sampler_mask);
      ralloc_free(nir);
      if (!valid)
         return false;
   }
   return true;
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

static bool
check_separable_blur_compilation(struct chrome9_context *context)
{
   /* Exact linked glmark2 desktop horizontal pass at 300x300. */
   static const uint32_t offset_bits[11] = {
      0xbc88850a, 0xbc5a6e76, 0xbc23d2d8, 0xbbda6e76, 0xbb5a6e76,
      0x00000000,
      0x3b5a6e76, 0x3bda6e76, 0x3c23d2d8, 0x3c5a6e76, 0x3c88850a,
   };
   static const uint32_t weight_bits[6] = {
      0x3e751c19, 0x3e4cbbc3, 0x3dee9dd8,
      0x3d42070c, 0x3c5c26dd, 0x3b2e429e,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-eleven-tap-blur");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *coord = nir_load_interpolated_input(
      &b, 2, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_def *sum = NULL;
   unsigned textures = 0, adds = 0, multiplies = 0, moves = 0;
   unsigned alpha_moves = 0;
   unsigned alpha_temp = CHROME9_PS_MAX_VIRTUAL_TEMPS;
   bool coordinates_valid = true;
   bool weights_valid = true;
   bool valid = false;

   b.shader->info.io_lowered = true;
   for (int tap = -5; tap <= 5; tap++) {
      float offset, weight;
      nir_def *sample_coord;
      nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
      nir_def *weighted;

      memcpy(&offset, &offset_bits[tap + 5], sizeof(offset));
      memcpy(&weight, &weight_bits[tap < 0 ? -tap : tap], sizeof(weight));
      sample_coord = tap ?
         nir_fadd(&b, coord, nir_imm_vec2(&b, offset, 0.0f)) : coord;
      tex->op = nir_texop_tex;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->coord_components = 2;
      tex->dest_type = nir_type_float32;
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, sample_coord);
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      weighted = nir_fmul(&b, &tex->def, nir_imm_float(&b, weight));
      sum = sum ? nir_fadd(&b, sum, weighted) : weighted;
   }
   nir_store_output(&b, nir_vec4(&b,
      nir_channel(&b, sum, 0), nir_channel(&b, sum, 1),
      nir_channel(&b, sum, 2), nir_imm_float(&b, 1.0f)),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &shader.semantic_fs_ir.instruction[i];
      const struct chrome9_ps_source *source = &instruction->source[0];

      if (instruction->op == CHROME9_PS_OPCODE_TEX) {
         const struct chrome9_ps_instruction *multiply =
            i + 1 < shader.semantic_fs_ir.count ?
            &shader.semantic_fs_ir.instruction[i + 1] : NULL;
         const struct chrome9_ps_source *sample = multiply ?
            &multiply->source[0] : NULL;
         const struct chrome9_ps_source *weight = multiply ?
            &multiply->source[1] : NULL;
         unsigned distance = textures > 5 ? textures - 5 : 5 - textures;

         coordinates_valid &= textures < ARRAY_SIZE(offset_bits);
         weights_valid &= distance < ARRAY_SIZE(weight_bits);
         coordinates_valid &= instruction->sampler == 0 &&
            source->swizzle[0] == CHROME9_PS_SWIZZLE_X &&
            source->swizzle[1] == CHROME9_PS_SWIZZLE_Y;
         if (textures == 5) {
            coordinates_valid &= source->file ==
               CHROME9_PS_SOURCE_TEXTURE && source->id == 0;
         } else if (!i || source->file != CHROME9_PS_SOURCE_TEMP) {
            coordinates_valid = false;
         } else {
            const struct chrome9_ps_instruction *coordinate =
               &shader.semantic_fs_ir.instruction[i - 1];
            const struct chrome9_ps_source *varying =
               &coordinate->source[0];
            const struct chrome9_ps_source *offset =
               &coordinate->source[1];

            coordinates_valid &= coordinate->op == CHROME9_PS_OPCODE_ADD &&
               coordinate->destination.file ==
                  CHROME9_PS_DESTINATION_TEMP &&
               coordinate->destination.id == source->id &&
               coordinate->destination.writemask == 3 &&
               varying->file == CHROME9_PS_SOURCE_TEXTURE &&
               varying->id == 0 &&
               varying->swizzle[0] == CHROME9_PS_SWIZZLE_X &&
               varying->swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
               offset->file == CHROME9_PS_SOURCE_CONSTANT &&
               offset->id < 32 && offset->swizzle[0] < 4 &&
               offset->swizzle[1] < 4 &&
               shader.native_fs.immediate[offset->id]
                  [offset->swizzle[0]] ==
                     offset_bits[textures < ARRAY_SIZE(offset_bits) ?
                                 textures : 0] &&
               shader.native_fs.immediate[offset->id]
                  [offset->swizzle[1]] == 0;
         }
         weights_valid &= multiply &&
            multiply->op == CHROME9_PS_OPCODE_MUL &&
            multiply->destination.writemask == 7 &&
            sample->file == CHROME9_PS_SOURCE_TEMP &&
            sample->id == instruction->destination.id &&
            weight->file == CHROME9_PS_SOURCE_CONSTANT &&
            weight->id < 32 && weight->swizzle[0] < 4;
         if (weights_valid && distance < ARRAY_SIZE(weight_bits)) {
            weights_valid &= shader.native_fs.immediate[weight->id]
               [weight->swizzle[0]] == weight_bits[distance];
            for (unsigned lane = 0; lane < 3; lane++)
               weights_valid &= sample->swizzle[lane] == lane &&
                  weight->swizzle[lane] == weight->swizzle[0];
         }
      }
      textures += instruction->op == CHROME9_PS_OPCODE_TEX;
      adds += instruction->op == CHROME9_PS_OPCODE_ADD;
      multiplies += instruction->op == CHROME9_PS_OPCODE_MUL;
      moves += instruction->op == CHROME9_PS_OPCODE_MOV;
      if (instruction->op == CHROME9_PS_OPCODE_MOV &&
          instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
          instruction->destination.writemask == 8 &&
          source->file == CHROME9_PS_SOURCE_CONSTANT &&
          source->id < 32 && source->swizzle[3] < 4 &&
          shader.native_fs.immediate[source->id][source->swizzle[3]] ==
             0x3f800000) {
         alpha_moves++;
         alpha_temp = instruction->destination.id;
      }
   }
   const struct chrome9_ps_instruction *terminal =
      shader.semantic_fs_ir.count ?
      &shader.semantic_fs_ir.instruction[shader.semantic_fs_ir.count - 1] :
      NULL;
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.semantic_fs_ir.count == 45 && textures == 11 &&
      adds == 20 && multiplies == 11 && moves == 3 && alpha_moves == 1 &&
      coordinates_valid && weights_valid && terminal &&
      terminal->op == CHROME9_PS_OPCODE_MOV &&
      terminal->destination.file == CHROME9_PS_DESTINATION_COLOR &&
      terminal->source[0].file == CHROME9_PS_SOURCE_TEMP &&
      terminal->source[0].id == alpha_temp &&
      terminal->source[0].swizzle[3] == CHROME9_PS_SWIZZLE_W &&
      shader.semantic_sampler_mask == 1 &&
      shader.semantic_varying_mask == 1 &&
      __builtin_popcount(shader.native_fs.immediate_mask) <= 8;
   for (unsigned i = 0; i < 11; i++) {
      uint32_t bits = offset_bits[i];
      bool found = !bits;

      for (unsigned id = 16; id < 32 && !found; id++)
         if (shader.native_fs.immediate_mask & (1u << id))
            for (unsigned lane = 0; lane < 4; lane++)
               found |= shader.native_fs.immediate[id][lane] == bits;
      valid &= found;
   }
   for (unsigned i = 0; i < 7; i++) {
      uint32_t bits = i == 6 ? 0x3f800000 : weight_bits[i];
      bool found = false;

      for (unsigned id = 16; id < 32 && !found; id++)
         if (shader.native_fs.immediate_mask & (1u << id))
            for (unsigned lane = 0; lane < 4; lane++)
               found |= shader.native_fs.immediate[id][lane] == bits;
      valid &= found;
   }
   if (!valid)
      fprintf(stderr, "eleven-tap blur compilation failed: "
              "supported=%u semantic=%u ir=%u tex=%u add=%u mul=%u "
              "mov=%u alpha=%u coords=%u weights=%u samplers=%x varyings=%x "
              "immediates=%08x\n",
              shader.supported,
              shader.semantic_fs_valid, shader.semantic_fs_ir.count,
              textures, adds, multiplies, moves, alpha_moves,
              coordinates_valid, weights_valid,
              shader.semantic_sampler_mask,
              shader.semantic_varying_mask,
              shader.native_fs.immediate_mask);
   ralloc_free(b.shader);
   return valid;
}

static bool
check_vector_conditional_compilation(struct chrome9_context *context,
                                     unsigned components,
                                     bool uniform_predicate)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-vector-conditional");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *color = uniform_predicate ?
      nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                        nir_imm_int(&b, 0)) :
      nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_COL0);
   nir_def *condition = nir_fge(
      &b, nir_ffract(&b, nir_channel(&b, color, 0)),
      nir_imm_float(&b, 0.5f));
   nir_def *true_value = components == 1 ? nir_imm_float(&b, 0.25f) :
      nir_trim_vector(&b,
         nir_imm_vec4(&b, 0.25f, 0.5f, 0.75f, 1.0f), components);
   nir_def *false_value = components == 1 ? nir_imm_float(&b, 0.75f) :
      nir_trim_vector(&b,
         nir_imm_vec4(&b, 0.75f, 0.25f, 0.5f, 0.0f), components);
   nir_def *selected = nir_bcsel(&b, condition, true_value, false_value);
   nir_def *output[4];
   bool found = false;
   bool valid;

   b.shader->info.io_lowered = true;
   for (unsigned c = 0; c < 4; c++)
      output[c] = c < components ? nir_channel(&b, selected, c) :
         nir_imm_float(&b, c == 3 ? 1.0f : 0.0f);
   nir_store_output(&b, nir_vec4(&b, output[0], output[1],
                                 output[2], output[3]),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   if (uniform_predicate)
      chrome9_nir_find_inlinable_uniforms(b.shader);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &shader.semantic_fs_ir.instruction[i];
      const struct chrome9_ps_source *predicate = &instruction->source[0];

      if (instruction->op != CHROME9_PS_OPCODE_CMP)
         continue;
      found = instruction->destination.writemask ==
                 (1u << components) - 1 &&
         predicate->file == CHROME9_PS_SOURCE_TEMP;
      for (unsigned lane = 0; lane < 4; lane++) {
         found &= predicate->swizzle[lane] ==
            (lane < components ? CHROME9_PS_SWIZZLE_X :
                                 CHROME9_PS_SWIZZLE_ZERO);
         if (lane >= components)
            for (unsigned arm = 1; arm <= 2; arm++)
               found &= instruction->source[arm].swizzle[lane] ==
                        CHROME9_PS_SWIZZLE_ZERO;
      }
   }
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.native_fs_valid && found &&
      (!uniform_predicate ||
       (b.shader->info.num_inlinable_uniforms == 1 &&
        b.shader->info.inlinable_uniform_dw_offsets[0] == 0));
   if (!valid) {
      fprintf(stderr, "vector conditional compilation failed: width=%u "
              "supported=%u semantic=%u native=%u ir=%u found=%u\n",
              components, shader.supported, shader.semantic_fs_valid,
              shader.native_fs_valid, shader.semantic_fs_ir.count, found);
      for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
         const struct chrome9_ps_instruction *instruction =
            &shader.semantic_fs_ir.instruction[i];

         if (instruction->op != CHROME9_PS_OPCODE_CMP)
            continue;
         fprintf(stderr, "cmp mask=%x predicate=%u/%u/%u/%u "
                 "arm1=%u/%u/%u/%u arm2=%u/%u/%u/%u\n",
                 instruction->destination.writemask,
                 instruction->source[0].swizzle[0],
                 instruction->source[0].swizzle[1],
                 instruction->source[0].swizzle[2],
                 instruction->source[0].swizzle[3],
                 instruction->source[1].swizzle[0],
                 instruction->source[1].swizzle[1],
                 instruction->source[1].swizzle[2],
                 instruction->source[1].swizzle[3],
                 instruction->source[2].swizzle[0],
                 instruction->source[2].swizzle[1],
                 instruction->source[2].swizzle[2],
                 instruction->source[2].swizzle[3]);
      }
   }
   ralloc_free(b.shader);
   return valid;
}

static bool
check_equal_zero_select_compilation(struct chrome9_context *context,
                                    float reference, bool expect_supported)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-shared-equal-zero-select");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *varying = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR1);
   nir_def *predicate = nir_feq(&b, nir_channel(&b, varying, 2),
                                 nir_imm_float(&b, reference));
   nir_def *color = nir_imm_vec3(&b, 0.0f, 0.0f, 0.0f);
   unsigned abs_count = 0, sub_count = 0, cmp_count = 0;
   bool valid;

   nir_instr_as_alu(nir_def_instr(predicate))->fp_math_ctrl =
      nir_fp_preserve_inf | nir_fp_preserve_nan;
   for (unsigned i = 0; i < 3; i++) {
      nir_def *selected = nir_bcsel(
         &b, predicate,
         nir_imm_vec3(&b, 0.125f, 0.25f, 0.375f),
         nir_imm_vec3(&b, 0.5f, 0.625f, 0.75f));

      color = nir_fadd(&b, color, selected);
   }
   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_vec4(&b, nir_channel(&b, color, 0),
                                  nir_channel(&b, color, 1),
                                  nir_channel(&b, color, 2),
                                  nir_imm_float(&b, 1.0f)),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      switch (shader.semantic_fs_ir.instruction[i].op) {
      case CHROME9_PS_OPCODE_ABS: abs_count++; break;
      case CHROME9_PS_OPCODE_SUB: sub_count++; break;
      case CHROME9_PS_OPCODE_CMP: cmp_count++; break;
      default: break;
      }
   }
   /* NIR CSE merges the three selects.  A nonzero reference takes the
    * general finite path: SUB, ABS and a negated CMP, without a guard. */
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.native_fs_valid &&
      shader.semantic_equal_zero_input_mask ==
         (expect_supported ? 1u << 6 : 0) &&
      abs_count == 1 && sub_count == 1 && cmp_count == 1;
   if (!valid)
      fprintf(stderr, "equal-zero select compilation failed: ref=%g "
              "expected=%u supported=%u semantic=%u native=%u "
              "domain=%08x abs/sub/cmp=%u/%u/%u\n", reference,
              expect_supported, shader.supported, shader.semantic_fs_valid,
              shader.native_fs_valid,
              shader.semantic_equal_zero_input_mask,
              abs_count, sub_count, cmp_count);
   if (!valid)
      nir_print_shader(b.shader, stderr);
   ralloc_free(b.shader);
   return valid;
}

static bool
check_nonnegative_select_compilation(struct chrome9_context *context)
{
   for (unsigned test = 0; test < 5; test++) {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_FRAGMENT, &options, "chrome9-nonnegative-select");
      struct chrome9_shader shader = { 0 };
      nir_def *zero = nir_imm_int(&b, 0);
      nir_def *a = nir_load_ubo_vec4(&b, 4, 32, zero, zero);
      nir_def *c = nir_load_ubo_vec4(&b, 4, 32, zero, nir_imm_int(&b, 1));
      nir_def *length2 = nir_fdot3(&b, a, a);
      nir_def *scale = nir_fmul(&b, nir_imm_float(&b, 2.0f),
                                nir_frsq(&b, length2));
      nir_def *lighting = nir_fmax(&b, nir_fdot3(&b, a, c),
                                   nir_imm_float(&b, 0.0f));
      nir_def *bounded = nir_fmul(&b, scale, lighting);
      nir_def *value = test == 1 ? nir_channel(&b, a, 0) : bounded;
      nir_def *predicate;
      nir_def *selected;
      unsigned sub = 0, cmp = 0;
      bool found = false;
      /* The general finite-predicate path also admits the unbounded input,
       * zero and negative thresholds, and preserve_nan on a producer of
       * finite operands (d987544). */
      bool expected = true;
      bool valid;

      nir_instr_as_alu(nir_def_instr(lighting))->fp_math_ctrl =
         nir_fp_preserve_inf | nir_fp_preserve_nan;
      if (test == 3)
         nir_instr_as_alu(nir_def_instr(bounded))->fp_math_ctrl =
            nir_fp_preserve_nan;
      predicate = nir_fge(&b, value, nir_imm_float(&b,
         test == 2 ? 0.0f : test == 4 ? -0.1f : 0.1f));
      nir_instr_as_alu(nir_def_instr(predicate))->fp_math_ctrl =
         nir_fp_preserve_inf;
      selected = nir_bcsel(
         &b, predicate,
         nir_imm_vec3(&b, 0.125f, 0.25f, 0.375f),
         nir_imm_vec3(&b, 0.5f, 0.625f, 0.75f));
      nir_store_output(&b, nir_vec4(&b,
         nir_channel(&b, selected, 0), nir_channel(&b, selected, 1),
         nir_channel(&b, selected, 2), nir_imm_float(&b, 1.0f)),
         zero, .write_mask = 0xf,
         .io_semantics.location = FRAG_RESULT_COLOR);
      nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
      nir_recompute_io_bases(b.shader,
                             nir_var_shader_in | nir_var_shader_out);
      chrome9_compile_fs_nir(context, &shader, b.shader);
      for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
         const struct chrome9_ps_instruction *instruction =
            &shader.semantic_fs_ir.instruction[i];

         sub += instruction->op == CHROME9_PS_OPCODE_SUB;
         cmp += instruction->op == CHROME9_PS_OPCODE_CMP;
         if (instruction->op != CHROME9_PS_OPCODE_CMP)
            continue;
         found = instruction->destination.writemask == 0x7;
         for (unsigned lane = 0; lane < 4; lane++) {
            found &= instruction->source[0].swizzle[lane] ==
               (lane < 3 ? CHROME9_PS_SWIZZLE_X :
                           CHROME9_PS_SWIZZLE_ZERO);
            if (lane == 3)
               found &= instruction->source[1].swizzle[lane] ==
                           CHROME9_PS_SWIZZLE_ZERO &&
                        instruction->source[2].swizzle[lane] ==
                           CHROME9_PS_SWIZZLE_ZERO;
         }
      }
      valid = expected ?
         shader.supported && shader.semantic_fs_valid &&
         shader.native_fs_valid && sub == 1 && cmp == 1 && found :
         !shader.supported && !shader.semantic_fs_valid;
      if (!valid)
         fprintf(stderr, "nonnegative select case %u failed: expected=%u "
                 "supported=%u semantic=%u native=%u sub=%u cmp=%u "
                 "found=%u ir=%u\n", test, expected, shader.supported,
                 shader.semantic_fs_valid, shader.native_fs_valid,
                 sub, cmp, found, shader.semantic_fs_ir.count);
      ralloc_free(b.shader);
      if (!valid)
         return false;
   }
   return true;
}

/* A packed vec4 varying sampled as .zw (ioquake3 GL2 lightmap) must go
 * through a MOV bridge: the native direct coordinate is the varying's XY. */
static bool
check_swizzled_texcoord_bridge(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-zw-texcoord");
   struct chrome9_shader shader = { 0 };
   nir_def *bary = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *tc = nir_load_interpolated_input(
      &b, 4, 32, bary, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_def *sample[2];
   bool valid;

   b.shader->info.io_lowered = true;
   for (unsigned i = 0; i < 2; i++) {
      nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);

      tex->op = nir_texop_tex;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->coord_components = 2;
      tex->dest_type = nir_type_float32;
      tex->texture_index = tex->sampler_index = i;
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                        nir_channels(&b, tc, i ? 0xc : 0x3));
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      sample[i] = &tex->def;
   }
   nir_store_output(&b, nir_fmul(&b, sample[0], sample[1]),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   valid = shader.supported && shader.semantic_fs_valid;
   for (unsigned i = 0; valid && i < shader.semantic_fs_ir.count; i++) {
      const struct chrome9_ps_instruction *in =
         &shader.semantic_fs_ir.instruction[i];

      if (in->op == CHROME9_PS_OPCODE_TEX &&
          in->source[0].file == CHROME9_PS_SOURCE_TEXTURE &&
          (in->source[0].swizzle[0] != CHROME9_PS_SWIZZLE_X ||
           in->source[0].swizzle[1] != CHROME9_PS_SWIZZLE_Y))
         valid = false;
   }
   if (!valid)
      fprintf(stderr, "swizzled texcoord bridge failed: supported=%u "
              "semantic=%u ir=%u\n", shader.supported,
              shader.semantic_fs_valid, shader.semantic_fs_ir.count);
   ralloc_free(b.shader);
   return valid;
}

static nir_def *
flag_fp(nir_def *def, unsigned flags)
{
   nir_instr_as_alu(nir_def_instr(def))->fp_math_ctrl = flags;
   return def;
}

/* glmark2 shading:shading=cel as linked by Mesa 26.2.2 (preserve flags
 * included); straight-line lighting with three nested selects. */
static bool
check_cel_compilation(struct chrome9_context *context)
{
   const unsigned inf = nir_fp_preserve_inf;
   const unsigned inf_nan = nir_fp_preserve_inf | nir_fp_preserve_nan;
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-cel");
   struct chrome9_shader shader = { 0 };
   nir_def *bary = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *v9 = nir_load_interpolated_input(
      &b, 4, 32, bary, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_def *v11 = nir_load_interpolated_input(
      &b, 3, 32, bary, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR1);
   nir_def *zero = nir_imm_float(&b, 0.0f);
   nir_def *tenth = nir_imm_float(&b, 0.1f);
   nir_def *v12 = nir_vec4(&b, nir_channel(&b, v9, 3), nir_channel(&b, v11, 0),
                           nir_channel(&b, v11, 1), nir_channel(&b, v11, 2));
   nir_def *v12xyz = nir_trim_vector(&b, v12, 3);
   nir_def *v9xyz = nir_trim_vector(&b, v9, 3);
   nir_def *v13 = flag_fp(nir_fneg(&b, v12xyz), inf);
   nir_def *v14 = nir_fadd(&b, nir_imm_vec3(&b, 4.0f, 3.0f, 1.0f), v13);
   nir_def *v17 = nir_fmul(&b, v9xyz,
                           nir_frsq(&b, nir_fdot3(&b, v9xyz, v9xyz)));
   nir_def *v19 = nir_frsq(&b, nir_fdot3(&b, v14, v14));
   nir_def *v20 = nir_fmul(&b, v14, v19);
   nir_def *v21 = nir_fdot3(&b, v17, v20);
   nir_def *v22 = flag_fp(nir_fmax(&b, v21, zero), inf_nan);
   nir_def *v24 = nir_fmul(&b, nir_imm_float(&b, 5.196152f), v19);
   nir_def *v25 = nir_fmul(&b, v24, v22);
   nir_def *v26 = flag_fp(nir_fge(&b, v25, tenth), inf);
   nir_def *v28 = nir_bcsel(&b, v26, nir_imm_vec3(&b, 0.0f, 0.6f, 0.0f),
                            nir_imm_vec3(&b, 0.0f, 0.3f, 0.0f));
   nir_def *v30 = nir_frsq(&b, nir_fdot3(&b, v13, v13));
   nir_def *v31 = flag_fp(nir_fmul(&b, v12xyz, v30), inf);
   nir_def *v32 = flag_fp(nir_fneg(&b, v31), inf);
   nir_def *v33 = nir_fdot3(&b, v32, v17);
   nir_def *v36 = nir_fadd(&b, tenth,
                           nir_fmul(&b, v22, nir_imm_float(&b, 0.3f)));
   nir_def *v37 = flag_fp(nir_flt(&b, v33, v36), inf);
   nir_def *v39 = nir_bcsel(&b, v37, nir_imm_vec3(&b, 0.0f, 0.0f, 0.0f), v28);
   nir_def *v40 = flag_fp(nir_fneg(&b, v20), inf);
   nir_def *v41 = nir_fdot3(&b, v17, v40);
   nir_def *v43 = nir_fmul(&b, nir_fmul(&b, v41, nir_imm_float(&b, 2.0f)),
                           v17);
   nir_def *v45 = nir_fadd(&b, v40, nir_fneg(&b, v43));
   nir_def *v47 = flag_fp(nir_fmax(&b, zero, nir_fdot3(&b, v45, v32)),
                          inf_nan);
   nir_def *v48 = nir_fpow(&b, v47, nir_imm_float(&b, 10.0f));
   nir_def *v49 = flag_fp(nir_flt(&b, zero, v21), inf);
   nir_def *v51 = flag_fp(nir_flt(&b, nir_imm_float(&b, 0.5f),
                                  nir_fmul(&b, v24, v48)), inf);
   nir_def *v52 = nir_iand(&b, v49, v51);
   nir_def *v55 = nir_fadd(&b, nir_imm_vec3(&b, 0.7f, 0.7f, 0.7f),
                           nir_fmul(&b, nir_imm_float(&b, 0.3f), v39));
   nir_def *v56 = nir_bcsel(&b, v52, v55, v39);
   bool valid;

   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_vec4(&b, nir_channel(&b, v56, 0),
                                 nir_channel(&b, v56, 1),
                                 nir_channel(&b, v56, 2),
                                 nir_imm_float(&b, 1.0f)),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   /* cmd22 submits the semantic IR; the kernel allocates native temps. */
   valid = shader.supported && shader.semantic_fs_valid;
   if (!valid)
      fprintf(stderr, "cel compilation failed: supported=%u semantic=%u "
              "native=%u ir=%u alu=%u\n", shader.supported,
              shader.semantic_fs_valid, shader.native_fs_valid,
              shader.semantic_fs_ir.count, shader.native_fs.alu_count);
   ralloc_free(b.shader);
   return valid;
}

static bool
check_general_predicate_compilation(struct chrome9_context *context)
{
   static const char *const names[] = {
      "b2f32-fge", "flt-and-bcsel", "feq-bcsel", "fneu-bcsel",
      "ior-bcsel", "inot-bcsel", "preserve-nan-finite",
      "preserve-inf-normalize", "preserve-inf-unbounded",
   };
   static const unsigned min_compares[] = { 1, 2, 1, 1, 2, 1, 1, 1, 1 };

   for (unsigned test = 0; test < ARRAY_SIZE(names); test++) {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_FRAGMENT, &options, "%s", names[test]);
      struct chrome9_shader shader = { 0 };
      nir_def *zero = nir_imm_int(&b, 0);
      nir_def *a = nir_load_ubo_vec4(&b, 4, 32, zero, zero);
      nir_def *c = nir_load_ubo_vec4(&b, 4, 32, zero,
                                     nir_imm_int(&b, 1));
      nir_def *ax = nir_channel(&b, a, 0);
      nir_def *cx = nir_channel(&b, c, 0);
      nir_def *predicate;
      nir_def *selected;
      unsigned subtracts = 0, compares = 0;
      bool valid;

      switch (test) {
      case 0:
         predicate = nir_fge(&b, ax, cx);
         selected = nir_b2f32(&b, predicate);
         break;
      case 1:
         predicate = nir_iand(&b, nir_flt(&b, ax, cx),
                              nir_fge(&b, nir_channel(&b, a, 1),
                                      nir_channel(&b, c, 1)));
         break;
      case 2:
         predicate = nir_feq(&b, ax, cx);
         break;
      case 3:
         predicate = nir_fneu(&b, ax, cx);
         break;
      case 4:
         predicate = nir_ior(&b, nir_flt(&b, ax, cx),
                             nir_fge(&b, nir_channel(&b, a, 1),
                                     nir_channel(&b, c, 1)));
         break;
      case 5:
         predicate = nir_inot(&b, nir_flt(&b, ax, cx));
         break;
      case 7:
      case 8: {
         /* GLSL tags neg/compare and normalize()'s folded multiply with
          * preserve_inf; finite operands admit both forms. */
         nir_def *v = nir_trim_vector(&b, a, 3);
         nir_def *n = nir_fneg(&b, v);
         nir_def *m;

         nir_instr_as_alu(nir_def_instr(n))->fp_math_ctrl =
            nir_fp_preserve_inf;
         m = test == 7 ?
            nir_fmul(&b, v, nir_frsq(&b, nir_fdot3(&b, n, n))) :
            nir_fmul(&b, v, nir_trim_vector(&b, c, 3));
         nir_instr_as_alu(nir_def_instr(m))->fp_math_ctrl =
            nir_fp_preserve_inf;
         predicate = nir_flt(&b, nir_fdot3(&b, m, n), cx);
         nir_instr_as_alu(nir_def_instr(predicate))->fp_math_ctrl =
            nir_fp_preserve_inf;
         break;
      }
      default:
         predicate = nir_fge(&b, ax, cx);
         nir_instr_as_alu(nir_def_instr(predicate))->fp_math_ctrl =
            nir_fp_preserve_nan;
         break;
      }
      if (test != 0)
         selected = nir_bcsel(&b, predicate,
                              nir_imm_float(&b, 0.25f),
                              nir_imm_float(&b, 0.75f));
      nir_store_output(&b, nir_vec4(&b, selected, selected, selected,
                                    nir_imm_float(&b, 1.0f)), zero,
                       .write_mask = 0xf,
                       .io_semantics.location = FRAG_RESULT_COLOR);
      nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
      nir_recompute_io_bases(b.shader,
                             nir_var_shader_in | nir_var_shader_out);
      chrome9_compile_fs_nir(context, &shader, b.shader);
      for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
         subtracts += shader.semantic_fs_ir.instruction[i].op ==
                      CHROME9_PS_OPCODE_SUB;
         compares += shader.semantic_fs_ir.instruction[i].op ==
                     CHROME9_PS_OPCODE_CMP;
      }
      /* Finite operands make preserve flags vacuous (d987544, e17da35). */
      valid = shader.supported && shader.semantic_fs_valid &&
         shader.native_fs_valid &&
         subtracts >= min_compares[test] &&
         compares >= min_compares[test];
      if (!valid) {
         fprintf(stderr, "%s compilation failed: supported=%u semantic=%u "
                 "native=%u sub=%u cmp=%u ir=%u\n", names[test],
                 shader.supported, shader.semantic_fs_valid,
                 shader.native_fs_valid, subtracts, compares,
                 shader.semantic_fs_ir.count);
         nir_print_shader(b.shader, stderr);
      }
      ralloc_free(b.shader);
      if (!valid)
         return false;
   }
   return true;
}

static bool
check_canonical_uniform_select_context(void)
{
   struct pipe_screen screen = { .get_screen_fd = test_screen_fd };
   struct pipe_context *pipe = chrome9_context_create(&screen, NULL, 0);
   struct chrome9_context *ctx;
   struct chrome9_shader *shader;
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES,
      .index_size = 2,
      .has_user_indices = true,
      .instance_count = 1,
   };
   struct pipe_draw_start_count_bias draw = { .count = 3 };
   bool valid = false;

   if (!pipe)
      return false;
   ctx = chrome9_context(pipe);
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-canonical-uniform-select");
   nir_def *uniform = nir_load_ubo_vec4(&b, 4, 32,
      nir_imm_int(&b, 0), nir_imm_int(&b, 0));
   nir_def *predicate = nir_fge(&b,
      nir_ffract(&b, nir_channel(&b, uniform, 0)),
      nir_imm_float(&b, 0.5f));
   nir_def *selected = nir_bcsel(&b, predicate,
      nir_imm_float(&b, 0.25f), nir_imm_float(&b, 0.75f));
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_vec4(&b, selected, selected, selected,
                                 nir_imm_float(&b, 1.0f)),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_nir_find_inlinable_uniforms(b.shader);
   shader = pipe->create_fs_state(pipe, &state);
   if (!shader) {
      ralloc_free(b.shader);
      goto out;
   }
   if (!shader->canonical_supported || !shader->supported ||
       b.shader->info.num_inlinable_uniforms != 1 ||
       b.shader->info.inlinable_uniform_dw_offsets[0] != 0)
      goto out_shader;

   pipe->bind_fs_state(pipe, shader);
   /* A missing index pointer stops the draw immediately after variant
    * selection, before draw-module execution or a GPU ioctl. */
   ctx->vs = shader;
   for (unsigned i = 0; i < 65; i++) {
      float scalar = (float)i / 71.0f;
      uint32_t bits;

      memcpy(&bits, &scalar, sizeof(bits));
      pipe->set_inlinable_constants(pipe, MESA_SHADER_FRAGMENT, 1, &bits);
      pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
      if (!shader->supported || shader->variant_count || unexpected_ioctls ||
          (ctx->warnings & CHROME9_WARN_DRAW_STATE))
         goto out_shader;
   }
   valid = true;

out_shader:
   ctx->vs = NULL;
   pipe->bind_fs_state(pipe, NULL);
   pipe->delete_fs_state(pipe, shader);
out:
   if (!valid)
      fprintf(stderr, "canonical uniform select context fallback failed\n");
   pipe->destroy(pipe);
   return valid;
}

static bool
check_front_face_variant_context(void)
{
   struct pipe_screen screen = { .get_screen_fd = test_screen_fd };
   struct pipe_context *pipe = chrome9_context_create(&screen, NULL, 0);
   struct chrome9_context *ctx;
   struct chrome9_shader *shader = NULL;
   struct pipe_rasterizer_state raster = { 0 };
   void *state = NULL;
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES,
      .index_size = 2,
      .has_user_indices = true,
      .instance_count = 1,
   };
   struct pipe_draw_start_count_bias draw = { .count = 3 };
   uint32_t front_color = 0;
   bool valid = false;

   if (!pipe)
      return false;
   ctx = chrome9_context(pipe);
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-front-face-variant");
   nir_def *face = nir_load_front_face(&b, 1);
   nir_def *value = nir_bcsel(&b, face, nir_imm_float(&b, 0.25f),
                             nir_imm_float(&b, 0.75f));
   struct pipe_shader_state fs_state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = b.shader,
   };

   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_vec4(&b, value, value, value,
                                 nir_imm_float(&b, 1.0f)),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   shader = pipe->create_fs_state(pipe, &fs_state);
   state = pipe->create_rasterizer_state(pipe, &raster);
   if (!shader || !state || shader->canonical_supported ||
       b.shader->info.num_inlinable_uniforms)
      goto out;
   pipe->bind_fs_state(pipe, shader);
   ctx->vs = shader;

   raster.cull_face = PIPE_FACE_BACK;
   pipe->delete_rasterizer_state(pipe, state);
   state = pipe->create_rasterizer_state(pipe, &raster);
   pipe->bind_rasterizer_state(pipe, state);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   if (!shader->supported || shader->variant_count != 1 ||
       shader->variants->face_mode != 1 || unexpected_ioctls)
      goto out;
   front_color = shader->literal_color;

   raster.cull_face = PIPE_FACE_FRONT;
   pipe->bind_rasterizer_state(pipe, NULL);
   pipe->delete_rasterizer_state(pipe, state);
   state = pipe->create_rasterizer_state(pipe, &raster);
   pipe->bind_rasterizer_state(pipe, state);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   if (!shader->supported || shader->variant_count != 2 ||
       shader->variants->face_mode != 2 ||
       shader->literal_color == front_color || unexpected_ioctls)
      goto out;

   raster.cull_face = 0;
   pipe->bind_rasterizer_state(pipe, NULL);
   pipe->delete_rasterizer_state(pipe, state);
   state = pipe->create_rasterizer_state(pipe, &raster);
   pipe->bind_rasterizer_state(pipe, state);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   if (shader->supported || shader->variant_count != 2 || unexpected_ioctls)
      goto out;

   raster.cull_face = PIPE_FACE_BACK;
   pipe->bind_rasterizer_state(pipe, NULL);
   pipe->delete_rasterizer_state(pipe, state);
   state = pipe->create_rasterizer_state(pipe, &raster);
   pipe->bind_rasterizer_state(pipe, state);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   valid = shader->supported && shader->variant_count == 2 &&
           shader->literal_color == front_color && !unexpected_ioctls;

out:
   if (!valid)
      fprintf(stderr, "front-face cull variant cache failed: supported=%u "
              "variants=%u ioctls=%u\n", shader && shader->supported,
              shader ? shader->variant_count : 0, unexpected_ioctls);
   ctx->vs = NULL;
   if (state) {
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->delete_rasterizer_state(pipe, state);
   }
   if (shader) {
      pipe->bind_fs_state(pipe, NULL);
      pipe->delete_fs_state(pipe, shader);
   } else {
      ralloc_free(b.shader);
   }
   pipe->destroy(pipe);
   return valid;
}

static bool
check_saturated_add_rgb_alpha_split(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-saturated-add-rgb-alpha-split");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *first = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_def *second = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR1);
   nir_def *sum = nir_fadd(&b, first, second);
   nir_def *clamped = nir_fsat(&b, sum);
   unsigned add_count = 0, sat_count = 0;
   bool valid;

   b.shader->info.io_lowered = true;
   nir_store_output(&b,
      nir_vec4(&b, nir_channel(&b, clamped, 0),
                nir_channel(&b, clamped, 1),
                nir_channel(&b, clamped, 2),
                nir_channel(&b, first, 3)),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &shader.semantic_fs_ir.instruction[i];

      if (instruction->op == CHROME9_PS_OPCODE_ADD &&
          instruction->destination.saturate &&
          (instruction->destination.writemask & 0x7) == 0x7)
         add_count++;
      if (instruction->op == CHROME9_PS_OPCODE_MOV &&
          instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
          instruction->destination.saturate &&
          (instruction->destination.writemask & 0x7) == 0x7)
         sat_count++;
   }
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.semantic_varying_mask == 3 && add_count == 1 && !sat_count;
   if (!valid)
      fprintf(stderr, "saturated ADD RGB/alpha split failed: "
              "supported=%u semantic=%u varyings=%x add=%u sat=%u\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_varying_mask, add_count, sat_count);
   ralloc_free(b.shader);
   return valid;
}

static nir_shader *
make_position_alias_fragment(unsigned offset)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-position-alias");
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *position = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, offset),
      .io_semantics.location = VARYING_SLOT_POS);

   b.shader->info.io_lowered = true;
   /* The measured POSITION source exposes XY only, so test exactly the
    * gl_FragCoord lanes that the structured fragment ABI can read. */
   nir_store_output(&b,
                    nir_vec4(&b, nir_channel(&b, position, 0),
                              nir_channel(&b, position, 1),
                              nir_imm_float(&b, 0.0f),
                              nir_imm_float(&b, 1.0f)),
                    nir_imm_int(&b, 0),
                    .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
check_position_alias_compilation(struct chrome9_context *context)
{
   struct chrome9_shader direct = { 0 }, indirect = { 0 };
   nir_shader *direct_nir = make_position_alias_fragment(0);
   nir_shader *indirect_nir = make_position_alias_fragment(1);
   const struct chrome9_ps_instruction *instruction;
   bool valid;

   chrome9_compile_fs_nir(context, &direct, direct_nir);
   chrome9_compile_fs_nir(context, &indirect, indirect_nir);
   instruction = &direct.semantic_fs_ir.instruction[0];
   valid = direct.supported && direct.semantic_fs_valid &&
      direct.semantic_fs_ir.count >= 1 &&
      direct.semantic_input_mask == 0 &&
      direct.semantic_varying_mask == 0 &&
      instruction->source[0].file == CHROME9_PS_SOURCE_POSITION &&
      instruction->source[0].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
      instruction->source[0].swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
      !indirect.semantic_fs_valid;
   if (!valid)
      fprintf(stderr, "POSITION alias failed: direct=%u/%u/%u "
              "src=%u varyings=%x indirect=%u\n",
              direct.supported, direct.semantic_fs_valid,
              direct.semantic_fs_ir.count, instruction->source[0].file,
              direct.semantic_varying_mask, indirect.semantic_fs_valid);
   ralloc_free(direct_nir);
   ralloc_free(indirect_nir);
   return valid;
}

/* Reference evaluation of the semantic IR for the condition tests. */
static bool
eval_ps_source(const float temp[][4], const float constant[][4],
               const struct chrome9_ps_source *source, float value[4])
{
   const float *base;

   if (source->file == CHROME9_PS_SOURCE_TEMP &&
       source->id < CHROME9_PS_MAX_VIRTUAL_TEMPS)
      base = temp[source->id];
   else if (source->file == CHROME9_PS_SOURCE_CONSTANT && source->id < 32)
      base = constant[source->id];
   else
      return false;
   for (unsigned c = 0; c < 4; c++) {
      const unsigned lane = source->swizzle[c];

      value[c] = lane < 4 ? base[lane] :
                 lane == CHROME9_PS_SWIZZLE_ONE ? 1.0f : 0.0f;
      if (source->modifier == CHROME9_PS_SOURCE_NEGATE)
         value[c] = -value[c];
      else if (source->modifier)
         return false;
   }
   return true;
}

static bool
eval_ps_ir(const struct chrome9_ps_ir *ir, const float constant[][4],
           float color[4], bool *killed)
{
   static float temp[CHROME9_PS_MAX_VIRTUAL_TEMPS][4];

   *killed = false;
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction = &ir->instruction[i];
      float s[3][4], r[4];

      if (instruction->op == CHROME9_PS_OPCODE_KILL) {
         if (!eval_ps_source(temp, constant, &instruction->source[0], s[0]))
            return false;
         for (unsigned c = 0; c < 4; c++)
            *killed |= s[0][c] < 0.0f;
         continue;
      }

      const unsigned count =
         instruction->op == CHROME9_PS_OPCODE_MOV ||
         instruction->op == CHROME9_PS_OPCODE_ABS ? 1 :
         instruction->op == CHROME9_PS_OPCODE_MAD ||
         instruction->op == CHROME9_PS_OPCODE_CMP ? 3 : 2;

      for (unsigned n = 0; n < count; n++)
         if (!eval_ps_source(temp, constant, &instruction->source[n], s[n]))
            return false;
      for (unsigned c = 0; c < 4; c++) {
         switch (instruction->op) {
         case CHROME9_PS_OPCODE_MOV: r[c] = s[0][c]; break;
         case CHROME9_PS_OPCODE_ADD: r[c] = s[0][c] + s[1][c]; break;
         case CHROME9_PS_OPCODE_SUB: r[c] = s[0][c] - s[1][c]; break;
         case CHROME9_PS_OPCODE_MUL: r[c] = s[0][c] * s[1][c]; break;
         case CHROME9_PS_OPCODE_MAD:
            r[c] = s[0][c] * s[1][c] + s[2][c];
            break;
         case CHROME9_PS_OPCODE_MIN: r[c] = fminf(s[0][c], s[1][c]); break;
         case CHROME9_PS_OPCODE_MAX: r[c] = fmaxf(s[0][c], s[1][c]); break;
         case CHROME9_PS_OPCODE_ABS: r[c] = fabsf(s[0][c]); break;
         case CHROME9_PS_OPCODE_CMP:
            r[c] = s[0][c] >= 0.0f ? s[1][c] : s[2][c];
            break;
         case CHROME9_PS_OPCODE_DP3:
            r[c] = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                   s[0][2] * s[1][2];
            break;
         case CHROME9_PS_OPCODE_DP4:
            r[c] = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                   s[0][2] * s[1][2] + s[0][3] * s[1][3];
            break;
         default:
            return false;
         }
      }
      for (unsigned c = 0; c < 4; c++) {
         if (!(instruction->destination.writemask & (1u << c)))
            continue;
         if (instruction->destination.file == CHROME9_PS_DESTINATION_COLOR)
            color[c] = r[c];
         else if (instruction->destination.file ==
                  CHROME9_PS_DESTINATION_TEMP)
            temp[instruction->destination.id][c] = r[c];
         else
            return false;
      }
   }
   return true;
}

enum condition_case {
   COND_SHARED_FGE,
   COND_FEQ,
   COND_FNEU,
   COND_AND_FGE,
   COND_OR_FLT,
   COND_AND_FEQ,
   COND_ALL_EQUAL4,
   COND_ANY_NEQUAL2,
   COND_ANY_NEQUAL3,
   COND_LANE_FLT3,
   COND_LANE_B2F_FGE4,
   COND_LANE_FEQ3,
   COND_LANE_MIXED2,
   COND_AND_MIXED,
   COND_SIGN,
   COND_LANE_CONST_MIXED4,
   COND_ALL_BVEC4,
   COND_FINITE_CHAIN,
   /* Boolean uniforms hold exactly 0.0 or 1.0 from here on. */
   COND_BOOL_SELECT,
   COND_BOOL_LANE_IEQ2,
   COND_BOOL_ALL2,
   COND_BOOL_ANY3,
   COND_BOOL_XOR,
   COND_CASES,
};

static const struct {
   const char *name;
   unsigned sub, abs, minmax, dp, cmp;
} condition_expect[COND_CASES] = {
   [COND_SHARED_FGE] = { "shared-fge", 1, 0, 0, 0, 3 },
   [COND_FEQ] = { "feq", 1, 1, 0, 0, 1 },
   [COND_FNEU] = { "fneu", 1, 1, 0, 0, 1 },
   [COND_AND_FGE] = { "and-fge", 2, 0, 1, 0, 1 },
   [COND_OR_FLT] = { "or-flt", 2, 0, 1, 0, 1 },
   [COND_AND_FEQ] = { "and-feq", 2, 2, 1, 0, 1 },
   [COND_ALL_EQUAL4] = { "all-equal4", 1, 1, 0, 1, 1 },
   [COND_ANY_NEQUAL2] = { "any-nequal2", 1, 1, 1, 0, 1 },
   [COND_ANY_NEQUAL3] = { "any-nequal3", 1, 1, 0, 1, 1 },
   [COND_LANE_FLT3] = { "lane-flt3", 1, 0, 0, 0, 1 },
   [COND_LANE_B2F_FGE4] = { "lane-b2f-fge4", 1, 0, 0, 0, 1 },
   [COND_LANE_FEQ3] = { "lane-feq3", 1, 1, 0, 0, 1 },
   [COND_LANE_MIXED2] = { "lane-mixed2", 2, 0, 0, 0, 2 },
   [COND_AND_MIXED] = { "and-mixed", 2, 0, 0, 0, 2 },
   [COND_SIGN] = { "sign", 0, 0, 0, 0, 2 },
   [COND_LANE_CONST_MIXED4] = { "lane-const-mixed4", 2, 0, 0, 0, 2 },
   [COND_ALL_BVEC4] = { "all-bvec4", 3, 1, 0, 1, 3 },
   [COND_FINITE_CHAIN] = { "finite-chain", 2, 0, 0, 0, 4 },
   [COND_BOOL_SELECT] = { "bool-select", 0, 1, 0, 0, 1 },
   [COND_BOOL_LANE_IEQ2] = { "bool-lane-ieq2", 1, 1, 0, 0, 1 },
   [COND_BOOL_ALL2] = { "bool-all2", 1, 1, 1, 0, 1 },
   [COND_BOOL_ANY3] = { "bool-any3", 1, 1, 0, 1, 1 },
   [COND_BOOL_XOR] = { "bool-xor", 1, 1, 0, 0, 1 },
};

static nir_def *
build_condition_case(nir_builder *b, enum condition_case test, nir_def *a,
                     nir_def *c)
{
   nir_def *t = nir_imm_vec4(b, 0.25f, 0.5f, 0.75f, 1.0f);
   nir_def *f = nir_imm_vec4(b, 0.75f, 0.125f, 0.5f, 0.0f);
   nir_def *ax = nir_channel(b, a, 0), *cx = nir_channel(b, c, 0);
   nir_def *ay = nir_channel(b, a, 1), *cy = nir_channel(b, c, 1);
   nir_def *p;

   switch (test) {
   case COND_SHARED_FGE:
      p = nir_fge(b, ax, cx);
      return nir_fadd(b, nir_fadd(b, nir_bcsel(b, p, t, f),
                                  nir_bcsel(b, p, f, t)),
                      nir_bcsel(b, p, nir_fmul(b, t, t), f));
   case COND_FEQ: p = nir_feq(b, ax, cx); break;
   case COND_FNEU: p = nir_fneu(b, ax, cx); break;
   case COND_AND_FGE:
      p = nir_iand(b, nir_fge(b, ax, cx), nir_fge(b, ay, cy));
      break;
   case COND_OR_FLT:
      p = nir_ior(b, nir_flt(b, ax, cx), nir_flt(b, ay, cy));
      break;
   case COND_AND_FEQ:
      p = nir_iand(b, nir_feq(b, ax, cx), nir_feq(b, ay, cy));
      break;
   case COND_ALL_EQUAL4: p = nir_ball_fequal4(b, a, c); break;
   case COND_ANY_NEQUAL2:
      p = nir_bany_fnequal2(b, nir_trim_vector(b, a, 2),
                            nir_trim_vector(b, c, 2));
      break;
   case COND_ANY_NEQUAL3:
      p = nir_bany_fnequal3(b, nir_trim_vector(b, a, 3),
                            nir_trim_vector(b, c, 3));
      break;
   case COND_LANE_FLT3:
      return nir_pad_vector_imm_int(b, nir_bcsel(b,
         nir_flt(b, nir_trim_vector(b, a, 3), nir_trim_vector(b, c, 3)),
         nir_trim_vector(b, t, 3), nir_trim_vector(b, f, 3)), 0, 4);
   case COND_LANE_B2F_FGE4:
      return nir_b2f32(b, nir_fge(b, a, c));
   case COND_LANE_FEQ3:
      return nir_pad_vector_imm_int(b, nir_bcsel(b,
         nir_feq(b, nir_trim_vector(b, a, 3), nir_trim_vector(b, c, 3)),
         nir_trim_vector(b, t, 3), nir_trim_vector(b, f, 3)), 0, 4);
   case COND_LANE_MIXED2:
      return nir_pad_vector_imm_int(b, nir_bcsel(b,
         nir_vec2(b, nir_fge(b, ax, cx), nir_flt(b, ay, cy)),
         nir_trim_vector(b, t, 2), nir_trim_vector(b, f, 2)), 0, 4);
   case COND_SIGN: return nir_fsign(b, a);
   case COND_LANE_CONST_MIXED4:
      return nir_b2f32(b, nir_vec4(b, nir_imm_true(b), nir_fge(b, ax, cx),
                                   nir_imm_false(b),
                                   nir_inot(b, nir_flt(b, ay, cy))));
   case COND_ALL_BVEC4:
      p = nir_ball_iequal4(b, nir_vec4(b, nir_imm_true(b),
                                       nir_fge(b, ax, cx),
                                       nir_inot(b, nir_flt(b, ay, cy)),
                                       nir_imm_true(b)),
                           nir_imm_true(b));
      break;
   case COND_FINITE_CHAIN:
      /* sign, b2f and bcsel results are finite compare operands. */
      p = nir_fge(b, cx, nir_fadd(b, nir_fsign(b, ax),
                                  nir_b2f32(b, nir_fge(b, ay, cy))));
      break;
   case COND_BOOL_XOR:
      p = nir_ixor(b, nir_b2b1(b, ax), nir_b2b1(b, cx));
      break;
   case COND_BOOL_SELECT: p = nir_b2b1(b, ax); break;
   case COND_BOOL_LANE_IEQ2:
      return nir_pad_vector_imm_int(b, nir_b2f32(b, nir_ieq(b,
         nir_b2b1(b, nir_trim_vector(b, a, 2)),
         nir_b2b1(b, nir_trim_vector(b, c, 2)))), 0, 4);
   case COND_BOOL_ALL2:
      p = nir_ball_iequal2(b, nir_b2b1(b, nir_trim_vector(b, a, 2)),
                           nir_imm_true(b));
      break;
   case COND_BOOL_ANY3:
      p = nir_bany_inequal3(b, nir_b2b1(b, nir_trim_vector(b, a, 3)),
                            nir_imm_false(b));
      break;
   case COND_AND_MIXED:
   default:
      p = nir_iand(b, nir_flt(b, ax, cx), nir_fge(b, ay, cy));
      break;
   }
   return nir_bcsel(b, p, t, f);
}

static void
reference_condition_case(enum condition_case test, const float a[4],
                         const float c[4], float out[4])
{
   static const float t[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
   static const float f[4] = { 0.75f, 0.125f, 0.5f, 0.0f };
   bool p = false, lane[4] = { false };

   switch (test) {
   case COND_SHARED_FGE:
      p = a[0] >= c[0];
      for (unsigned i = 0; i < 4; i++)
         out[i] = ((p ? t[i] : f[i]) + (p ? f[i] : t[i])) +
                  (p ? t[i] * t[i] : f[i]);
      return;
   case COND_FEQ: p = a[0] == c[0]; break;
   case COND_FNEU: p = a[0] != c[0]; break;
   case COND_AND_FGE: p = a[0] >= c[0] && a[1] >= c[1]; break;
   case COND_OR_FLT: p = a[0] < c[0] || a[1] < c[1]; break;
   case COND_AND_FEQ: p = a[0] == c[0] && a[1] == c[1]; break;
   case COND_ALL_EQUAL4:
      p = a[0] == c[0] && a[1] == c[1] && a[2] == c[2] && a[3] == c[3];
      break;
   case COND_ANY_NEQUAL2: p = a[0] != c[0] || a[1] != c[1]; break;
   case COND_ANY_NEQUAL3:
      p = a[0] != c[0] || a[1] != c[1] || a[2] != c[2];
      break;
   case COND_LANE_FLT3:
   case COND_LANE_FEQ3:
      for (unsigned i = 0; i < 3; i++) {
         lane[i] = test == COND_LANE_FLT3 ? a[i] < c[i] : a[i] == c[i];
         out[i] = lane[i] ? t[i] : f[i];
      }
      out[3] = 0.0f;
      return;
   case COND_LANE_B2F_FGE4:
      for (unsigned i = 0; i < 4; i++)
         out[i] = a[i] >= c[i] ? 1.0f : 0.0f;
      return;
   case COND_LANE_MIXED2:
      out[0] = a[0] >= c[0] ? t[0] : f[0];
      out[1] = a[1] < c[1] ? t[1] : f[1];
      out[2] = out[3] = 0.0f;
      return;
   case COND_LANE_CONST_MIXED4:
      out[0] = 1.0f;
      out[1] = a[0] >= c[0] ? 1.0f : 0.0f;
      out[2] = 0.0f;
      out[3] = !(a[1] < c[1]) ? 1.0f : 0.0f;
      return;
   case COND_ALL_BVEC4: p = a[0] >= c[0] && !(a[1] < c[1]); break;
   case COND_SIGN:
      for (unsigned i = 0; i < 4; i++)
         out[i] = a[i] > 0.0f ? 1.0f : a[i] < 0.0f ? -1.0f : 0.0f;
      return;
   case COND_FINITE_CHAIN:
      p = c[0] >= (a[0] > 0.0f ? 1.0f : a[0] < 0.0f ? -1.0f : 0.0f) +
                  (a[1] >= c[1] ? 1.0f : 0.0f);
      break;
   case COND_BOOL_XOR: p = (a[0] != 0.0f) != (c[0] != 0.0f); break;
   case COND_BOOL_SELECT: p = a[0] != 0.0f; break;
   case COND_BOOL_LANE_IEQ2:
      for (unsigned i = 0; i < 4; i++)
         out[i] = i < 2 && (a[i] != 0.0f) == (c[i] != 0.0f) ? 1.0f : 0.0f;
      return;
   case COND_BOOL_ALL2: p = a[0] != 0.0f && a[1] != 0.0f; break;
   case COND_BOOL_ANY3:
      p = a[0] != 0.0f || a[1] != 0.0f || a[2] != 0.0f;
      break;
   case COND_AND_MIXED:
   default:
      p = a[0] < c[0] && a[1] >= c[1];
      break;
   }
   for (unsigned i = 0; i < 4; i++)
      out[i] = p ? t[i] : f[i];
}

/* Boolean selects lower to shared CMP conditions; the emitted IR must match
 * the NIR semantics for equal, signed-zero, ordered and mixed operands. */
static bool
check_condition_lowering(struct chrome9_context *context)
{
   static const float values[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.0f };
   const unsigned n = ARRAY_SIZE(values);

   for (unsigned test = 0; test < COND_CASES; test++) {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_FRAGMENT, &options, "%s", condition_expect[test].name);
      struct chrome9_shader shader = { 0 };
      nir_def *zero = nir_imm_int(&b, 0);
      nir_def *a = nir_load_ubo_vec4(&b, 4, 32, zero, zero);
      nir_def *c = nir_load_ubo_vec4(&b, 4, 32, zero, nir_imm_int(&b, 1));
      unsigned sub = 0, abs_count = 0, minmax = 0, dp = 0, cmp = 0;
      unsigned mismatches = 0;
      bool valid;

      nir_store_output(&b, build_condition_case(&b, test, a, c), zero,
                       .write_mask = 0xf,
                       .io_semantics.location = FRAG_RESULT_COLOR);
      nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
      nir_recompute_io_bases(b.shader,
                             nir_var_shader_in | nir_var_shader_out);
      chrome9_compile_fs_nir(context, &shader, b.shader);
      for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
         switch (shader.semantic_fs_ir.instruction[i].op) {
         case CHROME9_PS_OPCODE_SUB: sub++; break;
         case CHROME9_PS_OPCODE_ABS: abs_count++; break;
         case CHROME9_PS_OPCODE_MIN:
         case CHROME9_PS_OPCODE_MAX: minmax++; break;
         case CHROME9_PS_OPCODE_DP3:
         case CHROME9_PS_OPCODE_DP4: dp++; break;
         case CHROME9_PS_OPCODE_CMP: cmp++; break;
         default: break;
         }
      }
      valid = shader.supported && shader.semantic_fs_valid &&
         sub == condition_expect[test].sub &&
         abs_count == condition_expect[test].abs &&
         minmax == condition_expect[test].minmax &&
         dp == condition_expect[test].dp &&
         cmp == condition_expect[test].cmp;
      /* Every operand lane walks the value set; a and c share lane values
       * rotated so that equal, ordered and signed-zero pairs all occur. */
      for (unsigned i = 0; valid && i < n * n * n; i++) {
         float constant[32][4] = { { 0 } }, color[4], expect[4];

         memcpy(constant, shader.native_fs.immediate, sizeof(constant));
         for (unsigned lane = 0; lane < 4; lane++) {
            unsigned index = i / (lane & 1 ? n : 1) + lane;

            constant[0][lane] = values[index % n];
            constant[1][lane] = values[(i / (n * n) + lane * (lane & 2)) %
                                       n];
            if (test >= COND_BOOL_SELECT)
               for (unsigned u = 0; u < 2; u++)
                  constant[u][lane] = constant[u][lane] > 0.0f ? 1.0f : 0.0f;
         }
         reference_condition_case(test, constant[0], constant[1], expect);
         bool killed;

         if (!eval_ps_ir(&shader.semantic_fs_ir, constant, color, &killed) ||
             killed) {
            valid = false;
            break;
         }
         mismatches += memcmp(color, expect, sizeof(color)) != 0;
      }
      valid &= !mismatches;
      if (!valid)
         fprintf(stderr, "%s condition failed: supported=%u semantic=%u "
                 "sub=%u abs=%u minmax=%u dp=%u cmp=%u ir=%u "
                 "mismatches=%u\n", condition_expect[test].name,
                 shader.supported, shader.semantic_fs_valid, sub, abs_count,
                 minmax, dp, cmp, shader.semantic_fs_ir.count, mismatches);
      ralloc_free(b.shader);
      if (!valid)
         return false;
   }
   return true;
}

/* Two discards merge into one KILL of MIN(CMP(c0, -1, 1), CMP(c1, -1, 1)). */
static bool
check_discard_lowering(struct chrome9_context *context)
{
   static const float values[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f };
   const unsigned n = ARRAY_SIZE(values);
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-discard");
   struct chrome9_shader shader = { 0 };
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *a = nir_load_ubo_vec4(&b, 4, 32, zero, zero);
   nir_def *c = nir_load_ubo_vec4(&b, 4, 32, zero, nir_imm_int(&b, 1));
   unsigned kills = 0, cmp = 0, min = 0, mismatches = 0;
   bool old_fragment_kill = context->fragment_kill;
   bool valid;

   nir_terminate_if(&b, nir_flt(&b, nir_channel(&b, a, 0),
                                nir_channel(&b, c, 0)));
   nir_terminate_if(&b, nir_feq(&b, nir_channel(&b, a, 1),
                                nir_channel(&b, c, 1)));
   nir_store_output(&b, nir_imm_vec4(&b, 0.0f, 1.0f, 0.0f, 1.0f), zero,
                    .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   b.shader->info.fs.uses_discard = true;
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   context->fragment_kill = true;
   chrome9_compile_fs_nir(context, &shader, b.shader);
   context->fragment_kill = old_fragment_kill;
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      kills += shader.semantic_fs_ir.instruction[i].op ==
               CHROME9_PS_OPCODE_KILL;
      cmp += shader.semantic_fs_ir.instruction[i].op == CHROME9_PS_OPCODE_CMP;
      min += shader.semantic_fs_ir.instruction[i].op == CHROME9_PS_OPCODE_MIN;
   }
   valid = shader.supported && shader.semantic_fs_valid && kills == 1 &&
           cmp == 2 && min == 1;
   for (unsigned i = 0; valid && i < n * n * n * n; i++) {
      float constant[32][4] = { { 0 } }, color[4];
      bool killed;

      memcpy(constant, shader.native_fs.immediate, sizeof(constant));
      constant[0][0] = values[i % n];
      constant[1][0] = values[i / n % n];
      constant[0][1] = values[i / (n * n) % n];
      constant[1][1] = values[i / (n * n * n)];
      if (!eval_ps_ir(&shader.semantic_fs_ir, constant, color, &killed)) {
         valid = false;
         break;
      }
      mismatches += killed != (constant[0][0] < constant[1][0] ||
                               constant[0][1] == constant[1][1]);
   }
   valid &= !mismatches;
   if (!valid)
      fprintf(stderr, "discard lowering failed: supported=%u semantic=%u "
              "kill=%u cmp=%u min=%u ir=%u mismatches=%u\n",
              shader.supported, shader.semantic_fs_valid, kills, cmp, min,
              shader.semantic_fs_ir.count, mismatches);
   ralloc_free(b.shader);
   return valid;
}

static bool
check_blinn_phong_integer_power(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-blinn-phong-integer-power");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *normal = nir_load_interpolated_input(
      &b, 3, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_VAR0);
   nir_def *unit = nir_fmul(&b, normal,
      nir_frsq(&b, nir_fdot3(&b, normal, normal)));
   nir_def *half = nir_imm_vec3(&b, 0.0f, 0.5f, 0.5f);
   nir_def *light = nir_imm_vec3(&b, 0.0f, 0.0f, 1.0f);
   nir_def *spec = nir_fpow(&b,
      nir_fmax(&b, nir_fdot3(&b, unit, half), nir_imm_float(&b, 0.0f)),
      nir_imm_float(&b, 100.0f));
   nir_def *diffuse = nir_fmax(&b, nir_fdot3(&b, unit, light),
                               nir_imm_float(&b, 0.0f));
   nir_def *rgb = nir_fadd(&b,
      nir_fadd(&b, nir_fmul(&b,
         nir_imm_vec3(&b, 0.8f, 0.8f, 0.8f), spec),
         nir_imm_vec3(&b, 0.1f, 0.1f, 0.1f)),
      nir_fmul(&b, nir_imm_vec3(&b, 0.0f, 0.0f, 0.8f), diffuse));
   unsigned dp3 = 0, rsq = 0, max = 0, mul = 0, add = 0, mov = 0;
   bool high_virtual_temp = false, terminal_color = false;
   bool valid;

   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_vec4(&b,
      nir_channel(&b, rgb, 0), nir_channel(&b, rgb, 1),
      nir_channel(&b, rgb, 2), nir_imm_float(&b, 1.0f)),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &shader.semantic_fs_ir.instruction[i];

      dp3 += instruction->op == CHROME9_PS_OPCODE_DP3;
      rsq += instruction->op == CHROME9_PS_OPCODE_RSQ;
      max += instruction->op == CHROME9_PS_OPCODE_MAX;
      mul += instruction->op == CHROME9_PS_OPCODE_MUL;
      add += instruction->op == CHROME9_PS_OPCODE_ADD;
      mov += instruction->op == CHROME9_PS_OPCODE_MOV;
      high_virtual_temp |=
         instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
         instruction->destination.id >= 12;
      if (i + 1 == shader.semantic_fs_ir.count)
         terminal_color =
            instruction->destination.file == CHROME9_PS_DESTINATION_COLOR &&
            instruction->destination.writemask == 0xf;
   }
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.fragment_program == CHROME9_FS_COMPILED_ALU &&
      shader.semantic_fs_ir.count == 22 &&
      shader.semantic_input_mask == 0 &&
      shader.semantic_varying_mask == 1 &&
      !shader.semantic_sampler_mask &&
      shader.native_fs.immediate_mask == 0xc0000000 &&
      high_virtual_temp && terminal_color &&
      dp3 == 3 && rsq == 1 && max == 2 && mul == 11 &&
      add == 2 && mov == 3;
   if (!valid)
      fprintf(stderr, "Blinn-Phong integer-power cmd22 compilation failed: "
              "supported=%u semantic=%u program=%u ir=%u varying=%x "
              "sampler=%x immed=%08x native=%u temps=%u "
              "histogram=%u/%u/%u/%u/%u/%u terminal=%u\n",
              shader.supported, shader.semantic_fs_valid,
              shader.fragment_program, shader.semantic_fs_ir.count,
              shader.semantic_varying_mask, shader.semantic_sampler_mask,
              shader.native_fs.immediate_mask, shader.native_fs_valid,
              high_virtual_temp, dp3, rsq, max, mul, add, mov,
              terminal_color);
   ralloc_free(b.shader);
   return valid;
}

static nir_shader *
make_function_medium_fragment(const nir_shader_compiler_options *shader_options)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, shader_options, "chrome9-function-medium-five");
   nir_def *position = nir_load_frag_coord(&b);
   nir_def *d = nir_ffract(&b, nir_fmul(&b,
      nir_fmul(&b, nir_channel(&b, position, 0),
               nir_imm_float(&b, 0.0001f)),
      nir_channel(&b, position, 1)));

   b.shader->info.io_lowered = true;
   for (unsigned i = 0; i < 5; i++) {
      nir_def *a = nir_ffract(&b, nir_fmul(&b, nir_imm_float(&b, 3.0f), d));

      d = nir_ffract(&b, nir_fsqrt(&b, nir_fmul(&b, a, a)));
   }
   nir_store_output(&b, nir_vec4(&b, d, d, d, nir_imm_float(&b, 1.0f)),
                    nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static unsigned
count_alu_op(nir_shader *nir, nir_op op)
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
check_function_medium_sqrt_boundary(struct chrome9_context *context)
{
   struct chrome9_shader current = { 0 }, lowered = { 0 };
   nir_shader *nir = make_function_medium_fragment(&options);
   nir_shader *lowered_nir = NULL;
   struct pipe_screen *screen = NULL;
   int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
   unsigned sqrt_before = count_alu_op(nir, nir_op_fsqrt);
   unsigned sqrt_after, rsq_after, rcp_after;
   unsigned mul = 0, frc = 0, rsq = 0, rcp = 0, mov = 0;
   bool valid;

   if (fd >= 0) {
      creating_test_screen = true;
      screen = chrome9_screen_create(fd, NULL);
      creating_test_screen = false;
      close(fd);
   }
   if (!screen || !screen->nir_options[MESA_SHADER_FRAGMENT] ||
       !screen->nir_options[MESA_SHADER_FRAGMENT]->lower_fsqrt) {
      fprintf(stderr, "Chrome9 screen does not enable fsqrt lowering\n");
      if (screen)
         screen->destroy(screen);
      ralloc_free(nir);
      return false;
   }
   lowered_nir = make_function_medium_fragment(
      screen->nir_options[MESA_SHADER_FRAGMENT]);
   chrome9_compile_fs_nir(context, &current, nir);
   /* Use Mesa's standard algebraic fsqrt lowering, not a Chrome9 test-only
    * opcode substitution.  Remove dead pre-lowering nodes before compiling. */
   nir_opt_algebraic(lowered_nir);
   nir_opt_dce(lowered_nir);
   sqrt_after = count_alu_op(lowered_nir, nir_op_fsqrt);
   rsq_after = count_alu_op(lowered_nir, nir_op_frsq);
   rcp_after = count_alu_op(lowered_nir, nir_op_frcp);
   chrome9_compile_fs_nir(context, &lowered, lowered_nir);
   for (unsigned i = 0; i < lowered.semantic_fs_ir.count; i++) {
      switch (lowered.semantic_fs_ir.instruction[i].op) {
      case CHROME9_PS_OPCODE_MUL: mul++; break;
      case CHROME9_PS_OPCODE_FRC: frc++; break;
      case CHROME9_PS_OPCODE_RSQ: rsq++; break;
      case CHROME9_PS_OPCODE_RCP: rcp++; break;
      case CHROME9_PS_OPCODE_MOV: mov++; break;
      default: break;
      }
   }
   valid = sqrt_before == 5 && !current.supported &&
      !current.semantic_fs_valid && sqrt_after == 0 &&
      rsq_after == 5 && rcp_after == 5 &&
      lowered.supported && lowered.semantic_fs_valid &&
      lowered.fragment_program == CHROME9_FS_COMPILED_ALU &&
      lowered.semantic_fs_ir.count == 36 &&
      mul == 12 && frc == 11 && rsq == 5 && rcp == 5 && mov == 3 &&
      lowered.semantic_input_mask == 0 &&
      lowered.semantic_varying_mask == 0 &&
      lowered.semantic_sampler_mask == 0;
   if (!valid)
      fprintf(stderr, "function-medium sqrt boundary failed: "
              "sqrt=%u->%u rsq=%u rcp=%u current=%u/%u "
              "lowered=%u/%u/%u ir=%u histogram=%u/%u/%u/%u/%u\n",
              sqrt_before, sqrt_after, rsq_after, rcp_after,
              current.supported, current.semantic_fs_valid,
              lowered.supported, lowered.semantic_fs_valid,
              lowered.fragment_program, lowered.semantic_fs_ir.count,
              mul, frc, rsq, rcp, mov);
   ralloc_free(lowered_nir);
   ralloc_free(nir);
   screen->destroy(screen);
   return valid;
}

static nir_shader *
make_ideas_logo_shadow_fragment(float alpha_reference, unsigned sample_lane,
                                bool reverse_predicate)
{
   /* Exact linked fragment expression.  A8, NEAREST, and REPEAT belong to
    * the sampler view/state, not to the NIR texture instruction. */
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-ideas-logo-shadow");
   nir_def *position = nir_load_frag_coord(&b);
   nir_def *transform = nir_load_ubo(
      &b, 2, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
      .align_mul = 16, .range_base = 0, .range = 8);
   nir_def *y = nir_fadd(&b,
      nir_fmul(&b, nir_channel(&b, position, 1),
                    nir_channel(&b, transform, 0)),
      nir_channel(&b, transform, 1));
   nir_def *coordinate = nir_fmul(&b,
      nir_vec2(&b, nir_channel(&b, position, 0), y),
      nir_imm_vec2(&b, 1.0f / 32.0f, 1.0f / 32.0f));
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);

   b.shader->info.io_lowered = true;
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coordinate);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *sampled = nir_channel(&b, &tex->def, sample_lane);
   nir_def *reference = nir_imm_float(&b, alpha_reference);

   nir_discard_if(&b, reverse_predicate ?
      nir_flt(&b, reference, sampled) : nir_flt(&b, sampled, reference));
   nir_store_output(&b, &tex->def, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
ideas_immediate_equals(const struct chrome9_shader *shader,
                       const struct chrome9_ps_source *source,
                       unsigned lane, uint32_t value)
{
   return source->file == CHROME9_PS_SOURCE_CONSTANT &&
      source->id < 32 && source->swizzle[lane] < 4 &&
      (shader->native_fs.immediate_mask & (1u << source->id)) &&
      shader->native_fs.immediate[source->id][source->swizzle[lane]] == value;
}

static bool
check_ideas_logo_shadow_compilation(struct chrome9_context *context)
{
   struct chrome9_shader shader = { 0 }, wrong_reference = { 0 };
   struct chrome9_shader reversed = { 0 }, non_alpha = { 0 };
   struct chrome9_shader no_kill_capability = { 0 };
   nir_shader *nir = make_ideas_logo_shadow_fragment(0.5f, 3, false);
   nir_shader *negative = make_ideas_logo_shadow_fragment(0.25f, 3, false);
   nir_shader *reversed_nir = make_ideas_logo_shadow_fragment(0.5f, 3, true);
   nir_shader *non_alpha_nir = make_ideas_logo_shadow_fragment(0.5f, 0, false);
   const struct chrome9_ps_instruction *p = shader.semantic_fs_ir.instruction;
   bool valid;
   bool old_fragment_kill = context->fragment_kill;

   context->fragment_kill = true;
   chrome9_compile_fs_nir(context, &shader, nir);
   chrome9_compile_fs_nir(context, &wrong_reference, negative);
   chrome9_compile_fs_nir(context, &reversed, reversed_nir);
   chrome9_compile_fs_nir(context, &non_alpha, non_alpha_nir);
   context->fragment_kill = false;
   chrome9_compile_fs_nir(context, &no_kill_capability, nir);
   context->fragment_kill = old_fragment_kill;
   /* This is the exact source-order semantic chain: transformed Y, X/Y
    * assembly, normalized sample, sampled-alpha KILL, then sampled color. */
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.fragment_program == CHROME9_FS_COMPILED_ALU &&
      shader.semantic_fs_ir.count == 9 &&
      shader.semantic_input_mask == 0 &&
      shader.semantic_varying_mask == 0 &&
      shader.semantic_sampler_mask == 1 &&
      shader.semantic_uniform_mask == 1 &&
      p[0].op == CHROME9_PS_OPCODE_MUL &&
      p[0].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[0].destination.writemask == 1 &&
      p[0].source[0].file == CHROME9_PS_SOURCE_POSITION &&
      p[0].source[0].swizzle[0] == CHROME9_PS_SWIZZLE_Y &&
      p[0].source[1].file == CHROME9_PS_SOURCE_CONSTANT &&
      p[0].source[1].id == 0 &&
      p[0].source[1].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
      p[1].op == CHROME9_PS_OPCODE_ADD &&
      p[1].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[1].destination.writemask == 1 &&
      p[1].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[1].source[0].id == p[0].destination.id &&
      p[1].source[0].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
      p[1].source[1].file == CHROME9_PS_SOURCE_CONSTANT &&
      p[1].source[1].id == 0 &&
      p[1].source[1].swizzle[0] == CHROME9_PS_SWIZZLE_Y &&
      p[2].op == CHROME9_PS_OPCODE_MOV &&
      p[2].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[2].destination.writemask == 1 &&
      p[2].source[0].file == CHROME9_PS_SOURCE_POSITION &&
      p[2].source[0].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
      p[3].op == CHROME9_PS_OPCODE_MOV &&
      p[3].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[3].destination.id == p[2].destination.id &&
      p[3].destination.writemask == 2 &&
      p[3].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[3].source[0].id == p[1].destination.id &&
      p[3].source[0].swizzle[1] == CHROME9_PS_SWIZZLE_X &&
      p[4].op == CHROME9_PS_OPCODE_MUL &&
      p[4].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[4].destination.writemask == 3 &&
      p[4].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[4].source[0].id == p[2].destination.id &&
      p[4].source[0].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
      p[4].source[0].swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
      ideas_immediate_equals(&shader, &p[4].source[1], 0, 0x3d000000) &&
      ideas_immediate_equals(&shader, &p[4].source[1], 1, 0x3d000000) &&
      p[5].op == CHROME9_PS_OPCODE_TEX && p[5].sampler == 0 &&
      p[5].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[5].destination.writemask == 0xf &&
      p[5].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[5].source[0].id == p[4].destination.id &&
      p[5].source[0].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
      p[5].source[0].swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
      p[5].source[0].swizzle[2] == CHROME9_PS_SWIZZLE_ZERO &&
      p[5].source[0].swizzle[3] == CHROME9_PS_SWIZZLE_ONE &&
      p[6].op == CHROME9_PS_OPCODE_SUB &&
      p[6].destination.file == CHROME9_PS_DESTINATION_TEMP &&
      p[6].destination.writemask == 0xf &&
      p[6].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[6].source[0].id == p[5].destination.id &&
      ideas_immediate_equals(&shader, &p[6].source[1], 0, 0x3f000000) &&
      p[7].op == CHROME9_PS_OPCODE_KILL &&
      p[7].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[7].source[0].id == p[6].destination.id &&
      p[8].op == CHROME9_PS_OPCODE_MOV &&
      p[8].destination.file == CHROME9_PS_DESTINATION_COLOR &&
      p[8].destination.id == 0 &&
      p[8].destination.writemask == 0xf &&
      !p[8].destination.saturate &&
      p[8].source[0].file == CHROME9_PS_SOURCE_TEMP &&
      p[8].source[0].id == p[5].destination.id &&
      !p[8].source[0].modifier &&
      /* Other predicates take the general CMP discard. */
      wrong_reference.supported && wrong_reference.semantic_fs_valid &&
      reversed.supported && reversed.semantic_fs_valid &&
      non_alpha.supported && non_alpha.semantic_fs_valid &&
      !no_kill_capability.supported &&
      !no_kill_capability.semantic_fs_valid;
   if (valid)
      for (unsigned lane = 0; lane < 4; lane++)
         valid &= p[6].source[0].swizzle[lane] ==
                     CHROME9_PS_SWIZZLE_W &&
                  p[6].source[1].swizzle[lane] ==
                     p[6].source[1].swizzle[0] &&
                  p[7].source[0].swizzle[lane] == lane &&
                  p[8].source[0].swizzle[lane] == lane;
   if (!valid)
      fprintf(stderr, "ideas logo-shadow compilation failed: "
              "supported=%u semantic=%u ir=%u inputs=%x varyings=%x "
              "samplers=%x uniforms=%x wrong=%u/%u reversed=%u/%u "
              "nonalpha=%u/%u no-kill=%u/%u\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_fs_ir.count, shader.semantic_input_mask,
              shader.semantic_varying_mask, shader.semantic_sampler_mask,
              shader.semantic_uniform_mask,
              wrong_reference.supported, wrong_reference.semantic_fs_valid,
              reversed.supported, reversed.semantic_fs_valid,
              non_alpha.supported, non_alpha.semantic_fs_valid,
              no_kill_capability.supported,
              no_kill_capability.semantic_fs_valid);
   ralloc_free(non_alpha_nir);
   ralloc_free(reversed_nir);
   ralloc_free(negative);
   ralloc_free(nir);
   return valid;
}

static nir_shader *
make_product_alpha_kill_fragment(bool transformed_factor,
                                 bool wrong_sample_lane,
                                 bool reverse_predicate)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-product-alpha-kill");
   nir_variable *var0 = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "var0");
   nir_variable *var1 = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec2_type(), "var1");
   nir_variable *output = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "color");

   var0->data.location = VARYING_SLOT_VAR0;
   var0->data.interpolation = INTERP_MODE_SMOOTH;
   var1->data.location = VARYING_SLOT_VAR1;
   var1->data.interpolation = INTERP_MODE_SMOOTH;
   output->data.location = FRAG_RESULT_COLOR;
   nir_def *varying0 = nir_load_deref(&b, nir_build_deref_var(&b, var0));
   nir_def *varying1 = nir_load_deref(&b, nir_build_deref_var(&b, var1));
   nir_def *coordinate = nir_vec2(&b, nir_channel(&b, varying0, 0),
                                  nir_channel(&b, varying0, 1));
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);

   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coordinate);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *factor = nir_channel(&b, varying1, 1);

   if (transformed_factor)
      factor = nir_fadd(&b, factor, nir_imm_float(&b, 0.0f));
   nir_def *alpha = nir_fmul(
      &b, nir_channel(&b, &tex->def, wrong_sample_lane ? 0 : 3), factor);
   nir_def *predicate = reverse_predicate ?
      nir_flt(&b, nir_imm_float(&b, 0.5f), alpha) :
      nir_flt(&b, alpha, nir_imm_float(&b, 0.5f));

   nir_discard_if(&b, predicate);
   nir_def *lighting = nir_vec3(&b, nir_channel(&b, varying0, 2),
                                nir_channel(&b, varying0, 3),
                                nir_channel(&b, varying1, 0));
   nir_def *rgb = nir_fmul(&b, nir_trim_vector(&b, &tex->def, 3), lighting);
   nir_store_deref(&b, nir_build_deref_var(&b, output),
                   nir_vec4(&b, nir_channel(&b, rgb, 0),
                                 nir_channel(&b, rgb, 1),
                                 nir_channel(&b, rgb, 2), alpha), 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
check_product_alpha_kill_compilation(struct chrome9_context *context)
{
   struct chrome9_shader shader = { 0 }, transformed = { 0 };
   struct chrome9_shader wrong_lane = { 0 }, reversed = { 0 };
   struct chrome9_shader no_kill = { 0 };
   nir_shader *nir = make_product_alpha_kill_fragment(false, false, false);
   nir_shader *transformed_nir =
      make_product_alpha_kill_fragment(true, false, false);
   nir_shader *wrong_lane_nir =
      make_product_alpha_kill_fragment(false, true, false);
   nir_shader *reversed_nir =
      make_product_alpha_kill_fragment(false, false, true);
   unsigned texture = 0, multiply = 0, subtract = 0, kill = 0;
   bool old_fragment_kill = context->fragment_kill;
   bool valid;

   context->fragment_kill = true;
   chrome9_compile_fs_nir(context, &shader, nir);
   chrome9_compile_fs_nir(context, &transformed, transformed_nir);
   chrome9_compile_fs_nir(context, &wrong_lane, wrong_lane_nir);
   chrome9_compile_fs_nir(context, &reversed, reversed_nir);
   context->fragment_kill = false;
   chrome9_compile_fs_nir(context, &no_kill, nir);
   context->fragment_kill = old_fragment_kill;
   for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
      switch (shader.semantic_fs_ir.instruction[i].op) {
      case CHROME9_PS_OPCODE_TEX: texture++; break;
      case CHROME9_PS_OPCODE_MUL: multiply++; break;
      case CHROME9_PS_OPCODE_SUB: subtract++; break;
      case CHROME9_PS_OPCODE_KILL: kill++; break;
      default: break;
      }
   }
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.fragment_program == CHROME9_FS_COMPILED_ALU &&
      shader.semantic_input_mask == 0 &&
      shader.semantic_varying_mask == 3 &&
      shader.semantic_sampler_mask == 1 &&
      shader.semantic_unit_interval_input_mask == (1u << 5) &&
      texture == 1 && multiply == 2 && subtract == 1 && kill == 1 &&
      /* Other predicates take the general CMP discard. */
      transformed.supported && transformed.semantic_fs_valid &&
      wrong_lane.supported && wrong_lane.semantic_fs_valid &&
      reversed.supported && reversed.semantic_fs_valid &&
      !no_kill.supported && !no_kill.semantic_fs_valid;
   if (!valid)
      fprintf(stderr, "product-alpha KILL compilation failed: "
              "supported=%u semantic=%u ir=%u inputs=%x varyings=%x "
              "samplers=%x unit=%08x ops=%u/%u/%u/%u "
              "negative=%u/%u %u/%u %u/%u no-kill=%u/%u\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_fs_ir.count, shader.semantic_input_mask,
              shader.semantic_varying_mask, shader.semantic_sampler_mask,
              shader.semantic_unit_interval_input_mask,
              texture, multiply, subtract, kill,
              transformed.supported, transformed.semantic_fs_valid,
              wrong_lane.supported, wrong_lane.semantic_fs_valid,
              reversed.supported, reversed.semantic_fs_valid,
              no_kill.supported, no_kill.semantic_fs_valid);
   if (!valid)
      nir_print_shader(nir, stderr);
   ralloc_free(reversed_nir);
   ralloc_free(wrong_lane_nir);
   ralloc_free(transformed_nir);
   ralloc_free(nir);
   return valid;
}

static bool
check_product_alpha_unit_interval(void)
{
   static const uint32_t accepted[] = {
      0x00000000, 0x80000000, 0x00000001, 0x3f000000, 0x3f800000,
   };
   static const uint32_t rejected[] = {
      0x80000001, 0xbf800000, 0x3f800001, 0x7f800000, 0xff800000,
      0x7fc00000,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(accepted); i++)
      if (!chrome9_fragment_unit_interval_value(accepted[i]))
         return false;
   for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++)
      if (chrome9_fragment_unit_interval_value(rejected[i]))
         return false;
   return true;
}

enum compact_alu_case {
   COMPACT_FMAD,
   COMPACT_FNEG,
   COMPACT_FNEG_C_SLOT,
   COMPACT_FSAT,
   COMPACT_FSAT_MULTI_USE,
   COMPACT_FSAT_ALIAS,
   COMPACT_SHARED_FSAT,
   COMPACT_SHARED_FSAT_REVERSE,
   COMPACT_FDOT2,
};

static nir_shader *
make_compact_alu_fragment(enum compact_alu_case test)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-compact-alu");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *a = nir_load_ubo(&b, 4, 32, zero, nir_imm_int(&b, 0),
                             .align_mul = 16, .range_base = 0, .range = 48);
   nir_def *c = nir_load_ubo(&b, 4, 32, zero, nir_imm_int(&b, 16),
                             .align_mul = 16, .range_base = 0, .range = 48);
   nir_def *result;

   switch (test) {
   case COMPACT_FMAD:
      /* The native ALU exposes only two distinct constant ports per
       * record; reuse c0 for B so this isolates FMAD opcode lowering. */
      result = nir_fmad(&b, a, c, a);
      break;
   case COMPACT_FNEG:
      result = nir_fadd(&b, nir_fneg(&b, a), c);
      break;
   case COMPACT_FNEG_C_SLOT:
      result = nir_fmul(&b, a, nir_fneg(&b, c));
      break;
   case COMPACT_FSAT:
      result = nir_fsat(&b, nir_fmul(&b, a, c));
      break;
   case COMPACT_FSAT_MULTI_USE: {
      nir_def *product = nir_fmul(&b, a, c);

      result = nir_fadd(&b, product, nir_fsat(&b, product));
      break;
   }
   case COMPACT_FSAT_ALIAS: {
      nir_def *product = nir_fmul(&b, a, c);
      nir_def *alias = nir_mov(&b, product);

      result = nir_fadd(&b, product, nir_fsat(&b, alias));
      break;
   }
   case COMPACT_SHARED_FSAT: {
      nir_def *dot = nir_fdot3(&b, a, c);
      nir_def *inner = nir_fsat(&b, dot);
      nir_def *outer = nir_fsat(&b, nir_fmax(&b, dot,
                                              nir_imm_float(&b, 0.25f)));

      result = nir_vec4(&b, inner, outer, inner, outer);
      break;
   }
   case COMPACT_SHARED_FSAT_REVERSE: {
      nir_def *dot = nir_fdot3(&b, a, c);
      nir_def *maximum = nir_fmax(&b, dot, nir_imm_float(&b, 0.25f));
      nir_def *inner = nir_fsat(&b, dot);
      nir_def *outer = nir_fsat(&b, maximum);

      result = nir_vec4(&b, inner, outer, inner, outer);
      break;
   }
   case COMPACT_FDOT2: {
      nir_def *dot = nir_fdot2(&b, a, c);

      result = nir_vec4(&b, dot, dot, dot, dot);
      break;
   }
   default:
      abort();
   }
   nir_store_output(&b, result, zero, .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   return b.shader;
}

static bool
check_compact_alu_lowering(struct chrome9_context *context)
{
   for (enum compact_alu_case test = COMPACT_FMAD;
        test <= COMPACT_FDOT2; test++) {
      struct chrome9_shader shader = { 0 };
      nir_shader *nir = make_compact_alu_fragment(test);
      unsigned mad = 0, multiply = 0, add = 0, dp3 = 0, dp3_sat = 0;
      unsigned max_sat = 0;
      unsigned saturated_mov = 0, saturated_multiply = 0;
      unsigned negated = 0;

      chrome9_compile_fs_nir(context, &shader, nir);
      for (unsigned i = 0; i < shader.semantic_fs_ir.count; i++) {
         const struct chrome9_ps_instruction *instruction =
            &shader.semantic_fs_ir.instruction[i];

         mad += instruction->op == CHROME9_PS_OPCODE_MAD;
         multiply += instruction->op == CHROME9_PS_OPCODE_MUL;
         add += instruction->op == CHROME9_PS_OPCODE_ADD;
         dp3 += instruction->op == CHROME9_PS_OPCODE_DP3;
         saturated_mov += instruction->op == CHROME9_PS_OPCODE_MOV &&
                          instruction->destination.saturate;
         saturated_multiply += instruction->op == CHROME9_PS_OPCODE_MUL &&
                               instruction->destination.saturate;
         dp3_sat += instruction->op == CHROME9_PS_OPCODE_DP3 &&
                    instruction->destination.saturate;
         max_sat += instruction->op == CHROME9_PS_OPCODE_MAX &&
                    instruction->destination.saturate;
         for (unsigned s = 0; s < 3; s++)
            negated += !!instruction->source[s].modifier;
      }
      bool valid = test == COMPACT_FMAD ?
         !shader.supported && !shader.semantic_fs_valid :
         shader.supported && shader.semantic_fs_valid;
      if (test == COMPACT_FMAD)
         valid &= mad == 0;
      else if (test == COMPACT_FNEG)
         valid &= shader.semantic_fs_ir.count == 2 && add == 1 &&
                  multiply == 1 && negated == 0;
      else if (test == COMPACT_FNEG_C_SLOT)
         valid &= shader.semantic_fs_ir.count == 2 && multiply == 2 &&
                  negated == 0;
      else if (test == COMPACT_FSAT)
         valid &= shader.semantic_fs_ir.count == 2 && multiply == 1 &&
                  shader.semantic_fs_ir.instruction[0].destination.saturate;
      else if (test == COMPACT_FSAT_MULTI_USE)
         valid &= shader.semantic_fs_ir.count == 3 && multiply == 1 &&
                  add == 1 && saturated_mov == 1 && !saturated_multiply;
      else if (test == COMPACT_FSAT_ALIAS)
         valid &= shader.semantic_fs_ir.count == 3 && multiply == 1 &&
                  add == 1 && saturated_mov == 1 && !saturated_multiply;
      else if (test == COMPACT_SHARED_FSAT)
         valid &= dp3_sat == 1 && max_sat == 1;
      else if (test == COMPACT_SHARED_FSAT_REVERSE)
         valid &= dp3_sat == 0 && max_sat == 1 && saturated_mov == 1;
      else {
         const struct chrome9_ps_instruction *dot =
            &shader.semantic_fs_ir.instruction[0];

         valid &= shader.semantic_fs_ir.count == 2 && dp3 == 1 &&
                  dot->op == CHROME9_PS_OPCODE_DP3;
         for (unsigned s = 0; s < 2; s++)
            valid &= dot->source[s].swizzle[0] == CHROME9_PS_SWIZZLE_X &&
                     dot->source[s].swizzle[1] == CHROME9_PS_SWIZZLE_Y &&
                     dot->source[s].swizzle[2] == CHROME9_PS_SWIZZLE_ZERO &&
                     dot->source[s].swizzle[3] == CHROME9_PS_SWIZZLE_ZERO;
      }
      if (!valid) {
         fprintf(stderr, "compact ALU case %u failed: supported=%u "
                 "semantic=%u ir=%u mad=%u mul=%u add=%u neg=%u "
                 "dp3=%u dp3sat=%u maxsat=%u movsat=%u mulsat=%u\n", test,
                 shader.supported,
                 shader.semantic_fs_valid, shader.semantic_fs_ir.count,
                 mad, multiply, add, negated, dp3, dp3_sat, max_sat,
                 saturated_mov, saturated_multiply);
         nir_print_shader(nir, stderr);
         ralloc_free(nir);
         return false;
      }
      ralloc_free(nir);
   }
   return true;
}

/* piglit glsl-max-varyings: val += contribution[i] * v_i over eight
 * varyings.  The optional offline encoder takes at most twelve SSA temps
 * and skips this program, but the kernel-compiled semantic program fits the
 * twelve registers, so the draw stays on the generic hardware path. */
static bool
check_max_varyings_compilation(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-max-varyings");
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   struct chrome9_shader shader = { 0 };
   nir_def *value = NULL;
   unsigned peak = 0;
   bool ok;

   b.shader->info.io_lowered = true;
   for (unsigned i = 0; i < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; i++) {
      nir_def *varying = nir_load_interpolated_input(
         &b, 4, 32, barycentric, nir_imm_int(&b, 0),
         .io_semantics.location = VARYING_SLOT_VAR0 + i);
      nir_def *contribution = nir_load_ubo(
         &b, 1, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 16 * i),
         .align_mul = 16, .range_base = 0,
         .range = 16 * DRM_CHROME9_FRAGMENT_MAX_VARYINGS);
      nir_def *term = nir_fmul(&b, contribution, varying);

      value = value ? nir_fadd(&b, value, term) : term;
   }
   nir_store_output(&b, value, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader, nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   context->fs = &shader;
   ok = shader.supported && shader.semantic_fs_valid &&
        shader.fragment_program == CHROME9_FS_COMPILED_ALU &&
        shader.semantic_varying_mask == 0xff &&
        chrome9_ps_ir_temp_pressure(&shader.semantic_fs_ir, &peak) &&
        peak <= 12 && chrome9_generic_fragment_program(context);
   context->fs = NULL;
   if (!ok)
      fprintf(stderr, "max varyings: supported=%u semantic=%u program=%u "
              "varyings=%x ir=%u peak=%u\n", shader.supported,
              shader.semantic_fs_valid, shader.fragment_program,
              shader.semantic_varying_mask, shader.semantic_fs_ir.count, peak);
   ralloc_free(b.shader);
   return ok;
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
   unsigned failures = 0;

   /* The stub screen has no llvmpipe fallback behind it. */
   setenv("CHROME9_SW_FALLBACK", "0", 1);
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
   /* Run every case so one failure does not hide the others. */
   failures += !check_identity_projector_compilation(&context);
   failures += !check_scalar_literal_packing(&context, false, 17, true);
   failures += !check_scalar_literal_packing(&context, true, 17, true);
   failures += !check_scalar_literal_packing(&context, false, 64, true);
   failures += !check_scalar_literal_packing(&context, false, 65, false);
   failures += !check_reordered_vector_literals(&context);
   failures += !check_packed_alpha_kill(&context);
   failures += !check_vector_conditional_compilation(&context, 1, false);
   failures += !check_vector_conditional_compilation(&context, 2, false);
   failures += !check_vector_conditional_compilation(&context, 3, false);
   failures += !check_vector_conditional_compilation(&context, 4, false);
   failures += !check_vector_conditional_compilation(&context, 1, true);
   failures += !check_nonnegative_select_compilation(&context);
   failures += !check_general_predicate_compilation(&context);
   failures += !check_cel_compilation(&context);
   failures += !check_swizzled_texcoord_bridge(&context);
   failures += !check_equal_zero_select_compilation(&context, 0.0f, true);
   failures += !check_equal_zero_select_compilation(&context, 1.0f, false);
   failures += !check_canonical_uniform_select_context();
   failures += !check_front_face_variant_context();
   failures += !check_saturated_add_rgb_alpha_split(&context);
   failures += !check_position_alias_compilation(&context);
   failures += !check_condition_lowering(&context);
   failures += !check_discard_lowering(&context);
   failures += !check_blinn_phong_integer_power(&context);
   failures += !check_function_medium_sqrt_boundary(&context);
   failures += !check_ideas_logo_shadow_compilation(&context);
   failures += !check_product_alpha_kill_compilation(&context);
   failures += !check_product_alpha_unit_interval();
   failures += !check_compact_alu_lowering(&context);
   failures += !check_separable_blur_compilation(&context);
   failures += !check_max_varyings_compilation(&context);
   if (failures)
      return 1;
   puts("chrome9 primary alpha, texture modulation, packed literals, vector and equality selects, Blinn-Phong shape, function-medium sqrt boundary, ideas logo-shadow, and blur and max-varyings compilation passed");
   return 0;
}
