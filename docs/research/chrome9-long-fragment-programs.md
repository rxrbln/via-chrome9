<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 fragment programs longer than the 96-record ALU store

Status 2026-09-25.  Evidence labels: **S** static (disassembly, manual),
**D** dynamic (vendor code executed on the host), **Si** silicon (measured on
the VX900), **I** inference.  No program above 96 ALU records was submitted,
`ps_alu_max` was not touched, no module was loaded and X was not restarted.
The only target runs were ordinary glmark2/piglit runs with library
`tcl-20260925/p81`, where every over-long program already renders in llvmpipe.

Companion documents: [chrome9-fragment-capacity-strategies.md](chrome9-fragment-capacity-strategies.md)
(terrain noise record bounds, llvmpipe fallback design),
[chrome9-fragment-capacity-boundary.md](chrome9-fragment-capacity-boundary.md)
(packet and uAPI bounds), [chrome9-queries-msaa-3d-flow-re.md](chrome9-queries-msaa-3d-flow-re.md)
§12.5 (rung `psstore`), [chrome9-mrt-re.md](chrome9-mrt-re.md),
[render-target-format-oracles.md](render-target-format-oracles.md).

## Summary

1. No vendor driver runs a pixel program longer than 96 ALU records.  The
   Linux 5.75 DRI, the Windows D3D UMD and the Windows GL ICD share one
   append routine that refuses record 97 and returns without error; its
   callers never test the result, so an over-long program is silently
   truncated.  There is no split, no reload during a draw, no software
   pixel path and no use of any store window above 96 (§1).
2. The manual states the store size explicitly ("total 96 instructions",
   384 dwords).  The A95..A9A switch bits and the 9-bit length field describe
   128 records, not 144; A9A[23:8] are temporary destination-check bits.  No
   bank, paging or prefetch mechanism exists in the manual or the vendor
   code (§2).  Silicon: 97 records lock up the engine (`psstore`).
3. A Mesa multi-pass splitter is possible in principle but is not viable
   today: the only exact intermediate carrier is R32F (one scalar per target,
   four with MRT), which neither the kernel nor Mesa implements and which is
   silicon-unqualified; 8-bit carriers need encode/decode code that consumes
   most of each pass.  On the one real workload (glmark2 terrain noise) the
   current compiler order needs 8 passes carrying 13..20 scalars per cut,
   far beyond four R32F targets (§3, §4).
4. The piglit programs that exceed the store (`fs-sign-times-*`) and
   `glsl-fs-loop-300` depend on uniforms only.  They are better served by
   draw-time CPU evaluation than by multi-pass (§4.2).  glmark2 `ideas` no
   longer has an over-long program.
5. No Mesa prototype was built: the splitter needs an unqualified format on
   silicon, and the uniform-only case already passes through llvmpipe.  The
   partition analysis tool and a guarded R32F ladder for the main session
   are provided (§5, §6).

## 1. Vendor behaviour (question 1)

### 1.1 One append routine in all three drivers

| Driver | ALU append | Bound | On overflow | TAU append |
|---|---|---|---|---|
| Linux 5.75 DRI | `INVPS_AddAInst` `0x19a290` | `cmp edx,0x5f; jbe` at `0x19a2b5` (count at object `+0x08`) | returns 0 at `0x19a2ba` | `INVPS_AddTInst` `0x19a1e0`, `cmp edx,0x1f` |
| Windows D3D UMD `VTGUModeDX32A.dll` | `0x102006e0` | `cmp eax,0x60; jb` at `0x102006ec` | `xor eax,eax; ret` at `0x102006f1` | `0x10200790`, `cmp eax,0x20` at `0x1020079c` |
| Windows GL ICD `VTGOGL32A.dll` | `0x10253760` | `cmp eax,0x60; jb` at `0x1025376c` | `xor eax,eax; ret` at `0x10253771` | same pattern |

**S**  The Windows routines are identified by the same object layout as the
Linux symbolised one: ALU data `+0x04`, ALU count `+0x08`, ALU grow array
`+0x0c`, TAU data `+0x10`, TAU count `+0x14`, TAU array `+0x18`, flags
`+0x1c` (bit 18 = HPSFireALU set by the first ALU record), last-unit state
`+0x58` (1 = ALU, 2 = TAU), and the A2T/T2A switch bit clear/set helpers.
Windows additionally clears the switch bit with an inline `count / 24`
divide (`0xaaaaaaab` multiply at `0x1020075c`).

**S**  No caller checks the return value: 211 call sites in the Linux DRI,
227 in the UMD (`call 0x102006e0`) and 213 in the GL ICD (`call 0x10253760`)
continue with the object's data pointer.  **D** The Linux executable sweep in
`via-5.75-shader-re.md` shows the consequence: 97..128 MOVs "compile" with
ALU count 96, 129 MOVs crash the compiler.

