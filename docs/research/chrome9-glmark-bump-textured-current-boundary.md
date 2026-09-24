# Chrome9 glmark2 textured-bump current boundary

This note closes the offline fragment-program question for the installed
default-suite `bump:bump-render=normals` and `bump:bump-render=height` rows.  It
does not claim a live rendering result.  The result is that neither shader has
a deterministic fragment compiler or kernel-resource rejection in the current
Mesa/kernel pair.  The next useful discriminator is an application validation
run, not another feature-shaped compiler special case.

## Provenance and exact inputs

The application source is
`src.glmark2.x86-64-desktop.260917.193020.2191764/glmark2-2023.01/src/scene-bump.cpp`.
The installed shader files and SHA-256 values are:

| variant | stage | installed shader SHA-256 |
| --- | --- | --- |
| normals | VS | `6712adf4f05b253db428e4b76519020fe5b30b73eebc67d6fdd5d5c880df3d63` |
| normals | FS | `c599bd6c8602a7378d1e70bb1b3b7dd38fbfd53c19609b601cb1583242dd8b79` |
| height | VS | `189b99f7012547ee8854ae42ee93539c5ebe19c50b7ad06f2a5f915fff0e8ad7` |
| height | FS | `67ac4beda3cb5f0134d7e8cf68f693d7fb35f2369b207db43d797a1c26cf3f4a` |

`SceneBump` injects these source constants before GLSL compilation:

```text
LightSourcePosition = vec4(20, 20, 10, 1)
LightSourceHalfVector = vec3(0.408248, 0.408248, 0.816497)
TextureStepX = TextureStepY = 1 / 1024
```

The last two occur only in the height variant.  `ShaderSource` formats the
step as `0.000977` at its default six-place precision.  The audit used the
current Chrome9 compiler at nested Mesa commit
`4aaa1cf1ae89b5b06d3ea304a2b10df4456e56f6` and the current kernel allocator
at nested Chrome9 commit `565698261d40773053fe7c9ea7ca694a8070674c`.

The source-exact host probe performed the GLSL 1.20 link, then the state
tracker operations relevant to Chrome9: lower default uniforms to unpacked
vec4 UBO loads, lower sampler dereferences, run `gl_nir_opts`, shrink vectors,
and DCE.  The resulting fragment NIR identities were:

| variant | linked/final NIR `source_blake3` | transcript SHA-256 |
| --- | --- | --- |
| normals | `{0381665b,5a2e8f14,27a13375,107aa320,afed7027,85be1ccd,5c231b46,734b7004}` | `9421bf7d8c9723705ccbc450a7cd5a2db1534ae660f3fda221185b52b88989bc` |
| height | `{f9ca6c0b,82e0b767,6a927a75,c03efc45,df053000,b4baf944,d80a83ab,d22661c1}` | `696ea0f1b3d6695b904d290c4576e98af216a49f7f5d77123e036b3f8abe09b1` |

The transcript hash covers the printed finalized NIR, every semantic record
including sources and swizzles, and the compiler summary.  It is an audit
identity, not a Mesa cache key.

## Normals fragment program

The linked program has one smooth `vec2` input and one `sampler2D` fetch.  It
decodes the object-space normal with `sample.xyz * 2 - 1`, transforms it by the
four rows of `NormalMatrix`, normalizes it with DP3/RSQ/MUL, and evaluates the
same ambient, diffuse, and integer-power Blinn-Phong core as the plain bump
program.

Current generic lowering produces:

```text
semantic records: 28
semantic ALU:     27
semantic TEX:      1
op inventory:     MUL 15, ADD 6, MAX 2, DP3 3, RSQ 1, TEX 1
varyings:          T0.xy
samplers:          s0
uniform constants: c0-c3 (NormalMatrix)
immediates:        c29-c31
constant count:    32 (high-water ABI count, not 32 live vectors)
```

The kernel TEX reads T0 directly, so it adds no computed-coordinate bridge.
The expected encoded program is therefore 27 native ALU and one TAU.  A model
of the kernel's exact allocate-before-release algorithm peaks at four of the
twelve physical temporary registers.  Its resource controls are consequently
in the `physical <= 4` bucket: A90 `0x90010000`, A91 `0x91000001`, and A92
`0x9201001b`.

