/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * Hardware TCL flow control and indirect uniform reads: lower NIR with
 * chrome9_tcl_compile, run the kernel vertex IR in a reference
 * interpreter and compare with the NIR semantics.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "chrome9_specialise.h"
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
   /* for (i < cb0.x) for (j < cb0.x) d = d * 0.5 + 0.25: one loop counter */
   SHAPE_NESTED_LOOP,
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
   case SHAPE_NESTED_LOOP: {
      nir_variable *j = nir_local_variable_create(b.impl, glsl_int_type(), "j");

      nir_store_var(&b, j, nir_imm_int(&b, 0), 1);
      nir_push_loop(&b);
      {
         nir_def *jv = nir_load_var(&b, j);

         nir_break_if(&b, nir_ige(&b, jv, nir_f2i32(&b, nir_channel(&b, cb, 0))));
         counted_loop(&b, d, e, nir_channel(&b, cb, 0), false);
         nir_store_var(&b, j, nir_iadd_imm(&b, jv, 1), 1);
      }
      nir_pop_loop(&b, NULL);
      break;
   }
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
   /* a0 and the kernel's ARL clamp per lane; constant rows of the draw. */
   float a0[4];
   float low[4];
   float high[4];
   unsigned constant_count;
   bool outside;
   uint32_t bools;
   const uint8_t (*ints)[4];
   float varying[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
   unsigned executed;
};

static float
lane(struct machine *m, const struct drm_chrome9_vertex_source *s,
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
   else if (s->file == DRM_CHROME9_VERTEX_FILE_CONSTANT && s->relative) {
      const int row = s->index + (int)m->a0[s->relative - 1];

      m->outside |= row < 0 || row >= (int)m->constant_count;
      v = m->outside ? 0.0f : m->constant[row][sel];
   } else if (s->file == DRM_CHROME9_VERTEX_FILE_CONSTANT)
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
         /* Silicon tests b# of the source register number (rung vsdyn). */
         const bool taken = m->bools >> i->src[0].index & 1;
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
         case DRM_CHROME9_VERTEX_OP_ARL:
            r[c] = floorf(fminf(fmaxf(s[0][c], m->low[c]), m->high[c]));
            break;
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
         else if (i->dst.file == DRM_CHROME9_VERTEX_DEST_ADDRESS)
            m->a0[c] = r[c];
         else if (i->dst.file == DRM_CHROME9_VERTEX_DEST_VARYING)
            m->varying[i->dst.index][c] = r[c];
      }
      pc++;
   }
   return pc;
}

/* CHROME9_TCL_FLOW_TEST_DUMP=1 lists the kernel IR; [aN] is a0-relative. */
static void
dump(const struct chrome9_tcl_program *p)
{
   for (unsigned i = 0; getenv("CHROME9_TCL_FLOW_TEST_DUMP") &&
                        i < p->instruction_count; i++) {
      const struct drm_chrome9_vertex_instruction *in = &p->instructions[i];

      printf("  %2u op%-2u d%u.%u/%x", i, in->op, in->dst.file, in->dst.index,
             in->dst.writemask);
      for (unsigned k = 0; k < 3; k++)
         printf("  %c%u.%u%s.%u%u%u%u", in->src[k].modifier ? '-' : ' ',
                in->src[k].file, in->src[k].index,
                in->src[k].relative ? (const char *[]){ "[a0]", "[a1]",
                   "[a2]", "[a3]" }[in->src[k].relative - 1] : "",
                in->src[k].swizzle[0], in->src[k].swizzle[1],
                in->src[k].swizzle[2], in->src[k].swizzle[3]);
      printf("\n");
   }
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
                       NULL, NULL, flow, NULL, p);
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
   dump(p);
   /* CHROME9_TCL_FLOW_TEST_EXPORT=file appends the programs for a kernel
    * compiler run: depth, dynamic, count, inputs, constants, records. */
   if (p->supported && getenv("CHROME9_TCL_FLOW_TEST_EXPORT")) {
      FILE *f = fopen(getenv("CHROME9_TCL_FLOW_TEST_EXPORT"), "ab");
      const uint32_t head[5] = {
         flow->max_depth, 0u, p->instruction_count,
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
                       NULL, NULL, a, NULL, &p[0]);
   chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, b, NULL, &p[1]);
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

