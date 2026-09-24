/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * llvmpipe fallback for draws the Chrome9 pixel engine cannot execute
 * (fragment programs beyond 96 ALU / 32 TEX / 12 temps).  llvmpipe renders
 * directly into the chrome9 BO mappings: level-0 surfaces are wrapped as
 * llvmpipe display targets, mipmapped textures are copied.  All chrome9
 * ioctls are synchronous and the kernel flushes CPU caches around every
 * job, so ordering only needs "finish llvmpipe before the next GPU job"
 * (chrome9_sw_finish, called from chrome9_draw_flush_batch) and "retire GPU
 * batches before llvmpipe starts" (chrome9_draw_flush_batch here).
 * docs/research/chrome9-llvmpipe-fragment-fallback-design.md.
 */

#include "chrome9_swfb.h"

#include "chrome9_context.h"
#include "chrome9_draw.h"
#include "chrome9_public.h"
#include "chrome9_resource.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "frontend/sw_winsys.h"
#include "frontend/winsys_handle.h"
#include "llvmpipe/lp_public.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/hash_table.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct chrome9_sw_dt {
   uint8_t *map;
};

/* The display target is the chrome9 level-0 mapping itself. */
static struct sw_displaytarget *
chrome9_sw_dt_from_handle(struct sw_winsys *ws,
                          const struct pipe_resource *templ,
                          struct winsys_handle *whandle, unsigned *stride)
{
   struct chrome9_resource *res = whandle->com_obj;
   struct chrome9_sw_dt *dt = CALLOC_STRUCT(chrome9_sw_dt);

   (void)ws;
   (void)templ;
   if (!dt)
      return NULL;
   dt->map = (uint8_t *)res->bo.map + res->level_offset[0];
   *stride = res->level_stride[0];
   return (struct sw_displaytarget *)dt;
}

static void *
chrome9_sw_dt_map(struct sw_winsys *ws, struct sw_displaytarget *dt,
                  unsigned flags)
{
   (void)ws;
   (void)flags;
   return ((struct chrome9_sw_dt *)dt)->map;
}

static void
chrome9_sw_dt_unmap(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
   (void)ws;
   (void)dt;
}

static void
chrome9_sw_dt_destroy(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
   (void)ws;
   FREE(dt);
}

static bool
chrome9_sw_dt_format_supported(struct sw_winsys *ws, unsigned usage,
                               enum pipe_format format)
{
   (void)ws;
   (void)usage;
   (void)format;
   return false;
}

static void
chrome9_sw_winsys_destroy(struct sw_winsys *ws)
{
   FREE(ws);
}

/* displaytarget_create_mapped stays NULL: llvmpipe would otherwise treat
 * the handle as a dma-buf. */
struct pipe_screen *
chrome9_sw_screen_create(void)
{
   struct sw_winsys *ws = CALLOC_STRUCT(sw_winsys);
   struct pipe_screen *screen;

   if (!ws)
      return NULL;
   ws->destroy = chrome9_sw_winsys_destroy;
   ws->is_displaytarget_format_supported = chrome9_sw_dt_format_supported;
   ws->displaytarget_from_handle = chrome9_sw_dt_from_handle;
   ws->displaytarget_map = chrome9_sw_dt_map;
   ws->displaytarget_unmap = chrome9_sw_dt_unmap;
   ws->displaytarget_destroy = chrome9_sw_dt_destroy;
   screen = llvmpipe_create_screen(ws);
   if (!screen)
      FREE(ws);
   return screen;
}

struct chrome9_sw_context {
   struct pipe_screen *screen;
   struct pipe_context *lp;
   /* chrome9 CSO/shader/resource pointer -> llvmpipe object. */
   struct hash_table *objects;
   /* Level-0 aliases of the pending draws, released by chrome9_sw_finish
    * so none outlives its chrome9 BO. */
   struct hash_table *aliases;
   /* chrome9 references held while llvmpipe may still access them. */
   struct util_dynarray held;
   bool pending;
   uint64_t draws;
   uint64_t us;
};

