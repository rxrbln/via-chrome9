// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/errno.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
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

static const struct drm_simple_display_pipe_funcs chrome9_pipe_funcs = {
	.check = chrome9_pipe_check,
};

static const u32 chrome9_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 chrome9_primary_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const struct drm_mode_config_funcs chrome9_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int chrome9_kms_init(struct chrome9_device *cdev)
{
	struct drm_device *drm = &cdev->drm;
	int ret;

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
	return 0;
}
