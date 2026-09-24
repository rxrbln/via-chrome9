/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * Hardware TCL flow control: lower NIR loops and branches with
 * chrome9_tcl_compile, run the kernel vertex IR in a reference
 * interpreter and compare with the NIR semantics.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "chrome9_tcl.h"

static const nir_shader_compiler_options options = {
   .lower_fdiv = true,
   .lower_flrp32 = true,
   .lower_fsat = true,
   .lower_uniforms_to_ubo = true,
   .max_unroll_iterations = 31,
   .no_integers = true,
};

enum shape {
   /* d = x; for (i < cb0.x) d = d * 0.5 + 0.25 */
   SHAPE_LOOP,
   /* if (cb0.y > 0.5) { loop } else d = x + 1 */
   SHAPE_UNIFORM_IF_LOOP,
   /* if (x > 0) { loop } else d = x + 1 */
   SHAPE_DYNAMIC_IF_LOOP,
   /* two loop-carried values swapped every iteration */
   SHAPE_SWAP,
   /* the loop with a literal count of 40, beyond the unroll limit */
   SHAPE_LITERAL_LOOP,
   /* d = x * 0.5 + 0.25 without control flow */
   SHAPE_STRAIGHT,
};

static void
store(nir_builder *b, nir_def *value, gl_varying_slot slot)
{
   nir_store_output(b, value, nir_imm_int(b, 0), .base = slot,
                    .write_mask = 0xf, .src_type = nir_type_float32,
                    .io_semantics.location = slot,
                    .io_semantics.num_slots = 1);
}

static void
counted_loop(nir_builder *b, nir_variable *d, nir_variable *e,
             nir_def *count, bool swap)
{
   nir_variable *i = nir_local_variable_create(b->impl, glsl_int_type(), "i");

   nir_store_var(b, i, nir_imm_int(b, 0), 1);
   nir_push_loop(b);
   {
      nir_def *iv = nir_load_var(b, i);

      nir_break_if(b, nir_ige(b, iv, nir_f2i32(b, count)));
      if (swap) {
         nir_def *dv = nir_load_var(b, d), *ev = nir_load_var(b, e);

         nir_store_var(b, d, ev, 1);
         nir_store_var(b, e, nir_fadd_imm(b, dv, 1.0), 1);
      } else {
         nir_store_var(b, d, nir_fadd_imm(b, nir_fmul_imm(b, nir_load_var(b, d), 0.5), 0.25),
                       1);
      }
      nir_store_var(b, i, nir_iadd_imm(b, iv, 1), 1);
   }
   nir_pop_loop(b, NULL);
}

static nir_shader *
build(enum shape shape)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                   "flow");
   nir_variable *d = nir_local_variable_create(b.impl, glsl_float_type(), "d");
   nir_variable *e = nir_local_variable_create(b.impl, glsl_float_type(), "e");
   nir_def *pos = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 0);
   nir_def *cb = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                   nir_imm_int(&b, 0));
   nir_def *x = nir_channel(&b, pos, 0);

   b.shader->info.io_lowered = true;
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 2;
   b.shader->info.inputs_read = VERT_BIT_GENERIC0;
   b.shader->info.outputs_written = VARYING_BIT_POS | VARYING_BIT_VAR(0);
   b.shader->info.num_ubos = 1;
   nir_store_var(&b, d, x, 1);
   nir_store_var(&b, e, nir_channel(&b, pos, 1), 1);
   switch (shape) {
   case SHAPE_LOOP:
      counted_loop(&b, d, e, nir_channel(&b, cb, 0), false);
      break;
   case SHAPE_SWAP:
      counted_loop(&b, d, e, nir_channel(&b, cb, 0), true);
      break;
   case SHAPE_LITERAL_LOOP:
      counted_loop(&b, d, e, nir_imm_float(&b, 40.0), false);
      break;
   case SHAPE_STRAIGHT:
      nir_store_var(&b, d, nir_fadd_imm(&b, nir_fmul_imm(&b, x, 0.5), 0.25),
                    1);
      break;
   default:
      nir_push_if(&b, shape == SHAPE_UNIFORM_IF_LOOP ?
                  nir_flt_imm(&b, nir_channel(&b, cb, 1), 0.5) :
                  nir_flt_imm(&b, x, 0.0));
      nir_store_var(&b, d, nir_fadd_imm(&b, x, 1.0), 1);
      nir_push_else(&b, NULL);
      counted_loop(&b, d, e, nir_channel(&b, cb, 0), false);
      nir_pop_if(&b, NULL);
      break;
   }
   store(&b, pos, VARYING_SLOT_POS);
   store(&b, nir_vec4(&b, nir_load_var(&b, d), nir_load_var(&b, e),
                      nir_imm_float(&b, 0.0), nir_imm_float(&b, 1.0)),
         VARYING_SLOT_VAR0);
   return b.shader;
}

