<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 queries, MSAA, volume textures, shader flow control and native points/lines

Status: static reverse engineering plus manual cross-check, 2026-09-24.  Nothing
in this note has run on VX900 silicon.  It consolidates and extends
[occlusion-query.md](occlusion-query.md),
[msaa-aa-viewport-clip-contract.md](msaa-aa-viewport-clip-contract.md),
[fixed-function-texture-sync.md](fixed-function-texture-sync.md) (volume
section), [via-5.75-shader-re.md](via-5.75-shader-re.md),
[chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md),
[glmark2-control-flow-specialization-contract.md](glmark2-control-flow-specialization-contract.md)
and [chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md).

New results in this note:

* the manual documents a second, fully specified occlusion read-back path:
  PPC type `17h` with explicit SL/SF location (pp.174-175);
* the current kernel 3D job tail already resets the occlusion counter, so a
  per-job counter segment is exact without any extra reset packet;
* the vendor GL `glBeginQuery` accepts only `GL_SAMPLES_PASSED`; there is no
  GPU timestamp source in either manual part;
* the Windows D3DCAPS9 template proves volume textures are supported but
  power-of-two only and without mip filtering, and proves `vs_2_0` static flow
  control depth 1 with no dynamic flow control and no predication;
* the vendor linear volume layout is slice stride = row pitch x height
  (`sfm3DTextureBlt_inv`), and GL `GL_TEXTURE_3D` shares coordinate key 3 with
  cube maps;
* the H6 multisample helpers `sfmGetMSParam_inv` and `sfmGetResampleInfo_inv`
  are constant-zero stubs.

Evidence labels:

* **manual**: `docs/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, printed
  page numbers (`pdftotext -layout`); Part I is named explicitly;
* **static**: disassembly of `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`
  (i386, symbols; `dri:0x...`) or
  `binary/VT3410_Windows7_241601j_VIAwSetup_LOGO/VTGUModeDX32A.dll` (`win:0x...`);
* **cross**: the same fact in both vendor stacks;
* **source**: current Chrome9 kernel/Mesa trees;
* **inference**: derived, not observed; marked where used.

## 1. Ranked summary

| rank | feature | hardware | confidence | plan | CAP2 bit | rung |
|---:|---|---|---|---|---|---|
| 1 | occlusion query (`GL_ARB_occlusion_query`, predicates, conditional render in software) | 32-bit Z+stencil pass counter A1C/A1D; vendor CR read-back packet; PPC type 17h write-back | high (manual + exact vendor packet); silicon unmeasured | kernel query objects + per-draw counter segments; Mesa `pipe_query` | 6 `OCCLUSION_QUERY` | `occlusion` O1-O6 |
| 2 | 3D (volume) textures | A18 mode 2, A21 depth, A22[11:8] log2 depth, A32 R wrap/filter | high for registers and layout; no-mip is manual + D3D caps | versioned texture state; POT, single level | 7 `TEXTURE_3D` | `volume` V1-V3 |
| 3 | native points, sprites, point size | A23 point, A2A/A2B size, A01[21] sprite, A2F[9:0] | high (manual + vendor setters); silicon unmeasured | topology POINT_LIST + raster extension | 9 `NATIVE_POINTS` | `points` P1-P2 |
| 4 | native lines, stipple | A23 line, A20 strip reuse, A01[22]/[23], A84/A85 | high; wide lines have no register | topology LINE_LIST/LINE_STRIP | 10 `NATIVE_LINES` | `lines` L1-L3 |
| 5 | VS static flow control (REP/LOOP, IF b#, CALL) | native ops 28/29, 9-bit displacements, i# and b# stores | high for encodings (offline corpus); semantics unmeasured | structured ops in the kernel VS compiler | 8 `VERTEX_STATIC_FLOW` | `vsflow` F1-F3 |
| 6 | polygon mode (fill/line/point per winding) | A23[3:0], [6:5], bit 4 | high for encoding; coverage unmeasured | raster extension; draw unfilled stage stays fallback | 11 `POLYGON_MODE` | `polymode` M1-M2 |
| 7 | pipeline statistics | PPC types 00h-15h | manual only | optional, after rank 1 | 12 (candidate) | reuse O-rung harness |
| - | timer/timestamp query | none | high (negative) | do not expose GPU timer queries | none | none |
| - | MSAA | none (edge AA only) | high (negative) | keep sample count 1 | none | none |
| - | PS flow control | none | high (negative) | Mesa lowering + software fallback only | none | none |

CAP2 bits 1-4 remain reserved for cube/DXT and bit 5 is the main session's
`DRM_CHROME9_CAP2_VERTEX_TOPOLOGY`.  The bit numbers above are proposals;
`~/tmp/agent-rq/chrome9_rq_proposed.h` carries them together with the draft
UAPI used by the rungs.

## 2. Occlusion queries

### 2.1 Counter

Type-01 A1C `[23:0]` is the "Low 24 Bits Occlusion Count of both Z Test and
Stencil Test Result" and A1D `[7:0]` the high eight bits (manual p.95).  Both
are write-only; writing zero resets the full 32-bit counter.  The vendor
begin-query packet is exactly that reset (`hwmUpdateStatus_h5`, `dri:0xac970`,
recovered in [occlusion-query.md](occlusion-query.md)):

```text
FE020440 0000043C 00010000 CC000000
1C000000 1D000000 CCCCCCCC CCCCCCCC
```

The current kernel emits the identical group in every 3D job tail
(`chrome9_emit_3d_tail()`, `chrome9_engine.c`, `status[] = { 0x1c000000,
0x1d000000, ... }`) (source).  Every 3D job therefore starts with a zero
counter as long as no other client submits 3D work in between, which the
single serialized engine timeline already guarantees.  Consequence: a query
can be attached per draw without any global counter state.

### 2.2 Read-back path A: vendor CR register read-back (exact packet, SL target)

`hwmUpdateStatus_h5:0xaca68..0xacbaf` (static) snapshots the counter:

```text
FE020420 0000041C 00100000 CC000000      type-10 group
02000000 0300011C CCCCCCCC CCCCCCCC      A02 = read-back ID 0, A03 = 00 01 1C
FE020440 0000043C 00010000 CC000000      type-01 group
21000000|(Q & 00ffffff) 22000000|(Q >> 24) CCCCCCCC CCCCCCCC
```

Manual p.74 defines type-10 A02 `[7:0]` as the read-back source ID and A03
`[23:0]` as `HParaSubType<<16 | HParaType<<8 | subaddress`; `00011C` names
type-01 A1C.  Type-01 A21/A22 are absent from the manual (the register list
jumps from A20 to A23, p.96); VIA's old KMD verifier names them
`check_zocclusion_addr0/1`.  `Q` is a byte offset in SL: the ICD computes it
from its aperture base and leaves A22 domain bits clear.  An SF destination
encoding for A22 is unknown and must not be guessed.

### 2.3 Read-back path B: PPC write-back (manual, explicit location)

HParaType 08h (manual pp.174-175):

| sub | field | meaning |
|---|---|---|
| A01 `[4:0]` | PPC type | `00h`..`15h` pipeline counters, `16h` working counter, `17h` HZOcclusionCNT |
| A02 `[23:0]` | address low 24 bits | in 4-byte units |
| A03 `[23:22]` | location | `00` SL, `01` SF, `10`/`11` reserved |
| A03 `[8]` | fire | 1 writes the selected counter to memory |
| A03 `[7:0]` | address high 8 bits | 4-byte units |

The derived packet (inference from the table; not vendor-observed, no PPC
writer was found in `via_chrome9_dri.so`) is:

```text
FE020440 0000043C 00080000 CC000000
01000017 02000000|((A>>2)&00ffffff) 03000000|(loc<<22)|0100|((A>>2)>>24) CCCCCCCC
```

Path B can target SF directly, but it is manual-only; path A is
vendor-exact but SL-only.  The rung below measures both.

### 2.4 Negative results for timers

* `__glim_BeginQuery` (`dri:0x2a1aa0`) accepts only target `0x8914`
  (`GL_SAMPLES_PASSED`) and raises `GL_INVALID_ENUM` otherwise
  (`dri:0x2a1ac7..0x2a1ad7`); `GL_TIME_ELAPSED` is not handled (static).
* The Windows query manager accepts only D3D9 `EVENT` and `OCCLUSION`
  ([windows-draw-path.md](windows-draw-path.md)).
* Neither manual part has a GPU clock or timestamp write.  Part I timers are
  VGA/display/DP AUX timers (Part I, "Timer Control Register" 3C5.5D and DP
  "Software Timer Counter"), not command-stream stamps.  The fence engine
  (T11 A05..A07, p.82-83) writes IDs, not times.
* PPC `16h` "working counter enabled by HenWK_CNT" names an enable bit that
  is documented nowhere else.  It may be a busy-cycle counter, but that is
  speculation; it must not back `PIPE_QUERY_TIME_ELAPSED`.

Recommendation: do not advertise `GL_ARB_timer_query`.  A CPU-side
`PIPE_QUERY_TIMESTAMP` derived from `dma_fence` signal time would only measure
the polling retirement worker (`chrome9_engine_retire_locked()`), not GPU
execution.

### 2.5 Kernel plan

1. UAPI (draft in `chrome9_rq_proposed.h`): `DRM_CHROME9_QUERY` (0x22) with
   CREATE/DESTROY/RESET/READ(+WAIT); `DRM_CHROME9_FRAGMENT_QUERY` flag (bit 4)
   with the query id in `fragment.reserved[0]`.  Both fields are rejected by
   current kernels, so the extension is fail-closed and needs no new ABI
   version.  Advertise `DRM_CHROME9_CAP2_OCCLUSION_QUERY` (bit 6).
2. Query objects: per-file `xarray` of `{u64 samples; struct dma_fence *last;
   int error}`.  Accumulation happens in the kernel, so the GPU never writes
   user memory and the result has 64 bits although the counter has 32.
3. Snapshot storage: one kernel-owned SL page (inside the reserved local
   frame-buffer range, accessed through the BAR2 aperture already located by
   `chrome9_detect_vram()`), with a slot allocator sized from the page and
   slots owned by the job until retirement.  Each job carries a dynamically
   sized array of `{query, slot}` pairs.
4. Emission per draw carrying a query: A1C/A1D reset before the draw (explicit,
   so the result does not depend on tail ordering), the draw, then the path-A
   snapshot into the slot.  Poison the slot with `ffffffff` before
   publication.  Keep the existing tail reset after the snapshot.
5. Retirement (`chrome9_fragment_job_finish_locked()`): read each slot, reject
   `ffffffff` as an incomplete snapshot (set the query error, keep the job
   quarantine policy), add to the accumulator, free the slot and replace
   `last` fence.
6. READ+WAIT: bounded `dma_fence_wait_timeout()` on `last`, then return
   `result` and `available=1`; an error returns `-EIO`.
7. If rung O-B shows path B works into SF, the SL page can be replaced by a
   pinned GART page; the UAPI is unchanged.

### 2.6 Mesa plan

* `chrome9_query.c`: `create_query` for `PIPE_QUERY_OCCLUSION_COUNTER`,
  `OCCLUSION_PREDICATE` and `OCCLUSION_PREDICATE_CONSERVATIVE`;
  `begin_query` flushes the draw batch and issues RESET; draws attach the
  active id in both the generic fragment path and TCL (`fragment.reserved[0]`
  of each `drm_chrome9_draw_vertex_program`); `end_query` flushes;
  `get_query_result` issues READ, with WAIT when `wait` is set.
* `set_active_query_state(false)` (u_blitter) must suppress attachment.
* Every software path must add its own samples: llvmpipe/`chrome9_swfb`
  fallbacks and the draw-module point/line expansion when it runs through
  software.  A draw that falls back without counting must fail the query
  (report the result as unavailable with an error) rather than under-count.
* Conditional rendering: implement in software in `render_condition` +
  draw entry (wait for the result, skip when zero), as softpipe does.
* Caps: `occlusion_query`, `conditional_render` only with CAP2 bit 6.

### 2.7 Rungs (`occlusion`)

All on a guarded 64x64 BGRA target and D16 depth cleared to `ffff`, triangle
`(8,8),(56,8),(8,56)` at z=0.5, D16 LESS; every pixel read is preceded by
`GEM_CPU_ACCESS(WAIT|PREPARE_READ)`:

| step | action | exact oracle |
|---|---|---|
| O1 | red triangle | result == number of red pixels (nonzero) |
| O2 | same triangle, green, same z | result 0 and no green pixel |
| O3 | query without draws | result 0, available |
| O4 | the hidden triangle again plus its complementary triangle, two ioctls, one query | result == newly blue pixels (accumulation across jobs) |
| O5 | full-target quad on fresh depth | result 4096 (snapshot waited for the whole draw) |
| O6 | depth disabled (informative) | result is coverage or 0; records whether the counter counts without Z test |

Kernel-only precursor (before the UAPI exists), behind a debug module
parameter: emit the path-A snapshot of the existing fragment smoke job into a
fixed SL dword and log it; oracle = CPU-counted changed pixels (the
`chrome9_fragment_smoke` triangle).  Repeat with path B (`loc=01`, SF dword in
the job's own guarded destination page tail) to decide the storage domain.

## 3. MSAA

Negative, high confidence.  The existing contract in
[msaa-aa-viewport-clip-contract.md](msaa-aa-viewport-clip-contract.md) stands
and is strengthened:

* Manual: only A01 bit 5 "Enable Anti-Aliasing" plus A23 `[11:9]` edge flags
  (p.91, p.161); no sample count, sample position, sample mask, multisample
  surface layout or resolve.
* D3D caps template `win:0x1030fcc8` (layout validated by
  `PixelShaderVersion=ffff0200` at offset `0xcc`): `RasterCaps=077333b7` has
  the legacy edge-AA bit `0x1000` but not `D3DPRASTERCAPS_MULTISAMPLE_TOGGLE`
  (`0x08000000`); D3D9 multisample render states are packet no-ops
  (dispatcher entry 3, `win:0x100cada9`).
* Linux: `sfmGetMSParam_inv` (`dri:0xec640`) and `sfmGetResampleInfo_inv`
  (`dri:0xec630`) are `xor eax,eax; ret` stubs, as are
  `sfmNeedTargetPatch_inv`, `sfmInitTempTarget_inv` and
  `sfmUpdateOrigTarget_inv` (`dri:0xec650..0xec676`).
  `__glRenderbufferStorage` (`dri:0x2d0cb0`) accepts `samples <= 8` generically
  and stores the value (`dri:0x2d0e24`), but no chip path consumes it.
* The ICD string pool contains `GL_ARB_multisample` and
  `GL_EXT_framebuffer_multisample`, yet the previously recovered extension
  builder does not append them ([gl-feature-matrix.md](gl-feature-matrix.md)).

Plan: keep `nr_samples <= 1` (already enforced in `chrome9_resource.c`).
Supersampling by rendering at 2x and down-filtering with the texture sampler
would be a Mesa-only feature with no hardware support; not recommended given
fill rate.  Edge AA stays debug-only per the existing contract.

## 4. 3D (volume) textures

### 4.1 Registers (manual pp.123-124, 131)

| word | field | volume value |
|---|---|---|
| A18 `[11:8]` | texture mode | `0010` "3 Dimension volume texture. S, T, R coordinates" |
| A18 `[12]` | base-address mode | vendor sets 1 (offset table relative to L0) |
| A18 `[1:0]` | face0/volume location | `01` SF |
| A20 | `[23:12]` height, `[11:0]` width | |
| A21 `[11:0]` | "Length of Texture Level 0, Maximum to 2048 (for 3D Volume Texture's R Axis)... NO MIP for 3D volume texture" | depth |
| A22 `[11:8]` | exponent of length (depth), max 11 | log2 depth |
| A22 `[15]` | width and height both power of two | 1 |
| A32 `[8:6]` | R axis: 000 border, 001 clamp, 010 repeat, 011 mirror, 111 mirror once | |
| A32 `[21:19]` / `[18:16]` | R filter for enlargement / shrinking: 000 nearest, 001 linear | |

A minimal SF volume image is `18001201 20004004 21000004 22008222` for a
4x4x4 texture (inference assembled from the fields; A18 matches the vendor
`dri:0x1b2a90..0x1b2aa3` mode-2 path).

### 4.2 Vendor capability and layout

* D3D caps template (static, `win:0x1030fcc8`): `TextureCaps=00076d47`
  includes `VOLUMEMAP` (`0x2000`) and `VOLUMEMAP_POW2` (`0x40000`) but not
  `MIPVOLUMEMAP` (`0x8000`); `VolumeTextureFilterCaps=03000300` has min/mag
  point+linear and no mip filter bits, whereas `TextureFilterCaps=07030700`
  has them; `MaxVolumeExtent=2048`; `VolumeTextureAddressCaps=3f`.  This
  agrees with the manual's no-mip sentence.  Volume textures are therefore
  POT-only and single-level on the vendor's own claims.
* The Linux builder still walks the mip chain for volume resources and fills
  the subtype-10 offset table (`dri:0x1b2aa6..0x1b2b9d`), so the vendor does
  not rely on the table being ignored; the manual and D3D caps are the
  stronger evidence.
* Upload layout: `sfmTexture_Write` routes volume resources (resource flag
  `+0x22` bit 3) to `sfm3DTextureBlt` (`dri:0xcb80a`, `0xcbc38..0xcbc62`).
  The H6 implementation `sfm3DTextureBlt_inv:0xe2780` waits for idle and
  copies rows with `memcpy`; the slice step is `pitch(+0x194) * height(+0x17c)`
  (`dri:0xe2894..0xe28b4`), advanced per slice at `dri:0xe29dc..0xe29ee`.
  Slices are therefore contiguous with no extra alignment (static, high for
  the CPU layout; that the sampler uses the same stride is an inference).
* GL coordinate path: `__glSetTexEnableDimension` (`dri:0x28c570`) gives cube
  priority 4, 3D 3, rectangle 5, 2D 2, 1D 1.  `texEnableToStateKey`
  (`dri:0x4299e0`) = `0,1,2,3,3,5`, so 3D and cube share state key 3 (a
  four-component coordinate tuple, see
  [chrome9-format-cube-bridge-closure.md](chrome9-format-cube-bridge-closure.md));
  the declaration mask table `dri:0x3f8e60` gives `Tn.xyz` for dimension 3.
  Only key 4 (cube) switches TEXLD to TEXLDP, so a volume lookup is the
  ordinary TAU operation 1 (static; the TAU record for a volume sampler has
  not been compiled offline yet and is the first offline check).

### 4.3 Current driver state (source)

`chrome9_screen.c` never sets `max_texture_3d_levels` (Gallium default 0) and
`chrome9_resource.c` accepts only `PIPE_TEXTURE_2D`/`RECT` surface targets, so
every GL 3D texture currently fails at `glTexImage3D`.

### 4.4 Plan

Kernel (CAP2 bit 7 `TEXTURE_3D`):

* texture state `reserved[0]` = target (`0` 2D, `1` 3D), `reserved[1]` =
  depth, `reserved[2]` = wrap_r (draft; rejected today);
* require POT width/height/depth, `first_level == last_level`, mip filter
  NONE, BO range `level_offset[0] + level_pitch[0] * height * depth`, pitch
  canonical (`ALIGN(width * cpp, 32)`) for the first rung;
* emit A18 mode 2, A21 depth, A22 log2 depth, A32 R wrap and R filters from
  `min/mag_filter`; program the FE coordinate for this unit with three used
  lanes (vendor key 3 uses the four-lane tuple; start from the vendor tuple);
* restore A18 mode 1/A21=1 on the next 2D draw (transition closure).

Mesa:

* allow `PIPE_TEXTURE_3D` resources with slices at `stride * height`,
  `max_texture_3d_levels = 12`;
* use native sampling only for single-level views or `mip_filter NONE`
  (GL `GL_TEXTURE_BASE_LEVEL == MAX_LEVEL` or non-mip min filter) and POT
  sizes; otherwise select the llvmpipe fragment fallback
  ([chrome9-llvmpipe-fragment-fallback-design.md](chrome9-llvmpipe-fragment-fallback-design.md));
* lower `sampler3D` TEX in NIR to a TEX with a three-component coordinate.

### 4.5 Rungs (`volume`)

4x4x4 BGRA volume, canonical 32-byte rows, slice stride 128 bytes; slices 0,
2, 3 uniform red/blue/white, slice 1 unique per texel:

| step | coordinate | exact oracle |
|---|---|---|
| V1 | `s=1.5/4, t=2.5/4, r=(k+.5)/4`, nearest, k=0..3 | every covered pixel == texel (1,2,k) |
| V2 | `r=1.125`, REPEAT then CLAMP | slice 0, then slice 3 |
| V3 | `s=t=.5/4, r=3/4`, linear | average of slices 2 and 3, `ff7f7fff` +-1 |

A V1 failure where slice 1 shows a row-shifted texel identifies a different
slice stride (the next variant is `ALIGN(pitch*height, 256)`).

## 5. Pixel-shader flow control

Negative, high confidence; no kernel work.

* Windows caps (`win:0x1030fcc8`): `PixelShaderVersion=ps_2_0`,
  `PS20Caps.Caps=0` (no predication), `DynamicFlowControlDepth=0`,
  `StaticFlowControlDepth=0`, `NumInstructionSlots=96`,
  `MaxPixelShader30InstructionSlots=0` (offset `0x12c`).
* Linux PS compiler: the dispatchers `stmPPSDispatcherPS14/PS113/PS20_invi`
  and the `stmPPS_*` emitter set contain no IF/LOOP/REP/CALL/LABEL entry
  (symbol list, static); production compilation of `IF c0; MOV oC0,c1; ENDIF`
  emits only the MOV
  ([glmark2-control-flow-specialization-contract.md](glmark2-control-flow-specialization-contract.md)).
* Manual: the PS upload sub-types are only "Pixel Shader ALU Instruction",
  "TAU Instruction" and "Constant Register" (p.71), while the VS has an
  "Integer Register" store (p.72) that the PS lacks; A90-A9A carry counts
  and phase words, no branch state.

What the kernel PS compiler (`chrome9_fragment.c`) needs: nothing new.  The
native selection primitive is CMP (`result = cond >= 0 ? a : b`), already
`DRM_CHROME9_FRAGMENT_OP_CMP`; KILL is the only side effect.  Mesa keeps: NIR
unrolling (`max_unroll_iterations = 31`), uniform-bound loop specialization,
if-conversion to `bcsel`/CMP, inlining; anything left (data-dependent loops,
`discard` inside loops, early returns with side effects) goes to the llvmpipe
fragment fallback.  The 96-ALU store bounds unrolling.

## 6. Vertex-shader flow control

### 6.1 Evidence

* Windows caps modifier `win:0x100c6f70`, hardware-VS branch
  `win:0x100c70b9..0x100c70ed`: `VertexShaderVersion=fffe0200` (vs_2_0),
  `MaxVertexShaderConst=256`, `VS20Caps.Caps=0` (no predication),
  `DynamicFlowControlDepth=0`, `NumTemps=12`, `StaticFlowControlDepth=1`
  (`edi = 6 - 5`, `win:0x100c6fb9`, `0x100c701b`) (static).
* Native encodings (static + dynamic corpus, cross,
  [via-5.75-shader-re.md](via-5.75-shader-re.md),
  [chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md)):

  ```text
  REP i0 / LOOP aL,i0   00180154 00280400 00300400 00380000   op 28, src i0 (unified 96), dword2 = body length
  IF b0                 001c014c 00280400 00300800 003a0000   op 29, src b0 (unified 112), true +1, false +2
  ELSE                  0020a490 00280800 00000000 003a0000   op 29, literal-true source (unified 130)
  CALL l0               0020a490 00282000 00300400 003a4000   bit 14 = push
  CALLNZ l0,b0          001c014c 00282000 00300400 003a4000
  subroutine end        ....     ....     ....     0084010f   dword3 bit 23 = return
  ```

  Displacements are nine bits in `[18:10]` of dwords 1 and 2; the vendor
  postpass (`PVSCP_POSTPROCESS_H5`) patches them without range checks.  The
  final body instruction of IF and REP gets dword-3 bit 2 (`...0100` ->
  `...0104`), a strong end-marker candidate.
* Stores: `i0..i15` upload as `fe03043c 00000010 33030000 cc000000` + 16
  packed dwords (x,y,z,w low bytes); `b0..b15` are A61 `[15:0]`
  (`stmUpdateIntBoolConst_inv:0x17baa0`, cross with `win:0x101593b0`).  The
  kernel already emits the `33030000` group (`chrome9_engine.c`) and owns
  A61 (`chrome9_vertex.c`, `program->a61`).
* Both vendor compilers silently drop `ifc`, `breakc` and `break`; no dynamic
  branch encoding exists in any corpus.  The ELSE record branching on a
  literal-true source is the only hint that op 29 evaluates an ordinary
  source operand (inference): a temporary as the condition source might give
  data-dependent branching, but nothing proves it.

### 6.2 Kernel plan (CAP2 bit 8 `VERTEX_STATIC_FLOW`)

* Structured ops REP/ENDREP (source file INT), IF/ELSE/ENDIF (source file
  BOOL), draft values in `chrome9_rq_proposed.h`; `bool_constants` and
  `int_constants_ptr` in the vertex-draw raster extension (the 24 bytes of
  `reserved[3]`).
* Compiler: validate nesting (depth 1 first, the vendor claim; deeper only
  after F-rungs), emit op 28/29 records with computed displacements
  (reject > 511 records), set the end-marker bit, and make the physical temp
  allocator flow-aware (a temp live anywhere in a loop body stays live for the
  whole loop; SSA temps become redefinable only inside structured regions).
* Upload the 16 integer dwords and OR the Boolean mask into A61 `[15:0]`
  without disturbing bits 19/23.
* Loops multiply executed instructions; keep the command-timeout/quarantine
  path as the only runaway protection (REP count is at most 255 by the byte
  packing).

Mesa: map NIR loops with uniform trip counts to REP and uniform `if` to
IF b#, instead of per-value variants; keep unrolling for literal counts.
Dynamic VS branching stays in draw (software VS) unless F3 passes.

### 6.3 Rungs (`vsflow`)

| step | program | exact oracle |
|---|---|---|
| F1 | `r0=c0; IF b0; r0=c1; ENDIF; T0=r0`, b0 = 0 / 1 | red / green +-1 |
| F2 | `r0=(0,0,0,1); REP i0; r0.xyz+=1/8; ENDREP`, i0.x = 0, 3, 5 | `00`, `60`, `a0` +-1 in RGB |
| F3 | IF on temporary r1 = v1.x (RE knob only) | informative: follows v1 (dynamic works) or not |

## 7. Points, lines and polygon modes

The register-level recovery is complete in
[chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md)
(A20 reuse words, A23 primitive/edge/last-pixel/polygon-mode fields p.161,
A2A/A2B point size float s[8].15 (float bits >> 8) p.169, A01 bit 21 sprite p.91, A2F `[9:0]` per
coordinate sprite enables p.170, no line-width register, vendor wide lines by
replication).  Additional facts:

* D3D caps: `MaxPointSize=64.0` (offset `0xb0`), `FVFCaps` has `PSIZE`
  (`0x100000`), `LineCaps=1f` has no antialias bit.  The manual limit is
  2048; the vendor cap is a policy limit, not a register limit.
* Mesa today turns every point and line into triangles in draw
  (`draw_wide_point_threshold(0)`, `draw_wide_line_threshold(0)`,
  `draw_enable_point_sprites(true)`, `chrome9_context.c`), and the TCL gate
  falls back for non-FILL polygon modes (`chrome9_tcl.c`).  Correctness is
  already provided; native primitives remove CPU expansion and allow TCL for
  point/line draws.

Plan (topology field from the main session; POINT_LIST 1, LINE_LIST 2,
LINE_STRIP 3 are reserved for this work):

* Points (bit 9): A20 `20000000`, A23 `23001f00`-family with the draw's
  shading, A24 = N, A2A/A2B from `point_size` (or clamp values with a VS
  point-size output, destination `0x27`, A25 bit 19), A01 bit 21 for sprites
  and A2F `[9:0]` from the sprite mask; the viewport snap bias (rows 257/258,
  `49400000`) must be zero for point primitives
  (`dri:0x1bb6ee..0x1bb713`).  Mesa: stop forcing the wide-point stage when the
  cap is present; `sprite_coord_mode` LOWER_LEFT needs `t' = 1 - t` in the
  FS.
