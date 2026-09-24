// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <linux/module.h>
#include <linux/aperture.h>
#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/iopoll.h>
#include <linux/dma-mapping.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/swiotlb.h>
#include <linux/vgaarb.h>

#include <video/vga.h>

#ifdef CONFIG_X86
#include <asm/cacheflush.h>
#endif

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_drv.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>

#include "chrome9_drv.h"
#include "chrome9_engine.h"

#define CHROME9_DRIVER_NAME	"chrome9"
#define CHROME9_DRIVER_DESC	"VIA Chrome9 HD clean-room render driver"

static bool chrome9_enable;
module_param_named(enable, chrome9_enable, bool, 0400);
MODULE_PARM_DESC(enable,
		 "Bind the experimental Chrome9 DRM driver (default: false)");

static bool chrome9_kms;
module_param_named(kms, chrome9_kms, bool, 0400);
MODULE_PARM_DESC(kms,
		 "Expose the Chrome9 DRM/KMS device (default: false)");

static unsigned long chrome9_scanout_offset = ULONG_MAX;
module_param_named(scanout_offset, chrome9_scanout_offset, ulong, 0400);
MODULE_PARM_DESC(scanout_offset,
		 "Operator-reserved BAR2 scanout carveout byte offset");

static unsigned long chrome9_scanout_size;
module_param_named(scanout_size, chrome9_scanout_size, ulong, 0400);
MODULE_PARM_DESC(scanout_size,
		 "Operator-reserved BAR2 scanout carveout size (default: disabled)");

static bool chrome9_fixed_iga1;
module_param_named(fixed_iga1, chrome9_fixed_iga1, bool, 0400);
MODULE_PARM_DESC(fixed_iga1,
		 "Enable native VX900 IGA1 VGA modesetting (compatibility option name)");

bool chrome9_two_texture_signed_repeat;
module_param_named(two_texture_signed_repeat,
		   chrome9_two_texture_signed_repeat, bool, 0400);
MODULE_PARM_DESC(two_texture_signed_repeat,
		 "Expose the bounded two-texture signed REPEAT path (default: false)");

bool chrome9_copy_rect_preserve_alpha;
module_param_named(copy_rect_preserve_alpha,
		   chrome9_copy_rect_preserve_alpha, bool, 0400);
MODULE_PARM_DESC(copy_rect_preserve_alpha,
		 "Expose the silicon-gated one-pass RGB copy preserving A/X");

static bool chrome9_dma32_scanout;
module_param_named(dma32_scanout, chrome9_dma32_scanout, bool, 0400);
MODULE_PARM_DESC(dma32_scanout,
		 "Allocate explicitly flagged scanout BOs below 4 GiB without SWIOTLB");

bool chrome9_dma32_scanout_supported(struct chrome9_device *cdev)
{
	return chrome9_dma32_scanout && IS_ENABLED(CONFIG_ZONE_DMA32) &&
	       dma_get_mask(&cdev->pdev->dev) == DMA_BIT_MASK(32) &&
	       !is_swiotlb_force_bounce(&cdev->pdev->dev);
}

static bool chrome9_staging_scanout;
module_param_named(staging_scanout, chrome9_staging_scanout, bool, 0400);
MODULE_PARM_DESC(staging_scanout,
		 "Use the legacy BAR2 copy path instead of direct SF scanout");

static bool chrome9_mmio_probe;
module_param_named(mmio_probe, chrome9_mmio_probe, bool, 0400);
MODULE_PARM_DESC(mmio_probe,
		 "Read BAR0/BAR1 status candidates once (default: false)");

static bool chrome9_recover_2d;
module_param_named(recover_2d, chrome9_recover_2d, bool, 0400);
MODULE_PARM_DESC(recover_2d,
		 "Pulse the documented SR4E 2D-engine reset (default: false)");

static unsigned int chrome9_engine_bar = 1;
module_param_named(engine_bar, chrome9_engine_bar, uint, 0400);
MODULE_PARM_DESC(engine_bar,
		 "BAR used for engine writes during explicit tests (default: 1)");

static unsigned int chrome9_dma_bits = 32;
module_param_named(dma_bits, chrome9_dma_bits, uint, 0400);
MODULE_PARM_DESC(dma_bits,
		 "DMA address width for bounded SF reachability tests (24..32)");

static bool chrome9_selftest_cr;
module_param_named(selftest_cr, chrome9_selftest_cr, bool, 0400);
MODULE_PARM_DESC(selftest_cr,
		 "Run the exact Windows CR stop-stream bring-up test");

static unsigned long chrome9_selftest_cr_sl_offset = ULONG_MAX;
module_param_named(selftest_cr_sl_offset, chrome9_selftest_cr_sl_offset,
		   ulong, 0400);
MODULE_PARM_DESC(selftest_cr_sl_offset,
		 "Run the CR stop-stream test from this BAR2/SL byte offset");

static bool chrome9_selftest_cr_observe_2d;
module_param_named(selftest_cr_observe_2d, chrome9_selftest_cr_observe_2d,
		   bool, 0400);
MODULE_PARM_DESC(selftest_cr_observe_2d,
		 "Prepend safe 2D latch writes to make CR fetch observable");

static bool chrome9_selftest_cr_observe_3d;
module_param_named(selftest_cr_observe_3d, chrome9_selftest_cr_observe_3d,
		   bool, 0400);
MODULE_PARM_DESC(selftest_cr_observe_3d,
		 "Emit the address-free H5/H6 3D status-update packet");

static bool chrome9_selftest_init_3d;
module_param_named(selftest_init_3d, chrome9_selftest_init_3d, bool, 0400);
MODULE_PARM_DESC(selftest_init_3d,
		 "Run the vendor H5S2VP1 address-free 3D initialization sweep");

static bool chrome9_selftest_3d_quad;
module_param_named(selftest_3d_quad, chrome9_selftest_3d_quad, bool, 0400);
MODULE_PARM_DESC(selftest_3d_quad,
		 "Draw and verify the exact recovered H6 untextured 32x32 quad");

static bool chrome9_selftest_3d_texture;
module_param_named(selftest_3d_texture, chrome9_selftest_3d_texture, bool,
		   0400);
MODULE_PARM_DESC(selftest_3d_texture,
		 "Draw and verify the exact recovered H6 one-texture 32x32 quad");

static bool chrome9_selftest_3d_sf;
module_param_named(selftest_3d_sf, chrome9_selftest_3d_sf, bool, 0400);
MODULE_PARM_DESC(selftest_3d_sf,
		 "Put the guarded 3D target in translated system-framebuffer memory");

static bool chrome9_selftest_3d_shmem;
module_param_named(selftest_3d_shmem, chrome9_selftest_3d_shmem, bool, 0400);
MODULE_PARM_DESC(selftest_3d_shmem,
		 "Back the guarded system-framebuffer target with GEM shmem pages");

static bool chrome9_selftest_3d_shmem_cached;
module_param_named(selftest_3d_shmem_cached,
		   chrome9_selftest_3d_shmem_cached, bool, 0400);
MODULE_PARM_DESC(selftest_3d_shmem_cached,
		 "Use cached GEM shmem with the recovered RTSF PTE cache attribute");

static bool chrome9_selftest_3d_dma_gem;
module_param_named(selftest_3d_dma_gem, chrome9_selftest_3d_dma_gem, bool,
		   0400);
MODULE_PARM_DESC(selftest_3d_dma_gem,
		 "Back the guarded system-framebuffer target with a DMA GEM object");

static unsigned int chrome9_selftest_3d_store;
module_param_named(selftest_3d_store, chrome9_selftest_3d_store, uint, 0400);
MODULE_PARM_DESC(selftest_3d_store,
		 "3D store probe: 1=PS, 2=VS NOP, 3=VS full, 4=PS control+store, 5=VS full+valid, 6=H6 linkage/constants+VS, 7=full PS c0..c31 draw");

static bool chrome9_selftest_cr_sl_physical;
module_param_named(selftest_cr_sl_physical,
		   chrome9_selftest_cr_sl_physical, bool, 0400);
MODULE_PARM_DESC(selftest_cr_sl_physical,
		 "Add the decoded SL physical base to the CR ring address");

static bool chrome9_selftest_cr_pcie;
module_param_named(selftest_cr_pcie, chrome9_selftest_cr_pcie, bool, 0400);
MODULE_PARM_DESC(selftest_cr_pcie,
		 "Run the vendor-derived VX900 PCIe GART/CR pause test");

static unsigned long chrome9_selftest_gart_offset = ULONG_MAX;
module_param_named(selftest_gart_offset, chrome9_selftest_gart_offset,
		   ulong, 0400);
MODULE_PARM_DESC(selftest_gart_offset,
		 "BAR2 byte offset of a disposable 64-KiB GART table");

static unsigned long chrome9_selftest_2d_offset = ULONG_MAX;
module_param_named(selftest_2d_offset, chrome9_selftest_2d_offset, ulong, 0400);
MODULE_PARM_DESC(selftest_2d_offset,
		 "Run destructive-then-restored 2D fill at this BAR2 byte offset");

#define CHROME9_SELFTEST_WIDTH		32U
#define CHROME9_SELFTEST_HEIGHT		32U
#define CHROME9_SELFTEST_PITCH		(CHROME9_SELFTEST_WIDTH * 4U)
#define CHROME9_SELFTEST_SIZE		(CHROME9_SELFTEST_PITCH * \
					 CHROME9_SELFTEST_HEIGHT)
#define CHROME9_SELFTEST_GUARD		64U
#define CHROME9_SELFTEST_SENTINEL	0xdeadbeefU
#define CHROME9_SELFTEST_GUARD_WORD	0x13579bdfU
#define CHROME9_SELFTEST_FILL		0x41a7c9e3U
#define CHROME9_2D_MODE_32BPP		(3U << 8)
#define CHROME9_2D_COMMAND_FILL		((0xf0U << 24) | BIT(23) | \
					 BIT(13) | 1U)
#define CHROME9_H1(reg)			(0xf0000000U | ((reg) >> 2))
#define CHROME9_PCIE_GART_SIZE		SZ_64K
#define CHROME9_PCIE_GART_ENTRIES	(CHROME9_PCIE_GART_SIZE / sizeof(u32))
#define CHROME9_CR_READ_AHEAD		0x220U
#define CHROME9_SEQ_INDEX		0x83c4
#define CHROME9_SEQ_DATA		0x83c5

static u32 chrome9_selftest_texture_pixel(unsigned int index)
{
	unsigned int x = index % CHROME9_SELFTEST_WIDTH;
	unsigned int y = index / CHROME9_SELFTEST_WIDTH;

	if (y < CHROME9_SELFTEST_HEIGHT / 2)
		return x < CHROME9_SELFTEST_WIDTH / 2 ?
			0x00ff0000 : 0x0000ff00;
	return x < CHROME9_SELFTEST_WIDTH / 2 ?
		0x00ffffff : 0x000000ff;
}

static void chrome9_log_status(struct pci_dev *pdev, unsigned int bar,
			       void __iomem *mmio)
{
	dev_info(&pdev->dev,
		 "BAR%u read-only snapshot: eng=%08x empty=%08x full=%08x err=%08x fifo=%08x cr_set=%08x fe_set=%08x cr_status=%08x fence=%08x\n",
		 bar, readl(mmio + CHROME9_REG_ENGINE_STATUS),
		 readl(mmio + CHROME9_REG_FIFO_EMPTY),
		 readl(mmio + CHROME9_REG_FIFO_FULL),
		 readl(mmio + CHROME9_REG_ENGINE_ERROR),
		 readl(mmio + CHROME9_REG_FIFO_STATUS),
		 readl(mmio + CHROME9_REG_CR_TRANSSET),
		 readl(mmio + CHROME9_REG_FE_TRANSSET),
		 readl(mmio + CHROME9_REG_CR_STATUS),
		 readl(mmio + CHROME9_REG_CURRENT_FENCE));
	dev_info(&pdev->dev,
		 "BAR%u 2D candidate: cmd=%08x mode=%08x pitch=%08x dim=%08x dstxy=%08x dstbase=%08x color=%08x\n",
		 bar, readl(mmio + CHROME9_2D_REG_COMMAND),
		 readl(mmio + CHROME9_2D_REG_MODE),
		 readl(mmio + CHROME9_2D_REG_PITCH),
		 readl(mmio + CHROME9_2D_REG_DIMENSION),
		 readl(mmio + CHROME9_2D_REG_DST_POS),
		 readl(mmio + CHROME9_2D_REG_DST_BASE),
		 readl(mmio + CHROME9_2D_REG_FG_COLOR));
}

static int chrome9_read_status(struct chrome9_device *cdev)
{
	struct pci_dev *pdev = cdev->pdev;
	void __iomem *bar0;
	void __iomem *bar1;

	if (chrome9_engine_bar > 1)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "engine_bar must be zero or one\n");

	if (!chrome9_mmio_probe && !chrome9_recover_2d && !chrome9_selftest_cr &&
	    !chrome9_selftest_cr_pcie &&
	    chrome9_selftest_cr_sl_offset == ULONG_MAX &&
	    chrome9_selftest_2d_offset == ULONG_MAX)
		return 0;
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) <= CHROME9_REG_CURRENT_FENCE ||
	    !(pci_resource_flags(pdev, 1) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 1) <= CHROME9_REG_CURRENT_FENCE)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "BAR0/BAR1 too small for status comparison\n");

	/* The programming manual identifies PCI BAR1 (config 0x14) as MB1. */
	bar0 = pcim_iomap(pdev, 0, 0);
	if (!bar0)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "cannot map BAR0 for status comparison\n");
	bar1 = pcim_iomap(pdev, 1, 0);
	if (!bar1)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "cannot map BAR1 for status comparison\n");
	cdev->mmio = chrome9_engine_bar ? bar1 : bar0;
	cdev->mmio_alt = chrome9_engine_bar ? bar0 : bar1;

	if (chrome9_mmio_probe) {
		chrome9_log_status(pdev, 0, bar0);
		chrome9_log_status(pdev, 1, bar1);
		dev_info(&pdev->dev, "explicit engine tests use BAR%u\n",
			 chrome9_engine_bar);
	}
	return 0;
}

static int chrome9_recover_2d_engine(struct chrome9_device *cdev)
{
	struct pci_dev *pdev = cdev->pdev;
	u8 saved_index;
	u8 reset;
	int ret;

	if (!chrome9_recover_2d)
		return 0;
	ret = chrome9_vga_seq_get(cdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot lock VGA I/O for 2D recovery\n");
	saved_index = vga_io_r(VGA_SEQ_I);
	reset = vga_io_rseq(0x4e);
	vga_io_wseq(0x4e, (reset & ~GENMASK(3, 2)) | BIT(2));
	usleep_range(10, 20);
	vga_io_wseq(0x4e, reset);
	vga_io_w(VGA_SEQ_I, saved_index);
	chrome9_vga_seq_put(cdev);

	dev_info(&pdev->dev,
		 "pulsed documented SR4E 2D engine reset: old=%02x status=%08x\n",
		 reset, readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS));
	return 0;
}

static resource_size_t chrome9_decode_sr14_vram(u8 code)
{
	switch (code) {
	case 0x00:
		return SZ_512M;
	case 0x80:
		return SZ_256M;
	case 0xc0:
		return SZ_128M;
	case 0xe0:
		return SZ_64M;
	case 0xf0:
		return SZ_32M;
	case 0xf8:
		return SZ_16M;
	default:
		return 0;
	}
}

static resource_size_t chrome9_decode_vx900_vram(u32 config)
{
	u32 code = FIELD_GET(GENMASK(14, 12), config);

	if (!(config & BIT(15)) || !code || code > 7)
		return 0;
	return (resource_size_t)SZ_4M << code;
}

