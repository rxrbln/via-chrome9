# ioquake3 GL1 blend and alpha-test state matrix

Status: renderer/source, shipped-asset, live-log, Mesa, UAPI, kernel and
vendor-packet audit, 2026-09-21. This document describes fixed-function state
after the texture program has produced a fragment. It does not imply that the
corresponding texture shader, mip state or depth mode is already executable.

## Workload evidence

The renderer source is ioquake3 revision
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`. The tested machine's shipped
archive is:

```text
/root/.q3a/baseq3/pak0.pk3
size   46853694
sha256 e77abad2466f45a0a7ea018445528f9b95a0fe7789fa1abc1a7718bbf0754b08
```

A brace-aware scan of every `scripts/*.shader` stage, with `add`, `filter`
and `blend` expanded as ioquake's parser expands them, gives these dominant
tuples. Counts describe archive stages, not one frame's draw count.

| stages | blend | alpha test | depth func | explicit depth write |
|---:|---|---|---|---|
| 862 | ONE, ONE | disabled | LEQUAL | no |
| 701 | disabled | disabled | LEQUAL | no |
| 543 | DST_COLOR, ZERO | disabled | LEQUAL | no |
| 286 | SRC_ALPHA, INV_SRC_ALPHA | disabled | LEQUAL | no |
| 109 | ONE, ZERO | disabled | LEQUAL | no |
| 103 | DST_COLOR, SRC_ALPHA | disabled | LEQUAL | no |
| 57 | DST_COLOR, INV_DST_ALPHA | disabled | LEQUAL | no |
| 55 | DST_COLOR, ONE | disabled | LEQUAL | no |
| 49 | disabled | GEQUAL, 0.5 | LEQUAL | yes |
| 48 | DST_COLOR, ZERO | disabled | EQUAL | no |
| 29 | ONE, ZERO | GEQUAL, 0.5 | LEQUAL | yes |
| 28 | INV_SRC_ALPHA, SRC_ALPHA | disabled | LEQUAL | no |
| 19 | ONE, SRC_COLOR | disabled | LEQUAL | no |
| 14 | disabled | GEQUAL, 0.5 | LEQUAL | implicit |
| 14 | DST_COLOR, SRC_COLOR | disabled | LEQUAL | no |
| 13 | ONE, ONE | disabled | EQUAL | no |

`ONE,ZERO` is special: `ParseStage()` removes those blend bits and restores
depth writes, so it reaches Gallium as ordinary disabled blending. For every
other explicit blend, the parser clears the depth mask unless `depthWrite`
was explicitly present. Most of the high-count blend rows therefore require
read-only D16 even though the shader script does not say `depthwrite 0`.

The three alpha-test directives map exactly as follows:

| ioquake directive | GL compare/reference | A33 |
|---|---|---:|
| `GT0` | GREATER, 0 | `33004000` |
| `LT128` | LESS, 0.5 -> byte 128 | `33001202` |
| `GE128` | GEQUAL, 0.5 -> byte 128 | `33006202` |

Representative masked assets include `textures/base_floor/cybergrate`,
`textures/base_floor/proto_grate*`, model grates/chains and player cutouts.
The common two-pass grate form writes an alpha-tested base at LEQUAL, then
uses `DST_COLOR,ZERO` with EQUAL/read-only depth. This means alpha+D16 and
read-only EQUAL blending are both ordinary map state, not synthetic tests.

Renderer-owned passes add two more exact combinations:

* `ProjectDlightTexture_scalar()` uses one sampled texture times primary
  color, EQUAL/read-only D16, and either `ONE,ONE` or `DST_COLOR,ONE`;
* `RB_FogPass()` uses one sampled texture times primary fog color,
  `SRC_ALPHA,INV_SRC_ALPHA`, read-only depth, and either EQUAL or LEQUAL.

The 2-D path is the important already-reached exception. `RB_SetGL2D()`
disables depth and selects `SRC_ALPHA,INV_SRC_ALPHA`. The live log
`ioquake3-gl1-projected-modulate-v1.log` reached
`reason=projected-source-format` with XRGB target, no depth/DSA and
`blend_successor=1`. Since blend validation precedes source-format
validation, that draw's blend tuple was accepted by the current translator.

## Exact Chrome9 words for the common rows

Windows D3D `0x101fd9e0`, its byte-matched OpenGL copy `0x10251220`, Linux
`STM_AlphaBlend_INV:0x196e30`, and the manual agree on the factor table.
Legacy UniChrome `via_state.c` independently maps the same GL factors and
keeps alpha test separate; it is a semantic cross-check, not the Chrome9
packet source.

The specific audit anchors are:

* ioquake `code/renderer/tr_shader.c:ParseStage()` for stage normalization,
  `code/renderer/tr_backend.c:GL_State()` for API calls, and the same file's
  `RB_SetGL2D()`, `ProjectDlightTexture_scalar()` and `RB_FogPass()` for the
  renderer-owned tuples;
* Mesa 7.11.2
  `src/mesa/drivers/dri/unichrome/via_state.c` for the older VIA factor and
  alpha-compare mappings;
* Windows D3D `0x101fd9e0` and OpenGL `0x10251220` for the byte-identical
  Chrome9 factor-selector switch, plus OpenGL `STM_AlphaFunc_INV:0x194ad0`
  for A33; and
* the live target log
  `/root/tmp/chrome9/ioquake3-gl1-projected-modulate-v1.log` for the accepted
  depthless blend boundary.

For the ioquake GL1 call `glBlendFunc(src,dst)`, the same API factors apply
to RGB and alpha. In the alpha equation, SRC_COLOR aliases SRC_ALPHA and
DST_COLOR aliases DST_ALPHA. The important clamped ADD rows are:

| ioquake blend | A34 | A35 | A36 | A37 | current enum support |
|---|---:|---:|---:|---:|---|
| disabled/COPY ONE,ZERO | `34000150` | `35000114` | `36000590` | `37000224` | yes |
| ONE,ONE | `34000150` | `35000154` | `36000590` | `37000264` | yes |
| SRC_ALPHA,INV_SRC_ALPHA | `34000020` | `35000148` | `36000410` | `37000244` | yes |
| INV_SRC_ALPHA,SRC_ALPHA | `34000120` | `35000108` | `36000510` | `37000204` | yes |
| DST_COLOR,ZERO | `34000010` | `35000114` | `36000420` | `37000224` | no |
| DST_COLOR,ONE | `34000010` | `35000154` | `36000420` | `37000264` | no |
| DST_COLOR,SRC_ALPHA | `34000010` | `35000108` | `36000420` | `37000204` | no |
| DST_COLOR,INV_DST_ALPHA | `34000010` | `3500014c` | `36000420` | `37000248` | no |

The ioquake source-over alpha row is intentionally `36000410`, not the
canonical Porter-Duff row `36000590`. `glBlendFunc(SRC_ALPHA,
ONE_MINUS_SRC_ALPHA)` computes:

```text
Aout = As*As + Ad*(1-As)
```

The current `DRM_CHROME9_TEXTURED_D16_SOURCE_OVER` shortcut instead requires
alpha factors `ONE,INV_SRC_ALPHA`. It is exact for glamor's separate-alpha
state, but it does not equal ioquake GL1 on a BGRA target. It can be an
XRGB-only optimization because format 98 has no observable destination alpha
and supplies one when read, but it must not silently stand in for the GL
tuple on format 99.

Every blend-aware body must close A38--A3F to zero until nonzero blend color
is exposed. ZERO/ONE share the native constant selectors with
CONST/INV_CONST; inherited nonzero factor constants otherwise corrupt even
the COPY and ONE/ONE rows.

## Flags, A00, A33 and packet slots

The existing normalized blend state has two independent flags:

```text
DRM_CHROME9_BLEND_ENABLE             bit 0
DRM_CHROME9_BLEND_ALPHA_TEST_ENABLE  bit 1
```

The kernel lowers them independently:

```text
disabled                 A00 = 00000001
blend                    A00 = 00000011
alpha test               A00 = 00100001
blend + alpha test       A00 = 00100011
```

Alpha-test-only state must leave the blend flag clear. A33 holds the compare
and reference and consumes a pre-existing aligned slot, so alpha test does
not grow the packet. In a closed textured destination block (`B=4`), use
`body[19]` for A33. This slot remains available with D16 and avoids the
current one-texture builder collision in which `common[7]` first holds A33
and is then overwritten by depth A10. Never fix that collision by dropping
A10 or by shifting unchecked native words.

For the current compact one-texture body, blend closure adds four dwords and
D16 adds four. The state-complete MODULATE body therefore remains:

```text
228 + B + D dwords
body[19]          A33 or cc000000 when B=4
body[42+B]        A53
body[43+B]        A00
body[44+B]        A01
body[47+B..51+B]  A10,A11,A12,A13,A20 when D16
```

Two-texture packets have the same self-addressed A33/A00 semantics. Their
existing no-blend and D16 ABIs deliberately reject all blend and alpha-test
state, so a new state-complete selector must not reinterpret reserved dwords
without a capability.

## A52 destination format boundary

A52 is part of blend correctness, not just storage bookkeeping:

```text
BGRX/XRGB  A52 = 52980000 | (pitch >> 5)  # ARGB0888, read alpha = 1
BGRA/ARGB  A52 = 52990000 | (pitch >> 5)  # ARGB8888, stored alpha
```

The manual explicitly says a format with no alpha channel supplies 1.0 as
alpha. Therefore `DST_ALPHA` and `INV_DST_ALPHA` reduce to ONE and ZERO on
format 98, but must read stored destination alpha on format 99. Alpha test is
performed on fragment alpha before the render-target store and works with
either A52 value. Do not require BGRA merely for alpha test.

The target choice also controls what may be simplified:

* same-RGB but different alpha equations may alias only on format 98;
* any destination-alpha blend needs the actual drawable format in A52;
* a format-99 draw requires the BGRA capability even with blend disabled;
* two-texture current paths reject format 99 because setting BGRA currently
  selects a successor they do not implement.

## A7B is not blend state

There is no A7B requirement for any blend or alpha-test draw. Three similarly
named register spaces must not be conflated:

1. type-01 subaddresses A79--A7F, including A7B, are reserved fog slots in
   the VX900 Chrome9 HD 3-D manual;
2. HParaType 10h subaddress 7Bh in the VX855/VX875 HCM manual is global
   `CMDQ Setting 9`, the queue length in 128-bit units; and
3. VGA sequencer index SR7B is saved/programmed/restored by Chrome9 engine
   initialization and is not a type-01 command-stream word.

Per-draw emission of `7bxxxxxx` would therefore be unsupported at best and a
queue-control hazard at worst. Kernel-generated blend packets must touch
A00, A33--A3F, A52/A53 and the already-closed raster/depth state only. Ring
initialization owns command-queue length and sequencer state.

## Current acceptance matrix and exact gaps

The current Mesa/kernel pair supports only ADD and factors ZERO, ONE,
SRC_ALPHA and INV_SRC_ALPHA. Generic no-depth untextured/textured successors
can combine those factors, channel masks and A33 alpha test. Untextured D16
blend has the same normalized state but still requires write-enabled depth.

| requested state | current result | exact reason |
|---|---|---|
| 2-D texture, same-factor source-over, no depth | accepted | generic textured blend successor |
| ONE,ONE, no depth | accepted | all four factors are ONE, ADD |
| masked one-texture + D16 LEQUAL/write | rejected | alpha flag makes the narrow textured-D16 source-over test fail |
| masked two-texture + D16 | rejected | all two-texture successors reject `blend_successor` |
| any common blended world stage | rejected | parser makes depth read-only; current D16 validation requires writes |
| dynamic light/fog EQUAL | rejected | only LESS/LEQUAL and write-enabled D16 are admitted |
| DST_COLOR filter/dlight rows | rejected | factor enum/lowering stops at INV_SRC_ALPHA |
| ioquake source-over + textured D16 | rejected | special shortcut requires alpha ONE, not alpha SRC_ALPHA |
| two textures into BGRA | rejected | BGRA forces an unsupported successor in the two-texture gate |
| alpha test without depth | accepted if fragment alpha is defined | generic blend successor carries A33 |

Advertising `caps.alpha_test` is necessary to prevent state-tracker lowering
to unsupported discard NIR, but it is not sufficient for these combinations.
The actual draw gate must accept the complete texture/depth/blend tuple.

## Minimal implementation tranches

### 1. Masked opaque texture + D16

This is the safest map-visible extension. Add an explicit alpha-test flag and
function/reference fields to the one- and two-texture D16 structured ABIs,
capability-gated and backward-compatible. The one-texture structure has three
reserved dwords; function, byte reference and one still-zero reserved dword
fit exactly. The two-texture structure has five reserved dwords. Kernel owns
A00/A33 and places A33 at body 19 after closing A38--A3F. Blend remains
disabled, color mask full, depth write enabled, and depth function LESS or
LEQUAL.

### 2. Read-only depth and EQUAL

Add an explicit read-only flag plus capability-gated EQUAL compare. A01 must
be `...1548` for test/read-only instead of `...154c`; A13 is `13020000` for
EQUAL. This unlocks the depth half of nearly every blended world, dynamic
light and fog tuple. It changes no BO: the depth object still receives a
WRITE reservation conservatively until read-only fencing is separately
proved safe.

### 3. Nonconstant blend factors

Extend the existing factor enum with SRC_COLOR/INV_SRC_COLOR, DST_COLOR/
INV_DST_COLOR, DST_ALPHA/INV_DST_ALPHA and SRC_ALPHA_SATURATE. The struct and
packet size do not change. Promote DST_COLOR,ZERO first because it covers 543
archive stages and the EQUAL grate follow-up; then DST_COLOR,ONE for dynamic
lights. Use the exact factor sweeps in `blend-alpha-packet-recipes.md` before
accepting the rarer rows.

### 4. Full textured-D16 blend successor

Arbitrary blend plus depth cannot fit the narrow source-over flag. Add a new
structured one-texture-D16 blend ioctl containing the existing
`drm_chrome9_blend_state`, destination format and depth flags. Do the same for
two textures only after the four-BO non-blend path is stable. BO lookup,
guards, fences and quarantine remain those of the corresponding D16 draw;
only kernel-generated state grows.

## Guarded smoke vectors

1. **GE128 kill before depth.** Clear D16 to `ffff` and XRGB to blue. Draw a
   near red texel with alpha `40`, GEQUAL/ref `80`; it must be killed. Draw a
   farther green texel with alpha `80`; expect green and its depth. This proves
   a killed fragment writes neither color nor depth.
2. **Exact ioquake source-over alpha.** On BGRA prefilled transparent green,
   draw half-alpha red with `SRC_ALPHA,INV_SRC_ALPHA` for both equations.
   Expect approximately `40807f00`. Canonical separate-alpha source-over
   instead produces `80807f00` and is a useful negative discriminator.
3. **Filter.** On XRGB destination `004080c0`, draw `00804020` with
   `DST_COLOR,ZERO`; expect approximately `00202018`.
4. **Additive read-only.** Prefill depth `8000`, submit equal-depth ONE,ONE,
   verify the color sum and that depth remains `8000`; then transition to an
   opaque LEQUAL/write draw to prove A01/A13 closure.
5. **Alpha plus blend ordering.** Enable GEQUAL/ref `80` and ONE,ONE. An
   alpha-`40` fragment must not contribute color or depth; an alpha-`80`
   fragment must. This catches a post-blend alpha test.
6. **Format closure.** Run the same destination-alpha factor on format 99,
   format 98, then format 99 without context reset. The two 99 results must
   match, while format 98 must behave as destination alpha one.

Every oracle uses separate guarded target/texture/depth BOs, checks every
guard after retirement, records submitted/rejected deltas and verifies cookie,
CR and work status. Follow every new state by disabled COPY and never emit
A7B as part of recovery or transition closure.
