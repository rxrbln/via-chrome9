<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 lines, points, two-sided color and user clip planes

Status: static reverse engineering plus manual cross-check, 2026-09-24.
Offline only; nothing in this note has run on VX900 silicon.  It extends
[line-point-raster-contract.md](line-point-raster-contract.md),
[stipple-polygon-mode-contract.md](stipple-polygon-mode-contract.md),
[chrome9-face-sign-bfc-producer.md](chrome9-face-sign-bfc-producer.md) and
section 2 of [ioquake3-blocker-roadmap-re.md](ioquake3-blocker-roadmap-re.md),
and records one new result: **VX900 has a hardware per-pixel user clip-plane
test** (front-end type-02 A05), fed by vertex-shader DP4s into spare texture
coordinates.  Earlier notes treated user clip planes as absent because the
`STM_*ClipPlane*_H5` setters are empty; the real producer is elsewhere.

Evidence labels used below:

* **manual**: VX900 `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf` (text
  extraction line numbers `M:<line>`), unless another manual is named;
* **static**: disassembly of the Linux ICD
  `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so` (SHA-256
  `fddb4dea…a69af4`), addresses as `dri:0x…`;
* **cross**: the same fact in two independently compiled vendor binaries;
* **inference**: derived, not observed; each such statement is marked.

## 1. Summary

| feature | hardware mechanism | vendor use | silicon status |
|---|---|---|---|
| point list | A23[3:0]=0, A24=points, A20=`20000000` | yes | unmeasured |
| line list / strip | A23[3:0]=1, A20 `20000000` / `20206000` | yes | unmeasured |
| tri strip / fan | A20 `201c7800` / `201c5c00`, A24=prims+2 | yes | unmeasured (lists only live) |
| point size (state) | A2A max/width, A2B min/height, float s[8].15 | yes | unmeasured |
| point size from VS (`oPts`, dest 0x27) | A25 bit 19 + A2A/A2B as clamp | yes (`STM_PointSize_H5`) | unmeasured |
| point sprite coords | A01 bit 21; A2F[9:0] per coordinate | A01 bit 21 only found | unmeasured |
| wide lines | **no register**; vendor replicates 1-px lines | yes (software) | n/a |
| line stipple | A01[0], A84/A85 | yes | unmeasured |
| polygon mode | A23[3:0]/[6:5]/[4] per physical winding | yes | unmeasured |
| two-sided color | VS dest 36/37 (oD2/oD3 = oBFD0/1) + A61[19] + A23[7] | yes (GL FF VS) | unmeasured |
| `gl_FrontFacing` | no pixel-shader face register | not in vendor PS | PRIMARY-marker only |
| user clip planes | FE A05 two UCP coordinate groups + VS DP4 | yes (GL FF and programmable VS) | unmeasured |

The Mesa premise in the task is only partly current: Chrome9 Gallium
`chrome9_vbuf_draw_arrays()` / `draw_elements()` reject `MESA_PRIM_POINTS`
and all line primitives (`chrome9_draw.c:3249`, `:3316`, `CHROME9_REJECT`);
no `draw_wide_point_threshold()` / `draw_enable_point_sprites()` call exists
in the driver.  Wide lines (width > 1) are expanded by draw's default wide-line
stage, but points and one-pixel lines currently produce no rendering.

## 2. Current driver state

* Kernel post-TNL cmd22: `chrome9_fragment_raster_state()`
  (`linux-7.2/drivers/gpu/drm/chrome9/chrome9_engine.c:2078`) always emits
  `20000000 23004f02 24000000|vertex_count`.
* Kernel TCL: `chrome9_tcl_raster_state()` (`chrome9_engine.c:2153`) emits
  A20=`20000000`, A23 `23004f02`/`23004e02` (bit 8 from
  `DRM_CHROME9_VERTEX_CULL_CW`) with bits 14:12 Gouraud (4) or FlatC (3), and
  A24=`vertex_count`; validation at `chrome9_engine.c:8500..8542` requires
  `vertex_count % 3 == 0` and sets internal rows 258.xy = `49400000`.
* Kernel VS compiler (`chrome9_vertex.c:25..31`) knows destinations
  `0x10` temp, `0x22/0x23` oD0/oD1, `0x28+n` oTn and `0xa0` position; it
  always fills oD1 (and oD0 when PRIMARY is unused) with literal zero
  (`chrome9_vertex.c:498..500`).
* UAPI: `drm_chrome9_draw_vertex_program.flags` accepts only
  `CULL_CW`, `CULL_CCW`, `FLAT` (`chrome9_drm.h:1293..1295`); vertex
  destinations are TEMP/POSITION/PRIMARY/VARYING (`chrome9_drm.h:1237`).
  The highest allocated capability bit is 59 (`CAP_VERTEX_BUFFERS`).
* Mesa TCL gate: `chrome9_tcl_draw()` (`chrome9_tcl.c:1513..1516`) falls back
  for non-FILL polygon modes, `light_twoside`, `clip_plane_enable`, and any
  non-triangle topology; `tcl_emit_store` (`chrome9_tcl.c:744..750`) rejects
  CLIP_DIST0/1, CLIP_VERTEX, BFC0/1 outputs.