static resource_size_t chrome9_decode_sr68_vram(u8 code)
{
	switch (code) {
	case 0x00:
		return SZ_512M;
	case 0x80:
		return SZ_256M;
	case 0xc0:
		return SZ_128M;
	case 0xe0:
		return SZ_64M;
	case 0xf0:
		return SZ_32M;
	case 0xf8:
		return SZ_16M;
	case 0xfc:
		return SZ_8M;
	case 0xfe:
		return SZ_4M;
	case 0xff:
		return SZ_2M;
	default:
		return 0;
	}
}

static resource_size_t chrome9_decode_pci_b2_vram(u8 code)
{
	switch (code) {
	case 0x00:
		return SZ_512M;
	case 0x40:
		return SZ_256M;
	case 0x60:
		return SZ_128M;
	case 0x70:
		return SZ_64M;
	case 0x78:
		return SZ_32M;
	case 0x7c:
		return SZ_16M;
	case 0x7e:
		return SZ_8M;
	case 0x7f:
		return SZ_4M;
	default:
		return 0;
	}
}

int chrome9_detect_vram(struct chrome9_device *cdev)
{
	struct pci_dev *pdev = cdev->pdev;
	struct pci_dev *memory;
	resource_size_t aperture_size;
	resource_size_t controller_size;
	resource_size_t pci_b2_size;
	resource_size_t sr14_size;
	resource_size_t sr68_size;
	u16 memory_a0;
	u8 pci_b0 = 0xff;
	u8 pci_b2 = 0xff;
	u8 saved_index;
	u8 size_code;
	u8 sl_size_code;
	u8 gti[6];
	u8 clock_control;
	u8 cr_gating;
	u8 gfx_power;
	int ret;

	if (cdev->vram_size)
		return 0;

	memory = pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus),
					     pdev->bus->number,
					     PCI_DEVFN(0, 3));
	if (!memory || memory->vendor != PCI_VENDOR_ID_VIA ||
	    memory->device != 0x3410) {
		pci_dev_put(memory);
		return dev_err_probe(&pdev->dev, -ENODEV,
			"cannot find VX900 DRAM controller at D0F3\n");
	}
	ret = pci_read_config_word(memory, 0xa0, &memory_a0);
	pci_dev_put(memory);
	if (ret)
		return dev_err_probe(&pdev->dev, pcibios_err_to_errno(ret),
			"cannot read VX900 framebuffer size at D0F3 A0\n");
	controller_size = chrome9_decode_vx900_vram(memory_a0);
	if (!controller_size)
		return dev_err_probe(&pdev->dev, -ENODEV,
			"unknown VX900 framebuffer-size code %#x at D0F3 A0\n",
			(u32)FIELD_GET(GENMASK(14, 12), memory_a0));
	aperture_size = pci_resource_len(pdev, 2);
	if (!(pci_resource_flags(pdev, 2) & IORESOURCE_MEM) || !aperture_size)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "BAR2 VRAM aperture is unavailable\n");
	if (aperture_size < controller_size)
		return dev_err_probe(&pdev->dev, -ERANGE,
			"BAR2 aperture %llu is smaller than the %llu-byte VX900 framebuffer\n",
			(unsigned long long)aperture_size,
			(unsigned long long)controller_size);

	ret = chrome9_vga_seq_get(cdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot lock VGA I/O for SR14 read\n");
	saved_index = vga_io_r(VGA_SEQ_I);
	size_code = vga_io_rseq(0x14);
	sl_size_code = vga_io_rseq(0x68);
	gti[0] = vga_io_rseq(0x6a);
	gti[1] = vga_io_rseq(0x6b);
	gti[2] = vga_io_rseq(0x6c);
	gti[3] = vga_io_rseq(0x6d);
	gti[4] = vga_io_rseq(0x6e);
	gti[5] = vga_io_rseq(0x6f);
	clock_control = vga_io_rseq(0x3f);
	cr_gating = vga_io_rseq(0x4f);
	gfx_power = vga_io_rseq(0x59);
	vga_io_w(VGA_SEQ_I, saved_index);
	chrome9_vga_seq_put(cdev);
	ret = pci_read_config_byte(pdev, 0xb0, &pci_b0);
	if (ret)
		dev_warn(&pdev->dev, "cannot read diagnostic PCI B0: %pe\n",
			 ERR_PTR(pcibios_err_to_errno(ret)));
	ret = pci_read_config_byte(pdev, 0xb2, &pci_b2);
	if (ret)
		dev_warn(&pdev->dev, "cannot read diagnostic PCI B2: %pe\n",
			 ERR_PTR(pcibios_err_to_errno(ret)));
	cdev->sl_base = (u64)gti[3] << 21 |
			(u64)gti[4] << 29 |
			(u64)(gti[5] & GENMASK(6, 0)) << 37;
	sr14_size = chrome9_decode_sr14_vram(size_code);
	sr68_size = chrome9_decode_sr68_vram(sl_size_code);
	pci_b2_size = chrome9_decode_pci_b2_vram(pci_b2);
	cdev->vram_size = controller_size;
	dev_info(&pdev->dev,
		 "memory state: D0F3_A0=%04x UMA=%llu BAR2=%llu SR14=%#02x (%llu) SR68=%#02x (%llu) PCI_B0=%#02x PCI_B2=%#02x (%llu)\n",
		 memory_a0, (unsigned long long)controller_size,
		 (unsigned long long)aperture_size,
		 size_code, (unsigned long long)sr14_size,
		 sl_size_code, (unsigned long long)sr68_size,
		 pci_b0, pci_b2, (unsigned long long)pci_b2_size);
	if (aperture_size != controller_size)
		dev_warn(&pdev->dev,
			 "BAR2 aperture %llu differs from VX900 framebuffer size %llu\n",
			 (unsigned long long)aperture_size,
			 (unsigned long long)controller_size);
	if (sr14_size && sr14_size != cdev->vram_size)
		dev_warn(&pdev->dev,
			 "ignoring stale SR14 VRAM size %llu; D0F3/BAR2 give %llu\n",
			 (unsigned long long)sr14_size,
			 (unsigned long long)cdev->vram_size);
	if (sr68_size && sr68_size != cdev->vram_size)
		dev_warn(&pdev->dev,
			 "SR68 VRAM size %llu disagrees with D0F3 size %llu\n",
			 (unsigned long long)sr68_size,
			 (unsigned long long)cdev->vram_size);
	if (pci_b2_size && pci_b2_size != cdev->vram_size)
		dev_warn(&pdev->dev,
			 "PCI B2 VRAM size %llu disagrees with D0F3 size %llu\n",
			 (unsigned long long)pci_b2_size,
			 (unsigned long long)cdev->vram_size);
	dev_info(&pdev->dev,
		 "GTI state: SR6A=%02x SR6B=%02x SR6C=%02x SR6D=%02x SR6E=%02x SR6F=%02x SL-base=%#llx\n",
		 gti[0], gti[1], gti[2], gti[3], gti[4], gti[5],
		 (unsigned long long)cdev->sl_base);
	dev_info(&pdev->dev,
		 "clock state: SR3F=%02x SR4F=%02x SR59=%02x\n",
		 clock_control, cr_gating, gfx_power);
	return 0;
}

static void chrome9_program_cr_ring(struct chrome9_device *cdev,
				    u32 start, u32 end, u32 pause,
				    u32 pause_id)
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
	writel(0x65000000 | ((pause_id & 3) << 8) | (pause >> 24),
	       cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x66fffff0, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x670000ff, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	wmb();
	writel(0x680a0001, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	readl(cdev->mmio + CHROME9_REG_CR_TRANSPACE);
}

static void chrome9_flush_command_memory(void *address, size_t size)
{
	/* VIA's H6/H5S2VP1 KMD flushes every command-buffer cache line. */
	mb();
#ifdef CONFIG_X86
	clflush_cache_range(address, size);
#endif
	mb();
}

static u8 chrome9_seq_read_mmio(struct chrome9_device *cdev, u8 index)
{
	writeb(index, cdev->mmio + CHROME9_SEQ_INDEX);
	return readb(cdev->mmio + CHROME9_SEQ_DATA);
}

static void chrome9_seq_write_mmio(struct chrome9_device *cdev, u8 index,
				   u8 value)
{
	writeb(index, cdev->mmio + CHROME9_SEQ_INDEX);
	writeb(value, cdev->mmio + CHROME9_SEQ_DATA);
}

static int chrome9_gart_flush(struct chrome9_device *cdev)
{
	u8 value;
	int ret;

	writeb(0x6f, cdev->mmio + CHROME9_SEQ_INDEX);
	ret = readb_poll_timeout(cdev->mmio + CHROME9_SEQ_DATA, value,
				 !(value & BIT(7)), 10, 100000);
	if (ret)
		return ret;
	writeb(value | BIT(7), cdev->mmio + CHROME9_SEQ_DATA);
	return readb_poll_timeout(cdev->mmio + CHROME9_SEQ_DATA, value,
				  !(value & BIT(7)), 10, 100000);
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

	/* VIA addcmdheader2_invi(): every command header is 128-bit aligned. */
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

	/* VIA addcmdheader3_invi(): count is the exact payload dword count. */
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

void chrome9_init_2d_h6(struct chrome9_device *cdev)
{
	static const u32 precr[] = {
		0x00000000, 0x00333004,
		0x60000000, 0x61000000, 0x62000000,
		0x63000000, 0x64000000, 0x7d000000,
	};
	static const u32 config[] = {
		0x00000004, 0x40008c0f, 0x44000000,
		0x45080c04, 0x46800408,
	};
	unsigned int i;

	/* Address-free VX900 path in Xorg VIAInitialize2DEngine(). */
	for (i = 1; i <= 0x10; i++)
		writel(0, cdev->mmio + i * sizeof(u32));
	writel(0x00100000, cdev->mmio + CHROME9_REG_FE_TRANSSET);
	for (i = 0; i < ARRAY_SIZE(precr); i++)
		writel(precr[i], cdev->mmio + CHROME9_REG_FE_TRANSPACE);
	writel(0xfe020000, cdev->mmio + CHROME9_REG_FE_TRANSSET);
	writel(0, cdev->mmio + CHROME9_REG_FE_TRANSPACE);
	writel(0x00fe0000, cdev->mmio + CHROME9_REG_FE_TRANSSET);
	for (i = 0; i < ARRAY_SIZE(config); i++)
		writel(config[i], cdev->mmio + CHROME9_REG_FE_TRANSPACE);
	readl(cdev->mmio + CHROME9_REG_FE_TRANSPACE);
}

static void chrome9_write_3d_group(struct chrome9_device *cdev, u32 type,
				   const u32 *words, unsigned int count)
{
	unsigned int i;

	writel(type, cdev->mmio + CHROME9_REG_FE_TRANSSET);
	for (i = 0; i < count; i++)
		writel(words[i], cdev->mmio + CHROME9_REG_FE_TRANSPACE);
}

static void chrome9_write_3d_indexed_range(struct chrome9_device *cdev,
					   u32 type, unsigned int count)
{
	unsigned int i;

	writel(type, cdev->mmio + CHROME9_REG_FE_TRANSSET);
	for (i = 0; i < count; i++)
		writel(i << 24, cdev->mmio + CHROME9_REG_FE_TRANSPACE);
}

static void chrome9_write_3d_zeros(struct chrome9_device *cdev, u32 type,
				   unsigned int count)
{
	unsigned int i;

	writel(type, cdev->mmio + CHROME9_REG_FE_TRANSSET);
	for (i = 0; i < count; i++)
		writel(0, cdev->mmio + CHROME9_REG_FE_TRANSPACE);
}

void chrome9_init_3d_h5s2vp1(struct chrome9_device *cdev)
{
	static const u32 degamma[] = {
		0x41e03c05, 0x07714434, 0x11b36ca5,
		0x2176e565, 0x374bd680,
	};
	static const u32 gamma[] = {
		0x40d0340d, 0x41a0681a, 0x42709c27, 0x0330cc33,
		0x04711c47, 0x05715c57, 0x0711c471, 0x08721c87,
		0x0aa2a8aa, 0x0c731cc7, 0x0f63d8f6, 0x11c4711c,
		0x13d4f53d, 0x15b56d5b, 0x1755d575, 0x18e6398e,
		0x1ba6e9ba, 0x1e1785e1, 0x20581605, 0x22689a26,
		0x26198661, 0x296a5a96, 0x2c5b16c5, 0x2f1bc6f1,
		0x31ac6b1a, 0x340d0340, 0x364d9364, 0x386e1b86,
		0x3a6e9ba6, 0x3c6f1bc6, 0x3e4f93e4,
	};
	static const u32 config[] = {
		0x4000840f, 0x47000404, 0x44000000, 0x46000005,
	};
	static const u32 misconfig[] = {
		0x00001004, 0x08000249, 0x0a0002c9, 0x0b0002fb,
		0x0c000000, 0x0d0002cb, 0x0e000009, 0x10000049,
		0x110002ff, 0x12000008, 0x130002db,
	};
	unsigned int stage;

	/* Exact CHIP_H5S2VP1 path in VIAInitialize3DEngine(). */
	chrome9_write_3d_indexed_range(cdev, 0x00010000, 0x9b);
	for (stage = 0; stage < 16; stage++)
		chrome9_write_3d_indexed_range(cdev,
					       0x00020000 | (stage << 24),
					       0x31);
	for (stage = 0; stage < 16; stage++)
		chrome9_write_3d_indexed_range(cdev,
					       0x20020000 | (stage << 24),
					       0x37);
	chrome9_write_3d_indexed_range(cdev, 0xfe020000, 0x14);
	chrome9_write_3d_group(cdev, 0x15030000, degamma,
			       ARRAY_SIZE(degamma));
	chrome9_write_3d_group(cdev, 0x17030000, gamma, ARRAY_SIZE(gamma));
	chrome9_write_3d_zeros(cdev, 0x00030000, 16);
	chrome9_write_3d_zeros(cdev, 0x11030000, 32);
	chrome9_write_3d_zeros(cdev, 0x14030000, 8);
	chrome9_write_3d_indexed_range(cdev, 0x00040000, 0x63);
	chrome9_write_3d_indexed_range(cdev, 0x00fe0000, 0x48);
	chrome9_write_3d_indexed_range(cdev, 0x00110000, 0x21);
	chrome9_write_3d_group(cdev, 0x00fe0000, config,
			       ARRAY_SIZE(config));
	chrome9_write_3d_group(cdev, 0x00fe0000, misconfig,
			       ARRAY_SIZE(misconfig));
	readl(cdev->mmio + CHROME9_REG_FE_TRANSPACE);
}

static void chrome9_emit_3d_quad_h6_internal(u32 **cursor,
					     unsigned long target,
					     bool system_framebuffer,
					     bool constants32,
					     const struct chrome9_quad_vs *vs)
{
	static const u32 config[] = {
		0x00000001, 0x01002521, 0x08010100, 0x05000000,
		0x0d810000,
	};
	static const u32 raster_fvf[] = {
		0x00000206, 0x010000c2, 0x020041d1, 0x030052d0,
		0x04006342, 0x1200001f, 0x61800000, 0x622081ff,
		0x201c5c00, 0x23000f02, 0x24000004, 0x25f6cc00,
		0x28ffcc00,
		0x40800000, 0x41400000, 0x4200493f, 0x43800000,
		0x44400000, 0x4500493f, 0x46800000, 0x47400000,
		0x4800003f, 0x49000000, 0x50000000, 0x51000000,
		0x52147fff,
	};
	static const u32 ps_constants[] = { 0, 0, 0, 0 };
	static const u32 ps_constants_bank0[64] = {
		/* c15 = yellow. */
		[60] = 0x3f800000, [61] = 0x3f800000,
		[62] = 0x00000000, [63] = 0x3f800000,
	};
	static const u32 ps_constants_bank1[64] = {
		/* c16 = magenta; c31 = blue. */
		[0] = 0x3f800000, [1] = 0x00000000,
		[2] = 0x3f800000, [3] = 0x3f800000,
		[60] = 0x00000000, [61] = 0x00000000,
		[62] = 0x3f800000, [63] = 0x3f800000,
	};
	static const u32 vs_float_constants[] = {
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 vs_integer_constants[] = { 0, 0, 0, 0 };
	static const u32 clip[] = {
		0x80000020, 0x81000020, 0x82000020, 0x83000020,
	};
	static const u32 vs_program[] = {
		0x00380150, 0x00000000, 0x00000000, 0x0008010f,
		0x0008014c, 0x00100150, 0x00384154, 0x0004021f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
	};
	static const u32 vs_control[] = { 0x60800003 };
	static const u32 ps_control[] = {
		0x90058000, 0x91000000, 0x92000001, 0x93000000,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 ps_control_ps20[] = {
		0x90058000, 0x91000000, 0x92000001, 0x93000000,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000, 0x9b000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 ps_program[] = {
		0x01c00000, 0x301c0000, 0x00f04005, 0x000d083f,
	};
	static const u32 ps_program_c15_c16[] = {
		0x0140b6d0, 0x300a0053, 0x00f08f05, 0x00000820,
	};
	static const u32 ps_program_c31[] = {
		0x01c00530, 0x301c0053, 0x00f0af05, 0x000d0820,
	};
	static const u32 vertices[] = {
		/* TL, TR, BR, BL; x/y use the Xorg -0.5 pixel-center bias. */
		0xbf000000, 0xbf000000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x41fc0000, 0xbf000000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x41fc0000, 0x41fc0000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
		0xbf000000, 0x41fc0000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
	};
	static const u32 right_vertices[] = {
		/* Overwrite x=16..31 only, proving the last legal user vector. */
		0x41780000, 0xbf000000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x41fc0000, 0xbf000000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x41fc0000, 0x41fc0000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
		0x41780000, 0x41fc0000, 0, 0x3f800000,
		0xffff0000, 0xffff0000,
	};
	u32 fvf[ARRAY_SIZE(raster_fvf)];
	u32 clip_config[ARRAY_SIZE(config)];
	unsigned int i;
	u32 state[] = {
		0x50000000 | (target >> 8),
		0x51000000 | (system_framebuffer ? BIT(20) : 0),
		0x52980004,
		0x38000000, 0x3b000000, 0x39000000, 0x3c000000,
		0x34000150, 0x35000114, 0x36000500, 0x37000200,
		0x860000ff, 0x870000ff, 0x53010c0f, 0x00000011,
		0x01c01540, 0x88001ed0, 0x89000800,
	};

	/* Exact 7122 H6 vendor emission with state+0x178 set to one. */
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     state, ARRAY_SIZE(state));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0xfe020000,
			     vs && vs->clip ? clip_config : config,
			     ARRAY_SIZE(config));
	memcpy(fvf, raster_fvf, sizeof(fvf));
	if (vs) {
		fvf[7] = vs->a62;
		fvf[10] = vs->a24;
	}
	if (vs && vs->vertex_address) {
		/* SF memory type (A00[16:15] = 01) and 16-bit indices (A20). */
		fvf[0] |= BIT(15);
		fvf[8] |= 1;
	}
	if (vs && vs->clip) {
		/*
		 * The clip engine reads its own copy of rows 256/257: low 24
		 * bits per word, exponent bytes packed into A42/A45/A48.
		 */
		for (i = 0; i < 3; i++) {
			u32 scale = vs->internal[i];
			u32 offset = vs->internal[4 + i];

			fvf[13 + 3 * i] = (0x40 + 3 * i) << 24 |
					  (scale & 0x00ffffff);
			fvf[14 + 3 * i] = (0x41 + 3 * i) << 24 |
					  (offset & 0x00ffffff);
			fvf[15 + 3 * i] = (0x42 + 3 * i) << 24 |
					  (offset >> 24) << 8 | scale >> 24;
		}
	}
	memcpy(clip_config, config, sizeof(config));
	clip_config[1] |= 0x800;
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
			     fvf, ARRAY_SIZE(fvf));
	if (constants32) {
		chrome9_emit_header3(cursor, 0x22030000, ps_constants_bank0,
				     ARRAY_SIZE(ps_constants_bank0));
		chrome9_emit_header3(cursor, 0x22030080, ps_constants_bank1,
				     ARRAY_SIZE(ps_constants_bank1));
	} else {
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x22030000,
				     ps_constants, ARRAY_SIZE(ps_constants));
	}
	if (vs && vs->user_dwords)
		chrome9_emit_header3(cursor, 0x32030000, vs->user,
				     vs->user_dwords);
	if (vs)
		chrome9_emit_header3(cursor, 0x32030400, vs->internal,
				     vs->internal_dwords);
	else
		chrome9_emit_header3(cursor, 0x32030400, vs_float_constants,
				     ARRAY_SIZE(vs_float_constants));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x33030000,
			     vs_integer_constants,
			     ARRAY_SIZE(vs_integer_constants));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     clip, ARRAY_SIZE(clip));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x31030000,
			     vs ? vs->program : vs_program,
			     vs ? vs->program_dwords : ARRAY_SIZE(vs_program));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
			     vs ? &vs->a60 : vs_control, 1);
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     constants32 ? ps_control_ps20 : ps_control,
			     constants32 ? ARRAY_SIZE(ps_control_ps20) :
				     ARRAY_SIZE(ps_control));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x20030000,
			     constants32 ? ps_program_c15_c16 : ps_program,
			     ARRAY_SIZE(ps_program));
	if (vs && vs->vertex_address) {
		unsigned int count = vs->a24 & 0xffffff;
		u32 streams[3], draw[4], indices[2];

		/* One stream slot per element: 1-KiB base | stride in dwords. */
		for (i = 0; i < ARRAY_SIZE(streams); i++)
			streams[i] = vs->vertex_address | vs->vertex_dwords / count;
		chrome9_emit_header3(cursor, 0x30030000, streams,
				     ARRAY_SIZE(streams));
		draw[0] = 0x2c000000 | (count - 1);
		draw[1] = 0x2d000000;
		draw[2] = 0x2e000000;
		draw[3] = vs->a24;
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
				     draw, ARRAY_SIZE(draw));
		for (i = 0; i < ARRAY_SIZE(indices); i++)
			indices[i] = 2 * i < count ?
				     (2 * i + 1 < count ? (2 * i + 1) << 16 : 0) |
				     2 * i : 0;
		chrome9_emit_header3(cursor, 0x00000000, indices,
				     DIV_ROUND_UP(count, 2));
	} else {
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00000000,
				     vs ? vs->vertices : vertices,
				     vs ? vs->vertex_dwords :
					  ARRAY_SIZE(vertices));
	}
	if (constants32) {
		/* A program switch replays the complete compiler control object. */
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
				     ps_control_ps20,
				     ARRAY_SIZE(ps_control_ps20));
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x20030000,
				     ps_program_c31, ARRAY_SIZE(ps_program_c31));
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00000000,
				     right_vertices, ARRAY_SIZE(right_vertices));
	}
}

