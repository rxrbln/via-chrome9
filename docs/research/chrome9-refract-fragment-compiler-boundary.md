# Chrome9 full refract fragment-compiler boundary

## Current-tree recheck after standard square-root lowering

Mesa Chrome9 commit `5be93e7` enables the standard
`fsqrt -> frcp(frsq(x))` lowering after the separate scalar hardware
qualification.  A source-exact standalone link of the default glmark2
2023.01 refract shader, followed by the current Chrome9 NIR options, uniform
and sampler lowering, `gl_nir_opts`, vector shrinking, and DCE, narrows the
remaining compiler boundary further than the historical analysis below.

The final fragment NIR has source BLAKE3 words:

```text
217a676f dd6e1609 596c4dc1 d63b1447
59629813 13364492 a4d8e477 bccdbec2
```

It has three linked smooth inputs with widths `{4,4,3}`, sampler/TEX mask
`0x07`, three computed-coordinate 2-D texture operations, no `fsqrt`, no
`nir_if`, and no phi.  The first `refract` remains this exact straight-line
subgraph:

```text
k         = 1 - 1.44 * (1 - dot(N,I)^2)
negative  = flt(k, 0.0)                  // preserve:inf
root      = frcp(frsq(k))
candidate = 1.2 * I - (1.2 * dot(N,I) + root) * N
front     = bcsel(negative.xxx, 0.xxx, candidate)
```

The second `refract`, with reciprocal index `0.833333`, is optimized to a
branchless two-component candidate in this linked consumer; there is only one
remaining `bcsel`.  The arithmetic inventory before semantic selection is 68
ALU nodes including the `flt` and `bcsel`, plus three TEX nodes.  Replacing
that comparison/select pair with one native CMP and expanding POW100 to the
existing seven-MUL chain projects 73 semantic ALUs and three TEX operations;
the kernel then needs three computed-coordinate bridge records.  The resulting
76-ALU/3-TAU upper bound fits the independently advertised 96-ALU/32-TEX
stores.  Sampler slots are a separate limit: current extended linkage exposes
eight slots, while ABI-v1 fallback remains three.

The current compiler returns `supported=0`, `semantic_fs_valid=0`, and an
empty semantic stream.  The first deterministic rejection is not control
flow or `fsqrt`: `chrome9_ps_lower_unit_interval_select()` receives the vec3
`bcsel` above and rejects its `flt(k,0)` predicate because that helper admits
only `fge` against a normal threshold on a value proven in `[0,1]` by an FRC
DAG.  The generic implementation boundary is now exactly a zero-sign vector
select, not broad Boolean lowering and not a sampler/resource-cap change.

For this exact producer, direct native CMP is materially narrower than the
general ordered-comparison problem.  The condition is a TEMP value and the
hardware operation is simply `CMP(k,candidate,zero)`: finite nonnegative
including either signed zero selects the candidate, while finite negative
selects zero.  No SUB comparison surrogate is involved.  In every
source-defined execution the two normalized vectors are finite and nonzero,
so their dot product and the subsequent squared/product chain are finite.
The last two operations are separate binary32 records:

```text
p = round_f32((1 - round_f32(d*d)) * 0x1.70a3d8p+0)
k = round_f32(1.0 - p)
```

The hexadecimal literal is binary32 `1.44` (`0x3fb851ec`).  Although the real
function crosses zero, the rounded `p` is itself a binary32 value.  The next
representable value above one is `1 + 2^-23` and the next below is
`1 - 2^-24`; therefore a nonzero emitted `k` near the crossing has magnitude
at least `2^-24`, many orders above the subnormal range.  Zero-length
normalization and exceptional interpolants are outside the defined GLSL
`normalize`/unqualified-RSQ domain.  Thus prior live finite-normal TEMP-CMP
and signed-zero behavior is enough for the comparison semantics of this exact
shader.  This producer-domain proof, not the older SUB/RCP probes, discharges
the `flt` `preserve:inf` requirement; it must not be generalized to arbitrary
`flt` operands.

