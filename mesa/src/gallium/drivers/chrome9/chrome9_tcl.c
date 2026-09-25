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
#include "chrome9_swfb.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_range_analysis.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/hash_table.h"

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
/* Kernel structural limit (D3D9 static flow depth); the cap grants one. */
#define TCL_MAX_FLOW_DEPTH 4
/* instr->pass_flags of code a REP makes redundant (its counter). */
#define TCL_SKIP 1
#define TCL_EXPR_LITERAL 0xffff
#define TCL_EXPR_DWORD 0xfffe
/* Base rows one a0 lane serves (a mat4 element); the pad keeps the lane
 * clamp from cutting reads of the lower bases. */
#define TCL_RELATIVE_SPREAD 3
/* f2i32 plus folded additions and ftrunc of one address. */
#define TCL_ADDRESS_CHAIN 16

/* How to read the components of one NIR value. */
struct tcl_value {
   bool valid;
   uint8_t file;
   uint8_t index;
   uint8_t sel[4];
   bool neg;
   /* CONSTANT only: 1..4 reads c[index + a0.x..w]. */
   uint8_t relative;
   /* Flow region a temp was defined in; zero is the top level. */
   uint16_t scope;
};

/* One a0 lane: the index scalar it holds for reads at bases
 * [base_min, base_max], and the region of its last ARL. */
struct tcl_address {
   nir_scalar index;
   unsigned base_min;
   unsigned base_max;
   bool written;
   uint16_t scope;
};

/* Shortest non-indexed draw worth indexing its repeated vertices. */
#define TCL_DEDUPE_MIN_VERTICES 3072
#define TCL_DEDUPE_ENTRIES 8
/* Only nearby repeats: far references fetch cold vertices and widen the
 * chunk's index range, which was slower than transforming again. */
#define TCL_DEDUPE_WINDOW 64

struct tcl_dedupe_stream {
   uint64_t serial;
   uint64_t offset;
   uint32_t generation;
   unsigned stride;
   unsigned bytes;
};

/* Vertex repetition of one non-indexed draw over unchanged buffers. */
struct tcl_dedupe {
   struct tcl_dedupe_stream streams[DRM_CHROME9_VERTEX_MAX_INPUTS];
   unsigned inputs;
   unsigned start;
   unsigned count;
   uint64_t last_use;
   bool valid;
   /* The map is built on the second draw of unchanged buffers, so data
    * rewritten every frame never pays for hashing. */
   bool built;
   /* Draw-relative position of the first equal vertex; NULL when too few
    * vertices repeat. */
   uint32_t *first;
};

/*
 * The distinct vertices of a non-indexed draw over unchanged buffers,
 * copied into one array, and the draw as indices into it.  VS texture
 * lookups are then sampled once per distinct vertex.
 */
struct tcl_compact {
   struct tcl_dedupe_stream streams[DRM_CHROME9_VERTEX_MAX_INPUTS];
   unsigned inputs;
   unsigned start;
   unsigned count;
   bool valid;
   bool built;
   uint32_t *index;
   uint8_t *vertices;
   /* The vertices in a vertex buffer, written once. */
   struct pipe_resource *buffer;
   unsigned unique;
   unsigned stride;
   unsigned offset[DRM_CHROME9_VERTEX_MAX_INPUTS];
};

struct chrome9_tcl_state {
   struct chrome9_tcl_fragment fragment;
   struct tcl_compact compact;
   struct tcl_dedupe dedupe[TCL_DEDUPE_ENTRIES];
   uint64_t dedupe_clock;
   uint32_t constants[DRM_CHROME9_VERTEX_MAX_CONSTANTS][4];
   uint32_t vertices[TCL_MAX_VERTEX_BYTES / sizeof(uint32_t)];
   /* One stage B chunk; the kernel bounds index_count per draw. */
   uint32_t indices[DRM_CHROME9_VERTEX_MAX_INDICES];
   uint8_t ints[DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS][4];
   /* Coordinates of the prefetched VS texture lookups. */
   float *coords;
   size_t prefetch_coords;
   /* Vertex buffers the lookups are written to, in turn. */
   struct pipe_resource *prefetch_ring[3];
   unsigned prefetch_next;
   /* Draw-relative vertex positions of the current chunk. */
   unsigned positions[MAX2(DRM_CHROME9_VERTEX_MAX_INDICES,
                           DRM_CHROME9_VERTEX_MAX_VERTICES)];
};

struct tcl_builder {
   struct chrome9_tcl_program *p;
   struct tcl_value *values;
   unsigned next_temp;
   bool failed;
   enum chrome9_tcl_colour colour;
   /* The shader writes BFC0 (for CHROME9_TCL_COLOUR_BACK). */
   bool writes_back;
   unsigned clip_enable;
   bool point_size;
   bool unit_temp[DRM_CHROME9_VERTEX_MAX_TEMPS];
   /* Lanes written per fragment varying and of the primary colour. */
   uint8_t varying_written[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
   uint8_t primary_written;
   /* Whole-register copies used to legalize read ports. */
   int16_t constant_temp[DRM_CHROME9_VERTEX_MAX_CONSTANTS];
   int16_t input_temp[DRM_CHROME9_VERTEX_MAX_INPUTS];
   const struct chrome9_tcl_flow *flow;
   nir_function_impl *impl;
   unsigned depth;
   unsigned scope_serial;
   uint16_t scope[TCL_MAX_FLOW_DEPTH + 1];
   /* NIR scalar of each program expression node. */
   struct util_dynarray expr_keys;
   /* Ifs lowered to IF b#: their condition is evaluated on the CPU. */
   struct util_dynarray uniform_ifs;
   bool relative;
   unsigned address_count;
   struct tcl_address address[4];
   nir_fp_analysis_state fp;
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
   /* Vector selects picked wrong lanes on silicon (mr3.shader_test,
    * vs-temp-array-mat4-col-row-wr); scalar selects are exact. */
   case nir_op_bcsel:
   case nir_op_fcsel:
   /* Vector equality reductions become scalar compares. */
   case nir_op_ball_fequal2:
   case nir_op_ball_fequal3:
   case nir_op_ball_fequal4:
   case nir_op_bany_fnequal2:
   case nir_op_bany_fnequal3:
   case nir_op_bany_fnequal4:
      return 1;
   default:
      return 0;
   }
}

/* Values are float-encoded: integer equality reductions that the
 * optimizer forms after the bool lowering compare as floats. */
static bool
tcl_float_reduction(nir_builder *b, nir_alu_instr *alu, void *data)
{
   static const nir_op map[][2] = {
      { nir_op_ball_iequal2, nir_op_ball_fequal2 },
      { nir_op_ball_iequal3, nir_op_ball_fequal3 },
      { nir_op_ball_iequal4, nir_op_ball_fequal4 },
      { nir_op_bany_inequal2, nir_op_bany_fnequal2 },
      { nir_op_bany_inequal3, nir_op_bany_fnequal3 },
      { nir_op_bany_inequal4, nir_op_bany_fnequal4 },
   };

   (void)b;
   (void)data;
   for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
      if (alu->op == map[i][0]) {
         alu->op = map[i][1];
         return true;
      }
   }
   return false;
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

/* GL rounds the size of aliased points to the nearest integer; the
 * hardware rounding of fractional sizes is unmeasured. */
static bool
tcl_round_point_size(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output ||
       nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_PSIZ)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_src_rewrite(&intr->src[0],
                   nir_ffloor(b, nir_fadd_imm(b, intr->src[0].ssa, 0.5)));
   return true;
}

/*
 * GL 2.1 2.12 discards a point whose vertex lies outside the clip volume;
 * the clip engine draws the visible part instead (rung pedge).  Move such
 * a point behind the near plane, z = 0 after the half-z lowering.
 */
static bool
tcl_clip_point(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   unsigned *stores = data;
   nir_def *pos, *outside, *z;

   if (intr->intrinsic != nir_intrinsic_store_output ||
       nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;
   (*stores)++;
   if (nir_intrinsic_component(intr) || nir_intrinsic_write_mask(intr) != 0xf ||
       intr->src[0].ssa->num_components != 4) {
      (*stores)++;
      return false;
   }
   b->cursor = nir_before_instr(&intr->instr);
   pos = intr->src[0].ssa;
   outside = nir_b2f32(b, nir_flt(b, nir_channel(b, pos, 3),
                                  nir_fmax(b, nir_fabs(b, nir_channel(b, pos, 0)),
                                           nir_fabs(b, nir_channel(b, pos, 1)))));
   z = nir_ffma(b, outside,
                nir_fadd_imm(b, nir_fmul_imm(b, nir_fabs(b, nir_channel(b, pos, 3)),
                                             -2.0), -1.0),
                nir_channel(b, pos, 2));
   nir_src_rewrite(&intr->src[0],
                   nir_vec4(b, nir_channel(b, pos, 0), nir_channel(b, pos, 1),
                            z, nir_channel(b, pos, 3)));
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

/* Selects whose condition lanes are constant (inlined control uniforms)
 * become plain lane moves. */
static bool
tcl_fold_const_select(nir_builder *b, nir_alu_instr *alu, void *data)
{
   nir_scalar lanes[NIR_MAX_VEC_COMPONENTS];
   const unsigned n = alu->def.num_components;

   (void)data;
   if (alu->op != nir_op_bcsel && alu->op != nir_op_fcsel &&
       alu->op != nir_op_b32csel)
      return false;
   for (unsigned c = 0; c < n; c++) {
      nir_scalar cond = nir_scalar_chase_alu_src(nir_get_scalar(&alu->def, c),
                                                 0);
      bool take;

      if (!nir_scalar_is_const(cond))
         return false;
      take = alu->op == nir_op_fcsel ? nir_scalar_as_float(cond) != 0.0 :
                                       nir_scalar_as_bool(cond);
      lanes[c] = nir_scalar_chase_alu_src(nir_get_scalar(&alu->def, c),
                                          take ? 1 : 2);
   }
   b->cursor = nir_before_instr(&alu->instr);
   nir_def_replace(&alu->def, nir_vec_scalars(b, lanes, n));
   return true;
}

static bool
tcl_is_select(const nir_alu_instr *alu)
{
   return alu->op == nir_op_fcsel || alu->op == nir_op_fcsel_gt ||
          alu->op == nir_op_fcsel_ge;
}

/* A select is MAD(a, mask, MAD(-b, mask, b)); an operand that overflowed
 * to infinity turns the masked-off term into NaN.  Mark arithmetic
 * operands that may be infinite in pass_flags bits 1 and 2. */
static bool
tcl_select_mark_infinite(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)b;
   alu->instr.pass_flags = 0;
   if (!tcl_is_select(alu))
      return false;
   for (unsigned s = 1; s < 3; s++) {
      nir_def *def = alu->src[s].src.ssa;
      nir_alu_instr *src = nir_def_as_alu_or_null(def);

      if (src && (src->op == nir_op_fmul || src->op == nir_op_ffma ||
                  src->op == nir_op_fadd || src->op == nir_op_fsub) &&
          (nir_analyze_fp_class(data, def) &
           (FP_CLASS_NEG_INF | FP_CLASS_POS_INF)))
         alu->instr.pass_flags |= BITFIELD_BIT(s);
   }
   return false;
}

/* Clamp the marked operands to the finite range. */
static bool
tcl_select_finite(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)data;
   if (!tcl_is_select(alu) || !alu->instr.pass_flags)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   u_foreach_bit(s, alu->instr.pass_flags) {
      nir_def *def = nir_fmin(b, nir_fmax(b, alu->src[s].src.ssa,
                                          nir_imm_float(b, -FLT_MAX)),
                              nir_imm_float(b, FLT_MAX));

      nir_src_rewrite(&alu->src[s].src, def);
   }
   alu->instr.pass_flags = 0;
   return true;
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
      NIR_PASS(progress, nir, nir_shader_alu_pass, tcl_fold_const_select,
               nir_metadata_control_flow, NULL);
      NIR_PASS(progress, nir, nir_opt_vectorize, tcl_vector_width, NULL);
   } while (progress);
}

/* Source operands of a vertex op. */
static unsigned
tcl_op_sources(unsigned op)
{
   switch (op) {
   case DRM_CHROME9_VERTEX_OP_MAD:
      return 3;
   case DRM_CHROME9_VERTEX_OP_ADD:
   case DRM_CHROME9_VERTEX_OP_SUB:
   case DRM_CHROME9_VERTEX_OP_MUL:
   case DRM_CHROME9_VERTEX_OP_DP3:
   case DRM_CHROME9_VERTEX_OP_DP4:
   case DRM_CHROME9_VERTEX_OP_DPH:
   case DRM_CHROME9_VERTEX_OP_MIN:
   case DRM_CHROME9_VERTEX_OP_MAX:
   case DRM_CHROME9_VERTEX_OP_SGE:
   case DRM_CHROME9_VERTEX_OP_SLT:
      return 2;
   default:
      return 1;
   }
}

/* SGN reads source lane w and writes it to every enabled lane on silicon. */
static bool
tcl_op_scalar(unsigned op)
{
   return op == DRM_CHROME9_VERTEX_OP_SGN ||
          (op >= DRM_CHROME9_VERTEX_OP_RCP && op <= DRM_CHROME9_VERTEX_OP_LOG2);
}

static bool
tcl_op_dot(unsigned op)
{
   return op == DRM_CHROME9_VERTEX_OP_DP3 || op == DRM_CHROME9_VERTEX_OP_DP4 ||
          op == DRM_CHROME9_VERTEX_OP_DPH;
}

/* Selectors of src that the op reads. */
static unsigned
tcl_source_lanes(const struct drm_chrome9_vertex_instruction *insn,
                 unsigned src)
{
   switch (insn->op) {
   case DRM_CHROME9_VERTEX_OP_DP3:
      return 0x7;
   case DRM_CHROME9_VERTEX_OP_DPH:
      return src ? 0xf : 0x7;
   case DRM_CHROME9_VERTEX_OP_DP4:
   case DRM_CHROME9_VERTEX_OP_SGN:
   case DRM_CHROME9_VERTEX_OP_RCP:
   case DRM_CHROME9_VERTEX_OP_RSQ:
   case DRM_CHROME9_VERTEX_OP_EXP2:
   case DRM_CHROME9_VERTEX_OP_LOG2:
      return 0xf;
   default:
      return insn->dst.writemask;
   }
}

/* Maximum number of temps live at one instruction. */
static unsigned
tcl_temp_pressure(const struct chrome9_tcl_program *p, unsigned temp_count,
                  int *first, int *last)
{
   unsigned pressure = 0;

   for (unsigned t = 0; t < temp_count; t++)
      first[t] = last[t] = -1;
   for (unsigned i = 0; i < p->instruction_count; i++) {
      const struct drm_chrome9_vertex_instruction *insn = &p->instructions[i];

      for (unsigned s = 0; s < tcl_op_sources(insn->op); s++)
         if (insn->src[s].file == DRM_CHROME9_VERTEX_FILE_TEMP &&
             insn->src[s].index < temp_count && tcl_source_lanes(insn, s))
            last[insn->src[s].index] = i;
      if (insn->dst.file == DRM_CHROME9_VERTEX_DEST_TEMP &&
          insn->dst.index < temp_count) {
         if (first[insn->dst.index] < 0)
            first[insn->dst.index] = i;
         last[insn->dst.index] = i;
      }
   }
   for (unsigned i = 0; i < p->instruction_count; i++) {
      unsigned live = 0;

      for (unsigned t = 0; t < temp_count; t++)
         live += first[t] >= 0 && first[t] <= (int)i && last[t] >= (int)i;
      pressure = MAX2(pressure, live);
   }
   return pressure;
}

static unsigned
tcl_lane_root(uint16_t *parent, unsigned v)
{
   while (parent[v] != v)
      v = parent[v] = parent[parent[v]];
   return v;
}

/*
 * The kernel maps every temp to one of 16 physical vec4 registers for its
 * whole live range.  Selects and other lane-wise sequences keep one live
 * lane per temp, so a program with many of them fails with -E2BIG.  Lanes
 * written by one instruction or read by one source stay together; such
 * groups move into free lanes of temps that are live anyway.  Lanes are
 * written once (the kernel rejects redefinition).  Straight-line programs
 * only; the program is kept when the pressure does not drop.
 */
