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
`CHROME9_PS_NEG_SLOTS` enables A NEG on all ops and B NEG on ADD, SUB,
MAD and CMP; see section 6 for the qualification.

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
  `CHROME9_PS_NEG_SLOTS` (2891 pass, 267 fail).  NEG slots only shortens
  two cold ioquake3 programs (30 to 28, 47 to 45 records), so it stays off
  by default until a directed rung exercises every op/slot pair.
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
