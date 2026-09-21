# Persistent Chrome9 draw submission performance plan

## Decision

The next performance tranche should combine **one native immediate-list
packet per immutable draw run** with the already proven persistent 64-KiB
command ring.  A CPU-only 32-KiB staging area is sufficient for 272
untextured D16 RHW triangles, 256 one-texture triangles, or 256 two-texture
triangles, including the command reader's exact `0x220`-byte read-ahead
closure.  This replaces the earlier eight-page-segment designs: it removes
seven redundant state uploads and, more importantly, seven synchronous
cookie/PAUSE retirements from a full run.

Persistent SF/GART mappings should be a separate second tranche.  Cache one
kernel-chosen mapping per GEM object, retain an object reference while it is
cached, and batch all changed PTE spans behind one VGA-arbitration acquisition
and one `SR6F` invalidation.  A cache hit then performs no PTE writes, VGA
arbitration, `SR6F` pulse, `drm_mm` operation, or PTE allocation.  CPU/device
ownership transitions and guard verification remain once per run: mmap writes
are not tracked, so skipping those across ioctls would be unsound.

Neither tranche changes the structured-command security boundary.  Userspace
still supplies only validated state and vertices; the kernel generates all
native words.  Neither tranche uses the unresolved vendor circular-ring
rewind, trusts MMIO `043c` as retirement, or continues after a timeout.

This plan is derived from the current T2 Chrome9 engine source under
`src.linux.../linux-7.2`, Mesa Chrome9 `bed597d`, the retained live traces,
and the three vendor implementations cited below.  It is an implementation
design, not a claim that a 256-triangle list has already run on silicon.

## Why the current path cannot reach 60 FPS reliably

The current synchronous path pays the following for every submission, even
when the next triangle uses the same objects and state:

1. GEM lookup/reference and SGT acquisition for every object;
2. a `chrome9_job` allocation and a page-count-sized PTE allocation;
3. one `drm_exec` reservation transaction and all dependency waits;
4. vmap of each object and one transient `drm_mm` SF allocation;
5. PTE publication through `chrome9_gart_set()`, which obtains VGA legacy-I/O
   ownership and performs a global `SR6F` translation flush;
6. full guard preparation, whole-object cache flush, and DMA sync to device;
7. one fence attachment, one state-complete command, one cookie wait, one
   PID00 PAUSE wait, and an engine-work-mask wait;
8. whole-object DMA sync to CPU, cache flush, guard scan, and scanout resync;
9. PTE invalidation through a second VGA acquisition/global flush, followed
   by SF node removal, vunmap, and allocation teardown.

The live glmark Gouraud trace made 237 private draw ioctls in one frame and
spent 383.435 ms in all ioctls out of 584.657 ms wall time.  The matched
glmark texture cube takes 14.385 ms for twelve separately submitted textured
triangles versus 6.045 ms untextured.  A retained ioquake trace contains
1,500 accepted triangles, 1,490 of them one-texture blend.  These workloads
do not prove the latency of any one operation, but they do prove that
per-triangle submission dominates useful work.  See
[batching-workload-priority.md](batching-workload-priority.md) for the source
logs and exact qualifications.

After the first tranche, all nine groups occur once per immutable run rather
than once per triangle.  After the mapping-cache tranche, groups 2, 4, 5, and
9 lose their PTE/SF allocation and translation work on a hit.  The job/fence,
reservations, cache ownership, and guards deliberately remain once per run.

## Vendor model

All three old driver families retain the command and SF infrastructure and
emit state once before a primitive range:

- Windows D3D `VTGUModeDX32A.dll:0x101f7410 -> 0x101f5350` and OpenGL
  `VTGOGL32A.dll:0x1024be90 -> 0x10249dd0` build a triangle list whose A24
  contains the complete `3 * triangle_count` vertex count.  Their normal draw
  callbacks (`0x10156b20` and `0x102019a0`) collect dirty state once before
  the range.
- Windows KMD keeps a four-MiB command ring.  Ordinary publication at
  `VTGKModeDX32A.sys:0xc8190` advances only A64/A65; it does not recreate the
  ring or SF address space for every primitive.