* Mesa caps: `two_sided_color=false`, `clip_planes=0`
  (`chrome9_screen.c:428..430`).  Consequently the state tracker lowers
  two-sided color into the fragment shader through `nir_lower_two_sided_color`
  (FACE-based, `st_program.c:1108..1110`, `st_atom_shader.c:147`) and lowers
  enabled user planes into VS `gl_ClipDistance` writes through
  `nir_lower_clip_vs` (`st_program.c:694..720`, `st_atom_shader.c:247`), with
  `MaxClipPlanes = 8` (`st_extensions.c:1286`).  Gallium draw then clips
  against those distances in software, so user planes are already correct
  on the draw path; two-sided color works only when the FACE-lowered shader is
  specialisable (cull-derived `face_mode`, `chrome9_context.c:5527..5585`).

## 3. Native primitive encoding

### 3.1 A20 vertex-reuse fields (manual M:9428, type-04 sub 20h)

```text
A20[21:16]  H2nd1VT: second/even single-vertex cycle, 2 bits each for a,b,c
A20[15:8]   render mode: 00 = full cycle; bit 14 = automatic fast
            single-vertex cycle; bits 13:12/11:10/9:8 = a/b/c source for the
            first/odd single-vertex cycle
            source code: 00 new, 01 prev a, 10 prev b, 11 prev c
A20[1:0]    index mode (00 immediate, 01 16-bit, 11 32-bit)
```

Decoding the vendor words against these fields (inference from manual text;
the words themselves are static/cross, see
[indexed-vbo-topology-plan.md](indexed-vbo-topology-plan.md) and
[line-point-raster-contract.md](line-point-raster-contract.md)):

| topology | A20 | odd cycle (bits 13:8) | even cycle (21:16) | resulting primitive k>0 |
|---|---:|---|---|---|
| point list | `20000000` | n/a | n/a | one vertex per point |
| line list | `20000000` | full 2-vertex cycle | n/a | `(v2k, v2k+1)` |
| line strip | `20206000` | a←prev b, b new | a←prev b, b new | `(vk, vk+1)` |
| triangle list | `20000000` | full 3-vertex cycle | n/a | `(v3k..v3k+2)` |
| triangle strip | `201c7800` | a←prev c, b←prev b, c new | a←prev a, b←prev c, c new | odd `(vk+1, vk, vk+2)`, even `(vk, vk+1, vk+2)` — GL winding preserved |
| triangle fan | `201c5c00` | a←prev a, b←prev c, c new | same | `(v0, vk+1, vk+2)` |

For native 16-bit indices OR `1` into A20; for 32-bit OR `3`.

### 3.2 A23 fields (manual M:9503..9573, printed p.161)

```text
23      disable clipping for triangle-point (near plane only)
22      disable W=0 clipping plane
21..19  clipping-engine / 3D ECO disables (keep 0)
15      last pixel of line: 0 discard, 1 draw
14:12   shading: 0 solid, 1 FlatA, 2 FlatB, 3 FlatC, 4 Gouraud
11:9    edge flags (a,b),(b,c),(c,a) for wireframe/AA
8       culled physical winding: 0 CW, 1 CCW (enable is A01 bit 7)
7       oBFD# selection: 0 CW triangles use oBFD, 1 CCW triangles use oBFD
6:5     primitive type for CW triangles when bit 4 set:
        00 triangle, 10 wireframe, 11 point
4       Hen2FRender: different PMType for front/back
3:0     primitive type (CCW when bit 4 set): 0 point, 1 line,
        2 triangle, 4 rectangle, 6 wireframe triangle, 7 point triangle
```

Vendor defaults: point `23001f00` (FlatA, edges 111, bit 8 set), line
`23004f01`, triangle `23004f02` (static/cross,
`VTGUModeDX32A.dll:0x101f4ec5..0x101f4ee6`, `:0x101f509c`,
`dri:0x1bbc32..0x1bbc53`).  Flat shading on lines needs the provoking vertex:
GL provoking-last for a line is vertex b, i.e. FlatB (`0x2000`); this is an
inference by analogy with the measured FlatC = GL provoking-last for
triangles (chrome9-hw-tcl-re.md L13b) and must be measured.

### 3.3 Exact per-primitive A24 counts

A24[23:0] is the number of **vertices** (or indices) in the primitive list,
never payload dwords (manual M:9576; static/cross):

| topology | A24 | validation |
|---|---|---|
| point list | `N` | N ≥ 1 |
| line list | `2·L` | even, ≥ 2 |
| line strip | `L+1` | ≥ 2 |
| triangle list | `3·T` | multiple of 3, ≥ 3 |
| triangle strip / fan | `T+2` | ≥ 3 |

Splitting a strip or fan across command segments requires re-sending the
reused vertices (last one for a line strip; last two for a triangle strip
with parity; first plus last for a fan).  The hardware reuse cache is not
proven to survive an A20/A23/A24 re-emission, so a split strip must be
re-issued as a fresh strip rather than continued (inference).