Mesa's separate offline native-object compilation reports invalid for this
program.  That is not the generic draw gate: `chrome9_use_generic_fragment_program()`
uses the valid semantic program, and the DRM ioctl deliberately leaves final
allocation and encoding to the kernel.

## Height fragment program

The source declares four varyings, but the GLSL linker packs their eleven live
components into exactly three hardware inputs:

```text
T0.xyzw = TextureCoord.xy, NormalEye.xy
T1.xyzw = NormalEye.z, TangentEye.xyz
T2.xyz  = BitangentEye.xyz
```

This distinction matters: the height variant does not require four Chrome9
varying slots.  Its three height samples all use logical sampler zero.  The
first reads T0.xy directly; the +X and +Y samples use computed coordinates.

Current generic lowering produces:

```text
semantic records: 37
semantic ALU:     34
semantic TEX:      3
op inventory:     MOV 2, MUL 18, ADD 8, MAX 2, DP3 3, RSQ 1, TEX 3
varyings:          T0.xyzw, T1.xyzw, T2.xyz
samplers:          s0 (three instructions, one descriptor)
uniform constants: none
immediates:        c28-c31
constant count:    32 (high-water ABI count)
```

The kernel adds one ALU-to-TAU coordinate bridge for each computed TEX.  The
expected encoded result is therefore 36 native ALU and three TAU records, well
inside the independent 96-ALU and 32-TEX stores.  Allocate-before-release again
peaks at four of twelve physical temporaries.  The three direct varying
coordinates plus computed coordinate IDs 3 and 4 fit the resource bucket;
the derived controls are A90 `0x90050007`, A91 `0x91000019`, and A92
`0x92030024`.

## Texture and fixed state

Both scenes use one 1024x1024 texture at unit zero, `GL_NEAREST` minification
and magnification, no mipmaps, and clamp-to-edge in S and T.  The normal map is
an 8-bit RGB PNG and follows the supported BGRX upload route.  The height map
is an 8-bit gray+alpha PNG; this glmark2 loader presents every non-RGB image to
`glTexImage2D` as RGBA, so the relevant Chrome9 resource is the supported BGRA
route rather than a new native gray sampler format.  The height shader's three
fetches therefore exercise repeated TEX with one sampler, not three sampler
slots.

The scene also updates MVP and normal matrices for the software-draw vertex
path and renders the default depth-backed framebuffer.  These are draw/state
requirements independent of the fragment instruction-store counts above.

## Capability boundary and next test

Current Mesa advertises eight fragment inputs and eight sampler slots when the
kernel reports `DRM_CHROME9_CAP_FRAGMENT_EXTENDED_LINKAGE`.  The matching
current kernel advertises that capability whenever the engine is ready.  The
legacy fallback with `DRAW_TWO_TEXTURED_TRIANGLE` advertises three inputs and
three sampler slots, which still fits height exactly.  Only the older two-input
fallback deterministically rejects height at GLSL linkage; that is a stale
kernel/capability boundary, not a missing height-shader operation.

Normals additionally requires the current 32-constant capability because its
four matrix rows occupy c0-c3 and compiler immediates occupy c29-c31.  The
matching engine-ready kernel advertises `DRM_CHROME9_CAP_FRAGMENT_CONSTANTS_32`.

Thus the current offline ladder closes without a fragment blocker:

1. both shaders link after the real glmark constant injection;
2. current Mesa lowers both to valid generic semantic IR;
3. sampler, varying, constant, ALU, and TEX limits fit;
4. the kernel allocator model fits both with four physical temporaries;
5. native resource selectors fit the characterized resource bucket.

The next bounded tests are live
`glmark2 --validate --off-screen --frame-end=readpixels -b
bump:bump-render=normals:nframes=1` and the corresponding
`bump-render=height` row, first at rotation zero.  The center-pixel oracles in
`SceneBump::validate()` are respectively
`0xa4a4a4ff` and `0x9d9d9dff`, with RGB Euclidean tolerance
`sqrt(3) + 0.01`.  A failure there should be localized as vertex packing,
constant upload, texture upload/swizzle, depth/raster state, or a native
encoding discrepancy; it should not be answered by scene-name special casing.
