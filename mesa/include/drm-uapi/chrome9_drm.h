/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#ifndef _UAPI_CHROME9_DRM_H_
#define _UAPI_CHROME9_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_CHROME9_GET_PARAM	0x00
#define DRM_CHROME9_GEM_CREATE	0x01
#define DRM_CHROME9_FIXED_QUAD	0x02
#define DRM_CHROME9_DRAW_TRIANGLE	0x03
#define DRM_CHROME9_DRAW_TRIANGLE_D16	0x04

#define DRM_IOCTL_CHROME9_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CHROME9_GET_PARAM, \
		 struct drm_chrome9_get_param)
#define DRM_IOCTL_CHROME9_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CHROME9_GEM_CREATE, \
		 struct drm_chrome9_gem_create)
#define DRM_IOCTL_CHROME9_FIXED_QUAD \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_FIXED_QUAD, \
		struct drm_chrome9_fixed_quad)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE, \
		struct drm_chrome9_draw_triangle)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16 \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE_D16, \
		struct drm_chrome9_draw_triangle_d16)

enum drm_chrome9_param {
	DRM_CHROME9_PARAM_DEVICE_ID = 0,
	DRM_CHROME9_PARAM_REVISION = 1,
	DRM_CHROME9_PARAM_CAPABILITIES = 2,
	DRM_CHROME9_PARAM_BAR0_SIZE = 3,
	DRM_CHROME9_PARAM_BAR1_SIZE = 4,
	DRM_CHROME9_PARAM_BAR2_SIZE = 5,
};

/* The kernel can allocate and mmap GEM objects. */
#define DRM_CHROME9_CAP_GEM	(1ULL << 0)
/* Historical bring-up allocator; not advertised by the shmem-backed ABI. */
#define DRM_CHROME9_CAP_GEM_DMA	(1ULL << 1)
/* Cached shmem pages use the measured RTSF PTE cache attribute (bit 25). */
#define DRM_CHROME9_CAP_GEM_SHMEM_CACHED	(1ULL << 2)
/* The bounded fixed-quad submission ABI is initialized and available. */
#define DRM_CHROME9_CAP_FIXED_QUAD	(1ULL << 3)
/* Variable-size, kernel-generated immediate Gouraud triangles are available. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE	(1ULL << 4)
/* Guarded X8R8G8B8 plus linear D16 LESS/write triangles are available. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16	(1ULL << 5)

struct drm_chrome9_get_param {
	__u32 param;
	__u32 pad;
	__u64 value;
};

struct drm_chrome9_gem_create {
	__u64 size;
	__u32 flags;
	__u32 handle;
	__u64 mmap_offset;
};

#define DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE	(1U << 0)
#define DRM_CHROME9_FIXED_QUAD_CONSTANT_RED	(1U << 1)
#define DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE	(1U << 2)
#define DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE	(1U << 3)

/*
 * Diagnostic bounded submission: fill the middle page of an exactly
 * three-page native GEM object and verify both guard pages in the kernel.
 * Flags select packet-exact bring-up oracles and are mutually exclusive.
 * This is deliberately not a general command-submission ABI.
 */
struct drm_chrome9_fixed_quad {
	__u32 destination_handle;
	__u32 flags;
	__u64 pad[2];
};

/* Screen-space pixel coordinates in signed 16.16 fixed point. */
struct drm_chrome9_vertex {
	__s32 x_16_16;
	__s32 y_16_16;
	__u32 color;
	__u32 pad;
};

/*
 * Draw one Gouraud-shaded triangle into a guarded X8R8G8B8 surface.
 * The native shmem BO layout is one guard page, page-aligned pixel storage,
 * then one guard page. Pitch must equal ALIGN(width * 4, 32).
 */
struct drm_chrome9_draw_triangle {
	__u32 destination_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 pitch;
	__u32 pad;
	struct drm_chrome9_vertex vertices[3];
	__u64 reserved[2];
};

/* Screen-space X/Y and normalized depth in signed 16.16 fixed point. */
struct drm_chrome9_vertex_d16 {
	__s32 x_16_16;
	__s32 y_16_16;
	__s32 z_16_16;
	__u32 color;
};

/*
 * Draw one Gouraud-shaded, depth-tested triangle.  Color and depth are
 * distinct native shmem BOs, each laid out as one guard page, page-aligned
 * pixel storage, then one guard page.  The color pitch must be
 * ALIGN(width * 4, 32), and the linear D16 pitch ALIGN(width * 2, 32).
 * This first depth ABI intentionally exposes only the live-proven LESS
 * comparison with depth writes enabled.
 */
struct drm_chrome9_draw_triangle_d16 {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 pad;
	struct drm_chrome9_vertex_d16 vertices[3];
	__u64 reserved[2];
};

#if defined(__cplusplus)
}
#endif

#endif
