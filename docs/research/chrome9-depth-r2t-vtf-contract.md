# Chrome9 depth render-to-texture and software-VTF contract

This note closes the generic resource/state boundary needed by glmark2's
`shadow`, `refract`, and `terrain` scenes.  It is an implementation contract,
not permission to expose a format or capability before its guarded live test
passes.  No rule below depends on a scene name or shader fingerprint.

The source snapshot examined here is Mesa 26.2.2 under
`src.mesa.x86-64-desktop.260909.123648.3741772`, the current Chrome9 kernel
driver, the proprietary Linux `via_chrome9_dri.so` from 5.75.32.87a, and the
packet/format recovery already recorded in this directory.

## Bottom line

### Live VX900 checkpoint, 2026-09-23

The guarded generic command-22 D24X8 LESS draw now retires on
`192.168.2.60` with a zero-varying `MOV oC0,c0` fragment program.  A 64x64
BGRA destination and separate X8Z24 depth surface produced 1,176 red triangle
pixels; the sampled depth word was `80000000` for z=0.5.  The kernel verified
both complete guard pages after retirement, and the smoke test checked
triangle interior, exterior, and guards.  The test is
`tools/re/chrome9_d24_guard_smoke.c`; it uses the kernel's fixed
`13579bdf` guard word for both BOs.  An earlier test-only mismatch expected
a different depth guard word and falsely reported corruption.

This proves one untextured D24 LESS write, not D24 sampling, alternate
comparison modes, read-only depth, render-to-texture ordering, or Gallium FBO
completeness.  Mesa still does not advertise the format.

The subsequent guarded raw-sampler oracle also passed on the live VX900 using
`tools/re/chrome9_d24_sampler_guard_smoke.c`: nearest sampling of four
contract and four asymmetric X8Z24 words produced the expected 8-bit depth
colors, independent of poisoned low bytes.  Both BO guards and all source
bytes remained intact.  This proves standalone raw X8Z24 readout, but not
ordering a depth write before a texture read from the same BO.

The separate same-BO producer/consumer oracle subsequently passed as well:
`tools/re/chrome9_d24_r2t_guard_smoke.c` wrote the 64x64 D24 triangle,
then used that exact depth BO as a raw X8Z24 texture in a second synchronous
command-22 draw.  Six written/unwritten samples matched, all guards stayed
intact, and the producer BOs were byte-for-byte unchanged after sampling.
This establishes the kernel command-22 D24 write-to-raw-read transition on
this VX900.  Gallium FBO completeness, alternate compare modes, depth-only
scratch rendering, and GL capability exposure still require implementation
and focused proof.

Mesa's experimental private `X8Z24_UNORM` resource/FBO smoke then passed on
the target with `CHROME9_EXPERIMENTAL_X8Z24=1`: private sampler-only and
depth-only allocation, rejection of mip/shared forms, identity sampler view,
guarded depth-only scratch, CPU clear to `80000000`, and color/depth-only FBO
rebind.  This is a Gallium resource/state/CPU-clear proof, not yet a Gallium
GPU draw or GL state-tracker conformance proof; the experimental gate remains.

* Chrome9's native sampled 24-bit depth layout is depth in bits 31:8.  The
  correct Gallium formats are `PIPE_FORMAT_X8Z24_UNORM` and
  `PIPE_FORMAT_S8_UINT_Z24_UNORM`, not `Z24X8` or `Z24_UNORM_S8_UINT`.
* D24X8 and D24S8 use the same depth-surface word, `A12=123e0000`, and the
  same raw-depth texture word, `A30=30fe2000`.  X8 versus S8 changes only the
  meaning/preservation of the low byte and the stencil state.
* Until a no-color hardware mode is proved, lower a depth-only framebuffer to
  a private, guarded BGRX scratch render target of the same dimensions and
  force `A53=53010c00`.  Keep color globally enabled.  The scratch is a
  hardware implementation detail, not a Gallium color attachment.
* A depth resource may be created for sampling alone, attachment alone, or
  both.  Do not make `PIPE_BIND_DEPTH_STENCIL` a prerequisite for accepting
  `PIPE_BIND_SAMPLER_VIEW`.