void chrome9_emit_3d_quad_h6(u32 **cursor, unsigned long target,
			    bool system_framebuffer)
{
	chrome9_emit_3d_quad_h6_internal(cursor, target, system_framebuffer,
					 false, NULL);
}

void chrome9_emit_3d_quad_h6_vs(u32 **cursor, unsigned long target,
				const struct chrome9_quad_vs *vs)
{
	chrome9_emit_3d_quad_h6_internal(cursor, target, true, false, vs);
}

static void chrome9_emit_3d_texture_h6(u32 **cursor, unsigned long target,
				       unsigned long texture,
				       bool target_sf, bool texture_sf)
{
	static const u32 destination[] = {
		0x52980004, 0x38000000, 0x3b000000, 0x39000000,
		0x3c000000, 0x34000150, 0x35000114, 0x36000500,
		0x37000200,
	};
	static const u32 texture_stage[] = {
		0x20020020, 0x21000001, 0x22008055, 0x30980000,
	};
	static const u32 sampler[] = { 0x31100000, 0x32000052 };
	static const u32 common[] = {
		0x860000ff, 0x870000ff, 0x53010c0f, 0x00000011,
		0x01c01540, 0x88001ed0, 0x89000800,
	};
	static const u32 config[] = {
		0x00000011, 0x01002521, 0x08010101, 0x05000000,
		0x0d810000,
	};
	static const u32 raster_fvf[] = {
		0x00000308, 0x010000c2, 0x020041d0, 0x030052d0,
		0x04006342, 0x1200000f, 0x61800000, 0x623081ff,
		0x201c5c00, 0x23000f02, 0x24000004, 0x25f6cc00,
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
	static const u32 clip[] = {
		0x80000020, 0x81000020, 0x82000020, 0x83000020,
	};
	static const u32 vs_program[] = {
		0x00380150, 0x00000000, 0x00000000, 0x0008010f,
		0x0008014c, 0x00100150, 0x00384154, 0x0004021f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
		0x0008c14c, 0x00000000, 0x00000000, 0x0008028f,
	};
	static const u32 vs_control[] = { 0x60800004 };
	static const u32 ps_control[] = {
		0x90018000, 0x91000001, 0x92010001, 0x93000001,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 ps_texture[] = { 0x301101b0 };
	static const u32 ps_program[] = {
		0x01c00000, 0x301c0000, 0x00f20105, 0x000d083f,
	};
	static const u32 vertices[] = {
		0xbf000000, 0xbf000000, 0, 0x3f800000,
		0xffff0000, 0xffff0000, 0x00000000, 0x00000000,
		0x41fc0000, 0xbf000000, 0, 0x3f800000,
		0xffff0000, 0xffff0000, 0x3f800000, 0x00000000,
		0x41fc0000, 0x41fc0000, 0, 0x3f800000,
		0xffff0000, 0xffff0000, 0x3f800000, 0x3f800000,
		0xbf000000, 0x41fc0000, 0, 0x3f800000,
		0xffff0000, 0xffff0000, 0x00000000, 0x3f800000,
	};
	u32 destination_state[2 + ARRAY_SIZE(destination)];
	u32 texture_state[2 + ARRAY_SIZE(texture_stage)];

	destination_state[0] = 0x50000000 | (target >> 8);
	destination_state[1] = 0x51000000 | (target_sf ? BIT(20) : 0);
	memcpy(&destination_state[2], destination, sizeof(destination));
	texture_state[0] = texture >> 8;
	texture_state[1] = 0x18001100 | (texture_sf ? 1 : 0);
	memcpy(&texture_state[2], texture_stage, sizeof(texture_stage));

	/* Exact normal 7122 PictOpSrc one-texture emission. */
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     destination_state, ARRAY_SIZE(destination_state));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00020000,
			     texture_state, ARRAY_SIZE(texture_state));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x20020000,
			     sampler, ARRAY_SIZE(sampler));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     common, ARRAY_SIZE(common));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0xfe020000,
			     config, ARRAY_SIZE(config));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
			     raster_fvf, ARRAY_SIZE(raster_fvf));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x22030000,
			     ps_constants, ARRAY_SIZE(ps_constants));
	chrome9_emit_header3(cursor, 0x32030400, vs_float_constants,
			     ARRAY_SIZE(vs_float_constants));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x33030000,
			     vs_integer_constants,
			     ARRAY_SIZE(vs_integer_constants));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     clip, ARRAY_SIZE(clip));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x31030000,
			     vs_program, ARRAY_SIZE(vs_program));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
			     vs_control, ARRAY_SIZE(vs_control));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
			     ps_control, ARRAY_SIZE(ps_control));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x21030000,
			     ps_texture, ARRAY_SIZE(ps_texture));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x20030000,
			     ps_program, ARRAY_SIZE(ps_program));
	chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00000000,
			     vertices, ARRAY_SIZE(vertices));
}

static void chrome9_emit_3d_store_probe(u32 **cursor, unsigned int probe)
{
	static const u32 h6_config[] = {
		0x00000001, 0x01002521, 0x08010100, 0x05000000,
		0x0d810000,
	};
	static const u32 h6_raster_fvf[] = {
		0x00000206, 0x010000c2, 0x020041d0, 0x030052d0,
		0x04006342, 0x1200001f, 0x61800000, 0x622081ff,
		0x201c5c00, 0x23000f02, 0x24000004, 0x25f6cc00,
		0x28ffcc00, 0x40800000, 0x41400000, 0x4200493f,
		0x43800000, 0x44400000, 0x4500493f, 0x46800000,
		0x47400000, 0x4800003f, 0x49000000, 0x50000000,
		0x51000000, 0x52147fff,
	};
	static const u32 h6_vs_float_constants[] = {
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
		0x49400000, 0x49400000, 0x00000000, 0x00000000,
	};
	static const u32 h6_vs_integer_constants[] = { 0, 0, 0, 0 };
	static const u32 ps_control[] = {
		0x90058000, 0x91000000, 0x92000001, 0x93000000,
		0x94000000, 0x95000000, 0x96000000, 0x97000000,
		0x98000000, 0x99000000, 0x9a000000,
		0xcccccccc, 0xdddddddd,
	};
	static const u32 ps_program[] = {
		0x01c00000, 0x301c0000, 0x00f04005, 0x000d083f,
	};
	static const u32 vs_nop[] = { 0, 0, 0, 0 };
	static const u32 vs_program[] = {
		0x00380150, 0x00000000, 0x00000000, 0x0008010f,
		0x0008014c, 0x00100150, 0x00384154, 0x0004021f,
		0x0008414c, 0x00000000, 0x00000000, 0x0008422f,
		0x0008814c, 0x00000000, 0x00000000, 0x0008423f,
	};
	static const u32 vs_control[] = { 0x60800003 };

	if (probe == 6) {
		/* Exact address-free H6 delta; intentionally no type-00 vertices. */
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0xfe020000,
				     h6_config, ARRAY_SIZE(h6_config));
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
				     h6_raster_fvf, ARRAY_SIZE(h6_raster_fvf));
		chrome9_emit_header3(cursor, 0x32030400,
				     h6_vs_float_constants,
				     ARRAY_SIZE(h6_vs_float_constants));
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x33030000,
				     h6_vs_integer_constants,
				     ARRAY_SIZE(h6_vs_integer_constants));
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x31030000,
				     vs_program, ARRAY_SIZE(vs_program));
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
				     vs_control, ARRAY_SIZE(vs_control));
		return;
	}

	if (probe == 4)
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00010000,
				     ps_control, ARRAY_SIZE(ps_control));
	if (probe == 1 || probe == 4)
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x20030000,
				     ps_program, ARRAY_SIZE(ps_program));
	else if (probe == 2)
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x31030000,
				     vs_nop, ARRAY_SIZE(vs_nop));
	else if (probe == 3 || probe == 5)
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x31030000,
				     vs_program, ARRAY_SIZE(vs_program));
	if (probe == 5)
		chrome9_emit_header2(cursor, 0x440, 0x0000043c, 0x00040000,
				     vs_control, ARRAY_SIZE(vs_control));
}

