# From the Mesa 7.11.2 Unichrome driver to a Chrome9 Gallium draw path

## Purpose and limits

This note maps the minimum state needed by `glxgears` from the classic
Mesa 7.11.2 Unichrome driver onto the Chrome9 hardware facts recovered in
this tree.  It is an implementation plan, not a claim that Unichrome and
Chrome9 share command encodings.

Evidence is classified as follows:

* **live**: exercised successfully on the Chrome9 silicon;
* **static**: recovered from the Windows driver or programming manual but not
  yet exercised on silicon;
* **legacy**: behavior of the old Unichrome driver, useful as an API/state
  checklist only.

The decisive architectural result is that no old command stream should be
ported.  The reusable idea is its software transform-and-light pipeline:
execute the GL vertex work on the CPU, then submit already transformed
position and color.  In current Gallium that role belongs to the `draw`
module.  Chrome9 can consume the result through the live-proven H6 shader
envelope: a six-dword pretransformed vertex, the known vertex shader that
copies position and diffuse color, and the pixel shader `MOV oC0, v0`.
The fourth position component is meaningful reciprocal clip W, not padding.
The current untextured ioctl happens not to need it, but a textured extension
must preserve it for the hardware's perspective-correction path.

## What `glxgears` actually requires

The minimum compatibility-profile workload uses projection/model-view
matrices, lighting and normals, normalization, per-vertex colors, smooth and
flat shading, culling, a viewport, color/depth clears, and depth test/write.
Its quads and quad strips must be decomposed to triangles.  It also needs a
window-system color buffer, resize handling, flush, swap/present, and finish
semantics.  Texturing, blending, stencil, multisampling, and a general GPU
shader compiler are not on the first correctness path.

Consequently, the first supported contract can deliberately be narrow:

* one linear X8R8G8B8 color render target;
* one linear D16 depth buffer;
* one viewport/scissor rectangle;
* untextured primary-color fragment output;
* triangle-list submission after software transform, lighting, clipping,
  culling, primitive decomposition, and explicit provoking-color lowering;
* depth compare/write and color write masks;
* synchronous submission initially, with a Gallium fence facade;
* CPU clears as a temporary bring-up fallback, followed by accelerated color
  and depth clears before claiming a fully accelerated path.

## Current kernel rung

### 2026-09-20 end-to-end gears gate audit

The general D16 rung is now live, not merely a fixed-depth oracle.  The
combined DMA-ownership correction and type-01 A20 `20007fff` pixel-Z clip
state passed the degenerate preservation control, arbitrary per-vertex Z,
the full 64 by 64 and 257 by 129 depth suites, and every earlier fixed/color/
Gallium regression.  The persistent module and probe hashes are recorded in
[depth-blend-oracles.md](depth-blend-oracles.md).

Static inspection of the actual target `/usr/X11/bin/glxgears` confirms the
minimum state precisely.  Initialization enables `GL_CULL_FACE` (`0x0b44`),
`GL_LIGHTING` (`0x0b50`), `GL_LIGHT0` (`0x4000`), and `GL_DEPTH_TEST`
(`0x0b71`).  Each frame clears `0x4100`, color plus depth.  Gear construction
switches between `GL_FLAT` (`0x1d00`) and `GL_SMOOTH` (`0x1d01`).  It does not
require a texture, blending, stencil, multisampling, or a general native
shader compiler.  The current Gallium `draw` path supplies CPU vertex
lighting/transformation, clipping, culling, flat-color propagation, viewport
mapping, and quad/strip decomposition; the live D16 ioctl supplies exactly
the default LESS/write depth behavior.

One pre-GPU loader defect was found by auditing the built module rather than
the Meson option alone.  `src/gallium/targets/dril/meson.build` created the
`chrome9_dri.so -> libdril_dri.so` link, but `dril_target.c` omitted
`DEFINE_LOADER_DRM_ENTRYPOINT(chrome9)`, so the proxy exported no
`__driDriverGetExtensions_chrome9`.  Parent Mesa commit `cea2543` adds the
entry point; a full `objs-chrome9-full` rebuild passed and `readelf` verifies
the symbol.  This failure could otherwise look like a rendering or device
probe failure while no Chrome9 screen code had run.