* Lines (bit 10): A23 `23004f01`, A20 `20000000`/`20206000`, A01 bit 22
  (diamond) and bit 23 (canonical direction) as in the live baseline, A23
  bit 15 = 0; FlatB for provoking-last flat lines (inference); stipple via
  A01 bit 0, A84/A85 and the A85 bit 23 counter reset per primitive.  Width >
  1 stays in draw's wide-line stage.
* Polygon mode (bit 11): A23 `[3:0]` 6 (wireframe) / 7 (point triangle),
  A23 bit 4 with `[6:5]` for differing windings, all three edge flags;
  per-vertex GL edge flags, polygon offset in line/point mode and stipple
  combinations stay on draw's unfilled stage.

Rungs (`points`, `lines`, `polymode`), TCL pass-through VS, FS `oC0 = T0`:

| step | geometry (GL window) | exact oracle |
|---|---|---|
| P1 | size-4 points at (16,16), (32,16), (16,42) | exactly the three 4x4 squares 14..17 / 30..33 / 40..43 |
| P2 | size-8 sprite at (32,32) | s and t rise by 1/8 per pixel, UL origin, +-2 |
| L1 | line (8.5,20.5)-(24.5,20.5) | exactly x=8..23 on row 20 |
| L2 | line (40.5,8.5)-(40.5,24.5) | exactly y=8..23 on column 40 |
| L3 | L1 reversed | 16 contiguous pixels on row 20 within 8..24 |
| M1 | triangle (8.5,8.5),(56.5,8.5),(8.5,56.5) LINE mode | interior (20,20) blank, all pixels within 1 px of an edge, edge pixels present |
| M2 | same, POINT mode | exactly pixels (8,8), (56,8), (8,56) |

## 8. Rung program

`~/tmp/agent-rq/chrome9_rq_rungs.c` (GPL-2.0-only) with the draft UAPI
`~/tmp/agent-rq/chrome9_rq_proposed.h`.  It builds on the host against the
current kernel UAPI header:

```sh
K=src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/include/uapi
gcc -std=gnu11 -O2 -Wall -Wextra -D__user= -I ~/tmp/agent-rq -I $K/drm \
    -o chrome9_rq_rungs ~/tmp/agent-rq/chrome9_rq_rungs.c -lm
```

Each rung checks its CAP2 bit and exits 77 when absent, so it can be copied to
the target before the kernel work lands.  One rung per run, at most eight
draws, each read guarded by `GEM_CPU_ACCESS(WAIT|PREPARE_READ)`:

```sh
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs occlusion
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs volume
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs points
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs lines
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs polymode
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs vsflow
```

A timeout, guard damage or engine quarantine is a failure of the rung and of
the capability; pixel mismatches disable only the new capability.

## 9. Open items

* A22 SF location bits for the path-A destination (unknown; avoided by the SL
  scratch page).
* Whether the counter counts with Z test disabled (O6) and whether stencil
  failures are excluded (add a stencil-fail step once O1-O5 pass).
* The volume-sampler TAU record and FE coordinate tuple: compile
  `dcl_volume s0; texld r0,t0,s0; mov oC0,r0` offline with the production
  wrapper before the kernel work.
* Silicon behaviour of op 29 on a temporary (F3), nesting beyond depth 1,
  and the call stack depth.
* PPC counter reset semantics (no reset field is documented; deltas of two
  snapshots are the safe interpretation).

## 10. Implementation patches: native points, lines and polygon modes

Status 2026-09-25: reviewable patches, not applied to the trees, built and
host-tested only; nothing below has run on VX900 silicon.

| file | content |
|---|---|
| `~/tmp/agent-pl/kernel-points-lines.patch` | kernel, `-p1` from `linux-7.2`, includes `include/uapi/drm/chrome9_drm.h` |
| `~/tmp/agent-pl/mesa-points-lines.patch` | Mesa, `-p1` from `mesa-26.2.2`, includes the `include/drm-uapi/chrome9_drm.h` sync |
| `~/tmp/agent-pl/chrome9_gl_points_lines_native_smoke.c` | GL smoke test, draw module against the native path |
| `~/tmp/agent-pl/tools-kraster-selftest.patch` | `tools/re/chrome9_kraster_selftest.sh`: extracts the new helpers and self-tests |
| `~/tmp/agent-pl/rq-rungs-uapi.patch` | `~/tmp/agent-rq` rung tool: `ext` overlay on the implemented fields (`point_size` onwards) |

Both patches pass `git apply --check` against the working trees of
2026-09-25 00:05 (they are based on the uncommitted cube/DXT work, not on
kernel `4f9c199` / Mesa `4e4d49d` alone).  The kernel copy builds with
`W=1` without warnings, the raster/topology and vertex self-tests pass on
the host (each new check was verified to fail on a mutated expectation),
and the Mesa files compile with the `objs-chrome9-llvm` commands without
diagnostics.

### 10.1 UAPI

* CAP2 bit 9 `NATIVE_POINTS`, 10 `NATIVE_LINES`, 11 `POLYGON_MODE`,
  advertised together with `VERTEX_TOPOLOGY` only when `hw_tcl` is set.
* `topology` 1 `POINT_LIST` (N ≥ 1), 2 `LINE_LIST` (even), 3 `LINE_STRIP`
  (≥ 2).  Points and lines reject `CULL_*`, `BFC_*` and polygon modes;
  `FLAT` gives FlatA for points and FlatB for lines.
* `reserved[3]` becomes `point_size`, `raster_flags`, `reserved[2]`, the
  layout of `drm_chrome9_vertex_raster_ext` in the rung tool (stipple and
  VS flow control keep the remaining 16 bytes).  `point_size` is float bits
  in [1.0, 2048.0], zero for 1.0, and must be zero unless the draw
  rasterizes points.
* `raster_flags`: bit 0 `POINT_SPRITE` with `SPRITE_MASK` bits 8..15
  (logical varyings), bit 2 `POINT_SIZE_VERTEX`, bits 16..17 / 18..19
  polygon mode of CW / CCW screen windings (FILL 0, LINE 1, POINT 2).
  Bit 1 stays reserved for line stipple.  A POINT face requires POINT on
  both windings, because the vendor decides the snap bias from A23[3:0]
  alone (the CCW mode when A23 bit 4 is set).
* Vertex destination 6 `POINT_SIZE` (index 0, writemask X, no SAT).  It is
  used only with `POINT_SIZE_VERTEX` (then written exactly once) and
  dropped otherwise, so Mesa emits it for every VS that writes PSIZ once the
  capability is enabled.

