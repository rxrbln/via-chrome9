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
That flag constrains a comparison when one of its own defined inputs is
infinite; it does not retroactively add infinity preservation to its producer
DAG. Mesa `nir.h` specifies that an ALU result is undefined when an input or
result is infinite unless that ALU itself carries `preserve:inf`. In the
captured shader, `frcp`, `fmul`, and `fadd` carry no such flag; only the two
`flt` records do. This scene has a useful narrow domain:

* the sampled X8Z24 depth value is finite and lies in `[0,1]`;
* the first compare is exactly finite zero versus finite W;
* the second compare has a finite sampled left operand; if projection or
  bias would produce infinity, its unqualified producer has undefined output,
  so there is no defined infinite right operand which `flt` must preserve;
* a NaN from projection or bias is outside the local contract because neither
  comparison requests `preserve:nan`.

There is therefore no remaining infinity prerequisite for this exact shadow
shader. Finite ordered comparison remains required. Subnormal comparison is
a separate execution-mode discriminator, not a default-mode prerequisite.
The first predicate can receive a finite
subnormal W directly from the validated varying stream, so `0-W` can be
exactly `-minsubnormal`; the live gate below shows that VX900 flushes this
case before CMP.  However, this shader has
`float_controls_execution_mode=0`, and the Chrome9 backend rejects every
explicit preserve-denorm, FTZ, or rounding request.  Its fixed default mode
may therefore flush.  A shader requesting denorm preservation remains
unsupported.

Do not infer a broad `flt`/`fge` lowering from that default-mode allowance.
Arbitrary operands can form `Inf-Inf`, comparisons may request NaN
preservation, and the captured shadow comparisons separately request
`preserve:inf`. The exceptional-value probes below remain useful before
promoting a broader generic comparison rule, but they do not gate this exact
producer DAG.

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
* Under an explicit preserve-denorm mode, `0 < minsubnormal`,
  `-minsubnormal < -0`, and `largest-subnormal < min-normal` all require one.
  Every corresponding difference is exactly `-minsubnormal`; these are the
  shortest flush-to-zero discriminators.  Default mode may flush them.
* finite versus `+Inf`/`-Inf` requires the mathematical sign of the infinite
  difference. This is required by a broader generic comparison whose inputs
  can be defined infinities, but not by shadow's unqualified producer DAG.
* equal infinities form NaN in the first record.  A general comparison with
  `preserve:inf` must still handle those inputs, but shadow's finite left
  operand and finite uploaded W exclude them.
* a comparison which requests `preserve:nan` must return the NIR operation's
  ordered/unordered result for a NaN in either source.  Shadow does not request
  that contract, so it must not be used to promote the general case.

### Minimal denormal-mode discriminator

To characterize the fixed hardware mode and reject an incompatible explicit
preserve request, use one two-ALU guarded semantic oracle with the
already-defined SUB and CMP operations.  Upload:

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

`tools/re/chrome9_cmp_guard_smoke.c --infinity-sign` now implements the first
five-ALU case.  It constructs `+Inf` and `-Inf` through RCP of uploaded signed
zeros, subtracts each from finite zero, and checks the expected alternating
true/false color `40bf40bf`.  It retains the existing D16, exterior, and guard
checks and builds with strict host warnings. The bounded run result follows.

### Live infinity-sign result, 2026-09-24

The reviewed binary with SHA-256
`f530ac9ec59ba33d06118d39303d8ddd8447578ff420414d9197ffae415b1e36`
was run once between passing known-good baselines on the VX900 target. The
draw retired and all sampled lanes selected the `.75` true arm: actual host
BGRA was `bfbfbfbf`, not the alternating `40bf40bf`; D16 was `8000`, the
checked guards passed, and the bounded kernel log contained no new fault.

This is a negative result for the complete
`RCP(signed zero) -> 0-result -> CMP` exceptional-value path. It does not yet
locate the loss: `RCP(-0)` may lose its sign, reciprocal zero may use a
finite/saturated convention, or SUB may fail to preserve the required sign.
Do not use this gate to promote an unrestricted comparison over defined
infinities. Conversely, do not reject the exact shadow shader or claim that
ordinary finite RCP/SUB is broken; both are outside what this single
exceptional-value result distinguishes.