/* NIR semantics of build(). */
static void
reference(enum shape shape, const float *pos, const float *cb, float *out)
{
   const int count = shape == SHAPE_LITERAL_LOOP ? 40 :
                     shape == SHAPE_STRAIGHT ? 1 : (int)cb[0];
   float d = pos[0], e = pos[1];
   bool loop = shape == SHAPE_LOOP || shape == SHAPE_SWAP ||
               shape == SHAPE_LITERAL_LOOP || shape == SHAPE_STRAIGHT ||
               (shape == SHAPE_UNIFORM_IF_LOOP ? !(cb[1] < 0.5f) :
                !(pos[0] < 0.0f));

   if (!loop)
      d = pos[0] + 1.0f;
   for (int i = 0; loop && i < count; i++) {
      if (shape == SHAPE_SWAP) {
         float t = d;

         d = e;
         e = t + 1.0f;
      } else {
         d = d * 0.5f + 0.25f;
      }
   }
   out[0] = d;
   out[1] = e;
}

struct machine {
   float temp[DRM_CHROME9_VERTEX_MAX_TEMPS][4];
   const float (*input)[4];
   const float (*constant)[4];
   uint32_t bools;
   const uint8_t (*ints)[4];
   float varying[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   unsigned executed;
};

static float
lane(const struct machine *m, const struct drm_chrome9_vertex_source *s,
     unsigned c)
{
   const uint8_t sel = s->swizzle[c];
   float v;

   if (sel == DRM_CHROME9_FRAGMENT_SWIZZLE_ONE)
      v = 1.0f;
   else if (sel == DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO)
      v = 0.0f;
   else if (s->file == DRM_CHROME9_VERTEX_FILE_INPUT)
      v = m->input[s->index][sel];
   else if (s->file == DRM_CHROME9_VERTEX_FILE_CONSTANT)
      v = m->constant[s->index][sel];
   else
      v = m->temp[s->index][sel];
   return s->modifier == DRM_CHROME9_FRAGMENT_MOD_NEG ? -v : v;
}

static unsigned
skip_region(const struct drm_chrome9_vertex_instruction *in, unsigned pc,
            unsigned count, bool stop_at_else)
{
   unsigned depth = 0;

   for (; pc < count; pc++) {
      const uint8_t op = in[pc].op;

      if (op == DRM_CHROME9_VERTEX_OP_REP || op == DRM_CHROME9_VERTEX_OP_IF)
         depth++;
      else if (depth && (op == DRM_CHROME9_VERTEX_OP_ENDREP ||
                         op == DRM_CHROME9_VERTEX_OP_ENDIF))
         depth--;
      else if (!depth && (op == DRM_CHROME9_VERTEX_OP_ENDIF ||
                          op == DRM_CHROME9_VERTEX_OP_ENDREP ||
                          (stop_at_else && op == DRM_CHROME9_VERTEX_OP_ELSE)))
         return pc;
   }
   return pc;
}

/* Structured execution of [pc, end); returns the index it stopped at. */
static unsigned
run(struct machine *m, const struct drm_chrome9_vertex_instruction *in,
    unsigned pc, unsigned count)
{
   while (pc < count) {
      const struct drm_chrome9_vertex_instruction *i = &in[pc];
      float r[4], s[3][4];

      switch (i->op) {
      case DRM_CHROME9_VERTEX_OP_ELSE:
      case DRM_CHROME9_VERTEX_OP_ENDIF:
      case DRM_CHROME9_VERTEX_OP_ENDREP:
         return pc;
      case DRM_CHROME9_VERTEX_OP_REP: {
         const unsigned trips = m->ints[i->src[0].index][0];
         unsigned end = skip_region(in, pc + 1, count, false);

         for (unsigned t = 0; t < trips; t++)
            run(m, in, pc + 1, end);
         pc = end + 1;
         continue;
      }
      case DRM_CHROME9_VERTEX_OP_IF: {
         const bool taken = i->src[0].file == DRM_CHROME9_VERTEX_FILE_BOOL ?
            m->bools >> i->src[0].index & 1 : lane(m, &i->src[0], 0) != 0.0f;
         unsigned mid = skip_region(in, pc + 1, count, true);
         unsigned end = in[mid].op == DRM_CHROME9_VERTEX_OP_ELSE ?
                        skip_region(in, mid + 1, count, false) : mid;

         if (taken)
            run(m, in, pc + 1, mid);
         else if (mid != end)
            run(m, in, mid + 1, end);
         pc = end + 1;
         continue;
      }
      default:
         break;
      }
      m->executed++;
      for (unsigned k = 0; k < 3; k++)
         for (unsigned c = 0; c < 4; c++)
            s[k][c] = lane(m, &i->src[k], c);
      for (unsigned c = 0; c < 4; c++) {
         switch (i->op) {
         case DRM_CHROME9_VERTEX_OP_MOV: r[c] = s[0][c]; break;
         case DRM_CHROME9_VERTEX_OP_ADD: r[c] = s[0][c] + s[1][c]; break;
         case DRM_CHROME9_VERTEX_OP_SUB: r[c] = s[0][c] - s[1][c]; break;
         case DRM_CHROME9_VERTEX_OP_MUL: r[c] = s[0][c] * s[1][c]; break;
         case DRM_CHROME9_VERTEX_OP_MAD:
            r[c] = s[0][c] * s[1][c] + s[2][c];
            break;
         case DRM_CHROME9_VERTEX_OP_DP3:
            r[c] = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2];
            break;
         case DRM_CHROME9_VERTEX_OP_DP4:
            r[c] = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                   s[0][2] * s[1][2] + s[0][3] * s[1][3];
            break;
         case DRM_CHROME9_VERTEX_OP_DPH:
            r[c] = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                   s[0][2] * s[1][2] + s[1][3];
            break;
         case DRM_CHROME9_VERTEX_OP_MIN: r[c] = fminf(s[0][c], s[1][c]); break;
         case DRM_CHROME9_VERTEX_OP_MAX: r[c] = fmaxf(s[0][c], s[1][c]); break;
         case DRM_CHROME9_VERTEX_OP_ABS: r[c] = fabsf(s[0][c]); break;
         case DRM_CHROME9_VERTEX_OP_SGE: r[c] = s[0][c] >= s[1][c]; break;
         case DRM_CHROME9_VERTEX_OP_SLT: r[c] = s[0][c] < s[1][c]; break;
         case DRM_CHROME9_VERTEX_OP_SGN:
            r[c] = s[0][c] > 0 ? 1.0f : s[0][c] < 0 ? -1.0f : 0.0f;
            break;
         case DRM_CHROME9_VERTEX_OP_FRC: r[c] = s[0][c] - floorf(s[0][c]); break;
         case DRM_CHROME9_VERTEX_OP_FLR: r[c] = floorf(s[0][c]); break;
         case DRM_CHROME9_VERTEX_OP_RCP: r[c] = 1.0f / s[0][c]; break;
         case DRM_CHROME9_VERTEX_OP_RSQ: r[c] = 1.0f / sqrtf(fabsf(s[0][c])); break;
         case DRM_CHROME9_VERTEX_OP_EXP2: r[c] = exp2f(s[0][c]); break;
         default: r[c] = log2f(fabsf(s[0][c])); break;
         }
         if (i->dst.modifier & DRM_CHROME9_FRAGMENT_DEST_SAT)
            r[c] = fminf(fmaxf(r[c], 0.0f), 1.0f);
      }
      for (unsigned c = 0; c < 4; c++) {
         if (!(i->dst.writemask & (1u << c)))
            continue;
         if (i->dst.file == DRM_CHROME9_VERTEX_DEST_TEMP)
            m->temp[i->dst.index][c] = r[c];
         else if (i->dst.file == DRM_CHROME9_VERTEX_DEST_VARYING)
            m->varying[i->dst.index][c] = r[c];
      }
      pc++;
   }
   return pc;
}