### 3.4 Point and triangle-point viewport bias

`STM_ViewPort_INV` reads the A23 low nibble (`dri:0x1babc4..0x1babcd`) and
writes internal row 258.xy = `49400000` (786432, the 1/16-pixel snap bias)
only when the nibble is neither 0 (point) nor 7 (triangle point)
(`dri:0x1bb6ee..0x1bb713`); the same condition gates the bias inside the
row-257 viewport offsets (chrome9-hw-tcl-re.md, viewport formula).  The
kernel currently hard-codes rows 258.xy = `49400000`
(`chrome9_engine.c:8542..8543`) and the UAPI documents that userspace ox/oy
already include the bias (`chrome9_drm.h` comment above
`drm_chrome9_draw_vertex_program`).  **A native point draw must use zero bias
in rows 257/258 and in the A40..A48 clip-engine copies.**  The post-TNL
cmd22 program adds its bias from row 257 (`vs_float_constants`,
`chrome9_engine.c:2621..2624`), so a cmd22 point request must upload
`(1,1,1,1),(0,0,0,0)` instead.  Whether the bias only affects snapping or also
moves the point centre is a measurement item (L-P3 below).

### 3.5 Point size

Manual (M:10025..10050): A2A is the default width, A2B the default height,
both s[8].15, range 1.0..2048.0.  A25 bit 19 (M:9608) declares that the
primitive vertex carries a point size; then

```text
point_width = max(A2B, min(A2A, vertex.point_size))
```

so A2A/A2B become the max/min clamp.  `STM_PointSize_H5` (`dri:0x193730`)
implements exactly this (static):

* if the bound VS object flags `+0x39e8` bit 5 (the compiler output-linkage
  bit for `oPts`, see via-5.75-shader-re.md) or the FVF declares a size
  (`+0x204` bit 5): A2A = FloatToP_E8_N(max,15) (`hw+0x20a8`), A2B =
  FloatToP_E8_N(min,15) (`hw+0x20a4`) (`dri:0x193776..0x19383a`);
* otherwise both are `clamp(size, min, max)` (`dri:0x19378e..0x1937e8`).

Image offsets `+0x2164/+0x2168` are A2A/A2B (A23 is `+0x2148`, dword stride).
Point size therefore **can** come from the VS: native destination `0x27`
(unified 39, D3D `oPts`, full MOV `0008027f`, shader-upload-oracles.md:184)
plus A25 bit 19.  The record position of the size lane inside the primitive
vertex is hardware-internal in TCL mode; for post-TNL cmd22 it would sit after
W in manual bit order (inference, untested).

Exact words (corrected 2026-09-25): "Floating s[8].15" is a 24-bit float
(sign, 8-bit exponent, 15-bit mantissa), and `INV_FloatToP_E8_N(f, 15)`
(`dri:0x1a7130`) is `(bits & 0x7fffffff) >> 8`.  So `1.0 → 2a3f8000`,
`2.0 → 2a400000`, `4.0 → 2a408000`, `64.0 → 2a428000`, `2048.0 → 2a450000`
(static).  The earlier fixed-point list (`2a008000` for 1.0) was an
inference and is wrong; `0x400000` is 2.0, not 2048.0.

### 3.6 Point sprites

Manual A01 bit 21 (M:4983..4997): with point or triangle-point PMType, the
four generated corners get texture coordinates UL `(0,0,0,1)`, UR `(1,0,0,1)`,
LL `(0,1,0,1)`, LR `(1,1,0,1)`.  A2F bits 9:0 (M:10121..10150) are
per-coordinate "point sprite enable for texture A..J".

Vendor use (static):

* `STM_PointSprite_INV` (`dri:0x194330`) sets only A01 bit 21 (image
  `+0x6` bit 5) from `hw+0x1cf4`.
* `stmUpdateTCI_invi` (`dri:0x17dad0`), when `hw+0x1cf4 != 0` and the
  primitive class `+0x3b18 == 1` (points), forces every coordinate to 4D
  (`dri:0x17dcb4..0x17dd08`, `0x17def5..0x17df26`) so the (s,t,0,1) corner
  values reach the pixel shader.
* No writer of A2F[9:0] was found; `stmInitChipImage_inv` sets only
  A2F bytes 1/2 (`0x2f5820xx`: Z, W, colour and alpha defaults, Q default 1)
  at `dri:0x1789d8..0x178a0d`.  Whether A2F[9:0] = 0 disables replacement or
  is ignored when A01[21] is set is **open** (ladder L-S2).

GL `GL_POINT_SPRITE_COORD_ORIGIN` defaults to `GL_UPPER_LEFT`, which matches
the manual corner table for the default window orientation (inference); for
flipped FBO orientation Gallium's `sprite_coord_mode` must select either the
native table or a `t' = 1 - t` fixup in the fragment program.

### 3.7 Line width

