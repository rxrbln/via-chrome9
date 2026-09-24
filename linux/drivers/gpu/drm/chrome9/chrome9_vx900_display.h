/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_VX900_DISPLAY_H__
#define __CHROME9_VX900_DISPLAY_H__

#include <linux/types.h>

struct drm_display_mode;

bool chrome9_vx900_iga1_mode_valid(const struct drm_display_mode *mode);
int chrome9_vx900_iga1_program_mode(const struct drm_display_mode *mode,
				    u32 pitch);
int chrome9_vx900_display_selftest(void);

#endif
