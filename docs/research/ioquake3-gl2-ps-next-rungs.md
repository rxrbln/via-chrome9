# Chrome9 ioquake3 GL2 pixel-shader next rungs

Status: source inventory plus static Windows/Linux reverse engineering and
fresh hardware-free compiler differentials, 2026-09-21.  None of the new ALU
or multi-phase programs in this note has executed on VX900 silicon.  The
existing texture, blend, alpha-test and D16 paths cited here remain the only
live evidence.

This note starts where
[ioquake3-gl2-fragment-constants.md](ioquake3-gl2-fragment-constants.md)
ends.  The 32-vector fragment-constant store is large enough for ioquake3's
startup declarations, but it does not make the resulting programs executable.
The next implementation must still recognize closed NIR shapes, select a
kernel-owned native program, and reject everything else before publication.

The machine-readable compiler rows are in
[windows-ps-gl2-next-corpus.tsv](windows-ps-gl2-next-corpus.tsv).

## Startup inventory after the constant gate

The audited application is ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`.  Its
`GLSL_InitGPUShaders()` constructs every program family at startup, rather
than compiling only the first scene's program:

| family | source loop / call | startup variants | fragment-side requirement |
|---|---|---:|---|
| generic | `tr_glsl.c:936..986` | 48 with bones, 32 without | one 2D sample, texture times primary, runtime integer alpha-test tree |
| texturecolor | `991..1000` | 1 | one 2D sample times one vec4 constant |
| fogpass | `1004..1038` | 6 with bones, 4 without | clamp and square root into output alpha |
| dlight | `1041..1063` | 2 | same fragment shape as generic |
| lightall | `1066..1222` | 42 at default cvars with bones, 28 without | one to seven samplers, dot/normalize/clamp/specular ALU, runtime alpha-test tree |
| shadowfill | `1224..1257` | 3 with bones, 2 without | FRC and pack arithmetic or a scalar depth move |
| post/shadow helpers | `1259..1431` | 12 | offset/dependent samples, accumulation, comparisons, derivatives and discard |

The counts follow the exact conflict skips in the source.  The 12 helper
programs are pshadow, down4x, bokeh, tonemap, two calclevels4x variants,
shadowmask, ssao, and four depth-blur variants.  In total the default source
builds 114 programs when animated bones are available and 81 otherwise.  Several
variants share identical fragment source; these are program creation counts,
not unique native PS objects or measured draw frequency.

The default cvars which drive the 42 lightall forms are normal mapping on,
specular mapping on, parallax off, cube mapping off, and sunlight mode one.
Consequently the useful execution priority is not the lexical order in
`GLSL_InitGPUShaders()`:

1. generic/dlight and texturecolor, because they are ordinary scene/UI paths;
2. reduced fast-light lightall, which is the known two textures times primary
   product when normal/specular mapping is disabled;
3. default lightall DP3/RSQ/MUL normalization and clamp/MAD arithmetic;
4. down4x/bokeh offset-sample accumulation;
5. shadow/cube/derivative paths only after their separate texture semantics
   exist.

A lexical count over the fragment sources gives 83 `texture2D`, 26 `dot`, 14
`shadow2D`, 13 `discard`, eight `sqrt`, eight derivative, five `fract`, five
`mod`, four `normalize`, four `textureCubeLod`, and two `pow` occurrences.
These counts include compile-time-dead branches and unrolled source text; they
rank reverse-engineering value but do not claim post-NIR instruction counts.

## The first runtime branch should not become native flow

`generic_fp.glsl:12..32` and `dlight_fp.glsl:11..31` compute exactly:

```text
rgba = texture2D(s0, coord0) * primary
if alpha_mode == 1: discard when rgba.a == 0
if alpha_mode == 2: discard when rgba.a >= 0.5
if alpha_mode == 3: discard when rgba.a < 0.5
output = rgba
```

The result expression is the already recovered one-texture RGBA MODULATE
program.  The integer uniform must not be uploaded as if it were a float
constant: raw integer one has bits `00000001`, not IEEE float one, and no
fragment integer compare path is proven.

The safe Mesa key is a complete structural recognizer.  It may ignore copy,
SSA and vec-construction nodes, but must prove one ordinary non-shadow 2D
sample, one full RGBA multiply by COL0, one full oC0 write, the exact nested
uniform branch above, no other store/discard, and a direct constant-buffer-zero
32-bit integer load at a recorded aligned byte offset.  Mode one additionally
requires the existing normalized D1 primary-color route and a normalized
UNORM sampler, so the product cannot be negative.  At draw time Mesa can
read that CPU-visible dword and select:

| uniform | shader keep condition | fixed-function candidate |
|---:|---|---|
| 0 | all fragments | alpha test disabled, canonical disabled A33 |
| 1 | alpha greater than zero | GREATER/zero, `A33=33004000` |
| 2 | alpha less than one half | LESS/half |
| 3 | alpha greater than or equal to one half | GEQUAL/half |

Unknown values, indirect uniform offsets, a different output alpha, a second
discard, or any additional side effect must reject the draw.  Program creation
may retain the shader as unsupported; it must not fail application linking
merely because a later draw is rejected.

There is one exactness trap.  The existing semantic alpha-test ABI accepts an
8-bit reference and expands byte `80` to the proprietary value `202`:

```text
encode_ref(80) = (80 << 2) | (80 >> 6) = 202
```

That represents the byte-quantized compatibility reference, not necessarily
the GLSL literal `0.5`.  The apparent exact half in A33's endpoint-inclusive
fixed field is `200`, producing candidate words `33001200` and `33006200`.
No proprietary builder emits that direct value and it has not run live.
Therefore mode 0 and mode 1 can reuse the current fixed state exactly, while
modes 2 and 3 must remain gated until a guarded `200` versus `202`
discriminator proves the field interpretation.  Approximating the GLSL branch
with the current byte reference must be labelled approximate, never silently
accepted by an exact classifier.

Do not use TEXKILL for this first path.  The recovered TEXKILL lowering kills
negative lanes, changes TAU scheduling, and is not live-proven to suppress
color, depth and stencil together.  Fixed A33 already has that ordering proof.

## Common ALU encodings

Fresh `ps_2_0` probes wrote directly to oC0 and passed through the 2015 Windows
scheduler.  Each four-dword row below is compiler-exact.  For one-record
ALU-only programs the execution controls are the existing closed tuple
`90058000 91000000 92000001` with A93--A9a zero.  SGE has two records and uses
`92000002`.

```text
ADD c0,c1  00810530 30140924 00f08005 00000820
MAD c0,c1,c2
             00820530 30081053 00f08005 00000820
