# Chrome9 ioquake3 first-frame coverage

This note maps the ioquake3 GL1 and GL2 renderers to the bounded Chrome9
Gallium paths. It is a source audit plus one live launch trace, not a claim
that unobserved shaders have the exact final NIR spelling shown below. Each
pattern is labelled **live**, **Mesa-construction exact**, or
**source-semantic** so inferred behavior cannot be mistaken for a target NIR
capture.

The audited source is ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1` in the T2 source tree. The live
VX900 logs are `/root/tmp/chrome9/ioquake3-gl1-probe-v2.log` and
`ioquake3-gl1-probe-v1.log`, captured on 2026-09-21.

## Observed launch boundary

The client defaults to `cl_renderer=opengl2`. On Chrome9 that renderer first
fails its requested OpenGL 3.2 core context, obtains the compatibility context,
reports GLSL 1.20, and then fails in `GLSL_InitGPUShaders()`:

```text
error: Too many fragment shader default uniform block components
error: Too many fragment shader uniform components
ERROR: shaders failed to link
```

This is consistent with the current Chrome9 fragment cap of one 16-byte
constant vector. The GL2 source constructs all generic and lightall variants
at renderer initialization, including fragment programs with more uniforms
and sampler bindings than the bounded executor can run. Raising the reported
limit alone would only move the failure to unsupported NIR and is not a fix.

Explicit `cl_renderer opengl1` gets much farther. The live run proves:

* a GL 2.1 compatibility context, 24-bit color and Z16 visual;
* two fixed-function texture units;
* complete renderer initialization and texture/shader registration;
* loading q3dm1, the game VM, AAS and bot data; and
* the first attempted textured render, rejected cleanly as an unsupported
  bounded fragment program with no reported engine error or wedge.

The trace then restarts the renderer and later receives SIGSEGV while loading
cgame/UI. That crash is not evidence of a GPU fault and should be diagnosed
separately after the first fragment rejection is captured and removed.

## GL1 first-frame patterns

### 1. Loading/UI stretch picture

`RB_SetGL2D()` selects depth disabled and source-over blend. `RB_StretchPic()`
emits two indexed triangles with one 2D coordinate and one RGBA byte color per
vertex. The default 2D shader has `rgbGen vertex`, `alphaGen vertex`, one
texture, and the same blend equation.

Mesa's fixed-function fragment builder constructs the unit-zero MODULATE
operation exactly as:

```text
t0 = tex(unit=0, smooth TEX0.xy)
c0 = smooth COL0
out = t0 * c0
```

This expression is **Mesa-construction exact**: `ff_fragment_shader.c` starts
PREVIOUS at primary color and emits `nir_fmul(texture, previous)` for
MODULATE. NIR copy propagation can exchange the multiply operands and wrap
loads in identity moves, but cannot fold the color array merely because a
particular draw filled it with white.

Required state is:

| state | value |
|---|---|
| textures | one normalized 2D sampler |
| depth | disabled, no writes |
| blend | `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, ADD |
| raster | two-sided triangles; software `draw` may expand indices |
| color | smooth primary RGBA |

The current Chrome9 tree has a closed one-texture-times-primary classifier,
normalized D1 color, BGRA/XRGB targets and canonical source-over. Therefore
the live unsupported-FS result is probably an unrecognized optimized spelling
of this pattern or a different loading shader, not proof that new silicon
state is required. The next run must dump the sole rejected NIR before this
gate is broadened.

### 2. Single-texture opaque entity/world stage

The optimized vertex-lit iterator and the default `LIGHTMAP_NONE` /
`LIGHTMAP_BY_VERTEX` shaders use one texture times a computed or explicit
primary color:

```text
out = tex0(TEX0.xy) * COL0
depth = Z16, LEQUAL, write enabled
blend = disabled
```

This is the same closed fragment key as the 2D path with a different DSA
tuple. Vertex lighting, fixed transforms, clipping and culling can execute in
Mesa draw before the Chrome9 post-TNL callback. One-texture+D16 LEQUAL and
reciprocal-W records are already bounded paths. It still cannot render the
normal game texture correctly with the default mip sampler described below.

