# Chrome9 zero-copy KMS mapping and lifetime design

This note turns the proven Chrome9 SF/GART machinery into a bounded KMS
implementation contract.  The first goal is one native Chrome9 GEM shmem
object which is rendered by 3D and scanned out by IGA1 through a persistent SF
mapping.  There is no PRIME/offload dependency, no second GPU, and no BAR2
copy.  Page flips remain disabled until the separate IGA1 vblank/latch oracle
has passed.

The register-level address and flip evidence is in
`zero-copy-sf-scanout.md` and `iga1-vblank-flip-oracle.md`.  This note focuses
on the object, address-space, atomic, and failure lifetimes which must surround
those writes.

## Non-negotiable invariants

1. SF virtual address zero through `0x0000ffff` belongs permanently to the
   64-KiB command ring.  The one 64-MiB `drm_mm` owns the remaining interval
   `[0x00010000, 0x04000000)`.  Persistent scanout nodes and transient render
   nodes are allocated from this same allocator, never from parallel range
   counters.
2. A scanout mapping covers the complete GEM allocation, including Mesa's
   leading and trailing guard pages.  IGA1 receives
   `mapping.node.start + fb->offsets[0]`, not the node start and not a DMA
   address.
3. Every native WB shmem page uses the live-proven PTE form
   `((dma >> 12) & 0x3fffffff) | BIT(25)`.  Addresses and SG lengths are
   page-aligned and below 4 GiB.  A plain PFN is not an acceptable substitute.
4. All `drm_mm` and GART mutations take `engine->submit_lock` and then
   `engine->vm_lock`, in that order.  A display path must not introduce the
   reverse order.
5. A node is removed only after every corresponding PTE has been replaced by
   `0x80000000` and the bounded SR6F translation flush has completed.  Failure
   after PTE publication quarantines the mapping, GEM reference, pages, node,
   and device/module lifetime through reboot.
6. `prepare_fb` never programs a display register.  It only establishes the
   mapping and snapshots render dependencies.  IGA base programming happens
   after atomic helpers have waited for the incoming fence.
7. A mapping is not reclaimable merely because atomic cleanup ran.  It is
   reclaimable only when it has no prepared-state references and hardware can
   no longer fetch it.  Confirmed base latch transfers display ownership;
   confirmed CRTC disable removes it.  Timeout or ambiguity retains it.
8. Hard IRQ code acknowledges/classifies the latch and transfers references;
   it never takes `submit_lock`, sleeps, edits GART PTEs, or removes a
   `drm_mm_node`.

These rules intentionally make address-space exhaustion recoverable while an
incorrect unmap could become system-memory corruption.

## Existing address-space facts

`chrome9_engine.c` currently defines a 64-KiB GART table with 16,384 32-bit
entries, hence a 64-MiB SF aperture.  `drm_mm_init()` starts allocation at
`CHROME9_RING_SIZE == 0x10000`; entries 0--15 are the persistent ring.  Current
render ioctls allocate a page-aligned low node from the same `sf_mm`, bind a
contiguous PTE image, submit synchronously, invalidate the image, and remove
the node.  A live framebuffer node simply remains allocated across jobs.  The
allocator prevents overlap without reserving a second aperture or moving the
ring.

At 1024x768 XRGB8888, Mesa's allocation is:

```text
object byte 000000..000fff       leading guard
object byte 001000..300fff       visible image, pitch 4096
object byte 301000..301fff       trailing guard
AddFB2 offset                    00001000
SF scanout address               node.start + 00001000
mapping size                     00302000 bytes, 770 pages
```

Two such persistent front buffers consume about 6 MiB and coexist with the
ring plus transient render nodes.  Large submissions must return `-ENOSPC`
when fragmentation or persistent scanout ownership leaves no contiguous SF
range; they must not evict an active mapping.

The first implementation may render through a transient alias while the same
object has a persistent scanout alias.  Both aliases name the same PFNs with
the same bit-25 attribute, so this is still zero-copy.  It is also a property
which must be tested explicitly.  If alias coherency fails, the production
follow-up is one object-level `chrome9_sf_mapping` reused directly by render
jobs; guessing a cache packet or silently copying through BAR2 is not an
acceptable fix.

## Data ownership

The least invasive implementation is a private framebuffer plus a mapping
object which can outlive the framebuffer if quarantined:

```c
struct chrome9_sf_mapping {
        struct list_head quarantine_link;
        struct drm_mm_node node;
        struct drm_gem_object *obj;       /* held while bound/quarantined */
        struct sg_table *sgt;             /* shmem-cached, valid via obj */
        u32 *ptes;
        u32 page_count;
        u32 prepare_refs;                 /* successful prepare_fb calls */
        u32 display_refs;                 /* active or pending IGA fetch */
        bool node_allocated;
        bool ptes_bound;
        bool quarantined;
};

struct chrome9_framebuffer {
        struct drm_framebuffer base;
        struct mutex sf_lock;
        struct chrome9_sf_mapping *sf;
};
```

The counters are modified under `sf_lock`; use checked increments and refuse
overflow.  (A Linux `refcount_t` cannot be used as a zero-based counter without
an additional owning reference.)  They may instead be one lifetime reference
count plus explicit
`active` and `pending` ownership, but the two meanings must remain visible in
review and diagnostics.  In particular, `cleanup_fb(old_state)` is not proof
that IGA stopped fetching the old mapping.

Use `drm_gem_fb_init_with_funcs()` to initialize the subclass.  Its custom
framebuffer functions use `drm_gem_fb_create_handle()` and a custom destroy
callback.  The generic `drm_gem_fb_destroy()` cannot be the destroy callback
because it frees the enclosing allocation and knows nothing about a retained
mapping.  Normal destroy asserts zero prepared/display references, performs
any final safe unbind, puts the GEM objects, calls `drm_framebuffer_cleanup()`,
and frees the wrapper.  A quarantined mapping is instead anchored on an engine
quarantine list and owns its own GEM, module, and device references; no dangling
pointer to the freed framebuffer is permitted.

`drm_gem_shmem_get_pages_sgt()` creates and caches the native object's pinned,
DMA-mapped SG table.  It has no per-call matching put; shmem object release
normally unmaps and releases it.  Therefore the mapping's GEM reference is the
pin lifetime.  Do not manually free the cached `sgt` from framebuffer cleanup.

An object-level mapping is cleaner once render jobs can reuse it:

```c
struct chrome9_gem_object {
        struct drm_gem_shmem_object shmem;
        struct mutex sf_lock;
        struct chrome9_sf_mapping *sf;
};
```

That phase requires a driver `gem_create_object` hook and a custom shmem free
path, and must account for current multi-object render jobs which deliberately
place destination, depth, and textures into one contiguous transient node.  It
is not necessary for the first static scanout oracle.

## AddFB2 and page-offset validation

The guarded Mesa resource exports a KMS handle, stride, offset `PAGE_SIZE`, and
the linear modifier.  The matching KMS request must use modifier-aware AddFB2.
The kernel accepts exactly one native same-device, non-imported object, one
plane, XRGB8888, and `DRM_FORMAT_MOD_LINEAR` for the first oracle.  Retain the
fixed 1024x768 and 4096-byte-pitch restrictions until direct SF scanout is
proven.

Reject unknown AddFB2 flag bits, nonzero unused handles/pitches/offsets, and
legacy offset-zero layout for the guarded oracle.  Require:

```text
offset == PAGE_SIZE
offset and object size are page aligned
pitch == 4096
visible_end = offset + pitch * (height - 1) + width * 4
visible_end <= obj->size
guarded_end = offset + pitch * height + PAGE_SIZE
guarded_end <= obj->size
obj->size <= 0x04000000 - 0x00010000
```

All additions and multiplications are overflow-checked in `u64` before
comparison.  `drm_gem_fb_init_with_funcs()` independently checks the exact
minimum visible size formula `(height - 1) * pitch + min_pitch + offset`; the
driver's stronger trailing-guard check is intentional.  Do not use
`cdev->scanout_size` or the BAR2 carveout as a zero-copy size limit.

When the persistent node is allocated, additionally prove:

```text
node.start >= 0x00010000
node.start is page aligned
node.start + obj->size <= 0x04000000
scanout_base = node.start + offset
scanout_base is even
scanout_base + pitch * height <= node.start + obj->size
```

CR48 selects SF, and CR0D/CR0C/CR34/CR48 or packed MMIO `0x0214` receive
`scanout_base >> 1` as already documented.  No page offset is added twice.

