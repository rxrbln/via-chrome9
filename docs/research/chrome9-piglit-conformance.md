# Chrome9 piglit conformance

Copyright 2026 René Rebe <rene@exactco.de>

This page tracks piglit on the VX900. Each section gives the fixes that came
out of a run and the known failure classes that remain.

## Setup

- piglit is built from git on the host (`~/tmp/piglit-install`) and installed
  on the target at `/root/tmp/piglit`. The GL 2.1 subsets are
  `gl21-quick.txt` (5,660 entries) and `gl21-ext.txt`.
- Runner: `LIB=<libdir> [LOCKW=<s>] ./run-chunk.sh <list> <out.tsv> [first [last]]`.
  It runs one test at a time, 7 s each, under the GPU lock. Every test runs
  with `-auto -fbo`, `MESA_GL_VERSION_OVERRIDE=2.1` and `CHROME9_TCL=1`.
  `LOCKW` sets how long each test waits for the GPU lock (5 s by default).
- To run a single test: `bash /root/tmp/pgone.sh <libdir> bin/<test> [args]`.
- The X server blanks the screen after 600 s. Window-readback smoke tests then
  read black, so run `DISPLAY=:0 xset s off -dpms` first. Runs with `-fbo`
  are not affected.

## 2026-09-25 gl21-quick

| run | lib | PASS | FAIL | CRASH | TIMEOUT | SKIP |
|---|---|---|---|---|---|---|
| baseline | tcl-20260924/p1 (switched to p2 part way) | 3841 | 770 | 48 | 10 | 553 |
| non-passing tests rerun | tcl-20260925/p12 | +255 | 571 | 0 | 2 | – |

The combined result is 4096 passes out of 4670 tests that are not skipped
(87.7%), and no crashes.

A second rerun of the 574 remaining non-passing tests with lib p27 (all
fixes below, kernel with `018a504`) passed 200 more. The total is 4296 of
4670 (92.0%), with 0 crashes and 3 timeouts. Of the 370 failures, 290 are
`fs-*` built-in function tests that check float results against a 1e-5
relative tolerance. That is below the 16-bit fragment mantissa, so they
fail on this hardware (mult, div, add, sub, exp/log, sqrt and similar).

glmark2 (`chrome9_glmark_matrix.sh`, lib p27): 26 Success, 6 Unknown (no
reference image), and 1 Failure (desktop blur, a 1-LSB difference that
was accepted). No rejects and no kernel faults. The slowest scenes are
terrain at 2 fps and refract at 6 fps.

### Fixes

| Mesa commit | fix | tests |
|---|---|---|
| `4ae2e90` | `pipe->blit` on the CPU. Handles colour from any sampled format (compressed included), Z and S, scaling, flips, filters, masks, swizzle, scissor, render condition and alpha blend. | copytexsubimage, getteximage, copypixels, s3tc crashes |
| `939057c` | Scalarize `ddx`/`ddy` for the llvmpipe fallback. gallivm emits only one channel. | glsl-fwidth, texgen, derivs crashes |
| `242eb09` | `max_varyings` = fragment inputs (8). The bring-up value of 2 reported `GL_MAX_VARYING_FLOATS` = 8. | varying-packing crashes |
| `6393f06` | RECT sampler views. Draws to depth/stencil-only framebuffers go to llvmpipe. | fbo-stencil read/draw/copy/blit, copytexsubimage RECT |
| `98a9515` | Convert integer texture queries to float for llvmpipe (`txs` of the RECT lowering). | glsl-fs-texture2drect* |
| `528a54a` | Lower integer operations before the uniform key is inlined. The key held `ieq(1.0f bits, 1)`. | vs int comparisons |
| `f43db6a` | `lower_bitops`. `nir_lower_int_to_float` has no float form of shifts. | indexed temp arrays |
| `52f38bc` | Integer lowering for the llvmpipe fallback VS, because uniforms are float-encoded. | vs-op-*-int |
| `6650103` | `vs->indirect_temp_addr = false`. The vertex engine has no relative temp addressing; selects replace it and run in TCL. | vs-temp-array-* crashes |
| `f20cfd2` | Interpolate convergent varyings (`nir_io_always_interpolate_convergent_fs_inputs`). The FS accepts the noperspective mode that nir_opt_varyings assigns to them. | vs-op-* moved from llvmpipe to hardware TCL |
| `619ecd2` | TCL scalarizes vector equality reductions (`ball`/`bany`). Integer forms compare as floats. | vs-outerProduct-*-ivec, vector compares on TCL |
| `5698720` | llvmpipe fallback keeps integer fragment centres. The state tracker already added the half-pixel bias. | glsl-fs-integer-multiplication, glsl-arb-fragment-coord-conventions |
| `de8031c` | Views with swizzles the native decoders cannot express are sampled in llvmpipe instead of being rejected. | fbo-fragcoord2 |
| `e2fbfcd` | `max_texture_lod_bias` = 15. The sampler takes an s5.5 bias; 0 was below the GL minimum of 2. | minmax |
| `0c5f908` | llvmpipe renders into mip levels (a subimage shadow is its own level 0) and into odd-height targets. Guarded single-level layouts pad to whole 4-row blocks. `CHROME9_SW_DEBUG` names missing fallback state. | glsl-lod-bias |
| `60aade4` | Framebuffers with several colour buffers (GL_FRONT_AND_BACK): extra buffers are cleared through mappings, with channel masks, and draws go to llvmpipe. | gl-1.0-drawbuffer-modes, gl-1.0-front-invalidate-back |
| `dd69ac9` | The fixed fragment programs' source-format checks no longer reject generic programs. | s3tc-teximage, s3tc-texsubimage, compressedteximage, gen-compressed-teximage |
| `17a6625` | Hardware draws are scissored to the viewport. TCL clips only to its guard band. | triangle-guardband-viewport |
| `3f85f9f` | TCL flat-shades quads from their last vertex (quads do not follow the provoking-vertex convention). | clipflat |
| `c3bc0a9` | Blend states without a hardware encoding (logic op, MIN/MAX/SUB, some factors) go to llvmpipe. | gl-1.1-xor*, blendminmax; gl-1.0-blend-func is correct but takes >60 s until the factors are in hardware |
| `641ed72` | Polygon stipple: the pattern is kept and stippled draws go to llvmpipe. | polygon-stipple-fs |
| `41f1601` | Two-sided colour with visible face order goes to llvmpipe. Before, the post-TNL path dropped it. | interpolation-none-gl_Back* |
| `cdb3b5e` | Constant blend colour in the pixel engine (kernel `5f9fb6f`); EXT_blend_equation_separate advertised. Under `MESA_GL_VERSION_OVERRIDE=2.1` `glBlendEquationSeparate` raised GL_INVALID_OPERATION before. | gl-1.0-blend-func: timeout (>200 s in llvmpipe) to pass in 19 s; `--quick` 60 s timeout to pass in 1 s |
| `00785c4` | `txb` as TEXLDB (kernel `dbadc6e`); exact `ftrunc` and `fceil` from FRC of \|x\|. | glsl-lod-bias, glsl-fs-texture2d-bias, glsl-fs-integer-multiplication now on hardware |

