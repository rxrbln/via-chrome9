<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 performance work, 2026-09-24

Targets: glxgears > 100 fps, ioquake3 q3dm1 800x600 GL1 > 60 fps, GL2 >
30 fps.  All numbers are on the VX900 target (VIA Nano X2 L4350, 2 cores)
with `CHROME9_TCL=1`, measured over short runs; ioquake3 fps is the
`cg_drawFPS` value in the screenshot after about 14 s.

## Measurement tools

* Kernel `profile=N` (0600): logs the average microseconds per fragment
  job phase every N jobs (entry, compile, map, gart, flush, build, append,
  gpu, verify, retire), the flush split (guard pages, full flushes with
  bytes and reason, DMA sync) and the wait split (FE33 fence, cookie,
  PAUSE, work drain).
* `tools/re/chrome9_fill_bench.c`: times full-target flat fills through
  DRAW_FRAGMENT_PROGRAM.
* `perf` on the target (`perf record -a -g`); module symbols resolve with
  `addr2line -j .text` against the deployed `chrome9.ko`.

## Findings and changes

| step | change | glxgears | q3 GL1 | q3 GL2 |
|---|---|---|---|---|
| start | stage A TCL | 36 | ~2 | ~2 |
| stage B | GEM vertex buffers, client arrays uploaded (kernel `d538ba0`, Mesa `a155dfa`/`b58338c`) | 36 | ~2 | ~2 |
| explicit CPU access | whole-surface clflush only after announced CPU access (kernel `eb62c41`, Mesa `80ac004`); each draw had flushed destination and depth twice (0.94 of 1.1 ms) | 68 | 11 | 8 |
| persistent kmap | one vmap per object lifetime (kernel `dc17c94`); map 73 -> 11 us, retire 24 -> 1 us | 73 | 13 | 10 |
| shadow reuse | one guarded shadow per drawable, lazy copy-in (Mesa `981f2e7`) | 79 | 13 | 10 |
| GPU clears | full-surface clears as one fragment-program rectangle (Mesa `2ae7fed`) | 84 | 13 | 11 |
| non-snooped GART | `gart_nosnoop=1` (kernel `a16035c`, default since the next commit) | 90 | 18 | 14 |
| shadow generation | skip the drawable copy-in when the shadow is current (Mesa `cd4b1f7`) | 90 | 19 | 15 |
| staging/guards | fill only the copied staging range, reuse verified guards (kernel `3d5a77f`) | 101 | 22 | 16 |
| written-only verify | guard verify/invalidate only for color and depth (kernel `7380570`) | 103 | 23 | 17 |
| TCL batching | DRAW_VERTEX_PROGRAMS, Mesa queue (kernel `a37ee05`, Mesa `863c0fc`, `...`) | 116 | 28 | 19 |
| Xorg on the same Mesa | Xorg started from `xlib-20260924` (explicit access, GPU clears, shadow reuse in the X server) | 151 | 32 | 21 |
| async retirement | last segment retires in a worker, pipe fences via GEM_CPU_ACCESS WAIT (kernel `85ad597`, Mesa `b7025cd`) | 306 | 33 | 22 |
| guarded shared | DRI3 backs and the scanout front keep the guarded layout, external offset 0 maps to PAGE; no render/sampler shadows, no publish copies (kernel `22d0655`, Mesa `b929f9f`, Xorg from `xlib-gs`) | 513 | 41 | 23 |

glmark2 default set, 3 s per scene, explicit CPU access versus explicit
plus non-snooped GART (`artifacts/2026-09-24/tcl/gclear/glmark2-explicit-vs-nosnoop.tsv`):
every validation result unchanged; build 36 -> 55, texture 36 -> 59,
shading 27..31 -> 35..45, bump 20..37 -> 26..60, pulsar 34 -> 54, desktop
shadow 20 -> 35, jellyfish 21 -> 31, shadow 16 -> 29, conditionals 26..37
-> 35..57, loop 13 -> 18, refract 4 -> 5, terrain 2 (llvmpipe fragment
fallback).

Fill rate (800x600 flat color, `chrome9_fill_bench`, 16 quads per job):
SF with snooped PTEs (bit 25) 55 ms = 141 Mpixel/s; local frame buffer
(`vram_target_test`) 22.4 ms = 343 Mpixel/s; SF with non-snooped PTEs
22.4 ms.  The 2.7x difference was the snoop, not the GART translation, so
render targets stay in system memory.  See
[chrome9-local-fb-placement.md](chrome9-local-fb-placement.md) for the
local placement bits (not needed for fill rate).

Other measurements:

* A small draw costs about 70-85 us of GPU time per job (FE33 fence wait
  with nothing else outstanding): the per-job state reload and tail.
* ioquake3 q3dm1 issues about 104 draws per frame (GL1) and 109 (GL2).
  At 400x300 it ran 22/17 fps against 13/11 at 800x600 (snooped), so
  roughly half of the frame was fill and half per-draw cost.
* Xorg composites the client window through the same driver; its jobs
  on implicit (shared) objects paid full-surface flushes (5.6 % of the
  system in `clflush`).  Shared DRI3 buffers now stay in the explicit
  protocol (Mesa `5266a7d`); objects that become KMS framebuffers revert to
  implicit (kernel `fb9265f`).  Xorg itself still runs the older Mesa.