### 1.2 Upload: whole program at store address 0, once per bind

**S**  Every vendor ALU upload writes the H3 sub-header `0x20030000`
(HParaSubType 20h, HParaType 03h, HParaAdr 0) followed by `count * 4`
dwords, and TAU `0x21030000` followed by `count` dwords:

- Linux `stmUpdatePS_inv` `0x17fdcb`; the blit paths `via3DBlit`
  `0x1f27d6`/`0x1f2df6` and `__glDpSyncFrontBufferToFakeFront`
  `0x23715e`/`0x23783e` upload their own fixed programs the same way.
- UMD `0x1015b857` (count from the object `+0x08` at `0x1015b378`, then
  A95..A9B control words at `0x1015b7a8..0x1015b801`), TAU at `0x1015b8d0`.
- GL ICD `0x10205f77`, TAU at `0x10205ff0`.

The start address is always the immediate 0, and the upload is part of the
state emitted before the draw.  **I** There is no path that loads a second
program part or reloads the store while a primitive is in flight; a program
change between draws is ordinary state.

### 1.3 No split and no software pixel path

**S**  `stmCreatePixelShaderInstance_inv` compiles once and has no second
program or pass loop (capacity-strategies §1).  Neither the Linux DRI symbol
table nor the Windows DLL strings contain a multi-pass or split path; the
DRI's `__glDepthPassSpan` family is the software rasteriser's span code, not
a pixel shader emulator.  The only shader emulator is for vertex shaders.

### 1.4 API limits

**S**  D3D9 caps (UMD template `0x1030fcc8`): `ps_2_0`,
`PS20Caps.NumInstructionSlots=96`, `NumTemps=12`, no flow control,
`MaxPixelShader30InstructionSlots=0`.  The D3D runtime rejects longer
token streams before the UMD, so the Windows driver relies on the
application.  A shader within the D3D slot count can still expand beyond
96 native records (computed-coordinate TEX bridges, macro expansion); the
UMD then truncates like the Linux DRI (**I**, from the unchecked append).
Linux GL advertises 64 native instructions / 32 ALU / 32 TEX / 12 temps
for ARB programs and still reports an over-long program as "under native
limits" (`__glim_GetProgramivARB` `0x2db956`).  The Windows GL ICD exposes
`GL_ARB_fragment_program`, `GL_ARB_fragment_shader` and
`GL_NV_fragment_program2`; its limit queries were not traced beyond the
shared append routine.

## 2. Store size and the 97..144 window (question 2)

| Item | Evidence |
|---|---|
| "Each instruction contains 4 double words ... Because of total 96 instructions, there are up to 96*4 (=384) entries" (HParaSubType 20h) | **S** HC3 manual Part 2 p.129; identical in the HCM manual |
| TAU store "totally 32" records | **S** same page |
| A92[8:0] PSALUILen (9 bits), A92[21:16] PSTAUILen | **S** p.96 |
| A95..A99: ALU-to-TAU switch after ALU record 0..119, A9A[7:0]: 120..127 | **S** pp.97--99 |
| A9A[23:8]: HPSR0..15DstChk, first-use destination check of r0..r15 | **S** p.99 |
| 97 records: `job timeout phase=fe33`, engine status `00002711`, power cycle | **Si** rung `psstore` |

The control words therefore describe at most 128 records, and indices
128..143 would set destination-check bits, not switch bits.  The kernel's
probing range `ps_alu_max` 96..144 (`chrome9_fragment.c:17..26`) maps
`controls[5 + index / 24]` for index 120..143 into A9A, i.e. into
HPSRnDstChk for index >= 128 (**S**).  This is moot because 97 already
locks up, but the parameter's upper bound should be read as a software
bound, not a hardware window.

Other A90 fields that could be mistaken for store extensions:

- A90[16:15] HPSConFig (normal/double/triple/4-time) trades per-pixel
  texture and temporary registers (16/12, 8/6, 5/4, 4/3) for pixels in
  flight.  It does not change the instruction store (**S** p.94).
- A90[17] HPSDbCnstR mirrors constants 0..22 into 32..54 (**S**).
- HParaAdr on the ALU upload is a dword start address, so partial reloads
  are addressable.  **I** They would only replace records between draws;
  without flow control a pixel cannot continue into reloaded code.

No bank select, page register, prefetch or "long shader" bit exists in the
PS setting registers (A90..A9A) of either manual, and no vendor code writes
anything but address 0.  Nothing in this section is silicon-verified beyond
the `psstore` lockup.

