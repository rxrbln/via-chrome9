/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * Multi-pass fragment programs.  The straight-line NIR is viewed as a DAG of
 * scalar lanes.  Walking it in program order, a lane whose cone (the lanes
 * it needs back to free inputs and carried values) exceeds the record or
 * sampler budget gets part of that cone carried: the instruction whose lanes
 * in the cone save the most records per lane.  Every carried lane is a job
 * writing an R32F carrier.  Jobs are compiled by the normal backend and
 * checked against the store; a job that does not fit gets a smaller budget
 * and the plan is redone.
 */

#include "chrome9_fs_split.h"
#include "chrome9_context.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "util/u_memory.h"
#include "util/ralloc.h"

enum split_kind {
   SPLIT_FREE,
   SPLIT_COMPUTED,
   SPLIT_SINK,
};

struct split_instr {
   nir_instr *instr;
   nir_def *def;
   unsigned first_node;
   uint8_t lanes;
   /* The lane computing the same value as each lane (xxxx swizzles). */
   uint8_t canon[4];
   uint8_t kind;
   uint8_t cost;
   bool lane_cost;
   bool carriable;
};

struct split_node {
   unsigned instr;
   unsigned dep_first;
   unsigned dep_count;
   unsigned cone_cost;
   /* Record budget of this node's job, lowered when it failed to fit. */
   unsigned budget;
   int carried;
   uint8_t lane;
   bool alias;
};

struct split_graph {
   struct split_instr *instrs;
   unsigned instr_count;
   struct split_node *nodes;
   unsigned node_count;
   struct util_dynarray deps;
   /* Scratch for cone walks. */
   unsigned *node_mark;
   unsigned *instr_mark;
   unsigned *stack;
   unsigned generation;
   unsigned final_node;
};

struct split_cone {
   unsigned cost;
   struct util_dynarray reads;
   struct util_dynarray members;
};

/* Opt-in: llvmpipe runs the long programs seen so far 10-15x faster than
 * the pixel engine runs their jobs. */
bool
chrome9_fs_split_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = debug_get_bool_option("CHROME9_FS_SPLIT", false);
   return enabled;
}

/* CHROME9_DEBUG_FS_SPLIT=1 traces plans, 2 also prints jobs that fail. */
static unsigned
split_debug(void)
{
   static int level = -1;

   if (level < 0)
      level = debug_get_num_option("CHROME9_DEBUG_FS_SPLIT", 0);
   return level;
}

/* Estimated records of one vector instruction (the backend expansion). */
static unsigned
split_alu_cost(const nir_alu_instr *alu, bool *lane_cost)
{
   *lane_cost = false;
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fsat:
   case nir_op_inot:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      return 0;
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_frcp:
   case nir_op_frsq:
      *lane_cost = true;
      return 1;
   case nir_op_fsqrt:
      *lane_cost = true;
      return 2;
   case nir_op_fpow:
      *lane_cost = true;
      return 3;
   case nir_op_feq:
   case nir_op_fneu:
   case nir_op_ffloor:
   case nir_op_fsign:
      return 2;
   case nir_op_ftrunc:
   case nir_op_fceil:
   case nir_op_fround_even:
      return 5;
   default:
      return 1;
   }
}

static bool
split_float_or_bool(const nir_def *def)
{
   return def->bit_size == 1 || def->bit_size == 32;
}

static void
split_graph_free(struct split_graph *graph)
{
   FREE(graph->instrs);
   FREE(graph->nodes);
   FREE(graph->node_mark);
   FREE(graph->instr_mark);
   FREE(graph->stack);
   util_dynarray_fini(&graph->deps);
}

static unsigned
split_instr_index(const struct split_graph *graph, const nir_instr *instr)
{
   return instr->index < graph->instr_count &&
          graph->instrs[instr->index].instr == instr ?
          instr->index : UINT_MAX;
}

static bool
split_add_dep(struct split_graph *graph, const nir_src *src, unsigned lane)
{
   const unsigned i = split_instr_index(graph, nir_def_instr(src->ssa));

   if (i == UINT_MAX)
      return false;
   if (graph->instrs[i].kind != SPLIT_COMPUTED)
      return true;
   if (lane >= graph->instrs[i].lanes)
      return false;
   util_dynarray_append_typed(&graph->deps, unsigned,
                              graph->instrs[i].first_node +
                              graph->instrs[i].canon[lane]);
   return true;
}

static bool
split_src_free(const struct split_graph *graph, const nir_src *src)
{
   const unsigned i = split_instr_index(graph, nir_def_instr(src->ssa));

   return i != UINT_MAX && graph->instrs[i].kind == SPLIT_FREE;
}

static bool
split_alu_all_free(const struct split_graph *graph, const nir_alu_instr *alu)
{
   for (unsigned s = 0; s < nir_op_infos[alu->op].num_inputs; s++)
      if (!split_src_free(graph, &alu->src[s].src))
         return false;
   return true;
}

