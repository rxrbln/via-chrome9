# Chrome9 dma-buf handles and SWIOTLB lifetime

This note separates two lifetimes which currently reinforce one another in an
Xorg/DRI3 workload:

1. a userspace GEM handle returned by `PRIME_FD_TO_HANDLE`; and
2. Chrome9's device DMA mapping cached in `drm_gem_shmem_object::sgt`.

A leaked receiving-file handle can keep the GEM object alive.  The current
Chrome9 GEM-create path eagerly creates the cached DMA mapping, so that object
also keeps 32-bit SWIOTLB slots occupied.  Fixing only one side reduces the
symptom, but the production design needs bounded ownership on both sides.

The conclusions below apply to the 2026-09-21 source snapshots:

* Chrome9 kernel `f39a90d`;
* Chrome9 Gallium `98ab258`; and
* Linux 7.2 DRM/shmem helpers in the matching T2 tree.

## Live failure signature

The VX900 host reports `chrome9 dma_bits=32`.  Its default SWIOTLB has 32,768
slots.  Linux defines `IO_TLB_SHIFT=11`, so each slot is 2 KiB and the whole
pool is 64 MiB.

During the long-lived Xorg import test, kernel messages repeatedly recorded
Chrome9 mapping failures.  The common requested size was 262,144 bytes, with
other failed mappings from 69,632 through 258,048 bytes.  Representative
high-water samples were:

```text
used 32174 / 32768, request 102400
used 32344 / 32768, request  73728
used 32379 / 32768, request 262144
893 and 1050 mapping callbacks suppressed
```

After earlier Xorg runs exited, `io_tlb_used` returned to roughly 85--200
slots.  With the current Xorg process still alive it read 5,207 slots and Xorg
held four `/dev/dri/card0` descriptors.  This is strong evidence for
lifetime-bound mappings, not an allocator pool permanently lost inside
SWIOTLB.  Increasing `swiotlb=` can postpone the failure but does not repair
the lifetime defect.

## Exact PRIME identity and handle rules

### Same device is the original object

`drm_gem_shmem_prime_import_no_map()` at
`drivers/gpu/drm/drm_gem_shmem_helper.c:934..980` first calls
`drm_gem_is_prime_exported_dma_buf()`.  For a dma-buf exported by the same
Chrome9 `drm_device`, it returns `dma_buf->priv`, the original GEM object,
after taking an object reference.  It does not create an import proxy, attach
the dma-buf, or set `obj->import_attach`.

Therefore the normal same-device DRI3 path has one GEM object and reservation
object but separate per-`drm_file` handles:

```text
client drm_file: handle A --+             +-- Xorg drm_file: handle B
                            +-- GEM object -- dma_resv
                                  |
                               dma-buf fd
```

`drm_gem_is_imported(obj)` remains false.  This is why Chrome9 can reject true
foreign-device import proxies while accepting a same-Chrome9 dma-buf.

### The kernel PRIME cache is not a handle reference count

`drm_gem_prime_fd_to_handle()` at `drm_prime.c:292..360` holds the receiving
file's PRIME mutex and looks up the dma-buf in its per-file rb-tree.  On a
cache hit it returns the existing handle.  It does **not** create a second
GEM-handle reference.  On a miss it imports the object, creates one handle,
and records the `(dma_buf, handle)` pair.

`GEM_CLOSE` removes that pair through `drm_prime_remove_buf_handle()` before
dropping the object's handle reference (`drm_gem.c:365..422`).  Consequently,
two userspace wrappers which received the same numeric handle cannot each
close it.  There is exactly one kernel handle to close.

The installed libdrm header states this contract explicitly at
`xf86drm.h:815..833`: handles are not reference-counted, a caller must maintain
one userspace reference-count manager, and two independent managers must not
operate on the same DRM file description.  `dup()`/`F_DUPFD_CLOEXEC` does not
make an independent handle namespace; it duplicates the same open file
description and hence the same kernel `drm_file`.