Guarded shared surfaces (design: [chrome9-present-path.md](chrome9-present-path.md);
implemented as in-object guards with an offset translation instead of the
mapping-level guards proposed there, so every draw path stays unchanged):
glmark2 validation unchanged, build 74 -> 123, texture 86 -> 196, bump
normals 78 -> 154, pulsar 77 -> 171, desktop shadow 59 -> 114, shadow
41 -> 64, conditionals 75 -> 138
(`artifacts/2026-09-24/tcl/k44/glmark2.tsv`).

Measurement pitfalls: `CHROME9_TCL=1` is required (the default is the
software vertex path, GL1 12 fps); a reset `q3config.cfg`
(r_subdivisions 4, trilinear) changes the workload, the benchmark config
is `q3config.cfg.bak` on the target.

The black strips seen with hardware TCL were the mip layout (fixed, see
below).

## Next

1. Batch several draws into one job (silicon rung B7): the per-job
   overhead (map, GART, guards, verify, fence wait, about 200 us) is paid
   about 100 times per ioquake3 frame.
2. Asynchronous retirement so CPU preparation overlaps GPU execution
   (the system was 55 % idle during ioquake3).
3. Per-draw kernel compile/build caching (compile 18 us, build 43 us).

## Mip level layout (correctness, 2026-09-24)

The black strips in ioquake3 were mip levels read from the wrong address.
`tools/re/chrome9_gl_mip_smoke.c` fills every level with its own colour
and selects each level with a 64x64 repeating quad; the old layout (L1
after an internal guard page, 256-byte aligned levels) gave 69-80
mismatches on both vertex paths.  For POT textures the sampler ignores the
subtype-10 address table and derives each level directly after the
previous level's rows, `offset(L+1) = offset(L) + ALIGN(w * cpp, 32) * h`
(exact height, not the vendor allocator's `ALIGN(h, 8)`): 8-row padding
still failed every level after a 4-row level, 4-row padding every level
after a 2-row level, exact rows gave 0 mismatches.  The kernel now requires
exactly these offsets for POT textures and emits no table words for them;
NPOT textures keep 256-byte aligned table offsets.

## Status 2026-09-25

| step | change | glxgears | q3 GL1 | q3 GL2 |
|---|---|---|---|---|
| mip layout, strips/fans, cube/DXT | kernel `9dd8151`..`4b06bc2`, Mesa `50b6120`..`58b8d2f` | 519 | 38 (42 with `r_ext_compressed_textures 1`) | 26 |
| draw mapping cache | bounded by GART pages, O(1) lookup (kernel `5749595`); 89 % misses -> 99 % hits, GART phase 270 -> 19 us per job | 521 | 39 | 27 |
| software vertex buffer reuse | draw module vertex buffer kept across batches (Mesa `b790e39`); zero-faulting 2 MB per batch was GL2's top CPU cost | 499 | 41 | 28 |

Where the q3dm1 time goes (k62): 400x300 runs GL1 65 fps (15.4 ms) and
GL2 38 fps (26.3 ms) against 41 / 28 at 800x600, so about 12 ms per frame
is fill at 800x600 and about 12 ms is fixed per frame.  The kernel profile
shows per 25-draw job about 1.2 ms of CPU preparation and 3.2 ms GPU wait,
strictly serialised (one job in flight), with the system 55-65 % idle.

`tools/re/chrome9_gl_fillrate.c` (full-window quads, 800x600): flat 369
Mpixel/s, blending free, texturing 1:1 nearest 363, bilinear with
magnification 181, trilinear 153, 4x repeat nearest 224 (texture cache).
The fill part of the frame is therefore near the hardware rate.

The per-segment state cache (rungs R2/R3 on silicon) shows a full state
emission costs under 1 us of GPU time per draw (64 tiny draws: 83 us per
job full state, 40 us cached), so state re-emission is not the GPU cost;
the cache saves CPU (26 -> 21 us per draw) and ring space.  The texture
group cannot yet be skipped (R2 `tex` differs on silicon).

Next: several jobs in flight so CPU preparation overlaps GPU execution
(`chrome9-multi-inflight.md`), then the state cache for CPU time.

## Targets reached (2026-09-25)

ioquake3 q3dm1 `timedemo 1 +demo bench`, 800x600, `CHROME9_TCL=1`
(`~/tmp/c9timedemo.sh`), glxgears in a default window:

| step | change | glxgears | GL1 timedemo | GL2 timedemo |
|---|---|---|---|---|
| queue, 2 in flight | kernel `73e7e08` | 681 | (38-41 in-game) | (29 in-game) |
| NOWAIT uploads, rasterizer fix | kernel `ac9d780`, Mesa `beb3ed0` | | | |
| one-segment jobs, 8 in flight, 1 MiB ring, state cache on, seqno fences | kernel `6c823ec`, Mesa `f510d7d` | 633 | 56.9 | 35.3 |
| program cache, per-ioctl arena | kernel `79e9347` | 920 | 63.2 | 43.0 |

The in-game spawn view (heavy sky and fill, `cg_drawFPS`) shows 49 (GL1)
and 43 (GL2) fps.  What mattered, in order: CPU and GPU overlapping
(several jobs in flight, no synchronous waits for non-final segments,
unsynchronized uploads that do not wait, fences that do not wait for
later frames), then kernel CPU per draw (program cache, no large zeroed
allocations).  glmark2 validation is unchanged throughout.
