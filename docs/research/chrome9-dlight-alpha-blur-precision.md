<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9: ioquake3 dynamic lights, glmark2 desktop-blur precision, refract and terrain

Status: offline analysis on 2026-09-24. The VX900 target was not accessed and
no driver source was changed. A host llvmpipe run of the installed glmark2
2023.01 binary under Xvfb provides the reference values for section 2.
Each statement is marked as **evidence** (read from source or measured on
the host) or **inference** (derived, not measured on silicon).

Path prefixes:

| prefix | path |
|---|---|
| `C/` | `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/chrome9/` |
| `K/` | `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/chrome9/` |
| `Q/` | `/home/ai/src/parhelia/ioq3-93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1/code/` |
| `G/` | `/home/ai/tmp/gm2/glmark2-2023.01/` (unpacked T2 source, same version as the target) |
| `B/` | `/home/ai/tmp/blurmodel/` (CPU model and `LD_PRELOAD` shims written for this note) |

The Mesa working tree is being edited while this note is written: the
llvmpipe fallback (`C/chrome9_swfb.c`, not yet committed) is in progress.
Line numbers in `C/chrome9_context.c` and `C/chrome9_draw.c` refer to the
tree as read at the time of writing and may shift.

## Summary and ranked recommendations

| rank | item | action | size |
|---|---|---|---|
| 1 | GL2 dlight: every dlight draw is rejected, so dynamic lights are missing | lower `terminate_if(feq(x, ±0))` to `ABS` + `CMP(-|x|, -1, +1)` + `KILL` in `chrome9_ps_lower_alpha_terminate()` (section 1.4) | about 60 lines plus a host NIR test; one silicon oracle |
| 2 | Fallback: GL2 dlight must not go to llvmpipe | as long as rank 1 is open, the new fallback accepts the dlight draw. It then runs depth `EQUAL` against a depth buffer the hardware wrote, so the light renders incompletely (section 1.5). Rank 1 removes the case; otherwise apply the design's depth-coherence rule R3 | small |
| 3 | desktop blur: determine the 1-LSB source | run `B/stage_dump.so` on the target and compare the pre-blend column with the prediction table in section 2.5 (flat rows separate truncation from sampling error) | one bounded run of about 7 s |
| 4 | desktop blur fix, conditional on rank 3 | if the fragment-output UNORM conversion truncates: add a +0.5/255 output bias in the compiler for UNORM8 targets. If the blender truncates: look for a rounding control bit, otherwise accept and document. If the sample position is off: correct the coordinate bias | depends on the result |
| 5 | Fallback robustness for terrain/refract | replace the fixed `held[128]` array, which silently drops references, with a dynamic array; derive alias bind flags from the resource `bind` field, not from the first use; add a host test with the chrome9-finalized NIR (design R8) (section 3.4) | small, independent |
| 6 | refract testability | use `refract:model=cube` (12 triangles) for feature and correctness work. The default bunny has 69,666 triangles in each of two passes, so one frame cannot finish in the 7 s run window (section 3.2) | none (command line) |
| 7 | GL1 dlight | state already qualified; only a locked weapon-fire frame is missing (section 1.2) | verification only |

## 1. ioquake3 dynamic lights

### 1.1 GL2 renderer (`r_dlightMode 0`, the default)

Evidence:

* `ProjectDlightTexture()` (`Q/renderergl2/tr_shade.c:351-427`) binds
  `tr.dlightShader[deform ? 1 : 0]`, the 16x16 `*dlight` image on
  `TB_COLORMAP` (`:408`), and sets
  `GLS_ATEST_GT_0 | ONE/ONE | DEPTHFUNC_EQUAL` for additive lights or
  `GLS_ATEST_GT_0 | DST_COLOR/ONE | DEPTHFUNC_EQUAL` otherwise (`:412-417`).
  It also sets `u_AlphaTest = 1` (`:419`) and draws the whole surface index
  list (`:421`). Depth writes are off, because no `GLS_DEPTHMASK_TRUE` is set.
  The GL2 backend implements `GLS_ATEST_*` in GLSL, not as fixed-function
  alpha test.
