/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_CONTEXT_H
#define CHROME9_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "compiler/shader_info.h"
#include "util/list.h"
#include "util/u_dynarray.h"

#include "chrome9_shader_offline.h"
#include "chrome9_tcl.h"
#include "chrome9_vs_texture.h"

struct chrome9_fs_split;
struct chrome9_query;
struct chrome9_query_interval;
struct draw_context;
struct nir_shader;

enum chrome9_warning {
   CHROME9_WARN_SUBMIT = 1u << 0,
   CHROME9_WARN_DRAW_STATE = 1u << 1,
   CHROME9_WARN_INDIRECT = 1u << 2,
   CHROME9_WARN_CLEAR = 1u << 3,
   CHROME9_WARN_FRAMEBUFFER = 1u << 4,
   CHROME9_WARN_SAMPLER_VIEW_CREATE = 1u << 5,
   CHROME9_WARN_SAMPLER_VIEW_BIND = 1u << 6,
};

enum chrome9_fragment_program {
   CHROME9_FS_UNSUPPORTED,
   CHROME9_FS_PRIMARY_COLOR,
   CHROME9_FS_PRIMARY_GENERIC_RGBA,
   CHROME9_FS_PRIMARY_GENERIC_RGBX,
   CHROME9_FS_PRIMARY_GENERIC_XXY_RGBX,
   CHROME9_FS_PRIMARY_SCALAR_RGBX,
   CHROME9_FS_PRIMARY_FLAT_SCALAR_RGBX,
   CHROME9_FS_PRIMARY_SCALAR_BLUE_RGBX,
   CHROME9_FS_LITERAL_COLOR,
   CHROME9_FS_CONSTANT_COLOR,
   CHROME9_FS_CONSTANT_MODULATE,
   CHROME9_FS_CONSTANT_MASK_ALPHA_MODULATE,
   CHROME9_FS_CONSTANT_SOURCE_ALPHA_MODULATE,
   CHROME9_FS_TEXTURE_REPLACE,
   CHROME9_FS_TEXTURE_RGBX,
   CHROME9_FS_TEXTURE_MODULATE,
   CHROME9_FS_TEXTURE_PROJECTED_MODULATE,
   CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_MODULATE,
   CHROME9_FS_TEXTURE_PROJECTED_CONSTANT_RGB_ALPHA,
   CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA,
   CHROME9_FS_TEXTURE_PACKED_GRAY_MODULATE,
   CHROME9_FS_TEXTURE_CONSTANT_MODULATE,
   CHROME9_FS_TEXTURE_CONSTANT_ALPHA_MODULATE,
   CHROME9_FS_TWO_TEXTURE_MODULATE,
   CHROME9_FS_TWO_PROJECTED_TEXTURE_PRIMARY_RGB_ALPHA,
   CHROME9_FS_TWO_PROJECTED_TEXTURE0_ALPHA_PRIMARY,
   CHROME9_FS_IOQUAKE3_GENERIC_PACKED,
   CHROME9_FS_IOQUAKE3_LIGHTMAP_PACKED,
   /* Generic one-instruction NIR backend result, not an app-shaped key. */
   CHROME9_FS_COMPILED_ALU,
};

/*
 * Uniform-only subexpressions of a fragment program, evaluated per draw on
 * the CPU into constant registers (a preshader).  Nodes are in dependency
 * order; a leaf reads CB0 dwords (word >= 0) or holds literals.
 */
struct chrome9_preshader_node {
   uint16_t op;
   uint8_t components;
   uint8_t bit_size;
   uint16_t src[3];
   uint8_t swizzle[3][4];
   int32_t word;
   uint32_t value[4];
};

struct chrome9_preshader_root {
   uint16_t node;
   uint8_t reg;
   uint8_t lane;
};

struct chrome9_preshader {
   struct chrome9_preshader_node *node;
   unsigned node_count;
   struct chrome9_preshader_root *root;
   unsigned root_count;
   /* Constant registers written by the roots. */
   uint32_t reg_mask;
};