enum relative_shape {
   /* c[1 + int(x)] */
   REL_PLAIN,
   /* c[1 + int(x)] + c[1 + int(x) + 1]: one lane, two bases */
   REL_OFFSET,
   /* c[1 + int(x)] + c[1 + int(y)]: two lanes */
   REL_TWO,
   /* rows 0..3 of mat4 element int(x) at 1: one lane, bases 1..4 */
   REL_MAT,
   /* d = x; for (i < cb0.x) d += c[1 + int(y)].x */
   REL_LOOP,
   /* five index values: more than a0 has lanes */
   REL_FIVE,
   /* prog_to_nir ARL: c[3 + floor(x)], floor lowered to x - fract(x) */
   REL_ARB,
   /* c[1 + int(abs(x))]: a non-negative fraction needs no truncation */
   REL_ABS,
};

static nir_def *
relative_load(nir_builder *b, nir_def *index, unsigned base)
{
   return nir_load_ubo_vec4(b, 4, 32, nir_imm_int(b, 0), index, .base = base);
}

static nir_shader *
build_relative(enum relative_shape shape)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                   "relative");
   nir_def *pos = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 0);
   nir_def *i = nir_f2i32(&b, nir_channel(&b, pos, 0));
   nir_def *v = relative_load(&b, i, 1);

   b.shader->info.io_lowered = true;
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 2;
   b.shader->num_uniforms = shape == REL_MAT ? 17 : 9;
   b.shader->info.inputs_read = VERT_BIT_GENERIC0;
   b.shader->info.outputs_written = VARYING_BIT_POS | VARYING_BIT_VAR(0);
   b.shader->info.num_ubos = 1;
   switch (shape) {
   case REL_OFFSET:
      v = nir_fadd(&b, v, relative_load(&b, nir_iadd_imm(&b, i, 1), 1));
      break;
   case REL_TWO:
      v = nir_fadd(&b, v, relative_load(&b, nir_f2i32(&b, nir_channel(&b, pos, 1)),
                                        1));
      break;
   case REL_MAT: {
      nir_def *row = nir_imul_imm(&b, i, 4);

      v = relative_load(&b, row, 1);
      for (unsigned k = 1; k < 4; k++)
         v = nir_fadd(&b, v, nir_fmul_imm(&b, relative_load(&b,
                      nir_iadd_imm(&b, row, k), 1), 1 << k));
      break;
   }
   case REL_LOOP: {
      nir_variable *d = nir_local_variable_create(b.impl, glsl_vec4_type(), "d");
      nir_variable *n = nir_local_variable_create(b.impl, glsl_int_type(), "n");
      nir_def *cb = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                      nir_imm_int(&b, 0));
      nir_def *j = nir_f2i32(&b, nir_channel(&b, pos, 1));

      nir_store_var(&b, d, pos, 0xf);
      nir_store_var(&b, n, nir_imm_int(&b, 0), 1);
      nir_push_loop(&b);
      {
         nir_def *nv = nir_load_var(&b, n);

         nir_break_if(&b, nir_ige(&b, nv, nir_f2i32(&b, nir_channel(&b, cb, 0))));
         nir_store_var(&b, d, nir_fadd(&b, nir_load_var(&b, d),
                                       relative_load(&b, j, 1)), 0xf);
         nir_store_var(&b, n, nir_iadd_imm(&b, nv, 1), 1);
      }
      nir_pop_loop(&b, NULL);
      v = nir_load_var(&b, d);
      break;
   }
   case REL_ARB:
      v = relative_load(&b, nir_iadd_imm(&b, nir_f2i32(&b,
                        nir_fsub(&b, nir_channel(&b, pos, 0),
                                 nir_ffract(&b, nir_channel(&b, pos, 0)))), 3),
                        0);
      break;
   case REL_ABS:
      v = relative_load(&b, nir_f2i32(&b, nir_fabs(&b, nir_channel(&b, pos, 0))),
                        1);
      break;
   case REL_FIVE:
      for (unsigned k = 1; k < 5; k++)
         v = nir_fadd(&b, v, relative_load(&b, nir_f2i32(&b,
                      k < 4 ? nir_channel(&b, pos, k) :
                      nir_fmul_imm(&b, nir_channel(&b, pos, 0), 0.5)), 1));
      break;
   default:
      break;
   }
   store(&b, pos, VARYING_SLOT_POS);
   store(&b, v, VARYING_SLOT_VAR0);
   /* As chrome9_finalize_nir: row * 4 addresses become float products. */
   chrome9_nir_lower_ubo_index_shifts(b.shader);
   return b.shader;
}

