# Chrome9 default terrain fragment closure

Status: source-exact glmark2 2023.01 plus Mesa 26.2.2 offline audit, refreshed
through Chrome9 commit `78f237a` and shared Mesa commit `4575a3c`.  No DRM
node or GPU was used.  The Windows production probe was attempted only under
the shared offline lock, but its existing Wine prefix did not complete within
the bounded timeout; no new Windows result is claimed.  Existing hashed
Windows/Linux compiler corpora are used where noted.

## Result

The default terrain fragment shader is not blocked by an unknown
terrain-specific instruction.  Its former first compiler failure was an
ordering bug in draw-time uniform specialization:

1. the GLSL branch on `pointLightDistance[0]` is if-converted to a scalar
   `bcsel` before the Chrome9 finalizer runs;
2. `chrome9_nir_find_inlinable_uniforms()` discovers only surviving `nir_if`
   nodes (`chrome9_specialise.c:175-195`), so it does not key a variant on the
   distance uniform;
3. the unfurled true arm retains `fsqrt(dot(lVector,lVector))`, and the merge
   retains a general `bcsel(flt(0,distance), computed, 1)`;
4. Chrome9 has neither an `fsqrt` backend case nor a general compare-select
   lowering.  Its only `bcsel` path accepts `fge(unit_interval, constant)`
   with a threshold in `[.25,.75]` (`chrome9_context.c:2900-2997`).

glmark2 uploads `pointLightDistance[0] = 0.0` once in
`TerrainRenderer::init_program()`.  Correctly specializing that value makes
the condition false and removes the complete distance arm, including
`fsqrt`, reciprocal, MIN and select.  Implementing that semantic
specialization before raising caps is the smallest compiler closure; adding a
native square-root opcode is not required by the shipped default scene.

After that fold, the only distinct arithmetic correctness gap is
`pow(max(dot(...),0), uShininess)`.  `uShininess` is a uniform (uploaded as
`30.0`), not a literal in NIR, so the existing integer-constant POW lowering
does not apply.  The backend emits native `LOG2; MUL; EXP2`.  This is exact
compiler structure, but the base can legally be zero and Chrome9 LOG zero is
not yet qualified on silicon.  A positive-only LOG gate is therefore
insufficient for terrain correctness.

## Exact linked interface and operation inventory

The audited glmark2 tree is commit
`42e3d8fe3aa88743ef90348138f643f7b04a9237`.  Raw and exact default
`ShaderSource::str()` hashes are:

```text
terrain.vert file          f07ac9490a748428ce9e1df9c5896f61659f927e99c1aa5334f926f4b013c76a
terrain.frag file          efc2c1cd2ab3ad01e9d42430f427e6ba441f2e3311fb9afe5aaafe25b503a8d2
post-ShaderSource vertex   f82cc07c2bad46f554694623d5c71588bb899152146f9d36240e611abf19d6db
post-ShaderSource fragment c666837de4308cb64903d6d97d178a292929a4c6dd495a4d70e364da13293558
```

The final hashes include glmark2's generated precision macro block; the
fragment also includes its default mediump-float block.  Mesa's standalone
GLSL linker followed by the Chrome9 options, vector shrinking and DCE produces
printed fragment NIR SHA-256
`86b4a3ce28006216e507afab18aec0553c4788fb98ed35a42f06d5970a458c41`.
Its source BLAKE3 words are:

```text
1a5275b3 a5aed1dd 0d1c076a 1839fb95
31c35861 717b45c9 b9f92d1f 9c4f0d76
```

Its exact resources are:

| resource | linked requirement |
| --- | ---: |
| ordinary 2D TEX | 5 |
| active shader samplers | diffuse1, diffuse2, detail, specular, displacement |
| application texture units | `0,1,2,4,5` (high-water 6) |
| logical smooth varyings | 5 / 14 components |
| physical interpolation slots after scalar packing | 4 (`VAR0..VAR3`) |
| non-sampler uniform vec4 slots | 16; the distance load becomes dead after folding but the linked ABI allocation remains |
| color outputs | 1 |
| optimized linked NIR ALU nodes before distance folding | 70 |
| TEX nodes | 5 |

The 70 ALU nodes comprise 26 MUL, 15 ADD, seven DP3, five RSQ, three `vec3`,
two `vec4`, one `vec2`, three NEG, two MAX, and one each of FLT, SQRT, RCP,
MIN, BCSEL and POW.  Chrome9's options have already lowered FLRP; POW remains
a NIR operation and general POW later expands to LOG2, MUL, EXP2.  This
inventory is after the literal-one light loop is unrolled but before semantic
vector-constructor materialization, so it is not a finalized ALU-store count.

The physical varying map is concrete and useful for linkage tests:

```text
VAR0.xy                vTangent.xy
VAR0.zw                vUv.xy
VAR1.x                 vTangent.z
VAR1.yzw               vBinormal.xyz
VAR2.xyz               vNormal.xyz
VAR2.w, VAR3.xy        vViewPosition.xyz
```

Thus five logical GLSL varyings do not require five hardware interpolation
slots in this build.  A cap/link test must require at least four physical FS
inputs while retaining the 14-component packing contract.  The current
structured path advertises 96 ALU/total instructions and 32 TEX instructions,
but still only three FS inputs, three fragment sampler/view slots, and zero
public vertex sampler/view slots.  The scene therefore remains unavailable
independently of the instruction-store capacity.

Four fragment samples consume the computed coordinate
`uRepeatOverlay * vUv + uOffset`; displacement consumes direct `vUv`.  The
computed coordinate is one shared SSA value, not four unrelated coordinate
expressions.  Normal/detail decode and lighting use only already represented
native families:

```text
normalization       DP3, RSQ, MUL
TBN transform       three MUL, two ADD
matrix/position     MUL and ADD
diffuse/specular    DP3, MAX, MUL, ADD
uniform POW         LOG2, MUL, EXP2
texture mixing      FLRP lowered by Mesa, then MUL
```

With the default distance fixed to zero, the optimized offline shader contains
61 NIR ALU nodes and five TEX nodes (printed NIR SHA-256
`022f057502d3ea51c333776a23fe1066395428570d05504cb09b46a2ac0d4015`).
General POW adds two records when expanded from one NIR node to
LOG2/MUL/EXP2; disjoint vector construction adds further MOVs.  The exact
post-inline compiler result below replaces this earlier capacity estimate.

At the current compiler boundary the exact unspecialized program produces no
semantic object: `supported=0`, `semantic=0`, `semantic_count=0`, with zero
ALU and TAU output.  The output DAG reaches the general
`bcsel(flt(0,pointLightDistance[0]), falloff, 1)` form.  The only current
selector lowering requires a scalar `fge` of a proven unit-interval value
against a constant threshold in `[0.25,0.75]`; this zero-threshold uniform
comparison fails before either arm can become native.  The runtime value zero
is therefore the first compiler gate, not an instruction-store limit.

## Why current specialization misses the default value

Chrome9 asks the state tracker to call its finalizer in the linker.  The state
tracker first performs its NIR optimization/lowering, then calls
`screen->finalize_nir`; Chrome9's callback subsequently runs integer lowering
and `chrome9_nir_find_inlinable_uniforms()` (`chrome9_screen.c:284-298`).
The linked terrain object visible at that boundary contains:

```text
condition = flt(0.0, pointLightDistance[0])
length    = fsqrt(dot(lVector, lVector))
falloff   = 1.0 - min(length / pointLightDistance[0], 1.0)
lDistance = bcsel(condition, falloff, 1.0)
```

The finder walks `nir_block_get_following_if()` only.  It does not inspect a
`bcsel` predicate, even though the later inliner already runs constant
folding, dead-CF, peephole-select and DCE (`chrome9_specialise.c:244-278`).
Consequently it never obtains the distance dword whose substitution would let
those existing passes remove the arm.

The generic fix should collect uniform leaves from bounded `bcsel` predicates
as well as `nir_if` conditions.  It must retain the existing depth/budget
limits and variant-count ceiling; this is semantic control-uniform discovery,
not a terrain or uniform-name key.  An offline regression should compile:

```glsl
float x = distance > 0.0 ? expensive_length / distance : 1.0;
```

and prove that `distance=0` removes FLT/SQRT/RCP/MIN/BCSEL while a positive
distance either compiles through a separately qualified general selector or
continues to fail closed.

### Implementation progress (2026-09-24)

Chrome9 now discovers wholly uniform scalar or vector `bcsel` predicates
after collecting required loop bounds, so optional selects cannot displace a
loop bound from the four-dword variant key.  A host NIR regression exercises
the terrain-shaped `flt(0,distance)` select: distance zero removes the SQRT,
RCP, MIN, and select arm; positive distance keeps the expensive arm, and a
varying-dependent predicate is not keyed.  A separate regression covers
vector predicates and four selects competing with a loop bound.

Canonical fragment NIR is also compiled before creating variants.  If the
dynamic program already has a supported native lowering, it remains the
program for all uniform values instead of exhausting the 64-variant cache;
the existing native unit-interval CMP path is a regression case.  Only a
canonical program that cannot compile proceeds to draw-time specialization.

The exact default terrain ShaderSource inputs have been regenerated at
`/tmp/chrome9-terrain-final.vert` and `.frag`, matching the hashes above.

### Exact post-inline result

