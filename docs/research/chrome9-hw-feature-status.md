<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 hardware 3D feature status

Status of every VX900 3D engine feature against the goal "all hardware
features implemented, tested on silicon and used by Mesa".  Tests named
`tools/re/chrome9_gl_*_smoke.c` run through the real GLX driver and compare
the hardware vertex path (`CHROME9_TCL=1`) with the software vertex path.

## Implemented and verified on silicon

| feature | kernel / Mesa | test |
|---|---|---|
| command ring, FE33 fences, asynchronous retirement, pipe fences | `85ad597` / `b7025cd` | glmark2 matrix |
| guarded shared surfaces (zero-copy DRI3 and scanout) | `22d0655` / `b929f9f` | glmark2 matrix, ioquake3 |
| explicit CPU access, non-snooped GART | `eb62c41`, `a16035c` | glmark2 matrix |
| hardware TCL: vertex programs, stage A inline, stage B buffers and 16/32-bit indices, native vertex formats, 64 draws per ioctl | `d538ba0`, `a37ee05` | `chrome9_tcl_oracle`, glmark2 |
| homogeneous clipping incl. near plane, guard band | `2541417` | `chrome9_gl_guardband_smoke` |
| 6 user clip planes | `eff46d0` / `e3afdf6` | `chrome9_gl_ucp_smoke` |
| two-sided colour (two culled passes; hardware selection does not work) | Mesa `fb6780a` | `chrome9_gl_twoside_smoke` 8/8 |
| native triangle strips and fans | kernel topology field, CAP2 bit 5 | `chrome9_gl_topology_smoke` 60/60 identical to lists |
| fragment programs, 2D textures, NPOT, repeat/clamp/mirror | | glmark2 |
| POT mip chains at the sampler-derived offsets | `9dd8151` / `50b6120` | `chrome9_gl_mip_smoke` 68/68 |
| depth D16/X8Z24, stencil, all depth functions, blending, alpha test | | glmark2, stencil/depth smokes |
| KIL, fragment position, pixel-shader read-port rule | `229b18c` | `chrome9_gl_kill_zero_smoke` |
| cube maps (one object, vendor face layout, A18 mode 3) | `4b06bc2` / `58b8d2f` | `chrome9_next_oracle cube c1..c4`, `chrome9_gl_cube_dxt_smoke` |
| native lines (list, strip, loop) | `4f6eeda` / `3cfd219` | `chrome9_rq_rungs lines`, `chrome9_gl_points_lines_native_smoke` |
| native points (state size, sprites upper-left, VS point size via A61[22:20]), on by default | `a6a86da` / `71d7d78`, CAP2 bit 19 | rungs `points`, `pedge`, `sprite`, `psize`, `psizen`; GL smoke PASS |
| polygon mode LINE (triangles, strips, fans), on by default | `4f6eeda` / `71d7d78` | rungs `polymode`, `wire`; GL smoke PASS |
| vertex a0 relative addressing (ARL, indirect uniform reads) | `a6a86da` / `71d7d78`, CAP2 bit 14 | `chrome9_gl_arl_smoke` 8/8 native |
| blend equations SUBTRACT, REVERSE_SUBTRACT, MIN, MAX | `a6a86da` / `71d7d78`, CAP2 bit 18 | rungs `blend`, `blendalpha`; piglit blendminmax, blendsquare |
| separate RGB/alpha blend equations (EXT_blend_equation_separate) | Mesa `cdb3b5e` | piglit gl-1.0-blend-func |
| constant blend colour (CONST_COLOR/ALPHA and inverses, A38..A3F), on by default | `5f9fb6f` / `cdb3b5e`, CAP2 bit 20 `blend_color` | rungs `blendconst`, `blendstale`; piglit gl-1.0-blend-func pass in 19 s (llvmpipe: timeout) |
| fragment TEX with LOD bias (TEXLDB, TAU op 3, bias in coordinate W), on by default | `dbadc6e` / `00785c4`, CAP2 bit 21 `texture_bias` | rung `texbias`; piglit glsl-lod-bias, glsl-fs-texture2d-bias |
| exact `ftrunc` and `fceil` in the PS (FRC of \|x\| only) | Mesa `00785c4` | piglit glsl-fs-integer-multiplication, fs-ceil-* |
| sampling depth of S8_UINT_Z24_UNORM (D24S8) textures with the X8Z24 sampler format | Mesa `16180ff` | piglit fbo-depthtex, texsubimage-depth-formats (+pbo) |
| render to cube faces and mip levels (guarded shadow per image when no direct view fits), base-level views | Mesa `d4eb87d` | `chrome9_gl_rtt_smoke` 8/8 |
| direct render targets at non-zero levels and layers (256-byte aligned window, `destination_offset`), on by default | `77ca263` / `d298db6`, CAP2 bit 22 `destination_offset` | rungs `dstoff`, `reject`; `chrome9_gl_rtt_smoke`; `chrome9-mip-destination-re.md` |
| GPU mipmap generation (level L from L-1 of one object, 4 NEAREST taps), on by default | Mesa `d298db6` | rung `mipgen` (within 1 LSB), piglit fbo-generatemipmap*, refract 7 -> 9 fps |
| DXT1/3/5 incl. POT mip chains | `4b06bc2` / `58b8d2f` | `dxt d1..d6` (d2/d5 within 3 LSB), `m1`/`m2`, `chrome9_gl_cube_dxt_smoke` |
| 2D COPY_RECT, KMS scanout, hardware cursor | | Xorg |
| coarse-Z depth rejection (A01 bits 13/9, A29/A2A, 16-byte entry per 8x8 block), module `coarse_z` (default off: ioquake3 about 1.6 % slower, no glmark2 gain) | `dcccfc5` / Mesa coarse-Z, CAP2 bit 23 | `chrome9_coarsez_rung` (image identical, 39.3 -> 24.5 ms for hidden quads), back-to-front GL layers 55.8 -> 31.3 ms |
| edge antialiasing (A01 bit 5): one-pixel smooth lines, blend-off polygon smooth, on by default | `64242a2` / Mesa, CAP2 bit 25 | `eaa` probe, piglit line/polygon subsets unchanged |
| fence interrupts (CR 0x204 bit 5; internal DP status 0x1280 bits 13/11/9 acknowledged), on by default | `a0858ab` | 47001 interrupts, no foreign source (`chrome9-fence-irq-re.md`) |
| Fast-Z clear (A27 index in X8Z24 depth bits 10:8, A14/A15 clear data), on by default | `5ca17f7` / Mesa Fast-Z, CAP2 bit 24 | rung `fastz`, 24-bit FBO probe identical; Z16 windows do not use it |
| pixel-shader depth output (DEST_DEPTH directly before the colour write, A8A 8a820000), on by default | `42677af`, `2440737` / Mesa `7146ff9`, CAP2 bit 26 | `fd` probe, piglit fragdepth tests (`chrome9-depth-output-audit.md`) |
| NaN primitive drop per draw (stage B B14) | `fd0f932` | raster self-test, `nan` probe |
| tiled depth (A12 bit 0) and tiled colour targets and textures (A51 bit 23, A22 bit 17), on by default, no linear fallback | `a9dfc98`, `fe20655` / Mesa `9a62ef4`, `6db49ba`, CAP2 bits 27/28 | rungs `tile`, `tileperf`; piglit texture/depth subsets unchanged (`chrome9-tiling-re.md`) |
| local frame buffer (SL) placement of depth buffers and render targets: GEM_CREATE_LOCAL objects in an SL pool (16 MiB up to the occlusion page), CPU mapping write-combined through BAR2, engine location code 00 in A51, A11, A29, A18 and the level entries; resources the CPU reads are moved to GART pages once | kernel GEM_CREATE_LOCAL, CAP2 bit 29 / Mesa `chrome9_resource_local`, `chrome9_context_demote_local` | refract 78.0 -> 64.1 ms (15.6 fps), glmark2 113; fd, sprite, blur and VS texture probes identical with and without |
| FE43 index fetch: stage B indices from a GART index arena instead of the command stream (queried draws inline), on by default | kernel `412444b` | vtf probe identical without inline indices; refract 64.1 -> 62.1 ms; piglit unchanged (`chrome9-fe43-index-fetch.md`) |
| depth-only passes on the hardware: a D24 depth buffer without colour buffers draws into a private scratch colour target with every colour store masked | Mesa `5904ec3` | glmark2 shadow 16 -> 91 fps, image equal to llvmpipe; stencil states the kernel path cannot take stay in llvmpipe |
| buffer renaming: GEM_BUSY (CAP2 bit 31) tells whether queued jobs only read an object; a synchronized write map then moves the buffer to idle spare or new storage when the copy is at most the mapped size | kernel `b7e0552` / Mesa `6921094`, `0cc6531` | glmark2 buffer map 46 -> 56, interleaved 58 -> 72 fps; ioquake3 opengl2 unchanged |
| multiple render targets: up to 4 colour targets (A58..A6B, A01[16:14]), oC1..oC3 records, on by default | kernel `8f02aef`, uAPI `b107d09`, CAP2 bit 30 / Mesa `1e35f02` | `mrt` probe 2..4 targets within 1 LSB of llvmpipe; piglit fbo-drawbuffers* 6 SKIP -> PASS (`chrome9-mrt-re.md`) |
| lower-left point sprite origin (FS variant flips t, draw module gets the upper-left rasterizer) | Mesa `2e5c349` | `sprite` probe: both origins, window and FBO, equal to llvmpipe, no TCL fallback; piglit point sprite subset |

