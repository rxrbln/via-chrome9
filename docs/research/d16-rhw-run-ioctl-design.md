# Bounded eight-segment D16 RHW run design

## Goal and non-goals

The live Gouraud trace issued 237 synchronous
`DRAW_TRIANGLE_D16_RHW_BATCH` ioctls for one cat frame.  Each ioctl can carry
only 34 triangles because the current command plus its `0x220`-byte read-ahead
closure nearly fills the one-page staging buffer.  The desired optimization
is one resource-ownership transaction around several **unchanged** proven
packets, not a larger packet, asynchronous execution, raw command ABI, native
index fetch, or persistent cross-ioctl mapping.

The first run is limited to eight segments.  It copies at most 272 triangles,
or 816 post-draw vertices, binds the color and D16 objects once, executes up
to eight existing 34-triangle command segments synchronously, then performs
one CPU ownership transition, guard scan, unbind, and fence signal.  The
existing batch remains the compatibility and fallback path.

The original design was based on kernel Chrome9 commit `f39a90d`, UAPI
commit `b623464`, and Mesa Chrome9 commit `91cd42a`.  The patch-ready audit
below was repeated against kernel Chrome9 `f56ac02` and Mesa Chrome9
`d7c2570` on 2026-09-21.  In particular it includes the subsequently merged
read-only-depth and extended-blend-factor state.  The private-number
assignments come from
[chrome9-uapi-allocation.md](chrome9-uapi-allocation.md).

## Exact UAPI

Use new command `0x0f` and capability bit 22.  Command `0x0e` and bit 21 are
already reserved for the two-level mip successor.

```c
#define DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN          0x0f
#define DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_RUN      (1ULL << 22)

#define DRM_CHROME9_TRIANGLE_RUN_MAX_SEGMENTS          8U
#define DRM_CHROME9_TRIANGLE_RUN_MAX_TRIANGLES         \
        (DRM_CHROME9_TRIANGLE_RUN_MAX_SEGMENTS *       \
         DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES)     /* 272 */

#define DRM_IOCTL_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN    \
        DRM_IOW(DRM_COMMAND_BASE +                     \
                 DRM_CHROME9_DRAW_TRIANGLE_D16_RHW_RUN, \
                 struct drm_chrome9_draw_triangle_d16_rhw_run)

struct drm_chrome9_draw_triangle_d16_rhw_run {
        __u32 destination_handle;       /* 0 */
        __u32 depth_handle;             /* 4 */
        __u32 flags;                    /* 8 */
        __u32 triangle_count;           /* 12: 1..272 */
        __u32 width;                    /* 16 */
        __u32 height;                   /* 20 */
        __u32 destination_pitch;        /* 24 */
        __u32 depth_pitch;              /* 28 */
        __u32 destination_format;       /* 32 */
        __u32 reserved0;                /* 36: zero */
        struct drm_chrome9_blend_state blend;   /* 40..79 */
        struct drm_chrome9_scissor_state scissor; /* 80..91 */
        __u32 pad;                      /* 92: zero */
        __u64 vertices_ptr;             /* 96 */
        __u64 reserved[3];              /* 104..127: zero */
};
```

The structure is exactly 128 bytes on 32- and 64-bit userspace and deliberately
duplicates the old fixed state envelope.  Do not reinterpret the old
structure or raise its maximum.  Add compile-time size assertions to the
kernel and Mesa UAPI copy.  The resulting x86 ioctl value is `0x4080644f`.

The run accepts exactly the state accepted by the current perspective batch:

- `DEPTH` is mandatory; `BLEND` is optional and all other unknown flag bits
  are rejected;
- the depth-function field accepts implicit/explicit LESS and explicit
  LEQUAL through the existing bounded helper;
- `DRM_CHROME9_DEPTH_READ_ONLY` is accepted because capability bit 23 and the
  common A01 lowering are now merged; it disables depth writes without
  changing the two-object lifetime or the depth object's WRITE reservation
  fence;
