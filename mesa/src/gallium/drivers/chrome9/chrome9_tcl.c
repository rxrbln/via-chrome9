/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * Hardware TCL: lower a Gallium vertex shader to the kernel's semantic
 * vertex IR (DRAW_VERTEX_PROGRAM) and submit raw vertex attributes, so
 * the draw module and its per-vertex CPU work are bypassed.  Anything the
 * IR or the silicon rules cannot express returns false and the caller
 * uses the draw module.
 */

#include "chrome9_tcl.h"

#include "chrome9_context.h"
#include "chrome9_draw.h"
#include "chrome9_resource.h"
#include "chrome9_specialise.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TCL_SEL_ONE DRM_CHROME9_FRAGMENT_SWIZZLE_ONE
#define TCL_SEL_ZERO DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO
#define TCL_MAX_VERTEX_BYTES (40U * 1024)
#define TCL_MAX_TRIANGLES (DRM_CHROME9_VERTEX_MAX_VERTICES / 3)

/* How to read the components of one NIR value. */
struct tcl_value {
   bool valid;
   uint8_t file;
   uint8_t index;
   uint8_t sel[4];
   bool neg;
};

struct chrome9_tcl_state {
   struct chrome9_tcl_fragment fragment;
   uint32_t constants[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4];
   uint32_t vertices[TCL_MAX_VERTEX_BYTES / sizeof(uint32_t)];
   /* One stage B chunk; the kernel bounds index_count per draw. */
   uint32_t indices[DRM_CHROME9_VERTEX_MAX_INDICES];
};

struct tcl_builder {
   struct chrome9_tcl_program *p;
   struct tcl_value *values;
   unsigned next_temp;
   bool failed;
   bool unit_temp[DRM_CHROME9_VERTEX_MAX_TEMPS];
   /* Whole-register copies used to legalize read ports. */
   int16_t constant_temp[DRM_CHROME9_VERTEX_MAX_CONSTANTS];
   int16_t input_temp[DRM_CHROME9_VERTEX_MAX_INPUTS];
};

static unsigned
tcl_type_size(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type, false);
}

static uint8_t
tcl_scalar_width(const nir_instr *instr, const void *data)
{
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return 0;
   switch (nir_instr_as_alu(instr)->op) {
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_fpow:
   case nir_op_fsqrt:
      return 1;
   default:
      return 0;
   }
}

/* GL clip-space z (-w..w) to the clip engine's D3D range (0..w). */
static bool
tcl_lower_halfz(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   nir_def *pos;

   (void)data;
   if (intr->intrinsic != nir_intrinsic_store_output ||
       nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS ||
       nir_intrinsic_component(intr) || nir_intrinsic_write_mask(intr) != 0xf ||
       intr->src[0].ssa->num_components != 4)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   pos = intr->src[0].ssa;
   nir_src_rewrite(&intr->src[0],
      nir_vec4(b, nir_channel(b, pos, 0), nir_channel(b, pos, 1),
               nir_fmul_imm(b, nir_fadd(b, nir_channel(b, pos, 2),
                                        nir_channel(b, pos, 3)), 0.5),
               nir_channel(b, pos, 3)));
   return true;
}

static bool
tcl_single_block(nir_shader *nir, nir_block **block)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_cf_node *node;

   if (!impl || !exec_list_is_singular(&impl->body))
      return false;
   node = exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node);
   if (node->type != nir_cf_node_block)
      return false;
   *block = nir_cf_node_as_block(node);
   return true;
}

/* Vertex inputs, CB0 loads and constants cannot fault; move them out of
 * branches so peephole select can flatten every if into selects. */
static bool
tcl_hoist_loads(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_block *start = nir_start_block(impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      if (block == start)
         continue;
      nir_foreach_instr_safe(instr, block) {
         bool movable = instr->type == nir_instr_type_load_const ||
                        instr->type == nir_instr_type_undef;

         if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            movable = (intr->intrinsic == nir_intrinsic_load_input ||
                       intr->intrinsic == nir_intrinsic_load_ubo ||
                       intr->intrinsic == nir_intrinsic_load_ubo_vec4) &&
                      nir_src_is_const(intr->src[0]) &&
                      (intr->intrinsic == nir_intrinsic_load_input ||
                       nir_src_is_const(intr->src[1]));
         }
         if (!movable)
            continue;
         nir_instr_move(nir_after_block_before_jump(start), instr);
         progress = true;
      }
   }
   if (progress)
      nir_progress(true, impl, nir_metadata_control_flow);
   return progress;
}

static uint8_t
tcl_vector_width(const nir_instr *instr, const void *data)
{
   (void)data;
   /* Keep scalar-unit ops scalar; they replicate one lane per record. */
   return tcl_scalar_width(instr, NULL) ? 1 : 4;
}

static void
tcl_optimize(nir_shader *nir)
{
   const nir_opt_peephole_select_options select = {
      .limit = 1024, .indirect_load_ok = true, .expensive_alu_ok = true,
   };
   bool progress;

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
      NIR_PASS(progress, nir, nir_opt_if, 0);
      NIR_PASS(progress, nir, tcl_hoist_loads);
      NIR_PASS(progress, nir, nir_opt_peephole_select, &select);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_vectorize, tcl_vector_width, NULL);
   } while (progress);
}

