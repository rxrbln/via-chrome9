# glmark2 shadow: depth image preflight and next shader boundary

The `shadow` scene creates an unsized `GL_DEPTH_COMPONENT` / `GL_UNSIGNED_INT`
2D texture, changes its minification filter to `GL_NEAREST`, and attaches level
zero to a depth-only FBO. The Chrome9 format choice is `PIPE_FORMAT_X8Z24_UNORM`
when `CHROME9_EXPERIMENTAL_X8Z24=1`.

Before the Mesa state-tracker fix, the target log showed
`can-create ... X8Z24 ... last=9 ... bind=0`, followed by
`GL_OUT_OF_MEMORY in glTexImage2D(image too large)` and FBO status `0x8cd6`.
This was not an actual 640x480 mipmapped allocation. Real `glTexImage2D`
calls `st_TestProxyTexImage` with a proxy target; the proxy object's default
mipmap filter caused a full-chain estimate. `allocate_full_mipmap` for an
ordinary mutable depth image instead selects level zero. Mesa's proxy
preflight now uses `last_level=0` for mutable level-zero formats containing a
depth component, while immutable `numLevels>0` probes retain their requested
level count. Chrome9's actual multilevel X8Z24 rejection remains in place.

The rebuilt library was tested on the VX900 target with one bounded
`glmark2 --validate --off-screen --frame-end=readpixels -b shadow:nframes=1`
run. The final log at
`/root/tmp/chrome9-live/depth-proxy/shadow-final.log` shows the proxy
`can-create` query and actual resource creation both at `last=0`, followed by
a depth sampler view and no TexImage or FBO error. The scene is not rendered
correctly yet: the draw is rejected as an unsupported bounded fragment
program, and glmark reports `Validation: Unknown` for this one-frame variant.
The target's X server remained running and no new GPU fault appeared in dmesg.

The unsupported fragment NIR is captured at
`/root/tmp/chrome9-live/depth-proxy/shadow-nir.log`. It computes a projected
coordinate (`frcp` of input W), samples the depth texture, tests both positive
W and `texture.x < projected_z + 0.1505`, combines those booleans with `iand`,
and selects RGB `(0.15, 0.15, 0.15)` or `(0.30, 0.30, 0.30)`. This is the next
compiler/ISA task; FBO completeness alone is not a shadow-scene pass.

Keep actual multilevel X8Z24 fenced off until its rendering and sampling ABI
is implemented: the fragment kernel currently requires the depth BO size to
match the level-zero allocation exactly, and Mesa rejects depth mip sampling.

## Exact fragment boundary

The captured NIR is a straight-line program.  In compact form it is:

```text
sc       = T0 * rcp(T0.wwww)
limit    = sc.z + 0.1505
sample   = TEX2D(s0, sc.xy).x
positive = flt(0.0, T0.w)       // preserve:inf
occluded = flt(sample, limit)   // preserve:inf
rgb      = bcsel(positive & occluded, vec3(0.15), vec3(0.30))
oC0      = vec4(rgb, 1.0)
```

The source is `data/shaders/shadow.frag` from glmark2 2023.01, SHA-256
`7077ca148b89fec91add2e1c3a82d79e24df57752c2159f62aec9025878d9fdf`.
Its two source-language predicates are `ShadowCoord.w > 0.0` and
`light_distance < sc_perspective.z`.  Material diffuse is constant 0.30 in
this scene, explaining the 0.15/0.30 NIR arms.  The reciprocal, projection,
bias, computed-coordinate sample, and final vector construction already map
to implemented semantic operations.  The unsupported part is precisely the
two `flt` values, integer `iand`, and their `bcsel` consumer; it is not another
depth-format or FBO failure.

The boolean graph does not need a materialized integer AND.  Because native
CMP selects its C source when A is nonnegative and its B source when A is
negative, the faithful fused graph is:

```text
inner = CMP(sample - limit, false=.30, true=.15)
oC0   = CMP(0.0 - T0.w, false=.30, true=inner)
```

