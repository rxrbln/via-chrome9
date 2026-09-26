/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#ifndef CHROME9_FS_SPLIT_H
#define CHROME9_FS_SPLIT_H

#include <stdbool.h>
#include <stdint.h>

struct chrome9_context;
struct chrome9_shader;
struct nir_def;
struct nir_shader;

/*
 * A fragment program beyond the 96-record store, run as a sequence of
 * jobs over the same triangles.  Every job but the last writes one scalar
 * to a driver-owned R32F carrier; jobs read earlier carriers at their own
 * pixel (vPos) through samplers carrier_base..carrier_base + reads - 1.
 */
struct chrome9_fs_split_job {
   struct chrome9_shader *program;
   /* Output carrier slot, -1 for the final job. */
   int carrier;
   unsigned read_count;
   /* Carrier slot read through sampler carrier_base + i. */
   unsigned *reads;
};

struct chrome9_fs_split {
   unsigned job_count;
   struct chrome9_fs_split_job *jobs;
   unsigned carrier_count;
   unsigned carrier_base;
   /* c[inverse_register] = (1/w, 1/h, 0.5/w, 0.5/h) of the carriers. */
   unsigned inverse_register;
   /* Application samplers, uniforms and immediates over all jobs. */
   uint8_t app_sampler_mask;
};

bool chrome9_fs_split_enabled(void);
/* chrome9_context.c: a job compiled with the parent's varying layout, and
 * what the backend proves about a lane of the original program. */
void chrome9_fs_split_lane_proof(struct nir_def *def, unsigned lane,
                                 bool *finite, bool *uninterpolated);
bool chrome9_fs_split_compile_job(struct chrome9_context *ctx,
                                  const struct chrome9_shader *parent,
                                  struct chrome9_shader *job,
                                  struct nir_shader *nir);
struct chrome9_fs_split *
chrome9_fs_split_create(struct chrome9_context *ctx,
                        const struct chrome9_shader *parent,
                        struct nir_shader *nir);
void chrome9_fs_split_destroy(struct chrome9_fs_split *split);

#endif
