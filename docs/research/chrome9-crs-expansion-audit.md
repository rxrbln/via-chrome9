# Chrome9 pixel CRS expansion audit

Pixel CRS is a stable two-record expansion in both production compilers.  All
five tested terminal topologies have identical arithmetic words between
Windows and Linux before dependency finalization.  Linux configuration 2 adds
only the second record's temporary-B dependency bit.  CRS is therefore a good
future multi-record compiler target, but it is not a one-record command-19
successor and remains rejected today.

## Exact lowering

For logical sources `a` and `b`, the first record computes:

```
t0 = ZERO + a.zxy0 * b.yzx0
```

The second computes:

```
oC0 = -t0 + a.yzx0 * b.zxy0
     = (a.y*b.z-a.z*b.y,
        a.z*b.x-a.x*b.z,
        a.x*b.y-a.y*b.x,
        0)
```

For `CRS c0,c1`, Windows emits:

```
0140b6d0 d0081285 00f08040 00000020
00000538 5008140d 00f08028 00000820
```

Linux configuration 2 emits the same first record and changes only the second
dword 0 from `00000538` to `02000538`, marking the temporary B source as a
dependency.  Windows controls are A90=`90040000`, A92=`92000002`; Linux uses
A90=`90058000`, A92=`92000002`.  The same dependency delta holds for `v0,v1`,
`v0,v0`, `v0,c0`, and `c0,v0`; all exact words are in
[`chrome9-crs-expansion-corpus.tsv`](chrome9-crs-expansion-corpus.tsv).

This result also explains the older r0 corpus context, whose record-tail bits
differ because it stores to a temporary rather than terminal oC0.  Destination
context must not be mixed into the executable terminal contract.

The five-case differential JSON SHA-256 is
`0bd5da93a67836703497acc39c055f6e322cddc33d517116e720f46ccab943ee`.

## Runtime and numerical boundary

Keep CRS rejected by the current one-record validator.  The lowering needs one
compiler-managed temporary, an ordered pair of records, and a configuration-2
dependency rewrite.  Those properties are outside the current structured
UAPI even though the record algebra is now exact.

`CRS(v0,v1)` is the preferred future bounded topology because both D3D color
inputs are normalized to [0,1].  Each product lies in [0,1] and each component
difference lies in [-1,1], independent of floating-point rounding direction;
there is no overflow.  `CRS(v0,v0)` is identically zero and is not a useful
functional oracle.  Constant-bearing forms remain rejected until a separate
constant-domain contract is justified; arbitrary finite products can
overflow.

A future live gate can use flat `v0=(0.25,0.75,0.5,1)` and
`v1=(0.8,0.2,0.6,1)`, for cross-product RGB approximately
`(0.35,0.25,-0.55)`, W=0.  The negative blue lane is expected to clamp in the
UNORM target, while positive red and green distinguish the operand order and
both swizzle cycles.  A reverse-order companion draw should exchange the sign
pattern.  The test must use command 19's mandatory fresh D16, depth clear 1,
LESS/write, passing z=0.5, guarded destination, and readback.  Compiler bytes
do not by themselves prove silicon arithmetic, so CRS remains fail-closed
until that oracle and a multi-record UAPI are deliberately enabled.
