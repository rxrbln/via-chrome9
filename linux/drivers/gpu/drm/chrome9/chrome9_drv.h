/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Chrome9 contributors */
#ifndef __CHROME9_DRV_H__
#define __CHROME9_DRV_H__

#include <linux/iosys-map.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/vgaarb.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_simple_kms_helper.h>

#include <drm/chrome9_drm.h>

#define CHROME9_PCI_VENDOR_ID	0x1106
#define CHROME9_PCI_DEVICE_VX900	0x7122

extern bool chrome9_two_texture_signed_repeat;
extern bool chrome9_copy_rect_preserve_alpha;

#define CHROME9_REG_ENGINE_STATUS	0x0400
#define CHROME9_REG_FIFO_EMPTY		0x0404
#define CHROME9_REG_FIFO_FULL		0x0408
#define CHROME9_REG_ENGINE_ERROR	0x040c
#define CHROME9_REG_FIFO_STATUS		0x0410
#define CHROME9_REG_CR_TRANSSET		0x041c
#define CHROME9_REG_CR_TRANSPACE		0x0420
#define CHROME9_REG_FE_TRANSSET		0x043c
#define CHROME9_REG_FE_TRANSPACE		0x0440
#define CHROME9_REG_CR_STATUS		0x0444
#define CHROME9_REG_CURRENT_FENCE	0x0494

/*
 * VX900 direct scanout-base aliases recovered independently from the Windows
 * KMD (IGA1 0x79683..0x796de, IGA2 0x79d70..0x79de9).  See the full evidence
 * trail in docs/research/display-flip-vblank.md.  Only the kernel KMS path
 * may use these packers to publish a validated, persistently pinned base.
 *
 * IGA1 stores a byte address shifted right by one in bits 28:0 and preserves
 * bits 31:29.  IGA2 stores an eight-byte-aligned byte address in bits 28:3
 * and preserves both bits 31:29 and the non-address bits 2:0.
 */
#define CHROME9_REG_IGA1_BASE		0x0214
#define CHROME9_REG_IGA2_BASE		0x0218
#define CHROME9_REG_DISPLAY_STATUS	0x0200
#define CHROME9_REG_BASE_UPDATE_STATUS	0x0204
#define CHROME9_DISPLAY_IGA1_BLANK	BIT(1)
#define CHROME9_BASE_UPDATE_IGA1_PENDING	BIT(1)
#define CHROME9_IGA1_BASE_KEEP_MASK	GENMASK(31, 29)
#define CHROME9_IGA1_BASE_ADDR_MASK	GENMASK(28, 0)
#define CHROME9_IGA2_BASE_KEEP_MASK	(GENMASK(31, 29) | GENMASK(2, 0))
#define CHROME9_IGA2_BASE_ADDR_MASK	GENMASK(28, 3)
#define CHROME9_IGA1_BASE_MAX		BIT(30)
#define CHROME9_IGA2_BASE_MAX		BIT(29)

/* Disable IGA1 T11 automatic swapping before either direct-base update. */
#define CHROME9_REG_DISPLAY_TRANSSET	0x041c
#define CHROME9_REG_DISPLAY_TRANSPACE	0x0420
#define CHROME9_DISPLAY_TRANSSET_A12	0x00110000
#define CHROME9_DISPLAY_TRANSPACE_T11	0x12000000

/* By-frame (zero) versus by-line (one) direct-flip strategy selectors. */
#define CHROME9_CR_IGA1_FLIP_MODE	0x32
#define CHROME9_CR_IGA1_FLIP_BY_LINE	BIT(0)
#define CHROME9_CR_IGA2_FLIP_MODE	0x88
#define CHROME9_CR_IGA2_FLIP_BY_LINE	BIT(5)

static inline bool chrome9_iga1_base_valid(u32 byte_address)
{
	return !(byte_address & 1) && byte_address < CHROME9_IGA1_BASE_MAX;
}

static inline bool chrome9_iga2_base_valid(u32 byte_address)
{
	return !(byte_address & 7) && byte_address < CHROME9_IGA2_BASE_MAX;
}

static inline u32 chrome9_iga1_base_pack(u32 old, u32 byte_address)
{
	return (old & CHROME9_IGA1_BASE_KEEP_MASK) |
	       ((byte_address >> 1) & CHROME9_IGA1_BASE_ADDR_MASK);
}

static inline u32 chrome9_iga2_base_pack(u32 old, u32 byte_address)
{
	return (old & CHROME9_IGA2_BASE_KEEP_MASK) |
	       (byte_address & CHROME9_IGA2_BASE_ADDR_MASK);
}

#define CHROME9_ENGINE_WAIT_MASK		0x00101ff3
/* CR bit 4 may remain set while the owned reader is safely at PAUSE. */
#define CHROME9_ENGINE_WORK_MASK		0x00101fe3

#define CHROME9_2D_REG_COMMAND		0x0000
#define CHROME9_2D_REG_MODE		0x0004
#define CHROME9_2D_REG_PITCH		0x0008
#define CHROME9_2D_REG_DIMENSION	0x000c
#define CHROME9_2D_REG_DST_POS		0x0010
#define CHROME9_2D_REG_DST_BASE		0x0014
/* H6/VX900 solid-color register; older Chrome9 paths use 0x4c. */
#define CHROME9_2D_REG_FG_COLOR		0x0058