### 3. Opaque world lightmap fast path

`RB_StageIteratorLightmappedMultitexture()` enables the color array and points
it at `tess.constantColor255`, selects base coordinates on unit 0, lightmap
coordinates on unit 1, and sets unit 1 to MODULATE. That array is initialized
to all `0xff`, but it remains a runtime vertex input. The fixed-function NIR
is therefore a nested three-factor product:

```text
p0 = tex0(TEX0.xy) * COL0
out = tex1(TEX1.xy) * p0
depth = Z16, LEQUAL, write enabled
blend = disabled
```

This is **Mesa-construction exact** modulo reassociation and operand order.
It is not the existing `tex0 * tex1` key. The known Chrome9 PS successor is:

```text
0140B6D0 30202053 00F20005 0000007F   r1 = r0 * r2
0140B6D0 30040053 00F20105 0000007F   r1 = r1 * v0
01C00000 301C0000 00F20105 000D083F   oC0 = r1
CCCCCCCC DDDDDDDD
```

with PS instruction count `92020003`. This sequence is statically recovered
but not yet a live three-factor silicon oracle. It preserves alpha as well as
RGB. The pending two-texture+D16 ioctl supplies the four-object lifetime and
depth state, but its current two-factor PS would still render the wrong
semantics unless primary color is included.

### 4. Masked surfaces

GL1 shader state maps its three alpha-test forms to:

| ioquake state | GL compare | reference |
|---|---|---:|
| `GLS_ATEST_GT_0` | GREATER | 0 |
| `GLS_ATEST_LT_80` | LESS | 0.5 |
| `GLS_ATEST_GE_80` | GEQUAL | 0.5 |

The fragment color remains a one- or two-stage texture product; alpha test is
orthogonal fixed state. A complete first map needs alpha test combined with
texturing, Z16 LEQUAL and the relevant color-write mode. Current bounded
successor combinations must stay closed until each exact tuple is accepted;
support for alpha test on a depthless blit does not establish the D16 tuple.

### 5. Dynamic light and fog passes

These occur after the opaque base surface and are not the first bring-up gate:

* dynamic light samples `dlightImage`, modulates by primary color, uses
  depth EQUAL with no depth write, and blends either `ONE,ONE` or
  `DST_COLOR,ONE`;
* fog samples `fogImage`, modulates by a constant per-vertex fog color, uses
  source-over, and for `FP_EQUAL` uses depth EQUAL with no depth write.

The current D16 contract is write-enabled LESS/LEQUAL. EQUAL/read-only depth
and the additive/destination-color blend factors are separate required
successors. Fog coordinates are calculated on the CPU, so basic fog does not
need fragment coordinate generation or a general fragment ALU compiler.

## Default mip state is an early independent blocker

Both renderers default `r_textureMode` to `GL_LINEAR_MIPMAP_LINEAR`; the live
GL1 configuration selected `GL_LINEAR_MIPMAP_NEAREST`. `Upload32()` uploads
every level down to 1x1 for images carrying `IMGFLAG_MIPMAP`, then assigns that
minification filter. Ordinary world/base images are registered with
`IMGFLAG_MIPMAP | IMGFLAG_PICMIP`; 2D non-mip images instead use linear
filtering.

Chrome9 currently stores guarded mip-capable resources but only submits a
level-zero sampler view and accepts `PIPE_TEX_MIPFILTER_NONE`. Thus, after the
first NIR classifier fix, a normal world texture is expected to fail sampler
state/view validation or sample only L0 incorrectly. Truthful ioquake
coverage requires the complete mip address table, LOD selection and nearest
and linear mip modes. `r_textureMode GL_LINEAR` is useful as a diagnostic,
not an acceptable final implementation.

## GL2 runtime patterns after the startup link gate