The next bounded discriminator is `chrome9_cmp_guard_smoke --rcp-sign`.
It removes SUB entirely and constructs one TEMP with four partial native RCP
writes from `(+0,-0,+2,-2)`, followed by the already-live sign CMP. The
proprietary RCP emitters do not inspect runtime constant bits or select a
value-dependent opcode: Linux `stmPPS_RCP_PS20_invi` at `0x0016abe0` and
Windows `0x10219920` both emit subop 6 with the source selector and ordinary
destination mask. The earlier infinity gate already showed that partial
TEMP RCP of zero retires, while the finite +/-2 lanes avoid exceptional
arithmetic and control the same native subop and CMP orientation.

With false `.25` and true `.75`, sign-preserving RCP predicts RGBA
`(.25,.75,.25,.75)`, host BGRA `bf40bf40`. The result separates the main
possibilities without another matrix:

* zero lanes false/false and finite lanes false/true: RCP loses `-0` only;
* both zero and finite pairs false/true: RCP preserves sign and the preceding
  SUB path lost the distinction;
* both pairs false/false: native RCP does not preserve negative sign in this
  path, so it cannot implement an unrestricted IEEE-sign reciprocal path;
* any other pattern is an unclassified RCP/CMP behavior and remains closed.

The five-record test retains the existing BGRA+D16 guard/exterior checks and
builds with strict warnings. It has not been submitted to the GPU.

### Shadow-domain refinement

The native frontend is a D3D9 PS 2.x compiler. Microsoft's instruction
contract says that RCP of zero yields infinity and that the scalar result is
replicated, but it does not promise that `-0` produces negative infinity:
[`rcp - ps`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/rcp---ps).
The all-true live result is therefore compatible with the native D3D
contract even though it is insufficient for an unrestricted NIR
`preserve:inf` lowering.

The exact glmark2 shadow graph is narrower. The inner projected comparison is
observed only when the outer predicate proves `ShadowCoord.w > 0`; zero,
negative zero, and every negative W select the outer false arm. The installed
2023.01 scene builds `LightMatrix` as bias * perspective * lookAt from eye
`(0,3,2)` to the origin, with near/far `2/50`, and feeds ground vertices
`(x,y,0,1)` for x,y in [-1,1]. Therefore at all four vertices:

```text
d = ShadowCoord.w = (13 - 3*y) / sqrt(13)
  in [10/sqrt(13), 16/sqrt(13)] = [2.7735, 4.4376]

ShadowCoord.z / w = 25/24 - (25/12)/d
                  in [0.2905, 0.5722]
```

Both are strictly positive normal values, and perspective-correct
interpolation over the quad preserves that bounded interval. Thus the
default scene itself never exercises signed-zero RCP or an infinite
projection. This is useful validation scope, not permission for a scene-name
special case. The generic reason this exact shader is bounded is its NIR
dataflow: submitted varyings and samples are finite, while any infinity
produced by the unqualified RCP/MUL/ADD chain makes that producer result
undefined. A different shader that feeds defined infinities directly to
`flt`, or whose producers request infinity preservation, still requires an
exceptional-value gate.

### Runnable subnormal gate

`tools/re/chrome9_cmp_guard_smoke.c` now implements the two finite
discriminators above as `--subnormal-lt` and
`--subnormal-lt-reverse`.  Both modes use exactly two semantic records:
full-lane `SUB t0,c0,c1`, followed by
`CMP oC0,t0,c2.xxxx,c2.yyyy`.  Packing false `.25` and true `.75` into one
constant register avoids the independently recovered three-distinct-register
limit on a single ALU record.  The forward/reverse expected host BGRA words
are `bfbf40bf` and `4040bf40`, respectively.

The tool builds under strict host warnings with the current UAPI.  It retains
the existing BGRA+D16 leading/trailing guard checks, triangle-exterior check,
and bounded render-node interface.  The forward case was initially held for
independent review.  A timeout, engine quarantine, or negative numeric result
ends the experiment; do not run the reverse case afterward.