/* Lane dependencies of lane c of an ALU result. */
static bool
split_alu_deps(struct split_graph *graph, const nir_alu_instr *alu,
               unsigned c)
{
   const nir_op_info *info = &nir_op_infos[alu->op];

   for (unsigned s = 0; s < info->num_inputs; s++) {
      if (nir_op_is_vec(alu->op)) {
         if (s == c && !split_add_dep(graph, &alu->src[s].src,
                                      alu->src[s].swizzle[0]))
            return false;
         continue;
      }
      if (!info->output_size && !info->input_sizes[s]) {
         if (!split_add_dep(graph, &alu->src[s].src, alu->src[s].swizzle[c]))
            return false;
         continue;
      }
      const unsigned width = info->input_sizes[s] ?
         info->input_sizes[s] : alu->def.num_components;

      for (unsigned l = 0; l < width; l++)
         if (!split_add_dep(graph, &alu->src[s].src, alu->src[s].swizzle[l]))
            return false;
   }
   return true;
}

static bool
split_all_lanes(struct split_graph *graph, const nir_src *src)
{
   for (unsigned l = 0; l < src->ssa->num_components; l++)
      if (!split_add_dep(graph, src, l))
         return false;
   return true;
}

/* Source lanes of equal value: a computed source's canonical lanes, a
 * free source's identical lanes. */
static bool
split_same_lane(const struct split_graph *graph, const nir_src *src,
                unsigned a, unsigned b)
{
   const unsigned i = split_instr_index(graph, nir_def_instr(src->ssa));

   if (a == b)
      return true;
   return i != UINT_MAX && graph->instrs[i].kind == SPLIT_COMPUTED &&
          graph->instrs[i].canon[a] == graph->instrs[i].canon[b];
}

/* Lanes of a per-component op reading equal source lanes are equal. */
static void
split_alu_canon(const struct split_graph *graph, const nir_alu_instr *alu,
                uint8_t canon[4])
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   const bool vec = nir_op_is_vec(alu->op);

   if (info->output_size && !vec)
      return;
   for (unsigned s = 0; !vec && s < info->num_inputs; s++)
      if (info->input_sizes[s])
         return;
   for (unsigned c = 1; c < alu->def.num_components; c++)
      for (unsigned e = 0; e < c; e++) {
         bool same = true;

         if (vec)
            same = alu->src[c].src.ssa == alu->src[e].src.ssa &&
                   split_same_lane(graph, &alu->src[c].src,
                                   alu->src[c].swizzle[0],
                                   alu->src[e].swizzle[0]);
         else
            for (unsigned s = 0; s < info->num_inputs; s++)
               same &= split_same_lane(graph, &alu->src[s].src,
                                       alu->src[s].swizzle[c],
                                       alu->src[s].swizzle[e]);
         if (same) {
            canon[c] = canon[e];
            break;
         }
      }
}

