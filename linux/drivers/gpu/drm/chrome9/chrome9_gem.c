// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/dma-mapping.h>
#include <linux/mm.h>

#include <drm/drm_dumb_buffers.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mode.h>
#include <drm/drm_vma_manager.h>

#include "chrome9_drv.h"

#define CHROME9_MAX_BO_SIZE	(256ULL * 1024 * 1024)
#define CHROME9_SCANOUT_MAX_WIDTH	2048U
#define CHROME9_SCANOUT_MAX_HEIGHT	2048U
#define CHROME9_SCANOUT_PITCH_ALIGN	8U
#define CHROME9_SCANOUT_PITCH_MAX	0x3ff8U

static int chrome9_scanout_layout_validate(u32 width, u32 height, u32 pitch,
					   u32 offset)
{
	u32 min_pitch;

	if (!width || !height || width > CHROME9_SCANOUT_MAX_WIDTH ||
	    height > CHROME9_SCANOUT_MAX_HEIGHT)
		return -EINVAL;

	min_pitch = width * 4;
	if (pitch < min_pitch || pitch > CHROME9_SCANOUT_PITCH_MAX ||
	    !IS_ALIGNED(pitch, CHROME9_SCANOUT_PITCH_ALIGN) || offset)
		return -EINVAL;

	return 0;
}

int chrome9_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			    struct drm_mode_create_dumb *args)
{
	int ret;

	/* The only primary-plane format exposed by KMS is linear XRGB8888. */
	if (args->flags || args->bpp != 32)
		return -EINVAL;

	ret = drm_mode_size_dumb(drm, args, CHROME9_SCANOUT_PITCH_ALIGN,
				 PAGE_SIZE);
	if (ret)
		return ret;
	ret = chrome9_scanout_layout_validate(args->width, args->height,
					      args->pitch, 0);
	if (ret)
		return ret;

	/*
	 * Do not fall back to drm_gem_shmem_dumb_create().  Display fetches SL
	 * offsets, while render GEM objects intentionally remain SF-backed
	 * shmem.  BAR2 has no ownership table: viafb may put the primary at the
	 * bottom and its second framebuffer, VQ, and cursor at the top.  Until
	 * firmware scanout and those tail reservations can be bounded, there is
	 * no interval that a local-VRAM allocator may safely claim.
	 */
	return -EOPNOTSUPP;
}

struct drm_framebuffer *
chrome9_gem_fb_create(struct drm_device *drm, struct drm_file *file,
		      const struct drm_format_info *info,
		      const struct drm_mode_fb_cmd2 *mode_cmd)
{
	u64 modifier;
	int ret;

	if (info->format != DRM_FORMAT_XRGB8888 || info->num_planes != 1)
		return ERR_PTR(-EINVAL);
	if (mode_cmd->flags & ~DRM_MODE_FB_MODIFIERS)
		return ERR_PTR(-EINVAL);

	modifier = mode_cmd->flags & DRM_MODE_FB_MODIFIERS ?
		mode_cmd->modifier[0] : DRM_FORMAT_MOD_LINEAR;
	if (modifier != DRM_FORMAT_MOD_LINEAR)
		return ERR_PTR(-EINVAL);
	ret = chrome9_scanout_layout_validate(mode_cmd->width, mode_cmd->height,
					      mode_cmd->pitches[0],
					      mode_cmd->offsets[0]);
	if (ret)
		return ERR_PTR(ret);

	/* Current handles are shmem render BOs, not BAR2 scanout BOs. */
	return ERR_PTR(-EOPNOTSUPP);
}

int chrome9_gem_create_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct drm_chrome9_gem_create *args = data;
	struct drm_gem_shmem_object *shmem_obj;
	struct drm_gem_object *obj;
	struct sg_table *sgt;
	int ret;

	if (args->flags || !args->size || args->size > CHROME9_MAX_BO_SIZE)
		return -EINVAL;
	if (args->handle || args->mmap_offset)
		return -EINVAL;

	shmem_obj = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);
	obj = &shmem_obj->base;

	/*
	 * Establish this device's DMA mapping before the object can be shared or
	 * acquire reservation fences. Submission then never has to perform a
	 * first dma_map_sgtable() ahead of waiting for an external writer.
	 */
	sgt = drm_gem_shmem_get_pages_sgt(shmem_obj);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto out_put;
	}
	dma_sync_sgtable_for_cpu(&to_chrome9(drm)->pdev->dev, sgt,
				 DMA_BIDIRECTIONAL);

	ret = drm_gem_handle_create(file, obj, &args->handle);
	if (!ret)
		args->mmap_offset = drm_vma_node_offset_addr(&obj->vma_node);

	/* The handle, on success, owns the userspace-visible reference. */
out_put:
	drm_gem_object_put(obj);
	return ret;
}
