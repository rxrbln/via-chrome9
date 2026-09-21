# Chrome9 nested MAD production differential

Status: exact hardware-free Linux production compiler/configuration-two
differential.  It pins all three MAD source slots and every duplicate-temp
release combination.  It does not prove fused/unfused numerical semantics on
silicon and does not make MAD admissible through the current two-source IR.

Every row starts with `mov r0,v0`, then executes a full-XYZW terminal MAD to
`oC0`.  The three logical operands cover r0 alone in each position, each pair
of duplicate positions, and all three positions.  Exact tokens and raw/final
records are in
[chrome9-nested-mad-production-corpus.tsv](chrome9-nested-mad-production-corpus.tsv).

All seven rows use the same two-record ALU-only controls:

```text
raw     90040000 91000000 92000002 93000000 ... 9a000000
config2 90058000 91000000 92000002 93000000 ... 9a000000
```

The production compiler maps D3D MAD exactly as expected by the recovered
native arithmetic contract:

| D3D logical operand | native slot |
|---:|---|
| source 0 | A |
| source 1 | C |
| source 2 | B |

The native expression is `B + A*C`.  Placing r0 once in source 0, 1, or 2
therefore moves its sole config2 change among A d2.bit17, C d1.bit21, and B
d0.bit25.

Duplicate forms prove the complete arbitration order within one record:

| duplicate physical slots | emitted marker |
|---|---|
| A+C | A only |
| A+B | A only |
| C+B | C only |
| A+C+B | A only |

This is direct production evidence for A-over-C-over-B.  A finalizer must
count a duplicated logical temp once for release, after source allocation and
slot assignment.

The current Mesa semantic instruction has only `source[2]` and therefore
cannot represent arbitrary MAD without an ABI-neutral internal IR extension.
Even after adding a third source and source-B packing, ordinary NIR `fmul`
plus `fadd` must not be contracted: hardware fusion/rounding behavior remains
unknown.  Only an explicitly compatible NIR form and a bounded numerical
silicon oracle can promote MAD.  The special shape
`MAD(v0,c0,c0) = c0*(v0+1)` can use the prior conservative constant-magnitude
bound `bits(abs(c0)) <= 0x7e7fffff`, but remains rejected until that semantic
gate exists.

Evidence used production callbacks parser/validator/processor at backend
offsets `+0x137c/+0x1380/+0x1384`, configuration value two, and the serialized
`/tmp/chrome9-shader-probe.lock`.  No GPU was touched.