No line-width field exists in the VX900 manual (no match in type-01/04
register text).  The vendor GL driver emulates width: `__glDpSTMLineStates`
(`dri:0x1ecc20`) stores `glLineWidth` in `hw+0x1dd8` and sets `hw+0x1ce4=1`
for width > 1 (`dri:0x1eccc0..0x1eccdf`); `__glS3InvDrawPrimitiveLineList_H6`
(`dri:0x2169c0`) truncates the width to an integer (`fldcw` RC=11,
`dri:0x216a4d..0x216a66`) and, for width > 1, emits `line_count·width`
one-pixel lines centred on `width/2` (`dri:0x216b1b..0x216b4e`).  The per-copy
offset axis was not decoded.  Chrome9 should keep Gallium draw's wide-line
stage (two triangles per segment), which is GL-conformant and needs no new
hardware state.

### 3.8 Line stipple

Already exact in [stipple-polygon-mode-contract.md](stipple-polygon-mode-contract.md):
A01 bit 0, A84[15:0] pattern LSB-first, A85[15:0] repeat, A85 bit 23 counter
reset that the vendor sets on every line primitive
(`dri:0x1bbdd1`, `0x1bc0b1`; `VTGUModeDX32A:0x101f50aa`, `0x101f52a0`).

### 3.9 Polygon mode

`__glDpSTMUpdatePolygonMode` (`dri:0x1ec910`) maps GL
POINT/LINE/FILL through the table at `dri:0x3f76c0` = `1,2,3` into
`hw+0x1dfc` (front) and `hw+0x1e00` (back).
`stmTriangleListPrimitiveSetting_invi` (`dri:0x1bc7f0`) then writes A23
(static):

* equal front/back mode: bit 4 clear; A23[3:0] = 7 point, 6 wireframe,
  2 fill (`dri:0x1bc836..0x1bc860`, `0x1bcb80`, `0x1bcbc8`);
* different modes (internal draw-path ids 5, 7, 8 and 14): bit 4 set; A23[3:0] takes the
  mode of the CCW-physical face and A23[6:5] (00 fill, 10 line, 11 point)
  the CW face, with front = CCW when `glFrontFace(GL_CCW)`
  (`dri:0x1bca18..0x1bcaa9`, `0x1bcb0c..0x1bcbb3`);
* A01 bit 5 (edge anti-aliasing) is copied from line-smooth state
  `+0x3c28` for wireframe and polygon-smooth `+0x3c30` for fill
  (`dri:0x1bcb1e`, `0x1bcb3a..0x1bcb53`).

Wireframe uses A23[11:9] edge flags per packet, so native wireframe cannot
honour per-vertex GL edge flags; those must stay on draw's unfilled stage.
Native polygon mode also interacts with polygon offset (point/line/fill offset
enables), line stipple and triangle-point clipping (A23 bit 23), so it is an
optimisation after native line/point coverage, not a correctness path.

## 4. Two-sided color

### 4.1 Hardware route

* VS destinations 36 and 37 (`0x24`/`0x25`, D3D ATTROUT 2/3, vendor
  `oD2`/`oD3`) are the back-face colors oBFD0/oBFD1.  `mov oD2, c0` compiles to
  `0040014c 00000000 00000000 0008024f` without the saturate bit, whereas
  oD0/oD1 carry bit 14 (static/cross,
  [chrome9-face-bfc-oracle-corpus.tsv](chrome9-face-bfc-oracle-corpus.tsv)).
* Writing oD2/oD3 sets VS object `+0x0c` bit 6/7, and
  `A61[19] = (object+0x0c != 0)` (`dri:0x17c610..0x17c620`,
  `VTGUModeDX32A:0x1015acfb..0x1015ad06`; static/cross).  The bit is
  undocumented; it is the only recovered oBFD enable.
* A23 bit 7 chooses the physical winding that receives oBFD (manual).  The
  vendor sets it from `glFrontFace` only: `__glDpSTMUpdateCull`
  (`dri:0x1ee290`) maps `GL_CW → hw+0x1db8 = 1`, `GL_CCW → 2`, and
  `STM_UpdateCull_INV` writes `A23[7] = (value == 1)` in both the culled and
  unculled branches (`dri:0x196ade..0x196af8`, `0x196b40..0x196b5b`).  So
  oBFD goes to CCW triangles when the front face is CW, and vice versa.
* The same routine derives A01 bit 7 / A23 bit 8 from GL cull face and front
  face (`dri:0x196aaf..0x196bdb`): cull front + front CW → cull CW (bit 8 = 0).
  The vendor thus names hardware windings exactly as GL does; the live Mesa
  mapping `tcl_cull()` (`chrome9_tcl.c:1298..1308`) and silicon rung L7
  (A23 `23004e02` culls the screen-clockwise triangle) agree.  Therefore
  **A23[7] = front_ccw ? 0 : 1** in Gallium terms (inference from the two
  consistent mappings; ladder T3 proves it).
* No VX900 FVF or output-mask bit for back colors exists in A25/A26/A28
  (manual pp.161..166); the HCM FVF attributes 31h/32h are not the VX900
  mechanism (ioquake3-blocker-roadmap-re.md §2.3).

