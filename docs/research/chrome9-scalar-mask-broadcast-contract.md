# Chrome9 scalar destination-mask and broadcast contract

This audit covers native scalar RCP, RSQ, EXP, and LOG with source c0.w.  The
56 dynamic cases are compacted into 28 terminal/temp pairs in
[`chrome9-scalar-mask-corpus.tsv`](chrome9-scalar-mask-corpus.tsv).  Compiler
packing is closed; numerical precision and hardware scalar broadcast are not.

## Direct destination masks

For each operation, Windows and Linux byte-match terminal oC0 masks X, Y, Z,
W, XY, XYZ, and XYZW.  The mask changes only d2 bits 23:20:

| D3D mask | native d2 mask nibble |
| --- | ---: |
| X / Y / Z / W | `1` / `2` / `4` / `8` |
| XY / XYZ / XYZW | `3` / `7` / `f` |

Every record retains the dedicated scalar source shape:

```text
d0 = 01c00530
d1 = b01c0053
d2 low 20 bits = 080b6
d3 subop = EXP 4, LOG 5, RCP 6, RSQ 7
```

Decoded native A is `(ZERO,ZERO,ZERO,c0.w)`.  There is no per-destination-lane
source rewrite or hidden expansion.  For example RCP X and W are respectively
`01c00530 b01c0053 001080b6 00060820` and
`01c00530 b01c0053 008080b6 00060820`.

This proves that both compilers expose independent destination write bits.  It
does not prove that the scalar unit produces the same numerical value for all
enabled hardware lanes.

## Masked temporaries

All 28 masked scalar-producer chains compile as two records when the MOV
consumer replicates a lane that the producer wrote.  Production Linux assigns
the consumer selector before target finalization:

| produced/read lane | Linux raw MOV A source | configuration-2 MOV A source |
| --- | --- | --- |
| X | `001c0053 00f00000` | `001c0053 00f20000` |
| Y | `901c0053 00f00024` | `901c0053 00f20024` |
| Z | `201c0053 00f00049` | `201c0053 00f20049` |
| W | `b01c0053 00f0006d` | `b01c0053 00f2006d` |

Thus the scheduler models a scalar result as independently available in every
written lane; configuration 2 adds only source-A dependency bit 17.  The
producer is unchanged.  XY/XYZ cases deliberately consume replicated X, and
full XYZW consumes identity.  No row reads an unwritten lane.

The Windows headless wrapper emits an X consumer for every temporary because
its synthetic identity allocator does not reproduce production component
allocation.  This known wrapper limitation means only the direct terminal
records are cross-driver byte contracts.  All temporary rows are offline
Linux allocation/finalizer evidence and do not authorize multi-record runtime.

## Fail-closed kernel and Mesa boundary

The first scalar UAPI keys must accept only:

- exactly one ALU record and no TAU;
- source exactly c0.w, encoded as native A=`000W` with no modifier;
- terminal oC0 with full XYZW mask `f`;
- no SAT, result scale, temporary, partial write, or alternate output;
- default FP32 modes and all c0 lanes finite;
- the operation-specific selected-lane interval below.

| operation | required c0.w |
| --- | --- |
| RCP | positive `[1,16]` |
| RSQ | positive `[1,256]` |
| EXP | `[-4,0]` |
| LOG | positive `[1,2]` |

Mesa must match a terminal full-vector store whose four components are the
same scalar SSA result of the exact operation on `channel(c0,3)`.  A full
store of four independent scalar operations is not the same structural key.
Reject another source channel, an identity-vector source, source NEG/ABS,
destination SAT or scale, partial store masks, temporary users, algebraic
reciprocal/power rewrites, and explicit non-default FP controls.

Partial terminal masks are compiler-exact but remain rejected until the live
preservation test below passes.  Masked temporaries remain rejected even after
that test because the current runtime is intentionally one record.

## Broadcast and mask silicon tests

Use command 19's mandatory fresh D16 target, clear depth 1, LESS/write at
z=0.5, COPY, full fixed-function color mask, exact command-word checks,
target/depth guards, ordered fences, reset/error counters, and known-good
draws before and after.

For full-mask broadcast, prefill four unequal channel sentinels, submit the
exact full record, and require all four BGRA lanes to become the same expected
scalar value.  Use the exact-power inputs:

```text
c0.w RCP inputs 2,4,8,16       -> .5,.25,.125,.0625
c0.w RSQ inputs 4,16,64,256    -> .5,.25,.125,.0625
c0.w EXP inputs -4,-3,-2,-1    -> .0625,.125,.25,.5
c0.w LOG inputs 1,2^.25,2^.5,2^.75 -> approximately 0,.25,.5,.75
```

Each list denotes four separate W-source draws, not vector execution in one
instruction.  Each draw must broadcast its scalar result into every output
lane.  Allow at most one byte around the nearest UNORM value in this coarse
gate.  A retired draw or a full destination mask alone is not proof.

Then test X, Y, Z, W, XY, and XYZ terminal masks with a result near 0.5.
Prefill every logical channel with a different non-0.5 sentinel.  Written
lanes must change to the scalar result and every unwritten lane must remain
bit exact.  Use COPY blending and a full fixed-function color mask so only the
shader destination mask changes.  Keep partial masks rejected if any lane
mapping or preservation differs.

## Precision limit and A33 brackets

Offline compilation cannot establish transcendental/reciprocal precision.
After exact-power controls, use full-mask A33 pairs that observe the pre-RT
alpha value:

| operation/value | must pass | must fail |
| --- | --- | --- |
| RCP(3) ~= .3333333 | GREATER/ref84 `33004151` | GREATER/ref86 `33004159` |
| RSQ(2) ~= .7071068 | GREATER/ref180 `330042d2` | GREATER/ref181 `330042d6` |
| EXP(-.5) ~= .7071068 | GREATER/ref180 `330042d2` | GREATER/ref181 `330042d6` |
| LOG(1.5) ~= .5849625 | GREATER/ref149 `33004256` | GREATER/ref150 `3300425a` |

Known alpha-test controls must pass first; otherwise the pair is
inconclusive.  These approximately one-byte brackets prove only coarse
monotonic numerical behavior suitable for the bounded UNORM experiment.  They
do not establish an FP32 ULP bound, exceptional-value semantics, or permission
for arbitrary NIR lowering.  Any failed broadcast, preservation, bracket, or
repeatability check keeps the corresponding operation rejected.
