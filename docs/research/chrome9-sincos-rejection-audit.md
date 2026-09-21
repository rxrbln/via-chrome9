# Chrome9 SINCOS and split SIN/COS rejection audit

This audit closes the public ps_2_0 SINCOS expansion for terminal oC0 masks
X, Y, XY, and XYZW.  All four Windows/Linux production probes compile
successfully to three ALU records.  The arithmetic sequence agrees, but
Windows allocates special r12 while Linux allocates ordinary r0.  Exact
finalized streams are in
[`chrome9-sincos-terminal-corpus.tsv`](chrome9-sincos-terminal-corpus.tsv).
Split SIN and COS remain internal-frontend-only and have no production
dynamic byte contract.

The corrected nine-case CRS/SINCOS production differential JSON SHA256 is
`51fd2836baf75ff03460eff830ae310709b090626319468ab2ac621a1b5bae6e`.
The prior SINCOS JSON `b5fb1b5c...` used a Windows identity allocator that
omitted `stmSetRegMgr(instance,8,12)`; its r0 schedule was not production
Windows output.

## Public SINCOS expansion

For every destination mask, the Windows production raw schedule begins with
the same two records through special r12:

```text
01c00530 b01c0053 00f080b6 000d0320
0140b6d0 30096000 00f00c05 00000320
```

The terminal record is:

```text
01c00530 301c0053 00M00c05 000f0820
```

where native destination mask `M` is 1, 2, 3, or f.  Windows configuration 2
marks the terminal special-r12 read as last use, changing `00M00c05` to
`00M20c05`.  Linux emits the same subop sequence through ordinary r0 and its
final record is `01c00530 301c0053 00M20005 000f0820`.  Full XYZW is thus
`01c00530 301c0053 00f20c05 000f0820` on Windows and
`01c00530 301c0053 00f20005 000f0820` on Linux.

Windows reports `A90=90040000`, `A92=92000003`; production Linux reports
`A90=90058000`, `A92=92000003`.  The first record uses the dedicated scalar
c0.w source and subop d, the middle record uses subop 0, and the terminal uses
subop f.  Destination masking does not select a shorter or split operation.
The two compiler streams must not be mixed: each needs its matching scratch
allocation and A90 control.  The three source tokens c0/c1/c2 are accepted
without payload range checks;
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
