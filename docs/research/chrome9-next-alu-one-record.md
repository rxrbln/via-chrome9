# Chrome9 next one-record ALU compiler corpus

This note checkpoints byte-exact compiler evidence for possible successors to
the bounded MOV/MUL pixel-shader path.  It does not authorize any of these
operations in the kernel ABI or Mesa runtime.  The machine-readable source is
[`chrome9-next-alu-one-record-corpus.tsv`](chrome9-next-alu-one-record-corpus.tsv).

## Scope and provenance

The corpus contains 32 `ps_2_0` shaders that write `oC0` with one ALU record,
no texture record, and operands restricted to `v0` and `c0`.  Each payload was
compiled through both proprietary compiler paths:

- Windows scheduled output from the headless shader wrapper;
- Linux production output after the compiler's config-2 finalization pass.

All 32 ALU records match byte for byte between those paths.  The Windows
wrapper is not initialized far enough to produce executable target control:
its first control word is `90040000`, whereas the Linux config-2 output is
`90058000`.  The latter control block is the executable reference.  The match
therefore establishes instruction encoding and source placement, not Windows
target-state validity.

The corpus is compiler evidence only.  None of the new arithmetic semantics
has yet been demonstrated by guarded readback on Chrome9 HD silicon.

## Recovered source placement

The static ALU relation is best described as `B + A * C`, with an operation
selector changing the numerical unit where appropriate:

| D3D operation | Recovered placement |
| --- | --- |
| ADD | `A=src0`, `B=src1`, `C=ONE` |
| SUB | `B=src0`, `A=src1`, `C=NEG_ONE` |
| MAD | `A=src0`, `C=src1`, `B=src2` |
| DP3, DP4, MIN, MAX | `A=src0`, `C=src1` |
| CMP | `A=condition`, `C=true`, `B=false` |

For CMP, the intended D3D relation is `src0 >= 0 ? src1 : src2`.  The table
records the compiler placement but does not prove comparison boundary, NaN,
or signed-zero behavior in hardware.

## Runtime rejection boundaries

RCP and RSQ remain rejected.  The proprietary frontends compile an identity
source token as the same ALU record as `.wwww`; that is not proof that an
identity vector operand has defined scalar semantics.  Even explicit `.wwww`
is only byte-matched compiler evidence until scalar selector, exceptional
input, precision, and sign/domain behavior are tested on silicon.

Partial writes also remain rejected.  The RGB and alpha ADD probes change
several operand/coissue words, not merely the destination mask, and leave the
unwritten color lanes outside this corpus's closed model.

Saturate remains offline-only.  The compiler consistently toggles bit 12 in
ALU dword 3, but render-target clamping can conceal whether saturation occurs
inside the ALU.  A future oracle should use a downstream comparison such as an
alpha test, not only BGRA framebuffer readback.

## Safest validation order

A guarded silicon campaign should promote operations independently and keep
the submitted program to one ALU record:

1. MIN/MAX with finite values, including operand-order and signed-zero cases.
2. ADD/SUB with exactly representable fractions and asymmetric operands.
3. CMP with a `c0` condition and different `v0`/`c0` arms.
4. DP3/DP4 with a deliberately distinct alpha component.
5. MAD with exact binary fractions, while treating fused rounding as unknown.
6. RCP only for finite nonzero `c0`, and RSQ only for finite positive normal
   `c0`, after an explicit scalar-selector oracle exists.

Until each step passes guarded readback and wedge recovery, it must remain an
offline compiler result rather than a structured runtime opcode.
