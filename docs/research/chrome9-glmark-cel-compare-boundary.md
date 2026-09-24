# Chrome9 glmark2 cel-shading comparison boundary

Status: source-exact Mesa 26.2.2 NIR and current-backend audit, 2026-09-24.
No target GPU was used and no production code was changed.

## Exact installed shader

The installed glmark2 2023.01 `shading:shading=cel` pair is
`light-phong.vert` plus `light-cel.frag`.  Their SHA-256 values are:

```text
light-phong.vert  8fcc96b66863adae3ec6f11e1f251fd54ce6b32b39400ebb390d23ef0122dbea
light-cel.frag    3f6ebeefcab388a9304348aad4fbb9bea52c166a94619bdfc8db7e6e72485062
```

The Mesa standalone GLSL 1.20 linker was run through the Chrome9 compiler
options (`lower_fceil`, `lower_fdiv`, `lower_ffloor`, `lower_flrp32`,
`lower_fmod`, `lower_fsat`, `lower_sincos`, UBO lowering and unroll limit 31),
then vector shrinking and DCE.  The complete printed fragment NIR has
SHA-256 `429e900b51123b32051d50906f760d76b8ae0fe780f7359402fea23df60349bd`
and source BLAKE3 words:

```text
5a99353b b7509737 ec46c818 6c367b3f
49d9345b dd49c2d0 d9e80e5d 960c1960
```

It reads two varyings: `T0=vertex_position.xyzw` and
`T1=vertex_normal.xyz`.  There is no texture or fragment side effect.  The
three source `if` statements are already if-converted to four comparisons,
three `bcsel` nodes, and one Boolean `iand`:

```text
N = normalize(T1)
L = normalize((4,3,1) - T0.xyz)
maxNdotL = max(dot(N,L), 0)
attenuation = sqrt(27) * rsq(dot((4,3,1)-T0, same))

diffuse = bcsel(fge(attenuation * maxNdotL, .1), (0,.6,0), (0,.3,0))
V = normalize(-T0.xyz)
outlined = bcsel(flt(dot(V,N), .1 + .3*maxNdotL), (0,0,0), diffuse)

R = reflect(-L,N)
spec = pow(max(dot(R,V),0), 10)
lit = iand(flt(0,NdotL), flt(.5,attenuation*spec))
rgb = bcsel(lit, .7 + .3*outlined, outlined)
```

The remaining arithmetic inventory is native-shaped: DP3, RSQ, MUL,
ADD, MAX, finite integer POW10 and source negations already represented by
the generic backend's multiply-by-minus-one lowering.  POW10 expands to the
four-MUL chain `x2,x4,x5,x10`, preserving a zero base.  The program therefore
does not need a new arithmetic opcode, native branch, or texture state.

## Deterministic first rejection

Against Chrome9 Mesa nested commit
`9f4d9a94740f36d283c41316598f2bea0bf56d89`, the first recursively reached
unsupported value is the diffuse three-component selection:

```text
%24 = fge %23, 0.1       // preserve:inf
%26 = bcsel %24.xxx, vec3(0,.6,0), vec3(0,.3,0)
```

`chrome9_ps_lower_def()` sends every `bcsel` to
`chrome9_ps_lower_unit_interval_select()` (`chrome9_context.c:3193`).  That
lowerer requires a scalar result at line 2948, so `%26` fails immediately.
Even a scalarized version would still fail: the helper accepts only `fge` of
a proved FRC/selection value in `[0,1]` against a constant in `[.25,.75]`.
Here the predicate is an attenuation product and the threshold is `.1`.

This is a generic vector compare/select boundary.  It is not a reason to
recognize the scene, shader hash, or final green color.

## Exact lowering shape and numerical gate

For a comparison whose input domain independently makes subtraction-sign
lowering valid, the native forms are:

```text
bcsel(fge(x,y), t, f): d = SUB(x,y); result = CMP(d,t,f)
bcsel(flt(x,y), t, f): d = SUB(x,y); result = CMP(d,f,t)
```

