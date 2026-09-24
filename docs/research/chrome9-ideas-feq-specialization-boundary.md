# Chrome9 `ideas` FEQ specialization boundary

Status: source-exact application audit, captured linked NIR, and offline Linux
5.75 compiler objects.  No GPU was used.  This note does not authorize an
application-name or shader-hash special case.

## Result

The glmark2 `ideas` lamp does use an exact-value domain which collapses every
fragment `unitvec(vertex_position, lightNPosition)` call to
`lightNPosition.xyz`: all three uploaded light `w` lanes are zero and the
corresponding vertex-position `w` lane is one.  That collapse needs no
floating equality, no integer boolean, no select, and no reciprocal.  It also
needs no promise about NaNs or subnormals because it follows from exact
zero/one provenance rather than a numerical range approximation.

The current linked fragment NIR does **not** carry that provenance.  It sees
three dynamic uniform `w` lanes and one smooth interpolated varying `w` lane.
Its four ordered `feq` operations explicitly preserve infinity and NaN.  A
generic backend looking only at this fragment NIR must therefore continue to
reject the FEQ/`iand`/`bcsel` graph.  The existing ABS/CMP finite-domain
rewrite remains invalid without an independently proved normal-or-zero,
non-NaN domain.

There is a generic, non-app-specific route forward, but it has two distinct
proof obligations:

1. extend draw-time specialization to key the three uniform `w` dwords used
   by select predicates and fold exact uploaded zero values;
2. carry a linked-stage exact-value fact proving the smooth fragment input
   `vertex_position.w` is constant one, including the raster interpolation
   boundary.

Only after both facts are present in compiler IR may all three unit-vector
select trees be replaced by their light-vector operands.  Application source
inspection is an oracle for the expected result, not a production compiler
predicate.

## Current specialized bind, 2026-09-24

The draw-time uniform-predicate finder has since advanced beyond the original
snapshot described below.  In a one-frame `ideas` run it keyed UBO dword
offsets `{3,7,11}` to exact zero, then constant-folded all three light-W
equalities, their general reciprocal arms, and the Boolean conjunctions.  The
logo-shadow program (`program=28`, 11 ALU plus one TEX) compiled successfully;
the first fragment program actually bound and rejected was instead the
three-light lamp specialization.

Its captured NIR is in
`/root/tmp/chrome9-live/ideas-kill-20260924/ideas-one-frame.log` and has source
BLAKE3 words:

```text
58557edd 2eab1772 eb3c62b6 00c61868
ef79a373 82a55503 90bd4936 8de934a0
```

The exact first unsupported value is now one shared predicate:

```text
eq_vertex = feq T1.z, 0                 // preserve:inf,nan
light0    = bcsel(eq_vertex.xxx, c0.xyz - position.xyz, c0.xyz)
light1    = bcsel(eq_vertex.xxx, c1.xyz - position.xyz, c1.xyz)
light2    = bcsel(eq_vertex.xxx, c2.xyz - position.xyz, c2.xyz)
```

`T1.z` is the packed `vertex_position.w` component.  The rejection is not the
reviewed POSITION/A8/TEXKILL logo-shadow object and no longer involves a
dynamic light-W equality or `iand`.  The rest of the captured lamp is
straight-line DP3/RSQ/MUL/ADD/MAX plus three finite integer POW20 chains.
After replacing each POW20 with five MUL records and accounting for the two
mixed-vector assemblies, the remaining equality/select lowering is expected
to stay below the 96-record semantic limit.  Exact compiler allocation and
controls must still be replayed before claiming that fit.

### Generic fail-closed successor

An unrestricted FEQ lowering remains invalid.  A bounded successor can avoid
application and shader identities by attaching a draw-time domain proof to a
direct smooth-input component:

1. Accept only `feq(input_component, +/-0)` whose result is broadcast to
   side-effect-free `bcsel` users, and reject explicit denormal-preserve mode.
2. Form one reusable condition with `ABS(input); SUB(0,abs)`.  This avoids the
   still-unqualified native NEG source modifier.  Emit one native CMP per
   result width using the proved live-lane/X-broadcast/ZERO-padding rule.
3. Record the input component in semantic metadata.  For every queued
   triangle require the three post-VS values to be bit-identical
   normal-or-zero before using this program; otherwise reject that draw.
   Existing positive-finite RHW and finite-varying validation remains in
   force.

