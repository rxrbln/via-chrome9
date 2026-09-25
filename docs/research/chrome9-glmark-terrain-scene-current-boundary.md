# Chrome9 glmark2 terrain current boundary

Status: source-exact offline audit against installed glmark2 2023.01 and
Chrome9 driver commit `d56f61f` (no GPU), extended by the silicon frame-time
audit of 2026-09-25 below.

## Frame-time audit on silicon (2026-09-25, lib `tbias2`, 800x600)

Measured with `CHROME9_SW_DEBUG=1 CHROME9_SW_SYNC=1` and two `perf` runs
(logs on the target in `/root/tmp/terrain-inv/`): 560-710 ms per frame.

| part | cost per frame | reason |
|---|---|---|
| main terrain draw, 393216 vertices, in llvmpipe | ~380 ms (~220 ms vertex/setup/clip, ~160 ms fragments) | every vertex shader that samples a texture renders in llvmpipe (`chrome9_context.c`, VS `textures_used`); the TCL engine has no vertex fetch from textures |
| of which `draw_reset_vertex_ids` | ~70 ms | llvmpipe's 4 KiB vbuf flushes every ~40 vertices and each flush walks the whole vertex array |
| bloom and tilt-shift passes (hardware) plus CPU mipmap generation | ~127 ms | 557 -> 430 ms with `bloom=false:tilt-shift=false` |
| noise pass (256x256, two triangles) in llvmpipe | ~12 ms | the only rejected fragment program |

Only the noise program (`terrain-noise.frag`) is rejected (`reject ir=96
temps=98` twice: create and the face variant).  The rejection lands in
octave 1's third `permute`; a single octave is already beyond 96 records.
All other programs compile: normal map 26, main terrain 85 records with
5 TEX, bloom 34, overlay 2, tilt 64 (before the MAD fusion below).

The noise program cannot run natively even when it fits: `permute()`
needs exact integers up to about 2.8e6 (22 bits), while the 16-bit
fragment mantissa is exact only up to 2^17 and `1/289` is inexact; the
terrain would differ far beyond an LSB.  A best-case compiler still needs
about 90 records per octave (about 360 in total).  Keep it in llvmpipe.
Nothing is cacheable under GL semantics: `uvOffset` changes every frame, so
the height map and the normal map derived from it change too.