## Binding transaction

`chrome9_sf_mapping_get()` is the only entry which may change an unbound
mapping to bound:

1. Lock the framebuffer/object mapping mutex and reuse an already bound
   mapping by incrementing its prepared reference.
2. Take a GEM reference, obtain the native shmem SG table, allocate the PTE
   array, and run the same strict SG-to-PTE checks as
   `chrome9_collect_sgt_ptes()`.
3. Take `submit_lock`, reject `stopping`, `wedged`, or `!ready`, then take
   `vm_lock`.
4. Insert a page-aligned `obj->size` node with `DRM_MM_INSERT_LOW` and bind all
   pages using `chrome9_gart_set()`.
5. Only after the final PTE readback, write barrier, and successful SR6F flush
   mark `ptes_bound=true` and publish the mapping to prepare/display code.
6. Unlock `vm_lock`, `submit_lock`, then the mapping mutex.

If node insertion fails, remove no PTEs and unwind normally.  If bind fails,
attempt a full invalidation while both engine locks remain ordered.  A
successful invalidation permits node removal and normal unwind.  Failed
invalidation is ambiguous publication: mark the engine wedged, quarantine the
node/PTE array/GEM reference, and return an error without freeing any of them.
This mirrors the existing render-job containment rule.

Unbind is the inverse and is legal only with zero prepared and display
references.  Under `submit_lock` then `vm_lock`, write invalid PTEs, complete
the GTI flush, set `ptes_bound=false`, and only then remove the node.  An
invalidation timeout quarantines rather than continuing to removal.

## Atomic prepare and cleanup

Add `prepare_fb` and `cleanup_fb` to `chrome9_pipe_funcs`.  Because the simple
pipe helper stops installing its default as soon as a driver callback exists,
the custom prepare **must** call `drm_gem_plane_helper_prepare_fb()` itself.
That helper snapshots the incoming explicit fence, or otherwise the BO's
write reservation fence, into `plane_state->fence`.  It deliberately does not
wait; atomic helper code waits at the correct blocking/nonblocking point.

The safe prepare order is:

```text
if no new fb: return 0
validate native framebuffer wrapper and unchanged guarded layout
call drm_gem_plane_helper_prepare_fb(pipe_plane, new_state)
bind/get mapping and increment prepare_refs
on success: return 0
on mapping failure: undo this callback's partial mapping/ref and put/clear
                    new_state->fence before returning the error
```

`drm_atomic_helper_prepare_planes()` calls cleanup only for planes whose
prepare completed before the failing plane.  It does not call cleanup for the
callback which itself returns failure, so that callback must fully unwind its
own partial work.  After a successful overall prepare, both commit abort via
`drm_atomic_helper_unprepare_planes()` and successful commit via
`drm_atomic_helper_cleanup_planes()` provide a matching cleanup call.

`cleanup_fb(state)` drops exactly the prepare reference belonging to
`state->fb`.  It starts an unbind only if the mapping then has neither prepare
nor display ownership.  This produces the following useful overlap:

| transition | prepare result | hardware/display ownership | cleanup result |
|---|---|---|---|
| initial enable | new mapped | new becomes active | old is null; new stays |
| flip A to B | B mapped while A valid | pending holds A and B until latch | old-state cleanup cannot free A early |
| abort before programming | new mapping prepared | active mapping unchanged | unprepare drops new safely |
| disable | no new mapping | disable/restore must first stop fetch | old cleanup may unbind only after display ref drops |
| unrelated commit, same fb | prepare ref increments | active ref unchanged | old cleanup balances ref; mapping stays |

Do not wait for render fences while holding `submit_lock`, `vm_lock`, the VGA
legacy-I/O lock, or the display state lock.  Atomic helpers wait before calling
the hardware update.  Immediately before publishing the new display base,
call `dma_sync_sgtable_for_device(..., DMA_BIDIRECTIONAL)` on the now-retired
incoming object.  The existing synchronous render ioctl currently performs a
CPU sync at retirement; display is another device consumer and must reacquire
device ownership before scanout.

The first production contract is render into an inactive BO and then commit a
flip.  Rendering into the unchanged active front buffer has no new atomic
prepare/update point at which to perform that ownership transition and can
tear; keep that path unsupported until a defined frontbuffer-flush/dirty
mechanism exists.

