/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_QUERY_H
#define CHROME9_QUERY_H

#include <stdbool.h>
#include <stdint.h>

struct chrome9_context;
struct pipe_context;
struct pipe_query;

void chrome9_init_query_functions(struct chrome9_context *ctx);
void chrome9_query_context_destroy(struct chrome9_context *ctx);
/* Draws count samples into an open interval. */
bool chrome9_query_counting(const struct chrome9_context *ctx);
/* Kernel query of the open interval for FRAGMENT_QUERY, 0 if none. */
uint32_t chrome9_query_draw_id(const struct chrome9_context *ctx);
/* A draw of the open interval could not be counted. */
void chrome9_query_mark_inexact(struct chrome9_context *ctx);
/* Keep an llvmpipe counter active for the fallback draws of the interval. */
void chrome9_query_sw_begin(struct chrome9_context *ctx,
                            struct pipe_context *lp);
bool chrome9_render_condition_passes(struct chrome9_context *ctx);

#endif