## 3. Intermediate carriers for a split (question 3)

Fragment ALU values have 16 mantissa bits (**Si**, §12.5 of the flow
document), so a carrier is exact if it stores at least sign, exponent and
16 mantissa bits.

| Carrier | Status | Exact? | Scalars per target |
|---|---|---|---|
| R32F RT (A52 `f5`, private `203e`) + R32F texture (A30 `f5`), nearest only, no blending | **S** vendor RT and FORMATOP tables; not in kernel uAPI or Mesa (`DESTINATION_FORMAT_*` stops at ARGB4444, textures are 8-bit and DXT) | yes, if RT store and TAU return are unmodified (**Si** unverified) | 1 |
| G16R16F (`f4`), R16F (`f0`) | **S**; `f4` channel order unproven | no (10-bit mantissa) | 2 / 1 |
| G16R16, L16 UNORM16, A2R10G10B10 | **S** | only for a proven [0,1] range, 16 bits | 2 / 1 / 3 |
| BGRA8888 (+MRT, live) | **Si** | only with an encode (exponent via LG2/FLR, two mantissa bytes via MUL/FRC, sign/exponent byte) and decode; about 10--12 ALU records to encode and 6 to decode per scalar (**I**), plus the float-to-UNORM8 rounding question of `chrome9-dlight-alpha-blur-precision.md` | 1 |
| A16B16G16R16(F), A32B32G32R32F | absent (UMD maps `D3DFMT_A16B16G16R16` to 0) | -- | -- |
| depth output | one clamped [0,1] value coupled to the depth test | no | -- |

MRT limits (`chrome9-mrt-re.md`): four targets, all of one bit depth (no
`MRTINDEPENDENTBITDEPTHS`), and any float or 16-bit-per-channel target
disables post-rendering (blend, AA, masks forced to `f`) for every target.
Consequently the intermediate passes can carry at most **four exact scalars**
(four R32F targets), and the pass that writes the application target cannot
also write R32F intermediates.  With BGRA8 encoding one cut of four scalars
costs about 40--50 encode records in the writing pass and about 25 decode
records plus four TEX in the reading pass, i.e. most of the 96-record
budget; that carrier is not useful.

## 4. Splitter design and feasibility

### 4.1 Design (R32F carrier)

Compiler (Mesa, on the lowered straight-line NIR):

