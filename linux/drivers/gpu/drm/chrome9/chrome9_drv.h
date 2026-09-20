/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Chrome9 contributors */
#ifndef __CHROME9_DRV_H__
#define __CHROME9_DRV_H__

#include <linux/pci.h>

#include <drm/drm_device.h>

#include <drm/chrome9_drm.h>

#define CHROME9_PCI_VENDOR_ID	0x1106
#define CHROME9_PCI_DEVICE_VX900	0x7122

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

#define CHROME9_ENGINE_WAIT_MASK		0x00101ff3

#define CHROME9_2D_REG_COMMAND		0x0000
#define CHROME9_2D_REG_MODE		0x0004
#define CHROME9_2D_REG_PITCH		0x0008
#define CHROME9_2D_REG_DIMENSION	0x000c
#define CHROME9_2D_REG_DST_POS		0x0010
#define CHROME9_2D_REG_DST_BASE		0x0014
/* H6/VX900 solid-color register; older Chrome9 paths use 0x4c. */
#define CHROME9_2D_REG_FG_COLOR		0x0058

struct drm_file;
struct chrome9_engine;

struct chrome9_device {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *mmio;
	void __iomem *mmio_alt;
	void __iomem *vram;
	resource_size_t vram_size;
	u64 sl_base;
	struct chrome9_engine *engine;
};

static inline struct chrome9_device *to_chrome9(struct drm_device *drm)
{
	return container_of(drm, struct chrome9_device, drm);
}

int chrome9_gem_create_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file);

/* Live-verified H5S2VP1/H6 sequences shared with the fixed engine path. */
void chrome9_init_2d_h6(struct chrome9_device *cdev);
void chrome9_init_3d_h5s2vp1(struct chrome9_device *cdev);
void chrome9_emit_3d_quad_h6(u32 **cursor, unsigned long target,
			    bool system_framebuffer);

#endif