static void
relative_reference(enum relative_shape shape, const float *pos,
                   const float (*cb)[4], float *out)
{
   const int i = (int)pos[0], j = (int)pos[1];

   for (unsigned c = 0; c < 4; c++) {
      switch (shape) {
      case REL_OFFSET:
         out[c] = cb[1 + i][c] + cb[2 + i][c];
         break;
      case REL_TWO:
         out[c] = cb[1 + i][c] + cb[1 + j][c];
         break;
      case REL_MAT:
         out[c] = cb[1 + 4 * i][c] + 2 * cb[2 + 4 * i][c] +
                  4 * cb[3 + 4 * i][c] + 8 * cb[4 + 4 * i][c];
         break;
      case REL_LOOP:
         out[c] = pos[c] + cb[0][0] * cb[1 + j][c];
         break;
      case REL_ARB:
         out[c] = cb[3 + (int)floorf(pos[0])][c];
         break;
      case REL_ABS:
         out[c] = cb[1 + (int)fabsf(pos[0])][c];
         break;
      default:
         out[c] = cb[1 + i][c];
         break;
      }
   }
}

/* The kernel's ARL clamp: per lane the rows every relative read keeps. */
static void
relative_bounds(const struct chrome9_tcl_program *p, struct machine *m)
{
   int low[4] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
   int high[4] = { INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX };

   for (unsigned i = 0; i < p->instruction_count; i++)
      for (unsigned k = 0; k < 3; k++) {
         const struct drm_chrome9_vertex_source *s = &p->instructions[i].src[k];

         if (s->file != DRM_CHROME9_VERTEX_FILE_CONSTANT || !s->relative)
            continue;
         low[s->relative - 1] = MAX2(low[s->relative - 1], -(int)s->index);
         high[s->relative - 1] = MIN2(high[s->relative - 1],
                                      (int)m->constant_count - 1 - s->index);
      }
   for (unsigned c = 0; c < 4; c++) {
      m->low[c] = low[c];
      m->high[c] = high[c];
   }
}

static int
check_relative(const char *name, enum relative_shape shape,
               const struct chrome9_tcl_flow *flow, bool expect_supported,
               unsigned expect_pad)
{
   static const float xs[] = { 0.0f, 1.0f, 2.75f, 5.0f, 6.0f, -0.5f };
   nir_shader *nir = build_relative(shape);
   struct chrome9_tcl_program *p = calloc(1, sizeof(*p));
   const unsigned rows = nir->num_uniforms;
   unsigned arls = 0;
   int failures = 0;

   chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, flow, NULL, p);
   for (unsigned i = 0; i < p->instruction_count; i++)
      arls += p->instructions[i].op == DRM_CHROME9_VERTEX_OP_ARL;
   if (p->supported != expect_supported ||
       (!p->supported && !p->indirect) ||
       (p->supported && (p->constant_pad != expect_pad || !arls ||
                         chrome9_tcl_constant_count(p) + 2 >
                         DRM_CHROME9_VERTEX_MAX_CONSTANTS))) {
      fprintf(stderr, "%s: supported %d indirect %d pad %u arls %u\n", name,
              p->supported, p->indirect, p->constant_pad, arls);
      failures++;
      goto out;
   }
   for (unsigned n = 0; p->supported && n < ARRAY_SIZE(xs); n++) {
      for (unsigned k = 0; k < ARRAY_SIZE(xs); k++) {
         float cb[17][4] = { { 3.0f } };
         float constant[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4] = { 0 };
         const float input[1][4] = { { xs[n], xs[k], 2.0f, 1.0f } };
         struct machine m = {
            .input = input, .constant = constant,
            .constant_count = chrome9_tcl_constant_count(p),
         };
         uint8_t ints[DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS][4];
         float want[4];

         /* Element 5 + 1 is the last row of the eight-row array. */
         if ((shape == REL_OFFSET && xs[n] > 5.0f) ||
             (shape == REL_MAT && xs[n] > 3.0f) ||
             (shape == REL_ARB && xs[n] > 5.0f))
            continue;
         for (unsigned r = 1; r < rows; r++)
            for (unsigned c = 0; c < 4; c++)
               cb[r][c] = 16 * r + c + 0.5f;
         memcpy(constant + p->constant_pad, cb, p->uniform_rows * sizeof(cb[0]));
         memcpy(constant + p->constant_pad + p->uniform_rows, p->immediates,
                p->immediate_count * sizeof(p->immediates[0]));
         if (!chrome9_tcl_flow_constants(p, (const uint8_t *)cb, sizeof(cb),
                                         &m.bools, ints)) {
            failures++;
            continue;
         }
         m.ints = ints;
         relative_bounds(p, &m);
         run(&m, p->instructions, 0, p->instruction_count);
         relative_reference(shape, input[0], cb, want);
         for (unsigned c = 0; c < 4; c++) {
            if (!m.outside && fabsf(m.varying[0][c] - want[c]) <= 1e-4f)
               continue;
            fprintf(stderr, "%s: x %g y %g lane %u: %g, want %g%s\n", name,
                    xs[n], xs[k], c, m.varying[0][c], want[c],
                    m.outside ? " (outside the rows)" : "");
            failures++;
            break;
         }
      }
   }
   dump(p);
   /* CHROME9_TCL_FLOW_TEST_EXPORT appends the program as in check(). */
   if (p->supported && getenv("CHROME9_TCL_FLOW_TEST_EXPORT")) {
      FILE *f = fopen(getenv("CHROME9_TCL_FLOW_TEST_EXPORT"), "ab");
      const uint32_t head[5] = {
         flow->max_depth, 0u, p->instruction_count,
         p->input_count, chrome9_tcl_constant_count(p),
      };

      if (!f || fwrite(head, sizeof(head), 1, f) != 1 ||
          fwrite(p->instructions, sizeof(p->instructions[0]),
                 p->instruction_count, f) != p->instruction_count)
         failures++;
      if (f)
         fclose(f);
   }
   printf("%-24s %s: %u instructions, %u ARL, pad %u\n", name,
          failures ? "FAIL" : "ok", p->instruction_count, arls,
          p->constant_pad);