static bool
chrome9_sw_debug(void)
{
   return debug_get_bool_option("CHROME9_SW_DEBUG", false);
}

bool
chrome9_sw_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = debug_get_bool_option("CHROME9_SW_FALLBACK", true);
   return enabled;
}

static void
chrome9_sw_release_held(struct chrome9_sw_context *sw)
{
   struct pipe_framebuffer_state empty = { 0 };

   /* Drop every llvmpipe binding of an alias before releasing it. */
   sw->lp->set_framebuffer_state(sw->lp, &empty);
   sw->lp->set_sampler_views(sw->lp, MESA_SHADER_FRAGMENT, 0, 0,
                             PIPE_MAX_SHADER_SAMPLER_VIEWS, NULL);
   sw->lp->set_sampler_views(sw->lp, MESA_SHADER_VERTEX, 0, 0,
                             PIPE_MAX_SHADER_SAMPLER_VIEWS, NULL);
   hash_table_foreach(sw->aliases, entry) {
      struct pipe_resource *alias = entry->data;

      pipe_resource_reference(&alias, NULL);
   }
   _mesa_hash_table_clear(sw->aliases, NULL);
   util_dynarray_foreach(&sw->held, struct pipe_resource *, res)
      pipe_resource_reference(res, NULL);
   util_dynarray_clear(&sw->held);
}

void
chrome9_sw_finish(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct pipe_fence_handle *fence = NULL;

   if (!sw || !sw->pending)
      return;
   sw->lp->flush(sw->lp, &fence, 0);
   if (fence) {
      sw->screen->fence_finish(sw->screen, NULL, fence, OS_TIMEOUT_INFINITE);
      sw->screen->fence_reference(sw->screen, &fence, NULL);
   }
   chrome9_sw_release_held(sw);
   sw->pending = false;
}

static void
chrome9_sw_hold(struct chrome9_sw_context *sw, struct pipe_resource *res)
{
   struct pipe_resource *ref = NULL;

   if (!res)
      return;
   pipe_resource_reference(&ref, res);
   util_dynarray_append(&sw->held, ref);
}

static struct chrome9_sw_context *
chrome9_sw_context(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;

   if (sw)
      return sw;
   sw = CALLOC_STRUCT(chrome9_sw_context);
   if (!sw)
      return NULL;
   sw->screen = chrome9_screen_sw(ctx->base.screen);
   if (sw->screen)
      sw->lp = sw->screen->context_create(sw->screen, NULL, 0);
   util_dynarray_init(&sw->held, NULL);
   sw->objects = _mesa_pointer_hash_table_create(NULL);
   sw->aliases = _mesa_pointer_hash_table_create(NULL);
   if (!sw->lp || !sw->objects || !sw->aliases) {
      if (sw->lp)
         sw->lp->destroy(sw->lp);
      _mesa_hash_table_destroy(sw->objects, NULL);
      _mesa_hash_table_destroy(sw->aliases, NULL);
      FREE(sw);
      return NULL;
   }
   ctx->sw = sw;
   return sw;
}

enum chrome9_sw_kind {
   CHROME9_SW_BLEND,
   CHROME9_SW_DSA,
   CHROME9_SW_RASTERIZER,
   CHROME9_SW_SAMPLER,
   CHROME9_SW_VELEMS,
   CHROME9_SW_VS,
   CHROME9_SW_FS,
};

struct chrome9_sw_object {
   enum chrome9_sw_kind kind;
   void *cso;
};

static void
chrome9_sw_delete_object(struct pipe_context *lp, struct chrome9_sw_object *o)
{
   switch (o->kind) {
   case CHROME9_SW_BLEND:
      lp->delete_blend_state(lp, o->cso);
      break;
   case CHROME9_SW_DSA:
      lp->delete_depth_stencil_alpha_state(lp, o->cso);
      break;
   case CHROME9_SW_RASTERIZER:
      lp->delete_rasterizer_state(lp, o->cso);
      break;
   case CHROME9_SW_SAMPLER:
      lp->delete_sampler_state(lp, o->cso);
      break;
   case CHROME9_SW_VELEMS:
      lp->delete_vertex_elements_state(lp, o->cso);
      break;
   case CHROME9_SW_VS:
      lp->delete_vs_state(lp, o->cso);
      break;
   case CHROME9_SW_FS:
      lp->delete_fs_state(lp, o->cso);
      break;
   }
   FREE(o);
}