static int chrome9_selftest_pcie_ring(struct chrome9_device *cdev)
{
	struct pci_dev *pdev = cdev->pdev;
	void __iomem *gart;
	void __iomem *test = NULL;
	void __iomem *texture = NULL;
	u8 *gart_saved = NULL;
	u8 *test_saved = NULL;
	u8 *texture_saved = NULL;
	u32 *ring = NULL;
	u32 *staged_ring = NULL;
	u32 *sf_allocation = NULL;
	u32 *sf_target = NULL;
	u32 *sf_texture = NULL;
	struct drm_gem_shmem_object *sf_shmem = NULL;
	struct drm_gem_dma_object *sf_dma_gem = NULL;
	struct sg_table *sf_sgt = NULL;
	struct iosys_map sf_map = {};
	u32 sf_ptes[6] = {};
	u32 *p;
	dma_addr_t ring_dma;
	dma_addr_t sf_dma = 0;
	u16 pci_command;
	u8 saved_index;
	u8 saved_6a;
	u8 saved_6b;
	u8 saved_6c;
	u8 saved_7b;
	u8 restore_6c;
	u32 current_before;
	u32 current_after;
	u32 prime_current;
	u32 prime_pause;
	u32 prime_ring_status;
	u32 prime_engine_status;
	u32 ring_status;
	u32 engine_status;
	u32 engine_error_before;
	u32 engine_error_after;
	u32 status_45c;
	u32 status_460;
	u32 status_464;
	u32 saved_2d_mode = 0;
	u32 saved_2d_pitch = 0;
	u32 saved_2d_dimension = 0;
	u32 saved_2d_dst_pos = 0;
	u32 saved_2d_dst_base = 0;
	u32 saved_2d_color = 0;
	u32 saved_2d_sync = 0;
	u32 saved_2d_transparency = 0;
	u32 saved_2d_410_30 = 0;
	u32 saved_2d_410_34 = 0;
	u32 saved_2d_410_38 = 0;
	u32 observed_2d_mode = 0;
	u32 observed_2d_pitch = 0;
	u32 observed_2d_dimension = 0;
	u32 observed_2d_dst_pos = 0;
	u32 observed_2d_dst_base = 0;
	u32 observed_2d_color = 0;
	u32 observed_2d_command = 0;
	u32 pause;
	u32 pte;
	unsigned int bad_guard = 0;
	unsigned int bad_pixels = 0;
	unsigned int sf_bad_guard = 0;
	unsigned int sf_bad_pixels = 0;
	unsigned int unchanged_pixels = 0;
	unsigned int zero_pixels = 0;
	u32 first_pixel = 0;
	u32 last_pixel = 0;
	u32 sf_first_pixel = 0;
	u32 sf_last_pixel = 0;
	u32 texture_samples[4] = {};
	unsigned int texture_bad_guard = 0;
	unsigned int texture_bad_source = 0;
	unsigned int texture_changed_pixels = 0;
	unsigned int texture_pre_bad_guard = 0;
	unsigned int texture_pre_bad_source = 0;
	unsigned int i;
	const bool do_2d_fill = chrome9_selftest_2d_offset != ULONG_MAX;
	const bool do_3d_sf = chrome9_selftest_3d_sf;
	const bool do_3d_shmem = chrome9_selftest_3d_shmem;
	const bool do_3d_shmem_cached = chrome9_selftest_3d_shmem_cached;
	const bool do_3d_dma_gem = chrome9_selftest_3d_dma_gem;
	const bool do_3d_texture = chrome9_selftest_3d_texture;
	const unsigned int sf_page_count = do_3d_texture ? 6 : 3;
	const unsigned int guard_size = chrome9_selftest_3d_quad ?
		PAGE_SIZE : CHROME9_SELFTEST_GUARD;
	unsigned long texture_offset = 0;
	bool was_master;
	bool seq_locked = false;
	bool gart_modified = false;
	bool retain_ring = false;
	int idle_ret = -ETIMEDOUT;
	int pause_ret = -ETIMEDOUT;
	int prime_idle_ret = -ETIMEDOUT;
	int prime_pause_ret = -ETIMEDOUT;
	int dispatch_ret = 0;
	int ret;

	if (!chrome9_selftest_cr_pcie)
		return 0;
	if (do_3d_texture && !chrome9_selftest_3d_quad)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"one-texture test requires the 3D quad test\n");
	if (do_3d_sf && !chrome9_selftest_3d_quad)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"system-framebuffer 3D target requires the quad test\n");
	if (do_3d_shmem && !do_3d_sf)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"GEM shmem 3D target requires system-framebuffer mode\n");
	if (do_3d_shmem_cached && !do_3d_shmem)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"cached shmem mode requires the GEM shmem target\n");
	if (do_3d_dma_gem && !do_3d_sf)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"DMA GEM 3D target requires system-framebuffer mode\n");
	if (do_3d_dma_gem && do_3d_shmem)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"DMA GEM and shmem target modes are mutually exclusive\n");
	if (chrome9_selftest_3d_store > 7)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid 3D store probe %u\n",
				     chrome9_selftest_3d_store);
	if (chrome9_selftest_3d_store == 7 &&
	    (!chrome9_selftest_3d_quad || do_3d_texture))
		return dev_err_probe(&pdev->dev, -EINVAL,
			"full PS constant probe requires an untextured 3D quad\n");
	if (chrome9_selftest_3d_store &&
	    (!chrome9_selftest_init_3d || !chrome9_selftest_cr_observe_3d ||
	     !do_2d_fill))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "3D store probe requires init/status and guarded 2D regression\n");
	if (chrome9_engine_bar != 1)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "VX900 PCIe CR test requires BAR1 MMIO\n");
	if (chrome9_selftest_gart_offset == ULONG_MAX)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "VX900 PCIe CR test requires selftest_gart_offset\n");
	if (chrome9_selftest_cr ||
	    chrome9_selftest_cr_sl_offset != ULONG_MAX)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "VX900 PCIe CR test is mutually exclusive with other engine tests\n");
	if (!cdev->vram_size) {
		ret = chrome9_detect_vram(cdev);
		if (ret)
			return ret;
	}
	if (!IS_ALIGNED(chrome9_selftest_gart_offset, PAGE_SIZE) ||
	    chrome9_selftest_gart_offset >
		cdev->vram_size - CHROME9_PCIE_GART_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid 64-KiB GART offset %#lx\n",
				     chrome9_selftest_gart_offset);
	if (do_2d_fill &&
	    (chrome9_selftest_2d_offset < guard_size ||
	     !IS_ALIGNED(chrome9_selftest_2d_offset, 64) ||
	     chrome9_selftest_2d_offset >
		cdev->vram_size - CHROME9_SELFTEST_SIZE -
		guard_size ||
	     (chrome9_selftest_2d_offset - guard_size <
		chrome9_selftest_gart_offset + CHROME9_PCIE_GART_SIZE &&
	      chrome9_selftest_2d_offset + CHROME9_SELFTEST_SIZE +
		guard_size > chrome9_selftest_gart_offset)))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid or overlapping PCIe 2D offset %#lx\n",
				     chrome9_selftest_2d_offset);
	if (chrome9_selftest_3d_quad &&
	    (!do_2d_fill || !chrome9_selftest_init_3d ||
	     !chrome9_selftest_cr_observe_3d ||
	     !IS_ALIGNED(chrome9_selftest_2d_offset, 256)))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "3D quad requires a 256-byte-aligned 2D target plus init/status gates\n");
	if (do_3d_texture && !do_3d_sf) {
		if (chrome9_selftest_2d_offset < SZ_64K + PAGE_SIZE)
			return dev_err_probe(&pdev->dev, -EINVAL,
				"one-texture source cannot precede target safely\n");
		texture_offset = chrome9_selftest_2d_offset - SZ_64K;
		if (!IS_ALIGNED(texture_offset, 256) ||
		    texture_offset < PAGE_SIZE ||
		    texture_offset > cdev->vram_size - 2 * PAGE_SIZE ||
		    texture_offset + PAGE_SIZE >
			chrome9_selftest_2d_offset - guard_size)
			return dev_err_probe(&pdev->dev, -EINVAL,
				"invalid or overlapping texture offset %#lx\n",
				texture_offset);
	}
	engine_status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	ring_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	engine_error_before = readl(cdev->mmio + CHROME9_REG_ENGINE_ERROR);
	if (engine_status & CHROME9_ENGINE_WAIT_MASK)
		return dev_err_probe(&pdev->dev, -EBUSY,
				     "refusing PCIe CR test: engine=%08x CR=%08x\n",
				     engine_status, ring_status);
	if (ring_status & BIT(31))
		dev_info(&pdev->dev,
			 "replacing an idle paused CR, as in VIA ring initialization\n");

	cdev->vram = pcim_iomap(pdev, 2, 0);
	if (!cdev->vram)
		return -ENOMEM;
	gart = cdev->vram + chrome9_selftest_gart_offset;
	gart_saved = kvmalloc(CHROME9_PCIE_GART_SIZE, GFP_KERNEL);
	if (!gart_saved)
		return -ENOMEM;
	memcpy_fromio(gart_saved, gart, CHROME9_PCIE_GART_SIZE);

	ring = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &ring_dma,
				  GFP_KERNEL);
	if (!ring) {
		ret = -ENOMEM;
		goto out_free_backup;
	}
	if (!IS_ALIGNED(ring_dma, PAGE_SIZE) || ring_dma + PAGE_SIZE > U32_MAX) {
		ret = -ERANGE;
		goto out_free_ring;
	}
	staged_ring = (u32 *)get_zeroed_page(GFP_KERNEL);
	if (!staged_ring) {
		ret = -ENOMEM;
		goto out_free_ring;
	}
	if (do_3d_sf) {
		if (do_3d_dma_gem) {
			sf_dma_gem = drm_gem_dma_create(&cdev->drm,
						       sf_page_count * PAGE_SIZE);
			if (IS_ERR(sf_dma_gem)) {
				ret = PTR_ERR(sf_dma_gem);
				sf_dma_gem = NULL;
				goto out_free_ring;
			}
			sf_allocation = sf_dma_gem->vaddr;
			sf_dma = sf_dma_gem->dma_addr;
			if (!IS_ALIGNED(sf_dma, PAGE_SIZE) ||
			    sf_dma + sf_page_count * PAGE_SIZE > U32_MAX) {
				ret = -ERANGE;
				goto out_free_ring;
			}
			for (i = 0; i < sf_page_count; i++)
				sf_ptes[i] = lower_32_bits((sf_dma +
					i * PAGE_SIZE) >> PAGE_SHIFT) & 0x3fffffff;
		} else if (do_3d_shmem) {
			struct scatterlist *sg;
			unsigned int pte_count = 0;
			unsigned int sg_index;

			sf_shmem = drm_gem_shmem_create(&cdev->drm,
						       sf_page_count * PAGE_SIZE);
			if (IS_ERR(sf_shmem)) {
				ret = PTR_ERR(sf_shmem);
				sf_shmem = NULL;
				goto out_free_ring;
			}
			/*
			 * Windows sets PTE bit 25 only for an MmCached CPU mapping.
			 * Its noncached/WC paths and VIA's old Linux NC heap use a
			 * plain PFN.
			 */
			if (!do_3d_shmem_cached)
				sf_shmem->map_wc = true;
			sf_sgt = drm_gem_shmem_get_pages_sgt(sf_shmem);
			if (IS_ERR(sf_sgt)) {
				ret = PTR_ERR(sf_sgt);
				sf_sgt = NULL;
				goto out_free_ring;
			}
			ret = drm_gem_vmap(&sf_shmem->base, &sf_map);
			if (ret)
				goto out_free_ring;
			sf_allocation = sf_map.vaddr;
			for_each_sgtable_dma_sg(sf_sgt, sg, sg_index) {
				dma_addr_t address = sg_dma_address(sg);
				unsigned int length = sg_dma_len(sg);

				if (!IS_ALIGNED(address, PAGE_SIZE) ||
				    !IS_ALIGNED(length, PAGE_SIZE) ||
				    upper_32_bits(address) ||
				    address > U32_MAX - length) {
					ret = -ERANGE;
					goto out_free_ring;
				}
				while (length && pte_count < sf_page_count) {
					if (!pte_count)
						sf_dma = address;
					sf_ptes[pte_count++] =
						(lower_32_bits(address >> PAGE_SHIFT) &
						 0x3fffffff) |
						(do_3d_shmem_cached ? BIT(25) : 0);
					address += PAGE_SIZE;
					length -= PAGE_SIZE;
				}
			}
			if (pte_count != sf_page_count) {
				ret = -EINVAL;
				goto out_free_ring;
			}
			dev_info(&pdev->dev,
				 "VX900 GEM shmem SF mode=%s dma=%pad first_pte=%08x\n",
				 do_3d_shmem_cached ? "cached/PTE25" :
				 "WC/plain-PFN", &sf_dma, sf_ptes[0]);
		} else {
			sf_allocation = dma_alloc_coherent(&pdev->dev,
						   sf_page_count * PAGE_SIZE,
						   &sf_dma,
						   GFP_KERNEL);
			if (!sf_allocation) {
				ret = -ENOMEM;
				goto out_free_ring;
			}
			if (!IS_ALIGNED(sf_dma, PAGE_SIZE) ||
			    sf_dma + sf_page_count * PAGE_SIZE > U32_MAX) {
				ret = -ERANGE;
				goto out_free_ring;
			}
			for (i = 0; i < sf_page_count; i++)
				sf_ptes[i] = lower_32_bits((sf_dma +
					i * PAGE_SIZE) >> PAGE_SHIFT) & 0x3fffffff;
		}
		sf_target = sf_allocation + PAGE_SIZE / sizeof(u32);
		if (do_3d_texture)
			sf_texture = sf_allocation + 4 * PAGE_SIZE / sizeof(u32);
		for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
			sf_allocation[i] = CHROME9_SELFTEST_GUARD_WORD;
			sf_target[i] = CHROME9_SELFTEST_SENTINEL;
			sf_target[PAGE_SIZE / sizeof(u32) + i] =
				CHROME9_SELFTEST_GUARD_WORD;
			if (do_3d_texture) {
				sf_target[2 * PAGE_SIZE / sizeof(u32) + i] =
					CHROME9_SELFTEST_GUARD_WORD;
				sf_texture[i] =
					chrome9_selftest_texture_pixel(i);
				sf_texture[PAGE_SIZE / sizeof(u32) + i] =
					CHROME9_SELFTEST_GUARD_WORD;
			}
		}
		/* The internal SF path is not reliably coherent with CPU WB cache. */
		chrome9_flush_command_memory(sf_allocation,
					     sf_page_count * PAGE_SIZE);
		if (sf_sgt)
			dma_sync_sgtable_for_device(&pdev->dev, sf_sgt,
						    DMA_BIDIRECTIONAL);
		dma_wmb();
		if (do_3d_texture) {
			for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
				texture_pre_bad_source +=
					READ_ONCE(sf_texture[i]) !=
					chrome9_selftest_texture_pixel(i);
				texture_pre_bad_guard += READ_ONCE(sf_target[
					2 * PAGE_SIZE / sizeof(u32) + i]) !=
					CHROME9_SELFTEST_GUARD_WORD;
				texture_pre_bad_guard += READ_ONCE(sf_texture[
					PAGE_SIZE / sizeof(u32) + i]) !=
					CHROME9_SELFTEST_GUARD_WORD;
			}
			dev_info(&pdev->dev,
				 "VX900 SF texture preflight dma=%pad bad_source=%u bad_guard=%u\n",
				 &sf_dma, texture_pre_bad_source,
				 texture_pre_bad_guard);
			if (texture_pre_bad_source || texture_pre_bad_guard) {
				ret = -EIO;
				goto out_free_ring;
			}
		}
	}
	if (do_2d_fill) {
		test = cdev->vram + chrome9_selftest_2d_offset;
		test_saved = kmalloc(CHROME9_SELFTEST_SIZE +
				    2 * guard_size, GFP_KERNEL);
		if (!test_saved) {
			ret = -ENOMEM;
			goto out_free_ring;
		}
		memcpy_fromio(test_saved, test - guard_size,
			      CHROME9_SELFTEST_SIZE +
			      2 * guard_size);
		for (i = 0; i < guard_size; i += sizeof(u32)) {
			writel(CHROME9_SELFTEST_GUARD_WORD,
			       test - guard_size + i);
			writel(CHROME9_SELFTEST_GUARD_WORD,
			       test + CHROME9_SELFTEST_SIZE + i);
		}
		for (i = 0; i < CHROME9_SELFTEST_SIZE; i += sizeof(u32))
			writel(CHROME9_SELFTEST_SENTINEL, test + i);
		readl(test + CHROME9_SELFTEST_SIZE - sizeof(u32));
	}
	if (do_3d_texture && !do_3d_sf) {
		texture = cdev->vram + texture_offset;
		texture_saved = kmalloc(3 * PAGE_SIZE, GFP_KERNEL);
		if (!texture_saved) {
			ret = -ENOMEM;
			goto out_free_ring;
		}
		memcpy_fromio(texture_saved, texture - PAGE_SIZE,
			      3 * PAGE_SIZE);
		for (i = 0; i < PAGE_SIZE; i += sizeof(u32)) {
			writel(CHROME9_SELFTEST_GUARD_WORD,
			       texture - PAGE_SIZE + i);
			writel(chrome9_selftest_texture_pixel(i / sizeof(u32)),
			       texture + i);
			writel(CHROME9_SELFTEST_GUARD_WORD,
			       texture + PAGE_SIZE + i);
		}
		readl(texture + 2 * PAGE_SIZE - sizeof(u32));
	}
	memset(ring, 0xcc, PAGE_SIZE);
	p = ring;
	/*
	 * Both VIA's Linux KMD and the 7122 Windows KMD establish a fresh
	 * command reader with a dummy-only, 128-byte-aligned transaction.
	 * Ordinary work is appended later and made visible by A64/A65 only.
	 */
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	prime_pause = (u8 *)p - (u8 *)ring - 16;
	memset(staged_ring, 0xcc, PAGE_SIZE);
	p = (u32 *)((u8 *)staged_ring + prime_pause + 16);
	if (chrome9_selftest_cr_observe_2d || do_2d_fill) {
		/* VIA addcmdheader0_invi(): count is register/value pairs. */
		saved_2d_mode = readl(cdev->mmio + CHROME9_2D_REG_MODE);
		saved_2d_pitch = readl(cdev->mmio + CHROME9_2D_REG_PITCH);
		saved_2d_dimension = readl(cdev->mmio + CHROME9_2D_REG_DIMENSION);
		saved_2d_dst_pos = readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
		saved_2d_dst_base = readl(cdev->mmio + CHROME9_2D_REG_DST_BASE);
		saved_2d_color = readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);
	}
	if (do_2d_fill) {
		u32 sync[] = { 0x6c, 0x00400000 };
		u32 transparency[] = { 0x48, 0 };
		u32 disable_410[] = { 0x30, 0, 0x34, 0, 0x38, 0 };
		u32 solid[] = {
			CHROME9_2D_REG_MODE, CHROME9_2D_MODE_32BPP,
			CHROME9_2D_REG_DST_BASE,
			chrome9_selftest_2d_offset >> 3,
			CHROME9_2D_REG_PITCH,
			(CHROME9_SELFTEST_PITCH >> 3) << 16,
			CHROME9_2D_REG_DST_POS, 0,
			CHROME9_2D_REG_DIMENSION,
			((CHROME9_SELFTEST_HEIGHT - 1) << 16) |
			(CHROME9_SELFTEST_WIDTH - 1),
			CHROME9_2D_REG_FG_COLOR, CHROME9_SELFTEST_FILL,
			CHROME9_2D_REG_COMMAND, 0xf0002001,
		};

		saved_2d_sync = readl(cdev->mmio + 0x6c);
		saved_2d_transparency = readl(cdev->mmio + 0x48);
		saved_2d_410_30 = readl(cdev->mmio + 0x30);
		saved_2d_410_34 = readl(cdev->mmio + 0x34);
		saved_2d_410_38 = readl(cdev->mmio + 0x38);
		chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
		chrome9_emit_header0(&p, transparency,
				     ARRAY_SIZE(transparency) / 2);
		chrome9_emit_header0(&p, disable_410,
				     ARRAY_SIZE(disable_410) / 2);
		chrome9_emit_header0(&p, solid, ARRAY_SIZE(solid) / 2);
		chrome9_emit_header0(&p, sync, ARRAY_SIZE(sync) / 2);
	} else if (chrome9_selftest_cr_observe_2d) {
		u32 observe[] = {
			CHROME9_2D_REG_MODE, CHROME9_2D_MODE_32BPP,
			CHROME9_2D_REG_FG_COLOR, CHROME9_SELFTEST_FILL,
		};

		chrome9_emit_header0(&p, observe, ARRAY_SIZE(observe) / 2);
	}
	if (chrome9_selftest_3d_store && chrome9_selftest_3d_store != 7) {
		u32 sync_3d[] = { 0x6c, 0x00800000 };

		chrome9_emit_header0(&p, sync_3d, ARRAY_SIZE(sync_3d) / 2);
		chrome9_emit_3d_store_probe(&p, chrome9_selftest_3d_store);
	}
	if (chrome9_selftest_3d_quad) {
		u32 sync_3d[] = { 0x6c, 0x00800000 };
		u32 *body;
		unsigned int body_dwords;

		chrome9_emit_header0(&p, sync_3d, ARRAY_SIZE(sync_3d) / 2);
		body = PTR_ALIGN(p, 16);
		if (do_3d_texture)
			chrome9_emit_3d_texture_h6(&p,
				do_3d_sf ? 2 * PAGE_SIZE :
				chrome9_selftest_2d_offset,
				do_3d_sf ? 5 * PAGE_SIZE : texture_offset,
				do_3d_sf, do_3d_sf);
		else
			chrome9_emit_3d_quad_h6_internal(&p,
				do_3d_sf ? 2 * PAGE_SIZE :
				chrome9_selftest_2d_offset,
				do_3d_sf,
				chrome9_selftest_3d_store == 7, NULL);
		body_dwords = p - body;
		if (body_dwords != (do_3d_texture ? 232 :
				    chrome9_selftest_3d_store == 7 ? 372 : 188)) {
			ret = -EINVAL;
			goto out_free_ring;
		}
		chrome9_emit_header0(&p, sync_3d, ARRAY_SIZE(sync_3d) / 2);
	}
	if (chrome9_selftest_cr_observe_3d) {
		static const u32 status[] = {
			0x1c000000, 0x1d000000,
			0xcccccccc, 0xcccccccc,
		};

		/* Exact hwmUpdateStatus_h5() address-free 3D transaction. */
		chrome9_emit_header2(&p, 0x440, 0x0000043c, 0x00010000,
				     status, ARRAY_SIZE(status));
	}
	chrome9_emit_header2(&p, 0x420, 0x0000041c, 0x00300000, NULL, 0);
	if ((u8 *)p - (u8 *)staged_ring >
	    PAGE_SIZE - CHROME9_CR_READ_AHEAD) {
		ret = -E2BIG;
		goto out_free_ring;
	}
	do {
		*p++ = 0xccccccc0;
		*p++ = 0xddd00000;
	} while (!IS_ALIGNED((unsigned long)p, 128));
	pause = (u8 *)p - (u8 *)staged_ring - 16;
	if (pause > PAGE_SIZE - CHROME9_CR_READ_AHEAD) {
		ret = -E2BIG;
		goto out_free_ring;
	}
	chrome9_flush_command_memory(ring, PAGE_SIZE);
	dma_wmb();

	ret = chrome9_vga_seq_get(cdev);
	if (ret)
		goto out_free_ring;
	seq_locked = true;
	saved_index = readb(cdev->mmio + CHROME9_SEQ_INDEX);
	saved_6a = chrome9_seq_read_mmio(cdev, 0x6a);
	saved_6b = chrome9_seq_read_mmio(cdev, 0x6b);
	saved_6c = chrome9_seq_read_mmio(cdev, 0x6c);
	saved_7b = chrome9_seq_read_mmio(cdev, 0x7b);

	/* The 2010 VIA VX900 driver disables GTI while changing its table. */
	chrome9_seq_write_mmio(cdev, 0x6c, saved_6c & ~BIT(7));
	chrome9_seq_write_mmio(cdev, 0x6a,
			 (chrome9_selftest_gart_offset >> 12) & 0xff);
	chrome9_seq_write_mmio(cdev, 0x6b,
			 (chrome9_selftest_gart_offset >> 20) & 0xff);
	chrome9_seq_write_mmio(cdev, 0x6c,
			 (saved_6c & ~BIT(7) & ~BIT(0)) |
			 ((chrome9_selftest_gart_offset >> 28) & 1));
	chrome9_seq_write_mmio(cdev, 0x7b, (saved_7b & 0xf0) | 4);

	for (i = 0; i < CHROME9_PCIE_GART_ENTRIES; i++)
		writel(0x80000000, gart + i * sizeof(u32));
	pte = lower_32_bits(ring_dma >> PAGE_SHIFT) & 0x3fffffff;
	writel(pte, gart);
	if (do_3d_sf) {
		for (i = 0; i < sf_page_count; i++)
			writel(sf_ptes[i], gart + (i + 1) * sizeof(u32));
	}
	readl(gart);
	gart_modified = true;
	ret = chrome9_gart_flush(cdev);
	if (ret)
		goto out_restore;
	restore_6c = chrome9_seq_read_mmio(cdev, 0x6c) | BIT(7);
	chrome9_seq_write_mmio(cdev, 0x6c, restore_6c);
	writeb(saved_index, cdev->mmio + CHROME9_SEQ_INDEX);
	chrome9_vga_seq_put(cdev);
	seq_locked = false;

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
	was_master = pci_command & PCI_COMMAND_MASTER;
	pci_set_master(pdev);

	/*
	 * Xorg's 7122 mode-init order is 2D then 3D.  Both precede CR setup:
	 * the 3D sweep clears type-11 CR state, and its final shared FE/config
	 * values must not be overwritten by the shorter 2D initializer.
	 */
	if (do_2d_fill)
		chrome9_init_2d_h6(cdev);
	if (chrome9_selftest_init_3d)
		chrome9_init_3d_h5s2vp1(cdev);

	/* Exact H5S2VP1 CR state setup from VIA's 2010 Linux KMD. */
	writel(0x00110000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
	writel(0x06000000, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x07100000, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	writel(0x00100000, cdev->mmio + CHROME9_REG_CR_TRANSSET);
	writel(0x02000000, cdev->mmio + CHROME9_REG_CR_TRANSPACE);
	wmb();

	current_before = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	/* VX900 PCIe rings use GTI address zero with location code 01b. */
	chrome9_program_cr_ring(cdev, 1, PAGE_SIZE, prime_pause, 0);
	prime_pause_ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS,
					    prime_ring_status,
					    prime_ring_status & BIT(31),
					    10, 100000);
	prime_engine_status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	/* A newly armed dummy-only PAUSE can retain CR-busy bit 4. */
	prime_idle_ret = prime_engine_status &
		(CHROME9_ENGINE_WAIT_MASK & ~BIT(4)) ? -EBUSY : 0;
	prime_current = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	if (prime_pause_ret || prime_idle_ret) {
		pause_ret = prime_pause_ret;
		idle_ret = prime_idle_ret;
		goto read_results;
	}

	/*
	 * The suffix must not exist while the dummy ring is being primed: the
	 * request FIFO can otherwise consume it as look-ahead behind the old
	 * PAUSE.  Publish the staged commands only after PAUSE-ready.
	 */
	memcpy((u8 *)ring + prime_pause + 16,
	       (u8 *)staged_ring + prime_pause + 16,
	       PAGE_SIZE - prime_pause - 16);
	free_page((unsigned long)staged_ring);
	staged_ring = NULL;
	chrome9_flush_command_memory(ring, PAGE_SIZE);
	dma_wmb();
	/*
	 * On the live 7122, an A64/A65-only extension fetched to the new PAUSE
	 * but did not dispatch the appended packets.  Re-arm the owned paused
	 * reader at the first newly published byte; direct paused-ring
	 * replacement is independently proven on this target.
	 */
	chrome9_program_cr_ring(cdev, (prime_pause + 16) | 1,
				PAGE_SIZE, pause, 0);
	/*
	 * 043c can already have prefetched the suffix while the reader is
	 * paused.  For a guarded fill, the mode latch is the first reliable
	 * proof that the phase-2 commands were dispatched.
	 */
	if (do_2d_fill)
		dispatch_ret = readl_poll_timeout(
			cdev->mmio + CHROME9_2D_REG_MODE, observed_2d_mode,
			observed_2d_mode == CHROME9_2D_MODE_32BPP,
			10, 100000);
	pause_ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS,
				      ring_status, ring_status & BIT(31),
				      10, 100000);
	idle_ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
				     engine_status,
				     !(engine_status & CHROME9_ENGINE_WAIT_MASK),
				     10, 100000);