out:
   chrome9_tcl_program_fini(p);
   free(p);
   ralloc_free(nir);
   return failures;
}

/* mat4 m[3] = 0; m[index] = src; m[index][col][row] = value; out =
 * m[index2] * v: the selects of an indexed temporary array (piglit
 * vs-temp-array-mat4-index-col-row-wr), with row 0 = index, index2, col,
 * row, row 1 = value, rows 2..5 = src and row 6 = v. */
static nir_shader *
build_temp_array(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                   "temp array");
   nir_def *pos = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 0);
   nir_def *cb[7], *m[3][4], *out = NULL;

   b.shader->info.io_lowered = true;
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 2;
   b.shader->info.inputs_read = VERT_BIT_GENERIC0;
   b.shader->info.outputs_written = VARYING_BIT_POS | VARYING_BIT_VAR(0);
   b.shader->info.num_ubos = 1;
   for (unsigned r = 0; r < ARRAY_SIZE(cb); r++)
      cb[r] = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                nir_imm_int(&b, r));
   for (unsigned k = 0; k < 3; k++) {
      nir_def *is_k = nir_feq_imm(&b, nir_channel(&b, cb[0], 0), k);

      for (unsigned c = 0; c < 4; c++) {
         nir_def *at = nir_iand(&b, is_k,
                                nir_feq_imm(&b, nir_channel(&b, cb[0], 2), c));
         nir_def *lane[4];

         m[k][c] = nir_bcsel(&b, is_k, cb[2 + c], nir_imm_vec4(&b, 0, 0, 0, 0));
         for (unsigned r = 0; r < 4; r++)
            lane[r] = nir_bcsel(&b, nir_iand(&b, at,
                                   nir_feq_imm(&b, nir_channel(&b, cb[0], 3), r)),
                                nir_channel(&b, cb[1], 0),
                                nir_channel(&b, m[k][c], r));
         m[k][c] = nir_vec(&b, lane, 4);
      }
   }
   for (unsigned c = 0; c < 4; c++) {
      nir_def *index2 = nir_channel(&b, cb[0], 1);
      nir_def *column = nir_bcsel(&b, nir_feq_imm(&b, index2, 0), m[0][c],
                                  nir_bcsel(&b, nir_feq_imm(&b, index2, 1),
                                            m[1][c], m[2][c]));
      nir_def *term = nir_fmul(&b, column, nir_channel(&b, cb[6], c));

      out = out ? nir_fadd(&b, out, term) : term;
   }
   store(&b, pos, VARYING_SLOT_POS);
   store(&b, out, VARYING_SLOT_VAR0);
   return b.shader;
}