- width and height are each `1..2048`;
- destination pitch is exactly `ALIGN(width * 4, 32)` and D16 pitch exactly
  `ALIGN(width * 2, 32)`;
- destination format is BGRX8888 or the already capability-gated BGRA8888
  blend form;
- an absent batch `BLEND` flag requires format zero and an all-zero blend
  structure;
- an enabled batch blend envelope reuses `chrome9_validate_blend_state()`:
  factors are the merged enum range ZERO through SRC_ALPHA_SATURATE, RGB and
  alpha functions are ADD, masks contain only RGBA bits, alpha reference is
  `0..255`, and blend-disabled state inside that envelope must retain the
  canonical ONE/ZERO factors;
- scissor uses the existing half-open validator;
- all padding and reserved words are zero.

The `DEPTH`, `BLEND`, depth-function and read-only fields share one `u32`, but
they are not interchangeable namespaces: the exact allowed mask is
`DEPTH | BLEND | DRM_CHROME9_DEPTH_FUNC_MASK |
DRM_CHROME9_DEPTH_READ_ONLY`.  All other bits fail before object lookup.

## Handler validation and overflow order

Reuse the old batch handler but give the run a distinct entry point and
public structure.  Every condition below must complete before GEM lookup,
PTE publication, or command construction:

1. Validate all fixed fields, flags, formats, blend, and scissor.
2. Require `triangle_count` in `1..272` and a nonzero `vertices_ptr`.
3. Compute `destination_pitch * height` in `u64`, page-align it, and add two
   guard pages with checked multiplication/addition.  Repeat for D16.
4. Check the sum of exact object sizes and require it at most
   `SZ_64M - CHROME9_RING_SIZE`, matching the SF aperture reservation.
5. Check `triangle_count * 3` before forming `vertex_count`; check
   `vertex_count * sizeof(struct drm_chrome9_vertex_d16_rhw)` before copy.
   At the limit these are 816 records and 16,320 bytes.
6. Use `memdup_array_user()`.  No userspace pointer remains live after this
   copy.
7. Validate every vertex: the existing signed 16.16 half-pixel coordinate
   bounds, Z in `0..0x10000`, finite positive IEEE-754 RHW, and one
   nondegenerate cross product per three records.

Retain the checks even though the public maxima make the arithmetic fit
today.  Also use checked additions for destination/depth page counts before
`kcalloc()`: the later aperture-size check is not a reason to leave an
intermediate native-width sum unchecked.

The handler frees the copied array on every return after validation.  The
internal request may point into it only for the synchronous duration of
`chrome9_submit_depth_triangle_run()`.

At the public maxima, the checked quantities are:

| quantity | maximum |
|---|---:|
| destination pitch | `ALIGN(2048*4,32) = 8192` bytes |
| D16 pitch | `ALIGN(2048*2,32) = 4096` bytes |
| guarded destination | `8192*2048 + 8192 = 16,785,408` bytes / 4,098 pages |
| guarded D16 | `4096*2048 + 8192 = 8,396,800` bytes / 2,050 pages |
| combined mapping | `25,182,208` bytes / 6,148 pages |
| aperture limit | `64 MiB - 64 KiB = 67,043,328` bytes |
| total vertices | `272*3 = 816` |
| copied vertex bytes | `816*20 = 16,320` |
| segments | `DIV_ROUND_UP(272,34) = 8` |
| vertices per segment | `34*3 = 102` |

The coordinate validator's widest possible delta is `2048<<16 = 2^27`;
each cross-product term is at most `2^54` and their difference is at most
`2^55`, which fits signed 64-bit arithmetic.
Pointer advancement uses vertex-record units and must assert
`first <= total`, `chunk <= total-first`, and `first*3 <= vertex_count`
before adding to the copied-array base.