Chrome9 CMP broadcasts the scalar predicate while selecting vector C/B
arms, so each of the first two vector selections needs one scalar SUB plus
one vec3 CMP.  The final conjunction must stay value-based, not become
hardware control flow:

```text
bcsel(iand(a,b), t, f) -> bcsel(a, bcsel(b,t,f), f)
```

After recursively lowering each comparison this costs two SUBs and two CMPs.
The rewrite is valid only because both arms are side-effect free; it must not
be applied across TEXKILL, memory operations, or a texture operation whose
execution semantics need short-circuiting.

### Exact vec3 CMP record and inactive-lane rule

Linux 5.75 production configuration-two compilation now fixes the smallest
defined TEMP/TEMP/TEMP vec3 object.  It first moves a scalar condition and
two vec3 arms, then executes `CMP r3.xyz,r0.xxx,r1.xyz,r2.xyz` and consumes
that result:

```text
01c00530 d01c0053 00108016 000d0020
01c00530 501c0053 00708105 000d0060
01c00530 501c0053 00708205 000d00a0
02020550 50201055 00720000 000900e0  # vec3 CMP
01c00530 501c0053 00720305 000d0820

active=(0,4,0,0)
A90=90050000 A91=91000000 A92=92000005
```

The exact tokens and full control tail are retained in
`chrome9-vec3-cmp-production-corpus.tsv`.  The Windows private compiler emits
the same `XYZ` destination mask and normalizes the same inactive source lane,
but its research identity-register callbacks do not reproduce Linux
configuration-two lifetime bits; Linux remains the executable byte oracle.

The fourth lane is not don't-care.  Production normalizes every source to an
inactive-W selector of ZERO:

```text
condition = (X,X,X,ZERO)
true arm  = (X,Y,Z,ZERO)
false arm = (X,Y,Z,ZERO)
```

That normalization explains the CMP record's `...0550`, `...1055`, and
high `5...` fields.  A generic vector-select helper must therefore:

1. emit scalar `SUB` to a one-lane TEMP;
2. manually broadcast that TEMP X selector over only the destination's live
   XYZ lanes, padding W with ZERO;
3. lower each vec3 arm with `components=3`, which already pads W with ZERO;
4. emit CMP with destination writemask `0x7`.

Calling the existing scalar helper unchanged would use an X000 predicate and
select only the first lane correctly.  Conversely XXXX would violate the
vendor inactive-lane image.  This is a byte-level compiler requirement, not
a cosmetic canonicalization.

The same Linux production compiler was also run for destination widths one,
two, and four.  It emits predicate/arm images `X000/X000`, `XX00/XY00`, and
`XXXX/XYZW`, respectively, with destination masks `1`, `3`, and `f`.  All
four objects retain active tuple `(0,4,0,0)` and controls
`90050000/91000000/92000005`.  The expanded corpus and checker therefore
qualify the generic rule "broadcast X only over live lanes, ZERO-pad the
rest" for widths 1--4; the rule is no longer an extrapolation from vec3.

The three source constants in the corpus are deliberately materialized into
TEMP registers before CMP.  The live VX900 limit is three *distinct constant
registers in one MAD/CMP record*, not three operands.  A real select may use
up to two distinct constant arm registers beside a TEMP condition, but the
existing generic constant-port legalization should remain authoritative.

The proprietary SGE/SLT objects and the existing semantic CMP path both
form their predicate by subtracting the operands.  Live evidence shows the
hardware's default mode flushes a subnormal difference, and the exceptional
RCP/sign probe does not qualify defined infinities.  Therefore the compiler
must not turn the shapes above into an unrestricted implementation of NIR
`fge`/`flt` carrying arbitrary local FP controls.

Finite producers alone are **not** enough: two distinct finite normals can
subtract to a subnormal, and the live SUB/CMP probe shows such a result may
flush to zero.  The cel predicates divide into narrower classes:

* `%23 >= .1` is subtraction-sign safe.  Around the normal binary32 constant
  `.1`, adjacent representable values differ by many orders of magnitude
  more than the smallest normal.  Any other finite value is farther away.