static void
temp_array_reference(const float (*cb)[4], float *out)
{
   float m[3][4][4] = { 0 };
   const int index = cb[0][0], index2 = cb[0][1];

   memcpy(m[index], &cb[2], sizeof(m[index]));
   m[index][(int)cb[0][2]][(int)cb[0][3]] = cb[1][0];
   for (unsigned r = 0; r < 4; r++) {
      out[r] = 0.0f;
      for (unsigned c = 0; c < 4; c++)
         out[r] += m[index2][c][r] * cb[6][c];
   }
}

/*
 * Without its control uniforms the select network needs more than the
 * DRAW_VERTEX_PROGRAM instruction limit (and more virtual temps, each of
 * which costs an instruction), so only keyed variants run on the TCL.
 */
static int
check_temp_array(const struct chrome9_tcl_flow *flow)
{
   static const uint16_t key_dword[4] = { 0, 1, 2, 3 };
   struct chrome9_tcl_program *p = calloc(1, sizeof(*p));
   nir_shader *nir = build_temp_array();
   unsigned instructions = 0;
   int failures = 0;

   chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, flow, NULL, p);
   if (p->supported) {
      fprintf(stderr, "temp array: unkeyed program fits %u instructions\n",
              p->instruction_count);
      failures++;
   }
   chrome9_tcl_program_fini(p);
   for (unsigned n = 0; n < 3 * 3 * 4 * 4; n++) {
      float cb[7][4] = {
         { n % 3, n / 3 % 3, n / 9 % 4, n / 36 }, { 9.5f },
         { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 },
         { 13, 14, 15, 16 }, { 0.5f, -1.0f, 2.0f, 0.25f },
      };
      float constant[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4] = { 0 };
      const float input[1][4] = { { 0.0f, 0.0f, 0.0f, 1.0f } };
      struct machine m = { .input = input, .constant = constant };
      uint8_t ints[DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS][4];
      uint32_t key[4];
      float want[4];

      memcpy(key, cb[0], sizeof(key));
      chrome9_tcl_compile(nir, true, CHROME9_TCL_COLOUR_FRONT, 0, false,
                          ARRAY_SIZE(key), key_dword, key, flow, NULL, p);
      if (!p->supported ||
          !chrome9_tcl_flow_constants(p, (const uint8_t *)cb, sizeof(cb),
                                      &m.bools, ints)) {
         fprintf(stderr, "temp array: keyed variant %u unsupported\n", n);
         failures++;
         chrome9_tcl_program_fini(p);
         continue;
      }
      instructions = MAX2(instructions, p->instruction_count);
      memcpy(constant, cb, MIN2(p->uniform_rows, ARRAY_SIZE(cb)) *
                           sizeof(cb[0]));
      memcpy(constant + p->uniform_rows, p->immediates,
             p->immediate_count * sizeof(p->immediates[0]));
      m.ints = ints;
      run(&m, p->instructions, 0, p->instruction_count);
      temp_array_reference((const float (*)[4])cb, want);
      for (unsigned c = 0; c < 4; c++) {
         if (fabsf(m.varying[0][c] - want[c]) <= 1e-4f)
            continue;
         fprintf(stderr, "temp array: index %g index2 %g col %g row %g "
                 "lane %u: %g, want %g\n", cb[0][0], cb[0][1], cb[0][2],
                 cb[0][3], c, m.varying[0][c], want[c]);
         failures++;
         break;
      }
      chrome9_tcl_program_fini(p);
   }
   printf("%-24s %s: keyed variants up to %u instructions\n",
          "temp array selects", failures ? "FAIL" : "ok", instructions);
   free(p);
   ralloc_free(nir);
   return failures;
}

/* !(any(a.xy != b.xy) || any(a.zw != b.zw)): the Boolean vector reduction
 * of a matrix compare must reach the vertex program as 0.0/1.0 floats. */