static int
check(const char *name, enum shape shape, const struct chrome9_tcl_flow *flow,
      bool expect_supported, unsigned expect_reps, unsigned expect_bools)
{
   static const float counts[] = { 0, 1, 3, 5, 31, 32, 40, 255 };
   static const float xs[] = { -0.75f, 0.0f, 0.375f, 2.0f };
   nir_shader *nir = build(shape);
   struct chrome9_tcl_program *p = calloc(1, sizeof(*p));
   int failures = 0;

   chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, flow, p);
   if (p->supported != expect_supported || (p->supported &&
       (p->rep_count != expect_reps || p->bool_count != expect_bools))) {
      fprintf(stderr, "%s: supported %d reps %u bools %u\n", name,
              p->supported, p->rep_count, p->bool_count);
      failures++;
      goto out;
   }
   for (unsigned n = 0; p->supported && n < ARRAY_SIZE(counts); n++) {
      for (unsigned k = 0; k < ARRAY_SIZE(xs) * 2; k++) {
         float cb[4][4] = { { counts[n], k & 1 ? 1.0f : 0.0f, 0, 0 } };
         float constant[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4] = { 0 };
         const float input[1][4] = { { xs[k / 2], 0.5f + k, 0, 1 } };
         struct machine m = { .input = input, .constant = constant };
         uint8_t ints[DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS][4];
         float want[2];

         memcpy(constant, cb, p->uniform_rows * sizeof(cb[0]));
         memcpy(constant + p->uniform_rows, p->immediates,
                p->immediate_count * sizeof(p->immediates[0]));
         if (!chrome9_tcl_flow_constants(p, (const uint8_t *)cb, sizeof(cb),
                                         &m.bools, ints)) {
            fprintf(stderr, "%s: flow constants for %g\n", name, counts[n]);
            failures++;
            continue;
         }
         m.ints = ints;
         run(&m, p->instructions, 0, p->instruction_count);
         reference(shape, input[0], cb[0], want);
         if (fabsf(m.varying[0][0] - want[0]) > 1e-5f ||
             fabsf(m.varying[0][1] - want[1]) > 1e-5f) {
            fprintf(stderr, "%s: count %g x %g: %g %g, want %g %g\n", name,
                    counts[n], input[0][0], m.varying[0][0], m.varying[0][1],
                    want[0], want[1]);
            failures++;
         }
      }
   }
   /* 256 trips do not fit i#.x. */
   if (p->supported && p->rep_count && shape != SHAPE_LITERAL_LOOP) {
      float cb[4] = { 256.0f, 1.0f, 0, 0 };
      uint8_t ints[DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS][4];
      uint32_t bools;

      if (chrome9_tcl_flow_constants(p, (const uint8_t *)cb, sizeof(cb),
                                     &bools, ints)) {
         fprintf(stderr, "%s: 256 trips accepted\n", name);
         failures++;
      }
   }
   /* CHROME9_TCL_FLOW_TEST_DUMP=1 lists the kernel IR. */
   for (unsigned i = 0; getenv("CHROME9_TCL_FLOW_TEST_DUMP") &&
                        i < p->instruction_count; i++) {
      const struct drm_chrome9_vertex_instruction *in = &p->instructions[i];

      printf("  %2u op%-2u d%u.%u/%x", i, in->op, in->dst.file, in->dst.index,
             in->dst.writemask);
      for (unsigned k = 0; k < 3; k++)
         printf("  %c%u.%u.%u%u%u%u", in->src[k].modifier ? '-' : ' ',
                in->src[k].file, in->src[k].index, in->src[k].swizzle[0],
                in->src[k].swizzle[1], in->src[k].swizzle[2],
                in->src[k].swizzle[3]);
      printf("\n");
   }
   /* CHROME9_TCL_FLOW_TEST_EXPORT=file appends the programs for a kernel
    * compiler run: depth, dynamic, count, inputs, constants, records. */
   if (p->supported && getenv("CHROME9_TCL_FLOW_TEST_EXPORT")) {
      FILE *f = fopen(getenv("CHROME9_TCL_FLOW_TEST_EXPORT"), "ab");
      const uint32_t head[5] = {
         flow->max_depth, flow->dynamic, p->instruction_count,
         p->input_count, p->uniform_rows + p->immediate_count,
      };

      if (!f || fwrite(head, sizeof(head), 1, f) != 1 ||
          fwrite(p->instructions, sizeof(p->instructions[0]),
                 p->instruction_count, f) != p->instruction_count)
         failures++;
      if (f)
         fclose(f);
   }
   printf("%-24s %s: %u instructions, %u reps, %u bools\n", name,
          failures ? "FAIL" : "ok", p->instruction_count, p->rep_count,
          p->bool_count);