`chrome9_rhw_valid()` is stricter than the phrase "finite positive": the
sign bit must be zero and the exponent must be in `1..254`.  Positive zero,
positive subnormals, infinities and NaNs are all rejected.  The run must call
that same helper and must not silently broaden the accepted RHW domain.

## One job, reservation fence, and resource lifetime

The new submit helper is the two-object subset of
`chrome9_submit_depth_triangle()` with the following unchanged lifetime:

1. Enter the DRM device and look up both handles in the calling
   `drm_file`.  Reject aliases, foreign/imported objects, and any size other
   than the two exact canonical guarded sizes.
2. Obtain both shmem SGTs, build the complete cached PTE image, initialize one
   `chrome9_job` fence, and prepare both objects in one interruptible
   `drm_exec` transaction.
3. Wait both reservation objects with `dma_resv_usage_rw(true)` and
   `CHROME9_DEP_TIMEOUT`, then vmap both while their reservations remain held.
4. Under `submit_lock`, allocate one contiguous SF node, publish its PTEs once,
   initialize both guards, flush both complete objects, and transfer both SG
   tables to the device once.
5. Attach the **same single fence** to destination and depth with
   `DMA_RESV_USAGE_WRITE`.  It represents the entire run, not one segment.
6. Execute the segment loop below.  After the final proved segment, transfer
   both objects to the CPU once, flush, verify all four guards, resync any
   persistent display mapping, invalidate the transient PTE range once, and
   remove the node.
7. Drop `submit_lock`, unmap, finish `drm_exec`, signal the run fence, drop GEM
   lookup references, and drop the submitter's fence reference.  The fence
   release continues to own and free `job->ptes` and the job allocation after
   all reservation references disappear.

Do not initialize one fence per segment and do not call `drm_exec_fini()`
between segments.  Either would expose intermediate ownership to another
submitter and defeat the optimization.  The ioctl remains synchronous and
exports no userspace fence.

## Segment loop and exact cookie sequencing

Keep `chrome9_build_depth_triangle()` unchanged, including its 34-triangle
limit and exact 4-KiB staging assertion.  A stack-local copy of the request
selects the current subrange:

```text
remaining = total triangle_count
first = 0
retired_cookie = engine->last_cookie

while remaining != 0:
        chunk = min(remaining, 34)
        chunk_request.triangle_count = chunk
        chunk_request.batch_vertices_rhw = vertices + first * 3
        job->cookie = retired_cookie == 00010001 ? 00020002 : 00010001
        build the existing packet
        append the existing segment
        wait for cookie, PAUSE, and non-CR engine idle
        retired_cookie = job->cookie
        engine->last_cookie = retired_cookie
        first += chunk
        remaining -= chunk
```

The cookie must alternate on **every** segment.  Reusing one cookie lets the
second `chrome9_wait_job()` accept the first segment's stale MMIO value and
can unbind live resources.  Update `engine->last_cookie` immediately after
each proved wait, while `submit_lock` is held, so a later prepublication error
still leaves the next submission with the last retired value.  Eight segments
can legitimately finish with the same cookie value seen at run entry; the
intermediate alternation is what prevents stale completion.

Keep `job->kind` equal to the new RUN kind for timeout diagnostics.  Extend
the builder's current perspective predicate from equality with
`CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_BATCH` to equality with either RHW
kind; do not disguise each chunk as an old ioctl kind.  `job->flags` remains
the one immutable run envelope.  `job->segment_pause`, `segment_stop`,
`ring_pause`, and `ring_stop` are intentionally overwritten with the current
segment so a timeout identifies the reachable command.  Set
`job->retirement_proven` only after the final chunk has passed all three waits.
For a build or append rejection after a retired prefix, a separate local
`current_published`/the append helper's `must_quarantine` result—not the
cumulative `job->published` bit—decides whether ordinary cleanup is legal.

`chrome9_append_segment()` may wrap the 64-KiB ring during the run.  Waiting
each segment before building/reusing the one-page staging buffer preserves its
current STOP replacement and read-ahead rules.  No segment can contain more
than:

```text
34 triangles / 102 vertices
782 dynamic body dwords without blend
PAUSE 0x0cf0, STOP 0x0d70
STOP + 0x220 read-ahead = 0x0f90 <= PAGE_SIZE
```

The blend derivative's four added state dwords retains the same aligned
PAUSE/STOP bound.  The run maximum changes neither A24's per-packet count nor
the native vertex payload.

## Failure and quarantine matrix

Partial rendering is possible: a later segment can fail after earlier ones
retired.  An ioctl error must never cause Mesa to replay the whole run.

| failure point | published work not proved retired? | action |
|---|---|---|
| UAPI, copy, vertex, GEM, reservation, vmap, or allocation validation | no | ordinary unwind; no fence attachment/publication |
| PTE bind or packet build before the first append | no | invalidate any bound PTEs; signal the fence with error if attachment has occurred, then normal unwind |
| packet build or append rejection before publication after earlier segments retired | no | preserve last retired cookie, unbind, signal fence with error; partial pixels are allowed |
| ring-wrap STOP update failed and `must_quarantine` is true | yes | set fence error, wedge engine, retain job/node/maps/objects/ring epoch, do not signal fence |
| cookie, PAUSE, or engine-work wait timed out | yes | same unsignaled-fence quarantine |
| final guard verification failed after retirement | no | set `-EIO`, unbind, signal error fence; do not replay |
| final PTE invalidation failed | commands retired, mapping cannot be disproved | wedge and retain the mapping; the current path may signal the error fence only after CPU ownership is restored |

`job->published` is cumulative and remains useful for diagnostics, but it is
not enough to choose the failure path in a loop.  Track whether the **current**
segment reached publication and whether its retirement was proved.  Earlier
published-and-retired segments do not require quarantine.  Conversely, never
free or signal the reservation fence for an unproved current segment: the
quarantine list and module/device holds intentionally make affected BO waits
remain blocked until reboot.

The current cleanup has two deliberately different fence outcomes.  A
published command whose retirement is unproved leaves `signal == false`:
`drm_exec_fini()` drops the reservation locks, but the unsignaled error fence,
GEM lookup references, vmaps, SGTs, PTE image and SF node remain owned by the
quarantined job.  A post-retirement PTE-invalidation failure sets
`signal == true`; its error fence is signaled, but the same mapping and object
references remain quarantined because the translation cannot be disproved.
Preserve this distinction exactly.  Engine teardown already refuses to pass
`drm_mm_takedown()` when the quarantine list is nonempty and takes permanent
module/device holds for reboot-scoped safety.

The first nonzero return sets the job fence error.  Stop immediately; do not
publish later chunks.  A userspace-visible error after a retired prefix is an
allowed partial draw failure, just as other DRM submits can fail after partial
side effects.  Mesa must mark the batch failed, reject the remainder of that
draw-module callback, and never fall back by resubmitting the vertices through
the old ioctl.

## Kernel integration checklist

- Copy the UAPI definition to kernel and Mesa, add both 128-byte static
  assertions, and reserve command `0x0f`/capability bit 22 in the canonical
  ledger.
- Add `CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN` only for diagnostics and
  builder classification; extend the builder's perspective test to both RHW
  kinds and do not make the run a texture job.
- Add a handler prototype, one `DRM_RENDER_ALLOW` descriptor, and the
  capability only under `chrome9_engine_ready()`.
- Refactor only the resource-lifetime shell needed to retain one two-object
  job across the segment loop.  Keep the old batch submit path intact for ABI
  regression and fallback.
- Add a `chrome9_next_cookie()` helper so the old path and run cannot diverge
  on the two exact cookie values.
- Pass a per-segment request copy to the existing builder; assert chunk count
  `1..34`, source offset within the validated copied array, and final
  `first == total`.