### 10.2 Kernel words

| state | words |
|---|---|
| point list | A20 `20000000`, A23 `23001f00` (FlatA), A24 = N |
| line list / strip | A20 `20000000` / `20206000`, A23 `23004f01`, flat `23002f01` |
| polygon LINE / POINT | A23[3:0] = 6 / 7; mixed FILL/LINE: bit 4, [3:0] CCW, [6:5] = `10` for a CW wireframe |
| point size | A2A = `2a000000 | size >> 8`, A2B the same or `2b3f8000` (1.0) with `POINT_SIZE_VERTEX`; after the A25..A29 link words |
| VS point size | record dest `0x27`, mask `f`, lane X broadcast (`0008027f`), A25 bit 19 |
| sprite | A01 bit 21, A2F = `2f582000` (vendor defaults) with bit n per packed coordinate n; replaced varyings get a zero MOV in the VS |
| snap bias | point list or point-mode triangles: rows 258.xy = 0 and rows 257.xy = ox/oy − 786432, computed exactly in integer arithmetic (a validated ox has 1/16-pixel mantissa steps); A40..A48 follow rows 257 |

The format correction: "Floating s[8].15" is a 24-bit float, vendor
`INV_FloatToP_E8_N(f, 15)` = `(bits & 0x7fffffff) >> 8` (`dri:0x1a7130`),
so 1.0 is `2a3f8000`, not the `2a008000` of the earlier word list (fixed in
[chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md)
§3.5).

RE knobs (`tcl_re`): 256 omits the A2F write (ladder L-S1), 512 clears
A01[23] for lines and wireframe so the hardware draws from vertex a to b.

### 10.3 Mesa

Native use happens only in `chrome9_tcl.c` and only when GL is met
exactly; otherwise the draw module path is unchanged:

* points: `point_smooth`, GLES `point_line_tri_clip` and
  `!half_pixel_center` fall back; legacy points round the state size
  (minimum 1), sprites need a whole size; per-vertex size needs
  `CHROME9_TCL_POINT_SIZE` and a VS PSIZ write and uses the screen
  `max_point_size` (255) as clamp; sprite coordinates are computed from
  `sprite_coord_enable` (TEXn = generic n, VARn = generic 9 + n), need the
  PIPE upper-left origin (window-system framebuffers with the GL default;
  FBOs fall back), and a shader reading `gl_PointCoord` or both TEXn and
  VARn of a replaced varying falls back.  Sprite lanes count as
  (s, t, 0, 1) for the guarded fragment domains.
* lines: width below 1.5 (GL rounds to one), no smooth, stipple,
  rectangular or last-pixel lines, flat only with the last-vertex
  convention.  `GL_LINE_LOOP` is a line strip closing on vertex 0; long
  strips are chunked with a one-vertex overlap (host-tested for every
  chunk size 2..9 and 2..40 vertices), so the provoking vertex of every
  segment is unchanged.
* polygon modes: triangles, strips and fans only (quads and polygons hide
  inner edges), no VS edge flags, no flat unfilled polygons, no
  `offset_line`/`offset_point`; the mode of a culled face is ignored; a
  POINT face needs POINT on the other visible face and the point gates
  (no sprites, no per-vertex size).
* points and lines take no cull or back-colour state (GL treats them as
  front-facing).

Environment switches, all default off until the rungs pass:

| switch | enable after |
|---|---|
| `CHROME9_TCL_POINTS` | rung `points` P1 and P2; ladder L-P1..L-P3 (phase of sizes 1, 2, 3, 4 at quarter offsets), L-S1..L-S3; smoke `points*`, `sprites upper-left` exact |
| `CHROME9_TCL_POINT_SIZE` | ladder L-P4 (not in the rung tool yet); smoke `points vertex size` |
| `CHROME9_TCL_LINES` | rung `lines` L1..L3; ladder L-L3 (strip shared pixel), L-L4 (FlatB); smoke `lines*`, `line strip*`, `line loop*` |
| `CHROME9_TCL_POLYGON_MODE` | rung `polymode` M1, M2; smoke `polygon*` |

### 10.4 GL rasterization rules the silicon must confirm

* Lines: A01[22] selects the diamond rule; A01[23], set in every current
  A01 word, draws left-to-right / top-to-bottom regardless of vertex
  order.  That moves the included end pixel of reversed lines by one, which
  GL 4.6 §14.5.1 permits (at most one fragment difference, no gap or
  duplicate between segments running the same way), but it is not the
  exact diamond-exit result the draw module produces.  Rung L3 accepts
  either; knob 512 shows whether a→b order gives exact diamond exit.
* Wireframe corners: with A01[23] a vertex may be the excluded end of both
  adjoining edges (allowed by the same rule, since the edges run in
  opposite directions); rung M1 only checks proximity.
* Point centre: GL puts an odd-size point on the pixel containing the
  vertex and an even-size point on the nearest pixel corner.  Whether the
  hardware snaps the centre that way without the bias is unmeasured (L-P2);
  fractional sizes (sprites, per-vertex sizes) are not accepted until the
  rounding of A2A and of the vertex size is known.
* Clipping: GL discards a point whose centre is outside the volume; the
  clip engine behaviour for points and the edges it introduces into
  wireframe triangles crossing a plane are unmeasured.  User clip
  distances on points and lines are per-pixel (FE A05), which equals the
  vertex clip for points and differs from geometric line clipping by less
  than a pixel.
* FlatB = GL provoking-last for lines is an inference from FlatC for
  triangles.

### 10.5 Open questions

* Whether A12 (VS output mask) needs a bit for the point-size output in
  addition to A25[19].
* A2F[9:0] versus A01[21] alone (L-S1/L-S2), sprite coordinates narrower
  than four lanes (the vendor forces 4D coordinates for sprite points),
  and whether polygon-mode POINT triangles honour A2A/A2B and sprites.
* The existing TCL path ignores `offset_tri` (the draw module applies
  polygon offset); only the new line and point forms fall back on offset.
* The rung tool warns on `draw_volume`'s texture `reserved` initializer
  against the current header (texture UAPI change outside this work).

### 10.6 First silicon smoke (kernel `4f6eeda`, Mesa `3cfd219`)

Run with `CHROME9_TCL_POINTS=1 CHROME9_TCL_POINT_SIZE=1
CHROME9_TCL_POLYGON_MODE=1`.  Points of size 1, 2, 3, 4, 8, `points 1000`
(arrays and elements, flat) matched exactly.  The failures, checked
against GL 2.1 and the source (host llvmpipe under Xvfb serves as an
independent GL implementation for points and sprites; its lit counts are
given in brackets):

| case | reported native / ref | GL 2.1 | verdict |
|---|---|---|---|
| points size 2.4 | 360 / 160 | 160, §3.4.1 rounds the aliased width to 2 (llvmpipe 160) | see below; the code predicts native 160, ref 360 |
| points at the edges | 48 / 78 | 48, §2.12 discards a point whose vertex is outside the clip volume (llvmpipe 78) | native correct, smoke expectation wrong |
| sprites upper-left | 0 / 768 | 768 (llvmpipe 768, exact texels) | native broken, cause open (rung `sprite`) |
| points vertex size | 1000 / 40 | 1000, §2.15.3 takes `gl_PointSize` (5) with `VERTEX_PROGRAM_POINT_SIZE` on (llvmpipe 1000) | native correct, draw module wrong |
| polygon line strip | 412 / 354 | per-polygon edges, §3.5.4 (llvmpipe line loops 354) | within §3.4.1 rule 1, smoke tolerance wrong |
| polygon line fan | 430 / 398 | as above (llvmpipe 398) | as above |
| polygon front point back line | 145 / 286 | 3 points + CW outline (llvmpipe equivalent 146) | native correct; both runs used the draw module |

**Size 2.4.**  `tcl_raster_points()` sends `roundf(2.4) = 2.0` for
legacy points (the st sets `point_quad_rasterization` only with
`GL_POINT_SPRITE`; `point_size_fixed` is `LOWER_NEVER`, so the FF VS writes
no PSIZ and `point_size_per_vertex` stays false).  The kernel words are
then those of `points size 2` (`2a400000 2b400000`), which matched
exactly.  The draw module does not round: `widepoint_point()` uses half
size 1.2 with its `+1/8, -1/8` bias, so the point at (4.25, 123.75) spans
x [3.175, 5.575] and y [122.425, 124.825] = 3x3, 360 for 40 points.  The
smoke prints `lit a/b` with a = native, so native 160 / ref 360 is the
reading that fits the code; native 360 would need a Mesa build without the
rounding.  Rung `psize` measures A2A 2.4 directly and the fixed smoke now
prints `lit native N ref M`.  The VS size had no rounding at all: Mesa now
stores `floor(size + 0.5)` (§3.4.1 applies to the derived size too), and
per-vertex sizes with point sprites (not rounded by GL) fall back.

**Edges.**  Size 6: points 1 and 2 have the centre inside and 4x6 visible
pixels each; points 3 (60.25, -1.25), 4 (60.25, 129.25), 5 (-2.25, 30.25)
have the vertex outside.  GL: 24 + 24 = 48 = native.  The draw module
(Gallium `point_line_tri_clip = false`, guard band) draws the visible part
of the outside points: 12 + 12 + 6 = 30 more, 78.  So the VX900 clip engine
discards points by the vertex and clips the square to the window; nothing
to change.

**Vertex size.**  `gl_PointSize = 1 + floor(mod(x, 6))` is 5 for every
grid x (4.25 + 12k), 25 px each.  The reference drew size 1, i.e. the
wide-point stage used the state size instead of the PSIZ output in the
chrome9 draw fallback; llvmpipe draws 1000 from the same program.  Open
(draw path, not the native path).

**Sprites.**  Zero lit pixels out of 768 means nothing was written or
every texel lookup returned black.  Without replacement the kernel's
former zero fill gives (0, 0, 0, 0); the fixed-function texenv looks up
projectively (`nir_tex_src_projector`, q), so a real divide sees 0/0,
while a fragment variant relying on q = 1 (Mesa accepts the guarded
identity projector for sprite lanes) samples texel (0, 0), which is not
black.  Candidates:
(a) the kernel rejected the queued draw (batched submissions have no
fallback; `chrome9_tcl_batch_flush()` prints `DRAW_VERTEX_PROGRAMS of N
failed` once), (b) A01[21] with the A2F word produces no pixels, (c) s/t
replaced but q from the VS (0).  Rung `sprite` separates (b) and (c) from
a correct replacement; a smoke run with `CHROME9_TCL_BATCH=0` turns (a)
into a draw-module fallback with a `tcl_dump`.  The kernel now fills a
replaced coordinate with (0, 0, 0, 1), which makes (c) correct.

**Wireframe strips and fans** (explanation withdrawn in 10.8: rung `wire`
shows direction-independent wireframe; the cause was a stale rasterizer).  The extra pixels are +58 for the strip and
+32 for the fan.  The strip shares two 30x50 y-major diagonals (v1v2,
v3v4) and one vertical edge (v2v3); the fan shares one such diagonal
(v0v3) and two horizontal edges (v0v2, v0v4).  About 29 extra pixels per
shared diagonal and none per axis-aligned shared edge fit one mechanism:
the hardware draws each wireframe edge in the triangle's vertex order
(a->b, b->c, c->a; adjacent triangles of one winding run a shared edge in
opposite directions) without the A01[23] direction normalisation that
native lines get, so the two copies of a non-45-degree edge pick different
pixels in about 58% of the rows.  The list cases have only 45-degree,
unshared edges and cannot show it.  GL draws every polygon's edges
(§3.5.4) and lets each segment's fragments deviate by one unit from
diamond exit (§3.4.1 rule 1), so this is conformant; removing the second
copy through A23[11:9] edge flags would change blended results (GL blends
a shared edge twice) and is not done.  Rung `wire` checks the mechanism
and that strips equal lists.

