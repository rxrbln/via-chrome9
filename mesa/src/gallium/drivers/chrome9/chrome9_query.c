/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

/*
 * Occlusion queries.  Hardware draws of a counting interval carry one
 * kernel query (DRM_CHROME9_FRAGMENT_QUERY), llvmpipe fallback draws of
 * the same interval are counted by an llvmpipe query.  An interval ends
 * whenever the set of active queries changes or counting is paused, so
 * any number of simultaneously active queries share one tag per draw:
 * each query sums the intervals it was active in.
 */

#include "chrome9_query.h"

#include "chrome9_context.h"
#include "chrome9_draw.h"
#include "chrome9_drm.h"
#include "chrome9_swfb.h"

#include <stdio.h>

#include "draw/draw_context.h"
#include "pipe/p_screen.h"
#include "util/list.h"
#include "util/u_dynarray.h"
#include "util/u_memory.h"

struct chrome9_query_interval {
   struct list_head link;
   unsigned references;
   /* Kernel query; zero once resolved or after a failed ioctl. */
   uint32_t id;
   /* llvmpipe counter of the fallback draws. */
   struct pipe_query *sw;
   uint64_t samples;
   bool kernel_done;
   bool resolved;
   /* Some draw's samples are unknown: predicates answer true. */
   bool inexact;
};

struct chrome9_query {
   enum pipe_query_type type;
   bool active;
   struct list_head link;
   struct util_dynarray intervals;
};

static int
chrome9_query_fd(struct chrome9_context *ctx)
{
   return ctx->base.screen->get_screen_fd(ctx->base.screen);
}

static int
chrome9_query_op(struct chrome9_context *ctx, uint32_t op, uint32_t id,
                 uint32_t flags, struct drm_chrome9_query *out)
{
   struct drm_chrome9_query args = {
      .op = op,
      .flags = flags,
      .id = id,
      .type = DRM_CHROME9_QUERY_TYPE_OCCLUSION,
   };
   int ret = chrome9_query_ioctl(chrome9_query_fd(ctx), &args);

   if (out)
      *out = args;
   return ret;
}

static void
chrome9_query_release_id(struct chrome9_context *ctx, uint32_t id)
{
   if (id)
      util_dynarray_append(&ctx->query_ids, id);
}

static struct chrome9_query_interval *
chrome9_query_interval_create(struct chrome9_context *ctx)
{
   struct chrome9_query_interval *iv =
      CALLOC_STRUCT(chrome9_query_interval);
   struct drm_chrome9_query args;
   uint32_t id = 0;

   if (!iv)
      return NULL;
   iv->references = 1;
   list_addtail(&iv->link, &ctx->query_intervals);
   /* Without the hardware counter llvmpipe counts every draw. */
   if (!ctx->occlusion_query)
      return iv;
   if (util_dynarray_num_elements(&ctx->query_ids, uint32_t))
      id = util_dynarray_pop(&ctx->query_ids, uint32_t);
   else if (!chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_CREATE, 0, 0, &args))
      id = args.id;
   if (id && chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_BEGIN, id, 0,
                              NULL)) {
      chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_DESTROY, id, 0, NULL);
      id = 0;
   }
   iv->id = id;
   iv->inexact = !id;
   return iv;
}

static void
chrome9_query_interval_unref(struct chrome9_context *ctx,
                             struct chrome9_query_interval *iv)
{
   struct pipe_context *lp = chrome9_sw_pipe(ctx);

   if (--iv->references)
      return;
   if (iv->sw && lp)
      lp->destroy_query(lp, iv->sw);
   chrome9_query_release_id(ctx, iv->id);
   list_del(&iv->link);
   FREE(iv);
}

static void
chrome9_query_interval_close(struct chrome9_context *ctx)
{
   struct chrome9_query_interval *iv = ctx->query_interval;
   struct pipe_context *lp = chrome9_sw_pipe(ctx);

   if (!iv)
      return;
   ctx->query_interval = NULL;
   if (iv->id && chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_END, iv->id, 0,
                                  NULL)) {
      chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_DESTROY, iv->id, 0, NULL);
      iv->id = 0;
      iv->inexact = true;
   }
   if (iv->sw && lp)
      lp->end_query(lp, iv->sw);
   chrome9_query_interval_unref(ctx, iv);
}

/*
 * The set of counting queries changed: submit the draws tagged with the
 * current interval, close it and open the next one.  draw_flush() makes the
 * vbuf stage re-select its path for the new state.
 */