out:
   chrome9_tcl_program_fini(p);
   free(p);
   ralloc_free(nir);
   return failures;
}

/* Flow control must not change straight-line programs. */
static int
same_program(enum shape shape, const struct chrome9_tcl_flow *a,
             const struct chrome9_tcl_flow *b)
{
   struct chrome9_tcl_program *p = calloc(2, sizeof(*p));
   nir_shader *nir = build(shape);
   int failed;

   chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, a, &p[0]);
   chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, b, &p[1]);
   failed = !p[0].supported || !p[1].supported ||
            p[0].instruction_count != p[1].instruction_count ||
            memcmp(p[0].instructions, p[1].instructions,
                   p[0].instruction_count * sizeof(p[0].instructions[0]));
   printf("%-24s %s\n", "same straight line", failed ? "FAIL" : "ok");
   chrome9_tcl_program_fini(&p[0]);
   chrome9_tcl_program_fini(&p[1]);
   free(p);
   ralloc_free(nir);
   return failed;
}

int
main(void)
{
   static const struct chrome9_tcl_flow none = { 0 };
   static const struct chrome9_tcl_flow depth1 = { 1, false };
   static const struct chrome9_tcl_flow depth2 = { 2, false };
   static const struct chrome9_tcl_flow dynamic2 = { 2, true };
   int failures = 0;

   glsl_type_singleton_init_or_ref();
   failures += check("loop, no flow", SHAPE_LOOP, &none, false, 0, 0);
   failures += check("loop", SHAPE_LOOP, &depth1, true, 1, 0);
   failures += check("swap", SHAPE_SWAP, &depth1, true, 1, 0);
   failures += check("uniform if, depth 1", SHAPE_UNIFORM_IF_LOOP, &depth1,
                     false, 0, 0);
   failures += check("uniform if", SHAPE_UNIFORM_IF_LOOP, &depth2, true, 1, 1);
   failures += check("dynamic if, static", SHAPE_DYNAMIC_IF_LOOP, &depth2,
                     false, 0, 0);
   failures += check("dynamic if", SHAPE_DYNAMIC_IF_LOOP, &dynamic2, true, 1,
                     0);
   failures += check("literal 40 loop", SHAPE_LITERAL_LOOP, &depth1, true, 1,
                     0);
   failures += check("straight line", SHAPE_STRAIGHT, &depth1, true, 0, 0);
   failures += same_program(SHAPE_STRAIGHT, &none, &depth1);
   glsl_type_singleton_decref();
   printf("chrome9 TCL flow tests %s\n", failures ? "FAILED" : "passed");
   return failures ? 1 : 0;
}