**Mixed point/line.**  Mesa gates front POINT with back LINE (A23 decides
the snap bias from one nibble), so both runs drew this case with the draw
module unfilled stage; the dump run produced both faces as lines (286 =
2 x 143), the compare run the correct 145.  The draw-module result
depends on the preceding draws; open.  The smoke reference no longer uses
the unfilled stage.

**Smoke reference.**  `CHROME9_TCL_LINES` defaults to on since `3cfd219`,
so a dump without `CHROME9_TCL_LINES=0` compared native lines with native
lines.

### 10.7 Fixes and rungs (2026-09-25, `~/tmp/agent-pl2`)

| file | content |
|---|---|
| `kernel.patch` | `-p1` from `linux-7.2` on `272da90`: sprite-replaced coordinates default to (0, 0, 0, 1) (literal `00206db0`), self-test record updated |
| `mesa.patch` | `-p1` from `mesa-26.2.2` on `7f2163a`: VS point size stored as `floor(size + 0.5)`; per-vertex size with point sprites falls back |
| `tools-smoke.patch` | `tools/re/chrome9_gl_points_lines_native_smoke.c` (applied): points and sprites checked against GL 2.1 computed in the test, polygon-mode reference drawn as GL equivalents, shared-edge allowance, dump refuses native lines/points/polygon modes, `lit native N ref M` |
| `chrome9_rq_rungs.c` | rung tool with `psize`, `pedge`, `sprite`, `wire` (`rq-rungs.patch` against `~/tmp/agent-rq`) |

Host verification: kernel copy `W=1` warning-free, raster/FE self-test and
vertex self-test pass (the new sprite record fails the self-test when the
old zero literal is emitted); Mesa files compile without diagnostics; a
host program compiling a PSIZ-writing VS through `chrome9_tcl_compile()`
gets ADD 0.5 / FLR / MOV POINT_SIZE (old code: MOV only).  The smoke on
host llvmpipe passes every point, sprite and polygon case except the edge
case (llvmpipe draws the guard-band 78).

Rungs, each one bounded run (`flock -w 5 -x /tmp/chrome9-vx900-gpu.lock
timeout -k 2s 7s ./chrome9_rq_rungs <rung>`); expected output where the
hardware follows GL:

* `psize`: A2A and VS sizes 1, 2, 3, 4, 2.4, 2.5, 2.6, 3.4 at phases
  (20.25, 20.75) and (40.75, 40.25).  Whole sizes must print `GL`
  (fails otherwise): size 1 at (20,20)/(40,40), 2 at (19,20)/(40,39),
  3 at (19,19)/(39,39), 4 at (18,19)/(39,38).  Fractional sizes print
  `GL` (2.4 as 2x2, 2.5/2.6/3.4 as 3x3), `ceil`, `truncate` or
  `shifted`: this is the A2A / A25[19] rounding the Mesa change avoids.
  All-GL for 2.4 confirms that native `points size 2.4` must be 160.
* `pedge`: `P6 point A: 24`, `B: 0`, `C: 0`, `D: 24`, `total 48: GL centre
  clip` (66 guard band, 0 crossing squares discarded).
* `sprite`: `P3 T0 sprite: drawn 64/64, s,t gradient 64`, pixel (30, 31)
  about `ff507000`, `s,t replaced (upper-left), r=0 q=1`; `P3b` (sprite on
  packed coordinate 1 plus an ordinary T0 = 0.25) (28,28) about
  `ff505040`, (35,35) about `ffffff40`.  `no pixels` = hypothesis (b),
  `not replaced` = no replacement, `r/q from the VS value` = (c) on the old
  kernel.  Repeat with `tcl_re=256` (no A2F write): a difference decides
  the A2F[9:0] contract (L-S1/L-S2).
* `wire`: lines `A->B` and `B->A` 0 off diamond exit; wireframe rows
  0 beyond one unit; `M3 T1/T2 shared edge differs in K of 27 rows` with
  K > 0 (about 16 from the smoke counts) under the direction hypothesis, 0
  if wireframe is direction independent; `M4 strip vs list: 0 pixels
  differ` (fails if strips differ from lists).  Repeat with `tcl_re=512`
  (A01[23] clear): if the line rows move and K stays, A01[23] does not
  reach wireframe.

### 10.8 Second silicon run and fixes (2026-09-25, `~/tmp/agent-pl3`)

Module k59 (kernel `d83bd67` + 10.7 kernel patch + occlusion v3), Mesa
`2ebf75c` + 10.7 Mesa patch, rung tool and smoke of 10.7:

| rung / case | result | conclusion |
|---|---|---|
| `psize` A2A | all whole sizes GL, 3.4 as 3x3 | A2A rounds to nearest; `points size 2` words are right |
| `psize` VS | size 1 GL, size 2.0 draws 1 px (the rung stopped at the first miss) | VS point size not delivered (from an input lane); open |
| `pedge` | A 24, B 6, C 12, D 24 = 66 | the clip engine guard-bands points: centre-outside points draw their visible part, not GL |
| `sprite` P3 | 64/64, s,t gradient, `ff507000`, r = 0, q = 1 | sprite coordinates correct on T0 |
| `sprite` P3b | T0 + T1 = `ff202000` at (28,28), same with `tcl_re=256` | A01[21] replaces **every** coordinate; A2F[9:0] selects nothing |
| `wire` | 0 of 27 rows differ, strip = list, also with `tcl_re=512` | wireframe is direction independent: the 10.6 shared-edge explanation is wrong |
| smoke `polygon line` | native 2352 (filled), ref 284 | 284 = two outlines (llvmpipe 284) is right; native used a stale FILL state |
| smoke `front point back line` | native 286, ref 145 | same stale state (both through the draw module) |
| smoke default path | `2.4` 360, sprites 0 | draw module: unrounded size with 1/8 bias; sprites see below |

**Stale rasterizer.**  `chrome9_bind_rasterizer_state()` set
`ctx->rasterizer = state` and then called `draw_set_rasterizer_state()`.
That flushes the draw pipeline; `widepoint_flush()` and `wideline_flush()`
restore `draw->rast_handle` (still the previous state) through
`pipe->bind_rasterizer_state()` with flushing suspended, which lands in
the same function and sets `ctx->rasterizer` back to the previous state.
With the wide thresholds at 0 both stages are in every pipeline, so after
any draw-module point or line draw every hardware path (TCL raster,
cull, polygon mode, scissor, llvmpipe fallback) used the rasterizer of
the preceding state change.  This explains the filled native wireframe,
the swapped mixed modes, the strip/fan counts of the first run and the
run-to-run differences of the draw-module results.  Fix: take the state
after `draw_set_rasterizer_state()`.  The draw-module sprite result (0 of
768) is re-measured with the fix; if it stays, `CHROME9_DEBUG_REJECT=1`
names the rejecting site.

**Fixes** (`kernel.patch` on `38e43f1`, `mesa.patch` on the Mesa root
plus the chrome9 driver `b790e39`, both `git apply --check` clean against
the working trees):

* Mesa: rasterizer bind order (above).
* draw: `draw_wide_point_gl()`; aliased wide points round the size to an
  integer (at least 1), centre odd sizes on the pixel holding the vertex
  and even sizes on the nearest corner, without the 1/8 bias; sprites are
  the exact square.  Square edges fall on integers, so the triangle fill
  rule reproduces GL 2.1 3.4.1 exactly.  chrome9 enables it; other drivers
  keep the old behaviour.
* Mesa TCL: native point lists compile a variant (`CHROME9_TCL_POINT_CLIP`)
  that moves a point with `max(|x|, |y|) > w` to `z - 2|w| - 1` after the
  half-z lowering, behind the near plane; `pedge` P6b checks that the clip
  engine drops such points.  If it does not, the per-pixel UCP test
  (FE A05) with a distance `w - max(|x|, |y|)` is the alternative.
* Mesa TCL: native sprites only when every coordinate the fragment
  shader reads is replaced and no clip plane is on; native polygon
  point mode falls back (GL draws the vertices of the clipped polygon).
* Kernel: sprite draws must replace every fragment varying and use no
  clip distance (`-EINVAL`); vertex self-test covers T0+T1 and UCP
  (fails when the check is removed).
* Smoke: the wireframe allowance is one per point or edge again (the
  shared-edge allowance is withdrawn after `wire`).

Host verification: kernel `W=1` clean, raster/FE and vertex self-tests
pass; Mesa `chrome9_tcl.c`, `chrome9_context.c`, `chrome9_draw.c`,
`chrome9_tcl_flow_test.c` and the draw files compile without
diagnostics; a host program through `chrome9_tcl_compile()` shows the
clip variant (ABS, MAX, SLT) and the PSIZ rounding in all four
combinations; the smoke on llvmpipe passes except the edge case.

**VS point size.**  The vendor path matches the kernel words
(`stmUpdateFVFOut_H5i` sets A25[19] from object `+4` bit 5, A28 only
takes bits 18:17; `STM_PointSize_H5` puts max in A2A, min in A2B;
`stmUpdateVS_inv` builds A60..A62 without a point-size bit).  `psize` now
reports every size and three sources (input lane, temporary, constant)
without stopping; `CHROME9_TCL_POINT_SIZE` stays off, per-vertex sizes use
the draw module.

Expected with the fixes (`flock -w 5 -x /tmp/chrome9-vx900-gpu.lock
timeout -k 2s 7s ./chrome9_rq_rungs <rung>`):

* `pedge`: `P6 raw ... total 66: guard band`, `P6b behind near: A 24 B 0 C
  0 D 24 E 0 ... near clip drops points` (pass), `P6c` informative.
* `sprite`: P3 as before, `P3b ... rejected (EINVAL)`, `P3c` (28,28) about
  `ff202000`, (35,35) about `ffffff00`.
* `psize`: `a2a` 0 of 8 wrong; the three `vs` lines tell whether the size
  is lost for every source (`size 1 (VS size lost)`) or only for inputs.
* smoke (dump `CHROME9_TCL=1 CHROME9_TCL_LINES=0`, compare with
  `CHROME9_TCL_POINTS=1 CHROME9_TCL_POLYGON_MODE=1`): all point cases
  `gl`-exact, `polygon line` near 284, `front point back line` near 145.

### 10.9 Draw-module sprites with a texture (2026-09-25)

`tools/re/chrome9_gl_sprite_smoke.c`: a textured sprite drew nothing on
the draw-module path (no reject), untextured sprites and textured points
without sprites were correct.  Cause: `draw_create_fragment_shader()`
scans NIR fragment shaders with `need_texcoord = true`, so TEX0 is
`TEXCOORD 0`, while `nir_to_tgsi()` names the vertex outputs and the
wide-point stage matches `sprite_coord_enable` with the screen's
`tgsi_texcoord` (false on chrome9: TEXn is `GENERIC n`, VARn
`GENERIC 9 + n`, the st numbering).  `widepoint_first_point()` therefore
found no input to replace and generated no coordinates, and the
fixed-function vertex program writes no TEXn for a replaced unit
(`ffvertex_prog.c` `coord_replace`).  The coordinate went out as zero,
q = 0, and the projective lookup gave black.  Fix
(`~/tmp/agent-pl4/mesa-sprite.patch`, Mesa root): scan with the screen's
`tgsi_texcoord`.  Drivers with `tgsi_texcoord` set see no change.  The
smoke's GLSL sub-test lacked `#version 120` (gl_PointCoord is GLSL 1.20),
now checks the link status and each channel; llvmpipe and softpipe pass
every sub-test.

## 11. Implementation patches: occlusion queries and 3D textures