struct chrome9_fs_variant {
   struct chrome9_fs_variant *next;
   uint32_t inline_values[MAX_INLINABLE_UNIFORMS];
   /* 0: dynamic FACE, 1: surviving front face, 2: surviving back face. */
   uint8_t face_mode;
   /* chrome9_shadow_key() of every shadow sampler, zero otherwise. */
   uint16_t shadow_keys[PIPE_MAX_SAMPLERS];
   /* chrome9_sprite_flip_key() of the draw. */
   uint32_t sprite_flip;
   enum chrome9_fragment_program fragment_program;
   uint32_t literal_color;
   struct chrome9_fs_key native_fs_key;
   struct chrome9_ps_ir semantic_fs_ir;
   struct chrome9_ps_program native_fs;
   uint32_t semantic_constant_mask;
   uint32_t semantic_uniform_mask;
   uint16_t semantic_input_mask;
   uint16_t semantic_varying_mask;
   uint8_t semantic_sampler_mask;
   uint8_t semantic_cube_sampler_mask;
   uint8_t semantic_volume_sampler_mask;
   /* Direct varying lanes used by equality-to-zero selects.  The draw path
    * proves exact signed zero or unit-magnitude values before submission. */
   uint32_t semantic_equal_zero_input_mask;
   /* Projected samples lowered to 2D TEX only when Q is exactly one. */
   uint32_t semantic_identity_projector_input_mask;
   /* Direct varying lanes whose complete post-VS draw is proven finite and
    * within [0, 1] before a range-dependent fragment lowering is submitted. */
   uint32_t semantic_unit_interval_input_mask;
   bool semantic_fs_valid;
   bool semantic_passthrough;
   bool native_fs_valid;
   bool native_fs_from_nir;
   bool flat_primary;
   bool flat_generic;
   bool supported;
   /* Multi-pass form of a program beyond the store (owned). */
   struct chrome9_fs_split *split;
   /* Owned. */
   struct chrome9_preshader *preshader;
};

struct chrome9_tcl_shader;
struct chrome9_tcl_state;
struct chrome9_sw_context;
struct nir_shader;