The bit-identical triangle rule is deliberately stronger than checking that
each vertex is merely finite: it excludes NaN/Inf, a varying sign crossing,
and a subnormal approach to zero across interpolation.  It admits the lamp's
exact-one T1.z producer without teaching the fragment compiler about affine
matrices or glmark2.  Native finite ABS, SUB, CMP signed-zero selection, and
vec1--vec4 CMP packing already have independent production/live evidence.
Before product use, an offline regression must assert the complete specialized
lamp record count, physical high-water, A90--A92 controls, the shared condition
being emitted once, and rejection when any triangle vertex violates the
recorded input-domain requirement.

### Exact bounded replay and first resource gate

That replay is now complete against the 141-line current capture.  The copied
capture has SHA-256
`eccffdeee025eeeda4d6e6f9115c706c5012f089a524f74d96569931d8d13880`;
it is Mesa root `a854407` with Chrome9 nested commit `3ecbc20`.  Its post-inline
arithmetic and dependency order were rebuilt with the three uniforms retaining
distinct UBO bases 0, 1, and 2.  An offline-only `ffract`/`fge` surrogate made
the existing compiler emit the same one-scalar-to-three-vec3-select graph.  The
complete surrogate compiler transcript has SHA-256
`96dc3b56687a071e24cb649fa2161fb208fcc6f494080616338fe166062acc76`.
The surrogate is only a scheduling instrument: it is not an FEQ semantic
claim and is not production code.

Replacing its one FRC, three per-select SUB conditions, and three CMP records
with the reviewed shared condition gives the exact bounded stream:

```text
ABS  qabs, T1.zzzz
SUB  qneg, c31.zzzz, qabs.xxxx
CMP  light0.xyz, qneg.xxxx, light0_minus_position.xyz, light0.xyz
CMP  light1.xyz, qneg.xxxx, light1_minus_position.xyz, light1.xyz
CMP  light2.xyz, qneg.xxxx, light2_minus_position.xyz, light2.xyz
```

Here c31.z is exact zero; `T1.z` is the captured packed
`vertex_position.w`.  The remaining stream is unchanged, including three
five-MUL POW20 chains and the current source-fneg-to-MUL treatment.  Its exact
semantic census is:

| operation | records |
|---|---:|
| MOV | 2 |
| MUL | 38 |
| ADD | 12 |
| SUB | 1 |
| MAX | 6 |
| DP3 | 13 |
| RSQ | 4 |
| ABS | 1 |
| CMP | 3 |
| **total** | **80** |

There are zero TEX records.  The three direct varyings have widths 4, 4, and
2 and mask `0x07`.  UBO constants c0--c2 plus immediates c29--c31 give
constant mask `0xe0000007` and constant high-water 32.  Every record remains
inside the constant-port rule.  Thus neither the 96-ALU limit, the 32-constant
limit, varying linkage, nor texture capacity is the first failure.

The allocate-before-releasing-last-use replay reaches ten simultaneously live
whole-register temporaries and physical high-water ten (p0--p9), still inside
the recovered twelve-register physical file.  The current kernel nevertheless
fails sooner.  At zero-based bounded record 29, the second light's
`RSQ t38, t39.x` allocates the ninth live temporary to p8.  At allocation time
the live set is:

```text
t10:p1  shared equality condition       t12:p0  shared -position
t8:p2   first-light direction            t20:p4  normalized normal
t23:p3  packed eye vector                t28:p6  first POW/accumulator value
t37:p7  second-light selected vector     t39:p5  its DP3 length
t38:p8  new second-light RSQ result
```

`chrome9_fragment_resource_note_destination()` indexes an eight-entry
resource array and returns `-E2BIG` for destination id p8.  This is the first
deterministic capacity blocker; the encoder never reaches the later ten-live
peak or finalizes controls.  A different physical choice cannot help this
record under the current whole-register allocator because nine values are live
under its allocate-before-release rule.

If a generic resource representation and schedule first admit p8/p9, the
projected no-bucket controls are `A90=90040007`, `A91=91000000`, and
`A92=92000050`.  They are a projection, not current kernel output: the present
encoder returns before writing the final controls.  The next implementation
gate is therefore generic scheduling/recomputation or a proven extension of
the resource analyzer, followed by this complete 80-record replay.  Raising an
advertised ALU cap or adding a lamp/shader identity would not fix the failure.

## Exact captured NIR contract

The post-option lamp NIR is `/tmp/ideas-lamp-post.nir`, SHA-256
`2a0b7b0ace57f4269b9179c55cf61d181802dbfcf0418ae2d2d950a6f4252b35`.
Its first light is:

```text
eq_vertex = feq vertex_position.w, 0        // preserve:inf,nan
eq_light0 = feq light0Position.w, 0         // preserve:inf,nan
both       = iand eq_vertex, eq_light0

general    = light.xyz / light.w - vertex.xyz / vertex.w
light_arm  = bcsel eq_light0, light.xyz, general
vertex_arm = bcsel eq_vertex, -vertex.xyz, light_arm
result     = bcsel both, light.xyz - vertex.xyz, vertex_arm
```

The unrolled lamp has `feq` definitions `%20`, `%21`, `%49`, and `%76`, three
`iand` definitions, and nine vector `bcsel` definitions.  `%20` is the shared
vertex predicate.  Every `feq` prints `preserve:inf,nan`.

Mesa 26.2.2 assigns `nir_fp_preserve_inf = 1 << 1` and
`nir_fp_preserve_nan = 1 << 2`, so each FEQ has exact local
`fp_math_ctrl = 0x6`.  It has neither `preserve_signed_zero` nor any
`no_contract`, `no_reassoc`, `no_transform`, or `exact` bit.  `iand` and
`bcsel` are integer/selection operations and carry no floating-point control
field.  This does not make NaNs ignorable: ordered `feq(NaN, 0)` must still
produce false because the FEQ explicitly preserves NaN behavior.

Inlining only `lightNPosition.w = 0` folds each tree to:

```text
bcsel(feq(vertex_position.w, 0),
      lightNPosition.xyz - vertex_position.xyz,
      lightNPosition.xyz)
```

This is materially smaller, but it still cannot use `CMP(-ABS(w), ...)` for
an arbitrary smooth varying.  A nonzero subnormal may flush to zero and the
Chrome9 CMP NaN result is not qualified.  If the linked compiler additionally
proves `vertex_position.w = 1`, the predicate is false and the expression is
simply `lightNPosition.xyz`.

## What the scene source proves

The audited glmark2 2023.01 files are:

| file | SHA-256 |
|---|---|
| `src/scene-ideas.cpp` | `fac8d039a2d42d9af47b1dab0a48c04606300d4dc74a0603aea1873eb7ba06d5` |
| `src/scene-ideas/lamp.cc` | `acb890f063320d7e012bc295c478995b78868f2426a2b2dbcc08596ffb58f3d7` |
| `data/shaders/ideas-lamp-lit.vert` | `624373a30b8aa73c90193eb7320211070680c8998763396387483bb95ca6623d` |
| `data/shaders/ideas-lamp-lit.frag` | `dc38503c9d11e0173e5ca068fdce05b627b990e62f7eee8914bfcb7eedb94b35` |

`scene-ideas.cpp:103..105` initializes all three direction vectors with an
exact zero `w`.  `initLights()` multiplies them by the default-identity
modelview stack at `:108..120`.  Every lamp draw uploads those three vec4s at
`lamp.cc:227..237`.  After the draw, only light zero changes, at
`scene-ideas.cpp:346..350`: an exact-`w=0` spline direction is multiplied by
the current `lookAt` modelview.  Lights one and two retain their initialized
zero `w` lanes.

The matrix library makes the homogeneous claim explicit:

* a `MatrixStack` starts at identity and `loadIdentity()` restores it;
* translate, scale, rotate, and `lookAt` construct affine matrices whose last
  row is `(0,0,0,1)`;
* multiplying two affine matrices preserves that last row;
* multiplying such a matrix by a finite `(x,y,z,0)` direction returns an
  exact zero `w`.

The scene spline control points and interpolation arithmetic are finite in
the normal scene-time interval, so the CPU path does not introduce a
`0 * Inf` or `0 * NaN` exception.  This is stronger than a vague
"finite-domain" promise for the compared lane: the resulting homogeneous
lane is exactly zero.

The vertex shader constructs `curVertex = vec4(vertex.xyz, 1.0)` and stores
`vertex_position = modelview * curVertex` at
`ideas-lamp-lit.vert:23..27`.  For the affine modelview uploaded by this draw,
the vertex output `w` is exactly one.  Projection is used only for
`gl_Position` and does not enter `vertex_position`.

This source proof is not visible to the fragment compiler.  The captured NIR
declares `vertex_position` as an ordinary smooth `vec4` input and loads its
`w` component dynamically.  A production proof must come from linked vertex
NIR plus constant/uniform specialization and must establish that raster setup
preserves a constant-one plane.  Inferring it from the name
`vertex_position`, from a glmark2 hash, or from observed uniform values alone
would be an invalid generic optimization.

## Current specialization misses this graph

