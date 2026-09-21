# Program 24 signed-repeat oracle and sustained-submit hang audit

## Result

Program 24 (the structured `TEX0_ALPHA_PRIMARY` selector, UAPI value 2) and
capability-40 signed REPEAT coordinates compose without any packet-layout
change.  Signed S/T remain raw immediate float dwords; only stage-0 sampler
A32 selects repeat.  The exact four-ALU object grows the one-triangle body by
eight dwords, but all three current no-depth/depth/depth+blend forms still
produce the same PAUSE `0x4f0`, STOP `0x570`, and copied/read-ahead end
`0x790` as the shorter program.  It therefore does not increase ring advance,
copy length, or wrap frequency.

The proposed command-15 batching path cannot explain a hang in the current
q3 run: command 15 and capability 31 are reserved documentation only and are
absent from the kernel ioctl table and Mesa dispatch.  A four-ALU *execution*
or state-transition fault remains possible until the combined guarded oracle
below passes, but body-length overflow and command-15 execution are excluded
by the current source.

## 2026-09-21 live composition failure

A later complete smoke run with `two_texture_signed_repeat=1` passed the
ordinary two-unit signed-REPEAT sweep and then failed the dedicated
"program24-equivalent signed-repeat draw" gate.  Engine status remained
clean.  This narrows the failure to the selector-2 program/state/readback
contract rather than cap-40 coordinate validation or a command-reader wedge.

The old A01 affine-primary discrepancy is no longer present in the tested
source.  `chrome9_build_two_textured_triangle()` now calls
`chrome9_depth_control_word(request, tex0_alpha_primary)` and asserts the
same word, producing `01ce154c` for this depth-writing selector-2 case.

The highest-priority remaining packet discriminator is stage 1's image
format.  The recovered selector-2 object below requires BGRA for both
samplers (`A30=30990000`), but
`run_pipe_two_npot_textures(..., texture0_alpha=true)` currently creates only
unit 0 as BGRA; unit 1 remains BGRX.  Thus the Gallium baseline and signed
loop already use `A30=30980000` for unit 1 and may return before the final
direct-ioctl oracle.  That direct oracle independently assigns unit 1 BGRX as
well.  The kernel accepts the tuple, so it can retire without an engine error
while remaining outside the exact recovered program contract.

For the next run, change only unit 1 to BGRA in both places: set
`texture_templates[1].format = PIPE_FORMAT_B8G8R8A8_UNORM` in the
`texture0_alpha` branch before resource creation, and assign
`DRM_CHROME9_TEXTURE_FORMAT_BGRA8888` to direct-oracle unit 1.  Retain its
texels as `00ffffff`; program 24 ignores TEX1 alpha, so the expected output
does not change.  Do not broaden selector 2 to BGRX until that tuple has its
own readback proof.

The smoke contains two distinct post-cap-40 checks and should report their
actual pixels separately on the next run:

1. the Gallium selector-2 signed loop, which expects output alpha
   `0x3e..0x41`; and
2. the direct unequal-RHW oracle at `(48,16)`, which expects alpha `0x80`, red
   `0x6f..0x74`, and green/blue zero.

An ioctl error distinguishes validation from execution; a retired but wrong
pixel distinguishes sampler/program/FVF state.  Preserve submitted/rejected
counters, all four BO guards, D16, cookie, and `040c` in either case.

## Exact semantic discriminator

Use command `0x13`, the already-audited two-texture D16+blend envelope, so a
BGRA render target preserves fragment alpha.  Keep blending and alpha test
disabled inside that envelope, set color mask RGBA, use D16 LESS with a
passing incoming depth, and select top-level program bit 1/value 2.  The four
BOs remain distinct, native, canonical, and guarded.

Both sources are 32x32 BGRA, pitch 128.  Repeat stage 0 in S/T and clamp stage
1.  Fill every row identically.  Columns other than 3 and 28 use unique
sentinel colors; the two addressed columns are:

| source | logical RGBA | little-endian BGRA u32 |
|---|---|---:|
| TEX0 column 3 | `(255,128,64,255)` | `ffff8040` |
| TEX0 column 28 | `(64,255,128,0)` | `0040ff80` |
| TEX1, every texel | `(0,255,255,0)` | `0000ffff` |
| primary D1 | `(255,255,0,128)` | `80ffff00` |

Program 24 implements:

```text
RGB = TEX0.rgb * TEX1.rgb * primary.rgb
A   = TEX0.a * primary.a
```

Every expected output channel is exact because each multiplication chain
uses zero or 255 except the final alpha selection, whose other factor is
exactly 255 or zero:

| stage-0 S | repeat fraction / selected column | expected logical RGBA | BGRA target u32 |
|---:|---|---|---:|
| `+31.109375` (`41f8e000`) | `0.109375`, column 3 | `(0,128,0,128)` | `80008000` |
| `-30.890625` (`c1f72000`) | `0.109375`, column 3 | `(0,128,0,128)` | `80008000` |
| `-31.109375` (`c1f8e000`) | `0.890625`, column 28 | `(0,255,0,0)` | `0000ff00` |
| `+30.890625` (`41f72000`) | `0.890625`, column 28 | `(0,255,0,0)` | `0000ff00` |

Hold stage-0 T and both stage-1 coordinates at `0.5` (`3f000000`).  Make each
triangle use one constant coordinate so the first gate has no large
derivative or LOD ambiguity.  The two signs selecting the same column must be
byte-identical.  A full-RGBA product incorrectly including TEX1 alpha yields
alpha zero for column 3, directly distinguishing it from program 24.

The test order is:

```text
program24 + in-range S=0.109375
program24 + S=+31.109375
program24 + S=-30.890625
legacy program + in-range coordinates
program24 + S=-31.109375
program24 + S=+30.890625
program24 + in-range S=0.109375
```

This closes both program and wrap-state restoration without an engine reset.
After constant-coordinate draws pass, one triangle may span columns 3 and 28
across the signed boundary.  Do not start with that interpolation test.

## Exact sampler, control, TAU, and ALU words

For command 0x13 the stage blocks are:

```text
stage 0 image A30 = 30990000              # BGRA
stage 0 sampler   = 30000000 31100000 32000052 cccccccc
stage 1 image A30 = 30990000              # BGRA required by selector 2
stage 1 sampler   = 30000000 31100000 32000049 cccccccc
```

Nearest/no-mip is intentional for the first discriminator.  Linear filtering
changes both A31 words to `31102490` only after nearest succeeds.  Signed
repeat changes no PS, TAU, FVF, VS, image, relocation, or packet-length word.

The program-24 physical object is indivisible:

```text
# PS control A90..A9a
90000000 91000001 92020004 93000002
94000000 95000000 96000000 97000000
98000000 99000000 9a000000

# two ordinary TAUs
a01001b0 311001b1

# four ALUs
0140b6d0 3003f000 00f00005 000003e0
0140b6d0 50040055 00703f00 000003e0
0140b6d0 b0040b6b 008200b6 200003e0
01c00530 001c0053 00f03f00 000d0820
```

The command-13 body indices, including framing words, are exact:

| body indices | contents |
|---:|---|
| 24 / 29 | stage-0 image address / BGRA A30 |
| 36..39 | stage-0 sampler `30000000 31100000 32000052 cccccccc` |
| 44 / 49 | stage-1 image address / BGRA A30 |
| 56..59 | stage-1 sampler `30000000 31100000 32000049 cccccccc` |
| 68 | common A01, required `01ce154c` for program24 depth write |
| 71..75 | D16 A10/A11/A12/A13/A20 |
| 92 / 94 / 96 / 97 / 100 | FVF A00/A02/A04/A05/A62 |
| 188 | VS A60 `60800005` |
| 192..195 | PS H2 header |
| 196..206 | A90..A9a exactly as above |
| 207..208 | program control `cccccccc dddddddd` |
| 209..211 | H2 alignment `cc000000` |
| 212..215 | TAU H2 header |
| 216..219 | `a01001b0 311001b1 cccccccc dddddddd` |
| 220..223 | ALU H2 header `fe020440 0000043c 20030000 cc000000` |
| 224..239 | the four ALUs above |
| 240..241 | program `cccccccc dddddddd` |
| 242..243 | H2 alignment `cc000000 cc000000` |
| 244..247 | immediate H2 header `fe020440 0000043c 00000000 cc000000` |
| 248..277 | three ten-dword vertices |

