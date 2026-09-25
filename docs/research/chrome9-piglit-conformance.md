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
| `46cfd50` | Local arrays become SSA before `nir_lower_int_to_float`, which otherwise turned a CSE-shared index constant into float bits (`m[1]` -> `m[0x3f800000]`); the link-time inline-uniform list is kept when the state tracker finalizes a variant again; the texture-alias check applies only to the fixed two-texture ioctls. | 368-test fs indexing list: 59 fail -> 0 (fs-temp-mat*-col-row-rd, fs-varying-*mat*, glsl-fs-vec4-indexing-temp-*-nested-loop-*, glsl-fs-texture-uniforms-share-same-unit) |
| `16180ff` | Depth views of S8_UINT_Z24_UNORM sample with the X8Z24 format (depth in bits 31:8, the stencil byte is ignored). | fbo-depthtex, texsubimage-depth-formats (+pbo) |
| `3ecd811` | Fragment programs fuse MUL+ADD into MAD and recombine SUB (refract 79 -> 57 ALU, terrain main 85 -> 72). | 19 fs built-ins (matrix products, outerProduct, mod, radians) fail -> pass, none regress in 1112 fs tests |
| `34b1ffa` | Smooth lines render in llvmpipe; draw passed them unwidened to the triangle-only vbuf render, which dropped them. | line-aa-width, line-smooth-coverage, line-smooth-stipple, point-line-no-cull |

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
- Smooth lines: fixed in `34b1ffa` (AA lines were dropped, not culled).
  `line-flat-clip-color` also fails in pure llvmpipe with
  `MESA_GL_VERSION_OVERRIDE=2.1`.
- `depth-tex-modes`: one draw fails with EINVAL in DRAW_VERTEX_PROGRAMS
  (independent of the depth format).
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

## 2026-09-25 afternoon: indexed arrays and texture formats

| Mesa commit | fix | tests |
|---|---|---|
| `ea70e83` | TCL keeps selects scalar (vector selects over a0-relative rows picked wrong lanes on silicon) and folds selects on inlined control uniforms. | vs-temp-array-*-col-row-wr, vs-uniform-*mat4*-rd |
| `2c236d6` | TCL writes whole colour and varying registers; lanes NIR left undefined are zero. The kernel rejects partial varyings. | texcoord-array, glsl-texcoord-array-2 |
| `fedbe6d` | `prefer_imm_arrays_as_constbuf = false`. The 32 fragment constant rows cannot hold indexed constant arrays. | fs-vec4-const-array-indirect-access-* |
| `706090d` | Mipmapped render targets are cleared on the GPU (DESTINATION_EXTENDED). | glmark2 refract 6 -> 7 fps |
| `d00d9cb` | NPOT DXT mip chains and DXT cube maps are created (sampler_unsupported) and sampled in llvmpipe. | s3tc-errors, texwrap/fbo-generatemipmap-formats S3TC, getteximage-targets CUBE S3TC |
| `c0016cc` | Z16 textures, views of formats without a sampler encoding, and mipmapped D24 textures go through llvmpipe; draws with a mipmapped depth attachment too. | getteximage-depth, texsubimage-depth-formats, fbo-depthtex |

Known remaining indexing failure:
- `vs-output-array-vec2-index-wr-no-unroll`. The fragment NIR the driver
  receives already lacks the `i < 1` arm of the packed-varying ladder. The
  fault is in the common varying lowering, not in chrome9.

Hardware limit:
- `gl_lightsource_indirect`. Indexing `gl_LightSource` in a fragment
  shader needs more than the 32 fragment constant rows.

## 2026-09-25 evening: remaining indexing and projected-texture failures

