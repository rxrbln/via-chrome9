/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_CONTEXT_H
#define CHROME9_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "compiler/shader_info.h"

#include "chrome9_shader_offline.h"
#include "chrome9_vs_texture.h"

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

struct chrome9_fs_variant {
   struct chrome9_fs_variant *next;
   uint32_t inline_values[MAX_INLINABLE_UNIFORMS];
   /* 0: dynamic FACE, 1: surviving front face, 2: surviving back face. */
   uint8_t face_mode;
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
   /* Vertex shader NIR kept for the llvmpipe fragment fallback. */
   struct nir_shader *sw_nir;
   struct chrome9_fs_variant *variants;
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
};

struct chrome9_sampler_state {
   struct pipe_sampler_state base;
   uint8_t min_filter;
   uint8_t mag_filter;
   uint8_t mip_filter;
   uint8_t wrap_s;
   uint8_t wrap_t;
   bool supported;
};

struct chrome9_sampler_view {
   struct pipe_sampler_view base;
   /* Unguarded external images remain the Gallium-visible texture.  The
    * hardware samples this guarded, draw-refreshed copy instead. */
   struct pipe_resource *shadow;
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
   bool render_target_dirty;
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
   bool fragment_primary;
   bool fragment_kill;
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
   bool alpha_test_supported;
   bool scissor_supported;
   bool fragment_stencil;
   struct pipe_stencil_ref stencil_ref;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_constant_buffer fs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   uint32_t fs_inline[MAX_INLINABLE_UNIFORMS];
   unsigned fs_inline_count;
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
   struct chrome9_vertex_elements *vertex_elements;
   struct pipe_scissor_state scissor;
   bool scissor_valid;
   struct pipe_viewport_state viewport;
   /* CHROME9_TCL=1 with DRM_CHROME9_CAP_VERTEX_PROGRAM. */
   bool tcl_enabled;
   /* Stage B: hardware vertex fetch from GEM buffers. */
   bool tcl_buffers;
   struct chrome9_tcl_state *tcl_state;
   /* llvmpipe fragment fallback (chrome9_swfb.c), created on first use. */
   struct chrome9_sw_context *sw;
   struct pipe_blend_color blend_color;
   struct pipe_clip_state clip;
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
void chrome9_compile_fs_nir(struct chrome9_context *ctx,
                            struct chrome9_shader *shader,
                            struct nir_shader *nir);

struct pipe_context *
chrome9_context_create(struct pipe_screen *screen, void *priv, unsigned flags);

#endif
