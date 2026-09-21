# Chrome9 PS 3.0 DCL negative boundary

This bounded differential found no accepted pixel-input declaration stream.  It
does not prove that every possible DCL encoding is unsupported: the two DCL
token candidates were constructed directly and were not independently
confirmed by a known-good assembler.  It does establish a fail-closed boundary
for the current compiler work.

## Exact probe

The corrected production-RegMgr artifact is `/tmp/chrome9-dcl-production.json`,
SHA-256
`a4d1d38614dac341ebb99ddde6e16672336271c3c8c29293a45942f1b134142d`.
The positive PS 3.0 framing control, `mov oC0,v0`, byte-matches:

```
01c00530 301c0053 00f04005 000d0820
```

Both production compilers report `90058000/92000001`, one phase, and one
color output.  The prior JSON `4b41b051...` omitted the Windows
register-manager builder; its baseline record and DCL rejection were accurate,
but A90=`90040000` was a harness artifact.

The candidate `dcl_texcoord0 v0` token sequence uses opcode `0x1f`, semantic
token `00000005`, and destination `100f0000`.  Windows stops in the tokenizer
(`wine_shader_probe` exit 40) and Linux returns compiler rejection (exit 4), so
neither produces a native record.  Adding candidate destination modifier bit
`0x00400000` for centroid produces the same boundary.

Both rejected candidates stop before register-manager setup can affect a
native program.  Rerunning them with the production wrapper reproduces the
same Windows tokenizer rejection and Linux compiler rejection.

## Policy

Default `v0` consumption without a declaration remains compiler-supported, as
the positive control shows.  That is not evidence for arbitrary declarations,
semantic remapping, centroid interpolation, or other interpolation qualifiers.
NIR requiring any of those features must therefore be rejected until an
assembler-confirmed token stream produces native metadata and records in both
compiler paths.  There is no live-silicon oracle for a rejected stream.  Any
future oracle still uses command 19's mandatory fresh D16 clear=1, LESS/write,
and passing z=0.5 contract.