* Software vertex texture fetch belongs beside the draw module, using
  stage-private sampler arrays and a refresh immediately before every draw.
  The m9x helper is the right template, but Chrome9 must resolve outstanding
  GPU writes before exposing CPU bytes and must not leak vertex-stage views
  into fragment hardware state.

## Exact destination and depth words

The first linear, non-tiled implementation should emit a closed state image.
For a private BGRX destination:

```text
A50 = 50000000 | (target_sf_va >> 8)
A51 = 51100000
A52 = 52980000 | (target_pitch >> 5)     # B8G8R8X8
A53 = 53010c00 | remapped_mask           # see mask table below
```

The expression for A53 is conceptual: preserve `53010c00` and OR exactly the
four hardware mask bits.  Do not OR a public GL/Gallium mask directly.

```text
Gallium R -> A53 bit 2
Gallium G -> A53 bit 1
Gallium B -> A53 bit 0
Gallium A -> A53 bit 3

RGBA = 53010c0f      RGB = 53010c07      none = 53010c00
R    = 53010c04      G   = 53010c02      B    = 53010c01
A    = 53010c08
```

BGRA has an alpha channel and changes only the A52 private format:

```text
A52 = 52990000 | (target_pitch >> 5)     # B8G8R8A8
```

For D24X8 or D24S8:

```text
A10 = 10000000 | (depth_sf_va >> 8)
A11 = 11000001 | ((depth_pitch >> 5) << 13)
A12 = 123e0000                          # format 6, linear SF
A13 = 13000000 | (depth_func << 16)

A16 = 16000000                          # neutral polygon bias
A17 = 17000000
A18 = 18000000
A19 = 19800000                          # -1 clamp low payload
A1A = 1a800000                          # +1 clamp high payload
A1B = 1b803fbf                          # precision/clamp bytes
A1E = 1e000000
A1F = 1f000000
A20 = 20007fff                          # wide pixel-Z interval

A23 = 2300ffff                          # neutral stencil CW
A24 = 24070000                          # ALWAYS, KEEP/KEEP/KEEP
A25 = 2500ffff                          # neutral stencil CCW
A26 = 26070000
A33 = 33007000                          # alpha test disabled
```

The exact A01 controls are:

| depth | write | stencil | A01 |
|---|---|---|---:|
| off | off | off | `01c01540` |
| on | on | off | `01c0154c` |
| on | read-only | off | `01c01548` |
| off | off | on | `01c01550` |
| on | on | on | `01c0155c` |
| on | read-only | on | `01c01558` |

Use `A00=00000001` when neither texture nor blend/alpha-test state is active.
The low bit is the texture-cache clear pulse.  A one-texture consumer normally
uses `A00=00000011`; the active-texture count and any blend/alpha-test bits
must come from the selected generic draw state rather than being copied from
this example.

If alpha test or shader discard is lowered to alpha test, retain it during a
depth-only pass: killed fragments must not write depth.  A color mask of zero
does not justify deleting alpha-test state.  With alpha test enabled, emit its
canonical A00 enable and exact A33 compare/reference instead of `33007000`.

### D24 memory and clear

The canonical row pitch is `ALIGN(width * 4, 32)`.  Each dword is:

```text
X8Z24:  round(clamp(depth, 0, 1) * 0xffffff) << 8
S8Z24: (round(clamp(depth, 0, 1) * 0xffffff) << 8) | stencil
```

Depth 0 and 1 are therefore `00000000` and `ffffff00`.  An X8Z24 clear must
write zero to the unused low byte so later X8Z24/S8Z24 view transitions are
deterministic.  A depth-only clear of S8Z24 must preserve the low stencil byte;
a combined clear writes both components.  Do not reuse the current Z16 clear
conversion.

## Depth-only framebuffer lowering

The public Gallium framebuffer remains exactly what the caller supplied:
`nr_cbufs=0`, `zsbuf=X8Z24`.  At draw lowering time only, select an internal
BGRX target with:

```text
width, height = framebuffer width, height
format        = PIPE_FORMAT_B8G8R8X8_UNORM
target        = PIPE_TEXTURE_2D
last_level    = 0
bind          = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR
pitch         = ALIGN(width * 4, 32)
guarded       = true
A53           = 53010c00
```

Allocate the scratch with the exact native guarded layout.  The current
kernel validators derive a trailing guard from the exact dimensions and may
require the exact BO size; reusing an arbitrary larger resource can place
ordinary data where the validator expects the smaller resource's guard.
Cache one scratch per context/framebuffer size, or replace it when dimensions
change.  Hold the old reference until every job using it has retired.

The scratch must be:

* a distinct native private shmem object, never imported/exported, displayed,
  scanned out, sampled, or shared;
* distinct from the depth BO and every sampler BO;
* retained with the job and conservatively reserved/fenced WRITE even though
  A53 suppresses all stores;
* excluded from `ctx->render_target`, color clear, copy-back, presentation,
  stale-drawable publication, and `flush_resource` semantics.

Its initial contents are irrelevant when blend, logic operation, destination
read/de-gamma, and all channel writes are disabled.  There is no reason to
clear it.  Reject a depth-only draw state which would read the destination
until the lowering explicitly disables or supports that state.  Preserve
alpha kill as described above.

The forced effective color mask must be zero whenever the public framebuffer
has no color attachment, independent of the application's color-mask state.
`shadow` happens to call `glColorMask(GL_FALSE,...)`, but relying on that is
not a generic driver contract.

### Vendor evidence and its limit

The 5.75 Linux UMD does not reveal a hidden color allocation:

* `__glS3InvFBOMakeCurrent:0x22b550` scans three 0x14-byte attachment records
  and makes texture/renderbuffer attachments resident independently.
* `__glDpUpdateDrawBufferFromFBO:0x2382c0` sees an attachment type of zero and
  zeros its draw-buffer pointers at context `+0x3acc`, `+0x3adc`, and
  `+0x3aac`; it stores a null color surface in the active FBO state.
* `__glDpUpdateDepthStencilBuffer:0x238ac0` resolves depth and stencil
  attachments separately.
* `__glS3InvClearDepthBuffer_FBO:0x23b900` calls the depth/stencil clear path
  directly and passes the 24-bit clear mask `00ffffff`; it does not require a
  color clear.
* `STM_ColorWriteMode_INV:0x194df0` has an internal override at context
  `+0x4ffc` which forces all A53 mask components to zero.

This proves the logical null-color and mask-zero model, but not what address
the proprietary emitter leaves in A50 for a depth-only draw.  It may retain a
previous destination.  Therefore it is not evidence that A50 can be omitted
or zero.  A deliberately allocated guarded scratch target is safer and more
deterministic until a live packet proves global color-disable/no-target mode.
Keep A01 color-enable bit 6 set; clearing it is a separate, unproved mode.

## X8Z24 and S8Z24 resource rules

### `PIPE_FORMAT_X8Z24_UNORM`

Initially accept only:

* `PIPE_TEXTURE_2D`, width and height in the existing surface bounds;
* `depth0=array_size=1`, one sample, linear layout;
* level zero only until D24 mip addressing is independently proved;
* any subset of `PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SAMPLER_VIEW |
  PIPE_BIND_LINEAR`;
* neither render-target, display-target, scanout nor shared bindings.

The allocation uses four bytes per texel, the canonical 32-byte pitch, and
the exact guarded private layout.  Supporting SAMPLER_VIEW alone matters:
Mesa may allocate a depth texture before it is attached.  Supporting
DEPTH_STENCIL alone matters for renderbuffers.  Supporting both is the actual
render-to-texture case.  Reject only unknown/external binds; do not require
one legal bind merely because the other is present.

Expose this format only after all of these pass on the live VX900:

1. CPU clear/readback at 0, 1, and asymmetric midpoints, guards intact.
2. D24 LESS/LEQUAL/EQUAL/ALWAYS writes and read-only depth, followed by Z16
   state closure.
3. Raw nearest sampling of four poisoned low-byte values.
4. D24 producer completion followed by D24 consumer cache invalidation.
5. Depth-only and color+D24 framebuffer completeness/draw/clear/rebind.

