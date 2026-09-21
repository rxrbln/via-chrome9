# Chrome9 production three-factor two-texture object

Status: exact hardware-free Linux production compiler/finalizer differential,
2026-09-21.  This is a compiler golden for a future Mesa backend.  It has not
executed on Chrome9 silicon and does not replace the complete fixed-Xorg
three-factor object in an existing packet.

## Exact semantic program

The probe supplied this PS 2.0 program to the Linux 5.75 compiler with its
production chip-5 parser/validator/processor callback order:

```text
texld r0,t0,s0
texld r1,t1,s1
mul   r2,r0,r1
mul   r3,r2,v0
mov   oC0,r3
```

The exact token body, raw words, finalized words and controls are retained in
[`chrome9-production-three-factor-corpus.tsv`](chrome9-production-three-factor-corpus.tsv).

## Raw and finalized objects

Before configuration-two post-translation the compiler emits:

```text
TAU 201001b0 211111b1
ALU 0140b6d0 30001053 00f00005 000000a0
    0140b6d0 30040053 00f00205 000000e0
    01c00530 301c0053 00f00305 000d0820
```

After `stmPPS_PostTranslationSetting_invi(..., 2)` the indivisible production
object is:

```text
A90-A95 90010000 91000003 92020003 93000002 94000000 95000000
TAU     301001b0 311111b1
ALU     0140b6d0 30201053 00f20005 000000a0
        0140b6d0 30040053 00f20205 000000e0
        01c00530 301c0053 00f20305 000d0820
resources 2 4 2 2
```

The raw/final ALU SHA-256 values are respectively
`f071eba4521eefaf28545142237752650971400ddb679067d4d61ca5029ad95f` and
`48dc5a025de82339e284b4056c650ed0a25769ddee8631fdb44295ce725f1d00`.
The raw/final TAU hashes are respectively
`503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f` and
`610e547dd5793a3b71f7c59c4ae8c5591a6ec703942b766bf323f8644c813a05`.

The final-use placements are internally consistent:

- both sampled values die in the first MUL, setting C.bit21 and A.bit17;
- r2 dies in the second MUL, setting A.bit17 there; and
- r3 dies in the final MOV, setting A.bit17 there.

The destination ids are r2=`d3[9:6]=2` (`000000a0`) and
r3=`d3[9:6]=3` (`000000e0`); oC0 remains class 2/id 0 (`000d0820`).  The
register-manager remap trace is `0 1 2 0 1 3 2 3`.  No special/source-63
allocation participates.

## A90 discriminator

The finalized compiler metadata has `object+2c=00010000`, while the simpler
two-independent-TEX plus one direct MUL object has `object+2c=00018000`.
Therefore the former is A90=`90010000` and the latter is A90=`90018000`, even
though both begin with two TAUs and end in ALU.  A91 is `object+70=3`; A92 is
`(2 << 16) | 3`; A93 is the T2A map `2`.

This is direct evidence that `(tau_count, alu_count, start_phase)` is not a
sufficient general control encoder.  The backend must select a closed control
template or reproduce all finalizer metadata; it must never infer A90 by
blindly copying the one-ALU two-texture tuple.

## Fixed-object separation and runtime gate

The familiar fixed-Xorg schedule uses different controls, TAUs, allocation
and terminal-use fields.  Its complete bytes remain useful silicon evidence,
but are not production allocator output.  Never combine its `301211b1`,
special-source ids or `0000007f/000d083f` destinations with this r0--r3
family.

The first hardware gate for the production object must compare it against the
already bounded fixed object using asymmetric texture and primary colors,
with guarded sources/target, exact pixel checks, retirement/status capture,
and a known-good draw before and after.  Until that passes, use this object
only as a compiler and offline-verifier golden.

Evidence used `tools/re/chrome9_pps_probe.c` with the production callbacks,
configuration two, register-manager trace and resource-count query.  The
Linux DRI SHA-256 is
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
