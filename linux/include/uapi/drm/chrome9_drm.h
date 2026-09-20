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
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE	0x05
#define DRM_CHROME9_DRAW_TRIANGLE_BLEND	0x06
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_BLEND	0x07

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
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE, \
		struct drm_chrome9_draw_textured_triangle)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_BLEND \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE_BLEND, \
		struct drm_chrome9_draw_triangle_blend)
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_BLEND \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_BLEND, \
		struct drm_chrome9_draw_textured_triangle_blend)

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
/* One bounded, level-zero 2D texture and immediate triangle are available. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE	(1ULL << 6)
/* RT0 ADD blending and color masks for immediate color triangles. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND	(1ULL << 7)
/* RT0 ADD blending and color masks for immediate textured triangles. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND	(1ULL << 8)
/* Blend successors can render to guarded A8R8G8B8 surfaces. */
#define DRM_CHROME9_CAP_BLEND_BGRA8888	(1ULL << 9)

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

#define DRM_CHROME9_TEXTURE_PROGRAM_REPLACE	0
#define DRM_CHROME9_TEXTURE_PROGRAM_MODULATE	1

#define DRM_CHROME9_TEXTURE_FORMAT_BGRX8888	0
#define DRM_CHROME9_TEXTURE_FORMAT_BGRA8888	1

#define DRM_CHROME9_TEXTURE_FILTER_NEAREST	0
#define DRM_CHROME9_TEXTURE_FILTER_LINEAR	1

#define DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE	1
#define DRM_CHROME9_TEXTURE_WRAP_REPEAT		2
#define DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT	3

/* IEEE-754 fields are passed as their exact 32-bit representation. */
struct drm_chrome9_textured_vertex {
	__s32 x_16_16;
	__s32 y_16_16;
	__s32 z_16_16;
	__u32 rhw_float;
	__u32 color;
	__u32 specular;
	__u32 s_float;
	__u32 t_float;
};

/*
 * Draw one single-textured triangle into a guarded X8R8G8B8 surface.
 * Both objects use the native one-guard-page, data, one-guard-page layout.
 * Texture pitch must be the canonical ALIGN(texture_width * 4, 32), and
 * coordinates are normalized against texture_width.  Level zero and 32-bit
 * linear textures are the only layouts exposed by this first ABI.
 */
struct drm_chrome9_draw_textured_triangle {
	__u32 destination_handle;
	__u32 texture_handle;
	__u32 flags;
	__u32 program;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 texture_width;
	__u32 texture_height;
	__u32 texture_pitch;
	__u32 texture_format;
	__u32 min_filter;
	__u32 mag_filter;
	__u32 wrap_s;
	__u32 wrap_t;
	__u32 pad;
	struct drm_chrome9_textured_vertex vertices[3];
	__u64 reserved[2];
};

#define DRM_CHROME9_BLEND_ENABLE	(1U << 0)

#define DRM_CHROME9_BLEND_FACTOR_ZERO		0
#define DRM_CHROME9_BLEND_FACTOR_ONE		1
#define DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA	2
#define DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA	3

#define DRM_CHROME9_BLEND_FUNC_ADD	0

#define DRM_CHROME9_COLOR_MASK_R	(1U << 0)
#define DRM_CHROME9_COLOR_MASK_G	(1U << 1)
#define DRM_CHROME9_COLOR_MASK_B	(1U << 2)
#define DRM_CHROME9_COLOR_MASK_A	(1U << 3)

#define DRM_CHROME9_DESTINATION_FORMAT_BGRX8888	0
#define DRM_CHROME9_DESTINATION_FORMAT_BGRA8888	1

struct drm_chrome9_blend_state {
	__u32 flags;
	__u32 rgb_src_factor;
	__u32 rgb_dst_factor;
	__u32 alpha_src_factor;
	__u32 alpha_dst_factor;
	__u32 rgb_func;
	__u32 alpha_func;
	__u32 color_mask;
	__u32 reserved[2];
};

/*
 * Blend-aware successor; the original DRAW_TRIANGLE ABI remains unchanged.
 * destination_format selects the destination A52 storage format.
 */
struct drm_chrome9_draw_triangle_blend {
	__u32 destination_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 pitch;
	__u32 pad;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_vertex vertices[3];
	__u32 destination_format;
	__u32 reserved[3];
};

/*
 * Blend-aware successor; the original textured-triangle ABI is unchanged.
 * destination_format selects the destination A52 storage format.
 */
struct drm_chrome9_draw_textured_triangle_blend {
	__u32 destination_handle;
	__u32 texture_handle;
	__u32 flags;
	__u32 program;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 texture_width;
	__u32 texture_height;
	__u32 texture_pitch;
	__u32 texture_format;
	__u32 min_filter;
	__u32 mag_filter;
	__u32 wrap_s;
	__u32 wrap_t;
	__u32 pad;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_textured_vertex vertices[3];
	__u32 destination_format;
	__u32 reserved[3];
};

#if defined(__cplusplus)
}
#endif

#endif
