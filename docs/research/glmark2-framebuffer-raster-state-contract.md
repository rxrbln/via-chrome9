<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# glmark2 framebuffer, blend, depth, viewport, and cull contract

This is the exact non-shader state audit for the installed glmark2 2023.01
source in `/home/ai/tmp/gm2/glmark2-2023.01`.  It complements
`glmark2-generic-feature-matrix.md` and
`chrome9-depth-r2t-vtf-contract.md`.  The goal is one generic Chrome9 state
machine which runs every scene; none of the requirements below is a scene
special case.

## Actual state universe

The canvas establishes one persistent baseline:

```text
viewport = full drawable
depth test = enabled
depth func = LEQUAL
depth write = enabled
cull = BACK
front face = default CCW
blend = disabled
color mask = RGBA
scissor = disabled
```

The scenes use only the following deltas.

| state | values actually requested |
|---|---|
| viewport | `(0,0,target_width,target_height)` for the current window/FBO |
| scissor | never enabled by an installed scene |
| depth func | `LEQUAL` only |
| depth access | enabled/LEQUAL/write or disabled/no effective writes; desktop and terrain also clear the depth writemask while disabled, while jellyfish/ideas merely disable the test |
| cull | BACK, FRONT, or disabled; no scene calls `glFrontFace` |
| color mask | RGBA or zero |
| blend equation | ADD only |
| blend factors | `SRC_ALPHA/INV_SRC_ALPHA` for both lanes; RGB source-over plus alpha `ZERO/ONE`; or `SRC_ALPHA/ONE` for both lanes |
| stencil, logic op, A2C, MSAA | unused |

This narrow state set does not make the implementation scene-specific.  It
defines the first generic GL 2.1 state subset which must be complete before
more factors/functions are promoted.

## Per-scene transition matrix

| path | framebuffer/depth | state transition |
|---|---|---|
| ordinary scenes | default BGRX plus window Z16 | inherited LEQUAL/write and BACK cull, blend off |
| `shadow` producer | depth-only X8Z24 texture | full 2x viewport, LEQUAL/write, BACK cull, color mask zero, depth clear |
| `shadow` consumer | default BGRX+Z16, samples X8Z24 | full canvas viewport, LEQUAL/write, BACK cull, full color mask |
| `refract` producer | BGRA texture plus X8Z24 texture | full 2x viewport, color+depth clear, LEQUAL/write, cull FRONT |
| `refract` consumer | default BGRX+Z16, samples D24+BGRA-mips+image | canvas viewport, LEQUAL/write, cull BACK |
| `desktop` FBO chain | BGRA color only | per-FBO full viewport, depth off/write off, blend off during blur/copy |
| `desktop` composite/shadow | BGRA color only | depth off/write off; RGB source-over, alpha `ZERO/ONE` |
| `terrain` generated maps | BGRA color only | per-FBO full viewport, depth off/write off, blend off |
| `terrain` main | BGRA plus Z16 renderbuffer | full target viewport, LEQUAL/write, blend off |
| `terrain` overlay/bloom | BGRA color only | depth off/write off; `SRC_ALPHA/ONE` additive blend |
| `jellyfish` | default BGRX, no active depth | cull off, depth off, two textures, `SRC_ALPHA/INV_SRC_ALPHA` blend on both RGB and alpha |
| `pulsar` | default BGRX+Z16 | cull off, LEQUAL/write; RGB source-over, alpha `ZERO/ONE` |
| `ideas` / `buffer` | default target | toggle depth and/or cull off, then restore the baseline |
| optional annotation text | default target | depth off; one atlas texture plus `SRC_ALPHA/INV_SRC_ALPHA`, then restore prior state |

The source calls are important for transition ordering.  `shadow` and
`refract` bind the FBO before changing viewport/state, then bind the canvas
before restoring them.  Terrain's `BaseRenderer::make_current()` binds the
target, sets its viewport, and toggles depth enable and depth mask together.
Desktop disables depth and depth writes for the whole scene and restores both
at teardown.

## Exact framebuffer formats and words

The installed scenes need only these attachment shapes:

| public framebuffer | Chrome9 physical state |
|---|---|
| BGRX color, no depth | A52 format `98` |
| BGRA color, no depth | A52 format `99` |
| BGRX/BGRA plus Z16 | A12 format `0` (`12380000`) |
| no color plus X8Z24 | internal BGRX scratch, A53 zero; D24 A12 format `6` |
| BGRA plus X8Z24 | A52 `99`; D24 A12 format `6` |

For linear SF targets:

```text
A50 = 50000000 | (color_sf_va >> 8)
A51 = 51100000
A52 = 52980000 | (color_pitch >> 5)       # BGRX
A52 = 52990000 | (color_pitch >> 5)       # BGRA
A53 = 53010c00 | physical_channel_mask

A10 = 10000000 | (depth_sf_va >> 8)
A11 = 11000001 | ((depth_pitch >> 5) << 13)
A12 = 12380000                             # Z16
A12 = 123e0000                             # X8Z24/S8Z24
A13 = 13030000                             # LEQUAL
```

Color pitch is `ALIGN(width*4,32)`.  Z16 pitch is
`ALIGN(width*2,32)`, while X8Z24 is `ALIGN(width*4,32)`.  All glmark FBO
attachments are L0, layer zero, one sample, and dimension-matched.  There is
no MRT, nonzero color mip attachment, layered target, or renderbuffer blit.

The full color window and disabled/full scissor are target-dimension state,
not API viewport state:

```text
A80 = 80000000 | height                     # color Y [0,height)
A81 = 81000000 | width                      # color X [0,width)
A82 = 82000000 | height                     # scissor Y [0,height)
A83 = 83000000 | width                      # scissor X [0,width)
```

For a real half-open scissor `[minx,maxx) x [miny,maxy)`:

```text
A82 = 82000000 | (miny << 12) | maxy
A83 = 83000000 | (minx << 12) | maxx
```

Endpoints are at most the corresponding target dimension and 2048.  Mesa's
state tracker has already converted GL origin into Gallium top-origin
coordinates.  Never flip Y a second time.

### FBO bind lifecycle

Every `set_framebuffer_state` must perform this order:

1. Flush retained triangles targeting the old framebuffer.
2. Complete/publish an imported old color shadow if one exists.
3. Retain the new public cbuf/zsbuf resources.
4. Select or allocate the new guarded hardware color target.  For a public
   color attachment this is its guarded native BO or the existing guarded
   import shadow.  For depth-only it is the internal exact-size scratch.
5. Set draw's Z/S format from the public depth surface.
6. On the next draw, program all address, pitch, format, window, mask, and
   depth-enable words from the new state; do not inherit attachment state.

Depth-only scratch remains invisible to public clear, sampler, copy-back,
presentation, and stale-render-target tracking.  It exists only because the
first safe Chrome9 packet still carries A50--A53.  The public framebuffer
continues to have `nr_cbufs=0`.

FBO binding itself does not change the GL viewport.  Glmark explicitly sets
the new full-target viewport after each bind.  The driver must therefore keep
framebuffer and viewport state independent and must not synthesize a second
viewport transformation from target dimensions.

## Viewport and clipping boundary

The current software boundary is already the correct one for all installed
scenes:

* Gallium draw executes VS, homogeneous clipping, culling, primitive
  decomposition, and viewport transformation.
* Chrome9 receives finite post-viewport XYZRHW.
* For GL half-integer centers, Mesa subtracts `0.5` from post-viewport X/Y
  only, then converts to signed 16.16.
* Accepted coordinates are `[-0.5,width-0.5]` and
  `[-0.5,height-0.5]`.  The target limit is 2048.
* A01 native clip-engine bit 11 stays clear.  Enabling it would apply a
  second clip/viewport transform.

No installed scene enables scissor.  The existing half-open A82/A83 path is
still a required core regression, but it is not a blocker for glmark's scene
set.  Clears are likewise full-FBO clears; there is no masked color clear,
stencil clear, or scissored clear in the benchmark source.

