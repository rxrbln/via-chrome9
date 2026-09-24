/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_SPECIALISE_H
#define CHROME9_SPECIALISE_H

#include <stdbool.h>
#include <stdint.h>

struct nir_shader;

/* Replace the exact power-of-four integer indexing used only to address
 * load_ubo_vec4 with float-backed multiplication.  The draw interpreter's
 * no-integer lowering can then retain the bounded address arithmetic. */
bool chrome9_nir_lower_ubo_index_shifts(struct nir_shader *nir);

/* Chrome9 has no native integer ALU.  Lower integer shader operations to the
 * float representation Mesa places in CB0 while retaining integer UBO
 * element indices used only for addressing. */
void chrome9_nir_lower_integers(struct nir_shader *nir);

/* Record only direct CB0 dwords that decide eliminable control flow or are
 * uniform leaves of a mixed terminate predicate.  Mesa's state tracker uses
 * this metadata to deliver their current raw values through
 * pipe_context::set_inlinable_constants. */
void chrome9_nir_find_inlinable_uniforms(struct nir_shader *nir);

/* Substitute the raw values recorded above, then fold and unroll until the
 * shader reaches a stable form. */
void chrome9_nir_inline_uniforms(struct nir_shader *nir,
                                 const uint32_t *values);

/* Replace FACE by the one semantic face which can survive a cull-front or
 * cull-back draw.  Cull-none callers must retain dynamic FACE and must not
 * call this helper. */
bool chrome9_nir_specialize_front_face(struct nir_shader *nir,
                                       bool front_face);

#endif
