# Sustained program-24 submission lifetime and hang audit

## Scope and result

This note audits failure mechanisms outside the already-closed program-24
packet length.  The source snapshot is the 2026-09-21 Chrome9 kernel tree,
where `chrome9_submit_depth_triangle()` implements the four-object
two-texture+D16 paths.  It covers object lifetime, reservations, DMA/SWIOTLB
mapping, SF PTE publication, cache ownership, ring wrap, timeout, and
quarantine.  It does not change the driver.

The current four-object fence directions, PTE order, cache transitions, and
normal retirement/unbind order are internally consistent.  No object can be
freed while a normally executing or quarantined job still references it.
Program 24 also has the same `0x4f0` PAUSE, `0x570` STOP, `0x790` copied end,
and `0x500` ring advance as the shorter programs.  These findings make a
simple packet-overrun, fence-direction, or premature-object-free explanation
unlikely.

Three sustained-workload risks remain, in this order:

1. **Measured resource pressure:** every native GEM creation eagerly creates
   and retains a 32-bit DMA mapping.  On the VX900 machine this consumed the
   64-MiB SWIOTLB pool until old Xorg objects died.  q3 texture churn can
   exhaust or heavily fragment that shared pool even when each draw retires.
2. **High-frequency global translation churn:** every synchronous triangle
   binds and then invalidates its transient SF range.  Each operation takes
   uninterruptible VGA arbitration, rewrites the GART under `SR6C.bit7`, and
   pulses the global `SR6F.bit7` GTI cache flush.  Program 24 with four BOs
   still costs two such transactions per triangle.  The old Linux and Windows
   drivers instead retain a large mapped SF heap and batch commands in a
   persistent ring.
3. **Intentional post-publication quarantine:** a retirement timeout or a
   PID10-wrap timeout after publication wedges the engine and retains an
   unsignalled reservation fence, all BOs, DMA mappings, PTEs, and the ring
   until reboot.  A later waiter can look like a system hang even though this
   is the safety policy.  A prepublication dependency wait can separately
   consume up to four sequential five-second waits without touching hardware.

The smallest useful next diagnostic is not another shader dump.  Add one
monotonic submit sequence, one phase field, per-phase failure counters, and a
last-job snapshot containing the four object page counts, SF range, ring
epoch/offsets, program selector, and status registers.  Combine that with the
existing ring append/wrap counts and system SWIOTLB debugfs values.  This
distinguishes allocation pressure, CPU-side arbitration/wait stalls, GART
flush failure, command-fetch failure, and post-cookie 3D retirement failure
in one recovered run.

Fresh read-only access to `192.168.1.230` was attempted during this audit but
returned `No route to host`.  Thus the source audit and prior saved live
evidence below are authoritative for this revision; no new target counters
were available.

## Exact four-object role map

The historical names in `chrome9_submit_depth_triangle()` are misleading for
two-texture+D16 requests.  The actual order is:

| local/job field | program-24 role | PTE order | fence use |
|---|---|---:|---|
| `obj` / `destination` / `sgt` | render target | 0 | WRITE |
| `depth_obj` / `depth` / `depth_sgt` | texture unit 0 | 1 | READ |
| `texture2_obj` / `texture2_sgt` | texture unit 1 | 2 | READ |
| `texture3_obj` / `texture3_sgt` | D16 | 3 | WRITE |

The ioctl rejects aliases between all four objects, true foreign imports,
wrong device ownership, and total mapped size above the 64-MiB SF aperture
minus the persistent ring.  The target and D16 sizes are exact; mip-capable
texture allocations may exceed their canonical level-0 size under the
explicit existing rules.  Relocations use the same contiguous order:

```text
target = sf_base + PAGE_SIZE
tex0   = sf_base + target_bo_size + PAGE_SIZE
tex1   = sf_base + target_bo_size + tex0_bo_size + PAGE_SIZE
depth  = sf_base + target_bo_size + tex0_bo_size + tex1_bo_size + PAGE_SIZE
```

The collected PTEs come from DMA addresses, reject non-page-aligned or
above-32-bit segments, and set the documented cached-valid bit.  Therefore a
successful PTE collection names the exact active DMA mappings, not CPU PFNs.

