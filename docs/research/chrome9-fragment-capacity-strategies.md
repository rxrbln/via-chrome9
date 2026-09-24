# Chrome9 fragment capacity strategies for over-long shaders

Status: offline research only.  No DRM node, GPU or test target was used.
Vendor evidence is static disassembly of `via_chrome9_dri.so` (5.75.32.87a,
i386, addresses are ELF virtual addresses) plus the previously recovered
Windows UMD tables.  The noise NIR was regenerated with the existing host
probe `/tmp/chrome9-terrain-inline-probe-current` on
`/tmp/chrome9-terrain-noise-final.{vert,frag}`; its operation counts are
identical to `chrome9-glmark-terrain-scene-current-boundary.md`.

## Summary and ranking

The exact `terrain-noise.frag` cannot be compiled into one Chrome9 pixel
program by any generic compiler technique.  A lane-level lower bound for the
exact linked NIR, assuming perfect four-lane packing and perfect MUL+ADD
fusion, is **248 ALU records**; a realistic, carefully vectorized
(corner-transposed) schedule needs about **90 records per octave, 360
total**, or about 300 with permitted floating-point reassociation.  The store
is 96 ALU + 32 TAU records, 12 temporaries, no flow control, and no vendor
path exposes more (section 1).

Ranked plan:

1. **Generic CPU fragment fallback through llvmpipe** for every draw whose
   fragment program is not natively compilable (capacity, unsupported
   operations, non-unrollable control flow).  It is exact FP32, generic, and
   the terrain height pass costs an estimated 10--40 ms per frame (65,536
   fragments).  It needs LLVM enabled in the Chrome9 Mesa build.  Softpipe
   is the no-LLVM variant but is estimated at 0.5--1.5 s per frame for this
   shader, which misses the target.
2. **Compiler record reduction** (scalarize, then SLP re-vectorize with
   native arbitrary swizzles; dot-product transposition; affine hoisting to
   the software vertex stage; uniform folding).  It does not rescue terrain
   noise (≥ 248), but moves other shaders from the fallback to native
   execution and is required by option 3.
3. **Automatic GPU multi-pass splitting** with R32F ping-pong intermediates
   at narrow cuts (for terrain: one pass per octave, one FP32 scalar carried
   between passes).  This is faster only if the Chrome9 pixel throughput
   is sufficient; it depends on unqualified R32F render/sampling, a
   fragment-position input and overlap-free batching.  It is an optimization
   to consider after option 1 is in place, not the correctness path.

## 1. Vendor evidence: no split, no pixel fallback, no larger store

