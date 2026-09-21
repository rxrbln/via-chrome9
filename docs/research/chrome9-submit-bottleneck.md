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
guard scan, persistent-scanout resync, and complete SF unbind.  On the live
1024x768 color-plus-D16 layout, one batch touches 1,156 PTEs in each direction
and explicitly cache-flushes 9,469,952 bytes.  These costs are invariant with
the number and screen area of triangles in that batch.

This is a static source audit, not a claim that any one operation consumes a
measured percentage of frame time.  The first follow-up must count ioctls and
time the phases independently.

## Existing batch and command cost

Mesa's `chrome9_vbuf_draw_arrays()` and `chrome9_vbuf_draw_elements()` collect
untextured primitives through `chrome9_emit_triangle()`.  They flush at the
34-triangle ABI limit or at the end of each draw-module primitive call.
Textured triangles still use one synchronous ioctl per triangle.

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

The batch ioctl has DRM command number `0x49`; with the current 128-byte
structure its x86 ioctl value is `0x40806449`.  Count this separately from
KMS, GEM, and sync ioctls.  Record elapsed time, rendered frames, batch count,
and the distribution of syscall duration.  Do not infer triangles per batch
from ioctl count: Mesa flushes at both the 34-triangle limit and every
draw-module primitive boundary.

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

## Safe optimization order

### 1. Make bring-up guard readback optional, but default it on first

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

### 2. Replace per-framebuffer and per-job aliases with one object mapping

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

### 3. Preserve device ownership across GPU-only dependencies

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

### 4. Add asynchronous retirement only after persistent mapping ownership

The ioctl currently waits for hardware completion and signals its fence before
return.  True asynchronous submission needs a retirement source, pending-job
queue, and ownership which outlives the ioctl.  Do not return early while a
stack-local/transient SF node may be removed.

A command-reader/event interrupt must be recovered and proven separately from
the display-vblank interrupt.  Until then, a bounded worker/polling retirement
queue can reduce caller serialization only if it owns every job, ring range,
mapping reference, and reservation fence until exact retirement.  Any timeout
quarantines the published ring epoch and all mappings reachable from it.

### 5. Revisit command aggregation last

After stable mappings and device ownership remove fixed per-ioctl memory work,
measure again.  Possible later changes are a larger staging buffer/ring
segment, batching across draw-module primitive boundaries when all state and
resources are identical, and textured multi-triangle batches.  Each requires
new UAPI bounds and packet-size assertions.  Simply raising 34 is unsafe, and
it does nothing for the present whole-object ownership cost.

## No-wedge validation sequence

Apply only one optimization boundary at a time:

1. Run the existing 32x32 guarded color/D16/blend/texture/two-texture oracles
   in validation mode and require byte-identical results and guards.
2. Run 1024x768 Xorg direct scanout; require the same FB offset/base proof,
   `040c=0`, no new busy status, and confirmed restore.
3. For stable mappings, record PTE indices and prove they do not change across
   at least 1,000 draws.  Ordinary draws must cause zero SR6F pulses; final
   release must invalidate every page exactly once.
4. Alternate CPU upload/readback and GPU draw to exercise each ownership edge;
   include SWIOTLB if the target uses it.
5. With two buffers, render only the inactive object, retire, flip, and release
   the old display reference only after the exact vblank/latch predicate.
6. Inject failures before PTE publication, after publication, after command
   append, and during final unbind.  Prepublication failures unwind; every
   unproved reachable state remains quarantined for reboot.

The 20.2-FPS result is therefore a successful correctness baseline, not a GPU
throughput limit.  The next evidence should distinguish fixed submission
overhead from actual raster time before any capability or shader work is
optimized around that number.
