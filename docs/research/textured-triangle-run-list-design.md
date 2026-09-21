# Bounded one- and two-texture list/run batching

## Result

Chrome9 can reduce synchronous textured draw submissions without changing
the shader, resource layout, or command-reader safety proof.  The smallest
successor combines two independent improvements behind one new ABI per
texture count:

1. put several independent triangles in one native immediate-list packet by
   changing only A24 and appending post-viewport vertices; and
2. execute at most eight such packets while one `drm_exec` transaction, one
   reservation fence, one SF mapping, and one cache-ownership interval hold
   all resources.

Reserve private command `0x14` / capability bit 30 for one texture and
private command `0x15` / capability bit 31 for two textures.  The namespaces
are independent and follow the canonical assignments in
[chrome9-uapi-allocation.md](chrome9-uapi-allocation.md).  This design does
not resize or reinterpret any existing ioctl.

The exact one-page limits are 24 triangles per one-texture packet and 18
triangles per two-texture packet.  Eight segments therefore bound one ioctl
to 192 or 144 triangles respectively.  Depth, blend, alpha test, scissor,
and depth-read-only state are immutable over the run.  A resource or state
change ends the Mesa run.

This is a static, patch-ready design against kernel Chrome9 `f56ac02` and
Mesa Chrome9 `d7c2570`.  The multi-vertex textured packet still needs a
bounded silicon gate before either capability is advertised.

## Independent vendor evidence

The three vendor paths agree on the batching model even though none proves
the new packet safe on its own:

- Windows D3D dispatcher `VTGUModeDX32A.dll:0x101f7410` selects the triangle
  list builder at `0x101f5350`; OpenGL uses the byte-matching pair
  `VTGOGL32A.dll:0x1024be90 -> 0x10249dd0`.  Their primitive builders set A24
  to the complete list vertex count, `3 * triangle_count`, rather than firing
  once per triangle.
- The normal Windows draw callbacks at D3D `0x10156b20` and OpenGL
  `0x102019a0` collect dirty state once through `0x10150be0` and `0x101fc050`
  before emitting the primitive range.  Texture and destination state are
  not rebuilt per triangle when the draw state is unchanged.
- Proprietary Linux 5.75 uses `cmGetSpace_inv:0xb2730`,
  `cmReleaseSpace_inv:0xb2690`, and `cmKickoffDMA_inv:0xb25e0` around a
  variable-sized command allocation.  Its resident-ring path at `0xb0fb0`
  likewise advances the ring instead of remapping render objects for each
  primitive.
- Mesa 7.11.2 UniChrome keeps extending a primitive until its 3,968-byte DMA
  high-water mark and only then wraps and submits.  That is older hardware,
  but independently supports retaining state and resource ownership across
  a bounded vertex run.

The current Chrome9 bring-up path remains the encoding authority.  In
particular, the successor retains command-mode-independent triangle lists:

```text
A20 = 20000000
A23 = 23004f02
A24 = 24000000 | (3 * chunk_triangle_count)
```

It must not inherit the vendor template's `201c5c00` fan state.  No strip,
fan, index fetch, or vertex-buffer DMA is introduced here.

## Exact packet accounting

### One texture

The native one-texture record is eight dwords:

```text
X Y Z RHW diffuse specular S T
```

The current one-triangle bodies are 224 dwords for REPLACE and 228 for
MODULATE.  The triangle contributes 24 dwords, so for `T` triangles the
builder invariant is:

```text
body_dw = 200 + 4*MODULATE + 4*BLEND + 4*DEPTH + 24*T
```

`MODULATE`, `BLEND`, and `DEPTH` are zero or one.  Depth-read-only changes
the closed A01 word but contributes no dwords.  At `T=24`, body length ranges
from 776 to 788 dwords.  Every variant aligns to final PAUSE `0x0cf0`, STOP
`0x0d70`, and copied/read-ahead end `0x0f90`.  The remaining staging headroom
is `0x70` bytes.