The smallest offline regression should construct a scalar float32 predicate
broadcast over result widths one through four and accept only:

```text
bcsel(flt(k, +0.0), zero, candidate) -> CMP(k, candidate, zero)
```

It must resolve only bit-exact aliases, require identical result widths,
preserve lane swizzles and masks, reject per-lane predicates, nonzero
thresholds, integer conditions, nonzero true arms, and any requested NaN
preservation.  The exact refract link must then produce sampler mask `0x07`,
three TEMP coordinates, no Boolean records, no more than 96 semantic ALUs,
and a kernel-mirror allocation with no more than twelve physical temporaries.
The exact scene therefore needs an offline selector/allocation proof followed
by one bounded negative-`k` run to ensure speculative RSQ/RCP of the discarded
candidate retires safely; the comparison result itself is already within the
qualified finite-normal/signed-zero domain.  An on-chip-generated NaN
discriminator remains mandatory before promoting the selector beyond a
compiler-proved non-NaN producer domain.  This test shape is generic and
contains no scene name, shader hash, or application-specific branch.

## Result

The full `glmark2` `light-refract.frag` consumer is not blocked by texture
indirection, sampler count, native instruction storage, physical temporary
pressure, or the known three-constant MAD/CMP alias.  A semantic-equivalent
PS 2.0 program compiles with the proprietary Linux 5.75 backend to **75 native
ALU records and 3 TAU records**, uses **six coordinate resources and five
physical temporaries**, and assigns a distinct computed coordinate to each of
the three texture reads.  All of those values fit the current kernel ABI and
allocator limits.

The remaining static compiler boundary is exact and local:

1. GLSL `refract` lowers twice to an `if`/phi around `sqrt(k)`;
2. the Chrome9 NIR compiler accepts only a single straight-line block;
3. `fsqrt` is neither lowered in the advertised NIR options nor accepted by
   the native selector; and
4. after flattening, the existing `bcsel` selector accepts only the scalar
   `fge(ffract(...), constant)` finite-range form, not the vector
   `bcsel(flt(k, 0), 0, candidate)` produced by `refract`.

This is a generic control-flow and ALU legalization gap, not a reason to add a
shader-name, hash, or glmark-specific path.

## Source and reproducibility boundary

The audited GLSL source is glmark2 2023.01
`data/shaders/light-refract.frag`, SHA-256:

```text
5d60dd85933ef77a7bc89eb5d567fb9791fd9b14170ce82b06985da718a5e001
```

Its relevant topology is:

```text
eye = normalize(-vertex_position.xyz)
normal = normalize(vertex_normal)
front = refract(eye, normal, 1.2)
depth = texture2D(DepthMap, projected_position + front)
back_position = vertex_position + depth * front
back_normal = texture2D(NormalMap, back_position.xy * .5 + .5)
back = refract(back_position, back_normal, .833333)
image = texture2D(ImageMap, back.xy * .5 + .5)
light = normalize(vertex_position.xyz / vertex_position.w - (1,1,2))
specular = pow(max(dot(reflect(light, normal), eye), 0), 100)
color = image + specular * vec4(.8,.8,.8,1)
```

There are three smooth inputs with widths `{3,4,4}` and three 2D samplers.
The exact GLSL was compiled through Mesa's standalone GLSL compiler.  Its LIR
contains two instances of the standard refract expansion:

```text
d = dot(I, N)
k = 1 - eta * eta * (1 - d * d)
if (k < 0)
        result = 0
else
        result = eta * I - (eta * d + sqrt(k)) * N
```

A semantic-equivalent HLSL PS 2.0 object, with the constants above fixed to
their exact float32 values, has SHA-256:

```text
object:      2cde03c2d0d146def02bbb4b8a22f802f72cd7ca0eb144bbea3fa0597db6f819
disassembly: 9f0a6bb55dc45175ad0783434bcc1fb039db5187b35eda33adbe9a6653529b67
```

