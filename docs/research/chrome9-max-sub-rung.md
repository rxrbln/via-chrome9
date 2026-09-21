# Chrome9 guarded MAX and SUB one-record rungs

## Result and promotion order

MAX is the direct successor to MIN.  Its first exact key is:

```text
MAX oC0,v0,c0  01c00530 30080053 00f04005 000b0820
A90-A9b        90058000 91000000 92000001 93000000
               94000000 95000000 96000000 97000000
               98000000 99000000 9a000000 9b000000
```

It reuses MIN's A/C placement and cannot create infinity or NaN from finite
v0/c0.  SUB is later because it adds the B slot and canonical NEG_ONE C
literal:

```text
SUB oC0,v0,c0  80400530 30140924 00f08005 00000820
```

The safest global promotion order is MIN, MAX, ADD, then SUB.  MIN/MAX first
exercise selection with A/C only.  ADD/SUB then introduce B plus a literal C
and require an arithmetic constant-domain gate.  CMP, DP3/DP4, RCP/RSQ,
partial masks, SAT, and multi-record programs remain rejected.

The additional compiler results are in
[`chrome9-max-sub-rung-corpus.tsv`](chrome9-max-sub-rung-corpus.tsv).

## Compiler and modifier boundary

Windows and Linux byte-match MAX and SUB identity records and the tested
`.aaaa` placement in either source.  MAX also byte-matches NEG in either A or
C with d3.bit20 clear.  These forms remain reject-only because numerical
modifier behavior has not passed silicon.

SUB places source 0 in B, source 1 in A, and canonical NEG_ONE in C, producing
`B + A*(-1)`.  Its `.aaaa` records independently close both user-source
packers:

```text
SUB v0.aaaa,c0  80406db0 30140924 00f08005 00000820
SUB v0,c0.aaaa  80400530 b0140924 00f0806d 00000820
```

SUB user NEG is not cross-driver stable.  Windows sets d3.bit20 for a
modifier on B or A, while Linux clears it; the other three dwords match.
Canonical NEG_ONE in C does not set d3.bit20 in the identity record.  This is
another proof that bit 20 is a frontend/path companion, not a generic rule.
Reject every SUB source modifier.

## First MAX validator and live oracle

Admit only one terminal `nir_op_fmax` whose source 0 is exactly v0 and source
1 exactly c0: FP32, identity XYZW, NONE source modifiers, full XYZW oC0,
no SAT/scale, no temporary/texture/discard/depth/control flow, finite c0, and
default FP32 controls.  The kernel owns the exact one-record object and
twelve control words.  Reject source reversal and all compiler-proven
swizzles/modifiers until separate keys pass.

Use the same guarded D16/RHW/BGRA8888/COPY envelope and flat primary logical
RGBA `(32,96,160,224)/255` as the MIN gate.  Run known-good before and after
each candidate and check guards, fences, error state, and reset counts.

| c0 logical RGBA | expected logical RGBA bytes | purpose |
| --- | --- | --- |
| `(0.5,0,1,0.5)` | `(128,96,255,224)` | alternates the winning source by lane and distinguishes MAX from either MOV |
| `(-1,2,-0,0.5)` | `(32,255,160,224)` | negative, high clamp, and signed-zero safety without sign claim |
| `(FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX)` | `(255,255,255,255)` | positive endpoint selects c0 and clamps |
| `(-FLT_MAX,-FLT_MAX,-FLT_MAX,-FLT_MAX)` | `(32,96,160,224)` | negative endpoint selects v0 |

Require exact converted BGRA pixels and closure for every row, then 1, 64,
and 1024 repetitions of the alternating-source row.  A later source-order
key and denormal alpha-test oracle remain separate.

## First SUB validator and constant domain

Admit only terminal `nir_op_fsub` with source 0 exactly v0 and source 1
exactly c0, under the same identity/full-write/default-state restrictions.
SUB is noncommutative: `c0-v0` is a different key and stays rejected.

For rounding-independent finite closure, require each lane:

```text
magnitude = c0[i] & 0x7fffffff
magnitude <= 0x7effffff
```

This symmetric FLT_MAX/2 bound is conservative and automatically rejects
NaN/infinity.  With v0 in `[0,1]`, `v0-c0` then remains far inside the finite
range under every IEEE rounding direction.  A mathematically wider
sign-dependent domain is possible, but it is not worth expanding the first
kernel key before silicon establishes the fixed rounding behavior.

The decisive flat-color rows are:

| c0 logical RGBA | expected logical RGBA bytes | purpose |
| --- | --- | --- |
| `(0,0.25,-0.25,0.25)` | `(32,32,224,160)` | lane arithmetic and subtraction direction |
| `(1,-0.25,1,-0.25)` | `(0,160,0,255)` | negative result clamp and positive overflow-to-target clamp |
| `(FLT_MAX/2,FLT_MAX/2,FLT_MAX/2,FLT_MAX/2)` | `(0,0,0,0)` | positive bounded endpoint |
| `(-FLT_MAX/2,-FLT_MAX/2,-FLT_MAX/2,-FLT_MAX/2)` | `(255,255,255,255)` | negative bounded endpoint |

Use the same known-good/candidate/known-good closure and repetition gates as
MIN/MAX.  Normalized readback cannot establish signed-zero, denormal, or exact
rounding behavior; keep explicit non-default NIR FP modes and all expanded
constant domains rejected.
