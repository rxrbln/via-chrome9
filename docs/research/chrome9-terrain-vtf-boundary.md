# Chrome9 terrain vertex-texture boundary

Status: source-exact glmark2 2023.01 and Mesa 26.2.2 audit, refreshed through
shared Mesa commit `4575a3c` and Chrome9 commit `78f237a`.  No GPU run is part
of this result.  The target uses draw's TGSI interpreter, not LLVM; the first
bounded software VTF path and its no-LLVM execution regression now exist, but
public vertex texture caps deliberately remain zero.

## Result

Terrain does not require native Chrome9 TCL.  Its vertex shader remains in
Mesa's software draw module and now has a bounded `struct tgsi_sampler`
implementation.  Both `objs-chrome9/build.ninja` and
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
`tDisplacement = 5`.  Those are application texture units, not Gallium shader
slots.  The linker assigns the two active per-stage opaque uniforms dense
indices 0 and 1 through `UniformStorage[].opaque[VERTEX].index`; the state
tracker then uses `prog->SamplerUnits[0..1]` to select application units 3 and
5 and installs their states/views at pipe slots 0 and 1.  Thus the truthful
per-stage cap floor is two vertex samplers/views, not application-unit
high-water six.  Both resources are 256x256 generated BGRA color targets.
`BaseRenderer` defaults them to `GL_LINEAR` minification/magnification and
`GL_CLAMP_TO_EDGE` in both axes; neither renderer changes those defaults.

Implicit vertex `texture2D` did not become explicit LOD zero merely because
NIR-to-TGSI happened to run its projector lowering.  Shared Mesa commit
`4575a3c` fixes the responsible layer: `nir_to_tgsi_lower_txp()` now invokes
`nir_lower_tex()` with `lower_invalid_implicit_lod=true`.  NIR defines that
invalid non-fragment implicit LOD as zero, so both the implicit terrain form
and an explicit `textureLod(...,0)` reach the TGSI sampler with bounded
LOD-zero control.  The first terrain envelope is therefore:

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

The context now owns separate vertex sampler/view arrays, refreshes mapped
views before software execution, and installs `chrome9_vs_tgsi_sampler` with
`draw_texture_sampler(..., MESA_SHADER_VERTEX, ...)`.  Terrain's shared unit 5
and vertex-only unit 3 therefore no longer alias fragment binding ownership.
The bounded callback accepts normalized finite 2D BGRA, linear/clamp-to-edge,
and explicit/implicit LOD zero; other operation/state combinations return
zero and remain unadvertised.

## Exact main-fragment resource floor

The linked main fragment shader has these source-exact resources:

| resource | requirement |
|---|---:|
| active TEX instructions | 5 |
| application texture units | 0, 1, 2, 4, 5 |
| linked/Gallium FS sampler slots | 0, 1, 2, 3, 4 |
| sampler/view slot count | 5 |
| logical smooth varyings | 5 |
| packed physical FS inputs | 4, widths `4,4,4,2` |
| varying components | 14 (`3+3+3+2+3`) |
| color outputs | 1 |
| worst-case active non-sampler uniform vec4 slots | 16 |

The five fetches are detail, diffuse 1, diffuse 2, displacement, and
specular.  `tNormal` is declared in the fragment source but dead there.  Four
fetches consume the one shared computed `uvOverlay`; displacement consumes
direct `vUv`, packed in VAR0.zw.  The earlier five-direct-coordinate
description was incorrect: all five logical GLSL varyings are scalar-packed
into physical VAR0..VAR3.  Exact computed-coordinate allocation and physical
temp pressure remain contingent on successful full fragment compilation.

Those interface resources fit the recovered UAPI maxima of eight textures,
eight varyings, 32 TAU records, and 32 constant vec4s.  They do not fit the
current production Gallium claims: three FS samplers and three FS inputs.
The structured stores now advertise 96 ALU/total and 32 TEX instructions, so
instruction caps are not the first link gate.

The application-unit holes do **not** become holes in the Gallium shader
slots.  `gl_nir_lower_samplers_as_deref()` assigns the five linked opaque
uniforms dense per-stage indices 0 through 4 from
`UniformStorage[location].opaque[stage].index`
(`gl_nir_lower_samplers_as_deref.c:176-178,388`).  At draw time
`st_atom_texture.c:101-152` and `st_atom_sampler.c:222-243` walk those dense
`prog->SamplersUsed` slots, map each through `prog->SamplerUnits[slot]` to
application units 0,1,2,4,5, and install the selected state back into pipe
slots 0 through 4.  The exact linked Chrome9 fragment probe consequently
reports `sampler_mask=0x1f`, not sparse `0x37`.  Terrain therefore needs five
FS sampler/view slots, but it does not require the optional sparse-descriptor
ABI.

The fragment ALU count and physical-temp pressure are intentionally not
claimed closed by this audit.  The literal one-iteration light loop can
unroll, and glmark uploads `pointLightDistance[0] = 0`, allowing the distance
branch to specialize away, but a captured linked Chrome9 NIR object is still
required to prove `ALU + computed-coordinate bridges <= 96` and at most 12
simultaneously live physical temps.  The five-TEX, five-varying, five-slot and
16-constant figures above are exact independently of that later compiler
proof.

## Completed bounded offline gate

Chrome9 commit `78f237a` implements and tests the originally required
no-LLVM gate without changing public caps:

1. it adds per-stage vertex sampler/view ownership and a Chrome9
   `struct tgsi_sampler` installed through `draw_texture_sampler()`;
2. it retires retained draws and refreshes guarded shadows before software
   fetch;
3. it implements the exact 2D LOD-zero BGRA linear/clamp operation;
4. it feeds an asymmetric 2x2 BGRA image through `get_samples()` at texel
   centers and half-way points, with different resources in logical units 3
   and 5;
5. it runs both implicit and explicit-LOD variants of a draw-module vertex
   shader whose deliberately raw NIR unit-3 and unit-5 samples independently
   alter position and a varying, checking both vec4 outputs for four vertices
   against a CPU reference.

The test passes with the target sysroot loader.  Cap promotion still requires
extending the callback to the complete operation/state envelope implied by
the chosen Gallium limits.  It proves sparse callback array indexing, but it
does not prove the linked GL terrain binding path: ordinary linked terrain
uses dense pipe slots 0 and 1 with `SamplerUnits[]` mapping them to GL units 3
and 5.  A state-tracker-level dense-slot regression remains necessary before
cap promotion.  The present bounded success is not permission to advertise
arbitrary vertex sampling.  This gate remains independent of the fragment
compiler and kernel packet ABI.