Within each vertex, S0/T0 are dwords 6/7 and S1/T1 are 8/9.  Thus the signed
stage-0 S words are body 254, 264, and 274.  Stage-0 T is 255/265/275;
stage-1 S/T are 256..257, 266..267, and 276..277.  Complete body length is
278 dwords.

The header/control distinctions matter: `cccccccc/dddddddd` are compiler
payload sentinels, while H2 alignment uses `cc000000`.  Substituting one for
the other changes executable packet data and is not harmless padding.

### Current-source A01 discrepancy

The current in-progress kernel builder does **not** yet emit the required
program24 A01 word.  It calls
`chrome9_depth_control_word(request, false)` and therefore produces
`01c0154c` at body 68 (`01c01548` for read-only); its no-depth initializer is
likewise `01c01540`.  The recovered lightall contract requires the
perspective-primary forms:

```text
program24 no depth / D16 write / D16 read-only
01ce1540       / 01ce154c  / 01ce1548
```

Selector 2 consumes normalized D1 primary color, so it must pass `true` to
the depth helper and set the same `ce` fields without depth.  The shorter
two-factor program may retain `c0` because it does not consume D1.  Add an
exact body[64/68] assertion keyed by the selected program.

The constant-primary vector above cannot distinguish `c0` from `ce`; it is
only the safe activation/wrap/program discriminator.  Follow it with a
varying-primary, unequal-RHW triangle after activation succeeds.  The current
`c0` word is already-valid hardware state, so this mismatch predicts wrong
interpolation rather than a command-reader or system hang.  It must still be
fixed before treating a q3 world frame as semantically valid.

The follow-up can remain a program24 plus signed-repeat test.  Reuse the
64x64 triangle from `raster-front-end-oracles.md`:

```text
v0 position=(-0.5,-0.5), rhw=1.0,  primary=80000000
v1 position=(63.5,-0.5), rhw=0.25, primary=80ff0000
v2 position=(63.5,63.5), rhw=0.25, primary=80ff0000
```

Use constant stage-0 S=`+31.109375`, T=`0.5`, with selected TEX0 texel opaque
white, and use a stage-1 texel whose RGB is white and alpha is zero.  At
target pixel `(48,16)` the perspective red fraction is `97/221`, while affine
interpolation gives `97/128`:

```text
required ce result: R = 255 * 97/221 = 111.9, A = 128
current c0 result:  R = 255 * 97/128 = 193.2, A = 128
```

Accept red near `6f..74` and require packed BGRA near `806f0000..80740000` for
the corrected path; `80c00000..80c40000` diagnoses the current affine word.
Green and blue remain zero, and alpha must remain exactly `80`.  Because S/T
are constant, this isolates D1 correction without confounding it with texture
coordinate interpolation, while still exercising cap40 acceptance and the
program24 object.

## Guard and retirement requirements

The initial command-13 oracle owns four distinct objects: BGRA target, TEX0,
TEX1, and D16.  Require canonical leading page, aligned image/depth data, and
trailing page for all four.  Before each draw:

- initialize every guard with the canonical guard word;
- initialize target data to a non-output sentinel;
- initialize D16 to `ffff`, submit a passing depth less than one, and record
  the expected covered D16 value; and
- retain source snapshots, including all non-addressed column sentinels.