## Gallium handle-manager audit

Chrome9 Gallium commit `98ab258` implements the required screen-local manager:

* `chrome9_screen_bo_import_fd()` serializes `drmPrimeFDToHandle`, lookup, and
  insertion with `shared_lock`;
* entries are keyed by the returned handle and contain wrapper references plus
  one `owns_handle` bit;
* a cache miss owns the one handle returned by the import ioctl;
* `chrome9_screen_bo_export_fd()` moves a native BO into the table before an
  alias can be returned through this implementation; and
* `chrome9_screen_bo_destroy()` closes the handle only when the entry's last
  wrapper disappears.

The lazy registration on first export is valid.  An unexported local BO cannot
have a PRIME alias; export and registration happen under the same screen lock.
Registering every allocation eagerly is unnecessary overhead.

The implementation must retain these invariants:

1. The table identity is `(screen/open-file-description, GEM handle)`, never a
   dma-buf fd number.  fd integers are reusable.
2. `PRIME_FD_TO_HANDLE` and table lookup/insertion stay in one critical
   section.  The kernel deliberately collapses concurrent aliases.
3. A miss owns one handle; a hit owns no additional handle.  Exactly one
   `GEM_CLOSE` occurs at entry reference zero.
4. A local BO's `handle_owned` transfers to the table on first export.  Its
   ordinary destructor must no longer close that handle independently.
5. Every failure after a miss either installs a table entry or closes the new
   handle.  It must also undo mmap and wrapper state.
6. Every returned dma-buf fd is independently closed by its receiver.  The fd
   lifetime is not the table reference count.
7. Screen destruction should find the table empty.  Forced cleanup is useful
   as a last-resort release, but a debug warning/counter is needed because a
   nonempty table means Gallium resources outlived their screen.

The irreducible boundary is shared-fd ownership.  `chrome9_screen_create()`
duplicates the loader fd with `F_DUPFD_CLOEXEC`; this is the same open file
description.  Chrome9's table is safe only if it is the sole PRIME handle
manager for handles it creates/imports there.  EGL, GBM, the loader, or another
library must not independently import/export on that same file description.
If that contract cannot be guaranteed, use a separately opened DRM fd and
explicitly translate any KMS-facing handle; simply duplicating the fd is not a
solution.

Recommended diagnostic counters on `chrome9_screen` are:

```text
shared_entries_live, shared_wrappers_live, import_misses, import_alias_hits,
gem_closes, shared_entries_high_water, nonempty_entries_at_screen_destroy
```

These identify a userspace lifetime leak without requiring GPU execution.

## Why handle cleanup alone is insufficient

`chrome9_gem_create_ioctl()` at `chrome9_gem.c:296..326` currently calls
`drm_gem_shmem_get_pages_sgt()` before returning every newly created handle.
That helper:

1. gets one `pages_use_count` reference;
2. creates an SG table from the pages;
3. calls `dma_map_sgtable(..., DMA_BIDIRECTIONAL)` for the Chrome9 device; and
4. caches the mapped table in `shmem->sgt`.

Every native BO therefore consumes any required 32-bit bounce space at
creation, even if it is never submitted or scanned out.  Every engine and KMS
consumer then reuses that raw cached pointer:

* one-object jobs: `chrome9_engine.c:2465`;
* depth/two-texture jobs: `:2804..2826`;
* direct SF scanout: `:1828..1867`; and
* diagnostic shmem SF allocation: `chrome9_drv.c:1100..1155`.

The cache is released only with the GEM object today.  A receiving Xorg handle
which remains in its file's handle table therefore extends the DMA mapping and
SWIOTLB allocation to the X server's lifetime.

True foreign-device imports are different.  Chrome9 uses
`drm_gem_shmem_prime_import_no_map()`, so a foreign proxy starts without a
Chrome9 SG mapping, and current rendering/scanout rejects it.  Same-device
round trips are the original native object and therefore retain its cached
Chrome9 mapping.