It contains 75 executable D3D instructions: 72 ALU operations and three TEX
operations.  This object is a semantic and resource oracle for the exact GLSL
consumer, not a claim that the Direct3D and OpenGL frontends produce
byte-identical native code.

## Proprietary Linux finalized object

The production Linux probe has SHA-256
`a22c873536dc4b6c45c43a00a48dbc183e40ddb89ed5e1461890efad2d21e6ba`;
the loaded proprietary DRI object has SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
Finalizing the PS 2.0 object with post-processing configuration 2 gives:

```text
input D3D schedule       72 ALU + 3 TEX
native finalized object 75 ALU + 3 TAU
active resource tuple   (coordinate, physical-temp, sampler, address)
                        (6, 5, 3, 3)

TAU[0] 301331b0
TAU[1] 311341b1
TAU[2] 321251b2

A90 90048007
A91 91000038
A92 9203004b
A93 93000007
A94 94000000
A95 95000000
A96 96000840
A97 97000020
A98 98000000
A99 99000000
A9a 9a000000
```

The three ALU records added during finalization are the computed-coordinate
bridge MOVs.  Direct varying arithmetic occupies coordinate resources 0, 1,
and 2.  The three ALU-generated texture coordinates occupy resources 3, 4,
and 5, hence `A91 & 0xff == 0x38`.  The first ALU-to-TAU transition is in
`A96=0x840`; the later transition crosses the first 24-bit control word and
sets `A97=0x20`.  Thus each computed texture coordinate has an independent
address resource and dependency edge.

The native resource high-water of five physical temporaries is below the
kernel's twelve-register limit.  Six coordinates fit the eight-coordinate
namespace.  The object also fits the ABI limits of 96 native ALU records, 32
TAU records, and 128 semantic instructions.  The relevant limits and
allocator are in:

- Mesa `chrome9_shader_offline.h:9..12`;
- kernel `chrome9_fragment.c:12..13` and `:378..491`;
- UAPI `chrome9_drm.h:959..961`.

The Windows compiler independently accepts and finalizes the same PS 2.0
object through the checked minimal semantic wrapper, yielding 77 ALU records
and three TAU records.  That wrapper deliberately supplies synthetic
allocation callbacks, so its native register assignment is not executable
evidence and the two extra records must not be interpreted as a Linux/Windows
hardware difference.  The authoritative resource and control oracle above is
the Linux production object.

## Proprietary schedule and exceptional-value evidence

The proprietary schedule normalizes with `DP3; RSQ; MOV; MUL`.  Each refract
sequence computes `k`, materializes the sign with CMP, then eagerly executes
`RSQ(k); RCP`, forms the vector candidate, and uses a final CMP to choose zero
or the candidate.  In abbreviated form:

```text
DP3 d, I, N
... k = 1 - eta^2 * (1 - d^2)
CMP sign, k, zero, one
...
RSQ root, k
RCP root, root
... candidate = eta*I - (eta*d + root)*N
CMP result, -sign, zero, candidate
```

This is source-backed evidence that the production compiler intentionally
speculates the negative-domain RSQ/RCP and masks its result afterward.  It
therefore supports the same straight-line strategy in Mesa.  It does not by
itself establish native CMP behavior for a NaN condition.

The final specular power uses `LOG; MUL; EXP` in the proprietary object.  The
current Mesa compiler instead recognizes positive integer exponent 100 and
uses a multiplication chain.  Both preserve the required zero-to-positive
power result; this difference is not a blocker.

No proprietary refract CMP or MAD names three distinct constant registers.
The sign materialization repeats lanes from one constant register.  The final
select uses a TEMP condition, a zero constant, and a TEMP candidate.  The
proposed Mesa select has the same safe source-file topology.  It does not hit
the kernel rejection at `chrome9_fragment.c:217..229`.