MUL c0,c1  0140b6d0 30081053 00f08005 00000820
RCP c0     01c00530 b01c0053 00f080b6 00060820
RSQ c0     01c00530 b01c0053 00f080b6 00070820
DP3 c0,c1  01c00530 50081055 00f08005 00020820
DP4 c0,c1  01c00530 30081053 00f08005 00030820
MIN c0,c1  01c00530 30081053 00f08005 000c0820
MAX c0,c1  01c00530 30081053 00f08005 000b0820
FRC c0     01c00530 301c0053 00f08005 000e0820
ABS c0     00800538 30080053 00f08005 00090820
CMP c0,c1,c2
             00820530 30081053 00f08005 00090820
SGE c0,c1  00810538 30080924 00f08005 00000020
             0140b6d0 30140924 00f00005 00090820
```

CMP has D3D semantics `c0 >= 0 ? c1 : c2`; its native field order is not the
source-token order.  SGE is a SUB-like first record followed by a CMP-family
record into oC0, not a unique one-record subopcode.  Configuration-2
finalization changes SGE's second-record source word from `00f00005` to
`00f20005`; use that Linux execution form with `A92=92000002`, not the
Windows pre-dependency row above.  Direct SLT was rejected by
the D3D frontend even though the OpenGL internal dispatch table contains
`STM_SLT_ICD` and calls native emitter `VTGUModeDX32A:0x10216550`.  Until that
internal emitter receives an independently allocated executable object, NIR
`flt` must not be generalized from SGE or CMP.

The D3D/OpenGL cross-check is structural, not an assumption: the OpenGL ICD
compiler/cache constructor, scheduler and metadata finalizer at
`102463c0/10256550/10254d30` are byte-identical to D3D
`101ddc20/10203530/10201cb0`.  Common RCP, RSQ, DP4, MIN, MAX, SGE, EXP, LOG,
FRC, ABS, TEXKILL, TEX and CMP dispatch directly to the same native emitters;
the per-frontend alternatives for ADD/MAD/MUL/DP3 still produce the same
records in the Linux and Windows corpora.

These encodings are patch-ready templates, not a reason to expose native
instructions.  Initially admit separate semantic keys with exact operand
classes, swizzles, writemasks and constant masks.  Every numerical family
needs a silicon oracle before advertising arbitrary NIR lowering.  RCP/RSQ in
particular need zero, denormal, sign and precision characterization.

## Exact normalize rung

The high-frequency lighting form `normalize(c0.xyz)` compiles from
`DP3 r0.x,c0,c0; RSQ r0.x,r0.x; MUL oC0,c0,r0.xxxx`.  Windows and Linux agree
on the first two records.  The Linux allocator plus mandatory configuration-2
finalizer supplies this executable three-record object:

```text
A90..A92 90058000 91000000 92000003
ALU      01c00530 50080055 00108005 00020020
         01c00530 801c0053 001000b6 00070020
         0140b6d0 30200000 00f08005 00000820