| Finding | Evidence |
|---|---|
| Native pixel ALU append refuses beyond 96 records and returns without error | `INVPS_AddAInst` `0x19a290`: `cmp edx,0x5f` at `0x19a2b5`, early return at `0x19a2ba` |
| Native TAU append refuses beyond 32 records | `INVPS_AddTInst` `0x19a1e0`: `cmp edx,0x1f` at `0x19a1ff` |
| Overflow is silent truncation, not a failure: 97..128 MOVs compile "successfully" with ALU count 96; 33/34 TEXLD give TAU count 32; 129 MOVs crash the compiler | executable sweep in `via-5.75-shader-re.md` "Pixel program object, limits, and upload" |
| ARB fragment-program compiler has no instruction-count diagnostic at all.  Its message table `glCompilerMsg` (`0x4150c0`, 21 entries, sprintf at `compiler_check_error` `0x14c320`) lists syntax, binding, swizzle/mask and three resource errors: 14 "Too much constant register binding", 15 "Too much temporary register declaration", 19 "Too much address register declaration" | table dump; `compiler_prefirstcompile` `0x14f280` raises 14/15/19 at `0x14f2be` (`>0x140`), `0x14f2c7` (`>16` temps), `0x14f2cd` (`>1` address register) |
| `GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB` (`0x88b6`) returns program field `+0x186c` (`__glim_GetProgramivARB` `0x2db956..0x2db96f`).  `__glDpProgramString` (`0x2400f0`) clears it only for error codes 14, 15 or 19 (`0x240235..0x24028d`) | so an over-long ARB program is still reported as under native limits and then truncated |
| Advertised ARB limits are below the store: native instructions 64, ALU 32, TEX 32, temporaries 12 | `ioquake3-blocker-roadmap-re.md` section 3.2 (`__glS3InvGetDeviceConstants` `0x1d7840`) |
| Pixel shaders are always compiled for hardware; the only CPU shader emulator is for vertex shaders (`SWEmu_SWTL` `0x1225d0`, `__glS3EmulateVertexShader` `0x1ff470`, DWARF units `swemu.cpp`, `SSEVSCompiler.cpp`) | `__glDpUpdatePixelShaders` `0x1eef30` only calls `stmSetPixelShader`, fixed-function PS generation and patch-state helpers |
| `stmCreatePixelShaderInstance_inv` (`0x1b8ff0`) runs `stmPPSCompile_invi` (`0x1b9172`) then SDG optimization and post-translation; there is no second program, pass loop or split path | call list at `0x1b9172..0x1b9247` |
| No multi-pass or split symbol/string exists in the Linux DRI or the Windows UMD/ICD (`nm`/`strings` search for multipass, split, fallback) | absence evidence only |
| Windows D3D9 caps: `PixelShaderVersion=ps_2_0`, `PS20Caps.NumInstructionSlots=96`, `NumTemps=12`, `MaxPShaderInstructionsExecuted=96`, `MaxPixelShader30InstructionSlots=0`, no flow control | D3DCAPS9 template `VTGUModeDX32A.dll:0x1030fcc8` (`ioquake3-blocker-roadmap-re.md` 3.1) |

Interpretation: on Windows, an over-long shader is rejected before the driver
by the D3D9 runtime/HLSL compiler (ps_2_0 allows 64 arithmetic + 32 texture
slots), and the application must choose another technique.  On Linux GL the
vendor stack truncates silently.  There is no vendor precedent for splitting
or for software pixel execution; neither A99/A9A nor any second ALU bank is
ever written (`ioquake3-blocker-roadmap-re.md` 3.1).

## 2. Hardware options for intermediates

| Resource | Status | Relevance |
|---|---|---|
| R32F render target (A52 `f5`, private `203e`) and texture (A30 `f5`) | static: Windows RT lookup `0x1020a4c0`, Linux `stmLookupRenderTargetFormat_invi` `0x1a6c20`; FORMATOP: texture + RT, `NOFILTER`, `NOALPHABLEND` | only exact FP32 intermediate; one channel; nearest only; no blending |
| G16R16F (A52/A30 `f4`), R16F (`f0`) | static, same sources; channel order of `f4` unproven | FP16 (11-bit mantissa) is not exact for general intermediates |
| G16R16, L16 UNORM16; A2R10G10B10/A2B10G10R10 | static RT formats; post-render must be disabled for L16/G16R16 | range-limited [0,1]; useful only with a proven value range |
| A16B16G16R16(F), A32B32G32R32F | **absent** (Windows maps `D3DFMT_A16B16G16R16` to 0) | no four-channel high-precision target exists |
| MRT | `NumSimultaneousRTs=4`, `MRTPOSTPIXELSHADERBLENDING`, `INDEPENDENTWRITEMASKS`; manual A01[16:14]; oC0..oC3 compiler encoding closed (`chrome9-mrt-output-audit.md`) | up to 4 targets; RT1..RT3 state and memory safety not recovered; both vendor GL ICDs expose one draw buffer |
| Depth output | `mov oDepth` sets object `+0x78`, copied to context byte `+0x4e` bit 7 by `stmUpdatePS_inv` (`via-5.75-shader-re.md`) | one clamped [0,1] scalar at D16/D24 precision, coupled with the depth test; not a general carrier |
| Depth texture X8D24/S8D24 (A30 `fe`) | FORMATOP depth + texture; nearest only | could read back a 24-bit [0,1] value; same range limitation |