At `T=25`, body length ranges from 800 to 812 dwords.  Every variant crosses
to STOP `0x0df0`, whose required `0x220`-byte command-reader read-ahead ends
at `0x1010`.  It is therefore invalid for the 4-KiB staging page.  Twenty-four
is the exact safe maximum, not a policy choice.

The builder must not create `u32 vertices[24 * 3 * 8]` on the kernel stack.
Emit the immediate-data H2 header with the checked payload count, then stream
each validated and converted record directly into the staging page.  Copy
`raster_fvf[]` to a small local array and replace only its A24 word with the
checked vertex count.  Retain exact assertions for the body length, A20,
A23, A24, immediate header, PAUSE, STOP, and read-ahead end.

### Two textures

The two-texture record is ten dwords:

```text
X Y Z RHW diffuse specular S0 T0 S1 T1
```

The legacy fixed RGBA-MODULATE body is 262 dwords for one triangle, of which
30 are vertex payload.  Primary-color successors add one or two four-dword
ALUs.  The state-complete derivatives are:

```text
program 0, two factor:         body_dw = 232 + 4*BLEND + 4*DEPTH + 30*T
program 1, full primary:       body_dw = 236 + 4*BLEND + 4*DEPTH + 30*T
program 2, TEX0-alpha primary: body_dw = 240 + 4*BLEND + 4*DEPTH + 30*T
```

The 262/266 no-depth/depth rows come directly from the current executable
builder.  The additional four-dword BLEND closure is the separately audited
`0x13` state-complete successor and is still a reservation, not merged/live
evidence.  Keeping it in the worst-case proof prevents that successor from
silently reducing the list margin when implemented.

At `T=18`, body length ranges from 772 through 788 dwords.  Every variant
again lands at PAUSE `0x0cf0`, STOP `0x0d70`, and read-ahead end `0x0f90`.
At `T=19`, the range is 802 through 818 dwords and STOP becomes `0x0df0`,
ending the read-ahead at `0x1010`.  Eighteen is therefore the exact maximum.

Use the same direct-to-staging conversion and local FVF copy as the
one-texture builder.  The program-specific fixed size is part of the
immutable builder key and must be asserted.  A future stage-0 H3 mip table
adds 12 dwords; the largest program-2 depth+blend+mip key is then 800 dwords
at T=18 and must reduce its segment maximum to 17.  The first command-15 ABI
carries no mip metadata.

### Generated bounds table

The implementation should encode the proof as immutable keyed data rather
than a loose limit:

| path | native dwords/vertex | fixed body dwords | maximum triangles | maximum body | PAUSE | STOP |
|---|---:|---:|---:|---:|---:|---:|
| one REPLACE | 8 | 200 | 24 | 776 | `0xcf0` | `0xd70` |
| one MODULATE | 8 | 204 | 24 | 780 | `0xcf0` | `0xd70` |
| one REPLACE + depth + blend | 8 | 208 | 24 | 784 | `0xcf0` | `0xd70` |
| one MODULATE + depth + blend | 8 | 212 | 24 | 788 | `0xcf0` | `0xd70` |
| two texture, program 0 | 10 | 232 | 18 | 772 | `0xcf0` | `0xd70` |
| two texture, program 0 + depth + blend | 10 | 240 | 18 | 780 | `0xcf0` | `0xd70` |
| two texture, program 1 + depth + blend | 10 | 244 | 18 | 784 | `0xcf0` | `0xd70` |
| two texture, program 2 + depth + blend | 10 | 248 | 18 | 788 | `0xcf0` | `0xd70` |

All rows start the aligned 3D body at byte `0x20`.  The common post-body
status, flush, cookie, PAUSE/STOP framing is unchanged.  The builder must
calculate and assert the actual positions; the table is not permission to
omit runtime bounds checks.

## Proposed UAPI

Use one fixed state envelope and one copied userspace vertex array per run.
The flags are deliberately common to both requests:

```c
#define DRM_CHROME9_TEXTURE_RUN_DEPTH       (1U << 0)
#define DRM_CHROME9_TEXTURE_RUN_BLEND       (1U << 1)

#define DRM_CHROME9_TWO_TEXTURE_PROGRAM_MODULATE            0U
#define DRM_CHROME9_TWO_TEXTURE_PROGRAM_PRIMARY_MODULATE    1U
#define DRM_CHROME9_TWO_TEXTURE_PROGRAM_TEX0_ALPHA_PRIMARY  2U

#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_RUN      0x14
#define DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_RUN  0x15
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_RUN      (1ULL << 30)
#define DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_RUN  (1ULL << 31)

#define DRM_CHROME9_TEXTURE_RUN_MAX_SEGMENTS          8U
#define DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES        192U
#define DRM_CHROME9_TWO_TEXTURE_RUN_MAX_TRIANGLES    144U

struct drm_chrome9_draw_textured_triangle_run {
        __u32 destination_handle;       /* 0 */
        __u32 depth_handle;             /* 4: zero without DEPTH */
        __u32 flags;                    /* 8 */
        __u32 triangle_count;           /* 12: 1..192 */
        __u32 program;                  /* 16: REPLACE/MODULATE */
        __u32 width;                    /* 20 */
        __u32 height;                   /* 24 */
        __u32 destination_pitch;        /* 28 */
        __u32 depth_pitch;              /* 32: zero without DEPTH */
        __u32 destination_format;       /* 36 */
        struct drm_chrome9_texture_state texture; /* 40..75 */
        struct drm_chrome9_blend_state blend;     /* 76..115 */
        struct drm_chrome9_scissor_state scissor; /* 116..127 */
        __u64 vertices_ptr;             /* 128 */
        __u64 reserved[3];              /* 136..159: zero */
};

struct drm_chrome9_draw_two_textured_triangle_run {
        __u32 destination_handle;       /* 0 */
        __u32 depth_handle;             /* 4: zero without DEPTH */
        __u32 flags;                    /* 8 */
        __u32 triangle_count;           /* 12: 1..144 */
        __u32 width;                    /* 16 */
        __u32 height;                   /* 20 */
        __u32 destination_pitch;        /* 24 */
        __u32 depth_pitch;              /* 28: zero without DEPTH */
        __u32 destination_format;       /* 32 */
        __u32 program;                  /* 36: semantic program enum */
        struct drm_chrome9_texture_state textures[2]; /* 40..111 */
        struct drm_chrome9_blend_state blend;     /* 112..151 */
        struct drm_chrome9_scissor_state scissor; /* 152..163 */
        __u32 pad;                      /* 164: zero */
        __u64 vertices_ptr;             /* 168 */
        __u64 reserved[3];              /* 176..199: zero */
};
```

The structures are exactly 160 and 200 bytes on 32- and 64-bit userspace.
Add static size and `offsetof(vertices_ptr)` assertions to both kernel and
Mesa UAPI copies.  On x86 their complete ioctl values are respectively
`0x40a06454` and `0x40c86455`.

Do not add four separate no-depth/depth commands.  Optional depth and blend
are closed state envelopes in these successors, not arbitrary packet state.
Conversely, do not combine one and two textures into a variable-size generic
resource array: separate structures keep the object roles and validation
fixed.

## Complete validation order

All public values and all copied vertices must validate before GEM lookup,
PTE publication, or command construction:

1. Reject unknown flags and all nonzero reserved/pad words.  The allowed
   mask is `DEPTH | BLEND | DEPTH_FUNC_MASK | DEPTH_READ_ONLY`.  For the
   two-texture request validate the separate `program` enum as described in
   step 5; never interpret flags bits 0/1 as a program selector.
2. Require `triangle_count` in `1..192` or `1..144` and a nonzero
   `vertices_ptr`.  Check multiplication by three and by the exact record
   size before `memdup_array_user()`.
