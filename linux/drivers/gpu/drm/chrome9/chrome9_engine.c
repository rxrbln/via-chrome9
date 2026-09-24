// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <linux/build_bug.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/vgaarb.h>
#include <linux/vmalloc.h>

#include <video/vga.h>

#ifdef CONFIG_X86
#include <asm/cacheflush.h>
#endif

#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>

#include "chrome9_drv.h"
#include "chrome9_engine.h"
#include "chrome9_fragment.h"
#include "chrome9_vertex.h"

#define CHROME9_GART_SIZE		SZ_64K
#define CHROME9_GART_ENTRIES		(CHROME9_GART_SIZE / sizeof(u32))
#define CHROME9_GART_INVALID		0x80000000U
#define CHROME9_GART_CACHED		BIT(25)
#define CHROME9_RING_PAGES		16U
#define CHROME9_RING_SIZE		(CHROME9_RING_PAGES * PAGE_SIZE)
#define CHROME9_RUN_STAGING_SIZE	SZ_64K
#define CHROME9_CR_READ_AHEAD		0x220U
#define CHROME9_PRIME_PAUSE		0x70U
#define CHROME9_PRIME_STOP		0xf0U
#define CHROME9_WRAP_APPEND		0x380U
#define CHROME9_SEQ_INDEX		0x83c4
#define CHROME9_SEQ_DATA			0x83c5
#define CHROME9_FIXED_BO_SIZE		(3U * PAGE_SIZE)
#define CHROME9_TARGET_WORDS		(PAGE_SIZE / sizeof(u32))
#define CHROME9_GUARD_WORD		0x13579bdfU
#define CHROME9_TARGET_SENTINEL		0xdeadbeefU
#define CHROME9_TARGET_RESULT		0x000000ffU
#define CHROME9_TARGET_RED		0x00ff0000U
#define CHROME9_TARGET_DIM		32U
#define CHROME9_DEP_TIMEOUT		msecs_to_jiffies(5000)
#define CHROME9_HW_TIMEOUT_US		100000
#define CHROME9_DRAW_CACHE_MAX_ENTRIES	64U
#define CHROME9_DRAW_CACHE_MAX_PAGES	(CHROME9_GART_ENTRIES - \
					 CHROME9_RING_PAGES)
#define CHROME9_FE33_SYSTEM_WRITE	0xfe330004U
#define CHROME9_FE33_WAIT_3D		0x00200000U
#define CHROME9_FE33_SLOT_SIZE		16U
#define CHROME9_FE33_CANARY		0x5afe3300U

static_assert(IS_ALIGNED(CHROME9_PRIME_PAUSE, 16));
static_assert(IS_ALIGNED(CHROME9_PRIME_STOP, 16));
static_assert(IS_ALIGNED(CHROME9_WRAP_APPEND, 128));
static_assert(CHROME9_PRIME_STOP - CHROME9_PRIME_PAUSE >= 0x20);
static_assert(CHROME9_WRAP_APPEND >=
	      ALIGN(CHROME9_PRIME_STOP + CHROME9_CR_READ_AHEAD, 128));

static bool chrome9_engine_enable;
module_param_named(engine, chrome9_engine_enable, bool, 0400);
MODULE_PARM_DESC(engine,
		 "Enable persistent GART/ring and fixed-quad ioctl (default: false)");

static bool chrome9_textured_triangle_run;
module_param_named(textured_triangle_run, chrome9_textured_triangle_run,
		   bool, 0400);
MODULE_PARM_DESC(textured_triangle_run,
		 "Enable the pre-promotion one-texture run ioctl (default: false)");

static bool chrome9_textured_triangle_mip;
module_param_named(textured_triangle_mip, chrome9_textured_triangle_mip,
		   bool, 0400);
MODULE_PARM_DESC(textured_triangle_mip,
		 "Enable the pre-promotion guarded mip-texture run ioctl (default: false)");

static bool chrome9_rhw_chain;
module_param_named(rhw_chain, chrome9_rhw_chain, bool, 0400);
MODULE_PARM_DESC(rhw_chain,
		 "Enable the pre-promotion D16 RHW chain ioctl (default: false)");

static bool chrome9_ps_add;
module_param_named(ps_add, chrome9_ps_add, bool, 0400);
MODULE_PARM_DESC(ps_add,
		 "Enable the pre-promotion structured pixel ADD opcode (default: false)");

static bool chrome9_fragment_sparse_textures;
module_param_named(fragment_sparse_textures,
	chrome9_fragment_sparse_textures, bool, 0400);
MODULE_PARM_DESC(fragment_sparse_textures,
	"Enable unqualified sparse fragment sampler ABI v2 (default: false)");

static bool chrome9_fe33_fence;
module_param_named(fe33_fence, chrome9_fe33_fence, bool, 0400);
MODULE_PARM_DESC(fe33_fence,
	"Require a coherent FE330004 3D completion write per 3D job (default: false)");

static bool chrome9_hw_tcl;
module_param_named(hw_tcl, chrome9_hw_tcl, bool, 0400);
MODULE_PARM_DESC(hw_tcl,
	"Enable the pre-promotion hardware vertex program ioctl (default: false)");

/* RE knob for the two-side/UCP ladder: 1 clear A61[19], 2 no saturate on
 * oBFD0, 4 UCP perspective (A05[6]/[14]), 8 UCP bits in A28/A29. */
#define CHROME9_TCL_RE_NO_A61_BFC	BIT(0)
#define CHROME9_TCL_RE_NO_BFD_SAT	BIT(1)
#define CHROME9_TCL_RE_UCP_PERSPECTIVE	BIT(2)
#define CHROME9_TCL_RE_UCP_CONSUMER	BIT(3)
/* 16 clip engine off (A01 bits 20/11), 32 A23[4], 64 A23[8] = 0 uncull. */
#define CHROME9_TCL_RE_CLIP_OFF		BIT(4)
#define CHROME9_TCL_RE_A23_2FRENDER	BIT(5)
#define CHROME9_TCL_RE_A23_CULL_CW	BIT(6)
/* 128 vendor single-color layout: no oD1/oD3 records, no Cs in A25/A28. */
#define CHROME9_TCL_RE_NO_SECONDARY	BIT(7)

/*
 * Debug: fragment/TCL job phase profile.  With profile=N every N jobs log
 * the average microseconds per phase.  Single-client measurement only.
 */
/* Debug: non-zero VRAM offset receives fragment job color writes instead of
 * the SF destination (local frame buffer fill-rate measurement). */
/* Draw objects are cache-maintained by the kernel, so their SF pages may
 * be mapped without the snoop (cached) PTE bit. */
static bool chrome9_gart_nosnoop = true;
module_param_named(gart_nosnoop, chrome9_gart_nosnoop, bool, 0400);
MODULE_PARM_DESC(gart_nosnoop, "Map draw objects with non-snooped GART PTEs (default: true)");

static ulong chrome9_vram_target_test;
module_param_named(vram_target_test, chrome9_vram_target_test, ulong, 0600);
MODULE_PARM_DESC(vram_target_test, "Debug: VRAM offset for fragment color writes");

static uint chrome9_profile;
module_param_named(profile, chrome9_profile, uint, 0600);
MODULE_PARM_DESC(profile, "Debug: log fragment job phase times every N jobs");

enum chrome9_prof_phase {
	CHROME9_PROF_ENTRY,	/* ioctl copy-in and validation */
	CHROME9_PROF_COMPILE,	/* PS and VS compilers */
	CHROME9_PROF_MAP,	/* lookup, pages, lock, vmap, finite check */
	CHROME9_PROF_GART,	/* SF draw-cache mapping acquire */
	CHROME9_PROF_FLUSH,	/* guards and CPU cache flush */
	CHROME9_PROF_BUILD,	/* command stream build */
	CHROME9_PROF_APPEND,	/* ring copy and doorbell */
	CHROME9_PROF_GPU,	/* wait for retirement */
	CHROME9_PROF_VERIFY,	/* flush back, guard verify, mapping release */
	CHROME9_PROF_RETIRE,	/* unlock, vunmap, object and fence release */
	CHROME9_PROF_COUNT,
};

static const char *const chrome9_prof_names[CHROME9_PROF_COUNT] = {
	"entry", "compile", "map", "gart", "flush", "build", "append", "gpu",
	"verify",
	"retire",
};
static u64 chrome9_prof_ns[CHROME9_PROF_COUNT];
/* Sub-costs inside the flush phase: guards, full flushes, DMA sync. */
static u64 chrome9_prof_sub_ns[3];
static u64 chrome9_prof_full_bytes;
/* Wait phases: FE33 fence, cookie, PAUSE, engine work drain. */
static u64 chrome9_prof_wait_ns[4];
/* Full flushes by reason: implicit destination/depth/texture, dirty same. */
static u64 chrome9_prof_full_kind[6];
static u64 chrome9_prof_jobs;
static ktime_t chrome9_prof_last;

static void chrome9_prof_start(void)
{
	if (READ_ONCE(chrome9_profile))
		chrome9_prof_last = ktime_get();
}

static void chrome9_prof_mark(enum chrome9_prof_phase phase)
{
	ktime_t now;

	if (!READ_ONCE(chrome9_profile))
		return;
	now = ktime_get();
	chrome9_prof_ns[phase] += ktime_to_ns(ktime_sub(now,
						       chrome9_prof_last));
	chrome9_prof_last = now;
}

static void chrome9_prof_job(struct drm_device *drm)
{
	u32 every = READ_ONCE(chrome9_profile);
	unsigned int i;

	if (!every || ++chrome9_prof_jobs < every)
		return;
	for (i = 0; i < CHROME9_PROF_COUNT; i++) {
		dev_info(drm->dev, "profile %s %llu us\n", chrome9_prof_names[i],
			 div64_u64(chrome9_prof_ns[i], 1000 * chrome9_prof_jobs));
		chrome9_prof_ns[i] = 0;
	}
	dev_info(drm->dev,
		 "profile flush: guards %llu us, full %llu us (%llu KiB), dma sync %llu us\n",
		 div64_u64(chrome9_prof_sub_ns[0], 1000 * chrome9_prof_jobs),
		 div64_u64(chrome9_prof_sub_ns[1], 1000 * chrome9_prof_jobs),
		 div64_u64(chrome9_prof_full_bytes, 1024 * chrome9_prof_jobs),
		 div64_u64(chrome9_prof_sub_ns[2], 1000 * chrome9_prof_jobs));
	dev_info(drm->dev,
		 "profile full flushes: implicit dst %llu depth %llu tex %llu, dirty dst %llu depth %llu tex %llu\n",
		 chrome9_prof_full_kind[0], chrome9_prof_full_kind[1],
		 chrome9_prof_full_kind[2], chrome9_prof_full_kind[3],
		 chrome9_prof_full_kind[4], chrome9_prof_full_kind[5]);
	memset(chrome9_prof_full_kind, 0, sizeof(chrome9_prof_full_kind));
	dev_info(drm->dev, "profile wait: fence %llu cookie %llu pause %llu work %llu us\n",
		 div64_u64(chrome9_prof_wait_ns[0], 1000 * chrome9_prof_jobs),
		 div64_u64(chrome9_prof_wait_ns[1], 1000 * chrome9_prof_jobs),
		 div64_u64(chrome9_prof_wait_ns[2], 1000 * chrome9_prof_jobs),
		 div64_u64(chrome9_prof_wait_ns[3], 1000 * chrome9_prof_jobs));
	memset(chrome9_prof_wait_ns, 0, sizeof(chrome9_prof_wait_ns));
	memset(chrome9_prof_sub_ns, 0, sizeof(chrome9_prof_sub_ns));
	chrome9_prof_full_bytes = 0;
	chrome9_prof_jobs = 0;
}
static uint chrome9_tcl_re;
module_param_named(tcl_re, chrome9_tcl_re, uint, 0600);
MODULE_PARM_DESC(tcl_re,
	"Debug: two-side/UCP oracle variants for hardware TCL (default: 0)");

static unsigned long chrome9_engine_gart_offset = ULONG_MAX;
module_param_named(engine_gart_offset, chrome9_engine_gart_offset, ulong, 0400);
MODULE_PARM_DESC(engine_gart_offset,
		 "Override BAR2 byte offset of the persistent 64-KiB GART (default: final 64 KiB of detected VRAM)");

static void __iomem *chrome9_iomap_bar_once(struct pci_dev *pdev, int bar)
{
	void __iomem * const *iomap_table = pcim_iomap_table(pdev);

	if (iomap_table && iomap_table[bar])
		return iomap_table[bar];
	return pcim_iomap(pdev, bar, 0);
}

struct chrome9_fence {
	struct dma_fence base;
	spinlock_t lock;
};

struct chrome9_sf_mapping;

/* Destination, depth, textures and the stage B vertex buffers. */
#define CHROME9_FRAGMENT_MAX_OBJECTS \
	(2 + DRM_CHROME9_FRAGMENT_MAX_TEXTURES + DRM_CHROME9_VERTEX_MAX_INPUTS)

struct chrome9_job {
	struct chrome9_fence fence;
	struct drm_gem_shmem_object *destination;
	struct drm_gem_shmem_object *depth;
	struct drm_gem_shmem_object *texture2;
	struct drm_gem_shmem_object *texture3;
	struct sg_table *sgt;
	struct sg_table *depth_sgt;
	struct sg_table *texture2_sgt;
	struct sg_table *texture3_sgt;
	struct iosys_map map;
	struct iosys_map depth_map;
	struct iosys_map texture2_map;
	struct iosys_map texture3_map;
	struct chrome9_sf_mapping *sf_mappings[CHROME9_FRAGMENT_MAX_OBJECTS];
	struct drm_gem_shmem_object *fragment_objects[
		CHROME9_FRAGMENT_MAX_OBJECTS];
	struct sg_table *fragment_sgts[CHROME9_FRAGMENT_MAX_OBJECTS];
	struct iosys_map fragment_maps[CHROME9_FRAGMENT_MAX_OBJECTS];
	bool fragment_mapped[CHROME9_FRAGMENT_MAX_OBJECTS];
	struct list_head quarantine_link;
	unsigned int sf_mapping_count;
	u32 cookie;
	u32 fence_id;
	u32 flags;
	u32 segment_pause;
	u32 segment_stop;
	u32 ring_pause;
	u32 ring_stop;
	u8 kind;
	bool mapped;
	bool depth_mapped;
	bool texture2_mapped;
	bool texture3_mapped;
	bool fence_attached;
	bool published;
	bool retirement_proven;
};

enum chrome9_draw_kind {
	CHROME9_DRAW_FIXED,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_BLEND,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_BLEND,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BLEND,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BATCH,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP,
	CHROME9_DRAW_COPY_RECT,
	CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE,
	CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16,
	CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16_BLEND,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_BATCH,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_BATCH,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_BATCH,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_CHAIN,
	CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH,
	CHROME9_DRAW_FRAGMENT_PROGRAM,
};

/*
 * Hardware TCL: compiled VS, viewport rows and either raw inline vertices
 * (stage A) or GEM vertex buffers fetched through rebased indices (stage B).
 */
struct chrome9_tcl_draw {
	struct chrome9_vertex_program program;
	const struct drm_chrome9_vertex_instruction *instructions;
	u32 instruction_count;
	const u32 *constants;
	u32 constant_first;
	u32 constant_count;
	const u32 *vertices;
	u32 vertex_count;
	u32 stride_dw;
	u32 element_count;
	u32 cull;
	/* Enabled user clip planes (DRM_CHROME9_VERTEX_DEST_CLIP_DISTANCE). */
	u32 clip_mask;
	/* chrome9_tcl_re sampled once per ioctl. */
	u32 re;
	u8 element_format[DRM_CHROME9_VERTEX_MAX_INPUTS];
	u8 element_type[DRM_CHROME9_VERTEX_MAX_INPUTS];
	u8 element_offset[DRM_CHROME9_VERTEX_MAX_INPUTS];
	u8 input_lanes[DRM_CHROME9_VERTEX_MAX_INPUTS];
	/* Rows 256..258: viewport scale, offset and the snap bias. */
	u32 rows[12];
	/* Stage B: indices are rebased to index_min and packed per dword. */
	const u32 *indices;
	u32 index_count;
	u32 index_dwords;
	u32 index_min;
	u32 index_range;
	bool index32;
	s32 base_vertex;
	u32 buffer_count;
	struct drm_chrome9_vertex_buffer buffers[DRM_CHROME9_VERTEX_MAX_INPUTS];
	u8 element_buffer[DRM_CHROME9_VERTEX_MAX_INPUTS];
	u32 element_byte_offset[DRM_CHROME9_VERTEX_MAX_INPUTS];
};

struct chrome9_draw_request {
	enum chrome9_draw_kind kind;
	u32 destination_handle;
	u32 depth_handle;
	u32 texture_handle;
	u32 texture2_handle;
	u32 texture3_handle;
	u32 flags;
	u32 width;
	u32 height;
	u32 pitch;
	u64 object_size;
	u32 depth_pitch;
	u64 depth_object_size;
	u32 texture_width;
	u32 texture_height;
	u32 texture_pitch;
	u32 texture_format;
	u32 min_filter;
	u32 mag_filter;
	u32 wrap_s;
	u32 wrap_t;
	u32 texture_last_level;
	u32 mip_filter;
	s32 lod_bias_s5_5;
	u32 texture_level_offsets[10];
	u32 texture_program;
	u32 destination_format;
	u64 texture_object_size;
	u32 texture2_width;
	u32 texture2_height;
	u32 texture2_pitch;
	u32 texture2_format;
	u32 texture2_min_filter;
	u32 texture2_mag_filter;
	u32 texture2_wrap_s;
	u32 texture2_wrap_t;
	u64 texture2_object_size;
	u64 texture3_object_size;
	struct drm_chrome9_scissor_state scissor;
	struct drm_chrome9_blend_state blend;
	struct drm_chrome9_vertex vertices[3];
	struct drm_chrome9_vertex_d16 depth_vertices[3];
	struct drm_chrome9_textured_vertex textured_vertices[3];
	struct drm_chrome9_two_textured_vertex two_textured_vertices[3];
	const struct drm_chrome9_vertex_d16 *batch_vertices;
	const struct drm_chrome9_vertex_d16_rhw *batch_vertices_rhw;
	const struct drm_chrome9_textured_vertex *textured_batch_vertices;
	const struct drm_chrome9_fragment_vertex *fragment_vertices;
	const struct drm_chrome9_fragment_texture_state *fragment_textures;
	const struct chrome9_fragment_program *fragment_program;
	const struct chrome9_tcl_draw *tcl;
	u64 fragment_texture_size[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
	u32 fragment_texture_count;
	u8 fragment_texture_units[DRM_CHROME9_FRAGMENT_MAX_TEXTURES];
	u32 fragment_depth_format;
	struct drm_chrome9_stencil_state stencil;
	u32 ps_program[4];
	u32 ps_constant_c0[4];
	u32 triangle_count;
	u32 destination_x;
	u32 destination_y;
	u32 source_x;
	u32 source_y;
	u32 copy_width;
	u32 copy_height;
};

static_assert(sizeof(struct drm_chrome9_scissor_state) == 12);
static_assert(sizeof(struct drm_chrome9_draw_triangle) == 88);
static_assert(sizeof(struct drm_chrome9_draw_triangle_d16) == 96);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle) == 176);
static_assert(sizeof(struct drm_chrome9_blend_state) == 40);
static_assert(sizeof(struct drm_chrome9_draw_triangle_blend) == 128);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_blend) == 216);
static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_blend) == 144);
static_assert(sizeof(struct drm_chrome9_draw_triangle_batch) == 128);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_batch) == 160);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_run) == 160);
static_assert(offsetof(struct drm_chrome9_draw_textured_triangle_run,
		       vertices_ptr) == 128);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_mip) == 176);
static_assert(offsetof(struct drm_chrome9_draw_textured_triangle_mip,
		       vertices_ptr) == 144);
static_assert(sizeof(struct drm_chrome9_copy_rect) == 96);
static_assert(sizeof(struct drm_chrome9_vertex_d16_rhw) == 20);
static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_rhw_batch) == 128);
static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_rhw_run) == 128);
static_assert(sizeof(struct drm_chrome9_draw_triangle_d16_rhw_chain) == 128);
static_assert(sizeof(struct drm_chrome9_ps_source) == 8);
static_assert(sizeof(struct drm_chrome9_ps_instruction) == 24);
static_assert(sizeof(struct drm_chrome9_draw_program_triangle_d16_rhw_batch) ==
	      144);
static_assert(sizeof(struct drm_chrome9_fragment_source) == 8);
static_assert(sizeof(struct drm_chrome9_fragment_destination) == 4);
static_assert(sizeof(struct drm_chrome9_fragment_instruction) == 32);
static_assert(sizeof(struct drm_chrome9_fragment_vertex) == 152);
static_assert(sizeof(struct drm_chrome9_fragment_texture_state) == 176);
static_assert(sizeof(struct drm_chrome9_draw_fragment_program) == 184);
static_assert(sizeof(struct drm_chrome9_texture_state) == 36);
static_assert(sizeof(struct drm_chrome9_two_textured_vertex) == 40);
static_assert(sizeof(struct drm_chrome9_draw_two_textured_triangle) == 240);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_d16) == 192);
static_assert(sizeof(struct drm_chrome9_draw_textured_triangle_d16_blend) == 232);
static_assert(sizeof(struct drm_chrome9_draw_two_textured_triangle_d16) == 256);
static_assert(sizeof(struct drm_chrome9_draw_two_textured_triangle_d16_blend) == 296);

struct chrome9_blend_words {
	u32 a00;
	u32 a33;
	u32 a34;
	u32 a35;
	u32 a36;
	u32 a37;
	u32 a53;
};

struct chrome9_engine {
	struct chrome9_device *cdev;
	struct mutex submit_lock;
	struct mutex vm_lock;
	struct drm_mm sf_mm;
	struct list_head quarantine;
	struct list_head scanout_mappings;
	struct list_head draw_mappings;
	void *ring_cpu;
	dma_addr_t ring_dma;
	void *staging_cpu;
	void *run_staging_cpu;
	u32 *fence_cpu;
	dma_addr_t fence_dma;
	/* Header plus ring bytes of the most recent timed-out segment. */
	u32 *timeout_segment;
	size_t timeout_segment_size;
	u32 fence_last_id;
	u64 fence_count;
	u64 fence_canary_faults;
	void __iomem *gart;
	void *gart_saved;
	resource_size_t gart_offset;
	u64 fence_context;
	atomic64_t fence_seqno;
	u32 saved_dst_pos;
	u8 saved_seq_index;
	u8 saved_6a;
	u8 saved_6b;
	u8 saved_6c;
	u8 saved_7b;
	u32 append;
	u32 active_pause;
	u32 active_stop;
	u32 ring_epoch;
	u64 append_count;
	u64 wrap_count;
	u64 draw_cache_hits;
	u64 draw_cache_misses;
	u64 draw_cache_evictions;
	u64 draw_cache_scanout_hits;
	u64 gart_update_count;
	u64 gart_pte_write_count;
	u64 gart_vga_acquire_count;
	u64 gart_flush_count;
	u64 gart_flush_rerequests;
	u64 gart_flush_slow;
	u64 gart_flush_kicks;
	/* Segment staging for GTI kicks; never shared with a job being built. */
	u32 *kick_cpu;
	/* Evicted/unmapped PTEs point here instead of the invalid entry. */
	u32 *scratch_cpu;
	dma_addr_t scratch_dma;
	u32 scratch_pte;
	unsigned int draw_cache_entries;
	unsigned int draw_cache_pages;
	u32 last_cookie;
	bool mm_initialized;
	bool gart_programmed;
	bool ring_published;
	bool ready;
	bool stopping;
	bool wedged;
	bool was_master;
	bool quarantine_hold;
	bool module_hold;
	bool device_hold;
};

struct chrome9_sf_mapping {
	struct chrome9_engine *engine;
	struct drm_gem_object *object;
	struct sg_table *sgt;
	struct drm_mm_node node;
	struct list_head link;
	u32 *ptes;
	unsigned int page_count;
	unsigned int users;
	bool node_allocated;
	bool bound;
	bool quarantined;
	bool draw_cached;
};

static const char *chrome9_fence_get_driver_name(struct dma_fence *fence)
{
	return "chrome9";
}

static const char *chrome9_fence_get_timeline_name(struct dma_fence *fence)
{
	return "fixed-quad";
}

static void chrome9_fence_release(struct dma_fence *fence)
{
	dma_fence_free(fence);
}

static const struct dma_fence_ops chrome9_fence_ops = {
	.get_driver_name = chrome9_fence_get_driver_name,
	.get_timeline_name = chrome9_fence_get_timeline_name,
	.release = chrome9_fence_release,
};

static void chrome9_cache_flush(void *address, size_t size)
{
	/* Publish earlier CPU stores before evicting this cached shmem range. */
	mb();
#ifdef CONFIG_X86
	clflush_cache_range(address, size);
#endif
	/* Complete eviction before the caller transfers ownership to the GPU. */
	mb();
}

static u8 chrome9_seq_read(struct chrome9_device *cdev, u8 index)
{
	writeb(index, cdev->mmio + CHROME9_SEQ_INDEX);
	return readb(cdev->mmio + CHROME9_SEQ_DATA);
}

static void chrome9_seq_write(struct chrome9_device *cdev, u8 index, u8 value)
{
	writeb(index, cdev->mmio + CHROME9_SEQ_INDEX);
	writeb(value, cdev->mmio + CHROME9_SEQ_DATA);
}

static int chrome9_gart_wait_idle(struct chrome9_device *cdev)
{
	u8 value;

	writeb(0x6f, cdev->mmio + CHROME9_SEQ_INDEX);
	return readb_poll_timeout(cdev->mmio + CHROME9_SEQ_DATA, value,
				  !(value & BIT(7)), 10,
				  CHROME9_HW_TIMEOUT_US);
}

#define CHROME9_GART_FLUSH_BURST	10
#define CHROME9_GART_FLUSH_TIMEOUT_MS	600

static u8 chrome9_gart_request_flush(struct chrome9_device *cdev)
{
	u8 value;

	writeb(0x6f, cdev->mmio + CHROME9_SEQ_INDEX);
	value = (readb(cdev->mmio + CHROME9_SEQ_DATA) & 0x7f) | BIT(7);
	writeb(value, cdev->mmio + CHROME9_SEQ_DATA);
	return value;
}

static int chrome9_gart_kick(struct chrome9_engine *engine);

/*
 * Wait for SR6F bit 7 to clear.  A request the GTI did not take is
 * re-issued: VIA rev 0-2 write it up to ten times back to back, rev 3
 * polls every 1 ms for 600 ms and gives an idle engine traffic
 * (docs/research/chrome9-gart-flush-timeout.md).  kick requires
 * submit_lock: it runs a kernel-owned cookie segment through the ring.
 */
static int chrome9_gart_flush_wait(struct chrome9_engine *engine, u8 request,
				   bool kick)
{
	struct chrome9_device *cdev = engine->cdev;
	unsigned int i;
	int ret;

	writeb(0x6f, cdev->mmio + CHROME9_SEQ_INDEX);
	for (i = 0; i < CHROME9_GART_FLUSH_BURST; i++) {
		if (!(readb(cdev->mmio + CHROME9_SEQ_DATA) & BIT(7)))
			return 0;
		writeb(request, cdev->mmio + CHROME9_SEQ_DATA);
		engine->gart_flush_rerequests++;
	}
	engine->gart_flush_slow++;
	for (i = 0; i < CHROME9_GART_FLUSH_TIMEOUT_MS; i++) {
		if (kick && i % 10 == 0 &&
		    !(readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS) &
		      CHROME9_ENGINE_WORK_MASK)) {
			ret = chrome9_gart_kick(engine);
			/* A busy engine already generates GTI traffic. */
			if (ret && ret != -EBUSY)
				return ret;
		} else {
			usleep_range(1000, 1200);
		}
		writeb(0x6f, cdev->mmio + CHROME9_SEQ_INDEX);
		if (!(readb(cdev->mmio + CHROME9_SEQ_DATA) & BIT(7)))
			return 0;
		if (i % 10 == 9) {
			writeb(request, cdev->mmio + CHROME9_SEQ_DATA);
			engine->gart_flush_rerequests++;
		}
	}
	return -ETIMEDOUT;
}

static void chrome9_emit_header0(u32 **cursor, const u32 *pairs,
				 unsigned int count)
{
	u32 *p = *cursor;
	unsigned int i;

	while (!IS_ALIGNED((unsigned long)p, 16))
		*p++ = 0xcc000000;
	*p++ = 0xfe000000;
	*p++ = count;
	*p++ = 0;
	*p++ = 0xcc000000;
	for (i = 0; i < count * 2; i++)
		*p++ = pairs[i];
	*cursor = p;
}

static void chrome9_emit_header2(u32 **cursor, u32 transspace, u32 transset,
				 u32 paratype, const u32 *payload,
				 unsigned int count)
{
	u32 *p = *cursor;
	unsigned int i;

	while (!IS_ALIGNED((unsigned long)p, 16))
		*p++ = 0xcc000000;
	*p++ = 0xfe020000 | transspace;
	*p++ = transset;
	*p++ = paratype;
	*p++ = 0xcc000000;
	for (i = 0; i < count; i++)
		*p++ = payload[i];
	*cursor = p;
}

static void chrome9_emit_header3(u32 **cursor, u32 paratype,
				 const u32 *payload, unsigned int count)
{
	u32 *p = *cursor;
	unsigned int i;

	while (!IS_ALIGNED((unsigned long)p, 16))
		*p++ = 0xcc000000;
	*p++ = 0xfe03043c;
	*p++ = count;
	*p++ = paratype;
	*p++ = 0xcc000000;
	for (i = 0; i < count; i++)
		*p++ = payload[i];
	*cursor = p;
}

/*
 * Client-derived vertex data must travel in a counted H3 packet, as the
 * vendor drivers send it.  In an uncounted H2 packet the command regulator
 * ends the payload at the next 16-byte aligned FE-prefixed dword, so a
 * vertex colour such as feff0000 was decoded as a command header.  Builders
 * validate their H2-shaped layout first and then rewrite the header here.
 */
static void chrome9_count_vertex_payload(u32 *header, u32 dwords)
{
	header[0] = 0xfe03043c;
	header[1] = dwords;
	header[2] = 0x00000000;
	header[3] = 0xcc000000;
}

static int chrome9_fe33_encode(u32 record[4], u32 id, dma_addr_t address)
{
	if (!id || !IS_ALIGNED(address, CHROME9_FE33_SLOT_SIZE) ||
	    upper_32_bits(address) ||
	    address > U32_MAX - (CHROME9_FE33_SLOT_SIZE - 1))
		return -EINVAL;
	record[0] = CHROME9_FE33_SYSTEM_WRITE;
	record[1] = CHROME9_FE33_WAIT_3D | (id & 0xff) << 24;
	record[2] = (id >> 8) | ((u32)address & 0x00000ff0) << 20;
	record[3] = ((u32)address >> 12) & 0x000fffff;
	return 0;
}

static void chrome9_fe33_decode(const u32 record[4], u32 *id, u32 *address)
{
	*id = (record[2] & 0x00ffffff) << 8 | record[1] >> 24;
	*address = record[3] << 12 | (record[2] >> 24) << 4;
}

static int chrome9_fe33_selftest(void)
{
	static const u32 golden[4] = {
		0xfe330004, 0x78200000, 0x12123456, 0x00001088,
	};
	u32 record[4];
	u32 address;
	u32 id;

	if (chrome9_fe33_encode(record, 0x12345678, 0x01088120) ||
	    memcmp(record, golden, sizeof(golden)))
		return -EINVAL;
	chrome9_fe33_decode(record, &id, &address);
	if (id != 0x12345678 || address != 0x01088120)
		return -EINVAL;
	if (chrome9_fe33_encode(record, 0xffffffff, 0xfffffff0))
		return -EINVAL;
	chrome9_fe33_decode(record, &id, &address);
	if (id != 0xffffffff || address != 0xfffffff0)
		return -EINVAL;
	if (!chrome9_fe33_encode(record, 0, 0x1000) ||
	    !chrome9_fe33_encode(record, 1, 0x1008) ||
	    !chrome9_fe33_encode(record, 1, (dma_addr_t)0xfffffff8) ||
	    (sizeof(dma_addr_t) > 4 &&
	     !chrome9_fe33_encode(record, 1, (dma_addr_t)BIT_ULL(32))))
		return -EINVAL;
	return 0;
}

/* Reserve a fresh ID, poison the coherent slot and emit FE330004 after the body. */
static int chrome9_emit_fe33(struct chrome9_engine *engine,
			     struct chrome9_job *job, u32 **cursor)
{
	u32 record[4];
	u32 *p = *cursor;
	u32 id;
	int ret;

	job->fence_id = 0;
	if (!engine->fence_cpu)
		return 0;
	lockdep_assert_held(&engine->submit_lock);
	if (engine->fence_last_id == U32_MAX)
		return -ENOSPC;
	id = engine->fence_last_id + 1;
	ret = chrome9_fe33_encode(record, id, engine->fence_dma);
	if (ret)
		return ret;
	engine->fence_last_id = id;
	WRITE_ONCE(engine->fence_cpu[0], ~id);
	WRITE_ONCE(engine->fence_cpu[1], CHROME9_FE33_CANARY + 1);
	WRITE_ONCE(engine->fence_cpu[2], CHROME9_FE33_CANARY + 2);
	WRITE_ONCE(engine->fence_cpu[3], CHROME9_FE33_CANARY + 3);
	/* The poison must be visible before the segment can be fetched. */
	dma_wmb();
	while (!IS_ALIGNED((unsigned long)p, CHROME9_FE33_SLOT_SIZE))
		*p++ = 0xcc000000;
	memcpy(p, record, sizeof(record));
	*cursor = p + ARRAY_SIZE(record);
	job->fence_id = id;
	return 0;
}

static int chrome9_emit_3d_tail(struct chrome9_engine *engine,
				struct chrome9_job *job, u32 **cursor)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	int ret;

	ret = chrome9_emit_fe33(engine, job, cursor);
	if (ret)
		return ret;
	chrome9_emit_header0(cursor, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(cursor, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(cursor, cookie, ARRAY_SIZE(cookie) / 2);
	return 0;
}

static int chrome9_finish_ring_sized(u32 *base, u32 *p, size_t capacity,
				     u32 *pause, u32 *stop)
{
	u32 *end;
	size_t used;

	if (!base || !p || p < base || !capacity ||
	    !IS_ALIGNED(capacity, 2 * sizeof(*base)) ||
	    capacity < CHROME9_CR_READ_AHEAD)
		return -EINVAL;
	used = (u8 *)p - (u8 *)base;
	if (!IS_ALIGNED(used, 2 * sizeof(*base)) ||
	    used > capacity - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	end = base + capacity / sizeof(*base);
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;

	/* The reader is first retired from PID00 PAUSE into this immutable
	 * vendor dummy trailer using PID10 STOP.  Keep one full 128-byte block
	 * between the two addresses and publish legal H5 data through the end
	 * of the slot so read-ahead never reaches the memset poison. */
	*p++ = 0xfe020420;
	*p++ = 0x0000041c;
	*p++ = 0x00300000;
	*p++ = 0xcc000000;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*stop = (u8 *)p - (u8 *)base - 16;
	if (*stop <= *pause || *stop - *pause < 0x20 ||
	    *stop > capacity - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	/* Only STOP plus the read-ahead window is copied to the ring. */
	end = min(end, base + (*stop + 16 + CHROME9_CR_READ_AHEAD) /
			   sizeof(*base));
	while (p < end) {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	}
	return 0;
}

static int chrome9_finish_ring(u32 *base, u32 *p, u32 *pause, u32 *stop)
{
	return chrome9_finish_ring_sized(base, p, PAGE_SIZE, pause, stop);
}

static int chrome9_build_prime(struct chrome9_engine *engine,
			       u32 *pause, u32 *stop)
{
	u32 *base = engine->staging_cpu;
	u32 *p;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	return chrome9_finish_ring(base, p, pause, stop);
}

#define CHROME9_FIXED_QUAD_VS_MASK (DRM_CHROME9_FIXED_QUAD_VS_ORACLE | \
				    DRM_CHROME9_FIXED_QUAD_VS_W2 | \
				    DRM_CHROME9_FIXED_QUAD_VS_HALF | \
				    DRM_CHROME9_FIXED_QUAD_VS_M4X4 | \
				    DRM_CHROME9_FIXED_QUAD_VS_CLIP_WIDE | \
				    DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR | \
				    DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL | \
				    DRM_CHROME9_FIXED_QUAD_VS_FETCH)

/*
 * Byte-exact vendor compiler records (hw-TCL corpus out_d0_d1 and
 * m4x4_r0_v0_c0, each ending in the five-record position finalizer) and
 * STM_ViewPort_INV rows for a 32x32 window target.
 */
/* Clip-space W=1 quad TL, TR, BR, BL: position, oD0, oD1. */
static const u32 chrome9_vs_oracle_quad[] = {
	0xbf800000, 0x3f800000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
	0x3f800000, 0x3f800000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
	0x3f800000, 0xbf800000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
	0xbf800000, 0xbf800000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
};

static void chrome9_fixed_vs_oracle(u32 flags, u32 target,
				    struct chrome9_quad_vs *vs)
{
	static const u32 internal[] = {
		0x41800000, 0xc1800000, 0x3f800000, 0x00000000,
		0x494000f8, 0x494000f8, 0x00000000, 0x00000000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 passthrough[] = {
		0x0008014c, 0x00000000, 0x00000000, 0x00080a0f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
		0x00201b6c, 0x00000000, 0x00000000, 0x00280108,
		0x00200150, 0x00101b6c, 0x00201b6c, 0x001c0107,
		0x0010014c, 0x00380150, 0x00384154, 0x0004021f,
		0x0020014c, 0x00000000, 0x00000000, 0x0008020f,
		0x0020014c, 0x00000000, 0x00000000, 0x0008020f,
	};
	static const u32 m4x4[] = {
		0x0008014c, 0x0040014c, 0x00000000, 0x00060801,
		0x0008014c, 0x0040414c, 0x00000000, 0x00060802,
		0x0008014c, 0x0040814c, 0x00000000, 0x00060804,
		0x0008014c, 0x0040c14c, 0x0000014c, 0x0034010f,
		0x0010014c, 0x00000000, 0x00000000, 0x00080a0f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
		0x00201b6c, 0x00000000, 0x00000000, 0x00280108,
		0x00200150, 0x00101b6c, 0x00201b6c, 0x001c0107,
		0x0010014c, 0x00380150, 0x00384154, 0x0004021f,
		0x0020014c, 0x00000000, 0x00000000, 0x0008020f,
		0x0020014c, 0x00000000, 0x00000000, 0x0008020f,
	};
	/* Pixel (x, y, 0, 1) to clip space for a 32x32 target. */
	static const u32 pixel_to_clip[] = {
		0x3d800000, 0x00000000, 0x00000000, 0xbf800000,
		0x00000000, 0xbd800000, 0x00000000, 0x3f800000,
		0x00000000, 0x00000000, 0x3f800000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x3f800000,
	};
	static const u32 clip_w2[] = {
		0xc0000000, 0x40000000, 0, 0x40000000, 0xffff0000, 0xffff0000,
		0x40000000, 0x40000000, 0, 0x40000000, 0xffff0000, 0xffff0000,
		0x40000000, 0xc0000000, 0, 0x40000000, 0xffff0000, 0xffff0000,
		0xc0000000, 0xc0000000, 0, 0x40000000, 0xffff0000, 0xffff0000,
	};
	static const u32 clip_wide[] = {
		0xc0800000, 0x40800000, 0x3f000000, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x40800000, 0x40800000, 0x3f000000, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x40800000, 0xc0800000, 0x3f000000, 0x3f800000,
		0xffff0000, 0xffff0000,
		0xc0800000, 0xc0800000, 0x3f000000, 0x3f800000,
		0xffff0000, 0xffff0000,
	};
	/* (-.5,-.5,.9,1), (.5,-.5,.9,1), (0,.5,-.35,-.25): z = w - 0.1. */
	static const u32 clip_near[] = {
		0xbf000000, 0xbf000000, 0x3f666666, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x3f000000, 0xbf000000, 0x3f666666, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x00000000, 0x3f000000, 0xbeb33333, 0xbe800000,
		0xffff0000, 0xffff0000,
	};
	static const u32 pixel[] = {
		0x00000000, 0x00000000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
		0x42000000, 0x00000000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
		0x42000000, 0x42000000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
		0x00000000, 0x42000000, 0, 0x3f800000, 0xffff0000, 0xffff0000,
	};
	bool matrix = flags == DRM_CHROME9_FIXED_QUAD_VS_M4X4;

	memset(vs, 0, sizeof(*vs));
	vs->internal = internal;
	vs->internal_dwords = ARRAY_SIZE(internal);
	vs->program = matrix ? m4x4 : passthrough;
	vs->program_dwords = matrix ? ARRAY_SIZE(m4x4) : ARRAY_SIZE(passthrough);
	/* Active records exclude the two inactive MOV tails. */
	vs->a60 = 0x60800000 | (vs->program_dwords / 4 - 3);
	vs->a62 = matrix ? 0x62208003 : 0x62208002;
	if (matrix) {
		vs->user = pixel_to_clip;
		vs->user_dwords = ARRAY_SIZE(pixel_to_clip);
	}
	vs->vertices = matrix ? pixel :
		       flags == DRM_CHROME9_FIXED_QUAD_VS_W2 ? clip_w2 :
		       flags == DRM_CHROME9_FIXED_QUAD_VS_CLIP_WIDE ? clip_wide :
		       flags & (DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR |
				DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL) ?
		       clip_near : chrome9_vs_oracle_quad;
	vs->a24 = flags & (DRM_CHROME9_FIXED_QUAD_VS_HALF |
			   DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR |
			   DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL) ?
		  0x24000003 : 0x24000004;
	vs->clip = flags & (DRM_CHROME9_FIXED_QUAD_VS_CLIP_WIDE |
			    DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR |
			    DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL);
	if (flags == DRM_CHROME9_FIXED_QUAD_VS_FETCH)
		vs->vertex_address = target + 2 * PAGE_SIZE;
	/* Also execute the first MOV clip-position(32) <- scratch tail. */
	if (flags == DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL)
		vs->a60++;
	vs->vertex_dwords = (vs->a24 & 0xff) * 6;
}

static int chrome9_build_fixed(struct chrome9_engine *engine,
			       struct chrome9_job *job, u32 target,
			       u32 flags, u32 *pause)
{
	static const u32 triangle_vertices[] = {
		0xbf000000, 0xbf000000, 0x00000000, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x41fc0000, 0xbf000000, 0x00000000, 0x3f800000,
		0xff00ff00, 0xffff0000,
		0x41fc0000, 0x41fc0000, 0x00000000, 0x3f800000,
		0xff0000ff, 0xffff0000,
	};
	static const u32 sync[] = { 0x6c, 0x00800000 };
	u32 *base = engine->staging_cpu;
	u32 *body;
	u32 *p;
	int ret;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	if (flags & CHROME9_FIXED_QUAD_VS_MASK) {
		struct chrome9_quad_vs vs;

		chrome9_fixed_vs_oracle(flags, target, &vs);
		chrome9_emit_3d_quad_h6_vs(&p, target, &vs);
		goto tail;
	}
	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188)
		return -EINVAL;
	if (flags == DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE) {
		if (body[49] != 0x23000f02 || body[50] != 0x24000004 ||
		    body[160] != 0xfe020440 || body[163] != 0xcc000000)
			return -EINVAL;
		body[49] = 0x23004f02;
		body[50] = 0x24000003;
		memcpy(body + 164, triangle_vertices, sizeof(triangle_vertices));
		body[182] = 0xcc000000;
		body[183] = 0xcc000000;
		p = body + 184;
	} else if (flags == DRM_CHROME9_FIXED_QUAD_CONSTANT_RED) {
		body[72] = 0x3f800000;
		body[75] = 0x3f800000;
		body[156] = 0x01c00530;
		body[157] = 0x301c0053;
		body[158] = 0x00f08005;
		body[159] = 0x000d0820;
	}
tail:
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	return chrome9_finish_ring(base, p, pause, &job->segment_stop);
}

static u32 chrome9_q16_to_float(s32 value)
{
	u32 sign = 0;
	u32 magnitude;
	u32 normalized;
	unsigned int highest;
	u32 exponent;

	if (!value)
		return 0;
	if (value < 0) {
		sign = BIT(31);
		magnitude = -(u32)value;
	} else {
		magnitude = value;
	}
	highest = fls(magnitude) - 1;
	exponent = highest + (127 - 16);
	if (highest <= 23) {
		normalized = magnitude << (23 - highest);
	} else {
		unsigned int shift = highest - 23;
		u32 remainder = magnitude & (BIT(shift) - 1);
		u32 halfway = BIT(shift - 1);

		normalized = magnitude >> shift;
		if (remainder > halfway ||
		    (remainder == halfway && (normalized & 1)))
			normalized++;
		if (normalized & BIT(24)) {
			normalized >>= 1;
			exponent++;
		}
	}
	return sign | exponent << 23 | (normalized & GENMASK(22, 0));
}

static bool chrome9_draw_has_blend(const struct chrome9_draw_request *request)
{
	return request->kind == CHROME9_DRAW_IMMEDIATE_TRIANGLE_BLEND ||
	       request->kind == CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_BLEND ||
	       request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BLEND ||
	       request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND ||
	       request->kind ==
		CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16_BLEND ||
	       (request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16 &&
		(request->flags & DRM_CHROME9_TEXTURED_D16_SOURCE_OVER)) ||
	       (request->triangle_count &&
		(request->flags & DRM_CHROME9_TRIANGLE_BATCH_BLEND));
}

static u32 chrome9_blend_factor_code(u32 factor, bool alpha)
{
	switch (factor) {
	case DRM_CHROME9_BLEND_FACTOR_ZERO:
		return alpha ? 0x09 : 0x05;
	case DRM_CHROME9_BLEND_FACTOR_ONE:
		return alpha ? 0x19 : 0x15;
	case DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA:
		return alpha ? 0x01 : 0x02;
	case DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA:
		return alpha ? 0x11 : 0x12;
	case DRM_CHROME9_BLEND_FACTOR_SRC_COLOR:
		return alpha ? 0x01 : 0x00;
	case DRM_CHROME9_BLEND_FACTOR_INV_SRC_COLOR:
		return alpha ? 0x11 : 0x10;
	case DRM_CHROME9_BLEND_FACTOR_DST_ALPHA:
		return alpha ? 0x02 : 0x03;
	case DRM_CHROME9_BLEND_FACTOR_INV_DST_ALPHA:
		return alpha ? 0x12 : 0x13;
	case DRM_CHROME9_BLEND_FACTOR_DST_COLOR:
		return alpha ? 0x02 : 0x01;
	case DRM_CHROME9_BLEND_FACTOR_INV_DST_COLOR:
		return alpha ? 0x12 : 0x11;
	case DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE:
		return alpha ? 0x19 : 0x08;
	default:
		/* The ioctl validator rejects unsupported factors. */
		return 0;
	}
}

static u32 chrome9_blend_color_mask(u32 mask)
{
	u32 hardware = 0;

	if (mask & DRM_CHROME9_COLOR_MASK_R)
		hardware |= BIT(2);
	if (mask & DRM_CHROME9_COLOR_MASK_G)
		hardware |= BIT(1);
	if (mask & DRM_CHROME9_COLOR_MASK_B)
		hardware |= BIT(0);
	if (mask & DRM_CHROME9_COLOR_MASK_A)
		hardware |= BIT(3);
	return hardware;
}

static u32 chrome9_scissor_y(const struct chrome9_draw_request *request)
{
	if (!(request->scissor.flags & DRM_CHROME9_SCISSOR_ENABLE))
		return 0x82000000 | request->height;
	return 0x82000000 | request->scissor.min_y << 12 |
		request->scissor.max_y;
}

static u32 chrome9_scissor_x(const struct chrome9_draw_request *request)
{
	if (!(request->scissor.flags & DRM_CHROME9_SCISSOR_ENABLE))
		return 0x83000000 | request->width;
	return 0x83000000 | request->scissor.min_x << 12 |
		request->scissor.max_x;
}

static u32 chrome9_depth_control_word(
	const struct chrome9_draw_request *request, bool perspective)
{
	u32 word = perspective ? 0x01ce1548 : 0x01c01548;

	if (!(request->flags & DRM_CHROME9_DEPTH_READ_ONLY))
		word |= BIT(2);
	return word;
}

/* A23/A24 carry the clockwise face and A25/A26 the counter-clockwise one;
 * identical faces reproduce the vendor's one-sided mirroring.
 */
static void chrome9_lower_stencil(const struct drm_chrome9_stencil_state *state,
				  u32 words[4])
{
	const struct drm_chrome9_stencil_face *face[2] = { &state->cw, &state->ccw };
	unsigned int i;

	for (i = 0; i < 2; i++) {
		words[2 * i] = (0x23 + 2 * i) << 24 | face[i]->ref << 16 |
			face[i]->value_mask << 8 | face[i]->write_mask;
		words[2 * i + 1] = (0x24 + 2 * i) << 24 | face[i]->func << 16 |
			face[i]->fail_op << 6 | face[i]->zfail_op << 3 |
			face[i]->zpass_op;
	}
}

static int chrome9_validate_stencil(const struct drm_chrome9_stencil_state *state)
{
	const struct drm_chrome9_stencil_face *face[2] = { &state->cw, &state->ccw };
	unsigned int i;

	for (i = 0; i < 2; i++)
		if (face[i]->func > DRM_CHROME9_COMPARE_ALWAYS ||
		    face[i]->fail_op > DRM_CHROME9_STENCIL_OP_DECR_WRAP ||
		    face[i]->zfail_op > DRM_CHROME9_STENCIL_OP_DECR_WRAP ||
		    face[i]->zpass_op > DRM_CHROME9_STENCIL_OP_DECR_WRAP ||
		    face[i]->pad)
			return -EINVAL;
	return 0;
}

static int chrome9_stencil_selftest(void)
{
	/* Neutral faces must reproduce the closed disabled block. */
	static const struct drm_chrome9_stencil_state neutral = {
		.cw = { DRM_CHROME9_COMPARE_ALWAYS, 0, 0, 0, 0, 0xff, 0xff, 0 },
		.ccw = { DRM_CHROME9_COMPARE_ALWAYS, 0, 0, 0, 0, 0xff, 0xff, 0 },
	};
	static const struct drm_chrome9_stencil_state faces = {
		.cw = { DRM_CHROME9_COMPARE_EQUAL, DRM_CHROME9_STENCIL_OP_INVERT,
			DRM_CHROME9_STENCIL_OP_INCR_WRAP,
			DRM_CHROME9_STENCIL_OP_REPLACE, 0xa5, 0xf0, 0x0f, 0 },
		.ccw = { DRM_CHROME9_COMPARE_NEVER,
			 DRM_CHROME9_STENCIL_OP_DECR_WRAP,
			 DRM_CHROME9_STENCIL_OP_ZERO,
			 DRM_CHROME9_STENCIL_OP_DECR_SAT, 0x5a, 0x0f, 0xf0, 0 },
	};
	u32 words[4];

	chrome9_lower_stencil(&neutral, words);
	if (words[0] != 0x2300ffff || words[1] != 0x24070000 ||
	    words[2] != 0x2500ffff || words[3] != 0x26070000)
		return -EINVAL;
	chrome9_lower_stencil(&faces, words);
	if (words[0] != 0x23a5f00f || words[1] != 0x24020172 ||
	    words[2] != 0x255a0ff0 || words[3] != 0x260001cc)
		return -EINVAL;
	return 0;
}

static void chrome9_lower_blend(const struct drm_chrome9_blend_state *state,
				struct chrome9_blend_words *words)
{
	u32 ref = state->alpha_test_ref;

	words->a00 = 0x00000001;
	if (state->flags & DRM_CHROME9_BLEND_ENABLE)
		words->a00 |= BIT(4);
	if (state->flags & DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE)
		words->a00 |= BIT(20);
	if (ref != 255)
		ref = ref << 2 | ref >> 6;
	else
		ref = 0x400;
	words->a33 = 0x33000000 | state->alpha_test_func << 12 | ref;
	words->a34 = 0x34000000 |
		chrome9_blend_factor_code(state->rgb_src_factor, false) << 4;
	words->a35 = 0x35000100 |
		chrome9_blend_factor_code(state->rgb_dst_factor, false) << 2;
	words->a36 = 0x36000400 |
		chrome9_blend_factor_code(state->alpha_src_factor, true) << 4;
	words->a37 = 0x37000200 |
		chrome9_blend_factor_code(state->alpha_dst_factor, true) << 2;
	words->a53 = 0x53010c00 | chrome9_blend_color_mask(state->color_mask);
}

static int chrome9_build_triangle(struct chrome9_engine *engine,
				  const struct chrome9_draw_request *request,
				  struct chrome9_job *job, u32 target,
				  u32 *pause)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	struct chrome9_blend_words blend_words;
	u32 *base = engine->staging_cpu;
	u32 *body;
	u32 *p;
	u32 delta = 0;
	u32 triangle_count = request->triangle_count ?: 1;
	u32 vertex_count = triangle_count * 3;
	bool blend = chrome9_draw_has_blend(request);
	unsigned int i;
	int ret;

	if (!triangle_count ||
	    triangle_count > DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES)
		return -E2BIG;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188 || body[6] != 0x52980004 ||
	    body[49] != 0x23000f02 || body[50] != 0x24000004 ||
	    body[160] != 0xfe020440 || body[163] != 0xcc000000)
		return -EINVAL;
	if (blend) {
		/* Expand the partial legacy constant block to closed A38--A3F. */
		memmove(body + 19, body + 15, (188 - 15) * sizeof(*body));
		chrome9_lower_blend(&request->blend, &blend_words);
		for (i = 0; i < 8; i++)
			body[7 + i] = (0x38 + i) << 24;
		body[15] = blend_words.a34;
		body[16] = blend_words.a35;
		body[17] = blend_words.a36;
		body[18] = blend_words.a37;
		body[21] = blend_words.a53;
		body[22] = blend_words.a00;
		if (request->blend.flags &
		    DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE)
			body[26] = blend_words.a33;
		delta = 4;
	}
	body[6] = 0x52000000 |
		(request->destination_format ==
		 DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ? 0x00990000 :
		 0x00980000) | (request->pitch >> 5);
	body[48 + delta] = 0x20000000;
	body[49 + delta] = 0x23004f02;
	body[50 + delta] = 0x24000000 | vertex_count;
	body[100 + delta] = 0x80000000 | request->height;
	body[101 + delta] = 0x81000000 | request->width;
	body[102 + delta] = chrome9_scissor_y(request);
	body[103 + delta] = chrome9_scissor_x(request);
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_vertex_d16 *batch =
			request->batch_vertices ? &request->batch_vertices[i] : NULL;
		u32 *vertex = body + 164 + delta + i * 6;

		vertex[0] = chrome9_q16_to_float(batch ? batch->x_16_16 :
						 request->vertices[i].x_16_16);
		vertex[1] = chrome9_q16_to_float(batch ? batch->y_16_16 :
						 request->vertices[i].y_16_16);
		vertex[2] = 0;
		vertex[3] = 0x3f800000;
		vertex[4] = batch ? batch->color : request->vertices[i].color;
		vertex[5] = 0xffffffff;
	}
	body[164 + delta + vertex_count * 6] = 0xcc000000;
	body[165 + delta + vertex_count * 6] = 0xcc000000;
	p = body + 166 + delta + vertex_count * 6;
	if ((!blend &&
	     (p - body != 166 + vertex_count * 6 ||
	      body[17] != 0x53010c0f ||
	      body[18] != 0x00000011 ||
	      body[48] != 0x20000000 ||
	      body[50] != (0x24000000 | vertex_count) ||
	      body[160] != 0xfe020440)) ||
	    (blend &&
	     (p - body != 170 + vertex_count * 6 ||
	      body[7] != 0x38000000 ||
	      body[14] != 0x3f000000 || body[15] != blend_words.a34 ||
	      body[16] != blend_words.a35 || body[17] != blend_words.a36 ||
	      body[18] != blend_words.a37 || body[21] != blend_words.a53 ||
	      body[22] != blend_words.a00 || body[23] != 0x01c01540 ||
	      body[26] !=
		((request->blend.flags &
		  DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
		 blend_words.a33 : 0xcc000000) ||
	      body[52] != 0x20000000 || body[53] != 0x23004f02 ||
	      body[54] != (0x24000000 | vertex_count) ||
	      body[164] != 0xfe020440 || body[167] != 0xcc000000)))
		return -EINVAL;
	chrome9_count_vertex_payload(body + 160 + delta, vertex_count * 6);
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	return chrome9_finish_ring(base, p, pause, &job->segment_stop);
}

static int chrome9_build_copy_rect(struct chrome9_engine *engine,
		const struct chrome9_draw_request *request,
		struct chrome9_job *job, u32 target, u32 source, u32 *pause)
{
	static const u32 pre_sync[] = { 0x6c, 0x00c00000 };
	static const u32 post_sync[] = { 0x6c, 0x00400000 };
	static const u32 clear_state[] = {
		0x30, 0x00000000,
		0x34, 0x00000000,
		0x38, 0x00000000,
		0x48, 0x00000000,
	};
	u32 copy[] = {
		0x04, 0x00000300,
		0x1c, source >> 3,
		0x14, target >> 3,
		0x08, 0x40001000 |
		      ((request->pitch >> 3) << 16) |
		      (request->texture_pitch >> 3),
		0x18, request->source_y << 16 | request->source_x,
		0x10, request->destination_y << 16 | request->destination_x,
		0x0c, (request->copy_height - 1) << 16 |
		      (request->copy_width - 1),
		0x00, 0xcc000001,
	};
	u32 alpha_mask[] = {
		0x48, 0x70000000,
	};
	static const u32 preserve_alpha[] = { 0x48, 0x80000000 };
	u32 alpha_fill[] = {
		0x04, 0x00000300,
		0x14, target >> 3,
		0x08, 0x40000000 | ((request->pitch >> 3) << 16),
		0x10, request->destination_y << 16 | request->destination_x,
		0x0c, (request->copy_height - 1) << 16 |
		      (request->copy_width - 1),
		0x58, 0xff000000,
		0x00, 0xf0002001,
	};
	static const u32 reset_mask[] = { 0x48, 0x00000000 };
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 *base = engine->staging_cpu;
	u32 *p;

	if (!request->copy_width || !request->copy_height ||
	    !IS_ALIGNED(target, 16) || !IS_ALIGNED(source, 16))
		return -EINVAL;
	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, pre_sync, 1);
	chrome9_emit_header0(&p, clear_state, 4);
	if (request->flags &
	    DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA)
		chrome9_emit_header0(&p, preserve_alpha, 1);
	chrome9_emit_header0(&p, copy, 8);
	chrome9_emit_header0(&p, post_sync, 1);
	if (request->flags & DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE) {
		/*
		 * The Windows H6 builders use 48[31:28] as byte-inhibit bits.
		 * Keep the copy and masked fill in separate, ordered H0 packets so
		 * no address or geometry state is changed while the copy is live.
		 */
		chrome9_emit_header0(&p, alpha_mask, 1);
		chrome9_emit_header0(&p, alpha_fill, 7);
		chrome9_emit_header0(&p, post_sync, 1);
		chrome9_emit_header0(&p, reset_mask, 1);
	}
	if (request->flags &
	    DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA)
		chrome9_emit_header0(&p, reset_mask, 1);
	chrome9_emit_header0(&p, cookie, 1);
	return chrome9_finish_ring(base, p, pause, &job->segment_stop);
}

static int chrome9_build_textured_triangle(struct chrome9_engine *engine,
		const struct chrome9_draw_request *request,
		struct chrome9_job *job, u32 target, u32 texture, u32 depth,
		u32 *pause)
{
	static const u32 destination_tail[] = {
		0x38000000, 0x3b000000, 0x39000000, 0x3c000000,
		0x34000150, 0x35000114, 0x36000500, 0x37000200,
	};
	static const u32 config[] = {
		0x00000011, 0x01002521, 0x08010101, 0x04000000,
		0x05000000, 0x0d810000,
	};
	static const u32 raster_fvf_template[] = {
		0x00000308, 0x010000c2, 0x020041d1, 0x030052d0,
		0x04006342, 0x1200000f, 0x61800000, 0x623081ff,
		0x20000000, 0x23004f02, 0x24000003, 0x25f6cc00,
		0x28ffcc00, 0x40800000, 0x41400000, 0x4200493f,
		0x43800000, 0x44400000, 0x4500493f, 0x46800000,
		0x47400000, 0x4800003f, 0x49000000, 0x50000000,
		0x51000000, 0x52147fff,
	};
	static const u32 ps_constants[] = { 0, 0, 0, 0 };
	static const u32 vs_float_constants[] = {
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 vs_integer_constants[] = { 0, 0, 0, 0 };
	static const u32 vs_program[] = {
		0x00380150, 0x00000000, 0x00000000, 0x0008010f,
		0x0008014c, 0x00100150, 0x00384154, 0x0004021f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
		0x0008c14c, 0x00000000, 0x00000000, 0x0008028f,
	};
	static const u32 vs_control[] = { 0x60800004 };
	static const u32 replace_control[] = {
		0x90018000, 0x91000001, 0x92010001, 0x93000001,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 modulate_control[] = {
		0x90018000, 0x91000001, 0x92010001, 0x93000001,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 replace_tau[] = { 0x301101b0 };
	static const u32 modulate_tau[] = { 0x301001b0 };
	static const u32 replace_program[] = {
		0x01c00000, 0x301c0000, 0x00f20105, 0x000d083f,
	};
	static const u32 modulate_program[] = {
		0x0140b6d0, 0x30040053, 0x00f20005, 0x00000820,
	};
	static const u32 sync[] = { 0x6c, 0x00800000 };
	struct chrome9_blend_words blend_words;
	u32 destination_state[16];
	u32 texture_state[6];
	u32 level_table[8] = { 0 };
	u32 sampler[4];
	u32 common[12] = {
		0x860000ff, 0x870000ff, 0x53010c0f, 0x00000011,
		0x01c01540, 0x88001ed0, 0x89000800, 0xcc000000,
	};
	u32 clip[] = {
		0x80000000 | request->height,
		0x81000000 | request->width,
		chrome9_scissor_y(request),
		chrome9_scissor_x(request),
	};
	u32 raster_fvf[ARRAY_SIZE(raster_fvf_template)];
	bool mip = request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP;
	bool run = request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN || mip;
	void *staging = mip ? engine->run_staging_cpu : engine->staging_cpu;
	size_t staging_size = mip ? CHROME9_RUN_STAGING_SIZE : PAGE_SIZE;
	u32 max_triangles = run ? DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES :
		DRM_CHROME9_TEXTURED_TRIANGLE_BATCH_MAX_TRIANGLES;
	u32 *base = staging;
	u32 *body;
	u32 *p;
	u32 *vertex_header;
	u32 delta = 0;
	u32 depth_delta = 0;
	u32 mip_delta = mip ? 12 : 0;
	u32 ps_offset;
	u32 depth_func = (request->flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
			 DRM_CHROME9_DEPTH_FUNC_SHIFT;
	u32 triangle_count = request->triangle_count ?: 1;
	u32 vertex_count = triangle_count * 3;
	unsigned int destination_count;
	bool blend = chrome9_draw_has_blend(request);
	bool depth_active =
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16 ||
		request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND ||
		(run && (request->flags & DRM_CHROME9_TEXTURE_RUN_DEPTH));
	bool depth_blend = request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND ||
		(run && depth_active && blend);
	unsigned int common_count = 8;
	unsigned int i;
	u32 log_size = ilog2(request->texture_height) << 4 |
		       ilog2(request->texture_width);
	int ret;

	if (!base || !triangle_count || triangle_count > max_triangles)
		return -E2BIG;
	memcpy(raster_fvf, raster_fvf_template, sizeof(raster_fvf));
	raster_fvf[10] = 0x24000000 | vertex_count;

	destination_state[0] = 0x50000000 | (target >> 8);
	destination_state[1] = 0x51100000;
	destination_state[2] = 0x52000000 |
		(request->destination_format ==
		 DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ? 0x00990000 :
		 0x00980000) | (request->pitch >> 5);
	if (blend) {
		chrome9_lower_blend(&request->blend, &blend_words);
		for (i = 0; i < 8; i++)
			destination_state[3 + i] = (0x38 + i) << 24;
		destination_state[11] = blend_words.a34;
		destination_state[12] = blend_words.a35;
		destination_state[13] = blend_words.a36;
		destination_state[14] = blend_words.a37;
		destination_state[15] =
			(request->blend.flags &
			 DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
			blend_words.a33 : 0x33007000;
		destination_count = depth_blend ? ARRAY_SIZE(destination_state) :
			ARRAY_SIZE(destination_state) - 1;
		common[2] = blend_words.a53;
		common[3] = blend_words.a00;
		if ((request->blend.flags &
		     DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) && !depth_blend)
			common[7] = blend_words.a33;
		delta = 4;
	} else {
		memcpy(destination_state + 3, destination_tail,
		       sizeof(destination_tail));
		destination_count = 3 + ARRAY_SIZE(destination_tail);
	}
	if (is_power_of_2(request->texture_width) &&
	    is_power_of_2(request->texture_height))
		log_size |= BIT(15);
	texture_state[0] = texture >> 8;
	texture_state[1] = 0x18001101;
	texture_state[2] = 0x20000000 | request->texture_height << 12 |
			   request->texture_width;
	texture_state[3] = 0x21000001;
	texture_state[4] = 0x22000000 | log_size;
	texture_state[5] = 0x30000000 |
		(request->texture_format == DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ?
		 0x00990000 : 0x00980000);
	if (mip) {
		for (i = 1; i <= min(request->texture_last_level, 8U); i++)
			level_table[i - 1] = 0x40000000 |
				((request->texture_level_offsets[i] - PAGE_SIZE) >> 8);
		sampler[0] = 0x30000000 |
			((request->lod_bias_s5_5 & 0x3ff) << 12) |
			(request->texture_last_level << 6);
	} else {
		/* Explicitly select level zero; do not inherit stale mip offsets. */
		sampler[0] = 0x30000000;
	}
	sampler[1] = 0x31100000 | request->mag_filter << 13 |
		     request->min_filter << 10 | request->mag_filter << 7 |
		     request->min_filter << 4 |
		     (mip ? request->mip_filter : 0);
	sampler[2] = 0x32000040 | request->wrap_t << 3 | request->wrap_s;
	sampler[3] = 0xcccccccc;
	if (request->texture_program == DRM_CHROME9_TEXTURE_PROGRAM_MODULATE)
		common[4] = 0x01ce1540;
	if (depth_active) {
		common[4] = chrome9_depth_control_word(
			request, request->texture_program ==
			DRM_CHROME9_TEXTURE_PROGRAM_MODULATE);
		common[7] = 0x10000000 | (depth >> 8);
		common[8] = 0x11000001 |
			((request->depth_pitch >> 5) << 13);
		common[9] = 0x12380000;
		common[10] = 0x13000000 | depth_func << 16;
		common[11] = 0x20007fff;
		common_count = ARRAY_SIZE(common);
		depth_delta = 4;
	}
	memset(base, 0xcc, staging_size);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     destination_state, destination_count);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00020000,
			     texture_state, ARRAY_SIZE(texture_state));
	if (mip)
		chrome9_emit_header3(&p, 0x10030000, level_table,
				     ARRAY_SIZE(level_table));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x20020000,
			     sampler, ARRAY_SIZE(sampler));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     common, common_count);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0xfe020000,
			     config, ARRAY_SIZE(config));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
			     raster_fvf, ARRAY_SIZE(raster_fvf));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x22030000,
			     ps_constants, ARRAY_SIZE(ps_constants));
	chrome9_emit_header3(&p, 0x32030400, vs_float_constants,
			     ARRAY_SIZE(vs_float_constants));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x33030000,
			     vs_integer_constants, ARRAY_SIZE(vs_integer_constants));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     clip, ARRAY_SIZE(clip));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x31030000,
			     vs_program, ARRAY_SIZE(vs_program));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
			     vs_control, ARRAY_SIZE(vs_control));
	if (request->texture_program == DRM_CHROME9_TEXTURE_PROGRAM_MODULATE) {
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
				     modulate_control, ARRAY_SIZE(modulate_control));
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x21030000,
				     modulate_tau, ARRAY_SIZE(modulate_tau));
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x20030000,
				     modulate_program, ARRAY_SIZE(modulate_program));
	} else {
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
				     replace_control, ARRAY_SIZE(replace_control));
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x21030000,
				     replace_tau, ARRAY_SIZE(replace_tau));
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x20030000,
				     replace_program, ARRAY_SIZE(replace_program));
	}
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00000000, NULL, 0);
	vertex_header = p - 4;
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_textured_vertex *src =
			request->textured_batch_vertices ?
			&request->textured_batch_vertices[i] :
			&request->textured_vertices[i];

		*p++ = chrome9_q16_to_float(src->x_16_16);
		*p++ = chrome9_q16_to_float(src->y_16_16);
		*p++ = chrome9_q16_to_float(src->z_16_16);
		*p++ = src->rhw_float;
		*p++ = src->color;
		*p++ = src->specular;
		*p++ = src->s_float;
		*p++ = src->t_float;
	}
	ps_offset = 160 + delta + depth_delta + mip_delta;
	if (p - body != delta + depth_delta + mip_delta + 224 +
	    (vertex_count - 3) * 8 ||
	    body[4] != destination_state[0] ||
	    body[5] != 0x51100000 || body[6] != destination_state[2] ||
	    body[20 + delta] != texture_state[0] ||
	    body[21 + delta] != 0x18001101 ||
	    (mip &&
	     (body[28 + delta] != 0xfe03043c ||
	      body[29 + delta] != ARRAY_SIZE(level_table) ||
	      body[30 + delta] != 0x10030000 ||
	      body[31 + delta] != 0xcc000000 ||
	      memcmp(&body[32 + delta], level_table, sizeof(level_table)))) ||
	    body[32 + delta + mip_delta] != sampler[0] ||
	    body[33 + delta + mip_delta] != sampler[1] ||
	    body[34 + delta + mip_delta] != sampler[2] ||
	    body[55 + delta + depth_delta + mip_delta] != 0x04000000 ||
	    (!depth_active &&
	     body[74 + delta + mip_delta] !=
		(0x24000000 | vertex_count)) ||
	    (blend &&
	     (body[7] != 0x38000000 || body[14] != 0x3f000000 ||
	      body[15] != blend_words.a34 || body[16] != blend_words.a35 ||
	      body[17] != blend_words.a36 || body[18] != blend_words.a37 ||
	      (depth_blend && body[19] != destination_state[15]) ||
	      body[42 + delta + mip_delta] != blend_words.a53 ||
	      body[43 + delta + mip_delta] != blend_words.a00 ||
	      body[44 + delta + mip_delta] != common[4] ||
	      body[47 + delta + mip_delta] != common[7])) ||
	    (depth_active &&
	     (body[44 + delta + mip_delta] !=
		chrome9_depth_control_word(
			request, request->texture_program ==
			DRM_CHROME9_TEXTURE_PROGRAM_MODULATE) ||
	      body[47 + delta + mip_delta] !=
		(0x10000000 | (depth >> 8)) ||
	      body[48 + delta + mip_delta] != (0x11000001 |
		((request->depth_pitch >> 5) << 13)) ||
	      body[49 + delta + mip_delta] != 0x12380000 ||
	      body[50 + delta + mip_delta] !=
		(0x13000000 | depth_func << 16) ||
	      body[51 + delta + mip_delta] != 0x20007fff ||
	      body[76 + delta + mip_delta] != 0x20000000 ||
	      body[77 + delta + mip_delta] != 0x23004f02 ||
	      body[78 + delta + mip_delta] !=
		(0x24000000 | vertex_count) ||
	      body[200 + delta + mip_delta] != 0xfe020440 ||
	      body[202 + delta + mip_delta] != 0x00000000)) ||
	    (request->texture_program == DRM_CHROME9_TEXTURE_PROGRAM_MODULATE &&
	     (body[ps_offset + 6] != 0x92010001 ||
	      body[ps_offset + 20] != 0xfe020440 ||
	      body[ps_offset + 22] != 0x21030000 ||
	      body[ps_offset + 24] != modulate_tau[0] ||
	      body[ps_offset + 28] != 0xfe020440 ||
	      body[ps_offset + 30] != 0x20030000 ||
	      memcmp(&body[ps_offset + 32], modulate_program,
		     sizeof(modulate_program)) ||
	      body[ps_offset + 36] != 0xfe020440 ||
	      body[ps_offset + 38] != 0x00000000)) ||
	    body[196 + delta + depth_delta + mip_delta] !=
		0xfe020440)
		return -EINVAL;
	chrome9_count_vertex_payload(vertex_header, vertex_count * 8);
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	if (chrome9_finish_ring_sized(base, p, staging_size, pause,
				      &job->segment_stop))
		return -E2BIG;
	if (run && triangle_count == DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES &&
	    (*pause != 0x0cf0 || job->segment_stop != 0x0d70 ||
	     raster_fvf[10] != 0x24000048))
		return -EINVAL;
	return 0;
}

static int chrome9_build_two_textured_triangle(struct chrome9_engine *engine,
		const struct chrome9_draw_request *request,
		struct chrome9_job *job, u32 target, u32 texture0, u32 texture1,
		u32 depth, u32 *pause)
{
	static const u32 destination_tail[] = {
		0x38000000, 0x3b000000, 0x39000000, 0x3c000000,
		0x34000150, 0x35000114, 0x36000500, 0x37000200,
	};
	static const u32 config[] = {
		0x00000021, 0x01002521, 0x08010102, 0x04000000,
		0x05000000, 0x0d810000,
	};
	static const u32 raster_fvf[] = {
		0x0000040a, 0x010000c2, 0x020041d1, 0x030052d0,
		0x04006342, 0x05008442, 0x1200001f, 0x61800000,
		0x624081ff, 0x20000000, 0x23004f02, 0x24000003,
		0x25f6cc00, 0x28ffcc00, 0x40800000, 0x41400000,
		0x4200493f, 0x43800000, 0x44400000, 0x4500493f,
		0x46800000, 0x47400000, 0x4800003f, 0x49000000,
		0x50000000, 0x51000000, 0x52147fff,
	};
	static const u32 ps_constants[] = { 0, 0, 0, 0 };
	static const u32 vs_float_constants[] = {
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 vs_integer_constants[] = { 0, 0, 0, 0 };
	static const u32 vs_program[] = {
		0x00380150, 0x00000000, 0x00000000, 0x0008010f,
		0x0008014c, 0x00100150, 0x00384154, 0x0004021f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
		0x0008c14c, 0x00000000, 0x00000000, 0x0008028f,
		0x0009014c, 0x00000000, 0x00000000, 0x0008029f,
	};
	static const u32 vs_control[] = { 0x60800005 };
	static const u32 ps_control[] = {
		0x90018000, 0x91000003, 0x92020002, 0x93000002,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 tau[] = {
		0x301001b0, 0x311211b1, 0xcccccccc, 0xdddddddd,
	};
	static const u32 rgba_program[] = {
		0x0140b6d0, 0x30202053, 0x00f20005, 0x0000007f,
		0x01c00000, 0x301c0000, 0x00f20105, 0x000d083f,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 tex0_alpha_control[] = {
		0x90000000, 0x91000001, 0x92020004, 0x93000002,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 tex0_alpha_tau[] = {
		0xa01001b0, 0x311001b1, 0xcccccccc, 0xdddddddd,
	};
	static const u32 tex0_alpha_program[] = {
		0x0140b6d0, 0x3003f000, 0x00f00005, 0x000003e0,
		0x0140b6d0, 0x50040055, 0x00703f00, 0x000003e0,
		0x0140b6d0, 0xb0040b6b, 0x008200b6, 0x200003e0,
		0x01c00530, 0x001c0053, 0x00f03f00, 0x000d0820,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 sync[] = { 0x6c, 0x00800000 };
	struct chrome9_blend_words blend_words;
	u32 destination_state[16];
	u32 image[2][6];
	u32 sampler[2][4];
	u32 common[12] = {
		0x860000ff, 0x870000ff, 0x53010c0f, 0x00000011,
		0x01c01540, 0x88001ed0, 0x89000800, 0xcc000000,
	};
	u32 clip[] = {
		0x80000000 | request->height,
		0x81000000 | request->width,
		chrome9_scissor_y(request),
		chrome9_scissor_x(request),
	};
	u32 vertices[3 * 10];
	u32 *base = engine->staging_cpu;
	u32 *body;
	u32 *p;
	u32 *vertex_header;
	u32 delta = 0;
	u32 depth_delta = 0;
	u32 depth_func = (request->flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
			 DRM_CHROME9_DEPTH_FUNC_SHIFT;
	unsigned int common_count = 8;
	unsigned int i;
	bool blend = request->kind ==
		CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16_BLEND;
	bool depth_active =
		request->kind ==
			CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16 ||
		blend;
	bool tex0_alpha_primary = request->flags &
		DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY;
	const u32 *selected_control = tex0_alpha_primary ?
		tex0_alpha_control : ps_control;
	const u32 *selected_tau = tex0_alpha_primary ? tex0_alpha_tau : tau;
	const u32 *selected_program = tex0_alpha_primary ?
		tex0_alpha_program : rgba_program;
	unsigned int selected_program_count = tex0_alpha_primary ?
		ARRAY_SIZE(tex0_alpha_program) : ARRAY_SIZE(rgba_program);
	u32 program_delta = tex0_alpha_primary ? 8 : 0;
	unsigned int destination_count;
	const u32 addresses[2] = { texture0, texture1 };
	const u32 widths[2] = { request->texture_width,
				 request->texture2_width };
	const u32 heights[2] = { request->texture_height,
				  request->texture2_height };
	const u32 formats[2] = { request->texture_format,
				  request->texture2_format };
	const u32 min_filters[2] = { request->min_filter,
				      request->texture2_min_filter };
	const u32 mag_filters[2] = { request->mag_filter,
				      request->texture2_mag_filter };
	const u32 wrap_s[2] = { request->wrap_s, request->texture2_wrap_s };
	const u32 wrap_t[2] = { request->wrap_t, request->texture2_wrap_t };
	int ret;

	destination_state[0] = 0x50000000 | (target >> 8);
	destination_state[1] = 0x51100000;
	destination_state[2] = 0x52000000 |
		(request->destination_format ==
		 DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ? 0x00990000 :
		 0x00980000) | (request->pitch >> 5);
	if (blend) {
		chrome9_lower_blend(&request->blend, &blend_words);
		for (i = 0; i < 8; i++)
			destination_state[3 + i] = (0x38 + i) << 24;
		destination_state[11] = blend_words.a34;
		destination_state[12] = blend_words.a35;
		destination_state[13] = blend_words.a36;
		destination_state[14] = blend_words.a37;
		destination_state[15] =
			(request->blend.flags &
			 DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
			blend_words.a33 : 0x33007000;
		destination_count = ARRAY_SIZE(destination_state);
		common[2] = blend_words.a53;
		common[3] = blend_words.a00;
		delta = 4;
	} else {
		memcpy(destination_state + 3, destination_tail,
		       sizeof(destination_tail));
		destination_count = 3 + ARRAY_SIZE(destination_tail);
	}
	for (i = 0; i < 2; i++) {
		u32 log_size = ilog2(heights[i]) << 4 | ilog2(widths[i]);

		if (is_power_of_2(widths[i]) && is_power_of_2(heights[i]))
			log_size |= BIT(15);

		image[i][0] = addresses[i] >> 8;
		image[i][1] = 0x18001101;
		image[i][2] = 0x20000000 | heights[i] << 12 | widths[i];
		image[i][3] = 0x21000001;
		image[i][4] = 0x22000000 | log_size;
		image[i][5] = 0x30000000 |
			(formats[i] == DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ?
			 0x00990000 : 0x00980000);
		sampler[i][0] = 0x30000000;
		sampler[i][1] = 0x31100000 | mag_filters[i] << 13 |
				min_filters[i] << 10 | mag_filters[i] << 7 |
				min_filters[i] << 4;
		sampler[i][2] = 0x32000040 | wrap_t[i] << 3 | wrap_s[i];
		sampler[i][3] = 0xcccccccc;
	}
	if (tex0_alpha_primary)
		common[4] = 0x01ce1540;
	if (depth_active) {
		common[4] = chrome9_depth_control_word(request,
					      tex0_alpha_primary);
		common[7] = 0x10000000 | (depth >> 8);
		common[8] = 0x11000001 |
			((request->depth_pitch >> 5) << 13);
		common[9] = 0x12380000;
		common[10] = 0x13000000 | depth_func << 16;
		common[11] = 0x20007fff;
		common_count = ARRAY_SIZE(common);
		depth_delta = 4;
	}
	for (i = 0; i < ARRAY_SIZE(request->two_textured_vertices); i++) {
		const struct drm_chrome9_two_textured_vertex *src =
			&request->two_textured_vertices[i];
		u32 *dst = vertices + i * 10;

		dst[0] = chrome9_q16_to_float(src->x_16_16);
		dst[1] = chrome9_q16_to_float(src->y_16_16);
		dst[2] = chrome9_q16_to_float(src->z_16_16);
		dst[3] = src->rhw_float;
		dst[4] = src->color;
		dst[5] = src->specular;
		dst[6] = src->s0_float;
		dst[7] = src->t0_float;
		dst[8] = src->s1_float;
		dst[9] = src->t1_float;
	}

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     destination_state, destination_count);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00020000,
			     image[0], ARRAY_SIZE(image[0]));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x20020000,
			     sampler[0], ARRAY_SIZE(sampler[0]));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x01020000,
			     image[1], ARRAY_SIZE(image[1]));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x21020000,
			     sampler[1], ARRAY_SIZE(sampler[1]));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     common, common_count);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0xfe020000,
			     config, ARRAY_SIZE(config));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
			     raster_fvf, ARRAY_SIZE(raster_fvf));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x22030000,
			     ps_constants, ARRAY_SIZE(ps_constants));
	chrome9_emit_header3(&p, 0x32030400, vs_float_constants,
			     ARRAY_SIZE(vs_float_constants));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x33030000,
			     vs_integer_constants, ARRAY_SIZE(vs_integer_constants));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     clip, ARRAY_SIZE(clip));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x31030000,
			     vs_program, ARRAY_SIZE(vs_program));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
			     vs_control, ARRAY_SIZE(vs_control));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     selected_control, ARRAY_SIZE(ps_control));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x21030000,
			     selected_tau, ARRAY_SIZE(tau));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x20030000,
			     selected_program, selected_program_count);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00000000,
			     vertices, ARRAY_SIZE(vertices));
	vertex_header = p - ARRAY_SIZE(vertices) - 4;
	if (p - body != 262 + delta + depth_delta + program_delta ||
	    body[4] != destination_state[0] ||
	    body[6] != destination_state[2] ||
	    (blend &&
	     (body[7] != 0x38000000 || body[14] != 0x3f000000 ||
	      body[15] != blend_words.a34 || body[16] != blend_words.a35 ||
	      body[17] != blend_words.a36 || body[18] != blend_words.a37 ||
	      body[19] != destination_state[15])) ||
	    body[20 + delta] != image[0][0] ||
	    body[21 + delta] != 0x18001101 ||
	    body[24 + delta] != image[0][4] ||
	    body[25 + delta] != image[0][5] ||
	    body[32 + delta] != 0x30000000 ||
	    body[40 + delta] != image[1][0] ||
	    body[41 + delta] != 0x18001101 ||
	    body[44 + delta] != image[1][4] ||
	    body[45 + delta] != image[1][5] ||
	    body[52 + delta] != 0x30000000 ||
	    (depth_active &&
	     (body[64 + delta] !=
		chrome9_depth_control_word(request, tex0_alpha_primary) ||
	      body[67 + delta] != (0x10000000 | (depth >> 8)) ||
	      body[68 + delta] != (0x11000001 |
		((request->depth_pitch >> 5) << 13)) ||
	      body[69 + delta] != 0x12380000 ||
	      body[70 + delta] != (0x13000000 | depth_func << 16) ||
	      body[71 + delta] != 0x20007fff)) ||
	    (!depth_active && body[64 + delta] !=
		(tex0_alpha_primary ? 0x01ce1540 : 0x01c01540)) ||
	    body[72 + delta + depth_delta] != 0x00000021 ||
	    body[74 + delta + depth_delta] != 0x08010102 ||
	    body[75 + delta + depth_delta] != 0x04000000 ||
	    body[84 + delta + depth_delta] != 0x0000040a ||
	    body[89 + delta + depth_delta] != 0x05008442 ||
	    body[92 + delta + depth_delta] != 0x624081ff ||
	    body[93 + delta + depth_delta] != 0x20000000 ||
	    body[94 + delta + depth_delta] != 0x23004f02 ||
	    body[95 + delta + depth_delta] != 0x24000003 ||
	    body[172 + delta + depth_delta] != 0x0009014c ||
	    body[175 + delta + depth_delta] != 0x0008029f ||
	    body[180 + delta + depth_delta] != 0x60800005 ||
	    body[190 + delta + depth_delta] != selected_control[2] ||
	    body[208 + delta + depth_delta] != selected_tau[0] ||
	    body[209 + delta + depth_delta] != selected_tau[1] ||
	    body[216 + delta + depth_delta] != selected_program[0] ||
	    body[217 + delta + depth_delta] != selected_program[1] ||
	    body[219 + delta + depth_delta] != selected_program[3] ||
	    body[216 + delta + depth_delta + selected_program_count - 2] !=
		0xcccccccc ||
	    body[217 + delta + depth_delta + selected_program_count - 2] !=
		0xdddddddd ||
	    body[228 + delta + depth_delta + program_delta] != 0xfe020440 ||
	    body[230 + delta + depth_delta + program_delta] != 0x00000000)
		return -EINVAL;
	chrome9_count_vertex_payload(vertex_header, ARRAY_SIZE(vertices));
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	return chrome9_finish_ring(base, p, pause, &job->segment_stop);
}

static u32 chrome9_fragment_texture_format(u32 format)
{
	switch (format) {
	case DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888:
		return 0x98;
	case DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888:
		return 0x99;
	case DRM_CHROME9_FRAGMENT_TEXTURE_A8:
		return 0x1b;
	case DRM_CHROME9_FRAGMENT_TEXTURE_L8:
		return 0x13;
	case DRM_CHROME9_FRAGMENT_TEXTURE_L8A8:
		return 0x15;
	case DRM_CHROME9_FRAGMENT_TEXTURE_RGB565:
		return 0x89;
	case DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24:
		return 0xfe;
	default:
		return 0;
	}
}

static void chrome9_fragment_mip_state(
	const struct drm_chrome9_fragment_texture_state *texture, u32 mip[8])
{
	u32 level_count = texture->last_level - texture->first_level;
	unsigned int level;

	memset(mip, 0, 8 * sizeof(*mip));
	for (level = 1; level <= min(level_count, 8U); level++)
		mip[level - 1] = 0x40000000 |
			((texture->level_offset[texture->first_level + level] -
			  texture->level_offset[texture->first_level]) >> 8);
}

static void chrome9_fragment_sampler_state(
	const struct drm_chrome9_fragment_texture_state *texture, u32 sampler[4])
{
	u32 lod_min = texture->min_lod_s5_5 / 32;
	u32 lod_max = texture->max_lod_s5_5 / 32;

	sampler[0] = 0x30000000 |
		((u32)texture->lod_bias_s5_5 & 0x3ff) << 12 |
		lod_max << 6 | lod_min;
	sampler[1] = 0x31100000 | texture->mag_filter << 13 |
		texture->min_filter << 10 | texture->mag_filter << 7 |
		texture->min_filter << 4 | texture->mip_filter;
	sampler[2] = 0x32000040 | texture->wrap_t << 3 | texture->wrap_s;
	sampler[3] = 0xcccccccc;
}

static u32 chrome9_fragment_varying_format(u32 width)
{
	static const u8 format[] = { 0, 0x02, 0x42, 0x82, 0xc2 };

	return format[width];
}

static u32 chrome9_fragment_varying_link(u32 width)
{
	static const u8 link[] = { 0, 0x8, 0xc, 0xe, 0xf };

	return link[width];
}

/* Texture state caps PS consumer fields at three, as ComputeTexDim does,
 * but a direct ALU read of W must keep the fourth produced lane.
 */
static u32 chrome9_fragment_consumer_width(u32 producer_width,
	u32 texture_count, const u8 *alu_width, unsigned int varying)
{
	u32 cap = texture_count ? 3 : 4;

	if (alu_width)
		cap = max_t(u32, cap, alu_width[varying]);
	return min(producer_width, cap);
}

/* A25/A26 describe the VS producer, A28/A29 the PS consumer. */
static void chrome9_fragment_link_words(u32 varying_count,
		const u8 *varying_width, u32 texture_count, const u8 *alu_width,
		u32 link[4])
{
	unsigned int varying;

	link[0] = 0x25f60000;
	link[1] = 0x26000000;
	link[2] = 0x28ff0000;
	link[3] = 0x29000000;
	for (varying = 0; varying < varying_count; varying++) {
		u32 shift = 12 - 4 * (varying & 3);
		u32 producer = chrome9_fragment_varying_link(
			varying_width[varying]);
		u32 consumer = chrome9_fragment_varying_link(
			chrome9_fragment_consumer_width(varying_width[varying],
				texture_count, alu_width, varying));

		link[varying < 4 ? 0 : 1] |= producer << shift;
		link[varying < 4 ? 2 : 3] |= consumer << shift;
	}
}

static u32 chrome9_fragment_raster_state(
		u32 varying_count, const u8 *varying_width, u32 texture_count,
		const u8 *alu_width, u32 vertex_count,
		u32 raster[40])
{
	static const u32 zero_varying[] = {
		0x00000206, 0x010000c2, 0x020041d1, 0x030052d0,
		0x04006342, 0x1200001f, 0x61800000, 0x622081ff,
		0x20000000, 0x23004f02, 0x24000000, 0x25f6cc00,
		0x28ffcc00, 0x40800000, 0x41400000, 0x4200493f,
		0x43800000, 0x44400000, 0x4500493f, 0x46800000,
		0x47400000, 0x4800003f, 0x49000000, 0x50000000,
		0x51000000, 0x52147fff,
	};
	u32 vertex_stride = 6;
	u32 link[4];
	u32 offset = 6;
	unsigned int i;

	memset(raster, 0, 40 * sizeof(*raster));
	if (!varying_count) {
		memcpy(raster, zero_varying, sizeof(zero_varying));
		raster[10] |= vertex_count;
		return ARRAY_SIZE(zero_varying);
	}
	for (i = 0; i < varying_count; i++)
		vertex_stride += varying_width[i];
	raster[0] = ((2 + varying_count) << 8) | vertex_stride;
	raster[1] = 0x010000c2;
	raster[2] = 0x020041d1;
	raster[3] = 0x030052d1;
	for (i = 0; i < varying_count; i++) {
		u32 width = varying_width[i];

		raster[4 + i] = (4 + i) << 24 | offset << 12 |
			(3 + i) << 8 | chrome9_fragment_varying_format(width);
		offset += width;
	}
	i = 4 + varying_count;
	raster[i++] = 0x12000000 | GENMASK(2 + varying_count, 0);
	raster[i++] = 0x61800000;
	/* H6 resource high-water is D2 plus the dense T inputs. */
	raster[i++] = 0x620081ff | (2 + varying_count) << 20;
	raster[i++] = 0x20000000;
	raster[i++] = 0x23004f02;
	raster[i++] = 0x24000000 | vertex_count;
	chrome9_fragment_link_words(varying_count, varying_width, texture_count,
				    alu_width, link);
	raster[i++] = link[0];
	raster[i++] = link[1];
	raster[i++] = link[2];
	raster[i++] = link[3];
	raster[i++] = 0x40800000;
	raster[i++] = 0x41400000;
	raster[i++] = 0x4200493f;
	raster[i++] = 0x43800000;
	raster[i++] = 0x44400000;
	raster[i++] = 0x4500493f;
	raster[i++] = 0x46800000;
	raster[i++] = 0x47400000;
	raster[i++] = 0x4800003f;
	raster[i++] = 0x49000000;
	raster[i++] = 0x50000000;
	raster[i++] = 0x51000000;
	raster[i++] = 0x52147fff;
	return i;
}

#define CHROME9_TCL_RASTER_WORDS	48

/* Stream slot (3003 n): 1-KiB base in [31:10], pitch in dwords in [8:0]. */
static u32 chrome9_tcl_stream_slot(u32 address, u32 stride)
{
	return (address & ~0x3ffU) | (stride >> 2 & 0x1ff);
}

/*
 * Stage A TCL raster/FVF block: raw application elements feed v0..vN-1,
 * the output linkage stays byte-identical to the post-TNL path and
 * A40..A48 carry the clip engine's copy of rows 256/257.
 */
static u32 chrome9_tcl_raster_state(const struct chrome9_fragment_program *fp,
		const struct chrome9_tcl_draw *tcl, const u32 *addresses,
		u32 raster[CHROME9_TCL_RASTER_WORDS])
{
	u32 planes = hweight32(tcl->clip_mask);
	u32 groups = DIV_ROUND_UP(planes, 3);
	u32 outputs = fp->varying_count || groups ?
		      3 + fp->varying_count + groups : 5;
	u32 link[4];
	unsigned int e, g, i = 0;

	/* Stage B: SF memory type (A00[16:15] = 01); element bits 19:12 hold
	 * address bits 9:2 inside the slot's 1-KiB window. */
	raster[i++] = (tcl->element_count - 1) << 8 | tcl->stride_dw |
		      (addresses ? BIT(15) : 0);
	for (e = 0; e < tcl->element_count; e++)
		raster[i++] = (1 + e) << 24 |
			      (addresses ? (addresses[e] >> 2) & 0xff :
					   tcl->element_offset[e]) << 12 |
			      e << 8 | tcl->element_format[e];
	/* Superset of the input and output masks; the vendor quad uses 1f. */
	raster[i++] = 0x12000000 |
		      GENMASK(max(outputs, tcl->element_count) - 1, 0);
	raster[i++] = tcl->program.a61 &
		      ~(tcl->re & CHROME9_TCL_RE_NO_A61_BFC ? BIT(19) : 0);
	raster[i++] = tcl->program.a62;
	/* A20[0] index mode, A20[1] 32-bit indices. */
	raster[i++] = addresses ? 0x20000001 | (tcl->index32 ? 2 : 0) :
		      0x20000000;
	/* A23 bit 8 selects the culled physical winding (set = CCW); bits
	 * 14:12 select Gouraud (4) or flat from the third vertex C (3). */
	raster[i++] = ((tcl->cull & DRM_CHROME9_VERTEX_CULL_CW ||
			tcl->re & CHROME9_TCL_RE_A23_CULL_CW ?
			0x23004e02 : 0x23004f02) & ~GENMASK(14, 12)) |
		      (tcl->re & CHROME9_TCL_RE_A23_2FRENDER ? BIT(4) : 0) |
		      (tcl->cull & DRM_CHROME9_VERTEX_FLAT ? 0x3000 : 0x4000) |
		      /* A23[7]: set = CCW triangles take oBFD (vendor front CW). */
		      (tcl->cull & DRM_CHROME9_VERTEX_BFC_CCW ? BIT(7) : 0);
	raster[i++] = 0x24000000 |
		      (addresses ? tcl->index_count : tcl->vertex_count);
	if (fp->varying_count || groups) {
		chrome9_fragment_link_words(fp->varying_count, fp->varying_width,
					    fp->texture_count,
					    fp->varying_alu_width, link);
		/* UCP coordinates are VS outputs only (stmUpdateFVFOut_H5i). */
		for (g = 0; g < groups; g++) {
			u32 coordinate = fp->varying_count + g;
			u32 lanes = min(planes - 3 * g, 3U);

			link[coordinate < 4 ? 0 : 1] |=
				chrome9_fragment_varying_link(lanes) <<
				(12 - 4 * (coordinate & 3));
			if (tcl->re & CHROME9_TCL_RE_UCP_CONSUMER)
				link[coordinate < 4 ? 2 : 3] |=
					chrome9_fragment_varying_link(lanes) <<
					(12 - 4 * (coordinate & 3));
		}
		memcpy(raster + i, link, sizeof(link));
		i += ARRAY_SIZE(link);
	} else {
		raster[i++] = 0x25f6cc00;
		raster[i++] = 0x28ffcc00;
	}
	if (tcl->re & CHROME9_TCL_RE_NO_SECONDARY)
		for (e = 0; e < i; e++)
			if (raster[e] >> 24 == 0x25 || raster[e] >> 24 == 0x28)
				raster[e] &= ~BIT(17);
	for (e = 0; e < 3; e++) {
		u32 scale = tcl->rows[e];
		u32 offset = tcl->rows[4 + e];

		raster[i++] = (0x40 + 3 * e) << 24 | (scale & 0x00ffffff);
		raster[i++] = (0x41 + 3 * e) << 24 | (offset & 0x00ffffff);
		raster[i++] = (0x42 + 3 * e) << 24 | (offset >> 24) << 8 |
			      scale >> 24;
	}
	raster[i++] = 0x49000000;
	raster[i++] = 0x50000000;
	raster[i++] = 0x51000000;
	raster[i++] = 0x52147fff;
	return i;
}

/* The proven VS oracle rows: 32x32 viewport, 786432 snap bias. */
static int chrome9_tcl_raster_selftest(void)
{
	static const u32 expected[] = {
		0x00000206, 0x010000c2, 0x020041d1, 0x030052d1,
		0x1200001f, 0x61800000, 0x62208002, 0x20000000,
		0x23004f02, 0x24000003, 0x25f6cc00, 0x28ffcc00,
		0x40800000, 0x414000f8, 0x42004941, 0x43800000,
		0x444000f8, 0x450049c1, 0x46800000, 0x47000000,
		0x4800003f, 0x49000000, 0x50000000, 0x51000000,
		0x52147fff,
	};
	static const u32 rows[] = {
		0x41800000, 0xc1800000, 0x3f800000, 0x00000000,
		0x494000f8, 0x494000f8, 0x00000000, 0x00000000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 stage_b_address[] = {
		0x00100000, 0x00200000, 0x00300010,
	};
	struct chrome9_fragment_program *fp;
	struct chrome9_tcl_draw *tcl;
	u32 raster[CHROME9_TCL_RASTER_WORDS];
	int ret = -EINVAL;

	fp = kzalloc(sizeof(*fp), GFP_KERNEL);
	tcl = kzalloc(sizeof(*tcl), GFP_KERNEL);
	if (!fp || !tcl) {
		ret = -ENOMEM;
		goto out;
	}
	tcl->element_count = 3;
	tcl->stride_dw = 6;
	tcl->vertex_count = 3;
	tcl->element_format[0] = 0xc2;
	tcl->element_format[1] = 0xd1;
	tcl->element_format[2] = 0xd1;
	tcl->element_offset[1] = 4;
	tcl->element_offset[2] = 5;
	tcl->program.a61 = 0x61800000;
	tcl->program.a62 = 0x62208002;
	memcpy(tcl->rows, rows, sizeof(rows));
	if (chrome9_tcl_raster_state(fp, tcl, NULL, raster) !=
		ARRAY_SIZE(expected) ||
	    memcmp(raster, expected, sizeof(expected)))
		goto out;
	/* Stage B, vendor stream probe: 00100000/12, 00200000/8, 00300010/4. */
	tcl->element_format[0] = 0x82;
	tcl->element_format[1] = 0x42;
	tcl->element_format[2] = 0xc7;
	tcl->index_count = 6;
	tcl->index32 = true;
	if (chrome9_tcl_raster_state(fp, tcl, stage_b_address, raster) !=
		ARRAY_SIZE(expected) ||
	    raster[0] != 0x00008206 || raster[1] != 0x01000082 ||
	    raster[2] != 0x02000142 || raster[3] != 0x030042c7 ||
	    raster[7] != 0x20000003 || raster[9] != 0x24000006 ||
	    chrome9_tcl_stream_slot(stage_b_address[0], 12) != 0x00100003 ||
	    chrome9_tcl_stream_slot(stage_b_address[1], 8) != 0x00200002 ||
	    chrome9_tcl_stream_slot(stage_b_address[2], 4) != 0x00300001 ||
	    chrome9_tcl_stream_slot(0x00100404, 2044) != 0x001005ff)
		goto out;
	/* BFC for CCW, two planes behind one ST varying: oT1.xy, A25 cc. */
	tcl->cull = DRM_CHROME9_VERTEX_BFC_CCW;
	tcl->clip_mask = 0x3;
	fp->varying_count = 1;
	fp->varying_width[0] = 2;
	fp->varying_logical[0] = 0;
	if (chrome9_tcl_raster_state(fp, tcl, NULL, raster) !=
		ARRAY_SIZE(expected) + 2 ||
	    raster[4] != 0x1200001f || raster[8] != 0x23004f82 ||
	    raster[10] != 0x25f6cc00 || raster[11] != 0x26000000 ||
	    raster[12] != 0x28ffc000 || raster[13] != 0x29000000)
		goto out;
	ret = 0;
out:
	kfree(tcl);
	kfree(fp);
	return ret;
}

static bool chrome9_float_finite(u32 value);

/*
 * Vertex element formats: native A(1+i) low byte (components-1 << 6 |
 * type << 1 | normalize, VX900 manual), fetch dwords and which dwords hold
 * float32 or float16 data for the stage A finite check.
 */
static const struct {
	u8 native;
	u8 dwords;
	u8 float32;
	u8 float16;
} chrome9_tcl_formats[] = {
	[DRM_CHROME9_VERTEX_FORMAT_FLOAT1] = { 0x02, 1, 1, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_FLOAT2] = { 0x42, 2, 2, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_FLOAT3] = { 0x82, 3, 3, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_FLOAT4] = { 0xc2, 4, 4, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_BGRA8_UNORM] = { 0xd1, 1, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RGBA8_UNORM] = { 0xc7, 1, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RGBA8_USCALED] = { 0xc6, 1, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RG16_UNORM] = { 0x49, 1, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RGBA16_UNORM] = { 0xc9, 2, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RG16_SNORM] = { 0x4b, 1, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RGBA16_SNORM] = { 0xcb, 2, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RG16_SSCALED] = { 0x4a, 1, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RGBA16_SSCALED] = { 0xca, 2, 0, 0 },
	[DRM_CHROME9_VERTEX_FORMAT_RG16_FLOAT] = { 0x44, 1, 0, 1 },
	[DRM_CHROME9_VERTEX_FORMAT_RGBA16_FLOAT] = { 0xc4, 2, 0, 2 },
};

static bool chrome9_tcl_half_finite(u32 value)
{
	return (value & 0x7c00) != 0x7c00 && (value & 0x7c000000) != 0x7c000000;
}

/*
 * Stage B element byte offsets of the first fetched vertex; every vertex in
 * [index_min, index_min + index_range] (+ base_vertex) must lie inside its
 * object.
 */
static int chrome9_tcl_buffer_bounds(const struct chrome9_tcl_draw *tcl,
		struct drm_gem_object *const *objects, const u32 *buffer_object,
		u64 *element_start, u64 *object_low, u64 *object_high)
{
	s64 first = (s64)tcl->index_min + tcl->base_vertex;
	unsigned int e;

	if (first < 0)
		return -EINVAL;
	for (e = 0; e < tcl->element_count; e++) {
		u8 b = tcl->element_buffer[e];
		const struct drm_chrome9_vertex_buffer *buffer = &tcl->buffers[b];
		u64 start, span, end;

		if (check_mul_overflow((u64)first, (u64)buffer->stride, &start) ||
		    check_add_overflow(start, buffer->offset, &start) ||
		    check_add_overflow(start, (u64)tcl->element_byte_offset[e],
				       &start) ||
		    check_mul_overflow((u64)tcl->index_range,
				       (u64)buffer->stride, &span) ||
		    check_add_overflow(start, span, &end) ||
		    check_add_overflow(end,
			4ULL * chrome9_tcl_formats[tcl->element_type[e]].dwords,
			&end) ||
		    end > objects[buffer_object[b]]->size)
			return -EINVAL;
		element_start[e] = start;
		object_low[buffer_object[b]] = min(object_low[buffer_object[b]],
						   start);
		object_high[buffer_object[b]] = max(object_high[buffer_object[b]],
						    end);
	}
	return 0;
}

static bool chrome9_tcl_buffers_finite(const struct chrome9_tcl_draw *tcl,
		const struct iosys_map *maps, const u32 *buffer_object,
		const u64 *element_start)
{
	unsigned int v, e, c;

	for (e = 0; e < tcl->element_count; e++) {
		u8 type = tcl->element_type[e];
		u8 b = tcl->element_buffer[e];
		const u8 *data = (const u8 *)maps[buffer_object[b]].vaddr +
				 element_start[e];

		if (!chrome9_tcl_formats[type].float32 &&
		    !chrome9_tcl_formats[type].float16)
			continue;
		for (v = 0; v <= tcl->index_range; v++) {
			const u32 *vertex = (const u32 *)(data +
				(size_t)v * tcl->buffers[b].stride);

			for (c = 0; c < chrome9_tcl_formats[type].float32; c++)
				if (!chrome9_float_finite(READ_ONCE(vertex[c])))
					return false;
			for (c = 0; c < chrome9_tcl_formats[type].float16; c++)
				if (!chrome9_tcl_half_finite(READ_ONCE(vertex[c])))
					return false;
		}
	}
	return true;
}

static int chrome9_fragment_raster_selftest(void)
{
	static const u32 expected_zero_varying[] = {
		0x00000206, 0x010000c2, 0x020041d1, 0x030052d0,
		0x04006342, 0x1200001f, 0x61800000, 0x622081ff,
		0x20000000, 0x23004f02, 0x24000003, 0x25f6cc00,
		0x28ffcc00, 0x40800000, 0x41400000, 0x4200493f,
		0x43800000, 0x44400000, 0x4500493f, 0x46800000,
		0x47400000, 0x4800003f, 0x49000000, 0x50000000,
		0x51000000, 0x52147fff,
	};
	static const u8 width[] = { 3 };
	static const u8 format[] = { 0, 0x02, 0x42, 0x82, 0xc2 };
	static const u8 link[] = { 0, 0x8, 0xc, 0xe, 0xf };
	u32 raster[40];
	u32 count;
	unsigned int varying_count;

	count = chrome9_tcl_raster_selftest();
	if (count)
		return count;
	count = chrome9_fragment_raster_state(0, NULL, 0, NULL, 3, raster);
	if (count != ARRAY_SIZE(expected_zero_varying) ||
	    memcmp(raster, expected_zero_varying,
		   sizeof(expected_zero_varying)))
		return -EINVAL;
	count = chrome9_fragment_raster_state(ARRAY_SIZE(width), width, 0,
		NULL, 3, raster);
	if (count != 28 || raster[0] != 0x00000309 ||
	    raster[4] != 0x04006382 || raster[5] != 0x1200000f ||
	    raster[7] != 0x623081ff || raster[10] != 0x24000003 ||
	    raster[11] != 0x25f6e000 || raster[12] != 0x26000000 ||
	    raster[13] != 0x28ffe000 || raster[14] != 0x29000000)
		return -EINVAL;
	for (varying_count = 1; varying_count <= 8; varying_count++) {
		u32 layouts = 1U << (2 * varying_count);
		u32 layout;

		for (layout = 0; layout < layouts; layout++) {
			u8 widths[8] = { 0 };
			u32 expected_a25 = 0x25f60000;
			u32 expected_a26 = 0x26000000;
			u32 expected_a28 = 0x28ff0000;
			u32 expected_a29 = 0x29000000;
			u32 stride = 6;
			u32 offset = 6;
			unsigned int varying;

			for (varying = 0; varying < varying_count; varying++) {
				u32 shift = 12 - 4 * (varying & 3);

				widths[varying] = ((layout >> (2 * varying)) & 3) + 1;
				stride += widths[varying];
				if (varying < 4) {
					expected_a25 |= link[widths[varying]] << shift;
					expected_a28 |= link[widths[varying]] << shift;
				} else {
					expected_a26 |= link[widths[varying]] << shift;
					expected_a29 |= link[widths[varying]] << shift;
				}
			}
			count = chrome9_fragment_raster_state(varying_count, widths,
				0, NULL, 3, raster);
			if (count != 27 + varying_count ||
			    raster[0] != ((2 + varying_count) << 8 | stride) ||
			    raster[6 + varying_count] !=
					(0x620081ff | (2 + varying_count) << 20) ||
			    raster[10 + varying_count] != expected_a25 ||
			    raster[11 + varying_count] != expected_a26 ||
			    raster[12 + varying_count] != expected_a28 ||
			    raster[13 + varying_count] != expected_a29)
				return -EINVAL;
			offset = 6;
			for (varying = 0; varying < varying_count; varying++) {
				u32 expected = (4 + varying) << 24 | offset << 12 |
					(3 + varying) << 8 | format[widths[varying]];

				if (raster[4 + varying] != expected)
					return -EINVAL;
				offset += widths[varying];
			}
		}
	}
	{
		static const u8 float4_width[] = { 4 };
		static const u8 mixed_width[] = { 4, 2, 3 };
		static const u8 fifth_float4[] = { 2, 2, 2, 2, 4 };

		count = chrome9_fragment_raster_state(1, float4_width, 1, NULL, 3,
			raster);
		if (count != 28 || raster[11] != 0x25f6f000 ||
		    raster[13] != 0x28ffe000 ||
		    chrome9_fragment_raster_state(1, float4_width, 0, NULL, 3,
						 raster) != 28 ||
		    raster[11] != 0x25f6f000 ||
		    raster[13] != 0x28fff000)
			return -EINVAL;
		if (chrome9_fragment_raster_state(1, float4_width, 1,
						 float4_width, 3, raster) != 28 ||
		    raster[11] != 0x25f6f000 || raster[13] != 0x28fff000)
			return -EINVAL;
		count = chrome9_fragment_raster_state(3, mixed_width, 1, NULL, 3,
			raster);
		if (count != 30 || raster[13] != 0x25f6fce0 ||
		    raster[15] != 0x28ffece0)
			return -EINVAL;
		count = chrome9_fragment_raster_state(5, fifth_float4, 1, NULL, 3,
			raster);
		if (count != 32 || raster[15] != 0x25f6cccc ||
		    raster[16] != 0x2600f000 ||
		    raster[17] != 0x28ffcccc ||
		    raster[18] != 0x2900e000)
			return -EINVAL;
	}
	return 0;
}

static int chrome9_fragment_fe_config(u32 active_texture_count,
	u32 texture_count, u32 varying_count,
	const u8 varying_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS],
	const u8 *alu_width, u32 config[10])
{
	/* These are self-addressed register words.  Write A02/A03 on every
	 * draw so a smaller next program cannot inherit stale high FE fields.
	 * Keep the qualified one-/two-input A01 tail as the empty baseline.
	 */
	static const u32 default_config[] = {
		0x00000001, 0x01002521, 0x08010100,
		0x04000000, 0x05000000, 0x0d810000,
		0x09000000, 0x0a000000,
		0x02000000, 0x03000000,
	};
	unsigned int i;

	if (active_texture_count > DRM_CHROME9_FRAGMENT_MAX_VARYINGS)
		return -E2BIG;
	memcpy(config, default_config, sizeof(default_config));
	/* A00 counts compiler-active texture resources. A08 independently counts
	 * VS coordinate outputs, not sampler high-water or internal TAU slots.
	 */
	config[0] |= active_texture_count << 4;
	config[2] |= varying_count;
	for (i = 0; i < varying_count; i++) {
		u32 producer_width = varying_width[i];
		u32 consumer_width = chrome9_fragment_consumer_width(
			producer_width, texture_count, alu_width, i);
		u32 consumer_word = i < 3 ? 1 : i < 6 ? 8 : 9;
		u32 consumer_shift = (i < 3 ? i : i < 6 ? i - 3 : i - 6) * 8;
		u32 word, shift;

		/* Direct ALU and TAU reads share the compact T-input order.  The
		 * programmable FE caps producer width by physical consumer ordinal,
		 * independently of the sampler selected by each TAU instruction.
		 */
		config[consumer_word] &= ~(0xffU << consumer_shift);
		config[consumer_word] |=
			(0x20 | i << 2 | (consumer_width - 1)) << consumer_shift;
		if (i < 2) {
			word = 2;
			shift = 8 * (i + 1);
		} else if (i < 5) {
			word = 6;
			shift = 8 * (i - 2);
		} else {
			word = 7;
			shift = 8 * (i - 5);
		}
		config[word] &= ~(0xffU << shift);
		config[word] |= (producer_width - 1) << shift;
	}
	return 0;
}

/*
 * Hardware user clip planes (FE A05): plane rank k is lane k % 3 of VS
 * coordinate base + k / 3; A08 counts the extra coordinates and their
 * dimensions follow the producer-width layout of the varyings.
 */
static void chrome9_fragment_fe_ucp(u32 base, u32 planes, u32 config[10])
{
	u32 groups = DIV_ROUND_UP(planes, 3);
	u32 a05 = 0x05000000;
	unsigned int g;

	for (g = 0; g < groups; g++) {
		u32 coordinate = base + g;
		u32 dim = min(planes - 3 * g, 3U) - 1;
		u32 word, shift;

		a05 |= (BIT(7) | coordinate << 2 | dim) << (8 * g);
		if (coordinate < 2) {
			word = 2;
			shift = 8 * (coordinate + 1);
		} else if (coordinate < 5) {
			word = 6;
			shift = 8 * (coordinate - 2);
		} else {
			word = 7;
			shift = 8 * (coordinate - 5);
		}
		config[word] &= ~(0xffU << shift);
		config[word] |= dim << shift;
	}
	config[2] = (config[2] & ~GENMASK(4, 0)) | (base + groups);
	config[4] = a05;
}

static int chrome9_fragment_fe_config_selftest(void)
{
	static const struct {
		u8 count;
		u8 width[8];
		u32 a01, a02, a03, a08, a09, a0a;
	} cases[] = {
		{ 1, { 1 }, 0x01002520, 0x02000000, 0x03000000,
		  0x08010001, 0x09000000,
		  0x0a000000 },
		{ 1, { 2 }, 0x01002521, 0x02000000, 0x03000000,
		  0x08010101, 0x09000000,
		  0x0a000000 },
		{ 1, { 3 }, 0x01002522, 0x02000000, 0x03000000,
		  0x08010201, 0x09000000,
		  0x0a000000 },
		{ 1, { 4 }, 0x01002523, 0x02000000, 0x03000000,
		  0x08010301, 0x09000000,
		  0x0a000000 },
		{ 3, { 4, 2, 3 }, 0x012a2523, 0x02000000, 0x03000000,
		  0x08010303,
		  0x09000002, 0x0a000000 },
		{ 5, { 1, 4, 2, 3, 1 }, 0x01292720, 0x0200302e,
		  0x03000000, 0x08030005, 0x09000201,
		  0x0a000000 },
		{ 8, { 4, 1, 2, 3, 4, 1, 2, 3 }, 0x01292423, 0x0234332e,
		  0x03003e39, 0x08000308,
		  0x09030201, 0x0a020100 },
	};
	static const u8 empty_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS];
	static const u8 packed3_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS] = { 3 };
	static const u8 packed4_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS] = { 4 };
	static const u8 mixed_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS] = { 4, 2 };
	static const u8 terrain_width[DRM_CHROME9_FRAGMENT_MAX_VARYINGS] = {
		4, 4, 4, 2,
	};
	static const u8 five_float2[DRM_CHROME9_FRAGMENT_MAX_VARYINGS] = {
		2, 2, 2, 2, 2,
	};
	u32 config[10];
	unsigned int i;

	if (chrome9_fragment_fe_config(0, 0, 0, empty_width, NULL, config))
		return -EINVAL;
	if (config[0] != 0x00000001 || config[1] != 0x01002521 ||
	    config[2] != 0x08010100 ||
	    config[6] != 0x09000000 || config[7] != 0x0a000000 ||
	    config[8] != 0x02000000 || config[9] != 0x03000000)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		if (chrome9_fragment_fe_config(cases[i].count, 0,
					       cases[i].count, cases[i].width,
					       NULL, config))
			return -EINVAL;
		if (config[0] != (cases[i].count << 4 | 1) ||
		    config[1] != cases[i].a01 ||
		    config[2] != cases[i].a08 || config[6] != cases[i].a09 ||
		    config[7] != cases[i].a0a || config[8] != cases[i].a02 ||
		    config[9] != cases[i].a03)
			return -EINVAL;
	}
	/* A sparse sampler or an ALU-produced TAU coordinate does not add a
	 * vertex-shader output to A08.
	 */
	if (chrome9_fragment_fe_config(1, 1, 0, empty_width, NULL, config) ||
	    config[0] != 0x00000011 || config[1] != 0x01002521 ||
	    config[2] != 0x08010100 || config[6] != 0x09000000 ||
	    config[7] != 0x0a000000 || config[8] != 0x02000000 ||
	    config[9] != 0x03000000)
		return -EINVAL;
	/* A textured float3/float4 T0 is capped at three components.
	 */
	if (chrome9_fragment_fe_config(1, 1, 1, packed3_width, NULL, config) ||
	    config[1] != 0x01002522 || config[2] != 0x08010201 ||
	    chrome9_fragment_fe_config(1, 1, 1, packed4_width, NULL, config) ||
	    config[1] != 0x01002522 || config[2] != 0x08010301)
		return -EINVAL;
	/* A direct ALU W read keeps textured float4 T0 at four components. */
	if (chrome9_fragment_fe_config(1, 1, 1, packed4_width, packed4_width,
				       config) ||
	    config[1] != 0x01002523 || config[2] != 0x08010301)
		return -EINVAL;
	/* ALU T0 precedes sampled T1, with the cap applied to each FE field.
	 */
	if (chrome9_fragment_fe_config(2, 1, 2, mixed_width, NULL, config) ||
	    config[1] != 0x01002522 ||
	    config[2] != 0x08010302)
		return -EINVAL;
	/* One MAD can read three T slots while using one active resource. */
	if (chrome9_fragment_fe_config(1, 0, cases[4].count,
				       cases[4].width, NULL, config) ||
	    config[0] != 0x00000011 || config[2] != cases[4].a08)
		return -EINVAL;
	/* Five direct float2 inputs use A01 then A02; a later one-input draw
	 * resets both A02 and A03 instead of inheriting the prior map.
	 */
	if (chrome9_fragment_fe_config(5, 5, 5, five_float2, NULL, config) ||
	    config[1] != 0x01292521 || config[8] != 0x0200312d ||
	    config[9] != 0x03000000 ||
	    chrome9_fragment_fe_config(1, 1, 1, five_float2, NULL, config) ||
	    config[1] != 0x01002521 || config[8] != 0x02000000 ||
	    config[9] != 0x03000000)
		return -EINVAL;
	/* The terrain-sized mixed-width map is capped per physical input, not
	 * by the dimensions of the five logical sampler descriptors.
	 */
	/* UCP: 2 planes after one ST varying, then 4 planes (vendor table). */
	if (chrome9_fragment_fe_config(1, 1, 1, packed3_width, NULL, config))
		return -EINVAL;
	chrome9_fragment_fe_ucp(1, 2, config);
	if (config[4] != 0x05000085 || (config[2] & 0x1f) != 2 ||
	    (config[2] >> 16 & 0xff) != 1)
		return -EINVAL;
	chrome9_fragment_fe_ucp(1, 4, config);
	if (config[4] != 0x05008886 || (config[2] & 0x1f) != 3 ||
	    (config[2] >> 16 & 0xff) != 2 || (config[6] & 0xff) != 0)
		return -EINVAL;
	if (chrome9_fragment_fe_config(5, 5, 4, terrain_width, NULL, config) ||
	    config[1] != 0x012a2622 || config[8] != 0x0200002d ||
	    config[9] != 0x03000000 || config[2] != 0x08030304 ||
	    config[6] != 0x09000103)
		return -EINVAL;
	return 0;
}

struct chrome9_fragment_rt_format {
	u8 a52_format;
	u8 cpp;
};

static const struct chrome9_fragment_rt_format chrome9_fragment_rt_formats[] = {
	[DRM_CHROME9_DESTINATION_FORMAT_BGRX8888] = { 0x98, 4 },
	[DRM_CHROME9_DESTINATION_FORMAT_BGRA8888] = { 0x99, 4 },
	[DRM_CHROME9_DESTINATION_FORMAT_RGB565] = { 0x89, 2 },
	[DRM_CHROME9_DESTINATION_FORMAT_L8A8] = { 0x15, 2 },
	[DRM_CHROME9_DESTINATION_FORMAT_ARGB1555] = { 0x8a, 2 },
	[DRM_CHROME9_DESTINATION_FORMAT_ARGB4444] = { 0x8b, 2 },
};

static const struct chrome9_fragment_rt_format *
chrome9_fragment_rt_format(u32 format)
{
	if (format >= ARRAY_SIZE(chrome9_fragment_rt_formats) ||
	    !chrome9_fragment_rt_formats[format].cpp)
		return NULL;

	return &chrome9_fragment_rt_formats[format];
}

static void chrome9_emit_fragment_texture(u32 **cursor,
	const struct drm_chrome9_fragment_texture_state *texture,
	u32 texture_base, u32 unit)
{
	u32 width = texture->width >> texture->first_level;
	u32 height = texture->height >> texture->first_level;
	u32 log_size = ilog2(max(height, 1U)) << 4 |
		ilog2(max(width, 1U));
	u32 level_count = texture->last_level - texture->first_level;
	u32 mip[8];
	u32 image[] = {
		(texture_base + texture->level_offset[texture->first_level]) >> 8,
		0x18001101, 0x20000000 | height << 12 | width,
		0x21000001, 0x22000000 |
		(is_power_of_2(width) && is_power_of_2(height) ? BIT(15) : 0) |
		log_size,
		0x30000000 | chrome9_fragment_texture_format(texture->format) << 16 |
		(texture->format == DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24 ?
		 0x2000 : 0),
	};
	u32 sampler[4];

	chrome9_fragment_sampler_state(texture, sampler);
	chrome9_emit_header2(cursor, 0x440, 0x0000043c,
		unit << 24 | 0x00020000, image, ARRAY_SIZE(image));
	if (level_count) {
		chrome9_fragment_mip_state(texture, mip);
		chrome9_emit_header3(cursor, 0x10030000 | unit << 3, mip,
			ARRAY_SIZE(mip));
	}
	chrome9_emit_header2(cursor, 0x440, 0x0000043c,
		unit << 24 | 0x20020000, sampler, ARRAY_SIZE(sampler));
}

static int chrome9_build_fragment_program(struct chrome9_engine *engine,
	const struct chrome9_draw_request *request, struct chrome9_job *job,
	u32 target, u32 depth, const u32 texture_base[DRM_CHROME9_FRAGMENT_MAX_TEXTURES],
	const u32 *element_address, u32 *pause)
{
	static const u32 destination_tail[] = {
		0x38000000, 0x3b000000, 0x39000000, 0x3c000000,
		0x34000150, 0x35000114, 0x36000500, 0x37000200,
	};
	static const u32 pixel_z_clip[] = {
		0x1e000000, 0x1f000000, 0x20007fff,
	};
	static const u32 neutral_depth_bias[] = {
		0x16000000, 0x17000000, 0x18000000,
		0x19800000, 0x1a800000, 0x1b803fbf,
	};
	static const u32 neutral_stencil[] = {
		0x2300ffff, 0x24070000, 0x2500ffff, 0x26070000,
	};
	static const u32 vs_prefix[] = {
		0x00380150, 0x00000000, 0x00000000, 0x0008010f,
		0x0008014c, 0x00100150, 0x00384154, 0x0004021f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
	};
	static const u32 vs_float_constants[] = {
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 vs_integer_constants[] = { 0, 0, 0, 0 };
	static const u32 sync[] = { 0x6c, 0x00800000 };
	/* VX900 HPSLocationReg: direct PEY, TS-ZTAG deadlock ECO enabled. */
	static const u32 misc_location[] = { 0x8a020000 };
	const struct chrome9_fragment_program *program = request->fragment_program;
	const struct chrome9_tcl_draw *tcl = request->tcl;
	const struct chrome9_fragment_rt_format *rt_format =
		chrome9_fragment_rt_format(request->destination_format);
	struct chrome9_blend_words blend_words;
	u32 destination_state[16];
	u32 config[10];
	u32 common[32] = {
		0x860000ff, 0x870000ff, 0x53010c0f, 0x00000011,
		0x01ce1540, 0x88001ed0, 0x89000800,
	};
	u32 clip[] = {
		0x80000000 | request->height,
		0x81000000 | request->width,
		chrome9_scissor_y(request), chrome9_scissor_x(request),
	};
	u32 raster[CHROME9_TCL_RASTER_WORDS];
	u32 vs_program[4 * (4 + DRM_CHROME9_FRAGMENT_MAX_VARYINGS)] = { 0 };
	u32 vs_control[] = { 0 };
	u32 *base = engine->run_staging_cpu;
	u32 *p = base;
	u32 *vertex_header;
	u32 destination_count;
	u32 common_count = 7;
	u32 vertex_count = tcl ? tcl->vertex_count : request->triangle_count * 3;
	u32 depth_func = (request->flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
		DRM_CHROME9_DEPTH_FUNC_SHIFT;
	u32 raster_count;
	bool blend = request->flags & DRM_CHROME9_FRAGMENT_BLEND;
	bool depth_active = request->flags & DRM_CHROME9_FRAGMENT_DEPTH;
	unsigned int i;
	int ret;

	if (!base || !program || (!request->fragment_vertices && !tcl) ||
	    !rt_format)
		return -EINVAL;
	ret = chrome9_fragment_fe_config(program->active_texture_count,
		program->texture_count, program->varying_count,
		program->varying_width, program->varying_alu_width, config);
	if (ret)
		return ret;
	if (tcl && tcl->clip_mask) {
		chrome9_fragment_fe_ucp(program->varying_count,
					hweight32(tcl->clip_mask), config);
		if (tcl->re & CHROME9_TCL_RE_UCP_PERSPECTIVE)
			config[4] |= BIT(6) |
				(hweight32(tcl->clip_mask) > 3 ? BIT(14) : 0);
	}
	vs_control[0] = 0x60800000 | (3 + program->varying_count);
	destination_state[0] = 0x50000000 | (target >> 8);
	destination_state[1] = 0x51100000;
	if (READ_ONCE(chrome9_vram_target_test)) {
		destination_state[0] = 0x50000000 |
			(READ_ONCE(chrome9_vram_target_test) >> 8);
		destination_state[1] = 0x51000000;
	}
	destination_state[2] = 0x52000000 |
		(rt_format->a52_format << 16) | (request->pitch >> 5);
	if (blend) {
		chrome9_lower_blend(&request->blend, &blend_words);
		for (i = 0; i < 8; i++)
			destination_state[3 + i] = (0x38 + i) << 24;
		destination_state[11] = blend_words.a34;
		destination_state[12] = blend_words.a35;
		destination_state[13] = blend_words.a36;
		destination_state[14] = blend_words.a37;
		destination_state[15] =
			(request->blend.flags & DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
			blend_words.a33 : 0x33007000;
		destination_count = ARRAY_SIZE(destination_state);
		common[2] = blend_words.a53;
		common[3] = blend_words.a00;
	} else {
		memcpy(destination_state + 3, destination_tail,
		       sizeof(destination_tail));
		destination_count = 3 + ARRAY_SIZE(destination_tail);
	}
	if (depth_active) {
		if (!depth_func)
			depth_func = DRM_CHROME9_COMPARE_LESS;
		common[4] = chrome9_depth_control_word(request, true);
		common[7] = 0x10000000 | (depth >> 8);
		common[8] = 0x11000001 | ((request->depth_pitch >> 5) << 13);
		common[9] = request->fragment_depth_format ==
			DRM_CHROME9_FRAGMENT_DEPTH_X8Z24 ? 0x123e0000 : 0x12380000;
		common[10] = 0x13000000 | depth_func << 16;
		common_count = 11;
		memcpy(common + common_count, neutral_depth_bias,
		       sizeof(neutral_depth_bias));
		common_count += ARRAY_SIZE(neutral_depth_bias);
	}
	/* The pre-depth pixel-Z clip interval is required even without depth. */
	memcpy(common + common_count, pixel_z_clip, sizeof(pixel_z_clip));
	common_count += ARRAY_SIZE(pixel_z_clip);
	if (depth_active) {
		if (request->flags & DRM_CHROME9_FRAGMENT_STENCIL) {
			common[4] |= BIT(4);
			chrome9_lower_stencil(&request->stencil,
					      common + common_count);
		} else {
			memcpy(common + common_count, neutral_stencil,
			       sizeof(neutral_stencil));
		}
		common_count += ARRAY_SIZE(neutral_stencil);
		common[common_count++] = blend &&
			(request->blend.flags &
			 DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
			blend_words.a33 : 0x33007000;
	}

	/* Vendor hardware TCL: type-01 A01 clip engine (bit 11) plus bit 20. */
	if (tcl)
		common[4] |= (tcl->re & CHROME9_TCL_RE_CLIP_OFF ? 0 : 0x00100800) |
			     (tcl->cull & (DRM_CHROME9_VERTEX_CULL_CW |
					   DRM_CHROME9_VERTEX_CULL_CCW) ? BIT(7) : 0);
	/* D0 position, normalized D1/D2 colors, then packed dense T0..T7. */
	if (tcl)
		raster_count = chrome9_tcl_raster_state(program, tcl,
			element_address, raster);
	else
		raster_count = chrome9_fragment_raster_state(
			program->varying_count, program->varying_width,
			program->texture_count, program->varying_alu_width,
			vertex_count, raster);
	memcpy(vs_program, vs_prefix, sizeof(vs_prefix));
	for (i = 0; i < program->varying_count; i++) {
		u32 *record = vs_program + 16 + i * 4;

		record[0] = 0x0008c14c + i * 0x00004000;
		record[3] = (0x00080280 + i * 0x10) |
			GENMASK(program->varying_width[i] - 1, 0);
	}

	/* Every dword up to the read-ahead end is written explicitly. */
	chrome9_emit_header0(&p, sync, 1);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     destination_state, destination_count);
	for (i = 0; i < request->fragment_texture_count; i++) {
		chrome9_emit_fragment_texture(&p, &request->fragment_textures[i],
			texture_base[i], request->fragment_texture_units[i]);
	}
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     common, common_count);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0xfe020000,
			     config, ARRAY_SIZE(config));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000, raster,
			     raster_count);
	if (program->constant_count) {
		u32 first = min(program->constant_count, 16U);

		chrome9_emit_header3(&p, 0x22030000, &program->constants[0][0],
				     first * 4);
		if (program->constant_count > 16)
			chrome9_emit_header3(&p, 0x22030080,
					     &program->constants[16][0],
					     (program->constant_count - 16) * 4);
	}
	if (tcl && tcl->constant_count)
		chrome9_emit_header3(&p, 0x32030000 + 4 * tcl->constant_first,
				     tcl->constants, 4 * tcl->constant_count);
	if (tcl)
		chrome9_emit_header3(&p, 0x32030400, tcl->rows,
				     ARRAY_SIZE(tcl->rows));
	else
		chrome9_emit_header3(&p, 0x32030400, vs_float_constants,
				     ARRAY_SIZE(vs_float_constants));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x33030000,
			     vs_integer_constants, ARRAY_SIZE(vs_integer_constants));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     clip, ARRAY_SIZE(clip));
	if (tcl) {
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x31030000,
				     &tcl->program.records[0][0],
				     4 * tcl->program.record_count);
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
				     &tcl->program.a60, 1);
	} else {
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x31030000,
				     vs_program, 4 * (4 + program->varying_count));
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
				     vs_control, ARRAY_SIZE(vs_control));
	}
	if (program->controls[0] & BIT(19))
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
				     misc_location, ARRAY_SIZE(misc_location));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     program->controls, ARRAY_SIZE(program->controls));
	/* The compiler's counted H3 transaction stores ALU before TAU. */
	chrome9_emit_header3(&p, 0x20030000, &program->alu[0][0],
			     program->alu_count * 4);
	if (program->tau_count)
		chrome9_emit_header3(&p, 0x21030000, program->tau,
				     program->tau_count);
	if (element_address) {
		u32 slots[DRM_CHROME9_VERTEX_MAX_INPUTS];
		u32 draw[4];

		/* Keep room for the tail, PAUSE/STOP and the read-ahead. */
		if (p + 0x40 + tcl->index_dwords + CHROME9_CR_READ_AHEAD / 4 +
		    0x100 > base + CHROME9_RUN_STAGING_SIZE / 4)
			return -E2BIG;
		for (i = 0; i < tcl->element_count; i++)
			slots[i] = chrome9_tcl_stream_slot(element_address[i],
				tcl->buffers[tcl->element_buffer[i]].stride);
		chrome9_emit_header3(&p, 0x30030000, slots, tcl->element_count);
		/* Inclusive index bounds; indices were rebased to zero. */
		draw[0] = 0x2c000000 | (tcl->index_range & 0x00ffffff);
		draw[1] = 0x2d000000;
		draw[2] = 0x2e000000 | (tcl->index_range >> 24) << 8;
		draw[3] = 0x24000000 | tcl->index_count;
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00040000,
				     draw, ARRAY_SIZE(draw));
		chrome9_emit_header3(&p, 0x00000000, tcl->indices,
				     tcl->index_dwords);
		ret = chrome9_emit_3d_tail(engine, job, &p);
		if (ret)
			return ret;
		return chrome9_finish_ring_sized(base, p,
			CHROME9_RUN_STAGING_SIZE, pause, &job->segment_stop);
	}
	while (!IS_ALIGNED((unsigned long)p, 16))
		*p++ = 0xcc000000;
	vertex_header = p;
	p += 4;
	if (tcl) {
		u32 dwords = vertex_count * tcl->stride_dw;

		/* Keep room for the tail, PAUSE/STOP and the read-ahead. */
		if (p + dwords + CHROME9_CR_READ_AHEAD / 4 + 0x100 >
		    base + CHROME9_RUN_STAGING_SIZE / 4)
			return -E2BIG;
		memcpy(p, tcl->vertices, dwords * sizeof(*p));
		p += dwords;
	}
	for (i = 0; !tcl && i < vertex_count; i++) {
		const struct drm_chrome9_fragment_vertex *vertex =
			&request->fragment_vertices[i];
		unsigned int varying;

		*p++ = chrome9_q16_to_float(vertex->x_16_16);
		*p++ = chrome9_q16_to_float(vertex->y_16_16);
		*p++ = chrome9_q16_to_float(vertex->z_16_16);
		*p++ = vertex->rhw_float;
		*p++ = vertex->color;
		*p++ = vertex->specular;
		for (varying = 0; varying < program->varying_count; varying++) {
			u32 logical = program->varying_logical[varying];
			unsigned int component;

			for (component = 0;
			     component < program->varying_width[varying]; component++)
				*p++ = vertex->varying[logical][component];
		}
	}
	chrome9_count_vertex_payload(vertex_header, p - vertex_header - 4);
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	return chrome9_finish_ring_sized(base, p, CHROME9_RUN_STAGING_SIZE,
					 pause, &job->segment_stop);
}

static int chrome9_build_depth_triangle(struct chrome9_engine *engine,
		const struct chrome9_draw_request *request,
		struct chrome9_job *job, u32 target, u32 depth, u32 *pause)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	struct chrome9_blend_words blend_words;
	bool run = request->kind ==
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN ||
		request->kind == CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH;
	void *staging = run ? engine->run_staging_cpu : engine->staging_cpu;
	size_t staging_size = run ? CHROME9_RUN_STAGING_SIZE : PAGE_SIZE;
	u32 max_triangles = run ? DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES :
		DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES;
	u32 *base = staging;
	u32 *body;
	u32 *p;
	u32 delta = 0;
	u32 triangle_count = request->triangle_count ?: 1;
	u32 vertex_count = triangle_count * 3;
	u32 depth_func = (request->flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
			 DRM_CHROME9_DEPTH_FUNC_SHIFT;
	bool blend = chrome9_draw_has_blend(request);
	bool compiled = request->kind ==
		CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH;
	bool perspective = compiled || request->kind ==
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_BATCH || run;
	unsigned int i;
	int ret;

	if (!base || !triangle_count || triangle_count > max_triangles)
		return -E2BIG;
	if (!depth_func)
		depth_func = DRM_CHROME9_COMPARE_LESS;

	memset(base, 0xcc, staging_size);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188 || body[6] != 0x52980004 ||
	    body[19] != 0x01c01540 || body[24] != 0xfe020440 ||
	    body[49] != 0x23000f02 || body[50] != 0x24000004 ||
	    body[160] != 0xfe020440 || body[163] != 0xcc000000)
		return -EINVAL;

	if (blend) {
		/* Close A38--A3F before adding depth state to the successor. */
		memmove(body + 19, body + 15, (188 - 15) * sizeof(*body));
		chrome9_lower_blend(&request->blend, &blend_words);
		for (i = 0; i < 8; i++)
			body[7 + i] = (0x38 + i) << 24;
		body[15] = blend_words.a34;
		body[16] = blend_words.a35;
		body[17] = blend_words.a36;
		body[18] = blend_words.a37;
		body[21] = blend_words.a53;
		body[22] = blend_words.a00;
		delta = 4;
	}

	/* Add D16 words and the wide-open near/far clip state. */
	memmove(body + 28 + delta, body + 24 + delta,
		(188 - 24) * sizeof(*body));
	body[6] = 0x52000000 |
		(request->destination_format ==
		 DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ? 0x00990000 :
		 0x00980000) | (request->pitch >> 5);
	body[19 + delta] = chrome9_depth_control_word(request, perspective);
	body[22 + delta] = 0x10000000 | (depth >> 8);
	body[23 + delta] = 0x11000001 |
		((request->depth_pitch >> 5) << 13);
	body[24 + delta] = 0x12380000;
	body[25 + delta] = 0x13000000 | depth_func << 16;
	body[26 + delta] = 0x20007fff;
	body[27 + delta] = blend &&
		(request->blend.flags & DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
		blend_words.a33 : 0xcc000000;
	if (compiled) {
		/* The ioctl carries semantics; these words were rebuilt in-kernel. */
		memcpy(body + 76 + delta, request->ps_constant_c0,
		       sizeof(request->ps_constant_c0));
		memcpy(body + 160 + delta, request->ps_program,
		       sizeof(request->ps_program));
	}
	body[52 + delta] = 0x20000000;
	body[53 + delta] = 0x23004f02;
	body[54 + delta] = 0x24000000 | vertex_count;
	body[104 + delta] = 0x80000000 | request->height;
	body[105 + delta] = 0x81000000 | request->width;
	body[106 + delta] = chrome9_scissor_y(request);
	body[107 + delta] = chrome9_scissor_x(request);
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_vertex_d16 *src = NULL;
		const struct drm_chrome9_vertex_d16_rhw *src_rhw = NULL;
		u32 *vertex = body + 168 + delta + i * 6;

		if (perspective)
			src_rhw = &request->batch_vertices_rhw[i];
		else
			src = request->batch_vertices ? &request->batch_vertices[i] :
						      &request->depth_vertices[i];
		vertex[0] = chrome9_q16_to_float(perspective ?
						 src_rhw->x_16_16 : src->x_16_16);
		vertex[1] = chrome9_q16_to_float(perspective ?
						 src_rhw->y_16_16 : src->y_16_16);
		vertex[2] = chrome9_q16_to_float(perspective ?
						 src_rhw->z_16_16 : src->z_16_16);
		vertex[3] = perspective ? src_rhw->rhw_float : 0x3f800000;
		vertex[4] = perspective ? src_rhw->color : src->color;
		vertex[5] = 0xffffffff;
	}
	body[168 + delta + vertex_count * 6] = 0xcc000000;
	body[169 + delta + vertex_count * 6] = 0xcc000000;
	p = body + 170 + delta + vertex_count * 6;
	if ((compiled &&
	     (memcmp(body + 76 + delta, request->ps_constant_c0,
		     sizeof(request->ps_constant_c0)) ||
	      memcmp(body + 160 + delta, request->ps_program,
		     sizeof(request->ps_program)))) ||
	    (!blend &&
	     (p - body != 170 + vertex_count * 6 ||
	      body[19] != chrome9_depth_control_word(request, perspective) ||
	      body[22] != (0x10000000 | (depth >> 8)) ||
	      body[27] != 0xcc000000 || body[52] != 0x20000000 ||
	      body[53] != 0x23004f02 ||
	      body[54] != (0x24000000 | vertex_count) ||
	      body[104] != (0x80000000 | request->height) ||
	      body[164] != 0xfe020440)) ||
	    (blend &&
	     (p - body != 174 + vertex_count * 6 ||
	      body[7] != 0x38000000 ||
	      body[14] != 0x3f000000 || body[15] != blend_words.a34 ||
	      body[18] != blend_words.a37 || body[21] != blend_words.a53 ||
	      body[22] != blend_words.a00 ||
	      body[23] != chrome9_depth_control_word(request, perspective) ||
	      body[26] != (0x10000000 | (depth >> 8)) ||
	      body[31] !=
		((request->blend.flags &
		  DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ?
		 blend_words.a33 : 0xcc000000) ||
	      body[32] != 0xfe020440 || body[57] != 0x23004f02 ||
	      body[56] != 0x20000000 ||
	      body[58] != (0x24000000 | vertex_count) ||
	      body[108] != (0x80000000 | request->height) ||
	      body[168] != 0xfe020440)))
		return -EINVAL;
	chrome9_count_vertex_payload(body + 164 + delta, vertex_count * 6);
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	ret = chrome9_finish_ring_sized(base, p, staging_size, pause,
				       &job->segment_stop);
	if (ret)
		return ret;
	if (run && triangle_count == DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES &&
	    (*pause != 0x9c70 || job->segment_stop != 0x9cf0 ||
	     body[54 + delta] != 0x24000660))
		return -EINVAL;
	return 0;
}

static int chrome9_emit_depth_body(u32 **cursor, u32 target, u32 depth,
				   u32 solid_color)
{
	u32 *body = PTR_ALIGN(*cursor, 16);
	u32 *p = body;

	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188 || body[15] != 0x860000ff ||
	    body[19] != 0x01c01540 || body[24] != 0xfe020440 ||
	    body[49] != 0x23000f02 || body[50] != 0x24000004 ||
	    body[160] != 0xfe020440 || body[163] != 0xcc000000)
		return -EINVAL;
	memmove(body + 28, body + 24, (188 - 24) * sizeof(*body));
	body[15] = 0x86000000 | solid_color;
	body[19] = 0x01c0154c;
	body[22] = 0x10000000 | (depth >> 8);
	body[23] = 0x11004001;
	body[24] = 0x12380000;
	body[25] = 0x13010000;
	body[26] = 0x20007fff;
	body[27] = 0xcc000000;
	body[53] = 0x23000f02;
	body[54] = 0x24000003;
	body[186] = 0xcc000000;
	body[187] = 0xcc000000;
	*cursor = body + 188;
	return 0;
}

static int chrome9_build_depth_oracle(struct chrome9_engine *engine,
				      struct chrome9_job *job, u32 target,
				      u32 depth, u32 *pause)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	u32 *base = engine->staging_cpu;
	u32 *p;
	int ret;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	ret = chrome9_emit_depth_body(&p, target, depth, 0xff0000);
	if (ret)
		return ret;
	ret = chrome9_emit_depth_body(&p, target, depth, 0x00ff00);
	if (ret)
		return ret;
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	return chrome9_finish_ring(base, p, pause, &job->segment_stop);
}

static int chrome9_emit_blend_body(u32 **cursor, u32 target, u32 solid_color)
{
	u32 *body = PTR_ALIGN(*cursor, 16);
	u32 *p = body;

	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188 || body[11] != 0x34000150 ||
	    body[12] != 0x35000114 || body[13] != 0x36000500 ||
	    body[14] != 0x37000200 || body[15] != 0x860000ff ||
	    body[16] != 0x870000ff || body[49] != 0x23000f02 ||
	    body[50] != 0x24000004 || body[160] != 0xfe020440)
		return -EINVAL;
	body[11] = 0x34000020;
	body[12] = 0x35000148;
	body[13] = 0x36000410;
	body[14] = 0x37000244;
	body[15] = 0x86000000 | solid_color;
	body[16] = 0x87000080;
	body[50] = 0x24000003;
	body[182] = 0xcc000000;
	body[183] = 0xcc000000;
	*cursor = body + 184;
	return 0;
}

static int chrome9_build_blend_oracle(struct chrome9_engine *engine,
				      struct chrome9_job *job, u32 target,
				      u32 *pause)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	u32 *base = engine->staging_cpu;
	u32 *p;
	int ret;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	ret = chrome9_emit_blend_body(&p, target, 0xff0000);
	if (ret)
		return ret;
	ret = chrome9_emit_blend_body(&p, target, 0x00ff00);
	if (ret)
		return ret;
	ret = chrome9_emit_3d_tail(engine, job, &p);
	if (ret)
		return ret;
	return chrome9_finish_ring(base, p, pause, &job->segment_stop);
}

static void chrome9_install_stopped_ring(struct chrome9_device *cdev,
					 u32 start, u32 end, u32 pause)
{
	writel(0x00100000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
	writel(0x60000000 | (start & 0x00fffff3),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x61000000 | (start >> 24),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x62000000 | (end & 0x00fffff0),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x63000000 | (end >> 24),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x64000000 | (pause & 0x00fffff0),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x65000000 | (pause >> 24),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x66fffff0, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x670000ff, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	/* All ring bounds and the pause address must precede the start trigger. */
	wmb();
	writel(0x680a0001, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	readl(cdev->mmio + CHROME9_REG_CR_TRANSPACE);
}

static void chrome9_fill_ring_dummy(struct chrome9_engine *engine);

static u32 chrome9_ring_append_after(u32 stop)
{
	return ALIGN(stop + CHROME9_CR_READ_AHEAD, 128);
}

static int chrome9_install_prime_ring(struct chrome9_engine *engine,
				      bool *published)
{
	struct chrome9_device *cdev = engine->cdev;
	u32 *ring = engine->ring_cpu;
	u32 status;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	*published = false;
	/* PID10 STOP must retire the old PID00 generation before A68. */
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, status,
				 !(status & BIT(31)), 10,
				 CHROME9_HW_TIMEOUT_US);
	if (ret)
		return ret;

	/* Match the vendor replacement path: A68 starts only a dummy H2 head. */
	chrome9_fill_ring_dummy(engine);
	ring[0] = 0xfe020420;
	ring[1] = 0x0000041c;
	ring[2] = 0x00300000;
	ring[3] = 0xcc000000;
	ring[0x80 / sizeof(*ring)] = 0xfe020420;
	ring[0x84 / sizeof(*ring)] = 0x0000041c;
	ring[0x88 / sizeof(*ring)] = 0x00300000;
	ring[0x8c / sizeof(*ring)] = 0xcc000000;
	chrome9_cache_flush(ring, CHROME9_RING_SIZE);
	dma_wmb();
	chrome9_install_stopped_ring(cdev, 1, CHROME9_RING_SIZE,
				     CHROME9_PRIME_PAUSE);
	*published = true;

	/* The clear-to-set PID00 edge distinguishes this new dummy generation. */
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, status,
				 status & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (ret)
		return ret;
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
				 status, !(status & CHROME9_ENGINE_WORK_MASK), 10,
				 CHROME9_HW_TIMEOUT_US);
	return ret;
}

static int chrome9_stop_active_ring(struct chrome9_engine *engine,
				    bool *pid10_published)
{
	struct chrome9_device *cdev = engine->cdev;
	u32 address;
	u32 status;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	if (pid10_published)
		*pid10_published = false;
	if (engine->active_stop <= engine->active_pause ||
	    engine->active_stop - engine->active_pause < 0x20)
		return -EINVAL;
	status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	if (!(status & BIT(31)) ||
	    (readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS) &
	     CHROME9_ENGINE_WORK_MASK))
		return -EBUSY;

	address = engine->active_stop;
	/* Vendor replacement sequence: changing PID00 PAUSE to a later PID10
	 * STOP resumes the owned reader without touching A60--A63 or A68. */
	dma_wmb();
	writel(0x00100000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
	writel(0x64000000 | (address & 0x00fffff0),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x65000200 | (address >> 24),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	readl(cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	if (pid10_published)
		*pid10_published = true;
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
				 status, !(status & CHROME9_ENGINE_WAIT_MASK), 10,
				 CHROME9_HW_TIMEOUT_US);
	if (ret)
		dev_err(&cdev->pdev->dev,
			"PID10 STOP timeout epoch=%u pause=%#x stop=%#x cr=%08x engine=%08x\n",
			engine->ring_epoch, engine->active_pause, engine->active_stop,
			readl(cdev->mmio + CHROME9_REG_CR_STATUS),
			readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS));
	return ret;
}

static void chrome9_fill_ring_dummy(struct chrome9_engine *engine)
{
	u32 *ring = engine->ring_cpu;
	unsigned int i;

	for (i = 0; i < CHROME9_RING_SIZE / sizeof(*ring); i += 2) {
		ring[i] = 0xccccccc0;
		ring[i + 1] = 0xddd00000;
	}
}

static int chrome9_append_segment(struct chrome9_engine *engine,
				  struct chrome9_job *job, const void *source,
				  size_t source_capacity, u32 segment_pause,
				  bool *must_quarantine)
{
	struct chrome9_device *cdev = engine->cdev;
	size_t copy_len;
	u32 append;
	u32 status;
	bool pid10_published;
	bool prime_published;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	*must_quarantine = false;
	job->segment_pause = segment_pause;
	if (!source || job->segment_stop <= segment_pause ||
	    job->segment_stop - segment_pause < 0x20 ||
	    check_add_overflow((size_t)job->segment_stop,
			       (size_t)CHROME9_CR_READ_AHEAD, &copy_len) ||
	    copy_len > source_capacity || copy_len > CHROME9_RING_SIZE)
		return -E2BIG;

	status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	if (!(status & BIT(31)) ||
	    (readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS) &
	     CHROME9_ENGINE_WORK_MASK))
		return -EBUSY;

	append = engine->append;
	/* Equality is an exact fit: the copied read-ahead closure ends at A62. */
	if (append > CHROME9_RING_SIZE - copy_len) {
		if (copy_len > CHROME9_RING_SIZE - CHROME9_WRAP_APPEND)
			return -E2BIG;
		ret = chrome9_stop_active_ring(engine, &pid10_published);
		if (ret) {
			*must_quarantine = pid10_published;
			return ret;
		}
		ret = chrome9_install_prime_ring(engine, &prime_published);
		if (prime_published) {
			engine->ring_epoch++;
			engine->wrap_count++;
		}
		if (ret) {
			job->published = true;
			if (prime_published) {
				engine->active_pause = CHROME9_PRIME_PAUSE;
				engine->active_stop = CHROME9_PRIME_STOP;
				engine->append =
					chrome9_ring_append_after(CHROME9_PRIME_STOP);
			}
			*must_quarantine = true;
			return ret;
		}
		/* Stay beyond every byte fetched while the prime reached PAUSE. */
		append = chrome9_ring_append_after(CHROME9_PRIME_STOP);
		memcpy((u8 *)engine->ring_cpu + append, source, copy_len);
		chrome9_cache_flush((u8 *)engine->ring_cpu + append, copy_len);
		dma_wmb();
		job->ring_pause = append + segment_pause;
		job->ring_stop = append + job->segment_stop;
		writel(0x00100000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
		writel(0x64000000 | (job->ring_pause & 0x00fffff0),
		       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
		writel(0x65000000 | (job->ring_pause >> 24),
		       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
		readl(cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	} else {
		memcpy((u8 *)engine->ring_cpu + append, source,
		       copy_len);
		chrome9_cache_flush((u8 *)engine->ring_cpu + append, copy_len);
		dma_wmb();
		job->ring_pause = append + segment_pause;
		job->ring_stop = append + job->segment_stop;
		writel(0x00100000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
		writel(0x64000000 | (job->ring_pause & 0x00fffff0),
		       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
		writel(0x65000000 | (job->ring_pause >> 24),
		       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
		readl(cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	}

	/*
	 * A65 (or the full install's A65/A68 sequence) publishes this tuple.
	 * Update ownership immediately even if the following retirement waits.
	 */
	job->published = true;
	engine->active_pause = job->ring_pause;
	engine->active_stop = job->ring_stop;
	/* Never overwrite command bytes already fetched beyond PID00 PAUSE. */
	engine->append = chrome9_ring_append_after(job->ring_stop);
	engine->append_count++;
	return 0;
}

static int chrome9_gart_set_kick(struct chrome9_engine *engine, u32 first,
				 const u32 *ptes, unsigned int count, bool kick)
{
	struct chrome9_device *cdev = engine->cdev;
	u8 saved_index;
	u8 saved_6c;
	u8 request;
	unsigned int i;
	int ret;

	lockdep_assert_held(&engine->vm_lock);
	if (first >= CHROME9_GART_ENTRIES ||
	    count > CHROME9_GART_ENTRIES - first)
		return -ERANGE;
	engine->gart_update_count++;
	ret = chrome9_vga_seq_get(cdev);
	if (ret)
		return ret;
	engine->gart_vga_acquire_count++;
	saved_index = readb(cdev->mmio + CHROME9_SEQ_INDEX);
	/* A still pending earlier flush is covered by the new request. */
	if (chrome9_gart_wait_idle(cdev))
		dev_warn_ratelimited(&cdev->pdev->dev,
			"GART flush still pending before update first=%u count=%u\n",
			first, count);
	saved_6c = chrome9_seq_read(cdev, 0x6c);
	chrome9_seq_write(cdev, 0x6c, saved_6c & ~BIT(7));
	for (i = 0; i < count; i++)
		writel(ptes ? ptes[i] : engine->scratch_pte,
		       engine->gart + (first + i) * sizeof(u32));
	engine->gart_pte_write_count += count;
	readl(engine->gart + (first + count - 1) * sizeof(u32));
	/* Make every PTE visible before requesting the GTI translation flush. */
	wmb();
	engine->gart_flush_count++;
	request = chrome9_gart_request_flush(cdev);
	/* The VIA mapper re-protects SR6C immediately after requesting SR6F. */
	chrome9_seq_write(cdev, 0x6c, saved_6c | BIT(7));
	ret = chrome9_gart_flush_wait(engine, request, kick);
	if (ret)
		dev_err_ratelimited(&cdev->pdev->dev,
			"GART translation flush failed first=%u count=%u ret=%d SR6F=%02x SR3F=%02x 0400=%08x\n",
			first, count, ret, chrome9_seq_read(cdev, 0x6f),
			chrome9_seq_read(cdev, 0x3f),
			readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS));
	writeb(saved_index, cdev->mmio + CHROME9_SEQ_INDEX);
	chrome9_vga_seq_put(cdev);
	return ret;
}

/* Callers not holding submit_lock cannot kick the ring. */
static int chrome9_gart_set(struct chrome9_engine *engine, u32 first,
			    const u32 *ptes, unsigned int count)
{
	return chrome9_gart_set_kick(engine, first, ptes, count, false);
}

static int chrome9_gart_bind_ring(struct chrome9_engine *engine)
{
	struct chrome9_device *cdev = engine->cdev;
	u32 ptes[CHROME9_RING_PAGES];
	u8 programmed_6c;
	u8 request;
	unsigned int i;
	int ret;

	ret = chrome9_vga_seq_get(cdev);
	if (ret)
		return ret;
	engine->saved_seq_index = readb(cdev->mmio + CHROME9_SEQ_INDEX);
	engine->saved_6a = chrome9_seq_read(cdev, 0x6a);
	engine->saved_6b = chrome9_seq_read(cdev, 0x6b);
	engine->saved_6c = chrome9_seq_read(cdev, 0x6c);
	engine->saved_7b = chrome9_seq_read(cdev, 0x7b);
	ret = chrome9_gart_wait_idle(cdev);
	if (ret)
		goto out_restore_index;
	chrome9_seq_write(cdev, 0x6c, engine->saved_6c & ~BIT(7));
	chrome9_seq_write(cdev, 0x6a, (engine->gart_offset >> 12) & 0xff);
	chrome9_seq_write(cdev, 0x6b, (engine->gart_offset >> 20) & 0xff);
	programmed_6c = (engine->saved_6c & ~BIT(7) & ~BIT(0)) |
			((engine->gart_offset >> 28) & 1);
	chrome9_seq_write(cdev, 0x6c, programmed_6c);
	chrome9_seq_write(cdev, 0x7b, (engine->saved_7b & 0xf0) | 4);
	/* Unused entries translate to the scratch page, never 80000000. */
	for (i = 0; i < CHROME9_GART_ENTRIES; i++)
		writel(engine->scratch_pte, engine->gart + i * sizeof(u32));
	for (i = 0; i < CHROME9_RING_PAGES; i++) {
		ptes[i] = lower_32_bits((engine->ring_dma + i * PAGE_SIZE) >>
					    PAGE_SHIFT) & 0x3fffffff;
		writel(ptes[i], engine->gart + i * sizeof(u32));
	}
	readl(engine->gart + (CHROME9_RING_PAGES - 1) * sizeof(u32));
	/* Publish scratch defaults and ring PTEs before the GTI flush. */
	wmb();
	request = chrome9_gart_request_flush(cdev);
	chrome9_seq_write(cdev, 0x6c, programmed_6c | BIT(7));
	ret = chrome9_gart_flush_wait(engine, request, false);
	if (!ret)
		engine->gart_programmed = true;
out_restore_index:
	writeb(engine->saved_seq_index, cdev->mmio + CHROME9_SEQ_INDEX);
	chrome9_vga_seq_put(cdev);
	return ret;
}

static int chrome9_gart_restore(struct chrome9_engine *engine)
{
	struct chrome9_device *cdev = engine->cdev;
	u8 index;
	u8 value;
	u8 request;
	int ret;

	ret = chrome9_vga_seq_get(cdev);
	if (ret)
		return ret;
	index = readb(cdev->mmio + CHROME9_SEQ_INDEX);
	ret = chrome9_gart_wait_idle(cdev);
	if (ret)
		goto out;
	value = chrome9_seq_read(cdev, 0x6c);
	chrome9_seq_write(cdev, 0x6c, value & ~BIT(7));
	memcpy_toio(engine->gart, engine->gart_saved, CHROME9_GART_SIZE);
	readl(engine->gart + CHROME9_GART_SIZE - sizeof(u32));
	/* Publish the restored table before invalidating cached translations. */
	wmb();
	request = chrome9_gart_request_flush(cdev);
	chrome9_seq_write(cdev, 0x6c, value | BIT(7));
	ret = chrome9_gart_flush_wait(engine, request, false);
	if (ret)
		goto out;
	chrome9_seq_write(cdev, 0x6a, engine->saved_6a);
	chrome9_seq_write(cdev, 0x6b, engine->saved_6b);
	chrome9_seq_write(cdev, 0x7b, engine->saved_7b);
	chrome9_seq_write(cdev, 0x6c, engine->saved_6c);
out:
	writeb(index, cdev->mmio + CHROME9_SEQ_INDEX);
	chrome9_vga_seq_put(cdev);
	return ret;
}

static void chrome9_take_quarantine_hold(struct chrome9_engine *engine)
{
	if (engine->quarantine_hold)
		return;
	if (!engine->module_hold) {
		if (WARN_ON_ONCE(!try_module_get(THIS_MODULE)))
			return;
		engine->module_hold = true;
	}
	get_device(&engine->cdev->pdev->dev);
	engine->device_hold = true;
	engine->quarantine_hold = true;
}

static void chrome9_quarantine_job_locked(struct chrome9_engine *engine,
					 struct chrome9_job *job)
{
	lockdep_assert_held(&engine->submit_lock);
	engine->wedged = true;
	chrome9_take_quarantine_hold(engine);
	list_add_tail(&job->quarantine_link, &engine->quarantine);
}

static int chrome9_collect_sgt_ptes(struct sg_table *sgt, u32 *ptes,
				    unsigned int page_count, bool snoop)
{
	struct scatterlist *sg;
	unsigned int index;
	unsigned int count = 0;

	for_each_sgtable_dma_sg(sgt, sg, index) {
		dma_addr_t address = sg_dma_address(sg);
		unsigned int length = sg_dma_len(sg);

		if (!length || !IS_ALIGNED(address, PAGE_SIZE) ||
		    !IS_ALIGNED(length, PAGE_SIZE) || upper_32_bits(address) ||
		    length - 1 > U32_MAX - address)
			return -ERANGE;
		while (length) {
			if (count >= page_count)
				return -E2BIG;
			ptes[count++] =
				(lower_32_bits(address >> PAGE_SHIFT) & 0x3fffffff) |
				(snoop ? CHROME9_GART_CACHED : 0);
			address += PAGE_SIZE;
			length -= PAGE_SIZE;
		}
	}
	return count == page_count ? 0 : -EINVAL;
}

static void chrome9_sf_mapping_free(struct chrome9_sf_mapping *mapping)
{
	drm_gem_object_put(mapping->object);
	kfree(mapping->ptes);
	kfree(mapping);
}

static struct chrome9_sf_mapping *
chrome9_draw_mapping_find_locked(struct chrome9_engine *engine,
				 struct drm_gem_object *object)
{
	struct chrome9_sf_mapping *mapping;

	lockdep_assert_held(&engine->submit_lock);
	list_for_each_entry(mapping, &engine->draw_mappings, link) {
		if (mapping->object == object && mapping->bound &&
		    !mapping->quarantined)
			return mapping;
	}
	return NULL;
}

static int
chrome9_draw_mapping_evict_locked(struct chrome9_engine *engine,
				  struct chrome9_sf_mapping *mapping)
{
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	if (WARN_ON_ONCE(!mapping->draw_cached || mapping->users ||
			 mapping->quarantined || !mapping->bound ||
			 !mapping->node_allocated))
		return -EBUSY;

	mutex_lock(&engine->vm_lock);
	ret = chrome9_gart_set_kick(engine, mapping->node.start >> PAGE_SHIFT,
			       NULL, mapping->page_count, true);
	if (ret) {
		engine->wedged = true;
		mapping->quarantined = true;
		chrome9_take_quarantine_hold(engine);
		mutex_unlock(&engine->vm_lock);
		dev_crit(&engine->cdev->pdev->dev,
			 "SF draw-cache eviction failed; mapping retained for reboot\n");
		return -EIO;
	}
	mapping->bound = false;
	drm_mm_remove_node(&mapping->node);
	mapping->node_allocated = false;
	list_del_init(&mapping->link);
	engine->draw_cache_entries--;
	engine->draw_cache_pages -= mapping->page_count;
	engine->draw_cache_evictions++;
	mutex_unlock(&engine->vm_lock);
	dev_dbg(&engine->cdev->pdev->dev,
		"draw SF cache evict base=%#llx pages=%u evictions=%llu\n",
		(unsigned long long)mapping->node.start, mapping->page_count,
		engine->draw_cache_evictions);
	chrome9_sf_mapping_free(mapping);
	return 0;
}

static int chrome9_draw_mapping_evict_lru_locked(struct chrome9_engine *engine)
{
	struct chrome9_sf_mapping *mapping;

	lockdep_assert_held(&engine->submit_lock);
	list_for_each_entry(mapping, &engine->draw_mappings, link) {
		if (!mapping->users && !mapping->quarantined)
			return chrome9_draw_mapping_evict_locked(engine, mapping);
	}
	return -ENOSPC;
}

static int
chrome9_draw_mapping_acquire_locked(struct chrome9_engine *engine,
				    struct drm_gem_object *object,
				    struct sg_table *sgt,
				    struct chrome9_sf_mapping **out_mapping,
				    u32 *out_base)
{
	struct chrome9_sf_mapping *mapping;
	unsigned int page_count;
	int cleanup_ret;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	if (!out_mapping || !out_base || !object || !sgt ||
	    object->dev != &engine->cdev->drm || drm_gem_is_imported(object) ||
	    !object->size || !IS_ALIGNED(object->size, PAGE_SIZE) ||
	    object->size > U32_MAX)
		return -EINVAL;
	if (engine->stopping || engine->wedged || !engine->ready)
		return -EIO;

	/* A KMS pin already supplies a stable mapping for this local object. */
	list_for_each_entry(mapping, &engine->scanout_mappings, link) {
		if (mapping->object != object || !mapping->bound ||
		    mapping->quarantined)
			continue;
		engine->draw_cache_scanout_hits++;
		dev_dbg(&engine->cdev->pdev->dev,
			"draw SF scanout hit base=%#llx pages=%u hits=%llu\n",
			(unsigned long long)mapping->node.start,
			mapping->page_count, engine->draw_cache_scanout_hits);
		*out_mapping = NULL;
		*out_base = mapping->node.start;
		return 0;
	}

	mapping = chrome9_draw_mapping_find_locked(engine, object);
	if (mapping) {
		mapping->users++;
		list_move_tail(&mapping->link, &engine->draw_mappings);
		engine->draw_cache_hits++;
		dev_dbg(&engine->cdev->pdev->dev,
			"draw SF cache hit base=%#llx pages=%u hits=%llu\n",
			(unsigned long long)mapping->node.start,
			mapping->page_count, engine->draw_cache_hits);
		*out_mapping = mapping;
		*out_base = mapping->node.start;
		return 0;
	}

	engine->draw_cache_misses++;
	page_count = object->size >> PAGE_SHIFT;
	if (page_count > CHROME9_DRAW_CACHE_MAX_PAGES)
		return -E2BIG;
	while (engine->draw_cache_entries >= CHROME9_DRAW_CACHE_MAX_ENTRIES ||
	       engine->draw_cache_pages >
	       CHROME9_DRAW_CACHE_MAX_PAGES - page_count) {
		ret = chrome9_draw_mapping_evict_lru_locked(engine);
		if (ret)
			return ret;
	}

	mapping = kzalloc_obj(*mapping);
	if (!mapping)
		return -ENOMEM;
	mapping->engine = engine;
	mapping->object = object;
	drm_gem_object_get(object);
	mapping->page_count = page_count;
	mapping->users = 1;
	mapping->draw_cached = true;
	INIT_LIST_HEAD(&mapping->link);
	mapping->ptes = kcalloc(page_count, sizeof(*mapping->ptes), GFP_KERNEL);
	if (!mapping->ptes) {
		ret = -ENOMEM;
		goto err_free;
	}
	mapping->sgt = sgt;
	ret = chrome9_collect_sgt_ptes(mapping->sgt, mapping->ptes,
				       page_count, !chrome9_gart_nosnoop);
	if (ret)
		goto err_free;

	for (;;) {
		mutex_lock(&engine->vm_lock);
		ret = drm_mm_insert_node_generic(&engine->sf_mm, &mapping->node,
						 object->size, PAGE_SIZE, 0,
						 DRM_MM_INSERT_LOW);
		mutex_unlock(&engine->vm_lock);
		if (ret != -ENOSPC)
			break;
		ret = chrome9_draw_mapping_evict_lru_locked(engine);
		if (ret)
			goto err_free;
	}
	if (ret)
		goto err_free;
	mapping->node_allocated = true;
	mapping->bound = true;
	mutex_lock(&engine->vm_lock);
	ret = chrome9_gart_set_kick(engine, mapping->node.start >> PAGE_SHIFT,
			       mapping->ptes, mapping->page_count, true);
	if (ret) {
		cleanup_ret = chrome9_gart_set_kick(engine,
					       mapping->node.start >> PAGE_SHIFT,
					       NULL, mapping->page_count, true);
		if (cleanup_ret) {
			engine->wedged = true;
			mapping->quarantined = true;
			mapping->users = 0;
			list_add_tail(&mapping->link, &engine->draw_mappings);
			engine->draw_cache_entries++;
			engine->draw_cache_pages += mapping->page_count;
			chrome9_take_quarantine_hold(engine);
			mutex_unlock(&engine->vm_lock);
			dev_crit(&engine->cdev->pdev->dev,
				 "SF draw-cache bind cleanup failed bind=%d cleanup=%d; mapping retained for reboot\n",
				 ret, cleanup_ret);
			return -EIO;
		}
		mapping->bound = false;
		drm_mm_remove_node(&mapping->node);
		mapping->node_allocated = false;
		mutex_unlock(&engine->vm_lock);
		goto err_free;
	}
	list_add_tail(&mapping->link, &engine->draw_mappings);
	engine->draw_cache_entries++;
	engine->draw_cache_pages += mapping->page_count;
	mutex_unlock(&engine->vm_lock);
	dev_dbg(&engine->cdev->pdev->dev,
		"draw SF cache miss base=%#llx pages=%u misses=%llu entries=%u cached_pages=%u\n",
		(unsigned long long)mapping->node.start, mapping->page_count,
		engine->draw_cache_misses, engine->draw_cache_entries,
		engine->draw_cache_pages);
	*out_mapping = mapping;
	*out_base = mapping->node.start;
	return 0;

err_free:
	chrome9_sf_mapping_free(mapping);
	return ret;
}

static int chrome9_job_acquire_mapping_locked(struct chrome9_engine *engine,
					      struct chrome9_job *job,
					      struct drm_gem_object *object,
					      struct sg_table *sgt,
					      u32 *base)
{
	struct chrome9_sf_mapping *mapping;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	if (WARN_ON_ONCE(job->sf_mapping_count >= ARRAY_SIZE(job->sf_mappings)))
		return -E2BIG;
	ret = chrome9_draw_mapping_acquire_locked(engine, object, sgt, &mapping,
						  base);
	if (ret)
		return ret;
	if (mapping)
		job->sf_mappings[job->sf_mapping_count++] = mapping;
	return 0;
}

static void chrome9_job_release_mappings_locked(struct chrome9_engine *engine,
						struct chrome9_job *job)
{
	lockdep_assert_held(&engine->submit_lock);
	while (job->sf_mapping_count) {
		struct chrome9_sf_mapping *mapping =
			job->sf_mappings[--job->sf_mapping_count];

		if (WARN_ON_ONCE(!mapping->users))
			continue;
		mapping->users--;
		job->sf_mappings[job->sf_mapping_count] = NULL;
	}
}

static int chrome9_draw_cache_purge_locked(struct chrome9_engine *engine)
{
	struct chrome9_sf_mapping *mapping;
	struct chrome9_sf_mapping *next;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	list_for_each_entry_safe(mapping, next, &engine->draw_mappings, link) {
		if (mapping->users || mapping->quarantined)
			return -EBUSY;
		ret = chrome9_draw_mapping_evict_locked(engine, mapping);
		if (ret)
			return ret;
	}
	return 0;
}

int chrome9_sf_pin(struct chrome9_device *cdev, struct drm_gem_object *obj,
		   struct chrome9_sf_mapping **out_mapping)
{
	struct drm_gem_shmem_object *shmem;
	struct chrome9_sf_mapping *mapping;
	struct chrome9_engine *engine;
	int cleanup_ret;
	int ret;

	if (!out_mapping || !obj || obj->dev != &cdev->drm ||
	    drm_gem_is_imported(obj) || !obj->size ||
	    !IS_ALIGNED(obj->size, PAGE_SIZE) || obj->size > U32_MAX)
		return -EINVAL;
	engine = READ_ONCE(cdev->engine);
	if (!engine)
		return -ENODEV;

	mapping = kzalloc_obj(*mapping);
	if (!mapping)
		return -ENOMEM;
	mapping->engine = engine;
	mapping->object = obj;
	drm_gem_object_get(obj);
	mapping->page_count = obj->size >> PAGE_SHIFT;
	INIT_LIST_HEAD(&mapping->link);
	mapping->ptes = kcalloc(mapping->page_count, sizeof(u32), GFP_KERNEL);
	if (!mapping->ptes) {
		ret = -ENOMEM;
		goto err_free;
	}
	shmem = to_drm_gem_shmem_obj(obj);
	/* Shmem owns the cached pages_sgt until release; our GEM ref pins it. */
	mapping->sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(mapping->sgt)) {
		ret = PTR_ERR(mapping->sgt);
		mapping->sgt = NULL;
		goto err_free;
	}
	ret = chrome9_collect_sgt_ptes(mapping->sgt, mapping->ptes,
				       mapping->page_count, true);
	if (ret)
		goto err_free;

	ret = mutex_lock_interruptible(&engine->submit_lock);
	if (ret)
		goto err_free;
	if (engine->stopping || engine->wedged || !engine->ready) {
		ret = -EIO;
		goto err_unlock_submit;
	}
	mutex_lock(&engine->vm_lock);
	ret = drm_mm_insert_node_generic(&engine->sf_mm, &mapping->node,
					 obj->size, PAGE_SIZE, 0,
					 DRM_MM_INSERT_LOW);
	if (ret)
		goto err_unlock_vm;
	mapping->node_allocated = true;
	mapping->bound = true;
	ret = chrome9_gart_set_kick(engine, mapping->node.start >> PAGE_SHIFT,
			       mapping->ptes, mapping->page_count, true);
	if (ret) {
		/* A failed flush can leave a partly visible table: erase or retain. */
		cleanup_ret = chrome9_gart_set_kick(engine,
					       mapping->node.start >> PAGE_SHIFT,
					       NULL, mapping->page_count, true);
		if (cleanup_ret) {
			engine->wedged = true;
			mapping->quarantined = true;
			list_add_tail(&mapping->link,
				      &engine->scanout_mappings);
			chrome9_take_quarantine_hold(engine);
			dev_crit(&cdev->pdev->dev,
				 "SF scanout bind cleanup failed bind=%d cleanup=%d; mapping retained for reboot\n",
				 ret, cleanup_ret);
			mutex_unlock(&engine->vm_lock);
			mutex_unlock(&engine->submit_lock);
			return -EIO;
		}
		mapping->bound = false;
		drm_mm_remove_node(&mapping->node);
		mapping->node_allocated = false;
		goto err_unlock_vm;
	}
	list_add_tail(&mapping->link, &engine->scanout_mappings);
	mutex_unlock(&engine->vm_lock);
	mutex_unlock(&engine->submit_lock);

	dev_dbg(&cdev->pdev->dev,
		"pinned direct scanout BO size=%zu at SF %#llx (%u PTEs)\n",
		obj->size, (unsigned long long)mapping->node.start,
		mapping->page_count);
	*out_mapping = mapping;
	return 0;

err_unlock_vm:
	mutex_unlock(&engine->vm_lock);
err_unlock_submit:
	mutex_unlock(&engine->submit_lock);
err_free:
	chrome9_sf_mapping_free(mapping);
	return ret;
}

void chrome9_sf_unpin(struct chrome9_sf_mapping *mapping)
{
	struct chrome9_engine *engine;
	struct chrome9_device *cdev;
	int ret;

	if (!mapping)
		return;
	engine = mapping->engine;
	cdev = engine->cdev;
	mutex_lock(&engine->submit_lock);
	if (mapping->quarantined) {
		mutex_unlock(&engine->submit_lock);
		return;
	}
	if (engine->wedged || engine->stopping) {
		mapping->quarantined = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "SF scanout mapping retained after unsafe engine state\n");
		mutex_unlock(&engine->submit_lock);
		return;
	}

	mutex_lock(&engine->vm_lock);
	ret = chrome9_gart_set(engine, mapping->node.start >> PAGE_SHIFT,
			       NULL, mapping->page_count);
	if (ret) {
		engine->wedged = true;
		mapping->quarantined = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "SF scanout unbind failed; mapping retained for reboot\n");
		mutex_unlock(&engine->vm_lock);
		mutex_unlock(&engine->submit_lock);
		return;
	}
	mapping->bound = false;
	drm_mm_remove_node(&mapping->node);
	mapping->node_allocated = false;
	list_del_init(&mapping->link);
	mutex_unlock(&engine->vm_lock);
	mutex_unlock(&engine->submit_lock);

	dev_dbg(&cdev->pdev->dev, "unpinned direct SF scanout mapping\n");
	chrome9_sf_mapping_free(mapping);
}

u32 chrome9_sf_mapping_base(const struct chrome9_sf_mapping *mapping)
{
	return mapping->node.start;
}

void chrome9_sf_sync_for_device(struct chrome9_sf_mapping *mapping)
{
	struct device *dev = &mapping->engine->cdev->pdev->dev;

	dma_sync_sgtable_for_device(dev, mapping->sgt, DMA_BIDIRECTIONAL);
	dma_wmb();
}

static void
chrome9_sf_resync_object_locked(struct chrome9_engine *engine,
				struct drm_gem_object *object)
{
	struct chrome9_sf_mapping *mapping;

	lockdep_assert_held(&engine->submit_lock);
	list_for_each_entry(mapping, &engine->scanout_mappings, link) {
		if (mapping->object != object || !mapping->bound ||
		    mapping->quarantined)
			continue;
		/* Retirement-side CPU inspection must return scanout to device. */
		chrome9_sf_sync_for_device(mapping);
		return;
	}
}

int chrome9_engine_display_begin(struct chrome9_device *cdev)
{
	struct chrome9_engine *engine = READ_ONCE(cdev->engine);

	if (!engine)
		return -ENODEV;
	/* Atomic enable/disable callbacks cannot propagate an interrupted lock. */
	mutex_lock(&engine->submit_lock);
	if (engine->stopping || engine->wedged || !engine->ready) {
		mutex_unlock(&engine->submit_lock);
		return -EIO;
	}

	/* Windows disables IGA1 T11 automatic swapping before base updates. */
	writel(CHROME9_DISPLAY_TRANSSET_A12,
	       cdev->mmio + CHROME9_REG_DISPLAY_TRANSSET);
	writel(CHROME9_DISPLAY_TRANSPACE_T11,
	       cdev->mmio + CHROME9_REG_DISPLAY_TRANSPACE);
	readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	return 0;
}

void chrome9_engine_display_end(struct chrome9_device *cdev)
{
	struct chrome9_engine *engine = cdev->engine;

	lockdep_assert_held(&engine->submit_lock);
	mutex_unlock(&engine->submit_lock);
}

static bool chrome9_verify_target(struct chrome9_job *job, u32 flags)
{
	u32 *words = job->map.vaddr;
	u32 *target = words + CHROME9_TARGET_WORDS;
	u32 expected = flags == DRM_CHROME9_FIXED_QUAD_CONSTANT_RED ?
		       CHROME9_TARGET_RED : CHROME9_TARGET_RESULT;
	unsigned int i;
	unsigned int changed = 0;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		if (words[i] != CHROME9_GUARD_WORD ||
		    words[2 * CHROME9_TARGET_WORDS + i] != CHROME9_GUARD_WORD)
			return false;
		if (target[i] != CHROME9_TARGET_SENTINEL)
			changed++;
	}
	if (flags & (DRM_CHROME9_FIXED_QUAD_VS_CLIP_WIDE |
		     DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR |
		     DRM_CHROME9_FIXED_QUAD_VS_CLIP_NEAR_TAIL))
		return true;
	if (flags == DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE ||
	    flags == DRM_CHROME9_FIXED_QUAD_VS_HALF)
		return changed >= 400 && changed <= 600 &&
		       target[8 * CHROME9_TARGET_DIM + 24] !=
			CHROME9_TARGET_SENTINEL &&
		       target[24 * CHROME9_TARGET_DIM + 8] ==
			CHROME9_TARGET_SENTINEL;
	for (i = 0; i < CHROME9_TARGET_WORDS; i++)
		if (target[i] != expected)
			return false;
	return true;
}

static bool chrome9_verify_surface_guards(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	u32 *last = words + (job->destination->base.size >> 2) -
		    CHROME9_TARGET_WORDS;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++)
		if (words[i] != CHROME9_GUARD_WORD ||
		    last[i] != CHROME9_GUARD_WORD)
			return false;
	return true;
}

static void chrome9_prepare_target(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		words[i] = CHROME9_GUARD_WORD;
		words[CHROME9_TARGET_WORDS + i] = CHROME9_TARGET_SENTINEL;
		words[2 * CHROME9_TARGET_WORDS + i] = CHROME9_GUARD_WORD;
	}
}

static void chrome9_prepare_surface_guards(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	u32 *last = words + (job->destination->base.size >> 2) -
		    CHROME9_TARGET_WORDS;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		words[i] = CHROME9_GUARD_WORD;
		last[i] = CHROME9_GUARD_WORD;
	}
}

static void chrome9_prepare_map_guards(struct iosys_map *map, u64 size)
{
	u32 *words = map->vaddr;
	u32 *last = words + (size >> 2) - CHROME9_TARGET_WORDS;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		words[i] = CHROME9_GUARD_WORD;
		last[i] = CHROME9_GUARD_WORD;
	}
}

static bool chrome9_verify_map_guards(struct iosys_map *map, u64 size)
{
	u32 *words = map->vaddr;
	u32 *last = words + (size >> 2) - CHROME9_TARGET_WORDS;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++)
		if (words[i] != CHROME9_GUARD_WORD ||
		    last[i] != CHROME9_GUARD_WORD)
			return false;
	return true;
}

/* Only the guard pages carry CPU stores of an explicit-access object. */
static void chrome9_flush_map_guards(struct iosys_map *map, u64 size)
{
	chrome9_cache_flush(map->vaddr, PAGE_SIZE);
	chrome9_cache_flush((u8 *)map->vaddr + size - PAGE_SIZE, PAGE_SIZE);
}

/*
 * Invalidate lines older than the device's writes (caller holds the resv
 * lock) and optionally leave the explicit protocol.
 */
static int chrome9_bo_cpu_sync_locked(struct chrome9_bo *bo)
{
	struct iosys_map map;
	int ret;

	if (!bo->device_written)
		return 0;
	ret = drm_gem_shmem_vmap_locked(&bo->base, &map);
	if (ret)
		return ret;
	if (!map.is_iomem)
		chrome9_cache_flush(map.vaddr, bo->base.base.size);
	drm_gem_shmem_vunmap_locked(&bo->base, &map);
	bo->device_written = false;
	return 0;
}

/* KMS scanout reads the pages without announcing CPU or device access. */
void chrome9_bo_force_implicit(struct drm_gem_object *obj)
{
	struct chrome9_bo *bo = to_chrome9_bo(obj);

	dma_resv_lock(obj->resv, NULL);
	chrome9_bo_cpu_sync_locked(bo);
	bo->explicit_access = false;
	bo->cpu_dirty = true;
	dma_resv_unlock(obj->resv);
}

int chrome9_gem_cpu_access_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_chrome9_gem_cpu_access *args = data;
	const u32 prepare = DRM_CHROME9_GEM_CPU_PREPARE_READ |
			    DRM_CHROME9_GEM_CPU_PREPARE_WRITE;
	struct drm_gem_object *obj;
	struct chrome9_bo *bo;
	long wait;
	int ret;

	if (!args->flags ||
	    args->flags & ~(prepare | DRM_CHROME9_GEM_CPU_EXPLICIT |
			    DRM_CHROME9_GEM_CPU_IMPLICIT) ||
	    (args->flags & DRM_CHROME9_GEM_CPU_EXPLICIT &&
	     args->flags & DRM_CHROME9_GEM_CPU_IMPLICIT))
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	if (obj->dev != drm || drm_gem_is_imported(obj)) {
		ret = -EINVAL;
		goto out_put;
	}
	bo = to_chrome9_bo(obj);
	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		goto out_put;
	wait = dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_BOOKKEEP, true,
				     CHROME9_DEP_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_unlock;
	}
	/* Invalidate lines that may predate the device's writes. */
	if (args->flags & (prepare | DRM_CHROME9_GEM_CPU_IMPLICIT)) {
		ret = chrome9_bo_cpu_sync_locked(bo);
		if (ret)
			goto out_unlock;
	}
	if (args->flags & (DRM_CHROME9_GEM_CPU_PREPARE_WRITE |
			   DRM_CHROME9_GEM_CPU_EXPLICIT))
		bo->cpu_dirty = true;
	if (args->flags & DRM_CHROME9_GEM_CPU_EXPLICIT)
		bo->explicit_access = true;
	if (args->flags & DRM_CHROME9_GEM_CPU_IMPLICIT)
		bo->explicit_access = false;
out_unlock:
	dma_resv_unlock(obj->resv);
out_put:
	drm_gem_object_put(obj);
	return ret;
}

static void chrome9_copy_rect_cache_flush(struct chrome9_job *job,
		const struct chrome9_draw_request *request, bool after_device)
{
	bool unguarded = request->flags &
			 DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED;
	u8 *destination = (u8 *)job->map.vaddr + (unguarded ? 0 : PAGE_SIZE);
	u8 *source = (u8 *)job->depth_map.vaddr + PAGE_SIZE;
	size_t row_bytes = request->copy_width * sizeof(u32);
	unsigned int y;

	/*
	 * Only the validated source and destination rectangles participate in
	 * this closed 2D operation.  Before execution, publish both rectangles;
	 * after a read-only source operation, invalidate only the destination.
	 * Include the corresponding kernel-owned guard pages in each phase.
	 */
	mb();
#ifdef CONFIG_X86
	if (!unguarded) {
		clflush_cache_range(job->map.vaddr, PAGE_SIZE);
		clflush_cache_range((u8 *)job->map.vaddr + request->object_size -
				    PAGE_SIZE, PAGE_SIZE);
	}
	if (!after_device) {
		clflush_cache_range(job->depth_map.vaddr, PAGE_SIZE);
		clflush_cache_range((u8 *)job->depth_map.vaddr +
				    request->depth_object_size - PAGE_SIZE,
				    PAGE_SIZE);
	}
	for (y = 0; y < request->copy_height; y++) {
		clflush_cache_range(destination +
			(request->destination_y + y) * request->pitch +
			request->destination_x * sizeof(u32), row_bytes);
		if (!after_device)
			clflush_cache_range(source +
				(request->source_y + y) * request->texture_pitch +
				request->source_x * sizeof(u32), row_bytes);
	}
#endif
	mb();
}

static void chrome9_prepare_depth_oracle(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	u16 *depth = (u16 *)(words + 4 * CHROME9_TARGET_WORDS);
	unsigned int i;

	for (i = 0; i < 6 * CHROME9_TARGET_WORDS; i++)
		words[i] = CHROME9_GUARD_WORD;
	for (i = 0; i < CHROME9_TARGET_WORDS; i++)
		words[CHROME9_TARGET_WORDS + i] = 0;
	for (i = 0; i < PAGE_SIZE / sizeof(*depth); i++)
		depth[i] = 0xffff;
}

static void chrome9_prepare_blend_oracle(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		words[i] = CHROME9_GUARD_WORD;
		words[CHROME9_TARGET_WORDS + i] = 0;
		words[2 * CHROME9_TARGET_WORDS + i] = CHROME9_GUARD_WORD;
	}
}

static bool chrome9_verify_depth_oracle(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	u32 *color = words + CHROME9_TARGET_WORDS;
	u16 *depth = (u16 *)(words + 4 * CHROME9_TARGET_WORDS);
	unsigned int covered = 0;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		if (words[i] != CHROME9_GUARD_WORD ||
		    words[2 * CHROME9_TARGET_WORDS + i] != CHROME9_GUARD_WORD ||
		    words[3 * CHROME9_TARGET_WORDS + i] != CHROME9_GUARD_WORD ||
		    words[5 * CHROME9_TARGET_WORDS + i] != CHROME9_GUARD_WORD)
			return false;
		if (color[i] == CHROME9_TARGET_RED && depth[i] == 0)
			covered++;
		else if (color[i] != 0 || depth[i] != 0xffff)
			return false;
	}
	for (i = CHROME9_TARGET_WORDS;
	     i < PAGE_SIZE / sizeof(*depth); i++)
		if (depth[i] != 0xffff)
			return false;
	return covered >= 400 && covered <= 600;
}

static bool chrome9_verify_blend_oracle(struct chrome9_job *job)
{
	u32 *words = job->map.vaddr;
	u32 *color = words + CHROME9_TARGET_WORDS;
	unsigned int covered = 0;
	unsigned int i;

	for (i = 0; i < CHROME9_TARGET_WORDS; i++) {
		if (words[i] != CHROME9_GUARD_WORD ||
		    words[2 * CHROME9_TARGET_WORDS + i] != CHROME9_GUARD_WORD)
			return false;
		if (color[i] == 0x00408000)
			covered++;
		else if (color[i] != 0)
			return false;
	}
	return covered >= 400 && covered <= 600;
}

#define CHROME9_TIMEOUT_SEGMENT_MAGIC	0xc9d00001U
#define CHROME9_TIMEOUT_HEADER_WORDS	8U

/* Keep the whole failing segment (plus read-ahead) for offline replay. */
static void chrome9_capture_timeout_segment(struct chrome9_engine *engine,
					    struct chrome9_job *job)
{
	u32 start = job->ring_pause - job->segment_pause;
	u32 end = min_t(u32, job->ring_stop + CHROME9_CR_READ_AHEAD,
			CHROME9_RING_SIZE);
	size_t size;
	u32 *copy;

	lockdep_assert_held(&engine->submit_lock);
	if (job->ring_pause < job->segment_pause || end <= start)
		return;
	size = (CHROME9_TIMEOUT_HEADER_WORDS * sizeof(u32)) + (end - start);
	copy = kvmalloc(size, GFP_KERNEL);
	if (!copy)
		return;
	copy[0] = CHROME9_TIMEOUT_SEGMENT_MAGIC;
	copy[1] = start;
	copy[2] = job->segment_pause;
	copy[3] = job->segment_stop;
	copy[4] = job->fence_id;
	copy[5] = job->cookie;
	copy[6] = job->kind;
	copy[7] = job->flags;
	dma_rmb();
	memcpy(copy + CHROME9_TIMEOUT_HEADER_WORDS,
	       (u8 *)engine->ring_cpu + start, end - start);
	kvfree(engine->timeout_segment);
	engine->timeout_segment = copy;
	engine->timeout_segment_size = size;
}

ssize_t chrome9_engine_read_timeout_segment(struct chrome9_device *cdev,
					    char *buf, loff_t off,
					    size_t count)
{
	struct chrome9_engine *engine = READ_ONCE(cdev->engine);
	ssize_t ret = 0;

	if (!engine)
		return -ENODEV;
	mutex_lock(&engine->submit_lock);
	if (off >= 0 && off < engine->timeout_segment_size) {
		ret = min_t(size_t, count, engine->timeout_segment_size - off);
		memcpy(buf, (u8 *)engine->timeout_segment + off, ret);
	}
	mutex_unlock(&engine->submit_lock);
	return ret;
}

static int chrome9_wait_job(struct chrome9_engine *engine,
			    struct chrome9_job *job)
{
	struct chrome9_device *cdev = engine->cdev;
	const char *phase = "fe33";
	u32 cookie;
	u32 cr_status;
	u32 engine_status;
	u32 cr_current;
	u32 value;
	ktime_t t = ktime_get(), now;
	int ret;

	if (job->fence_id) {
		ret = read_poll_timeout(READ_ONCE, value, value == job->fence_id,
					10, CHROME9_HW_TIMEOUT_US, false,
					engine->fence_cpu[0]);
		if (ret)
			goto timeout;
		dma_rmb();
		if (!engine->fence_count++)
			dev_info(&cdev->pdev->dev,
				 "FE33 first 3D completion id=%08x slot=%pad kind=%u\n",
				 job->fence_id, &engine->fence_dma, job->kind);
		if (READ_ONCE(engine->fence_cpu[1]) != CHROME9_FE33_CANARY + 1 ||
		    READ_ONCE(engine->fence_cpu[2]) != CHROME9_FE33_CANARY + 2 ||
		    READ_ONCE(engine->fence_cpu[3]) != CHROME9_FE33_CANARY + 3) {
			engine->fence_canary_faults++;
			dev_warn_ratelimited(&cdev->pdev->dev,
				"FE33 id=%08x wrote beyond its ID dword: %08x %08x %08x\n",
				job->fence_id, READ_ONCE(engine->fence_cpu[1]),
				READ_ONCE(engine->fence_cpu[2]),
				READ_ONCE(engine->fence_cpu[3]));
		}
	}
	now = ktime_get();
	chrome9_prof_wait_ns[0] += ktime_to_ns(ktime_sub(now, t));
	t = now;
	phase = "cookie";
	ret = readl_poll_timeout(cdev->mmio + CHROME9_2D_REG_DST_POS, value,
				 value == job->cookie, 10,
				 CHROME9_HW_TIMEOUT_US);
	if (ret)
		goto timeout;
	now = ktime_get();
	chrome9_prof_wait_ns[1] += ktime_to_ns(ktime_sub(now, t));
	t = now;
	phase = "pause";
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, value,
				 value & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (ret)
		goto timeout;
	/* A reader stopped at its owned PAUSE may legally retain CR busy bit 4.
	 * The cookie orders the submitted body before PAUSE; only the non-CR
	 * engine work bits must drain before the job can retire. */
	now = ktime_get();
	chrome9_prof_wait_ns[2] += ktime_to_ns(ktime_sub(now, t));
	t = now;
	phase = "work";
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
				 value, !(value & CHROME9_ENGINE_WORK_MASK), 10,
				 CHROME9_HW_TIMEOUT_US);
	chrome9_prof_wait_ns[3] += ktime_to_ns(ktime_sub(ktime_get(), t));
	if (!ret)
		return 0;

timeout:
	cookie = readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
	cr_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	engine_status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	cr_current = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	dev_err(&cdev->pdev->dev,
		"job timeout phase=%s seq=%llu kind=%u flags=%#x epoch=%u cookie=%08x/%08x current=%08x pause=%#x stop=%#x cr=%08x engine=%08x\n",
		phase, job->fence.base.seqno, job->kind, job->flags,
		engine->ring_epoch, cookie, job->cookie, cr_current,
		job->ring_pause, job->ring_stop, cr_status, engine_status);
	if (job->fence_id)
		dev_err(&cdev->pdev->dev,
			"FE33 id=%08x slot=%pad [%08x %08x %08x %08x] 0494=%08x 040c=%08x\n",
			job->fence_id, &engine->fence_dma,
			READ_ONCE(engine->fence_cpu[0]),
			READ_ONCE(engine->fence_cpu[1]),
			READ_ONCE(engine->fence_cpu[2]),
			READ_ONCE(engine->fence_cpu[3]),
			readl(cdev->mmio + CHROME9_REG_CURRENT_FENCE),
			readl(cdev->mmio + CHROME9_REG_ENGINE_ERROR));
	chrome9_capture_timeout_segment(engine, job);
	if (!(cr_current & 3) && cr_current < CHROME9_RING_SIZE) {
		u32 first = cr_current >= CHROME9_CR_READ_AHEAD ?
			    cr_current - CHROME9_CR_READ_AHEAD : 0;
		u32 last = min_t(u32, cr_current + 0x20,
				 CHROME9_RING_SIZE);
		u32 *ring = engine->ring_cpu;

		dma_rmb();
		dev_err(&cdev->pdev->dev,
			"ring window [%#x,%#x), current=%#x (cursor may lead decode by read-ahead)\n",
			first, last, cr_current);
		for (u32 offset = first; offset < last; offset += sizeof(u32))
			dev_err(&cdev->pdev->dev, "ring[%#04x]=%08x%s\n",
				offset, ring[offset / sizeof(u32)],
				offset == cr_current ? " <- current" : "");
	}
	return ret;
}

bool chrome9_engine_ready(const struct chrome9_device *cdev)
{
	struct chrome9_engine *engine = READ_ONCE(cdev->engine);

	return engine && READ_ONCE(engine->ready) &&
	       !READ_ONCE(engine->wedged);
}

bool chrome9_textured_triangle_run_enabled(void)
{
	return READ_ONCE(chrome9_textured_triangle_run);
}

bool chrome9_textured_triangle_mip_enabled(void)
{
	return READ_ONCE(chrome9_textured_triangle_mip);
}

bool chrome9_rhw_chain_enabled(void)
{
	return READ_ONCE(chrome9_rhw_chain);
}

bool chrome9_ps_add_enabled(void)
{
	return READ_ONCE(chrome9_ps_add);
}

bool chrome9_fragment_sparse_textures_enabled(void)
{
	return READ_ONCE(chrome9_fragment_sparse_textures);
}

static int chrome9_fragment_texture_selftest(void);

int chrome9_engine_init(struct chrome9_device *cdev)
{
	struct chrome9_engine *engine;
	resource_size_t gart_offset;
	u32 pause;
	u32 stop;
	u32 status;
	u16 command;
	int ret;

	if (!chrome9_engine_enable)
		return 0;
#ifndef CONFIG_X86
	return -EOPNOTSUPP;
#endif
	ret = chrome9_fragment_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"semantic fragment compiler self-test failed: %d\n", ret);
		return ret;
	}
	ret = chrome9_fragment_texture_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"fragment texture-state self-test failed: %d\n", ret);
		return ret;
	}
	ret = chrome9_fragment_raster_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"fragment raster-state self-test failed: %d\n", ret);
		return ret;
	}
	ret = chrome9_stencil_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"stencil-state self-test failed: %d\n", ret);
		return ret;
	}
	ret = chrome9_fragment_fe_config_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"fragment FE configuration self-test failed: %d\n", ret);
		return ret;
	}
	ret = chrome9_vertex_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev,
			"semantic vertex compiler self-test failed: %d\n", ret);
		return ret;
	}
	ret = chrome9_fe33_selftest();
	if (ret) {
		dev_err(&cdev->pdev->dev, "FE33 encoder self-test failed: %d\n",
			ret);
		return ret;
	}
	/* Diagnostics may already have mapped either BAR through pcim_iomap(). */
	cdev->mmio = chrome9_iomap_bar_once(cdev->pdev, 1);
	cdev->vram = chrome9_iomap_bar_once(cdev->pdev, 2);
	if (!cdev->mmio || !cdev->vram)
		return -ENOMEM;
	ret = chrome9_detect_vram(cdev);
	if (ret)
		return ret;
	if (cdev->vram_size < CHROME9_GART_SIZE)
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"detected VRAM is too small for the final 64-KiB GART\n");
	gart_offset = cdev->vram_size - CHROME9_GART_SIZE;
	if (chrome9_engine_gart_offset != ULONG_MAX &&
	    (!IS_ALIGNED(chrome9_engine_gart_offset, PAGE_SIZE) ||
	     chrome9_engine_gart_offset != gart_offset))
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"engine_gart_offset %#lx does not match detected final 64-KiB GART at %#llx\n",
			chrome9_engine_gart_offset,
			(unsigned long long)gart_offset);

	engine = kzalloc_obj(*engine);
	if (!engine)
		return -ENOMEM;
	engine->cdev = cdev;
	mutex_init(&engine->submit_lock);
	mutex_init(&engine->vm_lock);
	INIT_LIST_HEAD(&engine->quarantine);
	INIT_LIST_HEAD(&engine->scanout_mappings);
	INIT_LIST_HEAD(&engine->draw_mappings);
	engine->fence_context = dma_fence_context_alloc(1);
	atomic64_set(&engine->fence_seqno, 0);
	engine->gart_offset = gart_offset;
	engine->gart = cdev->vram + engine->gart_offset;
	engine->gart_saved = kvzalloc(CHROME9_GART_SIZE, GFP_KERNEL);
	if (!engine->gart_saved) {
		ret = -ENOMEM;
		goto err_free_engine;
	}
	memcpy_fromio(engine->gart_saved, engine->gart, CHROME9_GART_SIZE);
	engine->ring_cpu = dma_alloc_coherent(&cdev->pdev->dev,
					      CHROME9_RING_SIZE,
					      &engine->ring_dma, GFP_KERNEL);
	if (!engine->ring_cpu) {
		ret = -ENOMEM;
		goto err_free_saved;
	}
	if (!IS_ALIGNED(engine->ring_dma, PAGE_SIZE) ||
	    upper_32_bits(engine->ring_dma) ||
	    engine->ring_dma > U32_MAX - CHROME9_RING_SIZE) {
		ret = -ERANGE;
		goto err_free_ring;
	}
	if (chrome9_fe33_fence) {
		/* Direct PCI-master target: never an SF/GART page. */
		engine->fence_cpu = dma_alloc_coherent(&cdev->pdev->dev,
						       PAGE_SIZE,
						       &engine->fence_dma,
						       GFP_KERNEL);
		if (!engine->fence_cpu) {
			ret = -ENOMEM;
			goto err_free_ring;
		}
		if (!IS_ALIGNED(engine->fence_dma, CHROME9_FE33_SLOT_SIZE) ||
		    upper_32_bits(engine->fence_dma) ||
		    engine->fence_dma > U32_MAX - (PAGE_SIZE - 1)) {
			ret = -ERANGE;
			goto err_free_ring;
		}
	}
	engine->scratch_cpu = dma_alloc_coherent(&cdev->pdev->dev, PAGE_SIZE,
						 &engine->scratch_dma, GFP_KERNEL);
	if (!engine->scratch_cpu) {
		ret = -ENOMEM;
		goto err_free_ring;
	}
	if (upper_32_bits(engine->scratch_dma)) {
		ret = -ERANGE;
		goto err_free_ring;
	}
	engine->scratch_pte = lower_32_bits(engine->scratch_dma >> PAGE_SHIFT) &
			      0x3fffffff;
	engine->kick_cpu = (u32 *)get_zeroed_page(GFP_KERNEL);
	if (!engine->kick_cpu) {
		ret = -ENOMEM;
		goto err_free_ring;
	}
	engine->staging_cpu = (void *)get_zeroed_page(GFP_KERNEL);
	if (!engine->staging_cpu) {
		ret = -ENOMEM;
		goto err_free_ring;
	}
	engine->run_staging_cpu = vzalloc(CHROME9_RUN_STAGING_SIZE);
	if (!engine->run_staging_cpu) {
		ret = -ENOMEM;
		goto err_free_ring;
	}
	cdev->engine = engine;
	engine->saved_dst_pos = readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
	ret = chrome9_gart_bind_ring(engine);
	if (ret)
		goto err_restore_gart;

	chrome9_init_2d_h6(cdev);
	chrome9_init_3d_h5s2vp1(cdev);
	writel(0x00110000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
	writel(0x06000000, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x07100000, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x00100000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
	writel(0x02000000, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	/* Complete the documented 3D initialization before publishing a ring. */
	wmb();
	ret = chrome9_build_prime(engine, &pause, &stop);
	if (ret)
		goto err_restore_gart;
	chrome9_fill_ring_dummy(engine);
	memcpy(engine->ring_cpu, engine->staging_cpu,
	       stop + CHROME9_CR_READ_AHEAD);
	chrome9_cache_flush(engine->ring_cpu, CHROME9_RING_SIZE);
	dma_wmb();
	pci_read_config_word(cdev->pdev, PCI_COMMAND, &command);
	engine->was_master = command & PCI_COMMAND_MASTER;
	pci_set_master(cdev->pdev);
	engine->ring_published = true;
	chrome9_install_stopped_ring(cdev, 1, CHROME9_RING_SIZE, pause);
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, status,
				 status & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
					 status,
					 !(status & CHROME9_ENGINE_WORK_MASK),
					 10, CHROME9_HW_TIMEOUT_US);
	if (ret) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "persistent CR prime timed out; retained for reboot\n");
		return 0;
	}
	drm_mm_init(&engine->sf_mm, CHROME9_RING_SIZE,
		    SZ_64M - CHROME9_RING_SIZE);
	engine->mm_initialized = true;
	engine->append = chrome9_ring_append_after(stop);
	engine->active_pause = pause;
	engine->active_stop = stop;
	engine->last_cookie = readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
	/* Persistent-engine mode is deliberately reboot-scoped during bring-up. */
	__module_get(THIS_MODULE);
	engine->module_hold = true;
	engine->ready = true;
	dev_info(&cdev->pdev->dev,
		 "persistent SF engine ready: GART=%#llx ring=%pad fe33=%s\n",
		 (unsigned long long)engine->gart_offset, &engine->ring_dma,
		 engine->fence_cpu ? "on" : "off");
	return 0;

err_restore_gart:
	if (chrome9_gart_restore(engine)) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "GART restore timed out; retained for reboot\n");
		return 0;
	}
err_free_ring:
	cdev->engine = NULL;
	vfree(engine->run_staging_cpu);
	if (engine->staging_cpu)
		free_page((unsigned long)engine->staging_cpu);
	free_page((unsigned long)engine->kick_cpu);
	if (engine->scratch_cpu)
		dma_free_coherent(&cdev->pdev->dev, PAGE_SIZE,
				  engine->scratch_cpu, engine->scratch_dma);
	if (engine->fence_cpu)
		dma_free_coherent(&cdev->pdev->dev, PAGE_SIZE, engine->fence_cpu,
				  engine->fence_dma);
	dma_free_coherent(&cdev->pdev->dev, CHROME9_RING_SIZE,
			  engine->ring_cpu, engine->ring_dma);
err_free_saved:
	kvfree(engine->gart_saved);
err_free_engine:
	kfree(engine);
	return ret;
}

/*
 * Give the GTI an outstanding request so a pending SR6F translation flush
 * can complete while every engine is idle: one cookie write through the
 * ring, whose PTEs never change.  VIA rev 3 uses a direct 2D blit for the
 * same purpose, but its MMIO would overwrite the cookie register.
 */
static int chrome9_gart_kick(struct chrome9_engine *engine)
{
	struct chrome9_job *job;
	u32 *p = engine->kick_cpu;
	bool must_quarantine;
	u32 cookie[2];
	u32 pause;
	int ret;

	lockdep_assert_held(&engine->submit_lock);
	if (!p || engine->stopping || engine->wedged)
		return -EIO;
	job = kzalloc_obj(*job);
	if (!job)
		return -ENOMEM;
	INIT_LIST_HEAD(&job->quarantine_link);
	job->cookie = engine->last_cookie == 0x00010001 ?
		      0x00020002 : 0x00010001;
	cookie[0] = CHROME9_2D_REG_DST_POS;
	cookie[1] = job->cookie;
	memset(p, 0xcc, PAGE_SIZE);
	chrome9_emit_header0(&p, cookie, 1);
	ret = chrome9_finish_ring(engine->kick_cpu, p, &pause,
				  &job->segment_stop);
	if (!ret)
		ret = chrome9_append_segment(engine, job, engine->kick_cpu,
					     PAGE_SIZE, pause,
					     &must_quarantine);
	if (!ret)
		ret = chrome9_wait_job(engine, job);
	else if (!must_quarantine)
		goto out;
	if (ret) {
		/* The ring may still own the segment: keep it for reboot. */
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&engine->cdev->pdev->dev,
			 "GART kick did not retire: %d\n", ret);
		goto out;
	}
	engine->last_cookie = job->cookie;
	engine->gart_flush_kicks++;
out:
	kfree(job);
	return ret;
}

static int chrome9_submit_draw(struct drm_device *drm, struct drm_file *file,
			       const struct chrome9_draw_request *request)
{
	struct chrome9_device *cdev = to_chrome9(drm);
	struct chrome9_engine *engine;
	struct drm_gem_object *obj;
	struct chrome9_job *job;
	struct dma_resv *resv;
	u32 pause;
	u32 target;
	long wait;
	bool signal = false;
	bool quarantine = false;
	bool must_quarantine;
	int dev_idx;
	int ret;

	/*
	 * Both the immediate and batched one-texture MODULATE+blend command
	 * objects have reproducibly stopped VX900 before their completion cookie.
	 * Reject the shared unsafe state at the ABI boundary until its remaining
	 * vendor state difference is proven; otherwise any client can wedge the
	 * ring and take rendering away from every later X client.
	 */
	if (request->texture_program == DRM_CHROME9_TEXTURE_PROGRAM_MODULATE &&
	    (request->blend.flags & DRM_CHROME9_BLEND_ENABLE) &&
	    (request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BLEND ||
	     request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND ||
	     request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16 ||
	     request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BATCH ||
	     request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN ||
	     request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP))
		return -EOPNOTSUPP;

	if (!drm_dev_enter(drm, &dev_idx))
		return -ENODEV;
	engine = READ_ONCE(cdev->engine);
	if (!engine || !chrome9_engine_ready(cdev))
		goto out_no_engine;
	obj = drm_gem_object_lookup(file, request->destination_handle);
	if (!obj) {
		ret = -ENOENT;
		goto out_dev_exit;
	}
	if (obj->dev != drm || drm_gem_is_imported(obj) ||
	    obj->size != request->object_size) {
		ret = -EINVAL;
		goto out_put_object;
	}

	job = kzalloc_obj(*job);
	if (!job) {
		ret = -ENOMEM;
		goto out_put_object;
	}
	INIT_LIST_HEAD(&job->quarantine_link);
	job->destination = to_drm_gem_shmem_obj(obj);
	job->sgt = drm_gem_shmem_get_pages_sgt(job->destination);
	if (IS_ERR(job->sgt)) {
		ret = PTR_ERR(job->sgt);
		goto out_free_job;
	}
	spin_lock_init(&job->fence.lock);
	dma_fence_init64(&job->fence.base, &chrome9_fence_ops,
			 &job->fence.lock, engine->fence_context,
			 atomic64_inc_return(&engine->fence_seqno));
	resv = obj->resv;
	ret = dma_resv_lock_interruptible(resv, NULL);
	if (ret)
		goto out_put_fence;
	wait = dma_resv_wait_timeout(resv, dma_resv_usage_rw(true), true,
				     CHROME9_DEP_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_unlock_resv;
	}
	ret = dma_resv_reserve_fences(resv, 1);
	if (ret)
		goto out_unlock_resv;
	ret = drm_gem_shmem_vmap_locked(job->destination, &job->map);
	if (ret)
		goto out_unlock_resv;
	job->mapped = true;
	if (job->map.is_iomem) {
		ret = -EINVAL;
		goto out_vunmap;
	}
	ret = mutex_lock_interruptible(&engine->submit_lock);
	if (ret)
		goto out_vunmap;
	if (engine->stopping || engine->wedged || !engine->ready) {
		ret = -EIO;
		goto out_unlock_submit;
	}
	job->kind = request->kind;
	job->flags = request->flags;
	job->cookie = engine->last_cookie == 0x00010001 ?
		      0x00020002 : 0x00010001;
	job->fence_id = 0;
	ret = chrome9_job_acquire_mapping_locked(engine, job, obj, job->sgt,
						 &target);
	if (ret)
		goto out_prepublication;

	if (request->kind == CHROME9_DRAW_FIXED) {
		/* The kernel initializes oracle storage after reclaiming it. */
		dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->sgt,
					 DMA_BIDIRECTIONAL);
		if (request->flags == DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE)
			chrome9_prepare_depth_oracle(job);
		else if (request->flags == DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE)
			chrome9_prepare_blend_oracle(job);
		else
			chrome9_prepare_target(job);
		if (request->flags == DRM_CHROME9_FIXED_QUAD_VS_FETCH)
			memcpy((u8 *)job->map.vaddr + 3 * PAGE_SIZE,
			       chrome9_vs_oracle_quad,
			       sizeof(chrome9_vs_oracle_quad));
	} else {
		chrome9_prepare_surface_guards(job);
	}
	chrome9_cache_flush(job->map.vaddr, request->object_size);
	dma_sync_sgtable_for_device(&cdev->pdev->dev, job->sgt,
				    DMA_BIDIRECTIONAL);
	dma_resv_add_fence(resv, &job->fence.base, DMA_RESV_USAGE_WRITE);
	job->fence_attached = true;
	target += PAGE_SIZE;
	if (request->kind == CHROME9_DRAW_FIXED) {
		if (request->flags == DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE)
			ret = chrome9_build_depth_oracle(engine, job, target,
						 target + 3 * PAGE_SIZE, &pause);
		else if (request->flags == DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE)
			ret = chrome9_build_blend_oracle(engine, job, target,
						 &pause);
		else
			ret = chrome9_build_fixed(engine, job, target,
						  request->flags, &pause);
	} else {
		ret = chrome9_build_triangle(engine, request, job, target, &pause);
	}
	if (ret)
		goto out_prepublication;
	ret = chrome9_append_segment(engine, job, engine->staging_cpu, PAGE_SIZE,
				     pause, &must_quarantine);
	if (ret) {
		if (!must_quarantine)
			goto out_prepublication;
		dma_fence_set_error(&job->fence.base, -ETIME);
		/*
		 * PID10 was published but did not retire.  Conservatively retain
		 * this job's mappings together with the still-owned ring.
		 */
		job->published = true;
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
		goto out_unlock_submit;
	}
	ret = chrome9_wait_job(engine, job);
	if (ret) {
		dma_fence_set_error(&job->fence.base, -ETIME);
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
		goto out_unlock_submit;
	}
	job->retirement_proven = true;
	dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->sgt,
				 DMA_BIDIRECTIONAL);
	chrome9_cache_flush(job->map.vaddr, request->object_size);
	dma_rmb();
	if ((request->kind == CHROME9_DRAW_FIXED &&
	     request->flags == DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE &&
	     !chrome9_verify_depth_oracle(job)) ||
	    (request->kind == CHROME9_DRAW_FIXED &&
	     request->flags == DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE &&
	     !chrome9_verify_blend_oracle(job)) ||
	    (request->kind == CHROME9_DRAW_FIXED &&
	     request->flags != DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE &&
	     request->flags != DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE &&
	     !chrome9_verify_target(job, request->flags)) ||
	    ((request->kind == CHROME9_DRAW_IMMEDIATE_TRIANGLE ||
	      request->kind == CHROME9_DRAW_IMMEDIATE_TRIANGLE_BLEND ||
	      request->kind == CHROME9_DRAW_IMMEDIATE_TRIANGLE_BATCH) &&
	     !chrome9_verify_surface_guards(job))) {
		ret = -EIO;
		dma_fence_set_error(&job->fence.base, ret);
	}
	chrome9_sf_resync_object_locked(engine, obj);
	engine->last_cookie = job->cookie;
	chrome9_job_release_mappings_locked(engine, job);
	signal = true;
	drm_gem_shmem_vunmap_locked(job->destination, &job->map);
	job->mapped = false;
	mutex_unlock(&engine->submit_lock);
	dma_resv_unlock(resv);
	dma_fence_signal(&job->fence.base);
	job->destination = NULL;
	drm_gem_object_put(obj);
	dma_fence_put(&job->fence.base);
	goto out_dev_exit;

out_prepublication:
	dma_fence_set_error(&job->fence.base, ret ?: -EIO);
	chrome9_job_release_mappings_locked(engine, job);
	signal = true;
out_unlock_submit:
	mutex_unlock(&engine->submit_lock);
	if (quarantine) {
		dma_resv_unlock(resv);
		if (signal)
			dma_fence_signal(&job->fence.base);
		ret = ret ?: -EIO;
		goto out_dev_exit;
	}
out_vunmap:
	if (job->mapped) {
		drm_gem_shmem_vunmap_locked(job->destination, &job->map);
		job->mapped = false;
	}
out_unlock_resv:
	dma_resv_unlock(resv);
	if (signal)
		dma_fence_signal(&job->fence.base);
out_put_fence:
	job->destination = NULL;
	drm_gem_object_put(obj);
	dma_fence_put(&job->fence.base);
	goto out_dev_exit;

out_free_job:
	kfree(job);
out_put_object:
	drm_gem_object_put(obj);
out_dev_exit:
	drm_dev_exit(dev_idx);
	return ret;

out_no_engine:
	ret = -EIO;
	goto out_dev_exit;
}

static int chrome9_submit_depth_triangle(struct drm_device *drm,
		struct drm_file *file,
		const struct chrome9_draw_request *request)
{
	struct chrome9_device *cdev = to_chrome9(drm);
	struct drm_gem_object *texture3_obj = NULL;
	struct drm_gem_object *texture2_obj = NULL;
	struct drm_gem_object *depth_obj = NULL;
	struct drm_gem_object *obj = NULL;
	struct chrome9_engine *engine;
	struct chrome9_job *job = NULL;
	struct chrome9_draw_request segment_request;
	const struct chrome9_draw_request *segment = request;
	struct drm_exec exec;
	u32 pause;
	u32 target;
	u32 depth_target;
	u32 texture2_target;
	u32 texture3_target;
	u64 mapped_size;
	long wait;
	bool exec_initialized = false;
	bool device_owned = false;
	bool signal = false;
	bool quarantine = false;
	bool must_quarantine;
	bool chained = request->kind ==
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_CHAIN;
	u32 chain_offset = 0;
	bool textured_depth =
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16 ||
		request->kind ==
		CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND ||
		((request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN ||
		  request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP) &&
		 (request->flags & DRM_CHROME9_TEXTURE_RUN_DEPTH));
	bool two_textured_depth =
		request->kind ==
			CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16 ||
		request->kind ==
			CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16_BLEND;
	bool four_objects = two_textured_depth;
	bool three_objects =
		request->kind == CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE ||
		textured_depth || four_objects;
	bool texture_objects = three_objects ||
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE ||
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BLEND ||
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BATCH ||
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN ||
		request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP ||
		request->kind == CHROME9_DRAW_COPY_RECT;
	int dev_idx;
	int ret;

	if (!drm_dev_enter(drm, &dev_idx))
		return -ENODEV;
	engine = READ_ONCE(cdev->engine);
	if (!engine || !chrome9_engine_ready(cdev)) {
		ret = -EIO;
		goto out_dev_exit;
	}
	obj = drm_gem_object_lookup(file, request->destination_handle);
	depth_obj = drm_gem_object_lookup(file, request->depth_handle);
	if (three_objects)
		texture2_obj = drm_gem_object_lookup(file,
						     request->texture2_handle);
	if (four_objects)
		texture3_obj = drm_gem_object_lookup(file,
						     request->texture3_handle);
	if (!obj || !depth_obj || (three_objects && !texture2_obj) ||
	    (four_objects && !texture3_obj)) {
		ret = -ENOENT;
		goto out_put_objects;
	}
	if (obj == depth_obj || (three_objects &&
	    (obj == texture2_obj || depth_obj == texture2_obj)) ||
	    (four_objects &&
	     (obj == texture3_obj || depth_obj == texture3_obj ||
	      texture2_obj == texture3_obj)) ||
	    obj->dev != drm || depth_obj->dev != drm ||
	    (three_objects && texture2_obj->dev != drm) ||
	    (four_objects && texture3_obj->dev != drm) ||
	    drm_gem_is_imported(obj) || drm_gem_is_imported(depth_obj) ||
	    (three_objects && drm_gem_is_imported(texture2_obj)) ||
	    (four_objects && drm_gem_is_imported(texture3_obj)) ||
	    obj->size != request->object_size ||
	    (request->kind == CHROME9_DRAW_COPY_RECT ?
	     depth_obj->size != request->depth_object_size :
	     request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP ?
	     depth_obj->size != request->texture_object_size :
	     request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN ?
	     depth_obj->size != request->depth_object_size :
	     texture_objects ?
	     depth_obj->size < request->depth_object_size :
	     depth_obj->size != request->depth_object_size) ||
	    (three_objects &&
	     (textured_depth ?
	      texture2_obj->size != request->texture2_object_size :
	      texture2_obj->size < request->texture2_object_size)) ||
	    (four_objects &&
	     texture3_obj->size != request->texture3_object_size)) {
		ret = -EINVAL;
		goto out_put_objects;
	}
	if (check_add_overflow(obj->size, depth_obj->size, &mapped_size) ||
	    (three_objects &&
	     check_add_overflow(mapped_size, texture2_obj->size,
				&mapped_size)) ||
	    (four_objects &&
	     check_add_overflow(mapped_size, texture3_obj->size,
				&mapped_size)) ||
	    mapped_size > SZ_64M - CHROME9_RING_SIZE) {
		ret = -E2BIG;
		goto out_put_objects;
	}

	job = kzalloc_obj(*job);
	if (!job) {
		ret = -ENOMEM;
		goto out_put_objects;
	}
	INIT_LIST_HEAD(&job->quarantine_link);
	job->destination = to_drm_gem_shmem_obj(obj);
	job->depth = to_drm_gem_shmem_obj(depth_obj);
	if (three_objects)
		job->texture2 = to_drm_gem_shmem_obj(texture2_obj);
	if (four_objects)
		job->texture3 = to_drm_gem_shmem_obj(texture3_obj);
	job->sgt = drm_gem_shmem_get_pages_sgt(job->destination);
	if (IS_ERR(job->sgt)) {
		ret = PTR_ERR(job->sgt);
		goto out_free_job;
	}
	job->depth_sgt = drm_gem_shmem_get_pages_sgt(job->depth);
	if (IS_ERR(job->depth_sgt)) {
		ret = PTR_ERR(job->depth_sgt);
		goto out_free_job;
	}
	if (three_objects) {
		job->texture2_sgt = drm_gem_shmem_get_pages_sgt(job->texture2);
		if (IS_ERR(job->texture2_sgt)) {
			ret = PTR_ERR(job->texture2_sgt);
			goto out_free_job;
		}
	}
	if (four_objects) {
		job->texture3_sgt = drm_gem_shmem_get_pages_sgt(job->texture3);
		if (IS_ERR(job->texture3_sgt)) {
			ret = PTR_ERR(job->texture3_sgt);
			goto out_free_job;
		}
	}
	spin_lock_init(&job->fence.lock);
	dma_fence_init64(&job->fence.base, &chrome9_fence_ops,
			 &job->fence.lock, engine->fence_context,
			 atomic64_inc_return(&engine->fence_seqno));
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT,
		      four_objects ? 4 : three_objects ? 3 : 2);
	exec_initialized = true;
	drm_exec_until_all_locked(&exec) {
		ret = drm_exec_prepare_obj(&exec, obj, 1);
		drm_exec_retry_on_contention(&exec);
		if (ret)
			goto out_exec;
		ret = drm_exec_prepare_obj(&exec, depth_obj, 1);
		drm_exec_retry_on_contention(&exec);
		if (ret)
			goto out_exec;
		if (three_objects) {
			ret = drm_exec_prepare_obj(&exec, texture2_obj, 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
		if (four_objects) {
			ret = drm_exec_prepare_obj(&exec, texture3_obj, 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	wait = dma_resv_wait_timeout(obj->resv, dma_resv_usage_rw(true), true,
				     CHROME9_DEP_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_exec;
	}
	wait = dma_resv_wait_timeout(depth_obj->resv, dma_resv_usage_rw(true), true,
				     CHROME9_DEP_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_exec;
	}
	if (three_objects) {
		wait = dma_resv_wait_timeout(texture2_obj->resv,
					     dma_resv_usage_rw(true), true,
					     CHROME9_DEP_TIMEOUT);
		if (wait <= 0) {
			ret = wait < 0 ? wait : -ETIMEDOUT;
			goto out_exec;
		}
	}
	if (four_objects) {
		wait = dma_resv_wait_timeout(texture3_obj->resv,
					     dma_resv_usage_rw(true), true,
					     CHROME9_DEP_TIMEOUT);
		if (wait <= 0) {
			ret = wait < 0 ? wait : -ETIMEDOUT;
			goto out_exec;
		}
	}
	ret = drm_gem_shmem_vmap_locked(job->destination, &job->map);
	if (ret)
		goto out_exec;
	job->mapped = true;
	if (job->map.is_iomem) {
		ret = -EINVAL;
		goto out_unmap;
	}
	ret = drm_gem_shmem_vmap_locked(job->depth, &job->depth_map);
	if (ret)
		goto out_unmap;
	job->depth_mapped = true;
	if (job->depth_map.is_iomem) {
		ret = -EINVAL;
		goto out_unmap;
	}
	if (three_objects) {
		ret = drm_gem_shmem_vmap_locked(job->texture2,
						 &job->texture2_map);
		if (ret)
			goto out_unmap;
		job->texture2_mapped = true;
		if (job->texture2_map.is_iomem) {
			ret = -EINVAL;
			goto out_unmap;
		}
	}
	if (four_objects) {
		ret = drm_gem_shmem_vmap_locked(job->texture3, &job->texture3_map);
		if (ret)
			goto out_unmap;
		job->texture3_mapped = true;
		if (job->texture3_map.is_iomem) {
			ret = -EINVAL;
			goto out_unmap;
		}
	}

	ret = mutex_lock_interruptible(&engine->submit_lock);
	if (ret)
		goto out_unmap;
	if (engine->stopping || engine->wedged || !engine->ready) {
		ret = -EIO;
		goto out_unlock_submit;
	}
	job->kind = request->kind;
	job->flags = request->flags;
	ret = chrome9_job_acquire_mapping_locked(engine, job, obj, job->sgt,
						 &target);
	if (ret)
		goto out_prepublication;
	ret = chrome9_job_acquire_mapping_locked(engine, job, depth_obj,
						 job->depth_sgt,
						 &depth_target);
	if (ret)
		goto out_prepublication;
	if (three_objects) {
		ret = chrome9_job_acquire_mapping_locked(engine, job,
							 texture2_obj,
							 job->texture2_sgt,
							 &texture2_target);
		if (ret)
			goto out_prepublication;
	}
	if (four_objects) {
		ret = chrome9_job_acquire_mapping_locked(engine, job,
							 texture3_obj,
							 job->texture3_sgt,
							 &texture3_target);
		if (ret)
			goto out_prepublication;
	}

	/* Structured BO contents are already CPU-owned on ioctl entry. */
	if (request->kind != CHROME9_DRAW_COPY_RECT ||
	    !(request->flags & DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED))
		chrome9_prepare_map_guards(&job->map, request->object_size);
	chrome9_prepare_map_guards(&job->depth_map,
				   request->depth_object_size);
	/*
	 * A mip-capable texture keeps the canonical L0 guard at the requested
	 * size and adds a final guard at the end of its larger allocation.
	 */
	if (texture_objects && depth_obj->size > request->depth_object_size)
		chrome9_prepare_map_guards(&job->depth_map, depth_obj->size);
	if (three_objects)
		chrome9_prepare_map_guards(&job->texture2_map,
					   request->texture2_object_size);
	if (three_objects && !textured_depth &&
	    texture2_obj->size > request->texture2_object_size)
		chrome9_prepare_map_guards(&job->texture2_map,
					   texture2_obj->size);
	if (four_objects)
		chrome9_prepare_map_guards(&job->texture3_map,
					   request->texture3_object_size);
	if (request->kind == CHROME9_DRAW_COPY_RECT)
		chrome9_copy_rect_cache_flush(job, request, false);
	else {
		chrome9_cache_flush(job->map.vaddr, request->object_size);
		chrome9_cache_flush(job->depth_map.vaddr, depth_obj->size);
	}
	if (three_objects)
		chrome9_cache_flush(job->texture2_map.vaddr,
				    texture2_obj->size);
	if (four_objects)
		chrome9_cache_flush(job->texture3_map.vaddr,
				    texture3_obj->size);
	dma_sync_sgtable_for_device(&cdev->pdev->dev, job->sgt,
				    DMA_BIDIRECTIONAL);
	dma_sync_sgtable_for_device(&cdev->pdev->dev, job->depth_sgt,
				    DMA_BIDIRECTIONAL);
	if (three_objects)
		dma_sync_sgtable_for_device(&cdev->pdev->dev,
					    job->texture2_sgt,
					    DMA_BIDIRECTIONAL);
	if (four_objects)
		dma_sync_sgtable_for_device(&cdev->pdev->dev,
					    job->texture3_sgt,
					    DMA_BIDIRECTIONAL);
	device_owned = true;
	dma_resv_add_fence(obj->resv, &job->fence.base,
			   DMA_RESV_USAGE_WRITE);
	dma_resv_add_fence(depth_obj->resv, &job->fence.base,
			   texture_objects ?
			   DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE);
	if (three_objects)
		dma_resv_add_fence(texture2_obj->resv, &job->fence.base,
				   textured_depth ? DMA_RESV_USAGE_WRITE :
				   DMA_RESV_USAGE_READ);
	if (four_objects)
		dma_resv_add_fence(texture3_obj->resv, &job->fence.base,
				   DMA_RESV_USAGE_WRITE);
	job->fence_attached = true;
	if (request->kind != CHROME9_DRAW_COPY_RECT ||
	    !(request->flags & DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED))
		target += PAGE_SIZE;
	depth_target += PAGE_SIZE;
	if (three_objects)
		texture2_target += PAGE_SIZE;
	if (four_objects)
		texture3_target += PAGE_SIZE;
	do {
		if (chained) {
			u32 count = min_t(u32,
				request->triangle_count - chain_offset,
				DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES);

			segment_request = *request;
			segment_request.kind =
				CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN;
			segment_request.triangle_count = count;
			segment_request.batch_vertices_rhw =
				request->batch_vertices_rhw + 3U * chain_offset;
			segment = &segment_request;
		}
		/*
		 * Every segment receives a new completion value.  Reusing the
		 * previous cookie could make the retirement poll pass before the
		 * newly published packet executes.
		 */
		job->cookie = engine->last_cookie == 0x00010001 ?
			      0x00020002 : 0x00010001;
		job->fence_id = 0;
		job->retirement_proven = false;
		if (segment->kind == CHROME9_DRAW_COPY_RECT)
			ret = chrome9_build_copy_rect(engine, segment, job, target,
						      depth_target, &pause);
		else if (segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE ||
			 two_textured_depth)
			ret = chrome9_build_two_textured_triangle(engine, segment,
					job, target, depth_target, texture2_target,
					two_textured_depth ? texture3_target : 0,
					&pause);
		else if (textured_depth)
			ret = chrome9_build_textured_triangle(engine, segment, job,
					target, depth_target, texture2_target, &pause);
		else if (segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE ||
			 segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BLEND ||
			 segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BATCH ||
			 segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN ||
			 segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP)
			ret = chrome9_build_textured_triangle(engine, segment, job,
						      target, depth_target, 0, &pause);
		else
			ret = chrome9_build_depth_triangle(engine, segment, job,
						   target, depth_target, &pause);
		if (ret)
			goto out_prepublication;
		if (segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN ||
		    segment->kind ==
			 CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH ||
		    segment->kind ==
			 CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP)
			ret = chrome9_append_segment(engine, job,
					     engine->run_staging_cpu,
					     CHROME9_RUN_STAGING_SIZE, pause,
					     &must_quarantine);
		else
			ret = chrome9_append_segment(engine, job,
					     engine->staging_cpu, PAGE_SIZE, pause,
					     &must_quarantine);
		if (ret) {
			if (!must_quarantine)
				goto out_prepublication;
			dma_fence_set_error(&job->fence.base, -ETIME);
			/*
			 * PID10 was published but did not retire.  Retain every
			 * mapping covered by the request's single fence.
			 */
			job->published = true;
			chrome9_quarantine_job_locked(engine, job);
			quarantine = true;
			goto out_unlock_submit;
		}
		ret = chrome9_wait_job(engine, job);
		if (ret) {
			dma_fence_set_error(&job->fence.base, -ETIME);
			chrome9_quarantine_job_locked(engine, job);
			quarantine = true;
			goto out_unlock_submit;
		}
		job->retirement_proven = true;
		engine->last_cookie = job->cookie;
		if (chained)
			chain_offset += segment->triangle_count;
	} while (chained && chain_offset < request->triangle_count);
	dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->sgt,
				 DMA_BIDIRECTIONAL);
	dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->depth_sgt,
				 DMA_BIDIRECTIONAL);
	if (three_objects)
		dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->texture2_sgt,
					 DMA_BIDIRECTIONAL);
	if (four_objects)
		dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->texture3_sgt,
					 DMA_BIDIRECTIONAL);
	if (request->kind == CHROME9_DRAW_COPY_RECT)
		chrome9_copy_rect_cache_flush(job, request, true);
	else {
		chrome9_cache_flush(job->map.vaddr, request->object_size);
		chrome9_cache_flush(job->depth_map.vaddr, depth_obj->size);
	}
	if (three_objects)
		chrome9_cache_flush(job->texture2_map.vaddr,
				    texture2_obj->size);
	if (four_objects)
		chrome9_cache_flush(job->texture3_map.vaddr,
				    texture3_obj->size);
	dma_rmb();
	device_owned = false;
	if ((!(request->kind == CHROME9_DRAW_COPY_RECT &&
	       (request->flags & DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED)) &&
	     !chrome9_verify_map_guards(&job->map, request->object_size)) ||
	    !chrome9_verify_map_guards(&job->depth_map,
				       request->depth_object_size) ||
	    (texture_objects &&
	     depth_obj->size > request->depth_object_size &&
	     !chrome9_verify_map_guards(&job->depth_map, depth_obj->size)) ||
	    (three_objects &&
	     !chrome9_verify_map_guards(&job->texture2_map,
					 request->texture2_object_size)) ||
	    (three_objects && !textured_depth &&
	     texture2_obj->size > request->texture2_object_size &&
	     !chrome9_verify_map_guards(&job->texture2_map,
					 texture2_obj->size)) ||
	    (four_objects &&
	     !chrome9_verify_map_guards(&job->texture3_map,
					 request->texture3_object_size))) {
		ret = -EIO;
		dma_fence_set_error(&job->fence.base, ret);
	}
	chrome9_sf_resync_object_locked(engine, obj);
	chrome9_sf_resync_object_locked(engine, depth_obj);
	if (three_objects)
		chrome9_sf_resync_object_locked(engine, texture2_obj);
	if (four_objects)
		chrome9_sf_resync_object_locked(engine, texture3_obj);
	engine->last_cookie = job->cookie;
	chrome9_job_release_mappings_locked(engine, job);
	signal = true;
	goto out_unlock_submit;

out_prepublication:
	dma_fence_set_error(&job->fence.base, ret ?: -EIO);
	chrome9_job_release_mappings_locked(engine, job);
	signal = true;
out_unlock_submit:
	mutex_unlock(&engine->submit_lock);
	if (quarantine) {
		drm_exec_fini(&exec);
		if (signal)
			dma_fence_signal(&job->fence.base);
		ret = ret ?: -EIO;
		goto out_dev_exit;
	}
out_unmap:
	if (device_owned) {
		dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->sgt,
					 DMA_BIDIRECTIONAL);
		dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->depth_sgt,
					 DMA_BIDIRECTIONAL);
		if (three_objects)
			dma_sync_sgtable_for_cpu(&cdev->pdev->dev,
						 job->texture2_sgt,
						 DMA_BIDIRECTIONAL);
		if (four_objects)
			dma_sync_sgtable_for_cpu(&cdev->pdev->dev,
						 job->texture3_sgt,
						 DMA_BIDIRECTIONAL);
		chrome9_cache_flush(job->map.vaddr, request->object_size);
		chrome9_cache_flush(job->depth_map.vaddr, depth_obj->size);
		if (three_objects)
			chrome9_cache_flush(job->texture2_map.vaddr,
					    texture2_obj->size);
		if (four_objects)
			chrome9_cache_flush(job->texture3_map.vaddr,
					    texture3_obj->size);
		/* Preserve device ownership for any object currently scanned out. */
		mutex_lock(&engine->submit_lock);
		chrome9_sf_resync_object_locked(engine, obj);
		chrome9_sf_resync_object_locked(engine, depth_obj);
		if (three_objects)
			chrome9_sf_resync_object_locked(engine, texture2_obj);
		if (four_objects)
			chrome9_sf_resync_object_locked(engine, texture3_obj);
		mutex_unlock(&engine->submit_lock);
	}
	if (job->texture3_mapped) {
		drm_gem_shmem_vunmap_locked(job->texture3, &job->texture3_map);
		job->texture3_mapped = false;
	}
	if (job->texture2_mapped) {
		drm_gem_shmem_vunmap_locked(job->texture2, &job->texture2_map);
		job->texture2_mapped = false;
	}
	if (job->depth_mapped) {
		drm_gem_shmem_vunmap_locked(job->depth, &job->depth_map);
		job->depth_mapped = false;
	}
	if (job->mapped) {
		drm_gem_shmem_vunmap_locked(job->destination, &job->map);
		job->mapped = false;
	}
out_exec:
	if (exec_initialized)
		drm_exec_fini(&exec);
	if (signal)
		dma_fence_signal(&job->fence.base);
	job->destination = NULL;
	job->depth = NULL;
	job->texture2 = NULL;
	job->texture3 = NULL;
	if (texture3_obj)
		drm_gem_object_put(texture3_obj);
	if (texture2_obj)
		drm_gem_object_put(texture2_obj);
	drm_gem_object_put(depth_obj);
	drm_gem_object_put(obj);
	dma_fence_put(&job->fence.base);
	goto out_dev_exit;

out_free_job:
	kfree(job);
out_put_objects:
	if (texture3_obj)
		drm_gem_object_put(texture3_obj);
	if (texture2_obj)
		drm_gem_object_put(texture2_obj);
	if (depth_obj)
		drm_gem_object_put(depth_obj);
	if (obj)
		drm_gem_object_put(obj);
out_dev_exit:
	drm_dev_exit(dev_idx);
	return ret;
}

static bool chrome9_fragment_destination_size_valid(u64 actual, u64 required,
						 bool extended)
{
	if (!extended)
		return actual == required;

	return actual > required && IS_ALIGNED(actual, PAGE_SIZE);
}

static int chrome9_submit_fragment_program(struct drm_device *drm,
		struct drm_file *file, const struct chrome9_draw_request *request)
{
	struct chrome9_device *cdev = to_chrome9(drm);
	const struct chrome9_tcl_draw *tcl = request->tcl;
	struct drm_gem_object *objects[CHROME9_FRAGMENT_MAX_OBJECTS] = { };
	u64 object_sizes[CHROME9_FRAGMENT_MAX_OBJECTS] = { };
	u64 mapping_sizes[CHROME9_FRAGMENT_MAX_OBJECTS] = { };
	u32 bases[CHROME9_FRAGMENT_MAX_OBJECTS] = { };
	u32 texture_bases[DRM_CHROME9_FRAGMENT_MAX_TEXTURES] = { };
	u64 element_start[DRM_CHROME9_VERTEX_MAX_INPUTS] = { };
	u32 element_address[DRM_CHROME9_VERTEX_MAX_INPUTS] = { };
	u32 buffer_object[DRM_CHROME9_VERTEX_MAX_INPUTS] = { };
	/* Byte range the fetcher reads per vertex object; only it is flushed. */
	u64 object_low[CHROME9_FRAGMENT_MAX_OBJECTS];
	u64 object_high[CHROME9_FRAGMENT_MAX_OBJECTS] = { };
	u32 buffer_first;
	struct chrome9_engine *engine;
	struct chrome9_job *job = NULL;
	struct drm_exec exec;
	u64 mapped_size = 0;
	u32 object_count = 1;
	u32 depth_index = U32_MAX;
	u32 pause;
	long wait;
	bool exec_initialized = false;
	bool device_owned = false;
	bool signal = false;
	bool quarantine = false;
	bool must_quarantine;
	unsigned int i;
	int dev_idx;
	int ret;

	if (!drm_dev_enter(drm, &dev_idx))
		return -ENODEV;
	engine = READ_ONCE(cdev->engine);
	if (!engine || !chrome9_engine_ready(cdev)) {
		ret = -EIO;
		goto out_dev_exit;
	}
	objects[0] = drm_gem_object_lookup(file, request->destination_handle);
	object_sizes[0] = request->object_size;
	if (request->flags & DRM_CHROME9_FRAGMENT_DEPTH) {
		depth_index = object_count++;
		objects[depth_index] = drm_gem_object_lookup(file,
							      request->depth_handle);
		object_sizes[depth_index] = request->depth_object_size;
	}
	for (i = 0; i < request->fragment_texture_count; i++) {
		objects[object_count] = drm_gem_object_lookup(file,
			request->fragment_textures[i].handle);
		object_sizes[object_count] = request->fragment_texture_size[i];
		object_count++;
	}
	/* Stage B vertex buffers are plain, unguarded read-only objects;
	 * bindings of one handle share its object. */
	buffer_first = object_count;
	for (i = 0; tcl && i < tcl->buffer_count; i++) {
		unsigned int j;

		for (j = 0; j < i; j++)
			if (tcl->buffers[j].handle == tcl->buffers[i].handle)
				break;
		if (j < i) {
			buffer_object[i] = buffer_object[j];
			continue;
		}
		buffer_object[i] = object_count;
		objects[object_count] = drm_gem_object_lookup(file,
			tcl->buffers[i].handle);
		if (objects[object_count])
			object_sizes[object_count] = objects[object_count]->size;
		object_count++;
	}
	for (i = 0; i < object_count; i++) {
		unsigned int j;

		if (!objects[i]) {
			ret = -ENOENT;
			goto out_put_objects;
		}
		if (objects[i]->dev != drm || drm_gem_is_imported(objects[i]) ||
		    (i == 0 ?
		     !chrome9_fragment_destination_size_valid(
			objects[i]->size, object_sizes[i],
			request->flags &
			DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED) :
		     objects[i]->size != object_sizes[i])) {
			ret = -EINVAL;
			goto out_put_objects;
		}
		mapping_sizes[i] = objects[i]->size;
		/* Read-only textures may alias each other, never a render target. */
		for (j = 0; j < i; j++)
			if (objects[i] == objects[j] &&
			    (i == 0 || i == depth_index ||
			     j == 0 || j == depth_index)) {
				ret = -EINVAL;
				goto out_put_objects;
			}
		if (check_add_overflow(mapped_size, objects[i]->size,
				       &mapped_size)) {
			ret = -E2BIG;
			goto out_put_objects;
		}
	}
	if (mapped_size > SZ_64M - CHROME9_RING_SIZE) {
		ret = -E2BIG;
		goto out_put_objects;
	}
	if (tcl && tcl->buffer_count) {
		for (i = 0; i < ARRAY_SIZE(object_low); i++)
			object_low[i] = U64_MAX;
		ret = chrome9_tcl_buffer_bounds(tcl, objects, buffer_object,
				element_start, object_low, object_high);
		if (ret)
			goto out_put_objects;
	}

	job = kzalloc_obj(*job);
	if (!job) {
		ret = -ENOMEM;
		goto out_put_objects;
	}
	INIT_LIST_HEAD(&job->quarantine_link);
	for (i = 0; i < object_count; i++) {
		job->fragment_objects[i] = to_drm_gem_shmem_obj(objects[i]);
		job->fragment_sgts[i] =
			drm_gem_shmem_get_pages_sgt(job->fragment_objects[i]);
		if (IS_ERR(job->fragment_sgts[i])) {
			ret = PTR_ERR(job->fragment_sgts[i]);
			goto out_free_job;
		}
	}
	job->destination = job->fragment_objects[0];
	spin_lock_init(&job->fence.lock);
	dma_fence_init64(&job->fence.base, &chrome9_fence_ops,
			 &job->fence.lock, engine->fence_context,
			 atomic64_inc_return(&engine->fence_seqno));
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, object_count);
	exec_initialized = true;
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < object_count; i++) {
			ret = drm_exec_prepare_obj(&exec, objects[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	for (i = 0; i < object_count; i++) {
		bool write = i == 0 || i == depth_index;

		wait = dma_resv_wait_timeout(objects[i]->resv,
			dma_resv_usage_rw(write), true, CHROME9_DEP_TIMEOUT);
		if (wait <= 0) {
			ret = wait < 0 ? wait : -ETIMEDOUT;
			goto out_exec;
		}
		ret = drm_gem_shmem_vmap_locked(job->fragment_objects[i],
						 &job->fragment_maps[i]);
		if (ret)
			goto out_unmap;
		job->fragment_mapped[i] = true;
		if (job->fragment_maps[i].is_iomem) {
			ret = -EINVAL;
			goto out_unmap;
		}
		chrome9_bo_hold_kmap(to_chrome9_bo(objects[i]));
	}
	if (tcl && tcl->buffer_count &&
	    !chrome9_tcl_buffers_finite(tcl, job->fragment_maps, buffer_object,
					element_start)) {
		ret = -EINVAL;
		goto out_unmap;
	}
	chrome9_prof_mark(CHROME9_PROF_MAP);

	ret = mutex_lock_interruptible(&engine->submit_lock);
	if (ret)
		goto out_unmap;
	if (engine->stopping || engine->wedged || !engine->ready) {
		ret = -EIO;
		goto out_unlock_submit;
	}
	job->kind = request->kind;
	job->flags = request->flags;
	for (i = 0; i < object_count; i++) {
		ret = chrome9_job_acquire_mapping_locked(engine, job, objects[i],
				job->fragment_sgts[i], &bases[i]);
		if (ret)
			goto out_prepublication;
	}
	chrome9_prof_mark(CHROME9_PROF_GART);
	for (i = 0; i < object_count; i++) {
		if (i >= buffer_first) {
			chrome9_cache_flush((u8 *)job->fragment_maps[i].vaddr +
					    object_low[i],
					    object_high[i] - object_low[i]);
			dma_sync_sgtable_for_device(&cdev->pdev->dev,
				job->fragment_sgts[i], DMA_BIDIRECTIONAL);
			continue;
		}
		struct chrome9_bo *bo = to_chrome9_bo(objects[i]);
		ktime_t t0 = ktime_get(), t1;

		/* Verified guards without CPU writes since need no rewrite. */
		if (bo->explicit_access && !bo->cpu_dirty &&
		    bo->guard_object_size == object_sizes[i] &&
		    bo->guard_mapping_size == mapping_sizes[i]) {
			t1 = ktime_get();
			chrome9_prof_sub_ns[0] += ktime_to_ns(ktime_sub(t1, t0));
			dma_sync_sgtable_for_device(&cdev->pdev->dev,
				job->fragment_sgts[i], DMA_BIDIRECTIONAL);
			continue;
		}
		bo->guard_object_size = 0;
		chrome9_prepare_map_guards(&job->fragment_maps[i], object_sizes[i]);
		if (mapping_sizes[i] != object_sizes[i])
			chrome9_prepare_map_guards(&job->fragment_maps[i],
					    mapping_sizes[i]);
		if (bo->explicit_access && !bo->cpu_dirty) {
			chrome9_flush_map_guards(&job->fragment_maps[i],
						 object_sizes[i]);
			if (mapping_sizes[i] != object_sizes[i])
				chrome9_flush_map_guards(&job->fragment_maps[i],
							 mapping_sizes[i]);
			t1 = ktime_get();
			chrome9_prof_sub_ns[0] += ktime_to_ns(ktime_sub(t1, t0));
		} else {
			chrome9_cache_flush(job->fragment_maps[i].vaddr,
					    mapping_sizes[i]);
			bo->cpu_dirty = false;
			t1 = ktime_get();
			chrome9_prof_sub_ns[1] += ktime_to_ns(ktime_sub(t1, t0));
			chrome9_prof_full_bytes += mapping_sizes[i];
			chrome9_prof_full_kind[(bo->explicit_access ? 3 : 0) +
				(i == 0 ? 0 : i == depth_index ? 1 : 2)]++;
		}
		/* The job never writes a sampled object's pages. */
		if (bo->explicit_access && i != 0 && i != depth_index) {
			bo->guard_object_size = object_sizes[i];
			bo->guard_mapping_size = mapping_sizes[i];
		}
		dma_sync_sgtable_for_device(&cdev->pdev->dev,
			job->fragment_sgts[i], DMA_BIDIRECTIONAL);
		chrome9_prof_sub_ns[2] += ktime_to_ns(ktime_sub(ktime_get(), t1));
	}
	device_owned = true;
	for (i = 0; i < object_count; i++)
		dma_resv_add_fence(objects[i]->resv, &job->fence.base,
			(i == 0 || i == depth_index) ? DMA_RESV_USAGE_WRITE :
							  DMA_RESV_USAGE_READ);
	job->fence_attached = true;
	for (i = 0; i < request->fragment_texture_count; i++)
		texture_bases[i] = bases[buffer_first -
			request->fragment_texture_count + i];
	for (i = 0; tcl && tcl->buffer_count && i < tcl->element_count; i++)
		element_address[i] = bases[buffer_object[
			tcl->element_buffer[i]]] + element_start[i];
	job->cookie = engine->last_cookie == 0x00010001 ?
		      0x00020002 : 0x00010001;
	job->fence_id = 0;
	job->retirement_proven = false;
	chrome9_prof_mark(CHROME9_PROF_FLUSH);
	ret = chrome9_build_fragment_program(engine, request, job,
		bases[0] + PAGE_SIZE,
		depth_index == U32_MAX ? 0 : bases[depth_index] + PAGE_SIZE,
		texture_bases, tcl && tcl->buffer_count ? element_address : NULL,
		&pause);
	if (ret)
		goto out_prepublication;
	chrome9_prof_mark(CHROME9_PROF_BUILD);
	ret = chrome9_append_segment(engine, job, engine->run_staging_cpu,
		CHROME9_RUN_STAGING_SIZE, pause, &must_quarantine);
	chrome9_prof_mark(CHROME9_PROF_APPEND);
	if (ret) {
		if (!must_quarantine)
			goto out_prepublication;
		dma_fence_set_error(&job->fence.base, -ETIME);
		job->published = true;
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
		goto out_unlock_submit;
	}
	ret = chrome9_wait_job(engine, job);
	if (ret) {
		dma_fence_set_error(&job->fence.base, -ETIME);
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
		goto out_unlock_submit;
	}
	chrome9_prof_mark(CHROME9_PROF_GPU);
	job->retirement_proven = true;
	engine->last_cookie = job->cookie;
	for (i = 0; i < object_count; i++) {
		dma_sync_sgtable_for_cpu(&cdev->pdev->dev,
			job->fragment_sgts[i], DMA_BIDIRECTIONAL);
		struct chrome9_bo *bo = to_chrome9_bo(objects[i]);

		/* The job writes only its color and depth destinations. */
		if (i != 0 && i != depth_index)
			continue;
		if (!bo->explicit_access) {
			chrome9_cache_flush(job->fragment_maps[i].vaddr,
					    mapping_sizes[i]);
			continue;
		}
		/* Defer the invalidate to the next CPU access. */
		chrome9_flush_map_guards(&job->fragment_maps[i],
					 object_sizes[i]);
		if (mapping_sizes[i] != object_sizes[i])
			chrome9_flush_map_guards(&job->fragment_maps[i],
						 mapping_sizes[i]);
		bo->device_written = true;
	}
	dma_rmb();
	device_owned = false;
	for (i = 0; i < object_count; i++) {
		/* Sampled textures and vertex buffers are only read. */
		if (i != 0 && i != depth_index) {
			chrome9_sf_resync_object_locked(engine, objects[i]);
			continue;
		}
		if (!chrome9_verify_map_guards(&job->fragment_maps[i],
					       object_sizes[i]) ||
		    (mapping_sizes[i] != object_sizes[i] &&
		     !chrome9_verify_map_guards(&job->fragment_maps[i],
						mapping_sizes[i]))) {
			ret = -EIO;
			dma_fence_set_error(&job->fence.base, ret);
			to_chrome9_bo(objects[i])->guard_object_size = 0;
		} else if (to_chrome9_bo(objects[i])->explicit_access) {
			to_chrome9_bo(objects[i])->guard_object_size =
				object_sizes[i];
			to_chrome9_bo(objects[i])->guard_mapping_size =
				mapping_sizes[i];
		}
		chrome9_sf_resync_object_locked(engine, objects[i]);
	}
	chrome9_job_release_mappings_locked(engine, job);
	chrome9_prof_mark(CHROME9_PROF_VERIFY);
	signal = true;
	goto out_unlock_submit;

out_prepublication:
	dma_fence_set_error(&job->fence.base, ret ?: -EIO);
	chrome9_job_release_mappings_locked(engine, job);
	signal = true;
out_unlock_submit:
	mutex_unlock(&engine->submit_lock);
	if (quarantine) {
		drm_exec_fini(&exec);
		if (signal)
			dma_fence_signal(&job->fence.base);
		ret = ret ?: -EIO;
		goto out_dev_exit;
	}
out_unmap:
	if (device_owned) {
		for (i = 0; i < object_count; i++) {
			dma_sync_sgtable_for_cpu(&cdev->pdev->dev,
				job->fragment_sgts[i], DMA_BIDIRECTIONAL);
			chrome9_cache_flush(job->fragment_maps[i].vaddr,
					    mapping_sizes[i]);
		}
		mutex_lock(&engine->submit_lock);
		for (i = 0; i < object_count; i++)
			chrome9_sf_resync_object_locked(engine, objects[i]);
		mutex_unlock(&engine->submit_lock);
	}
	for (i = object_count; i-- > 0; )
		if (job->fragment_mapped[i]) {
			drm_gem_shmem_vunmap_locked(job->fragment_objects[i],
						    &job->fragment_maps[i]);
			job->fragment_mapped[i] = false;
		}
out_exec:
	if (exec_initialized)
		drm_exec_fini(&exec);
	if (signal)
		dma_fence_signal(&job->fence.base);
	job->destination = NULL;
	for (i = 0; i < object_count; i++) {
		job->fragment_objects[i] = NULL;
		drm_gem_object_put(objects[i]);
	}
	dma_fence_put(&job->fence.base);
	goto out_dev_exit;

out_free_job:
	kfree(job);
out_put_objects:
	for (i = 0; i < object_count; i++)
		if (objects[i])
			drm_gem_object_put(objects[i]);
out_dev_exit:
	drm_dev_exit(dev_idx);
	return ret;
}

int chrome9_fixed_quad_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct drm_chrome9_fixed_quad *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_FIXED,
		.destination_handle = args->destination_handle,
		.flags = args->flags,
		.object_size = args->flags == DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE ?
			       6U * PAGE_SIZE :
			       args->flags == DRM_CHROME9_FIXED_QUAD_VS_FETCH ?
			       4U * PAGE_SIZE : CHROME9_FIXED_BO_SIZE,
	};

	if ((args->flags != 0 &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_CONSTANT_RED &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE &&
	     hweight32(args->flags & CHROME9_FIXED_QUAD_VS_MASK) != 1) ||
	    (args->flags & ~CHROME9_FIXED_QUAD_VS_MASK &&
	     args->flags & CHROME9_FIXED_QUAD_VS_MASK) ||
	    args->pad[0] || args->pad[1])
		return -EINVAL;
	return chrome9_submit_draw(drm, file, &request);
}

static bool chrome9_coordinate_valid(s32 value, u32 limit)
{
	s64 minimum = -0x8000;
	s64 maximum = ((s64)limit << 16) - 0x8000;

	return value >= minimum && value <= maximum;
}

static bool chrome9_triangle_nondegenerate(s32 x0, s32 y0, s32 x1, s32 y1,
					   s32 x2, s32 y2)
{
	s64 dx10 = (s64)x1 - x0;
	s64 dy10 = (s64)y1 - y0;
	s64 dx20 = (s64)x2 - x0;
	s64 dy20 = (s64)y2 - y0;

	/* The recovered immediate-triangle path is not yet proven safe for
	 * zero-area primitives.  Reject them before publishing a command. */
	return dx10 * dy20 != dy10 * dx20;
}

static int
chrome9_validate_scissor(const struct drm_chrome9_scissor_state *scissor,
			 u32 width, u32 height)
{
	if (scissor->flags & ~DRM_CHROME9_SCISSOR_ENABLE)
		return -EINVAL;
	if (!(scissor->flags & DRM_CHROME9_SCISSOR_ENABLE))
		return scissor->min_x || scissor->max_x || scissor->min_y ||
		       scissor->max_y ? -EINVAL : 0;
	if (scissor->min_x > scissor->max_x || scissor->max_x > width ||
	    scissor->min_y > scissor->max_y || scissor->max_y > height)
		return -EINVAL;
	return 0;
}

static int chrome9_validate_blend_state(const struct drm_chrome9_blend_state *state)
{
	if (state->flags & ~(DRM_CHROME9_BLEND_ENABLE |
			     DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) ||
	    state->color_mask & ~(DRM_CHROME9_COLOR_MASK_R |
				  DRM_CHROME9_COLOR_MASK_G |
				  DRM_CHROME9_COLOR_MASK_B |
				  DRM_CHROME9_COLOR_MASK_A))
		return -EINVAL;
	if (state->flags & DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE) {
		if (state->alpha_test_func > DRM_CHROME9_COMPARE_ALWAYS ||
		    state->alpha_test_ref > 255)
			return -EINVAL;
	} else if (state->alpha_test_func || state->alpha_test_ref) {
		return -EINVAL;
	}
	if (state->rgb_src_factor >
		DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
	    state->rgb_dst_factor >
		DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
	    state->alpha_src_factor >
		DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
	    state->alpha_dst_factor >
		DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
	    state->rgb_func != DRM_CHROME9_BLEND_FUNC_ADD ||
	    state->alpha_func != DRM_CHROME9_BLEND_FUNC_ADD)
		return -EOPNOTSUPP;
	if (!(state->flags & DRM_CHROME9_BLEND_ENABLE) &&
	    (state->rgb_src_factor != DRM_CHROME9_BLEND_FACTOR_ONE ||
	     state->rgb_dst_factor != DRM_CHROME9_BLEND_FACTOR_ZERO ||
	     state->alpha_src_factor != DRM_CHROME9_BLEND_FACTOR_ONE ||
	     state->alpha_dst_factor != DRM_CHROME9_BLEND_FACTOR_ZERO))
		return -EINVAL;
	return 0;
}

int chrome9_draw_triangle_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_chrome9_draw_triangle *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TRIANGLE,
		.destination_handle = args->destination_handle,
		.width = args->width,
		.height = args->height,
		.pitch = args->pitch,
		.scissor = args->scissor,
	};
	u64 surface_size;
	unsigned int i;
	int ret;

	if (args->flags || args->pad || args->reserved ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->pitch != ALIGN(args->width * sizeof(u32), 32))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size) ||
	    request.object_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		if (args->vertices[i].pad ||
		    !chrome9_coordinate_valid(args->vertices[i].x_16_16,
					      args->width) ||
		    !chrome9_coordinate_valid(args->vertices[i].y_16_16,
					      args->height))
			return -EINVAL;
		request.vertices[i] = args->vertices[i];
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_draw(drm, file, &request);
}

int chrome9_draw_triangle_blend_ioctl(struct drm_device *drm, void *data,
				      struct drm_file *file)
{
	struct drm_chrome9_draw_triangle_blend *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TRIANGLE_BLEND,
		.destination_handle = args->destination_handle,
		.width = args->width,
		.height = args->height,
		.pitch = args->pitch,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
	};
	u64 surface_size;
	unsigned int i;
	int ret;

	if (args->flags || args->pad ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->pitch != ALIGN(args->width * sizeof(u32), 32))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	ret = chrome9_validate_blend_state(&args->blend);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size) ||
	    request.object_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		if (args->vertices[i].pad ||
		    !chrome9_coordinate_valid(args->vertices[i].x_16_16,
					      args->width) ||
		    !chrome9_coordinate_valid(args->vertices[i].y_16_16,
					      args->height))
			return -EINVAL;
		request.vertices[i] = args->vertices[i];
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_draw(drm, file, &request);
}

static int chrome9_validate_depth_flags(u32 flags, u32 allowed)
{
	u32 depth_func;

	if (flags & ~(allowed | DRM_CHROME9_DEPTH_FUNC_MASK |
		      DRM_CHROME9_DEPTH_READ_ONLY))
		return -EINVAL;
	depth_func = (flags & DRM_CHROME9_DEPTH_FUNC_MASK) >>
		     DRM_CHROME9_DEPTH_FUNC_SHIFT;
	if (!depth_func)
		depth_func = DRM_CHROME9_COMPARE_LESS;
	return depth_func == DRM_CHROME9_COMPARE_LESS ||
	       depth_func == DRM_CHROME9_COMPARE_EQUAL ||
	       depth_func == DRM_CHROME9_COMPARE_LEQUAL ||
	       depth_func == DRM_CHROME9_COMPARE_ALWAYS ? 0 : -EINVAL;
}

int chrome9_draw_triangle_d16_ioctl(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct drm_chrome9_draw_triangle_d16 *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16,
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.scissor = args->scissor,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (chrome9_validate_depth_flags(args->flags, 0) ||
	    args->pad || args->reserved ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->depth_pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.depth_object_size) ||
	    check_add_overflow(request.object_size, request.depth_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		if (!chrome9_coordinate_valid(args->vertices[i].x_16_16,
					      args->width) ||
		    !chrome9_coordinate_valid(args->vertices[i].y_16_16,
					      args->height) ||
		    args->vertices[i].z_16_16 < 0 ||
		    args->vertices[i].z_16_16 > 0x10000)
			return -EINVAL;
		request.depth_vertices[i] = args->vertices[i];
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_triangle_d16_blend_ioctl(struct drm_device *drm, void *data,
					  struct drm_file *file)
{
	struct drm_chrome9_draw_triangle_d16_blend *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_BLEND,
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (chrome9_validate_depth_flags(args->flags, 0) ||
	    args->pad || args->reserved[0] ||
	    args->reserved[1] ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	ret = chrome9_validate_blend_state(&args->blend);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->depth_pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.depth_object_size) ||
	    check_add_overflow(request.object_size, request.depth_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		if (!chrome9_coordinate_valid(args->vertices[i].x_16_16,
					      args->width) ||
		    !chrome9_coordinate_valid(args->vertices[i].y_16_16,
					      args->height) ||
		    args->vertices[i].z_16_16 < 0 ||
		    args->vertices[i].z_16_16 > 0x10000)
			return -EINVAL;
		request.depth_vertices[i] = args->vertices[i];
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

static bool chrome9_rhw_valid(u32 value);

static bool
chrome9_ps_source_valid(const struct drm_chrome9_ps_source *source)
{
	bool identity = true;
	unsigned int i;

	if (source->file > DRM_CHROME9_PS_SOURCE_CONSTANT || source->index ||
	    source->reserved || source->modifier)
		return false;
	for (i = 0; i < ARRAY_SIZE(source->swizzle); i++)
		identity &= source->swizzle[i] == i;
	return identity;
}

static u32 chrome9_ps_source_file(const struct drm_chrome9_ps_source *source)
{
	return source->file == DRM_CHROME9_PS_SOURCE_VARYING ? 1 : 2;
}

static u32
chrome9_ps_source_modifier(const struct drm_chrome9_ps_source *source)
{
	return source->modifier ? 8 : 0;
}

static void
chrome9_encode_ps_source_a(u32 record[4],
			   const struct drm_chrome9_ps_source *source)
{
	record[1] &= 0x00ffffff;
	record[2] &= ~0x0003ffff;
	record[1] |= chrome9_ps_source_modifier(source) << 24 |
		     (u32)source->swizzle[3] << 28 |
		     (u32)(source->swizzle[2] & 1) << 31;
	record[2] |= (u32)source->swizzle[0] << 5 |
		     (u32)source->swizzle[1] << 2 |
		     (u32)source->swizzle[2] >> 1 |
		     chrome9_ps_source_file(source) << 14;
}

static void
chrome9_encode_ps_source_c(u32 record[4],
			   const struct drm_chrome9_ps_source *source)
{
	record[0] &= 0x0fffffff;
	record[1] &= ~0x003fffff;
	record[3] &= ~0x00100000;
	record[0] |= chrome9_ps_source_modifier(source) << 28;
	record[1] |= (u32)source->swizzle[0] << 9 |
		     (u32)source->swizzle[1] << 6 |
		     (u32)source->swizzle[2] << 3 |
		     (u32)source->swizzle[3] |
		     chrome9_ps_source_file(source) << 18;
	if (source->modifier)
		record[3] |= 0x00100000;
}

static void
chrome9_encode_ps_source_b(u32 record[4],
			   const struct drm_chrome9_ps_source *source)
{
	record[0] &= ~0x03ffffff;
	record[0] |= chrome9_ps_source_modifier(source) |
		     (u32)source->swizzle[0] << 13 |
		     (u32)source->swizzle[1] << 10 |
		     (u32)source->swizzle[2] << 7 |
		     (u32)source->swizzle[3] << 4 |
		     chrome9_ps_source_file(source) << 22;
}

static int chrome9_encode_ps_instruction(
		const struct drm_chrome9_ps_instruction *instruction,
		const u32 constant_c0[4], u32 record[4])
{
	static const u32 mov[4] = {
		0x01c00530, 0x301c0053, 0x00f08005, 0x000d0820,
	};
	static const u32 mul[4] = {
		0x0140b6d0, 0x30081053, 0x00f08005, 0x00000820,
	};
	static const u32 add[4] = {
		0x00810530, 0x30140924, 0x00f08005, 0x00000820,
	};
	bool constant;
	unsigned int i;

	if (instruction->reserved ||
	    !chrome9_ps_source_valid(&instruction->src[0]))
		return -EINVAL;
	switch (instruction->op) {
	case DRM_CHROME9_PS_OP_MOV:
		if (memchr_inv(&instruction->src[1], 0,
			       sizeof(instruction->src[1])))
			return -EINVAL;
		memcpy(record, mov, sizeof(mov));
		chrome9_encode_ps_source_a(record, &instruction->src[0]);
		constant = instruction->src[0].file ==
			DRM_CHROME9_PS_SOURCE_CONSTANT;
		break;
	case DRM_CHROME9_PS_OP_MUL:
		if (!chrome9_ps_source_valid(&instruction->src[1]))
			return -EINVAL;
		/* Version one closes only the independently compiled v0*c0 form. */
		if (instruction->src[0].file == instruction->src[1].file)
			return -EINVAL;
		memcpy(record, mul, sizeof(mul));
		chrome9_encode_ps_source_a(record, &instruction->src[0]);
		chrome9_encode_ps_source_c(record, &instruction->src[1]);
		constant = instruction->src[0].file ==
			DRM_CHROME9_PS_SOURCE_CONSTANT ||
			instruction->src[1].file ==
			DRM_CHROME9_PS_SOURCE_CONSTANT;
		break;
	case DRM_CHROME9_PS_OP_ADD:
		if (!chrome9_ps_add_enabled() ||
		    !chrome9_ps_source_valid(&instruction->src[1]))
			return -EINVAL;
		/* The first live gate admits only v0+c0 in either source order. */
		if (instruction->src[0].file == instruction->src[1].file)
			return -EINVAL;
		memcpy(record, add, sizeof(add));
		chrome9_encode_ps_source_a(record, &instruction->src[0]);
		chrome9_encode_ps_source_b(record, &instruction->src[1]);
		constant = true;
		break;
	default:
		return -EINVAL;
	}
	if (!constant)
		return memchr_inv(constant_c0, 0, 4 * sizeof(*constant_c0)) ?
			-EINVAL : 0;
	for (i = 0; i < 4; i++)
		if (!chrome9_float_finite(constant_c0[i]))
			return -EINVAL;
	return 0;
}

static int chrome9_draw_triangle_batch(struct drm_device *drm, void *data,
				       struct drm_file *file, bool perspective,
				       u32 max_triangles,
				       enum chrome9_draw_kind perspective_kind,
				       const u32 ps_program[4],
				       const u32 ps_constant_c0[4])
{
	struct drm_chrome9_draw_triangle_batch *args = data;
	void *vertices;
	struct chrome9_draw_request request = {
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
		.triangle_count = args->triangle_count,
	};
	u64 surface_size;
	u64 total_size;
	size_t vertex_size = perspective ?
		sizeof(struct drm_chrome9_vertex_d16_rhw) :
		sizeof(struct drm_chrome9_vertex_d16);
	u32 vertex_count;
	bool depth = args->flags & DRM_CHROME9_TRIANGLE_BATCH_DEPTH;
	bool blend = args->flags & DRM_CHROME9_TRIANGLE_BATCH_BLEND;
	u32 state_flags = DRM_CHROME9_TRIANGLE_BATCH_DEPTH |
			  DRM_CHROME9_TRIANGLE_BATCH_BLEND;
	unsigned int i;
	int ret;

	if (chrome9_validate_depth_flags(args->flags, state_flags) ||
	    (perspective && !depth) ||
	    (!depth && (args->flags & (DRM_CHROME9_DEPTH_FUNC_MASK |
				      DRM_CHROME9_DEPTH_READ_ONLY))) ||
	    !args->triangle_count ||
	    args->triangle_count > max_triangles ||
	    !args->vertices_ptr || args->reserved0 || args->pad ||
	    args->reserved[0] || args->reserved[1] || args->reserved[2] ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    (!blend && (args->destination_format ||
			  memchr_inv(&args->blend, 0, sizeof(args->blend)))) ||
	    (depth && (!args->depth_handle ||
		args->depth_pitch != ALIGN(args->width * sizeof(u16), 32))) ||
	    (!depth && (args->depth_handle || args->depth_pitch)))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (blend) {
		ret = chrome9_validate_blend_state(&args->blend);
		if (ret)
			return ret;
	}
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (depth) {
		if (check_mul_overflow((u64)args->depth_pitch,
				       (u64)args->height, &surface_size) ||
		    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
				       2ULL * PAGE_SIZE,
				       &request.depth_object_size) ||
		    check_add_overflow(request.object_size,
				       request.depth_object_size, &total_size) ||
		    total_size > SZ_64M - CHROME9_RING_SIZE)
			return -E2BIG;
	} else if (request.object_size > SZ_64M - CHROME9_RING_SIZE) {
		return -E2BIG;
	}
	if (check_mul_overflow(args->triangle_count, 3U, &vertex_count))
		return -E2BIG;
	if (max_triangles > DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES)
		vertices = vmemdup_array_user(
			u64_to_user_ptr(args->vertices_ptr), vertex_count,
			vertex_size);
	else
		vertices = memdup_array_user(u64_to_user_ptr(args->vertices_ptr),
					      vertex_count, vertex_size);
	if (IS_ERR(vertices))
		return PTR_ERR(vertices);
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_vertex_d16 *vertex = vertices;
		const struct drm_chrome9_vertex_d16_rhw *vertex_rhw = vertices;
		s32 x = perspective ? vertex_rhw[i].x_16_16 : vertex[i].x_16_16;
		s32 y = perspective ? vertex_rhw[i].y_16_16 : vertex[i].y_16_16;
		s32 z = perspective ? vertex_rhw[i].z_16_16 : vertex[i].z_16_16;

		if (!chrome9_coordinate_valid(x, args->width) ||
		    !chrome9_coordinate_valid(y, args->height) ||
		    (depth && (z < 0 || z > 0x10000)) ||
		    (perspective && !chrome9_rhw_valid(vertex_rhw[i].rhw_float))) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	for (i = 0; i < vertex_count; i += 3) {
		const struct drm_chrome9_vertex_d16 *vertex = vertices;
		const struct drm_chrome9_vertex_d16_rhw *vertex_rhw = vertices;
		s32 x0 = perspective ? vertex_rhw[i].x_16_16 : vertex[i].x_16_16;
		s32 y0 = perspective ? vertex_rhw[i].y_16_16 : vertex[i].y_16_16;
		s32 x1 = perspective ? vertex_rhw[i + 1].x_16_16 :
							 vertex[i + 1].x_16_16;
		s32 y1 = perspective ? vertex_rhw[i + 1].y_16_16 :
							 vertex[i + 1].y_16_16;
		s32 x2 = perspective ? vertex_rhw[i + 2].x_16_16 :
							 vertex[i + 2].x_16_16;
		s32 y2 = perspective ? vertex_rhw[i + 2].y_16_16 :
							 vertex[i + 2].y_16_16;

		if (!chrome9_triangle_nondegenerate(
			x0, y0, x1, y1, x2, y2)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	if (ps_program) {
		request.batch_vertices_rhw = vertices;
		request.kind = CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH;
		memcpy(request.ps_program, ps_program,
		       sizeof(request.ps_program));
		memcpy(request.ps_constant_c0, ps_constant_c0,
		       sizeof(request.ps_constant_c0));
	} else if (perspective) {
		request.batch_vertices_rhw = vertices;
		request.kind = perspective_kind;
	} else {
		request.batch_vertices = vertices;
		request.kind = depth ? CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_BATCH :
				       CHROME9_DRAW_IMMEDIATE_TRIANGLE_BATCH;
	}
	ret = depth ? chrome9_submit_depth_triangle(drm, file, &request) :
		      chrome9_submit_draw(drm, file, &request);

out_free:
	kvfree(vertices);
	return ret;
}

int chrome9_draw_triangle_batch_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	return chrome9_draw_triangle_batch(
		drm, data, file, false,
		DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES,
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_BATCH, NULL, NULL);
}

int chrome9_draw_triangle_d16_rhw_batch_ioctl(struct drm_device *drm,
					       void *data,
					       struct drm_file *file)
{
	return chrome9_draw_triangle_batch(
		drm, data, file, true,
		DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES,
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_BATCH, NULL, NULL);
}

int chrome9_draw_triangle_d16_rhw_run_ioctl(struct drm_device *drm,
					    void *data,
					    struct drm_file *file)
{
	struct drm_chrome9_draw_triangle_d16_rhw_run *args = data;
	struct drm_chrome9_draw_triangle_batch batch = {
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.triangle_count = args->triangle_count,
		.width = args->width,
		.height = args->height,
		.destination_pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.destination_format = args->destination_format,
		.reserved0 = args->reserved0,
		.blend = args->blend,
		.scissor = args->scissor,
		.pad = args->pad,
		.vertices_ptr = args->vertices_ptr,
		.reserved = {
			args->reserved[0], args->reserved[1], args->reserved[2],
		},
	};

	return chrome9_draw_triangle_batch(
		drm, &batch, file, true,
		DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES,
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN, NULL, NULL);
}

int chrome9_draw_triangle_d16_rhw_chain_ioctl(struct drm_device *drm,
					       void *data,
					       struct drm_file *file)
{
	struct drm_chrome9_draw_triangle_d16_rhw_chain *args = data;
	struct drm_chrome9_draw_triangle_batch batch = {
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.triangle_count = args->triangle_count,
		.width = args->width,
		.height = args->height,
		.destination_pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.destination_format = args->destination_format,
		.reserved0 = args->reserved0,
		.blend = args->blend,
		.scissor = args->scissor,
		.pad = args->pad,
		.vertices_ptr = args->vertices_ptr,
		.reserved = {
			args->reserved[0], args->reserved[1], args->reserved[2],
		},
	};

	if (!chrome9_rhw_chain_enabled())
		return -EOPNOTSUPP;
	return chrome9_draw_triangle_batch(
		drm, &batch, file, true,
		DRM_CHROME9_TRIANGLE_CHAIN_MAX_TRIANGLES,
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_CHAIN, NULL, NULL);
}

int chrome9_draw_program_triangle_d16_rhw_batch_ioctl(
		struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_chrome9_draw_program_triangle_d16_rhw_batch *args = data;
	struct drm_chrome9_draw_triangle_batch batch = {
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.triangle_count = args->triangle_count,
		.width = args->width,
		.height = args->height,
		.destination_pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.destination_format = args->destination_format,
		.reserved0 = args->reserved0,
		.blend = args->blend,
		.scissor = args->scissor,
		.pad = args->pad,
		.vertices_ptr = args->vertices_ptr,
	};
	u32 program[4];
	int ret;

	ret = chrome9_encode_ps_instruction(&args->instruction,
					    args->constant_c0, program);
	if (ret)
		return ret;
	return chrome9_draw_triangle_batch(
		drm, &batch, file, true,
		DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES,
		CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_BATCH, program,
		args->constant_c0);
}

static bool chrome9_float_finite(u32 value)
{
	return (value & GENMASK(30, 23)) != GENMASK(30, 23);
}

static bool chrome9_rhw_valid(u32 value)
{
	return !(value & BIT(31)) && (value & GENMASK(30, 23)) &&
	       chrome9_float_finite(value);
}

static bool chrome9_safe_texcoord(u32 value, u32 wrap)
{
	u32 magnitude = value & ~BIT(31);

	if (!chrome9_float_finite(value))
		return false;
	if (chrome9_two_texture_signed_repeat &&
	    wrap == DRM_CHROME9_TEXTURE_WRAP_REPEAT)
		return magnitude <= 0x42000000; /* 32.0f */
	return (!(value & BIT(31)) || !magnitude) && magnitude <= 0x3f800000;
}

static int chrome9_validate_two_texture_state(
		const struct drm_chrome9_texture_state *texture, u64 *object_size)
{
	u64 data_size;

	if (!texture->handle || !texture->width || texture->width > 2048 ||
	    !texture->height || texture->height > 2048 ||
	    texture->pitch != ALIGN(texture->width * sizeof(u32), 32) ||
	    texture->format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ||
	    texture->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    texture->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    (texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT))
		return -EINVAL;
	if (check_mul_overflow((u64)texture->pitch, (u64)texture->height,
			       &data_size) ||
	    check_add_overflow(ALIGN(data_size, PAGE_SIZE), 2ULL * PAGE_SIZE,
			       object_size))
		return -E2BIG;
	return 0;
}

static int chrome9_validate_mip_texture_state(
		const struct drm_chrome9_texture_state *texture,
		u32 last_level, u32 mip_filter, s32 lod_bias_s5_5,
		struct chrome9_draw_request *request)
{
	u64 cursor;
	u64 level_size;
	u64 offset;
	u64 l0_object_size;
	u64 delta;
	u32 width;
	u32 height;
	u32 pitch;
	unsigned int level;
	int ret;

	ret = chrome9_validate_two_texture_state(texture, &l0_object_size);
	if (ret)
		return ret;
	if (texture->format != DRM_CHROME9_TEXTURE_FORMAT_BGRX8888 ||
	    texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE ||
	    texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE ||
	    texture->width != texture->height ||
	    !is_power_of_2(texture->width) || texture->width > 512 ||
	    !last_level || last_level > 9 ||
	    last_level > ilog2(texture->width) ||
	    (last_level == 9 && texture->width != 512) ||
	    mip_filter > DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR ||
	    lod_bias_s5_5 < -512 || lod_bias_s5_5 > 511)
		return -EINVAL;

	request->texture_level_offsets[0] = PAGE_SIZE;
	request->depth_object_size = l0_object_size;
	cursor = l0_object_size;
	for (level = 1; level <= last_level; level++) {
		if (check_add_overflow(cursor, 255ULL, &offset))
			return -E2BIG;
		offset &= ~255ULL;
		if (offset > U32_MAX)
			return -E2BIG;
		request->texture_level_offsets[level] = offset;

		width = max(texture->width >> level, 1U);
		height = max(texture->height >> level, 1U);
		pitch = ALIGN(width * sizeof(u32), 32);
		if (check_mul_overflow((u64)pitch, (u64)height, &level_size) ||
		    check_add_overflow(offset, level_size, &cursor))
			return -E2BIG;

		delta = offset - PAGE_SIZE;
		if ((delta & 0xff) || (delta >> 8) > 0x00ffffff)
			return -EINVAL;
	}
	if (last_level == 9 &&
	    request->texture_level_offsets[9] -
	    request->texture_level_offsets[8] != 0x100)
		return -EINVAL;
	if (check_add_overflow(cursor, (u64)PAGE_SIZE - 1, &cursor))
		return -E2BIG;
	cursor &= ~((u64)PAGE_SIZE - 1);
	if (check_add_overflow(cursor, (u64)PAGE_SIZE, &cursor))
		return -E2BIG;

	request->texture_object_size = cursor;
	request->texture_last_level = last_level;
	request->mip_filter = mip_filter;
	request->lod_bias_s5_5 = lod_bias_s5_5;
	return 0;
}

int chrome9_draw_two_textured_triangle_ioctl(struct drm_device *drm,
					      void *data,
					      struct drm_file *file)
{
	struct drm_chrome9_draw_two_textured_triangle *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE,
		.destination_handle = args->destination_handle,
		.depth_handle = args->textures[0].handle,
		.texture_handle = args->textures[0].handle,
		.texture2_handle = args->textures[1].handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.texture_width = args->textures[0].width,
		.texture_height = args->textures[0].height,
		.texture_pitch = args->textures[0].pitch,
		.texture_format = args->textures[0].format,
		.min_filter = args->textures[0].min_filter,
		.mag_filter = args->textures[0].mag_filter,
		.wrap_s = args->textures[0].wrap_s,
		.wrap_t = args->textures[0].wrap_t,
		.texture2_width = args->textures[1].width,
		.texture2_height = args->textures[1].height,
		.texture2_pitch = args->textures[1].pitch,
		.texture2_format = args->textures[1].format,
		.texture2_min_filter = args->textures[1].min_filter,
		.texture2_mag_filter = args->textures[1].mag_filter,
		.texture2_wrap_s = args->textures[1].wrap_s,
		.texture2_wrap_t = args->textures[1].wrap_t,
		.scissor = args->scissor,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if ((args->flags &
	     ~DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY) ||
	    args->reserved0 || args->reserved[0] ||
	    args->reserved[1] || args->reserved[2] ||
	    !args->destination_handle ||
	    args->destination_handle == args->textures[0].handle ||
	    args->destination_handle == args->textures[1].handle ||
	    args->textures[0].handle == args->textures[1].handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	ret = chrome9_validate_two_texture_state(&args->textures[0],
						 &request.texture_object_size);
	if (ret)
		return ret;
	request.depth_object_size = request.texture_object_size;
	ret = chrome9_validate_two_texture_state(&args->textures[1],
						 &request.texture2_object_size);
	if (ret)
		return ret;
	if (check_add_overflow(request.object_size,
			       request.texture_object_size, &total_size) ||
	    check_add_overflow(total_size, request.texture2_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_two_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_safe_texcoord(vertex->s0_float,
					  args->textures[0].wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t0_float,
					  args->textures[0].wrap_t) ||
		    !chrome9_safe_texcoord(vertex->s1_float,
					  args->textures[1].wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t1_float,
					  args->textures[1].wrap_t))
			return -EINVAL;
		request.two_textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_two_textured_triangle_d16_ioctl(struct drm_device *drm,
						 void *data,
						 struct drm_file *file)
{
	struct drm_chrome9_draw_two_textured_triangle_d16 *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16,
		.destination_handle = args->destination_handle,
		.depth_handle = args->textures[0].handle,
		.texture_handle = args->textures[0].handle,
		.texture2_handle = args->textures[1].handle,
		.texture3_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.texture_width = args->textures[0].width,
		.texture_height = args->textures[0].height,
		.texture_pitch = args->textures[0].pitch,
		.texture_format = args->textures[0].format,
		.min_filter = args->textures[0].min_filter,
		.mag_filter = args->textures[0].mag_filter,
		.wrap_s = args->textures[0].wrap_s,
		.wrap_t = args->textures[0].wrap_t,
		.texture2_width = args->textures[1].width,
		.texture2_height = args->textures[1].height,
		.texture2_pitch = args->textures[1].pitch,
		.texture2_format = args->textures[1].format,
		.texture2_min_filter = args->textures[1].min_filter,
		.texture2_mag_filter = args->textures[1].mag_filter,
		.texture2_wrap_s = args->textures[1].wrap_s,
		.texture2_wrap_t = args->textures[1].wrap_t,
		.scissor = args->scissor,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (!(args->flags & DRM_CHROME9_DEPTH_FUNC_MASK) ||
	    chrome9_validate_depth_flags(
		args->flags, DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY) ||
	    args->reserved0 ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle || !args->depth_handle ||
	    !args->textures[0].handle || !args->textures[1].handle ||
	    args->destination_handle == args->depth_handle ||
	    args->destination_handle == args->textures[0].handle ||
	    args->destination_handle == args->textures[1].handle ||
	    args->depth_handle == args->textures[0].handle ||
	    args->depth_handle == args->textures[1].handle ||
	    args->textures[0].handle == args->textures[1].handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	ret = chrome9_validate_two_texture_state(&args->textures[0],
						 &request.texture_object_size);
	if (ret)
		return ret;
	request.depth_object_size = request.texture_object_size;
	ret = chrome9_validate_two_texture_state(&args->textures[1],
						 &request.texture2_object_size);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->depth_pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE,
			       &request.texture3_object_size) ||
	    check_add_overflow(request.object_size,
			       request.texture_object_size, &total_size) ||
	    check_add_overflow(total_size, request.texture2_object_size,
			       &total_size) ||
	    check_add_overflow(total_size, request.texture3_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_two_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_safe_texcoord(vertex->s0_float,
					  args->textures[0].wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t0_float,
					  args->textures[0].wrap_t) ||
		    !chrome9_safe_texcoord(vertex->s1_float,
					  args->textures[1].wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t1_float,
					  args->textures[1].wrap_t))
			return -EINVAL;
		request.two_textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(args->vertices[0].x_16_16,
					    args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_two_textured_triangle_d16_blend_ioctl(
		struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_chrome9_draw_two_textured_triangle_d16_blend *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TWO_TEXTURED_TRIANGLE_D16_BLEND,
		.destination_handle = args->destination_handle,
		.depth_handle = args->textures[0].handle,
		.texture_handle = args->textures[0].handle,
		.texture2_handle = args->textures[1].handle,
		.texture3_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.texture_width = args->textures[0].width,
		.texture_height = args->textures[0].height,
		.texture_pitch = args->textures[0].pitch,
		.texture_format = args->textures[0].format,
		.min_filter = args->textures[0].min_filter,
		.mag_filter = args->textures[0].mag_filter,
		.wrap_s = args->textures[0].wrap_s,
		.wrap_t = args->textures[0].wrap_t,
		.texture2_width = args->textures[1].width,
		.texture2_height = args->textures[1].height,
		.texture2_pitch = args->textures[1].pitch,
		.texture2_format = args->textures[1].format,
		.texture2_min_filter = args->textures[1].min_filter,
		.texture2_mag_filter = args->textures[1].mag_filter,
		.texture2_wrap_s = args->textures[1].wrap_s,
		.texture2_wrap_t = args->textures[1].wrap_t,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (!(args->flags & DRM_CHROME9_DEPTH_FUNC_MASK) ||
	    chrome9_validate_depth_flags(
		args->flags, DRM_CHROME9_TWO_TEXTURE_TEX0_ALPHA_PRIMARY) ||
	    args->reserved0 ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle || !args->depth_handle ||
	    !args->textures[0].handle || !args->textures[1].handle ||
	    args->destination_handle == args->depth_handle ||
	    args->destination_handle == args->textures[0].handle ||
	    args->destination_handle == args->textures[1].handle ||
	    args->depth_handle == args->textures[0].handle ||
	    args->depth_handle == args->textures[1].handle ||
	    args->textures[0].handle == args->textures[1].handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32) ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888)
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	ret = chrome9_validate_blend_state(&args->blend);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	ret = chrome9_validate_two_texture_state(&args->textures[0],
						 &request.texture_object_size);
	if (ret)
		return ret;
	request.depth_object_size = request.texture_object_size;
	ret = chrome9_validate_two_texture_state(&args->textures[1],
						 &request.texture2_object_size);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->depth_pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE,
			       &request.texture3_object_size) ||
	    check_add_overflow(request.object_size,
			       request.texture_object_size, &total_size) ||
	    check_add_overflow(total_size, request.texture2_object_size,
			       &total_size) ||
	    check_add_overflow(total_size, request.texture3_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_two_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_safe_texcoord(vertex->s0_float,
					  args->textures[0].wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t0_float,
					  args->textures[0].wrap_t) ||
		    !chrome9_safe_texcoord(vertex->s1_float,
					  args->textures[1].wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t1_float,
					  args->textures[1].wrap_t))
			return -EINVAL;
		request.two_textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(args->vertices[0].x_16_16,
					    args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_textured_triangle_ioctl(struct drm_device *drm, void *data,
					 struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture_handle,
		.texture_handle = args->texture_handle,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.texture_width = args->texture_width,
		.texture_height = args->texture_height,
		.texture_pitch = args->texture_pitch,
		.texture_format = args->texture_format,
		.min_filter = args->min_filter,
		.mag_filter = args->mag_filter,
		.wrap_s = args->wrap_s,
		.wrap_t = args->wrap_t,
		.texture_program = args->program,
		.scissor = args->scissor,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (args->flags || args->pad || args->reserved ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    !args->texture_width || args->texture_width > 2048 ||
	    !args->texture_height || args->texture_height > 2048 ||
	    args->texture_pitch !=
		ALIGN(args->texture_width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->texture_format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ||
	    args->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    args->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    (args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->texture_pitch,
			       (u64)args->texture_height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture_object_size) ||
	    check_add_overflow(request.object_size, request.texture_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	request.depth_object_size = request.texture_object_size;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_float_finite(vertex->s_float) ||
		    !chrome9_float_finite(vertex->t_float))
			return -EINVAL;
		request.textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_textured_triangle_batch_ioctl(struct drm_device *drm,
						void *data,
						struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle_batch *args = data;
	struct drm_chrome9_textured_vertex *vertices;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BATCH,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture_handle,
		.texture_handle = args->texture_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.texture_width = args->texture_width,
		.texture_height = args->texture_height,
		.texture_pitch = args->texture_pitch,
		.texture_format = args->texture_format,
		.min_filter = args->min_filter,
		.mag_filter = args->mag_filter,
		.wrap_s = args->wrap_s,
		.wrap_t = args->wrap_t,
		.texture_program = args->program,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
		.triangle_count = args->triangle_count,
	};
	u64 surface_size;
	u64 total_size;
	u32 vertex_count;
	bool blend = args->flags & DRM_CHROME9_TRIANGLE_BATCH_BLEND;
	unsigned int i;
	int ret;

	if (args->flags & ~DRM_CHROME9_TRIANGLE_BATCH_BLEND ||
	    !args->triangle_count ||
	    args->triangle_count >
		DRM_CHROME9_TEXTURED_TRIANGLE_BATCH_MAX_TRIANGLES ||
	    !args->vertices_ptr || args->reserved0 || args->pad ||
	    args->reserved[0] || args->reserved[1] || args->reserved[2] ||
	    !args->destination_handle || !args->texture_handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    !args->texture_width || args->texture_width > 2048 ||
	    !args->texture_height || args->texture_height > 2048 ||
	    args->texture_pitch !=
		ALIGN(args->texture_width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    args->texture_format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ||
	    args->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    args->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    (args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (!blend && (args->destination_format ||
			  memchr_inv(&args->blend, 0, sizeof(args->blend)))))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (blend) {
		ret = chrome9_validate_blend_state(&args->blend);
		if (ret)
			return ret;
	}
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->texture_pitch,
			       (u64)args->texture_height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture_object_size) ||
	    check_add_overflow(request.object_size, request.texture_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	request.depth_object_size = request.texture_object_size;
	if (check_mul_overflow(args->triangle_count, 3U, &vertex_count))
		return -E2BIG;
	vertices = memdup_array_user(u64_to_user_ptr(args->vertices_ptr),
				     vertex_count, sizeof(*vertices));
	if (IS_ERR(vertices))
		return PTR_ERR(vertices);
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_textured_vertex *vertex = &vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_float_finite(vertex->s_float) ||
		    !chrome9_float_finite(vertex->t_float)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	for (i = 0; i < vertex_count; i += 3) {
		if (!chrome9_triangle_nondegenerate(
			vertices[i].x_16_16, vertices[i].y_16_16,
			vertices[i + 1].x_16_16, vertices[i + 1].y_16_16,
			vertices[i + 2].x_16_16, vertices[i + 2].y_16_16)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	request.textured_batch_vertices = vertices;
	ret = chrome9_submit_depth_triangle(drm, file, &request);

out_free:
	kfree(vertices);
	return ret;
}

int chrome9_draw_textured_triangle_run_ioctl(struct drm_device *drm,
					     void *data,
					     struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle_run *args = data;
	struct drm_chrome9_textured_vertex *vertices;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_RUN,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture.handle,
		.texture_handle = args->texture.handle,
		.texture2_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.texture_width = args->texture.width,
		.texture_height = args->texture.height,
		.texture_pitch = args->texture.pitch,
		.texture_format = args->texture.format,
		.min_filter = args->texture.min_filter,
		.mag_filter = args->texture.mag_filter,
		.wrap_s = args->texture.wrap_s,
		.wrap_t = args->texture.wrap_t,
		.texture_program = args->program,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
		.triangle_count = args->triangle_count,
	};
	u32 state_flags = DRM_CHROME9_TEXTURE_RUN_DEPTH |
			  DRM_CHROME9_TEXTURE_RUN_BLEND;
	bool depth = args->flags & DRM_CHROME9_TEXTURE_RUN_DEPTH;
	bool blend = args->flags & DRM_CHROME9_TEXTURE_RUN_BLEND;
	size_t vertex_bytes;
	u64 surface_size;
	u64 total_size;
	u32 vertex_count;
	unsigned int i;
	int ret;

	if (!chrome9_textured_triangle_run_enabled())
		return -EOPNOTSUPP;
	if (chrome9_validate_depth_flags(args->flags, state_flags) ||
	    (depth && !(args->flags & DRM_CHROME9_DEPTH_FUNC_MASK)) ||
	    (!depth && (args->flags & (DRM_CHROME9_DEPTH_FUNC_MASK |
					 DRM_CHROME9_DEPTH_READ_ONLY))) ||
	    !args->triangle_count ||
	    args->triangle_count > DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES ||
	    !args->vertices_ptr ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle ||
	    args->destination_handle == args->texture.handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    (!blend && (args->destination_format ||
			  memchr_inv(&args->blend, 0, sizeof(args->blend)))) ||
	    (depth && (!args->depth_handle ||
		       args->depth_handle == args->destination_handle ||
		       args->depth_handle == args->texture.handle ||
		       args->depth_pitch !=
			ALIGN(args->width * sizeof(u16), 32))) ||
	    (!depth && (args->depth_handle || args->depth_pitch)))
		return -EINVAL;
	ret = chrome9_validate_two_texture_state(&args->texture,
						 &request.texture_object_size);
	if (ret)
		return ret;
	request.depth_object_size = request.texture_object_size;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (blend) {
		ret = chrome9_validate_blend_state(&args->blend);
		if (ret)
			return ret;
	}
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size) ||
	    check_add_overflow(request.object_size,
			       request.texture_object_size, &total_size))
		return -E2BIG;
	if (depth) {
		if (check_mul_overflow((u64)args->depth_pitch,
				       (u64)args->height, &surface_size) ||
		    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
				       2ULL * PAGE_SIZE,
				       &request.texture2_object_size) ||
		    check_add_overflow(total_size,
				       request.texture2_object_size, &total_size))
			return -E2BIG;
	}
	if (total_size > SZ_64M - CHROME9_RING_SIZE ||
	    check_mul_overflow(args->triangle_count, 3U, &vertex_count) ||
	    check_mul_overflow((size_t)vertex_count, sizeof(*vertices),
			       &vertex_bytes))
		return -E2BIG;
	vertices = memdup_user(u64_to_user_ptr(args->vertices_ptr),
			       vertex_bytes);
	if (IS_ERR(vertices))
		return PTR_ERR(vertices);
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_textured_vertex *vertex = &vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_float_finite(vertex->s_float) ||
		    !chrome9_float_finite(vertex->t_float)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	for (i = 0; i < vertex_count; i += 3) {
		const struct drm_chrome9_textured_vertex *triangle = &vertices[i];

		if (!chrome9_triangle_nondegenerate(triangle[0].x_16_16,
			triangle[0].y_16_16, triangle[1].x_16_16,
			triangle[1].y_16_16, triangle[2].x_16_16,
			triangle[2].y_16_16)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	request.textured_batch_vertices = vertices;
	ret = chrome9_submit_depth_triangle(drm, file, &request);

out_free:
	kfree(vertices);
	return ret;
}

int chrome9_draw_textured_triangle_mip_ioctl(struct drm_device *drm,
					     void *data,
					     struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle_mip *args = data;
	struct drm_chrome9_textured_vertex *vertices;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_MIP,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture.handle,
		.texture_handle = args->texture.handle,
		.texture2_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.texture_width = args->texture.width,
		.texture_height = args->texture.height,
		.texture_pitch = args->texture.pitch,
		.texture_format = args->texture.format,
		.min_filter = args->texture.min_filter,
		.mag_filter = args->texture.mag_filter,
		.wrap_s = args->texture.wrap_s,
		.wrap_t = args->texture.wrap_t,
		.texture_program = args->program,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
		.triangle_count = args->triangle_count,
	};
	u32 state_flags = DRM_CHROME9_TEXTURE_RUN_DEPTH |
			  DRM_CHROME9_TEXTURE_RUN_BLEND;
	bool depth = args->flags & DRM_CHROME9_TEXTURE_RUN_DEPTH;
	bool blend = args->flags & DRM_CHROME9_TEXTURE_RUN_BLEND;
	size_t vertex_bytes;
	u64 surface_size;
	u64 total_size;
	u32 vertex_count;
	unsigned int i;
	int ret;

	if (!chrome9_textured_triangle_mip_enabled())
		return -EOPNOTSUPP;
	if (chrome9_validate_depth_flags(args->flags, state_flags) ||
	    (depth && !(args->flags & DRM_CHROME9_DEPTH_FUNC_MASK)) ||
	    (!depth && (args->flags & (DRM_CHROME9_DEPTH_FUNC_MASK |
					 DRM_CHROME9_DEPTH_READ_ONLY))) ||
	    !args->triangle_count ||
	    args->triangle_count > DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES ||
	    !args->vertices_ptr || args->reserved0 ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle ||
	    args->destination_handle == args->texture.handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    (!blend && (args->destination_format ||
			  memchr_inv(&args->blend, 0, sizeof(args->blend)))) ||
	    (depth && (!args->depth_handle ||
		       args->depth_handle == args->destination_handle ||
		       args->depth_handle == args->texture.handle ||
		       args->depth_pitch !=
			ALIGN(args->width * sizeof(u16), 32))) ||
	    (!depth && (args->depth_handle || args->depth_pitch)))
		return -EINVAL;
	ret = chrome9_validate_mip_texture_state(&args->texture,
			args->texture_last_level, args->mip_filter,
			args->lod_bias_s5_5, &request);
	if (ret)
		return ret;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (blend) {
		ret = chrome9_validate_blend_state(&args->blend);
		if (ret)
			return ret;
	}
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size) ||
	    check_add_overflow(request.object_size,
			       request.texture_object_size, &total_size))
		return -E2BIG;
	if (depth) {
		if (check_mul_overflow((u64)args->depth_pitch,
				       (u64)args->height, &surface_size) ||
		    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
				       2ULL * PAGE_SIZE,
				       &request.texture2_object_size) ||
		    check_add_overflow(total_size,
				       request.texture2_object_size, &total_size))
			return -E2BIG;
	}
	if (total_size > SZ_64M - CHROME9_RING_SIZE ||
	    check_mul_overflow(args->triangle_count, 3U, &vertex_count) ||
	    check_mul_overflow((size_t)vertex_count, sizeof(*vertices),
			       &vertex_bytes))
		return -E2BIG;
	vertices = memdup_user(u64_to_user_ptr(args->vertices_ptr), vertex_bytes);
	if (IS_ERR(vertices))
		return PTR_ERR(vertices);
	for (i = 0; i < vertex_count; i++) {
		const struct drm_chrome9_textured_vertex *vertex = &vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_safe_texcoord(vertex->s_float, args->texture.wrap_s) ||
		    !chrome9_safe_texcoord(vertex->t_float, args->texture.wrap_t)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	for (i = 0; i < vertex_count; i += 3) {
		const struct drm_chrome9_textured_vertex *triangle = &vertices[i];

		if (!chrome9_triangle_nondegenerate(triangle[0].x_16_16,
			triangle[0].y_16_16, triangle[1].x_16_16,
			triangle[1].y_16_16, triangle[2].x_16_16,
			triangle[2].y_16_16)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	request.textured_batch_vertices = vertices;
	ret = chrome9_submit_depth_triangle(drm, file, &request);

out_free:
	kfree(vertices);
	return ret;
}

int chrome9_copy_rect_ioctl(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_chrome9_copy_rect *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_COPY_RECT,
		.flags = args->flags,
		.destination_handle = args->destination_handle,
		.depth_handle = args->source_handle,
		.texture_handle = args->source_handle,
		.width = args->destination_width,
		.height = args->destination_height,
		.pitch = args->destination_pitch,
		.texture_width = args->source_width,
		.texture_height = args->source_height,
		.texture_pitch = args->source_pitch,
		.texture_format = args->format,
		.destination_format = args->format,
		.destination_x = args->destination_x,
		.destination_y = args->destination_y,
		.source_x = args->source_x,
		.source_y = args->source_y,
		.copy_width = args->width,
		.copy_height = args->height,
	};
	u64 surface_size;
	u64 total_size;

	if (args->flags & ~(DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE |
			    DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED |
			    DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA) ||
	    ((args->flags & DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE) &&
	     (args->flags &
	      DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA)) ||
	    ((args->flags & DRM_CHROME9_COPY_RECT_FORCE_ALPHA_ONE) &&
	     args->format != DRM_CHROME9_TEXTURE_FORMAT_BGRX8888) ||
	    ((args->flags &
	      DRM_CHROME9_COPY_RECT_PRESERVE_DESTINATION_ALPHA) &&
	     (!chrome9_copy_rect_preserve_alpha ||
	      args->format != DRM_CHROME9_TEXTURE_FORMAT_BGRX8888)) ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle || !args->source_handle ||
	    args->destination_handle == args->source_handle ||
	    !args->width || !args->height ||
	    !args->destination_width || args->destination_width > 2048 ||
	    !args->destination_height || args->destination_height > 2048 ||
	    !args->source_width || args->source_width > 2048 ||
	    !args->source_height || args->source_height > 2048 ||
	    args->destination_pitch !=
		ALIGN(args->destination_width * sizeof(u32), 32) ||
	    args->source_pitch != ALIGN(args->source_width * sizeof(u32), 32) ||
	    !IS_ALIGNED(args->destination_pitch, 16) ||
	    !IS_ALIGNED(args->source_pitch, 16) ||
	    args->destination_x > args->destination_width ||
	    args->width > args->destination_width - args->destination_x ||
	    args->destination_y > args->destination_height ||
	    args->height > args->destination_height - args->destination_y ||
	    args->source_x > args->source_width ||
	    args->width > args->source_width - args->source_x ||
	    args->source_y > args->source_height ||
	    args->height > args->source_height - args->source_y ||
	    args->format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888)
		return -EINVAL;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->destination_height, &surface_size))
		return -E2BIG;
	if (args->flags & DRM_CHROME9_COPY_RECT_DESTINATION_UNGUARDED)
		request.object_size = ALIGN(surface_size, PAGE_SIZE);
	else if (check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
				    2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->source_pitch,
			       (u64)args->source_height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture_object_size) ||
	    check_add_overflow(request.object_size, request.texture_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	request.depth_object_size = request.texture_object_size;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_textured_triangle_d16_ioctl(struct drm_device *drm, void *data,
					     struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle_d16 *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture_handle,
		.texture_handle = args->texture_handle,
		.texture2_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.texture_width = args->texture_width,
		.texture_height = args->texture_height,
		.texture_pitch = args->texture_pitch,
		.texture_format = args->texture_format,
		.min_filter = args->min_filter,
		.mag_filter = args->mag_filter,
		.wrap_s = args->wrap_s,
		.wrap_t = args->wrap_t,
		.texture_program = args->program,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (!(args->flags & DRM_CHROME9_DEPTH_FUNC_MASK) ||
	    chrome9_validate_depth_flags(
		args->flags, DRM_CHROME9_TEXTURED_D16_SOURCE_OVER) ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle || !args->texture_handle ||
	    !args->depth_handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32) ||
	    !args->texture_width || args->texture_width > 2048 ||
	    !args->texture_height || args->texture_height > 2048 ||
	    args->texture_pitch !=
		ALIGN(args->texture_width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    args->texture_format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ||
	    args->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    args->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    (args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->texture_pitch,
			       (u64)args->texture_height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture_object_size))
		return -E2BIG;
	request.depth_object_size = request.texture_object_size;
	if (check_mul_overflow((u64)args->depth_pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture2_object_size) ||
	    check_add_overflow(request.object_size, request.texture_object_size,
			       &total_size) ||
	    check_add_overflow(total_size, request.texture2_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_float_finite(vertex->s_float) ||
		    !chrome9_float_finite(vertex->t_float))
			return -EINVAL;
		request.textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(args->vertices[0].x_16_16,
					    args->vertices[0].y_16_16,
					    args->vertices[1].x_16_16,
					    args->vertices[1].y_16_16,
					    args->vertices[2].x_16_16,
					    args->vertices[2].y_16_16))
		return -EINVAL;
	if (args->flags & DRM_CHROME9_TEXTURED_D16_SOURCE_OVER) {
		request.blend.flags = DRM_CHROME9_BLEND_ENABLE;
		request.blend.rgb_src_factor =
			DRM_CHROME9_BLEND_FACTOR_SRC_ALPHA;
		request.blend.rgb_dst_factor =
			DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA;
		request.blend.alpha_src_factor = DRM_CHROME9_BLEND_FACTOR_ONE;
		request.blend.alpha_dst_factor =
			DRM_CHROME9_BLEND_FACTOR_INV_SRC_ALPHA;
		request.blend.rgb_func = DRM_CHROME9_BLEND_FUNC_ADD;
		request.blend.alpha_func = DRM_CHROME9_BLEND_FUNC_ADD;
		request.blend.color_mask = DRM_CHROME9_COLOR_MASK_R |
			DRM_CHROME9_COLOR_MASK_G | DRM_CHROME9_COLOR_MASK_B |
			DRM_CHROME9_COLOR_MASK_A;
	}
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_textured_triangle_d16_blend_ioctl(struct drm_device *drm,
						   void *data,
						   struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle_d16_blend *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_D16_BLEND,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture_handle,
		.texture_handle = args->texture_handle,
		.texture2_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.texture_width = args->texture_width,
		.texture_height = args->texture_height,
		.texture_pitch = args->texture_pitch,
		.texture_format = args->texture_format,
		.min_filter = args->min_filter,
		.mag_filter = args->mag_filter,
		.wrap_s = args->wrap_s,
		.wrap_t = args->wrap_t,
		.texture_program = args->program,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (!(args->flags & DRM_CHROME9_DEPTH_FUNC_MASK) ||
	    chrome9_validate_depth_flags(args->flags, 0) ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->destination_handle || !args->texture_handle ||
	    !args->depth_handle ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32) ||
	    !args->texture_width || args->texture_width > 2048 ||
	    !args->texture_height || args->texture_height > 2048 ||
	    args->texture_pitch !=
		ALIGN(args->texture_width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    args->texture_format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ||
	    args->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    args->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    (args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	ret = chrome9_validate_blend_state(&args->blend);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->texture_pitch,
			       (u64)args->texture_height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture_object_size))
		return -E2BIG;
	request.depth_object_size = request.texture_object_size;
	if (check_mul_overflow((u64)args->depth_pitch, (u64)args->height,
			       &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE,
			       &request.texture2_object_size) ||
	    check_add_overflow(request.object_size, request.texture_object_size,
			       &total_size) ||
	    check_add_overflow(total_size, request.texture2_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_float_finite(vertex->s_float) ||
		    !chrome9_float_finite(vertex->t_float))
			return -EINVAL;
		request.textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(args->vertices[0].x_16_16,
					    args->vertices[0].y_16_16,
					    args->vertices[1].x_16_16,
					    args->vertices[1].y_16_16,
					    args->vertices[2].x_16_16,
					    args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

int chrome9_draw_textured_triangle_blend_ioctl(struct drm_device *drm,
					       void *data,
					       struct drm_file *file)
{
	struct drm_chrome9_draw_textured_triangle_blend *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE_BLEND,
		.destination_handle = args->destination_handle,
		.depth_handle = args->texture_handle,
		.texture_handle = args->texture_handle,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.texture_width = args->texture_width,
		.texture_height = args->texture_height,
		.texture_pitch = args->texture_pitch,
		.texture_format = args->texture_format,
		.min_filter = args->min_filter,
		.mag_filter = args->mag_filter,
		.wrap_s = args->wrap_s,
		.wrap_t = args->wrap_t,
		.texture_program = args->program,
		.destination_format = args->destination_format,
		.scissor = args->scissor,
		.blend = args->blend,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;
	int ret;

	if (args->flags || args->pad ||
	    args->destination_format >
		DRM_CHROME9_DESTINATION_FORMAT_BGRA8888 ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    !args->texture_width || args->texture_width > 2048 ||
	    !args->texture_height || args->texture_height > 2048 ||
	    args->texture_pitch !=
		ALIGN(args->texture_width * sizeof(u32), 32) ||
	    args->program > DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ||
	    args->texture_format > DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ||
	    args->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    args->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    (args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     args->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	ret = chrome9_validate_blend_state(&args->blend);
	if (ret)
		return ret;
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	if (check_mul_overflow((u64)args->texture_pitch,
			       (u64)args->texture_height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.texture_object_size) ||
	    check_add_overflow(request.object_size, request.texture_object_size,
			       &total_size) ||
	    total_size > SZ_64M - CHROME9_RING_SIZE)
		return -E2BIG;
	request.depth_object_size = request.texture_object_size;
	for (i = 0; i < ARRAY_SIZE(args->vertices); i++) {
		const struct drm_chrome9_textured_vertex *vertex =
			&args->vertices[i];

		if (!chrome9_coordinate_valid(vertex->x_16_16, args->width) ||
		    !chrome9_coordinate_valid(vertex->y_16_16, args->height) ||
		    vertex->z_16_16 < 0 || vertex->z_16_16 > 0x10000 ||
		    !chrome9_rhw_valid(vertex->rhw_float) ||
		    !chrome9_float_finite(vertex->s_float) ||
		    !chrome9_float_finite(vertex->t_float))
			return -EINVAL;
		request.textured_vertices[i] = *vertex;
	}
	if (!chrome9_triangle_nondegenerate(
		args->vertices[0].x_16_16, args->vertices[0].y_16_16,
		args->vertices[1].x_16_16, args->vertices[1].y_16_16,
		args->vertices[2].x_16_16, args->vertices[2].y_16_16))
		return -EINVAL;
	return chrome9_submit_depth_triangle(drm, file, &request);
}

static int chrome9_validate_fragment_texture(
	const struct drm_chrome9_fragment_texture_state *texture,
	u64 *object_size, bool sparse_textures)
{
	u64 base_offset;
	u64 end = 0;
	u64 previous_end = 0;
	u64 relative;
	u64 level_size;
	u32 bytes_per_pixel;
	u32 level_count;
	unsigned int i;

	switch (texture->format) {
	case DRM_CHROME9_FRAGMENT_TEXTURE_BGRX8888:
	case DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888:
	case DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24:
		bytes_per_pixel = 4;
		break;
	case DRM_CHROME9_FRAGMENT_TEXTURE_A8:
	case DRM_CHROME9_FRAGMENT_TEXTURE_L8:
		bytes_per_pixel = 1;
		break;
	case DRM_CHROME9_FRAGMENT_TEXTURE_L8A8:
	case DRM_CHROME9_FRAGMENT_TEXTURE_RGB565:
		bytes_per_pixel = 2;
		break;
	default:
		return -EINVAL;
	}
	if (!texture->handle || !texture->width || texture->width > 2048 ||
	    !texture->height || texture->height > 2048 ||
	    texture->first_level > texture->last_level ||
	    texture->last_level >= DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS ||
	    texture->last_level > ilog2(max(texture->width, texture->height)) ||
	    texture->min_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    texture->mag_filter > DRM_CHROME9_TEXTURE_FILTER_LINEAR ||
	    texture->mip_filter > DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR ||
	    (texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     texture->wrap_s != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    (texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE &&
	     texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_REPEAT &&
	     texture->wrap_t != DRM_CHROME9_TEXTURE_WRAP_MIRRORED_REPEAT) ||
	    texture->lod_bias_s5_5 < -512 || texture->lod_bias_s5_5 > 511 ||
	    texture->min_lod_s5_5 < 0 || texture->max_lod_s5_5 < 0 ||
	    (texture->min_lod_s5_5 & 31) ||
	    (texture->max_lod_s5_5 & 31) || texture->border_color ||
	    (sparse_textures ?
	     texture->reserved0 >= DRM_CHROME9_FRAGMENT_MAX_TEXTURES :
	     texture->reserved0) ||
	    memchr_inv(texture->reserved, 0, sizeof(texture->reserved)))
		return -EINVAL;
	level_count = texture->last_level - texture->first_level;
	if (texture->min_lod_s5_5 > texture->max_lod_s5_5 ||
	    texture->max_lod_s5_5 > level_count * 32 ||
	    (!level_count &&
	     texture->mip_filter != DRM_CHROME9_TEXTURE_MIP_FILTER_NONE) ||
	    (level_count &&
	     texture->format == DRM_CHROME9_FRAGMENT_TEXTURE_X8Z24))
		return -EINVAL;
	base_offset = texture->level_offset[texture->first_level];
	if (base_offset != PAGE_SIZE)
		return -EINVAL;
	for (i = 0; i < DRM_CHROME9_FRAGMENT_MAX_TEXTURE_LEVELS; i++) {
		u32 width;
		u32 height;
		u32 pitch;

		if (i < texture->first_level || i > texture->last_level) {
			if (texture->level_offset[i] || texture->level_pitch[i])
				return -EINVAL;
			continue;
		}
		width = max(texture->width >> i, 1U);
		height = max(texture->height >> i, 1U);
		pitch = ALIGN(width * bytes_per_pixel, 32);
		if (texture->level_pitch[i] != pitch ||
		    !IS_ALIGNED(texture->level_offset[i], 256) ||
		    (i > texture->first_level &&
		     texture->level_offset[i] < ALIGN(previous_end, 256)))
			return -EINVAL;
		relative = texture->level_offset[i] - base_offset;
		if ((relative >> 8) > 0x00ffffff ||
		    (i > texture->first_level + 8 &&
		     texture->level_offset[i] !=
		     texture->level_offset[i - 1] + 0x100))
			return -EINVAL;
		if (check_mul_overflow((u64)pitch, (u64)height, &level_size) ||
		    check_add_overflow((u64)texture->level_offset[i], level_size,
				       &end))
			return -E2BIG;
		previous_end = end;
	}
	if (check_add_overflow(ALIGN(end, PAGE_SIZE), (u64)PAGE_SIZE,
			       object_size))
		return -E2BIG;
	return 0;
}

static int chrome9_fragment_texture_selftest(void)
{
	static const struct {
		u32 format;
		u32 image;
		u32 bytes_per_pixel;
	} formats[] = {
		{ DRM_CHROME9_FRAGMENT_TEXTURE_A8, 0x1b, 1 },
		{ DRM_CHROME9_FRAGMENT_TEXTURE_L8, 0x13, 1 },
		{ DRM_CHROME9_FRAGMENT_TEXTURE_L8A8, 0x15, 2 },
		{ DRM_CHROME9_FRAGMENT_TEXTURE_RGB565, 0x89, 2 },
	};
	struct drm_chrome9_fragment_texture_state texture = {
		.handle = 1,
		.format = DRM_CHROME9_FRAGMENT_TEXTURE_BGRA8888,
		.width = 64,
		.height = 32,
		.last_level = 1,
		.min_filter = DRM_CHROME9_TEXTURE_FILTER_LINEAR,
		.mag_filter = DRM_CHROME9_TEXTURE_FILTER_LINEAR,
		.mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR,
		.wrap_s = DRM_CHROME9_TEXTURE_WRAP_REPEAT,
		.wrap_t = DRM_CHROME9_TEXTURE_WRAP_REPEAT,
		.lod_bias_s5_5 = -16,
		.max_lod_s5_5 = 32,
		.level_offset = { PAGE_SIZE, 3 * PAGE_SIZE },
		.level_pitch = { 256, 128 },
	};
	static const u32 expected_sampler[] = {
		0x303f0040, 0x31102492, 0x32000052, 0xcccccccc,
	};
	u32 sampler[4];
	u32 mip[8];
	u32 packet[40] __aligned(16);
	u32 *cursor;
	u64 object_size;
	unsigned int i;
	int ret;

	/* L0 of a 64x32 BGRA target occupies one 0x2000-byte image span
	 * between two guard pages.  A mip-capable target has further storage
	 * after that second guard, and a separate final guard page. */
	if (!chrome9_fragment_destination_size_valid(4 * PAGE_SIZE,
						      4 * PAGE_SIZE, false) ||
	    chrome9_fragment_destination_size_valid(6 * PAGE_SIZE,
						     4 * PAGE_SIZE, false) ||
	    chrome9_fragment_destination_size_valid(4 * PAGE_SIZE,
						     4 * PAGE_SIZE, true) ||
	    chrome9_fragment_destination_size_valid(3 * PAGE_SIZE,
						     4 * PAGE_SIZE, true) ||
	    !chrome9_fragment_destination_size_valid(6 * PAGE_SIZE,
						      4 * PAGE_SIZE, true) ||
	    chrome9_fragment_destination_size_valid(6 * PAGE_SIZE + 1,
						     4 * PAGE_SIZE, true))
		return -EINVAL;

	ret = chrome9_validate_fragment_texture(&texture, &object_size, false);
	if (ret)
		return ret;
	chrome9_fragment_sampler_state(&texture, sampler);
	chrome9_fragment_mip_state(&texture, mip);
	if (object_size != 5 * PAGE_SIZE ||
	    memcmp(sampler, expected_sampler, sizeof(expected_sampler)) ||
	    mip[0] != 0x40000020 || memchr_inv(mip + 1, 0,
					       7 * sizeof(*mip)))
		return -EINVAL;
	/* Both ABIs use compact BO addresses.  Only the packet's image,
	 * mipmap and sampler selectors change to the logical unit in v2. */
	cursor = packet;
	chrome9_emit_fragment_texture(&cursor, &texture, 0x4000, 0);
	if (cursor != packet + 32 || packet[2] != 0x00020000 ||
	    packet[4] != 0x50 || packet[14] != 0x10030000 ||
	    packet[26] != 0x20020000)
		return -EINVAL;
	texture.reserved0 = 5;
	if (chrome9_validate_fragment_texture(&texture, &object_size, false) !=
	    -EINVAL ||
	    chrome9_validate_fragment_texture(&texture, &object_size, true))
		return -EINVAL;
	cursor = packet;
	chrome9_emit_fragment_texture(&cursor, &texture, 0x4000,
		texture.reserved0);
	if (cursor != packet + 32 || packet[2] != 0x05020000 ||
	    packet[4] != 0x50 || packet[14] != 0x10030028 ||
	    packet[26] != 0x25020000)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		memset(&texture, 0, sizeof(texture));
		texture.handle = 1;
		texture.format = formats[i].format;
		texture.width = 32;
		texture.height = 32;
		texture.last_level = 1;
		texture.min_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
		texture.mag_filter = DRM_CHROME9_TEXTURE_FILTER_NEAREST;
		texture.mip_filter = DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST;
		texture.wrap_s = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
		texture.wrap_t = DRM_CHROME9_TEXTURE_WRAP_CLAMP_TO_EDGE;
		texture.max_lod_s5_5 = 32;
		texture.level_offset[0] = PAGE_SIZE;
		texture.level_pitch[0] =
			ALIGN(texture.width * formats[i].bytes_per_pixel, 32);
		texture.level_offset[1] = 3 * PAGE_SIZE;
		texture.level_pitch[1] =
			ALIGN((texture.width >> 1) * formats[i].bytes_per_pixel, 32);
		ret = chrome9_validate_fragment_texture(&texture, &object_size,
			false);
		if (ret || object_size != 5 * PAGE_SIZE ||
		    chrome9_fragment_texture_format(texture.format) !=
			formats[i].image)
			return -EINVAL;
		texture.level_pitch[0] += 32;
		if (chrome9_validate_fragment_texture(&texture, &object_size,
			false) !=
		    -EINVAL)
			return -EINVAL;
	}
	return 0;
}

static int chrome9_draw_fragment(struct drm_device *drm, struct drm_file *file,
				 struct drm_chrome9_draw_fragment_program *args,
				 struct chrome9_tcl_draw *tcl)
{
	const struct chrome9_fragment_rt_format *rt_format =
		chrome9_fragment_rt_format(args->destination_format);
	struct drm_chrome9_fragment_instruction *instructions = NULL;
	struct drm_chrome9_fragment_texture_state *textures = NULL;
	struct drm_chrome9_fragment_vertex *vertices = NULL;
	struct chrome9_fragment_program *program = NULL;
	u32 *constants = NULL;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_FRAGMENT_PROGRAM,
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.flags = args->flags,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
		.destination_format = args->destination_format,
		.fragment_depth_format = args->depth_format,
		.stencil = args->stencil,
		.scissor = args->scissor,
		.blend = args->blend,
		.triangle_count = args->triangle_count,
		.fragment_texture_count = args->texture_count,
	};
	u32 allowed_flags = DRM_CHROME9_FRAGMENT_DEPTH |
		DRM_CHROME9_FRAGMENT_BLEND |
		DRM_CHROME9_FRAGMENT_DESTINATION_EXTENDED |
		DRM_CHROME9_FRAGMENT_STENCIL;
	bool stencil = args->flags & DRM_CHROME9_FRAGMENT_STENCIL;
	u64 surface_size;
	u64 total_size;
	u32 vertex_count;
	u32 descriptor_mask = 0;
	bool depth = args->flags & DRM_CHROME9_FRAGMENT_DEPTH;
	bool blend = args->flags & DRM_CHROME9_FRAGMENT_BLEND;
	bool sparse_textures = args->abi_version ==
		DRM_CHROME9_FRAGMENT_ABI_SPARSE_TEXTURES;
	unsigned int i;
	int ret;

	if ((args->abi_version != DRM_CHROME9_FRAGMENT_ABI_VERSION &&
	     !sparse_textures) ||
	    (sparse_textures && !chrome9_fragment_sparse_textures_enabled()) ||
	    args->reserved0 || args->pad ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    chrome9_validate_depth_flags(args->flags, allowed_flags) ||
	    (!depth && (args->flags & (DRM_CHROME9_DEPTH_FUNC_MASK |
				       DRM_CHROME9_DEPTH_READ_ONLY))) ||
	    !args->destination_handle ||
	    (tcl ? args->triangle_count || args->vertices_ptr :
	     !args->triangle_count ||
	     args->triangle_count > DRM_CHROME9_FRAGMENT_MAX_TRIANGLES ||
	     !args->vertices_ptr) ||
	    !args->instructions_ptr ||
	    !args->instruction_count ||
	    args->instruction_count > DRM_CHROME9_FRAGMENT_MAX_INSTRUCTIONS ||
	    args->constant_count > DRM_CHROME9_FRAGMENT_MAX_CONSTANTS ||
	    args->texture_count > DRM_CHROME9_FRAGMENT_MAX_TEXTURES ||
	    args->varying_count > DRM_CHROME9_FRAGMENT_MAX_VARYINGS ||
	    (!!args->constants_ptr != !!args->constant_count) ||
	    (!!args->textures_ptr != !!args->texture_count) ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    !rt_format ||
	    args->destination_pitch != ALIGN(args->width * rt_format->cpp, 32) ||
	    (rt_format->cpp == 2 &&
	     (depth || blend)) ||
	    (!blend && memchr_inv(&args->blend, 0, sizeof(args->blend))) ||
	    (depth && (!args->depth_handle ||
		(args->depth_format != DRM_CHROME9_FRAGMENT_DEPTH_D16 &&
		 args->depth_format != DRM_CHROME9_FRAGMENT_DEPTH_X8Z24))) ||
	    (!depth && (args->depth_handle || args->depth_pitch ||
			   args->depth_format)) ||
	    (stencil && (!depth ||
		args->depth_format != DRM_CHROME9_FRAGMENT_DEPTH_X8Z24 ||
		chrome9_validate_stencil(&args->stencil))) ||
	    (!stencil && memchr_inv(&args->stencil, 0, sizeof(args->stencil))))
		return -EINVAL;
	ret = chrome9_validate_scissor(&args->scissor, args->width,
				       args->height);
	if (ret)
		return ret;
	if (blend) {
		ret = chrome9_validate_blend_state(&args->blend);
		if (ret)
			return ret;
	}
	if (check_mul_overflow((u64)args->destination_pitch,
			       (u64)args->height, &surface_size) ||
	    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
			       2ULL * PAGE_SIZE, &request.object_size))
		return -E2BIG;
	total_size = request.object_size;
	if (depth) {
		u32 bytes_per_pixel = args->depth_format ==
			DRM_CHROME9_FRAGMENT_DEPTH_X8Z24 ? 4 : 2;

		if (args->depth_pitch !=
		    ALIGN(args->width * bytes_per_pixel, 32) ||
		    check_mul_overflow((u64)args->depth_pitch,
				       (u64)args->height, &surface_size) ||
		    check_add_overflow(ALIGN(surface_size, PAGE_SIZE),
				       2ULL * PAGE_SIZE,
				       &request.depth_object_size) ||
		    check_add_overflow(total_size, request.depth_object_size,
				       &total_size))
			return -E2BIG;
	}
	if (check_mul_overflow(args->triangle_count, 3U, &vertex_count))
		return -E2BIG;
	instructions = memdup_array_user(u64_to_user_ptr(args->instructions_ptr),
		args->instruction_count, sizeof(*instructions));
	if (IS_ERR(instructions))
		return PTR_ERR(instructions);
	if (args->constant_count) {
		constants = memdup_array_user(u64_to_user_ptr(args->constants_ptr),
			args->constant_count, 4 * sizeof(*constants));
		if (IS_ERR(constants)) {
			ret = PTR_ERR(constants);
			constants = NULL;
			goto out_free;
		}
	}
	if (args->texture_count) {
		textures = memdup_array_user(u64_to_user_ptr(args->textures_ptr),
			args->texture_count, sizeof(*textures));
		if (IS_ERR(textures)) {
			ret = PTR_ERR(textures);
			textures = NULL;
			goto out_free;
		}
		for (i = 0; i < args->texture_count; i++) {
			u32 unit = sparse_textures ? textures[i].reserved0 : i;

			if (unit >= DRM_CHROME9_FRAGMENT_MAX_TEXTURES ||
			    (sparse_textures && i &&
			     unit <= request.fragment_texture_units[i - 1])) {
				ret = -EINVAL;
				goto out_free;
			}
			ret = chrome9_validate_fragment_texture(&textures[i],
				&request.fragment_texture_size[i], sparse_textures);
			if (ret)
				goto out_free;
			request.fragment_texture_units[i] = unit;
			descriptor_mask |= BIT(unit);
			if (check_add_overflow(total_size,
				request.fragment_texture_size[i], &total_size)) {
				ret = -E2BIG;
				goto out_free;
			}
		}
	}
	if (total_size > SZ_64M - CHROME9_RING_SIZE) {
		ret = -E2BIG;
		goto out_free;
	}
	if (!tcl) {
		vertices = vmemdup_array_user(u64_to_user_ptr(args->vertices_ptr),
			vertex_count, sizeof(*vertices));
		if (IS_ERR(vertices)) {
			ret = PTR_ERR(vertices);
			vertices = NULL;
			goto out_free;
		}
		for (i = 0; i < vertex_count; i++) {
			unsigned int varying;

			if (!chrome9_coordinate_valid(vertices[i].x_16_16, args->width) ||
			    !chrome9_coordinate_valid(vertices[i].y_16_16, args->height) ||
			    vertices[i].z_16_16 < 0 || vertices[i].z_16_16 > 0x10000 ||
			    !chrome9_rhw_valid(vertices[i].rhw_float)) {
				ret = -EINVAL;
				goto out_free;
			}
			for (varying = 0; varying < args->varying_count; varying++) {
				unsigned int component;

				for (component = 0; component < 4; component++)
					if (!chrome9_float_finite(
						vertices[i].varying[varying][component])) {
						ret = -EINVAL;
						goto out_free;
					}
			}
			for (; varying < DRM_CHROME9_FRAGMENT_MAX_VARYINGS; varying++)
				if (memchr_inv(vertices[i].varying[varying], 0,
					       sizeof(vertices[i].varying[varying]))) {
					ret = -EINVAL;
					goto out_free;
				}
		}
		for (i = 0; i < vertex_count; i += 3)
			if (!chrome9_triangle_nondegenerate(
				vertices[i].x_16_16, vertices[i].y_16_16,
				vertices[i + 1].x_16_16, vertices[i + 1].y_16_16,
				vertices[i + 2].x_16_16, vertices[i + 2].y_16_16)) {
				ret = -EINVAL;
				goto out_free;
			}
	}
	chrome9_prof_mark(CHROME9_PROF_ENTRY);
	program = kvzalloc(sizeof(*program), GFP_KERNEL);
	if (!program) {
		ret = -ENOMEM;
		goto out_free;
	}
	if (sparse_textures)
		ret = chrome9_fragment_compile_sparse(instructions,
			args->instruction_count, constants, args->constant_count,
			args->texture_count, descriptor_mask, args->varying_count,
			program);
	else
		ret = chrome9_fragment_compile(instructions,
			args->instruction_count, constants, args->constant_count,
			args->texture_count, args->varying_count, program);
	if (ret)
		goto out_free;
	if (tcl) {
		ret = chrome9_vertex_compile(tcl->instructions,
			tcl->instruction_count, program, tcl->element_count,
			tcl->input_lanes, tcl->constant_first,
			tcl->constant_count,
			tcl->cull & (DRM_CHROME9_VERTEX_BFC_CW |
				     DRM_CHROME9_VERTEX_BFC_CCW),
			tcl->clip_mask, &tcl->program);
		if (ret)
			goto out_free;
		if (tcl->re & CHROME9_TCL_RE_NO_SECONDARY) {
			struct chrome9_vertex_program *vp = &tcl->program;
			u32 last_input = vp->a62 & 0x1ff, kept = 0, r;

			for (r = 0; r < vp->record_count; r++) {
				u32 dest = vp->records[r][3] >> 4 & 0xff;

				if (dest == 0x23 || dest == 0x25) {
					if (r < last_input)
						vp->a62--;
					continue;
				}
				memmove(vp->records[kept++], vp->records[r],
					sizeof(vp->records[r]));
			}
			vp->active_count -= vp->record_count - kept;
			vp->record_count = kept;
			vp->a60 = 0x60800000 | (vp->active_count - 1);
		}
		if (tcl->re & CHROME9_TCL_RE_NO_BFD_SAT)
			for (i = 0; i < tcl->program.record_count; i++)
				if ((tcl->program.records[i][3] >> 4 & 0xff) == 0x24)
					tcl->program.records[i][3] &= ~BIT(14);
		request.triangle_count = tcl->vertex_count / 3;
		request.tcl = tcl;
	}
	request.fragment_vertices = vertices;
	request.fragment_textures = textures;
	request.fragment_program = program;
	chrome9_prof_mark(CHROME9_PROF_COMPILE);
	ret = chrome9_submit_fragment_program(drm, file, &request);
	chrome9_prof_mark(CHROME9_PROF_RETIRE);
	chrome9_prof_job(drm);

out_free:
	kvfree(program);
	kvfree(vertices);
	kfree(textures);
	kfree(constants);
	kfree(instructions);
	return ret;
}

int chrome9_draw_fragment_program_ioctl(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	chrome9_prof_start();
	return chrome9_draw_fragment(drm, file, data, NULL);
}

bool chrome9_hw_tcl_enabled(void)
{
	return READ_ONCE(chrome9_hw_tcl);
}

/* Raw stage A payload bound; the rest of the 64-KiB segment is state. */
#define CHROME9_TCL_MAX_VERTEX_BYTES	(40U * 1024)

/*
 * Final float bits from userspace (no kernel FPU): finite, |sx|,|sy| <=
 * 2048, |sz|,|oz| <= 1, ox/oy inside the [2^19, 2^20) snap domain.
 */
static bool chrome9_tcl_viewport_valid(const u32 viewport[8])
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		if (!chrome9_float_finite(viewport[i]))
			return false;
	return (viewport[0] & 0x7fffffff) <= 0x45000000 &&
	       (viewport[1] & 0x7fffffff) <= 0x45000000 &&
	       (viewport[2] & 0x7fffffff) <= 0x3f800000 &&
	       (viewport[5] & 0x7fffffff) <= 0x3f800000 &&
	       viewport[3] >> 23 == 0x92 && viewport[4] >> 23 == 0x92 &&
	       !viewport[6] && !viewport[7];
}


static int chrome9_tcl_elements(struct chrome9_tcl_draw *tcl,
		const struct drm_chrome9_vertex_element *elements, u32 stride)
{
	u32 fetched = 0;
	unsigned int e;

	for (e = 0; e < tcl->element_count; e++) {
		const struct drm_chrome9_vertex_element *element = &elements[e];

		if (element->format >= ARRAY_SIZE(chrome9_tcl_formats) ||
		    element->reserved || !IS_ALIGNED(element->offset, 4) ||
		    (tcl->buffer_count ?
		     element->buffer >= tcl->buffer_count :
		     element->buffer || element->offset >
			stride - 4 * chrome9_tcl_formats[element->format].dwords))
			return -EINVAL;
		tcl->element_format[e] = chrome9_tcl_formats[element->format].native;
		tcl->element_type[e] = element->format;
		if (tcl->buffer_count) {
			tcl->element_buffer[e] = element->buffer;
			tcl->element_byte_offset[e] = element->offset;
		} else {
			tcl->element_offset[e] = element->offset / 4;
		}
		/* The fetcher fills lanes a format lacks with (0, 0, 0, 1). */
		tcl->input_lanes[e] = 0xf;
		fetched += chrome9_tcl_formats[element->format].dwords;
	}
	if (tcl->buffer_count) {
		tcl->stride_dw = fetched;
		return 0;
	}
	/* Inline vertices are parsed as the sum of element fetch dwords
	 * (A00[6:0]); padding misaligns the stream and stops the engine. */
	return fetched * 4 == stride ? 0 : -EINVAL;
}

/*
 * Stage B indices: find the inclusive range, rebase to zero (the range
 * start folds into the element addresses) and pack 16-bit pairs when the
 * range allows.  An odd 16-bit count leaves the upper lane zero.
 */
static u32 *chrome9_tcl_indices(struct chrome9_tcl_draw *tcl,
		const void *indices, u32 index_size)
{
	u32 min = U32_MAX, max = 0, value;
	unsigned int i;
	u32 *packed;

	for (i = 0; i < tcl->index_count; i++) {
		value = index_size == 2 ? ((const u16 *)indices)[i] :
			((const u32 *)indices)[i];
		min = min(min, value);
		max = max(max, value);
	}
	tcl->index_min = min;
	tcl->index_range = max - min;
	tcl->index32 = tcl->index_range > 0xffff;
	tcl->index_dwords = tcl->index32 ? tcl->index_count :
		DIV_ROUND_UP(tcl->index_count, 2);
	packed = kvcalloc(tcl->index_dwords, sizeof(*packed), GFP_KERNEL);
	if (!packed)
		return NULL;
	for (i = 0; i < tcl->index_count; i++) {
		value = (index_size == 2 ? ((const u16 *)indices)[i] :
			 ((const u32 *)indices)[i]) - min;
		if (tcl->index32)
			packed[i] = value;
		else
			packed[i / 2] |= value << (i & 1 ? 16 : 0);
	}
	return packed;
}


static bool chrome9_tcl_vertices_finite(const struct chrome9_tcl_draw *tcl)
{
	unsigned int v, e, c;

	for (v = 0; v < tcl->vertex_count; v++) {
		const u32 *vertex = tcl->vertices + v * tcl->stride_dw;

		for (e = 0; e < tcl->element_count; e++) {
			const u32 *data = vertex + tcl->element_offset[e];
			u8 type = tcl->element_type[e];

			for (c = 0; c < chrome9_tcl_formats[type].float32; c++)
				if (!chrome9_float_finite(data[c]))
					return false;
			for (c = 0; c < chrome9_tcl_formats[type].float16; c++)
				if (!chrome9_tcl_half_finite(data[c]))
					return false;
		}
	}
	return true;
}

int chrome9_draw_vertex_program_ioctl(struct drm_device *drm, void *data,
				      struct drm_file *file)
{
	struct drm_chrome9_draw_vertex_program *args = data;
	struct drm_chrome9_vertex_instruction *instructions = NULL;
	struct drm_chrome9_vertex_element *elements = NULL;
	struct drm_chrome9_vertex_buffer *buffers = NULL;
	struct chrome9_tcl_draw *tcl = NULL;
	void *indices = NULL;
	u32 *constants = NULL;
	u32 *vertices = NULL;
	bool stage_b = args->buffer_count;
	unsigned int i;
	int ret;

	if (!chrome9_hw_tcl_enabled())
		return -EOPNOTSUPP;
	chrome9_prof_start();
	if (stage_b) {
		if (args->vertex_count || args->vertex_stride ||
		    args->vertices_ptr ||
		    args->buffer_count > args->element_count ||
		    args->buffer_count > DRM_CHROME9_VERTEX_MAX_INPUTS ||
		    !args->index_count || args->index_count % 3 ||
		    args->index_count > DRM_CHROME9_VERTEX_MAX_INDICES ||
		    (args->index_size != 2 && args->index_size != 4) ||
		    !args->indices_ptr || !args->buffers_ptr)
			return -EINVAL;
	} else if (args->index_count || args->index_size ||
		   args->base_vertex || args->indices_ptr || args->buffers_ptr ||
		   !args->vertex_count || args->vertex_count % 3 ||
		   args->vertex_count > DRM_CHROME9_VERTEX_MAX_VERTICES ||
		   !IS_ALIGNED(args->vertex_stride, 4) ||
		   args->vertex_stride < 4 || args->vertex_stride > 4 * 127 ||
		   args->vertex_count * args->vertex_stride >
			CHROME9_TCL_MAX_VERTEX_BYTES ||
		   !args->vertices_ptr) {
		return -EINVAL;
	}
	if (args->abi_version != DRM_CHROME9_VERTEX_ABI_VERSION ||
	    (args->flags & ~(DRM_CHROME9_VERTEX_CULL_CW |
			     DRM_CHROME9_VERTEX_CULL_CCW |
			     DRM_CHROME9_VERTEX_FLAT |
			     DRM_CHROME9_VERTEX_BFC_CW |
			     DRM_CHROME9_VERTEX_BFC_CCW)) ||
	    hweight32(args->flags & (DRM_CHROME9_VERTEX_CULL_CW |
				     DRM_CHROME9_VERTEX_CULL_CCW)) > 1 ||
	    hweight32(args->flags & (DRM_CHROME9_VERTEX_BFC_CW |
				     DRM_CHROME9_VERTEX_BFC_CCW)) > 1 ||
	    (args->clip_distance_mask &
	     ~GENMASK(DRM_CHROME9_VERTEX_MAX_CLIP_DISTANCES - 1, 0)) ||
	    args->pad0 ||
	    memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    !args->instruction_count ||
	    args->instruction_count > DRM_CHROME9_VERTEX_MAX_INSTRUCTIONS ||
	    !args->element_count ||
	    args->element_count > DRM_CHROME9_VERTEX_MAX_INPUTS ||
	    args->constant_first > DRM_CHROME9_VERTEX_MAX_CONSTANTS ||
	    args->constant_count >
		DRM_CHROME9_VERTEX_MAX_CONSTANTS - args->constant_first ||
	    !!args->constants_ptr != !!args->constant_count ||
	    !args->instructions_ptr || !args->elements_ptr ||
	    !chrome9_tcl_viewport_valid(args->viewport))
		return -EINVAL;
	tcl = kvzalloc(sizeof(*tcl), GFP_KERNEL);
	if (!tcl)
		return -ENOMEM;
	tcl->instruction_count = args->instruction_count;
	tcl->cull = args->flags;
	tcl->clip_mask = args->clip_distance_mask;
	tcl->re = READ_ONCE(chrome9_tcl_re);
	tcl->element_count = args->element_count;
	tcl->constant_first = args->constant_first;
	tcl->constant_count = args->constant_count;
	tcl->vertex_count = args->vertex_count;
	tcl->stride_dw = args->vertex_stride / 4;
	for (i = 0; i < 3; i++) {
		tcl->rows[i] = args->viewport[i];
		tcl->rows[4 + i] = args->viewport[3 + i];
	}
	tcl->rows[8] = 0x49400000;
	tcl->rows[9] = 0x49400000;
	if (stage_b) {
		tcl->buffer_count = args->buffer_count;
		tcl->index_count = args->index_count;
		tcl->base_vertex = args->base_vertex;
		buffers = memdup_array_user(u64_to_user_ptr(args->buffers_ptr),
			args->buffer_count, sizeof(*buffers));
		if (IS_ERR(buffers)) {
			ret = PTR_ERR(buffers);
			buffers = NULL;
			goto out_free;
		}
		/* Stride 0 repeats one element (constant attribute). */
		for (i = 0; i < args->buffer_count; i++) {
			if (!buffers[i].handle ||
			    !IS_ALIGNED(buffers[i].stride, 4) ||
			    buffers[i].stride > 2044 ||
			    !IS_ALIGNED(buffers[i].offset, 4)) {
				ret = -EINVAL;
				goto out_free;
			}
			tcl->buffers[i] = buffers[i];
		}
		indices = vmemdup_array_user(u64_to_user_ptr(args->indices_ptr),
			args->index_count, args->index_size);
		if (IS_ERR(indices)) {
			ret = PTR_ERR(indices);
			indices = NULL;
			goto out_free;
		}
		tcl->indices = chrome9_tcl_indices(tcl, indices,
						   args->index_size);
		if (!tcl->indices) {
			ret = -ENOMEM;
			goto out_free;
		}
		tcl->vertex_count = args->index_count;
	}

	elements = memdup_array_user(u64_to_user_ptr(args->elements_ptr),
		args->element_count, sizeof(*elements));
	if (IS_ERR(elements)) {
		ret = PTR_ERR(elements);
		elements = NULL;
		goto out_free;
	}
	ret = chrome9_tcl_elements(tcl, elements, args->vertex_stride);
	if (ret)
		goto out_free;
	instructions = memdup_array_user(u64_to_user_ptr(args->instructions_ptr),
		args->instruction_count, sizeof(*instructions));
	if (IS_ERR(instructions)) {
		ret = PTR_ERR(instructions);
		instructions = NULL;
		goto out_free;
	}
	tcl->instructions = instructions;
	if (args->constant_count) {
		constants = memdup_array_user(u64_to_user_ptr(args->constants_ptr),
			args->constant_count, 4 * sizeof(*constants));
		if (IS_ERR(constants)) {
			ret = PTR_ERR(constants);
			constants = NULL;
			goto out_free;
		}
		for (i = 0; i < 4 * args->constant_count; i++)
			if (!chrome9_float_finite(constants[i])) {
				ret = -EINVAL;
				goto out_free;
			}
		tcl->constants = constants;
	}
	if (!stage_b) {
		vertices = vmemdup_array_user(u64_to_user_ptr(args->vertices_ptr),
			args->vertex_count, args->vertex_stride);
		if (IS_ERR(vertices)) {
			ret = PTR_ERR(vertices);
			vertices = NULL;
			goto out_free;
		}
		tcl->vertices = vertices;
		if (!chrome9_tcl_vertices_finite(tcl)) {
			ret = -EINVAL;
			goto out_free;
		}
	}
	ret = chrome9_draw_fragment(drm, file, &args->fragment, tcl);

out_free:
	if (tcl)
		kvfree(tcl->indices);
	kvfree(indices);
	kfree(buffers);
	kvfree(vertices);
	kfree(constants);
	kfree(instructions);
	kfree(elements);
	kvfree(tcl);
	return ret;
}

void chrome9_engine_fini(struct chrome9_device *cdev)
{
	struct chrome9_engine *engine = cdev->engine;
	u32 invalid[CHROME9_RING_PAGES];
	unsigned int i;
	u32 status;
	int ret;

	if (!engine)
		return;
	for (i = 0; i < ARRAY_SIZE(invalid); i++)
		invalid[i] = CHROME9_GART_INVALID;
	mutex_lock(&engine->submit_lock);
	/* KMS must retire every persistent display pin before engine teardown. */
	if (!list_empty(&engine->scanout_mappings)) {
		dev_crit(&cdev->pdev->dev,
			 "refusing engine teardown with live scanout mappings\n");
		mutex_unlock(&engine->submit_lock);
		return;
	}
	engine->stopping = true;
	engine->ready = false;
	if (engine->wedged || !list_empty(&engine->quarantine)) {
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "refusing unsafe engine teardown with live/quarantined mappings; reboot required\n");
		mutex_unlock(&engine->submit_lock);
		return;
	}
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, status,
				 status & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
					 status,
					 !(status & CHROME9_ENGINE_WORK_MASK),
					 10, CHROME9_HW_TIMEOUT_US);
	if (!ret)
		ret = chrome9_stop_active_ring(engine, NULL);
	if (ret) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "engine did not quiesce; retained for reboot\n");
		mutex_unlock(&engine->submit_lock);
		return;
	}
	ret = chrome9_draw_cache_purge_locked(engine);
	if (ret) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "draw SF cache did not drain; retained for reboot\n");
		mutex_unlock(&engine->submit_lock);
		return;
	}
	mutex_lock(&engine->vm_lock);
	ret = chrome9_gart_set(engine, 0, invalid, ARRAY_SIZE(invalid));
	mutex_unlock(&engine->vm_lock);
	if (ret) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		mutex_unlock(&engine->submit_lock);
		return;
	}
	ret = chrome9_gart_restore(engine);
	if (ret) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		mutex_unlock(&engine->submit_lock);
		return;
	}
	if (engine->mm_initialized)
		drm_mm_takedown(&engine->sf_mm);
	writel(engine->saved_dst_pos, cdev->mmio + CHROME9_2D_REG_DST_POS);
	readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
	if (!engine->was_master)
		pci_clear_master(cdev->pdev);
	dev_info(&cdev->pdev->dev,
		 "persistent SF engine stopped: appends=%llu wraps=%llu epochs=%u cache_hits=%llu cache_misses=%llu scanout_hits=%llu evictions=%llu gart_updates=%llu pte_writes=%llu vga_acquires=%llu sr6f_requests=%llu sr6f_rerequests=%llu sr6f_slow=%llu sr6f_kicks=%llu fe33=%llu/%u fe33_canary_faults=%llu\n",
		 engine->append_count, engine->wrap_count, engine->ring_epoch,
		 engine->draw_cache_hits, engine->draw_cache_misses,
		 engine->draw_cache_scanout_hits, engine->draw_cache_evictions,
		 engine->gart_update_count, engine->gart_pte_write_count,
		 engine->gart_vga_acquire_count, engine->gart_flush_count,
		 engine->gart_flush_rerequests, engine->gart_flush_slow,
		 engine->gart_flush_kicks,
		 engine->fence_count, engine->fence_last_id,
		 engine->fence_canary_faults);
	mutex_unlock(&engine->submit_lock);
	cdev->engine = NULL;
	vfree(engine->run_staging_cpu);
	free_page((unsigned long)engine->staging_cpu);
	free_page((unsigned long)engine->kick_cpu);
	dma_free_coherent(&cdev->pdev->dev, PAGE_SIZE, engine->scratch_cpu,
			  engine->scratch_dma);
	if (engine->fence_cpu)
		dma_free_coherent(&cdev->pdev->dev, PAGE_SIZE, engine->fence_cpu,
				  engine->fence_dma);
	dma_free_coherent(&cdev->pdev->dev, CHROME9_RING_SIZE,
			  engine->ring_cpu, engine->ring_dma);
	kvfree(engine->gart_saved);
	kvfree(engine->timeout_segment);
	if (engine->module_hold)
		module_put(THIS_MODULE);
	kfree(engine);
}
