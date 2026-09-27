<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# glmark2 2023.01 generic texture, format, and FBO matrix

Status: source and current-driver audit, 2026-09-23.  This note deliberately
does not propose scene-name checks or shader-pattern substitutes.  Every item
below is a generic Gallium, resource, or validated Chrome9 command contract.

The workload source is upstream glmark2 tag `2023.01`, commit
`42e3d8fe3aa88743ef90348138f643f7b04a9237`, matching the target's reported
glmark2 version.  The target advertises these scenes:

```text
buffer build bump clear conditionals desktop effect2d function ideas
jellyfish loop pulsar refract shading shadow terrain texture
```

## Conclusions that constrain implementation

1. D16 cannot implement the depth textures in `shadow` or `refract`.  Both
   vendor UMDs reject D16 in their sampler-format lookup.  The only recovered
   raw depth sampler is D24X8/D24S8, texture A30=`30fe2000`, with Z routed to
   every component and nearest/no-mip filtering.  Chrome9 stores D24 in bits
   31:8, so the matching Gallium formats are `PIPE_FORMAT_X8Z24_UNORM` and,
   when stencil is needed, `PIPE_FORMAT_S8_UINT_Z24_UNORM`; the usual
   low-24-bit `Z24X8`/`Z24_S8` formats have the wrong memory layout.
2. These two scenes use ordinary `sampler2D` and compare sampled depth in the
   shader.  They do not require a comparison sampler, PCF, or
   `PIPE_CAP_TEXTURE_SHADOW_MAP`.  Keep comparison mode rejected.
3. `shadow` uses a depth-only FBO.  The current Gallium draw path rejects a
   framebuffer with no color surface because every ioctl requires a color
   target.  If the hardware packet cannot omit RT0, a per-context guarded
   scratch color surface plus an exact zero color-write mask is a valid
   generic lowering.  It must be dimensioned from the FBO, retained through
   completion, and tested as a depth-only implementation detail; it must not
   be selected by a scene or shader name.
4. The default benchmark set really includes the mipmap texture case.  The
   current mip ioctl covers only one square POT BGRX texture, clamp/clamp,
   at most 512 square and level 9.  That is enough only for the default
   512-square crate texture.  It cannot cover RGBA render targets, repeat,
   multiple samplers, arbitrary rectangles/NPOT, the 1024 asteroid choices,
   annotated text, `refract`, or `terrain`.
5. Sampler count and texture-instruction count are different limits.  The
   current driver sets both to one or two.  The vendor API exposes eight
   texture units, while its native compiler storage has separate hard limits
   of 32 TAU records and 96 ALU records.  glmark2 needs at most six fragment
   texture-unit slots (five statically active samplers in terrain, including
   unit 5) but up to 15 fetch instructions in an installed default scene.
6. Terrain's two vertex samples do not require immediate hardware TCL work.
   Chrome9 already executes vertex NIR through Mesa's draw module.  A proper
   generic software-VTF path can publish linear resource mappings through
   `draw_set_samplers`, `draw_set_sampler_views`, and
   `draw_set_mapped_texture`.  The local `m9x/m9_vs_texture.c` is a working
   Mesa implementation pattern for precisely this arrangement.  It remains
   necessary to retain per-stage sampler/view state and refresh an FBO-backed
   resource before each vertex draw.
7. No installed scene uses MRT, FBO blit, FBO multisample, or stencil.  The
   sources contain no `glDrawBuffers`, `glBlitFramebuffer`, secondary color
   attachment, or multisample-renderbuffer call.  One color target is enough.

## Current Chrome9 generic implementation