static bool
split_graph_build(struct split_graph *graph, nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_block *block = nir_start_block(impl);
   unsigned count = 0, nodes = 0, sinks = 0;

   memset(graph, 0, sizeof(*graph));
   util_dynarray_init(&graph->deps, NULL);
   nir_foreach_instr(instr, block)
      count++;
   graph->instr_count = count;
   graph->instrs = CALLOC(count, sizeof(*graph->instrs));
   if (!graph->instrs)
      return false;
   count = 0;
   nir_foreach_instr(instr, block) {
      struct split_instr *info = &graph->instrs[count];

      instr->index = count++;
      info->instr = instr;
      info->def = nir_instr_def(instr);
      switch (instr->type) {
      case nir_instr_type_load_const:
      case nir_instr_type_undef:
      case nir_instr_type_deref:
         info->kind = SPLIT_FREE;
         break;
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (!info->def) {
            if (intr->intrinsic != nir_intrinsic_store_deref &&
                intr->intrinsic != nir_intrinsic_terminate &&
                intr->intrinsic != nir_intrinsic_terminate_if)
               return false;
            info->kind = SPLIT_SINK;
            sinks++;
            break;
         }
         /* Loads of inputs, uniforms and system values only. */
         for (unsigned s = 0; s < nir_intrinsic_infos[intr->intrinsic].num_srcs; s++)
            if (!split_src_free(graph, &intr->src[s]))
               return false;
         info->kind = SPLIT_FREE;
         break;
      }
      case nir_instr_type_alu: {
         nir_alu_instr *alu = nir_instr_as_alu(instr);

         if ((alu->op == nir_op_mov || alu->op == nir_op_fneg ||
              nir_op_is_vec(alu->op) || alu->op == nir_op_b2f32) &&
             split_alu_all_free(graph, alu)) {
            info->kind = SPLIT_FREE;
            break;
         }
         info->kind = SPLIT_COMPUTED;
         info->cost = split_alu_cost(alu, &info->lane_cost);
         info->carriable = split_float_or_bool(info->def) &&
            (info->def->bit_size == 1 ||
             nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) ==
                nir_type_float);
         break;
      }
      case nir_instr_type_tex:
         info->kind = SPLIT_COMPUTED;
         info->cost = 1;
         info->carriable = info->def->bit_size == 32;
         break;
      default:
         return false;
      }
      if (info->kind == SPLIT_COMPUTED) {
         if (!info->def || info->def->num_components > 4)
            return false;
         info->lanes = info->def->num_components;
      } else if (info->kind == SPLIT_SINK) {
         info->lanes = 1;
      }
      for (unsigned c = 0; c < 4; c++)
         info->canon[c] = c;
      if (info->kind == SPLIT_COMPUTED && instr->type == nir_instr_type_alu)
         split_alu_canon(graph, nir_instr_as_alu(instr), info->canon);
      info->first_node = nodes;
      nodes += info->lanes;
   }
   if (!sinks)
      return false;
   /* One more node joins every sink: the final job. */
   graph->node_count = nodes + 1;
   graph->final_node = nodes;
   graph->nodes = CALLOC(graph->node_count, sizeof(*graph->nodes));
   graph->node_mark = CALLOC(graph->node_count, sizeof(unsigned));
   graph->instr_mark = CALLOC(graph->instr_count, sizeof(unsigned));
   graph->stack = CALLOC(graph->node_count, sizeof(unsigned));
   if (!graph->nodes || !graph->node_mark || !graph->instr_mark ||
       !graph->stack)
      return false;
   for (unsigned i = 0; i < graph->instr_count; i++) {
      struct split_instr *info = &graph->instrs[i];

      for (unsigned c = 0; c < info->lanes; c++) {
         struct split_node *node = &graph->nodes[info->first_node + c];
         const unsigned first = util_dynarray_num_elements(&graph->deps,
                                                           unsigned);

         node->instr = i;
         node->lane = c;
         node->carried = -1;
         node->alias = info->canon[c] != c;
         node->dep_first = first;
         if (info->instr->type == nir_instr_type_alu) {
            if (!split_alu_deps(graph, nir_instr_as_alu(info->instr), c))
               return false;
         } else if (info->instr->type == nir_instr_type_tex) {
            nir_tex_instr *tex = nir_instr_as_tex(info->instr);

            for (unsigned s = 0; s < tex->num_srcs; s++)
               if (!split_all_lanes(graph, &tex->src[s].src))
                  return false;
         } else {
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(info->instr);

            if (intr->intrinsic == nir_intrinsic_store_deref) {
               const unsigned mask = nir_intrinsic_write_mask(intr);

               for (unsigned l = 0; l < intr->src[1].ssa->num_components; l++)
                  if ((mask & (1u << l)) &&
                      !split_add_dep(graph, &intr->src[1], l))
                     return false;
            } else if (intr->intrinsic == nir_intrinsic_terminate_if &&
                       !split_add_dep(graph, &intr->src[0], 0)) {
               return false;
            }
         }
         node->dep_count = util_dynarray_num_elements(&graph->deps,
                                                      unsigned) - first;
      }
   }
   {
      struct split_node *final = &graph->nodes[graph->final_node];

      final->instr = UINT_MAX;
      final->carried = -1;
      final->dep_first = util_dynarray_num_elements(&graph->deps, unsigned);
      for (unsigned i = 0; i < graph->instr_count; i++)
         if (graph->instrs[i].kind == SPLIT_SINK)
            util_dynarray_append_typed(&graph->deps, unsigned, graph->instrs[i].first_node);
      final->dep_count = util_dynarray_num_elements(&graph->deps, unsigned) -
                         final->dep_first;
   }
   return true;
}

/* Cone of a node: records to compute it and the carried lanes it reads.
 * The lanes of instruction extra count as carried too (UINT_MAX: none). */
static void
split_cone(struct split_graph *graph, unsigned root, unsigned extra,
           struct split_cone *cone, bool members)
{
   const unsigned *deps = util_dynarray_begin(&graph->deps);
   unsigned top = 0;

   graph->generation++;
   cone->cost = 0;
   util_dynarray_clear(&cone->reads);
   util_dynarray_clear(&cone->members);
   graph->stack[top++] = root;
   graph->node_mark[root] = graph->generation;
   while (top) {
      const unsigned n = graph->stack[--top];
      const struct split_node *node = &graph->nodes[n];

      if (node->instr != UINT_MAX) {
         const struct split_instr *info = &graph->instrs[node->instr];

         if (info->lane_cost)
            cone->cost += info->cost;
         else if (graph->instr_mark[node->instr] != graph->generation)
            cone->cost += info->cost;
         graph->instr_mark[node->instr] = graph->generation;
      }
      if (members && n != root)
         util_dynarray_append_typed(&cone->members, unsigned, n);
      for (unsigned d = 0; d < node->dep_count; d++) {
         const unsigned dep = deps[node->dep_first + d];

         if (graph->node_mark[dep] == graph->generation)
            continue;
         graph->node_mark[dep] = graph->generation;
         if (graph->nodes[dep].carried >= 0 ||
             graph->nodes[dep].instr == extra)
            util_dynarray_append_typed(&cone->reads, unsigned, dep);
         else
            graph->stack[top++] = dep;
      }
   }
}

