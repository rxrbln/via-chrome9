# Mesa 26.2 capability audit for the bounded Chrome9 driver

## Result

There is no `pipe_caps` combination in Mesa 26.2 which truthfully advertises
the present three-fragment-program Chrome9 implementation as a complete
desktop GL driver.  The compatibility state tracker has a hard OpenGL 1.3
floor, requires at least one fragment texture unit to create/bind a context,
and unconditionally exposes the GLSL/ARB program API framework.  The current
screen is useful as a bring-up pipe driver, but a successful EGL/GLX context
does not establish GL conformance.

This conclusion is independent of KMS.  It follows from the Mesa frontend and
state-tracker code before a drawable is scanned out.

## Measured surfaceless result and crash root cause

The first isolated custom-Mesa surfaceless `eglinfo` run created a Chrome9
context, reported GL 1.5 / GLSL 1.20 and a broad unsupported extension list,
then exited with status 139.  The crash was not caused by extension-string
construction.  `draw_init_shader_caps()` had populated the vertex stage with
generic sampler, image, shader-buffer, and atomic-interface limits.  Chrome9
does not install the corresponding pipe-context interfaces, so
`cso_unbind_context()` trusted the advertised nonzero limits and reached a
NULL callback during teardown.

The exact bounded correction is to retain the draw module's CPU vertex ALU
and constant-buffer capability but zero its unwired resource interfaces:

```text
VS max_tex_instructions = 0
VS max_tex_indirections = 0
VS max_texture_samplers = 0
VS max_sampler_views = 0
VS max_shader_buffers = 0
VS max_shader_images = 0
VS max_hw_atomic_counters = 0
VS max_hw_atomic_counter_buffers = 0
```

Together with `texture_shadow_map=false`, the generic-cap restrictions below,
and the existing one-unit fragment limits, the next surfaceless `eglinfo`
run exited with status zero and reported GL 1.3.  The direct Gallium suite,
including full source/destination guard validation and the varying-reciprocal-W
REPLACE/MODULATE projective interpolation cases, passed on the same build.
This validates the crash fix and bounded pipe path; it does not make the
remaining GL 1.3 or GLSL/extension claims conformant.

## Exact version path

`chrome9_screen_create()` calls `u_init_pipe_screen_caps()`.  Mesa 26.2
`u_screen.c` sets, among other defaults:

```text
glsl_feature_level = glsl_feature_level_compatibility = 120
texture_shadow_map = true
texture_transfer_modes = PIPE_TEXTURE_TRANSFER_BLIT
texrect = true
buffer_map_persistent_coherent = true
native_fp32_depth = true
alpha_test = true
two_sided_color = true
clip_planes = 1
packed_stream_output = true
dest_surface_srgb_control = true
```

The DRI path is:

```text
dri_init_screen
  -> st_api_query_versions
     -> get_version
        -> _mesa_init_constants
        -> _mesa_init_extensions
        -> st_init_limits / st_init_extensions
        -> _mesa_get_version / compute_version
```

`st_init_extensions()` maps `texture_shadow_map` directly to `ARB_shadow`.
`version.c::compute_version()` defines GL 1.4 as `ARB_shadow` and GL 1.5 as
exactly the same condition.  Consequently the current screen advertises
desktop compatibility **GL 1.5**.  `npot_textures` remains zero, so the GL 2.0
and ES 2.0 gates remain closed.  `_mesa_get_version(API_OPENGLES)` returns
ES 1.1 unconditionally.

The GLSL result cannot be suppressed by lowering ordinary shader limits:

- `_mesa_init_constants()` establishes GLSL 1.20 as the compatibility
  minimum;
- `u_init_pipe_screen_caps()` also sets both GLSL feature levels to 120;
- `_mesa_init_extensions()` unconditionally enables `ARB_vertex_shader`,
  `ARB_fragment_shader`, `ARB_vertex_program`, `ARB_fragment_program`, and
  `ATI_fragment_shader`;
- `ARB_shader_objects` and `ARB_shading_language_100` use `dummy_true` in
  `extensions_table.h`, so the normal extension override mechanism cannot
  disable them;
- `getstring.c::glGetString(GL_SHADING_LANGUAGE_VERSION)` returns the value
  selected by `Const.GLSLVersion` for every desktop context, independently of
  whether the two stage extensions were later hidden.

Setting `glsl_feature_level=0` is therefore not a supported solution: it does
not remove the unconditional API/extension assumptions and makes
`shading_language_version()` report an invalid internal version.

## Mandatory one-unit and GL floor conflict

`st_extensions.c` derives `MaxTextureUnits` from fragment
`max_texture_samplers`.  `context.c::check_context_limits()` asserts that
fragment image units, coordinate units, combined units, and fixed-function
texture units are all greater than zero.  This is why Chrome9 must report at
least one logical fragment sampler/view even before sampling is complete.