## Emulated or falling back

| feature | today | plan |
|---|---|---|
| wide or stippled lines, smooth points, fractional sprite sizes | draw module turns them into triangles | no line-width register |
| smooth (AA) lines, also from polygon mode LINE | llvmpipe (Mesa `34b1ffa`) | no AA line hardware; draw's AA line stage is not installed |
| polygon POINT mode, unfilled quads/polygons, per-vertex edge flags | draw module | GL draws the clipped polygon's vertices; quads hide inner edges |
| vertex texture fetch | llvmpipe draw fallback | not in the hardware (below) |
| fragment programs above 96 ALU records | llvmpipe fallback | 96 is the store size in every vendor driver (overflow silently truncated); multi-pass splitting needs R32F targets (untested) and denser code (`chrome9-long-fragment-programs.md`) |
| quad strips, quads, polygons | decomposed into lists | stay decomposed (quad diagonal must match the draw module) |
| terrain fragment shader, glmark2 ideas (one FS above 96 records), some GL2 draws | llvmpipe fallback / software vertex path | per-case compiler work; cel runs on hardware since flagged arithmetic of finite operands counts as finite (32 -> 81 fps) |
| desktop blur | validation fails (sampler precision) | open |

## Not implemented

| feature | state |
|---|---|
| stage B rungs B8 (out-of-range ignore), B13 (A12 default lanes) | open (B14 done); B7 (several A24 chunks per job) is exercised by DRAW_VERTEX_PROGRAMS batching |
| per-segment state cache | patch ready (default off), rungs R2/R3 |
| vertex IF nesting | depth 4 by default, reported by GET_PARAM VS_FLOW_DEPTH (kernel `a5fee45`, Mesa `7d88b30`); most vertex IFs are flattened to selects before TCL emission |

