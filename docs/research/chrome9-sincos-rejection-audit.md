# Chrome9 SINCOS and split SIN/COS rejection audit

This audit closes the public ps_2_0 SINCOS expansion for terminal oC0 masks
X, Y, XY, and XYZW.  All four Windows/Linux probes compile successfully to
three ALU records.  Exact streams are in
[`chrome9-sincos-terminal-corpus.tsv`](chrome9-sincos-terminal-corpus.tsv).
Split SIN and COS remain internal-frontend-only and have no production
dynamic byte contract.

The terminal differential JSON SHA256 is
`b5fb1b5ccae22688252a684b2b11c12f40fa3089ff4d0eaa0bccd5c53f34766f`.

## Public SINCOS expansion

For every destination mask, the Windows raw schedule begins with the same two
records:

```text
01c00530 b01c0053 00f080b6 000d0020
0140b6d0 30096000 00f00005 00000020
```

The terminal record is:

```text
01c00530 301c0053 00M00005 000f0820
```

where native destination mask `M` is 1, 2, 3, or f.  Linux configuration 2
changes only this record's source word from `00M00005` to `00M20005`, adding
the generic source-A dependency bit.  Full XYZW is therefore
`01c00530 301c0053 00f20005 000f0820` in the Linux execution stream.

Windows reports `A90=90040000`, `A92=92000003`; production Linux reports
`A90=90058000`, `A92=92000003`.  The first record uses the dedicated scalar
c0.w source and subop d, the middle record uses subop 0, and the terminal uses
subop f.  Destination masking does not select a shorter or split operation.
The three source tokens c0/c1/c2 are accepted without payload range checks;
the two auxiliary approximation constants remain part of the indivisible
vendor lowering.

## Split SIN and COS boundary

Static cross-reference assigns internal opcode 105 to SIN and 106 to COS.
The Windows programmable dispatcher calls shared emitter `0x10219be0` with
mode 1 for SIN at `0x101dccb4` and mode 0 for COS at `0x101dccc7`.  The Linux
symbolized parser and dispatcher establish the same direction through
`stmPPS_SIN_COS_ICD_PS20_invi` at `0x00162ca0`.

The public D3D SINCOS emitter at `0x10219f20` never exposes that mode argument:
X-only, Y-only, XY, and XYZW tokens all retain the same three-record schedule
and vary only the terminal destination mask.  The internal grammar path has
not been invoked with a production-initialized allocator/finalizer, so static
emitter reachability is not an executable SIN/COS byte contract.  Mesa must
not infer a direct one-record sine or cosine key from subop f or from the
internal dispatch addresses.

## Fail-closed policy and live boundary

SINCOS remains rejected because the runtime admits one ALU record, while its
public lowering requires three records and two auxiliary source constants.
Split SIN/COS remain rejected because their dynamic record count, allocation,
finalization, numerical range reduction, lane placement, and precision are
unproven.  Partial destination masks remain rejected even though their public
compiler packing is exact.  SAT, scale, source modifiers/swizzles, temporary
destinations, and exceptional values are also outside this corpus.

If a future multi-record UAPI admits public SINCOS, start with bounded finite
c0.w angles whose expected sine and cosine both lie in `[0,1]`, retain the
exact vendor c1/c2 constants, and independently observe X/Y lane assignment.
Negative results need A33 discrimination or a signed/float target because a
terminal UNORM render target masks them.  A retired draw alone is not numeric
proof.

Every live experiment uses command 19's mandatory fresh D16 target, depth
clear 1, LESS/write at z=0.5, COPY, exact command-word checks, target/depth
guards, ordered fences, reset/error counters, and known-good draws before and
after.  Only after public SINCOS passes should a production-initialized split
emitter wrapper be considered; it must be treated as a separate ABI and
silicon gate.