static unsigned
tcl_new_temp(struct tcl_builder *b)
{
   if (b->next_temp >= DRM_CHROME9_VERTEX_MAX_TEMPS) {
      if (!b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
         fprintf(stderr, "chrome9: TCL out of virtual temps\n");
      b->failed = true;
      return 0;
   }
   return b->next_temp++;
}

static bool
tcl_source_reads(const struct drm_chrome9_vertex_source *src)
{
   for (unsigned i = 0; i < 4; i++)
      if (src->swizzle[i] < TCL_SEL_ONE)
         return true;
   return false;
}

static void
tcl_append(struct tcl_builder *b, const struct drm_chrome9_vertex_instruction *insn)
{
   if (b->p->instruction_count >= DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS) {
      if (!b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
         fprintf(stderr, "chrome9: TCL out of instructions\n");
      b->failed = true;
      return;
   }
   b->p->instructions[b->p->instruction_count++] = *insn;
}

/* Copy a whole input or constant register into a temp once. */
static unsigned
tcl_register_temp(struct tcl_builder *b, uint8_t file, uint8_t index)
{
   int16_t *slot = file == DRM_CHROME9_VERTEX_FILE_INPUT ?
                   &b->input_temp[index] : &b->constant_temp[index];
   struct drm_chrome9_vertex_instruction mov = {
      .op = DRM_CHROME9_VERTEX_OP_MOV,
      .src = { { file, index, { 0, 1, 2, 3 }, 0, 0 } },
   };

   if (*slot >= 0)
      return *slot;
   mov.dst.file = DRM_CHROME9_VERTEX_DEST_TEMP;
   mov.dst.index = tcl_new_temp(b);
   mov.dst.writemask = 0xf;
   tcl_append(b, &mov);
   *slot = mov.dst.index;
   return mov.dst.index;
}

/*
 * Silicon rules (kernel chrome9_vertex_validate): one distinct input and
 * one distinct constant row per record, no direct constant source for
 * RCP/RSQ/EXP2/LOG2.  Offending sources read a temp copy instead.
 */
static void
tcl_emit(struct tcl_builder *b, struct drm_chrome9_vertex_instruction *insn,
         unsigned count)
{
   const bool scalar = insn->op >= DRM_CHROME9_VERTEX_OP_RCP;

   for (unsigned file = DRM_CHROME9_VERTEX_FILE_INPUT;
        file <= DRM_CHROME9_VERTEX_FILE_CONSTANT; file++) {
      int keep = -1;

      for (unsigned s = 0; s < count; s++) {
         struct drm_chrome9_vertex_source *src = &insn->src[s];

         if (src->file != file || !tcl_source_reads(src))
            continue;
         if (keep < 0 && !(scalar && file == DRM_CHROME9_VERTEX_FILE_CONSTANT))
            keep = src->index;
         if (src->index == keep)
            continue;
         src->index = tcl_register_temp(b, file, src->index);
         src->file = DRM_CHROME9_VERTEX_FILE_TEMP;
      }
   }
   tcl_append(b, insn);
}

static struct drm_chrome9_vertex_source
tcl_source(const struct tcl_value *v, const uint8_t *swizzle, unsigned lanes,
           bool neg)
{
   struct drm_chrome9_vertex_source src = { v->file, v->index };

   for (unsigned i = 0; i < 4; i++)
      src.swizzle[i] = v->sel[swizzle[i < lanes ? i : 0]];
   src.modifier = v->neg != neg ? DRM_CHROME9_FRAGMENT_MOD_NEG : 0;
   /* Literal-only operands name no register. */
   if (!tcl_source_reads(&src)) {
      src.file = DRM_CHROME9_VERTEX_FILE_TEMP;
      src.index = 0;
   }
   return src;
}

static const struct tcl_value *
tcl_value(struct tcl_builder *b, const nir_def *def)
{
   const struct tcl_value *v = &b->values[def->index];

   if (!v->valid || def->bit_size != 32) {
      if (!b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false)) {
         fprintf(stderr, "chrome9: TCL operand %s from: ",
                 v->valid ? "not 32-bit" : "undefined");
         nir_print_instr(nir_def_instr((nir_def *)def), stderr);
         fprintf(stderr, "\n");
      }
      b->failed = true;
   }
   return v;
}

static struct drm_chrome9_vertex_source
tcl_alu_source(struct tcl_builder *b, const nir_alu_instr *alu, unsigned s)
{
   return tcl_source(tcl_value(b, alu->src[s].src.ssa), alu->src[s].swizzle,
                     nir_ssa_alu_instr_src_components(alu, s), false);
}

static void
tcl_set_temp(struct tcl_builder *b, const nir_def *def, unsigned temp)
{
   struct tcl_value *v = &b->values[def->index];

   memset(v, 0, sizeof(*v));
   v->valid = true;
   v->file = DRM_CHROME9_VERTEX_FILE_TEMP;
   v->index = temp;
   /* Lanes past the written width replicate x so they stay defined. */
   for (unsigned i = 0; i < 4; i++)
      v->sel[i] = i < def->num_components ? i : 0;
}

/* Read lanes [0, lanes) of a temp; the rest replicate x. */
static struct drm_chrome9_vertex_source
tcl_temp_source(unsigned temp, unsigned lanes, bool neg)
{
   struct drm_chrome9_vertex_source src = {
      DRM_CHROME9_VERTEX_FILE_TEMP, temp, { 0 },
      neg ? DRM_CHROME9_FRAGMENT_MOD_NEG : 0, 0,
   };

   for (unsigned i = 0; i < 4; i++)
      src.swizzle[i] = i < lanes ? i : 0;
   return src;
}

static unsigned
tcl_op(struct tcl_builder *b, uint8_t op, uint8_t mask, unsigned temp,
       const struct drm_chrome9_vertex_source *srcs, unsigned count)
{
   struct drm_chrome9_vertex_instruction insn = { .op = op };

   insn.dst.file = DRM_CHROME9_VERTEX_DEST_TEMP;
   insn.dst.index = temp;
   insn.dst.writemask = mask;
   for (unsigned s = 0; s < count; s++)
      insn.src[s] = srcs[s];
   tcl_emit(b, &insn, count);
   return temp;
}

/* Per-component scalar op; each lane replicates its source selector. */
static void
tcl_scalar(struct tcl_builder *b, uint8_t op, unsigned temp, unsigned lanes,
           struct drm_chrome9_vertex_source src)
{
   for (unsigned c = 0; c < lanes; c++) {
      struct drm_chrome9_vertex_source lane = src;

      for (unsigned i = 0; i < 4; i++)
         lane.swizzle[i] = src.swizzle[c];
      if (!tcl_source_reads(&lane)) {
         lane.file = DRM_CHROME9_VERTEX_FILE_TEMP;
         lane.index = 0;
      }
      tcl_op(b, op, 1u << c, temp, &lane, 1);
   }
}

/* Express mov/fneg/vecN as a read of one existing register when the
 * components allow it; literal ONE lanes must share the resulting sign. */
static bool
tcl_alias(struct tcl_builder *b, nir_alu_instr *alu)
{
   struct tcl_value alias = { .valid = true };
   const unsigned n = alu->def.num_components;
   const bool neg = alu->op == nir_op_fneg;
   const struct tcl_value *first = NULL;
   int one_sign = -1;

   for (unsigned c = 0; c < n; c++) {
      const unsigned s = alu->op == nir_op_mov || neg ? 0 : c;
      const struct tcl_value *v = tcl_value(b, alu->src[s].src.ssa);
      const uint8_t sel = v->sel[alu->src[s].swizzle[s ? 0 : c]];

      if (b->failed)
         return false;
      if (sel == TCL_SEL_ONE) {
         const int sign = v->neg != neg;

         if (one_sign >= 0 && one_sign != sign)
            return false;
         one_sign = sign;
      } else if (sel != TCL_SEL_ZERO) {
         if (first && (first->file != v->file || first->index != v->index ||
                       first->neg != v->neg))
            return false;
         first = v;
      }
      alias.sel[c] = sel;
   }
   alias.file = first ? first->file : DRM_CHROME9_VERTEX_FILE_TEMP;
   alias.index = first ? first->index : 0;
   alias.neg = first ? first->neg != neg : one_sign > 0;
   if (one_sign >= 0 && one_sign != alias.neg)
      return false;
   for (unsigned c = n; c < 4; c++)
      alias.sel[c] = alias.sel[0];
   b->values[alu->def.index] = alias;
   return true;
}

static void
tcl_vec(struct tcl_builder *b, nir_alu_instr *alu)
{
   unsigned temp;

   if (tcl_alias(b, alu) || b->failed)
      return;
   temp = tcl_new_temp(b);
   for (unsigned c = 0; c < alu->def.num_components; c++) {
      const unsigned s = alu->op == nir_op_mov ? 0 : c;
      const struct tcl_value *v = tcl_value(b, alu->src[s].src.ssa);
      struct drm_chrome9_vertex_source src =
         tcl_source(v, &alu->src[s].swizzle[s ? 0 : c], 1, false);

      tcl_op(b, DRM_CHROME9_VERTEX_OP_MOV, 1u << c, temp, &src, 1);
   }
   tcl_set_temp(b, &alu->def, temp);
}

/* cond ? a : b as MAD(a - b, mask, b) with a 0/1 mask. */
static unsigned
tcl_select(struct tcl_builder *b, unsigned mask_temp, unsigned lanes,
           struct drm_chrome9_vertex_source a,
           struct drm_chrome9_vertex_source c)
{
   const unsigned diff = tcl_new_temp(b), out = tcl_new_temp(b);
   struct drm_chrome9_vertex_source src[3] = { a, c };

   tcl_op(b, DRM_CHROME9_VERTEX_OP_SUB, BITFIELD_MASK(lanes), diff, src, 2);
   src[0] = tcl_temp_source(diff, lanes, false);
   src[1] = tcl_temp_source(mask_temp, lanes, false);
   src[2] = c;
   return tcl_op(b, DRM_CHROME9_VERTEX_OP_MAD, BITFIELD_MASK(lanes), out, src,
                 3);
}

static void
tcl_alu(struct tcl_builder *b, nir_alu_instr *alu)
{
   static const struct { nir_op nir; uint8_t op; uint8_t count; } direct[] = {
      { nir_op_fadd, DRM_CHROME9_VERTEX_OP_ADD, 2 },
      { nir_op_fsub, DRM_CHROME9_VERTEX_OP_SUB, 2 },
      { nir_op_fmul, DRM_CHROME9_VERTEX_OP_MUL, 2 },
      { nir_op_ffma, DRM_CHROME9_VERTEX_OP_MAD, 3 },
      { nir_op_ffma_weak, DRM_CHROME9_VERTEX_OP_MAD, 3 },
      { nir_op_fmin, DRM_CHROME9_VERTEX_OP_MIN, 2 },
      { nir_op_fmax, DRM_CHROME9_VERTEX_OP_MAX, 2 },
      { nir_op_fabs, DRM_CHROME9_VERTEX_OP_ABS, 1 },
      { nir_op_fsign, DRM_CHROME9_VERTEX_OP_SGN, 1 },
      { nir_op_ffract, DRM_CHROME9_VERTEX_OP_FRC, 1 },
      { nir_op_ffloor, DRM_CHROME9_VERTEX_OP_FLR, 1 },
      { nir_op_slt, DRM_CHROME9_VERTEX_OP_SLT, 2 },
      { nir_op_sge, DRM_CHROME9_VERTEX_OP_SGE, 2 },
      { nir_op_fdot3, DRM_CHROME9_VERTEX_OP_DP3, 2 },
      { nir_op_fdot4, DRM_CHROME9_VERTEX_OP_DP4, 2 },
      { nir_op_fdph, DRM_CHROME9_VERTEX_OP_DPH, 2 },
      { nir_op_fsat, DRM_CHROME9_VERTEX_OP_MOV, 1 },
   };
   const unsigned n = alu->def.num_components;
   const uint8_t mask = BITFIELD_MASK(n);
   struct drm_chrome9_vertex_source src[3] = { 0 };
   unsigned temp;

   if (alu->def.bit_size != 32) {
      b->failed = true;
      return;
   }
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      if (alu->op == nir_op_fneg ? !tcl_alias(b, alu) : false) {
         temp = tcl_new_temp(b);
         src[0] = tcl_alu_source(b, alu, 0);
         src[0].modifier ^= DRM_CHROME9_FRAGMENT_MOD_NEG;
         tcl_op(b, DRM_CHROME9_VERTEX_OP_MOV, mask, temp, src, 1);
         tcl_set_temp(b, &alu->def, temp);
         return;
      }
      if (alu->op != nir_op_fneg)
         tcl_vec(b, alu);
      return;
   default:
      break;
   }
   if (nir_op_infos[alu->op].num_inputs > 3) {
      b->failed = true;
      return;
   }
   for (unsigned s = 0; s < nir_op_infos[alu->op].num_inputs; s++)
      src[s] = tcl_alu_source(b, alu, s);
   if (b->failed)
      return;
   temp = tcl_new_temp(b);
   for (unsigned i = 0; i < ARRAY_SIZE(direct); i++) {
      struct drm_chrome9_vertex_instruction insn = { .op = direct[i].op };

      if (direct[i].nir != alu->op)
         continue;
      insn.dst.file = DRM_CHROME9_VERTEX_DEST_TEMP;
      insn.dst.index = temp;
      insn.dst.writemask = alu->op == nir_op_fdot3 ||
                           alu->op == nir_op_fdot4 ||
                           alu->op == nir_op_fdph ? 1 : mask;
      if (alu->op == nir_op_fsat) {
         insn.dst.modifier = DRM_CHROME9_FRAGMENT_DEST_SAT;
         b->unit_temp[temp] = true;
      }
      memcpy(insn.src, src, sizeof(src));
      tcl_emit(b, &insn, direct[i].count);
      tcl_set_temp(b, &alu->def, temp);
      return;
   }
   switch (alu->op) {
   case nir_op_fdot2:
      src[0].swizzle[2] = TCL_SEL_ZERO;
      src[1].swizzle[2] = TCL_SEL_ZERO;
      tcl_op(b, DRM_CHROME9_VERTEX_OP_DP3, 1, temp, src, 2);
      break;
   case nir_op_frcp:
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_RCP, temp, n, src[0]);
      break;
   case nir_op_frsq:
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_RSQ, temp, n, src[0]);
      break;
   case nir_op_fexp2:
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_EXP2, temp, n, src[0]);
      break;
   case nir_op_flog2:
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_LOG2, temp, n, src[0]);
      break;
   case nir_op_fsqrt: {
      const unsigned rsq = tcl_new_temp(b);

      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_RSQ, rsq, n, src[0]);
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_RCP, temp, n,
                 tcl_temp_source(rsq, n, false));
      break;
   }
   case nir_op_fpow: {
      const unsigned log = tcl_new_temp(b), mul = tcl_new_temp(b);
      struct drm_chrome9_vertex_source m[2] = {
         tcl_temp_source(log, n, false), src[1],
      };

      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_LOG2, log, n, src[0]);
      tcl_op(b, DRM_CHROME9_VERTEX_OP_MUL, mask, mul, m, 2);
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_EXP2, temp, n,
                 tcl_temp_source(mul, n, false));
      break;
   }
   case nir_op_seq:
   case nir_op_sne: {
      const unsigned ge = tcl_new_temp(b), le = tcl_new_temp(b);
      struct drm_chrome9_vertex_source swapped[2] = { src[1], src[0] };
      struct drm_chrome9_vertex_source m[2] = {
         tcl_temp_source(ge, n, false), tcl_temp_source(le, n, false),
      };

      tcl_op(b, DRM_CHROME9_VERTEX_OP_SGE, mask, ge, src, 2);
      tcl_op(b, DRM_CHROME9_VERTEX_OP_SGE, mask, le, swapped, 2);
      if (alu->op == nir_op_seq) {
         tcl_op(b, DRM_CHROME9_VERTEX_OP_MUL, mask, temp, m, 2);
      } else {
         const unsigned eq = tcl_new_temp(b);
         struct drm_chrome9_vertex_source one_minus[2] = {
            tcl_temp_source(eq, n, true),
            { DRM_CHROME9_VERTEX_FILE_TEMP, 0,
              { TCL_SEL_ONE, TCL_SEL_ONE, TCL_SEL_ONE, TCL_SEL_ONE } },
         };

         tcl_op(b, DRM_CHROME9_VERTEX_OP_MUL, mask, eq, m, 2);
         tcl_op(b, DRM_CHROME9_VERTEX_OP_ADD, mask, temp, one_minus, 2);
      }
      break;
   }
   case nir_op_fcsel:
   case nir_op_fcsel_gt:
   case nir_op_fcsel_ge: {
      const unsigned cond = tcl_new_temp(b);
      struct drm_chrome9_vertex_source zero = {
         DRM_CHROME9_VERTEX_FILE_TEMP, 0,
         { TCL_SEL_ZERO, TCL_SEL_ZERO, TCL_SEL_ZERO, TCL_SEL_ZERO },
      };
      struct drm_chrome9_vertex_source c[2];

      if (alu->op == nir_op_fcsel) {
         const unsigned sign = tcl_new_temp(b);

         /* c != 0 as |sgn(c)|. */
         tcl_op(b, DRM_CHROME9_VERTEX_OP_SGN, mask, sign, src, 1);
         c[0] = tcl_temp_source(sign, n, false);
         tcl_op(b, DRM_CHROME9_VERTEX_OP_ABS, mask, cond, c, 1);
      } else if (alu->op == nir_op_fcsel_gt) {
         c[0] = zero;
         c[1] = src[0];
         tcl_op(b, DRM_CHROME9_VERTEX_OP_SLT, mask, cond, c, 2);
      } else {
         c[0] = src[0];
         c[1] = zero;
         tcl_op(b, DRM_CHROME9_VERTEX_OP_SGE, mask, cond, c, 2);
      }
      tcl_set_temp(b, &alu->def, tcl_select(b, cond, n, src[1], src[2]));
      return;
   }
   default:
      b->failed = true;
      return;
   }
   tcl_set_temp(b, &alu->def, temp);
}