static int
split_compare_unsigned(const void *a, const void *b)
{
   const unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;

   return x < y ? -1 : x > y;
}

struct split_plan {
   /* Carried nodes in program order, then the final node. */
   struct util_dynarray jobs;
};

static bool
split_plan(struct split_graph *graph, unsigned budget, unsigned samplers,
           struct split_plan *plan)
{
   struct split_cone cone, trial;
   bool ok = true;

   util_dynarray_init(&cone.reads, NULL);
   util_dynarray_init(&cone.members, NULL);
   util_dynarray_init(&trial.reads, NULL);
   util_dynarray_init(&trial.members, NULL);
   util_dynarray_clear(&plan->jobs);
   for (unsigned n = 0; n < graph->node_count; n++) {
      graph->nodes[n].carried = -1;
      graph->nodes[n].cone_cost = 0;
   }
   for (unsigned n = 0; n < graph->node_count && ok; n++) {
      const unsigned limit = MIN2(budget, graph->nodes[n].budget);

      if (graph->nodes[n].alias)
         continue;
      for (;;) {
         unsigned reads, best = UINT_MAX, best_cost = 0, best_reads = 0;

         split_cone(graph, n, UINT_MAX, &cone, true);
         reads = util_dynarray_num_elements(&cone.reads, unsigned);
         if (cone.cost <= limit && reads <= samplers)
            break;
         /* Candidates are instructions: all their lanes in the cone are
          * carried together, scored by records saved per carried lane. */
         const unsigned member_count =
            util_dynarray_num_elements(&cone.members, unsigned);
         const unsigned *members = util_dynarray_begin(&cone.members);
         unsigned tried[24], tried_count = 0, best_lanes = 1;

         while (tried_count < ARRAY_SIZE(tried)) {
            unsigned pick = UINT_MAX, pick_cost = 0, lanes = 0;

            for (unsigned m = 0; m < member_count; m++) {
               const struct split_node *node = &graph->nodes[members[m]];
               bool seen = false;

               for (unsigned t = 0; t < tried_count; t++)
                  seen |= tried[t] == node->instr;
               if (seen || node->instr == UINT_MAX ||
                   !graph->instrs[node->instr].carriable || !node->cone_cost)
                  continue;
               if (pick == UINT_MAX || node->cone_cost > pick_cost) {
                  pick = node->instr;
                  pick_cost = node->cone_cost;
               }
            }
            if (pick == UINT_MAX)
               break;
            for (unsigned m = 0; m < member_count; m++)
               lanes += graph->nodes[members[m]].instr == pick;
            tried[tried_count++] = pick;
            {
               unsigned t_reads, saved;
               bool better;

               split_cone(graph, n, pick, &trial, false);
               t_reads = util_dynarray_num_elements(&trial.reads, unsigned);
               saved = cone.cost - trial.cost;
               if (reads > samplers)
                  better = best == UINT_MAX || t_reads < best_reads ||
                           (t_reads == best_reads &&
                            saved * best_lanes > best_cost * lanes);
               else
                  better = best == UINT_MAX ||
                           saved * best_lanes > best_cost * lanes ||
                           (saved * best_lanes == best_cost * lanes &&
                            t_reads < best_reads);
               if (better) {
                  best = pick;
                  best_cost = saved;
                  best_reads = t_reads;
                  best_lanes = lanes;
               }
            }
         }
         if (best == UINT_MAX || (!best_cost && best_reads >= reads)) {
            ok = false;
            break;
         }
         for (unsigned m = 0; m < member_count; m++)
            if (graph->nodes[members[m]].instr == best &&
                graph->nodes[members[m]].carried < 0) {
               graph->nodes[members[m]].carried = 0;
               util_dynarray_append_typed(&plan->jobs, unsigned, members[m]);
            }
      }
      graph->nodes[n].cone_cost = cone.cost;
   }
   util_dynarray_fini(&cone.reads);
   util_dynarray_fini(&cone.members);
   util_dynarray_fini(&trial.reads);
   util_dynarray_fini(&trial.members);
   if (!ok)
      return false;
   /* Program order is a topological order of the jobs. */
   qsort(util_dynarray_begin(&plan->jobs),
         util_dynarray_num_elements(&plan->jobs, unsigned), sizeof(unsigned),
         split_compare_unsigned);
   return true;
}

static nir_variable *
split_color_output(nir_shader *nir)
{
   nir_foreach_shader_out_variable(var, nir)
      if ((var->data.location == FRAG_RESULT_COLOR ||
           var->data.location == FRAG_RESULT_DATA0) &&
          glsl_get_components(var->type) == 4 &&
          glsl_get_base_type(var->type) == GLSL_TYPE_FLOAT)
         return var;
   nir_variable *var = nir_variable_create(nir, nir_var_shader_out,
                                           glsl_vec4_type(), "chrome9_carry");
   var->data.location = FRAG_RESULT_DATA0;
   return var;
}

