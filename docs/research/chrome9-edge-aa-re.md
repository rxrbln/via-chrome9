<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 edge antialiasing (A01 bit 5, A23 edge flags)

Status: static reverse engineering, 2026-09-25.  Nothing in this note has run
on VX900 silicon.  It extends
[msaa-aa-viewport-clip-contract.md](msaa-aa-viewport-clip-contract.md) and
**corrects** its smooth-line recipe for VX900 (section 5).

Evidence:

| label | source |
|---|---|
| manual | `docs/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, printed pages |
| dri | `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, SHA-256 `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4` (i386, symbols) |
| win | `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll`, SHA-256 `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| source | current Chrome9 kernel/Mesa trees |
| **I** | inference, not observed |

## 1. Summary

* The whole AA facility is one A01 bit plus the A23 edge mask; the vendor
  drivers touch no other register for AA: no blend, alpha, A00 or A2x word
  (dri, all readers of the AA fields listed in section 3).
* VX900 (Linux UMD family 8, PCI `7122`) gets A01 bit 5 for
  * `GL_LINE_SMOOTH`: line lists and strips (width 1.0) and triangles in
    polygon mode LINE;
  * `GL_POLYGON_SMOOTH`: triangles in polygon mode FILL, **only while
    `GL_BLEND` is disabled**.  With blending enabled and a FILL face the
    vendor draws in software (`__glFillAntiAliasedTriangle`).
* Software fallbacks on VX900: smooth points (always), smooth lines wider than
  1.0 (all wide lines), polygon smooth + `GL_BLEND` + FILL, and different
  FRONT/BACK polygon modes when `LINE_SMOOTH != POLYGON_SMOOTH` (one A01 bit
  cannot carry both).
* The A23 smooth-line special cycle (`23004506`, strip A20 `202c6c00`) in the
  existing contract is gated on chip family **6** (PCI `3371`) in both vendor
  stacks.  VX900 never gets it: a VX900 smooth line keeps the normal words
  `A23 = 23004f01` and A20, and sets only A01 bit 5.
* Triangle edge flags under AA stay `111` (A23 `[11:9]`); only quads, quad
  strips and polygons in polygon mode LINE get `110` (the (c,a) diagonal is
  hidden).  Strip and fan interior edges are not masked.
* Coverage semantics (alpha output vs. blend into the destination) are not
  determinable statically.  The vendor fallback rule points to an internal
  destination blend for triangles (**I**, section 7), which the rung in
  section 9 settles.
* Mesa today: smooth lines go to llvmpipe (`chrome9_draw_smooth_lines()`);
  `poly_smooth` is referenced nowhere in the chrome9 driver or the draw
  module, so `GL_POLYGON_SMOOTH` is drawn **aliased on the hardware** today,
  not in llvmpipe (source).

## 2. Manual

A01, p.91: `5 WO xxh Enable Anti-Aliasing  0: Disable 1: Enable`.  The
neighbouring fields that matter here: bit 13 Coarse Z test ("only available
for triangles, not the lines"), bit 9 coarse-Z write, bit 6 colour write,
bit 1 stipple, bit 0 line pattern.

A23, p.161:

```text
15     Last Pixel Control for Drawing Line  0: discard the last pixel of each line
14:12  Shading: 000 solid, 001/010/011 flat via a/b/c, 100 Gouraud
11:9   Edge Flag.  Assume the vertex transmission sequence of a triangle is a, b, then c.
       000: Render NO Edge for triangle wire-frame or antialiasing
       1xx: Render Edge (a, b) for triangle wire-frame or antialiasing
       x1x: Render Edge (b, c) ...
       xx1: Render Edge (c, a) ...
