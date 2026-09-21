# Chrome9 Mesa fragment encoder v1

Status: implementation contract for replacing the current closed-key table
with a bounded native encoder. This is a small straight-line compiler, not a
claim of complete GLSL or PS 2.0 support.

## Existing state and target

Mesa currently accepts NIR, canonicalizes it to a small expression key, and
selects byte-exact records in `chrome9_shader_offline.c`. It is a hardcoded
program matcher, not a NIR-to-Chrome9 compiler. The first real encoder should
produce the existing `struct chrome9_ps_program`: 12 A90--A9b controls, up to
96 four-dword ALU records, up to 32 one-dword TAU records, and exact resource
masks. Unsupported shapes remain rejected.

The smallest subset covering ordinary current matches is:

```text
SSA operations: mov, fmul
sources:        r0..r11, c0..c31, v0/v1, tex results rN
swizzles:       xyzw and aaaa
destinations:   r0..r11, oC0; xyzw plus proven RGB/X partial writes
texture:        ordinary 2-D TEXLD, t0/s0 and t1/s1
schedules:      ALU only; 1 TAU -> ALU; 2 independent TAUs -> ALU
modifiers:      source NONE/NEG, destination NONE/SAT
```

ADD, SUB-as-ADD+NEG, and MAD are the next contained rung. All have exact
one-record compiler encodings but need separate numerical silicon gates.
Three-factor multiplication needs two MUL records. RGBX output uses the
proven two partial MOV records and a driver-owned constant-one lane.

## Program object and scheduling

A pixel ALU is four little-endian dwords and a TAU is one dword. They upload
under H3 selectors `20030000` and `21030000`. There is no END record. A92
carries both lengths:

```text
A92 = 92000000 | (tau_count << 16) | alu_count
```

Termination is exhausted count plus final phase. Hard limits are 96 ALUs, 32
TAUs, and 128 normalized inputs before expansion. Preflight expanded counts.
The following are closed tuples, not formulas for arbitrary programs. The
ALU-only form is reproduced for one through three ordinary records. The textured
forms are proven only at the stated count and shape:

```text
# one to three proven ordinary ALUs, no TAU
90058000 91000000 (92000000|alu_count) 93000000 94000000 95000000
96000000 97000000 98000000 99000000 9a000000 9b000000

# one leading TAU, then one or two proven ALUs
90018000 91000001 (92010000|alu_count) 93000001 94000000 95000000
96000000 97000000 98000000 99000000 9a000000 9b000000

# two independent TAUs, then one direct-output ALU
90018000 91000003 92020001 93000002 94000000 95000000
96000000 97000000 98000000 99000000 9a000000 9b000000
```

A9b is kernel-owned. Never accept controls from NIR or untrusted userspace.
Unused records, transition bits, and packet slack are zero/canonical.

## Operand encoder

Use `{class,id,swizzle[4],modifier}` and validate the logical class before
packing. Proven PS source classes are TEMP=0, INPUT=1, CONST=2, TEXTURE=3,
SPECIAL/LITERAL=5. A six-bit native id is not permission for 64 registers.

Selectors are X=0, Y=1, Z=2, W=3, ONE=4, ZERO=5; reject 6/7. Modifier bits
are BIAS=1, COMP=2, X2=4, NEG=8; v1 allows only NONE/NEG.

| slot | id/class | swizzle X,Y,Z,W | modifier |
|---|---|---|---|
| B | d0 `21:16` / `24:22` | d0 `15:13,12:10,9:7,6:4` | d0 `3:0` |
| C | d1 `17:12` / `20:18` | d1 `11:9,8:6,5:3,2:0` | d0 `31:28`, d3.bit20 if nonzero |
| A | d2 `13:8` / `16:14` | d2 `7:5,4:2,1:0` plus d1.bit31 for Z; W=d1 `30:28` | d1 `27:24` |

Destination writemask is d2 `23:20`, id d3 `9:6`, class d3 `11:10`, SAT
d3.bit12, and scale d3 `15:13`. V1 allows oC0 or r0--r11 and zero scale.