static bool
tcl_ubo_row(const nir_intrinsic_instr *load, unsigned *row, unsigned *lane)
{
   unsigned word;

   if ((load->intrinsic != nir_intrinsic_load_ubo &&
        load->intrinsic != nir_intrinsic_load_ubo_vec4) ||
       !nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
       !nir_src_is_const(load->src[1]) || load->def.bit_size != 32)
      return false;
   if (load->intrinsic == nir_intrinsic_load_ubo) {
      if (nir_src_as_uint(load->src[1]) & 3)
         return false;
      word = nir_src_as_uint(load->src[1]) / 4;
   } else {
      word = (nir_intrinsic_base(load) + nir_src_as_uint(load->src[1])) * 4 +
             nir_intrinsic_component(load);
   }
   *row = word / 4;
   *lane = word % 4;
   return *row < DRM_CHROME9_VERTEX_MAX_CONSTANTS &&
          *lane + load->def.num_components <= 4;
}

static void
tcl_load_const(struct tcl_builder *b, nir_load_const_instr *load)
{
   struct tcl_value *out = &b->values[load->def.index];
   const unsigned n = load->def.num_components;
   uint32_t row[4] = { 0 };
   bool literal = true, negative = false, positive = false;

   if (load->def.bit_size != 32) {
      b->failed = true;
      return;
   }
   memset(out, 0, sizeof(*out));
   out->valid = true;
   out->file = DRM_CHROME9_VERTEX_FILE_TEMP;
   for (unsigned c = 0; c < n; c++) {
      const float f = load->value[c].f32;

      row[c] = load->value[c].u32;
      if (!isfinite(f))
         b->failed = true;
      if (f == 0.0f)
         out->sel[c] = TCL_SEL_ZERO;
      else if (f == 1.0f || f == -1.0f)
         out->sel[c] = TCL_SEL_ONE;
      else
         literal = false;
      negative |= f == -1.0f;
      positive |= f == 1.0f;
   }
   if (literal && !(negative && positive)) {
      out->neg = negative;
      for (unsigned c = n; c < 4; c++)
         out->sel[c] = out->sel[0];
      return;
   }
   /* Immediates follow the CB0 rows; reuse an identical row. */
   unsigned k;
   for (k = 0; k < b->p->immediate_count; k++)
      if (!memcmp(b->p->immediates[k], row, sizeof(row)))
         break;
   if (k == b->p->immediate_count) {
      if (b->p->uniform_rows + k >= DRM_CHROME9_VERTEX_MAX_CONSTANTS) {
         b->failed = true;
         return;
      }
      memcpy(b->p->immediates[k], row, sizeof(row));
      b->p->immediate_count++;
   }
   out->neg = false;
   out->file = DRM_CHROME9_VERTEX_FILE_CONSTANT;
   out->index = b->p->uniform_rows + k;
   for (unsigned c = 0; c < 4; c++)
      out->sel[c] = c < n ? c : 0;
}

