// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/vgaarb.h>

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

#define CHROME9_GART_SIZE		SZ_64K
#define CHROME9_GART_ENTRIES		(CHROME9_GART_SIZE / sizeof(u32))
#define CHROME9_GART_INVALID		0x80000000U
#define CHROME9_GART_CACHED		BIT(25)
#define CHROME9_RING_PAGES		2U
#define CHROME9_RING_SIZE		(CHROME9_RING_PAGES * PAGE_SIZE)
#define CHROME9_CR_READ_AHEAD		0x220U
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

static bool chrome9_engine_enable;
module_param_named(engine, chrome9_engine_enable, bool, 0400);
MODULE_PARM_DESC(engine,
		 "Enable persistent GART/ring and fixed-quad ioctl (default: false)");

static unsigned long chrome9_engine_gart_offset = ULONG_MAX;
module_param_named(engine_gart_offset, chrome9_engine_gart_offset, ulong, 0400);
MODULE_PARM_DESC(engine_gart_offset,
		 "Reserved BAR2 byte offset of the persistent 64-KiB GART");

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

struct chrome9_job {
	struct chrome9_fence fence;
	struct drm_gem_shmem_object *destination;
	struct drm_gem_shmem_object *depth;
	struct sg_table *sgt;
	struct sg_table *depth_sgt;
	struct iosys_map map;
	struct iosys_map depth_map;
	struct drm_mm_node sf_node;
	struct list_head quarantine_link;
	u32 *ptes;
	unsigned int page_count;
	unsigned int destination_page_count;
	unsigned int depth_page_count;
	u32 cookie;
	u8 slot;
	bool mapped;
	bool depth_mapped;
	bool node_allocated;
	bool sf_bound;
	bool fence_attached;
	bool published;
	bool retirement_proven;
};

enum chrome9_draw_kind {
	CHROME9_DRAW_FIXED,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE,
	CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16,
	CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE,
};

struct chrome9_draw_request {
	enum chrome9_draw_kind kind;
	u32 destination_handle;
	u32 depth_handle;
	u32 texture_handle;
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
	u32 texture_program;
	u64 texture_object_size;
	struct drm_chrome9_vertex vertices[3];
	struct drm_chrome9_vertex_d16 depth_vertices[3];
	struct drm_chrome9_textured_vertex textured_vertices[3];
};

struct chrome9_engine {
	struct chrome9_device *cdev;
	struct mutex submit_lock;
	struct mutex vm_lock;
	struct drm_mm sf_mm;
	struct list_head quarantine;
	void *ring_cpu;
	dma_addr_t ring_dma;
	void __iomem *gart;
	void *gart_saved;
	u64 fence_context;
	atomic64_t fence_seqno;
	u32 saved_dst_pos;
	u8 saved_seq_index;
	u8 saved_6a;
	u8 saved_6b;
	u8 saved_6c;
	u8 saved_7b;
	u8 active_slot;
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
	struct chrome9_job *job =
		container_of(fence, struct chrome9_job, fence.base);