## Submission and retirement timeline

The current code performs the following ordered transaction:

1. Look up and reference all BOs; validate roles, aliases, sizes, and imports.
2. Obtain each cached shmem SGT and collect all PTEs **before** reservations.
3. Initialize one job fence.
4. Use `drm_exec` WW locking to lock all object reservations and reserve one
   fence slot per object.
5. Wait all existing reservation uses conservatively with
   `dma_resv_usage_rw(true)`, five seconds per object.
6. Vmap all objects while their reservations remain held.
7. Lock `submit_lock`, then `vm_lock`; allocate one contiguous SF range and
   bind all four PTE lists with one `chrome9_gart_set()` transaction.
8. Initialize leading, level-0 trailing, and applicable final mip guards;
   flush CPU caches over the relevant full objects; and sync all four SGTs for
   device ownership.
9. Attach the new fence to target and D16 as WRITE, both textures as READ.
10. Build into the staging page, append to the persistent ring, publish the
    new PAUSE, and wait cookie, PAUSE, then non-CR work idle, each under the
    common 100-ms hardware deadline.
11. After proven retirement, sync all four SGTs for CPU, flush caches, execute
    `dma_rmb()`, and verify all guards.
12. Return any persistently scanned-out object to device ownership.
13. Under `vm_lock`, replace the whole transient range with invalid PTEs and
    complete the GTI flush; only then remove the `drm_mm` node.
14. Unlock, vunmap, signal the fence, release object references, and free the
    job through its fence release.

This ordering is conservative but safe.  In particular, the texture objects
do not incorrectly acquire WRITE fences, and the D16 object does not
incorrectly acquire a READ fence.  `drm_exec_retry_on_contention()` supplies
WW deadlock recovery; the explicit no-alias rule avoids duplicate-role
ambiguity.

One subtle future constraint is already documented by the deferred-mapping
design: if eager cached SGT creation is removed, replacement DMA mapping must
occur only after reservations and producer fences are acquired.  Calling the
public `drm_gem_shmem_get_pages_sgt()` from inside a held reservation would
deadlock, while mapping before the producer wait could snapshot stale data
into SWIOTLB.  The driver-owned role mapping in
`deferred-shmem-dma-mapping-plan.md` resolves both constraints.

## GART publication and global-flush pressure

`chrome9_gart_set()` requires `vm_lock` and performs one indivisible update:

```text
vga_get_uninterruptible(VGA_RSRC_LEGACY_IO)
wait SR6F.bit7 == 0
save sequencer index and SR6C
clear SR6C.bit7                     # table write protection only
write every PTE and read back the final one
wmb()
set SR6F.bit7 and wait for self-clear
restore SR6C and sequencer index
vga_put()
```

Both the old proprietary Linux source and Windows KMD use the same
`SR6C.bit7` write-protect and `SR6F.bit7` GTI-flush protocol.  The manuals and
both binaries contradict treating `SR6C.bit7` as a translation enable.  The
new code is correct on that point and keeps every BO and SGT alive through the
successful invalidation flush.

The sustained-workload concern is frequency, not order.  A successful
one-triangle ioctl performs one global flush for bind and another for unbind.
At thousands of immediate triangles, this repeatedly serializes all SF
translation users and enters `vga_get_uninterruptible()`.  The VGA-arbiter
wait has no Chrome9-local timeout; a contending legacy-VGA owner can leave the
submitting task in an uninterruptible wait before any driver timeout message.
The two `SR6F` polls themselves are bounded to 100 ms.

The vendor lifetime model is materially different:

- old Linux maps one persistent noncached system-memory allocation into a
  post-ring SF heap and keeps it until driver cleanup;
- its userspace batches state and many primitives into DMA buffers rather
  than issuing one bind/draw/unbind transaction per triangle;
- Windows keeps a 4-MiB persistent command ring and its ring allocator
  advances A64/A65, while KMD mapping/patching operates on submitted
  allocations rather than globally rebinding the same four-object tuple for
  every immediate primitive.

These sources prove that long-lived backing and batching are vendor practice;
they do not prove that transient mapping is electrically invalid.  A bounded
same-BO GART-churn test is required before blaming the program object.

