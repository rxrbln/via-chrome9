# ioquake3 alpha, depth and blend on generic command 22

Status: VX900 bounded silicon oracle, 2026-09-24. This note concerns fixed
fragment state and the Q=1-lowered one-texture program. It does not claim an
uncapped ioquake3 gameplay frame or a general projective sampler.

## Exact workload and packet semantics

The shipped `pak0.pk3` shader-stage census in
`ioquake3-blend-alpha-matrix.md` identifies GE128 masked base passes with
LEQUAL/write depth and follow-up `DST_COLOR,ZERO` EQUAL/read-only lightmap
passes. GL1 2-D and fog passes call `glBlendFunc(SRC_ALPHA,
ONE_MINUS_SRC_ALPHA)`, which applies the same factors to RGB and alpha.
The observable BGRA alpha equation is therefore
`As*As + Ad*(1-As)`, not separate-alpha Porter-Duff source-over.

The Windows D3D/OpenGL factor tables at `0x101fd9e0`/`0x10251220` and
OpenGL alpha selector at `0x194ad0` agree with the Chrome9 UAPI mapping
already recorded in `ioquake3-blend-alpha-matrix.md`. The current kernel
`chrome9_lower_blend()` emits independent A00, A33, A34--A37 and A53 words.
For GE128, A33 is `33006202`; A00 bit 20 enables alpha test. For same-factor
GL1 source-over, A34--A37 are `34000020 35000148 36000410 37000244`.
The generic builder emits A52 format 99 for BGRA and format 98 for XRGB,
and independently selects D16 EQUAL/read-only as A01 `01ce1548` and A13
`13020000`. No A7B draw-time register is needed.

## Isolated VX900 proof

`tools/re/chrome9_alpha_depth_blend_guard_smoke.c` uses canonical guarded
64×64 BGRA/XRGB destination, guarded D16, and a distinct guarded 2×2 BGRA
texture. Every draw is two triangles and uses the semantic command-22 ABI;
the kernel compiles native instructions and owns packet state. The test was
built with strict warnings and ran to exit zero on the target at
`192.168.2.60`. Its binary was staged at
`/root/tmp/chrome9-live/ioquake-projector-20260924/chrome9-alpha-depth-blend-guard-smoke`.

The verified sequence was:

1. GE128 rejected a near alpha-`0x40` PRIMARY fragment without writing
   color or D16. A farther alpha-`0x80` fragment then passed LEQUAL and wrote
   both color and depth.
2. With BGRA target initially `0000ff00`, PRIMARY `80ff0000` and GL1
   same-factor source-over, the covered pixel matched `40807f00` within one
   8-bit LSB. This distinguishes the GL1 alpha equation from canonical
   separate-alpha source-over, which would yield alpha near `0x80`.
3. GE128, EQUAL/read-only D16 and ONE/ONE blending worked in one packet:
   the accepted equal-depth half matched `a080a0d0`; the unequal half,
   exterior and both depth values remained unchanged.
4. The exact Q=1-lowered `TEX s0,T0.xy; MUL oC0,s0,PRIMARY` program sampled
   BGRA `80ff0000`. Its no-blend baseline matched the texel; GL1
   source-over then matched `40807f00` on BGRA and RGB `00807f00` on XRGB.
   A subsequent no-blend draw passed, proving this bounded state transition.
   Texture bytes and all three BO guards remained intact.
5. With an XRGB dlight source `00804020`, white primary, BGRA destination
   `204080c0` and EQUAL/read-only D16, ONE/ONE produced `ffc0c0e0` on the
   equal-depth half. `DST_COLOR,ONE`, with DST_ALPHA as the alpha source
   factor, produced `4060a0d8`. The unequal-depth half and both depth values
   stayed unchanged.
6. With BGRA fog source `80ffffff` and primary `ff804020`, same-factor
   source-over produced `50606070` with EQUAL/read-only D16 on the equal
   half. Switching to LEQUAL/read-only produced the same pixel on both
   halves, without changing depth. Source bytes and all guards remained
   intact.
7. The q3dm1 masked two-stage state was isolated with a sampled BGRA base:
   alpha `0x40` failed GE128 without writing color or D16; alpha `0x80`
   passed and wrote both. A sampled XRGB follow-up with `DST_COLOR,ZERO`
   and EQUAL/read-only D16 produced `80800000` on the equal-depth half only.
   The unequal half, texture source bytes and all guards remained unchanged.

The target's Xorg process remained alive and the post-test kernel log had no
Chrome9 fault, timeout or quarantine line. This closes the *generic*
TEX×PRIMARY plus source-over pixel/retirement boundary, the isolated
alpha/depth/blend composition, and the tested dynamic-light and fog tuples.
It does not qualify the older fixed MODULATE
list packet that previously timed out at shutdown; that remains gated.
Likewise, command 22 receives direct T0.xy, not a projector operand. Mesa's
separate post-TNL raw-Q-equals-one preflight must remain in force for each
projected ioquake3 draw.

## Remaining game-specific combinations

The tested masked two-stage sequence closes the fixed state and sampled
pixel equations for a constant-texel version of q3dm1's banner; it does not
exercise the asset's actual image coordinates or vertex deformation. The
dynamic-light/fog tests likewise use small constant images to isolate their
exact state tuples. They do not substitute for the application's 16×16 XRGB dlight
and 256×32 BGRA fog images, LINEAR filtering, or actual per-vertex
coordinates. Those sampler dimensions and data patterns remain application
validation work before claiming full gameplay correctness.

## Gameplay screenshot boundary

The cap-32768 GL1 run reached q3dm1 gameplay with all observed shader
classes admitted and no reported GPU fault. Its late screenshot
`gl1-limit32768-fps5-game.png` shows a large red wedge, but the log reports
`stopped before triangle=32768` at line 601 and then continues for over
3,300 shader-bind lines with draws suppressed. A separate early screenshot
`/tmp/gl1-limit32768-earlyshot.png`, captured before the global diagnostic
cap truncated a frame, shows coherent courtyard, sky, statues, weapon and
HUD. The late wedge is therefore a cap-truncated frame, not evidence for a
near-plane, viewport or raster geometry defect. Do not change the clipping
or interpolation path based on that late image.
