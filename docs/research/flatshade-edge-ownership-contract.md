# Chrome9 flat shading and triangle-edge ownership

Status: static cross-check complete; the A23 shading values are exact, but
the flat-color and shared-edge discriminators in this note have not yet run on
VX900 silicon.  Keep the current hardware Gouraud path for smooth draws and
do not claim general flat interpolation until the bounded tests below pass.

The Linux executable used here is `via_chrome9_dri.so`, SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
The Windows D3D and OpenGL executables have SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
and `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`.

## Exact H6 shading selector

The VX900 manual defines type-04 A23 bits `[14:12]` as solid, FlatA,
FlatB, FlatC, and Gouraud.  Preserving the proven triangle word's primitive,
edge, and winding fields gives the complete words:

| interpolation | A23 |
|---|---:|
| solid | `23000f02` |
| flat from vertex A | `23001f02` |
| flat from vertex B | `23002f02` |
| flat from vertex C | `23003f02` |
| Gouraud | `23004f02` |

This table has three independent executable cross-checks.

* The Windows D3D H6 triangle-list builder starts at
  `VTGUModeDX32A.dll:0x101f5350`.  Its shading branch at
  `0x101f559a..0x101f55f3` reads D3D `SHADEMODE` from context `+0x1e8c`.
  Public value 2 selects `0x4000` (Gouraud).  Public value 1 selects
  `0x3000` (FlatC) on internal selector 8 and `0x1000` (FlatA) on the
  alternate path.  The byte-identical OpenGL copy is the builder at
  `VTGOGL32A.dll:0x10249dd0`, with the same branch at
  `0x1024a01a..0x1024a073`.
* The D3D triangle-strip builder at `0x101f56d0` selects FlatC on the H6
  selector-8 path at `0x101f5943..0x101f5985`, then installs
  `A20=201c7800`.  The triangle-fan builder at `0x101f5aa0` computes FlatC
  for selector 8 and FlatB for the alternate path at
  `0x101f5d09..0x101f5d58`, then installs `A20=201c5c00`.  The difference is
  caused by which physical A/B/C slot is the API provoking vertex; it is not
  a different interpolation encoding.
* The proprietary Linux GL setters
  `stmTriangleStripPrimitiveSetting_invi:0x1bc3e0` and
  `stmTriangleListPrimitiveSetting_invi:0x1bc7f0` independently write the
  same H6 image byte.  At `0x1bc6b0..0x1bc6d5` and
  `0x1bc9a1..0x1bcac6`, internal shade value 1 selects byte bits `0x30`
  (FlatC) and value 2 selects `0x40` (Gouraud).  The list builder clears the
  A20 reuse fields at `0x1bc9aa..0x1bc9b1`; the strip builder writes reuse
  bytes `78/1c` at `0x1bc5a1..0x1bc5b5`.

Old Mesa provides a semantic, not bit-position, cross-check.
`unichrome/via_tris.c:932..989` uses FlatC for triangle lists, strips, fans,
and polygons and FlatB for lines.  Its older register generation locates the
field at different bits, so its `HC_HShading_*` constants must not be copied
into an H6 packet.

## What the current Gallium path does not yet guarantee

`chrome9_vbuf_need_pipeline()` always returns true and says that this makes
draw perform flat-shade fixup.  That statement is too broad.  Mesa's
`draw_pipe_validate.c` inserts `draw_pipe_flatshade` only when a later stage
needs precomputed flat values, such as wide/AA/stippled lines or unfilled
triangles.  An ordinary filled, unclipped triangle can reach the Chrome9 vbuf
renderer with distinct per-vertex colors.  The current kernel packets then
use `A23=23004f02`, so they Gouraud-interpolate those colors.

Clipping does preserve the selected provoking value for vertices it creates,
but it does not turn every untouched primitive into three identical color
records.  `need_pipeline=true` is therefore not by itself a flat-shading
implementation.

There is also a current capability bug.  `u_init_pipe_screen_caps()` defaults
`pipe_caps::flatshade` to true, and `chrome9_restrict_generic_caps()` does not
override it.  The state tracker therefore keeps fixed-function flat shading
as rasterizer state instead of lowering it, even though the Chrome9 packet
path neither selects FlatA/FlatC nor duplicates the provoking color.  The
immediate fail-closed correction is `caps->flatshade = false`; this can make
the present bounded fragment classifier reject the resulting explicit-flat
NIR, which is preferable to silently rendering a gradient.  Re-enable the
cap only after the D1/D2 lowering below passes.

The smallest implementation does not require a new kernel ABI.  For the
currently admitted fixed-function fragment classes, Mesa can select the
provoking vertex after topology decomposition and copy its packed D1 primary
color to all three submitted vertices while retaining `A23=23004f02`.
Texture coordinates remain untouched and continue to interpolate normally.
The same operation must be applied to D2 if a fragment class consumes
secondary color.

That lowering is valid only for fragment inputs known to occupy D1/D2.  A
future generic shader may mark an arbitrary varying `flat`; such a program
must be rejected until its packed-varying map can copy exactly the constant
inputs, or until native interpolation routing is understood.  Copying every
varying would silently turn smooth texture coordinates into flat ones.

Topology ownership must be explicit:

* a normalized triangle uses argument 0 when `flatshade_first` is true and
  argument 2 otherwise;
* both triangles made from one quad must receive the quad's API provoking
  value, not independently choose a vertex after decomposition;