## DMA/SWIOTLB lifetime and cache ownership

`chrome9_gem_create_ioctl()` currently calls
`drm_gem_shmem_get_pages_sgt()` for every new BO.  That helper pins pages,
creates and DMA-maps an SG table bidirectionally, and caches it in
`drm_gem_shmem_object::sgt` until object destruction.  The VX900 has a
32-bit DMA mask, so pages outside its addressable range consume SWIOTLB bounce
slots for the whole GEM-object lifetime, not merely while a draw executes.

Prior target measurements are decisive:

```text
SWIOTLB pool: 32768 * 2 KiB = 64 MiB
observed used: 32174, 32344, 32379 / 32768
failed mappings: commonly 262144 bytes, also 69632..258048 bytes
after prior Xorg processes exited: io_tlb_used returned to roughly 85..200
with the then-current Xorg alive: io_tlb_used was 5207
```

This is lifetime-bound pressure, not a permanently broken SWIOTLB allocator.
The same-device PRIME path returns the original Chrome9 GEM object, so another
handle can extend the same cached mapping's lifetime without creating a true
foreign import.  The screen-local Gallium handle manager fixes duplicate
handle ownership, but live Xorg resources legitimately keep their kernel
objects and cached DMA mappings alive.

Within a submitted job, cache ownership is ordered correctly:

```text
CPU guard writes + whole-object clflush
dma_sync_sgtable_for_device(all roles)
GPU publication and proven retirement
dma_sync_sgtable_for_cpu(all roles) + clflush + dma_rmb
guard inspection
dma_sync_sgtable_for_device(active scanout aliases)
PTE invalidation
```

Whole-object cache flushing of two textures, target, and depth for every
triangle is expensive and can resemble a machine stall, but by itself should
not wedge hardware.  Never remove the ownership transitions as a performance
experiment: on SWIOTLB, an incorrect sync-for-CPU can copy stale bounce data
over newer shmem contents.  Measure time and bytes by role first.

The production correction is the already-specified reservation-protected DMA
role manager: no eager map at GEM creation; map after producer waits; keep one
mapping while draw/scanout roles exist; release after retirement and PTE
invalidation; retain permanently on quarantine.  Increasing `swiotlb=` is a
diagnostic only because it merely moves the exhaustion point.

## Ring append, wrap, and retirement

Ordinary append copies a staged segment plus the fixed `0x220` read-ahead
closure into the persistent 64-KiB ring, cache-flushes the copied range, does
`dma_wmb()`, then publishes only A64/A65.  It immediately records the published
PAUSE/STOP and advances `append` to `ring_pause + 0x10`.

When the segment no longer fits, the code:

1. verifies owned PAUSE and non-CR idle;
2. replaces PAUSE with the job's later PID10 STOP;
3. waits the full engine wait mask idle;
4. fills the entire ring with legal dummy words;
5. copies the new segment at offset zero and flushes all 64 KiB;
6. installs and triggers the full A60--A68 tuple; and
7. increments `ring_epoch` and `wrap_count`.

The monotonic ring previously completed 1,272 appends and 28 wraps with safe
unload/reload.  Program 24 does not change wrap cadence because its additional
eight ALU dwords fit inside the same finish-alignment slack.  Generic wrap is
therefore live-proven, although q3 uniquely combines it with four transient
BO mappings, high allocation churn, and new shader/state transitions.

The vendor circular-ring algorithms are not a safer replacement.  Linux and
Windows reserve 64 bytes beyond an ordinary request and 0x400 bytes at wrap,
double-read unreliable `043c`, and poll without deadlines.  Their VX900 tail
JUMP implementations disagree on minus 16 versus minus 64.  The current
bounded PID10/full-reinstall wrap deliberately avoids that unresolved reuse
protocol.

## Timeout and quarantine state matrix