	kfree(job->ptes);
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

static int chrome9_gart_flush(struct chrome9_device *cdev)
{
	u8 value;
	int ret;

	ret = chrome9_gart_wait_idle(cdev);
	if (ret)
		return ret;
	value = readb(cdev->mmio + CHROME9_SEQ_DATA);
	writeb(value | BIT(7), cdev->mmio + CHROME9_SEQ_DATA);
	return readb_poll_timeout(cdev->mmio + CHROME9_SEQ_DATA, value,
				  !(value & BIT(7)), 10,
				  CHROME9_HW_TIMEOUT_US);
}

static int chrome9_detect_vram_size(struct chrome9_device *cdev)
{
	resource_size_t size;
	u8 saved_index;
	u8 code;
	int ret;

	if (cdev->vram_size)
		return 0;
	ret = vga_get_uninterruptible(cdev->pdev, VGA_RSRC_LEGACY_IO);
	if (ret)
		return ret;
	saved_index = readb(cdev->mmio + CHROME9_SEQ_INDEX);
	code = chrome9_seq_read(cdev, 0x14);
	writeb(saved_index, cdev->mmio + CHROME9_SEQ_INDEX);
	vga_put(cdev->pdev, VGA_RSRC_LEGACY_IO);

	switch (code) {
	case 0x00:
		size = SZ_512M;
		break;
	case 0x80:
		size = SZ_256M;
		break;
	case 0xc0:
		size = SZ_128M;
		break;
	case 0xe0:
		size = SZ_64M;
		break;
	case 0xf0:
		size = SZ_32M;
		break;
	case 0xf8:
		size = SZ_16M;
		break;
	default:
		return -ENODEV;
	}
	cdev->vram_size = min_t(resource_size_t, size,
				 pci_resource_len(cdev->pdev, 2));
	return 0;
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

static int chrome9_build_prime(struct chrome9_engine *engine, u8 slot,
			       u32 *pause)
{
	u32 *base = engine->ring_cpu + slot * PAGE_SIZE;
	u32 *p;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
	return 0;
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
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 *base = engine->ring_cpu + job->slot * PAGE_SIZE;
	u32 *body;
	u32 *p;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
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
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(&p, cookie, ARRAY_SIZE(cookie) / 2);
	if ((u8 *)p - (u8 *)base > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
	return 0;
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

static int chrome9_build_triangle(struct chrome9_engine *engine,
				  const struct chrome9_draw_request *request,
				  struct chrome9_job *job, u32 target,
				  u32 *pause)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 *base = engine->ring_cpu + job->slot * PAGE_SIZE;
	u32 *body;
	u32 *p;
	unsigned int i;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188 || body[6] != 0x52980004 ||
	    body[49] != 0x23000f02 || body[50] != 0x24000004 ||
	    body[160] != 0xfe020440 || body[163] != 0xcc000000)
		return -EINVAL;
	body[6] = 0x52980000 | (request->pitch >> 5);
	body[49] = 0x23004f02;
	body[50] = 0x24000003;
	body[100] = 0x80000000 | request->height;
	body[101] = 0x81000000 | request->width;
	body[102] = 0x82000000 | request->height;
	body[103] = 0x83000000 | request->width;
	for (i = 0; i < ARRAY_SIZE(request->vertices); i++) {
		u32 *vertex = body + 164 + i * 6;

		vertex[0] = chrome9_q16_to_float(request->vertices[i].x_16_16);
		vertex[1] = chrome9_q16_to_float(request->vertices[i].y_16_16);
		vertex[2] = 0;
		vertex[3] = 0x3f800000;
		vertex[4] = request->vertices[i].color;
		vertex[5] = 0xffffffff;
	}
	body[182] = 0xcc000000;
	body[183] = 0xcc000000;
	p = body + 184;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(&p, cookie, ARRAY_SIZE(cookie) / 2);
	if ((u8 *)p - (u8 *)base > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
	return 0;
}

static int chrome9_build_textured_triangle(struct chrome9_engine *engine,
		const struct chrome9_draw_request *request,
		struct chrome9_job *job, u32 target, u32 texture, u32 *pause)
{
	static const u32 destination_tail[] = {
		0x38000000, 0x3b000000, 0x39000000, 0x3c000000,
		0x34000150, 0x35000114, 0x36000500, 0x37000200,
	};
	static const u32 config[] = {
		0x00000011, 0x01002521, 0x08010101, 0x05000000,
		0x0d810000,
	};
	static const u32 raster_fvf[] = {
		0x00000308, 0x010000c2, 0x020041d0, 0x030052d0,
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
		0x90018000, 0x91000001, 0x92010002, 0x93000001,
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
		0x0140b6d0, 0x30040053, 0x00f20005, 0x0000007f,
		0x01c00000, 0x301c0000, 0x00f20105, 0x000d083f,
	};
	static const u32 sync[] = { 0x6c, 0x00800000 };
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 destination_state[3 + ARRAY_SIZE(destination_tail)];
	u32 texture_state[6];
	u32 sampler[4];
	u32 common[] = {
		0x860000ff, 0x870000ff, 0x53010c0f, 0x00000011,
		0x01c01540, 0x88001ed0, 0x89000800,
	};
	u32 clip[] = {
		0x80000000 | request->height,
		0x81000000 | request->width,
		0x82000000 | request->height,
		0x83000000 | request->width,
	};
	u32 vertices[3 * 8];
	u32 *base = engine->ring_cpu + job->slot * PAGE_SIZE;
	u32 *body;
	u32 *p;
	unsigned int i;
	u32 log_size = 0;

	destination_state[0] = 0x50000000 | (target >> 8);
	destination_state[1] = 0x51100000;
	destination_state[2] = 0x52980000 | (request->pitch >> 5);
	memcpy(destination_state + 3, destination_tail,
	       sizeof(destination_tail));
	if (is_power_of_2(request->texture_width) &&
	    is_power_of_2(request->texture_height))
		log_size = BIT(15) | ilog2(request->texture_height) << 4 |
			   ilog2(request->texture_width);
	texture_state[0] = texture >> 8;
	texture_state[1] = 0x18001101;
	texture_state[2] = 0x20000000 | request->texture_height << 12 |
			   request->texture_width;
	texture_state[3] = 0x21000001;
	texture_state[4] = 0x22000000 | log_size;
	texture_state[5] = 0x30000000 |
		(request->texture_format == DRM_CHROME9_TEXTURE_FORMAT_BGRA8888 ?
		 0x00990000 : 0x00980000);
	/* Explicitly select level zero; do not inherit stale mip offsets. */
	sampler[0] = 0x30000000;
	sampler[1] = 0x31100000 | request->mag_filter << 13 |
		     request->min_filter << 10 | request->mag_filter << 7 |
		     request->min_filter << 4;
	sampler[2] = 0x32000040 | request->wrap_t << 3 | request->wrap_s;
	sampler[3] = 0xcccccccc;
	if (request->texture_program == DRM_CHROME9_TEXTURE_PROGRAM_MODULATE)
		common[4] = 0x01ce1540;
	for (i = 0; i < ARRAY_SIZE(request->textured_vertices); i++) {
		const struct drm_chrome9_textured_vertex *src =
			&request->textured_vertices[i];
		u32 *dst = vertices + i * 8;

		dst[0] = chrome9_q16_to_float(src->x_16_16);
		dst[1] = chrome9_q16_to_float(src->y_16_16);
		dst[2] = chrome9_q16_to_float(src->z_16_16);
		dst[3] = src->rhw_float;
		dst[4] = src->color;
		dst[5] = src->specular;
		dst[6] = src->s_float;
		dst[7] = src->t_float;
	}

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     destination_state, ARRAY_SIZE(destination_state));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00020000,
			     texture_state, ARRAY_SIZE(texture_state));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x20020000,
			     sampler, ARRAY_SIZE(sampler));
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     common, ARRAY_SIZE(common));
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
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00000000,
			     vertices, ARRAY_SIZE(vertices));
	if (p - body !=
	    (request->texture_program == DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ?
	     228 : 224) || body[4] != destination_state[0] ||
	    body[5] != 0x51100000 || body[6] != destination_state[2] ||
	    body[20] != texture_state[0] || body[21] != 0x18001101 ||
	    body[32] != 0x30000000 || body[33] != sampler[1] ||
	    body[34] != sampler[2] ||
	    body[request->texture_program ==
		 DRM_CHROME9_TEXTURE_PROGRAM_MODULATE ? 200 : 196] !=
		0xfe020440)
		return -EINVAL;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(&p, cookie, ARRAY_SIZE(cookie) / 2);
	if ((u8 *)p - (u8 *)base > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
	return 0;
}

