# Chrome9 DST one-record audit

DST is a genuine one-record pixel operation.  Four terminal source topologies
using v0/c0/c1 byte-match between the Windows compiler and Linux production
configuration 2.  The exact records are retained in
[`chrome9-dst-one-record-corpus.tsv`](chrome9-dst-one-record-corpus.tsv).
Compiler packing is closed; live silicon semantics are not.

The differential JSON SHA256 is
`db2a6432262113b83a9e4baa816aa1114453375131202ad23b722040a35152fc`.

## Exact lowering

The native arithmetic contract is the established `B + A*C` slot order.  DST
packs its two logical sources as:

```text
A = (ONE, src0.y, src0.z, ONE)
B = ZERO
C = (ONE, src1.y, ONE, src1.w)
```

Therefore the result is the D3D DST vector:

```text
(1, src0.y * src1.y, src0.z, src1.w)
```

All four records use subop 0, terminal other-output class, full XYZW mask,
SAT clear, scale zero, and no dependency rewrite.  The varying-only form is:

```text
DST oC0,v0,v0
0040b6d0 40040863 00f04085 00000820
```

Changing either logical source to a constant changes only the corresponding
native class/id fields.  The compiler does not expand the vector construction
or insert an extra multiply.

## Safest first key

`DST(v0,v0)` is the preferred initial DST key.  D3DColor-normalized v0 lanes
are in `[0,1]`, so the complete result `(1,v0.y^2,v0.z,v0.w)` is finite and
in `[0,1]` under every IEEE rounding direction.  It uses no uploaded
constants, has no overflow or exceptional-input path, and needs only already
understood input and literal ONE/ZERO encodings.

The fail-closed validator must require:

- exactly one ALU record and no TAU;
- exactly `DST oC0,v0,v0` with both logical sources identity v0;
- exact record `0040b6d0 40040863 00f04085 00000820`;
- terminal full XYZW output, SAT clear, scale zero, and default FP32 modes;
- no source modifier, source swizzle, partial mask, temporary, or alternate
  output.

Mesa must recognize the exact four-component expression
`(1.0, v0.y*v0.y, v0.z, v0.w)` produced from the same primary color input.
Do not accept an arbitrary four-component construct, reordered channels,
algebraically transformed multiply, fast-math reassociation, or separate
sources under this first key.  The other three byte-matched topologies remain
compiler evidence only until separately admitted.

## Guarded pixel oracle

Use a flat primary color v0=`(.25,.5,.75,1)`.  The exact expected logical
result is `(1,.25,.75,1)`, whose asymmetric lanes distinguish MOV, source
exchange, an unperformed Y multiply, and common swizzle mistakes.  Repeat
with v0=`(.5,.25,1,.75)` expecting `(1,.0625,1,.75)`.  Require the full BGRA
readback mapping and repeated draws to agree before admission.

Every draw uses command 19's mandatory fresh D16 target, depth clear 1,
LESS/write at z=0.5, COPY, full fixed-function color mask, exact command-word
checks, target/depth guards, ordered fences, reset/error counters, and
known-good draws before and after.  A retired draw without the asymmetric
pixel result is not semantic proof.

## Deferred forms

`DST(v0,c0)` and `DST(c0,v0)` are finite for every finite c0 because the only
mixed product includes a normalized factor no greater than one; nevertheless
they add constant upload and operand-order surfaces and should follow the
v0/v0 gate.  `DST(c0,c1)` can overflow in its Y product and needs an explicit
rounding-independent magnitude policy before admission.  All constant forms,
source swizzles/modifiers, SAT/scale, partial masks, and temporary consumers
remain rejected by the first key.