Equality selects the false arm in both records, preserving both strict `<`
tests.  The outer false arm also implements short-circuit conjunction: when
W is nonpositive, it ignores `inner` and returns 0.30.  This is an exact DAG
rewrite, not a generic lowering of `iand`, and it must be recognized from the
resolved scalar definitions rather than enabled for arbitrary integer logic.

## Production Linux object

`tools/chrome9_shader_corpus.py --case glmark_shadow_nested_cmp_compact`
contains the complete PS 2.0 token oracle.  It was replayed through the Linux
5.75 compiler and configuration-two finalizer in
`via_chrome9_dri.so` (SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`).
The relevant exported emitters are `stmPPS_TEXLD_PS20_invi` at `0x00167e90`,
`stmPPS_RCP_PS20_invi` at `0x0016abe0`, `stmPPS_SUB_PS20_invi` at
`0x0016bc40`, `stmPPS_MUL_PS20_invi` at `0x0016efe0`,
`stmPPS_CMP_PS20_invi` at `0x00171fd0`, and `stmPPS_ADD_PS20_invi` at
`0x00172310`.  The resulting active-resource tuple is `(2,4,1,1)` and the
finalized schedule is four ALUs, one TAU, then four ALUs:

```text
01c00530 b01c0053 0010c0b6 00060020
0140b6d0 30000000 00f0c005 00000060
008016d0 d0140924 00100156 00000020
01c00530 101c0053 00f00104 000d0460
808116d0 b0140924 0012c06d 00000060
820216d0 00140924 00100000 000000e0
00830530 00082053 00f20300 00090020
02000530 00082053 00f20100 00090820
TAU 301211b0
A90-A9a:
90050001 91000002 92010008 93000001 94000000 95000008
96000000 97000000 98000000 99000000 9a000000
```

The fourth ALU is the compiler-inserted TEMP-to-coordinate bridge; A95 bit 3
switches from ALU to TAU and A93 bit 0 returns to the remaining ALUs.  Direct
T0 is A90 bit 0, while computed coordinate slot one is A91 bit 1.  This exact
object proves the schedule, resource bucket, phase controls, strict operand
orientation, and nested-CMP shape.  It does not prove exceptional numeric
behavior on VX900.

## Bounded acceptance and exceptional-value gate

Both captured comparisons carry `preserve:inf` but not `preserve:nan`.
Therefore an implementation may leave NaN-input behavior undefined, but it
must preserve comparisons involving infinity.  This scene has a useful
narrow domain:

* the sampled X8Z24 depth value is finite and lies in `[0,1]`;
* the first compare is exactly finite zero versus W, so it never subtracts
  two infinities;
* the second compare has a finite left operand, so an infinite limit also
  never produces `Inf-Inf`;
* a NaN from projection or bias is outside the local contract because neither
  comparison requests `preserve:nan`.

The remaining silicon questions are finite-versus-infinity sign handling and
whether a nonzero **subnormal difference retains its sign**.  They are not a
general IEEE NaN requirement for this shader.  The first predicate can receive
a finite subnormal W directly from the validated varying stream, so `0-W`
alone can be exactly `-minsubnormal`.  If SUB flushes that value to `-0`,
native CMP treats the condition as nonnegative and changes a strict-true
result to false.  The sampled-depth predicate may admit a stronger spacing
proof after its exact conversion and projection ranges are analyzed, but that
does not discharge the W predicate.  Do not broadly lower `flt`/`fge`:
arbitrary operands can additionally form `Inf-Inf`, and comparisons which
request NaN preservation have a stronger contract.

### Exact proprietary SLT mapping

The separately recovered internal proprietary SLT emitter confirms the
materialized strict-less-than shape.  For the defined TEMP chain
`SLT r2,r0,r1`, the two finalized records are:

```text
02010538 30000924 00f20005 000000e0
01409240 30140b6d 00f20305 000900a0
```

The first record computes `r0 - r1` into dynamically allocated scratch r3.
The second is native subop-9 CMP with A=`r3` plus release, C=native ZERO,
B=native ONE, and destination r2.  It therefore implements:

```text
d = x - y
result = d >= 0 ? 0 : 1
```

Scratch r3 is an observed allocation for this populated chain, not a fixed
architectural register.  The kernel allocator must reserve any free physical
temporary across both records and release it only in the second.  Isolated
constant/varying probes which happened to allocate r12 are not executable
register-allocation contracts.  The complete encoding and provenance are in
`chrome9-generic-float-compare-boundary.md`.

This two-record object fixes opcode, operand order, literal source class,
scratch lifetime, and strict equality orientation.  Static bytes cannot prove
subnormal, infinity, or NaN arithmetic.  In particular:

* `+0 < -0` and `-0 < +0` both require zero.  Existing CMP tests show either
  signed zero condition selects the nonnegative arm, so this passes if SUB
  produces either zero sign.
* `0 < minsubnormal`, `-minsubnormal < -0`, and
  `largest-subnormal < min-normal` all require one.  Every corresponding
  difference is exactly `-minsubnormal`; these are the shortest flush-to-zero
  discriminators.
* finite versus `+Inf`/`-Inf` requires the mathematical sign of the infinite
  difference.  This is the infinity shape used by shadow.
* equal infinities form NaN in the first record.  A general comparison with
  `preserve:inf` must still handle those inputs, but shadow's finite left
  operand and finite uploaded W exclude them.
* a comparison which requests `preserve:nan` must return the NIR operation's
  ordered/unordered result for a NaN in either source.  Shadow does not request
  that contract, so it must not be used to promote the general case.

### Minimal subnormal oracle

Before accepting the shadow nested select, add one two-ALU guarded semantic
oracle using the already-defined SUB and CMP operations.  Upload:

```text
x = (+0, +minsubnormal, -minsubnormal, largest-subnormal)
y = (+minsubnormal, +0, -0, min-normal)
d = x - y
oC0 = CMP(d, false=.25, true=.75)
```

The exact FP32 input bits are:

```text
x = (00000000, 00000001, 80000001, 007fffff)
y = (00000001, 00000000, 80000000, 00800000)
```

All constants pass the kernel's finite-value check.  The strict results are
`(true,false,true,true)`, so identical `.25/.75` arm vectors produce host
BGRA8888 `bfbf40bf`.  Reverse x and y in a second draw; the results are
`(false,true,false,false)` and the expected host word is `4040bf40`.
Use the same constant register for both CMP arms through scalar swizzles, so
the gate cannot trip the unrelated three-distinct-constant port hazard.

This semantic SUB+CMP gate exercises the same arithmetic and native subop-9
condition as proprietary SLT but not its native file-5 ZERO/ONE literals.  A
future semantic SLT opcode still needs an offline byte-exact kernel test for
both records and scratch lifetime.  The numeric gate can run first because
the condition path is independent of which finite values occupy the arms.

Two checked-in offline cases define a minimal guarded follow-up without
uploading nonfinite constants, which the kernel correctly rejects:

* `ordered_cmp_infinity_sign` uploads only `+0` and `-0`, computes their
  reciprocals on-chip, forms `0-result`, and feeds the two opposite-signed
  conditions to CMP.  With false color c1 and true color c2, its expected
  lanes are `c2.x, c1.y, c2.z, c1.w`.  The production object is five ALUs,
  no TAU, `A92=92000005`, and all other phase controls zero after
  `A90=90058000`.
* `ordered_cmp_nan_from_infinity` computes `RCP(+0)`, subtracts the result
  from itself, and feeds that condition to CMP.  Ordered `+Inf < +Inf`
  requires the false c1 arm in all lanes.  It is a three-ALU, no-TAU object
  with `A92=92000003`.  This gate is needed before any generic comparison
  lowering that can compare two infinities, but it is not required by the
  narrower shadow-domain proof above.

A runnable guard should use the established 64x64 BGRA plus D16 layout,
positive-winding two-triangle rectangle, untouched exterior checks, leading
and trailing BO guard pages, and a known-good baseline before and after.  Run
the sign case once first; stop on timeout or engine quarantine.  Both cases
are ALU-only and use already-live RCP/SUB/CMP framing, so their additional
risk is numeric exceptional-value handling rather than a new packet class.
The reciprocal-zero result itself is not bit-readable through the normalized
render target, so a passing color result qualifies only the complete
RCP/SUB/CMP path used by the test.
