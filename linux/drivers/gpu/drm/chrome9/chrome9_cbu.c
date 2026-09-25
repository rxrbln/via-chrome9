// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * CBU windows (MMIO 0x1e00 + 0x20 * n, n = 0..15): the host bus interface
 * rewrites CPU accesses to the SL byte range [base, end) in place, from
 * linear order to the 32-byte x 8-row tile order.  Evidence and the probing
 * ladder: docs/research/chrome9-cbu-detile-re.md.
 */

#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/xxhash.h>

#include <drm/drm_device.h>
#include <drm/drm_managed.h>

#include <video/vga.h>

#include "chrome9_cbu.h"
#include "chrome9_drv.h"

#define CHROME9_CBU_REG_BASE	0x1e00
#define CHROME9_CBU_STRIDE	0x20
#define CHROME9_CBU_WINDOWS	16

enum {
	CHROME9_CBU_CTL,
	CHROME9_CBU_BASE,
	CHROME9_CBU_END,
	CHROME9_CBU_RECIP,
	CHROME9_CBU_PITCH,
	CHROME9_CBU_SIZE,
	CHROME9_CBU_FIELDS,
};

#define CHROME9_CBU_ENABLE	BIT(0)
#define CHROME9_CBU_BPP16	(1U << 1)
#define CHROME9_CBU_BPP32	(2U << 1)
#define CHROME9_CBU_TILE	BIT(5)
#define CHROME9_CBU_TILE16	BIT(7)

/* Pitch unit of the pitch and reciprocal fields in tile mode. */
enum {
	CHROME9_CBU_PITCH_PIXELS,
	CHROME9_CBU_PITCH_TILES,
};

static int chrome9_cbu_selftest_steps;
module_param_named(cbu_selftest, chrome9_cbu_selftest_steps, int, 0400);
MODULE_PARM_DESC(cbu_selftest,
	"Debug: run the CBU window ladder up to this step at load (0 = off, 1..12)");

static uint chrome9_cbu_skip;
module_param_named(cbu_skip, chrome9_cbu_skip, uint, 0400);
MODULE_PARM_DESC(cbu_skip, "Debug: bit n skips CBU ladder step n");

static uint chrome9_cbu_window = CHROME9_CBU_WINDOWS - 1;
module_param_named(cbu_window, chrome9_cbu_window, uint, 0400);
MODULE_PARM_DESC(cbu_window,
	"Debug: CBU window of the ladder, 3..15; step 10 also uses the one below (default: 15)");

static uint chrome9_cbu_route;
module_param_named(cbu_route, chrome9_cbu_route, uint, 0400);
MODULE_PARM_DESC(cbu_route,
	"Debug: CBU routing probe instead of steps 3..12: bit 0 probe, 1 D0F2 9E[1], 2 DFA D1F0 B9[1] (with bit 1), 3 SR42[7]");

static int chrome9_cbu_pitch_mode = -1;
module_param_named(cbu_pitch_mode, chrome9_cbu_pitch_mode, int, 0400);
MODULE_PARM_DESC(cbu_pitch_mode,
	"CBU tile pitch unit: 0 pixels, 1 32-byte columns, -1 decided by the ladder (default: -1)");

struct chrome9_cbu {
	int pitch_mode;
};

static void __iomem *chrome9_cbu_reg(struct chrome9_device *cdev,
				     unsigned int w, unsigned int field)
{
	return cdev->mmio + CHROME9_CBU_REG_BASE + w * CHROME9_CBU_STRIDE +
	       field * 4;
}

static void chrome9_cbu_read(struct chrome9_device *cdev, unsigned int w,
			     u32 *v)
{
	unsigned int i;

	for (i = 0; i < CHROME9_CBU_FIELDS; i++)
		v[i] = readl(chrome9_cbu_reg(cdev, w, i));
}

/* Control last; the readback posts the MMIO write before later BAR2
 * accesses, the barrier drains CPU stores issued under the old window. */
static void chrome9_cbu_write(struct chrome9_device *cdev, unsigned int w,
			      const u32 *v)
{
	unsigned int i;

	wmb();
	writel(0, chrome9_cbu_reg(cdev, w, CHROME9_CBU_CTL));
	for (i = CHROME9_CBU_BASE; i < CHROME9_CBU_FIELDS; i++)
		writel(v[i], chrome9_cbu_reg(cdev, w, i));
	writel(v[CHROME9_CBU_CTL], chrome9_cbu_reg(cdev, w, CHROME9_CBU_CTL));
	readl(chrome9_cbu_reg(cdev, w, CHROME9_CBU_CTL));
}

static void chrome9_cbu_disable(struct chrome9_device *cdev, unsigned int w)
{
	wmb();
	writel(0, chrome9_cbu_reg(cdev, w, CHROME9_CBU_CTL));
	readl(chrome9_cbu_reg(cdev, w, CHROME9_CBU_CTL));
}

static u32 chrome9_cbu_bpp(u32 cpp)
{
	return cpp == 4 ? CHROME9_CBU_BPP32 : cpp == 2 ? CHROME9_CBU_BPP16 : 0;
}

/* Proposed VX900 recipe (research note, section 4); width in pixels. */
static void chrome9_cbu_encode(u32 *v, u32 base, u32 pitch, u32 height,
			       u32 cpp, u32 width, int mode, u32 control)
{
	u32 p = mode == CHROME9_CBU_PITCH_TILES ? pitch >> 5 : pitch / cpp;

	v[CHROME9_CBU_CTL] = control;
	v[CHROME9_CBU_BASE] = base;
	v[CHROME9_CBU_END] = base + pitch * height;
	v[CHROME9_CBU_RECIP] = (u32)div_u64(BIT_ULL(32), p);
	v[CHROME9_CBU_PITCH] = p << 16 | p;
	v[CHROME9_CBU_SIZE] = height << 16 | width;
}

static u64 chrome9_cbu_tiled_offset(u32 xb, u32 y, u32 pitch, u32 tile_rows)
{
	return (u64)(y / tile_rows) * pitch * tile_rows +
	       (xb / 32) * 32 * tile_rows + (y % tile_rows) * 32 + xb % 32;
}

static void chrome9_cbu_untile(u64 o, u32 pitch, u32 tile_rows, u32 *xb,
			       u32 *y)
{
	u32 band = pitch * tile_rows;
	u32 r = do_div(o, band);

	*y = (u32)o * tile_rows + (r % (32 * tile_rows)) / 32;
	*xb = r / (32 * tile_rows) * 32 + r % 32;
}

/* Debug ladder, docs/research/chrome9-cbu-detile-re.md section 6. */

enum {
	CHROME9_CBU_STEP_DUMP = 1,	/* C0 */
	CHROME9_CBU_STEP_READBACK,	/* C1 */
	CHROME9_CBU_STEP_IDENTITY,	/* C2 */
	CHROME9_CBU_STEP_PITCH_PIXELS,	/* C3a */
	CHROME9_CBU_STEP_PITCH_TILES,	/* C3b */
	CHROME9_CBU_STEP_MATRIX,	/* C3c */
	CHROME9_CBU_STEP_WRITE,		/* C4 */
	CHROME9_CBU_STEP_BOUNDS,	/* C5 */
	CHROME9_CBU_STEP_BPP,		/* C6 */
	CHROME9_CBU_STEP_TWO,		/* C9 */
	CHROME9_CBU_STEP_TIMING,	/* C10 */
	CHROME9_CBU_STEP_TILE16,	/* C8 */
	CHROME9_CBU_STEP_ROUTE,		/* R */
	CHROME9_CBU_STEPS,
};

#define CHROME9_CBU_GUARD	0x13579bdfU
#define CHROME9_CBU_TAG_RAW	0xa5U
#define CHROME9_CBU_TAG_LIN	0x5aU

struct chrome9_cbu_test {
	struct chrome9_device *cdev;
	struct device *dev;
	unsigned int win;
	u32 snapshot[CHROME9_CBU_WINDOWS][CHROME9_CBU_FIELDS];
	/* Bits of each field that read back as written (C1). */
	u32 mask[CHROME9_CBU_FIELDS];
	unsigned long touched;
	/* Uncached map of the occlusion page and GART table. */
	void __iomem *tail;
	u64 gart_hash;
	int mode;
	const char *step;
};