static void
tcl_pack_temp_lanes(struct chrome9_tcl_program *p, unsigned temp_count)
{
   enum { LANES = DRM_CHROME9_VERTEX_MAX_TEMPS * 4 };
   const unsigned n = p->instruction_count;
   struct drm_chrome9_vertex_instruction *saved;
   int first[DRM_CHROME9_VERTEX_MAX_TEMPS], last[DRM_CHROME9_VERTEX_MAX_TEMPS];
   int def[LANES], end[LANES], host_last[DRM_CHROME9_VERTEX_MAX_TEMPS];
   uint16_t parent[LANES];
   uint8_t host[LANES], lane[LANES], host_used[DRM_CHROME9_VERTEX_MAX_TEMPS];
   unsigned hosts = 0, before, after;

   if (temp_count > DRM_CHROME9_VERTEX_MAX_TEMPS)
      return;
   before = tcl_temp_pressure(p, temp_count, first, last);
   if (debug_get_bool_option("CHROME9_DEBUG_TCL_DUMP", false))
      for (unsigned i = 0; i < n; i++) {
         const struct drm_chrome9_vertex_instruction *insn = &p->instructions[i];

         fprintf(stderr, "TCLDUMP %u %u %u %u %u", insn->op, insn->dst.file,
                 insn->dst.index, insn->dst.writemask, insn->dst.modifier);
         for (unsigned s = 0; s < 3; s++)
            fprintf(stderr, " %u %u %u %u %u %u %u %u", insn->src[s].file,
                    insn->src[s].index, insn->src[s].swizzle[0],
                    insn->src[s].swizzle[1], insn->src[s].swizzle[2],
                    insn->src[s].swizzle[3], insn->src[s].modifier,
                    insn->src[s].relative);
         fputc('\n', stderr);
      }
   /* The kernel also needs scratch registers for read-port copies. */
   if (before < 12)
      return;
   for (unsigned v = 0; v < temp_count * 4; v++) {
      parent[v] = v;
      def[v] = end[v] = -1;
   }
   for (unsigned i = 0; i < n; i++) {
      const struct drm_chrome9_vertex_instruction *insn = &p->instructions[i];

      if (insn->op >= DRM_CHROME9_VERTEX_OP_REP &&
          insn->op != DRM_CHROME9_VERTEX_OP_ARL)
         return;
      for (unsigned s = 0; s < tcl_op_sources(insn->op); s++) {
         const struct drm_chrome9_vertex_source *src = &insn->src[s];
         const unsigned used = tcl_source_lanes(insn, s);
         int joined = -1;

         if (src->file != DRM_CHROME9_VERTEX_FILE_TEMP)
            continue;
         for (unsigned c = 0; c < 4; c++) {
            unsigned v;

            if (!(used & BITFIELD_BIT(c)) || src->swizzle[c] > 3)
               continue;
            v = src->index * 4 + src->swizzle[c];
            if (src->index >= temp_count || def[v] < 0)
               return;
            end[v] = i;
            if (joined >= 0)
               parent[tcl_lane_root(parent, v)] = tcl_lane_root(parent, joined);
            joined = v;
         }
      }
      if (insn->dst.file == DRM_CHROME9_VERTEX_DEST_TEMP) {
         int joined = -1;

         if (insn->dst.index >= temp_count)
            return;
         u_foreach_bit(c, insn->dst.writemask) {
            const unsigned v = insn->dst.index * 4 + c;

            def[v] = i;
            end[v] = MAX2(end[v], (int)i);
            if (joined >= 0)
               parent[tcl_lane_root(parent, v)] = tcl_lane_root(parent, joined);
            joined = v;
         }
      }
   }
   /* Groups in order of their first definition; best fit into a live host. */
   for (unsigned i = 0; i < n; i++) {
      const struct drm_chrome9_vertex_instruction *insn = &p->instructions[i];

      if (insn->dst.file != DRM_CHROME9_VERTEX_DEST_TEMP)
         continue;
      u_foreach_bit(c, insn->dst.writemask) {
         const unsigned root = tcl_lane_root(parent, insn->dst.index * 4 + c);
         unsigned size = 0, h = hosts, next;
         int start = INT_MAX, stop = -1;

         if (def[root] == -2)
            continue;
         for (unsigned v = 0; v < temp_count * 4; v++)
            if (def[v] >= 0 && tcl_lane_root(parent, v) == root) {
               size++;
               start = MIN2(start, def[v]);
               stop = MAX2(stop, end[v]);
            }
         /* Values ending close together share a host, so a long-lived
          * lane never keeps a register of scratch lanes alive. */
         for (unsigned k = 0; k < hosts; k++)
            if (host_last[k] >= start && host_used[k] + size <= 4 &&
                abs(host_last[k] - stop) <= 8 &&
                (h == hosts ||
                 abs(host_last[k] - stop) < abs(host_last[h] - stop)))
               h = k;
         if (h == hosts) {
            if (hosts == temp_count)
               return;
            hosts++;
            host_used[h] = 0;
            host_last[h] = -1;
         }
         next = host_used[h];
         for (unsigned v = 0; v < temp_count * 4; v++)
            if (def[v] >= 0 && tcl_lane_root(parent, v) == root) {
               host[v] = h;
               lane[v] = next++;
            }
         host_used[h] = next;
         host_last[h] = MAX2(host_last[h], stop);
         /* Placed; the root's own def is recorded in host/lane. */
         def[root] = -2;
      }
   }
   saved = MALLOC(n * sizeof(*saved));
   if (!saved)
      return;
   memcpy(saved, p->instructions, n * sizeof(*saved));
   for (unsigned i = 0; i < n; i++) {
      struct drm_chrome9_vertex_instruction *insn = &p->instructions[i];
      const bool temp_dst = insn->dst.file == DRM_CHROME9_VERTEX_DEST_TEMP;
      const unsigned d = insn->dst.index;
      uint8_t writemask = 0;

      /* Lane-wise ops move their selectors with the destination lanes. */
      for (unsigned s = 0; s < tcl_op_sources(insn->op); s++) {
         struct drm_chrome9_vertex_source *src = &insn->src[s];
         const unsigned t = src->index;
         uint8_t swizzle[4];
         unsigned used = tcl_source_lanes(insn, s), fill = TCL_SEL_ZERO;

         memcpy(swizzle, src->swizzle, sizeof(swizzle));
         if (temp_dst && !tcl_op_dot(insn->op) &&
             !tcl_op_scalar(insn->op)) {
            used = 0;
            u_foreach_bit(c, insn->dst.writemask) {
               const unsigned to = lane[d * 4 + c];

               swizzle[to] = src->swizzle[c];
               used |= BITFIELD_BIT(to);
            }
         }
         for (unsigned c = 0; c < 4; c++) {
            if (!(used & BITFIELD_BIT(c)))
               continue;
            if (src->file == DRM_CHROME9_VERTEX_FILE_TEMP &&
                swizzle[c] <= 3) {
               const unsigned v = t * 4 + swizzle[c];

               swizzle[c] = lane[v];
               src->index = host[v];
            }
            fill = swizzle[c];
         }
         for (unsigned c = 0; c < 4; c++)
            if (!(used & BITFIELD_BIT(c)))
               swizzle[c] = fill;
         memcpy(src->swizzle, swizzle, sizeof(swizzle));
      }
      if (temp_dst) {
         u_foreach_bit(c, insn->dst.writemask)
            writemask |= BITFIELD_BIT(lane[d * 4 + c]);
         insn->dst.index = host[d * 4 + __builtin_ctz(insn->dst.writemask)];
         insn->dst.writemask = writemask;
      }
   }
   after = tcl_temp_pressure(p, hosts, first, last);
   if (after >= before)
      memcpy(p->instructions, saved, n * sizeof(*saved));
   FREE(saved);
   if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL lane packing %u -> %u temps, pressure "
              "%u -> %u%s\n", temp_count, hosts, before, after,
              after >= before ? " (kept)" : "");
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