static int
check_bool_reduction(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                   "bool-reduction");
   nir_def *pos = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 0);
   nir_def *u = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                  nir_imm_int(&b, 0));
   nir_def *v = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                  nir_imm_int(&b, 1));
   struct chrome9_tcl_program *p = calloc(1, sizeof(*p));
   nir_def *columns, *equal;
   int failures = 0;

   b.shader->info.io_lowered = true;
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 2;
   b.shader->info.inputs_read = VERT_BIT_GENERIC0;
   b.shader->info.outputs_written = VARYING_BIT_POS | VARYING_BIT_VAR(0);
   b.shader->info.num_ubos = 1;
   columns = nir_vec2(&b,
      nir_bany_fnequal2(&b, nir_channels(&b, u, 0x3), nir_channels(&b, v, 0x3)),
      nir_bany_fnequal2(&b, nir_channels(&b, u, 0xc), nir_channels(&b, v, 0xc)));
   equal = nir_inot(&b, nir_bany_inequal2(&b, columns, nir_imm_false(&b)));
   store(&b, pos, VARYING_SLOT_POS);
   store(&b, nir_vec4(&b, nir_b2f32(&b, equal), nir_imm_float(&b, 0.0),
                      nir_imm_float(&b, 0.0), nir_imm_float(&b, 1.0)),
         VARYING_SLOT_VAR0);
   chrome9_tcl_compile(b.shader, true, CHROME9_TCL_COLOUR_FRONT, 0, false, 0,
                       NULL, NULL, NULL, NULL, p);
   if (!p->supported) {
      fprintf(stderr, "Boolean reduction: not supported\n");
      failures++;
   }
   for (unsigned k = 0; p->supported && k < 64; k++) {
      /* Lane l of v differs from u when bit l of k is set; bits 4, 5 pick
       * signed zeros and negative values. */
      float cb[2][4], constant[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4] = { 0 };
      const float input[1][4] = { { 0, 0, 0, 1 } };
      struct machine m = { .input = input, .constant = constant };
      bool equal = !(k & 0xf);

      for (unsigned l = 0; l < 4; l++) {
         cb[0][l] = k & 16 ? -0.0f : k & 32 ? -1.5f : 0.25f * l;
         cb[1][l] = k & (1u << l) ? cb[0][l] + 1.0f :
                    k & 16 ? 0.0f : cb[0][l];
      }
      memcpy(constant, cb, p->uniform_rows * sizeof(cb[0]));
      memcpy(constant + p->uniform_rows, p->immediates,
             p->immediate_count * sizeof(p->immediates[0]));
      run(&m, p->instructions, 0, p->instruction_count);
      if (m.varying[0][0] != (equal ? 1.0f : 0.0f)) {
         fprintf(stderr, "Boolean reduction: case %u gives %g\n", k,
                 m.varying[0][0]);
         failures++;
      }
   }
   printf("%-24s %s: %u instructions\n", "Boolean reduction",
          failures ? "FAIL" : "ok", p->instruction_count);
   chrome9_tcl_program_fini(p);
   free(p);
   ralloc_free(b.shader);
   return failures;
}

int
main(void)
{
   static const struct chrome9_tcl_flow none = { 0 };
   static const struct chrome9_tcl_flow depth1 = { 1 };
   static const struct chrome9_tcl_flow depth2 = { 2 };
   static const struct chrome9_tcl_flow relative = { .relative = true };
   static const struct chrome9_tcl_flow relative_flow = {
      .max_depth = 1, .relative = true,
   };
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
   failures += check("nested loops", SHAPE_NESTED_LOOP, &depth2, false, 0, 0);
   failures += check("literal 40 loop", SHAPE_LITERAL_LOOP, &depth1, true, 1,
                     0);
   failures += check("straight line", SHAPE_STRAIGHT, &depth1, true, 0, 0);
   failures += same_program(SHAPE_STRAIGHT, &none, &depth1);
   failures += check_relative("indirect, no cap", REL_PLAIN, &depth1, false, 0);
   failures += check_relative("indirect", REL_PLAIN, &relative, true, 0);
   failures += check_relative("indirect offset", REL_OFFSET, &relative, true, 1);
   failures += check_relative("indirect two lanes", REL_TWO, &relative, true, 0);
   failures += check_relative("indirect mat4 rows", REL_MAT, &relative, true, 3);
   failures += check_relative("indirect in a loop", REL_LOOP, &relative_flow,
                              true, 0);
   failures += check_relative("indirect five lanes", REL_FIVE, &relative, false,
                              0);
   failures += check_relative("indirect ARB ARL form", REL_ARB, &relative, true,
                              0);
   failures += check_relative("indirect abs, no trunc", REL_ABS, &relative,
                              true, 0);
   failures += check_temp_array(&depth1);
   failures += check_bool_reduction();
   glsl_type_singleton_decref();
   printf("chrome9 TCL flow tests %s\n", failures ? "FAILED" : "passed");
   return failures ? 1 : 0;
}