After each draw require exact covered color and depth, untouched target
outside the half-open scissor, byte-identical sources, all eight guard pages
across the four BOs, expected cookie, CR paused, non-CR work mask clear, and
zero error status.  A timeout follows the existing quarantine and reboot
rule; do not replay the failed request through a shorter shader.

Negative prepublication cases are selector 2 without cap39, selector 2 with
XRGB stage 1, signed coordinate without cap40, magnitude above 32, signed
CLAMP/MIRROR, NaN/infinity, object alias, short allocation, and bad guard.

## Body-length and ring audit

The current builder uses a closed size assertion:

```text
body_dw = 262 + blend_delta + depth_delta + program_delta
blend_delta   = 0 or 4
depth_delta   = 0 or 4
program_delta = 0 or 8
```

Therefore selector-2 bodies are 270 (0x0a), 274 (0x0d), and 278 (0x13)
dwords.  The implementation also asserts the selected A92, both TAUs, first
and last program records, immediate header, and first vertex boundary.  The
signed-coordinate change copies different vertex data only and contributes
zero dwords.

Simulating the actual 16-byte H0/H2 alignment and 128-byte finish routine
gives:

| one-triangle body | program/state | PAUSE | STOP | `STOP+0x220` copy end |
|---:|---|---:|---:|---:|
| 262 | short, no depth | `4f0` | `570` | `790` |
| 266 | short + depth or blend | `4f0` | `570` | `790` |
| 270 | program24, no depth | `4f0` | `570` | `790` |
| 274 | program24 + depth | `4f0` | `570` | `790` |
| 278 | program24 + depth + blend | `4f0` | `570` | `790` |

The extra eight dwords consume alignment slack before the same 128-byte
PAUSE block.  `chrome9_append_segment()` consequently copies the same 0x790
bytes and advances `engine->append` by the same `pause+0x10 = 0x500` bytes.
Selector 2 cannot increase ring-wrap cadence or cross the 4-KiB staging-page
read-ahead boundary.

This excludes a length/framing explanation only.  It does not prove that the
new physical program cannot stall a shader unit.  If the combined oracle
hangs, capture cookie, `043c`, ring PAUSE/STOP, `0400/040c/0444`, and the ring
window.  A cookie miss/current pointer in the PS block implicates decode or
program activation; a cookie hit with non-CR work bits stuck implicates
shader/fetch retirement; a fully retired clean job shifts attention to the
next submission or CPU-side lifecycle.

## Command-15 exclusion and future batching bound

The current trees contain no `DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_RUN`, no
cap31 definition/advertisement, no ioctl-table entry, and no Mesa run
marshaller.  Current q3 triangles therefore use synchronous per-triangle
commands 0x0a/0x0d/0x13.  Command-15 batching cannot be the cause of a
current hang.  Confirm this operationally with ioctl/debug counters: any
observed private command 0x15 would prove a mismatched experimental binary.

For the future corrected command 15, selector 2 uses:

```text
body_dw = 240 + 4*BLEND + 4*DEPTH + 30*T
```

At T=18 with depth+blend, body is 788 dwords, PAUSE `0xcf0`, STOP `0xd70`,
and copy end `0xf90`; it remains inside one page.  T=19 is 818 dwords and
crosses to copy end `0x1010`, so it must be rejected.  If a future run builder
uses the legacy 232-dword fixed base for selector 2 or admits 19 triangles,
that is a real staging/read-ahead overrun capable of a hang.  The corrected
offset-36 program enum and program-keyed body calculation in
[ioquake3-signed-repeat-command15.md](ioquake3-signed-repeat-command15.md)
are mandatory before cap31 is advertised.

The persistent ring itself has already survived 1,272 state-complete appends
and 28 wraps.  A q3-only sustained hang should therefore first be separated
into (a) selector-2 execution/state transition, (b) a distinct texture/depth
resource or cache transition, or (c) CPU/kernel deadlock.  Packet length and
inactive command-15 batching are not evidence-backed causes in the current
build.