* `vs-varying-array-mat2-index-col-row-wr` (GLSL 1.10 and 1.20): the
  dynamic column/row write becomes about 40 scalar selects, each of them a
  four-record sequence whose result stays live in one lane of its own temp.
  The kernel maps every temp to one of 16 physical registers for its whole
  live range and returned -E2BIG (21 live temps).  Mesa now repacks lane
  values into shared temps before submission (`tcl_pack_temp_lanes`): lanes
  written by one instruction or read by one source stay together, a lane is
  written once (the kernel rejects redefinitions), groups go into live
  temps whose remaining lifetime matches theirs.  21 -> 11 live temps; the
  packed program passes the kernel lane rules and interprets identically to
  the original on random inputs (`~/tmp/tclpack/{pack.c,val.py,sim.py}`).
  Only programs with 12 or more live temps are repacked.
* `texwrap 2d proj` and `proj bordercolor`: the fragment program assumed the
  projector q == 1 and the per-draw domain check rejected the whole draw, so
  nothing was drawn.  A draw that leaves the proven domain now renders in
  llvmpipe; nothing of it was submitted to the hardware.
* `s3tc-*_gles2`: the binaries are not in the piglit build on the target
  (harness, not driver).
* `gl-2.0-large-point-fs` failed after any point sprite test: A2F (sprite
  enables per coordinate) was written only for sprite draws.  Kernel
  `7d077a9` writes it for every draw.

## 2026-09-25 night: conditionals, varyings and dropped draws

Full list (2102 tests) on kernel k126, Mesa p69: 1775 pass, 3 fail, 2
timeout, 1 warn, 5 not built (binaries missing on the target: GLES2/GLES3
and GLX-only tests; `run-chunk.sh` now reports exit 127 as NOTBUILT).

| Mesa / kernel | cause | tests |
|---|---|---|
| `73a0655` | idiv was trunc(x * rcp(y)), one low for exact quotients (76/76 = 0); corrected by the exact remainder | vs-op-div-*, vs-op-assign-div-* (10) |
| `2ff7b5a` | TCL SGN reads source lane w only (silicon: sign((-5,5,-0.5,0.5)) = (1,1,1,1)); emitted per lane | glsl-vs-sign, vs-sign-times-sign |
| `7224d7e` | select as b + mask(a - b) cancels b beside a large a; now MAD(a, mask, MAD(-b, mask, b)), possibly infinite arithmetic operands clamped | vs-atan-* (4) |
| `10be88a` | pixel EXP2/LOG2/RCP/RSQ read one lane and replicate; vector forms split per lane | glsl-fs-exp2, glsl-fs-sqrt-branch |
| `9bf9e42` | NaN/Inf program constants (NaN folded from reading an unwritten varying) dropped the draw; NaN -> 0, Inf -> +-FLT_MAX | glsl-routing (most probes) |
| `d3bf93f` | edge AA blends coverage into the destination even with blending off | line-flat-clip-color smooth cases |
| `580a72e`, `ff0fa1a`, `5218a71` | unwritten varyings zero-filled; TEXn/VARn collision moved to a free fragment varying index; vbuf path fills undefined varyings, sprite coordinates from the wide-point stage | glsl-orangebook-ch06-bump, glsl-routing (non-TCL), arb_point_sprite-checkerboard |
| kernel `4dd77fc` | VS outputs after oT3 need A61[22:20] = 4 (`chrome9-varying-t4-re.md`) | glsl-max-varyings (2), glsl-routing |
| kernel `df442d4` | `trace_einval=1` logs the function:line of every validator -EINVAL | debugging aid |

Remaining: `gl-1.0-user-clip-all-planes` (clip-distance interpolation vs.
polygon edge at one pixel), `line-flat-clip-color` non-smooth cases (last
column of draw-module lines), `triangle-rasterization*` (random geometry,
seed-dependent, identical before and after), `gl-2.1-point_smooth` (warn),
`copypixels-draw-sync` (7.5 s, just over the 7 s run limit) and
`streaming-texture-leak` (5000 uploads of 4 MiB textures, over 120 s: kernel
`memset_io` zeroing of local objects plus tiled staging copy, about 170 MB/s).
