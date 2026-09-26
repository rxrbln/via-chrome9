# Shipped ioquake3 GL1 material census: ADD versus additive blend

Status: source and installed game-data audit, a bounded cmd22 saturation
oracle, and a live q3dm17 shader capture on VX900, 2026-09-24.  This is not
yet a framebuffer-pixel qualification of the whole material.  The latest coherent q3dm1
gameplay trace reached the deliberate 32,768-triangle cap with no shader or
draw rejection; no missing q3dm1 fragment class is established by that run.

## q3dm1: no classic texture-env ADD candidate

The installed `/root/.q3a/baseq3/pak0.pk3` q3dm1 BSP has 94 shader names
and 2,097 draw surfaces.  Eight material names have additive *blend* stages:

| Material | BSP surfaces | Why this is not evidence of GL_ADD texture env |
|---|---:|---|
| `textures/gothic_block/largerblock3blood` | 1 | First two stages omit `rgbGen`; `ParseStage` defaults GL_ONE sources to `CGEN_IDENTITY_LIGHTING`, while ADD collapse requires `CGEN_IDENTITY`. |
| `textures/gothic_light/pentagram_light1_1K` | 4 | First pair is opaque then `DST_COLOR,ZERO`, a MODULATE collapse; third pass is `ONE,ONE`. |
| `textures/sfx/computer_blocks17` | 1 | First pair is opaque then `DST_COLOR,ZERO`; third animated pass is `ONE,ONE`. |
| `models/mapobjects/wallhead/lion_m` | 2 | First stage uses vertex RGB; second additive stage has default identity-lighting, so the generators differ. |
| `textures/sfx/flame1_hell`, `flame1side`, `flame2` | 1, 4, 6 | Animated additive blend passes use different wave RGB generators. |
| `textures/skies/tim_hell` | 16 | Both first stages default to identity-lighting, and sky has a separate iterator. |

Intersecting all q3dm1 BSP material names with the strict ADD-collapse
candidates from the shipped shader scripts gives the empty set.  The
`GL_DECAL` collapse row is disabled in the ioquake3 source with `#if 0`
(`renderergl1/tr_shader.c:1796`).  `DrawMultitextured` selects `GL_REPLACE`
only for the `r_lightmap` diagnostic; normal gameplay uses its material mode
(`tr_shade.c:375..381`).  Consequently implementing ADD, DECAL, or a new
REPLACE variant solely to repair the observed q3dm1 frame would be
speculative.  Additive framebuffer blending is a distinct, real q3dm1 path.

