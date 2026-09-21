# Chrome9 synchronous submission bottleneck audit

The first direct-SF Xorg/glamor gate is correct but deliberately slow.  The
2026-09-21 run with kernel `6193bb5` reached about 20.2 FPS in bounded
`glxgears`, with no engine error and a clean display restore.  This note
identifies the work performed by each current draw ioctl and orders the next
performance changes so that none weaken the proven no-wedge boundary.

The main result is that packet construction is not the first bottleneck to
attack.  Untextured primitives already share one state image in batches of up
to 34 triangles.  Each batch nevertheless performs a complete transient SF
bind, whole-object cache ownership round trip, synchronous hardware wait,
guard scan, persistent-scanout resync when applicable, and complete SF
unbind.  On the live 1024x768 color-plus-D16 layout, one batch touches 1,156
PTEs in each direction and explicitly cache-flushes 9,469,952 bytes.  These
costs are invariant with the number and screen area of triangles in that
batch.

The original accounting below was a static source audit.  A later live
Gouraud trace now proves that synchronous ioctl submission dominates traced
syscall time.  It still does not attribute the time among GART publication,
cache ownership, hardware execution, guard readback, and teardown; the kernel
phase counters proposed below remain necessary.

The cross-workload counts and implementation priority are recorded in
[batching-workload-priority.md](batching-workload-priority.md).  The measured
order is the untextured RHW run first, then the one-texture run, then the
two-texture run; 1,498 of 1,500 accepted triangles in the retained ioquake
trace require a one-texture blend envelope.

## Existing batch and command cost

Mesa's `chrome9_vbuf_draw_arrays()` and `chrome9_vbuf_draw_elements()` collect
untextured primitives through `chrome9_emit_triangle()`.  They flush at the
34-triangle ABI limit or at the end of each draw-module primitive call.
Textured triangles still use one synchronous ioctl per triangle.

The exact state-, packet-, and lifetime-complete design for retaining
one-/two-texture triangles is now in
[textured-triangle-run-list-design.md](textured-triangle-run-list-design.md).
Its one-page maxima are 24 and 18 triangles per packet respectively; an
eight-segment resource run carries at most 192 or 144 triangles under one
mapping and fence.

The perspective D16 successor follows the same aggregation path.  When depth
is active and `DRM_CHROME9_CAP_DRAW_TRIANGLE_D16_RHW_BATCH` is present,
`chrome9_emit_triangle()` copies post-viewport X/Y/Z, the original RHW word,
and normalized diffuse color into `batch_vertices_rhw[]`; it does not fall
back to one ioctl per triangle.  `chrome9_vbuf_draw_arrays()` and
`chrome9_vbuf_draw_elements()` nevertheless reset and flush this array at
each draw-module callback boundary.  Batches cannot currently span such a
boundary, a Gallium `draw_vbo`, or a state/resource change.

The kernel's `chrome9_build_depth_triangle()` emits one common H6 state body
followed by six dwords per vertex.  For the maximum non-blended D16 batch:

```text
triangle count                  34
vertex count                    102
dynamic body dwords             170 + 102 * 6 = 782
final PAUSE byte offset          0x0cf0
final STOP byte offset           0x0d70
copied bytes including read-ahead 0x0d70 + 0x220 = 0x0f90
4-KiB staging headroom           0x70 bytes
```

The blended derivative adds four state dwords but reaches the same aligned
PAUSE and STOP addresses.  A 35th triangle would push the STOP plus the
required `0x220`-byte command-reader read-ahead beyond the 4-KiB staging page;
34 is therefore the exact safe limit for this builder, not an arbitrary small
batch.  Enlarging only the UAPI count cannot work without changing staging
and the packet-size proof.

The persistent 64-KiB command ring already avoids a full A60--A68 reinstall on
ordinary submissions: `chrome9_append_segment()` copies one segment, flushes
only that ring interval, and advances A64/A65.  A full-ring flush and install
occur only on wrap.  State factoring and monotonic append are consequently
already doing useful work.

## Measured Gouraud versus build result

The September 21 hardware result localizes the current Gouraud cost.  The
one-frame run used Mesa git `370a43cf5b`, a 256x256 XRGB+D16 drawable, and
completed without an engine error:

```text
glmark2 shading cat, Gouraud     584.657 ms under strace
all ioctls                      258 calls, 0.383435 s
D16_RHW_BATCH ioctl 0x4c        237 calls
ioctl share of traced syscalls  90.92%
ioctl share of benchmark frame  65.58%
```