static bool
tcl_scope_active(const struct tcl_builder *b, uint16_t scope)
{
   for (unsigned d = 0; d <= b->depth; d++)
      if (b->scope[d] == scope)
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

/* Copy an a0-relative constant row into a new temp; constant_temp caches
 * absolute rows only. */
static unsigned
tcl_relative_temp(struct tcl_builder *b, const struct drm_chrome9_vertex_source *src)
{
   struct drm_chrome9_vertex_instruction mov = {
      .op = DRM_CHROME9_VERTEX_OP_MOV,
      .src = { { src->file, src->index, { 0, 1, 2, 3 }, 0, src->relative } },
   };

   mov.dst.file = DRM_CHROME9_VERTEX_DEST_TEMP;
   mov.dst.index = tcl_new_temp(b);
   mov.dst.writemask = 0xf;
   tcl_append(b, &mov);
   return mov.dst.index;
}

/*
 * Silicon rules (kernel chrome9_vertex_validate): one distinct input and
 * one distinct constant row per record, where (index, relative) names the
 * row, no direct constant source for RCP/RSQ/EXP2/LOG2 and an absolute ARL
 * source.  Offending sources read a temp copy instead.
 */
static void
tcl_emit(struct tcl_builder *b, struct drm_chrome9_vertex_instruction *insn,
         unsigned count)
{
   const bool scalar = insn->op >= DRM_CHROME9_VERTEX_OP_RCP &&
                       insn->op <= DRM_CHROME9_VERTEX_OP_LOG2;

   for (unsigned file = DRM_CHROME9_VERTEX_FILE_INPUT;
        file <= DRM_CHROME9_VERTEX_FILE_CONSTANT; file++) {
      int keep = -1, keep_relative = 0;

      for (unsigned s = 0; s < count; s++) {
         struct drm_chrome9_vertex_source *src = &insn->src[s];
         const bool copy = (scalar && file == DRM_CHROME9_VERTEX_FILE_CONSTANT) ||
                           (src->relative && insn->op == DRM_CHROME9_VERTEX_OP_ARL);

         if (src->file != file || !tcl_source_reads(src))
            continue;
         if (keep < 0 && !copy) {
            keep = src->index;
            keep_relative = src->relative;
         }
         if (src->index == keep && src->relative == keep_relative)
            continue;
         src->index = src->relative ? tcl_relative_temp(b, src) :
                      tcl_register_temp(b, file, src->index);
         src->file = DRM_CHROME9_VERTEX_FILE_TEMP;
         src->relative = 0;
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
   src.relative = v->relative;
   /* Literal-only operands name no register. */
   if (!tcl_source_reads(&src)) {
      src.file = DRM_CHROME9_VERTEX_FILE_TEMP;
      src.index = 0;
      src.relative = 0;
   }
   return src;
}

static const struct tcl_value *
tcl_value(struct tcl_builder *b, const nir_def *def)
{
   const struct tcl_value *v = &b->values[def->index];

   /* A temp written inside a flow region is undefined after it. */
   if (!v->valid || def->bit_size != 32 ||
       (v->file == DRM_CHROME9_VERTEX_FILE_TEMP && !tcl_scope_active(b, v->scope))) {
      if (!b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false)) {
         fprintf(stderr, "chrome9: TCL operand %s from: ",
                 !v->valid ? "undefined" :
                 def->bit_size != 32 ? "not 32-bit" : "outside its flow region");
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
   v->scope = b->scope[b->depth];
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
                       first->relative != v->relative ||
                       first->neg != v->neg))
            return false;
         first = v;
      }
      alias.sel[c] = sel;
   }
   alias.file = first ? first->file : DRM_CHROME9_VERTEX_FILE_TEMP;
   alias.index = first ? first->index : 0;
   alias.relative = first ? first->relative : 0;
   alias.scope = first ? first->scope : 0;
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

/* cond ? a : b as MAD(a, mask, MAD(-c, mask, c)) with a 0/1 mask; each
 * term is exact, where b + mask * (a - b) cancels b beside a large a. */
static unsigned
tcl_select(struct tcl_builder *b, unsigned mask_temp, unsigned lanes,
           struct drm_chrome9_vertex_source a,
           struct drm_chrome9_vertex_source c)
{
   const unsigned rest = tcl_new_temp(b), out = tcl_new_temp(b);
   struct drm_chrome9_vertex_source src[3] = {
      c, tcl_temp_source(mask_temp, lanes, false), c,
   };

   src[0].modifier ^= DRM_CHROME9_FRAGMENT_MOD_NEG;
   tcl_op(b, DRM_CHROME9_VERTEX_OP_MAD, BITFIELD_MASK(lanes), rest, src, 3);
   src[0] = a;
   src[2] = tcl_temp_source(rest, lanes, false);
   return tcl_op(b, DRM_CHROME9_VERTEX_OP_MAD, BITFIELD_MASK(lanes), out, src,
                 3);
}

/* trunc(x) = sign(x) * floor(|x|) in lanes [0, lanes) of a new temp. */
static unsigned
tcl_trunc(struct tcl_builder *b, unsigned lanes,
          struct drm_chrome9_vertex_source src)
{
   const uint8_t mask = BITFIELD_MASK(lanes);
   const unsigned mag = tcl_new_temp(b), whole = tcl_new_temp(b);
   const unsigned sign = tcl_new_temp(b), out = tcl_new_temp(b);
   struct drm_chrome9_vertex_source m[2];

   tcl_op(b, DRM_CHROME9_VERTEX_OP_ABS, mask, mag, &src, 1);
   m[0] = tcl_temp_source(mag, lanes, false);
   tcl_op(b, DRM_CHROME9_VERTEX_OP_FLR, mask, whole, m, 1);
   tcl_scalar(b, DRM_CHROME9_VERTEX_OP_SGN, sign, lanes, src);
   m[0] = tcl_temp_source(whole, lanes, false);
   m[1] = tcl_temp_source(sign, lanes, false);
   return tcl_op(b, DRM_CHROME9_VERTEX_OP_MUL, mask, out, m, 2);
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
   case nir_op_ftrunc:
      tcl_set_temp(b, &alu->def, tcl_trunc(b, n, src[0]));
      return;
   case nir_op_frcp:
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_RCP, temp, n, src[0]);
      break;
   case nir_op_fsign:
      tcl_scalar(b, DRM_CHROME9_VERTEX_OP_SGN, temp, n, src[0]);
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
         tcl_scalar(b, DRM_CHROME9_VERTEX_OP_SGN, sign, n, src[0]);
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

/* One scalar CLIP_DISTANCE write per enabled plane; the kernel packs the
 * planes into spare coordinate lanes. */
static void
tcl_store_clip(struct tcl_builder *b, unsigned base, unsigned component,
               unsigned mask, const struct tcl_value *v)
{
   u_foreach_bit(lane, mask) {
      const unsigned plane = base + lane;
      struct drm_chrome9_vertex_instruction mov = {
         .op = DRM_CHROME9_VERTEX_OP_MOV,
         .dst = { DRM_CHROME9_VERTEX_DEST_CLIP_DISTANCE, plane, 1, 0 },
      };
      uint8_t swizzle[4];

      if (!(b->clip_enable & BITFIELD_BIT(plane)))
         continue;
      memset(swizzle, lane - component, sizeof(swizzle));
      mov.src[0] = tcl_source(v, swizzle, 4, false);
      b->p->clip_written |= BITFIELD_BIT(plane);
      tcl_emit(b, &mov, 1);
   }
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
   unsigned index = 0;

   /* The kernel takes outputs only outside flow regions. */
   if (!nir_src_is_const(store->src[1]) || nir_src_as_uint(store->src[1]) ||
       mask > 0xf || b->failed || b->depth) {
      b->failed = true;
      return;
   }
   if (io.location == VARYING_SLOT_POS) {
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_POSITION;
   } else if (io.location == VARYING_SLOT_COL0) {
      /* The back-face pass takes its primary colour from BFC0. */
      if (b->colour == CHROME9_TCL_COLOUR_BACK && b->writes_back)
         return;
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_PRIMARY;
   } else if (io.location == VARYING_SLOT_BFC0) {
      /* Without light_twoside only front colors are visible. */
      if (b->colour == CHROME9_TCL_COLOUR_FRONT)
         return;
      mov.dst.file = b->colour == CHROME9_TCL_COLOUR_BACK ?
                     DRM_CHROME9_VERTEX_DEST_PRIMARY :
                     DRM_CHROME9_VERTEX_DEST_BACK_PRIMARY;
      b->p->back_primary = true;
   } else if (io.location == VARYING_SLOT_CLIP_DIST0 ||
              io.location == VARYING_SLOT_CLIP_DIST1) {
      tcl_store_clip(b, io.location == VARYING_SLOT_CLIP_DIST1 ? 4 : 0,
                     component, mask, v);
      return;
   } else if (io.location == VARYING_SLOT_PSIZ) {
      /* Used by the kernel only for RASTER_POINT_SIZE_VERTEX draws. */
      if (!b->point_size)
         return;
      if (mask != 1) {
         b->failed = true;
         return;
      }
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_POINT_SIZE;
      b->p->point_size = true;
   } else if (io.location == VARYING_SLOT_BFC1 ||
              io.location == VARYING_SLOT_CLIP_VERTEX) {
      /* COL1 is not consumed either; st lowers CLIP_VERTEX to distances
       * whenever a plane is enabled. */
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
   if (mov.dst.file == DRM_CHROME9_VERTEX_DEST_VARYING)
      b->varying_written[index] |= mask;
   else if (mov.dst.file == DRM_CHROME9_VERTEX_DEST_PRIMARY)
      b->primary_written |= mask;
   tcl_emit(b, &mov, 1);
}

/* The kernel requires whole colour and varying registers; lanes NIR left
 * undefined (e.g. after dead store elimination) and varyings the VS never
 * writes are zero.  The kernel drops those the fragment does not read. */
static void
tcl_fill_outputs(struct tcl_builder *b)
{
   struct drm_chrome9_vertex_instruction mov = {
      .op = DRM_CHROME9_VERTEX_OP_MOV,
      .src = { { DRM_CHROME9_VERTEX_FILE_TEMP, 0,
                 { TCL_SEL_ZERO, TCL_SEL_ZERO, TCL_SEL_ZERO,
                   TCL_SEL_ZERO } } },
   };

   for (unsigned v = 0; v < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; v++) {
      if (b->varying_written[v] == 0xf)
         continue;
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_VARYING;
      mov.dst.index = v;
      mov.dst.writemask = ~b->varying_written[v] & 0xf;
      u_foreach_bit(c, mov.dst.writemask)
         b->p->origin[v][c] = CHROME9_TCL_ORIGIN_ZERO;
      tcl_emit(b, &mov, 1);
   }
   if (b->primary_written != 0xf) {
      mov.dst.file = DRM_CHROME9_VERTEX_DEST_PRIMARY;
      mov.dst.index = 0;
      mov.dst.writemask = ~b->primary_written & 0xf;
      tcl_emit(b, &mov, 1);
   }
}

/* Whether a use of def is emitted inside a flow region (phi moves are). */
static bool
tcl_used_in_flow(const struct tcl_builder *b, nir_def *def)
{
   nir_foreach_use_including_if(src, def) {
      const nir_cf_node *parent;

      if (nir_src_is_if(src)) {
         parent = nir_src_use_if(src)->cf_node.parent;
      } else {
         nir_instr *instr = nir_src_use_instr(src);

         if (instr->type == nir_instr_type_phi)
            return true;
         parent = instr->block->cf_node.parent;
      }
      if (parent != &b->impl->cf_node)
         return true;
   }
   return false;
}

/* An indirect CB0 read the lowering cannot express. */
static void
tcl_indirect_fail(struct tcl_builder *b, const char *reason)
{
   if (!b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL indirect constant: %s\n", reason);
   b->p->indirect = true;
   b->failed = true;
}

static bool
tcl_indirect_load(const nir_instr *instr)
{
   const nir_intrinsic_instr *intr;

   if (instr->type != nir_instr_type_intrinsic)
      return false;
   intr = nir_instr_as_intrinsic(instr);
   return (intr->intrinsic == nir_intrinsic_load_ubo ||
           intr->intrinsic == nir_intrinsic_load_ubo_vec4) &&
          !nir_src_is_const(intr->src[1]);
}

/* nir_scalar_chase_movs that records the moves it passes. */
static nir_scalar
tcl_chase(nir_scalar s, nir_instr **chain, unsigned *count)
{
   while (nir_scalar_is_alu(s) && *count < TCL_ADDRESS_CHAIN &&
          (nir_scalar_alu_op(s) == nir_op_mov ||
           nir_op_is_vec(nir_scalar_alu_op(s)))) {
      chain[(*count)++] = nir_def_instr(s.def);
      s = nir_scalar_chase_alu_src(s, nir_scalar_alu_op(s) == nir_op_mov ?
                                      0 : s.comp);
   }
   return s;
}

/*
 * Index scalar of an indirect load_ubo_vec4, whose address is f2i32 of
 * float-encoded integer arithmetic (chrome9_nir_lower_integers).  Constant
 * terms fold into *base while the other term is integral, so truncation
 * and the ARL floor still agree; *trunc asks the ARL for truncation of a
 * possibly negative fraction.  chain collects the folded instructions.
 */
static bool
tcl_address_index(struct tcl_builder *b, const nir_intrinsic_instr *load,
                  nir_scalar *index, int *base, bool *trunc,
                  nir_instr **chain, unsigned *count)
{
   nir_scalar s;
   fp_class_mask fp;

   *count = 0;
   *base = nir_intrinsic_base(load);
   s = tcl_chase(nir_get_scalar(load->src[1].ssa, 0), chain, count);
   if (load->intrinsic != nir_intrinsic_load_ubo_vec4 ||
       !nir_scalar_is_alu(s) || nir_scalar_alu_op(s) != nir_op_f2i32 ||
       *count >= TCL_ADDRESS_CHAIN)
      return false;
   chain[(*count)++] = nir_def_instr(s.def);
   s = tcl_chase(nir_scalar_chase_alu_src(s, 0), chain, count);
   while (*count + 3 <= TCL_ADDRESS_CHAIN && nir_scalar_is_alu(s) &&
          nir_scalar_alu_op(s) == nir_op_fadd) {
      const unsigned t = nir_scalar_is_const(
         nir_scalar_chase_movs(nir_scalar_chase_alu_src(s, 0)));
      nir_scalar term = nir_scalar_chase_alu_src(s, t);
      nir_scalar k = nir_scalar_chase_movs(nir_scalar_chase_alu_src(s, !t));
      float f;

      if (!nir_scalar_is_const(k) ||
          (nir_analyze_fp_class(&b->fp, nir_scalar_chase_movs(term).def) &
           FP_CLASS_NON_INTEGRAL))
         break;
      f = nir_scalar_as_float(k);
      if (f != floorf(f) || fabsf(f) > DRM_CHROME9_VERTEX_MAX_CONSTANTS)
         break;
      chain[(*count)++] = nir_def_instr(s.def);
      chain[(*count)++] = nir_def_instr(k.def);
      *base += (int)f;
      s = tcl_chase(term, chain, count);
   }
   if (nir_scalar_is_alu(s) && nir_scalar_alu_op(s) == nir_op_ftrunc &&
       *count < TCL_ADDRESS_CHAIN) {
      chain[(*count)++] = nir_def_instr(s.def);
      s = nir_scalar_chase_movs(nir_scalar_chase_alu_src(s, 0));
   }
   fp = nir_analyze_fp_class(&b->fp, s.def);
   *trunc = (fp & FP_CLASS_NON_INTEGRAL) && (fp & FP_CLASS_ANY_NEG);
   *index = s;
   return true;
}

/* Address arithmetic read only by indirect loads is not emitted. */
static void
tcl_skip_addresses(struct tcl_builder *b)
{
   bool progress;

   do {
      progress = false;
      nir_foreach_block(block, b->impl) {
         nir_foreach_instr(instr, block) {
            nir_instr *chain[TCL_ADDRESS_CHAIN];
            nir_scalar index;
            unsigned count;
            bool trunc;
            int base;

            if (!tcl_indirect_load(instr) ||
                !tcl_address_index(b, nir_instr_as_intrinsic(instr), &index,
                                   &base, &trunc, chain, &count))
               continue;
            for (unsigned i = 0; i < count; i++) {
               bool only = chain[i]->pass_flags != TCL_SKIP;

               nir_foreach_use_including_if(src, nir_instr_def(chain[i])) {
                  const nir_instr *user;

                  if (!only || nir_src_is_if(src)) {
                     only = false;
                     break;
                  }
                  user = nir_src_use_instr(src);
                  only = user->pass_flags == TCL_SKIP ||
                         (tcl_indirect_load(user) &&
                          src == &nir_instr_as_intrinsic(user)->src[1]);
               }
               if (only) {
                  chain[i]->pass_flags = TCL_SKIP;
                  progress = true;
               }
            }
         }
      }
   } while (progress);
}

/*
 * The a0 lane holding index for reads at base, written by an ARL unless an
 * active region already did.  A lane only ever holds its one index, and
 * its bases stay within TCL_RELATIVE_SPREAD so the kernel clamp, taken
 * over all of them, cuts no valid read once the rows are padded.
 */
static int
tcl_address_lane(struct tcl_builder *b, nir_scalar index, unsigned base,
                 bool trunc)
{
   struct drm_chrome9_vertex_instruction arl = {
      .op = DRM_CHROME9_VERTEX_OP_ARL,
      .dst = { DRM_CHROME9_VERTEX_DEST_ADDRESS, 0, 0, 0 },
   };
   struct tcl_address *a = NULL;
   const struct tcl_value *v;
   uint8_t swizzle[4];
   unsigned lane;

   for (lane = 0; lane < b->address_count; lane++) {
      a = &b->address[lane];
      if (a->index.def == index.def && a->index.comp == index.comp &&
          MAX2(a->base_max, base) - MIN2(a->base_min, base) <=
          TCL_RELATIVE_SPREAD)
         break;
   }
   if (lane == b->address_count) {
      if (lane == ARRAY_SIZE(b->address)) {
         tcl_indirect_fail(b, "more than four a0 lanes");
         return -1;
      }
      a = &b->address[b->address_count++];
      memset(a, 0, sizeof(*a));
      a->index = index;
      a->base_min = a->base_max = base;
   }
   a->base_min = MIN2(a->base_min, base);
   a->base_max = MAX2(a->base_max, base);
   if (a->written && tcl_scope_active(b, a->scope))
      return lane;
   v = tcl_value(b, index.def);
   if (b->failed)
      return -1;
   memset(swizzle, index.comp, sizeof(swizzle));
   arl.src[0] = tcl_source(v, swizzle, 4, false);
   /* The ARL floors; a possibly negative fraction needs truncation. */
   if (trunc)
      arl.src[0] = tcl_temp_source(tcl_trunc(b, 1, arl.src[0]), 1, false);
   arl.dst.writemask = 1u << lane;
   tcl_emit(b, &arl, 1);
   a->written = true;
   a->scope = b->scope[b->depth];
   return lane;
}

/* c[base + a0.lane] for load_ubo_vec4 with a computed element index. */
static void
tcl_relative_load(struct tcl_builder *b, nir_intrinsic_instr *load)
{
   struct tcl_value *out = &b->values[load->def.index];
   const unsigned component = nir_intrinsic_component(load);
   nir_instr *chain[TCL_ADDRESS_CHAIN];
   nir_scalar index;
   unsigned count;
   bool trunc;
   int base, lane;

   if (!b->relative) {
      tcl_indirect_fail(b, "no relative addressing");
      return;
   }
   if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
       load->def.bit_size != 32 ||
       component + load->def.num_components > 4 ||
       !tcl_address_index(b, load, &index, &base, &trunc, chain, &count) ||
       base < 0 || base >= (int)b->p->uniform_rows) {
      tcl_indirect_fail(b, "address form");
      return;
   }
   lane = tcl_address_lane(b, index, base, trunc);
   if (lane < 0)
      return;
   memset(out, 0, sizeof(*out));
   out->valid = true;
   out->file = DRM_CHROME9_VERTEX_FILE_CONSTANT;
   out->index = base;
   out->relative = DRM_CHROME9_VERTEX_RELATIVE_A0_X + lane;
   for (unsigned c = 0; c < 4; c++)
      out->sel[c] = component + (c < load->def.num_components ? c : 0);
}

/* Zero pad rows for the widest lane, reserved kernel rows, and the
 * constant sources moved behind the front pad. */
static void
tcl_relative_finish(struct tcl_builder *b)
{
   struct chrome9_tcl_program *p = b->p;
   unsigned pad = 0;

   for (unsigned lane = 0; lane < b->address_count; lane++)
      pad = MAX2(pad, b->address[lane].base_max - b->address[lane].base_min);
   /* The kernel keeps its ARL bound rows after the draw's constants. */
   if (2 * pad + p->uniform_rows + p->immediate_count + 2 >
       DRM_CHROME9_VERTEX_MAX_CONSTANTS) {
      tcl_indirect_fail(b, "constant rows");
      return;
   }
   p->constant_pad = pad;
   for (unsigned i = 0; pad && i < p->instruction_count; i++)
      for (unsigned k = 0; k < 3; k++)
         if (p->instructions[i].src[k].file == DRM_CHROME9_VERTEX_FILE_CONSTANT)
            p->instructions[i].src[k].index += pad;
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
      /* The kernel reads inputs only outside flow regions. */
      if (b->flow && tcl_used_in_flow(b, &intr->def)) {
         if (b->depth) {
            b->failed = true;
            return;
         }
         out->index = tcl_register_temp(b, DRM_CHROME9_VERTEX_FILE_INPUT,
                                        out->index);
         out->file = DRM_CHROME9_VERTEX_FILE_TEMP;
      }
      return;
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ubo_vec4:
      if (tcl_indirect_load(&intr->instr)) {
         tcl_relative_load(b, intr);
         return;
      }
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


/*
 * A 2D lookup at LOD 0 whose coordinate is a vertex attribute becomes an
 * extra input the CPU samples per vertex before the draw.
 */
static void
tcl_tex(struct tcl_builder *b, nir_tex_instr *tex)
{
   struct chrome9_tcl_program *p = b->p;
   struct tcl_value *out = &b->values[tex->def.index];
   const int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   const int lod = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   nir_scalar st[2];
   nir_intrinsic_instr *input = NULL;
   struct chrome9_tcl_prefetch f = { 0 };
   unsigned k;

   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txl) ||
       tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_array ||
       tex->is_shadow || tex->def.bit_size != 32 || coord < 0 ||
       tex->num_srcs != 1u + (lod >= 0) ||
       (lod >= 0 && (!nir_src_is_const(tex->src[lod].src) ||
                     nir_src_as_float(tex->src[lod].src) != 0.0)) ||
       tex->src[coord].src.ssa->num_components != 2 ||
       tex->texture_index >= CHROME9_VS_MAX_SAMPLERS ||
       tex->sampler_index >= CHROME9_VS_MAX_SAMPLERS ||
       (b->flow && tcl_used_in_flow(b, &tex->def))) {
      b->failed = true;
      return;
   }
   for (unsigned c = 0; c < 2; c++) {
      nir_intrinsic_instr *load;

      st[c] = nir_scalar_resolved(tex->src[coord].src.ssa, c);
      if (!nir_scalar_is_intrinsic(st[c])) {
         b->failed = true;
         return;
      }
      load = nir_def_as_intrinsic(st[c].def);
      if (load->intrinsic != nir_intrinsic_load_input ||
          !nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          (input && nir_intrinsic_base(load) != nir_intrinsic_base(input))) {
         b->failed = true;
         return;
      }
      input = load;
   }
   f.unit = tex->texture_index;
   f.sampler = tex->sampler_index;
   f.input = nir_intrinsic_base(input);
   f.s = nir_intrinsic_component(nir_def_as_intrinsic(st[0].def)) + st[0].comp;
   f.t = nir_intrinsic_component(nir_def_as_intrinsic(st[1].def)) + st[1].comp;
   for (k = 0; k < p->prefetch_count; k++)
      if (!memcmp(&p->prefetch[k], &f, sizeof(f)))
         break;
   if (k == p->prefetch_count) {
      if (p->attrib_count + k >= DRM_CHROME9_VERTEX_MAX_INPUTS) {
         b->failed = true;
         return;
      }
      p->prefetch[p->prefetch_count++] = f;
   }
   memset(out, 0, sizeof(*out));
   out->valid = true;
   out->file = DRM_CHROME9_VERTEX_FILE_INPUT;
   out->index = p->attrib_count + k;
   for (unsigned c = 0; c < 4; c++)
      out->sel[c] = c;
   p->input_count = MAX2(p->input_count, out->index + 1u);
}

static void
tcl_flow_fail(struct tcl_builder *b, const char *reason)
{
   if (!b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL flow: %s\n", reason);
   b->failed = true;
}

struct tcl_expr_key {
   nir_def *def;
   unsigned comp;
};

static bool
tcl_expr_alu(nir_op op)
{
   switch (op) {
   case nir_op_mov: case nir_op_fneg: case nir_op_fabs: case nir_op_fadd:
   case nir_op_fsub: case nir_op_fmul: case nir_op_ffma: case nir_op_ffma_weak:
   case nir_op_fmin: case nir_op_fmax: case nir_op_slt: case nir_op_sge:
   case nir_op_seq: case nir_op_sne: case nir_op_ffloor: case nir_op_fceil:
   case nir_op_ftrunc: case nir_op_ffract: case nir_op_fsat: case nir_op_fsign:
   case nir_op_frcp: case nir_op_frsq: case nir_op_fsqrt: case nir_op_fcsel:
   case nir_op_fcsel_gt: case nir_op_fcsel_ge:
      return true;
   default:
      return false;
   }
}

/* Node of a scalar computed only from CB0 dwords and literals, or -1. */
static int
tcl_expr(struct tcl_builder *b, nir_scalar s)
{
   struct chrome9_tcl_program *p = b->p;
   const struct tcl_expr_key *keys = b->expr_keys.data;
   struct chrome9_tcl_expr node = { 0 };
   struct tcl_expr_key key;
   unsigned row, lane;

   s = nir_scalar_chase_movs(s);
   if (s.def->bit_size != 32)
      return -1;
   for (unsigned i = 0; i < p->expr_count; i++)
      if (keys[i].def == s.def && keys[i].comp == s.comp)
         return i;
   if (nir_scalar_is_const(s)) {
      node.op = TCL_EXPR_LITERAL;
      node.value = nir_scalar_as_uint(s);
   } else if (nir_scalar_is_intrinsic(s) &&
              tcl_ubo_row(nir_def_as_intrinsic(s.def), &row, &lane)) {
      node.op = TCL_EXPR_DWORD;
      node.value = row * 4 + lane + s.comp;
   } else if (nir_scalar_is_alu(s) && tcl_expr_alu(nir_scalar_alu_op(s))) {
      const nir_op op = nir_scalar_alu_op(s);

      node.op = op;
      for (unsigned k = 0; k < nir_op_infos[op].num_inputs; k++) {
         const int src = tcl_expr(b, nir_scalar_chase_alu_src(s, k));

         if (src < 0)
            return -1;
         node.src[k] = src;
      }
   } else {
      return -1;
   }
   /* bool_expr and rep bounds hold node indices in a byte. */
   if (p->expr_count > UINT8_MAX)
      return -1;
   if (!(p->expr_count & (p->expr_count - 1))) {
      struct chrome9_tcl_expr *grown =
         REALLOC(p->expr, p->expr_count * sizeof(*grown),
                 MAX2(2 * p->expr_count, 8) * sizeof(*grown));

      if (!grown)
         return -1;
      p->expr = grown;
   }
   key.def = s.def;
   key.comp = s.comp;
   util_dynarray_append(&b->expr_keys, key);
   p->expr[p->expr_count] = node;
   return p->expr_count++;
}

static bool
tcl_list_empty(struct exec_list *list)
{
   nir_cf_node *node = exec_node_data(nir_cf_node, exec_list_get_head(list),
                                      node);

   return exec_list_is_singular(list) && node->type == nir_cf_node_block &&
          exec_list_is_empty(&nir_cf_node_as_block(node)->instr_list);
}

static bool
tcl_break_arm(struct exec_list *list)
{
   nir_cf_node *node = exec_node_data(nir_cf_node, exec_list_get_head(list),
                                      node);
   nir_block *block;

   if (!exec_list_is_singular(list) || node->type != nir_cf_node_block)
      return false;
   block = nir_cf_node_as_block(node);
   return exec_list_is_singular(&block->instr_list) &&
          nir_block_ends_in_break(block);
}

/* No jump but the terminator's break leaves or restarts this loop. */
static bool
tcl_loop_jumps_ok(struct exec_list *list, const nir_if *terminator)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         nir_foreach_instr(instr, nir_cf_node_as_block(node))
            if (instr->type == nir_instr_type_jump)
               return false;
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);

         if (nif != terminator &&
             (!tcl_loop_jumps_ok(&nif->then_list, terminator) ||
              !tcl_loop_jumps_ok(&nif->else_list, terminator)))
            return false;
         break;
      }
      default:
         /* Nested loops own their breaks. */
         break;
      }
   }
   return true;
}