static int chrome9_build_depth_triangle(struct chrome9_engine *engine,
		const struct chrome9_draw_request *request,
		struct chrome9_job *job, u32 target, u32 depth, u32 *pause)
{
	static const u32 sync[] = { 0x6c, 0x00800000 };
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 *base = engine->ring_cpu + job->slot * PAGE_SIZE;
	u32 *body;
	u32 *p;
	unsigned int i;

	memset(base, 0xcc, PAGE_SIZE);
	p = base;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	body = PTR_ALIGN(p, 16);
	chrome9_emit_3d_quad_h6(&p, target, true);
	if (p - body != 188 || body[6] != 0x52980004 ||
	    body[19] != 0x01c01540 || body[24] != 0xfe020440 ||
	    body[49] != 0x23000f02 || body[50] != 0x24000004 ||
	    body[160] != 0xfe020440 || body[163] != 0xcc000000)
		return -EINVAL;

	/* Add the D16 words and the wide-open near/far clip state. */
	memmove(body + 28, body + 24, (188 - 24) * sizeof(*body));
	body[6] = 0x52980000 | (request->pitch >> 5);
	body[19] = 0x01c0154c;
	body[22] = 0x10000000 | (depth >> 8);
	body[23] = 0x11000001 | ((request->depth_pitch >> 5) << 13);
	body[24] = 0x12380000;
	body[25] = 0x13010000;
	body[26] = 0x20007fff;
	body[27] = 0xcc000000;
	body[53] = 0x23004f02;
	body[54] = 0x24000003;
	body[104] = 0x80000000 | request->height;
	body[105] = 0x81000000 | request->width;
	body[106] = 0x82000000 | request->height;
	body[107] = 0x83000000 | request->width;
	for (i = 0; i < ARRAY_SIZE(request->depth_vertices); i++) {
		u32 *vertex = body + 168 + i * 6;

		vertex[0] = chrome9_q16_to_float(request->depth_vertices[i].x_16_16);
		vertex[1] = chrome9_q16_to_float(request->depth_vertices[i].y_16_16);
		vertex[2] = chrome9_q16_to_float(request->depth_vertices[i].z_16_16);
		vertex[3] = 0x3f800000;
		vertex[4] = request->depth_vertices[i].color;
		vertex[5] = 0xffffffff;
	}
	body[186] = 0xcc000000;
	body[187] = 0xcc000000;
	p = body + 188;
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(&p, cookie, ARRAY_SIZE(cookie) / 2);
	if ((u8 *)p - (u8 *)base > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
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
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 *base = engine->ring_cpu + job->slot * PAGE_SIZE;
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
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(&p, cookie, ARRAY_SIZE(cookie) / 2);
	if ((u8 *)p - (u8 *)base > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
	return 0;
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
	static const u32 status[] = {
		0x1c000000, 0x1d000000, 0xcccccccc, 0xcccccccc,
	};
	u32 cookie[] = { CHROME9_2D_REG_DST_POS, job->cookie };
	u32 *base = engine->ring_cpu + job->slot * PAGE_SIZE;
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
	chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
			     status, ARRAY_SIZE(status));
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	chrome9_emit_header0(&p, cookie, ARRAY_SIZE(cookie) / 2);
	if ((u8 *)p - (u8 *)base > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	*pause = (u8 *)p - (u8 *)base - 16;
	if (*pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD)
		return -E2BIG;
	chrome9_cache_flush(base, PAGE_SIZE);
	dma_wmb();
	return 0;
}

static void chrome9_program_ring(struct chrome9_device *cdev, u32 start,
				 u32 end, u32 pause)
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

static int chrome9_gart_set(struct chrome9_engine *engine, u32 first,
			    const u32 *ptes, unsigned int count)
{
	struct chrome9_device *cdev = engine->cdev;
	u8 saved_index;
	u8 saved_6c;
	unsigned int i;
	int ret;

	lockdep_assert_held(&engine->vm_lock);
	if (first >= CHROME9_GART_ENTRIES ||
	    count > CHROME9_GART_ENTRIES - first)
		return -ERANGE;
	ret = vga_get_uninterruptible(cdev->pdev, VGA_RSRC_LEGACY_IO);
	if (ret)
		return ret;
	saved_index = readb(cdev->mmio + CHROME9_SEQ_INDEX);
	ret = chrome9_gart_wait_idle(cdev);
	if (ret)
		goto out_restore_index;
	saved_6c = chrome9_seq_read(cdev, 0x6c);
	chrome9_seq_write(cdev, 0x6c, saved_6c & ~BIT(7));
	for (i = 0; i < count; i++)
		writel(ptes ? ptes[i] : CHROME9_GART_INVALID,
		       engine->gart + (first + i) * sizeof(u32));
	readl(engine->gart + (first + count - 1) * sizeof(u32));
	/* Make every PTE visible before requesting the GTI translation flush. */
	wmb();
	ret = chrome9_gart_flush(cdev);
	chrome9_seq_write(cdev, 0x6c, saved_6c);
out_restore_index:
	writeb(saved_index, cdev->mmio + CHROME9_SEQ_INDEX);
	vga_put(cdev->pdev, VGA_RSRC_LEGACY_IO);
	return ret;
}

static int chrome9_gart_bind_ring(struct chrome9_engine *engine)
{
	struct chrome9_device *cdev = engine->cdev;
	u32 ptes[CHROME9_RING_PAGES];
	u8 programmed_6c;
	unsigned int i;
	int ret;

	ret = vga_get_uninterruptible(cdev->pdev, VGA_RSRC_LEGACY_IO);
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
	chrome9_seq_write(cdev, 0x6a,
			  (chrome9_engine_gart_offset >> 12) & 0xff);
	chrome9_seq_write(cdev, 0x6b,
			  (chrome9_engine_gart_offset >> 20) & 0xff);
	programmed_6c = (engine->saved_6c & ~BIT(7) & ~BIT(0)) |
			((chrome9_engine_gart_offset >> 28) & 1);
	chrome9_seq_write(cdev, 0x6c, programmed_6c);
	chrome9_seq_write(cdev, 0x7b, (engine->saved_7b & 0xf0) | 4);
	for (i = 0; i < CHROME9_GART_ENTRIES; i++)
		writel(CHROME9_GART_INVALID, engine->gart + i * sizeof(u32));
	for (i = 0; i < CHROME9_RING_PAGES; i++) {
		ptes[i] = lower_32_bits((engine->ring_dma + i * PAGE_SIZE) >>
					    PAGE_SHIFT) & 0x3fffffff;
		writel(ptes[i], engine->gart + i * sizeof(u32));
	}
	readl(engine->gart + sizeof(u32));
	/* Publish invalid defaults and ring PTEs before the GTI flush. */
	wmb();
	ret = chrome9_gart_flush(cdev);
	chrome9_seq_write(cdev, 0x6c, programmed_6c | BIT(7));
	if (!ret)
		engine->gart_programmed = true;
out_restore_index:
	writeb(engine->saved_seq_index, cdev->mmio + CHROME9_SEQ_INDEX);
	vga_put(cdev->pdev, VGA_RSRC_LEGACY_IO);
	return ret;
}

static int chrome9_gart_restore(struct chrome9_engine *engine)
{
	struct chrome9_device *cdev = engine->cdev;
	u8 index;
	u8 value;
	int ret;

	ret = vga_get_uninterruptible(cdev->pdev, VGA_RSRC_LEGACY_IO);
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
	ret = chrome9_gart_flush(cdev);
	if (ret)
		goto out;
	chrome9_seq_write(cdev, 0x6a, engine->saved_6a);
	chrome9_seq_write(cdev, 0x6b, engine->saved_6b);
	chrome9_seq_write(cdev, 0x7b, engine->saved_7b);
	chrome9_seq_write(cdev, 0x6c, engine->saved_6c);
out:
	writeb(index, cdev->mmio + CHROME9_SEQ_INDEX);
	vga_put(cdev->pdev, VGA_RSRC_LEGACY_IO);
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
				    unsigned int page_count)
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
				CHROME9_GART_CACHED;
			address += PAGE_SIZE;
			length -= PAGE_SIZE;
		}
	}
	return count == page_count ? 0 : -EINVAL;
}