Two boundaries remain distinct from the now-live off-screen draw path:

* the new KMS scaffold deliberately reports the connector disconnected,
  returns no modes, and rejects every enabled CRTC with `-EOPNOTSUPP`; it
  cannot yet display glxgears as a standalone new graphics stack;
* the fragment shader caps still report zero texture samplers/views.  That
  is sufficient for the narrow untextured release-build gears experiment,
  but violates Mesa's normal context invariant that at least one fixed-
  function texture unit exists and aborts a debug build in
  `check_context_limits`.  Do not raise the cap until the first sampler path
  below is real.

The kernel tree has moved one step beyond the original fixed-quad diagnostic.
`include/uapi/drm/chrome9_drm.h:15` now defines
`DRM_IOCTL_CHROME9_DRAW_TRIANGLE`, and `chrome9_engine.c:1151`
`chrome9_draw_triangle_ioctl` validates one Gouraud triangle with:

* a native, non-imported X8R8G8B8 shmem GEM object laid out as guard page,
  page-aligned pixels, guard page;
* width and height in `1..2048` and pitch exactly
  `ALIGN(width * 4, 32)`;
* three screen-space x/y coordinates in signed 16.16, each ranging from
  `-0.5` through `dimension - 0.5`, plus packed colors;
* synchronous completion and in-kernel verification of both guard pages.

`chrome9_engine.c:413` `chrome9_build_triangle` substitutes target pitch,
full-target A80-A83 windows, three x/y/color records, Gouraud A23, and A24=3
into the known packet. It still fixes z=0, w=1, D2=white, shader state,
blend/depth state, and the scissor to the whole target. It accepts exactly one
triangle and exports no fence. This is an excellent Gallium bring-up ioctl,
but not yet the minimum gears ABI. The companion `tools/chrome9_triangle.c`
now passes live at both 64x64 and 257x129 with capabilities `0x1d`, including
pixel readback, both guards, and row padding. The variable-size x/y/color
triangle rung is therefore **live**; its fixed z/w/depth/scissor/fence
limitations remain.

## End-to-end implementation map

