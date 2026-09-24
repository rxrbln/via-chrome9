# Chrome9 desktop blur render-to-texture ordering audit

## Scope

The default glmark2 desktop blur uses a chain of private, level-zero BGRA8
color FBOs.  Each blur pass samples the result of an earlier pass.  This note
checks whether the current generic Gallium and kernel paths provide the two
different operations required at that transition:

1. complete and publish the render-target producer; and
2. invalidate the texture consumer's cache before the first fetch.

This is a source and proprietary-binary audit.  It does not claim that the
eleven-tap blur shader has rendered correctly on silicon.

## Current Gallium producer boundary

`chrome9_set_framebuffer_state()` calls `chrome9_draw_flush_batch()` before
releasing the old `render_target` reference or installing the next FBO
(`chrome9_context.c:4388..4408`).  The generic flush copies the retained
vertices and state into `DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM`
(`chrome9_draw.c:1539..1563`).  A changed target, descriptor, fragment
program, constants, blend state, or scissor also ends the retained triangle
batch (`chrome9_draw.c:1286..1336`).

Private color textures use the canonical guarded layout and are bound as the
render target directly.  Only shared/imported offset-zero resources use the
CPU-copy render shadow.  Therefore the desktop blur FBO chain does not need a
CPU shadow copy between ordinary private passes.

The shadow path is nevertheless ordered: framebuffer replacement first
flushes the retained producer and only then copies a dirty guarded target
back to the external image (`chrome9_context.c:47..57,4388..4418`).  An
unguarded sampler shadow similarly flushes retained triangles before the
CPU copy immediately preceding a draw (`chrome9_context.c:4668..4683`).

## Kernel completion is synchronous

The generic fragment ioctl locks every destination, depth, and texture BO,
waits its reservation dependencies, maps all objects, transfers CPU
ownership to the device, and attaches one write/read fence transaction
(`chrome9_engine.c:4726..4857`).

After publishing the command segment it waits, in order, for the exact job
cookie, the owned command-reader PAUSE, and all non-CR engine work bits to
drain.  It then transfers every object back to CPU ownership, verifies guard
pages, releases the SF mappings, and only then returns from the ioctl
(`chrome9_engine.c:4870..4895` and `chrome9_wait_job()`).  Thus the Gallium
framebuffer switch is a completed producer boundary, not merely a userspace
queue flush.

## Consumer texture-cache invalidation

Every generic fragment draw constructs FE configuration from
`default_config` whose A00 is `00000001`; the compiler-active texture-resource
count is ORed into bits `[7:4]` while bit zero remains set
(`chrome9_engine.c:1793..1811`).  Texture images and samplers are emitted
before this type-FE A00 group.  Consequently a one-resource blur consumer
emits A00 `00000011` after binding its source image and before the primitive.

This matches the proprietary Linux OpenGL `via3DBlit` packet at
`via_chrome9_dri.so:0x1f26b1`:

```text
FE020440 0000043C FE020000 CC000000
00000011 01000021 08000101
```

The binary's front-buffer helper distinguishes A00 bit zero from the resource
count: `__glDpSyncFrontBufferToFakeFront:0x237719..0x237749` emits the same
group with A00 `00000010` in its special no-pulse branch.  The normal R2T
consumer requires the `...11` form.  The producer wait and A00 pulse are not
interchangeable; current code performs both in the required order.

The kernel also rejects a destination or depth BO aliased as any source
texture (`chrome9_engine.c:4757..4765`), while Mesa rejects the equivalent
resource/handle feedback loop before submission.  Read-only texture aliasing
is allowed and does not weaken producer ownership.

## Bounded live gate

The first desktop blur run should remain one bounded validation, not a cache
test matrix:

1. use the exact source shader: eleven distinct taps, linear/clamp BGRA8,
   radius five, one horizontal and one vertical pass;
2. use a nonuniform source so each tap contributes a distinguishable result;
3. compare interior and edge samples against a CPU FP32/UNORM reference;
4. require the expected submitted-program counters and no rejected triangles;
5. require intact source/destination guards, a clean engine error register,
   and a known-good draw afterward.

If this shader submits but produces stale pixels, capture the consumer packet
and first check that FE A00 retains bit zero.  If the packet contains the
expected `00000011`, the source-visible ordering contract above is complete;
the next suspects are repeated-TEX scheduling, coordinates, or blend/raster
state, not an absent Gallium framebuffer barrier.

## Evidence boundary

| item | status |
|---|---|
| private guarded BGRA producer flushed on FBO switch | source-exact |
| ioctl waits producer cookie/PAUSE/work drain before return | source-exact |
| one-resource consumer emits FE A00 `00000011` | source-exact |
| A00 bit-zero cache-clear meaning | proprietary Linux packet-exact |
| destination/depth feedback alias rejection | source-exact |
| eleven-tap desktop blur numerical result | pending bounded live gate |
