// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "chrome9_drv.h"

/*
 * This is intentionally a non-driving scaffold.  It lets the atomic object
 * topology and GEM framebuffer ABI evolve without claiming that an output is
 * usable.  The first real KMS patch must add state save/restore, EDID/detect,
 * mode validation, timing/FIFO/PLL programming and vblank-backed flip events
 * before changing either callback below.
 */
static enum drm_connector_status
chrome9_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_disconnected;
}

static int chrome9_connector_get_modes(struct drm_connector *connector)
{
	return 0;
}

static const struct drm_connector_helper_funcs chrome9_connector_helper_funcs = {
	.get_modes = chrome9_connector_get_modes,
};

static const struct drm_connector_funcs chrome9_connector_funcs = {
	.detect = chrome9_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int chrome9_pipe_check(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *plane_state,
			      struct drm_crtc_state *crtc_state)
{
	if (crtc_state->enable)
		return -EOPNOTSUPP;

	return 0;
}

static void chrome9_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_plane_state)
{
	struct chrome9_device *cdev = to_chrome9(pipe->crtc.dev);
	struct drm_plane_state *plane_state = pipe->plane.state;
	int ret;

	if (!plane_state || !plane_state->fb)
		return;

	ret = chrome9_gem_copy_fb_to_scanout(cdev, plane_state->fb);
	if (ret)
		dev_err_ratelimited(&cdev->pdev->dev,
				    "BAR2 scanout staging copy failed: %d\n", ret);
}

static const struct drm_simple_display_pipe_funcs chrome9_pipe_funcs = {
	.check = chrome9_pipe_check,
	.update = chrome9_pipe_update,
};

static const u32 chrome9_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 chrome9_primary_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const struct drm_mode_config_funcs chrome9_mode_config_funcs = {
	.fb_create = chrome9_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int chrome9_scanout_carveout_init(struct chrome9_device *cdev,
					 unsigned long offset,
					 unsigned long size)
{
	void __iomem * const *iomap_table;
	resource_size_t usable;

	mutex_init(&cdev->scanout_lock);
	if (offset == ULONG_MAX && !size)
		return 0;
	if (offset == ULONG_MAX || !size)
		return dev_err_probe(&cdev->pdev->dev, -EINVAL,
			"scanout_offset and scanout_size must be supplied together\n");
	if (!IS_ALIGNED(offset, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
		return dev_err_probe(&cdev->pdev->dev, -EINVAL,
			"scanout carveout must be page aligned\n");
	if (cdev->vram_size < SZ_64K)
		return dev_err_probe(&cdev->pdev->dev, -ENODEV,
			"BAR2 size is unavailable for scanout validation\n");

	usable = cdev->vram_size - SZ_64K;
	if (offset > usable || size > usable - offset)
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"scanout carveout [%#lx,%#lx) overlaps the final 64-KiB GART or exceeds BAR2\n",
			offset, offset + size);
	if (!chrome9_iga1_base_valid(offset) ||
	    offset + size > CHROME9_IGA1_BASE_MAX)
		return dev_err_probe(&cdev->pdev->dev, -ERANGE,
			"scanout carveout is outside the IGA1 addressable range\n");

	iomap_table = pcim_iomap_table(cdev->pdev);
	if (iomap_table && iomap_table[2])
		cdev->vram = iomap_table[2];
	else
		cdev->vram = pcim_iomap(cdev->pdev, 2, 0);
	if (!cdev->vram)
		return dev_err_probe(&cdev->pdev->dev, -ENOMEM,
				     "cannot map BAR2 scanout carveout\n");

	cdev->scanout_offset = offset;
	cdev->scanout_size = size;
	dev_info(&cdev->pdev->dev,
		 "lab-only BAR2 scanout staging enabled: [%#lx,%#lx), final 64 KiB reserved for GART\n",
		 offset, offset + size);
	return 0;
}

int chrome9_kms_init(struct chrome9_device *cdev,
		     unsigned long scanout_offset,
		     unsigned long scanout_size)
{
	struct drm_device *drm = &cdev->drm;
	int ret;

	ret = chrome9_scanout_carveout_init(cdev, scanout_offset,
					    scanout_size);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 2048;
	drm->mode_config.max_height = 2048;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &chrome9_mode_config_funcs;

	drm_connector_helper_add(&cdev->display_connector,
				 &chrome9_connector_helper_funcs);
	ret = drm_connector_init(drm, &cdev->display_connector,
				 &chrome9_connector_funcs,
				 DRM_MODE_CONNECTOR_VGA);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &cdev->display_pipe,
					   &chrome9_pipe_funcs,
					   chrome9_primary_formats,
					   ARRAY_SIZE(chrome9_primary_formats),
					   chrome9_primary_modifiers,
					   &cdev->display_connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	if (!cdev->scanout_size)
		dev_warn(&cdev->pdev->dev,
			 "KMS scanout staging disabled: no private BAR2 carveout supplied\n");
	return 0;
}