/* R32F carrier read at this pixel: returns lane x. */
static nir_def *
split_carrier_read(nir_builder *b, nir_def *coord, unsigned unit)
{
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 1);

   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_float32;
   tex->coord_components = 2;
   tex->texture_index = unit;
   tex->sampler_index = unit;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   return nir_channel(b, &tex->def, 0);
}

/* The NIR of one job: carried lanes become carrier reads, and an
 * intermediate job stores its lane to colour output 0 instead of the
 * program's outputs. */
static nir_shader *
split_job_nir(const struct split_graph *graph, nir_shader *base,
              unsigned root, const unsigned *reads, unsigned read_count,
              unsigned carrier_base, unsigned inverse_register)
{
   nir_shader *nir = nir_shader_clone(NULL, base);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_instr **map = MALLOC(graph->instr_count * sizeof(*map));
   nir_def *coord = NULL;
   unsigned count = 0;
   nir_builder b;

   if (!nir || !map)
      goto fail;
   nir_foreach_instr(instr, nir_start_block(impl)) {
      if (count >= graph->instr_count)
         goto fail;
      map[count++] = instr;
   }
   if (count != graph->instr_count)
      goto fail;
   b = nir_builder_at(nir_before_impl(impl));
   if (read_count) {
      nir_def *position = nir_load_frag_coord(&b);
      nir_def *inverse = nir_load_ubo_vec4(&b, 4, 32, nir_imm_int(&b, 0),
                                           nir_imm_int(&b, 0),
                                           .base = inverse_register);

      coord = nir_ffma(&b, nir_channels(&b, position, 0x3),
                       nir_channels(&b, inverse, 0x3),
                       nir_channels(&b, inverse, 0xc));
   }
   for (unsigned r = 0; r < read_count; r++) {
      const struct split_node *node = &graph->nodes[reads[r]];
      nir_def *def;
      nir_def *lanes[4];
      bool first = true;

      /* One replacement per instruction, covering all its carried lanes. */
      for (unsigned e = 0; e < r; e++)
         if (graph->nodes[reads[e]].instr == node->instr)
            first = false;
      if (!first)
         continue;
      def = nir_instr_def(map[node->instr]);
      b.cursor = nir_after_instr(map[node->instr]);
      for (unsigned l = 0; l < def->num_components; l++)
         lanes[l] = nir_channel(&b, def, l);
      for (unsigned e = r; e < read_count; e++) {
         const struct split_node *read = &graph->nodes[reads[e]];
         nir_def *value;

         if (read->instr != node->instr)
            continue;
         value = split_carrier_read(&b, coord, carrier_base + e);
         if (def->bit_size == 1)
            value = nir_fneu_imm(&b, value, 0.0);
         for (unsigned l = 0; l < def->num_components; l++)
            if (graph->instrs[read->instr].canon[l] == read->lane)
               lanes[l] = value;
      }
      nir_def *vector = nir_vec(&b, lanes, def->num_components);

      nir_def_rewrite_uses_after(def, vector);
   }
   if (root != graph->final_node) {
      const struct split_node *node = &graph->nodes[root];
      nir_def *def = nir_instr_def(map[node->instr]);
      nir_def *value;

      for (unsigned i = 0; i < graph->instr_count; i++)
         if (graph->instrs[i].kind == SPLIT_SINK)
            nir_instr_remove(map[i]);
      b = nir_builder_at(nir_after_impl(impl));
      value = nir_channel(&b, def, node->lane);
      if (value->bit_size == 1)
         value = nir_b2f32(&b, value);
      nir_store_deref(&b, nir_build_deref_var(&b, split_color_output(nir)),
                      nir_replicate(&b, value, 4), 0xf);
   }
   FREE(map);
   /* Unused lanes of vectors keep whole cones alive: trim to a fixpoint. */
   for (bool progress = true; progress;) {
      progress = nir_opt_shrink_vectors(nir, true);
      progress |= nir_opt_copy_prop(nir);
      progress |= nir_opt_dce(nir);
   }
   nir_shader_gather_info(nir, impl);
   return nir;

fail:
   FREE(map);
   ralloc_free(nir);
   return NULL;
}

/* Native records and TAU entries as the kernel counts them. */
static void
split_ir_size(const struct chrome9_ps_ir *ir, unsigned *alu, unsigned *tau)
{
   *alu = *tau = 0;
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction = &ir->instruction[i];

      if (instruction->op == CHROME9_PS_OPCODE_TEX) {
         (*tau)++;
         /* A computed coordinate takes a bridge MOV. */
         if (instruction->source[0].file == CHROME9_PS_SOURCE_TEMP)
            (*alu)++;
      } else {
         (*alu)++;
         if (instruction->op == CHROME9_PS_OPCODE_KILL)
            (*tau)++;
      }
   }
   if (ir->depth_output)
      (*alu)++;
}

void
chrome9_fs_split_destroy(struct chrome9_fs_split *split)
{
   if (!split)
      return;
   for (unsigned j = 0; j < split->job_count; j++) {
      FREE(split->jobs[j].program);
      FREE(split->jobs[j].reads);
   }
   FREE(split->jobs);
   FREE(split);
}