## Current Mesa boundary

The following existing pieces are already sufficient:

- computed TEMP coordinates are accepted by the NIR texture selector at
  `chrome9_context.c:3081..3133`;
- scalar `fdot3`, `frsq`, `frcp`, `flog2`, and `fexp2` have native mappings at
  `chrome9_context.c:3283..3307`;
- positive integer `fpow` has a bounded multiplication-chain lowering at
  `chrome9_context.c:3143..3195`;
- the semantic builder admits 96 ALU, 32 TAU, 128 total instructions, and 128
  virtual temporaries; and
- the kernel inserts the coordinate bridges at
  `chrome9_fragment.c:948..1009` and performs first-free physical allocation
  at `chrome9_fragment.c:429..455`.

The blockers are:

1. `chrome9_fs_straight_line_entrypoint()` rejects any entrypoint whose body
   is not one block (`chrome9_context.c:318..338`).  Both classification and
   native compilation depend on it (`:3442..3444`, `:3887..3901`).
2. `chrome9_nir_options` does not set `lower_fsqrt`
   (`chrome9_screen.c:268..282`).  The selector accepts `frsq` and `frcp`, but
   its default case rejects `fsqrt` (`chrome9_context.c:3293..3323`).  Mesa's
   source-backed algebraic rule is exactly
   `fsqrt(a) -> frcp(frsq(a))` when `options->lower_fsqrt` is true
   (`nir_opt_algebraic.py:1908`).
3. `chrome9_ps_lower_unit_interval_select()` is deliberately limited to a
   scalar `bcsel(fge(...),...)` whose compared value is proven FRC-derived and
   whose threshold is in `[.25,.75]` (`chrome9_context.c:2841..2972`).  A
   refract phi is vector-width and uses `flt(k, 0)`.
4. The existing specialization loop invokes `nir_opt_peephole_select` with
   limit 8 only after a uniform was inlined
   (`chrome9_specialise.c:244..274`).  A shader with no inlinable control
   uniform does not enter that loop, and the compiler must not rely on this
   incidental specialization path to flatten structural refract control
   flow.

The former API instruction-cap blocker is closed.  Mesa commit `284e01a`
advertises 96 structured fragment instructions/ALUs and 32 TEX instructions
when cmd22 is present, independently of the sampler-slot count.  Extended
linkage commit `db6365a` advertises eight inputs and eight sampler/view slots
when the matching kernel capability is present.  `max_temps` remains eight,
but the exact linked source already reaches NIR and the kernel performs its
own liveness allocation onto the twelve-entry physical file; the proprietary
five-temp high-water is inside both limits.  The first static rejection is
therefore still control-flow/fsqrt/select legalization, not public capacity.

The function-medium offline regression in Mesa commit `f836ff3` now also
proves the standard `lower_fsqrt` result (`FRSQ` followed by `FRCP`) through
the Chrome9 semantic compiler.  Production deliberately leaves
`lower_fsqrt` disabled until the repeated scalar live gate passes.  That work
removes uncertainty about instruction selection, but it does not flatten the
two refract `if`/phi regions or broaden the finite unit-interval `bcsel`
contract to the zero/candidate vector select here.

## Ranked bounded compiler gates

### Gate 1: offline straight-line legalization

Operate on a clone of the linked fragment NIR, not on the canonical shader.
Run `nir_opt_peephole_select` with the documented non-control-flow-hardware
mode `.limit = ~0u`, followed by copy propagation, algebraic optimization,
CSE, DCE, dead-CF removal, and phi removal until stable.  Mesa documents this
mode in `nir_opt_peephole_select.c:39..63`; it admits ALU, constants, derefs,
phis, and texture instructions but rejects calls and jumps.  The two refract
regions contain only cheap ALU definitions and phis.

The exact-source offline test must assert:

- the entrypoint becomes one block with no phi or `nir_if`;
- the two refract zero/candidate selections remain, allowing only ordinary
  NIR lane scalarization;
- sampler and varying masks are both `0x7`;
- all three TEX coordinates are ALU-generated TEMP values;
- the semantic program is at most 128 instructions and 96 ALU operations;
- kernel-mirror finalization succeeds without `E2BIG`; and
- no MAD/CMP has three distinct constant-register sources.

Fail closed if any non-flattenable control flow remains.  Do not globally
raise caps at this gate.

### Gate 2: source-backed `fsqrt` lowering

Set `.lower_fsqrt = true` in the Chrome9 NIR compiler options and prove in the
same offline test that neither refract sequence contains `fsqrt` after the
normal NIR optimization pipeline.  Each root must be `frcp(frsq(k))`.  This is
preferable to adding an ad-hoc `fsqrt` case in the native selector because it
uses Mesa's standard backend contract and already-supported native opcodes.

The ordering of gates 1 and 2 is not semantically significant for the exact
source, but the test must inspect the final cloned NIR after both passes.

### Gate 3: exact zero-sign vector select

Add a separate generic selector for only this normalized form:

```text
bcsel(flt(k, 0.0), zero-vector, candidate-vector)
```

Recognize bit-exact zero through resolved MOV/vec aliases, require float32,
require the condition to be scalar and splatted, and require equal vector
widths.  Do not broaden the existing FRC finite-range helper.  Since native
CMP selects its true arm for a condition greater than or equal to zero, invert
the NIR predicate by swapping the arms and emit:

```text
CMP(k, candidate, zero)
```

This direct zero test needs no SUB, so it introduces neither comparison
underflow nor an extra condition temporary.  It has at most one constant
register source and is outside the three-distinct-constant hazard.

The exact-source test must verify both three-component refract results, lane
swizzles, writemasks, zero-arm placement, and equality behavior.  Existing
live evidence qualifies finite negative, positive, `+0`, and `-0` CMP
conditions, but NaN remains unresolved; see
`chrome9-generic-float-compare-boundary.md`.

### Gate 4: guarded numeric execution

Before enabling the new select for production, execute a tiny program with
`k = -.25, +0, +.25` in guarded lanes.  It must eagerly compute
`frcp(frsq(k))`, then select zero for the negative lane and the roots
`0`/`.5` for the nonnegative lanes.  Bracket it with known-good draws and
inspect engine completion.  This validates speculative negative RSQ/RCP,
CMP arm placement, signed-zero equality, and the resulting lifetimes.

Then add a NaN condition lane.  If native CMP does not implement GLSL's
ordered `flt` rule for NaN, keep this lowering restricted to a compiler-proven
non-NaN `k` domain or add an independently qualified ordered-compare
expansion.  Finite vertex and constant validation in the kernel
(`chrome9_engine.c:7411..7419`, `chrome9_fragment.c:1175..1177`) is helpful
but is not by itself a proof that intermediate arithmetic cannot produce NaN.

### Gate 5: full consumer and cap promotion

Only after gates 1--4 pass, compile the unmodified glmark shader, assert its
actual Mesa semantic/native counts, and run the full three-sampler scene.
Promote API-facing instruction/temp caps no higher than the independently
qualified hardware and allocator limits.  The proprietary `(6,5,3,3)` object
is the expected resource-class oracle, not a requirement that Mesa duplicate
its exact register schedule.

## Closure statement

The proprietary object proves that the full refract consumer fits Chrome9 and
that three computed texture coordinates, the normalization/refract schedule,
and the specular power can coexist in one finalized program.  The next
implementable Mesa slice is therefore narrowly bounded: flatten the two
if/phi regions on a clone, use the standard `fsqrt -> frcp(frsq)` lowering,
and lower only the exact zero-sign vector select to native CMP.  Resource
capacity and three-source hazards are already closed statically; exceptional
CMP behavior and the final cap promotion remain explicit guarded gates.