- Proprietary Linux 5.75 allocates variable-sized command space through
  `cmGetSpace_inv:0xb2730`, releases it through
  `cmReleaseSpace_inv:0xb2690`, and kicks a resident ring through
  `cmKickoffDMA_inv:0xb25e0` / `0xb0fb0`.  More directly, its PCIe setup maps
  one SG allocation covering `DMA_size + agp_tex_size` into consecutive PTEs
  from SF zero; `agp_offset = DMA_size`, and later heap allocations are stable
  offsets in the post-ring SF range (`via_chrome9_dma.c:157-189,617-620,
  669-676`, `via_chrome9_mm.c:661-745`).  It does not republish object PTEs at
  every primitive boundary.
- Windows KMD's allocator-level SF map/unmap routines are at
  `VTGKModeDX32A.sys:0x2ddf0` / `0x2db10`.  They populate an allocation
  residency array alongside the PTEs and issue the `SR6F` translation flush;
  the UMD draw path instead submits relocation/state ranges.  This supports
  allocation-lifetime translation residency, while not establishing Linux's
  proposed LRU limits or eviction policy.
- Mesa 7.11.2 UniChrome extends a primitive until a 3,968-byte DMA high-water
  mark instead of firing each triangle independently.

The Windows and old Linux ring allocators both use MMIO `043c` and an
unbounded polling/rewind algorithm.  They disagree on whether the VX900 tail
jump is 16 or 64 bytes before the aligned cursor.  Therefore they establish
the persistent-list architecture, but they do **not** justify importing their
circular overwrite algorithm.  The live-proven monotonic append plus bounded
PID10 full-idle wrap remains authoritative; see
[monotonic-command-ring.md](monotonic-command-ring.md).

## Tranche A: one large immediate list

### Exact 32-KiB closure proof

Every builder starts with the H0 sync and aligns the 3D body at byte `0x20`.
After the body it emits H0 sync, H2 status, H2 CR dummy, and H0 cookie packets,
with the existing 16-byte header alignment.  `chrome9_finish_ring()` then
always advances to the next 128-byte boundary, defines PAUSE as that cursor
minus `0x10`, emits an immutable H2 dummy, advances to the following
128-byte boundary, and defines STOP as that cursor minus `0x10`.
`chrome9_append_segment()` copies through
`STOP + CHROME9_CR_READ_AHEAD`, where the read-ahead constant is `0x220`.

Parameterize this code by an explicit staging capacity; do not alter the
alignment algorithm.  The worst state-complete bodies and exact limits are:

| path | body dwords | body end | post-body end | PAUSE | STOP | copied end | headroom |
|---|---:|---:|---:|---:|---:|---:|---:|
| D16 RHW + blend, 272 triangles | `174 + 18*272 = 5070` | `0x4f58` | `0x4fc8` | `0x4ff0` | `0x5070` | `0x5290` | `0x2d70` |
| one texture + depth + blend, 256 triangles | `212 + 24*256 = 6356` | `0x6370` | `0x63d8` | `0x63f0` | `0x6470` | `0x6690` | `0x1970` |
| two texture program 2 + depth + blend, 256 triangles | `248 + 30*256 = 7928` | `0x7c00` | `0x7c68` | `0x7c70` | `0x7cf0` | `0x7f10` | `0x00f0` |

The final row is the limiting proof.  Its copied/read-ahead interval is 240
bytes short of `0x8000`, so it fits a 32-KiB stage and a 64-KiB persistent
ring.  Because `finish_ring()` uses a do/while, an already 128-byte-aligned
body end still advances by one dummy pair and then to the next boundary; the
shown `0x7c70` PAUSE accounts for that behavior.

The A24 values are `0x24000330` for 816 untextured vertices and
`0x24000300` for 768 textured vertices.  A20 remains list
`0x20000000`, and A23 remains `0x23004f02`.  Do not substitute fan
`0x201c5c00`.  The list builder must assert all three words, its body count,
PAUSE, STOP, and copied end for every supported state key.

The maximum copied userspace arrays are also bounded:

| path | vertex size | count | bytes |
|---|---:|---:|---:|
| D16 RHW | 20 | 816 | 16,320 |
| one texture | 32 | 768 | 24,576 |
| two texture | 40 | 768 | 30,720 |

These limits fit the staging proof; they are not permission to put the arrays
on the kernel stack.

### UAPI transition

Do not resize or reinterpret an already shipped structure.  Command `0x0f`
and capability bit 22 were reserved for the 272-triangle D16 RHW run;
commands `0x14`/`0x15` and capability bits 30/31 are reserved for one- and
two-texture runs.  Before those capabilities are implemented or advertised,
revise their design from eight one-page segments to one large packet while
retaining the same maxima:

```text
0x0f: triangle_count 1..272, one A24 list, one retirement
0x14: triangle_count 1..256, one A24 list, one retirement
0x15: triangle_count 1..256, one A24 list, one retirement
```

The textured structures continue to carry exactly one immutable state
envelope and one vertex pointer.  All flags, formats, pitches, depth/blend
state, texture handles/layouts, wrap/filter state, program enum, scissor,
and reserved-word rules from
[textured-triangle-run-list-design.md](textured-triangle-run-list-design.md)
remain unchanged.  Only `MAX_TRIANGLES`, the removal of
`MAX_SEGMENTS`, and the execution model change.

If any command/capability has reached a released userspace header before this
change, allocate a new command and capability instead of changing its
contract.  In the current bring-up tree these are design reservations rather
than an advertised upstream ABI, but that must be checked again at patch
time.

### Kernel source sequence

Implement this tranche without changing GART lifetimes:

1. Add `CHROME9_RUN_STAGING_SIZE SZ_32K` and
   `engine->run_staging_cpu`.  Allocate it once with
   `vzalloc(CHROME9_RUN_STAGING_SIZE)` at engine initialization and free it
   with `vfree()` only after the engine has stopped.  `vzalloc()` gives the
   page-aligned virtual base required by the emitters' absolute-pointer
   alignment.  It is CPU-only and is never entered into GART.
2. Factor `chrome9_finish_ring_sized(base, p, capacity, pause, stop)`.
   Preserve a page-sized wrapper for every existing builder.  Bounds checks,
   dummy filling, PAUSE/STOP derivation, and the read-ahead check use the
   passed capacity.
3. Make `chrome9_append_segment()` accept a source pointer and source
   capacity.  Reject `STOP + 0x220 > capacity` and
   `copy_len > CHROME9_RING_SIZE`.  Existing builders pass the old page;
   run builders pass the 32-KiB stage.
4. Add one run builder per vertex layout.  Copy the fixed state template
   once, set A24 to the checked full vertex count, stream validated converted
   vertices directly into the stage, add one status/cookie closure, then call
   the sized finisher.  Do not allocate a second native-vertex array.
5. Validate and copy every userspace vertex before GEM lookup, reservation,
   SF publication, or engine locking.  Overflow-check `triangle_count*3` and
   `vertex_count*sizeof(vertex)` first.
6. Reuse one existing synchronous multi-object submit helper for the complete
   run: one `drm_exec`, one fence, one transient SF mapping, one guard/cache
   ownership interval, one append, and one `chrome9_wait_job()`.

The largest `0x7f10` copy is less than half the 64-KiB ring.  A second
maximum-size command will usually force the current bounded PID10/full
reinstall path; that is safe and intentional.  Do not enlarge the ring or
enable vendor circular byte reuse in this tranche.  Ring size can be raised
independently after the list packet is proven.

### Mesa state lifetime

Accumulate triangles only within one Gallium `draw_vbo` call initially.  The
run key is the complete native state envelope:

```text
destination object/level/format/pitch/dimensions
depth object/level/pitch/function/write-enable
texture object(s)/levels/formats/pitches/dimensions
sampler wrap/filter and program enum
blend equation/factors/mask/alpha test/constant
scissor and post-TNL vertex layout
```

Flush on capacity, key mismatch, outer draw end, validation failure, or
fallback.  The first implementation must not retain vertices or resource
references across Gallium draws; that would require explicit state hooks and
resource-lifetime bookkeeping.  On a failed run ioctl, report the error and
do not replay a prefix through legacy ioctls because the kernel may have
published the complete list before the failure.