struct chrome9_shader {
   void *draw;
   void *nir;
   /* Hardware TCL form of a vertex shader, compiled on first use. */
   struct chrome9_tcl_shader *tcl;
   /* NIR before hardware-only lowering, kept for the llvmpipe fallback. */
   struct nir_shader *sw_nir;
   /* Fragment shaders: the TEX or VAR varying slot of fragment varying n,
    * zero when unused (chrome9_fs_assign_varyings). */
   uint8_t varying_slot[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
   /* Fragment samplers read by shadow TEX; variants lower the compare. */
   uint32_t shadow_sampler_mask;
   struct chrome9_fs_variant *variants;
   /* The canonical compile of a canonical_supported shader, and the
    * variant whose program the shader fields hold. */
   const struct chrome9_fs_variant *canonical;
   const struct chrome9_fs_variant *applied;
   unsigned variant_count;
   enum chrome9_fragment_program fragment_program;
   uint32_t literal_color;
   struct chrome9_fs_key native_fs_key;
   struct chrome9_ps_ir semantic_fs_ir;
   struct chrome9_ps_program native_fs;
   uint32_t semantic_constant_mask;
   uint32_t semantic_uniform_mask;
   uint16_t semantic_input_mask;
   uint16_t semantic_varying_mask;
   uint8_t semantic_sampler_mask;
   /* Samplers read by a cube TEX (three coordinate lanes). */
   uint8_t semantic_cube_sampler_mask;
   uint8_t semantic_volume_sampler_mask;
   uint32_t semantic_equal_zero_input_mask;
   uint32_t semantic_identity_projector_input_mask;
   uint32_t semantic_unit_interval_input_mask;
   bool semantic_fs_valid;
   /* Synthesized MOV oC0, primary: used by hardware TCL only, the
    * post-TNL path keeps its faster fixed-function route. */
   bool semantic_passthrough;
   bool native_fs_valid;
   bool native_fs_from_nir;
   bool flat_primary;
   bool flat_generic;
   bool canonical_supported;
   bool supported;
   /* The variant's multi-pass form, and set on its jobs: never split. */
   struct chrome9_fs_split *split;
   bool split_job;
   /* The applied variant's preshader, or the compile's before capture. */
   struct chrome9_preshader *preshader;
   /* Jobs: carrier samplers without the finite or uninterpolated proof. */
   uint8_t split_unproven_finite;
   uint8_t split_interpolated;
};

struct chrome9_sampler_state {
   struct pipe_sampler_state base;
   uint8_t min_filter;
   uint8_t mag_filter;
   uint8_t mip_filter;
   uint8_t wrap_s;
   uint8_t wrap_t;
   /* Zero when R wrapping has no native mode; only 3D reads it. */
   uint8_t wrap_r;
   /* ARGB8888, set when an axis wraps to the border. */
   uint32_t border_color;
   /* Whole-level LOD limits (chrome9_lod_level). */
   float min_level;
   float max_level;
   bool supported;
};

struct chrome9_sampler_view {
   struct pipe_sampler_view base;
   /* Unguarded external images remain the Gallium-visible texture.  The
    * hardware samples this guarded, draw-refreshed copy instead. */
   struct pipe_resource *shadow;
   /* Only llvmpipe can sample this view. */
   bool sw_only;
};

static inline struct chrome9_sampler_view *
chrome9_sampler_view(struct pipe_sampler_view *view)
{
   return (struct chrome9_sampler_view *)view;
}

static inline struct pipe_resource *
chrome9_sampler_view_resource(struct pipe_sampler_view *view)
{
   struct chrome9_sampler_view *cview = chrome9_sampler_view(view);

   return cview->shadow ? cview->shadow : view->texture;
}

struct chrome9_vertex_elements {
   unsigned count;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

/* A pipe fence: completes when resource, the context's last submitted
 * target, is idle. */
struct chrome9_fence {
   struct pipe_reference reference;
   struct pipe_resource *resource;
   /* Engine sequence number of the last job before the flush. */
   uint64_t seqno;
   bool has_seqno;
};

struct chrome9_context {
   struct pipe_context base;
   struct draw_context *draw;
   void *vbuf_render;