1. Estimate records per instruction (the builder's cost model), compute the
   live scalar set at each cut in a schedule order, treating inputs,
   uniforms and constants as rematerialisable (free in every pass).
2. Choose cuts by dynamic programming: each pass <= 96 ALU records including
   its output stores and the TEX bridges of its reads, <= 32 TAU, <= 12
   temporaries, <= 4 carried scalars in and out, application samplers plus
   carried reads <= 8.
3. Emit one NIR shader per pass: carried values come from
   `texture(carry_k, gl_FragCoord.xy * inv_size)` (vPos is supported, HPSLocationReg
   A90[19]); intermediate passes write `FRAG_RESULT_DATA0..3`; only the last
   pass writes the application colour, depth output, and performs KILL.  A
   KILL decided in an earlier pass is carried as one scalar.
4. Fail closed to llvmpipe whenever any bound is not met.

Draw (Mesa `chrome9_draw.c`/`chrome9_context.c`):

- Per-pixel intermediates are only correct if no two fragments of a batch
  hit the same pixel.  Split draws must use the software vertex path (the
  hardware TCL path does not return screen positions) and bin triangles
  into screen-disjoint batches; each batch runs all passes before the next.
- Intermediate passes: same vertices and rasteriser state (identical
  coverage, **I**), depth/stencil test with writes disabled, no blending,
  outside any occlusion query; the last pass restores the application
  state.  Carried targets are driver-owned R32F objects the size of the
  framebuffer, ping-ponged between passes, with the render-to-texture
  flush between a write and the next read.
- Cost per batch: one program upload (about 1.7 KB) and one job per pass
  plus the R2T flush.  With many small overlapping triangles (glmark2
  `ideas`-like scenes) this is dominated by per-batch overhead; it only
  pays off for large, non-overlapping primitives such as full-screen passes.

### 4.2 Workloads (measured with `p81`, **Si** host-side scan)

| Workload | Current state | Split outlook |
|---|---|---|
| glmark2 `ideas` | all fragment programs native (`unsupportedFS=0`, 69 fps); the lamp program fits since the pow20 vectorization | not needed |
| glmark2 `terrain` noise pass | one unsupported FS, builder rejects at `ir=96`; scene 4 fps | see below |
| piglit `fs-sign-times-{sign,neg,abs,neg-abs}` | builder rejects at `ir=96 temps=99`; PASS through llvmpipe | output depends on the uniform array `v[25]` only |
| piglit `glsl-fs-loop-300` | unsupported (uniform loop bound 300, unroll limit 31); PASS through llvmpipe | output depends on uniforms `iters`, `step`, `expected` only |

The remaining unsupported piglit fragment programs in the scan (`fs-dfdx-*`,
convolution, front-facing ternaries, indirect constant arrays, non-unrolled
loops) are rejected for operation or control-flow reasons, not length.

Terrain noise, analysed with `tools/re/chrome9_fs_cut_width.py` on the
`CHROME9_DUMP_NIR` output (694 NIR instructions, one record per vector
operation, free negation, constructor moves counted): about **528 records**
in NIR order, octaves of 129--133 records.  The only cuts of at most four
scalars lie between octaves.  Mid-octave cuts that keep both halves
within 96 records carry at least 13 scalars, and no partition with <= 19
carried scalars exists; with 20 the partition is 8 passes carrying
19/13/20/13/20/13/17 scalars.  A splitter on today's
compiler therefore cannot run terrain with four R32F targets.  With the
corner-transposed schedule of capacity-strategies §3 (about 90 records per
octave) the octave cuts would carry one scalar and 4 passes would suffice;
that compiler work is the prerequisite for any split.

The uniform-only piglit programs do not need a split at all: their colour is
a function of uniforms, so a draw-time evaluation on the CPU (substitute all
referenced CB0 dwords, fold, unroll with the known trip count, cache by
value) reduces them to one constant MOV.  This extends the existing
inline-uniform variants (`chrome9_select_fs_variant`, limited to four
dwords and `max_unroll_iterations=31`).  It moves five piglit tests from
llvmpipe to the hardware and has no glmark2 effect.

### 4.3 Assessment

- Correctness risks: R32F store/sample exactness and special values, vPos
  exactness at texel centres, identical coverage across passes, R2T
  ordering, MRT with float targets.  All are silicon-unverified.
- Benefit: terrain noise only, and only after record reduction; everything
  else affected today is either native already or uniform-only.
- Recommendation: keep llvmpipe as the correctness path.  Do the
  record-reduction compiler work first (capacity-strategies §3), then run
  the R32F ladder below, then measure the pixel ALU rate before writing the
  splitter.  The uniform-only evaluation is optional and low value.

## 5. Prototype status (question 4)

No Mesa code was written.  The splitter depends on the R32F carrier, whose
kernel and silicon qualification is outside what this session may run, and
on a compiler that the current record density rules out for the one real
workload.  Delivered instead: the partition analysis
`tools/re/chrome9_fs_cut_width.py <nir.txt> [budget] [max_targets] [-v]`
(record estimate, cut-width histogram, minimum pass count per carrier
width).  No full piglit run was needed because no driver code changed.

## 6. Guarded silicon ladder for the main session

Preconditions: kernel with an R32F destination and texture format behind a
0600 parameter defaulting to off; vendor post-render rule applied when any
target is R32F (A00[11:0] = 0, A00 bit 20 = 0, A01 bit 5 = 0, A53/A5B/A63/A6B
= `f`); blend, logic op and AA rejected for float targets; every program
<= 96 ALU records (`ps_alu_max` untouched).  One rung per run, `flock -x
/tmp/chrome9-vx900-gpu.lock`, runs under 20 s, guard pages around every
target; stop and power-cycle via NanoKVM on the first timeout.

| Rung | Program / action | Pass condition |
|---|---|---|
| F1 | 3-record program writes a constant to a 16x16 R32F target (oC0 only); values 1.5, 1/3, -2^-20, 65504, 1e30, 0 | CPU readback equals the value rounded to 16 mantissa bits; guards intact |
| F2 | CPU-filled 16x16 R32F texture, NEAREST at texel centres, scaled into a BGRA8 target | exact 8-bit images for known patterns |
| F3 | R32F texture -> R32F target copy, 257x129 NPOT, coordinate `vPos * inv_size` | bit-exact copy of 16-bit-mantissa values at every pixel |
| F4 | two jobs: pass A writes R32F, pass B samples it (R2T) | B sees A's data with the production flush, never stale data |
| F5 | MRT with 2, 3, 4 R32F targets (A01[16:14]) | each target receives its own value; guards intact |
| F6 | 90-record program over 256x256 with PPC counter 08h (HRPPC_PSALU) and elapsed time | records per clock for the cost model |

Only after F1..F6 pass is a Mesa splitter worth prototyping, behind an
environment variable, default off until a full piglit run shows zero
regressions against `results/pg-all81.tsv`.
