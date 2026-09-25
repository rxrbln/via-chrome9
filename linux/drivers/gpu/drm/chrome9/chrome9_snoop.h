/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __CHROME9_SNOOP_H__
#define __CHROME9_SNOOP_H__

#include <linux/types.h>

struct chrome9_device;
struct drm_mm_node;

/* One 2D SF-to-SF copy (validated COPY_RECT builder), same geometry at
 * both ends: 32-bit pixels, rows of pitch bytes, x in pixels. */
struct chrome9_snoop_copy {
	u32 dst;
	u32 src;
	u32 pitch;
	u32 x;
	u32 width;
	u32 height;
};

/* Engine side (chrome9_engine.c); all but begin need snoop_begin held. */
int chrome9_snoop_begin(struct chrome9_device *cdev);
void chrome9_snoop_end(struct chrome9_device *cdev);
int chrome9_snoop_map(struct chrome9_device *cdev, const u32 *ptes,
		      unsigned int count, struct drm_mm_node *node);
int chrome9_snoop_unmap(struct chrome9_device *cdev, struct drm_mm_node *node);
u64 chrome9_snoop_gart_hash(struct chrome9_device *cdev);
int chrome9_snoop_run_copy(struct chrome9_device *cdev,
			   const struct chrome9_snoop_copy *copy,
			   void (*cpu_step)(void *arg), void *arg);

/* Ladder (chrome9_snoop.c), a no-op unless selftest_snoop is set. */
void chrome9_snoop_selftest(struct chrome9_device *cdev);

#endif
