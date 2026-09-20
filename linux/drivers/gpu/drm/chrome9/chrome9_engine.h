/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Chrome9 contributors */
#ifndef __CHROME9_ENGINE_H__
#define __CHROME9_ENGINE_H__

#include <linux/types.h>

struct chrome9_device;
struct drm_device;
struct drm_file;

int chrome9_engine_init(struct chrome9_device *cdev);
void chrome9_engine_fini(struct chrome9_device *cdev);
bool chrome9_engine_ready(const struct chrome9_device *cdev);
int chrome9_fixed_quad_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file);
int chrome9_draw_triangle_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file);
int chrome9_draw_triangle_d16_ioctl(struct drm_device *drm, void *data,
				    struct drm_file *file);

#endif