### 4.2 How the vendor GL driver implements `GL_LIGHT_MODEL_TWO_SIDE`

1. `__glDpSTMUpdateLightStates` (`dri:0x1ed120`) copies lighting enable to
   `hw+0x1d04` and the two-side flag (`gc+0x4f0c`) to `hw+0x33e4`.
2. `__glFFVSUpdateLightState` (`dri:0x2458d0`) builds a two-bit side mask in
   the fixed-function VS key byte `+7` (`dri:0x245918..0x245978`):
   two-side off → 1 (front); two-side on and culling off → 3 (both);
   cull `GL_FRONT` → 2 (back only); cull `GL_BACK` → 1; cull
   `FRONT_AND_BACK` → 0.  `__glFFVSUpdateCullState` (`dri:0x245b60`)
   re-evaluates it on cull changes.
3. The GL fixed-function VS generator emits D3D-style tokens compiled by the
   ordinary PVS compiler (`__glFFVSGenerate` → `stmCreateVertexShaderFunc`,
   `dri:0x24939e`).  The back-side lighting block writes
   `mov_sat oD3, r9` and `mov_sat oD2, r8` (ATTROUT type byte `0xd0`,
   index 3/2, destination-modifier saturate in token bit 20)
   (`__glFFVSLightCompute_Looped` `dri:0x24c5fe..0x24c6ab`; the `_Expanded`
   variant has the same pattern at `dri:0x24ce54`, `0x24efa3`).
4. For point and line primitives (GL mode ≤ 3) the back bit is cleared before
   drawing (`__glS3InvUpdateDrawPrimitivePath` `dri:0x230a94..0x230ab4`),
   matching GL, where two-sided lighting affects polygons only.
5. The ARB vertex-program result table maps `result.color.back.primary/
   secondary` to ATTROUT 2/3 in both GL drivers (ioquake3-blocker-roadmap-re.md
   §2.2, static/cross).

Open: the D3D compile path drops the saturate bit on oD2/oD3 although the GL
generator requests it.  Whether the hardware clamps back colors anyway is
unknown; Chrome9 must clamp explicitly (MIN/MAX against literal 0/1, or test
native bit 14 on dest 36, ladder T5).  Also open: what an unwritten oBFD
delivers when A61[19] = 0 (the live path never writes it and renders both
windings correctly, which suggests A61[19] gates the selection; inference).

### 4.3 `gl_FrontFacing`

The vendor pixel frontend has no working face register (vPos and vFace
compile to the same MISC source, see
[chrome9-vface-normal-sign-boundary.md](chrome9-vface-normal-sign-boundary.md)).
The only exact producer is the BFC selection itself: write PRIMARY = 1 into
oD0 and 0 into oD2, then `face = primary.r` in the pixel program
([chrome9-draw-twoside-face-sign.md](chrome9-draw-twoside-face-sign.md) for
the draw path, [chrome9-face-sign-bfc-producer.md](chrome9-face-sign-bfc-producer.md)
for the native path).  This consumes the single PRIMARY interface, so it is
exclusive with application use of COLOR0.

## 5. User clip planes

### 5.1 Hardware (manual M:7927..7985, type-02/FE sub 05h)

```text
A05[15]     HTXUCP1Enable: 2nd UCP texture coordinate group
A05[14]     2nd group perspective correction
A05[13:10]  2nd group source coordinate A..J (0..9)
A05[9:8]    2nd group dimension: 0 S, 1 ST, 2 STR, 3 STRQ
A05[7]      HTXUCP0Enable: 1st group
A05[6]      1st group perspective correction
A05[5:2]    1st group source coordinate A..J
A05[1:0]    1st group dimension
```

The same fields exist in the HCM (M-HCM:7976) and HC3 manuals (`HTXUCP1PPmode`,
`HTXUCP1Src`, `HTXUCP1Dim`).  The manual does not state the comparison;
the vendor code below writes signed plane distances, so the test is
"reject the pixel when any enabled component is negative" (inference, the
standard D3D/GL convention; ladder U2 measures the zero boundary).

### 5.2 Vendor implementation (static)

1. `__glDpSTMUpdateUserClipPlanes` (`dri:0x1ee600`) copies the enabled plane
   mask (`gc+0x5fe0`) to `hw+0x1d34` and each enabled eye-space plane
   (`gc+0x5f5c`, 16 bytes) to `hw+0x1ffc + 0x18·i`; with any plane enabled it
   calls `__glDpSTMUpdateProjInv` (`dri:0x1ee3f0`), which stores the
   inverse-transposed projection (`__glInvertTransposeMatrix`) at
   `hw+0xc40..0xc7c`.  The planes are thus converted to clip space before
   upload (the multiply site was not located; inference from the stored
   matrix).
2. `stmPVSUpdateDirtyConst_H5` uploads all six planes as internal rows
   259..264: `fe03043c 00000018 3203040c cc000000` + 24 dwords
   (`dri:0x17be12..0x17be2e`, `0x17c3d5..0x17c3ec`).  Unified source ids are
   227..232 (chrome9-hw-tcl-re.md).