static void
tcl_store(struct tcl_builder *b, nir_intrinsic_instr *store)
{
   const struct nir_io_semantics io = nir_intrinsic_io_semantics(store);
   const unsigned component = nir_intrinsic_component(store);
   const unsigned mask = nir_intrinsic_write_mask(store) << component;
   const struct tcl_value *v = tcl_value(b, store->src[0].ssa);
   struct drm_chrome9_vertex_instruction mov = {
      .op = DRM_CHROME9_VERTEX_OP_MOV,
   };
   uint8_t swizzle[4] = { 0 };
   unsigned index;

   if (!nir_src_is_const(store->src[1]) || nir_src_as_uint(store->src[1]) ||
       mask > 0xf || b->failed) {
      b->failed = true;
      return;
   }
   if (io.location == VARYING_SLOT_POS) {
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_POSITION;
   } else if (io.location == VARYING_SLOT_COL0) {
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_PRIMARY;
   } else if (io.location == VARYING_SLOT_CLIP_DIST0 ||
              io.location == VARYING_SLOT_CLIP_DIST1 ||
              io.location == VARYING_SLOT_CLIP_VERTEX ||
              io.location == VARYING_SLOT_BFC0 ||
              io.location == VARYING_SLOT_BFC1) {
      b->failed = true;
      return;
   } else {
      /* Fragment varying n is VAR0+n or TEX0+n (chrome9_context.c). */
      if (io.location >= VARYING_SLOT_VAR0 &&
          io.location < VARYING_SLOT_VAR0 + DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
         index = io.location - VARYING_SLOT_VAR0;
      else if (io.location >= VARYING_SLOT_TEX0 &&
               io.location < VARYING_SLOT_TEX0 +
                             DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
         index = io.location - VARYING_SLOT_TEX0;
      else
         return;
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_VARYING;
      mov.dst.index = index;
   }
   mov.dst.writemask = mask;
   for (unsigned i = 0; i < 4; i++)
      swizzle[i] = mask & (1u << i) ? i - component : 0;
   mov.src[0] = tcl_source(v, swizzle, 4, false);
   if (mov.dst.file == DRM_CHROME9_VERTEX_DEST_VARYING) {
      for (unsigned i = 0; i < 4; i++) {
         const uint8_t sel = mov.src[0].swizzle[i];
         const bool neg = mov.src[0].modifier;
         uint8_t *origin = &b->p->origin[index][i];

         if (!(mask & (1u << i)))
            continue;
         b->p->origin_input[index][i] = mov.src[0].index;
         b->p->origin_lane[index][i] = sel;
         *origin = sel == TCL_SEL_ZERO ? CHROME9_TCL_ORIGIN_ZERO :
                   sel == TCL_SEL_ONE && !neg ? CHROME9_TCL_ORIGIN_ONE :
                   neg ? CHROME9_TCL_ORIGIN_UNKNOWN :
                   mov.src[0].file == DRM_CHROME9_VERTEX_FILE_INPUT ?
                   CHROME9_TCL_ORIGIN_INPUT :
                   mov.src[0].file == DRM_CHROME9_VERTEX_FILE_TEMP &&
                   b->unit_temp[mov.src[0].index] ?
                   CHROME9_TCL_ORIGIN_UNIT : CHROME9_TCL_ORIGIN_UNKNOWN;
      }
   }
   tcl_emit(b, &mov, 1);
}

static void
tcl_intrinsic(struct tcl_builder *b, nir_intrinsic_instr *intr)
{
   struct tcl_value *out = &b->values[intr->def.index];
   unsigned row, lane;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          nir_intrinsic_base(intr) >= DRM_CHROME9_VERTEX_MAX_INPUTS ||
          nir_intrinsic_component(intr) + intr->def.num_components > 4 ||
          intr->def.bit_size != 32) {
         b->failed = true;
         return;
      }
      memset(out, 0, sizeof(*out));
      out->valid = true;
      out->file = DRM_CHROME9_VERTEX_FILE_INPUT;
      out->index = nir_intrinsic_base(intr);
      for (unsigned c = 0; c < 4; c++)
         out->sel[c] = nir_intrinsic_component(intr) +
                       (c < intr->def.num_components ? c : 0);
      b->p->input_count = MAX2(b->p->input_count, out->index + 1u);
      return;
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ubo_vec4:
      if (!tcl_ubo_row(intr, &row, &lane)) {
         b->failed = true;
         return;
      }
      memset(out, 0, sizeof(*out));
      out->valid = true;
      out->file = DRM_CHROME9_VERTEX_FILE_CONSTANT;
      out->index = row;
      for (unsigned c = 0; c < 4; c++)
         out->sel[c] = lane + (c < intr->def.num_components ? c : 0);
      return;
   case nir_intrinsic_store_output:
      tcl_store(b, intr);
      return;
   default:
      b->failed = true;
      return;
   }
}

/* CB0 dword range of a constant-offset UBO load, or false. */
static bool
tcl_ubo_dwords(const nir_intrinsic_instr *load, unsigned *first)
{
   unsigned row, lane;

   if (!tcl_ubo_row(load, &row, &lane))
      return false;
   *first = row * 4 + lane;
   return true;
}

struct tcl_key {
   unsigned count;
   const uint16_t *dword;
   const uint32_t *value;
};

static bool
tcl_inline_key(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct tcl_key *key = data;
   nir_const_value values[4] = { 0 };
   unsigned first;

   if ((intr->intrinsic != nir_intrinsic_load_ubo &&
        intr->intrinsic != nir_intrinsic_load_ubo_vec4) ||
       !tcl_ubo_dwords(intr, &first))
      return false;
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      unsigned k;

      for (k = 0; k < key->count; k++)
         if (key->dword[k] == first + c)
            break;
      if (k == key->count)
         return false;
      values[c].u32 = key->value[k];
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_build_imm(b, intr->def.num_components, 32,
                                             values));
   return true;
}

/* Collect CB0 dwords a condition depends on through ALU chains. */
static void
tcl_collect_key(struct chrome9_tcl_shader *shader, nir_def *def,
                BITSET_WORD *visited)
{
   nir_instr *instr = nir_def_instr(def);
   unsigned first;

   if (BITSET_TEST(visited, def->index))
      return;
   BITSET_SET(visited, def->index);
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      for (unsigned s = 0; s < nir_op_infos[alu->op].num_inputs; s++)
         tcl_collect_key(shader, alu->src[s].src.ssa, visited);
      return;
   }
   if (instr->type != nir_instr_type_intrinsic ||
       !tcl_ubo_dwords(nir_instr_as_intrinsic(instr), &first))
      return;
   for (unsigned c = 0; c < def->num_components; c++) {
      unsigned k;

      for (k = 0; k < shader->key_count; k++)
         if (shader->key_dword[k] == first + c)
            break;
      if (k == shader->key_count && k < CHROME9_TCL_MAX_KEY)
         shader->key_dword[shader->key_count++] = first + c;
   }
}

static void
tcl_find_key(struct chrome9_tcl_shader *shader)
{
   nir_shader *nir = nir_shader_clone(NULL, shader->nir);
   nir_function_impl *impl;
   BITSET_WORD *visited;
   bool progress;

   if (!nir)
      return;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_algebraic);
   } while (progress);
   impl = nir_shader_get_entrypoint(nir);
   nir_index_ssa_defs(impl);
   visited = rzalloc_array(nir, BITSET_WORD, BITSET_WORDS(impl->ssa_alloc));
   nir_foreach_block(block, impl) {
      nir_if *nif = nir_block_get_following_if(block);

      if (nif)
         tcl_collect_key(shader, nif->condition.ssa, visited);
      nir_foreach_instr(instr, block) {
         nir_alu_instr *alu;

         if (instr->type != nir_instr_type_alu)
            continue;
         alu = nir_instr_as_alu(instr);
         if (alu->op == nir_op_bcsel || alu->op == nir_op_fcsel ||
             alu->op == nir_op_fcsel_gt || alu->op == nir_op_fcsel_ge)
            tcl_collect_key(shader, alu->src[0].src.ssa, visited);
      }
   }
   ralloc_free(nir);
}

bool
chrome9_tcl_compile(const struct nir_shader *source, bool clip_halfz,
                    unsigned key_count, const uint16_t *key_dword,
                    const uint32_t *key_value, struct chrome9_tcl_program *p)
{
   struct tcl_builder b = { .p = p };
   struct tcl_key key = { key_count, key_dword, key_value };
   nir_shader *nir = nir_shader_clone(NULL, source);
   nir_shader_compiler_options *options;
   nir_block *block;

   memset(p, 0, sizeof(*p));
   p->compiled = true;
   if (!nir)
      return false;
   /* Every record is a MAD; let algebraic fuse multiply-add chains. */
   options = ralloc(nir, nir_shader_compiler_options);
   if (!options)
      goto out;
   *options = *nir->options;
   options->float_mul_add32 = nir_float_muladd_support_has_ffma |
                              nir_float_muladd_support_fuse;
   nir->options = options;
   if (!nir->info.io_lowered) {
      if (!clip_halfz)
         NIR_PASS(_, nir, nir_lower_clip_halfz);
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               tcl_type_size, (nir_lower_io_options)0);
   } else if (!clip_halfz) {
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, tcl_lower_halfz,
               nir_metadata_control_flow, NULL);
   }
   /* Fold the current control uniforms so their branches disappear. */
   if (key_count)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, tcl_inline_key,
               nir_metadata_control_flow, &key);
   NIR_PASS(_, nir, nir_lower_alu_width, tcl_scalar_width, NULL);
   tcl_optimize(nir);
   /* Integer uniforms are float-encoded; booleans become 0.0/1.0. */
   chrome9_nir_lower_integers(nir);
   NIR_PASS(_, nir, nir_lower_bool_to_float, true);
   tcl_optimize(nir);
   if (!tcl_single_block(nir, &block)) {
      if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
{
         fprintf(stderr, "chrome9: TCL cannot express control flow\n");
         if (debug_get_bool_option("CHROME9_DEBUG_TCL_NIR", false))
            nir_print_shader(nir, stderr);
      }
      goto out;
   }
   /* CB0 rows come first so immediates can follow them. */
   nir_foreach_instr(instr, block) {
      unsigned row, lane;

      if (instr->type == nir_instr_type_intrinsic &&
          tcl_ubo_row(nir_instr_as_intrinsic(instr), &row, &lane))
         p->uniform_rows = MAX2(p->uniform_rows, row + 1);
   }
   b.values = CALLOC(nir_shader_get_entrypoint(nir)->ssa_alloc,
                     sizeof(*b.values));
   if (!b.values)
      goto out;
   memset(b.constant_temp, 0xff, sizeof(b.constant_temp));
   memset(b.input_temp, 0xff, sizeof(b.input_temp));
   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         tcl_alu(&b, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_load_const:
         tcl_load_const(&b, nir_instr_as_load_const(instr));
         break;
      case nir_instr_type_undef: {
         nir_undef_instr *undef = nir_instr_as_undef(instr);
         struct tcl_value *v = &b.values[undef->def.index];

         memset(v, 0, sizeof(*v));
         v->valid = true;
         memset(v->sel, TCL_SEL_ZERO, sizeof(v->sel));
         break;
      }
      case nir_instr_type_intrinsic:
         tcl_intrinsic(&b, nir_instr_as_intrinsic(instr));
         break;
      default:
         b.failed = true;
         break;
      }
      if (b.failed) {
         if (debug_get_bool_option("CHROME9_DEBUG_TCL_NIR", false))
            nir_print_shader(nir, stderr);
         if (debug_get_bool_option("CHROME9_DEBUG_TCL", false)) {
            fprintf(stderr, "chrome9: TCL cannot express: ");
            nir_print_instr(instr, stderr);
            fprintf(stderr, "\n");
         }
         break;
      }
   }
   p->supported = !b.failed && p->instruction_count && p->input_count;
   FREE(b.values);
