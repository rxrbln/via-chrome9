# Chrome9 fragment programs: register pressure, record count and modifiers

Copyright 2026 René Rebe <rene@exactco.de>

Measurements on VX900, ioquake3 `demo bench` timedemo at 1024x768, kernel
k142 (`b571e81`), 2026-09-26.

## 1. A90 HPSConFig: pixels in flight

A90[16:15] trades per-pixel registers for pixels in flight (manual p.94):
normal, double, triple and 4-time keep 16/12, 8/6, 5/4 and 4/3 texture
coordinates/temporaries.  The kernel picks the mode from the program's
active coordinate count and physical temporary high water
(`chrome9_fragment_resource_bucket`).  The debug parameter `ps_config_max`
caps the mode:

| mode cap | GL2 fps |
|---|---|
| 0 normal | 36.2 |
| 1 double | 39.5 |
| 2 triple | 40.3 |
| 3 4-time | 40.4 |

GL1 stays at 50.4 fps in every mode.  Most of the gain is normal to
double, so the target is at most 6 physical temporaries per program.

The Mesa IR peak (`chrome9_ps_ir_temp_pressure`) equals the kernel's
first-fit physical high water, because the UAPI translation is one record
per IR instruction.

## 2. Pressure-aware scheduling

`chrome9_schedule_ps_ir` used to run only above the 12-register hardware
limit.  It now also runs for programs above 3 temporaries, aiming for the
6/4/3 thresholds (512 bounded variants), and the result is taken only when
it reaches a better mode.  KILL is schedulable: it has no destination, is
a reachability root and precedes the colour writes.

ioquake3 GL2: the 62-record alpha-test program drops from 7 to 6 temps
(normal to double) and a 10-record KILL program from 4 to 3.  Neither is
hot, so fps is unchanged; the hot programs already use 2-3 temporaries.

## 3. Record count

The pixel pipeline is ALU-record bound, so shorter programs are faster.
The hot GL2 program (`texture * vertex colour`, the colour packed by the
TCL varying packer into T0.zw and T1.xy) was 7 records:

    TEX t1; MOV t2.xy = T0.zw; MOV t2.z = T1.x; MUL t0.xyz = t1 * t2;
    MUL t4.w = t1.w * T1.y; MOV t4.xyz = t0; MOV oC = t4

Three semantic IR passes make it 4:

- `chrome9_ps_ir_coalesce_lanes` handles multi-lane copies (an injective
  lane map, the producer writing exactly the copied lanes);
- `chrome9_ps_ir_split_gathers` substitutes a temporary gathered by MOVs
  from varyings/constants into its componentwise readers, splitting a
  reader per source combination when that saves records;
- `chrome9_ps_ir_merge_lanes` joins partial writes of one temporary by
  the same op on the same operands.

Result: `TEX t1; MUL t4.xy = t1.xy * T0.zw; MUL t4.zw = t1.zw * T1.xy;
MOV oC = t4`.  GL2 40.4 to 43.3 fps.  Every rewrite is validated by the
UAPI translation and reverted when it fails.

## 4. NEG source modifier

The ALU computes A*C+B; SUB is the ADD template with C = -1 (its C NEG
bit) and the subtrahend in A.  The A-slot NEG bit (record 1 bit 27) is
therefore the same datapath bit for every op, and B-slot NEG (record 0 bit
3) is exercised by the kernel's ABS lowering (select of C = x, B = -x).
C-slot NEG needs record 3 bit 20 as well and stays rejected.

Directed rung (`tools/re/chrome9_neg_rung.py`, 30 shader_runner tests,
three uniform sets each with positive, negative and zero lanes, including
MIN/MAX ordering on both sides of the negated value, DP3/DP4, CMP arms and
the scalar ops), Mesa p103 on VX900; every test was checked in the
semantic dump for the `-` on the slot under test and for zero rejects:

| op | slot | result |
|---|---|---|
| MOV (also as `fsat(-x)` and bare `-x`) | A | pass |
| MUL | A (also `a * -b` moved from C) | pass |
| ADD | A, B (`-(a + b)`) | pass |
| SUB | B, A (`-(a - b)`) | pass |
| MAD | A, B, A+B, `-(a*b+c)` | pass |
| MIN, MAX | A (also `min(a, -b)` moved from C) | pass |
| DP3, DP4 | A | pass |
| EXP2, LOG2, RCP, RSQ | A, all four lanes | pass |
| CMP | B (select arm) | pass |
| FRC | A | **fail**: FRC(-0.0) returns 1.0 |

FRC of a uniform -0.0 also returns 1.0 without any modifier, so the
failure is FRC's own sign-of-zero behaviour: a NEG on the FRC source turns
+0 into -0.  ALU results canonicalise the zero (`fract(min(-0, 0))`,
`fract(max(-0, -1))`, `fract(dot(-0, b))` and `fract(-(0 * b))` all give
0), so NEG elsewhere does not reach FRC as -0.  FRC keeps the MUL(-1) or
MOV path.

NEG is now the default on A of every op except FRC and on B of ADD, SUB,
MAD and CMP; `CHROME9_PS_NEG_SLOTS=0` restores MAD/CMP A only.  To use
the slots the lowering also

- moves a negated operand of a commutative op (MUL, ADD, MAD's factors,
  MIN, MAX, DP3, DP4) out of the NEG-less C slot,
- folds `-(single-use MUL, MAD, ADD, SUB)` into source modifiers of that
  record (NIR hoists fneg out of products, so `clamp(-(a*b))` used to be a
  MUL and a MUL by -1),
- lowers a remaining fneg as `MOV -x` (no immediate lane for -1), and
- keeps NEG on select arms: CMP B takes it, the C arm is settled into a
  temporary.

Rung totals: 45 records with NEG (default) against 58 with
`CHROME9_PS_NEG_SLOTS=0`, which matches Mesa p94b record for record.
llvmpipe passes all 30 tests.

## 5. Saturate

The screen sets `lower_fsat` (the vertex path has no saturate), so NIR turns
`clamp(x, 0, 1)` into MAX+MIN.  `chrome9_nir_form_fsat` re-forms fsat in
the fragment semantic optimizer after the algebraic loops; the backend then
fuses it into the producer or emits one saturated MOV.  ioquake3 GL2 fog
program 6 to 5 records, alpha-test programs 62 to 58 and 47 to 45 (none
hot, fps unchanged).

## 6. Qualification

- Full piglit list (2101): p90 1776 pass, no regressions against p84
  (triangle-rasterization flips with its random seed).
- `pg-fold.txt` (3315, fragment built-ins): p91 identical with and without
  `CHROME9_PS_NEG_SLOTS` (2891 pass, 267 fail).  p103 with NEG slots on by
  default (commit `1d885e1`): 2891 pass, 267 fail, no test changed against
  p93.  Full list p103: 1775 pass (p93 1774; only
  triangle-rasterization-overdraw flipped, seed-dependent).  ioquake3
  1024x768: GL1 50.4 / GL2 43.7 fps (p94b 50.6 / 43.7).  The host
  `chrome9-shader-nir-test` (run on the target: the host glibc is older
  than the build sysroot) has the same 12 failing cases before and after.
- fs-refract-vec{2,3,4} and glsl-1.10-built-in-matrix-state fail since
  `901a070`: the programs used to be rejected (bcsel) and ran on llvmpipe;
  on hardware the 16-bit fragment mantissa misses the tests' absolute
  tolerances (1e-5 on refract, large gl_ProjectionMatrixInverse values).
  Hardware precision, not a lowering error.

## 7. What bounds ioquake3 now

Resolution scaling (Mesa p93, kernel k142):

| | 320x240 | 640x480 | 1024x768 |
|---|---|---|---|
| GL1 ms/frame | 11.9 | 13.8 | 20.0 |
| GL2 ms/frame | 12.3 | 14.7 | 23.2 |

At 320x240 ioquake3 is single-core CPU bound, about 70% of it in the
driver.  The kernel's finite vertex cache was a linear 1024-entry array;
an interval tree (kernel `764eca5`) gives GL1 84.3 to 90.3 and GL2 81.6
to 86.1 fps at 320x240.  At 1024x768 the GPU bounds the frame (CPU work
overlaps), and sampling textures from local memory
(`CHROME9_LOCAL_TEXTURES=1`: 665 of 744 allocations local) changes
nothing, so GL1 is not texture-bandwidth bound.