External dma-buf attachment mappings are also separate from `shmem->sgt`.
Freeing Chrome9's own cached mapping must not pretend to release mappings owned
by another device's attachment.

## Correct shmem release primitive

The Linux helper has no public refcount for borrowers of `shmem->sgt`.
`drm_gem_shmem_get_pages_sgt()` returns the cached pointer on every later call.
Chrome9 must provide its own borrower/lifetime discipline before releasing it.

Once no Chrome9 hardware borrower can reach the mapping, release requires both
operations under the object's reservation lock:

```c
if (shmem->sgt) {
        __drm_gem_shmem_free_sgt_locked(shmem);
        drm_gem_shmem_put_pages_locked(shmem);
}
```

`__drm_gem_shmem_free_sgt_locked()` DMA-unmaps and frees only the cached SG
table.  `drm_gem_shmem_get_pages_sgt_locked()` took a pages-use reference when
it created that table, so `drm_gem_shmem_put_pages_locked()` must balance it.
Other VMA, vmap, pin, or dma-buf-attachment references remain independently
counted and keep the backing pages alive as needed.

Calling only `free_sgt` leaks the SGT's pages-use reference.  Calling only
`put_pages` can leave a DMA mapping which names released pages.  Calling either
while a job, SF PTE, or scanout still has the returned pointer is a use-after-
unmap and can corrupt memory.

That pair is correct if Chrome9 continues to use the shmem helper's cached
`shmem->sgt`, but it is not the preferred new implementation.  The public
`drm_gem_shmem_get_pages_sgt()` acquires `obj->resv` internally.  Calling it
before the authoritative reservation-fence wait can make SWIOTLB copy stale
CPU-visible data while an external producer is still writing; calling it with
the reservation already held deadlocks.  The comment in the current eager
GEM-create path explicitly exists to avoid that first-map-after-sharing case.

The safer role manager should own a separate SG table.  With `obj->resv` held
and producer fences waited, its first-map operation is:

```c
ret = drm_gem_shmem_pin_locked(shmem);
if (ret)
        return ret;
sgt = drm_gem_shmem_get_sg_table(shmem);
if (IS_ERR(sgt)) {
        drm_gem_shmem_unpin_locked(shmem);
        return PTR_ERR(sgt);
}
ret = dma_map_sgtable(dev, sgt, DMA_BIDIRECTIONAL, 0);
if (ret) {
        sg_free_table(sgt);
        kfree(sgt);
        drm_gem_shmem_unpin_locked(shmem);
        return ret;
}
```

The matching last-role release, also with `obj->resv` held, is
`dma_unmap_sgtable()`, `sg_free_table()`, `kfree()`, then
`drm_gem_shmem_unpin_locked()`.  The SG pointer lives in the Chrome9 role
record, not in `shmem->sgt`.  This uses exported shmem interfaces, gives the
driver an explicit mapping lifetime, and remains independent of VMA/vmap and
foreign-device dma-buf attachment mappings.

## Implementation-ready kernel ownership design

### Phase 1: stop eager mapping

Remove the eager `drm_gem_shmem_get_pages_sgt()` and initial
`dma_sync_sgtable_for_cpu()` from `chrome9_gem_create_ioctl()`.  GEM creation
should allocate an object and handle, not reserve bounce space.  Do this only
in the same series which converts every engine/KMS SGT borrower to the role
manager below; otherwise two consumers can race a lazy unmap.

An unused GEM allocation must then consume zero Chrome9 SWIOTLB slots.  Its
first hardware use can fail cleanly with `-ENOMEM` before publication if the
DMA mapping cannot be created.

### Phase 2: one Chrome9 DMA record per native object

Maintain an engine/device table keyed by `struct drm_gem_object *`.  Each
record owns a GEM object reference and contains at least:

```text
sgt pointer
state = UNMAPPED | MAPPING | MAPPED | UNMAPPING | QUARANTINED
job_refs, scanout_refs, quarantine_refs
mapped_bytes / DMA-segment count for diagnostics
per-record mutex (or equivalent waitable serialization)
```

Replace all Chrome9 calls to `drm_gem_shmem_get_pages_sgt()` with:

```text
chrome9_dma_get(obj, JOB | SCANOUT) -> role token
chrome9_dma_ensure_mapped_locked(token) -> stable sgt
chrome9_dma_put(token)
chrome9_dma_quarantine(token)
```

The first role creates the record but need not map immediately.  After
reservation fences have been waited, the first `ensure_mapped_locked` pins the
shmem pages, creates the role-record SG table, and DMA-maps it.  Further roles
reuse the same stable pointer.  The last non-quarantined role unmaps and
unpins.  The table entry is removed only after unmapping and dropping its GEM
reference.  A quarantine role is intentionally permanent until reboot because
hardware reachability is not disproven.

A custom Chrome9 GEM subclass is not required.  A device-owned hash/xarray can
hold these records, provided the object reference makes the pointer key stable
and lookup/removal is serialized.

### Role boundaries

Acquire and release at these exact semantic points:

| Role | Acquire | Safe release |
| --- | --- | --- |
| job destination/depth/texture | create role before `drm_exec`; map after reservations/fences are acquired, then collect PTEs | only after retirement is proven, all job SF PTEs are invalidated successfully, CPU/device sync is complete, and the record can reacquire its reservation |
| direct scanout | create role in atomic prepare; wait the producer and map under its reservation before collecting SF PTEs | cleanup/unpin only after the CRTC no longer fetches the old base and SF PTE invalidation succeeds |
| diagnostic persistent SF | engine initialization | after engine quiesce and GART restore prove it unreachable |
| quarantined job/scanout | when publication or unbind can no longer be disproven | reboot only |

An object can carry job and scanout roles simultaneously.  Job retirement
currently syncs its SGT for CPU guard inspection and then
`chrome9_sf_resync_object_locked()` returns an active scanout mapping to device
ownership.  Preserve that ordering before dropping the job role; the scanout
role remains and prevents unmapping.

No release is legal while any of the following is true:

* a submitted ring segment may still reference the SF range;
* SF PTE invalidation or its flush failed;
* a framebuffer/CRTC can still fetch the range;
* the device owns data which has not been returned/synchronized as required;
* a timeout, wedge, or teardown path quarantined the mapping; or
* another job/scanout role exists.

The current timeout quarantine and module-unload refusal are safety features,
not leaks to optimize away.  They must retain the DMA role as well as the GEM
object, PTE allocation, and command-ring reachability.

### Locking contract

No path may hold the role-table lock while waiting for `obj->resv`.  Use this
order consistently for submission:

1. create role tokens under the short-lived role-table lock, then drop it;
2. let `drm_exec` lock all object reservations and wait their fences;
3. with each reservation held, re-enter the table briefly, transition the
   record to `MAPPING`, and perform the driver-owned pin/SG/DMA-map operation;
4. collect PTEs, perform `dma_sync_*_for_device`, and publish the job;
5. retire, sync for CPU/scanout as required, and invalidate PTEs;
6. release `submit_lock`, `vm_lock`, and `drm_exec` locks;
7. put role tokens while GEM object references are still alive.  A last put
   reacquires `obj->resv`, rechecks that the role count is still zero, marks
   `UNMAPPING`, unmaps/unpins, removes the record, and wakes waiters.

Mapping/unmapping states must be waitable so another acquirer cannot observe
an SG table being destroyed.  A new acquire which meets `UNMAPPING` waits and
retries; it never resurrects or reads that pointer.  Operations which hold
`obj->resv` may take the short role-table lock, but the inverse order is
forbidden.  Do not expose either the record SGT or legacy `shmem->sgt` without
a role token.