read_results:
	current_after = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	ring_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	engine_status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	engine_error_after = readl(cdev->mmio + CHROME9_REG_ENGINE_ERROR);
	status_45c = readl(cdev->mmio + 0x45c);
	status_460 = readl(cdev->mmio + 0x460);
	status_464 = readl(cdev->mmio + 0x464);
	if (chrome9_selftest_cr_observe_2d || do_2d_fill) {
		observed_2d_mode = readl(cdev->mmio + CHROME9_2D_REG_MODE);
		observed_2d_pitch = readl(cdev->mmio + CHROME9_2D_REG_PITCH);
		observed_2d_dimension =
			readl(cdev->mmio + CHROME9_2D_REG_DIMENSION);
		observed_2d_dst_pos =
			readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
		observed_2d_dst_base =
			readl(cdev->mmio + CHROME9_2D_REG_DST_BASE);
		observed_2d_color = readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);
		observed_2d_command =
			readl(cdev->mmio + CHROME9_2D_REG_COMMAND);
		writel(0, cdev->mmio + CHROME9_2D_REG_COMMAND);
		writel(saved_2d_mode, cdev->mmio + CHROME9_2D_REG_MODE);
		writel(saved_2d_pitch, cdev->mmio + CHROME9_2D_REG_PITCH);
		writel(saved_2d_dimension,
		       cdev->mmio + CHROME9_2D_REG_DIMENSION);
		writel(saved_2d_dst_pos, cdev->mmio + CHROME9_2D_REG_DST_POS);
		writel(saved_2d_dst_base, cdev->mmio + CHROME9_2D_REG_DST_BASE);
		writel(saved_2d_color, cdev->mmio + CHROME9_2D_REG_FG_COLOR);
		if (do_2d_fill) {
			writel(saved_2d_sync, cdev->mmio + 0x6c);
			writel(saved_2d_transparency, cdev->mmio + 0x48);
			writel(saved_2d_410_30, cdev->mmio + 0x30);
			writel(saved_2d_410_34, cdev->mmio + 0x34);
			writel(saved_2d_410_38, cdev->mmio + 0x38);
		}
		readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);
	}
	if (do_2d_fill) {
		for (i = 0; i < CHROME9_SELFTEST_SIZE; i += sizeof(u32)) {
			u32 pixel = readl(test + i);

			if (!i)
				first_pixel = pixel;
			last_pixel = pixel;
			unchanged_pixels += pixel == CHROME9_SELFTEST_FILL;
			zero_pixels += pixel == 0;
			if (do_3d_texture && !do_3d_sf) {
				texture_changed_pixels +=
					pixel != CHROME9_SELFTEST_SENTINEL;
				bad_pixels +=
					pixel == CHROME9_SELFTEST_SENTINEL;
			} else if (chrome9_selftest_3d_quad && !do_3d_sf) {
				u32 expected = chrome9_selftest_3d_store == 7 ?
					(i / sizeof(u32)) % CHROME9_SELFTEST_WIDTH < 16 ?
					0x00ff0000 : 0x000000ff : 0x000000ff;

				bad_pixels += pixel != expected;
			}
			else
				bad_pixels += pixel != CHROME9_SELFTEST_FILL;
		}
		if (do_3d_texture && !do_3d_sf) {
			texture_samples[0] = readl(test +
				(8 * CHROME9_SELFTEST_PITCH + 8 * sizeof(u32)));
			texture_samples[1] = readl(test +
				(8 * CHROME9_SELFTEST_PITCH + 24 * sizeof(u32)));
			texture_samples[2] = readl(test +
				(24 * CHROME9_SELFTEST_PITCH + 24 * sizeof(u32)));
			texture_samples[3] = readl(test +
				(24 * CHROME9_SELFTEST_PITCH + 8 * sizeof(u32)));
		}
		for (i = 0; i < guard_size; i += sizeof(u32)) {
			bad_guard += readl(test - guard_size + i) !=
				     CHROME9_SELFTEST_GUARD_WORD;
			bad_guard += readl(test + CHROME9_SELFTEST_SIZE + i) !=
				     CHROME9_SELFTEST_GUARD_WORD;
		}
		memcpy_toio(test - guard_size, test_saved,
			    CHROME9_SELFTEST_SIZE + 2 * guard_size);
		readl(test - guard_size);
		kfree(test_saved);
		test_saved = NULL;
	}
	if (do_3d_texture && !do_3d_sf) {
		for (i = 0; i < PAGE_SIZE; i += sizeof(u32)) {
			texture_bad_source += readl(texture + i) !=
				chrome9_selftest_texture_pixel(i / sizeof(u32));
			texture_bad_guard += readl(texture - PAGE_SIZE + i) !=
				CHROME9_SELFTEST_GUARD_WORD;
			texture_bad_guard += readl(texture + PAGE_SIZE + i) !=
				CHROME9_SELFTEST_GUARD_WORD;
		}
		memcpy_toio(texture - PAGE_SIZE, texture_saved, 3 * PAGE_SIZE);
		readl(texture - PAGE_SIZE);
		kfree(texture_saved);
		texture_saved = NULL;
	}
	if (do_3d_sf) {
		dma_rmb();
		if (sf_sgt)
			dma_sync_sgtable_for_cpu(&pdev->dev, sf_sgt,
						 DMA_BIDIRECTIONAL);
		/* Invalidate clean CPU lines before observing GPU writeback. */
		chrome9_flush_command_memory(sf_allocation,
					     sf_page_count * PAGE_SIZE);
		for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
			u32 pixel = READ_ONCE(sf_target[i]);

			if (!i)
				sf_first_pixel = pixel;
			sf_last_pixel = pixel;
			if (do_3d_texture) {
				texture_changed_pixels +=
					pixel != CHROME9_SELFTEST_SENTINEL;
				sf_bad_pixels +=
					pixel == CHROME9_SELFTEST_SENTINEL;
				texture_bad_source += READ_ONCE(sf_texture[i]) !=
					chrome9_selftest_texture_pixel(i);
				texture_bad_guard += READ_ONCE(sf_target[
					2 * PAGE_SIZE / sizeof(u32) + i]) !=
					CHROME9_SELFTEST_GUARD_WORD;
				texture_bad_guard += READ_ONCE(sf_texture[
					PAGE_SIZE / sizeof(u32) + i]) !=
					CHROME9_SELFTEST_GUARD_WORD;
			} else {
				u32 expected = chrome9_selftest_3d_store == 7 ?
					i % CHROME9_SELFTEST_WIDTH < 16 ?
					0x00ff0000 : 0x000000ff : 0x000000ff;

				sf_bad_pixels += pixel != expected;
			}
			sf_bad_guard +=
				READ_ONCE(sf_allocation[i]) !=
				CHROME9_SELFTEST_GUARD_WORD;
			sf_bad_guard +=
				READ_ONCE(sf_target[PAGE_SIZE / sizeof(u32) + i]) !=
				CHROME9_SELFTEST_GUARD_WORD;
		}
		if (do_3d_texture) {
			texture_samples[0] = READ_ONCE(sf_target[
				8 * CHROME9_SELFTEST_WIDTH + 8]);
			texture_samples[1] = READ_ONCE(sf_target[
				8 * CHROME9_SELFTEST_WIDTH + 24]);
			texture_samples[2] = READ_ONCE(sf_target[
				24 * CHROME9_SELFTEST_WIDTH + 24]);
			texture_samples[3] = READ_ONCE(sf_target[
				24 * CHROME9_SELFTEST_WIDTH + 8]);
		}
	}
	if (chrome9_selftest_cr_observe_3d)
		dev_info(&pdev->dev,
			 "VX900 H5/H6 address-free 3D status packet completed engine_error=%08x->%08x\n",
			 engine_error_before, engine_error_after);
	if (!ret && chrome9_selftest_cr_observe_3d &&
	    engine_error_after != engine_error_before)
		ret = -EIO;
	if (chrome9_selftest_3d_store)
		dev_info(&pdev->dev,
			 "VX900 3D store probe=%u raw45c=%08x raw460=%08x raw464=%08x\n",
			 chrome9_selftest_3d_store, status_45c, status_460,
			 status_464);

	if (!ret)
		ret = prime_pause_ret ?: prime_idle_ret ?: dispatch_ret ?:
			pause_ret ?: idle_ret;
	retain_ring = prime_pause_ret || pause_ret ||
		(engine_status & CHROME9_ENGINE_WAIT_MASK);

	/*
	 * Keep the idle reader paused.  Firmware also leaves 0444.bit31 set,
	 * and VIA's ring setup replaces a paused reader in place.  Changing its
	 * pause ID to STOP without immediately installing another ring leaves
	 * 0400.bit4 asserted on VX900 and is not a teardown operation.
	 */
	if (!retain_ring && !was_master)
		pci_clear_master(pdev);

	dev_info(&pdev->dev,
		 "VX900 PCIe CR prime pause=%#x current=%08x status=%08x engine=%08x pause_ret=%d idle_ret=%d dispatch_ret=%d\n",
		 prime_pause, prime_current, prime_ring_status,
		 prime_engine_status, prime_pause_ret, prime_idle_ret,
		 dispatch_ret);
	dev_info(&pdev->dev,
		 "VX900 PCIe CR ring dma=%pad pte=%08x gart=%#lx pause=%#x current=%08x->%08x cr=%08x engine=%08x pause_ret=%d idle_ret=%d\n",
		 &ring_dma, pte, chrome9_selftest_gart_offset, pause,
		 current_before, current_after, ring_status, engine_status,
		 pause_ret, idle_ret);
	if (chrome9_selftest_cr_observe_2d) {
		dev_info(&pdev->dev,
			 "VX900 header0 2D latches mode=%08x->%08x color=%08x->%08x (restored; color may be write-only)\n",
			 saved_2d_mode, observed_2d_mode, saved_2d_color,
			 observed_2d_color);
		if (!ret && observed_2d_mode != CHROME9_2D_MODE_32BPP)
			ret = -EIO;
	}
	if (do_2d_fill) {
		if (chrome9_selftest_3d_quad)
			dev_info(&pdev->dev,
				 "VX900 H6 2D prefill submitted offset=%#lx mode=%08x pitch=%08x dim=%08x pos=%08x base=%08x color=%08x cmd=%08x\n",
				 chrome9_selftest_2d_offset, observed_2d_mode,
				 observed_2d_pitch, observed_2d_dimension,
				 observed_2d_dst_pos, observed_2d_dst_base,
				 observed_2d_color, observed_2d_command);
		else
			dev_info(&pdev->dev,
				 "VX900 H6 2D fill offset=%#lx mode=%08x pitch=%08x dim=%08x pos=%08x base=%08x color=%08x cmd=%08x bad_pixels=%u bad_guard=%u restored=yes\n",
				 chrome9_selftest_2d_offset, observed_2d_mode,
				 observed_2d_pitch, observed_2d_dimension,
				 observed_2d_dst_pos, observed_2d_dst_base,
				 observed_2d_color, observed_2d_command,
				 bad_pixels, bad_guard);
		if (!ret && (bad_pixels || bad_guard))
			ret = -EIO;
	}
	if (chrome9_selftest_3d_quad) {
		if (do_3d_texture && do_3d_sf) {
			dev_info(&pdev->dev,
				 "VX900 H6 SF one-texture quad src=0x5000 dst=0x2000 dma=%pad ptes=%08x/%08x/%08x/%08x/%08x/%08x changed=%u samples=%08x/%08x/%08x/%08x bad_pixels=%u bad_source=%u bad_guard=%u; SL 2D bad_pixels=%u bad_guard=%u restored=yes\n",
				 &sf_dma, sf_ptes[0], sf_ptes[1], sf_ptes[2],
				 sf_ptes[3], sf_ptes[4], sf_ptes[5],
				 texture_changed_pixels, texture_samples[0],
				 texture_samples[1], texture_samples[2],
				 texture_samples[3], sf_bad_pixels,
				 texture_bad_source,
				 texture_bad_guard + sf_bad_guard,
				 bad_pixels, bad_guard);
		} else if (do_3d_texture) {
			dev_info(&pdev->dev,
				 "VX900 H6 local one-texture quad src=%#lx dst=%#lx changed=%u samples=%08x/%08x/%08x/%08x bad_pixels=%u bad_source=%u bad_guard=%u restored=yes\n",
				 texture_offset, chrome9_selftest_2d_offset,
				 texture_changed_pixels, texture_samples[0],
				 texture_samples[1], texture_samples[2],
				 texture_samples[3], bad_pixels,
				 texture_bad_source,
				 texture_bad_guard + bad_guard);
		} else if (do_3d_sf) {
			dev_info(&pdev->dev,
				 "VX900 H6 SF 3D %s quad gpu=0x2000 dma=%pad ptes=%08x/%08x/%08x first=%08x last=%08x solid_pixels=%u bad_guard=%u; SL 2D bad_pixels=%u bad_guard=%u restored=yes\n",
				 chrome9_selftest_3d_store == 7 ?
				 "c15*c16/c31" : "blue",
				 &sf_dma, sf_ptes[0], sf_ptes[1], sf_ptes[2],
				 sf_first_pixel, sf_last_pixel,
				 (unsigned int)(PAGE_SIZE / sizeof(u32)) -
				 sf_bad_pixels, sf_bad_guard, bad_pixels,
				 bad_guard);
		} else {
			dev_info(&pdev->dev,
				 "VX900 H6 3D %s quad offset=%#lx first=%08x last=%08x solid_pixels=%u unchanged_2d=%u zero=%u bad_guard=%u restored=yes\n",
				 chrome9_selftest_3d_store == 7 ?
				 "c15*c16/c31" : "blue",
				 chrome9_selftest_2d_offset, first_pixel,
				 last_pixel,
				 (unsigned int)(CHROME9_SELFTEST_SIZE /
				 sizeof(u32)) - bad_pixels,
				 unchanged_pixels, zero_pixels, bad_guard);
		}
	}
	if (!ret && do_3d_sf && (sf_bad_pixels || sf_bad_guard))
		ret = -EIO;
	if (!ret && do_3d_texture &&
	    (bad_pixels || texture_bad_source || texture_bad_guard || bad_guard ||
	     texture_samples[0] != 0x00ff0000 ||
	     texture_samples[1] != 0x0000ff00 ||
	     texture_samples[2] != 0x000000ff ||
	     texture_samples[3] != 0x00ffffff))
		ret = -EIO;