3. Require dimensions in `1..2048`, destination pitch
   `ALIGN(width * 4, 32)`, and checked guarded object size.  With DEPTH,
   require canonical D16 pitch `ALIGN(width * 2, 32)`, a nonzero distinct
   handle, and explicit LESS or LEQUAL.  Without DEPTH, the depth handle,
   pitch, depth function, and read-only bit are zero.
4. Without BLEND, require BGRX/zero destination format and an all-zero blend
   structure.  With BLEND, reuse `chrome9_validate_blend_state()` and its
   capability-gated extended factors, alpha test, masks, and canonical
   blend-disabled ONE/ZERO state.  This envelope also permits BGRA copy with
   blending disabled.  Dither remains observationally accepted; it is not a
   reason to reject GL's default state.
5. Reuse the current level-zero texture validator independently for every
   source: nonzero distinct native handle, `1..2048` dimensions, canonical
   `ALIGN(width * 4, 32)` pitch, BGRX/BGRA, nearest/linear, and
   clamp/repeat/mirrored-repeat.  One texture accepts REPLACE or MODULATE.
   Two-texture `program` accepts 0 unconditionally, 1 only with cap25, and 2
   only with cap39 and the initial BGRA-unit-1 gate; reject every other value.
   Program bits never share the command-15 flags word.
6. Validate the half-open scissor with the existing helper and destination
   dimensions.
7. Reject every object alias and imported object.  Check each exact guarded
   allocation and the overflow-checked sum against
   `SZ_64M - CHROME9_RING_SIZE`.
8. For every vertex, reuse signed 16.16 screen-coordinate bounds, Z in
   `[0,1]`, normalized diffuse/specular interpretation, and
   `chrome9_rhw_valid()`.  That helper excludes zero, subnormal, infinity,
   NaN, and negative RHW.  One-texture S/T need only be finite because wrap
   modes intentionally accept coordinates outside `[0,1]`; retain the
   currently narrower `[0,1]` two-texture rule unless cap40 applies: any axis
   set to REPEAT accepts finite magnitude at most 32, while CLAMP and MIRROR
   axes remain in `[0,1]`.  Reject every degenerate three-record group with
   the checked 64-bit cross product.  See
   [ioquake3-signed-repeat-command15.md](ioquake3-signed-repeat-command15.md).

The public copy maxima are:

| request | vertices | record bytes | copied bytes |
|---|---:|---:|---:|
| one texture | 576 | 32 | 18,432 |
| two textures | 432 | 40 | 17,280 |

The copy can be freed after the synchronous submit returns.  No userspace
pointer may remain in a job or quarantine object.

## Resource ownership and fences

The run is one atomic resource-lifetime transaction even though it contains
up to eight command segments:

1. Look up and classify destination, texture0, optional texture1, and
   optional D16 exactly once in the calling `drm_file`.
2. Lock all reservations in one interruptible `drm_exec`, wait all existing
   dependencies, obtain SGTs/vmaps, and construct one complete PTE image.
3. Under `submit_lock`, allocate one contiguous SF node, bind all PTEs once,
   initialize every canonical guard, flush each complete object, and transfer
   every SG table to the device once.
4. Attach one fence to the destination with WRITE usage, each texture with
   READ usage, and D16 with WRITE usage even when depth-read-only is selected.
   Keeping WRITE for read-only depth is conservative and matches the merged
   structured path until a separate reservation semantic is proved.
5. Build, append, and synchronously retire every segment while ownership,
   reservations, mappings, and `submit_lock` remain held.  Alternate cookie
   `00010001/00020002` for each segment and update `engine->last_cookie`
   immediately after each proved cookie/PAUSE/non-CR-idle wait.
6. After the final segment only, transfer resources to the CPU once, flush,
   verify every guard, resync an active persistent scanout mapping for the
   destination, invalidate transient PTEs, remove the node, signal the fence,
   and release all mappings/references.

