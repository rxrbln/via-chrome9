# Chrome9 EXP/LOG scalar and frontend audit

This note records compiler evidence for terminal base-2 exponential and
logarithm operations.  Neither operation is admitted by command 19.  Exact
selector, dependency, and shader-version rows are retained in
[`chrome9-exp-log-scalar-corpus.tsv`](chrome9-exp-log-scalar-corpus.tsv).

## Static emitter boundary

The Windows emitters are VA `0x10216740` (EXP) and `0x10216f00` (LOG).  The
symbolized Linux twins are `stmPPS_EXP_PS20_invi` at `0x00171390` and
`stmPPS_LOG_PS20_invi` at `0x00171040`.  As with RCP/RSQ, the pairs have the
same one-record structure and differ in native subop 4 versus 5.  They check
only register/token structure, select the source token's W lane, update
temporary tracking, and append the ALU record.

No emitter inspects the constant payload, exponent bits, sign, NaN, infinity,
or magnitude.  The bounded value rules below are therefore new driver policy,
not vendor compiler or hardware limits.

## Direct selector records

Explicit c0 `.xxxx`, `.yyyy`, `.zzzz`, and `.wwww` terminal shaders match
byte for byte between the Windows scheduled output and Linux configuration 2:

| operation | X | Y | Z | W |
| --- | --- | --- | --- | --- |
| EXP | `01c00530 801c0053 00f080b6 00040820` | `01c00530 901c0053 00f080b6 00040820` | `01c00530 a01c0053 00f080b6 00040820` | `01c00530 b01c0053 00f080b6 00040820` |
| LOG | `01c00530 801c0053 00f080b6 00050820` | `01c00530 901c0053 00f080b6 00050820` | `01c00530 a01c0053 00f080b6 00050820` | `01c00530 b01c0053 00f080b6 00050820` |

Native A.xyz are literal ZERO and A.w selects the requested logical component.
Both compilers also agree on NEG in the A modifier nibble (`80 -> 88` for X,
`b0 -> b8` for W).  NEG remains reject-only.  A full XYZW destination mask is
still compiler evidence rather than proof that silicon broadcasts the scalar
result to every output lane.

## Dependencies and shader versions

EXP/LOG followed by MOV leaves the scalar producer unchanged and applies the
ordinary Linux configuration-2 dependency rewrite to the MOV consumer:

```text
001c0053 00f00000 -> 301c0053 00f20005
```

When EXP/LOG consumes a MOV temporary, production Linux keeps the scalar in
native W and changes only source-A dependency d2
`00f000b6 -> 00f200b6`.  The Windows headless wrapper instead leaves this
temporary source on X because its synthetic identity allocator does not model
production component allocation.  This is the same known wrapper boundary as
RCP/RSQ, not an EXP/LOG-specific finalizer.  There is no TAU, special control,
or extra record.

A separate ten-case frontend differential feeds the same identity source to
pixel-shader versions 1.1, 1.2, 1.3, 1.4, and 2.0.  Every accepted EXP and LOG
token produces exactly one identical raw record; neither driver expands an
older version into a polynomial or multi-record sequence.  Linux target-2
finalization adds d3.bit29 to the standalone r0 destination in every version,
while the Windows synthetic finalizer does not.  A direct Linux raw/final
probe confirms `00040020 -> 20040020` for EXP and the analogous LOG delta.
That is destination-id finalization for a terminal r0 object, not a numerical
opcode difference.  Since r0 is not a valid ps_2_0 color-output shape, these
version rows remain offline evidence; the bounded runtime always uses explicit
ps_2_0 oC0.

## Mesa semantics and fail-closed shapes

Mesa NIR defines `fexp2(x)` as `exp2f(x)` and `flog2(x)` as `log2f(x)`.
Current TGSI-to-NIR maps scalar EX2/LG2 source X after TGSI swizzling directly
to those operations, and the classic Mesa program path does the same.  The
vendor D3D record does not reveal exceptional or negative LOG behavior, so the
shared safe semantic intersection is a positive LOG input.