Status 2026-09-25: reviewable patches, not applied to the trees, built and
host-tested only; nothing below has run on VX900 silicon.

| file | content |
|---|---|
| `~/tmp/agent-oq/kernel-occlusion-3d.patch` | kernel, `-p1` from `linux-7.2`, includes `include/uapi/drm/chrome9_drm.h` |
| `~/tmp/agent-oq/mesa-occlusion-3d.patch` | Mesa, `-p1` from `mesa-26.2.2`, includes the `include/drm-uapi/chrome9_drm.h` sync |
| `~/tmp/agent-oq/chrome9_gl_occlusion_smoke.c` | GL smoke test O1..O9 (exact counts against pixel counts) |
| `~/tmp/agent-oq/chrome9_gl_tex3d_smoke.c` | GL smoke test V1..V7 (native and llvmpipe 3D cases) |
| `~/tmp/agent-oq/rq-rungs-occlusion-3d.patch` | `~/tmp/agent-rq` rung tool: `occlusion` and `volume` on the implemented UAPI |
| `~/tmp/agent-oq/scripts/` | `rebase.sh` (snapshot live trees, re-apply), `verify.sh` (build, self-tests with mutations, Mesa compile and tests) |
| `~/tmp/agent-oq/{kbase,knew,mbase,mnew}/` | base and patched copies |

The rung tool patch also resolves the `draw_volume` `reserved` warning
of §10.5: both rungs now require the implemented header and skip otherwise.

Merge order: cube/DXT (kernel `4b06bc2`, Mesa `58b8d2f`), points/lines
(`4f6eeda`, `3cfd219`), Mesa `d4eb87d` (render to cube faces and mip
levels), then these patches, which are based on exactly
those commits and pass `git apply --check` against the working trees.
They compose with the cube work instead of adding a parallel mechanism:

* the texture state `target` field gets `DRM_CHROME9_FRAGMENT_TARGET_3D`
  (2, after CUBE 1); `face_stride` is the slice stride of a volume;
  cube's `reserved[2]` becomes `__u16 depth`, `__u16 wrap_r`,
  `reserved[1]` (size stays 176);
* `chrome9_fragment_compile_targets()`'s `cube_mask` becomes `str_mask`
  (cube and 3D samplers both read S, T, R; same TAU record);
* CAP2 bits 6 `OCCLUSION_QUERY` and 7 `TEXTURE_3D` sit between
  `VERTEX_TOPOLOGY` (5) and `NATIVE_POINTS` (9).

Both features are module-parameter gated and default off:
`occlusion_query=0` (1 = vendor snapshot, 2 = snapshot after the tail's
`6c 00800000` sync) and `texture_3d=0`.  With the defaults every packet is
byte-identical to the base.

### 11.1 UAPI

| item | value |
|---|---|
| `DRM_IOCTL_CHROME9_QUERY` | `0x22`, `struct drm_chrome9_query` (48 bytes): `op`, `flags`, `id`, `type`, `result`, `available`, `pad`, `reserved[2]` |
| ops | `CREATE` 0 (type `OCCLUSION`, returns `id`), `DESTROY` 1, `BEGIN` 2, `END` 3, `READ` 4 (`flags` `READ_WAIT`) |
| `DRM_CHROME9_FRAGMENT_QUERY` | fragment flag bit 4; `draw_fragment_program.query` names an active query (old `reserved[0]` low half; `reserved1`, `reserved[1]` stay zero) |
| `DRM_CHROME9_FRAGMENT_TARGET_3D` | 2; POT width/height/depth ≤ 2048, `first_level = last_level = 0`, `face_stride = level_pitch[0] * height`, `wrap_r` CLAMP_TO_EDGE/REPEAT/MIRRORED |

BEGIN zeroes the 64-bit accumulator and starts a new interval (a
generation number): snapshots of draws submitted in an older interval are
ignored.  READ fails with `-EBUSY` while the query is active, returns
`available = 1` once every attached draw retired, and `-EIO` if an attached
draw was lost (engine timeout, damaged snapshot) until the next BEGIN.  The
vertex-program ioctls carry the query in their embedded fragment state, per
draw.

### 11.2 Kernel: occlusion queries

* `chrome9_query.c`: per-file `xarray` (`drm_driver.open/postclose`), at
  most 16384 queries per file, `kref`-counted objects with a spinlock.  A
  lookup takes its reference under the xarray lock, so DESTROY never frees
  an object a draw or job still uses; jobs hold their own references until
  they are reaped, a closed file leaves orphans that only its jobs update.
* Snapshot storage: one kernel-owned SL page, `vram_size - 64 KiB -
  PAGE_SIZE`, directly below the GART; `CHROME9_ENGINE_VRAM_RESERVED`
  also keeps it out of the lab scanout carveout.  256 slots of 16 bytes
  (bitmap sized from the page): result dword poisoned `ffffffff`, three
  canaries `0cc10001..3`, written through BAR2 and read back before the
  ring can execute.  The GPU never writes user memory, and the address in
  the packet always comes from the kernel's slot index.
* Emission (`chrome9_emit_fragment_draw`): a query draw is bracketed by the
  vendor reset (type-01 `1C000000 1D000000`) before its state and by the
  path-A snapshot after its vertices or indices (type-10 `02000000
  0300011C`, then `21000000 | Q[23:0]`, `22000000 | Q[31:24]`).  The
  reservation is part of the draw's `-E2BIG` bound, so a draw that moves
  to the next segment keeps its slot.  Selftest: packet words for both
  modes.
* Attachment and retirement: slots are reserved and poisoned under
  `submit_lock` before any append; the draws of a segment are attached to
  their queries (fence of the job) right before that segment is appended,
  only if the query is still active in the same interval.  A segment that
  retires synchronously adds its snapshots at once; the last segment adds
  them in `chrome9_fragment_job_finish_locked()`, i.e. in the retire worker
  or the next `submit_lock` holder, before the job fence signals, so READ
  after the fence wait always sees the samples.  A poisoned result or a
  damaged canary gives `-EIO` and a rate-limited dump.  A pre-publication
  failure drops the slots of the unexecuted draws and marks queries they
  were already attached to `-EIO`; a quarantined job marks its attached
  queries `-EIO` and never returns its slots (the engine may still write
  them).  The entry array is allocated before `submit_lock` is taken.
* READ+WAIT waits on the last attached fence, interruptible, at most 5 s.

### 11.3 Kernel: 3D textures

Validation (`chrome9_validate_fragment_texture`, needs `CAP2_TEXTURE_3D`):
uncompressed, not X8Z24, POT width, height and depth (≤ 2048), level zero
only (hence no mip filter and zero LOD range), `face_stride` exactly the
level-zero image size, object size `ALIGN(L0 + depth * face_stride, PAGE)
+ PAGE`.  2D and cube states must keep `depth` and `wrap_r` zero.
Emission: A18 `18001201`, A21 = depth, A22 `[11:8]` = log2 depth next to
the POT flag and log2 height/width, A32 `[8:6]` R wrap and `[21:19]` /
`[18:16]` R filters from mag/min (2D keeps `0x40`, R clamp).  With
`cube_map` enabled the A01..A05 closure is written for volumes as for 2D.
Selftest: the 4x4x4 BGRA vector of §4.1 (`18001201 20004004 21000004
22008222`, sampler `31102490 32090089`) and seven rejections (NPOT depth,
wrong slice stride, zero `wrap_r`, NPOT width, depth 4096, 2D with depth,
2D with `wrap_r`), plus the A01..A05 closure form emitted when
`cube_map` is on.  The slice stride is still the inference of §4.2; rung
V1 slice 1 decides it.

### 11.4 Mesa: queries

`chrome9_query.c` implements `PIPE_QUERY_OCCLUSION_COUNTER`,
`OCCLUSION_PREDICATE` and `OCCLUSION_PREDICATE_CONSERVATIVE` (caps
`occlusion_query`, `conditional_render`, `conditional_render_inverted`
with CAP2 bit 6).

* Counting intervals: all active queries of a context share one kernel
  query per interval.  Every BEGIN, END, DESTROY of an active query and
  `set_active_query_state()` (u_blitter) flushes the draw module and the
  queued GPU batches, ENDs the interval and opens a new one if any query is
  still active and counting is not paused; each active query references
  the intervals it spans and sums them.  This is what makes GL's
  simultaneously active `GL_SAMPLES_PASSED` and `GL_ANY_SAMPLES_PASSED`
  work with one tag per draw.  Kernel ids are recycled once an interval is
  resolved or unreferenced.
* Hardware draws: the generic fragment-program state carries the tag, and
  while counting the post-TNL path uses it for every generic-capable shader
  (including the pass-through program that normally takes the fixed
  immediate path; the textured COPY_RECT shortcut is thereby off).  Hardware
  TCL draws carry it through the same state builder.  Clears are never
  tagged (GL does not count them).
* Software draws: a counting draw whose fragment program is not generic
  goes to llvmpipe, and every llvmpipe fallback draw of an interval runs
  with an llvmpipe `OCCLUSION_COUNTER` query of that interval, resolved with
  the kernel part.  The result is therefore exact across both engines.
* Conformance choice: exact counts, no approximations.  Only when a
  counting draw can be counted by neither engine (llvmpipe disabled with
  `CHROME9_SW_FALLBACK=0`, or its refusal of cross-engine depth EQUAL) or
  the kernel reports `-EIO` is the interval marked inexact: predicates
  then answer true (rendering is not skipped, the conservative answer;
  strictly only `ANY_SAMPLES_PASSED_CONSERVATIVE` allows it), counters
  return the counted part, and a one-time warning is printed.  Every
  rejected triangle and every failed tagged submission (the TCL batch
  retries draw by draw after a failure, so segments may count twice) also
  marks the interval inexact.  This covers the one behavioural side effect
  of counting: a pass-through draw forced onto the generic path that the
  generic state gate refuses is dropped, and its query reports it.
* Conditional rendering is software, as in softpipe: `draw_vbo`, `clear`
  and the render-condition-enabled clears evaluate the query (waiting in
  the WAIT modes, drawing when a NO_WAIT result is unavailable).

### 11.5 Mesa: 3D textures

* Resources: `PIPE_TEXTURE_3D`, sampler formats of 2D minus DXT/depth,
  RENDER_TARGET accepted like cube maps; a slice (level, layer) renders
  through the same guarded 2D shadow as cube faces and mip levels
  (`d4eb87d`), `chrome9_subimage()` now steps 3D slices by the level's
  image size.
  Layout: after the leading guard page each level's slices are contiguous
  at the level's image size (`ALIGN(w * cpp, 32) * h`, the vendor
  `sfm3DTextureBlt` stride), levels follow each other, trailing guard.
  Transfers address `box.z` slices; mipmaps are generated on the CPU with a
  2x2x2 box filter (`util_gen_mipmap` would need `pipe->blit`).
* `max_texture_3d_levels = 12` with `CAP2_TEXTURE_3D` or with the llvmpipe
  fallback available, so GL 1.2 3D textures always work.
* Native sampling (`chrome9_draw_needs_sw_textures()` false): CAP2 bit 7,
  generic fragment program, the shader samples the unit as `sampler3D`
  without projection, POT sizes up to 2048, native format and swizzle,
  a single-level resource (the kernel sizes a volume object from level
  zero alone, so a mipmapped volume is refused even for a base-level view)
  and view; the texture state describes level 0 (`mip_filter` NONE, LOD
  0).  Everything else (NPOT, mipmapped volumes, `CLAMP_TO_BORDER` in R)
  runs the whole draw on llvmpipe, which gets a copy of all levels for
  each fallback draw.
* NIR: a `GLSL_SAMPLER_DIM_3D` TEX lowers like a cube TEX (three lanes,
  XYZ of a varying or a computed TEMP) and sets
  `semantic_volume_sampler_mask`; the draw checks that the mask matches the
  bound view's target.
