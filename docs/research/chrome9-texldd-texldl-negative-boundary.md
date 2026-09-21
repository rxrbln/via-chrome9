# Chrome9 TEXLDD/TEXLDL negative boundary

The recovered production compilers do not emit a TAU instruction for explicit
gradient TEXLDD or explicit-LOD TEXLDL.  PS 2.0 and PS 3.0 framing produce the
same result.  TEXLDL is silently dropped, leaving exactly the trailing
uninitialized-r0 MOV baseline.  TEXLDD desynchronizes the two frontend parsers
into different, non-output ALU records.  Neither is executable texture
support, and both operations remain strictly rejected.

## Differential and baseline

Each explicit texture token writes r0 and is followed by `MOV oC0,r0`.  A
standalone `MOV oC0,r0` baseline produces:

```
Windows 01c00530 001c0053 00f00000 000d0820
Linux   01c00530 c01c0053 00f200b6 000d0820
```

TEXLDL under both shader versions produces exactly those respective baseline
ALUs and no TAU.  Windows reports texture mask 0/temp mask 0; Linux reports
texture mask 0/temp mask 1.  Therefore opcode `0x5f` was not translated—the
observable program merely reads an uninitialized temporary.

TEXLDD under both versions also produces no TAU, but the parser residues are:

```
Windows 0140b6d0 09140924 00400100 001004a0
Linux   01c00530 531c0053 00403fb4 200d0420
```

Windows reports color-output count 0 and both resource masks zero.  Linux
reports texture mask 4 and temporary mask 2.  The streams disagree in opcode,
operands, and destination class and do not contain an oC0 store.  This is a
cross-driver token-walk/desynchronization failure, not an alternative TEXLDD
encoding.  Merely checking compiler return status or a nonzero ALU count would
accept corrupt output.

The PS 2.0 differential JSON SHA-256 is
`7bb620e9ffb82ade5a5b3b9c50e4665d0bbe65eb890758318e24fe4f4be4bcee`;
the PS 3.0-framed SHA-256 is
`466a80507e8e06bfaec71d6dfe757b3bc9488ee5f3d3a0b3dbf2031e8ccecd4e`;
the baseline SHA-256 is
`68e575bf6bd232a19e7bc62f14734c76396ecd766c42e9273e094c6253dfb405`.
Exact tokens, metadata, records, and controls are retained in
[`chrome9-texldd-texldl-negative-corpus.tsv`](chrome9-texldd-texldl-negative-corpus.tsv).

## Feature boundary

Reject NIR explicit-gradient and per-fragment explicit-LOD texture operations,
and do not advertise shader features which require them.  PS 3.0 version
retention does not change this result.  A compiler must require the expected
TAU count, output store, resource masks, and normalized controls; successful
return plus an arbitrary ALU is insufficient.

This boundary does not revoke ordinary implicit TEXLD.  Ordinary sampling has
a separately recovered TAU operation 1 and silicon/runtime gates.  Likewise,
the compiler-exact TEXLDP operation 7 and TEXLDB operation 3 remain separate
pending-silicon candidates.  A constant LOD might eventually be implemented
through independently proven sampler min/max state, but that cannot implement
a general per-fragment NIR `textureLod`, and implicit sampler derivatives do
not expose programmable gradients.

There is no valid TEXLDD/TEXLDL command stream for a silicon oracle.  If one is
later recovered from another driver path, its first execution must use command
19's mandatory fresh D16, depth clear 1, LESS/write, passing z=0.5, guarded
texture/destination BOs, timeout/reset, and readback.  Until then both native
keys are fail-closed.