The 1,486 us/call number in the syscall summary is the average over all 258
ioctls.  It is not a direct duration measurement for each of the 237 draw
calls.  Charging all ioctl time to those draws would give a 1,618-us upper
bound per draw; the other 21 DRM and terminal ioctls make the true value
slightly lower.  Strace itself also perturbs the run.  The authoritative
artifacts and SHA-256 sums are:

```text
/root/tmp/chrome9/glmark2-gouraud-strace-run-v1.log
  179683b07ef3a179fe16a482d758cef3c0df921ed5b5f77d07e77ce31b54e27b
/root/tmp/chrome9/glmark2-gouraud-strace-v1.txt
  cce05bfeeab15ef284386c6c5b345e367f2232d4cbc9ed9193aff15e197bb152
/root/tmp/chrome9/glmark2-gouraud-ioctl-v1.txt
  c91439976b80b692aae1e0c8b1e3854a7a787ab1c0f3edc719428d5734f89f4b
```

This is not a shader-only comparison with the approximately 166-FPS build
result.  The installed 3DS assets have these primary mesh chunks:

| model | vertices (`0x4110`) | faces (`0x4120`) | SHA-256 |
|---|---:|---:|---|
| `cube.3ds` | 20 | 12 | `8d8b716db79445dbd009da40b788b6c3cd4760035ed17f5dd9b53d1050c799b9` |
| `cat.3ds` | 7,340 | 14,348 | `9e535473afd73d3dbeda82e98f32072853cf2966048f69613739813fa4695450` |

The cat therefore begins with 1,195.7 times as many faces as the cube.  The
build result used Mesa git `43199c6ac0`, `model=cube`, `use-vbo=true`, and
reported 6.031 ms; the ten-frame cat result used git `370a43cf5b` and
reported 478.804 ms.  Their 79.39-times frame-time ratio is directional, not
a controlled performance comparison, because the Mesa revisions and scenes
differ.

The current maximum is exactly 34 triangles, not 64.  The ioctl encoding
contains byte `0x64` because that is the Linux DRM ioctl type, while `0x4c` is
the Chrome9 command number; neither byte is a triangle count.  If all 14,348
cat faces survived software clipping and culling, one frame would require
exactly 422 full 34-triangle batches.  The 237 observed calls can contain no
more than 8,058 submitted triangles.  That is consistent with face culling
and clipping, but ioctl count alone cannot recover the exact triangle count
because Mesa also flushes at every draw-module callback boundary.  A
triangle-count histogram is still required.

For a canonical 256x256 render target the fixed per-call memory work is also
substantial:

```text
XRGB color object     1024 * 256 payload + two guards = 270,336 B = 66 pages
D16 depth object       512 * 256 payload + two guards = 139,264 B = 34 pages
combined transient mapping                              409,600 B = 100 pages
two explicit cache passes per call                      819,200 B
237 calls                                             194,150,400 B = 185.16 MiB
PTE publication plus invalidation                    47,400 PTE writes
SR6F translation-cache flushes                           474
```

Those totals exclude SG-table DMA ownership operations, guard reads, command
copying, and any active-scanout resynchronization.  Conversely, eliminating
all ioctl time from the straced frame would still leave 201.222 ms.  That is
not a stable CPU profile, but it proves the important planning point: batching
can plausibly improve this scene by a multiple, while software VS, clipping,
14,348-face processing, raster work, and X presentation prevent it from
approaching the 12-face cube result by batching alone.

## Exact per-batch memory work

For the live native front buffer:

```text
XRGB color payload              4096 * 768 = 3,145,728 bytes
color object with two guards    3,153,920 bytes = 770 pages
D16 payload                     2048 * 768 = 1,572,864 bytes
depth object with two guards    1,581,056 bytes = 386 pages
combined transient SF range     4,734,976 bytes = 1,156 pages
```

`chrome9_submit_depth_triangle()` performs all of the following for every
batch, even a single tiny triangle:

1. look up both GEM handles, rebuild arrays for all 1,156 cached PTEs, lock
   both reservations, and vmap both complete objects;
2. allocate one contiguous `drm_mm` node and publish all 1,156 PTEs with one
   bounded SR6F translation-cache flush;
3. rewrite the two guards on each object;
4. call `chrome9_cache_flush()` over all 3,153,920 color bytes and all
   1,581,056 depth bytes, then sync both complete SG tables for device;
5. append the packet and poll the cookie, CR PAUSE, and engine-work idle
   synchronously while still holding `submit_lock`;
