# Minimal deferred shmem DMA-mapping patch plan

This is the code-level follow-up to
`dmabuf-handle-swiotlb-lifetime.md`.  It targets the Linux 7.2 Chrome9 kernel
at `f39a90d` and deliberately changes no UAPI or packet construction.  The
goal is narrow: a GEM object which is merely allocated, mmaped, exported, or
imported must not consume Chrome9's 32-bit SWIOTLB pool.  A DMA mapping exists
only while a proven GPU job or direct scanout role needs it.

## Recommended minimal representation

Use a Chrome9 shmem GEM subclass instead of a device-global pointer table.
This removes a second lookup lock and lets the object's existing reservation
lock serialize its one device mapping.

```c
struct chrome9_gem_object {
        /* Must be first: drm_gem_shmem_free() kfree()s this address. */
        struct drm_gem_shmem_object shmem;
        struct sg_table *dma_sgt;
        unsigned int dma_use_count;
};

static inline struct chrome9_gem_object *
to_chrome9_gem(struct drm_gem_shmem_object *shmem)
{
        return container_of(shmem, struct chrome9_gem_object, shmem);
}
```

Both `dma_sgt` and `dma_use_count` are protected by `shmem.base.resv`.  A
successful DMA-role get is valid only while its caller also holds a GEM object
reference.  Existing jobs retain their handle-lookup reference until cleanup;
`chrome9_sf_mapping` explicitly takes an object reference.  Quarantined jobs
and scanout mappings retain those references permanently.

Add `chrome9_gem_create_object(struct drm_device *, size_t)` to both Chrome9
`struct drm_driver` instances through `.gem_create_object`.  The callback
allocates `struct chrome9_gem_object`, assigns custom object functions, and
returns `&cobj->shmem.base`; shmem core performs the subsequent object init.
This covers normal GEM create, dumb buffers, and the private proxy allocated
by `drm_gem_shmem_prime_import_no_map()`.

The custom `drm_gem_object_funcs` should be the shmem set already used by the
7.2 helper, with only `.free` replaced:

```c
static const struct drm_gem_object_funcs chrome9_gem_object_funcs = {
        .free = chrome9_gem_object_free,
        .print_info = drm_gem_shmem_object_print_info,
        .pin = drm_gem_shmem_object_pin,
        .unpin = drm_gem_shmem_object_unpin,
        .get_sg_table = drm_gem_shmem_object_get_sg_table,
        .vmap = drm_gem_shmem_object_vmap,
        .vunmap = drm_gem_shmem_object_vunmap,
        .mmap = drm_gem_shmem_object_mmap,
        .vm_ops = &drm_gem_shmem_vm_ops,
};
```

`chrome9_gem_object_free()` must `WARN_ON(dma_use_count)` and
`WARN_ON(dma_sgt)`, then call `drm_gem_shmem_free(&cobj->shmem)`.  It must not
silently unmap a nonzero role: a role's object reference makes this state
unreachable, while a missing reference could mean hardware still reaches it.

## Exact Linux 7.2 mapping helpers

The needed functions are exported in this tree with these signatures:

```c
int drm_gem_shmem_pin_locked(struct drm_gem_shmem_object *shmem);
void drm_gem_shmem_unpin_locked(struct drm_gem_shmem_object *shmem);
struct sg_table *
drm_gem_shmem_get_sg_table(struct drm_gem_shmem_object *shmem);

int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                    enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
                       enum dma_data_direction dir, unsigned long attrs);
```

The locked shmem pin/unpin helpers require `dma_resv_assert_held()`.  Pinning
adds one logical shmem pin and, on the first pin, one pages-use reference.
`drm_gem_shmem_get_sg_table()` allocates a fresh SG table over those pinned
pages but does not DMA-map it.  This is distinct from
`drm_gem_shmem_get_pages_sgt()`, which takes the reservation itself and stores
a permanently cached mapping in `shmem->sgt`.

Implement these Chrome9 helpers in `chrome9_gem.c` and declare them in the
driver header:

```c
struct sg_table *
chrome9_gem_dma_get_locked(struct drm_gem_shmem_object *shmem);

void chrome9_gem_dma_put_locked(struct drm_gem_shmem_object *shmem);
```

### `chrome9_gem_dma_get_locked()`

The exact algorithm is:

1. `dma_resv_assert_held(shmem->base.resv)`.
2. Reject true imported proxy objects defensively.  Current submit and KMS
   validation already rejects them before this helper.
3. If `dma_use_count != 0`, reject `UINT_MAX`, increment the count, and return
   the existing non-NULL `dma_sgt`.  Role acquisition alone does not change
   CPU/device ownership.