| Area | Current implementation | Consequence |
|---|---|---|
| color resources | linear guarded `B8G8R8X8_UNORM` and `B8G8R8A8_UNORM`; 2048 ceiling | sufficient storage for all installed RGB/RGBA assets and color FBO L0 images |
| depth resources | guarded `Z16_UNORM` only | sufficient for terrain's unsampled D16 renderbuffer, insufficient for depth textures |
| sampler formats | BGRX/BGRA and sample-only A8 (among other legacy formats); native A8 decodes to `(0,0,0,A)` | guarded cmd22 and Gallium A8 nearest/clamp sampling passed; repeat, shader discard, and the `ideas` scene remain unverified; depth views remain experimental |
| sampler filters | nearest/linear image filter; none/nearest/linear mip enum accepted | non-mip works; mip is useful only when the much narrower draw gate below accepts it |
| mip draw | one BGRX, square POT, clamp, <=512, complete levels through at most L9 | default crate mip can fit; RGBA, repeat, multi-unit, 1024 and R2T mip cannot |
| mip generation | CPU box generation for BGRX/BGRA/Z16 over guarded levels | correct implementation class, but the sampler path cannot consume most generated chains |
| fragment units | one or two, capability gated; diagnostic override can claim up to eight | `refract` fails link at three; the override must never be a production solution |
| fragment TEX cap | set equal to sampler count | repeated fetch programs such as effect2d and blur are under-advertised independently of their one sampler |
| vertex units | zero; bind/view callbacks ignore non-fragment stages | terrain is correctly skipped today |
| framebuffer | exactly one L0 color attachment plus optional Z16 | color FBOs and color+Z16 shapes exist; depth-only and D24 do not |
| clear | CPU full-channel BGRX/BGRA and depth-only Z16 | add format-correct D24 clear; masked color/stencil clears are not glmark2 prerequisites |
| R2T ordering | synchronous submits; normal texture bodies emit A00 cache-clear bit 0 | packet mechanism exists, but a guarded producer-to-consumer color and depth test is still required before relying on it broadly |
| copies | `resource_copy_region`; no `pipe->blit` | sufficient for these scenes; do not make blit a glmark2 dependency |
| render targets | one color target, no MSAA | matches both vendor FBO exposure and glmark2 usage |

Current target failures agree with that table: `refract` reports too many
fragment shader samplers, `shadow` fails `glTexImage2D(GL_DEPTH_COMPONENT)`
and creates an incomplete FBO, and `terrain` skips because
`GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` is zero.

## Exact installed-scene matrix

“Fetches” means static texture operations in the relevant default shader,
not distinct texture objects.  Shader ALU and instruction selection remain a
separate backend task, but the caps and resource bindings must allow these
programs to link and reach that backend.