## Texture sampler: border, GL_CLAMP, mirror once, 1D, shadow, LOD limits

| feature | kernel / Mesa | state and test |
|---|---|---|
| CLAMP_TO_BORDER with sampler border colour (A32 code 0, A35/A36 s1.10) | `b9d6f7d` / `0c2c83d` | on (CAP2 bit 16, `texture_border`); `chrome9_gl_border_smoke` exact, piglit texwrap `bordercolor` variants pass where the draw reaches the kernel |
| GL_CLAMP (NEAREST = clamp to edge, LINEAR = border + saturated coordinate) | Mesa `0c2c83d` | on; the state tracker's saturated TXD becomes TEX (no TEXLDD), LOD differs only on clamped edge samples; smoke exact |
| MIRROR_CLAMP_TO_EDGE (A32 code 7, mirror once) | `b9d6f7d` / `0c2c83d`, default on `cb20c55` | on (CAP2 bit 17, `texture_mirror_once`): smoke exact (nearest) / 1 LSB (linear) on the first runs after two fresh module loads; the one zero-sample run predates the SWIOTLB fix `018a504` |
| 1D textures as height-1 2D | Mesa `0c2c83d` | on; smoke exact |
| depth-compare samplers (ARB_shadow, shadow1D/2D/2DRect, ARB_fp shadow) | Mesa `0c2c83d`, `d00e5a2` | on; compare lowered in the FS per variant like the vendor GL; D24 reads raw / 2^24, the reference is scaled to match; LINEAR compare = one nearest sample (vendor); `chrome9-shadow-compare-re.md` |
| whole-level min/max LOD limits (A30) | Mesa `0c2c83d` | on; fractional LINEAR limits and other unsupported sampler states render in llvmpipe; piglit lodclamp* pass |
| NPOT cube maps | Mesa `d00e5a2` | llvmpipe (`sw_only` view); the cube sampler takes POT faces only |

## Not present in the hardware

Per `chrome9-queries-msaa-3d-flow-re.md`: MSAA (only edge antialiasing,
A01 bit 5; no multisample capability in the vendor D3D caps), pixel-shader
flow control (PS 2.0 without flow control; Mesa keeps unrolling and
if-conversion), GPU timer queries (no GPU clock), wide lines (no width
register; the draw module expands them), vertex texture fetch (vs_2_0 only,
no vertex sampler or fetch opcode), pixel shader programs above 96 ALU
records (97 locks up the engine, rung `psstore`).

Fragment ALU precision: 16 mantissa bits ((1+2^-k)-1 exact for k <= 16,
zero for k >= 17); the vertex stage is FP32.  piglit fs-op-* misses at
1e-5 tolerance are hardware behaviour.

Silicon evidence for the 2026-09-25 batch: `chrome9-queries-msaa-3d-flow-re.md` §12.
