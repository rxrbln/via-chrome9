# Shipped ioquake3 GL1 material census: ADD versus additive blend

Status: source and original game-data audit plus a bounded cmd22 saturation
oracle on VX900, 2026-09-24.  This is not a live q3dm17 shader capture or
full material qualification.  The latest coherent q3dm1
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

Mesa 26.2.2 `main/ff_fragment_shader.c:650..713` emits a stage-local
`nir_fsat` around the RGB `fadd`, then assembles alpha separately.  Chrome9's
generic NIR lowering in `chrome9_context.c:3496..3554` accepts `fadd`, but
does not currently lower `nir_op_fsat`.  The fragment IR and cmd22 encoder
already carry destination saturation (`chrome9_shader_offline.c:62..71`,
`chrome9_fragment.c:328..329`).  The Linux and Windows compiler-exact ADD
records are archived in
[`ioquake-texture-environment-combine.md`](ioquake-texture-environment-combine.md),
but this full fixed-function key has not been captured or executed.

The minimal safe lowering is a separate native `MOV` with destination SAT
for an `fsat` node, retaining a new virtual result rather than changing a
shared producer.  It must preserve the node's vector width and feed only the
RGB lanes where Mesa emitted saturation.  After a real q3dm17 linked NIR
capture, folding SAT into the producer's destination can be considered only
when dominance/use analysis proves the producer has no unsaturated consumer.
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
yet the entire q3dm17 shader's NIR lowering or runtime sampler/depth state.

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
not a renderer trace.  Before code admission, run a bounded `+devmap q3dm17`
capture under the shared GPU lock, record the linked NIR, verify an actual
ADD bind and post-TNL projector guards, and compare framebuffer/depth/guard
pixels against the reference.  If the stage is outside the viewport at the
spawn point, a script-specific forced camera or isolated Gallium test is
needed; a BSP reference alone does not prove a visible fragment.
