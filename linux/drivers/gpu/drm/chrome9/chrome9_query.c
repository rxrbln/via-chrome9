// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <linux/dma-fence.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/chrome9_drm.h>

#include "chrome9_progcache.h"
#include "chrome9_query.h"

/* Query objects one DRM file may own. */
#define CHROME9_QUERY_MAX_PER_FILE	16384U
#define CHROME9_QUERY_WAIT_TIMEOUT	msecs_to_jiffies(5000)

static uint chrome9_occlusion_query = 1;
module_param_named(occlusion_query, chrome9_occlusion_query, uint, 0600);
MODULE_PARM_DESC(occlusion_query,
	"Occlusion queries: 0 off, 1 on (default: 1)");

struct chrome9_query {
	struct kref ref;
	spinlock_t lock;
	u64 samples;
	/* BEGIN starts a new interval; older draws no longer count. */
	u32 generation;
	bool active;
	int error;
	/* Fence of the last job with a draw of this interval. */
	struct dma_fence *last;
};

struct chrome9_file {
	struct xarray queries;
	struct chrome9_progcache *programs;
};

struct chrome9_progcache *chrome9_file_programs(struct drm_file *file)
{
	struct chrome9_file *cfile = file->driver_priv;

	return cfile ? cfile->programs : NULL;
}

unsigned int chrome9_occlusion_query_mode(void)
{
	return !!READ_ONCE(chrome9_occlusion_query);
}

static void chrome9_query_release(struct kref *ref)
{
	struct chrome9_query *query =
		container_of(ref, struct chrome9_query, ref);

	dma_fence_put(query->last);
	kfree(query);
}

struct chrome9_query *chrome9_query_get(struct chrome9_query *query)
{
	kref_get(&query->ref);
	return query;
}

void chrome9_query_put(struct chrome9_query *query)
{
	if (query)
		kref_put(&query->ref, chrome9_query_release);
}

/* The reference is taken under the xarray lock, so DESTROY cannot free it. */
static struct chrome9_query *chrome9_query_lookup(struct drm_file *file,
						  u32 id)
{
	struct chrome9_file *cfile = file->driver_priv;
	struct chrome9_query *query;

	xa_lock(&cfile->queries);
	query = xa_load(&cfile->queries, id);
	if (query)
		kref_get(&query->ref);
	xa_unlock(&cfile->queries);
	return query;
}

struct chrome9_query *chrome9_query_get_active(struct drm_file *file, u32 id,
					       u32 *generation)
{
	struct chrome9_query *query = chrome9_query_lookup(file, id);
	bool active;

	if (!query)
		return ERR_PTR(-ENOENT);
	spin_lock(&query->lock);
	active = query->active;
	*generation = query->generation;
	spin_unlock(&query->lock);
	if (!active) {
		chrome9_query_put(query);
		return ERR_PTR(-EINVAL);
	}
	return query;
}

/* A draw counts only if its interval is still open when it is published. */
bool chrome9_query_attach(struct chrome9_query *query, u32 generation,
			  struct dma_fence *fence)
{
	struct dma_fence *old = NULL;
	bool attached;

	spin_lock(&query->lock);
	attached = query->active && query->generation == generation;
	if (attached && query->last != fence) {
		old = query->last;
		query->last = dma_fence_get(fence);
	}
	spin_unlock(&query->lock);
	dma_fence_put(old);
	return attached;
}

void chrome9_query_add(struct chrome9_query *query, u32 generation,
		       u32 samples, int error)
{
	spin_lock(&query->lock);
	if (query->generation == generation) {
		query->samples += samples;
		if (error && !query->error)
			query->error = error;
	}
	spin_unlock(&query->lock);
}

int chrome9_query_open(struct drm_device *drm, struct drm_file *file)
{
	struct chrome9_file *cfile = kzalloc_obj(*cfile);

	if (!cfile)
		return -ENOMEM;
	cfile->programs = chrome9_progcache_create();
	if (!cfile->programs) {
		kfree(cfile);
		return -ENOMEM;
	}
	xa_init_flags(&cfile->queries, XA_FLAGS_ALLOC1);
	file->driver_priv = cfile;
	return 0;
}