struct chrome9_cbu_surface {
	struct drm_mm_node node;
	u32 base;
	u32 width;
	u32 height;
	u32 cpp;
	u32 pitch;
	u32 tile_rows;
	u32 extent;
	u32 *buf;
};

static const char * const chrome9_cbu_field_name[CHROME9_CBU_FIELDS] = {
	"ctl", "base", "end", "recip", "pitch", "hw",
};

static u32 chrome9_cbu_pattern(u32 tag, u64 offset)
{
	return tag << 24 | ((offset >> 2) & GENMASK(23, 0));
}

static const char *chrome9_cbu_mode_name(int mode)
{
	return mode == CHROME9_CBU_PITCH_TILES ? "columns" : "pixels";
}

/* Scratch surfaces live in the local pool, mapped write-combined only. */
static void __iomem *chrome9_cbu_sl(struct chrome9_cbu_test *t, u64 offset)
{
	return t->cdev->sl_wc + (offset - t->cdev->sl_pool_start);
}

static u64 chrome9_cbu_gart_hash(struct chrome9_cbu_test *t)
{
	void *copy = kvmalloc(CHROME9_ENGINE_VRAM_RESERVED, GFP_KERNEL);
	u64 hash;

	if (!copy)
		return 0;
	memcpy_fromio(copy, t->tail, CHROME9_ENGINE_VRAM_RESERVED);
	hash = xxh64(copy, CHROME9_ENGINE_VRAM_RESERVED, 0);
	kvfree(copy);
	return hash;
}

static int chrome9_cbu_surface_alloc(struct chrome9_cbu_test *t,
				     struct chrome9_cbu_surface *s, u32 width,
				     u32 height, u32 cpp, u32 tile_rows)
{
	struct chrome9_device *cdev = t->cdev;
	u32 size, o;
	int ret;

	memset(s, 0, sizeof(*s));
	s->width = width;
	s->height = height;
	s->cpp = cpp;
	s->tile_rows = tile_rows;
	s->pitch = max(ALIGN(width * cpp, 32), 64U);
	s->extent = s->pitch * ALIGN(height, tile_rows);
	size = PAGE_ALIGN(s->extent) + 2 * PAGE_SIZE;
	s->buf = vmalloc(s->extent);
	if (!s->buf)
		return -ENOMEM;
	mutex_lock(&cdev->sl_lock);
	ret = drm_mm_insert_node_generic(&cdev->sl_mm, &s->node, size,
					 PAGE_SIZE, 0, DRM_MM_INSERT_LOW);
	mutex_unlock(&cdev->sl_lock);
	if (ret) {
		vfree(s->buf);
		s->buf = NULL;
		return ret;
	}
	s->base = s->node.start + PAGE_SIZE;
	for (o = s->node.start; o < s->base; o += 4)
		writel(CHROME9_CBU_GUARD, chrome9_cbu_sl(t, o));
	for (o = s->base + s->extent; o < s->node.start + size; o += 4)
		writel(CHROME9_CBU_GUARD, chrome9_cbu_sl(t, o));
	wmb();
	return 0;
}

static unsigned int chrome9_cbu_guards_damaged(struct chrome9_cbu_test *t,
					       struct chrome9_cbu_surface *s)
{
	u64 end = s->node.start + s->node.size;
	unsigned int bad = 0;
	u64 o;

	for (o = s->node.start; o < s->base; o += 4)
		bad += readl(chrome9_cbu_sl(t, o)) != CHROME9_CBU_GUARD;
	for (o = s->base + s->extent; o < end; o += 4)
		bad += readl(chrome9_cbu_sl(t, o)) != CHROME9_CBU_GUARD;
	return bad;
}

static void chrome9_cbu_surface_free(struct chrome9_cbu_test *t,
				     struct chrome9_cbu_surface *s)
{
	if (!drm_mm_node_allocated(&s->node))
		return;
	mutex_lock(&t->cdev->sl_lock);
	drm_mm_remove_node(&s->node);
	mutex_unlock(&t->cdev->sl_lock);
	vfree(s->buf);
	s->buf = NULL;
}

/* Raw (window off) fill: every dword names its own offset. */
static void chrome9_cbu_fill_raw(struct chrome9_cbu_test *t,
				 struct chrome9_cbu_surface *s)
{
	u32 o;

	for (o = 0; o < s->extent; o += 4)
		s->buf[o / 4] = chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW, o);
	memcpy_toio(chrome9_cbu_sl(t, s->base), s->buf, s->extent);
	wmb();
}

static int chrome9_cbu_arm(struct chrome9_cbu_test *t, unsigned int w,
			   const struct chrome9_cbu_surface *s, u32 width,
			   int mode, u32 control)
{
	u32 regs[CHROME9_CBU_FIELDS], back[CHROME9_CBU_FIELDS];
	unsigned int i;

	chrome9_cbu_encode(regs, s->base, s->pitch, s->height, s->cpp, width,
			   mode, control);
	t->touched |= BIT(w);
	chrome9_cbu_write(t->cdev, w, regs);
	chrome9_cbu_read(t->cdev, w, back);
	for (i = 0; i < CHROME9_CBU_FIELDS; i++) {
		if (!((back[i] ^ regs[i]) & t->mask[i]))
			continue;
		chrome9_cbu_disable(t->cdev, w);
		dev_err(t->dev,
			"CBU %s: window %u %s wrote %08x read %08x (mask %08x), disabled\n",
			t->step, w, chrome9_cbu_field_name[i], regs[i], back[i],
			t->mask[i]);
		return -EIO;
	}
	return 0;
}

/*
 * Compare the linear view of rows < height and bytes < width_bytes with the
 * raw tiled pattern (tile_rows 0: with the raw bytes themselves).  Misses
 * count as untranslated (the raw dword at the linear offset) or other, and
 * the first ones are decoded.
 */
static u32 chrome9_cbu_check_view(struct chrome9_cbu_test *t,
				  struct chrome9_cbu_surface *s,
				  u32 width_bytes, u32 tile_rows,
				  u32 *untranslated)
{
	u32 linear = s->pitch * s->height;
	u32 bad = 0, xb, y, sx, sy;
	u64 o;

	*untranslated = 0;
	memcpy_fromio(s->buf, chrome9_cbu_sl(t, s->base), linear);
	for (y = 0; y < s->height; y++) {
		for (xb = 0; xb < width_bytes; xb += 4) {
			u32 got = s->buf[(y * s->pitch + xb) / 4];

			o = tile_rows ? chrome9_cbu_tiled_offset(xb, y, s->pitch,
								 tile_rows) :
					y * s->pitch + xb;
			if (got == chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW, o))
				continue;
			if (got == chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW,
						       y * s->pitch + xb))
				(*untranslated)++;
			if (++bad > 4)
				continue;
			if (got >> 24 == CHROME9_CBU_TAG_RAW && tile_rows) {
				chrome9_cbu_untile((got & GENMASK(23, 0)) * 4ULL,
						   s->pitch, tile_rows, &sx, &sy);
				dev_info(t->dev,
					 "CBU %s:   linear byte %u row %u reads raw +%#x (tiled byte %u row %u), expected raw +%#llx\n",
					 t->step, xb, y,
					 (u32)(got & GENMASK(23, 0)) * 4, sx, sy, o);
			} else {
				dev_info(t->dev,
					 "CBU %s:   linear byte %u row %u reads %08x, expected raw +%#llx\n",
					 t->step, xb, y, got, o);
			}
		}
	}
	return bad;
}

/* After linear writes of rows < height: raw dwords of those rows hold the
 * linear pattern, the tail rows keep the raw one. */
