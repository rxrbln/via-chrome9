# Chrome9 glmark2 cel current boundary

Status: exact installed glmark2 2023.01 source and linked NIR rechecked
against Chrome9 Mesa nested commit
`4aaa1cf1ae89b5b06d3ea304a2b10df4456e56f6`, 2026-09-24.  This was an
offline audit; no target GPU or production code was used.

## Exact program remains unchanged

The default row is `shading:shading=cel`, linking these installed files:

```text
light-phong.vert  8fcc96b66863adae3ec6f11e1f251fd54ce6b32b39400ebb390d23ef0122dbea
light-cel.frag    3f6ebeefcab388a9304348aad4fbb9bea52c166a94619bdfc8db7e6e72485062
```

After Mesa's GLSL 1.20 linker, Chrome9 compiler options, vector shrinking,
and DCE, the printed fragment NIR SHA-256 remains
`429e900b51123b32051d50906f760d76b8ae0fe780f7359402fea23df60349bd`.
Its NIR source BLAKE3 words remain:

```text
5a99353b b7509737 ec46c818 6c367b3f
49d9345b dd49c2d0 d9e80e5d 960c1960
```

It reads smooth `T0=vertex_position.xyzw` and
`T1=vertex_normal.xyz`, with no texture, discard, or other side effect.  The
linked graph has four comparisons, three selections, and one Boolean AND:

```text
diffuse  = bcsel(fge(attenuation*max(NdotL,0), .1), green.6, green.3)
outlined = bcsel(flt(dot(V,N), .1 + .3*max(NdotL,0)), black, diffuse)
lit      = iand(flt(0,NdotL), flt(.5, attenuation*specular))
rgb      = bcsel(lit, .7 + .3*outlined, outlined)
```

Normalization, reflection and POW10 otherwise reduce to existing DP3, RSQ,
MUL, ADD, MAX, source negation and a four-MUL positive-integer power chain.
The shader does not need native branch instructions.

## What the latest vector-select work closes

The current semantic backend accepts result widths one through four in
`chrome9_ps_lower_unit_interval_select()`.  For a scalar predicate and an
N-component result it emits one scalar SUB and one N-lane CMP, broadcasts
the predicate X selector over exactly the live lanes, zero-pads inactive
lanes, and uses destination mask `(1<<N)-1`.  Commit `6c1d1d8` tests widths
one, two, three, and four, plus rejection of a genuinely per-lane predicate.
The current host `chrome9-shader-nir-test` passes that matrix.

This closes the old *packing* question for cel's vec3 arms.  It does not make
the compare generic.  The lowerer deliberately accepts only:

```text
bcsel(fge(bounded, constant), true, false)
```

where `bounded` is recursively proven to be an FRC result or a selection of
such results in `[0,1]`, the finite threshold is in `[.25,.75]`, and local FP
controls are empty except optional `preserve:inf` on the comparison.

## Current deterministic rejection

The first recursively reached cel selection is now the right width, but
still outside both numerical preconditions:

```text
%24 = fge %23, 0.1       // preserve:inf
%26 = bcsel %24.xxx, vec3(0,.6,0), vec3(0,.3,0)
```

`%23` is an attenuation product, not a proved unit-interval FRC/select value,
and `.1` is below the accepted `.25` threshold floor.  Therefore
`chrome9_ps_unit_interval()` and the threshold guard fail closed before a CMP
is appended.  The compiler returns no usable semantic/native program and the
draw remains unsupported.  The first current gate is a generic numerical
range/sign proof for comparison lowering, not vec3 construction, sampler
state, instruction capacity, or application validation.

This distinction also prevents overclaiming the latest change: the passing
width-3 regression uses `fge(ffract(x),.5)`.  Replacing its literals with
cel's green arms tests packing only and still must reject when its predicate
is `%23 >= .1`.

## Remaining ordered gates

A safe extension must reason about the comparison's producer and distance
from zero, rather than recognize this shader or threshold as a special case.
For the first cel predicate, the defined finite producer is nonnegative and
the fixed normal `.1` boundary makes every unequal representable result's
difference far larger than a subnormal.  That can justify SUB+CMP once the
range proof is represented generically.

The rest of the linked graph remains independently gated:

1. `dot(V,N) < .1 + .3*maxNdotL` needs a dynamic positive-normal lower-bound
   proof before SUB+CMP;
2. `.5 < attenuation*specular` needs the corresponding normal-bound proof;
3. `0 < NdotL` can observe a DP3 result arbitrarily close to zero and still
   needs the documented DP3/subtraction FTZ-consistency silicon gate; and
4. the Boolean AND must lower to nested side-effect-free selections while
   preserving all four truth combinations.

Only after these transformations should the exact linked NIR be compiled to
completion and measured against the 96-ALU, 32-TEX, 12-physical-temporary,
two-varying envelope.  Source operation counts alone cannot prove final fit:
vector assembly, compare materialization, and constant-port legalization can
add records.

## Focused regression boundary

The smallest next host regression should build a vec3 select with a scalar
`fge` predicate whose left side carries a generic nonnegative/finite range
fact and whose constant boundary is `.1`.  It must produce scalar SUB plus
XYZ CMP with `XXX0` predicate and `XYZ0` arms.  Negative cases must continue
to reject an unbounded producer, a comparison requiring NaN semantics, and a
per-lane predicate.

The full linked-NIR regression must retain the hashes above and initially
assert this exact first rejection.  Once generic compare facts and Boolean
lowering land, change it to require complete compilation, record exact
ALU/TEMP/constant counts, and compare the final native stream and A90--A92
controls before a bounded scene run.  The live order remains vec3 safe
positive-normal select, dynamic-threshold select, DP3/zero consistency,
nested Boolean selection, then the exact `shading:shading=cel` validation
pixel.  Skipping directly to the scene would conflate numerical comparison,
packing, allocation, and raster correctness.