/* Every use of def is one of the given instructions or the if. */
static bool
tcl_only_used_by(nir_def *def, const nir_instr *a, const nir_instr *c,
                 const nir_if *nif)
{
   nir_foreach_use_including_if(src, def) {
      if (nir_src_is_if(src) ? nir_src_use_if(src) != nif :
          nir_src_use_instr(src) != a && nir_src_use_instr(src) != c)
         return false;
   }
   return true;
}

/* next = fadd(counter, constant step) with counter a header phi whose back
 * edge value is next. */
static bool
tcl_counter_step(nir_scalar next, nir_block *header, nir_block *latch,
                 nir_phi_instr **counter, float *step)
{
   if (!nir_scalar_is_alu(next) || nir_scalar_alu_op(next) != nir_op_fadd ||
       next.def->num_components != 1)
      return false;
   for (unsigned k = 0; k < 2; k++) {
      nir_scalar own = nir_scalar_chase_movs(nir_scalar_chase_alu_src(next, k));
      nir_scalar add = nir_scalar_chase_movs(nir_scalar_chase_alu_src(next, !k));
      nir_instr *instr = nir_def_instr(own.def);

      if (instr->type != nir_instr_type_phi || instr->block != header ||
          !nir_scalar_is_const(add) ||
          nir_phi_get_src_from_block(nir_instr_as_phi(instr),
                                     latch)->src.ssa != next.def)
         continue;
      *counter = nir_instr_as_phi(instr);
      *step = nir_scalar_as_float(add);
      return true;
   }
   return false;
}

/*
 * A REP-countable loop: the header block, then "if (c) break;" (or the
 * mirrored else-break), no other jump, one latch.  c compares a counter
 * (header phi, constant init and nonzero step) with a CB0 or literal
 * expression, either directly or rotated by NIR into a condition phi fed
 * by the compare of the next counter value.  The counter code is skipped
 * when nothing else reads it.
 */
static bool
tcl_match_rep(struct tcl_builder *b, nir_loop *loop,
              struct chrome9_tcl_rep *rep, nir_if **terminator)
{
   nir_cf_node *first = exec_node_data(nir_cf_node,
                                       exec_list_get_head(&loop->body), node);
   nir_block *header, *latch, *preheader;
   nir_phi_instr *counter = NULL, *cond_phi = NULL;
   nir_scalar cond, init, next = { 0 };
   nir_cf_node *second;
   nir_if *nif;
   int bound = -1;

   if (nir_loop_has_continue_construct(loop) ||
       first->type != nir_cf_node_block)
      return false;
   header = nir_cf_node_as_block(first);
   second = nir_cf_node_next(first);
   latch = nir_loop_last_block(loop);
   preheader = nir_cf_node_as_block(nir_cf_node_prev(&loop->cf_node));
   if (!second || second->type != nir_cf_node_if ||
       nir_block_num_preds(header) != 2)
      return false;
   nif = nir_cf_node_as_if(second);
   if (tcl_break_arm(&nif->then_list) && tcl_list_empty(&nif->else_list))
      rep->break_on_true = true;
   else if (tcl_break_arm(&nif->else_list) && tcl_list_empty(&nif->then_list))
      rep->break_on_true = false;
   else
      return false;
   if (!tcl_loop_jumps_ok(&loop->body, nif))
      return false;
   cond = nir_scalar_chase_movs(nir_get_scalar(nif->condition.ssa, 0));
   /* Rotated: the break flag of iteration k + 1 is computed in k. */
   if (nir_def_instr(cond.def)->type == nir_instr_type_phi &&
       nir_def_instr(cond.def)->block == header &&
       cond.def->num_components == 1) {
      cond_phi = nir_def_as_phi(cond.def);
      cond = nir_scalar_chase_movs(nir_get_scalar(
         nir_phi_get_src_from_block(cond_phi, latch)->src.ssa, 0));
   }
   if (!nir_scalar_is_alu(cond))
      return false;
   rep->compare = nir_scalar_alu_op(cond);
   if (rep->compare != nir_op_slt && rep->compare != nir_op_sge &&
       rep->compare != nir_op_seq && rep->compare != nir_op_sne)
      return false;
   for (unsigned k = 0; k < 2; k++) {
      nir_scalar side = nir_scalar_chase_movs(nir_scalar_chase_alu_src(cond, k));
      nir_instr *instr = nir_def_instr(side.def);
      nir_scalar candidate = side;

      if (!cond_phi && instr->type == nir_instr_type_phi &&
          instr->block == header)
         candidate = nir_scalar_chase_movs(nir_get_scalar(
            nir_phi_get_src_from_block(nir_instr_as_phi(instr),
                                       latch)->src.ssa, 0));
      if (!counter &&
          (cond_phi || instr->type == nir_instr_type_phi) &&
          tcl_counter_step(candidate, header, latch, &counter, &rep->step) &&
          (cond_phi || &counter->instr == instr)) {
         next = candidate;
         rep->counter_first = !k;
      } else {
         bound = tcl_expr(b, side);
      }
   }
   if (!counter || bound < 0)
      return false;
   rep->bound = bound;
   init = nir_scalar_chase_movs(nir_get_scalar(
      nir_phi_get_src_from_block(counter, preheader)->src.ssa, 0));
   if (!nir_scalar_is_const(init))
      return false;
   rep->init = nir_scalar_as_float(init);
   if (rep->step == 0.0f || !isfinite(rep->step) || !isfinite(rep->init))
      return false;
   if (cond_phi ?
       nif->condition.ssa == &cond_phi->def &&
       nir_phi_get_src_from_block(cond_phi, latch)->src.ssa == cond.def &&
       tcl_only_used_by(&counter->def, nir_def_instr(next.def), NULL, NULL) &&
       tcl_only_used_by(next.def, &counter->instr, nir_def_instr(cond.def),
                        NULL) &&
       tcl_only_used_by(cond.def, &cond_phi->instr, NULL, NULL) &&
       tcl_only_used_by(&cond_phi->def, NULL, NULL, nif) :
       nif->condition.ssa == cond.def &&
       tcl_only_used_by(&counter->def, nir_def_instr(cond.def),
                        nir_def_instr(next.def), NULL) &&
       tcl_only_used_by(next.def, &counter->instr, NULL, NULL) &&
       tcl_only_used_by(cond.def, NULL, NULL, nif)) {
      counter->instr.pass_flags = TCL_SKIP;
      nir_def_instr(next.def)->pass_flags = TCL_SKIP;
      nir_def_instr(cond.def)->pass_flags = TCL_SKIP;
      if (cond_phi)
         cond_phi->instr.pass_flags = TCL_SKIP;
   }
   *terminator = nif;
   return true;
}

/* Match every REP and find the b# ifs before emission, so counter and
 * uniform condition code is never emitted; tcl_skip_dead removes it. */
static void
tcl_mark_reps(struct tcl_builder *b, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);

         if (tcl_expr(b, nir_get_scalar(nif->condition.ssa, 0)) >= 0)
            util_dynarray_append(&b->uniform_ifs, nif);
         tcl_mark_reps(b, &nif->then_list);
         tcl_mark_reps(b, &nif->else_list);
      } else if (node->type == nir_cf_node_loop) {
         struct chrome9_tcl_rep rep;
         nir_if *terminator;

         tcl_match_rep(b, nir_cf_node_as_loop(node), &rep, &terminator);
         tcl_mark_reps(b, &nir_cf_node_as_loop(node)->body);
      }
   }
}

static bool
tcl_uniform_if(const struct tcl_builder *b, const nir_if *nif)
{
   util_dynarray_foreach(&b->uniform_ifs, nir_if *, entry)
      if (*entry == nif)
         return true;
   return false;
}

static void
tcl_skip_dead(struct tcl_builder *b, nir_function_impl *impl)
{
   bool progress;

   do {
      progress = false;
      nir_foreach_block_reverse(block, impl) {
         nir_foreach_instr_reverse(instr, block) {
            bool dead = instr->type == nir_instr_type_alu &&
                        instr->pass_flags != TCL_SKIP;

            if (!dead)
               continue;
            nir_foreach_use_including_if(src, &nir_instr_as_alu(instr)->def)
               dead &= nir_src_is_if(src) ?
                       tcl_uniform_if(b, nir_src_use_if(src)) :
                       nir_src_use_instr(src)->pass_flags == TCL_SKIP;
            if (dead) {
               instr->pass_flags = TCL_SKIP;
               progress = true;
            }
         }
      }
   } while (progress);
}

static void tcl_emit_list(struct tcl_builder *b, struct exec_list *list);
static void tcl_emit_loop(struct tcl_builder *b, nir_loop *loop);

static void
tcl_flow_op(struct tcl_builder *b, uint8_t op,
            const struct drm_chrome9_vertex_source *src)
{
   struct drm_chrome9_vertex_instruction insn = { .op = op };

   if (src)
      insn.src[0] = *src;
   tcl_append(b, &insn);
}

static void
tcl_flow_source(struct drm_chrome9_vertex_source *src, uint8_t file,
                uint8_t index)
{
   memset(src, 0, sizeof(*src));
   src->file = file;
   src->index = index;
   for (unsigned i = 0; i < 4; i++)
      src->swizzle[i] = i;
}

/* Region caches: register copies made inside do not exist outside. */
struct tcl_region {
   int16_t constant_temp[DRM_CHROME9_VERTEX_MAX_CONSTANTS];
   int16_t input_temp[DRM_CHROME9_VERTEX_MAX_INPUTS];
};

static void
tcl_enter(struct tcl_builder *b, struct tcl_region *r)
{
   memcpy(r->constant_temp, b->constant_temp, sizeof(r->constant_temp));
   memcpy(r->input_temp, b->input_temp, sizeof(r->input_temp));
   b->scope[++b->depth] = ++b->scope_serial;
}

static void
tcl_leave(struct tcl_builder *b, const struct tcl_region *r)
{
   memcpy(b->constant_temp, r->constant_temp, sizeof(r->constant_temp));
   memcpy(b->input_temp, r->input_temp, sizeof(r->input_temp));
   b->depth--;
}

static void
tcl_move(struct tcl_builder *b, unsigned temp, const struct tcl_value *v,
         unsigned lanes)
{
   static const uint8_t identity[4] = { 0, 1, 2, 3 };
   struct drm_chrome9_vertex_source src = tcl_source(v, identity, lanes,
                                                     false);

   tcl_op(b, DRM_CHROME9_VERTEX_OP_MOV, BITFIELD_MASK(lanes), temp, &src, 1);
}

/* Phis after an if: each arm writes the phi's temp from its source. */
static void
tcl_phi_moves(struct tcl_builder *b, nir_block *after, nir_block *pred)
{
   nir_foreach_phi(phi, after) {
      nir_phi_src *src = nir_phi_get_src_from_block(phi, pred);
      const struct tcl_value *v;

      if (!src) {
         tcl_flow_fail(b, "phi without an arm source");
         return;
      }
      v = tcl_value(b, src->src.ssa);
      if (b->failed)
         return;
      tcl_move(b, b->values[phi->def.index].index, v, phi->def.num_components);
   }
}

static void
tcl_emit_if(struct tcl_builder *b, nir_if *nif)
{
   nir_block *after = nir_cf_node_as_block(nir_cf_node_next(&nif->cf_node));
   struct drm_chrome9_vertex_source cond;
   struct tcl_region region;
   bool phis = false;
   unsigned at;
   int expr;

   if (b->depth >= b->flow->max_depth) {
      tcl_flow_fail(b, "nesting depth");
      return;
   }
   expr = tcl_expr(b, nir_get_scalar(nif->condition.ssa, 0));
   if (expr >= 0) {
      if (b->p->bool_count >= DRM_CHROME9_VERTEX_MAX_BOOL_CONSTANTS) {
         tcl_flow_fail(b, "out of b# constants");
         return;
      }
      b->p->bool_expr[b->p->bool_count] = expr;
      tcl_flow_source(&cond, DRM_CHROME9_VERTEX_FILE_BOOL,
                      b->p->bool_count++);
   } else if (b->flow->dynamic) {
      const struct tcl_value *v = tcl_value(b, nif->condition.ssa);
      unsigned temp;

      if (b->failed)
         return;
      temp = v->index;
      if (v->file != DRM_CHROME9_VERTEX_FILE_TEMP || v->sel[0] > 3 || v->neg) {
         temp = tcl_new_temp(b);
         tcl_move(b, temp, v, 1);
      }
      tcl_flow_source(&cond, DRM_CHROME9_VERTEX_FILE_TEMP, temp);
      memset(cond.swizzle, v->file == DRM_CHROME9_VERTEX_FILE_TEMP &&
             v->sel[0] <= 3 && !v->neg ? v->sel[0] : 0, sizeof(cond.swizzle));
   } else {
      tcl_flow_fail(b, "data-dependent branch");
      return;
   }
   nir_foreach_phi(phi, after) {
      tcl_set_temp(b, &phi->def, tcl_new_temp(b));
      phis = true;
   }
   at = b->p->instruction_count;
   tcl_flow_op(b, DRM_CHROME9_VERTEX_OP_IF, &cond);
   tcl_enter(b, &region);
   tcl_emit_list(b, &nif->then_list);
   tcl_phi_moves(b, after, nir_if_last_then_block(nif));
   tcl_leave(b, &region);
   if (phis || !tcl_list_empty(&nif->else_list)) {
      tcl_flow_op(b, DRM_CHROME9_VERTEX_OP_ELSE, NULL);
      tcl_enter(b, &region);
      tcl_emit_list(b, &nif->else_list);
      tcl_phi_moves(b, after, nir_if_last_else_block(nif));
      tcl_leave(b, &region);
   } else if (b->p->instruction_count == at + 1) {
      b->p->instruction_count--;
      return;
   }
   tcl_flow_op(b, DRM_CHROME9_VERTEX_OP_ENDIF, NULL);
}

static void
tcl_emit_instr(struct tcl_builder *b, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      tcl_alu(b, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_load_const:
      tcl_load_const(b, nir_instr_as_load_const(instr));
      break;
   case nir_instr_type_undef: {
      nir_undef_instr *undef = nir_instr_as_undef(instr);
      struct tcl_value *v = &b->values[undef->def.index];

      memset(v, 0, sizeof(*v));
      v->valid = true;
      memset(v->sel, TCL_SEL_ZERO, sizeof(v->sel));
      break;
   }
   case nir_instr_type_intrinsic:
      tcl_intrinsic(b, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_tex:
      tcl_tex(b, nir_instr_as_tex(instr));
      break;
   case nir_instr_type_phi:
      /* Set up by the enclosing if or loop. */
      if (!b->values[nir_instr_as_phi(instr)->def.index].valid)
         b->failed = true;
      break;
   default:
      b->failed = true;
      break;
   }
   if (b->failed && debug_get_bool_option("CHROME9_DEBUG_TCL", false)) {
      fprintf(stderr, "chrome9: TCL cannot express: ");
      nir_print_instr(instr, stderr);
      fprintf(stderr, "\n");
   }
}

static void
tcl_emit_node(struct tcl_builder *b, nir_cf_node *node)
{
   switch (node->type) {
   case nir_cf_node_block:
      nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
         if (b->failed)
            return;
         if (instr->pass_flags != TCL_SKIP)
            tcl_emit_instr(b, instr);
      }
      break;
   case nir_cf_node_if:
      tcl_emit_if(b, nir_cf_node_as_if(node));
      break;
   case nir_cf_node_loop:
      tcl_emit_loop(b, nir_cf_node_as_loop(node));
      break;
   default:
      b->failed = true;
      break;
   }
}