static u32 chrome9_cbu_check_raw(struct chrome9_cbu_test *t,
				 struct chrome9_cbu_surface *s,
				 u32 *untranslated)
{
	u32 bad = 0, o, xb, y, expect;

	*untranslated = 0;
	memcpy_fromio(s->buf, chrome9_cbu_sl(t, s->base), s->extent);
	for (o = 0; o < s->extent; o += 4) {
		chrome9_cbu_untile(o, s->pitch, s->tile_rows, &xb, &y);
		expect = y < s->height ?
			 chrome9_cbu_pattern(CHROME9_CBU_TAG_LIN,
					     y * s->pitch + xb) :
			 chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW, o);
		if (s->buf[o / 4] == expect)
			continue;
		/* The linear write landed at its own offset. */
		if (o < s->pitch * s->height &&
		    s->buf[o / 4] == chrome9_cbu_pattern(CHROME9_CBU_TAG_LIN, o))
			(*untranslated)++;
		if (++bad <= 4)
			dev_info(t->dev,
				 "CBU %s:   raw +%#x (byte %u row %u) holds %08x, expected %08x\n",
				 t->step, o, xb, y, s->buf[o / 4], expect);
	}
	return bad;
}

static u32 chrome9_cbu_first_read(struct chrome9_cbu_test *t,
				  struct chrome9_cbu_surface *s, u32 xb,
				  u32 y)
{
	u32 got = readl(chrome9_cbu_sl(t, s->base + y * s->pitch + xb));

	dev_info(t->dev,
		 "CBU %s: first read byte %u row %u = %08x (tiled %08x)\n",
		 t->step, xb, y, got,
		 chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW,
				     chrome9_cbu_tiled_offset(xb, y, s->pitch,
							      s->tile_rows)));
	return got;
}

/* Common end of a case: window off, guards and raw bytes checked. */
static int chrome9_cbu_finish(struct chrome9_cbu_test *t,
			      struct chrome9_cbu_surface *s, unsigned int w)
{
	unsigned int guards;

	chrome9_cbu_disable(t->cdev, w);
	guards = chrome9_cbu_guards_damaged(t, s);
	if (guards) {
		dev_err(t->dev, "CBU %s: %u guard dwords changed\n", t->step,
			guards);
		return -EIO;
	}
	return 0;
}

/* One read case: raw fill, arm, one dword, then the whole linear view. */
static int chrome9_cbu_read_case(struct chrome9_cbu_test *t, u32 width,
				 u32 height, u32 cpp, int mode)
{
	struct chrome9_cbu_surface s;
	u32 bad, untranslated, total;
	u64 ns;
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, width, height, cpp, 8);
	if (ret)
		return ret;
	chrome9_cbu_fill_raw(t, &s);
	ret = chrome9_cbu_arm(t, t->win, &s, s.pitch / cpp, mode,
			      CHROME9_CBU_ENABLE | chrome9_cbu_bpp(cpp) |
			      CHROME9_CBU_TILE);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s, 0, 0);
	chrome9_cbu_first_read(t, &s, s.pitch - 4, height - 1);
	ns = ktime_get_ns();
	bad = chrome9_cbu_check_view(t, &s, s.pitch, 8, &untranslated);
	ns = ktime_get_ns() - ns;
	ret = chrome9_cbu_finish(t, &s, t->win);
	total = s.pitch / 4 * height;
	dev_info(t->dev,
		 "CBU %s: %ux%u cpp %u pitch %u (%s): %u/%u dwords match, untranslated %u, other %u, %llu us\n",
		 t->step, width, height, cpp, s.pitch,
		 chrome9_cbu_mode_name(mode), total - bad, total, untranslated,
		 bad - untranslated, div_u64(ns, 1000));
	if (!ret && bad)
		ret = 1;
out:
	chrome9_cbu_disable(t->cdev, t->win);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

static int chrome9_cbu_step_dump(struct chrome9_cbu_test *t)
{
	struct pci_dev *pdev = t->cdev->pdev;
	u8 d0f0_b0 = 0, d0f2_9e = 0, b0 = 0, b2 = 0, b9 = 0, sr42 = 0, index;
	int domain = pci_domain_nr(pdev->bus);
	struct pci_dev *nb;
	unsigned int w, enabled = 0;
	u16 d0f3_a0 = 0;
	bool dfa, routed;

	for (w = 0; w < CHROME9_CBU_WINDOWS; w++) {
		u32 *v = t->snapshot[w];

		chrome9_cbu_read(t->cdev, w, v);
		enabled += v[CHROME9_CBU_CTL] & CHROME9_CBU_ENABLE;
		dev_info(t->dev,
			 "CBU C0: window %2u ctl %08x base %08x end %08x recip %08x pitch %08x hw %08x\n",
			 w, v[0], v[1], v[2], v[3], v[4], v[5]);
	}
	nb = pci_get_domain_bus_and_slot(domain, 0, PCI_DEVFN(0, 0));
	if (nb)
		pci_read_config_byte(nb, 0xb0, &d0f0_b0);
	pci_dev_put(nb);
	nb = pci_get_domain_bus_and_slot(domain, 0, PCI_DEVFN(0, 2));
	if (nb)
		pci_read_config_byte(nb, 0x9e, &d0f2_9e);
	pci_dev_put(nb);
	nb = pci_get_domain_bus_and_slot(domain, 0, PCI_DEVFN(0, 3));
	if (nb)
		pci_read_config_word(nb, 0xa0, &d0f3_a0);
	pci_dev_put(nb);
	pci_read_config_byte(pdev, 0xb0, &b0);
	pci_read_config_byte(pdev, 0xb2, &b2);
	pci_read_config_byte(pdev, 0xb9, &b9);
	if (!chrome9_vga_seq_get(t->cdev)) {
		index = vga_io_r(VGA_SEQ_I);
		sr42 = vga_io_rseq(0x42);
		vga_io_w(VGA_SEQ_I, index);
		chrome9_vga_seq_put(t->cdev);
	}
	/* SPM: SL cycles reach GFXCTL (D0F0 B0[0]) unless direct frame
	 * buffer access (D1F0 B9[1]) sends them to DRAM; then only
	 * D0F2 9E[1] routes tile-window addresses back to GFXCTL. */
	routed = d0f0_b0 & BIT(0);
	dfa = b9 & BIT(1);
	dev_info(t->dev,
		 "CBU C0: D0F0.B0=%02x D0F2.9E=%02x D0F3.A0=%04x D1F0.B0=%02x B2=%02x B9=%02x SR42=%02x: SL via GFXCTL %d, DFA %d, NB tile windows %d; %u windows enabled\n",
		 d0f0_b0, d0f2_9e, d0f3_a0, b0, b2, b9, sr42, routed, dfa,
		 !!(d0f2_9e & BIT(1)), enabled);
	if (enabled) {
		dev_err(t->dev, "CBU C0: a window is already enabled, stop\n");
		return 1;
	}
	if (!routed || (dfa && !(d0f2_9e & BIT(1)))) {
		dev_err(t->dev,
			"CBU C0: CPU SL accesses bypass GFXCTL, no window can translate them, stop\n");
		return 1;
	}
	return 0;
}

static int chrome9_cbu_step_readback(struct chrome9_cbu_test *t)
{
	static const u32 pattern[CHROME9_CBU_FIELDS] = {
		0, 0x1234567f, 0x2345678f, 0x89abcdef, 0x01230456, 0x07890abc,
	};
	u32 back[2][CHROME9_CBU_FIELDS];
	unsigned int i, pass;
	u32 ctl;

	t->touched |= BIT(t->win);
	for (pass = 0; pass < 2; pass++) {
		for (i = CHROME9_CBU_BASE; i < CHROME9_CBU_FIELDS; i++)
			writel(pass ? ~pattern[i] : pattern[i],
			       chrome9_cbu_reg(t->cdev, t->win, i));
		chrome9_cbu_read(t->cdev, t->win, back[pass]);
	}
	for (i = CHROME9_CBU_BASE; i < CHROME9_CBU_FIELDS; i++) {
		writel(0, chrome9_cbu_reg(t->cdev, t->win, i));
		t->mask[i] = ~(back[0][i] ^ pattern[i]) &
			     ~(back[1][i] ^ ~pattern[i]);
	}
	/* Every control bit but enable. */
	writel(0xfe, chrome9_cbu_reg(t->cdev, t->win, CHROME9_CBU_CTL));
	ctl = readl(chrome9_cbu_reg(t->cdev, t->win, CHROME9_CBU_CTL));
	chrome9_cbu_disable(t->cdev, t->win);
	t->mask[CHROME9_CBU_CTL] = ~(ctl ^ 0xfe) & 0xfe;
	dev_info(t->dev,
		 "CBU C1: window %u readback masks ctl %08x (read %08x) base %08x end %08x recip %08x pitch %08x hw %08x\n",
		 t->win, t->mask[0], ctl, t->mask[1], t->mask[2], t->mask[3],
		 t->mask[4], t->mask[5]);
	dev_info(t->dev,
		 "CBU C1: read after %08x/%08x/%08x/%08x/%08x: %08x/%08x/%08x/%08x/%08x\n",
		 pattern[1], pattern[2], pattern[3], pattern[4], pattern[5],
		 back[0][1], back[0][2], back[0][3], back[0][4], back[0][5]);
	return readl(chrome9_cbu_reg(t->cdev, t->win, CHROME9_CBU_CTL)) ? 1 : 0;
}