### `PIPE_FORMAT_S8_UINT_Z24_UNORM`

This is the same four-byte physical layout with an active low stencil byte.
It may be deferred because installed glmark2 scenes do not require stencil.
When enabled, require the complete D24S8 stencil packet/state validators; do
not silently expose it through the X8Z24 work alone.

Mesa's `util_format_get_depth_only()` maps S8Z24 to X8Z24.  A depth-only
sampler view of an S8Z24 resource may therefore be accepted as X8Z24 because
the depth bits and stride are identical.  Reject stencil-only sampling until
there is an explicit hardware contract.  Keep the resource's storage format
for clears and attachment state; the view format controls only sampled depth.

### `PIPE_FORMAT_Z16_UNORM`

Keep Z16 attachment-only.  Mesa may add `PIPE_BIND_SAMPLER_VIEW` to an
otherwise depth-stencil renderbuffer for meta plumbing, so allocation may
tolerate that combined bind, but `create_sampler_view` must reject Z16.
The proprietary format lookup rejects D16 as a texture format.  Never route
it through A30 `fe` or advertise it as a depth sampler.

## Raw depth sampler view

For the first D24 sampler implementation require:

```text
resource/view format = X8Z24_UNORM
target               = PIPE_TEXTURE_2D
level/layer          = 0/0
compare_mode         = NONE
min/mag              = NEAREST
mip filter           = NONE
A30                  = 30fe2000
```

The private vendor formats are D24X8=`2032`, D24S8=`2033`.  Both select A30
format `fe`; the Linux UMD selects Z mode four, which routes sampled depth to
all four native result components.  D16 and D32 are rejected.  There is no
proved comparison/PCF mode.

Use this bounded oracle, with a different poisoned low byte in every region:

```text
stored 0000005a -> expected raw depth 0x000000
stored 555555a5 -> expected raw depth 0x555555
stored aaaaaa3c -> expected raw depth 0xaaaaaa
stored ffffff00 -> expected raw depth 0xffffff
```

The consumer must neither leak the low X/stencil byte nor normalize the wrong
24 bits.

Do not mistake A30 Z-mode routing for Gallium view swizzles.  Mesa compatibility
depth mode can request LUMINANCE `(X,X,X,1)`, INTENSITY `(X,X,X,X)`, ALPHA
`(0,0,0,X)`, or RED `(X,0,0,1)`, composed with the user swizzle.  The shader
backend must lower constants/swizzles after the raw sample.  Glmark2 reads
`.x`, so it does not test all of those contracts, but advertising a general
sampler view does.

## Render-to-texture ordering

A render-target-to-sampler transition has three separate concerns:

1. Finish any producer job which writes the BO.
2. Transfer ownership/cache visibility only if CPU code maps or regenerates
   the resource.
3. Emit the consumer type-FE texture-cache clear pulse (`A00` bit 0) before
   sampling.

The consumer's ordinary L0 image state is:

```text
A18 = 18001101                  # 2D, relative base, SF domain
A20 = encoded width/height
A21 = 21000001
A22 = encoded POT geometry
A30(image) = 30fe2000
A30(level 0) = 30000000
A31 = 31100000                  # nearest
A32 = encoded wrap modes
A00 = 00000011                  # one active texture plus cache clear
```

Retain the same D24 BO from producer through consumer; do not copy or change
its physical format.  Bind the producer depth and depth-only scratch as two
distinct objects in the same reservation transaction.  A later consumer job
binds the depth BO READ and emits the texture cache pulse.  If CPU code maps
the BO between jobs, it must first flush/wait the producer and perform the
same explicit CPU/GPU ownership transition used by other Chrome9 resources.

## Software vertex texture fetch boundary

Chrome9's vertex shader already executes through the draw module.  The m9x
implementation in `m9_vs_texture.c` demonstrates the missing interface:

* separate vertex-stage arrays of up to eight sampler states and views;
* `draw_set_samplers(..., MESA_SHADER_VERTEX, ...)` and
  `draw_set_sampler_views(...)` at bind time;
* before every draw, map/copy every current view and publish it with
  `draw_set_mapped_texture(...)`;