| Scene / installed default variants | Texture and sampler contract | Offscreen/depth contract | Generic non-shader gap |
|---|---|---|---|
| `buffer` (map/subdata, interleaved/non-interleaved) | none | default framebuffer | no texture/FBO gap; preserve generic VBO map and subdata correctness |
| `build` (client arrays and VBO) | none | default framebuffer | no texture/FBO gap |
| `clear` | none | ordinary full color/depth clear by the main loop | existing full-mask color and Z16 clear are enough |
| `conditionals`, `function`, `loop`, `shading` | none | default framebuffer and normal window depth | no texture/FBO gap; remaining work is shader/raster state |
| `pulsar` default | no texture; source-alpha blending with destination alpha preserved | default framebuffer | existing blend/resource shape; optional `texture=true` is one BGRX nearest/clamp sampler |
| `texture` nearest/linear | one 512x512 RGB asset, BGRX, clamp; one fetch | default framebuffer | already inside generic single-level resource envelope |
| `texture:mipmap` | same BGRX asset, trilinear, complete L0..L9 | default framebuffer | current narrow mip command matches this one case, conditional on its capability and live correctness |
| `texture:linear-shader` optional | one sampler, four nearest fetches | default framebuffer | repeated TEX support, not another texture unit |
| `bump:high-poly` | none | default framebuffer with depth | no format gap |
| `bump:normals` | one 1024x1024 RGB/BGRX texture, nearest/clamp, one fetch | default framebuffer with depth | single-level 1024 sampling already fits |
| `bump:height` | gray+alpha PNG is expanded by the loader to RGBA/BGRA, nearest/clamp; one sampler and three fetches | default framebuffer with depth | repeated TEX support; no native gray format is required by this source version |
| `effect2d` | one 800x600 NPOT RGB/BGRX nearest texture; installed kernels issue 9 and 15 fetches | default framebuffer | NPOT L0 resource already fits; advertise/compile up to at least 15 TAU operations |
| `desktop:blur` | one linear/clamp RGBA sampler per pass; radius 5 separable shader issues 11 fetches | a chain of RGBA8 color-only FBOs, clears, and R2T transitions; source-over/separate-alpha blend | validate generic BGRA R2T ordering and 11-fetch programs; no depth, mip, MRT, or blit |
| `desktop:shadow` | RGBA window/shadow images, one linear/clamp sampler per draw | RGBA8 color FBOs and source-over/separate-alpha blend | same color R2T and blend contract; no depth or mip |
| `ideas` | one procedural 32x32 `GL_ALPHA` texture, nearest/repeat; shader reads alpha and discards | default framebuffer with depth | native sample-only A8 (`A30=301b0000`, logical `(0,0,0,A)`) and nearest/clamp are live; verify repeat, discard/depth interaction, and the scene before claiming support |
| `jellyfish` | unit 0 RGBA jellyfish plus unit 1 RGB caustic, linear; caustic repeats; two fetches and alpha blend | default framebuffer, depth disabled | generic two-unit formats/state already exist; remaining failure is shader backend |
| `shadow` | one raw depth sampler, nearest/clamp, one fetch; manual shader compare | 2x-canvas depth-only D24 texture FBO, depth clear, color mask zero during depth pass, then D24 R2T sampling | X8Z24 render+sample+clear, depth-only FBO lowering, color-mask-zero depth draw, D24 producer/consumer ordering |
| `refract` | unit 0 raw D24 depth nearest/clamp; unit 1 BGRA color RT trilinear/clamp; unit 2 RGBA image linear/clamp; three dependent fetches | 2x-canvas FBO with one RGBA8 color texture plus one D24 depth texture; color/depth clear; generate color mip chain after render | at least three FS slots, D24 render/sample, combined BGRA+D24, BGRA dynamic mip generation/sampling, multi-source R2T ordering |
| `terrain` | main VS samples `tNormal` and `tDisplacement`; main FS has five active samplers in units 0,1,2,4,5; three 512 RGB assets are trilinear/repeat; generated RGBA maps are linear or trilinear and clamp/repeat | height 256 RGBA FBO, normal 256 RGBA FBO, specular 512 RGBA FBO, screen-sized RGBA+Z16 terrain FBO, two 256 bloom FBOs, screen-sized tilt FBOs; repeated R2T and mip generation | per-stage state and software VTF for two VS samples; at least six FS slots/eight combined; generic mixed single/mip sources, BGRX/BGRA repeat mips, dynamic BGRA R2T mips, robust color+Z16 chains |

Terrain's main fragment shader declares six sampler uniforms, but `tNormal`
is dead in that stage.  Five are statically sampled.  Nevertheless glmark2
binds resources in units 0 through 5 and the active fragment unit 5 must be
representable.  Do not equate “five active sampler variables” with an array
that may only address indices 0 through 4.  Across stages the main program
has two vertex samplers and five fragment samplers; an eight-unit combined
contract covers it and agrees with the vendor API ceiling.

## Formats actually needed

| Gallium/storage format | Bind roles needed by glmark2 | Hardware evidence and policy |
|---|---|---|
| `B8G8R8X8_UNORM` | RGB upload, sampler, color RT/scanout as already used | A30/A52=`98`, live |
| `B8G8R8A8_UNORM` | RGBA upload, sampler, alpha-preserving color FBO | A30/A52=`99`, live in bounded paths |
| `A8_UNORM` | sample-only ideas logo | vendor Linux/Windows A30=`301b0000`; guarded cmd22 eight-alpha and Gallium nearest/clamp quadrant tests passed; repeat and alpha-discard remain separate validation gates |
| `Z16_UNORM` | terrain depth renderbuffer only | A12 format 0, live; never advertise it as sampleable |
| `X8Z24_UNORM` | raw depth texture, depth attachment, clear | private D24X8 `2032`, A12 code 6 and sampler A30=`30fe2000`; packet-exact static evidence, requires guarded live render/sample promotion |
| `S8_UINT_Z24_UNORM` | future stencil-capable depth texture/attachment | private D24S8 `2033`, same raw Z sampler plus stencil lane; not needed for glmark2, defer stencil state |