### Live forward-gate result, 2026-09-24

The strict host build of the reviewed probe (SHA-256
`1a29e12623d283e36a784343af69e915b35dd437376e7a505b2e8ef041e66166`)
was staged at `/root/tmp/chrome9-live/cmp-oracle/chrome9-cmp-subnormal` on the
192.168.2.60 VX900 target.  A known-good `--cmp-temp-true` draw passed before
and after the new draw.  The single bounded `--subnormal-lt` draw returned
normally but failed its pixel check: at (12,12) the actual BGRA host word was
`40404040`, not the strict-FP32 expected `bfbf40bf`; D16 was `8000`.
Leading/trailing BO guards passed their check before the pixel mismatch was
reported.  The bounded kernel log check found no new Chrome9 fault.  The
reverse case was **not** run after this negative forward result.

All four lanes selecting the `.25` false arm is consistent with loss of the
subnormal difference before CMP, but this probe alone does not distinguish
input denormal flushing, arithmetic result flushing, or another shader
numeric behavior. The proposed generic strict-`<` lowering is therefore
**not qualified for denormal-preserving execution modes**. This does not by
itself reject the default-mode glmark2 shadow shader: its NIR requests
`preserve:inf` locally but no denormal-preserving execution mode, and the
Chrome9 backend already rejects nonzero `float_controls_execution_mode`.
The exact shadow producer DAG has no defined-infinity input to its compares,
so this denormal result is the only FP-mode qualification it needs: default
mode may flush, preserve-denorm mode remains rejected. Static proprietary SLT
bytes do not override the live subnormal counterexample.

### Proprietary mode/control conclusion

Both proprietary comparison implementations have the same denormal exposure
as the semantic candidate.  Linux public SGE at `0x0016ad60` and internal SLT
at `0x001644e0` first emit `src0 + -src1`; their Windows counterparts are
`VTGUModeDX32A.dll:0x10219700` and `0x10216550`.  The second records differ in
literal-arm placement, but neither path compares the original operands
directly.

No recovered compiler entry point accepts a floating-point mode argument.
Constant values do not change ALU records or A90--A9b, and static control RE
assigns A90--A9b to resource counts and ALU/TAU scheduling rather than a
denormal selector.  The live common state therefore characterizes a fixed
default mode; there is no source-backed state bit to switch into
preserve-denorm execution.  The old pre-Chrome9 `HC_HenCPUDAZ_MASK` name is
not used by either Chrome9 H5 compiler/runtime packet and is not evidence of
a programmable Chrome9 PS mode.

### Full finite shadow-program gate

`tools/re/chrome9_shadow_fragment_guard_smoke.c` contains the complete
source-equivalent projected comparison as eight semantic records: RCP,
projective MUL, biased-depth ADD, X8Z24 TEX, two SUBs, and two nested CMPs.
The kernel adds the required TEMP-coordinate bridge, producing the same
eight-ALU/one-TAU production schedule.  Three guarded rectangles distinguish
sampled depth below the threshold, sampled depth above it, and the outer
`W <= 0` arm.  Expected gray values are `.15`, `.30`, and `.30`; the test also
requires D16 near `8000`, untouched exterior pixels, intact color/depth/source
guards, and a byte-identical X8Z24 source.  It builds under strict warnings.
The failed generic infinity-sign characterization is not a prerequisite.

### Independent packet and guard review

The oracle was re-reviewed against the current Chrome9 UAPI and ordered-field
front-end builder before any silicon submission.  It is a bounded **GO** for
one fresh-boot run, with an immediate stop on timeout or engine quarantine:

* varying T0 is float4 because the projection consumes W; the computed TEX
  coordinate is a distinct XY01 TEMP source, so the kernel inserts the same
  TEMP-to-coordinate bridge and ordered T0/T1 field image as the proprietary
  `(2,4,1,1)` object;