The source audit uses ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`:
`ParseStage()` defaults at `tr_shader.c:1053..1060` and converts an explicit
`ONE,ZERO` blend to opaque at `:1067..1073`; `CollapseMultitexture()` maps
opaque+`ONE,ONE` to `GL_ADD` at `:1790..1795`, requires matching generators
and `CGEN_IDENTITY` at `:1857..1870`, and is called at `:2356`.

## First concrete other-map GL_ADD target

`pak0.pk3/maps/q3dm17.bsp` references
`textures/base_wall/metalfloor_wall_15ow` on **two** draw surfaces.  The
two four-vertex surface centroids are `(288, 672, 328)` and
`(288, -544, 328)` in map coordinates; deathmatch spawns occur near both,
so an early view may include the material.  The
shipped shader has:

```text
stage 0: metalfloor_wall_15ow.tga; ONE,ZERO; rgbGen identity
stage 1: hologirl.jpg; ONE,ONE; rgbGen identity; tcMod scale/scroll
stage 2: metalfloor_wall_15ow.tga; SRC_ALPHA,ONE_MINUS_SRC_ALPHA
stage 3: $lightmap; DST_COLOR,ZERO
```

The first pair satisfies the source's ADD-collapse conditions; the remaining
two passes remain separate.  Stage 0's uncompressed 256x256 32-bit TGA has
2,600 non-opaque alpha texels and therefore takes ioquake's GL_RGBA8/BGRA
upload path.  Stage 1 is JPEG and takes GL_RGB8/XRGB.  The stage-1 classic
ADD result is therefore:

```text
previous = T0 * primary               # unit-0 MODULATE
out.rgb  = saturate(previous.rgb + T1.rgb)
out.a    = previous.a                 # RGB texture has no API alpha
```

Do not substitute a four-lane ADD: it would incorrectly add alpha.  Also do
not infer that a 32-bit TGA is opaque from its header; ioquake scans pixel
alpha when selecting GL_RGB8 or GL_RGBA8.

Mesa 26.2.2 `main/ff_fragment_shader.c:650..713` constructs a stage-local
`nir_fsat` around the RGB `fadd`, then assembles alpha separately.  The live
linked NIR, however, lowers that saturation to `fmax(sum, 0)` followed by
`fmin(..., 1)`.  Chrome9's generic compiler accepts these exact scalar/vector
operations and produces a supported native program.  The separately added
`nir_op_fsat` lowering remains useful for NIR that retains the intrinsic,
but was **not** the route exercised by this q3dm17 capture.  The fragment IR
and cmd22 encoder carry destination saturation
(`chrome9_shader_offline.c:62..71`, `chrome9_fragment.c:328..329`).  The Linux
and Windows compiler-exact ADD records are archived in
[`ioquake-texture-environment-combine.md`](ioquake-texture-environment-combine.md),
but this full fixed-function key has not been captured or executed.

The minimal safe lowering for an unlowered `fsat` is a separate native `MOV`
with destination SAT, retaining a new virtual result rather than changing a
shared producer.  It must preserve the node's vector width and feed only the
RGB lanes where Mesa emitted saturation.  Folding SAT into a producer's
destination can be considered only when dominance/use analysis proves the
producer has no unsaturated consumer.
An isolated oracle should use normalized inputs
`primary=(.5,.75,.25,.5)`, `T0=(.5,.5,.5,1)`,
`T1=(.875,.25,.5,.25)`: RGB must become `(1,.625,.625)`, and the RGB second
texture must preserve alpha `.5`; a hypothetical RGBA second texture would
instead yield alpha `.125`.  Keep the q3dm17 texture/upload alpha scan in the
test, since the actual first source has non-opaque texels.

The separate guarded cmd22 oracle
`tools/re/chrome9_gl1_add_sat_guard_smoke.c` ran successfully on the VX900
under `/tmp/chrome9-vx900-gpu.lock`.  It submits two triangles using BGRA T0,
XRGB T1, primary-color MUL, full-vector ADD with T1 alpha explicitly zero,
SUB to make G negative, a separate full-vector `MOV.SAT` TEMP, a post-SAT ADD,
and one terminal COLOR MOV.  This post-SAT arithmetic distinguishes both
native saturation bounds from final UNORM target clamping.  All nine sampled
interior pixels matched BGRA `0x80bf40a0` within two channel LSBs; exterior
pixels stayed zero, both texture BO snapshots and all guard pages were
unchanged, and the immediate kernel log showed no GPU fault/timeout.  This
qualifies the saturation *instruction* and the BGRA/XRGB alpha contract, not
the complete q3dm17 material's framebuffer, sampler, or depth behavior.

## Bounded q3dm17 runtime capture

The distinct-saturation Mesa build (library SHA-256
`580e8f0aeaa6710d8f78892285c0c73650d79d884f1928a0a98459503a5de3e7`)
was staged at `/root/tmp/chrome9-live/ioquake-addsat-20260924/` and run with
`CHROME9_DRAW_LIMIT=32768`, `CHROME9_DEBUG_SHADERS=1`, and
`CHROME9_DUMP_NIR=1` under the shared GPU lock.  The retained trace is
`gl1-q3dm17-addsat-limit32768.log`, SHA-256
`0189284efa9f7efc447a6d7a7b2fbbe6cb844a2fbd45b78e9247fa45e26b284f`.
The game entered q3dm17 and hit exactly the deliberate triangle limit; there
were no draw rejections, timeouts, quarantine records, or new kernel faults.

The live fixed-function fragment NIR was:

```text
T0 = projected_tex(0, TEX0.xy, TEX0.w)
T1 = projected_tex(1, TEX1.xy, TEX1.w)
P  = T0 * COL0
S  = T1 + P
U  = max(S, 0)
V  = min(U, 1)
out = vec4(V.rgb, P.a)
```

It compiled as `program=28 supported=1 semantic=1 native=1 nir=1`, with
9 semantic instructions, two TAUs and seven ALUs; the game bound that shader
218 times during the bounded run.  This is strong compiler and live draw-state
admission evidence for the ADD material, but not a per-pixel proof of its
visible surface.  A subsequent bounded early-frame X capture is retained on
target as `gl1-q3dm17-addsat-earlyshot.png` and locally at
`artifacts/2026-09-24/gl1-q3dm17-addsat-earlyshot.png` (SHA-256
`6ce4e7d286cd21ae22fb685761d583f4b54b07bebd4d2d76d403088d7aaf825b`).
It shows a 640x480 floor and HUD within the 1024x768 desktop, but substantial
black void/missing world geometry.  The particular two ADD surfaces cannot
be identified from this frame.  The log records player entry at line 335 and
the 32,768-triangle cap at line 682; the capture was triggered two seconds
after entry, so it may have occurred *after* the cap and may show a partially
drawn frame.  It is not evidence of a complete or correct q3dm17 image.  The
run had no rejected draw or new kernel fault.  A camera-positioned uncapped
reference or isolated material-pixel capture remains the next correctness
check.  The
runtime also warned that installed `pak0.pk3`
has a noncanonical checksum; this audit describes the *installed* archive,
not a claim that it is byte-identical to a retail CD.

A follow-up capture started **before** the game and recorded 24 full-screen
PNGs at 2 fps with the same 32,768-triangle cap.  The entire sequence and
timing logs are in `artifacts/2026-09-24/q3dm17-firstframe-seq/`, with target
originals under `q3dm17-firstframe-seq/` beside the trace.  The cap was
detected at Unix time `1790238308.604`; `frame-20.png` and `frame-24.png`
were written approximately 2.87 and 1.67 seconds before that, respectively.
Both show the gameplay HUD, floor, distant platform and sky bands, but also
substantial black space.  Therefore a post-cap partial frame is **not** a
sufficient explanation for the black space.  Quake III's q3dm17 is a floating
platform map, so this may be intentional void/sky; a software-rendered or
known-good reference is required before classifying it as a driver defect.
Neither pre-cap frame singles out the two GL_ADD material surfaces.

`tools/re/ioquake_gl1_material_census.py` is a read-only source-candidate
filter with self-tests for the identity-lighting trap and independent RGB/A
ADD vector.  Example:

```sh
python3 tools/re/ioquake_gl1_material_census.py --self-test
ssh root@192.168.2.60 "unzip -p /root/.q3a/baseq3/pak0.pk3 'scripts/*.shader'" |
  python3 tools/re/ioquake_gl1_material_census.py \
    --material textures/base_wall/metalfloor_wall_15ow
```

This filter handles stage generation/blend state only; it is intentionally
not a renderer trace.  The bounded live trace now proves the linked NIR and
an actual ADD-program bind.  Remaining work is to compare framebuffer pixels
at a known visible material surface, verify post-TNL projector guards for
that draw, and test later source-over/filter passes.  If the stage is outside
the viewport at the spawn point, a script-specific forced camera or isolated
Gallium test is needed; a BSP reference or shader bind alone does not prove a
visible fragment.