out:
   ralloc_free(nir);
   return p->supported;
}

struct chrome9_tcl_shader *
chrome9_tcl_shader_create(const struct nir_shader *nir)
{
   struct chrome9_tcl_shader *shader = CALLOC_STRUCT(chrome9_tcl_shader);

   if (!shader)
      return NULL;
   shader->nir = nir_shader_clone(NULL, nir);
   if (!shader->nir) {
      FREE(shader);
      return NULL;
   }
   tcl_find_key(shader);
   return shader;
}

void
chrome9_tcl_shader_destroy(struct chrome9_tcl_shader *shader)
{
   if (!shader)
      return;
   while (shader->variants) {
      struct chrome9_tcl_variant *next = shader->variants->next;

      FREE(shader->variants);
      shader->variants = next;
   }
   ralloc_free(shader->nir);
   FREE(shader);
}

/*
 * Draw-position triangle t of a primitive, rotated (winding kept) so the
 * GL provoking vertex (ARB_provoking_vertex) is last, the hardware's flat
 * source.  first selects the first-vertex convention.
 */
static bool
tcl_triangle(enum mesa_prim mode, unsigned t, bool first, unsigned v[3])
{
   unsigned provoking, k;

   switch (mode) {
   case MESA_PRIM_TRIANGLES:
      v[0] = 3 * t; v[1] = 3 * t + 1; v[2] = 3 * t + 2;
      provoking = first ? v[0] : v[2];
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      /* Odd triangles swap their first two vertices to keep winding. */
      v[0] = t + (t & 1); v[1] = t + 1 - (t & 1); v[2] = t + 2;
      provoking = first ? t : t + 2;
      break;
   case MESA_PRIM_TRIANGLE_FAN:
      v[0] = 0; v[1] = t + 1; v[2] = t + 2;
      provoking = first ? t + 1 : t + 2;
      break;
   case MESA_PRIM_POLYGON:
      v[0] = 0; v[1] = t + 1; v[2] = t + 2;
      provoking = 0;
      break;
   case MESA_PRIM_QUADS:
      /* Both halves share the provoking corner. */
      k = 4 * (t / 2);
      if (first) {
         v[0] = k; v[1] = k + 1 + (t & 1); v[2] = k + 2 + (t & 1);
         provoking = k;
      } else {
         v[0] = k + (t & 1); v[1] = k + 1 + (t & 1); v[2] = k + 3;
         provoking = k + 3;
      }
      break;
   case MESA_PRIM_QUAD_STRIP:
      /* Quad k is 2k, 2k+1, 2k+3, 2k+2. */
      k = 2 * (t / 2);
      if (!(t & 1)) {
         v[0] = k; v[1] = k + 1; v[2] = k + 3;
      } else if (first) {
         v[0] = k; v[1] = k + 3; v[2] = k + 2;
      } else {
         v[0] = k + 2; v[1] = k; v[2] = k + 3;
      }
      provoking = first ? k : k + 3;
      break;
   default:
      return false;
   }
   while (v[2] != provoking) {
      const unsigned rotate = v[0];

      v[0] = v[1];
      v[1] = v[2];
      v[2] = rotate;
   }
   return true;
}

static unsigned
tcl_triangle_count(enum mesa_prim mode, unsigned count)
{
   switch (mode) {
   case MESA_PRIM_TRIANGLES:
      return count / 3;
   case MESA_PRIM_TRIANGLE_STRIP:
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      return count >= 3 ? count - 2 : 0;
   case MESA_PRIM_QUADS:
      return count / 4 * 2;
   case MESA_PRIM_QUAD_STRIP:
      return count >= 4 ? (count - 2) / 2 * 2 : 0;
   default:
      return 0;
   }
}

struct tcl_stream {
   const uint8_t *base;
   size_t size;
   unsigned stride;
   enum pipe_format format;
   unsigned bytes;
   /* Kernel element format; FLOAT4 means CPU unpack. */
   uint8_t drm_format;
   uint8_t dwords;
   /* float32 or float16 dwords to check for Inf/NaN (kernel rule). */
   uint8_t float32;
   uint8_t float16;
};

/* Formats the vertex fetcher reads natively (silicon rungs B9-B11). */
static bool
tcl_native_format(enum pipe_format format, struct tcl_stream *stream)
{
   static const struct {
      enum pipe_format format;
      uint8_t drm, dwords, float32, float16;
   } table[] = {
      { PIPE_FORMAT_R32_FLOAT, DRM_CHROME9_VERTEX_FORMAT_FLOAT1, 1, 1, 0 },
      { PIPE_FORMAT_R32G32_FLOAT, DRM_CHROME9_VERTEX_FORMAT_FLOAT2, 2, 2, 0 },
      { PIPE_FORMAT_R32G32B32_FLOAT, DRM_CHROME9_VERTEX_FORMAT_FLOAT3, 3, 3, 0 },
      { PIPE_FORMAT_R32G32B32A32_FLOAT, DRM_CHROME9_VERTEX_FORMAT_FLOAT4, 4, 4, 0 },
      { PIPE_FORMAT_B8G8R8A8_UNORM, DRM_CHROME9_VERTEX_FORMAT_BGRA8_UNORM, 1, 0, 0 },
      { PIPE_FORMAT_R8G8B8A8_UNORM, DRM_CHROME9_VERTEX_FORMAT_RGBA8_UNORM, 1, 0, 0 },
      { PIPE_FORMAT_R8G8B8A8_USCALED, DRM_CHROME9_VERTEX_FORMAT_RGBA8_USCALED, 1, 0, 0 },
      { PIPE_FORMAT_R16G16_UNORM, DRM_CHROME9_VERTEX_FORMAT_RG16_UNORM, 1, 0, 0 },
      { PIPE_FORMAT_R16G16B16A16_UNORM, DRM_CHROME9_VERTEX_FORMAT_RGBA16_UNORM, 2, 0, 0 },
      { PIPE_FORMAT_R16G16_SNORM, DRM_CHROME9_VERTEX_FORMAT_RG16_SNORM, 1, 0, 0 },
      { PIPE_FORMAT_R16G16B16A16_SNORM, DRM_CHROME9_VERTEX_FORMAT_RGBA16_SNORM, 2, 0, 0 },
      { PIPE_FORMAT_R16G16_SSCALED, DRM_CHROME9_VERTEX_FORMAT_RG16_SSCALED, 1, 0, 0 },
      { PIPE_FORMAT_R16G16B16A16_SSCALED, DRM_CHROME9_VERTEX_FORMAT_RGBA16_SSCALED, 2, 0, 0 },
      { PIPE_FORMAT_R16G16_FLOAT, DRM_CHROME9_VERTEX_FORMAT_RG16_FLOAT, 1, 0, 1 },
      { PIPE_FORMAT_R16G16B16A16_FLOAT, DRM_CHROME9_VERTEX_FORMAT_RGBA16_FLOAT, 2, 0, 2 },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(table); i++) {
      if (table[i].format != format)
         continue;
      stream->drm_format = table[i].drm;
      stream->dwords = table[i].dwords;
      stream->float32 = table[i].float32;
      stream->float16 = table[i].float16;
      return true;
   }
   return false;
}

