# Chrome9 MSAA, edge-AA, viewport, and clip contract

Status: exact static negative MSAA result, exact edge-AA and smooth-line
packet deltas, and implementation-ready software viewport/clip boundary,
2026-09-21.  No state in this note is a claim that Chrome9 has multisample
render targets.  Native AA and native clip-space promotion remain behind the
pixel oracles below.

## Executive contract

The safe Gallium envelope is deliberately asymmetric:

| feature | initial policy | reason |
|---|---|---|
| render/storage samples | exactly one | both D3D9 multisample render states are no-ops; no sample layout or resolve is recovered |
| triangle edge AA | reject / debug oracle only | A01 bit 5 is exact, but coverage-alpha and blend interaction are not measured |
| smooth line | software fallback | H6 requires a special A20/A23 cycle in addition to A01 bit 5 |
| smooth point | software fallback | the native point builder has no AA branch; the ICD contains a software AA-point patch |
| viewport 0 | Gallium draw transformation | current vertices are already clipped and post-viewport |
| additional viewports | unsupported | no verified index or array state |
| API/user clip planes | shader/software lowering only | the proprietary H5/H6 setters are empty and current shader ABI has no clip-distance output |
| scissor | existing capability 11 | the half-open A82/A83 path is already live |

No command or capability number is reserved here.  In particular, an
`EDGE_AA` bit must not be represented by a multisample resource capability.
The next UAPI allocation remains available to an application-observed
feature with a complete state envelope.

## Independent negative proof for multisampling

The VX900 manual names A01 bit 5 only **Enable Anti-Aliasing**.  It documents
no sample count, sample positions, sample mask, multisample surface layout,
per-sample depth/stencil layout, resolve command, or multisample clear state.
The bit is consumed together with A23's three per-edge flags; that is an edge
rasterizer facility, not a resource layout.

Windows supplies a stronger executable result.  D3D9 render states 161 and
162 are `D3DRS_MULTISAMPLEANTIALIAS` and `D3DRS_MULTISAMPLEMASK`.
`VTGUModeDX32A.dll` subtracts one from the public state and indexes the byte
dispatcher at `0x100caf00`.  Table entries 160 and 161 are both `03`.
Dispatcher entry 3 is `0x100cada9`, the common return/no-op path.  Therefore
neither public state contributes a word to the normal Chrome9 draw image.

The Linux driver agrees independently:

* `s3gFillInModes:0x2cf980` passes no sample-count array and all visuals have
  zero sample buffers;
* neither `GL_ARB_multisample` nor `GL_EXT_framebuffer_multisample` is in the
  advertised extension string;
* `sfmSetSurfaceMultiSampleTech:0x0dcd40` only stores its value at surface
  `+0x1cc`; and
* `sfmMultisampleOn:0x0b3c90` is a software predicate over object flags, not a
  packet builder.

The registry values `S3GDW_MultiSampleCount` and
`S3GDW_MultiSampleCountOGL`, and the default sentinel `80000000`, do not
override those results.  They show a policy knob, not a validated surface
encoding.

Mesa must consequently keep both `nr_samples` and `nr_storage_samples` at
zero/one.  `chrome9_resource.c` already rejects either count above one in
resource creation and format support.  Do not implement a fake resolve with
ordinary blits: without a recovered sample layout it can read outside the
validated BO or silently reinterpret samples as pixels.

Promotion requires all of the following, not merely an image that looks
smoother: a guarded allocation formula, color and Z/S sample ordering,
sample-mask state, full/partial clears, resolve direction and rounding, and
clean transitions back to single-sample render and scanout.  Until then
Gallium's sample-count query and every MSAA format query must return only 1.

## Exact edge-AA state

Linux `STM_EdgeAntialias_INV:0x194400..0x194444` reads the boolean at context
`+0x1ce0`, clears or sets byte bit `0x20` in hardware-image A01, marks type-01
state dirty, and returns.  Thus the complete delta on the live color image is:

```text
off: A01 = 01c01540
 on: A01 = 01c01560
```

The manual independently calls A01 bit 5 anti-alias enable and A23 bits
`[11:9]` the three edge flags used by wireframe or antialiasing.  The normal
triangle word `23004f02` already selects all three edges.  A bounded triangle
experiment therefore changes only A01 bit 5.  Rebuild the full A01 word;
never OR bit 5 into inherited depth, stencil, cull, or clip state.

For existing state-complete families, A01 stays at its current semantic slot:

| body family | A01 index without/with blend |
|---|---:|
| untextured color/D16 | `19 / 23` |
| one texture D16 | `44 / 48` |
| two texture D16 | `64 / 68` |
| proposed command 17 D24S8 | `23` |

Those indices do not authorize AA in those ABIs.  They show that a future
semantic flag needs no packet growth.  A validator would accept one enable
bit only, require triangle topology and nonzero A23 edge mask, and reject
multisample resources, logic-op, stipple, wireframe/point polygon modes, and
unknown shader output-alpha behavior.  The kernel would synthesize A01 bit 5
and every A23 edge bit; userspace must never submit raw words.

### Coverage-alpha oracle

Use a guard-surrounded 32x32 BGRA target, full scissor, a white opaque source,
black prefill, and a non-axis-aligned triangle whose three edges cross pixel
centers at unrelated phases.  Submit `off -> on -> off`, changing only A01
bit 5.  Compare every pixel, not a screenshot.

1. Under COPY, interior pixels must remain byte-identical white and pixels
   far outside must remain black.  Record every changed boundary value.
2. Repeat with source-over, source alpha `ff`, over black.  If boundary RGB is
   fractional only with blending, AA is exporting coverage through alpha or
   the blend path.  If COPY itself becomes fractional, coverage is applied
   before the write without relying on blend.
3. Repeat source-over with source alpha `80` over opaque green.  This
   distinguishes coverage multiplication from replacement of shader alpha.
4. Disable AA and require the first image exactly, then run a legacy
   textured/depth draw to prove state closure.

Do not expose `PIPE_CAP_POLYGON_SMOOTH` or any multisample cap from a clean
retirement alone.  Promotion requires a stable, API-correct boundary image
for COPY and blend and a measured rule for shared triangle edges.

## Smooth-line special cycle

Both proprietary drivers show that H6 smooth lines require more state than
A01 bit 5.  In the Windows line-list builder
`VTGUModeDX32A:0x101f4f50..0x101f5136`, the effective-AA flag at context
`+0x3f1c` first controls A01 bit 5.  On the H6-specific branch
`0x101f5054..0x101f509a`, the builder applies:

```text
A23 = (A23 & fffff5f6) | 00000406
```

to the current shade/orientation image.  From the proven Gouraud line state
this is exactly:

```text
normal line list: A20=20000000 A23=23004f01
smooth line list: A20=20000000 A23=23004506
```

The independently compiled Linux function
`stmLineListPrimitiveSetting_invi:0x1bbf90`, specifically
`0x1bc165..0x1bc19f`, performs the same byte operations when its effective-AA
state at context `+0x3c28` is set.

Line strip adds a special reuse cycle.  Windows
`0x101f5274..0x101f5290` and Linux
`stmLineStripPrimitiveSetting_invi:0x1bbe95..0x1bbee1` synthesize:

```text
A20 = (A20 & ffec6cff) | 002c6c00 = 202c6c00
A23 = 23004506
```

for the established strip image.  A23 low nibble `6` is the manual's
triangle-wireframe primitive, and only the selected edge remains active.
This explains why the ordinary `23004f01` line recipe plus A01 bit 5 is not a
valid smooth-line implementation.

A23 bit 15 is independently documented as last-pixel control.  Both normal
and recovered smooth words leave it zero, discarding the final pixel of each
line.  Keep it zero for connected primitives so a shared endpoint is not
double blended.  A01 bit 23 canonicalizes vertical/horizontal direction;
A01 bit 22 enables the diamond rule.  The live baseline `01c01540` has both
set, so endpoint reversal should not change the coverage set.

The first smooth-line oracle is a four-way matrix: list/strip times forward/
reversed endpoints, first with COPY and then alpha blending.  Test horizontal,
vertical, both diagonal major axes, and a two-segment join.  Require reversal
invariance, no crack, and no twice-blended join.  Then run smooth-off again.
Native line smoothing remains rejected until that matrix passes; draw-based
triangle expansion is the safe fallback.

The point-list builder has no corresponding AA branch.  The Linux image also
exports `__glDpInitAAPointPatch`, `__glDpEnableAAPointPatch`, and
`__glDpResetAAPointPatch`, positive evidence for a software point-smoothing
path.  A01 bit 5 must not be guessed onto `23001f00`.  Smooth points and point
sprites remain software-expanded; ordinary 1x1/2x2 native points retain the
separate oracle in [line-point-raster-contract.md](line-point-raster-contract.md).

## Viewport and clipping boundary

### Current post-viewport path