| Need | Mesa 7.11.2 precedent | Gallium/Chrome9 implementation | Known hardware state and remaining gap |
| --- | --- | --- | --- |
| Context creation | `via_context.c:386` `via_pipeline`; `via_context.c:457` `viaCreateContext` | `chrome9_context_create` now initializes uploaders, Gallium draw, Chrome9 `vbuf_render`, state/resource/draw/clear/flush callbacks. | **Live:** the pipe fixture creates/destroys the context and executes a full draw. The old swrast/VBO/TNL module construction is legacy; only the software-T&L design transfers. |
| Drawable/color/depth resources | `via_context.c:145` `viaInitRenderbuffer`; `via_context.c:203` `calculate_buffer_parameters`; `via_context.c:353` `viaReAllocateBuffers`; `via_screen.c:201` `viaCreateBuffer`; `via_screen.c:318` `viaFillInModes` | Extend the implemented local textures/surfaces/transfers with `resource_from_handle` and `resource_get_handle`, then add D16. Track BO, pixel offset, stride, size, map state, and kernel GPU address lifetime. Let the modern DRI loader manage drawable handles and presentation. | `chrome9_resource.c` exposes guarded linear BGRX/BGRA render targets, Z16 depth, bounds-checked transfers, and BGRX PRIME import/export with guarded shadow copy-back. BGRA is deliberately internal-only: shared/display/scanout bindings plus PRIME import/export are rejected. Type-01 A50 is color base/256, A51 layout/domain, and A52 carries format 98/99 plus pitch. Real loader presentation remains separate. |
| Clear | `via_ioctl.c:109` `viaBlit`; `via_ioctl.c:173` `viaFillBuffer`; `via_ioctl.c:204` `viaClear` | Implement `pipe_context::clear`.  For initial correctness, wait for BO ownership and CPU-fill mapped color/depth with masks respected. Two full-target triangles with identical vertex colors can accelerate an unmasked color clear. Then validate a dedicated depth-clear method. | Both fixed H6 solid color and variable x/y/color triangles are live. `chrome9_drv.c:1276` also proves a 2D solid fill to local SL memory, including `/8` base and pitch encodings, but not a generalized SF/shmem or depth fill. GPU depth clear is unresolved and blocks an all-accelerated claim. |
| Vertex transforms, lighting, and buffers | `via_context.c:386` custom classic TNL pipeline; `via_tris.c:742` `viaChooseVertexState`; `via_tris.c:856` `viaRenderStart`; `via_render.c:89` `via_run_fastrender` | Let Gallium's `draw` module execute the state tracker's VS, transform/lighting, clipping, and viewport work.  Map vertex/index/constant buffers around `draw_vbo`, following the callback shape in current `i915_context.c:51` `i915_draw_vbo`.  Feed its post-TNL output to a Chrome9 `vbuf_render` implementation, patterned structurally after `i915_prim_vbuf.c`. | The live H6 record is six dwords: D0 float4 x/y/z/w, D1 packed diffuse, and D2 packed secondary color. The live VS copies D0 to its transformed-position path, D1 to oD0, and D2 to oD1; the known PS consumes oD0. Mesa `draw` stores reciprocal clip W in D0.w after viewport mapping. The current structured ioctl discards W, which is harmless only for untextured or equal-W tests; texture A01 enables perspective correction and requires it. Arbitrary user vertex buffers are therefore not required for the first kernel ABI; validated immediate post-TNL vertices are sufficient. |
| VS/FS state | No separable shader API in the classic driver | Create/bind/delete VS and FS using the `draw` module, as current `i915_state.c:569` and `:666` do.  Recognize only the untextured primary-color fragment-shader form emitted for fixed-function gears; bind the known Chrome9 PS. Reject unsupported NIR explicitly. | Shader activation order and exact known VS/PS words are documented in `shader-upload-oracles.md`. Generic shader input/output routing, constants, and instruction lowering remain unknown and are outside the gears gate. |
| Primitive emission | `via_tris.c:87` `via_draw_triangle`; `via_tris.c:101` `via_draw_quad`; `via_tris.c:900` `viaRasterPrimitive`; `via_tris.c:1044` `viaFinishPrimitive`; `via_tris.c:1151` `viaRunPipeline` | Normalize everything to triangle lists in the draw stage. The first implementation can invoke `DRAW_TRIANGLE` once per output triangle; then extend it to a bounded list to avoid one synchronous ioctl per primitive. Emit H6 type-04 declarations/state, A20 triangle-list mode, A23 Gouraud mode, A24 vertex count, and `count * 6` immediate dwords. | Type-04 A20 list=`0x20000000`; strip=`0x201c7800`; fan=`0x201c5c00`. A23/A24 semantics and the immediate stream are live for fixed draw; `chrome9_build_triangle` implements one variable Gouraud triangle. H6 ends from A24/payload length; the Unichrome explicit fire/PLEND sequence in `viaFinishPrimitive` is incompatible and must not appear. |
| Viewport and scissor | `via_context.c:726` `viaXMesaWindowMoved`; `via_state.c:600` `viaScissor`; `via_state.c:719` `viaCalcViewport` | Have `draw` bake viewport transform into x/y/z/w, retaining the known pretransformed H6 envelope. Convert Gallium's scissor to Chrome9's exclusive-right/exclusive-bottom type-01 A82/A83 rectangle. Keep DRI3 drawable coordinates resource-relative; do not add DRI1 window origins or iterate cliprects. | Type-01 A80/A81 color window and A82/A83 scissor use top/left in `[23:12]`, exclusive bottom/right in `[11:0]`, with a documented 2048 maximum. The live variable ioctl programs full-target windows and accepts half-pixel-bounded screen coordinates, but cannot set a smaller scissor. Type-04 A40-A48 encode split IEEE x/y/z scale-offset pairs and must match VS c256/c257; type-04 A50-A52 are coordinate clamps, not type-01 render-target registers. |
| Depth | `via_state.c:1274` `viaChooseDepthState` | Translate Gallium depth state to type-01 A13 compare, A01 depth-test/depth-write bits, and D16 A10-A12 surface state. Allocate and retain the depth BO in each job. Begin with LESS/LEQUAL/ALWAYS plus write enable; reject unsupported combinations. | The fixed guarded D16 LESS/write oracle is **live**: 528 red/zero-depth samples, equal-depth green rejected, four guards intact. Variable per-vertex Z and a general D16 draw ABI are still required for gears. |
| Cull and flat shade | `via_state.c:1312` `viaChoosePolygonState`; `via_state.c:1424` `viaChooseTriangle`; `via_tris.c:648` `viaChooseRenderState` | Let `draw` cull, but explicitly carry the API provoking index through topology decomposition and duplicate only packed flat D1/D2 inputs onto all three emitted vertices. Leave hardware culling disabled and A23 Gouraud. Later enable A01 bit 7 and select orientation with A23 bit 8 after a winding test. | The programming manual identifies A01.7 as back-face-cull enable and A23.8 as the cull orientation (0 clockwise, 1 counter-clockwise). A23 solid/FlatA/B/C/Gouraud encodings are exact. Ordinary filled triangles do not automatically traverse draw's flatshade-copy stage; the bounded fix and topology table are documented separately. |
| Blend and color mask | `via_state.c:526` `viaBlendEquationSeparate`; `via_state.c:548` `viaBlendFunc`; `via_state.c:681` `viaClearColor`; `via_state.c:699` color mask; `via_state.c:953` `viaChooseColorState` | Lower only validated RT0 ADD factors and the API-order channel mask; reject every wider state. | **Live:** normalized COLOR0 plus structured color/textured successors pass `SRC_ALPHA/INV_SRC_ALPHA`, disabled R-mask and successor-to-legacy closure. Raw untextured and BGRA-textured source-over yielded `00807f00`; Gallium yielded the valid alternate rounding `007f8000`. Format-99 COPY, A-only mask, and independent-alpha source-over proved stored destination alpha (`80ff0000`, `80332211`, `80807f00`). Wider factors/functions remain unproved. |
| Flush, finish, and fences | `via_ioctl.c:376` `viaEmitBreadcrumbLocked`; `via_ioctl.c:416` `viaCheckBreadcrumb`; `via_ioctl.c:443` `viaWaitIdle`; `via_ioctl.c:674` `fire_buffer`; `via_ioctl.c:802` `viaFlushDmaLocked`; `via_ioctl.c:954` `viaFlush`; `via_ioctl.c:960` `viaFinish` | Accumulate one validated draw job/batch, submit on flush/swap/resource hazard, and return a `pipe_fence_handle`. Initially the ioctl may synchronously finish and return an already-signaled fence. Add `fence_reference`/`fence_finish`; resource mapping waits on the BO reservation. Later expose asynchronous syncobj/fence completion without changing Gallium behavior. | `chrome9_engine.c:895` `chrome9_submit_draw` installs an internal `dma_fence`, BO reservation, and cache transition, then `chrome9_engine.c:737` waits synchronously. Neither draw ioctl exports a fence. The old breadcrumb, status-register spin, DRI lock, and raw command-buffer ioctls are unusable. |
| Swap/present/resize | `via_context.c:927` `viaSwapBuffers`; `via_ioctl.c:346` `viaDoSwapBuffers` | Flush rendering and hand the external color resource back to the loader/present path. Re-import or recreate resources when drawable dimensions/handles change. | PRIME import/export plus guarded shadow allocation and CPU copy-back are **live** through an imported-target draw/flush/readback test. Real DRI3/GLX presentation remains; the exact GPU SF 2D copy successor is documented in `sf-2d-copy-present.md`. |