* The texture is RGB = inverse-square falloff and **A = 255** everywhere,
  with `IMGFLAG_CLAMPTOEDGE` and no mipmaps (`Q/renderergl2/tr_image.c:2561-2585`).
* No texgen. The vertex shader computes
  `var_Tex1 = dist.xy * u_DlightInfo.a + 0.5`,
  `dlightmod = step(0, dot(dist, normal)) * clamp(2(1 - |dist.z| * scale), 0, 1)`
  and `var_Color = u_Color * dlightmod`, with `u_Color.a = 1.0`
  (`Q/renderergl2/glsl/dlight_vp.glsl:87-91`, `tr_shade.c:396-400`).
* The fragment shader computes `alpha = tex.a * var_Color.a` and, with
  `u_AlphaTest == 1`, executes `if (alpha == 0.0) discard;`.
  It outputs `rgb = tex.rgb * var_Color.rgb` and `a = alpha`
  (`Q/renderergl2/glsl/dlight_fp.glsl:13-31`).

After Chrome9 inlines `u_AlphaTest` (`C/chrome9_specialise.c:242-257,313`)
the predicate is `terminate_if(feq(fmul(tex.a, varying.a), 0.0))`.

### 1.2 GL1 renderer

Evidence (`Q/renderergl1/tr_shade.c:407-557`): the texture coordinates
(`0.5 + dist.xy / radius`) and per-vertex byte colours (colour x modulate,
alpha 255) are computed **on the CPU**. There is no texgen and no alpha test.
The texture is the same 16x16 A=255 image (`Q/renderergl1/tr_image.c:1072-1097`).
TMU0 texture env is `MODULATE`: it is set at init (`tr_init.c:848-855`), and
only TMU1 is changed in `DrawMultitextured` (`tr_shade.c:366-381`).
Blend is `ONE/ONE` or `DST_COLOR/ONE` with depth `EQUAL` and no depth write
(`:547-552`). Only triangles with at least one lit vertex are drawn (`:517-532`).

The GL1 dlight pass is therefore `TEX * PRIMARY` with blend and `EQUAL`
read-only depth. The guarded cmd22 oracle has already qualified the state
combinations `EQUAL`/read-only plus `ONE,ONE` and `DST_COLOR,ONE`
(`ioquake3-blocker-roadmap-re.md:500-507`, **evidence** from an earlier run).
`DST_COLOR` needs `DRM_CHROME9_CAP_BLEND_EXTENDED_FACTORS`
(`C/chrome9_draw.c:639-641`), which the kernel reports (`K/chrome9_drv.c:2599-2600`).
No reject site for the GL1 dlight pass was found (**inference**). It needs
only the locked weapon-fire capture listed in
`ioquake3-gl2-remaining-features.md:306`.

### 1.3 Exact reject site (GL2)

Evidence:

1. `chrome9_compile_ps_nir()` accepts a `terminate_if` only through
   `chrome9_ps_lower_alpha_terminate()` (`C/chrome9_context.c:4824-4830`).
2. That function accepts only `nir_op_flt` with the constant `0.5` as
   reference (`C/chrome9_context.c:4709-4718`). `feq(alpha, 0)` returns
   `false` here.
3. `chrome9_compile_native_fs()` then leaves `semantic_fs_valid = false`,
   `chrome9_compile_fs_nir()` leaves `CHROME9_FS_UNSUPPORTED`, and
   `chrome9_select_fs_variant()` (`:5522`) returns false.
4. `chrome9_draw_vbo()` rejects the draw with `"draw rejected: unsupported
   bounded fragment program"` (`C/chrome9_context.c:6427-6435`). In the
   current working tree it first tries `chrome9_sw_draw_vbo()` (`:6429`);
   see section 1.5.

The kernel does not reject the pass. `K/chrome9_fragment.c:239-260` accepts
exactly one `KILL` of a full-swizzle TEMP predicate before any colour
output. `KILL` discards when any component is negative (PS 2.0 `texkill`
semantics), and it consumes one ALU record, one TAU record and an internal
coordinate slot (`:455-464`). CMP is a supported kernel opcode (`:82`).