static bool
tcl_bind_inputs(struct chrome9_context *ctx, unsigned inputs,
                struct tcl_stream *streams)
{
   if (!ctx->vertex_elements || ctx->vertex_elements->count < inputs)
      return false;
   for (unsigned k = 0; k < inputs; k++) {
      const struct pipe_vertex_element *el = &ctx->vertex_elements->elements[k];
      const struct pipe_vertex_buffer *vb;
      const struct util_format_description *desc;
      const uint8_t *map;

      if (el->instance_divisor || el->dual_slot ||
          el->vertex_buffer_index >= ctx->num_vertex_buffers)
         return false;
      desc = util_format_description(el->src_format);
      if (!desc || desc->layout != UTIL_FORMAT_LAYOUT_PLAIN ||
          util_format_is_pure_integer(el->src_format) ||
          !util_format_unpack_description(el->src_format)->unpack_rgba)
         return false;
      vb = &ctx->vertex_buffers[el->vertex_buffer_index];
      map = vb->is_user_buffer ? vb->buffer.user :
            vb->buffer.resource ?
            chrome9_resource(vb->buffer.resource)->bo.map : NULL;
      if (!map)
         return false;
      streams[k].base = map + vb->buffer_offset + el->src_offset;
      streams[k].size = vb->is_user_buffer ? SIZE_MAX :
         (size_t)vb->buffer.resource->width0 >
            (size_t)vb->buffer_offset + el->src_offset ?
         vb->buffer.resource->width0 - vb->buffer_offset - el->src_offset : 0;
      streams[k].stride = el->src_stride;
      streams[k].format = el->src_format;
      streams[k].bytes = desc->block.bits / 8;
      if (!tcl_native_format(el->src_format, &streams[k])) {
         streams[k].drm_format = DRM_CHROME9_VERTEX_FORMAT_FLOAT4;
         streams[k].dwords = 0;
      }
   }
   return true;
}

static unsigned
tcl_stream_dwords(const struct tcl_stream *stream)
{
   return stream->dwords ? stream->dwords : 4;
}

/* Pack one vertex: native elements are copied, others unpacked to FLOAT4.
 * Returns the dwords written, 0 on an out-of-range or non-finite value. */
static unsigned
tcl_fetch_vertex(const struct tcl_stream *streams, unsigned inputs,
                 unsigned index, uint32_t *out)
{
   uint32_t *start = out;

   for (unsigned k = 0; k < inputs; k++) {
      const struct tcl_stream *stream = &streams[k];
      const size_t offset = (size_t)index * stream->stride;

      if (stream->size != SIZE_MAX &&
          (offset > stream->size || stream->size - offset < stream->bytes))
         return 0;
      if (!stream->dwords) {
         float value[4];

         util_format_unpack_rgba(stream->format, value, stream->base + offset,
                                 1);
         for (unsigned c = 0; c < 4; c++)
            if (!isfinite(value[c]))
               return 0;
         memcpy(out, value, sizeof(value));
         out += 4;
         continue;
      }
      /* Formats are whole dwords, so a padding-free copy is exact. */
      memset(out, 0, stream->dwords * 4);
      memcpy(out, stream->base + offset, MIN2(stream->bytes, stream->dwords * 4));
      for (unsigned c = 0; c < stream->float32; c++)
         if ((out[c] & 0x7f800000) == 0x7f800000)
            return 0;
      for (unsigned c = 0; c < stream->float16; c++)
         if ((out[c] & 0x7c00) == 0x7c00 || (out[c] & 0x7c000000) == 0x7c000000)
            return 0;
      out += stream->dwords;
   }
   return out - start;
}

/* Upper bound for one client-array span copied into the stream uploader. */
#define TCL_MAX_UPLOAD_BYTES (4U << 20)

/*
 * Stage B vertex range: the draw's biased index minimum and maximum.  The
 * indices sent are rebased to the minimum, so buffer entries start at the
 * minimum vertex and client arrays upload only the referenced span.
 */
static bool
tcl_index_range(const struct pipe_draw_info *info,
                const struct pipe_draw_start_count_bias *range,
                const uint8_t *indices, uint32_t *min, uint32_t *max)
{
   int64_t lo = INT64_MAX, hi = INT64_MIN;

   if (!info->index_size) {
      *min = range->start;
      *max = range->start + range->count - 1;
      return range->count && *max >= *min;
   }
   for (unsigned i = 0; i < range->count; i++) {
      const unsigned position = range->start + i;
      int64_t value = info->index_size == 1 ? indices[position] :
                      info->index_size == 2 ?
                      ((const uint16_t *)indices)[position] :
                      ((const uint32_t *)indices)[position];

      value += range->index_bias;
      lo = MIN2(lo, value);
      hi = MAX2(hi, value);
   }
   if (lo < 0 || hi > UINT32_MAX || lo > hi)
      return false;
   *min = lo;
   *max = hi;
   return true;
}

/*
 * Stage B: every element is fetched by the hardware from a GEM buffer.
 * Elements of one BO (or one client array) and stride share a kernel
 * buffer entry.  Client arrays are copied, from the minimum to the
 * maximum vertex, into the stream uploader; uploads[] holds the
 * references until the draw is submitted.
 */
static bool
tcl_bind_buffers(struct chrome9_context *ctx, unsigned inputs,
                 const struct tcl_stream *streams, uint32_t min, uint32_t max,
                 struct drm_chrome9_vertex_element *elements,
                 struct drm_chrome9_vertex_buffer *buffers,
                 unsigned *buffer_count, struct pipe_resource **uploads)
{
   const void *user[DRM_CHROME9_VERTEX_MAX_INPUTS];
   uint64_t low[DRM_CHROME9_VERTEX_MAX_INPUTS];
   uint64_t high[DRM_CHROME9_VERTEX_MAX_INPUTS];
   unsigned n = 0;

   for (unsigned k = 0; k < inputs; k++) {
      const struct pipe_vertex_element *el = &ctx->vertex_elements->elements[k];
      const struct pipe_vertex_buffer *vb =
         &ctx->vertex_buffers[el->vertex_buffer_index];
      const unsigned stride = el->src_stride;
      const uint64_t offset = (uint64_t)vb->buffer_offset + el->src_offset;
      const void *array = vb->is_user_buffer ? vb->buffer.user : NULL;
      uint32_t handle = 0;
      unsigned b;

      if ((!array && !vb->buffer.resource) || !streams[k].dwords ||
          stride < 4 || stride > 2044 || stride % 4 || offset % 4 ||
          offset > UINT32_MAX)
         return false;
      if (!array)
         handle = chrome9_resource(vb->buffer.resource)->bo.handle;
      for (b = 0; b < n; b++)
         if (buffers[b].stride == stride &&
             (array ? user[b] == array : !user[b] && buffers[b].handle == handle))
            break;
      if (b == n) {
         buffers[n].handle = handle;
         buffers[n].stride = stride;
         buffers[n].offset = (uint64_t)min * stride;
         user[n] = array;
         low[n] = UINT64_MAX;
         high[n] = 0;
         n++;
      }
      low[b] = MIN2(low[b], offset);
      high[b] = MAX2(high[b], offset + 4 * streams[k].dwords);
      elements[k].format = streams[k].drm_format;
      elements[k].buffer = b;
      elements[k].reserved = 0;
      elements[k].offset = offset;
   }
   for (unsigned b = 0; b < n; b++) {
      const uint64_t span = (uint64_t)(max - min) * buffers[b].stride +
                            high[b] - low[b];
      unsigned out_offset;

      if (!user[b])
         continue;
      if (span > TCL_MAX_UPLOAD_BYTES)
         return false;
      u_upload_data_ref(ctx->base.stream_uploader, 0, span, 4,
                        (const uint8_t *)user[b] +
                           (uint64_t)min * buffers[b].stride + low[b],
                        &out_offset, &uploads[b]);
      if (!uploads[b])
         return false;
      buffers[b].handle = chrome9_resource(uploads[b])->bo.handle;
      /* Vertex 0 of the rebased range now starts at out_offset. */
      buffers[b].offset = out_offset;
      for (unsigned k = 0; k < inputs; k++)
         if (elements[k].buffer == b)
            elements[k].offset -= low[b];
   }
   u_upload_unmap(ctx->base.stream_uploader);
   *buffer_count = n;
   return true;
}

/*
 * Stage B draw: triangle list indices (provoking vertex last) rebased to
 * min, in chunks.  Returns false only when nothing was submitted, so
 * stage A can retry.
 */
static bool
tcl_draw_buffers(struct chrome9_context *ctx, const struct pipe_draw_info *info,
                 const struct pipe_draw_start_count_bias *range,
                 const uint8_t *indices, unsigned triangles, uint32_t min,
                 struct chrome9_tcl_state *state,
                 struct drm_chrome9_draw_vertex_program *draw)
{
   const unsigned per_chunk = DRM_CHROME9_VERTEX_MAX_INDICES / 3;
   const int64_t bias = (int64_t)(info->index_size ? range->index_bias : 0) -
                        min;
   unsigned done = 0;

   draw->index_size = 4;
   draw->indices_ptr = (uintptr_t)state->indices;
   draw->base_vertex = 0;
   while (done < triangles) {
      const unsigned count = MIN2(per_chunk, triangles - done);
      unsigned n = 0;

      for (unsigned t = done; t < done + count; t++) {
         unsigned v[3] = { 0 };

         tcl_triangle(info->mode, t, ctx->rasterizer->flatshade_first, v);
         for (unsigned i = 0; i < 3; i++) {
            const unsigned position = range->start + v[i];
            int64_t value = position;

            if (info->index_size == 1)
               value = indices[position];
            else if (info->index_size == 2)
               value = ((const uint16_t *)indices)[position];
            else if (info->index_size == 4)
               value = ((const uint32_t *)indices)[position];
            state->indices[n++] = value + bias;
         }
      }
      draw->index_count = n;
      if (!chrome9_draw_tcl_submit(ctx, &state->fragment, draw, count)) {
         ctx->tcl_buffer_rejects++;
         if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
            fprintf(stderr, "chrome9: TCL buffers rejected (%s): buffers=%u "
                            "indices=%u min=%u textures=%u\n",
                    strerror(errno), draw->buffer_count, n, min,
                    state->fragment.draw.texture_count);
         if (!done)
            return false;
         CHROME9_REJECT(ctx, triangles - done);
         return true;
      }
      done += count;
   }
   ctx->tcl_buffer_triangles += triangles;
   return true;
}

