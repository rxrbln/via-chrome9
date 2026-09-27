<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Hardware TCL: two-sided color and user clip planes

Status: 2026-09-24.  This note is based on host-side work only: static
analysis, runs of the vendor compiler on the host, and host self-tests of
the drafted code.  Nothing in it has run on VX900 silicon.  It turns the
two-sided color and user clip plane findings in
[chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md)
(sections 4 and 5) into an implementation plan for `DRAW_VERTEX_PROGRAM`.
The plan consists of UAPI, kernel compiler and raster changes, Mesa
lowering in `chrome9_tcl.c`, and a one-variable silicon ladder.  The code
drafts are patch files under `~/tmp/agent-twoside/` (see section 7).  They
are not applied to either tree, because stage B vertex buffers are being
developed in the same files at the same time.

Evidence labels:

* **manual**: VX900 `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, text
  extraction line `M:<n>` and printed page;
* **static**: disassembly of `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`
  (SHA-256 `fddb4dea…a69af4`, `dri:0x…`) or
  `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll`
  (`win:0x…`); **static/cross** when both show the same thing;
* **vendor-compiler**: the Linux vendor VS compiler run on the host by
  `tools/re/chrome9_face_bfc_oracle_check.py`, using the corpus
  [chrome9-face-bfc-oracle-corpus.tsv](chrome9-face-bfc-oracle-corpus.tsv)
  (no GPU is involved);
* **inference**: derived from the above but not observed directly.

## 1. What the TCL path does today

Mesa snapshot `chrome9_tcl.c` SHA-256 `76c0ed84…`.  Kernel snapshot
`chrome9_engine.c` `8dc4fc36…`, `chrome9_vertex.c` `a141db11…`.

### 1.1 Two-sided color, `gl_BackColor`, TGSI `BCOLOR`

* `chrome9_screen.c` sets `caps->two_sided_color = false`.  Because of that,
  `st_atom_rasterizer.c` never sets `pipe_rasterizer_state::light_twoside`
  (it is written only when the cap is set).  The
  `rs->light_twoside` test in the `chrome9_tcl_draw()` "rasterizer state"
  gate therefore never fires.
* The state tracker instead sets `key.lower_two_sided_color`
  (`st_atom_shader.c`), and `st_program.c` runs `nir_lower_two_sided_color`.
  That pass moves the selection into the fragment shader, which then reads
  `gl_FrontFacing` plus the `BFC0` input.  The vertex shader still writes
  `VARYING_SLOT_BFC0/1`: the fixed-function generator does this when
  `GL_LIGHT_MODEL_TWO_SIDE` is set, and GLSL `gl_BackColor` does it always.
  In TGSI terms these are `TGSI_SEMANTIC_BCOLOR[0/1]`
  (`tgsi_get_gl_varying_semantic`).
* `tcl_store()` sets `b->failed` for `BFC0`/`BFC1`, so the draw falls back
  with **"vertex shader not expressible"**.  The same happens to any vertex
  shader that writes `gl_BackColor`, even when two-sided lighting is off.
* On the fallback path, the lowered fragment shader has a dynamic FACE
  input.  `chrome9_select_fs_variant()` can specialize FACE only when back or
  front faces are culled (`face_mode`).  Otherwise it marks the shader
  unsupported, and `chrome9_draw_vbo()` sends the draw to llvmpipe
  (`chrome9_sw_draw_vbo`).  Two-sided lighting without culling therefore
  currently renders entirely in software.

### 1.2 User clip planes, `gl_ClipDistance`, `clip_plane_enable`

* `caps->clip_planes = 0`, so `st_atom_shader.c` sets `key.lower_ucp`.
  `st_program.c:lower_ucp()` then either runs `nir_lower_clip_vs` (planes
  from state uniforms into `CLIP_DIST0/1` vec4 outputs, removing
  `CLIP_VERTEX`) or runs `nir_lower_clip_disable` for shaders that already
  write `gl_ClipDistance`.  `nir_lower_clip_disable` writes 0 to disabled
  lanes; it does not remove them.  `rs->clip_plane_enable` carries
  `ClipPlanesEnabled`.
* `chrome9_tcl_draw()` falls back at the first gate, **"rasterizer state"**,
  whenever `clip_plane_enable != 0`.  Separately, `tcl_store()` rejects
  `CLIP_DIST0/1` and `CLIP_VERTEX`, so a shader that writes clip distances
  with no plane enabled also falls back ("vertex shader not expressible").
* On the draw path, `draw_pipe_clip` clips against the distances in software
  before the post-TNL cmd22 upload, so the output is already correct there.

### 1.3 Kernel

`chrome9_vertex.c` knows the native destinations `0x10` (temp), `0x22`/`0x23`
(oD0/oD1), `0x28+n` (oTn) and `0xa0` (position).  It always emits `A61`
without bit 19.  `chrome9_tcl_raster_state()` sets A23 bit 8 from `CULL_CW`
and bits 14:12 from `FLAT`, and never sets bit 7.
`chrome9_fragment_fe_config()` always emits FE `A05 = 05000000`.  The UAPI
accepts only the flags `CULL_CW`, `CULL_CCW` and `FLAT`, and only the
destinations TEMP/POSITION/PRIMARY/VARYING.

## 2. Register and record encoding

### 2.1 Two-sided color

| item | encoding | evidence |
|---|---|---|
| back primary color | VS destination `0x24` (unified output 36, vendor `oD2`/`oBFD0`); `mov oD2, c0` = `0040014c 00000000 00000000 0008024f` | vendor-compiler (corpus `back_primary_from_c0`) |
| back secondary color | destination `0x25` (`oD3`/`oBFD1`), `…0008025f` | vendor-compiler (`back_secondary_oD3`) |
| saturate | the vendor compiler sets bit 14 on `0x22`/`0x23` (`0008422f`) but not on `0x24`/`0x25` (`0008024f`) | vendor-compiler |
| linkage side effect | writing oD2 also marks oD0 in the output descriptor (object `+0x04` = `0x44`), and oD3 marks oD1 (`0x84`) | vendor-compiler (`meta` column) |
| **A61[19]** | `A61[19] = (VS object +0x0c != 0)`; `+0x0c` gets bit 6 for oD2 and bit 7 for oD3 | static/cross: `dri:0x17c610..0x17c620` (byte `+0x2242` bit 3), `win:0x1015acfb..0x1015ad13` (`shl $0x13`, mask `0x80000`, image `+0x2264`); vendor-compiler for `+0x0c` values |
| meaning of A61[19] | enables oBFD selection | inference: it is the only recovered consumer.  The manual lists type-04 sub-addresses 53..62h as reserved (M:10301) |
| **A23[7]** | 0: CW triangles use oBFD#; 1: CCW triangles use oBFD# | manual M:9547..9549, p.161 |
| A23[7] rule | vendor `A23[7] = (glFrontFace == GL_CW)` in both the culled and unculled branch | static: `STM_UpdateCull_INV` `dri:0x196ade..0x196af8` and `0x196b40..0x196b5b`; `__glDpSTMUpdateCull` `dri:0x1ee2c3..0x1ee3e0` maps `GL_CW → 1`, `GL_CCW → 2` |
| A23[8] / A01[7] | culled winding / cull enable; vendor cull BACK + front CCW → A23[8] = 0 (cull CW) | manual M:9544..9546; static `dri:0x196aaf..0x196bdb` (cull FRONT = `1db4` 1, BACK = 2, FRONT_AND_BACK = 3) |
| FVF | no back-color bit in A25/A26/A28 on VX900 | manual M:9585..9942 (A25 bits 23..16 are X Y Z W PointSize Cd Cs Fog) |

Consequences:

* **Gallium rule.**  The back winding is the winding that `PIPE_FACE_BACK`
  culls.  `tcl_cull()` maps it to `CULL_CW` when `front_ccw`, and silicon
  rung L7 qualified that mapping.  The vendor derives A23[7] and A23[8] from
  `glFrontFace` in the same way.  Therefore
  `A23[7] = front_ccw ? 0 : 1`.  With back-face culling, A23[7] equals
  A23[8] (inference from two consistent static mappings; rung t1 against the
  CULL_CW reference proves it).
* **Culling.**  Culling is decided before selection (HCM manual algorithm,
  [chrome9-face-sign-bfc-producer.md](chrome9-face-sign-bfc-producer.md)).
  The state tracker clears `light_twoside` for culled back faces.  With
  front-face culling, every surviving triangle has the back winding and
  takes oBFD.  Rungs t4/t4b cover both cases.
* **FlatC.**  A23[14:12] = 3 takes the color of vertex c.  The selection is
  per primitive, so a flat two-sided triangle should show vertex c's back
  color (inference).  Mesa already rotates each triangle so that the GL
  provoking vertex is c (`tcl_triangle()`).  Rung t6 checks this.
* **Clipping.**  The manual defines the rule on "vertex input order".  After
  near-plane clipping, the generated polygon keeps its winding, so the
  selection should not change (inference, rung t7).
* **Points and lines.**  The vendor clears the back side for GL modes ≤ 3
  (`dri:0x230a94..0x230ab4`).  TCL is triangle-only today.  A later native
  point or line topology must not set BFC.

### 2.2 User clip planes

| item | encoding | evidence |
|---|---|---|
| FE A05 | `[15]` group-1 enable, `[14]` group-1 perspective, `[13:10]` group-1 source coordinate A..J, `[9:8]` group-1 dimension; `[7]`, `[6]`, `[5:2]`, `[1:0]` the same for group 0 | manual M:7928..7985, p.137 |
| comparison | not documented; the vendor writes signed plane distances, so the pixel is presumably rejected where an enabled component is negative | inference (rungs u2, u2n, u2z) |
| plane storage | vendor: 6 eye planes are converted to clip space and uploaded as internal rows 259..264 (`3203040c`, 24 dwords, unified 227..232) | static `dri:0x17be12`, `0x17c3d5`; chrome9-hw-tcl-re.md |
| plane evaluation | vendor programmable VS: one DP4 per enabled plane; the k-th enabled plane goes to destination `0x28 + N + k/3`, mask `1 << (k%3)`, where N = VS texture coordinate outputs (object `+4` bits 11:8) | static `stmPVSReBundleUCP_h5i` `dri:0x1607a7..0x160853` (mask `& 0x3f`, rows `0x103+i`, `N + 0x28`) |
| count limit | 6 planes (`& 0x3f`), two groups of three lanes; planes 4..6 use the second group | static (as above); table `dri:0x3f1d00` = `{dim0, dim1, en0, en1}` per count: 1 `{0,0,1,0}`, 2 `{1,0,1,0}`, 3 `{2,0,1,0}`, 4 `{2,0,1,1}`, 5 `{2,1,1,1}`, 6 `{2,2,1,1}` |
| sources | group 0 = coordinate N, group 1 = N+1 | static `stmUpdateTCI_invi` `dri:0x17e608..0x17e719` |
| perspective bits | the vendor keeps A05[6] and A05[14] from the image (`& 0x7f`/`& 0xc3`, `& 0x43`) and never computes them | static `dri:0x17e653..0x17e6ae` |
| A08 | `A08[4:0] = coordinate count + ceil(planes/3)` | static `dri:0x17ddb4..0x17ddf5` (`(n+2)*0xaaaaaaab>>33`) |
| VS output FVF | S/T/R presence nibbles for the UCP coordinates are ORed into A25/A26 (the whole 24-bit fields); only A28 bits 18:17 (colors) are updated there | static `stmUpdateFVFOut_H5i` `dri:0x1ba9c0..0x1baa2d`, `0x1ba82b..0x1ba85a`, `0x1baa38..0x1baa51` |
| A28/A29 textures | built from the PS coordinate count (FE A00[7:4] = PS object `+0x34`) and per-coordinate dimensions | static `stmUpdatePS_inv` `dri:0x17fa1b..0x17fb5b`; that the UCP coordinates are excluded is inference |
| I/J | used only when N = 7 with more than three planes (`dri:0x160844`, table `dri:0x3f1c80`) | static; not supported by this plan |

Resulting words for N = packed fragment varyings (dimension code = lanes-1):

| planes | A05 | A08 low bits | A25/A26 producer nibble(s) at N, N+1 |
|---:|---|---|---|
| 1 | `05000080 \| N<<2` | N+1 | `8` |
| 2 | `05000081 \| N<<2` | N+1 | `c` |
| 3 | `05000082 \| N<<2` | N+1 | `e` |
| 4 | `05008082 \| N<<2 \| (N+1)<<10` | N+2 | `e`, `8` |
| 5 | `05008182 \| …` | N+2 | `e`, `c` |
| 6 | `05008282 \| …` | N+2 | `e`, `e` |

Consequences:

* **No plane rows are needed.**  Mesa already emits the plane DP4s as
  ordinary VS code with CB0 uniforms (`nir_lower_clip_vs`), so the kernel
  only routes `CLIP_DISTANCE` lanes.  This also avoids the vendor CALL/RET
  subroutine form, because VS control flow is not qualified on silicon.
* **Varying budget.**  `varying_count + ceil(n/3) ≤ 8` (A..H).
* **Interpolation.**  The test is per pixel after interpolation, so the
  primitive is not subdivided.  GL correctness depends on perspective-correct
  interpolation of the distance, and the perspective bit is not set by the
  vendor.  Rungs u4 and u4p measure this.
* **Cull and FlatC.**  UCP coordinates are ordinary texture coordinates,
  independent of A23.  Flat shading covers colors only, so the distances
  stay interpolated (manual A23[14:12] names vertex colors; inference).

## 3. UAPI draft (patch 0001)

* New `drm_chrome9_vertex_dest` values:
  * `DRM_CHROME9_VERTEX_DEST_BACK_PRIMARY = 4`: index 0, all four lanes
    written once.  It requires a BFC flag and is dropped when the fragment
    program does not read PRIMARY.
  * `DRM_CHROME9_VERTEX_DEST_CLIP_DISTANCE = 5`: index = plane (< 6),
    writemask X only.  Planes in `clip_distance_mask` must be written once;
    writes to other planes are dropped, which follows the convention for
    unconsumed outputs.
* Flags `DRM_CHROME9_VERTEX_BFC_CW (1 << 3)` and `BFC_CCW (1 << 4)` name the
  screen winding that takes BACK_PRIMARY.  At most one may be set.  This
  mirrors `CULL_CW`/`CULL_CCW`.
* `__u32 clip_distance_mask; __u32 pad0;` are taken from the reserved tail
  (`reserved[4]` becomes `reserved[3]`).  The struct size is unchanged, and
  older kernels reject a non-zero mask.
* `DRM_CHROME9_VERTEX_MAX_CLIP_DISTANCES 6`.
* Capability bits `CAP_VERTEX_TWO_SIDE` (60) and `CAP_VERTEX_CLIP_DISTANCE`
  (61) are **provisional**.  The ledger
  [chrome9-uapi-allocation.md](chrome9-uapi-allocation.md) is stale (it
  still names 48 as next free), and the line/point proposal in
  chrome9-lines-points-twoside-re.md §6.2 also starts at 60.  Reserve the
  bits in the ledger at merge time.

## 4. Kernel draft (patches 0002..0005)

**Compiler (`chrome9_vertex.c`, patch 0002).**
`chrome9_vertex_compile()` gains `bool two_side, u32 clip_mask`.

* BACK_PRIMARY encodes to destination `0x24` with bit 14 set.  The
  saturate is a deviation from the vendor, chosen so that back colors clamp
  like PRIMARY; rungs t5 and t5n decide whether it is needed.  When
  emitted, the compiler also adds a zero fill of `0x25` (`…0008425f`),
  because A61[19] covers both oBFD registers and the kernel already fills
  oD1 by the same rule.  It also sets `A61 |= BIT(19)`.
* A CLIP_DISTANCE write to enabled plane p gets rank
  `k = popcount(mask & (BIT(p)-1))` and encodes to destination
  `0x28 + varying_count + k/3` with mask `1 << (k%3)`.  For lane-wise ops,
  the lane-0 source selectors are copied to lane k%3.  Dot products
  broadcast, so they need no change.
* Validation covers: a complete BACK_PRIMARY when two-sided and PRIMARY is
  consumed; every enabled plane written exactly once; mask bits < 6; and
  `varying_count + ceil(n/3) ≤ 8` (otherwise `-E2BIG`).
* Host self-tests check the vendor-derived records `0040014c … 0008424f`,
  `0008014c 0040014c 0 00060291` (the vendor example "DP4 oT1.x"),
  `00060292` and a relocated ADD `0008410c 00206490 0040810c 00040294`,
  plus A61 `61880000` and the rejection paths.

**Engine (`chrome9_engine.c`, patch 0003).**

* `chrome9_tcl_raster_state()`: `A23 |= BIT(7)` for `BFC_CCW`.  The UCP
  producer nibbles go into A25/A26 at coordinates N.., and the A12
  output mask counts them.  With UCP but no varyings, the generic link
  words replace the fixed `25f6cc00 28ffcc00` pair (rung u7).
* New `chrome9_fragment_fe_ucp()` writes FE A05 as in the table above.  It
  also writes the A08 count and the A08/A09/A0A dimension fields for the
  UCP coordinates, using the producer-width layout.  It does not write
  PS consumer words (FE A01..A03, A28/A29).
* `chrome9_draw_vertex_program_ioctl()` validates the new flags, the mask
  and `pad0`, and passes them to the compiler.
* Host self-tests: `A05 05000085` (N=1, 2 planes), `05008886` (N=1, 4
  planes; this equals the vendor-table value `05008082|N<<2|(N+1)<<10`),
  `A23 23004f82`, `A25 25f6cc00`, `A28 28ffc000`.

**Patch 0004 (temporary).**  Adds the mutable module parameter `tcl_re`,
sampled once per ioctl.  Bit 0 clears A61[19], bit 1 drops bit 14 on
`0x24`, bit 2 sets A05[6]/[14], and bit 3 lists the UCP coordinates in
A28/A29.  This follows the precedent of the removed `tcl_a61_or` knob
(commit `a8b2445`).  Drop it after the ladder.

**Patch 0005.**  Advertises capability bits 60/61.  Apply only after the
ladder passes.

## 5. Mesa draft (patches 0001..0003)

* Sync `include/drm-uapi/chrome9_drm.h`.
* `chrome9_screen.c`: `two_sided_color = true`.  Without TCL, the draw
  module's twoside stage (`draw_pipe_twoside.c`, which runs after clip and
  cull and so uses the final determinant) selects BCOLOR.  The fragment
  shader no longer receives a FACE-based lowering, which removes the
  llvmpipe route of §1.1.  This is a separate patch because it also changes
  the non-TCL path; re-run the glmark2 and ioquake3 matrices after it.
* `chrome9_tcl.c`:
  * `tcl_store()`: `BFC0` becomes `BACK_PRIMARY` when the variant is
    two-sided and is dropped otherwise.  `BFC1` and `CLIP_VERTEX` are
    ignored; COL1 is not consumed either, and the state tracker has already
    lowered `CLIP_VERTEX` whenever a plane is enabled.  `CLIP_DIST0/1` go to
    the new `tcl_store_clip()`, which emits one scalar `CLIP_DISTANCE` MOV
    per enabled lane.
  * The TCL variant key gains `twoside` and `clip_enable`, and
    `chrome9_tcl_compile()` takes both.
  * `chrome9_tcl_draw()`:
    `twoside = light_twoside && cull_face != PIPE_FACE_BACK`.  It falls back
    with "two-sided color" or "user clip planes" when the kernel lacks the
    capability or more than six planes are enabled, and with "unwritten clip
    distance" when an enabled plane is not written.  It sets
    `draw.flags |= front_ccw ? BFC_CW : BFC_CCW` and
    `draw.clip_distance_mask = clip_plane_enable`.
* `chrome9_context.[ch]`: `tcl_twoside` and `tcl_clip_distance` from the
  capability bits.
* Limitation: a GLSL `gl_ClipDistance[]` store with a non-constant or
  non-zero IO offset still sets `failed` and uses the draw module.

## 6. Silicon ladder (tools/re/chrome9_tcl_oracle.c, `oracle.patch`)

The ladder uses the common harness: GPU lock, hash-gated module handoff and
runs capped at about 7 s, as in [hardware-lab.md](hardware-lab.md).  Every
rung checks the guard pages.  Every rung ends by redrawing the qualified
reference, and a mismatch there counts as a failure.  Steps marked
*observe* record data and never pass or fail on colors.  Run the steps in
the order listed below, and stop at the first failure or timeout.

`twoside <step>`: triangle A and its opposite-winding mirror B, PS
`MOV oC0, PRIMARY`, PRIMARY = white, BACK_PRIMARY = c0
(`ff4080bf`).  Each run first draws with `CULL_CW` only, which is qualified
by L7, to learn which triangle is CW.

| step | single variable vs previous | expectation | hang risk |
|---|---|---|---|
| t0 | baseline, no BFC, no oD2 | both white | low (qualified state) |
| t1 | + oD2 write, A61[19], `BFC_CW` (A23[7]=0) | CW = `ff4080bf`, CCW white | low–medium (vendor form except bit 14) |
| t3 | `BFC_CCW` (A23[7]=1) | colors swapped | low (vendor form) |
| t4 | t1 + `CULL_CCW` | CW back color, CCW blank | low |
| t4b | t1 + `CULL_CW` | CW blank, CCW white | low |
| t6 | t1 + `FLAT`, per-vertex colors | CW = back color of vertex c, CCW = front color of vertex c | low |
| t8 | BFC draw, then a plain draw | plain draw both white (A61/A23 re-emitted) | low |
| t5 | BACK = (2, −1, .5, 1) with bit 14 | CW `ffff0080` | low–medium (bit 14 on `0x24` is not a vendor form) |
| t7 | t1 with the CW triangle crossing the near plane | every remaining CW pixel back color | medium (clipper plus oBFD) |
| t2 | t1 with `tcl_re=1` (oD2 written, A61[19]=0) | *observe*; inference: both white | medium (vendor never emits this) |
| t5n | t5 with `tcl_re=2` (no bit 14) | *observe*: does the hardware clamp? | low (vendor form) |

`ucp <step>`: full-target quad (4096 pixels), one T0 varying = c0, planes as
VS DP4 against clip-space v0.  Row 16 reports the first and last covered
column.

| step | single variable | expectation | hang risk |
|---|---|---|---|
| u0 | no planes | 4096 | none |
| u1 | 1 plane, distance constant +1 (A05 `05000084`, A08 2, A25 nibble 8) | 4096 | **medium**: first FE UCP use.  A05, A08 and A25 change together by construction |
| u2 | plane = x_clip | 2048, first column 32 | low after u1 |
| u2n | plane = −x_clip | 2048, columns 0..31 | low |
| u3 | + y plane (ST, `05000085`) | 1024, column 32 | low–medium |
| u3b | + third plane x_ndc ≤ .5 (STR, `05000086`) | 512 | low–medium |
| u5 | 4 planes, second group (`05008886`, A08 3) | 2048 | medium |
| u6 | two varyings, UCP at coordinate 2 (`05000088`) | 2048 | low–medium |
| u2z | distance zero at the centre of column 31 | *observe*: inclusive or exclusive | low |
| u4 | right edge w=4, A05[6]=0 | *observe*: boundary at column 32 (perspective) or near 12.8 (screen-linear) | low |
| u4p | u4 with `tcl_re=4` (A05[6]=1) | *observe* | medium (the vendor never writes this bit) |
| u1c | u1 with `tcl_re=8` (UCP bits in A28) | *observe*, only if u1 fails | medium |
| u7 | zero varyings, PRIMARY output, generic link words | 2048 | **medium–high**: the zero-varying linkage is qualified only with the fixed words |

Later rungs, not in the draft: UCP on a near-clipped triangle (U8), and
two-sided color combined with UCP.

The two-sided color and UCP rungs are independent, so either ladder can be
merged alone.  The only shared change is the `tcl_re` knob.

## 7. Files and host verification

Drafts (patch `-p1` against the tree roots, signed off, with C function
context in the hunk headers):

* `~/tmp/agent-twoside/kernel/0001..0005-*.patch` (linux-7.2 root): UAPI,
  vertex compiler, engine, `tcl_re` knob, capability advertisement.
* `~/tmp/agent-twoside/mesa/0001..0003-*.patch` (mesa-26.2.2 root): UAPI
  sync, `two_sided_color`, TCL lowering.
* `~/tmp/agent-twoside/oracle.patch` (repository root): `twoside` and `ucp`
  modes.
* `~/tmp/agent-twoside/base/`: the snapshots the patches were made against.
  `~/tmp/agent-twoside/engine_host_test.sh` runs the extracted raster/FE
  self-tests on the host.

Host checks (no GPU access):

* The patched `chrome9_vertex_selftest` passes through
  `tools/re/chrome9_kvertex_selftest.c`.
* The patched raster and FE self-tests pass.  Deliberately wrong expected
  values make both fail, which shows the tests run and are not stale.
* The patched module compiles out of tree against the configured
  linux-7.2 tree without warnings; the `.ko` was deleted.
* The three Mesa files compile with the `objs-chrome9-full` flags with no
  new warnings.
* The oracle builds with `-Wall -Wextra`.
* All kernel patches apply in sequence to a copy of the current live tree.
  The Mesa patches apply to the current live Mesa tree with offsets (one
  hunk with fuzz 2).  Rebase both after stage B lands.

## 8. Open questions

1. What exactly A61[19] enables, and what an unwritten oBFD delivers when it
   is set (rung t2 measures the case of oD2 written with A61[19] = 0).
2. Whether the hardware clamps oBFD0 without bit 14 (t5n), and whether the
   oBFD1 fill is required at all.
3. The UCP comparison and its zero boundary (u2, u2n, u2z), and whether
   A05[6] is needed for GL-correct interpolation (u4, u4p).  The image
   default of A05[6]/[14] in the vendor driver was not located.
4. Whether A28/A29 must list the UCP coordinates (static analysis says no;
   u1c exists only for a failing u1), and whether the FE A08/A09/A0A
   dimension fields matter for the UCP coordinates; the vendor writer of
   those fields for UCP was not traced.
5. Q lanes (four planes per coordinate, which would allow eight planes)
   and the I/J coordinates (N = 7).  Neither is used here, so Mesa keeps
   planes 7/8 and draws with 8 varyings plus UCP on the draw module.
6. The zero-varying UCP linkage (u7) and near-plane clipping with oBFD or
   UCP coordinates (t7, U8).
7. The capability bit numbers (§3).
8. Windows D3D UCP and back-color routes were not audited beyond the A61[19]
   producer (`win:0x1015acfb`).