The Mesa fixed-function alternative (A00 bit 20 + A33 `GREATER 0`,
`C/chrome9_draw.c:912-930`) is used only for the `IOQUAKE3_*_PACKED`
classifications. It compares the quantized output alpha, so it is not an
exact replacement for `feq(x, 0)` in a generic GLSL program.

### 1.4 Smallest correct fix

Extend `chrome9_ps_lower_alpha_terminate()` (or add a sibling function
called from the same site) as follows:

1. Accept `compare->op == nir_op_feq` with one operand a constant whose bits
   satisfy `(bits & 0x7fffffff) == 0`. The same rule is already used in
   `chrome9_ps_lower_equal_zero_select()`, `C/chrome9_context.c:3804-3830`.
   Also accept the float controls allowed there.
2. Lower the other operand through the ordinary `chrome9_ps_lower_def()`.
   Do not require a sampled-alpha shape: the lowering is generic for any
   scalar that has a finite proof. Use `CHROME9_PS_PROOF_FINITE`. For
   dlight, alpha is a UNORM texel times a varying, and draw-time
   validation already rejects non-finite varyings.
3. Emit:

   ```text
   ABS  t0.x, alpha
   CMP  t1.xyzw, -t0.xxxx, c(-1), c(+1)   ; -|a| >= 0  <=>  a == ±0
   KILL t1
   ```

   The NEG modifier on the CMP A operand is silicon-qualified
   (`C/chrome9_context.c:2830-2838`). Use `chrome9_ps_emit_cmp()`
   (`:3879-3908`) with a four-lane writemask, because the kernel requires
   the xyzw predicate swizzle. Place `-1` and `+1` as packed lanes of one
   immediate register. The kernel rule against three distinct constant
   registers does not apply, because source A is a TEMP (`K/chrome9_fragment.c:224-237`).
4. Total cost: 2 ALU + 1 KILL record. No unit-interval proof is needed, so
   no guarded-domain vertex scan is required, unlike the `CMP(-alpha)`
   variant in `ioquake3-gl2-remaining-features.md:244-261`.

Tests:

* Host NIR test in `C/chrome9_shader_nir_test.c`. `feq(fmul(tex.a, T1.w), 0)`
  must give TEX, MUL, ABS, CMP, KILL, with KILL before the colour output.
  `feq(x, -0.0)` must be accepted. `feq(x, 0.25)`, `fneu(x, 0)` and a
  NaN-preserving compare without a finite proof must be rejected.
* One guarded silicon oracle with alpha values `+0`, `-0` (from `MUL(-1, 0)`),
  `0 * 1`, `1/255` and `1.0`. This decides whether `CMP` treats `-0 >= 0`
  as true. Denormal policy: GLSL allows flush-to-zero, and a flushed
  subnormal alpha that is discarded is conformant.
* Then one q3dm1 GL2 frame with a fired weapon (`+wait 200 +attack`).

Remark (**evidence** from the shader source, **not** a basis for the
generic fix): for this specific shader, `rgb = tex.rgb * u_Color.rgb *
dlightmod` is exactly zero wherever `alpha = dlightmod = 0`. Under both
`ONE/ONE` and `DST_COLOR/ONE` the discarded fragments are therefore
no-ops. A one-LSB difference in the discard boundary cannot change the
image, and the silicon oracle above is sufficient.

### 1.5 Interaction with the llvmpipe fallback (inference)

`C/chrome9_context.c:6427-6431` now sends every draw whose fragment
variant fails to `chrome9_sw_draw_vbo()`. The GL2 dlight draw would
therefore run in llvmpipe against the S8Z24 buffer the hardware wrote with
depth test `EQUAL`. The hardware path sends 16.16 fixed-point vertex depth
(`C/chrome9_draw.c:594-599`), while llvmpipe interpolates FP32 z. This is
risk R3 in `chrome9-llvmpipe-fragment-fallback-design.md:640-643`, which
also proposes the rule. Most `EQUAL` tests will fail and the light will
render incompletely. The native lowering in 1.4 is the correct fix. Until
it exists, the fallback should reject or specially route `EQUAL` and
read-only `LEQUAL` draws whose depth buffer was written by the hardware.

