# Chrome9 terminal FRC one-record rung

This note defines the next bounded ALU candidate after the identity MOV/MUL
path and the separately documented MIN/MAX arithmetic rungs. It does not
authorize a kernel or Mesa capability. All six records in
[`chrome9-frc-one-record-corpus.tsv`](chrome9-frc-one-record-corpus.tsv) were
regenerated through both the 2015 Windows D3D compiler and the Linux 5.75
production compiler with configuration-two finalization.

## Exact encoding

A terminal full-XYZW `FRC oC0,src` is one native ALU record. FRC selects
sub-operation `e`; the source occupies native A and the unused B/C slots keep
their compiler-owned literal shape:

```text
FRC oC0,c0  01c00530 301c0053 00f08005 000e0820
FRC oC0,v0  01c00530 301c0053 00f04005 000e0820
```

The executable Linux control image is the ordinary one-record ALU-only tuple:

```text
A90..A9b 90058000 91000000 92000001 93000000
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000 9b000000
```

The headless Windows object remains pre-target at `A90=90040000`; its ALU
record nevertheless matches the Linux execution form byte for byte. No TAU,
temporary, varying beyond v0, or extra constant is present.

Replicated alpha changes both native A swizzle halves:

```text
FRC oC0,c0.aaaa  01c00530 b01c0053 00f0806d 000e0820
FRC oC0,v0.aaaa  01c00530 b01c0053 00f0406d 000e0820
```

NEG is independently byte-exact on both source files and changes only native
A's modifier nibble in dword 1 from zero to eight:

```text
FRC oC0,-c0  01c00530 381c0053 00f08005 000e0820
FRC oC0,-v0  01c00530 381c0053 00f04005 000e0820
```

This is stronger than the ADD/SUB NEG evidence: those paths disagree on the
Windows-only dword-3 companion bit, whereas this FRC set is byte-identical.
It still proves encoding only, not the numerical meaning of the modifier on
silicon.

## Why FRC is the next fail-closed operation

The intended PS 2.0/NIR relation is `fract(x) = x - floor(x)`. For finite
input its mathematical result is bounded to `[0,1)`, while FP32 evaluation
may round a small negative fraction to exactly 1.0.  Thus a finite input stays
finite in `[0,1]` and cannot create an infinity or NaN. The normalized D1 v0
input is already finite and in `[0,1]`.
That makes terminal identity-swizzle `ffract(v0)` the narrowest new semantic
key: it consumes no user float constant and has no overflow surface.

It is not yet live-proven. In particular, negative-input floor versus
truncate behavior, input precision for large values, signed zero, denormal
handling, and the location of source NEG relative to FRC remain unknown.
Therefore the first admission must be a separate semantic key, not a general
one-record encoder.

The initial NIR classifier should require all of the following:

- one entry point and one acyclic block;
- exactly one terminal `ffract` and one full-XYZW color store;
- a direct full-vector v0 source with identity swizzle;
- no constant, source modifier, destination modifier, partial mask,
  temporary, texture, discard, depth output, integer operation, indirect,
  or second side effect;
- default FP32 execution controls and the existing normalized D1 input;
- a kernel-owned exact record and the fixed one-record control image above.

Copies and SSA wrappers may be ignored only when they do not change component
selection. Scalarized or reconstructed vectors, `fmod`, `floor` plus subtract,
and algebraically equivalent expressions are not the same closed key. General
`FRC(c0)`, `.aaaa`, and NEG remain separate later keys even though their bytes
are now known.

This one-record key is useful principally as an encoder and execution rung.
It does not by itself implement ioquake3 shadow packing or glamor repeat
repair, both of which need downstream arithmetic/selection and therefore a
multi-record dependency proof.

## Guarded silicon sequence

Command 19 requires D16; do not describe this as a depth-disabled oracle.
For every row use a fresh guarded D16 surface cleared to one, proven
LESS/write state, vertex z=0.5, one guarded BGRA8888 target, COPY blend, full
color mask, alpha test disabled, and the unchanged known-good common state.
Run `known-good MOV -> candidate -> known-good MOV`, and check target/depth
guards, retirement, engine/error state, and reset counters after each draw.

Promote one boundary at a time:

1. **Identity v0.** Use flat D1 lanes containing zero, interior byte values,
   and 255. Lanes below 255 must reproduce the normalized input; a 255 lane
   must become zero. This distinguishes FRC from MOV without user constants.
2. **Identity c0.** Use exact binary values `(0.25, 0.5, 0.75, 1.0)` and
   require `(0.25, 0.5, 0.75, 0.0)` within the already characterized UNORM
   store conversion.
3. **Negative floor rule.** Use c0
   `(-0.25, -0.5, -0.75, -1.0)` and require
   `(0.75, 0.5, 0.25, 0.0)`. A truncate-based implementation would instead
   expose negative fractions and is not NIR `ffract`.
4. **Source NEG.** Repeat the preceding expected result with positive c0 and
   `FRC(-c0)`. This isolates dword-1 modifier eight from signed constant
   upload. Repeat with positive v0 before allowing NEG on interpolated input.
5. **Replicated alpha.** Give c0 distinct RGB lanes and alpha 0.25, then
   require all four output lanes to equal 0.25 from `c0.aaaa`. Repeat on v0.

Use exact 0, 0.25, 0.5, 0.75, and 1 values first. Do not begin with denormals,
large exponents, NaN, or infinity. Exponent-`ff` constants must still fail
before reservation/publication. A mismatch disables only the candidate key;
it must not fall through to a nearby native record.

## Admission order

After guarded results, the safe order is:

1. identity `FRC(v0)`;
2. identity `FRC(c0)` with all four finite lanes restricted initially to
   `[-2,2]` and default FP mode;
3. `.aaaa` only after the replicated-lane oracle;
4. NEG only after both signed-constant and source-modifier oracles agree.

The small initial c0 bound is a testable policy boundary, not a recovered
hardware limit. Widen it only after integer-boundary and large-magnitude
precision vectors establish the required GL/NIR behavior. ABS source
modifiers remain unrelated and rejected; native ABS is a separate opcode.