The GL_ALPHA mapping in Mesa first tries `PIPE_FORMAT_A8_UNORM`, which the
Chrome9 driver now exposes for sample-only use.  It falls back through
ordinary RGBA formats if A8 is unavailable, but the current sampler-view
validator rejects the non-identity swizzle needed by that fallback.  A8 must
remain sample-only: it is absent from the vendor render-target lookup.

Do not add sRGB, float color, S3TC, luminance, cube, 3D, rectangle, or
additional packed RT formats merely to run this glmark2 set.  They are useful
later for wider GL conformance and games, but they are independent gates.

## Mipmap contract needed beyond the first live case

The existing CPU generator is a conforming implementation strategy; using
the CPU is not a scene hack.  Its output must be consumable by a general
sampler path.  Required promotion points are:

1. both BGRX and BGRA chains;
2. clamp, repeat, and mirrored-repeat addressing independent of mip choice;
3. rectangular and NPOT chains, or a truthful format/size rejection before
   resource creation if silicon validation forces narrower limits;
4. actual view first/last levels, sampler min/max LOD and signed 5.5 LOD bias;
5. mip state for every used texture stage, including mixed mipmapped and
   single-level sources in one draw;
6. L0 rendered by the GPU, ordered retirement, CPU mip generation, cache
   publication, and subsequent GPU sample;
7. level coverage consistent with the advertised 2048 maximum.  Vendor
   constants state 12 levels (L0..L11).  The current L9/512 special case does
   not justify complete 1024 or 2048 textures.  Prove the vendor tail-entry
   behavior for L9..L11 or lower the truthful maximum for mip-capable use.

Use per-level distinct-color and explicit LOD-bias probes before derivative
selection.  Then test minification and trilinear blends at boundaries.  The
kernel must derive and range-check all offsets from a resource layout; never
accept raw userspace addresses or an offset outside the retained BO.

## Render-to-texture and FBO contract

Every producer-to-consumer transition needs all three independent operations:

1. producer completion/order;
2. CPU ownership/cache maintenance only if CPU mip generation or inspection
   intervenes; and
3. the consumer's normal type-FE A00 bit-0 texture-cache clear pulse.

An A00 pulse does not complete an outstanding producer, and a synchronous
wait does not invalidate the texture cache.  The current bounded packets
already use A00=`0x11` for one active texture.  Extend the active-texture
count correctly for later stages while preserving bit 0 on a write-to-sample
transition.

Required generic FBO shapes are exactly:

| Shape | Scenes |
|---|---|
| one BGRA8 color, no depth | desktop and most terrain intermediate passes |
| one BGRA8 color plus Z16 renderbuffer | terrain main offscreen pass |
| no color plus X8Z24 depth texture | shadow |
| one BGRA8 color texture plus X8Z24 depth texture | refract |

All attachments are level zero, one layer, and single-sampled.  No scene
requires rendering to a nonzero mip level.  Full color clears, full depth
clears, and combined color+depth clears suffice.  Shadow additionally needs a
draw with all color channels masked while depth remains writable; that is not
a masked clear.

## Safest implementation order

1. **Make format/layout selection table driven.**  Carry cpp, sampler A30,
   render A52, depth A12, legal bind roles, filter restrictions, and logical
   swizzle per format.  Keep sampler, color-render, and depth-render support
   independent.  Retain the guarded A8 sample-only path; extend its tests to
   repeat addressing and discard before treating `ideas` as supported.
2. **Promote guarded X8Z24 in isolation.**  Add 32-bpp layout, CPU clear and
   readback, A12 code 6 depth draw, then the exact raw sampler A30=`fe`
   nearest oracle with poisoned low bytes.  Never route D16 through it.