### File-by-file patch boundary

Keep reviewable commits at the nested-repository boundaries:

- kernel UAPI `include/uapi/drm/chrome9_drm.h`: run command/capability,
  fixed-size structures, maxima, and ioctl definitions only;
- kernel `chrome9_engine.c`: persistent run stage, sized finish/append,
  builders, prepublication validation, synchronous submit, counters, and
  capability publication only after the corresponding implementation exists;
- kernel `chrome9_drv.c`: ioctl table entries; no KMS behavior change;
- Mesa `include/drm-uapi/chrome9_drm.h`: byte-identical UAPI copy;
- Mesa `chrome9_drm.[ch]`: capability query and one wrapper per structured
  run, with no native command words;
- Mesa `chrome9_context.[ch]` / `chrome9_draw.c`: outer-draw accumulator,
  immutable key comparison, and legacy fallback before any run is submitted;
- `chrome9_screen_smoke.c` and the standalone tools: generated maximum/count
  rejection, image/guard, ring-wrap, and counter assertions.

The SF/GART cache is kernel-internal and needs no new UAPI or Mesa behavior.
It should be a later `chrome9_engine.c` commit plus focused KUnit/helper tests
for span validation, LRU eligibility, aliases, and failure transitions.

### First silicon gates

Promote each path independently in this order:

1. 2 triangles with disjoint solid colors and guards;
2. previous one-page maximum and maximum plus one (34/35 untextured, 24/25
   one texture, 18/19 two texture);
3. 64, 128, and 256 triangles;
4. 272 triangles for D16 RHW;
5. every supported depth/blend/program endpoint, with the highest-size
   two-texture program-2 row last;
6. two consecutive maximum runs to force a monotonic-ring wrap.

Each gate requires exact interior samples, unchanged guard pages, cookie and
PAUSE retirement, engine error register `040c=0`, safe unload/reload, and a
successful post-test legacy triangle.  The maximum count plus one must fail
before lookup/publication.  Run a single new state/program per reboot during
initial promotion.

## Tranche B: persistent SF/GART object mappings

### Cache record and identity

Add a device-global cache keyed by `struct drm_gem_object *`, not by handle.
Handles are per-file and may alias; the GEM object is the lifetime identity.
One possible internal record is:

```c
struct chrome9_sf_binding {
        struct drm_gem_object *obj; /* cache-owned reference */
        struct sg_table *sgt;       /* valid while obj reference is held */
        struct drm_mm_node node;
        u32 *ptes;
        u32 page_count;
        unsigned int active;
        struct dma_fence *last_fence;
        u64 last_used_seq;
        bool scanout;
        bool quarantined;
        struct list_head lru;
};
```

Use a hash or xarray for object lookup and an LRU list for idle records.  The
record owns a GEM reference from insertion through eviction, so a closed
userspace handle cannot free or recycle its SGT/PTE source underneath a live
mapping.  Handle aliases which resolve to the same GEM object naturally find
the same record.  Imported foreign objects remain outside the bounded
renderer contract; no PRIME/offload dependency is introduced.

The first cache limit should be both 256 objects and 48 MiB of the 64-MiB SF
aperture.  This leaves the resident 64-KiB ring and substantial allocation
headroom while preventing an application from pinning every historical BO.
The exact byte limit is policy, not hardware evidence, and should be a
read-only module parameter during measurement.

The engine already has persistent `chrome9_sf_mapping` records for direct
scanout.  Do not create a second SF alias when the render object is present in
`scanout_mappings`: under `submit_lock`, borrow that record's stable node for
the synchronous run and treat it as a non-evictable cache hit.  KMS unpin also
takes `submit_lock`, so it cannot invalidate the mapping during the run.  Keep
the existing retirement-side `chrome9_sf_resync_object_locked()` transition;
PTE reuse does not remove display/cache ownership requirements.  A later
cleanup may unify the record types, but that is not required for the first
cache implementation.

### Lock and publication order

Retain the current global synchronous submit serialization:

```text
copy/validate userspace
GEM lookup and get the shmem-cached SGT for every distinct object
drm_exec reservation of every distinct object
wait dependencies and vmap objects
submit_lock
  reject stopping/wedged/not-ready
  sf-cache lookup/create/evict under vm_lock
  publish all changed PTE spans in one transaction
  active++ for every referenced binding
  guards/cache sync, fence attach, build, append, wait
  CPU sync, guards, scanout resync
  active-- and record last_fence/last_used_seq
submit_unlock
unlock reservations, signal fence
```

Mapping lookup is protected by `submit_lock`; `drm_mm` and PTE changes also
require `vm_lock`, in that order.  Do not take an object reservation while
holding either engine lock.  All objects are already reservation-locked by
the surrounding `drm_exec` when they become active.

`drm_gem_shmem_get_pages_sgt()` takes the object's reservation internally,
so obtain the SGT before entering `drm_exec`, as the current submit path does;
calling it for the first time while the reservation is already held would
recurse on that lock.  On a cache hit, reuse the binding's PTE array and do no
allocation.  On a miss, allocate and populate the PTE array from the already
obtained SGT after reservations are held and before publication.  Allocation
under the sleepable engine mutex is legal; if its latency is material, a
speculative candidate can be prepared before `submit_lock` and discarded
after a double-checked hit, but that is not needed for correctness.

The current builders derive texture/depth addresses as offsets inside one
contiguous transient node.  Refactor them to receive an independent SF
address for destination, each texture, and depth.  A cache record owns one
stable, separately allocated node; concatenating unrelated object lifetimes
into one cached node would make eviction and aliases unsafe.

### One PTE flush for all changes

Replace repeated `chrome9_gart_set()` calls with a bounded span helper:

```text
validate every [first,count) range and source before MMIO
vga_get_uninterruptible() once
wait for full engine idle once
save sequence index and SR6C
clear SR6C protection once
write every invalidation and insertion span
posted-read the final dword of each nonempty span
wmb()
pulse SR6F once
restore SR6C/index and vga_put once
```

On a pure cache hit there are no spans and this helper is not called.  On a
miss, select idle victims first, invalidate their ranges and write all new
bindings in the same transaction, then publish commands.  PTE arrays are
created once at binding insertion and freed at eviction, eliminating the
current page-count-sized per-run allocation.

The helper must never rewrite or flush a cached translation while CR or any
engine work bit is active.  Current submission is synchronous, so this is
normally true after the previous job retirement; retain an explicit idle
predicate.  PTE success precedes command publication.  Failure before A64/A65
still wedges the mapping manager conservatively because old victim ranges may
already have changed.

### Eviction eligibility

A binding can be evicted only when all conditions are true:

- `active == 0`;
- it is not a scanout mapping;
- it is not quarantined;
- its retained last fence is signaled without an outstanding ownership use;
- the engine is at its software-owned PID00 PAUSE and the complete engine
  work mask is clear; and
- it is the oldest eligible LRU entry needed to satisfy the object/byte limit
  or a new `drm_mm` allocation.

There is a subtle fence race in the current synchronous path: `active` can
be decremented under `submit_lock` before the fence is signaled after
reservation unlock.  Therefore `active == 0` alone is insufficient; eviction
must also test the retained fence.  Drop victim GEM/fence references and free
CPU memory outside `vm_lock` after the PTE invalidation/node removal is
committed.

CPU/device DMA ownership is independent of PTE residency.  Every run still
syncs read/write objects for device before publication and for CPU after
retirement, and still prepares/verifies guards once.  CPU mmap writes do not
produce a cache-key transition that would make cross-ioctl sync elision safe.
The later deferred-DMA-role/SWIOTLB work may move DMA mapping ownership into
this record, but it must not be mixed into the first PTE-cache patch.

### Timeout and teardown

After any A64/A65 or PID10 publication failure, mark every binding referenced
by the job quarantined and retain its node, PTE array, SGT, object reference,
vmap ownership required by the existing job, and fence.  Set the engine
wedged and reject new submissions.  Quarantined mappings never re-enter the
LRU and are never invalidated or reused before reboot.