The important glmark viewport regression is target switching, not an exotic
viewport: alternate canvas -> small BGRA FBO -> 2x D24 FBO -> canvas while
drawing a full-extent quad and verify every edge, guard, and A80--A83 word.

## Cull transitions

Software draw currently resolves culling correctly and is sufficient for
all scenes.  The state tracker has already folded GL front-face, framebuffer
orientation, clip origin, and viewport Y sign into
`pipe_rasterizer_state::front_ccw`.  Draw culls physical post-viewport
triangles before Chrome9 submission.

The critical transition is refract's BACK -> FRONT -> BACK sequence across
FBO/window orientation.  Test both windings in both targets.  Do not add an
extra FBO Y flip in Chrome9.

Native cull is an optional performance promotion, not a scene blocker.  If
used later, its exact fields are A01 bit 7 and A23 bit 8, with:

```text
cull NONE:  A01 bit 7 clear
cull FRONT: culled_ccw = rasterizer->front_ccw
cull BACK:  culled_ccw = !rasterizer->front_ccw
A23 bit 8 = culled_ccw
```

It must disable only draw's software cull for that draw to avoid double
culling; draw still needs the original `front_ccw` for other semantics.

## Depth state

Glmark needs one compare function, LEQUAL:

```text
A13 = 13030000
A01 = 01c0154c     # enabled, write enabled
A01 = 01c01540     # disabled, writes disabled
```

Keep the wide pixel-Z interval and neutral bias state:

```text
A16=16000000 A17=17000000 A18=18000000
A19=19800000 A1A=1a800000 A1B=1b803fbf
A1E=1e000000 A1F=1f000000 A20=20007fff
```

The current Z16 LEQUAL/write path is live.  X8Z24 must reproduce the same
coverage/write oracle with A12 `123e0000`.  D24 producer passes do not need
stencil, polygon offset, or read-only depth.

Consumer combinations matter as much as the D24 producer:

* shadow samples X8Z24 while rendering to the default BGRX+Z16 framebuffer;
* refract samples X8Z24 plus two color textures while rendering to the
  default BGRX+Z16 framebuffer.

Thus raw D24 sampling cannot be implemented only in a D24-destination ioctl.
The generic texture descriptor must accept an X8Z24 source independently of
the destination depth format.  Producer depth and consumer Z16 are distinct
resources and distinct format words.

When depth is disabled, the API depth writemask has no independent packet
effect: emit A01 with both test and hardware writes off whether the retained
Gallium mask is true (for example jellyfish) or false (desktop/terrain).  A
later depth-enabled packet must rewrite A10--A13 and A01 completely.  Never
treat a stale D24 A12 as a Z16 surface.

## Color mask and blend words

The public-to-physical mask mapping is:

```text
R -> A53 bit 2       G -> bit 1       B -> bit 0       A -> bit 3
RGBA 53010c0f        RGB 53010c07     none 53010c00
```

Shadow's zero mask is generic depth-only state.  The internal scratch lowering
must force effective mask zero even if an application leaves its public mask
at RGBA.  The following canvas draw rewrites full mask.

All glmark blends use ADD and zero blend constants.  Exact clamped words are:

| GL equation | A34 | A35 | A36 | A37 |
|---|---:|---:|---:|---:|
| COPY `ONE/ZERO`, both lanes | `34000150` | `35000114` | `36000590` | `37000224` |
| `SRC_A/INV_SRC_A`, both lanes | `34000020` | `35000148` | `36000410` | `37000244` |
| RGB `SRC_A/INV_SRC_A`, alpha `ZERO/ONE` | `34000020` | `35000148` | `36000490` | `37000264` |
| `SRC_A/ONE`, both lanes | `34000020` | `35000154` | `36000410` | `37000264` |

Emit all A38--A3F constant halves as zero for every current blend-aware
packet.  ZERO and ONE share native selectors with constant factors, so a
partially stale constant image can corrupt even ordinary blending.  Blending
disabled must canonicalize COPY rather than trusting unused CSO fields.

### Missing blend command combinations

Two current gates block real generic states:

1. Jellyfish uses two texture samples, blending enabled, and depth disabled.
   The driver currently rejects every two-texture blend unless depth is
   active.  A color-only two-or-more-texture blend successor is required; a
   dummy depth surface is not an acceptable workaround.