* both CMP records use one uploaded constant register.  They do not exercise
  the independently rejected three-distinct-constant-register case;
* the three rectangles separately require inner true, inner false, and outer
  `W <= 0` behavior, so a constant arm or missing short-circuit select cannot
  satisfy the checker;
* BGRA, D16, two exterior points, all color/depth/source guard pages, and the
  complete X8Z24 source BO are checked after retirement.

A strict host build with `-std=c11 -Wall -Wextra -Werror` completed; the
review binary SHA-256 is
`b906316cbf9bac02324bd29a6515be4d2d9dfc3e55ff581195c31e08d30c6c1a`.
This review qualifies only the finite/default-mode scene domain described
above.  It deliberately does not reinterpret the negative subnormal or
infinity-sign results as passing general comparison coverage.

### First full-program silicon result

The reviewed oracle was submitted once on VX900 between passing known-good
texture baselines.  It retired without a kernel fault, preserved the complete
X8Z24 source and every checked guard, and wrote D16 `8000` at the samples.
The high-depth rectangle and the outer `W <= 0` rectangle both produced their
expected unshadowed host word `ff4d4d4d`.  The low-depth rectangle alone was
wrong: it also produced `ff4d4d4d` instead of the expected shadowed
`ff262626`.  The post-run texture baseline passed.  Tools commit `6049abd`
prints all three rectangle results for subsequent discriminators.

This is a finite correctness failure, not a timeout or proof that the whole
packet class is unsafe.  The observation only says that the inner condition
was not negative for the low-depth sample; the first run does not distinguish
projected-Z transport, computed-coordinate sampling, SUB orientation, or CMP
selection.  Keep each cause open until one source-backed draw exposes the
intermediate values.  The most compact discriminator retains the same
projective prefix and FE route, then writes projected Z, sampled X, and their
biased threshold into separate color lanes.  For the low-depth rectangle the
expected tuple is approximately `(.4,.2,.5505,1)`.

### Partial-output discriminator timeout and revised isolation

The first attempt to expose that tuple was not a valid isolation of the
projective prefix. It retained RCP, projective MUL, threshold ADD and the
computed-coordinate TEX, then replaced the comparison tail with three direct
COLOR stores:

```text
MOV oC0.xz, threshold.xx
MOV oC0.y,  sample.x
MOV oC0.w,  c0.w
```

On 2026-09-24 that reviewed `--inspect-prefix` binary timed out at the cmd22
cookie and quarantined the engine. The target operating system remained
reachable, but no post-draw baseline or further GPU submission was attempted.
The persistent target log is
`/root/tmp/chrome9-live/ideas-kill-20260924/shadow-prefix-timeout-dmesg.txt`;
the copied local artifact is `/tmp/chrome9-shadow-prefix-timeout-dmesg.txt`.
The fault was:

```text
phase=cookie seq=1561 kind=22 flags=0x101 epoch=101
current=0x8900 pause=0x88f0 stop=0x8970
cr=80000000 engine=00001711
```

Reconstruction against the current kernel encoder gives seven ALUs and one
TAU, with the established phase controls:

```text
01c00530 b01c0053 0010c0b6 00060020
0140b6d0 30000000 0072c005 00000060
00800000 20140924 00100149 00000020
01c00530 c01c0053 00f20106 000d0460  # TEMP-coordinate bridge
01c00530 501c0053 00520014 000d0820  # oC0.xz
01c00530 d01c0053 002202a2 000d0820  # oC0.y
01c00530 b01c0053 008080b6 000d0820  # oC0.w
TAU 301211b0
A90..A95 90058001 91000002 92010007 93000001 94000000 95000008
```

The timeout does **not** prove that RCP, projection or the computed TEX prefix
wedged. Three disjoint partial oC0 stores were introduced at the same time.
That is an independently unsafe variable: the corrected T2 split-output
probe timed out with four disjoint partial COLOR stores, and the old vector
sqrt probe timed out with X and YZW COLOR stores, while their safer scalar
successors end in one full oC0 write. The proprietary RGB/W objects in
`shader-upload-oracles.md` and `ioquake-texture-environment-combine.md` prove
compiler bytes, not silicon execution; the former explicitly labels its
two-store RGBX sequence an unsubmitted candidate. Therefore do not use the
failed run as evidence about projected-Z transport and do not retry it.