static void
chrome9_query_update(struct chrome9_context *ctx)
{
   struct chrome9_query_interval *iv;

   draw_flush(ctx->draw);
   chrome9_draw_flush_gpu_batch(ctx);
   chrome9_query_interval_close(ctx);
   if (ctx->queries_paused || list_is_empty(&ctx->active_queries))
      return;
   iv = chrome9_query_interval_create(ctx);
   if (!iv) {
      fprintf(stderr, "chrome9: occlusion query interval allocation failed\n");
      return;
   }
   list_for_each_entry(struct chrome9_query, q, &ctx->active_queries, link) {
      util_dynarray_append(&q->intervals, iv);
      iv->references++;
   }
   ctx->query_interval = iv;
}

bool
chrome9_query_counting(const struct chrome9_context *ctx)
{
   return ctx->query_interval != NULL;
}

uint32_t
chrome9_query_draw_id(const struct chrome9_context *ctx)
{
   return ctx->query_interval ? ctx->query_interval->id : 0;
}

void
chrome9_query_mark_inexact(struct chrome9_context *ctx)
{
   if (ctx->query_interval)
      ctx->query_interval->inexact = true;
}

void
chrome9_query_sw_begin(struct chrome9_context *ctx, struct pipe_context *lp)
{
   struct chrome9_query_interval *iv = ctx->query_interval;

   if (!iv || iv->sw)
      return;
   iv->sw = lp->create_query(lp, PIPE_QUERY_OCCLUSION_COUNTER, 0);
   if (iv->sw && !lp->begin_query(lp, iv->sw)) {
      lp->destroy_query(lp, iv->sw);
      iv->sw = NULL;
   }
   iv->inexact |= !iv->sw;
}

static bool
chrome9_query_interval_resolve(struct chrome9_context *ctx,
                               struct chrome9_query_interval *iv, bool wait)
{
   struct pipe_context *lp = chrome9_sw_pipe(ctx);
   struct drm_chrome9_query args;
   union pipe_query_result result;
   int ret;

   if (iv->resolved)
      return true;
   if (iv == ctx->query_interval)
      return false;
   if (iv->id && !iv->kernel_done) {
      ret = chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_READ, iv->id,
                             wait ? DRM_CHROME9_QUERY_READ_WAIT : 0, &args);
      if (!ret && !args.available)
         return false;
      if (ret) {
         /* -EIO: a tagged draw was lost to an engine error. */
         if (!wait && ret == -EBUSY)
            return false;
         iv->inexact = true;
      } else {
         iv->samples += args.result;
      }
      iv->kernel_done = true;
   }
   if (iv->sw) {
      if (!lp)
         iv->inexact = true;
      else if (!lp->get_query_result(lp, iv->sw, wait, &result))
         return false;
      else
         iv->samples += result.u64;
      if (lp)
         lp->destroy_query(lp, iv->sw);
      iv->sw = NULL;
   }
   iv->resolved = true;
   chrome9_query_release_id(ctx, iv->id);
   iv->id = 0;
   return true;
}

static void
chrome9_query_drop_intervals(struct chrome9_context *ctx,
                             struct chrome9_query *q)
{
   util_dynarray_foreach(&q->intervals, struct chrome9_query_interval *, iv)
      chrome9_query_interval_unref(ctx, *iv);
   util_dynarray_clear(&q->intervals);
}

static struct pipe_query *
chrome9_create_query(struct pipe_context *pipe, unsigned type, unsigned index)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_query *q;

   (void)index;
   if ((!ctx->occlusion_query && !chrome9_sw_enabled()) ||
       (type != PIPE_QUERY_OCCLUSION_COUNTER &&
        type != PIPE_QUERY_OCCLUSION_PREDICATE &&
        type != PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE))
      return NULL;
   q = CALLOC_STRUCT(chrome9_query);
   if (!q)
      return NULL;
   q->type = type;
   list_inithead(&q->link);
   util_dynarray_init(&q->intervals, NULL);
   return (struct pipe_query *)q;
}

static void
chrome9_destroy_query(struct pipe_context *pipe, struct pipe_query *query)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_query *q = (struct chrome9_query *)query;

   if (ctx->render_cond_query == q)
      ctx->render_cond_query = NULL;
   if (q->active) {
      list_del(&q->link);
      q->active = false;
      chrome9_query_update(ctx);
   }
   chrome9_query_drop_intervals(ctx, q);
   util_dynarray_fini(&q->intervals);
   FREE(q);
}