static void
tcl_emit_list(struct tcl_builder *b, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list)
      if (!b->failed)
         tcl_emit_node(b, node);
}

struct tcl_back_move {
   unsigned temp;
   unsigned lanes;
   struct tcl_value value;
};

/* Back edge as a parallel copy: a source reading another carried temp is
 * staged first, so no carried temp is read after it was rewritten. */
static void
tcl_back_moves(struct tcl_builder *b, nir_block *header, nir_block *latch,
               const BITSET_WORD *carried)
{
   struct util_dynarray moves;

   util_dynarray_init(&moves, NULL);
   nir_foreach_phi(phi, header) {
      struct tcl_back_move move;

      if (phi->instr.pass_flags == TCL_SKIP)
         continue;
      move.temp = b->values[phi->def.index].index;
      move.lanes = phi->def.num_components;
      move.value = *tcl_value(b, nir_phi_get_src_from_block(phi, latch)->src.ssa);
      if (b->failed)
         break;
      if (move.value.file == DRM_CHROME9_VERTEX_FILE_TEMP &&
          move.value.index != move.temp &&
          BITSET_TEST(carried, move.value.index)) {
         const unsigned staged = tcl_new_temp(b);

         tcl_move(b, staged, &move.value, move.lanes);
         move.value.index = staged;
         move.value.neg = false;
         for (unsigned i = 0; i < 4; i++)
            move.value.sel[i] = i < move.lanes ? i : 0;
         move.value.scope = b->scope[b->depth];
      }
      util_dynarray_append(&moves, move);
   }
   util_dynarray_foreach(&moves, struct tcl_back_move, move) {
      bool self = move->value.file == DRM_CHROME9_VERTEX_FILE_TEMP &&
                  move->value.index == move->temp && !move->value.neg;

      for (unsigned i = 0; i < move->lanes; i++)
         self &= move->value.sel[i] == i;
      if (!self && !b->failed)
         tcl_move(b, move->temp, &move->value, move->lanes);
   }
   util_dynarray_fini(&moves);
}

static void
tcl_emit_loop(struct tcl_builder *b, nir_loop *loop)
{
   nir_block *header = nir_loop_first_block(loop);
   nir_block *preheader =
      nir_cf_node_as_block(nir_cf_node_prev(&loop->cf_node));
   BITSET_DECLARE(carried, DRM_CHROME9_VERTEX_MAX_TEMPS) = { 0 };
   struct drm_chrome9_vertex_source count;
   struct chrome9_tcl_rep rep = { 0 };
   struct tcl_region region;
   nir_if *terminator;
   unsigned at;

   if (b->depth >= b->flow->max_depth) {
      tcl_flow_fail(b, "nesting depth");
      return;
   }
   if (!tcl_match_rep(b, loop, &rep, &terminator)) {
      tcl_flow_fail(b, "loop without a CB0 or literal trip count");
      return;
   }
   if (b->p->rep_count >= DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS) {
      tcl_flow_fail(b, "out of i# constants");
      return;
   }
   /* Loop-carried values live in one temp each, set before the loop. */
   nir_foreach_phi(phi, header) {
      const struct tcl_value *v;
      unsigned temp;

      if (phi->instr.pass_flags == TCL_SKIP)
         continue;
      v = tcl_value(b, nir_phi_get_src_from_block(phi, preheader)->src.ssa);
      temp = tcl_new_temp(b);
      if (b->failed)
         return;
      tcl_move(b, temp, v, phi->def.num_components);
      tcl_set_temp(b, &phi->def, temp);
      BITSET_SET(carried, temp);
   }
   b->p->reps[b->p->rep_count] = rep;
   tcl_flow_source(&count, DRM_CHROME9_VERTEX_FILE_INT, b->p->rep_count++);
   at = b->p->instruction_count;
   tcl_flow_op(b, DRM_CHROME9_VERTEX_OP_REP, &count);
   tcl_enter(b, &region);
   foreach_list_typed(nir_cf_node, node, node, &loop->body)
      if (node != &terminator->cf_node && !b->failed)
         tcl_emit_node(b, node);
   if (!b->failed)
      tcl_back_moves(b, header, nir_loop_last_block(loop), carried);
   tcl_leave(b, &region);
   if (b->p->instruction_count == at + 1) {
      b->p->instruction_count--;
      return;
   }
   tcl_flow_op(b, DRM_CHROME9_VERTEX_OP_ENDREP, NULL);
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
                    enum chrome9_tcl_colour colour, unsigned clip_enable,
                    unsigned point, unsigned key_count,
                    const uint16_t *key_dword, const uint32_t *key_value,
                    const struct chrome9_tcl_flow *flow,
                    struct chrome9_tcl_program *p)
{
   struct tcl_builder b = {
      .p = p, .colour = colour, .clip_enable = clip_enable,
      .point_size = point & CHROME9_TCL_POINT_SIZE,
      .writes_back = !!(source->info.outputs_written & VARYING_BIT_BFC0),
      .flow = flow && flow->max_depth ? flow : NULL,
      .relative = flow && flow->relative,
   };
   struct tcl_key key = { key_count, key_dword, key_value };
   nir_shader *nir = nir_shader_clone(NULL, source);
   nir_shader_compiler_options *options;
   bool indirect = false;
   nir_block *block;

   /* CHROME9_DEBUG_TCL_NIR=2 prints every VS entering the lowering. */
   if (debug_get_num_option("CHROME9_DEBUG_TCL_NIR", 0) > 1)
      nir_print_shader(nir, stderr);

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
   if (point & CHROME9_TCL_POINT_SIZE)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, tcl_round_point_size,
               nir_metadata_control_flow, NULL);
   if (point & CHROME9_TCL_POINT_CLIP) {
      unsigned stores = 0;

      NIR_PASS(_, nir, nir_shader_intrinsics_pass, tcl_clip_point,
               nir_metadata_control_flow, &stores);
      if (stores != 1)
         goto out;
   }
   /* Integer uniforms are float-encoded: lower integer operations before
    * the uniform values are inlined. */
   chrome9_nir_lower_integers(nir);
   /* Fold the current control uniforms so their branches disappear. */
   if (key_count)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, tcl_inline_key,
               nir_metadata_control_flow, &key);
   NIR_PASS(_, nir, nir_lower_alu_width, tcl_scalar_width, NULL);
   tcl_optimize(nir);
   /* Booleans become 0.0/1.0. */
   NIR_PASS(_, nir, nir_lower_bool_to_float, true);
   tcl_optimize(nir);
   if (nir_shader_alu_pass(nir, tcl_float_reduction, nir_metadata_all,
                           NULL)) {
      NIR_PASS(_, nir, nir_lower_alu_width, tcl_scalar_width, NULL);
      NIR_PASS(_, nir, nir_lower_bool_to_float, true);
      tcl_optimize(nir);
   }
   {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_fp_analysis_state fp = nir_create_fp_analysis_state(impl);

      nir_shader_alu_pass(nir, tcl_select_mark_infinite, nir_metadata_all,
                          &fp);
      nir_free_fp_analysis_state(&fp);
      NIR_PASS(_, nir, nir_shader_alu_pass, tcl_select_finite,
               nir_metadata_control_flow, NULL);
   }
   /* CHROME9_DEBUG_TCL_NIR=3 also prints the lowered program. */
   if (debug_get_num_option("CHROME9_DEBUG_TCL_NIR", 0) > 2)
      nir_print_shader(nir, stderr);
   b.impl = nir_shader_get_entrypoint(nir);
   if (!b.flow && !tcl_single_block(nir, &block)) {
      if (debug_get_bool_option("CHROME9_DEBUG_TCL", false)) {
         fprintf(stderr, "chrome9: TCL cannot express control flow\n");
         if (debug_get_num_option("CHROME9_DEBUG_TCL_NIR", 0))
            nir_print_shader(nir, stderr);
      }
      goto out;
   }
   /* CB0 rows come first so immediates can follow them. */
   nir_foreach_block(blk, b.impl) {
      nir_foreach_instr(instr, blk) {
         unsigned row, lane;

         instr->pass_flags = 0;
         if (instr->type == nir_instr_type_intrinsic &&
             tcl_ubo_row(nir_instr_as_intrinsic(instr), &row, &lane))
            p->uniform_rows = MAX2(p->uniform_rows, row + 1);
         /* Prefetched lookups follow the attribute inputs. */
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_load_input)
            p->attrib_count = MAX2(p->attrib_count,
               nir_intrinsic_base(nir_instr_as_intrinsic(instr)) + 1);
         /* An indirect read may reach any row of the default block. */
         if (tcl_indirect_load(instr)) {
            indirect = true;
            if (b.relative)
               p->uniform_rows = MAX2(p->uniform_rows,
                                      MIN2(nir->num_uniforms,
                                           DRM_CHROME9_VERTEX_MAX_CONSTANTS));
         }
      }
   }
   b.values = CALLOC(b.impl->ssa_alloc, sizeof(*b.values));
   if (!b.values)
      goto out;
   memset(b.constant_temp, 0xff, sizeof(b.constant_temp));
   memset(b.input_temp, 0xff, sizeof(b.input_temp));
   util_dynarray_init(&b.expr_keys, NULL);
   util_dynarray_init(&b.uniform_ifs, NULL);
   if (b.flow) {
      tcl_mark_reps(&b, &b.impl->body);
      tcl_skip_dead(&b, b.impl);
   }
   if (indirect) {
      b.fp = nir_create_fp_analysis_state(b.impl);
      tcl_skip_addresses(&b);
   }
   tcl_emit_list(&b, &b.impl->body);
   if (!b.failed)
      tcl_fill_outputs(&b);
   if (b.address_count && !b.failed)
      tcl_relative_finish(&b);
   if (!b.failed)
      tcl_pack_temp_lanes(p, b.next_temp);
   if (indirect)
      nir_free_fp_analysis_state(&b.fp);
   util_dynarray_fini(&b.uniform_ifs);
   util_dynarray_fini(&b.expr_keys);
   if (b.failed && debug_get_num_option("CHROME9_DEBUG_TCL_NIR", 0))
      nir_print_shader(nir, stderr);
   p->supported = !b.failed && p->instruction_count && p->input_count;
   FREE(b.values);
out:
   ralloc_free(nir);
   return p->supported;
}

void
chrome9_tcl_program_fini(struct chrome9_tcl_program *p)
{
   FREE(p->expr);
   p->expr = NULL;
   p->expr_count = 0;
}

static bool
tcl_compare(uint16_t op, float a, float c)
{
   switch (op) {
   case nir_op_slt:
      return a < c;
   case nir_op_sge:
      return a >= c;
   case nir_op_seq:
      return a == c;
   default:
      return a != c;
   }
}

/* The NIR ALU ops tcl_expr accepts, on floats (integers are lowered). */
static float
tcl_expr_alu_eval(uint16_t op, const float *s)
{
   switch (op) {
   case nir_op_mov: return s[0];
   case nir_op_fneg: return -s[0];
   case nir_op_fabs: return fabsf(s[0]);
   case nir_op_fadd: return s[0] + s[1];
   case nir_op_fsub: return s[0] - s[1];
   case nir_op_fmul: return s[0] * s[1];
   case nir_op_ffma:
   case nir_op_ffma_weak: return s[0] * s[1] + s[2];
   case nir_op_fmin: return fminf(s[0], s[1]);
   case nir_op_fmax: return fmaxf(s[0], s[1]);
   case nir_op_slt:
   case nir_op_sge:
   case nir_op_seq:
   case nir_op_sne: return tcl_compare(op, s[0], s[1]) ? 1.0f : 0.0f;
   case nir_op_ffloor: return floorf(s[0]);
   case nir_op_fceil: return ceilf(s[0]);
   case nir_op_ftrunc: return truncf(s[0]);
   case nir_op_ffract: return s[0] - floorf(s[0]);
   case nir_op_fsat: return fminf(fmaxf(s[0], 0.0f), 1.0f);
   case nir_op_fsign: return s[0] > 0.0f ? 1.0f : s[0] < 0.0f ? -1.0f : 0.0f;
   case nir_op_frcp: return 1.0f / s[0];
   case nir_op_frsq: return 1.0f / sqrtf(s[0]);
   case nir_op_fsqrt: return sqrtf(s[0]);
   case nir_op_fcsel: return s[0] != 0.0f ? s[1] : s[2];
   case nir_op_fcsel_gt: return s[0] > 0.0f ? s[1] : s[2];
   default: return s[0] >= 0.0f ? s[1] : s[2];
   }
}

/*
 * Per-draw b# mask and i# counts ({trip, 0, 1, 0}, the vs_2_0 defi form)
 * from the current CB0; false when a trip count exceeds 255.
 */
