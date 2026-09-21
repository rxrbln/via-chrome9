# Chrome9 scalar result-scale and compare-lowering audit

This audit closes compiler packing for destination result scale and SAT on
terminal RCP, RSQ, EXP, and LOG with source c0.w.  All 56 Windows/Linux
dynamic cases succeeded and are compacted into 28 scale rows in
[`chrome9-scalar-result-scale-corpus.tsv`](chrome9-scalar-result-scale-corpus.tsv).
It also confirms statically that SGE and SLT are not one-record successors.
None of these findings relaxes the current no-scale/no-SAT scalar validator.

## Exact destination packing

Every Windows record byte-matches the Linux production compiler after
configuration 2.  The source and destination placement never changes:

```text
d0 = 01c00530
d1 = b01c0053
d2 = 00f080b6
d3[19:16] = EXP 4, LOG 5, RCP 6, RSQ 7
d3[15:13] = result-scale code
d3[12] = SAT
```

The D3D destination shift and native scale mappings are:

| D3D shift | native d3[15:13] | operation |
| --- | ---: | --- |
| 0 | 0 | identity |
| 1 / 2 / 3 | 1 / 2 / 3 | multiply by 2 / 4 / 8 |
| 15 / 14 / 13 | 5 / 6 / 7 | divide by 2 / 4 / 8 |

SAT independently adds d3 bit 12 for every operation and scale code.  For
example terminal RCP(c0.w) has d3 `00060820`; SAT is `00061820`, x2 is
`00062820`, SAT+x2 is `00063820`, /2 is `0006a820`, and SAT+/8 is
`0006f820`.  Neither compiler adds a scale-specific dependency or control
word.  The differential JSON SHA256 is
`849108628da3d06b5329511437664b4aed0e6e04d3ac822fc808a412e4fbdbf4`.

This is byte-exact compiler evidence only.  It does not prove whether scale
is applied before or after SAT, whether intermediate rounding is exposed, or
whether all enabled lanes receive a scalar result on silicon.

## Value closure and admission policy

Under the existing narrow input domains, every scale is finite: RCP and RSQ
produce `[1/16,1]`, EXP produces `[1/16,1]`, and LOG produces `[0,1]` before
scaling.  The divide scales remain in the terminal UNORM-visible interval.
The multiply scales can exceed one, but still cannot overflow.  This finite
closure is not enough for runtime admission: result scale is unnecessary for
the first scalar key, its rounding/ordering is not proven, and SAT plus scale
couples two unproven destination features.

The fail-closed kernel/Mesa contract therefore continues to require native
scale code zero and SAT clear, despite exact compiler closure.  Reject every
nonzero d3[15:13], d3 bit 12, D3D/NIR destination scale, or saturating result.
Do not lower an ordinary NIR multiply into the scale field until silicon has
shown the operation and rounding order.  Combined SAT+scale remains rejected
even if SAT and a scale later pass independently.

## Guarded silicon sequence

Every draw uses command 19's mandatory fresh D16 target, depth clear 1,
LESS/write at z=0.5, COPY blending, exact command-word checks, target/depth
guards, ordered fences, reset/error counters, and known-good draws before and
after.

First establish the unscaled scalar/broadcast result.  Then use exact powers
which remain below or equal to one after scale: RCP(16), RSQ(256), and
EXP(-4) each produce 1/16, so x2/x4/x8 must produce 1/8, 1/4, and 1/2;
LOG(2^.125) produces 1/8 and has the same scaled targets.  Divide-scale tests
start from an exact result of 1 and expect 1/2, 1/4, and 1/8.  Require all four
output lanes and repeated draws to agree.  These checks establish coarse
UNORM-visible scale behavior, not an FP32 ULP contract.

Test SAT separately before any combined case.  The lower discriminator uses
a finite negative alpha, A33 LESS/ref0, and requires the unsaturated control
to pass while SAT clamps to zero and fails.  The upper discriminator uses
alpha +2, A33 GREATER/ref255 (`33004400`), and requires unsaturated pass then
SAT fail.  If alpha test observes an already-clamped value, the upper control
does not pass and the experiment is inconclusive.  Only after both SAT edges
and every scale pass may combined SAT+scale ordering be tested with inputs
whose pre- and post-scale clamp orders differ.

## SGE and SLT are multi-record or unavailable

Linux `stmPPS_SGE_PS20_invi` at `0x0016ad60` and Windows SGE at `0x10219700`
both allocate a temporary and append two ALU records.  The older r0-target
corpus records `00810538 30080924 00f08005 00000060` followed by
`0140b6d0 30140924 00f00105 00090020`.  This is not the terminal execution
form: a fresh oC0-target Windows probe instead emits
`00810538 30080924 00f08005 00000020` followed by
`0140b6d0 30140924 00f00005 00090820`, while Linux configuration 2 changes
the second record's source word to `00f20005`.  These contexts agree on the
two-record lowering and subops 0 then 9, not on a single universal executable
byte stream.

Linux `stmPPS_SLT_ICD_PS20_invi` at `0x001644e0` and the Windows internal
routine at `0x10216550` have the same allocate/append-two/finish structure.
The tested Windows ps_2_0 tokenizer has no callable direct SLT path and
crashes on that token.  Consequently SGE is a proven two-record lowering and
SLT is static-only with a failed frontend path.  Both remain rejected as
one-record keys; result-scale bits do not change that conclusion.