* Fixed on the way: the llvmpipe copy of a single-level cube or
  compressed texture leaked one reference per fallback draw.

### 11.6 Verification and enabling

Host: the kernel copy builds with `W=1` without diagnostics;
`chrome9_fragment_texture_selftest()` (with the 3D vector),
`chrome9_occlusion_selftest()` (both modes) and `chrome9_fragment_selftest()`
pass in the host harness, and four mutated expectations fail.  All 31
chrome9 compile units of `objs-chrome9-llvm` compile from the copy without
diagnostics; `chrome9-fragment-test` and `chrome9-format-test` pass
(`chrome9-shader-nir-test` and `chrome9-mipmap-test` do not link in this
environment, as before).  Both GL smoke tests were run against the host's
Mesa llvmpipe under Xvfb to validate their expected values (16/16 and
15/15); that says nothing about the chrome9 driver.  The rung tool patch
builds warning-free against the old and the new header.

Silicon order (one run each, `flock`, `timeout 7`):

| step | load | must show | then |
|---|---|---|---|
| 1 | `occlusion_query=1` | rung `occlusion` O1..O5 exact | O5 low but O1 right: retry with `occlusion_query=2` |
| 2 | same | O6 equals coverage | otherwise depth-off queries need a Z ALWAYS/no-write state for query draws (open item) |
| 3 | same | `chrome9_gl_occlusion_smoke` 0 mismatches | make `occlusion_query` default 1 (or 2) |
| 4 | `texture_3d=1` | rung `volume` V1..V3 | a row-shifted slice 1 means a padded slice stride (§4.5) |
| 5 | same | `chrome9_gl_tex3d_smoke` 0 mismatches | make `texture_3d` default on |

### 11.7 Open questions

* Whether A1C counts with the depth test disabled (O6) and whether the
  vendor snapshot waits for the draw's last pixels (O5, mode 2 otherwise).
* The SL page below the GART is assumed unused by firmware; the canaries
  and the poison detect a collision, not prevent it.
* Rendering into 3D slices goes through the shadow path and is untested;
  the llvmpipe alias refuses every non-2D resource, so a fallback draw
  into a slice uses the shadow as well.
* The kernel has one tag per draw.  Mesa's interval scheme needs no more,
  but a future pipeline-statistics query would need a second tag or its
  own intervals.

## 11. Implementation patches: VS static flow control

Status 2026-09-25: reviewable patches, not applied, built and host-tested
only; nothing below has run on VX900 silicon.

| file | content |
|---|---|
| `~/tmp/agent-vsf/rebase/kernel-vsflow.patch` | kernel, `-p1` from `linux-7.2`, includes `include/uapi/drm/chrome9_drm.h` |
| `~/tmp/agent-vsf/rebase/mesa-vsflow.patch` | Mesa, `-p1` from `mesa-26.2.2`, includes the `include/drm-uapi/chrome9_drm.h` sync and `chrome9-tcl-flow-test` |
| `~/tmp/agent-vsf/chrome9_gl_vsflow_smoke.c` | GL smoke test, hardware TCL with/without flow control against the software vertex path |

Base (rebased): kernel `272da90` (occlusion query, 3D textures; CAP2 bits
6/7 kept, VS flow stays bit 8), Mesa driver `7f2163a` and Mesa root
`099d3b6`; `patch -p1 --dry-run` is clean on both trees.  The first
version against kernel `4f6eeda` / Mesa `d4eb87d` remains in
`~/tmp/agent-vsf/`.  Both
patches pass `git apply --check` in place and on copies of the working
trees (including the uncommitted `chrome9_engine.c`/`chrome9_drv.h`
changes of 00:40).  The patched kernel copy builds with `W=1` without
warnings; the Mesa files compile with the `objs-chrome9-llvm` commands
(only the pre-existing `-Wextra` `fabsf` note in `chrome9_context.c`).

### 11.1 UAPI and kernel

* CAP2 bit 8 `VERTEX_STATIC_FLOW`, advertised only with `hw_tcl=1` and
  the new `vs_flow=1` (default off until rung `vsflow` passes).  RE knobs
  `vs_flow_depth` (1..4, default 1 = the vendor `StaticFlowControlDepth`)
  and `vs_flow_dynamic` (IF on a temporary, rung F3; never advertised).
* Ops `REP 20`/`ENDREP 21` (src0 file `INT` 4, identity swizzle, i0..i15)
  and `IF 22`/`ELSE 23`/`ENDIF 24` (src0 file `BOOL` 3, b0..b15), the
  values of `chrome9_rq_proposed.h`.  Unused operands zero.
* The last draw words become `reserved` (line stipple), `bool_constants`
  (bits above 15 rejected) and `int_constants_ptr` (16 x XYZW `__u8`), the
  offsets of `drm_chrome9_vertex_raster_ext`; the rung tool compiles
  unchanged.  Both must be zero without `vs_flow`.
* Records (vendor corpus rows `if_b0`, `if_else_b0`, `rep_i0` reproduced
  bit-exact by the self-test):

  ```text
  REP i#   (96+#)<<14|154  00280400  0030xxxx (body records)   00380000
  IF b#    (112+#)<<14|14c 00280400  0030xxxx (end or ELSE+1)  003a0000
  ELSE     0020a490        0028xxxx (end)  00000000            003a0000
  ```

  ENDIF/ENDREP patch displacements only.  Correction to 6.1: the
  "end-marker" bit 2 of the last body record is the Z writemask bit; the
  corpus bodies with a full mask (`0004010f`) are identical to
  straight-line records, so no marker is emitted.  A partial-mask body
  (rung F2 writes `.xyz`) is still the silicon check.
* Validation: nesting up to the knob; inside a region only TEMP
  destinations and no INPUT sources (A62 names the last input record);
  temps may be rewritten inside a region (loop-carried values), after
  ENDIF only lanes written on both arms stay defined, after ENDREP only
  the lanes defined before the loop (zero trips).  Empty REP bodies are
  rejected.  A temp defined before a loop and touched in it stays
  allocated until ENDREP.
* Upload: b# ORed into the TCL A61 word, i# as counted H3 `33030000`
  with 16 packed dwords (client bytes may look like `fe` headers).

### 11.2 Mesa

`chrome9_tcl.c` keeps the straight-line path unchanged without the cap
(checked byte-for-byte by the test).  With it, control flow left after
unrolling, key specialisation and peephole selects is emitted:

* loops of the form header + `if (c) break` (also NIR's rotated form with
  the break flag in a phi) whose counter has a constant init and step and
  whose bound is a CB0/literal expression become REP i#; the trip count is
  found per draw on the CPU from the current CB0 (at most 255, else the
  draw falls back with "flow control constants");
* ifs on CB0/literal conditions become IF b#, the condition evaluated per
  draw; other ifs fall back unless `CHROME9_TCL_DYNAMIC_FLOW=1`;
* phis become temps (moves before the loop, a parallel copy on the back
  edge, moves in both arms); inputs read inside a region are copied at the
  top; outputs inside a region and values escaping a region reject the
  variant;
* after eight keyed compiles of one shader (`CHROME9_TCL_KEY_CHURN`) one
  unkeyed variant reads the control uniforms at draw time.

Environment: `CHROME9_TCL_FLOW=0` disables, `CHROME9_TCL_FLOW_DEPTH`
matches `vs_flow_depth`, `CHROME9_DEBUG_TCL=1` prints `TCL flow:` reasons.

### 11.3 Host tests

* `tools/re/chrome9_kvertex_selftest.c` with `kshim`: new corpus, loop
  lifetime, nesting, dynamic and 13 rejection cases pass; mutating an
  expectation or removing the ENDREP lifetime extension makes it fail.
* `chrome9-tcl-flow-test`: uniform loop, swap, literal 40-trip loop,
  uniform if around a loop (depth 2), data-dependent if around a loop
  (dynamic), straight line; the emitted kernel IR runs in a reference
  interpreter against the NIR semantics for trip counts 0..255.
* The Mesa-emitted programs and the rung F1/F2/F3 programs compile with
  the host-built kernel compiler (F3 only with the knob).
* The GL smoke runs under Xvfb with software GL; its cases give distinct
  images (count 0..255 change the covered area).

### 11.4 Workload value (static analysis, no target runs)