- Preserve lock order: reservations through `drm_exec`, then `submit_lock`,
  then `vm_lock` only around SF allocation/PTE changes.  Never wait for a
  reservation while holding `submit_lock` or `vm_lock`.
- Keep one device ownership interval, one fence, one node, and one PTE image.
  Do not perform guard scans or `dma_sync_sgtable_for_cpu()` between segments.
- Preserve the current active-scanout resync after the final CPU transition.
- Add segment index/total, chunk triangle count, and cookie to timeout logs;
  retain ring epoch/current/pause/stop/engine status.

## Patch-ready audit against the current kernel

The following is the smallest source change against Chrome9 `f56ac02`; no
second resource-management implementation is required.

| file / symbol | required change |
|---|---|
| external and Mesa UAPI copies | add command `0x0f`, ioctl macro, cap bit 22, the two run-limit constants and the 128-byte request; retain command `0x0e`/bit 21 for mipmaps and every later ledger allocation |
| `chrome9_engine.c` static assertions | assert the run request is 128 bytes and keep the 20-byte RHW vertex assertion |
| `enum chrome9_draw_kind` | append `CHROME9_DRAW_IMMEDIATE_TRIANGLE_D16_RHW_RUN`; do not renumber existing values |
| `chrome9_build_depth_triangle()` | treat BATCH and RUN kinds as perspective; leave its public-independent `1..34` chunk check, A24 count, six-dword vertex stride and all body assertions unchanged |
| `chrome9_draw_triangle_batch()` | factor fixed-envelope validation into a normalized internal copy; old batch keeps max 34 while the new handler selects max 272 and RUN kind |
| `chrome9_submit_depth_triangle()` | for RUN only, replace the single build/append/wait block with the bounded loop; reuse all code before and after it unchanged |
| `struct chrome9_job` / timeout log | add current 1-based segment, total segments and current chunk count for diagnostics; overwrite them before each build just like pause/stop |
| `chrome9_engine.h` / `chrome9_drv.c` | add the handler prototype, `DRM_RENDER_ALLOW` descriptor, and advertise bit 22 only from the engine-ready capability branch |

`DRM_IOCTL_DEF_DRV()` is a designated initializer indexed by private command
number, so command `0x0f` can be added while `0x0e` remains unimplemented;
`ARRAY_SIZE(chrome9_ioctls)` grows through index `0x0f` and the zeroed `0x0e`
slot rejects calls.  Do not insert an unindexed placeholder or compact the
table, and do not consume the already reserved `0x0e` mipmap command.

The submit refactor should move cookie selection from its present one-time
location before SF allocation to immediately before each packet build and use
one helper returning the alternate of only `0x00010001` and `0x00020002`.
After each successful `chrome9_wait_job()`, update `engine->last_cookie`
immediately under `submit_lock`.  Remove the old success-only assignment from
the PTE-unbind branch or leave it as an assertion/equal assignment; otherwise
a later prepublication failure after a retired prefix loses the actual cookie
history.

The existing two-object path already supplies all required lifetime rules:

- destination and depth aliases, foreign devices, imports and noncanonical
  exact sizes are rejected before mapping;
- both SGTs and the one combined PTE array live in `chrome9_job`; one
  `drm_exec` locks both objects and both reservation dependencies are waited;
- destination and depth both receive the same fence with WRITE usage, even
  for read-only depth, because this first ABI deliberately keeps conservative
  ownership and guard handling;
- the one combined SF node places color data at `node.start + PAGE_SIZE` and
  D16 data at `node.start + color_object_size + PAGE_SIZE`;
- one device sync precedes the loop, and one CPU sync, full cache flush,
  four-guard verification, active-scanout resync and PTE invalidation follow
  the final retired segment;
- normal cleanup unmaps, calls `drm_exec_fini()`, signals the one fence, drops
  both lookup references and drops the submitter's fence reference.  Fence
  release alone frees `job->ptes` and the job after reservation references
  disappear;
- live-engine quarantine bypasses every unmap/object-put/fence-put label.

