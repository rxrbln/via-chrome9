<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Hardware TCL two-sided color: complete vendor enable set

Status: 2026-09-24.  Host-only reverse engineering.  No word in this note
was executed on VX900 silicon by this work.  The silicon behaviour quoted as
the starting point came from the separate rung run (`chrome9_tcl_oracle
twoside t1/t3/t4/t4b/t6`): with the kernel drafts from
[chrome9-tcl-twoside-clip-plan.md](chrome9-tcl-twoside-clip-plan.md)
(VS writes oD0 = white and oD2 = c0 `ff4080bf` with saturate, A61[19] set,
A23[7] set per winding, PS `MOV oC0, PRIMARY`), both windings show the front
color.  Culling behaves as expected and nothing hangs.

This note answers one question: which register or state words does the
vendor write differently when back colors are active?  It then lists
one-variable kernel experiments.

Evidence labels:

* **manual**: VX900 `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, text
  extraction `~/tmp/ogpm3d.txt`, line `M:<n>`.  **manual/HCM** is the
  VX855/VX875 HCM Part 2 text.
* **static**: disassembly of `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`
  (SHA-256 `fddb4dea85319c76…`, `dri:0x…`), of the Windows GL ICD
  `VTGOGL32A.dll` (`a54e38ca7c038d7c…`, `ogl:0x…`) and of the D3D UMD
  `VTGUModeDX32A.dll` (`8ac6959fc3ab21b7…`, `win:0x…`).
* **dynamic**: vendor code executed on the host (32-bit `dlopen`, no DRM or
  GPU) by the tools in section 8.
* **inference**: derived from the above, not observed.

## 1. Result

The Linux vendor GL two-sided path changes exactly four things.  The
Windows GL ICD agrees on items 1 and 4, and the D3D UMD agrees on item 2
(static/cross):

1. The VS writes the back colors to oD2/oD3 (native destinations `0x24` and
   `0x25`).
2. **A61[19]** = 1.
3. **A23[7]** = 1 when the GL front face is `GL_CW`, otherwise 0.
4. Triangle strips and fans are decomposed into independent triangle lists
   when two-sided lighting is on and culling is off.

No other hardware word depends on two-sided state.  This covers the other
type-04 words A00..A62 (including A12, A25, A26, A28, A29, A60 and A62),
type-01 A01, the FE words,
the PS control words A90..A9B, the PS input mapping, the VS output descriptor
`+0x04` and the vertex cache enable.  The fragment side reads PRIMARY
(`v0`, class 1 id 0) unchanged, so the vendor expects the selection to happen
before the pixel shader, in setup.

The kernel draft already emits items 1 to 3 with the same values the vendor
uses.  The difference must therefore come from state that the vendor does
not tie to two-sided lighting.  Section 5 compares the two emissions and
section 6 ranks the experiments.

One dynamic result shows that the vendor depends on the hardware selection
(section 3.1).  With `GL_LIGHT_MODEL_TWO_SIDE` and `glCullFace(GL_FRONT)`,
the generated VS writes **only oD2** and never writes oD0.  A61[19] and
culling are on, so every surviving triangle gets its color from oBFD0 or
from an undefined oD0.

## 2. Every two-side consumer in the Linux DSO (static)

The GL two-side flag is `gc+0x4f0c`, copied to `hw+0x33e4` by
`__glDpSTMUpdateLightStates` (`dri:0x1ed141`).  Every reader in the DSO:

| reader | effect | class |
|---|---|---|
| `__glFFVSUpdateLightState` `dri:0x245918` | FF VS key byte 7 bits 1:0: side mask (1 front, 2 back, 3 both) | VS code only |
| `__glS3InvUpdateDrawPrimitivePath` `dri:0x230ace` | lighting && two-side && mode > GL_LINE_STRIP && culling off: HW primitive type from `glPrimTypeToStmPrimTypeTwoSided_INV` (`dri:0x429860`) instead of `glPrimTypeToStmPrimType_INV` (`0x4297e0`); front polygon mode GL_LINE sets draw flag `0x3ac8 = 3` (fallback) | primitive type |
| `stmBuildLegacyVSD` `dri:0x1d1b60` (reads `hw+0x33e4`) | software VS emulation only: declares back colors as extra inputs 18/19 | not the HW VS path |
| `__glSwp*_SWRasterise_*`, `__glSwpBuildVertex`, `__glSwpConfigRasterInStream`, `__glSwpValidateAttribute` | software rasterizer | not HW |
| `__glim_LightModel*`, `__glDoGet`, `__glEvaluateAttributeChange` | API state | none |

Table contents (dword per `(polygon_mode - GL_POINT) * 10 + GL mode`,
dumped from `.data.rel.ro`; STM types 1 point, 2 line list, 3 line strip,
4 triangle list, 5 triangle strip, 6 triangle fan):

```text
normal   1 1 1 1 4 5 6 4 4 4 | 1 2 3 3 4 5 6 4 4 4 | 1 2 3 3 4 5 6 4 4 4
twosided 1 1 1 1 4 4 4 4 4 4 | 1 2 3 3 4 5 6 4 4 4 | 1 2 3 3 4 4 4 4 4 4
```

In GL_FILL mode, strips (5) and fans (6) become lists (4).  The Windows GL
ICD holds the same two tables at `ogl:0x10389e68`/`0x10389ee0`.  It selects
the two-sided table under the same condition (`ogl:0x100628ec..0x1006291c`),
with a GL_LINE exception (`0x10062916`) (static/cross).

The only reader of the compiler's back-color flags is the dispatcher.  The
parser sets `parsed+0x39ec` and `+0x39f0` for ATTROUT 2 and 3
(`PVSCP_MSVSPARSER_VS20` `dri:0x190dc4`, `0x190d25`) and also ORs the
oD0/oD1 bit into the output mask (`dri:0x190dbd`, `0x190d1b`).
`PVSCP_DISPATCHER_VS20_H5` (`dri:0x18e86d..0x18e890`) turns the flags into
object `+0x0c = 0x40 | 0x80`.  The recovered consumer of `+0x0c` is
`stmUpdateVS_inv` (`dri:0x17c610..0x17c620`), which sets
`A61[19] = (+0x0c != 0)`.  No other reader of the parser flags exists.
`PVSCP_DISPATCHER_VS11_H5` never sets `+0x0c`.  Object `+0x08`, which
feeds A61[22:20], is 0 in every dynamic case below, and no write to it
appears in the VS20 dispatcher's metadata tail (static, partial).

Other readers of the VS object (`STM_PointSize_H5` tests `+0x39e8` bit 5,
oPts; `stmUpdateFVFOut_H5i` reads `+0x04` only) do not look at back colors
(static).

## 3. Dynamic results

### 3.1 Fixed-function VS: outputs and metadata

`tools/re/chrome9_tcl_ffvs_gl_probe.c` runs the real GL state updaters,
`__glFFVSGenerate` and the PVS compiler.  It has new keys `cullfront`
(`GL_FRONT`) and `cullfb`.  The list shows the native color-output records
(destination `0x22 + n` = oDn):

```text
light=1                          +4=044 +c=00  oD0=0008422f
light=1 twoside                  +4=044 +c=40  oD0=0008422f oD2=0008424f
light=1 twoside sepspec          +4=0c4 +c=c0  oD1=0008423f oD0=0008422f oD3=0008025f oD2=0008424f
light=1 twoside cull (GL_BACK)   +4=044 +c=00  oD0=0008422f
light=1 twoside cullfront        +4=044 +c=40  oD2=0008424f
light=1 twoside sepspec cullfront +4=0c4 +c=c0 oD3=0008025f oD2=0008424f
light=1 twoside cullfb           +4=004 +c=00  (no color output)
```

In every case `+0x08 = 0`, so vendor A61[22:20] = 0.

* The GL generator's oD2 carries saturate (bit 14, `0008424f`), and so does
  the kernel's.  The GL secondary back color oD3 does not (`0008025f`).
* **cullfront**: the program writes only oD2 (and oD3), but `+4` still marks
  oD0 and A61[19] = 1.  The vendor relies on the hardware to use oBFD for
  the surviving back faces (dynamic).  If the selection did not work, the
  shipped driver would draw an unwritten oD0 in `GL_FRONT`-culled two-sided
  lighting (inference).
* **cull GL_BACK** drops oD2 entirely (A61[19] = 0).

### 3.2 A01/A23 from `STM_UpdateCull_INV`

`tools/re/chrome9_twoside_state_probe.c` runs the real `STM_UpdateCull_INV`
with the drawable flag `hw+0x4ff4`.  `__glDpSTMUpdateViewport` sets it to 1
for a window (Y-inverted) and 0 for an FBO (`dri:0x1ec440`, `0x1ec2ab`):

| GL cull | front | window: A01 / A23 | FBO: A01 / A23 |
|---|---|---|---|
| off | CW | `01ce1540 / 23000080` | same |
| off | CCW | `01ce1540 / 23000000` | same |
| FRONT | CW | `01ce15c0 / 23000080` | `01ce15c0 / 23000180` |
| FRONT | CCW | `01ce15c0 / 23000100` | `01ce15c0 / 23000000` |
| BACK | CW | `01ce15c0 / 23000180` | `01ce15c0 / 23000080` |
| BACK | CCW | `01ce15c0 / 23000000` | `01ce15c0 / 23000100` |

* A23[7] depends only on the front face.  For FBOs the vendor swaps the
  culled winding through table `dri:0x3f248c` (`{…,1,3,2}`), but it does
  **not** swap A23[7].  In a window, A23[7] and the culled winding name the
  same physical winding.  In an FBO, the vendor's oBFD winding is the front
  winding, which is a vendor defect for FBO two-sided lighting (inference).
  It does not affect the kernel, which derives both bits from the same
  physical winding.
* Without culling, A23[8] keeps its previous value and A01[7] = 0.

### 3.3 FVF masks from `stmUpdateFVFOut_H5i`

The same probe runs `stmUpdateFVFOut_H5i`, which reads only object `+0x04`:

```text
+4=004  A25=25f00000 A26=26000000 A28=28f9ffff A29=2900ffff
+4=044  A25=25f40000 A26=26000000 A28=28fdffff A29=2900ffff   (twoside, single color)
+4=0c4  A25=25f60000 A26=26000000 A28=28ffffff A29=2900ffff   (twoside, separate specular)
+4=2244 A25=25f5cc00 A26=26000000 A28=28fdffff A29=2900ffff
```

No back-color mask or slot exists.  Back colors reach A25/A28 only through
the oD0/oD1 side effect.  `stmUpdatePS_inv` later clears A28/A29[15:0] and
sets them from the PS coordinate count (`dri:0x17f9e7..0x17fb5b`).  A28[19] is manual-reserved and is 1 in every
vendor image (baseline `28f9ffff`) (dynamic; manual M:9852).

### 3.4 Vendor baseline image

`stmInitChipImage_inv` (family 8, device 0x7122) sets only these words away
from `sub << 24`.  Type-01: `A00=0000000f A01=01ce1540 A14=14ffffff
A15=150000ff A19=19800000 A1A=1a800000 A1B=1b803fbf A53/5B/63/6B=xx010c00
A88=88001ed0 A89=89000800 A8A=8a800000`.  Type-04: `A00=00a80000 A28=28f9ffff
A29=2900ffff A2F=2f582000 A52=52147fff A62=620081ff`.  A61 starts at
`61000000` because the configuration word `+0x2e8` is 0, which leaves bits
18/16 clear.  The image init gives no reserved type-04 word (A11, A13..A1F, A21, A22,
A53..A5F) a non-zero payload (dynamic).  `STM_ViewPort_INV` later ORs
type-01 A01 bits 20 and 11 on the HW TCL path (`dri:0x1bb522`, static).

## 4. Items checked and found independent of two-sided state

| item | vendor behaviour with back colors | evidence |
|---|---|---|
| type-04 A12 | input-register mask from the declaration only | static (`stmUpdateVertexInput_H5`), dynamic stream probe (hw-tcl-re) |
| A25/A26/A28/A29 | from `+0x04` only; oD2/oD3 add no bit | dynamic 3.3 |
| A60 | `60800000 \| (active-1)` | static `dri:0x17c586` |
| A61 | only bit 19 changes (`+0x0c != 0`); bits 22:20 = `+0x08` = 0; bits 18/16 are a configuration option (`+0x2e8`, Windows `+0x30c`), not two-side | dynamic 3.1, static 2, `win:0x101551e6` |
| A62 | inputs and `+0x10`; unchanged | static |
| A23 | bit 7 only (section 3.2); per-primitive setters keep bit 7 (`stmTriangleListPrimitiveSetting_invi` masks `0xf0`/`0xef`/`0x9f` on byte 0, `STM_PrimitiveType_INV` likewise) | static |
| type-01 A01 | bit 7 = culling only; bits 20/11 from the viewport TCL path | dynamic 3.2, static |
| FE (type-02 FE A00..A0E) | texture/UCP coordinate state only; manual has no color field | manual; FE builders do not read `+0x0c` |
| PS A90..A9B, PS input map | FF PS generator has no two-side input; `v0` = class 1 id 0 (`00f04005`), `v1` = class 1 id 1 (`00f04105`) | static 2 (no PS reader of the flag); shader-isa.md |
| VS output linkage `+0x04` | oD2 marks oD0 (bit 6), oD3 marks oD1 (bit 7); no extra bit | dynamic 3.1, static `dri:0x190dbd` |
| vertex output size | the parser adds 4 dwords per oD2/oD3 into `parsed+0x39f4`, but nothing reads that field in the Linux DSO | static `dri:0x190dce`, `0x19047f` |
| vertex cache (type-01 A01[12]) | always on (`01ce1540`) | dynamic 3.4 |
| HCM FVF17 BFC masks (A10[23:22] on HCM) | not present on VX900; VX900 A10 is stream declaration 16, A11 reserved | manual M:9343..9364, manual/HCM p.152 |

The HCM manual states the selection rule that VX900 kept only as the A23[7]
field (manual/HCM A23, "Note to Driver"):

```text
If (HenBFCull & triangle meets HBFcae4Cull)          drop
Else if ((FVFMask has BFC0 | FVFMask has BFC1) & triangle meets HBFcae4BFD)
    C0 = BFC0, C1 = BFC1