That preserves context creation, but it is not a conformant GL 1.3 exposure:
GL 1.3 requires at least two fixed-function texture units, while the current
hardware path accepts only sampler zero and only REPLACE/MODULATE.  Mesa's
`compute_version()` has no GL 1.0--1.2 fallback; if `ARB_shadow` is cleared,
its final fallback is still 1.3.

Thus the smallest context-preserving shader envelope is one sampler/view,
one texture instruction/indirection, and the actual ALU/input/output/temp
limits of the classifier.  It is a bring-up envelope, not a truthful GL
version claim.  If the kernel lacks the textured-triangle capability, the
honest DRI policy is to fail screen/context creation rather than advertise a
dummy unit; standalone pipe smoke tests may still use an explicitly separate
probe path.

## Defaults which must be overridden immediately

At minimum, after `u_init_pipe_screen_caps()` the bounded driver should clear
defaults for operations it neither implements nor safely lowers:

| field | bounded value | reason |
|---|---:|---|
| `texture_shadow_map` | `false` | removes the accidental GL 1.5/ARB shadow promotion |
| `texture_transfer_modes` | `PIPE_TEXTURE_TRANSFER_DEFAULT` (`0`) | Chrome9 installs neither `pipe_context::blit` nor `resource_copy_region`; advertising BLIT permits a NULL callback |
| `texrect` | `false` as an API sampling cap | there is no rectangle sampler/view path |
| `gl_clamp` | `false` until GL_CLAMP lowering is accepted | the sampler callback accepts CLAMP_TO_EDGE but not native GL_CLAMP |
| `native_fp32_depth` | `false` | only bounded Z16 is implemented |
| `buffer_map_persistent_coherent` | `false` until storage/map flag tests exist | otherwise advertises a stronger buffer-storage contract |
| `dest_surface_srgb_control` | `false` | no sRGB target/sampler format is accepted |
| `packed_stream_output` | `false` | no stream-output stage/callback exists |
| `flatshade` | `false` until provoking D1/D2 is proven | the generic initializer defaults this true, but the current packet is always Gouraud and ordinary filled triangles bypass draw's flatshade-copy stage |
| `two_sided_color` | `false` unless verified through draw-module lowering | current bounded submission has one primary-color linkage |
| `clip_planes` | `0` unless verified through draw-module clipping | default one is not native evidence |
| `dithering` | `false` until exact output tests pass | current draw validation does not represent controllable dither state |
| `mixed_colorbuffer_formats` | `false` | only one B8G8R8X8 target is accepted |

`alpha_test=false` tells the state tracker to lower alpha test into the
fragment shader; it does **not** make alpha test work.  The present classifier
rejects discard/compare NIR, while setting the cap true claims a fixed alpha
test that draw validation also rejects.  Alpha test is therefore a real
minimum-GL semantic blocker, not a cap-toggle solution.  The same principle
applies to unsupported blend, stencil, fog, and texture-environment modes.

`flatshade=false` has the same fail-closed character.  It asks the state
tracker to make flat interpolation explicit in NIR; the bounded classifier
may reject that form until its provoking-color path exists.  Leaving the
initializer's true default is worse: the state tracker keeps only rasterizer
flatshade state, while Chrome9 emits `A23=23004f02` and can silently Gouraud
interpolate three different vertex colors.  Exact A23 words and the software
D1/D2 lowering boundary are in
[flatshade-edge-ownership-contract.md](flatshade-edge-ownership-contract.md).

The exact alternative route is in `st_atom_depth.c`: with
`screen->caps.alpha_test=true`, Mesa copies `AlphaEnabled`, the compare
function, and `AlphaRefUnclamped` into the pipe DSA state.  Pipe compare enums
are `NEVER..ALWAYS = 0..7`, matching the recovered A33 selector, but the float
reference must be converted with `float_to_ubyte()` before the kernel packs
its 10-bit A33 reference.  With the cap false, `st_atom_shader.c` instead keys
an FS variant and `st_nir_lower_alpha_test.c` inserts `nir_discard_if`, which
the bounded classifier rejects.  Thus an A33 UAPI and the cap must land as one
semantic change.  Do not advertise the cap after only color/textured tests:
the current D16 ABI cannot carry alpha state, so alpha+depth would still be
silently rejected.

The fragment shader limits should describe the accepted programs rather than
the hardware's eventual native ceiling: one sampler/view, one texture
instruction and indirection, two ALU operations for MODULATE, two inputs, one
output, and only the temporaries actually needed.  The native 96/32 limits
belong only to a future validated general compiler.