```

Native temporary allocation in the last row differs from the minimal Windows
identity-remap wrapper, so only the Linux object above is an execution
candidate.  It stays offline-only.  It proves that the vendor allocator can
represent the DP3/RSQ/MUL chain within three records; it does not prove
normalization accuracy or safe zero-vector behavior.

The corresponding NIR key must be the exact scalarized chain `fdot3(v,v)`,
`frsq`, and componentwise multiply by the original vector, with no reassociate
or reciprocal rewrite.  The first live oracle should use axis-aligned vectors
whose normalized result is exactly zero/one, followed by `(3,4,0)` with a
tolerant expected `(0.6,0.8,0)`.  Zero input remains rejected until its result
is measured.

## Texture forms and scheduler boundaries

The semantic TAU operation nibble is independently closed:

```text
ordinary TEXLD   201001b0   operation 1
bias TEXLDB       203001b0   operation 3
TEXKILL           ...4...    operation 4
projected TEXLDP 207001b0   operation 7
```

Only ordinary normalized 2D TEXLD is part of the current live sampler path.
TEXLDP has an offline compiler object and a separately recovered coordinate
contract.  TEXLDB, cube sampling, shadow comparison, explicit LOD and explicit
gradients remain outside the first GL2 admission.

Five independent ordinary samples followed by one MOV establish the regular
Windows schedule:

```text
TAU       201001b0 211101b1 221201b2 231301b3 241401b4
A90..A94 90000000 91000000 92050001 93000010 94000000
```

With proper DCL records and Linux configuration-2 finalization, the executable
counterpart is:

```text
TAU       a01001b0 a11001b1 a21001b2 a31001b3 341001b4
A90..A94 90000000 91000001 92050001 93000010 94000000
ALU       01c00530 301c0053 00f20005 000d0820
```

Thus A93 bit `N-1` switches from N independent TAUs to the consuming ALU for
N=1..5.  This proves scheduler construction through sampler four, but not
texture-state linkage or silicon behavior.  Native sampler ids are four bits
and the compiler store holds 32 TAUs; the Windows OpenGL ICD advertises eight
texture units.  The bounded driver must retain its current two-unit limit
until A18--A32 state, varying linkage and BO lifetime are extended together.

The first postprocess coordinate shape, `coord=t0+c0; sample s0 at coord;
oC0=sample`, needs an ALU-to-TAU-to-ALU schedule.  Linux configuration two
emits exactly:

```text
A90..A95 90040000 91000001 92010003 93000001 94000000 95000002
ALU       00800530 30140924 00f0c005 00000020
          01c00530 301c0053 00f20005 000d0420
          01c00530 001c0053 00f03f00 000d0820