bool
chrome9_tcl_flow_constants(const struct chrome9_tcl_program *p,
                           const uint8_t *cb0, size_t size, uint32_t *bools,
                           uint8_t (*ints)[4])
{
   float v[UINT8_MAX + 1];

   *bools = 0;
   memset(ints, 0, DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS * sizeof(*ints));
   for (unsigned i = 0; i < p->expr_count; i++) {
      const struct chrome9_tcl_expr *e = &p->expr[i];
      float src[3];

      if (e->op == TCL_EXPR_LITERAL) {
         memcpy(&v[i], &e->value, sizeof(v[i]));
         continue;
      }
      if (e->op == TCL_EXPR_DWORD) {
         if (!cb0 || (e->value + 1ull) * 4 > size)
            return false;
         memcpy(&v[i], cb0 + e->value * 4, sizeof(v[i]));
         continue;
      }
      for (unsigned k = 0; k < 3; k++)
         src[k] = v[e->src[k]];
      v[i] = tcl_expr_alu_eval(e->op, src);
   }
   for (unsigned i = 0; i < p->bool_count; i++)
      if (v[p->bool_expr[i]] != 0.0f)
         *bools |= 1u << i;
   for (unsigned i = 0; i < p->rep_count; i++) {
      const struct chrome9_tcl_rep *r = &p->reps[i];
      float counter = r->init;
      unsigned k;

      for (k = 0; k <= UINT8_MAX; k++, counter += r->step)
         if (tcl_compare(r->compare, r->counter_first ? counter : v[r->bound],
                         r->counter_first ? v[r->bound] : counter) ==
             r->break_on_true)
            break;
      if (k > UINT8_MAX)
         return false;
      ints[i][0] = k;
      ints[i][2] = 1;
   }
   return true;
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

      chrome9_tcl_program_fini(&shader->variants->program);
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

   /* Quads do not follow the provoking-vertex convention (the screen
    * reports quads_follow_provoking_vertex_convention false): last. */
   if (mode == MESA_PRIM_QUADS || mode == MESA_PRIM_QUAD_STRIP)
      first = false;
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

/*
 * Submission order of a draw: a list expands every triangle, a native
 * strip or fan sends its vertices.  Strip chunks start at an even triangle
 * and overlap by two vertices, fan chunks repeat vertex 0, line strip
 * chunks overlap by one vertex and a line loop closes on vertex 0.
 */
struct tcl_chunker {
   enum mesa_prim mode;
   uint32_t topology;
   bool flat_first;
   unsigned primitives;
   unsigned done;
   /* Vertices of the draw, for the line loop wrap. */
   unsigned vertices;
};

/* Next chunk of at most max_vertices positions (relative to the draw's
 * first vertex); returns its primitive count, 0 when finished. */
static unsigned
tcl_chunk(struct tcl_chunker *c, unsigned max_vertices, unsigned *positions,
          unsigned *vertex_count)
{
   unsigned n = c->primitives - c->done, k = 0;

   if (!n)
      return 0;
   switch (c->topology) {
   case DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_STRIP:
      n = MIN2(n, (max_vertices - 2) & ~1u);
      for (unsigned i = 0; i < n + 2; i++)
         positions[k++] = c->done + i;
      break;
   case DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_FAN:
      n = MIN2(n, max_vertices - 2);
      positions[k++] = 0;
      for (unsigned i = 0; i < n + 1; i++)
         positions[k++] = c->done + 1 + i;
      break;
   case DRM_CHROME9_VERTEX_TOPOLOGY_POINT_LIST:
      n = MIN2(n, max_vertices);
      for (unsigned i = 0; i < n; i++)
         positions[k++] = c->done + i;
      break;
   case DRM_CHROME9_VERTEX_TOPOLOGY_LINE_LIST:
      n = MIN2(n, max_vertices / 2);
      for (unsigned i = 0; i < 2 * n; i++)
         positions[k++] = 2 * c->done + i;
      break;
   case DRM_CHROME9_VERTEX_TOPOLOGY_LINE_STRIP:
      n = MIN2(n, max_vertices - 1);
      for (unsigned i = 0; i < n + 1; i++)
         positions[k++] = (c->done + i) % c->vertices;
      break;
   default:
      n = MIN2(n, max_vertices / 3);
      for (unsigned t = c->done; t < c->done + n; t++) {
         tcl_triangle(c->mode, t, c->flat_first, &positions[k]);
         k += 3;
      }
      break;
   }
   c->done += n;
   *vertex_count = k;
   return n;
}

/* Native assembly keeps the GL provoking-last vertex (hardware vertex c).
 * Quad strips stay decomposed: a native strip splits their quads along
 * the other diagonal than the draw module does. */
static uint32_t
tcl_topology(const struct chrome9_context *ctx, enum mesa_prim mode,
             uint32_t flags)
{
   const bool flat = flags & DRM_CHROME9_VERTEX_FLAT;

   if (!ctx->tcl_topology ||
       flags & (DRM_CHROME9_VERTEX_BFC_CW | DRM_CHROME9_VERTEX_BFC_CCW) ||
       (flat && ctx->rasterizer->flatshade_first))
      return DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_LIST;
   switch (mode) {
   case MESA_PRIM_TRIANGLE_STRIP:
      return DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_STRIP;
   case MESA_PRIM_TRIANGLE_FAN:
      return DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_FAN;
   default:
      return DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_LIST;
   }
}

/* Kernel primitive class and raster state of one draw. */
struct tcl_raster {
   bool triangles;
   /* Points and lines; triangles take tcl_topology(). */
   uint32_t topology;
   unsigned primitives;
   uint32_t point_size;
   uint32_t flags;
};

/* GL aliased lines one pixel wide (widths below 1.5 round to one) under
 * the diamond-exit rule, which A01[22] selects; smooth lines of that
 * width with hardware edge antialiasing. */
static const char *
tcl_raster_lines(const struct chrome9_context *ctx,
                 const struct pipe_rasterizer_state *rs)
{
   if ((rs->line_smooth && !ctx->tcl_edge_aa) ||
       rs->line_stipple_enable ||
       /* st asks for rectangular smooth lines; the hardware AA's shape. */
       (rs->line_rectangular && !rs->line_smooth) ||
       rs->line_last_pixel || rs->point_line_tri_clip ||
       !rs->half_pixel_center || !(rs->line_width < 1.5f))
      return "wide, smooth or stippled lines";
   return NULL;
}

/*
 * Aliased points of a state size (rounded unless sprites are on, where
 * only whole sizes are accepted) or of the rounded VS size, and sprite
 * coordinates for the logical varyings sprite_coord_enable names (TEXn is
 * generic n, VARn generic 9 + n) with the upper-left origin of the
 * hardware; the FS variant flips t for lower-left.
 */
static const char *
tcl_raster_points(const struct chrome9_context *ctx, bool polygon,
                  struct tcl_raster *r)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   const nir_shader *fs = ctx->fs->nir;
   float size = rs->point_size;
   unsigned sprite = 0, read = 0;

   if (rs->point_smooth || rs->point_line_tri_clip || !rs->half_pixel_center)
      return "smooth or clipped points";
   if (rs->point_size_per_vertex) {
      /* The VS size is rounded, which sprites must not be. */
      if (polygon || !ctx->tcl_point_size ||
          rs->point_quad_rasterization ||
          !(ctx->vs->tcl->nir->info.outputs_written & VARYING_BIT_PSIZ))
         return "per-vertex point size";
      r->flags |= DRM_CHROME9_RASTER_POINT_SIZE_VERTEX;
      size = ctx->base.screen->caps.max_point_size;
   } else if (!rs->point_quad_rasterization) {
      size = MAX2(1.0f, roundf(size));
   } else if (!(size >= 1.0f) || size != roundf(size)) {
      return "fractional point size";
   }
   if (!(size <= 2048.0f))
      return "point size";
   if (rs->point_quad_rasterization && rs->sprite_coord_enable) {
      if (!fs || fs->info.inputs_read & VARYING_BIT_PNTC)
         return "point coordinate";
      for (unsigned n = 0; n < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; n++) {
         const bool tex = fs->info.inputs_read & VARYING_BIT_TEX(n);
         const bool var = fs->info.inputs_read & VARYING_BIT_VAR(n);

         if (tex || var)
            read |= BITFIELD_BIT(n);
         if (!(tex && rs->sprite_coord_enable & BITFIELD_BIT(n)) &&
             !(var && rs->sprite_coord_enable & BITFIELD_BIT(9 + n)))
            continue;
         /* Both would feed logical varying n. */
         if (tex && var)
            return "point coordinate";
         sprite |= BITFIELD_BIT(n);
      }
   }
   if (sprite) {
      /* Lower-left takes the FS variant that flips t. */
      if (polygon ||
          (rs->sprite_coord_mode != PIPE_SPRITE_COORD_UPPER_LEFT &&
           !(ctx->fs_sprite_flip && ctx->fs->applied &&
             ctx->fs->applied->sprite_flip == ctx->fs_sprite_flip)))
         return "point sprite origin";
      /* A01[21] replaces every coordinate, A2F[9:0] selects none (rung
       * sprite P3b); user clip distances ride on coordinates too. */
      if (sprite != read || rs->clip_plane_enable)
         return "point sprite on a subset of coordinates";
      r->flags |= DRM_CHROME9_RASTER_POINT_SPRITE |
                  DRM_CHROME9_RASTER_SPRITE_MASK(sprite);
   }
   memcpy(&r->point_size, &size, sizeof(r->point_size));
   return NULL;
}

static uint32_t
tcl_polygon_mode(unsigned mode)
{
   return mode == PIPE_POLYGON_MODE_LINE ? DRM_CHROME9_POLYGON_LINE :
          mode == PIPE_POLYGON_MODE_POINT ? DRM_CHROME9_POLYGON_POINT :
          DRM_CHROME9_POLYGON_FILL;
}

/*
 * Native points and lines, or triangles with their polygon modes; returns
 * the fallback reason.  Quads and polygons hide inner edges and per-vertex
 * edge flags hide others, so their unfilled forms stay with the draw
 * module, as do flat unfilled polygons and polygon offset of lines and
 * points.
 */
static const char *
tcl_raster(const struct chrome9_context *ctx, const struct pipe_draw_info *info,
           unsigned count, bool flat, struct tcl_raster *r)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   unsigned front = rs->fill_front, back = rs->fill_back;
   const char *why;

   memset(r, 0, sizeof(*r));
   switch (info->mode) {
   case MESA_PRIM_POINTS:
      if (!ctx->tcl_points)
         return "primitive type";
      r->topology = DRM_CHROME9_VERTEX_TOPOLOGY_POINT_LIST;
      r->primitives = count;
      return tcl_raster_points(ctx, false, r);
   case MESA_PRIM_LINES:
   case MESA_PRIM_LINE_STRIP:
   case MESA_PRIM_LINE_LOOP:
      if (!ctx->tcl_lines)
         return "primitive type";
      /* Flat lines take vertex B, the last one. */
      if (flat && rs->flatshade_first)
         return "flat lines from the first vertex";
      r->topology = info->mode == MESA_PRIM_LINES ?
                    DRM_CHROME9_VERTEX_TOPOLOGY_LINE_LIST :
                    DRM_CHROME9_VERTEX_TOPOLOGY_LINE_STRIP;
      r->primitives = info->mode == MESA_PRIM_LINES ? count / 2 :
                      count < 2 ? 0 :
                      info->mode == MESA_PRIM_LINE_LOOP ? count : count - 1;
      /* Edge AA blends coverage into the destination; without blending
       * GL coverage changes only alpha, so those lines stay aliased. */
      if (rs->line_smooth && ctx->blend && ctx->blend->rt[0].blend_enable)
         r->flags |= DRM_CHROME9_RASTER_EDGE_AA;
      return tcl_raster_lines(ctx, rs);
   default:
      break;
   }
   r->triangles = true;
   r->primitives = tcl_triangle_count(info->mode, count);
   /* Polygon smooth without blending: the hardware's coverage blend
    * (it applies coverage once more to blended fragments). */
   if (rs->poly_smooth && ctx->tcl_edge_aa && ctx->blend &&
       !ctx->blend->rt[0].blend_enable &&
       rs->fill_front == PIPE_POLYGON_MODE_FILL &&
       rs->fill_back == PIPE_POLYGON_MODE_FILL)
      r->flags |= DRM_CHROME9_RASTER_EDGE_AA;
   if (!r->primitives &&
       !tcl_triangle(info->mode, 0, false, (unsigned[3]){ 0 }))
      return "primitive type";
   /* The mode of a culled face is never seen. */
   if (rs->cull_face == PIPE_FACE_BACK)
      back = front;
   else if (rs->cull_face == PIPE_FACE_FRONT)
      front = back;
   if (front == PIPE_POLYGON_MODE_FILL && back == PIPE_POLYGON_MODE_FILL)
      return NULL;
   if (!ctx->tcl_polygon_mode || front > PIPE_POLYGON_MODE_POINT ||
       back > PIPE_POLYGON_MODE_POINT ||
       (front == PIPE_POLYGON_MODE_POINT) !=
          (back == PIPE_POLYGON_MODE_POINT) ||
       (info->mode != MESA_PRIM_TRIANGLES &&
        info->mode != MESA_PRIM_TRIANGLE_STRIP &&
        info->mode != MESA_PRIM_TRIANGLE_FAN) || flat ||
       ctx->vs->tcl->nir->info.outputs_written & VARYING_BIT_EDGE ||
       ((front == PIPE_POLYGON_MODE_LINE || back == PIPE_POLYGON_MODE_LINE) &&
        rs->offset_line) ||
       (front == PIPE_POLYGON_MODE_POINT && rs->offset_point))
      return "polygon mode";
   if (front == PIPE_POLYGON_MODE_LINE || back == PIPE_POLYGON_MODE_LINE) {
      /* Smooth polygon-mode lines stay in llvmpipe. */
      why = rs->line_smooth ? "smooth polygon-mode lines" :
                              tcl_raster_lines(ctx, rs);
      if (why)
         return why;
   }
   /* GL draws the vertices of the clipped polygon, the hardware those of
    * the unclipped triangle. */
   if (front == PIPE_POLYGON_MODE_POINT)
      return "polygon point mode";
   r->flags |= DRM_CHROME9_RASTER_POLYGON_CW(
                  tcl_polygon_mode(rs->front_ccw ? back : front)) |
               DRM_CHROME9_RASTER_POLYGON_CCW(
                  tcl_polygon_mode(rs->front_ccw ? front : back));
   return NULL;
}

struct tcl_stream {
   /* Vertex first's element; size counts from there. */
   const uint8_t *base;
   size_t size;
   unsigned first;
   /* Synthetic streams: a CPU array (uploaded per draw) or a buffer, and
    * this element's offset in it. */
   const uint8_t *user;
   struct pipe_resource *resource;
   unsigned user_offset;
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
      streams[k].first = 0;
      streams[k].user = NULL;
      streams[k].resource = NULL;
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
      const size_t offset = (size_t)(index - stream->first) * stream->stride;

      if (index < stream->first ||
          (stream->size != SIZE_MAX &&
           (offset > stream->size || stream->size - offset < stream->bytes)))
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
                 unsigned *buffer_count, struct pipe_resource **uploads,
                 struct pipe_resource **resources)
{
   const void *user[DRM_CHROME9_VERTEX_MAX_INPUTS];
   uint64_t low[DRM_CHROME9_VERTEX_MAX_INPUTS];
   uint64_t high[DRM_CHROME9_VERTEX_MAX_INPUTS];
   unsigned n = 0;

   unsigned first[DRM_CHROME9_VERTEX_MAX_INPUTS];

   for (unsigned k = 0; k < inputs; k++) {
      const bool synthetic = streams[k].user || streams[k].resource;
      const struct pipe_vertex_element *el = synthetic ? NULL :
         &ctx->vertex_elements->elements[k];
      const struct pipe_vertex_buffer *vb = el ?
         &ctx->vertex_buffers[el->vertex_buffer_index] : NULL;
      const unsigned stride = streams[k].stride;
      const uint64_t offset = el ?
         (uint64_t)vb->buffer_offset + el->src_offset : streams[k].user_offset;
      struct pipe_resource *resource = streams[k].resource ?
         streams[k].resource : vb && !vb->is_user_buffer ?
         vb->buffer.resource : NULL;
      const void *array = streams[k].resource ? NULL :
                          !el ? streams[k].user :
                          vb->is_user_buffer ? vb->buffer.user : NULL;
      uint32_t handle = 0;
      unsigned b;

      /* Stride 0 repeats one element (silicon rung B12). */
      if ((!array && !resource) || !streams[k].dwords ||
          stride > 2044 || stride % 4 || offset % 4 || offset > UINT32_MAX)
         return false;
      if (!array)
         handle = chrome9_resource(resource)->bo.handle;
      for (b = 0; b < n; b++)
         if (buffers[b].stride == stride &&
             (array ? user[b] == array : !user[b] && buffers[b].handle == handle))
            break;
      if (b == n) {
         buffers[n].handle = handle;
         buffers[n].stride = stride;
         buffers[n].offset = (uint64_t)(min - streams[k].first) * stride;
         user[n] = array;
         first[n] = streams[k].first;
         resources[n] = array ? NULL : resource;
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
                           (uint64_t)(min - first[b]) * buffers[b].stride +
                           low[b],
                        &out_offset, &uploads[b]);
      if (!uploads[b])
         return false;
      resources[b] = uploads[b];
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

static bool
tcl_vertex_equal(const struct tcl_stream *streams, unsigned inputs,
                 unsigned a, unsigned b)
{
   for (unsigned k = 0; k < inputs; k++)
      if (memcmp(streams[k].base +
                    (size_t)(a - streams[k].first) * streams[k].stride,
                 streams[k].base +
                    (size_t)(b - streams[k].first) * streams[k].stride,
                 streams[k].bytes))
         return false;
   return true;
}

/* Identity of the attribute data of a non-indexed draw over buffers
 * whose writes are announced; false for data that may change silently. */
static bool
tcl_stream_key(const struct chrome9_context *ctx, unsigned inputs,
               const struct tcl_stream *streams,
               const struct pipe_draw_start_count_bias *range,
               struct tcl_dedupe_stream *key)
{
   memset(key, 0, DRM_CHROME9_VERTEX_MAX_INPUTS * sizeof(*key));
   for (unsigned k = 0; k < inputs; k++) {
      const struct pipe_vertex_element *el = &ctx->vertex_elements->elements[k];
      const struct pipe_vertex_buffer *vb =
         &ctx->vertex_buffers[el->vertex_buffer_index];
      const struct chrome9_resource *res;

      /* Persistent mappings change without an announced write. */
      if (vb->is_user_buffer || !vb->buffer.resource ||
          (vb->buffer.resource->flags & (PIPE_RESOURCE_FLAG_MAP_PERSISTENT |
                                         PIPE_RESOURCE_FLAG_MAP_COHERENT)) ||
          ((uint64_t)range->start + range->count - 1) * streams[k].stride +
             streams[k].bytes > streams[k].size)
         return false;
      res = chrome9_resource_const(vb->buffer.resource);
      key[k].serial = res->serial;
      key[k].generation = res->cpu_write_generation;
      key[k].offset = (uint64_t)vb->buffer_offset + el->src_offset;
      key[k].stride = streams[k].stride;
      key[k].bytes = streams[k].bytes;
   }
   return true;
}

/*
 * Maps each vertex of a long non-indexed draw to an equal vertex at most
 * TCL_DEDUPE_WINDOW positions earlier, so the post-transform cache reuses
 * the repeated vertices of meshes stored as triangle soup.  The map is kept while the buffers do not
 * change; NULL when it does not apply or too few vertices repeat.
 */
static const uint32_t *
tcl_dedupe_map(struct chrome9_context *ctx, struct chrome9_tcl_state *state,
               const struct chrome9_tcl_program *p,
               const struct tcl_stream *streams,
               const struct pipe_draw_start_count_bias *range)
{
   struct tcl_dedupe_stream key[DRM_CHROME9_VERTEX_MAX_INPUTS];
   const nir_shader *nir = ctx->vs->tcl->nir;
   /* Prefetched lookups are functions of the attributes. */
   const unsigned inputs = p->input_count - p->prefetch_count;
   struct tcl_dedupe *victim = &state->dedupe[0];
   uint32_t *table, *first, mask, unique = 0;

   if (!ctx->tcl_dedupe || range->count < TCL_DEDUPE_MIN_VERTICES ||
       !inputs ||
       BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_VERTEX_ID) ||
       BITSET_TEST(nir->info.system_values_read,
                   SYSTEM_VALUE_VERTEX_ID_ZERO_BASE))
      return NULL;
   if (!tcl_stream_key(ctx, inputs, streams, range, key))
      return NULL;
   for (unsigned e = 0; e < TCL_DEDUPE_ENTRIES; e++) {
      struct tcl_dedupe *entry = &state->dedupe[e];

      if (entry->valid && entry->inputs == inputs &&
          entry->start == range->start && entry->count == range->count &&
          !memcmp(entry->streams, key, inputs * sizeof(*key))) {
         entry->last_use = ++state->dedupe_clock;
         if (entry->built)
            return entry->first;
         victim = entry;
         break;
      }
      if (!entry->valid ||
          (victim->valid && entry->last_use < victim->last_use))
         victim = entry;
   }

   if (!victim->valid || victim->inputs != inputs ||
       victim->start != range->start || victim->count != range->count ||
       memcmp(victim->streams, key, inputs * sizeof(*key))) {
      free(victim->first);
      memcpy(victim->streams, key, sizeof(key));
      victim->inputs = inputs;
      victim->start = range->start;
      victim->count = range->count;
      victim->first = NULL;
      victim->built = false;
      victim->valid = true;
      victim->last_use = ++state->dedupe_clock;
      return NULL;
   }
   mask = util_next_power_of_two(2 * range->count) - 1;
   table = calloc(mask + 1, sizeof(*table));
   first = malloc(range->count * sizeof(*first));
   if (!table || !first) {
      free(table);
      free(first);
      return NULL;
   }
   for (unsigned i = 0; i < range->count; i++) {
      const unsigned v = range->start + i;
      uint32_t hash = 0, slot;

      for (unsigned k = 0; k < inputs; k++)
         hash = _mesa_hash_data_with_seed(streams[k].base +
                                             (size_t)v * streams[k].stride,
                                          streams[k].bytes, hash);
      for (slot = hash & mask; table[slot]; slot = (slot + 1) & mask)
         if (tcl_vertex_equal(streams, inputs, range->start + table[slot] - 1,
                              v))
            break;
      if (table[slot] && i - (table[slot] - 1) <= TCL_DEDUPE_WINDOW) {
         first[i] = table[slot] - 1;
      } else {
         table[slot] = i + 1;
         first[i] = i;
         unique++;
      }
   }
   free(table);
   if (unique > range->count - range->count / 8) {
      free(first);
      first = NULL;
   }
   victim->first = first;
   victim->built = true;
   return first;
}

void
chrome9_tcl_state_destroy(struct chrome9_tcl_state *state)
{
   if (!state)
      return;
   FREE(state->coords);
   for (unsigned r = 0; r < ARRAY_SIZE(state->prefetch_ring); r++)
      pipe_resource_reference(&state->prefetch_ring[r], NULL);
   free(state->compact.index);
   free(state->compact.vertices);
   pipe_resource_reference(&state->compact.buffer, NULL);
   for (unsigned e = 0; e < TCL_DEDUPE_ENTRIES; e++)
      free(state->dedupe[e].first);
   FREE(state);
}

/*
 * Stage B draw: triangle list indices (provoking vertex last) rebased to
 * min, in chunks.  Returns false only when nothing was submitted, so
 * stage A can retry.
 */
static bool
tcl_draw_buffers(struct chrome9_context *ctx, const struct pipe_draw_info *info,
                 const struct pipe_draw_start_count_bias *range,
                 const uint8_t *indices, const uint32_t *first,
                 struct tcl_chunker chunker,
                 uint32_t min, struct chrome9_tcl_state *state,
                 struct drm_chrome9_draw_vertex_program *draw,
                 struct pipe_resource *const *resources)
{
   const int64_t bias = (int64_t)(info->index_size ? range->index_bias : 0) -
                        min;
   unsigned count, n;