4. Call `drm_gem_shmem_pin_locked(shmem)`.
5. Call `drm_gem_shmem_get_sg_table(shmem)`.
6. Call `dma_map_sgtable(shmem->base.dev->dev, sgt,
   DMA_BIDIRECTIONAL, 0)`.
7. Publish `dma_sgt=sgt`, `dma_use_count=1`, and return it.  Do not sync for
   CPU in this generic role helper.

The caller owns the access transition.  A draw calls
`dma_sync_sgtable_for_cpu()` after its producer-fence wait and before any vmap
access.  Direct scanout does not need CPU ownership and can be prepared before
the atomic helper waits its producer fence; it performs the existing
`dma_sync_sgtable_for_device()` after that wait and immediately before display
use.  In particular, doing a CPU sync in scanout prepare could copy stale
SWIOTLB bounce data back over memory which an unsignaled producer still owns.

Failure unwinds in the strict reverse order:

```text
dma_map failure: sg_free_table(sgt), kfree(sgt), shmem_unpin_locked
SG allocation failure: shmem_unpin_locked
pin failure: no state changed
```

Do not publish the pointer or increment the role count until all three setup
steps succeed.

### `chrome9_gem_dma_put_locked()`

Assert the reservation, warn and return on zero count, then decrement.  If a
role remains, return.  On the last role:

```c
sgt = cobj->dma_sgt;
cobj->dma_sgt = NULL;
dma_unmap_sgtable(shmem->base.dev->dev, sgt, DMA_BIDIRECTIONAL, 0);
sg_free_table(sgt);
kfree(sgt);
drm_gem_shmem_unpin_locked(shmem);
```

The direction and attributes must exactly match the map.  Clearing the
published pointer before unmap makes a bad reentrant observation fail loudly,
although reservation serialization should make reentry impossible.

This driver-owned SGT must never be assigned to `shmem->sgt`.  Shmem release
owns that field and assumes the separate pages-use accounting performed by
`drm_gem_shmem_get_pages_sgt_locked()`.

## Remove eager mapping from GEM create

Delete `sgt`, the `drm_gem_shmem_get_pages_sgt()` call, and the initial
`dma_sync_sgtable_for_cpu()` from `chrome9_gem_create_ioctl()`
(`chrome9_gem.c:296..326`).  Leave object creation, handle creation, mmap
offset return, and the final object put unchanged.

`chrome9_gem_dumb_create()` is already lazy.  It needs no analogous deletion.
Same-device PRIME import returns the original Chrome9 object, so it naturally
shares the subclass mapping and count.  True foreign proxies receive the
subclass too but remain rejected before DMA-role acquisition.

The private diagnostic shmem allocation in `chrome9_drv.c` may retain its
existing `drm_gem_shmem_get_pages_sgt()` initially.  It is engine-private,
never an Xorg pixmap, and its cached SGT dies with that diagnostic object.
Converting it is cleanup, not required for bounding per-BO Xorg pressure.

## Single-object submit conversion

`chrome9_submit_draw()` currently DMA-maps and collects PTEs before locking
the reservation.  Move both operations to after this existing sequence:

```c
dma_resv_lock_interruptible(resv, NULL);
dma_resv_wait_timeout(resv, dma_resv_usage_rw(true), true,
                      CHROME9_DEP_TIMEOUT);
dma_resv_reserve_fences(resv, 1);
```

Then:

```c
job->sgt = chrome9_gem_dma_get_locked(job->destination);
if (IS_ERR(job->sgt)) {
        ret = PTR_ERR(job->sgt);
        job->sgt = NULL;
        goto out_unlock_resv;
}
ret = chrome9_collect_ptes(job);
if (ret)
        goto out_dma_put;
dma_sync_sgtable_for_cpu(&cdev->pdev->dev, job->sgt,
                         DMA_BIDIRECTIONAL);
```

The existing vmap, guard preparation, final
`dma_sync_sgtable_for_device()`, fence insertion, submission, wait, CPU sync,
guard verification, scanout re-sync, and SF-PTE invalidation stay in their
current order.

On every non-quarantined success or error, after any vmap is released and
after SF PTE invalidation has succeeded, call
`chrome9_gem_dma_put_locked(job->destination)` before
`dma_resv_unlock(resv)`.  Clear `job->sgt`.  If retirement or PTE invalidation
is unproven, the quarantine path must skip put, keep `job->sgt`, and retain the
job/object exactly as today.

The practical label order is:

```text
out_vunmap -> out_dma_put -> out_unlock_resv -> fence/object cleanup
```

The quarantine branch continues to jump around `out_dma_put`.

## Multi-object `drm_exec` conversion

Linux 7.2 provides:

```c
void drm_exec_init(struct drm_exec *exec, u32 flags, unsigned int nr);
int drm_exec_prepare_obj(struct drm_exec *exec,
                         struct drm_gem_object *obj,
                         unsigned int num_fences);
void drm_exec_fini(struct drm_exec *exec);
long dma_resv_wait_timeout(struct dma_resv *obj,
                           enum dma_resv_usage usage,
                           bool intr, unsigned long timeout);
```

`drm_exec_prepare_obj()` locks `obj->resv`, takes an object reference, and
reserves the requested fence slots.  `drm_exec_fini()` unlocks every prepared
reservation and drops only those exec-owned references.  The existing
`drm_exec_until_all_locked()` plus `drm_exec_retry_on_contention()` loop is
correct and should remain unchanged.  Chrome9 already rejects aliased
destination/depth/texture objects, so `DRM_EXEC_IGNORE_DUPLICATES` is not
needed.

`dma_resv_wait_timeout()` returns positive remaining jiffies on success, zero
on timeout, and a negative errno when interrupted.  Keep the current Chrome9
translation (`0 -> -ETIMEDOUT`, negative unchanged) and the bounded
`CHROME9_DEP_TIMEOUT`.  Its `dma_resv_usage_rw(true)` argument is deliberately
the conservative write case.

Remove the four pre-`drm_exec` calls to
`drm_gem_shmem_get_pages_sgt()` and move all PTE collection after the existing
per-object `dma_resv_wait_timeout()` calls.  With every reservation still
held, get roles in this order:

```text
destination, depth, texture2 if present, texture3 if present
```

On any failure, put prior roles in reverse order before `drm_exec_fini()`.
After all gets succeed, call `dma_sync_sgtable_for_cpu()` for every role while
the reservations remain held, then collect PTEs from the role-owned SGTs and
continue with the current vmaps.  This is required even when the mapping
already existed for direct scanout.

Preserve the current conservative waits using
`dma_resv_usage_rw(true)`.  This waits existing readers and writers and is
safe for mixed destination/depth/texture roles.  Narrowing read-only texture
waits to `dma_resv_usage_rw(false)` is a separate optimization and should not
be combined with the lifetime patch.

Normal cleanup already reaches `out_unmap` with all reservations held.  After
CPU/device synchronization, scanout resync, and vunmaps, put acquired DMA
roles in exact reverse order, clear the four SGT pointers, then execute
`drm_exec_fini()`.

The current quarantine branch calls `drm_exec_fini()` and retains the job.
It must continue to skip all DMA puts.  `job->sgt`, `depth_sgt`, and texture
SGTs then represent permanent quarantine roles and remain reachable from the
quarantine job.  Engine teardown already refuses to proceed with this list
nonempty.

No DMA-role put is legal merely because packet building failed if SF PTE
cleanup also failed.  That branch becomes quarantine exactly as it does now.

## Direct scanout conversion

`chrome9_sf_pin()` must use the same per-object role rather than the cached
shmem SGT:

1. allocate the mapping and take its existing GEM object reference;
2. lock `obj->resv` interruptibly;
3. call `chrome9_gem_dma_get_locked(shmem)` and store the returned pointer;
4. unlock the reservation;
5. collect PTEs and bind them using the existing `submit_lock -> vm_lock`
   order.

Atomic `prepare_fb` stashes the incoming producer fence and the atomic helper
waits it before pipe enable/update.  The first DMA map can therefore happen
before that fence signals, as is normal for a prepared attachment.  The
scanout get path must not sync for CPU.  `chrome9_gem_fb_sync_for_device()`
must remain in enable/update after the atomic fence wait: its device sync
refreshes any SWIOTLB bounce contents before IGA fetches them.  Do not remove
that sync.

Every safe `chrome9_sf_unpin()` path first invalidates SF PTEs under the
existing engine locks.  After those locks are released, take `obj->resv`, put
the DMA role, unlock, then drop the mapping's GEM reference.  Bind cleanup
failures, wedges, or stopping-engine paths quarantine the mapping and must
skip the DMA put.

If the same object is both scanout and a draw operand, its count is at least
two.  Job cleanup drops only the job role; the SGT remains mapped for scanout.
The existing retirement sequence which syncs for CPU inspection and then
calls `chrome9_sf_resync_object_locked()` must run before the job put.

## Ownership and ordering invariants

