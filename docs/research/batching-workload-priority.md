# Live workload evidence and batching priority

## Decision

Implement the one-texture retained run (`0x14`, capability bit 30) before the
two-texture run (`0x15`, bit 31).  The live application evidence exercises
all useful one-texture variants heavily, while the retained glmark/ioquake
logs contain no successful application evidence that would quantify a
two-texture win.  Two texture remains important for later lightmaps, but its
current proof is the focused smoke suite rather than a measured bottleneck.

Across all batching work, the untextured D16 RHW run (`0x0f`, bit 22) still
has the largest directly timed bottleneck: one glmark Gouraud frame made 237
Chrome9 batch calls.  If it is not already being implemented, the measured
priority is RHW run, then one-texture run, then two-texture run.

This note separates measured counts from source-derived or projected counts.
No live command was issued to collect it; all data comes from retained logs
on the target and the current Mesa/kernel sources.

## Reproducible input logs

| target file under `/root/tmp/chrome9` | SHA-256 |
|---|---|
| `glmark2-gouraud-strace-v1.txt` | `cce05bfeeab15ef284386c6c5b345e367f2232d4cbc9ed9193aff15e197bb152` |
| `glmark2-gouraud-ioctl-v1.txt` | `c91439976b80b692aae1e0c8b1e3854a7a787ab1c0f3edc719428d5734f89f4b` |
| `glmark2-gouraud-strace-run-v1.log` | `179683b07ef3a179fe16a482d758cef3c0df921ed5b5f77d07e77ce31b54e27b` |
| `glmark2-default-bgra-export-v1.log` | `eea6b333200e3c8894b173d1154c31f7c1c8676f79bea055be8072a1c226e8be` |
| `glmark2-default-texture-capture-v1.log` | `e91406b8fe515da3a495b9facfd192f8dbec299fbfec42bedc2f5bb06430ea85` |
| `glxgears-drawtrace-v1.out` | `f9269fc051d98f01d4a4561743a97a8e599e13cbeab0542962ccf34539e1cc5c` |
| `glxgears-direct-sf-c7.out` | `91f08d61b1bc0e084c3bab89a18ff2f182a52113bf166e20f114899b08794642` |
| `ioquake3-gl1-textured-d16-blend-v1.log` | `570cd1ba01ed4c760431bb45f470f070fa3c73d9743e9e82f6a33d8debd8de4d` |

The older `batch.strace` and `batch2.strace` are rejected pre-fix probe logs:
their single private `0x49` ioctl returns `EINVAL` and the test then faults.
They are not performance evidence.

## Measured workloads

### glmark Gouraud: the synchronous-ioctl control case

The one-frame strace reports 258 total ioctls taking 383.435 ms, 90.92% of
all traced syscall time.  The detailed trace identifies exactly 237 as
Chrome9 private command `0x0c`, payload `0x80`,
`DRAW_TRIANGLE_D16_RHW_BATCH`.  The reported frame time is 584.657 ms, so
time spent inside all ioctls is 65.6% of frame wall time.  A separate ten
frame run reports 478.804 ms/frame, about 3 FPS.

The 1,486 microseconds/call value in the strace summary is averaged over all
258 ioctls, not measured separately for the 237 Chrome9 calls.  Do not quote
it as an exact Chrome9-handler latency.  The evidence does prove that the
synchronous submission path, rather than fragment work alone, dominates the
traced system-call cost.

The eight-segment RHW run has a theoretical state-compatible floor of
`ceil(237/8) = 30` ioctls, an 87.3% call-count reduction.  It intentionally
retains all 237-or-fewer hardware segments and retirement waits; only live
phase counters can predict the resulting wall-time gain.

### glxgears: not a textured-run target

The 50-triangle draw-limit log contains only `kind=color+d16`; it has no
textured draw.  Current source batches that path at 34 triangles, and the log
shows the first submitted count reaching 34 before the limit-tail.  The older
direct-SF run reports about 20.1 FPS, but it combines presentation-era costs
and cannot rank one- versus two-texture batching.

Use glxgears as a regression after the new ioctls, not as their performance
acceptance workload.  Its command count should remain unchanged.

### glmark texture cube: small one-texture latency discriminator

A matched 256x256/depth16 one-frame log from the same Mesa build reports:

| scene | frame time | FPS |
|---|---:|---:|
| untextured `build:model=cube` | 6.045 ms | 166 |
| one-texture `texture:model=cube` | 14.385 ms | 70 |

The measured delta is 8.340 ms.  The standard cube topology is twelve
triangles and the current driver source sends every textured triangle through
a separate synchronous ioctl, whereas the untextured list uses its batch
transport.  The twelve-versus-one submit count is therefore an inference from
topology and driver source, not captured by strace in this log.  A long
texture run reports 37.472 ms/frame (27 FPS),
but presentation and run-duration differences make it a secondary baseline.

The first cap30 performance gate should rerun the one-frame pair with a
Chrome9-ioctl trace and require exactly one `0x14` draw call for the textured
cube's immutable list.  The principal target is a 12-to-1 draw-ioctl
reduction.  As a secondary target over at least five runs, require the median
texture frame time to improve by at least 25% from the matched pre-cap30
baseline; do not fail correctness merely because presentation noise prevents
the secondary threshold.