static bool
tcl_float_bits(float value, uint32_t *bits)
{
   memcpy(bits, &value, sizeof(*bits));
   return isfinite(value);
}

static bool
tcl_viewport(const struct chrome9_context *ctx, uint32_t viewport[8])
{
   const struct pipe_viewport_state *vp = &ctx->viewport;
   const float bias = ctx->rasterizer->half_pixel_center ? 0.5f : 0.0f;
   const float ox = vp->translate[0] - bias + 786432.0f;
   const float oy = vp->translate[1] - bias + 786432.0f;

   memset(viewport, 0, 8 * sizeof(*viewport));
   /* Kernel bounds; outside them the draw module handles the draw. */
   if (fabsf(vp->scale[0]) > 2048.0f || fabsf(vp->scale[1]) > 2048.0f ||
       fabsf(2.0f * vp->scale[2]) > 1.0f ||
       fabsf(vp->translate[2] - vp->scale[2]) > 1.0f ||
       ox < 524288.0f || ox >= 1048576.0f || oy < 524288.0f ||
       oy >= 1048576.0f)
      return false;
   return tcl_float_bits(vp->scale[0], &viewport[0]) &&
          tcl_float_bits(vp->scale[1], &viewport[1]) &&
          tcl_float_bits(2.0f * vp->scale[2], &viewport[2]) &&
          tcl_float_bits(ox, &viewport[3]) &&
          tcl_float_bits(oy, &viewport[4]) &&
          tcl_float_bits(vp->translate[2] - vp->scale[2], &viewport[5]);
}

static bool
tcl_constants(struct chrome9_context *ctx, const struct chrome9_tcl_program *p,
              uint32_t (*rows)[4])
{
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *map = cb->user_buffer;

   if (p->uniform_rows) {
      if (!map && cb->buffer)
         map = chrome9_resource(cb->buffer)->bo.map;
      if (!map || cb->buffer_size < p->uniform_rows * 16)
         return false;
      /* As in chrome9_draw_vbo(): the offset applies to both kinds. */
      memcpy(rows, map + cb->buffer_offset, p->uniform_rows * 16);
      for (unsigned i = 0; i < p->uniform_rows * 4; i++) {
         float f;

         memcpy(&f, &rows[i / 4][i % 4], sizeof(f));
         if (!isfinite(f))
            return false;
      }
   }
   memcpy(rows + p->uniform_rows, p->immediates, p->immediate_count * 16);
   return true;
}

static uint32_t
tcl_cull(const struct pipe_rasterizer_state *rs)
{
   /* Mesa's screen CW/CCW (chrome9_triangle_stencil) is the kernel's. */
   const uint32_t front = rs->front_ccw ? DRM_CHROME9_VERTEX_CULL_CCW :
                                          DRM_CHROME9_VERTEX_CULL_CW;
   const uint32_t back = rs->front_ccw ? DRM_CHROME9_VERTEX_CULL_CW :
                                         DRM_CHROME9_VERTEX_CULL_CCW;

   return rs->cull_face == PIPE_FACE_BACK ? back :
          rs->cull_face == PIPE_FACE_FRONT ? front : 0;
}

static const struct chrome9_tcl_program *
tcl_program(struct chrome9_context *ctx, bool clip_halfz)
{
   struct chrome9_tcl_shader *shader = ctx->vs->tcl;
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *map = cb->user_buffer;
   struct chrome9_tcl_variant **link, *variant;
   uint32_t key[CHROME9_TCL_MAX_KEY] = { 0 };

   if (shader->key_count) {
      if (!map && cb->buffer)
         map = chrome9_resource(cb->buffer)->bo.map;
      if (!map)
         return NULL;
      map += cb->buffer_offset;
      for (unsigned k = 0; k < shader->key_count; k++) {
         if ((shader->key_dword[k] + 1u) * 4 > cb->buffer_size)
            return NULL;
         memcpy(&key[k], map + shader->key_dword[k] * 4, sizeof(key[k]));
      }
   }
   /* Most recently used first; the tail is reused when the list is full. */
   for (link = &shader->variants; (variant = *link); link = &variant->next) {
      if (variant->clip_halfz == clip_halfz &&
          !memcmp(variant->key, key, sizeof(key))) {
         *link = variant->next;
         variant->next = shader->variants;
         shader->variants = variant;
         return variant->program.supported ? &variant->program : NULL;
      }
      if (!variant->next &&
          shader->variant_count >= CHROME9_TCL_MAX_VARIANTS) {
         *link = NULL;
         shader->variant_count--;
         break;
      }
   }
   if (!variant)
      variant = CALLOC_STRUCT(chrome9_tcl_variant);
   if (!variant)
      return NULL;
   variant->clip_halfz = clip_halfz;
   memcpy(variant->key, key, sizeof(key));
   chrome9_tcl_compile(shader->nir, clip_halfz, shader->key_count,
                       shader->key_dword, key, &variant->program);
   variant->next = shader->variants;
   shader->variants = variant;
   shader->variant_count++;
   return variant->program.supported ? &variant->program : NULL;
}

