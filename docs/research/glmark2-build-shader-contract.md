# glmark2 `build:model=cube` shader contract

This note bounds the shader work required by the observed glmark2 2023.01
`build:model=cube` path.  It separates target-captured NIR from offline GLSL
compiler output and from native Chrome9 shader reverse engineering.  The
conclusion is deliberately narrow: Mesa `draw` already executes the required
vertex program in software.  A later full live dump corrected the initial
interpretation of the linked fragment program: it carries a `vec3` RGB
varying, not one replicated scalar.  The remaining shader-related correctness
gaps are the exact RGBX classifier and perspective interpolation, not a
missing vertex ALU lowering.

This does not authorize advertising general GLSL.  The implementation remains
a collection of exact fragment-program classifiers, and its untextured
post-TNL ABI currently loses reciprocal W.

## Application shaders

The target capture for this benchmark used the following GLSL 1.20 vertex
program (whitespace omitted here only for brevity):

```glsl
const vec4 MaterialDiffuse = vec4(1.0, 1.0, 1.0, 1.0);
const vec4 LightSourcePosition = vec4(20.0, 20.0, 10.0, 1.0);
attribute vec3 position;
attribute vec3 normal;
attribute vec2 texcoord;
uniform mat4 ModelViewProjectionMatrix;
uniform mat4 NormalMatrix;
varying vec4 Color;
varying vec2 TextureCoord;

void main(void)
{
    vec3 N = normalize(vec3(NormalMatrix * vec4(normal, 1.0)));
    vec3 L = normalize(LightSourcePosition.xyz);
    float diffuse = max(dot(N, L), 0.0);
    Color = vec4(diffuse * MaterialDiffuse.rgb, MaterialDiffuse.a);
    TextureCoord = texcoord;
    gl_Position = ModelViewProjectionMatrix * vec4(position, 1.0);
}
```

The fragment program is only:

```glsl
varying vec4 Color;
varying vec2 TextureCoord;

void main(void)
{
    gl_FragColor = Color;
}
```

The fragment shader does not sample a texture.  `TextureCoord` is consequently
dead at link time.  The `use-vbo` and interleaved-attribute benchmark choices
change vertex storage, not these shader semantics; as documented in
[indexed-vbo-topology-plan.md](indexed-vbo-topology-plan.md), this benchmark
also expands the cube to 36 vertices and issues `glDrawArrays(GL_TRIANGLES)`.

## Exact captured fragment NIR

The `NIR_DEBUG` target capture in
`/root/tmp/chrome9/glmark2-build-cube-86c.out` identified the linked fragment
shader with these `source_blake3` words:

```text
8105bebf 60cef3c7 c9c3e978 d20442bc
46d83156 9a4abb0b d1a4de24 422a9790
```

Its only live input is the 32-bit highp `vec3`
`Color (VARYING_SLOT_VAR0.xyz)`.  The executable expression is exactly the
following shape, allowing ordinary NIR `mov` wrappers:

```text
rgb = load smooth VAR0.xyz
o = vec4(rgb.x, rgb.y, rgb.z, 1.0)
store_output FRAG_RESULT_COLOR, o.xyzw
```

There is one output store, no texture instruction, no fragment constant, no
discard, and no control flow.  This target capture, rather than the unlinked
GLSL declarations, is the contract the fragment classifier sees.

## Offline vertex lowering

Mesa 26.2.2's host `glsl_compiler --version 120 --link --dump-lir` reproduces
the application source above without hardware access.  It proves these
operation classes:

1. `normalize(vec3(20,20,10))` folds to the exact constant
   `(0.666667, 0.666667, 0.333333)`, mathematically `(2/3,2/3,1/3)`.
2. Construct `(normal.xyz, 1)`, multiply it by `NormalMatrix`, and retain xyz.
3. Normalize the transformed normal as
   `n * rsq(dot(n,n))`.
4. Compute `max(dot(normalized_n, constant_light), 0)`.
5. Export the three-component diffuse result in RGB and constant `1.0` in
   alpha.  Although this GLSL computes the same diffuse scalar in each channel
   for a white material, the linked interface remains `vec3`.
6. Construct `(position.xyz, 1)` and multiply by
   `ModelViewProjectionMatrix` for clip position.
7. Pass through `texcoord`, which full VS/FS linking can delete because the
   fragment shader never consumes it.