   draw->indices_ptr = (uintptr_t)state->indices;
   while ((count = tcl_chunk(&chunker, DRM_CHROME9_VERTEX_MAX_INDICES,
                             state->positions, &n))) {
      uint32_t lo = UINT32_MAX, hi = 0;

      for (unsigned i = 0; i < n; i++) {
         const unsigned position = range->start + state->positions[i];
         int64_t value = first ?
            range->start + first[state->positions[i]] : position;

         if (info->index_size == 1)
            value = indices[position];
         else if (info->index_size == 2)
            value = ((const uint16_t *)indices)[position];
         else if (info->index_size == 4)
            value = ((const uint32_t *)indices)[position];
         state->indices[i] = value + bias;
         lo = MIN2(lo, state->indices[i]);
         hi = MAX2(hi, state->indices[i]);
      }
      /* A chunk spanning at most 64Ki vertices sends 16-bit indices
       * relative to its lowest vertex: half the ring bytes. */
      if (n && hi - lo <= UINT16_MAX) {
         uint16_t *narrow = (uint16_t *)state->indices;

         for (unsigned i = 0; i < n; i++)
            narrow[i] = state->indices[i] - lo;
         draw->index_size = 2;
         draw->base_vertex = lo;
      } else {
         draw->index_size = 4;
         draw->base_vertex = 0;
      }
      draw->index_count = n;
      if (!chrome9_draw_tcl_submit(ctx, &state->fragment, draw, count,
                                   resources, draw->buffer_count)) {
         ctx->tcl_buffer_rejects++;
         if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
            fprintf(stderr, "chrome9: TCL buffers rejected (%s): buffers=%u "
                            "indices=%u min=%u textures=%u\n",
                    strerror(errno), draw->buffer_count, n, min,
                    state->fragment.draw.texture_count);
         if (chunker.done == count)
            return false;
         CHROME9_REJECT(ctx, chunker.primitives - chunker.done + count);
         return true;
      }
   }
   ctx->tcl_buffer_triangles += chunker.primitives;
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
   const unsigned pad = p->constant_pad;

