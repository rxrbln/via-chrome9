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
90000000 91000001 92020001 93000002 94000000 95000000
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

## Texture and dependency finalization

Raw ordinary compiler TEXLD words are:

```text
201001b0  # r0 = TEX(t0,s0)
211111b1  # r1 = TEX(t1,s1)
```

TEXLDP=`207001b0` and TEXLDB=`203001b0` remain excluded pending runtime
semantics. Validate sampler id before packing; unchecked s16 collides with a
valid finalized s0 word.

Configuration-2 finalization owns dependency fields. One leading sample
becomes TAU `301001b0` and a consuming ALU gets d2.bit17. Two independent
samples followed by a consuming ALU use:

```text
TAU a01001b0 311001b1
ALU ... ... 00f20005 ...
```

High TAU bits depend on phase order. Never implement `tau |= 10000000`.
Until an SSA-edge finalizer byte-matches the corpus, emit only the exact
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
`0140b6d0 30082053 00f20005 00000820`; it marks an ordinary earlier-ALU
producer in the same source-A field. Dependency metadata must come from the
complete producer graph and phase finalizer, never merely from source
register class.

The raw/final differential separates scheduler allocation from dependency
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

Configuration-2 adds dependency bits to both records, changes TAU bit28, and
updates A90. The historical destination-id-15/source-id-63 object is retained
in the corpus only as `NONPROD_CALLBACK_SWAP_*`: it is reproduced by swapping
the validator and processor callback slots and is not hardware evidence.

The H5 register manager exposes 12 ordinary plus four high temporary slots;
the native destination field is four bits. Production compilation maps r0
through r15 without the id-63 miss. The vendor frontend nevertheless accepts
r16: its destination wraps to native id 0 while its source remains id 16.
Mesa must therefore allocate at most 16 simultaneously addressable physical
fragment temporaries and reject or spill anything beyond that boundary.

The Windows backend before configuration-2 and the Linux production
finalizer give exact one-texture dependency deltas for every v1 source slot:

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
source B, dependency finalization changes d0 rather than d2. A generic
"sampled operand means d2.bit17" rule is therefore wrong.

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
tex0*tex1     ALU 0140b6d0 3003f000 00f20005 00000820
               TAU a01001b0 311001b1
```

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
