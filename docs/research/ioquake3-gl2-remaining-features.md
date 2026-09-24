# ioquake3 GL2 remaining features after the lightmap `.zw` bridge

Status: read-only forward analysis, 2026-09-24.  Nothing was run on the
VX900 target and no source tree was changed.  Inputs: ioquake3
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1` (T2 tree), Mesa chrome9 HEAD
`ad64c2f`, kernel chrome9 HEAD `2541417`, the shipped
`/tmp/pak0.pk3` (SHA-256 `e77abad2...4b08`, see
[`ioquake3-q3dm1-mip-envelope.md`](ioquake3-q3dm1-mip-envelope.md)) and the
PNG captures in `artifacts/2026-09-24/render-status/`.  It extends
[`ioquake3-blocker-roadmap-re.md`](ioquake3-blocker-roadmap-re.md) (G1..G11)
and [`chrome9-render-status-20260924.md`](chrome9-render-status-20260924.md).

Evidence labels: **source** (code or shader text at the cited line),
**asset** (parsed from the pk3/BSP), **capture** (pixel statistics measured
from the committed PNGs on this host), **inference** (reasoned, not measured).

Paths: `Q=code/renderergl2` of the ioquake3 tree,
`C=src/gallium/drivers/chrome9` of the Mesa tree,
`K=drivers/gpu/drm/chrome9` of the kernel tree.

## 0. Findings that change the plan

1. **The llvmpipe reference is not the same GL2 path (source).**  The
   capture scripts only set `MESA_GL_VERSION_OVERRIDE=2.1`
   (`tools/chrome9_q3shot.sh:15`).  That does not remove
   `GL_ARB_framebuffer_object` or `GL_ARB_texture_float`: both are
   `GLL` entries with no minimum version (`src/mesa/main/extensions_table.h:82,169`,
   filter at `extensions.c:322`).  ioquake3 therefore takes its FBO path on
   llvmpipe (`$Q/tr_extensions.c:59-80,107-114`): RGBA16F render target
   without per-pass clamping, destination alpha, `glGenerateMipmap` instead of
   the CPU gamma-correct mips (`$Q/tr_image.c:1983-1994`), and the tonemap
   plus auto-exposure post pass (`$Q/tr_backend.c:1465-1525`,
   `glsl/tonemap_fp.glsl`).  Chrome9 (no `mixed_framebuffer_sizes`) runs the
   no-FBO path.  Brightness/tint comparisons of the two panels are not
   meaningful until the reference runs with `+set r_ext_framebuffer_object 0`
   (confirm `...GL_ARB_framebuffer_object ignored` in the log).
2. **The additive sky layer is missing on hardware (capture + asset).**
   `killsky_1.jpg` averages RGB (52.1, 0.4, 0.4) and `killsky_2.jpg`
   (19.8, 7.0, 1.1).  The upper sky in the current GL2 hardware panel averages
   (44.5, 0.3, 0.2) with near-zero green/blue: only the first cloud stage
   contributes.  The pre-`ad64c2f` GL2 capture (71.3, 10.6, 0.5), GL1 hardware
   (75.6, 11.6, 2.0) and GL1 llvmpipe (74.5, 11.3, 0.6) all contain both
   layers.  This is a regression between the two GL2 runs, not a precision
   difference.  Details in section 2.1.
3. **Rejected draws are silent after the first message (source).**
   `chrome9_draw_vbo()` drops draws whose FS variant is unsupported with
   `chrome9_warn_once(ctx, CHROME9_WARN_DRAW_STATE, ...)`
   (`$C/chrome9_context.c:6261-6264`), and bounded-draw-state rejects print
   only while that same bit is clear (`$C/chrome9_draw.c:2104-2141`).  After
   the first reject of either kind every further reject only increments
   `ctx->rejected_triangles`, which is never reported.  Missing surfaces
   (sky layer, dlights, alpha modes 1/2) cannot be distinguished from
   correct output with the current logs.
4. **q3dm1 needs no dynamic FACE, no alpha mode 2 and no fog (asset).**
   Section 3 lists every q3dm1 BSP shader.  Alpha mode 1 is still reached
   by every dynamic light (`$Q/tr_shade.c:413-419`).
5. The captures are `xwd` grabs after a fixed delay, not frame-locked
   (HUD counters differ: 104 vs 114).  `tools/glx_capture_swap.c:162-185`
   fakes only `clock_gettime(CLOCK_MONOTONIC)`, but ioquake3's
   `Sys_Milliseconds()` uses `gettimeofday()` (`code/sys/sys_unix.c:153-167`),
   so the shim cannot lock ioquake3 animation phase yet.  Scrolling sky,
   `tcMod turb` fire floors and `rgbGen wave` stages differ by phase in every
   current comparison.

## 1. Re-ranked G1..G11 (default cvars, no FBO)

"Real blocker" means it prevents a correct q3dm1 first frame, normal q3dm1
play, or the main menu under `cl_renderer opengl2`.

| new rank | item | state now | real blocker? |
|---:|---|---|---|
| 1 | **N1 (new)** reject visibility | shared warn-once hides all later rejects (finding 3) | yes: blocks every correctness claim |
| 2 | **N2 (new)** reference parity | llvmpipe runs the FBO/HDR/tonemap path; no frame lock for ioquake3 (findings 1, 5) | yes for comparisons, not for rendering |
| 3 | **N3 (new)** sky additive stage | second `tim_hell` stage absent since the `ad64c2f` run (section 2.1) | yes, visible on q3dm1 |
| 4 | G5 alpha mode 1 (dlight) | `dlight_fp` sets `u_AlphaTest=1` for every dynamic light; `chrome9_ps_lower_alpha_terminate()` admits only `flt(alpha, 0.5)` (`$C/chrome9_context.c:4558-4652,4692-4697`), so every dlight variant is rejected and dropped | yes for play (weapon fire, rockets, respawn effects); not for the first static frame |
| 5 | G7 fogpass / menu | q3dm1 has no fog volume; GL2 main menu capture (15:38) looks like llvmpipe, but not frame-locked | fogpass: no for q3dm1 (yes for q3dm7, q3tourney2); menu: needs a locked diff only |
| 6 | **N4 (new)** mip LOD parity | floor high-frequency energy HW/llvmpipe 0.73 (GL1) and 0.59 (GL2, tonemap-inflated) (section 2.3) | quality, not a missing feature |
| 7 | G1 lightall lightmap | renders after `ad64c2f`; no pixel oracle against a same-path reference | no longer a blocker; verify after N2 |
| 8 | G3 lightall `LIGHT_VECTOR` | view weapon and items draw (weapon has its own FS id in `q3dm1-gl2-fs-id-colors.png`); `b2f32(flt)` covered by `311e99e` | no; pixel check pending |
| 9 | G6 bounded run length | current captures are uncapped full frames | closed, but superseded by N1 |
| 10 | G2 FACE cull-none | no q3dm1 or menu surface needs it (section 3.1); first demo user is `q3tourney2` `textures/base_floor/pjgrate2` | no for q3dm1/menu |
| 11 | G4 lightall `LIGHT_VERTEX` | only for `rgbGen vertexLit/exactVertexLit` (`$Q/tr_shader.c:2523`) or `r_vertexLight`; no q3dm1 shader uses them | no |
| - | G8..G11 FBO, sun shadows, pshadow/SSAO/cubemap, float targets | off path while `GL_ARB_framebuffer_object` is absent | no |

## 2. The visible differences

### 2.1 Sky

**Program path (source).**  `textures/skies/tim_hell` has two stages:
`killsky_1` with `tcMod scroll 0.05 .1; tcMod scale 2 2; depthWrite`, and
`killsky_2` with `blendfunc GL_ONE GL_ONE; tcMod scroll 0.05 0.06; tcMod scale 3 2`.
The sky shader is not excluded from `CollapseStagesToGLSL()`
(`$Q/tr_shader.c:3181`), so each stage becomes lightall permutation
`LIGHTDEF_USE_TCGEN_AND_TCMOD` (8) with no light type
(`$Q/tr_shader.c:2328-2337`), not the generic program:

* VS: `var_TexCoords.xy = ModTexCoords(...)` (`glsl/lightall_vp.glsl:200-213`),
  `var_Color = u_VertColor * attr_Color + u_BaseColor` (`:242`).  This runs in
  Gallium Draw on the CPU, so `tcMod scroll/scale/turb` precision is the same
  float32 code as llvmpipe.  The scroll offset is wrapped to [0,1) on the CPU
  (`RB_CalcScrollTexMatrix`), and sky coordinates stay below about 10, far
  inside the 128-repeat range.
* FS: `diffuse = texture2D(u_DiffuseMap, var_TexCoords.xy)`
  (`glsl/lightall_fp.glsl:296`), alpha test block `:298-313` (mode 0),
  `gl_FragColor.rgb = diffuse.rgb * lightColor` with `lightColor = var_Color.rgb`
  (`:273,516`).  Implicit `rgbGen` is `CGEN_IDENTITY_LIGHTING`
  (`$Q/tr_shader.c:1369`), which leaves `u_BaseColor = 1` (`$Q/tr_shade.c:534`).
  Both stages use the same program; the coordinate is a direct `.xy`, so
  the `ad64c2f` TEMP bridge (`$C/chrome9_context.c:4215-4245`) does not apply.
* State: `RB_StageIteratorSky()` sets `glDepthRange(1.0, 1.0)`
  (`$Q/tr_sky.c:866`) and draws both stages with the same indices
  (`:895-897`, indices added only for stage 0 at `:692`).  Stage 0 writes
  depth; stage 1 uses the default `LEQUAL`, depth writes off, blend ONE/ONE.

The shader math cannot produce a missing layer; with the uniforms above the
output is `killsky_1 + killsky_2`.  The loss is in draw submission or
per-fragment state.  The `CHROME9_DEBUG_FS_MARK=id` capture agrees: the sky
has R = 77 (index bits R=2 plus the constant +3..4 LSB offset seen on every
marked color), while two ONE/ONE passes of the same program would give
about 150.

Hypotheses and discriminators, cheapest first:

| # | hypothesis | Chrome9 code path | discriminating experiment |
|---|---|---|---|
| S1 | stage-1 draw rejected and silently dropped | `chrome9_select_fs_variant()` `$C/chrome9_context.c:5360-5440` (variant cache, failed variants cached), silent drop `:6261-6264`; `chrome9_draw_state_supported()` `$C/chrome9_draw.c:803-1000` (blend, `depth-state` for read-only LEQUAL, `mip-view-state`), silent reject `:2104-2141` | run with a per-reason reject counter (N1, section 5 step 1); or `CHROME9_DEBUG_DRAWS=1` for one frame and look for a textured draw with blend factors ONE/ONE directly after the first sky draw |
| S2 | stage-1 depth test fails at the far plane | depth range [1,1]: stage 1 compares `z == 1.0` against the value stage 0 wrote.  With `CHROME9_EXPERIMENTAL_X8Z24=1` and ioquake3's default `r_stencilbits 8` the winsys depth can now be S8Z24 (`311e99e`, 14:27), whereas the pre-`ad64c2f` capture (15:10) may still have used a different library/depth format | `devmap q3dm1` + `r_showsky 1` (sets depth range [0,0], `$Q/tr_sky.c:864`): a restored second layer implicates the far-plane depth compare; repeat with `+set r_stencilbits 0 +set r_depthbits 16` to force D16 |
| S3 | the regression is in `ad64c2f` itself | only the new `next_temp` guard before every TEX (`$C/chrome9_context.c:4242-4243`) touches non-bridged samples | rebuild with only the bridge hunk reverted (keep FS_MARK), same command line; if the layer returns, bisect the guard |
| S4 | ONE/ONE blend or color mask lost in the batch key | batch compare `$C/chrome9_draw.c:1590-1600` compares the full `draw` struct including `blend`; unlikely | the GL2 main menu banner (`models/mapobjects/banner/q3banner02`, three ONE/ONE stages, same lightall family, normal depth range) appears layered as on llvmpipe in the unlocked 15:38 capture, which argues against S4 and for S2; confirm with a locked frame |

Texture-coordinate animation, `tcGen` and `deformVertexes` all run in Draw's
software VS on both drivers; they are not a plausible cause of this sky
difference.

### 2.2 Floor tint

The world floor surfaces are two kinds (asset, `maps/q3dm1.bsp` lump 1):

* implicit shaders (`blocks17floor2` 70 faces, `largerblock3b` 23,
  `xstairtop4` 48, ...): collapsed to lightall `USE_LIGHTMAP` with
  `USE_LIGHT`, normal/specular defaults (`r_normalMapping 1`,
  `r_specularMapping 1`), no deluxe map in the retail BSP;
* three-stage "overwrite" floors that cannot collapse because the lightmap
  stage uses `blendFunc GL_DST_COLOR GL_ONE_MINUS_DST_ALPHA`
  (`$Q/tr_shader.c:2392-2398` sets `skip`): `largerblock3b_ow`,
  `center2trn`, `killblockgeomtrn`, `demon_block15fx`, `chapthroatooz`.
  They run as three generic programs: `firegorre` ONE/ZERO with
  `tcMod scroll/turb/scale`, the `_ow` texture with SRC_ALPHA/INV_SRC_ALPHA,
  then `$lightmap` with DST_COLOR/INV_DST_ALPHA.

Relevant renderer lines (source):

* `ComputeShaderColors()` uses `overbright = 2` for every non-blended stage
  when `r_overBrightBits 1` (default) (`$Q/tr_shade.c:439,528-533`); ONE/ZERO
  stages are normalized to "no blend" first (`$Q/tr_shader.c:1382`).  So the
  first `firegorre` pass and the implicit lightall `var_Color` are 2.0.
* Lightmaps are shifted by `r_mapOverBrightBits - overbrightBits = 1` with
  hue-preserving normalization (`$Q/tr_bsp.c:108-128`), stored RGBA8, no RGBM
  (no `.hdr` lightmaps, `$Q/tr_bsp.c:279-287`).
* lightall: `lightColor = var_Color.rgb * texture2D(u_LightMap, var_TexCoords.zw).rgb`
  (`glsl/lightall_fp.glsl:273-283`), then `lightColor *= 0.75;
  lightColor /= max(surfNL, 0.25); ambientColor = max(ambientColor - lightColor*surfNL, 0)`
  (`:368-376`), `diffuse.rgb *= 1 - specular.rgb` (`:413`),
  `gl_FragColor = lightColor*reflectance*NL + ambientColor*diffuse` (`:436-437`).
  Intermediate values reach about 6.0 before the output clamp.

Hypotheses:

| # | hypothesis | relevant code | discriminator |
|---|---|---|---|
| F1 | **reference path**: in the llvmpipe FBO path the multi-pass `_ow` floors blend in RGBA16F (no clamp of the ×2 overbright `firegorre` pass) and see real destination alpha in `ONE_MINUS_DST_ALPHA`, then pass through the filmic tonemap/auto-exposure | finding 1 | rerun llvmpipe with `+set r_ext_framebuffer_object 0`.  The captured statistics already exclude a single tone curve: HW orange floor (148.7, 60.4, 44.9) vs reference (178.7, 80.9, 59.4), but HW walls (74.7, 35.6, 34.2) vs reference (54.2, 26.2, 27.8).  A monotone per-channel mapping cannot send HW 74.7 to 54.2 and HW 60.4 to 80.9, so the pre-tonemap inputs already differ, as F1 predicts for the multi-pass floors |
| F2 | `INV_DST_ALPHA` on a BGRX window target not evaluated with Ad = 1 | `chrome9_translate_blend()` passes the factor through (`$C/chrome9_draw.c:629-631,683-690`); the kernel maps it to native `0x13/0x12` (`$K/chrome9_engine.c:915-918`).  st/mesa rewrites it only when `_BlendForceAlphaToOneDraw` is set (`st_atom_blend.c:212-220,323-328`), i.e. not for a pipe format without alpha | localize the `_ow`/`center2trn` surfaces with `CHROME9_DEBUG_FS_MARK=<index>`; guarded cmd22 oracle: BGRX target with X byte 0x00 and 0x80, blend DST_COLOR/INV_DST_ALPHA, expect `src*dst` exactly.  GL1 uses the same blend on the same surfaces and matches llvmpipe, so F2 is lower priority unless the oracle fails |
| F3 | overbright > 1 values clipped inside the fragment program | `var_Color` is a generic varying (float interpolator, `$K/chrome9_engine.c` `chrome9_fragment_varying_format`), not PRIMARY0; MIN/MAX/RCP/RSQ/DP3 lowering `$C/chrome9_context.c:4425-4490`; SAT only from explicit `fsat` | run both paths with `+set r_overBrightBits 0` (var_Color = 1.0, lightmap shift 2); a difference that disappears only on this toggle points to range handling |
| F4 | lighting-direction terms (FACE sign, specular) | cull-specialized FACE `$C/chrome9_context.c:5378-5388` and `chrome9_specialise.c:430-470`; `cull_face = FRONT` correctly gives `front_face = false` | `+set r_normalMapping 0 +set r_specularMapping 0` (FAST_LIGHT: `gl_FragColor = diffuse * lightColor`, no FACE).  By `:368-376` the non-specular result is within about 3 % of `lightmap * diffuse` for any `surfNL`, so a sign error would only remove the small specular term; F4 cannot explain a 20 % difference |
| F5 | texture LOD | section 2.3 | `r_colorMipLevels 1` |

The orange octagon is very likely one of the `_ow` multi-pass floors, since
their `firegorre` stage produces exactly that glow; confirm with FS_MARK
before acting on F2.

### 2.3 Texture sharpness

Mean absolute neighbour difference in the grey floor, hardware/llvmpipe:
GL1 11.96/16.45 = 0.73, GL2 17.14/29.27 = 0.59 (capture).  The GL1 value is
a same-path comparison, so the hardware selects coarser mip levels than
llvmpipe also outside GL2.  Default `r_textureMode` is
`GL_LINEAR_MIPMAP_LINEAR` in this tree (`$Q/tr_init.c:1310`, not the older
`LINEAR_MIPMAP_NEAREST` assumption).  Mesa passes the GL bias unchanged
(`$C/chrome9_draw.c:1463-1469`) and the kernel writes A30 bias/min/max and the
A31 filters (`$K/chrome9_engine.c:1900-1912`).  The VX900 LOD formula
(rounding, per-quad vs per-pixel, max-axis vs Euclidean) has no silicon
oracle; the kernel self-test uses bias `-16` (-0.5) at `:7666`, the origin
of which should be checked against the vendor GL sampler builder.
Discriminators: `+set r_colorMipLevels 1` (latched; tints every mip level,
`$Q/tr_image.c:1976-1977`) on both drivers, then `r_textureMode GL_LINEAR`
(no mips) as control.  Also statically RE whether the vendor GL ICD adds a
filter-dependent A30 bias.

## 3. Two-sided FACE, alpha modes 1/2, fogpass and dlight

### 3.1 Surface census (asset)

Rules from source: FACE is read only by lightall with `USE_LIGHT` and not
`USE_FAST_LIGHT` (`glsl/lightall_fp.glsl:266-268`); shaders with
`deformVertexes` never collapse to lightall (`$Q/tr_shader.c:2345-2349`);
light types come only from a `$lightmap` filter stage, `lightingDiffuse` or
`vertexLit` (`:2515-2527`).

| map | cull-none + lit + no deform (dynamic FACE) | `alphaFunc GT0` (mode 1) | `LT128` (mode 2) | `GE128` (mode 3) |
|---|---|---|---|---|
| q3dm1 | none | none | none | `base_wall/protobanner` (deform, generic), `gratetorch2b` (lightall, no light) |
| q3dm7 | none | none | none | `skel`, `timlamp` |
| q3dm17 | none | none | none | `bot_wing` (deform), `spotlamp` |
| q3tourney2 | `base_floor/pjgrate2` (also GE128) | none | none | `pjgrate2`, `chain`, `banner_strgg` (deform) |

The q3dm1 cull-none shaders are `flame1_hell`, `flame2`, `flame1side`
(`nolightmap`, `rgbGen wave`), `lavahell_750` and `protobanner` (deform),
`lavahellflat_400` (`nolightmap`) and `gratetorch2b` (`rgbGen vertex`); none
defines a light type, so none reads FACE.  The main menu banner
`q3banner02` is cull-none but `rgbGen wave`, also without FACE.  Player
models in `models.shader` with `cull disable` + `lightingDiffuse` (lucy,
bones, xaero, hunter, flags) are not q3dm1 defaults.

Dynamic light: `ProjectDlightTexture()` binds `dlightShader` and always sets
`u_AlphaTest = 1` with `GLS_ATEST_GT_0` and depth EQUAL
(`$Q/tr_shade.c:411-419`), so **mode 1 is on every map** as soon as a light
source is active.  Fog: q3dm1 has zero fog lumps; q3dm7 has 2, q3tourney2 7.

### 3.2 Remaining Mesa/kernel work

**Alpha mode 1 (dlight, lightall/generic `GT0`).**  NIR form:
`terminate_if(feq(alpha, 0))`, alpha = `tex.a * var_Color.a`
(`glsl/dlight_fp.glsl:12-18`).  For `dlight_fp` the texture alpha is 255
(`$Q/tr_image.c:2583`) and `var_Color.a = u_Color.a * dlightmod` with
`dlightmod` in [0,1] (`glsl/dlight_vp.glsl`), so the existing draw-time
unit-interval proof (`unit_interval_input_mask`, `$C/chrome9_context.c:4613-4623`)
applies.  With alpha proven non-negative and finite, an exact lowering is
`CMP(-alpha, -1, +1)` followed by the existing TEMP-to-address KILL bridge:
`-0 >= 0` selects `-1` (kill), any positive alpha selects `+1` (keep).  NEG
is already an allowed CMP-A source modifier.  Open point to decide: the
earlier boundary note requires positive subnormal alpha to survive; GLSL
permits denormal flushing, and the NIR `feq` carries only
`preserve:inf,nan`.  Needed: extend `chrome9_ps_lower_alpha_terminate()`
beyond `nir_op_flt` for `feq(x, 0)` with the unit-interval proof, one
isolated silicon oracle (alpha 0, +0 from a product of 0 and 1, 1/255 and
1.0), then the real dlight pass on q3dm1 with a fired weapon.  The fixed A00/A33
`GREATER 0` path (`$C/chrome9_draw.c:872-889`) compares the quantized output
alpha and is not an exact substitute (an alpha below 1/510 would discard).

**Alpha mode 2.**  No demo-map user.  Keep the design in
`ioquake3-blocker-roadmap-re.md` section 12 (`SUB(alpha, 0.5)`,
`CMP(d, -1, +1)`, KILL, exact-half oracle); low priority.

**Mode 3 in full lightall with FACE (q3tourney2 `pjgrate2`).**  Needs the
dynamic FACE producer plus SUB+KILL in the 95-ALU lightmap object, which
does not fit unchanged (roadmap section 13); first enable `has_fsub` with
`nir_opt_algebraic_late` (projected 88 ALU), then add the marker MAD and the
kill.

**Dynamic FACE (cull none).**  Not needed for q3dm1.  Remaining work, in
order: implement the Draw two-side COLOR/BCOLOR white/black marker into the
unused PRIMARY0 as specified in `chrome9-draw-twoside-face-sign.md`; lower
`load_front_face` in `chrome9_select_fs_variant()` for `face_mode == 0`
(`$C/chrome9_context.c:5378-5394`, currently dynamic FACE is only accepted
if the canonical program already compiles) to `1 - 2*PRIMARY0.r`; offline
tests for opposite windings, flipped `front_ccw` and clipped triangles; one
guarded cmd22 front/back pixel oracle.  No new kernel ABI is needed while
Draw runs the VS; the native ATTROUT 2/3 + A61[19] + A23[7] route belongs to
the hardware-TCL work.

**Fogpass (q3dm7/q3tourney2).**  `gl_FragColor = u_Color;
gl_FragColor.a = sqrt(clamp(var_Scale, 0, 1))` (`glsl/fogpass_fp.glsl`).
Mesa lowers `fsqrt` to `frcp(frsq(x))` (`lower_fsqrt`, `$C/chrome9_screen.c:275`),
so `var_Scale = 0` depends on native `RSQ(0) = +inf` and `RCP(+inf) = 0`,
which has no silicon oracle; the clamp is `MOV.SAT`.  It is a zero-texture
program with one float varying, SRC_ALPHA/INV_SRC_ALPHA, depth EQUAL or
LEQUAL read-only (`ioquake3-gl2-fogpass-native-design.md`, capability 37
still disabled).  Remaining: zero-texture cmd22 submission, the RSQ/RCP
edge oracle, then a q3tourney2 locked comparison.

**Dlight colour pass.**  Beyond mode 1 the program is TEX * varying with
ONE/ONE or DST_COLOR/ONE blend and depth EQUAL read-only; the cmd22 fixed
states were already qualified
(`ioquake3-alpha-depth-blend-cmd22-qualification.md`).  `r_dlightMode 0` is
default; modes 1/2 use lightall `LIGHT_VECTOR` passes instead.

## 4. GL1 features still unverified

| feature | where it occurs | test idea |
|---|---|---|
| texture env `GL_ADD` two-unit collapse (`renderergl1/tr_shader.c:1790-1794`) | q3dm17 `metalfloor_wall_15ow`; not q3dm1 | frame-locked q3dm17 capture after fixing the shim; synthetic two-unit ADD oracle with saturating inputs (0.75 + 0.75 must clamp to 1.0 per channel) |
| MODULATE collapse of vertex colour x texture x lightmap and remaining three-stage shaders | q3dm1 `protobanner` (CPU deform, GE128 + EQUAL lightmap), `largerblock3blood` (turb, `tcGen environment` ONE/ONE, blend, lightmap filter) | FS-mark style localisation is not available in GL1 (fixed function); use `r_showtris`-free crop comparisons on locked frames at those surfaces |
| fog pass (`RB_FogPass`, fog LUT texture, SRC_ALPHA blend, depth EQUAL, CPU `tcGen fog`) | q3dm7, q3tourney2 | locked capture inside and outside a fog volume |
| dynamic lights (CPU-projected, DST_COLOR/ONE or ONE/ONE, depth EQUAL) | any map, weapon fire | append `+wait 200 +attack` to the command line, take a locked capture of the muzzle-flash frame, count rejects |
| overbright via hardware gamma | GL1 sets `overbrightBits = 0` when windowed or without gamma (`renderergl1/tr_image.c:1272-1279`), so all windowed captures skip it | run `r_fullscreen 1`, check the log for `GAMMA: hardware w/ 1 overbright bits` and verify the X server applies the SDL gamma ramp (xwd does not show it) |
| stencil shadows (`cg_shadows 2`) | any map | now possible with the S8Z24 stencil path; compare against llvmpipe |
| cinematics (`RE_StretchRaw`, per-frame `glTexSubImage2D`) | menu CINEMATICS, intro (skipped by `com_introplayed 1`) | play `idlogo` and compare one locked frame |

Menu GL1 capture (15:43) differs in banner brightness, but the banner uses
`rgbGen wave sin .5 .5 0 .2` (`models.shader:570-590`), so only a locked
frame can decide.

## 5. Prioritized next implementation steps

1. **Reject accounting (N1).**  Replace the shared warn-once with per-reason,
   per-FS-index counters (reason string, `fs_index`, first/last frame),
   printed rate-limited under `CHROME9_DEBUG_REJECT` and summarized at
   context destroy.
2. **Same-path reference and frame lock (N2).**  Add `gettimeofday()`
   interception for the main program to `tools/glx_capture_swap.c` and use
   it in `chrome9_q3shot.sh`; run llvmpipe with
   `+set r_ext_framebuffer_object 0`; recompute the floor/wall/sky
   statistics.
3. **Sky additive layer (N3).**  With step 1 in place, one run each of:
   plain, `devmap` + `r_showsky 1`, `r_stencilbits 0 r_depthbits 16`, and the
   bridge-reverted library; fix whichever of S1..S3 the evidence selects.
4. **Alpha mode 1 for dlights (G5).**  `feq(alpha, 0)` lowering through
   `CMP(-alpha)` + KILL under the unit-interval proof, after deciding the
   subnormal policy; isolated silicon oracle, then a q3dm1 weapon-fire frame.
5. **LOD parity (N4).**  `r_colorMipLevels 1` on both drivers; static RE of
   the vendor GL A30 bias rule; correct the bias or level selection if the
   boundaries differ.
6. **Destination alpha on BGRX (F2).**  Guarded cmd22 oracle; if the native
   factor reads the X byte, rewrite DST_ALPHA to ONE and INV_DST_ALPHA to
   ZERO (and SRC_ALPHA_SATURATE to ZERO) in `chrome9_translate_blend()` for
   targets without alpha.
7. Then the non-q3dm1 features: fogpass zero-texture path (q3dm7,
   q3tourney2), `has_fsub` compaction, dynamic FACE producer and mode 3 in
   full lightall (q3tourney2 `pjgrate2`), alpha mode 2, GL1 ADD/fog/dlight
   locked checks.

## 6. Reproduction of the capture numbers

Panels in `q3dm1-gl2-*.png` (972x384) span x 67..485 (hardware) and
553..971 (llvmpipe), y 50..383; `q3dm1-gl1-hw-vs-llvmpipe.png` (810x320)
spans 56..404 and 461..809.  Sky statistics use the panel-relative box
x 5-50 %, y 15-30 % (GL1) or the absolute box (80,60)-(300,110) plus the
llvmpipe offset 486 (GL2).  Texture means were computed with Pillow over
the full decoded `killsky_1.jpg`/`killsky_2.jpg` from `/tmp/pak0.pk3`.
The high-frequency measure is the mean of |dx| + |dy| of the luminance
over the grey-floor box x 3-20 %, y 87-98 %.

## Sky stage 2 investigation (2026-09-24, silicon)

* Matched reference: llvmpipe with `+set r_ext_framebuffer_object 0` gives a
  sky mean of R 71.9 in the q3dm1 start view; Chrome9 gives 41.6, i.e. the
  `killsky_2` additive stage contributes nothing.
* Not the depth format (X8Z24 and Z16 identical), not `ad64c2f` (reverting it
  changes nothing), not texture wrap (`tools/re/chrome9_gl_wrap_smoke.c`
  passes REPEAT/MIRRORED/CLAMP on silicon), and not the plain far-plane
  `GL_LEQUAL` equality (`chrome9_gl_depthfar_smoke.c` passes at depth range
  1.0/0.5/0.0).
* `CHROME9_DEBUG_FS_MARK` bisection: only fragment shader index 161 (GL2
  generic `tex.rgb * var_Color.rgb`, inlined alpha-test mode) changes the sky;
  marking the additive shaders seen in ONE/ONE draws (96/98/99, other
  effects at depth 0.75-0.99) does not.  No triangle is rejected during
  normal play (per-site counters), so the second stage is either not issued
  through the cmd22 path or contributes a zero colour.
* Reject counters also showed that ioquake3's SIGTERM handler re-enters the
  driver mid-ioctl during shutdown; those rejects are harness artifacts
  (Mesa `354f6df` fails closed instead of overrunning the batch).

Next step: log every draw with the sky shader index (no budget) including
blend, depth flags and first-vertex colour/z, to see whether stage 2 reaches
the kernel and with which colour.
