// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>

#include <drm/drm_dumb_buffers.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
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
	    !IS_ALIGNED(pitch, CHROME9_SCANOUT_PITCH_ALIGN) ||
	    (offset && offset != PAGE_SIZE))
		return -EINVAL;

	return 0;
}

int chrome9_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			    struct drm_mode_create_dumb *args)
{
	struct chrome9_device *cdev = to_chrome9(drm);
	struct drm_gem_shmem_object *shmem_obj;
	struct drm_gem_object *obj;
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
	if (!cdev->scanout_size)
		return -EOPNOTSUPP;
	if (args->size > cdev->scanout_size)
		return -ENOSPC;

	shmem_obj = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);
	obj = &shmem_obj->base;
	ret = drm_gem_handle_create(file, obj, &args->handle);
	drm_gem_object_put(obj);
	return ret;
}

struct drm_framebuffer *
chrome9_gem_fb_create(struct drm_device *drm, struct drm_file *file,
		      const struct drm_format_info *info,
		      const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct chrome9_device *cdev = to_chrome9(drm);
	u64 modifier;
	u64 footprint;
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
	if (!cdev->scanout_size)
		return ERR_PTR(-EOPNOTSUPP);
	footprint = (u64)mode_cmd->pitches[0] * mode_cmd->height;
	if (footprint > cdev->scanout_size)
		return ERR_PTR(-ENOSPC);

	return drm_gem_fb_create(drm, file, info, mode_cmd);
}

int chrome9_gem_copy_fb_to_scanout(struct chrome9_device *cdev,
				   struct drm_framebuffer *fb)
{
	struct drm_gem_object *obj;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	size_t row_bytes;
	size_t footprint;
	unsigned int y;
	int ret;

	if (!cdev->scanout_size || !cdev->vram || !fb)
		return -EOPNOTSUPP;
	if (fb->format->format != DRM_FORMAT_XRGB8888 ||
	    fb->format->num_planes != 1)
		return -EINVAL;
	ret = chrome9_scanout_layout_validate(fb->width, fb->height,
					      fb->pitches[0], fb->offsets[0]);
	if (ret)
		return ret;
	footprint = (size_t)fb->pitches[0] * fb->height;
	if (footprint > cdev->scanout_size)
		return -ENOSPC;

	obj = drm_gem_fb_get_obj(fb, 0);
	if (!obj)
		return -EINVAL;
	if (fb->offsets[0] > obj->size ||
	    footprint > obj->size - fb->offsets[0])
		return -EINVAL;
	ret = drm_gem_vmap(obj, &map);
	if (ret)
		return ret;
	if (map.is_iomem) {
		ret = -EOPNOTSUPP;
		goto out_vunmap;
	}

	row_bytes = (size_t)fb->width * fb->format->cpp[0];
	mutex_lock(&cdev->scanout_lock);
	for (y = 0; y < fb->height; y++)
		memcpy_toio(cdev->vram + cdev->scanout_offset +
			    (size_t)y * fb->pitches[0],
			    map.vaddr + fb->offsets[0] +
			    (size_t)y * fb->pitches[0], row_bytes);
	/* Flush posted BAR writes before an eventual scanout-base update. */
	wmb();
	readb(cdev->vram + cdev->scanout_offset +
	      (size_t)(fb->height - 1) * fb->pitches[0] + row_bytes - 1);
	mutex_unlock(&cdev->scanout_lock);

out_vunmap:
	drm_gem_vunmap(obj, &map);
	return ret;
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