## 2. glmark2 desktop blur precision

### 2.1 Scene and validation (evidence)

* Validation draws only the first frame. `MainLoopValidation::draw()` does
  not call `update()` (`G/src/main-loop.cpp:372-383`), so the window
  positions are exact. The 800x600 canvas gives windows of 210x210
  (`0.35 * 600`); window 0 is at desktop (495,195)-(705,405)
  (`G/src/scene-desktop.cpp:856-873`).
* The pixel read is (512,209) (`:1025`). The reference is `0x89a353` =
  (137,163,83) (`:1008`), and the pass criterion is RGB distance
  `< sqrt(12) + 0.01 = 3.474` (`:988,1027-1029`). Only window 0 covers this
  pixel; windows 1-3 end at x = 505.
* Per window (`:497-538`), the stages are:
  1. **H pass**: `render_from()` draws a full-screen quad into the
     window's 210x210 RGBA8 FBO. It samples the desktop texture at
     `(495+i+0.5)/800` with `TextureStepX = 1/800`, so all eleven taps
     fall on texel centres.
  2. **V pass**: the window texture is drawn back into the desktop with
     blending disabled. `TextureStepY` is `1/target.size().y() = 1/600`
     (`:502`), printed with `std::fixed` as `0.001667`
     (`G/src/libmatrix/shader-source.cc:244-252`). This step samples a
     **210-texel** texture, so the taps are at **0.35007-texel** steps
     and every tap except the centre is a fractional bilinear sample.
  3. **Window contents**: `desktop-window.png` (512²), minified to a
     210² FBO with `SRC_ALPHA/INV_SRC_ALPHA` over a zero clear. At the
     pixel the texel is (0,0,0,127), so the FBO holds (0,0,0,63). This FBO
     is then drawn 1:1 over the desktop with
     `SRC_ALPHA, INV_SRC_ALPHA, ZERO, ONE`, which gives
     `final = V * (1 - 63/255)`.
* Shader math (`G/src/scene-desktop.cpp:45-102`): 11 taps
  `texture2D(T, uv + vec2(k*step, 0)) * Kernel|k|` with
  `Kernel = 0.239365, 0.199935, 0.116512, 0.047370, 0.013437, 0.002659`
  (sigma 5/3). The weights sum to 0.99932. The PS constants are exact FP32
  immediates (`chrome9-glmark-desktop-blur-closure.md`), so the constant
  format is not a source of error.
* The pixel lies on a hard horizontal edge of `effect-2d.png`. Desktop rows
  y <= 208 are white (255) and rows y >= 209 are (161,205,65). The V pass
  mixes the two rows through the 0.35-texel taps.

### 2.2 CPU model, confirmed bit-exact against llvmpipe (evidence)

`B/model.py` models the three stages with FP32-exact weights, bilinear
CLAMP sampling, round-to-nearest UNORM8 stores and an exact blend.
glmark2 2023.01 on the host, with llvmpipe (LLVM 22.1.7) under Xvfb and
`--validate`, reports `Validation: Success`. The shims `B/rp.so` and
`B/stage_dump.so` intercept `glReadPixels` and `glBlendFuncSeparate` through
`dlsym`. They record:

| quantity | llvmpipe | CPU model |
|---|---|---|
| pre-blend V column x=512, y=207..211 | 254/254/253, 235/244/214, **181/216/106**, 162/205/67, 161/205/65 | identical |
| final pixel (512,209) | **136/163/80** | 136/163/80 |
| distance to reference | **3.16** (limit 3.474) | 3.16 |

The reference GPU and llvmpipe already differ by 3.16. The margin to the
limit is 0.31, so **any 1-LSB loss in G or B fails**. The reference itself
corresponds to a vertical sample position about 0.05 texel lower than
llvmpipe's (table 2.3).

### 2.3 Sensitivity (evidence from the model)

