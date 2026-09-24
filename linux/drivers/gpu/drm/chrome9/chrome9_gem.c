// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */

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
#include <drm/drm_modeset_lock.h>
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
#define CHROME9_CURSOR_BYTES	(64U * 64U * 4U)

struct chrome9_framebuffer {
	struct drm_framebuffer base;
	/* Protects the shared mapping and atomic-state reference count. */
	struct mutex lock;
	struct chrome9_sf_mapping *mapping;
	unsigned int pin_count;
};

static void chrome9_gem_free(struct drm_gem_object *obj)
{
	struct chrome9_bo *bo = to_chrome9_bo(obj);

	if (!iosys_map_is_null(&bo->kmap)) {
		dma_resv_lock(obj->resv, NULL);
		drm_gem_shmem_vunmap_locked(&bo->base, &bo->kmap);
		dma_resv_unlock(obj->resv);
	}
	drm_gem_shmem_object_free(obj);
}

static const struct drm_gem_object_funcs chrome9_gem_funcs = {
	.free = chrome9_gem_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

/*
 * Keep one kernel mapping reference for the object's lifetime, so per-job
 * vmap/vunmap only count uses instead of rebuilding and tearing down the
 * page table range (TLB flush) on every draw.  Caller holds the resv lock.
 */
void chrome9_bo_hold_kmap(struct chrome9_bo *bo)
{
	struct iosys_map map;

	if (!iosys_map_is_null(&bo->kmap) || drm_gem_is_imported(&bo->base.base))
		return;
	if (!drm_gem_shmem_vmap_locked(&bo->base, &map))
		bo->kmap = map;
}

struct drm_gem_object *chrome9_gem_create_object(struct drm_device *drm,
						 size_t size)
{
	struct chrome9_bo *bo = kzalloc_obj(*bo);

	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.base.funcs = &chrome9_gem_funcs;
	/* Implicit until userspace opts in; nothing is known about caches. */
	bo->cpu_dirty = true;
	bo->dirty_high = U64_MAX;
	return &bo->base.base;
}

static void
chrome9_gem_set_dma32(struct drm_gem_shmem_object *shmem_obj)
{
	struct address_space *mapping = shmem_obj->base.filp->f_mapping;
	gfp_t mask = mapping_gfp_mask(mapping);

	/* drm_gem_get_pages() requires this zone to remain stable thereafter. */
	mapping_set_gfp_mask(mapping,
			     (mask & ~GFP_ZONEMASK) | __GFP_DMA32);
}

struct drm_gem_object *chrome9_gem_cursor_create(struct drm_device *drm)
{
	struct drm_gem_shmem_object *shmem;

	shmem = drm_gem_shmem_create(drm, CHROME9_CURSOR_BYTES);
	if (IS_ERR(shmem))
		return ERR_CAST(shmem);
	/* Cursor SF PTEs are 32-bit even when scanout uses a wider DMA mask. */
	chrome9_gem_set_dma32(shmem);
	return &shmem->base;
}

static inline struct chrome9_framebuffer *
to_chrome9_framebuffer(struct drm_framebuffer *fb)
{
	return container_of(fb, struct chrome9_framebuffer, base);
}

static int chrome9_gem_copy_fb_damage_to_scanout(
	struct chrome9_device *cdev, struct drm_framebuffer *fb,
	const struct drm_clip_rect *clips, unsigned int num_clips);

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

static int chrome9_fb_dirty(struct drm_framebuffer *fb, struct drm_file *file,
			    unsigned int flags, unsigned int color,
			    struct drm_clip_rect *clips,
			    unsigned int num_clips)
{
	struct chrome9_device *cdev = to_chrome9(fb->dev);
	struct drm_plane_state *plane_state;
	int ret = 0;

	/*
	 * Xorg's modesetting driver publishes front-buffer damage through
	 * DIRTYFB.  Direct SF scanout needs only store ordering, while the
	 * compatibility scanout path must mirror the active shmem framebuffer
	 * into its fixed BAR2 interval.
	 */
	if (!cdev->scanout_size) {
		wmb();
		return 0;
	}

	drm_modeset_lock(&cdev->display_pipe.plane.mutex, NULL);
	plane_state = cdev->display_pipe.plane.state;
	if (plane_state && plane_state->fb == fb &&
	    cdev->display_pipe.crtc.state &&
	    cdev->display_pipe.crtc.state->active)
		ret = chrome9_gem_copy_fb_damage_to_scanout(
			cdev, fb,
			flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY ? NULL : clips,
			flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY ? 0 : num_clips);
	drm_modeset_unlock(&cdev->display_pipe.plane.mutex);
	return ret;
}

static const struct drm_framebuffer_funcs chrome9_fb_funcs = {
	.destroy = chrome9_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty = chrome9_fb_dirty,
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

	if ((info->format != DRM_FORMAT_XRGB8888 &&
	     info->format != DRM_FORMAT_ARGB8888) || info->num_planes != 1)
		return ERR_PTR(-EINVAL);
	/* ARGB is exposed only for the bounded IGA1 cursor, never scanout. */
	if (info->format == DRM_FORMAT_ARGB8888 &&
	    (mode_cmd->width > 64 || mode_cmd->height > 64 ||
	     mode_cmd->offsets[0]))
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
	/* A layout offset of 0 names the first row of a guarded object. */
	if (to_chrome9_bo(obj)->create_flags &
	    DRM_CHROME9_GEM_CREATE_GUARDED_SHARED &&
	    !chrome9_fb->base.offsets[0]) {
		if (obj->size < PAGE_SIZE + (u64)chrome9_fb->base.pitches[0] *
				chrome9_fb->base.height) {
			chrome9_fb_destroy(&chrome9_fb->base);
			return ERR_PTR(-EINVAL);
		}
		chrome9_fb->base.offsets[0] = PAGE_SIZE;
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

static int chrome9_gem_copy_fb_damage_to_scanout(
	struct chrome9_device *cdev, struct drm_framebuffer *fb,
	const struct drm_clip_rect *clips, unsigned int num_clips)
{
	struct drm_gem_object *obj;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	u8 __iomem *last = NULL;
	size_t row_bytes;
	size_t footprint;
	unsigned int i, y;
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

	mutex_lock(&cdev->scanout_lock);
	if (!clips || !num_clips) {
		row_bytes = (size_t)fb->width * fb->format->cpp[0];
		for (y = 0; y < fb->height; y++) {
			u8 __iomem *dst = cdev->vram + cdev->scanout_offset +
					  (size_t)y * fb->pitches[0];

			memcpy_toio(dst, map.vaddr + fb->offsets[0] +
				    (size_t)y * fb->pitches[0], row_bytes);
			last = dst + row_bytes - 1;
		}
	} else {
		for (i = 0; i < num_clips; i++) {
			u32 x1 = min_t(u32, clips[i].x1, fb->width);
			u32 x2 = min_t(u32, clips[i].x2, fb->width);
			u32 y1 = min_t(u32, clips[i].y1, fb->height);
			u32 y2 = min_t(u32, clips[i].y2, fb->height);
			size_t x_offset;

			if (x1 >= x2 || y1 >= y2)
				continue;
			x_offset = (size_t)x1 * fb->format->cpp[0];
			row_bytes = (size_t)(x2 - x1) * fb->format->cpp[0];
			for (y = y1; y < y2; y++) {
				u8 __iomem *dst = cdev->vram +
					cdev->scanout_offset +
					(size_t)y * fb->pitches[0] + x_offset;

				memcpy_toio(dst, map.vaddr + fb->offsets[0] +
					    (size_t)y * fb->pitches[0] +
					    x_offset, row_bytes);
				last = dst + row_bytes - 1;
			}
		}
	}
	/* Flush posted BAR writes before an eventual scanout-base update. */
	wmb();
	if (last)
		readb(last);
	mutex_unlock(&cdev->scanout_lock);

out_vunmap:
	drm_gem_vunmap(obj, &map);
	return ret;
}

int chrome9_gem_copy_fb_to_scanout(struct chrome9_device *cdev,
				   struct drm_framebuffer *fb)
{
	return chrome9_gem_copy_fb_damage_to_scanout(cdev, fb, NULL, 0);
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

	if (args->flags & ~(DRM_CHROME9_GEM_CREATE_DMA32_SCANOUT |
			    DRM_CHROME9_GEM_CREATE_GUARDED_SHARED) ||
	    !args->size || args->handle || args->mmap_offset)
		return -EINVAL;
	if ((args->flags & DRM_CHROME9_GEM_CREATE_GUARDED_SHARED) &&
	    (!IS_ALIGNED(args->size, PAGE_SIZE) || args->size <= 2 * PAGE_SIZE))
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
	to_chrome9_bo(obj)->create_flags = args->flags;
	/*
	 * This VX900 path has an exact 32-bit DMA mask and no IOMMU.  Every GEM
	 * object submitted to SF must therefore live below 4 GiB; otherwise the
	 * generic shmem DMA attachment permanently consumes SWIOTLB slots and the
	 * GPU executes against the bounce copy.  The userspace flag remains the
	 * stronger scanout contract (eager allocation plus explicit verification),
	 * while unflagged render resources receive the same physical placement.
	 */
	if (chrome9_dma32_scanout_supported(to_chrome9(drm)))
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

int chrome9_gem_info_ioctl(struct drm_device *drm, void *data,
			   struct drm_file *file)
{
	struct drm_chrome9_gem_info *args = data;
	struct drm_gem_object *obj;

	if (args->flags || args->size)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	if (obj->dev == drm && !drm_gem_is_imported(obj))
		args->flags = to_chrome9_bo(obj)->create_flags;
	args->size = obj->size;
	drm_gem_object_put(obj);
	return 0;
}
