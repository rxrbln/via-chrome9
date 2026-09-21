# Chrome9 glmark2 fixed-function roadmap

This note ranks the `shading`, `bump`, `terrain`, and `conditionals` scenes in
glmark2 2023.01 by the smallest truthful Chrome9 implementation they require.
It deliberately separates a closed fixed-function lowering from a general
shader compiler.  The source audited here is the T2 copy at
`/srv/t2/src.glmark2.x86-64-desktop.260917.185204.2143133/glmark2-2023.01`.

## Priority and risk

| rank | scene/key | minimum new work | packet change | wedge risk |
|---:|---|---|---|---|
| 1 | `shading:shading=gouraud` | accept direct `vec4 VAR0` as primary RGBA and pack it into D1 | none | low |
| 2 | `shading:shading=blinn-phong-inf` | a native fragment ALU program with interpolated normal/eye vector, normalize, dot, max and pow | new verified PS | high |
| 3 | `bump:bump-render=off/high-poly` | general per-fragment normal lighting, including normalize/dot/max/pow | new verified PS | high |
| 4 | `bump:bump-render=normals` | sampled normal remap plus matrix multiply and the same lighting program | sampler plus new verified PS | high |
| 5 | `conditionals` | `gl_FragCoord`, `fract`, multiply and dynamic if/else in the fragment program | general control-flow compiler | very high |
| 6 | `terrain` | vertex texture fetch, FBO render chains, five fragment samplers, dependent ALU and post-processing | several unproved blocks | prohibitive now |

The default `shading` option is `gouraud`.  Its `light-basic.vert` performs
lighting in the vertex shader and exports `varying vec4 Color`; Gallium draw
already executes this vertex shader in software.  `light-basic.frag` contains
only `gl_FragColor = Color`.  The linker places this user varying at
`VARYING_SLOT_VAR0`, whereas the driver's older full-color key accepted only
legacy `VARYING_SLOT_COL0`.  This is a semantic-slot mismatch, not a new GPU
program.

The bounded lowering is:

```text
VAR0.xyzw, finite float32 -> clamp/round to D3DCOLOR ARGB8
                       R -> bits 23:16
                       G -> bits 15:8
                       B -> bits 7:0
                       A -> bits 31:24
```

Only an identity-ordered four-component load, optionally through identity
`mov`, `fsat`, or a component-preserving `vec4`, belongs to this key.  A
swizzle, arithmetic, partial vector, second varying, uniform, texture or
control-flow instruction must remain rejected.

## Exact Gouraud command inheritance

No UAPI or kernel builder delta is required.  The existing untextured primary
color and D16 builders already emit the exact program and raster state:

```text
A02 = 020041d1       D1 packed D3DCOLOR, normalized
A20 = 20000000       independent triangle list
A23 = 23004f02       Gouraud triangle
A24 = 24000003       three immediate vertices
H6 vertex = x, y, z, w, diffuse-D1, specular-D2
VS: D0 -> transformed position, D1 -> oD0, D2 -> oD1
PS: MOV oC0, v0
```

`020041d1` is live-proven; `020041d0` leaves 0..255 color bytes unnormalized
and must not return.  The D16 LESS/LEQUAL state is orthogonal and changes no
FVF, vertex, VS, PS, TAU or ALU word.  Consequently the safe implementation
is entirely Mesa-side: select the generic draw output, emit it as four floats,
validate all components are finite, then pack one normalized D1 word.  It
uses the already bounded primary-color ioctl path.

### Oracle

Render a full-screen quad from a `VAR0` vec4 passthrough shader with all four
vertices `(0.25, 0.5, 0.75, 0.5)`.  Require:

```text
BGRX target: RGB = 4080bf
BGRA target: pixel = 804080bf, allowing the existing 127/128 half conversion
submitted triangles += 2
rejected triangles unchanged
both target guards unchanged
```

A follow-up live glmark2 run should use
`shading:shading=gouraud:model=cat:nframes=1` with NIR dumping enabled.  The
accepted shader must still be the direct four-component `VAR0` store.  Any
extra operation means it is a different key and must not be admitted by this
lowering.

## Why the other scenes stay closed

### Phong, cel, and bump

`light-phong.frag`, `light-cel.frag`, and `bump-poly.frag` all normalize
interpolated vectors and calculate diffuse/specular lighting per fragment.
They include dot products and exponentiation; cel additionally branches.
The normal-map variants add texture-to-signed-normal remapping, matrix
multiplication, tangent-space varyings, or three dependent height samples.
None is equivalent to the established texture REPLACE/MODULATE or primary
color programs.  Matching only the final grayscale shape would be incorrect
because the result varies per fragment.

Do not encode these as another closed classifier until a Windows shader-body
oracle proves the exact PS instruction encoding, constant map, temporary
lifetime, interpolator declaration, and termination words.  First tests must
use a guard-surrounded target with a 32-by-32 viewport and one triangle; a
timeout quarantines all referenced BOs.

### Conditionals

The default generated fragment shader starts with
`fract(gl_FragCoord.x * gl_FragCoord.y * 0.0001)` and one dynamic branch:

```glsl
if (d >= 0.5)
    d = fract(2.0 * d);
else
    d = fract(3.0 * d);
```

The vertex shader independently applies `fract` and another generated branch.
Software draw can execute the vertex half, but the fragment half needs
`gl_FragCoord`, `fract`, comparison, control flow, and merge semantics.  It is
not representable by the known combiner keys.  Keep it rejected until a
general verified PS path exists.

### Terrain

`SceneTerrain::supported()` explicitly requires a positive
`GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` and framebuffer support.  Its vertex
shader samples displacement/normal textures; its fragment shader combines
multiple diffuse/detail/specular/displacement sources and lighting.  The
scene also constructs height/normal, bloom and tilt-shift FBO chains.  The
driver must continue advertising zero vertex samplers, causing a truthful
skip.  Enabling the cap to reach the scene would be a false claim and risks
unbounded shader and sampler state.

## Next evidence order

1. Hardware-validate the direct RGBA `VAR0` oracle and default Gouraud scene.
2. Capture NIR and state for `blinn-phong-inf` and `bump-render=off` without
   broadening the classifier.
3. Recover one native PS ALU instruction at a time from the Windows UMD and
   validate it with guard pages and the existing timeout/quarantine policy.
4. Leave terrain skipped and conditionals rejected until the instruction
   verifier covers every opcode and control-flow edge they actually use.
