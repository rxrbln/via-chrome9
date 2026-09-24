# Chrome9 glmark2 conditionals current boundary

Status: exact installed glmark2 2023.01 source, current Chrome9 semantic path,
and existing live-result audit, 2026-09-24.  No GPU or production code was
used for this audit.

## Exact default benchmark rows

The installed default suite contains these three rows:

```text
conditionals:vertex-steps=0:fragment-steps=0
conditionals:vertex-steps=0:fragment-steps=5
conditionals:vertex-steps=5:fragment-steps=0
```

Both `vertex-conditionals` and `fragment-conditionals` retain their default
value `true`.  The exact source hashes are:

```text
scene-conditionals.cpp          81224bfb72d79e364cd6c5c48c04abf1302c215d498026d16470a15f716d02e1
conditionals.vert               8a820a95ff556d566b2605b56621ccddb86963d1bfa4eea824e72aecdb6022d3
conditionals.frag               5764e6aa924622e0ac228742778b7196e9e2e6bdacc2b06f54ca70115d6f92b9
conditionals-step-conditional   c4abfa1a0fb2148c8cbbe9b88837fb770d3eb0a77ad653371e1b41d741634b19
conditionals-step-simple        4aea58864ff92f0e18d8c23b1e1cd177f6ce329101e56628bb2d271f42f65498
```

The fragment prefix is always:

```glsl
float d = fract(gl_FragCoord.x * gl_FragCoord.y * 0.0001);
```

Each enabled conditional step is:

```glsl
if (d >= 0.5)
    d = fract(2.0 * d);
else
    d = fract(3.0 * d);
```

The result is `vec4(d,d,d,1)`.  The vertex shader uses the same step text on
`fract(position.x)`, then perturbs only position Y before its matrix
transform.  Its `dummy` varying is present in source for an unrelated r600g
performance workaround and is link-dead for the Chrome9 fragment shader.

## Fragment path is closed for the default rows

The zero-step fragment is a strict subset of the already exercised five-step
program: fragment-position input, two multiplies, FRC, and color assembly.
It has no comparison, sampler, ordinary varying, uniform block, or new state
requirement.

For the five-step form, Mesa if-converts the side-effect-free source control
to dependent scalar `bcsel` nodes.  The current Chrome9 semantic backend
accepts exactly this bounded predicate:

```text
bcsel(fge(ffract(value), 0.5), true-fract, false-fract)
```

It proves the selected value remains in `[0,1]`, lowers each step to SUB,
two MUL/FRC arms, and CMP, and broadcasts the scalar condition only over the
live result lanes.  The compare may carry `preserve:inf`; other preservation
flags, thresholds outside `[.25,.75]`, per-lane predicates, or values without
the bounded FRC proof still fail closed.  This is not general GLSL branch or
floating-comparison support.

The installed five-step shader produced 40 semantic fragment records in the
current driver, within the independent 96-ALU limit and with no TAU records.
The kernel allocator reuses physical temporaries for the dependent chain.
The same semantic six-record step has a proprietary compiler object and the
31-ALU direct-seed guarded chain retired on VX900.  Most importantly, the
exact installed five-step glmark2 row has already reported
`Validation: Success` at 800x600.  Therefore no fragment compiler or hardware
gate remains for any of the three default conditionals rows.

The current host `chrome9-shader-nir-test` also passes its scalar/vector
bounded-select cases and rejects a per-lane predicate.  This preserves the
generic acceptance boundary independently of the application result.

## Vertex five-step row

Chrome9 does not translate this vertex control flow into a native VX900
vertex program.  `chrome9_create_vs_state()` hands the NIR shader to Gallium
Draw.  In the current no-LLVM build, `draw_vs_exec.c` converts NIR to TGSI and
the TGSI interpreter executes IF/ELSE using its per-lane execution mask.
`chrome9_vbuf_need_pipeline()` always selects the draw pipeline, so this row
cannot bypass the software vertex execution.

The fragment side of the vertex-five row is the already closed zero-step
program.  The VS adds no sampler, fragment varying, or kernel fragment
resource.  Thus it has no unresolved Chrome9 hardware-ISA requirement.  A
remaining application run would validate Draw's generated positions and the
same raster/readback path; it is not a prerequisite for another GPU feature.

## Validation boundary

`SceneConditionals::validate()` reads pixel `(293,89)`.  With fragment
conditionals enabled it expects:

```text
fragment-steps=0: RGBA a0 a0 a0 ff
fragment-steps=5: RGBA 25 25 25 ff
```

using an RGB distance tolerance of `sqrt(3*5*5)+0.01`.  The five-fragment-
step row is already live-validated.  The two zero-fragment-step rows need only
bounded application-level validation if a complete default-suite ledger is
desired: run both under one healthy X session, require the `a0` reference,
no draw rejection or engine fault, and a known-good draw afterward.

Do not add a scene key, shader hash, native branch opcode, or hardware TCL
dependency for those runs.  Any future failure in the vertex-five row should
first be localized to Draw NIR-to-TGSI/vertex execution or ordinary raster
coverage, because its fragment and kernel resource envelopes are already
strict subsets of the live five-fragment-step row.