### ioquake3: one texture is the immediate application workload

The bounded successful trace stops after exactly 1,500 accepted triangles
with no draw-state rejection:

| lowered draw kind | triangles | share |
|---|---:|---:|
| one texture + blend | 1,490 | 99.33% |
| one texture + D16 + blend | 8 | 0.53% |
| textured + D16 | 2 | 0.13% |

The blend rows are definitively one texture: the current
`chrome9_draw_state_supported()` rejects every two-texture blend before
submission.  The first two full-screen D16 triangles are also visually and
sequentially the one-texture startup copy, although their debug string alone
does not encode texture count.

The kind sequence is one 2-triangle D16 run, five long blend runs of
174/318/326/330/342 triangles, and four 2-triangle D16+blend runs.  Grouping
only by kind and ignoring real texture/state/API boundaries gives an
optimistic, non-authoritative cap30 floor of 14 ioctls.  Conversely, the
coordinate stream is dominated by adjacent two-triangle rectangles, so even
callback-local batching should commonly reduce two synchronous calls to one.
Neither observation substitutes for a full state-key/callback histogram.

The same log reports `CL_InitCGame: 6.38 seconds`, but it also prints 1,500
debug lines and performs substantial game loading.  It is not a valid
per-ioctl timing measurement.  Treat 1,500-to-N call count as the primary
metric.

For the first cap30 ioquake gate, replay the same bounded startup and require:

- exactly 1,500 accepted triangles, zero new rejection, identical captured
  image checksum, all guards intact, and engine error `040c=0`;
- no legacy one-texture draw ioctl after cap30 selection;
- at most 750 one-texture run ioctls, proving at least the adjacent-quad
  aggregation rather than only changing the command number; and
- logged state/draw-end/capacity flush reasons and run-size histogram so the
  next target is based on observed boundaries.

The stretch target is at most 100 run ioctls for those 1,500 triangles.  It
is deliberately not an initial correctness condition because texture-handle
and Gallium draw boundaries are missing from the retained debug format.

## Why cap30 must include blend early

A no-blend-only one-texture list is a useful silicon gate and should be the
first live packet.  It is not the application milestone: 1,498 of the 1,500
ioquake triangles use a blend successor.  The implementation order inside
cap30 should therefore be:

1. REPLACE/MODULATE, no depth, no blend, 2/23/24-triangle list;
2. D16 LESS/LEQUAL and read-only, matching the glmark texture cube;
3. no-depth blend/alpha/mask/BGRA, which covers 99.33% of the retained
   ioquake trace;
4. D16 plus blend, which closes the final eight ioquake triangles; and
5. eight-segment retained ownership plus the Mesa outer-draw accumulator.

Do not advertise cap30 after step 1 if its contract accepts the later flag
combinations.  Either keep the capability disabled through step 5 or make
the kernel reject unpromoted combinations and add separate semantic bits.
The single-cap contract in the current design assumes full promotion.

Correctness depends on the already proved normalized D1 diffuse format,
RHW preservation, one-texture REPLACE/MODULATE programs, canonical guarded
layouts, NPOT A22 clearing, destination BGRA state, D16 ownership/A20 state,
LESS/LEQUAL/read-only, normalized blend equations, alpha test, scissor, and
active-scanout resync.  The run changes list length and lifetime only; it must
not create alternative lowering for any of those states.

## Why cap31 follows

No retained glmark/ioquake application log contains a successful classifier
or rejection identifying `fs=19` / `tex=2`.  The exact two-texture NPOT+D16
smoke passed previously, so cap31 has a correctness base, but there is no
measured app call count or frame-time delta to justify implementing it before
cap30.  The next useful evidence is an ioquake world/lightmap trace that logs
texture count, both handles, depth/blend flags, callback boundary, and
candidate run size.

Cap31 also has one extra correctness dependency: no-depth two-texture blend
has no predecessor ioctl.  Its combined program/state needs an independent
live image gate before the capability can truthfully promise all optional
flags.  This makes it strictly higher risk than the one-texture run, whose
blend and D16 compositions already have predecessor oracles.

## Required counter format

Add disabled-by-default Mesa counters before judging the final speedup:

```text
outer_draws
vbuf_callbacks
accepted_triangles[untextured,one_texture,two_texture]
legacy_draw_ioctls[kind]
run_ioctls[kind]
run_segments[kind]
run_triangles histogram: 1,2,3-8,9-24,25-48,49-96,97+
flush_reason: capacity,draw_end,key_mismatch,failure,legacy
key change field: destination,depth,texture0,texture1,program,blend,scissor
```

Kernel counters should report run ioctls, requested triangles, retired
segments, GART bind/unbind count, cache ownership transitions, ring wraps,
and timeout segment.  For a successful run, bind/unbind and device/CPU
ownership transitions must each increment once regardless of segment count.

Report both draw ioctls and hardware segments.  Counting only ioctls can hide
an unchanged segment bottleneck; counting only segments hides the eliminated
GART/cache/fence work that the live strace identifies as the primary target.
