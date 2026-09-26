/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef CHROME9_HDMI_H
#define CHROME9_HDMI_H

struct chrome9_device;
struct drm_crtc;
struct drm_encoder;

/* HDMI connector and encoder on crtc, cloneable with clone. */
int chrome9_hdmi_init(struct chrome9_device *cdev, struct drm_crtc *crtc,
		      struct drm_encoder *clone);

#endif