The destination id is only four native bits.  With twelve ordinary temporaries
simultaneously live, Linux production output proves the complete linear map
`r0=...0020`, `r1=...0060`, through `r11=...02e0`; requested `r12` and `r15`
encode `...0320` and `...03e0`.  Both Windows and Linux accept requested r16
but its destination aliases `...0020` while a later source still carries id
16.  That is an unsafe frontend wrap, not a seventeenth physical temporary.
The first Mesa allocator therefore remains capped at r0--r11; a later r12--r15
promotion needs its own live gate.  Logical ids at or above 16 are always
rejected before packing.

Do not assume that a requested logical id is the final physical id.  The Linux
production register manager compacts a lone logical r1 or r11 to physical r0,
whereas the twelve-simultaneously-live case preserves ids 0--11.  Allocation
and liveness must precede encoding.  The exact raw/configuration-2 corpus is
SHA-256
`9e28634dc951fce310e62494363317ed2437dc990e5cdc6daf93623bbfa292bf`.
Windows destination-field controls are retained by SHA-256
`25f6c24c2a471d35c956f256491e82c7529af8e5fab35fc9abe0d2e00ffc9af2`
and
`08e4c407aa09cc6fcaf3aa43e2e26b996c5451b8987b20e9d32a7a58181b715a`.

Start from the independently recovered neutral operation template and encode
only named slots. D3 `19:16` is a sub-operation selector, not a complete
opcode, so compare all four dwords in tests.

| operation | exact parallel-slot assignment | neutral compiler record |
|---|---|---|
| MOV | A=src | `01c00530 301c0053 00f08005 000d0020` |
| ADD | A=src0, B=src1, C=ONE | `00810530 30140924 00f08005 00000020` |
| SUB | B=src0, A=src1, C=NEG_ONE | `80800530 30140924 00f08105 00000020` |
| MUL | A=src0, C=src1, B=ZERO | `0140b6d0 30081053 00f08005 00000020` |
| MAD | A=src0, C=src1, B=src2 | `00820530 30081053 00f08005 00000020` |

Those examples target r0 and use c0/c1/c2. Encoding oC0 changes d3 to an
`...0820` destination. MUL's `0140b6d0` and ADD's literal-one fields are
functional parallel-ALU sources, not padding.

The non-obvious SUB assignment is deliberate: the hardware parallel form is
`B + A*C`. With B=src0, A=src1 and C=-1 it implements src0-src1. Recasting
SUB as an ordinary ADD with an arbitrary negated operand would encode the
wrong slots.

Finalized `oC0` differentials extend the one-record table:

| operation | exact finalized record for c0[,c1[,c2]] -> oC0 |
|---|---|
| DP3 | `01c00530 50081055 00f08005 00020820` |
| DP4 | `01c00530 30081053 00f08005 00030820` |
| MIN | `01c00530 30081053 00f08005 000c0820` |
| MAX | `01c00530 30081053 00f08005 000b0820` |
| RCP | `01c00530 b01c0053 00f080b6 00060820` |
| RSQ | `01c00530 b01c0053 00f080b6 00070820` |
| CMP | `00820530 30081053 00f08005 00090820` |

DP3/DP4/MIN/MAX use A=src0 and C=src1. RCP/RSQ use A=src. CMP uses
A=condition src0, C=true src1 and B=false src2. These assignments come from
complete output records, not temporary-only opcode inference.

## Texture phase and temporary-lifetime finalization

Raw ordinary compiler TEXLD words are:

```text
201001b0  # r0 = TEX(t0,s0)
211111b1  # r1 = TEX(t1,s1)
```

TEXLDP=`207001b0` and TEXLDB=`203001b0` remain excluded pending runtime
semantics. Validate sampler id before packing; unchecked s16 collides with a
valid finalized s0 word.

Configuration-2 finalization owns TAU phase fields and ALU temporary-lifetime
fields. One leading sample becomes TAU `301001b0` and a final source-A use gets
d2.bit17. Two independent
samples followed by a consuming ALU use:

```text
TAU 301001b0 311111b1
ALU 0140b6d0 30201053 00f20005 00000820
```

High TAU bits depend on phase order. Never implement `tau |= 10000000`.
Until a liveness/phase finalizer byte-matches the corpus, emit only the exact
shapes and bounded counts above.

### Production callback selection and multi-ALU allocation

The offline wrapper must use the same callback setup as
`stmCreatePixelShader_inv`. For PS 2.0 and chip id 5 the exact sequence is:

```text
stmPPSSetFunctionsPS20_invi(context, instance)  # void cdecl(void *, void *)
stmPPSSetRegManager_invi(context, instance)     # void cdecl(void *, void *)
stmPPSCompile_invi(context, input, instance)    # int cdecl(void *, void *, void *)
```

Both setup calls read `context+0x240 -> +0x0c -> chip_id`. The production
function-selector table is `+137c=0x1b6e10` token parser,
`+1380=0x1b5870` validator, and `+1384=0x1b8b80` processor/emitter. An older
probe manually installed the processor at `+1380` and validator at `+1384`.
That swap compiled simple records by accident but did not initialize normal
multi-record allocation.

With the production selectors, configuration-2 compilation of

```text
texld r0,t0,s0
add   r1,r0,c0
mul   oC0,r1,c1
```

produces an ordinary r0/r1 chain:

```text
A90..A9b 90018000 91000001 92010002 93000001 94000000 95000000
          96000000 97000000 98000000 99000000 9a000000 9b000000
TAU       301001b0
ALU       00800530 30140924 00f20005 00000060
          0140b6d0 30081053 00f20105 00000820
```

The equivalent first SUB is
`82000530 30140924 00f08005 00000060`. Allocator tracing and
`stmGetActiveTexTmpNum` report tuple `(1,2,1,1)`, with logical r0/r1 mapped to
physical 0/1. This makes the two-ALU textured chain valid compiler evidence;
it is still not silicon proof for every possible chain.

Bit d2.17 is not texture-specific. The ALU-only chain
`add r0,c0,c1; mul oC0,r0,c2` emits its consumer as
`0140b6d0 30082053 00f20005 00000820`; it marks the final use of the temporary
in source A.  It is not a producer-edge bit.  An undefined `mov oC0,r0` and
`mov oC0,t0` receive the same d2.bit17 because TEMP/TEXTURE sources are
eligible and that occurrence is their final use, even though neither has a
producer.  Conversely, a non-final temporary read does not receive the bit.  A
correct finalizer must compute last use after physical allocation, not set
fields merely because an SSA producer exists.

The raw/final differential separates scheduler allocation from lifetime/phase
finalization. Before the explicit configuration-2 pass, the production
textured chain already contains ordinary destination/source id 1:

```text
raw ALU0   00800530 30140924 00f00005 00000060
raw ALU1   0140b6d0 30081053 00f00105 00000820
raw TAU    201001b0
raw A90    90000000

config2 ALU0 00800530 30140924 00f20005 00000060
config2 ALU1 0140b6d0 30081053 00f20105 00000820
config2 TAU  301001b0
config2 A90  90018000
```

Configuration-2 adds source final-use bits to both records, changes TAU bit28,
and updates A90. The historical destination-id-15/source-id-63 object is retained
in the corpus only as `NONPROD_CALLBACK_SWAP_*`: it is reproduced by swapping
the validator and processor callback slots and is not hardware evidence.

The H5 register manager exposes 12 allocatable ordinary slots plus four high
native ids.  The high r12--r15 ids encode without the historical id-63 miss,
but are not part of the first Mesa allocation contract.  The vendor frontend
nevertheless accepts r16 with the asymmetric wrap described above; Mesa must
reject it rather than rely on truncation.

The Windows backend before configuration-2 and the Linux production
finalizer give exact one-texture final-use deltas for every v1 source slot:

| operation | Windows scheduled ALU | Linux configuration-2 ALU |
|---|---|---|
| ADD(tex0,c0) | `00800530 00140924 00f00000 00000820` | `00800530 30140924 00f20005 00000820` |
| SUB(tex0,c0) | `80000000 30140924 00f08005 00000820` | `82000530 30140924 00f08005 00000820` |
| MAD(tex0,c0,c1) | `00810530 00080053 00f00000 00000820` | `00810530 30080053 00f20005 00000820` |
| DP3(tex0,c0) | `01c00530 50080055 00f00000 00020820` | `01c00530 50080055 00f20005 00020820` |
| RCP(tex0) | `01c00530 801c0053 00f000b6 00060820` | `01c00530 b01c0053 00f200b6 00060820` |
| CMP(tex0,c0,c1) | `00810530 00080053 00f00000 00090820` | `00810530 30080053 00f20005 00090820` |