/* Registers read by load_ubo_vec4 and samplers read by TEX. */
static bool
split_scan_resources(nir_shader *nir, uint32_t *uniform_mask,
                     uint8_t *sampler_mask)
{
   *uniform_mask = 0;
   *sampler_mask = 0;
   nir_foreach_instr(instr, nir_start_block(nir_shader_get_entrypoint(nir))) {
      if (instr->type == nir_instr_type_tex) {
         nir_tex_instr *tex = nir_instr_as_tex(instr);

         if (tex->sampler_index >= 8)
            return false;
         *sampler_mask |= 1u << tex->sampler_index;
      } else if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         unsigned reg;

         if (intr->intrinsic != nir_intrinsic_load_ubo_vec4)
            continue;
         if (!nir_src_is_const(intr->src[1]))
            return false;
         reg = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1]);
         if (reg >= 32)
            return false;
         *uniform_mask |= 1u << reg;
      }
   }
   return true;
}

/* Carrier slots: a job's output never shares a slot with its reads. */
static bool
split_assign_carriers(struct chrome9_fs_split *split, unsigned carried)
{
   unsigned *last = CALLOC(MAX2(carried, 1), sizeof(unsigned));
   int *slot_of = MALLOC(MAX2(carried, 1) * sizeof(int));
   bool *busy = CALLOC(MAX2(carried, 1), sizeof(bool));
   bool ok = last && slot_of && busy;

   for (unsigned c = 0; ok && c < carried; c++) {
      last[c] = c;
      slot_of[c] = -1;
   }
   for (unsigned j = 0; ok && j < split->job_count; j++)
      for (unsigned r = 0; r < split->jobs[j].read_count; r++)
         last[split->jobs[j].reads[r]] = MAX2(last[split->jobs[j].reads[r]], j);
   split->carrier_count = 0;
   for (unsigned j = 0; ok && j < split->job_count; j++) {
      struct chrome9_fs_split_job *job = &split->jobs[j];

      if (j < carried) {
         unsigned slot = 0;

         while (busy[slot])
            slot++;
         busy[slot] = true;
         slot_of[j] = slot;
         job->carrier = slot;
         split->carrier_count = MAX2(split->carrier_count, slot + 1);
      }
      for (unsigned r = 0; r < job->read_count; r++)
         job->reads[r] = slot_of[job->reads[r]];
      for (unsigned c = 0; c < carried && c <= j; c++)
         if (slot_of[c] >= 0 && last[c] == j) {
            busy[slot_of[c]] = false;
            slot_of[c] = -1 - slot_of[c];
         }
   }
   FREE(last);
   FREE(slot_of);
   FREE(busy);
   return ok;
}

/* Distinct job compiles one split may try. */
#define SPLIT_MAX_COMPILES 64

/* A compiled job, keyed by its root and the carried nodes it reads. */
struct split_cached {
   unsigned root;
   unsigned read_count;
   unsigned *reads;
   struct chrome9_shader *program;
   /* Records of the compiled job, zero when the backend refused it. */
   unsigned alu;
};

static struct split_cached *
split_cache_find(struct util_dynarray *cache, unsigned root,
                 const unsigned *reads, unsigned read_count)
{
   util_dynarray_foreach(cache, struct split_cached, entry)
      if (entry->root == root && entry->read_count == read_count &&
          !memcmp(entry->reads, reads, read_count * sizeof(*reads)))
         return entry;
   return NULL;
}

static void
split_cache_free(struct util_dynarray *cache)
{
   util_dynarray_foreach(cache, struct split_cached, entry) {
      FREE(entry->reads);
      FREE(entry->program);
   }
   util_dynarray_fini(cache);
}

