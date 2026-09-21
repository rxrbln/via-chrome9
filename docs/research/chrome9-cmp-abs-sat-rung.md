# Chrome9 one-record CMP, ABS, and destination-SAT boundary

## Compiler result

Windows and Linux production compilation byte-match all eight identity/full
write CMP combinations over v0/c0, both identity ABS sources, and twelve
representative destination-SAT records.  The complete token and native-word
set is in
[`chrome9-cmp-abs-sat-corpus.tsv`](chrome9-cmp-abs-sat-corpus.tsv).

CMP uses the same physical slots as MAD but selects rather than adds:

```text
A = source 0 / condition
C = source 1 / D3D true arm
B = source 2 / D3D false arm
subop = 9
```

For example, `CMP oC0,c0,v0,c0` is exactly:

```text
00800530 30040053 00f08005 00090820
```

The D3D compiler contract is `condition >= 0 ? true : false`.  Source-slot
placement is now byte-exact, but the native subop-9 predicate, its zero
boundary, signed-zero handling, and scalar lane behavior remain silicon
questions.  Compiler acceptance must not be quoted as proof of those
numerical semantics.

ABS expands one logical source into all three family slots:

```text
ABS v0  00400538 30040053 00f04005 00090820
ABS c0  00800538 30080053 00f08005 00090820
```

The native record has A=source, B=-source, C=source and subop 9.  This is an
ABS operation, not an ABS source modifier.  Both records now have direct
Windows/Linux dynamic byte evidence; their numerical result is still pending
silicon.

Destination SAT changes only dword-3 bit 12 in every tested operation.  It
does not change source packing, destination mask, subop, or the control tuple.
The cross-driver set covers MOV, MUL, ADD, MIN, MAX, MAD, DP3, DP4, CMP, RCP,
and both ABS source files.  This closes the encoder bit, not clamp ordering or
the full `[0,1]` numerical contract.

## UAPI boundary

The current command-19 instruction has `src[2]`.  General CMP has three
ordered operands and therefore cannot fit it.  Even with only v0 and c0, two
descriptors do not identify which physical position is duplicated.  These
three distinct compiler objects cannot be selected by one generic two-source
key:

```text
CMP c0,v0,c0
CMP c0,c0,v0
CMP v0,c0,v0
```

A future general successor needs `src[3]`.  A bounded predecessor may instead
assign a separate semantic opcode to one exact topology, such as
`CMP_C0_V0_C0`, so the duplicate position is implicit and kernel-owned.
Guessing the third operand from source equality is rejected.

## Value domains and fail-closed IR shapes

CMP only selects an arm, so finite condition and finite arms are closed under
all rounding directions and need no magnitude bound.  Reject every exponent-
ff c0 lane, source modifier, swizzle, partial mask, SAT, texture, temporary,
control-flow form, and non-default FP mode.  A first Mesa match must be the
exact per-lane tree `bcsel(fge(c0,+0),v0,c0)`, replicated as a full XYZW oC0
write in one terminal instruction.  Do not map a generic conditional-select
whose zero, unordered, or signed-zero rule differs from D3D CMP.

Native ABS maps finite inputs to a finite magnitude and likewise needs only
the exponent-ff rejection.  It should not be the next UAPI rung, however:

- `fabs(v0)` is MOV(v0), because normalized D3DColor v0 is nonnegative;
- terminal `fabs(c0)` can clear each finite job-local c0 word's sign bit on
  the CPU and submit the already-live MOV(c0), deterministically converting
  `-0` to `+0` without a new opcode;
- native ABS becomes useful only when a future multi-record program applies it
  to a temporary or another dynamic source.

The CPU rewrite is valid only for an exact finite constant source.  It is not
permission to fold ABS across NaN policy, source modifiers, arithmetic, or a
dynamic value.

Destination SAT remains compiler-only.  No NIR `fsat`/clamp may select bit 12
until both lower and upper clamps and their ordering against alpha test are
proved.  In particular, do not infer SAT from the fact that the final UNORM
render target also clamps.

## CMP guarded silicon oracle