6:5    primitive type for clockwise triangles (with bit 4 Hen2FRender)
3:0    0001 line, 0010 triangle, 0110 triangle wire-frame, 0111 triangle point
```

A text search of Part II for "anti", "coverage" and "edge" finds only these
two fields.  There is no coverage register, no AA blend mode and no alpha
coverage control.  The edge flags are defined for "wire-frame or
antialiasing", so on a filled triangle they select which edges are
antialiased (**I** from the wording; the vendor always sets all three).

## 3. Linux GL state flow (dri)

`__glEnableDisable` (`0x28cce0`) stores enable bytes in the GL context and
raises dirty bits:

| GL cap | enable byte | dirty word | vendor hwm field |
|---|---|---|---|
| `GL_POLYGON_SMOOTH` 0x0B41 | `+0x60e2` (`0x28d986`) | `+0xbf04` bit 16 | `+0x3c30` |
| `GL_LINE_SMOOTH` 0x0B20 | `+0x60e9` (`0x28e21a`) | `+0xbf04` bit 20 | `+0x3c28` |
| `GL_POINT_SMOOTH` 0x0B10 | `+0x6100` (`0x28d339`) | `+0xbf04` bit 24 | none: software only |
| `GL_BLEND` 0x0BE2 | `+0x60dd` (`0x28ed7c`) | `+0xbf00` bit 7 | read by the draw-path selector |

* `__glDpSTMUpdatePolygonMode` (`0x1ec910`) copies `+0x60e2` to hwm `+0x3c30`
  (`0x1ec9ea`) and marks server state `0xa9`; it maps front/back
  `GL_POINT/LINE/FILL` through the table at `0x3f76c0` = {1, 2, 3} into
  `+0x1dfc`/`+0x1e00`.
* `__glDpSTMLineStates` (`0x1ecc20`) copies `+0x60e9` to hwm `+0x3c28`
  (`0x1ecd65`) and marks server state `0xa7`.
* `STM_EdgeAntialias_INV` (`0x194400`, server state 27 in `server_states_inv`
  at `0x427000`, reloc `0x4270dc`) sets A01 bit 5 from hwm `+0x1ce0`.  No
  instruction in the dri stores to `+0x1ce0`.  It is the D3D-style
  `EDGEANTIALIAS` render state, and the GL path does not use it.

The AA fields are read only by the primitive builders (`STM_PrimitiveType_INV`
`0x1be1f0`, `stmTriangleList/Strip/LineList/LineStripPrimitiveSetting_invi`
`0x1bc7f0`/`0x1bc3e0`/`0x1bbf90`/`0x1bbcb0`), the software-VS H6H7 line draws,
and `STM_CoarseZ_INV` (`0x193bd8..0x193bf4`).  `STM_AlphaBlend_INV`,
`STM_AlphaTest_INV` and the colour-write setter do not read them.  **AA
never changes blend state.**

## 4. VX900 draw-path decision (dri)

`__glS3InvCreateContext` installs `__glS3H5UpdateDrawPrimitivePath`
(`0x230cb0`) for family 8 = PCI `7122` (`hwmContextCreate 0xab042`;
[chrome9-throughput-re.md](chrome9-throughput-re.md) §3.1).  It sets
`+0x3ac8 = 0` (hardware) and ORs 3 (software rasterizer, which includes the
SGI-derived `__glRenderAntiAliasLine 0x3bef10` and
`__glFillAntiAliasedTriangle 0x3c5b00`) when:

| condition (primitive = GL mode argument) | address |
|---|---|
| render mode FEEDBACK/SELECT | `0x230cf9` |
| front and back modes both non-POINT, different, and `LINE_SMOOTH != POLYGON_SMOOTH` | `0x230e39..0x230e71` |
| line width > 1.0 and a line primitive (1..3), or a triangle with a LINE face | `0x230f10..0x230f4c` |
| `POINT_SMOOTH` and a point primitive, or a triangle with a POINT face | `0x230f58..0x230f88` |
| `POLYGON_SMOOTH`, a triangle primitive, `GL_BLEND` on, and front or back mode FILL | `0x231060..0x231097` |
| a bound texture that needs software | `0x230fc0..0x230fd4` |

There is no fallback for `LINE_SMOOTH` on 1-pixel lines, with or without
blending.  The generic `__glS3InvUpdateDrawPrimitivePath` (`0x2308f0`, used by
other families) instead sends every smooth line primitive to software
(`0x230b62`).

## 5. Register programming on VX900 (dri)

The hardware image is `+0x9430`: A01 is byte `+0x4`, A23 is `+0x2148/+0x2149`,
A20 is `+0x213c..+0x213e`, and type-01 A85 is `+0x214`.  The family is
`[[+0x9464]+0x240]+0xc` (8 for VX900; 6 = PCI `3371`, `0xab00a`).

**Triangles** (`STM_PrimitiveType_INV` cases 4..6; for family 8 the branch at
`0x1be9e0`, identical to `stmTriangleListPrimitiveSetting_invi 0x1bca18`):

| face mode | A23 `[3:0]` or `[6:5]` | A01 bit 5 |
|---|---|---|
| FILL (3) | `0010` / `00` | `POLYGON_SMOOTH` (`0x1bf04a`, `0x1bf36d`) |
| LINE (2) | `0110` / `10` | `LINE_SMOOTH` (`0x1bf31a`, `0x1bf2e8`) |
| POINT (1) | `0111` / `11` | unchanged |

When the modes differ, A23 bit 4 is set and the front face's AA source is
written last, which is why the selector falls back when the two smooth enables
disagree.  Edge flags: `A23[11:9] = +0x1e04 & 7` (`0x1beb2a`).  The draw-path
selector writes `+0x1e04 = 7` for every draw (`0x230d42`) and 6 only for GL
QUADS, QUAD_STRIP and POLYGON (modes 7..9) with a LINE face (`0x23102b`).
Filled AA triangles, strips and fans therefore use all three edges.  The
kernel's filled word `23004f02` (or `23004e02`) is already the vendor AA
triangle word.

**Lines** (`stmLineListPrimitiveSetting_invi 0x1bbf90`, strip case `.L313`
`0x1be4e0`):

```text
A01 bit 5 = LINE_SMOOTH                                   0x1bbfc5 / 0x1bc1b0
family != 6 (VX900):  A23[3:0] = 1, A23[11:9] = 111, A85 bit 23 set  0x1bc0aa..0x1bc0c9
                      strip: A20[15:8] = 60, A20[21:16] = 20 (A20 = 20206000)  0x1be63f..0x1be653
