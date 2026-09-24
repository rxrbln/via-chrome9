# Chrome9 default terrain fragment closure

Status: source-exact glmark2 2023.01 plus Mesa 26.2.2 offline audit.  No DRM
node or GPU was used.  The Windows production probe was attempted only under
the shared offline lock, but its existing Wine prefix did not complete within
the bounded timeout; no new Windows result is claimed.  Existing hashed
Windows/Linux compiler corpora are used where noted.

## Result

The default terrain fragment shader is not presently blocked by an unknown
terrain-specific instruction.  Its first compiler failure is an ordering bug
in draw-time uniform specialization:

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

The audited source is `terrain.frag` SHA-256
`efc2c1cd2ab3ad01e9d42430f427e6ba441f2e3311fb9afe5aaafe25b503a8d2`.
It was linked with five independent passthrough vertex attributes so the
linker could perform normal scalar varying packing without algebraically
identifying unrelated terrain values.  The Mesa 26.2.2 post-link, scalarized
offline NIR has source Blake3 words:

```text
2ad40952 d0c218d1 87f717ec 0d474950
3f8a3d6b ae2b9b11 e7267648 cd0e9a75
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
| scalarized NIR ALU nodes before distance folding | 69 |
| TEX nodes | 5 |

The 69 ALU nodes comprise 26 MUL, 13 ADD, seven DP3, five RSQ, four `vec3`,
two `vec4`, two NEG, and one each of FLRP, FLT, SQRT, RCP, MIN, BCSEL, two
MAX, LOG2 and EXP2.  This inventory is after the loop with literal trip count
one is unrolled.  It is before Chrome9's `lower_flrp32` expansion and
semantic vector-constructor materialization, so it must not be mistaken for
a finalized ALU-store count.

The physical varying map is concrete and useful for linkage tests:

```text
VAR0.xyz              vTangent.xyz
VAR0.w, VAR1.xy       vBinormal.xyz
VAR1.zw, VAR2.x       vNormal.xyz
VAR2.yz               vUv.xy
VAR2.w, VAR3.xy       vViewPosition.xyz
```

Thus five logical GLSL varyings do not require five hardware interpolation
slots in this build.  A cap/link test must require at least four physical FS
inputs while retaining the 14-component packing contract.  The current
driver still advertises only two or three FS inputs, 32 ALUs, three fragment
samplers, and zero vertex samplers (`chrome9_screen.c:301-362`), so the scene
correctly remains unavailable independently of this compiler fix.

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

With distance fixed to zero, the offline shader contains 60 scalarized NIR
ALU nodes and five TEX nodes.  Straight mechanical backend accounting gives
approximately 69 semantic ALUs after four-record FLRP lowering and disjoint
vector packing, still below the recovered 96-ALU store.  This is a capacity
estimate, not a finalized-object claim: exact physical-temp pressure and any
kernel coordinate bridges still require a real Chrome9 compiler dump.

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
