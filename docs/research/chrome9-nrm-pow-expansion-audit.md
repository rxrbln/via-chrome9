# Chrome9 terminal NRM and POW expansion audit

This bounded audit reconciles the legacy r0-target examples with fresh
terminal oC0 programs and production Linux configuration 2.  POW is exactly
three ALU records and NRM is exactly four.  Both Windows raw schedules and
Linux pre-configuration schedules byte-match; production dependency
finalization changes one field in the terminal consumer.  Exact streams are
in [`chrome9-nrm-pow-expansion-corpus.tsv`](chrome9-nrm-pow-expansion-corpus.tsv).

The differential JSON SHA256 is
`9eefd30649c7a2e8d0e47d9828432fb0b126a54321268d2ee8fe08b48db863bb`.
This is compiler evidence, not permission to submit multi-record programs.

## POW expansion

For `POW oC0,c0,c1`, both compilers construct:

1. scalar LOG of c0 through native A=`000W` into a temporary;
2. a subop-0 multiply-like record combining that temporary and scalar c1.w;
3. scalar EXP of the temporary into full oC0.

The Windows raw/final stream is:

```text
01c00530 b01c0053 00f080b6 00050020
0140b6d0 30081b6b 00f00005 00000020
01c00530 301c0053 00f00005 00040820
```

Linux configuration 2 changes only the final EXP source word from
`00f00005` to `00f20005`, the generic source-A dependency bit.  It uses
`A90=90058000`, `A92=92000003`; the synthetic Windows wrapper reports
`A90=90040000`, `A92=92000003`.  Use the Linux tuple for production execution.

Neither frontend validates constant payload values.  POW therefore retains
LOG's positive-domain requirement and adds unknown compound precision,
rounding, temporary allocation, and scalar broadcast behavior.  A conservative
future live domain is positive c0.w in `[1/4,1]` and c1.w in `[1/2,2]`, which
keeps the mathematical output in `[1/16,1]`.  Exact-power controls include
`pow(.5,2)=.25` and `pow(.25,.5)=.5`; follow those with non-power values and
A33 brackets.  Zero, negative base, NaN/Inf, source modifiers/swizzles, SAT,
scale, and partial destinations remain rejected.

## NRM expansion

For `NRM oC0,c0`, both compilers construct four records with subops
DP3(2), ABS/CMP-family(9), RSQ(7), and multiply-family(0):

```text
01c00530 30080053 00f08005 00020020
00000530 38140924 00f00005 00090020
01c00530 001c0053 00f00000 00070020
0140b6d0 30000053 00f08005 00000820
```

Linux configuration 2 changes only the final record's source-C dependency,
`30000053` to `30200053`.  It uses `A90=90058000`, `A92=92000004`; the
Windows wrapper reports `A90=90040000`, `A92=92000004`.  The second record's
exact numerical clamp/absolute behavior is not inferred merely from shared
subop 9, so the stream must be treated as an indivisible vendor lowering.

The emitter validates token/register form, not vector magnitude.  Zero-length,
denormal, exceptional, and extreme vectors therefore remain hazardous.  A
future guarded oracle should begin with finite exact unit vectors and the
3-4-5 vector c0.xyz=`(.6,.8,0)`, require full-vector output and guards, then
test a bounded non-unit vector.  Alpha/w behavior must be observed rather than
assumed.  Do not synthesize NRM from independent DP3/RSQ/MUL keys until the
intermediate subop-9 semantics and all dependency fields pass silicon.

## Fail-closed conclusion

The current runtime admits one ALU record only.  POW and NRM must therefore be
rejected before native encoding, even though their terminal compiler streams
are now exact.  POW's two logical sources do not make its three physical
records fit the one-record contract; NRM likewise requires temporary state
and four records.  No result-scale or SAT bit can collapse either expansion.

Any later multi-record promotion requires a dedicated UAPI key, fixed record
count and bytes, production Linux controls/dependencies, bounds above, and a
guarded live sequence using command 19's mandatory fresh D16 target, depth
clear 1, LESS/write at z=0.5, COPY, target/depth guards, fences, reset/error
counters, and known-good draws before and after.