/* Rotate function, type 0, no tile: expected to read the raw bytes. */
static int chrome9_cbu_step_identity(struct chrome9_cbu_test *t)
{
	struct chrome9_cbu_surface s;
	u32 bad, untranslated, total;
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, 64, 64, 4, 8);
	if (ret)
		return ret;
	chrome9_cbu_fill_raw(t, &s);
	ret = chrome9_cbu_arm(t, t->win, &s, 64, CHROME9_CBU_PITCH_PIXELS,
			      CHROME9_CBU_ENABLE | CHROME9_CBU_BPP32);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s, 0, 0);
	bad = chrome9_cbu_check_view(t, &s, s.pitch, 0, &untranslated);
	ret = chrome9_cbu_finish(t, &s, t->win);
	total = s.pitch / 4 * s.height;
	dev_info(t->dev,
		 "CBU C2: 64x64 linear, control 05: %u/%u dwords unchanged\n",
		 total - bad, total);
	if (!ret && bad)
		ret = 1;
out:
	chrome9_cbu_disable(t->cdev, t->win);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

static int chrome9_cbu_step_pitch(struct chrome9_cbu_test *t, int mode)
{
	int ret = chrome9_cbu_read_case(t, 256, 64, 4, mode);

	if (!ret)
		t->mode = mode;
	return ret;
}

static int chrome9_cbu_step_matrix(struct chrome9_cbu_test *t)
{
	static const struct {
		u32 width, height, cpp;
	} cases[] = {
		{ 800, 600, 4 },
		{ 1600, 1200, 4 },
		{ 256, 128, 2 },
		{ 800, 600, 2 },
		/* Not tile aligned: pitch 416 (104 px), 61 rows. */
		{ 100, 61, 4 },
		/* Pitch 128 (64 px), 33 rows. */
		{ 50, 33, 2 },
	};
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(cases) && !ret; i++)
		ret = chrome9_cbu_read_case(t, cases[i].width, cases[i].height,
					    cases[i].cpp, t->mode);
	return ret;
}

enum {
	CHROME9_CBU_WRITE_DWORD,
	CHROME9_CBU_WRITE_BYTE_WORD,
	CHROME9_CBU_WRITE_QWORD,
	CHROME9_CBU_WRITE_WC,
};

static const char * const chrome9_cbu_write_name[] = {
	"dword", "byte/word", "qword", "memcpy WC",
};

/* The pool is write-combined: a fence after every store keeps each one a
 * transaction of its own size; memcpy lets the CPU combine them. */
static void chrome9_cbu_store(void __iomem *dst, const u32 *buf, u32 bytes,
			      int kind)
{
	u32 l, v;

	switch (kind) {
	case CHROME9_CBU_WRITE_DWORD:
		for (l = 0; l < bytes; l += 4) {
			writel(buf[l / 4], dst + l);
			wmb();
		}
		break;
	case CHROME9_CBU_WRITE_BYTE_WORD:
		for (l = 0; l < bytes; l += 4) {
			v = buf[l / 4];
			if (l & 4) {
				writew(v, dst + l);
				wmb();
				writew(v >> 16, dst + l + 2);
			} else {
				writeb(v, dst + l);
				wmb();
				writeb(v >> 8, dst + l + 1);
				wmb();
				writeb(v >> 16, dst + l + 2);
				wmb();
				writeb(v >> 24, dst + l + 3);
			}
			wmb();
		}
		break;
	case CHROME9_CBU_WRITE_QWORD:
#ifdef CONFIG_64BIT
		for (l = 0; l < bytes; l += 8) {
			writeq((u64)buf[l / 4 + 1] << 32 | buf[l / 4], dst + l);
			wmb();
		}
#endif
		break;
	case CHROME9_CBU_WRITE_WC:
		memcpy_toio(dst, buf, bytes);
		wmb();
		break;
	}
}

static int chrome9_cbu_write_case(struct chrome9_cbu_test *t, u32 width,
				  u32 height, u32 cpp, int kind)
{
	struct chrome9_device *cdev = t->cdev;
	struct chrome9_cbu_surface s;
	u32 linear, l, bad, untranslated;
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, width, height, cpp, 8);
	if (ret)
		return ret;
	linear = s.pitch * height;
	chrome9_cbu_fill_raw(t, &s);
	ret = chrome9_cbu_arm(t, t->win, &s, s.pitch / cpp, t->mode,
			      CHROME9_CBU_ENABLE | chrome9_cbu_bpp(cpp) |
			      CHROME9_CBU_TILE);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s, 0, 0);
	for (l = 0; l < linear; l += 4)
		s.buf[l / 4] = chrome9_cbu_pattern(CHROME9_CBU_TAG_LIN, l);
	chrome9_cbu_store(chrome9_cbu_sl(t, s.base), s.buf, linear, kind);
	ret = chrome9_cbu_finish(t, &s, t->win);
	bad = chrome9_cbu_check_raw(t, &s, &untranslated);
	dev_info(t->dev,
		 "CBU %s: %s %ux%u cpp %u pitch %u: %u/%u raw dwords as expected, untranslated %u\n",
		 t->step, chrome9_cbu_write_name[kind], width, height, cpp,
		 s.pitch, s.extent / 4 - bad, s.extent / 4, untranslated);
	if (!ret && bad)
		ret = 1;
out:
	chrome9_cbu_disable(cdev, t->win);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

static int chrome9_cbu_step_write(struct chrome9_cbu_test *t)
{
	int ret, kind;

	for (kind = CHROME9_CBU_WRITE_DWORD; kind <= CHROME9_CBU_WRITE_WC;
	     kind++) {
		if (!IS_ENABLED(CONFIG_64BIT) && kind == CHROME9_CBU_WRITE_QWORD)
			continue;
		ret = chrome9_cbu_write_case(t, 256, 64, 4, kind);
		if (ret)
			return ret;
	}
	ret = chrome9_cbu_write_case(t, 100, 61, 4, CHROME9_CBU_WRITE_DWORD);
	if (!ret)
		ret = chrome9_cbu_write_case(t, 256, 128, 2,
					     CHROME9_CBU_WRITE_BYTE_WORD);
	return ret;
}

/* Where did the dword at SL offset @at come from while the window is on? */
static const char *chrome9_cbu_classify(struct chrome9_cbu_test *t, u32 at,
					u32 expect_raw, u32 expect_tiled)
{
	u32 got = readl(chrome9_cbu_sl(t, at));

	if (got == expect_raw)
		return "untranslated";
	if (got == expect_tiled)
		return "translated";
	return "other";
}

