# Chrome9 nested terminal ALU production differential

Status: exact hardware-free Linux production-compiler and configuration-two
differential for one temporary producer followed by terminal ADD, SUB, MIN,
or MAX.  This constrains a future Mesa compiler extension; it is not a runtime
promotion or numerical silicon proof.

## Corpus shape

Every row begins with the same producer:

```text
mov r0,v0
```

The terminal operation writes full XYZW to `oC0` and uses `r0,c0`, `c0,r0`,
or the duplicate pair `r0,r0`.  Exact token bodies, raw/configuration-two
records, controls, physical slots, and release xors are retained in
[chrome9-nested-terminal-alu-production-corpus.tsv](chrome9-nested-terminal-alu-production-corpus.tsv).

All twelve rows have identical control closure:

```text
raw     90040000 91000000 92000002 93000000 ... 9a000000
config2 90058000 91000000 92000002 93000000 ... 9a000000
```

The producer is `01c00530 301c0053 00f04005 000d0020` and is byte-identical
before and after configuration two.  It is live because the terminal record
consumes r0.  The only terminal-record config2 change is the final whole-temp
read marker in its actual physical slot.

## Exact source-slot maps

The logical-to-native mapping is operation-specific:

| operation | logical source 0 | logical source 1 | arithmetic form |
|---|---|---|---|
| ADD | A | B | `B + A*ONE` |
| SUB | B | A | `B + A*(-ONE)` |
| MIN | A | C | native MIN |
| MAX | A | C | native MAX |

Consequently the r0 marker moves exactly as follows:

| slot | marker |
|---|---:|
| A | d2.bit17 = `00020000` |
| B | d0.bit25 = `02000000` |
| C | d1.bit21 = `00200000` |

ADD `r0,c0` marks A while ADD `c0,r0` marks B.  SUB reverses that mapping:
SUB `r0,c0` marks B and SUB `c0,r0` marks A.  MIN/MAX mark A in forward order
and C in reverse order.

When both logical operands are the same temp, only one release marker is
allowed.  ADD and SUB both use A+B and select A; MIN/MAX use A+C and also
select A.  This confirms A-over-C-over-B priority for this new operation set.
The SUB duplicate is the important counterexample to source-index-based
finalization: logical source 1 occupies A and wins over logical source 0 in B.

## Mesa compiler consequence

The current semantic IR has two logical sources, so it can represent each of
these two-input operations.  Its encoder and enum currently admit only MOV and
MUL, with native A/C source packing.  Extending it safely requires:

- a source-B encoder before ADD or SUB;
- an opcode-specific logical-to-physical slot map before lifetime marking;
- physical A-over-C-over-B duplicate-temp arbitration, not `source[0]` wins;
- exact SUB lowering through negative literal ONE, without treating SUB as a
  commutative ADD key;
- retaining full XYZW, identity sources, zero SAT/scale, and the existing
  finite-domain/numerical gates until silicon tests pass.

The current MOV/MUL-only lifetime pass is correct for its A/C subset.  It
would happen to mark duplicate ADD correctly, but would mark duplicate SUB's
B occurrence instead of the required A occurrence if reused by logical source
ordinal.  No new operation should be admitted until slot assignment precedes
the final-use pass.

Evidence used production callbacks parser/validator/processor at backend
offsets `+0x137c/+0x1380/+0x1384`, configuration value two, and the serialized
`/tmp/chrome9-shader-probe.lock`.  No GPU was touched.