3. Programmable VS: `stmPVSReBundleUCP_h5i` (`dri:0x160770`, called from
   `stmPVSCompile_H5` `dri:0x160d8c`) takes `mask & 0x3f` (six planes), and
   for the k-th enabled plane appends one DP4:
   * plane k → coordinate `N + k/3`, component `k % 3` (x,y,z; w unused),
     where N is the program's texture-coordinate output count (object
     `+4` bits 11:8) (`dri:0x1607f0..0x160833`, table at `ebp-0x60`);
   * src0 = clip-space position: the redirected oPos temporary
     (`r[object+0x18]`, unified 64+t) or scratch 128 when there is none
     (`dri:0x160937..0x160965`, `0x160c38`);
   * src1 = internal row `0x103+i` (unified 227+i);
   * opcode 3 (DP4), destination `0x28 + N + k/3`, mask 1/2/4.

   Example, plane 0 into oT1.x reading scratch 128:

   ```text
   0020014c 0038c14c 00000000 00060291        DP4 oT1.x, s128, c227
   ```

   The DP4s are placed after the upload tail as a subroutine: a CALL (op 29,
   push bit 14) is written at the old active end, the last DP4 gets the
   return bit 23 (`dri:0x16088a..0x160927`, `0x160b27`), active count +2 and
   uploaded count +planes (`dri:0x160b40..0x160b47`).  The plane count goes
   to `hw+0x378c`.  If N = 7 and more than three planes are enabled the
   groups move to coordinates I/J (`dri:0x160bb8..0x160bd5`,
   `0x160c60..0x160d2b`).
4. Fixed-function VS: `stmFFVS_UCP_H5i` (`dri:0x1a6a30`) emits the same DP4
   records inline, src0 = `r3` (`0010c14c`), constants from the byte table
   `dri:0x3f2560` = `0x103..0x108`.
5. Front end: `stmUpdateTCI_invi` (`dri:0x17dad0`) programs A05 from the
   16-byte-per-count table at `dri:0x3f1d00` `{dim0, dim1, en0, en1}`
   (`dri:0x17e608..0x17e719`), with source0 = N and source1 = N+1
   (`ebp-0x120`); a second table at `dri:0x3f1c80` serves the
   eight-coordinate case.  The coordinate-output count in A08[4:0] grows by
   `ceil(planes/3)` (`dri:0x17ddb4..0x17ddf5`; the PS side
   `stmUpdatePS_inv` `dri:0x180b54..0x180b7d` does the same).  Perspective
   bits A05[6]/[14] are never written (they keep the image value).
6. VS output FVF: `stmUpdateFVFOut_H5i` ORs per-count S/T/R presence
   nibbles `0x80,0xc0,0xe0,0xe8,0xec,0xee` (planes 1..6) at coordinate N into
   the 64-bit texture FVF mask (`dri:0x1ba9c0..0x1baa2d`, `0x1ba714`).

Resulting A05 values for UCP coordinates starting at N:

| planes | group 0 | group 1 | A05 (perspective bits clear) |
|---:|---|---|---|
| 0 | off | off | `05000000` |
| 1 | S | off | `05000080 \| N<<2` |
| 2 | ST | off | `05000081 \| N<<2` |
| 3 | STR | off | `05000082 \| N<<2` |
| 4 | STR | S | `05008082 \| N<<2 \| (N+1)<<10` |
| 5 | STR | ST | `05008182 \| N<<2 \| (N+1)<<10` |
| 6 | STR | STR | `05008282 \| N<<2 \| (N+1)<<10` |

`STM_ClipPlaneEnable_H5`/`STM_SetClipPlane_H5` are empty (0x1bab40/0x1bab50)
because the D3D-derived H5 layer handles planes through this VS/FE route, not
through a fixed-function packet.  The software emulation paths
(`ComputeUCPClipCode` `dri:0x136e60`, `UpdateUCPInfo` `dri:0x13bd30`) serve
the CPU TNL fallback only.  The Windows binaries do not contain the same
contiguous tables; the Windows route was not audited.

### 5.3 Consequences

* Clip planes cost one coordinate per three planes against the eight
  texture-coordinate budget (A..H; I/J exist for UCP only).
* The test is per pixel after interpolation, so a clipped triangle is not
  subdivided; coverage, depth and attributes are those of the unclipped
  triangle.  This is GL-conformant for the visible pixels provided the
  interpolation is exact at the boundary: a clip distance is linear in clip
  space, so screen-space interpolation must be perspective-correct.  The
  vendor never sets A05[6]; whether "disable" still interpolates with 1/w is
  **open** (ladder U4).
* Mesa already produces `gl_ClipDistance` writes for enabled planes, so the
  kernel only needs to route CLIP_DISTANCE lanes to the reserved coordinate
  slots; the vendor CALL/RET subroutine form is unnecessary and avoids VS
  control flow, which is unqualified on silicon (chrome9-hw-tcl-re.md, open
  unknowns).