PPC over one GL1 run at 1024x768: 3.8M pixels per frame (4.9x overdraw),
94% pass depth, one ALU record and 0.77 TAU per pixel, colour reads 80% of
colour writes (blended passes; glmark2 `build`, opaque, reads none).  GL2
has the same pixel and memory traffic with 2.1 ALU records per pixel;
disabling the linker's varying compaction (`MESA_GLSL_DISABLE_IO_OPT=1`)
halves that to 1.0 (the vertex colour no longer straddles T0.zw/T1.xy, so
the hot program is TEX + MUL oC) without changing fps: GL2 is no longer
ALU bound.  During the 1024x768 frames ioquake3 keeps about one core busy,
the same share per second as at 320x240, with a flat profile and no
busy-waiting, so its CPU time per frame grows with resolution.  The CPU
and the pixel engine share the UMA memory; the remaining limit is memory
bandwidth rather than any single shader or driver path.

## 8. Hardware TCL for every GL2 and glmark2 draw (2026-09-26)

Draw-time linear proofs (Mesa `b45ec96`).  Fragment programs that compare
a varying with zero, divide by a projector or threshold a product need
per-draw facts about the VS output lane (exactly 0 or 1, or within
[0, 1]).  A lane computed by the VS was unknown and the draw left hardware
TCL.  The TCL compiler now records a lane that is a sum of
coefficient x input and constant terms (coefficients are CB0 dwords or
literals) and resolves it per draw against the bound constants and vertex
formats.  `modelview * vec4(v, 1)` of an affine matrix has w exactly 1;
`u_VertColor * attr_Color + u_BaseColor` stays in [0, 1] for UNORM
colours.

| workload | before | after |
|---|---|---|
| glmark2 ideas | 76 fps, 6425 fallback draws | 138 fps, none |
| ioquake3 GL2 1024x768 | 43.4 fps, worst frame 255 ms | 45.9 fps, worst frame 77 ms, every draw on hardware TCL |
| glmark2 score | 132 | 134 |

glmark2 terrain samples its displacement texture in the VS; the hardware
has no vertex texture fetch, so the texels are fetched on the CPU per
distinct vertex and fed as a vertex input.  A 393216-vertex soup with two
lookups exceeded the upload limit on the first draw; it is now compacted
at once (Mesa `ddc2b4e`), so terrain has no fallback draw.  Terrain is
fragment-ALU bound: PPC counts 83M ALU records per frame, at 2.49 ns per
record 207 ms, the measured frame time.

## 9. Preshader (Mesa `97ace77`)

Uniform-only subexpressions a per-pixel instruction uses move into
constant registers evaluated per draw on the CPU (nir_eval_const_opcode on
the bound CB0).  Terrain's main program 65 -> 59 records (frame 218 ->
207 ms), jellyfish 82 -> 88 fps.  The values are FP32 exact instead of the
16-bit pixel ALU: pg-fold 2891 -> 3155 pass, 267 -> 3 fail (the three
remaining failed before too); full piglit unchanged.

Remaining terrain cost: varyings straddling vec4 slots after the linker's
scalar compaction (vViewPosition in T2.w/T3.xy) need gather MOVs; a
lane-level varying layout would save about 2-3 of 59 records.

## 10. TEXLDP, per-lane varying layout, tiled back buffers (2026-09-26)

**TEXLDP** (kernel `b03a024`, Mesa `3500904`): TAU operation 7 divides
the coordinate by its W lane.  A direct varying coordinate reaches the TAU
through the textured FE consumer, which carries three lanes unless W is
read: TEXLDP widens that varying's consumer to STRQ, otherwise the divisor
is the default 1 (first silicon run: every sample at s*q).  q in another
lane goes through a computed (s, t, 0, q) temporary.  Silicon
(`tools/re/chrome9_gl_texldp_prec.c`: 256-texel NEAREST ramps, q from
0.0123 to 100, sub-texel offsets 0.02..0.98, CLAMP and REPEAT over five
periods, constant and interpolated q, both axes): identical to the
RCP/MUL path except the neighbouring texel within about 0.02-0.1 texel of
a boundary (at most 92 of 8960 samples, one texel).  All projective piglit
tests pass; on by default.