## Reuse boundaries

### Concepts worth carrying forward

The old driver provides a useful completeness audit:

* translate API state lazily, emit it before a primitive, and invalidate cached
  state after ownership loss (`via_state.c:79` `viaEmitState`,
  `via_state.c:1448` `viaValidateState`, and `via_state.c:1495`
  `viaInvalidateState`);
* run software transform, lighting, clipping, and primitive fallback before
  packing a small hardware vertex (`via_context.c:386`,
  `via_tris.c:742`, and `via_render.c:89`);
* end a primitive before changing state and flush before clear/swap/resource
  hazards;
* align strides, keep color/depth allocation metadata together, honor masks,
  and split API primitives into hardware-supported primitives;
* use a completion object for `glFinish` and resource reuse rather than
  assuming submit completion.

These are behavioral patterns only. Current Mesa helpers should implement
them; copying 2011 module code would recreate obsolete interfaces.

### Assumptions that must be rejected

Do not reuse any of the following:

* DRI1 SAREA, hardware locks, drawable cliprect replication, window-origin
  addition, page flipping, or userspace MMIO/AGP mappings
  (`via_screen.c:89`, `via_context.c:726`, `via_context.c:894`, and
  `via_ioctl.c:740`);
* old `DRM_VIA_CMDBUFFER`/`DRM_VIA_PCICMD` raw submission or breadcrumb fills;
* Unichrome HPara register values, primitive fire words, vertex declarations,
  texture layouts, blend/depth bit positions, or engine-idle polling;