Do not add an explicit put for the pointers returned by
`drm_gem_shmem_get_pages_sgt()` in this patch.  They are cached by the shmem
object and remain valid under the retained GEM reference; adding an unmatched
SGT put would create a new lifetime bug.  Deferred DMA-map lifetime changes
remain a separate project.

The current `page_count` additions use native `unsigned int`.  The public
limits make the exact run maximum only 6,148 pages, but the new normalized
validator should still calculate color pages, depth pages and their sum in
`u64`, reject any value above `UINT_MAX`, and only then narrow for
`kcalloc()`.  Similarly, compute rounded payload size as a checked add of
`PAGE_SIZE - 1` followed by `round_down()` before adding the two guards; the
present width/height maxima prove `ALIGN()` safe, but explicit checked order
keeps the new ABI review local and mechanically complete.

### Exact one-page packet boundary

The packet limit is still exact after read-only depth and extended blend
factors because neither changes command length.  The initial H0 sync leaves
the aligned H6 body at byte `0x20`:

| case | body dwords | body end | PAUSE | STOP | copied through read-ahead | headroom |
|---|---:|---:|---:|---:|---:|---:|
| 34 triangles, no blend | `170 + 102*6 = 782` | `0x0c58` | `0x0cf0` | `0x0d70` | `0x0f90` | `0x70` |
| 34 triangles, blend | `174 + 102*6 = 786` | `0x0c68` | `0x0cf0` | `0x0d70` | `0x0f90` | `0x70` |
| 35 triangles, either form | 105 vertices | beyond safe limit | `0x0d70` | `0x0df0` | `0x1010` | overflow |

Thus every loop chunk remains at most 34 triangles.  The builder writes A24
as 102 vertices for a full chunk (34 triangles), not 816 vertices for the
whole run.  `chrome9_finish_ring()` supplies the immutable STOP trailer and
`chrome9_append_segment()` copies exactly `segment_stop + 0x220`; neither
function needs a size or allocation change.

The ring can wrap between any two chunks.  A retired chunk leaves the reader
at its PAUSE.  On wrap, `chrome9_stop_active_ring()` first publishes PID10
STOP for that exact active tuple; only after it retires may the ring be
refilled and reinstalled.  If that STOP publication cannot be proved retired,
`must_quarantine` is authoritative even though the draw cookie for the prior
chunk was observed.

## Mesa integration checklist

- Copy command `0x0f`, capability bit 22, constants, and the 128-byte structure
  into `include/drm-uapi/chrome9_drm.h`.
- Add the winsys wrapper in `chrome9_drm.[ch]`; no generic raw-submit entry is
  introduced.
- Add `draw_triangle_d16_rhw_run` to `chrome9_context`, set it only from the
  new capability, and retain the old RHW-batch boolean independently.
- Resize only `batch_vertices_rhw[]` to 816 records (16,320 bytes) and add a
  run envelope; the affine array remains 102 records.  The vbuf object is
  heap allocated, so the bounded inline array avoids a second
  failure/lifetime path.
- Select a 272-triangle flush limit only for perspective untextured D16 when
  the run cap is present.  Affine, textured, old-kernel, and non-depth paths
  retain their present limits and ioctls.
- Initialize the complete run state from the first accepted triangle exactly
  as `batch_rhw` is initialized now.  Gallium state is immutable during one
  vbuf callback; initially flush unconditionally at the end of
  `draw_arrays()` or `draw_elements()` rather than crossing callbacks.
- The current vbuf advertises `max_indices=UINT16_MAX` and
  `max_vertex_buffer_bytes=UINT16_MAX * 8 * sizeof(uint32_t)`; it does not
  impose the 34-triangle boundary.  Still retain the triangle-count histogram
  from the performance audit to prove real callbacks fill the larger run.
- On success add the complete count to `submitted_triangles` and publish the
  render-target shadow once.  On failure add no submitted count, set
  `batch_failed`, log once, and never replay through command `0x0c`.