The initial classifier, if ever promoted, must recognize only these replicated
scalar terminal forms:

```text
vec4(fexp2(channel(c0, 3)))
vec4(flog2(channel(c0, 3)))
```

Require a full-XYZW oC0 write, identity `.wwww` source, no source modifier,
no SAT, no temporary consumer, one record, and default FP32 modes.  Reject
arbitrary componentwise operations, algebraic `pow` replacements, source
swizzles other than the fixed W selector, explicit fast-math/denormal modes,
and every exceptional input.

For an observable first terminal gate, enforce all c0 lanes finite and the
selected lane in these intervals:

| operation | initial c0.w | exact output interval |
| --- | --- | --- |
| EXP | `[-4,0]` | `[1/16,1]`, positive and finite |
| LOG | `[1,2]` | `[0,1]`, finite |

Positive EXP inputs are mathematically safe but produce values above one,
which a UNORM target clamps and cannot numerically discriminate.  Using a
NEG source modifier to recover a visible result would couple two unproven
features, so the first EXP key instead uses an identity source with a negative
job-local constant.  A later range proof may broaden EXP to `[-126,127]`,
which keeps `exp2` normal and finite, and LOG to all positive normal FP32
inputs.  Those broader ranges are not initial admission policy.

For completeness, positive finite EXP input `[0,127]` is closed to normal
finite output `[1,2^127]` without depending on a rounding-mode overflow edge.
It is not a useful terminal numerical domain: every strictly positive result
is above one after exponentiation.  A33 GREATER/ref255 (`33004400`) can prove
that EXP(1) reaches alpha test above one while EXP(0)=1 does not, but cannot
measure the magnitude or approximation accuracy.  Keep this as an upper-range
control, not an admission gate.

LOG zero, negative, denormal, infinity, and NaN remain rejected.  EXP
infinity, NaN, underflow/denormal-output ranges, and overflow ranges remain
rejected.  The static compiler accepts them but gives no behavioral promise.

## Guarded live oracles

Use command 19's mandatory fresh D16 target, clear depth 1, LESS/write at
z=0.5, COPY, full color mask, exact word checks, target/depth guards, ordered
fences, reset/error counters, and known-good draws before and after.

First test all four direct selector records and require the selected scalar in
all BGRA lanes:

```text
EXP c0=(-4,-3,-2,-1) -> (1/16,1/8,1/4,1/2)
LOG c0=(0x3f800000,0x3f9837f0,0x3fb504f3,0x3fd744fd)
                         -> approximately (0,.25,.5,.75)
```

The LOG inputs are the nearest FP32 values to 1, 2^(1/4), 2^(1/2), and
2^(3/4).  Accept at most one byte around nearest UNORM conversion
(EXP approximately 16, 32, 64, 128; LOG approximately 0, 64, 128, 191) for
this first coarse selector/broadcast gate.  Follow with exact W controls
EXP(-1)=.5, EXP(-2)=.25, LOG(1)=0, and LOG(2)=1.  This is not an FP32
precision guarantee.

Then use paired pre-RT alpha thresholds to bound nontrivial results:

| operation | value | must pass | must fail |
| --- | --- | --- | --- |
| EXP | `exp2(-0.5) ~= .7071068` | GREATER/ref180 `A33=330042d2` | GREATER/ref181 `A33=330042d6` |
| LOG | `log2(1.5) ~= .5849625` | GREATER/ref149 `A33=33004256` | GREATER/ref150 `A33=3300425a` |

Run known alpha-test controls first; a failed positive control makes the pair
inconclusive.  Sweep additional non-power values and retain maximum observed
error before selecting a Mesa precision policy.  If selector, broadcast,
monotonicity, threshold, or repeatability differs from these bounds, fail
closed.  Even a passing coarse pixel campaign does not justify an FP32-ULP
claim or arbitrary NIR lowering.

EXP/LOG remain behind FRC, RCP, and RSQ in promotion order because their
approximation accuracy is unknown and terminal UNORM masks much of their
natural range.