Therefore a generic split can carry at most one exact FP32 scalar per
target; with unqualified MRT, four scalars per pass.  RGBA8 packing of FP32
needs an exponent encode/decode (tens of records) or a proven range, so it is
not a generic carrier.

## 3. Compiler options and achievable record count

### Exact inventory (regenerated)

```text
fadd 176 (644 lanes)   fmul 134 (488)   fneg 105 (389)   ffract 40 (152)
fdot3 56               fdot4 4          fge 28 (scalar)  b2f32 28 (scalar)
fabs 12 (36)           fmax 8 (28)      fmin 4 (12)
vec4 41   vec3 21   vec2 8
```

Chrome9's NIR options (`chrome9_screen.c:268-283`) do not scalarize, so the
40 `ffract` are already vector operations: 152 lanes / 40 ops = 3.8 lanes per
op.  Per octave they are floor(v+…), mod(i), three permute mods, three
gradient floors and floor(b0)/floor(b1) = 10.  "4 scalar FRC to one vec4
FRC" therefore saves at most 2 of 40 records.

### Lane-level lower bound

A native ALU record performs one operation on at most four lanes; the best
case is a MAD lane (one MUL and one ADD).  Counting dot products as their
MUL/ADD lanes and materialized `step()` as SUB+CMP:

```text
MUL lanes  488 + 56*3 + 4*4             = 672
ADD lanes  644 + 56*2 + 4*3 + 28 (fge)  = 796
other      152 FRC + 28 CMP + 36 ABS + 28 MAX + 12 MIN = 256
records >= (672 + 796)/8 + 256/4 = 183.5 + 64  ->  248
```

This ignores constructor MOVs and assumes every MUL fuses and every record
is full, so it is not reachable.  It is 2.6 times the 96-record store.

### Best realistic schedule: corner-transposed ("SoA") octave

Simplex noise evaluates four corners.  Rewriting the lanes so each register
holds one quantity for all four corners turns the four `dot(p_k,p_k)`, four
`dot(x_k,x_k)` and four `dot(p_k,x_k)` into 3-record MUL/MAD chains, removes
the `b0/b1` and `a0.xzyw` shuffles, and makes the transposed corner indices
`(0,i1.c,i2.c,1)` the same vectors that feed `permute`.  Native arbitrary
selectors (0..3, ONE, ZERO; `glmark2-generic-fragment-backend-plan.md`)
allow building them with writemasked MIN (`.xy`) and MAX (`.zw`) records
without extra MOVs.  With `s=v+dot(v,1/3)` and `v` hoisted to varyings:

| Octave step | Records |
|---|---:|
| `i=floor(s)`: FRC, SUB | 2 |
| `x0=v-i+dot(i,1/6)`: DP3, SUB, ADD | 3 |
| `g=step(x0.yzx,x0)`: SUB, CMP; `l=1-g` | 3 |
| transposed `Ix,Iy,Iz` via masked MIN/MAX | 6 |
| `mod(i,289)` (NIR lowering: MUL, FRC, SUB, MAD) | 4 |
| three `permute` (MAD, MUL, 4-record mod) + stage adds | 23 |
| `j`, `x_`, `y_` floors | 10 |
| `x`, `y`, `h=1-|x|-|y|` | 6 |
| `sx`, `sy` (floor*2+1), `sh`, `ax`, `ay` | 9 |
| `taylorInvSqrt(ax²+ay²+h²)` | 4 |
| corner offsets `X,Y,Z` | 6 |
| `m=max(0.6-Σ,0)`, `m⁴` | 6 |
| `(ax·X+ay·Y+h·Z)·norm`, DP4, ×42 | 6 |
| `abs`, weighted accumulate | 2 |
| **per octave** | **90** |

