# Chrome9 glmark2 function-medium fragment gate

Status: source-exact and Mesa-26.2.2 offline-NIR audit plus guarded silicon
closure, 2026-09-24.  The corrected byte-exact scalar gates and the complete
five-step medium scene now pass.  The older combined multi-lane oracle remains
a separate timed-out packet and does not qualify vector partial writes.

## Exact scene source

glmark2 2023.01 constructs the `function` fragment shader from
`data/shaders/function.frag`, `function-call.all`, and the selected step file.
For

```text
fragment-complexity=medium
fragment-function=true
fragment-steps=5
```

the inlined scalar recurrence is:

```glsl
float d = fract(gl_FragCoord.x * gl_FragCoord.y * 0.0001);
repeat five times {
    float a = fract(3.0 * d);
    d = fract(sqrt(a * a));
}
gl_FragColor = vec4(d, d, d, 1.0);
```

`SceneFunction::validate()` expects `0x5e,0x5e,0x5e,0xff` at `(293,89)` for
five fragment steps.  The live 2026-09-24 run linked successfully and reached
the Chrome9 draw gate, which rejected it as an unsupported bounded fragment
program.  Thus the failure is not GLSL parsing, linking, vertex execution, or
scene validation setup.

## Exact optimized arithmetic NIR

The shader pair was replayed through the Mesa 26.2.2 standalone GLSL linker,
the same `gl_nir_opts()` path used by the in-tree offline compiler tests, with
the five calls present in both stages.  The fragment arithmetic was:

```text
%5  = fmul fragcoord.x, 0.0001
%6  = fmul %5, fragcoord.y
%7  = ffract %6

%9  = fmul 3.0, %7
%10 = ffract %9
%11 = fmul %10, %10
%12 = fsqrt %11
%13 = ffract %12

%14 = fmul 3.0, %13
%15 = ffract %14
%16 = fmul %15, %15
%17 = fsqrt %16
%18 = ffract %17

%19 = fmul 3.0, %18
%20 = ffract %19
%21 = fmul %20, %20
%22 = fsqrt %21
%23 = ffract %22

%24 = fmul 3.0, %23
%25 = ffract %24
%26 = fmul %25, %25
%27 = fsqrt %26
%28 = ffract %27

%29 = fmul 3.0, %28
%30 = ffract %29
%31 = fmul %30, %30
%32 = fsqrt %31
%33 = ffract %32
%34 = vec4 %33, %33, %33, 1.0
```

The offline harness represents window position as a POS input while the
Chrome9 state-tracker path retains its supported fragment-coordinate load.
That IO-form difference does not affect the arithmetic list or this gate.
The replay options have no extra lowering relevant to this shader and, like
the current Chrome9 options, leave `fsqrt` intact.

## First unsupported operation

`chrome9_nir_options` in `chrome9_screen.c` lowers division, floor/ceil,
modulo, saturation, linear interpolation, and sine/cosine, but does not set
`lower_fsqrt`.  `chrome9_ps_lower_def()` in `chrome9_context.c` accepts native
`frsq` and `frcp` and has no `nir_op_fsqrt` case.  It therefore lowers the
initial `FMUL, FMUL, FRC` and the first step's `FMUL, FRC, FMUL`, then fails
closed at `%12 = fsqrt %11`.  The remaining four roots are the same gate, not
four distinct missing features.

The smallest generic implementation is:

```c
.lower_fsqrt = true,
```

in the Chrome9 NIR compiler options, plus an offline regression asserting
that final NIR contains `frcp(frsq(x))` and no `fsqrt`.  Mesa's standard
algebraic rule in `nir_opt_algebraic.py` is exactly:

```text
fsqrt(a) -> frcp(frsq(a)) when options->lower_fsqrt
```

Do not add an application-shaped function-scene classifier or an ad-hoc
backend `fsqrt` opcode.  The standard lowering is also the path required by
refract and ioquake3 fog shaders.

## Resulting pressure

After standard square-root lowering, the scalar recurrence contains:

```text
initial coordinate expression             3 ALU
five steps, 6 ALU each                    30 ALU
vec4 assembly, scalar RGB plus alpha       2 MOV
terminal color write                       1 MOV
                                             ----
                                             36 ALU records
```

