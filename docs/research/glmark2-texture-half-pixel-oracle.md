# Glmark2 texture half-pixel oracle

This note reduces the September 21, 2026 `glmark2` texture validation failure
to one post-viewport coordinate convention. It does not require a texture
format, pitch, shader, or sampler-state change. The correction is now
live-proven on VX900 by Mesa nested commit `9fc563b` (parent commit
`4b6f0e2`).

## Reproducible inputs

The checked T2 glmark2 2023.01 tree is
`/srv/t2/src.glmark2.x86-64-desktop.260917.193020.2191764/glmark2-2023.01`.
The relevant files and SHA-256 values are:

```text
scene-texture.cpp       e0354687f0f9ba64f94cbd947b7901d0d1c41a62b278e9bd9a0c9af27582b0dc
cube.3ds                8d8b716db79445dbd009da40b788b6c3cd4760035ed17f5dd9b53d1050c799b9
crate-base.png          bf76ecdbab16091fbe63581cf209baf9cb3b8921f94da84b42689c43c6c02102
light-basic.vert        45b986a32e42e5702697c1bbfa394c12e9cee46e3b582495707da4429ee4b250
light-basic-tex.frag    0ec582ee4c2707450fb2a1d03a38e97ffb19fdbd479806b50fa05d1886988321
```

The validation geometry is the unrotated 800 by 600 cube. The vertex shader
normalizes light `(20,20,10)`, computes a scalar diffuse value, and passes the
model texture coordinate. The fragment shader multiplies the texture by that
diffuse color. The Chrome9 classifier packs diffuse and S/T together in
`generic0.xyz`; Mesa converts diffuse to normalized D1 and emits S/T as the
ordinary stage-zero coordinate.

`SceneTexture::validate()` reads GL pixel `(403,303)`. The retained live
results were:

| filter | reference RGB | Chrome9 RGB | distance |
|---|---:|---:|---:|
| nearest | `(43,42,40)` | `(51,50,51)` | 15.779734 |
| linear | `(43,42,40)` | `(48,47,47)` | 9.949874 |

The two different Chrome9 results prove that A31's nearest/linear selection
is live. Changing filter bits cannot explain a common coordinate error.

## Exact reconstruction of both wrong pixels

Parse `cube.3ds` with the same chunk rules as `Model::load_3ds()`, average
face normals as `Model::calculate_normals()` does, and apply the projection
from `SceneTexture::setup()`. `ImageData::load()` reverses PNG rows before
upload, so GL texture row `r` is PNG row `511-r`.

The render target is top-origin below the state tracker. The hardware's
integer interpolation point for GL readback pixel `(403,303)` corresponds to
bottom-origin point `(403,304)`. At that point the visible front triangle
has:

```text
S/T                         0.4899999490 / 0.5074999680
interpolated packed diffuse 91.58750212 / 255
nearest PNG x/y             250 / 252
nearest PNG RGB             141 / 140 / 142
nearest output RGB          51 / 50 / 51
linear source RGB           133.831201 / 132.071253 / 131.491417
linear output RGB           48 / 47 / 47
```

Those are both live Chrome9 results exactly, including channel rounding.
They rule out pitch, channel swizzle, texture upload, and MODULATE arithmetic
as the first cause.

The GL half-integer sample point is instead `(403.5,303.5)`. The same
reconstruction gives:

```text
S/T                         0.4912499488 / 0.5087499693
interpolated packed diffuse 91.57500225 / 255
nearest PNG x/y             251 / 251
nearest PNG RGB             120 / 117 / 112
nearest output RGB          43 / 42 / 40
linear source RGB           120.282261 / 117.282661 / 112.302319
linear output RGB           43 / 42 / 40
```

These are both glmark2 reference results exactly. The high-frequency crate
pixels make this an unusually strong half-pixel discriminator.

## Minimal implementation experiment

Chrome9 consumes integer-centered top-origin post-viewport positions, while
Gallium's GL rasterizer state has `half_pixel_center=true`. Before calling
`chrome9_coord_16_16()` in `chrome9_emit_triangle()`, use:

```c
float bias = ctx->rasterizer && ctx->rasterizer->half_pixel_center ?
             -0.5f : 0.0f;
x[i] = chrome9_coord_16_16(position[0] + bias, width);
y[i] = chrome9_coord_16_16(position[1] + bias, height);
```

Subtracting one half in both top-origin axes makes the hardware integer point
evaluate the original GL triangle at its half-integer center. Do not offset
S/T: a varying-only adjustment would be derivative- and primitive-dependent,
would not correct diffuse or depth interpolation, and would diverge between
textured and untextured draws.

For the first diagnostic, a debug gate may restrict the delta to the packed
gray textured path. The correct final semantic is rasterizer-wide and
conditional on `half_pixel_center`, so promotion must cover every Chrome9
triangle path after validation.

Required gates are:

1. nearest and linear texture validation both become `(43,42,40)`;
2. the complete Mesa hardware smoke, including NPOT, signed-repeat, D16,
   blend, scissor, and guard checks, remains clean;
3. Gouraud validation remains successful;
4. coverage tests explicitly recheck all four target edges, because moving
   post-viewport X/Y changes edge ownership even though packet state and BO
   bounds are unchanged; and
5. old/new/old textured and untextured transitions leave engine status clean.

Do not change A31, A20/A22, texture pitch, MODULATE words, or upload layout in
the same experiment. The exact paired nearest/linear reconstruction makes
those changes both unnecessary and less diagnostic.

## Live closure

The implementation subtracts `0.5f` from both post-viewport X and Y exactly
when Gallium requests `half_pixel_center`. It leaves S/T and all other
varyings unchanged. On the target VX900:

- the complete Chrome9 screen smoke passed, including the guarded texture,
  signed-repeat, D16, blend, scissor, and transition paths;
- `glmark2 --validate` passed the texture cube with both nearest and linear
  filtering, and the Gouraud cat remained successful;
- SWIOTLB occupancy remained 200 and engine MMIO `040c` remained zero; and
- deterministic full-frame captures were retained locally for comparison.

This promotes the post-viewport half-pixel correction from a derived oracle
to live-proven behavior. It also confirms the original diagnosis: no sampler,
texture-layout, format, or MODULATE change was needed.

The same validated build produced these indicative measurements (not a
correctness contract): no-LLVM Draw ran the cat at 6 FPS over 20 frames and
texture at 54 FPS over 200 frames; static LLVM Draw ran the cat at 18 FPS over
20 frames and texture at 51 FPS over 200 frames / 57 FPS over 1000 frames.
Both configurations preserved validation results.

## Unrelated build-scene trap

`SceneBuild::validate()` always compares against `0xa7a7a7`, the reference
for its default horse. It does not select a different reference when
`model=cube` is requested. Consequently an observed cube result near
`0x5a5a5a` is not evidence of broken Gouraud interpolation. The cube's
averaged vertex normals predict that center value; `build:model=cube` must
not be used as a pixel oracle unless its reference is independently derived.
