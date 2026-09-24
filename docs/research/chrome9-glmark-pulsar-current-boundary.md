# Chrome9 glmark2 pulsar current boundary

Status: exact default glmark2 2023.01 source, linked-fragment-NIR, and current
Chrome9 state-path audit on 2026-09-24.  This was an offline audit: no target
GPU was used and no production code was changed.

## Exact default scene

The installed default benchmark is:

```text
pulsar:quads=5:texture=false:light=false
```

The audited glmark2 tree is commit
`42e3d8fe3aa88743ef90348138f643f7b04a9237`.  This variant selects
`pulsar.vert` and `light-basic.frag`; their file SHA-256 values are:

```text
pulsar.vert       4ac9ddb6d1d73680bbe0e3dd6a37eb4497e1a227d83e570f968f959ff12bd557
light-basic.frag  49942f9e79a8f856cfe363a3b2fc6e9f9992ef39ce79528cc8de4099d0f29946
```

The vertex stage transforms position and copies the four-component vertex
color.  `TextureCoord` is link-dead because the default does not use a
texture.  The four mesh colors are red, green, blue, and white, with alpha
`.4,.4,.4,1`; the two indexed triangles form one quad.  Five rotated copies
are drawn over the normal canvas depth target.

The scene disables culling, enables blending, and requests exactly:

```text
RGB:   SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD
alpha: ZERO,      ONE,                    ADD
mask:  RGBA
```

The alpha equation preserves destination alpha.  It is intentionally not the
usual `ONE,ONE_MINUS_SRC_ALPHA` source-over alpha equation.

## Linked fragment and native resource shape

The linked fragment NIR has one smooth vec4 `VAR0`, no texture, no uniform,
no arithmetic, and one full color store.  Its NIR source BLAKE3 words from
the current standalone linker are:

```text
5c2a39f6 731967f6 45d968a3 21ee6317
18c80121 7b5cf898 ef8ed9e4 5c931a37
```

At current Mesa Chrome9 HEAD the semantic compiler accepts it as one record:

```text
MOV oC0.xyzw, T0.xyzw
```

The program has one ALU, no TAU, direct varying mask `0x0001`, no constants,
no samplers, and no virtual or physical temporary.  The corresponding native
record and kernel controls are the already-qualified direct-T0 form:

```text
01c00530 301c0053 00f2c005 000d0820
A90=90058001 A91=91000000 A92=92000001
```

This does not introduce a new fragment operation or FE route.  The guarded
direct float4 T0 path has already retired correctly on VX900.

## Current state-path fit

The scene's complete state is inside existing generic paths:

- Gallium Draw performs the vertex shader, clipping, culling-disabled
  primitive handling, and perspective interpolation before cmd22.
- The generic fragment path accepts the one float4 varying and one-record
  program.
- `chrome9_translate_blend()` accepts `SRC_ALPHA/INV_SRC_ALPHA` for RGB and
  `ZERO/ONE` for alpha, with ADD and the full mask.  These are baseline
  factors and do not depend on extended blend-factor capability.
- The canvas enables ordinary D16 depth testing.  The generic path already
  accepts D16 LESS/write state; pulsar adds no depth format or function.
- The default variant has no texture, FBO, mip, discard, comparison, or
  exceptional floating-point requirement.

Consequently there is no missing hardware feature or compiler lowering for
the default pulsar scene.  The remaining claim is application-level image
validation, not another implementation slice.

## Bounded validation gate

On a healthy fresh session, run exactly the default validation case above for
one frame.  glmark2's source checks canvas pixel `(400,299)` against RGB
`(0x77,0x02,0x77)` with Euclidean tolerance just above one 8-bit level and
expects `Validation: Success`.

The run must additionally confirm the Chrome9 renderer, no draw rejection,
no timeout/quarantine/reset message, and a known-good guarded draw afterward.
Do not use the textured or lit pulsar options as substitutes: those add a
sampler or software vertex lighting and have different validation status.

