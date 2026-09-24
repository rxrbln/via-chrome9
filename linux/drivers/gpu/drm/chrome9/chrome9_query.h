/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_QUERY_H__
#define __CHROME9_QUERY_H__

#include <linux/types.h>

struct chrome9_query;
struct dma_fence;
struct drm_device;
struct drm_file;

unsigned int chrome9_occlusion_query_mode(void);
int chrome9_query_open(struct drm_device *drm, struct drm_file *file);
void chrome9_query_postclose(struct drm_device *drm, struct drm_file *file);
int chrome9_query_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file);
struct chrome9_query *chrome9_query_get_active(struct drm_file *file, u32 id,
					       u32 *generation);
struct chrome9_query *chrome9_query_get(struct chrome9_query *query);
void chrome9_query_put(struct chrome9_query *query);
bool chrome9_query_attach(struct chrome9_query *query, u32 generation,
			  struct dma_fence *fence);
void chrome9_query_add(struct chrome9_query *query, u32 generation,
		       u32 samples, int error);

#endif