The current semantic builder assigns one virtual temporary per arithmetic
result (33) and one for the mixed-source output vector, for 34 virtual
temporaries.  This is a single dependency chain.  Only the current scalar and
its successor are simultaneously live; output assembly adds at most one more
live value, so physical pressure is approximately two of the 12 hardware
temporaries.  There are no textures, varyings, UBO loads, or control-flow
records.  Constants are only `0.0001`, `3.0`, and `1.0` and fit the immediate
packer.  The 36-record result fits the 96-ALU semantic/UAPI envelope, although
it is larger than the conservative `max_alu_instructions=32` capability value.
The existing scene already links with that advertised value; this mismatch
does not cause the current rejection but should be reconciled separately.

## Smallest guarded numerical oracle

The lowering cannot be promoted solely from compiler replay because its zero
endpoint depends on native RSQ/RCP special-value behavior.  Use one guarded,
ALU-only D16 draw bracketed by known-good draws.  Supply one float varying:

```text
x = (0, 1/16, 1/4, 9/16)
sqrt(x) = (0, 1/4, 1/2, 3/4)
```

Use partial writes to execute `RSQ; RCP` independently for X, Y, Z, and W
(eight records) into one result temporary.  Then discriminate the zero lane
without changing the finite samples:

```text
SUB  t2.x, c0.x(0), result.x
CMP  t3.x, t2.x, c0.y(1), c0.x(0)
MOV  color.x, t3.x
MOV  color.yzw, result.yzw
```

The semantic/UAPI operand order is predicate, true/C, false/B: Chrome9 CMP
selects source 1/C when the predicate is nonnegative and source 2/B when it is
negative.  The expected BGRA8888 host word (AARRGGBB) is therefore
`0xbfff4080`: exact red `1` proves the zero chain did not leave a positive
residue, while green/blue/alpha prove `.25/.5/.75` within two UNORM8 codes.
CMP must write fresh `t3.x`: the kernel rejects overlapping temporary
definitions, so overwriting the already-defined `result.x` is invalid.  The
two COLOR moves have disjoint masks and complete RGBA together.  The entire
discriminator is 12 ALU records, one varying, one constant register, no
texture, and no control flow.  Check D16 and all BO guards, require normal
retirement, and run a known-good draw afterward.  A direct-root companion
sample (without CMP) should also require the zero byte to be zero so a NaN or
unexpected sign encoding cannot be hidden solely by CMP behavior.

`tools/re/chrome9_normalize_guard_smoke.c --sqrt-lower` implements this
12-record discriminator and the `0xbfff4080` check.  It passes a strict host
compile with `-std=c11 -Wall -Wextra -Werror`; it has not been GPU-run as part
of this audit.

Passing this gate qualifies `frcp(frsq(x))` for the nonnegative `[0,1)` domain
used by this scene.  It does not qualify negative roots, arbitrary NaN/
infinity handling, or a global IEEE exceptional-value claim.  The broader
refract and fog gates remain documented in
`chrome9-refract-fragment-compiler-boundary.md` and
`ioquake3-gl2-fogpass-native-design.md`.

## Live timeout and narrowed qualification order

The first bounded `--sqrt-lower` run was bracketed by a passing known-good
baseline, but the oracle itself timed out.  The kernel reported:

```text
phase=cookie seq=16343 kind=22 flags=0x101 epoch=3337
current=0x2380 pause=0x2370 stop=0x23f0
cr=80000000 engine=00001711
```

The engine was quarantined immediately.  No post-oracle baseline or other GPU
work was attempted on that boot.  The preserved log is
`/root/tmp/chrome9-live/sqrt-lower/dmesg-after-timeout.log`, SHA-256
`16cb7fb072b539927ce2850b17110d6f668bde5013eff762b0a1013e836f29fa`.
The available ring window contains only the final ALUs and vertices, so the
cursor does not localize which scalar record stopped execution.

Static replay does not expose a resource-envelope error.  The program uses
one direct T0 resource, four physical temporaries at peak, no TAU, and the
ordinary controls A90=`90058001`, A91=`91000000`, A92=`9200000c`.  Twelve
ALUs fit the UAPI limits.  The disjoint COLOR.x and COLOR.yzw writes are legal
and occur at the tail.  However, the first ALU is `RSQ T0.x` with T0.x=`+0`,
followed by X-mask RCP.  Native RSQ/RCP partial destination masks for Y/Z/W
are compiler-byte-backed but remain independently silicon-unqualified.

The combined test therefore proves neither that zero is solely responsible
nor that partial masks are safe.  The safe next order, each as a separate
bounded gate after a cold recovery, is:

1. finite-positive, X-only `RSQ -> RCP`;
2. `+0`, X-only `RSQ -> RCP`, treated as a wedge-risk gate; and
3. finite Y/Z/W partial destination masks only after both X-only cases retire.

Until those gates separate the failure, `.lower_fsqrt` must remain disabled
and function-medium remains fail-closed.

`tools/re/chrome9_normalize_guard_smoke.c --sqrt-x-positive` implements the
first gate without the zero endpoint, CMP, or Y/Z/W partial writes.  It sends
one finite full-float T0.x value, `.25`, through X-mask `RSQ`, X-mask `RCP`,
then replicates the scalar X result to the full color output.  The Linux 5.75
configuration-two compiler gives the exact production precedent:

```text
01c00530 801c0053 0012c0b6 00070020
01c00530 801c0053 001200b6 00060060
01c00530 001c0053 00f20100 000d0820

active=(1,2,1,0) A90=90058001 A91=91000000 A92=92000003
```

The expected mathematical result is `.5`, so every BGRA8 lane must be within
two codes of `0x80`.  The tool also requires D16 near `0x8000`, an untouched
triangle exterior, and intact color/depth guard pages.  It passes a strict
host build; it has not been submitted to the GPU by this audit.  Passing this
gate would qualify only the finite-positive X-lane chain and scalar forward,
not zero, signed values, exceptional values, or partial Y/Z/W destinations.

The adjacent `--sqrt-x-zero` mode is the second, explicitly wedge-risk gate.
It reuses the exact same three semantic records, target, geometry, and checks;
the only input change is T0.x `.25 -> +0`, and the required full BGRA word is
exactly zero.  This is source-backed as an *unguarded* vendor path, not as a
safety guarantee: the symbolized Linux RSQ/RCP emitters and their Windows
twins inspect no input value bits and emit the same records for a runtime
zero as for `.25`.  They do not insert a zero test, clamp, or alternate
instruction.  The tool prints a cold-boot wedge warning before submission.
Run it only after the finite-positive X-only gate retires, bracket it with a
known-good draw, and quarantine the engine immediately on timeout.  It passes
a strict host build.

### Finite-positive silicon result

On a fresh VX900 boot with module build `83a0d6`, the exact three-record
`--sqrt-x-positive` program retired and produced RGBA=`.5` within the guarded
checker.  D16 and both guard regions passed; known-good draws passed before
and after, and dmesg gained no timeout.  The target log is
`/root/tmp/chrome9-live/sqrt-lower/sqrt-x-positive.log`; the tested binary has
SHA-256
`75daf5f9703bfd055840c439462d3c5a8480af5a6d9275b59707c0656e6bd229`.

This closes the X-mask RSQ/RCP opcode order, scalar TEMP forward, three-record
controls/resource bucket, and finite-positive numerical path.  It does not
rehabilitate the timed-out combined oracle.

### Positive-zero silicon result

On the same fresh-boot module build `83a0d6`, the visible-write
`--sqrt-x-zero` gate also retired.  Its color payload was prefilled with
`5a91c3e7`; all interior samples changed to exact zero, while D16, the
triangle exterior, and both BO guards remained intact.  Known-good baselines
passed before and after, and dmesg gained no timeout or engine fault.  The
target log is `/root/tmp/chrome9-live/sqrt-lower/sqrt-x-zero.log`; the tested
tool binary SHA-256 begins `008b6da`.

This is silicon proof that an isolated X-mask `RCP(RSQ(+0))` chain is safe and
numerically returns positive zero through the final scalar MOV.  The zero
endpoint therefore was not sufficient to cause the older 12-record timeout.
That failure still contains three independent variables absent from both
passing gates: serial Y/Z/W partial destination writes, a longer mixed-lane
chain, and the SUB/CMP plus split-COLOR tail.  Function-medium's optimized NIR
is scalar, so Y/Z/W partial destinations and the CMP discriminator are not
requirements for this scene and should be deferred.

### Selector-image qualification found by the 31-record startup test

The two silicon results above qualify the arithmetic and retirement behavior,
but not the byte-exact Linux production source image.  Module `83a0d6` still
contained a semantic-compiler packing bug: for EXP2/LOG2/RCP/RSQ it copied the
selected component into native W, then filled native XYZ with selector value
zero (`X`) instead of `DRM_CHROME9_FRAGMENT_SWIZZLE_ZERO` (value 5).  Thus an
input written semantically as `(X,ZERO,ZERO,ZERO)` reached those live draws as
native `XXXX`.  The expected Linux/Windows record below is native `000X`:

```text
old live compiler source A:  word1=001c0053 word2=...0000  # XXXX
production source A:         word1=801c0053 word2=...00b6  # 000X
```

The difference first became observable when
`chrome9_fragment_sqrt_chain_selftest()` compared all native words at module
startup.  It is a real source-selector mismatch, not an equivalent temporary
allocation: source id and destination mask are unchanged, while the three
inactive selector fields differ.  The generic correction applies equally to
EXP2, LOG2, RCP, and RSQ.

Consequently the earlier positive and zero passes remain useful numerical and
wedge-safety evidence for the scalar unit, but they do not qualify the
vendor-canonical `000X` packet.  Likewise, the old 12-record timeout used
`XXXX` and must not be attributed to the corrected `000X` chain.  The first
five-step run after the startup correction is the first complete live gate for
the production selector image; bracket it with known-good draws and stop on a
timeout as already required below.

## Source-shaped five-step scalar gate

The next relevant discriminator repeats the exact scalar body used by the
scene, rather than reconstructing four roots in different vector lanes:

```text
repeat five times:
    d = FRC(3.0 * d)
    d = FRC(RCP(RSQ(d * d)))
MOV color, d.xxxx
```

The Linux 5.75 production compiler was run offline with this exact PS 2.0
sequence and configuration-two finalization.  Using the same alternating
two-register schedule chosen by the kernel lifetime allocator, it emits 31
ALU records, no TAU, active resources `(1,2,1,0)`, A90=`90058001`, and
A92=`9200001f`.
The first iteration is byte-exact:

```text
0140b6d0 d008016d 0012c016 00000020  MUL  T0.x,3
01c00530 d01c0053 00100016 000e0060  FRC
0140b6d0 d000116d 00100116 00000020  MUL  x,x
01c00530 801c0053 001000b6 00070060  RSQ
01c00530 801c0053 001001b6 00060020  RCP
01c00530 d01c0053 00100016 000e0060  FRC
```

The fifth iteration has the same schedule.  Its last RCP/FRC gain the normal
last-use bits and the object terminates with:

```text
01c00530 801c0053 001001b6 00060020
01c00530 d01c0053 00120016 000e0060
01c00530 001c0053 00f20100 000d0820
```

`tools/re/chrome9_normalize_guard_smoke.c` now provides two offline-reviewed
modes with this 31-instruction shape:

- `--sqrt-chain5-positive` starts at exact `.25`.  The recurrence alternates
  `.25,.75`, so the fifth result must be `.75` in all BGRA8 lanes.
- `--sqrt-chain5-zero` starts at exact `+0`.  All five roots exercise the
  qualified zero path, and the final write must replace a nonzero color
  sentinel with exact zero.

Both modes use SSA-distinct semantic temporaries so the kernel's production
lifetime allocator is exercised.  Every masked scalar source is explicitly
`(X,ZERO,ZERO,ZERO)`; only the terminal full-color MOV uses `XXXX`.  Kernel
self-test `chrome9_fragment_sqrt_chain_selftest()` compiles this exact
31-instruction semantic program and compares all 124 native ALU dwords plus
A90--A92 with the production object above.  This catches inactive selector,
physical lifetime, release-bit, control-count, and resource-bucket drift
before engine initialization.

Mesa Chrome9 commit `f836ff3` adds an offline test for the complete five-step
fragment recurrence, including the fragment-coordinate prefix and RGB output
assembly.  With the current production options it still rejects `fsqrt`.
With test-local `lower_fsqrt` and Mesa's standard algebraic pass, all five
roots become RSQ/RCP pairs and the semantic compiler accepts exactly 36 ALU
records (12 MUL, 11 FRC, 5 RSQ, 5 RCP, 3 MOV), with no texture or varying
resource.  The T2-loader test passes; this is compiler evidence only and does
not enable square-root lowering in the production driver.

The raw modes check a 3x3 interior sample grid, D16, untouched exterior
pixels, and both color/depth guards.  A strict C11 `-Wall -Wextra -Werror`
host build and the kernel object build pass.  Neither mode was submitted by
this audit.

### Corrected five-step silicon result

The complete source-shaped recurrence was subsequently run on a fresh VX900
boot with the debug-free kernel commit `5656982` and module SHA-256
`f7e0c4023d813182842b7142e1943c2e01e384003abe6230b0c788e2bf83fcfb`.
This build contains the corrected proprietary `000X` scalar-source packing;
it is not the older `XXXX` stream qualified above.