The current internal names are hazardous.  In the historical helper,
`depth_obj` is texture0 for textured jobs, `texture2_obj` is either texture1
or actual D16, and `texture3_obj` is actual D16 for a two-texture depth job.
A run patch should introduce explicit role helpers or a small indexed object
descriptor.  If it retains the old names, every new kind must be added to
all of `textured_depth`, `two_textured_depth`, `three_objects`,
`four_objects`, `texture_objects`, target-address selection, guard/sync
loops, fence usage, builder selection, and cleanup.  Missing only one of
those classifications can fence a texture as a target or use a texture
address as depth.

The builder loop is:

```text
limit = one_texture ? 24 : 18
remaining = triangle_count
first = 0

while remaining:
        chunk = min(remaining, limit)
        request_copy.triangle_count = chunk
        request_copy.vertices = copied_vertices + first * 3
        job.cookie = next_cookie(engine.last_cookie)
        build one state-complete list with A24 = 3 * chunk
        append segment
        wait for cookie, PAUSE, and non-CR engine idle
        engine.last_cookie = job.cookie
        first += chunk
        remaining -= chunk
```

The fence covers the whole run, never one segment.  Do not drop `drm_exec`,
repeat cache ownership transitions, scan guards, or unbind PTEs between
segments.  The staging page is reusable only because each preceding segment
has proved retirement before the next is built.

## Failure and quarantine rules

A later-segment failure can leave a valid retired prefix.  That is an allowed
partial draw failure; userspace must not replay the run through legacy
ioctls.

- Validation, copy, lookup, reservation, vmap, or allocation failure before
  publication uses ordinary unwind.
- Build or append rejection before publication of the current segment may
  unbind and signal the single fence with error, even if earlier segments
  retired.  Preserve the last retired cookie.
- Ring STOP replacement failure with `must_quarantine`, cookie/PAUSE/work-idle
  timeout, or any other published-but-unproved current segment wedges and
  quarantines the whole job.  Retain mappings, PTE node, object references,
  ring epoch, and an unsignaled error fence until reboot.
- Guard failure after proved retirement signals the fence with `-EIO` after
  safe unbind.  A post-retirement PTE invalidation failure quarantines the
  mapping; after CPU ownership is restored its error fence may be signaled,
  matching the current distinction.

`job->published` is cumulative and cannot decide loop cleanup.  Add explicit
per-segment `current_published/current_retired` state and preserve the append
helper's `must_quarantine` result.  Set `job->retirement_proven` only after
the final segment.  Log kind, segment index/count, triangle count, cookie,
ring epoch, current/PAUSE/STOP, and engine/error registers on every failure.

## Mesa accumulation and flush boundaries

Add separate one- and two-texture run arrays to `chrome9_vbuf_render`; do not
mix them with the untextured RHW array.  The complete immutable run key is:

```text
destination handle, dimensions, pitch, and format
optional D16 handle, pitch, compare function, and write mask
texture count and every handle/dimension/pitch/format/filter/wrap/view level
one-texture program, or exact two-texture program enum
complete blend/alpha-test/color-mask state
scissor enable and rectangle
```

Diffuse/specular and S/T are per-vertex data, not key fields.  Flush before
adding a triangle if any key bit changes, the relevant 192/144 run capacity
is reached, or the outer Gallium draw ends.  The current-source audit below
shows that copied native records can safely span draw-module callbacks inside
one synchronous `chrome9_draw_vbo()`.  Do not span separate Gallium draws;
that requires additional state hooks and resource references.

Also flush before resource destruction, context teardown, a software
fallback, or any operation that requires prior rendering visible.  A failed
run marks the callback failed and drops its remaining queued triangles; it
must not resubmit an already partially executed run.  Count accepted plus
queued triangles against draw limits.  Publish the render target shadow once
after a successful run rather than once per triangle.

Old kernels or missing capabilities retain the exact per-triangle paths.
Advertise cap30 or cap31 only after the corresponding packet/list and
eight-segment live gates pass; support may be promoted independently.

## Expected command-count reduction

The theoretical reductions within one immutable outer Gallium draw are exact:

| path | old ioctls | new ioctls |
|---|---:|---:|
| one-texture run maximum | 192 | 1 |
| two-texture run maximum | 144 | 1 |
| Xorg/glamor textured copy quad | 2 | 1 |
| one textured cube list | 12 | 1 |