* the legacy choice to represent a 24-bit depth buffer as a 32-bit Mesa
  format; start with Chrome9 D16, whose state has a concrete oracle;
* the old raster fallback matrix as a driver architecture. Gallium `draw`
  already provides the appropriate decomposition, clipping, and software VS
  execution.

Most importantly, neither existing Chrome9 ioctl is yet the gears submission
API. `FIXED_QUAD` is deliberately constrained to one exact three-page GEM
object and canned variants. `DRAW_TRIANGLE` adds a variable-size guarded
color target and arbitrary screen x/y/color for exactly three vertices, but
still has no z/w, depth surface/state, partial scissor, color mask, batch, or
asynchronous fence. It is the right safe seed to extend, not a reason to
expose raw packets.

## Mesa split and callback ownership

The resource/DRM portion of the following split is now implemented; the
context, state, draw, and batch portions remain the concrete next target:

* `chrome9_context.c`: `chrome9_context_create`, destruction, draw-module and
  uploader setup, `chrome9_draw_vbo`, clear/flush entry points;
* `chrome9_state.c`: Gallium CSO creation/bind/delete for blend, depth/stencil,
  rasterizer, VS, FS, vertex elements; framebuffer, viewport, scissor,
  constant, vertex, and index setters; dirty-bit validation;
* `chrome9_draw.c`: Chrome9 `vbuf_render` callbacks, primitive normalization,
  post-TNL six-dword packing, flat-color fixup, and H6 draw packet creation;
* `chrome9_batch.c`: structured job construction, BO reference list, ioctl,
  cache ownership, fence object, flush and wait;
* `chrome9_resource.c`: 2D resource layout, surfaces, transfers, handle
  import/export, map synchronization, and render/depth capability checks;
* existing `chrome9_screen.c`: advertise only capabilities that those paths
  really implement and provide fence callbacks;
* existing `chrome9_drm.c`: remain the thin UAPI wrapper, extended for the
  general validated submit interface rather than accepting raw packets.

The state tracker will generate a fixed-function vertex shader for gears.
`draw_create_vertex_shader`/`draw_bind_vertex_shader` execute that shader on
the CPU.  A deliberately narrow FS recognizer accepts primary color and maps
it to the known Chrome9 pixel program.  This avoids pretending the opaque
Chrome9 shader resource fields form a general compiler interface.

### Exact draw-module wiring for the first path

Current `i915` is a useful Gallium API example, independently of its hardware
packets.  Mirror these interactions:

* `i915_context.c:182` `i915_create_context`: create the upload manager and
  `draw_context`, set its constant stride, and install a `vbuf_render` raster
  stage;