The minimal patch must preserve all of these:

1. `obj->resv` protects mapping creation, destruction, and use-count changes.
2. No call to public `drm_gem_shmem_get_pages_sgt()` is made while the
   reservation is held; it would try to lock the same reservation.
3. PTE collection happens only while a DMA role owns a stable SGT.
4. After its producer wait, every draw explicitly returns the map to CPU
   ownership before draw-side guard writes; scanout prepare does not.
5. Every GPU publication is preceded by `dma_sync_sgtable_for_device()`.
6. Every CPU read/verification after retirement is preceded by
   `dma_sync_sgtable_for_cpu()`.
7. An active scanout is returned to device ownership after job-side CPU
   inspection.
8. Last-role unmap occurs only after all SF PTEs are invalid and all command or
   display fetches are disproven.
9. Quarantine retains the mapping, object, SF range, and ring reachability
   until reboot.
10. No role count or SGT pointer survives normal object free.

The important lock orders are unchanged:

```text
draw: drm_exec/object resv -> submit_lock -> vm_lock
scanout bind/unbind: submit_lock -> vm_lock, then object resv after release
```

Never acquire `obj->resv` while holding `submit_lock` or `vm_lock`; doing the
scanout role put after those locks avoids a new inversion with draw submission.

## Instrumentation

Add device atomic counters, not a new userspace ABI:

```text
dma_maps, dma_unmaps, dma_map_failures, dma_roles_live,
dma_roles_high_water, dma_bytes_live, dma_bytes_high_water,
dma_quarantine_roles
```

Count bytes once per mapped object, not once per role.  Expose them in DRM
debugfs or at least the existing unload summary.  `dma_maps - dma_unmaps`
must equal the number of currently mapped objects; `dma_roles_live` may be
higher when draw and scanout share one object.

## Fault and regression tests

### Build/static

* Build both KMS and render-only configurations with `W=1`.
* Run with lockdep and `CONFIG_DEBUG_WW_MUTEX_SLOWPATH` to exercise drm-exec
  contention retries.
* Run with `CONFIG_DMA_API_DEBUG`; there must be no direction, attrs, nents, or
  double-unmap warnings.
* Assert at compile time that `offsetof(struct chrome9_gem_object, shmem)==0`.

### Prepublication failures

Use a temporary internal fault-injection counter around the three mapping
steps; remove it or leave it read-only/debug-only before production.

1. Fail pin: no SG allocation, role, PTE, or ring publication.
2. Fail SG allocation: pin is balanced by unpin.
3. Fail `dma_map_sgtable`: SG table is freed and pin is balanced.
4. Fail destination/depth/texture2/texture3 successively in a four-object
   request.  Earlier roles unwind in reverse and all counters return to
   baseline.
5. Fail PTE allocation/collection after all maps.  All roles unwind; no GART
   entry is published.
6. Exhaust SWIOTLB before first use.  The draw returns the DMA mapping errno,
   engine status remains idle, and the object stays usable by CPU.

### Lifetime and sharing

1. Create and mmap 128 MiB of Chrome9 GEM without drawing.  SWIOTLB use and
   `dma_maps` remain unchanged.
2. Draw to one BO repeatedly.  With no scanout role, every synchronous ioctl
   finishes with zero live roles/maps; map/unmap totals match.
3. Pin the BO for scanout, draw to it repeatedly, then disable/unpin.  One map
   persists during scanout, job roles return to one, and the final unpin
   returns to zero.
4. Export/import the same native BO across Chrome9 drm files.  Both handles
   address the same subclass object and share one `dma_sgt`; destroying either
   handle does not affect the active role.
5. Repeat Xorg pixmap churn beyond 64 MiB aggregate with a small live set.
   `io_tlb_used` tracks only live draw/scanout roles and returns near baseline
   while Xorg remains running.

### Safety paths

1. Force a ring timeout after publication.  Map/role counters remain elevated,
   quarantine counters rise, unload refuses teardown, and no DMA unmap occurs.
2. Force SF PTE invalidation failure on job cleanup and on scanout cleanup.
   Both retain their mappings and object references.
3. Exercise draw/scanout overlap while alternating CPU writes.  Pixel and
   guard checks must prove the CPU/device sync sequence.
4. Run all current fixed, color, blend, texture, D16, mip, two-texture, KMS,
   unload/reload, and 1,000-submit replacement tests.

The first live acceptance gate is simple and strong: with Xorg kept alive,
unused and destroyed pixmaps no longer cause monotonically increasing
`io_tlb_used`; a deliberately quarantined job still does.