**Per-lane varying layout** (Mesa `3577321`): the linker compacts
varyings lane by lane, so vectors the fragment shader builds can straddle
two slots.  The FS input lanes are regrouped so each such vector (also of
per-component operations) and each TEX coordinate lies in one hardware
varying; TCL stores and the software vertex path follow
`chrome9_shader::varying_layout`.  ioquake3 GL2 hot program 4 -> 2
records, terrain 59 -> 56; fps unchanged (not ALU bound).

**Tiled window back buffers** (kernel `f49dd0c`, Mesa `7604c93`,
`1195e70`): DRI3 back buffers (`PIPE_BIND_BACKBUFFER`, new) are tiled and
created with `GEM_CREATE_TILED`; importers read the layout with GEM_INFO,
the LINEAR modifier is not advertised while tiled sharing is on (implicit
layout path), KMS refuses tiled framebuffers so Present copies.
`tools/re/chrome9_glx_present_check.c` compares a per-pixel unique
pattern between glReadPixels and XGetImage: identical from 64x8 to
1000x700.

| 1024x768 | linear back buffer | tiled |
|---|---|---|
| ioquake3 GL1 | 50.1 fps | 54.6-55.0 fps |
| ioquake3 GL2 | 46.0 fps | 49.1-49.4 fps |
| glmark2 score | 134 | 142 |

**llvmpipe fallback on tiled local levels**: llvmpipe cannot render into
the tiled level in place, and CPU detiling through write-combined memory
cost 10 s in texture-packed-formats (576 fallback draws of a 700x620
window).  The engine now copies between the level and a linear GART
staging image (`chrome9_resource_copy_gpu`, NEAREST at texel centres),
and the llvmpipe copy is kept while the level's kernel `write_seq`
(GEM_BUSY, kernel `cfcd259`, advanced by every write fence) and CPU write
generation are unchanged; a device clear clears a kept copy alike (Mesa
`a8c1fc4`).  Uploads: texture-packed-formats 576 -> 1, tex3d-npot 7986
-> 1.  The write-back after each fallback draw remains (tex3d-npot 8.0 s
against 4.8 s with a linear back buffer).

**Rendering into the staging image** (Mesa `f55af3a`): each kept entry
now owns a linear staging image in local memory, and llvmpipe renders into
it directly through a handle alias.  The engine copies the level in only
when it changed and copies the image back after the draws; there is no
full-surface CPU copy per fallback draw.  A GART staging image needed a
cache flush of all its pages before every copy back (`clflush_cache_range`
10 % of tex3d-npot); local pages need none.  The copy back must pass the
real level height: the image is padded to 4 rows for llvmpipe blocks, and
a whole-image copy was refused for heights not a multiple of 4 (texwrap
failures in the first build).

CPU reads (`transfer_map`) of 64 KiB or more from a tiled local BGRA level
use an engine copy of the box into a linear GART image instead of
detiling write-combined memory (about 120 MB/s).  glReadPixels of the
1024x768 back buffer: 28 ms -> 15 ms; 512x512: 10.5 ms -> 6.1 ms.  Smaller
reads stay on the CPU, where the fixed cost of the copy and wait
dominates.  Reading from the kept staging image instead of the level
brought no gain; re-copying the level after a device clear instead of
clearing the kept image on the CPU was slower.

| test | linear back buffer | tiled, kept copy | tiled, staging render |
|---|---|---|---|
| tex3d-npot | 4.8 s | 8.0 s | 6.8-7.2 s |
| texture-packed-formats | 0.5 s | 3.4 s | 1.6-1.9 s |
| texsubimage | 4.6 s | 6.9 s | 4.9-5.6 s |

The fragment constant buffer is advertised as 128 registers (Mesa
`aa2d0ee`).  4096 registers let glsl-uniform-interstage-limits build
shaders large enough to exceed the runner timeout (subdivide 5: 2.3 s at
32, over 30 s at 4096, 3.7 s at 128); 128 still covers gl_LightSource[8].
