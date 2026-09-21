# Chrome9 fragment NIR lowering matrix

Status: compiler-backend routing contract.  This consolidates exact native
shapes, semantic mismatches, and fail-closed policy for Mesa 26.2 NIR.  It
does not advertise operations or replace the guarded silicon gates in the
operation-specific notes.

## Admission levels

Each NIR form belongs to exactly one level:

1. **current exact key** -- the existing runtime already selects a bounded,
   kernel-owned program for that complete expression;
2. **encoding closed** -- both proprietary compilers establish native bytes,
   but numerical silicon behavior or a structured ABI is still missing;
3. **expansion closed** -- a complete multi-record compiler schedule exists,
   but the current one-record path cannot submit it;
4. **static only** -- emitter disassembly predicts a shape without a complete
   production raw/finalized object;
5. **reject** -- frontend disagreement, silent zero-code success, unsupported
   token, or a NIR semantic mismatch prevents lowering.

No level below current exact key is executable merely because an instruction
can be encoded.  Every accepted value is 32-bit float with default FP controls
unless a narrower operation contract says otherwise.

## Ordinary ALU

The native ALU has parallel operands A, B, C.  The common arithmetic relation
is `B + A*C`, while d3[19:16] and other fields select specialized operations.
Logical NIR source numbers must first be assigned to these physical slots:

| NIR form | native schedule | level and fail-closed rule |
|---|---|---|
| `nir_op_mov` | MOV, A=src | current exact key only for existing identity/aaaa leaves |
| `nir_op_fmul` | MUL, A=src0, C=src1, B=ZERO | current exact key only for existing bounded pairs |
| `nir_op_fadd` | ADD, A=src0, B=src1, C=ONE | encoding closed; source-B and finite-domain silicon gate pending |
| `nir_op_fsub` | SUB, B=src0, A=src1, C=NEG_ONE | encoding closed; never synthesize as unordered ADD+NEG |
| `nir_op_fmin` | MIN subop c, A/C | encoding closed; finite `v0,c0` is preferred first gate |
| `nir_op_fmax` | MAX subop b, A/C | encoding closed; independently gate operand order and NaN rule |
| `nir_op_fdot3_replicated` | DP3 subop 2, A/C | encoding closed; only explicit full-vector replication may match |
| `nir_op_fdot4_replicated` | DP4 subop 3, A/C | encoding closed; only explicit full-vector replication may match |
| scalar `nir_op_fdot3`/`fdot4` | same native record only after proven broadcast | reject direct scalar/partial-store shortcut |
| `nir_op_ffract` | FRC subop e, A | encoding closed; negative, endpoint and precision gates pending |
| `nir_op_fabs` | ABS-family subop 9, A/B/C derived from one source | encoding closed, but terminal constant forms should CPU-fold to MOV first |
| `nir_op_fsat` | producer destination d3.bit12 | encoding closed only when folded into that exact producer; clamp order/NaN gate pending |
| `nir_op_fneg` | source modifier, slot- and opcode-dependent | reject generically; Linux/Windows disagree for some C-slot paths |
| `nir_op_ffma`, `ffma_weak`, `ffmaz` | possible MAD, A=src0, C=src1, B=src2 | reject: native fusion and exceptional/rounding semantics are not proven |
| multiply then add with contraction permitted | possible MAD | reject until the NIR exact/contract flags and native rounding are matched |
| `nir_op_bcsel` | not a general CMP mapping | reject; a boolean condition is not D3D CMP's floating sign predicate |
| exact `bcsel(fge(x,+0),t,f)` | CMP subop 9 candidate A=x,C=t,B=f | encoding closed only as an exact pattern; zero/NaN predicate silicon gate pending |

Native DP3/DP4 and scalar operations write a vector destination.  The backend
must not silently reinterpret scalar NIR as broadcast.  It may accept an
explicit replicated NIR operation or prove that one scalar SSA value feeds
all four lanes of a terminal vec4 with no other uses.

## Scalar operations

The scalar family uses A=`000W`: A.xyz are literal ZERO and A.w selects the
requested scalar component.  A full XYZW destination mask is compiler output,
not silicon proof of result broadcast.

| NIR form | native subop | level and required first domain |
|---|---:|---|
| `nir_op_frcp` | 6 | encoding closed; c0.w in `[1,16]`, nonzero |
| `nir_op_frsq` | 7 | encoding closed; c0.w in `[1,256]`, positive |
| `nir_op_fexp2` | 4 | encoding closed; c0.w in `[-4,0]` |
| `nir_op_flog2` | 5 | encoding closed; c0.w in `[1,2]`, positive |