DP4, MIN, MAX and RSQ follow the same slot-specific delta as DP3 or RCP.
SUB is the important exception: because the sampled value is in parallel
source B, finalization changes d0 rather than d2.  The exact per-slot final-use
bits are B=d0.bit25, C=d1.bit21, and A=d2.bit17.  Direct undefined TEMP and
TEXTURE source probes receive those same class-driven bits, proving they are
not producer-edge or texture-dependency flags.  If a physical temporary is
read more than once, only its last read is marked; if it occurs in multiple
slots of one record, the compiler emits one release marker rather than one per
occurrence, with exact precedence A over C over B.  A+C, A+B, and A+C+B set
only d2.bit17; C+B sets only d1.bit21.  The result is identical with an actual
r0 producer and with an undefined direct r0 read.  The precedence corpus is
SHA-256
`8c0cc9d6ccb09c46ba83b550c28d62aab02478559e90fe3035b3f48abc50937a`.
Therefore a generic "sampled operand means d2.bit17" rule is wrong, as is
setting every source occurrence from the SSA graph.  Mesa commit `e34de89`
now implements the bounded semantic form correctly: its validation pass counts
uses after physical source placement and its second pass marks only the last
read, including A-over-C precedence when the same temporary occupies both
slots.  New opcodes must first map logical operands into their actual A/C/B
slots; applying the marker from the NIR source ordinal remains invalid.

The Windows direct-slot/raw and two-record artifacts are SHA-256
`ef0645552e914325a353ccb740f3715a9917dea3da144569055f354d150108bd`
and
`0edc7b2d153b15ed0666e0e7005379a1214a8c000166a7d1e112b6ea2fb536fc`.
That wrapper establishes raw field placement but does not run the Linux
configuration-2 lifetime pass; the Linux production differential above is
authoritative for the three final-use bits.

## Fail-closed NIR mapping

Native compiler evidence and a valid NIR mapping are separate gates.  The
following is the patch boundary; “encoded” does not mean advertised before its
listed silicon oracle succeeds.

| NIR shape | native mapping | initial status |
|---|---|---|
| `mov(vec4)` | MOV, source A | implemented; identity/aaaa leaf only |
| `fmul(vec4,vec4)` | MUL, A=src0, C=src1 | implemented; cmd19 only exact v0/c0 identity pair |
| `fadd` | ADD, A=src0, B=src1, C=literal ONE | proven bytes; disabled until source-B encoder and numerical gate |
| `fsub` | SUB, B=src0, A=src1, C=literal NEG_ONE | proven bytes; never lower as generic ADD+NEG |
| `fmin` / `fmax` | MIN/MAX, A=src0, C=src1 | proven bytes; MIN(v0,c0) is the safest first successor |
| scalar `fdot3`/`fdot4` replicated to all four store lanes | DP3/DP4, A/C | proven bytes; reject scalar/partial-store shortcuts |
| `ffract(vec4)` | FRC, source A | proven bytes; guarded silicon gate pending |
| scalar `frcp`, `frsq`, `fexp2`, `flog2` broadcast to XYZW | scalar subop, dedicated A=000W form | proven bytes; initially c0.w only and operation-specific finite domain |
| `fsat` | destination SAT bit | compiler bytes proven; reject until lower and upper pre-RT alpha oracles pass |
| terminal `fabs(c0)` | CPU-clear c0 sign bits then ordinary MOV | no native ABS admission needed; native ABS remains future work |
| `bcsel` | not general CMP | reject; NIR boolean condition is not automatically D3D CMP's floating sign test |
| `ffma` / `fmad` | potential MAD A/C/B | reject; fusion/rounding equivalence and three-source ABI are unproved |
| derivatives, discard, depth output, MRT, flow | separate control/state paths | reject from this ALU encoder |

Every accepted floating ALU must have 32-bit components, full XYZW output,
zero destination scale, no explicit NIR floating-control mode, and only its
operation's independently proven source swizzles/modifiers.  `fdot3` and
`fdot4` are scalar NIR operations: admission requires recognizing the explicit
replication feeding all four output lanes.  Scalar RCP/RSQ/EXP/LOG likewise
must not be accepted merely because the native destination mask is XYZW; the
native result broadcast remains a silicon gate.