2. Terrain overlay and annotated text use a sampled/modified fragment result
   with blending.  The driver currently rejects one-texture blend for most
   fragment programs because of the broad
   `textured-modulate-blend-unproven` safety gate.  Characterize a generic
   sampled-result blend packet, then gate it by complete validated state, not
   by scene or hardcoded shader identity.

The packet blend block is independent of TAU count.  A future general PS
command should carry one validated RT, optional depth, N validated texture
descriptors, the closed A34--A3F/A53/A00/A01 state, and program state.  Avoid
adding separate ioctl ordinals for every glmark sampler-count/blend/depth
cross product.

## Color render-to-texture and mip blocker

BGRA L0 rendering itself is live, but refract exposes a separate format/state
gap: its 2x-canvas `GL_RGBA` color attachment is sampled with
`LINEAR_MIPMAP_LINEAR` after `glGenerateMipmap`.  It is normally rectangular,
larger than 512, and stored as BGRA.

The current mip submission accepts only square POT BGRX at most 512.  It
therefore cannot run refract even if D24 and three samplers are added.  Static
RE has recovered:

* L1--L8 explicit 256-byte-unit addresses;
* canonical-pitch L0 NPOT dimension encoding in A20/A22;
* a bounded 30x18 NPOT mip oracle using the actual resource offsets.

What remains is live validation of asymmetric NPOT mip addressing and BGRA
format 99 sampling across the generated chain.  Do not round the logical
texture to a POT or silently sample L0.  Generate levels with their actual
logical dimensions, explicit offsets and canonical 32-byte row pitches, then
test transition closure BGRA-mip -> L0-only -> BGRA-mip.

## Generic state-closure suite

The minimum ordered regression should use guarded resources and run without
recreating the context:

1. BGRX+Z16, LEQUAL/write, BACK cull, blend off.
2. BGRA color-only, depth off/write off, source-over with destination alpha
   preserved.
3. BGRA+Z16, LEQUAL/write, blend off.
4. depth-only X8Z24 via scratch, mask zero, LEQUAL/write.
5. BGRA+X8Z24, FRONT cull, LEQUAL/write.
6. BGRX+Z16 consumer sampling X8Z24, BACK cull.
7. BGRX color-only consumer sampling two textures with source-over.
8. BGRA color-only consumer sampling one texture with `SRC_ALPHA/ONE`.
9. disabled/full scissor -> small enabled scissor -> disabled/full.
10. final legacy BGRX+Z16 COPY draw and 2D/present closure.

For every step verify the complete target, depth payload where applicable,
both guard pages, completion cookie, engine error state, and the first packet
of the next transition.  In particular assert A52, A53, A01, A12 when active,
A34--A3F, and A80--A83.  A pixel-only test can miss stale state which happens
to be inert in one step and wedges the following one.

## Remaining RE/live gaps ranked by scene impact

1. **General PS/texture descriptor count.** Refract needs three FS samplers;
   terrain needs at least six logical FS slots with five active.  This is the
   largest command-shape gap.
2. **X8Z24 production and raw consumption.** Exact words are known; guarded
   silicon promotion and mixed X8Z24-source/Z16-destination submission remain.
3. **Rectangular BGRA mip chains.** Exact static NPOT dimensions/level table
   are known; the asymmetric chain still needs its bounded live oracle.
4. **Two-texture color-only blend and generic sampled-result blend.** Exact
   blend words are known.  The missing work is a safe closed command and the
   live transition which permits removal of the broad safety rejection.
5. **Depth-only physical destination.** The mask-zero scratch design is
   complete; allocate/validate it and run the producer/closure oracle.
6. **Viewport/scissor/cull.** No new RE blocks installed glmark scenes.
   Software viewport/cull and the live scissor packet already cover their
   required semantics; keep them in the closure suite.

Stencil, native cull, native viewport/clipping, MRT, MSAA, blit, blend
constants, non-ADD equations, and partial clears are useful later features,
but none should delay the installed glmark2 correctness milestone.