- Preserve `CHROME9_DRAW_LIMIT`: the test against submitted plus queued
  triangles must operate correctly through 272.

The current draw backend already proves that batching need not cross state:
`chrome9_vbuf_draw_arrays()` and `chrome9_vbuf_draw_elements()` reset the
batch at entry and call the flush helper before returning.  The vbuf stage may
invoke either callback multiple times for one Gallium draw, so the first run
implementation must not carry pending vertices across those callback
boundaries.  Consequently no new context-flush, state-bind or CPU-map hook is
needed in this tranche.  If a later optimization crosses callbacks, all of
those hooks become mandatory before that change is enabled.

Use the run iff the current triangle is untextured, D16 is active, the RHW-run
cap is present, and the existing state validator accepted the complete
envelope.  `batch_perspective` must become true for either RHW cap; select
limit 272 and the run wrapper only for the run cap, otherwise retain the
34-triangle RHW-batch fallback.  Affine color/D16 batching stays at 34.
Capacity flushes and callback-end partial flushes both use the selected limit.

On a failed run, add the entire attempted count to `rejected_triangles`, set
`batch_failed`, clear `batch_count`, and reject each later primitive in the
same callback.  Do not try command `0x0c`: the kernel may have retired a
prefix before returning the error.  `CHROME9_DRAW_LIMIT` remains exact with
the existing test `submitted_triangles + batch_count >= draw_limit`; a
capacity flush updates `submitted_triangles` before the next primitive, while
a callback-end partial remains queued in `batch_count` until that flush.

## Benchmark counters and acceptance thresholds

The live baseline is 237 command-`0x0c` calls among 258 ioctls, with
0.383435 seconds in all ioctls and a 584.657-ms frame.  The trace proves no
triangle-count or callback-boundary distribution.  With at most eight old
segments per run, 30 calls is the theoretical floor
`DIV_ROUND_UP(237, 8)`, not a guaranteed result: the actual count is the sum
of `DIV_ROUND_UP(old_batches_in_callback, 8)` over vbuf callbacks.

Add cheap counters before interpreting a benchmark:

| layer | counter | update point |
|---|---|---|
| Mesa | `rhw_run_ioctls`, `rhw_run_triangles`, `rhw_run_failures` | once in the run flush wrapper |
| Mesa | `rhw_run_full`, `rhw_run_callback_partial` | capacity flush versus draw-arrays/elements tail flush |
| Mesa | exact `count[1..272]` histogram | immediately before the ioctl, including failed attempts |
| kernel | `rhw_run_ioctls`, `rhw_run_requested_triangles` | after full UAPI/copy validation, under `submit_lock` |
| kernel | `rhw_run_retired_segments`, `rhw_run_retired_triangles` | after each successful three-phase wait |
| kernel | `rhw_run_failures`, `rhw_run_quarantines` | first run error and quarantine transition respectively |

Keep per-engine kernel totals under `submit_lock`; they require no atomics.
Print them with the existing engine-stop append/wrap totals for the first
oracle.  Keep Mesa counters per context and print only behind a new
disabled-by-default `CHROME9_DEBUG_BATCH` option.  Do not add a telemetry
ioctl or take VGA/VM/reservation locks to read counters.

Acceptance requires: old `0x0c` calls disappear on a run-capable kernel;
Mesa and kernel requested triangle totals agree; kernel retired segments
equal the sum of `DIV_ROUND_UP(count,34)` for successful run calls; bind,
unbind, ownership-round-trip and guard-scan counts become one per run rather
than one per segment; and rendering, rejection totals and frame checksum stay
unchanged.  Report both ioctl reduction and frame time—the first verifies the
patch, while the second includes software VS/clipping and presentation costs.

## Legacy and proprietary batching cross-check

This lifetime is conservative relative to all three recovered predecessors:

- Mesa 7.11.2 defines `VIA_DMA_BUFSIZ=4096` and a 3,968-byte high-water mark
  in `via_context.h`.  `via_render.c` emits entire unclipped vertex-buffer
  runs and retains strip/fan topology.  `viaExtendPrimitive()` in
  `via_ioctl.h` appends until high water, `viaWrapPrimitive()` closes only at
  that boundary, and `via_ioctl.c:fire_buffer()` submits the accumulated DMA
  buffer.  Resource ownership is not rebuilt for every triangle list.
- The proprietary Linux 5.75 `via_drv.so` manager reserves variable dword
  ranges with `cmGetSpace_inv:0xb2730`, advances/aligns them with
  `cmReleaseSpace_inv:0xb2690`, and kicks through
  `cmKickoffDMA_inv:0xb25e0`.  Its resident-ring path
  `cmKickoffRingAGP_inv:0xb0fb0` updates the running PAUSE rather than
  reinstalling resource translations per primitive.
- The Windows D3D and OpenGL normal primitive callbacks at `0x10156b20` and
  `0x102019a0` accept the API primitive range, gather dirty state once through
  `0x10150be0`/`0x101fc050`, and clear emitted dirty records afterward.  Their
  A24 count covers the whole list (`3*p` for a triangle list and `p+2` for a
  strip/fan), with native index/vertex streams available.

Those paths justify retaining mappings/state across a run, but they do not
prove a 272-triangle Chrome9 packet safe under the current one-page staging
allocation.  Repeating the already-live 34-triangle packet up to eight times
is the deliberate intermediate step.  Dirty-state factoring, one larger A24
list, native strip/fan reuse, and indexing remain separate optimizations.

## Required regression and no-wedge gates

1. Compile-time layout checks must pass for native and 32-bit UAPI builds.
2. Direct UAPI calls with counts 0 and 273, null or inaccessible/overflowing
   pointers, every reserved bit, arithmetic edge values, invalid RHW,
   coordinate/Z bounds, and a degenerate triangle must return before
   append/PTE counters change.  A readable userspace pointer need not be
   naturally aligned because the kernel copies records rather than directly
   dereferencing the userspace type.
3. Submit 34, 35, 271, and 272 triangles.  Require append counts 1, 2, 8, and
   8 respectively but exactly one ioctl, bind/unbind pair, ownership round
   trip, and guard scan per run.
4. Compare a 35-triangle run byte-for-byte with old command `0x0c` split as
   34+1.  Put unequal positive RHW and diffuse colors in triangles 34 and 35
   so an affine or stale-record implementation cannot pass.
5. Put overlapping LESS and LEQUAL triangles on opposite sides of a segment
   boundary.  Repeat with `DEPTH_READ_ONLY` and prove color changes while
   every D16 sample remains unchanged.  Require exact equal/far/near values,
   proving that no depth clear, write-mask loss or state loss occurs between
   packets.
6. Repeat with BGRX/BGRA, source-over blend, channel masks, and enabled
   scissor.  Require all four guards and MMIO `040c=0`.
7. Force a ring wrap between segments.  Require cookie alternation and the
   same result; a stale-cookie implementation should be caught here.
8. Inject build/append failure before publication after one retired segment:
   require safe unbind, signaled error fence, no replay, and no later segment.
9. Inject wait timeout after publication: require engine wedge, unsignaled
   fence, retained node/object/ring epoch, and reboot-only recovery.
10. New kernel plus old Mesa must keep command `0x0c`; new Mesa plus old kernel
    must see no cap and do the same.  The complete existing smoke and real
    Gouraud checks remain required after the new run passes its direct oracle.

The legacy Mesa DMA high-water design, proprietary Linux command manager, and
Windows dirty-state/whole-primitive callbacks all support aggregation as the
steady-state architecture.  None overrides the current silicon proof: this
first implementation retains independent-list packets of at most 34 triangles
until larger native A24 lists, strip/fan reuse, or indexed fetch receive their
own bounded live gates.
