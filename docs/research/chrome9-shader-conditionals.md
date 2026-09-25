<!-- SPDX-License-Identifier: GPL-2.0-only
Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 shader conditionals: selects, discard and vertex flow

Status 2026-09-25.  Survey of every piglit and glmark2 case with fragment or
vertex conditionals on the VX900, silicon probes of the vertex branch
semantics, and the Mesa/kernel changes that followed.  Mesa commits are in
the chrome9 driver repository (branch `master`), the kernel change is a
proposal (not loaded).

## 1. Method

* piglit with `CHROME9_SW_DEBUG=1 CHROME9_DEBUG_TCL=1 CHROME9_DEBUG_REJECT=1
  CHROME9_DEBUG_SHADERS=1`: every per-test log then records the llvmpipe
  fallback draws (`sw fallback draws=N`), the vertex fallback reasons
  (`TCL fallback:`, `TCL flow:`, `TCL cannot express:`) and the first NIR
  instruction the fragment builder rejected.  The classifier is
  `/root/tmp/cond-classify.sh <results name>` on the target (name, result,
  sw draws, TCL reasons, unsupported FS creates, first reject).
* Lists: the full `pg-all60.txt` (2101 tests, vertex built-ins, no fragment
  built-ins) and a new `cond-fs.txt` (609 fragment tests of `gl21-quick.txt`
  with compares, selects, sign/step, all/any, discard, loops, face).
* A "fallback" below is a test with llvmpipe draws; a fragment program
  created unsupported but replaced by a supported uniform/face variant at
  draw time is not one.
* glmark2 at 800x600 (`gmdiag.sh`, `gmall.sh`) and llvmpipe image
  comparison (`chrome9_scene_ref.sh`, `tools/chrome9_cmp_rgba.py`).

## 2. Findings before the changes (p76)

Fragment (`cond-fs.txt`, 609 tests): 188 tests with 3099 llvmpipe draws,
all passing through the fallback.  Causes, by first rejected instruction:

| cause (test family) | tests |
|---|---:|
| vector equality: `equal()`/`notEqual()`, `==`/`!=` of vectors and matrices (per-lane `b2f`, `ball_fequal`/`bany_fnequal`) | 74 |
| Boolean uniforms: `b2b1` of the 0.0/1.0 store, `==`/`!=`/`^^` of bools, `all()`/`any()` of bvecs | 48 |
| per-lane ordered compares: `lessThan()`/`greaterThan()`/`step()` of vectors | 33 |
| `sign()` (`fsign` not lowered) and compares of `sign`/`b2f`/`bcsel` results (finite proof) | 10 |
| ternaries on `gl_FrontFacing` (uint input not specialized) | 6 |
| loops beyond the unroll limit or the 96 records | 3 |
| `discard` other than the sampled-alpha form | 3 |
| other: `all()`/`any()` of compares, integer negate selects, open-coded mix, if around texture lookups, NaN built-ins | 11 |

Vertex (`pg-all60.txt`): 37 tests fell back to the draw module on a 1-bit
`seq` left by the Boolean lowering of `bany_inequal`/`ball_iequal`
(`vs-op-eq-mat*`, `vs-*-bvec*-using-if`, `vs-outerproduct-*`, uniform
initializer tests); 7 on data-dependent loop counts, 3 on nesting depth,
1 on a data-dependent branch.  glmark2 `ideas` drew its lamp through the
draw module ("guarded fragment domain") at 67 fps, and its generic lamp
fragment program was unsupported (above 96 records).

Existing precision limits (not conditionals): `conditionals`
fragment-steps=5 differs from llvmpipe by mean 3.74, 2.2 % of pixels > 16.
A host simulation of the scene with 16 mantissa bits in every operation
gives mean 2.9 (round to nearest) to 8.4 (truncation), 0.9 % to 6.1 % of
pixels > 16, and 0.00 with 23 bits: the difference comes from
`fract(x * y * 0.0001)` at 16 bits, amplified 2x/3x by each of the five
steps.  The select itself is exact.

## 3. Silicon probes (rungs `vsdyn`, `vsnest`)

Both rungs are in `tools/re/chrome9_rq_rungs.c` and drive
`DRAW_VERTEX_PROGRAM` directly.  Program: `r0 = c0 (red)`, the flow
region adds or selects `c1 (green)` or `c2 = 1/8`, `T0 = r0`; the centre
pixel is read back.