out_restore:
	if (retain_ring) {
		/*
		 * The reader may still hold prefetched references.  Retain its DMA
		 * page, PTE, GTI setup, and PCI bus mastering until controlled reboot;
		 * freeing any of them would turn a bounded hang into memory corruption.
		 */
		dev_crit(&pdev->dev,
			 "CR is not safely retired; retaining ring/GART/BME until reboot\n");
		if (seq_locked)
			chrome9_vga_seq_put(cdev);
		if (test_saved) {
			memcpy_toio(test - guard_size, test_saved,
				    CHROME9_SELFTEST_SIZE + 2 * guard_size);
			readl(test - guard_size);
			kfree(test_saved);
		}
		if (texture_saved) {
			memcpy_toio(texture - PAGE_SIZE, texture_saved,
				    3 * PAGE_SIZE);
			readl(texture - PAGE_SIZE);
			kfree(texture_saved);
		}
		if (staged_ring)
			free_page((unsigned long)staged_ring);
		kvfree(gart_saved);
		return dev_err_probe(&pdev->dev, ret ?: -EBUSY,
				     "VX900 PCIe GART/CR pause test requires reboot\n");
	}
	if (!seq_locked) {
		int lock_ret = chrome9_vga_seq_get(cdev);

		if (lock_ret) {
			if (!ret)
				ret = lock_ret;
			goto out_free_ring;
		}
		seq_locked = true;
	}
	chrome9_seq_write_mmio(cdev, 0x6c, saved_6c & ~BIT(7));
	if (gart_modified) {
		memcpy_toio(gart, gart_saved, CHROME9_PCIE_GART_SIZE);
		readl(gart + CHROME9_PCIE_GART_SIZE - sizeof(u32));
		if (chrome9_gart_flush(cdev) && !ret)
			ret = -ETIMEDOUT;
	}
	chrome9_seq_write_mmio(cdev, 0x6a, saved_6a);
	chrome9_seq_write_mmio(cdev, 0x6b, saved_6b);
	chrome9_seq_write_mmio(cdev, 0x7b, saved_7b);
	chrome9_seq_write_mmio(cdev, 0x6c, saved_6c);
	writeb(saved_index, cdev->mmio + CHROME9_SEQ_INDEX);
	chrome9_vga_seq_put(cdev);
	seq_locked = false;

out_free_ring:
	if (seq_locked)
		chrome9_vga_seq_put(cdev);
	if (test_saved) {
		memcpy_toio(test - guard_size, test_saved,
			    CHROME9_SELFTEST_SIZE + 2 * guard_size);
		readl(test - guard_size);
		kfree(test_saved);
	}
	if (texture_saved) {
		memcpy_toio(texture - PAGE_SIZE, texture_saved, 3 * PAGE_SIZE);
		readl(texture - PAGE_SIZE);
		kfree(texture_saved);
	}
	if (staged_ring)
		free_page((unsigned long)staged_ring);
	if (sf_dma_gem) {
		drm_gem_object_put(&sf_dma_gem->base);
	} else if (sf_shmem) {
		if (sf_allocation)
			drm_gem_vunmap(&sf_shmem->base, &sf_map);
		drm_gem_object_put(&sf_shmem->base);
	} else if (sf_allocation) {
		dma_free_coherent(&pdev->dev, sf_page_count * PAGE_SIZE,
				  sf_allocation, sf_dma);
	}
	dma_free_coherent(&pdev->dev, PAGE_SIZE, ring, ring_dma);
out_free_backup:
	kvfree(gart_saved);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "VX900 PCIe GART/CR pause test failed\n");
	return 0;
}

