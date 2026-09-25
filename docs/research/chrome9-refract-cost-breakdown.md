# glmark2 refract: frame cost breakdown (2026-09-25)

Copyright 2026 René Rebe <rene@exactco.de>

glmark2 refract (default model: bunny, 69 451 triangles drawn non-indexed,
208 353 vertices per pass) at 800x600 runs at 9-10 fps (105-109 ms per frame)
with kernel 85370c6 and Mesa 99525d6.  The scene renders:

1. a 1600x1200 FBO (twice the canvas size) with an RGBA mipmapped colour
   texture and a `GL_DEPTH_COMPONENT`/`GL_UNSIGNED_INT` (X8Z24) depth texture:
   colour+depth clear, the bunny with front-face culling and a 3-ALU
   `vec4(Normal, 1.0)` fragment shader;
2. `glGenerateMipmap` of the 1600x1200 colour texture;
3. the canvas: colour+depth clear and the bunny with the 57-ALU + 3-TEX
   refraction shader (three dependent texture reads).

## Method

`glFinish()` bracketing does not attribute GPU time: a clear following a draw
takes about 60 % of that draw's time, independent of a 100-200 ms CPU sleep
in between, so per-operation timings are unreliable.  The numbers below are
whole-frame A/B deltas with a temporary skip switch (not committed) that
dropped one stage at a time.

| variant (800x600) | frame | delta |
|---|---:|---:|
| all stages | 105.6 ms | |
| no GPU mipmap generation | 86.6 ms | 19 ms |
| no FBO depth pass | 64.6 ms | 41 ms |
| no main pass | 56.8 ms | 49 ms |
| trivial main fragment shader (`CHROME9_DEBUG_FS_MARK=id`) | 76.7 ms | 29 ms |
| 200x150 canvas (FBO 400x300) | 30-34 ms | |

The depth-pass cost scales with the FBO area (27 ms at a 1200x900 FBO, 42 ms
at 1600x1200): about 8 ms of geometry plus about 18 ns per FBO pixel.

## Fill-rate probe

`~/tmp/fill/fill.c` (full-screen quads into a 1600x1200 mipmapped FBO,
TCL path):

| fragment program / depth | rate |
|---|---:|
| constant colour, 1 ALU, no depth | 715 Mpix/s |
| `vec4(varying, 1.0)`, 3 ALU, no depth | 260 Mpix/s |
| 1 ALU, D24 depth test and write | 120 Mpix/s |
| 1 ALU, D16 depth test and write | 137 Mpix/s |
| DRAW_FRAGMENT_PROGRAM clear, colour only | 690 Mpix/s |

Without depth, throughput follows the ALU record count, about 750 M ALU
records per second.  With a depth buffer, the linear system-memory depth
read and write (non-snooped GART) limits fill to about 1.4 GB/s.  Folding the
depth pass shader from 3 ALU to 1 ALU therefore did not change the refract
frame time: the pass is depth-bandwidth bound with its back-face overdraw.

Vertex throughput: the triangle setup limit is about 5.5 Mtri/s; the two
bunny passes cost about 25 ms at a 200x150 canvas.

## Changes from this analysis

* Kernel `85370c6`: finite-check cache for explicit vertex objects and
  announced-range flushes (refract 119 -> 109 ms).
* Mesa: the terminal `MOV oC0, rN` folds into rN's producer, and MOVs of
  literal 1.0 merge into the MOV defining the other lanes (literal ONE
  selector).  `vec4(n, 1.0)` is now one ALU record (260 -> 690 Mpix/s in the
  probe); glmark2 gouraud 154 -> 176, conditionals 153 -> 174, loop/function
  100 -> 107 fps.
* Mesa: long non-indexed TCL draws from unchanged buffers index repeated
  vertices seen at most 64 positions earlier, so the post-transform cache
  reuses them.  Unbounded reuse was slower (126 ms): far references fetch cold
  vertices and force 32-bit index ranges.  The map is built only on the
  second draw of unchanged buffers, so the glmark2 buffer scenes are
  unaffected.  build (vbo) 208 -> 231, bump high-poly 77 -> 99 fps.
* Rejected: GPU mipmap generation with one bilinear tap instead of four
  nearest taps is slower (125 vs 106 ms).

glmark2 score 101 -> 106.

## Limit

A >15 fps refract needs a frame of at most 66 ms.  Geometry (about 25 ms at
the setup limit), the 1600x1200 depth pass with overdraw (about 34 ms of
pixel work) and the 57-ALU main shader (29 ms) already exceed that before
clears, mipmaps and presentation.  The remaining hardware lever is coarse-Z
rejection and Fast-Z clear
([chrome9-fastz-coarsez-boundary.md](chrome9-fastz-coarsez-boundary.md)),
whose metadata allocation and lifetime are still being reverse engineered.

## Update 2026-09-25 evening

Refract now takes 78 ms (12.8 fps) at 800x600: tiled colour/depth, the
LOG2/MUL/EXP2 `pow(x, 100.0)` (main pass 60 -> 55 records) and the 4 MiB
command ring (kernel; ring wraps per frame 1 -> 0.11).  The frame is GPU
bound: only about 35 % CPU busy, and after the ring change the submitter
waits on the full 8-job queue about 7 times per frame.

| measurement | result |
|---|---|
| every fragment program replaced by a constant (`CHROME9_DEBUG_FS_MARK=id`) | 53.7 ms |
| `glGenerateMipmap` of the 1600x1200 target alone (`~/tmp/mipb`) | 3.9 ms (clear 2.5 ms) |
| 400x300 / 200x150 canvas | 44.4 / 29.6 ms |
| coarse-Z on | 82.6 ms (no gain) |
| GPU present copy (`CHROME9_GPU_PUBLISH=1`) | 79.5 ms (no gain) |

The fragment programs cost about 25 ms; geometry (two 69k-triangle
passes), the 1600x1200 depth pass and the clears take the rest.

## Local frame buffer placement: 64 ms (15.6 fps)

The depth-tested fill is bandwidth bound in GART pages.  With the debug
knob `vram_target_test` colour writes to SL took the frame from 77.7 to
72.2 ms, and depth in SL (a temporary knob) to 63.9 ms.  The real
implementation (kernel GEM_CREATE_LOCAL, Mesa local placement of private
depth buffers and render targets) gives 64.1 ms.  The SL range is
uncacheable for the CPU (MTRR UC over 0xd0000000..0xdfffffff), so CPU
readers see uncached reads: VS texture prefetch and llvmpipe fallback
draws move their resources to GART pages once (terrain would otherwise
lose 259 -> 345 ms).

