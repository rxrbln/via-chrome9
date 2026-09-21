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

## Static operand boundary

The Linux 5.75 DSO and Windows UMD provide a second, independent boundary on
the compiler records.  This evidence explains fields but does not replace a
compiler differential or a silicon test.  Exact static candidates and their
status are in
[`chrome9-next-alu-static-policy.tsv`](chrome9-next-alu-static-policy.tsv).

The relevant Linux symbols are `stmPPS_GetSrcSetting_invi` at `0x162490`,
`stmModifySourceSwizzle_invi` at `0x169960`, MIN/MAX at
`0x16f480`/`0x16f700`, RCP/RSQ at `0x16abe0`/`0x16aa60`, and ABS at
`0x1727e0`.  The corresponding Windows routines are the source converter at
VA `0x1020f070`, writemask helper at `0x1020e600`, MIN/MAX at
`0x102189b0`/`0x102187f0`, RCP/RSQ at `0x10219920`/`0x10219a80`, and ABS at
`0x10215590`.

For a full XYZW write, the MIN/MAX emitters preserve every X/Y/Z/W selector
from both sources and place source 0 in A and source 1 in C.  Their writemask
helper rewrites selectors only for lanes not written.  This independently
explains why partial writes are not obtained by changing d2 `23:20` alone.
Arbitrary swizzles remain reject-only because the byte-exact dynamic corpus
covers identity operands, not every permutation.

MIN/MAX source-A NEG occupies d1 `27:24`.  Source-C NEG occupies d0
`31:28`.  Importantly, both common MIN/MAX emitters omit d3.bit20 for a
source-C modifier.  The bit-20 behavior recovered from the Windows
programmable MAD converter is therefore operation/path-specific, not a
generic source-C rule.  All source modifiers remain rejected until a direct
compiler differential and numerical test cover the exact operation.

RCP and RSQ are scalar-W operations in both static emitters.  Windows calls
the swizzle helper with mask 8; Linux extracts the packed W selector.  Both
place that selector in native A.w and force native A.xyz to literal zero.
Consequently an identity token selects W and compiles exactly like `.wwww`,
while `.xxxx`, `.yyyy`, and `.zzzz` would select X, Y, and Z through their W
token lane.  This closes the compiler quirk but not reciprocal arithmetic,
so every RCP/RSQ form stays rejected.

ABS is an operation, not evidence for an ABS source modifier.  The exact
identity record expands one source into A=`src`, B=`-src`, C=`src` under
sub-operation 9.  Windows and Linux byte-match for c0.  Linux also emits the
listed v0 record; Windows v0 is only statically predicted from its common ABS
emitter and remains reject-only.  D3D ABS/ABSNEG source-modifier codes have no
cross-driver contract.

## Next fail-closed NIR candidate

After guarded silicon readback, identity-swizzle MIN/MAX are the safest next
structured operations.  With finite c0 and normalized finite v0 they cannot
create infinity or NaN, unlike unrestricted ADD/SUB near the finite endpoint.
The first admission must still require a terminal full-XYZW oC0 write, one
record, no texture or temporary, source NONE modifiers, no SAT, v0/c0 only,
and default FP32 execution modes.  Test both operand orders and signed zero;
do not infer arbitrary swizzle, NEG, partial-write, or temporary support from
the static packing proof.