static int chrome9_cbu_step_bounds(struct chrome9_cbu_test *t)
{
	struct chrome9_device *cdev = t->cdev;
	struct chrome9_cbu_surface s;
	const char *before, *after;
	u32 linear, xb, y, got, x_in = 0, x_tiled = 0, x_raw = 0, x_other = 0;
	u32 tiled_end, end_raw;
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, 100, 61, 4, 8);
	if (ret)
		return ret;
	linear = s.pitch * s.height;
	chrome9_cbu_fill_raw(t, &s);
	ret = chrome9_cbu_arm(t, t->win, &s, s.pitch / 4, t->mode,
			      CHROME9_CBU_ENABLE | CHROME9_CBU_BPP32 |
			      CHROME9_CBU_TILE);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s, 0, 0);
	before = chrome9_cbu_classify(t, s.base - 16, CHROME9_CBU_GUARD, 0);
	tiled_end = chrome9_cbu_tiled_offset(0, s.height, s.pitch, 8);
	end_raw = chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW, linear);
	after = chrome9_cbu_classify(t, s.base + linear, end_raw,
				     chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW,
							 tiled_end));
	/* Stores just outside [base, end) must land untranslated. */
	writel(0x0badf00d, chrome9_cbu_sl(t, s.base - 16));
	wmb();
	writel(0x0badf00d, chrome9_cbu_sl(t, s.base + linear));
	chrome9_cbu_disable(cdev, t->win);
	got = readl(chrome9_cbu_sl(t, s.base - 16));
	writel(CHROME9_CBU_GUARD, chrome9_cbu_sl(t, s.base - 16));
	if (got != 0x0badf00d)
		before = "store lost";
	got = readl(chrome9_cbu_sl(t, s.base + linear));
	writel(end_raw, chrome9_cbu_sl(t, s.base + linear));
	wmb();
	if (got != 0x0badf00d)
		after = "store lost";
	/* Window width 100 of the 104-pixel pitch. */
	ret = chrome9_cbu_arm(t, t->win, &s, s.width, t->mode,
			      CHROME9_CBU_ENABLE | CHROME9_CBU_BPP32 |
			      CHROME9_CBU_TILE);
	if (ret)
		goto out;
	memcpy_fromio(s.buf, chrome9_cbu_sl(t, s.base), linear);
	for (y = 0; y < s.height; y++) {
		for (xb = 0; xb < s.pitch; xb += 4) {
			got = s.buf[(y * s.pitch + xb) / 4];
			if (got == chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW,
					chrome9_cbu_tiled_offset(xb, y,
								 s.pitch, 8))) {
				if (xb < s.width * 4)
					x_in++;
				else
					x_tiled++;
			} else if (xb >= s.width * 4 &&
				   got == chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW,
							      y * s.pitch + xb)) {
				x_raw++;
			} else if (xb >= s.width * 4) {
				x_other++;
			}
		}
	}
	ret = chrome9_cbu_finish(t, &s, t->win);
	dev_info(t->dev,
		 "CBU C5: base-16 %s, end %s; width 100 of 104: %u/%u inside match, beyond width tiled %u untranslated %u other %u\n",
		 before, after, x_in, s.width * s.height, x_tiled, x_raw,
		 x_other);
	if (!ret && (strcmp(before, "untranslated") ||
		     !strcmp(after, "other") || !strcmp(after, "store lost") ||
		     x_in != s.width * s.height))
		ret = 1;
out:
	chrome9_cbu_disable(cdev, t->win);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

/* Byte model: the same bytes through 16- and 8-bpp programming. */
static int chrome9_cbu_step_bpp(struct chrome9_cbu_test *t)
{
	static const u32 cpps[] = { 2, 1 };
	struct chrome9_cbu_surface s;
	u32 bad, untranslated, total;
	unsigned int i;
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, 256, 64, 4, 8);
	if (ret)
		return ret;
	chrome9_cbu_fill_raw(t, &s);
	total = s.pitch / 4 * s.height;
	for (i = 0; i < ARRAY_SIZE(cpps) && !ret; i++) {
		/* The 32-bpp surface described as cpps[i] pixels. */
		s.cpp = cpps[i];
		ret = chrome9_cbu_arm(t, t->win, &s, s.pitch / s.cpp, t->mode,
				      CHROME9_CBU_ENABLE |
				      chrome9_cbu_bpp(s.cpp) |
				      CHROME9_CBU_TILE);
		if (ret)
			break;
		chrome9_cbu_first_read(t, &s, 0, 0);
		bad = chrome9_cbu_check_view(t, &s, s.pitch, 8, &untranslated);
		ret = chrome9_cbu_finish(t, &s, t->win);
		dev_info(t->dev,
			 "CBU C6: pitch 1024 as %u bpp: %u/%u dwords match, untranslated %u\n",
			 s.cpp * 8, total - bad, total, untranslated);
		if (!ret && bad)
			ret = 1;
	}
	chrome9_cbu_disable(t->cdev, t->win);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

static int chrome9_cbu_arm_tiled(struct chrome9_cbu_test *t, unsigned int w,
				 struct chrome9_cbu_surface *s)
{
	return chrome9_cbu_arm(t, w, s, s->pitch / s->cpp, t->mode,
			       CHROME9_CBU_ENABLE | chrome9_cbu_bpp(s->cpp) |
			       CHROME9_CBU_TILE);
}

/* Two windows at once; then one moves while the other stays. */
static int chrome9_cbu_step_two(struct chrome9_cbu_test *t)
{
	const unsigned int w2 = t->win - 1;
	struct chrome9_cbu_surface s[3];
	u32 bad[4], untranslated, released = 0, i;
	int ret;

	memset(s, 0, sizeof(s));
	ret = chrome9_cbu_surface_alloc(t, &s[0], 256, 64, 4, 8);
	if (!ret)
		ret = chrome9_cbu_surface_alloc(t, &s[1], 128, 32, 2, 8);
	if (!ret)
		ret = chrome9_cbu_surface_alloc(t, &s[2], 64, 16, 4, 8);
	if (ret)
		goto out;
	for (i = 0; i < 3; i++)
		chrome9_cbu_fill_raw(t, &s[i]);
	ret = chrome9_cbu_arm_tiled(t, w2, &s[0]);
	if (!ret)
		ret = chrome9_cbu_arm_tiled(t, t->win, &s[1]);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s[0], 0, 0);
	chrome9_cbu_first_read(t, &s[1], 0, 0);
	bad[0] = chrome9_cbu_check_view(t, &s[0], s[0].pitch, 8, &untranslated);
	bad[1] = chrome9_cbu_check_view(t, &s[1], s[1].pitch, 8, &untranslated);
	ret = chrome9_cbu_arm_tiled(t, t->win, &s[2]);
	if (ret)
		goto out;
	bad[2] = chrome9_cbu_check_view(t, &s[2], s[2].pitch, 8, &untranslated);
	bad[3] = chrome9_cbu_check_view(t, &s[0], s[0].pitch, 8, &untranslated);
	/* The released range reads raw again. */
	memcpy_fromio(s[1].buf, chrome9_cbu_sl(t, s[1].base), s[1].extent);
	for (i = 0; i < s[1].extent; i += 4)
		if (s[1].buf[i / 4] !=
		    chrome9_cbu_pattern(CHROME9_CBU_TAG_RAW, i))
			released++;
	chrome9_cbu_disable(t->cdev, w2);
	ret = chrome9_cbu_finish(t, &s[0], t->win);
	if (!ret)
		ret = chrome9_cbu_finish(t, &s[1], t->win);
	if (!ret)
		ret = chrome9_cbu_finish(t, &s[2], t->win);
	dev_info(t->dev,
		 "CBU C9: windows %u+%u mismatches %u/%u, after moving %u: %u (new) %u (kept), released range %u raw mismatches\n",
		 w2, t->win, bad[0], bad[1], t->win, bad[2], bad[3],
		 released);
	if (!ret && (bad[0] || bad[1] || bad[2] || bad[3] || released))
		ret = 1;
out:
	chrome9_cbu_disable(t->cdev, w2);
	chrome9_cbu_disable(t->cdev, t->win);
	for (i = 0; i < 3; i++)
		chrome9_cbu_surface_free(t, &s[i]);
	return ret;
}

static u32 chrome9_cbu_mbps(u32 bytes, u64 ns)
{
	return ns ? div64_u64((u64)bytes * 1000, ns) : 0;
}

