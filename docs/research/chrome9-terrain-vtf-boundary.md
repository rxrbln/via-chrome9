# Chrome9 terrain vertex-texture boundary

Status: source-exact glmark2 2023.01 and Mesa 26.2.2 audit.  No GPU run is
part of this result.  The important conclusion is build-specific: the target
Mesa uses draw's TGSI interpreter, not LLVM, so a mapped-image-only VTF path
cannot execute.

## Result

Terrain does not require native Chrome9 TCL.  Its vertex shader can remain in
Mesa's software draw module, but the current T2 build needs a real
`struct tgsi_sampler` implementation.  Both `objs-chrome9/build.ninja` and
`objs-chrome9-full/build.ninja` define:

```text
DRAW_LLVM_AVAILABLE=0
```

This changes the correct implementation boundary:

- `draw_create_vertex_shader()` falls through to `draw_create_vs_exec()`;
- `draw_vs_exec.c:227-229` converts incoming vertex NIR to TGSI;
- `vs_exec_prepare()` binds `draw->vs.tgsi.sampler` into the TGSI machine;
- `draw_set_mapped_texture()` is entirely inside
  `#if DRAW_LLVM_AVAILABLE` (`draw_context.c:1122-1143`) and is therefore a
  no-op in these builds.

Consequently the nearby `m9x/m9_vs_texture.c` mapped-texture strategy is not
an executable model for this Chrome9 build.  The applicable Mesa model is
softpipe: create a `struct tgsi_sampler`, install it once with
`draw_texture_sampler(draw, MESA_SHADER_VERTEX, sampler)`, and keep its
sampler/view state current.  The interface has four callbacks:
`get_samples`, `get_dims`, `get_texel`, and `query_lod`; see
`tgsi_exec.h:167-212` and softpipe's implementation at
`sp_tex_sample.c:3649-3863`.

## Exact terrain vertex contract

The shipped `terrain.vert` performs exactly two ordinary two-dimensional
fetches, both from the input `uv`:

```glsl
vec3 dv = texture2D(tDisplacement, uv).xyz;
...
vec3 normalTex = texture2D(tNormal, uv).xyz * 2.0 - 1.0;
```

`TerrainRenderer::init_program()` assigns `tNormal = 3` and
`tDisplacement = 5`.  Thus this is two active vertex samplers but a sampler
and view high-water of six; arrays of length two are insufficient.  Both
resources are 256x256 generated BGRA color targets.  `BaseRenderer` defaults
them to `GL_LINEAR` minification/magnification and `GL_CLAMP_TO_EDGE` in both
axes; neither renderer changes those defaults.

NIR-to-TGSI must run `nir_lower_tex()` for every non-fragment shader
(`nir_to_tgsi.c:3791-3810`) so implicit vertex `texture2D` becomes an explicit
LOD operation.  For this shader that LOD is zero.  The first terrain oracle
therefore needs only the following dynamic case, although cap promotion must
remain bounded by everything the driver advertises:

```text
target       2D
format       B8G8R8A8_UNORM
coordinates  normalized finite S/T
LOD          explicit 0
filter       linear
wrap         clamp-to-edge
units        3 and 5, independently bound
```

Chrome9 resources are already linear and persistently mapped with per-level
offsets and 32-byte row pitches.  A software sampler can read
`bo.map + level_offset[level]` directly.  It must nevertheless retire
`chrome9_draw_flush_batch()` before the CPU reads any vertex-sampled view:
the generated height and normal maps are outputs of earlier GPU/FBO draws,
and a persistent mapping is not itself a submit point.  The ioctl path is
synchronous after that retirement.  Unguarded imported views must use the
existing guarded `chrome9_sampler_view_resource()` shadow and refresh it at
draw time, just like the fragment consumer.

The context presently cannot represent this state.  It has only one flat
sampler/view array, both binding callbacks return immediately for a
non-fragment stage (`chrome9_context.c:223-239,4319-4358`), and context
creation never calls `draw_texture_sampler()` (`:4925-4970`).  Vertex and
fragment state must be owned separately; terrain binds unit 5 in both stages
and unit 3 only in the vertex stage after fragment dead-code elimination.

## Exact main-fragment resource floor

The linked main fragment shader has these source-exact resources:

| resource | requirement |
|---|---:|
| active TEX instructions | 5 |
| active FS sampler ids | 0, 1, 2, 4, 5 |
| sampler/view high-water | 6 |
| direct smooth varyings | 5 |
| varying components | 14 (`3+3+3+2+3`) |
| color outputs | 1 |
| worst-case active non-sampler uniform vec4 slots | 16 |

The five fetches are detail, diffuse 1, diffuse 2, displacement, and
specular.  `tNormal` is declared in the fragment source but dead there.  Four
fetches consume the computed `uvOverlay`; displacement consumes direct
`vUv`.  With five direct varying coordinates, the kernel allocator has slots
5, 6, and 7 for computed coordinates and cycles them
(`chrome9_fragment.c:378-417`), so the coordinate namespace reaches its exact
eight-slot ceiling without exceeding it.

Those interface resources fit the recovered UAPI maxima of eight textures,
eight varyings, 32 TAU records, and 32 constant vec4s.  They do not fit the
current production Gallium claims: three FS samplers, three FS inputs, and 32
total/ALU instructions (`chrome9_screen.c:326-358`).  Sampler mask `0x37` is
also sparse, and the draw path explicitly rejects it as
`sparse-sampler-abi` (`chrome9_draw.c:951-959`).  Raising only the cap cannot
make terrain submit.

The fragment ALU count and physical-temp pressure are intentionally not
claimed closed by this audit.  The literal one-iteration light loop can
unroll, and glmark uploads `pointLightDistance[0] = 0`, allowing the distance
branch to specialize away, but a captured linked Chrome9 NIR object is still
required to prove `ALU + computed-coordinate bridges <= 96` and at most 12
simultaneously live physical temps.  The five-TEX, five-varying, six-slot and
16-constant figures above are exact independently of that later compiler
proof.

## Smallest independent implementation gate

Implement and test the non-LLVM vertex sampler before changing any public
cap:

1. add per-stage vertex sampler/view ownership and a Chrome9
   `struct tgsi_sampler` installed through `draw_texture_sampler()`;
2. retire retained draws and refresh guarded shadows before software fetch;
3. implement the exact 2D explicit-LOD-zero BGRA linear/clamp operation;
4. feed an asymmetric 2x2 BGRA image through `get_samples()` at texel centers
   and half-way points, with different resources in logical units 3 and 5;
5. run a draw-module vertex shader whose two samples independently alter
   position and one varying, and compare the post-transform vertices to a CPU
   reference;
6. extend the sampler to the complete vertex texture operation/state envelope
   which the chosen Gallium caps expose, then and only then publish at least
   six vertex sampler/view slots and two texture instructions.

This gate is independent of the fragment compiler and kernel packet ABI.  It
proves that enabling `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` will execute real
sample semantics in the actual no-LLVM build rather than merely making the
terrain support check pass.