3. **Close generic depth FBO shapes.**  Test D24 depth-only with the internal
   scratch RT/color-mask-zero lowering, then BGRA+D24.  Verify clear values,
   compare results, guards, alias rejection, BO lifetime, and D24-to-D16
   transition closure.  Only then let Mesa expose a depth texture format.
4. **Validate color and depth R2T ordering.**  Use guarded source/prime/final
   images and the existing A00 clear/no-clear/force-miss discriminator.
   Repeat with rendered D24 sampled as raw intensity.  This is a prerequisite
   for desktop, shadow, refract, and terrain chains.
5. **Generalize mip state, not just the crate case.**  Prefer one structured
   semantic texture-array ABI capable of BGRX/BGRA, per-stage filter/wrap,
   validated mip tables, and mixed sources.  Test static uploads first, then
   GPU-L0 -> CPU-generate -> GPU-sample.  Promote L10/L11 only after silicon
   proof.
6. **Generalize fragment texture slots and TAU limits.**  Submit every active
   BO in one reservation/fence transaction, encode logical sampler ids 0..7,
   and initialize unused stage/mip state to closed values.  Prove one through
   eight stages with distinguishable constant textures and sparse logical
   slots.  Set `max_texture_samplers/views` no higher than tested, and set
   `max_tex_instructions` independently, up to the recovered 32-record limit.
   Remove the diagnostic sampler-cap override from any production decision.
7. **Wire software VTF through the draw module.**  Store sampler states/views
   per shader stage, publish mapped linear levels, refresh FBO-backed inputs,
   and advertise at least two vertex samplers only after nearest/linear,
   clamp/repeat, LOD-zero, and R2T-fed vertex tests pass.  Two VS plus six FS
   slots and eight combined are sufficient for terrain.
8. **Run generic tests before scenes.**  Each promotion must have raw guarded
   kernel tests, Gallium resource/view tests, and a small API-level FBO test.
   Then run `texture:mipmap`, `ideas`, `shadow`, `refract`, desktop, and
   terrain in that dependency order.  Shader support may change the last
   ordering, but resource caps must never be raised merely to make linking
   proceed.

## Minimum regression tests

* A8 nearest and linear, repeat including negative coordinates, alpha 0/255,
  discard and source-over, with all guards intact.
* X8Z24 clear values 0, 1 and midpoints; LESS/LEQUAL/EQUAL/ALWAYS, read-only
  and write; raw sample of `000000`, `555555`, `aaaaaa`, `ffffff` depth with
  distinct ignored low bytes.
* depth-only FBO and BGRA+D24 FBO completeness, clear, draw, detach/rebind,
  target/depth alias rejection, and closure back to BGRX+Z16.
* color R2T and depth R2T producer/consumer sequences with and without the
  exact A00 pulse for diagnosis.
* BGRX and BGRA mip chains at 512, 1024 and 2048; non-square/NPOT sizes;
  clamp/repeat/mirror; nearest-mip and trilinear; mixed single/mip multi-unit;
  rendered L0 followed by regeneration.
* fragment sampler slots 0..7 and sparse slots, repeated fetches through 32,
  rejected sampler id 8+, and all resource reservations/guards.
* two software vertex samplers reading distinct FBO-produced images, with
  view replacement/unbind and resource destruction after retirement.

## Evidence links inside this tree

* `fixed-function-texture-sync.md`: exact A8 and D24 sampler words, sampler
  format table, and R2T A00 ordering.
* `raster-front-end-oracles.md`: D16/D24X8/D24S8 A12 encodings and memory
  interpretation.
* `gl-feature-matrix.md`: vendor limit 8, 2048/12-level constants, one-RT FBO
  exposure, and absence of MRT/blit/MSAA advertisements.
* `via-5.75-shader-re.md`: four-bit native sampler id and 32 TAU/96 ALU
  compiler stores.
* `vx900-mipmap-next-oracle.md` and `mip-format-projective-oracles.md`: H3 mip
  tables, filters, and current bounded promotion gates.
* `resource-layout-clear-cache.md`: guarded resource layout and the exact
  producer-to-consumer cache discriminator.
* `legacy-texture-format-oracles.md`: native sample-only A8/L8/L8A8/packed
  format mappings.
