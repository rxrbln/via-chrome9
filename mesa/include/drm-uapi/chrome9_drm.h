/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
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
#define DRM_CHROME9_DRAW_TRIANGLE_D16_BLEND	0x08
#define DRM_CHROME9_DRAW_TRIANGLE_BATCH	0x09
#define DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE	0x0a
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_D16	0x0b
#define DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_BATCH	0x0c
#define DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16	0x0d
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP	0x0e
#define DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN	0x0f
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_D16_BLEND	0x12
#define DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND	0x13
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_RUN	0x14
#define DRM_CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH	0x19
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_BATCH	0x1a
#define DRM_CHROME9_COPY_RECT	0x1b
#define DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_CHAIN	0x1c
#define DRM_CHROME9_DRAW_FRAGMENT_PROGRAM	0x1d
#define DRM_CHROME9_DRAW_VERTEX_PROGRAM	0x1e
#define DRM_CHROME9_GEM_CPU_ACCESS	0x1f
#define DRM_CHROME9_DRAW_VERTEX_PROGRAMS	0x20
#define DRM_CHROME9_GEM_INFO	0x21
#define DRM_CHROME9_QUERY	0x22

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
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_BLEND \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE_D16_BLEND, \
		struct drm_chrome9_draw_triangle_d16_blend)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_BATCH \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE_BATCH, \
		struct drm_chrome9_draw_triangle_batch)
#define DRM_IOCTL_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE, \
		struct drm_chrome9_draw_two_textured_triangle)
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_D16 \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_D16, \
		struct drm_chrome9_draw_textured_triangle_d16)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_BATCH \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_BATCH, \
		struct drm_chrome9_draw_triangle_d16_rhw_batch)
#define DRM_IOCTL_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16 \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16, \
		struct drm_chrome9_draw_two_textured_triangle_d16)
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP, \
		struct drm_chrome9_draw_textured_triangle_mip)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN, \
		struct drm_chrome9_draw_triangle_d16_rhw_run)
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_D16_BLEND \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_D16_BLEND, \
		struct drm_chrome9_draw_textured_triangle_d16_blend)
#define DRM_IOCTL_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND \
	DRM_IOW(DRM_COMMAND_BASE + \
		DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND, \
		struct drm_chrome9_draw_two_textured_triangle_d16_blend)
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_RUN \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_RUN, \
		struct drm_chrome9_draw_textured_triangle_run)
#define DRM_IOCTL_CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH \
	DRM_IOW(DRM_COMMAND_BASE + \
		DRM_CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH, \
		struct drm_chrome9_draw_program_triangle_d16_rhw_batch)
#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_BATCH \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_BATCH, \
		struct drm_chrome9_draw_textured_triangle_batch)
#define DRM_IOCTL_CHROME9_COPY_RECT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_COPY_RECT, \
		struct drm_chrome9_copy_rect)
#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_CHAIN \
	DRM_IOW(DRM_COMMAND_BASE + \
		DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_CHAIN, \
		struct drm_chrome9_draw_triangle_d16_rhw_chain)
#define DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_FRAGMENT_PROGRAM, \
		struct drm_chrome9_draw_fragment_program)
#define DRM_IOCTL_CHROME9_DRAW_VERTEX_PROGRAM \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_VERTEX_PROGRAM, \
		struct drm_chrome9_draw_vertex_program)
#define DRM_IOCTL_CHROME9_GEM_CPU_ACCESS \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_GEM_CPU_ACCESS, \
		struct drm_chrome9_gem_cpu_access)
#define DRM_IOCTL_CHROME9_DRAW_VERTEX_PROGRAMS \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CHROME9_DRAW_VERTEX_PROGRAMS, \
		struct drm_chrome9_draw_vertex_programs)
#define DRM_IOCTL_CHROME9_GEM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CHROME9_GEM_INFO, \
		 struct drm_chrome9_gem_info)
#define DRM_IOCTL_CHROME9_QUERY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CHROME9_QUERY, \
		 struct drm_chrome9_query)