There is a subtle distinction between the Gallium dither cap and the current
packet default.  `st_update_blend()` copies `ctx->Color.DitherFlag` into
`pipe_blend_state::dither` only when `screen->caps.dithering` is true.  With
the bounded Chrome9 cap false, both the GL default (dither enabled) and a
later `glDisable(GL_DITHER)` reach the driver as `dither=0`.  Every current
color packet nevertheless writes type-01 A00 bit 0 as one: the legacy body
uses `00000011`, while the blend-aware lowering starts at `00000001` and
adds bit 4 when blending is enabled.  The companion A53 value `53010c0f` has
bits `[16:15]=10`, which the manual defines as rounding with multiplication by
`2^n-1`; it is not one of the two spatial dither-table modes.  The enabled
post-render path is therefore already a plausible explanation for the live
round-to-nearest blend result.  The GL default incidentally agrees with the
packet, but disabling dither is not represented, and treating Gallium
`dither=0` and `dither=1` identically is not proven harmless even on XRGB8888.
Keep the cap false and do not claim selectable dither until the UAPI can
control A00 bit 0 and both settings are measured with fractional output.

## DRI drawable-target inconsistency

`dri_screen.c::dri_init_screen()` chooses the internal drawable target as
`PIPE_TEXTURE_2D` only when `npot_textures` is true; otherwise it chooses
`PIPE_TEXTURE_RECT`.  Chrome9 correctly leaves the NPOT GL cap false, but its
resource layout and draw validation currently accept only
`PIPE_TEXTURE_2D`.  At the same time `u_init_pipe_screen_caps()` claims
`texrect=true`.

This is a concrete crash/failure candidate for EGL/GLX paths even without
KMS.  Do not set `npot_textures=true` merely to steer DRI to 2D: that exposes
the complete ARB NPOT contract, including mip/filter/wrap semantics not yet
implemented.  The bounded fix is to support `PIPE_TEXTURE_RECT` only for the
internal render-target/display binding (same guarded linear layout), accept
it as a destination, and continue rejecting rectangle sampler views.  A Mesa
frontend change providing a separate internal-drawable target cap would be
cleaner but is broader.

## BGRA render-target capability boundary

The format-99 extension deliberately exposes
`PIPE_FORMAT_B8G8R8A8_UNORM` only as a guarded, linear, internal render
target.  `PIPE_BIND_SHARED`, display, and scanout combinations are rejected,
and commit `8cda1e0` also rejects PRIME export so the actual handle callbacks
match the advertised binding set.  Every BGRA draw is forced through a
blend-aware successor even for disabled blending and a full color mask,
because the legacy ioctls have no destination-format field and always emit
A52 format 98.

Two validation boundaries remain important:

* `DRM_CHROME9_CAP_BLEND_BGRA8888` is useful only with the untextured blend
  successor and, when the texture path is exposed, the textured blend
  successor.  The live kernel advertises that bundle, but the screen should
  either validate the dependency once or make `is_format_supported()` test
  the required successor bits before advertising a BGRA render target.
* A `pipe_surface` is a view and carries its own format, level, and layer
  range.  The bounded draw and CPU-clear paths currently choose A52 and color
  packing from the underlying `pipe_resource::format`.  They must accept only
  a color view whose format equals the resource format and whose level and
  layers are all zero; otherwise a BGRX view of a BGRA resource, or the
  converse, silently gets the wrong destination-alpha semantics.  The direct
  smoke constructs a matching view and therefore does not exercise this
  negative case.

The current guarded smoke does cover opaque alpha, canonical independent-
alpha source-over, and an A-only write mask.  The remaining coverage gaps are
a textured successor writing a BGRA destination and the intentionally
unsupported BGRA-plus-D16 combination; the latter cannot work until a
depth-aware successor also carries destination format.

## What extension overrides can and cannot do

For a laboratory run, `mesa_extension_override` /
`MESA_EXTENSION_OVERRIDE` can hide stage-specific extensions such as
`GL_ARB_vertex_shader`, `GL_ARB_fragment_shader`, `GL_ARB_vertex_program`,
`GL_ARB_fragment_program`, and `GL_ATI_fragment_shader`.  It is initialized
through `_mesa_initialize()` only once per process and is global, not a safe
per-screen driver policy.  It also cannot disable `dummy_true`
`ARB_shader_objects` or `ARB_shading_language_100`, and it cannot change the
hard GL 1.3 floor.

A product-quality truthful exposure therefore needs one of:

1. implement the GL 1.3 minimum plus the Gallium shader contract (at least two
   texture units and all core fixed-function lowering that reaches NIR);
2. add an explicit per-screen state-tracker/API policy which can cap desktop
   below 1.3, disable ES1, suppress shader-stage extensions, and avoid the
   unconditional GLSL string; or
3. do not register a DRI GL screen yet, while retaining the standalone pipe
   and ioctl tests for bring-up.

The least risky near-term course is (3) for truthful claims, while using the
one-unit context only as an explicitly labelled development configuration.

The successful sanitized `eglinfo` run demonstrates that caps can make this
development context stable.  They still cannot remove Mesa's unconditional
GLSL 1.20 string/framework or hard GL 1.3 floor, and the extension list still
contains software/default capabilities beyond the three accepted fragment
programs.  Extension-list policy therefore remains a separate unresolved API
problem rather than part of the fixed NULL-callback bug.