static int chrome9_collect_ptes(struct chrome9_job *job)
{
	job->ptes = kcalloc(job->page_count, sizeof(*job->ptes), GFP_KERNEL);
	if (!job->ptes)
		return -ENOMEM;
	return chrome9_collect_sgt_ptes(job->sgt, job->ptes,
					job->page_count);
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
	if (flags == DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE)
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

static int chrome9_wait_job(struct chrome9_engine *engine,
			    struct chrome9_job *job)
{
	struct chrome9_device *cdev = engine->cdev;
	u32 value;
	int ret;

	ret = readl_poll_timeout(cdev->mmio + CHROME9_2D_REG_DST_POS, value,
				 value == job->cookie, 10,
				 CHROME9_HW_TIMEOUT_US);
	if (ret)
		return ret;
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, value,
				 value & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (ret)
		return ret;
	return readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
				 value, !(value & CHROME9_ENGINE_WAIT_MASK), 10,
				 CHROME9_HW_TIMEOUT_US);
}

bool chrome9_engine_ready(const struct chrome9_device *cdev)
{
	struct chrome9_engine *engine = READ_ONCE(cdev->engine);

	return engine && READ_ONCE(engine->ready) &&
	       !READ_ONCE(engine->wedged);
}

int chrome9_engine_init(struct chrome9_device *cdev)
{
	struct chrome9_engine *engine;
	u32 pause;
	u32 status;
	u16 command;
	int ret;

	if (!chrome9_engine_enable)
		return 0;
#ifndef CONFIG_X86
	return -EOPNOTSUPP;
#endif
	if (chrome9_engine_gart_offset == ULONG_MAX ||
	    !IS_ALIGNED(chrome9_engine_gart_offset, PAGE_SIZE))
		return dev_err_probe(&cdev->pdev->dev, -EINVAL,
			"engine requires a page-aligned engine_gart_offset\n");
	/* Diagnostics may already have mapped either BAR through pcim_iomap(). */
	cdev->mmio = chrome9_iomap_bar_once(cdev->pdev, 1);
	cdev->vram = chrome9_iomap_bar_once(cdev->pdev, 2);
	if (!cdev->mmio || !cdev->vram)
		return -ENOMEM;
	ret = chrome9_detect_vram_size(cdev);
	if (ret)
		return ret;
	if (cdev->vram_size < CHROME9_GART_SIZE ||
	    chrome9_engine_gart_offset != cdev->vram_size - CHROME9_GART_SIZE)
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"engine GART must be the vendor-defined final 64 KiB of VRAM\n");

	engine = kzalloc_obj(*engine);
	if (!engine)
		return -ENOMEM;
	engine->cdev = cdev;
	mutex_init(&engine->submit_lock);
	mutex_init(&engine->vm_lock);
	INIT_LIST_HEAD(&engine->quarantine);
	engine->fence_context = dma_fence_context_alloc(1);
	atomic64_set(&engine->fence_seqno, 0);
	engine->gart = cdev->vram + chrome9_engine_gart_offset;
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
	ret = chrome9_build_prime(engine, 0, &pause);
	if (ret)
		goto err_restore_gart;
	pci_read_config_word(cdev->pdev, PCI_COMMAND, &command);
	engine->was_master = command & PCI_COMMAND_MASTER;
	pci_set_master(cdev->pdev);
	engine->ring_published = true;
	chrome9_program_ring(cdev, 1, PAGE_SIZE, pause);
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, status,
				 status & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
					 status,
					 !(status & CHROME9_ENGINE_WAIT_MASK),
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
	engine->active_slot = 0;
	engine->last_cookie = readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
	/* Persistent-engine mode is deliberately reboot-scoped during bring-up. */
	__module_get(THIS_MODULE);
	engine->module_hold = true;
	engine->ready = true;
	dev_info(&cdev->pdev->dev,
		 "persistent SF engine ready: GART=%#lx ring=%pad\n",
		 chrome9_engine_gart_offset, &engine->ring_dma);
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
	dma_free_coherent(&cdev->pdev->dev, CHROME9_RING_SIZE,
			  engine->ring_cpu, engine->ring_dma);