   struct pipe_framebuffer_state framebuffer;
   /* DRAW_TRIANGLE requires a native guarded BO.  Imported DRI3 images use
    * this guarded shadow and are copied back on flush. */
   struct pipe_resource *render_target;
   /* A depth-only public framebuffer still needs a guarded native A50
    * destination.  This private target is never a Gallium color buffer. */
   struct pipe_resource *depth_only_scratch;
   /* Guarded render shadows of unguarded drawables, keyed by drawable. */
   struct hash_table *shadows;
   bool render_target_dirty;
   /* Target of the most recent submission, for fences. */
   struct pipe_resource *last_target;
   /* Pixels written since the last publish, when render_target_dirty. */
   struct pipe_scissor_state render_dirty;
   bool render_target_stale;
   bool draw_triangle_d16;
   bool draw_triangle_d16_blend;
   bool depth_func_supported;
   bool depth_equal_supported;
   bool depth_always_supported;
   bool depth_read_only_supported;
   bool draw_triangle_batch;
   bool draw_triangle_d16_rhw_batch;
   bool draw_triangle_d16_rhw_run;
   bool draw_triangle_d16_rhw_chain;
   bool draw_program_triangle_d16_rhw_batch;
   bool draw_fragment_program;
   bool fragment_sparse_textures;
   /* R32F carriers of multi-pass fragment programs (pipe_resource *),
    * all split_carrier_width x split_carrier_height. */
   struct util_dynarray split_carriers;
   unsigned split_carrier_width;
   unsigned split_carrier_height;
   bool fragment_primary;
   bool fragment_kill;
   bool texture_bias;
   bool fragment_add;
   bool draw_textured_triangle;
   bool draw_textured_triangle_batch;
   bool draw_textured_triangle_run;
   bool draw_textured_triangle_mip;
   bool copy_rect;
   bool copy_rect_unguarded_destination;
   bool copy_rect_preserve_alpha;
   bool draw_textured_triangle_d16;
   bool draw_textured_triangle_d16_blend;
   bool textured_d16_bgra8888;
   bool textured_d16_source_over;
   bool draw_two_textured_triangle;
   bool draw_two_textured_triangle_d16;
   bool draw_two_textured_triangle_d16_blend;
   bool two_texture_tex0_alpha_primary;
   bool two_texture_signed_repeat_coords;
   bool draw_triangle_blend;
   bool draw_textured_triangle_blend;
   bool blend_bgra8888;
   bool blend_extended_factors;
   bool blend_equation;
   bool blend_color_factors;
   bool alpha_test_supported;
   bool scissor_supported;
   bool fragment_stencil;
   /* DRM_CHROME9_CAP2_OCCLUSION_QUERY and DRM_CHROME9_CAP2_TEXTURE_3D. */
   bool occlusion_query;
   bool texture_3d;
   /* Active pipe queries and the counting interval their draws share. */
   struct list_head active_queries;
   struct list_head query_intervals;
   struct chrome9_query_interval *query_interval;
   /* Recycled kernel query ids. */
   struct util_dynarray query_ids;
   bool queries_paused;
   bool query_inexact_reported;
   struct chrome9_query *render_cond_query;
   bool render_cond_condition;
   enum pipe_render_cond_flag render_cond_mode;
   struct pipe_stencil_ref stencil_ref;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_constant_buffer fs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   uint32_t fs_inline[MAX_INLINABLE_UNIFORMS];
   unsigned fs_inline_count;
   /* Lower-left sprite coordinates the FS variant flips, zero without. */
   uint32_t fs_sprite_flip;
   struct chrome9_sampler_state *sampler_states[PIPE_MAX_SAMPLERS];
   unsigned num_sampler_states;
   struct pipe_sampler_view *sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   unsigned num_sampler_views;
   struct pipe_sampler_state *vs_sampler_states[CHROME9_VS_MAX_SAMPLERS];
   unsigned num_vs_sampler_states;
   struct pipe_sampler_view *vs_sampler_views[CHROME9_VS_MAX_SAMPLERS];
   unsigned num_vs_sampler_views;
   struct chrome9_vs_texture_mapping vs_texture_mappings[CHROME9_VS_MAX_SAMPLERS];
   struct tgsi_sampler *vs_tgsi_sampler;