/* The program of one job, compiled once per root and read set. */
static struct chrome9_shader *
split_job_program(struct chrome9_context *ctx,
                  const struct chrome9_shader *parent,
                  const struct split_graph *graph, nir_shader *base,
                  const struct chrome9_fs_split *split,
                  struct util_dynarray *cache, unsigned root,
                  const unsigned *reads, unsigned read_count,
                  unsigned *records)
{
   struct split_cached *entry = split_cache_find(cache, root, reads,
                                                 read_count);
   struct split_cached added = { root, read_count, NULL, NULL, 0 };
   nir_shader *nir;
   unsigned alu = 0, tau = 0;

   if (entry) {
      *records = entry->alu;
      return entry->program;
   }
   added.reads = MALLOC(MAX2(read_count, 1) * sizeof(unsigned));
   added.program = CALLOC_STRUCT(chrome9_shader);
   if (!added.reads || !added.program) {
      FREE(added.reads);
      FREE(added.program);
      return NULL;
   }
   memcpy(added.reads, reads, read_count * sizeof(unsigned));
   for (unsigned r = 0; r < read_count; r++) {
      const struct split_node *node = &graph->nodes[reads[r]];
      bool finite, uninterpolated;

      chrome9_fs_split_lane_proof(graph->instrs[node->instr].def, node->lane,
                                  &finite, &uninterpolated);
      if (!finite)
         added.program->split_unproven_finite |=
            BITFIELD_BIT(split->carrier_base + r);
      if (!uninterpolated)
         added.program->split_interpolated |=
            BITFIELD_BIT(split->carrier_base + r);
   }
   const int64_t t0 = os_time_get_nano();
   nir = split_job_nir(graph, base, root, reads, read_count,
                       split->carrier_base, split->inverse_register);
   const int64_t t1 = os_time_get_nano();
   bool compiled = nir &&
      chrome9_fs_split_compile_job(ctx, parent, added.program, nir);
   if (split_debug() > 2)
      fprintf(stderr, "chrome9: split node %u: nir %.1f ms compile %.1f ms\n",
              root, (t1 - t0) / 1e6, (os_time_get_nano() - t1) / 1e6);
   if (!compiled) {
      if (nir && split_debug() > 1) {
         struct split_graph *g = (struct split_graph *)graph;
         struct split_cone cone;

         util_dynarray_init(&cone.reads, NULL);
         util_dynarray_init(&cone.members, NULL);
         split_cone(g, root, UINT_MAX, &cone, true);
         fprintf(stderr, "chrome9: split cone of node %u cost %u:", root,
                 cone.cost);
         util_dynarray_foreach(&cone.members, unsigned, m)
            fprintf(stderr, " %%%u.%c", g->instrs[g->nodes[*m].instr].def->index,
                    "xyzw"[g->nodes[*m].lane]);
         fprintf(stderr, "\nchrome9: split reads:");
         util_dynarray_foreach(&cone.reads, unsigned, m)
            fprintf(stderr, " %%%u.%c", g->instrs[g->nodes[*m].instr].def->index,
                    "xyzw"[g->nodes[*m].lane]);
         fprintf(stderr, "\n");
         util_dynarray_fini(&cone.reads);
         util_dynarray_fini(&cone.members);
         nir_print_shader(nir, stderr);
      }
      FREE(added.program);
      added.program = NULL;
   }
   ralloc_free(nir);
   if (added.program) {
      split_ir_size(&added.program->semantic_fs_ir, &alu, &tau);
      added.alu = alu;
      /* Application samplers stay put; carriers only at their units. */
      if (alu > CHROME9_PS_MAX_ALU || tau > CHROME9_PS_MAX_TAU ||
          (added.program->semantic_sampler_mask &
           ~(split->app_sampler_mask |
             (BITFIELD_MASK(read_count) << split->carrier_base)))) {
         FREE(added.program);
         added.program = NULL;
      }
   }
   if (split_debug())
      fprintf(stderr, "chrome9: split node %u estimate %u reads %u: %s "
              "alu %u tau %u\n", root, root == graph->final_node ? 0 :
              graph->nodes[root].cone_cost, read_count,
              added.program ? "compiled" : "does not fit",
              added.program ? alu : 0, added.program ? tau : 0);
   util_dynarray_append(cache, added);
   *records = added.alu;
   return added.program;
}

/* Jobs of one plan; the root of a job that does not fit and its records
 * (zero: refused by the backend), else UINT_MAX. */
static unsigned
split_build_jobs(struct chrome9_context *ctx,
                 const struct chrome9_shader *parent,
                 struct split_graph *graph, nir_shader *base,
                 const struct split_plan *plan, unsigned samplers,
                 struct util_dynarray *cache, struct chrome9_fs_split *split,
                 unsigned *failed_records)
{
   const unsigned carried = util_dynarray_num_elements(&plan->jobs, unsigned);
   const unsigned *carried_nodes = util_dynarray_begin(&plan->jobs);
   struct split_cone cone;
   unsigned failed = UINT_MAX;

   *failed_records = 0;
   util_dynarray_init(&cone.reads, NULL);
   util_dynarray_init(&cone.members, NULL);
   split->job_count = carried + 1;
   split->jobs = CALLOC(split->job_count, sizeof(*split->jobs));
   for (unsigned c = 0; c < carried; c++)
      graph->nodes[carried_nodes[c]].carried = c;
   for (unsigned j = 0; split->jobs && j < split->job_count; j++) {
      struct chrome9_fs_split_job *job = &split->jobs[j];
      const unsigned root = j < carried ? carried_nodes[j] : graph->final_node;
      unsigned read_count, *reads;

      job->carrier = -1;
      split_cone(graph, root, UINT_MAX, &cone, false);
      read_count = util_dynarray_num_elements(&cone.reads, unsigned);
      reads = util_dynarray_begin(&cone.reads);
      qsort(reads, read_count, sizeof(unsigned), split_compare_unsigned);
      job->read_count = read_count;
      job->reads = CALLOC(MAX2(read_count, 1), sizeof(unsigned));
      if (read_count > samplers || !job->reads) {
         failed = root;
         break;
      }
      job->program = split_job_program(ctx, parent, graph, base, split, cache,
                                       root, reads, read_count,
                                       failed_records);
      if (!job->program) {
         failed = root;
         break;
      }
      for (unsigned r = 0; r < read_count; r++)
         job->reads[r] = graph->nodes[reads[r]].carried;
   }
   util_dynarray_fini(&cone.reads);
   util_dynarray_fini(&cone.members);
   if (!split->jobs)
      return graph->final_node;
   if (failed == UINT_MAX && !split_assign_carriers(split, carried))
      failed = graph->final_node;
   return failed;
}

