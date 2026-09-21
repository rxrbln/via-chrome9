# Chrome9 TEXLDP/TEXLDB compiler boundary

Terminal projected and biased samples have exact, matching operation fields in
the Windows and Linux production compilers.  Linux configuration 2 performs a
substantial execution-form dependency rewrite on the sample-to-output path;
the Windows wrapper retains the predependency form.  These bytes close the
compiler packet boundary but do not prove projective division, bias sign/unit,
LOD selection, or mip addressing on silicon, so both operations remain
rejected.

## Exact terminal programs

Both test programs sample `r0` from `t0,s0`, then move `r0` to full oC0.
Windows emits the common output ALU:

```
01c00530 001c0053 00f00000 000d0820
```

with TAU `207001b0` for TEXLDP and `203001b0` for TEXLDB.  Thus bits 23:20 are
operation 7 and 3 respectively.  The Windows controls are:

```
90000000 91000000 92010001 93000001
```

Linux configuration 2 rewrites the output ALU and adds the TAU dependency:

```
01c00530 301c0053 00f20005 000d0820
TAU 307001b0  # TEXLDP
TAU 303001b0  # TEXLDB
```

with controls:

```
90018000 91000001 92010001 93000001
```

The Linux object reports texture-register mask 1 and temporary mask 1.  This
Windows wrapper reports both masks zero, reinforcing that executable Linux
configuration-2 words and controls must be treated as one coherent target
contract rather than mixed with Windows metadata.

The two-case differential JSON SHA-256 is
`c19ee2ebe2230af3119d455d250500c621a51405f963d51c361964a1dc4b30ea`.
Exact tokens and records are retained in
[`chrome9-texldp-texldb-corpus.tsv`](chrome9-texldp-texldb-corpus.tsv).

## Silicon gates

TEXLDP must first use a small texture with distinct texels and homogeneous
coordinates chosen so division by Q selects a different texel from the raw
S/T coordinates.  Include Q=1 as the ordinary-sample control, then a finite
positive Q such as 2.  Reject Q=0, negative Q, NaN/Inf, source swizzles,
offsets, and noncanonical sampler/destination ids in the first gate.

TEXLDB requires an independently proven two-level mip layout.  Use solid,
different colors for L0/L1, a footprint that otherwise selects L0, and compare
bias 0 against a conservative positive bias selecting L1.  The test must
separate TAU operation 3 from ordinary operation 1 and verify the actual bias
lane, sign, and unit; compiler opcode naming is insufficient.

Each live test must use command 19's mandatory fresh D16, depth clear 1,
LESS/write, passing z=0.5, guarded destination and texture BOs, timeout/reset,
and readback.  Until the dedicated projective and mip-bias matrices pass,
Mesa must reject projective/bias NIR shapes that cannot be reduced to already
proven ordinary coordinates/sampling, and the kernel must reject these native
TAU operation keys.