/* Called from every chrome9 delete hook: drop the llvmpipe twin.  Bound
 * twins are unbound first so llvmpipe never keeps a dangling pointer. */
void
chrome9_sw_forget(struct chrome9_context *ctx, const void *object)
{
   struct chrome9_sw_context *sw = ctx->sw;
   struct hash_entry *entry;
   struct chrome9_sw_object *o;

   if (!sw || !object)
      return;
   entry = _mesa_hash_table_search(sw->objects, object);
   if (!entry)
      return;
   o = entry->data;
   chrome9_sw_finish(ctx);
   switch (o->kind) {
   case CHROME9_SW_BLEND:
      sw->lp->bind_blend_state(sw->lp, NULL);
      break;
   case CHROME9_SW_DSA:
      sw->lp->bind_depth_stencil_alpha_state(sw->lp, NULL);
      break;
   case CHROME9_SW_RASTERIZER:
      sw->lp->bind_rasterizer_state(sw->lp, NULL);
      break;
   case CHROME9_SW_SAMPLER: {
      void *none[PIPE_MAX_SAMPLERS] = { 0 };

      sw->lp->bind_sampler_states(sw->lp, MESA_SHADER_FRAGMENT, 0,
                                  PIPE_MAX_SAMPLERS, none);
      sw->lp->bind_sampler_states(sw->lp, MESA_SHADER_VERTEX, 0,
                                  PIPE_MAX_SAMPLERS, none);
      break;
   }
   case CHROME9_SW_VELEMS:
      sw->lp->bind_vertex_elements_state(sw->lp, NULL);
      break;
   case CHROME9_SW_VS:
      sw->lp->bind_vs_state(sw->lp, NULL);
      break;
   case CHROME9_SW_FS:
      sw->lp->bind_fs_state(sw->lp, NULL);
      break;
   }
   chrome9_sw_delete_object(sw->lp, o);
   _mesa_hash_table_remove(sw->objects, entry);
}

void
chrome9_sw_context_destroy(struct chrome9_context *ctx)
{
   struct chrome9_sw_context *sw = ctx->sw;

   if (!sw)
      return;
   chrome9_sw_finish(ctx);
   chrome9_sw_release_held(sw);
   sw->lp->bind_blend_state(sw->lp, NULL);
   sw->lp->bind_depth_stencil_alpha_state(sw->lp, NULL);
   sw->lp->bind_rasterizer_state(sw->lp, NULL);
   sw->lp->bind_vertex_elements_state(sw->lp, NULL);
   sw->lp->bind_vs_state(sw->lp, NULL);
   sw->lp->bind_fs_state(sw->lp, NULL);
   hash_table_foreach(sw->objects, entry)
      chrome9_sw_delete_object(sw->lp, entry->data);
   _mesa_hash_table_destroy(sw->objects, NULL);
   _mesa_hash_table_destroy(sw->aliases, NULL);
   util_dynarray_fini(&sw->held);
   if (chrome9_sw_debug())
      fprintf(stderr, "chrome9: sw fallback draws=%" PRIu64 " time=%" PRIu64
                      " us\n", sw->draws, sw->us);
   sw->lp->destroy(sw->lp);
   FREE(sw);
   ctx->sw = NULL;
}

/* Chrome9 fragment shaders read the facing flag as the FACE varying;
 * llvmpipe provides it as the front-face system value. */
static bool
chrome9_sw_lower_face_instr(nir_builder *b, nir_intrinsic_instr *intr,
                            void *data)
{
   nir_variable *var;
   nir_def *face, *value;

   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;
   var = nir_intrinsic_get_var(intr, 0);
   if (!var || var->data.mode != nir_var_shader_in ||
       var->data.location != VARYING_SLOT_FACE ||
       intr->def.bit_size != 32)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   face = nir_load_front_face(b, 1);
   value = glsl_get_base_type(var->type) == GLSL_TYPE_FLOAT ?
           nir_bcsel(b, face, nir_imm_float(b, 1.0f),
                     nir_imm_float(b, -1.0f)) :
           nir_b2b32(b, face);
   nir_def_replace(&intr->def,
                   nir_replicate(b, value, intr->def.num_components));
   return true;
}

