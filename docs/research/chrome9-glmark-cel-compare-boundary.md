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

For this exact linked NIR, every comparison leaf is a finite uploaded
varying or finite literal, or is produced by arithmetic which does not
preserve an exceptional result.  The comparison nodes request
`preserve:inf` but not denormal preservation; the shader-wide float-control
mode is zero.  That makes a bounded *default-mode defined-finite producer*
analysis the smallest generic successor: admit a predicate only when its two
operands are defined finite on every defined path, use the established FTZ
comparison behavior, and continue rejecting direct defined infinities,
preserve-denorm modes, and NaNs.  This proof must follow NIR definitions; a
finite upload check alone is insufficient because intermediate arithmetic
may request exceptional-value preservation.

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

The first useful change is not another ALU opcode.  It is a generic,
fail-closed vector compare/select lowering with a defined-finite/default-FTZ
domain proof, followed by the side-effect-free Boolean-AND rewrite.  This
same machinery is reusable by finite lighting shaders; it must remain
separate from the narrower FRC threshold proof used by `conditionals` and
from the projected-depth proof used by `shadow`.
