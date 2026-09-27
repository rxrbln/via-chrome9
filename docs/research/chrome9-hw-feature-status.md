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
| two-sided colour (two culled passes; the hardware back-colour selection does not work on VX900: every vendor state and candidate tested, `chrome9-twoside-enable-re.md`) | Mesa `fb6780a` | `chrome9_gl_twoside_smoke` 8/8 |
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
| per-segment state cache (default on), stage A draws clear it | `98fa4fd`, `980d647` | `chrome9_state_rungs` R2 30/30, R3; piglit and ioquake3 equal or better than `state_cache=0` |
| tiled depth (A12 bit 0) and tiled colour targets and textures (A51 bit 23, A22 bit 17), on by default, no linear fallback | `a9dfc98`, `fe20655` / Mesa `9a62ef4`, `6db49ba`, CAP2 bits 27/28 | rungs `tile`, `tileperf`; piglit texture/depth subsets unchanged (`chrome9-tiling-re.md`) |
| local frame buffer (SL) placement of depth buffers and render targets: GEM_CREATE_LOCAL objects in an SL pool (16 MiB up to the occlusion page), CPU mapping write-combined through BAR2, engine location code 00 in A51, A11, A29, A18 and the level entries; resources the CPU reads are moved to GART pages once | kernel GEM_CREATE_LOCAL, CAP2 bit 29 / Mesa `chrome9_resource_local`, `chrome9_context_demote_local` | refract 78.0 -> 64.1 ms (15.6 fps), glmark2 113; fd, sprite, blur and VS texture probes identical with and without |
| FE43 index fetch: stage B indices from a GART index arena instead of the command stream (queried draws inline), on by default | kernel `412444b` | vtf probe identical without inline indices; refract 64.1 -> 62.1 ms; piglit unchanged (`chrome9-fe43-index-fetch.md`) |
| depth-only passes on the hardware: a D24 depth buffer without colour buffers draws into a private scratch colour target with every colour store masked | Mesa `5904ec3` | glmark2 shadow 16 -> 91 fps, image equal to llvmpipe; stencil states the kernel path cannot take stay in llvmpipe |
| buffer renaming: GEM_BUSY (CAP2 bit 31) tells whether queued jobs only read an object; a synchronized write map then moves the buffer to idle spare or new storage when the copy is at most the mapped size | kernel `b7e0552` / Mesa `6921094`, `0cc6531` | glmark2 buffer map 46 -> 56, interleaved 58 -> 72 fps; ioquake3 opengl2 unchanged |
| local framebuffers and shared local objects: KMS scans out of SL (CR48[7:6] = 00), local objects export as same-device dma-bufs, DRI3 back buffers and the X front buffer live in local memory; CPU mappings of the pool are write-combined (the whole-BAR uncached mapping is gone) | kernel `ac343fb`, `2ffad27` / Mesa `548b230` | glmark2 114 -> 131, ioquake3 1024x768 GL1 46 -> 50.5 fps, streaming-texture-leak >120 s -> 76 s; full piglit unchanged |
| R32F render targets and textures (A52/A30 `f5`), NEAREST only, float targets without blending/AA | kernel `b5f0b7c`, CAP2 bit 32 | `chrome9_rq_rungs r32f` F1-F4 (16 mantissa bits, saturation at 2^64, sampler returns (r,1,1,1)), `r32fmrt` F5 (2-4 R32F MRT targets exact), `r32freuse` F7 (rewritten carrier read back fresh); not reachable from GL (no RGBA16F/32F, R8/RG8); used by the fragment splitter's carriers |
| A90 HPSConFig pixels-in-flight modes chosen from physical temps (<= 6/4/3), with pressure-aware scheduling (KILL included) towards those thresholds; `ps_config_max` debug cap | kernel `b571e81` / Mesa `ed89f58` | ioquake3 GL2 normal 36.2, double 39.5, 4-time 40.4 fps (`chrome9-fs-register-pressure.md`) |
| fragment record reduction: multi-lane copy coalescing, gathered-varying substitution, lane merging, fsat re-formed after NIR's lower_fsat | Mesa `ed89f58`, `dc1f3f4` | hot ioquake3 GL2 program 7 -> 4 records, GL2 40.4 -> 43.3 fps; piglit and pg-fold unchanged |
| fragment NEG source modifier on the A operand of every op but FRC and on the B operand of ADD, SUB, MAD and CMP, default on (`CHROME9_PS_NEG_SLOTS=0` restricts it to MAD/CMP A) | Mesa `1d885e1` | directed rung `chrome9_neg_rung.py` 30/30 on silicon (FRC(-0) returns 1.0, so FRC keeps MOV/MUL); rung records 58 -> 45; piglit and pg-fold unchanged (`chrome9-fs-register-pressure.md` §4) |
| TEXLDP projective lookups (TAU operation 7), direct varyings widened to STRQ | kernel `b03a024`, CAP2 bit 33 / Mesa `3500904` | `chrome9_gl_texldp_prec`: equal to RCP/MUL up to a neighbouring texel within 0.02-0.1 texel of a boundary; projective piglit tests pass |
| tiled window back buffers: GEM_CREATE_TILED records the layout for importers (GEM_INFO), KMS refuses tiled framebuffers | kernel `f49dd0c`, CAP2 bit 34 / Mesa `7604c93` | ioquake3 GL1 50.1 -> 54.6, GL2 46.0 -> 49.1 fps, glmark2 134 -> 142; `chrome9_glx_present_check` exact |
| per-object device write count (GEM_BUSY write_seq): llvmpipe fallback keeps its copy of unchanged tiled local levels, engine-side detile | kernel `cfcd259`, CAP2 bit 35 / Mesa `a8c1fc4` | fallback uploads 576 -> 1 (texture-packed-formats) |
| PPC performance counters (HParaType 08h types 00h-17h) as a debug profiler: `ppc_profile` mask, one write-back per job, debugfs `chrome9_ppc` | kernel `7d38c66` | byte address aligned to 16 bytes; several fires per job lose writes or stop the engine |
| multiple render targets: up to 4 colour targets (A58..A6B, A01[16:14]), oC1..oC3 records, on by default | kernel `8f02aef`, uAPI `b107d09`, CAP2 bit 30 / Mesa `1e35f02` | `mrt` probe 2..4 targets within 1 LSB of llvmpipe; piglit fbo-drawbuffers* 6 SKIP -> PASS (`chrome9-mrt-re.md`) |
| lower-left point sprite origin (FS variant flips t, draw module gets the upper-left rasterizer) | Mesa `2e5c349` | `sprite` probe: both origins, window and FBO, equal to llvmpipe, no TCL fallback; piglit point sprite subset |