### Hardware limits found

- **Fragment ALU precision.** It keeps exactly 16 mantissa bits, an
  s7e16-class FP24 format. With `a = 1 + 2^-k` uniform, `(a - 1) * 2^k` is
  exact for k ≤ 16 and 0 for k ≥ 17 (`~/tmp/st/prec.shader_test`). The vertex
  (TCL) path is FP32. The `fs-op-*`, `fs-sqrt`, `fs-exp` and similar
  built-in tests check a relative tolerance of 1e-5, below 2^-16. They fail
  by design of the silicon. These are the bulk of the remaining built-in
  failures (291 fs versus 37 vs).
- **FRC of negative inputs.** Just below an integer (e.g. -0.9999923)
  FRC returns 1.0, so `x - FRC(x)` is one too small there.  `ftrunc` and
  `fceil` therefore use FRC of |x| only; `ffloor` keeps NIR's 2-ALU
  `x - FRC(x)` and inherits this edge case for negative inputs.
- **Fragment temporaries.** The PS has 12 temporaries. glsl-max-varyings with
  8 varyings needs 14 and is rejected (`reject native compile ir=15
  temps=14`).
- **Back-colour selection.** Hardware BFC selection does not work (see
  chrome9-hw-feature-status.md). Two culled passes are used instead, or
  llvmpipe when the face order is visible.

### Open issues

- **Fixed: intermittent lost jobs** (all-zero, clear-only or draws-only
  readback). The cause was SWIOTLB bounce buffers for GEM objects above
  4 GiB, combined with non-snooped GART access. Kernel `018a504` places
  every GEM object below 4 GiB when the DMA mask is 32 bits. Repro:
  `tools/re/chrome9_swiotlb_repro.sh`; details in chrome9-per-draw-state.md
  section 4.3. On the VS sample, 246 of 250 now pass (81 failed before).
- **TCL virtual temporaries run out** on large indexed matrix arrays, for
  example `vs-temp-array-mat4-index-col-row-wr`. Each value takes one of 255
  virtual temps, and the builder does not reuse dead ones.
- **Missing sampler features:** border wrap mode and colour, `GL_CLAMP` with
  linear filtering, shadow compare, and 1D textures. This is texwrap,
  glsl-fs-shadow* and tex-border (in progress).
- **Rectangle textures and `txs`.** The hardware FS rejects `txs`, so RECT
  sampling runs in llvmpipe.
- Done 2026-09-25: blend equations (`71d7d78`), constant blend colour and
  separate equations (`cdb3b5e`), `txb` and `ftrunc` in the hardware FS
  (`00785c4`).
- Smooth and stippled lines. Pure llvmpipe passes `point-line-no-cull` and
  `line-flat-clip-color`, but the chrome9 paths (including the llvmpipe
  fallback under chrome9 caps) cull, stipple or polygon-mode AA lines like
  polygons.
- `glsl-fs-bug25902`,
  and the uniform-initializer and out-parameter-indexing groups have not been
  triaged yet.

## Performance check, 2026-09-25

The ioquake3 timedemo "bench" at 800x600, kernel k86, measured with
`~/tmp/c9timedemo.sh`, which now calls `q3time.sh`:

| lib | GL1 | GL2 |
|---|---|---|
| tcl-20260924/p1 (baseline) | 65.0 fps | 43.4 fps |
| committed Mesa HEAD before 71d7d78 | 65.3 fps | 43.2 fps |
| p27 (all fixes in this document plus 71d7d78) | 65.3 fps | 43.3 fps |

An apparent drop to about 42 fps was a benchmark artefact. The old script
killed the `flock` wrapper, not ioquake3. The leftover instance rendered its
menu with `com_maxfps 0` for up to the 60 s timeout and took CPU from the
next run. `q3time.sh` now ends ioquake3 itself.
