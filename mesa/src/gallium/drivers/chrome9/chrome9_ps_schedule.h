/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef CHROME9_PS_SCHEDULE_H
#define CHROME9_PS_SCHEDULE_H

#include "chrome9_shader_offline.h"

/* Replay the kernel allocator's allocate-before-release TEMP pressure for an
 * existing instruction order. */
bool chrome9_ps_ir_temp_pressure(const struct chrome9_ps_ir *ir,
                                 unsigned *peak_live);

/* Reorder a completed, side-effect-free semantic fragment program to reduce
 * whole-vector TEMP pressure.  This pass deliberately has a narrower domain
 * than the compiler: it accepts only one terminal full-mask COLOR write and
 * rejects KILL or dead instructions.  A fast greedy order is followed by a
 * deterministic bounded search only when it misses the twelve-register
 * hardware envelope.  The returned peak follows the kernel allocator's
 * allocate-before-release convention. */
bool chrome9_schedule_ps_ir(const struct chrome9_ps_ir *input,
                            struct chrome9_ps_ir *output,
                            unsigned *peak_live);

#endif