Four octaves: about **360** records (≈300 if `mod(x,y)` may be rewritten as
`y*fract(x/y)` and constant factors reassociated).  Without hoisting, add
three records per octave.  The current backend lowers one NIR vector op per
record with no transposition; its per-octave cost is roughly 120 records
including constructor moves, so even a single octave does not fit today.

### Generic passes worth implementing (for all shaders)

In `chrome9_context.c`, ahead of the builder (`chrome9_ps_append_instruction`
`:2632`, ALU switch around `:3429`):

1. **Scalarize then SLP-vectorize with a record cost model.**  Run
   `nir_lower_alu_to_scalar`, then a Chrome9 packer that groups isomorphic
   scalar ops into ≤4-lane records, choosing lane positions to minimize
   gathers (arbitrary selectors are free; writemasks are free).  This
   subsumes the dot-product transposition (a group of four independent
   `fdot3` whose results form a vec4 becomes MUL+MAD+MAD).
2. **MUL+ADD contraction** into the `B + A*C` datapath where NIR marks the
   ops non-exact.
3. **Affine hoisting.**  An expression affine in interpolated inputs with
   uniform/constant coefficients is exact (in real arithmetic) under
   perspective-correct interpolation, because barycentric weights sum to one.
   Chrome9 runs the vertex stage in software (`draw_create_no_llvm`,
   `chrome9_context.c:6564`), so extra outputs cost CPU time only; the limit
   is the 8 fragment inputs.  Recompute varying packing after hoisting.
4. **Uniform folding** through the existing draw-time specialization
   (`chrome9_specialise.c`); for terrain noise `time` and `uvScale` are set
   once, but the savings are small (one lane per octave).
5. **Fail closed** at 96/32/12 exactly as now, then hand the draw to the
   fallback instead of rejecting triangles (`chrome9_draw.c:2091-2113`
   currently counts `rejected_triangles`; the height map stays unrendered).

## 4. Fallback designs

### 4a. CPU fragment fallback through llvmpipe (recommended first)

Design:

- Build llvmpipe into the Chrome9 Gallium library (`-Dllvm=enabled`; the T2
  sysroot already provides LLVM 23.1.1, see the sibling build log
  `mesa-26.2.2/objs-m9x-llvm/meson-logs/meson-log.txt`).  Current Chrome9 builds use
  `-Dllvm=disabled -Ddraw-use-llvm=false`.
- Lazily create one llvmpipe screen/context per Chrome9 context (null
  sw_winsys).  Keep the CSO templates and a clone of every fragment shader's
  NIR in the Chrome9 state objects so they can be re-created in llvmpipe.
- At draw time, when `ctx->fs->fragment_program` is unsupported
  (`chrome9_draw.c:271-272`) or the compiled program exceeds 96/32/12, route
  the whole draw (vertex, raster, fragment, depth, blend) to llvmpipe.
- Resources: Chrome9 BOs are linear, cached shmem with a persistent CPU map
  (`chrome9_resource.c`, `bo.map`; level 0 starts after one guard page).
  llvmpipe's `resource_from_user_memory` (`lp_texture.c:893`) imposes its own
  layout: row stride `align(align(w,4)*cpp, cacheline)` and height aligned to
  4.  Alias `bo.map + level_offset[0]` only when stride and 4-row padding fit
  inside the level; otherwise use an llvmpipe staging resource and row copies.
  The terrain height map (256×256 RGBA8, stride 1024) aliases directly.
- Ordering: flush Chrome9 batches and wait for the fences of every BO the
  draw reads or writes before CPU access (the existing CPU-map
  synchronization); after CPU writes, the kernel's cached-shmem flush on
  submit (`chrome9_engine.c:405`) publishes them to later GPU draws.  Verify
  that this flush covers render-target and texture BOs, not only the ring.

Correctness: llvmpipe is Mesa's reference rasterizer with IEEE FP32, so the
result is the Mesa reference image.  Mixed-path seams (CPU- and GPU-rendered
primitives sharing edges in one target) depend on identical fill and pixel
center rules; this does not arise for full-target passes such as terrain
noise, but must be tested for partial fallbacks.