static bool
chrome9_begin_query(struct pipe_context *pipe, struct pipe_query *query)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_query *q = (struct chrome9_query *)query;

   if (q->active)
      return false;
   chrome9_query_drop_intervals(ctx, q);
   q->active = true;
   list_addtail(&q->link, &ctx->active_queries);
   chrome9_query_update(ctx);
   return true;
}

static bool
chrome9_end_query(struct pipe_context *pipe, struct pipe_query *query)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_query *q = (struct chrome9_query *)query;

   if (!q->active)
      return true;
   list_del(&q->link);
   q->active = false;
   chrome9_query_update(ctx);
   return true;
}

static bool
chrome9_get_query_result(struct pipe_context *pipe, struct pipe_query *query,
                         bool wait, union pipe_query_result *result)
{
   struct chrome9_context *ctx = chrome9_context(pipe);
   struct chrome9_query *q = (struct chrome9_query *)query;
   uint64_t samples = 0;
   bool inexact = false;

   if (q->active)
      return false;
   util_dynarray_foreach(&q->intervals, struct chrome9_query_interval *, iv) {
      if (!chrome9_query_interval_resolve(ctx, *iv, wait))
         return false;
      samples += (*iv)->samples;
      inexact |= (*iv)->inexact;
   }
   if (inexact && !ctx->query_inexact_reported) {
      fprintf(stderr, "chrome9: occlusion query lost samples; predicates "
                      "report true\n");
      ctx->query_inexact_reported = true;
   }
   if (q->type == PIPE_QUERY_OCCLUSION_COUNTER)
      result->u64 = samples;
   else
      result->b = samples || inexact;
   return true;
}

/* u_blitter pauses counting around its internal draws. */
static void
chrome9_set_active_query_state(struct pipe_context *pipe, bool enable)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   if (ctx->queries_paused == !enable)
      return;
   ctx->queries_paused = !enable;
   if (!list_is_empty(&ctx->active_queries))
      chrome9_query_update(ctx);
}

static void
chrome9_render_condition(struct pipe_context *pipe, struct pipe_query *query,
                         bool condition, enum pipe_render_cond_flag mode)
{
   struct chrome9_context *ctx = chrome9_context(pipe);

   ctx->render_cond_query = (struct chrome9_query *)query;
   ctx->render_cond_condition = condition;
   ctx->render_cond_mode = mode;
}

/* As softpipe: without a result in NO_WAIT modes, render. */
bool
chrome9_render_condition_passes(struct chrome9_context *ctx)
{
   struct chrome9_query *q = ctx->render_cond_query;
   union pipe_query_result result = { 0 };
   bool wait, passed;

   if (!q)
      return true;
   wait = ctx->render_cond_mode == PIPE_RENDER_COND_WAIT ||
          ctx->render_cond_mode == PIPE_RENDER_COND_BY_REGION_WAIT;
   if (!chrome9_get_query_result(&ctx->base, (struct pipe_query *)q, wait,
                                 &result))
      return true;
   passed = q->type == PIPE_QUERY_OCCLUSION_COUNTER ? result.u64 != 0 :
                                                      result.b;
   return passed != ctx->render_cond_condition;
}

void
chrome9_init_query_functions(struct chrome9_context *ctx)
{
   list_inithead(&ctx->active_queries);
   list_inithead(&ctx->query_intervals);
   util_dynarray_init(&ctx->query_ids, NULL);
   ctx->base.create_query = chrome9_create_query;
   ctx->base.destroy_query = chrome9_destroy_query;
   ctx->base.begin_query = chrome9_begin_query;
   ctx->base.end_query = chrome9_end_query;
   ctx->base.get_query_result = chrome9_get_query_result;
   ctx->base.set_active_query_state = chrome9_set_active_query_state;
   ctx->base.render_condition = chrome9_render_condition;
}

/* Before the llvmpipe context goes away: release its query objects. */
void
chrome9_query_context_destroy(struct chrome9_context *ctx)
{
   struct pipe_context *lp = chrome9_sw_pipe(ctx);

   /* A context that failed early never initialized its query state. */
   if (!ctx->query_intervals.next)
      return;
   chrome9_query_interval_close(ctx);
   list_for_each_entry(struct chrome9_query_interval, iv,
                       &ctx->query_intervals, link) {
      if (iv->sw && lp)
         lp->destroy_query(lp, iv->sw);
      iv->sw = NULL;
      chrome9_query_release_id(ctx, iv->id);
      iv->id = 0;
   }
   util_dynarray_foreach(&ctx->query_ids, uint32_t, id)
      chrome9_query_op(ctx, DRM_CHROME9_QUERY_OP_DESTROY, *id, 0, NULL);
   util_dynarray_fini(&ctx->query_ids);
}
