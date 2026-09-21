# Chrome9 one-record MAD and dot-product compiler tranche

## Result

The proprietary Windows scheduler and production Linux compiler byte-match
all 16 identity/full-write one-record combinations of MAD, DP3, and DP4 whose
logical sources are only v0 and c0.  This closes source placement, not runtime
semantics.  The complete records and D3D tokens are retained in
[`chrome9-mad-dot-rung-corpus.tsv`](chrome9-mad-dot-rung-corpus.tsv).

The smallest useful fail-closed key in this tranche is exactly
`DP3 oC0,v0,v0`:

```text
ALU       01c00530 50040055 00f04005 00020820
A90-A9b   90058000 91000000 92000001 93000000
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000 9b000000
```

It needs no constant, cannot overflow for D3DColor-normalized v0, fits the
existing two-source descriptor, and adds a useful RGB dot operation.  This is
the ranking within the new MAD/dot tranche; it does not bypass the earlier
MIN, MAX, ADD, SUB validation ladder.  DP4(v0,v0) ranks second.  MAD remains
later because its three source positions do not fit the current general UAPI
and because the native operation's fused-versus-unfused rounding has not been
measured.

## Exact source-slot mapping

The native ALU evaluates the family expression `B + A*C`.  MAD maps D3D
sources without reordering:

| D3D source | native slot | packed location |
| --- | --- | --- |
| source 0 | A | dword 1/2 split source |
| source 1 | C | dword 1 low source |
| source 2 | B | dword 0 source |

Consequently `MAD v0,c0,c0` produces
`00800530 30080053 00f04005 00000820`, while moving v0 among the three
positions independently changes A, C, or B.  The eight v0/c0 triples in the
corpus cover every duplicate topology and byte-match across both compilers.

DP3 and DP4 place source 0 in A and source 1 in C.  Their dword-3 subops are
respectively 2 and 3.  DP3 rewrites both source swizzles to `xyz0`:

```text
DP3 v0,c0  A selectors 0,1,2,5  C selectors 0,1,2,5
DP4 v0,c0  A selectors 0,1,2,3  C selectors 0,1,2,3
```

Selector 5 is the recovered ZERO literal.  This is exact compiler evidence
for excluding W from DP3.  Scalar-result replication to all oC0 lanes is the
D3D operation contract and must still be confirmed by the guarded silicon
oracle; the full XYZW destination mask alone does not prove replication.

## Why command 19 cannot express general MAD

`struct drm_chrome9_ps_instruction` has `src[2]`.  It has no third source and
no duplicate-topology field.  Three different MAD operands are therefore
unrepresentable.  Even when the only files are v0 and c0, the same pair of
descriptors cannot distinguish all of:

```text
v0*c0 + c0
c0*v0 + c0
c0*c0 + v0
```

The first two are arithmetically equal for ordinary finite numbers but encode
differently because A and C are distinct physical slots; compiler-exact ABI
validation cannot silently treat them as one record.  A future structured
successor needs three source descriptors.  Alternatively, a separately
numbered semantic key may hard-code one exact duplicate topology.  Reusing a
two-source generic MAD opcode and guessing the duplicate position is closed.

## Conservative constant bounds

These are rounding-direction-independent first-validator bounds, not maximal
domains.  `magnitude` means `constant_c0[lane] & 0x7fffffff`; the comparison
also rejects exponent-ff values.

| operation/source topology | maximum accepted magnitude per c0 lane | reason |
| --- | ---: | --- |
| DP3/DP4 v0,c0 or c0,v0 | `0x7dffffff` (FLT_MAX/8) | at most four products and four-term absolute sum stays at most FLT_MAX/2 |
| DP3/DP4 c0,c0 | `0x5e800000` (2^62) | four squared lanes sum to at most 2^126, with large overflow headroom |
| MAD with c0 but without c0*c0 | `0x7e7fffff` (FLT_MAX/4) | covers c0*(v0+1), v0*c0+v0, and v0*v0+c0 with at least a factor-two headroom |
| MAD with c0*c0 | `0x5e800000` (2^62) | c0*c0 plus c0 or v0 remains far below FLT_MAX |

DP3/DP4(v0,v0) need no constant; require all four unused c0 words to be zero.
MAD(v0,v0,v0) likewise needs no constant.  These bounds prove only finite
closure assuming ordinary finite IEEE operations.  They do not establish dot
accumulation order, internal precision, denormal handling, or MAD fusion.

Do not map a NIR `fmul` plus `fadd` pair to native MAD.  The hardware could
round the multiply and add separately or fuse them, and the current evidence
does not distinguish those behaviors.  Only a proven operation whose
contraction/rounding contract matches a silicon result may select a future
MAD semantic key.

## First DP3(v0,v0) validator

The first key must admit only one terminal full-vector dot instruction with
both logical sources exactly v0: identity source views, no source modifiers,
no destination saturation or scale, full XYZW oC0 write, and default FP32
controls.  Reject constants, source-file variation, explicit swizzles, ABS,
NEG, partial masks, textures, temporaries, discard, depth writes, control
flow, second outputs, and multi-record programs.  The kernel must construct
the exact record and control words above; userspace never supplies native
words.

Compiler output does not by itself prove that a particular NIR dot operation
has the required scalar replication and precision semantics.  Keep the NIR
match unadvertised until both the live oracle and a Mesa IR contract audit are
complete.  In particular, the classifier must recognize one scalar fdot3
whose result is replicated into all four output-store lanes.  A scalar-only
store or a partial-writemask shortcut is not equivalent.  Both input XYZ
triples must come from identity v0; reject an explicit source swizzle even
though the native DP3 record itself replaces W with ZERO.

## Guarded endpoint pixel oracle

Use the proven command-19 D16/RHW envelope, BGRA8888 target, COPY blend, full
mask, disabled alpha test, flat per-triangle color, and the established guard
and fence checks.  Bracket every candidate with a known-good command-19 MOV
or MUL and require the following draw to retire.  Record exact ALU/control
words, command errors, reset count, surface padding, and both guard pages.

Endpoint colors avoid every unknown rounding choice:

| flat logical v0 RGBA | exact expected logical output | proves |
| --- | --- | --- |
| `(0,0,0,0)` | `(0,0,0,0)` | zero baseline |
| `(1,0,0,0)` | `(1,1,1,1)` | X contributes and scalar result replicates |
| `(0,1,0,0)` | `(1,1,1,1)` | Y contributes |
| `(0,0,1,0)` | `(1,1,1,1)` | Z contributes |
| `(0,0,0,1)` | `(0,0,0,0)` | W is excluded and distinguishes DP3 from DP4 |

Require exact black/white BGRA pixels, unchanged guards, ordered retirement,
no reset/error increment, and a succeeding known-good draw.  Repeat the W-only
and each RGB-basis case for 1, 64, and 1024 draws.  Any timeout, stale output,
non-replicated lane, guard mutation, reset, or subsequent-command failure
keeps the key rejected.

After the exact endpoint rows, add flat byte color `(64,128,128,64)`.  The
ideal DP3 value is `4096/7225`, or 144.565 in UNORM8 units, in every output
lane; DP4 would be 160.627 and MOV would preserve the unequal input lanes.
Record the actual conversion result and accept at most the predeclared
one-byte quantization neighborhood around 145.  This row discriminates the
three operations without making the endpoint gate depend on an unrecovered
rounding rule.

If DP3 closes, DP4(v0,v0) can use the same suite with the W-only row changed
to exact white.  Constant-bearing dot cases and every MAD topology require
separate numerical oracles after their validator bounds are implemented.
