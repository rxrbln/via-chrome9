# Chrome9 pixel CRS expansion audit

Pixel CRS is a stable two-record expansion in both production compilers.  All
five tested terminal topologies have the same arithmetic and swizzle lowering,
but not identical records: production Windows allocates special r12 while
Linux allocates ordinary r0.  Both configuration-2 streams mark the second
record's temporary-B read as last use.  CRS is therefore a good future
multi-record compiler target, but it is not a one-record command-19 successor
and remains rejected today.

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

For `CRS c0,c1`, finalized production Windows emits:

```
0140b6d0 d0081285 00f08040 00000320
020c0538 5008140d 00f08028 00000820
```

Finalized Linux emits the same operand algebra through ordinary r0:

```
0140b6d0 d0081285 00f08040 00000020
02000538 5008140d 00f08028 00000820
```

The Windows second record changes from raw `000c0538` to finalized
`020c0538`; Linux likewise changes ordinary-r0 `00000538` to `02000538`.
This bit is the temporary-B last-use marker, not a producer dependency.
Windows controls are A90=`90040000`, A92=`92000002`; Linux uses
A90=`90058000`, A92=`92000002`.  The same allocation and finalization split
holds for `v0,v1`, `v0,v0`, `v0,c0`, and `c0,v0`; all exact words are in
[`chrome9-crs-expansion-corpus.tsv`](chrome9-crs-expansion-corpus.tsv).

The prior corpus was generated with a Windows identity allocator that omitted
`stmSetRegMgr(instance,8,12)`.  Its Windows r0 bytes only coincidentally
matched Linux before finalization and are not production compiler output.
Destination and allocator context must not be mixed into the executable
terminal contract.

The corrected nine-case CRS/SINCOS production differential JSON SHA-256 is
`51fd2836baf75ff03460eff830ae310709b090626319468ab2ac621a1b5bae6e`.
It used Windows production-RegMgr wrapper SHA-256
`c56637fad7bfaeb994595e3363d439b7f0e35bcf78138bdc0d2bfdc70fdfa4e1`,
which collects usage and calls `stmSetRegMgr(instance,8,12)` before dispatch,
and Linux production probe SHA-256
`a22c873536dc4b6c45c43a00a48dbc183e40ddb89ed5e1461890efad2d21e6ba`.

## Runtime and numerical boundary

Keep CRS rejected by the current one-record validator.  The lowering needs one
compiler-managed temporary, an ordered pair of records, a configuration-2
last-use rewrite, and an explicit scratch-allocation/control policy.  Those
properties are outside the current structured UAPI even though the record
algebra is now exact.

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