This evidence is sufficient for a fail-closed runtime boundary even though it
does not prove an architectural ban: accept exactly one full-mask oC0 store,
as the final semantic instruction, until a dedicated partial-output oracle
passes. Current Mesa already requires one full `store_output` and appends one
terminal full-mask COLOR instruction, so this boundary removes only raw,
unqualified diagnostic forms. KILL may still precede the terminal store.

The next prefix discriminator must avoid partial COLOR entirely and must not
extend the projected-coordinate TEMP lifetime across TEX. One bounded shape
reconstructs projected Z from the already-live threshold after sampling:

```text
t4 = (threshold, 0, 0, 0) - (bias, 0, 0, 0)
t5 = t4 + (0, sample, 0, 0)
t6 = (0, 0, threshold, 0) + (0, 0, 0, 1)
oC0 = t5 + t6
```

Use full XYZW destinations for all three packing temporaries and the single
final COLOR store. Unlike the first candidate, this preserves the retiring
full shadow program's T1 last use at the computed-coordinate TEX and does not
keep T1 live into the post-TAU ALU phase. Current allocation gives physical
high-water four and this source-exact candidate:

```text
01c00530 b01c0053 0010c0b6 00060020
0140b6d0 30000000 0072c005 00000060
00800000 20140924 00100149 00000020
01c00530 c01c0053 00f00106 000d0460  # bridge
800016d0 d0140924 00f08016 00000060  # t4
0202a2d0 30140924 00f00105 000000e0  # t5
0080b6b0 50140924 00f200b4 00000060  # t6
02010530 30140924 00f20305 00000820  # one full terminal oC0
TAU 301211b0
A90..A95 90058001 91000002 92010008 93000001 94000000 95000008
```

For the first rectangle its expected logical RGBA is approximately
`(.4,.2,.5505,1)`, host BGRA word `ff66338c` within one UNORM byte. These
bytes still require a focused compiler self-test and an independent packet
review before any fresh-boot submission; they are a safe offline next gate,
not authorization to run it now.

### Narrower production-prefix differential

There is a smaller source-backed discriminator than exporting the prefix.
The raw oracle currently writes only XYZ in its projective multiply:

```text
MUL t1.xyz, T0.xyzw, rcpW.xxxx
```

Its current native destination mask is therefore `7` in
`0140b6d0 30000000 0070c005 00000060`. The exact proprietary shadow object
above instead writes full XYZW in the same operation:

```text
0140b6d0 30000000 00f0c005 00000060
```

That is the only production-prefix difference which directly explains the
observed branch while preserving the known-correct sample coordinate. X and Y
must have reached the sampler closely enough to fetch `.2`; if the partial
vector result loses only Z, the threshold becomes bias `.1505`, so `.2` is
unoccluded and the oracle emits the observed `.30`. This is a hypothesis tied
to the exact failed pixel, not proof that partial TEMP masks generally lose Z.

The next cold-boot shadow discriminator should therefore change only the
projective MUL destination from mask `7` to `f`, leaving the original nested
comparison graph, sampler, vertices, constants, and single full terminal
COLOR write unchanged. For finite nonzero W its newly written W is exactly
one and is otherwise dead. This one-word change aligns the prefix with the
Linux production object and avoids every partial-COLOR hazard. It still needs
independent packet review and the usual stop-on-timeout discipline before a
single hardware run.

The reviewed mask-only build was run once on the safe single-terminal-output
kernel. It retired without a timeout or new Chrome9 fault, and the succeeding
known-good texture baseline passed. The low-depth rectangle was unchanged:
host BGRA remained `ff4d4d4d` with D16 `8000`, instead of `ff262626`.
Therefore widening the projective MUL to full XYZW is **not sufficient**, and
the lost-Z-from-mask hypothesis above is falsified as the complete cause. Keep
the full vendor mask in future packets, but do not repeat this discriminator.