## 6. Register words, stream placement, UAPI and Mesa integration

### 6.1 Stream placement

* A20/A23/A24 stay at their current positions in both raster builders
  (`chrome9_fragment_raster_state()` indices 8..10 of the no-varying image or
  after A62; `chrome9_tcl_raster_state()` after A62).
* A2A/A2B are inserted after the A25..A28 link words and before A40, keeping
  ascending sub-addresses in the same type-04 group.  In the cmd22 body this
  consumes the two existing alignment pads (line-point-raster-contract.md);
  in the TCL builder `CHROME9_TCL_RASTER_WORDS` (48) must grow by two and the
  self-test `expected[]` image (`chrome9_engine.c:2210..2242`) must be
  regenerated.
* A2F, when used, follows A2B.
* A61 bit 19 is part of `tcl->program.a61` (`chrome9_vertex.c` A61 builder).
* FE A05 and A08 live in `chrome9_fragment_fe_config()`
  (`chrome9_engine.c:2367`, `config[4]` and `config[2]`); UCP S/T/R presence
  bits extend the VS-output FVF words built by `chrome9_fragment_link_words()`.
* Internal rows 259..264 are a separate H3 constant packet
  `fe03043c 00000018 3203040c cc000000` placed next to the existing
  `32030400` internal-row upload (`chrome9_engine.c:2777`).

### 6.2 Kernel UAPI proposal (fail-closed; no numbers reserved by this note)

All hardware words stay kernel-generated.  Proposed additions, each behind
its own capability bit (next free: 60):

1. **Topology** (vertex program ABI 2 and fragment program flag field):
   `topology` enum {TRIANGLE_LIST=0, POINT_LIST, LINE_LIST, LINE_STRIP}; strip
   and fan triangles stay CPU-decomposed.  Validation: counts per §3.3,
   `CULL_*` must be zero for points/lines, `FLAT` selects FlatA for points and
   FlatB for lines (after ladder), strips are never split (reject if above
   the per-request vertex bound), and the viewport is supplied **without**
   snap bias in ABI 2 so the kernel adds 786432 to ox/oy and rows 258.xy only
   for line/triangle topology.
2. **Point size**: `point_size_min_bits`, `point_size_max_bits`,
   `point_size_bits` (IEEE float bits).  Reject non-finite values and values
   outside [1.0, 2048.0]; require min ≤ size ≤ max.  Kernel converts to float s[8].15 (bits >> 8)
   and always emits A2A/A2B for point topology (neutral `2a3f8000 2b3f8000`
   otherwise).  New vertex destination `POINT_SIZE` (index 0, mask x only,
   written exactly once on every path, SAT forbidden) sets A25 bit 19 and
   switches A2A/A2B to max/min.
3. **Point sprite**: flag `POINT_SPRITE` plus `sprite_coord_mask` (bit per
   fragment varying).  Accepted only with POINT_LIST; each masked varying must
   be a declared width-4 coordinate.  Kernel sets A01 bit 21 and, after
   ladder L-S2 decides it, A2F[9:0].
4. **Two-sided color**: vertex destination `BACK_PRIMARY` (index 0, full
   mask).  Accepted only with TRIANGLE_LIST, only if PRIMARY is also written
   and the fragment program consumes PRIMARY; both must be defined on every
   path.  Kernel emits dest `0x24` with an explicit clamp, sets A61[19], and
   takes A23 bit 7 from a new `FRONT_CCW` flag (`bit7 = !front_ccw`).
   `BACK_SECONDARY` (dest `0x25`) stays rejected until specular is consumed.
5. **User clip distances**: vertex destination `CLIP_DISTANCE` (index 0..5,
   scalar lanes), accepted only when `varying_count + ceil(n/3) ≤ 8`.  The
   kernel maps distance k to `oT[N + k/3]` lane `k % 3`, programs A05, A08,
   and the output FVF presence bits.  Every enabled lane must be written once;
   distances beyond six, I/J placement and Q lanes are rejected until
   measured.

Stale-state closure: every draw re-emits A01, A05, A20, A23, A24, A2A/A2B and
A61 so that no topology, size, sprite, UCP or BFC state leaks into the next
triangle draw.

### 6.3 Mesa integration points

* `chrome9_context.c:6764` (after `draw_create_no_llvm`): call
  `draw_wide_point_threshold(ctx->draw, 0.0f)` and
  `draw_enable_point_sprites(ctx->draw, true)` so draw expands points and
  sprites to triangles immediately; this is the correctness baseline before
  any native point.  One-pixel lines need a Chrome9 stage or the native
  line topology (line-point-raster-contract.md).
* `chrome9_vbuf_draw_arrays/elements()` (`chrome9_draw.c:3214..3250`,
  `3272..3317`): add POINTS/LINES/LINE_STRIP cases once the kernel topology
  capability exists; keep `CHROME9_REJECT` otherwise.