### 3.1 IF on a temporary (`vsdyn`, module `vs_flow_dynamic=1`)

* `IF r1.<s>` with `r1 = v1` never takes the first arm for any value in any
  lane: 0, -0, 1, -1, 0.5, 2, 1.5, 2^-149 bit pattern, 1+ulp, the smallest
  normal, 8388609, 127, 255, -2, swizzles xxxx..wwww and identity.
* Setting the b# store instead: with the condition in physical `r0`,
  only `b0` (and `0xffff`) take the arm; with the condition moved to
  physical `r3` (four other temps live), only `b3` does.

Op 29 therefore reads the b# bit selected by the register number of its
source; the value of a temporary is never tested.  The VX900 vertex engine
has no data-dependent branch, consistent with `VS20Caps.Caps = 0` and
`DynamicFlowControlDepth = 0` (section 6.1 of
[chrome9-queries-msaa-3d-flow-re.md](chrome9-queries-msaa-3d-flow-re.md)).
Data-dependent vertex conditionals must be selects.

### 3.2 Nesting (`vsnest`, module `vs_flow_depth=3/4`)

| program | result |
|---|---|
| `REP a { REP b { +1 } }` | b, for every a >= 1 (outer loop ends after one trip) |
| `REP a { IF b0 { +1 } }`, `IF b0 { REP a { +1 } }` | exact for a = 0..3 |
| `IF b0 { IF b1 {+1} ELSE {+2} } ELSE {+3}` | exact for all four b0/b1 |
| `IF` nested 3 and 4 deep | exact for all 8 / 16 bool patterns |
| `REP a { IF b0 { IF b1 { +1 } } }` | exact |
| `REP a { REP b { REP c } }` | b * c at a = 1 or 2 |

IF nests at least four deep, inside or around one REP; a REP inside a REP
ends the outer loop (one loop counter, as the vendor's
`StaticFlowControlDepth = 1`).  Zero-trip REP skips its body.

## 4. Changes

Mesa, chrome9 driver repository:

1. `chrome9: host tests with the llvmpipe fallback` - the NIR/TCL host
   tests link again with llvmpipe enabled and run every case.
2. `chrome9: shared CMP conditions for fragment selects` - a Boolean lowers
   once to a condition `(negate ? -t : t) >= 0`, optionally complemented,
   evaluated by CMP with its qualified A-operand NEG.  Ordered compares are
   SUB; equality is SUB+ABS with a negated condition (was 2 SUB + 2 CMP per
   select); `&&`/`||` of conditions of one form are MIN/MAX; per-lane
   selects use one vector SUB and CMP; vector `all(equal())`/
   `any(notEqual())` sum the magnitudes (MAX, DP3, DP4); Boolean uniforms
   compare as their exact 0.0/1.0 store; `sign()` is two CMP.  The host
   test evaluates the emitted IR against NIR for equal, signed-zero,
   ordered and mixed operands.
3. `chrome9: fragment discard on any lowered condition` - every
   `terminate_if` becomes `CMP(condition, -1, +1)`, several merge by MIN
   into the one KILL the kernel allows.
4. `chrome9: flatten every fragment if` - loop-free ifs become selects at
   finalize and after uniform inlining (the linker stops at 8
   instructions per arm).
5. `chrome9: face variants for the uint gl_FrontFacing input` - the
   cull-specialized face variants now also replace the uint form the
   linker emits (the piglit ternary tests draw without culling, so they
   still need a dynamic face, see 6).
6. `chrome9: scalar Boolean vector reductions for hardware TCL` - the
   1-bit `seq` vertex fallback.
7. `chrome9: TCL flow control as measured on silicon` - no dynamic IF
   (`CHROME9_TCL_DYNAMIC_FLOW` removed) and never REP inside REP.
8. `chrome9: fragment float equality only on uninterpolated values` -
   perspective interpolation of a triangle-constant value is exact only
   for 0 and +-1, so `==` on a varying (`vs-output-array-vec*-index-wr`
   compares varyings to 1.1, 2.1, ...) is lowered only when no
   interpolated input reaches either operand.
9. `chrome9: occlusion queries of KILL programs in llvmpipe` - the
   hardware sample counter includes pixels a KILL discards
   (`occlusion-query-discard` counted 25600 of 0).
10. `chrome9: guarded equality to zero inside any fragment condition` -
    the existing draw-time guard (varying lane bit-identical 0 or +-1 per
    triangle) now applies inside the general condition lowering, so it
    composes under `&&`/`||` and discard; one record less per test.