| perturbation (V pass only unless stated) | final RGB | distance | result |
|---|---|---|---|
| exact (= llvmpipe) | 136/163/80 | 3.16 | pass |
| vertical sample shift +0.01 texel | 136/162/79 | 4.24 | fail |
| vertical sample shift +0.02 texel | 136/162/79 | 4.24 | fail |
| vertical sample shift -0.05 texel | 139/163/84 | 2.24 | pass |
| bilinear weight rounded to 5 bits | 136/162/79 | 4.24 | fail |
| bilinear weight rounded to 4, 6, 7, 8 bits | 136/163/80 | 3.16 | pass |
| bilinear weight truncated to 3..8 bits | 136-137/163/80-81 | <= 3.16 | pass |
| V store truncated instead of rounded | 136/162/79 | 4.24 | fail |
| H store truncated | 136/162/79 | 4.24 | fail |
| final blend truncated | 136/162/79 | 4.24 | fail |
| blend `(255-a)/256` fixed point, rounded | 136/162/80 | 3.32 | pass |

The reported failure is "off by 1/1/4" against the reference. That matches
136/162/79 (delta -1/-1/-4, distance 4.24), which is **llvmpipe minus one
LSB in G and B**. Several single causes produce exactly this value
(**inference**): one truncating float-to-UNORM8 conversion (H store,
V store or blender), or a vertical sample-position error of only about
+0.01 texel. The first-order sensitivity at this pixel is about
0.6 blue levels per 0.01 texel of vertical offset.

### 2.4 Ranking of causes (inference)