family == 6 and smooth: A23 = (A23 & ~1f) | 6, A23[11:9] = 010, A20 = 20000000 (list)  0x1bc165..0x1bc19f
```

Thus on VX900 a smooth line list is `A20 20000000, A23 23004f01` and a strip
is `A20 20206000, A23 23004f01`, with A01 bit 5 set.  The
`23004506`/`202c6c00` words in
[msaa-aa-viewport-clip-contract.md](msaa-aa-viewport-clip-contract.md) belong
to family 6 and must not be used on VX900.  Windows has the same gate:
`VTGUModeDX32A:0x101f503f cmpl $0x6` on `[[+0x24c]+0xc]`, and the Windows
family table maps `7122` to 5 and `3371` to 6 (`0x100f2187..0x100f21df`).

## 6. Windows D3D (win)

* The render-state dispatcher (`0x100c9f82`, byte table `0x100caf00`, jump
  table `0x100cadc0`) sends `D3DRENDERSTATE_EDGEANTIALIAS` (40) to
  `0x100ca0dd`: store `+0x1d60`, dirty 27.  The dirty-27 setter `0x101fbd90`
  sets or clears A01 bit 5 from `+0x1d60`, the same logic as Linux
  `STM_EdgeAntialias_INV`.  D3D9 no longer defines state 40.
* `D3DRS_ANTIALIASEDLINEENABLE` (176) goes to `0x100ca101`: store `+0x1d64`,
  dirty `0x4b`.  The line builder reads `+0x3f1c` instead (`0x101f4f7b`); no
  store to `+0x3f1c` or `+0x3f24` was found in the DLL.  How 176 reaches the
  hardware bit is therefore unresolved.
* The Windows builder changes A01 bit 5 only when `[+0x9c3c]+0x1c == 8`
  (`0x101f4f67`, `0x101f5377`), a field not yet identified.  The triangle
  builder (`0x101f536c..0x101f548d`) is the same code as Linux: FILL takes
  `+0x3f24`, LINE takes `+0x3f1c`.
* `MULTISAMPLEANTIALIAS`/`MULTISAMPLEMASK` (161/162) are no-ops (dispatcher
  entry 3), as recorded before.  `RasterCaps 0x1000` (ANTIALIASEDGES) is set;
  `LineCaps` has no ANTIALIAS bit.

`VTGOGL32A.dll` was not examined for this note.

## 7. Blend and coverage semantics

Facts:

* No vendor path forces a blend function for AA; A00/A34..A37 come from the
  application state unchanged (section 3).
* The vendor keeps hardware smooth lines with any blend state, but refuses
  hardware polygon smooth when `GL_BLEND` is enabled.
* A23 bit 15 = 0 (last pixel discarded) in every vendor line word, so a
  connected strip does not blend a shared endpoint twice.

Inferences to be measured, not implemented:

* **I1**: AA on filled triangles blends coverage into the destination inside
  the pixel pipe.  This would be correct with blending off and doubled with
  application blending on, which explains the fallback.  The alternative, that
  the hardware writes coverage to alpha, cannot serve GL's canonical
  `SRC_ALPHA_SATURATE, ONE` polygon smooth.  Both readings predict the same
  vendor rule, so only silicon decides.
* **I2**: AA lines scale source alpha by coverage (the GL model with
  application `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`), because the vendor keeps
  hardware lines under blending.  If I1 also held for lines, blended smooth
  lines would be double-blended in the shipping driver.
* **I3**: AA pixels are touched outside the aliased footprint.  The vendor
  handles this through coarse Z: `STM_CoarseZ_INV` takes separate branches
  when `+0x1ce0`, `+0x3c28` or `+0x3c30` is set (`0x193bd8..0x193bfc` to
  `0x193e70`/`0x194280`).  The first AA work should therefore run with
  coarse Z and Fast-Z off.

## 8. Gallium mapping (proposal)

| Gallium state | hardware | fallback |
|---|---|---|
| `line_smooth`, `line_width <= 1`, no stipple, LINES or TRIANGLES with both faces LINE | A01 bit 5, normal line/wireframe A23 | draw/llvmpipe as today |
| `poly_smooth`, both faces FILL, blending disabled on RT0 | A01 bit 5, A23 unchanged | llvmpipe when blending is enabled (vendor rule), until I1 is measured |
| `point_smooth` | none | llvmpipe or the draw aapoint stage |
| smooth with different CW/CCW modes | only if both select the same AA enable | llvmpipe |
| `multisample` | bit clear (GL ignores smooth under multisample) | none |

Gallium has no cap for polygon smooth.  `poly_smooth` must stop being
ignored: route it to llvmpipe until the rung passes, then use hardware in the
blend-off case.  UAPI shape (**I**, the numbers are proposals): one raster flag
`DRM_CHROME9_RASTER_EDGE_AA`, the next free bit (`1U << 3` in today's header),
behind a new CAP2 bit (next free: 25).  The kernel ORs `0x20` into its own A01
and keeps its own A23 edge mask; userspace never supplies either word.  It
rejects the flag for point topology and for POINT polygon modes.  The kernel
clears coarse-Z test/write (A01 bits 13/9) while the flag is set.

## 9. Smallest silicon rung `edgeaa`

Harness: `tools/re/chrome9_rq_rungs.c` style: a 64x64 BGRA guarded target,
TCL pass-through VS, FS `oC0 = T0`, no depth, coarse Z off, one draw per job.
The only change from existing rungs is the A01 word: kernel A01 `| 0x20`
(`01c01540 -> 01c01560`, with TCL clip `01d01d40 -> 01d01d60`).

Triangle `a=(8.3,5.7) b=(57.1,19.4) c=(15.6,58.2)` (no edge along a pixel
row/column), vertex colour white alpha `ff` (E4: alpha `80`), `A23 23004f02`:

| step | blend (A00 bit 4; A34..A37) | prefill | pass/record |
|---|---|---|---|
| E1 | off | `ff000000` | reference image R0 (aliased) |
| E2 | off, AA on | `ff000000` | pixels with centre distance >= 1.5 px inside/outside equal R0.  Record boundary RGBA: RGB fractional = I1, only alpha fractional = coverage-to-alpha, identical = inert |
| E3 | source-over `34000020 35000148 36000410 37000244`, AA on | `ff000000` | boundary values vs E2: equal = coverage applied once, before or instead of blending; darker = double application |
| E4 | as E3, source alpha `80` | `ff00ff00` | separates coverage x alpha from alpha replacement |
| E5 | off, AA off | `ff000000` | byte-identical to E1 (state closure) |

Lines, `A23 23004f01`, `A20 20000000`, A01 bit 5 (the VX900 vendor words,
**not** `23004506`):

| step | geometry | pass/record |
|---|---|---|
| L1 | horizontal (8.5,20.5)-(24.5,20.5), blend off | record the pixel set vs. rung `lines` L1; record rows 19/21 |
| L2 | diagonal (8.5,8.5)-(40.5,24.5), blend off then source-over | fractional neighbours present, main span full intensity, forward == reversed endpoints |
| L3 | strip `A20 20206000`, two segments sharing (24.5,20.5), source-over | no crack, join pixel not blended twice |

Promotion rule: native polygon smooth needs E2 to show a monotone boundary
ramp and E5 exact.  Blend-on polygon smooth stays in llvmpipe unless E3/E4
match GL's `SRC_ALPHA_SATURATE` model.  Native smooth lines need L1..L3 to
match a coverage-alpha model within 1 LSB under source-over.

## 9a. Silicon results, 2026-09-25

GL probe `~/tmp/eaa/eaa.c` (64x64 FBO, triangle a/b/c above, a one-pixel
line (30.5,50.5)-(60.5,62.5)), kernel knob `edge_aa_re=0x20`:

* Blend off, white on black: the triangle's edge pixel on row 32 becomes
  `787878`, the line a smooth two-to-three pixel profile (`606060 c7c7c7
  cfcfcf 686868 080808`).  On a green destination the same pixels are
  `78ff78` etc.: coverage x source + (1 - coverage) x destination, with
  the destination alpha unchanged.
* With source-over blending and source alpha 1 the result is identical
  (GL's coverage-alpha model gives the same).  With source alpha 0.5 the
  edge pixels are brighter than GL's model (triangle `5cff5c` and an
  interior-adjacent `a9ffa9` over an interior `80ff80`): coverage is
  applied after the blend again.
* Implemented: kernel `64242a2` (CAP2 bit 25, `RASTER_EDGE_AA`, lines and
  filled triangles, coarse-Z bits cleared), Mesa: one-pixel unstippled
  smooth lines and blend-off polygon smooth.  The piglit line and polygon
  subsets are unchanged (line-aa-width, line-smooth-coverage,
  line-smooth-stipple pass on the hardware path).

## 10. Open items

* The Windows field `[+0x9c3c]+0x1c == 8` that gates A01 bit 5 in the D3D
  builders, and the path from D3D state 176 to `+0x3f1c`.
* Whether the hardware uses the edge flags for coverage on filled triangles
  (edges `000` + AA should then equal aliased).  Optional rung E6: A23
  `23004102`.
* Coarse-Z/Fast-Z interaction (I3).