   struct chrome9_shader *vs;
   struct chrome9_shader *fs;
   struct pipe_blend_state *blend;
   struct pipe_depth_stencil_alpha_state *dsa;
   struct pipe_rasterizer_state *rasterizer;
   /* The rasterizer copy bound to the draw module. */
   const struct pipe_rasterizer_state *draw_rasterizer;
   struct chrome9_vertex_elements *vertex_elements;
   struct pipe_scissor_state scissor;
   bool scissor_valid;
   struct pipe_viewport_state viewport;
   /* CHROME9_TCL=1 with DRM_CHROME9_CAP_VERTEX_PROGRAM. */
   bool tcl_enabled;
   /* Stage B: hardware vertex fetch from GEM buffers. */
   bool tcl_buffers;
   /* Stage B non-indexed draws index repeated vertices once. */
   bool tcl_dedupe;
   /* CAP2_COARSE_Z: depth surfaces cleared to 1.0 get a coarse-Z buffer;
    * the last draw used it (else the next use resets the tag cache). */
   bool coarse_z;
   bool coarse_z_last;
   /* CAP2_FAST_Z: X8Z24 clears to 1.0 advance a scene index. */
   bool fast_z;
   /* CAP2_DEPTH_OUTPUT: fragment programs may write gl_FragDepth. */
   bool depth_output;
   /* CAP2_EDGE_AA: smooth TCL lines with hardware edge antialiasing. */
   bool tcl_edge_aa;
   /* The draw module JIT-compiles vertex shaders (and samples vertex
    * textures through mapped levels). */
   bool draw_llvm;
   /* The current draw rasterizes filled triangles. */
   bool draw_filled_triangles;
   /* TCL draws queued for one DRAW_VERTEX_PROGRAMS submission. */
   bool tcl_batching;
   struct chrome9_tcl_batch *tcl_batch;
   /* Native back-face color (oBFD0) and user clip planes (FE A05). */
   bool tcl_twoside;
   /* Native triangle strips and fans. */
   bool tcl_topology;
   bool tcl_clip_distance;
   /* Native points (and VS point size), one-pixel lines, polygon modes. */
   bool tcl_points;
   bool tcl_point_size;
   bool tcl_lines;
   bool tcl_polygon_mode;
   /* REP/IF lowering (CAP2_VERTEX_STATIC_FLOW); max_depth 0 without. */
   struct chrome9_tcl_flow tcl_flow;
   struct chrome9_tcl_state *tcl_state;
   /* llvmpipe fragment fallback (chrome9_swfb.c), created on first use. */
   struct chrome9_sw_context *sw;
   struct pipe_blend_color blend_color;
   struct pipe_clip_state clip;
   struct pipe_poly_stipple poly_stipple;
   uint64_t tcl_triangles;
   uint64_t tcl_buffer_triangles;
   uint64_t tcl_buffer_rejects;
   uint64_t tcl_fallback_draws;
   uint64_t tcl_flushes;

   uint64_t submitted_triangles;
   uint64_t submitted_batches;
   uint64_t submitted_program_batches;
   uint64_t submitted_rhw_runs;
   uint64_t submitted_rhw_chains;
   uint64_t rejected_triangles;
   /* Per drop site (static "file:line" string) counts, CHROME9_DEBUG_REJECT. */
   struct hash_table *reject_sites;
   uint64_t reject_reported;
   uint64_t draw_limit;
   bool debug_draws;
   bool draw_limit_reported;
   uint32_t warnings;
};

static inline struct chrome9_context *
chrome9_context(struct pipe_context *pipe)
{
   return (struct chrome9_context *)pipe;
}

struct chrome9_resource;
struct chrome9_resource *
chrome9_context_render_target(struct chrome9_context *ctx);
struct chrome9_resource *
chrome9_context_depth_target(struct chrome9_context *ctx);
void chrome9_context_render_target_written(struct chrome9_context *ctx);
void chrome9_context_render_target_direct_written(struct chrome9_context *ctx);
void chrome9_context_prepare_render_target(struct chrome9_context *ctx);
void chrome9_count_reject(struct chrome9_context *ctx, const char *site,
                          unsigned count);
#define CHROME9_STR2(x) #x
#define CHROME9_STR(x) CHROME9_STR2(x)
#define CHROME9_REJECT(ctx, count) \
   chrome9_count_reject(ctx, __FILE__ ":" CHROME9_STR(__LINE__), count)
void chrome9_context_sync_resource(struct chrome9_context *ctx,
                                   struct pipe_resource *resource,
                                   unsigned usage);
/* The NIR normalization of the fragment backend, before lowering. */
void chrome9_fs_prepare_semantic_nir(struct nir_shader *nir);
void chrome9_preshader_destroy(struct chrome9_preshader *pre);
bool chrome9_preshader_eval(const struct chrome9_preshader *pre,
                            const uint8_t *map, size_t size,
                            uint32_t constants[DRM_CHROME9_FRAGMENT_MAX_CONSTANTS][4]);
void chrome9_compile_fs_nir(struct chrome9_context *ctx,
                            struct chrome9_shader *shader,
                            struct nir_shader *nir);

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags);

#endif