static void
chrome9_sw_lower_face_input(nir_shader *nir)
{
   if (!nir_shader_intrinsics_pass(nir, chrome9_sw_lower_face_instr,
                                   nir_metadata_control_flow, NULL))
      return;
   nir_foreach_shader_in_variable(var, nir)
      if (var->data.location == VARYING_SLOT_FACE)
         var->data.mode = nir_var_shader_temp;
   nir_fixup_deref_modes(nir);
   nir_opt_dce(nir);
   nir_remove_dead_variables(nir, nir_var_shader_temp, NULL);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static void *
chrome9_sw_object(struct chrome9_sw_context *sw, const void *key,
                  enum chrome9_sw_kind kind, const void *state)
{
   struct hash_entry *entry = _mesa_hash_table_search(sw->objects, key);
   struct chrome9_sw_object *o;

   if (entry)
      return ((struct chrome9_sw_object *)entry->data)->cso;
   o = CALLOC_STRUCT(chrome9_sw_object);
   if (!o)
      return NULL;
   o->kind = kind;
   switch (kind) {
   case CHROME9_SW_BLEND:
      o->cso = sw->lp->create_blend_state(sw->lp, state);
      break;
   case CHROME9_SW_DSA:
      o->cso = sw->lp->create_depth_stencil_alpha_state(sw->lp, state);
      break;
   case CHROME9_SW_RASTERIZER:
      o->cso = sw->lp->create_rasterizer_state(sw->lp, state);
      break;
   case CHROME9_SW_SAMPLER:
      o->cso = sw->lp->create_sampler_state(sw->lp, state);
      break;
   case CHROME9_SW_VELEMS: {
      const struct chrome9_vertex_elements *velems = state;

      o->cso = sw->lp->create_vertex_elements_state(sw->lp, velems->count,
                                                    velems->elements);
      break;
   }
   case CHROME9_SW_VS:
   case CHROME9_SW_FS: {
      struct pipe_shader_state shader = {
         .type = PIPE_SHADER_IR_NIR,
         .ir.nir = nir_shader_clone(NULL, state),
      };

      /* The chrome9 lowering float-encodes integer uniforms but keeps
       * native 1-bit booleans; gallivm's no_integers path cannot take
       * those, so compile under llvmpipe's own options. */
      if (shader.ir.nir) {
         shader.ir.nir->options =
            sw->screen->nir_options[kind == CHROME9_SW_VS ?
                                    MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT];
         if (kind == CHROME9_SW_FS)
            chrome9_sw_lower_face_input(shader.ir.nir);
      }
      if (shader.ir.nir && debug_get_bool_option("CHROME9_SW_DUMP", false))
         nir_print_shader(shader.ir.nir, stderr);
      if (shader.ir.nir)
         o->cso = kind == CHROME9_SW_VS ?
                  sw->lp->create_vs_state(sw->lp, &shader) :
                  sw->lp->create_fs_state(sw->lp, &shader);
      break;
   }
   }
   if (!o->cso) {
      FREE(o);
      return NULL;
   }
   _mesa_hash_table_insert(sw->objects, key, o);
   return o->cso;
}

/* Level-0 display-target alias of a chrome9 surface, created once. */
static struct pipe_resource *
chrome9_sw_alias(struct chrome9_sw_context *sw, struct pipe_resource *resource,
                 unsigned bind)
{
   struct chrome9_resource *res = chrome9_resource(resource);
   struct hash_entry *entry = _mesa_hash_table_search(sw->aliases, resource);
   struct pipe_resource templ = *resource;
   struct winsys_handle handle = {
      .type = WINSYS_HANDLE_TYPE_SHARED,
      .com_obj = res,
   };
   struct pipe_resource *alias;

   /* llvmpipe accesses the pages directly in this draw. */
   chrome9_resource_cpu_access(res, bind != 0);
   if (entry)
      return entry->data;
   if (!res->bo.map || resource->target == PIPE_BUFFER)
      return NULL;
   /* llvmpipe touches whole 4x4 blocks: the row overhang must stay in the
    * page padding before the guard page. */
   if ((resource->height0 & 3) &&
       (uint64_t)res->level_stride[0] * align(resource->height0, 4) >
          align64(res->level_size[0], 4096))
      return NULL;
   (void)bind;
   templ.last_level = 0;
   /* One alias serves sampling and rendering within a draw sequence. */
   templ.bind = PIPE_BIND_SAMPLER_VIEW |
                (util_format_is_depth_or_stencil(resource->format) ?
                 PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET);
   templ.next = NULL;
   pipe_reference_init(&templ.reference, 1);
   alias = sw->screen->resource_from_handle(sw->screen, &templ, &handle, 0);
   if (alias)
      _mesa_hash_table_insert(sw->aliases, resource, alias);
   return alias;
}

/* Mipmapped textures: llvmpipe needs its own packed layout; copy the
 * current contents for every fallback draw. */
static struct pipe_resource *
chrome9_sw_texture(struct chrome9_sw_context *sw,
                   struct pipe_resource *resource)
{
   struct chrome9_resource *res = chrome9_resource(resource);
   struct pipe_resource templ = *resource;
   struct pipe_resource *copy;

   if (!resource->last_level)
      return chrome9_sw_alias(sw, resource, 0);
   chrome9_resource_cpu_access(res, false);
   templ.bind = PIPE_BIND_SAMPLER_VIEW;
   templ.next = NULL;
   pipe_reference_init(&templ.reference, 1);
   copy = sw->screen->resource_create(sw->screen, &templ);
   if (!copy)
      return NULL;
   for (unsigned level = 0; level <= resource->last_level; level++) {
      struct pipe_box box;
      unsigned height = util_format_get_nblocksy(resource->format,
         u_minify(resource->height0, level));

      u_box_2d(0, 0, u_minify(resource->width0, level),
               u_minify(resource->height0, level), &box);
      sw->lp->texture_subdata(sw->lp, copy, level, PIPE_MAP_WRITE, &box,
                              (uint8_t *)res->bo.map + res->level_offset[level],
                              res->level_stride[level],
                              res->level_stride[level] * height);
   }
   return copy;
}

static bool
chrome9_sw_bind_views(struct chrome9_sw_context *sw,
                      mesa_shader_stage stage,
                      struct pipe_sampler_view **views, unsigned count)
{
   struct pipe_sampler_view *lp_views[PIPE_MAX_SHADER_SAMPLER_VIEWS] = { 0 };
   bool ok = true;

   for (unsigned i = 0; i < count; i++) {
      struct pipe_sampler_view templ;
      struct pipe_resource *texture;

      if (!views[i])
         continue;
      texture = chrome9_sw_texture(sw, views[i]->texture);
      if (!texture) {
         ok = false;
         break;
      }
      templ = *views[i];
      templ.texture = NULL;
      templ.context = NULL;
      lp_views[i] = sw->lp->create_sampler_view(sw->lp, texture, &templ);
      if (texture->last_level)
         pipe_resource_reference(&texture, NULL);
      chrome9_sw_hold(sw, views[i]->texture);
   }
   sw->lp->set_sampler_views(sw->lp, stage, 0, ok ? count : 0,
                             PIPE_MAX_SHADER_SAMPLER_VIEWS -
                                (ok ? count : 0), ok ? lp_views : NULL);
   for (unsigned i = 0; i < count; i++)
      pipe_sampler_view_reference(&lp_views[i], NULL);
   return ok;
}

static bool
chrome9_sw_bind_constants(struct chrome9_sw_context *sw,
                          mesa_shader_stage stage,
                          const struct pipe_constant_buffer *buffers)
{
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct pipe_constant_buffer *cb = &buffers[i];
      struct pipe_constant_buffer copy = { 0 };
      const uint8_t *data = cb->user_buffer;

      if (!data && cb->buffer)
         data = chrome9_resource(cb->buffer)->bo.map;
      if (!data || !cb->buffer_size) {
         sw->lp->set_constant_buffer(sw->lp, stage, i, NULL);
         continue;
      }
      /* llvmpipe keeps constant buffers by reference until rasterized. */
      u_upload_data_ref(sw->lp->const_uploader, 0, cb->buffer_size, 16,
                    data + cb->buffer_offset, &copy.buffer_offset,
                    &copy.buffer);
      if (!copy.buffer)
         return false;
      copy.buffer_size = cb->buffer_size;
      sw->lp->set_constant_buffer(sw->lp, stage, i, &copy);
      pipe_resource_reference(&copy.buffer, NULL);
   }
   u_upload_unmap(sw->lp->const_uploader);
   return true;
}