/* Jobs still holding a query keep it alive until they retire. */
void chrome9_query_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct chrome9_file *cfile = file->driver_priv;
	struct chrome9_query *query;
	unsigned long id;

	if (!cfile)
		return;
	xa_for_each(&cfile->queries, id, query)
		chrome9_query_put(query);
	xa_destroy(&cfile->queries);
	chrome9_progcache_destroy(cfile->programs);
	kfree(cfile);
	file->driver_priv = NULL;
}

static int chrome9_query_read(struct chrome9_query *query,
			      struct drm_chrome9_query *args)
{
	struct dma_fence *fence;
	u32 generation;
	long wait;
	int ret = 0;

	spin_lock(&query->lock);
	if (query->active)
		ret = -EBUSY;
	else if (query->error)
		ret = query->error;
	generation = query->generation;
	fence = dma_fence_get(query->last);
	spin_unlock(&query->lock);
	if (!ret && fence && (args->flags & DRM_CHROME9_QUERY_READ_WAIT) &&
	    !dma_fence_is_signaled(fence)) {
		wait = dma_fence_wait_timeout(fence, true,
					      CHROME9_QUERY_WAIT_TIMEOUT);
		if (wait < 0)
			ret = wait;
		else if (!wait)
			ret = -ETIMEDOUT;
	}
	if (!ret) {
		/* Retirement adds the samples before it signals the fence. */
		args->available = !fence || dma_fence_is_signaled(fence);
		spin_lock(&query->lock);
		if (query->generation != generation || query->active)
			ret = -EBUSY;
		else if (query->error)
			ret = query->error;
		args->result = query->samples;
		spin_unlock(&query->lock);
	}
	dma_fence_put(fence);
	return ret;
}

int chrome9_query_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct chrome9_file *cfile = file->driver_priv;
	struct drm_chrome9_query *args = data;
	struct dma_fence *old = NULL;
	struct chrome9_query *query;
	u32 id;
	int ret = 0;

	if (!chrome9_occlusion_query_mode())
		return -EOPNOTSUPP;
	if (args->pad || memchr_inv(args->reserved, 0, sizeof(args->reserved)) ||
	    (args->flags &&
	     (args->op != DRM_CHROME9_QUERY_OP_READ ||
	      args->flags & ~DRM_CHROME9_QUERY_READ_WAIT)) ||
	    (args->op != DRM_CHROME9_QUERY_OP_CREATE && args->type))
		return -EINVAL;
	switch (args->op) {
	case DRM_CHROME9_QUERY_OP_CREATE:
		if (args->type != DRM_CHROME9_QUERY_TYPE_OCCLUSION || args->id)
			return -EINVAL;
		query = kzalloc_obj(*query);
		if (!query)
			return -ENOMEM;
		kref_init(&query->ref);
		spin_lock_init(&query->lock);
		ret = xa_alloc(&cfile->queries, &id, query,
			       XA_LIMIT(1, CHROME9_QUERY_MAX_PER_FILE),
			       GFP_KERNEL);
		if (ret) {
			kfree(query);
			return ret == -EBUSY ? -ENOSPC : ret;
		}
		args->id = id;
		return 0;
	case DRM_CHROME9_QUERY_OP_DESTROY:
		query = xa_erase(&cfile->queries, args->id);
		if (!query)
			return -ENOENT;
		chrome9_query_put(query);
		return 0;
	case DRM_CHROME9_QUERY_OP_BEGIN:
	case DRM_CHROME9_QUERY_OP_END:
	case DRM_CHROME9_QUERY_OP_READ:
		break;
	default:
		return -EINVAL;
	}
	query = chrome9_query_lookup(file, args->id);
	if (!query)
		return -ENOENT;
	if (args->op == DRM_CHROME9_QUERY_OP_READ) {
		args->result = 0;
		args->available = 0;
		ret = chrome9_query_read(query, args);
		chrome9_query_put(query);
		return ret;
	}
	spin_lock(&query->lock);
	if (args->op == DRM_CHROME9_QUERY_OP_BEGIN) {
		if (query->active) {
			ret = -EBUSY;
		} else {
			query->active = true;
			query->generation++;
			query->samples = 0;
			query->error = 0;
			old = query->last;
			query->last = NULL;
		}
	} else if (!query->active) {
		ret = -EINVAL;
	} else {
		query->active = false;
	}
	spin_unlock(&query->lock);
	dma_fence_put(old);
	chrome9_query_put(query);
	return ret;
}
