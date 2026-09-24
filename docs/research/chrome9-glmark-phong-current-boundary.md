# Chrome9 glmark2 Phong current boundary

Status: exact default one-light glmark2 2023.01 source, linked-fragment-NIR,
compiler, and kernel-resource audit on 2026-09-24.  This was an offline audit:
no target GPU was used and no Mesa or kernel production code was changed.

## Exact generated source

The audited glmark2 tree is commit
`42e3d8fe3aa88743ef90348138f643f7b04a9237`.  The default
`shading:shading=phong:model=cat:num-lights=1` setup selects
`light-phong.vert` and generates the fragment source from `light-phong.frag`.
The exact hashes are:

```text
light-phong.vert file       8fcc96b66863adae3ec6f11e1f251fd54ce6b32b39400ebb390d23ef0122dbea
light-phong.frag file       87d06398e1cec6f50a906d5a5b9478d8e13e72a0e7f50bb2b3311f45397f95ba
post-ShaderSource vertex    f08b42aa97b7ceaaa5b7aa785fc64b46d7267680df98b177dd95be2fb6431dd0
post-ShaderSource fragment  2389fbaee2366a28a1b279b583200cae66b0f2778c19d11a1df228b0c42eff7a
```

For one light, glmark2 inserts `LightColor0=(0.8,0.8,0.8,1)` and
`LightSourcePosition0=(0,1,0,1)`, replaces `$DO_LIGHTS$` with one
`compute_color()` call, and later inserts `MaterialDiffuse=(0,0,1,1)`.
`ShaderSource::add_const()` always inserts at offset zero.  The light color
therefore precedes the light position.  The fragment source is first returned
through one `ShaderSource::str()`, then appended to a second `ShaderSource` and
returned again; its exact hash includes both generated precision preambles.
These are hashes of the bytes passed to Mesa, not normalized GLSL.

The vertex shader remains in Gallium Draw software.  It exports eye-space
`vertex_position` as a vec4 and normalized eye-space `vertex_normal` as a
vec3, while separately producing clip position.

## Exact linked fragment program

Mesa's standalone GLSL linker followed by the Chrome9 NIR options, vector
shrinking, and DCE produces a printed fragment NIR SHA-256 of
`1462558e412a9b59dbfb6965adb16c9b510cdcb9d300787556c0dcd5a1e73ef7`.
Its source BLAKE3 words are:

```text
3d080f6d 14ba2cb7 e3f91818 58494ed6
da070168 6aae4b78 6196ccdb 92c4f445
```

The linked inputs are smooth vec4 `VAR0` (`vertex_position`) and smooth vec3
`VAR1` (`vertex_normal`).  There are no textures, uniforms, branches, or
side effects.  After constant folding, the live expression is:

```text
P       = VAR0.xyz / VAR0.w
L       = normalize((0, 1, 0) - P)
N       = normalize(VAR1.xyz)
R       = reflect(-L, N)
E       = normalize(-VAR0.xyz)
spec    = max(dot(R, E), 0) ^ 100
diffuse = max(dot(N, L), 0)
color   = (0.8,0.8,0.8,1)*spec + (0.02,0.02,0.02,1)
        + (0,0,0.8,1)*diffuse
```

At Chrome9 Mesa commit `284e01a`, the semantic compiler accepts this exact
linked NIR and emits 37 ALU records, zero TAU records, direct varying mask
`0x0003`, and immediates c30/c31.  The operations comprise one RCP, three
RSQ, six DP3, two MAX, four ADD, and 21 MUL records.  The integer POW100
lowering is the exact eight-MUL chain x2, x3, x6, x12, x24, x25, x50, x100.
The complete dependency schedule is recorded in
`chrome9-glmark-phong-semantic.tsv`.

This fits the current structured-program envelope without consuming a
sampler: 37 of 96 ALU/instructions, zero of 32 TEX instructions, and zero of
three independently advertised sampler slots.  The three-slot descriptor
limit is not a three-TEX-instruction limit.

## Kernel resource fit

Applying the kernel allocator's allocate-before-releasing-last-use rule to
the exact schedule peaks at five physical temporaries (p0 through p4), below
the twelve-entry physical file.  The two direct varyings have widths four
and three; c30/c31 make the constant high-water count 32.  With active0=2 and
physical=5, the derived resource bucket is `0x08000` and the complete control
words are:

```text
A90 = 90048003   # ALU first, resource bucket 0x08000, direct T0 and T1
A91 = 91000000   # no texture-coordinate consumer
A92 = 92000025   # zero TAU, 37 ALU
```

`chrome9_phong_resource_check.py` replays the allocation and asserts the
schedule, widths, constant count, limits, and controls.  The userspace-native
diagnostic encoder does not accept the large virtual temp numbers and reports
`native=0`; that optional encoder is not the production cmd22 route.  The
semantic program and kernel compiler are the relevant path and both accept
the program.

## First correctness and performance gates

There is no missing compiler opcode or resource capacity for default Phong.
The first correctness gate is finite full-program execution on silicon:
RCP, the three DP3/RSQ/MUL normalization chains, reflect, MAX, the exact
POW100 chain, and partial-vector result reconstruction must agree together.
This should be tested before exceptional zero-length normalization or w=0.

A bounded first oracle can use one constant-varying triangle with
`T0=(0,-1,0,1)` and `T1=(0,1,0)`.  Every reciprocal and normalization is
finite, specular and diffuse are both one, and the clamped RGBA result is
`(0.82,0.82,1,1)`, host BGRA word `0xffd1d1ff`.  Guarded color/depth edges,
submitted-versus-rejected counters, and engine error state remain mandatory.
A second oracle should choose a nontrivial specular base in (0,1) so the
POW100 chain cannot pass merely because x=1; its expected result should be
computed from the same scalar expression with an explicit tolerance rather
than from a scene or shader hash.

After correctness, the first performance boundaries are outside the fragment
compiler: the vertex shader still runs in Gallium Draw software, and generic
cmd22 batches remain capped at 16 triangles.  Phong genuinely requires
per-fragment arithmetic and cannot take Gouraud's closed D1/RGBX shortcut.
The measured 8,043-triangle cat consequently requires 503 submissions at the
current cap.  A native VS/TCL backend and a proved larger kernel builder bound
are the generic next performance steps.

