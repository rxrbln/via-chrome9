# Chrome9 glmark2 desktop blur compiler boundary

Status: source-exact linked NIR and production compiler object; no GPU claim.

This note closes the static compiler envelope for the default glmark2 desktop
blur and identifies the first independently implementable generic Mesa gate.
It refines the generic eleven-fetch result in
`chrome9-repeated-texture-production.md`: the real shader's centre fetch uses
the interpolated coordinate directly, so it is not byte-identical to the
older all-computed-coordinate corpus.

## Default source and linked NIR

glmark2 2023.01 selects:

```text
desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true
```

`scene-desktop.cpp:create_blur_shaders()` emits eleven literal taps for each
separable pass.  The horizontal pass is:

```text
sum(i=-5..5) texture2D(Texture0,
                       TextureCoord + vec2(i * TextureStepX, 0))
                 * Kernel[abs(i)]
```

and the vertical pass exchanges X and Y.  The fragment output is
`vec4(result.xyz, 1.0)`.  Source SHA-256 values are:

```text
scene-desktop.cpp  a20a5e5346fa641aef554b912f9e9be23068df053dc1c903f44a8e756c6c53db
desktop-blur.frag  b62d22ba163e5fddc438e2e11f85c1a84f0c2460347e9f69c19b7e64c3818e35
```

The captured Chrome9 linked NIR for a 300x300 default window is
`/tmp/chrome9-glmark-nir-v4/nir/desktop.err`, SHA-256
`ef30e596da15d54827cb44e6aa0daf4a4b5480a32dcee8f915639e5823fac46d`.
The horizontal program has source BLAKE3 words:

```text
60d50ad2 95754dc8 598522fc df62b04a
7dde6550 1c934cd6 c3e1aa25 f4919d25
```

Its optimized straight-line DAG contains exactly:

| NIR item | count | detail |
|---|---:|---|
| computed coordinates | 10 | five negative and five positive `fadd(T0, literal vec2)` |
| direct coordinates | 1 | centre tap is `tex(T0)`; `T0 + vec2(0)` is folded |
| texture operations | 11 | all ordinary implicit-LOD 2-D fetches from s0 |
| weighted samples | 11 | `fmul(sample, literal scalar)` |
| accumulation | 10 | left-associated `fadd` chain |
| output construction | 1 | `vec4(sum.x, sum.y, sum.z, 1.0)` |

There are exactly seventeen distinct `load_const` values relevant to the
output DAG: ten nonzero vec2 offsets, six symmetric scalar Gaussian weights,
and scalar `1.0`.  The vertical program has the same counts and constants
with the offset lane exchanged.

## Source-exact production object

An exact PS 2.0 semantic equivalent was passed to the Linux 5.75.32.87a
production compiler and finalized with
`stmPPS_PostTranslationSetting_invi(object, 2)`.  It uses a computed
coordinate for taps -5..-1 and 1..5, direct T0 for tap zero, one MUL followed
by ten MAD accumulations, RGB output, and constant alpha.  The input token
SHA-256 is:

```text
d6b93dd51383da2dc51a832aec804c0e8bf8ba5ff232d7469383a0bb32cc289a
```

The active-resource tuple is `(8,3,1,1)` and the finalized object contains
33 ALU records and 11 TAU records:

```text
ALU
008006d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d0460
0140b6d0 30080492 00f00105 000000a0
008106d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d04a0
00020530 30081492 00f00105 000000a0
008206d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d04e0
00020530 30082492 00f00105 000000a0
008306d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d0520
00020530 30083492 00f00105 000000a0
008406d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d0560
00020530 30084492 00f00105 000000a0
00020530 30085492 00f00105 000000a0
008606d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d05a0
00020530 30086492 00f00105 000000a0
008706d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d05e0
00020530 30087492 00f00105 000000a0
008806d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d0460
00020530 30088492 00f00105 000000a0
008906d0 d0140924 0030c006 00000020
01c00530 c01c0053 00f00006 000d04a0
00020530 30089492 00f00105 000000a0
008a06d0 d0140924 0032c006 00000020
01c00530 c01c0053 00f20006 000d04e0
00020530 3008a492 00f20105 000000a0
01c00530 501c0053 00720205 000d0820
01c00530 801c0053 00808bb6 000d0820

TAU
201111b0 201121b0 201131b0 301141b0 301151b0 301101b0
301161b0 301171b0 301111b0 301121b0 301131b0

A90--A9b
90048001 910000ff 920b0021 930007ff
94000000 95926492 96000024 97000000
98000000 99000000 9a000000 9b000000
```

Packed little-endian SHA-256 values are:

| stream | SHA-256 |
|---|---|
| 33 ALU records | `b09d27d384d6e67470c21565cfc20dcca9d5e18b723110f7f691da0bf5b9fadf` |
| 11 TAU records | `8a91303c365e4e998dec5a296cbf948fdef49d2f74e7754e0f1ae9378a1dbcc6` |
| A90--A9b | `f79eac6cdc956f64b0b3cda1efc60654a2d85de3d94327ec86ee5c0cf131eca0` |
| ALU then TAU then controls | `811cfa396defb2921671edfa99fcf56351f81b2fdc95dfa52802ecf1d91d50c4` |

The ten computed coordinates consume circular physical slots 1..7 and then
1..3; the centre direct fetch consumes slot 0.  Consequently A91 is `ff`,
not the `fe` of the older eleven-computed-coordinate object.  A95/A96 encode
ALU-to-TAU transitions after ALU indices
`1,4,7,10,13,14,17,20,23,26,29`; A93 encodes TAU-to-ALU transitions after
every TAU index 0..10.  The direct centre transition after ALU 14 is real and
must not be dropped merely because that fetch needs no coordinate bridge.

The object is static compiler/finalizer evidence.  It does not independently
prove rendering, filtering, or FBO-to-texture cache closure.

## Exact first clean-room failure

The current generic NIR lowerer allocates immediate registers only in
c16..c31 (`chrome9_context.c:chrome9_ps_immediate`).  It compares and
allocates complete vec4 values; it does not track unused lanes.  The blur's
ten unique offset vectors and six unique symmetric weights consume all
sixteen registers while lowering the RGB accumulation.  The output vec4 is
then lowered component order X,Y,Z,W, and the seventeenth literal, alpha
`1.0`, cannot allocate a register.  Lowering therefore fails before semantic
instruction validation or computed-coordinate scheduling.

This is deterministic from the captured DAG and source:

```text
10 unique vec2 offsets + 6 unique scalar weights = 16 whole registers
+ scalar output alpha                              = allocation failure
```

The 96-ALU/32-TAU limits are not the first blocker.  Without contraction, the
existing semantic lowerer would emit 34 ALU plus 11 TEX instructions: ten
coordinate ADDs, eleven MULs, ten accumulation ADDs, two partial output MOVs,
and one terminal MOV.  Kernel computed-coordinate legalization adds ten
bridge MOVs, yielding a bounded 44-ALU/11-TAU native envelope.  Fusing MUL+ADD
to MAD is therefore a later performance and precision-choice optimization,
not required to clear the first correctness gate.

## Smallest generic gate

Add scalar immediate lane reuse, not a desktop-blur key:

1. Track a four-bit used-lane mask for every compiler-owned immediate c16..c31.
2. Preserve the existing exact whole-register deduplication and allocation.
3. For a scalar literal, first reuse an already occupied lane with the same
   bit pattern.  If none exists, place it in an unused lane of an existing
   immediate register and return a source swizzle selecting that lane.
4. Never pack into a user-uniform register, never overwrite a used lane, and
   fail closed for vectors that do not fit one register.
5. Include used-lane masks in any compiler variant/cache identity only if
   immediate layout is not reproduced deterministically from NIR.

This gate is generic constant-register allocation, preserves exact FP32 bits,
and is sufficient for the blur's final scalar one to share a free lane.  It
does not rely on the unqualified ALU literal-ONE selector and does not alias
the separately proven driver-owned RGBX c0.x convention.

The bounded offline test should construct the captured topology and assert:

```text
semantic: 34 ALU + 11 TEX
samplers: s0 only
varyings: T0 only
immediate register population: 16
packed scalar lane: exact 3f800000
all instruction/constant/UAPI bounds accepted
one extra unmatched scalar or any occupied-lane conflict: rejected
```

Only after that test passes should a guarded render compare horizontal and
vertical output against a CPU eleven-tap reference.  The first live image
should make every tap distinguishable; a uniform-color texture cannot detect
wrong transition maps, coordinate reuse, or a missing centre fetch.

## Compiler implementation and offline qualification

The Mesa Chrome9 backend now packs bit-exact literal components into free
lanes of c16..c31, preserving full-register ownership for uniforms.  A
computed-coordinate alpha-KILL test also verifies that its 0.5 threshold can
be read from a packed Y lane.  Host tests cover 64 unique scalar literals
fitting in sixteen registers, rejection of a 65th, a uniform discovered
later in the NIR block, and distinct `+0`/`-0` vector components.

An additional host test reconstructs the captured 300x300 horizontal blur
with the exact FP32 offsets and weights above.  It compiles to 45 semantic
instructions: 20 ADD, 11 MUL, 3 MOV, and 11 TEX.  It checks that the first
five and last five TEX operations use the corresponding preceding computed
coordinate ADD results, the sixth/centre TEX uses direct T0, and the packed
alpha-1.0 lane reaches the terminal color output.  The semantic UAPI
translation accepts the resulting program.  This is compiler evidence only;
it does not yet validate GPU execution, FBO-to-texture coherency, or the
complete glmark2 desktop scene.