* `i915_state.c:569` and `:666`: create the draw-module FS and VS objects as
  well as the Chrome9 wrapper objects; its bind functions at `:622` and
  `:684` keep draw's shader state synchronized;
* `i915_state.c:910` `i915_set_clip_state`, `:926`
  `i915_set_viewport_states`, and `:1010` `i915_bind_rasterizer_state`: pass
  clip, viewport, cull, flatshade/provoking-vertex, and related raster state
  into draw even when Chrome9 hardware state remains fixed;
* `i915_state.c:1034` `i915_set_vertex_buffers` and `:1062`
  `i915_bind_vertex_elements_state`: retain vertex resources/elements and
  mirror their descriptions into draw;
* `i915_context.c:51` `i915_draw_vbo`: map vertex, index, and VS constant
  resources, call `draw_set_mapped_vertex_buffer`, `draw_set_indexes`, and
  `draw_set_mapped_constant_buffer`, invoke `draw_vbo`, then remove all mapped
  pointers and call `draw_flush`.

This is not relying on unspecified helper behavior for culling:
`draw_pipe_validate.c` inserts the cull stage and `draw_pipe_cull.c:75`
classifies post-viewport winding.  Flat shading has a narrower boundary.
`draw_pipe_flatshade.c:200` identifies constant/color interpolants and `:263`
selects the API provoking vertex, but `draw_pipe_validate.c` inserts that
stage only when another raster stage requests precomputed flat values.  An
ordinary filled triangle can bypass it.  Keeping H6 in Gouraud mode therefore
also requires the explicit D1/D2 provoking-color lowering in
[flatshade-edge-ownership-contract.md](flatshade-edge-ownership-contract.md).

The Chrome9 `vbuf_render::get_vertex_info` should always describe the proven
six-dword record, rather than vary with arbitrary fragment inputs:

```text
draw POSITION[0] -> EMIT_4F       -> D0, dwords 0..3
draw COLOR[0]    -> EMIT_4UB_BGRA -> D1, dword 4
draw COLOR[1]    -> EMIT_4UB_BGRA -> D2, dword 5
```

`draw_find_shader_output` supplies each source slot. A missing secondary color
is deliberately emitted as zero by using `DRAW_ATTR_NONEXIST`; do not shorten
the record, because the live declaration says six dwords and the known VS
reads D2. Call `draw_compute_vertex_size` and require the result to be six.
The BGRA emitter yields the little-endian `AARRGGBB` dword; for example opaque
red is `0xffff0000`.  The captured declaration format `d0` preserves the byte
ordering but does **not** normalize it.  A real post-TNL API color needs format
`d1` (Normalize bit 0 set), otherwise every nonzero byte at or above one is
liable to clamp to 1.0 downstream.

The first `vbuf_render` implementation may retain CPU memory between
`allocate_vertices` and `unmap_vertices`, then copy the checked range into the
structured immediate-vertex ioctl during `draw_arrays`/`draw_elements`.
For the current bridge ioctl, expand the returned element list into triples,
round/clamp post-viewport x/y to signed 16.16, require the kernel's
`[-0.5, dimension-0.5]` bounds, and pass packed COLOR0. z and w are discarded
only in this no-depth bring-up rung; doing so is not valid once depth is
advertised.
That avoids adding the statically recovered but not yet live-tested SF vertex
stream packet to the critical path. Once stream fetch is validated, the same
interface can allocate a GEM upload BO and use the 1-KiB-aligned subtype-30
stream state described in `raster-front-end-oracles.md` without changing the
state tracker-facing code.

## First correct draw sequence

For every dirty batch, the driver should produce one kernel-validated job with
this logical order:

1. Reference/map the color BO and optional D16 BO into the engine's SF domain;
   retain them until the job fence retires.
2. Program the known common H6 envelope. Substitute validated color
   base/pitch/format in type-01 A50-A52 and the requested type-01 A53 write
   mask.
3. Program type-01 A10-A13 and A01 depth bits when depth is enabled; otherwise
   keep the live no-depth words.