/* Drop the plan's jobs; programs stay in the cache. */
static void
split_reset_jobs(struct chrome9_fs_split *split)
{
   for (unsigned j = 0; split->jobs && j < split->job_count; j++)
      FREE(split->jobs[j].reads);
   FREE(split->jobs);
   split->jobs = NULL;
   split->job_count = 0;
}

struct chrome9_fs_split *
chrome9_fs_split_create(struct chrome9_context *ctx,
                        const struct chrome9_shader *parent,
                        struct nir_shader *source)
{
   struct chrome9_fs_split *split = CALLOC_STRUCT(chrome9_fs_split);
   nir_shader *base = nir_shader_clone(NULL, source);
   struct util_dynarray cache;
   struct split_graph graph;
   struct split_plan plan;
   uint32_t uniform_mask;
   /* CHROME9_FS_SPLIT_BUDGET: estimated records per job (debug). */
   const unsigned budget = debug_get_num_option("CHROME9_FS_SPLIT_BUDGET", 84);
   const int64_t start = os_time_get_nano();
   int64_t plan_ns = 0;
   unsigned samplers, attempts = 0;
   bool done = false;

   util_dynarray_init(&plan.jobs, NULL);
   util_dynarray_init(&cache, NULL);
   memset(&graph, 0, sizeof(graph));
   if (!split || !base)
      goto fail;
   chrome9_fs_prepare_semantic_nir(base);
   nir_index_ssa_defs(nir_shader_get_entrypoint(base));
   if (split_debug() > 1)
      nir_print_shader(base, stderr);
   if (!split_scan_resources(base, &uniform_mask, &split->app_sampler_mask) ||
       !split_graph_build(&graph, base))
      goto fail;
   split->carrier_base = util_last_bit(split->app_sampler_mask);
   split->inverse_register = util_last_bit(uniform_mask);
   if (split->carrier_base >= 8 || split->inverse_register >= 32)
      goto fail;
   samplers = 8 - split->carrier_base;
   for (unsigned n = 0; n < graph.node_count; n++)
      graph.nodes[n].budget = CHROME9_PS_MAX_ALU;
   /* A job over the store lowers its own budget and the plan is redone;
    * the other jobs come from the cache. */
   while (!done && attempts++ < 32) {
      unsigned failed;

      const int64_t plan_start = os_time_get_nano();

      if (!split_plan(&graph, budget, samplers, &plan))
         break;
      plan_ns += os_time_get_nano() - plan_start;
      unsigned records, cost;

      failed = split_build_jobs(ctx, parent, &graph, base, &plan, samplers,
                                &cache, split, &records);
      if (failed == UINT_MAX) {
         done = true;
         break;
      }
      split_reset_jobs(split);
      cost = MIN2(graph.nodes[failed].budget, graph.nodes[failed].cone_cost);
      /* A small job that does not compile holds an unsupported operation:
       * shrinking it further cannot help.  Compiling is the expensive part,
       * so the number of distinct jobs is bounded too. */
      if (graph.nodes[failed].cone_cost < 32 ||
          util_dynarray_num_elements(&cache, struct split_cached) >
             SPLIT_MAX_COMPILES)
         break;
      /* Scale the budget by the measured overshoot, or by 3/5 when the
       * backend stopped at its own limit. */
      graph.nodes[failed].budget = records > CHROME9_PS_MAX_ALU ?
         MIN2(cost - 1, cost * (CHROME9_PS_MAX_ALU - 4) / records) :
         cost * 3 / 5;
   }
   if (split_debug())
      fprintf(stderr, "chrome9: split %s after %u plans: %u jobs, "
              "%u carriers, %u nodes, %.1f ms planning, %.1f ms total\n",
              done ? "done" : "failed", attempts, split->job_count,
              split->carrier_count, graph.node_count, plan_ns / 1e6,
              (os_time_get_nano() - start) / 1e6);
   if (!done)
      goto fail;
   /* The jobs own their programs now. */
   util_dynarray_foreach(&cache, struct split_cached, entry)
      for (unsigned j = 0; j < split->job_count; j++)
         if (split->jobs[j].program == entry->program)
            entry->program = NULL;
   split_cache_free(&cache);
   split_graph_free(&graph);
   util_dynarray_fini(&plan.jobs);
   ralloc_free(base);
   return split;

fail:
   split_cache_free(&cache);
   split_graph_free(&graph);
   util_dynarray_fini(&plan.jobs);
   ralloc_free(base);
   chrome9_fs_split_destroy(split);
   return NULL;
}