err_free_saved:
	kvfree(engine->gart_saved);
err_free_engine:
	kfree(engine);
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
	int dev_idx;
	int ret;

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
	job->page_count = obj->size >> PAGE_SHIFT;
	job->sgt = drm_gem_shmem_get_pages_sgt(job->destination);
	if (IS_ERR(job->sgt)) {
		ret = PTR_ERR(job->sgt);
		goto out_free_job;
	}
	ret = chrome9_collect_ptes(job);
	if (ret)
		goto out_free_job;
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
	job->slot = engine->active_slot ^ 1;
	job->cookie = engine->last_cookie == 0x00010001 ?
		      0x00020002 : 0x00010001;
	mutex_lock(&engine->vm_lock);
	ret = drm_mm_insert_node_generic(&engine->sf_mm, &job->sf_node,
					 request->object_size, PAGE_SIZE, 0,
					 DRM_MM_INSERT_LOW);
	if (ret) {
		mutex_unlock(&engine->vm_lock);
		goto out_unlock_submit;
	}
	job->node_allocated = true;
	job->sf_bound = true;
	ret = chrome9_gart_set(engine, job->sf_node.start >> PAGE_SHIFT,
			       job->ptes, job->page_count);
	mutex_unlock(&engine->vm_lock);
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
	} else {
		chrome9_prepare_surface_guards(job);
	}
	chrome9_cache_flush(job->map.vaddr, request->object_size);
	dma_sync_sgtable_for_device(&cdev->pdev->dev, job->sgt,
				    DMA_BIDIRECTIONAL);
	dma_resv_add_fence(resv, &job->fence.base, DMA_RESV_USAGE_WRITE);
	job->fence_attached = true;
	target = job->sf_node.start + PAGE_SIZE;
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
	job->published = true;
	chrome9_program_ring(cdev, job->slot * PAGE_SIZE | 1,
			     (job->slot + 1) * PAGE_SIZE,
			     job->slot * PAGE_SIZE + pause);
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
	    (request->kind == CHROME9_DRAW_IMMEDIATE_TRIANGLE &&
	     !chrome9_verify_surface_guards(job))) {
		ret = -EIO;
		dma_fence_set_error(&job->fence.base, ret);
	}
	mutex_lock(&engine->vm_lock);
	if (chrome9_gart_set(engine, job->sf_node.start >> PAGE_SHIFT,
			     NULL, job->page_count)) {
		if (!ret)
			ret = -EIO;
		dma_fence_set_error(&job->fence.base, -EIO);
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
	} else {
		job->sf_bound = false;
		drm_mm_remove_node(&job->sf_node);
		job->node_allocated = false;
		engine->active_slot = job->slot;
		engine->last_cookie = job->cookie;
	}
	mutex_unlock(&engine->vm_lock);
	if (quarantine) {
		signal = true;
		goto out_unlock_submit;
	}
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
	if (job->sf_bound) {
		mutex_lock(&engine->vm_lock);
		if (chrome9_gart_set(engine,
				     job->sf_node.start >> PAGE_SHIFT,
				     NULL, job->page_count)) {
			ret = -EIO;
			dma_fence_set_error(&job->fence.base, ret);
			chrome9_quarantine_job_locked(engine, job);
			quarantine = true;
		} else {
			job->sf_bound = false;
			drm_mm_remove_node(&job->sf_node);
			job->node_allocated = false;
		}
		mutex_unlock(&engine->vm_lock);
	}
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
	kfree(job->ptes);
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
	struct drm_gem_object *depth_obj = NULL;
	struct drm_gem_object *obj = NULL;
	struct chrome9_engine *engine;
	struct chrome9_job *job = NULL;
	struct drm_exec exec;
	u32 pause;
	u32 target;
	u32 depth_target;
	long wait;
	bool exec_initialized = false;
	bool device_owned = false;
	bool signal = false;
	bool quarantine = false;
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
	if (!obj || !depth_obj) {
		ret = -ENOENT;
		goto out_put_objects;
	}
	if (obj == depth_obj || obj->dev != drm || depth_obj->dev != drm ||
	    drm_gem_is_imported(obj) || drm_gem_is_imported(depth_obj) ||
	    obj->size != request->object_size ||
	    depth_obj->size != request->depth_object_size) {
		ret = -EINVAL;
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
	job->destination_page_count = obj->size >> PAGE_SHIFT;
	job->depth_page_count = depth_obj->size >> PAGE_SHIFT;
	job->page_count = job->destination_page_count + job->depth_page_count;
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
	job->ptes = kcalloc(job->page_count, sizeof(*job->ptes), GFP_KERNEL);
	if (!job->ptes) {
		ret = -ENOMEM;
		goto out_free_job;
	}
	ret = chrome9_collect_sgt_ptes(job->sgt, job->ptes,
					 job->destination_page_count);
	if (ret)
		goto out_free_job;
	ret = chrome9_collect_sgt_ptes(job->depth_sgt,
					 job->ptes + job->destination_page_count,
					 job->depth_page_count);
	if (ret)
		goto out_free_job;

	spin_lock_init(&job->fence.lock);
	dma_fence_init64(&job->fence.base, &chrome9_fence_ops,
			 &job->fence.lock, engine->fence_context,
			 atomic64_inc_return(&engine->fence_seqno));
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 2);
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
	}
	wait = dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_WRITE, true,
				     CHROME9_DEP_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_exec;
	}
	wait = dma_resv_wait_timeout(depth_obj->resv, DMA_RESV_USAGE_WRITE, true,
				     CHROME9_DEP_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_exec;
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

	ret = mutex_lock_interruptible(&engine->submit_lock);
	if (ret)
		goto out_unmap;
	if (engine->stopping || engine->wedged || !engine->ready) {
		ret = -EIO;
		goto out_unlock_submit;
	}
	job->slot = engine->active_slot ^ 1;
	job->cookie = engine->last_cookie == 0x00010001 ?
		      0x00020002 : 0x00010001;
	mutex_lock(&engine->vm_lock);
	ret = drm_mm_insert_node_generic(&engine->sf_mm, &job->sf_node,
					 request->object_size +
					 request->depth_object_size,
					 PAGE_SIZE, 0, DRM_MM_INSERT_LOW);
	if (ret) {
		mutex_unlock(&engine->vm_lock);
		goto out_unlock_submit;
	}
	job->node_allocated = true;
	job->sf_bound = true;
	ret = chrome9_gart_set(engine, job->sf_node.start >> PAGE_SHIFT,
			       job->ptes, job->page_count);
	mutex_unlock(&engine->vm_lock);
	if (ret)
		goto out_prepublication;

	/* Structured BO contents are already CPU-owned on ioctl entry. */
	chrome9_prepare_map_guards(&job->map, request->object_size);
	chrome9_prepare_map_guards(&job->depth_map,
				   request->depth_object_size);
	chrome9_cache_flush(job->map.vaddr, request->object_size);
	chrome9_cache_flush(job->depth_map.vaddr, request->depth_object_size);
	dma_sync_sgtable_for_device(&cdev->pdev->dev, job->sgt,
				    DMA_BIDIRECTIONAL);
	dma_sync_sgtable_for_device(&cdev->pdev->dev, job->depth_sgt,
				    DMA_BIDIRECTIONAL);
	device_owned = true;
	dma_resv_add_fence(obj->resv, &job->fence.base,
			   DMA_RESV_USAGE_WRITE);
	dma_resv_add_fence(depth_obj->resv, &job->fence.base,
			   DMA_RESV_USAGE_WRITE);
	job->fence_attached = true;
	target = job->sf_node.start + PAGE_SIZE;
	depth_target = job->sf_node.start + request->object_size + PAGE_SIZE;
	if (request->kind == CHROME9_DRAW_IMMEDIATE_TEXTURED_TRIANGLE)
		ret = chrome9_build_textured_triangle(engine, request, job, target,
						      depth_target, &pause);
	else
		ret = chrome9_build_depth_triangle(engine, request, job, target,
					   depth_target, &pause);
	if (ret)
		goto out_prepublication;
	job->published = true;
	chrome9_program_ring(cdev, job->slot * PAGE_SIZE | 1,
			     (job->slot + 1) * PAGE_SIZE,
			     job->slot * PAGE_SIZE + pause);
	ret = chrome9_wait_job(engine, job);
	if (ret) {
		dma_fence_set_error(&job->fence.base, -ETIME);
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
		signal = true;
		goto out_unlock_submit;
	}
	job->retirement_proven = true;
	dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->sgt,
				 DMA_BIDIRECTIONAL);
	dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->depth_sgt,
				 DMA_BIDIRECTIONAL);
	chrome9_cache_flush(job->map.vaddr, request->object_size);
	chrome9_cache_flush(job->depth_map.vaddr, request->depth_object_size);
	dma_rmb();
	device_owned = false;
	if (!chrome9_verify_map_guards(&job->map, request->object_size) ||
	    !chrome9_verify_map_guards(&job->depth_map,
				       request->depth_object_size)) {
		ret = -EIO;
		dma_fence_set_error(&job->fence.base, ret);
	}
	mutex_lock(&engine->vm_lock);
	if (chrome9_gart_set(engine, job->sf_node.start >> PAGE_SHIFT,
			     NULL, job->page_count)) {
		if (!ret)
			ret = -EIO;
		dma_fence_set_error(&job->fence.base, -EIO);
		chrome9_quarantine_job_locked(engine, job);
		quarantine = true;
	} else {
		job->sf_bound = false;
		drm_mm_remove_node(&job->sf_node);
		job->node_allocated = false;
		engine->active_slot = job->slot;
		engine->last_cookie = job->cookie;
	}
	mutex_unlock(&engine->vm_lock);
	signal = true;
	goto out_unlock_submit;