4. Program the known shader activation sequence, constants, VS, and PS without
   reordering its opaque linkage words.
5. Program type-01 A80-A83 from target size and scissor. Retain the proven
   type-04 A40-A48 scale/offset, A50-A52 clamp tuple, and matching VS
   c256/c257 as one pretransformed-viewport linkage block until a normal GL
   viewport capture and variable-viewport experiments identify every
   dependency. (A49 is the reserved zero between those ranges.)
6. Select triangle list and Gouraud shading, set A24 to the checked vertex
   count, and copy `count * 6` post-TNL immediate dwords.
7. Append required alignment only; do not append the legacy Unichrome fire
   command.
8. Flush CPU cache lines before GPU ownership, queue the engine, attach a
   fence to every referenced BO, wait or export completion, invalidate CPU
   mappings on return, and unmap transient SF PTEs only after retirement.

All addresses, pitches, dimensions, counts, enum values, and multiplication
must be range checked in the kernel.  The ioctl should describe state in
structured fields and handles; it should not expose raw MMIO packet submission
to unprivileged clients.

## Mesa context milestone (2026-09-20)

The first software-T&L bridge is now implemented and build-tested.  A Chrome9
`pipe_context` creates Gallium draw, mirrors vertex buffers/elements, vertex
constants, viewport, clip, rasterizer, framebuffer, and shader state into it,
and installs a `vbuf_render` stage.  The stage requests the live six-dword
POSITION/COLOR0/COLOR1 layout, forces the primitive pipeline so clipping,
culling, flat shading, and decomposition stay in software, and submits each
resulting triangle through structured `DRM_CHROME9_DRAW_TRIANGLE`.  Both NIR
and the fixture's exact TGSI primary-color MOV are narrowly recognized; other
fragment programs and unsupported blend/depth/scissor state reject the draw.

Native guarded B8G8R8X8 targets are submitted directly.  Imported DRI3 images
cannot satisfy the diagnostic ioctl's guard-page contract, so the context now
allocates a guarded shadow, seeds it from the imported image, renders there,
and copies it back on synchronous flush.  This is a correctness bridge for
presentation, not the final zero-copy ABI.  Full Mesa builds with LLVM disabled
complete cleanly and the new sources pass formatting and whitespace checks.

The pipe-level context/draw fixture now passes on silicon: one Gallium
triangle changed 16,641 pixels and produced an interior `00408040` sample
with PRIME alias and guard checks intact.  Remaining blockers are explicit:
depth-enabled draws remain rejected until a structured D16
ABI exists; the color-only ABI has no partial scissor, blend, color mask,
multi-triangle batching, or exported fence; and CPU shadow copies and CPU color
clears must eventually be replaced.  This proves end-to-end color-only
Gallium rendering, not yet a complete GL drawable.

## Prioritized gaps and acceptance gates

### P0: variable triangle rung — passed

`tools/chrome9_triangle.c` was run against the matching new kernel. Its
64x64 case verifies ordinary pitch and its 257x129 case exercises a non-square
target, 32-byte pitch padding, more than one pixel page, and screen coordinates
far outside the original 32x32 oracle. Both pass live with capabilities
`0x1d`, including guards and row padding.

Acceptance: both cases pass readback, padding, guards, PAUSE, and complete
engine-idle checks on silicon.

### P1: renderable shared resources and context creation

Implement import/export, surfaces, transfers, render-target/depth formats, and
the context/draw-module skeleton. This is what lets the DRI loader create a
drawable rather than merely a `PIPE_BUFFER` BO.

Acceptance: `glxinfo` creates a direct context and color/depth drawable, maps
it, and exits without leaks or hangs; a pipe-loader smoke test also maps and
clears the guarded color resource.

### P2: variable post-TNL geometry in the known shader envelope

Implement `draw_vbo`, the Chrome9 `vbuf_render` stage, triangle decomposition,
six-dword packing, and the primary-color FS recognizer. Keep culling and flat
shade in software. Initially reduce the record to the current ioctl's x/y and
COLOR0 and issue one synchronous ioctl per triangle; this is a correctness
bridge, not the final batching design.