static int chrome9_selftest_command_reader(struct chrome9_device *cdev)
{
	struct pci_dev *pdev = cdev->pdev;
	const bool use_sl = chrome9_selftest_cr_sl_offset != ULONG_MAX;
	u32 stream[16] = {
		0xfe020420, 0x0000041c, 0x00300000, 0xcc000000,
		0xccccccc0, 0xddd00000, 0xccccccc0, 0xddd00000,
	};
	dma_addr_t ring_dma;
	u32 *ring_cpu = NULL;
	void __iomem *sl_ring = NULL;
	u8 *sl_saved = NULL;
	u32 ring_addr;
	u32 before_current;
	u32 after_current;
	u32 cr_transset;
	u32 ring_status;
	u32 status;
	u32 saved_mode = 0;
	u32 saved_color = 0;
	u32 observed_mode = 0;
	u32 observed_color = 0;
	u32 pause_offset = 16;
	size_t stream_size = 32;
	int current_ret = -ETIMEDOUT;
	int idle_ret;
	int state_ret;

	if (!chrome9_selftest_cr && !use_sl)
		return 0;
	if (!cdev->mmio)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "CR self-test has no engine MMIO mapping\n");
	if ((chrome9_selftest_cr && use_sl) ||
	    chrome9_selftest_2d_offset != ULONG_MAX)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "CR self-test modes are mutually exclusive\n");
	status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	ring_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	if ((status & CHROME9_ENGINE_WAIT_MASK) || (ring_status & BIT(31)))
		return dev_err_probe(&pdev->dev, -EBUSY,
				     "refusing CR test: engine=%08x CR=%08x\n",
				     status, ring_status);
	if (chrome9_selftest_cr_observe_2d) {
		if (!use_sl)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "observable CR test requires an SL ring\n");
		saved_mode = readl(cdev->mmio + CHROME9_2D_REG_MODE);
		saved_color = readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);
		stream[0] = CHROME9_H1(CHROME9_2D_REG_MODE);
		stream[1] = CHROME9_2D_MODE_32BPP;
		stream[2] = CHROME9_H1(CHROME9_2D_REG_FG_COLOR);
		stream[3] = CHROME9_SELFTEST_FILL;
		stream[4] = 0xfe020420;
		stream[5] = 0x0000041c;
		stream[6] = 0x00300000;
		stream[7] = 0xcc000000;
		stream[8] = 0xccccccc0;
		stream[9] = 0xddd00000;
		stream[10] = 0xccccccc0;
		stream[11] = 0xddd00000;
		stream[12] = 0xccccccc0;
		stream[13] = 0xddd00000;
		stream[14] = 0xccccccc0;
		stream[15] = 0xddd00000;
		stream_size = sizeof(stream);
		pause_offset = 48;
	}

	if (use_sl) {
		if (!cdev->vram_size) {
			state_ret = chrome9_detect_vram(cdev);
			if (state_ret)
				return state_ret;
		}
		if (!IS_ALIGNED(chrome9_selftest_cr_sl_offset, PAGE_SIZE) ||
		    chrome9_selftest_cr_sl_offset > cdev->vram_size - PAGE_SIZE)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid SL ring offset %#lx\n",
					     chrome9_selftest_cr_sl_offset);
		cdev->vram = pcim_iomap(pdev, 2, 0);
		if (!cdev->vram)
			return -ENOMEM;
		sl_ring = cdev->vram + chrome9_selftest_cr_sl_offset;
		sl_saved = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!sl_saved)
			return -ENOMEM;
		memcpy_fromio(sl_saved, sl_ring, PAGE_SIZE);
		memset_io(sl_ring, 0xcc, PAGE_SIZE);
		memcpy_toio(sl_ring, stream, stream_size);
		readl(sl_ring + stream_size - sizeof(u32));
		if (chrome9_selftest_cr_sl_physical) {
			if (cdev->sl_base + chrome9_selftest_cr_sl_offset > U32_MAX) {
				memcpy_toio(sl_ring, sl_saved, PAGE_SIZE);
				readl(sl_ring);
				kfree(sl_saved);
				return dev_err_probe(&pdev->dev, -ERANGE,
						     "physical SL ring exceeds 32 bits\n");
			}
			ring_addr = cdev->sl_base +
				    chrome9_selftest_cr_sl_offset;
		} else {
			ring_addr = chrome9_selftest_cr_sl_offset;
		}
	} else {
		ring_cpu = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &ring_dma,
					      GFP_KERNEL);
		if (!ring_cpu)
			return -ENOMEM;
		if (ring_dma + PAGE_SIZE > U32_MAX) {
			dma_free_coherent(&pdev->dev, PAGE_SIZE, ring_cpu,
					  ring_dma);
			return dev_err_probe(&pdev->dev, -ERANGE,
					     "SM command ring address exceeds 32 bits\n");
		}
		memcpy(ring_cpu, stream, stream_size);
		memset((u8 *)ring_cpu + stream_size, 0xcc,
		       PAGE_SIZE - stream_size);
		dma_wmb();
		ring_addr = ring_dma;
	}

	before_current = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	if (!use_sl)
		pci_set_master(pdev);
	/* SL is location 00b; a direct DMA address uses SM location 10b. */
	chrome9_program_cr_ring(cdev, ring_addr | (use_sl ? 0 : 2),
				ring_addr + PAGE_SIZE - 16,
				ring_addr + pause_offset, 2);
	state_ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS,
				       ring_status, ring_status & BIT(31),
				       10, 100000);
	if (state_ret) {
		current_ret = readl_poll_timeout(cdev->mmio +
						 CHROME9_REG_FE_TRANSSET,
						 after_current,
						 after_current >= ring_addr &&
						 after_current <
						 ring_addr + PAGE_SIZE,
						 10, 100000);
		if (!current_ret)
			state_ret = 0;
	}
	idle_ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_ENGINE_STATUS,
				      status,
				      !(status & CHROME9_ENGINE_WAIT_MASK),
				      10, 100000);
	status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	cr_transset = readl(cdev->mmio + CHROME9_REG_CR_TRANSSET);
	ring_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	after_current = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	if (chrome9_selftest_cr_observe_2d) {
		observed_mode = readl(cdev->mmio + CHROME9_2D_REG_MODE);
		observed_color = readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);
		if (observed_mode == CHROME9_2D_MODE_32BPP &&
		    observed_color == CHROME9_SELFTEST_FILL)
			state_ret = 0;
		writel(saved_mode, cdev->mmio + CHROME9_2D_REG_MODE);
		writel(saved_color, cdev->mmio + CHROME9_2D_REG_FG_COLOR);
		readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);
	}
	if (!use_sl) {
		pci_clear_master(pdev);
		dma_free_coherent(&pdev->dev, PAGE_SIZE, ring_cpu, ring_dma);
	} else if (!state_ret && !idle_ret) {
		memcpy_toio(sl_ring, sl_saved, PAGE_SIZE);
		readl(sl_ring);
	}
	kfree(sl_saved);

	dev_info(&pdev->dev,
		 "CR stop stream %s ring=%#x current=%08x->%08x cr_set=%08x cr_status=%08x engine=%08x mode=%08x color=%08x state_ret=%d current_ret=%d idle_ret=%d restored=%s\n",
		 use_sl ? "SL" : "SM", ring_addr, before_current,
		 after_current, cr_transset, ring_status, status, observed_mode,
		 observed_color, state_ret, current_ret, idle_ret,
		 use_sl ? ((!state_ret && !idle_ret) ? "yes" : "no") : "n/a");
	if (state_ret)
		return dev_err_probe(&pdev->dev, state_ret,
				     "CR did not report the submitted stop stream\n");
	if (idle_ret)
		return dev_err_probe(&pdev->dev, idle_ret,
				     "CR stop stream did not become idle\n");
	return 0;
}

static int chrome9_selftest_2d(struct chrome9_device *cdev)
{
	struct pci_dev *pdev = cdev->pdev;
	const unsigned long offset = chrome9_selftest_2d_offset;
	void __iomem *test;
	u8 *saved;
	u32 saved_mode;
	u32 saved_pitch;
	u32 saved_dimension;
	u32 saved_dst_pos;
	u32 saved_dst_base;
	u32 saved_fg_color;
	u32 commands[24];
	u32 *ring_cpu;
	dma_addr_t ring_dma;
	u32 ring_start;
	u32 ring_end;
	u32 ring_pause;
	u32 cr_transset;
	u32 fe_transset;
	u32 ring_status;
	u32 status;
	unsigned int bad_guard = 0;
	unsigned int bad_pixels = 0;
	unsigned int i;
	int active_ret;
	int idle_ret = 0;
	int ret;

	/* The PCIe-ring path consumes this offset with proper GTI translation. */
	if (chrome9_selftest_cr_pcie)
		return 0;
	if (offset == ULONG_MAX)
		return 0;
	if (!cdev->mmio)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "2D self-test has no engine MMIO mapping\n");
	if (!cdev->vram_size) {
		ret = chrome9_detect_vram(cdev);
		if (ret)
			return ret;
	}
	status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	ring_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	if ((status & CHROME9_ENGINE_WAIT_MASK) || (ring_status & BIT(31)))
		return dev_err_probe(&pdev->dev, -EBUSY,
				     "refusing 2D test: engine=%08x CR=%08x\n",
				     status, ring_status);
	if (!(pci_resource_flags(pdev, 2) & IORESOURCE_MEM) ||
	    cdev->vram_size < CHROME9_SELFTEST_SIZE +
			      2 * CHROME9_SELFTEST_GUARD ||
	    offset < CHROME9_SELFTEST_GUARD ||
	    !IS_ALIGNED(offset, 64) ||
	    offset > cdev->vram_size - CHROME9_SELFTEST_SIZE -
		     CHROME9_SELFTEST_GUARD)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid BAR2 self-test offset %#lx\n", offset);

	cdev->vram = pcim_iomap(pdev, 2, 0);
	if (!cdev->vram)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "cannot map BAR2 for 2D self-test\n");
	test = cdev->vram + offset;
	ring_cpu = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &ring_dma,
				      GFP_KERNEL);
	if (!ring_cpu)
		return -ENOMEM;
	if (ring_dma + PAGE_SIZE > U32_MAX) {
		dma_free_coherent(&pdev->dev, PAGE_SIZE, ring_cpu, ring_dma);
		return dev_err_probe(&pdev->dev, -ERANGE,
				     "SM command ring address exceeds 32 bits\n");
	}
	ring_start = ring_dma;
	ring_end = ring_start + PAGE_SIZE - 16;
	ring_pause = ring_start + sizeof(commands) - 16;
	saved = kmalloc(CHROME9_SELFTEST_SIZE + 2 * CHROME9_SELFTEST_GUARD,
			GFP_KERNEL);
	if (!saved)
		goto err_free_ring;

	memcpy_fromio(saved, test - CHROME9_SELFTEST_GUARD,
		      CHROME9_SELFTEST_SIZE + 2 * CHROME9_SELFTEST_GUARD);
	for (i = 0; i < CHROME9_SELFTEST_GUARD; i += sizeof(u32)) {
		writel(CHROME9_SELFTEST_GUARD_WORD,
		       test - CHROME9_SELFTEST_GUARD + i);
		writel(CHROME9_SELFTEST_GUARD_WORD,
		       test + CHROME9_SELFTEST_SIZE + i);
	}
	for (i = 0; i < CHROME9_SELFTEST_SIZE; i += sizeof(u32))
		writel(CHROME9_SELFTEST_SENTINEL, test + i);

	saved_mode = readl(cdev->mmio + CHROME9_2D_REG_MODE);
	saved_pitch = readl(cdev->mmio + CHROME9_2D_REG_PITCH);
	saved_dimension = readl(cdev->mmio + CHROME9_2D_REG_DIMENSION);
	saved_dst_pos = readl(cdev->mmio + CHROME9_2D_REG_DST_POS);
	saved_dst_base = readl(cdev->mmio + CHROME9_2D_REG_DST_BASE);
	saved_fg_color = readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);

	commands[0] = CHROME9_H1(CHROME9_2D_REG_MODE);
	commands[1] = CHROME9_2D_MODE_32BPP;
	commands[2] = CHROME9_H1(CHROME9_2D_REG_PITCH);
	commands[3] = (CHROME9_SELFTEST_PITCH >> 3) << 16;
	commands[4] = CHROME9_H1(CHROME9_2D_REG_DIMENSION);
	commands[5] = ((CHROME9_SELFTEST_HEIGHT - 1) << 16) |
		      (CHROME9_SELFTEST_WIDTH - 1);
	commands[6] = CHROME9_H1(CHROME9_2D_REG_DST_BASE);
	commands[7] = offset >> 3;
	commands[8] = CHROME9_H1(CHROME9_2D_REG_FG_COLOR);
	commands[9] = CHROME9_SELFTEST_FILL;
	commands[10] = CHROME9_H1(CHROME9_2D_REG_COMMAND);
	commands[11] = CHROME9_2D_COMMAND_FILL;
	commands[12] = CHROME9_H1(CHROME9_2D_REG_MODE);
	commands[13] = CHROME9_2D_MODE_32BPP;
	commands[14] = CHROME9_H1(CHROME9_2D_REG_DST_POS);
	commands[15] = 0;
	/* Exact VX900 KMD 32-byte-aligned CR stop trailer. */
	commands[16] = 0xfe020420;
	commands[17] = 0x0000041c;
	commands[18] = 0x00300000;
	commands[19] = 0xcc000000;
	commands[20] = 0xccccccc0;
	commands[21] = 0xddd00000;
	commands[22] = 0xccccccc0;
	commands[23] = 0xddd00000;

	memcpy(ring_cpu, commands, sizeof(commands));
	/* Publish the coherent SM command buffer before programming CR. */
	dma_wmb();

	/* CR type 10h: describe and start the SM ring. */
	pci_set_master(pdev);
	chrome9_program_cr_ring(cdev, ring_start | 2, ring_end, ring_pause, 2);
	/* The Windows KMD tests 444h bit 31 for an initialized CR. */
	active_ret = readl_poll_timeout(cdev->mmio + CHROME9_REG_CR_STATUS,
					ring_status, ring_status & BIT(31),
					10, 100000);
	if (!active_ret)
		idle_ret = readl_poll_timeout(cdev->mmio +
					       CHROME9_REG_ENGINE_STATUS,
					       status,
					       !(status &
						 CHROME9_ENGINE_WAIT_MASK),
					       10, 100000);
	ret = active_ret ?: idle_ret;
	status = readl(cdev->mmio + CHROME9_REG_ENGINE_STATUS);
	cr_transset = readl(cdev->mmio + CHROME9_REG_CR_TRANSSET);
	fe_transset = readl(cdev->mmio + CHROME9_REG_FE_TRANSSET);
	ring_status = readl(cdev->mmio + CHROME9_REG_CR_STATUS);
	pci_clear_master(pdev);
	writel(0, cdev->mmio + CHROME9_2D_REG_COMMAND);
	writel(saved_mode, cdev->mmio + CHROME9_2D_REG_MODE);
	writel(saved_pitch, cdev->mmio + CHROME9_2D_REG_PITCH);
	writel(saved_dimension, cdev->mmio + CHROME9_2D_REG_DIMENSION);
	writel(saved_dst_pos, cdev->mmio + CHROME9_2D_REG_DST_POS);
	writel(saved_dst_base, cdev->mmio + CHROME9_2D_REG_DST_BASE);
	writel(saved_fg_color, cdev->mmio + CHROME9_2D_REG_FG_COLOR);
	readl(cdev->mmio + CHROME9_2D_REG_FG_COLOR);

	for (i = 0; i < CHROME9_SELFTEST_SIZE; i += sizeof(u32))
		bad_pixels += readl(test + i) != CHROME9_SELFTEST_FILL;
	for (i = 0; i < CHROME9_SELFTEST_GUARD; i += sizeof(u32)) {
		bad_guard += readl(test - CHROME9_SELFTEST_GUARD + i) !=
			     CHROME9_SELFTEST_GUARD_WORD;
		bad_guard += readl(test + CHROME9_SELFTEST_SIZE + i) !=
			     CHROME9_SELFTEST_GUARD_WORD;
	}

	memcpy_toio(test - CHROME9_SELFTEST_GUARD, saved,
		    CHROME9_SELFTEST_SIZE + 2 * CHROME9_SELFTEST_GUARD);
	readl(test - CHROME9_SELFTEST_GUARD);
	kfree(saved);
	dma_free_coherent(&pdev->dev, PAGE_SIZE, ring_cpu, ring_dma);
	dev_info(&pdev->dev,
		 "2D SM ring fill offset=%#lx ring=%#x cr_set=%08x fe_set=%08x cr_status=%08x engine=%08x active_ret=%d idle_ret=%d bad_pixels=%u bad_guard=%u restored=yes bus-master-cleared=yes\n",
		 offset, ring_start, cr_transset, fe_transset, ring_status,
		 status, active_ret, idle_ret, bad_pixels, bad_guard);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "2D ring did not become active and idle\n");
	if (bad_pixels || bad_guard)
		return dev_err_probe(&pdev->dev, -EIO,
				     "2D fill verification failed\n");
	return 0;