For a multi-object request, unwind acquired roles in reverse order on every
prepublication failure.  A `dma_map_sgtable()` failure is prepublication:
return its errno with no SF PTE or ring mutation.  After publication, any
unproven retirement moves all referenced roles to quarantine.

## Old-driver cross-check and limitation

The proprietary Linux 2.6.31 source does not implement modern GEM, PRIME, or
DMA-API mapping lifetime.  `via_chrome9_drm_sg_alloc()` allocates one global
`dev->sg`, obtains pages from a noncached vmalloc region, and writes
`page_to_pfn()` directly into the GART.  `via_chrome9_cleanup()` frees that
single allocation at driver cleanup (`via_chrome9_drm.c:860..863`).

This supports the hardware fact that valid GART PTEs require the backing
allocation to remain alive, but it is not evidence that modern Chrome9 should
cache a DMA/SWIOTLB mapping for every GEM object.  The old code predates both
SWIOTLB-correct `dma_map_sgtable()` use and per-object sharing.

## Tests and acceptance criteria

### Handle manager, no hardware required

1. Export local resource A and import it twice through duplicate dma-buf fds.
   All wrappers must name one handle/table entry.  Destroy in every order; the
   final wrapper triggers exactly one `GEM_CLOSE`.
2. Destroy A before its imported alias B.  B must still export/import and map.
3. Import 1,000 distinct buffers, destroy each wrapper, and close each dma-buf
   fd.  `shared_entries_live`, process fd count, and kernel handle count must
   return to baseline without destroying the screen.
4. Randomly create aliases and destroy them from multiple threads.  No table
   duplicates, double-close, or reference overflow is allowed.
5. Inject failures after `PRIME_FD_TO_HANDLE`, during mmap, and during wrapper
   allocation.  A miss must close its new handle; a hit must not close the
   shared handle.
6. Repeat on two independently opened DRM files.  Each has its own table and
   owned handle.  Closing one must not invalidate the other.
7. Destroying a screen with live table entries must trip the diagnostic
   counter/warning.  Normal loader teardown must leave it zero.

### SWIOTLB and DMA-role tests, hardware required

Record `/sys/kernel/debug/swiotlb/io_tlb_used`, `io_tlb_used_hiwater`, and
`io_tlb_nslabs` before, during, and after each test.

1. Create many unused GEM objects.  After removing eager mapping, SWIOTLB use
   must remain at baseline.
2. Submit one object.  First use may raise the slot count; proven retirement
   and last-role release must return it near baseline while its GEM handle and
   userspace mmap remain alive.
3. Keep one object scanned out.  Its slots must remain occupied across render
   retirement and drop only after a proven flip/disable plus SF unpin.
4. Loop unique DRI3 pixmap import/destroy with an aggregate allocation larger
   than 64 MiB but a small live set.  Slot usage must track the live hardware
   set and never climb monotonically to 32,768.
5. Hold a known live set, release half, and verify a corresponding decrease.
   Use measured debugfs counts rather than assuming perfect size/2-KiB
   packing, because segment boundaries and fragmentation add overhead.
6. Alternate CPU writes, GPU rendering, and scanout on one object.  Verify
   pixels and guards to catch missing `dma_sync_*` transitions.
7. Run the complete color, texture, D16, two-texture, and KMS regression suite.
8. Force prepublication DMA-map failure.  The ioctl must return an allocation
   error with no PTE/ring publication and all earlier roles unwound.
9. Force a post-publication timeout or GART-unbind failure.  Slot use must stay
   elevated, the engine must wedge/quarantine, and unload must refuse unsafe
   teardown.  Reboot is the expected release boundary.
10. Confirm a foreign-device dma-buf remains rejected without creating a
    Chrome9 cached DMA mapping.

The release criterion is not merely absence of `swiotlb buffer is full` in a
short run.  With Xorg remaining alive, both the Gallium shared-handle entry
count and Chrome9's DMA-role/SWIOTLB usage must return to a stable baseline
after transient resources are destroyed.