Acceptance: independent triangles with asymmetric coordinates and RGB vertex
colors land on the expected pixels; indexed and non-indexed input produce the
same result; guards remain intact.

### P3: coordinate, viewport, scissor, and resize calibration

Exercise non-square targets, non-zero viewport/scissor origins, one-pixel
edges, and asymmetric triangles. Determine Y orientation, half-pixel bias,
inclusive/exclusive edges, and how type-04 A40-A48, c256/c257, and the A50-A52
clamp tuple vary together. Until then, the draw module can bake the viewport
while the hardware block remains fixed.

Acceptance: readback matches a software reference at all rectangle boundaries
and after drawable resize.

### P4: extend the structured ABI and live-qualify D16

Extend `DRAW_TRIANGLE`, or add its versioned successor, with a separately
handled D16 BO, per-vertex z, depth compare/write, color mask, and a bounded
multi-triangle payload. Preserve the current structured validation, per-job
fence, BO reservations, cached-shmem PTE handling, cache maintenance, and
timeout/quarantine behavior. Then run the guarded two-draw D16 discriminator
from `depth-blend-oracles.md`, vary compare functions and write enable one bit
at a time, and establish a safe full-surface clear. This is the first
correctness-critical unproved 3D state.

Acceptance: crossing triangles render the same visible surface independent of
submission order for LESS, depth-write disable behaves distinctly, and each
frame starts from a verified cleared depth value.

### P5: flush, fence, and presentation

Wire Gallium flush/fence callbacks and loader presentation. A synchronous
ioctl/signaled fence is acceptable for bring-up; asynchronous syncobj/fence
export is the performance follow-up. Exercise map-after-render and
render-after-CPU-write cache transitions.

Acceptance: continuous resizeable gears survives repeated swap, `glFinish`,
and CPU readback without stale frames, hangs, or use-after-unmap.

### P6: remove temporary CPU clears and per-triangle submits

Promote the live solid-color H6 path to color clear and prove either a safe 3D
depth-clear draw or a format-correct engine clear. Partial clears must honor
scissor and write masks. Submit bounded triangle lists rather than one
synchronous job per primitive.

Acceptance: trace/counters show both color and depth clears executed by the
GPU; no software raster clear remains in a normal gears frame, and one frame
does not require an ioctl per triangle.

### P7: optional hardware state and broader API

Only after gears correctness, test hardware culling/winding, native strip/fan,
flat-shade A23 modes, source-over blend, stencil, additional color/depth
formats, textures, and general shader lowering. `depth-blend-oracles.md` gives
the next blend experiment; `xorg-one-texture-oracle.md` provides a separate
texture reference.

## Unknowns to mine or test next

The shortest path to removing uncertainty is:

1. Capture a normal GL viewport and experimentally bracket type-04 A40-A48
   plus c256/c257 using two non-symmetric sizes and a non-zero viewport
   origin; preserve the distinct A50-A52 clamp tuple during this test.
2. Confirm A51/A52 color domain, pitch, and format fields across two SF/shmem
   strides; never infer them from the old Unichrome layout.
3. Execute the D16 oracle with guard regions, then isolate A11 domain/pitch,
   A12 format/layout, A13 compare, and A01 enables one change at a time.
4. Determine whether the shader envelope accepts an arbitrary vertex count and
   multiple sequential jobs without its diagnostic cookie/pause tail.
5. Prove job retirement rules under timeout/reset so SF mappings and BO
   reservations cannot outlive or underlive the engine.
6. Inspect the fixed-function GL state tracker's exact NIR/TGSI fragment form
   for untextured primary color and make the narrow recognizer test-based.
7. Use an asymmetric winding test to resolve the combined GL Y convention,
   software viewport transform, and A23.8 orientation before enabling A01.7.

The implementation should advertise acceleration only after a context, a
renderable external resource, and the validated general draw path all exist.
The live fixed-quad diagnostic is strong hardware evidence, but it is not a
substitute for those interfaces.
