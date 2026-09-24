# Chrome9 glmark2 Gouraud current boundary

Status: exact glmark2 2023.01 source, linked-fragment-NIR, compiler, and
draw-path audit on 2026-09-24.  This was an offline audit: no target GPU was
used and no Mesa or kernel production code was changed.

## Exact scene source

The audited glmark2 tree is commit
`42e3d8fe3aa88743ef90348138f643f7b04a9237`.  The default
`shading:shading=gouraud:model=cat` setup selects `light-basic.vert` and
`light-basic.frag`, then injects these vertex constants:

```glsl
const vec4 MaterialDiffuse = vec4(0.0, 0.0, 1.0, 1.0);
const vec4 LightSourcePosition = vec4(20.0, 20.0, 10.0, 1.0);
```

The exact file and final `ShaderSource::str()` hashes are:

```text
light-basic.vert file       45b986a32e42e5702697c1bbfa394c12e9cee46e3b582495707da4429ee4b250
light-basic.frag file       49942f9e79a8f856cfe363a3b2fc6e9f9992ef39ce79528cc8de4099d0f29946
post-add_const vertex       03985ce05541b0f6b4462f106a4d4c8bc47dc59cb209f9868bb036ec21cb8b74
post-precision fragment     692b9ec85475f3392f39ae011c4489d97cc9e69e70ec09e302ae8f2f344fcf60
```

`ShaderSource::add_const()` inserts each new definition at global offset
zero.  Because glmark2 adds `LightSourcePosition` first and
`MaterialDiffuse` second, the material declaration precedes the light in the
hashed final vertex source.  The fragment hash includes glmark2's default
GL-ES precision preamble; neither hash is a hand-normalized source digest.

The vertex shader computes:

```text
N        = normalize((NormalMatrix * vec4(normal, 1)).xyz)
L        = normalize((20, 20, 10))
diffuse  = max(dot(N, L), 0)
Color    = vec4(diffuse * (0, 0, 1), 1)
position = ModelViewProjectionMatrix * vec4(position, 1)
```

`TextureCoord` is source-written but fragment-dead in this scene.

## Exact linked fragment program

Mesa's standalone GLSL linker followed by the Chrome9 NIR options, vector
shrinking, and DCE produces a printed fragment NIR SHA-256 of
`0e817a1debc1bc31df0dccc62325fbd0c37af03b0687f4455c3fd10e4f6efe53`.
Its source BLAKE3 words are:

```text
8105bebf 60cef3c7 c9c3e978 d20442bc
46d83156 9a4abb0b d1a4de24 422a9790
```

The whole live expression is:

```text
rgb    = smooth vec3 VAR0.xyz
result = vec4(rgb.x, rgb.y, rgb.z, 1.0)
store FRAG_RESULT_COLOR.xyzw = result
```

There is one varying slot, no texture, no uniform, no flow, no side effect,
and no fragment arithmetic.  The link has already removed `TextureCoord` and
the unused alpha input lane.  The earlier scalar-blue description is not the
current linked contract.

At Chrome9 Mesa commit
`21b61d081b0d992b560e876ef834dfdb1e9f7171`, the current semantic compiler
accepts that NIR and emits exactly:

```text
MOV t0.xyz, T0.xyz
MOV t0.w,   c31.w              # c31.w = 1.0
MOV oC0,    t0
```

The compile result is `supported=1`, `semantic_fs_valid=1`, three ALUs, zero
TAUs, varying mask `0001`, constant/immediate mask `80000000`, and no primary
color input.  Its exact userspace-native records are:

```text
01c00530 501c0053 0072c005 000d0020
01c00530 801c0053 00809fb6 000d0020
01c00530 301c0053 00f20005 000d0820
```

The kernel compiler sees one logical varying of width three, one virtual temp
with complete XYZ/W definitions, and an ALU-first program.  Its derived
controls are therefore:

```text
A90 = 90058001   # ALU start, resource bucket 0x18000, direct T0
A91 = 91000000
A92 = 92000003   # zero TAU, three ALU
```

This is a generic vec3-varying plus literal-lane construction.  Nothing in the
compiler needs to recognize glmark2, the cat mesh, blue material, or a shader
hash.

## Current execution paths and the first real boundary

There is no Gouraud correctness blocker in either shader stage:

* `chrome9_create_vs_state()` installs the vertex shader in Gallium Draw.
  Draw executes both matrices, normalization, dot/max, clipping, culling,
  viewport transform, and varying export in software.  Chrome9 has no native
  Mesa vertex/TCL compiler selection in this path, so the first missing
  hardware-vertex feature is the native VS backend as a whole, not an opcode
  in the Gouraud shader.
* `chrome9_fs_classify()` recognizes the linked RGBX form as
  `CHROME9_FS_PRIMARY_GENERIC_RGBX`.  Independently, the semantic backend
  compiles the same fragment expression into the three native MOVs above.
  Thus there is no missing fragment opcode, linker semantic, sampler, or
  constant feature for this scene.
* The cat uses ordinary mapped vertex/index buffers, one guarded color target,
  and the established depth path.  Prior current-driver runs have reported
  glmark2 `Validation: Success`, so there is no unresolved resource-format or
  draw-state blocker for correctness.

There is, however, a deterministic performance boundary.  When cmd22 is
available, `chrome9_vbuf_get_vertex_info()` sets `generic_program` from
`chrome9_use_generic_fragment_program()` before considering the closed
`PRIMARY_GENERIC_RGBX` route.  The current Gouraud draw therefore emits
POSITION plus one four-float generic attribute and submits cmd22, even though
the older closed route can pack the same finite RGB result into normalized D1
and use the primary-color RHW run/chain.

The generic userspace batch is hard-coded to 16 triangles while the UAPI
allows 128.  A measured cat frame contains 8,043 triangles, giving 503 cmd22
submissions at that cap.  This is the first current draw/submission cost after
shader compilation; it is not a missing GPU feature.  Increasing the cap
requires the kernel builder's pre-write stage bound first.  Preferentially
routing this exact already-classified RGBX shader through normalized D1 is a
separate generic optimization and must retain the cmd22 path for arbitrary
fragment programs.

## Regression shape

The smallest truthful offline compiler regression should build the linked
shape, not identify the scene:

1. Create one smooth, 32-bit, three-component `VAR0` load.
2. construct `vec4(VAR0.x, VAR0.y, VAR0.z, 1.0)` and make one full color
   store;
3. require the closed classifier result `PRIMARY_GENERIC_RGBX`;
4. independently require semantic validity, three MOVs, `varying_mask=1`,
   `immediate_mask=BIT(31)`, the exact three records above, and no TAU;
5. feed those semantic records to the kernel compiler offline and require
   varying width three and A90--A92 `90058001/91000000/92000003`;
6. reject the *closed D1 classifier* for channel reordering, alpha other than
   exact one, partial stores, flat/sample/centroid interpolation, a second
   varying, or arithmetic.  Those forms may remain valid generic cmd22
   programs; the regression must not globally reject them.

The guarded hardware regression should use one triangle or quad with three
different finite vertex RGB values and alpha one, D16 enabled, and protected
color/depth edges.  It must check an interior interpolated pixel, all guards,
submitted-versus-rejected counters, and no engine error.  If route selection
is optimized later, run the identical oracle once through cmd22 and once
through the D1 primary path and require equal RGB within the established
8-bit pack/interpolation tolerance.  The unchanged real
`shading:model=cat:shading=gouraud` glmark2 validation remains the application
gate.