Those bounds are the first guarded policy, not hardware limits.  Swizzled
component selection is compiler-exact, but the first gate stays c0.w with
identity scalar selection, full replicated output, no modifier/SAT/scale,
and exact one-record controls.

## Multi-record and special vector operations

| NIR or recognized expression | compiler result | policy |
|---|---|---|
| `nir_op_ffloor` | FRC subop e then subop-9 record | static only; no production dynamic finalized object |
| `nir_op_flrp` | Windows one dedicated record; Linux two ordinary records | reject cross-driver mismatch |
| `nir_op_fpow` | LOG, multiply, EXP; three records | expansion closed; positive bounded domain only after multi-ALU gate |
| normalize expression | DP3, subop9, RSQ, MUL; four records | expansion closed only as indivisible vendor NRM lowering |
| cross-product expression | two-record CRS expansion | expansion closed; one temp and B-slot final-use marker |
| DST expression `(1,a.y*b.y,a.z,b.w)` | one specialized B+A*C record | encoding closed for exact same-source `v0,v0` first key |
| `nir_op_fdph_replicated` | DP4-family A=`xyz1`, C=`xyzw` | static only; production wrapper and finalization still missing |
| `nir_op_fsin` / `nir_op_fcos` | public D3D SINCOS is a three-record paired approximation | reject one-to-one mapping; split internal emitters are not dynamic contracts |
| `nir_op_fsign` | no usable pixel SGN program | reject; Windows tokenizer faults and Linux rejects |
| `nir_op_fmod` / `nir_op_frem` | no closed direct opcode | reject; do not infer from FRC/FLR/divide identities |

Multi-record schedules require post-allocation whole-register lifetime
finalization from
[chrome9-ps-temp-lifetime-finalizer.md](chrome9-ps-temp-lifetime-finalizer.md).
The A/B/C flags are single final-use/release markers, not producer-edge bits.
An operation-specific expansion cannot be concatenated from isolated records
unless its exact scheduled object and lifetime fields match the corpus.

## Texture and side effects

| NIR class | native path | policy |
|---|---|---|
| ordinary `nir_texop_tex`, 2-D implicit LOD | TEXLD TAU plus consumer ALU | current bounded texture keys only; arbitrary dependent sample remains rejected |
| projective `nir_texop_txp` | TEXLDP TAU opcode 7 | compiler exact; reject until silicon projection and q=0 behavior pass |
| bias `nir_texop_txb` | TEXLDB TAU opcode 3 | compiler exact; reject until bias/LOD silicon gate |
| explicit LOD `nir_texop_txl` | compiler emits zero TAU | reject |
| explicit gradients `nir_texop_txd` | parser-desync residue/zero useful TAU | reject |
| implicit derivatives used by ordinary sampling | sampler hardware path | do not equate with programmable derivative ALU |
| `fddx` / `fddy` | compiler reports success with zero ALU/TAU | reject |
| discard/demote | TEXKILL has an exact separate schedule | reject from general ALU encoder; dedicated side-effect ABI only |
| depth store | oDepth class2/id4 plus A13.bit23 | reject from general ALU encoder; dedicated depth-output state contract |
| MRT stores | oC0..oC3 ids0..3 | current runtime remains oC0 only; reject extra outputs |
| control flow, phi, indirect, relative constants | incomplete/unsafe frontend paths | reject before allocation |

## Backend ordering

The smallest correct compiler pipeline is:

1. require one acyclic NIR block and run copy propagation plus DCE;
2. recognize only rows admitted by the current capability level;
3. preserve exact/fast-math flags and reject any semantic mismatch;
4. lower logical operations to their exact A/B/C assignments or indivisible
   recorded expansion;
5. schedule TAU and ALU records;
6. allocate no more than 12 physical temporaries for the first gate, with 16
   as the proven encoding ceiling;
7. encode physical source/destination ids and validate resource masks;
8. mark exactly one final source occurrence per physical vec4 temporary;
9. require no dead temp destinations in the first executable compiler path;
10. select a closed A90--A9b tuple and byte-compare the whole object against
    the golden corpus before submission.

Fallback is rejection or a separately proven CPU/utility path, never an
approximately similar native opcode.  In particular, compiler success with
zero records, wrapped ids, or a cross-driver schedule mismatch is negative
evidence.

## Evidence routing

The one-record encodings are in `mesa-ps-encoder-v1.md` and the
`chrome9-next-alu-*`, scalar, FRC, CMP/ABS/SAT, DST and DPH notes.  Multi-record
shapes are in the FLR, LRP, CRS, NRM/POW, and SINCOS audits.  Texture negative
boundaries are in the TEXLDP/TEXLDB and explicit-LOD audits.  This matrix is
an index over those independently retained corpora; where it says static
only or reject, it intentionally does not manufacture executable words.