Use the proven command-19 D16/RHW packet, BGRA8888 target, COPY, no blend,
flat v0, guarded pages, and known-good draws before and after each candidate.
For `CMP_C0_V0_C0`, use logical v0 `(0.25,0.75,0.5,1)` and c0
`(-0.5,+0.25,+0,-0)`.  D3D `>= 0` semantics produce logical
`(0,0.75,0.5,1)`, approximately bytes `(0,191,128,255)`.  One vector
simultaneously distinguishes reversed sense, swapped arms, strict-positive
instead of nonnegative, and mishandling of negative zero.  Run a companion
`CMP_C0_C0_V0` row to validate the opposite arm placement.

Then use fixed A33 as an observation before normalized target conversion:

| condition/arms in alpha | A33 | expected D3D result |
| --- | --- | --- |
| c0=-0.5, true v0=1, false c0=-0.5 | LESS/ref0 `33001000` | pass; negative condition selects the negative false arm |
| c0=-0, true v0=1, false c0=-0 | GREATER/ref0 `33004000` | pass; `-0 >= 0` selects one |
| c0=+0.25, true v0=1, false c0=+0.25 | GREATER/ref0.5 `33004202` | pass; positive condition selects one |

Canary-fill the target so a killed fragment is unambiguous.  Command 19 makes
D16 mandatory: use a freshly cleared depth surface at 1.0, LESS/write, and
passing z=0.5 for each isolated row; check both color and depth guards.  Repeat
each row 1, 64, and 1024 times.  Require the predicted pass plus ordinary
asymmetric-color readback, exact submitted words, ordered fences, unchanged
guards, no reset/error increment, and retirement of the following known-good
draw.  Include deliberately reversed predicates/arms as negative controls
before advertising a semantic key.

## Native ABS oracle and preferred lowering

For native `ABS(c0)`, use finite c0 logical RGBA
`(-0.25,+0.5,-0.75,-0.5)`.  Expected output is
`(+0.25,+0.5,+0.75,+0.5)`, approximately bytes `(64,128,191,128)`; MOV(c0)
would clamp the negative lanes to zero.  With alpha GREATER/ref0
(`33004000`), ABS must pass while unsaturated MOV(c0) must fail.  This proves
that positive magnitude reaches A33 before the target conversion.

Neither normalized readback nor A33 distinguishes ABS(-0)=+0 from preserved
-0.  A downstream sign-sensitive ALU or float target is required for native
ABS signed-zero proof.  The recommended terminal constant lowering avoids
that uncertainty entirely by clearing the sign bits on the CPU and using MOV.

## Destination-SAT lower-clamp oracle and remaining blocker

The terminal UNORM target masks SAT in ordinary readback.  A33 provides
lower- and upper-bound discriminators if it consumes the shader value before
target conversion.  Use a canary target, COPY, and the mandatory fresh D16
clear=1 with LESS/write at z=0.5.

First compare the unsaturated and saturated MOV(c0) records with c0
alpha=-0.5 and alpha LESS/ref0 (`33001000`):

```text
MOV c0      01c00530 301c0053 00f08005 000d0820  -> -0.5 < 0, pass
MOV_SAT c0  01c00530 301c0053 00f08005 000d1820  ->  0.0 < 0, fail
```

First require the unsaturated draw to pass; otherwise the experiment has not
proved that A33 sees the raw negative ALU result.  Then require the SAT draw
to leave the sentinel untouched, followed by a known-good passing draw.  This
proves the lower clamp and bit-12 effect without relying on UNORM readback.

Use the symmetric upper-bound pair with c0 alpha=+2.0 and alpha
GREATER/ref255.  Reference 255 has the special encoded value `0x400`, so the
candidate state is `A33=33004400`:

```text
MOV c0      raw 2.0 > 1.0, pass
MOV_SAT c0      1.0 > 1.0, fail
```

Again, require the unsaturated draw to pass before interpreting the saturated
failure.  If either unsaturated control fails, hardware has preclamped the
A33 input or the recovered ordering is wrong; that result is inconclusive,
not evidence for SAT.  Full destination SAT remains rejected until both
negative/lower and positive/upper pairs pass with fresh depth, unchanged
guards, clean error/reset counters, and succeeding known-good retirement.
