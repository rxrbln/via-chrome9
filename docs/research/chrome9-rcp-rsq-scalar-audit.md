# Chrome9 RCP/RSQ scalar and finalizer audit

This note closes the compiler-side scalar selector boundary for terminal
RCP/RSQ.  It does not promote either operation in command 19.  Numerical
behavior, scalar broadcast, and precision remain silicon gates.  The exact
28-case differential is retained in
[`chrome9-rcp-rsq-scalar-corpus.tsv`](chrome9-rcp-rsq-scalar-corpus.tsv).

## Emitters and absence of value validation

The Windows emitters are VA `0x10219920` (RCP) and `0x10219a80` (RSQ) in
`VTGUModeDX32A.dll` SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`.
The symbolized Linux twins are `stmPPS_RCP_PS20_invi` at `0x0016abe0` and
`stmPPS_RSQ_PS20_invi` at `0x0016aa60` in `via_chrome9_dri.so` SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.

The two emitters in each binary are structurally identical except for native
subop 6 versus 7.  They copy the 36-byte intermediate instruction, check the
referenced constant register, obtain ordinary destination/source settings,
select the source token's W lane, update temporary-write tracking, and append
one ALU record.  Their instructions contain no floating compare, exponent
inspection, constant payload access, or magnitude test.  Consequently every
zero, sign, denormal, infinity, NaN, and range rule below is a new fail-closed
driver policy, not a recovered vendor limit.

Windows calls its destination/source helpers at `0x1020f200` and
`0x1020f070`, then its writemask/swizzle helper at `0x1020e600` with mask 8.
Linux extracts the corresponding packed W selector after
`stmPPS_GetSrcSetting_invi`.  This explains the frontend rule: a scalar D3D
operation reads the component named by the W selector of its source token.
An unswizzled `xyzw` token therefore selects W.

## Exact direct selector matrix

Explicit c0 `.xxxx`, `.yyyy`, `.zzzz`, and `.wwww` terminal shaders compile
byte for byte through Windows scheduling and Linux configuration 2:

| operation | X | Y | Z | W |
| --- | --- | --- | --- | --- |
| RCP | `01c00530 801c0053 00f080b6 00060820` | `01c00530 901c0053 00f080b6 00060820` | `01c00530 a01c0053 00f080b6 00060820` | `01c00530 b01c0053 00f080b6 00060820` |
| RSQ | `01c00530 801c0053 00f080b6 00070820` | `01c00530 901c0053 00f080b6 00070820` | `01c00530 a01c0053 00f080b6 00070820` | `01c00530 b01c0053 00f080b6 00070820` |

In native A, XYZ selectors are forced to literal ZERO and A.w is respectively
X/Y/Z/W.  The destination remains a full XYZW `oC0` write.  Both frontends
also byte-match NEG in the A modifier nibble: X changes `80` to `88`, and W
changes `b0` to `b8` in dword 1.  That is packing evidence only.  NEG stays
outside the first key.

The full destination mask and scalar source record do not prove that silicon
broadcasts the numerical scalar result to all four output lanes.  The first
live test must check all BGRA bytes independently; it must not infer broadcast
from compiler output.

## Temporary result and finalizer behavior

RCP/RSQ as a producer followed by MOV has the same finalizer delta for all
four selectors.  The scalar producer is unchanged.  The raw MOV temporary A
source

```text
001c0053 00f00000
```

becomes the ordinary configuration-2 dependency form

```text
301c0053 00f20005
```

RCP/RSQ as a consumer of a MOV temporary gives the complementary Linux
result.  The production compiler places the scalar temporary in native W;
configuration 2 preserves dword 1 `b01c0053` and changes only dword 2
`00f000b6 -> 00f200b6`.  Thus the added field is the ordinary A-source
dependency bit, not an RCP/RSQ-specific control.  Producers and consumers use
normal A92 counts, `90058000` for the Linux ALU-only executable control, no
TAU, and no additional state word.

The headless Windows wrapper instead leaves a temporary scalar consumer on
native X (`801c0053`) because its synthetic identity register callbacks do
not reproduce the production Linux component allocator.  This known wrapper
limit must not override the symbolized Linux W placement.  Windows VA
`0x10203530` is a scheduler/finalizer for the synthetic object, whereas Linux
`stmPPS_PostTranslationSetting_invi` at `0x0019aba0` applies the target-2
dependency pass.  Only direct c0 terminal records are cross-driver byte
contracts here; every two-record row remains offline evidence.

## Mesa semantic boundary

Mesa NIR defines `frcp(x)` as FP32 `1.0f/x` and `frsq(x)` as
`1.0f/sqrtf(x)`.  The current TGSI-to-NIR path reads scalar source X after
TGSI swizzling and maps it directly to those NIR operations.  The older Mesa
program path explicitly applies `fabs` before `frsq`, and Gallivm still
documents the historical TGSI RSQ absolute-value contract even though its
low-level helper computes without ABS.  The supplied D3D compiler emits no
separate ABS record, so its negative-input contract cannot be distinguished
from native subop behavior by static code.

This frontend discrepancy is irrelevant for a positive-only key and is a
hard reason to reject negative RSQ.  The initial NIR classifier must recognize
only a replicated scalar expression:

```text
vec4(frcp(channel(c0, 3)))
vec4(frsq(channel(c0, 3)))
```

stored as a terminal full-XYZW `oC0` result.  Reject vector/componentwise
forms, a different channel, source modifiers, SAT, partial writes, temporary
users, explicit non-default FP modes, and every algebraic rewrite into or out
of reciprocal operations.

## Fail-closed value domains

The compiler accepts all bit patterns, but the first structured candidates
should be deliberately narrower:

| candidate | required selected c0.w | reason |
| --- | --- | --- |
| RCP | finite positive FP32 in `[1,16]` | result remains in `[1/16,1]`; no zero, overflow, denormal input/output, sign, or UNORM upper clamp |
| RSQ | finite positive FP32 in `[1,256]` | result remains in `[1/16,1]`; same exceptional-value closure |

Keep the existing all-lanes-finite c0 validation even though the native
record selects only W.  These bounds are live-oracle/admission bounds, not
hardware limits.  A later proof may broaden RCP to positive normals
`0x00800000..0x7e800000`, which keeps its reciprocal normal and finite, and
RSQ to all positive normals.  Do not broaden before testing rounding mode and
flush behavior.

RCP/RSQ of zero can produce infinity; RCP of a small subnormal can overflow;
either operation may flush denormal inputs; and RSQ of a negative value has
the frontend semantic conflict above.  NaN and infinity remain rejected.
Negative RCP is also excluded initially because a terminal UNORM target maps
the result to zero and therefore cannot demonstrate reciprocal magnitude.

## First guarded silicon campaign

Every draw uses command 19's mandatory fresh D16 target, clear depth to 1,
LESS/write with z=0.5, COPY, full color mask, exact command-word checks,
target/depth guards, ordered fences, reset/error counters, and known-good
draws before and after.

First submit each direct selector record, not only W:

```text
RCP c0=(2,4,8,16)       -> selected result=(.5,.25,.125,.0625)
RSQ c0=(4,16,64,256)    -> selected result=(.5,.25,.125,.0625)
```

For each shader the selected scalar must appear in all four output lanes.
The four inputs stay below the UNORM upper clamp after the operation and give
distinct results, proving X/Y/Z/W selection and broadcast rather than merely
successful retirement.  Accept at most one UNORM byte of error around the
nearest conversion result: approximately 128, 64, 32, and 16.  This is a
coarse first-pixel tolerance, not a claim of FP32 ULP accuracy.

Then run W-only exact controls: RCP(1)=1, RCP(2)=.5, RCP(4)=.25 and
RSQ(1)=1, RSQ(4)=.5, RSQ(16)=.25.  Follow with non-power values 3, 5, 7, and
10 and record raw BGRA plus paired A33 threshold outcomes before selecting a
precision policy.  Zero, signed zero, negative, denormal, infinity, and NaN
are fault-domain tests only after the bounded positive campaign; they cannot
gate initial admission.

RCP `.wwww` is the safer successor to FRC: its positive-domain semantics
agree across NIR and the vendor frontend, and powers of two give exact
discriminators.  RSQ should follow only after RCP selector/broadcast succeeds
and its own approximation sweep passes.  Until those live gates complete,
both operations remain rejected by command 19.
