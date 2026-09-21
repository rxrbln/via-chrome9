// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Chrome9 contributors */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

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
#include "chrome9_engine.h"

#define CHROME9_MAX_BO_SIZE	(256ULL * 1024 * 1024)
#define CHROME9_SCANOUT_MAX_WIDTH	2048U
#define CHROME9_SCANOUT_MAX_HEIGHT	2048U
#define CHROME9_SCANOUT_PITCH_ALIGN	8U
#define CHROME9_SCANOUT_PITCH_MAX	0x3ff8U
#define CHROME9_DMA32_SCANOUT_MAX_SIZE \
	((u64)CHROME9_SCANOUT_PITCH_MAX * CHROME9_SCANOUT_MAX_HEIGHT)

struct chrome9_framebuffer {
	struct drm_framebuffer base;
	/* Protects the shared mapping and atomic-state reference count. */
	struct mutex lock;
	struct chrome9_sf_mapping *mapping;
	unsigned int pin_count;
};

static void
chrome9_gem_set_dma32(struct drm_gem_shmem_object *shmem_obj)
{
	struct address_space *mapping = shmem_obj->base.filp->f_mapping;
	gfp_t mask = mapping_gfp_mask(mapping);

	/* drm_gem_get_pages() requires this zone to remain stable thereafter. */
	mapping_set_gfp_mask(mapping,
			     (mask & ~GFP_ZONEMASK) | __GFP_DMA32);
}

static inline struct chrome9_framebuffer *
to_chrome9_framebuffer(struct drm_framebuffer *fb)
{
	return container_of(fb, struct chrome9_framebuffer, base);
}

static void chrome9_fb_destroy(struct drm_framebuffer *fb)
{
	struct chrome9_framebuffer *chrome9_fb = to_chrome9_framebuffer(fb);
	struct chrome9_sf_mapping *mapping;

	mutex_lock(&chrome9_fb->lock);
	WARN_ON(chrome9_fb->pin_count);
	mapping = chrome9_fb->mapping;
	chrome9_fb->mapping = NULL;
	chrome9_fb->pin_count = 0;
	mutex_unlock(&chrome9_fb->lock);
	chrome9_sf_unpin(mapping);
	drm_gem_fb_destroy(fb);
}

static const struct drm_framebuffer_funcs chrome9_fb_funcs = {
	.destroy = chrome9_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

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
	if (args->size > CHROME9_MAX_BO_SIZE)
		return -E2BIG;

	shmem_obj = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);
	if (chrome9_dma32_scanout_supported(to_chrome9(drm)))
		chrome9_gem_set_dma32(shmem_obj);
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
	struct chrome9_framebuffer *chrome9_fb;
	struct drm_gem_object *obj;
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

	chrome9_fb = kzalloc_obj(*chrome9_fb);
	if (!chrome9_fb)
		return ERR_PTR(-ENOMEM);
	mutex_init(&chrome9_fb->lock);
	ret = drm_gem_fb_init_with_funcs(drm, &chrome9_fb->base, file, info,
					 mode_cmd, &chrome9_fb_funcs);
	if (ret) {
		kfree(chrome9_fb);
		return ERR_PTR(ret);
	}
	obj = drm_gem_fb_get_obj(&chrome9_fb->base, 0);
	if (!obj || obj->dev != drm || drm_gem_is_imported(obj)) {
		chrome9_fb_destroy(&chrome9_fb->base);
		return ERR_PTR(-EINVAL);
	}

	return &chrome9_fb->base;
}

int chrome9_gem_fb_pin(struct drm_framebuffer *fb)
{
	struct chrome9_framebuffer *chrome9_fb = to_chrome9_framebuffer(fb);
	struct chrome9_device *cdev = to_chrome9(fb->dev);
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
	struct chrome9_sf_mapping *mapping;
	int ret = 0;

	mutex_lock(&chrome9_fb->lock);
	if (chrome9_fb->pin_count) {
		if (!chrome9_engine_ready(cdev))
			ret = -EIO;
		else if (chrome9_fb->pin_count == UINT_MAX)
			ret = -EOVERFLOW;
		else
			chrome9_fb->pin_count++;
		goto out_unlock;
	}
	ret = chrome9_sf_pin(cdev, obj, &mapping);
	if (!ret) {
		chrome9_fb->mapping = mapping;
		chrome9_fb->pin_count = 1;
	}
out_unlock:
	mutex_unlock(&chrome9_fb->lock);
	return ret;
}

void chrome9_gem_fb_unpin(struct drm_framebuffer *fb)
{
	struct chrome9_framebuffer *chrome9_fb = to_chrome9_framebuffer(fb);
	struct chrome9_sf_mapping *mapping = NULL;

	mutex_lock(&chrome9_fb->lock);
	if (WARN_ON(!chrome9_fb->pin_count))
		goto out_unlock;
	if (--chrome9_fb->pin_count)
		goto out_unlock;
	mapping = chrome9_fb->mapping;
	chrome9_fb->mapping = NULL;
out_unlock:
	mutex_unlock(&chrome9_fb->lock);
	chrome9_sf_unpin(mapping);
}

int chrome9_gem_fb_sf_base(struct drm_framebuffer *fb, u32 *base)
{
	struct chrome9_framebuffer *chrome9_fb = to_chrome9_framebuffer(fb);
	u64 address;
	int ret = 0;

	mutex_lock(&chrome9_fb->lock);
	if (!chrome9_fb->pin_count || !chrome9_fb->mapping) {
		ret = -EINVAL;
		goto out_unlock;
	}
	address = (u64)chrome9_sf_mapping_base(chrome9_fb->mapping) +
		  fb->offsets[0];
	if (address > U32_MAX || !chrome9_iga1_base_valid(address)) {
		ret = -ERANGE;
		goto out_unlock;
	}
	*base = address;
out_unlock:
	mutex_unlock(&chrome9_fb->lock);
	return ret;
}

void chrome9_gem_fb_sync_for_device(struct drm_framebuffer *fb)
{
	struct chrome9_framebuffer *chrome9_fb = to_chrome9_framebuffer(fb);

	mutex_lock(&chrome9_fb->lock);
	if (WARN_ON(!chrome9_fb->pin_count || !chrome9_fb->mapping))
		goto out_unlock;
	chrome9_sf_sync_for_device(chrome9_fb->mapping);
out_unlock:
	mutex_unlock(&chrome9_fb->lock);
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
	struct scatterlist *sg;
	struct sg_table *sgt;
	unsigned int i;
	int ret;

	if (args->flags & ~DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT ||
	    !args->size || args->handle || args->mmap_offset)
		return -EINVAL;
	if ((args->flags & DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT) &&
	    args->size > CHROME9_DMA32_SCANOUT_MAX_SIZE)
		return -E2BIG;
	if (args->size > CHROME9_MAX_BO_SIZE)
		return -E2BIG;
	if ((args->flags & DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT) &&
	    !chrome9_dma32_scanout_supported(to_chrome9(drm)))
		return -EOPNOTSUPP;

	shmem_obj = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);
	obj = &shmem_obj->base;
	if (args->flags & DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT)
		chrome9_gem_set_dma32(shmem_obj);

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
	if (args->flags & DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT) {
		for (i = 0; i < obj->size >> PAGE_SHIFT; i++) {
			if (page_to_phys(shmem_obj->pages[i]) >= BIT_ULL(32)) {
				ret = -ERANGE;
				goto out_put;
			}
		}
		for_each_sgtable_sg(sgt, sg, i) {
			if (sg_dma_is_swiotlb(sg)) {
				ret = -ERANGE;
				goto out_put;
			}
		}
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