## Initial enable and static-scanout milestone

The first code milestone deliberately implements only a modeset/static
scanout:

1. AddFB2 creates the private framebuffer and validates the guarded offset.
2. Atomic prepare establishes its complete persistent SF mapping and captures
   the reservation fence.
3. The helper waits for the fence without any engine/display lock held.
4. `pipe_enable` confirms the prepared mapping, performs the device DMA sync,
   obtains VGA ownership, saves IGA1, programs pitch and
   `mapping.node.start + PAGE_SIZE`, selects CR48 SF, and unblanks.
5. The KMS state takes the active display reference before publishing the
   base.  It retains that reference until a complete IGA1 restore/disable.
6. `chrome9_pipe_check` rejects a framebuffer-base change with
   `-EOPNOTSUPP` while vblank/latch support is absent.  `pipe_update` never
   copies pixels.

An enable callback is `void`, so all operations which can normally fail--GEM
validation, SF allocation/bind, and fence capture--belong in fb creation,
atomic check, or prepare.  The only remaining enable failures are hardware
ownership/programming failures; on one of those, leave the mapping prepared,
restore any partially touched IGA state if possible, and fail closed rather
than selecting BAR2.  Drop the tentative active display reference only when
the saved display state is confirmed restored; ambiguous partial programming
retains and quarantines it.

On disable, disarm T11 automatic swapping, complete the existing full IGA1
restore/disable sequence, and only after that succeeds drop the active display
reference.  SR01 blanking alone is not a proven fetch-stop condition.  Failure
to acquire VGA ownership or to restore the display retains the mapping and
quarantines the display path.

## Page flip, vblank, and fencing milestone

Enable page flips only after IRQ cadence and direct-base latch are independently
proven.  One serialized pending flip contains references to both mappings,
their expected SF base, the arm vblank sequence, and the DRM event:

```c
struct chrome9_pending_flip {
        struct chrome9_sf_mapping *old;
        struct chrome9_sf_mapping *new;
        u32 expected_0214;
        u64 arm_sequence;
        struct drm_pending_vblank_event *event;
};
```

After the incoming fence wait and DMA device sync, `pipe_update` refuses a
second pending flip, takes display references to both sides, clears/disarms the
T11 auto-swap path, selects by-frame CR32 bit 0, records pending ownership
before the register write, writes the new packed `0x0214`, and flush-reads it.
The old active reference is not dropped here.

The IRQ handler recognizes `0x0200.bit3`, acknowledges only that RW1C source
with the driver enable shadow, calls `drm_crtc_handle_vblank()`, and tests:

```text
the interrupt sequence is after arm
and (MMIO 0x0204 & BIT(1)) == 0
and (MMIO 0x0214 & 0x1fffffff) == expected_0214
```

On success it sends the event, makes new active, and queues deferred release
of the old display reference.  On a first-vblank miss it keeps both mappings
and retries for a bounded number of real vblanks.  On timeout, lost IRQ, or
base mismatch it sends no successful event, disables further flips, and
retains both mappings pending a confirmed CRTC disable.

The default commit tail waits one vblank before old-state `cleanup_fb`, but the
documented `0x0204` predicate may remain pending longer.  Therefore prepared
references and display references must be independent; generic cleanup is not
the hardware latch completion.  If nonblocking commit tracking is adopted,
the driver may use `drm_atomic_helper_wait_for_flip_done()`, but that still
does not replace the mapping reference rule.

Current render ioctls synchronously retire and install an exclusive
`dma_resv` write fence.  The custom prepare preserves that dependency through
`drm_gem_plane_helper_prepare_fb()`.  A future asynchronous render path must
continue publishing its write fence before returning.  Display completion is
not a render-completion fence: it controls when the old mapping may be
unbound, while the incoming render fence controls when the new mapping may be
presented.

## Teardown ordering

Normal driver removal is ordered as follows:

1. Stop new atomic commits and render submissions.
2. Disable vblank sources and `synchronize_irq()` without dropping active or
   pending mapping references.
3. Disarm flips and perform a complete IGA1 disable/restore.
4. After confirmed fetch stop, cancel pending events as failed and drop all
   display references.
5. Run deferred mapping release: invalidate persistent scanout PTEs, flush
   GTI, and remove their nodes.