static bool
chrome9_sw_bind_state(struct chrome9_context *ctx, struct chrome9_sw_context *sw)
{
   struct pipe_context *lp = sw->lp;
   struct pipe_framebuffer_state fb = ctx->framebuffer;
   struct pipe_vertex_buffer vbs[PIPE_MAX_ATTRIBS];
   void *samplers[PIPE_MAX_SAMPLERS] = { 0 };
   void *cso;

   /* Color renders into the guarded target the hardware path uses. */
   for (unsigned i = 0; i < fb.nr_cbufs; i++) {
      struct pipe_resource *target = i == 0 && ctx->render_target ?
                                     ctx->render_target : fb.cbufs[i].texture;

      if (!target)
         continue;
      fb.cbufs[i].texture = chrome9_sw_alias(sw, target,
                                             PIPE_BIND_RENDER_TARGET);
      if (!fb.cbufs[i].texture || fb.cbufs[i].level)
         return false;
      chrome9_sw_hold(sw, target);
   }
   if (fb.zsbuf.texture) {
      struct pipe_resource *zs = fb.zsbuf.texture;

      fb.zsbuf.texture = chrome9_sw_alias(sw, zs, PIPE_BIND_DEPTH_STENCIL);
      if (!fb.zsbuf.texture || fb.zsbuf.level)
         return false;
      chrome9_sw_hold(sw, zs);
   }
   lp->set_framebuffer_state(lp, &fb);

   if (!ctx->blend || !ctx->dsa || !ctx->rasterizer || !ctx->vertex_elements)
      return false;
   if (!(cso = chrome9_sw_object(sw, ctx->blend, CHROME9_SW_BLEND, ctx->blend)))
      return false;
   lp->bind_blend_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->dsa, CHROME9_SW_DSA, ctx->dsa)))
      return false;
   lp->bind_depth_stencil_alpha_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->rasterizer, CHROME9_SW_RASTERIZER,
                                 ctx->rasterizer)))
      return false;
   lp->bind_rasterizer_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->vertex_elements, CHROME9_SW_VELEMS,
                                 ctx->vertex_elements)))
      return false;
   lp->bind_vertex_elements_state(lp, cso);
   if (!ctx->vs->sw_nir || !ctx->fs->nir ||
       !(cso = chrome9_sw_object(sw, ctx->vs, CHROME9_SW_VS, ctx->vs->sw_nir)))
      return false;
   lp->bind_vs_state(lp, cso);
   if (!(cso = chrome9_sw_object(sw, ctx->fs, CHROME9_SW_FS, ctx->fs->nir)))
      return false;
   lp->bind_fs_state(lp, cso);

   lp->set_stencil_ref(lp, ctx->stencil_ref);
   lp->set_blend_color(lp, &ctx->blend_color);
   lp->set_sample_mask(lp, ~0u);
   lp->set_viewport_states(lp, 0, 1, &ctx->viewport);
   lp->set_scissor_states(lp, 0, 1, &ctx->scissor);
   lp->set_clip_state(lp, &ctx->clip);

   for (unsigned i = 0; i < ctx->num_sampler_states; i++)
      if (ctx->sampler_states[i] &&
          !(samplers[i] = chrome9_sw_object(sw, ctx->sampler_states[i],
                                            CHROME9_SW_SAMPLER,
                                            &ctx->sampler_states[i]->base)))
         return false;
   lp->bind_sampler_states(lp, MESA_SHADER_FRAGMENT, 0,
                           ctx->num_sampler_states, samplers);
   memset(samplers, 0, sizeof(samplers));
   for (unsigned i = 0; i < ctx->num_vs_sampler_states; i++)
      if (ctx->vs_sampler_states[i] &&
          !(samplers[i] = chrome9_sw_object(sw, ctx->vs_sampler_states[i],
                                            CHROME9_SW_SAMPLER,
                                            ctx->vs_sampler_states[i])))
         return false;
   lp->bind_sampler_states(lp, MESA_SHADER_VERTEX, 0,
                           ctx->num_vs_sampler_states, samplers);
   if (!chrome9_sw_bind_views(sw, MESA_SHADER_FRAGMENT, ctx->sampler_views,
                              ctx->num_sampler_views) ||
       !chrome9_sw_bind_views(sw, MESA_SHADER_VERTEX, ctx->vs_sampler_views,
                              ctx->num_vs_sampler_views))
      return false;
   if (!chrome9_sw_bind_constants(sw, MESA_SHADER_VERTEX, ctx->vs_constants) ||
       !chrome9_sw_bind_constants(sw, MESA_SHADER_FRAGMENT, ctx->fs_constants))
      return false;

   /* Vertex data is consumed synchronously inside draw_vbo. */
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];

      vbs[i] = *vb;
      if (!vb->is_user_buffer) {
         vbs[i].is_user_buffer = true;
         vbs[i].buffer.user = vb->buffer.resource ?
            chrome9_resource(vb->buffer.resource)->bo.map : NULL;
      }
   }
   lp->set_vertex_buffers(lp, ctx->num_vertex_buffers, vbs);
   return true;
}