To reach 15 fps the main draw has to leave llvmpipe: either prefetch the
LOD 0 vertex texture reads on the CPU into an extra vertex stream for the
TCL engine (786k bilinear reads per frame, estimated 15-40 ms), or run
the vertex stage in the LLVM draw module with hardware fragments (then the
16-triangle draw batch has to grow towards the kernel's 128).  Both also
need the post-processing chain well below 66 ms.

With MAD fusion (Mesa, `chrome9_nir_optimize_semantic`) the records drop
to: main 85 -> 72, tilt 64 -> 55, bloom 34 -> 30, normal map 26 -> 23.

## Result

The default terrain **main** vertex/fragment pair is no longer rejected by
the current semantic backend.  After the draw-uniform value
`pointLightDistance[0] = 0` is specialized, it produces 73 semantic records
(68 ALU plus 5 TEX), has physical TEMP pressure 10, and translates to the
fragment-program UAPI.  Its five fragment sampler slots are dense `0..4`;
the application's units `0,1,2,4,5` do not become sparse descriptor ids.

The complete default scene nevertheless has an earlier, independent shader
gate.  Every frame first renders the procedural height/normal chain
(`scene-terrain.cpp:332-338`).  The height pass uses
`terrain-texture.vert` plus `terrain-noise.frag`
(`simplex-noise-renderer.cpp:42-52`).  Its exact linked NIR contains 28
`b2f32(fge(...))` results originating in GLSL `step()` calls.  Chrome9 has no
generic `b2f32` lowering.  This is the first source-exact semantic rejection,
not sparse texture state.

Adding only that lowering cannot close the height pass.  The optimized linked
program contains 40 live `ffract` operations and 60 live dot products (56
DP3 and 4 DP4).  Those two non-fusible families alone require at least 100
native ALU records, already exceeding the current 96-record ALU store before
ADD, MUL, comparison conversion, ABS, MIN/MAX, or output assembly.  The
four-octave shader therefore needs a real capacity strategy; raising sampler
caps or teaching one comparison conversion is insufficient.

## Exact sources and linked objects

glmark2 constructs the sources by adding `ShaderSource::str()` precision
macros (`shader-source.cc:477-520`) without otherwise rewriting these two
files.  The exact post-`ShaderSource` hashes are:

```text
terrain-texture.vert  98160e0d269cda0c71d1624f4e83dda83974c4d2d091ed0357cf95fe87029c9f
terrain-noise.frag    d0d5d1e254105fe38df2881ea119d0ee4cba4c59724fda595f947954ee0ae84e
```

The exact linked noise fragment's source-BLAKE3 words are:

```text
ab448632 2051c9bb 49fbbb07 4f518dc4
927b796a d50ac268 68f108d3 4be6d495
```

It has one packed smooth `vec2` input, two default-UBO loads (`time` and
`uvScale`), no textures, and one color output.  Relevant finalized NIR counts
are:

```text
fadd 176       fmul 134       fneg 105       ffract 40
fdot3 56       fdot4 4        fge 28         b2f32 28
fabs 12        fmax 8         fmin 4
```

The first compare/conversion pair in the printed linked NIR is:

```text
%39 = fge   %38.x, %38.y  // preserve:inf
%40 = b2f32 %39
```

It comes from the first `step(x0.yzx, x0.xyz)` at
`terrain-noise.frag:35-39`; a second four-lane step occurs at lines 72-76.
`surface3()` invokes the noise function four times at lines 100-107, which is
why the final object has 28 comparison conversions.

## Current backend boundary

The generic scalar backend's ordinary ALU switch supports MOV/SAT, MUL, ADD,
SUB, MAD, MIN/MAX, DP3/DP4, EXP2/LOG2, RCP/RSQ, ABS, FRC, and NEG
(`chrome9_context.c:3851-3931`).  `b2f32` is absent and reaches the default
failure at line 3931.  The other `fge` path is deliberately narrower: it is a
predicate recognizer for a scalar `bcsel` with a proven range and constant
threshold (`chrome9_context.c:3373-3427`), not a materialized float compare.

The compiler recursively lowers the color-output DAG and fails closed when
that first materialized `b2f32` is reached.  Accordingly the exact current
probe returns no semantic object:

```text
program=0 supported=0 semantic=0 semantic_count=0 alu=0 tau=0
```

Even after a bounded compare conversion is added, the capacity checks at
`chrome9_context.c:3933-3937` and the store definition
`chrome9_shader_offline.h:9-12` cap the result at 96 ALU records.  The exact
40-FRC plus 60-dot lower bound proves that scheduler or TEMP allocation work
alone cannot make this program fit.

## Main terrain pair: no current compiler reject

The exact post-`ShaderSource` hashes for the main pair remain:

```text
terrain.vert  f82cc07c2bad46f554694623d5c71588bb899152146f9d36240e611abf19d6db
terrain.frag  c666837de4308cb64903d6d97d178a292929a4c6dd495a4d70e364da13293558
```

The application binds units `0,1,2,4,5`
(`terrain-renderer.cpp:108-125,142-160`), but state-tracker sampler lowering
packs the five live fragment samplers into dense Gallium slots `0..4`.
The current exact zero-distance replay reports:

```text
records=73  ALU=68  TEX=5  TEMP-peak=10
sampler_mask=0x1f  varying_mask=0x000f
constant_mask=0x80007fff  uniform_mask=0x00007fff
UAPI-build=pass
```

The current caps cover this shape: two vertex samplers
(`chrome9_screen.c:320-327`), 96 FS ALU records, eight extended FS inputs,
eight extended sampler/view slots, and 32 TEX records
(`chrome9_screen.c:341-383`).  Four fragment samples share one computed
overlay coordinate while displacement uses direct `vUv`
(`terrain.frag:45-57`).

Sparse `[0,1,2,4,5]` descriptor qualification therefore does not remove a
main-pair compiler gate: the main pair never emits that sparse mask.  It does
strengthen the generic descriptor ABI for other shaders.

Two hardware-correctness gates remain for the accepted main program:

1. `pow(max(dot(...),0), uShininess)` uses LOG2/MUL/EXP2 and can feed zero to
   LOG2.  `tools/re/chrome9_terrain_pow_zero_guard_smoke.c` is already the
   bounded arithmetic oracle.
2. The main resource mix needs four mipmapped/trilinear sources (three static
   repeat textures plus the generated specular target) together with one
   generated L0 displacement source.  The sparse-five smoke uses direct T0
   coordinates and `MIP_FILTER_NONE`; it does not prove this mip/R2T mix.

## Small generic regressions

For the first semantic operation, add a host NIR test containing a vector
`b2f32(fge(lhs,rhs))`.  The positive case must prove finite/range-bounded
operands and produce SUB plus native CMP with exact `step` orientation;
unknown/NaN-preserving inputs and unsupported vector shapes must fail closed.
Include lanes for negative, negative zero, positive zero, and positive
differences.  This tests a generic operation and must not match shader text,
uniform names, or glmark2.

Keep the complete exact noise object as a negative capacity regression.  Once
comparison conversion exists it must fail specifically at the 96-record
capacity boundary, without publishing a partial semantic/UAPI program.

For the accepted main pair, the smallest new raw resource oracle is a dense
two-stage cmd22 draw: one repeat/trilinear BGRX mip chain sampled through a
computed XY TEMP and one clamp/linear BGRA L0 render-target source sampled
through a direct varying, with asymmetric per-level/source colors, D16,
exterior, source, and destination guards.  Extend to the exact four-mip-plus-
one-L0 mix only after that discriminator passes.  This remains a generic
texture-state test and does not special-case terrain.