* release view references and copied storage at context destruction.

Chrome9 integration should preserve the following boundaries.

### State separation

Add vertex-stage sampler/view arrays independent of the existing
`ctx->sampler_states[]` and `ctx->sampler_views[]`, which are fragment
hardware inputs.  Dispatch `bind_sampler_states` and `set_sampler_views` by
shader stage.  Vertex views must never increment the fragment texture count,
populate a fragment draw ioctl, or change A00's active hardware texture
count.

Use eight logical VS slots if the draw sampler supports them.  Terrain binds
two active vertex samplers at nontrivial logical units; a capacity of two is
not equivalent unless the state tracker is proved to compact bindings.
Mesa derives `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS` by summing the per-stage
limits, so VS=8 and FS=8 advertises 16.  That is broader than the proprietary
stack's reported combined limit of eight, but it is implementable here
because VS sampling is on the CPU and FS sampling is separate hardware state.
Before advertising it, test simultaneous distinct VS and FS bindings at the
top of both stage-local ranges; there is no separate Gallium combined-limit
knob which can truthfully report eight while retaining eight units in each
stage.

### Refresh point and ownership

Refresh immediately before the call to `draw_vbo()`, after flushing any
retained Chrome9 producer batch.  An FBO-generated height/normal texture can
change every frame, so a bind-time copy is stale.  For a BO last written by
the GPU, resolve/wait it before CPU access.

The present Chrome9 draw ioctl is synchronous and `chrome9_flush()` therefore
returns no fence, but Mesa deliberately retains compatible triangles across
Gallium draw calls.  `chrome9_transfer_map()` itself only returns the
persistent BO mapping and performs no synchronization.  Consequently the VTF
refresh still has to call the batch flush before mapping.  Keep that ordering
explicit so a later asynchronous ioctl cannot turn the map into a race.

The safest first version follows m9x and copies through
`pipe_texture_map(PIPE_MAP_READ)` plus `util_format_translate` into a packed
linear allocation.  Chrome9 resources are already linear and persistently
mapped, so a later optimized path may publish the BO mapping directly, but
only after proving:

* producer retirement and CPU cache ownership;
* view level offsets and per-level row/image strides;
* resource lifetime while draw holds the mapped pointer;
* no concurrent GPU write to the published bytes.

The copied path must publish each view's `first_level..last_level`, dimensions,
row strides, image strides and mip offsets.  An unbound/unreadable view should
publish the same safe zero texel pattern as m9x, never a null pointer followed
by the draw sampler.

### Initial VTF format/state subset

Terrain requires two vertex samplers reading level-zero RGBA/BGRX-like color
textures with normalized coordinates, linear filtering, and clamp-to-edge.
That is the minimum initial generic subset.  Do not claim VS mipmapping,
depth sampling, cube/3D targets, compare mode, or anisotropy merely because
the CPU sampler has broader code paths; resource/view creation and the
Chrome9 ownership rules must also support them.

Only set `vs->max_texture_samplers`, `max_sampler_views`, and texture
instruction limits after the helper initialized and the stage callbacks are
installed.  The current zero limits are correct until then.

## Implementation and validation order

1. Add private X8Z24 layout, transfer, clear/readback and format tests without
   exposing the format to GL.
2. Add one state-complete untextured D24 successor and guarded depth oracles.
3. Add generic depth-only scratch lowering and A53-zero tests, then combined
   BGRA+D24 FBO tests.
4. Add raw nearest X8Z24 sampling and the poisoned-byte oracle.
5. Add producer-to-consumer ordering tests and only then expose X8Z24 through
   `is_format_supported` and sampler-view creation.
6. Port the m9x-style software-VTF helper with two dynamically produced
   textures, sparse slots, unbind/rebind and destruction tests before
   advertising VS sampler limits.
7. Run `shadow`, then `refract`, then `terrain`.  Failures at that point are
   shader/linkage or performance work, not reasons to special-case a scene.

S8Z24/stencil follows later as its own state-complete promotion.  No installed
glmark2 scene justifies bringing stencil into the first D24 R2T milestone.