## Emulated or falling back

| feature | today | plan |
|---|---|---|
| wide or stippled lines, smooth points, fractional sprite sizes | draw module turns them into triangles | no line-width register |
| smooth (AA) lines, also from polygon mode LINE | llvmpipe (Mesa `34b1ffa`) | no AA line hardware; draw's AA line stage is not installed |
| polygon POINT mode, unfilled quads/polygons, per-vertex edge flags | draw module | GL draws the clipped polygon's vertices; quads hide inner edges |
| vertex texture fetch | llvmpipe draw fallback | not in the hardware (below) |
| fragment programs above 96 ALU records | llvmpipe fallback by default; `CHROME9_FS_SPLIT=1` runs them as multi-pass jobs through R32F carriers (Mesa splitter, silicon-verified: split results bit-identical across plans, <= 1 LSB from llvmpipe where the 16-bit ALU allows) | opt-in because llvmpipe runs the long programs found (terrain noise, 19 jobs) 14x faster than the pixel engine; 96 is the store size in every vendor driver (`chrome9-long-fragment-programs.md` §8, §9) |
| stage B index objects, A2C bound from the buffer size | kernel `f79a548`, Mesa `7fd63dd` | on (CAP2 bit 36): indices fetched raw through FE43 from the element buffer, A2C = largest vertex the vertex buffers hold (B8: an index beyond it ends the list), finite check once per buffer range (cached); no CPU scan or copy; draws with an occlusion query keep inline indices |
| stage A/B default inputs: a stride-0 (0,0,0,1) attribute is not fetched, its A12 bit is clear | kernel `745d209`, Mesa `18596ff`, CAP2 bit 38 | on; `chrome9_gl_vertex_default_check` (default input below and above the output count, 1-4 varyings) |
| depth functions GREATER, NOTEQUAL, GEQUAL; Z16 depth-only passes; unsupported depth states to llvmpipe instead of dropped | kernel `745d209`, Mesa `18596ff`, CAP2 bit 37 | on; `chrome9_gl_depth_only_check` 18/18 (NEVER renders in llvmpipe: A13 code 0 is the implicit LESS) |
| quad strips, quads, polygons | decomposed into lists | stay decomposed (quad diagonal must match the draw module) |
| terrain noise fragment shader | llvmpipe fallback | the noise program splits into 19 jobs (`CHROME9_FS_SPLIT=1`) but its mod-289 hash needs more than the 16 mantissa bits of the fragment ALU and llvmpipe is faster; all GL2/glmark2 vertex work runs on hardware TCL since Mesa `b45ec96`/`ddc2b4e` (terrain VS texels fetched on the CPU per distinct vertex) |
| desktop blur | done: runs entirely on the hardware (no unsupported FS, no fallback draws, 49 fps at 800x600); glmark2 `--validate` reports 1 LSB (G, B) off its reference pixel | accepted LSB deviation: the sampler's bilinear weights have 5 bits (1/32 steps), max 5 LSB against llvmpipe on high-contrast texel edges, 99.7 % of pixels within 1 LSB (`chrome9-dlight-alpha-blur-precision.md` §2.8) |

## Not implemented

| feature | state |
|---|---|
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
| NPOT cube maps (BGRA/BGRX) | kernel `5cc7b60` / Mesa `6b34b58` | on: the cube sampler takes POT faces only, so the view samples a POT shadow cube (next power of two above 1.5 x the size) the engine resamples face by face and level by level, nearest, when the cube changed; other formats llvmpipe; piglit `cubemap npot` passes |

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
