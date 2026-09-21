# Chrome9 FRC emitter, finalizer, and Mesa semantic audit

## Independent binary audit

The Windows FRC emitter is VA `0x102168a0`; the symbolized Linux 5.75 twin is
`stmPPS_FRC_PS20_invi` at `0x001711c0`.  Both copy the 36-byte intermediate
instruction, run the ordinary constant-use, source-swizzle, destination, and
source helpers, pack one A-source ALU record with subop e, update temporary
write tracking, and append it to the ordinary ALU list.

Neither emitter validates a floating-point value or magnitude.  Their bodies
contain integer masks, shifts, register/count bounds, and helper calls but no
floating compare, exponent test, or access to the job's c0 upload values.
Linux calls `stmCheckConstPS20_invi`, but that tracks/validates the referenced
constant register; it does not inspect the later constant payload.  Windows
has the corresponding call at `0x1020f6e0`.  Therefore the `[-2,2]` c0 limit
in the bounded proposal is purely a kernel policy for initial numerical
coverage, not a vendor compiler or hardware limit.

Swizzle and NEG are consumed by the generic source path.  Linux first calls
`stmModifySourceSwizzle_invi`, obtains destination and source settings, then
copies the selected A lanes and modifier nibble into the record.  Windows
does the same through `0x1020e4e0`, source conversion at `0x1020f070`, and
writemask processing at `0x1020e600`.  There is no FRC-specific source range,
sign, or swizzle normalization.  This independently agrees with the dynamic
identity/`.aaaa`/NEG corpus, but still supplies no numerical guarantee.

## Dependency and finalizer result

Three two-record production probes are retained in
[`chrome9-frc-dependency-corpus.tsv`](chrome9-frc-dependency-corpus.tsv):

```text
FRC r0,c0; FRC oC0,r0
FRC r0,c0; MOV oC0,r0
MOV r0,c0; FRC oC0,r0
```

In every Linux configuration-2 result, the producer is unchanged and the
consumer's raw temporary A source

```text
d1/d2 001c0053 00f00000
```

becomes the ordinary finalized dependency form

```text
d1/d2 301c0053 00f20005
```

This is identical whether FRC is the producer, consumer, or both.  A92 is
exactly the ordinary ALU count `92000002`, A90 is the normal Linux executable
`90058000`, and there is no TAU or additional control word.  FRC has no
observed special scheduler/finalizer rule.  The Windows object remains in its
ordinary pretarget form (`A90=90040000`, A92 count two), as expected for this
wrapper.

This result explains dependency participation; it does not authorize a
multi-record runtime.  Command 19 must retain its terminal one-record limit
until the general producer graph and allocation path are separately admitted.

## NIR and TGSI contract

Mesa 26.2 defines `nir_op_ffract` in `nir_opcodes.py` as
`src0 - floorf(src0)` for FP32.  `tgsi_to_nir.c` maps `TGSI_OPCODE_FRC`
directly to that NIR opcode.  The TGSI interpreter's `micro_frc` and
Gallivm's FRC action independently implement the same subtract-floor
relation.  The D3D FRC token and Mesa frontends therefore agree on floor,
not truncate, semantics.

For finite mathematical input the exact result is in `[0,1)`, but an FP32
implementation may round a small negative fraction up to exactly 1.0.  Mesa's
NIR range analysis explicitly includes positive one for negative inputs in
`(-1,0)`.  The safe finite-output statement is therefore `[0,1]`, not the
strict mathematical interval.  Infinity/NaN remain rejected: NIR defines
their FRC result as NaN, while Chrome9 exceptional handling is unmeasured.

NIR also classifies fractional integral input, including either zero sign, as
positive zero.  A terminal normalized target and fixed alpha comparison
cannot distinguish +0 from -0.  The first terminal key may observe identical
API output, but native sign-zero behavior must not be generalized to a later
temporary consumer without a sign-sensitive downstream oracle.

## Exact silicon vectors

Use command-19's mandatory fresh guarded D16 clear=1, LESS/write at z=0.5,
COPY, full color mask, and known-good draws before and after.  Check target and
depth guards, exact words, ordered fences, error/reset counters, and following
known-good retirement.

The first vector should combine exact binary values:

```text
c0 = (+0.25, +1.0, -0.25, -1.0)
FRC expected = (+0.25, +0.0, +0.75, +0.0)
```

It proves pass-through below one, the positive and negative integer boundary,
and floor rather than truncate for a negative fraction without any ambiguous
rounding.  The next vector should be
`(+0.5,+0.75,-0.5,-0.75) -> (+0.5,+0.75,+0.5,+0.25)`.

Then test the FP32 mantissa boundary using exact bit patterns:

| input bits/value | expected FRC | purpose |
| --- | --- | --- |
| `0x4affffff` / 8388607.5 | 0.5 | largest positive half before 2^23 |
| `0x4b000000` / 8388608 | 0.0 | all FP32 values at this binade are integral |
| `0xcaffffff` / -8388607.5 | 0.5 | negative floor rule at the precision edge |
| `0xcb000000` / -8388608 | 0.0 | negative integral edge |

For an in-range precision discriminator, pair `0x3f800001` (1+2^-23) with
1.0.  Ordinary UNORM readback maps both tiny/zero fractions to zero, so use
A33 GREATER/ref0: the first must pass and exact one must fail.  Require the
positive control to pass before interpreting the zero case.

Denormal behavior is a later, separately advertised gate.  Positive minimum
subnormal should produce itself and requires GREATER/ref0 to observe; negative
minimum subnormal can round `x-floor(x)` to 1.0 and should reach a white UNORM
endpoint.  A zero/white result can also expose input flush-to-zero, so bracket
it with exact normal controls and record the active FP mode.  Do not use these
vectors to admit explicit NIR denorm-preserve modes until silicon behavior is
repeatable.