enum drm_chrome9_param {
	DRM_CHROME9_PARAM_DEVICE_ID = 0,
	DRM_CHROME9_PARAM_REVISION = 1,
	DRM_CHROME9_PARAM_CAPABILITIES = 2,
	DRM_CHROME9_PARAM_BAR0_SIZE = 3,
	DRM_CHROME9_PARAM_BAR1_SIZE = 4,
	DRM_CHROME9_PARAM_BAR2_SIZE = 5,
	DRM_CHROME9_PARAM_CAPABILITIES2 = 6,
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
/* Blend-aware draw successors expose fixed-function fragment alpha test. */
#define DRM_CHROME9_CAP_ALPHA_TEST	(1ULL << 10)
/* Every bounded draw ABI accepts an optional half-open raster scissor. */
#define DRM_CHROME9_CAP_SCISSOR	(1ULL << 11)
/* D16 LESS/write can be combined with blend, masks, and alpha test. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND	(1ULL << 12)
/* Up to 34 untextured triangles share one validated state envelope. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_BATCH	(1ULL << 13)
/* Two level-zero textures can be multiplied RGBA-by-RGBA. */
#define DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE	(1ULL << 14)
/* D16 draw flags can select a validated depth comparison function. */
#define DRM_CHROME9_CAP_DEPTH_FUNC	(1ULL << 15)
/* One level-zero texture can be combined with linear D16 test/write. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16	(1ULL << 16)
/* Textured D16 draws can retain alpha in an A8R8G8B8 destination. */
#define DRM_CHROME9_CAP_TEXTURED_D16_BGRA8888	(1ULL << 17)
/* Textured D16 draws can use one canonical source-over blend equation. */
#define DRM_CHROME9_CAP_TEXTURED_D16_SOURCE_OVER	(1ULL << 18)
/* Untextured D16 batches preserve RHW for perspective-correct diffuse. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_BATCH	(1ULL << 19)
/* Two level-zero textures can be multiplied with D16 test/write. */
#define DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16	(1ULL << 20)
/* One guarded mip chain can be sampled by a state-complete command list. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_MIP	(1ULL << 21)
/* Up to 544 perspective D16 triangles share one state-complete command. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_RUN	(1ULL << 22)
/* Every D16 draw can test depth without updating the depth surface. */
#define DRM_CHROME9_CAP_DEPTH_READ_ONLY	(1ULL << 23)
/* One texture, D16 and the complete normalized blend state can coexist. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND	(1ULL << 27)
/* Two textures, D16 and complete normalized blend state can coexist. */
#define DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND	(1ULL << 28)
/* Blend state accepts the proven source/destination color/alpha factors. */
#define DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS	(1ULL << 29)
/* Up to 24 one-texture triangles share one state-complete command list. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_RUN	(1ULL << 30)
/* D16 draw flags additionally accept the live-proven EQUAL comparison. */
#define DRM_CHROME9_CAP_DEPTH_EQUAL	(1ULL << 32)
/* Both discontinuous banks provide 32 live-proven PS float vec4 constants. */
#define DRM_CHROME9_CAP_FRAGMENT_CONSTANTS_32	(1ULL << 34)
/* D16 draw flags additionally accept the live-proven ALWAYS comparison. */
#define DRM_CHROME9_CAP_DEPTH_ALWAYS	(1ULL << 35)
/* Two-texture programs can preserve tex0*primary alpha when tex1 has alpha. */
#define DRM_CHROME9_CAP_TWO_TEXTURE_TEX0_ALPHA_PRIMARY	(1ULL << 39)
/* REPEAT two-texture coordinates accept finite values through +/-32. */
#define DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS	(1ULL << 40)
/* One compiler-generated MOV/MUL fragment instruction with D16 RHW runs. */
#define DRM_CHROME9_CAP_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH	(1ULL << 41)
/* COPY_RECT can preserve destination A/X while copying BGR source bytes. */
#define DRM_CHROME9_CAP_COPY_RECT_PRESERVE_ALPHA	(1ULL << 42)
/* Up to 16 one-texture triangles share one validated state envelope. */
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BATCH	(1ULL << 43)
/* Same-format, linear 32-bpp SF rectangles can use bounded H6 GXcopy. */
#define DRM_CHROME9_CAP_COPY_RECT	(1ULL << 44)
/* COPY_RECT may target a strictly sized offset-zero scanout allocation. */
#define DRM_CHROME9_CAP_COPY_RECT_UNGUARDED_DESTINATION	(1ULL << 45)
/* GEM_CREATE can keep scanout pages directly addressable by the 32-bit GPU. */
#define DRM_CHROME9_CAP_GEM_DMA32_SCANOUT	(1ULL << 46)
/* One immutable RHW draw is internally split into proven 544-triangle runs. */
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_CHAIN	(1ULL << 47)
/* The structured one-instruction pixel program accepts ADD(v0,c0). */
#define DRM_CHROME9_CAP_FRAGMENT_ADD			(1ULL << 48)
/* Kernel-compiled, straight-line semantic fragment programs are available. */
#define DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM		(1ULL << 49)
/* Generic fragment textures accept A8, L8, L8A8, and RGB565 storage. */
#define DRM_CHROME9_CAP_FRAGMENT_TEXTURE_LEGACY_FORMATS	(1ULL << 50)
#define DRM_CHROME9_CAP_FRAGMENT_KILL			(1ULL << 51)
/* Fragment ALU may read the native pixel-position MISC source in XY. */
#define DRM_CHROME9_CAP_FRAGMENT_POSITION		(1ULL << 52)
/* Fragment L0 may target a guarded BO with additional trailing mip storage. */
#define DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED	(1ULL << 53)
/* Generic fragment programs may read normalized vertex primary color D1. */
#define DRM_CHROME9_CAP_FRAGMENT_PRIMARY		(1ULL << 54)
/* Generic fragment descriptors may name sparse logical texture units. */
#define DRM_CHROME9_CAP_FRAGMENT_SPARSE_TEXTURES	(1ULL << 55)
/* The programmable FE writes all eight compact T inputs in A01--A03;
 * structured draws accept eight descriptor slots independently of TEX count.
 */
#define DRM_CHROME9_CAP_FRAGMENT_EXTENDED_LINKAGE	(1ULL << 56)
/* Fragment draws accept resolved per-winding stencil state with X8Z24. */
#define DRM_CHROME9_CAP_FRAGMENT_STENCIL	(1ULL << 57)
/* DRAW_VERTEX_PROGRAM with immediate vertices (stage A). */
#define DRM_CHROME9_CAP_VERTEX_PROGRAM		(1ULL << 58)
/* DRAW_VERTEX_PROGRAM with GEM vertex buffers and indices (stage B). */
#define DRM_CHROME9_CAP_VERTEX_BUFFERS		(1ULL << 59)
/* DRAW_VERTEX_PROGRAM BACK_PRIMARY destination and BFC_* flags. */
#define DRM_CHROME9_CAP_VERTEX_TWO_SIDE		(1ULL << 60)
/* DRAW_VERTEX_PROGRAM CLIP_DISTANCE destination and clip_distance_mask. */
#define DRM_CHROME9_CAP_VERTEX_CLIP_DISTANCE	(1ULL << 61)
/* GEM_CPU_ACCESS: explicit CPU access bracketing defers cache flushes. */
#define DRM_CHROME9_CAP_GEM_CPU_ACCESS		(1ULL << 62)
/* DRAW_VERTEX_PROGRAMS: several TCL draws per submission. */
#define DRM_CHROME9_CAP_VERTEX_BATCH		(1ULL << 63)

/* Second capability word, DRM_CHROME9_PARAM_CAPABILITIES2. */
/* GEM_CREATE_GUARDED_SHARED and GEM_INFO. */
#define DRM_CHROME9_CAP2_GUARDED_SHARED		(1ULL << 0)
/* Fragment texture target CUBE: six faces in one object, face_stride. */
#define DRM_CHROME9_CAP2_CUBE_MAP		(1ULL << 1)
/* Fragment texture formats DXT1_RGB..DXT5, level zero. */
#define DRM_CHROME9_CAP2_DXT			(1ULL << 2)
/* DXT mip chains of POT textures, levels packed at whole block rows. */
#define DRM_CHROME9_CAP2_DXT_MIP		(1ULL << 3)
/* DRAW_VERTEX_PROGRAM topology TRIANGLE_STRIP and TRIANGLE_FAN. */
#define DRM_CHROME9_CAP2_VERTEX_TOPOLOGY		(1ULL << 5)
/* DRM_CHROME9_QUERY occlusion queries and DRM_CHROME9_FRAGMENT_QUERY. */
#define DRM_CHROME9_CAP2_OCCLUSION_QUERY	(1ULL << 6)
/* Fragment texture target 3D: POT volume, one level, no mip filter. */
#define DRM_CHROME9_CAP2_TEXTURE_3D		(1ULL << 7)
/* DRAW_VERTEX_PROGRAM REP/ENDREP and IF/ELSE/ENDIF on constants,
 * bool_constants and int_constants_ptr; nesting depth one. */
#define DRM_CHROME9_CAP2_VERTEX_STATIC_FLOW	(1ULL << 8)
/* DRAW_VERTEX_PROGRAM topology POINT_LIST: point_size, POINT_SIZE and
 * RASTER_POINT_SPRITE. */
#define DRM_CHROME9_CAP2_NATIVE_POINTS		(1ULL << 9)
/* DRAW_VERTEX_PROGRAM topology LINE_LIST and LINE_STRIP, one pixel wide. */
#define DRM_CHROME9_CAP2_NATIVE_LINES		(1ULL << 10)
/* DRAW_VERTEX_PROGRAM RASTER_POLYGON_CW/CCW modes for triangles. */
#define DRM_CHROME9_CAP2_POLYGON_MODE		(1ULL << 11)
/* GEM_CPU_ACCESS NOWAIT. */
#define DRM_CHROME9_CAP2_GEM_CPU_NOWAIT		(1ULL << 12)

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

/*
 * Explicit CPU access.  After EXPLICIT, the kernel flushes the object's CPU
 * cache lines before device use only if a PREPARE_WRITE came after the
 * previous device use, and invalidates them after device writes only at the
 * next PREPARE_READ/PREPARE_WRITE.  Userspace must call PREPARE_* before
 * every CPU access through its mapping, and IMPLICIT before sharing the
 * object with a client that does not follow this protocol.  Scanout reads
 * snoop the CPU caches and need no announcement.
 */
#define DRM_CHROME9_GEM_CPU_EXPLICIT		(1U << 0)
#define DRM_CHROME9_GEM_CPU_IMPLICIT		(1U << 1)
#define DRM_CHROME9_GEM_CPU_PREPARE_READ	(1U << 2)
#define DRM_CHROME9_GEM_CPU_PREPARE_WRITE	(1U << 3)
/* Only wait until the device no longer uses the object. */
#define DRM_CHROME9_GEM_CPU_WAIT		(1U << 4)
/*
 * With PREPARE_WRITE only: record the written range without waiting for
 * the device, for writes to bytes no queued job uses (unsynchronized
 * maps).  Needs DRM_CHROME9_CAP2_GEM_CPU_NOWAIT.
 */
#define DRM_CHROME9_GEM_CPU_NOWAIT		(1U << 5)

struct drm_chrome9_gem_cpu_access {
	__u32 handle;
	__u32 flags;
	/* Byte range PREPARE_* covers; size 0 is the whole object. */
	__u64 offset;
	__u64 size;
};

/*
 * Allocate native pages below 4 GiB and reject the object if the DMA mapping
 * nevertheless uses SWIOTLB.  This is intended only for exported scanout
 * buffers: ordinary render resources should not consume constrained memory.
 */
#define DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT	(1U << 0)
/*
 * The object keeps the guarded layout of draw targets (guard page, rows from
 * byte PAGE_SIZE, trailing guard page), but a layout offset of 0 given by a
 * client that cannot carry plane offsets (DRI3 without modifiers, KMS AddFB)
 * refers to its first row at byte PAGE_SIZE.
 */
#define DRM_CHROME9_GEM_CREATE_GUARDED_SHARED	(1U << 1)

/*
 * Occlusion queries.  The kernel keeps a 64-bit sample accumulator per
 * query and DRM file; draws name an active query with FRAGMENT_QUERY and
 * the kernel adds their Z/stencil pass counts when they retire.  BEGIN
 * zeroes the accumulator, draws of earlier BEGIN/END intervals no longer
 * count.  READ reports available once the query is ended and every
 * attached draw retired; WAIT blocks for that.  A draw lost to an engine
 * error makes READ fail with -EIO until the next BEGIN.
 */
#define DRM_CHROME9_QUERY_OP_CREATE		0
#define DRM_CHROME9_QUERY_OP_DESTROY		1
#define DRM_CHROME9_QUERY_OP_BEGIN		2
#define DRM_CHROME9_QUERY_OP_END		3
#define DRM_CHROME9_QUERY_OP_READ		4
#define DRM_CHROME9_QUERY_READ_WAIT		(1U << 0)
#define DRM_CHROME9_QUERY_TYPE_OCCLUSION	0

struct drm_chrome9_query {
	__u32 op;
	__u32 flags;
	/* Out for CREATE, in otherwise. */
	__u32 id;
	/* CREATE: DRM_CHROME9_QUERY_TYPE_*. */
	__u32 type;
	/* READ: accumulated samples. */
	__u64 result;
	/* READ: 1 when ended and every attached draw retired. */
	__u32 available;
	__u32 pad;
	__u64 reserved[2];
};

/* Creation flags and size of an object, e.g. after PRIME import. */
struct drm_chrome9_gem_info {
	__u32 handle;
	__u32 flags;
	__u64 size;
};

#define DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE	(1U << 0)
#define DRM_CHROME9_FIXED_QUAD_CONSTANT_RED	(1U << 1)
#define DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE	(1U << 2)
#define DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE	(1U << 3)
/* Hardware-VS oracles: compiled VS, viewport rows 256..258, clip-space
 * vertices (W=1 or W=2), one triangle, or pixel vertices through M4x4. */
#define DRM_CHROME9_FIXED_QUAD_VS_ORACLE	(1U << 4)
#define DRM_CHROME9_FIXED_QUAD_VS_W2		(1U << 5)
#define DRM_CHROME9_FIXED_QUAD_VS_HALF		(1U << 6)
#define DRM_CHROME9_FIXED_QUAD_VS_M4X4		(1U << 7)
/* Clip engine (A01 bit 11) with the A40..A48 viewport: a +-4 clip-space
 * quad, or a triangle with one vertex behind the eye.  Only the guards are
 * verified in the kernel; userspace compares the coverage. */
#define DRM_CHROME9_FIXED_QUAD_VS_CLIP_WIDE	(1U << 8)
#define DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR	(1U << 9)
#define DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL	(1U << 10)
/* The W=1 quad fetched from a fourth page of a 4-page object through
 * per-element stream slots and inline 16-bit indices. */
#define DRM_CHROME9_FIXED_QUAD_VS_FETCH		(1U << 11)

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

#define DRM_CHROME9_SCISSOR_ENABLE	(1U << 0)

/* Half-open [min,max) pixel coordinates; zero flags disable scissoring. */
struct drm_chrome9_scissor_state {
	__u32 flags;
	__u16 min_x;
	__u16 max_x;
	__u16 min_y;
	__u16 max_y;
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
	struct drm_chrome9_scissor_state scissor;
	__u32 reserved;
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
	struct drm_chrome9_scissor_state scissor;
	__u32 reserved;
};

#define DRM_CHROME9_TEXTURE_PROGRAM_REPLACE	0
#define DRM_CHROME9_TEXTURE_PROGRAM_MODULATE	1

#define DRM_CHROME9_TEXTURE_FORMAT_BGRX8888	0
#define DRM_CHROME9_TEXTURE_FORMAT_BGRA8888	1

#define DRM_CHROME9_TEXTURE_FILTER_NEAREST	0
#define DRM_CHROME9_TEXTURE_FILTER_LINEAR	1

#define DRM_CHROME9_TEXTURE_MIP_FILTER_NONE	0
#define DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST	1
#define DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR	2

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

/* One source of the fixed two-texture RGBA MODULATE program. */
struct drm_chrome9_texture_state {
	__u32 handle;
	__u32 width;
	__u32 height;
	__u32 pitch;
	__u32 format;
	__u32 min_filter;
	__u32 mag_filter;
	__u32 wrap_s;
	__u32 wrap_t;
};

/* D3 and D4 are independent normalized coordinates for texture units 0/1. */
struct drm_chrome9_two_textured_vertex {
	__s32 x_16_16;
	__s32 y_16_16;
	__s32 z_16_16;
	__u32 rhw_float;
	__u32 color;
	__u32 specular;
	__u32 s0_float;
	__u32 t0_float;
	__u32 s1_float;
	__u32 t1_float;
};

/*
 * Draw one triangle with the fixed GL1 lightmap operation
 * texture0.rgba * texture1.rgba.  The destination and both level-zero,
 * linear texture sources are distinct native guarded shmem BOs.  This first
 * ABI intentionally excludes depth, blending, mipmaps and programmable
 * combine state.
 */
struct drm_chrome9_draw_two_textured_triangle {
	__u32 destination_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 reserved0;
	struct drm_chrome9_texture_state textures[2];
	struct drm_chrome9_two_textured_vertex vertices[3];
	struct drm_chrome9_scissor_state scissor;
	__u32 reserved[3];
};

/*
 * Draw one triangle with texture0.rgba * texture1.rgba and linear D16
 * test/write.  The destination, both guarded level-zero sources, and depth
 * surface are four distinct native shmem BOs.  This first successor accepts
 * only an X8R8G8B8 destination, LESS/LEQUAL, and no blend or alpha test.
 */
struct drm_chrome9_draw_two_textured_triangle_d16 {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 reserved0;
	struct drm_chrome9_texture_state textures[2];
	struct drm_chrome9_two_textured_vertex vertices[3];
	struct drm_chrome9_scissor_state scissor;
	__u32 reserved[5];
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
	struct drm_chrome9_scissor_state scissor;
	__u32 reserved;
};

/*
 * Draw one single-textured triangle with linear D16 test/write.  This
 * successor is deliberately narrow: X8R8G8B8 or A8R8G8B8 destination, one
 * level-zero 32-bit texture, optional canonical source-over, no alpha test,
 * and an explicit LESS or LEQUAL comparison in flags.  All three BOs are
 * distinct native guarded shmem objects.  The depth pitch is
 * ALIGN(width * 2, 32).
 */
struct drm_chrome9_draw_textured_triangle_d16 {
	__u32 destination_handle;
	__u32 texture_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 program;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 texture_width;
	__u32 texture_height;
	__u32 texture_pitch;
	__u32 texture_format;
	__u32 min_filter;
	__u32 mag_filter;
	__u32 wrap_s;
	__u32 wrap_t;
	struct drm_chrome9_textured_vertex vertices[3];
	struct drm_chrome9_scissor_state scissor;
	__u32 destination_format;
	__u32 reserved[3];
};

#define DRM_CHROME9_TEXTURED_D16_SOURCE_OVER	(1U << 0)

#define DRM_CHROME9_BLEND_ENABLE		(1U << 0)
#define DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE	(1U << 1)

#define DRM_CHROME9_BLEND_FACTOR_ZERO		0
#define DRM_CHROME9_BLEND_FACTOR_ONE		1
#define DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA	2
#define DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA	3
#define DRM_CHROME9_BLEND_FACTOR_SRC_COLOR	4
#define DRM_CHROME9_BLEND_FACTOR_INV_SRC_COLOR	5
#define DRM_CHROME9_BLEND_FACTOR_DST_ALPHA	6
#define DRM_CHROME9_BLEND_FACTOR_INV_DST_ALPHA	7
#define DRM_CHROME9_BLEND_FACTOR_DST_COLOR	8
#define DRM_CHROME9_BLEND_FACTOR_INV_DST_COLOR	9
#define DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE	10

#define DRM_CHROME9_BLEND_FUNC_ADD	0

#define DRM_CHROME9_COMPARE_NEVER	0
#define DRM_CHROME9_COMPARE_LESS		1
#define DRM_CHROME9_COMPARE_EQUAL	2
#define DRM_CHROME9_COMPARE_LEQUAL	3
#define DRM_CHROME9_COMPARE_GREATER	4
#define DRM_CHROME9_COMPARE_NOTEQUAL	5
#define DRM_CHROME9_COMPARE_GEQUAL	6
#define DRM_CHROME9_COMPARE_ALWAYS	7

/*
 * D16 draw flag field.  A zero field retains the original ABI's implicit
 * LESS comparison.  Kernels advertise DRM_CHROME9_CAP_DEPTH_FUNC before
 * accepting explicit values; the first bounded implementation accepts only
 * LESS and LEQUAL.
 */
#define DRM_CHROME9_DEPTH_FUNC_SHIFT	8
#define DRM_CHROME9_DEPTH_FUNC_MASK	(7U << DRM_CHROME9_DEPTH_FUNC_SHIFT)
#define DRM_CHROME9_DEPTH_FUNC(func) \
	((__u32)(func) << DRM_CHROME9_DEPTH_FUNC_SHIFT)
#define DRM_CHROME9_DEPTH_READ_ONLY	(1U << 11)

/* Select RGB=tex0*tex1*primary, A=tex0*primary for two-texture draws. */
#define DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY	(1U << 1)

#define DRM_CHROME9_COLOR_MASK_R	(1U << 0)
#define DRM_CHROME9_COLOR_MASK_G	(1U << 1)
#define DRM_CHROME9_COLOR_MASK_B	(1U << 2)
#define DRM_CHROME9_COLOR_MASK_A	(1U << 3)

#define DRM_CHROME9_DESTINATION_FORMAT_BGRX8888	0
#define DRM_CHROME9_DESTINATION_FORMAT_BGRA8888	1
#define DRM_CHROME9_DESTINATION_FORMAT_RGB565	2
#define DRM_CHROME9_DESTINATION_FORMAT_L8A8	3
#define DRM_CHROME9_DESTINATION_FORMAT_ARGB1555	4
#define DRM_CHROME9_DESTINATION_FORMAT_ARGB4444	5

struct drm_chrome9_blend_state {
	__u32 flags;
	__u32 rgb_src_factor;
	__u32 rgb_dst_factor;
	__u32 alpha_src_factor;
	__u32 alpha_dst_factor;
	__u32 rgb_func;
	__u32 alpha_func;
	__u32 color_mask;
	__u32 alpha_test_func;
	__u32 alpha_test_ref;	/* normalized byte, 0..255 */
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
	struct drm_chrome9_scissor_state scissor;
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
	struct drm_chrome9_scissor_state scissor;
};

/* State-complete one-texture D16 blend and alpha-test successor. */
struct drm_chrome9_draw_textured_triangle_d16_blend {
	__u32 destination_handle;
	__u32 texture_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 program;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 texture_width;
	__u32 texture_height;
	__u32 texture_pitch;
	__u32 texture_format;
	__u32 min_filter;
	__u32 mag_filter;
	__u32 wrap_s;
	__u32 wrap_t;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_textured_vertex vertices[3];
	struct drm_chrome9_scissor_state scissor;
	__u32 destination_format;
	__u32 reserved[3];
};

/* State-complete two-texture D16 blend and alpha-test successor. */
struct drm_chrome9_draw_two_textured_triangle_d16_blend {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 reserved0;
	struct drm_chrome9_texture_state textures[2];
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_two_textured_vertex vertices[3];
	struct drm_chrome9_scissor_state scissor;
	__u32 destination_format;
	__u32 reserved[4];
};

/*
 * State-complete untextured D16 successor.  Depth remains the live-proven
 * linear D16 LESS comparison with writes enabled; blend and alpha-test state
 * use the same normalized lowering as the color-only successor.
 */
struct drm_chrome9_draw_triangle_d16_blend {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 pad;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_vertex_d16 vertices[3];
	__u32 destination_format;
	struct drm_chrome9_scissor_state scissor;
	__u32 reserved[2];
};

#define DRM_CHROME9_TRIANGLE_BATCH_DEPTH	(1U << 0)
#define DRM_CHROME9_TRIANGLE_BATCH_BLEND	(1U << 1)
#define DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES	34U
#define DRM_CHROME9_TEXTURED_TRIANGLE_BATCH_MAX_TRIANGLES	16U
#define DRM_CHROME9_TEXTURE_RUN_DEPTH	(1U << 0)
#define DRM_CHROME9_TEXTURE_RUN_BLEND	(1U << 1)
#define DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES	24U

/*
 * Submit a triangle list under one state envelope.  vertices_ptr addresses
 * triangle_count * 3 drm_chrome9_vertex_d16 records.  Z is ignored unless
 * DEPTH is set.  This remains a structured, kernel-generated command ABI;
 * userspace never supplies native packets.
 */
struct drm_chrome9_draw_triangle_batch {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

/*
 * Submit a one-texture triangle list under one immutable state envelope.
 * vertices_ptr addresses triangle_count * 3 drm_chrome9_textured_vertex
 * records.  The command retains the original level-zero, linear texture
 * contract and optionally enables the normalized blend/alpha-test state.
 */
struct drm_chrome9_draw_textured_triangle_batch {
	__u32 destination_handle;
	__u32 texture_handle;
	__u32 flags;
	__u32 triangle_count;
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
	__u32 destination_format;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

/*
 * State-complete one-texture list successor.  vertices_ptr addresses
 * triangle_count * 3 drm_chrome9_textured_vertex records.  DEPTH and BLEND
 * select their closed state envelopes; all native packet words remain
 * kernel-generated.
 */
struct drm_chrome9_draw_textured_triangle_run {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 program;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	struct drm_chrome9_texture_state texture;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

/*
 * Mipmapped one-texture list successor.  The kernel derives every level
 * offset and native table word from the validated guarded linear layout.
 * Userspace supplies only the semantic last level, filter, and signed s5.5
 * bias; DEPTH and BLEND retain the state-complete RUN meanings.
 */
struct drm_chrome9_draw_textured_triangle_mip {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 program;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	struct drm_chrome9_texture_state texture;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 texture_last_level;
	__u32 mip_filter;
	__s32 lod_bias_s5_5;
	__u32 reserved0;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

#define DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE	(1U << 0)
#define DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED	(1U << 1)
#define DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA	(1U << 2)

/*
 * Copy one integer rectangle between distinct linear SF objects.  Source is
 * guarded; destination is guarded unless DESTINATION_UNGUARDED is selected.
 * Source and destination use the same 32-bpp bit significance; no blend,
 * conversion, keying, scaling, tiling, clipping, or overlap is permitted.
 * FORCE_ALPHA_ONE and PRESERVE_DESTINATION_ALPHA are mutually exclusive.
 */
struct drm_chrome9_copy_rect {
	__u32 destination_handle;
	__u32 source_handle;
	__u32 flags;
	__u32 destination_x;
	__u32 destination_y;
	__u32 source_x;
	__u32 source_y;
	__u32 width;
	__u32 height;
	__u32 destination_width;
	__u32 destination_height;
	__u32 destination_pitch;
	__u32 source_width;
	__u32 source_height;
	__u32 source_pitch;
	__u32 format;
	__u32 reserved[8];
};

/*
 * Perspective-correct untextured D16 vertex.  rhw_float is the exact
 * IEEE-754 post-viewport reciprocal W; it must be finite and positive.
 */
struct drm_chrome9_vertex_d16_rhw {
	__s32 x_16_16;
	__s32 y_16_16;
	__s32 z_16_16;
	__u32 rhw_float;
	__u32 color;
};

/*
 * D16-only successor to DRAW_TRIANGLE_BATCH.  The state envelope remains
 * fixed-size, but vertices_ptr addresses triangle_count * 3
 * drm_chrome9_vertex_d16_rhw records.  DEPTH must be set; the old batch ABI
 * and vertex record remain unchanged.
 */
struct drm_chrome9_draw_triangle_d16_rhw_batch {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

#define DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES	544U

/*
 * Large-list successor to DRAW_TRIANGLE_D16_RHW_BATCH.  It is byte-identical
 * only in version one so that each command retains an immutable count limit:
 * the old batch remains capped at 34 while this request accepts up to 544
 * post-viewport RHW triangles under one complete state envelope.
 */
struct drm_chrome9_draw_triangle_d16_rhw_run {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

#define DRM_CHROME9_TRIANGLE_CHAIN_MAX_TRIANGLES	16384U

/*
 * State-complete successor to DRAW_TRIANGLE_D16_RHW_RUN.  The kernel copies
 * and validates the complete immutable vertex array before taking any GEM
 * reservation or submitting work, then emits consecutive packets containing
 * at most TRIANGLE_RUN_MAX triangles while retaining one state envelope,
 * mapping lifetime and fence.  The request layout is deliberately distinct
 * even though version one has the same fields as the run request.
 */
struct drm_chrome9_draw_triangle_d16_rhw_chain {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	__u64 reserved[3];
};

/*
 * Version-one structured fragment instruction.  This is deliberately not a
 * native packet ABI: the kernel validates these logical operands and rebuilds
 * the four native instruction words.  The first executable subset writes all
 * four oC0 channels with one MOV, MUL, or capability-gated ADD and exposes
 * only the primary varying v0 and one job-local float constant c0.
 */
#define DRM_CHROME9_PS_OP_MOV		0
#define DRM_CHROME9_PS_OP_MUL		1
#define DRM_CHROME9_PS_OP_ADD		2

#define DRM_CHROME9_PS_SOURCE_VARYING	0
#define DRM_CHROME9_PS_SOURCE_CONSTANT	1

#define DRM_CHROME9_PS_SWIZZLE_X	0
#define DRM_CHROME9_PS_SWIZZLE_Y	1
#define DRM_CHROME9_PS_SWIZZLE_Z	2
#define DRM_CHROME9_PS_SWIZZLE_W	3
#define DRM_CHROME9_PS_SWIZZLE_ONE	4
#define DRM_CHROME9_PS_SWIZZLE_ZERO	5

struct drm_chrome9_ps_source {
	__u8 file;
	__u8 index;
	__u8 swizzle[4];
	__u8 modifier;
	__u8 reserved;
};

struct drm_chrome9_ps_instruction {
	__u32 op;
	__u32 reserved;
	struct drm_chrome9_ps_source src[2];
};

/*
 * Compiler-backed successor to DRAW_TRIANGLE_D16_RHW_RUN.  DEPTH is
 * mandatory and vertices_ptr addresses up to TRIANGLE_RUN_MAX perspective-
 * correct records.
 * constant_c0 contains the exact IEEE-754 words uploaded to fragment c0.
 * Version one accepts identity swizzles and requires modifier zero.  MUL and
 * ADD must pair v0 with c0.  The remaining defined swizzle values reserve
 * stable encodings for later independently gated successors.
 */
struct drm_chrome9_draw_program_triangle_d16_rhw_batch {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	struct drm_chrome9_ps_instruction instruction;
	__u32 constant_c0[4];
};

/*
 * Generic fragment-program ABI.  Userspace submits only semantic records;
 * native instructions, resource controls, scheduling maps, and packet words
 * are generated by the kernel.  Version one is straight-line and SSA-only.
 */
#define DRM_CHROME9_FRAGMENT_MAX_ALU		96U
#define DRM_CHROME9_FRAGMENT_MAX_TAU		32U
#define DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS	128U
#define DRM_CHROME9_FRAGMENT_MAX_CONSTANTS	32U
#define DRM_CHROME9_FRAGMENT_MAX_TEXTURES	8U
#define DRM_CHROME9_FRAGMENT_MAX_VARYINGS	8U
#define DRM_CHROME9_FRAGMENT_MAX_TRIANGLES	128U
#define DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS	12U
#define DRM_CHROME9_FRAGMENT_ABI_VERSION		1U
#define DRM_CHROME9_FRAGMENT_ABI_SPARSE_TEXTURES	2U

#define DRM_CHROME9_FRAGMENT_DEPTH	(1U << 0)
#define DRM_CHROME9_FRAGMENT_BLEND	(1U << 1)
/* Allow a guarded L0 target to contain trailing mip storage. */
#define DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED	(1U << 2)
/* Apply the stencil block; requires DEPTH with the X8Z24 (D24S8) format. */
#define DRM_CHROME9_FRAGMENT_STENCIL	(1U << 3)
/* Count the draw's passing samples into the active query named by query. */
#define DRM_CHROME9_FRAGMENT_QUERY	(1U << 4)

/* Native stencil operations; note INVERT precedes the wrapping forms. */
#define DRM_CHROME9_STENCIL_OP_KEEP		0
#define DRM_CHROME9_STENCIL_OP_ZERO		1
#define DRM_CHROME9_STENCIL_OP_REPLACE		2
#define DRM_CHROME9_STENCIL_OP_INCR_SAT		3
#define DRM_CHROME9_STENCIL_OP_DECR_SAT		4
#define DRM_CHROME9_STENCIL_OP_INVERT		5
#define DRM_CHROME9_STENCIL_OP_INCR_WRAP	6
#define DRM_CHROME9_STENCIL_OP_DECR_WRAP	7

enum drm_chrome9_fragment_op {
	DRM_CHROME9_FRAGMENT_OP_MOV = 0,
	DRM_CHROME9_FRAGMENT_OP_MUL = 1,
	DRM_CHROME9_FRAGMENT_OP_ADD = 2,
	DRM_CHROME9_FRAGMENT_OP_SUB = 3,
	DRM_CHROME9_FRAGMENT_OP_MAD = 4,
	DRM_CHROME9_FRAGMENT_OP_MIN = 5,
	DRM_CHROME9_FRAGMENT_OP_MAX = 6,
	DRM_CHROME9_FRAGMENT_OP_DP3 = 7,
	DRM_CHROME9_FRAGMENT_OP_DP4 = 8,
	DRM_CHROME9_FRAGMENT_OP_EXP2 = 9,
	DRM_CHROME9_FRAGMENT_OP_LOG2 = 10,
	DRM_CHROME9_FRAGMENT_OP_RCP = 11,
	DRM_CHROME9_FRAGMENT_OP_RSQ = 12,
	DRM_CHROME9_FRAGMENT_OP_ABS = 13,
	DRM_CHROME9_FRAGMENT_OP_CMP = 14,
	DRM_CHROME9_FRAGMENT_OP_FRC = 15,
	DRM_CHROME9_FRAGMENT_OP_TEX = 16,
	/* Destination-free, identity-XYZW TEMP predicate; one per program. */
	DRM_CHROME9_FRAGMENT_OP_KILL = 17,
};

enum drm_chrome9_fragment_file {
	DRM_CHROME9_FRAGMENT_FILE_TEMP = 0,
	DRM_CHROME9_FRAGMENT_FILE_VARYING = 1,
	DRM_CHROME9_FRAGMENT_FILE_CONSTANT = 2,
	/* Native fragment position, not a vertex varying; index must be zero. */
	DRM_CHROME9_FRAGMENT_FILE_POSITION = 3,
	/* Normalized vertex primary color D1, native v0; index must be zero. */
	DRM_CHROME9_FRAGMENT_FILE_PRIMARY = 4,
};

#define DRM_CHROME9_FRAGMENT_DEST_TEMP	0
#define DRM_CHROME9_FRAGMENT_DEST_COLOR	1
#define DRM_CHROME9_FRAGMENT_MOD_NONE	0
#define DRM_CHROME9_FRAGMENT_MOD_NEG	1
#define DRM_CHROME9_FRAGMENT_DEST_SAT	(1U << 0)

#define DRM_CHROME9_FRAGMENT_SWIZZLE_X	0
#define DRM_CHROME9_FRAGMENT_SWIZZLE_Y	1
#define DRM_CHROME9_FRAGMENT_SWIZZLE_Z	2
#define DRM_CHROME9_FRAGMENT_SWIZZLE_W	3
#define DRM_CHROME9_FRAGMENT_SWIZZLE_ONE	4
#define DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO	5

#define DRM_CHROME9_FRAGMENT_DEPTH_D16	0
#define DRM_CHROME9_FRAGMENT_DEPTH_X8Z24	1

#define DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888	0
#define DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888	1
#define DRM_CHROME9_FRAGMENT_TEXTURE_A8		2
#define DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24	3
#define DRM_CHROME9_FRAGMENT_TEXTURE_L8		4
#define DRM_CHROME9_FRAGMENT_TEXTURE_L8A8	5
#define DRM_CHROME9_FRAGMENT_TEXTURE_RGB565	6
/*
 * S3TC 4x4 blocks: width/height stay in texels, level_pitch is the byte
 * pitch of one block row, ALIGN(DIV_ROUND_UP(width, 4) * block, 32).
 * DXT1_RGB and DXT1_RGBA sample identically (punch-through alpha).
 */
#define DRM_CHROME9_FRAGMENT_TEXTURE_DXT1_RGB	7
#define DRM_CHROME9_FRAGMENT_TEXTURE_DXT1_RGBA	8
#define DRM_CHROME9_FRAGMENT_TEXTURE_DXT3	9
#define DRM_CHROME9_FRAGMENT_TEXTURE_DXT5	10

#define DRM_CHROME9_FRAGMENT_TARGET_2D		0
/* Square POT, faces +X -X +Y -Y +Z -Z, TEX reads three coordinate lanes. */
#define DRM_CHROME9_FRAGMENT_TARGET_CUBE	1
/* POT volume, level zero only, slices at face_stride; TEX reads STR. */
#define DRM_CHROME9_FRAGMENT_TARGET_3D		2

struct drm_chrome9_fragment_source {
	__u8 file;
	__u8 index;
	__u8 swizzle[4];
	__u8 modifier;
	__u8 reserved;
};

struct drm_chrome9_fragment_destination {
	__u8 file;
	__u8 index;
	__u8 writemask;
	__u8 modifier;
};

/* Fixed-width 32-byte semantic record.  sampler is used only by TEX. */
struct drm_chrome9_fragment_instruction {
	__u8 op;
	__u8 sampler;
	__u16 reserved;
	struct drm_chrome9_fragment_destination dst;
	struct drm_chrome9_fragment_source src[3];
};

/*
 * Post-viewport RHW vertex.  T0..T7 stay IEEE-754 float32 throughout the
 * command path; color/specular retain the proven normalized D3DCOLOR links.
 */
struct drm_chrome9_fragment_vertex {
	__s32 x_16_16;
	__s32 y_16_16;
	__s32 z_16_16;
	__u32 rhw_float;
	__u32 color;
	__u32 specular;
	__u32 varying[DRM_CHROME9_FRAGMENT_MAX_VARYINGS][4];
};

/*
 * Complete packed-linear texture layout.  Active levels carry explicit byte
 * offsets and pitches; the kernel validates every level against the BO.
 */
struct drm_chrome9_fragment_texture_state {
	__u32 handle;
	__u32 format;
	__u32 width;
	__u32 height;
	__u32 first_level;
	__u32 last_level;
	__u32 min_filter;
	__u32 mag_filter;
	__u32 mip_filter;
	__u32 wrap_s;
	__u32 wrap_t;
	__s32 lod_bias_s5_5;
	__s32 min_lod_s5_5;
	__s32 max_lod_s5_5;
	__u32 border_color;
	/* ABI v1: zero.  ABI v2: logical texture unit, 0..7. */
	__u32 reserved0;
	__u32 level_offset[DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS];
	__u32 level_pitch[DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS];
	/* DRM_CHROME9_FRAGMENT_TARGET_*; levels describe face 0 of a cube. */
	__u32 target;
	/*
	 * CUBE: byte distance between equal levels of adjacent faces.
	 * 3D: byte distance between slices, level_pitch[0] * height.
	 */
	__u32 face_stride;
	/* 3D: number of slices; zero otherwise. */
	__u16 depth;
	/* 3D: DRM_CHROME9_TEXTURE_WRAP_* of the R coordinate; zero otherwise. */
	__u16 wrap_r;
	__u32 reserved[1];
};

/* One stencil face; func is a DRM_CHROME9_COMPARE_* value. */
struct drm_chrome9_stencil_face {
	__u8 func;
	__u8 fail_op;
	__u8 zfail_op;
	__u8 zpass_op;
	__u8 ref;
	__u8 value_mask;
	__u8 write_mask;
	__u8 pad;
};

/*
 * Faces are resolved to screen-space winding by userspace: cw applies to
 * clockwise triangles in window coordinates, ccw to counter-clockwise ones.
 */
struct drm_chrome9_stencil_state {
	struct drm_chrome9_stencil_face cw;
	struct drm_chrome9_stencil_face ccw;
};

/*
 * All pointers address immutable arrays copied before GEM reservation.  The
 * constants array contains constant_count vec4s; textures contains
 * texture_count states; instructions and vertices use their explicit counts.
 */
struct drm_chrome9_draw_fragment_program {
	__u32 destination_handle;
	__u32 depth_handle;
	__u32 flags;
	__u32 triangle_count;
	__u32 width;
	__u32 height;
	__u32 destination_pitch;
	__u32 depth_pitch;
	__u32 destination_format;
	__u32 depth_format;
	__u32 instruction_count;
	__u32 constant_count;
	__u32 texture_count;
	__u32 varying_count;
	__u32 abi_version;
	__u32 reserved0;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_scissor_state scissor;
	__u32 pad;
	__u64 vertices_ptr;
	__u64 instructions_ptr;
	__u64 constants_ptr;
	__u64 textures_ptr;
	struct drm_chrome9_stencil_state stencil;
	/* FRAGMENT_QUERY: id of an active DRM_CHROME9_QUERY object. */
	__u32 query;
	__u32 reserved1;
	__u64 reserved[1];
};

#define DRM_CHROME9_VERTEX_ABI_VERSION		1U
#define DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS	256U
/* Native records including fills, position move and finalizer. */
#define DRM_CHROME9_VERTEX_MAX_RECORDS		256U
#define DRM_CHROME9_VERTEX_MAX_TEMPS		255U
#define DRM_CHROME9_VERTEX_MAX_CONSTANTS	256U
#define DRM_CHROME9_VERTEX_MAX_INPUTS		16U
#define DRM_CHROME9_VERTEX_MAX_VERTICES		384U
#define DRM_CHROME9_VERTEX_MAX_INDICES		12288U
/* Draws per DRAW_VERTEX_PROGRAMS call. */
#define DRM_CHROME9_VERTEX_MAX_BATCH		64U
/* Hardware user clip planes: two FE coordinate groups of three lanes. */
#define DRM_CHROME9_VERTEX_MAX_CLIP_DISTANCES	6U
/* Integer (i#) and Boolean (b#) constants of static flow control. */
#define DRM_CHROME9_VERTEX_MAX_INT_CONSTANTS	16U
#define DRM_CHROME9_VERTEX_MAX_BOOL_CONSTANTS	16U

enum drm_chrome9_vertex_op {
	DRM_CHROME9_VERTEX_OP_MOV = 0,
	DRM_CHROME9_VERTEX_OP_ADD = 1,
	DRM_CHROME9_VERTEX_OP_SUB = 2,
	DRM_CHROME9_VERTEX_OP_MUL = 3,
	DRM_CHROME9_VERTEX_OP_MAD = 4,
	DRM_CHROME9_VERTEX_OP_DP3 = 5,
	DRM_CHROME9_VERTEX_OP_DP4 = 6,
	DRM_CHROME9_VERTEX_OP_DPH = 7,
	DRM_CHROME9_VERTEX_OP_MIN = 8,
	DRM_CHROME9_VERTEX_OP_MAX = 9,
	DRM_CHROME9_VERTEX_OP_ABS = 10,
	DRM_CHROME9_VERTEX_OP_SGE = 11,
	DRM_CHROME9_VERTEX_OP_SLT = 12,
	DRM_CHROME9_VERTEX_OP_SGN = 13,
	DRM_CHROME9_VERTEX_OP_FRC = 14,
	DRM_CHROME9_VERTEX_OP_FLR = 15,
	/* Scalar ops: all four source selectors must be equal. */
	DRM_CHROME9_VERTEX_OP_RCP = 16,
	DRM_CHROME9_VERTEX_OP_RSQ = 17,
	DRM_CHROME9_VERTEX_OP_EXP2 = 18,
	DRM_CHROME9_VERTEX_OP_LOG2 = 19,
	/*
	 * Structured static flow control (CAP2_VERTEX_STATIC_FLOW).  REP
	 * runs its body i#.x times (0..255), IF takes its first arm when b#
	 * is set.  Only TEMP destinations and no INPUT sources inside; a
	 * temp may be rewritten there, and after ENDREP/ENDIF it keeps only
	 * the lanes defined on every path.  Unused operands are zero.
	 */
	DRM_CHROME9_VERTEX_OP_REP = 20,		/* src[0] INT, identity swizzle */
	DRM_CHROME9_VERTEX_OP_ENDREP = 21,
	DRM_CHROME9_VERTEX_OP_IF = 22,		/* src[0] BOOL, identity swizzle */
	DRM_CHROME9_VERTEX_OP_ELSE = 23,
	DRM_CHROME9_VERTEX_OP_ENDIF = 24,
	DRM_CHROME9_VERTEX_OP_COUNT
};

enum drm_chrome9_vertex_file {
	/* Virtual SSA temporaries, defined component-wise before any read. */
	DRM_CHROME9_VERTEX_FILE_TEMP = 0,
	/* Dense inputs: index n reads vertex element n. */
	DRM_CHROME9_VERTEX_FILE_INPUT = 1,
	DRM_CHROME9_VERTEX_FILE_CONSTANT = 2,
	/* Flow-control operands: b0..b15 and i0..i15. */
	DRM_CHROME9_VERTEX_FILE_BOOL = 3,
	DRM_CHROME9_VERTEX_FILE_INT = 4,
};

enum drm_chrome9_vertex_dest {
	DRM_CHROME9_VERTEX_DEST_TEMP = 0,
	/* Clip-space position, index zero, every lane written once. */
	DRM_CHROME9_VERTEX_DEST_POSITION = 1,
	/* Saturated primary color; dropped when the fragment ignores it. */
	DRM_CHROME9_VERTEX_DEST_PRIMARY = 2,
	/* Logical fragment varying; every consumed lane must be written,
	 * unconsumed varyings and lanes are dropped. */
	DRM_CHROME9_VERTEX_DEST_VARYING = 3,
	/* Saturated back-face primary color, index zero, every lane written
	 * once; requires a BFC_* flag and is dropped with PRIMARY. */
	DRM_CHROME9_VERTEX_DEST_BACK_PRIMARY = 4,
	/* Clip distance, index = plane, writemask X only; enabled planes in
	 * clip_distance_mask are written once, others are dropped.  Pixels
	 * with a negative interpolated distance are discarded. */
	DRM_CHROME9_VERTEX_DEST_CLIP_DISTANCE = 5,
	/* Point size, index zero, writemask X, no SAT; used only with
	 * RASTER_POINT_SIZE_VERTEX (written once), dropped otherwise. */
	DRM_CHROME9_VERTEX_DEST_POINT_SIZE = 6,
};

/* Swizzles, NEG and SAT use the DRM_CHROME9_FRAGMENT_* values. */
struct drm_chrome9_vertex_source {
	__u8 file;
	__u8 index;
	__u8 swizzle[4];
	__u8 modifier;
	__u8 reserved;
};

struct drm_chrome9_vertex_destination {
	__u8 file;
	__u8 index;
	__u8 writemask;
	__u8 modifier;
};

/* Fixed-width 32-byte semantic record; unused sources are all zero. */
struct drm_chrome9_vertex_instruction {
	__u8 op;
	__u8 reserved0;
	__u16 reserved1;
	struct drm_chrome9_vertex_destination dst;
	struct drm_chrome9_vertex_source src[3];
};

#define DRM_CHROME9_VERTEX_FORMAT_FLOAT1	0
#define DRM_CHROME9_VERTEX_FORMAT_FLOAT2	1
#define DRM_CHROME9_VERTEX_FORMAT_FLOAT3	2
#define DRM_CHROME9_VERTEX_FORMAT_FLOAT4	3
#define DRM_CHROME9_VERTEX_FORMAT_BGRA8_UNORM	4
/* Byte order as in memory: R first.  Missing lanes read (0, 0, 0, 1). */
#define DRM_CHROME9_VERTEX_FORMAT_RGBA8_UNORM	5
#define DRM_CHROME9_VERTEX_FORMAT_RGBA8_USCALED	6
#define DRM_CHROME9_VERTEX_FORMAT_RG16_UNORM	7
#define DRM_CHROME9_VERTEX_FORMAT_RGBA16_UNORM	8
#define DRM_CHROME9_VERTEX_FORMAT_RG16_SNORM	9
#define DRM_CHROME9_VERTEX_FORMAT_RGBA16_SNORM	10
#define DRM_CHROME9_VERTEX_FORMAT_RG16_SSCALED	11
#define DRM_CHROME9_VERTEX_FORMAT_RGBA16_SSCALED	12
#define DRM_CHROME9_VERTEX_FORMAT_RG16_FLOAT	13
#define DRM_CHROME9_VERTEX_FORMAT_RGBA16_FLOAT	14

struct drm_chrome9_vertex_element {
	__u8 format;
	__u8 buffer;
	__u16 reserved;
	__u32 offset;
};

struct drm_chrome9_vertex_buffer {
	__u32 handle;
	__u32 stride;
	__u64 offset;
};

/* Culling uses screen-space winding. */
#define DRM_CHROME9_VERTEX_CULL_CW	(1U << 0)
#define DRM_CHROME9_VERTEX_CULL_CCW	(1U << 1)
#define DRM_CHROME9_VERTEX_FLAT		(1U << 2)
/* Screen winding whose triangles take BACK_PRIMARY instead of PRIMARY. */
#define DRM_CHROME9_VERTEX_BFC_CW	(1U << 3)
#define DRM_CHROME9_VERTEX_BFC_CCW	(1U << 4)

/* Primitive assembly of the vertex or index sequence. */
#define DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_LIST	0U
#define DRM_CHROME9_VERTEX_TOPOLOGY_POINT_LIST		1U
#define DRM_CHROME9_VERTEX_TOPOLOGY_LINE_LIST		2U
#define DRM_CHROME9_VERTEX_TOPOLOGY_LINE_STRIP		3U
#define DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_STRIP	4U
#define DRM_CHROME9_VERTEX_TOPOLOGY_TRIANGLE_FAN	5U

/* raster_flags.  POINT_SPRITE replaces the logical varyings in SPRITE_MASK
 * by (s, t, 0, 1) with (0, 0) at the upper-left corner of the point;
 * masked varyings need no VS write.  POINT_SIZE_VERTEX takes the size from
 * the POINT_SIZE destination, clamped to [1.0, point_size].  Both require
 * POINT_LIST; bit 1 is reserved. */
#define DRM_CHROME9_RASTER_POINT_SPRITE		(1U << 0)
#define DRM_CHROME9_RASTER_POINT_SIZE_VERTEX	(1U << 2)
#define DRM_CHROME9_RASTER_SPRITE_MASK(m)	((__u32)(m) << 8)
/* Polygon mode per screen winding of a triangle topology; a POINT face
 * requires POINT on both windings. */
#define DRM_CHROME9_POLYGON_FILL		0U
#define DRM_CHROME9_POLYGON_LINE		1U
#define DRM_CHROME9_POLYGON_POINT		2U
#define DRM_CHROME9_RASTER_POLYGON_CW(m)	((__u32)(m) << 16)
#define DRM_CHROME9_RASTER_POLYGON_CCW(m)	((__u32)(m) << 18)

/*
 * fragment carries the complete pixel state with vertices_ptr and
 * triangle_count zero.  viewport holds the final float bits
 * sx sy sz ox oy oz 0 0, where ox/oy include the 786432 snap bias.
 * Stage A sends vertex_count raw vertices (in topology order) packed by
 * elements with offsets relative to one vertex.  Stage B (buffer_count
 * non-zero, vertex_count/vertex_stride/vertices_ptr zero) fetches every
 * element from buffers[element.buffer] at offset + element.offset +
 * (index + base_vertex) * stride through index_count 16- or 32-bit
 * indices.  A triangle list takes a multiple of three vertices, a strip
 * or fan at least three, a point list one, a line list a multiple of two
 * and a line strip at least two; only triangle lists take BFC_* flags and
 * only triangles CULL_* flags.  FLAT shades lines from the last vertex.
 * Offsets and strides are dword aligned, strides 0..2044 bytes; stride 0
 * repeats one element for every vertex.  point_size (float bits, 1.0 to
 * 2048.0, zero for 1.0) is non-zero only for draws that rasterize points.
 */
struct drm_chrome9_draw_vertex_program {
	struct drm_chrome9_draw_fragment_program fragment;
	__u32 abi_version;
	__u32 flags;
	__u32 instruction_count;
	__u32 element_count;
	__u32 constant_first;
	__u32 constant_count;
	__u32 vertex_count;
	__u32 vertex_stride;
	__u32 index_count;
	__u32 index_size;
	__u32 buffer_count;
	__s32 base_vertex;
	__u32 viewport[8];
	__u64 instructions_ptr;
	__u64 constants_ptr;
	__u64 elements_ptr;
	__u64 vertices_ptr;
	__u64 indices_ptr;
	__u64 buffers_ptr;
	/* Enabled CLIP_DISTANCE planes, bits below MAX_CLIP_DISTANCES. */
	__u32 clip_distance_mask;
	__u32 topology;		/* DRM_CHROME9_VERTEX_TOPOLOGY_* */
	__u32 point_size;
	__u32 raster_flags;	/* DRM_CHROME9_RASTER_* */
	/* Zero; reserved for line stipple. */
	__u32 reserved;
	/* CAP2_VERTEX_STATIC_FLOW: b0..b15, and i0..i15 as 16 x {x, y, z, w}
	 * __u8 (REP count in x); a zero pointer uploads zeros. */
	__u32 bool_constants;
	__u64 int_constants_ptr;
};

/*
 * Draws executed in order.  Every draw names the same destination, depth
 * buffer and their formats and sizes; at most 64 distinct objects in
 * total (-E2BIG otherwise, submit fewer draws).
 */
struct drm_chrome9_draw_vertex_programs {
	__u32 count;
	__u32 flags;
	__u64 draws_ptr;	/* struct drm_chrome9_draw_vertex_program[count] */
	__u64 reserved[2];
};

#if defined(__cplusplus)
}
#endif

#endif