ioquake3 compiles shader variants with preprocessor feature defines, but alpha
test is always a runtime integer uniform. Therefore even the simplest generic
fragment program is not the existing straight-line texture key:

```text
t = tex0(var_DiffuseTex)
a = t.a * var_Color.a
if u_AlphaTest == 1: discard if a == 0
if u_AlphaTest == 2: discard if a >= 0.5
if u_AlphaTest == 3: discard if a < 0.5
out = vec4(t.rgb * var_Color.rgb, a)
```

That shape is **source-semantic** until linked NIR is captured. Because
`u_AlphaTest` is a mutable uniform, setting it to zero for one draw does not
make the branches compile-time dead.

Default GL2 cvars enable normal, specular and deluxe mapping. A collapsed
world lightmap stage selects `LIGHTDEF_USE_LIGHTMAP`; with those defaults the
lightall program samples diffuse and lightmap and may also sample normal,
deluxe and specular maps (or bound white substitutes), then performs tangent
space normalization, dot products, attenuation and specular calculations.
That is outside every current closed key and exceeds the two-unit executor.

With `r_normalMapping 0`, `r_specularMapping 0`, deluxe/cube/shadows disabled,
the fast-light semantic core reduces to:

```text
light = var_Color.rgb * tex1(lightCoord).rgb
diffuse = tex0(diffuseCoord)
alpha = diffuse.a * var_Color.a
runtime alpha-test branch
out.rgb = diffuse.rgb * light
out.a = alpha
```

Ignoring the still-required runtime alpha test, this is again the GL1
three-factor base*lightmap*primary operation, not plain two-texture MODULATE.
The reduced cvars are a useful compiler probe but do not bypass the startup
uniform contract or justify a general GLSL claim.

## Ranked blockers against current Chrome9

| rank | renderer/path | first missing block | evidence | wedge risk |
|---:|---|---|---|---:|
| 1 | GL1 first live textured draw | capture and classify the exact rejected NIR; likely one texture times primary | live rejection; source-derived expected key | low |
| 2 | GL1 normal textured assets | full mip view/address/LOD and mip-nearest/mip-linear filters | source and live texture mode | medium |
| 3 | GL1 opaque world | two textures times primary plus Z16 LEQUAL/write | exact Mesa construction; static PS bytes | medium-high |
| 4 | GL1 masked world | alpha test combined with texture(s) and D16 | exact GL state mapping | medium |
| 5 | GL1 fog/dynamic light | read-only EQUAL depth plus source-over/add/DST_COLOR blend tuples | exact GL1 backend state | medium |
| 6 | GL1 throughput | indexed/batched triangle submission and state reuse | source draw topology | low correctness, high performance value |
| 7 | GL2 initialization | truthful larger FS uniform model and enough samplers, followed by verified shader execution | live link failure | high |
| 8 | GL2 generic UI/entity | dynamic alpha-test control flow around texture*primary | exact GLSL source | high |
| 9 | GL2 default lightall | 4+ samples and general normalize/dot/specular ALU | exact feature defines/source | very high |

The GL1 route is therefore the only bounded near-term game path. The safest
next evidence sequence is: dump the first rejected GL1 NIR; rerun with a
non-mip diagnostic filter to expose its next state gate; validate the
two-texture+D16 four-BO oracle; then validate the three-factor PS with distinct
non-white primary colors. GL2 should remain fail-closed until its startup
shader set is representable rather than papered over with larger caps.

## Exact capture checklist

For each first newly admitted draw, archive:

1. linked FS NIR and source hash;
2. texture count, view levels, sampler min/mag/mip filters and wrap;
3. color/depth formats, depth enable/write/function, blend factors, alpha
   function/reference and color mask;
4. post-TNL output semantic layout and reciprocal W;
5. submitted/rejected counter deltas and selected ioctl/program key;
6. target/depth/texture guard verification, retirement and MMIO error state.

Do not treat the current post-restart SIGSEGV, an application return code, or
an FPS line as evidence that any pixel path passed.