6. sync both complete SG tables for CPU, explicitly cache-flush the same
   4,734,976 bytes again, and scan the guards;
7. because color is the active direct-scanout object, sync its persistent SG
   table back for device ownership;
8. invalidate all 1,156 transient PTEs, perform a second SR6F flush, remove the
   node, signal the already-retired fence, vunmap, and return.

The two explicit `chrome9_cache_flush()` passes alone cover:

```text
2 * (3,153,920 + 1,581,056) = 9,469,952 bytes per batch
```

That number does not include the architecture-specific work inside the four
normal SG-table ownership calls or the additional full-color
`chrome9_sf_sync_for_device()` required by the current front-buffer bridge.
On a SWIOTLB system the ownership calls may also copy through bounce storage;
they must be measured, not assumed free.

The destination simultaneously retains its complete 770-page persistent
scanout alias at SF `0x10000` while each render batch creates a second
temporary alias.  Both use PTE bit 25, so the tested path is coherent, but the
duplicate address and page-table churn have no production benefit.

## Authoritative low-risk measurement

Measure before changing ownership rules.  A bounded Xorg run can identify the
batch rate without modifying or probing hardware registers:

```sh
strace -f -ttT -e trace=ioctl -o /root/tmp/chrome9/gears-ioctl.trace \
  timeout 10s glxgears
```

The affine batch ioctl has DRM command number `0x49`, with x86 ioctl value
`0x40806449`.  The perspective D16/RHW successor is command `0x4c`, value
`0x4080644c`; this is the one seen 237 times in the Gouraud trace.  Count each
separately from KMS, GEM, and sync ioctls.  Record elapsed time, rendered
frames, batch count, and the distribution of syscall duration.  Do not infer
triangles per batch from ioctl count: Mesa flushes at both the 34-triangle
limit and every draw-module primitive boundary.

For phase attribution, add counters/timestamps behind a disabled-by-default
kernel debug option, rather than changing execution:

```text
submit count and requested triangle count
destination/depth/texture page counts
GART bind and unbind count, PTE count, and elapsed time
bytes passed to explicit cache flush
SG sync-for-device / sync-for-CPU count and elapsed time
cookie, PAUSE, and work-idle poll elapsed time
guard verification elapsed time
ring copied bytes, wrap count, and epoch
active-scanout resync count
```

Use monotonic kernel time and per-engine `u64` totals under `submit_lock`.
Expose a read-only debugfs snapshot only after ensuring it takes no VGA, VM,
reservation, or submit lock.  The existing engine-stop log provides only
aggregate append/wrap counts at teardown and cannot attribute live cost.

Add a userspace-side histogram at the actual ioctl boundary as well:

```text
draw_vbo calls and source index/vertex counts
draw-module callback count and topology
surviving/decomposed triangles
RHW batch count, triangle-count histogram, and partial-batch reason
```

This distinguishes culling from callback fragmentation and verifies that a
larger run ABI would actually be filled before changing the kernel.

## What the vendor and legacy paths batch

The old implementations do not establish that one larger Chrome9 packet is
safe, but they independently show that per-34-triangle resource ownership is
not the intended steady-state submission model.

The Mesa 7.11.2 UniChrome driver has a 4,096-byte DMA buffer and a 3,968-byte
high-water mark (`VIA_DMA_BUFSIZ` and `VIA_DMA_HIGHWATER`).  Its fast-render
stage explicitly emits an entire unclipped vertex-buffer run and retains
native strip/fan topology where possible.  `viaExtendPrimitive()` continues
adding vertices until the high-water boundary; only then does
`viaWrapPrimitive()` finish the current primitive and flush.  `fire_buffer()`
submits the accumulated buffer, not one ioctl per triangle.  This is an older
engine and command ABI, but it is a useful software-architecture precedent.

The proprietary Linux 5.75 Chrome9 path goes further: `cmGetSpace_inv`
reserves a variable dword range in a persistent AGP/ring/BCI manager,
`cmReleaseSpace_inv` aligns and advances it, and the ring path updates only
A64/A65 while the ring remains resident.  It does not rebuild SF mappings and
round-trip complete render objects at every primitive-list boundary.

The Windows 7122 D3D and OpenGL paths also receive a complete API primitive
range.  Their shared callback gathers dirty state once, serializes only dirty
ranges, and emits the primitive list.  A24 is the complete vertex/index count:
triangle list `3*p`, strip/fan `p+2`.  Native strip/fan reuse and indexed
streams avoid the modern bring-up driver's forced expansion to independent
three-vertex lists.  These paths support two later goals—resident state and
native topology/indexing—but importing either before bounded immediate-list
batching is proven would add avoidable front-end risk.

