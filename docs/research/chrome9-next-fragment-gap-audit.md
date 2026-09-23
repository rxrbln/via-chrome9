# Chrome9 next fragment-feature gap audit

Status: implementation-oriented audit against Mesa nested HEAD `7d0d27b` and
kernel Chrome9 nested HEAD `ac649e5`, 2026-09-23.  This is a generic feature
inventory, not an application or scene allowlist.

The current path is materially beyond the original one-record bring-up.  Mesa
can now lower an ordered straight-line NIR data-flow graph to semantic ALU/TEX
records, including TEMP-generated texture coordinates.  The kernel validates,
allocates r0--r11, constructs ALU/TAU transition maps, packs varying widths,
and emits mip/LOD state.  The remaining work below is therefore ranked by
missing semantics or unreachable state, not by whether a scene name has been
recognized.

## Ranked implementation blockers

### P0: make the existing generic path the trusted execution baseline

Before another opcode is admitted, the live generic 2-D texture program must
retire with the packed varying fix and byte-match its production oracle.  Its
kernel packet must remain one coherent object: FVF/VS linkage, A90--A9b,
TAU/ALU words, constants, texture state, vertices, H2 boundary, and cookie.
Every later feature uses that same path; a timeout here is not a missing GL
feature and must not be worked around in Mesa.

Current structural checks are good but incomplete: `chrome9_fragment_selftest`
byte-compares the semantic compiler, while `chrome9_fragment_raster_selftest`
checks packed widths separately.  Add a whole-command selftest for at least a
T0.xyz sample/consumer object so the linkage and shader halves cannot regress
independently.

### P1: finite lighting arithmetic, normalize, reflect, and power

Mesa already lowers the arithmetic operations needed for a general finite
lighting DAG: DP3/DP4, RSQ, MUL, ADD/SUB/MAD, MIN/MAX, LOG2, and EXP2.  The
kernel already has templates and liveness allocation for those operations.
The missing step is live numerical qualification and complete-object tests,
not another program classifier.

The production normalize/lighting oracle is retained in
`chrome9-glmark-lighting-production-corpus.tsv`.  Its reusable core is six
ALUs, active tuple `(1,5,1,0)`, and controls beginning
`90048001 91000000 92000006 93000000`.  It covers
`DP3 -> RSQ -> vector MUL -> DP3 -> MAX -> MOV` with five fresh temporaries.
The standalone vendor NRM opcode expands to four records
`DP3, subop9, RSQ, MUL`; the generic backend should keep explicit operations
and prove their finite semantics rather than adding an opaque NRM UAPI opcode.

A fresh production differential now closes a compact generic reflect object:

```text
# DP3 r0.x,T0,c0; ADD r1.x,r0.x,r0.x; MAD oC0,-r1.x,c0,T0
Linux configuration 2:
01c00530 50080055 0010c005 00020020
000016d0 d0140924 00120016 00000060
02c00530 08080053 00f20100 00000820

Windows configuration 2:
01c00530 50080055 0010c005 00020020
000016d0 d0140924 00120016 00000060
02c00530 08080053 00f20100 00100820
```

The active tuple is `(1,2,1,0)` and the Linux controls are
`90058001 91000000 92000003` followed by zero A93--A9b.  The only cross-driver
delta is final dword3 bit 20 for the modified-source MAD.  The kernel must keep
the Linux configuration-2 form; mixing the Windows modifier mirror into Linux
records is not allowed.  Exact tokens and hashes are in
`chrome9-reflect-production-corpus.tsv`.

Current `fpow` lowering emits scalar LOG2, MUL, EXP2 and is structurally
correct for positive finite bases.  That is not yet sufficient for glmark-like
lighting, where `pow(max(dot,0), positive_integer)` legitimately reaches base
zero.  The first conformant promotion is generic constant-positive-integer
exponentiation by squaring/addition chain, selected from the NIR constant and
its exactness flags.  The existing 20/30/100 corpora use 5/7/8 temporaries and
6/8/9 ALUs including the terminal move, all below r0--r11 and 96 ALUs.  Keep
LOG-MUL-EXP for the general positive finite domain only after zero, denormal,
rounding, and precision are measured on silicon.

Required live vectors are finite unit axes, `(0.6,0.8,0)`, parallel and
antiparallel reflect inputs, and power bases `0, .25, .5, 1` with exponents
`1,2,20`.  Read float-sensitive intermediates through multiple color lanes;
an UNORM terminal alone cannot prove scalar broadcast.

### P1: discard is a separate side effect, not CMP

The generic compiler currently rejects every discard/demote/terminate
intrinsic.  Older ioquake-specific matchers count exact `terminate_if`
shapes, but they are compatibility paths and must not become the generic
implementation.

The native TEXKILL schedule is exact:

```text
ALU 01c00530 301c0053 00f20005 000d0420
TAU 314001b1
```

For `MOV r0,c0; TEXKILL r0; MOV oC0,v0`, Linux controls are:

```text
90058000 91000001 92010003 93000001 94000000 95000002
```

Add a dedicated semantic KILL instruction only after a guarded live test
establishes which negative lanes kill, the zero/NaN boundary, and suppression
of both color and depth.  Its source must participate in normal SSA liveness,
but KILL itself has no ordinary destination and must retain program order.
Do not lower `discard_if` to CMP: CMP selects values and has no fragment side
effect.  Alpha-test A33 remains a legal independent fixed-function lowering
only for conditions exactly representable by its GL comparison state.