Else C0 = Color0, C1 = Color1
```

On VX900 the only remaining candidate for "FVFMask has BFC" is A61[19]
(inference, both vendors agree).

## 5. Kernel draft compared with the vendor

The kernel (working tree plus predecessor patches 0001..0005) uses the same
values for all four vendor items: oD2 `0040014c 0 0 0008424f`, A61
`61880000`, A23[7] per BFC flag, and triangle lists.  It differs from the
vendor in state that the vendor does not tie to two-sided lighting:

| # | kernel | vendor (GL two-side) | note |
|---|---|---|---|
| D1 | type-01 A01 `\|= 0x00100800` (bits 20, 11: clip engine on) | same on the HW TCL viewport path | identical to the vendor, which enables the clip engine in every HW TCL draw, two-sided or not.  Before the clip fix, the kernel ran TCL with both bits clear |
| D2 | A61 is written in the raster block, **before** the `31030000` upload; A60 is written **after** the upload | A61, A62, A60, then upload | write order |
| D3 | VS always writes oD1 (literal 0) and oD3 (literal 0); A25 `25f6cc00`, A28 `28ffcc00` (Cs + unused T0/T1) | single color: only oD0/oD2, A25 `25f40000`, A28 `28fd0000` (an untextured PS clears A28/A29[15:0]: `stmUpdatePS_inv` `dri:0x17f9e7..0x17fa22`) | layout |
| D4 | oD0 always written | cull FRONT: oD0 never written | program shape |
| D5 | A12 `1200001f` (superset) | input mask (`12000003` for two inputs) | |
| D6 | A2F never written | `2f582000` | default values only |
| D7 | A23[8] = 1 in the uncull base (`23004f02`) | A23[8] stale; 0 from init, and 0 after any GL_BACK/CCW cull in a window | without culling, t1/t3 cover A23[8:7] = 10/11, but the vendor window state 00 was not run (t4b culls the CW triangle) |

The t1 (A23[8:7] = 10), t3 (11) and t4 (10 + cull) results all show the
same front color.  A23[7] polarity alone therefore cannot explain the
failure (inference from the reported rungs).

## 6. Ranked one-variable experiments

Base: rung t1 geometry (two unclipped triangles, one per winding, one draw)
with `BFC_CW`, unless noted.  "Select" means the CW triangle shows
`ff4080bf`.  Each step changes one kernel word from the current draft.
Where a step needs a new `tcl_re` bit, it must follow the existing knob
pattern: written before the draw, cleared after it.

Ranking: the vendor-identical items (A61[19], A23[7], oD2 encoding, clip
engine) are least likely to explain the difference on their own.  The first
rows therefore target the differences D2 to D4, which are cheap to change,
followed by low-risk diagnostics.

| rank | change (one variable) | rationale | hang risk |
|---:|---|---|---|
| 1 | vendor cull-front replica: t4 flags (`BFC_CW \| CULL_CCW`, A23 `23004f02`, A01[7]=1, A61 `61880000`), VS writes **only oPos and oD2** (no oD0, no oD1/oD3 fills); A25/A28 unchanged | the only vendor program shape that cannot work without selection (3.1, D4) | low (vendor-shipped shape; fewer output writes) |
| 2 | vendor order: emit `A61, A62, A60` directly before the `31030000` upload instead of A61/A62 in the raster block and A60 after the upload (or re-emit `61880000` after A60) | a program upload or A60 write may reset the output configuration latched from A61 (D2) | low (same values) |
| 3 | vendor single-color output set: t1 with the oD1 and oD3 literal fills dropped (VS writes oPos, oD0, oD2 only); A25/A28 unchanged | the vendor never writes oD3 for single-color two-side (3.1, D3) | low (fewer output writes) |
| 4 | type-01 A01: clear **bits 20 and 11** together (clip engine off, the TCL state before the clip fix); t1 A01 (no depth) `01de1d40` becomes `01ce1540` | the clip engine forwards only FVF-described attributes (A25/A26), and those have no back-color field.  If it drops oBFD, setup never sees it.  TCL rungs ran on silicon with both bits clear before the fix ([chrome9-near-plane-clipping-re.md](chrome9-near-plane-clipping-re.md)) | low (previously run TCL configuration; the geometry is unclipped) |
| 5 | if 4 selects: bit 11 only (`01ce1d40`), then bit 20 only (`01de1540`), as in near-plane rungs N3/N4 | finds which bit gates the loss | low-medium (the vendor never writes the bits separately) |
| 6 | secondary-pair observation: PS source `00f04105` (v1, Cs) instead of `00f04005`; VS writes oD1 = white (not 0) and oD3 = c0 | tells a swapped or pair-1-only selection apart from no selection | low-medium (v1 is a valid PS 2.0 input; only the kernel encoder allowlist changes) |
| 7 | A23[4] = 1 (Hen2FRender) with A23[6:5] = 00 and [3:0] = 2 | turns on the documented per-face setup path; both faces still render as triangles | low (documented bits) |
| 8 | vendor no-cull A23: t1 with A23 `23004e02` (bit 8 clear, A01[7] = 0), the exact vendor window word for front CCW (D7) | t3 argues against a bit-8 dependency, but this A23[8:7] = 00 state is the only vendor no-cull state not yet run | low (documented bit; culling stays off) |
| 9 | vendor single-color masks: with step 3, A25 = `25f40000`, A28 = `28fd0000` (no Cs, no T0/T1 bits) | the vendor never uses a layout with Cs plus unused coordinates for two-sided lighting (D3) | medium (changes the setup layout; FE must still match zero varyings) |
| 10 | PS source class 1 **id 2** (`00f04205`, extrapolated from `00f04005`/`00f04105`) | shows whether oD2 reaches the pixel stage as a separately interpolated color | medium (PS source id with no known encoding; result may be undefined) |
| 11 | type-01 A01[12] = 0 (vertex cache off) | per-vertex color latching; see the strip decomposition in section 2 | medium |
| 12 | type-04 A11 = `11c00000` (analogue of HCM FVF17 bits 23:22 at the VX900-reserved slot) and, separately, A2F = `2f582000` (D6) | an HCM-style BFC mask may still exist | low-medium (reserved register write) |
| 13 | A61[22:20] = 1 | object `+0x08` is never non-zero in vendor VS 2.0 | medium-high |
| — | A61[18] / A61[16] | configuration option (`+0x2e8`); bit 18 is the known engine timeout | high; do not run |

Reading the outcomes:

* **1 selects, 3 does not:** the hardware ignores oD2 whenever oD0 is also
  written.  Emit only one of the two per draw, or split the draw by face
  using culling.
* **3 selects:** a written oD3 (or oD1) interferes.  Drop the fills and
  define A61[19] per written back color.
* **2 selects:** the output configuration is latched when A60 or the
  program is written.  Emit in the vendor order.
* **4 selects:** the clip engine discards oBFD.  Step 5 finds the bit.
  Clipped triangles (t7) then need a separate check.
* **6 shows c0 on Cs:** the back pair mapping is swapped.  Route the back
  primary through oD3.
* **None selects, and 10 shows c0:** oD2 reaches the pixel stage.  Look for
  a face bit in A90..A9B/A8A.
* **None selects, and 10 shows no c0:** VX900 silicon either lacks oBFD
  selection or needs state that no vendor path writes (inference).  Keep
  two-sided color on the draw path
  ([chrome9-draw-twoside-face-sign.md](chrome9-draw-twoside-face-sign.md)),
  and keep `CAP_VERTEX_TWO_SIDE` unadvertised.

## 7. Integration notes

* **Primitive topology.**  If selection works, any later strip, fan or
  vertex-reuse topology (A20 fast-cycle or reuse fields) must be decomposed
  into lists while a BFC flag is set and culling is off, as both vendor GL
  drivers do (static).  With culling on, only one winding survives, so reuse
  is harmless.  This is consistent with per-vertex color latching when a
  vertex enters setup (inference).
* **Winding source.**  Derive A23[7] from the same physical winding as
  A23[8], after any Y flip.  Do not copy the vendor's FBO behaviour
  (3.2).
* **Fragment side.**  No PS change is needed for the vendor model.  The PS
  keeps reading PRIMARY (class 1 id 0).

## 8. Reproduction (host only)

```sh
L=~/tmp/re-tcl/lib32            # i386 libexpat.so.1 + libdrm.so.2
gcc -m32 -O0 -g -rdynamic -o ffvs tools/re/chrome9_tcl_ffvs_gl_probe.c -ldl
LD_LIBRARY_PATH=$L ./ffvs light=1 twoside cullfront     # oD2 only, +c=40
gcc -m32 -O0 -g -o state tools/re/chrome9_twoside_state_probe.c -ldl
LD_LIBRARY_PATH=$L ./state 0x7122     # baseline image, A01/A23 table, FVF masks
```

Both probes run from the repository root and dlopen the vendor DSO.  They
never open a DRM device.  The primitive tables were read from
`.data.rel.ro` at the addresses in section 2 (GOT entries `dri:0x40d428` and
`0x40e880`).

## Silicon results (2026-09-24, kernel k17/k18)

`tools/re/chrome9_tcl_oracle twoside t1` with `TCL_RE=<bits>` ORed into the
`tcl_re` knob of every back-color draw (kernel `eff46d0` plus the knob bits
16/32/64/128).  PS `MOV oC0, PRIMARY`, oD0 white, oD2 = c0 `ff4080bf`,
A61[19] set, BFC_CW (A23[7] = 0), triangle A is screen-clockwise.

| knob | variable | CW pixel | CCW pixel |
|---|---|---|---|
| 0 | vendor words as drafted | `ffffffff` | `ffffffff` |
| 16 | clip engine off (A01 bits 20/11 clear) | `ffffffff` | `ffffffff` |
| 32 | A23[4] (separate front/back render mode) | `ffffffff` | `ffffffff` |
| 64 | A23[8] = 0 without culling (vendor `23004e02` form) | `ffffffff` | `ffffffff` |
| 48, 80, 112 | combinations of the above | `ffffffff` | `ffffffff` |
| 128 | vendor single-color layout: oD1/oD3 records removed, Cs cleared in A25/A28 | `ffffffff` | `ffffffff` |
| 144, 192, 208 | 128 combined with 16 and/or 64 | `ffffffff` | `ffffffff` |

Earlier runs of t3 (BFC_CCW), t4/t4b (with culling) and t6 (flat) also show
only front colors.  No variant timed out.  The hardware back-color
selection therefore stays unproven; VERTEX_TWO_SIDE is not advertised and
two-sided lighting keeps the state tracker's gl_FrontFacing lowering on the
draw-module/llvmpipe path.  Untested from the ranked list: the vendor A61,
A62, A60 emission order, a shader writing only oPos and oD2 with front-face
culling, PS color id 2, A11/A2F, vertex cache off and A61[22:20].

## Follow-up (2026-09-27): vendor binaries and rungs T9..T15

Host-only static analysis and kernel/oracle drafts.  Nothing here ran on
silicon.

### New evidence

* **Emission order is not vendor-specific (weakens D2).**  `stmUpdateVS_inv`
  emits A61, A62, A60 and then the `31030000` upload (`dri:0x17c7e4..0x17c890`).
  `via3DBlit` emits the kernel's order: A61 `61800000` and A62 `623041ff` in
  the type-04 block (`dri:0x1f2ec5`, `0x1f2ecc`), the upload
  (`dri:0x1f2efe`), and A60 `60800004` last (`dri:0x1f304c`).
  `__glDpSyncFrontBufferToFakeFront` does the same (`dri:0x23790d`,
  `0x237939`).  Both orders ship (static), so the order is an unlikely
  cause (inference).  A61 alone is also re-emitted without an upload at
  `dri:0x17cb1c..0x17cb40` (static).
* **The Windows GL ICD also emits items 2 and 3 of section 1.**  It sets
  `A61[19] = (VS +0x0c != 0)` at `ogl:0x1020538d..0x102053a3` (`shl $0x13`,
  mask `0x80000`), and A23[7] from the front face at
  `ogl:0x1024e912..0x1024e91e`.  Its order is A61/A62/A60
  (`ogl:0x1020561f..0x10205641`), then the upload (`ogl:0x10205694`)
  (static).  Both vendor GL drivers therefore agree on all four items.
* **The D3D UMD never writes A23[7] in D3D mode.**  The type-04 image is at
  `+0x20e0` (`win:0x101fc5cf`), so A23 is at `+0x216c` and A60/A61/A62 are at
  `+0x2260/+0x2264/+0x2268`.
  * The A23 builder `win:0x101fabd0` sets A23[7] from the front face
    (`0x101fac44..0x101fac5e`) only when `[+0x9c3c]+0x1c == 8`, a mode that
    uses GL-style cull and front-face fields (static; that it is a GL
    emulation mode is inference).
  * The other A23 writers (`0x101f4f50`, `0x101f5140`, `0x101f5350`,
    `0x101f56d0`, `0x101f5aa0`) touch bits 15, 14:12, 11:9, 6:4 and 3:0
    only (static).
  * `D3DRS_CULLMODE` (22, `win:0x100ca13e`) and `D3DRS_TWOSIDEDSTENCILMODE`
    (185, `win:0x100cacc3`, field `+0x1d4c`) feed only the cull state and the
    type-01 CW/CCW stencil words A23..A26 (readers `0x101fc1ec`,
    `0x101fceb3`, `0x101fd06f`) (static).
  * The only readers of VS object `+0x0c` are the getters `0x1020a410` and
    `0x102093a0`, both called from the A61 builder `0x1015abb0`.  No PS,
    setup or FE word depends on the back-color flags (static; the
    table-management code `0x102096a0..0x1020a0a0` was not fully read).
* **PS color inputs.**  `stmPPS_GetSrcSetting_invi` (`dri:0x162527`) takes
  the input id as `token & 0x3f` without a range check, so `v2`/`v3` would
  encode as `00f04205`/`00f04305`.  No vendor path that emits them was found.
  MISC sources are forced to id 15 (`dri:0x1625d0`), and
  `stmPPS_DCL_PS20_invi` (`dri:0x162c60`) is empty, so declarations never
  remap inputs.  No ps_3_0 path exists (static).  A88..A9B/A8A contain no
  color-count, color-class or face field beyond the known MISC/location pair
  (static).
* **Manual.**
  * M:9547..9549 confirm the A23[7] polarity: 0 selects oBFD# for clockwise
    input order, 1 for counterclockwise.
  * A23[23:19] are clip ECO bits (M:9508..9527) and contain no color select.
  * A2F[20:17] are the default Color0/Alpha0/Color1/Alpha1 values for
    missing parameters (M:10088..10103).
  * Type-04 A53..A62 are "Reserved" (M:10301).  No other back-color field
    exists in the VX900 manual (manual).
* **A61 bit map (static).**  Bit 23 = (VS object `+0x00` ≤ 8)
  (`dri:0x17c5db`, `win:0x1015acbd`).  Bits 15:0 are the boolean constants
  (`stmUpdateIntBoolConst_inv`, `dri:0x17bc49`).  The kernel's `61880000`
  matches both.
* **Kernel state since the 2026-09-24 runs.**  The kernel now writes A2F
  `2f582000` in every TCL raster block; `tcl_re` 256 suppresses it
  (kernel source).  The earlier runs predate the points work that added the
  write, so D6 was probably still open then (inference).

### Rungs T9..T15

Draft patches (not applied):

* `~/tmp/twoside-rungs/0001-drm-chrome9-tcl_re-two-side-rungs-T9-T15.patch`:
  kernel, linux-7.2 root, `-p1`.
* `~/tmp/twoside-rungs/0002-re-tcl-oracle-twoside-t9.patch`: oracle,
  repository root.

The runner is `~/tmp/twoside-rungs/rungs.sh`.  Each knob is ORed into the
back-color draw only, like the existing bits.  Base: `twoside t1` (BFC_CW),
where success is CW `ff4080bf` and CCW `ffffffff`.

| knob | rung | variable | hang risk |
|---|---|---|---|
| 0 / 256 | t1 | A2F `2f582000` written / omitted (control) | low |
| t9c, then t9 | t4 geometry | PRIMARY from c1, no oD1/oD3 (128); t9 also drops oD0 (32768), the vendor cullfront shape.  A green plain draw first makes a stale oD0 visible (`ff00ff00`).  Success: CW `ff4080bf`, CCW `deadbeef` | low-medium |
| 8192 | t1 | A61, A62, A60 as one block before the upload | low (both orders ship) |
| 16384 | t1 | A61 again after A60 | low |
| 131072 | t1 | type-04 A11 `11c00000` (reserved) | low-medium |
| 65536 | t1 | PS slot A PRIMARY read as class 1 id 2 (`00f04205`); *observe*: c0 on both windings means oD2 reaches the PS unselected | medium |
| 262144 | t1 | type-01 A01[12] = 0, vertex cache off (manual M:5013) | medium |
| 524288 | t1 | A61[22:20] = 1 (the kernel's own value for VS point size) | medium |

Host checks: the patched `chrome9_engine.c` builds out of tree with `W=1`
and no warnings.  The extracted raster self-test passes, including new
A11/order and A61 cases, and fails against a wrong expected A61, so it is
not stale.  Both oracle variants build.

### Silicon results T9..T15 (2026-09-27, kernel 745d209 + the rung patch)

`rungs.sh` on the VX900 (`chrome9_tcl_oracle`, oracle patch 0002; kernel
745d209 with patch 0001, the one conflicting hunk ported by hand; the
applied diff is `~/tmp/twoside-rungs/applied-x161.diff`):

| knob | rung | CW pixel | CCW pixel |
|---|---|---|---|
| 0 | t1 (A2F `2f582000` written) | `ffffffff` | `ffffffff` |
| 256 | t1 (A2F omitted) | `ffffffff` | `ffffffff` |
| 0 | t9c | `ffffffff` | `deadbeef` (culled) |
| 0 | t9 (vendor cull-front shape, oD0 not written) | `ffffffff` | `deadbeef` |
| 8192 | t1, vendor A61/A62/A60 order | `ffffffff` | `ffffffff` |
| 16384 | t1, A61 after A60 | `ffffffff` | `ffffffff` |
| 131072 | t1, A11 `11c00000` | `ffffffff` | `ffffffff` |
| 65536 | t1, PS reads colour id 2 | `ffffffff` | `ffffffff` |
| 262144 | t1, vertex cache off | `ffffffff` | `ffffffff` |
| 524288 | t1, A61[22:20] = 1 | `ffffffff` | `ffffffff` |

No rung timed out; guards intact.  t9 shows white although its program
writes no oD0 and a green draw preceded it: an unwritten PRIMARY reads
(1, 1, 1, 1), not a stale value, and oD2 does not replace it.  PS colour
id 2 also reads white, so oD2 reaches neither the setup selection nor the
pixel stage as a separate colour.  Every state the vendor writes for
two-sided lighting, and every remaining candidate from section 6, is now
covered.  Conclusion: VX900 silicon has no working back-colour selection
(the HCM FVF BFC masks are absent on VX900, section 4); two-sided colour
stays the two culled passes of Mesa `fb6780a`, which draw each winding
once on the hardware.  The rung knobs were not merged.
