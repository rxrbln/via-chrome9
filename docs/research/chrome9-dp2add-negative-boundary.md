# Chrome9 pixel DP2ADD negative boundary

An isolated terminal PS 2.0 DP2ADD is not portable across the recovered
production compilers.  The Windows UMD returns success but emits zero ALU and
zero TAU records and leaves phase and color-output count zero.  Its production
updater still emits the default A90 resource-bucket field; this is not an
executable program.  Linux 5.75 emits a two-record arithmetic expansion.  This
is a strict rejection boundary, not a one-record opcode candidate.

## Dynamic result

Two terminal forms were compiled through the initialized production paths:

- `DP2ADD oC0,c0,c1,c2`
- `DP2ADD oC0,v0,v1,c0`

For both, Windows returns success with `ALU=0`, `TAU=0`, phase 0,
color-output count 0, A90=`90018000`, and A92=`92000000`.  The lack of a
compiler error is therefore not evidence of an executable shader.

Linux configuration 2 lowers the constant form to:

```
00820530 00081000 00f08000 00000020
02000530 90081249 00f08024 00000820
```

and the normalized-input form to:

```
00800530 00041000 00f04000 00000020
02000530 90041249 00f04024 00000820
```

Both have A90=`90058000`, A92=`92000002`.  Configuration 2 marks the second
record's temporary B source with dword-0 bit `02000000`.  The first record
adds the third logical source to the X product; the second adds the Y product
through that temporary.  Exact source swizzles matter to DP2ADD's scalar
semantics, so the identity-swizzle test rows are retained as compiler evidence
and are not promoted into a general NIR semantic contract.

The corrected production-register-manager LRP/DP2ADD differential JSON
SHA-256 is
`6f5d1a727b5ed2ef78af0190114dd1fe2a8d71b8af1fe85e6451869ed77428c6`.
The earlier JSON `3a548756...` omitted the Windows register-manager builder;
its zero-record result was accurate, but its zero A90 payload was a harness
artifact.
Exact tokens and records are in
[`chrome9-dp2add-negative-boundary.tsv`](chrome9-dp2add-negative-boundary.tsv).

## Why Windows emits nothing

The Windows dispatcher table routes public opcode `0x5a` through
`0x1021b710`, while the internal emitter at `0x10215d60` constructs and
appends two arithmetic records at `0x10215f1e` and `0x10216019`.  Static
analysis shows this route is an adjacent-instruction pattern combiner with
strict masks/swizzles rather than an isolated-instruction lowering.  The
recovered standalone production wrapper therefore reaches the silent
no-output behavior reproducibly.  It must not be treated as a callable
general DP2ADD compiler ABI.

## Runtime policy

Reject every DP2ADD key in the current validator.  It requires three logical
sources, while the current structured UAPI has two, and the only complete
dynamic lowering needs two records, a temporary, ordering, and dependency
finalization.  Never accept frontend success without also requiring a
nonzero, expected record count and output metadata.

Mesa can eventually lower the NIR operation explicitly after scalar swizzle,
rounding, and constant-domain behavior are specified.  A future silicon test
must submit only the verified Linux-style expansion and use command 19's
mandatory fresh D16, depth clear 1, LESS/write, passing z=0.5, guarded
destination, and readback.  No live DP2ADD capability follows from this
compiler-only evidence.