No generic multi-record NIR program is patch-ready yet.  The Mesa builder now
derives `source.last_use` after semantic validation for its current A/C-slot
subset.  Keep a post-allocation finalizer boundary before admitting B-slot or
general multi-record runtime paths; same-record duplicate sources additionally
require the proved A-over-C-over-B release precedence.

## Cmd19 one-record operand boundary

The structured cmd19 encoder's executable v0/c0 surface is narrower than the
native field width suggests.  Version one admits identity swizzles only.
Production Linux compilation also byte-matches MOV and MUL with `aaaa`
swizzles in either A or C, but those forms remain reserved for a separately
gated successor.  Every measured one-record, no-texture object uses the same
controls:

```text
90058000 91000000 92000001 93000000 94000000 95000000
96000000 97000000 98000000 99000000 9a000000 9b000000
```

A-slot NEG also byte-matches the current packing formula. C-slot NEG does
not: Linux emits no d3.bit20, while the Windows compiler and current generic
encoder set it. For example, `v0 * -c0` is respectively:

```text
Linux   8140b6d0 30080053 00f04005 00000820
encoder 8140b6d0 30080053 00f04005 00100820
```

Keep C-slot NEG rejected until numerical silicon testing resolves that
version/chip difference. A-slot NEG is byte-exact but likewise remains a
separate numerical gate, not part of the current cmd19 allowlist.

ONE/ZERO are native literal selectors, not ordinary v0/c0 component
swizzles. Vendor canonical literal operands carry source class 5. Retaining
the requested input/constant class produces a different, unproved tuple: for
example the generic encoder makes `v0.1111` source A `00f04092`, whereas the
canonical literal source A is `00f14092`; a canonical source-C ONE is
`30140924`, not the c0-class `30080924`. D3D PS 2.0 cannot express selectors
4/5 for a differential. Cmd19 must continue to reject v0/c0 ONE and ZERO.

The complete records and statuses are in
[chrome9-cmd19-one-record-corpus.tsv](chrome9-cmd19-one-record-corpus.tsv).

## Golden acceptance corpus

`via-5.75-shader-corpus.tsv` and `windows-closed-key-shader-corpus.tsv` are
machine-readable semantic goldens. A new encoder must at least reproduce:

```text
mov oC0,v0    01c00530 301c0053 00f04005 000d0820
mov oC0,c0    01c00530 301c0053 00f08005 000d0820
mul oC0,v0,c0 0140b6d0 30080053 00f04005 00000820
mul oC0,c0,c1 0140b6d0 30081053 00f08005 00000820
tex0->oC0     ALU 01c00530 301c0053 00f20005 000d0820
               TAU 301001b0
tex0*v0       ALU 0140b6d0 30040053 00f20005 00000820
               TAU 301001b0
tex0*tex1     ALU 0140b6d0 30201053 00f20005 00000820
               TAU 301001b0 311111b1
               controls 90018000 91000003 92020001 93000002
```

The older `a01001b0 311001b1` / `3003f000` tuple is a reproducible
`CHROME9_NONPRODUCTION_CALLBACK_ORDER=1` artifact.  Fixed Xorg objects using
those bytes remain separate silicon evidence, but they are not output from
the production register manager and must not be used as its golden.

A fresh Wine three-factor replay and exact hashes are recorded in
[windows-umd-wine-entrypoint-boundary.md](windows-umd-wine-entrypoint-boundary.md).
The expanded finalized output and texture-consumer corpus is
[chrome9-ps-encoder-v1-corpus.tsv](chrome9-ps-encoder-v1-corpus.tsv).

Acceptance is stricter than byte generation:

1. scalarize, copy-propagate, DCE, and require one acyclic NIR block;
2. reject unsupported opcodes, indirects, flow, derivatives, discard, depth
   output, integer operations and unmapped varyings;
3. allocate only r0--r11 and preflight expanded counts;
4. derive and cross-check constant/varying/sampler masks;
5. byte-compare all closed golden objects in offline tests;
6. retain semantic program ids and kernel-owned packets for initial
   promotion. A future general UAPI must be structured and kernel-verified;
   raw A90/ALU/TAU arrays are not a safe ABI.

This removes duplicated operand combinations while preserving fail-closed
execution. It does not enable flow, arbitrary varyings, dependent samples,
explicit LOD, TEXKILL, depth output, or arbitrary native-code submission.