out_prepublication:
	dma_fence_set_error(&job->fence.base, ret ?: -EIO);
	if (job->sf_bound) {
		mutex_lock(&engine->vm_lock);
		if (chrome9_gart_set(engine,
				     job->sf_node.start >> PAGE_SHIFT,
				     NULL, job->page_count)) {
			ret = -EIO;
			dma_fence_set_error(&job->fence.base, ret);
			chrome9_quarantine_job_locked(engine, job);
			quarantine = true;
		} else {
			job->sf_bound = false;
			drm_mm_remove_node(&job->sf_node);
			job->node_allocated = false;
		}
		mutex_unlock(&engine->vm_lock);
	}
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
		chrome9_cache_flush(job->map.vaddr, request->object_size);
		chrome9_cache_flush(job->depth_map.vaddr,
				    request->depth_object_size);
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
	drm_gem_object_put(depth_obj);
	drm_gem_object_put(obj);
	dma_fence_put(&job->fence.base);
	goto out_dev_exit;

out_free_job:
	kfree(job->ptes);
	kfree(job);
out_put_objects:
	if (depth_obj)
		drm_gem_object_put(depth_obj);
	if (obj)
		drm_gem_object_put(obj);
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
			       6U * PAGE_SIZE : CHROME9_FIXED_BO_SIZE,
	};

	if ((args->flags != 0 &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_GOURAUD_TRIANGLE &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_CONSTANT_RED &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_DEPTH_ORACLE &&
	     args->flags != DRM_CHROME9_FIXED_QUAD_BLEND_ORACLE) ||
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
	};
	u64 surface_size;
	unsigned int i;

	if (args->flags || args->pad || args->reserved[0] || args->reserved[1] ||
	    !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->pitch != ALIGN(args->width * sizeof(u32), 32))
		return -EINVAL;
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
	return chrome9_submit_draw(drm, file, &request);
}