11. `chrome9: fragment programs that only discard` - oC0 = 0 after the
    KILL when the shader writes no colour.

A first variant dropped the guard and ran the `ideas` lamp on hardware
TCL at 128 fps (67 before) with an unchanged image, but the same rule
made `vs-output-array-vec*-index-wr` fail: the guard stays, and the lamp
keeps its draw-module vertex path until TCL can prove the lane (see 6).

Kernel (proposal, host-built with W=1, host vertex self-test passes and a
mutation of the new rule fails it): `drm/chrome9: vertex flow control as
measured on silicon` removes `vs_flow_dynamic` (op 29 only on b#) and
rejects REP inside REP at every `vs_flow_depth`.  The program cache key
keeps its size (the byte becomes reserved).

## 5. Results

All numbers on VX900 at 800x600, final Mesa library `cond-10` (chrome9
`2cd0d5c` on `da0e389`), before = `p76`/`p79` (the main-session builds).

| measure | before | after |
|---|---:|---:|
| `cond-fs.txt` (609 fragment tests): tests with llvmpipe draws | 188 | 15 |
| same: llvmpipe draws | 3099 | 20 |
| same: pass / fail / skip | 597 / 8 / 4 | 597 / 8 / 4 (identical per test) |
| baseline-fallback subset of `pg-all60.txt` (200 tests): TCL "vertex shader not expressible" | 47 | 17 |
| same: tests with llvmpipe draws | 136 | 101 |
| `pg-all60.txt` full run: PASS / FAIL | 1772 / 5 (p79) | 1774 / 4, no regression |
| glmark2 default suite score (3 s scenes) | 116 | 116 |
| glmark2 `ideas` generic lamp program | unsupported (> 96 records) | compiles |

The eight FAILs of `cond-fs.txt` are the `fs-smoothstep-*` and
`fs-mix-float-float-float` tolerance checks (1e-5 relative, below the 16
mantissa bits), unchanged.  The 15 remaining fallbacks are the face
ternaries, `fs-sign-times-*` above 96 records, three loop tests and the
two NaN built-ins (section 6).  Image comparison against llvmpipe is
unchanged: `ideas` mean 0.17, `conditionals` fragment-steps=5 mean 3.6
(precision, section 2).

## 6. Remaining

* glmark2 `ideas` lamp: its `feq(vertex_position.w, 0)` needs the 0/+-1
  guard, which only the draw module checks per vertex.  Hardware TCL
  needs a draw-time proof that the output lane is exactly 1 (row
  (0,0,0,1) of the modelview uniform times an input w of 1), which also
  needs finite inputs; worth about 60 fps in that scene.
* `gl_FrontFacing` without culling (`fs-frontfacing-ternary-*`): face
  variants exist only when culling fixes the face; splitting a draw into
  its front and back triangles would let both variants run.
* Vertex IF nesting beyond one: silicon-proven to depth 4 (section 3.2),
  but the kernel default `vs_flow_depth=1` and Mesa's
  `CHROME9_TCL_FLOW_DEPTH=1` stay until the kernel reports the depth to
  userspace (a GET_PARAM or CAP2 bit).  Needed by `glsl-vs-if-nested-loop`
  and else-if chains on uniforms.
* Nested vertex loops cannot use REP twice; they need a literal inner trip
  count to unroll (`glsl-vs-vec4-indexing-temp-*-in-nested-loop-*`).
* Data-dependent vertex loop counts (`glsl-vs-loop`, `glsl-vs-loop-nested`,
  count from an attribute) have no hardware form; a REP over a proven
  upper bound with a select-guarded body would need a bound.
* `glsl-vs-loop-300` / `glsl-fs-loop-300`: 300 trips exceed REP's 255 and
  the 96/256 records of an unrolled body.
* Fragment programs above 96 records: `fs-sign-times-*` (25 iterations of
  two `sign()`), `glsl-fs-loop-vec4-counter`, `glsl-fs-if-nested-loop`.
* `fs-nan-builtin-min/max` test NaN semantics the finite proofs exclude.
* Dynamic column/row writes of temp matrices (`vs-temp-array-mat*-wr`)
  exceed the 255 virtual vertex temporaries.
* The glmark2 `function`/`loop` fragment scenes differ from llvmpipe by
  mean 7.6 (8.5 % > 16) on p75, p79 and after these changes alike: a
  precision boundary like `conditionals`, not a select error.