* `%31 < (.1 + .3*maxNdotL)` has the same property on every defined finite
  path.  MAX makes its contribution nonnegative, so the dynamic right side
  is at least `.1`; two unequal representable values in that positive-normal
  region cannot differ by a subnormal amount.  Overflow in the unqualified
  producer is already undefined rather than a defined infinity reaching the
  compare.
* `.5 < %48` is safe by the same normal-constant spacing argument.
* `0 < %19` remains different.  `%19=dot(N,L)` may approach zero by
  cancellation, so it can be a subnormal before the strict comparison.  A
  zero threshold supplies no spacing floor.  This predicate needs a DP3
  output/FTZ consistency gate before it can use SUB+CMP.

All four comparison nodes request `preserve:inf` but not denormal
preservation; the shader-wide float-control mode is zero.  Their direct
leaves are finite varyings/literals, and any exceptional intermediate that
does not request preservation is undefined, but that observation discharges
only Inf/NaN handling.  It does not discharge subtraction underflow.  A
generic proof must therefore track both defined exceptional values and a
normal-distance/range fact; a finite upload check alone is insufficient.

The smallest zero-threshold discriminator should make the DP3 result itself
subnormal from normal inputs, for example `2^-70 * 2^-70 = 2^-140`, then
observe two paths in one guarded draw:

```text
d = DP3((2^-70,0,0), (2^-70,0,0))
survives = MUL(MUL(d, 2^70), 2^70)       # 1 if preserved, 0 if FTZ
selected = strict_positive_via_SUB_CMP(d) # 1 only when d compares > 0
```

Pack `survives` and `selected` into different color lanes.  `(0,0)` is a
consistent default-FTZ result; `(1,1)` is a consistent preserve result;
`(1,0)` proves the introduced SUB loses a DP3 value that the original
comparison could observe and is a hard rejection.  `(0,1)` is unclassified.
Use only exact power-of-two normal literals, check D16/exterior/guards, and
bracket the draw with known-good programs.  This is the one new numerical
gate required by the cel predicate set; it does not enable preserve-denorm
execution mode, which Chrome9 continues to reject.

Linux 5.75 configuration-two compilation fixes that discriminator to the
following eight-record object, so a live gate need not guess at instruction
packing or resource controls:

```text
01c00530 50080055 00108005 00020020
0140b6d0 d008116d 00100016 00000060
0140b6d0 d008116d 00120116 000000a0
808216d0 00140924 00120000 000000e0
01c00530 d01c0053 00120216 000d0120
0082a6d0 d0082a2d 002203a2 00090120
01c00530 301c0053 00c082b5 000d0120
01c00530 301c0053 00f20405 000d0820

active=(0,5,0,0)
A90=90048000 A91=91000000 A92=92000008
```

`tools/re/chrome9_cmp_guard_smoke.c --dp3-ftz-consistency` now expresses
that exact semantic program using only normal uploaded constants.  It accepts
only a uniform `(0,0)` default-FTZ or `(1,1)` preserve result, while rejecting
both mixed outcomes, and retains the existing D16, exterior and BO-guard
checks.  `tools/re/chrome9_vec3_cmp_production_check.py` separately locks the
five-record vec3 CMP corpus, inactive lanes, active-resource tuple, and full
A90--A9a tail.

The reviewed oracle was subsequently submitted once on VX900 under the
exclusive GPU lock.  Binary SHA-256 was
`185d067b7e3dece934670a651f2eec4079126fd2c1b5c66579460625fecce943`.
It returned zero with host BGRA `ff000000`: both the scaled DP3-survival lane
and the SUB/CMP strict-positive lane were zero, proving consistent default
FTZ behavior for this exact producer/consumer boundary.  D16, exterior, and
all BO guards passed; X and `/dev/dri/renderD128` remained present and the
immediate dmesg delta was empty.  Logs are retained in
`/root/tmp/chrome9-live/cel-ftz-20260924/`.  This does not qualify a
denormal-preserving execution mode or comparisons with unbounded overflow.

Before enabling the whole shader, use three guarded stages:

1. vec3 `fge` select with a varying positive-normal predicate around `.1`,
   checking all three independently different output lanes;
2. vec3 `flt` select with the dynamic `.1 + .3*maxNdotL` threshold; and
3. nested `iand` selection with the four truth combinations, bracketed by
   known-good draws and checking D16, exterior pixels, BO guards and engine
   counters.

Then compile the complete linked shader offline and assert its native
record count, physical temporary high-water, A90--A92 controls, both direct
varying bits, and byte-exact Linux production records before one bounded
scene draw.  Do not infer that fit merely from the source node count: vector
assembly and legalized three-source constant ports add records, while the
physical allocator must keep peak live values within r0--r11.

The focused offline Mesa/kernel regression should construct a scalar
predicate TEMP plus two independently sourced vec3 arms, then compare every
native word above.  It should assert that Mesa never emits XXXX in the
inactive lane for this vec3 shape and that the kernel reconstruction retains
XYZ0 plus destination mask XYZ.  The UAPI need not reject every alternative
inactive selector globally; byte-exact compiler output is the narrower
contract here.  Negative cases must reject an actually undefined *live* arm
lane and a direct CMP naming three distinct constants.  A second vector must
swap the arms without changing A/C/B placement.  These tests close compiler
packing; the guarded silicon stage is still required for lane-wise numerical
select.

## Ranked implementation consequence

Mesa Chrome9 commit `722d91b` now implements the first deliberately narrow
part: a generic defined-finite/nonnegative DAG proof and vector
`fge(x, positive-normal constant)` lowering, with raw signed inputs,
zero/negative thresholds, and exceptional-value-preserving producers kept
closed.  Replaying the exact linked object after that commit identified its
`fmax(NdotL,0)` carrying `preserve:inf,nan`.  Commit `d56f61f` admits that
operator only after both fmax inputs prove no defined exceptional value,
making the preservation request vacuous; the exact first cel predicate is
now covered by the host shape regression.  The next useful change is dynamic
positive-normal FLT, followed by the zero-threshold signed-unit proof and
side-effect-free Boolean-AND rewrite.
This machinery remains separate from the narrower FRC threshold proof used
by `conditionals` and from the projected-depth proof used by `shadow`.

## Next bounded compiler contract

The remaining three predicates can be admitted without an unrestricted
ordered-compare lowering.  The next implementation should track two more
generic facts:

1. a normalized-vector component produced as
   `x_i * rsq(dot(x,x))`, with the same source/swizzles on both dot operands
   and no exceptional-value preservation request; and
2. a positive-normal floor produced as `positive_normal + nonnegative`,
   again with no exceptional-value preservation request.

From those facts, a dot of two proved normalized vectors is a bounded signed
value, while `.1 + .3*max(NdotL,0)` has a positive-normal floor.  They justify
only these source forms:

```text
flt(positive_normal_constant, nonnegative)
flt(bounded_normalized_dot, positive_normal_floor)
flt(+0, bounded_normalized_dot)
```

The first covers `.5 < attenuation*specular`.  The second covers the outline
test.  The last uses the live DP3/default-FTZ result above for `0 < NdotL`.
All three still reject direct unbounded varyings, exceptional-preserving
producer nodes, and a dynamic threshold lacking the positive floor.

Native CMP selects C when the subtraction condition is nonnegative.  For
`flt(a,b)`, emit `SUB(a,b)` but place the source true arm in CMP B and the
false arm in C.  Equality then follows the false arm, so this is strict less
than rather than less-or-equal.  Keep the existing live-lane predicate
broadcast (`XXX0` for vec3) and inactive ZERO arm selectors.

The final Boolean is value-only NIR.  The exact safe transformation is:

```text
bcsel(iand(a,b), t, f) -> bcsel(a, bcsel(b,t,f), f)
```

Require scalar Boolean `a` and `b`, side-effect-free SSA arms, and no
terminate/discard dependence.  Regression cases must cover all four truth
combinations and reject vector predicates.  After those pieces, the exact
linked cel NIR must be recompiled and measured; source-level algebra alone
does not prove its 12-physical-temporary fit.