Cost for terrain noise: 256×256 = 65,536 fragments per frame, two triangles
(`make_grid(1,1)`), no depth, no textures.  About 1,700 scalar lane-ops per
fragment map to about 430 four-wide SSE operations per fragment; SSE3
(`nocona` target) lacks `roundps`, so 152 FRC lanes cost several instructions
each.  Estimate 550--650 instructions per fragment, 36--43 M per frame, i.e.
roughly 25--35 ms single-threaded at 1.2--1.6 GHz, and 10--20 ms with
llvmpipe's tile threads on two to four cores.  Add one GPU/CPU
serialization per frame (the normal-map pass reads the height map).  The CPU
model is not recorded in this repository; confirm it before relying on these
figures.  Softpipe interprets TGSI per quad and is typically 20--50 times
slower than llvmpipe, about 0.5--1.5 s per frame here.

### 4b. Automatic GPU multi-pass splitting (optional optimization)

Design: partition the NIR DAG into passes each within 96 ALU, 32 TAU and 12
temporaries, with at most K live-out FP32 components per cut (K=1 with
R32F; K=4 with four R32F MRT targets once qualified).  Recompute cheap values
(interpolated inputs, uniforms, hoisted affine terms) in every pass instead
of storing them.  Each earlier pass writes a driver-owned R32F target; each
later pass reads it with a nearest TEX at its own pixel (needs a window
position input: the vPos candidate or a generated varying from the software
vertex stage) and ping-pongs two targets.  Only the last pass performs KILL,
depth, stencil, blend and the application color write.

Terrain noise has natural one-scalar cuts: the weighted sum `n` after each
octave.  With a transposed compiler (about 90 records per octave) it becomes
four passes of ≤96 records plus three R32F reads/writes of 256 KiB each.
With the current backend each octave would need two passes and a mid-octave
cut of several vec4 values, which requires MRT.

Correctness conditions:

- Overlapping primitives within one draw break per-pixel intermediates (a
  later triangle overwrites the value an earlier one needs).  Since the
  vertex stage already runs on the CPU, split each draw into batches with
  disjoint screen-space bounding boxes; batches pay all passes each.
- Intermediate passes must use the same coverage as the final pass
  (identical geometry and raster state, no depth/stencil writes).
- R32F render and nearest sampling, R32F exceptional values, the
  fragment-position input and render-to-texture cache transitions are all
  unqualified on silicon today.

Cost: about 4 × 65,536 × 92 ≈ 24 M ALU records per frame plus three TAU
reads per fragment.  Chrome9 pixel ALU throughput has not been measured; at
a hypothetical aggregate of one record per clock at 200 MHz this is about
120 ms, at four records per clock about 30 ms.  A fill-rate measurement with
a long straight-line program is required before choosing 4b over 4a.

## 5. Recommended implementation order

1. Replace triangle rejection for non-native fragment programs with the
   llvmpipe draw fallback (4a); gate with a host test that a >96-record
   program produces the llvmpipe image and a native program never takes the
   fallback.
2. Enable the terrain scene end-to-end with the height pass on the CPU and
   the remaining passes native; measure per-pass times.
3. Implement scalarize + SLP packing, contraction and affine hoisting (3.1-3.3);
   track how many glmark2 fragment programs leave the fallback.
4. Only if measured CPU time dominates: qualify R32F target/sampling and a
   position input, then add the multi-pass splitter (4b), keeping 4a as the
   fallback for programs the splitter cannot partition.

## Open questions

- Exact target CPU model and core count (for the llvmpipe estimate).
- Whether a libLLVM dependency is acceptable in the target image; if not,
  softpipe is correct but does not meet the frame-time goal for this shader.
- Chrome9 pixel ALU throughput (records per clock and engine clock).

Copyright (C) 2026 René Rebe <rene@exactco.de>