Items 1--7 are offline compiler evidence.  The precise linked VS NIR textual
form can vary with linker and NIR optimization order; do not key any Chrome9
acceptance rule to one incidental sequence of `fadd` versus `ffma`.  A future
target capture may use `NIR_DEBUG=print_vs` for the same one-frame benchmark
to archive the final DAG, but no additional live run is necessary to decide
the current execution path.

## Why the current software VS is sufficient

`chrome9_create_vs_state()` passes the NIR shader directly to
`draw_create_vertex_shader()`.  `chrome9_init_shader_caps()` starts with
`draw_init_shader_caps()`, retains NIR as the supported IR, and disables only
the unimplemented VS sampler/image/buffer/atomic interfaces.  The draw call
then:

* maps every active vertex buffer into `draw`;
* maps every VS constant buffer into `draw`;
* invokes `draw_vbo()` and `draw_flush()`; and
* receives post-clipping, viewport-mapped vertices through the Chrome9
  `vbuf_render` stage.

Matrix multiplication, dot products, reciprocal square root, multiply, max,
uniform loads, clipping, and viewport transformation therefore run in Mesa's
software vertex pipeline.  They do not require a native Chrome9 VS compiler
or new kernel shader-upload ABI for this benchmark.  Vertex-buffer versus
client-array storage is also already abstracted before shader execution.

The required safety gates remain the existing ones: all referenced buffers
must map, indirect draws remain rejected, the fragment program must classify,
and the vbuf output layout must match the classifier-selected layout.

## Exact bounded fragment implementation

The earlier scalar-blue/scalar-gray implementation does not recognize this
live shader and must not be treated as validation.  The correct classifier
accepts the target shape only when all of these conditions hold:

* the source is 32-bit `VARYING_SLOT_VAR0`, components XYZ;
* the linked source has exactly three components;
* R, G, and B preserve source components X, Y and Z respectively;
* alpha is the exact constant `1.0`;
* only bounded whole-vector `mov` or `fsat` wrappers are present, with
  recursion depth at most eight;
* there is one accepted color result, no texture, and no constant buffer.

The selected vbuf layout is nine dwords:

```text
D0.xyzw      position, EMIT_4F
generic0     RGB-bearing generic slot, EMIT_4F
secondary    COLOR1, EMIT_4UB_BGRA
```

`chrome9_emit_triangle()` checks that `generic0.xyz` are finite, clamps and
rounds each with `float_to_ubyte()`, and forms opaque packed RGB:

```text
B | G << 8 | R << 16 | 0xff000000
```

The existing untextured primary-color pixel program then consumes the packed
diffuse color.  Consequently **no additional fragment instruction or native
pixel-shader blob is needed for the captured cube shader**.  Variants which
retain a texture lookup, use a non-white alpha expression, add specular or
ambient arithmetic in the fragment shader, or use another varying shape
remain outside this classifier and must fail closed.

There is an intentional precision limitation: the software varying is
quantized to an 8-bit value at each vertex before hardware interpolation.
GLSL specifies interpolation of the float varying followed by conversion to
the render-target format.  The two differ for some gradients even after
perspective correction is fixed.  This classifier is a bounded bring-up path,
not evidence of general GLSL varying precision or conformance.

## The remaining perspective-correction defect

The target varying is ordinary GLSL `smooth`, so each RGB component must be
perspective-correct.  The current untextured/D16 path cannot do that:

* `struct drm_chrome9_vertex_d16` carries only fixed-point X, Y, Z and packed
  color; it has no reciprocal-W field.
* `chrome9_emit_triangle()` preserves post-viewport `position[3]` only for
  textured vertex ABIs.  Untextured batch records discard it.
* `chrome9_build_depth_triangle()` emits D0.w as constant `0x3f800000` for
  every vertex.
* its type-01 A01 word is `01c0154c`, whose diffuse perspective-correction
  bit 17 is clear.

The vendor GL initialization word is `01ce1540`; preserving the D16 bits gives
`01ce154c`.  Bits 17, 18, and 19 enable diffuse, specular, and fog correction,
respectively.  Changing A01 alone is insufficient while every D0.w remains
one.

The safe successor must be a new ABI, not an in-place enlargement of the
existing structure:

1. add an untextured post-TNL vertex record with X/Y/Z, `rhw_float`, and
   packed diffuse color;
2. provide corresponding one-triangle and bounded-batch requests/capability;
3. require finite, positive RHW with the existing `chrome9_rhw_valid()` rule;
4. copy post-viewport `position[3]` to D0.w without conversion;
5. emit vendor-exact type-01 `A01=01ce154c` for the generalized D16 path;
6. retain normalized packed-color FVF low byte `d1`, not the rejected `d0`;
7. leave the old ABI and packet byte-for-byte unchanged for regression and
   rollback.

