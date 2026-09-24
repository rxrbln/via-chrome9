# Chrome9 glmark2 function-medium fragment gate

Status: source-exact and Mesa-26.2.2 offline-NIR audit plus a failed guarded
silicon gate, 2026-09-24.  The combined square-root oracle timed out and
quarantined the engine.  Do not enable the lowering generally.

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