The last two depend on Mesa receiving both triangles under one outer draw and
one key.  Real ioquake3/glmark2 gains must be measured with a histogram; API
draw boundaries and texture/state changes can split runs well below the ABI
maximum, while clipping and software decomposition can create several vbuf
callbacks inside the retained outer-draw boundary.  Add userspace counters
for queued triangles, run ioctls, segments, list-size histogram, and flush
reason.  Add kernel counters for requested triangles, retired segments,
PTE/cache ownership transitions, copied command bytes, wraps, and timeout
segment.

## Incremental no-wedge gate

Keep both capabilities disabled until this exact progression passes for each
texture count:

1. Offline packet tests for one triangle must byte-match the current builder
   apart from the run kind and copied-array source.
2. Address-free activation with no primitive launch must leave CR and engine
   idle.
3. Draw lists of 2, maximum-minus-one, and maximum triangles into a small
   guarded target; validate image, every source, every guard, and `040c=0`.
4. Draw a maximum list with DEPTH LESS, LEQUAL, and read-only; verify depth
   values and unchanged depth for read-only.
5. Repeat with blend disabled in its envelope, source-over, alpha test, RGB
   masks, BGRA target, scissor, nearest/linear, NPOT, and every proved wrap.
6. Run two through eight segments with the exact boundary counts.  Verify
   alternating cookies and that only one bind/unbind, ownership round trip,
   and fence occur.
7. Fault-inject prepublication build failure, second-segment append failure,
   timeout quarantine, guard failure, and final PTE invalidation failure.
8. Run old per-triangle, D16, blend, two-texture, direct-scanout, and Xorg
   presentation regressions before advertising the cap.

The one-texture and two-texture gates are independent.  A passing
one-texture list is not evidence for the ten-dword D4/two-sampler path.

## Patch map

Kernel/UAPI changes are localized but must be coherent:

- add command `0x14`/`0x15`, cap30/cap31, flags, limits, requests, ioctls,
  and size/offset assertions to both UAPI copies;
- add two render-allowed ioctl handlers that perform the full prelookup
  validation and copied-array lifetime;
- extend internal kinds/resource-role classification and capability
  reporting only after live promotion;
- generalize the existing textured builders to a checked vertex span and
  mutable local A24, while preserving the single-triangle callers byte for
  byte;
- refactor the resource-lifetime shell only enough to execute the bounded
  segment loop under one job/fence; and
- add offline body/framing tests for every fixed-dword variant and boundary.

Mesa changes are confined to UAPI copies, capability discovery, per-draw
run accumulation, exact key comparison, new ioctl marshalling, failure
suppression, and tests.  The shader classifiers and per-triangle fallback
remain unchanged.

## Current Mesa integration audit

The current control flow makes a more useful first retention boundary
available than an individual vbuf callback.  `chrome9_draw_vbo()` refreshes
all sampler shadows, maps vertex/index/VS-constant inputs, calls draw's
software pipeline, calls `draw_flush()`, publishes an imported render-target
shadow, and only then unmaps inputs.  Gallium cannot bind a new framebuffer,
shader, sampler, blend/DSA/rasterizer state, or scissor in the middle of that
synchronous call.  Several `vbuf_render.draw_arrays/draw_elements` callbacks
may nevertheless be generated by clipping and primitive decomposition.

Therefore retain copied native run vertices across those callbacks, but not
across the outer `pipe_context::draw_vbo` call.  The exact lifecycle is:

```text
chrome9_draw_vbo:
        refresh sampler shadows
        reject unsupported/empty/indirect draw
        map draw inputs
        chrome9_vbuf_begin_draw(ctx)       # zero count/failure/key
        draw_vbo(ctx->draw, ...)
        draw_flush(ctx->draw)              # may issue final callbacks
        chrome9_vbuf_end_draw(ctx)         # submit retained tail
        chrome9_flush_render_target(ctx)   # publish once after retirement
        unmap draw inputs
```