## Safe optimization order

### 1. Add one bounded multi-segment RHW run, without enlarging a packet

The safest concrete aggregation change is a new capability and ioctl, leaving
the proven `0x4c` ABI untouched.  Initially accept at most eight current
segments.  The field-, lifetime-, failure-, and test-exact implementation is
specified in [d16-rhw-run-ioctl-design.md](d16-rhw-run-ioctl-design.md):

```text
maximum triangles       8 * 34 = 272
RHW vertex copy         272 * 3 * 20 = 16,320 bytes
hardware packet size    unchanged, at most 34 triangles each
resource/state key      exactly one color BO, D16 BO, dimensions, pitches,
                        format, depth/blend/scissor state for the whole run
```

The kernel must copy and validate the complete vertex array before any PTE or
ring publication.  It then acquires reservations, vmaps, publishes PTEs,
prepares guards, and transitions ownership exactly once.  While retaining
that one job and mapping, it loops over at most eight chunks and calls the
unchanged packet builder with at most 34 triangles per chunk.  It appends and
waits for each chunk before reusing the one-page staging buffer.  Thus the
known `STOP + 0x220` read-ahead proof and every RHW/depth state word remain
unchanged.

Each chunk needs a fresh alternating cookie.  Reusing the first cookie would
let the second wait observe stale completion and tear down a live mapping.
Update the engine's last-cookie state only after each proved retirement.  The
single job fence represents the whole run and signals only after the final
chunk, final CPU transition, guard scan, and unbind.  A timeout after any
published chunk quarantines the mapping and reachable ring epoch exactly as
the current path does; it never tries to unwind because earlier chunks may
have rendered.

Mesa can fill the new run only within one existing `draw_arrays` or
`draw_elements` callback at first.  It must still flush on callback end and
must not invent cross-draw state snapshots.  The current source never leaves
a batch pending beyond either callback, so state/resource changes, context
flush, CPU map, draw limit and vbuf release need no new hook in this first
tranche.  Crossing callbacks later would require every such hook.

The observed 237 old batch calls have a theoretical floor of 30 run calls
(`DIV_ROUND_UP(237,8)`), not a proven upper bound: actual run count is the sum
of `DIV_ROUND_UP(old_batches_in_callback,8)` over callbacks and can remain as
high as 237 if every callback is short.  A Mesa callback/count histogram is
therefore a required acceptance counter.  Every group that does coalesce
reduces its transient map/cache/guard ownership rounds from one per old batch
to one per run.  The old ABI remains the automatic fallback.

Do not implement this by accepting one 272-triangle command packet or merely
raising `DRM_CHROME9_TRIANGLE_BATCH_MAX_TRIANGLES`: either would exceed the
proved one-page staging/read-ahead boundary.  After the eight-segment oracle
is stable, a 16-segment/544-triangle cap is a separate measured extension.

### 2. Make bring-up guard readback optional, but default it on first

The unconditional post-job CPU transition and guard scan are diagnostic
policy, not a hardware retirement requirement.  Once counters confirm their
cost, add an explicit debug/validation mode:

- validation mode preserves today's sync-for-CPU, explicit flush, guard scan,
  and immediate active-scanout resync exactly;
- production mode leaves completed GPU-written objects device-owned and
  relies on reservation fences plus explicit CPU-map/readback transitions;
- no capability bit changes merely because the diagnostic scan is skipped;
- every timeout, error, or unproved retirement still quarantines all reachable
  mappings exactly as today.

Do not implement this as `sync_for_cpu()` immediately followed by
`sync_for_device()`.  On SWIOTLB that can copy stale bounce contents.  A BO
must have one explicit ownership state and transition only when a real CPU
access requires it.

### 3. Replace per-framebuffer and per-job aliases with one object mapping

The generic shmem object currently has no Chrome9-private mapping field, while
`struct chrome9_framebuffer` owns a framebuffer-local mapping and each render
job allocates another.  Introduce a driver GEM wrapper or an engine-owned
object-to-mapping table with reference-counted roles:

```text
mapping refs = render job + framebuffer prepare + active display + quarantine
mapping fields = GEM ref, cached SGT, immutable PTE image, drm_mm node, state
```

Bind the complete object once and retain the node until its last safe role is
released.  Use independent addresses in command builders:

```text
render target = destination_mapping.start + PAGE_SIZE
depth target  = depth_mapping.start + PAGE_SIZE
texture N     = texture_mapping[N].start + PAGE_SIZE
IGA1 base     = destination_mapping.start + fb->offsets[0]
```

No known packet requires these objects to occupy one contiguous SF node;
today's builders already accept separate target/depth/texture addresses and
only `chrome9_submit_depth_triangle()` manufactures contiguity.  Stable
object mappings remove per-ioctl PTE allocation, publication, invalidation,
two SR6F pulses, and the duplicate scanout alias.

The conversion must preserve current lock order, full-object PTE installation,
bit-25 attributes, and reboot-scoped quarantine.  A map lookup must never let
two concurrent creators bind the same object twice, and engine teardown must
refuse GART restoration while any mapping role remains.

### 4. Preserve device ownership across GPU-only dependencies

With stable mappings, rendering, texturing, and display are same-device
operations.  Keep an object device-owned from producer submission through
render retirement and scanout or sampling.  The separate mechanisms remain:

- reservation fence and cookie/PAUSE/work-idle prove producer retirement;
- A00 texture-cache invalidation handles render-target-to-texture hazards;
- SR6F is used only after PTE changes;
- DMA CPU/device synchronization is used only across actual CPU access;
- vblank plus 0204/0214 prove display latch.

This removes whole-object cache work from ordinary GPU-to-GPU and
GPU-to-display handoffs without guessing a new hardware flush packet.

### 5. Add asynchronous retirement only after persistent mapping ownership

The ioctl currently waits for hardware completion and signals its fence before
return.  True asynchronous submission needs a retirement source, pending-job
queue, and ownership which outlives the ioctl.  Do not return early while a
stack-local/transient SF node may be removed.

A command-reader/event interrupt must be recovered and proven separately from
the display-vblank interrupt.  Until then, a bounded worker/polling retirement
queue can reduce caller serialization only if it owns every job, ring range,
mapping reference, and reservation fence until exact retirement.  Any timeout
quarantines the published ring epoch and all mappings reachable from it.

### 6. Revisit native topology, indexing, and cross-draw aggregation last

After the bounded multi-segment run, stable mappings, and device ownership
remove fixed work, measure again.  Possible later changes are a larger staging
buffer/ring segment, batching across draw-module primitive boundaries when all
state and resources are identical, native strip/fan/indexed submissions, and
textured multi-triangle batches.  Each requires new UAPI bounds and
packet-size assertions.  Cross-draw aggregation also requires a complete
immutable key covering resource handles and layouts, depth/blend/scissor and
shader class, plus a deep copy of transient draw-module vertices.  Simply
raising 34 is unsafe.

## No-wedge validation sequence

Apply only one optimization boundary at a time:

1. Run the existing 32x32 guarded color/D16/blend/texture/two-texture oracles
   in validation mode and require byte-identical results and guards.
2. For the multi-segment run, compare one new run byte-for-byte with the same
   triangles submitted through old `0x4c` calls.  Exercise 34, 35, 271, and
   272 triangles; reject 273 before publication.  Put overlapping LESS and
   LEQUAL triangles on opposite sides of the 34/35 boundary, and use unequal
   RHW values and diffuse colors there so stale depth or affine interpolation
   cannot pass.  Require one final guard check and `040c=0`.
3. Inject a timeout in chunk two.  The job fence must report error and the
   complete mapping/ring epoch must remain quarantined; no later chunk may be
   published.  Inject validation and allocation failures before chunk one and
   require a complete unwind.
4. Run 1024x768 Xorg direct scanout; require the same FB offset/base proof,
   `040c=0`, no new busy status, and confirmed restore.
5. For stable mappings, record PTE indices and prove they do not change across
   at least 1,000 draws.  Ordinary draws must cause zero SR6F pulses; final
   release must invalidate every page exactly once.
6. Alternate CPU upload/readback and GPU draw to exercise each ownership edge;
   include SWIOTLB if the target uses it.
7. With two buffers, render only the inactive object, retire, flip, and release
   the old display reference only after the exact vblank/latch predicate.
8. Inject failures before PTE publication, after publication, after command
   append, and during final unbind.  Prepublication failures unwind; every
   unproved reachable state remains quarantined for reboot.

The 20.2-FPS result is therefore a successful correctness baseline, not a GPU
throughput limit.  The next evidence should distinguish fixed submission
overhead from actual raster time before any capability or shader work is
optimized around that number.
