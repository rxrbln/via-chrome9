# Chrome9 semantic compiler MIN successor

Status: implementation-ready compiler/validator design based on exact
Linux/Windows byte matches and a read-only audit of Mesa Chrome9 commit
`e34de89`.  Native MIN arithmetic has not yet passed a guarded VX900 pixel
oracle, so this note does not authorize runtime exposure.

## Smallest safe next operation

The smallest successor to the current semantic MOV/MUL encoder is terminal
full-vector MIN with exactly source 0=v0 and source 1=c0:

```text
nir:       oC0 = fmin(v0, c0)
native:    01c00530 30080053 00f04005 000c0820
A90-A93:   90058000 91000000 92000001 93000000
```

Both proprietary compilers produce that native record byte for byte.  The
program is ALU-first, allocates no temporary, has one ALU and no TAU, and
therefore remains in the now-exact first A90 resource bucket.  It needs no
new native source field: logical source 0 occupies A and logical source 1
occupies C, exactly like MUL.

For a structured encoder the MIN base before packing sources and destination
is:

```text
01c00530 00000000 00f00000 000c0820
```

Applying the existing source-A encoder to v0, source-C encoder to c0, and the
existing full oC0 destination encoder recreates the complete record above.
MAX differs only in the dword-3 operation field, `000b0820`, but should remain
a separately gated successor.

## Why current lifetime code is sufficient for MIN

Mesa `e34de89` validates semantic IR in a first pass and marks terminal reads
in a second pass.  For MOV/MUL, logical sources already correspond to physical
A/C slots.  MIN has the same A/C mapping, so the existing same-record rule is
correct unchanged:

- a final temporary in source 0 receives A d2.bit17;
- a final temporary in source 1 receives C d1.bit21; and
- the same temporary in both operands receives only A, preserving A-over-C
  precedence.

The production nested corpus independently proves all three cases.  With
`mov r0,v0` followed by terminal MIN, the finalized records are:

```text
MIN oC0,r0,c0  01c00530 30080053 00f20005 000c0820
MIN oC0,c0,r0  01c00530 30200053 00f08005 000c0820
MIN oC0,r0,r0  01c00530 30000053 00f20005 000c0820
```

This compatibility does not generalize automatically to ADD or SUB.  ADD
uses A/B and SUB reverses its logical operands into B/A, so those operations
need physical-slot assignment before the last-use pass and a new source-B
encoder.

## Fail-closed NIR contract

The first runtime classifier should accept only:

- a straight-line fragment shader with one terminal `nir_op_fmin`;
- FP32 vec4 source 0 exactly primary color v0 and source 1 exactly c0;
- identity XYZW swizzles, no NEG/ABS/SAT, full XYZW oC0, and no other output;
- no temporary, texture, discard, depth output, control flow, or explicit
  non-default float-control mode; and
- four finite c0 lanes.  v0 is D3DColor-normalized and therefore finite in
  `[0,1]`.

Keep reversed operands, c0/c1, `.aaaa`, literal ONE/ZERO swizzles, partial
writes, and nested MIN rejected for the first gate even though compiler bytes
exist for several of them.  This separates encoder completeness from silicon
semantics.

NIR floating MIN behavior is unsafe for NaNs until hardware NaN selection is
measured.  Requiring both sources finite removes that ambiguity.  Signed-zero
selection is not observable through ordinary normalized BGRA readback, so the
first capability must not promise IEEE sign-preserving `fmin`; a later
alpha/depth or downstream-operation oracle must close it if an API path
depends on the sign.

## Kernel ABI boundary

The existing structured instruction already has two logical sources and can
represent MIN without changing request size.  A safe kernel successor adds a
new logical opcode and rebuilds the one exact record above.  It must not accept
native dwords, source ids other than v0/c0, modifiers, nonidentity swizzles,
or caller-provided controls.

Do not silently broaden the meaning of capability 41, whose UAPI comment says
MOV/MUL.  Allocate a separately coordinated feature bit when implementation
starts; this note intentionally does not reserve one because bits 42--46 are
already assigned and other active work may consume the next slot.

The validator order stays pre-publication:

1. validate opcode, exact source tuple, all reserved bytes, and finite c0;
2. synthesize the native MIN record and fixed A90--A9b tuple;
3. only then enter BO reservation, mapping, ring publication, and fencing.

Any failure remains `-EINVAL` before command counters or ownership change.

## Required silicon gate

Use the already proven D16/RHW program-batch envelope with COPY blend, full
color mask, alpha test disabled, and guarded BGRA8888 target.  Surround each
candidate with a known-good MOV or MUL draw.  Primary v0 should be asymmetric,
for example `(32,96,160,224)/255`, and c0 should alternate the winning source
per lane, for example `(1,0,0.5,0.5)`, expecting logical bytes
`(32,0,128,128)`.

Run one, 64, and 1024 candidate draws.  Require exact pixels, unchanged color
and depth guards, ordered retirement, clean engine status, no reset-count
change, and a successful following known-good draw.  A timeout, stale output,
guard change, or arithmetic mismatch keeps MIN unadvertised.

## Evidence

Exact terminal records are in
[chrome9-next-alu-one-record-corpus.tsv](chrome9-next-alu-one-record-corpus.tsv).
Nested temporary placement and final-read deltas are in
[chrome9-nested-terminal-alu-production-corpus.tsv](chrome9-nested-terminal-alu-production-corpus.tsv).
The resource bucket closure is in
[chrome9-ps-resource-config-buckets.md](chrome9-ps-resource-config-buckets.md).
The broad numerical oracle is retained in
[chrome9-min-add-rung.md](chrome9-min-add-rung.md).