bool
chrome9_sw_draw_vbo(struct chrome9_context *ctx,
                    const struct pipe_draw_info *info, unsigned drawid_offset,
                    const struct pipe_draw_start_count_bias *draws,
                    unsigned num_draws)
{
   struct pipe_draw_info lp_info = *info;
   struct chrome9_sw_context *sw;
   int64_t start = os_time_get();

   if (!chrome9_sw_enabled() || !ctx->vs || !ctx->fs)
      return false;
   /* Depth written by the pixel engine and llvmpipe differ in rounding:
    * an EQUAL test across the two would only partly pass. */
   if (ctx->dsa && ctx->dsa->depth_enabled &&
       ctx->dsa->depth_func == PIPE_FUNC_EQUAL)
      return false;
   /* Retire GPU batches that wrote the targets or textures read here. */
   chrome9_draw_flush_gpu_batch(ctx);
   chrome9_context_prepare_render_target(ctx);
   sw = chrome9_sw_context(ctx);
   if (!sw)
      return false;
   if (!chrome9_sw_bind_state(ctx, sw) ||
       (info->index_size && !info->has_user_indices &&
        !info->index.resource)) {
      if (chrome9_sw_debug())
         fprintf(stderr, "chrome9: sw fallback state unavailable\n");
      if (!sw->pending)
         chrome9_sw_release_held(sw);
      return false;
   }
   if (info->index_size && !info->has_user_indices) {
      lp_info.has_user_indices = true;
      lp_info.index.user = chrome9_resource(info->index.resource)->bo.map;
   }
   sw->lp->draw_vbo(sw->lp, &lp_info, drawid_offset, NULL, draws, num_draws);
   sw->pending = true;
   if (debug_get_bool_option("CHROME9_SW_SYNC", false))
      chrome9_sw_finish(ctx);
   chrome9_context_render_target_written(ctx);
   sw->draws++;
   sw->us += os_time_get() - start;
   if (chrome9_sw_debug())
      fprintf(stderr, "chrome9: sw fallback draw %" PRIu64 " count=%u "
                      "%" PRId64 " us\n", sw->draws, draws[0].count,
              os_time_get() - start);
   return true;
}