1. **A float-to-UNORM8 conversion that truncates**, in the fragment output
   conversion or in the blender. This is the most economical explanation.
   It needs no sampling error, and it agrees with the earlier two-pass
   oracle, which found a maximum RGB delta of 1 against an `lroundf` CPU
   reference (`chrome9-glmark-desktop-blur-closure.md`, "Deterministic
   two-pass pixel oracle"). That oracle's 5-level tolerance hid the sign.
2. **Vertical sample position / texture-address precision** in the V pass.
   Plausible, but it needs an effective bias of at least +0.01 texel.
   The earlier oracle cannot detect it: at 300x300 its taps are at
   integer texel offsets (`tools/chrome9_egl_blur_oracle.c:25-31`,
   `0x3b5a6e76 * 300 = 0.9999`), not at 0.35-texel fractions.
3. Bilinear weight quantization alone passes for most bit widths; only
   5-bit round-to-nearest fails.
4. Varying interpolation and PS constant format: ruled out as dominant.
   The FP32 immediates are bit-exact, and the geometry is exact: 16.16
   positions with the D3D9 half-pixel bias, `C/chrome9_draw.c:579-591,2404-2410`.
   A full-target quad maps to [-0.5, W-0.5], exactly inside the clamp.

### 2.5 Isolating experiment

Run once on the target, with the usual lock and the 7 s limit. The shim is
an x86-64 build that links only against glibc `libdl`. Rebuild it with
`debug.buildenv` if the target's glibc differs.

```sh
LD_PRELOAD=~/tmp/blurmodel/stage_dump.so glmark2 --validate --debug \
  -b desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true \
  2>&1 | grep -E '^VPASS 5 |READPIXEL|Validation'
```

`VPASS 5` is the desktop column x=512, y=204..214 after window 0's V pass
and before the window-contents blend, in the validation frame. Calls 1-4
come from the setup dummy renders. `--debug` also prints glmark2's own
`Expected/Actual` line. Classify the result with this prediction for
y = 206..212 (`B/columns.py`):

| hypothesis | y=206 | 207 | 208 | 209 | 210 | 211 | 212 (flat) |
|---|---|---|---|---|---|---|---|
| exact / llvmpipe | 255/255/255 | 254/254/253 | 235/244/214 | 181/216/106 | 162/205/67 | 161/205/65 | 161/205/65 |
| V store truncates | 254/254/254 | 253/254/252 | 234/244/213 | 181/215/105 | 161/205/66 | 160/204/64 | 160/204/64 |
| H store truncates | 254/254/254 | 253/253/252 | 234/243/213 | 180/215/105 | 161/204/66 | 160/204/64 | 160/204/64 |
| sample shift +0.02 texel | 255/255/255 | 254/254/253 | 234/243/212 | 180/215/105 | 162/205/67 | 161/205/65 | 161/205/65 |
| 5-bit rounded weights | 255/255/255 | 254/254/253 | 235/244/214 | 181/215/105 | 162/205/67 | 161/205/65 | 161/205/65 |

The **flat rows** (y >= 211) separate conversion truncation, where the
0.99932 weight sum gives 160/204/64, from any sampling-position or
filter-weight error, which leaves 161/205/65. After that, compare
`READPIXEL` with `round(V * 192/255)` to check the blender separately. If
the column matches llvmpipe but the final value is 1 lower, the blender
truncates.

If the result needs a further split, the micro-oracles (each bit-exact, no
tolerance) are:

* **E1 output conversion**: constant uniform colour `(n + f)/255` for
  f in {0.25, 0.49, 0.51, 0.75}, blend off, BGRA target. This decides
  round versus truncate.
* **E2 blender**: shader alpha 63/255 over a 0..255 destination ramp with
  `SRC_ALPHA/INV_SRC_ALPHA`. Tabulate the result against `round` and
  `floor`, and against the /255 and /256 forms.
* **E3 filter**: a 2x1 texture (0, 255) sampled at `k/256`-texel offsets
  from a computed coordinate, and separately from the interpolated varying.
  This gives the weight transfer function and any coordinate bias.

### 2.6 Fix options (conditional, inference)

* Truncating **fragment output conversion**: add a `+0.5/255` bias in the
  compiler to the terminal colour write for UNORM8 targets. It can be
  folded into an existing final `MAD` or `ADD` constant, otherwise it
  costs one ALU. This is generic and belongs in the variant key only if
  the target format changes the program.
* Truncating **blender**: check the vendor state for a blend
  rounding or dither control bit (A00/A5x). If there is none, the
  hardware limit stays, and the scene result should be recorded as a
  precision limit, not as a functional defect.
* **Coordinate bias**: apply a compensating sub-texel bias only if E3
  shows a systematic offset. It must go into the sampler or coordinate
  setup, not into application-specific constants.

## 3. glmark2 refract and terrain

### 3.1 Terrain: per-frame draws (source evidence; chrome9 status marked)

`G/src/scene-terrain.cpp:60-176,332-338`, renderers in `G/src/scene-terrain/`:

| draw | target | program / state | chrome9 path now |
|---|---|---|---|
| height map (simplex noise) | 256² RGBA FBO, no depth, L0 only | `terrain-noise.frag`: no textures, 28 `b2f32(fge)`, 40 FRC, 60 dot products | **FS rejected**: no `b2f32` lowering, and at least 100 ALU records against 96 (`chrome9-glmark-terrain-scene-current-boundary.md`). Now goes to the llvmpipe fallback (`C/chrome9_context.c:6427-6431`) |
| normal map | 256² RGBA FBO | 3 TEX from the height map (2 computed coordinates), `normalize` | native (**inference**, not compiled here). Reads the fallback output, so it needs `chrome9_sw_finish()` before the GPU job, which `C/chrome9_context.c:6444-6445` provides |
| terrain main | screen-size RGBA FBO + **Z16 renderbuffer** (`base-renderer.cpp:148-161`) | VS samples height and normal (VTF). FS 68 ALU + 5 TEX. Three 512² REPEAT trilinear mip textures + 512² specular (REPEAT, trilinear, generated) + normal map. Then `GenerateMipmap` (`terrain-renderer.cpp:56`) | **goes to llvmpipe** because of the new VTF rule (`C/chrome9_context.c:6437-6442`) |
| bloom H | 256² FBO, trilinear/clamp | 5-tap blur of the 800x600 terrain texture at step 1/800, i.e. minified: needs the generated mip chain | native (**inference**) |
| bloom V | 256² FBO | 5-tap blur | native (**inference**) |
| overlay | draws into the terrain texture | `opacity * texel`, blend `SRC_ALPHA, ONE` (`overlay-renderer.cpp:80`), then `GenerateMipmap` again (`:89`) | native (**inference**) |
| tilt H | screen-size FBO, trilinear/clamp | 9 taps, step `* abs(0.5 - y) / 0.5` | native (**inference**) |
| tilt V | onscreen | 9 taps | native (**inference**) |
| (setup) specular | 512² FBO, REPEAT trilinear | luminance, then mip generation | native (**inference**) |

A frame therefore alternates GPU, llvmpipe (noise), GPU (normal), llvmpipe
(main), and GPU (bloom, overlay, tilt). The frame also has three CPU
mip-generation calls: the terrain texture twice, and the bloom and tilt
textures.

### 3.2 Refract: draws (source evidence)

`G/src/scene-refract.cpp`:

* Depth/normal pass: renders into a **2x canvas** FBO
  (`width_ = 2*w, height_ = 2*h`, `:179-180`), 1600x1200 at 800x600, within
  `max_texture_2d_size = 2048` (`C/chrome9_screen.c:488`). Attachments:
  a `GL_DEPTH_COMPONENT` texture (NEAREST, clamp, `:198-203`) and an RGBA
  texture with `LINEAR_MIPMAP_LINEAR` (`:205-211`). Cull `FRONT` (`:258`),
  program `depth.frag` (writes the normal).
* `GenerateMipmap` of that 1600x1200 NPOT colour target (`:432-433`).
  Chrome9 does this on the CPU (`C/chrome9_resource.c:755-800`).
* Main pass: `light-refract.frag` with three samplers: depth texture `.x`
  (unit 0), the colour target (unit 1, trilinear), and the image
  (`nasa1`, LINEAR, unit 2). Two `refract()` calls
  (`bcsel(flt(k, 0))` with `sqrt`), three `normalize`, and `pow(.., 100)`.
* Geometry: the default model `bunny.obj` has 69,666 triangles
  (`G/data/models/bunny.obj`), drawn in both passes.

Why no frame appears (**inference**): with 16 triangles per cmd22
submission (`C/chrome9_draw.c:33`) and the measured cost of about 20 ms per
ioctl (`chrome9-render-status-20260924.md:90`), one pass needs at most about
4,350 submissions. That is roughly 45-90 s per pass, depending on the
share of triangles Draw culls. The "no swap in 8 s" result is therefore
expected from submission granularity alone. It does not indicate a hang.
For correctness work, use `refract:model=cube` (`cube.3ds`, 12 triangles):
it exercises the same depth texture, mipmapped R2T and three-sampler chain
in well under 1 s per frame. Whether the refract FS compiles natively was
not checked here. `CHROME9_DEBUG_SHADERS=1` prints
`supported/alu/tau` per variant (`C/chrome9_context.c` in
`chrome9_select_fs_variant`). The open depth-format item remains as in
`chrome9-handoff-2026-09-24.md:157-158`: the default Z16 depth texture
cannot be sampled, and X8Z24 has a zero-sign `bcsel(flt)` gap.

### 3.3 What the llvmpipe fallback must handle for these scenes

Items marked **(ok)** are already covered in the current `C/chrome9_swfb.c`;
the other items are open or at risk.

1. **R2T into mipmapped targets.** Colour targets are aliased as level 0
   only (`chrome9_sw_alias`, `C/chrome9_swfb.c:414-445`), and chrome9
   generates the mips on the CPU afterwards. `chrome9_generate_mipmap()`
   calls `chrome9_draw_flush_batch()` → `chrome9_sw_finish()` before it
   reads level 0 (`C/chrome9_resource.c:785`, `C/chrome9_draw.c:3399-3405`)
   **(ok)**. This requires `level_offset[0] == 0` for every guarded
   mipmapped resource. Assert it in `chrome9_sw_alias()`.
2. **Sampling mipmapped sources** (terrain main: four trilinear sources;
   refract unit 1). `chrome9_sw_texture()` copies every level for every
   fallback draw (`C/chrome9_swfb.c:449-475`) **(ok, correct but costly)**.
   For terrain main this is about 4.5 MB per frame (**inference**). Views
   with `first_level > 0` or a partial `last_level` must keep the view
   levels **(ok, the template view is copied)**.
3. **Depth.** Terrain main uses a Z16 renderbuffer. It is cleared by chrome9
   and then tested only by llvmpipe, so the depth values stay coherent
   within one path. Refract's `DistanceMap` is an X8Z24 texture written by
   the hardware depth pass and sampled by llvmpipe (NEAREST). The X byte
   must be zero (design R2). The depth-texture-mode swizzle comes with the
   view template **(ok)**. Cross-path `EQUAL`/`LEQUAL` (R3) does not occur in
   these two scenes, but it does occur for ioquake3 (section 1.5).
4. **Vertex texture fetch.** The VS sampler states and views are mirrored
   (`C/chrome9_swfb.c:616-632`) **(ok)**. The terrain heightmap is L0, and
   the normal map is L0 with LINEAR.
5. **Sampler state variety**: REPEAT plus `LINEAR_MIPMAP_LINEAR` (terrain
   diffuse), CLAMP/LINEAR, NEAREST/CLAMP (refract depth). The original
   `sampler_states[i]->base` is mirrored, not chrome9's per-draw shadow
   **(ok)**.
6. **Blend**: `SRC_ALPHA, ONE` into an RGBA target (overlay stays native);
   the fallback draws in these scenes are blend-off.
7. **Reference lifetime** (risk). `held[4 * PIPE_MAX_SHADER_SAMPLER_VIEWS]`
   (`C/chrome9_swfb.c:132`) silently skips `pipe_resource_reference` when
   it is full (`:197`). Consecutive fallback draws without an intervening
   finish accumulate held references. A terrain main draw holds about nine
   (RT, ZS, five FS views, two VS views). If a resource is deleted while it
   is pending, llvmpipe can then access freed BO memory. Use
   `util_dynarray`, or call `chrome9_sw_finish()` when the array is full.
8. **Alias bind flags** (risk). The alias cache is keyed only by the
   resource, and `templ.bind = bind | SAMPLER_VIEW` comes from the first use
   (`C/chrome9_swfb.c:417-437`). A texture first sampled and then used as a
   render or depth target in the same pending window keeps an alias
   without `RENDER_TARGET`/`DEPTH_STENCIL`. Set the flags from
   `resource->bind`, or key the cache by (resource, bind).
9. **NIR form** (design R8/R9). `ctx->fs->nir` (`C/chrome9_swfb.c:596`) is
   the chrome9-finalized NIR (`no_integers`, float `front_face`,
   `load_frag_coord` convention). The noise program is the first real
   user. Run the planned host bit-exact test against stock llvmpipe before
   the first target run.
10. **Clears and CPU access.** Queued GPU work is retired before the
    fallback draw (`chrome9_draw_flush_gpu_batch`), and CPU maps go through
    `chrome9_context_sync_resource()` → `chrome9_draw_flush_batch()`
    (`C/chrome9_context.c:92-107`) **(ok)**. `PIPE_MAP_UNSYNCHRONIZED`
    (`:97`) skips the finish. This is only safe for buffers llvmpipe does
    not write.
11. **Odd heights.** A render target with `height % 4 != 0` and no page
    padding cannot be aliased (`C/chrome9_swfb.c:431-435`), and the draw is
    then rejected instead of staged. The canvas sizes used (600, 1200,
    256, 512) are multiples of 4, but other window heights are not.

## 4. Reproduction files

* `B/model.py`: stage model. `B/variants.py`, `B/shift.py`, `B/trunc.py`:
  sensitivity tables. `B/columns.py`: prediction table in 2.5.
* `B/stage_dump.c` / `.so`: `LD_PRELOAD` shim that hooks `dlsym` and
  `glXGetProcAddress`, and dumps the V-pass column and the validation
  `glReadPixels`. `B/rp.c` / `.so`: `glReadPixels` logging only.
* `B/llvmpipe-vpass5.txt`: llvmpipe V-pass column (host, 2026-09-24).
* Host run: `Xvfb :57`, `LIBGL_ALWAYS_SOFTWARE=1`,
  `/home/ai/tmp/gm2src/usr/bin/glmark2 --data-path
  /home/ai/tmp/gm2src/usr/share/glmark2 --validate -b
  desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true`.