6. Quiesce the command reader, invalidate ring entries, and restore the saved
   GART only when both the render and display quarantine lists are empty.
7. Tear down `drm_mm`, DMA memory, and MMIO last.

If any render job or scanout mapping is quarantined, the current reboot-scoped
policy wins: do not restore the table over potentially active translations,
do not free backing pages, and retain module/device ownership.  KMS cleanup
must check the same global condition as engine cleanup rather than assuming
its own list is sufficient.

## Incremental no-wedge validation

Each stage is a separate boot and must leave the previous render/2D regression
green.  Stop at the first mismatch; do not add a compensating packet or reset.

1. **Software-only validation.** Exercise AddFB2 accepts exactly XRGB8888,
   linear, pitch 4096, offset 4096, and the full two-guard object.  Test
   overflow, short object, imported handle, offset zero, non-linear modifier,
   and unused-plane garbage rejection.  No GART or display write occurs.
2. **Bind/unbind without display.** Prepare then abort one framebuffer.  Log
   node `[start,end)`, first/last PTE and page count; read back PTEs, complete
   SR6F flush, invalidate all 770 entries, flush again, then prove the node can
   be reallocated.  Run the existing Gallium, D16, texture, blend, and 2D
   suite afterward.
3. **Reservation coexistence.** Keep two 770-page mappings prepared while
   repeatedly running transient render jobs.  Assert all intervals are
   disjoint and ring entries 0--15 never change.  Force `-ENOSPC` with a safe
   oversized allocation and prove it causes no PTE or allocator mutation.
4. **Static SF scanout.** Render a high-visibility pattern into one guarded
   BO, retire its reservation fence, device-sync it, and enable fixed IGA1 at
   `node.start + 0x1000`.  Capture CR0C/0D/34/48, `0x0214`, SR6A/B/C/F/7B,
   first/last PTEs, and visible output.  Do not flip or unbind while enabled.
5. **Static disable.** Fully restore/disable IGA1, then invalidate/unbind.
   Verify both guards, read back invalid PTEs, and rerun render/2D.  Inject a
   pre-publication allocation failure to prove normal unwind and a simulated
   unbind failure to prove quarantine retains node/object rather than freeing
   them.
6. **Alias coherency.** Keep the persistent display mapping bound, render a
   new pattern through the normal transient alias while IGA is disabled, then
   re-enable the persistent address after fence retirement/device sync.  The
   complete image must change with intact guards.  Partial/stale output blocks
   alias-based production and triggers the object-level shared mapping work.
7. **Read-only vblank IRQ.** With static scanout retained, enable the exact
   `0x0200` shadow/ack path and prove stable bit-3 cadence plus DRM sequence
   advancement.  No base write and no mapping release occurs.
8. **Retained direct flips.** Bind two conspicuous BOs, alternate `0x0214`, and
   validate the exact post-arm IRQ + `0x0204.bit1==0` + base-match predicate.
   Retain both mappings for the whole run even after events.
9. **Post-latch release.** Repeat flips while releasing only the old display
   reference in deferred context after the proven predicate.  Verify invalid
   PTE readback and both guards every iteration.  Exercise flip timeout by
   suppressing completion in software and prove both mappings stay bound.
10. **Userspace presentation.** Only after stages 1--9, allow modesetting/glamor
    double buffering.  Trace GEM handle, AddFB2 offset/modifier, SF node,
    reservation fence, arm sequence, latch values, event, and old-node release
    as one correlated record.  There is still no BAR2 fallback.

## Old-driver cross-check

The supplied proprietary Linux `via_chrome9_dma.c` establishes one global
RTSF table, initializes every entry invalid, and binds the complete SG
allocation into a fixed translated range before flushing SR6F and restoring
SR6C write protection.  This independently supports stable long-lived
mappings rather than per-scanline remapping.  Its raw PFN entries describe its
old noncached allocation and must not override the modern, live-proven bit-25
WB-shmem rule.

The old upstream IRQ definitions and Windows KMD independently identify IGA1
vblank enable/status and the direct-base latch described in
`iga1-vblank-flip-oracle.md`.  Old unichrome Mesa likewise retained VRAM
surface offsets for the buffer lifetime.  None of those drivers supplies a
modern atomic lifetime model; the prepared/display split above is the missing
memory-safety contract.