err_free_ring:
	dma_free_coherent(&pdev->dev, PAGE_SIZE, ring_cpu, ring_dma);
	return -ENOMEM;
}

static int chrome9_get_param_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct drm_chrome9_get_param *args = data;
	struct chrome9_device *cdev = to_chrome9(drm);
	unsigned int bar;
	int dev_idx;
	int ret = 0;

	if (args->pad)
		return -EINVAL;
	if (!drm_dev_enter(drm, &dev_idx))
		return -ENODEV;

	switch (args->param) {
	case DRM_CHROME9_PARAM_DEVICE_ID:
		args->value = cdev->pdev->device;
		break;
	case DRM_CHROME9_PARAM_REVISION:
		args->value = cdev->pdev->revision;
		break;
	case DRM_CHROME9_PARAM_CAPABILITIES:
		args->value = DRM_CHROME9_CAP_GEM |
			DRM_CHROME9_CAP_GEM_SHMEM_CACHED;
		if (chrome9_dma32_scanout_supported(cdev))
			args->value |= DRM_CHROME9_CAP_GEM_DMA32_SCANOUT;
		if (chrome9_engine_ready(cdev)) {
			args->value |= DRM_CHROME9_CAP_FIXED_QUAD |
				       DRM_CHROME9_CAP_DRAW_TRIANGLE |
				       DRM_CHROME9_CAP_DRAW_TRIANGLE_D16 |
				       DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE |
				       DRM_CHROME9_CAP_DRAW_TRIANGLE_BLEND |
				       DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BLEND |
				       DRM_CHROME9_CAP_BLEND_BGRA8888 |
				       DRM_CHROME9_CAP_ALPHA_TEST |
				       DRM_CHROME9_CAP_SCISSOR |
				       DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_BLEND |
				       DRM_CHROME9_CAP_DRAW_TRIANGLE_BATCH |
				       DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE |
				       DRM_CHROME9_CAP_DEPTH_FUNC |
				       DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16 |
				       DRM_CHROME9_CAP_TEXTURED_D16_BGRA8888 |
				       DRM_CHROME9_CAP_TEXTURED_D16_SOURCE_OVER |
				       DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_BATCH |
				       DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16 |
			       DRM_CHROME9_CAP_DEPTH_READ_ONLY |
			       DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND |
			       DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND |
			       DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS |
			       DRM_CHROME9_CAP_DEPTH_EQUAL |
			       DRM_CHROME9_CAP_FRAGMENT_CONSTANTS_32 |
			       DRM_CHROME9_CAP_DEPTH_ALWAYS |
			       DRM_CHROME9_CAP_TWO_TEXTURE_TEX0_ALPHA_PRIMARY |
			       DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_RUN |
			       DRM_CHROME9_CAP_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH |
			       DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_BATCH |
			       DRM_CHROME9_CAP_COPY_RECT |
			       DRM_CHROME9_CAP_COPY_RECT_UNGUARDED_DESTINATION;
			if (chrome9_textured_triangle_run_enabled())
				args->value |=
					DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_RUN;
			if (chrome9_textured_triangle_mip_enabled())
				args->value |=
					DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_MIP;
			if (chrome9_rhw_chain_enabled())
				args->value |=
					DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_CHAIN;
			if (chrome9_ps_add_enabled())
				args->value |= DRM_CHROME9_CAP_FRAGMENT_ADD;
			args->value |= DRM_CHROME9_CAP_DRAW_FRAGMENT_PROGRAM;
			args->value |= DRM_CHROME9_CAP_FRAGMENT_EXTENDED_LINKAGE;
			args->value |=
				DRM_CHROME9_CAP_FRAGMENT_TEXTURE_LEGACY_FORMATS;
			args->value |= DRM_CHROME9_CAP_FRAGMENT_KILL;
			args->value |= DRM_CHROME9_CAP_FRAGMENT_POSITION;
			args->value |=
				DRM_CHROME9_CAP_FRAGMENT_DESTINATION_EXTENDED;
			args->value |= DRM_CHROME9_CAP_FRAGMENT_PRIMARY;
			args->value |= DRM_CHROME9_CAP_FRAGMENT_STENCIL;
			if (chrome9_fragment_sparse_textures_enabled())
				args->value |=
					DRM_CHROME9_CAP_FRAGMENT_SPARSE_TEXTURES;
			if (chrome9_two_texture_signed_repeat)
				args->value |=
					DRM_CHROME9_CAP_TWO_TEXTURE_SIGNED_REPEAT_COORDS;
			if (chrome9_copy_rect_preserve_alpha)
				args->value |=
					DRM_CHROME9_CAP_COPY_RECT_PRESERVE_ALPHA;
		}
		break;
	case DRM_CHROME9_PARAM_BAR0_SIZE:
	case DRM_CHROME9_PARAM_BAR1_SIZE:
	case DRM_CHROME9_PARAM_BAR2_SIZE:
		bar = args->param - DRM_CHROME9_PARAM_BAR0_SIZE;
		args->value = pci_resource_len(cdev->pdev, bar);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	drm_dev_exit(dev_idx);
	return ret;
}

static const struct drm_ioctl_desc chrome9_ioctls[] = {
	DRM_IOCTL_DEF_DRV(CHROME9_GET_PARAM, chrome9_get_param_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_GEM_CREATE, chrome9_gem_create_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_FIXED_QUAD, chrome9_fixed_quad_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE, chrome9_draw_triangle_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_D16,
			  chrome9_draw_triangle_d16_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE,
			  chrome9_draw_textured_triangle_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_BLEND,
			  chrome9_draw_triangle_blend_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE_BLEND,
			  chrome9_draw_textured_triangle_blend_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_D16_BLEND,
			  chrome9_draw_triangle_d16_blend_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_BATCH,
			  chrome9_draw_triangle_batch_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TWO_TEXTURED_TRIANGLE,
			  chrome9_draw_two_textured_triangle_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE_D16,
			  chrome9_draw_textured_triangle_d16_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_D16_RHW_BATCH,
			  chrome9_draw_triangle_d16_rhw_batch_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16,
			  chrome9_draw_two_textured_triangle_d16_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE_MIP,
			  chrome9_draw_textured_triangle_mip_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_D16_RHW_RUN,
			  chrome9_draw_triangle_d16_rhw_run_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE_D16_BLEND,
			  chrome9_draw_textured_triangle_d16_blend_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND,
			  chrome9_draw_two_textured_triangle_d16_blend_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE_RUN,
			  chrome9_draw_textured_triangle_run_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_PROGRAM_TRIANGLE_D16_RHW_BATCH,
			  chrome9_draw_program_triangle_d16_rhw_batch_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TEXTURED_TRIANGLE_BATCH,
			  chrome9_draw_textured_triangle_batch_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_COPY_RECT, chrome9_copy_rect_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_TRIANGLE_D16_RHW_CHAIN,
			  chrome9_draw_triangle_d16_rhw_chain_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CHROME9_DRAW_FRAGMENT_PROGRAM,
			  chrome9_draw_fragment_program_ioctl,
			  DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(chrome9_fops);

static const struct drm_driver chrome9_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.fops = &chrome9_fops,
	.ioctls = chrome9_ioctls,
	.num_ioctls = ARRAY_SIZE(chrome9_ioctls),
	DRM_GEM_SHMEM_DRIVER_OPS,
	.name = CHROME9_DRIVER_NAME,
	.desc = CHROME9_DRIVER_DESC,
	.major = 0,
	.minor = 1,
};

static const struct drm_driver chrome9_kms_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET |
			   DRIVER_ATOMIC,
	.fops = &chrome9_fops,
	.ioctls = chrome9_ioctls,
	.num_ioctls = ARRAY_SIZE(chrome9_ioctls),
	.gem_prime_import = drm_gem_shmem_prime_import_no_map,
	.dumb_create = chrome9_gem_dumb_create,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	.name = CHROME9_DRIVER_NAME,
	.desc = CHROME9_DRIVER_DESC,
	.major = 0,
	.minor = 1,
};

static int chrome9_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct chrome9_device *cdev;
	const struct drm_driver *driver;
	int ret;

	if (!chrome9_enable)
		return dev_err_probe(&pdev->dev, -ENODEV,
			"driver disabled; use chrome9.enable=1 for bring-up\n");
	if (chrome9_fixed_iga1 && !chrome9_kms)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"fixed_iga1 requires kms=1\n");
	if (chrome9_staging_scanout && !chrome9_fixed_iga1)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"staging_scanout requires fixed_iga1=1\n");
	/*
	 * vgaarb only serializes legacy-port accesses.  It does not stop vgacon
	 * from rewriting the IGA1 timing and base after a KMS update, so take
	 * exclusive display ownership before the first IGA1 operation.
	 * Keep render-only and disconnected staging probes non-destructive.
	 */
	if (chrome9_fixed_iga1) {
		ret = aperture_remove_conflicting_pci_devices(pdev,
							CHROME9_DRIVER_NAME);
		if (ret)
			return ret;
	}

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	if (chrome9_dma_bits < 24 || chrome9_dma_bits > 32)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"dma_bits must be between 24 and 32\n");
	ret = dma_set_mask_and_coherent(&pdev->dev,
					DMA_BIT_MASK(chrome9_dma_bits));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "%u-bit DMA is unavailable\n",
				     chrome9_dma_bits);

	driver = chrome9_kms ? &chrome9_kms_drm_driver : &chrome9_drm_driver;
	cdev = devm_drm_dev_alloc(&pdev->dev, driver,
				  struct chrome9_device, drm);
	if (IS_ERR(cdev))
		return PTR_ERR(cdev);
	cdev->pdev = pdev;
	mutex_init(&cdev->seq_lock);
	ret = chrome9_read_status(cdev);
	if (ret)
		return ret;
	ret = chrome9_detect_vram(cdev);
	if (ret)
		return ret;
	ret = chrome9_recover_2d_engine(cdev);
	if (ret)
		return ret;
	ret = chrome9_selftest_command_reader(cdev);
	if (ret)
		return ret;
	ret = chrome9_selftest_pcie_ring(cdev);
	if (ret)
		return ret;
	ret = chrome9_selftest_2d(cdev);
	if (ret)
		return ret;
	ret = chrome9_engine_init(cdev);
	if (ret)
		return ret;
	if (chrome9_fixed_iga1 && !chrome9_engine_ready(cdev)) {
		chrome9_engine_fini(cdev);
		return dev_err_probe(&pdev->dev, -EINVAL,
			"fixed_iga1 requires engine=1 to establish known T11 state\n");
	}
	if (chrome9_kms) {
		ret = chrome9_kms_init(cdev, chrome9_scanout_offset,
				       chrome9_scanout_size,
				       chrome9_fixed_iga1,
				       chrome9_staging_scanout);
		if (ret) {
			chrome9_engine_fini(cdev);
			return ret;
		}
	}

	ret = drm_dev_register(&cdev->drm, 0);
	if (ret) {
		if (chrome9_kms)
			(void)chrome9_kms_fini(cdev);
		chrome9_engine_fini(cdev);
		return ret;
	}

	pci_set_drvdata(pdev, &cdev->drm);
	if (chrome9_fixed_iga1)
		drm_client_setup_with_fourcc(&cdev->drm, DRM_FORMAT_XRGB8888);
	dev_info(&pdev->dev, "VX900 revision %02x: shmem GEM%s%s\n",
		 pdev->revision,
		 chrome9_engine_ready(cdev) ? ", validated 3D engine ready" : "",
		 chrome9_kms ? (chrome9_fixed_iga1 ?
		 ", native IGA1 KMS with DRM fbdev" :
		 ", disconnected KMS scaffold") : "");
	return 0;
}

static void chrome9_pci_remove(struct pci_dev *pdev)
{
	struct drm_device *drm = pci_get_drvdata(pdev);
	struct chrome9_device *cdev = to_chrome9(drm);

	if (!drm_dev_is_unplugged(drm))
		drm_dev_unplug(drm);
	if (chrome9_kms && !cdev->kms_shutdown) {
		drm_atomic_helper_shutdown(drm);
		(void)chrome9_kms_fini(cdev);
	}
	chrome9_engine_fini(cdev);
}

static DEFINE_MUTEX(chrome9_quiesce_lock);

/*
 * The bring-up engine keeps a module reference while its private GART and
 * ring are live.  Quiesce and restore the hardware before allowing rmmod;
 * on any timeout chrome9_engine_fini() retains the reference for safety.
 */
static ssize_t engine_quiesce_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct chrome9_device *cdev;
	bool quiesce;
	ssize_t ret;

	ret = kstrtobool(buf, &quiesce);
	if (ret)
		return ret;
	if (!quiesce)
		return -EINVAL;
	if (!drm)
		return -ENODEV;
	cdev = to_chrome9(drm);
	mutex_lock(&chrome9_quiesce_lock);
	if (atomic_read(&drm->open_count)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (chrome9_kms && !cdev->kms_shutdown) {
		drm_atomic_helper_shutdown(drm);
		ret = chrome9_kms_fini(cdev);
		if (ret)
			goto out_unlock;
		cdev->kms_shutdown = true;
	}
	if (!drm_dev_is_unplugged(drm))
		drm_dev_unplug(drm);
	chrome9_engine_fini(cdev);
	ret = cdev->engine ? -EBUSY : count;

out_unlock:
	mutex_unlock(&chrome9_quiesce_lock);

	return ret;
}
static DEVICE_ATTR_WO(engine_quiesce);

static struct attribute *chrome9_device_attrs[] = {
	&dev_attr_engine_quiesce.attr,
	NULL,
};
ATTRIBUTE_GROUPS(chrome9_device);

static const struct pci_device_id chrome9_pci_table[] = {
	{ PCI_DEVICE(CHROME9_PCI_VENDOR_ID, CHROME9_PCI_DEVICE_VX900) },
	{ }
};
MODULE_DEVICE_TABLE(pci, chrome9_pci_table);

static struct pci_driver chrome9_pci_driver = {
	.name = CHROME9_DRIVER_NAME,
	.id_table = chrome9_pci_table,
	.probe = chrome9_pci_probe,
	.remove = chrome9_pci_remove,
	.dev_groups = chrome9_device_groups,
};
module_pci_driver(chrome9_pci_driver);

MODULE_AUTHOR("Chrome9 contributors");
MODULE_DESCRIPTION(CHROME9_DRIVER_DESC);
MODULE_LICENSE("GPL");