TAU       b01001b0
```

The middle ALU is the compiler-inserted coordinate transfer.  A95 bit one
switches after that second ALU; A93 bit zero switches back after TAU zero.
This is the minimum scheduler oracle for down4x/bokeh-style offset samples.
It is not yet safe to extrapolate a 16-sample object in the kernel.  Generate
and verify every transition bitmap and dependency bit from a closed object;
never derive one by setting bit 28 on every TAU.

Several apparent compiler successes are deliberate rejection evidence:

| source operation | observed result | policy |
|---|---|---|
| DSX / DSY | return success, zero ALU and zero TAU records | reject derivatives |
| IF; MOV; ENDIF | return success, only the MOV record survives | reject every PS flow node |
| TEXLDL | return success but no native record | reject explicit LOD |
| TEXLDD | frontend failure | reject gradients |
| SLT | frontend failure | reject strict comparison as a native key |

Static Linux analysis agrees: PS 2.0 has no CALL/IF/LOOP dispatcher entries,
and IFC/BREAK/BREAKC can parse without appending a native operation.  There is
no recovered PS branch stack, target field or termination record.  Constant
loops may be unrolled in NIR if the resulting straight-line object fits, but
native fragment control flow is not a Chrome9 capability at this stage.

## Versioned implementation boundary

Reuse the job-local fragment-constant record proposed in the preceding note.
Do not add raw ALU/TAU arrays to UAPI.  Add semantic program ids in small,
capability-separated groups:

1. `TEX0_PRIMARY_ALPHA_SPECIALIZED`: exact generic/dlight result, one texture,
   COL0 and recorded integer alpha-mode offset; modes 0/1 first, 2/3 only
   after the exact-half A33 oracle.
2. `TEX0_C0_MODULATE`: already defined by the constant transport plan.
3. `TEX0_TEX1_PRIMARY`: existing compiler-exact three-factor program, promoted
   only after its distinct-color live oracle.
4. `C0_C1_ADD`, `C0_C1_DP3`, and `C0_C1_C2_MAD`: one-record ALU numerical
   oracles, each with an exact used-constant mask.
5. `NORMALIZE_C0`: the three-record closed object above, behind a later ALU
   capability.
6. `OFFSET_TEX0`: the one-offset schedule above, behind a dependent-coordinate
   capability and never silently generalized to arbitrary sample count.

Every kernel id owns its A90--A9a controls, ALU store, TAU store, constant
mask, sampler count and accepted draw-state combinations.  A92 counts must be
nonzero and bounded by 96 ALU/32 TAU; temporary ids remain 0..11 initially;
only oC0 is legal; constants stop at c31; sampler ids stop at the capability's
exact bound.  A9b stays kernel-owned.  Unused control words, constant lanes,
program-store tail and packet padding must be zero or canonical command
padding.

Mesa should record the semantic key in `chrome9_shader` at create time but
read mutable constant values only at draw time.  It must validate constant
buffer zero, direct offsets, alignment, bounds and finiteness, then copy the
entire job payload once.  No CPU-precomputed replacement is valid for general
per-fragment DP3/normalize/offset sampling.

## Guarded promotion oracles

Use a guarded BGRA8888 target, canonical closure, one known interior sample,
outside prefill, all BO guards, cookie retirement and MMIO error checks after
every draw.  Start without depth/blend, then repeat under already-live D16 and
blend tuples only after the color result is stable.

1. **Alpha mode zero/one.** A four-texel alpha strip `00,01,7f,ff` times
   primary alpha `ff`; mode zero draws all and mode one leaves only alpha zero
   untouched.  Repeat `on -> off -> on` to prove A00/A33 closure.
2. **Exact half discriminator.** A native constant-output test writes fragment
   alpha exactly IEEE `0.5`.  LESS with candidate A33 ref `200` must reject
   equality, while ref `202` must pass.  Repeat with GEQUAL.  Do not enable
   generic modes 2/3 until this distinguishes the codes on silicon.
3. **ADD/MAD/DP3.** Use zero/one constants first: ADD with disjoint lanes,
   MAD with one multiplier lane zero, and DP3 of orthogonal then identical
   axes.  This avoids output-rounding ambiguity while proving opcode choice.
4. **RCP/RSQ.** RCP(1) and RSQ(1) must produce white; RCP(2) and RSQ(4) then
   test one-half with a one-byte output tolerance.  Zero, negative, denormal,
   NaN and infinity stay rejected.
5. **Normalize.** Axis vectors first, `(3,4,0)` second, zero vector rejected.
6. **Five-sample schedule diagnostic.** Only after texture units 0..4 have
   complete state/lifetime support, give each a one-hot color and combine them
   without saturation ambiguity.  Until then the five-TAU object remains
   offline evidence.
7. **Offset sample.** A 4x4 quadrant texture and exactly one texel-sized
   constant offset must select the neighboring quadrant.  Zero-offset before
   and after proves transition/cache closure.

No passing compiler return, application link, frame counter or absence of a
kernel error is a pixel oracle.  Promotion requires the exact readback plus
guards and a following known-good draw.

## Provenance and confidence

| artifact | SHA-256 |
|---|---|
| Windows D3D UMD | `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| Windows OpenGL ICD | `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c` |
| Linux 5.75 compiler | `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4` |
| Wine compiler wrapper | `a87f9bf57008362bdfb8e616860b19dd8d8442d17b5c5f0e77edb4b2ec13d94c` |
| direct-oC0 ALU replay JSON | `044de418e5f91415b0270b842ad94dda2b0e82e2e152ff7e8c8812c9f01d1b56` |
| one-to-five sampler replay JSON | `c8c048d0e78bcc11914fc35110b6cc89dab3e20d9b0eb9dab82a352aa029a85f` |
| rejection replay JSON | `578b80fc2e67f3f464265a49f6b681c4929b54e8c744444a633552b88bea2269` |
| normalize replay JSON | `68cf691e903df43eb5143fffaa2e95311c94b4b1dcc873b26e2998927fd21882` |
| offset-sample replay JSON | `0a61d8ed9e2fc6b241fadd1ff16779c525927d43128fb1729eb7bcfcc8186002` |

ALU words, compiler limits, transition maps, byte-identical backend anchors
and silent-loss cases are **compiler-exact/code** evidence.  The ioquake
family counts and source shapes are **source-exact**.  NIR spelling before a
retained GL2 dump is **implementation-plan** evidence.  New numerical,
five-sampler, dependent-coordinate and exact-half A33 behavior is
**provisional until live**.
