/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */

#include "chrome9_context.h"
#include "chrome9_drm.h"
#include "chrome9_specialise.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "nir.h"
#include "nir_builder.h"
#include "pipe/p_screen.h"

/* Exercise the real context callbacks without opening a DRM node.  No BOs
 * or GPU submissions are permitted by this deliberately narrow ioctl stub. */
#define CHROME9_TEST_FD 1909
static unsigned unexpected_ioctls;

int
ioctl(int fd, unsigned long request, ...)
{
   va_list arguments;
   void *payload;

   va_start(arguments, request);
   payload = va_arg(arguments, void *);
   va_end(arguments);
   if (fd == CHROME9_TEST_FD && request == DRM_IOCTL_CHROME9_GET_PARAM) {
      struct drm_chrome9_get_param *param = payload;

      if (param && param->param == DRM_CHROME9_PARAM_CAPABILITIES) {
         param->value = DRM_CHROME9_CAP_DRAW_TRIANGLE |
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
       !check_packed_alpha_kill(&context) ||
       !check_vector_conditional_compilation(&context, 1, false) ||
       !check_vector_conditional_compilation(&context, 2, false) ||
       !check_vector_conditional_compilation(&context, 3, false) ||
       !check_vector_conditional_compilation(&context, 4, false) ||
       !check_vector_conditional_compilation(&context, 1, true) ||
       !check_canonical_uniform_select_context() ||
       !check_per_lane_conditional_rejected(&context) ||
       !check_blinn_phong_integer_power(&context) ||
       !check_separable_blur_compilation(&context))
      return 1;
   puts("chrome9 primary alpha, texture modulation, packed literals, vector select, Blinn-Phong shape, and blur compilation passed");
   return 0;
}