A current-Mesa standalone linker/probe now performs the same unpacked uniform
location assignment, sampler lowering, default-UBO lowering, Chrome9
inlinable-uniform discovery, zero-value substitution, and Chrome9 fragment
compilation.  It identifies exactly one key dword, default-UBO dword 60 for
`pointLightDistance[0]`.  Substituting zero removes the distance comparison,
SQRT, reciprocal, MIN, and select arm.

The finalized semantic result is accepted:

```text
supported=1 semantic=1 semantic_count=73
ALU=68 TEX=5
MOV=11 MUL=27 ADD=14 DP3=7 RSQ=5 MAX=2 LOG2=1 EXP2=1
sampler_mask=1f varying_mask=000f input_mask=0000
uniform_mask=00007fff constant_mask=80007fff
```

The semantic stream uses virtual temporary ids 0 through 66.  A conservative
component-lifetime reconstruction peaks at nine simultaneously live virtual
registers, at record 53; the high virtual id is SSA naming, not 67-register
pressure.  The five TEX records are interleaved with dependent ALU at records
4, 5, 8, 17, and 52.  Therefore the optional userspace TAU-first native
encoder reports `native=0`, as expected for this general ordered stream; that
is not a semantic rejection because cmd22 submits the 73-record semantic
program for kernel scheduling.

The regenerated source hashes remain
`f82cc07c2bad46f554694623d5c71588bb899152146f9d36240e611abf19d6db`
and `c666837de4308cb64903d6d97d178a292929a4c6dd495a4d70e364da13293558`.
The host probe binary SHA-256 is
`91c1a4c6deaf7fb717d5ba450e894a12c1716727254cf2cd968f0830e9cbe43a`;
its captured result is
`a39d393efc19bc0846fbb41592c8afe04c581840ecf622d09a0cd006a238eeca`.
No DRM node or GPU was used.

This closes the fragment semantic compiler gate, not the scene.  Public caps
still expose three fragment inputs and three fragment sampler/view slots,
while this shader needs four packed input slots and five dense fragment
samplers; the terrain vertex shader also needs the still-disabled public
vertex-texture path.  Uniform POW's LOG-zero numerical behavior remains a
separate hardware correctness gate.

## Proprietary uniform-POW oracle

The exact proprietary bounded slice matching terrain's non-literal exponent
is `POW oC0,c0,c1`.  Both production compilers expand it to scalar
`LOG; MUL; EXP`; they differ only in their scratch-allocation contract.

Linux production configuration two:

```text
01c00530 b01c0053 00f080b6 00050020
0140b6d0 30081b6b 00f00005 00000020
01c00530 301c0053 00f20005 00040820
A90=90058000 A92=92000003
```

Windows production configuration two:

```text
01c00530 b01c0053 00f080b6 00050320
0140b6d0 30081b6b 00f00c05 00000320
01c00530 301c0053 00f20c05 00040820
A90=90040000 A92=92000003
```

These records and controls are already retained in
`chrome9-nrm-pow-expansion-corpus.tsv`, with differential JSON SHA-256
`5b0770896bbb1234f0a35c82414abc571fc61d1b1531b3c427fccbd16e84a006`.
Mesa must use one coherent allocation/finalization policy; mixing the Windows
r12 stream with Linux controls is invalid.

This compiler oracle proves opcode order, scalar selection, dependency and
control counts.  It does not prove `LOG(0)` or compound numerical behavior.

## Minimal guarded future gate

After the finite positive EXP/LOG gates pass, execute one fresh-target,
timeout-bounded structured program which implements:

```text
b = MAX(T0.x, 0)
p = EXP2(LOG2(b) * 30)
oC0 = (p, 1-p, 0, 1)
```

Use four constant-varying rectangles and require approximately these
BGRA8-visible pairs (allow the separately established conversion tolerance):

| base | `p` | red byte | green byte |
| ---: | ---: | ---: | ---: |
| `0` | `0` | 0 | 255 |
| `1` | `1` | 255 | 0 |
| `255/256` | `0.88921433` | 227 | 28 |
| `15/16` | `0.14425746` | 37 | 218 |

The paired `p` and `1-p` channels make a zero-domain NaN or saturation error
more visible than writing `p` alone.  Require a fresh BGRA target and D16
target, intact inner/outer guards, zero exterior pixels, clean reset/error
counters, a known-good draw before and after, and no retry after a timeout.
This gate contains the terrain-critical MAX-to-POW dependency without five
textures, VTF, mipmaps, FBO transitions or the full long shader, so a failure
identifies arithmetic rather than resource plumbing.

Only after this gate and the four-slot varying linkage pass should the full
main fragment object be captured to establish exact final ALU/TAU counts and
physical-temp high-water.  Fragment closure still does not enable the scene:
software vertex texture sampling, six-slot sampler/view state, sparse logical
unit binding, mip/R2T chains and the broader caps remain independent gates.