/* Window versus raw access plus software (de)tiling, the Mesa path. */
static int chrome9_cbu_step_timing(struct chrome9_cbu_test *t)
{
	struct chrome9_device *cdev = t->cdev;
	struct chrome9_cbu_surface s;
	u32 linear, xb, y, bad, untranslated;
	void __iomem *sl;
	u32 *lin = NULL;
	u64 ns[4];
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, 1600, 1200, 4, 8);
	if (ret)
		return ret;
	linear = s.pitch * s.height;
	sl = chrome9_cbu_sl(t, s.base);
	lin = vmalloc(linear);
	if (!lin) {
		ret = -ENOMEM;
		goto out;
	}
	chrome9_cbu_fill_raw(t, &s);
	/* Raw read and CPU detile in 32-byte runs. */
	ns[0] = ktime_get_ns();
	memcpy_fromio(s.buf, sl, s.extent);
	for (y = 0; y < s.height; y++)
		for (xb = 0; xb < s.pitch; xb += 32)
			memcpy((u8 *)lin + y * s.pitch + xb, (u8 *)s.buf +
			       chrome9_cbu_tiled_offset(xb, y, s.pitch, 8), 32);
	ns[0] = ktime_get_ns() - ns[0];
	ret = chrome9_cbu_arm_tiled(t, t->win, &s);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s, 0, 0);
	ns[1] = ktime_get_ns();
	memcpy_fromio(lin, sl, linear);
	ns[1] = ktime_get_ns() - ns[1];
	for (y = 0; y < linear; y += 4)
		lin[y / 4] = chrome9_cbu_pattern(CHROME9_CBU_TAG_LIN, y);
	ns[2] = ktime_get_ns();
	memcpy_toio(sl, lin, linear);
	wmb();
	ns[2] = ktime_get_ns() - ns[2];
	ret = chrome9_cbu_finish(t, &s, t->win);
	bad = chrome9_cbu_check_raw(t, &s, &untranslated);
	/* CPU tiling and a raw write. */
	ns[3] = ktime_get_ns();
	for (y = 0; y < s.height; y++)
		for (xb = 0; xb < s.pitch; xb += 32)
			memcpy((u8 *)s.buf +
			       chrome9_cbu_tiled_offset(xb, y, s.pitch, 8),
			       (u8 *)lin + y * s.pitch + xb, 32);
	memcpy_toio(sl, s.buf, s.extent);
	wmb();
	ns[3] = ktime_get_ns() - ns[3];
	dev_info(t->dev,
		 "CBU C10: 1600x1200 MB/s: read raw+detile %u, window %u; write window %u, tile+raw %u; window writes %u mismatches\n",
		 chrome9_cbu_mbps(linear, ns[0]),
		 chrome9_cbu_mbps(linear, ns[1]),
		 chrome9_cbu_mbps(linear, ns[2]),
		 chrome9_cbu_mbps(linear, ns[3]), bad);
	if (!ret && bad)
		ret = 1;
out:
	chrome9_cbu_disable(cdev, t->win);
	vfree(lin);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

/* Control bit 7: records the mapping against the inferred 32x16 tile. */
static int chrome9_cbu_step_tile16(struct chrome9_cbu_test *t)
{
	struct chrome9_cbu_surface s;
	u32 bad, untranslated, bad8, total;
	int ret;

	ret = chrome9_cbu_surface_alloc(t, &s, 256, 64, 4, 16);
	if (ret)
		return ret;
	chrome9_cbu_fill_raw(t, &s);
	ret = chrome9_cbu_arm(t, t->win, &s, s.pitch / 4, t->mode,
			      CHROME9_CBU_ENABLE | CHROME9_CBU_BPP32 |
			      CHROME9_CBU_TILE | CHROME9_CBU_TILE16);
	if (ret)
		goto out;
	chrome9_cbu_first_read(t, &s, 0, 0);
	bad = chrome9_cbu_check_view(t, &s, s.pitch, 16, &untranslated);
	bad8 = chrome9_cbu_check_view(t, &s, s.pitch, 8, &untranslated);
	ret = chrome9_cbu_finish(t, &s, t->win);
	total = s.pitch / 4 * s.height;
	dev_info(t->dev,
		 "CBU C8: control a5 256x64: %u/%u match 32x16 tiles, %u match 32x8, untranslated %u\n",
		 total - bad, total, total - bad8, untranslated);
	if (!ret && bad)
		ret = 1;
out:
	chrome9_cbu_disable(t->cdev, t->win);
	chrome9_cbu_surface_free(t, &s);
	return ret;
}

/*
 * Routing probe (after C3 read the raw bytes through an enabled window).
 * SPM D0F2 Rx9E[1]: under direct frame buffer access (DFA, D1F0 RxB9[1])
 * the NB sends CPU cycles in 16 "tile mode" windows to GFXCTL and all
 * others straight to DRAM; the vendor X driver gates CBU rotation on a
 * "direct access window" chip capability.  Each bit changes one routing
 * register for the probe and restores it afterwards.
 */
#define CHROME9_CBU_ROUTE_PROBE	BIT(0)
#define CHROME9_CBU_ROUTE_NB	BIT(1)
#define CHROME9_CBU_ROUTE_DFA	BIT(2)
#define CHROME9_CBU_ROUTE_SR42	BIT(3)

struct chrome9_cbu_route {
	struct pci_dev *d0f2;
	u8 d0f2_9e;
	u8 b9;
	u8 sr42;
	unsigned int applied;
};

static int chrome9_cbu_sr42(struct chrome9_cbu_test *t, int value)
{
	u8 index;
	int ret;

	ret = chrome9_vga_seq_get(t->cdev);
	if (ret)
		return ret;
	index = vga_io_r(VGA_SEQ_I);
	if (value >= 0)
		vga_io_wseq(0x42, value);
	ret = vga_io_rseq(0x42);
	vga_io_w(VGA_SEQ_I, index);
	chrome9_vga_seq_put(t->cdev);
	return ret;
}

static void chrome9_cbu_route_restore(struct chrome9_cbu_test *t,
				      struct chrome9_cbu_route *r)
{
	struct pci_dev *pdev = t->cdev->pdev;
	u8 b9 = 0, nb = 0;
	int sr42 = -1;

	if (r->applied & CHROME9_CBU_ROUTE_SR42)
		sr42 = chrome9_cbu_sr42(t, r->sr42);
	if (r->applied & CHROME9_CBU_ROUTE_DFA)
		pci_write_config_byte(pdev, 0xb9, r->b9);
	if (r->applied & CHROME9_CBU_ROUTE_NB)
		pci_write_config_byte(r->d0f2, 0x9e, r->d0f2_9e);
	pci_read_config_byte(pdev, 0xb9, &b9);
	pci_read_config_byte(r->d0f2, 0x9e, &nb);
	dev_info(t->dev, "CBU R: restored D0F2.9E=%02x D1F0.B9=%02x SR42=%02x\n",
		 nb, b9, sr42 < 0 ? chrome9_cbu_sr42(t, -1) : sr42);
	r->applied = 0;
}

/* DFA translates [base, base + size) to the frame buffer: only enable it
 * when that range is exactly BAR2. */
static bool chrome9_cbu_dfa_matches(struct chrome9_cbu_test *t)
{
	struct pci_dev *pdev = t->cdev->pdev;
	struct pci_dev *d0f3;
	u16 a0 = 0;
	u64 base, size;

	d0f3 = pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus), 0,
					   PCI_DEVFN(0, 3));
	if (d0f3)
		pci_read_config_word(d0f3, 0xa0, &a0);
	pci_dev_put(d0f3);
	base = (u64)((a0 >> 3) & 0x1ff) << 23 | (u64)((a0 >> 2) & 1) << 32;
	size = SZ_4M << ((a0 >> 12) & 7);
	dev_info(t->dev, "CBU R: D0F3.A0=%04x DFA base %#llx size %#llx, BAR2 %#llx size %#llx\n",
		 a0, base, size, (u64)pci_resource_start(pdev, 2),
		 (u64)pci_resource_len(pdev, 2));
	return (a0 & BIT(15)) && !(a0 & BIT(1)) && (a0 >> 12) & 7 &&
	       base == pci_resource_start(pdev, 2) &&
	       size == pci_resource_len(pdev, 2);
}