* strips, fans, and quad strips must carry the original primitive's provoking
  index through winding correction and clipping.

The current quad expansion happens to use `(0,1,3)` and `(1,2,3)`, so the
last-vertex convention gives both triangles vertex 3.  Do not generalize that
accident to first-vertex or quad-strip cases.  If provenance is unavailable,
reject those flat topologies rather than emitting a visible color seam.

Native A23 FlatA/FlatC is a later optimization for known D1/D2-only programs.
It needs either structured raster state in every ioctl family or a run-level
state field, and must not be enabled in only one successor.  Software color
duplication is smaller and transition-neutral for the current path.

## Bounded flat-color oracle

Use a guarded 32-by-32 BGRX target, COPY, no depth, no cull, and the existing
primary-color program.  Submit one safely interior triangle whose A/B/C
colors are opaque red, green, and blue.

1. With first-vertex provoking, every covered pixel must be red.
2. With last-vertex provoking, every covered pixel must be blue.
3. Repeat after clipping one edge against the viewport and after reversing
   winding with culling disabled.  The chosen color must not change.
4. Repeat a quad and strip in which every possible source vertex has a
   different color.  Both generated triangles must use the API provoking
   color, including their shared diagonal.

For an isolated native diagnostic, retain the three different vertex colors
and change only A23 from `23004f02` to `23001f02` or `23003f02`.  FlatA must
produce red and FlatC blue.  Immediately follow it with a Gouraud triangle
and require an interior mixed color; this proves closure and catches stale
A23 state.  Guard pages, engine-idle state, and the complete target outside
the triangle must remain unchanged.

## Triangle fill and shared-edge ownership

No programmable top-left/bottom-edge selector has been recovered.  This is
a meaningful negative result:

* the manual's A23 bits `[11:9]` are explicitly edge flags for triangle
  wire-frame or antialiasing, not filled-triangle ownership;
* A23 bit 15 is explicitly the last-pixel control for lines;
* A01 bits 23/22 are explicitly line direction and diamond-rule controls;
* the Windows D3D and OpenGL triangle builders use the same A01/A23 fields.
  Their D3D `SHADEMODE`, cull, fill, edge-AA, and LASTPIXEL branches do not
  expose another fill-rule bit;
* the proprietary Linux triangle setters likewise change primitive, edge,
  cull, shade, and reuse fields without a top-left selector.

Thus filled-triangle ownership is most likely a fixed rasterizer convention,
with API orientation handled by the viewport transform.  This remains an
inference until pixel readback proves the physical rule.  In particular,
Gallium's `bottom_edge_rule` field cannot simply be accepted and ignored for
both values.  Mesa sets it to one for a top-origin GL framebuffer and toggles
it for upper-left clip origin.  Chrome9 currently does not expose
`ARB_clip_control` because `clip_halfz` is false, but ordinary drawable and
FBO orientation still deserve a differential check.

### Exact shared-diagonal discriminator

Clear a guarded 32-by-32 BGRX target to black.  Use additive `ONE,ONE`, no
depth/cull/AA, and constant source red `0x11`.  Draw a rectangle from
`(7.5,7.5)` through `(23.5,23.5)` as two separately submitted triangles:

```text
TL,TR,BR    TL,BR,BL
```

Every safely interior pixel on the shared `x=y` diagonal, for example
`(10,10)` through `(21,21)`, must have red `0x11`.  Red `0x00` is a gap and
`0x22` is double ownership.  Repeat with the opposite diagonal:

```text
TL,TR,BL    TR,BR,BL
```

and sample safely interior points on `x+y=31`.  Repeat both cases with draw
order reversed and with both triangle windings reversed while culling stays
off.  All variants must retain exactly one owner per shared-edge sample.

This oracle proves seam-free ownership but not which outer horizontal edge
is inclusive.  To label the rule, draw one triangle with integer-coordinate
horizontal and vertical edges and compare the full 32-by-32 result against
llvmpipe twice, once with `bottom_edge_rule=0` and once with it set.  Record
the first mismatching edge pixels rather than accepting only an interior
sample.  If Chrome9 matches only one value, validate that the state tracker's
viewport/orientation transform always converts the supported GL paths to
that physical rule before accepting the other value.

## Capability and validator boundary

Gallium does have `pipe_caps::flatshade`, and Chrome9 currently inherits the
unsafe true default.  Set it false while draws are rejected/lowered, then set
it true only once the provoking-color oracle passes.  The basic triangle fill
convention has no comparable opt-in cap.  Do not allocate a private kernel
capability merely for software D1/D2 duplication.  Keep these fail-closed
checks as well:

* reject flat fragment inputs outside the explicitly packed D1/D2 map;
* reject a flat topology for which the original provoking index is lost;
* keep A23 Gouraud for the software-duplicated path and reset any diagnostic
  FlatA/FlatC word before the next draw;
* keep triangle edge AA disabled during ownership tests, because A23 edge
  flags plus A01 bit 5 deliberately change boundary coverage;
* accept `bottom_edge_rule` only after the framebuffer-orientation matrix has
  a pixel-exact result, not merely because large interior triangles render.

The priority for glmark2 and ioquake3 is software D1/D2 provoking-color
closure followed by the additive shared-diagonal test.  Native FlatA/FlatC
and a generalized per-varying flat linker are optimizations after those core
paths are correct.
