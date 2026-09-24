/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_public.h"
#include "chrome9_specialise.h"

#include <errno.h>
#include <fcntl.h>
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
            valid = wrong_lane ? !shader.semantic_fs_valid :
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
      valid = wrong ? !shader.semantic_fs_valid :
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
   valid = expect_supported ?
      shader.supported && shader.semantic_fs_valid &&
      shader.native_fs_valid &&
      shader.semantic_equal_zero_input_mask == (1u << 6) &&
      abs_count == 1 && sub_count == 1 && cmp_count == 3 :
      !shader.supported && !shader.semantic_fs_valid;
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
          !instruction->destination.saturate)
         add_count++;
      if (instruction->op == CHROME9_PS_OPCODE_MOV &&
          instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
          instruction->destination.saturate &&
          (instruction->destination.writemask & 0x7) == 0x7)
         sat_count++;
   }
   valid = shader.supported && shader.semantic_fs_valid &&
      shader.semantic_varying_mask == 3 && add_count == 1 && sat_count == 1;
   if (!valid)
      fprintf(stderr, "saturated ADD RGB/alpha split failed: "
              "supported=%u semantic=%u varyings=%x add=%u sat=%u\n",
              shader.supported, shader.semantic_fs_valid,
              shader.semantic_varying_mask, add_count, sat_count);
   ralloc_free(b.shader);
   return valid;
}

static bool
check_per_lane_conditional_rejected(struct chrome9_context *context)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "chrome9-per-lane-conditional");
   struct chrome9_shader shader = { 0 };
   nir_def *barycentric = nir_load_barycentric_pixel(
      &b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *color = nir_load_interpolated_input(
      &b, 4, 32, barycentric, nir_imm_int(&b, 0),
      .io_semantics.location = VARYING_SLOT_COL0);
   nir_def *condition = nir_fge(
      &b, nir_ffract(&b, nir_trim_vector(&b, color, 3)),
      nir_imm_vec3(&b, 0.5f, 0.5f, 0.5f));
   nir_def *selected = nir_bcsel(
      &b, condition,
      nir_imm_vec3(&b, 0.25f, 0.5f, 0.75f),
      nir_imm_vec3(&b, 0.75f, 0.25f, 0.5f));
   bool rejected;

   b.shader->info.io_lowered = true;
   nir_store_output(&b, nir_vec4(&b,
      nir_channel(&b, selected, 0), nir_channel(&b, selected, 1),
      nir_channel(&b, selected, 2), nir_imm_float(&b, 1.0f)),
      nir_imm_int(&b, 0), .write_mask = 0xf,
      .io_semantics.location = FRAG_RESULT_COLOR);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_recompute_io_bases(b.shader,
                          nir_var_shader_in | nir_var_shader_out);
   chrome9_compile_fs_nir(context, &shader, b.shader);
   rejected = !shader.supported;
   if (!rejected)
      fputs("per-lane conditional was miscompiled as a scalar broadcast\n",
            stderr);
   ralloc_free(b.shader);
   return rejected;
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
      !wrong_reference.supported && !wrong_reference.semantic_fs_valid &&
      !reversed.supported && !reversed.semantic_fs_valid &&
      !non_alpha.supported && !non_alpha.semantic_fs_valid &&
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
   if (!check_identity_projector_compilation(&context) ||
       !check_scalar_literal_packing(&context, false, 17, true) ||
       !check_scalar_literal_packing(&context, true, 17, true) ||
       !check_scalar_literal_packing(&context, false, 64, true) ||
       !check_scalar_literal_packing(&context, false, 65, false) ||
       !check_reordered_vector_literals(&context) ||
       !check_packed_alpha_kill(&context) ||
       !check_vector_conditional_compilation(&context, 1, false) ||
       !check_vector_conditional_compilation(&context, 2, false) ||
       !check_vector_conditional_compilation(&context, 3, false) ||
       !check_vector_conditional_compilation(&context, 4, false) ||
       !check_vector_conditional_compilation(&context, 1, true) ||
       !check_equal_zero_select_compilation(&context, 0.0f, true) ||
       !check_equal_zero_select_compilation(&context, 1.0f, false) ||
       !check_canonical_uniform_select_context() ||
       !check_front_face_variant_context() ||
       !check_saturated_add_rgb_alpha_split(&context) ||
       !check_per_lane_conditional_rejected(&context) ||
       !check_blinn_phong_integer_power(&context) ||
       !check_function_medium_sqrt_boundary(&context) ||
       !check_ideas_logo_shadow_compilation(&context) ||
       !check_separable_blur_compilation(&context))
      return 1;
   puts("chrome9 primary alpha, texture modulation, packed literals, vector and equality selects, Blinn-Phong shape, function-medium sqrt boundary, ideas logo-shadow, and blur compilation passed");
   return 0;
}