`chrome9_nir_find_inlinable_uniforms()` currently discovers uniform leaves
only from structured `nir_if` conditions and loop-exit conditions.  The
linked `unitvec` source has already become straight-line `feq`/`iand`/`bcsel`
before the Chrome9 finalizer.  Consequently its light `w` dwords do not enter
the present variant key even though the existing variant machinery can
inline keyed UBO dwords and repeatedly run constant folding, dead-CF,
peephole-select, algebraic, CSE, and DCE.

A safe generic extension should walk only boolean predicates that actually
control a `bcsel` (including a bounded `iand` tree), collect the uniform
leaves through the existing constant-offset UBO rules, and keep the exact
draw-time bits in the variant key.  It should not inline every uniform in a
large arithmetic shader.  In the captured compact lamp layout the three
loads have vec4 bases 0, 1, and 2, so the exact prospective dword offsets are
`{3,7,11}`; these are UBO dword offsets, not GLSL uniform locations.  With
those three dwords keyed to zero, NIR must fold all light FEQs, eager light
reciprocals, and their dead general arms.  The remaining shared vertex FEQ
must stay until linked-stage constant provenance exists.

## Smallest Linux production objects

### Exact-value closure

When both exact facts are available, the unit-vector result aliases the
existing light constant and contributes **zero ALU records and zero
temporaries** inside the surrounding program.  The smallest standalone Linux
5.75 terminal witness is merely `MOV oC0,c0`:

```text
tokens
  ffff0200 03000001 000f0800 20e40000 0000ffff

active resource tuple
  (0,0,0,0)

ALU
  01c00530 301c0053 00f08005 000d0820

controls
  90058000 91000000 92000001 93000000 94000000 95000000
  96000000 97000000 98000000 99000000 9a000000 9b000000
```

This one-record object proves constant-source aliasing and terminal output;
the optimized unit-vector subexpression itself should not emit this MOV when
its consumer can read the constant directly.

### Uniform-zero-only residual, capacity evidence

For comparison with the old 12-record generic unit-vector schedule, keying
only `light.w = 0` leaves one equality/select.  On an independently proved
finite normal-or-zero, non-NaN vertex-`w` domain, the smallest Linux 5.75
production witness is:

```text
SUB r0,c0,T0
ABS r1,T0.wwww
CMP oC0,-r1.xxxx,r0,c0

tokens
  ffff0200
  04000003 000f0000 20e40000 30e40000
  03000023 000f0001 30ff0000
  05000058 000f0800 01000001 00e40000 20e40000
  0000ffff

active resource tuple
  (1,2,1,0)

ALU
  80800530 30140924 00f0c005 00000020
  02c06db8 b00c06db 00f0c06d 00090060
  00800530 08200053 00f20100 00090820

controls
  90058001 91000000 92000003 93000000 94000000 95000000
  96000000 97000000 98000000 99000000 9a000000 9b000000
```

The full XYZW writes are an isolation convenience; the shader consumes XYZ.
This is compiler/resource evidence only.  It is not a legal lowering for the
captured smooth varying because the required normal/non-NaN proof is absent.
The exact-value closure above is both smaller and semantically stronger.

## Smallest offline and guarded tests

The first offline test should construct the post-link unit-vector NIR with
three UBO light inputs and one smooth varying, then assert:

1. a `bcsel`-predicate uniform finder keys exactly the three light-`w`
   dwords, not their XYZ data or the varying;
2. values `{0,0,0}` fold nine light-controlled selects to three residual
   shared-vertex selects and remove all light-W reciprocals;
3. values `{1,0,0}`, NaN, and smallest-positive-subnormal produce distinct
   variant keys and are not folded as zero;
4. without a linked exact-one varying fact, compilation remains unsupported;
5. with an explicit compiler-owned exact-one fact, each result aliases its
   matching light XYZ source and no FEQ/`iand`/`bcsel` remains.

The smallest eventual guarded GPU test should not start with the entire
lamp.  Draw a triangle whose vertex stage writes constant
`vertex_position.w = 1`, upload a runtime light vec4 with `w = 0`, and return
the specialized unit-vector XYZ as an asymmetric color.  Repeat with light
`w = 1`, vertex `w = 0`, smallest subnormal `w`, and quiet NaN where the API
and kernel containment permit them.  Require distinct compiled variant keys,
exact submitted records, guarded color/depth BOs, and known-good draws before
and after.  Until the linked exact-one fact and its interpolation contract
are implemented, this remains an offline compiler task and generic FEQ stays
fail-closed.