   memset(rows, 0, pad * sizeof(*rows));
   memset(rows + chrome9_tcl_constant_count(p) - pad, 0, pad * sizeof(*rows));
   rows += pad;
   if (p->uniform_rows) {
      if (!map && cb->buffer)
         map = chrome9_resource(cb->buffer)->bo.map;
      if (!map || cb->buffer_size < p->uniform_rows * 16)
         return false;
      /* As in chrome9_draw_vbo(): the offset applies to both kinds. */
      memcpy(rows, map + cb->buffer_offset, p->uniform_rows * 16);
   }
   memcpy(rows + p->uniform_rows, p->immediates, p->immediate_count * 16);
   chrome9_finite_constants(rows[0],
                            (p->uniform_rows + p->immediate_count) * 4);
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

/* CB0 as the VS reads it, or NULL. */
static const uint8_t *
tcl_cb0(const struct chrome9_context *ctx, size_t *size)
{
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *map = cb->user_buffer;

   if (!map && cb->buffer)
      map = chrome9_resource(cb->buffer)->bo.map;
   *size = map ? cb->buffer_size : 0;
   return map ? map + cb->buffer_offset : NULL;
}

static struct chrome9_tcl_variant *
tcl_variant(struct chrome9_context *ctx, bool clip_halfz,
            enum chrome9_tcl_colour colour, uint8_t clip_enable, bool unkeyed,
            bool point_clip, const uint32_t *key)
{
   const uint8_t point_size =
      (ctx->tcl_point_size ? CHROME9_TCL_POINT_SIZE : 0) |
      (point_clip ? CHROME9_TCL_POINT_CLIP : 0);
   struct chrome9_tcl_shader *shader = ctx->vs->tcl;
   struct chrome9_tcl_variant **link, *variant;

   /* Most recently used first; the tail is reused when the list is full. */
   for (link = &shader->variants; (variant = *link); link = &variant->next) {
      if (variant->clip_halfz == clip_halfz &&
          variant->colour == colour &&
          variant->clip_enable == clip_enable &&
          variant->point_size == point_size &&
          variant->unkeyed == unkeyed &&
          !memcmp(variant->key, key, sizeof(variant->key))) {
         *link = variant->next;
         variant->next = shader->variants;
         shader->variants = variant;
         return variant;
      }
      if (!variant->next &&
          shader->variant_count >= CHROME9_TCL_MAX_VARIANTS) {
         *link = NULL;
         shader->variant_count--;
         chrome9_tcl_program_fini(&variant->program);
         break;
      }
   }
   if (!variant)
      variant = CALLOC_STRUCT(chrome9_tcl_variant);
   if (!variant)
      return NULL;
   variant->clip_halfz = clip_halfz;
   variant->colour = colour;
   variant->clip_enable = clip_enable;
   variant->point_size = point_size;
   variant->unkeyed = unkeyed;
   memcpy(variant->key, key, sizeof(variant->key));
   chrome9_tcl_compile(shader->nir, clip_halfz, colour, clip_enable,
                       point_size, unkeyed ? 0 : shader->key_count,
                       shader->key_dword, key, &ctx->tcl_flow,
                       &variant->program);
   shader->keyed_compiles += shader->key_count && !unkeyed;
   variant->next = shader->variants;
   shader->variants = variant;
   shader->variant_count++;
   return variant;
}

static const struct chrome9_tcl_program *
tcl_program(struct chrome9_context *ctx, bool clip_halfz,
            enum chrome9_tcl_colour colour, uint8_t clip_enable,
            bool point_clip)
{
   struct chrome9_tcl_shader *shader = ctx->vs->tcl;
   const uint32_t unkeyed_key[CHROME9_TCL_MAX_KEY] = { 0 };
   uint32_t key[CHROME9_TCL_MAX_KEY] = { 0 };
   struct chrome9_tcl_variant *variant;
   const uint8_t *map;
   size_t size;

   /* Control uniforms that change every few draws: read them at draw
    * time with hardware flow control instead of compiling per value. */
   if (shader->key_count && ctx->tcl_flow.max_depth &&
       shader->keyed_compiles > CHROME9_TCL_KEY_CHURN) {
      variant = tcl_variant(ctx, clip_halfz, colour, clip_enable, true,
                            point_clip, unkeyed_key);
      if (variant && variant->program.supported)
         return &variant->program;
   }
   if (shader->key_count) {
      map = tcl_cb0(ctx, &size);
      if (!map)
         return NULL;
      for (unsigned k = 0; k < shader->key_count; k++) {
         if ((shader->key_dword[k] + 1u) * 4 > size)
            return NULL;
         memcpy(&key[k], map + shader->key_dword[k] * 4, sizeof(key[k]));
      }
   }
   variant = tcl_variant(ctx, clip_halfz, colour, clip_enable, false,
                         point_clip, key);
   return variant ? &variant->program : NULL;
}

/* Value class of one varying lane for all vertices of a draw; sprite
 * coordinates are (s, t, 0, 1) with s and t in [0, 1]. */
static enum chrome9_tcl_origin
tcl_lane_origin(const struct chrome9_context *ctx,
                const struct chrome9_tcl_program *p, unsigned sprite,
                unsigned lane)
{
   const unsigned v = lane / 4, c = lane % 4;
   const struct util_format_description *desc;
   unsigned input, channel;

   if (v >= DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
      return CHROME9_TCL_ORIGIN_UNKNOWN;
   if (sprite & BITFIELD_BIT(v))
      return c < 2 ? CHROME9_TCL_ORIGIN_UNIT :
             c == 2 ? CHROME9_TCL_ORIGIN_ZERO : CHROME9_TCL_ORIGIN_ONE;
   if (p->origin[v][c] != CHROME9_TCL_ORIGIN_INPUT)
      return p->origin[v][c];
   input = p->origin_input[v][c];
   channel = p->origin_lane[v][c];
   /* Prefetched texels have no static range. */
   if (input >= p->input_count - p->prefetch_count)
      return CHROME9_TCL_ORIGIN_UNKNOWN;
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
                   const struct chrome9_tcl_program *p, uint32_t raster)
{
   const struct chrome9_shader *fs = ctx->fs;
   const unsigned sprite = raster & DRM_CHROME9_RASTER_POINT_SPRITE ?
                           raster >> 8 & 0xff : 0;

   for (uint32_t m = fs->semantic_identity_projector_input_mask; m; m &= m - 1)
      if (tcl_lane_origin(ctx, p, sprite, __builtin_ctz(m)) !=
          CHROME9_TCL_ORIGIN_ONE)
         return false;
   for (uint32_t m = fs->semantic_equal_zero_input_mask; m; m &= m - 1) {
      const enum chrome9_tcl_origin o =
         tcl_lane_origin(ctx, p, sprite, __builtin_ctz(m));

      if (o != CHROME9_TCL_ORIGIN_ZERO && o != CHROME9_TCL_ORIGIN_ONE)
         return false;
   }
   for (uint32_t m = fs->semantic_unit_interval_input_mask; m; m &= m - 1)
      if (tcl_lane_origin(ctx, p, sprite, __builtin_ctz(m)) ==
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
                   "flags=%#x clip=%#x fs varyings=%u fs instructions=%u\n",
           errno, p->input_count, p->uniform_rows, p->immediate_count,
           draw->flags, draw->clip_distance_mask,
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

/* One culled pass of a TCL draw; false only when nothing was submitted. */
static const struct tcl_compact *
tcl_compact_get(struct chrome9_context *ctx, struct chrome9_tcl_state *state,
                const struct chrome9_tcl_program *p,
                const struct tcl_stream *streams,
                const struct pipe_draw_start_count_bias *range)
{
   struct tcl_dedupe_stream key[DRM_CHROME9_VERTEX_MAX_INPUTS];
   const unsigned inputs = p->input_count - p->prefetch_count;
   const nir_shader *nir = ctx->vs->tcl->nir;
   struct tcl_compact *c = &state->compact;
   uint32_t *table, *source, mask;
   unsigned stride = 0;

   if (!inputs || !range->count ||
       BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_VERTEX_ID) ||
       BITSET_TEST(nir->info.system_values_read,
                   SYSTEM_VALUE_VERTEX_ID_ZERO_BASE) ||
       !tcl_stream_key(ctx, inputs, streams, range, key))
      return NULL;
   if (c->valid && c->inputs == inputs && c->start == range->start &&
       c->count == range->count && !memcmp(c->streams, key, sizeof(key))) {
      if (c->built)
         return c->unique ? c : NULL;
   } else {
      /* Built on the second draw of unchanged buffers. */
      free(c->index);
      free(c->vertices);
      pipe_resource_reference(&c->buffer, NULL);
      memset(c, 0, sizeof(*c));
      memcpy(c->streams, key, sizeof(key));
      c->inputs = inputs;
      c->start = range->start;
      c->count = range->count;
      c->valid = true;
      return NULL;
   }
   c->built = true;
   for (unsigned k = 0; k < inputs; k++) {
      c->offset[k] = stride;
      stride += align(streams[k].bytes, 4);
   }
   mask = util_next_power_of_two(2 * range->count) - 1;
   table = calloc(mask + 1, sizeof(*table));
   source = malloc(range->count * sizeof(*source));
   c->index = malloc(range->count * sizeof(*c->index));
   c->vertices = malloc((size_t)range->count * stride);
   if (!table || !source || !c->index || !c->vertices || stride > 2044) {
      free(table);
      free(source);
      free(c->index);
      free(c->vertices);
      c->index = NULL;
      c->vertices = NULL;
      return NULL;
   }
   for (unsigned i = 0; i < range->count; i++) {
      const unsigned v = range->start + i;
      uint32_t hash = 0, slot;

      for (unsigned k = 0; k < inputs; k++)
         hash = _mesa_hash_data_with_seed(streams[k].base +
                                             (size_t)v * streams[k].stride,
                                          streams[k].bytes, hash);
      for (slot = hash & mask; table[slot]; slot = (slot + 1) & mask)
         if (tcl_vertex_equal(streams, inputs, source[table[slot] - 1], v))
            break;
      if (!table[slot]) {
         uint8_t *out = c->vertices + (size_t)c->unique * stride;

         for (unsigned k = 0; k < inputs; k++)
            memcpy(out + c->offset[k],
                   streams[k].base + (size_t)v * streams[k].stride,
                   streams[k].bytes);
         source[c->unique] = v;
         table[slot] = ++c->unique;
      }
      c->index[i] = table[slot] - 1;
   }
   free(table);
   free(source);
   c->stride = stride;
   /* Few repeats, or more than one upload per frame: keep the draw. */
   if (c->unique > range->count - range->count / 8 ||
       (uint64_t)c->unique * stride > TCL_MAX_UPLOAD_BYTES) {
      free(c->index);
      free(c->vertices);
      c->index = NULL;
      c->vertices = NULL;
      c->unique = 0;
      return NULL;
   }
   uint8_t *shrunk = realloc(c->vertices, (size_t)c->unique * stride);

   if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
      fprintf(stderr, "chrome9: TCL compacted %u vertices to %u (stride %u)\n",
              range->count, c->unique, stride);

   if (shrunk)
      c->vertices = shrunk;
   c->buffer = pipe_buffer_create(ctx->base.screen, PIPE_BIND_VERTEX_BUFFER,
                                  PIPE_USAGE_IMMUTABLE,
                                  (size_t)c->unique * stride);
   if (!c->buffer) {
      c->unique = 0;
      return NULL;
   }
   pipe_buffer_write(&ctx->base, c->buffer, 0, (size_t)c->unique * stride,
                     c->vertices);
   return c;
}

/*
 * Sample every prefetched lookup for the draw's vertices [min, max] into
 * one interleaved FLOAT4 array and describe it as extra input streams.
 */
static bool
tcl_prefetch(struct chrome9_context *ctx, uint32_t min, uint32_t max,
             const struct chrome9_tcl_program *p,
             struct chrome9_tcl_state *state, struct tcl_stream *streams)
{
   const unsigned lookups = p->prefetch_count;
   struct pipe_resource **slot;
   float *prefetch;
   size_t bytes;

   bytes = ((size_t)max - min + 1) * lookups * 16;
   if (bytes > TCL_MAX_UPLOAD_BYTES)
      return false;
   /* Written after the draw three turns back; the access waits for it. */
   slot = &state->prefetch_ring[state->prefetch_next++ %
                                ARRAY_SIZE(state->prefetch_ring)];
   if (!*slot || (*slot)->width0 < bytes) {
      pipe_resource_reference(slot, NULL);
      *slot = pipe_buffer_create(ctx->base.screen, PIPE_BIND_VERTEX_BUFFER,
                                 PIPE_USAGE_STREAM, bytes);
      if (!*slot)
         return false;
   }
   chrome9_resource_cpu_access_range(chrome9_resource(*slot), true, 0, bytes);
   prefetch = chrome9_resource(*slot)->bo.map;
   if (state->prefetch_coords < 2 * ((size_t)max - min + 1)) {
      const size_t coords = 2 * ((size_t)max - min + 1);
      float *grown = REALLOC(state->coords,
                             state->prefetch_coords * sizeof(float),
                             coords * sizeof(float));

      if (!grown)
         return false;
      state->coords = grown;
      state->prefetch_coords = coords;
   }
   for (unsigned k = 0; k < lookups; k++) {
      const struct chrome9_tcl_prefetch *f = &p->prefetch[k];
      const struct tcl_stream *coord = &streams[f->input];
      const bool floats32 = coord->format == PIPE_FORMAT_R32G32_FLOAT ||
                            coord->format == PIPE_FORMAT_R32G32B32_FLOAT ||
                            coord->format == PIPE_FORMAT_R32G32B32A32_FLOAT;
      const unsigned vertices = max - min + 1;
      float *s = state->coords, *t = state->coords + vertices;
      float *out = prefetch + k * 4;

      if (f->input >= p->attrib_count ||
          !chrome9_vs_texture_samplable(ctx, f->unit, f->sampler))
         return false;
      for (unsigned i = 0; i < vertices; i++) {
         const size_t offset = (size_t)(min + i - coord->first) *
                               coord->stride;
         float value[4];

         if (min + i < coord->first ||
             (coord->size != SIZE_MAX &&
              (offset > coord->size || coord->size - offset < coord->bytes)))
            return false;
         if (floats32)
            memcpy(value, coord->base + offset, coord->bytes);
         else
            util_format_unpack_rgba(coord->format, value,
                                    coord->base + offset, 1);
         s[i] = value[f->s];
         t[i] = value[f->t];
      }
      chrome9_vs_texture_sample_array(ctx, f->unit, f->sampler, vertices,
                                      s, t, out, lookups * 4);
      /* The kernel rejects non-finite float inputs. */
      for (unsigned i = 0; i < vertices; i++)
         for (unsigned c = 0; c < 4; c++)
            if (!isfinite(out[(size_t)i * lookups * 4 + c]))
               return false;
   }
   for (unsigned k = 0; k < lookups; k++) {
      struct tcl_stream *stream = &streams[p->attrib_count + k];

      memset(stream, 0, sizeof(*stream));
      stream->resource = *slot;
      stream->user_offset = k * 16;
      stream->base = (const uint8_t *)prefetch + stream->user_offset;
      stream->first = min;
      stream->stride = lookups * 16;
      stream->size = ((size_t)max - min + 1) * stream->stride - k * 16;
      stream->format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      stream->bytes = 16;
      tcl_native_format(stream->format, stream);
   }
   return true;
}

static bool
tcl_draw_pass(struct chrome9_context *ctx, const struct pipe_draw_info *info,
              const struct pipe_draw_start_count_bias *range,
              const uint8_t *indices, const struct tcl_raster *raster,
              struct chrome9_tcl_state *state,
              const struct chrome9_tcl_program *p,
              const struct tcl_stream *bound, uint32_t flags,
              uint8_t clip_enable)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   struct drm_chrome9_vertex_element elements[DRM_CHROME9_VERTEX_MAX_INPUTS];
   struct tcl_stream streams[DRM_CHROME9_VERTEX_MAX_INPUTS];
   struct drm_chrome9_draw_vertex_program draw = { 0 };
   struct drm_chrome9_vertex_buffer buffers[DRM_CHROME9_VERTEX_MAX_INPUTS];
   const unsigned primitives = raster->primitives;
   struct tcl_chunker chunker = {
      .mode = info->mode,
      .topology = raster->triangles ? tcl_topology(ctx, info->mode, flags) :
                  raster->topology,
      .flat_first = rs->flatshade_first,
      .primitives = primitives,
      .vertices = range->count,
   };
   unsigned stride, max_vertices, buffer_count, count, n;
   uint32_t index_min, index_max;
   const struct tcl_compact *compact = NULL;
   bool have_range;
   struct pipe_draw_info compact_info;
   struct pipe_draw_start_count_bias compact_range;

   memcpy(streams, bound,
          (p->input_count - p->prefetch_count) * sizeof(*streams));
   /* Lookups of a vertex-soup draw are sampled per distinct vertex. */
   if (p->prefetch_count && !info->index_size)
      compact = tcl_compact_get(ctx, state, p, streams, range);
   if (compact) {
      for (unsigned k = 0; k < p->input_count - p->prefetch_count; k++) {
         streams[k].user = NULL;
         streams[k].resource = compact->buffer;
         streams[k].user_offset = compact->offset[k];
         streams[k].base = compact->vertices + compact->offset[k];
         streams[k].first = 0;
         streams[k].stride = compact->stride;
         streams[k].size = (size_t)compact->unique * compact->stride -
                           compact->offset[k];
      }
      compact_info = *info;
      compact_info.index_size = 4;
      compact_info.has_user_indices = true;
      compact_info.index.user = compact->index;
      compact_range.start = 0;
      compact_range.count = range->count;
      compact_range.index_bias = 0;
      info = &compact_info;
      range = &compact_range;
      indices = (const uint8_t *)compact->index;
   }
   /* Compacted draws reference every distinct vertex. */
   if (compact) {
      index_min = 0;
      index_max = compact->unique - 1;
      have_range = true;
   } else {
      have_range = (ctx->tcl_buffers || p->prefetch_count) &&
                   tcl_index_range(info, range, indices, &index_min,
                                   &index_max);
   }
   if (p->prefetch_count &&
       (!have_range ||
        !tcl_prefetch(ctx, index_min, index_max, p, state, streams)))
      return tcl_fallback(ctx, "vertex texture prefetch");
   if (!tcl_viewport(ctx, draw.viewport))
      return tcl_fallback(ctx, "viewport");
   if (!tcl_constants(ctx, p, state->constants))
      return tcl_fallback(ctx, "constants");
   if (p->bool_count || p->rep_count) {
      size_t size;
      const uint8_t *cb0 = tcl_cb0(ctx, &size);

      if (!chrome9_tcl_flow_constants(p, cb0, size, &draw.bool_constants,
                                      state->ints))
         return tcl_fallback(ctx, "flow control constants");
      draw.int_constants_ptr = p->rep_count ? (uintptr_t)state->ints : 0;
   }

   draw.abi_version = DRM_CHROME9_VERTEX_ABI_VERSION;
   draw.flags = flags;
   draw.topology = chunker.topology;
   draw.point_size = raster->point_size;
   draw.raster_flags = raster->flags;
   draw.clip_distance_mask = clip_enable;
   draw.instruction_count = p->instruction_count;
   draw.instructions_ptr = (uintptr_t)p->instructions;
   draw.element_count = p->input_count;
   draw.elements_ptr = (uintptr_t)elements;
   draw.constant_count = chrome9_tcl_constant_count(p);
   draw.constants_ptr = draw.constant_count ? (uintptr_t)state->constants : 0;
   if (ctx->tcl_buffers && have_range) {
      struct pipe_resource *uploads[DRM_CHROME9_VERTEX_MAX_INPUTS] = { 0 };
      struct pipe_resource *resources[DRM_CHROME9_VERTEX_MAX_INPUTS] = { 0 };
      bool drawn = tcl_bind_buffers(ctx, p->input_count, streams, index_min,
                                    index_max, elements, buffers,
                                    &buffer_count, uploads, resources);

      if (drawn) {
         const uint32_t *first = info->index_size ? NULL :
            tcl_dedupe_map(ctx, state, p, streams, range);

         draw.buffer_count = buffer_count;
         draw.buffers_ptr = (uintptr_t)buffers;
         drawn = tcl_draw_buffers(ctx, info, range, indices, first, chunker,
                                  index_min, state, &draw, resources);
      }
      for (unsigned b = 0; b < ARRAY_SIZE(uploads); b++)
         pipe_resource_reference(&uploads[b], NULL);
      if (drawn) {
         ctx->tcl_triangles += primitives;
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
   max_vertices = MIN2(3 * TCL_MAX_TRIANGLES, TCL_MAX_VERTEX_BYTES / stride);
   draw.vertex_stride = stride;
   draw.vertices_ptr = (uintptr_t)state->vertices;

   while ((count = tcl_chunk(&chunker, max_vertices, state->positions, &n))) {
      uint32_t *out = state->vertices;

      for (unsigned i = 0; i < n; i++) {
         const unsigned position = range->start + state->positions[i];
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
            if (chunker.done == count)
               return tcl_fallback(ctx, "vertex fetch");
            CHROME9_REJECT(ctx, primitives - chunker.done + count);
            return true;
         }
         out += written;
      }
      draw.vertex_count = n;
      if (!chrome9_draw_tcl_submit(ctx, &state->fragment, &draw, count,
                                   NULL, 0)) {
         /* A first-chunk rejection falls back to the draw module. */
         if (chunker.done == count) {
            if (debug_get_bool_option("CHROME9_DEBUG_TCL", false))
               tcl_dump(&draw, p, &state->fragment);
            return tcl_fallback(ctx, "kernel rejected the draw");
         }
         CHROME9_REJECT(ctx, primitives - chunker.done + count);
         return true;
      }
   }
   ctx->tcl_triangles += primitives;
   return true;
}

/*
 * Without hardware back-colour selection, two-sided colour draws the front
 * faces with COL0 and the back faces with BFC0 as two culled passes.  That
 * reorders the faces, so it is used only while the order cannot be seen: no
 * blending or logic op, no stencil writes, and a writing depth test that
 * keeps the nearer fragment.
 */
static bool
tcl_face_order_invisible(const struct chrome9_context *ctx)
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   const struct pipe_blend_state *blend = ctx->blend;

   if (!dsa || !blend || blend->logicop_enable || !dsa->depth_enabled ||
       !dsa->depth_writemask ||
       (dsa->depth_func != PIPE_FUNC_LESS &&
        dsa->depth_func != PIPE_FUNC_LEQUAL &&
        dsa->depth_func != PIPE_FUNC_GREATER &&
        dsa->depth_func != PIPE_FUNC_GEQUAL))
      return false;
   for (unsigned i = 0; i < 2; i++)
      if (dsa->stencil[i].enabled && dsa->stencil[i].writemask)
         return false;
   for (unsigned i = 0; i < ctx->framebuffer.nr_cbufs; i++) {
      const struct pipe_rt_blend_state *rt =
         &blend->rt[blend->independent_blend_enable ? i : 0];

      if (rt->blend_enable && rt->colormask)
         return false;
   }
   return true;
}

bool
chrome9_tcl_draw(struct chrome9_context *ctx, const struct pipe_draw_info *info,
                 const struct pipe_draw_start_count_bias *range)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   const uint32_t cull_front = rs && rs->front_ccw ?
      DRM_CHROME9_VERTEX_CULL_CCW : DRM_CHROME9_VERTEX_CULL_CW;
   const uint32_t cull_back = rs && rs->front_ccw ?
      DRM_CHROME9_VERTEX_CULL_CW : DRM_CHROME9_VERTEX_CULL_CCW;
   const struct chrome9_tcl_program *programs[2];
   enum chrome9_tcl_colour colours[2];
   uint32_t flags[2];
   struct tcl_stream streams[2][DRM_CHROME9_VERTEX_MAX_INPUTS];
   struct chrome9_tcl_state *state;
   const uint8_t *indices = NULL;
   const char *why;
   struct tcl_raster raster;
   bool flat, twoside;
   uint8_t clip_enable;
   unsigned passes = 1;

   if (!ctx->tcl_enabled)
      return false;
   if (!ctx->vs || !ctx->vs->tcl || !rs || !ctx->fs)
      return tcl_fallback(ctx, "no NIR vertex shader");
   if (info->primitive_restart || info->instance_count != 1 ||
       info->start_instance)
      return tcl_fallback(ctx, "restart or instancing");
   if (!rs->depth_clip_near || !rs->depth_clip_far || rs->rasterizer_discard)
      return tcl_fallback(ctx, "rasterizer state");
   clip_enable = rs->clip_plane_enable;
   if (clip_enable && (!ctx->tcl_clip_distance ||
                       clip_enable >> DRM_CHROME9_VERTEX_MAX_CLIP_DISTANCES))
      return tcl_fallback(ctx, "user clip planes");
   if (!tcl_fs_flat(ctx->fs, &flat))
      return tcl_fallback(ctx, "flat generic varying");
   flat |= rs->flatshade;
   why = tcl_raster(ctx, info, range->count, flat, &raster);
   if (why)
      return tcl_fallback(ctx, why);
   if (!raster.primitives)
      return true;
   /* Culling and back colours apply to polygons only. */
   if (raster.triangles && rs->cull_face == PIPE_FACE_FRONT_AND_BACK)
      return true;

   /* Back faces are invisible when culled (st also clears the flag); a
    * shader without BFC0 colours both faces with COL0. */
   twoside = raster.triangles && rs->light_twoside &&
             rs->cull_face != PIPE_FACE_BACK &&
             (ctx->vs->tcl->nir->info.outputs_written & VARYING_BIT_BFC0);
   colours[0] = CHROME9_TCL_COLOUR_FRONT;
   flags[0] = raster.triangles ? tcl_cull(rs) : 0;
   if (twoside && ctx->tcl_twoside) {
      /* Back colours go to the back-face winding. */
      colours[0] = CHROME9_TCL_COLOUR_SELECT;
      flags[0] |= rs->front_ccw ? DRM_CHROME9_VERTEX_BFC_CW :
                                  DRM_CHROME9_VERTEX_BFC_CCW;
   } else if (twoside) {
      /* Two culled passes would reorder overlapping faces: llvmpipe
       * keeps primitive order. */
      if (!tcl_face_order_invisible(ctx)) {
         if (chrome9_sw_draw_vbo(ctx, info, 0, range, 1))
            return true;
         return tcl_fallback(ctx, "two-sided color with visible face order");
      }
      if (rs->cull_face == PIPE_FACE_FRONT) {
         colours[0] = CHROME9_TCL_COLOUR_BACK;
      } else {
         flags[0] = cull_back;
         colours[1] = CHROME9_TCL_COLOUR_BACK;
         flags[1] = cull_front;
         passes = 2;
      }
   }
   for (unsigned i = 0; i < passes; i++) {
      const struct chrome9_tcl_program *p =
         tcl_program(ctx, rs->clip_halfz, colours[i], clip_enable,
                     !raster.triangles && raster.topology ==
                        DRM_CHROME9_VERTEX_TOPOLOGY_POINT_LIST);

      if (!p || !p->supported)
         return tcl_fallback(ctx, p && p->indirect ? "indirect constant" :
                                  "vertex shader not expressible");
      /* An enabled plane the VS never writes stays with the draw module. */
      if (p->clip_written != clip_enable)
         return tcl_fallback(ctx, "unwritten clip distance");
      if (!tcl_bind_inputs(ctx, p->input_count - p->prefetch_count,
                           streams[i]))
         return tcl_fallback(ctx, "vertex element format or buffer");
      if (!tcl_guarded_domain(ctx, p, raster.flags))
         return tcl_fallback(ctx, "guarded fragment domain");
      programs[i] = p;
      flags[i] |= flat ? DRM_CHROME9_VERTEX_FLAT : 0;
   }
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
   ctx->draw_filled_triangles = raster.triangles &&
      !(raster.flags & (DRM_CHROME9_RASTER_POLYGON_CW(3) |
                        DRM_CHROME9_RASTER_POLYGON_CCW(3)));
   if (!chrome9_draw_tcl_fragment(ctx, &state->fragment, &why))
      return tcl_fallback(ctx, why);
   for (unsigned i = 0; i < passes; i++) {
      if (tcl_draw_pass(ctx, info, range, indices, &raster, state,
                        programs[i], streams[i], flags[i], clip_enable))
         continue;
      if (!i)
         return false;
      /* The front faces are already drawn. */
      CHROME9_REJECT(ctx, raster.primitives);
   }
   return true;
}