The current Chrome9 vbuf renderer receives Gallium draw output after
homogeneous clipping and viewport transformation.  It converts finite X/Y to
signed 16.16 and the kernel accepts exactly:

```text
-0.5 <= x <= width  - 0.5
-0.5 <= y <= height - 0.5
1 <= width,height <= 2048
```

This is literal code in `chrome9_coordinate_valid()`:
`minimum=-0x8000`, `maximum=(limit<<16)-0x8000`.  The kernel additionally
rejects zero-area triangles before publication.  Scissor is independently a
half-open integer rectangle, `min <= max <= target dimension`; an equal pair
is a successful no-op at the Mesa submission boundary.

For this path retain A01 clipping-engine bit 11 clear and retain the complete
proven A40--A52 tuple.  The software-transformed position is the rasterizer
input; enabling native clipping would reinterpret it as clip coordinates and
apply the viewport twice.

This design already supports arbitrary API viewport origin/scale/depth range
inside target bounds because Gallium draw bakes them into the vertex.  There
is no new UAPI viewport field and no capability bit.  `set_viewport_states`
must continue forwarding viewport 0 into draw.  More than one viewport stays
unsupported because neither a native selector nor a per-primitive viewport
index has been verified.

### Native clip-space path is a different ABI

For a future native VS path, A01 bit 11 enables the homogeneous clipping
engine.  A23 bits 23 and 22 must stay zero: the manual defines those values as
enabling near clipping for triangle-point mode and the W=0 clipping ECO.  The
normal C-to-screen coefficients are split across A40--A48, but the manual
requires the same scales in VS c256 and offsets in c257.  Therefore A40--A48,
c256/c257, the VS finalizer, A01 bit 11, and raw clip-space inputs are one
indivisible template.

The exact split packing is:

```text
A40=40000000|(xs&ffffff)  A41=41000000|(xo&ffffff)
A42=42000000|((xo>>16)&ff00)|(xs>>24)
A43=43000000|(ys&ffffff)  A44=44000000|(yo&ffffff)
A45=45000000|((yo>>16)&ff00)|(ys>>24)
A46=46000000|(zs&ffffff)  A47=47000000|(zo&ffffff)
A48=48000000|(area_exp<<16)|((zo>>16)&ff00)|(zs>>24)
```

Linux `STM_ViewPort_INV:0x1bab80` independently performs this packing and
dirties A40--A48.  A50--A52 are coordinate clamps and area thresholds, not a
second API viewport.  The current unusual values and c256/c257 constants are
live only as one shader-specific post-viewport linkage block; changing A40
alone is unsafe.

Native clipping needs the bounded command 10/11 design and its six-plane
oracle.  It must validate finite matrix/input values and use full state
templates; it cannot be bolted onto the existing immediate RHW commands.

### User clip planes

The manual documents only frustum/W clipping and the post-transform pixel-Z
interval A1E--A20.  The latter drops pixels outside `[near,far]` before depth
test; it is not an arbitrary clip plane.  Both proprietary H5/H6 exports
`STM_Clipping_H5:0x1bab30`, `STM_ClipPlaneEnable_H5:0x1bab40`, and
`STM_SetClipPlane_H5:0x1bab50` immediately return.  There is consequently no
cross-checked fixed-function user-plane packet.

Keep `pipe_caps.clip_planes=0`.  Applications needing `gl_ClipDistance`
require a verified VS output/linkage and clip-distance lowering or software
clipping in draw.  Do not claim the capability merely because ordinary
frustum clipping works.  Kernel requests continue to reject unknown clip
flags; no plane equation crosses the UAPI.

## Regression and failure policy

Run each diagnostic on a fresh guarded target and check the exact whole image,
guard pages, completion cookie, PAUSE/current state, work mask, and `040c`.
The minimum transition suite is:

1. triangle AA off/on/off;
2. normal line list -> smooth line list -> normal line strip;
3. 1x1 point -> line -> triangle, proving A2A/A2B and special A20/A23 state do
   not leak;
4. small API viewport plus full scissor -> full viewport plus small scissor;
5. software-clipped triangle crossing each of the six homogeneous planes,
   compared with a CPU reference; and
6. final textured D16 draw and 2D fill closure.

A pixel mismatch disables only the unpromoted AA/native-line key.  Timeout,
guard mutation, unexpected engine error, or retirement-state mismatch invokes
the existing global 3D quarantine.  Validation failures publish no commands.
No test should intentionally submit an unknown multisample layout or raw user
clip-plane state.