### P1: expose the texture formats that the ABI already names

The kernel semantic ABI and validator already name A8 and X8Z24, and derive
A30 `1b` and `fe` respectively.  Mesa resource creation and the generic draw
builder currently make only BGRX/BGRA reachable; the builder's two-way format
choice would classify any newly admitted resource as BGRA.  Therefore A8 and
X8Z24 must be added table-first across layout cpp, legal bind roles, view
swizzle, clear/readback, and draw conversion before their caps change.

Implementation order:

1. A8 sample-only: cpp 1, A30=`301b0000`, logical `(0,0,0,A)`, no RT bind.
2. X8Z24 depth render/sample: cpp 4, A12 format code 6, raw sample
   A30=`30fe2000`, nearest/no-mip first, with D24 in bits 31:8.
3. L8, L8A8, and RGB565 sampler formats from the exact static table; keep A8
   and L8 non-renderable and gate each RT mapping independently.
4. S3TC, 3-D, and additional packed formats only after their own layout and
   filter oracles.  Format-table presence is not a renderability proof.

X8Z24 is required for a general depth-texture path.  D16 must never be exposed
as sampleable: both recovered vendor UMDs reject it in sampler lookup.

### P2: projected/bias sampling and the actual LOD boundary

Ordinary implicit 2-D TEX is implemented.  The production compiler also gives
exact terminal TAU operations for TEXLDP and TEXLDB:

```text
TEXLDP 307001b0   # operation nibble 7, Linux configuration 2
TEXLDB 303001b0   # operation nibble 3, Linux configuration 2
```

The UAPI currently has only one TEX semantic, so Mesa cannot preserve `txp`
or `txb`.  Add explicit semantic operations only after projective-Q and signed
bias/mip live matrices pass.  TEXLDL and TEXLDD have no usable native output
in either production compiler and remain unsupported.

The current sampler contract is narrower than its field names suggest:

- Mesa rejects nonzero `min_lod` when creating sampler state;
- the kernel rejects fractional min/max LOD (`s5_5 & 31`);
- first-level views are rejected by the generic draw builder;
- the view must end at the resource's last level;
- mip offsets cover one 2-D packed-linear chain only.

Signed fractional LOD bias is carried.  Implicit derivative-driven mip choice
is a texture-unit feature and does not imply programmable derivatives.
DSX/DSY compile to zero records in both recovered compilers; `fddx/fddy` must
remain rejected.  Likewise explicit-gradient and per-fragment explicit-LOD
NIR cannot be advertised.

### P2: cube and remaining targets need a new image-state contract

The recovered cube image state is exact, including six A00--A05 face bases,
A18 shape bits, shared mip table, STRQ coordinate linkage, and cube/TEXLDP
operation 7.  The current UAPI texture state has one handle/base and a 2-D
S/T wrap tuple, so cube cannot be represented correctly by pretending that
six faces are one 2-D allocation.

Use a versioned target-aware successor with six validated face roots (or one
BO plus six validated offsets), identical face dimensions/formats/level
layout, explicit R wrap/STRQ coordinates, and complete transition cleanup back
to 2-D.  Rectangle can share 2-D image storage but needs its unnormalized
coordinate contract.  3-D needs independently recovered slice/depth layout;
neither target should be inferred from extension strings.

## Current-code mismatch checklist

| Area | Mesa today | Kernel today | Required change |
|---|---|---|---|
| discard | generic straight-line scan rejects terminate/discard | no KILL semantic | ordered side-effect IR, exact TEXKILL schedule, live lane/depth oracle |
| normalize | explicit DP3/RSQ/MUL can lower | encoders/allocation exist | finite numerical and full-object live tests |
| reflect | primitive ops can lower; contraction depends on NIR | Linux modifier convention exists | add corpus test and finite numerical test; never import Windows bit20 |
| pow | LOG/MUL/EXP for scalar general form | three scalar records supported | integer-constant generic lowering for base zero; general-domain silicon gate |
| derivatives | rejected | no valid native opcode | keep rejected |
| TXP/TXB | only `nir_texop_tex` accepted | only TEX semantic | new semantic op after Q/bias live proof |
| TXL/TXD | rejected | no valid compiler output | keep rejected |
| A8/X8Z24 | resource/view path cannot create or select them | enums and A30 mappings exist | table-driven Mesa layout/binds/swizzle/clear and guarded tests |
| cube | 2-D only | one-base 2-D state | versioned six-face/STRQ ABI |
| LOD clamp | min LOD forced zero; full-chain views | whole-level min/max only | characterize fractional clamp and first/last-level addressing |

## Promotion tests

Every promoted row needs three proofs: an offline byte test, a guarded raw
kernel test with timeout/reset/error counters, and a Gallium/API test that
exercises the same generic state.  Required negative tests include unused or
undefined TEMP lanes, sampler/constant/varying high-water mismatches, more
than 12 simultaneously live temporaries, ALU 97, TAU 33, sampler id 8,
non-finite constants, unsupported target/format binds, and any residual NIR
control-flow or derivative instruction.

The recovered hardware ceilings remain 96 ALU, 32 TAU, eight API texture
units, 12 physical temporary registers for the initial allocator, 2048 2-D
dimension, and 12 mip levels.  They are validator/compiler limits, not claims
that every associated GL capability is already conformant.