/* Value class of one varying lane for all vertices of a draw. */
static enum chrome9_tcl_origin
tcl_lane_origin(const struct chrome9_context *ctx,
                const struct chrome9_tcl_program *p, unsigned lane)
{
   const unsigned v = lane / 4, c = lane % 4;
   const struct util_format_description *desc;
   unsigned input, channel;

   if (v >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
      return CHROME9_TCL_ORIGIN_UNKNOWN;
   if (p->origin[v][c] != CHROME9_TCL_ORIGIN_INPUT)
      return p->origin[v][c];
   input = p->origin_input[v][c];
   channel = p->origin_lane[v][c];
   desc = util_format_description(
      ctx->vertex_elements->elements[input].src_format);
   /* Unfetched lanes read the (0, 0, 0, 1) default. */
   if (channel >= desc->nr_channels)
      return channel == 3 ? CHROME9_TCL_ORIGIN_ONE : CHROME9_TCL_ORIGIN_ZERO;
   if (desc->channel[channel].normalized &&
       desc->channel[channel].type == UTIL_FORMAT_TYPE_UNSIGNED)
      return CHROME9_TCL_ORIGIN_UNIT;
   return CHROME9_TCL_ORIGIN_UNKNOWN;
}

/* The guarded fragment forms need per-lane facts the CPU can no longer
 * check per vertex; accept them only when the VS proves them statically. */
static bool
tcl_guarded_domain(const struct chrome9_context *ctx,
                   const struct chrome9_tcl_program *p)
{
   const struct chrome9_shader *fs = ctx->fs;

   for (uint32_t m = fs->semantic_identity_projector_input_mask; m; m &= m - 1)
      if (tcl_lane_origin(ctx, p, __builtin_ctz(m)) != CHROME9_TCL_ORIGIN_ONE)
         return false;
   for (uint32_t m = fs->semantic_equal_zero_input_mask; m; m &= m - 1) {
      const enum chrome9_tcl_origin o = tcl_lane_origin(ctx, p,
                                                        __builtin_ctz(m));

      if (o != CHROME9_TCL_ORIGIN_ZERO && o != CHROME9_TCL_ORIGIN_ONE)
         return false;
   }
   for (uint32_t m = fs->semantic_unit_interval_input_mask; m; m &= m - 1)
      if (tcl_lane_origin(ctx, p, __builtin_ctz(m)) ==
          CHROME9_TCL_ORIGIN_UNKNOWN)
         return false;
   return true;
}

static void
tcl_dump(const struct drm_chrome9_draw_vertex_program *draw,
         const struct chrome9_tcl_program *p,
         const struct chrome9_tcl_fragment *fragment)
{
   static unsigned dumps;

   if (dumps++ >= 4)
      return;
   fprintf(stderr, "chrome9: TCL rejected (errno %d): inputs=%u rows=%u+%u "
                   "flags=%#x fs varyings=%u fs instructions=%u\n", errno,
           p->input_count, p->uniform_rows, p->immediate_count, draw->flags,
           fragment->draw.varying_count, fragment->draw.instruction_count);
   for (unsigned i = 0; i < p->instruction_count; i++) {
      const struct drm_chrome9_vertex_instruction *in = &p->instructions[i];

      fprintf(stderr, "  %2u op%-2u d%u.%u/%x%s", i, in->op, in->dst.file,
              in->dst.index, in->dst.writemask, in->dst.modifier ? "s" : "");
      for (unsigned s = 0; s < 3; s++)
         fprintf(stderr, "  %c%u.%u.%u%u%u%u", in->src[s].modifier ? '-' : ' ',
                 in->src[s].file, in->src[s].index, in->src[s].swizzle[0],
                 in->src[s].swizzle[1], in->src[s].swizzle[2],
                 in->src[s].swizzle[3]);
      fprintf(stderr, "\n");
   }
   for (unsigned i = 0; i < fragment->draw.instruction_count; i++) {
      const struct drm_chrome9_fragment_instruction *in =
         &fragment->instructions[i];

      fprintf(stderr, "  fs%2u op%-2u d%u.%u/%x", i, in->op, in->dst.file,
              in->dst.index, in->dst.writemask);
      for (unsigned s = 0; s < 3; s++)
         fprintf(stderr, "  %u.%u.%u%u%u%u", in->src[s].file,
                 in->src[s].index, in->src[s].swizzle[0],
                 in->src[s].swizzle[1], in->src[s].swizzle[2],
                 in->src[s].swizzle[3]);
      fprintf(stderr, "\n");
   }
}

/* Flat interpolation: the hardware flat mode covers colors only. */
static bool
tcl_fs_flat(const struct chrome9_shader *fs, bool *primary)
{
   const nir_shader *nir = fs->nir;

   *primary = false;
   if (!nir)
      return true;
   nir_foreach_shader_in_variable(var, nir) {
      if (var->data.interpolation != INTERP_MODE_FLAT)
         continue;
      if (var->data.location == VARYING_SLOT_COL0)
         *primary = true;
      else
         return false;
   }
   return true;
}

/* CHROME9_DEBUG_TCL=1 names each new fallback reason once. */
static bool
tcl_fallback(struct chrome9_context *ctx, const char *reason)
{
   static const char *last;

   if (reason != last && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL fallback: %s (tcl triangles so far %" PRIu64
                      ", fs program=%u semantic=%u inputs=%#x)\n", reason,
              ctx->tcl_triangles, ctx->fs ? ctx->fs->fragment_program : 0,
              ctx->fs ? ctx->fs->semantic_fs_valid : 0,
              ctx->fs ? ctx->fs->semantic_input_mask : 0);
   last = reason;
   ctx->tcl_fallback_draws++;
   return false;
}

bool
chrome9_tcl_draw(struct chrome9_context *ctx, const struct pipe_draw_info *info,
                 const struct pipe_draw_start_count_bias *range)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   const struct chrome9_tcl_program *p;
   struct chrome9_tcl_state *state;
   struct drm_chrome9_vertex_element elements[DRM_CHROME9_VERTEX_MAX_INPUTS];
   struct tcl_stream streams[DRM_CHROME9_VERTEX_MAX_INPUTS];
   struct drm_chrome9_draw_vertex_program draw = { 0 };
   struct drm_chrome9_vertex_buffer buffers[DRM_CHROME9_VERTEX_MAX_INPUTS];
   const uint8_t *indices = NULL;
   const char *why;
   bool flat;
   unsigned triangles, stride, per_chunk, buffer_count, done = 0;
   uint32_t index_min, index_max;

   if (!ctx->tcl_enabled)
      return false;
   if (!ctx->vs || !ctx->vs->tcl || !rs || !ctx->fs)
      return tcl_fallback(ctx, "no NIR vertex shader");
   if (info->primitive_restart || info->instance_count != 1 ||
       info->start_instance)
      return tcl_fallback(ctx, "restart or instancing");
   if (rs->fill_front != PIPE_POLYGON_MODE_FILL ||
       rs->fill_back != PIPE_POLYGON_MODE_FILL || rs->light_twoside ||
       rs->clip_plane_enable || !rs->depth_clip_near ||
       !rs->depth_clip_far || rs->rasterizer_discard)
      return tcl_fallback(ctx, "rasterizer state");
   if (!tcl_fs_flat(ctx->fs, &flat))
      return tcl_fallback(ctx, "flat generic varying");
   flat |= rs->flatshade;
   triangles = tcl_triangle_count(info->mode, range->count);
   if (!triangles && !tcl_triangle(info->mode, 0, false, (unsigned[3]){ 0 }))
      return tcl_fallback(ctx, "primitive type");
   if (!triangles)
      return true;
   if (rs->cull_face == PIPE_FACE_FRONT_AND_BACK)
      return true;
   p = tcl_program(ctx, rs->clip_halfz);
   if (!p)
      return tcl_fallback(ctx, "vertex shader not expressible");
   if (!tcl_bind_inputs(ctx, p->input_count, streams))
      return tcl_fallback(ctx, "vertex element format or buffer");
   if (!tcl_guarded_domain(ctx, p))
      return tcl_fallback(ctx, "guarded fragment domain");
   if (info->index_size) {
      indices = info->has_user_indices ? info->index.user :
                info->index.resource ?
                chrome9_resource(info->index.resource)->bo.map : NULL;
      if (!indices || (info->index_size != 1 && info->index_size != 2 &&
                       info->index_size != 4) ||
          (!info->has_user_indices &&
           ((uint64_t)range->start + range->count) * info->index_size >
              info->index.resource->width0))
         return false;
   }
   state = ctx->tcl_state;
   if (!state) {
      state = ctx->tcl_state = CALLOC_STRUCT(chrome9_tcl_state);
      if (!state)
         return false;
   }
   if (!tcl_viewport(ctx, draw.viewport))
      return tcl_fallback(ctx, "viewport");
   if (!tcl_constants(ctx, p, state->constants))
      return tcl_fallback(ctx, "constants");
   if (!chrome9_draw_tcl_fragment(ctx, &state->fragment, &why))
      return tcl_fallback(ctx, why);

   draw.abi_version = DRM_CHROME9_VERTEX_ABI_VERSION;
   draw.flags = tcl_cull(rs) | (flat ? DRM_CHROME9_VERTEX_FLAT : 0);
   draw.instruction_count = p->instruction_count;
   draw.instructions_ptr = (uintptr_t)p->instructions;
   draw.element_count = p->input_count;
   draw.elements_ptr = (uintptr_t)elements;
   draw.constant_count = p->uniform_rows + p->immediate_count;
   draw.constants_ptr = draw.constant_count ? (uintptr_t)state->constants : 0;
   if (ctx->tcl_buffers &&
       tcl_index_range(info, range, indices, &index_min, &index_max)) {
      struct pipe_resource *uploads[DRM_CHROME9_VERTEX_MAX_INPUTS] = { 0 };
      bool drawn = tcl_bind_buffers(ctx, p->input_count, streams, index_min,
                                    index_max, elements, buffers,
                                    &buffer_count, uploads);

      if (drawn) {
         draw.buffer_count = buffer_count;
         draw.buffers_ptr = (uintptr_t)buffers;
         drawn = tcl_draw_buffers(ctx, info, range, indices, triangles,
                                  index_min, state, &draw);
      }
      for (unsigned b = 0; b < ARRAY_SIZE(uploads); b++)
         pipe_resource_reference(&uploads[b], NULL);
      if (drawn) {
         ctx->tcl_triangles += triangles;
         return true;
      }
      /* Stage A below; clear every stage B field. */
      draw.buffer_count = 0;
      draw.buffers_ptr = 0;
      draw.index_count = 0;
      draw.index_size = 0;
      draw.indices_ptr = 0;
      draw.base_vertex = 0;
   }

   /* Packed vertices: the stride is the sum of element fetch dwords. */
   stride = 0;
   for (unsigned k = 0; k < p->input_count; k++) {
      elements[k].format = streams[k].drm_format;
      elements[k].buffer = 0;
      elements[k].reserved = 0;
      elements[k].offset = stride;
      stride += 4 * tcl_stream_dwords(&streams[k]);
   }
   per_chunk = MIN2(TCL_MAX_TRIANGLES, TCL_MAX_VERTEX_BYTES / (3 * stride));
   draw.vertex_stride = stride;
   draw.vertices_ptr = (uintptr_t)state->vertices;

   while (done < triangles) {
      const unsigned count = MIN2(per_chunk, triangles - done);
      uint32_t *out = state->vertices;

      for (unsigned t = done; t < done + count; t++) {
         unsigned v[3];

         tcl_triangle(info->mode, t, rs->flatshade_first, v);
         for (unsigned i = 0; i < 3; i++) {
            const unsigned position = range->start + v[i];
            unsigned index = position;

            if (info->index_size == 1)
               index = indices[position];
            else if (info->index_size == 2)
               index = ((const uint16_t *)indices)[position];
            else if (info->index_size == 4)
               index = ((const uint32_t *)indices)[position];
            if (info->index_size)
               index += range->index_bias;
            unsigned written = tcl_fetch_vertex(streams, p->input_count,
                                                index, out);

            if (!written) {
               if (!done)
                  return tcl_fallback(ctx, "vertex fetch");
               CHROME9_REJECT(ctx, triangles - done);
               return true;
            }
            out += written;
         }
      }
      draw.vertex_count = 3 * count;
      if (!chrome9_draw_tcl_submit(ctx, &state->fragment, &draw, count)) {
         /* A first-chunk rejection falls back to the draw module. */
         if (!done) {
            if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
               tcl_dump(&draw, p, &state->fragment);
            return tcl_fallback(ctx, "kernel rejected the draw");
         }
         CHROME9_REJECT(ctx, triangles - done);
         return true;
      }
      done += count;
   }
   ctx->tcl_triangles += triangles;
   return true;
}
