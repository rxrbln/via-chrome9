/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_DRAW_H
#define CHROME9_DRAW_H

struct chrome9_context;
struct draw_stage;

struct draw_stage *chrome9_draw_vbuf_stage(struct chrome9_context *ctx);
void chrome9_draw_flush_batch(struct chrome9_context *ctx);

#endif