* `chrome9_screen.c:428..430`: set `two_sided_color = true` only with the BFC
  capability (then `light_twoside` reaches the rasterizer and draw's twoside
  stage or TCL handles it); `clip_planes` may stay 0 because the state
  tracker already lowers to clip distances.  Advertise `max_point_size` /
  `point_sprite` only after the point ladder.
* `chrome9_tcl.c:744..750`: map `VARYING_SLOT_CLIP_DIST0/1` to
  `CLIP_DISTANCE`, `BFC0` to `BACK_PRIMARY`, `PSIZ` to `POINT_SIZE`;
  `chrome9_tcl.c:1513..1516`: drop the `light_twoside`/`clip_plane_enable`
  fallbacks and accept point/line topology as the capabilities appear.
  Polygon modes other than FILL remain on draw's unfilled stage.

## 7. Silicon test ladder (one variable per step)

Common harness: guarded 32x32 or 64x32 BGRX target, GPU lock, FE33 on,
every step checks guards, exact unchanged pixels, retirement cookie,
work-mask clear and `040c == 0`, then a following reference triangle.
Timeout or guard mutation → quarantine and stop.

Points / lines / strips:

* **L-P1** one point, A23 `23001f00`, A2A/A2B `2a3f8000/2b3f8000`, bias kept
  as today.  Expect exactly one pixel.
* **L-P2** same, rows 257/258 bias zero (vendor form).  Compare phase.
* **L-P3** 2x2 then 1x1 again (A2A/A2B closure).
* **L-P4** point with VS dest 0x27 + A25 bit 19, size 4, clamp min 1 max 2
  → expect 2x2 (clamp proven).
* **L-L1** one horizontal line list `20000000 23004f01 24000002`.
* **L-L2** same with A23 bit 15 set (last pixel).
* **L-L3** two-segment strip `20206000`, alpha 0x80 blend (shared-pixel
  ownership).
* **L-L4** line with FlatB vs Gouraud (provoking vertex).
* **L-T1** three-triangle strip `201c7800 23004f02 24000005`, then fan
  `201c5c00`, then a plain list triangle (reuse-state leak).

Point sprites:

* **L-S1** A01 bit 21 with A2F[9:0]=0, one 8x8 point, PS outputs
  `t0.xy` → does the gradient appear?
* **L-S2** same with A2F bit 0 set.  Decides the A2F contract.
* **L-S3** sprite off after sprite on (A01 bit 21 closure).

Two-sided color:

* **T1** oD0 = white, oD2 = black, A61[19]=1, culling off, adjacent opposite
  windings, PS `MOV oC0, v0` → one white, one black.
* **T2** same with A61[19]=0 → learn the default (both white expected).
* **T3** toggle only A23 bit 7 → colors exchange.
* **T4** enable cull (A01 bit 7) with A23 bit 8 both ways → bit 7 and bit 8
  independent.
* **T5** oD2 = 2.0 with and without bit 14 → back-color clamping.
* **T6** clipped triangle crossing the near plane → selection follows the
  final physical winding.

User clip planes:

* **U1** VS writes `oT1.x = 1.0` constant, A05 `05000084` (group 0, S, source
  B), A08 count +1 → nothing clipped.
* **U2** `oT1.x = x_clip` (DP4 with row 259 = (1,0,0,0)) → exactly the left
  half rejected; locate the zero boundary pixel.
* **U3** two and three planes in x/y/z of one coordinate.
* **U4** perspective-projected quad, boundary position with A05[6] = 0 vs 1.
* **U5** four planes (second group), then a following draw with A05 =
  `05000000` (closure).
* **U6** UCP with an application texture on coordinate A (linkage offset).

Hang risks, in order of concern:

1. VS CALL/RET (vendor UCP subroutine form): control flow is unqualified on
   silicon; use inline DP4 records only.
2. Incorrect A08 coordinate count or FVF presence bits while A05 references a
   non-existent coordinate: the FE may wait for data that never arrives
   (inference; the documented `A61` bit 18 timeout shows FE/VS misconfiguration
   can need a cold boot).  Always change A05, A08 and the FVF mask together.
3. Strip/fan reuse bits left in A20 when the next draw is a list: wrong
   geometry at best; re-emit A20 on every draw.
4. Point sprite with A2F or coordinate dimensions inconsistent with the PS
   declaration.
5. Smooth-line special cycles (`202c6c00`, `23004506`) and polygon stipple
   (known `f0f0f0f0` refusal in old Mesa) remain excluded.
6. A61 bit 18: known engine timeout; never set.

## 8. Open questions

* A2F[9:0] versus A01[21] for sprite coordinate replacement.
* Point centre phase with and without the 786432 bias.
* Default oBFD contents with A61[19] clear; hardware clamping of dest 36/37.
* UCP comparison sign, perspective mode, the I/J path and whether a fourth
  plane per coordinate (Q) is usable, which would allow eight planes.
* Whether the pixel-side linkage (A28/A29 consumer words, A90 resource
  counts) must include UCP coordinates; the vendor adds them to its PS
  coordinate count (`dri:0x180b54`).
* Windows D3D/GL UCP and point-sprite routes (not audited).