struct drm_file;
struct drm_format_info;
struct drm_framebuffer;
struct drm_gem_object;
struct drm_mode_create_dumb;
struct drm_mode_fb_cmd2;
struct chrome9_engine;
struct chrome9_kms_state;

struct chrome9_device {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *mmio;
	void __iomem *mmio_alt;
	void __iomem *vram;
	resource_size_t vram_size;
	u64 sl_base;
	resource_size_t scanout_offset;
	resource_size_t scanout_size;
	/* vgaarb permits nested same-device users; serialize SEQ index/data. */
	struct mutex seq_lock;
	/* Serializes CPU copies into the single BAR2 staging interval. */
	struct mutex scanout_lock;
	struct chrome9_kms_state *kms_state;
	struct chrome9_engine *engine;
	bool kms_shutdown;
	struct drm_simple_display_pipe display_pipe;
	struct drm_plane cursor_plane;
	struct drm_connector display_connector;
};

static inline struct chrome9_device *to_chrome9(struct drm_device *drm)
{
	return container_of(drm, struct chrome9_device, drm);
}

static inline int chrome9_vga_seq_get(struct chrome9_device *cdev)
{
	int ret = vga_get_uninterruptible(cdev->pdev, VGA_RSRC_LEGACY_IO);

	if (!ret)
		mutex_lock(&cdev->seq_lock);
	return ret;
}

static inline void chrome9_vga_seq_put(struct chrome9_device *cdev)
{
	lockdep_assert_held(&cdev->seq_lock);
	mutex_unlock(&cdev->seq_lock);
	vga_put(cdev->pdev, VGA_RSRC_LEGACY_IO);
}

int chrome9_detect_vram(struct chrome9_device *cdev);
bool chrome9_dma32_scanout_supported(struct chrome9_device *cdev);

/* Every chrome9 GEM object: shmem plus CPU cache coherence state, all
 * protected by the object's reservation lock. */
struct chrome9_bo {
	struct drm_gem_shmem_object base;
	/* Userspace brackets its CPU accesses with GEM_CPU_ACCESS. */
	bool explicit_access;
	/* CPU caches may hold dirty lines: flush before device use. */
	bool cpu_dirty;
	/* The device wrote since the last CPU invalidate. */
	bool device_written;
	/* Kernel mapping kept from the first job until the object is freed. */
	struct iosys_map kmap;
};

void chrome9_bo_hold_kmap(struct chrome9_bo *bo);

static inline struct chrome9_bo *to_chrome9_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct chrome9_bo, base.base);
}

struct drm_gem_object *chrome9_gem_create_object(struct drm_device *drm,
						 size_t size);
int chrome9_gem_cpu_access_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file);
int chrome9_gem_create_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file);
int chrome9_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			    struct drm_mode_create_dumb *args);
struct drm_gem_object *chrome9_gem_cursor_create(struct drm_device *drm);
struct drm_framebuffer *
chrome9_gem_fb_create(struct drm_device *drm, struct drm_file *file,
		      const struct drm_format_info *info,
		      const struct drm_mode_fb_cmd2 *mode_cmd);
int chrome9_gem_copy_fb_to_scanout(struct chrome9_device *cdev,
				   struct drm_framebuffer *fb);
int chrome9_gem_fb_pin(struct drm_framebuffer *fb);
void chrome9_gem_fb_unpin(struct drm_framebuffer *fb);
int chrome9_gem_fb_sf_base(struct drm_framebuffer *fb, u32 *base);
void chrome9_gem_fb_sync_for_device(struct drm_framebuffer *fb);
int chrome9_kms_init(struct chrome9_device *cdev,
		     unsigned long scanout_offset,
		     unsigned long scanout_size, bool fixed_iga1,
		     bool staging_scanout);
int chrome9_kms_fini(struct chrome9_device *cdev);

/* Live-verified H5S2VP1/H6 sequences shared with the fixed engine path. */
void chrome9_init_2d_h6(struct chrome9_device *cdev);
void chrome9_init_3d_h5s2vp1(struct chrome9_device *cdev);
void chrome9_emit_3d_quad_h6(u32 **cursor, unsigned long target,
			    bool system_framebuffer);

/*
 * Replacement VS state for the proven quad: compiled program, internal
 * rows 256.., optional user constants c0.., A60/A62 and raw vertices.
 */
struct chrome9_quad_vs {
	const u32 *internal;
	unsigned int internal_dwords;
	const u32 *user;
	unsigned int user_dwords;
	const u32 *program;
	unsigned int program_dwords;
	u32 a60;
	u32 a62;
	const u32 *vertices;
	unsigned int vertex_dwords;
	u32 a24;
	bool clip;
	/* Nonzero: 1-KiB aligned SF address of the vertices, fetched by index. */
	u32 vertex_address;
};

void chrome9_emit_3d_quad_h6_vs(u32 **cursor, unsigned long target,
				const struct chrome9_quad_vs *vs);

#endif