After a known-good CMP baseline, `--sqrt-x-positive` again produced `.5`.
`--sqrt-chain5-positive` then produced exact `.75`, and
`--sqrt-chain5-zero` replaced the color sentinel with exact zero.  Both
five-step runs passed their D16, interior/exterior, and BO-guard checks.  A
known-good baseline passed after each candidate; no timeout or engine fault
was reported.

This closes the byte-exact 31-record Linux production stream, repeated
positive and zero RSQ/RCP endpoints, lifetime reuse, release bits, A90--A92,
and retirement on this silicon.  It does not by itself execute the complete
36-record scene program: the latter additionally contains fragment-position
arithmetic and output assembly.

The remaining production compiler change is generic rather than
scene-specific.  Set Mesa's standard `.lower_fsqrt = true` in the Chrome9 NIR
compiler options, so the state tracker lowers FSQRT to FRSQ followed by FRCP
before the semantic backend.  Convert the existing function-medium test from
a test-local preview into a production-options assertion: five FSQRT nodes
must become five FRSQ and five FRCP nodes; the resulting shader must remain a
36-record structured cmd22 program with the documented
`MUL/FRC/RSQ/RCP/MOV = 12/11/5/5/3` histogram and no sampler/varying input.
Continue rejecting non-default explicit float-control modes.  Before claiming
the scene, retain the UAPI translation check, capture the full native
POSITION/MISC resource tuple, and run one bounded validation bracketed by
known-good draws.

The raw prefix omits only the already-qualified fragment-coordinate
arithmetic and the output-vector assembly from the 36-ALU scene program.  Its
positive and zero gates have now retired in the required order.  Production
`.lower_fsqrt` may advance through the offline compiler/resource assertions
above; the generated complete shader and scene validation remain separate
bounded gates.

### Exact complete-program preflight

The test binary from Mesa commit `f836ff3` was stopped immediately after the
lowered shader's semantic compile and its complete 36-record IR was inspected.
The fragment-position prefix is:

```text
MUL t32.x, POSITION.x000, C31.y000
MUL t31.x, t32.x000,       POSITION.y000
FRC t30.x, t31.x000
```

It is followed by five copies of the canonical six-record scalar block
`MUL 3,d; FRC; MUL a,a; RSQ; RCP; FRC`.  The output suffix is:

```text
MOV t33.xyz, t0.xxx0
MOV t33.w,   C31.000z
MOV COLOR,   t33.xyzw
```

Every inactive scalar selector is ZERO; none of these sources accidentally
uses the earlier unsafe replicated `XXXX` form.  The only literal register is
C31, with X=`3.0`, Y=`0.0001`, and Z=`1.0`.  There is no sampler, TAU, primary
input, or T-varying input.

Kernel lifetime allocation alternates two physical temporaries, including the
output assembly.  With MISC as the sole active external source, the production
resource calculation is `(active0, physical)=(1,2)`: bucket `0x18000`, MISC
id three, and controls:

```text
A90=900d8008 A91=91000000 A92=92000024
```

`active_texture_count=0`, `varying_count=0`, and `tau_count=0`; consequently
there is no MISC/direct-T collision and no dependence on the still-unqualified
three-varying H6 route.  This complete-program result composes already-live
boundaries: the byte-exact 31-record scalar core passed positive and zero
inputs, and the POSITION-plus-constant path passed a longer eight-ALU shader.
The five extra prefix/output records do not introduce a new opcode or resource
class and remain below the 96-ALU ABI bound.

The production-options regression should additionally assert C31 as the sole
immediate, no user constant/uniform mask, one POSITION source with X000/Y000
padding, and successful 36-record UAPI translation.  Once those checks pass,
the next meaningful live gate is one bounded `function` medium-complexity
glmark2 validation bracketed by known-good draws; another scene-shaped raw
shader probe would not separate a remaining hardware unknown.

### Complete scene result

Mesa subsequently enabled the standard `lower_fsqrt` option and retained the
offline 36-record production-options regression described above.  On the
VX900 target the exact five-step medium-complexity fragment scene completed
with glmark2 `Validation: Success`.  A known-good textured baseline passed
afterward and the kernel log gained no GPU timeout or engine fault.

This closes the complete POSITION prefix, five lowered scalar roots, output
assembly, structured-UAPI translation, and application validator together.
It does not rehabilitate the old 12-record multi-lane partial-write timeout,
qualify negative square roots, or broaden explicit floating-point control
support.  Those remain independent contracts.
