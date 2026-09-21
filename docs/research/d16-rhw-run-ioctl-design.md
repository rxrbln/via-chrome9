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

This design is based on kernel Chrome9 commit
`f39a90d6a16b16066dfbd53ee1674f9336d12069`, UAPI commit
`b623464ba08e02e39475e8cd67ccc31eeb75a67e`, and Mesa Chrome9 commit
`91cd42a783ce8770ce23bcc6312c7550cd62951b`.  The private-number assignments
come from [chrome9-uapi-allocation.md](chrome9-uapi-allocation.md).

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

The run initially accepts exactly the state accepted by the old perspective
batch:

- `DEPTH` is mandatory; `BLEND` is optional and all other unknown flag bits
  are rejected;
- a depth-function field is accepted only by the existing bounded helper;
- width and height are each `1..2048`;
- destination pitch is exactly `ALIGN(width * 4, 32)` and D16 pitch exactly
  `ALIGN(width * 2, 32)`;
- destination format is BGRX8888 or the already capability-gated BGRA8888
  blend form;
- disabled blend requires format zero and an all-zero blend structure;
- scissor uses the existing half-open validator;
- all padding and reserved words are zero.

Future read-only depth flag bit 11 is not implicit.  The run accepts it only
after capability bit 23 and the common A01 lowering are implemented for this
ioctl too.

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
the cross-product term is at most `2^54` and fits signed 64-bit arithmetic.
Pointer advancement uses vertex-record units and must assert
`first <= total`, `chunk <= total-first`, and `first*3 <= vertex_count`
before adding to the copied-array base.

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
| PTE bind or packet build before the first append | no | invalidate any bound PTEs, signal attached fence with error, normal unwind |
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

## Mesa integration checklist

- Copy command `0x0f`, capability bit 22, constants, and the 128-byte structure
  into `include/drm-uapi/chrome9_drm.h`.
- Add the winsys wrapper in `chrome9_drm.[ch]`; no generic raw-submit entry is
  introduced.
- Add `draw_triangle_d16_rhw_run` to `chrome9_context`, set it only from the
  new capability, and retain the old RHW-batch boolean independently.
- Give `chrome9_vbuf_render` storage for 272 triangles, or 816 RHW vertex
  records (16,320 bytes), and a run envelope.  The vbuf object is heap
  allocated, so an inline array is bounded and avoids a second
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
   boundary.  Require exact equal/far/near color and D16 values, proving that
   no depth clear or state loss occurs between packets.
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