The next exact isolation can expose threshold and sampled depth without any
partial COLOR or extra TEMP packing. Retain RCP, full projective MUL,
threshold ADD and computed TEX, then replace the complete compare tail with
one full-mask terminal ADD:

```text
ADD oC0, threshold.x000, sample.0x00
```

This produces logical RGBA `(threshold,sample,0,0)`. The current encoder's
source-exact terminal record is:

```text
0202a2d0 d0140924 00f20016 00000820
```

The complete object has five ALUs, one TAU and controls
`90058001/91000002/92010005/93000001`, with A95=`95000008`. Expected host
BGRA words for the three guarded rectangles are approximately
`008c3300`, `008ccc00`, and `008c3300`, respectively.  For W=-1 the submitted
projective Z is `-.4`; RCP(-1) restores projected Z `+.4`, so the threshold is
still `.5505` and the sample is still `.2`.  The earlier `00003300` prediction
incorrectly omitted that reciprocal.  Likewise word1 `50140924` would encode
the threshold source as `(X,ZERO,ONE,ZERO)` and write blue one; the corrected
`d0140924` encodes `(X,ZERO,ZERO,ZERO)`.  D16 still distinguishes a black-alpha
diagnostic value from no draw.
This is the smallest source-backed separator between projected-Z/threshold
transport and X8Z24 sample-lane transport; it needs a focused compiler
self-test and independent packet review before a fresh-boot run.

### Live threshold/sample diagnostic, 2026-09-24

The reviewed five-instruction diagnostic was run once on the current VX900
module with `cursor_sf=Y` and `fragment_sparse_textures=Y`.  It completed
without a kernel fault, preserved D16 (`8000`), all checked source/BO guards
and the full exterior margin, and left Xorg and `renderD128` available.  It
failed the numeric check with these host BGRA words:

| Rectangle | Expected | Actual |
|---|---:|---:|
| low source depth, W=+1 | `008c3300` | `00ffcc00` |
| high source depth, W=+1 | `008ccc00` | `00ffcc00` |
| low source depth, W=-1 | `008c3300` | `00003300` |

The binary SHA-256 was
`9b7bd09144bd377912aea09a85c2bc66d810a2f280f4cdb40d42d48183b28f6a`.
The target log is
`/root/tmp/chrome9-live/shadow-inspect-20260924/inspect.log`; the immediate
dmesg delta was empty.  A previously passing sparse-s5 texture draw also
passed afterward without a fault.

The raw test initially used replicated ZZZZ and XXXX source swizzles for its
scalar threshold ADD.  Linux 5.75's exact shadow record instead normalizes
the unused lanes to Z000 and X000.  A second, independently reviewed draw
changed only those inactive selectors; its source is tools commit `93d51f5`
and its binary SHA-256 is
`9258d0b1780f267fb7a92d218d751fb8d023b7706bed17d14bc353a3b995d427`.
The three actual colors were unchanged, and its immediate dmesg delta was
again empty.  The scalar inactive-lane difference is therefore not
sufficient to explain this diagnostic failure.  Current Mesa already
zero-pads scalar ALU sources, so the second form is the relevant one for
the production shader.

The red lane saturating to `ff` for W=+1 and to zero for W=-1 is consistent
with the old `RCP(W)` temporary being read in place of the biased projected
depth.  The green lane is also consistent with sampling at `1/W` rather than
at the projected S coordinate, but S=.25 lies on a two-texel nearest-filter
boundary; that lane alone is weaker evidence.  These are **inferences from
pixels**, not a proven allocator or FE fault.  The five-instruction diagnostic
has a smaller physical-register high-water and shorter TEMP lifetime than
the eight-instruction proprietary full-shadow object, so its A90 bucket and
bridge release bit cannot be compared to that full object as if the programs
were otherwise identical.  A source-equivalent five-instruction Linux
configuration-two replay, with per-instruction physical IDs and release
bits, is the next offline gate.  Do not change A28, the computed bridge, or
the resource bucket based on these pixels alone.