| failure point | hardware publication | fence signal | resources |
|---|---|---|---|
| lookup, validation, SGT/PTE, WW lock, dependency wait, vmap | none | none/new fence not attached | normal unwind |
| GART bind or packet build, successful PTE cleanup | no ring work | error fence signalled if attached | normal unwind |
| prepublication cleanup PTE flush fails | no ring work, but SF reachability uncertain | error fence signalled | quarantine |
| ordinary job cookie/PAUSE/work timeout | ring published | deliberately **not signalled** | job, four BOs, DMA maps, PTEs, SF node, ring retained; engine wedged |
| wrap STOP timeout before PID10 publication | no new tuple | error fence signalled after cleanup | normal unwind if PTE cleanup succeeds |
| wrap STOP timeout after PID10 publication | transition ownership uncertain | deliberately **not signalled** | quarantine |
| post-retirement PTE invalidation/flush fails | retirement proven | error fence signalled | mappings/PTE range quarantined |

The unsignalled fence in an unproven post-publication timeout is intentional:
signalling it would let users reuse or free memory that hardware may still
reach.  The quarantine takes module and device holds, marks the engine wedged,
and makes teardown refuse unsafe release.  Subsequent submits fail readiness,
but any consumer waiting the retained reservation fence can block indefinitely.
That observable effect must be labelled `quarantine`, not confused with an
unbounded hardware poll.

`chrome9_wait_job()` already identifies the first failed phase—cookie,
PAUSE, or work—and dumps sequence, kind, flags, epoch, cookie, `043c`, ring
PAUSE/STOP, `0444`, `0400`, and a ring window.  Interpret it as follows:

- cookie miss with `043c` before/in the PS object: fetch/decode/program issue;
- cookie miss after the vertex payload: downstream 2D cookie or ordering issue;
- cookie hit and PAUSE absent: command-reader publication/fetch issue;
- cookie and PAUSE hit but work bits remain: 3D shader/fetch/PE retirement;
- no timeout log and task in D state: inspect VGA arbitration and reservation
  waits before treating it as GPU execution failure;
- clean retirement followed by failure on the next ioctl: inspect unbind GTI
  flush, SWIOTLB allocation, and ring wrap boundary.

## Minimal counters and last-state snapshot

Add device-local atomics/debugfs only; no UAPI is required.  Avoid per-draw
`printk`, which can perturb q3 enough to hide the failure.

### Required first tranche

```text
submit_seq, submit_completed, last_completed_seq
submit_prepub_fail
dep_timeout_target, dep_timeout_tex0, dep_timeout_tex1, dep_timeout_depth
gart_bind_count, gart_unbind_count, gart_wait_fail, gart_flush_fail
vga_get_enter, vga_get_return
sf_alloc_fail, sf_pages_live, sf_pages_high_water
timeout_cookie, timeout_pause, timeout_work
quarantine_jobs, quarantine_unsignalled_fences
```

Retain the existing `append_count`, `wrap_count`, and `ring_epoch`.  Update one
`last_job` snapshot under `submit_lock` at every phase transition:

```text
seq, phase, kind, flags, texture_program
four GEM handles or stable object-role tags and four page counts
sf_first, sf_page_count
ring_epoch, append_before, ring_pause, ring_stop
cookie_expected, cookie_observed
043c, 0444, 0400, 040c
last_errno, fence_attached, published, retirement_proven
```

Use phases `LOOKUP`, `WW_LOCK`, `DEP_WAIT_0..3`, `VMAP`, `SF_ALLOC`,
`GART_BIND`, `CACHE_TO_DEVICE`, `BUILD`, `RING_PUBLISH`, `COOKIE`, `PAUSE`,
`WORK`, `CACHE_TO_CPU`, `GUARDS`, `GART_UNBIND`, and `DONE`.  Record the phase
before entering VGA arbitration and immediately after it returns.  If the
machine recovers with `vga_get_enter > vga_get_return`, the CPU-side owner wait
is localized without requiring a GPU register access.

Record maximum elapsed microseconds, not every sample, for:

```text
WW lock/retry, each dependency wait, VGA arbitration, GTI flush,
cache+DMA sync by role, GPU wait, and GART unbind
```

Also record bytes cache-flushed and DMA-synced by role.  This tells whether the
machine is wedged or merely spending most wall time touching large objects.

### DMA-lifetime counters

The long-term role manager should expose the exact existing design counters:

```text
dma_maps, dma_unmaps, dma_map_failures
dma_roles_live, dma_roles_high_water
dma_bytes_live, dma_bytes_high_water
dma_quarantine_roles
```

Before that conversion, the smallest useful observation is external:

```text
/sys/kernel/debug/swiotlb/io_tlb_used
/sys/kernel/debug/swiotlb/io_tlb_used_hiwater
/sys/kernel/debug/swiotlb/io_tlb_nslabs
```

Sample them before Xorg/q3, after texture load, immediately before gameplay,
at failure/recovery, and after q3 exits while Xorg remains alive.  Pair with
Gallium's shared-entry live/high-water counters.  A monotonic SWIOTLB rise
with no driver timeout distinguishes resource pressure from a program stall.

## Safest staged bisect on recovered hardware

Use a fixed four-BO guarded oracle and stop after the first failed phase.  Do
not replay a timed-out request or reclaim quarantine.

1. **Same objects, same program, no allocation churn.** Loop program 24 with
   fixed canonical target/TEX0/TEX1/D16 BOs past the q3 failure count.  This
   retains the two GART transactions and every ring wrap but holds SWIOTLB
   object count constant.
2. **Program transition.** Alternate selector 0 and program 24 using the same
   BOs and in-range coordinates.  Then repeat with cap-40 signed REPEAT.  A
   failure only on transition implicates state closure; one at the same submit
   or wrap count implicates lifecycle/ring/GART.
3. **Remove depth/blend envelope.** Run the same RGB-visible program-24 result
   through the no-depth two-texture command, then D16, then D16+blend.  Keep
   alpha checks only where the target supports them.  This reduces four roles
   to three before adding D16 without changing the physical PS object.
4. **GART-only churn while idle.** A debug-only selftest may bind and invalidate
   the same four DMA mappings N times with the engine proven idle and no ring
   publication.  Verify guards and statuses every iteration.  This isolates
   VGA arbitration/SR6F/PTE lifetime and is safer than emitting a suspect
   shader repeatedly.
5. **Allocation churn without drawing.** Allocate/destroy q3-sized guarded BOs
   while sampling SWIOTLB.  Eager-mapping exhaustion here conclusively excludes
   PS execution.
6. **Wrap boundary.** With fixed BOs, stop immediately before and after each
   expected wrap.  Correlate failure with `ring_epoch`, `wrap_count`, PID10
   publication, and last completed sequence; never infer retirement from
   `043c` alone.
7. **q3 replay.** Only after the fixed-BO loop, GART-only loop, and allocation
   churn are classified, rerun q3 with counters but without per-draw logging.

Do not test by disabling cache sync, guard verification, PTE invalidation, or
quarantine.  Those are reachability and coherency guarantees, not optional
diagnostics.  A future persistent per-object SF mapping and multi-triangle
batch should be introduced only after these counters establish whether it is
an optimization or the correction for the observed failure.

## Evidence cross-reference

- **Code:** current `chrome9_engine.c`, especially
  `chrome9_gart_set()`, `chrome9_append_segment()`, `chrome9_wait_job()`, and
  `chrome9_submit_depth_triangle()`.
- **Manual/source/binary:** `via-5.75-kmd.md` documents the old persistent SF
  heap, exact `SR6C/SR6F` protocol, PTE lifetime, command ring, and batching;
  `windows-driver-re.md` and `monotonic-command-ring.md` recover the Windows
  4-MiB ring, ordinary A64/A65 publication, mapper/unmapper, and wrap logic.
- **Measured:** `monotonic-command-ring.md` records 1,272 appends and 28 wraps;
  `dmabuf-handle-swiotlb-lifetime.md` records the VX900 32-bit DMA mask,
  32,768-slot pool, mapping failures, and release after Xorg lifetime ended.
- **Design:** `deferred-shmem-dma-mapping-plan.md` gives the exact
  reservation-safe DMA-role replacement and fault tests;
  `render-to-scanout-ownership.md` defines the safe cache/scanout handoff.
- **Program-specific:** `program24-signed-repeat-hang-audit.md` proves the
  unchanged body closure/wrap cadence and records the separate A01 perspective
  correction, which predicts interpolation error rather than a hard hang.