static int chrome9_cbu_route_apply(struct chrome9_cbu_test *t,
				   struct chrome9_cbu_route *r)
{
	struct pci_dev *pdev = t->cdev->pdev;
	u8 v = 0;
	int sr42;

	if (chrome9_cbu_route & CHROME9_CBU_ROUTE_NB) {
		/* SPM: bit 5 must be 0 while bit 1 is set. */
		if (r->d0f2_9e & BIT(5))
			return -EINVAL;
		r->applied |= CHROME9_CBU_ROUTE_NB;
		pci_write_config_byte(r->d0f2, 0x9e, r->d0f2_9e | BIT(1));
		pci_read_config_byte(r->d0f2, 0x9e, &v);
		dev_info(t->dev, "CBU R: D0F2.9E %02x -> %02x\n", r->d0f2_9e, v);
	}
	if (chrome9_cbu_route & CHROME9_CBU_ROUTE_DFA) {
		if (!(chrome9_cbu_route & CHROME9_CBU_ROUTE_NB) ||
		    !chrome9_cbu_dfa_matches(t))
			return -EINVAL;
		r->applied |= CHROME9_CBU_ROUTE_DFA;
		pci_write_config_byte(pdev, 0xb9, r->b9 | BIT(1));
		pci_read_config_byte(pdev, 0xb9, &v);
		dev_info(t->dev, "CBU R: D1F0.B9 %02x -> %02x\n", r->b9, v);
	}
	if (chrome9_cbu_route & CHROME9_CBU_ROUTE_SR42) {
		r->applied |= CHROME9_CBU_ROUTE_SR42;
		sr42 = chrome9_cbu_sr42(t, r->sr42 | 0x80);
		dev_info(t->dev, "CBU R: SR42 %02x -> %02x\n", r->sr42, sr42);
	}
	return 0;
}

/* Where each linear dword of the view came from, for any mapping. */
static void chrome9_cbu_route_view(struct chrome9_cbu_test *t,
				   struct chrome9_cbu_surface *s,
				   const char *what, u32 control)
{
	u32 linear = s->pitch * s->height, l, o, got;
	u32 raw = 0, tiled = 0, rot180 = 0, other = 0, ctl;

	ctl = readl(chrome9_cbu_reg(t->cdev, t->win, CHROME9_CBU_CTL));
	memcpy_fromio(s->buf, chrome9_cbu_sl(t, s->base), linear);
	for (l = 0; l < linear; l += 4) {
		u32 x = l % s->pitch, y = l / s->pitch;

		got = s->buf[l / 4];
		o = (got & GENMASK(23, 0)) * 4;
		if (got >> 24 != CHROME9_CBU_TAG_RAW)
			other++;
		else if (o == l)
			raw++;
		else if (o == chrome9_cbu_tiled_offset(x, y, s->pitch, 8))
			tiled++;
		else if (o == (s->height - 1 - y) * s->pitch + s->pitch - 4 - x)
			rot180++;
		else if (other++ < 3)
			dev_info(t->dev,
				 "CBU R:   %s linear byte %u row %u reads raw byte %u row %u\n",
				 what, x, y, o % s->pitch, o / s->pitch);
	}
	dev_info(t->dev,
		 "CBU R: %s ctl wrote %02x read %08x: %u dwords raw, %u tiled, %u rotated 180, %u other\n",
		 what, control, ctl, raw, tiled, rot180, other);
}

static int chrome9_cbu_route_probe(struct chrome9_cbu_test *t,
				   struct chrome9_cbu_surface *s)
{
	const u32 tile = CHROME9_CBU_ENABLE | CHROME9_CBU_BPP32 |
			 CHROME9_CBU_TILE;
	const u32 rot180 = CHROME9_CBU_ENABLE | CHROME9_CBU_BPP32 | 2 << 3;
	u32 linear = s->pitch * s->height, l, bad, untranslated;
	int ret, mode;

	for (mode = CHROME9_CBU_PITCH_PIXELS; mode <= CHROME9_CBU_PITCH_TILES;
	     mode++) {
		ret = chrome9_cbu_arm(t, t->win, s, s->pitch / 4, mode, tile);
		if (ret)
			return ret;
		chrome9_cbu_first_read(t, s, 0, 0);
		chrome9_cbu_route_view(t, s, mode ? "tile read columns" :
				       "tile read pixels", tile);
		chrome9_cbu_disable(t->cdev, t->win);
	}
	ret = chrome9_cbu_arm(t, t->win, s, s->pitch / 4,
			      CHROME9_CBU_PITCH_PIXELS, rot180);
	if (ret)
		return ret;
	chrome9_cbu_first_read(t, s, 0, 0);
	chrome9_cbu_route_view(t, s, "rotate 180 read", rot180);
	chrome9_cbu_disable(t->cdev, t->win);
	/* Stores may be translated even where loads are not. */
	ret = chrome9_cbu_arm(t, t->win, s, s->pitch / 4,
			      CHROME9_CBU_PITCH_PIXELS, tile);
	if (ret)
		return ret;
	for (l = 0; l < linear; l += 4)
		s->buf[l / 4] = chrome9_cbu_pattern(CHROME9_CBU_TAG_LIN, l);
	chrome9_cbu_store(chrome9_cbu_sl(t, s->base), s->buf, linear,
			  CHROME9_CBU_WRITE_DWORD);
	chrome9_cbu_disable(t->cdev, t->win);
	bad = chrome9_cbu_check_raw(t, s, &untranslated);
	dev_info(t->dev,
		 "CBU R: tile write pixels: %u/%u raw dwords tiled as expected, untranslated %u\n",
		 s->extent / 4 - bad, s->extent / 4, untranslated);
	chrome9_cbu_fill_raw(t, s);
	return 0;
}

static int chrome9_cbu_step_route(struct chrome9_cbu_test *t)
{
	struct pci_dev *pdev = t->cdev->pdev;
	struct chrome9_cbu_route r = { };
	struct chrome9_cbu_surface s;
	u32 bad, untranslated;
	int ret, sr42;

	r.d0f2 = pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus), 0,
					     PCI_DEVFN(0, 2));
	if (!r.d0f2)
		return -ENODEV;
	pci_read_config_byte(r.d0f2, 0x9e, &r.d0f2_9e);
	pci_read_config_byte(pdev, 0xb9, &r.b9);
	sr42 = chrome9_cbu_sr42(t, -1);
	ret = sr42 < 0 ? sr42 :
	      chrome9_cbu_surface_alloc(t, &s, 256, 64, 4, 8);
	if (ret) {
		pci_dev_put(r.d0f2);
		return ret;
	}
	r.sr42 = sr42;
	/* Filled before any routing change, read back after it. */
	chrome9_cbu_fill_raw(t, &s);
	ret = chrome9_cbu_route_apply(t, &r);
	if (ret) {
		dev_err(t->dev, "CBU R: route %#x not applicable\n",
			chrome9_cbu_route);
		goto out;
	}
	bad = chrome9_cbu_check_view(t, &s, s.pitch, 0, &untranslated);
	if (bad || chrome9_cbu_gart_hash(t) != t->gart_hash) {
		dev_err(t->dev,
			"CBU R: window off, %u dwords or the GART hash differ after the routing change, restored\n",
			bad);
		ret = -EIO;
		goto out;
	}
	dev_info(t->dev, "CBU R: window off, raw surface and GART unchanged\n");
	ret = chrome9_cbu_route_probe(t, &s);
out:
	chrome9_cbu_disable(t->cdev, t->win);
	chrome9_cbu_route_restore(t, &r);
	if (!ret)
		ret = chrome9_cbu_finish(t, &s, t->win);
	chrome9_cbu_surface_free(t, &s);
	pci_dev_put(r.d0f2);
	return ret;
}

