# Chrome9 render status, 2026-09-24

Target VX900 `192.168.2.60`, kernel module with opt-in FE33 fence
(`665a532`, loaded with `fe33_fence=1`, SHA-256
`d763d55710b62922c0df33edcd77708d4596f9e71f69afb3abc59c96f658a283`).
Screenshots are in `artifacts/2026-09-24/render-status/`.

## FE33 on silicon

With `fe33_fence=1` the first glamor cmd22 job logged
`FE33 first 3D completion id=00000001` and every later 3D job retired
through the exact FE33 ID before the cookie.  The five stencil guard modes,
three-sampler, sparse-five, shadow threshold/sample and projected X8Z24
compare oracles, all 33 default glmark2 scenes and ioquake3 GL1/GL2 ran
without any timeout, canary or quarantine report.

## glmark2 default set (Mesa `311e99e`, before the fixes below)

`tools`-style runner: one `--validate` frame plus a 4 s run per scene
(`glmark_matrix.sh`, target `/root/tmp/chrome9-live/fe33-20260924/`).

| # | scene | validation | FPS |
|---:|---|---|---:|
| 1 | `build:use-vbo=false` | Success | 2 |
| 2 | `build:use-vbo=true` | Success | 2 |
| 3 | `texture:texture-filter=nearest` | Success | 33 |
| 4 | `texture:texture-filter=linear` | Success | 34 |
| 5 | `texture:texture-filter=mipmap` | Success | 34 |
| 6 | `shading:shading=gouraud` | Success | 1 |
| 7 | `shading:shading=blinn-phong-inf` | Success | 1 |
| 8 | `shading:shading=phong` | Success | 1 |
| 9 | `shading:shading=cel` | Unknown | 54 |
| 10 | `bump:bump-render=high-poly` | Success | 1 |
| 11 | `bump:bump-render=normals` | Success | 9 |
| 12 | `bump:bump-render=height` | Success | 9 |
| 13 | `effect2d:kernel=0,1,0;1,-4,1;0,1,0;` | Success | 26 |
| 14 | `effect2d:kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1;` | Success | 18 |
| 15 | `pulsar:quads=5:texture=false:light=false` | Success | 23 |
| 16 | `desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true` | Failure | 10 |
| 17 | `desktop:windows=4:effect=shadow` | Success | 9 |
| 18 | `buffer:update-fraction=0.5:update-dispersion=0.9:columns=200:update-method=map:interleave=false` | Success | 1 |
| 19 | `buffer:update-fraction=0.5:update-dispersion=0.9:columns=200:update-method=subdata:interleave=false` | Success | 1 |
| 20 | `buffer:update-fraction=0.5:update-dispersion=0.9:columns=200:update-method=map:interleave=true` | Success | 1 |
| 21 | `ideas:speed=duration` | Unknown | 2 |
| 22 | `jellyfish` | Unknown | 2 |
| 23 | `terrain` | Unknown | 1 |
| 24 | `shadow` | Unknown | 1 |
| 25 | `refract` | Unknown | none |
| 26 | `conditionals:vertex-steps=0:fragment-steps=0` | Success | 2 |
| 27 | `conditionals:vertex-steps=0:fragment-steps=5` | Success | 2 |
| 28 | `conditionals:vertex-steps=5:fragment-steps=0` | Success | 2 |
| 29 | `function:fragment-steps=5:fragment-complexity=low` | Success | 2 |
| 30 | `function:fragment-steps=5:fragment-complexity=medium` | Success | 2 |
| 31 | `loop:vertex-steps=5:fragment-steps=5:fragment-loop=false` | Success | 2 |
| 32 | `loop:vertex-steps=5:fragment-steps=5:fragment-uniform=false` | Success | 2 |
| 33 | `loop:vertex-steps=5:fragment-steps=5:fragment-uniform=true` | Success | 2 |

Later short runs replaced the 40 s FPS run; see the per-scene notes.

## Fixes found by this matrix

| Mesa commit | defect | effect |
|---|---|---|
| `9bdaacf` | A8 sampler view `0,0,0,W` rejected | ideas runs with no reject, matches llvmpipe (mean diff 0.00) |
| `da259a6` | finite/nonnegative proofs walked the shared DAG as a tree under a 128-visit budget, and treated GLSL's structural `preserve_inf` on neg/abs/min/max/compare as exceptional | cel lowers to 47 semantic records and matches llvmpipe (mean diff 0.03) |
| `8e076a2` | `transfer_map` neither submitted the retained batch nor resolved the shadow color target | `glReadPixels` before swap returned an empty frame; now matches |
| `ad64c2f` | direct varying TEX coordinate `.zw` lost lane W at the three-lane textured FE consumer cap | ioquake3 GL2 lightmapped world surfaces were black; now close to llvmpipe |

Kernel `a99b79f` now rejects a direct varying TEX coordinate whose first two
selectors are not `X,Y`, instead of silently sampling the wrong texel.

`CHROME9_DEBUG_REJECT=1` prints each rejected NIR def (innermost first);
`CHROME9_DEBUG_FS_MARK=id` paints every fragment shader with its creation
index (R bits 2:0, G 5:3, B 7:6) to identify a material on screen.

## Pixel comparisons against llvmpipe

`tools/glx_capture_swap.c` now fakes `CLOCK_MONOTONIC` for the main program
(`GLX_CAPTURE_FRAME_US`) and dumps the 800x600 back buffer before swap N
(`GLX_CAPTURE_SWAP`, `GLX_CAPTURE_SWAP_RAW`), so hardware and llvmpipe
render the same animation frame.

| scene | result |
|---|---|
| shading cel | mean RGB diff 0.03 |
| ideas | 0.00 |
| desktop blur (4 windows) | 0.08; `--validate` still fails at 512,209 by 1/1/4 (tolerance ~3.5) -- precision, correctness phase |
| shadow | 0.24 |
| jellyfish | renders correctly; animation phase differs (other time source) |
| refract | does not reach its first swap within 8 s (throughput: ~20 ms per cmd22 ioctl) |
| terrain | noise pass needs >= 100 ALU records against the 96-record store; rejected |

## ioquake3

GL1 q3dm1 (800x600) renders like llvmpipe, no rejects, no kernel report.
GL2 q3dm1 takes the no-FBO path and, after `ad64c2f`, renders close to
llvmpipe; remaining visible differences are small (sky and floor tint).

## Open

* terrain noise pass capacity (hardware limit; needs a fallback design);
* hardware TCL, remaining GL2 features (two-sided FACE, alpha modes 1/2);
* throughput, strictly after all hardware features.
