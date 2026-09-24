/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_SPECIALISE_H
#define CHROME9_SPECIALISE_H

#include <stdint.h>

struct nir_shader;

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

#endif