Store the vbuf-render pointer in `chrome9_context` when
`chrome9_draw_vbuf_stage()` creates it, and expose begin/end helpers through
`chrome9_draw.h`.  The end helper must execute after draw's own flush, because
that flush can still produce output callbacks.  Every early exit after begin
must call end; the simplest implementation places begin only after all input
mapping succeeds and uses one `draw_started` cleanup boolean.  The render
destroy callback clears the context pointer before freeing itself.

Remove the unconditional count reset and tail flush from
`chrome9_vbuf_draw_arrays()` and `chrome9_vbuf_draw_elements()` only for the
new retained run.  The capacity flush remains inside `chrome9_emit_triangle`.
If crossing callbacks is intentionally deferred for the first patch, retain
those two old boundaries; the UAPI and kernel design do not depend on the
userspace retention scope.  Do not retain across separate Gallium draws:
doing that safely would require a flush hook in every framebuffer, sampler,
view, shader, blend, DSA, rasterizer, scissor, constant-buffer, resource-map,
and teardown callback plus explicit resource references.

### Storage without enlarging every path twice

Only one texture count is active during a draw.  Add one tagged union rather
than two simultaneous maximum arrays:

```c
enum chrome9_texture_run_kind {
        CHROME9_TEXTURE_RUN_NONE,
        CHROME9_TEXTURE_RUN_ONE,
        CHROME9_TEXTURE_RUN_TWO,
};

union chrome9_texture_run_vertices {
        struct drm_chrome9_textured_vertex
                one[DRM_CHROME9_TEXTURE_RUN_MAX_TRIANGLES * 3];
        struct drm_chrome9_two_textured_vertex
                two[DRM_CHROME9_TWO_TEXTURE_RUN_MAX_TRIANGLES * 3];
};
```

The union is 18,432 bytes instead of 35,712 bytes for separate arrays.  Add
the two fixed request envelopes, run kind, triangle count, failure flag, and
debug flush reason to `chrome9_vbuf_render`.  Vertex records are complete
copies, so releasing draw's transient `crender->vertices` does not invalidate
a retained run.

`batch_failed` can be generalized to all batch transports, but a textured
run failure must not enter the existing per-triangle dispatch at the bottom
of `chrome9_emit_triangle()`.  Once a run ioctl fails, set failure, zero its
count, count every later triangle in that outer draw as rejected, and never
replay the retired prefix.

### Exact lowered state key

Build a zero-initialized candidate run request from the already translated
state in `chrome9_emit_triangle()`, clear only `triangle_count` and
`vertices_ptr`, and compare all remaining bytes with the retained envelope.
Explicit zero initialization makes `memcmp()` safe because both UAPI
structures have named pad/reserved storage.  The key consequently contains:

- destination handle, dimensions, pitch, and BGRX/BGRA format;
- DEPTH/BLEND/depth-function/read-only flags, D16 handle and pitch;
- one-texture REPLACE/MODULATE selection;
- separate two-texture program enum 0/1/2;
- every texture handle, level-zero dimension, pitch, format, min/mag filter,
  and S/T wrap mode;
- all ten blend/alpha-test/color-mask words; and
- the complete half-open scissor state.

Do not key on the high-level fragment-classifier identity, `packed_generic`,
constant color, diffuse/specular, or texture coordinates.  Classifiers that
lower to the same enumerated native program share the key; the program enum
itself must differ for the two-factor, full-primary, and TEX0-alpha-primary
objects.  Constants and varyings have already been materialized in each
copied native record.  The key should compare equal throughout one outer
draw.  A mismatch is an internal invariant violation: flush the valid prefix
to preserve ordering, mark the draw failed, and warn once rather than
silently starting a second state envelope.

The existing draw-limit test must use
`submitted_triangles + batch_count + texture_run_count`.  A capacity flush
increments `submitted_triangles` once by the run count and calls
`chrome9_context_render_target_written()` once.  A final tail does the same;
the existing post-draw shadow publication then copies the resulting partial
or complete target exactly once.