int chrome9_draw_triangle_d16_ioctl(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct drm_chrome9_draw_triangle_d16 *args = data;
	struct chrome9_draw_request request = {
		.kind = CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16,
		.destination_handle = args->destination_handle,
		.depth_handle = args->depth_handle,
		.width = args->width,
		.height = args->height,
		.pitch = args->destination_pitch,
		.depth_pitch = args->depth_pitch,
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;

	if (args->flags || args->pad || args->reserved[0] ||
	    args->reserved[1] || !args->width || args->width > 2048 ||
	    !args->height || args->height > 2048 ||
	    args->destination_pitch != ALIGN(args->width * sizeof(u32), 32) ||
	    args->depth_pitch != ALIGN(args->width * sizeof(u16), 32))
		return -EINVAL;
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
	return chrome9_submit_depth_triangle(drm, file, &request);
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
	};
	u64 surface_size;
	u64 total_size;
	unsigned int i;

	if (args->flags || args->pad || args->reserved[0] ||
	    args->reserved[1] || !args->width || args->width > 2048 ||
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
	return chrome9_submit_depth_triangle(drm, file, &request);
}

void chrome9_engine_fini(struct chrome9_device *cdev)
{
	struct chrome9_engine *engine = cdev->engine;
	u32 invalid[CHROME9_RING_PAGES] = {
		CHROME9_GART_INVALID, CHROME9_GART_INVALID,
	};
	u32 status;
	int ret;

	if (!engine)
		return;
	mutex_lock(&engine->submit_lock);
	engine->stopping = true;
	engine->ready = false;
	if (engine->wedged || !list_empty(&engine->quarantine)) {
		dev_crit(&cdev->pdev->dev,
			 "refusing unsafe engine teardown; reboot required\n");
		mutex_unlock(&engine->submit_lock);
		return;
	}
	ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS, status,
				 status & BIT(31), 10, CHROME9_HW_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
					 status,
					 !(status & CHROME9_ENGINE_WAIT_MASK),
					 10, CHROME9_HW_TIMEOUT_US);
	if (ret) {
		engine->wedged = true;
		chrome9_take_quarantine_hold(engine);
		dev_crit(&cdev->pdev->dev,
			 "engine did not quiesce; retained for reboot\n");
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
	mutex_unlock(&engine->submit_lock);
	cdev->engine = NULL;
	dma_free_coherent(&cdev->pdev->dev, CHROME9_RING_SIZE,
			  engine->ring_cpu, engine->ring_dma);
	kvfree(engine->gart_saved);
	if (engine->module_hold)
		module_put(THIS_MODULE);
	kfree(engine);
}