| rank | workload | VS control flow | today | with the patch |
|---:|---|---|---|---|
| 1 | glmark2 `ideas` (paper, table, text) | `if (currentTime > ... && ...)`, uniform, new value every frame | keyed: one NIR compile per frame and shader, 32-variant LRU churn | after 8 compiles one unkeyed variant (selects, no flow op needed) |
| 2 | glmark2 `loop` VS | `for (i < VertexLoops)`, uniform 5, set once | keyed once, unrolled | same; REP only when the count exceeds 31 or changes per draw |
| 3 | ioquake3 rend2 `generic`/`lightall`/`fogpass`/`dlight`/`shadowfill` | uniform int chains on DeformGen, TCGen, ColorGen, AlphaGen; no loops | keyed per combination; eviction possible with more than 32 combinations | churn variant only if all arms fit 256 records; else-if chains need depth > 1 (future: per-path b# conjunctions at depth 1) |
| - | `conditionals`, `function`, `jellyfish`, `shadow`, `refract`, `terrain` | data-dependent side-effect-free ifs or none | if-converted / straight line | unchanged |

No default glmark2 scene and no rend2 vertex shader needs flow control
that specialisation, unrolling and if-conversion cannot express; the
remaining TCL blockers there are elsewhere (terrain VS texture fetch,
rend2 bone indexing needs `a0` relative constant addressing).  Confirm on
the target with `CHROME9_TCL=1 CHROME9_DEBUG_TCL=1`: no `TCL cannot
express control flow` or `TCL flow:` line should appear for these scenes.

### 11.5 Target procedure

```sh
modprobe chrome9 enable=1 hw_tcl=1 vs_flow=1
flock -w 5 -x /tmp/chrome9-vx900-gpu.lock timeout 7 ./chrome9_rq_rungs vsflow
CHROME9_TCL=1 ./chrome9_gl_vsflow_smoke > hw.txt
CHROME9_TCL=1 CHROME9_TCL_FLOW=0 ./chrome9_gl_vsflow_smoke > noflow.txt
CHROME9_TCL=0 ./chrome9_gl_vsflow_smoke > sw.txt
```

`hw.txt` must equal `sw.txt` (hashes quantize two low bits).  Keying
folds the uniform branches of `branch loop`/`dynamic branch loop`, so they
run as depth-one REP or straight line; the 40-trip counts and the churn
cases exercise REP with literal and CB0 bounds, 256 trips must fall back.
Depth two and F3 need `vs_flow_depth=2` / `vs_flow_dynamic=1` together
with `CHROME9_TCL_FLOW_DEPTH=2` / `CHROME9_TCL_DYNAMIC_FLOW=1`.

Open: op-29 semantics on a temporary (F3), nesting beyond one, REP with a
partial-writemask last body record, and whether IF b# should replace
selects for large uniform arms (performance, after the features).

### 11.8 Silicon result 1 and v2 (2026-09-25)

Kernel `5749595` + v1 (`occlusion_query=1 texture_3d=1`), rung tool with
the v1 rung patch:

* `occlusion`: READ `-EIO`, dmesg `occlusion snapshot slot 0: ffffffff
  0cc10001 0cc10002 0cc10003` on both runs: the vendor type-10 read-back
  did not write the SL slot below the GART at all.
* `volume`: `DRAW_FRAGMENT_PROGRAM 3D: EINVAL`.  Cause is the rung, not the
  3D validation: its program was a single `TEX oC0`, which the generic ABI
  rejects for every target (`alu_count == 0`, host-checked with the kernel
  compiler: 2D and STR both `-EINVAL`; `TEX r0` + `MOV oC0, r0` compiles to
  TAU `301001b0` with a three-lane T0).

Re-reading the vendor path: `hwmUpdateStatus_h5` computes `Q` as the event
slot's CPU pointer minus a context base, and the slot lives in the shadow
region of `hwmInitShadowRegion_h5i:0xac860`, allocated with `mmAlloc` type
0, which `mmAlloc:0x1d72f0` turns into `mmAllocPageLockedMemory(.., 2)`:
page-locked system memory, not local frame buffer.  So A21/A22 most likely
hold an SF (GART) address; the SL reading of §2.2 was an inference.  With
an SL offset near 512 MiB the write, if any, went to an unmapped or
out-of-range GART address.

v2 (`~/tmp/agent-oq/v2/`: `kernel-occlusion-3d-v2-incremental.patch` on
top of the applied v1, `kernel-occlusion-3d-v2-full.patch` against
`5749595`, `rq-rungs-v2-*.patch`; Mesa unchanged) adds the 0600 knob
`occlusion_re` (sampled per submission) and makes `occlusion_query` and
`texture_3d` 0600:

| bit | variant |
|---|---|
| 1 | A21/A22 before the type-10 read-back |
| 2 | `6c 00800000` 3D drain first (as `occlusion_query=2`) |
| 4 | A21/A22 address in dwords |
| 8 | destination in a coherent kernel page at the last GART entry (SF `0x03fff000`), reserved from the draw mappings |
| 16 | manual PPC type 17h write-back (`00080000` group, `01000017`, dword address, location SL or SF with bit 8) instead of the read-back |
| 32 | dmesg per slot: the four slot words, stray writes into the GART scratch page (every unmapped SF address), and the SL dwords an A21-only or dword-unit decode would hit (read only) |

The rung tool gets `occlusion1` (one query, one triangle, result against
red pixels) and the fixed `volume` program.  Ladder, SF first because a
mis-decoded SF write lands in the scratch page: 40, 42, 41, 56, then SL
34, 33, 36, 48.  The first variant whose slot holds the red-pixel count
decides the default.

### 11.9 Silicon result 2 and v3 (2026-09-25)

v2 ladder on kernel `272da90` + VS-flow + v2: `volume` passes (V1..V3,
V3 within 1), so the 3D target, the slice stride `pitch * height` and the
R filter/wrap fields are confirmed.  `occlusion1` failed for every
`occlusion_re`: no type-10 read-back variant (SL or the GART page, any
order, drain, dword units) wrote anything anywhere watched, and nothing
reached the GART scratch page.  Only the PPC type 17h write-back to SL
(`re=48`) wrote: the dword at `slot_offset / 4` went `8dffb9ff ->
00000000`.  The field value there was `slot_offset >> 2`, so the engine
takes A02/A03 of HParaType 08h as a byte address, contrary to the manual's
"in unit of 4 bytes" (p.175).  It wrote 0.

Candidates for the 0: the write-back fires when the command regulator
parses the group, before the draw's pixels reach the back end (the draw
was reset right before); HZOcclusionCNT counts only with hierarchical Z;
or the counter needs an enable that neither manual part documents
(`HenWK_CNT` gates only 16h).  v3
(`~/tmp/agent-oq/v3/kernel-occlusion-v3-incremental.patch`) uses the byte
address, adds `occlusion_ppc_type` (0600, default 17h) and
`occlusion_re` bit 64 (the CR `00300000` engine-idle group before the
snapshot).  Pipeline counters 05h/06h are free-running: a non-zero value
there proves the write-back path and the pixel counters independently of
17h's reset.

### 11.10 Silicon result 3 and v4 (2026-09-25)

v3 ladder (`occlusion1`, 1176 red pixels): PPC 17h to the SL slot byte
address wrote `00000000 00000000` without a drain, `00000498 00000000`
(= 1176, exact) after the `6c 00800000` drain with or without the CR idle
group, and 0 after the CR idle group alone.  06h/05h returned large
free-running values; the GART page received nothing.  So: HZOcclusionCNT
is the A1C/A1D counter, its write-back happens when the command regulator
parses the group unless the 3D pipeline is drained first, it stores two
dwords (count, 0), and its address is a byte address.  v3's retirement
still expected a canary in dword 1, hence READ `-EIO` despite the exact
count.

v4 (`~/tmp/agent-oq/v4/kernel-occlusion-v4.patch`, against `bde40ec`)
makes drain + 17h write-back the production snapshot (knob 0), poisons
both written dwords, checks dword 1 == 0 plus the two canaries, keeps
`occlusion_re`/`occlusion_ppc_type` as debug overrides and
`occlusion_query` at default 0.  The drain costs one pipeline flush per
tagged draw, only while a query is active; Mesa already tags only draws of
an open counting interval.

## 12. Feature batch on silicon (2026-09-25)

Rung tool: `tools/re/chrome9_rq_rungs.c` (with `chrome9_rq_proposed.h`),
new rungs `psstore`, `psizen`, `blend`, `blendalpha`.  Kernel builds k72..k86
(`tcl-20260924/k*` on the target), Mesa libraries `chrome9-live/f1/m*`.

### 12.1 Vertex point size: A61[22:20]

The vendor writes object `+0x08` at draw time (`stmUpdateFVFOut_H5i`,
`dri:0x1ba8a4`; Windows `VTGUModeDX32A.dll:0x10206cd5..0x10206d34`) and
`stmUpdateVS_inv` puts it into A61[22:20].  Windows: with oPts (or oFog)
written, `n = texcoords + (UCP + 2) / 3`, field `n > 3 ? 4 : 1`; Linux DRI
always 4.  The earlier "never nonzero" reading came from compiler-only probes.

| rung | A61[22:20] | result |
|---|---|---|
| `psize`, VS size from input/temp/constant | 0 | size 1 for every VS size (A2B minimum) |
| `psize` | 1, 5 | all whole sizes GL-exact; 2.4 -> 2, 2.6/3.4 -> 3 (round to nearest like A2A) |
| `psizen`, 1..8 varyings, VS size 3 | 1, 4, 5 | 3x3 at (19,19) for every count |

One early run with 4 drew nothing; it did not reproduce after a fresh load
and is attributed to the intermittent readback failure below.  Kernel:
`chrome9_tcl_a61_outputs()` applies the Windows rule when
`RASTER_POINT_SIZE_VERTEX` is set, `tcl_a61_out` (0600) overrides it.  CAP2
bit 19 `POINT_SIZE_VERTEX`; Mesa enables per-vertex sizes (`CHROME9_TCL_POINT_SIZE`,
default on) only with it.  A2A (state size) rounds 2.5 inconsistently (2x3 /
3x2 depending on phase); Mesa rounds sizes itself, so this is never sent.

### 12.2 Native points and polygon mode on by default

Rungs `points`, `pedge` (near-plane point clip), `sprite`, `lines`, `wire`,
`polymode` pass.  `chrome9_gl_points_lines_native_smoke` passes against the
draw module (GL 1.3 and with `MESA_GL_VERSION_OVERRIDE=2.1`).  Two smoke
fixes: the wireframe reference now draws every edge in one direction (the
draw module's lines are direction dependent, so shared strip/fan diagonals
covered 412/430 pixels against the GL 354/398 of llvmpipe and the hardware),
and the lower-left sprite case needs GL 2.0 (chrome9 reports 1.3 unless
overridden; `GL_POINT_SPRITE_COORD_ORIGIN` is INVALID_ENUM before 2.0).
Remaining draw-module fallbacks: lower-left sprite origin, polygon POINT
mode (GL draws the clipped polygon's vertices), unfilled quads/polygons,
wide/smooth/stippled lines, flat lines with first-vertex convention.

### 12.3 Vertex a0 relative addressing

Encoding from the vendor corpus (`~/tmp/c9re-a0`): MOVA op 5, dword3
`000a001m`; relative constant reads carry the a0 lane in dword3[27:24]
(4..7).  Kernel: `DRM_CHROME9_VERTEX_OP_ARL`, `DEST_ADDRESS`, source
`relative` 1..4, ARL lowered to MAX/MIN against two kernel rows after the
user constants, FLR and MOVA (in-range clamp), CAP2 bit 14
`VERTEX_RELATIVE`.  Mesa lowers `load_ubo_vec4` with a dynamic offset.
`chrome9_gl_arl_smoke`: 8/8 cases native and correct (GLSL `colors[int(x)]`,
`int(-0.5)`, `int(abs(x))`, `[i]+[i+1]`, two indices, mat4 bones, ARB
`c[a.x]`, `c[a.x+1]`).  With `vs_relative_re=1` (bare op 5, no FLR) index
1.5 selects element 2, so MOVA does not floor; the production FLR+MOVA is
exact.  Out-of-range behaviour was not probed (clamped in production).

### 12.4 Vertex texture fetch: not in the hardware

D3D caps template: VS at most vs_2_0, `VertexTextureFilterCaps` 0,
`MaxVertexShader30InstructionSlots` 0, no `FORMATOP_VERTEXTEXTURE` format;
vendor GL has no `MAX_VERTEX_TEXTURE_IMAGE_UNITS` query; the native VS ISA
has no sampler file or fetch opcode.  Mesa keeps vertex-sampling shaders on
the llvmpipe path.

### 12.5 Fragment program store: 96 ALU records

`ps_alu_max` (0600, 96..144) lifts the kernel limit for probing only; rung
`psstore` builds a chain of L ALU records.  L = 3..96 draw the exact
(L - 2)/255 red.  L = 97 timed out the engine (`job timeout phase=fe33`,
engine status `00002711`) and needed a power cycle.  96 ALU records is the
hardware limit; the kernel default stays 96 and longer programs keep the
llvmpipe path.  A multi-pass split is not viable (no exact RGBA float
intermediate target, blending/KIL/depth only valid in the last pass).
Fragment ALU precision (main session): 16 mantissa bits.

### 12.6 Interrupt-driven fences: blocked by an unidentified source

`fence_irq` (0400): 1 = FE33 with interrupt generate + queue skip
(`fe330026`), 2 = gate only.  With only the global gate 0x200 bit 31 set and
every other enable cleared, the line (IRQ 36, INTx) fires continuously even
when no FE33 requests an interrupt (`fence_irq=2`): 0x200 `80000002`, 0x204
`00000c00` (bit 5 clear), 0x1280 `85000800` (only reserved bits and the MSI
re-trigger bit), 0x494 0.  The first attempt without a handler for foreign
sources ended in "irq 36: nobody cared".  The kernel now counts foreign
interrupts and after 64 restores 0x200 and falls back to polling
(`foreign interrupt (...), polling fences`).  The asserting source is not in
the documented status registers, so the feature stays off (default 0).

### 12.7 Blend equations and the clamp bit

ADD, MIN, MAX and all non-constant factors match the recipe words exactly
(rungs `blend`, `blendalpha`).  SUBTRACT/REVERSE_SUBTRACT: with A34/A36
bit 16 clear, a negative channel stores `ff` (`ffa0ff40` for 00a00040,
`40ff40ff` for 40004000); with bit 16 set every case is exact and ADD above
one still saturates (`c0ffffff`).  So bit 16 = 1 clamps at zero, the
opposite of the recipe note.  The kernel sets bit 16 only in subtract lanes,
so existing words are unchanged; CAP2 bit 18 `BLEND_EQUATION`.  Constant
blend colour factors still go to llvmpipe (no field in the blend state).

### 12.8 Intermittent readback failure

Several rung runs after module swaps drew nothing (buffer still at the
prefill) while GL clients worked; re-loading the same module later passed
(k74e: fail after boot, pass after a later handoff).  Not caused by these
patches (bisected variants passed and failed on the same binary); tracked
with the state-cache investigation.