### Capability and transport selection

Keep semantic acceptance separate from transport availability.  The current
`chrome9_draw_state_supported()` rejects every two-texture blend before
transport selection, so merely adding a cap check at the final ioctl
dispatcher will never reach the new path.  Amend its capability gates with
this matrix:

| requested state | required semantic capabilities before selecting run |
|---|---|
| one texture | existing one-texture base path + cap30 |
| one + D16 | one-texture D16 + depth-function/read-only bits as used + cap30 |
| one + blend | one-texture blend/alpha/extended-factor bits as used + cap30 |
| one + D16 + blend | one-texture D16-blend plus subordinate bits + cap30 |
| two textures | existing two-texture base path + cap31 |
| two + D16 | two-texture D16 plus depth-function/read-only bits as used + cap31 |
| two + blend, no D16 | cap31, whose ABI explicitly owns this new composition |
| two + D16 + blend | reserved two-texture D16-blend cap28 + cap31 |

The no-depth two-texture blend combination has no predecessor ioctl or
semantic capability.  It must be treated as part of cap31's contract and
must pass its own silicon image gate before cap31 is advertised.  Do not
infer it from cap28, which names the D16 composition.  Until that gate passes,
the kernel rejects BLEND without DEPTH for command `0x15`, and Mesa retains
the current rejection.

Context creation adds only two booleans from capability bits 30/31.  DRM
wrappers are direct synchronous `ioctl()` calls matching the existing style.
The legacy dispatcher remains byte-for-byte fallback when the relevant run
cap is absent, while unsupported semantic combinations remain rejected; a
run cap is not permission to bypass alpha, BGRA, extended-factor, depth-func,
or read-only gates.

## Command-ring reuse audit

The persistent command ring already implements the only ring reuse needed by
the run.  `chrome9_append_segment()` first requires CR paused and the work
engine idle, copies `segment_stop + 0x220` bytes, then publishes only the new
A64/A65 pause tuple.  It leaves A60--A63 and A68 resident on an ordinary
append.  After publication it sets:

```text
engine->append = job->ring_pause + 0x10
```

For either maximum textured list, staging PAUSE is `0x0cf0`, STOP `0x0d70`,
and copy length `0x0f90`.  Successive no-wrap segment starts therefore
advance by `0x0d00` bytes.  An eight-segment run advances 26,624 bytes and
copies 31,872 bytes in total.  It can wrap the 64-KiB ring at most once: if
the first segment wraps to zero, the eighth segment starts at `0x5b00` and
its closure ends at `0x6a90`, well below `0x10000`.

That wrap bound is useful for tests and fault injection, but it does not
remove any retirement rule.  The wrap path replaces the old PID00 PAUSE with
a PID10 STOP, waits it idle, fills the ring with dummy pairs, installs the
new stopped ring, and increments the epoch.  A failed published STOP must
quarantine the run exactly as documented above.

Three tempting extensions do not safely reduce syscall overhead:

- Omitting repeated state from segments would shrink command bytes, but not
  the number of ioctls or ownership transitions.  It also makes each segment
  depend on undocumented state persistence across PAUSE, wrap, and another
  context.  Keep every segment state-complete.
- Appending all eight segments before waiting would encounter a PAUSE after
  the first segment and would require a new linked-pause protocol.  It also
  defeats safe staging reuse and precise quarantine attribution.  Continue
  to alternate and wait one cookie per segment inside the one ioctl.
- Keeping SF mappings resident across separate ioctls does not remove the
  syscalls and greatly expands BO eviction, reservation, scanout alias, and
  context teardown lifetime.  The run's one bind/unbind interval captures
  the high-value reuse without creating persistent render mappings.

Thus command count is reduced in userspace by the new run ioctl, while the
kernel deliberately retains one state-complete command segment and one
retirement proof per 24/18 triangles.  The live timing counter must separate
syscall/run count from segment count: a maximum run is one ioctl but eight
hardware appends and waits.