static const char * const chrome9_cbu_step_name[CHROME9_CBU_STEPS] = {
	[CHROME9_CBU_STEP_DUMP] = "C0",
	[CHROME9_CBU_STEP_READBACK] = "C1",
	[CHROME9_CBU_STEP_IDENTITY] = "C2",
	[CHROME9_CBU_STEP_PITCH_PIXELS] = "C3a",
	[CHROME9_CBU_STEP_PITCH_TILES] = "C3b",
	[CHROME9_CBU_STEP_MATRIX] = "C3c",
	[CHROME9_CBU_STEP_WRITE] = "C4",
	[CHROME9_CBU_STEP_BOUNDS] = "C5",
	[CHROME9_CBU_STEP_BPP] = "C6",
	[CHROME9_CBU_STEP_TWO] = "C9",
	[CHROME9_CBU_STEP_TIMING] = "C10",
	[CHROME9_CBU_STEP_TILE16] = "C8",
	[CHROME9_CBU_STEP_ROUTE] = "R",
};

static int chrome9_cbu_run_step(struct chrome9_cbu_test *t, int step)
{
	switch (step) {
	case CHROME9_CBU_STEP_DUMP:
		return chrome9_cbu_step_dump(t);
	case CHROME9_CBU_STEP_READBACK:
		return chrome9_cbu_step_readback(t);
	case CHROME9_CBU_STEP_IDENTITY:
		return chrome9_cbu_step_identity(t);
	case CHROME9_CBU_STEP_PITCH_PIXELS:
		return chrome9_cbu_step_pitch(t, CHROME9_CBU_PITCH_PIXELS);
	case CHROME9_CBU_STEP_PITCH_TILES:
		return chrome9_cbu_step_pitch(t, CHROME9_CBU_PITCH_TILES);
	case CHROME9_CBU_STEP_MATRIX:
		return chrome9_cbu_step_matrix(t);
	case CHROME9_CBU_STEP_WRITE:
		return chrome9_cbu_step_write(t);
	case CHROME9_CBU_STEP_BOUNDS:
		return chrome9_cbu_step_bounds(t);
	case CHROME9_CBU_STEP_BPP:
		return chrome9_cbu_step_bpp(t);
	case CHROME9_CBU_STEP_TWO:
		return chrome9_cbu_step_two(t);
	case CHROME9_CBU_STEP_TIMING:
		return chrome9_cbu_step_timing(t);
	case CHROME9_CBU_STEP_TILE16:
		return chrome9_cbu_step_tile16(t);
	case CHROME9_CBU_STEP_ROUTE:
		return chrome9_cbu_step_route(t);
	}
	return -EINVAL;
}

/* A step that needs the pitch unit, when neither C3 step decided it. */
static bool chrome9_cbu_needs_mode(int step)
{
	return step >= CHROME9_CBU_STEP_MATRIX && step != CHROME9_CBU_STEP_ROUTE;
}

static void chrome9_cbu_selftest(struct chrome9_device *cdev)
{
	const int last = chrome9_cbu_route ? CHROME9_CBU_STEP_ROUTE :
			 min(chrome9_cbu_selftest_steps,
			     CHROME9_CBU_STEP_ROUTE - 1);
	struct chrome9_cbu_test *t;
	unsigned int w, i;
	int step, ret = 0;
	u32 ctl;
	u64 hash;

	if (!cdev->sl_ready || !cdev->mmio || !cdev->sl_wc) {
		dev_warn(&cdev->pdev->dev,
			 "CBU ladder skipped: no engine or local pool\n");
		return;
	}
	if (chrome9_cbu_window < 3 || chrome9_cbu_window >= CHROME9_CBU_WINDOWS) {
		dev_warn(&cdev->pdev->dev, "CBU ladder: cbu_window %u invalid\n",
			 chrome9_cbu_window);
		return;
	}
	t = kzalloc_obj(*t);
	if (!t)
		return;
	t->cdev = cdev;
	t->dev = &cdev->pdev->dev;
	t->win = chrome9_cbu_window;
	t->mode = chrome9_cbu_pitch_mode;
	/* Manual: control bits 7:0 are RW; C1 measures the other fields. */
	t->mask[CHROME9_CBU_CTL] = 0xff;
	t->tail = ioremap(pci_resource_start(cdev->pdev, 2) + cdev->vram_size -
			  CHROME9_ENGINE_VRAM_RESERVED,
			  CHROME9_ENGINE_VRAM_RESERVED);
	if (!t->tail) {
		kfree(t);
		return;
	}
	t->gart_hash = chrome9_cbu_gart_hash(t);
	dev_info(t->dev,
		 "CBU ladder: steps 1..%d skip %#x route %#x window %u pitch mode %d GART hash %016llx\n",
		 last, chrome9_cbu_skip, chrome9_cbu_route, t->win, t->mode,
		 t->gart_hash);
	for (step = 1; step <= last; step++) {
		t->step = chrome9_cbu_step_name[step];
		/* C0 takes the snapshot the restore needs. */
		if ((step > CHROME9_CBU_STEP_DUMP &&
		     chrome9_cbu_skip & BIT(step)) ||
		    (chrome9_cbu_route && step > CHROME9_CBU_STEP_READBACK &&
		     step < CHROME9_CBU_STEP_ROUTE) ||
		    (step == CHROME9_CBU_STEP_PITCH_PIXELS &&
		     t->mode == CHROME9_CBU_PITCH_TILES) ||
		    (step == CHROME9_CBU_STEP_PITCH_TILES && t->mode >= 0)) {
			dev_info(t->dev, "CBU %s: skipped\n", t->step);
			continue;
		}
		if (chrome9_cbu_needs_mode(step) && t->mode < 0) {
			dev_err(t->dev, "CBU %s: pitch unit unknown, stop\n",
				t->step);
			ret = 1;
			break;
		}
		for (w = 0; w < CHROME9_CBU_WINDOWS; w++) {
			ctl = readl(chrome9_cbu_reg(cdev, w, CHROME9_CBU_CTL));
			if (step > CHROME9_CBU_STEP_DUMP &&
			    (ctl & CHROME9_CBU_ENABLE))
				break;
		}
		if (w < CHROME9_CBU_WINDOWS) {
			dev_err(t->dev,
				"CBU %s: window %u enabled before the step (%08x), stop\n",
				t->step, w, ctl);
			ret = 1;
			break;
		}
		ret = chrome9_cbu_run_step(t, step);
		hash = chrome9_cbu_gart_hash(t);
		if (hash != t->gart_hash) {
			dev_err(t->dev, "CBU %s: GART hash %016llx changed, stop\n",
				t->step, hash);
			ret = -EIO;
		}
		/* Candidate A failing is a result: C3b tries candidate B. */
		if (ret > 0 && step == CHROME9_CBU_STEP_PITCH_PIXELS &&
		    chrome9_cbu_pitch_mode < 0) {
			dev_info(t->dev, "CBU C3a: pixel pitch rejected\n");
			ret = 0;
			continue;
		}
		dev_info(t->dev, "CBU %s: %s\n", t->step,
			 ret > 0 ? "MISMATCH, stop" : ret < 0 ? "ERROR, stop" :
			 "pass");
		if (ret)
			break;
	}
	for (w = 0; w < CHROME9_CBU_WINDOWS; w++) {
		if (!(t->touched & BIT(w)))
			continue;
		chrome9_cbu_write(cdev, w, t->snapshot[w]);
		dev_info(t->dev, "CBU ladder: window %u restored to ctl %08x\n",
			 w, t->snapshot[w][CHROME9_CBU_CTL]);
	}
	if (!ret && t->mode >= 0)
		cdev->cbu->pitch_mode = t->mode;
	i = step > last ? last : step;
	dev_info(t->dev, "CBU ladder: %s at step %u (%s), pitch unit %s\n",
		 ret ? "stopped" : "completed", i,
		 i ? chrome9_cbu_step_name[i] : "none",
		 t->mode < 0 ? "unknown" : chrome9_cbu_mode_name(t->mode));
	iounmap(t->tail);
	kfree(t);
}

int chrome9_cbu_init(struct chrome9_device *cdev)
{
	struct chrome9_cbu *cbu;

	cbu = drmm_kzalloc(&cdev->drm, sizeof(*cbu), GFP_KERNEL);
	if (!cbu)
		return -ENOMEM;
	cbu->pitch_mode = chrome9_cbu_pitch_mode;
	cdev->cbu = cbu;
	if (chrome9_cbu_selftest_steps > 0)
		chrome9_cbu_selftest(cdev);
	return 0;
}