An `SR6F`/PTE-flush failure similarly makes every touched old and new range
untrustworthy; quarantine those records and wedge rather than attempting a
partial rollback.  Engine teardown may drain ordinary idle cache entries
only after a proved PID10 STOP/full-idle transition.  If the engine or any
mapping is quarantined, preserve the existing reboot-scoped ownership policy
instead of freeing memory still reachable by hardware.

## Required counters and timings

Add debugfs counters before optimizing so an FPS change can be assigned to a
phase.  Counters are monotonically increasing `u64`; timing maxima use
`ktime_get_mono_fast_ns()` around the named phase and are disabled by default
if their overhead is measurable.

```text
run_ioctl[untextured,tex1,tex2]
run_triangles and histogram 1,2,3-8,9-24,25-64,65-128,129-256,257+
state_dwords, vertex_dwords, ring_copy_bytes, ring_wraps
job_allocs, fence_jobs, cookie_waits, pause_waits
pte_array_allocs, sf_node_allocs
gart_cache_hits, misses, evictions, blocked_evictions, quarantines
gart_span_transactions, gart_insert_pages, gart_invalidate_pages
vga_get_count, vga_get_ns_total/max
srf6_flush_count, srf6_flush_ns_total/max
sync_device_calls/bytes, sync_cpu_calls/bytes
guard_prepare_calls/bytes, guard_scan_calls/bytes
phase_ns: validate, reserve_wait, map, gart, device_sync, gpu_wait,
          cpu_sync_guard, scanout_resync, teardown
timeout: command, triangle_count, ring_epoch, pause, stop, cookie,
         cr_status, engine_status, cache_hit_mask
```

For a successful tranche-A run, assert diagnostically that there is exactly
one fence job, cookie wait, and PAUSE retirement; transient GART mode should
show exactly one bind and one unbind transaction.  For a tranche-B all-hit
run, assert zero PTE span transactions and zero VGA acquisitions.  Never make
debug counter overflow or timing collection affect correctness.

Mesa should retain the counters already specified in
[batching-workload-priority.md](batching-workload-priority.md), especially
run size and flush reason.  Add the exact immutable-key field that caused a
flush; otherwise a small average run cannot be distinguished from a kernel
limit.

## Expected call-count changes

These are arithmetic expectations, not FPS promises:

- glmark texture cube: twelve legacy textured ioctls become one run ioctl;
- the 237-call glmark Gouraud trace still needs about 30 calls if each old
  call already held 34 triangles, but each new call has one large command and
  one retirement instead of eight state-complete segments/retirements;
- the retained 1,500-triangle ioquake sequence has a state-ignorant floor of
  six 256-triangle packets.  Its observed long state runs
  `174/318/326/330/342` plus the small transitions imply roughly fourteen
  run ioctls before measuring finer key boundaries;
- old per-triangle submission performs two GART transactions/VGA acquisitions
  per ioctl.  Fourteen tranche-A runs reduce a 1,500-triangle theoretical
  3,000 transactions to about 28; tranche-B cache hits reduce steady-state
  PTE transactions toward zero.

The acceptance target for glxgears is more than 60 FPS with unchanged image
and presentation behavior, but no implementation should claim that target
until counters show where the remaining frame time resides.  If GPU fragment
or presentation time dominates after submission amortization, broadening the
command ABI will not fix it.

## Patch and deployment order

1. Land counters only and collect glxgears/glmark/ioquake baselines.
2. Land the 32-KiB stage and sized finisher with no capability exposed; prove
   old one-page builders are byte-identical in offline tests.
3. Land one large D16 RHW list and promote it through the staged oracle.
4. Land one-texture, then two-texture large lists.  Keep each capability off
   until its complete accepted state matrix passes.
5. Collect phase timings.  Only then land the persistent SF mapping cache and
   batched PTE helper, first with a one-entry limit, then 16, then the bounded
   production limits.
6. Consider a larger persistent command ring only if wrap counters/time are
   material.  Do not implement circular overwrite until `043c` and the
   `-16/-64` tail conflict have a bounded live resolution.

This order preserves a recovery boundary: every new mechanism can be
disabled independently, while the legacy structured draw commands and the
live-proven monotonic ring remain available for bisect and post-failure
health tests.