This successor is needed for the benchmark to be semantically correct under
non-affine model-view-projection matrices.  A process exit of zero or an FPS
line does not prove it: `/root/tmp/chrome9/glmark2-build-lequal.log` shows the
benchmark completing, but the result still needs a pixel oracle and the
presentation path can independently hide rendering.

## Cross-check against the recovered native lighting shader

[shader-isa.md](shader-isa.md) records an offline proprietary
one-directional-light VS with a 13-record semantic prefix followed by the
seven-record Windows finalizer.  The complete 20-record native blob has
SHA-256
`a91278efd965fa905c59dbc12a40a11b434f5ffb402b111fa4cc85785525f19f`;
the Linux 5.75 compiler independently reproduces the semantic prefix with
SHA-256
`c1e7685e30c0f83290a85167e40a2edf3b0150649fcaf319f7f8efd62c672ee1`.
It demonstrates native M4x4/M3x3, DP3, MAX, MUL/MAD and position/diffuse
output encodings.  The matching PS is the one-record `MOV oC0,v0`:

```text
01c00000 301c0000 00f04005 000d083f
```

That oracle is valuable ISA evidence but is not byte-for-byte the glmark2
shader.  The proprietary fixed-function form has its own ambient/material
constant layout, while glmark2 explicitly normalizes the transformed normal,
uses a compile-time normalized light direction, and exports a linked RGB
varying.  Do not insert the native fixed-function blob into this path;
the current software VS is both more general and already wired.

## Bounded validation plan

### Offline classifier corpus

The smoke corpus must include a positive NIR case matching the captured
`vec4(rgb.x,rgb.y,rgb.z,1)` graph under D16 and
reject at least these single-delta mutations:

* input location other than `VAR0` or non-XYZ component order;
* fewer/more than three source components or a component swizzle;
* nonconstant or non-one alpha;
* texture, fragment constant, discard, branch, second output, or second
  store;
* 16/64-bit input or a wrapper chain beyond the fixed depth.

Also keep the existing packed grayscale texture-modulate vectors separate;
they are not part of this untextured program.

### Software-VS reference

With identity matrices, normals `(1,0,0)`, `(0,1,0)`, `(0,0,1)`, and
`(-1,0,0)` must produce RGB diffuse values `(2/3)^3`, `(2/3)^3`, `(1/3)^3`,
and `(0,0,0)` for the white material.
Use a nontrivial normal matrix as a second case to ensure the normal is
renormalized after transformation.  Inspect the vbuf callback output, not
just shader creation, and require finite post-viewport XYZRHW.

### Perspective varying oracle

Reuse the geometry in
[raster-front-end-oracles.md](raster-front-end-oracles.md): reciprocal W is
`1, 1/4, 1/4`, v0 grayscale is black, and v1/v2 grayscale is white.  At pixel
`(48,16)`, the expected perspective value is

```text
255 * 97/221 = 111.9, normally 0x70
```

while affine interpolation gives

```text
255 * 97/128 = 193.2, normally 0xc1.
```

Accept a small subpixel/rounding band of `0x6f..0x74`; `0xc0..0xc4` proves
lost RHW or disabled diffuse correction.  Run this first against a guarded
offscreen BGRX target with D16, verify both guard pages and unchanged source,
and require idle engine/error-zero status.  Only then route the successor ABI
from the Mesa RGBX classifier.

Because the existing classifier quantizes each endpoint to 8 bits, add a
separate low-gradient test whose vertices differ by less than `1/255`.  It is
expected to expose the stated precision limitation and must not be counted as
a float-varying conformance pass.

### Application matrix

After the RHW successor passes, run `build:model=cube:nframes=1` for client
arrays/VBOs and interleaved/non-interleaved storage.  Require identical
offscreen readback checksums and nonzero changed-pixel counts, not merely the
FPS report.  Keep `MMIO 040c=0`, command retirement, D16 guards, and teardown
checks in the acceptance record.  Presentation/window visibility is a
separate KMS/Present criterion.

## Capability consequence

The captured benchmark requires no new advertised FS instruction class.
Until reciprocal-W and the endpoint-precision policy are resolved, however,
successful execution is a targeted benchmark milestone rather than grounds
to claim fully conformant GLSL 1.20 smooth varyings.  General lighting,
multiple varyings, arbitrary fragment ALU, native VS execution, and the
proprietary eight-light fixed-function builder remain separate future work.
